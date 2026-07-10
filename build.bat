@echo off
REM ============================================================
REM Thin wrapper -- delegates to scripts\build.bat
REM ============================================================
@call "%~dp0scripts\build.bat" %*
