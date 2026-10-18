#!/bin/bash
# build.sh <Lilu SDK dir (Contents/Resources)> <MacKernelSDK> <out dir> -> <out>/NMHDAVol.kext
set -euo pipefail
cd "$(dirname "$0")"
SDK=$1; MKSDK=$2; OUT=$3
[ -f "$SDK/Headers/plugin_start.hpp" ] || { echo "STOP: no Lilu SDK in $SDK"; exit 1; }
K="$MKSDK/Headers"; [ -d "$K" ] || { echo "STOP: no MacKernelSDK at $MKSDK"; exit 1; }
mkdir -p "$OUT/NMHDAVol.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -I"$K" -I"$SDK"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=NMHDAVol -DMODULE_VERSION=1.0.0
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Wno-unused-parameter
       -Wno-deprecated-declarations -g0)
xcrun clang++ "${FLAGS[@]}" -c nmhdavol.cpp -o "$OUT/nmhdavol.o"
xcrun clang++ "${FLAGS[@]}" -c "$SDK/Library/plugin_start.cpp" -o "$OUT/plugin_start.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -L"$MKSDK/Library/x86_64" -lkmodc++ -lkmod -lcc_kext \
  "$OUT/nmhdavol.o" "$OUT/plugin_start.o" -o "$OUT/NMHDAVol.kext/Contents/MacOS/NMHDAVol"
cp Info.plist "$OUT/NMHDAVol.kext/Contents/Info.plist"
echo "NMHDAVol $(shasum -a 256 "$OUT/NMHDAVol.kext/Contents/MacOS/NMHDAVol" | cut -c1-8) $(stat -f %z "$OUT/NMHDAVol.kext/Contents/MacOS/NMHDAVol") B"
