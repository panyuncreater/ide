@echo off
REM ============================================================
REM Thin wrapper -- delegates to scripts\run_tests.bat
REM ============================================================
@call "%~dp0scripts\run_tests.bat" %*
