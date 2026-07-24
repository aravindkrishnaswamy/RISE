# GUI / Editor — Open Items

**Purpose:** the machine-independent work ledger for the `gui-render-modes-p2`
branch (and its successors).  Session-local agent memory does not travel
between machines; this file does.  Keep it current: remove items when they
land (cite the commit), add items when a review round leaves something
open.  History and rationale live in
[PROPERTIES_AND_REFERENCES.md](PROPERTIES_AND_REFERENCES.md) §6–8 and
[ADR_DOCUMENT_FIRST_EDITOR.md](ADR_DOCUMENT_FIRST_EDITOR.md).

_Last updated: 2026-07-23, at commit `85e1b9bd`._

---

## 1. P2 sweep (from the round-6 three-reviewer pass)

Verify each against current code before fixing — line numbers drift.

1. **Camera absent-name fallback edits the wrong camera on a typo.**
   `uniqueFallback` for `kind == "camera"` is unconditional on the name
   (`Job.cpp` `ApplyCstParamEditImpl_` / `ApplyCstParamRemoveChecked`;
   `SceneEditController.cpp` `CaptureAgentPriorParamValue_` /
   `CaptureAgentChunkForRemoveUndo_`; `ResolveSourceChunkId`).  Scene with an
   unnamed `pinhole_camera` + `thinlens_camera { name tele }`: an agent edit
   naming `tel` (typo) matches nothing, falls back, and silently edits the
   unnamed pinhole.  Tighten: fall back only when the document has exactly
   one camera chunk total, or when the name matches the live active camera's
   registered name.  Pin both directions with a test.
2. **Stale comments assert the PRE-fix resolver contract** ("a unique name
   match returns regardless of the kind suffix"): `Job.cpp` ~10342–10367 and
   ~11026–11030, `CstIntrospection.cpp` ~150–156.  Update to the
   kind-constraint + registry-classifier semantics (`85e1b9bd`).
3. **Destructor quiescence-barrier ordering.**  `~SceneEditor`'s
   callback-quiescence wait runs after `~SceneEditController`'s body and
   after later-declared mutexes are destroyed — a cross-thread
   `RISE_API_DestroySceneEditController` racing a synchronously re-entrant
   dirty callback can touch destroyed mutexes.  In-tree shells are immune
   (both marshal async), but the C-ABI advertises hardened re-entry.  Move a
   wait-for-quiescence to the TOP of `~SceneEditController`.
4. **Undocumented refusal returns on `OnTimeScrub{Begin,,End}` exports.**
   `RISE_API.h` still documents them as "Time scrubber."; `false` now also
   means refusal (render-owns-scene / coordinated-render gate), not just a
   null controller.  Document it.
5. **Test holes in `tests/GeometryPanelJumpTest.cpp`:** (a) no POSITIVE
   cross-category narrowing assertion (geometry `x` + material `x`,
   `kind=material` must edit the material — the refusal side is pinned by
   T14, the success side is not); (b) named-chunk + unnamed-singleton
   coexistence for empty-target addressing is unpinned (currently resolves
   the unnamed one — decide intended vs refusal, then pin it).
6. *(P3, if cheap)* `DocFindByNameAnyRole` reports `occurrences`
   pre-narrowing, so `ApplyCstRemoveChunk` diagnoses "2 chunks named x"
   (ambiguous) when the truth is "no chunk of that kind".

## 2. Document-first phase 3b (deliberately deferred — ADR §4)

- **Film arm** migration onto the transaction path (post-clamp value
  recording + FrameStore/camera-resync side-effects need their own slice).
- **Film preset pick** (two-param atomic edit; the transaction path is
  single-param).
- **Environment arm** (`radiance_map` erase is a multi-param REMOVE the
  single-param path cannot express).
- All three are coherent today (pre-flight + gated dirty marks); migration
  deletes their bespoke sequences and buys undo, as it did for Rasterizer.

## 3. Document-first phase 4 (deferred until inverse-op pain recurs)

Document-state undo (N retained heads; undo = swap head + re-derive),
retiring the per-op inverse machinery.  Needs the gesture-coalescing design
first.  See ADR §4.

## 4. Other known follow-ups on this branch's arcs

- **Bridge enum translation switches** are still convention-guarded — the
  remaining class-C mirror; candidate for the same manifest treatment as
  `tests/IJobVtableManifest.txt` (SourceHygieneTest).
- **Windows shell**: all branch work is code-complete + review-verified
  only; needs a real Windows build/run pass before or at merge.
- **Standalone geometry creation templates** (`TemplatesFor(Geometry)`
  empty) — ENTITY_CREATION.md §4.2's import/primitive-set design.
- **Shader / Modifier UI categories** — no `IJob` enumeration surface yet;
  one-arm additions to the generic inspector once that lands
  (ENTITY_CREATION.md §3.1).

## 5. Merge gate

When §1 is clean and a fresh adversarial round returns zero P1s: full suite
(220/220), warning-free core, Mac arm64 GUI build, then the user decides the
merge to master.  (x86_64 Mac links fail against arm64 Homebrew dylibs —
environmental, not a regression.)
