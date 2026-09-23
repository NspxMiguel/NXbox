@echo off
setlocal
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -property installationPath`) do set "NXBOX_VSROOT=%%i"
if not defined NXBOX_VSROOT exit /b 1
call "%NXBOX_VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp
if errorlevel 1 exit /b 1
set "PATH=%NXBOX_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S tests/xbox-graphics -B build-graphics -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build-graphics --parallel 2
exit /b %errorlevel%
