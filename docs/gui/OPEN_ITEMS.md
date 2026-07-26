# GUI / Editor — Open Items

**Purpose:** the machine-independent work ledger for the `gui-render-modes-p2`
branch (and its successors).  Session-local agent memory does not travel
between machines; this file does.  Keep it current: remove items when they
land (cite the commit), add items when a review round leaves something
open.  History and rationale live in
[PROPERTIES_AND_REFERENCES.md](PROPERTIES_AND_REFERENCES.md) §6–8 and
[ADR_DOCUMENT_FIRST_EDITOR.md](ADR_DOCUMENT_FIRST_EDITOR.md).

_Last updated: 2026-07-25, after the round-8 independent review of the viewport work._

---

## 0. Viewport render-mode follow-up — DONE

- **T0 — DONE:** fixed the gesture-pinned scheduler spin exposed by
  BeautyVariant + object-gizmo interaction.  Dirty frozen siblings no longer
  self-arm an infinite stream of passes on the pinned pane.  Pointer-up polish
  ownership is now atomic with release; dropped property-End and pointer-Up
  lifecycle paths (including edits begun while paused) resume deferred panes;
  repeated Stop remains prompt during coordinated renders while plain Pause
  preserves owed polish; pane-0 mode/pose/exit setters cannot switch live
  registers during a secondary gesture; and layout shrink no longer confuses a
  stale pointer pane with a property scrub or drops a concurrent scrub's motion
  divisor.  The expanded
  `ViewportPaneSchedulerTest` scenarios execute the real low-resolution
  indirect pipeline and are mutation-verified; a Mac GUI
  `Quad → indirect → Move` reproduction established the original failure.
- **T1 — DONE:** the applied-once Mac and Windows layout preset now reveals
  pane 1 as `indirect`, pane 2 as `facets`, and (in Quad) pane 3 as `direct`.
  The existing success-only guard remains load-bearing: refused setters retry,
  while every explicit user choice—including Preview—survives later layout
  toggles.
- **T2 — DONE:** additive `SceneCameraNamed` vantage kind 3 binds panes 1–3
  to any manager camera without activating it.  Desired state stores the name
  plus a snapshot fallback, reconcile re-resolves the live camera, matching
  camera edits (including kind-omitted agent edits, undo/redo/rollback, and
  animated time evaluation) invalidate bound panes, and deletion preserves
  the fallback until same-name recreation wakes it. Authored ONB pinholes are
  captured as ray-equivalent navigable pinhole overrides, and binding parks
  any in-flight preview before reading manager-camera state. The C ABI and
  both shells expose the same fail-closed semantics without truncating camera
  identity; pane 0 deliberately refuses this kind.
- **T3 — DONE:** named-camera pane navigation follows the bound camera
  through the existing name-targeted edit/undo path; the contract is ratified
  in `RENDER_MODES.md` §7.2.
- **T4 — DONE:** `LastRender` is a persistent pane content source outside
  the render-mode registry.  The controller retains one owning,
  full-resolution deep copy across scene/gizmo edits, excludes frozen panes
  from scheduling, and publishes each successful GUI-production or agent
  completion exactly once to every subscriber.  Failed/cancelled renders
  preserve the prior copy; render-before-subscribe delivery and the
  transparent pre-first-render placeholder are regression-locked.  The
  additive C ABI, agent pane introspection, and both desktop shells expose the
  same source semantics.  Review remediation and mutation evidence are
  recorded in `RENDER_MODES.md` §7.9.

## 0b. Round-8 independent review of §0 — three P2s fixed (2026-07-25)

An independent four-reviewer round over the §0 arc found ZERO P1s but three
P2s that §0's own review round had missed (its "ZERO P1/P2" claim was
optimistic).  All three are fixed, each mutation-verified:

1. **Lost pointer-up froze the sibling panes.**  T0's self-arm suppression
   removed the only thing keeping siblings alive when a gesture flag never
   clears — reachable live, because both shells drop the pointer-up when a
   chat/agent render flips `interactionEnabled` while the core-side render is
   separately refused (so no teardown path runs either).  Added the
   `kPointerWatchdogMs` (10 s) arm + `mPointerGestureStale`: the pin and the
   arm-suppression lift, but `mPointerDown` stays SET so a late pointer-up
   still closes its composite.  `RENDER_MODES.md` §7.3.
2. **`~SceneEditController` reached `mJob`.**  T0's orphaned-gesture cleanup
   commits pending CST (and `EndComposite` probes `mJob` too), and the dtor
   reaches it via `Stop()` — violating the dtor's own documented no-`mJob`
   invariant, which exists because it previously caused a SIGSEGV.  The dtor
   now sets `mInDestructorTeardown` before `Stop()` and the cleanup is
   skipped there.  Known, accepted: pending CST edits are dropped on
   destruction rather than committed (matches pre-T0 behavior).
3. **Last Render published under `mMutex`** — ADR rule 5 violation plus a
   UI-thread hitch.  Split into record-under-lock / drain-after-unlock.
   `RENDER_MODES.md` §7.3.

Also fixed while stress-testing: `ViewportPaneSchedulerTest`'s `WriteTemp`
used fixed `/tmp` paths, so two concurrent instances clobbered each other's
scene files (3 of 12 concurrent runs failed as a bogus "fixture constructs");
it now suffixes the pid.  The shared `Fixture` neutralizes the pointer
watchdog so the pin-contract scenarios stay wall-clock-independent; w1 opts
back in with a short threshold.

A second round-8 review pass over those three fixes found a P1 IN THE FIX
itself plus six P2s; all are addressed, except two deliberately deferred
(below).  The P1: the watchdog gated on `mLastEditTimeMs`, which
`OnPointerMove` stamps only AFTER its render-completion wait — so on a
BeautyVariant pane (pinned divisor ⇒ every in-drag quantum is a full
fixed-spp + non-cancel-interruptible OIDN pass) a single pass could outlast
the window and freeze a genuinely active drag.  Fixed by separating pointer
ACTIVITY (`mLastPointerActivityMs`, stamped at the top of `OnPointerMove`
before any wait, and at gesture start under the same lock that publishes
`mPointerDown`) from EDIT COMPLETION, plus an in-flight-move stand-down
(`mPointerMovesInFlight`).  Also fixed from that round: the activity clock is
steady-time-since-BOOT so a zero-initialized read exceeds ANY finite
threshold — the stamp now happens inside the gesture-start lock, and `<= 0`
is an explicit watchdog disable (a "very large" threshold does NOT disable
it, which had made the test fixture's neutralization a no-op); the
`RefreshProperties` catch-all drain is non-blocking (it must never stall the
UI thread); both pane setters release `mRenderAdmissionMutex` — not just
`mMutex` — before delivering; the drain is exception-safe; a queued frame is
purged when a pane leaves LastRender; a late pointer-up no longer writes its
polish marker into a sibling's live registers; and the test's `getpid()` use
is Windows-guarded (a bare `<unistd.h>` would have broken the MSVC test
build).  Pinned by scenario w4 (an active drag across passes 3× longer than
the watchdog window).

DEFERRED from that round, both documented in code:
- **Destructor CST commit when the Job outlives the controller.**  Skipping
  the cleanup is right when `mJob` may be dead, but the flag cannot tell the
  two apart, and the normal shell teardown (window close mid-gizmo-drag)
  DOES have a live Job — so that drag's final delta stays in the live scene
  and never reaches the Document.  Matches pre-T0 behavior and is strictly
  better than the UAF it replaces.  Real fix: an explicit
  `PrepareForDestruction()` that the C-ABI destroy shims call while the
  caller can still vouch the Job is alive.
- **The watchdog does not break the render-refusal cycle it cites.**  It
  restores sibling-pane rotation only; admission still refuses on raw
  `mPointerDown` + `IsCompositeOpen()`, both true by design.  Breaking that
  cycle means finalizing a gesture from the render thread.

**Pre-existing test flakiness (measured, NOT introduced here):**
`ViewportPaneSchedulerTest` flakes at roughly 1 run in 10, always on a
timing/"settles"/"quiesces" assertion (a different scenario each time).
Measured at HEAD `b1e38e4e` with the round-8 changes stashed: 1 failure in
10 runs ("initial rotation settles"); with them applied: 1 in 10
("concurrent-shrink recovery quiesces", and separately scenario (n)).  Same
rate, same class, so the fixes neither cause nor worsen it — but the settle
assertions want a retry/backoff before this file is trusted in CI.

Round-8 P3s NOT acted on: `RENDER_MODES.md` claimed "12 focused failures" on
removing the T0 guard (measured 14); the T0 scrub watchdog now also snaps the
preview scale mid-scrub; a Last Render pane keeps showing the previous
scene's frame across a scene-variant switch with no staleness cue; T1's
layout-default preset has NO automated coverage (it is pure GUI-shell code,
and the Windows side is review-verified only).

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

§0 and §1 are clean.  The T4 gate was re-established on 2026-07-24: a
warning-free clean core rebuild, all 220 standalone tests, a clean arm64 Mac
GUI build, mutation verification of every new T4 assertion, and a fresh
three-reviewer adversarial round reporting ZERO P1/P2.  (x86_64 Mac links
fail against arm64 Homebrew dylibs — environmental, not a regression.  The
Windows-shell build/run pass in §4 remains outstanding for merge
consideration.)
