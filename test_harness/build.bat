@echo off
call "D:\vs\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1

set PROJ=C:\Users\v\Desktop\ide\ide
set OUT=%PROJ%\test_harness

cl /nologo /EHsc /std:c++17 ^
   /I"%OUT%" ^
   /I"%PROJ%" ^
   /Fo"%OUT%\\" ^
   "%OUT%\test_main.cpp" ^
   "%PROJ%\lexer\Lexer.cpp" ^
   "%PROJ%\parser\Parser.cpp" ^
   "%PROJ%\ast\ASTNode.cpp" ^
   "%PROJ%\interpreter\Interpreter.cpp" ^
   "%PROJ%\interpreter\Environment.cpp" ^
   /Fe"%OUT%\test_runner.exe" 2>&1

echo EXIT_CODE=%ERRORLEVEL%
