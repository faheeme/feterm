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
   With the COPR webhook in GitLab (Settings → Webhooks, "Tag push events"), the tag push
   alone starts the build.
4. **PPA:**
   ```sh
   wget -O feterm_X.Y.Z.orig.tar.gz https://gitlab.com/faheeme/feterm/-/archive/vX.Y.Z/feterm-vX.Y.Z.tar.gz
   tar xf feterm_X.Y.Z.orig.tar.gz && cd feterm-vX.Y.Z && cp -r packaging/debian .
   dch -v X.Y.Z-1~ppa1 "New upstream release"
   debuild -S -sa -k<KEYID>
   dput ppa:faheeme/feterm ../feterm_X.Y.Z-1~ppa1_source.changes
   ```
   Copy the `dch` changelog entry back into `packaging/debian/changelog` and commit it.

A fix to the packaging alone (no code change) keeps the version and bumps the package
release instead: `pkgrel=2`, `Release: 2`, or `-1~ppa2`.
