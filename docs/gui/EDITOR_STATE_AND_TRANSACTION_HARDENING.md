# Editor-State & Transaction Hardening — de-brittling the SceneEditor

**Status:** PLAN (post-mortem + go-forward). Distilled from eight review
rounds on the snapshot/transaction foundation (`feature/gui-snapshot-prototype`).
**Owner:** Aravind Krishnaswamy
**Why now:** the *remaining* GUI lift — the LLM/MCP agent that "fully operates the
scene," entity creation, the material editor — all drive `SceneEditController`
**programmatically**, which is exactly the caller class whose latent bugs the
reviews kept surfacing. De-brittling the chokepoints *before* that surface lands
makes every future mutator safe by construction instead of a fresh place to
forget a step.

---

## 1. The bug census (why this doc exists)

Across rounds 5–8 the reviews found ~17 distinct defects in the snapshot/undo/
transaction code. They are **not** 17 unrelated bugs — they are four root
patterns, each a structural invitation to the same mistake:

| # | Root pattern | Findings it produced |
|---|---|---|
| **P-STATE** | The transaction baseline is a **hand-assembled list of captured state**, so it is perpetually incomplete. | depth→seq baseline; F7 (dirty + selection not restored); F7-BUG-2 (the *fifth* dirty set `mScaleFromAnchorSet` uncaptured). Every review found one more uncaptured field. |
| **P-WALK** | Edit handling is **duplicated across five parallel walks** (Apply / single-Undo / single-Redo / composite-Undo / composite-Redo). A fix to one must be hand-replicated to four. | F1 (composite walk omitted `SetMaterialProperty`); F4-composite (composite walk cached the first camera); F5/F6 redo paths untested/at-risk. |
| **P-INVALIDATE** | Light/environment **invalidation is scattered** across every mutator — each must remember to bump `mLightTopologyGeneration`. | #2b-a (the original miss); P2a (3 Job mutators); the P2a *extension* (the add/remove-light/object class). |
| **P-FFMATH** | `-ffast-math` (`-ffinite-math-only`) **folds NaN/Inf sentinels**, so `std::nan`-as-not-found tests are false-green. | the keystone false-green, repeated in **3** test files. |

P-FFMATH is now mechanically prevented (`tests/SourceHygieneTest.cpp`,
`tools/red_prove.sh`, the `write-highly-effective-tests` skill). This doc
addresses the three *design* patterns (P-STATE, P-WALK, P-INVALIDATE).

The unifying lesson: **brittleness = state/logic/invalidation that is replicated
instead of owned by one chokepoint.** The fix for each is the same shape — make
there be exactly one place.

---

## 2. Phase H1 — one owned editor-state snapshot (fixes P-STATE)

**✅ SHIPPED (commit `47744de7`)** — behavior-identical; `EditorStateSnapshot` + `Capture/RestoreEditorState` are the single capture/restore path; `TestEditorStateSnapshotRoundTrip` round-trips dirty + the SFA 5th-set + selection. H2 and H3 (light-set slice) shipped too.  **Post-review (3 adversarial lenses):** correctness confirmed behavior-identical; extended the snapshot to own the FULL selection state (per-category arrays + section-expanded, B-gap) and added listener-fire + cross-category coverage (C-gap). The forward "impossible to forget" guarantee is PARTIAL -- see the dirty-vs-selection note below.

**Current brittleness.** A transaction baseline is three separate controller
members (`mTxnBaselineSeq`, `mTxnBaselineDirty` (itself recently widened from
`DirtyTracker::State` to `DirtySnapshot` to add the 5th set), `mTxnBaselineSelCat`
/`mTxnBaselineSelName`). `HasUnsavedChanges()` reads a *different* hand-listed set
of sources. Adding any new mutable editor state means remembering to extend both
lists — and the reviews proved nobody remembers.

**Target.** One value type that owns ALL transactionally-relevant editor state:

```
struct EditorStateSnapshot {                 // AS SHIPPED
    unsigned long long      historyMarker;       // EditHistory::NextSeq()
    DirtySnapshot           dirty;               // 4 tracker channels + the 5th SFA set
    Category                selectionCategory;   // primary tuple
    String                  selectionName;
    std::vector<String>     selectionByCategory; // FULL per-category selection memory
    std::vector<bool>       sectionExpanded;     // per-category panel-section expand state
};
```

- `SceneEditor::CaptureEditorState()` / `RestoreEditorState(const&)` are the ONLY
  capture/restore path. `BeginTransaction` = capture; `RollbackTransaction` =
  restore (then the existing inverse-edit replay down to `historyMarker`).
- **Single source of truth for "is anything dirty":** `HasUnsavedChanges()` and
  the snapshot both read `DirtySnapshot`, so a new `DirtyTracker` channel flows
  into both automatically. **Honest scope (B-gap2):** this auto-flow holds for
  the DIRTY side only; the SELECTION side of `Capture`/`RestoreEditorState` is
  hand-written field-by-field, so a new selection-ish member is NOT compile-time-
  forced into the snapshot. The originally-promised `static_assert` is not
  feasible for the selection arrays; the guard is the behavioral round-trip +
  per-category + listener tests (`TestEditorStateSnapshotRoundTrip` /
  `TestRollbackRestoresPerCategorySelection` / `TestRollbackFiresDirtyListener`).
  H2's single applier is where the remaining forward-guarantee should land.

**Payoff.** The entire P-STATE class disappears: "found another uncaptured set"
becomes structurally impossible. ~Medium effort; mostly consolidating members
that already exist.

---

## 3. Phase H2 — one applier + an inverse (fixes P-WALK)

**✅ SHIPPED Stages 1+2+3 (commits `f5560ae8` + coverage + `8fceb4c8` Stage 3 + review).** P-WALK fully closed. The composite-Undo/Redo walks were ~180-line hand-maintained twins of the single paths; they now LOOP over two shared dispatchers `ApplyForwardMutation` / `ApplyRevertMutation` (each = the single-edit body verbatim), so single and composite can never drift again, and (Stage 3) `Apply`'s forward mutation now routes through `ApplyForwardMutation` too. **-469 lines total.** 3-lens adversarial review: behaviour-identical (A, byte-for-byte bodies), complete (B, all 23 ops, no null/scope/notifier regression), drift bug class killed by construction (C); two surfaced coverage holes (SetSceneTime arm, composite `LastDirtyScope` aggregation) closed + RED-proven.

**Shipped form vs the heading:** two *direction* dispatchers, NOT `Inverse(edit)->edit`. The forward half was already centralized in `ApplyObjectOpForward`/`ApplyCameraOpForward`; a total `Inverse` would need new ops (`RemoveCamera`, clear-binding, set-transform-matrix) since AddCamera's inverse is a remove and transform inverses are state-restores -- more invasive for marginal gain.

**Stage 3 (SHIPPED, `8fceb4c8`):** `Apply` is now `CaptureForApply` (resolve + every
rejection gate + prev* capture) + `ApplyForwardMutation` (the SHARED mutation + side
effects + scope) + push. Forward mutation lives in EXACTLY one place, reused by Apply,
single-Redo, and the composite-redo loop. Adding an op is now three single-purpose touch
points: capture / forward-mutate / revert. Two-lens review confirmed behaviour-identical
(all 30 rejection gates preserved; camera resolution stable since pActiveCamera ==
GetItem(activeCameraName)); the only drifts were two log-only diagnostics, restored.
`CaptureForApply` RED-proven load-bearing.

**Benign asymmetry (A1, NOT changed):** `SetObjectGeometry` undo marks only the per-category Object dirty channel (via `MarkEditEntityDirty`), not the mNames transform channel that forward marks -- harmless (dirtiness is tracked either way) and pre-existing; left per the no-unprovable-change rule.

**Current brittleness.** Five walks (`Apply`, single `Undo`, single `Redo`, the
composite `Undo` walk, the composite `Redo` walk) each switch over the op kinds.
A new op, or a fix to an existing op, must be made in up to five places; the
reviews kept finding the one that was missed (composite omitted material-slot;
composite cached the camera).

**Target.** Collapse to **one applier**:

- `Inverse(const SceneEdit&) -> SceneEdit` produces the undo edit (it has all the
  captured `prev*` state today).
- `Undo` = `Apply(Inverse(edit))`; `Redo` = `Apply(edit)`; a composite is just a
  list replayed through `Apply` in the right direction. There is then exactly ONE
  switch over op kinds.
- Per-op side effects (light-gen bump, dirty marking, spatial invalidation,
  camera regen) live once, inside `Apply`, keyed off the op + resolved target —
  not duplicated per walk.

**Payoff.** "Composite forgot op X" and "walk N resolved the wrong entity"
become impossible — there is no walk N. This is the single biggest de-brittling
move. **Larger effort** (touches the core of `SceneEditor.cpp`); do it as a
deliberate phase with the full editor test suite as the guard, not under fire.
Sequence it AFTER H1 (a clean snapshot makes the refactor safer to verify).

---

## 4. Phase H3 — one light-topology invalidation chokepoint (fixes P-INVALIDATE)

**✅ SHIPPED — NARROWED scope (commit `a5b38882` + fail-loud follow-up).** What shipped:
the light SET self-invalidates -- `LightManager::AddItem`/`RemoveItem` fire a Job-installed
callback that bumps `Scene::BumpLightTopologyGeneration`, so every light add/remove (present
AND future) invalidates automatically; the 5 explicit Job light bumps are gone.  Renderer
read-side UNCHANGED (one counter); ABI-safe (concrete setter + dynamic_cast, no
`ILightManager` vtable change; fail-loud `else` if the cast ever nulls); over-bump-safe.
The CONDITIONAL cases were deliberately NOT automated -- emissive object add/remove,
material rebind, emission-scale, environment replace/scale, and in-place light-property
edits stay explicit (`BumpSceneLightGen` / SceneEditor helpers) because they are inherently
mutation-layer DECISIONS ("did this object become emissive?"), not forgettable unconditional
bumps; automating them would over-invalidate (every object add rebuilding light samplers) or
need object/material-layer firing -- a larger renderer-touching change not justified for a
currently-correct path.  The broad "object/material layer fires + one
`NotifyLightTopologyChanged` funnel" Target below is thus the ROAD-NOT-TAKEN (the funnel name
was not introduced; `BumpLightTopologyGeneration` remains the funnel).  2-lens review: no
under-invalidation, scope claim accurate; RED-proven ([h3rm] RemoveLight + [p2a2] add).

**Current brittleness.** "An emitter/env changed → bump `mLightTopologyGeneration`"
is open-coded at ~a dozen mutation sites across `SceneEditor` and `Job`. Each new
mutator is a fresh chance to forget; the reviews found three classes that did.

**Target.** Move the responsibility from *callers* to the *managed state*:

- The light manager fires on add/remove; the object/material layer fires when an
  object's emitter set changes (emissive↔non-emissive bind, exitance edit,
  spatial change of a luminaire); environment replacement/scale fires. One
  funnel: `Scene::NotifyLightTopologyChanged()`.
- Callers stop hand-bumping. New mutators get correct invalidation for free.
- Alternative considered: a content-hash computed at the `RayCaster::AttachScene`
  consumer instead of push-bumps — rejected for now (per-attach hashing cost),
  but revisit if the funnel proves leaky.

**Payoff.** The P-INVALIDATE class closes. ~Medium effort; mechanical once the
funnel exists. Sequence anytime, but it pairs naturally with H2 (both touch the
per-op side-effect sites).

---

## 5. Process guardrails already codified (do not regress these)

- **RED-prove every bug-fix test** (`tools/red_prove.sh`): see it fail without
  the fix before claiming done. Writing the test after the fix proves nothing.
- **No `-ffast-math`-foldable sentinels in tests** (`tests/SourceHygieneTest.cpp`
  enforces it). Use a finite poison or an explicit existence `Check`.
- **Sweep ALL siblings** when fixing a pattern (`audit-by-bug-pattern` skill) —
  the keystone disease reached three files because two-of-three was called done.
- **Adversarial review is a GATE, not a postscript** (`adversarial-code-review`
  skill): run the correctness / completeness / test-integrity lenses BEFORE
  declaring a correctness-sensitive change done. It found real bugs every round.
- **Report status honestly:** "tests X pass, RED-proven; *unverified:* Y." Never
  "complete / no P1 / sound" — that claim was wrong every time it was made here.

---

## 6. Recommended sequencing

1. **H1 (EditorStateSnapshot)** — first; consolidates existing members, immediate
   P-STATE win, makes H2 safer to verify.
2. **H3 (invalidation chokepoint)** — independent; closes P-INVALIDATE; do
   before/with the MCP surface so agent-driven mutators are safe.
3. **H2 (single applier + inverse)** — the big one; deliberate phase, full
   editor-suite guard, ideally its own review gate.

Each phase ships behind the existing `tests/SceneEdit*Test.cpp` +
`SamplerRebuildOnRestoreTest` + `SceneSnapshotTest` suite, RED-proven per fix,
and gated by an adversarial review before "done."
