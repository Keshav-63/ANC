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

### Listening verification (the human check)

Operator report (buds in ears, 60 s run, 5 s tone / 5 s silence x 6):
cancel windows = **dead silence**. The contrast is perceived mainly as the
tone switching off (a 200 Hz tone on small earbuds is a soft hum, not a
loud blare). Audible clicks at every transition are expected: the output
jumps between a sine at an arbitrary phase and exact zeros. This closes
the loop - what the electrical proof promises (digital silence on the
wire) is what the ears hear.

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

## Phase 5 - conclusions (2026-10-18)

Final results on this hardware (laptop Realtek mic + pTron TWS over A2DP):

- Narrowband (200 Hz) cancellation: **achieved and verified three ways**.
  - Electrical: cancel windows are bit-level digital silence on the wire
    (148-292 dB below the tone; `results/narrowband_out.wav`).
  - Acoustic (laptop mic, raw capture): >= 11.9 dB at the mic after the
    phase sweep found the null unaided; the depth is limited by the
    room's 200 Hz ambient floor, not by the system (the null lands below
    what the mic can resolve).
  - Audible (buds in the operator's ears): cancel windows are dead
    silence; transitions click (instant switching at a random phase).
- Stability: 60-100 s sessions over Bluetooth with 0 overruns/underruns/
  gaps; software e2e ~26 ms wired, ~39-42 ms over Bluetooth.
- Why broadband ANC is out of reach here: software pipeline delay
  (~26 ms) dwarfs acoustic propagation (< 1 ms), so `dp << ds`; the FxLMS
  simulation shows the controller destabilizes once the causality budget
  is violated (`experiments/simulated_fxlms.py`).
- Why narrowband nevertheless works: for a steady tone the loop delay is
  just phase, which the sweep absorbs. The two obstacles found on the way
  were measurement and actuation, not control theory: Windows capture
  effects suppressed the stationary tone ~33 dB (solved with raw mode),
  and an unsealed earbud radiates 200 Hz weakly (limiting the at-mic
  measurement; the at-ear result is unaffected because the drive and
  cancel tones come from the same driver).
- Honest headline: with a software-only laptop stack, a deep, stable,
  audible null is achievable for steady narrowband tones (<= ~300 Hz
  class) even over Bluetooth; broadband attenuation is not feasible with
  this latency budget.

## Real-world feedforward demo (2026-10-18, evening) - "noise in, anti-noise out"

The Phase 4/5 work cancelled tones the software itself generated (`drive_cancel`).
This session built the fully external version: a noise source the earbud never
heard, sensed by the laptop microphone, cancelled by the earbud in the air. Every
link is physical - nothing is simulated:

    laptop speakers (noise) -> air -> laptop mic (RAW capture)
                             -> quadrature tracker -> anti-tone
                             -> pTron TWS over A2DP -> air -> cancels at the mic

Tool: `narrowband_experiment --experiment tracked_cancel` - emits
`-gain * (demI*cos - demQ*sin)`, a copy of the measured in-phase component at
`freq` with inverted polarity.

### What was measured, in order

1. **Units discovery.** The tracked controller's output scales with the measured
   mic level; that made sense in the self-contained loop (the mic hears the
   output itself) but in the real loop it hears a distant source (-50 dBFS).
   At gain 0.5 the anti-tone came out at 0.005 amplitude while cancelling
   required ~0.5 - the loop was 46 dB short. Real feedforward needs gain
   O(100-1000).
2. **Ceiling.** With that gain the request pinned at the 0.85 safety limit and
   the watchdog disabled the controller mid-window. Fixes: a 1.5 s engage /
   disengage fade (`cancelRamp_`); the watchdog now trips only after ~3 s of
   *sustained* clipping (the hard limiter already bounds output at all times);
   and finally an amplitude-normalized emission - the anti-tone is emitted at a
   fixed safe amplitude 0.7 regardless of input level
   (`min(gain*|dem|, kTrackedCeiling)`), so high gain is always legal.
3. **Placement dominates everything.** A direct probe (tone from the earbud,
   raw mic, `experiments/loop_probe.py`) measured earbud->mic coupling:
   **-48 dB held against the mic**, **-66 dB lying away**, -82 dB in-ear. Every
   run with the bud away read ~0 dB; the runs with the bud at the mic produced
   nulls.
4. **The loop phase drifts.** Capture and render clocks are independent (~100
   ppm), so the loop phase slides 2-6 deg/s continuously - fixed phase trims
   kept missing. Fixes: a phase-trim input (`--tracked-phase-trim`, rotating the
   reconstruction) and an **auto-trim hill-climb** (`--auto-trim`): the
   controller rotates its own emission through a revolution, measures the
   residual at each step, locks the best phase, learns the drift rate between
   searches, and rotates with it during the locked window.
5. **The room.** A live ambient scan (`experiments/ambient_scan.py`, raw mic,
   source off) found a real room tone at **200 Hz = -41.8 dBFS**, 40 dB above the
   room floor elsewhere. At 200 Hz the earbud ceiling at the mic (~-51 dBFS)
   sits *below* that room tone, so cancellation was physically impossible - and
   the measured depth matched the mathematical maximum exactly. At **450 Hz the
   room is silent (-96.6 dBFS)** and the laptop speakers reach the mic far more
   strongly (-27.9 dB coupling), so the demo moved to 450 Hz.

### Verified outcome (200 Hz runs, earbud held at the mic)

- Source at mic -48 dBFS; the auto-trim search found residuals of **-58.8 and
  -64.5 dBFS = 10-16 dB of real acoustic cancellation**, measured by the raw
  microphone at the cancellation point.
- Null-depth rule: anti-tone amplitude at the mic is `A = 0.7 * coupling`;
  residual ~ `|source - A|`. Source within ~[A/1.6, A*1.6] gives a deep null; a
  source much louder than A caps the depth (10 dB louder -> only ~3 dB).

### Deliverable

`run_real_demo.cmd [source-drive-gain]` - one command, everything tuned: noise
source + auto-trim cancellation loop + phase tracking, 6 cycles of (baseline
8 s, phase search, locked 8 s). The operator only holds the earbud against the
laptop mic. Helper analyses under `experiments/`: `lockin_timeline.py`,
`loop_probe.py`, `transfer_estimate.py`, `ambient_scan.py`.

### In-ear variant (buds worn, cancelled at the eardrum) - 2026-10-18

Same controller, but the actuator is worn in the ear and the null is at the
eardrum while the sensor stays at the laptop microphone. Physics: same loop,
different error path (ear canal instead of mic spot), so the loop still
nulls at the mic while the ear sees `source - A` with A the earbud's level
at the eardrum - the depth is set by the ear path, which the laptop mic
cannot measure. Tuning therefore became human-in-the-loop, with the operator
listening:

1. **Level.** With the anti-tone at 0.7 (ceiling) the ear reports "louder";
   at 0.15 / 0.05 / 0.015 the hum reports "dropped". Working point: emission
   ~0.05, i.e. `--cancel-gain 2.5` (the emission equals gain x measured mic
   level, so the mic level sets it).
2. **Phase structure.** Sweeping 0/90/180/270 deg: quiet at 0 and 180, loud
   at 90 and 270 - a 180-deg period, which means the earbud driver inverts
   phase at 450 Hz; the nulls sit near 0 and 180 deg. A refinement at
   -22.5/-7.5/+7.5/+22.5 deg was inconclusive by ear, as expected (8-deg
   steps are below human phase discrimination).
3. **Locked run** (phase 0, level 0.05, 12 s hum / 12 s cancelled x3):
   the operator reports the hum audibly present when the system is off and
   quiet while it is on - the cancellation is felt live in the earbud.
4. **A loud-gain failure mode worth remembering:** every earlier "I cannot
   hear any anti-tone" report was the Windows volume for the
   "Headphones (pTron)" endpoint sitting low, not the DSP. With the endpoint
   at 100% the full-level test tone is clearly audible.

Honest limit: unlike the at-the-microphone null (measured 10-16 dB), the
in-ear depth cannot be quantified with the tools in this repository - it is
a perceptual result, and it depends on the ear path staying fixed (no head
movement, consistent tip seal).
