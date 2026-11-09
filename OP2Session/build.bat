@echo off
REM Build op2session.exe (32-bit) with MSVC. Finds the newest Visual Studio with the C++ tools via
REM vswhere (any edition). Set VSROOT first to use a specific install.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSROOT (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT ( echo ---NO VISUAL STUDIO WITH C++ TOOLS FOUND - set VSROOT--- & exit /b 1 )
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /EHsc /O2 /Fe:op2session.exe /Fo:op2session.obj op2session.cpp ws2_32.lib
echo ---DONE---
