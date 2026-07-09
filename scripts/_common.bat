@echo off
REM ============================================================
REM MiniLang 构建脚本 — 公共头部（VS 检测 + MSVC 初始化 + Qt 检测）
REM ------------------------------------------------------------
REM 由 configure.bat / build.bat / run_tests.bat 通过
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

REM --- 检测 Qt6 路径（多策略：环境变量 → PATH → 常见安装目录）---
if defined QTDIR goto :qtdir_found

REM 策略 1：尝试从 PATH 中查找 qmake 反推 QTDIR
for /f "delims=" %%q in ('where qmake 2^>nul') do (
    for %%p in ("%%~dpq..") do set "QTDIR=%%~fp"
)
if defined QTDIR (
    if not exist "%QTDIR%\lib\cmake\Qt6" set "QTDIR="
)
if defined QTDIR goto :qtdir_found

REM 策略 2：搜索常见安装路径（支持多个 Qt 安装位置）
for %%D in (D:\qt C:\qt D:\Qt C:\Qt "%USERPROFILE%\Qt" "%LOCALAPPDATA%\Qt") do (
    if exist "%%~D" (
        for /f "delims=" %%v in ('dir /b /ad "%%~D\6.*" 2^>nul') do (
            for %%k in (msvc2022_64 msvc2019_64 mingw_64 gcc_64 clang_64) do (
                if exist "%%~D\%%v\%%k\lib\cmake\Qt6" set "QTDIR=%%~D\%%v\%%k"
            )
        )
    )
)

:qtdir_found
if not defined QTDIR (
    echo [ERROR] QTDIR not set and Qt6 not found.
    echo        Searched: PATH, D:\qt, C:\qt, D:\Qt, C:\Qt, %%USERPROFILE%%\Qt, %%LOCALAPPDATA%%\Qt
    echo        Set QTDIR to your Qt6 install dir, e.g.: set QTDIR=D:\qt\6.10.3\msvc2022_64
    exit /b 1
)
echo [INFO] Qt6: %QTDIR%
set "PATH=%QTDIR%\bin;%PATH%"
