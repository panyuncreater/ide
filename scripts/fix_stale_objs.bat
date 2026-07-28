@echo off
setlocal

REM ============================================================
REM MiniLang stale unity object cleaner
REM ------------------------------------------------------------
REM Background (testing.md known limitation #3): after changing symbol
REM signatures in core headers (Bytecode.h / VmTypes.h etc., including
REM appending default parameters), Ninja's header dependency tracking
REM for unity batches may miss the change. minilang_core.lib then keeps
REM stale objects referencing old symbols, and consumers
REM (minilang_tests / minilang_ide) fail to link with LNK2019/LNK1120.
REM Remedy: delete the objects of the four core OBJECT sub-libraries
REM plus the aggregated minilang_core.lib, forcing the next build to
REM recompile those TUs (no full rebuild needed).
REM
REM Usage:
REM   fix_stale_objs.bat            Clean Debug build tree (default)
REM   fix_stale_objs.bat release    Clean Release build tree
REM ============================================================

set "BUILD_TYPE=debug"
if /i "%~1"=="release" set "BUILD_TYPE=release"

set "BUILD_DIR=%~dp0..\out\build\%BUILD_TYPE%"
if not exist "%BUILD_DIR%\build.ninja" (
    echo [ERROR] Build tree not found: %BUILD_DIR%
    exit /b 1
)

echo [INFO] Cleaning stale unity objects in %BUILD_DIR% ...
for %%L in (minilang_core_base minilang_frontend minilang_backend minilang_guibridge) do (
    if exist "%BUILD_DIR%\CMakeFiles\%%L.dir" (
        del /s /q "%BUILD_DIR%\CMakeFiles\%%L.dir\*.obj" >nul 2>&1
        echo [INFO]   cleaned CMakeFiles\%%L.dir
    )
)
if exist "%BUILD_DIR%\minilang_core.lib" (
    del /q "%BUILD_DIR%\minilang_core.lib" >nul 2>&1
    echo [INFO]   removed minilang_core.lib
)

echo [INFO] Done. Next build will recompile minilang_core sub-libraries.
endlocal
exit /b 0
