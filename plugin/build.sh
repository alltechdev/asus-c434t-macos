#!/bin/sh
# Build and install the CoreAudio HAL plug-in. Runs on the Mac (needs the
# Command Line Tools): sh plugin/build.sh [--install]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build/C434TAudio.driver
rm -rf "$OUT" && mkdir -p "$OUT/Contents/MacOS"
clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -bundle \
  -framework CoreAudio -framework CoreFoundation -framework IOKit \
  -o "$OUT/Contents/MacOS/C434TAudio" "$HERE/C434TAudio.c"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
echo "built $OUT"
if [ "${1:-}" = "--install" ]; then
  sudo rm -rf /Library/Audio/Plug-Ins/HAL/C434TAudio.driver
  sudo cp -R "$OUT" /Library/Audio/Plug-Ins/HAL/
  sudo killall coreaudiod || true
  echo "installed; coreaudiod restarted"
fi
