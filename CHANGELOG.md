# Changelog

All notable changes to ROUNDTABLE NLE are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

Targeting v0.2 public release. See `docs/FINAL_CLEANUP.txt` for the
release checklist.

### Added
- **GpuScheduler** — single authority for `vkQueueSubmit` across the
  whole codebase. Replaces 24+ ad-hoc submission sites with one queue-
  ordering point and per-queue mutex enforcement. Resolves a class of
  WDDM TDR crashes seen at shot boundaries.
- **Render graph (Phase 2)** — DAG-based render pass scheduling
  replaces the legacy fallback compositor paths.
- **NVDEC concurrency cap** — limits simultaneous hardware decoders to
  reduce GPU oversubscription at shot boundaries.
- **Triple-buffered GPU work submission** — bounded in-flight composite
  frames with proper fence tracking; eliminates the unbounded-queue
  growth path.
- **Crash recovery dialog actions** — *Copy Error Message*, *Open Logs
  Folder*, and *Report on GitHub* (opens a pre-filled issue with the
  log copied to clipboard) alongside the existing Reset/Continue
  options.
- **Export-in-progress guard** on window close — prompts the user
  before silently killing a running export.
- **Help → Third-Party Licenses** menu — opens
  `docs/THIRD_PARTY_LICENSES.md` with full attribution for all
  bundled and linked components.
- **Installer pre-install VC++ Redist check** — warns before install
  (was: only after) so users can cancel and install the runtime first.
- **Installer writable directory grants** — `{app}\logs`,
  `{app}\projects`, and `{app}\assets` get `users-modify` ACLs so
  non-admin users on Program Files installs can write to them.

### Changed
- Crash logs now land in `<install_dir>/logs/` (was: `%LOCALAPPDATA%`)
  for discoverability. Installer reserves the folder with user-write
  ACLs.
- Vulkan-Assisted and Synchronization validation layers wired in
  Debug builds; fatal-on-error toggle.
- README now uses a Shields.io release badge instead of a hard-coded
  version string.

### Fixed
- Compositor output texture use-after-free under fast scrub
  (commit `462a3df`).
- Vulkan threading violations during Program Monitor teardown
  (commit `e55a1ec`).
- Program Monitor blank-frame: double-lock on `computeQueueMutex`
  (commit `f9d2f8e`).
- Program Monitor blank-frame: false device-lost from recursive
  composite mutex (commit `29b7587`).
- Program Monitor UI echo during drag + Fit-mode padding regression
  (commits `2c13c90`, `1229552`).
- Timeline tool-swap getting stuck via keyboard shortcut + wrong
  shortcut key in Selection Tool tooltip (commit `2d2350f`).
- Timeline drag-marquee selection not selecting clips
  (commit `46c900f`).
- NikkeBKG drag-drop passing only basename instead of full file path
  (commit `d300028`).
- GpuScheduler `QueueSlot` copy-assign error on shutdown
  (commit `6ff5768`).

### Removed
- `vcpkg.json` — vestigial, project uses CMake `FetchContent` + pre-
  built FFmpeg. The file falsely listed `x264`/`x265` as FFmpeg
  features (the bundled binary is LGPL-only with neither enabled).
- Stale developer scratch files: `temp_insert_nikke.ps1`,
  `swap_tabs.ps1`, `fix_json.ps1`, `scratch/`, `assets/temp/`.
- Old installer artifacts in `dist/` (v0.0.0 through v0.16).
- Crash dumps and dev logs in `logs/`.
- Superseded planning docs consolidated into
  `docs/ARCHIVE_HISTORICAL_PLANS.md`.

### Known issues
- **Spine licensing.** spine-cpp ships under the Spine Runtimes License,
  which requires every end user to hold their own Spine Editor license
  unless the developer holds an Editor license and falls under Section
  2 of the Editor License Agreement. Public distribution path for this
  release is pending resolution; see `docs/FINAL_CLEANUP.txt` §B6.
- **AGPL-3.0 vs Spine.** AGPL §10 requires linked dependencies to be
  AGPL-compatible. Resolves alongside the Spine licensing decision.
- **HEVC software encoding is unavailable** — bundled FFmpeg is built
  without libx265. HEVC export requires NVENC / AMF / QSV hardware.
- **Scene detection (OmniShotCut)** — the C++ harness ships, but the
  Python detector + model checkpoint are not redistributed (no formal
  license on the upstream research code). The feature degrades to
  "unavailable" on user installs.

---

## [0.16] - 2026-05-11

Stability and performance push.

### Fixed
- GPU pipeline robustness improvements (Phase 2 of the stabilization
  push): paint recursion guards, use-after-free protection across
  thread-hosting classes.
- Source modularization: several files split below 300 lines.

(Commit: `cdbe595` — *"Stability & performance fixes: GPU pipeline
robustness, paint recursion guards, use-after-free protection, source
modularization"*.)

---

## [0.15] - 2026-05-10

(Internal release. See `git log v0.14..v0.15` for the commit detail.)

---

## [0.14] - 2026-05-07

(Internal release.)

---

## [0.13] - 2026-05-06

### Added
- ChromaKey effect overhaul.
- ColorGrading replacement.
- Shot system improvements.
- Color Matte clip type + New Sequence dialog revamp.

(Commits: `475fe99`, `cf0fd49`.)

---

## [0.12] and earlier

See `git log` — release history before v0.13 was not consistently
tagged.
