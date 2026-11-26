# Guilloché Fidelity — 3-Way Results: principled vs approximation, quality vs cost

The deliverable of the 3-way study (`docs/GUILLOCHE_ROSE_ENGINE_TIER2_PLAN.md`): render the
**same** engine-turned dial three ways — a cheap normal-perturbation **bump**, a memory-bounded
**displaced mesh**, and the exact analytic **SDF** — and measure, with numbers, which realization
earns its cost. All three consume **one** kinematic field (`tests/GuillocheDialExpr.h`
`BuildKinematic`, oracle-tested to 1e-9) so the only variable is *how the groove geometry is
represented*.

## The three realizations

| | what it is | groove relief | normals | memory |
|---|---|---|---|---|
| **bump** | flat disk + `bumpmap_modifier(∇kinfield)` | **none** (flat surface) | perturbed from the height gradient | **O(1)** |
| **mesh** | `displaced_geometry(kinfield)` | real, **tessellated** | flat per-facet (`face_normals`) | **O(triangles)** |
| **SDF** | `sdf_geometry { heightfield_function kinfield }` | real, **exact analytic** | exact field gradient (smooth) | **O(1)** |

The SDF is the principled ground truth: it sphere-traces `z = scale·f(u,v)` with no tessellation, so
its surface is exact at any pitch and its memory is constant. The bump and mesh are the two
approximations — bump trades away relief entirely; mesh trades away exactness for a finite triangle
budget and pays for it in memory.

## What's exact vs approximate (the core question)

- **SDF** — exact surface, exact smooth normals. The real |cos|-profile V-walls are smooth curves, so
  the analytic gradient is the *physically correct* normal. **Validated:** under Lambertian shading
  (pure normal response) the SDF matches a converged displaced mesh to **97.5 % mean / 94 % std** — the
  analytic normals reproduce the true surface.
- **mesh** — the surface converges to exact as `mesh_n → ∞` (P0-B: the groove NDF converges at
  ~12–24 verts/pitch), but `face_normals` makes every facet a flat micro-mirror. Under **sharp
  specular** those facets throw glints the true smooth surface does **not** have — the mesh
  *over-sparkles*. This is faceting **artifact**, not surface response.
- **bump** — head-on it can fake the *appearance* (the normal field drives the shading), but there is
  **no geometry**: no self-shadowing between grooves, no parallax, no silhouette relief. It fails where
  those matter — at **grazing angles**.

## Cost profile (full ⌀32.6 mm dial, R=20.6, 0.30 mm pitch, 800², PT)

| realization | peak RSS | render | notes |
|---|---|---|---|
| **SDF** (exact) | **0.23 GB** | 89 s | memory-light, trace-heavy; *constant* in pitch |
| **mesh** (`mesh_n 1200`, ~2.3 M tris) | **1.25 GB** | 18 s | memory-heavy, trace-light |
| **mesh** at authentic fine pitch (P0-B) | **19–43 GB** | — | the memory wall — infeasible on a normal machine |
| **bump** | _see table below_ | _fast_ | O(1) memory, no relief |

The decisive asymmetry: the mesh is fast to trace but its memory grows with the triangle budget, and at
*authentic* guilloché pitch (0.10–0.20 mm) that budget blows past 19 GB (P0-B). The SDF holds **0.23 GB
regardless of pitch** — it is the only memory-feasible exact representation of the full dial at the
finest pitches. That is the whole reason the SDF was elevated from benchmark to a real production path.

## Measured deviation from the exact SDF ground truth

Same dial, same kinematic field, same isotropic conductor + rake light, 800², 96 spp PT.
Quality = deviation from the exact SDF render (RMSE on luminance; blurRel% = location-insensitive
envelope error). Cost = wall time + peak RSS.

| realization | view | peak RSS | render | RMSE vs SDF | envelope err |
|---|---|---:|---:|---:|---:|
| **SDF** (exact) | head-on | 231 MB | 116 s | 0 (ref) | 0 % |
| **mesh** (2.3 M tris) | head-on | **3002 MB** | 20 s | 0.297 | 68 % |
| **bump** (engaged) | head-on | 384 MB | 13 s | 0.458 | —† |
| **SDF** (exact) | grazing | 231 MB | 61 s | 0 (ref) | 0 % |
| **mesh** (2.3 M tris) | grazing | 3003 MB | 14 s | 0.328 | 22 % |
| **bump** (engaged) | grazing | 385 MB | 9 s | 0.862 | —† |

† Bump rows **re-measured 2026-06-14** after fixing the modifier (see "Correction" below); RMSE-vs-SDF
recomputed, envelope% (original blurRel harness) not re-run for these rows.  The earlier numbers
(head-on 0.193 / grazing 1.031) were the **flat-disk limit** — the modifier was mis-configured, not
broken (details below), so the dial rendered with no perturbation at all.

Reading the rows:
- **Mesh deviates from the exact surface by ~0.30 RMSE at *both* views** — head-on that is pure
  *faceting over-sparkle* (mesh std 0.356 vs SDF 0.192: the flat facets throw glints the smooth
  surface lacks); at grazing the macro-relief flash dominates and the mesh tracks the SDF more closely
  in *envelope* (22 %) even as per-pixel RMSE stays ~0.33. The mesh is an honest-but-imperfect
  approximation whose error is faceting, shrinking as `mesh_n → ∞` (at the cost of the memory wall).
- **Bump (now engaged) reproduces the head-on rosette but cannot fake relief at grazing.** With the
  modifier working (slope-matched, `normalize_gradient TRUE`), head-on the bump renders the *same*
  12-lobe guilloché structure as the exact SDF (smooth gradient normals, visually close — far cleaner
  than the mesh's faceting). Its head-on RMSE-vs-SDF (0.46) is *higher* than the old flat-disk number
  (0.19) **not** because it's worse but because RMSE rewards the conditional mean: a featureless grey
  disk ≈ the SDF's average, so it scores low RMSE with *zero* structure, while the working bump carries
  the real pattern (disk std 0.50 vs SDF 0.19 — comparable order, slightly over-contrasty because a
  flat disk presents every tilt at normal incidence). Read RMSE *with* the structure metric, never
  alone. At **grazing** the bump improves on the flat limit (1.03 → 0.86) — the tilts do catch the rake
  light — but it still has **no geometry**: no self-shadowing between grooves, no parallax, no
  silhouette relief. That residual 0.86 is the irreducible cost of a normal-only fake, and it is
  exactly where the real engraving comes alive. The cost columns (O(1) memory, fastest render) hold.

## Correction (2026-06-14): the bump modifier was mis-configured, not broken

The original "bump did not engage at any scale — a modifier↔geometry plumbing issue" was **wrong**.
Instrumenting `BumpMap::Modify` and re-rendering proved the modifier fires correctly on the flat
`cartesian_disk`: `Modify` is called per hit, the disk supplies texcoords + a valid ONB
(`CreateFromW((0,0,1))` → tangents `(-1,0,0),(0,-1,0)`, *not* degenerate), and `GGXSPF` reads the
perturbed `ri.onb`. The "flat" render was a **unit mismatch**: the harness set the bump `scale` equal
to the mesh's `disp_scale` (both `0.101`), but they are different units.

- Mesh/SDF relief slope ≈ `surface_scale · (∂f/∂u) / (2R)`
- Legacy bump normal tilt ≈ `scale · (f(u+w) − f(u−w))` ≈ `scale · 2·windowsize · (∂f/∂u)`

So the legacy bump's amplitude **couples to `windowsize`**: at `windowsize 0.0015` a `scale` of `0.101`
is ~8× weaker than the equivalent relief, and the dial read flat. The mesh-matched value is
`scale = surface_scale / (2R) = 0.101/41.2 ≈ 0.00245`.

**Fix (shipped):** a new opt-in `normalize_gradient` flag on `bumpmap_modifier` (default `FALSE`,
byte-identical to every legacy scene). When `TRUE`, `BumpMap::Modify` divides the central difference by
`2·windowsize`, so `scale` becomes the **window-independent gradient amplitude** (verified on a smooth
field: a fixed `scale` over a 4× window change leaves the render essentially unchanged with the flag on
— disk-std ratio 0.98 — whereas with it off the same `scale` produces a materially different render per
window). The harness bump scenes now use `normalize_gradient true` at the matched `scale 0.002451`; the
rows above are that configuration. ABI-safe (Layer-1 `RISE_API_CreateBumpMapModifierEx` + legacy
wrapper; the `IJob` virtual and the Blender bridge are untouched; the flag routes via the `IJobPriv`
channel). All 139 unit tests pass; the legacy Veach-egg Perlin bump scene is byte-for-byte unchanged.

## Verdict

**The principled approach (exact SDF) wins on the axis that actually constrains this problem — memory —
and is the *only* option at authentic fidelity.** The trade is concrete and measured:

1. **Memory is the deciding axis, and only the SDF survives it.** The mesh is the fast-to-trace,
   accurate-with-enough-triangles approximation — but "enough triangles" at authentic 0.10–0.20 mm
   pitch is **19–43 GB** (P0-B), past a normal machine. The SDF holds **0.23 GB at any pitch**. For the
   full dial at real guilloché pitch, the exact SDF is not just *better* — it is the only thing that
   *runs*. That is why it was elevated from benchmark to production path.

2. **The cost the principled path pays is render time, not memory.** SDF is ~5–6× slower to trace than
   the mesh (116 s vs 20 s head-on) — the classic memory-light / trace-heavy profile. For a hero still
   that is a fine trade; for a turntable or interactive preview the mesh (at a memory-feasible coarse
   pitch) is the pragmatic choice.

3. **"Exact" genuinely matters for specular.** Under diffuse, all real-geometry realizations agree to
   ~95 %. Under the sharp directional flash that *is* the guilloché look, the faceted mesh deviates
   ~0.30 RMSE from the exact smooth surface — and that deviation is an *artifact* (facet glints), not
   signal. The exact SDF is the correct reference; the mesh's extra sparkle is tessellation noise that
   a viewer would (subtly) read as "CGI."

4. **The cheapest approximation (bump) is a real CONTENDER, bounded by geometry not cost (revised
   2026-06-14).** It is the lightest (384 MB — O(1) in pitch, ~8× under the mesh) and fastest of the
   three, and head-on it genuinely reproduces the rosette (engaged + slope-matched), visually close to
   the exact SDF. Its only real limit is *relief*: with no geometry it can only partly track the rake
   light at grazing (RMSE 0.86) and never the occlusion / parallax / silhouette of real grooves. So it
   is a **suitable production choice for the scenes that fit its envelope** — head-on or shallow-tilt
   views, memory- or throughput-constrained renders, real-time-ish previews, and many-dial scenes where
   per-instance memory dominates — and only yields to the SDF when the shot tilts into grazing relief.

**Recommendation for the GuillocheWatch dial:**
- **Hero stills / any tilted or grazing view → SDF.** Exact, memory-light, the only path to authentic
  fine pitch. Eat the trace cost.
- **Turntables / previews / coarse-pitch dials → displaced mesh** at a memory-safe `mesh_n`, accepting
  faceting and a ≤0.30 mm pitch.
- **Bump → a suitable, memory-light option for head-on / shallow-tilt / preview / many-dial scenes**
  (now that the modifier engages + slope-matches): lightest and fastest of the three, the rosette reads
  correctly face-on. Reach past it to the SDF only when the shot tilts into grazing relief. Note: set
  the flat base disk's `mesh_n` for a smooth rim — that is silhouette-only tessellation (a flat disk),
  negligible memory, and unrelated to the bump detail (which is shading-time normal perturbation).

The headline: **for an engine-turned dial rendered honestly at authentic pitch, the principled exact
representation isn't a luxury — the approximation that would replace it (a fine mesh) doesn't fit in
memory, and the one that does (bump) doesn't hold up when the dial tilts into the light.  But for
head-on or shallow-tilt scenes within a memory/throughput budget, bump is now a measured, legitimate
choice — no longer ruled out, just bounded to where relief doesn't read.**

---

### Reproduction
`var_test/gen_dial3way.py` (drift-proof generator: one shared `kinfield` + material + rig, geometry is
the only variable) and `var_test/compare3way.py` (the 6-render cost+quality harness). Kinematic field
oracle-tested in `tests/ExpressionFunction2DTest.cpp` Test 3. SDF heightfield: `src/Library/Geometry/
SDFGeometry.cpp` (commit `868d9087`).
