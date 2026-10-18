# Software-Only Active Noise Cancellation (feasibility study)

A step-by-step, hardware-free exploration of what active noise cancellation
can and cannot do with nothing but a normal Windows laptop: the built-in
microphone as the error sensor and the audio output as the anti-noise source.
No extra microphones, no dedicated DSP, no custom circuits.

The authoritative plan lives in `software_anc_project_plan.md`; this README
is the working map. Measured results and the experiment log are in
`results/experiment_notes.md`.

## What runs where (phase map)

| Phase | Tool | Status |
|-------|------|--------|
| 0 - device probe (formats, periods, clocks) | `audio_probe` | done on this machine |
| 1 - duplex pass-through (latency, dropouts) | `audio_passthrough` | done on this machine |
| 2 - latency measurement + acoustic cross-check | `audio_passthrough --mode loopback` + `experiments/analyze_latency.py` | analyzer self-tested; acoustic run pending earphones |
| 3 - DSP sandbox + unit tests | `dsp_sandbox`, `anc_tests`, `experiments/selftest_analyzers.py`, `simulated_fxlms.py` | done |
| 4 - narrowband cancellation experiment | `narrowband_experiment` + `experiments/analyze_spectrum.py` | done: null found, electrical + mic + listening verified over Bluetooth (2026-10-18) |
| 5 - interpretation / conclusions | `results/experiment_notes.md` | written 2026-10-18 |

## Build

Requirements: Visual Studio 2022 Build Tools (MSVC x64), CMake >= 3.24,
Windows 10/11 SDK. Python 3.9+ with `numpy` for the analysis scripts
(`matplotlib` optional for plots, `scipy` not required).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

All tools are run **from the project root** so the default output paths under
`results/` resolve correctly.

## Phase 0 - what does the hardware support?

```powershell
build\Release\audio_probe.exe            # enumeration + 1 s muted smoke test
build\Release\audio_probe.exe --attempt-raw   # probe raw/exclusive formats too
```

Writes `results/device_probe.txt`. Expected on this machine: one capture and
one render endpoint (Realtek), 48 kHz 2ch float32, engine period 480 frames
(10 ms), shared-mode buffers 1056 frames.

## Phase 1 - duplex pass-through

```powershell
build\Release\audio_passthrough.exe --duration 30 --metrics-csv results/latency_report.csv
python experiments\analyze_latency.py --passthrough-csv results/latency_report.csv
```

Plays the microphone live through the output. Use `--mute` for a silent
plumbing check (analogous to `--gain 1.0 --invert` for experiments; `--gain`
applies the pass-through gain). Watch for dropouts (overruns/underruns must
stay 0 post-warmup) and the software input->output estimate (`e2e_ms`, ~26 ms
on this machine with 10 ms engine periods).

## Phase 2 - latency and the acoustic cross-check

This machine's software pipeline estimate is known from the metrics CSV, but
the *true* input-to-output latency includes codec and air-path delays. The
loopback mode emits deterministic log chirps (200 -> 6000 Hz, Hann-windowed,
-20 dBFS) and records both the rendered output and the microphone signal,
plus device-clock anchors that put every sample on one QPC time base.

Setup: plug in earphones (so the chirps are not fired straight into the mic
at extreme level) and hold the earphone a few centimetres from the built-in
microphone, keeping both steady during the run.

```powershell
build\Release\audio_passthrough.exe --mode loopback ^
  --record-input results/loopback_mic.wav ^
  --record-output results/loopback_out.wav ^
  --timing-csv results/timing_anchors.csv ^
  --info-json results/run_info.json
python experiments\analyze_latency.py --loopback
```

The analyzer matched-filters the chirp template against the microphone
recording, maps sample indices to QPC seconds via the anchors, and reports
the per-chirp delay (`results/loopback_latency_report.csv` + plot). A clean
result is a tight cluster: median delay with a small spread (tens of
microseconds of platform jitter). If detection is weak raise `--chirp-amp`
(default 0.1); if it clips, lower it. The search window accommodates
high-latency outputs - Bluetooth codecs add 100-400 ms, so delays well above
100 ms mean codec buffering, not the software pipeline (which is ~26 ms here).

## Phase 3 - DSP sandbox and self-tests

```powershell
build\Release\dsp_sandbox.exe               # inverse/error-residual + FIR checks
build\Release\anc_tests.exe                 # 13 tests / 71 checks
python experiments\generate_test_signals.py # test WAVs in results/signals/
python experiments\selftest_analyzers.py    # validates both analyzers offline
python experiments\simulated_fxlms.py       # causality-budget simulation
```

`dsp_sandbox` prints ALL CHECKS PASSED and writes residual CSVs under
`results/`. `selftest_analyzers.py` proves the two acoustic analyzers recover
known synthetic values (7.5 ms delay, 20 dB attenuation) before any hardware
run. `simulated_fxlms.py` demonstrates the core lesson: broadband cancellation
requires primary-path delay >= secondary-path delay.

## Phase 4 - narrowband cancellation (the experiment that can work)

A steady tone is the one case where a controller without a causal delay
advantage can still win: the tone's amplitude/phase is predictable, so a
frozen or slowly adapting quadrature canceller reaches a deep steady-state
null. This also makes it the experiment that works over Bluetooth output:
the extra codec delay is just extra phase, which the sweep absorbs, while
any wideband scheme is hopeless with a 100-400 ms output path.
The `narrowband_experiment` tool drives a 200 Hz tone, sweeps the cancel
phase to find the null, then alternates baseline/cancel windows.

```powershell
build\Release\narrowband_experiment.exe --experiment drive_cancel --freq 200 ^
    --capture-raw
python experiments\analyze_spectrum.py --windows results/narrowband_windows.csv ^
    --mic results/narrowband_mic.wav
python experiments\analyze_spectrum.py --mic results/narrowband_out.wav ^
    --summary results/spectrum_summary_out.csv
```

`--capture-raw` is required on Windows laptops: the default capture path
("microphone enhancements": noise suppression/high-pass) removes ~33 dB
from a stationary tone. The second analyzer call verifies the *electrical*
cancellation from the recorded render stream (`narrowband_out.wav`).

Verified on 2026-10-18 (pTron TWS over Bluetooth, laptop mic, raw capture):
the sweep finds phase 0 deg unaided; cancel windows are bit-level digital
silence on the wire (148-292 dB below the tone); the at-mic null is
floor-limited (>= 11.9 dB measured); and the listening check confirms the
cancel windows are dead silent in the earbuds.

- `--experiment tracked_cancel` replaces the phase sweep with an adaptive
  quadrature tracker (`--cancel-gain`, default 0.5).
- The tool records the mic by default (`results/narrowband_mic.wav`); the
  analyzer lock-in-detects the drive frequency per window and reports the
  attenuation, cross-checking the processor's own readout.
- Safety: the processor hard-limits its output and a watchdog trips the
  controller off after repeated clipping; the hardware stays protected. Keep
  positions fixed across windows and hold the earphone near the mic.

See `results/narrowband_windows.csv` for per-window data and
`results/plots/narrowband_*.png` for sweep and spectra plots.

## Phase 5 - why this is the end of the road for wideband

This platform's software pipeline adds ~26 ms of latency to any processing
loop, while the speaker-to-microphone acoustic path is under 1 ms. With
`dp << ds`, broadband feedforward cancellation is impossible (the controller
would need the future of the noise); the simulation in
`experiments/simulated_fxlms.py` shows both sides of that boundary. What
remains possible is narrowband (steady-tone) cancellation through precise
phase alignment - what Phase 4 measures. The full write-up lives in
`results/experiment_notes.md`.

## Real-world demo - noise in, anti-noise out (every link physical)

Beyond the self-contained Phase 4 experiment, `tracked_cancel` closes a real
feedforward loop: the laptop's speakers hum a steady tone, the laptop
microphone hears it (raw capture, effects bypassed), the software measures
its amplitude/phase and emits the anti-tone to the earbud, and the two
cancel in the air at the microphone. The controller finds the phase itself
(`--auto-trim`: it sweeps its own emission, locks the null, and tracks the
Bluetooth clock drift) and emits a fixed-amplitude anti-tone, so it never
rides the safety limiter.

Run it - hold the earbud's nozzle against the laptop mic (the null exists
where the noise is sensed):

```
run_real_demo.cmd            # quieter hum: run_real_demo.cmd 0.05
```

Per cycle: 6 s hum / ~8 s phase search / 6 s locked cancellation; three
cycles, under 90 seconds total. Measured on this machine with the earbud
held at the mic: source -48 dBFS at the mic, residuals of **-58.8 and
-64.5 dBFS** during the search - 10-16 dB of acoustic cancellation,
verified by the raw microphone. The physics limits are known: the anti-tone
amplitude at the mic is `0.7 x coupling` (-51 dBFS with the earbud on the
mic, -104 dBFS with it lying away), so the source must arrive within about
[A/1.6, A*1.6] for a deep null, and the earbud must be at the sensor.
450 Hz is used because this room is silent there (its 200 Hz tone alone
measures -41.8 dBFS). Details: `results/experiment_notes.md`.

## Repository layout

```
src/core/      ring buffer, processor interface, metrics, config
src/audio/     WASAPI utilities, device probe, duplex engine, WAV writer
src/dsp/       pass-through, FIR, tone/chirp generators, narrowband canceller
src/tools/     audio_probe, audio_passthrough, dsp_sandbox, narrowband_experiment
tests/         unit tests (ring buffer, phase math, FIR)
experiments/   Python: signal generation, latency/spectrum analysis, FxLMS sim
results/       generated artifacts (WAVs, CSVs, plots) + experiment notes
```

## Safety notes

- Run a `--mute` pass first whenever you change device/period settings.
- Use earphones for any acoustic measurement; never point a live microphone
  back into speakers at high gain (feedback).
- The narrowband tool never exceeds a hard output limit and disables itself
  on repeated clipping - if that trips, reduce `--cancel-gain` and repeat.
