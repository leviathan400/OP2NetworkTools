@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
cd /d "%~dp0"
cl /nologo /EHsc /O2 /Fe:op2session.exe /Fo:op2session.obj op2session.cpp ws2_32.lib
echo ---DONE---
