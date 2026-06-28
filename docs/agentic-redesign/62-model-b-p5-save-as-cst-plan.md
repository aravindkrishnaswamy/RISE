# 62 — Model-B P5: save-as-CST execution plan

Scopes the gated prerequisite for the v6→v7 cutover's Phase C/D (see
[61-v6v7-parser-cutover-execution-plan.md](61-v6v7-parser-cutover-execution-plan.md) §"Phase C/D"). **P5 =
"save = serialize the CST" + the editor off `AsciiSceneParser`'s `SourceSpanIndex` (finding F3).** Grounded
in the kernel (`src/Library/Cst`), the editor (`src/Library/SceneEditor`), and the design facets
([30-edit-model-and-history.md](30-edit-model-and-history.md), [60-supersession-and-migration.md](60-supersession-and-migration.md) §P5 row).

## 0. Headline — the kernel is READY; P5 is an INTEGRATION + PROVENANCE effort, not a kernel build

Model-B (D2 / charter): the CST is canonical, the Scene is `derive(CST)`, edits are CST patches, undo is CST
versioning, and **save = `SerializeCst(head)` + the D17 atomic write**. The editor's text-byte-splice save
(`SaveEngine` keyed on `SourceSpanIndex`) is **deleted**, not evolved.

The four canonical-source operations all EXIST and are TESTED:

- **parse** text→CST: `ParseToCst` (Cst.cpp:902).
- **edit** CST→CST: `DocSetParamValue` / `DocReplaceItem` / `DocInsertItem` / `DocEraseItem` / `DocRename`
  (persistent path-copy, identity-preserving; each round-trips through reparse in `CstEditReparseTest`).
- **serialize** CST→text: `SerializeCst` (Cst.cpp:939) — **lossless byte-exact** round-trip, pinned by
  `CstKernelTest` G1 (comments / whitespace / CRLF / tabs / blank lines all preserved verbatim).
- **derive** CST→Job: `DeriveToJob` / `DeriveToJobIncremental` (descriptor-driven — the same `Finalize` as
  the legacy parser; DumpJob-equivalent to legacy across the whole corpus, the cutover gate).
- the **D17 atomic-save** (temp → fsync → revalidate content-hash → atomic rename) is substantively landed
  (stable-apply slices).

So the hard algorithmic parts are done. **What's missing is wiring + provenance** — nothing outside
`Cst.cpp` + the `Cst*` tests calls any of it; the live editor still loads via `AsciiSceneParser`
(`Job::LoadAsciiScene`, Job.cpp:9377), edits the live `Scene`, and saves by byte-splicing the source file.

## 1. The gap P5 must close

- **G1 — load retains a persistent CST; Scene = derive(CST).** Today `Job::LoadAsciiScene` →
  `AsciiSceneParser::ParseAndLoadScene` → Scene; the CST is test-only (zero live callers).
- **G2 — edits become CST patches.** Today `SceneEditor::Apply` mutates live engine objects
  (`*Introspection::SetProperty`) + a `DirtyTracker`; undo is an inverse-edit stack. Model-B: a GUI/agent
  edit → a `DocSetParamValue` patch on the `Document` → `DeriveToJobIncremental` → updated Scene; undo =
  CST versions.
- **G3 — save = SerializeCst.** Today `SceneEditController::RequestSave` → `SaveEngine`
  (SceneEditController.cpp:2724) byte-splices value strings at `SourceSpanIndex` offsets. Model-B:
  `SerializeCst(head)` + D17 atomic write.
- **G4 — provenance.** The CST is lossless/editable/derivable only for the **v7 runtime format**
  (macro / expr / directive-free). The corpus is v6 (`$()` / `DEFINE` / `FOR` / `> run`); those serialize
  verbatim but are NOT CST-editable. So the editor can only edit-as-CST a **v7** scene → either
  migrate-on-load or convert the corpus (Phase C).
- **G5 — the one missing test.** "Minimal-diff on edit" (an edit→serialize preserves comments / formatting
  except the edited value) is implemented (structural sharing) but UNTESTED. The current `SaveEngine`
  guarantees verbatim-except-the-value-bytes; P5 must not regress that.

## 2. Findings (these shape the sequence + the decisions)

- **F-P5.1 — P5 is inseparable from Phase C + D.** You can't save-as-CST without loading-as-CST (G1), and
  you can't load-as-CST without a v7 scene (G4). So "P5" is really the
  **{convert corpus (Phase C) + dual-path load (Phase D) + save-as-CST (P5)} bundle** — exactly what
  [61] §"Phase C/D" predicted. Scope it as one workstream.
- **F-P5.2 — animated / `instance_array` scenes refuse the incremental path.** `DeriveToJobIncremental` +
  `DocRename` REFUSE on any document with an animation/timeline (dynamic refs are invisible to the static
  reference graph, Cst.h:304) or an `instance_array` generator (Cst.h:184). The watch / showcase heroes are
  animated → need a **full-re-derive fallback** (correct, just slower) for those; the incremental path is a
  perf optimization, not a correctness requirement.
- **F-P5.3 — a minor save-fidelity behavior change.** `SerializeCst` preserves untouched nodes verbatim but
  re-tokenizes the *edited value* single-spaced; the current byte-splice preserved the edited line's exact
  inner spacing. Net: still minimal-diff (comments / layout preserved); only the edited value's internal
  whitespace normalizes.
- **F-P5.4 — FP round-trip drift.** A gizmo drag re-formats a float; many drags can drift `%g` precision
  (30-edit-model §"float round-trip"). Needs a pinned float-formatting policy + a test.
- **F-P5.5 — the legacy `SaveEngine` is fully SUBSUMED, not extended.** ~1900 lines of byte-splice +
  `SourceSpanIndex` + `OverrideSpanIndex` + the Mode-A/B model are DELETED by P5 (60-supersession §P5). The
  D6 fingerprint + the D17 atomic-save are the only retained contracts. (Note: the legacy SaveEngine is more
  complete than `ROUND_TRIP_SAVE_PLAN.md`'s stale "design-pending" header claims — but that's moot, it's
  replaced.)

## 3. Proposed slices (D1 = full upfront convert; reversible — SCENE-6 dual-readable + git — until the v6-delete)

- **Slice 0 — down-payment (no live change).** (a) the G5 fidelity test (edit → `SerializeCst` minimal-diff,
  preserving comments / layout around the edit); (b) a CST-canonical *session harness* in a test:
  `text → Migrate → ParseToCst → retain Document → DeriveToJob → assert Scene == legacy-parsed Scene` (reuse
  the corpus gate's DumpJob oracle). Proves G1 end-to-end + pins fidelity, touching nothing live. *Low risk;
  the de-risking step.*
- **Slice 1 — load via the CST (flagged, additive).** A new `Job::LoadAsciiSceneViaCst` reads the file -> `ParseToCst` -> retains the `Document` ->
  `DeriveToJob`; Scene = derive(CST); the legacy `LoadAsciiScene` stays the default.  **NO runtime
  `Migrate`** (D1: the corpus is converted OFFLINE in Slice 2; the library takes no dependency on the
  transition migrator), so the CST path loads NATIVE-v7 scenes.  **[IMPLEMENTED]** -- CstLoadViaCstTest
  proves `LoadAsciiSceneViaCst` == `LoadAsciiScene` (DumpJob) + Document retention on native-v7 scenes;
  Slice 2 extends the equivalence corpus-wide.  (It is a `Job` method for now; Slice 5 exposes it on
  `IJobPriv` to wire the GUI/CLI.)
- **Slice 2 — FULL CORPUS CONVERT (D1).** Run `tools/MigrateScenesV6toV7` over the ENTIRE corpus as one
  deliberate batch → the **SCENE-6 fold-all dual-readable** form (both the legacy runtime and the CST read
  it). Resolve the convert TAIL here: the 2 `sss` energy-conservation divergences + the 27 media-missing
  legacy-fails (on a machine with the assets). git history keeps the authored originals. *The big diff, but
  reversible (dual-readable + git).*
- **Slice 3 — edits → CST patches (flagged).** Retarget `SceneEditor::Apply`: a property edit →
  `DocSetParamValue` → `DeriveToJobIncremental` → Scene; undo = CST versions; animated / `instance_array`
  docs → the full-re-derive fallback (D2). *The bulk + the highest-risk slice — the edit-model pivot.*
- **Slice 4 — save = SerializeCst (flagged).** In CST mode, `RequestSave` → `SerializeCst(head)` + D17; the
  byte-splice path is bypassed. Verify against the G5 fidelity test + the existing save-test suite.
- **Slice 5 — flip the default + drop the coupling.** CST-load becomes the default; delete the `SaveEngine`
  byte-splice + `SourceSpanIndex` / `OverrideSpanIndex` + the editor's `AsciiSceneParser` dependency (F3 severed).
- **Slice 6 — delete the v6 reader (P8) + the `SCENE 6`→`7` header bump.** Once every load is CST-native.

## 4. Decisions for ratification

- **D1 — RATIFIED (2026-06-27): full corpus convert, not migrate-on-load.** Convert the WHOLE corpus to the
  canonical fold-all form as one deliberate upfront batch (Slice 2) — every scene natively CST-editable, no
  per-scene surprises later. Target the **SCENE-6 fold-all dual-readable** form (both runtimes read it →
  the dual-path stays reversible); the cosmetic `SCENE 6`→`7` bump rides the v6-delete (Slice 6). Trade-off:
  the convert FLATTENS authored `FOR`/macros/includes into verbose canonical text (the Model-B "text is the
  compiled view" premise; git history keeps the authored originals). "No surprises" also means resolving the
  convert TAIL up front (the 2 `sss` divergences + the 27 media-missing legacy-fails).
- **D2 — RATIFIED: full-re-derive fallback** for animated / `instance_array` scenes. MEASURED (CstEditCostTest,
  this machine): a full re-derive is ~2 ms @ ~1k chunks, ~9 ms @ ~4k, ~38 ms @ ~16k (O(N log N)); the
  incremental path is ~5 µs, flat. In absolute terms that is low-single-digit ms for realistic scenes (a hero
  is tens-to-low-hundreds of chunks; Sponza ~600) — within the ~16 ms/edit interactive frame budget up to ~4k
  chunks. Only a very large (>~8k-chunk) animated scene would lag a CONTINUOUS gizmo drag on the derive alone,
  which a debounce (re-derive on drag-RELEASE, not per-frame) fully mitigates. So the fallback is the default;
  extending the static graph to dynamic refs is NOT justified (large effort for a sub-frame saving).
- **D3 — accept the minimal-diff fidelity behavior** (F-P5.3): the edited value re-tokenizes single-spaced;
  everything else verbatim.
- **D4 — scope = the C + D + P5 bundle** (forced by F-P5.1) — one workstream, not save-only.
- **D5 — start with Slice 0** (the down-payment) before committing to the editor pivot.
- **D6 (deferrable) — save affordance:** explicit Save button vs autosave (the docs leave O1 open). Not a
  Slice-0/1/2 blocker; decide before Slice 3.

## 5. Gating + size

P5 unblocks Phase E (flip the default) + Phase F / P8 (delete the v6 reader). **Size: multi-week** — Slice 3
(the edit-model pivot: retargeting `SceneEditor::Apply` + undo + introspection from the live Scene to the CST)
is the bulk and the risk; Slices 0–1 are a low-risk down-payment that proves readiness end-to-end. Every slice
is reversible (flagged) until the v6-delete (the convert is SCENE-6 dual-readable, git-reversible).
