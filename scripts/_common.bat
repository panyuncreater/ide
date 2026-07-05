@echo off
REM ============================================================
REM MiniLang 构建脚本 — 公共头部（VS 检测 + MSVC 初始化 + Qt 检测）
REM ------------------------------------------------------------
REM 由 configure.bat / build_ide.bat / run_tests.bat 通过
REM call scripts\_common.bat 引入，消除三段重复的环境检测代码。
REM 执行后 %VSINSTALL%、%QTDIR% 已设置，MSVC 环境已初始化，
REM 当前目录已切换到项目根目录。
REM ============================================================

REM --- 定位项目根目录（_common.bat 在 scripts/ 下，根目录是上一级）---
set "PROJECT_ROOT=%~dp0.."
cd /d "%PROJECT_ROOT%"

REM --- 通过 vswhere 定位 Visual Studio ---
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
echo [INFO] Visual Studio: %VSINSTALL%

REM --- 初始化 MSVC 环境 ---
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] MSVC environment initialization failed.
    exit /b 1
)
echo [INFO] MSVC environment ready.

REM --- 检测 Qt6 路径 ---
if not defined QTDIR (
    for /f "delims=" %%v in ('dir /b /ad "D:\qt\6.*" 2^>nul') do (
        if exist "D:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=D:\qt\%%v\msvc2022_64"
    )
    for /f "delims=" %%v in ('dir /b /ad "C:\qt\6.*" 2^>nul') do (
        if exist "C:\qt\%%v\msvc2022_64\lib\cmake\Qt6" set "QTDIR=C:\qt\%%v\msvc2022_64"
    )
)
if not defined QTDIR (
    echo [ERROR] QTDIR not set and Qt6 ^(msvc2022_64^) not found under D:\qt or C:\qt.
    echo        Set QTDIR to your Qt6 install dir, e.g.: set QTDIR=D:\qt\6.10.3\msvc2022_64
    exit /b 1
)
echo [INFO] Qt6: %QTDIR%
set "PATH=%QTDIR%\bin;%PATH%"
