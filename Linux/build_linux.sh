#!/bin/sh
# Builds the DesktopTV Linux parts:
#   build/DesktopTV.addon64  the add-on, with MinGW-w64 (pacman -S mingw-w64-gcc), from the same source as
#                            Source/build.bat (../Source)
#   build/desktoptv_pw       the picture program of the helper (needs pipewire headers: pacman -S pipewire)
set -e
cd "$(dirname "$0")"
mkdir -p build
SRC=../Source
x86_64-w64-mingw32-g++ -shared -O2 -std=c++17 -include mingw_include/prelude.h -Wno-attributes -Wno-unknown-pragmas \
	-I mingw_include -I $SRC/reshade_include \
	$SRC/src/desktoptv_addon.cpp \
	-o build/DesktopTV.addon64 \
	-s -static -static-libgcc -static-libstdc++ -ld3d11 -ldxgi -ld3dcompiler -luser32 -lole32 -lws2_32
echo "BUILD OK: $(pwd)/build/DesktopTV.addon64"
g++ -O2 -std=c++17 desktoptv_pw.cpp -o build/desktoptv_pw $(pkg-config --cflags --libs libpipewire-0.3) -pthread
echo "BUILD OK: $(pwd)/build/desktoptv_pw"
