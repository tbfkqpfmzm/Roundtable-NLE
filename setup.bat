@echo off
echo ================================================================
echo   ROUNDTABLE NLE v2 — Setup
echo ================================================================
echo.
echo This will set up all dependencies and configure the build.
echo Prerequisites: Visual Studio 2022 (C++ workload), Git, Python 3
echo.
powershell -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
if %ERRORLEVEL% neq 0 (
    echo.
    echo Setup failed. See errors above.
    pause
    exit /b 1
)
pause
