# GUI / Editor — Open Items

**Purpose:** the machine-independent work ledger for the `gui-render-modes-p2`
branch (and its successors).  Session-local agent memory does not travel
between machines; this file does.  Keep it current: remove items when they
land (cite the commit), add items when a review round leaves something
open.  History and rationale live in
[PROPERTIES_AND_REFERENCES.md](PROPERTIES_AND_REFERENCES.md) §6–8 and
[ADR_DOCUMENT_FIRST_EDITOR.md](ADR_DOCUMENT_FIRST_EDITOR.md).

_Last updated: 2026-07-24, at commit `ce629e26` (the round-7 P2 sweep)._

---

## 1. P2 sweep (from the round-6 three-reviewer pass) — DONE (round 7, 2026-07-24)

All six items landed in `ce629e26`; a fresh 3-reviewer
adversarial round on the sweep returned ZERO P1s (one review-found P2 —
the destructive remove-side test hole — was fixed and mutation-verified
in the same round).

1. **Camera absent-name fallback** — gated via
   `Cst::DocCameraUniqueFallbackPermitted` (empty name / active camera's
   registered name / single-camera document); wired into
   `ApplyCstParamEditImpl_`, `ApplyCstParamRemoveChecked`,
   `ApplyCstRemoveChunk`, both SceneEditController capture mirrors,
   `ResolveSourceChunkId`, `EntitySourceLocation`, `DuplicateEntity`.
   The three registry-name-only camera sites (`ApplyCstCameraPoseEdit`,
   `ApplyCstRemoveCameraChunk`, `ApplyCstDeleteCameraChunk`) are
   deliberately ungated — documented at the pose-edit site.  Pinned by
   GeometryPanelJumpTest T21 (typo refusal, edit + destructive remove),
   T22 (active-name + empty-target positive), T23 (single-camera rule);
   all mutation-verified.
2. **Stale resolver-contract comments** — updated to the kind-constraint +
   registry-classifier semantics; the narrowing predicate is now the shared
   `Cst::RoleMatchesKindConstraint`, also used by the remove-chunk kind
   re-verify and CstIntrospection's defensive check (which previously
   suffix-only-refused registry-classified kinds, e.g. function-as-painter).
3. **Destructor quiescence barrier** — `~SceneEditController` now detaches
   the dirty listener (a quiescence barrier) as its FIRST statement.
4. **`OnTimeScrub{Begin,,End}` refusal returns** — documented in
   `RISE_API.h` (verified claim-by-claim against the implementations).
5. **Test holes** — T20 (positive cross-category narrowing) + T22
   (named-chunk + unnamed-singleton coexistence pinned as INTENDED
   resolve-the-unnamed).
6. **`occurrences` post-narrowing** — `DocFindByNameAnyRole` now reports the
   post-narrowing count when a kind constraint is supplied; wrong-kind
   removals report not-found instead of a bogus ambiguity (T24).

Known-accepted narrowing from item 1: addressing a NON-active unnamed
camera by its runtime registry name ("default") in a multi-camera document
now refuses (reveal/edit) — the (Camera, "") active-camera convention and
named cameras are unaffected.  P3s noted by the round-7 reviewers, not
acted on: `OnTimeScrub`'s Apply-refused path cancels the in-flight pass
without re-kicking (near-unreachable; next event recovers); the round-7
destructor barrier has no dedicated cross-thread regression test
(nondeterministic to pin; T17-T19 cover the adjacent contracts).

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

§1 is clean (round 7): fresh adversarial round with zero P1s, full suite
220/220, warning-free clean core rebuild, Mac arm64 GUI build succeeded.
The branch is ready for the user's merge-to-master decision.  (x86_64 Mac
links fail against arm64 Homebrew dylibs — environmental, not a
regression.  The Windows-shell build/run pass in §4 remains outstanding
for merge consideration.)
