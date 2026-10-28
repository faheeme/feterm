#!/bin/sh
# Runs inside a throwaway alpine container started by build-repo.sh; not for
# use on its own. Mounts (read-only except /dest and /out):
#   /in/APKBUILD, /in/*.patch   the package recipe
#   /keys/NAME.rsa[.pub]        signing key ("build"; only the .pub for "smoke")
#   /dest                       REPODEST: feterm/<arch>/ with the older packages
#   /out                        "checksum" writes the updated APKBUILD here
# Environment: KEYNAME, HOST_UID and HOST_GID (owner of the files written back).
set -eu

die() {
    printf 'container: %s\n' "$*" >&2
    exit 1
}

mode=${1:-}
case $mode in
build | checksum | smoke) ;;
*) die "usage: container.sh build|checksum|smoke" ;;
esac

uid=${HOST_UID:-}
gid=${HOST_GID:-}
case $uid in '' | *[!0-9]*) die "HOST_UID must be a number" ;; esac
case $gid in '' | *[!0-9]*) die "HOST_GID must be a number" ;; esac
if [ "$mode" != checksum ]; then
    case ${KEYNAME:-} in
    '' | *[!A-Za-z0-9._-]*) die "bad KEYNAME" ;;
    esac
fi

if [ "$mode" = smoke ]; then
    # A clean system that trusts Alpine's keys plus ours: install from the new
    # repo (apk checks the index and package signatures), verify, run.
    cp "/keys/$KEYNAME.rsa.pub" /etc/apk/keys/
    apk add -q --repository /dest/feterm feterm feterm-doc
    apk verify -q /dest/feterm/*/feterm-*.apk
    feterm --version
    exit 0
fi

apk add -q alpine-sdk
adduser -D -u 1500 builder
addgroup builder abuild
work=/home/builder/aports/feterm/feterm
mkdir -p "$work"
cp /in/APKBUILD /in/*.patch "$work"/

if [ "$mode" = checksum ]; then
    chown -R builder:builder /home/builder
    su builder -c "cd '$work' && abuild checksum"
    install -m 644 -o "$uid" -g "$gid" "$work/APKBUILD" /out/APKBUILD
    exit 0
fi

# build: sign with the mounted key, as the unprivileged builder user.
mkdir -p /home/builder/.abuild
install -m 600 "/keys/$KEYNAME.rsa" /home/builder/.abuild/
install -m 644 "/keys/$KEYNAME.rsa.pub" /home/builder/.abuild/
cp "/keys/$KEYNAME.rsa.pub" /etc/apk/keys/
printf 'PACKAGER_PRIVKEY=/home/builder/.abuild/%s.rsa\n' "$KEYNAME" >/home/builder/.abuild/abuild.conf
chown -R builder:builder /home/builder /dest

# abuild -r installs the build dependencies (through the abuild-apk helper the
# abuild group may use), builds, runs check(), signs the packages, and
# re-indexes and signs APKINDEX.tar.gz over everything in /dest/feterm/<arch>.
su builder -c "cd '$work' && REPODEST=/dest abuild -r"

apk verify -q /dest/feterm/*/feterm-*.apk
# Hand the results back to the invoking user; the key copy dies with the container.
rm -f "/home/builder/.abuild/$KEYNAME.rsa"
chown -R "$uid:$gid" /dest
