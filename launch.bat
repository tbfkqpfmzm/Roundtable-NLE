@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

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

REM Delegate to launch.vbs for a truly invisible launch (no terminal flash).
REM launch.vbs handles Debug vs Release selection and hides all windows.
start "" /min wscript.exe "%~dp0launch.vbs"
exit /b 0
