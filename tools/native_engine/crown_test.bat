@echo off
REM ============================================================
REM  crown_test.bat — A/B test the native engine subsystems
REM
REM  Sets diagnostic env vars then launches ROUNDTABLE. Use to
REM  pinpoint which native-engine feature is producing visual
REM  bugs. After the app closes, double-click this file again
REM  to run a different test.
REM ============================================================

setlocal EnableExtensions

REM Always start from a clean env-var slate
set "ROUNDTABLE_NATIVE_NO_IK="
set "ROUNDTABLE_NATIVE_NO_TC="

echo.
echo ============================================================
echo   CROWN TEST MENU  -  Pick a native-engine configuration
echo ============================================================
echo.
echo   1.  Disable IK only             (TC still ON)
echo   2.  Disable Transform Constraints only  (IK still ON)
echo   3.  Disable BOTH IK and TC      (only basic skeletal)
echo   4.  Native engine FULL          (all subsystems ON)
echo   5.  Quit
echo.
set /p "choice=Pick [1-5]: "

if "%choice%"=="1" goto :test_no_ik
if "%choice%"=="2" goto :test_no_tc
if "%choice%"=="3" goto :test_no_both
if "%choice%"=="4" goto :test_full
if "%choice%"=="5" goto :end

echo.
echo Invalid choice. Re-run and pick 1-5.
echo.
pause
exit /b 1

:test_no_ik
set "ROUNDTABLE_NATIVE_NO_IK=1"
echo.
echo [TEST 1] Launching with ROUNDTABLE_NATIVE_NO_IK=1
echo          IK solver: DISABLED
echo          Transform constraints: enabled
echo.
goto :launch

:test_no_tc
set "ROUNDTABLE_NATIVE_NO_TC=1"
echo.
echo [TEST 2] Launching with ROUNDTABLE_NATIVE_NO_TC=1
echo          IK solver: enabled
echo          Transform constraints: DISABLED
echo.
goto :launch

:test_no_both
set "ROUNDTABLE_NATIVE_NO_IK=1"
set "ROUNDTABLE_NATIVE_NO_TC=1"
echo.
echo [TEST 3] Launching with both diagnostics ON
echo          IK solver: DISABLED
echo          Transform constraints: DISABLED
echo.
goto :launch

:test_full
echo.
echo [TEST 4] Launching with default native engine
echo          IK solver: enabled
echo          Transform constraints: enabled
echo.
goto :launch

:launch
echo Load Crown after the app opens, then close it when done.
echo This window will exit automatically after launching.
echo Re-run crown_test.bat to try a different configuration.
echo.
call "%~dp0launch.bat"
goto :end

:end
endlocal
exit /b 0
