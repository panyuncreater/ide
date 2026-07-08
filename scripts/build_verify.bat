@echo off
call "D:\vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "C:\Users\v\Desktop\ide\ide"
if not exist "out\build\debug\build.ninja" (
    echo [INFO] Configuring CMake...
    cmake -S . -B out/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="C:\Qt\6.10.3\msvc2022_64"
    if errorlevel 1 exit /b 1
)
echo [INFO] Building MiniLang IDE...
cmake --build out/build/debug --target minilang_ide 2>&1
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)
echo [SUCCESS] Build OK
