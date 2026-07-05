@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang test script -- run already-built tests (no build step)
REM ------------------------------------------------------------
REM Usage:
REM   run_tests_only.bat            Run Debug tests via CTest
REM   run_tests_only.bat release    Run Release tests via CTest
REM ============================================================

REM --- Build type ---
set "BUILD_TYPE=debug"
if /i "%~1"=="release" set "BUILD_TYPE=release"

REM --- Use shared header: VS detection + MSVC init + Qt detection ---
call "%~dp0_common.bat"
if errorlevel 1 exit /b 1

cd /d "%PROJECT_ROOT%\out\build\%BUILD_TYPE%"

if not exist "tests\minilang_tests.exe" (
    echo [ERROR] minilang_tests.exe not found. Build tests first.
    exit /b 1
)

echo [INFO] Running tests via CTest (%BUILD_TYPE%)...
ctest --output-on-failure
set "TESTS_EXIT_CODE=!errorlevel!"
echo TESTS_EXIT_CODE=!TESTS_EXIT_CODE!
exit /b !TESTS_EXIT_CODE!
