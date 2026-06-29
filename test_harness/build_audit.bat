@echo off
call "D:\vs\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
chcp 65001 >nul 2>&1
set PROJ=C:\Users\v\Desktop\ide\ide
set OUT=%PROJ%\test_harness

cl /nologo /EHsc /std:c++17 /utf-8 ^
   /I"%OUT%" ^
   /I"%PROJ%" ^
   /I"%PROJ%\common" ^
   /I"%PROJ%\lexer" ^
   /I"%PROJ%\parser" ^
   /I"%PROJ%\ast" ^
   /I"%PROJ%\interpreter" ^
   /I"%PROJ%\formatter" ^
   /Fo"%OUT%\\" ^
   /Fe"%OUT%\formatter_audit.exe" ^
   "%OUT%\formatter_audit.cpp" ^
   "%PROJ%\lexer\Lexer.cpp" ^
   "%PROJ%\parser\Parser.cpp" ^
   "%PROJ%\ast\ASTNode.cpp" ^
   "%PROJ%\formatter\Formatter.cpp" ^
   "%PROJ%\interpreter\Value.cpp" ^
   "%PROJ%\interpreter\GcManager.cpp" > "%OUT%\audit_build.log" 2>&1

echo EXIT_CODE=%ERRORLEVEL%
type "%OUT%\audit_build.log" | findstr /C:"error" /C:"EXIT_CODE" | findstr /V "warning"
