@echo off
call "D:\vs\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=D:\qt\6.10.3\msvc2022_64\bin;%PATH%
set PROJECT_ROOT=%~dp0..
cd /d "%PROJECT_ROOT%\tests\build"
"C:\Users\v\AppData\Local\Programs\Python\Python310\Scripts\ninja.exe" minilang_tests
if errorlevel 1 (
    echo BUILD_FAILED
    exit /b 1
)
echo BUILD_OK
"minilang_tests.exe" --gtest_brief=1
echo TESTS_EXIT_CODE=%errorlevel%
