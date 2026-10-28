#!/bin/sh
# Builds MojaveMostWanted.dll without a C runtime or Windows SDK.
set -e
cd "$(dirname "$0")"
OUT=${1:-build}
mkdir -p "$OUT"
python3 ../tools/gen.py
llvm-dlltool -m i386 -d lib/kernel32.def -l "$OUT/kernel32.lib" -k
llvm-dlltool -m i386 -d lib/user32.def -l "$OUT/user32.lib" -k
llvm-dlltool -m i386 -d lib/winmm.def -l "$OUT/winmm.lib" -k
llvm-dlltool -m i386 -d lib/ole32.def -l "$OUT/ole32.lib" -k
llvm-dlltool -m i386 -d lib/gdi32.def -l "$OUT/gdi32.lib" -k
llvm-dlltool -m i386 -d lib/ws2_32.def -l "$OUT/ws2_32.lib" -k
CF="--target=i686-pc-windows-msvc -O2 -msse2 -mfpmath=sse -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -mno-stack-arg-probe -fno-builtin -fno-math-errno -Wall -Wno-multichar -Wno-unused-function"
for f in rt game audio radio music screen mirror rider net mmw; do clang $CF -c $f.cpp -o "$OUT/$f.obj"; done
# Minimp3 (CC0) wants <string.h>/<stdlib.h>: shim/ maps them onto rt.cpp
clang $CF -Ishim -Wno-unused-variable -c mp3.cpp -o "$OUT/mp3.obj"
lld-link /dll /nodefaultlib /entry:_DllMainCRTStartup /machine:x86 /safeseh:no /out:"$OUT/MojaveMostWanted.dll" /map:"$OUT/MojaveMostWanted.map" \
  "$OUT/rt.obj" "$OUT/game.obj" "$OUT/audio.obj" "$OUT/radio.obj" "$OUT/music.obj" "$OUT/screen.obj" "$OUT/mirror.obj" "$OUT/rider.obj" "$OUT/net.obj" "$OUT/mp3.obj" "$OUT/mmw.obj" "$OUT/kernel32.lib" "$OUT/user32.lib" "$OUT/winmm.lib" "$OUT/ole32.lib" "$OUT/gdi32.lib" "$OUT/ws2_32.lib"
echo built "$OUT/MojaveMostWanted.dll"
