# Packaging

| Folder | Target | Service |
|---|---|---|
| `arch/` | Arch, Omarchy, Manjaro | AUR |
| `rpm/` | Fedora, Rocky / Alma / RHEL 10 | COPR |
| `debian/` | Ubuntu 26.04 and newer | Launchpad PPA |
| `windows/` | Windows 10 (1809) and 11, x64 | GitLab release files, winget |

The Linux packages all download the tag's source archive from GitLab:
`https://gitlab.com/faheeme/feterm/-/archive/vX.Y.Z/feterm-vX.Y.Z.tar.gz`, which unpacks to
`feterm-vX.Y.Z/`. The project must be public for this to work.

They all build with `PREFIX=/usr` (the data directory is compiled into the binary, so it has
to be set at build time too) and `WERROR=`, so a warning from a newer compiler doesn't break
a user's build.

## Releasing a new version

1. Bump `VERSION` in the top-level `Makefile`, commit, then:
   ```sh
   git tag vX.Y.Z && git push origin main --tags
   glab release create vX.Y.Z --name "FeTerm X.Y.Z" --notes "..."
   ```
2. **AUR** (in your clone of `ssh://aur@aur.archlinux.org/feterm.git`): set `pkgver=X.Y.Z`
   and `pkgrel=1`, then
   ```sh
   updpkgsums && makepkg -si && makepkg --printsrcinfo > .SRCINFO
   git commit -am "Update to X.Y.Z" && git push
   ```
3. **COPR:** `rpmdev-bumpspec -n X.Y.Z -c "New upstream release" feterm.spec`, then
   ```sh
   spectool -g -R feterm.spec && rpmbuild -bs feterm.spec
   copr-cli build feterm ~/rpmbuild/SRPMS/feterm-X.Y.Z-1.*.src.rpm
   ```
   With automatic builds set up (below), the tag push alone starts the build.
4. **PPA:** `packaging/ppa-upload.sh vX.Y.Z` (see *PPA uploads* below), run by hand or by
   a build server after the tag push. The `X.Y.Z-1~ppa1` entry must already be at the top of
   `packaging/debian/changelog` in the tag, or the script stops and says so. Each Ubuntu
   series gets its own upload, `X.Y.Z-1~ppa1~26.04.1` and `~26.10.1`, built against its own
   libraries, so nothing has to be copied between series on Launchpad.

5. **Windows:** `packaging/windows/build.sh`, then attach `dist/feterm-X.Y.Z-windows-x64.zip`,
   `dist/feterm-X.Y.Z-windows-x64-setup.exe` and `dist/SHA256SUMS` to the GitLab release and
   update winget (see *Windows* below).

A fix to the packaging alone (no code change) keeps the version and bumps the package
release instead: `pkgrel=2`, `Release: 2`, or `-1~ppa2`.

## PPA uploads

`packaging/ppa-upload.sh` builds and signs one source package per series and uploads it
with `dput`; Launchpad then builds amd64 and arm64. A series whose version is already in the
PPA is skipped, so running it again only uploads what is missing. With `PPA_DRY_RUN=1` it
builds and signs without uploading. The signed files are left in `ppa-out/out/`.

### Uploading by hand

The same script works in the Ubuntu distrobox with your own keyring. It asks for the
key's passphrase:

```sh
cd ~/feterm && git pull
PPA_GPG_KEY_ID=<FINGERPRINT> packaging/ppa-upload.sh vX.Y.Z
```

### Packaging-only re-upload (~ppa2)

A re-upload keeps the version, so it must reuse the exact tarball Launchpad already has
for it, and take `debian/` from `main` instead of from the tag. Add the new top entry
(`X.Y.Z-1~ppa2`) to `packaging/debian/changelog`, commit and push it, then:

```sh
cd ~/feterm && git pull
PPA_GPG_KEY_ID=<FINGERPRINT> PPA_ORIG=~/ppa/feterm_X.Y.Z.orig.tar.gz \
    PPA_DEBIAN_DIR=packaging/debian packaging/ppa-upload.sh vX.Y.Z
```

`PPA_ORIG` must be the tarball from the first upload of that version. If you no longer
have it, download it from the PPA page (*View package details*, then the version, then the
`.orig.tar.gz` link). A freshly downloaded GitLab archive may not be byte-identical, and
Launchpad rejects a different tarball for a version it already has. Don't copy
`packaging/debian` over itself inside an unpacked tree: `dpkg-source` then stops with
"unexpected upstream changes", because the tree no longer matches the tarball.

## COPR automatic builds

`.copr/Makefile` builds the source RPM from the tag's GitLab archive. It refuses to build if
the spec's `Version:` doesn't match `VERSION` in the Makefile. To set it up once:

1. In the COPR project, open *Packages → New package → SCM* and fill in:
   - Package name: `feterm`
   - Clone url: `https://gitlab.com/faheeme/feterm.git`
   - Committish and Subdirectory: leave empty
   - Spec File: `packaging/rpm/feterm.spec`
   - How to build SRPM from the source: **make srpm**
   - Tick **Auto-rebuild**, then *Submit*.
2. In the COPR project, open *Settings → Integrations* and copy the **GitLab** webhook URL.
3. In GitLab, open *Settings → Webhooks → Add new webhook*. Paste the URL, tick only
   **Tag push events** (untick *Push events*, so ordinary commits don't trigger builds),
   then *Add webhook*.

For aarch64, add the `-aarch64` chroots too. The Makefile picks the control-flow protection
flag for the CPU: `-fcf-protection` on x86, `-mbranch-protection=standard` on ARM64.

## Windows

`packaging/windows/build.sh` cross-compiles on Linux in Docker (MinGW-w64; the image comes
from `windows/Dockerfile` and is built on the first run), runs the unit tests under Wine,
and writes to `dist/`:

| File | What it is |
|---|---|
| `feterm-X.Y.Z-windows-x64.zip` | portable: unzip anywhere and run `feterm.exe` |
| `feterm-X.Y.Z-windows-x64-setup.exe` | installer: per user without admin rights, or for all users |
| `SHA256SUMS` | checksums of both |

Both hold `feterm.exe`, `feterm-helper.exe` (the Telnet and serial clients, a console
program), the DLLs they need (found by `windows/stage.sh` from their imports, including
ANGLE's `libEGL.dll`/`libGLESv2.dll`, which draw through Direct3D 11), fontconfig's
`etc\fonts` and FeTerm's `data\`. Settings and sessions live in `%APPDATA%\feterm`; neither
the installer nor its uninstaller touches them. SSH sessions and the Files tab (SFTP, and
SCP for servers without it) use the OpenSSH client that ships with Windows
(`System32\OpenSSH`, else `ssh.exe` on `PATH`); "edit locally" copies go to
`%LOCALAPPDATA%\feterm\edit`.

The installer (`windows/feterm.iss`, Inno Setup) adds a Start-menu entry and, if ticked, a
desktop shortcut and "Open FeTerm here" on folders. Never change its `AppId`: Windows and
winget recognise installed versions by it.

### Attaching the files to a release

After `glab release create` (step 1):

```sh
glab release upload vX.Y.Z dist/feterm-X.Y.Z-windows-x64.zip \
    dist/feterm-X.Y.Z-windows-x64-setup.exe dist/SHA256SUMS
```

### winget

winget's catalogue is the GitHub repository `microsoft/winget-pkgs`, so this needs a GitHub
account. Copy the installer's download link from the GitLab release, then:

```sh
packaging/windows/winget.sh https://gitlab.com/.../feterm-X.Y.Z-windows-x64-setup.exe
```

It downloads the file, checks it matches `dist/`, and writes the three manifests to
`dist/winget/manifests/f/FaheemEssack/FeTerm/X.Y.Z/`. On Windows, check them and open the
pull request with Microsoft's tools:

```powershell
winget validate --manifest <that folder>
winget install --manifest <that folder>      # needs: winget settings --enable LocalManifestFiles
wingetcreate submit <that folder>            # forks winget-pkgs and opens the pull request
```

Microsoft's bots test the installer; the package appears in `winget search feterm` once a
moderator merges it, usually within a few days. Later versions go the same way.

### Unsigned builds

The programs are not code-signed. Microsoft Defender may flag a new build as
`Wacatac.B!ml` or similar (a machine-learning guess) and SmartScreen warns about the
installer until enough people have run it. Report a false positive at
<https://www.microsoft.com/en-us/wdsi/filesubmission>. Signing (e.g. Azure Trusted Signing)
removes both.

