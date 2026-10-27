#!/bin/bash
# ---------------------------------------------------------------------------
#  Turn a macOS build into a self-contained .app bundle:
#
#      packaging/macos/make-app.sh BINARY BUNDLE INFO_PLIST FONTS_CONF
#
#  Called by `make app` from the top of the source tree. The bundle gets the
#  binary, every non-system dylib it loads (copied into Contents/Frameworks
#  and relinked there by dylibbundler, so Homebrew is not needed at run
#  time), an icon built from data/icons, FeTerm's data and a fontconfig
#  configuration. Finally the whole bundle gets an ad-hoc signature: arm64
#  code must be signed to run at all, and relinking broke the linker's own.
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "make-app: $*" >&2; exit 1; }

bin=${1:-}; app=${2:-}; plist=${3:-}; fontsconf=${4:-}
[[ -n $bin && -n $app && -n $plist && -n $fontsconf ]] || die "usage: make-app.sh BINARY BUNDLE INFO_PLIST FONTS_CONF"
[[ -f $bin && -f $plist && -f $fontsconf ]] || die "missing input (build first)"
[[ $app == *.app ]] || die "bundle name must end in .app: $app"
command -v dylibbundler >/dev/null || die "dylibbundler not found (brew install dylibbundler)"

name=$(basename "$bin")
contents="$app/Contents"
rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/data"

cp "$bin" "$contents/MacOS/$name"
cp "$plist" "$contents/Info.plist"
printf 'APPL????' > "$contents/PkgInfo"

cp -R data/themes data/highlight data/config.ini.example "$contents/Resources/data/"
cp "$fontsconf" "$contents/Resources/data/fonts.conf"
cp LICENSE "$contents/Resources/"

# Icon: an iconset from the PNGs we have (no 1024 px source, so no 512@2x).
iconset=$(mktemp -d)/"$name".iconset
trap 'rm -rf "$(dirname "$iconset")"' EXIT
mkdir -p "$iconset"
for pair in 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 256:128x128@2x 256:256x256 \
            512:256x256@2x 512:512x512; do
    px=${pair%%:*}; slot=${pair#*:}
    cp "data/icons/feterm-$px.png" "$iconset/icon_$slot.png"
done
iconutil -c icns "$iconset" -o "$contents/Resources/$name.icns"

# Copy and relink the Homebrew dylibs (-of: overwrite, -b: bundle, -cd: create dir).
dylibbundler -of -b -cd -x "$contents/MacOS/$name" -d "$contents/Frameworks" \
             -p "@executable_path/../Frameworks/" >/dev/null

# Nothing may still point into Homebrew.
if otool -L "$contents/MacOS/$name" "$contents"/Frameworks/*.dylib | grep -E '/(opt/homebrew|usr/local)/' ; then
    die "a library still points outside the bundle"
fi

codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
echo "== built $app ($(du -sh "$app" | cut -f1))"
