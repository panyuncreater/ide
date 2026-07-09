@echo off
REM ============================================================
REM Thin wrapper -- delegates to scripts\configure.bat
REM ============================================================
@call "%~dp0scripts\configure.bat" %*
