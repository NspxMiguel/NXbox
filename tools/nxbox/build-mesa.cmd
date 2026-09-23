@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "NXBOX_VSROOT=%%i"
if not defined NXBOX_VSROOT exit /b 1
call "%NXBOX_VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
set "NXBOX_ROOT=%CD%"
set "PATH=C:\ProgramData\chocolatey\bin;C:\ProgramData\chocolatey\lib\winflexbison3\tools;%PATH%"
cd .cache\mesa
python ..\meson\meson.py setup build-nxbox --backend=vs2022 --uwp --buildtype=release --prefix="%NXBOX_ROOT%\mesa-install" -Dcpp_std=vc++17 -Dcpp_args="['/D_XBOX_UWP']" -Dc_args="['/D_XBOX_UWP']" -Db_pch=false -Dc_winlibs=WindowsApp.lib -Dcpp_winlibs=WindowsApp.lib -Dgallium-drivers=d3d12 -Dvulkan-drivers=[] -Dllvm=disabled -Ddraw-use-llvm=false -Dgallium-d3d12-video=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dbuild-tests=false
if errorlevel 1 exit /b 1
python ..\meson\meson.py compile -C build-nxbox -j 3
if errorlevel 1 exit /b 1
python ..\meson\meson.py install -C build-nxbox --no-rebuild
