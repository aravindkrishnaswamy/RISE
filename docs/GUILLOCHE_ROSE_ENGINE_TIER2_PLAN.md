# Guilloché / Rose-Engine Fidelity — 3-Way Study (bump vs displaced-mesh vs SDF)

Status: **Phase 0 complete (measured 2026-06-13); 3-way build underway.** Target branch
`feature/thin-film-interference`. Decision record for taking the GuillocheWatch dial from a
phenomenological displacement field to a **physically-grounded, geometry-faithful** engine-turned
surface — and for **objectively measuring** which realization (cheap normal-perturbation bump,
memory-bounded displaced mesh, or exact SDF ground truth) actually earns its cost.

**Pivot (2026-06-13):** Phase 0 found a full-dial fine-pitch mesh hits a **19–43 GB bake-memory wall**
(§7). Rather than abandon authentic pitch or pick a path by dogma, the plan now **builds all three
realizations of the same kinematic field and runs a deep 3-way comparison**, with the SDF as ground
truth. Bump maps are back in scope — *as a measured candidate*, not an article of faith.

Reference object: the **Ming × J.N. Shapiro 37.06 "Lightning"** — a heat-coloured (anodised)
engine-turned titanium dial. The heat-colour we already model well (real TiO₂-on-Ti thin-film
interference); this plan is about the *engraving*.

---

## 1. How a real rose engine works (the thing we are imitating)

A rose engine is a **rocking/pumping lathe**, fundamentally unlike a normal lathe: a **fixed
single-point cutter** is held still while the **workpiece oscillates** against it.

- **Rosettes** — hardened cam discs on the spindle whose rim is cut to a wave/scallop profile.
  A follower ("rubber") rides the selected rosette; as the spindle turns, the rosette **rocks the
  headstock transversely** (rose-engine proper) and/or **pumps it axially**. The rosette is
  effectively a periodic function of spindle angle — an *N*-lobe rosette ≈ `A·g(Nθ)`.
- **One closed wavy line per spindle revolution.** The rock modulates the cut's radial position
  around the turn, so a single revolution inscribes one rosette-shaped closed curve.
- **Indexing + feed** offset the next line — a small radial feed (the line *pitch*) and/or an
  angular **phase shift**. Hundreds of these accumulate into the field. The *phase relationship
  between adjacent lines* distinguishes the families: in-phase concentric, phase-shifted
  **barleycorn**, two crossing families → **clous-de-Paris / hobnail**, basketweave, sunburst, moiré.
- **The cut** — a single-point graver ground to a fine point leaves "the characteristic
  **V-section profile**," cut "**a few hundredths of a millimetre**" deep, i.e. **tens of microns**
  (≈ 40–130 µm); Shapiro's sectors are "aligned to the micron."
- **Optical signature (the point of it all)** — the faceted micro-relief "scatters incident light
  across **thousands of minute surfaces**… creating an almost **living optical depth**." Two
  ingredients: (a) **macro V-facets** — each groove wall is a small *smooth, polished* plane at a
  definite angle, so as the dial tilts the wall that bisects eye/light **flashes**, and because the
  walls follow the wavy loci the highlight **sweeps along the groove direction**; (b) **sub-µm
  directional finish** left by the tool, an **anisotropic sheen** along the cut.

The signature is therefore **coherent, directional, sweeping specular flashes off smooth V-facets**
— not the macro pattern alone.

Sources: [Rose engine lathe (Wikipedia)](https://en.wikipedia.org/wiki/Rose_engine_lathe),
[The Geometry Behind Guilloché (Skyjems)](https://skyjems.ca/pages/encyclopedia-rose-engine-lathe),
[Guilloché Pattern (Wolfram MathWorld)](https://mathworld.wolfram.com/GuillochePattern.html),
[Guilloché Dial (Skyjems)](https://skyjems.ca/pages/encyclopedia-guilloche-dial),
[Ming × J.N. Shapiro 37.06 Lightning (Monochrome)](https://monochrome-watches.com/ming-x-jn-shapiro-37-06-lightning-guilloche-titanium-dial-heat-coloured-introducing-price/),
[The Secrets of Horological Engine-Turning, J. Shapiro](https://www.jnshapirowatches.com/video/the-secrets-of-horological-engine-turning-guilloche-by-joshua-shapiro/).

---

## 2. What we do today (evidence-backed, with file:line)

Active dial in `scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene`: object `dial` (≈line 1430)
→ geometry `dialmesh` (≈1367) → displacement painter `dialfn_uniform` (≈1011), `disp_scale 0.18`,
`face_normals TRUE`, `uv_seam_fold FALSE`, base `dialdisk`
(`cartesian_disk_geometry radius 20.6 mesh_n 880`, ≈1003).

- **Cross-section**: the atom is `Stripe(arg) = clamp(|cos(τ·arg)| / gridE1, 0, 1)` with `gridE1=0.5`
  (`tests/GuillocheDialExpr.h:37-40`) — **flat land plateau** (clamp-saturated where |cos|≥0.5),
  **linear V-walls**, **sharp cusp floor** at |cos|=0. The 2D field is the product of two
  perpendicular Stripes (`AddWoven`, `GuillocheDialExpr.h:45-54`), evaluated in a **per-angular-sector
  rotated frame** — so cells run radially and flip handedness at each sector seam.
- **Loci**: mostly **sector-rotated square lattices + rosette masks** (uniform/radial/varwidth);
  `lightning` = zigzag rays + brick ground; **`swirl` = a true log-spiral** (the only kinematic one);
  `iris` = spiralling half-plane blades. The repeated grooves are **not** continuous rosette-modulated
  closed lines.
- **Displacement mechanics**: `vertex += N · f(u,v) · disp_scale`
  (`src/Library/Geometry/GeometryUtilities.cpp:523-524`); base normal `(0,0,1)` → relief in +Z.
  Disk is a regular Cartesian lattice (`RISE_API.cpp:807-866`) — **uniform world-space spacing**,
  chosen to kill the polar centre-collapse (`THIN_FILM_INTERFERENCE.md:461-470`).
- **Normals**: `face_normals TRUE` → **flat-shaded per-triangle facets** (no topology-averaging;
  `TriangleMeshGeometryIndexed.cpp:123-127`).
- **Material `tf_dial`** (≈710): `ggx_material`, **isotropic `alphax=alphay=0.08`**, `fresnel_mode
  thinfilm` (TiO₂-on-Ti), `film_thickness oxide_thk` (calibrated nm). **No `tangent_rotation`,
  no groove-direction field.** The engraved look is *purely* the V-facet geometry catching light
  plus the per-facet thin-film colour.

### Scale & resolution (the numbers)

`scene_unit 0.00079167` m ⇒ **1 unit = 0.79 mm**.

| Quantity | scene units | physical |
|---|---|---|
| dial radius `R` | 20.6 | **16.3 mm** (⌀ 32.6 mm) |
| vertex spacing (`mesh_n 880`) | 0.0469 | **37 µm** |
| displacement amplitude (relief-compressed ×0.85) | 0.153 | **~121 µm** land→floor |
| uniform groove pitch (`cell 0.9`) | 0.9 | **~0.71 mm** |
| **vertices per groove period** | 0.9/0.0469 | **~19** |

---

## 3. Critical analysis — current vs reality

| Dimension | Real rose engine | Today | Grade |
|---|---|---|---|
| Cross-section | sharp V, flat land between | flat land + linear-V + sharp floor | ✅ faithful |
| Depth | 40–130 µm | ~121 µm | ✅ in range (deep end) |
| Geometric resolution | continuous | ~19 verts/groove | ✅ resolved |
| Centre handling | n/a | Cartesian disk | ✅ correct |
| Heat colour | anodise thin-film | real TiO₂/Ti, calibrated | ✅✅ excellent |
| **Groove loci / generation** | rosette-modulated *continuous* lines, phase-indexed | sector-rotated square lattices (swirl excepted) | ❌ phenomenological |
| **Groove pitch / fineness** | ~0.10–0.30 mm | ~0.71 mm | ⚠️ 2–7× too coarse |
| **Wall optics (the flash)** | smooth polished walls → sweeping highlight | `face_normals` → stepped facets | ⚠️ blocky, not swept |
| **Directional sheen** | anisotropic finish along the cut | isotropic GGX, no tangent field | ❌ absent (designed at `THIN_FILM_INTERFERENCE.md:103`, never shipped) |
| V-angle calibration | graver geometry | incidental (depth/width) | ⚠️ arbitrary |

**Verdict:** macro ornament + heat-colour are faithful; the *soul* — directional, sweeping flashes
off continuous rosette-cut grooves — is the weak part. The primary levers are **real-geometry**
problems (coarse pitch; faceted walls) plus the missing **anisotropic finish**.

---

## 4. Design principles for Tier 2

1. **One kinematic field, three realizations, measured against ground truth (revised 2026-06-13).**
   The Phase-0 memory wall (full-dial fine-pitch mesh = 19–43 GB bake) makes the "real geometry only"
   stance untenable at authentic pitch. So instead of *excluding* the cheap path by dogma, we **build
   all three and measure them**: (a) **bump/normal map** — the same height field perturbing the
   shading normal, O(1) memory, but lies about geometry (no occlusion/parallax, wrong silhouette,
   incoherent grazing/shadow); (b) **displaced mesh** — real geometry, memory-bounded to a safe pitch;
   (c) **SDF swept-V** — exact geometry, the ground truth. The honest question — *does the expensive
   geometry actually matter for this dial, or does the bump map fool the eye?* — is answered by a
   **number** (§9), not an aesthetic prior. The sub-µm cutter finish is shared by all three:
   **anisotropic roughness** (`tangent_rotation` + `alphax≠alphay`), the principled microfacet model.
2. **Physical generation.** Replace the sector-rotated lattices with the actual kinematics:
   rosette-modulated continuous loci, calibrated V cross-section, phase-indexed families.
3. **Hybrid calibration.** Physically-grounded defaults (pitch/depth/V-angle in µm), exposed as
   knobs so a hero shot can push past realism.
4. **Keep it procedural & general.** Evolve the existing C++ expression-builder
   (`tests/GuillocheDialExpr.h`) to emit physical loci from parameters — no committed baked assets,
   named by technique (engine-turning), reusable beyond this dial.

---

## 5. Architecture — one kinematic field, three realizations

### 5.1 The kinematic model (the math we encode in expressions)

A groove line at base radius `R_i`: `r(θ) = R_i + A·g(Nθ + ψ_i)` — rosette waveform `g` (`N` lobes,
selectable profile), rock amplitude `A`, per-line phase `ψ_i`. Evaluate as a **phase field** rather
than enumerating lines:

```
phase(r,θ) = ( r − A·g(Nθ + ψ(r,θ)) ) / pitch
depth(r,θ) = D · Vprofile( frac(phase) )           # carved into the surface
height(r,θ) = envelope(r,θ) − depth(r,θ)
groove_dir(r,θ) = tangent of the locus  ≈  ⟂∇depth  # radians, for anisotropy
```

- **Families** from the phase: `ψ` const → concentric; `ψ=α·r` → barleycorn/drift; add
  `swirlTurns·log r` → spirals; **product/min of two phase fields** (different `N`, rotated) →
  basketweave / clous-de-Paris.
- **`Vprofile`** — *calibrated* V: flat land for `|t|>land_frac`, linear walls to a sharp floor,
  with the **half-angle a real parameter** (`tan(half_angle)=w/D`). Generalises today's `clamp(|cos|/0.5)`.
- **`envelope`** — the slow dial form (slight dish + sector/petal structure). This + the grooves are
  *all real displacement* on one mesh (no macro/micro bump split — that idea is retired with bump).

### 5.2 RISE wiring (all confirmed in-tree)

| Concern | Mechanism | Status |
|---|---|---|
| Pattern generation | evolve `tests/GuillocheDialExpr.h` → emit physical `height` + `groove_dir` from calibrated params | evolve |
| Real groove geometry | `expression_function2d(height)` → `displaced_geometry` on a **fine** `cartesian_disk` (Phase-0-sized `mesh_n`) | exists |
| Directional sheen | `expression_function2d(groove_dir)` → GGX **`tangent_rotation`** + anisotropic `alphax≠alphay` | **BRDF+Job done & validated**; one **parser gap** — see §7 Phase 0 findings |
| Heat colour | existing TiO₂/Ti thin-film + `oxide_thk` | keep |

The whole dial stays **procedural and parametric**; nothing baked to disk.

### 5.3 The cost reality

Real ~0.15 mm grooves well-resolved on a ⌀32.6 mm dial ⇒ **tens of millions of triangles**
(memory + build + BVH). That price is accepted as the cost of doing it honestly; the deferral work
(commits `d8911ac3`/`b29bb8ce`) means the bake is on-demand. Phase 0 finds the resolution where the
groove **normal distribution converges** so we spend the minimum triangles that optically resolve
the cut.

---

## 6. Calibration (hybrid)

A documented defaults table (sibling to `oxide_calibration.txt`, which calibrates *colour* not depth):
pitch **0.10–0.30 mm**, depth **40–130 µm**, **V included-angle** from graver geometry, line phase per
family — grounded in engine-turning references + the Lightning. All are builder knobs; record the
**µm → scene-unit** mapping explicitly.

---

## 7. Implementation phases

- **Phase 0 — measurement & de-risk (read/measure/micro-bench; no feature code).**
  - Mesh-resolution sweep: find `mesh_n` where the groove **NDF / per-pixel normal field converges**
    at calibrated pitch; record triangle count, build time, peak RSS.
  - **SDF feasibility probe** (for the Phase-4 benchmark): trace cost of a swept-V groove field at the
    same fidelity (memory-light / trace-heavy — the opposite cost profile to the mesh).
  - Confirm `tangent_rotation` painter authoring end-to-end on GGX; confirm TBN/tangent on the
    displaced Cartesian disk is correct so anisotropy aligns to the grooves.
  - Confirm a finely displaced dial round-trips through the deferral path (realize, render, free).

### Phase 0 findings (recorded as measured)

**P0-A — groove-aligned anisotropy is authorable end-to-end, with one precisely-scoped gap (the central de-risk; GREEN).**
The full chain is *already in-tree and validated*:
1. `cartesian_disk` emits linear UV ((x,y)/2R), so `TriangleMeshGeometry`
   (`TriangleMeshGeometry.cpp:523-546`) derives a **coherent** `sd.dpdu` (≈ world-x) across the
   whole dial — a stable base tangent for anisotropy to rotate from.
2. `DisplacedGeometry` **preserves UVs** through the bake (`DisplacedGeometry.cpp:247`
   `m_pMesh->AddTexCoords(coords)`), so the displaced mesh keeps that frame.
3. The GGX BRDF **rotates** that tangent by a `tangent_rotation` painter
   (`GGXBRDF.cpp` `ResolveTangentONB`→`RotateTangent`); `Job::AddGGXMaterial` (`Job.cpp:3435`)
   already resolves a painter/scalar and forwards it to the GGX ctor. This path is exercised and
   validated by the KHR-anisotropy round-2 fix in `AddPBRMetallicRoughnessMaterial`
   (`Job.cpp:3850-3857`: `anisotropy_rotation` → `tangent_rotation` → GGX). **The parser Describe
   comment at `AsciiSceneParser.cpp:3890` ("reads but does not yet APPLY … wired in L12") is STALE**
   — the rotation *is* applied.
4. **The only gap:** the dial's chunk, `ggx_material`, hard-codes `tangent_rotation="none"` at its
   `AddGGXMaterial` call (`AsciiSceneParser.cpp:3784`) and doesn't expose the param. It *does* expose
   `alphax`/`alphay` directly (3817-3818), so anisotropic α is authorable today but locked to the
   base tangent (un-steerable). → **Fix is a ~3-line append-only parser exposure** (add a
   `tangent_rotation` Describe param defaulting `"none"`, read it, pass instead of the literal
   `"none"`). No BRDF/material/ABI change. Lands in **Phase 2** with the dial wiring; default `"none"`
   keeps every existing `ggx_material` scene bit-identical.

**P0-D — deferral round-trip: SATISFIED by construction.** The dial geometry is a `DisplacedGeometry`,
which is exactly the type Phase-1 deferral covers (realize-pass bake; `DeferredRealizeTest` already
proves displaced realize/render/free). A finer `mesh_n` only changes triangle count, not the path.

**P0-B — mesh-resolution NDF sweep (measured; `var_test/p0_mesh_sweep.py`, single-frequency 0.20 mm-pitch
V-groove patch, face_normals TRUE, sharp conductor under a grazing across-groove key — the most
stringent normal-sensitivity test).** Convergence read on a location-free luminance-histogram EMD (the
NDF signature) vs the moving-firefly per-pixel RMSE:

| verts / pitch | histEMD vs prev | blurred-envelope Δ |
|---|---|---|
| 6  | 0.0183 | — |
| **12** | **0.0024** (7.5× drop — knee) | still refining |
| 24 | 0.0030 | refining |
| 36 | 0.0014 | — |
| 48–97 | 0.0009–0.0013 (MC noise floor) | slowly settling |

→ **The groove normal-distribution converges at ~12 verts/pitch; sharp-highlight *position* keeps
refining, so ~16–24 v/pitch is the safe production target.** Mapping to the full ⌀ dial (R = 20.6) at the
**target 0.20 mm pitch**: 16 v/p ⇒ `mesh_n ≈ 2600` (~11 M tris); 24 v/p ⇒ `mesh_n ≈ 3900` (~24 M tris).
(The *current* coarse dial at 0.71 mm pitch / `mesh_n 880` already sits at ~19 v/p — adequately
resolved *for its pitch*; the fidelity gap is the pitch being 3–7× too coarse, **not** under-tessellation.)

**P0-B memory wall (the decisive constraint).** Measured peak RSS (`/usr/bin/time -l`): the probe at
`mesh_n 1536` (2.36 M tris) peaks at **4.19 GB** ⇒ **~1.78 KB / triangle** (bake-time coord/normal copies
in `DisplacedGeometry::BuildMesh` + per-mesh BVH scratch). Extrapolated, a full-dial 0.20 mm-pitch mesh
(11–24 M tris) would peak at **~19–43 GB transient bake RSS** — at/over the ceiling of a typical machine.
Deferral makes it *transient* (only the rendered dial bakes, freed after) but does not lower the peak.
**Tier-2 at true fine pitch must therefore do one of:** (a) accept a coarser ~0.30 mm pitch (≈ 4.7 M tris
→ ~8 GB); (b) cut the ~1.78 KB/tri bake overhead (it is bloated — streaming / in-place displacement
could halve it); (c) region-tessellate only the visible dial annulus; or (d) lean on the SDF (Tier 3),
which sidesteps the wall. This makes the **SDF benchmark potentially load-bearing, not optional.**

**P0-C — SDF cost-profile probe (measured) + a blocker for the swept-V benchmark.** Two findings:
1. **No periodic/swept SDF primitive exists today.** `SDFGeometry::ParsePartLines`
   (`SDFGeometry.cpp:1262`) implements only `sphere|box|roundbox|cylinder|torus|capsule|roundcone`;
   `gyroid`/`menger` appear in the parser enum but are **not** wired in the part parser. A faithful
   swept-V groove field is therefore **not authorable with current parts** — building it *is* the Tier-3
   work (a new swept/periodic primitive), so the apples-to-apples cost comparison belongs to Phase 5,
   not Phase 0.
2. **Cost-profile contrast confirmed** (the input to the Tier-3 decision): an existing SDF scene
   (`sdf_shadows`) renders in 2.2 s at **0.30 GB** peak RSS vs the 2.36 M-tri mesh's 4.1 s at **4.19 GB**
   — ~14× less memory at this scale, and *constant* in surface complexity (the SDF stores ~a dozen part
   params; the mesh stores millions of verts+normals+BVH). Trace time is comparable here but a *fine*
   swept-V SDF will be march-heavier — the canonical "memory-light / trace-heavy" profile, the opposite
   of the mesh. This is exactly the trade the Phase-5 benchmark must quantify.
Restructured 2026-06-13 around the **3-way comparison** (one kinematic field; bump / mesh / SDF):

- **Phase 1 — shared kinematic generator** (feeds ALL three realizations). Evolve
  `GuillocheDialExpr.h` to emit a physical `height(u,v)` field + `groove_dir(u,v)` from calibrated
  rose-engine params (rosette-modulated continuous loci, calibrated V cross-section, phase-indexed
  families); oracle-test the math. **Not blocked by the bump/mesh/SDF fork — all three consume it.**
- **Phase 2 — anisotropy enabler.** Expose `tangent_rotation` on `ggx_material` (the ~3-line P0-A
  parser gap) so the shared thin-film material can steer groove-aligned anisotropy; parser test.
- **Phase 3 — three dial realizations of the same field, each with the shared anisotropic thin-film:**
  - **3a Bump** — flat disk + normal perturbation from `∇height` (`bumpmap_modifier` / normal-map).
    O(1) memory; the "does it fool the eye" candidate.
  - **3b Displaced mesh** — displaced disk at the memory-safe pitch (~0.30 mm, P0-B), `face_normals`.
  - **3c SDF swept-V** — **new** periodic/swept-V SDF part in `SDFGeometry` + `ParsePartLines`
    (P0-C found none exists) + the 5 build projects. Exact geometry = the ground truth.
- **Phase 4 — calibration pass + knobs** (shared µm→scene-unit table across all three).
- **Phase 5 — the deep 3-way objective comparison (§9).** L_bump vs L_mesh vs L_sdf under identical
  camera/light/spp; NDF/normal accuracy, flash tilt-series, image distance (RMSE/SSIM/ꟻLIP) with the
  SDF as ground truth. Output: a number that says which realization earns its cost.

Per phase: build clean (make + Xcode, 0 warnings), suite green, multi-round adversarial review;
controller commits; **user pushes**.

---

## 8. Test plan (unit / oracle)

- **Model math oracles:** at sampled (r,θ), `depth` matches the closed-form V at known loci;
  `frac(phase)` periodicity = pitch; `groove_dir ⟂ ∇depth`; family selectors produce concentric vs
  phase-shifted loci; µm calibration (depth/pitch scene-units ↔ expected mm).
- **Cross-section:** flat-land fraction, wall linearity, sharp floor, V-angle for given (D,w).
- **No-regression:** GGX furnace/reciprocity still pass (anisotropy is existing, tested); thin-film
  oracle unchanged; the deferral suite (Tests A–E) unchanged.

---

## 9. Verification plan — objective fidelity (the part that must be measured, not eyeballed)

A **fidelity ladder** rendered under identical camera/light/spp (converged), scripted like the
variance/HDR harness:

The 3-way ladder, all sharing the same kinematic field + anisotropic thin-film material:
**L0 = current** (isotropic, phenomenological — the regression baseline) → **L_bump** (3a: flat disk +
∇height normal perturbation) → **L_mesh** (3b: displaced mesh at memory-safe pitch) → **L_sdf** (3c:
exact swept-V SDF — **the ground truth**).

Metrics (each candidate scored *against L_sdf*):
1. **Normal-field accuracy / NDF** — per-pixel normal angular error + slope-distribution histogram vs
   the SDF's exact normals. Bump should diverge first (it perturbs a flat normal); mesh should track
   until its faceting limit. "Is the groove geometry optically resolved — and does the *fake* one lie?"
2. **Flash behaviour (tilt series)** — sweep light/dial through N angles; per frame measure highlight
   **streak elongation** (along- vs across-groove) and **coherence** (specular-mask autocorrelation).
   "Does it flash directionally and *sweep* like engine-turning?" L0 fails; the key question is whether
   **bump's flash sweeps like the real geometry's or betrays itself** (no occlusion/parallax at grazing).
3. **Image distance vs ground truth (the decision metric)** — L_bump-vs-L_sdf and L_mesh-vs-L_sdf:
   RMSE + SSIM + a perceptual metric (ꟻLIP / ΔE), globally **and at grazing angles** (where bump's lack
   of occlusion/parallax and the mesh's faceting should diverge from the exact SDF most).
4. **Cost** — render time + peak RSS per realization (the Phase-0 numbers, now measured on the real dial).
5. **Reference sanity** — qualitative/ΔE vs a Lightning photo (uncontrolled lighting → sanity, not a gate).

**Pre-registered decision rule:** rank by metric 3 at face-on (the dial's normal viewing) *and* at
grazing. If **L_bump** is within a set perceptual threshold of L_sdf face-on, the cheap path wins for
stills and the memory wall is moot — record *by how much* it diverges at grazing. If only **L_mesh**
closes the gap, the geometry matters and we ship mesh at the safe pitch. If **only L_sdf** satisfies at
authentic fine pitch, the SDF is the production path. The output is a **table of numbers**, not an opinion.

---

## 10. The SDF realization (ground truth — ELEVATED, built now)

We already ship `sdf_geometry` (sphere-tracer), but **Phase-0 P0-C found it has no periodic/swept
primitive** — `ParsePartLines` implements only sphere/box/roundbox/cylinder/torus/capsule/roundcone, so
**Phase-3c's first task is a new swept-V / periodic SDF part**: the dial as **flat base − swept-V cutter
volume** along the kinematic loci (exact sharp V, arbitrary fine pitch, no tessellation limit). It wires
through `SDFGeometry` + `ParsePartLines` + the 5 build projects, and reuses the analytic-distance
discipline already proven by the SDF feature (subtract/intersect conservativeness, front/back flags,
marching-tet mesher for area sampling).

Measured profile (P0-C): SDF ~0.30 GB vs the 2.36 M-tri mesh's 4.19 GB and *constant* in surface
complexity — memory-light, trace-heavy. **Elevated 2026-06-13** from back-pocket benchmark to a
**first-class realization in the 3-way study** (user direction): it is both the **ground truth** the
bump and mesh candidates are scored against (§9) *and* the candidate production path for authentic
0.10–0.20 mm pitch, where the mesh's memory wall (§7) rules the mesh out.

---

## 11. Risks
- **⚠ Bake memory wall (Phase-0 measured, ESCALATED to top risk).** A full-dial 0.20 mm-pitch mesh is
  11–24 M tris ⇒ **~19–43 GB transient bake RSS** at the measured ~1.78 KB/tri (§7 P0-B). At/over a
  typical machine's ceiling. Mitigations in priority order: coarser ~0.30 mm pitch (~8 GB) for the
  production default; trim the `BuildMesh` per-triangle overhead; region-tessellate the visible annulus;
  or route hero stills through the SDF (Tier 3). **This is now the gating constraint on how fine Tier 2
  can go**, and it materially raises the value of the Tier-3 benchmark.
- **Triangle budget** — Phase-0-sized: NDF converges at ~12 v/pitch, ~16–24 v/pitch for sharp
  highlights (§7 P0-B). RESOLVED as a *number*; the open question is the memory wall above, not the count.
- **TBN/tangent correctness** on the displaced Cartesian disk — **RESOLVED (Phase-0 P0-A):** disk linear
  UV → coherent `dpdu`, preserved through the displaced bake; the one gap is exposing `tangent_rotation`
  on `ggx_material` (~3-line parser add, lands in P2).
- **`groove_dir` per family** — analytic tangent differs per pattern (swirl/iris trickier than
  concentric). Open; Phase 1 generator work.
- **V-angle vs render read** — calibrated defaults may need art-direction for the camera/lighting rig.

---

## 12. Decisions captured
- **Hybrid calibration** (physical defaults + knobs).
- **Doc-first, then Phase 0 measurement bake-off.**
- **Phase-0 outcome (measured 2026-06-13):** anisotropy authoring is viable end-to-end (one ~3-line
  parser exposure, P0-A); the groove NDF converges at ~12–24 v/pitch (P0-B); but a full-dial fine-pitch
  mesh hits a **~19–43 GB bake-memory wall** (P0-B) and the SDF has **no swept primitive yet** (P0-C).
- **⟳ PIVOT (2026-06-13, user direction, supersedes the two struck items below):** the memory wall
  makes "real geometry only" untenable at authentic pitch, so **do a deep 3-way comparison instead of
  picking a path by dogma.** Build **bump** (reinstated — memory-light, *measured* not assumed),
  **displaced mesh** (memory-safe ~0.30 mm pitch), and **SDF swept-V** (elevated to *now*; the ground
  truth). The §9 ladder outputs a *table of numbers* on which realization earns its cost face-on and at
  grazing. ~~Bump/normal mapping excluded as an off-brand hack~~ — *reversed*: bump is a legitimate
  candidate to falsify by measurement. ~~Tier 2 production = fine displaced mesh; SDF = back-pocket
  benchmark~~ — *reversed*: the production winner is whatever the 3-way comparison selects.
- No feature code landed in Phase 0 (probe harness in `var_test/`, gitignored).
