@echo off
call "D:\vs\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=D:\qt\6.10.3\msvc2022_64\bin;%PATH%
set PROJECT_ROOT=%~dp0..
cd /d "%PROJECT_ROOT%"

if not exist "build_ide\CMakeCache.txt" (
    echo CONFIGURING_IDE_BUILD
    cmake -B build_ide -G "NMake Makefiles" -DCMAKE_CXX_COMPILER="D:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe" -DCMAKE_PREFIX_PATH="D:/qt/6.10.3/msvc2022_64" -DCMAKE_BUILD_TYPE=Debug
    if errorlevel 1 (
        echo CONFIGURE_FAILED
        exit /b 1
    )
    echo CONFIGURE_OK
)

cd /d "%PROJECT_ROOT%\build_ide"
nmake minilang_ide
if errorlevel 1 (
    echo BUILD_FAILED
    exit /b 1
)
echo IDE_BUILD_OK
