#!/bin/bash
# ---------------------------------------------------------------------------
#  Write the winget manifests for a release:
#
#      packaging/windows/winget.sh INSTALLER_URL
#
#  INSTALLER_URL is where the release's feterm-X.Y.Z-windows-x64-setup.exe
#  can be downloaded (the link on the GitLab release). The file must also be
#  in dist/ (from build.sh): its SHA-256 is checked against the download.
#  Writes dist/winget/manifests/f/FaheemEssack/FeTerm/X.Y.Z/, ready for a
#  pull request to github.com/microsoft/winget-pkgs (see packaging/README.md).
# ---------------------------------------------------------------------------
set -euo pipefail

die() { echo "winget: $*" >&2; exit 1; }

url=${1:-}
[[ $url == https://* ]] || die "usage: winget.sh https://.../feterm-X.Y.Z-windows-x64-setup.exe"
top=$(cd "$(dirname "$(realpath "$0")")/../.." && pwd)
version=$(sed -n 's/^VERSION[[:space:]]*?=[[:space:]]*//p' "$top/Makefile")
setup="$top/dist/feterm-$version-windows-x64-setup.exe"
[[ -f $setup ]] || die "$setup not found; run packaging/windows/build.sh"
[[ $url == *"feterm-$version-windows-x64-setup.exe"* ]] || die "the URL is not for version $version"

sha=$(sha256sum "$setup" | cut -d' ' -f1)
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
curl -fsSL -o "$tmp" "$url" || die "cannot download $url"
[[ $(sha256sum "$tmp" | cut -d' ' -f1) == "$sha" ]] || die "the download differs from $setup"

id=FaheemEssack.FeTerm
schema=1.10.0
out="$top/dist/winget/manifests/f/FaheemEssack/FeTerm/$version"
mkdir -p "$out"
head="# yaml-language-server: \$schema=https://aka.ms/winget-manifest"

cat > "$out/$id.yaml" <<EOF
$head.version.$schema.schema.json
PackageIdentifier: $id
PackageVersion: $version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $schema
EOF

cat > "$out/$id.installer.yaml" <<EOF
$head.installer.$schema.schema.json
PackageIdentifier: $id
PackageVersion: $version
InstallerType: inno
MinimumOSVersion: 10.0.17763.0
UpgradeBehavior: install
ProductCode: '{6F0D3C9E-4B1A-4E57-9C2B-8A7E5D1F3B64}_is1'
Installers:
- Architecture: x64
  Scope: user
  InstallerUrl: $url
  InstallerSha256: ${sha^^}
  InstallerSwitches:
    Custom: /CURRENTUSER
- Architecture: x64
  Scope: machine
  InstallerUrl: $url
  InstallerSha256: ${sha^^}
  InstallerSwitches:
    Custom: /ALLUSERS
ManifestType: installer
ManifestVersion: $schema
EOF

cat > "$out/$id.locale.en-US.yaml" <<EOF
$head.defaultLocale.$schema.schema.json
PackageIdentifier: $id
PackageVersion: $version
PackageLocale: en-US
Publisher: Faheem Essack
PublisherUrl: https://gitlab.com/faheeme
PackageName: FeTerm
PackageUrl: https://faheeme.gitlab.io/feterm-site/
License: MIT
LicenseUrl: https://gitlab.com/faheeme/feterm/-/blob/main/LICENSE
ShortDescription: Terminal emulator with syntax highlighting for Cisco, Arista and Juniper
Description: |-
  A GPU-accelerated terminal with PCRE2 syntax highlighting for network gear (Cisco, Arista,
  Juniper), a session manager for SSH, Telnet and serial consoles, tabs and splits.
Moniker: feterm
Tags:
- terminal
- ssh
- telnet
- serial
- network
- cisco
- juniper
- arista
ReleaseNotesUrl: https://gitlab.com/faheeme/feterm/-/releases/v$version
ManifestType: defaultLocale
ManifestVersion: $schema
EOF
echo "== wrote $out"
ls "$out"
