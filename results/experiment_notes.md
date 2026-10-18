# Experiment notes - software-only ANC

Living log for this machine and this build. Fill in the acoustic sections
(Phase 2 / Phase 4) after running them; everything else is already recorded.

## Environment / hardware

- Date of first runs: 2026-10-18
- OS: Windows (laptop, Realtek audio)
- Toolchain: MSVC 2022 Build Tools, Windows SDK 10.0.26100, CMake 3.31.5,
  Python 3.11 (numpy/scipy/matplotlib)
- Capture endpoint: Microphone Array (Realtek(R) Audio)
- Render endpoint: Speakers (Realtek(R) Audio) - earphones route through this
  endpoint when plugged into the laptop jack
- Mix format both directions: 48000 Hz, 2 ch, 32-bit float (extensible)
- Engine period: capture 480 / render 480 frames (10.0 ms; min = default =
  max, no low-latency IAudioClient3 period on this machine)
- Shared-mode buffers: 1056 frames both directions

## Phase 0 - device probe

Run: `build\Release\audio_probe.exe` -> `results/device_probe.txt`

- Concurrency smoke test (1 s, muted): capture callbacks 101, render
  callbacks 103, frames 48480 / 49440, overruns 0, underruns 0,
  position gaps 0 frames. Verdict: PASS.

Quirks found while bringing this up (fixed in code):

- `AUDCLNT_STREAMFLAGS_NOPERSIST` is rejected by
  `IAudioClient3::InitializeSharedAudioStream` on this stack
  (`AUDCLNT_E_INVALID_STREAM_FLAG`) - flag removed.
- Capture `GetBuffer` parameter order matters: the device position is the
  4th argument, QPC position the 5th. An early version passed them swapped,
  which inflated the "position gaps" counter with 100 ns timestamps
  (~9.85M bogus "frames"); fixed and verified (0 gaps).
- `PKEY_AudioEndpoint_FormFactor` is not resolvable via uuid.lib here;
  the probe defines the key locally and reports "(unknown)" for it.
- `GetStreamLatency` reads 0.00 ms on this stack; rely on anchor-based
  estimates instead.

## Phase 1 - duplex pass-through

Run: `build\Release\audio_passthrough.exe --mute --duration 8 --metrics-csv results/passthrough_smoke.csv`

Measured (8.1 s, muted):

| metric | value |
|---|---|
| capture / render callbacks | 805 / 807 |
| overruns / underruns (post-warmup) | 0 / 0 frames |
| warmup underruns | 960 frames (2 periods, expected while priming) |
| position gaps | 0 frames |
| discontinuities | 1 (startup) |
| clipped frames | 0 |
| capture proc time | avg 14.2 us, p99.9 142 us, max 205 us |
| render proc time | avg 26.4 us, p99.9 111 us, max 3963 us (first call, page faults) |
| software e2e estimate | median 26.3 ms (min 25.7, max 26.7) |
| verdict | STABLE (no dropouts) |

Note: the audible (unmuted) pass-through still needs a listening check by a
human (clicks/dropouts/feedback). Feedback is the main hazard - use `--mute`
first, then earphones.

## Bluetooth live-demo session (2026-10-18, pTron TWS)

Configs tried: capture + render both Bluetooth, and laptop mic + Bluetooth
render. Findings:

- **TWS "Headset" mic cannot serve as the error sensor for 200 Hz**: the
  Bluetooth mic measures the bud output 20-40 dB down and high-pass
  filtered (residuals -63 to -89 dBFS, no phase pattern) - the sweep found
  a noise minimum, not a null. Use the laptop mic; keep the earbuds as the
  output being listened to (both buds play the same signal, so the null
  heard and the null measured are the same one).
- **A2DP endpoint rejected `IAudioClient3::InitializeSharedAudioStream`**
  with `AUDCLNT_E_INVALID_STREAM_FLAG` (transient, after an HFP session).
  Fixed with an automatic classic-shared-mode fallback in
  `wasapi_streams.cpp` (and the same fallback for capture).
- **The Realtek laptop mic was delivering digital zeros** during the wired-
  mic run (residual readout pinned at -300 dBFS; a 3 s mic recording was
  bit-exact silent: rms 0.000e+00). OS/hardware mute - not a software bug.
  Diagnostics: Sound settings level bar, mic-mute F-key, privacy toggles.

### RAW capture is mandatory (Windows effects suppress the tone)

After the mic was unmuted, the same 0.2-amplitude drive tone measured
**-91.4 dBFS** through the default ("enhanced") capture path but **-58.9
dBFS** with `AUDCLNT_STREAMOPTIONS_RAW` (effects bypassed) - the OS/Realtek
APO chain (noise suppression / high-pass) removes ~33 dB from a stationary
200 Hz tone. `narrowband_experiment.exe` now takes `--capture-raw`, and it
prints the engine `plan.notes` so raw/fallback status is visible. Room
floor at 200 Hz: raw -71.5 dBFS vs enhanced -95.2 dBFS (4 s muted
recordings, `results/mic_floor_raw.wav` / `mic_floor_normal.wav`).

### Electrical ground truth from the recorded render stream

`--record-output results/narrowband_out.wav` captures what actually goes to
the Bluetooth link; lock-in analysis of that file is the cleanest
verification available (`analyze_spectrum.py --mic results/narrowband_out.wav`):

- phase-sweep amplitudes match theory `2G|sin(phi/2)|` to 0.01 dB
  (-7.96 dBFS at 180 deg down to digital silence at 0 deg);
- every cancel window at the best phase transmitted **digital silence**
  (-156 to -300 dBFS) = 148-292 dB electrical cancellation.

The tool picked phase 0 deg by itself (from the mic) - the true null.

### Readout race in the harness (fixed)

The first per-window sample could read the *previous* window's last audio
block, so cancel windows printed a stale `peak 0.200` and sweep peaks
showed a one-window "lag". A 50 ms settle before the sampling loop fixed
it: cancel windows now print `peak 0.000`, verdicts read STABLE, and the
summary prints `render missed deadline` (it explains otherwise-mysterious
UNSTABLE verdicts caused by benign event wakes).

### Microphone-side acoustic numbers (floor-limited)

Best run (RAW capture, bud near the mic): mic lock-in at 180 deg =
-61.0 dBFS vs 0 deg = -72.9 dBFS -> **11.9 dB reduction at the laptop
mic**, floored by the ambient 200 Hz noise (-71.5 dBFS). Repeated runs
measured only the floor (no tone/silence pattern) whenever the bud was not
at the mic. An unsealed earbud radiates 200 Hz weakly (the driver needs
ear-seal volume loading), so the at-mic number is a lower bound: the null
is deeper than the room floor can resolve. The at-ear cancellation equals
the electrical one - the buds receive digital zeros during cancel windows.

Bluetooth run metrics (all STABLE, e2e ~39 ms vs 26 ms wired): 0 overruns,
0 underruns, 0 gaps across ~100 s runs; render proc avg ~70-90 us, max
< 670 us; the A2DP stream stayed solid throughout.

## Phase 2 - acoustic loopback latency (pending: needs earphones)

Setup: earphones plugged, earphone held a few cm from the built-in mic,
both steady for the whole run. (First acoustic run 2026-10-18 uses the
pTron TWS Bluetooth output: expect 100-400 ms codec-dominated delays and
slightly larger jitter than a wired path.)

```powershell
build\Release\audio_passthrough.exe --mode loopback ^
  --record-input results/loopback_mic.wav --record-output results/loopback_out.wav ^
  --timing-csv results/timing_anchors.csv --info-json results/run_info.json
python experiments\analyze_latency.py --loopback
```

Record here after the run:

- chirps detected: __ / 20
- median delay: __ ms (min __ / max __, spread __ ms)
- software pipeline estimate from the same run: __ ms (if metrics CSV kept)
- notes (room, distance, anything unusual): __

Sanity expectation: loopback delay >= software e2e estimate (adds codec
buffers + air path). The spread should be tiny (the anchors share one QPC
time base). Analyzer was validated offline at exactly 7.50 ms on synthetic
data (`python experiments\selftest_analyzers.py`).

## Phase 3 - DSP sandbox, unit tests, simulation

- Unit tests: 13 tests, 71 checks, 0 failed (`build\Release\anc_tests.exe`).
- `dsp_sandbox`: ALL CHECKS PASSED; inverse/error residuals match theory
  (`2A sin(phi/2)` scaling); outputs `results/dsp_*.csv`.
- Analyzer self-test: PASS (loopback 7.50 ms recovered; spectrum 20.00 dB
  attenuation recovered) - the analysis chain is proven before any hardware.
- FxLMS causality simulation (`results/fxlms_sim.csv`):

| margin dp - ds | overall | 0-600 Hz | 600-8000 Hz |
|---|---|---|---|
| +2 ms | 31.3 dB reduction | 75.4 dB | 38.0 dB |
| 0 ms (exact model) | 36.8 dB | 42.7 dB | 42.3 dB |
| -2 ms (infeasible) | -5.9 dB (error grew) | -10.7 dB | -2.7 dB |

Interpretation: feasibility threshold is dp >= ds; beyond it the controller
can make things worse - a real system must limit control gain, exactly as the
narrowband processor does via its output limiter.

## Phase 4 - narrowband cancellation (pending: needs earphones)

```powershell
build\Release\narrowband_experiment.exe --experiment drive_cancel --freq 200
python experiments\analyze_spectrum.py --windows results/narrowband_windows.csv ^
    --mic results/narrowband_mic.wav
```

Recorded (2026-10-18, pTron TWS output + laptop mic, `--capture-raw`):

- best cancel phase from sweep: 0 deg (processor) / 0 deg (mic-verified)
- baseline vs cancel residual (processor readout): -57.3 dBFS -> -57.7
  dBFS (floor-limited; the at-mic tone is weak relative to ambient 200 Hz)
- attenuation (mic-verified): 11.9 dB in the best sweep run (the null sits
  at the room floor, so the true depth is >= this); electrical attenuation
  measured on the render stream: 148-292 dB (cancel windows are digital
  silence)
- mic vs processor cross-check: both floor-limited and flat in the
  no-sweep repeats; use the output-stream analysis for the electrical
  number and the sweep windows for the acoustic one
- safety watchdog trips: no
- notes: always run with `--capture-raw` (see Bluetooth section); analyze
  both `results/narrowband_mic.wav` and `results/narrowband_out.wav`.

Expectation for this platform: a deep, stable null is plausible at 200 Hz
because the tone is steady and the loop delay only sets a required phase
offset. Wideband noise in the same loop is hopeless (see Phase 3 table).

## Phase 5 - conclusions (to finalize after Phases 2/4)

- Software pipeline latency (~26 ms) dwarfs the acoustic path (< 1 ms) on
  this machine: `dp << ds`, so broadband feedforward ANC is physically out
  of reach with this software-only stack, regardless of algorithm.
- Narrowband cancellation remains possible; the experiment's job is to
  quantify how deep and how stable that null is in a real room with the
  earphone/microphone geometry available.
- The measured numbers (Phase 2 delay, Phase 4 attenuation) determine the
  honest headline for this project.
