# Native Spine Runtime — Deferred

This directory holds the in-progress work on a custom `.shotanim` runtime
that was originally going to replace `spine-cpp` (see `PARSER_PLAN.txt` at
the repo root for the original motivation).

**Status as of 2026-05-15:** deferred. ROUNDTABLE ships with `spine-cpp`
as the production renderer because the project owner holds a Spine Editor
license, and Section 2 of the Spine Editor License Agreement permits
shipping the runtimes inside this Product (a video NLE that adds
significant primary functionality on top of the runtimes). See
`docs/THIRD_PARTY_LICENSES.md` for the full notice.

The native runtime is not built by the main CMake. Nothing in
`src/` imports it. The dual-runtime wiring that previously lived in
`SpineEngine` / `SpineAnimation` / `CharacterBrowserNetwork` was removed
when this work was stowed.

## Layout

```
tools/native_engine/
├── README.md               (this file)
├── crown_test.bat          A/B test launcher: env-vars + launch app
├── src/
│   ├── SkeletonTypes.h     Pure-data skeleton package (rt::SkeletonPkg etc.)
│   ├── ShotAnimFormat.h    Binary layout, magic numbers, version (currently 5)
│   ├── ShotAnimWriter.{h,cpp}     spine::SkeletonData → .shotanim
│   ├── ShotAnimReader.{h,cpp}     .shotanim → rt::SkeletonPkg
│   ├── ShotAnimConvert.{h,cpp}    .skel + .atlas → .shotanim helper
│   └── NativeSpineEngine.{h,cpp}  Per-frame evaluator + mesh extractor
└── tools/
    ├── shotanim_parity.cpp        spine-cpp ↔ native diff harness
    └── migrate_shotanim.cpp       Bulk re-converter for the asset library
```

## What works

- Format v5 binary I/O round-trips for bones / slots / attachments /
  skins / IK / transform constraints / animations / draw order / deform.
- Region attachments, rigid + skinned mesh attachments, clipping masks.
- Bone world-transform computation (full spine 4.1 inherit-mode switch
  ported verbatim — Normal / OnlyTranslation / NoRotationOrReflection /
  NoScale / NoScaleOrReflection).
- IK solver for 1-bone and 2-bone chains, with animated mix.
- Transform constraints — full spine 4.2 model: per-axis mixes
  (mixX/mixY/mixScaleX/mixScaleY/mixShearY), constant offsets
  (offsetRotation/X/Y/ScaleX/ScaleY/ShearY), absolute/relative & world/
  local apply modes, animated mix from transformTracks.
- Setup-fallback gates on every timeline (rotate/translate/scale/shear/
  color/attachment/draw-order/IK-mix/TC-mix/deform): values fall back to
  setup pose when `time < frames[0]`, matching spine semantics.

## What's left (the reason this was deferred)

1. **Path constraints — completely unimplemented.** Every Nikke
   character has 1-7 of them (Crown has 6). They translate and rotate
   bones along a Bezier path attached to a slot. The writer doesn't
   serialize them, the reader doesn't parse them, and the evaluator
   doesn't apply them. This is the dominant residual divergence in
   the parity tool — bones that share a parent constrained by a path
   show identical world-position offsets of 15-50 units, with matrix
   correct (the path translates, doesn't rotate in `Chain` mode).
   Spine source: `third_party/spine-cpp/src/spine/PathConstraint.cpp`
   (~200 lines). Needs new fields in `rt::SkeletonPkg`, format bump
   v5 → v6, writer + reader + native solver.

2. **Vertex-count mismatch (~126 verts on Crown idle@0).** Native
   renders 126 more verts than spine for the same attachment selection.
   Suspected clipping range or attachment vertex data difference. See
   the parity tool's slot-attachment diff output for slot-level
   investigation.

3. **TC `local` mode shearY accumulator.** Spine modifies `bone._shearY`
   while passing the OLD `shearY` to `updateWorldTransform`. The native
   port modifies `shyL[bi]` and immediately re-resolves. Functionally
   close but not a verbatim match — observed delta is small.

4. **Path-constraint timelines** (writer logs "skipped — not in audit").

## How to resume

1. **Re-enable the build:** restore the two CMake options in
   `CMakeLists.txt` (`ROUNDTABLE_HAS_NATIVE_SHOTANIM`,
   `ROUNDTABLE_USE_NATIVE_SHOTANIM`), restore the matching
   `target_compile_definitions` block in `src/core/CMakeLists.txt`,
   add the source files back to the `roundtable_core` `target_sources`
   list, restore the `add_executable(shotanim_parity …)` and
   `add_executable(migrate_shotanim …)` blocks.

2. **Restore the dual-runtime wiring:** in `src/core/spine/SpineEngine.h`
   re-add the forward decl, `m_nativeEngine` member, and the four
   debug accessors (`extractMeshesSpineCpp`, `debugBoneWorldSpineCpp`,
   `debugSlotAttachmentsSpineCpp`). In `SpineEngine.cpp` re-add the
   native dispatch in `extractMeshes` / `getBounds` / `setSkin` /
   `setPosition` / `setScale`, and the parallel-engine load in both
   load paths. In `SpineAnimation.{h,cpp}` re-add `setNativeMirror` /
   `m_nativeMirror` and the mirror calls. In
   `CharacterBrowserNetwork.cpp` re-add the `convertCharacterDirectory`
   auto-conversion on download.

3. **Move the source back:** `tools/native_engine/src/*` →
   `src/core/spine/` (and `SkeletonTypes.h` → `src/core/skeleton/`).
   Move the tool files back to `tools/`.

4. **Implement path constraints** (the actual blocker). After that,
   `ROUNDTABLE_USE_NATIVE_SHOTANIM=ON`, run `migrate_shotanim
   assets/characters` to bump every .shotanim to the new version,
   then `shotanim_parity assets/characters` and chase whatever
   per-bone divergence remains.

## Diagnostics built into the runtime

- `ROUNDTABLE_NATIVE_NO_IK=1` — disables the IK solver
- `ROUNDTABLE_NATIVE_NO_TC=1` — disables transform constraints

`crown_test.bat` is a menu-driven launcher that combines these.

## Lessons learned (read before re-attempting)

- **Format-version bumps are easy; the data model is the hard part.**
  Each "we forgot to store X" round took a writer + reader + runtime
  change and a full asset re-conversion. Front-load the data audit:
  before writing any code, enumerate every getter on every spine
  ConstraintData / TimelineData class and decide which fields you need.

- **The parity tool is non-negotiable.** The TC bug was invisible until
  the per-bone diff was wired up. Without it, "looks roughly right" was
  hiding 120-unit position errors on hair/sword chains.

- **`extractMeshesSpineCpp()` exists for a reason.** When you re-enable
  dual-runtime, the parity tool's spine-side MUST use a path that
  bypasses native dispatch — otherwise it compares native against
  itself and reports zero delta forever.
