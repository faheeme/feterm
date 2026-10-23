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
