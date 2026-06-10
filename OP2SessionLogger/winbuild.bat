@echo off
REM Build OP2SessionLogger on Windows using the Visual Studio toolchain + bundled CMake/Ninja.
setlocal
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( echo ---CONFIG FAILED--- & exit /b 1 )
cmake --build build
if errorlevel 1 ( echo ---BUILD FAILED--- & exit /b 1 )
echo ---BUILD OK---
