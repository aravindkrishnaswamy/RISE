# v6→v7 Parser Cutover — Execution Plan

> **Execution plan for [Facet-6 §3](60-supersession-and-migration.md) (P0 + P8) and
> the singular migrate-then-DELETE contract (D8), written now that the offline
> migrator is built and green.** This doc does NOT re-design the cutover — the
> design (D1–D51, the supersession inventory, the P0–P8 phase sequence, the §3.5
> tooling spec, the §3.6 equivalence harness) lives in
> [`60-supersession-and-migration.md`](60-supersession-and-migration.md). This is
> the concrete, sequenced, gated *execution* given the current state, plus the
> findings that move work earlier than the design assumed.
>
> **Status: PLAN — for review. No source/build/scene changes are made by this doc.**

---

## 0. What is already done (P0's offline capability)

The migrator + the render-equivalence harness (§3.6) are built and green:

- **The CST kernel** (`src/Library/Cst/Cst.{h,cpp}`): lossless `ParseToCst` /
  `SerializeCst`, and **`DeriveToJob(doc, job)` is runtime-callable** (it builds a
  full `IJob`, converging at `IJob::Add*` exactly as the design's strangler-fig
  requires).
- **The migrator** `Migrate()` (in `tests/CstCorpusEquivalenceTest.cpp`): flatten
  includes (D7) + the full legacy macro/`$()`/FOR surface + the `> set
  global_medium` → `global_medium{}` chunk + **`hal()` folded** via a process-global
  `MultiHalton` mirroring legacy.
- **The gate**: `legacy(v6)` DumpJob **≡** `CST(migrate(v6))` DumpJob over the corpus
  → **347 MATCH, 2 ACCEPTED** (the user-ratified `sss` energy-conservation
  divergences), **0 unexpected**, 27 LEGACY-FAIL (media-missing, need a machine with
  the assets). Plus a global_medium **pixel** gate (mean-luminance).

**Not done:** the runtime still parses via `AsciiSceneParser` (`Job::LoadAsciiScene`
→ `ParseAndLoadScene`); the corpus is still v6; verification is structural only (see
F1); the editor still depends on `AsciiSceneParser` (see F3).

---

## 1. Three findings that shape the sequencing

### F1 — The gate is STRUCTURAL, not full-fidelity (the verification gap)
`DumpJob` ([CstRenderEquivalence.h](../../tests/CstRenderEquivalence.h)) captures
geometry/material/painter/object/camera **structure** (reference wiring, world bbox,
names, chunk set/order) but is **blind to any value that reaches no dumped field** —
explicitly: **material IOR, light power, camera intrinsics** (sensor/focal/fstop,
not on `ICamera`), and (per memory) **accelerator, light-RR threshold, the global
medium** (the last pixel-verified separately). So "347 MATCH" proves the CST derives
the same *structure*, **not** that the rendered image is identical. **Trusting the
CST as the runtime parser requires closing this** (see Phase 1).

### F2 — Corpus coverage ≠ all-153-chunk coverage
The runtime must `DeriveToJob` **every** chunk type correctly. The 376-scene corpus
exercises most, but un-corpus'd chunks (and rarely-used params/enum values — enum
values are advisory, not enforced) are **unverified by the gate**. A chunk-coverage
audit is a prerequisite, not an afterthought.

### F3 — The editor's save is coupled to `AsciiSceneParser` → the v6-DELETE (P8) is gated on Model-B P5
The GUI round-trip save (`SaveEngine`) re-reads the file and byte-splices, keyed by
the **`SourceSpanIndex` built by `AsciiSceneParser`'s Phase-B hook** — consumed at
[`SceneEditController.cpp:2724`](../../src/Library/SceneEditor/SceneEditController.cpp).
If `LoadAsciiScene` pivots off `AsciiSceneParser`, the editor's save breaks. Two
consequences:
- **Deleting the v6 reader (D8 / P8) genuinely requires P5** ("save = serialize the
  CST"), so the editor no longer needs `AsciiSceneParser`'s span index. The near-term
  cutover therefore **must not drop v6**.
- **`instance_array` is CST-only** (`Cst.cpp`, *not* an `AsciiSceneParser` chunk), so
  a v7 scene that uses it (migrated `loops.RISEscene`) **cannot be editor-loaded via
  `AsciiSceneParser`**. The other 12/13 FOR scenes desugar to explicit chunks, which
  `AsciiSceneParser` *can* read — so `loops.RISEscene` is the lone instance_array case
  to handle in the transition.

---

## 2. Prerequisites (Phase 0 — must land before any runtime pivot)

| # | Prerequisite | Why | Done-when |
|---|---|---|---|
| **0a** | **Extract `Migrate()` to a shared, canonical C++ location** (a header reused by the gate + a real `tools/` executable). **C++, not Python** — the fold reuses `MathExpressionEvaluator` + `MultiHalton`; a Python port cannot reproduce `hal()`/`$()` bit-exactly (the §3.5 doc's "Python line-rewriter" precedent does **not** apply where command/expr *semantics* are folded). | One migrator, one behavior, three consumers (gate, tool, future runtime check). | The gate uses the shared `Migrate()`; `bin/tools/MigrateScenesV6toV7` exists and round-trips the corpus identically. (New `src/Library` files ⇒ the 5-build-project tax; the tool's `.cpp` under `tools/` is globbed only if added to a build target — verify.) |
| **0b** | **Close the verification gap (F1).** Extend the equivalence oracle past `DumpJob` to the blind spots — material IOR, light power, camera intrinsics, accelerator, RR, medium params — **or** a full-render mean-luminance pixel-compare over the corpus (the global_medium pixel-gate pattern, generalized). | "Structural MATCH" is not "identical render." This is the load-bearing prerequisite for trusting the CST runtime. | Every corpus scene passes the extended oracle (structural + the F1 fields), and a pixel-spot-check sample renders within MC tolerance. |
| **0c** | **Chunk-coverage audit (F2).** Enumerate all 153 chunk types; verify each `DeriveToJob`s correctly (build a minimal scene per un-corpus'd chunk; assert derive == legacy). | The runtime needs every chunk, not just the corpus's. | A coverage report: every chunk type has a passing derive check. |
| **0d** | **Finalize the v7 grammar + header.** Decide per construct **fold-to-literal vs emit-dynamic** (D8's `let`/`expr(...)`/`halton(dim,idx)`); confirm the dynamic CST nodes exist + `DeriveToJob`. Define the header (`RISE ASCII SCENE 7`) as the path selector (§3.1). | The migrator currently **folds** `$()`/`DEFINE`/`hal()` to literals; D8 specifies dynamic forms for values that must stay dynamic. Folding loses authorial intent **permanently** once v6 is deleted. See Decision D-1 below. | The v7 grammar is pinned; the migrator emits it; the CST derives it; the header gates the path. |

---

## 3. The cutover sequence

Everything in §3.1 is **reversible** (the v6 path stays; the header is the flag). The
**point of no return** is §3.2 (deleting the v6 reader), which is gated on Model-B P5.

### 3.1 Near-term, reversible (a de-risking down-payment on P0/P8)

| Phase | Ships | Gate | Reversible? |
|---|---|---|---|
| **A** | The migrator as a first-class tool (0a). | Tool output == gate's `Migrate()` byte-for-byte. | n/a (additive) |
| **B** | The extended verification (0b) + chunk audit (0c) + v7 grammar (0d). | Extended oracle green on the corpus; coverage report complete. | n/a (additive) |
| **C** | **Convert the corpus to v7** (run the tool; commit the v7 files; the v6 originals stay in git history). | The extended gate verifies every converted scene before commit. | Yes — `git revert` the conversion; v6 originals in history. |
| **D** | **Dual-path runtime parse** (§3.1 design): `RISE ASCII SCENE 7` → `ParseToCst`+`DeriveToJob`; `RISE ASCII SCENE 6` → `AsciiSceneParser`. The **render/headless** path uses the CST for v7. The **editor** keeps `AsciiSceneParser` (for the `SourceSpanIndex`) and must learn the v7 header (+ `instance_array`, or `loops.RISEscene` stays CST-render-only until P5). | The full test suite + the equivalence harness stay green with both paths live. | Yes — flip the header / disable the v7 arm. |

### 3.2 Deferred (the point of no return — gated on Model-B P3–P5)

| Phase | Ships | Blocked on |
|---|---|---|
| **E** | Editor moves to CST-serialized save (Facet-6 **P5**: "save = serialize the CST"), so it no longer needs `AsciiSceneParser`'s `SourceSpanIndex`. | P3 (CST versioning) + P5 (save). The editor coupling (F3) is the gate. |
| **F = P8** | **DELETE** the v6 `ParseAndLoadScene` read path + the macro/FOR/`$()`/`hal`/`> load`/`> run` code + the version gate's v6 arm (**D8**). | Phase E complete + corpus 100% v7 + equivalence harness green. **Irreversible.** |

---

## 4. Open decisions for the reviewer

- **D-1 (fold vs dynamic for v7).** The migrator folds `$()`/`let`/`hal()` to literals
  today; the corpus is ~92% constant-folding, so this is render-faithful. But after
  the v6 delete (P8) the folded v7 is the *only* source — a scene with a *meaningful*
  parametric expression loses its parametricity. **Decide:** fold-all (simplest; the
  v6 in git history is the "real" source until P8) vs emit-dynamic
  (`let`/`expr`/`halton`) where the expression is authored-meaningful. Recommendation:
  fold-all for the corpus conversion; ship the dynamic CST forms as an *authoring*
  feature (new v7 scenes) separately — the corpus doesn't need them.
- **D-2 (verification strategy, 0b).** Extend `DumpJob` (cheap, structural+; still not
  pixel-exact) vs full-render pixel-compare (definitive but expensive + MC-noisy) vs
  both (extend for the cheap fields, pixel-spot-check a sample). Recommendation: both.
- **D-3 (timing/scope).** Do the near-term de-risking cutover (§3.1) **now** — proves
  the CST runtime parse + converts the corpus + closes the verification gap early, at
  the cost of a transient dual-path — **or** follow the designed P0→P8 sequence and
  not pivot the runtime until the editor is ready (P5), avoiding the transient
  complexity. Recommendation: do §3.1 Phases A–B–C now (tool + verification +
  conversion are pure wins and unblock everything); hold Phase D (the runtime pivot)
  until D-3 is decided, since it adds the dual-path before the editor can follow.

---

## 5. Risk register (execution-specific; see §6.2 of Facet 6 for the design risks)

- **R-V1 — the verification gap (F1).** Highest. A structural MATCH that hides an IOR
  / power / intrinsic divergence would ship a wrong render. Mitigation: Phase B (0b)
  is a hard gate before Phase D.
- **R-V2 — editor-save entanglement (F3).** Pivoting the parse without P5 breaks the
  GUI save. Mitigation: the dual-path keeps `AsciiSceneParser` for the editor; the
  delete is deferred to P8.
- **R-V3 — fold-loses-intent (D-1).** Irreversible at P8. Mitigation: keep v6 in git
  history; decide D-1 before P8.
- **R-V4 — `instance_array` editor-load.** `loops.RISEscene` can't round-trip through
  the editor until the editor is on the CST. Mitigation: it renders fine via the CST;
  editor-load of that one scene waits for P5 (or teach `AsciiSceneParser` the chunk
  transitionally).
- **R-V5 — the 5-build-project tax.** The new `tools/` migrator + any new `src/Library`
  verification helper costs the 5× project edit (Filelist, cmake, 2× vcxproj, pbxproj).
  Mitigation: prefer `tests/`-globbed helpers; budget the pbxproj edit.

---

## 6. Immediate next action (pending review)

If approved: **Phase A** (extract `Migrate()` to a shared header + a real
`tools/MigrateScenesV6toV7` executable, gate-verified byte-identical) — it's a pure
win, unblocks Phases B/C, and changes no runtime behavior. Phases B–F follow the gates
above; Phase D (runtime pivot) and Phase F (v6 delete) are the decision points (D-3,
and P5 readiness).

---

## 7. Execution status (2026-06-26) — Phases A + B DONE; C/D is the gated decision point

Phases A and B are complete on `feature/gui-snapshot-prototype` (unpushed), each driven through the
implementation-review-loop to zero P1s.

### Phase A — the migrator as a first-class tool (0a) ✓
`Migrate()` + helpers extracted to `tools/CstMigrator.h`, shared by the gate and a real
`tools/MigrateScenesV6toV7` executable (byte-identical by construction). Commits b4118667 / 909fff22 /
e49908c8.

### Phase B — verification gap (0b) + chunk audit (0c) + grammar (0d) ✓
- **0b structural oracle**: `DumpJob` extended past structure to every cheaply + DETERMINISTICALLY readable
  value — lights (power/colour/cone/photons), media (coefficients + phase-g + bbox-centre + placement +
  named global medium), the scene singletons (film, global radiance map, active camera), per-object
  radiance-map (painter/scale/transform) + interior-medium name, and the luminaire emitter exitance. The
  corpus gate held at MATCH throughout. A measurement-artifact was caught + fixed mid-stream:
  `IEmitter::averageRadiantExitance` was GlobalRNG-estimated at parse (`LambertianEmitter::RefreshAverages`),
  so it false-flagged the gltf scenes; fixed to a deterministic stratified grid (08db3d71), then the emitter
  row was re-enabled (94689f66) — the gltf scenes that diverged 20-180x now MATCH bit-exactly. Commits
  eace7535..94689f66.
- **0b render spot-check** (closes R-V1): the two fields with no readable surface — material IOR + camera
  intrinsics — are covered by the same-`Finalize` by-construction argument; `tests/CstRenderPixelEquivalence
  Test.cpp` CONFIRMS it by rendering the legacy-Job vs the CST-Job and comparing PER-CHANNEL mean luminance.
  11+ scenes across PT/BDPT/photon-mapping + RGB/spectral (incl. a glass-dominant scene + spectral-dispersive
  caustic): per-channel maxrel 0.0000-0.0111, all under 3%. So the CST derives IOR/intrinsics identically to
  legacy. Commits cb5ddeba / 65f3254e. (The CST DROPS `> set light_rr_threshold` — a known gap — but RR is
  unbiased, so the drop is render-mean-neutral; the threshold itself is a Phase-D dual-path item.)
- **0c chunk audit (F2)**: the full `DeriveToJob` is GENERIC over the descriptor registry — there is NO
  per-chunk special-casing (PASS-1 validates via `Describe()`, PASS-2 applies the same `Finalize` as legacy).
  154 registered chunk keywords; 129 corpus-covered (the gate proves them); 25 uncovered. Built a minimal
  scene per uncovered chunk in `scenes/Tests/ChunkCoverage/` (+ tiny synthetic exr/tiff/3ds/bdf assets),
  verified by the existing gate: all 25 MATCH. The corpus is now 401 scenes / 372 MATCH / 0 unexpected.
  Commit 6e57f729. (Side finding, spun off as a separate task: a pre-existing DataDrivenBSDF transmission-
  lobe copy-paste bug — unrelated to the cutover.)
- **0d grammar (D-1 = fold-all)**: ratified. The migrator already constant-folds `$()`/DEFINE/FOR/`hal()` to
  literals and flattens includes; NO dynamic forms (`let`/`expr`/`halton`) for the corpus (the v6 originals
  in git history are the authoritative source until P8). The migrated output KEEPS the `RISE ASCII SCENE 6`
  header (verified) → it is DUAL-READABLE (valid v6 AND CST-derivable). The `RISE ASCII SCENE 7` header is
  the Phase-D PATH SELECTOR; the migrator does not emit it today.

### Phase C/D — the gated decision point (this REVISES D-3's "do A-B-C now")
0d surfaced a sequencing fact D-3 glossed over: **Phase C (convert the corpus) cannot safely precede Phase D
(the dual-path runtime).** The legacy runtime is `CURRENT_SCENE_VERSION = 6` and rejects a `SCENE 7` header;
the CST ignores the version line. Therefore:
- Converting the corpus to **`SCENE 7`** (CST-only) BREAKS the legacy runtime — which still parses every
  scene — until Phase D's dual-path can read it. Unsafe before D.
- Converting to **`SCENE 6` fold-all** (dual-readable) is safe but PREMATURE: it flattens the LIVE corpus
  (macros/FOR/includes inlined → loses authorial editability) for NO runtime benefit, and the gate ALREADY
  proves the migrate→derive equivalence over 372 scenes, so rewriting the live files de-risks nothing.

**Conclusion: the near-term reversible down-payment ends at Phase B.** Phase C should BUNDLE with Phase D
(convert + header-bump-to-7 + dual-path, together), which is the deferred runtime pivot — gated on Model-B
**P5** ("save = serialize the CST", so the editor no longer needs `AsciiSceneParser`'s `SourceSpanIndex`,
per F3). Phase E/F (delete the v6 reader, D8) follow P5 + P8. This is the **major-design / blocker boundary**:
A + B are the safe, additive, reversible work; C/D/E/F are the entangled, gated pivot.
