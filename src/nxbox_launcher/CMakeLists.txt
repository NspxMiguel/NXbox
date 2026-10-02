# SPDX-License-Identifier: GPL-3.0-or-later

# nxbox-launcher: the tiny UWP app behind a per-game dashboard tile ("Install as game", see
# docs/nxbox-ui.md). It reads the title ID from title.txt in its package, asks the system to open
# nxbox://play?title=<ID> (which starts NXbox on that game) and exits. One binary serves every
# game; tools/nxbox/package_launcher.py bundles it with a title.txt, a name and art.
#
# Standalone project, not part of Eden's build. Configure from a 'vcvarsall.bat x64 uwp' shell:
#   cmake -S src/nxbox_launcher -B build-launcher -G Ninja -DCMAKE_SYSTEM_NAME=WindowsStore
#         -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.20)
project(NXboxLauncher LANGUAGES CXX)

add_executable(nxbox-launcher WIN32 main.cpp)
target_compile_features(nxbox-launcher PRIVATE cxx_std_20)
target_link_libraries(nxbox-launcher PRIVATE WindowsApp.lib)
if (MSVC)
    target_compile_options(nxbox-launcher PRIVATE /EHsc)
    # AppContainer executable: required for a sideloadable UWP package.
    target_link_options(nxbox-launcher PRIVATE /APPCONTAINER)
endif()
