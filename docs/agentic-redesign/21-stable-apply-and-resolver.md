# 21 — Stable-Object Incremental Apply + Shared Reference Resolver

Extends [20-derivation-engine.md](20-derivation-engine.md). This is the "Facet-2"
apply work the transfer-gate item-8 entry kept deferring, promoted to real design
after a bulk review of items 5–8 found **nine P1 blockers** in the first
drop/re-add incremental (`DeriveToJobIncremental`) and the parallel reference
scan (`TraceReferences`). Two root causes underlie all nine; this doc designs them
out. Decisions taken by the user on 2026-06-23: **stable-object in-place apply**
(not drop/re-add) and **build the shared D35 resolver now** (not narrow).

## 1. Why — the two roots behind the nine P1s

**Root A — the apply changes object addresses.** `DeriveToJobIncremental` does
`RemoveObject` + re-`Finalize`, so every recreated object gets a *new address*.
`ObjectManager`'s top-level BVH stores raw `const IObjectPriv*`
([ObjectManager.cpp:392](../../src/Library/Managers/ObjectManager.cpp)) and
`PrepareForRendering` keeps an existing BVH, so after an edit the TLAS points at
freed/stale objects (P1.1: UAF / wrong render). It also means a *non-spatial*
edit's closure recreates the dependent objects, so "non-spatial edits skip the
TLAS" is **false** (P1.2): with drop/re-add the TLAS must always rebuild. The
central item-8 result is only achievable with **address-stable objects**.

**Root B — the reference graph is a parallel scan, not the derivation's own
resolution.** `TraceReferences` re-derives edges from descriptors, so it is
unstamped, misses non-descriptor (dynamic) references, treats reference-or-literal
fields as references, and uses a coarser namespace than the managers — yet
`DocRename`/`DocEditClosure` consume it as authoritative, so D14/D25 aren't
actually satisfied (P1.8), rename collisions miss the runtime namespace (P1.7),
and partial renames get committed (P1.6).

The remaining P1s are per-parser reversibility (P1.3 PBR helper painters), failed
drops ignored + rename-through-incremental (P1.4), ambient thread-local parser
caches (P1.5), and inaccurate cost/status docs (P1.9).

## 2. Architecture A — the shared resolver (D35)

One authority resolves a reference by `(category, name)` over the **complete
derivation namespace** and **records the edge as it resolves** — used by *both*
the derive-apply and every consumer (rename, closure). No parallel scan.

- **Namespace = managers + runtime defaults.** The resolver resolves over the CST
  defs PLUS the engine's runtime defaults (the `none` material/painter added at
  [Job.cpp:351,356](../../src/Library/Job.cpp) and the `Default*` shader ops at
  357–373), not a CST-only view. So `none`/`Default*` are first-class names that
  collisions and resolutions see. **Interim (slices 1–4):** `BuildReferenceGraph` is
  a Document-only function (no Job handle), so the default set is a hardcoded
  `RuntimeDefaultDefs()` constant, kept honest by `CstResolverTest`'s [namespace]
  check (it derives an empty Job and asserts every listed default is present → a
  listed default renamed/removed in Job.cpp fails the check; a Job-side addition or a
  list-only drop is not auto-caught — the latter surfaces as a dangling diagnostic).
  **Endpoint (slice 5):** when resolution is routed through the derivation, the
  namespace is read from the live managers (`IJobPriv` already exposes every manager
  — `GetMaterials`/`GetPainters`/`GetShaderOps`/… with `GetItem`/`EnumerateItemNames`,
  no new getters — downcast `IJob&`→`IJobPriv&` as `override_object` does), so the
  hardcoded constant is retired.
- **Recorded during derive, stamped.** As `DeriveToJob` applies each chunk, the
  resolver records every `(sourceParam NodeId → resolved target)` edge into a
  `ReferenceGraph` attached to the derived artifact, stamped with the derive's
  generation. Rename/closure read that stamped graph; a stale graph (artifact
  re-derived) is detected by the stamp, never silently trusted.
- **Resolution is descriptor-driven at the apply layer**, replacing the scattered
  per-parser `GetX()->GetItem(name)` ([AsciiSceneParser.cpp:1411,1451,…](../../src/Library/Parsers/AsciiSceneParser.cpp))
  as the *recording* path. (Phase: the parser's internal lookups remain, but the
  recorded graph is what consumers use; a later phase routes the parser lookups
  through the resolver so the two cannot drift even in principle.)
- **Honest boundary.** Where a reference is genuinely dynamic (created by a helper,
  or an expr the static layer can't see), the resolver records it as
  *unresolved-dynamic* rather than guessing — and any operation (rename) that
  would need to rewrite an unresolved-dynamic referrer **refuses** (§4).
- **⚠ Static-graph gap to close before slice 2 (rename): `timeline`.** A `timeline`
  references its animated target (`element`) and its owning `animation` as
  `ValueKind::String`, NOT `Reference`
  ([AsciiSceneParser.cpp:9555](../../src/Library/Parsers/AsciiSceneParser.cpp)), so
  those chunk→chunk edges are **invisible to the descriptor scan** — this is a
  *static* edge the scan structurally misses, not the dynamic-helper case above.
  Renaming or recreating an animated entity on the static graph would silently
  dangle the timeline. Two ways out, in order of preference: (a) promote
  `timeline.element`/`.animation` to `Reference` with an `element_type`-keyed
  reference category (the dynamic-category resolver the design already anticipates);
  or (b) until then, **slices 0–2 refuse any incremental edit / rename when the Job
  has any animation** — slice 0 does this via the O(1) `IJob::GetAnimationCount()`
  (a `timeline` keyframe declares the implicit `(default)` animation, so it counts),
  NOT a per-call document scan (an O(N) scan would defeat the O(closure·log N) cost —
  the cost gate caught exactly that). A guard test enforces it. Slice 1 must not
  claim the static graph is complete without one of these.

## 3. Architecture B — stable-object in-place apply

Objects are **never** dropped/re-added on an edit; they keep their identity
(address) and are **re-pointed in place**. Only the *entities* objects reference
(material/geometry/painter — immutable after construction,
[Object.cpp:174–198](../../src/Library/Objects/Object.cpp)) are recreated.

For an edit whose closure is computed from the shared graph (§2):

1. **Recreate the changed non-object entities** (material/geometry/painter/…)
   via the existing `Finalize` (new addresses are fine — only objects are
   address-sensitive). Per-parser reversibility (§5) governs the drop half.
2. **Re-point the stable objects in place** to the recreated entities using the
   interactive editor's primitives
   ([IObjectPriv](../../src/Library/Interfaces/IObjectPriv.h):
   `AssignMaterial`, `AssignGeometry`, `AssignModifier`, `AssignShader`,
   **`AssignRadianceMap`**, `AssignInteriorMedium`/`ClearInteriorMedium`,
   `SetUVGenerator`, `SetShadowParams`; transforms via
   `SetPosition`/`SetOrientation`/`SetStretch` + `FinalizeTransformations`).
   The re-point set must cover **every** slot the object chunk binds —
   `standard_object`/`csg_object` also carry a `radiance_map` Reference→Painter
   slot ([Job.cpp AddObject ~5276](../../src/Library/Job.cpp)), and that one is
   *recreate-and-re-wrap* (a fresh `IRadianceMap` around the recreated painter,
   with the chunk's scale/orientation), not a bare re-point — a slot omitted here
   leaves the object holding a freed entity (UAF). CSG operands are themselves
   objects re-pointed in place, so a parent `CSGObject`'s raw operand pointers
   stay valid; a geometry edit on a CSG operand must still invalidate the TLAS
   (the operand's bbox feeds its parent's), and the closure includes the parent
   CSG via its `obja`/`objb` Reference→Object edges.
3. **Run a closure-gated invariant pass.** This is NOT a verbatim reuse of
   `SceneEditor::RunObjectInvariantChain`
   ([SceneEditor.cpp:851](../../src/Library/SceneEditor/SceneEditor.cpp)): that
   function invalidates the TLAS **unconditionally**, with the "only on a spatial
   op" gate living in its *caller* (`OpNeedsSpatialRebuild`). The CST apply has no
   edit-`Op`, so slice 3 implements a NEW variant whose TLAS-invalidation is gated
   on the **closure contents** — invalidate iff the closure recreated a geometry an
   object references OR changed an object's transform/geometry-ref; bump the
   light-topology generation iff the closure changed the emitter set. The reusable
   parts (`FinalizeTransformations()`, `ResetRuntimeData()`, the light-gen bump →
   `RayCaster::AttachScene` sampler rebuild, [RayCaster.cpp:166](../../src/Library/Rendering/RayCaster.cpp))
   are mirrored; the unconditional invalidate is replaced by the closure-gated one.
   (Getting this gate right is the prerequisite for slice 4's "non-spatial skips
   the TLAS" measurement — a verbatim reuse would reproduce P1.2.)

Consequence — the item-8 central result becomes **valid**: a **non-spatial** edit
(material/painter value) re-points objects in place, changes no bbox, and leaves
the TLAS valid → no rebuild; a **spatial** edit invalidates the TLAS, paying the
O(N log N) BVH build as a genuinely separate cost. Objects stay live across the
edit → no dangling pointer (P1.1/P1.2 dissolved).

Reuse, not reinvention: this is the SceneEditor's proven in-place machinery driven
by the CST derive instead of by edit records — which is exactly the supersession
the redesign intends (the live-mutable path becomes a *derivation* of the CST).

## 4. Reversible apply plan + atomic rename + namespace

- **Explicit plan, preflighted (P1.4/P1.6).** An edit compiles to a plan —
  `recreate-entity E`, `re-point objects [..] slot S → E`, `invalidate-TLAS?`,
  `rename old→new rewriting referrers [..]`. The plan is *validated whole* before
  any mutation; **any** infeasible step (a drop that can't fully reverse, a
  referrer that can't be rewritten, a name collision) aborts the entire plan and
  returns the document unchanged. Drop return values are checked, never ignored.
- **Rename uses the shared graph + the prior name.** Rename takes the *prior*
  identity (so it removes the old engine entity, not the new — P1.4) and rewrites
  **every** referrer from the stamped graph; if any referrer is unresolved-dynamic
  or otherwise unrewritable, it **refuses atomically** (no best-effort partial —
  P1.6).
- **Name validation against the full namespace (P1.7).** Reject empty names;
  reject collisions against the *complete* derivation namespace (CST chunks + the
  `none` defaults + `Default*` shader ops), via the resolver's namespace — not a
  CST-only scan.

## 5. Per-parser reversibility (P1.3)

Reversibility is a property of the **parser/chunk type**, not the broad category.
A chunk is incrementally re-derivable only if its `Finalize` registers in exactly
the managers a typed removal clears. Classification:

- **Single-entry (re-derivable):** lambertian/most materials, analytic geometries,
  standard objects (verified single-manager — a sweep of `Job::Add*` confirms each
  makes exactly one manager `AddItem`).
- **Multi-entry (needs full-undo or refusal):** these are NOT identifiable by
  category — the classifier must be **per-parser**:
  - `pbr_metallic_roughness_material` + composed `ggx_material` create helper
    painters / fold a painter; both register in `composedMaterialNames`, so
    `IsMaterialComposed` ([IJob.h:2929](../../src/Library/Interfaces/IJob.h))
    detects them (material-only).
  - `Add*Painter` dual-registers in the function-2D manager (refused by the
    Painter-category gate today).
  - **`gltf_import` is the worst case: it is `ChunkCategory::Geometry`** but its
    `Finalize` spawns many objects/materials/painters/lights/a camera
    ([AsciiSceneParser.cpp:5164](../../src/Library/Parsers/AsciiSceneParser.cpp)).
    `IsMaterialComposed` (material-only) does NOT catch it, and the Geometry gate
    ALLOWS it — so it must be refused explicitly. The Hosek-Wilkie sky similarly
    spawns a hidden `__hw_sun__` light (currently caught only because it has no
    `name`; if a future `name` is added to it before the capability bit lands, the
    interim denylist must gain `hosek_wilkie_skylight` too).
  Because a hardcoded denylist is silently defeated by the next bulk importer, the
  **principled fix is a capability bit on `IAsciiChunkParser`** ("single-manager,
  single-entry") that the classifier reads structurally; the interim slice-0 guard
  is a keyword denylist (`gltf_import`, `translucent_material`) + `IsMaterialComposed`.
- The classifier is consulted in the plan's preflight; a non-reversible chunk in
  the closure makes the edit fall back to a full re-derive (D51: never a silent
  partial undo).

## 6. Slices (each to zero-P1 via the review loop)

**Status (2026-06-23): slices 0–4 LANDED; slice 5's maintained-graph closure primitive +
the drift guard LANDED, with the structural one-resolution-path the remaining D35 step
(see slice 5).** Each landed slice converged through the review-fix loop to zero P1s.

0. **Interim safety + honesty (lands first, supersedes nothing):** make the
   current `DeriveToJobIncremental` *safe* until B replaces it — invalidate the
   TLAS + bump light-gen on object recreation (kills the UAF), abort on a failed
   drop, refuse composed materials + the func2d painters + the ambient-cache
   reader; and *retract* the overclaims now (the no-TLAS/true-O(closure) central
   result, the O(closure)/O(N) bounds → O(closure·log N)/O(N·log N), R13). Fix the
   stale docs (slices:3, the skill reflow example). [P1.1,3,4,5,9 + retract P1.2]
1. **Shared resolver + namespace** — the resolver, the stamped `ReferenceGraph`
   recorded at derive, rebuild `DocEditClosure` on it; collisions/validation use
   the full namespace. [Root B; P1.7, foundation for P1.6/P1.8]
2. **Atomic rename on the shared graph** — prior-name + rewrite-all-or-refuse.
   [P1.6, P1.8 for rename]
3. **Stable-object in-place apply** — recreate entities + re-point stable objects
   + the invariant chain; the central result becomes valid. [Root A; P1.1,P1.2]
4. **Re-measure item 8 on the stable apply** — non-spatial edit now genuinely
   skips the TLAS; honest wall-clock + the corrected bounds. [P1.2,P1.9]
5. **Maintained-graph closure + drift guard** — `BuildReferenceGraph` computes the
   reverse adjacency (`dependents`) in its single pass, and `DocEditClosure(id, graph)`
   walks it in O(closure·log N) over a held graph (CstEditCostTest: the BFS in isolation
   is ~0.2 µs flat). The `MaintainedReferenceGraph` holder (review P1.6) keeps the graph in
   sync INCREMENTALLY: it decides reuse-vs-rebuild from the EDIT in O(log N) (a NodeId-index chunk lookup + a descriptor scan; is the edited
   param a reference, or the chunk's `name`?), **not** by recomputing the stamp — which is
   itself an O(N) `BuildReferenceGraph`, so a stamp-validated reuse would save nothing
   end-to-end. A non-reference value edit reuses the graph (no rebuild); a reference/name
   edit rebuilds. The HONEST end-to-end per-edit cost (edit + the O(log N) reuse decision +
   the closure) is ~7.6 µs flat vs ~19 ms for the from-scratch edit+O(N log N) closure at
   N=4096 — that is the number, not the isolated BFS. (Structural edits are not yet handled
   by the holder; a holder would rebuild on those.) The static graph is guarded against
   drifting from the derive by
   `CstResolverTest` [consistency]/[drift] — which cross-verify every object→material and
   object→geometry edge against the derive's ACTUAL binding (by pointer), across multiple
   objects; painter/other edges are existence-checked only (a material does not expose its
   painters for a pointer cross-check). So this is a drift DETECTOR on the tested scenes,
   NOT an exhaustive structural every-edge proof. (`CstDeriveDifferentialTest` separately
   holds CST-derive ≡ legacy-derive *Job* equivalence — a related but distinct guard.)
   **Remaining D35 step (planned in §8):** route the derive's OWN resolution
   through the recorded graph (record each edge as `DeriveToJob` resolves it) so the two
   cannot drift *even in principle* — a holder/parser-instrumentation integration beyond
   this kernel slice. [P1.8: namespace + ref-or-literal + drift-detection done; structural
   one-path remaining]

## 7. Honest cost (P1.9)

With `std::map` managers and the CST's `std::map`-backed identity, the bounds carry
log factors: a structured edit is **O(log N)**; the incremental apply is
**O(closure · log N)** (per-member identity lookup + manager remove/insert + an
O(C log C) sort), **not O(closure)**; a full derive is **O(N · log N)**, not O(N).
R13 stands: we do **not** claim flat O(closure) before persistent O(1) managers
exist — the wall-clock shows the apply is *flat in N* (C fixed, log N slow), which
is consistent with O(closure · log N), and the suite asserts only ratios/scaling,
never constant-vs-log. The TLAS rebuild on a spatial edit is the engine's
O(N log N) BVH build, reported separately and now genuinely skipped by non-spatial
edits under B.

## 8. D35 — record-during-derive (the structural one-path)

**Status (2026-06-24): KERNEL COMPLETE (slices 1–2 + a comprehensive drift guard); the
consumer default-switch is GATED on live editor consumers.** This is the remaining D35 step
from §2/§6 (slice 5): route the derive's OWN resolution through the recorded graph so the
static graph and the engine **cannot drift**. Delivered: the recorded graph IS the engine's
production+resolution (it cannot drift by construction), and a bidirectional cross-check
(`CstRecordDeriveTest`: recorded == static across a corpus spanning the major reference-bearing
managers — painter colour+scalar, material, geometry, function1d, function2d, medium, modifier,
object) DETECTS any drift of the *static* heuristic from the engine FOR THE COVERED REFERENCE
PATTERNS (a drift on a pattern outside the corpus — shader/shaderop, light — is not caught by
this runtime guard; those edges rely on the 144-param convergence proof, §6 slice-5). It is a
drift DETECTOR on the tested patterns, not an exhaustive every-edge proof. What remains (gated, NOT
speculatively built): making closure CONSUMERS default to the recorded graph + maintaining it
incrementally — there are no live closure consumers today (the editor/agent layer doesn't
exist yet), the recorded graph requires a derive the Document-only consumers don't have, and
rename always needs the param-level static path regardless. The live editor (holding
`(doc, Job, recorded)`) adopts the recorded graph for closure when it lands; the cross-check
is the no-drift guarantee until then.

**Why it's needed.** Slices 1–5 + the post-gate review rounds converged the *static*
resolver (`BuildReferenceGraph`) to match the engine for all 144 reference params — but by
**duplicating** the engine's per-param manager choice. The "which manager does this param
resolve through" fact is encoded TWICE: once in each parser's `Finalize` (`GetX()->GetItem`)
and once in the CST resolver's `FunctionSubNamespace` + painter heuristics. Those six review
rounds were all instances of those two encodings drifting (a `{Painter}`-declared slot the
engine binds via Function2D; `transfer_*`; the painter colour/scalar axis). D35 collapses the
two encodings to ONE — the engine's actual resolution — so a new param or a changed
`Finalize` cannot silently desync the graph.

**Key finding (the seam).** Every `GenericManager`-based reference lookup flows through a
single template chokepoint, `GenericManager::GetItem(name)` (production through `AddItem`)
([GenericManager.h](../../src/Library/Managers/GenericManager.h)), and it is a **setup-time**
path (name→entity resolution during derive/parse), NOT a per-ray hot path (rendering uses
resolved pointers). So the engine's actual production + resolution can be CAPTURED at one
gated hook — bounded — instead of refactoring the ~100 scattered `Job::Add*` resolution sites.
**Caveat — media bypass the chokepoint (RESOLVED in slice 2):** participating media are NOT a
`GenericManager` — they live in the Job's separate `mediaMap`, so a `(medium → object)` edge
via `interior_medium` BYPASSES the `GetItem`/`AddItem` chokepoint. Slice 1 therefore could not
record them and the slice-1 corpus excluded media. Slice 2 fixed this with a parallel `mediaMap`
hook (`Job::Add*Medium` production + `SetObjectInteriorMedium` / `SetGlobalMedium` resolution,
same pointer-keying), so media ARE now recorded and the cross-check corpus includes a medium
scene. (Had it not been recorded, a medium scene would have failed the cross-check, and editing
a medium would not re-derive its consuming objects once a consumer relied on the recorded graph.)

**Slices** (each to zero-P1 via the review loop):

1. **Record-during-derive infrastructure + drift cross-check (no consumer switch). LANDED
   (ba904a48).** Two null-by-default thread-local sinks (zero cost when off — the default):
   `GenericManager::AddItem` appends each PRODUCED entity POINTER, `GetItem`-found appends each
   RESOLVED entity POINTER. `DeriveToJob(…, ReferenceGraph* outRecorded)` PASS 2 brackets each
   chunk's `Finalize`, then maps each produced pointer to the producing chunk (a persistent
   `entity-pointer → producer NodeId` map) and, for each resolved pointer, adds a
   `(producer → consumer)` edge to `outRecorded->dependents`. Pointer identity (not
   `(manager, name)` + a category mapping) is what ties a resolution back to its producer — so
   a multiple-inheritance sub-object pointer (a colour painter seen as `IPainter` vs
   `IFunction2D`) still maps to its one producer (both AddItems run in that chunk's bracket).
   `CstRecordDeriveTest` asserts the recorded graph EQUALS the static `BuildReferenceGraph` on
   clean scenes (recorded == static, BOTH directions — slice 2 widened this across the manager
   corpus) — *catching manager-choice drift directly* (recorded = engine truth vs static =
   heuristic) while changing no behavior. Low risk: behavior-neutral (CstDeriveDifferentialTest
   20/0, CST-derive still ≡ legacy), the hooks are null-gated, the path is setup-time, and the
   recorder only compares opaque pointers (no deref / no ownership).

2. **Switch closure to the recorded graph — recording side LANDED; consumer default-switch
   GATED.** Slice 2's RECORDING side is done: media are recorded (a `mediaMap` hook in
   `Job::Add*Medium` / `SetObjectInteriorMedium` / `SetGlobalMedium`, since media bypass the
   `GenericManager` chokepoint), the cross-check is now bidirectional (`recorded == static`)
   across a corpus spanning the major reference-bearing managers (painter colour+scalar,
   material, geometry, function1d, function2d, medium, modifier, object; shader/shaderop + light
   not yet in the corpus), and closure over the recorded graph is demonstrated EQUAL to closure over the
   static graph (`CstRecordDeriveTest` [closure]). The actual consumer SWITCH (`DocEditClosure`
   / `MaintainedReferenceGraph` *default* to the recorded edges) is GATED on live consumers
   (see Status): the path is ready (`DocEditClosure(id, recorded)` works + is tested), not
   speculatively wired. Chunk-level is sufficient for closure; rename keeps its conflation
   refusals (see the chunk-level note below).

3. **Retire the heuristics — DEMOTED + GUARDED; full retirement gated.** `FunctionSubNamespace`
   is now GUARDED by the bidirectional cross-check (recorded == static guarantees it matches the
   engine), so it cannot drift undetected. The painter same-name ALIAS is NOT covered by the
   cross-check — its edges are static-only by design (the engine does not alias; they would
   break `==`, so CstRecordDeriveTest deliberately scopes them out) — but it is a CONSERVATIVE
   superset (never misses a real dependent) and order-insensitive (8f9aea29), safe by construction. They are NOT removed:
   rename always needs the param-level static path + its conflation refusals (P1.4/#3), and
   closure won't depend on the recorded graph until the consumer-switch lands. Removal-for-closure (and with it the dissolution of the painter colour/scalar residual, since
   the recorded edge is whatever `GetItem` actually returned) follows the consumer-switch.  (The
   `ior`/`film_ior` phantom-Function over-declaration was dissolved INDEPENDENTLY in workstream #2
   -- a descriptor fix dropping the spurious Function category -- needing no recorded graph, since
   the engine resolves ior via scalar-then-colour painter, never Function.)

**Chunk-level boundary (honest).** The chokepoint records the resolved ENTITY pointer, not the
SOURCE param NodeId, so the recorded graph is **chunk→chunk**, sufficient for closure. Rename needs
**param-level** edges (to rewrite the specific referrer's value) and to disambiguate which
of two same-named-cross-manager painters a slot bound — which the chokepoint cannot do (and
neither can a post-hoc match in the conflation case). So slice 2 moves only CLOSURE to the
recorded graph; rename stays on the static/heuristic edges + its conflation refusals (P1.4 /
#3), exactly as today. Param-level recording (resolution lifted to the apply layer, passing
the param NodeId through) is a later step, only if rename ever needs to resolve a conflation
rather than refuse it.

**Slice-2 recording prerequisites — DONE.** These had to land before a consumer could rely on
the recorded graph; all three are addressed:
- **Media recording — DONE.** `interior_medium` / `global_medium` bypass the `GenericManager`
  chokepoint (media live in `Job::mediaMap`); recorded via hooks in `Job::Add*Medium`
  (production) + `SetObjectInteriorMedium` / `SetGlobalMedium` (resolution), keyed by the same
  entity pointer. A medium scene is in `CstRecordDeriveTest`.
- **Realize-time resolution — CONFIRMED none escapes.** Deferred realization at
  `RayCaster::AttachScene` BAKES geometry (displacement / CSG); it resolves no references
  (those are bound at Finalize/derive, inside the recording bracket). The cross-check's
  displacement scene confirms the function2d edge IS captured at derive-time (recorded ==
  static), so nothing leaks past the bracket.
- **Cross-check coverage — DONE for the major managers.** The cross-check is now bidirectional
  (`recorded == static`, catching static-OVER AND static-MISS) across a corpus spanning painter
  colour+scalar, material, geometry, function1d, function2d, medium, modifier, and object; no
  spurious recorded edge appears on the clean corpus. shader/shaderop + light reference edges
  are NOT yet in the corpus (the 144-param convergence proof covers them; adding a shader/
  shaderop scene -- shaderop resolves through the same chokepoint -- would close the runtime gap).
