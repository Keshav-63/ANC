@echo off
REM ============================================================================
REM  INTERACTIVE REAL-WORLD ANC  -  your room's own noise, on demand
REM ============================================================================
REM  No test tone. No hum. The tool listens to your room, finds the dominant
REM  steady tone it makes (fan / drone / hum), and cancels that tone at the
REM  error microphone. You control it:
REM
REM      S   toggle cancellation ON / OFF      (noise returns when OFF)
REM      Q   quit
REM
REM  WHERE TO LISTEN - the null forms at the error microphone, so that is
REM  where the cancellation is real. Two ways to set up:
REM
REM   (A) MEASURED (proven, deepest null):
REM       hold the earbud nozzle against the laptop mic and listen right
REM       there with a bare ear. Expect 10+ dB, printed live in the console.
REM
REM   (B) FELT (earbud worn):
REM       put the earbud in your ear, ANC OFF, and press your ear over the
REM       laptop mic. The null then forms at your eardrum.
REM
REM  The console prints the level at the mic every 2 s and the measured
REM  reduction while cancellation is on.
REM ============================================================================
setlocal
cd /d "%~dp0"

if not exist "build\Release\narrowband_experiment.exe" (
    echo Build first:  cmake --build build --config Release
    exit /b 1
)

echo   Interactive ANC - S = start/stop cancellation, Q = quit
echo   (A) hold the earbud nozzle against the laptop mic, listen there, or
echo   (B) wear the earbud (ANC off) and press your ear over the mic.
echo.
build\Release\narrowband_experiment.exe --experiment interactive --cancel-gain 150 ^
    --capture-raw --capture-device "Microphone Array" --render-device "pTron"

echo.
echo   Session ended. Any recording is in results\interactive_mic.wav
endlocal