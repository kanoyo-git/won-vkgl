#!/bin/bash

set -euo pipefail

mode=${1:-}; hl=${2:-}
[[ -f $hl/hl.exe ]] || { echo "not a Half-Life dir: $hl" >&2; exit 1; }
drvmap=$hl/gldrv/drvmap.txt
marker=$hl/.vkgl-installed

own() { echo "$1" >> "$marker"; sort -u -o "$marker" "$marker"; }

case $mode in
gldrv)
  dll=$3; name=$(basename "$dll"); label=${4:-vkgl ${name%.dll}}
  [[ -f $drvmap.vkgl-backup ]] || cp "$drvmap" "$drvmap.vkgl-backup"
  cp "$dll" "$hl/gldrv/$name"
  own "gldrv/$name"
  grep -q "^gldrv/$name " "$drvmap" ||
    printf 'gldrv/%s %s\r\n' "$name" "$label" >> "$drvmap"
  echo "installed gldrv/$name as \"$label\""
  ;;
opengl32)
  dll=$3
  if [[ -f $hl/opengl32.dll ]] && ! grep -qx opengl32.dll "$marker" 2>/dev/null; then
    echo "$hl/opengl32.dll exists and is not ours; refusing to overwrite" >&2
    exit 1
  fi
  cp "$dll" "$hl/opengl32.dll"
  own opengl32.dll
  echo "installed opengl32.dll; launch with WINEDLLOVERRIDES=opengl32=n,b"
  ;;
uninstall)
  [[ -f $marker ]] || { echo "nothing installed"; exit 0; }
  while read -r f; do rm -f "$hl/$f" "$hl/$(dirname "$f")/vkgl_trace.txt" "$hl/$(dirname "$f")/vkgl.log"; done < "$marker"
  [[ -f $drvmap.vkgl-backup ]] && mv "$drvmap.vkgl-backup" "$drvmap"
  rm -f "$marker"
  echo "uninstalled"
  ;;
*)
  printf '%s\n' 'Usage: tools/install.sh gldrv <Half-Life dir> <driver.dll> [name]' '       tools/install.sh opengl32 <Half-Life dir> <driver.dll>' '       tools/install.sh uninstall <Half-Life dir>' >&2
  exit 1
  ;;
esac
