#!/bin/sh
# Build a kext from kext/<NAME>/ on Linux: system clang + Apple ld64 (conda-forge
# ld64_osx-64) + acidanthera MacKernelSDK, see kext/README.md.
#   sh kext/build.sh C434TAudio      (default: C434TeMMCIRQ)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:-C434TeMMCIRQ}
TC=$HERE/../toolchain
SDK=$TC/MacKernelSDK
LD=$TC/env/bin/x86_64-apple-darwin13.4.0-ld
SRC=$HERE/$NAME
OUT=$HERE/build/$NAME
KEXT=$OUT/$NAME.kext

CFLAGS="-target x86_64-apple-macos10.13 -mkernel -fapple-kext -nostdinc
  -I$SDK/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -fno-builtin -fno-common -fno-stack-protector -fno-stack-check -O2 -Wall
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -D__APPLE_CC__=6000"

if [ -f "$SRC/gen_fw.py" ] && [ ! -f "$SRC/fw_skl.h" ]; then
  python3 "$SRC/gen_fw.py"
fi

rm -rf "$OUT" && mkdir -p "$KEXT/Contents/MacOS"
clang $CFLAGS -std=gnu11 -c "$SRC/kmod_info.c" -o "$OUT/kmod_info.o"
# Upstream clang registers static destructors with __cxa_atexit (a
# register_call_dtors stub), which the kernel does not export; Apple's
# toolchain puts them in __mod_term_func instead.
clang++ $CFLAGS -std=gnu++17 -nostdinc++ -fno-rtti -fno-exceptions \
  -fno-use-cxa-atexit -fno-register-global-dtors-with-atexit \
  -mllvm --disable-atexit-based-global-dtor-lowering -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -I"$SRC" -c "$SRC/$NAME.cpp" -o "$OUT/$NAME.o"
"$LD" -arch x86_64 -kext -static -macosx_version_min 10.13 \
  -o "$KEXT/Contents/MacOS/$NAME" \
  "$OUT/$NAME.o" "$OUT/kmod_info.o" \
  -L"$SDK/Library/x86_64" -lkmod
cp "$SRC/Info.plist" "$KEXT/Contents/Info.plist"
echo "built $KEXT"
