#!/bin/sh
# Build FeTerm's Alpine packages and the signed apk repository that GitLab
# Pages publishes (https://faheeme.gitlab.io/feterm-apk/). Everything is built
# in throwaway alpine containers; the private key never leaves this machine.
#
#   build-repo.sh keygen DIR             make a signing key pair in DIR (once)
#   build-repo.sh binfmt                 let Docker run aarch64 containers (after each boot)
#   build-repo.sh checksum               refresh sha512sums in the APKBUILD (each version)
#   build-repo.sh build --repo DIR --key FILE [--release R]... [--arch A]...
#
# "build" builds, tests, signs and smoke-tests every release x arch in a
# temporary copy, and only when all of them pass copies the result into the
# feterm-apk checkout. It never commits or pushes: review, then push yourself.
set -eu
umask 022

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
RELEASES_ALL="v3.23 v3.24 edge"
ARCHES_ALL="x86_64 aarch64"
KEEP_OLD=1            # older versions kept next to the new one, per arch
MIN_FREE_KB=$((4 * 1024 * 1024))
BINFMT_IMAGE="tonistiigi/binfmt:qemu-v10.2.3-68@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0"
HOST_UID=$(id -u)
HOST_GID=$(id -g)

die() {
    printf 'build-repo.sh: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '== %s\n' "$*"
}

in_list() { # in_list WORD "LIST"
    case " $2 " in *" $1 "*) return 0 ;; esac
    return 1
}

image_for() {
    case $1 in
    v3.23) echo alpine:3.23 ;;
    v3.24) echo alpine:3.24 ;;
    edge) echo alpine:edge ;;
    *) die "no image for release $1" ;;
    esac
}

platform_for() {
    case $1 in
    x86_64) echo linux/amd64 ;;
    aarch64) echo linux/arm64 ;;
    *) die "no platform for arch $1" ;;
    esac
}

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is needed"
}

free_kb() {
    df -Pk "$1" | awk 'NR == 2 { print $4 }'
}

apkbuild_field() { # the value of NAME=... in the APKBUILD, validated by the caller
    sed -n "s/^$1=//p" "$HERE/APKBUILD" | head -n 1
}

# ---- keygen ------------------------------------------------------------------
cmd_keygen() {
    [ $# -eq 1 ] || die "usage: build-repo.sh keygen DIR"
    need openssl
    dir=$1
    name=feterm-$(date -u +%Y%m%d)
    mkdir -p "$dir"
    chmod 700 "$dir"
    for f in "$dir"/*.rsa; do
        [ -e "$f" ] && die "$dir already holds a key ($f); refusing to make another"
    done
    (
        umask 077
        openssl genrsa -out "$dir/$name.rsa" 4096 2>/dev/null
    ) || die "openssl genrsa failed"
    openssl rsa -in "$dir/$name.rsa" -pubout -out "$dir/$name.rsa.pub" 2>/dev/null ||
        die "openssl rsa -pubout failed"
    chmod 600 "$dir/$name.rsa"
    chmod 644 "$dir/$name.rsa.pub"
    note "private key: $dir/$name.rsa (keep secret, back it up offline)"
    note "public key:  $dir/$name.rsa.pub (copy into feterm-apk/keys/)"
    note "fingerprint: $(sha256sum "$dir/$name.rsa.pub" | cut -d' ' -f1)"
}

# ---- binfmt ------------------------------------------------------------------
cmd_binfmt() {
    need docker
    if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        note "aarch64 emulation is already registered"
        return 0
    fi
    note "registering aarch64 emulation (privileged, one-off; lasts until reboot)"
    docker run --privileged --rm "$BINFMT_IMAGE" --install arm64
    [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || die "registration failed"
}

# ---- checksum ----------------------------------------------------------------
cmd_checksum() {
    need docker
    docker run --rm -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
        -v "$HERE:/in:ro,Z" -v "$HERE:/out:Z" alpine:3.24 sh /in/container.sh checksum
    note "updated sha512sums in $HERE/APKBUILD; review with git diff"
}

# ---- build -------------------------------------------------------------------
cmd_build() {
    need docker
    need openssl
    need rsync
    need sha256sum
    repo=''
    key=''
    releases=''
    arches=''
    while [ $# -gt 0 ]; do
        case $1 in
        --repo) [ $# -ge 2 ] || die "--repo needs a value"; repo=$2; shift 2 ;;
        --key) [ $# -ge 2 ] || die "--key needs a value"; key=$2; shift 2 ;;
        --release)
            [ $# -ge 2 ] || die "--release needs a value"
            in_list "$2" "$RELEASES_ALL" || die "unknown release $2 (choose from: $RELEASES_ALL)"
            releases="$releases $2"; shift 2 ;;
        --arch)
            [ $# -ge 2 ] || die "--arch needs a value"
            in_list "$2" "$ARCHES_ALL" || die "unknown arch $2 (choose from: $ARCHES_ALL)"
            arches="$arches $2"; shift 2 ;;
        *) die "unknown option $1" ;;
        esac
    done
    if [ -z "$repo" ] || [ -z "$key" ]; then
        die "usage: build-repo.sh build --repo DIR --key FILE [--release R]... [--arch A]..."
    fi
    [ -n "$releases" ] || releases=$RELEASES_ALL
    [ -n "$arches" ] || arches=$ARCHES_ALL

    # The package being built.
    pkgver=$(apkbuild_field pkgver)
    pkgrel=$(apkbuild_field pkgrel)
    printf '%s\n' "$pkgver" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || die "bad pkgver in APKBUILD: $pkgver"
    printf '%s\n' "$pkgrel" | grep -Eqx '[0-9]+' || die "bad pkgrel in APKBUILD: $pkgrel"
    current=$pkgver-r$pkgrel
    grep -Eq "^[0-9a-f]{128}  $(printf '%s' "feterm-$pkgver.tar.gz")\$" "$HERE/APKBUILD" ||
        die "APKBUILD has no checksum for feterm-$pkgver.tar.gz: run build-repo.sh checksum"

    # The repository checkout.
    [ -d "$repo/.git" ] || die "$repo is not a git checkout of feterm-apk"
    repo=$(cd "$repo" && pwd -P)

    # The key: a private file of ours, outside the repo, matching the published public key.
    if [ ! -f "$key" ] || [ -L "$key" ]; then
        die "$key is not a regular file"
    fi
    case $key in *.rsa) ;; *) die "the key file must be named NAME.rsa" ;; esac
    key=$(cd "$(dirname "$key")" && pwd -P)/$(basename "$key")
    case $key in "$repo"/*) die "the private key must not live inside the repository" ;; esac
    [ "$(stat -c %a "$key")" = 600 ] || die "$key must have mode 600 (chmod 600 $key)"
    keyname=$(basename "$key" .rsa)
    printf '%s\n' "$keyname" | grep -Eqx '[A-Za-z0-9._-]+' || die "bad key name: $keyname"
    pub=$repo/keys/$keyname.rsa.pub
    [ -f "$pub" ] || die "$pub is missing: copy the public key into the repo's keys/ folder"
    openssl rsa -in "$key" -pubout 2>/dev/null | cmp -s - "$pub" ||
        die "$key does not match $pub"

    # Room to work, and aarch64 emulation if needed.
    root=$(docker info -f '{{.DockerRootDir}}' 2>/dev/null || echo /var/lib/docker)
    for d in "${TMPDIR:-/tmp}" "$root" "$repo"; do
        [ -d "$d" ] || continue
        [ "$(free_kb "$d")" -ge "$MIN_FREE_KB" ] || die "less than 4 GB free on $d"
    done
    if in_list aarch64 "$arches" && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        die "aarch64 emulation is not registered: run build-repo.sh binfmt"
    fi

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 130' INT TERM
    # Private copy of the key for the containers (and the SELinux relabel);
    # removed with $tmp however the script ends.
    mkdir -m 700 "$tmp/keys" "$tmp/pubkeys"
    cp "$key" "$tmp/keys/$keyname.rsa"
    chmod 600 "$tmp/keys/$keyname.rsa"
    cp "$pub" "$tmp/keys/$keyname.rsa.pub"
    cp "$pub" "$tmp/pubkeys/$keyname.rsa.pub"

    for rel in $releases; do
        image=$(image_for "$rel")
        for arch in $arches; do
            platform=$(platform_for "$arch")
            dest=$tmp/$rel
            mkdir -p "$dest/feterm/$arch"
            # Carry over the newest older version, so users can still downgrade.
            old=$repo/$rel/feterm/$arch
            if [ -d "$old" ]; then
                for v in $(find "$old" -maxdepth 1 -name 'feterm-[0-9]*.apk' -printf '%f\n' |
                    sed -n 's/^feterm-\([0-9][0-9.]*-r[0-9][0-9]*\)\.apk$/\1/p' |
                    grep -vx "$current" | sort -V | tail -n "$KEEP_OLD"); do
                    for f in "feterm-$v.apk" "feterm-doc-$v.apk"; do
                        [ -f "$old/$f" ] && cp "$old/$f" "$dest/feterm/$arch/"
                    done
                done
            fi

            note "$rel $arch: build, test and sign feterm $current"
            docker run --rm --platform "$platform" \
                -e KEYNAME="$keyname" -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
                -v "$HERE:/in:ro,Z" -v "$tmp/keys:/keys:ro,Z" -v "$dest:/dest:Z" \
                "$image" sh /in/container.sh build ||
                die "$rel $arch: build failed; nothing was copied into $repo"

            note "$rel $arch: smoke test (install from the new repo, verify, run)"
            docker run --rm --platform "$platform" \
                -e KEYNAME="$keyname" -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
                -v "$HERE:/in:ro,Z" -v "$tmp/pubkeys:/keys:ro,Z" -v "$dest:/dest:ro,Z" \
                "$image" sh /in/container.sh smoke ||
                die "$rel $arch: smoke test failed; nothing was copied into $repo"
        done
    done

    # Everything passed: publish into the checkout (old files of each built
    # release/arch are replaced, others are left alone).
    rm -rf "$tmp/keys"
    for rel in $releases; do
        for arch in $arches; do
            mkdir -p "$repo/$rel/feterm/$arch"
            rsync -a --delete "$tmp/$rel/feterm/$arch/" "$repo/$rel/feterm/$arch/"
        done
    done
    note "done; key fingerprint $(sha256sum "$pub" | cut -d' ' -f1)"
    git -C "$repo" status --short
    note "review the changes above, then: git -C $repo add -A && git -C $repo commit && git -C $repo push"
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
keygen) cmd_keygen "$@" ;;
binfmt) cmd_binfmt "$@" ;;
checksum) cmd_checksum "$@" ;;
build) cmd_build "$@" ;;
*) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
