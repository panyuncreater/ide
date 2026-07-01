@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang IDE CMake configure script
REM ------------------------------------------------------------
REM Auto-detects Visual Studio, initializes the MSVC environment,
REM then runs cmake with a preset. The Qt-Debug preset (from
REM CMakeUserPresets.json) already sets QTDIR internally, so Qt
REM path detection is only needed for the shared windows-msvc-*
REM presets in CMakePresets.json.
REM
REM Usage:
REM   configure.bat              Configure Debug (default)
REM   configure.bat release      Configure Release
REM   configure.bat -- -DFOO=bar Pass extra args to cmake
REM ============================================================

REM Default preset: prefer Qt-Debug from CMakeUserPresets.json if present,
REM otherwise fall back to the shared windows-msvc-debug in CMakePresets.json.
set "PRESET=windows-msvc-debug"
if exist "%~dp0CMakeUserPresets.json" set "PRESET=Qt-Debug"
if /i "%~1"=="release" (
    if exist "%~dp0CMakeUserPresets.json" (
        set "PRESET=Qt-Release"
    ) else (
        set "PRESET=windows-msvc-release"
    )
    shift
)
if /i "%~1"=="debug" (
    if exist "%~dp0CMakeUserPresets.json" (
        set "PRESET=Qt-Debug"
    ) else (
        set "PRESET=windows-msvc-debug"
    )
    shift
)

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
echo [INFO] MSVC environment ready: cl.exe in PATH

REM --- Prefer VS-bundled cmake (recognizes the installed MSVC version;
REM     pip/pipx cmake may not know new MSVC like 19.51) ---
set "CMAKE_CMD=cmake"
set "VS_CMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%VS_CMAKE%" (
    set "CMAKE_CMD=%VS_CMAKE%"
    echo [INFO] Using VS-bundled cmake
) else (
    echo [INFO] Using cmake from PATH
)

REM --- Detect Qt6 path (only for windows-msvc-* presets) ---
echo !PRESET! | findstr /b "windows-msvc-" >nul
if not errorlevel 1 (
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
        echo        Set QTDIR to your Qt6 install dir, e.g.:
        echo        set QTDIR=D:\qt\6.10.3\msvc2022_64
        exit /b 1
    )
    echo [INFO] Qt6: !QTDIR!
)

REM --- Run CMake ---
echo [INFO] Running: "!CMAKE_CMD!" --preset !PRESET! %1 %2 %3 %4 %5
"!CMAKE_CMD!" --preset !PRESET! %1 %2 %3 %4 %5
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)
echo [INFO] CMake configuration done.
endlocal
