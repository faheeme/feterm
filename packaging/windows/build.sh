#!/bin/bash
# ---------------------------------------------------------------------------
#  Build the Windows release on Linux, in Docker:
#
#      packaging/windows/build.sh
#
#  Cross-compiles feterm.exe and feterm-helper.exe with MinGW-w64 (objects in
#  build-windows/, so a Linux build in build/ is left alone), runs the unit
#  tests under Wine, then writes to dist/:
#
#      feterm-X.Y.Z-windows-x64.zip          portable: unzip and run
#      feterm-X.Y.Z-windows-x64-setup.exe    installer (Inno Setup)
#      SHA256SUMS
#
#  X.Y.Z is VERSION in the Makefile. The first run builds the Docker image
#  (packaging/windows/Dockerfile), which takes a few minutes.
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "build: $*" >&2; exit 1; }

top=$(cd "$(dirname "$(realpath "$0")")/../.." && pwd)
image=local/feterm-windows
version=$(sed -n 's/^VERSION[[:space:]]*?=[[:space:]]*//p' "$top/Makefile")
[[ -n $version ]] || die "no VERSION in the Makefile"
command -v docker >/dev/null || die "docker is not installed"

echo "== image $image"
docker build -q -t "$image" "$top/packaging/windows" >/dev/null

echo "== building $version"
# Root inside the container (Wine's prefix in the image is root's); everything
# written to the tree is handed back to the caller at the end.
docker run --rm -v "$top:/src:z" -w /src -e VERSION="$version" -e OWNER="$(id -u):$(id -g)" "$image" bash -euo pipefail -c '
    cross=x86_64-w64-mingw32
    trap "chown -R \"\$OWNER\" build-windows dist feterm.exe feterm-helper.exe 2>/dev/null || true" EXIT
    make -j"$(nproc)" BUILD=build-windows CC=$cross-gcc PKG_CONFIG=$cross-pkg-config feterm.exe feterm-helper.exe
    make BUILD=build-windows CC=$cross-gcc PKG_CONFIG=$cross-pkg-config build-windows/test_core.exe
    echo "== tests (Wine)"
    cp /usr/$cross/sys-root/mingw/bin/libpcre2-8-0.dll /usr/$cross/sys-root/mingw/bin/libwinpthread-1.dll build-windows/
    wine build-windows/test_core.exe 2>/dev/null | tail -n 1 | tee /dev/stderr | grep -q " 0 failed"

    name=feterm-$VERSION-windows-x64
    stage=build-windows/stage/FeTerm
    packaging/windows/stage.sh "$stage"
    mkdir -p dist
    rm -f "dist/$name.zip" "dist/$name-setup.exe"
    (cd build-windows/stage && zip -qr "../../dist/$name.zip" FeTerm)
    echo "== installer"
    (cd packaging/windows && wine "C:\\InnoSetup\\ISCC.exe" /Q "/DAppVersion=$VERSION" \
        "/DStageDir=Z:\\src\\build-windows\\stage\\FeTerm" "/OZ:\\src\\dist" feterm.iss)
    (cd dist && sha256sum "$name.zip" "$name-setup.exe" > SHA256SUMS)
'
echo "== done:"
(cd "$top/dist" && ls -l "feterm-$version-windows-x64.zip" "feterm-$version-windows-x64-setup.exe" && cat SHA256SUMS)
