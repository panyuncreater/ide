@echo off
REM ============================================================
REM Thin wrapper -- delegates to scripts\build_ide.bat
REM ============================================================
@call "%~dp0scripts\build_ide.bat" %*
