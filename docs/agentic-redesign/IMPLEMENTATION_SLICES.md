# Agentic Redesign — Implementation Slices (review guide)

> **Status:** the design package (00–60 + `01-DECISIONS.md` D1–D51, six review rounds)
> is now backed by **four working, adversarially-reviewed prototype slices**. This doc is the
> entry point for an **external review of the implementation** before committing to the next
> phase (productionization vs more feature slices). It maps each slice to the decisions it
> validates, states the gates it meets, and — most importantly — is candid about what is **not
> yet proven** so the review can target the real risk.

All four slices are committed on `feature/gui-snapshot-prototype` (unpushed). They are
self-contained C++ test executables under `tests/`, sharing one prototype CST in
`tests/CstSlicePrototype.h`. **They are PROTOTYPES, deliberately not yet a `src/Library/Cst`
module** — the point was to validate the design before paying the five-build-project cost. Tests
are auto-discovered by the make build, so no build-project files were touched.

## How to build & run

```sh
make -C build/make/rise build-test/CstFirstSliceTest      && ./bin/tests/CstFirstSliceTest
make -C build/make/rise build-test/CstIncrementalDeriveTest && ./bin/tests/CstIncrementalDeriveTest
make -C build/make/rise build-test/CstReferenceSliceTest  && ./bin/tests/CstReferenceSliceTest
make -C build/make/rise build-test/CstCostSliceTest       && ./bin/tests/CstCostSliceTest    # run from repo root
```

Each prints its checks and exits 0 on pass (`N passed, 0 failed`). Clean compile, no warnings.

## The slices

| # | File | Commit | Checks | Proves |
|---|---|---|---|---|
| 1 | [`tests/CstFirstSliceTest.cpp`](../../tests/CstFirstSliceTest.cpp) | `8c9922ef` | 26 | lossless round-trip (G1) + descriptor binding + name-path + red cursor + path-copy sharing + real apply-layer reuse |
| 1.5 | [`tests/CstIncrementalDeriveTest.cpp`](../../tests/CstIncrementalDeriveTest.cpp) | `c1cbd4c5` | 24 | memoized node-granular derive + reparse-stable identity **for param/value NodeIds** (D15 invalidate-not-remap) |
| 2 | [`tests/CstReferenceSliceTest.cpp`](../../tests/CstReferenceSliceTest.cpp) | `be1b2c5c`, dangling-cache fix | 28 | references: reverse-index, forward-cone, freshness, NodeId-keyed cache, rename, dangling (incl. persistent), delete |
| 3 | [`tests/CstCostSliceTest.cpp`](../../tests/CstCostSliceTest.cpp) | `33f38b50` | 29 | O(closure) re-derive **by apply-layer-call count** (closure already computed; excludes lookup/COW/realize/TLAS), invariant to N |

Shared scaffolding: [`tests/CstSlicePrototype.h`](../../tests/CstSlicePrototype.h) — the green
CST (immutable shared nodes, relative-width), lossless lexer, descriptor-bound parser
(multi-chunk, brace-nested, bounds-safe), serializer, red cursor, name-path addressing, path-copy
edit (carries NodeId + chunk identity), and the real-engine derive helpers
(`Job::AddSphereGeometry` etc.).

### Slice 1 — sphere_geometry vertical
- **Validates:** D2 (relative widths, no stored offsets), D9/D15/D44 (name-path addressing),
  D11 (path-copy structural sharing), D16 (red cursor computes offsets), D26 (NodeId lineage),
  L6 (descriptor-driven binding), the Facet-2 apply-layer reuse seam.
- **Gate G1 (round-trip byte-identity):** met on MULTI-CHUNK documents in the real corpus shape
  (keyword/`{` on own lines, tab-aligned columns, comments between chunks) **and** on the live
  `scenes/Tests/Geometry/csg.RISEscene` sphere chunks, plus the §4 tar-pit cases (tabs, CRLF,
  inline `#` comments, blank lines, nested braces, no-trailing-newline).
- **Not covered:** full arbitrary scenes (header line, FOR/DEFINE/expr/`>` commands) need the
  general grammar.

### Slice 1.5 — incremental derive + reparse identity
- **Validates:** D4/§2.4 (memoized node-granular derive), D15 (trivia-insensitive key; the
  ambiguous-repeated-row INVALIDATION safety rule), D26 (lineage across edits).
- **Proves:** a comment/whitespace-only edit re-derives **nothing** (0 real apply-layer calls);
  a free-form text reparse re-associates prior NodeIds **by content then position** (a reorder
  never silently remaps), and genuinely-ambiguous rows are invalidated, never rebound.

### Slice 2 — references
- **Validates:** D4 (traced reverse-dependency index), §2.4 (forward-cone invalidation),
  D9/§2.9 (reference resolution + dangling), D14 (rename rewrites referrers), D20 (edge
  lifecycle incl. delete), D26 (NodeId-keyed cache).
- **Proves (real engine, `AddUniformColorPainter`/`AddLambertianMaterial`):** edit a producer →
  consumer re-derives; unrelated edit → it doesn't; freshness (consumer re-derives though its own
  CST is unchanged); rename without leaking the old engine object; diamond fan-out; reference
  retarget moves the edge; delete a producer → its object is removed and the orphaned referrer is
  flagged dangling (become-dangling), not silently bound.

### Slice 3 — the O(closure) cost gate (the headline)
- **Validates:** D16/D23 (persistent container → O(log N) edit + diff), §2.4 (transitive
  forward-cone), the §6.1 O(closure) thesis, into the real apply layer.
- **Proves (measured at N = 8/64/512):** a path-copy edit + structural-sharing diff is O(log N)
  (visits 3/6/9, pruning pointer-equal subtrees in O(1) — the unchanged bulk is never walked);
  the re-derive closure is **constant in N** (1 independent / 4 for the painter root). **The
  join:** re-derive the closure **into a real `Job`** at N=8 and N=512 → **4 apply-layer calls at
  both**, while the initial full load grows 8 → 512. The naive O(N) re-key baseline is an actual
  measurement (not a constant), and sequence correctness is asserted (an injected `UpdateSeq`
  off-by-one fails the test).

## What the adversarial review found (and what was fixed)

Three orthogonal reviewers ran on **every** slice (12 reviews total). They found a real shipped
bug in **each** one; all were fixed with the reviewer's own reproducer added as a regression test:
- Slice 1: `ParseToCst` silently dropped everything after the first chunk; nested braces
  truncated the document.
- Slice 1.5: the reparse matcher silently mis-mapped **reordered** distinct rows (position-only)
  — the cardinal D15 violation.
- Slice 2: delete/become-dangling was unhandled; a comment falsely claimed the engine refuses an
  in-use remove.
- Slice 3: the naive baseline was a fabricated constant; an uncounted O(N) `IdMap` copy sat
  inside the measured edit; cost was asserted but not data correctness; the cost path and the
  real-engine derive were never joined.

Cross-cutting lesson worth a reviewer's attention: writing an honest-scope note **and then having
an adversary check it against the code** caught under-disclosure twice.

## What is NOT yet proven (suggested review focus)

The residue is **productionization, not doubt** — but it is exactly where the next phase's risk
lives, so it deserves scrutiny:

1. **Transfer gap.** The persistent O(log N) sequence (slice 3) is a **standalone container**, not
   yet wired in as the CST `Document`'s child list (which still uses `std::vector` in the shared
   header). "The structure can be O(log N)" ≠ "the redesign's CST edit path is O(log N)." The
   biggest open question: does wiring it into the real CST + parser + `Job` hold up?
2. **Value-only edits / build-once tree.** Slice 3's persistent sequence is built balanced once
   and only value-edited (no insert/delete → no rebalancing; `DiffSeq` requires same-shape
   inputs). A general insert/delete-balanced persistent sequence (RRB / weight-balanced) is
   unbuilt.
3. **REBUILD vs D11 closure-copy COW.** Re-derive is remove+add of the closure (the REBUILD
   path). D11's closure-copy COW (copy the reverse-dependency closure of engine objects, share the
   rest) and the §2.5 APPLY in-place fast-path are not exercised.
4. **Persistent cache / stamps.** The memo/dep state is plain `std::map`, mutated in place — not
   the D23 persistent containers / D29–D30 stamp-keyed artifacts. (Per D23 the headline is
   formally gated on these; slice 3 proves the *work-count* invariance, an honest analogue.)
5. **Scene-language breadth.** No `expr(...)` (D4 traced-input), no RepeatGroup (D3), no
   `instance_array` (generators), no multi-token DoubleVec3 params (they currently parse as
   separate params).
6. **Gate G2 wall-clock.** Slice 3 proves O(closure) by **operation count**, not by a <50 ms
   wall-clock on a 155-mesh Sponza scene. The cost *model* is validated; the absolute latency on a
   real big scene is not yet measured.

## External review outcome (2026-06-21)

**Verdict: the architectural direction has enough evidence to justify in-tree implementation — but
the claimed end-to-end complexity result does not yet exist; make the next increment a NARROW
production transfer gate, not broad productionization.** The review raised 7 P1s (all legitimate;
most sharpen disclosures already in "What is NOT yet proven" above):

1. Slice 3 proves a balanced-sequence *primitive*, not D16's CST path — `SeqNode` caches only
   element count, is handed an already-known index, excludes trivia, and has no byte-width/newline
   aggregates or NodeId/name-path index; the real `Document` is still a vector.
2. The parser is **not descriptor-bound** — `ParseChunk` accepts generic token pairs; the registry
   integration is untested.
3. Slice 2 does **not trace during derivation** — it scans materials with hardcoded `ReflOf()` and
   builds `dependents` before the apply layer (demonstrates invalidation, not D4's resolver/trace).
4. Reparse identity **omits chunk identity** — `MatchIdentities` carries only param/value ids;
   match is by (keyword,name), so a free-form rename is delete+add and the old chunk id isn't even
   explicitly invalidated. "Reparse-stable identity" was too broad. *(This was a PROTOTYPE-era finding;
   **resolved in the in-tree item 4** — the kernel's 4-pass matcher carries chunk lineage through a
   unique-of-type rename (D9/D44) and explicitly invalidates genuinely-ambiguous rows.)*
5. **A failed derivation was cached as success** — a dangling material got a normal cache entry, so
   a later no-op derive skipped it and the error vanished. **FIXED** (`CstReferenceSliceTest.cpp`:
   dangling derivations are not cached; the error re-reports until resolved; +2 regression checks,
   slice 2 now 28/28).
6. "O(closure) into the real engine" **excludes dominant ops** — the count is apply-layer Add*
   calls *after* the closure is known; it omits lookup, persistent identity/cache updates,
   snapshot COW, realization, and TLAS rebuild; **spatial edits remain O(N log N)** (D24).
7. **The render-equivalence harness does not exist** — the migration plan makes it the pre-P0 gate
   (build before the first CST node); it's the main regression oracle for entering the real parser.

**Claims narrowed** (this doc + `00-OVERVIEW.md`) per #4 and #6 above. The dangling-cache bug (#5)
is fixed.

## Next increment: the in-tree transfer gate (the agreed plan)

A single narrow vertical that closes the transfer risk, keeping expr/RepeatGroup/instance_array OUT
until it is green:

1. **Build the render-equivalence harness first** (the pre-P0 regression oracle). **✅ DONE** —
   [`tests/CstRenderEquivalence.h`](../../tests/CstRenderEquivalence.h) (`ParseLegacy` drives the
   real `AsciiSceneParser`; `DumpJob` is the canonical structural equivalence metric) +
   [`tests/CstRenderEquivalenceTest.cpp`](../../tests/CstRenderEquivalenceTest.cpp) (9/9: the legacy
   parse is deterministic, and the metric discriminates a changed scene). The CST slices will assert
   `DumpJob(cstJob) == DumpJob(legacyJob)` against this.
2. Create the actual **`src/Library/Cst` kernel** (touches the five build projects). **✅ DONE** —
   [`src/Library/Cst/Cst.{h,cpp}`](../../src/Library/Cst/Cst.h) (`ParseToCst`/`SerializeCst`/
   `DeriveToJob`) wired into all five build projects (Filelist make-verified; cmake/vcxproj/filters
   updated; Xcode pbxproj replicated from the SDFGeometry pattern + a `Cst` group, `plutil -lint`
   OK). Gated by [`tests/CstKernelTest.cpp`](../../tests/CstKernelTest.cpp) (11/11): G1 lossless
   round-trip on real scenes (header / multi-chunk / tar-pit) AND `DumpJob(cstJob) ==
   DumpJob(legacyJob)` for sphere scenes, driving the real `Job::AddSphereGeometry`. (The
   `RISE ASCII SCENE 6` header is preserved losslessly as stray tokens that `DeriveToJob` ignores;
   a dedicated version-header node is a later item.) **Adversarially reviewed (3 agents): build
   wiring sound (plutil OK, zero warnings); foundation sound (the `std::vector` child container is
   encapsulated, so item 3's rope swap is contained). Fixed 3 real kernel divergences from legacy —
   `/* */` block comments are now stripped as trivia (a commented-out chunk is not derived),
   repeated params LAST-win (matching `ParseStateBag`), and a value-less line no longer swallows the
   next line's token. **A SECOND external review (commit range 70f28848..da6e4b87) found 2 P1s, both
   fixed: the oracle now uses LOSSLESS `%.17g` (was `%.6g`, which collided near-equal radii); and
   `DeriveToJob` is now a SAFE BOUNDARY — it validates every chunk (unknown param / value-less line /
   non-finite radius) and REFUSES ALL with diagnostics on any failure, never silently half-deriving
   (no `atof()->0` sphere). Now 25/25.** Deferred-and-honest (narrowed in `Cst.h`): `$( )`/DEFINE/FOR are
   the v6→v7 **migrator's** domain (D8), never the CST runtime; descriptor-driven param **validation**
   (legacy rejects unknown/ill-typed values) is item 5; so the equivalence gate is exact for
   macro-free, descriptor-valid scenes (item 5 narrows the canonical scope further — also
   directive-free and own-line-comments; see item 5). ← next: item 3 (persistent Document).
   - **Item-3 acceptance (from the review, do NOT repeat slice-3's gap):** the persistent `Document`
     must carry its subtree aggregates (cached **byte-width AND newline count**, D16) and make the
     **edit-target lookup COUNTED in the complexity measurement** — not a side table resolved by an
     O(N) scan. The balanced sequence is the easy half; the aggregates + counted lookup are the
     load-bearing part. **(Correction per the item-3 review: this bar's earlier phrase "NodeId/name-
     path index *inside* the node" is looser than D26 — per D26 the identity index is a SEPARATE
     persistent side-map (occurrence→NodeId), NOT a field on the green/seq node. So the bar is
     satisfied by a SPLIT: byte-offset addressing via the in-node aggregates lands in item 3; the
     NodeId/name-path index as the side-map lands in item 4. Item 3's `Node` intentionally has no
     NodeId.)**
   - **Gate-maintenance rule (items 5–7):** each new derived chunk type must, in the same change,
     extend `DumpJob` with that type's **discriminating fields** (transforms, param values, colors,
     ordering) — else the equivalence gate weakens silently. (Item 5 applied this: `DumpJob` now
     dumps geometry bounding-sphere radius AND object reference wiring + world-space bbox; the
     remaining types stay names-only until they are CST-derived with discriminating fields.)
3. Put the real `Document` on a **persistent sequence supporting update/insert/erase, with cached
   byte-width + newline count**. **✅ DONE** — `src/Library/Cst` `Document` is now a persistent
   balanced sequence (`SeqNode`) whose nodes cache subtree count + byte-width + newline aggregates
   (and each item's own stats, so a path-copy edit reuses them and stays O(log N) regardless of item
   size). `DocReplaceItem` (path-copy edit), `DocInsertItem`/`DocEraseItem`, `DocItemAtByteOffset`
   (the byte→node map), `DocByteWidth`/`DocItemCount`. Gated by
   [`tests/CstDocumentCostTest.cpp`](../../tests/CstDocumentCostTest.cpp) (67/67): at N=8/64/512,
   find-by-offset AND value-edit visits are **5/7/10** (~log N) while item count is **24/136/1032** —
   both **counted** and **<< N** (the slice-3 "handed an already-known index" gap is closed); **the
   rope insert AND erase are O(log N) on a persistent weight-balanced tree** (BB[α], Adams δ=3/γ=2) —
   measured max insert/find/erase visits **16/8/9 at N=512** (vs N=512), the tree stays balanced across
   any edit mix; aggregates stay exact, round-trip + structural sharing hold. The full CST suite stays
   green (kernel 25/25 unchanged — the swap is behavior-preserving). **(This O(log N) is the rope-level
   splice; item 4's identity layer adds an order-label whose gap-exhaustion reflow makes the COMBINED
   `DocInsertItem` O(log N) common-case but Θ(N·log N) worst-case under adversarial dense inserts — see
   item 4's reflow note.)**
   - **Two review rounds.** Round 1 (3 agents): O(log N)-and-counted verified genuine (instrumented
     probe); code ASan/UBSan-clean, boundaries hand-traced. Round 2 (external, 4 P1s) — all fixed:
     **(a)** insert/erase were flatten-and-rebuild O(N) (a D24 mis-attribution — D24 is TLAS, not the
     CST rope; D16 requires O(log N)) → replaced with the weight-balanced tree above; **(b)** null
     `NodeRef` reached `NodeStats` and crashed → `DocReplaceItem`/`DocInsertItem` now refuse null
     (no-op, non-null contract); **(c)** the cost gate trusted only a manual `visits` counter → added
     DURABLE work instrumentation (`DebugItemStatWalks`) + an adversarial huge-sibling test asserting
     an edit re-walks **exactly one** item invariant to N (a hidden re-scan now fails the gate);
     **(d)** the newline aggregate was ungated (zeroing it passed) → `DocNewlineCount` exposed +
     checked exact across parse/replace/insert/erase on LF **and** CRLF. 42→67. ← next: item 4.
4. Add **persistent NodeId/name-path lookup** so finding the edit target is *included* in the
   complexity measurement. **✅ DONE (pending re-review)** — kernel `a3cc2a77`; review fixes
   `0b0a25f4`, `955c1962`, `bfc57601`, `a5ba984d`, + the 4th-review fixes. [`tests/CstIdentityTest.cpp`](../../tests/CstIdentityTest.cpp)
   **134 checks**. *(Current shipped state — this entry describes what is in tree now, not the
   intermediate states the review rounds passed through; see the review log at the end.)*
   `NodeId` and the order labels are **explicit 64-bit (`int64_t`)** — NOT `long`, which is 32-bit on
   Windows. FOUR persistent structures, all SEPARATE from the green/seq node (per **D23/D26** —
   `Node`/`SeqNode` carry no id):
   - `idseq` — a positional `NodeId` WBT in lockstep with the item sequence, each node also carrying
     an **order-maintenance label** (ascending in document order);
   - `byName` — a key-ordered WBT (name-path → **list** of NodeIds);
   - `byId` — a `NodeId` → (current node, label) reverse index;
   - `paramIds` — a `(chunkId, role, occurrence-index)` → param `NodeId` index (per-occurrence PARAM
     identity, so REPEATED params get distinct ids), **matched by CONTENT not occurrence** on edit (a
     repeated param keeps its id across a sibling insert/remove; never position-remapped).

   **Counted O(log N), all proven by `maxVisits` gates sub-linear at N=8/64/512:** name-path lookup
   (`DocFindByName`); durable id → node (`DocResolveNodeId`); durable id → document **position**
   (`DocIndexOfNodeId`, via byId-label + `IdRankByLabel`) — so **end-to-end edit-by-NodeId**
   (`DocIndexOfNodeId` + `DocReplaceItem`/`EraseItem`) is O(log N), not an O(N) scan. Within-chunk
   descent (`DocParamAtByteOffset`, offset → identified Param-in-Chunk) for "edit geometry/s.radius".
   Identity survives value edit + insert/erase index shift + structured rename (exactly) and reparse
   (best-effort, D9/D44).

   `DocReparse` carries ids via a **4-pass** matcher — (1) full-content for unchanged multisets
   (reorder follows content; byte-identical duplicates partial-edited are *not* swapped), (2) unique
   `(keyword,name)` (named value edit keeps its id), (3) unique keyword (rename keeps lineage),
   (4) invalidate the rest — and reports invalidated **chunk AND param** ids. Cost is **O(M log M)**
   (the matching passes are O(M+N) hashed — committed anti-quadratic gate `DebugReparseOldVisits`
   137/1033 at N=64/512; the sorted index rebuilds carry the log factor, as `ParseToCst` itself does).
   `DocFindByName` **refuses an ambiguous** duplicate name (returns 0 + occurrence count).

   **Disclosed scope / fallbacks:** name-path key is `keyword/name` (category paths like `geometry/s`
   are DEFERRED — item 5 left them out: a CST-navigation nicety, not load-bearing, since the derive
   resolves references through the engine's named managers; see item 5); value-ATOM sub-identity within a multi-atom value, and repeated-param
   VALUE nodes, are RepeatGroup-era (out of this gate, as expr/RepeatGroup are); **`DocInsertItem` is
   NOT uniformly O(log N)** — assigning the order-label is O(log N) when a gap is available (the common
   case), but a gap-exhausting insert triggers a **WINDOWED** reflow (`ReflowWindow`). That window is
   tiny in the common/sparse case (`DebugReflowLabelWrites` measures **2** on sparse mid-edits), so it
   markedly improves the COMMON case over a global reflow — but it is **fixed-density, not level-scaled**,
   so an **adversarial DENSE pattern** (repeated inserts packing a prefix) can grow the window to
   **Θ(N)**, making that insert **Θ(N·log N) worst-case** (the `[reflow]` gate drives this dense
   adversary and asserts id↔position correctness, not a `≪ N` window). So the reflow is a **common-case**
   optimization, **not asymptotic** — the disclosed **v1 fallback** (D23 sanctions an O(N) v1 identity
   cost); **Bender's level-scaled order-maintenance** (window → O(1) amortized, restoring O(log N)
   inserts) is the refinement, not yet landed. The COUNTED **lookups** (name / id→node / id→position)
   ARE O(log N). Param matching is **O(P)** (hashed, `DebugParamMatchVisits` gate), not the prior Θ(P²).

   **Acceptance (item-3 review) ✓:** SEPARATE side-map; counted name lookup; survives value edit +
   index shift; within-chunk descent; reparse-stable invalidate-don't-remap.

   **Review log (every finding fixed):** a self-run 3-reviewer pass (`0b0a25f4`) caught the byName
   single-id dup bug, the reparse position-remap of ambiguous groups, and undisclosed Θ(N²) reparse.
   A 1st external review (6 P1s, `955c1962`+`bfc57601`): per-occurrence param identity, O(log N)
   reverse resolution, reparse no-swap, duplicate-name refuse, honest O(M log M)+gate, rename lineage.
   A 2nd external review (3 P1s, `a5ba984d`): **O(log N) edit-by-NodeId** (order-maintenance labels —
   the prior O(N) `DocIndexOfNodeId` reopened the counted-target gate), **per-occurrence** param keys
   (the prior `(chunkId,role)` overwrote repeated params), and **reparse param-id invalidation** (the
   prior invalidated list omitted dead param ids).
   A 3rd external review (3 P1s, the 4th-review-fix commit): **windowed label reflow** (the prior
   global reflow was O(N·log N) every ~32 same-spot inserts → now a local window, tiny in the common
   case but — being fixed-density, not level-scaled — still **Θ(N·log N) worst-case** under an
   adversarial dense pile; the disclosed v1 fallback, D23, with Bender level-scaled as the O(log N)
   refinement; gated by `DebugReflowLabelWrites`); **`int64_t` labels/NodeId** (the prior `long` collapsed on
   Windows/LLP64 where `1L << 32` is UB); and **content-based repeated-param matching** (the prior
   `(role, occurrence-index)` reuse still position-remapped a repeated param's id onto an unrelated
   value on sibling insert/remove → now matched by content, invalidate-don't-remap, and
   `DocReplaceItem`/`EraseItem` gained an `invalidated` out-param). ← next: item 5 (descriptor-registry binding).
5. **Bind through the live descriptor registry.** **DONE.** `DeriveToJob` no longer hand-derives
   `sphere_geometry`; it looks each chunk up by keyword in the **live registry** (`CreateAllChunkParsers`),
   validates its params through the **same `DispatchChunkParameters`** the legacy parser runs (now
   exposed as a public wrapper in `IAsciiChunkParser.h`), and applies via the **same
   `IAsciiChunkParser::Finalize`** — so EVERY registry chunk type derives and the CST path builds a Job
   identical to the legacy path **for the canonical scenes the CST is fed** (the v6→v7 serializer's
   output: macro-free, **directive-free** — no `>` run/load/set/clearall lines — comments on their own
   lines — `#` or MULTI-line `/* */`, never a single-line `/* ... */` — single-space values; all the
   migrator's domain per D8, so a non-canonical legacy input may diverge, each path by its own rules).
   **Two-tier failure boundary** (per the item-5
   review): VALIDATION-time failures (unknown chunk/param, value-less line, non-finite/non-numeric value)
   are **refuse-all** (validate every chunk via a populated `ParseStateBag`; apply none on any failure);
   an APPLY-time `Finalize` failure (e.g. an unresolved reference, undetectable pre-apply) matches the
   legacy parser's **abort-on-first-failure** (stop, emit a diagnostic, leave chunks before it applied —
   never silently swallowed or continued past; full rollback is later Facet-2 work). Two enabling
   changes: (a) `ParseChunk` now captures **multi-token param values** (`color 1 0 0`, `position 1 2 3`)
   as several `pvalue` tokens, round-trip-lossless, and the derive feeds each param line **whitespace-
   normalised exactly as the legacy parser normalises it** (`TokenizeString` collapses ` \t\r` runs +
   rejoins single-space — so tabs/multi-space/aligned columns in a string value can't drift the Job, the
   review's silent-object-drop case); (b) `DumpJob` gained **object reference wiring** (geometry +
   material names via manager reverse-lookup) and the **world-space bounding box** (encodes the
   multi-token position/scale), so a derive value mis-capture diverges the oracle. Test:
   `tests/CstDescriptorBindTest.cpp` (multi-type equivalence vs legacy, multi-token capture, whitespace
   normalisation, apply-time abort-on-dangling-ref, refuse-all on each malformed class) plus the SHARED
   `tests/CstDeriveDifferentialTest.cpp` (single-derive equivalence over a canonical corpus + cross-derive
   statelessness for the painter-colour and camera name-dedup leak vectors). Item-5 review: self-driven
   rounds — round 1: 2 code P1s (whitespace-normalisation equivalence break → silent object drop;
   apply-time refuse-all hole → silent half-derive); round 2: 2 doc P1s (a false mid-line-comment
   justification; scope omitted `>` directives); round 3: 3 doc P1s (own-line single-line `/* */` block
   comment also diverges; the `DispatchChunkParameters` wrapper doc undercounted its failure conditions;
   a file-header surface still said "macro-free, descriptor-valid" only) + a stale-family sweep; round 4:
   1 CODE P1 (cross-derive parse-state leak — `DeriveToJob` never reset the chunk parsers' file-scope
   caches the way legacy `ParseAndLoadScene` does → fixed with `ClearChunkParserState()` at the top +
   `[state-isolation]` regression test, red-proven) + 1 doc P1 (the non-canonical string-comment verb was
   "rejects"; legacy actually SILENTLY MIS-CAPTURES); rounds 5-6: 0 code P1s (1 doc P1 + tally fixes);
   round 7: CONVERGED — 3 fresh reviewers (correctness/claims/design) all returned ZERO P1s on the
   current state (only 2 non-misleading P2s, one polished). The DURABLE, reproducible differential is the committed
   `CstDeriveDifferentialTest` (single-derive corpus + cross-derive statelessness, red-proven to catch
   the leak); on top of it each round's reviewers ran independent THROWAWAY differentials of growing
   size (round 3 ≈150 scenes, round 4 ≈73 real + 38 synthetic, round 5 ≈2k cross-derive checks) — all
   zero Job divergences, but ephemeral (not reproducible from the tree; the committed test is the
   durable guard). Deferred-and-honest:
   **category name-paths** (`geometry/s`; one category → many keywords) stay out — reference resolution
   in the derive runs through the engine's
   named managers by name, so category addressing is a CST-navigation nicety, not load-bearing for
   items 5–8. ← next: item 6 (reference tracing through the real resolver).
6. **Trace references through the real resolver** and test a **three-level** dependency chain.
   **DONE (pending review).** `TraceReferences(doc)` builds the reference graph (D14/D25, §2.5) via a
   DESCRIPTOR-BASED resolver (D14's "descriptor-provided reference resolver") that uses the SAME
   category-name keying the engine's managers use (so it agrees with the engine for the references the
   descriptor DECLARES, modulo the (category,name) coarseness noted in SCOPE):
   PASS A indexes every chunk's definition by **(category, name) →
   NodeId** — the descriptor-derived category namespace the named managers key on (Geometry → geometry/,
   Material → materials/, …, so a reference resolves to the named chunk in the referenced CATEGORY
   regardless of keyword); PASS B records a **`ReferenceUse { sourceValueNodeId, targetNodeId }`** per
   EXPLICIT reference — a param whose descriptor kind is `Reference`, OR a Reference token of a TUPLE
   param (`tupleKinds[k]==Reference`: advanced_shader's `shaderop <ref> <min> <max> <op>`, voronoi's
   `gen <x> <y> <ref>`, scalar_painter's `multiply <ref> <ref>`) — present with value ≠ `none`, resolving
   the value across the param's `referenceCategories`. An unresolvable reference is a **dangling**
   diagnostic, never a silent edge. `sourceValueNodeId` is the referring param's NodeId (item-4
   granularity; value-atom sub-identity is the deferred refinement, and for a single-value ref the param
   IS the value holder). The registry is now a shared `DescriptorRegistry()` helper (DeriveToJob +
   TraceReferences). Test `tests/CstReferenceTraceTest.cpp` on the canonical 3-level chain
   (object→geometry, object→material→reflectance painter): the exact edges incl. the object→material→
   painter chain; source/target NodeIds resolve to the right Param/Chunk; **rename** referrers found from
   the graph (the only referrer of the painter is material.reflectance — D14); the **transitive closure**
   is walkable (D25); dangling ref flagged; explicit `none` is not an edge; a tuple-Reference traces
   (advanced_shader.shaderop x2 occurrences; scalar_painter.multiply's two ref tokens). Cost O(N log N)
   (per chunk an O(params × descriptor-params) scan + O(log N) NodeId/param-id lookups, bounded per
   chunk; measured linear-with-log to N=32k). **SCOPE (honest):** (a) **dynamic references** whose
   category is chosen at derive time by another param (e.g. `timeline.element` keyed by `element_type`,
   D14) are invisible to `referenceCategories` — not traced; (b) a reference in a param the descriptor
   declares as neither `Reference` nor a tuple-Reference is a descriptor-completeness gap — a 15-site
   review audit found + fixed all three (advanced_shader.shaderop, voronoi{2d,3d}.gen,
   scalar_painter.multiply); (c) the (category,name) namespace is COARSER than the engine's per-slot
   resolution — `scalar_painter` shares ChunkCategory::Painter with colour painters but a SEPARATE
   manager (a same-name scalar+colour pair mis-resolves), and `ior` over-declares {Painter,Function}
   though the engine resolves it only against scalar painters (spurious edge if a name exists only as a
   Function). The production primary path (D35) records `ReferenceUse` FROM the actual derivation
   resolver as it runs (no parallel pass → no drift, dynamic refs captured); this pass is the
   transfer-gate demonstration of the graph + its uses (rename / closure / dangling), with that
   derive-time tracing deferred. ← next: item 7 (structured edits + free-form reparses).
7. Exercise **structured edits AND free-form reparses**, including **chunk identity + rename**.
8. **Measure a non-spatial edit AND a spatial edit; report TLAS time separately.**

That is the gate that turns "the model and cost-model hold in prototypes" into "the redesign's real
CST path is O(closure) for non-spatial edits, with spatial cost reported honestly."
