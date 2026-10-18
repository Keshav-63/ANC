@echo off
REM ============================================================================
REM  IN-EAR LIVE ANC DEMO  -  cancellation felt in the earbud, realtime
REM ============================================================================
REM  Chain: laptop speakers hum 450 Hz -> your laptop mic hears it ->
REM  software tracks it -> your earbud plays the matching anti-noise ->
REM  the two cancel at your eardrum while you listen.
REM
REM  BEFORE YOU RUN:
REM    1. Put the earbuds IN your ears, ANC OFF (normal mode).
REM    2. Windows volume for "Headphones (pTron)" = 100%  (this is what
REM       scales our signal into the driver - it was the reason an earlier
REM       version sounded like "no anti-tone at all").
REM    3. Set the laptop SPEAKER volume so the hum is clearly audible but
REM       comfortable. If it gets irritating: press Ctrl+C or close this
REM       window - everything stops immediately.
REM
REM  WHAT YOU GET:  10 s hum only -> 10 s cancelled,  twice (~50 s total).
REM  The hum you hear in the "cancelled" window is the real-world noise
REM  being cancelled at your eardrum, generated live from the mic input.
REM
REM  USAGE:   run_in_ear_demo.cmd
REM  TUNED VALUES (found by listening, see results/experiment_notes.md):
REM    freq 450 Hz (this room is silent there), anti-tone level 0.05
REM    (--cancel-gain 2.5), phase trim 0 deg (null sits near 0/180; the
REM    earbud driver inverts phase at 450 Hz).
REM ============================================================================
setlocal
cd /d "%~dp0"

if not exist "build\Release\narrowband_experiment.exe" (
    echo Build first:  cmake --build build --config Release
    exit /b 1
)

echo.
echo   ======================================================
echo    IN-EAR LIVE ANC DEMO   450 Hz   ~50 seconds
echo    Earbuds in, ANC off, pTron volume 100%.
echo    Noise source starts in 3 s; everything stops by itself.
echo    To abort early: press Ctrl+C or close this window.
echo   ======================================================
echo.

REM --- the noise source: 450 Hz hum from the laptop's own speakers ----------
start "ANC noise source (closes by itself)" ^
    build\Release\narrowband_experiment.exe --experiment drive_cancel --freq 450 ^
    --drive-gain 0.4 --cancel-gain 0.0 --no-sweep --cycles 0 --warmup 75 ^
    --capture-device "Microphone Array" --render-device "Speakers" ^
    --record-input results\src_mic_discard.wav --record-output results\src_out.wav ^
    --windows-csv results\src_windows.csv

REM --- the loop: mic -> track -> anti-noise -> earbud -----------------------
timeout /t 10 /nobreak >nul
build\Release\narrowband_experiment.exe --experiment tracked_cancel --freq 450 ^
    --cancel-gain 2.5 --tracked-phase-trim 0 --cycles 2 --window-sec 10 --warmup 1 ^
    --capture-raw --capture-device "Microphone Array" --render-device "pTron" ^
    --record-input results\in_ear_mic.wav --record-output results\in_ear_out.wav ^
    --windows-csv results\in_ear_windows.csv

echo.
echo   Done. (Tip: the recorded microphone capture and output are in
echo   results\in_ear_mic.wav / in_ear_out.wav for offline analysis.)
endlocal