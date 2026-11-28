# M7m results: viscosity with sheets; the 3-D momentum audit; the m7l regressions traced (`liquid.mac` m7m.1)

2026-11-27. Follows [M7l](SIMULATION_M7L_RESULTS.md), [M7k](SIMULATION_M7K_RESULTS.md), [M7j](SIMULATION_M7J_RESULTS.md).
Contract: one Appendix C row (behavioural: viscosity with sheets, refused since m7d, is accepted; no port, member or
parameter added or changed; the checkpoint and cache layouts are unchanged).

**Status.** One model changed. Versions: `liquid.mac` `m7l.1` → `m7m.1`; `cloth.shell` unchanged by this round
(`m3a.7`; the performance agent's cloth changes are its own); `rigid.bodies` unchanged (`m6.2`). Sheets stay
`preview`. This is the last implementation round before the performance pass; its limits are stated in Known limits
(the end of this document), the contract's §6 note, the `sheet` help and the parser README.

| Task | Outcome |
|---|---|
| 1. Viscosity with sheets | **Done** (§1). Prescribed and implicitly coupled sheets are no-slip walls of the viscous solve on each wetted side; a difference whose segment crosses a sheet is one-sided (ghost fluid to the crossing) on each side, so no strain couples the liquid across a sheet; a coupled sheet's free vertex velocities are unknowns of the solve with its step matrix (with the face liquid) as their mass, the system symmetric positive definite; its viscous impulse is part of its steps' impulses, so it reaches the cloth through `surface_load_steps`. Honey layer down a held sheet against a rigid floor: flux **−1.4%** (2-D, 6 cells deep); the layer below a held sheet stays at rest (4.9e-5 of the upper layer's speed); free sheet in honey: momentum closes to **2.6e-4** of the sheet's momentum (3 axes). Refusal removed (staggered or one-way port sheets with viscosity still refused: their load is the pressure only). New scene `liquid_honey_cloth` |
| 2. 3-D momentum audit | **Done** (§2): new fast test `LiquidSheetViscosityTest` D, liquid + coupled sheet + coupled rigid body, translating and spinning, viscous and inviscid: linear momentum closes to **1.6-2.0e-3** on every axis; the body's offered torque impulse equals its change of angular momentum about its centre to 4e-15. Found: at a sheet's **free edge** the normal momentum does not close (§2.2; pressure path, m7i's partly covered faces; not viscosity) |
| 3a. Catch 64 indicators | **Traced, not fixed** (§3.1): both indicators are **one step** (t = 0.1333 s, the block's impact on the cloth's interior; the next largest 0.65%); m7l's face liquid (two-valued) and its dry-side test each account for part of the rise (with m7k's face liquid 0.93% / 1.7%, without the dry-side test 0.94% / 1.1%); no leak |
| 3b. Flat sheet, bowl convergence | **Traced, documented** (§3.2): m7k.1's 3.8% was the net of a 16.7% advection loss and a 10.3% gain from re-pinning (+2.3% reinitialisation); m7l's 6.1% is the advection stage (−6.5%) alone, and it converges: 48 cells (the same 3 cm, 4.8 cells) −2.1%. The bowl's advection loss halves too (24 → 48 cells: −4.4% → −2.4%); what turns the total the wrong way is the reinitialisation (+2.0% → −0.9%) with 1,988 cell-steps that would have turned wet across the curved sheet (44 at 24 cells), held by the guard |
| 4. Waterline limit, layer depth | **Documented** (Known limits; contract §6, `sheet` help, parser README); the progress detail's two warnings point to it |
| 5. Catch volumes to 1.6 s | **Done** (§4): 64 cells, every 0.1 s to the end: no leak; 4.76 L of 4.83 on the cloth at 1.6 s |

## Files

| File | Change |
|---|---|
| `src/Library/Simulation/Liquid/LiquidViscosity.{h,cpp}` | `ViscousSheetPoint`, `ViscousSheetFace`, `ViscousSheetCut`, `ViscousSheet`; `ViscosityInput::sheets`, `sheetFaces`, `sheetCuts`; `ViscousStep`'s `sheetImpulse`. The system: a sheet's closed faces are walls (`Solid`, the sheet's velocity at the crossing, ghost-fluid fraction along the face's axis); `DiffCut` (one-sided differences); `CutEdge`/`EdgeCopy` (an edge's strain per side); `SheetWeights` (sided weights of the cells beside closed faces and of the cut edges' copies); `SheetStrainList`/`ForSheetStrains`/`GatherSheets` (the coupled sheets' unknowns); `Sample` (the former `constTerm`/`sampleTerm` lambdas); CG over faces, bodies and sheet vertices with a block preconditioner (Jacobi; the sheet's factorised S' + strain diagonal) |
| `src/Library/Simulation/Liquid/LiquidModel.{h,cpp}` | `SheetViscousGeometry` (the cuts: closed faces of the grid shifted by h/2 along each component); `Viscous` takes the stage's previous pressure, the coupled sheets' V* and face mass; `Step`: `predictSheets` (the m7e prediction, before the viscous solve when it comes first); `sheetViscousImpulse`; the refusal narrowed to staggered/one-way port sheets; a prescribed sheet's viscous impulse joins `sheetImpulse` |
| `src/Library/Simulation/Liquid/LiquidSheets.{h,cpp}` | `FindSheetCrossings(..., cover)`: `false` skips the covers |
| `src/Library/Simulation/Liquid/LiquidSolver.cpp` | version m7m.1; the refusal narrowed; the scope gains an M7m sentence with viscosity and sheets; the progress detail's two warnings point to Known limits |
| `src/Library/Simulation/Liquid/LiquidParameterSchema.cpp` | the `sheet` help: viscosity, the stated limits |
| `src/Library/Parsers/README.md`, `docs/SIMULATION_SOLVER_CONTRACT.md` (§6, Appendix C) | the same |
| `scenes/Tests/Simulation/liquid_honey_cloth.RISEscene` (new) | honey poured onto a pinned cloth; 40 cells, 49 frames |
| `tests/data/cst_derive_golden.txt` | its line (only that line changed) |
| `tests/LiquidSheetViscosityTest.cpp` (new) | A-E (§1, §2) |
| `tests/LiquidSheetTest.cpp`, `tests/LiquidClothCouplingTest.cpp` | the refusals: a prescribed or implicit sheet with viscosity configures, a staggered one is refused; version pin |
| `tests/LiquidRigidTest.cpp` | version pin |
| `tests/test_schedule.tsv` | `LiquidSheetViscosityTest` (pool, 5 s) |

No library source file and no chunk keyword added (build lists, the Xcode project and the keyword count 191 unchanged);
the new test is discovered by the test build's glob.

## 1. Viscosity with sheets (task 1)

### 1.1 What was built

The viscous step (M7c, Batty and Bridson's strain-rate form) minimises the liquid's kinetic energy about u* plus
2 dt μ h³ times the weighted strain energy; walls enter as boundary values of its differences. A sheet is now such a
wall, on each side separately:

- **Closed faces.** A face a sheet closes (a barrier, cover ≥ 1/2) is not an unknown: its value in every difference
  is the sheet's velocity at the crossing (barycentric, full vector: no slip). Along the face's own axis (a cell's
  normal strain) the wall sits at the crossing, θ = |s − x_f|/h ∈ [θ_min, 1.5] from the other face (the crossing lies
  between the two cell centres); along the other axes at the face's centre.
- **Cut differences.** A difference of component c along a ≠ c between two c-faces whose segment crosses a sheet (found
  as the closed faces of the grid shifted by h/2 along c, whose cell centres are the c-faces) is replaced, per side,
  by the one-sided difference to the sheet's velocity at the crossing, θ = t or 1 − t (θ_min 0.05). It never appears
  two-sided, so no strain couples the two sides.
- **Edges a sheet cuts** (the shear strains, at cell edges): one strain per side of the sheet. The side holding the
  edge's point keeps its one-sided cut difference and the other, uncut difference; the other side has its one-sided
  difference only (an uncut difference on the far side is dropped: at a no-slip wall the derivative of the wall's
  velocity along it is the wall's own). Each copy is weighted by its side's share of the cut segment (t or 1 − t)
  times the liquid fraction of the mean level set of the edge's four cells on that side (by the crossed triangle's
  plane). A dry side's copy has no weight, so the dry side's faces stay out of the solve.
- **Cells beside a closed face.** Their liquid fraction is taken with their own side's level set (a neighbour across one
  of their closed faces takes the cell's value: the two-valued level set's sides kept apart) and only on their side
  of each closed face's crossing (the samples beyond it excluded, as a solid's open fraction excludes its inside).
- **The coupled sheet** (implicit, with its dynamics): its free vertex velocities V are unknowns with the energy
  (1/2)(V − V*)ᵀ S' (V − V*), S' = S + F the step matrix the projection uses (M7e: mass, Rayleigh damping, the
  corotational stiffness, the face liquid F), pins given. The system stays symmetric positive definite (S' is; the
  strain energy is a sum of weighted squares). CG with a block preconditioner: the faces' and bodies' Jacobi, the
  sheet's (S' + 2 dt μ h³ diag(G_Vᵀ W G_V))⁻¹ (a factorisation of a copy of its step matrix with the strains'
  diagonal). Its impulse S'(V − V*) moves V* by S'⁻¹ of it before the projection, as a coupled body's moves w*, so the
  step's impulse on the sheet (S V_new − its own right-hand side, m7e) holds the viscous part and the cloth receives
  it in `surface_load_steps` with the pressure's; the liquid's copy replays the cloth's step with that impulse (m7k).
- **The incremental pressure** (M7c): the viscous solve sees the previous pressure's gradient; a body sees its
  counterpart. A coupled sheet's V* gains S'⁻¹ ρ h³ J q_prev (J of the step's crossings, `SheetCoupling::Measure`),
  only the viscous impulse moving it: nothing new in the checkpoint (q and the sheet's state are there).
- **Order in the step.** With one momentum stage the viscous solve comes before the projection; the sheet's
  prediction (face liquid, rotations, factorisation, V*) is then made first, from the stage's face velocities with
  gravity (the viscous solve leaves the closed faces unchanged). With Heun (the default) the viscous solve is in the
  second stage, about V*₂ = V*₁ + S'⁻¹ I₁/2, as before.
- **Prescribed sheets** are walls with their given velocity; their viscous reaction is measured into `sheetImpulse`.
  A staggered or one-way port sheet with viscosity is refused: its load is the pressure (`SheetPressure` drops the
  tangential part), so its viscous traction could not reach its partner.
- **Free slip.** `viscous_walls free_slip` applies to the domain walls, static solids and bodies; sheets stay no-slip
  (stated in the scope).
- **Bodies near sheets.** A body term inside an edge a sheet cuts is not in the body's implicit coupling (not met in
  any case here; a body within a cell of a sheet).

Without viscous sheets every arithmetic path is m7l.1's (the body-only and sheet-free solves are bit-identical:
§5).

### 1.2 Tests (`LiquidSheetViscosityTest`, fast tier)

| Case | Result |
|---|---|
| A: honey (10 Pa s, 1400 kg/m³) 1.25 cm (6 cells) down a held sheet tilted 0.3 rad to gravity, 2-D 96 × 24, the sheet 0.3 h off the lattice, 0.08 s (3.6 viscous times) | flux at the middle **−1.41%** from the same layer on a rigid floor at the same height (gate 5%); floor −12.8%, sheet −14.1% from the Nusselt film g sin(a) d³/(3ν) (both: 6 cells and the free surface's cut cells); nothing below the sheet |
| B: honey moving at 2 cm/s along a held sheet above, at rest below (2-D), 0.05 s | below: largest speed **4.9e-5 of u0** (gate 1%); the upper layer stopped by the sheet's no-slip (flux 2.5e-4 → 5.1e-7 m²/s) |
| C: a free sheet (3 kg/m², spanning the tank, normal y, off the lattice) moving at (4, 3, −5) mm/s through a honey block clear of the walls, no gravity, 3-D 20³, 0.04 s | momentum (liquid + face liquid + sheet) closes to **2.6e-4** of the sheet's initial momentum on the worst axis, every step (inviscid: 2.1e-3); gate 1% |
| E: a staggered port sheet with viscosity | refused |

Slow motion is used in C and D so that the transport's own non-conservation (quadratic in the speed) and the
interface's changes (the reinitialisation and the volume correction are off in C and D) do not mask the exchange.

### 1.3 The scene

`scenes/Tests/Simulation/liquid_honey_cloth.RISEscene`: 1.2 L of honey (10 Pa s, 1420 kg/m³) released 4 cm above a
0.3 m cloth pinned at its edges, 40 cells, 49 frames (1.6 s). Baked at 40 cells (final binary): 48-50 s, 0.19 GB.
With cotton's own 0.15 kg/m² the impact kicked the cloth past both indicators (rotation stretch 1.9%, linearisation
correction 2.6%, warnings); the committed scene uses `material_density 0.5` (0.56% / 0.62%, no warning), as Known
limits advises. Reduced (24 cells, 13 frames): 7 s. Renders (`rendered/simulation_m7/`, outside git, path tracing
640 × 400, denoised copies beside them): `m7m_honey_cloth_t050_0000.png` (scene camera, t = 0.5 s) and
`m7m_honey_cloth_t160_0000.png` (t = 1.6 s); `m7m_honey_cloth_near_t050_0000.png` and `m7m_honey_cloth_near_t160_0000.png`
(camera 0.55 0.33 0.62, the same frames): the honey spreads into a layer over most of the cloth, which sags little at
0.5 kg/m². The honey's dielectric (the honey-water scene's amber `tau`) reads nearly clear at a layer this thin.

## 2. The 3-D momentum audit (task 2)

### 2.1 Liquid, coupled sheet and coupled rigid body (`LiquidSheetViscosityTest` D)

C's tank and sheet with a dynamic sphere (radius 7 mm, 700 kg/m³) below the sheet in the same honey block, moving at
(−3, 2, 4) mm/s, then also spinning at (0.3, −0.2, 0.4) rad/s; no gravity; the total momentum (liquid on the
projection's weights, the face liquid moving with the sheet, the sheet's vertices, the body) every step over 0.04 s:

| Variant | linear, worst axis (of the largest initial momentum) | the body's torque impulse − Δ(Iω) about its centre | the total angular momentum about the body's centre (reported) |
|---|---|---|---|
| translating, viscous | **1.6e-3** | (no spin) | 4.0% |
| translating and spinning, viscous | **2.0e-3** | **4.4e-15** of its I ω | 4.0% |
| translating and spinning, inviscid | **1.8e-3** | 2.4e-15 | 0.03% |

Gates: 1% (linear), 1e-6 (torque). The angular momentum of the MAC liquid about a point is not a conserved quantity of
the discrete scheme (the face momenta sit at face centres; the pressure's and the strains' discrete torques do not
cancel exactly), so it is reported, not gated.

### 2.2 Found: the normal momentum at a sheet's free edge

The same free sheet made smaller than the liquid (half size 1.2 cm, its four edges free inside the honey;
`LSV_HALF=0.12`): the two tangential axes close as before (1e-3 of the sheet's momentum, viscous: the viscous exchange
closes), but the normal axis loses **6.4e-6 N s in the first step** (0.62 of the sheet's initial momentum; then
another 1.2-3.5e-6 over nine steps), identically with and without viscosity. So it is the pressure path at free
edges, m7i's partly covered faces (cover < 1, the part c moving with the sheet, A(1 − c) open). Ruled out: the step
start's overwrite of a partly covered barrier face's velocity by the sheet's (skipping it for c < 1 changed nothing).
Not isolated further; with no free edge in the liquid the exchange closes (C, D). This bears on the catch's light free
edges only through the wave demo and the flaps (the catch's cloth is pinned all round).

## 3. The m7l regressions (task 3)

### 3.1 The catch's indicators at 64 cells

Each indicator is the largest of one step's values over the run. A trace of every step above 0.5% (temporary, removed)
of the 64-cell catch (49 frames) shows **one step above 1%**: t = 0.1333 s, correction 2.58%, rotation 0.168 rad
(stretch 1.4%), the fastest vertex 1.5 m/s at (0.35, 0.27, 0.325), inside the cloth's footprint under the water
block's edge: the block's arrival (released at t = 0, 6 cm above). Every other step is below 0.65%. Temporary
switches, each against the same build (64 cells, 49 frames):

| Variant | rotation stretch | correction | held crossings |
|---|---|---|---|
| m7l.1 as committed | 1.4% | 2.6% | 0 |
| the face liquid from the two cells' own values (m7k's `SheetFaceMass` θ) | 0.93% | 1.7% | 0 |
| the projection's free surface at sheet faces off (m7l's `SheetFreeSurface`) | 1.4% | 2.6% | 0 |
| the contact rule from the own values (m7k's `OpenUnwetted`) | 1.4% | 2.6% | 0 |
| the dry-side test off (M7L §1.1) | 0.94% | 1.1% | 2,649 |
| face liquid m7k's and dry-side test off (traced) | 1.2% (one step, 0.1333 s) | 1.9% at 0.1333 s, 1.1% at 0.1313 s (two steps) | |

So the rise is the impact step, made harder by two parts of m7l's two-valued representation: the face liquid that
appears whole when the front reaches a face (m7k's pinned values had held a partial θ beside the sheet before the
water came) and the dry-side test; neither is separable from m7l's fix (without the dry-side test 2,649 crossings are
held). Not contained, so not changed; at 128 cells the catch has no warning (M7L §1.6), and at 64 cells there is no
leak (§4).

### 3.2 The flat sheet and the bowl's convergence

Stage sums of the sharp volume (`M7F_STAGES=0 M7J_SHARP=1`, 2 s, no volume correction), the start binaries:

| Case | advected | re-pinned / swept | reinitialised | other | total |
|---|---|---|---|---|---|
| flat sheet, 2.4 cells (24 cells), m7k.1 | −16.7% | +10.3% | +2.3% | +0.2% | −3.8% |
| flat sheet, 2.4 cells, m7l.1 | **−6.5%** | 0 | +0.3% | +0.1% | −6.1% |
| flat sheet, the same 3 cm at 48 cells (4.8 cells), m7m.1 | −2.3% | 0 | 0.0% | +0.2% | **−2.1%** |
| bowl, 1.3-cell rim, 24 cells, m7l.1 | −4.4% | 0 | **+2.0%** | −0.05% | −2.5% |
| bowl, 48 cells, m7l.1 | **−2.4%** | 0 | **−0.9%** | +0.01% | −3.3% |
| bowl, 48 cells, m7k.1 | −7.0% | −12.0% | +4.1% | −0.04% | −14.9% |

- **Flat sheet.** m7k.1's 3.8% was not a better representation: its advection lost 16.7% and the re-pinning of the
  cells beside the sheet each step put 10.3% back. m7l.1's loss is all in the advection stage, the thin layer's own
  transport (a rigid floor's 2.4 cells lose 4.2%), and it **converges**: the same layer at 48 cells loses 2.1%.
- **Bowl.** The advection loss also converges (−4.4% → −2.4%). The total goes the wrong way because the
  reinitialisation's contribution changes sign: at 24 cells it gained 2.0% (partly cancelling the advection), at 48
  it loses 0.9%. At 48 cells 1,988 cell-steps would have turned wet across the curved sheet (10% of V0, held by the
  guard; 44 at 24 cells) and up to 0.40% of the volume lies below the sheet beyond its adjacent cells: the curved
  sheet's finer staircase crossing more lattice lines between the two-valued sides. Not contained (it is in the
  two-valued extension's geometry at a curved staircase); documented. The stated layer depth stands (Known limits).

## 4. The catch's volumes to 1.6 s (task 5)

The catch at 64 cells with `publish_fields TRUE`, 49 frames, final binary (inviscid: m7l.1's arithmetic, §5);
`LiquidClothCouplingTest --measure <pool> <cloth> 3` (M7L §1.7's measure, every 0.1 s). Litres:

| t (s) | above | below in footprint | > 1.5 h below | of it on the floor | outside | total | cloth lowest y (m) |
|---|---|---|---|---|---|---|---|
| 0 | 4.828 | 0 | 0 | 0 | 0 | 4.828 | 0.3000 |
| 0.2 | 4.865 | 0.009 | 0 | 0 | 0 | 4.874 | 0.2067 |
| 0.4 | 3.715 | 0 | 0 | 0 | 0.961 | 4.676 | 0.2597 |
| 0.6 | 4.278 | 0 | 0 | 0 | 0.400 | 4.678 | 0.2544 |
| 0.7 | 4.590 | 0.054 | 0.054 | 0.054 | 0.120 | 4.763 | 0.2499 |
| 0.8 | 4.686 | 0.018 | 0.018 | 0.018 | 0.073 | 4.776 | 0.2477 |
| 0.9 | 4.804 | 0.012 | 0.012 | 0.012 | 0.025 | 4.842 | 0.2463 |
| 1.0 | 4.832 | 0.003 | 0.003 | 0.003 | 0.017 | 4.852 | 0.2468 |
| 1.1 | 4.804 | 0 | 0 | 0 | 0.002 | 4.806 | 0.2485 |
| 1.2 | 4.770 | 0.001 | 0 | 0 | 0.021 | 4.792 | 0.2491 |
| 1.3 | 4.718 | 0 | 0 | 0 | 0.050 | 4.767 | 0.2490 |
| 1.4 | 4.747 | 0 | 0 | 0 | 0.030 | 4.777 | 0.2489 |
| 1.5 | 4.750 | 0 | 0 | 0 | 0.023 | 4.773 | 0.2488 |
| 1.6 | 4.761 | 0 | 0 | 0 | 0.003 | 4.764 | 0.2486 |

(0.2-0.8 s equal M7L §1.7's m7l.1 rows to the digit.) After 0.8 s the cloth settles (its lowest point within 2 mm), the
water outside the footprint falls back to 3 mL, nothing is below the cloth after 1.1 s, and 4.76 L of the 4.83 L start
is on the cloth at 1.6 s (the sharp count's own drift over the run −1.3%, no volume correction in the measure's
reading). No leak through the cloth's interior to the end.

## 5. Bytes: which reference scenes changed

The start binary (built from `052fc4338` plus only the build-flags agent's project-file edit; copied before any
change) and the final binary baked reduced copies (M7i's scripts), each in its own cache directory; content-addressed
frame names compared:

| Scene (reduced) | Result |
|---|---|
| `liquid_dam_break` (24 cells, 13 frames) | identical (13) |
| `liquid_rigid_bodies` (32 cells, 13 frames) | identical (39) |
| `liquid_honey_water` (24 cells, 7 frames; viscosity, no sheets) | identical (7) |
| `liquid_cloth_flap`, staggered and one-way | identical (27, 27) |
| `cloth_flag_scene` (31 frames), `rigid_cloth_trampoline` (31 frames) | identical (32, 63) |
| `liquid_cloth_wave`, `liquid_cloth_catch` (24 cells, 13 frames; implicit sheets) | identical (27, 27) |
| `liquid_cloth_catch` (64 cells, 49 frames) | not compared byte for byte; its indicators (1.4% / 2.6%, 0 crossings, 0 holes) and §4's volumes at 0.2-0.8 s equal M7L's m7l.1 readings to the printed digit |

Judged on the liquid-only scenes (dam break, honey water, rigid bodies): identical. The coupled scenes were identical
too at the time of the final build (the performance agent's cloth ordering change had not changed cloth bytes in that
tree); a later cloth change may change them, which would be that agent's. `liquid_honey_cloth` is new.

## 6. Tests, pins

- **Pins.** `liquid.mac` m7l.1 → m7m.1 (`LiquidRigidTest`, `LiquidSheetTest`); `cloth.shell` not touched.
- **New** `LiquidSheetViscosityTest` (A-E, about 5 s, one core).
- **Refusals** (`LiquidSheetTest` D, `LiquidClothCouplingTest` I): a prescribed and an implicit sheet with viscosity
  configure; a staggered port sheet with viscosity is refused.
- **Fast tier.** The non-fire fast tier (`--filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, with the performance and
  build-flags agents' uncommitted changes in the tree): 453 built, **453 passed** (452 + `LiquidSheetViscosityTest`;
  309 s wall). Fire tests were not run.
- **Suites** (final build): `LiquidClothCouplingTest`, `LiquidSheetTest`, `LiquidPhysicsValidationTest`,
  `LiquidRigidTest`, `LiquidSheetViscosityTest` pass.
- **Build.** `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free (Homebrew linker
  notices only).

## 7. Verified, inferred, not done

- **Verified (measured).** §1.2's and §2's tests; §1.3's bakes; §3.1's switches and trace (temporary code, removed);
  §3.2's stage sums (start binaries: m7l.1 and M7L's m7k.1 test binary); §4's table; §5's comparison.
- **Inferred.** §3.1's reading that the face liquid appears whole at the front (from the switch, not traced per
  face); §3.2's attribution of the bowl's reinitialisation loss and held crossings to the curved staircase (from the
  counts, mechanism not isolated); §2.2's attribution to the partly covered faces (from its appearing only with free
  edges in the liquid and on the normal axis only).
- **Not done (out of scope by decision).** The waterline kick itself, the rotation iteration, second-order cut cells;
  the free-edge momentum of §2.2; the VS2022, Android and Xcode builds (no source file added); the GUI; fire tests.

## Known limits (preview; stated for scene authors)

- **Layer depth.** Liquid resting on an implicitly coupled sheet needs a layer about **2.5 cells** deep, as on a rigid
  floor. Measured without the volume correction over 2 s: a held flat sheet's 2.4-cell sloshing layer loses 6.1%
  (the same layer at twice the resolution 2.1%), a sagging bag's 2.4 cells 4-5%, a held bowl's 1.3-cell rim 2.5%
  (3.3% at 48 cells, §3.2), a rigid floor's 2.4 cells 4.2%. Thinner layers lose more; the volume correction
  (on by default) puts the volume back but not the layer's shape.
- **Light cloth at a waterline.** Very light cloth (below about **0.1 kg/m²**) with a **free edge at a waterline** on a
  coarse grid is kicked at the edge: at 24 cells, 0.05 kg/m², in a dam break, the edge's peak speeds were 2.8-6.1 m/s
  (median 3.8) at a 1/30 s macro step and 3.4-8.7 m/s (median 4.3) at 1/120 s over ten perturbed runs
  ([M7k results](SIMULATION_M7K_RESULTS.md) §1.4); the 64-cell wave demo at 0.2 kg/m² read a linearisation correction
  up to 39% in one rare step (M7k §1.5). The progress detail warns when a step's rotation stretch or linearisation
  correction exceeds 1% ("warning: above 1%, …"), and the warning names this section. What to do: use **heavier
  cloth** (the honey scene's 0.5 kg/m² has no warning where cotton's 0.15 had 2.6%), **pin the edge** that meets the
  water, or **refine the grid** (the catch at 128 cells has no warning where 64 cells had one, M7L §1.6). Not being
  pursued (owner decision).
- **Viscosity with sheets.** No-slip on each wetted side, located at the crossing along each difference (ghost fluid,
  first order) and to the cell-centre staircase for the pressure; free slip is not available at sheets; a staggered
  or one-way sheet with viscosity is refused. A layer down a held sheet flows within 1.4% of the same layer on a rigid
  floor (6 cells deep, §1.2).
- **Momentum at free edges.** With a sheet's free edge inside the liquid the normal momentum of the exchange does not
  close (§2.2); with pinned or out-of-liquid edges it closes to 2e-3.
