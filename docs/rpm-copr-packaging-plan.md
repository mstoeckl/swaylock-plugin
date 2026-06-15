# RPM packaging + COPR build plan (swaylock-plugin, windowtolayer)

## Goal

Produce **Fedora 44 RPMs** for `swaylock-plugin` (this fork) and `windowtolayer`,
published to a **COPR** repo, with builds **triggered by GitHub releases**. These
RPMs are consumed by the `phalanx` hyprland image to provide the
swaylock-plugin xscreensaver lockscreen.

Three coordinated pieces (this is the build half):

| Piece | Repo | Provides |
|---|---|---|
| **This (packaging/CI)** | `syndr/swaylock-plugin` | the COPR + RPMs |
| Image | `syndr/phalanx` @ `feat/swaylock-screensaver-lockscreen` | `dnf copr enable` + install into the hyprland image |
| Hyprland config | `syndr/hyprland-wm-config` @ `feat/swaylock-screensaver-lockscreen` | lock wiring, rofi hack picker |

## Why this fork (must-haves baked into the RPM)

Build from this fork's `main`, NOT upstream `mstoeckl`. `main` carries the fixes
that make the lockscreen work at all (see git log):

- **`SIGCHLD` reset** in spawned plugins — without it, Xwayland's `waitpid()` for
  `xkbcomp` returns `ECHILD` and the hack background never starts.
- **multi-output use-after-free crash fix** (`--command-each` on N monitors).
- `__DATE__` version-string fix.

---

## Channel: COPR (decided)

- COPR project: **`syndr/swaylock-plugin`** (one project, two packages).
- Chroots: **`fedora-44-x86_64`** at minimum; add `fedora-44-aarch64` only if the
  phalanx image matrix needs it (the host is x86_64).
- COPR builds in clean chroots, signs packages, and serves repodata — consumers
  just `dnf copr enable syndr/swaylock-plugin`. Only the owner needs a (free)
  Fedora account; consumers are anonymous.

---

## Packages

### 1. `swaylock-plugin.spec` (Meson/C — straightforward)

- **Version:** track `meson.build` `project(version: …)` (currently `1.8.6`) and
  the release tag (see Versioning).
- **BuildRequires:** `meson ninja-build gcc pkgconfig(wayland-client)
  pkgconfig(wayland-server) pkgconfig(wayland-protocols)
  pkgconfig(wayland-scanner) pkgconfig(xkbcommon) pkgconfig(cairo)
  pkgconfig(gdk-pixbuf-2.0) pam-devel systemd-devel scdoc` (mirror the deps in
  `contrib/build-env.sh` / README).
- **Build:** standard `%meson` / `%meson_build` / `%meson_install`. Prefix is
  `/usr`, so `sysconfdir=/etc` → the PAM file installs to **`/etc/pam.d/swaylock-plugin`**
  automatically (Meson already does `install_dir: sysconfdir/pam.d`). Good — the
  RPM ships PAM; phalanx then needs nothing for PAM.
- **Files shipped:** `/usr/bin/swaylock-plugin`, `/usr/bin/swaylock-sleep-watcher`,
  man page, shell completions, `%config(noreplace) /etc/pam.d/swaylock-plugin`.
- **DECISION — ship the Xwayland wrapper:** install `example_xwayland_wrapper.py`
  to a stable path, e.g. **`/usr/libexec/swaylock-plugin/example_xwayland_wrapper.py`**
  (or `/usr/share/swaylock-plugin/`). This lets `hyprland-wm-config` reference
  the RPM path instead of vendoring the wrapper. Meson does not currently install
  it — add an `install_data` for it (small change to `meson.build`, or do it in
  `%install`). Coordinate the chosen path with the config repo's open question.
- **No `-D…` tweaks needed** beyond defaults (`pam` + `logind` auto-enable on
  Fedora). Confirm `gdk-pixbuf` enabled.

### 2. `windowtolayer.spec` (Rust — more involved)

`windowtolayer` lives at `https://gitlab.freedesktop.org/mstoeckl/windowtolayer`
(separate project). Options for sourcing — **decide (open question):**

- **(a)** Package it from its own upstream release tarball via `Source0` in a spec
  kept here (pragmatic, one COPR, but couples release cadence).
- **(b)** Fork it to `syndr/windowtolayer` with its own packaging + release
  workflow feeding the same COPR (cleaner separation; more repos).

Packaging notes regardless:
- Pure Rust, deps `arrayvec lexopt log rustix` (all crates.io).
- **BuildRequires:** `cargo rust rustfmt python3` — `rustfmt` is required by its
  `build.rs` (protocol codegen), `python3` by `protogen.py`. (We hit the
  `rustfmt` requirement during the ad-hoc build.)
- Use Fedora Rust macros (`%cargo_build`/`%cargo_install`) with vendored deps, or
  a network-allowed build; COPR allows network by default but vendoring is more
  reproducible.
- Ships `/usr/bin/windowtolayer`.

---

## Build trigger: GitHub release → COPR

Use **GitHub Actions on `release: [published]`** so RPMs are cut per tagged
release (explicit version control), submitting to COPR via `copr-cli`.

Outline (`.github/workflows/copr-release.yml`):

```yaml
on:
  release:
    types: [published]
jobs:
  copr:
    runs-on: ubuntu-latest
    container: fedora:44              # has rpmbuild/copr-cli, or dnf install them
    steps:
      - uses: actions/checkout@v4
      - run: dnf -y install copr-cli rpm-build rpmdevtools git
      - name: COPR auth
        run: |
          mkdir -p ~/.config
          printf '%s' "${{ secrets.COPR_API_TOKEN }}" > ~/.config/copr   # full ~/.config/copr file
      - name: Build SRPM
        run: |
          # set Version from the release tag; build SRPM from the spec
          rpmbuild -bs ... -> swaylock-plugin-<ver>.src.rpm
      - name: Submit to COPR
        run: copr-cli build syndr/swaylock-plugin path/to.src.rpm
```

- **`COPR_API_TOKEN`** GH secret = the full contents of the COPR-provided
  `~/.config/copr` token file (from copr.fedorainfracloud.org → API).
- Alternative (no Actions): COPR's own SCM/webhook build from a `.copr/Makefile`
  with an `srpm:` target — but that triggers on push, not releases, so Actions +
  `copr-cli` is the better match for "release-triggered".
- If windowtolayer is option (a) in this repo, the same workflow (or a second
  job) submits its SRPM too; if option (b), it has its own workflow.

### Versioning

- Tag releases as `vX.Y.Z` (e.g. `v1.8.6`). The workflow derives `Version` from
  the tag (`${GITHUB_REF_NAME#v}`) and injects it into the spec (e.g. `rpmbuild
  --define "version X.Y.Z"`), or keep the spec `Version` in sync with
  `meson.build` and just gate the build on the release event.
- Keep the spec `Version` and `meson.build` `project(version:)` aligned to avoid
  drift.

---

## Where files live in this repo

- `contrib/rpm/swaylock-plugin.spec` (and `windowtolayer.spec` if option (a)).
- `.github/workflows/copr-release.yml`.
- Optionally `.copr/Makefile` if also supporting COPR webhook builds.
- (If shipping the wrapper) a one-line `install_data` in `meson.build` for
  `example_xwayland_wrapper.py`.

---

## Tasks

1. Create the COPR project `syndr/swaylock-plugin` (chroot `fedora-44-x86_64`);
   generate an API token; add it as the `COPR_API_TOKEN` GH secret.
2. Write `swaylock-plugin.spec` (`%meson` build; ship PAM + binaries + man +
   completions; add the Xwayland wrapper at a stable path).
3. Decide windowtolayer sourcing (a vs b) and write its spec/packaging.
4. Add `.github/workflows/copr-release.yml` (release-triggered → SRPM →
   `copr-cli build`).
5. Cut a test release; confirm both RPMs build in COPR for F44 and install
   cleanly (`dnf copr enable` in a throwaway F44 container, `rpm-ostree`-style
   layering test if possible).
6. Tell phalanx + hyprland-wm-config the final paths (esp. the wrapper install
   path) to close their open questions.

## Open questions

- **windowtolayer sourcing** — package here from upstream tarball, or a separate
  `syndr/windowtolayer` fork with its own release flow?
- **wrapper install path** — `/usr/libexec/swaylock-plugin/` vs `/usr/share/…`;
  drives whether `hyprland-wm-config` vendors it or references it.
- **aarch64** — needed? (only if the image matrix builds aarch64).
- **version source of truth** — release tag vs `meson.build`; keep them aligned.
- **upstreaming** — the fork's fixes are upstream-worthy; if/when merged upstream,
  revisit whether to keep packaging from the fork or a tagged upstream.

## References

- This fork: `git@github.com:syndr/swaylock-plugin.git` (`main`) — the fixes,
  `contrib/build-env.sh` (build deps), expanded README install section.
- windowtolayer: `https://gitlab.freedesktop.org/mstoeckl/windowtolayer`.
- Consumer (image): `syndr/phalanx` @ `feat/swaylock-screensaver-lockscreen` →
  `docs/swaylock-screensaver-lockscreen-plan.md`.
- Consumer (config): `syndr/hyprland-wm-config` @
  `feat/swaylock-screensaver-lockscreen`.
- COPR: `https://copr.fedorainfracloud.org/` · `copr-cli` ·
  Fedora Rust packaging guidelines for the windowtolayer spec.
