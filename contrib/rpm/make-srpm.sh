#!/usr/bin/env bash
# =============================================================================
# contrib/rpm/make-srpm.sh
#
# Generate a source RPM for one of this project's packages. Used by COPR's SCM
# "make srpm" build method (see .copr/Makefile) and runnable locally.
#
#   Usage: make-srpm.sh <spec-path> <outdir>
#
# The two packages share this one entry point and branch on the spec basename:
#
#   swaylock-plugin.spec  archive THIS repo at the built ref. The package
#                         Version comes from meson.build; if COPR/you build from
#                         a vX.Y.Z tag, assert the tag matches meson.build.
#
#   windowtolayer.spec    track-latest: resolve the newest stable vX.Y.Z tag
#                         from the upstream GitLab repo at build time and build
#                         that. (The spec's hardcoded Version is only a fallback.)
#
# COPR runs the "make srpm" step in a mock chroot as root, so we dnf-install any
# missing helpers. Locally those tools already exist, so nothing is installed.
# =============================================================================
set -euo pipefail

spec="${1:?usage: make-srpm.sh <spec-path> <outdir>}"
outdir="${2:?usage: make-srpm.sh <spec-path> <outdir>}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

ensure() { command -v "$1" >/dev/null 2>&1 || dnf -y install "${2:-$1}"; }
ensure git
ensure curl
ensure rpmbuild rpm-build

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/SOURCES" "${outdir}"

build_srpm() { # <spec> [extra --define args...]
    local s="$1"; shift
    rpmbuild -bs \
        --define "_topdir ${work}" \
        --define "_sourcedir ${work}/SOURCES" \
        --define "_srcrpmdir ${outdir}" \
        "$@" \
        "${s}"
}

case "$(basename "${spec}")" in
swaylock-plugin.spec)
    version="$(sed -nE "s/[[:space:]]*version: '([^']+)'.*/\1/p" meson.build | head -n1)"
    [ -n "${version}" ] || { echo "could not read version from meson.build" >&2; exit 1; }

    # Release-vs-snapshot: a build from an exact vX.Y.Z tag that matches
    # meson.build is THE release -> Release: 1. Any other build (branch, manual
    # COPR rebuild, PR CI) gets a snapshot Release that sorts *below* 1, so it
    # can never overwrite a published NVR or be offered to dnf as an upgrade.
    tag="$(git describe --tags --exact-match 2>/dev/null || true)"
    release_def=()
    if [ "${tag}" = "v${version}" ]; then
        : # release build; spec default Release: 1
    elif [ -n "${tag}" ]; then
        echo "release tag ${tag} does not match meson.build version ${version}" >&2
        exit 1
    else
        snap="0.$(date -u +%Y%m%d%H%M%S).g$(git rev-parse --short HEAD)"
        release_def=(--define "pkg_release ${snap}")
        echo "make-srpm.sh: non-tag build -> snapshot Release ${snap}" >&2
    fi

    git archive --format=tar.gz \
        --prefix="swaylock-plugin-${version}/" \
        --output="${work}/SOURCES/swaylock-plugin-${version}.tar.gz" \
        HEAD
    build_srpm "${spec}" --define "pkg_version ${version}" "${release_def[@]}"
    ;;

windowtolayer.spec)
    api="https://gitlab.freedesktop.org/api/v4/projects/mstoeckl%2Fwindowtolayer/repository/tags"
    version="$(curl -sfL "${api}" \
        | grep -oE '"name":"v[0-9]+\.[0-9]+\.[0-9]+"' \
        | sed -E 's/.*"v([0-9]+\.[0-9]+\.[0-9]+)".*/\1/' \
        | sort -V | tail -n1)"
    [ -n "${version}" ] || { echo "could not resolve latest windowtolayer tag from ${api}" >&2; exit 1; }
    echo "make-srpm.sh: windowtolayer track-latest -> v${version}" >&2

    curl -sfL \
        --output "${work}/SOURCES/windowtolayer-v${version}.tar.gz" \
        "https://gitlab.freedesktop.org/mstoeckl/windowtolayer/-/archive/v${version}/windowtolayer-v${version}.tar.gz"
    build_srpm "${spec}" --define "pkg_version ${version}"
    ;;

*)
    echo "make-srpm.sh: unknown spec '${spec}'" >&2
    exit 1
    ;;
esac
