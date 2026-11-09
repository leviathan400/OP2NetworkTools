@echo off
REM Build OP2SessionLogger on Windows using the Visual Studio toolchain + its bundled CMake/Ninja.
REM Finds the newest Visual Studio with the C++ tools via vswhere (any edition: Community,
REM Professional, Insiders, BuildTools...). Set VSROOT first to use a specific install.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSROOT (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT ( echo ---NO VISUAL STUDIO WITH C++ TOOLS FOUND - set VSROOT--- & exit /b 1 )
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( echo ---CONFIG FAILED--- & exit /b 1 )
cmake --build build
if errorlevel 1 ( echo ---BUILD FAILED--- & exit /b 1 )
echo ---BUILD OK---
