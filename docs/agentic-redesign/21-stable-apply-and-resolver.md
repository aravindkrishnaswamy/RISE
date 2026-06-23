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

- **Namespace = managers + runtime defaults.** The resolver consults the live
  managers (which already hold the `none` material/painter added at
  [Job.cpp:351,356](../../src/Library/Job.cpp) and the `Default*` shader ops at
  357–373), not a CST-only view. So `none`/`Default*` are first-class names that
  collisions and resolutions see.
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
   same primitives the interactive editor uses
   ([IObjectPriv](../../src/Library/Interfaces/IObjectPriv.h):
   `AssignMaterial`, `AssignGeometry`, `AssignModifier`, `AssignShader`,
   `AssignInteriorMedium`/`ClearInteriorMedium`, `SetUVGenerator`,
   `SetShadowParams`; transforms via `SetPosition`/`SetOrientation`/`SetStretch`
   + `FinalizeTransformations`).
3. **Run the invariant chain**, mirroring `SceneEditor::RunObjectInvariantChain`
   ([SceneEditor.cpp:851](../../src/Library/SceneEditor/SceneEditor.cpp)):
   `FinalizeTransformations()` → `ResetRuntimeData()` → **invalidate the TLAS
   only if a bbox changed** (geometry shape / object transform / geometry-ref:
   `InvalidateSpatialStructure()`) → **bump the light-topology generation only if
   the emitter set changed** (`BumpLightTopologyGeneration` →
   `RayCaster::AttachScene` rebuilds samplers on the stamp mismatch,
   [RayCaster.cpp:166](../../src/Library/Rendering/RayCaster.cpp)).

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
  standard objects (verified single-manager).
- **Multi-entry (needs full-undo or refusal):** `pbr_metallic_roughness_material`
  creates ~15 helper painters `__pbrmr_<name>__*`
  ([Job.cpp:3749](../../src/Library/Job.cpp)); detect via `IsMaterialComposed`
  ([IJob.h:2929](../../src/Library/Interfaces/IJob.h)) and either remove all
  helpers (a composed-removal that walks the prefix registry) or **refuse**.
  `Add*Painter` dual-registers in the function-2D manager (the known painter case)
  → refuse until the multi-manager removal lands.
- The classifier is consulted in the plan's preflight; a non-reversible chunk in
  the closure makes the edit fall back to a full re-derive (D51: never a silent
  partial undo).

## 6. Slices (each to zero-P1 via the review loop)

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
5. **Route parser lookups through the resolver** — so the recorded graph and the
   apply resolution cannot drift even in principle (full D35). [P1.8 complete]

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
