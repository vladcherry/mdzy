#!/bin/sh
# Build mdzy.exe with mingw-w64 (Linux cross-compile or MSYS2 MinGW64 shell).
# Usage: ./build.sh [dev]
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
command -v "$CC" >/dev/null 2>&1 || CC=gcc
command -v "$WINDRES" >/dev/null 2>&1 || WINDRES=windres
DEFS=-DNDEBUG
[ "$1" = "dev" ] && DEFS=-DMDZY_DEV
mkdir -p build
"$WINDRES" -I src -O coff -o build/mdzy_res.o src/mdzy.rc
"$CC" -std=c11 -Os -municode -mwindows -s $DEFS -o build/mdzy.exe src/mdzy.c build/mdzy_res.o \
    -luser32 -lgdi32 -lshell32 -lshlwapi -lcomdlg32 -lole32 -ladvapi32 -ldwmapi -luxtheme \
    -lwindowscodecs -luuid -static
echo "Built build/mdzy.exe"
