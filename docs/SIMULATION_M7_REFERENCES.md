# M7 liquid solver: external references, checked

2026-11-26. Every external reference value, formula or paper description that `liquid.mac` uses as a pass criterion or
quotes as fact, with the source it was checked against and the verdict. Sources were read on 2026-11-26 from copies in
a scratch directory (not kept in the repository). "Primary" means the paper or book itself; "secondary" means a later
paper or notes that print the same formula. Where only a secondary source could be reached it says so. No digit was
estimated from a figure.

Verdicts: **match** (our value agrees with the source), **match (secondary)**, **wording corrected**, **not verified**.

## Summary

| # | Item | Our value | Source | Verdict |
|---|---|---|---|---|
| 1a | Martin & Moyce 1952, table 1, n² = 1, a = 2¼ in: front Z against T (19 rows) | `MartinMoyceSquare` | Primary: Phil. Trans. R. Soc. A 244, 312-324, scan at math.ntu.edu.tw (`~shyue/orch08/benchmarks/lowmach/broken-dam/91519.pdf`) | **match**, all 19 means (1.11→0.43 ... 4.89→3.29). The table's other series (a = 4½ in) and rows to Z = 14 are not carried |
| 1b | M&M scaling and normalisation: Z = z/a, T = n t √(g/a), time normalised so T = 0.80 at Z = 1.44 (no true zero) | same | same, §4 and the table footnote | **match** |
| 1c | M&M front speed 1.39 √(ga) | slope Z 1.44→4.89 of the table | computed from 1a: (4.89 − 1.44)/(3.29 − 0.80) = 1.386 | **match** (a number derived from the table, not one the authors state) |
| 1d | Lobovský et al. 2014: J. Fluids Struct. 48, 407-434; 100 runs per depth; average front speed v/√(gH) for t√(g/H) > 1: 1.56 (H = 300 mm), 1.34 (H = 600 mm); reduction of M&M 57 and 114 mm: 1.48, 1.69 | docs, now in code | Primary (preprint, arXiv 1308.0115, 2 Aug 2013), their Table 1 | **match** |
| 1e | Lobovský front positions as numbers | TODO | Fig. 12 only (a plot); supplementary MATLAB figures at canal.etsin.upm.es/papers/lobovskyetaljfs2013/ answer 404 | **not obtainable**; TODO stays, nothing digitised |
| 1f | Lobovský wall impact pressure, 100 runs, sensors 1-4 (3, 15, 30, 80 mm above the bed): median / 2.5 % / 97.5 % in mbar | new constants `LobovskyImpact300/600` | Primary (preprint), Tables 2 (H = 300 mm) and 4 (H = 600 mm) | transcribed; compared in case 8 (reported, not gated) |
| 1g | Lobovský setup: tank 1610 × 600 × 150 mm, reservoir 600 mm long, impact wall 1010 mm from the gate; H = 600 mm is a square column; their time t* = t√(g/H), x/H from the gate (x/H = Z − 1) | `RunDamBreakImpact` | Primary, §2 and Fig. 3, §4.3 | **match** |
| 2a | Standing gravity wave ω² = gk tanh(kh) | `RunStandingWave` | Linear potential theory; secondary: Wikipedia "Dispersion (water waves)" | **match (secondary)** |
| 2b | Viscous decay of a free-surface wave: amplitude ∝ exp(−2νk²t), energy 4νk² | M7c 10b: amplitude fit against Lamb's root; design doc | Lamb, Hydrodynamics 6th ed. §348 (dissipation) and §349 (direct); quoted by Dias, Dyachenko and Zakharov, arXiv:0704.3352, eq. (1.1) | **match (secondary)**. The test uses the amplitude decay, as it should. Design doc row 3 reworded (it said only "2νk²") |
| 2c | Lamb's exact root (s + 2νk²)² + gk + (σ/ρ)k³ = 4ν²k³ √(k² + s/ν) | `ViscousWaveRoot` | Lamb §349 (not fetched); its small-viscosity limit −2νk² agrees with 2b | **not verified** against the printed equation; the limit and the M7c derivation agree |
| 3a | Sloshing frequencies ω² = gk tanh(kd), k = nπ/L | cases 4, 3 | Faltinsen and Timokha 2009 via secondary papers; same relation as 2a with k = nπ/L | **match (secondary)** |
| 3b | Rayleigh–Taylor linear growth rate | none: no Rayleigh–Taylor case was built (design row 5 was replaced by the column collapse) | Wikipedia "Rayleigh-Taylor instability": σ² = Agk − γk³/(ρ₁ + ρ₂), k_c² = (ρ₂ − ρ₁)g/γ; viscous formula not given there | nothing to correct; design table now says so |
| 4a | Added mass: sphere ρV/2; 2-D circular cylinder ρπa² per length; 2-D flat plate of width L normal to itself ρπL²/4 per length (= the cylinder of radius L/2) | M7b 9a, M7d §6, `RunAddedMassImpulse` | Wikipedia "Added mass" (sphere); MIT OCW 2.016 reading 6 (cylinder, plate half-width a: ρπa²), citing Newman, Marine Hydrodynamics p. 145 | **match (secondary)**; the M7d estimate ρπL/4 per area is this divided by L |
| 4b | Added mass of a body in a concentric container: cylinder ρπa²(b² + a²)/(b² − a²), sphere (ρV/2)(b³ + 2a³)/(b³ − a³) | `RunAddedMassImpulse` | Derived in M7b §4.1 from potential flow; standard (Lamb, Milne-Thomson) forms; not fetched | **not verified** against a source; independent derivation in the docs, and the solver converges to it at order 2 |
| 4c | Bobbing frequency | independent FD potential-flow eigen-solve | none (a numerical reference by design) | n/a |
| 4d | Added-mass instability of staggered coupling, density-ratio limit "near 0.5 for a sphere" | design §2 | Causin, Gerbeau and Nobile, CMAME 194 (2005) 4506-4527, Prop. 3 (ICES report 04-02 copy); Förster, Wall and Ramm, CMAME 196 (2007) 1278-1293 (abstract only; full text not reached) | **wording corrected**. Causin et al.: the explicit scheme is unconditionally unstable, at any step, if ρ_s h_s < ρ_f μ_max, μ_max the largest eigenvalue of the added-mass operator (geometry dependent). Förster et al.: every sequential staggered scheme has a limiting density ratio. Neither gives 0.5 for a sphere; 0.5 is the same criterion read for a rigid body (m < m_added = ρV/2), measured in M7b §5 |
| 4e | Error multiplier −m_a/m_s per exchange (M7d §6) | M7d | Follows from the characteristic polynomial in Causin et al. (their eq. 27 has χ(−1) ∝ ρ_f μ_i − ρ_s h_s) | consistent |
| 5a | Stokes terminal velocity 2(ρ_s/ρ − 1)ga²/(9ν) | `RunSettlingSphere` | Standard | **match** |
| 5b | Haberman–Sayre 1958 wall factor (velocity ratio) (1 − 2.105λ + 2.0865λ³ − 1.7068λ⁵ + 0.72603λ⁶)/(1 − 0.75857λ⁵) | `HabermanSayre` | Secondary (the primary DTMB report 1143 was not reachable): arXiv:2607.02625 and Kagaku Kogaku 12(3) 239 print the inverse, the drag factor (1 − 0.75857λ⁵)/(1 − 2.1050λ + 2.0865λ³ − 1.7068λ⁵ + 0.72603λ⁶) | **match (secondary)**, digit for digit. Range of validity not checked; the test uses λ = 0.3 |
| 5c | Bohlin 1960 series 1 − 2.10443λ + 2.08877λ³ − 0.94813λ⁵ − 1.372λ⁶ + 3.87λ⁸ − 4.19λ¹⁰ | `BohlinFactor` | Secondary: Kagaku Kogaku (2.10443); arXiv:1103.4939 eq. 14 (2.10444) | **match (secondary)**, with a caveat: both print the λ⁵ coefficient as **6.94813**. With 6.94813 the series at λ = 0.3 is 0.4412, 4.5 % off Haberman–Sayre; with 0.94813 it is 0.42199 against 0.42200, and the Faxén series printed in the Kagaku Kogaku table has −0.95λ⁵. The printed 6 is a typographical error that two sources share; 0.94813 stands |
| 5d | Oseen correction 3Re/16 to Stokes drag | M7c 10f error budget | Standard | not re-checked (only an error estimate) |
| 5e | Cylinder in a concentric cylinder, Stokes: F = 4πμU/(ln(b/a) − (b² − a²)/(b² + a²)) | 10d | Happel and Brenner (not fetched); M7c checked it against the biharmonic solution and the dissipation integral to 1e-8 | **not verified** against the book; independently derived |
| 6a | Lamb drop, 3-D: ω² = n(n − 1)(n + 2)σ/(ρR³) (n = 2: 8σ/ρR³); decay (n − 1)(2n + 1)ν/R² | `RunOscillatingDrop` (3-D) | Rayleigh 1879, Lamb 1932 §§275, 355, via secondary sources (arXiv:1912.04688 and others) | **match (secondary)**; decay rate not re-fetched |
| 6b | 2-D (liquid cylinder) drop: ω² = n(n² − 1)σ/(ρR³) (n = 2: 6σ/ρR³); decay 2n(n − 1)ν/R² (irrotational) | 2-D case, the one run in the fast tier | n = 2: 6σ/(ρR³) in arXiv:2005.02865 (OpenFOAM) and arXiv:1701.02672 (SPH); the general n and the damping rate are the M7c derivation, which I re-derived (the energy balance KE = ½ρπnA²R^{2n}, dissipation μ∮∂|u|²/∂r gives β = 2n(n − 1)ν/R²) | **match (secondary) for n = 2**; general-n frequency and damping derived, not found in a source. The code uses the 2-D form for the 2-D run and the 3-D form for the 3-D run |
| 6c | Laplace jump Δp = σ/R (2-D), 2σ/R (3-D) | 11a | Standard | **match** |
| 6d | Capillary-gravity dispersion ω² = (gk + σk³/ρ) tanh(kd) | 11c | Wikipedia "Dispersion (water waves)" | **match (secondary)** |
| 6e | Capillary time-step limit | dt ≤ c √(ρh³/(2πσ)), c = 0.5 | Brackbill, Kothe and Zemach 1992: Δt < √((ρ_a + ρ_b)Δx³/(4πσ)); Denner and van Wachem 2015 (JCP 285): the same with 2π. Both as quoted in arXiv:2203.01963 (secondary; BKZ itself not fetched) | **wording corrected**. Our constant is Denner and van Wachem's 2π with ρ_b = 0, not Brackbill's 4π; at c = 0.5 the step is 0.35 of Brackbill's limit (0.5 of Denner's). Conservative, not an error. The comment in `LiquidModel.cpp` line 337 still says Brackbill, Kothe and Zemach (another agent's file; not edited) |
| 6f | Hysing et al. 2009 (Int. J. Numer. Meth. Fluids 60, 1259-1288) case 1: density ratio 10, viscosity ratio 10 (ρ 1000/100, μ 10/1, σ 24.5) | M7c §4.1 11d | featflow.de benchmark page, parameters table | **match** |
| 8a | Zalesak disk: radius 15, slot width 5, slot length 25 (up to y = 85), centre (50, 75), rotation about (50, 50), one revolution; unit-square scaling | `ZalesakPhi`, `InZalesak`, `RunZalesak` | Enright, Fedkiw, Ferziger and Mitchell, JCP 183 (2002) 83-116, §3.1 (UBC copy) | **match**: radius 0.15, slot 0.05 wide up to 0.85 (slot length 25 from the disk's bottom at 60), rotation 2π(0.5 − y, x − 0.5) |
| 8b | Single vortex: Ψ = sin²(πx) sin²(πy)/π, unit box, circle radius 0.15 at (0.5, 0.75), reversal by cos(πt/T) (LeVeque 1996) | `RunVortex` | same paper, §3 | **match**. Their error-analysis period is **T = 8**; the gated case here uses T = 2 (our choice, cheaper), T = 8 is reported. Our velocity has the opposite sign to the usual u = sin²(πx) sin(2πy) cos(πt/T), which is a mirror image about x = 0.5, so the errors are unchanged |
| 8c | Error norm: L1 = (1/L)∫|H(φ_expected) − H(φ_computed)| dx dy, L the length of the expected interface; plus % area loss | `r.l1 /= perimeter` | same paper, eq. 14 | **match** (symmetric difference over perimeter; the perimeter of the slotted disk is computed in `RunZalesak`). For orientation: their level-set (not ours) Zalesak L1 at 100 and 200 cells is 0.61 and 0.08 in a 100-unit box, i.e. 6.1e-3 and 8e-4 of the box; ours is 1.6e-3 and 4.4e-4. Different schemes; no comparison is gated |

## 1. Dam break, in detail

Martin and Moyce: the paper says the times "have been normalized so as to give, for any given record in a series, the
same time reading at a finite extent of spread", and the table footnote marks Z = 1.44 as the normalisation point
(T = 0.80). Our report aligns there. The table has two series, a = 2¼ in (57 mm) and a = 4½ in (114 mm); the code
carries the first (the one the docs name). Lobovský's Table 1 reductions 1.48 and 1.69 are the two series' average
speeds as they read them from the plot; the 1.39 quoted in M7a is our slope over Z 1.44 to 4.89 of the 2¼ in series
and is not the same measurement.

Lobovský's data against our geometry: their H = 600 mm run is the square column (reservoir 600 mm, so L = H), ours;
H = 300 mm has a reservoir 2H wide and is not run. Front speed: ours 1.56 √(ga) at 32 cells (M7a §4) against 1.34
(Lobovský, H = 600 mm) and 1.39 to 1.48 (Martin and Moyce, two reductions): the inviscid free-slip front is 12-16 %
fast, the expected direction (no bed friction; Ritter's 2 is the limit).

Impact pressure, new in case 8 (reported, not gated; 2-D inviscid solver against a 150 mm wide tank with an air
cushion, a gate, and a transducer of 4.2 mm). The square column in their tank, P* = p/(ρga) (a = H = 0.6 m, so
ρgH = 5886 Pa), peak over the run in the wall cell at each sensor height, 32 cells per a (cell 19 mm; sensors 1 and 2
are in the first cell, so they read the same value, as the output shows):

| Sensor (height above the bed) | Experiment median (2.5 %-97.5 %) | Ours, 32 cells |
|---|---|---|
| 1 (3 mm) | 3.15 (2.74-5.84) | 2.90 |
| 2 (15 mm) | 2.86 (2.54-3.49) | 2.90 |
| 3 (30 mm) | 2.36 (1.96-2.61) | 2.33 |
| 4 (80 mm) | 1.53 (1.37-1.63) | 1.35 |

Sensors 2 and 3 are inside the experimental spread, sensor 1 is inside it (its spread is wide), sensor 4 is 12 % below
the median and below the 2.5 % percentile (1.37) by 1.5 %. The table is from one coarse run; refinement is in the
extended tier. These are reported values; no test fails on them.

## 7. The variational formulations against the papers

The papers were obtained as PDFs: Batty, Bertails and Bridson 2007 (cs.uwaterloo.ca/~c2batty/papers/Batty07.pdf),
Batty and Bridson 2008 (.../BattyBridson08.pdf), Robinson-Mosher, Shinar, Gretarsson, Su and Fedkiw 2008
(cs.ucr.edu/~shinar/papers/2008_solid_fluid.pdf), and Guendelman, Selle, Losasso and Fedkiw 2005 (an archived copy of
physbam.stanford.edu/~fedkiw/papers/stanford2005-02.pdf). All four are author copies. Classes: **E** equivalent,
**D** deliberate deviation (stated in the solver docs), **P** possible error or open point.

### Batty, Bertails and Bridson 2007: pressure with solids (M7a §13.2, M7b §2)

| Paper | Ours | Class |
|---|---|---|
| Pressure minimises the kinetic energy ½ρ‖u‖² + ½VᵀM_S V; u = ũ − (Δt/ρ)Gp, V = Vⁿ + ΔtM_S⁻¹Jp | Same objective, scaled pressure q = p dt/(ρh) | E |
| Fluid weights: ρ × fluid volume in the Δx³ cube around each MAC face; "may be approximated by Δx × the face area" (a finite-volume variant), or by Δx² × the extent along the segment | Beside moving bodies: control-volume fractions (4³ samples), as the paper; beside static solids and the domain walls: open face areas (Ng, Min and Gibou 2009) | E for bodies; D for static solids. The paper lists the area form as an allowed approximation; the UBC page of the paper notes the Ng et al. critique that area weights are more accurate; M7b §2.2 measured that area weights give a staircase buoyancy for a moving flat-bottomed body, which is why bodies use volumes |
| J: net force Jp = −Σ vol_{i+1/2} (p_{i+1} − p_i)/Δx with the *solid's* volume in the face's cell; torque by the same weights with (x − X_com) × | J_c = Σ s_cf (1 − A_f) g_f, g_f = (e_a, (x_f − X) × e_a): the closed fraction of the face (control volume beside bodies) | E |
| Matrix gains ΔtJᵀM_S⁻¹J (rank 6 per body); right-hand side gains −JᵀVⁿ | (A + ρh³JᵀM⁻¹J)q = b with b containing Jᵀw* | E (scaling differs: A is the unscaled graph Laplacian) |
| For a perfectly neutral body at rest, M_S should be ρ × the discrete volumes rather than the exact mass ("only an issue for hydrostatic rest with neutrally buoyant bodies") | The exact mass; a neutral body is exactly at rest only if its mass equals ρ times the volume of its closed y-faces (M7b §2.1, test 9e uses that mass) | D (the paper says it may be ignored in dynamic scenes) |
| Time integration: one projection per step; collisions before the pressure solve | Heun's two stages with the step's impulse I₁/2 + I₂ (M7b §2.4) | D |
| §4: free surface / solid separation as an inequality constraint (fluid may leave a wall) | Not built | D (not in scope; fluid can stay stuck on walls and ceilings, the artefact the paper fixes) |
| Free surface: standard ghost fluid for pressure at the surface | Same, θ-weighted energy (equivalent to the 1/θ Laplacian coefficient of eq. 12 of Batty and Bridson 2008) | E |
| Scripted solids = infinite mass | Locked degrees of freedom have infinite mass; kinematic bodies are boundary conditions | E |
| Fluid weights near a free surface and a solid ("triple point") include air volume, exclude solid volume (BB08 §5.2) | Faces closed by solids carry A_f; the ghost-fluid fraction applies to the liquid-air side | E (not examined in detail) |

No error found. The paper's own critique by Robinson-Mosher et al. (below) applies to our rigid coupling too.

### Batty and Bridson 2008: viscosity (M7c §2)

| Paper | Ours | Class |
|---|---|---|
| min ∫ρ‖u − u_old‖² + 2Δt ∫μ‖(∇u + ∇uᵀ)/2‖²_F, volume integrals over the fluid only; stationary point = the backward-Euler PDE with zero traction | E(u) = ρ/2 ∫|u − u*|² + Δtμ ∫D:D (factor 2 apart, same minimiser); the same natural boundary condition | E |
| Strain-rate form (not the Laplacian), so rigid rotation is not damped | D:D form; case 10e: a rigid rotation is unchanged by a step | E |
| Stress locations: diagonal τ at cell centres, shear τ_ab on edges; weights = fluid volume fraction of the Δx³ cube around each sample | Diagonal strains at cell centres (weight w_c), shear on edges (2w_e), control-volume fractions from 4³ samples | E |
| Face (kinetic) weights V_u: fluid volume, *including* air volume at free-surface faces so the ghost fluid Dirichlet condition can be applied on top, excluding solid volume (stated for the pressure projection; "the same general method is applied in the viscous solve") | Kinetic weight m_f = the projection's: A between liquid cells, A θ to the air (θ clamped), 1e-6 for faces only the strains reach | D. Chosen so the two steps measure momentum alike; the paper does not scale these faces by θ. Effect untested against the paper's choice; the wave-damping case 10b converges to Lamb (order 1.2-1.3) with it |
| Solid walls: Dirichlet (variational solid weights); liquid level set extended into the wall | No slip: a face with its centre inside a solid is fixed; differences to it taken to the wall crossing with the ghost-fluid Dirichlet condition (θ clamped at 0.05); the liquid level set continues into solids as everywhere in the solver. Free slip: an additional mode | D (extension: second-order Dirichlet at the crossing; free slip is not in the paper) |
| Operator splitting: viscosity then pressure; PCG | Same, plus the incremental pressure (the previous pressure gradient is added to the viscous predictor) and the limit ν dt/h² ≤ 4, because the plain splitting does not give Stokes flow at large ν dt/h² (case 10d measured 26 % drag) | D (addition; the paper's demos use FLIP, where the problem does not show) |
| Variable viscosity (the paper handles it) | Not built | D |

No error found. The M7c text calls the strain energy "derived here"; it is the paper's.

### Robinson-Mosher et al. 2008: two-way coupling, shells (M7e §2.2)

| Paper | Ours | Class |
|---|---|---|
| Fluid in mixed dual cells is lumped onto the solid nodes (mass and momentum), conservation of momentum solved on the combined nodal mass; "alternatively" M̃_S = M_S + WᵀM̄_F W (symmetric, not diagonal) | The liquid in a closed face's control volume moves with the sheet: its kinetic energy ½ρh³Σw_f(G_fV − u*_f)², giving F = ρh³Σw_f G_fᵀG_f added to the sheet's S: the symmetric form | E |
| W: weights proportional to the area of the face intersecting a dual cell, per solid node, normalised so each row sums to 1 (so Wᵀ conserves momentum) | G_f: barycentric weights of the crossing on the crossed triangle (sum to 1) | D (both conserve; area-per-node weights spread a face's load over all nodes of all triangles meeting the dual cell, barycentric over one triangle) |
| The fluid face takes the solid's interpolated **velocity** (all components: u_SF = W V_S) | The face carries the **normal** component only: (w·V)·n · n_a | D (documented in M7d §2.2: no pumping of liquid along a sheet sliding in its own plane; consequence: no tangential traction, and linear momentum is exchanged only along n). The paper's choice would couple tangential motion |
| Solid integrated implicitly with D (linearised damping/elastic force); system SPD after block elimination or indefinite as a whole; reduced form costs O(n⁴) | Backward Euler on the linearised model, S = M(1 + dtα) + (dtβ + dt²)K, Schur complement applied with a banded Cholesky of (S + F) inside PCG | E |
| Shells have zero volume, so all fluid in the dual cell is lumped | A closed face's liquid is lumped likewise; "the face's fraction before the sheet closed it" is the weight | E |
| Critique of Batty et al. 2007: splitting impulse by volume fractions is non-physical when densities differ (a dual cell mixes two materials), so torque can be wrong, and it fails for zero-volume shells | Our rigid-body coupling is Batty's, so it inherits this; the Archimedes, added-mass and bobbing cases (M7b) agree with references to 1e-2 to 1e-4 | D (known limitation; the cases that would show it, a body of very different density spanning one cell, are not in the suite) |

No error found.

### Guendelman, Selle, Losasso and Fedkiw 2005: thin shells (M7d §2)

Authors: **Guendelman, Selle, Losasso and Fedkiw**; M7d, M7e and M7f (and `LiquidSheets.h` line 8) say "Guendelman, Bridson
and Fedkiw", which is wrong (Guendelman, Bridson and Fedkiw is the 2003 non-convex rigid body stacking paper). M7d is
corrected; M7e, M7f and `LiquidSheets.h` are other agents' files and still carry it.

| Paper | Ours | Class |
|---|---|---|
| A Neumann condition on a cell face when the segment between the two adjacent cell centres intersects a triangle | The same criterion: close the face whose segment a triangle crosses (M7d §2.2) | E |
| The face velocity is "the appropriate component of the effective velocity of the object", the effective velocity being the positional change over the next fluid step | (v·n)n_a, v barycentric on the crossed triangle | D (normal component only; as in the Robinson-Mosher row above) |
| The solid is swept to its next position and the fluid made to move as the solid will; force on solids from a second pressure solve with the solid masses added in the Immersed Boundary style, "to improve the noisy pressure" | One coupled solve; the load is the pressure impulse on the closed faces (M7d, M7e) | D |
| Visible, occluded and inside points: semi-Lagrangian rays, extrapolation and the level set made visibility-aware so water does not act across a thin sheet or stick to it | Two leak guards (M7d §2.3) at the level set's stencils | D (same aim, different mechanism) |
| Enclosed regions with a null space: remove the right-hand-side component, so the region may change volume | Not compared (M7f's closed sheet cases) | not assessed |

## Not verified

- Haberman and Sayre 1958 (DTMB report 1143) and Bohlin 1960 (Trans. Roy. Inst. Tech. Stockholm 155): primary reports not reached; two secondary sources each (above).
- Lamb 1932 §349 equation and §355 damping, Happel and Brenner 1965 (drag formula), Newman 1977 p. 145: not fetched; secondary or independent derivations as listed.
- Brackbill, Kothe and Zemach 1992 and Denner and van Wachem 2015 themselves: the constants are as quoted in arXiv:2203.01963.
- Förster, Wall and Ramm 2007 beyond its abstract.
- Lobovský's front positions (figure only) and their supplementary files (404).
- General-n 2-D drop frequency and its damping: derived in M7c and re-derived here, not found in a source.
