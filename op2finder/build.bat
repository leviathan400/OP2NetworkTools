@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
cd /d "%~dp0"
cl /nologo /O2 /Fe:op2finder.exe /Fo:op2finder.obj op2finder.c ws2_32.lib
echo ---DONE---
