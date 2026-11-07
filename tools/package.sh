#!/bin/bash

set -euo pipefail
cd "$(dirname "$0")/.."
ninja -C build vkgl >/dev/null
out=dist/vkgl-hl1-won
rm -rf "$out" "$out.zip"
mkdir -p "$out/vkgl"
crlf() { sed 's/\r*$/\r/' "$1" > "$2"; }
cp build/vkgl.dll "$out/vkgl/opengl32.dll"
crlf tools/vkgl.ini "$out/vkgl/vkgl.ini"
crlf package/launch.bat "$out/4. Half-Life (WON) + BXT + vkgl.bat"
crlf package/uninstall.bat "$out/vkgl/uninstall.bat"
crlf package/README.txt "$out/vkgl/README.txt"
i686-w64-mingw32-strip "$out/vkgl/opengl32.dll"
(cd "$out" && python3 -m zipfile -c ../vkgl-hl1-won.zip *)
echo "$out ($(git describe --always --dirty))"
ls -la "$out" "$out/vkgl" "$out.zip"
