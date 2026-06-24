$proj = "C:\Users\v\Desktop\ide\ide"
$out = "$proj\test_harness"
$batContent = @"
@echo off
call "D:\vs\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul 2>&1
cl /nologo /EHsc /std:c++17 /I"$out" /I"$proj" /Fo"$out\\" "$out\test_main.cpp" "$proj\lexer\Lexer.cpp" "$proj\parser\Parser.cpp" "$proj\ast\ASTNode.cpp" "$proj\interpreter\Interpreter.cpp" "$proj\interpreter\Environment.cpp" /Fe"$out\test_runner.exe"
echo EXIT_CODE=%ERRORLEVEL%
"@

$tempBat = "$out\build2.bat"
$batContent | Out-File -Encoding ascii $tempBat
$result = & cmd /c "$tempBat 2>&1"
$result | ForEach-Object { Write-Host $_ }
