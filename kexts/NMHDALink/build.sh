#!/bin/bash
# build.sh <MacKernelSDK checkout> <out dir> -> <out>/NMHDALink.kext
set -euo pipefail
cd "$(dirname "$0")"
MKSDK=$1; OUT=$2
K="$MKSDK/Headers"; [ -d "$K" ] || { echo "STOP: no MacKernelSDK at $MKSDK"; exit 1; }
mkdir -p "$OUT/NMHDALink.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -I"$K"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DPRODUCT_NAME=NMHDALink -DMODULE_VERSION=1.0.0
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Wno-unused-parameter -g0)
xcrun clang++ "${FLAGS[@]}" -c nmhdalink.cpp -o "$OUT/nmhdalink.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -L"$MKSDK/Library/x86_64" -lkmodc++ -lkmod -lcc_kext \
  "$OUT/nmhdalink.o" -o "$OUT/NMHDALink.kext/Contents/MacOS/NMHDALink"
cp Info.plist "$OUT/NMHDALink.kext/Contents/Info.plist"
echo "NMHDALink $(shasum -a 256 "$OUT/NMHDALink.kext/Contents/MacOS/NMHDALink" | cut -c1-8) $(stat -f %z "$OUT/NMHDALink.kext/Contents/MacOS/NMHDALink") B"
