#!/bin/bash
# ---------------------------------------------------------------------------
#  Build signed Ubuntu source packages for a release tag and upload them to
#  the PPA, one per Ubuntu series. Run it in an Ubuntu container, by hand with
#  your own keyring or from a build server with a key file:
#
#      PPA_GPG_KEY_ID=<fingerprint> packaging/ppa-upload.sh vX.Y.Z
#
#  Each series gets its own version, X.Y.Z-1~ppa1~26.04.1, so every series is
#  built against its own libraries and no two uploads share a version.
#
#  A packaging-only fix (~ppa2) re-uses the version's tarball and takes debian/
#  from the checkout instead of the tag:
#
#      PPA_GPG_KEY_ID=<fingerprint> PPA_ORIG=~/ppa/feterm_X.Y.Z.orig.tar.gz \
#          PPA_DEBIAN_DIR=packaging/debian packaging/ppa-upload.sh vX.Y.Z
#
#  Environment:
#    PPA                  upload target (default ppa:faheeme/feterm)
#    PPA_SERIES           "codename:version ..." (default "resolute:26.04 stonking:26.10")
#    PPA_GPG_KEY          file holding an armored secret key to import into a
#                         throwaway keyring (build server)
#    PPA_GPG_KEY_ID       fingerprint of the signing key; unset = build unsigned
#    PPA_GPG_PASSPHRASE   passphrase for that key, if it has one
#    PPA_DRY_RUN=1        build and sign, but don't upload
#    PPA_WORK             work directory (default ./ppa-out)
#    PPA_ORIG             use this orig tarball instead of downloading the tag's
#    PPA_DEBIAN_DIR       use this debian/ directory instead of the tag's
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "ppa-upload: $*" >&2; exit 1; }

tag=${1:-}
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: $0 vX.Y.Z (got '${tag}')"
version=${tag#v}

ppa=${PPA:-ppa:faheeme/feterm}
series_list=${PPA_SERIES:-resolute:26.04 stonking:26.10}
key_id=${PPA_GPG_KEY_ID:-}
dry_run=${PPA_DRY_RUN:-}
work=$(realpath -m "${PPA_WORK:-ppa-out}")
src_url=${PPA_SRC_URL:-https://gitlab.com/faheeme/feterm/-/archive/$tag/feterm-$tag.tar.gz}
orig_in=${PPA_ORIG:+$(realpath -e "$PPA_ORIG")}
debian_in=${PPA_DEBIAN_DIR:+$(realpath -e "$PPA_DEBIAN_DIR")}

[[ $ppa =~ ^ppa:([^/]+)/([^/]+)$ ]] || die "PPA must look like ppa:owner/name"
lp_api="https://api.launchpad.net/devel/~${BASH_REMATCH[1]}/+archive/ubuntu/${BASH_REMATCH[2]}"

# A key file goes into a throwaway keyring. With a passphrase, gpg reads it from a
# file instead of asking, so debsign can sign without a terminal.
if [[ -n ${PPA_GPG_KEY:-} ]]; then
    [[ -n $key_id ]] || die "PPA_GPG_KEY is set but PPA_GPG_KEY_ID is not"
    GNUPGHOME=$(mktemp -d)
    export GNUPGHOME
    trap 'rm -rf "$GNUPGHOME"' EXIT
    gpg --batch --quiet --import "$PPA_GPG_KEY"
    if [[ -n ${PPA_GPG_PASSPHRASE:-} ]]; then
        printf '%s' "$PPA_GPG_PASSPHRASE" > "$GNUPGHOME/passphrase"
        printf 'batch\npinentry-mode loopback\npassphrase-file %s\n' "$GNUPGHOME/passphrase" > "$GNUPGHOME/gpg.conf"
    fi
fi
if [[ -z $key_id && -z $dry_run ]]; then
    die "PPA_GPG_KEY_ID is not set; Launchpad only accepts signed uploads (PPA_DRY_RUN=1 builds unsigned)"
fi

mkdir -p "$work/out"
cd "$work"

# One tarball for every series: Launchpad stores a version's orig tarball once,
# so all uploads of X.Y.Z must carry identical bytes.
orig=feterm_${version}.orig.tar.gz
if [[ -n $orig_in ]]; then
    [[ $orig_in -ef $orig ]] || cp "$orig_in" "$orig"
else
    curl -fsSL -o "$orig" "$src_url" || die "can't download $src_url"
fi

for entry in $series_list; do
    series=${entry%%:*}
    ubuntu=${entry#*:}
    [[ -n $series && -n $ubuntu && $series != "$entry" ]] || die "bad PPA_SERIES entry '$entry'"

    build=$work/$series
    rm -rf "$build"
    mkdir -p "$build"
    cp "$orig" "$build/"            # dpkg-source wants it next to the source tree
    tar xf "$orig" -C "$build"
    dir=$build/feterm-$tag
    if [[ -n $debian_in ]]; then
        cp -r "$debian_in" "$dir/debian"
    else
        [[ -d $dir/packaging/debian ]] || die "$tag has no packaging/debian"
        cp -r "$dir/packaging/debian" "$dir/debian"
    fi

    base=$(dpkg-parsechangelog -l "$dir/debian/changelog" -S Version)
    [[ ${base%-*} == "$version" ]] ||
        die "debian/changelog starts at $base, not $version-1~ppaN; add the entry before tagging"
    pkgver=${base}~${ubuntu}.1

    if curl -fsS "$lp_api?ws.op=getPublishedSources&source_name=feterm&exact_match=true&version=$pkgver" |
        grep -q '"total_size": [1-9]'; then
        echo "== $pkgver is already in $ppa, skipping $series"
        continue
    fi

    # Retarget the top changelog entry to this series instead of adding one: a
    # "~26.04.1" entry would sort below the entry it sits on.
    sed -i "1s/^feterm ([^)]*) [^;]*;/feterm ($pkgver) $series;/" "$dir/debian/changelog"
    [[ $(dpkg-parsechangelog -l "$dir/debian/changelog" -S Version) == "$pkgver" ]] ||
        die "couldn't set the changelog version to $pkgver"

    echo "== building $pkgver for $series"
    (cd "$dir" && dpkg-buildpackage -S -sa -d -us -uc)
    changes=$build/feterm_${pkgver}_source.changes

    if [[ -n $key_id ]]; then
        debsign -k"$key_id" "$changes"
    fi
    cp "$build"/feterm_"${pkgver}"* "$work/out/"
    cp "$orig" "$work/out/"

    if [[ -n $dry_run ]]; then
        echo "== dry run: not uploading $changes"
    else
        dput "$ppa" "$changes"
    fi
done
