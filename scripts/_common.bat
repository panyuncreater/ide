@echo off
REM ============================================================
REM MiniLang build scripts - common header (VS detection + MSVC init + Qt detection)
REM ------------------------------------------------------------
REM Included by configure.bat / build.bat / run_tests.bat via
REM call scripts\_common.bat to dedupe the environment detection code.
REM After return, %VSINSTALL% and %QTDIR% are set, the MSVC environment is
REM initialized, and the current directory is the project root.
REM ============================================================

REM --- Locate project root (_common.bat lives in scripts/, root is one level up) ---
set "PROJECT_ROOT=%~dp0.."
cd /d "%PROJECT_ROOT%"

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
echo [INFO] Visual Studio: %VSINSTALL%

REM --- Initialize MSVC environment ---
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] MSVC environment initialization failed.
    exit /b 1
)
echo [INFO] MSVC environment ready.

REM --- Detect Qt6 path (strategies: env var -> PATH -> common install dirs) ---
if defined QTDIR goto :qtdir_found

REM Strategy 1: infer QTDIR from qmake found on PATH
for /f "delims=" %%q in ('where qmake 2^>nul') do (
    for %%p in ("%%~dpq..") do set "QTDIR=%%~fp"
)
if defined QTDIR (
    if not exist "%QTDIR%\lib\cmake\Qt6" set "QTDIR="
)
if defined QTDIR goto :qtdir_found

REM Strategy 2: search common install roots (supports multiple Qt locations)
REM fix(2026-07-19): the for loop never breaks on first match, so mingw_64 would
REM overwrite msvc2022_64. Probe in priority order; exit on first hit (goto).
for %%D in (D:\qt C:\qt D:\Qt C:\Qt "%USERPROFILE%\Qt" "%LOCALAPPDATA%\Qt") do (
    if exist "%%~D" (
        for /f "delims=" %%v in ('dir /b /ad "%%~D\6.*" 2^>nul') do (
            for %%k in (msvc2022_64 msvc2019_64 mingw_64 gcc_64 clang_64) do (
                if exist "%%~D\%%v\%%k\lib\cmake\Qt6" (
                    set "QTDIR=%%~D\%%v\%%k"
                    goto :qtdir_found
                )
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
