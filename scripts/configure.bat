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
if exist "%~dp0..\CMakeUserPresets.json" set "PRESET=Qt-Debug"
if /i "%~1"=="release" (
    if exist "%~dp0..\CMakeUserPresets.json" (
        set "PRESET=Qt-Release"
    ) else (
        set "PRESET=windows-msvc-release"
    )
    shift
)
if /i "%~1"=="debug" (
    if exist "%~dp0..\CMakeUserPresets.json" (
        set "PRESET=Qt-Debug"
    ) else (
        set "PRESET=windows-msvc-debug"
    )
    shift
)

REM --- 使用公共头部：VS 检测 + MSVC 初始化 + Qt 检测 ---
call "%~dp0_common.bat"
if errorlevel 1 exit /b 1

REM --- Prefer VS-bundled cmake (recognizes the installed MSVC version;
REM     pip/pipx cmake may not know new MSVC like 19.51) ---
set "CMAKE_CMD=cmake"
set "VS_CMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMakein\cmake.exe"
if exist "%VS_CMAKE%" (
    set "CMAKE_CMD=%VS_CMAKE%"
    echo [INFO] Using VS-bundled cmake
) else (
    echo [INFO] Using cmake from PATH
)

REM --- 对 windows-msvc-* 预设确认 QTDIR 已设置 ---
echo !PRESET! | findstr /b "windows-msvc-" >/dev/null
if not errorlevel 1 (
    if not defined QTDIR (
        echo [ERROR] QTDIR not set for preset !PRESET!
        exit /b 1
    )
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
