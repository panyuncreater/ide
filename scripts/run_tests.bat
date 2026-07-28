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
if not exist "out\build\%BUILD_TYPE%\build.ninja" (
    echo [INFO] Configuring with configure.bat %BUILD_TYPE%...
    call "%~dp0..\configure.bat" %BUILD_TYPE%
    if errorlevel 1 exit /b 1
)

echo [INFO] Building tests (%BUILD_TYPE%)...
set "BUILD_LOG=%TEMP%\minilang_build_tests.log"
cmake --build out/build/%BUILD_TYPE% --target minilang_tests > "%BUILD_LOG%" 2>&1
set "BUILD_EXIT=!errorlevel!"
type "%BUILD_LOG%"
if !BUILD_EXIT! neq 0 (
    REM testing.md known limitation #3: after core header ABI changes,
    REM stale unity objs cause LNK2019/LNK1120. Clean and retry once.
    findstr /c:"LNK2019" /c:"LNK2001" /c:"LNK1120" "%BUILD_LOG%" >nul 2>&1
    if not errorlevel 1 (
        echo [WARN] Link failure detected - cleaning stale unity objects and retrying...
        call "%~dp0fix_stale_objs.bat" %BUILD_TYPE%
        cmake --build out/build/%BUILD_TYPE% --target minilang_tests
        set "BUILD_EXIT=!errorlevel!"
    )
)
if !BUILD_EXIT! neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [INFO] Running tests via CTest...
cd /d "%PROJECT_ROOT%\out\build\%BUILD_TYPE%"
ctest --output-on-failure --no-compress-output
set "TESTS_EXIT_CODE=!errorlevel!"
echo TESTS_EXIT_CODE=!TESTS_EXIT_CODE!
exit /b !TESTS_EXIT_CODE!
