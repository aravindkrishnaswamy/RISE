# Guilloché / Rose-Engine Fidelity — Tier 2 Plan (+ Tier 3 SDF benchmark)

Status: **PLAN (planning only; no feature code yet).** Target branch `feature/thin-film-interference`.
Decision record for taking the GuillocheWatch dial from a phenomenological displacement
field to a **physically-grounded, geometry-faithful** engine-turned surface.

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

1. **Geometry for what is resolvable; statistical microfacet BRDF for what is not. No bump maps.**
   Grooves (~0.1–0.3 mm) are resolvable → **real displaced geometry**. The sub-µm cutter finish is
   below any feasible mesh → **anisotropic roughness** (`tangent_rotation` + `alphax≠alphay`), the
   principled, energy-conserving microfacet model — *not* a normal-perturbation hack. There is no
   middle scale, so there is no place for bump/normal mapping: it lies about geometry (no occlusion,
   no parallax, wrong silhouette, incoherent grazing/shadow behaviour) and is off-brand for a
   physical spectral path tracer. **Excluded by design.**
2. **Physical generation.** Replace the sector-rotated lattices with the actual kinematics:
   rosette-modulated continuous loci, calibrated V cross-section, phase-indexed families.
3. **Hybrid calibration.** Physically-grounded defaults (pitch/depth/V-angle in µm), exposed as
   knobs so a hero shot can push past realism.
4. **Keep it procedural & general.** Evolve the existing C++ expression-builder
   (`tests/GuillocheDialExpr.h`) to emit physical loci from parameters — no committed baked assets,
   named by technique (engine-turning), reusable beyond this dial.

---

## 5. Tier 2 architecture (real fine mesh; no bump)

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
| Directional sheen | `expression_function2d(groove_dir)` → GGX **`tangent_rotation`** (`Job.cpp:3435,3523`) + anisotropic `alphax≠alphay` | exists |
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
- **Phase 1 — physical generator.** Evolve `GuillocheDialExpr.h` to emit `height` + `groove_dir` from
  calibrated rose-engine params; oracle-test the math.
- **Phase 2 — wire the dial.** Fine displaced mesh + anisotropic GGX (`tangent_rotation`), keep
  thin-film; re-derive the patterns as physical loci.
- **Phase 3 — calibration pass + knobs.**
- **Phase 4 — verification (§9).**
- **Phase 5 (optional/back-pocket) — Tier 3 SDF benchmark + objective comparison (§10).**

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

**L0 = current** (isotropic, phenomenological) → **L2 = Tier 2** (fine mesh + groove anisotropy) →
**L3 = Tier 3 SDF** (exact swept-V geometry — the ground truth, built in Phase 5).

Metrics:
1. **Normal-field accuracy / NDF** — L2's mesh normals vs a super-fine mesh oracle (and later vs L3):
   per-pixel normal angular error + slope-distribution histogram. "Is the groove geometry optically
   resolved?"
2. **Flash behaviour (tilt series)** — sweep light/dial through N angles; per frame measure highlight
   **streak elongation** (along-groove vs across) and **coherence** (specular-mask autocorrelation).
   "Does it flash directionally and *sweep* like engine-turning?" L0 should fail; L2 should pass.
3. **L2-vs-L3 image distance (the decision metric)** — identical render, Tier 2 vs Tier 3: RMSE +
   SSIM + a perceptual metric (ꟻLIP / ΔE), globally and **at grazing angles** (where the mesh's
   finite resolution / faceting should diverge from the exact SDF most).
4. **Reference sanity** — qualitative/ΔE vs a Lightning photo (uncontrolled lighting → sanity check,
   not a gate).

**Pre-registered Tier-3 decision rule:** after L3 exists, run metrics 1+3 L2-vs-L3. If perceptual
distance is below a set threshold (especially face-on, the dial's normal viewing), **L2 is declared
sufficient and Tier 3 is shelved**; if L3 measurably wins, record *by how much* and decide whether a
hero-only SDF path earns its trace cost. The output is a **number**, not an opinion.

---

## 10. Tier 3 (SDF) — exactness benchmark

We already ship `sdf_geometry` (sphere-tracer). Tier 3 = dial as **flat base − swept-V cutter volume**
along the kinematic loci: exact sharp V, arbitrary fine pitch, no tessellation limit; memory-light,
trace-heavy. Scope it **only as the Phase-4 ground truth** for the objective L2-vs-L3 comparison —
promoted to a production path only if §9 metric 3 says the exactness measurably matters.

---

## 11. Risks
- **Triangle budget** of a finely displaced dial (memory/build) — Phase 0 sizes it to the minimum
  that resolves the cut.
- **TBN/tangent correctness** on the displaced Cartesian disk — anisotropy must align to grooves
  (Phase 0).
- **`groove_dir` per family** — analytic tangent differs per pattern (swirl/iris trickier than
  concentric).
- **V-angle vs render read** — calibrated defaults may need art-direction for the camera/lighting rig.

---

## 12. Decisions captured
- **Bump/normal mapping excluded** (normal-perturbation hack; off-brand for a physical PT). Geometry
  for grooves, anisotropic microfacet BRDF for the sub-µm finish.
- **Tier 2 production geometry = fine displaced triangle mesh.** **Tier 3 SDF = exactness benchmark.**
- **Hybrid calibration** (physical defaults + knobs).
- **Doc-first, then Phase 0 measurement bake-off.**
