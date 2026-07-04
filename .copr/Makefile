# COPR SCM "make srpm" entry point.
#
# COPR invokes this as:
#     make -f <clone>/.copr/Makefile srpm outdir=<dir> spec=<spec-path>
# and picks up the .src.rpm dropped in $(outdir). Configure each COPR package
# with source type "Custom" / SCM method "make srpm", clone URL of this repo,
# and its Spec File field pointing at the respective spec (that value arrives
# here as $(spec), which contrib/rpm/make-srpm.sh branches on).
#
# Runnable locally too, e.g.:
#     make -f .copr/Makefile srpm outdir=/tmp/out spec=contrib/rpm/windowtolayer.spec

ROOT := $(dir $(lastword $(MAKEFILE_LIST)))..

.PHONY: srpm
srpm:
	@"$(ROOT)/contrib/rpm/make-srpm.sh" "$(spec)" "$(outdir)"
