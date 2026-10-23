# ---------------------------------------------------------------------------
#  COPR "make srpm" build method. COPR clones the repo at the pushed tag and
#  runs, as root in a Fedora chroot with network access:
#
#      make -f .copr/Makefile srpm outdir=<dir> spec=<spec file>
#
#  The source RPM is built from the tag's GitLab archive, the same tarball the
#  AUR and PPA packages use, so all three ship identical sources.
# ---------------------------------------------------------------------------
TOP     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
outdir  ?= $(TOP)/build/srpm
spec    ?= $(TOP)/packaging/rpm/feterm.spec

SPEC_VERSION := $(shell sed -n 's/^Version:[[:space:]]*//p' '$(spec)')
MAKE_VERSION := $(shell sed -n 's/^VERSION[[:space:]]*?=[[:space:]]*//p' '$(TOP)/Makefile')

.PHONY: srpm

srpm:
	@test -n '$(SPEC_VERSION)' || { echo 'no Version: line in $(spec)' >&2; exit 1; }
	@test '$(SPEC_VERSION)' = '$(MAKE_VERSION)' || \
	    { echo 'spec Version $(SPEC_VERSION) does not match Makefile VERSION $(MAKE_VERSION)' >&2; exit 1; }
	command -v spectool >/dev/null || dnf -y install rpmdevtools rpm-build
	mkdir -p '$(outdir)'
	spectool -g -C '$(outdir)' '$(spec)'
	rpmbuild -bs '$(spec)' --define '_sourcedir $(outdir)' --define '_srcrpmdir $(outdir)'
