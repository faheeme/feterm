#!/bin/bash
# ---------------------------------------------------------------------------
#  Collect a Windows build into one folder that runs as is:
#
#      packaging/windows/stage.sh OUTDIR
#
#  Run from the top of the source tree after a Windows build (feterm.exe and
#  feterm-helper.exe exist). OUTDIR gets the two programs, every DLL they
#  need from the MinGW sys-root (found by following their imports, so the
#  list never goes stale), ANGLE's libEGL.dll (loaded at run time, not
#  imported), fontconfig's configuration and FeTerm's data. Everything is
#  stripped. Called by build.sh.
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "stage: $*" >&2; exit 1; }

out=${1:-}
[[ -n $out ]] || die "usage: stage.sh OUTDIR"
cross=${CROSS:-x86_64-w64-mingw32}
root=${MINGW_ROOT:-/usr/$cross/sys-root/mingw}
[[ -f feterm.exe && -f feterm-helper.exe ]] || die "build first (feterm.exe, feterm-helper.exe)"
[[ -d $root/bin ]] || die "no MinGW sys-root at $root"

rm -rf "$out"
mkdir -p "$out"
cp feterm.exe feterm-helper.exe "$out/"
cp "$root/bin/libEGL.dll" "$out/"

# Follow imports breadth-first; DLLs not in the sys-root are Windows' own.
todo=("$out/feterm.exe" "$out/feterm-helper.exe" "$out/libEGL.dll")
while ((${#todo[@]})); do
    f=${todo[0]}
    todo=("${todo[@]:1}")
    while read -r dll; do
        [[ -e "$out/$dll" ]] && continue
        src=$(find "$root/bin" -maxdepth 1 -iname "$dll" -print -quit)
        [[ -n $src ]] || continue
        cp "$src" "$out/"
        todo+=("$out/$(basename "$src")")
    done < <("$cross-objdump" -p "$f" | sed -n 's/^\s*DLL Name: //p')
done

mkdir -p "$out/data" "$out/etc"
cp -r data/themes data/highlight data/config.ini.example "$out/data/"
cp -r "$root/etc/fonts" "$out/etc/"
cp LICENSE "$out/LICENSE.txt"
"$cross-strip" --strip-unneeded "$out"/*.exe "$out"/*.dll
echo "== staged $(find "$out" -type f | wc -l) files in $out ($(du -sh "$out" | cut -f1))"
