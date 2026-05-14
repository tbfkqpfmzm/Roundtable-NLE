@echo off
REM Launch Roundtable with Vulkan validation layers enabled.
REM
REM Mirrors launch.bat but sets ROUNDTABLE_VALIDATION before delegating to
REM launch.vbs.  Env vars set here propagate through wscript.exe to the
REM roundtable.exe child process via normal Windows env inheritance.
REM
REM Validation messages land in logs/perf_log.txt as
REM   [Vulkan VALIDATION ERROR] ...
REM lines at critical severity.  VALIDATION_FATAL=0 keeps the app running
REM past the first violation so you collect a complete picture.

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ROUNDTABLE_VALIDATION=1"
set "ROUNDTABLE_SYNC_VALIDATION=1"
set "ROUNDTABLE_VALIDATION_FATAL=0"

REM Same Qt + FFmpeg PATH setup as launch.bat.
if exist "%~dp0third_party\qt\6.8.3\msvc2022_64\bin" (
    set "PATH=%~dp0third_party\qt\6.8.3\msvc2022_64\bin;%PATH%"
) else if exist "C:\Qt\6.8.3\msvc2022_64\bin" (
    set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
) else (
    echo WARNING: Qt 6.8.3 not found. Run setup.ps1 first.
    pause
    exit /b 1
)

if exist "%~dp0third_party\ffmpeg\bin" (
    set "PATH=%~dp0third_party\ffmpeg\bin;%PATH%"
)

start "" /min wscript.exe "%~dp0launch.vbs"
exit /b 0
