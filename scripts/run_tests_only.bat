@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang IDE test script — 仅运行已构建的测试（无构建步骤）
REM ------------------------------------------------------------
REM 修复问题 4（scripts/ 硬编码路径）。
REM ============================================================

REM --- Detect Qt6 path (测试需要 Qt DLL) ---
if not defined QTDIR (
    for /f "delims=" %%v in ('dir /b /ad "D:\qt\6.*" 2^>nul') do (
        if exist "D:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=D:\qt\%%v\msvc2022_64"
    )
    for /f "delims=" %%v in ('dir /b /ad "C:\qt\6.*" 2^>nul') do (
        if exist "C:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=C:\qt\%%v\msvc2022_64"
    )
)
if defined QTDIR set "PATH=!QTDIR!\bin;%PATH%"

set "PROJECT_ROOT=%~dp0.."
cd /d "%PROJECT_ROOT%\out\build\debug\tests"

if not exist "minilang_tests.exe" (
    echo [ERROR] minilang_tests.exe not found. Run scripts\build_ide.bat or scripts\run_tests.bat first.
    exit /b 1
)

minilang_tests.exe --gtest_brief=1
set "TESTS_EXIT_CODE=!errorlevel!"
echo TESTS_EXIT_CODE=!TESTS_EXIT_CODE!
exit /b !TESTS_EXIT_CODE!
