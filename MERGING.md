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

### Feature additions (net-new files/endpoints, not upstream renames)

Unlike the rename surface above, these are wholesale new files/features this fork adds on top of upstream — there's nothing upstream to conflict with directly, but a `git merge upstream/main` could still touch the *hook points* they extend (e.g. if upstream changes `telemetry.cpp`'s span pub/sub or `server.cpp`'s route registration). Treat conflicts in these shared hook points the same as "ours" above (keep this fork's addition, re-apply on top of upstream's new code); the net-new files themselves (`telemetry_history_store.*`, `HistoryPanel.tsx`, etc.) will never conflict since upstream has no equivalent.

**Feature 1 — Telemetry History Viewer** (persistent SQLite-backed request history, GET `/telemetry/history` + `/telemetry/history/summary`, POST `/internal/telemetry/history/clear`, GUI "History" tab):

- `src/cpp/server/telemetry_history_store.h` / `.cpp` — new, net-new feature
- `src/cpp/server/telemetry.h` / `.cpp` — extended `register_span_listener`/`unregister_span_listener` to return/take a handle instead of a single global callback (multiple independent subscribers can now coexist safely — this was a latent bug even before this feature: any listener's `unregister_span_listener()` used to clear every other subscriber's registration too); added `g_current_route_decision` thread-local
- `src/cpp/include/lemon/websocket_server.h`, `src/cpp/server/websocket_server.cpp` — updated to the handle-based listener API above
- `src/cpp/include/lemon/server.h`, `src/cpp/server/server.cpp` — new endpoint registrations, `telemetry_history_store_` member, `build_history_access_scope()` (three-tier admin/own-token/own-session scoping, with an open-server carve-out — see `docs/calamansi/history.md`'s Authentication section), `g_current_route_decision` wiring in `handle_chat_completions`
- `src/cpp/include/lemon/runtime_config.h`, `src/cpp/server/runtime_config.cpp`, `src/cpp/resources/defaults.json` — new `telemetry.history.*` config keys
- `CMakeLists.txt` — vendored sqlite3 (FetchContent, amalgamation build)
- `src/app/src/renderer/HistoryPanel.tsx` — new GUI panel
- `src/app/src/renderer/ModelManager.tsx`, `src/app/src/renderer/components/Icons.tsx`, `src/app/src/renderer/utils/appSettings.ts` — `'history'` added to `LeftPanelView`, rail button, persisted-layout-settings union type
- `src/app/styles/styles.css` — `.history-panel-*` rules
- `test/server_telemetry_history.py` — new test file
- `docs/calamansi/history.md`, `mkdocs.yml` (nav entry), `README.md` (new "Calamansi Juice 2 Additions" section)

**Feature 2 — System State Viewer** (point-in-time hardware/OS/firmware/AI-stack snapshot, `GET /system/state`, GUI "System" tab):

- `src/cpp/include/lemon/system_state.h`, `src/cpp/server/system_state.cpp` — new, net-new feature. Reuses `SystemInfo::get_device_dict()`/`get_system_info_dict()` and `SystemInfoCache::get_system_info_with_cache()["recipes"]` from `system_info.cpp` (upstream code, "theirs") rather than re-implementing CPU/GPU/NPU detection or backend-version parsing — if upstream changes those functions' signatures/return shapes on a merge, re-verify `system_state.cpp`'s call sites.
- `src/cpp/include/lemon/server.h`, `src/cpp/server/server.cpp` — `handle_system_state()`, `GET /system/state` route registration
- `CMakeLists.txt` — `system_state.cpp` added to `SOURCES_CORE`
- `src/app/src/renderer/SystemStatePanel.tsx`, `src/app/src/renderer/utils/systemStateData.ts` — new GUI panel + fetch helper
- `src/app/src/renderer/ModelManager.tsx`, `src/app/src/renderer/utils/appSettings.ts` — `'system'` added to `LeftPanelView` / persisted-layout-settings union type (reuses the existing `Cpu` icon, no `Icons.tsx` change needed)
- `src/app/styles/styles.css` — `.system-state-*` rules
- `test/server_system_state.py` — new test file
- `docs/calamansi/system-state.md`, `mkdocs.yml` (nav entry), `README.md` ("Calamansi Juice 2 Additions" section)

**Feature 3 — Resource Dashboard** (live-updating CPU/memory/GPU/NPU/disk/sensors + other-local-inference-service detection, `GET /resources/system` + `/resources/inference`, GUI "Resources" tab):

- `src/cpp/include/lemon/utils/system_probe.h` — new shared header (cmdline parsing, PATH tool lookup, short-timeout subprocess capture); used by `resource_dashboard.cpp` only. `system_state.cpp` (Feature 2) deliberately keeps its own local copies of the same small helpers rather than being refactored to share this header — that refactor was judged not worth the regression risk to an already-shipped, already-tested feature for a marginal dedup benefit. If touching either file, be aware the two sets of helpers can drift.
- `src/cpp/include/lemon/process_probe.h`, `src/cpp/server/process_probe.cpp` — new, net-new. Linux `/proc`-based process enumeration + listening-port discovery (socket-inode cross-referencing via `/proc/net/tcp{,6}` + `/proc/[pid]/fd/*`, falling back to argv `--port` parsing). Returns empty results on non-Linux platforms (this feature is Strix Halo/Linux-specific).
- `src/cpp/include/lemon/resource_dashboard.h`, `src/cpp/server/resource_dashboard.cpp` — new, net-new. Collectors are architecturally modeled on (not copied from) `amd-ai-max-dashboard`'s `backend/collectors.py` + `backend/inference_status.py` — JSON field names deliberately mirror that project's `/api/system`/`/api/inference` shapes. Two corrections made against the reference during manual verification on a real Strix Halo box, not blind ports: (1) `amd-smi metric --json`'s actual shape is `{"gpu_data": [{"usage": {"gfx_activity": {"value": N, "unit": "..."}}, ...}]}` — nested `{value,unit}` leaves under a `gpu_data` wrapper, not the flatter bare-list-of-bare-numbers shape a naive port of the reference's amd-smi fallback parsing would assume (that fallback path is rarely exercised there since `rocm-smi` succeeds first, which is likely why it went uncaught); (2) `/sys/module/amdxdna` existence is used for the module-loaded check instead of shelling out to `lsmod`, consistent with this codebase's existing sysfs-first preference (see `platform/metrics_linux.cpp`).
- `src/cpp/include/lemon/server.h`, `src/cpp/server/server.cpp` — `handle_resources_system()`, `handle_resources_inference()`, route registrations
- `CMakeLists.txt` — `resource_dashboard.cpp`, `process_probe.cpp` added to `SOURCES_CORE`
- `src/app/src/renderer/ResourcesPanel.tsx`, `src/app/src/renderer/utils/resourceData.ts` — new GUI panel + fetch helpers (hand-rolled SVG gauges/sparklines, no new charting dependency, same approach as `HistoryPanel.tsx`)
- `src/app/src/renderer/ModelManager.tsx`, `src/app/src/renderer/components/Icons.tsx`, `src/app/src/renderer/utils/appSettings.ts` — `'resources'` added to `LeftPanelView` / persisted-layout-settings union type, new `Activity` icon
- `src/app/styles/styles.css` — `.resources-*` rules
- `test/server_resources.py` — new test file
- `docs/calamansi/resources.md`, `mkdocs.yml` (nav entry), `README.md` ("Calamansi Juice 2 Additions" section)

**Bundled companion service — Strix Halo Dashboard** (Linux install only, not embedded in the app itself):

- `CMakeLists.txt` — `FetchContent_Declare`/`Populate` of `github.com/rolandpascua-cloud/amd-ai-max-dashboard` (MIT-licensed, same author), pinned to commit `fb6033366ecad13bb4093481fcebbc1479802c60`; `install()` rules copying `backend/`/`frontend/`/`LICENSE` to `share/calamansi-juice-server/strix-halo-dashboard`, installing the launcher script and systemd unit, and a `strix-halo-dashboard.service` → `/usr/lib/systemd/system` symlink (same pattern as `calamansi-juice-server.service`). Not enabled by default — `systemctl enable --now strix-halo-dashboard` is left to the operator.
- `data/strix-halo-dashboard-run.sh.in` — new. Launcher: creates a venv under `StateDirectory=strix-halo-dashboard` on first run, installs the dashboard's pinned `requirements.txt`, execs `uvicorn` bound to `127.0.0.1:8420` (loopback-only — deliberately more restrictive than that project's own `run.sh`, which defaults to `0.0.0.0`, since this instance is reached only via the in-app link, not as a standalone LAN tool).
- `data/strix-halo-dashboard.service.in` — new. Runs as the existing `calamansi` system user; no new sysuser needed.
- `src/app/src/renderer/ResourcesPanel.tsx`, `src/app/styles/styles.css` — "Open Strix Halo Dashboard" button (opens `http://localhost:8420` via `window.api.openExternal`/`window.open`, matching the existing external-link pattern in `MarketplacePanel.tsx`). This is a link, not an iframe embed — the dashboard is a genuinely separate FastAPI process/tech stack, and this app's own Resources tab already covers similar ground architecturally.
- `docs/calamansi/resources.md` — new "Bundled Strix Halo Dashboard" section.

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
- **CI workflow artifact renaming — done for `.msi`/`.rpm`/`.pkg`/embeddable, deliberately NOT done for `.deb`.** `.msi` (WiX output filenames), `.rpm` (`CPACK_PACKAGE_NAME`/`CPACK_PACKAGE_FILE_NAME`/`CPACK_RPM_PACKAGE_URL` in `src/cpp/CPackRPM.cmake`), macOS `.pkg` (`CPACK_PACKAGE_VENDOR`/`CPACK_PACKAGE_NAME`/`CPACK_PRODUCTBUILD_IDENTIFIER`/`CPACK_PRODUCTBUILD_BUNDLE(_ID)`, now `com.calamansi.server(.Applications)` matching Part 1's plist IDs, and `/Library/Application Support/Calamansi Juice`), and `*-embeddable-*` archive/dir naming are all renamed and GH Actions artifact upload/download name pairs kept consistent across `.github/workflows/cpp_server_build_test_release.yml` and the composite actions under `.github/actions/`. Verified with a real `cmake --build` + running server on this fork's dev box (health check, `/system/state`, `/resources/system`, `/telemetry/history` all responded correctly) after the rename.
  - **`.deb` package identity is intentionally left as `lemonade-server`/`lemonade-desktop`.** It's defined in `contrib/debian/control` (real Debian-archive packaging metadata, `Maintainer: Mario Limonciello <superm1@debian.org>`, `Vcs-Browser`/`Vcs-Git` pointing at `salsa.debian.org/debian/lemonade`) and consumed by `dpkg-buildpackage` via `.github/actions/prepare-debian-build` + `build-debian-package`. Renaming the package here would misattribute a real Debian Developer's maintainership to this fork and requires renaming the paired `contrib/debian/lemonade-server.install` / `lemonade-desktop.install` / `postinst` / `postrm` / `preinst` files in lockstep (`dpkg-buildpackage` fails outright if the `.install` manifest's paths don't exactly match what `cmake --install` produces) — out of scope for a CI-artifact-naming pass. Consequently `CMakeLists.txt`'s shared Linux `install(DESTINATION share/lemonade-server/...)` / `share/lemonade` paths and the matching C++ resource-search paths (`get_install_prefixes()` in `path_linux.cpp`/`path_macos.cpp`, `ConfigFile::get_defaults()`'s `/usr/share/lemonade/defaults.json` in `config_file.cpp`/`.h`) were **also** left unrenamed, since those `install()` rules are shared between the (renamed) RPM and the (unrenamed) `.deb` — renaming them would have broken the `.deb`'s hand-maintained `.install` manifest. Net effect: the `calamansi-juice-server` RPM installs its files under `/usr/share/lemonade-server` internally (cosmetic package-name/path mismatch, not a functional bug — nothing requires them to match). Already noted above: **`contrib/debian/lemonade-desktop.install`** additionally still expects `lemonade-web-app.desktop`/`lemonade-web-app`/`lemonade-app.svg`, which don't match Part 1's `calamansi-app.desktop`/`calamansi-app`/`calamansi-app.svg` install() calls — a pre-existing gap from Part 1, not introduced here. The whole `contrib/debian/` native-packaging path (rename + fix the maintainer-attribution question) needs its own dedicated follow-up.
- **`docs/man/man1/lemond.1` and `docs/man/man1/lemonade.1`** man pages not renamed/updated for the new binary names.
- **App/web UI source prose** (`src/app/src/renderer/*.tsx`, `*.ts`) still references "lemond"/"lemonade" in comments and user-facing copy (e.g. `CloudProvidersSection.tsx`, `ModelManager.tsx`, `serverConfig.ts`) — left untouched as in-scope "theirs" application code for this narrow branding pass.
- **README.md** was given a title/fork-notice/intro pass only; the bulk of the document (Getting Started links, Supported Platforms badges, Maintainers/Code-Signing/License-Attribution sections, marketplace app icons) still points at real `lemonade-sdk/lemonade` / `lemonade-server.ai` infrastructure intentionally, since this fork has no parallel infrastructure yet and breaking those links/credits would be worse than leaving them.
- **Systemd `StateDirectory=`/`RuntimeDirectory=`/`WorkingDirectory=` renamed `lemonade` → `calamansi`** in `data/calamansi-juice-server.service.in`, reversing the original branding-pass decision to leave them as `lemonade`. Found via live side-by-side testing on this fork's dev box: a real stock-Lemonade `lemond.service` (`User=lemonade`) already owns `/var/lib/lemonade` and `/run/lemonade`; those systemd directives make systemd `chown` those paths to whichever `User=`/`Group=` the unit declares on every start, so `calamansi-juice-server.service` (`User=calamansi`) reusing the name `lemonade` would have made the two units fight over ownership of the same directories — corrupting a real, unrelated Lemonade install the moment this fork's service was started, not just a cosmetic clash. `calamansid` itself doesn't read `$STATE_DIRECTORY`; its actual cache/config path comes from `$HOME/.cache/lemonade` (`path_linux.cpp::get_cache_dir()`), where `$HOME` is set by systemd from the unit's `User=`'s passwd entry (`/var/lib/calamansi` for `calamansi`, per `data/calamansi-juice.sysusers`) — so the rename has no effect on where the app actually stores data, it only stops the ownership collision at the systemd-directive level. `EnvironmentFile=-/etc/lemonade/conf.d/*.conf` and the `/etc/lemonade/conf.d` install destination are intentionally **not** part of this change and remain `lemonade` — that's a config-file path the app reads directly, not a systemd-managed directory, so it carries no ownership-collision risk and renaming it would be a separate, larger install-layout decision.
- **Deep-link URL scheme** (`lemonade://`, `MimeType=x-scheme-handler/lemonade`, `plugins.deep-link.desktop.schemes` in `tauri.conf.json`) and the **UDP discovery-beacon protocol's `"service":"lemonade"` field** (`src/app/src-tauri/src/beacon.rs`, `src/cpp/server/utils/network_beacon.cpp`) were intentionally left unchanged — these are protocol/config identifiers, not branding strings, and changing them would be a breaking wire-format/URL-handler change outside this pass's scope.
- **`route_decision` on history records is best-effort**: it's only populated for `chat.completions` requests that go through a router-collection (`x_lemonade_route`) rewrite; direct model requests and the other 4 instrumented routes (completions, embeddings, reranking, responses) leave it `null`. This mirrors the fact that route decisions are a chat-completions-specific concept upstream, not a gap in the history store itself.
- **Web app parity untested end-to-end**: `HistoryPanel.tsx` lives in the shared renderer source (`src/app/src/renderer/`), so it ships in both the Tauri desktop app and the browser-only web app (`src/web-app`) from a single build, same as every other panel — but this was only verified via `tsc --noEmit` and a `webpack` build in this sandbox (no Rust/Cargo toolchain available to build/run the actual Tauri app end-to-end). Same caveat applies to `SystemStatePanel.tsx`.
- **System State's `ai_stack.backends` versions are best-effort**: only `llamacpp` and `fastflowlm` have a live-CLI-query fallback (`resolve_version()` overrides upstream already provides in `system_info.cpp`) when no `version.txt` exists; `vllm`/`openmoss`/`trellis` report `"not installed / version unknown"` whenever neither exists, even if the backend is actually installed. Add `resolve_version()` overrides for those three (same pattern as `llamacpp`/`fastflowlm`) if live-CLI version detection for them turns out to matter in practice.
- **`os_kernel.boot_mode` is a heuristic**, not an authoritative source: it infers systemd-boot/BLS vs GRUB from the presence of `/boot/loader/entries` / `/boot/grub` directories, which can disagree with reality inside containers/chroots where `/boot` isn't the real host `/boot` (confirmed on this fork's own dev sandbox, which reports "UEFI (bootloader undetected)" for exactly this reason — not a bug, just a known limitation of directory-presence detection).
- **`amd-smi`'s `-v`/`--vram` flag is VRAM info, not VBIOS** — the video BIOS/IFWI flag is `-I`/`--ifwi`. Easy to get backwards ("-v" reads like "version"); `system_state.cpp`'s `gpu_vbios` collector was caught and fixed doing exactly this during manual verification on a real Strix Halo dev box, and now parses `amd-smi static -I --json`'s `gpu_data[0].ifwi.version` field specifically, with a `rocm-smi --showvbios` text-parsing fallback.
- **Resource Dashboard's process/port discovery is Linux-only**: `process_probe.cpp`'s `find_processes()`/`resolve_port()` return empty/not-found on non-Linux platforms rather than attempting a Windows/macOS equivalent (no `/proc` there). `/resources/inference` still responds with well-formed `{"process_running": false}`-shaped results in that case — it just never finds anything — rather than erroring, so the endpoint itself is cross-platform-safe even though the detection logic isn't.
- **CPU percent priming is per-process, not per-connection**: the first `/resources/system` poll after server start (not per GUI session) reports `null` per-core percentages, since the delta-based calculation needs a prior `/proc/stat` sample. A GUI opening the Resources tab hours after server start sees real numbers immediately; only a poll landing in the first ~2s window after the server itself started sees the priming nulls.
- **`get_vllm_status()` is weaker than the reference dashboard's**: the Python reference additionally checks `importlib.util.find_spec("vllm")` (i.e. "is the `vllm` package installed, even if not currently running") since it has a Python interpreter to ask. This C++ app has no equivalent to ask, so vLLM detection here is process-presence only (`{"process_running": bool}`) — an installed-but-not-running vLLM won't be reported as installed. Documented as a known gap, not silently different.
