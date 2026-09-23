@echo off
setlocal
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -property installationPath`) do set "NXBOX_VSROOT=%%i"
if not defined NXBOX_VSROOT exit /b 1
call "%NXBOX_VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp
if errorlevel 1 exit /b 1
if /i not "%VSCMD_ARG_app_plat%"=="UWP" (
  echo ERROR: UWP compiler environment was not selected.
  exit /b 1
)
set "PATH=C:\Strawberry\perl\bin;C:\Program Files\NASM;%NXBOX_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where perl
where nasm
if not exist build-uwp mkdir build-uwp
cmake --preset uwp-x64 -DYUZU_USE_BUNDLED_SIRIT=OFF > build-uwp\configure.log 2>&1
if errorlevel 1 (
  type build-uwp\configure.log
  exit /b 1
)
cmake --build --preset uwp-x64 --target eden-uwp --parallel 3 > build-uwp\build.log 2>&1
if errorlevel 1 (
  type build-uwp\build.log
  exit /b 1
)
type build-uwp\build.log
