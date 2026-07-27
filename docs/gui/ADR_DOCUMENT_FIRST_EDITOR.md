# ADR: The Document-First Editor

**Status: ACCEPTED 2026-07-22** (phases 1–3 landing with this commit; phase 4 deferred).
**Decision owner:** Aravind.  **Context:** the GUI-redesign review arc
([PROPERTIES_AND_REFERENCES.md](PROPERTIES_AND_REFERENCES.md) §7) — four
adversarial review rounds produced 12 P1s on one slice, and 11 of them
were instances of two structural seams, not independent mistakes.

---

## 1. Decision

**The retained CST Document is the UI's only truth.  The live scene is the
renderer's private, derived build artifact.  All edits are Document
transactions; all UI reads are published immutable snapshots; dirty and
undo are derived from Document state, not maintained beside it.**

The compiler analogy is exact and is the intended mental model: the CST is
source code, the live scene is the build output.  Nobody edits build
artifacts and back-ports the change into the source — an editor arm that
mutates live state and then "remembers" to record + mark dirty is doing
exactly that, and four review rounds showed it cannot be done reliably by
convention.

## 2. Why (the evidence)

The 12 P1s cluster (full table in PROPERTIES_AND_REFERENCES.md §6–7):

- **Class A — dual-representation incoherence (6):** live scene, CST, and
  the dirty tracker updated by hand-rolled per-arm sequences.  Every
  permutation of "missed one leg" occurred.  The arms already routed
  through the `ApplyAgentParamEdit` transaction path had **zero** such
  bugs.
- **Class B — advisory locking (5):** `mMutex` guarded the managers but
  nothing made readers hold it; each read surface invented a discipline
  (none / blocking / try) and each invention minted a new UAF or deadlock.
- **Class C — comment-enforced conventions (3):** append-only vtable,
  mirrored enum literals — violated exactly when the comment wasn't
  re-read.  Orthogonal to this ADR; fixed by mechanical guards
  (`SourceHygieneTest` vtable manifest, public `kNumCategories`).

## 3. The rules (what a reviewer should enforce from now on)

1. **One write funnel.**  Every scene mutation on a CST-retained scene
   goes through a Document transaction (`ApplyAgentParamEdit` /
   `ApplyCstParamEditChecked`-class): validate → apply to Document →
   derive → publish.  A new editable surface must NOT introduce a bespoke
   live-mutate-then-record sequence.  If the transaction path cannot
   address the target (as was true for unnamed singleton chunks), extend
   the path — do not bypass it.
2. **Refuse what cannot persist.**  An edit the Document cannot carry
   (unrecordable target, dangling reference, forward reference) is
   REJECTED with a diagnostic before any live mutation.  "Renders now,
   lost on save" is never an acceptable outcome.
3. **Dirty is derived.**  `hasUnsavedChanges` is floored by
   `headVersion != savedHeadVersion` — a Document commit after the last
   save ALWAYS reads dirty, regardless of any tracker bookkeeping.  The
   per-entity tracker persists only as a safe (false-dirty-only)
   supplement for live-only pending state (mid-gesture transforms,
   scale-from-anchor) and legacy scenes.  Known, accepted asymmetry:
   undo-back-to-saved-state still reads dirty (revision moved) — a safe
   false-positive; content-hash equality is the refinement if it ever
   matters.
4. **UI reads are snapshots.**  Nothing on the UI/C-ABI read surface
   touches live managers directly.  Reads serve immutable snapshot state
   (`EditorUiSnapshot`), refreshed opportunistically under `try_lock`
   with stale-fallback: never blocks, never deadlocks, never races a
   manager swap, never flickers empty.  Genuinely-runtime data (render
   progress, live camera pose, live film dims, computed material rows) is
   an explicit runtime overlay refreshed on the same snapshot cadence —
   not an excuse for an ad-hoc live read.
5. **Notifications fire outside locks.**  State-change listeners
   (dirty-changed) are recorded during the mutation and DRAINED after the
   lock is released (`DrainDirtyNotification`), with a per-frame catch-all
   drain in `RefreshProperties` so a missed drain site degrades to a
   one-frame delay, never a deadlock or a missed transition.
6. **ABI conventions are mechanical.**  New IJob virtuals append at the
   tail AND add a line to `tests/IJobVtableManifest.txt` in the same
   commit (SourceHygieneTest enforces).  New UI categories/enums follow
   the public-constant pattern, not mirrored literals.

## 4. Phases

- **Phase 1 (this commit): derived dirty + deferred notification.**
  `SceneEditor` records `mSavedHeadVersion` at bind + successful save;
  `HasUnsavedChanges()` gains the revision-diff floor; the dirty listener
  becomes pending-flag + post-unlock drain.  Kills the false-clean class
  permanently and retires the fire-under-lock hazard at the root.
- **Phase 2 (this commit): unified snapshot read surface.**  The
  piecemeal snapshots (`mProperties`, `mPropertiesByCategory`, the
  round-4 enumeration snapshots) consolidate into one
  `EditorUiSnapshot` behind one leaf mutex with one publish function.
  Cadences are unchanged (shell-driven `RefreshProperties` per frame;
  getter-driven opportunistic refresh); the point is one structure to
  extend and one pattern to follow.
- **Phase 3 (this commit): transaction path addresses singletons; the
  Rasterizer arm migrates.**  `Job::ApplyCstParamEditImpl_` generalizes
  its unnamed-unique-fallback (previously camera-only) to any kind
  addressed with an empty name; `SceneEditController::SetProperty`'s
  Rasterizer arm becomes a plain `ApplyAgentParamEdit("", <kind>, …)`
  call — deleting the bespoke pre-flight/record/mark sequence and gaining
  undo/redo for rasterizer edits for free.  **Deliberately NOT migrated
  yet:** the Film arm (post-clamp value recording + FrameStore/camera
  resync side-effects deserve their own slice), the Film preset pick
  (two-param atomic edit; the transaction path is single-param), and the
  Environment arm (`radiance_map` erase is a multi-param REMOVE the
  single-param path cannot express).  All three are coherent today
  (round-4 pre-flight + gated marks) and are the phase-3b work list.
- **Phase 4 (deferred): Document-state undo.**  Persistent value
  Documents make N previous heads cheap; undo becomes "swap head back and
  re-derive", retiring the per-op inverse machinery.  Deferred until the
  inverse-op code next causes real pain; needs the gesture-coalescing
  design first.

## 5. What this does not fix

Renderer-internal threading; legacy (non-CST) scenes (live-only editing,
tracker-based dirty — unchanged); derive latency as the edit-latency floor
(the ~4µs incremental apply is what makes document-first viable); class-C
hygiene for surfaces not yet manifest-guarded (bridge enum switches are
the next candidate).
