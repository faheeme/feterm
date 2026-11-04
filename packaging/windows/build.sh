#!/bin/bash
# ---------------------------------------------------------------------------
#  Build the Windows release on Linux, in Docker:
#
#      packaging/windows/build.sh [--force]     release build, from the vX.Y.Z tag
#      packaging/windows/build.sh --label NAME  test build of the working tree
#      packaging/windows/build.sh --sums        only rewrite dist/SHA256SUMS
#
#  Cross-compiles feterm.exe and feterm-helper.exe with MinGW-w64 (objects in
#  build-windows/, so a Linux build in build/ is left alone), runs the unit
#  tests under Wine, then writes to dist/:
#
#      feterm-X.Y.Z-windows-x64.zip          portable: unzip and run
#      feterm-X.Y.Z-windows-x64-setup.exe    installer (Inno Setup)
#      SHA256SUMS                            every feterm-X.Y.Z-* file in dist/
#
#  X.Y.Z is VERSION in the Makefile. A release build only runs with HEAD at
#  the tag vX.Y.Z and no uncommitted changes, and won't replace existing
#  release files without --force: those may already be published.
#
#  --label NAME builds whatever is checked out, changes and all, as
#  feterm-X.Y.Z-NAME-windows-x64.{zip,-setup.exe} with SHA256SUMS-X.Y.Z-NAME,
#  and never touches the release files.
#
#  SHA256SUMS covers the macOS DMGs too when they are in dist/. They come
#  later (GitHub builds them from the tag), so after downloading them run
#  --sums to add them before uploading.
#
#  The first run builds the Docker image (packaging/windows/Dockerfile),
#  which takes a few minutes.
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "build: $*" >&2; exit 1; }
usage() { sed -n '5,7s/^#  //p' "$0" >&2; exit 2; }

top=$(cd "$(dirname "$(realpath "$0")")/../.." && pwd)
image=local/feterm-windows
version=$(sed -n 's/^VERSION[[:space:]]*?=[[:space:]]*//p' "$top/Makefile")
[[ -n $version ]] || die "no VERSION in the Makefile"

label="" force=0 sums_only=0
while (($#)); do
    case $1 in
    --label)
        (($# >= 2)) || usage
        label=$2
        shift 2
        ;;
    --force) force=1; shift ;;
    --sums) sums_only=1; shift ;;
    -h|--help) usage ;;
    *) usage ;;
    esac
done
if [[ -n $label && ! $label =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]]; then
    die "--label takes letters, digits, '.' and '-' (e.g. clone-test)"
fi
if ((sums_only)) && [[ $force == 1 || -n $label ]]; then
    die "--sums takes no other option"
fi

# The checksums of one version's release files: Windows and, once
# downloaded, macOS. Never test builds (they carry a label).
write_sums() {
    local files=()
    local f
    for f in "feterm-$version-windows-x64.zip" "feterm-$version-windows-x64-setup.exe" \
             "feterm-$version-macos-arm64.dmg" "feterm-$version-macos-x86_64.dmg"; do
        [[ -f $top/dist/$f ]] && files+=("$f")
    done
    ((${#files[@]})) || die "no feterm-$version release files in dist/"
    (cd "$top/dist" && sha256sum "${files[@]}" > SHA256SUMS)
    echo "== dist/SHA256SUMS:"
    cat "$top/dist/SHA256SUMS"
    [[ -f $top/dist/feterm-$version-macos-arm64.dmg && -f $top/dist/feterm-$version-macos-x86_64.dmg ]] ||
        echo "build: note: no macOS DMGs yet; download them, then run $0 --sums" >&2
}

if ((sums_only)); then
    write_sums
    exit 0
fi

if [[ -z $label ]]; then
    tag=v$version
    git -C "$top" rev-parse -q --verify "refs/tags/$tag" >/dev/null ||
        die "no tag $tag: tag the release first, or use --label NAME for a test build"
    [[ $(git -C "$top" rev-parse HEAD) == $(git -C "$top" rev-parse "$tag^{commit}") ]] ||
        die "HEAD is not $tag: check out the tag, or use --label NAME for a test build"
    [[ -z $(git -C "$top" status --porcelain) ]] ||
        die "uncommitted changes: a release builds the tag as it is (or use --label NAME)"
    name=feterm-$version-windows-x64
    if ((force == 0)); then
        for f in "$name.zip" "$name-setup.exe"; do
            [[ -e $top/dist/$f ]] && die "dist/$f exists (maybe published already); --force replaces it"
        done
    fi
    sums=""
else
    name=feterm-$version-$label-windows-x64
    sums=SHA256SUMS-$version-$label
fi

command -v docker >/dev/null || die "docker is not installed"

echo "== image $image"
docker build -q -t "$image" "$top/packaging/windows" >/dev/null

echo "== building $name"
# Root inside the container (Wine's prefix in the image is root's); everything
# written to the tree is handed back to the caller at the end. The installer's
# version is the Makefile's plus the label, so a test install says what it is.
docker run --rm -v "$top:/src:z" -w /src -e VERSION="$version" -e NAME="$name" \
    -e APPVER="$version${label:+-$label}" -e SUMS="$sums" -e OWNER="$(id -u):$(id -g)" "$image" bash -euo pipefail -c '
    cross=x86_64-w64-mingw32
    trap "chown -R \"\$OWNER\" build-windows dist feterm.exe feterm-helper.exe 2>/dev/null || true" EXIT
    make -j"$(nproc)" BUILD=build-windows CC=$cross-gcc PKG_CONFIG=$cross-pkg-config feterm.exe feterm-helper.exe
    make BUILD=build-windows CC=$cross-gcc PKG_CONFIG=$cross-pkg-config build-windows/test_core.exe
    echo "== tests (Wine)"
    cp /usr/$cross/sys-root/mingw/bin/libpcre2-8-0.dll /usr/$cross/sys-root/mingw/bin/libwinpthread-1.dll build-windows/
    wine build-windows/test_core.exe 2>/dev/null | tail -n 1 | tee /dev/stderr | grep -q " 0 failed"

    stage=build-windows/stage/FeTerm
    packaging/windows/stage.sh "$stage"
    mkdir -p dist
    rm -f "dist/$NAME.zip" "dist/$NAME-setup.exe"
    (cd build-windows/stage && zip -qr "../../dist/$NAME.zip" FeTerm)
    echo "== installer"
    # The .iss names its output feterm-<AppVersion>-windows-x64-setup.exe, which is $NAME-setup.exe.
    (cd packaging/windows && wine "C:\\InnoSetup\\ISCC.exe" /Q "/DAppVersion=$APPVER" \
        "/DStageDir=Z:\\src\\build-windows\\stage\\FeTerm" "/OZ:\\src\\dist" feterm.iss)
    [[ -f "dist/$NAME-setup.exe" ]] || { echo "build: the installer is not dist/$NAME-setup.exe" >&2; exit 1; }
    if [[ -n $SUMS ]]; then
        (cd dist && sha256sum "$NAME.zip" "$NAME-setup.exe" > "$SUMS")
    fi
'
echo "== done:"
(cd "$top/dist" && ls -l "$name.zip" "$name-setup.exe")
if [[ -n $label ]]; then
    cat "$top/dist/$sums"
    echo "== test build: release files and dist/SHA256SUMS untouched"
else
    write_sums
fi
