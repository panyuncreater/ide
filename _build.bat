@echo off
cd /d C:\Users\v\Desktop\ide\ide
D:\vs\MSBuild\Current\Bin\MSBuild.exe ide.vcxproj /p:Configuration=Debug /p:Platform=x64 /nologo /verbosity:minimal > C:\Users\v\Desktop\ide\ide\_build_log.txt 2>&1
