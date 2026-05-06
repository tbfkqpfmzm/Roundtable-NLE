@echo off
echo Building ROUNDTABLE...
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    pause
    exit /b 1
)
pause
