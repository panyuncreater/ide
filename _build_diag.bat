@echo off
call "c:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath > "%TEMP%\vsinstall.txt"
set /p VSINSTALL=<"%TEMP%\vsinstall.txt"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
set "QTDIR=D:\qt\6.10.3\msvc2022_64"
set "PATH=%QTDIR%\bin;%PATH%"
cd /d "c:\Users\v\Desktop\ide\ide"
cmake --build out/build/debug --target minilang_ide --clean-first
