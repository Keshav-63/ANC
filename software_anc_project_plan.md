# Project Plan: Software-Only Active Noise Cancellation

**Research-backed engineering plan · Version 1.0 · October 18, 2026**

## Project goal

Develop and test software that attempts to turn ordinary wired earphones into active noise-cancelling earphones, starting on a laptop with its built-in microphone and 3.5 mm headphone jack. No additional hardware is required for the initial experiment.

Treat this as an **experimental audio-processing system**, not as a guaranteed way to reproduce commercial ANC.

## 1. Executive assessment

There are two different engineering challenges: solve the audio-processing problem first, then determine whether the processed sound can actually cancel environmental noise at the ears.

### Goal A — Low-latency audio processing

**Achievable.** Build a native application that captures microphone audio, processes it and outputs audio through wired earphones with minimal delay.

### Goal B — Measurable real-world noise cancellation

**Uncertain.** Determine whether the laptop microphone can provide a sufficiently early and useful reference signal to cancel a narrow range of environmental noise at the ears.

### Goal C — ANC comparable to commercial products

**Not a realistic initial target.** Commercial ANC relies on tuned acoustic paths, microphones in carefully selected positions, and feedback or feedforward control. Software alone cannot eliminate these hardware and geometry constraints.

A foundational reference is Kuo and Morgan's 1999 paper, *Active Noise Control: A Tutorial Review*, which explains the acoustic, adaptive-filtering and implementation constraints involved in real ANC systems. Research on causality in feedforward ANC also shows that cancellation depends not just on how fast an algorithm runs, but on whether the reference microphone provides enough advance warning of the disturbance.

Sources:
- Kuo & Morgan, *Active Noise Control: A Tutorial Review*: https://doi.org/10.1109/5.763310
- Feedforward ANC causality research: https://www.sciencedirect.com/science/article/pii/S0003682X1400005X

## 2. Recommended technology stack

The recommendation is **C++20 + native Windows WASAPI**, rather than Python, JavaScript or a browser-based audio pipeline.

The main reason is direct control over audio devices, buffer sizes, stream timing and real-time execution. Microsoft's Windows audio documentation explains that WASAPI supports low-latency capture and playback, and `IAudioClient3` exposes supported audio-engine periods. Some hardware can support periods around 1 ms, but that does **not** guarantee total microphone-to-earphone latency.

| Component | Choice | Reason |
|---|---|---|
| Language | C++20 | Native compiled processing and direct Windows audio API access |
| Audio API | WASAPI with `IAudioClient3` | Control over audio capture, rendering and supported buffer periods |
| Execution model | Event-driven audio threads | Avoid unnecessary polling and scheduling delay |
| DSP implementation | Native C++, initially scalar floating-point operations | Simple, deterministic processing without a large dependency stack |
| Build system | CMake + MSVC | Standard Windows-native development workflow |
| Offline analysis | Python + NumPy/SciPy | Analyze recordings, frequency spectra and experiment results outside the real-time path |
| Optional audio abstraction | PortAudio | Useful fallback for easier prototypes before moving to direct WASAPI if needed |

Windows also supports ASIO for low-latency professional audio, but it normally depends on an appropriate driver. Start with native WASAPI rather than assume that an ASIO driver exists for the laptop. Evaluate low-period shared-mode streams before committing to exclusive mode, because exclusive mode takes control of the audio endpoint and can interfere with other applications.

Useful documentation:
- Microsoft, Low Latency Audio: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- Microsoft, WASAPI: https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi
- PortAudio latency guidance: https://portaudio.com/docs/latency.html

### Why not Python, Rust or Java?

**C++ — chosen for the first implementation.** It provides direct access to Windows Core Audio APIs and makes it straightforward to keep allocation, synchronization and other unpredictable work out of the audio-processing thread.

**Rust — equally viable for native DSP.** Rust can also deliver native performance. CPAL supports Windows audio and optional ASIO integration. It is a reasonable alternative if memory safety and cross-platform portability matter more than using the Windows API directly.

- CPAL: https://github.com/RustAudio/cpal

**Python — for experiments, not the critical audio path.** Use Python for plots, spectral analysis, generating test signals, comparing recordings and analyzing results. Do not put a Python interpreter, a notebook, or a machine-learning model in the time-critical capture-to-playback loop.

**Important distinction:** C++ is not inherently guaranteed to deliver lower latency than Rust. The Windows driver, audio hardware, buffering, synchronization and microphone placement usually matter more than the choice between two compiled languages. PortAudio's latency guidance explains why capture and playback buffering both contribute to the total delay.

- PortAudio latency guidance: https://portaudio.com/docs/latency.html
- PortAudio callback guidance: https://portaudio.com/docs/v19-doxydocs/writing_a_callback.html

## 3. System architecture

The first version will have one job: move captured audio through a native processing pipeline and into wired earphones, while making every stage measurable.

```text
Built-in laptop microphone
          |
          v
WASAPI capture stream
(capture timestamps, sample rate, available frames)
          |
          v
Preallocated audio buffer
(transfer samples without blocking the real-time thread)
          |
          v
C++ DSP engine
(initially pass-through; later filters and anti-noise experiments)
          |
          v
WASAPI render stream
(write samples at the lowest stable supported period)
          |
          v
3.5 mm wired earphones
(audio reaches the earphones through the laptop's analog output)
```

This architecture is appropriate for testing low-latency audio processing. It does **not** yet establish that environmental sound and generated anti-noise cancel at the eardrum.

A critical hardware constraint is that the microphone is built into the laptop rather than positioned inside or beside the ear. There is also no measurement of residual noise inside the ear canal. Practical feedforward ANC headset research explicitly models the reference microphone, cancelling speaker, error microphone and the acoustic paths between them.

Reference:
- Feedforward ANC causality research: https://www.sciencedirect.com/science/article/pii/S0003682X1400005X

## 4. Windows audio configuration

Before writing a cancellation algorithm, establish the most efficient audio path the specific laptop supports.

### Initial configuration

| Setting | Starting value |
|---|---|
| Operating system | Run natively in Windows, not inside WSL |
| Output device | Laptop's 3.5 mm headphone endpoint |
| Input device | Built-in microphone or microphone array |
| Sample rate | 48,000 Hz, if supported by both streams |
| Capture format | Mono floating-point samples where supported |
| Playback format | Native endpoint format; stereo output if required |
| Initial audio period | Smallest common stable period supported by the endpoints |
| DSP processing | Minimal, with no added smoothing or look-ahead |
| Audio path | Event-driven WASAPI; start in low-period shared mode |

These are starting targets, not capabilities already verified for the laptop. `IAudioClient3::GetSharedModeEnginePeriod` can report available periods for a selected format. Query it instead of hardcoding a supposed minimum.

Also disable unnecessary microphone noise suppression, automatic gain control, spatial audio and other audio enhancements during controlled tests, wherever Windows and the driver allow it. These transformations can alter the waveform being measured.

Test in a native Windows executable rather than routing the audio through WSL, a browser or an ordinary desktop application with an unknown additional processing path.

References:
- Microsoft, Low Latency Audio: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- Microsoft, WASAPI: https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi

## 5. The critical physics: latency versus cancellation bandwidth

This is the most important part of the research. Getting the algorithm to run quickly is necessary, but it is not enough.

Suppose the environmental noise at the ear is

\[
d(t)=A\sin(2\pi ft).
\]

An ideal cancellation signal has matching amplitude and opposite phase:

\[
c(t)=-A\sin(2\pi ft).
\]

Adding them gives zero in the ideal mathematical case. In a physical system, however, the noise reference, digital processing, audio output and acoustic paths introduce timing and phase errors.

For a pure timing mismatch \(\Delta t\), the additional phase shift is

\[
\Delta\phi=360^\circ f\Delta t.
\]

The table below shows the timing error corresponding to a phase deviation of just 30 degrees. It is an illustrative engineering budget, not a guaranteed ANC specification.

| Noise frequency | Period of one cycle | Timing mismatch for 30° |
|---:|---:|---:|
| 50 Hz | 20 ms | 1.67 ms |
| 100 Hz | 10 ms | 0.83 ms |
| 200 Hz | 5 ms | 0.42 ms |
| 500 Hz | 2 ms | 0.17 ms |

At 200 Hz, just 1 ms of uncompensated timing error corresponds to 72 degrees of phase error. A cancellation signal with that much phase error may perform poorly even when its amplitude is correct.

This follows from the phase response of a time delay. Research on feedforward ANC headsets identifies the causal relationship between the reference microphone, the error location, the secondary acoustic path and the system's electrical delay as a central design constraint.

Reference:
- Feedforward ANC causality research: https://www.sciencedirect.com/science/article/pii/S0003682X1400005X

### Why the laptop microphone is a special problem

A conventional feedforward ANC system uses a reference microphone to hear an upcoming disturbance and an error microphone to measure what remains after cancellation. Its controller adapts to minimize that residual noise.

In this setup:

- The laptop microphone is the reference sensor, but it may not hear the noise early enough.
- The earphone driver generates the cancellation sound, but its acoustic path to the eardrum is not characterized.
- There is no microphone at the ear to measure the actual residual noise.
- Therefore, the system cannot directly optimize cancellation at the ear in the way a conventional adaptive ANC system does.

A microphone elsewhere in the room cannot substitute perfectly for a microphone at the ear. Noise from different directions reaches these two positions at different times and with different phases.

**Consequence:** Spend the first development effort measuring the real audio path and its delay rather than immediately writing an adaptive noise-cancellation algorithm.

## 6. Development roadmap: from zero to a tested prototype

Proceed through the following gates. Each phase has a concrete deliverable and a reason to continue or stop.

### Phase 0 — Hardware and audio capability audit

**Objective:** Verify that Windows exposes the microphone and the 3.5 mm earphone output in a way that permits low-latency full-duplex processing.

Build a small native C++ utility to enumerate audio endpoints, obtain supported formats and query the minimum and maximum shared-mode engine periods. Record which endpoint is actually used when the earphones are plugged in.

**Deliverable:** `audio_probe` report containing device names, formats, periods and initialization errors.

**Pass condition:** Both capture and playback streams can be initialized concurrently in the intended configuration. If the minimum periods differ, use the lowest common stable configuration rather than arbitrarily forcing both to a specific size.

### Phase 1 — Build a real-time audio pass-through engine

**Objective:** Capture the laptop microphone and play it back through the wired earphones with no intentional DSP delay.

Implement event-driven WASAPI capture and render streams using `IAudioCaptureClient` and `IAudioRenderClient`. Use preallocated sample buffers and a single-producer/single-consumer ring buffer if capture and render processing need separate threads.

The time-critical audio path must not allocate memory, perform disk I/O, write console logs, wait on UI locks or execute unbounded operations. PortAudio's callback documentation gives similar advice: audio callbacks must avoid work that could delay servicing the device.

**Deliverable:** `audio_passthrough`, with device selection, sample-rate reporting, buffer-period selection and counters for glitches, buffer overruns and underruns.

**Pass condition:** Stable operation for at least 10 minutes at the smallest common period the hardware supports without audible dropouts.

This establishes the audio pipeline only. It does not prove cancellation.

### Phase 2 — Measure latency and scheduling stability

**Objective:** Establish the lowest stable operating point on the laptop.

Test several supported periods, for example 48, 96 and 128 frames at 48 kHz, but use them only when the API reports them as valid for the device.

| Frames | Nominal duration at 48 kHz |
|---:|---:|
| 48 | 1 ms |
| 96 | 2 ms |
| 128 | 2.67 ms |
| 240 | 5 ms |

These are durations of audio frames, **not total end-to-end latency**.

Collect capture and render device positions and timestamps; use a high-resolution clock for scheduling measurements; and log callback or event-handling duration, missed deadlines and discontinuities. Use an acoustic test signal at low volume to obtain an approximate physical input-to-output measurement as a cross-check. Software-only timestamps can estimate parts of the pipeline but do not independently prove acoustic latency at the eardrum.

**Deliverable:** `latency_report.csv` and a report with minimum, median, 95th-percentile and 99.9th-percentile measurements where enough samples are available.

**Pass condition:** A repeatable operating point with no dropouts, bounded processing time and measured or carefully qualified end-to-end latency. Select the smallest stable period; do not select the smallest period that merely starts successfully.

### Phase 3 — Verify the anti-phase mathematics

**Objective:** Prove that the signal-processing implementation is correct before involving microphones and acoustic propagation.

Write a test harness that generates synthetic sinusoidal signals at 50, 100, 200 and 500 Hz. Add an equal-amplitude inverted signal and check the residual numerically. Then deliberately introduce time offsets, gain errors and phase errors.

Add a test for a finite impulse response (FIR) filter and measure its frequency response. Use Python/NumPy for offline comparisons with the C++ output.

**Deliverable:** `dsp_sandbox`, containing synthetic test cases, numerical assertions and residual-versus-phase plots.

**Pass condition:** The ideal equal-amplitude, opposite-phase case approaches numerical zero; deliberately introduced errors produce the expected residual. Verify behaviour at startup, at the edges of each buffer and at non-integer cycle boundaries.

This validates the mathematics and implementation, not physical ANC.

### Phase 4 — Run a restricted acoustic experiment

**Objective:** Evaluate the most favourable stationary-noise case that can be attempted without buying hardware.

Use the laptop's built-in speakers to generate a constant low-frequency tone at a safe listening level. Keep the relative positions of the laptop, noise source and ears fixed. Compare microphone capture, unprocessed pass-through and a cautiously calibrated candidate cancellation signal.

Begin with one stable frequency, then repeat across a narrow band. Do not use a continuously varying noise source for the first experiment.

The candidate controller should use a characterized, fixed filter rather than assume that simply negating the microphone waveform will cancel the noise. Without a reliable error signal at the ear, filter tuning will be limited and may be done only under carefully controlled conditions.

**Deliverable:** A reproducible experiment notebook, recorded input and output data, parameter settings, listening notes and a list of observed failure cases.

**Pass condition:** Repeatable evidence that the particular test tone becomes quieter at the listening position, not merely that an inverted waveform was generated. With the current equipment, a trustworthy measurement of that reduction at the ear will remain unavailable; any claimed result must be labelled accordingly.

### Phase 5 — Review the evidence and make a go/no-go decision

**Objective:** Determine whether the software-only architecture has demonstrated enough to justify further development.

Compare timing measurements with the frequency-dependent phase budget. Inspect CPU load, glitches, filter stability and whether the candidate signal increases perceived noise. Separate simulated cancellation from physical observations in the report.

**Deliverable:** A final experiment report with measured values, confidence limits where justified, reproducibility instructions and a decision on what the existing setup can achieve.

**Pass condition:** Proceed only if a controlled experiment produces a repeatable benefit without unstable output, excessive amplification or unexplained artifacts. Otherwise, record the limiting factor and stop rather than endlessly tuning software.

## 7. Algorithm strategy: what should actually be implemented?

Do not begin by taking the microphone waveform, multiplying it by `-1`, and playing the result. That is the simplest demonstration of the idea, but it is not a reliable ANC algorithm.

### Level 1 — Direct signal processing

Implement pass-through, gain control, a basic finite impulse response (FIR) filter and test-tone generation. These allow validation of the audio callback, processing speed and sample alignment without claiming that cancellation is occurring.

### Level 2 — Narrowband, model-based experiments

For a stationary tone, investigate a controlled cancellation signal with tunable amplitude, phase and delay. Compare different parameters under fixed, reproducible conditions.

This is an experiment rather than a general ANC solution. The best settings for one position and frequency may fail as soon as the noise source or listener moves.

### Level 3 — FxLMS in simulation, not yet as the real-world controller

The Filtered-x Least Mean Squares (FxLMS) algorithm is a standard ANC technique. Its controller adapts a filter using the residual error after the anti-noise passes through the acoustic path. The controller also accounts for an estimate of the secondary path. Research describes its computational efficiency, but also emphasizes sensitivity to system delay, acoustic-path modelling and the quality of the reference signal.

For the initial hardware setup, implement and validate FxLMS against simulated signals. However, the built-in laptop microphone is not positioned at the ear and does not measure the true residual there. Consequently, we cannot claim to have a conventional, properly adapted FxLMS headphone ANC system with this hardware alone.

**Key decision:** Make FxLMS simulation part of DSP validation, but do not make a real-world FxLMS controller the first deliverable. First prove the audio pipeline and determine whether the acoustic setup offers a practical opportunity for cancellation.

Reference:
- Feedforward ANC causality research: https://www.sciencedirect.com/science/article/pii/S0003682X1400005X

## 8. Suggested project structure

Keep the first version small and measurable. Avoid building a graphical interface or installing a large machine-learning stack before the core experiments work.

```text
software-anc/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp
│   ├── audio/
│   │   ├── device_probe.cpp
│   │   ├── wasapi_capture.cpp
│   │   └── wasapi_render.cpp
│   ├── core/
│   │   ├── audio_config.h
│   │   ├── audio_metrics.h
│   │   └── ring_buffer.h
│   └── dsp/
│       ├── pass_through.cpp
│       ├── fir_filter.cpp
│       ├── tone_generator.cpp
│       └── narrowband_experiment.cpp
├── tests/
│   ├── test_phase_math.cpp
│   ├── test_filter.cpp
│   └── test_buffer_boundaries.cpp
├── experiments/
│   ├── generate_test_signals.py
│   ├── analyze_latency.py
│   ├── analyze_spectrum.py
│   └── simulated_fxlms.py
└── results/
    ├── device_probe.txt
    ├── latency_report.csv
    └── experiment_notes.md
```

The actual WASAPI implementation should use the Windows SDK directly. The Python scripts are for offline experiments and analysis only; all live capture, filtering and playback must run in the native audio process.

## 9. How success will be measured

A convincing prototype needs evidence, not just a noise-cancelling toggle in its interface.

| Metric | Measurement method | What it proves |
|---|---|---|
| Device support | WASAPI capability report | The hardware exposes usable audio endpoints |
| Audio stability | Glitch and discontinuity counters | The chosen period is operationally stable |
| Processing duration | High-resolution timing around DSP blocks | Processing fits within its scheduling budget |
| Input-to-output latency | Device timestamps, supported timing APIs and a qualified physical test | How much delay the pipeline introduces |
| Phase and gain response | Synthetic signals and filter tests | The DSP behaves as intended |
| Noise reduction | Residual noise at the listening position, measured against a baseline | Whether physical cancellation occurs |

There is an important limitation in the last row. With no microphone positioned to measure the residual acoustic noise at the ear, the software-only setup cannot give a reliable objective noise-reduction measurement at that location. We can document listening observations and validate the signal processing, but those results must not be presented as measured ANC performance.

## 10. Risks and safety controls

The application should be fail-safe from the beginning.

- Start in muted mode. Make any candidate anti-noise output opt-in and begin at low volume.
- Never let an unstable filter produce unrestricted output. If processing fails, mute the candidate signal.
- Detect clipping, buffer starvation and unexpected amplitude growth.
- Do not test in traffic, while crossing roads or anywhere that hearing environmental sounds is needed for safety.
- Keep the original earphone audio pass-through separate from the experimental cancellation output, so modes can be compared without silently changing gain or filtering.
- Do not judge success by an inverted waveform looking correct in a graph. The residual acoustic noise is what matters.

## 11. Research papers and documentation

These sources form the technical foundation for the project.

### 1. Kuo and Morgan — *Active Noise Control: A Tutorial Review* (1999)

*Proceedings of the IEEE, 87(6), 943–975.* The foundational overview for feedforward and feedback control, adaptive filters, secondary-path modelling and real-time implementation.

- Paper: https://doi.org/10.1109/5.763310

### 2. *Causality Study on a Feedforward Active Noise Control Headset with Different Noise Coming Directions in Free Field*

Explains why cancellation bandwidth and attenuation deteriorate when system delay exceeds the available acoustic time advance. This is especially relevant to the laptop-microphone geometry.

- Research paper: https://www.sciencedirect.com/science/article/pii/S0003682X1400005X

### 3. Kuo, Mitra and Gan — *Active Noise Control System for Headphone Applications* (2006)

Studies ANC headphone control and the importance of the error-microphone location. It highlights why an acoustic measurement near the ear is valuable for tuning the controller.

- Find the paper: https://www.researchgate.net/publication/3332742_Active_noise_control_system_for_headphone_applications

### 4. Microsoft — *Low Latency Audio*

Implementation reference for Windows audio-engine periods, low-latency shared streams and the native APIs to use.

- Windows documentation: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- WASAPI documentation: https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi

### 5. PortAudio — *Audio Latency* and *Writing a Callback Function*

Useful implementation guidance for capture and playback buffering, the trade-off between low latency and glitches, and avoiding unpredictable work in the audio callback.

- Latency guidance: https://portaudio.com/docs/latency.html
- Callback implementation guidance: https://portaudio.com/docs/v19-doxydocs/writing_a_callback.html

## 12. Final development decision

The first milestone should be a native Windows C++ application that reports the available audio endpoints, starts microphone capture and earphone playback, and measures its timing without adding DSP delay.

Only after that works should filters, synthetic anti-phase tests and the controlled low-frequency experiment be introduced.

The central question this project needs to answer is not whether C++ can invert a waveform. It can. The question is whether the existing laptop microphone provides enough usable acoustic information, early enough, to generate a signal that reduces noise at the ears despite audio and acoustic delays.

**That is the experiment worth building first.** It provides a way to learn from real measurements while keeping the software-only constraint intact, and it helps distinguish limitations that can be addressed in code from those imposed by the physical setup.
