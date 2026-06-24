@echo off
call "D:\vs\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=D:\qt\6.10.3\msvc2022_64\bin;%PATH%
set PROJECT_ROOT=%~dp0..

if not exist "%PROJECT_ROOT%\build_ide" (
    cmake -B "%PROJECT_ROOT%\build_ide" -S "%PROJECT_ROOT%" -G Ninja ^
        -DCMAKE_C_COMPILER="D:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe" ^
        -DCMAKE_CXX_COMPILER="D:/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe" ^
        -DCMAKE_PREFIX_PATH="D:/qt/6.10.3/msvc2022_64" ^
        -DCMAKE_BUILD_TYPE=Debug
    if errorlevel 1 ( echo CONFIG_FAILED & exit /b 1 )
)

cd /d "%PROJECT_ROOT%\build_ide"
"C:\Users\v\AppData\Local\Programs\Python\Python310\Scripts\ninja.exe" minilang_ide
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )
echo IDE_BUILD_OK
