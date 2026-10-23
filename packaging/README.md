# Packaging

| Folder | Target | Service |
|---|---|---|
| `arch/` | Arch, Omarchy, Manjaro | AUR |
| `rpm/` | Fedora, Rocky / Alma / RHEL 10 | COPR |
| `debian/` | Ubuntu 26.04 and newer | Launchpad PPA |

All three download the tag's source archive from GitLab:
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
4. **PPA** (in the Ubuntu 26.04 distrobox). The `X.Y.Z-1~ppa1` entry must already be at
   the top of `packaging/debian/changelog` in the tag:
   ```sh
   cd ~/ppa
   wget -O feterm_X.Y.Z.orig.tar.gz https://gitlab.com/faheeme/feterm/-/archive/vX.Y.Z/feterm-vX.Y.Z.tar.gz
   tar xf feterm_X.Y.Z.orig.tar.gz && cd feterm-vX.Y.Z && cp -r packaging/debian .
   debuild -S -sa -k<KEYID>
   cd .. && dput ppa:faheeme/feterm feterm_X.Y.Z-1~ppa1_source.changes
   ```
   When the 26.04 build succeeds, copy it to 26.10 on Launchpad: *Copy packages*, destination
   series *Stonking*, **Copy existing binaries**. ("Rebuild" is refused inside the same PPA.)

A fix to the packaging alone (no code change) keeps the version and bumps the package
release instead: `pkgrel=2`, `Release: 2`, or `-1~ppa2`.

### PPA: packaging-only re-upload

A `~ppa2` (or later) upload reuses the version's original tarball, so it must not change
anything outside `debian/`. Don't copy `packaging/debian` over itself inside the source
tree: `dpkg-source` compares the tree with the tarball and stops with "unexpected upstream
changes". Take the new files from `main` and put them only in `debian/`:

```sh
cd ~/ppa                                  # still holds feterm_X.Y.Z.orig.tar.gz from the ~ppa1 upload
rm -rf feterm-vX.Y.Z newdeb
tar xf feterm_X.Y.Z.orig.tar.gz
mkdir newdeb
wget -qO- https://gitlab.com/faheeme/feterm/-/archive/main/feterm-main.tar.gz \
  | tar xz -C newdeb --strip-components=3 feterm-main/packaging/debian
cp -r newdeb feterm-vX.Y.Z/debian
cd feterm-vX.Y.Z && debuild -S -sd -k<KEYID>
cd .. && dput ppa:faheeme/feterm feterm_X.Y.Z-1~ppa2_source.changes
```

Use `-sd`, not `-sa`, and the same `.orig.tar.gz` file as before: Launchpad already has that
tarball and rejects a different one for the same version. Then copy the build to 26.10 as
above.

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
