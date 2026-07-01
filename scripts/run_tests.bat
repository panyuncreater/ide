@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang IDE test script — 自动检测 VS + Qt，构建并运行测试
REM ------------------------------------------------------------
REM 修复问题 4（scripts/ 硬编码路径）。
REM ============================================================

REM --- Locate Visual Studio via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Please install Visual Studio.
    exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [ERROR] No Visual Studio installation detected.
    exit /b 1
)

REM --- Initialize MSVC environment ---
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] MSVC environment initialization failed.
    exit /b 1
)
echo [INFO] MSVC environment ready.

REM --- Detect Qt6 path ---
if not defined QTDIR (
    for /f "delims=" %%v in ('dir /b /ad "D:\qt\6.*" 2^>nul') do (
        if exist "D:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=D:\qt\%%v\msvc2022_64"
    )
    for /f "delims=" %%v in ('dir /b /ad "C:\qt\6.*" 2^>nul') do (
        if exist "C:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=C:\qt\%%v\msvc2022_64"
    )
)
if not defined QTDIR (
    echo [ERROR] QTDIR not set and Qt6 (msvc2022_64) not found under D:\qt or C:\qt.
    echo        Set QTDIR to your Qt6 install dir, e.g.: set QTDIR=D:\qt\6.10.3\msvc2022_64
    exit /b 1
)
echo [INFO] Qt6: !QTDIR!
set "PATH=!QTDIR!\bin;%PATH%"

REM --- Configure and build ---
set "PROJECT_ROOT=%~dp0.."
cd /d "%PROJECT_ROOT%"

echo [INFO] Configuring with configure.bat...
call configure.bat
if errorlevel 1 exit /b 1

echo [INFO] Building tests...
cmake --build out/build/debug --target minilang_tests
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [INFO] Running tests...
cd /d "%PROJECT_ROOT%\out\build\debug\tests"
minilang_tests.exe --gtest_brief=1
set "TESTS_EXIT_CODE=!errorlevel!"
echo TESTS_EXIT_CODE=!TESTS_EXIT_CODE!
exit /b !TESTS_EXIT_CODE!
