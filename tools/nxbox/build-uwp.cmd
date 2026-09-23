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
set "NXBOX_CACHE_ARGS="
if defined SCCACHE_PATH set "NXBOX_CACHE_ARGS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
python tools\nxbox\run_logged.py build-uwp\configure.log cmake --preset uwp-x64 -DYUZU_USE_BUNDLED_SIRIT=OFF %NXBOX_CACHE_ARGS%
if errorlevel 1 (
  type build-uwp\configure.log
  exit /b 1
)
python tools\nxbox\run_logged.py build-uwp\frontend.log cmake --build --preset uwp-x64 --target src/eden_uwp/CMakeFiles/eden-uwp.dir/game_session.cpp.obj src/eden_uwp/CMakeFiles/eden-uwp.dir/mesa_window.cpp.obj src/eden_uwp/CMakeFiles/eden-uwp.dir/uwp_boot.cpp.obj --parallel 3
if errorlevel 1 exit /b 1
if /i "%~1"=="frontend" exit /b 0
python tools\nxbox\run_logged.py build-uwp\build.log cmake --build --preset uwp-x64 --target eden-uwp --parallel 3
if errorlevel 1 (
  exit /b 1
)
