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

Record here after the run:

- best cancel phase from sweep: __ deg (processor) / __ deg (mic-verified)
- baseline vs cancel residual (processor readout): __ dBFS -> __ dBFS
- attenuation (mic-verified): __ dB; mic vs processor cross-check: __ dB
- safety watchdog trips: yes/no
- notes: __

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
