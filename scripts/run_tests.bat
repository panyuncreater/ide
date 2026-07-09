@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang IDE test script
REM ------------------------------------------------------------
REM Usage:
REM   run_tests.bat            Build and test Debug (default)
REM   run_tests.bat release    Build and test Release
REM ============================================================

REM --- 构建类型 ---
set "BUILD_TYPE=debug"
if /i "%~1"=="release" set "BUILD_TYPE=release"

REM --- 使用公共头部：VS 检测 + MSVC 初始化 + Qt 检测 ---
call "%~dp0_common.bat"
if errorlevel 1 exit /b 1

REM --- Configure (if not already configured) ---
if not exist "outuild\%BUILD_TYPE%uild.ninja" (
    echo [INFO] Configuring with configure.bat %BUILD_TYPE%...
    call "%~dp0..\configure.bat" %BUILD_TYPE%
    if errorlevel 1 exit /b 1
)

echo [INFO] Building tests (%BUILD_TYPE%)...
cmake --build out/build/%BUILD_TYPE% --target minilang_tests
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [INFO] Running tests via CTest...
cd /d "%PROJECT_ROOT%\outuild\%BUILD_TYPE%"
ctest --output-on-failure --no-compress-output
set "TESTS_EXIT_CODE=!errorlevel!"
echo TESTS_EXIT_CODE=!TESTS_EXIT_CODE!
exit /b !TESTS_EXIT_CODE!
