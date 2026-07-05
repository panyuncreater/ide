@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM MiniLang IDE build script — 自动检测 VS + Qt 并构建 IDE
REM ------------------------------------------------------------
REM Usage:
REM   build_ide.bat            Build Debug (default)
REM   build_ide.bat release    Build Release
REM ============================================================

REM --- 构建类型 ---
set "BUILD_TYPE=debug"
if /i "%~1"=="release" set "BUILD_TYPE=release"

REM --- 使用公共头部：VS 检测 + MSVC 初始化 + Qt 检测 ---
call "%~dp0scripts\_common.bat"
if errorlevel 1 exit /b 1

REM --- Configure (if not already configured) ---
if not exist "out\build\%BUILD_TYPE%\build.ninja" (
    echo [INFO] Configuring with configure.bat %BUILD_TYPE%...
    call configure.bat %BUILD_TYPE%
    if errorlevel 1 exit /b 1
)

echo [INFO] Building IDE (%BUILD_TYPE%)...
cmake --build out/build/%BUILD_TYPE% --target minilang_ide
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)
echo [INFO] Build OK: out\build\%BUILD_TYPE%\minilang_ide.exe
endlocal
