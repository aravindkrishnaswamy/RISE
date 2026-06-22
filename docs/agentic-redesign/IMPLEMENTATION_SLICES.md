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
   explicitly invalidated. "Reparse-stable identity" was too broad.
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
   a dedicated version-header node is a later item.) ← next: item 3 (persistent Document).
3. Put the real `Document` on a **persistent sequence supporting update/insert/erase, with cached
   byte-width + newline count**.
4. Add **persistent NodeId/name-path lookup** so finding the edit target is *included* in the
   complexity measurement.
5. **Bind through the live descriptor registry.**
6. **Trace references through the real resolver** and test a **three-level** dependency chain.
7. Exercise **structured edits AND free-form reparses**, including **chunk identity + rename**.
8. **Measure a non-spatial edit AND a spatial edit; report TLAS time separately.**

That is the gate that turns "the model and cost-model hold in prototypes" into "the redesign's real
CST path is O(closure) for non-spatial edits, with spatial cost reported honestly."
