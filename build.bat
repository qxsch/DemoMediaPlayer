@echo off
rem ──────────────────────────────────────────────────────────────
rem  build.bat – Build DemoMediaPlayer for Windows via Docker
rem
rem  Extra arguments are forwarded to "docker build", e.g.:
rem    build.bat --build-arg MPV_DEV_URL="https://…"
rem    build.bat --no-cache
rem ──────────────────────────────────────────────────────────────
setlocal

echo === Building DemoMediaPlayer for Windows (x86_64) ===
echo.

docker build --target dist --output type=local,dest=.\dist %* .

if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

echo.
echo Build complete!  Output:
dir /B .\dist\
echo.
echo Run dist\mediaplayer.exe to start the player.
endlocal
