@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

REM Debug launcher — keeps console visible, captures stdout+stderr to debug.log
REM Use this when you need to see crash output or console log messages.

REM Use local Qt from third_party (portable), fall back to system Qt
if exist "%~dp0third_party\qt\6.8.3\msvc2022_64\bin" (
    set "PATH=%~dp0third_party\qt\6.8.3\msvc2022_64\bin;%PATH%"
) else if exist "C:\Qt\6.8.3\msvc2022_64\bin" (
    set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
) else (
    echo WARNING: Qt 6.8.3 not found. Run setup.ps1 first.
    pause
    exit /b 1
)

REM Add FFmpeg DLLs to PATH
if exist "%~dp0third_party\ffmpeg\bin" (
    set "PATH=%~dp0third_party\ffmpeg\bin;%PATH%"
)

REM Prefer the Debug build -- has CRT heap validation for catching corruption
if exist "%~dp0build\bin\Debug\roundtable.exe" (
    set "TARGET=%~dp0build\bin\Debug\roundtable.exe"
    echo Debug build selected ^(heap validation enabled^)
) else if exist "%~dp0build\bin\Release\roundtable.exe" (
    set "TARGET=%~dp0build\bin\Release\roundtable.exe"
    echo Release build selected ^(no heap validation^)
) else if exist "%~dp0roundtable.exe" (
    set "TARGET=%~dp0roundtable.exe"
) else (
    echo ERROR: roundtable.exe not found.
    echo Looked in: %~dp0build\bin\Debug\ , %~dp0build\bin\Release\ , %~dp0
    pause
    exit /b 1
)

echo Launching: !TARGET!
echo Console output is being saved to debug.log
echo.
"!TARGET!" >"%~dp0debug.log" 2>&1
echo.
echo Application exited. Check debug.log for output.
pause
