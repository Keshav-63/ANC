@echo off
REM ============================================================================
REM  REAL-WORLD ANC DEMO  -  noise in, anti-noise out, cancelled in the air
REM ============================================================================
REM  Chain:  laptop speakers hum -> laptop mic hears it (raw, effects bypassed)
REM          -> software measures amplitude+phase and computes the anti-tone
REM          -> your earbud plays it -> the two cancel in the air at the mic.
REM
REM  BEFORE YOU RUN:
REM    1. Hold the EARBUD'S NOZZLE pressed against the LAPTOP MIC HOLE.
REM       (The null exists where the noise is sensed - that is the mic spot.
REM        You can listen right there, or lean your ear next to it.)
REM    2. Keep the buds awake/connected. Keep hands off the mic during a cycle:
REM       touching the mic adds handling noise the loop will faithfully track.
REM    3. Speaker volume: whatever you like - the tool prints the level it
REM       measured at the mic. For the deepest null, the baseline should read
REM       between about -45 and -55 dBFS. If it prints much louder than -45,
REM       re-run with a smaller source:  run_real_demo.cmd 0.05
REM
REM  USAGE:   run_real_demo.cmd [source-drive-gain]
REM           (default 0.1; lower = quieter hum = deeper achievable null)
REM
REM  WHAT YOU GET, per cycle (~20 s), repeated 3 times:
REM    6 s  plain hum                 (baseline - no anti-noise)
REM    ~8 s  phase search              (anti-noise sweeps phase, finds its null)
REM    6 s  locked cancellation       (quietest window - this is the demo)
REM  The console prints baseline/cancel residual per window: the difference
REM  is the depth of the real acoustic cancellation, measured at the mic.
REM  Whole run: under 90 seconds.
REM ============================================================================
setlocal
cd /d "%~dp0"

set SRCGAIN=%1
if "%SRCGAIN%"=="" set SRCGAIN=0.1

if not exist "build\Release\narrowband_experiment.exe" (
    echo Build first:  cmake --build build --config Release
    exit /b 1
)

echo.
echo   ============================================================
echo    REAL-WORLD ANC DEMO   freq 450 Hz   source drive %SRCGAIN%
echo    Hold the earbud nozzle against the laptop mic NOW.
echo    Source starts in 4 seconds; whole run under 90 seconds.
echo   ============================================================
echo.

REM --- the noise source: 450 Hz hum from the laptop's own speakers ----------
start "ANC noise source (leave this window alone)" ^
    build\Release\narrowband_experiment.exe --experiment drive_cancel --freq 450 ^
    --drive-gain %SRCGAIN% --cancel-gain 0.0 --no-sweep --cycles 0 --warmup 95 ^
    --capture-device "Microphone Array" --render-device "Speakers" ^
    --record-input results\src_mic_discard.wav --record-output results\src_out.wav ^
    --windows-csv results\src_windows.csv

REM --- wake chime in the earbuds, then start the cancellation loop ----------
timeout /t 15 /nobreak >nul
powershell -NoProfile -Command "[System.Media.SystemSounds]::Asterisk.Play(); Start-Sleep -Seconds 1"

REM --- the loop: mic -> measure -> anti-tone -> earbud ----------------------
build\Release\narrowband_experiment.exe --experiment tracked_cancel --freq 450 ^
    --cancel-gain 500 --auto-trim --cycles 3 --window-sec 6 --warmup 1 --capture-raw ^
    --capture-device "Microphone Array" --render-device "pTron" ^
    --record-input results\tracked_mic.wav --record-output results\tracked_out.wav ^
    --windows-csv results\tracked_windows.csv

echo.
echo   Demo finished. Verify the numbers independently:
echo     python experiments\analyze_spectrum.py --windows results\tracked_windows.csv --mic results\tracked_mic.wav
echo   The other window (noise source) closes on its own.
endlocal