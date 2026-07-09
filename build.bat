@echo off
REM ============================================================
REM Thin wrapper -- delegates to scriptsuild.bat
REM ============================================================
@call "%~dp0scriptsuild.bat" %*
