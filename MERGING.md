# Merging & Upstream-Tracking Policy

Calamansi Juice 2 is a **white-label fork** of [lemonade-sdk/lemonade](https://github.com/lemonade-sdk/lemonade), tracking it as an upstream. The `upstream` git remote is already configured to point at `lemonade-sdk/lemonade`.

This document defines which files are "ours" (fork-specific — expected to conflict on `git merge upstream/main`) versus "theirs" (upstream's — take wholesale on conflict), the conflict-resolution policy, the version scheme, and the open TODOs left by the Part 1 rebrand commit.

## Naming changes made in this fork

| Upstream (lemonade-sdk/lemonade) | Calamansi Juice 2 |
|---|---|
| `lemonade` (CLI binary) | `calamansi` |
| `lemond` (server daemon binary) | `calamansid` |
| `lemonade-tray` (macOS/Linux tray) | `calamansi-tray` |
| `LemonadeServer.exe` (Windows tray CMake target) | `CalamansiServer.exe` |
| `lemonade-app` (Tauri desktop app, `src/app`) | `calamansi-app` |
| `lemonade-web` (browser app, `src/web-app`) | `calamansi-web` |

Old binary names still work: build- and install-time CMake steps produce same-directory alias copies/symlinks (see "Deprecation-alias policy" below), and any binary invoked under an old name prints a one-line deprecation warning to stderr via `src/cpp/include/lemon/deprecated_alias.h`, then runs normally. This is what keeps unmodified upstream tests (which hardcode `lemond`/`lemonade` binary filenames, e.g. `test/utils/test_models.py::_default_build_binary`) passing without any test changes.

API routes (`/api/v1/...`, `/spans/stream`, etc.), model registry names, config file keys, internal C++ namespaces/classes/files, and CI workflows were deliberately **not** touched in this pass — see "Deferred to a follow-up" below.

## "Ours" surface (expected to conflict on `git merge upstream/main`)

These are the files this Part 1 commit touched. On an upstream merge, prefer this fork's version for these paths and manually re-apply the narrow renames if upstream also touched the same file/region:

- `CMakeLists.txt` — `EXECUTABLE_NAME` value, `LEMON_FULL_VERSION_STRING`, POST_BUILD old-name alias blocks (calamansid/lemond, tray/server targets), `LEMONADE_SYSTEMD_UNIT_NAME` value, systemd/plist/desktop-entry `configure_file`/`install()` paths, WiX/macOS-signing target-name references, Tauri app output filename variables (`TAURI_EXE_NAME`, `TAURI_BUILD_OUTPUT_SOURCE`, `TAURI_APP_BUILD_COPY`), embeddable-archive target references
- `src/cpp/cli/CMakeLists.txt` — CLI target renamed `lemonade` → `calamansi`, POST_BUILD alias block, install-time compat symlink
- `src/cpp/tray/CMakeLists.txt` — tray targets renamed (`LemonadeServer` → `CalamansiServer`, `lemonade-tray` → `calamansi-tray`), POST_BUILD alias blocks, install-time symlink target name
- `src/cpp/include/lemon/deprecated_alias.h` — new shared shim (basename detection + stderr warning)
- `src/cpp/include/lemon/version.h.in` — `LEMON_VERSION_STRING` now substitutes `LEMON_FULL_VERSION_STRING` instead of `PROJECT_VERSION` directly
- `src/cpp/server/main.cpp`, `src/cpp/cli/main.cpp`, `src/cpp/tray/main.cpp` — `deprecated_alias.h` wired into each `main()`/`wWinMain()`; product-name/help/version strings
- `src/cpp/server/cli_parser.cpp` — `APP_NAME`/`APP_DESC` macros
- `src/cpp/tray/tray_ui.cpp` — notification/tooltip product-name strings
- `data/calamansi-juice-server.service.in` (was `data/lemond.service.in`), `data/calamansi-juice-server-user.service.in` (was `data/lemond-user.service.in`), `data/calamansi-juice.sysusers` (was `data/lemonade.sysusers`) — systemd unit templates + sysusers snippet
- `data/calamansi-app.desktop` (was `data/lemonade-app.desktop`), `data/calamansi-web-app.desktop` (was `data/lemonade-web-app.desktop`) — desktop entries
- `src/cpp/com.calamansi.server.plist.in` (was `src/cpp/com.lemonade.server.plist.in`), `src/cpp/com.calamansi.tray.plist.in` (was `src/cpp/com.lemonade.tray.plist.in`) — macOS launchd plists
- `src/app/package.json`, `src/web-app/package.json` — `name`/`version`/`description`
- `src/app/src-tauri/tauri.conf.json` — `productName`, `identifier`, window `title`, bundle `shortDescription`/`longDescription`
- `src/app/src-tauri/Cargo.toml`, `src/app/src-tauri/src/main.rs` — Cargo package/lib name renamed (`lemonade-app`/`lemonade_app_lib` → `calamansi-app`/`calamansi_app_lib`) and the one call site that references the lib crate name
- `src/app/src-tauri/src/tray_launcher.rs` — macOS tray-launcher binary path/process-name constants updated to match the renamed `calamansi-tray` binary (load-bearing: this is how the desktop app finds/kills/spawns the tray helper, not just branding prose)
- `src/app/assets/logo.svg` — placeholder mark (calamansi fruit + fork icon)
- `README.md` — title, fork-notice blockquote, top intro/roadmap/embeddable/connect section prose
- `mkdocs.yml` — `site_name`, `site_description`, `repo_name`, `repo_url`
- `NOTICE` — new file, upstream attribution
- `MERGING.md` — this file

## "Theirs" surface (take upstream wholesale on conflict)

Everything else, including but not limited to:

- All core C++ logic under `src/cpp/` not listed above: routes, router, backends, telemetry, model registry, config handling, etc.
- All docs content under `docs/` beyond what's listed above (this fork did not rebrand doc body content, only top-level site identity in `mkdocs.yml` and the README title/fork-notice)
- All of `src/app/src/` and `src/web-app/src/` application/UI source code (React/TS) — **except** `src/app/src-tauri/src/tray_launcher.rs` and `src/app/src-tauri/src/main.rs`, which needed the one-line fixes above to keep working after the CMake target rename
- `test/` — no test files were modified; this is the acceptance bar for the rename (unmodified upstream tests must still pass via the deprecated-alias binaries)
- `.github/workflows/` — CI is out of scope for this commit; see "Deferred to a follow-up" below
- `docs/man/man1/lemond.1`, `docs/man/man1/lemonade.1` — man pages, not renamed in this pass (see TODOs)
- CPack/macOS packaging identity (`CPACK_PACKAGE_VENDOR`, `CPACK_PACKAGE_NAME`, `CPACK_PRODUCTBUILD_IDENTIFIER`, `com.lemonade.server.Applications` bundle ID, `/Library/Application Support/Lemonade`, `share/lemonade-server/*` install paths, `lemonade-embeddable-*` archive naming, `.msi`/`.deb`/`.rpm`/`.pkg` artifact filenames) — deliberately left alone in this pass; see TODOs

## Conflict-resolution policy

On `git merge upstream/main`:

1. For paths in the "theirs" list: take upstream's version wholesale.
2. For paths in the "ours" list: keep this fork's version. If upstream also touched the exact same file/region, manually re-apply the narrow rename/branding delta on top of upstream's new content rather than reverting to this fork's stale version.
3. When in doubt (a file not clearly in either list), prefer upstream and re-apply only the specific rename lines this commit is known to have changed (grep the file for `calamansi`/`Calamansi` against this fork's history to find them).

## Version scheme

Full user-facing version string: `<upstream-version>+cj<fork-version>` (currently `11.0.0+cj2.0.0`).

- The base part (`11.0.0`) tracks the upstream `lemonade-sdk/lemonade` release/tag this fork is rebased on. It comes from `project(lemon_cpp VERSION 11.0.0)` in `CMakeLists.txt` (must stay numeric-only — a CMake requirement).
- The `+cj<fork-version>` suffix is this fork's own release number, set via `LEMON_FULL_VERSION_STRING` in `CMakeLists.txt` and substituted into `LEMON_VERSION_STRING` in `src/cpp/include/lemon/version.h.in`, which every consumer (CLI `--version`, server `--version`, tray) reads.
- Bump the `+cj` part for fork-only releases (e.g. `+cj2.1.0`).
- Rebase the base part (and re-verify/re-apply the "ours" surface above) when pulling a new upstream tag.
- `src/app/package.json` and `src/web-app/package.json` `version` fields follow the same scheme (`11.0.0+cj2.0.0`); `src/app/src-tauri/Cargo.toml`'s `version` field was left at its existing `0.0.0` placeholder since Tauri reads the real version from `../package.json` (see `tauri.conf.json`'s `"version": "../package.json"`), not from `Cargo.toml`.

## Deprecation-alias policy

Old-name aliases and their stderr deprecation warnings exist purely for backward compatibility during the transition and should be removed after **one fork release cycle** — i.e. drop them at `+cj3.0.0` or later. When that happens, delete:

- `src/cpp/include/lemon/deprecated_alias.h` (the shim itself)
- The `lemon::warn_if_deprecated_alias(...)` call sites in `src/cpp/server/main.cpp`, `src/cpp/cli/main.cpp`, and `src/cpp/tray/main.cpp`
- The POST_BUILD alias blocks in `CMakeLists.txt` (search for "Backward-compat alias" / "deprecated alias" comments around the `${EXECUTABLE_NAME}` target), `src/cpp/cli/CMakeLists.txt` (`calamansi` target), and `src/cpp/tray/CMakeLists.txt` (`CalamansiServer`/`calamansi-tray` targets)
- The install-time compat symlink block at the bottom of `src/cpp/cli/CMakeLists.txt` (`/usr/bin/lemonade` → `calamansi`)
- At that point also consider whether `test/` still needs to discover binaries by the old `lemond`/`lemonade` names at all, or whether the fork's own test suite should be updated to look for `calamansid`/`calamansi` directly (out of scope for this fork to decide unilaterally — this repo tracks upstream's `test/` as "theirs").

## Open TODOs left by this commit

- **Raster icon files not yet replaced** (need real image-generation tooling, not hand-editable):
  - `src/app/src-tauri/icons/*` (all sizes: `32x32.png`, `128x128.png`, `128x128@2x.png`, `icon.icns`, `icon.ico`)
  - `src/app/assets/favicon.ico`
  - `docs/favicon.ico`
  - `src/cpp/resources/static/favicon.ico`
- **`mkdocs.yml` `repo_url`/`repo_name`** point at `rolandpascua-cloud/calamansi-juice-2`, which does not exist yet — placeholder until the real fork repo is created and pushed (the `origin` remote in this working tree still points at `lemonade-sdk/lemonade` and will be repointed separately).
- **CI workflow artifact renaming** (`.github/workflows/*.yml`, `.msi`/`.deb`/`.rpm`/`.pkg`/embeddable-archive filenames, CPack package identity: `CPACK_PACKAGE_VENDOR`, `CPACK_PACKAGE_NAME`, `CPACK_PRODUCTBUILD_IDENTIFIER`, `com.lemonade.server.Applications` bundle ID, `/Library/Application Support/Lemonade` and `share/lemonade-server/*` install paths, `lemonade-embeddable-*` archive dir naming) — deferred to a separate, narrower follow-up commit, same as CI workflows.
- **`docs/man/man1/lemond.1` and `docs/man/man1/lemonade.1`** man pages not renamed/updated for the new binary names.
- **App/web UI source prose** (`src/app/src/renderer/*.tsx`, `*.ts`) still references "lemond"/"lemonade" in comments and user-facing copy (e.g. `CloudProvidersSection.tsx`, `ModelManager.tsx`, `serverConfig.ts`) — left untouched as in-scope "theirs" application code for this narrow branding pass.
- **README.md** was given a title/fork-notice/intro pass only; the bulk of the document (Getting Started links, Supported Platforms badges, Maintainers/Code-Signing/License-Attribution sections, marketplace app icons) still points at real `lemonade-sdk/lemonade` / `lemonade-server.ai` infrastructure intentionally, since this fork has no parallel infrastructure yet and breaking those links/credits would be worse than leaving them.
- **Systemd data paths** (`StateDirectory=lemonade`, `RuntimeDirectory=lemonade`, `WorkingDirectory=%S/lemonade`, `EnvironmentFile=-/etc/lemonade/conf.d/*.conf`, `/etc/lemonade/conf.d` install destination) were deliberately left as `lemonade` rather than renamed to `calamansi`, to avoid an operational data-migration concern (existing installs' state/config directories) that's out of scope for a branding-only pass. Revisit if/when this fork diverges further from upstream's install layout.
- **Deep-link URL scheme** (`lemonade://`, `MimeType=x-scheme-handler/lemonade`, `plugins.deep-link.desktop.schemes` in `tauri.conf.json`) and the **UDP discovery-beacon protocol's `"service":"lemonade"` field** (`src/app/src-tauri/src/beacon.rs`, `src/cpp/server/utils/network_beacon.cpp`) were intentionally left unchanged — these are protocol/config identifiers, not branding strings, and changing them would be a breaking wire-format/URL-handler change outside this pass's scope.
