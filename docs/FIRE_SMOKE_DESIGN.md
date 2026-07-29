# Fire & Smoke — Physics Simulation and Rendering Design

**Status:** DRAFT (revision 32 — after 4 internal review rounds, 6 external
expert review rounds, 20 post-r11 implementation-review rounds, and an r32
scope restoration; history in
[FIRE_SMOKE_DESIGN_HISTORY.md](FIRE_SMOKE_DESIGN_HISTORY.md)). No fire
*feature* code has landed; one Phase-A prerequisite has (the
trilinear-accessor repair, commit `2fba2b48`, in master). Phase gating per
§7.0.
**Companions:** [FIRE_SMOKE_SOLVER_SPEC.md](FIRE_SMOKE_SOLVER_SPEC.md)
(Phase-C solver implementation contract),
[RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md)
(repo-wide mutation/freeze/publication lifecycle this arc depends on).
**Date:** 2026-07-28 (r6–r32; r1–r5 were 2026-07-27).
**Goal:** accurately simulate fire and smoke — both the dynamics and the visual
radiometry — and use the effort to improve RISE in two distinct categories:

1. **Simulation-as-content-source.** Establish the pattern of a physics
   simulation that produces rendering elements (field grids), with a clean
   contract between simulator and renderer, reusable by future sims (clouds,
   snow, general fluids).
2. **Renderer capability.** Land the heterogeneous-medium and emission
   improvements the phenomenon demands: temperature-driven spectral volume
   emission with a collision-based estimator, emissive-volume next-event
   estimation with a fully specified MIS, chromatic heterogeneous media,
   time-varying grids, and (stretch) refractive-index-gradient ray bending.

The document is organized so a combustion person can review §2–§3, a
transport person §4–§7, and a pipeline person §8–§10 independently.
§12 records design decisions taken and the questions still open.

---

## 1. Scope and non-goals

**In scope:** buoyant diffusion flames at everyday scales — candle, campfire,
pool fire, torch, burning object — and the smoke they produce; the field
representation, transport, and spectral radiometry to render them predictively
(no artist color ramps); an offline simulator producing grid sequences; the
renderer features to consume them. **Ember/char-bed glow and lofted sparks**
are visually co-dominant with the gas flame at campfire scale; they are
surface/particle Planck emitters the existing pipeline handles cheaply and are
in scope as a Phase D item (§7.4), not a core driver.

**Dry-aerosol scope boundary:** “smoke” in this arc means soot carbon plus the
modeled dry condensed-organic surrogate. Water vapor, liquid-water aerosol,
humidity-driven particle growth, steam-cloud optics, and wet-fuel aerosol
thermodynamics are out of scope. Grid metadata records this as
`aerosol_humidity_model = none`; a future wet-smoke preset requires transported
water, its latent coupling, and multi-RH extinction validation before it may
claim the §1 fidelity bar. This restriction is material for wet wood and other
high-moisture fuels and is reported by the simulator when such a fuel preset is
requested.

**Loading scope boundary:** predictive dynamics additionally require
χ_load=(c_carbon+c_condensed)/ρ_g≤1 %. Heavily sooting black-plume states near
f_v≈10⁻⁵ can reach ~9 % loading and are outside this arc's one-fluid momentum
model even though their optics remain useful renderer fixtures. §3.2 makes an
exceedance a hard predictive error; Phase D owns two-way aerosol momentum.

**Non-goals (this arc):**
- Premixed-combustion engineering (engines, detonation). Premixed chemistry
  appears only insofar as it explains blue chemiluminescent flames, which we
  model radiometrically, not kinetically.
- Detailed chemical kinetics (GRI-Mech-class mechanisms). The design adopts
  the mixing-limited fast-chemistry closure (§3.1). Consequence, stated
  honestly: finite-rate phenomena — liftoff, local extinction beyond the
  simple ignition criterion of §3.3, blow-off — are out of regime.
- Solid-phase spread prediction (char, ignition, fire-safety engineering).
  Pyrolysis appears only as a boundary condition / source term.
- Wind/cross-flow. Quiescent ambient only for this arc; co-flow BCs are a
  natural later extension of §3.6 but no validation row depends on them.
- Real-time simulation. The simulator is offline; the renderer consumes cached
  grids, matching the existing animation workflow
  ([skills/rise-animation-render.md](skills/rise-animation-render.md)).

**Fidelity bar:** "accurate" means (a) the sim reproduces the empirical
scaling laws in §3.8 within their published scatter, and (b) the renderer's
flame/smoke color and intensity follow from measured material properties
(soot optical constants, Planck emission) with **no per-scene tuning beyond
exposure** — the same bar the spectral pipeline already meets for surfaces.
Where a modeling choice compromises this bar, the compromise is stated at the
point of decision (see §3.4 on soot-yield calibration and §4.3 on smoke
optical mapping).

**Fidelity state is enforceable, not descriptive prose.** A simulator/importer
supplies source qualification and producer gate evidence as the declared,
digest-covered manifest fields defined in §8; a `fire_medium`
scene/job independently requests `fidelity_mode=predictive|preview`. Predictive
is fail-closed: any unmet producer or renderer prerequisite in §7.0/§8 aborts
before fields are accepted or render workers launch; it never silently
downgrades. Preview may proceed, but the renderer derives
`render_fidelity_status=preview` plus stable producer/render/artifact reason codes. Grid
provenance carries source qualification only; output provenance carries the
request and freshly derived render result. §8 gives the complete ownership and
transition table.

---

## 2. Physics background — what fire is

Shared vocabulary for the rest of the document. Reviewers who know combustion
should check for errors that would propagate into the field design (§3.3) and
radiometry (§4).

### 2.1 Diffusion flames and the fast-chemistry limit

Nearly every visible everyday flame is a **diffusion (non-premixed) flame**:
fuel and oxidizer arrive separately and burn where they mix to
near-stoichiometric proportions. At flame temperatures the oxidation
chemistry is fast (∼ms) relative to the turbulent mixing that delivers
reactants (∼10–100 ms), so **mixing, not kinetics, is rate-controlling**
("mixed is burnt"). This licenses the mixing-limited closure of §3.1:
one conserved scalar locates the flame, a mixing-rate expression sets the
local heat release, and no reaction ODEs are integrated per cell.

### 2.2 The solid-fuel chain: pyrolysis → gas flame → soot → smoke

1. **Pyrolysis.** A solid fuel (wood, wax via wick, polymer) heated above
   ≈300 °C thermally decomposes into volatile gases — CO, H₂, CH₄, heavier
   hydrocarbons — plus solid char. The flame never sits on the solid; it sits
   in the gas above it. Pyrolysis is endothermic and driven by heat fed back
   from the flame (largely radiatively at scale). For our purposes it is a
   **boundary source term**: a fuel mass flux with given composition,
   injection temperature, and heat of combustion.
2. **Gas-phase oxidation.** Volatiles burn where the local mixture fraction
   crosses stoichiometric. Heat release peaks in a thin sheet (the "flame
   surface"). Adiabatic flame temperatures are 1900–2300 K; **instantaneous**
   flame-sheet temperatures in buoyant diffusion flames reach 1800–1950 K
   after radiative loss, while **time-averaged** centerline values are
   1200–1700 K. The distinction matters downstream: soot radiance scales
   ≈ T⁴⁺, so rendering from averaged-looking fields underpredicts luminosity
   (turbulence–radiation interaction; see §3.8 caveat and §12's TRI
   decision).
3. **Soot lifecycle.** In the hot fuel-rich region *inside* the flame sheet,
   hydrocarbons polymerize into polycyclic aromatics (PAHs), which nucleate
   into incipient particles (1–5 nm), grow by surface reaction (HACA:
   H-abstraction/C₂H₂-addition) and coagulation into fractal aggregates
   (primary particles 10–100 nm, aggregates up to microns; fractal dimension
   D_f ≈ 1.8), and are then largely **oxidized away** as flow carries them
   through the flame sheet into oxygen. Soot inception occurs in a
   temperature window of roughly 1300–1850 K (below: no inception; above:
   oxidation/fragmentation wins) — background physics; the model-level soot
   closure is §3.4. Typical soot volume fractions f_v ∼ 10⁻⁷–10⁻⁶ in small
   flames, up to ∼10⁻⁵ in heavily sooting pool-fire plumes. The luminous
   yellow flame body **is** this soot, thermally glowing.
4. **Smoke** is the soot that escaped oxidation (smoke point exceeded), plus
   condensed organics and water aerosol after cooling and dilution. This arc's
   explicit dry-aerosol boundary (§1) models the first two and excludes water.
   Sooty fuels (diesel, tires, PMMA) make black smoke; clean fuels (methanol,
   natural gas) burn with almost none — note methanol still radiates
   strongly via gas bands (§2.4), a constraint on the radiative-loss model.

### 2.3 Chemiluminescence — the blue is not thermal

The blue at a flame's base (and the entire color of premixed flames — gas
stove, bunsen) is emission from electronically excited radicals produced by
the chemistry itself, not by temperature:

| emitter | spectrum | where |
|---|---|---|
| CH* | A²Δ→X band at 431 nm; B²Σ⁻→X band at ~390 nm (distinct from CN violet at 388 nm — a classic confusion) | thin sheet at near-stoichiometric Z |
| C₂* | Swan bands, 470–560 nm | slightly rich side of the sheet |
| CO₂* | broad continuum, ~250–600 nm, peaking in the near-UV/blue | reaction zone volume |

Total chemiluminescent radiant power is tiny — on the order of 10⁻⁴ of the
heat release rate, vs 15–35 % radiated thermally — but it dominates the
*visible* signature wherever soot hasn't formed (flame base, edges, premixed
pilot flames). It is spatially **anchored to the reaction zone**, not to the
soot field, so it needs its own emission term (§4.4) keyed to the sim's
band-resolved chem source fields derived from reaction state, with separate
line/continuum SPDs—a genuinely spectral effect an RGB pipeline can only fake.

### 2.4 Dynamics: buoyancy, puffing, baroclinicity, radiative loss

- **Buoyancy.** Combustion products at ∼1500 K are ≈4–5× less dense than
  ambient; the resulting acceleration is the engine of the plume. Density
  ratios this large are far outside the Boussinesq approximation's validity
  (ρ′/ρ ≪ 1), so the sim must be **variable-density low-Mach** (§3.2), not a
  Boussinesq buoyancy hack.
- **Puffing.** Pool fires shed toroidal vortices at the base with the
  well-verified empirical frequency **f ≈ 1.5 / √D Hz** (D = base diameter in
  meters; Cetegen & Ahmed). This 2–3 Hz pulsation at campfire scale is the
  dominant visual rhythm of fire, and a free validation target (§3.8): a
  solver whose numerics are too dissipative simply fails to puff.
- **Baroclinic vorticity.** Vorticity is generated where density gradients
  misalign with pressure gradients (the ∇ρ × ∇p term). This is *the* physical
  source of the rolling structure at plume edges; standard graphics solvers
  (constant-density + buoyancy force) lack the term and compensate with
  vorticity confinement. A variable-density solver gets it for free — a
  concrete accuracy argument for §3.1's formulation choice.
- **Radiative loss.** Flames lose 15–35 % of heat release radiatively
  (radiative fraction χ_r), through **two channels**: continuum soot emission
  *and* molecular gas bands — hot CO₂ (2.7, 4.3 µm) and H₂O (1.4, 1.9, 2.7,
  6.3 µm). For lightly sooting fuels the gas bands carry the majority
  (methanol: χ_r ≈ 0.15–0.20 with f_v ≈ 0). A soot-only loss model would
  over-predict clean-fuel flame temperatures by hundreds of K. Loss caps real
  flame temperature well below adiabatic and couples dynamics to appearance:
  more soot → more loss → cooler, redder flame. Model in §3.5.
- **Regimes by scale.** Candle: laminar (Re ∼ 10²), essentially steady, a
  DNS-resolved case (§3.7) and analytic-profile test asset (§7.1). Campfire:
  transitional, puffing. Pool fire ≥ 0.5 m: fully turbulent LES.

---

## 3. Simulation design

### 3.1 Formulation decision

Three candidate formulations:

| | level-set thin flame (Nguyen–Fedkiw–Jensen 2002) | pure conserved-scalar state relation (classic mixture fraction / FDS 5-era) | transported species + enthalpy, mixing-limited reaction (FDS 6-style) |
|---|---|---|---|
| flame representation | tracked level set + ghost-fluid jump | iso-surface of Z; T = lookup T(Z) | reaction-rate field where fuel and oxidizer coexist |
| temperature | separate model needed | algebraically slaved to Z — **cannot carry a radiative-loss history or lag Z under puffing** | transported sensible enthalpy; T lags mixing, radiative sink natural |
| fuel-type fit | premixed fronts | diffusion flames | diffusion flames |
| what fire science uses today | — | superseded | **this (FDS 6)** |
| hands the renderer | front geometry only | T, Z | **T, reaction rate, composition — the full §4 contract** |

**Decision: the FDS 6-style transported formulation.** Revision 1 of this
document ambiguously mixed the second and third columns; reviewers correctly
flagged them as mutually exclusive. The transported formulation is the
commitment, for two load-bearing reasons: radiative loss — which sets the
1200–1700 K rendered color of the flame — is a *sink in a transported energy
equation* and is inexpressible in a pure state-relation lookup without adding
an enthalpy-defect dimension; and T must be able to lag Z under puffing.
Equilibrium composition-vs-Z relations survive only as *property lookups*
(mean molecular weight, c_p) — never as the temperature model. The flame
sheet is recovered as the support of the reaction-rate field; no explicit
surface tracking.

### 3.2 The low-Mach variable-density system

Specified to the level a CFD reviewer can check, since every §3.8 validation
row depends on it:

- **Pressure split.** p(x,t) = p₀ + p̃(x,t): thermodynamic background p₀
  (constant — open domain) and dynamic perturbation p̃ ≪ p₀. Density and
  temperature couple through the equation of state at p₀ only:
  **ρ_g = p₀ W̄ / (R T)**, with mean molecular weight W̄ from the lumped-species
  composition. Acoustics are removed by construction; p̃ enters only the
  momentum equation.
- **Momentum and transport closure.** The conservative gas-momentum equation is

  > ∂(ρ_gu)/∂t + ∇·(ρ_gu⊗u + u⊗J_g)
  > = −∇p̃ + ∇·τ_eff + (ρ_g−ρ∞)g + u S_ρ,phase,
  > τ_eff=μ_eff[∇u+(∇u)ᵀ−(2/3)(∇·u)I],
  > μ_eff=μ_mol+ρ_gν_sgs.

  where J_g=Σ_{k∈gas}J_k=−Σ_{i∈aerosol}J_i. The u⊗J_g flux is required because
  constituent diffusion changes gas mass even though the all-constituent flux
  sums to zero; omitting it changes M/ρ_g for a uniform translating loading
  gradient and breaks Galilean invariance. The phase source
  S_ρ,phase=ṁ̇‴_gas from below carries the common velocity,
  so phase transfer alone cannot accelerate the one-velocity mixture; boundary
  injection carries its prescribed momentum separately. Buoyancy is relative
  to the ambient hydrostatic state, so the unignited far field is quiescent.
  The baseline molecular record uses species μ_k(T), k_k(T) tables, Wilke
  viscosity mixing and Wassiljewa/Mason–Saxena conductivity mixing. With the
  declared unit-Lewis closure,

  > D_mol=k_mol/(ρ_gc_p,g),
  > D_sgs=ν_sgs/Sc_t,
  > D=D_mol+D_sgs,
  > k_eff=k_mol+ρ_gc_p,gν_sgs/Pr_t,

  and the same D drives every J_j and J_Z. Baseline Pr_t=Sc_t=0.7. Vreman uses
  α_ij=∂u_j/∂x_i, β_ij=Σ_mΔ_m²α_miα_mj,
  B_β=β_11β_22−β_12²+β_11β_33−β_13²+β_22β_33−β_23², and
  ν_sgs=C_v√(B_β/(α_ijα_ij)) with nonnegative B_β, zero when the denominator
  vanishes, **C_v=0.07**, and Δ_m equal to the directional MAC cell widths
  (uniform in Phase C). The versioned `transport_closure` record owns all
  property tables/ranges/mixing laws, Pr_t, Sc_t, C_v, filter widths, wall
  stress choice, τ_chem, T_CFT, and the relationships above; predictive mode
  permits no implicit library defaults. DNS sets ν_sgs=D_sgs=0 but retains the
  molecular laws. No-slip/adiabatic walls use resolved molecular stress/heat
  flux; no wall function is used in this arc.
- **Divergence constraint** derived from the EOS at constant p₀ **and the
  transported total-sensible-energy → T inversion together**. Aerosol mass
  shares the gas velocity in the supported one-fluid limit but contributes
  heat capacity, not pressure. Define the conservative sensible-energy
  density and its temperature derivative

  > ℋ_s = ρ_g Σ_k Y_k h_{s,k}(T) + Σ_i c_i h_{s,i}(T),
  > C_T = ∂ℋ_s/∂T = ρ_g c_{p,g} + Σ_i c_i c_{p,i},

  where i spans carbon and condensed-organic aerosol. With S_k the
  conservative gas-species source (kg·m⁻³·s⁻¹), S_{a,i} the conservative
  aerosol source, and

  > J_h = Σ_j h_{s,j}(T)J_j,
  > H = q̇‴_step + q̇‴_lat − q̇‴_rad
  >     + ∇·(k_eff∇T) − ∇·J_h,

  the complete local identity used by the projection is

  > ∇·u = H/(C_T T)
  >       + Σ_k [ W̄/(ρ_g W_k) − h_{s,k}/(C_T T) ] S_k
  >       − Σ_i h_{s,i} S_{a,i}/(C_T T).

  All three composition contributions are load-bearing: the W̄/W_k part is
  the molecular-weight effect (CH₄ 16 vs air 28.9 g/mol near the source),
  while the gas- and aerosol-enthalpy terms keep expansion EOS-consistent
  when species with different enthalpies mix, react, or change phase. Omit
  them and different histories give different expansion at identical
  (T, composition), the energy-imbalance failure NIST documents for
  low-Mach LES.
  **Implementation requirement:** S_div is derived from the *actual
  discrete* scalar/enthalpy updates (same operators, same limiters), not
  from the continuous form independently — and §3.8's verification tier
  gates it with a two-gas mixing box and a reacting
  manufactured-solution test. Gas and aerosol sensible enthalpies are defined as
  temperature integrals, h_{s,k}(T) = ∫_{T_ref}^{T} c_{p,k}(T′)dT′, so
  with C_T ≡ ∂ℋ_s/∂T there is no separate "Dc_p/Dt" to neglect (r6
  claimed one — an external-review correction). **J_h is not optional**:
  it uses the same accepted gas and aerosol face fluxes J_j as the
  constituent update, with h_s evaluated at the common face temperature.
  Omitting it makes an isothermal unequal-c_p diffusion problem change
  temperature and makes that error depend on the arbitrary T_ref.
- **Variable-coefficient projection.** Enforce the constraint via
  ∇·((1/ρ_g) ∇p̃) = (∇·u* − S_div)/Δt, S_div the right-hand side above
  (units s⁻¹). At density ratios of 4–5× this is a genuinely
  variable-coefficient Poisson problem: **geometric multigrid** is the
  planned solver (FFT/constant-coefficient shortcuts do not apply; FDS's
  constant-coefficient splitting trades this for a documented pressure error
  term — we take the multigrid cost instead). Residual target:
  ‖∇·u − S_div‖∞ below max(10⁻³·‖S_div‖∞, ε_abs) per step — S_div and the
  floor are in s⁻¹; ε_abs is set per scene as 10⁻³·(u_ref/L), the
  buoyant-velocity-over-domain scale, covering cold-start steps where
  S_div ≈ 0; solver accumulates in fp64 regardless of field precision
  (§3.7). Constituent sensible-enthalpy diffusion and every frozen
  step-integrated local-source packet enter S_div through the exact average
  increments consumed by that projected advance (§3.4–§3.7), keeping scalar,
  energy, and constraint operators consistent within a stage. A source map
  applied to fixed Eulerian cells and followed only by a projection is
  forbidden: projection changes velocity, not the already-heated density, so
  it cannot repair the EOS.
- **Phase-transfer closure — gas vs aerosol, defined** (round 4: soot
  formation and condensation move mass *out of the gas phase*; burnout
  and evaporation move it back — the divergence system must know). **ρ_g in
  the EOS and every gas-species equation is GAS density**; the aerosol
  inventories (c_carbon, c_condensed) are separate pressureless mass
  fields contributing **no molecular partial pressure** (the earlier "W̄
  closes over c_carbon" phrasing was wrong on that point — solid carbon
  enters the *element* balance, never the pressure). Gas continuity
  carries the **phase-transfer mass source**, pinned (round 5 — with the
  corrected burnout term):

  > ṁ‴_gas = ( −Δc_form − Δc_cond + Δc_ox + Δc_evap ) / Δt,

  where burnout's net gas gain is **+Δc_ox — the carbon mass only** (the
  2.667 O₂ mass was already gaseous; r9's "+3.667·Δc_ox" was wrong).
  **No separate ṁ‴_gas/ρ_g term is added to S_div** (round 5 caught r9
  double-counting it). Diffusion is written in the total-mixture variables
  pinned below: for every gas or aerosol constituent mass density q_j, define
  X_j=q_j/ρ_tot and J_j=−ρ_totD∇X_j. The conservative gas RHS is
  **S_k=ω̇_k−∇·J_k**. Thus Σ_kω̇_k=ṁ‴_gas, while Σ_kS_k also includes the
  resolved gas share of the zero-sum constituent diffusion flux
  (Σ_all j J_j=0). The gas-species and aerosol coefficient sums in the
  displayed divergence constraint therefore carry the complete phase-transfer
  and diffusion effects; no extra mass-source term is added. V2 gains
  manufactured condensation, burnout, and loading-gradient diffusion cases.
  **Momentum and buoyancy use gas density with aerosol inertia
  neglected — in a RESTRICTED loading regime** (round 5: the
  r9 "< 10⁻³" bound was numerically false — at f_v = 10⁻⁶ against
  hot-gas ρ ≈ 0.2 kg/m³ the loading is ~0.9 %, and ~9 % at the f_v = 10⁻⁵
  extreme). With c_aer = c_carbon + c_condensed, the supported regime is
  χ_load = c_aer/ρ_g ≤ 1 %. Aerosol heat
  capacity is **dynamically coupled through ℋ_s and C_T above**; only aerosol
  momentum inertia is neglected. Exceeding the limit is a hard
  unsupported-regime error for predictive output, not a warning that lets a
  run retain the fidelity label. The f_v ≈ 10⁻⁵, χ_load ≈ 9 % heavily-sooting
  pool-plume extreme in §2.2 is therefore background/out-of-envelope until
  two-way aerosol momentum lands in Phase D. V4 gains **inert-aerosol
  heating/cooling at χ_load = 1 %, gas↔soot, and vapor↔condensate closed-box
  EOS/energy tests**.
- **Density update policy: conservative-density primary.** The numerical gas
  density is **ρ_g ≡ Σ_k q_k** after every accepted scalar stage. Y_k=q_k/ρ_g,
  W̄ follows from those normalized gas fractions, and T follows from the
  conservative ℋ_s inversion. The constant-p₀ EOS is a compatibility
  constraint enforced dynamically by S_div, not a second owner allowed to
  overwrite ρ_g. Implementations must never rescale q_k to an independently
  diagnosed EOS density or alter ℋ_s merely to close the EOS; either operation
  destroys a conservation ledger. After each corrector the residual

  > r_EOS = |ρ_g R T/(p₀W̄) − 1|

  is monitored cellwise. A step with ‖r_EOS‖∞>10⁻³ is rejected, and V2 must
  additionally show the residual converging at the scheme's formal order under
  Δt and Δx refinement. This makes EOS drift a diagnostic of inconsistent
  scalar/energy/projection operators while preserving ΣY_k=ΣX_j=1 exactly to
  fp64 reduction tolerance.

### 3.3 Transported fields and the reaction closure

**Mixture-fraction ownership is total-mixture, not gas-only.** Define
ρ_tot = ρ_g + c_carbon + c_condensed and transport the conservative scalar
ρ_tot Z. For every gas species use q_k=ρ_gY_k; for the aerosol constituents
use q_i=c_i. Their total-mixture fractions X_j=q_j/ρ_tot sum to one. Ambient
material has Z = 0 and injected fuel material Z = 1;
reaction, soot formation/burnout, and condensation/evaporation only repartition
that material and therefore do not source Z. In the supported one-velocity,
unit-Lewis limit,

> ∂(ρ_tot Z)/∂t + ∇·(ρ_tot u Z) = −∇·J_Z,
> J_Z = −ρ_tot D∇Z,

If b(Z) is the total-mixture elemental mass-fraction vector, the gas-normalized
element constraint used by the limiter is

> E_g Y + E_a(c/ρ_g) = (1 + χ_load) b(Z),

not E_gY + E_a(c/ρ_g) = b(Z). This convention keeps Z unchanged through a
closed gas↔aerosol cycle and removes phase-transfer source ambiguity. The
production one-fluid closure uses **J_j=−ρ_totD∇X_j for every constituent**
(SGS mixing dominates; differential and Brownian diffusion are neglected).
Because b(Z) is affine, the elemental flux assembled from the same face
gradients is **in exact arithmetic** J_b=−ρ_totD∇b(Z); J_Z is not evaluated by
an independent stencil. This is what makes the summed elemental and Z fluxes
identical when aerosol loading varies. **Exactness is structural, not
numerical**: it holds only insofar as the cell-centre states satisfy
Σ_jX_j=1 and E X=b(Z), which fp64 rounding and the limiter's acceptance
tolerance perturb at the ulp level. §3.7's `nonadvective_flux_projection_v1`
is what enforces the identity against that residual — it is a
rounding/consistency projection onto the same subspace this paragraph
defines, **not an independent flux model**, and it is a no-op in exact
arithmetic. Its RED gate is meaningful because the unprojected residual
accumulates over a run.

**The gas state is an exhaustive versioned schema, not the illustrative rows
below.** A `gas_thermochemistry` record enumerates every q_k, including the
inert carrier (N₂ for the baseline air record), O₂, lumped fuel, CO₂, H₂O,
condensable vapor, and any additional product/inert species. For each it pins
the nominal elemental formula/column of E, W_k, valid T range, c_p,k(T) and
h_s,k(T) table/polynomial with interpolation rule and one common T_ref. It also
pins p₀, ambient and injected-fuel compositions **and their temperatures**
(including the unique sequence T_inf), the
fuel formula/LHV, and the atom-balanced primary product coefficients used with
the withheld streams. E carries **every conserved element present in the
schema, including N for an N₂ carrier**, not a hard-coded C/H/O subset. Unknown
species or an unbalanced coefficient record is a load error; the exact record
bytes/hash ride in §8 metadata. This one record owns W̄, ℋ_s inversion, S_div,
EOS checks, reaction products, and gas-opacity composition, so those consumers
cannot quietly use different thermochemistry.

Record admissibility also closes the inverse, rather than merely naming one.
Every h_s,k is continuous on its closed certified interval, satisfies
h_s,k(T_ref)=0, and has derivative exactly equal to the record's interpolated
c_p,k(T); the generator certifies c_p,k(T)≥c_p,min,k>0 over that whole interval.
The gas and aerosol records must have one nonempty common temperature interval
[T_min,T_max]. For every limiter-admissible non-vacuum composition/loading,

> C_T(T,q)=Σ_k q_k c_p,k(T)+Σ_i c_i c_p,i(T) ≥ C_min(q)>0,

so ℋ_s(T,q) is continuous and strictly increasing. Before every inversion require

> ℋ_s(T_min,q) ≤ ℋ_s,accepted ≤ ℋ_s(T_max,q),

then use a bracket-preserving root solve on that common interval. There is
exactly one T. A discontinuity, h_s/c_p derivative mismatch, nonpositive lower
bound, empty common interval, or unbracketed accepted energy rejects the record
or current step; clamping/extrapolation and implementation-selected roots are
forbidden. Generator RED fixtures inject each defect, and a multicomponent
gas+aerosol round trip tests monotonicity and inversion at interval endpoints.

| field | symbol | equation / source | sink | consumed by |
|---|---|---|---|---|
| mixture fraction | ρ_tot Z | conservative total-mixture scalar; fuel-inflow BC | — | property lookups; elemental feasible set; diagnostics (incl. the Y_O consistency check below) |
| fuel gas mass | q_F=ρ_gY_F | fuel-inflow BC; total-mixture diffusion | reaction (below) | reaction rate |
| oxygen gas mass | **q_O=ρ_gY_O (prognostic — r8)** | ambient/entrainment BC; total-mixture diffusion | gas reaction (s_st,eff per §3.4); soot burnout (2.667·Δc_ox) | reaction rate; burnout; products/W̄ element balance |
| inert/carrier gas masses | q_inert (N₂ in baseline air; exhaustive list comes from `gas_thermochemistry`) | ambient/fuel BC; total-mixture diffusion | — unless the versioned reaction schema says otherwise | ρ_g, W̄, ℋ_s, complete element ledger |
| radiating product gas masses | q_CO2, q_H2O | primary reaction and soot burnout, atom-balanced; total-mixture diffusion | — | W̄/EOS; gas-band κ_P from actual history (§3.5), never Z alone |
| total sensible-energy density | ℋ_s = ρ_gΣY_kh_{s,k}+Σc_ih_{s,i} | q̇‴_step + q̇‴_lat; conservative enthalpy transport | q̇‴_rad (§3.5) | **T = T(ℋ_s, gas+aerosol composition)** → EOS, buoyancy, Planck emission (§4.2) |
| carbon aerosol mass concentration | c_carbon | gross formation (§3.4) | oxidation (§3.4) | exported; renderer blends hot/cool optical regimes with φ(T), while both absorptive regimes emit by Kirchhoff (§4.1–§4.3) |
| condensed organics mass concentration | c_condensed | condensation of Y_cv per the §3.4 saturation rule | re-evaporation per the same rule | exported; droplet-preset scattering (§4.3) |
| condensable-vapor gas mass | q_cv=ρ_gY_cv | y_cond·δm_F with reaction; re-evaporation from c_condensed (§3.4 saturation rule) | condensation to c_condensed | conserved condensable inventory |
| reaction rates (derived) | q̇‴_gas, q̇‴_sox | conservative step rates (closure below — q̇‴_inst is closure-internal) | — | total q̇‴_step = gas+sox → S_div, ℋ_s, Q̇_tot, §3.5; q̇‴_gas → diagnostics and the versioned band-source closure below |
| chem radiant source powers (derived) | q̇‴_CH, q̇‴_C2, q̇‴_CO2* | versioned state-dependent band-yield closure applied to q̇‴_gas; zero when gas reaction is zero | — | exported absolute chem channels; renderer §4.4 |
| velocity | **u** | momentum (buoyancy + baroclinic) | — | advection; renderer motion blur (§8) |

**Reaction closure** (mixing-limited, FDS-*style* — two stated deviations
below):

> q̇‴_inst = ρ_g · min(Y_F, Y_O/s_st,eff) / τ_mix · Δh_c,eff,   (s_st,eff, Δh_c,eff per §3.4's withheld-streams coefficients; q̇‴_inst is closure-internal — see the named rates below)
> **τ_mix = max( τ_chem, min( τ_diff, τ_u, τ_buoy ) )**,
> τ_diff = Δ²/(D_mol + D_sgs), τ_u = Δ/√(2 k_sgs), τ_buoy = √(2Δ/g′),

with k_sgs the subgrid kinetic energy (estimated from the Vreman model's
ν_sgs via the standard eddy-viscosity relation k_sgs = (ν_sgs/(C_ν Δ))²
with **C_ν = 0.1** — Vreman does not carry k_sgs directly; the constant is
pinned here so implementations don't mix conventions). The advective timescale is built on the
**subgrid** velocity scale, not the resolved ‖u‖ — resolved uniform
translation produces no subgrid mixing, and a resolved-speed form would
make the burning rate frame-dependent (Galilean non-invariant). g′ is
**signed** — cold heavy fuel or cooled products give g′ < 0 (stable
stratification) and √(2Δ/g′) goes non-real — so τ_buoy uses
**g′₊ = max(g·(ρ∞−ρ_g)/ρ_g, 0)**, with τ_buoy = ∞ when g′₊ = 0 (stable or
neutral; unignited injected fuel is *not* generally neutral, hence the
explicit clamp rather than an assumption). τ_chem is the fast-chemistry bound on the rate; at
this fidelity a fuel-independent constant τ_chem ≈ 10⁻⁴ s suffices (it
only matters in the thinnest, hottest cells). The min-of-timescales form
matters (revision 2 had τ_mix = Δ²/ν_sgs with a "floor," which is broken in
exactly the laminar limit: ν_sgs → 0 gives τ_mix → ∞ and the DNS candle
never ignites — and a *floor* bounds burning from above, the wrong
direction). Here the laminar limit burns on the **molecular-diffusive**
time and turbulent cells burn on the fastest available mixing process; in
the candle's DNS mode the subgrid terms τ_u and τ_buoy are disabled
outright along with the SGS model (§3.7) — they are subgrid constructs.
Deviations from FDS, stated: the flame-period bound (τ_flame) is omitted
as irrelevant at these scales, and τ_buoy uses reduced gravity g′₊ where
FDS uses plain g (defensible — buoyant stirring scales with the actual
density deficit — but a deviation).

**Discrete realization** (the external review noted the continuous rate has
no Δt-convergent naive discretization: with τ_chem = 10⁻⁴ s the hydro step
can be ≫ τ, and explicit ρ_gL/τ·Δt overshoots the available reactants, while
clipping / subcycling / implicit each give *different* heat-release
histories). The committed update is the exact exponential-relaxation form
for frozen per-step τ_mix:

> ΔY_F = L · (1 − e^{−Δt/τ_mix}),  L = min(Y_F, Y_O/s_st,eff),

This expression chooses the candidate reacted fuel mass
δm_F⁰ = ρ_g,begin·ΔY_F; §3.4's shared O₂ allocator produces the accepted
δm_F=θδm_F⁰. It is **not** applied by subtracting mass fractions in
place. Fuel, O₂, gas products, gross carbon, condensable vapor, gas total mass,
and ℋ_s are updated as conservative mass/energy densities from that accepted δm_F,
then all Y_k are recomputed using the accepted new ρ_g. This is the same
one-consumption bookkeeping used by the phase-transfer formulas. **Named
rates, every consumer assigned** (external-review catch — when
Δt/τ is not small the instantaneous and realized rates differ
substantially): the *instantaneous closure rate*
q̇‴_inst = ρ_g·L·Δh_c,eff/τ_mix exists **only inside the closure** (it is the
thing the exponential update integrates). The realized conservative step
rates are the *gas-phase* rate **q̇‴_gas = δm_F·Δh_c,eff/Δt** (Δh_c,eff — the
withheld-streams effective heat, §3.4, so gas + burnout sum to the fuel
LHV net of the withheld condensable energy, with no double count) and the
*soot-burnout* rate **q̇‴_sox = Δc_ox·Δh_soot/Δt** (§3.4), with
**q̇‴_step = q̇‴_gas + q̇‴_sox**. Assignments: the ℋ_s update, the S_div
divergence source, Q̇_tot, the §3.5 radiative normalization, and
diagnostics use the **total q̇‴_step**; the diagnostic exported `reaction`
channel and the simulator's band-resolved chem source closure use
**q̇‴_gas only**—excited CH*/C₂*/CO₂* radicals come from the gas flame, and
driving them with soot-burnout heat would put chemiluminescence in the wrong
place. §3.8's verification tier includes a **timestep-refinement
convergence test** on the heat-release history — the closure must produce
Δt-independent physics under the CFL-selected step.

**Ignition and extinction — two separate criteria** (revision 3 conflated
them; the CFT test alone is an *extinction* model and does not block cold
fresh mixtures — an adiabatically burnt cold stoichiometric pocket reaches
~2200 K > T_CFT, so it would ignite spontaneously):

- **Ignition gate** — two routes, with the timing controlled by *resolved
  physics*, not by the gate (the external review noted that a bare
  adjacency gate propagates at one cell per step, i.e. δx/Δt — so refining
  Δt would *accelerate* ignition, a non-convergent scheme):
  Build one **memoryless same-source-packet graph** from accepted beginning-of-
  step Uⁿ. A vertex exists only where both reactants are present,
  T>T_pilot (**T_pilot≈600 K**, per-fuel), and the extinction trial below
  passes. Edges use the 6-neighbor face stencil. Seeds are eligible cells in
  the fixed pilot-source mask plus eligible cells with T>T_AIT (≈700–900 K for
  common gaseous fuels; wood volatiles can be 500–600 K, per-fuel). React
  exactly the connected components containing at least one seed. Prior-step
  "reacting cells" are **not** seeds and no ignition flag is transported; this
  removes hidden hysteretic state and prevents a chain of vitiated/CFT-failing
  cells from bridging a pilot to a remote pocket. A connected region already
  heated into admissibility ignites at once; the heat PDE, not one-cell-per-
  step graph propagation, controls when vertices appear. Front convergence
  remains gated by §3.8 timestep refinement.

  This is what prevents the fuel-bed rim and re-entrained cold pockets
  from burning spontaneously while letting the flame spread at a physical
  rate.
- **Extinction gate (FDS critical-flame-temperature test):** a cell that
  *would* react is suppressed if adiabatic combustion of its mixed contents
  cannot reach T_CFT (empirical, ≈1700 K for hydrocarbons) — this kills
  vitiated/over-diluted mixtures. The graph's eligibility trial and the final
  reaction use the **same primary-combustion thermochemistry**, but the CFT
  trial itself is explicitly timestep-independent and primary-only. From the
  accepted Uⁿ state, hypothetically burn to completion
  min(q_F,q_O/s_st,eff) using s_st,eff, Δh_c,eff, the frozen atom-balanced
  products, and the local gas+aerosol ℋ_s/C_T. Soot oxidation does not compete
  in this hypothetical trial, and neither the exponential finite-step extent
  nor §3.4's θ enters it; using either would make eligibility Δt-dependent and
  circular. A passing trial merely enables formation of δm_F⁰, after which the
  real finite-step primary and soot candidates share O₂ through θ. Full-LHV or
  gas-only-c_p trial temperatures are forbidden.

No reactedness *progress variable* is needed — reactedness is implicit in
(Z, Y_F) (though Y_O is transported as of r8, as the oxidizer DOF the soot
bookkeeping requires — below); the two-route ignition gate plus the CFT
extinction test
together do the job a progress variable would (§12, closed).

**Y_O is transported, not algebraic** (r8 — the third external round's P0:
the algebraic relation Y_O = Y_O,∞(1−Z) + s_st(Y_F − Z·Y_F,1) leaves the
chemical state with **no degree of freedom for soot burnout's independent
local O₂ sink** — advected soot oxidizes far from its formation cell, and
"folding CO₂ into products" cannot be implemented from (Z, Y_F) alone).
Y_O is a prognostic scalar with ambient/entrainment BCs, consumed by the
gas reaction (at the withheld-soot effective stoichiometry s_st,eff, §3.4)
and by soot burnout (2.667·Δc_ox). The algebraic relation is **demoted to
a verification diagnostic**: it holds exactly wherever both withheld streams are
inactive (no soot, no condensables) (equal-diffusivity/unit-Lewis assumption, stated once here for
all of §3), and its residual elsewhere measures the soot bookkeeping —
monitored, with a V-tier gate. Realizability is a **joint elemental
feasible set enforced by a CONSERVATIVE limiter, not per-field
nonnegativity and not naive per-cell projection** (round 4 flagged the
former; round 5 the latter — independent per-cell projection creates or
destroys global elements, and the r9 inequality was neither the full
schema-element polytope nor unit-consistent across gas fractions and aerosol
concentrations): feasibility is defined by the **element matrix E** over
the full state — gas species mass fractions plus aerosol concentrations
converted as c/ρ_g — requiring componentwise E·y ≥ 0 and every schema-element
totals fixed by **(1 + χ_load)b(Z)** under the total-mixture convention
above; enforcement is the **invariant-domain-preserving coupled
finite-volume FCT scheme pinned in §3.7** (one face coefficient shared by
the full state, hence globally element-conservative by construction), with
reaction/burnout updates capping consumption by
availability (§3.3/§3.4). V3 gains a multicomponent fuel/air/aerosol
interface test checking local realizability and all global element totals.

**The gas-phase rate q̇‴_gas is exported as the diagnostic `reaction` grid
channel**, but it is not enough to reconstruct chem color: §2.3's CH*, C₂*,
and CO₂* sources occupy different mixture states. On the accepted source
packet the simulator evaluates one frozen, versioned band-yield record at the
**accepted pre-reaction Uⁿ state**—Zⁿ,Y_Fⁿ,Y_Oⁿ,Tⁿ and fuel_id, never the
post-reaction scratch products—and exports

> q̇‴_b(x)=η_b(state)q̇‴_gas(x),  b∈{CH,C2,CO2*},

as three nonnegative absolute radiant-power-density channels. The record owns
arguments, interpolation, certified domains, and calibrated spatial/band data;
out-of-domain evaluation fails predictive simulation. Renderer-side
Z-windowing remains rejected because it would discard simulator state and make
resolution-sensitive guesses. The `reaction` channel serves the flame-height
diagnostic (§3.8). All four channels have **instantaneous snapshot at frame
time; sub-frame aliasing accepted and documented** (24 fps is
Nyquist-adequate for the 2–3 Hz puffing signal; broadband turbulent
fluctuation strobing is accepted at this fidelity — an optional
frame-interval-averaged channel is a future extension if it proves visible).

### 3.4 Soot model: fixed yield as a product species

Revision 1 proposed the Leung–Lindstedt–Jones two-equation model; reviewers
rejected it as incoherent here (its rates are Arrhenius in acetylene
concentration — a species this formulation does not carry; equilibrium C₂H₂
at rich Z is a poor surrogate; and its constants have no published set for
wood volatiles or wax, making it precision theater).

**Decision: gross formation + oxidation, calibrated to the published NET
yield — with a conserved aerosol inventory.** The external review caught a
definitional error r3–r5 carried: FDS's soot yield y_s is defined as soot
mass **in the products** per fuel mass reacted — a *net* quantity with the
remaining reaction coefficients atom-balanced. Producing at the published
y_s and *then* oxidizing again would undercount smoke and silently skip
the burnout bookkeeping. The corrected model:

- **Atom- and energy-balanced GROSS primary coefficients** (r8 — the
  third external round's P0: r7's primary step released the *full* Δh_c
  and consumed complete-combustion O₂ while *also* withholding y_form
  carbon as soot whose burnout later released *another* 32.8 MJ/kg —
  double-counting up to ~16 % of aromatic-fuel heat). The primary
  reaction **withholds BOTH non-burning product streams — soot carbon AND
  condensable organics — with their O₂ and chemical energy** (round 4:
  r8 withheld soot but let y_cond appear without withholding *its*
  primary products, O₂ demand, and energy — the same double-count class):
  per kg of fuel consumed, the primary step releases
  **Δh_c,eff = Δh_c − y_form·Δh_soot − y_cond·Δh_cv**, consumes O₂ at
  **s_st,eff = s_st − 2.667·y_form − s_cv·y_cond** (s_cv = the
  condensable's stoichiometric O₂ demand per unit mass from its formula —
  ≈ 1.18 kg/kg for C₆H₁₀O₅), and emits y_form of
  gross soot carbon plus y_cond of condensable vapor. Primary reaction plus
  complete **soot** burnout therefore releases
  **Δh_c − y_cond·Δh_cv** and consumes
  **s_st − s_cv·y_cond**, not the complete-combustion values; the remaining
  condensable chemical potential and O₂ demand stay explicitly withheld
  because condensable oxidation is not modeled. V4 gates the invariant
  energy ledger at primary formation, partial burnout, and final state.
  **Fuel-record admissibility is a hard preflight gate:** y_form≥0,
  y_cond≥0, s_st,eff>0, Δh_c,eff>0, and every atom-balanced primary product
  coefficient≥0. Failure is not clamped or logged-and-continued: it makes the
  record invalid for predictive simulation (preview may load only as an
  explicitly non-reacting diagnostic fixture). Boundary tests exercise zero,
  just-positive, and deliberately invalid coefficients so the
  `Y_O/s_st,eff` limiter can never become singular/negative and primary
  combustion can never be an endothermic step mislabeled as fire.
  **Primary combustion and soot burnout share one O₂ allocator.** From the
  same accepted Uⁿ source-packet input form the individually bounded candidate
  extents δm_F⁰ (§3.3 exponential map) and Δc_ox⁰ (below). Let

  > D_O=s_st,eff δm_F⁰+2.667Δc_ox⁰,
  > θ=1 when D_O=0, otherwise min(1,q_O/D_O),
  > δm_F=θδm_F⁰,  Δc_ox=θΔc_ox⁰.

  All fuel/O₂/products, soot/condensable inventories, heat, exported reaction
  rates, and radiative Q̇_tot use only these accepted extents. No priority rule
  or independent double claim on q_O is permitted. A closed cell in which both
  candidates individually request all available oxygen is a V4 gate.
  Soot advects, then
  **mixing-limited oxidation with a stated rate equation** consumes the
  *transported* Y_O (§3.3):

  > Δc_ox⁰ = min( c_carbon , ρ_g·Y_O/2.667 ) · (1 − e^{−Δt/τ_mix}),
  > active only where T > T_ox (≈ 1300 K — soot oxidation effectively
  > freezes below it),

  with accepted Δc_ox=θΔc_ox⁰ from the shared allocator, releasing
  Δh_soot·Δc_ox into ℋ_s (= q̇‴_sox, §3.3) and its CO₂ into the
  element-balanced products. **The element/W̄ closure spans the full state
  (Z, Y_F, Y_O, Y_cv, c_carbon, c_condensed)** — Y_cv and c_condensed
  carry fuel elements too (round 4) — with only the *gas-phase* species
  contributing to W̄ and pressure (§3.2's phase-transfer closure). The oxidation step, not a production-side
  temperature window, terminates the luminous zone (the §2.2 inception
  window stays background physics only).
- **Calibration constraint — with a pinned calibration state** (round 4:
  escaped soot depends on ventilation, residence time, resolution, and
  the oxidation closure, so a published net yield does not determine one
  universal gross coefficient; without a protocol y_form becomes hidden
  per-scene tuning). The reference case is **pinned concretely** (round
  5 — "the §3.8 pool-fire row" was not a defined case): a 0.30 m
  diameter heptane pool (published y_s = 0.037, χ_r, and mass-loss-rate
  data), quiescent §3.6 domain, D*/δx = 10, prescribed ṁ″_F from the
  published MLR, calibration functional = escaped-soot mass ÷ fuel
  consumed over a 30 s statistically-steady window after the §3.8
  discard. After that single fit, y_form must **predict both 0.30 m and
  0.60 m pools at D*/δx ∈ {6, 10, 14} unchanged**, with escaped yield within
  ±25 % of the measured value at every scale/resolution. Failure makes
  y_form a resolution-dependent closure coefficient and fails the §1
  no-scene-tuning bar; it is not repaired with a per-scene override.

  **y_cond gets an independently pinned mass-yield protocol** (round 5 —
  otherwise it is exactly the hidden smoke-opacity tune §1 forbids). The
  calibration dataset must name the wood-crib geometry, fuel moisture,
  MLR/HRR, sampling height, dilution ratio, sampling temperature, residence
  time, PM size cut, and analytical method. Required measurement is a paired
  particle-plus-gas organic yield over the same exhaust stream: thermo-optical
  OC/EC on the particle filter plus a downstream sorbent measurement of the
  gas condensable surrogate. For C₆H₁₀O₅ the carbon mass fraction is
  72/162 = 0.4444, so particle surrogate mass is **M_OC/0.4444**, not M_OC;
  gas and particle surrogate masses are summed before division by fuel
  consumed. One reference crib is fitted; an independently measured second
  scale and D*/δx ∈ {6, 10, 14} are cross-predicted unchanged within ±25 %.
  No y_cond preset may be frozen until a dataset satisfying every field is
  named and archived. The §3.8 known-mass smoke-transmittance gate separately
  validates renderer optics given mass; it is explicitly **not** evidence for
  simulated y_cond. Published net yields
  y_s anchor the reference: ~0.001 methanol, ~0.015
  wood volatiles, ~0.04 aliphatics (heptane/kerosene), **0.10–0.18 for the
  aromatic-heavy black-smoke class §2.2 describes** (e.g. toluene 0.178,
  polystyrene 0.164 — the same family as diesel/tires/PMMA). The in-flame
  hot-optics inventory comes from the gross field; the smoke comes from what
  escapes — both anchored to measurement.
- **Two transported aerosol inventories; the partition is DERIVED, never
  transferred** (r8 — the third external round showed r7's merged `smoke`
  channel was unrecoverable: cooled carbon and condensed organics, once
  summed, cannot be separated on reheating, and one merged k_m/n/ω/g
  contradicts §12's constituent-specific presets). The sim transports:
  - **c_carbon** — total soot-derived carbon aerosol (gross formation −
    oxidation; conserved otherwise), and
  - **c_condensed** — condensed organics, fed by the transported
    condensable-vapor scalar **Y_cv**. The condensable is a **defined
    pseudo-species** (round 4 — "condensables" without thermochemistry
    was unimplementable): a nominal formula and molecular weight
    (levoglucosan-class C₆H₁₀O₅, W ≈ 162 g/mol, as the wood-smoke
    surrogate; per-fuel override in metadata), referenced gas-vapor and
    condensed-aerosol thermochemistry for the ℋ_s bookkeeping, a heat of combustion Δh_cv for
    the primary withholding above, and a **latent heat exchanged with ℋ_s
    on every condensation/evaporation step**. Phase change follows a
    saturation rule with a mixing-limited rate: condensation where
    Y_cv exceeds the Clausius–Clapeyron-style saturation fraction at
    local T (the metadata pins one saturation-curve reference pair
    (T_sat,ref,p_sat,ref); the local dew point is the state-dependent solution
    Y_cv=Y_sat(T)), relaxation toward equilibrium on τ_mix, evaporation
    symmetric. **Pinned quantitatively and mass-conservatively** (round 6 — a dew point
    alone does not define Y_sat, and changing a gas mass fraction is not the
    transferred aerosol mass). With W_c the molecular weight of the
    non-condensable carrier mixture, compute

    > x_sat(T) = clamp[(p_sat,ref/p₀)
    >   exp{−(L_cv W_cv/R)(1/T − 1/T_sat,ref)}, 0, 1],
    > Y_sat(T) = x_sat W_cv / [x_sat W_cv + (1−x_sat)W_c].

    The clamp prevents an unphysical saturation partial pressure above p₀.
    Let r = 1−exp(−Δt/τ_mix) and choose the relaxed target
    Y* = Y_cv + r(Y_sat−Y_cv). The candidate transferred mass density is

    > condensation: δc = ρ_g (Y_cv−Y*)/(1−Y*),
    > evaporation:  δc = ρ_g (Y*−Y_cv)/(1−Y*), capped by c_condensed.

    Update gas total mass and vapor mass conservatively
    (ρ_g′, ρ_g′Y_cv′) = (ρ_g∓δc, ρ_gY_cv∓δc), with the upper signs for
    condensation, and update c_condensed by ±δc. If the evaporation cap
    binds, recompute Y_cv′ from those accepted conservative masses rather
    than retaining Y*. The accepted latent power is
    **q̇‴_lat = +L_cv δc/Δt for condensation and −L_cv δc/Δt for
    evaporation**; it feeds ℋ_s and the H used by S_div exactly once.
    The one local phase-source pass evaluates saturation at its input T;
    halving Δt must halve the Lie-splitting error. The
    condensable inventory is conserved end to end and never appears from
    nothing at the dew point.

  There is **no soot/smoke transfer operator at all**: the hot/cool split
  is the *derived* partition c_hot = φ(T)·c_carbon,
  c_cool = (1−φ)·c_carbon, with φ(T) = smoothstep over
  **T ∈ [700 K, 900 K]** — evaluated from the local temperature wherever
  needed (renderer-side, §4.3, since the renderer has T). Reheating is
  automatic and exact: carbon that warms moves continuously toward the
  hot-carbon optical model (the same particles; both regimes remain
  Kirchhoff emitters), while organics re-evaporate via Y_cv — with a
  **cool→reheat→cool conservation test** as the gate. The smooth φ also
  removes the hard-switch artifact the second round flagged (a 1.8×
  opacity step, 4.8 → 8.7 m²/g at 633 nm, plus an ω 0.1→0.6 step — a
  visible temperature shell); the effective optics are continuous by
  construction, and no mass is ever double-counted because each
  constituent has exactly one optics model at each temperature (§4.3).

  Carbon and condensed aerosol sensible thermochemistry has one separate
  versioned `aerosol_thermochemistry` owner. For each condensed phase it pins
  c_p,i(T), h_s,i(T), certified T domain, interpolation, provenance, and the
  **same T_ref convention** as `gas_thermochemistry`; optical hot/cool φ does
  not change carbon's thermodynamic phase or heat capacity. Predictive mode
  rejects missing/out-of-domain records. Each condensed-phase pair obeys the
  same continuity, h_s(T_ref)=0, dh_s/dT=c_p, and certified c_p>0 requirements
  above and participates in the common-domain, C_T lower-bound, bracket, RED,
  and round-trip proofs; neither record may pass preflight in isolation if the
  combined inverse is not certified. This closes the ℋ_s/C_T/S_div input
  at the allowed 1 % loading rather than leaving carbon c_p as an implementation
  constant. Ownership is non-overlapping: `gas_thermochemistry` owns Y_cv's
  h_s/c_p, `aerosol_thermochemistry` owns condensed-organic h_s/c_p, and the
  condensable record owns formula/W, Δh_cv/s_cv, the explicit L_cv reference
  jump and saturation law while referring to those two thermochemistry record
  IDs. It does not duplicate either sensible-enthalpy table.

Production, transport, oxidation, condensation/evaporation, radiation, and
projection follow §3.7's exact source-packet/coupled-expansion schedule, so escaped-soot mass
and every source dependency are reproducible.

**Known, accepted bias:** production is co-located with the reaction sheet
rather than distributed through the rich interior where inception physically
occurs — at campfire scale the advected soot still fills the flame body, but
the candle's interior yellow onset will sit slightly high. Accepted at this
fidelity; LLJ (or a flamelet-tabulated C₂H₂ variant) remains a Phase D
upgrade, gated on the calibrated-yield model demonstrably failing a §3.8
row.
**Fidelity-bar caveat (per §1): y_s is a per-fuel physical input, not a
per-scene artistic tune, but it is an empirical input nonetheless.**

### 3.5 Radiative loss model

Two-channel, per §2.4, **globally budgeted**. The r3–r5 max-blend
q̇‴_rad = max(χ_r·q̇‴, 4κ_PσΔT⁴) was numerically refuted by the external
review: at f_v = 10⁻⁶ and T = 1800 K the Planck-mean term is ≈ 5.6 MW/m³
against a χ_r·q̇‴ of ≈ 3 MW/m³ (χ_r = 0.3, q̇‴ = 10 MW/m³) — so max()
selects the *larger* term in exactly the sooty in-flame cells, the global
radiated fraction far exceeds the measured χ_r, and the optical depth
across a 0.5 m flame at that κ_P (τ ≈ 1.2) is outside any credible
optically-thin limit. Replacement — a **budget-partitioned escape-factor
model**:

> e(x) = 4σ [ κ_P(T; κ_λ(x)) T⁴
>                   − κ_P(T∞; κ_λ(x)) T∞⁴ ],
> (the optically-thin exchange with an isotropic black enclosure, using the
> same **local opacity spectrum** but a Planck mean at each radiation
> temperature), evaluated
> over the **total radiating support (all cells)**,
> **q̇‴_rad(x) = max( β(t), γ(t) ) · e(x)**, with
> β(t) = χ_r · Q̇_tot(t) / Σ_all e·V  and  γ(t) = clamp(1 − Q̇_tot/ε_Q, 0, 1),
> with ε_Q = 10⁻²·Q̇_ref (Q̇_ref the scene's nominal heat-release rate,
> a serialized §8 metadata value).

Normalizing over the **total** support (r7 correction — r6 normalized over
reacting cells only and exempted the plume, so the global sum was
χ_r·Q̇ + Σ_plume e·V, not the claimed budget; and r6's "plume ≤ 900 K"
rationale contradicted the document's own 1200–1700 K plume temperatures
and hot advected soot) makes the burning-regime budget **exactly**
χ_r·Q̇_tot with the plume holding its physical share of it, no seam and no
residual. This normalization is admissible only when the modeled opacity can
actually supply that loss: in the β-active burning branch, predictive runs
require ΣeV>0 and **0≤β≤1**. β>1 would cool faster than the local optically-
thin black-enclosure exchange and merely hide an inconsistent χ_r/opacity
record; ΣeV=0 with Q̇_tot>0 is the same failure. Either is a predictive hard
error (preview may log and continue for diagnosis), with an intentionally
under-opaque clean-fuel RED fixture. The γ term handles the post-fire regime continuously: as
Q̇_tot → 0, γ → 1 and the sink reverts to the unscaled optically-thin
e(x) (a dying fire's hot smoke keeps cooling; χ_r is meaningless without
heat release). Degenerate post-fire case pinned: Σe=0 and Q̇_tot=0 gives
q̇‴_rad≡0; it is not the burning-regime escape hatch prohibited above.
**Discrete closure — a split-first-order scheme, named as such** (third
external round: input-state normalization is exact only *before* the
temperature update, so the *accepted* sink deviates from the budget by a
first-order-in-Δt splitting error). In §3.7's provisional local source
trajectory, compute β and γ once from the post-phase, pre-radiation state and
freeze a=max(β,γ) and every species/aerosol inventory. The radiation map is
not an implementation-selected linearization. For each cell solve the scalar
backward-Euler equation

> ℋ_s(T⁺;q*) − ℋ_s(T*;q*) + Δt·a·e(T⁺;q*) = 0

by a bracketed fp64 root solve between T* and T∞ (ordered low/high). At every
trial T, recompute φ(T), the hot/cool split, both Planck means, and e from the
frozen q* inventories and frozen records; do not freeze κ_P, φ, or C_T at T*.
Before solving, interval-evaluate
**F′(T)=C_T(T;q*)+Δt·a·∂e(T;q*)/∂T** over that whole bracket, split at every
φ/table knot. Every optical/thermochemistry interpolation record must provide
the derivative bounds needed for this enclosure. The step is admissible only
if the lower bound is strictly positive; otherwise reduce Δt and rebuild the
source packet. This certified monotonicity plus the endpoint sign change proves
one root—a generic Brent/bisection call is not allowed to guess among multiple
roots. The root residual must be below the ℋ_s inversion tolerance. Define the
single accepted signed cooling rate

> q̇‴_rad = [ℋ_s(T*;q*)−ℋ_s(T⁺;q*)]/Δt.

That exact energy increment enters the frozen source packet, S_div, and
diagnostics once (the same one-rate discipline as q̇‴_step, §3.3). There is no
global β iteration; the splitting error is gated, not assumed:
**per-step accepted-vs-budget deviation ≤ 2 %, and halving Δt must halve
it** (first-order convergence check in the V-tier) — the gate applies
**only while β ≥ γ** (in the γ-dominated post-fire branch Q̇_tot → 0 while
plume cooling remains, so relative-to-budget deviation is intentionally
unbounded there; round 4). Harness
gates: integrated radiative fraction vs χ_r, and a per-column
optical-thickness monitor (κ_P·L across the flame) so the
optically-thin-shape assumption is measured, not presumed.
An intentionally steep/nonmonotone φ-dependent opacity fixture must fail the
F′ enclosure at a large Δt, pass after deterministic step reduction, and then
match a small-step reference; accepting whichever root a bracketed solver finds
is RED.
The aerosol part of the local absorption spectrum is exactly the renderer's
thermal spectrum (§4.1–§4.3). Product-gas bands are a **simulator-only cooling
closure** in this arc: they affect T but are neither exported nor rendered.
The predictive renderer claim is therefore scoped to the camera-visible
380–780 nm band, where the adopted gas table must bound CO₂/H₂O absorption
below the aerosol/chem validation tolerance. Rendering predictive thermal-IR
gas emission is a future channel-contract extension, not something silently
approximated from Z. The simulator uses

> κ_λ = κ_{a,hot-carbon} + κ_{a,cool-carbon}
>       + κ_{a,condensed} + κ_{a,gas},

where the first two terms already contain their φ and (1−φ) mass weights as
defined in §4.3.

Every absorptive term participates in both emission and ambient absorption in
the **simulator cooling integral**; φ blends optical parameterizations rather
than switching thermal emission on and off. Planck means at T and T∞ are
evaluated from this same simulator κ_λ. The renderer applies the same Kirchhoff
rule to the three exported aerosol terms, but not to unexported gas. Details:

- For the analytic hot-carbon component only,
  **κ_P,hot = 3.83·C₀·f_v,hot·T_r/C₂**, where T_r is the radiation
  temperature used by the mean, C₀≡6πE(m), and
  f_v,hot=φ·c_carbon/ρ_soot. The 3.83 coefficient is the exact mean of
  C₀f_v/λ under λ-independent E(m); the commonly quoted 3.72 is a
  Hubbard–Tien fit with wavelength-dependent optical constants. Its exchange
  contribution is f_v,hot(T⁵−T∞⁵), not f_v,hotT(T⁴−T∞⁴).
- Cool-carbon and condensed-organic absorption use the full wavelength-range
  constituent records required by §12 Q2. Visible-only k_m/n fits are
  insufficient for the cooling solve; predictive Phase C cannot enable those
  terms until their named IR extension is present.
- **κ_gas** comes from a frozen, versioned CO₂/H₂O table evaluated on the
  actual transported q_CO2 and q_H2O (or an exactly equivalent reconstruction
  from the full conservative gas state), never from Z alone. Its record pins
  wavelength and temperature ranges, p₀, composition basis, pressure/
  broadening convention, interpolation, band-overlap rule, source ID, and
  content hash. V5 includes an absolute homogeneous product-gas cooling slab
  against an independent line/band reference as well as two cells with the
  same Z but different remote-soot-burnout histories that must cool
  differently. No renderer can claim thermal-IR gas fidelity until those gas
  species and this exact record are added to §8 channels and to coefficients,
  majorants, emission CDF bounds, and Kirchhoff emission.
- For wavelength-dependent E(m), every constituent mean is evaluated
  numerically as ∫κ_λB_λ(T_r)dλ/∫B_λ(T_r)dλ from the same versioned dataset
  the renderer uses.
- **No two-way renderer coupling** (closed, §12): solving the RTE inside the
  sim is overkill at these scales; the budgeted optically-thin model is the
  appropriate standard of practice. The sink uses the exact bracketed
  backward-Euler scalar map above because the T⁵ soot term is stiff under
  explicit Euler (§3.7).

### 3.6 Boundary conditions and domain

The most notorious practical trap in fire LES; specified accordingly:

- **Fuel bed (bottom, source patch):** prescribe outward-into-domain fuel mass
  flux ṁ″_F, Z=1, the complete fuel-record gas composition and injection
  temperature/enthalpy, zero injected aerosol, and u_n=ṁ″_F/ρ_g; tangential
  velocity is zero. The projection pressure-correction boundary is Neumann and
  preserves that normal flux. The bed outside the patch is no-slip and
  **adiabatic**, with zero normal advective and molecular/SGS diffusive flux for
  every q_k, ρ_totZ, c_carbon, and c_condensed; ∂T/∂n=0. An isothermal-cold bed
  measurably changes flame standoff, so adiabatic is deliberate here.
- **Lateral and top faces: one pressure-open face contract.** Remove ambient
  hydrostatic pressure analytically and write p̃ relative to it, consistent
  with the (ρ_g−ρ∞)g body force. Every RK-stage projection solves the open-face
  class as an **active set**. Initialize from the prior accepted class (outflow
  at cold start), solve the pressure system, classify from the resulting
  outward projected velocity, and repeat until every face's class and sign
  agree. On inflow impose ambient total head
  **p̃_face+½ρ∞‖u_face‖²=0**; on outflow impose ambient static pressure
  **p̃_face=0**. These are the pressure operator's boundary equations, not log
  hints; the normal velocity is solved by the variable-coefficient projection,
  tangential velocity has zero normal gradient, and the pressure ghost/Robin
  stencil is the second-order face-centered discretization of the applicable
  equation. For the R0 and R1 stage projections and a fixed inflow active set,
  solve the coupled nonlinear
  Poisson+total-head boundary equations with damped Newton: each Newton step
  linearizes both p̃_face and the projected ‖u_face‖² term, solves the resulting
  variable-coefficient system, and line-searches on the combined interior
  divergence and boundary-head residual. Select the incoming physical branch
  modulo the deadband below: an inflow class is invalid only for converged
  u_n>u_bc,tol, and an outflow class only for u_n<−u_bc,tol; either returns to
  the active-set solve. Require both the normal projection residual and
  max_inflow|p̃+½ρ∞‖u‖²| ≤ p_bc,tol with
  p_bc,tol=ρ∞u_ref u_bc,tol. Lagging ‖u‖ from a prior iterate is forbidden.
  In the R0/R1 projections of §3.7, this p̃ symbol is the auxiliary stage
  multiplier π_r paired with the simultaneously solved u_r. The final accepted
  projection is different: π₂ is step-average pressure and its inflow boundary
  uses the Heun-consistent integrated head

  > π₂,face + (ρ∞/4)(I₀‖u₀,face‖²+I₁‖u₁,face‖²)=0,

  where I_r is exactly 1 when that face's converged Rr active set was inflow and
  0 when it was outflow (whose stage pressure was zero). This known trapezoidal
  average is imposed regardless of the separately diagnosed final u₂ sign, so
  the π₂ solve is linear rather than Newton-coupled; it becomes zero only when
  both stages were outflow. The converged u₂ sign records the endpoint class and
  initializes the next step, while the current Heun scalar boundary flux already
  used I₀/I₁ in F₀/F₁. Require the integrated-head residual below p_bc,tol.
  Pairing π₂ with endpoint ‖u₂‖², omitting either stage indicator, or
  claiming endpoint pressure is forbidden; a linear-ramp boundary fixture
  distinguishes endpoint, exact continuous average, Heun quadrature, and every
  (I₀,I₁) class-switch combination.
  For each R0/R1 active-set iteration use u_bc,tol=ε_absL, the already-defined
  projection velocity tolerance. R0 seeds from the preceding accepted endpoint
  class (all pressure-open faces seed outflow at the quiescent initial state);
  R1 seeds from converged R0. In either solve |u_n|≤u_bc,tol retains its
  seeded/current class, preventing a
  roundoff toggle; a repeated active-set state outside that band is a cycle and
  rejects/reduces Δt rather than choosing first/last wins. Only after this
  convergence is that stage's scalar upwinding selected. The endpoint u₂ class
  uses its sign with the same band, retaining I₁ inside the band, and seeds the
  next step; it does not reopen π₂. On inflow, advective ghost state is the complete frozen ambient
  record: Z=0, q_k=ρ∞Y_k,∞, ℋ_s=ℋ_s(T∞,Y∞), and zero carbon/condensed aerosol;
  diffusive/conductive flux uses that Dirichlet state. On outflow, every
  conservative scalar and tangential velocity has zero normal gradient and
  **all inward diffusive/conductive flux is zero**; advective flux uses the
  interior state. If projection reverses an outflow face **within the same
  stage**, the active-set re-solve immediately applies the ambient inflow
  above—fuel, aerosol, or hot enthalpy is never copied inward from a stale
  outflow ghost. A prescribed source patch is never reclassified. V1 plus an
  inflow→outflow scalar/enthalpy pulse, zero-start plume entrainment, oblique
  inflow, and a deliberately within-stage reversing-face budget test gate the
  pressure switch, ambient backflow, discrete total-head residual, incoming
  branch selection, and every advective/diffusive ledger.
- **Domain-size guidance:** lateral extent ≥ 2–3 D per side beyond the
  source; top ≥ 2× the expected flame height L_f for flame-zone rows — but
  **the plume-law row (§3.8 row 3) needs its own taller domain, top
  ≥ 4–5 L_f**, since a −5/3 fit with virtual origin over less than an octave
  of (z−z₀) is not a meaningful gate and the top ~0.5 D* is
  outflow-contaminated. Every validation row is run at two domain sizes to
  confirm boundary placement does not move the answer.
- **Symmetry breaking:** puffing onset on a symmetric grid with symmetric
  ICs is numerically delayed; a small **seeded, recorded** initial
  perturbation (part of the §3.8 determinism policy) breaks symmetry
  reproducibly.

### 3.7 Numerics

- **Grid:** uniform staggered MAC grid first; sparse/tiled (VDB-topology)
  when plumes outgrow dense memory. Precision: the conservative state
  (ρ_totZ, every gas q_k, both aerosol q_i, and ℋ_s) is stored and updated in
  **fp64**; pressure and global reductions are fp64. Velocity and derived
  diagnostic T may be fp32. §8's fp16/fp32 choices apply only to exported
  renderer grids, after conservation gates have run; quantization error is
  measured separately by the ingestion round trip.
- **Advection — all mass/energy scalars: conservative finite-volume FCT.**
  Advect ρ_totZ, every ρ_gY_k, c_carbon, c_condensed, and ℋ_s with the
  staggered-grid face mass flux. The low-order flux is donor-cell upwind; the
  high-order candidate is second-order MUSCL with the monotonized-central
  slope. **The reconstruction is performed in invariant coordinates, not
  component by component.** Let Q=(ρ_totZ,q_1,…,q_N) and form the constant
  matrix A for the homogeneous linear identities

  > E q − b(0)Σ_jq_j − [b(1)−b(0)]ρ_totZ = 0,

  which is equivalent to E q=ρ_totb(Z) because b(Z) is affine. The qualified
  schema carries a versioned `conservative_reconstruction_v1` record containing
  the exact state/constraint row order, declared rank r, dimensions, and every
  **binary64 byte of N_A**, plus SHA-256 over those canonical bytes. The same
  record ID is part of calibration/provenance. Runtime never recomputes QR/SVD,
  chooses column signs/order, or changes a rank tolerance: it verifies the
  recorded matrix against the exact A. Here “exact A” means its canonical
  binary64 entries interpreted as dyadic rationals, not a tolerance-ranked
  floating matrix. Qualification requires
  `cols(N_A)=state_dimension-r` and carries an exact rank factorization
  `A=UV`, with U m×r and V r×n encoded as canonical reduced arbitrary-precision
  rational numerator/positive-denominator pairs. Runtime verifies every entry of A=UV by exact
  integer arithmetic, proving rank(A)≤r. Recorded pivot-row/pivot-column
  indices additionally select an r×r minor B of A; its exact fraction-free
  elimination certificate has nonzero final pivots and proves rank(A)≥r.
  Thus rank(A)=r independently of the approximate N_A columns. The record also
  carries an exact rational nullspace basis B_A with `cols(B_A)=n-r`, encoded
  as canonical reduced numerator/positive-denominator pairs inside the same
  hashed record;
  exact arithmetic verifies A B_A=0 and a nonzero (n-r)×(n-r) pivot minor of
  B_A. Those facts prove `range(B_A)=ker(A)`. Let

  > P_A=B_A(B_AᵀB_A)⁻¹B_Aᵀ

  be evaluated as an exact rational matrix. Outward-rounded interval arithmetic
  evaluates the fp64 projector N_A N_Aᵀ and must prove
  `‖N_A N_Aᵀ-P_A‖∞≤1024ε_64`; otherwise the record is rejected. This projector
  certificate, not column count or a small A N_A residual, proves that the
  pinned numerical basis spans the intended nullspace to the declared fp64
  error. Runtime additionally verifies
  ‖AN_A‖_∞≤128ε_64‖A‖_∞ and
  ‖N_AᵀN_A−I‖_∞≤128ε_64, then uses those bytes or rejects. The residual bound
  is the explicitly accepted fp64 tangent error; it is not used as a rank
  proof. This pin is
  load-bearing because componentwise MC is not invariant to a rotation of the
  nullspace basis. With that recorded basis,
  encode each cell as ξ=N_AᵀQ, apply the MC reconstruction to ξ, and decode
  every face state as Q_f=N_Aξ_f. Thus both high- and low-order vector fluxes,
  and therefore their difference, satisfy every mass/element/Z tangent
  equality within the qualified fp64 forward-error envelope before limiting.
  Every stage checks that envelope and rejects the step on excess; tolerance is
  never evidence of a different rank. Projecting an independently reconstructed vector
  after the fact is forbidden because it changes its stencil and can violate
  face conservation. **ℋ_s is not part of this dimensionally-mass-valued
  nullspace basis:** it receives its own scalar MC MUSCL face reconstruction
  and donor-cell low-order flux. Its antidiffusive correction is assembled
  beside the constrained mass-block correction before limiting, and the one
  shared α_face below multiplies both. Leaving ℋ_s donor-cell or limiting its
  correction independently is forbidden. Their difference is the
  antidiffusive face flux. The limiter is a
  **multiconstraint Zalesak nodal-budget algorithm, not independent face-local
  clipping**: first compute the complete low-order update and all raw incident
  antidiffusive corrections A_cf. Equality residuals are already confined to
  the verified fp64 forward-error envelope by construction and are **not**
  represented as two zero-budget half-spaces.
  For every remaining inequality a_r·q≤b_r, only incident corrections with
  a_r·A_cf>0 consume the upper budget Q_cr=b_r−a_r·q_L; its lower bound is
  represented as a separate inequality and treated identically. Accumulate
  P_cr=Σ_f max(0,a_r·A_cf) and set R_cr=min(1,Q_cr/P_cr), with P_cr=0 defined
  as R_cr=1. Each face receives **one α_face shared by every
  component**, equal to the minimum applicable donor/receiver R over all
  inequalities whose projected correction is positive. Thus the aggregate
  of all incident corrections—not each face in isolation—fits every nodal
  budget. The constraint rows include the §3.3 element polytope, inventory
  nonnegativity, the explicit mixture-fraction bounds

  > −ρ_totZ ≤ 0,
  > ρ_totZ − Σ_j q_j ≤ 0,

  (hence 0≤Z≤1 wherever ρ_tot>0), and the linear sensible-energy bounds

  > ℋ_s ≥ Σ_j q_j h_{s,j}(T_amb),
  > ℋ_s ≤ Σ_j q_j h_{s,j}(T_ad).

  The low-order state must already be feasible; otherwise the step is rejected
  before antidiffusion.
  The same signed α_face correction is applied to both cells, so each species,
  every schema element, and total sensible energy are globally conservative to fp64 reduction
  tolerance. Independent per-component clipping or post-advection temperature
  clamping is forbidden: either destroys the coupled element/energy ledger.
  Semi-Lagrangian MacCormack remains a visually useful **debug-only** fallback
  and may not be used for validation or predictive grids.
- **Advection — momentum** (unstated in revision 2, flagged): a
  variable-density compatible mass/momentum flux on the staggered grid. For
  velocity component i, store momentum with the same face-density restriction
  ρ_g,i=I_ρ,iρ_g, where I_ρ,i is the arithmetic average of the two primal cells
  sharing that i-face. **Accept on the primal scalar grid first**:

  > Φ̂_g,p=Φ_g,p^L+α_p(Φ_g,p^H−Φ_g,p^L).

  Never average α values or limit an already interpolated dual candidate. Map
  this accepted primal advective flux and the single physical J_g defined below
  to the i-momentum control-volume faces with
  the boundary-aware arithmetic MAC operator I_i: on a dual face normal to i,
  average the two bracketing primal i-fluxes; on a dual face normal to j≠i,
  average the two bracketing primal j-fluxes across direction i. Ghost/boundary
  values are the same prescribed fluxes used by the primal scalar ledger. With
  D the primal cell divergence and D_i the i-momentum-CV divergence, these
  pinned operators obey exactly

  > D_i I_i(Φ̂)=I_ρ,i D(Φ̂).

  Then form K_i=I_i(Φ̂_g)(u_i,L+u_i,R)/2 and
  G_i=I_i(J_g)(u_i,L+u_i,R)/2 on each dual face. Consequently K_i(U)=U
  I_i(Φ̂_g) and the commuting identity preserves M_i−U I_ρ,iρ_g to roundoff
  even when spatially varying α is active. The arithmetic velocity factor also
  gives the compatible face kinetic-energy identity. The unlimited high-order
  candidate is the second-order central, kinetic-energy-conserving LES
  discretization; limiter blending adds only its explicit dissipation while
  preserving free stream. An independent central momentum flux, interpolation
  of α, or a density restriction different from I_ρ,i is forbidden.
  **Advection–reflection is dropped**: it is derived for constant-density
  incompressible flow (orthogonal L² projection onto a divergence-free
  space); neither assumption holds here, and no published variable-density
  validation exists. Semi-Lagrangian remains a debug fallback.
- **Conservation accounting:** the harness tracks global fuel, every element in E, aerosol,
  and total-sensible-energy budgets. On a closed or periodic domain, advection
  error must be at fp64 reduction tolerance; the ≤2 % flow-through tolerance
  applies only to the complete open-boundary/reacting balance after explicitly
  accounting for boundary fluxes, reaction heat, latent exchange, radiation,
  and resident chemical potential. It is not permission for scalar-advection
  drift.
- **Time stepping — source-packet freezing plus projected Heun.** The design
  decision, and the reason for it: chemistry, phase change, and radiation are
  *finite-step local maps* (exponential relaxation, saturation relaxation,
  backward-Euler radiation), while transport is a *conservative flux advance*.
  Evaluating a local map as if it were an RK derivative destroys its stiff
  limit — an analytic y′=−y/τ map at Δt/τ≫1 must yield y·e^{−Δt/τ}, but
  re-evaluating it at RK stages leaves ½y. So the local maps run **once** per
  step on a provisional scratch trajectory; their net effect is differenced
  into one conservative **source packet** ΔU_src (every Δq_k, Δc_i, Δℋ_s, with
  its own mass/element/chemical-potential/energy ledgers); and the frozen
  average S̄_src = ΔU_src/Δt is consumed *inside* the same coupled
  conservative advance that supplies expansion. Applying an accepted source
  update at fixed volume and projecting afterwards is a RED failure, as is
  re-running a finite-step map at an RK stage.

  The transport advance is **projected Heun** (predictor R0, corrector sample
  R1, commit) over the scalar/energy vector and conservative momentum, with:
  low/high face-flux candidates that **differ only in advection** (donor vs
  MC-MUSCL) sharing one centered physical nonadvective flux, so the FCT
  blending factor multiplies only the advective difference and diffusion is
  never limited away; **one shared limiter** over the combined update, so mass,
  elements, and momentum cannot be limited inconsistently; elemental closure
  enforced by **projection onto the constraint nullspace**, never by sequential
  species correction (§3.3 — structural in exact arithmetic, this projection
  realizes it under fp64); and a MAC projection using **the same staggered
  density as momentum storage** (not I_ρ(1/ρ), not a harmonic mean — a
  discontinuous-density fixture distinguishes them). The accepted multiplier
  π₂ approximates the **step-average** dynamic pressure over the step, not
  endpoint pressure; the predictor/corrector multipliers are auxiliary and
  discarded.

  Source freezing is explicitly **first-order** in the noncommuting
  local/transport split. That is accepted, not hidden: the Δt-halving gates
  must show first-order source-split convergence while transport-only
  manufactured cases retain Heun's second order.

  **Invariants an implementation must satisfy** (full fixtures in the solver
  spec): the stiff source-packet limit above; a constant-pressure heated
  control-volume case conserving packet plus face fluxes *and* closing the EOS;
  a multielement pure-diffusion case with nonzero aerosol loading requiring
  both C·J=0 and nonzero J_Z at every face (a ΣJ-only correction not tangent to
  the element matrix is RED, as is substituting ρ_gD for ρ_totD); and rejection
  of spatially uniform nonzero S_div on a periodic domain, which is
  unrealizable since ∫∇·u dV = 0 — it must be rejected, never used as a
  preservation test.

  **The exact operator schedule, Heun tableau, face-averaging and stencil
  rules, nullspace-basis qualification format with its rank certificates, and
  the complete RED fixture list are pinned in
  [FIRE_SMOKE_SOLVER_SPEC.md](FIRE_SMOKE_SOLVER_SPEC.md).**

  Step size: min of advective CFL, the buoyant-acceleration limit
  Δt ≲ √(2δx/g′₊) (g′₊ = max(g′,0) per §3.3 — the signed form goes non-real
  over cold dense fuel and is simply inactive where g′₊ = 0), and the explicit
  diffusion limit Δt ∝ δx²/ν. All molecular/SGS diffusion and conduction are
  explicit in this arc; subcycling or an IMEX replacement is a separately
  designed optimization, not an implementation choice hidden under "Heun."
  Radiative cooling alone uses §3.5's local backward-Euler map. Sim Δt is
  decoupled from frame write cadence (§8).
- **Turbulence:** LES with **Vreman as the default SGS model**
  (constant-C_s Smagorinsky is known over-dissipative in transitional
  buoyant plumes — exactly the puffing regime; it remains available as a
  fallback/ablation). **The candle is run as DNS: SGS off** — laminar and
  resolved; an SGS model would pollute it. (The reaction closure remains
  valid there by construction: τ_mix → τ_diff on molecular diffusivity,
  §3.3.)
- **Resolution targets.** LES guidance: **D*/δx ∈ [4, 16]** (FDS Validation
  Guide band), where D* = (Q̇/(ρ∞ c_p T∞ √g))^{2/5} is the *characteristic
  fire diameter* — not the physical base D. Each §3.8 scene states its Q̇,
  computes D*, and sets δx from it; puffing additionally requires a
  convergence check (frequency stable within 10 % under δx/2). The
  **candle** is a separate regime: flame standoff ∼1 mm and sheet thickness
  <1 mm demand δx ≈ 0.25 mm; a 4×4×8 cm domain at that δx is
  160×160×320 ≈ 8.2 M cells — tractable dense because the domain is tiny.
  At 2–4 cells across the sheet, the candle row grades **flame height and
  shape only** — not peak-T or in-sheet gradients.
- **Synthesized detail: excluded.** Wavelet-turbulence detail injection adds
  unsolved detail to fields the radiometry treats as physical; metadata
  flagging doesn't undo that. Added detail comes only from solver
  resolution. (Closed, §12.)

### 3.8 Verification and validation

**Verification tier — runs before any empirical validation** (added per the
external review, which correctly noted the empirical rows alone can pass a
wrong solver: puffing frequency, flame height, and a fitted −5/3 exponent
are all achievable with wrong temperature, species conservation, or
radiative loss):

- V1. Hydrostatic open-domain rest: unignited domain stays exactly
  quiescent (machine-level velocities) — exercises §3.6's BCs and the
  (ρ_g−ρ∞)g formulation.
- V2. Variable-density projection + the full divergence identity (§3.2),
  including a **two-gas isothermal mixing box** and a **reacting
  manufactured solution** — the S_div-from-discrete-updates gate. A nonzero
  variable-density manufactured momentum case simultaneously exercises
  conservative advection, dynamic pressure, buoyancy, viscous stress, and
  nonzero S_div with observed second-order velocity and **step-average dynamic-
  pressure** convergence; a
  separate linear-gradient fixture checks the pinned Vreman ν_sgs and its
  zero/rotation/laminar limits.
- V3. Conservative transport has three independent cases: (a) uniform-scalar
  preservation; (b) a periodic **pure-diffusion loading gradient** checking
  locally that E_gq_g+E_aq_a=ρ_totb(Z), not only global totals; and (c) a
  periodic, off-grid-Courant deforming velocity manufactured solution with
  local compression/expansion, nonuniform ρ_g and aerosol loading, and extrema
  chosen to activate the high-order clamp. Case (c) compares conservative
  densities against its analytic solution and is the negative control: the
  debug semi-Lagrangian MacCormack path must violate either the local affine
  invariant or the conservative solution. A constant-velocity translated blob
  remains a useful accuracy case but is **not** the negative control because a
  circulant interpolation operator can preserve its global sum. A fourth
  smooth periodic manufactured case remains away from every inequality bound,
  requires observed L1 order ≥1.8 over three refinements, asserts that a
  nonzero fraction of faces has α_face>0, and must be strictly more accurate
  than donor-cell for every mass field, ℋ_s, and the derived T. This prevents an all-α=0 implementation from passing the
  conservation checks while silently collapsing to first order. Every case
  checks local feasibility and global mass, gas species, aerosol inventories,
  every schema element (C/H/O/N for baseline air), ρ_totZ, and ℋ_s to fp64
  reduction tolerance. V2/V3 additionally run
  a periodic uniform-temperature mixture with unequal constituent c_p:
  diffusion must leave T uniform while conserving ℋ_s, repeated under two
  different T_ref choices to expose a missing J_h term.
- V4. **Algebraic source-packet ledger** (no fixed-volume EOS claim): atom,
  gas+aerosol mass, ℋ_s, and chemical potential balance to round-off at three
  scratch checkpoints—immediately after primary formation, after partial soot
  burnout, and after phase/radiation completion. At every checkpoint the
  invariant is

  > Q_released + M_carbon Δh_soot
  >   + (M_cv + M_condensed) Δh_cv = M_fuel,reacted Δh_c.

  The corresponding invariant is
  **M_O2,consumed + 2.667 M_carbon + s_cv(M_cv+M_condensed)
  = s_st M_fuel,reacted**. The test includes inert-aerosol heating/cooling
  at χ_load=1 %, homogeneous soot formation/burnout with unchanged Z,
  isothermal and adiabatic vapor↔condensate cycles (including the saturation
  cap), the simultaneous primary+burnout O₂-starvation fixture from §3.4, and
  gas↔aerosol phase-transfer ledgers. “Released heat = LHV” is required only
  after **all** withheld chemical inventories have been oxidized by a test-only
  completion step; the production model does not burn condensables. Separately,
  a constant-p₀ expanding control volume with open conservative faces consumes
  the same packet through §3.7: packet plus face-flux mass/element/energy
  balances close, the projected face velocity matches the analytic expansion,
  and the accepted state—not a fixed-volume scratch checkpoint—passes the EOS
  residual under refinement.
- V5. Radiative cooling of a single cell vs an **independent numerical
  wavelength integral** of 4π∫κ_λ[B_λ(T)−B_λ(T∞)]dλ, plus the analytic
  near-ambient derivative. For a hot-carbon-only φ=1 fixture with
  κ_λ∝1/λ the gate requires the f_v(T⁵−T∞⁵) law; merely integrating the implementation's own §3.5 ODE is
  not an independent test. A separate homogeneous CO₂/H₂O fixture requires
  absolute band-integrated cooling against an independent reference for the
  pinned gas-table state, plus the same-Z/different-product-history check.
- V6. **Timestep-refinement convergence** of heat-release and
  ignition-front histories at fixed grid (§3.3's discrete-realization and
  ignition-gate requirements), plus graph fixtures in which a CFT-failing
  vitiated barrier must block a pilot from a remote pocket and an isolated
  CFT-passing T_AIT cell must seed itself. Re-running after arbitrary prior
  reaction history with the same conservative state must give the identical
  eligibility graph.

**Empirical validation** — earned against measurements, with named datasets
and numeric tolerances recorded in the harness (correlation-only gates are
necessary but not sufficient; each row below also carries at least one
**absolute** gate — centerline temperature profile against the
McCaffrey/Heskestad plume datasets, HRR vs fuel consumption, integrated
radiative fraction vs the fuel's measured χ_r, and escaped soot vs y_s):

1. **Puffing frequency**: f ≈ 1.5/√D within published scatter (±20 %) across
   at least three base diameters; frequency measured by FFT of the centerline
   heat-release (or luminosity) signal over ≥30 cycles, stable within 10 %
   under δx/2 and under a domain-size increase (§3.6). (≥30 cycles at
   2–3 Hz plus the discard interval is ~15–20 s of simulated time —
   achievable at these grid sizes.)
2. **Flame height**: Heskestad correlation L_f = 0.235 Q̇^{2/5} − 1.02 D
   (Q̇ in kW, lengths in m), flame height measured by the standard
   **50 %-intermittency definition**, thresholded on the **luminosity
   proxy** (soot ε, matching the correlation's visible-flame anchoring); a
   q̇‴_gas threshold is the fallback for near-zero-soot fuels, with the
   luminous-tip vs heat-release-tip offset noted in the harness.
3. **Centerline plume law**: ΔT ∝ (z − z₀)^{−5/3} **above the flame tip**,
   fit with a virtual origin z₀, on the taller §3.6 plume domain. The same
   McCaffrey stations carry absolute centerline velocity (±10 %) and integrated
   entrainment mass flow (±15 %) gates; temperature alone cannot validate the
   momentum/SGS closure.
4. **Laminar candle**: steady flame height and shape vs. published candle
   measurements (DNS-resolved case; shape/height gate only, §3.7).
5. **Qualitative gates**: reaction sheet lies at Z ≈ Z_st; soot field sits
   inside/above the sheet with oxidation terminating it; no soot → no
   yellow; methanol case shows blue-only flame at a physical temperature
   (gas-band χ_r working, §3.5).

**Protocol requirements** (numbers in tables, not impressions, per
[UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md)): a
startup-transient discard interval (≥5 flow-through times) before
statistics; fixed, recorded RNG seeds including the §3.6 symmetry-breaking
perturbation; determinism stated precisely — **bitwise-identical fields at a
fixed thread count with ordered reductions** (unqualified cross-thread-count
bitwise determinism is not promised); harness scripts and tolerances checked
into the repo. The sim additionally exports **sim-rate centerline probe time
series** (T, q̇‴_step, u at stations) so the harness measures signals directly
rather than reconstructing them from frame-rate grids (§8).

**Dataset/tolerance pinning rule:** each empirical row names its exact
dataset, observation definition, and numeric tolerance in the harness
before it counts — row 1: Cetegen–Ahmed correlation, ±20 %; row 2:
Heskestad correlation with the 50 %-intermittency observable **defined
concretely** (third external round): side-view line-of-sight-integrated
soot emission over 550–700 nm, thresholded at 1 % of the time-mean peak;
row 3: McCaffrey centerline profiles (NBSIR 79-1910), ±10 % on the plume
region; row 4: **the Hamins–Bundy–Dillon candle characterization
(J. Fire Prot. Eng. 2005)**, steady flame height ±15 % and shape
qualitative.

**End-to-end radiometric gates** (second/third external rounds — component
tests alone can pass with wrong T–soot covariance or a wrong optical
mapping, and ratio-only gates pass under a uniform 100× scale error):
(a) a **calibrated flame spectral-radiance gate** — rendered two-color
pyrometry *ratios* of a validated flame column against published values,
**plus at least one absolute L_λ comparison in W·m⁻²·sr⁻¹·nm⁻¹** at a
stated wavelength and tolerance. That value is captured directly as raw
pre-exposure, pre-tone-map NM radiance by the spectral test harness; a
color-converted XYZ/RGB EXR is not an L_λ measurement. The dataset record also
pins geometry, view/solid-angle calibration, bandwidth, and uncertainty (§12
Q4). (b) a **known-mass smoke-transmittance
gate** — a column of known aerosol mass × path length rendered at 633 nm
must reproduce Beer–Lambert with the §4.3 constituent optics; being a
finite-spp stochastic full-pipeline render, the gate is stated as a
**95 % confidence interval containing the analytic value with CI
half-width ≤ 1 %**, not "round-off". Both run through the full
sim→grid→renderer pipeline, not on synthetic fields alone.

**Known-bias caveat:** at LES resolution, subgrid temperature fluctuations
are invisible to the resolved field, and soot radiance ∼T⁴⁺ means rendered
luminosity from resolved-mean T underpredicts reality (turbulence–radiation
interaction). Accepted for the arc and recorded (§12, TRI); the candle (DNS)
and the strict per-fuel radiometry keep the *rendering* side honest
independently.

---

## 4. Radiometry — mapping fields to light

The renderer-side physics contract. Everything here is wavelength-dependent,
and none of it passes through RGB or the JH uplift (§10.1).

### 4.1 Soot optical properties: κ_λ ∝ f_v/λ

Soot primaries (10–100 nm) are far smaller than visible wavelengths →
Rayleigh absorption regime:

> σ_a,hot(x, λ) = 6π E(m) · f_v,hot(x) / λ,  with **E(m) = −Im[(m²−1)/(m²+2)]**
> under the m = n − ik convention (the minus sign makes E(m) positive),
> and f_v,hot = φ(T)·10⁻³·c_carbon[g/m³] / ρ_soot[kg/m³] — the hot-optics
> fraction of the carbon inventory (§3.4/§4.3; the 10⁻³ converts the
> channel's g/m³ storage to SI — round 4 caught the missing factor). Unit
> gate: 1 g/m³ at φ = 1 must give f_v = 5.5556×10⁻⁷.

E(m) ≈ 0.26 for the classic Dalzell–Sarofim index m = 1.57 − 0.56i, and is
only weakly λ-dependent in the visible for mature soot (modern compilations
put E(m) in 0.24–0.33; dataset policy is §12's E(m) decision — and the
choice is shared
with the sim's hot-carbon component via C₀ = 6πE(m), §3.5, carried in the
§8 metadata).
Consequences:

- **Extinction is chromatic** (∝ 1/λ): blue is absorbed/emitted more than red.
- **Absorption dominates scattering** for soot. Single-scattering albedo in
  the visible: ∼0.05–0.15 for young in-flame soot (small aggregates), rising
  to ∼0.2–0.3 for mature overfire aggregates (Köylü–Faeth). Aggregate
  scattering (RDG-FA) is forward-peaked; Phase A approximates it with
  Henyey–Greenstein, Phase D upgrades (§12, presets decision).

### 4.2 Thermal emission: Planck × total local absorption, not blackbody

By Kirchhoff's law, **every absorptive aerosol constituent at T emits**. The
volumetric thermal-emission coefficient is

> ε_thermal(x,λ) = [σ_a,hot + σ_a,cool + σ_a,cond] B_λ(T(x)).

This renderer coefficient intentionally excludes the simulator-only CO₂/H₂O
cooling bands (§3.5); no gas state exists in the §8 renderer contract. The
predictive claim here is therefore the 380–780 nm aerosol+chem result. A future
thermal-IR claim must export gas products and add their absorption consistently
to transport, majorants, emission importance, and Kirchhoff emission.

In a hot soot-dominated cell this reduces to

> ε_hot = (6πE(m)f_v,hot/λ)B_λ(T).

(units W·sr⁻¹·m⁻³·nm⁻¹ — radiance per unit length). The 1/λ weighting
shifts the emitted spectrum measurably blue/white of a blackbody at the same
T — a real, photographable effect (it is why optical pyrometry of flames
needs the soot emissivity correction), and precisely the fidelity a spectral
renderer captures for free. **The Planck kernel is a new free function with
pinned quantity and units — the existing
[BlackBodyPainter.cpp](../src/Library/Painters/BlackBodyPainter.cpp)
evaluator is NOT reusable as-is** (external-review catch: its C1 = 2πhc²
at line 37 makes it spectral *exitance per metre of wavelength*; consumed
as the per-nm radiance this design needs, that is a π×10⁹ error). The
required function is per-nanometre spectral radiance:

> B_λ,nm(λ, T) = 10⁻⁹ · 2hc² / ( λ_m⁵ · expm1(hc/(λ_m k T)) )

with two numeric gates in `tests/`: the point anchor
B(500 nm, 1800 K) = 0.43477836 W·m⁻²·sr⁻¹·nm⁻¹, and the integral identity
π∫B_λ,nm dλ = σ_SB·T⁴. The painter becomes a caller (converting to its own
exitance-per-metre convention at its boundary); the fire path must *never*
reach Planck through `IPainter::GetColorNM` (§10.1).

Dynamic range: flame core ∼10⁴–10⁶ × ambient. HDR through the pipe is native
(EXR); exposure is the only artistic control this design admits. The
variance/firefly policy this implies is a renderer design item — §7.2 and
§10.5.

### 4.3 Cool smoke: chromatic scattering medium — with units and exponents

Cooled smoke becomes scattering-dominated, but its nonzero absorption still
emits thermally according to §4.2; it does not acquire a Kirchhoff exception.
At ordinary smoke temperatures that emission is negligible in the visible but
can matter to IR cooling. **The
renderer evaluates constituent optics from the two transported inventories
and the local temperature** (r8 — §3.4's derived partition; channels in
g/m³, sim-internal SI converted at export; "a scalar with no units" would
make smoke magnitude a per-scene tune, violating §1):

> hot carbon:  φ(T)·c_carbon → f_v,hot = φ·10⁻³·c_carbon[g/m³]/ρ_soot[kg/m³]
>              → §4.1 σ_a (1/λ),
>              σ_s = ω_hot·σ_a/(1−ω_hot), plus §4.2 emission;
> cool carbon: σ_e,cool=(1−φ)c_carbon k_m,carbon(633 nm/λ)^{n_carbon},
>              σ_a,cool=(1−ω_carbon)σ_e,cool,
>              σ_s,cool=ω_carbonσ_e,cool;
> condensed:   σ_e,cond=c_condensed k_m,cond(633 nm/λ)^{n_cond},
>              σ_a,cond=(1−ω_cond)σ_e,cond,
>              σ_s,cond=ω_condσ_e,cond;
> totals: σ_a/σ_s summed over constituents; the phase function is the
> **σ_s-weighted mixture of the constituent HG lobes**.

All concentrations in these equations use the §8 g/m³→SI conversion before
multiplication. Because φ(T) is smooth (§3.4), the hot↔cool carbon transition
(≈4.8 → 8.7 m²/g at 633 nm) is continuous in T, and each unit of mass has
exactly one absorption/scattering model at each temperature — no double count,
and §4.2 emits from the resulting **total σ_a**. Predictive constituent
constants come only from the frozen §12 versioned presets; until Q2 closes,
the listed numerical values are non-predictive regression fixtures. The formulas are dimensionally
consistent as written (g/m³ × m²/g = m⁻¹). The k_m anchor: Mulholland–
Croarkin's measured 8.7 m²/g at 633 nm is total post-flame smoke
extinction — the constituent presets (k_m,carbon, k_m,cond, each with its
albedo split) must be chosen to be consistent with it for wood-class
fuels. The spectral exponent n distinguishes the two physical regimes:

- **Fresh flaming (soot-dominated) smoke**: measured extinction Ångström
  exponents are **n ≈ 1–1.5** (consistent with Mulholland's k_m itself
  scaling ≈ λ⁻¹; the per-primary Rayleigh λ⁻⁴ *scattering* law is flattened
  by aggregate structure and never survives into the extinction exponent —
  revision 3 wrongly assigned n ≈ 2–4 here). Chromatic enough for the
  classic effect: **bluish against dark backgrounds** (scattered ambient),
  **yellow-brown in transmission**. Exponents of 2–4 occur only for
  ultrafine *non-absorbing* organic aerosol, a minority regime.
- **Aged / droplet-dominated smoke** (condensed organics, 0.1–1 µm — the
  dominant mass fraction in wood smoke): Mie regime, n ≈ 0–1, weakly
  chromatic, white-gray.

(Note the *absorption* 1/λ law of §4.1 is a different quantity — revision 1
conflated them. And the constituent split is what makes the accounting
sound: applying both the §4.1 soot optics and a merged k_m to the same
mass would double-count — soot's own mass absorption at 633 nm is
≈4.3 m²/g before scattering, so a merged treatment was already ~49 % over.)

Phase function: per-constituent HG (preset g values), mixed by σ_s as
above — and note the mixture weights are **wavelength-dependent**
(σ_s,j(λ) varies per constituent), which the current interface cannot
express: `IPhaseFunction` takes no wavelength and
`IMedium::GetPhaseFunction()` returns one fixed object (round 4,
verified). §7.1 step 4 therefore adds one pinned API: the spectral medium
constructs an immutable **`MakePhaseClosure(x, λ)`** at every collision. The
closure captures the local weights σ_s,j(x,λ) and exposes wavelength-bound
`Evaluate`, `Sample`, `Pdf`, and `GetMeanCosine`; callers cannot supply a
different λ after construction. Its mean cosine is

> g(x,λ) = Σ_j σ_s,j(x,λ)g_j / Σ_j σ_s,j(x,λ).

The Pel preview constructs `MakePhaseClosurePel(x, band_preset)`, where the
versioned preset is the renderer's three spectral-to-Pel response functions
R_c(λ), normalized per channel over 380–780 nm. It computes

> S_jc=∫R_c(λ)σ_s,j(x,λ)dλ,  S_c=Σ_jS_jc,
> p_c(ω)=Σ_j(S_jc/S_c)p_j(ω),
> q_Pel(ω)=Σ_c[S_c/Σ_dS_d]p_c(ω).

Zero-S_c channels contribute neither to q_Pel nor to that channel's scattered
throughput. `Evaluate` returns the Pel-valued (p_R,p_G,p_B); `Sample` draws the
single scalar proposal q_Pel and returns its scalar `PdfProposal`; continuation
multiplies each channel by p_c/q_Pel. `GetMeanCosine` is the corresponding
S_c-weighted mean of q_Pel, used only for preview guiding. This pins both the
projection and its throughput compensation; using luminance, a photopic curve,
or an unrecorded camera response is forbidden. Constituent lobes remain
stateless. The **same closure instance** serves continuation
sampling, direct-light adapters, and guiding for that collision; falling back
to `GetPhaseFunction()` anywhere after a fire-medium hit is an error. Gate: a
two-position, two-wavelength test with opposite constituent mixtures across
PT and RayCaster continuation, NEE adapters, and guiding on/off; an
HWSS-requested fire path must take the NM fallback before any legacy phase
lookup.
Tabulated Mie / RDG-FA in Phase D (§12, presets decision).

### 4.4 Chemiluminescence: band-resolved line/continuum source fields

One HRR field times one fixed composite SPD would force CH*, C₂*, and CO₂* to
have identical spatial profiles, contradicting §2.3 and making predictive
spatial color unrecoverable. The renderer therefore consumes §3.3's three
absolute radiant-source channels, not `reaction`:

> ε_chem(x,λ) = (1/4π) Σ_b q̇‴_b(x) S_b(λ),  b∈{CH,C2,CO2*},
> with **∫_{λa,b}^{λb,b} S_b(λ)dλ ≡ 1** and S_b in nm⁻¹.

Each q̇‴_b is already band-integrated W/m³, so isotropic division by 4π gives
§4.2's W·sr⁻¹·m⁻³ and the normalized S_b supplies per-nm units. Without both
conventions implementations can differ by 4π or 10⁹. The CH record contains
the 431/390 nm bands, C₂ the Swan bands, and CO₂* its broad continuum. If the
renderer clips any S_b to its visible support it **must not renormalize**:
out-of-band power is lost, not moved into visible wavelengths.

The simulator's state-dependent η_b closure (§3.3) is calibrated against
primary effective gas HRR q̇‴_gas. Values reported against total measured HRR
are converted at record-adoption time by Δh_c/Δh_c,eff; soot-burnout HRR never
enters. The chem record pins the η_b functions and certified state domains,
each SPD dataset/normalization interval/wavelength unit, the absolute
band-resolved spatial calibration data, and provenance. There is no renderer
scale knob. The integrated Σ_b q̇‴_b/q̇‴_gas remains near the ∼10⁻⁴ prior but is
not forced spatially constant. §12 Q1 freezes the actual per-fuel record before
predictive chem is enabled. Gates compare absolute pre-exposure radiance and
CH:C₂:CO₂* spatial profiles/ratios across at least a near-stoichiometric sheet,
a rich-side sheet, and a volumetric reaction-zone fixture. This makes the flame
base blue for the right reason and keeps zero-soot pilots renderable.

### 4.5 Heat shimmer (stretch): gradient-index ray bending

Hot gas has lower density → lower refractive index (Gladstone–Dale:
n − 1 ∝ ρ; ≈ 2.9×10⁻⁴ at STP falling to ∼0.6×10⁻⁴ at 1500 K). The visible
shimmer above a fire is refraction through this index field, honestly
renderable only with curved-ray (eikonal) marching through n(x) derived from
T(x). RISE currently has nothing for it (§6 G8). Phase D flex, explicitly
severable.

### 4.6 Embers, char glow, and sparks

Co-dominant with the gas flame in campfire visual identity. These are
*surface and particle* Planck emitters, not media: a char bed is geometry
with a temperature-driven emissive material (the extracted Planck function ×
char emissivity ≈ 0.85–0.95); sparks are lofted incandescent char particles
advected by the exported velocity field. Spark temperature follows a cooling
ODE **with a ventilation-modulated char-oxidation source term** — burning
char brightens in a gust rather than monotonically fading, and the source
term is what produces that signature look. No new transport machinery — a
Phase D content/tooling item (§7.4) reusing the §4.2 kernel.

---

## 5. RISE today — audited current state (2026-07-27)

Grounded in a code audit; file paths are the evidence trail. Summary: **the
volume transport substrate is strong; the fire-specific radiometry and data
plumbing are the gaps.**

### 5.1 What exists and is load-bearing

**Scoping correction (external review, r6 — the single biggest miss of the
internal rounds): there are TWO parallel volume-transport surfaces.** The
r1–r5 audit rows below describe
[RayCaster.cpp](../src/Library/Rendering/RayCaster.cpp)'s medium blocks —
but the PT rasterizers (and `PathTracingShaderOp`, and the auto route via
`AutoRasterizer.cpp:753`) actually run
[PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp),
which carries its *own* medium machinery: the same 0.5/0.5
DT-vs-equiangular mixture (`combinedPdf`, ~:301), volume scattering with
**path-guiding-aware direction pdfs** (`effectivePdf`, ~:1790 — RayCaster
stores raw `phasePdf` only), early termination on zero scattering weight
(~:1723 — no emission is scored, so a pure absorber currently contributes
nothing on this path), and a **hero-driven HWSS branch**
(`SampleDistanceWithEquiangularMIS_NM`, ~:4123) whose pure-DT companion
weights divide by each companion's own σ_t·Tr instead of the hero proposal
(~:4158) — **biased for chromatic media**. Every Phase A emission work
item must land on BOTH surfaces (§7.1), the estimator ordering must score
emission before any σ_s-based early-out, and **HWSS is hard-disabled for
fire media** (per-λ fallback) until the companion-proposal fix lands
(Phase D). Gap G11.

| capability | where | notes |
|---|---|---|
| Heterogeneous medium w/ voxel density | [Volume.h](../src/Library/Volume/Volume.h), [HeterogeneousMedium.cpp](../src/Library/Materials/HeterogeneousMedium.cpp) | dense 3D array from raw binary Z-slice files (printf pattern's slot = Z index, Volume.h:130); NN/trilinear/tricubic accessors ([IVolumeAccessor.h](../src/Library/Interfaces/IVolumeAccessor.h)) |
| Painter-baked volumes | `VolumeAccessor_Painter.h`, `painter_heterogeneous_medium` chunk | any 3D painter baked to a grid at author time — the Phase-A procedural-flame vehicle |
| Distance sampling | delta/Woodcock tracking (heterogeneous), analytic (homogeneous) | `RayCaster.cpp` medium blocks: ~747–1259 (RGB), ~1461–1835 (NM) |
| Transmittance | ratio tracking (Novák 2014) | |
| Majorant machinery | [MajorantGrid.h](../src/Library/Utilities/MajorantGrid.h) + Amanatides–Woo DDA; [NullScatteringTracker.h](../src/Library/Utilities/NullScatteringTracker.h) (Miller–Georgiev–Jarosz 2019 null-scattering PDFs) | the substrate for the Phase A emission estimator and Phase D spectral tracking; grid built once in the medium constructors (HeterogeneousMedium.cpp:55–62, 94–101); cell resolution `clamp(ceil(dim/8), 4, 32)` per axis — note the 32-cap, which drives §7.2.3's two-level CDF |
| Distance-sampling MIS | 50/50 **balance** one-sample MIS, delta-tracking vs equiangular ([EquiangularSampler.h](../src/Library/Utilities/EquiangularSampler.h), Kulla–Fajardo) | gated on `GetPositionalLightCount() > 0` (RayCaster.cpp:807, 1486); MIS denominators are *deterministic sums over all positional lights* via `EvalDeterministicOpticalDepth` (HeterogeneousMedium.cpp:729 — knot-aligned DDA + 5-pt Gauss–Legendre, exact for all three accessors *today*; §7.1 step 1 upgrades fire media to 7-point with φ-root panel splitting). Phase B extends exactly this structure (§7.2.4). In-tree comment drift note: the comment at RayCaster.cpp ~903–906 says pdf_dt uses majorant transmittance; the code computes deterministic real transmittance — fix the comment when Phase B lands |
| Medium stack / nesting | [MediumTracking.h](../src/Library/Utilities/MediumTracking.h) via IORStack; shadow-ray boundary walk in `LightSampler.cpp` | |
| Volume emission (limited) | `MediumCoefficients::emission`, closed-form per-segment integral `Le·(1−Tr)·t/τ` with Le evaluated at a **single point** per segment (scatter point RayCaster.cpp:~1014, midpoint :~1238/:~1821; blocks 1181–1259 RGB, 1792–1833 NM) | **constant per medium** (G1) and **the estimator itself dies with spatially varying Le** (G9) — see §6 |
| Spectral media (homogeneous only) | `homogeneous_medium` `absorption_spectral`/`scattering_spectral` | heterogeneous collapses to luminance (HeterogeneousMedium.cpp:228–240) — G4 |
| Planck evaluation | [BlackBodyPainter.cpp](../src/Library/Painters/BlackBodyPainter.cpp) (`blackbody_painter`) | surface painter only, and **its C1 = 2πhc² (line 37) makes it spectral exitance per metre — NOT reusable as the per-nm radiance kernel**; §4.2 defines the new pinned free function and its gates |
| Phase functions | isotropic, HG (`IsotropicPhaseFunction.h`, `HenyeyGreensteinPhaseFunction.*`) | a Rayleigh phase function exists but is buried in `BioSpecSkinSPFHelpers.cpp:79–80`, not reusable |
| Animation | keyframed parameters ([src/Library/Animation/](../src/Library/Animation/)) + HDR movie pipeline | no time-varying *volume data*; media are not `IKeyframable` — G6 |
| VDB ingestion (bridge only) | `rise_blender_bridge.cpp:527–650` | transcodes a VDB float grid to raw slices: normalizes by grid max (compensated downstream by folding `density_scale` into σ_a/σ_s, ~1418–1441) **and quantizes to 8-bit** — the quantization, not the normalization, is what's fatal for fire channels (Planck is exponential in T); §8 requires a float path |
| Shadow/NEE visibility split | `LightSampler.cpp:~300–310` (medium boundary walk), `:~1662` (surface shadow test) | the medium walk computes *attenuation only* and **deliberately ignores non-medium objects**; geometric occlusion is a separate shadow test. Volume NEE (§7.2.1) must run both — the walk alone would see the flame through walls. **The walk is also biased as inherited** (third external round, documented in its own header ~:118–132): nesting beyond MAX_DEPTH=4 is silently dropped, MAX_WALK_STEPS=16 truncates with stack-state extrapolation, Tr < 10⁻⁶ hard-zeros, and an interrupted global heterogeneous medium is evaluated as one interval from the ray origin (wrong spatial field); `HeterogeneousMedium` ratio tracking additionally returns a partial result at its step cap (~:537). Phase B replaces it (§7.2.1) |
| Medium-stack semantics | [MediumTracking.h](../src/Library/Utilities/MediumTracking.h):~38 | **innermost-exclusive** (Cycles-convention): only the IOR-stack top's interior medium is active — an outer medium is inactive inside a nested object, which constrains the §7.2.5 emission-CDF support |
| Majorant interpolation-overshoot machinery | [MajorantGrid.cpp](../src/Library/Utilities/MajorantGrid.cpp):~98–172 | dilation passes for stencil overlap + the Catmull–Rom overshoot factor 756/512 ≈ 1.4766 — the machinery §8 cites as evidence when *excluding* Catmull–Rom for fire channels (trilinear-only rule); note `HeterogeneousMedium::ClipDistanceToBounds` (~:688) clips to the *base* AABB, which motion blur must widen |
| Per-path time carrier | `RuntimeContext.h`, `Ray.h` | **absent** — neither carries a sampled path time; the existing temporal path mutates the Animator per sample, which §10.3 forbids for grids. §8's motion blur requires a new read-only per-path time threaded through every lookup |
| Physical scene scale | [src/Library/Parsers/README.md](../src/Library/Parsers/README.md):~177 (`scene_unit`) | currently governs only the camera mm conversion; the same doc names it the future home for physical-quantity scaling — §8 makes media the first consumer |

### 5.2 Integrator substrate decision

**All fire/smoke work targets the PT path (and its equiangular MIS)
exclusively**, on σ²·T grounds from the integrator matrix
([UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)): PT's
per-sample cost advantage is largest on heavy volumes (37×), and VCM shows
−63…+76 % volume luminance bias. (Correction from revision 1: BDPT's medium
support is *not* "global-medium-only" — `BDPTIntegrator.cpp` implements full
per-object nested-media connection transmittance
(`EvalConnectionTransmittanceImpl`, ~1108–1260); the stale comment at ~line
903 that says otherwise should be fixed in-tree. The PT-only decision rests
on cost, not capability.)

**Auto-rasterizer routing is a real work item, not a freebie**:
`AutoRasterizer.cpp` has no media detection, and
[AUTO_RASTERIZER_DESIGN.md](AUTO_RASTERIZER_DESIGN.md):239 lists
participating media as "reachable, not yet routed on." Volumes land on PT
only by default; a fire scene whose surfaces trip the caustic/glossy probes
could route to VCM (documented volume bias) or BDPT — and once
emissive-volume NEE is PT-only, a misroute silently loses the feature. Gap
G10; Phase A adds the Tier-1 rule: presence of a heterogeneous or emissive
medium forces the PT route.

## 6. Gap analysis

In dependency order. G1/G2/G9/G11/G4 block a fire rendering *at all* being
physical; G3 blocks fire lighting the scene; G5–G6 block rendering *sim
output*; G10 protects the routing; G7–G8 are quality/stretch.

- **G1 — Emission is a per-medium constant.** `HeterogeneousMedium.cpp:224`
  sets `c.emission = m_emission;` — spatially uniform. A flame is *nothing
  but* spatially-varying emission.
- **G2 — Single-channel volumes.** One scalar per medium. Fire needs
  (c_carbon, T) at minimum, plus c_condensed, q̇‴_gas, optionally **u** (§3.3).
- **G9 — The emission *estimator* must change, not just its inputs.** The
  closed form `Le·(1−Tr)·t/τ` is exact only for σ_t *and Le* constant on a
  segment, and the code evaluates Le at a single point. With
  Le = σ_a,total(carbon,condensed,T)·B_λ(T) varying by orders of magnitude over millimetres, a
  midpoint Planck lookup at 1500 K instead of 300 K is off by orders of
  magnitude (∼600× in total power, far more in the visible band) — the
  analytic pickup is structurally dead for fire. Replacement (Phase A): **score
  ε_thermal·T_det/p at the sampled scatter distance**, p the distance pdf
  actually in use — reducing to the classic (σ_a/σ_t)·B_λ real-collision
  estimator under pure delta tracking (§7.1 step 2 for the rule and the
  equiangular-mixture case) — which also produces the per-point pdf Phase
  B's MIS requires (§7.2). *Audit note:* code reading supports a
  suspicion that the existing pickup is biased **today**: the scatter branch
  adds the analytic [0,t_m] emission integral per scatter sample and the
  no-scatter branch adds the full-segment integral, without
  branch-probability compensation — expected pickup ≈ ∫Tr²·Le, not ∫Tr·Le.
  Phase A step 0 settles this with a brute-force gate before anything is
  built on top; the collision-based replacement supersedes the suspect form
  either way.
- **G3 — Emissive volumes are invisible to NEE.** No volume light type in
  [src/Library/Lights/](../src/Library/Lights/), no LightBVH entry, no
  emissive-voxel importance structure. Volume emission is collected only by
  rays that happen to march through it → a bright flame is noisy and fails
  to efficiently illuminate its own smoke — and *fire lighting its smoke
  from inside* is the identity-defining look of the phenomenon. Design in
  §7.2.
- **G4 — Heterogeneous media are achromatic in the spectral path.**
  `GetCoefficientsNM` collapses to luminance. Kills the 1/λ soot spectrum
  (§4.1) and chromatic smoke (§4.3).
- **G5 — HWSS + media is a stub.** `RayCaster::CastRayHWSS`
  (RayCaster.cpp:2465, fallback loop ~2489–2504) detects a medium and falls
  back to N independent per-λ `CastRayNM` traces. Correct but forfeits
  hero-wavelength sharing. **Phase A explicitly depends on this fallback
  remaining in place** (it is what makes per-λ chromatic media correct
  without spectral tracking); the real fix is Phase D spectral tracking, and
  landing it must not break the Phase A path (§7.4).
- **G6 — No time axis for volumes.** `volume_pattern`'s printf slot indexes
  Z-slices, not frames; media are not `IKeyframable`; majorant grid built
  once at load. Blocks rendering any sim sequence.
- **G10 — Auto-rasterizer volume routing** (§5.2): no media rule exists;
  fire scenes can misroute to integrators with documented volume bias/cost.
- **G11 — Second transport surface + HWSS companion bias** (§5.1 scoping
  correction): the PT rasterizers run `PathTracingIntegrator`, not
  RayCaster — it terminates on zero scattering weight before any emission
  could score (pure absorbers vanish), and its hero-driven HWSS pure-DT
  branch divides companion contributions by the companions' own σ_t·Tr
  rather than the hero proposal (biased for chromatic media). Phase A must
  land the emission estimator on both surfaces with
  score-before-early-out ordering, add pure-absorber tests (Pel/NM/HWSS ×
  both entry routes), and hard-disable HWSS for fire media until the
  companion fix (Phase D).
- **G7 — Phase-function menu** stops at isotropic/HG. Draine /
  tabulated-Mie / RDG-FA are Phase D.
- **G8 — No varying-IOR ray bending** (heat shimmer, §4.5). IOR is
  piecewise-constant across surface boundaries; `MediumCoefficients` has no
  IOR field. Stretch.

---

## 7. Phased plan

Each phase lands independently and is subject to the standard
definition-of-done loop
([skills/implementation-review-loop.md](skills/implementation-review-loop.md)).

### 7.0 Phase gating

Two different things were being conflated by earlier revisions of this
section: **what engineering must exist before a phase's work is sound**, and
**what measured data must exist before its output may be called predictive.**
They are separated here. **A phase gate is that phase's own exit criteria** —
the deliverables that must exist and be green before the phase is done and the
next may rely on it, not a barrier to starting. Every one can be satisfied by
engineering. A predictive-label gate depends on
laboratory data this project does not produce, and blocks *the claim*, not the
work — everything can be built, tested, and shipped as explicitly-labelled
preview while those datasets are open.

**Already landed:** the trilinear-accessor repair (commit `2fba2b48` — the
reversed z-blend and negative-coordinate fractions), which was a Phase-A
prerequisite and a pre-existing bug affecting every heterogeneous volume.

#### Phase A gates (all engineering)

1. **Radiometric kernels pinned** — the per-nm Planck free function with its
   two numeric anchors (§4.2), the E(m) dataset record, and the g/m³→SI
   conversion applied at *every* f_v site (§4.1, §4.3, §8).
2. **Multi-channel media with derived φ(T) optics** — the constituent
   inventories, the shared extinction lattice including temperature, the
   **trilinear-only rule for physical channels** (§8 — tricubic undershoot
   yields negative extinction, and clamping would break the quadrature
   exactness step 2 relies on), the φ-aware quadrature with panel splitting
   at the φ-thresholds, and the φ-sup extinction-majorant bound (§3.4, §4.3,
   §7.1 steps 1/4, §7.2.3). The **authoring surface is
   `multichannel_heterogeneous_medium`** (§9) — statically authored,
   preview-only; `fire_medium` and its manifest are Phase C.
3. **Emission estimator on both transport surfaces** — the σ_a·B_λ·T_det/p
   rule with correct event ordering in `PathTracingIntegrator` *and*
   `RayCaster`, the no-scatter deletion, HWSS hard-disabled for fire media,
   and the no-silent-cap distance-sampler repair (§7.1 step 2, G11).
4. **Chem transport** — the band-resolved source fields and the support-safe
   randomized line estimator (§4.4, §7.1 step 3).
5. **Auto-rasterizer media routing** (G10) and the scene/metadata contract for
   fire media (§8, §9).
6. **Gates green** — the isothermal-slab absolute radiance test, the
   metre-vs-centimetre scene-unit invariance test, and the Phase-A step-0
   emission-bias characterization (§7.1).

#### Phase B gates (all engineering)

1. The replacement segment-wise shadow walk with no silent caps, including the
   wrong-origin and step-cap-continuation cases (§7.2.1).
2. Volume NEE's estimator, the labeled multi-medium densities, and the
   two-level emission CDF (§7.2.1, §7.2.3, §7.2.5).
3. The MIS partition as specified: independent pivot/endpoint draws, the
   two-bit weight-1 state, boundary survival and the no-event compensation,
   direction-independent lobe preselection, and the RIS/guiding/SMS disables
   at competing vertices (§7.2.2, §7.2.4).
4. **The fire medium's row in the continuation-closure table** — its
   σ_s-weighted constituent HG-mixture closure (§7.2.2). Without this the
   feature is default-denied inside the only medium it exists to light.
5. Per-frame invalidation and rebuild of the emission structures (§7.2.6),
   scheduled in the same between-renders step as the majorant rebuild.
6. The §7.2.7 equality gates and configuration matrix.

#### Phase C gates (all engineering)

1. The §3.2 phase-transfer/EOS closure and discrete S_div derivation; §3.3's
   two-rate scheme, flood-fill ignition, and exponential updates; §3.4's
   withheld-stream coefficients and conserved inventories; §3.5's budgeted
   escape-factor sink.
2. The §3.7 numerics as decided, conforming to
   [FIRE_SMOKE_SOLVER_SPEC.md](FIRE_SMOKE_SOLVER_SPEC.md).
3. The §3.8 verification tier (V1–V6) green — these are executable and
   dataset-independent.
4. §8's *sequence* contract: the manifest, `fire_medium`, time mapping,
   residency, and velocity halo (the channel/precision and interpolation
   rules land with Phase A gate 2); plus the output-provenance sidecar and
   its digest verifier plumbed through `FrameStore`, file encoders, AOVs, and
   animation/MOV finalization — §8 makes a predictive primary fail if its
   sidecar cannot be written, so this is a required deliverable, not polish.
5. The freeze/prepared-input seam this arc depends on
   ([RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md)) —
   grid/majorant/CDF swaps strictly between renders, mid-render mutation
   detected.

#### Phase A execution order — the minimal end-to-end slice

The gates above say *what must be true*; this says *what to build first*. The
goal of the first increment is *a flame-shaped thing emitting physically
correct spectral radiance*, reached in the fewest steps that each end in a
green numeric test:

1. **Planck free function + its two gates** (§4.2). No dependencies; the
   B(500 nm, 1800 K) anchor and the π∫B dλ = σT⁴ identity are checkable in
   isolation. Extract from `BlackBodyPainter`, do not reuse it.
2. **Step-0 emission-bias characterization** (§7.1 step 0) on the *existing*
   single-channel medium. Settles whether today's pickup is biased before
   anything is built on it.
3. **Two-channel medium (carbon + temperature) via
   `multichannel_heterogeneous_medium`**, painter-baked, trilinear, no
   manifest, no chem, no condensed phase. This is the smallest thing that can
   carry a flame field.
4. **Collision-based emission on `RayCaster`** (§7.1 step 2), gated on the
   isothermal-slab absolute target L_λ = B_λ(T)·(1−e^{−τ_λ}) and the
   scene-unit invariance test, in Pel and NM. **Scope this increment to a
   grey (wavelength-independent σ) medium at constant φ**: with τ_λ constant
   the slab target is still exact and still checks the estimator against the
   pinned Planck kernel, which is what this step exists to prove. What it
   does *not* yet produce is a physically correct flame — soot extinction
   goes as 1/λ (§4.1), so grey σ gets τ(500 nm)/τ(700 nm) = 1 where the
   physics requires 1.4.
5. **Chromatic per-λ coefficients on the NM path** (§7.1 step 4's minimal
   core: λ-dependent σ_a/σ_s, per-λ majorant, tracking, and transmittance —
   `GetCoefficientsNM` currently collapses to luminance,
   `HeterogeneousMedium.cpp:228`). **Only after this does a procedural candle
   render with physically correct spectral radiance**, and only then does the
   slab gate exercise chromatic extinction rather than a grey special case.
6. **The whole estimator on `PathTracingIntegrator`** (G11), with the
   pure-absorber tests through both entry routes. Now the PT rasterizers —
   what users actually run — produce the same numbers.

The rest of Phase A (the condensed constituent, chem bands, the φ-aware
quadrature upgrade with its φ-root panel splitting, the auto-rasterizer rule)
extends a pipeline that already renders and already has numeric gates. Steps
1–6 are the critical path; the rest can be parallelized or deferred within
the phase. Note the ordering constraint that forced step 5 up: §7.1 step 2's
own gate list requires the NM path, so "defer chromatic" cannot mean "defer
past the estimator gates" — it means the φ-aware quadrature and the
constituent split, not per-λ coefficients.

#### Predictive-label gates (data, not code)

Output may be **labelled predictive** only when, in addition to the phase
gates above:

- **Optics** — §12 Q2's separate constituent datasets are frozen as preset v1
  and the total-mixture 8.7 m²/g anchor passes. The values currently in §12
  are explicitly non-predictive regression fixtures.
- **Chemiluminescence** — §12 Q1's absolutely-calibrated per-fuel record
  exists for the declared fuel. Otherwise `chem_model=none` is predictive
  **only** when a pinned measurement reports a 95 %-confidence upper bound
  below **both** 1 % of that fuel/case's measured 380–780 nm radiant power
  **and** the absolute radiance gate's uncertainty at every gated wavelength;
  absent that record, a missing calibrated chem record is a predictive hard
  error. Synthetic η_b/S_b records are estimator tests and preview assets
  only.
- **Soot/condensable yields** — §3.4's calibration and cross-prediction gates
  pass for the fuel in question.

Until then the same builds run and the same tests pass; output carries
`preview` with the specific reason code, and §8's manifest records why. **No
engineering task waits on this list.**

### 7.1 Phase A — "a fire renders at all" (renderer-only; no sim)

0. **Baseline emission-bias gate** (G9 audit note): brute-force test of the
   existing emissive-medium path against the closed form on a homogeneous
   emissive slab with σ_s > 0, RGB and NM. Confirms or refutes the suspected
   transmittance double-count *before* building on the path — and also
   characterizes a second pre-existing inconsistency the branch deletion in
   step 2 eliminates: the equiangular zero-contribution early-out
   (RayCaster.cpp:993) returns without executing the no-scatter emission
   block at all. Lands in `tests/` regardless of outcome (it is the
   regression gate for step 2). **Step 0 also fixes the trilinear
   accessor itself** (round 4, verified in-tree:
   `VolumeAccessor_TRI::GetValue` uses `modf`, which yields negative
   fractions for the negative centered coordinates heterogeneous volumes
   use, and its z blend is **reversed** — `front·wt + back·(1−wt)`
   returns the high-z plane at w = 0 — producing extrapolation, possible
   negative values, and knot discontinuities in the very accessor §8's
   trilinear-only rule mandates). Fix: floor-based index/fraction
   extraction with fractions in [0,1], blend `front·(1−w) + back·w`;
   gates: signed-coordinate ramps, knot continuity, voxel reproduction,
   convex-hull bounds. **Status: LANDED 2026-07-28 as commit
   `2fba2b48`** (in master) — the accessor fix and all four gates above
   are in tree (tests 6–11 in `tests/PainterVolumeAccessorTest.cpp`,
   covering TRI, NNB, and TriCubic); step 0 retains only the
   verification that the fire path builds on the repaired accessor.
1. **Multi-channel heterogeneous media** (G2), authored through the new
   **`multichannel_heterogeneous_medium`** chunk (§9 — statically sourced,
   preview-only; `fire_medium` and its manifest are Phase C): named scalar
   channels per
   medium (`carbon`, `temperature`, `condensed`, diagnostic `reaction`, and
   `chem_CH`/`chem_C2`/`chem_CO2` when chem is enabled), each a `Volume<>`
   grid + accessor sharing bbox/transform — and **all extinction-relevant
   channels — carbon, condensed, AND temperature — share one grid
   resolution** (round 4: φ(T) makes T extinction-relevant, and the
   earlier lattice rule omitted it), so the DDA knot lattice stays
   single. **Quadrature degree, recomputed for φ(T)** (round 4):
   trilinear T along a ray is degree 3, the cubic smoothstep composes to
   degree 9, and multiplying by degree-3 carbon gives a **degree-12**
   integrand — beyond 5-point Gauss–Legendre's degree-9 exactness — with
   piecewise breakpoints where T crosses the 700/900 K clamps. Committed
   scheme: DDA panels **split at the φ-threshold crossing roots** (roots
   of the per-cell cubic T(t) − 700/900, closed-form solvable) and
   `EvalDeterministicOpticalDepth` upgrades to **7-point Gauss–Legendre**
   (exact to degree 13) — preserving the deterministic-MIS-denominator
   and pure-DT-reduction guarantees, with majorant and `EvalDistancePdf`
   tests updated to cover φ-transition-straddling cells;
   `EvalDistancePdf`'s density lookup generalizes to the summed σ_t of
   those channels. **Scene-unit propagation lands here too**
   (external-review catch): all §8 quantities are SI, but RISE positions
   are in scene units (`scene_unit` = s metres, today camera-only) — σ, ε,
   and u must be converted at medium construction (σ_scene = s·σ_SI,
   ε_scene = s·ε_SI, u_scene = u_SI/s), with the scale read from job/scene
   state, and a **metre-vs-centimetre same-slab invariance test** (equal
   optical depth and radiance) as the gate. Mechanical surface (the honest
   checklist): `Volume.h`/accessors, `HeterogeneousMedium`,
   `IJob::AddHeterogeneousMedium` (IJob.h:1516) / `Job.cpp`, `RISE_API.h`
   factory signatures, chunk parser + registry, the five build projects
   (CLAUDE.md checklist), tests. **Scene-editor surfaces, concretely**: an
   `EntityTemplates.cpp` add-entity template (none exists even for
   `heterogeneous_medium` today), and a `MediaIntrospection` stance —
   the editor currently hard-rejects heterogeneous-medium edits because the
   majorant grid is construction-baked; the new chunk either extends that
   documented rejection or supports rebuild-on-edit. Scene *save* needs no
   per-chunk work (`SaveEngine` serializes the CST generically) — stated so
   reviewers don't re-litigate it.
2. **Collision-based spectral fire-thermal emission** (G1 + G9). This rule is
   for the Kirchhoff term ε_thermal=σ_aB, whose support is contained in σ_t;
   it is not the renderer's arbitrary additive-emission contract. For every
   distance sampler in use, at the sampled scatter
   distance t (whichever branch produced it), score

   > ε_thermal(t,λ) · T_det(t) / p(t)
   > = σ_a,total(t,λ) · B_λ(T(t)) · T_det(t) / p(t),

   where T_det is deterministic transmittance
   (`EvalDeterministicOpticalDepth`) and **p(t) is the pdf of the distance
   sampler actually in use** — the pattern the equiangular-MIS scatter path
   already follows in-tree (`Tr·σ_s/combined_pdf` under
   `useExplicitThroughput`; the pure-DT path uses implicit collision
   throughput, which the reduction below recovers). Unbiasedness is
   immediate:
   E = ∫ p(t)·[ε_thermal·T_det/p] dt = ∫ T_det·ε_thermal dt, the emission
   source term, for *any* valid p. Two operating regimes:
   - **Pure delta tracking** (no positional lights / no pivot): p = σ_t·Tr,
     and the score algebraically reduces to (σ_a/σ_t)·B_λ at real
     collisions — the classic collision-based estimator.
   - **50/50 DT-vs-equiangular mixture active**: p = 0.5·pdf_dt +
     0.5·pdf_eq (both deterministically evaluable). Note the equiangular
     branch has no real/null collision classification — which is precisely
     why the rule is stated in the T_det/p form rather than
     "score at real collisions and divide by the mixture pdf" (revision 3's
     phrasing, which double-compensated Tr on the DT branch and was
     undefined on the equiangular branch).

   **The no-scatter outcome scores no fire-thermal emission — and the existing
   no-scatter pickup is deleted for this term.** The rule is exactly unbiased as
   stated *because* the scatter-sample expectation over [0, L] already
   integrates the full segment emission (E = ∫₀^L p·[ε_thermal·T_det/p]dt =
   ∫₀^L T_det·ε_thermal dt); the current code's no-scatter branch
   (RayCaster.cpp:1228–1260) adds a full-segment analytic integral whose
   role is fully absorbed by that expectation — an implementer who "keeps
   the branch and upgrades the scoring" double-counts by
   ≈Tr(L)·∫T_det·ε_thermal.
   RISE's existing `MediumCoefficients::emission` is different: it is an
   absorption-independent additive source and can be nonzero where σ_t=0
   (including heterogeneous density holes), so collision sampling has no
   absolute continuity there. Its old closed-form/no-scatter block is replaced,
   not deleted without replacement. A truly homogeneous segment uses the exact
   analytic integral, with the σ_t→0 limit L·ε_add. A heterogeneous segment
   draws an independent t~Uniform(0,L) on every full marched segment and scores

   > L·T_det(0,t)·ε_add(t),

   using a disjoint RNG substream and the deterministic optical-depth evaluator;
   this runs regardless of collision/no-collision outcome and never shares the
   thermal sample. It is unbiased even in vacuum/density holes. **ε_add is
   excluded from the Phase-B emission CDF/volume NEE exactly like chem, and
   this full-segment estimator always has MIS weight 1**; the thermal
   p_march/p_V weight never touches it. §8 does **not** advect arbitrary ε_add:
   requested velocity blur with configured/render-consumed ε_add disables that medium's blur in
   preview and rejects predictive blur until a source-evolution contract exists.
   Zero-extinction
   homogeneous emitters, heterogeneous zero-density holes, mixed thermal+
   additive emission with NEE on/off, and the nonadvected-source blur preflight
   are regression gates through both `RayCaster` and
   `PathTracingIntegrator`.
   Note the (σ_a/σ_t)·B reduction holds only where tracking is per-λ
   (spectral path); the RGB path's scalarized tracker must use the general
   T_det/p form. ε_thermal(y,λ) = σ_a,total·B_λ per §4.2 via the extracted Planck free
   function; physical scalars end-to-end — never through IPainter/JH uplift
   (§10.1). **Surfaces and ordering (G11):** this estimator lands in BOTH
   `PathTracingIntegrator` (the PT rasterizers' actual integrator — where
   p_ω must be the guiding-aware `effectivePdf`) and `RayCaster`, and the
   score happens **immediately after a finite sampled event, before every
   σ_s, max-volume-bounce, path-depth, RR, or continuation gate**. The
   distance sampler and emission score still run when `maxVolumeBounce=0`;
   that cap suppresses scattering continuation only. The integrator currently
   encloses the event under its bounce test at ~:1723/~:4136 and must move the
   score outside it. A pure absorber must emit; a DT no-event atom and a
   zero-density equiangular proposal legitimately score zero.

   **No silent distance-proposal cap:** current Pel/NM `SampleDistance*`
   loops stop after 1024 candidates and return “no event,” deleting the
   remaining collision measure. Both transport surfaces must replace that
   behavior with state-preserving chunked continuation (same ray parameter,
   RNG stream, and accumulated tracker state) until a real event or segment
   end. The chunk size may remain a watchdog yield point but has no estimator
   semantics. **Distance densities are never floored.** The existing
   `fmax(pdf,1e-30)` sites in `SampleDistanceWithPdf` and
   `EvalDistancePdf{,NM}` must be removed: at τ=100 the true density is about
   3.72×10⁻⁴⁴, and replacing it by 10⁻³⁰ suppresses a deliberately bright
   tail by 3.72×10⁻¹⁴. The general path carries log T_det and log p; DT+
   equiangular mixtures use log-sum-exp, and power-heuristic weights are formed
   from scaled/log density ratios without exponentiating a common tiny factor.
   Pure per-λ DT uses the analytic (σ_a/σ_t)B cancellation before any division.
   A τ>70 slab scales B so the tail contribution remains measurable and must
   match a high-precision log-domain reference. **HWSS is hard-disabled
   for fire media** (auto per-λ fallback) until the integrator's
   companion-proposal bias (~:4158) is fixed in Phase D. Gates: matches
   step 0's closed form on constant media; matches brute-force ray-marched
   reference on a varying-T slab, with and without positional lights (both
   regimes above); **pure-absorber emissive slab (σ_s = 0) in Pel, NM, and
   HWSS-requested modes, through both entry routes**; and the
   **isothermal-slab absolute gate L_λ = B_λ(T)·(1−e^{−τ_λ})** against the
   pinned Planck kernel; Pel/NM through both entry routes with
   `maxVolumeBounce=0`; and a forced >1024-null-proposal slab that must match
   an uncapped reference. (CDF normalization is a Phase B gate — §7.2.7.)
3. **Chemiluminescence estimator** (the §4.4 term — revision 3 defined the
   source but gave it no transport path, and in exactly the showcase
   regimes — flame base, methanol blue-only flame, zero-soot premixed
   pilots — f_v ≈ 0 means σ_t ≈ 0: **no collisions ever occur there**, so a
   collision-based estimator collects nothing and the §3.8 methanol gate
   would be unpassable). ε_chem is absorption-free (it adds emission
   without adding extinction), so it is collected by an **independent
   randomized line estimator on every marched segment**:
   ∫ T_det(t)·ε_chem(t,λ) dt, at MIS weight 1 (see §7.2.3 for why NEE
   never competes for it). Panels traverse the union of reaction and
   extinction knot lattices. For each initial panel p, form the conservative
   chem-support weight R_p=L_p·Σ_b q̇‴_b,max,p from the panels' certified
   channel bounds, and define q_rxn(t) by choosing p with R_p/ΣR and sampling
   uniformly inside it. The actual proposal is

   > q_chem(t)=½/L+½q_rxn(t),
   > Ĉ_chem=T_det(t)ε_chem(t,λ)/q_chem(t),  t~q_chem,

   with q_chem=1/L when ΣR=0. The uniform half guarantees support even if a
   bound or lattice corner is loose; the same piecewise density is evaluated
   at the sampled t. One sample per segment is the baseline; a fixed N>1
   averages independent samples and changes variance only. T_det is the exact
   deterministic optical-depth evaluation in static mode. Therefore
   E[Ĉ_chem] is the required integral without treating an embedded quadrature
   discrepancy as a rigorous error bound. Segment
   semantics, stated precisely because they are where implementations
   diverge: the integral runs over the **full segment**
   [0, min(surface hit, medium exit)] **regardless of the scatter
   outcome** — it estimates the RTE's additive emission term independently
   of the scatter sample, and the continuation segment leaving a scatter
   vertex (new direction) creates no overlap; truncating at the sampled
   scatter distance undercounts. Likewise, **every early-out in the
   distance-sampling code (e.g. the equiangular zero-contribution return)
   must still score the segment's chem integral** — dropping it there
   biases exactly the zero-soot showcase this step exists for. It is collected
   for every path with a segment through the medium; no collisions required.
   This reaction-importance construction is **nominal/static only**. Requested
   blur with render-consumed chem channels is a predictive preflight error even
   when the nominal grid is zero; preview disables
   blur for the whole medium and retains this same estimator (§8). No advected-
   chem ratio estimator exists. Estimator tests may use a clearly marked synthetic
   SPD, but predictive enablement requires the §7.0 per-fuel record and an
   optically thin uniform reaction slab whose integrated spectral power equals
   the pinned band-source power over each S_b interval within a stated
   confidence interval. A thin reaction sheet aligned adversarially with a
   high-optical-depth transition is compared against a high-accuracy reference;
   the old correlated Gauss–Kronrod alias is a RED implementation.
4. **Chromatic heterogeneous coefficients** (G4): real `GetCoefficientsNM`
   with λ-dependent σ_a/σ_s from §4.1/§4.3 — plus the exact §4.3
   **`MakePhaseClosure(x,λ)` / `MakePhaseClosurePel(x,band_preset)` API**, with
   the returned immutable closure used by continuation, direct-light adapters,
   and guiding in both `PathTracingIntegrator` and `RayCaster`. This replaces,
   rather than supplements, the fixed `GetPhaseFunction()` path for fire
   media. **Spectral-path-first policy**:
   fire scenes target the spectral rasterizers, where per-λ tracking against
   a per-λ majorant is correct today via the G5 fallback. The RGB path gets
   band-averaged (projected) coefficients so fire scenes still *render*
   there — but this is **deterministic spectral-projection bias, not merely
   variance** (external-review correction: exp(−σ̄L) ≠ the channel-averaged
   spectral transmittance, so the expected RGB result itself changes). The
   RGB path is an explicitly **approximate preview path**: it renders with
   a logged diagnostic, and predictive output is spectral-only — the
   fidelity bar (§1) is claimed only for the spectral path (§12, RGB-path
   decision). Majorants: conservative max-over-λ first — with the bound
   including interpolation overshoot, and (once HWSS media land) bounding
   every wavelength in the carried bundle, not only the hero (§12,
   chromatic-majorants decision).
5. **Auto-rasterizer media rule** (G10): Tier-1 static check — any
   heterogeneous or emissive medium ⇒ force PT route. One rule + one test
   alongside `AutoRasterizerTest.cpp`.
6. **Procedural analytic flame test asset**: parametric candle/flame
   (f_v, T, q̇‴) fields via the painter-bake path (published laminar-flame
   profiles), so rendering work is never blocked on the sim and numeric
   tests have a closed-form target. This is a *radiometry* target, not a
   beauty target (§11).

*Side benefit:* chromatic heterogeneous media upgrade clouds/fog/nebulae
scenes independently of fire.

### 7.2 Phase B — "fire lights the scene": emissive-volume NEE (G3)

The full design; the estimator, the MIS partition, and the bookkeeping are
specified to implementation level because two competent implementers given
less would build differently-biased renderers.

**7.2.1 The estimator.** At a receiver vertex x the emission-gather term is

> L_e-NEE(x) = ∫_V f(x, ω_{x→y}) · V(x,y) · Tr(x,y) · ε(y,λ) / ‖x−y‖² dV(y),
> ε(y,λ) = σ_a(y,λ)·B_λ(T(y)),

with the receiver term matching the in-tree throughput factoring: at a
**surface** vertex f = BSDF · |cos θ_x|; at a **medium** vertex f = phase
function only — σ_s is already carried by the path throughput at scatter
events (RayCaster factors `Tr·σ_s/pdf` into throughput before the
scatter-vertex lighting), so including it again here would double-count.
Sampling: draw a cell then a voxel then a point (7.2.3), pdf p_V(y) per unit
volume. Contribution f·**V(x,y)**·Tr·ε/(‖x−y‖²·p_V(y)) — the integrand
carries **geometric visibility explicitly** (external-review catch): the
existing medium boundary walk computes *attenuation only* and deliberately
ignores non-medium objects (LightSampler.cpp ~:307), so volume NEE runs
**both** the surface shadow test (as ordinary NEE does at ~:1662) *and* the
medium walk. `transparent_shadows` policy, pinned: **direction-bending glass blocks
this strategy** (direction-bending dielectric occluders are treated as
opaque for volume NEE — the straight-line Fresnel-attenuated approximation
must not compete with a march path that physically refracts; refracted
flame paths are march-only at weight 1 per §7.2.2's support rule —
whereas only strict *null boundaries* pass NEE — every other interface,
including straight-through tinted transmitters, blocks; §7.2.2). Tr(x,y): **the inherited
boundary walk is NOT usable as-is** (third external round — its own header
documents silent 4-media nesting drops, a 16-crossing truncation, a
Tr < 10⁻⁶ hard-zero, and wrong-origin evaluation of an interrupted global
heterogeneous medium; the ratio tracker also returns a partial result at
its step cap, `HeterogeneousMedium.cpp:~537`). Phase B implements a **true
segment-wise active-medium walk** — each segment's transmittance evaluated
from that segment's own origin in that segment's active medium — with
**no silent caps**: deep nesting and high crossing counts either continue
unbiased or fail explicitly, and step-cap exits continue rather than
truncate. Gated by step-field tests with >4 overlapping media and >16
crossings (§7.2.7). The result is unbiased in the *contribution*, and
never inside a pdf, avoiding the stochastic-Tr-in-MIS-weight trap.

**7.2.2 MIS partition and the double-counting resolution.** Two strategy
families reach emission at a point y from vertex x:

- **March**: direction sampling followed by distance sampling to a real
  collision, with the following externally-reviewed refinements to its density:
  - **The pre-NEE object is a new immutable continuation closure for both
    surface and medium vertices.** It is not the current aggregate
    `IBSDF`/RNG-consuming `ISPF`. Before volume NEE and without consuming lobe,
    direction, pivot, endpoint, or roulette random numbers, construct one
    `IContinuationClosurePel` or `IContinuationClosureNM` for the vertex and
    current path/IOR state. (HWSS is already forbidden for fire.) A surface
    closure owns a stable ordered set of lobe-state
    classes. Each class exposes its measure (`solid_angle|delta`), bounce-limit
    category, direction-independent selection mass, subset `Evaluate(ω)` and
    normalized ungated `BasePdf(ω)`, the exact geometric-horizon predicate
    h_ℓ(ω)∈{0,1}, exact roulette-survival function after the candidate throughput,
    and deterministic IOR/medium-stack transition. Its successful continuous
    density is p_ℓ(ω)=BasePdf_ℓ(ω)h_ℓ(ω); the residual probability is an explicit
    null/termination atom. `Evaluate` uses the same horizon predicate. The closure exposes
    `EvaluateSubset(A,ω)`, `PdfMarginal(A,ω)`, and
    `SampleSubset(A,ξ_lobe,ξ_dir,ξ_rr)`; all three use the same normalized
    selection masses and return/consume the same transition metadata.
    `SampleSubset` orders select-lobe → sample normalized base direction →
    horizon reject-to-null → roulette; it never retries or renormalizes surviving
    directions, and the horizon indicator is not applied again through roulette.
    It is
    retained unchanged through NEE and the later continuation sample—rebuilding
    it from post-NEE state or adapting `ISPF::Scatter` output after the fact is
    forbidden.

    A medium closure retains the **same immutable `MakePhaseClosure(x,λ)` (or
    Pel-band closure) instance** used for evaluation and later sampling, plus
    total/path and volume-bounce availability and exact RR survival. To keep
    its counterfactual density evaluable, **volume path guiding is disabled at
    every medium vertex where volume NEE competes**: the closure's single
    solid-angle class uses the phase closure's actual Evaluate/Sample/Pdf, and
    the later continuation must keep guide α=0. Thus
    p_ω,reach(ω)=p_phase(ω)r(ω), never the unrecorded guide-aware
    `effectivePdf`. Medium vertices with no emissive-medium NEE competitor may
    retain existing guiding. The surface RIS/guiding disables below remain
    separate. This is an explicit capability, not an adaptation of legacy
    `GetPhaseFunction()`: append default-unsupported
    `IMedium::MakeContinuationPhaseClosurePel/NM(...)`. Before calling it, a
    central preflight requires an exact built-in dynamic-type pair. The closed
    table permits exact `HomogeneousMedium` and exact `HeterogeneousMedium`
    when their phase object is exact `IsotropicPhaseFunction` or exact
    `HenyeyGreensteinPhaseFunction` (the latter requiring finite -1<g<1), **and
    the arc's own multi-channel fire medium with its §4.3 σ_s-weighted
    constituent HG-mixture closure** (each constituent lobe requiring finite
    -1<g_j<1; the mixture weights are the local σ_s,j of §4.3, so the closure
    is exactly the object §7.1 step 4 constructs). **Adding that row is a
    Phase-A deliverable, not a later amendment** — the fire medium is the only
    medium volume NEE exists to light, so leaving it default-denied would
    disable Phase B inside its sole use case; §7.0's Phase-B gate lists the
    row explicitly. Every subclass, plugin medium, plugin phase, and unlisted
    future type is no, even if it inherits or overrides a yes factory. Predictive preflight of
    every render-reachable medium rejects
    `medium_continuation_closure_unsupported`; preview sets
    `competitionAvailable=false`, disables volume NEE at that vertex, and uses
    the legacy collision march with weight 1. Supported competing vertices
    must have guide alpha and guide sample count exactly zero. Pel/NM
    phase-Evaluate/Pdf/Sample equality, mixed supported/unsupported media,
    derived built-in, plugin, invalid-g, and guiding-zero tests gate the table.

    Closure creation is **material-owned**, not SPF-RTTI-owned: append
    default-unsupported `IMaterial::MakeContinuationClosurePel/NM(...)`. An
    audited material override constructs response, base sampling law, horizon,
    and transitions from one coherent parameter set. Aggregate
    `IBSDF::value` plus an independently returned `ISPF::Pdf`, a sampled
    `ScatteredRay` container, or lobe type learned only after `Scatter` is not
    sufficient. `CompositeSPF` is explicitly unsupported: its variable-depth
    stochastic internal layer walk and approximate `Pdf` cannot define an
    exact pre-NEE marginal without consuming forbidden random state. The same
    exclusion applies to any procedural SPF with a stochastic latent walk or
    only an approximate marginal. Predictive Phase B preflight recursively requires this
    capability on every render-reachable material and rejects
    `continuation_closure_unsupported`; preview disables volume NEE at an
    unsupported vertex, sets `competitionAvailable=false`, and leaves its
    collision march weight 1. This capability is part of the normal public
    interface/ABI, parser-independent implementation, all-build-project, and
    Pel/NM sibling checklist. The Phase-B implementation lands an audited exact
    capability allowlist; “built in” alone never implies support. That allowlist
    is enforced independently of the virtual factory by a central exact-dynamic-
    type check (`typeid(*material)==typeid(listed_type)`, or an equivalently
    unforgeable core type token). A plugin/subclass cannot inherit, override, or
    self-attest its way into a yes cell. Luminaire entries require the exact
    wrapper type and recursively apply the same exact-type check to the wrapped
    base before delegation. Only after this central check succeeds may Job call
    the material-owned factory. The initial allowlist is closed and
    default-deny:

    | exact material factory | Pel | NM | closure classes / selection / transition |
    |---|---:|---:|---|
    | `LambertianMaterial` | yes | yes | one diffuse solid-angle class, mass 1, ungated cosine base law + geometric-horizon null atom, matching Lambertian Evaluate, no IOR transition |
    | `OrenNayarMaterial` | yes | yes | one diffuse solid-angle class, mass 1, ungated cosine base law + horizon atom and exact Oren–Nayar Evaluate, no IOR transition |
    | `IsotropicPhongMaterial` | yes, only when exponent has no per-channel variation | yes | diffuse + glossy-reflection solid-angle classes; Pel masses normalize `max(0,MaxValue(Rd))` and `max(0,MaxValue(Rs))`, NM masses normalize the corresponding nonnegative wavelength values; ungated cosine/Phong base laws + per-class horizon atoms, matching Evaluate, no IOR transition |
    | `LambertianLuminaireMaterial` / `PhongLuminaireMaterial` | delegated | delegated | factory delegates unchanged to its wrapped base material and adds no scattering response; unsupported base stays unsupported |
    | integrator clay override | yes | yes | explicit synthetic `LambertianMaterial` factory used consistently for NEE and continuation |
    | all other built-ins and every plugin | no | no | unsupported until a later reviewed table revision |

    “All other built-ins” explicitly includes every material backed by
    `AshikminShirleyAnisotropicPhongSPF`, `BioSpecSkinSPF`, `CompositeSPF`,
    `CookTorranceSPF`, `DielectricSPF`, `GGXSPF`,
    `GenericHumanTissueSPF`, `PerfectReflectorSPF`, `PerfectRefractorSPF`,
    `PolishedSPF`, `SchlickSPF`, `SheenSPF`, `SubSurfaceScatteringSPF`,
    `TranslucentSPF`, both Ward SPFs, and any unlisted future type. A custom or
    wrapper `IMaterial` that merely returns an allowlisted SPF/BSDF pair remains
    unsupported unless its own audited factory delegates as listed. Each
    closure construction validates the parameter values actually used at the
    vertex before NEE: every Pel channel or queried NM response coefficient must
    be finite and componentwise nonnegative; the Phong exponent must be finite
    and ≥0 (and Pel-channel invariant as required by the table); Oren-Nayar
    roughness must be finite and ≥0. NaN, infinity, negative response, or an
    invalid exponent/roughness returns `continuation_parameter_invalid` and may
    not be clamped. Predictive rendering hard-fails the request; preview disables
    volume NEE for that vertex and uses the legacy weight-1 march. Physical
    energy-conservation bounds, if later desired, are a separate qualification
    rule and are not silently inferred here. With the required nonnegative
    coefficients, a zero sum of selection weights makes A empty and also makes
    its scattering response zero. Each yes cell has Pel/NM
    Evaluate/Pdf mass, horizon-null, sampled-frequency, cap, and transition tests;
    changing the table is a reviewed design/schema change, not adapter
    auto-discovery. RED controls include a derived Lambertian, a derived
    luminaire wrapper, negative/NaN/infinite constant and procedural painters,
    invalid Phong exponents, and invalid Oren-Nayar roughness.
  - **Continuation availability is resolved before NEE.** Build an immutable
    `ContinuationAvailability` view of that closure from the current total/path and per-type
    bounce limits. Let A be exactly the lobes that the later continuation is
    allowed to sample; renormalize its direction-independent selection masses
    over A, and let r_ℓ(ω) be the exact later roulette-survival probability
    (including any deterministic zero gate). The successful-continuation
    directional **subdensity** is

    > p_ω,reach(ω)=Σ_{ℓ∈A}s_ℓ^A p_ℓ(ω) r_ℓ(ω).

    It need not integrate to one. The later continuation must call
    `SampleSubset` on this same closure/A and use its roulette atom and survival compensation; a future random
    choice or gate that is not evaluable from the record is forbidden. If A is
    empty, or every r_ℓ is zero in direction ω, p_ω,reach(ω)=0. Volume NEE may
    still run at that terminal vertex and then has no march competitor.

    Mixed-lobe caps require an additive support split, not one weight on the
    full BSDF. Define f_A=Σ_{ℓ∈A}f_ℓ and f_D as the remaining direct-connection
    response. NEE evaluates f_A with the family MIS weight against
    p_ω,reach and evaluates f_D as a separate NEE-only, weight-1 term; the
    continuation evaluates f_A against the full allowed-lobe marginal, never a
    sampled labeled component. When A is empty the whole NEE response is f_D
    and has weight 1. This preserves direct lighting at a terminal vertex
    without pretending a capped continuation exists.
  - **p_ω is the actual direction proposal — with the ordering and
    RIS constraints that make that evaluable** (second external round):
    volume NEE runs at the scatter vertex *before* the guided continuation
    is sampled (verified, `PathTracingIntegrator.cpp` ~:1761ff). This baseline
    therefore applies the explicit medium- and surface-guiding disables above/
    below whenever volume NEE competes; the not-taken alternative would have to
    initialize an immutable guide state before NEE and share its exact mixture
    and fallback with the later sample. `RayCaster`
    must likewise record the *actual* proposal pdf (it currently
    propagates raw `phasePdf`, ~:1063–1142). At surface vertices where
    the direction proposal is RIS-based, the RIS quantity
    target·N/Σweights is **not** a marginal pdf usable in power MIS:
    **RIS is disabled at vertices where volume NEE competes**. And
    disabling RIS is *not sufficient* (third external round): surface
    NEE runs before the SPF generates its lobes, and `RandomlySelect`
    then picks a lobe with **direction-dependent sampled kray weights**
    (`PathTracingIntegrator.cpp:~2926`) — so the recorded BSDF pdf omits
    the lobe-selection law and the NEE-side counterfactual still cannot
    evaluate the later march proposal. The committed rule: **at vertices
    where volume NEE competes, lobe preselection is
    direction-independent** (selection probabilities s_ℓ from
    direction-independent lobe weights, e.g. hemispherical albedos, fixed
    before NEE) — and, before roulette, the march directional pdf is the **full
    allowed marginal p_ω,A(ω) = Σ_{ℓ∈A}s_ℓ^A·p_ℓ(ω)**, with the **total
    allowed response f_A** evaluated over that marginal in both its NEE term and the continuation
    throughput (round 4: the single labeled component s_ℓ·p_ℓ is not the
    marginal — with overlapping diffuse/glossy lobes it gives unequal
    lobes non-complementary MIS weights; the alternative — NEE decomposed
    per shared lobe label — is recorded and not taken). **Surface
    path-guiding is disabled at competing vertices too** (round 5,
    verified in-tree: guiding eligibility, the guide transform, and α all
    depend on the *selected lobe* — `GuidingSupportsSurfaceSampling(*pS)`,
    `GuidingEffectiveAlpha`, per-type cosine products,
    PathTracingIntegrator.cpp ~:2961ff — so the actual proposal is
    Σ_ℓ s_ℓ[(1−α_ℓ)p_ℓ + α_ℓ g_ℓ] including init-failure fallbacks, not
    the plain lobe marginal; disabling guiding there is the same
    containment strategy as the RIS rule, with the full lobe×guide
    marginal recorded as the not-taken alternative). The
    augmented-space-MIS alternative (candidate/lobe state as shared
    auxiliary state) is recorded and rejected for complexity. Gated by
    `IsotropicPhongSPF` diffuse+glossy NEE-on-vs-off equality tests,
    plus guided-configuration pdf-normalization and equality tests
    (§7.2.7).
  - **Support — with "medium boundary" now defined** (second external
    round: media are commonly hosted by geometric dielectric shells, and
    a `casts_shadows FALSE` shell could let NEE pass straight through a
    boundary the march *refracts* at — two weight-1 strategies, or none):
    a **null boundary** is a *dedicated interface class*, strictly
    defined (third external round: "IOR-matched" alone is insufficient —
    a tinted perfect transmitter or a coated IOR-matched surface leaves
    direction unchanged yet attenuates or reflects march paths, and NEE
    passing without that factor would be biased): **unit deterministic
    transmission; no reflection lobe, no absorption/tint, no emission, no
    shading, no depth increment, no roulette event**. Only a null
    boundary stays inside the march chain — preserving the originating
    vertex's NEE state, direction density, and survival atoms — and only
    a null boundary passes volume NEE (attenuation via the medium walk).
    **Every non-null interface blocks volume NEE and terminates march
    support — no exceptions** (round 4 killed r8's equal-transfer option
    for straight-through tinted transmitters: equal factors do NOT fix
    the partition, because pre-interface NEE takes weight 1 where
    p_march = 0 while the delta-transmitted march starts a *new* weight-1
    segment beyond — a structural double count). Direction-bending
    dielectrics block unconditionally regardless of
    `casts_shadows`/`transparent_shadows` flags; opaque surfaces block
    via the visibility term. A future "transparent-chain" exception
    (carrying the originating NEE state, direction density, interface
    selection mass, and survival factors through non-null straight
    interfaces) is recorded as out-of-scope follow-on work, not
    permitted now. Exactly one strategy owns each side of every
    boundary.
    For y in segment k of the chain, the density carries the **boundary
    survival factors**:
    p_march(y) = p_ω,reach(ω)/‖x−y‖² · (Π_{i<k} P_{0,i}) · p_{t,k}(s_k), where
    P_{0,i} is segment i's no-event probability — under the 50/50 mixture
    that is 0.5·S_i with **S_i = e^{−τ_i}, segment i's deterministic
    transmittance** (the DT no-scatter atom's mass), since only the
    delta-tracking branch owns the no-scatter atom (the equiangular branch
    always proposes a scatter). Proposal survival and physical throughput are
    separate. Immediately after every survived segment and before crossing its
    null boundary, update the sampled path weight exactly once as

    > W ← W · T_det,i / P_{0,i}.

    Thus the finite 50/50 case multiplies W by 2, while pure DT has
    P_{0,i}=T_det,i and multiplies by 1. The same P_{0,i} remains in
    p_march for endpoint MIS; omitting either the throughput compensation or
    the density factor is forbidden. This update occurs at each segment, not
    only at a terminal surface/background return.
    This p_march is identically zero when continuation cannot reach y; neither
    a nominal material lobe nor an NEE-before-RR implementation order creates
    proposal support.
  - p_t per segment is the pdf of the sampler actually in use — pure DT
    (σ_t·exp(−τ)) or the 50/50 balance mixture (§5.1) — evaluated
    deterministically via `EvalDeterministicOpticalDepth`, the same
    deterministic-denominator discipline the existing MIS uses.
- **Volume NEE**: p_V(y) from 7.2.3.

**BSSRDF/SSS containment.** “Eligible vertex” in this arc excludes the two
post-BSSRDF entry sites (diffusion-profile and random-walk SSS). Those sites
have a joint spatial/directional continuation measure that the closure above
does not model. The transitive shader/SPF dependency walk required by §10.3
also classifies `SubSurfaceScatteringShaderOp` and
`DonnerJensenSkinSSSShaderOp` as nonlocal SSS. Predictive Phase B therefore
rejects any render-reachable SSS material **or either shader op** with
`sss_volume_nee_unsupported`; unknown nested shader dependency is fail-closed.
In preview, both BSSRDF-entry NEE
sites hard-disable **volume** NEE (ordinary surface-light NEE is unchanged),
draw no flame pivot/endpoint, and initialize the recursively launched child
segment with `competitionAvailable=false`; any later collision emission is
march-only at weight 1. Each named shader op likewise enters a request-local
`SSSContainmentScope` before any remote irradiance/nested `Shade` call; the
scope is inherited by all nested calls and forces volume NEE off, no flame
pivot/endpoint, and a fresh competition=false child state. It does not mutate
the Scene or authored shader. Propagating the parent surface's competition/pivot state
through the BSSRDF or treating either entry as an ordinary solid-angle closure
is forbidden. Supporting SSS later requires a separate conditioned spatial ×
direction × roulette proposal and NEE-on/off proof for both diffusion and
random-walk paths.

Weights are **power-2 between the NEE and march families** (PT's surface
NEE-vs-BSDF convention), while **within the march family the existing 50/50
balance mixture stays balance** (the in-tree volumetric convention;
[MIS_HEURISTICS.md](MIS_HEURISTICS.md)) — a deliberately mixed convention,
stated. Soundness note: MIS weights need only sum to 1 at each y; each
strategy's *contribution* divides by its true sampling pdf, so
deterministic/quadrature-evaluated pdfs inside weights preserve
unbiasedness (and `EvalDeterministicOpticalDepth` is exact for all three
accessors anyway). When p_march's equiangular component is
pivot-conditioned (7.2.4), the **family-level weights are evaluated
conditioned on the same shared pivot vector U** — stated explicitly so the
fixed-U weights-sum-to-1 argument applies verbatim rather than by
inference.

**The weight-1 rule (this IS the volumetric emission-suppression
bookkeeping) — TWO bits of per-segment state, defined precisely** (round
5: RISE's `eRaySpecular` classification includes non-delta glossy
reflection — `PathTracingRayType` maps everything but non-delta diffuse
to specular, PathTracingIntegrator.cpp ~:531 — so "specular ⇒ weight 1"
would double-count NEE on glossy marches; conversely a true mirror lobe
from a diffuse+mirror material needs weight 1 even though NEE ran at the
vertex):

- **competitionAvailable** — set iff volume NEE was *enabled and
  attempted* at the originating vertex (set on attempt, NOT only on a
  nonzero/visible NEE sample — a rejected NEE draw is still a competing
  strategy, and gating on visibility would bias later march hits high);
- **continuationSingular** — set iff the *sampled* continuation lobe is
  measure-degenerate (a true delta), independent of the
  `eRaySpecular` classification.

**Only collision-collected thermal aerosol emission** takes MIS weight
p_march²/(p_march²+p_V²) iff
competitionAvailable ∧ ¬continuationSingular; weight 1 otherwise (camera
primaries, delta continuations, NEE-disabled vertices) — else the
directly-viewed flame dims or glossy marches double-count. (Revision 2's "the weights do the
bookkeeping, no flag needed" was wrong — the flag *selects which partition
applies*.) Explicitly scoped out: **flame seen through a refractive
boundary** — NEE cannot reach through refraction (straight shadow rays), so
those paths are march-only at weight 1 by this same rule; VCM-class
transport for volumetric SDS is out of scope (§5.2).

**7.2.3 The emission importance structure — two-level, thermal-aerosol only.**
Chem and ε_add remain excluded and keep their independent full-segment,
weight-1 estimators. “Voxel” here is replaced by one exact continuous
**emission-bin partition** so sample and pdf cannot disagree with §8's
center-sampled lattice. For a channel of dimensions N_x×N_y×N_z, bin
B_ijk is the Cartesian center cube

> [o+(i−½)Δx,o+(i+½)Δx) × [o+(j−½)Δy,o+(j+½)Δy)
> × [o+(k−½)Δz,o+(k+½)Δz),

with only the global upper faces closed. These N_xN_yN_z disjoint bins exactly
tile the core face bbox. Sampling is explicitly topology-aware: before reading a
stored value, test its active bit; an inactive VDB voxel/tile inside that bbox
samples the channel's declared background regardless of its stored payload:
T_inf kelvin for temperature and exact zero for
carbon, condensed mass, reaction, and every chem-source channel. The exterior
of the medium face bbox is **not** another background voxel: traversal ends and
the medium is absent. Point lookup is componentwise
clamp(floor((x−o)/Δ+½),0,N−1), including the closed upper-face clamp.
For each bin, a fixed 2×2×2 Gauss rule in space plus the pinned wavelength quadrature computes the nonnegative proposal
estimate

> Ĩ_v = V_v ∫ ε̃_thermal,v(λ)dλ.

over the camera-visible band. This is an importance approximation, not a
radiometric integral used in the contribution. The channel/temperature
interval bounds also produce a finite conservative emissive-power upper bound
U_v over the same bin and band. A center bin intersects halves of as many as
2³ trilinear interpolation cells; U_v encloses **every** such stencil, including
the declared per-channel background corners at the outer boundary, rather than treating the center
cube as one eight-corner interpolation cell. U_v>0 whenever trilinear
interpolation can produce nonzero thermal emission anywhere in the bin, even if
the quadrature nodes miss it. Define η=2⁻¹⁰ and

> w_v=(1−η)Ĩ_v+ηU_v,
> W_m=Σ_v w_v,  q_v=w_v/W_m.

If W_m=0 the medium has no thermal-emission strategy. Otherwise q_v>0 wherever
ε_thermal can be nonzero. Per-cell weights on the majorant topology are
Φ_c=Σ_{v∈c}w_v
(a raw scene-space value equal to the corresponding W·sr⁻¹ integral divided
by s², because V_v is in scene³ while ε_scene=sε_SI; it is **not physical
power**, both because the scene-space integral still needs s² and because
ηU_v intentionally inflates support).
Thus **A_m=4πs²W_m** is the watt-dimensioned band-importance proxy used across
media/lights, not a radiometric claim. The medium-only q^V pmf is unaffected
because the common scale cancels. Majorant grids are capped at 32 *cells per axis* (§5.1) —
at 512³ a cell spans 16³ voxels whose ε varies by orders of magnitude
across the flame sheet, so within-cell *uniform* sampling would leave large
variance the cell CDF cannot address. Assign each emission bin to the majorant
cell containing that bin's lattice center, using the same half-open/closed-upper
ownership rule. The structure is therefore
**two-level: cell CDF (Φ_c/W_m), then per-bin w_v/Φ_c CDF within the cell, then
uniform within the bin** — the per-bin pass that builds Φ_c yields the
second level at the same O(N) cost. Since Σ_cΦ_c=W_m,
**p_V(y)=q_v/V_v** using the bin lookup above, an O(1) density whose construction exactly matches its
sampler. **p_V is λ-independent by construction**
(band-integrated importance), so it is a valid — merely suboptimal —
importance for every per-λ path; radiometric, not photometric, weighting
(the spectral pipeline's NEE importance shouldn't bake in a photopic
curve). **ε_chem is excluded from Φ and from the NEE estimator entirely**:
measured chemiluminescent power is roughly 10⁻⁴ of **chemical HRR** (§2.3),
not of soot power (which can vanish in methanol). It is collected solely
by the randomized line estimator of §7.1 step 3 at weight 1 — which is
also what makes that line integral MIS-free (NEE never competes for the
chem term, so there is no double count by construction). Exclusion affects
variance, not expected energy: a nearby diffuse receiver in a chem-only case
must match a high-spp reference, and if its measured variance is impractical,
chem NEE becomes a required follow-on rather than being dismissed by a false
soot-relative ratio. Extinction-majorant
construction (corrected round 5 — "extinction majorants are
T-independent" became false the moment φ(T) entered the optics): the
per-λ extinction is σ_t = c·[k_cool(λ) + φ(T)·(k_hot(λ) − k_cool(λ))] +
d·k_cond(λ), and the safe per-block bound is pinned as

> σ̄_block(λ) = C_max·max(k_hot(λ), k_cool(λ)) + D_max·k_cond(λ),

then max over λ — i.e. the bound takes the sup over φ ∈ [0,1] so it is
*constructed* T-independently; composing only coincident corner values
of c and T can underbound anticorrelated fields (hot where thin, cold
where dense). Gate: an anticorrelated carbon/temperature field test
asserting the tracking acceptance ratio never exceeds one. The emission
CDF remains the only *new* nonlinear per-frame rebuild; it runs in the
frame-advance step with the majorant rebuild (§10.3).

**7.2.4 Equiangular interaction and strategy selection.** There are two
distinct distributions and implementations must not reuse one random roll for
the other. **Volume NEE is always attempted once at every eligible non-delta
vertex whenever at least one emissive medium has W_m>0**. Here eligible means
the immutable continuation closure is supported and the site is not either
BSSRDF entry contained in §7.2.2; no other depth/roulette gate suppresses the
attempt. It chooses a labeled
medium with q_m^V=W_m/Σ_nW_n and then an independent endpoint from p_m. This
attempt sets `competitionAvailable` even if the sample is inactive, occluded,
or zero-valued. Separately, the existing equiangular strategy pivots on a
positional-light/medium mixture with coefficients a_i and a_m. Entries use
explicit nonnegative **importance proxies with watt dimensions**, not claimed
exact powers: A_m=4πs²W_m for a medium (including §7.2.3's support inflation),
and A_i=`EstimateVisibleBandPower()` for a positional light, both using the
pinned 380–780 nm quadrature. Positional lights integrate their full spatial/
directional emission; spectral lights integrate their actual SPD, while legacy
RGB lights integrate the renderer's versioned RGB→spectrum reconstruction. The current
`MaxValue(radiantExitance())` weight is not comparable and is retired for this
mixed distribution. Only importance changes—the contribution remains spectral.
Set a_r=A_r/ΣA over all positional and medium entries. The s² factor cancels in
medium-only q_m^V but is mandatory in a_m when media
mix with lights. The flame thus joins as one more entry in this **equiangular
pivot** distribution, alongside the positional lights' existing power CDF,
and the activation gate widens accordingly:
`GetPositionalLightCount() > 0` becomes "positional lights *or* an
emissive medium present", else equiangular stays dead in exactly the
flame-only marquee case.
**The pivot scheme is Rao–Blackwellized** (revised per the external review,
which showed r5's hybrid — deterministic sums over positional pivots plus a
flame term conditioned on a pivot drawn only *when the flame is selected* —
is not the density of any actual sampler: given the selected entry R, the
true conditional distance density is 0.5·p_DT + 0.5·q_R, and the flame
pivot doesn't even exist on positional-entry draws, so the hybrid
denominator marginalizes the entry while conditioning on a sometimes-absent
pivot). The coherent scheme committed to:

- **The equiangular pivot and the NEE endpoint are DIFFERENT random
  variables, drawn independently** (second external round's catch — r6
  named both `y`, permitting one CDF draw to serve both roles, which
  breaks the fixed-conditioning proof): for each emissive medium m, an
  auxiliary pivot **u_m** is drawn unconditionally, once per path vertex,
  before volume NEE and before entry selection (one 7.2.3 CDF sample per
  medium — cheap); the NEE endpoint **Y ~ q_m^V·p_m** is then a *separate*
  draw. The pivot vector U = (u_1…u_N) is retained as shared random state
  for the vertex's NEE evaluation, the subsequent march segment's distance
  sampling, and every MIS weight on both sides — both the NEE-side and
  march-side weights condition on the same U, never on Y. Drawing a
  single pivot *through* the medium-selection pmf would reintroduce
  exactly the sometimes-absent-pivot conditioning this scheme removes.
- With U always in shared state, the equiangular technique density
  **Σ_i a_i·q_i(t) + Σ_m a_m·q_{u_m}(t)** (positional entries'
  deterministic sum plus each medium's flame term at its drawn pivot) is
  a genuine conditional density given U, evaluable on both the NEE and
  march sides — the weights-sum-to-1 argument applies pointwise at fixed
  U, and the deterministic-sum property over positional lights is
  *preserved*.
- The fully-conditional alternative (technique index = the selected entry
  R; only q_R in denominators) is recorded as the fallback — simpler but
  it abandons the deterministic positional sums.

The inherited near-collinear sampler is a Phase-B prerequisite, not trusted
as-is. Segment boundedness is an explicit flag from intersection/traversal—not
inferred by comparing L to the finite `RISE_INFINITY=DBL_MAX` sentinel. An
unbounded segment disables its equiangular technique before the technique roll:
p_march is pure DT, including its correct survival/no-event atom, and both
sampling and every MIS denominator use that same branch. No uniform density is
formed over DBL_MAX. On a genuinely finite segment, compute perpendicular D²
with a nonnegative robust difference and let
S=max(L,|pivot−rayOrigin|). If D≤32√ε_machine·S, both `Sample` and `Pdf` take
the same uniform-on-[0,L] fallback with density 1/L; otherwise both use the
same robust D² in the equiangular formula. The branch predicate is shared
code. Mixing a floored D in sampling with raw or negative D² in evaluation is
forbidden. On-ray, near-on-ray on both sides of the threshold, and large-scene-
scale tests require finite nonnegative samples/PDFs, numerical normalization,
and Sample/Pdf agreement. The metre-vs-centimetre gate also requires identical
q^V, `a`, and Sample/Pdf behavior after physical-power conversion. An
unbounded-global-medium fixture represented by the live DBL_MAX sentinel must
take pure DT and retain finite normalized/survival bookkeeping.

Note the equiangular 1/r² concentration argument weakens when the receiver
is *inside* the emitter; the delta-tracking half of the 50/50 pair covers
that regime — the point of keeping the pair.

**7.2.5 Multiple emissive media and nesting.** Each emissive medium carries
its own 7.2.3 structure and joins both distributions of 7.2.4: q_m^V among
emissive media for the dedicated NEE endpoint, and a_m among positional lights
plus media for equiangular pivoting. These labels and random draws remain
distinct. Two externally-reviewed refinements:

- **The labeled NEE density includes the selection factor**: choosing
  medium m with probability q_m^V gives p(y) = q_m^V·p_m(y), and every MIS
  weight uses that labeled density — not the per-medium p_m alone.
- **Innermost-exclusive support** (§5.1 `MediumTracking` row): RISE
  activates only the IOR-stack top's medium, so an outer medium's CDF —
  built from its full grid — can sample points where that medium is
  *inactive* (inside a nested object). The rule: such samples **return
  zero contribution without renormalizing** (the pdf is still the labeled
  q_m^V·p_m, so the estimator stays unbiased; support restriction at CDF
  build time via contained-object masks is a recorded optimization, not a
  correctness requirement).

NEE transmittance between x and y walks the full medium stack via the
§7.2.1 **replacement** segment-wise walk (the inherited walk is
disqualified there — round 4 caught this paragraph still citing it),
including an emissive medium nested inside another medium. One emission-CDF
structure per medium, with the distinct q^V and `a` normalizations of
§7.2.4 — no cross-medium voxel merging needed.

**7.2.6 Temporal invalidation.** Per-frame grids (G6) invalidate Φ every
frame; the rebuild is the O(N) pass above, scheduled with the majorant
rebuild in the between-renders frame-advance step (§10.3). No structure
persists across frames.

**7.2.7 Validation.** Flame-lit-smoke scene: NEE-on vs brute-force
(NEE-off, high spp) converging to the same image within MC noise, with
measured variance ratios per
[skills/variance-measurement.md](skills/variance-measurement.md); all
comparisons on pre-denoise EXRs (§10.5). MIS correctness stressed on the
canonical geometries: emitter enclosing the receiver, receiver inside the
emitter, thin emitter sheet at grazing angles, camera-primary direct view
(weight-1 rule), flame behind glass (march-only rule), a flame+point-light
scene (three-way strategy coexistence, 7.2.4), and a zero-soot chem-only
flame (methanol analog — exercises the §7.1 step 3 line integral with
σ_t ≈ 0 and no NEE). Structural gates: **7.2.3 CDF normalization**
(Σp_V·V = 1 over the emissive support; pdf lookup matches the sampled
distribution), knot-plane and outer-half-bin ownership, and one isolated hot
lattice sample: every center bin intersecting its complete nonzero trilinear
support must have q_v>0, including boundary bins with the declared per-channel
background corners.
The multi-media labeled-density check (7.2.5) remains. The point-light+flame fixture additionally
forces the equiangular label to the point light while confirming that the
independent volume endpoint was still attempted and march MIS used q_m^V;
on-ray/near-on-ray point pivots exercise the finite normalized fallback in
§7.2.4.
Also gated here: the **shadow-walk step-field tests** (>4 overlapping
media, >16 boundary crossings, the **wrong-origin interrupted-global-
medium case**, and **forced step-cap continuation** past the ratio
tracker's 1024-step budget — §7.2.1's no-silent-caps replacement walk)
and the **allowlisted Lambertian/Oren–Nayar/Isotropic-Phong NEE-on-vs-off
equality tests** (§7.2.2's direction-independent lobe preselection). Terminal total/path
depth and each per-type lobe cap are forced separately: volume NEE remains on,
the impossible continuation has p_march=0, and NEE-on/off high-spp references
agree. Mixed allowed+capped lobes additionally gate the f_A/f_D additive split,
roulette probabilities 0, (0,1), and 1, pdf normalization including its
survival mass, and recorded-proposal/throughput agreement. Using the uncapped
full-lobe marginal or omitting roulette survival is RED.
One- and multi-null-boundary homogeneous fixtures force the 50/50 sampler to
survive one and k segments before a background and before a downstream emitter;
both must match pure DT. Retaining T_det without dividing by P_0 is the explicit
`2^{-k}` RED control.
An `IsotropicPhong` diffuse+glossy fixture assigns unequal per-type caps and
requires the immutable closure's enumerated selection masses, subset
Evaluate/Pdf, sampled frequencies, and IOR/path-state transitions to agree in
Pel and NM. Grazing and glint-tilted Lambertian/Oren/Phong cases integrate the
successful gated continuous density and add the measured horizon-null mass to
exactly one; sampled null frequency must agree, and retry/renormalization or
double-applying h through roulette is RED. An aggregate-BSDF or post-Scatter adapter is RED. Unsupported
plugin material and built-in `CompositeSPF` preflight must reject predictive
mode before sampling; their preview fallbacks run NEE-on/off equality with
collision weight 1. A custom `IMaterial` returning an allowlisted SPF with a
mismatched BSDF must also reject; both luminaire wrapper delegates and the clay
factory must prove response/sample/Pdf identity. An isolated smoke-scatter receiver gate repeats NEE-on/off
with volume guiding requested on/off, terminal volume/path caps, and RR survival
0, intermediate, and 1; at a competing medium vertex the effective guide α and
guide sample count must remain zero and the retained phase-closure Pdf must
match every recorded p_ω,reach. Separate
diffusion-profile and random-walk SSS preview fixtures assert zero volume-NEE
attempts at BSSRDF entry, a fresh child competition state, and NEE-on/off
agreement; enabling SSS in predictive mode must return
`sss_volume_nee_unsupported`.
Equivalent fixtures cover nested calls from both named SSS shader ops and
unknown-dependency fail-closed preflight.
**Configuration matrix** (second external round): every equality gate
above re-runs across guiding on/off, RIS enabled at *non-competing*
vertices on/off (RIS at competing vertices is forbidden outright — an
"on" configuration there is not an expected-equality case, round 4),
null boundaries present, opaque occluders present,
nested media, and `casts_shadows`/`transparent_shadows` toggles — the
§7.2.2 support rules are exactly the places where a flag combination can
silently break the partition.

### 7.3 Phase C — the simulator (Category-1 deliverable)

Standalone offline tool under `tools/` (own executable, not linked into the
renderer; **its build wiring — at minimum the Unix Makefile, plus the other
build projects if it ships beyond dev use — is part of the work item**, as
the five-project checklist covers `src/Library` only): §3's solver. Writes
per-frame multi-channel grids + probe time series (§8). Renderer side:
grid-sequence loading + per-frame majorant/CDF rebuild (G6), wired into the
existing animation render workflow, plus the §8 manifest/preparation lifecycle
and portable output-provenance plumbing through `FrameStore`, file encoders,
AOVs, and animation/MOV finalization. This includes deterministic build/scene
identity emitters and the manifest digest verifier (§8's integrity contract —
declared fields plus SHA-256, deliberately *not* an authentication system);
the all-built-in mutation-sink/freeze capability and `IRayCaster`
light-sampler invalidation seam in §10.3. Digest verification tests cover
altered payloads, truncated frames, and mismatched declared/actual content.
Progression: laminar candle (DNS) →
puffing pool fire → turbulent plume, gating each on §3.8.

**Frontends.** The standalone §3 simulator is the only first-class predictive
Phase-C source. Blender/Mantaflow interoperability is explicitly
**non-predictive preview** by default: its normalized `flame` and `temperature`
grids do not contain an invertible mapping to W/m³ and kelvin, and float
transport fixes quantization rather than restoring that lost scale. Such an
import is tagged `source_kind=heuristic_import` and
`physical_mapping=heuristic:<profile_id>`; the profile is a named versioned
preview record owned by the importer and must supply the complete normalized
field→physical-channel mapping required by §8 loadability. It cannot use a
versioned predictive optical/chem qualification or pass §3.8's absolute gates. A future
exporter may qualify only by writing absolute T, carbon/condensed mass,
reaction plus band-resolved chem power, velocity, and the complete §8 sidecar at the simulation source;
the importer then performs round-trip unit/hash tests without fitting an
affine scale in RISE. The existing bridge's 8-bit path remains disqualified.
Mac/Windows GUI and Android are explicitly deferred. Phase-C sequence rendering
initially exists only in desktop CLI builds compiled with the OpenVDB
capability; other builds recognize the scene chunk but hard-error with a named
missing-capability diagnostic before loading. No portable frame encoding is
promised by this arc; native UI and a separately specified fallback container
may follow once the format stabilizes.

### 7.4 Phase D — fidelity polish (severable; prioritize after review)

Hero-wavelength spectral media via spectral tracking (G5 — must land without
breaking Phase A's per-λ fallback semantics), **including the G11
companion-proposal fix in `PathTracingIntegrator`'s HWSS branch** (every
companion divides by the actual hero/bundle proposal — the precondition for
re-enabling HWSS on fire media); LLJ or flamelet-tabulated soot
upgrade (gated per §3.4); RDG-FA / tabulated-Mie / Draine phase functions
(G7); embers and sparks content path (§4.6, incl. the char-oxidation
brightening term); sparse grids; residual-ratio-tracking control variates;
eikonal ray bending for shimmer (G8).

---

## 8. Sim → renderer data contract

**Channels and precision** (all in **physical units**; normalize-by-max
anywhere in the pipeline is a contract violation):

| channel | quantity | units | precision |
|---|---|---|---|
| `carbon` | c_carbon (total soot-derived carbon aerosol, §3.4 — the renderer derives f_v,hot = φ(T)·10⁻³·c_carbon/ρ_soot and the complementary cool-carbon extinction; both absorptive parts feed §4.2 emission) | **g/m³** (dilute aerosol at 10⁻⁶–10⁻⁴ kg/m³ would trip fp16 subnormals if stored SI; scale recorded in metadata) | **fp32** (feeds absorption/emission through the exponential Planck path) |
| `temperature` | T | K | **fp32** (Planck is exponential in T) |
| `reaction` | q̇‴_gas (§3.3), diagnostic only | W/m³ | **fp32** (spans to ∼10⁸; overflows fp16's 65504 max) |
| `chem_CH`, `chem_C2`, `chem_CO2` | band-integrated radiant source powers q̇‴_b (§3.3/§4.4); all three required when chem is enabled | W/m³ | **fp32** (absolute radiometric channels; zero is meaningful) |
| `condensed` | c_condensed (condensed organics, §3.4) | **g/m³** (same scaling rationale) | fp16 acceptable |
| `velocity` | u | m/s, **cell-centered collocated** (resampled from MAC staggering at write) | fp16 acceptable |

**Majorant support:** per-block **min/max (8³ or 16³ tiles)** per channel —
computable in one cheap leaf-manager pass over an OpenVDB grid (or read
directly from NanoVDB per-node stats; core OpenVDB does *not* store per-leaf
min/max as resident metadata — revision 2 overstated this). The renderer
aggregates blocks to its majorant-grid cells (a trivial min/max reduction —
block size and cell size need not match). **The blocks serve extinction
majorants only**: the Phase B emission CDF cannot be built from min/max
bounds (ε is nonlinear in (f_v, T) and the CDF needs sums, not bounds) —
it requires the O(N) per-bin pass of §7.2.3, which runs once per frame in
the frame-advance step. (Revision 3 claimed the CDF avoided a
full-resolution pass; that contradicted §7.2.3 and was wrong.)

**Metadata completeness rule:** two physically different grids must never
carry indistinguishable metadata. Each sequence stores:

- a versioned sequence manifest carrying sim Δt/frame rate. The
  manifest contains the complete time map
  (t₀,α,t_scene,0,Δt_frame,i₀), an ordered explicit frame-index/file list
  (paths are UTF-8 NFC and relative to the manifest directory), frame count,
  first/last indices, SHA-256 digest for every frame, `end_policy`, velocity-
  halo width, and outside-halo policy. The wire format is the application
  profile **RISE-CBOR64-v1**: RFC-8949 CBOR with definite lengths only,
  shortest integer encoding, UTF-8 NFC text, **unique text-string map keys
  only** (non-text or duplicate keys are invalid), maps sorted lexicographically by
  encoded UTF-8 key bytes, and every float encoded as big-endian IEEE-754
  binary64 with −0 normalized to +0 (NaN/Inf forbidden). This intentionally
  does not claim RFC's shortest-float core deterministic profile. A loader
  validates the raw token stream for definite lengths, key type/uniqueness,
  ordering, NFC, numeric width, and all other profile rules **before semantic
  field decoding**; noncanonical bytes are rejected rather than normalized.
  The same rules apply recursively to sequence manifests, every embedded/
  override record, output provenance, and every build record. The top-level
  manifest envelope is exactly `{payload, sequence_id}`.
  `payload` contains
  every semantic field listed in this section, including
  `schema_version=1`, but contains no `sequence_id`; `sequence_id` is SHA-256
  over the exact canonical RISE-CBOR64-v1 byte encoding of `payload` alone.
  This avoids a self-hashing manifest while giving every implementation one
  preimage. Producer qualification travels as declared `payload` fields
  (`source_kind`, `physical_mapping`, producer build ID, gate-evidence IDs) —
  covered by `sequence_id` and therefore tamper-evident, not tamper-proof; see
  the integrity-vs-authenticity note below. The complete envelope is then encoded with the same profile and
  media type `application/vnd.rise.fire-sequence+cbor`. Unknown versions are rejected. The scene locates
  this file explicitly with `sequence_manifest`; no frame-pattern discovery is
  performed. For this arc `end_policy` is `hold` or `error`. Let
  i_first=i₀, require a nonempty strictly increasing **contiguous** unique index
  list, frame_count=list length=i_last−i₀+1, and finite Δt_frame>0. Then
  t_first=t₀ and t_end=t₀+(i_last−i₀+1)Δt_frame. `error` rejects mapped times
  outside the half-open authored support [t_first,t_end). `hold` clamps to the
  closed evaluation interval [t_first,t_end], with i=min(i_last,
  i₀+floor((t_eval−t₀)/Δt_frame)); at t_eval=t_end the base is i_last and the
  advection offset is exactly Δt_frame. The nominal mapped time is always
  checked/clamped. Shutter endpoints are mapped and checked **only for a medium
  whose requested velocity blur survives capability/fidelity preflight**. A
  blur-off medium samples only its nominal base frame, so camera exposure or
  rolling scan outside sequence support cannot reject an otherwise valid final
  frame. For blur-on + `hold`, clamp each endpoint before forming §8's
  advection offset; for blur-on + `error`, an out-of-support endpoint is a
  predictive error, while preview may disable that medium's blur, record
  `blur_time_support_out_of_range`, and revert to nominal-only checks before
  choosing the render-global estimator mode. An index
  inside the declared range but absent from the explicit list, or any digest
  mismatch, duplicate path, inconsistent channel set/type, or invalid cadence/
  index invariant is a hard manifest-load error, not a lazy render failure;
- one canonical, deliberately axis-aligned coordinate contract plus a per-
  channel lattice descriptor, fixed across frames. Integer index (i,j,k)
  denotes the **voxel center**
  x_m=o_m+(i,j,k)⊙Δx_m in a right-handed SI-metre sequence frame; the core
  face bbox is [o_m−Δx_m/2, o_m+(N−1/2)⊙Δx_m]. Placement is
  x_scene=t_scene+x_m/s for `scene_unit=s` metres and a scene-space translation
  t_scene only. Rotation, reflection, shear, and extra/nonuniform scale are
  rejected in this arc (a future inverse-transform accessor can add them).
  Velocity axes are therefore unchanged and u_scene=u_m/s; it is never
  transformed as a point. Each channel descriptor stores o_m, Δx_m,
  dimensions, core bbox, explicit `background_value`, and the §8
  `temporal_semantics` enum.
  Carbon/temperature/condensed share the exact extinction lattice; reaction and
  all three chem-source channels share one separately declared knot lattice;
  velocity uses the declared core
  lattice plus its halo. Every VDB embedded transform, resolution, bbox,
  channel type, handedness, and topology contract must match its manifest descriptor after
  deterministic decode; the manifest is authoritative and mismatch is a load
  error. The simulator's sequence writer (or a separately qualified external
  exporter) is the only place deterministic resampling may occur;
- every OpenVDB grid's stored background must bit-match the manifest descriptor
  after deterministic decode: temperature uses the sequence's T_inf in kelvin;
  carbon, condensed, reaction, and chem sources use +0. **Every stored value-off
  tile/leaf voxel must bit-equal that declared background**; ordinary OpenVDB
  value lookup without an activity test is forbidden. Inactive voxels inside
  the velocity halo use (0,0,0) m/s for this quiescent-ambient arc before the
  separately declared outside-halo policy applies. Inactive voxels inside
  the core bbox take their declared background, while points outside the medium face bbox are
  outside the medium. Backgrounds participate in channel extrema/domain checks,
  U_v construction, and the frame digest; a zero temperature background or an
  active-only extrema scan is a load error. Boundary-bin fixtures exercise an
  inactive T_inf corner, zero material corners, and traversal immediately
  outside every face;
- before trusting any per-node extrema or building a majorant/CDF, preflight
  scans **every active voxel/tile value, every stored value-off voxel/tile, plus
  every declared background** after decode. A value-off payload unequal to
  background is an unconditional load error even if ordinary topology iteration
  would skip it. All scalar/vector components must be finite;
  carbon, condensed, reaction, and all `chem_*` values must be ≥0; temperature
  must be >0 and inside the common certified Planck/optical/thermochemistry
  domain; velocity components may have either sign but must be finite. These are
  unconditional loadability constraints in predictive and preview modes:
  reject the frame, never clamp, deactivate, or repair it. The scan computes
  trusted extrema used by subsequent domain/majorant work; file hashes and VDB
  metadata statistics are not substitutes. RED fixtures isolate active voxels
  and constant active tiles containing a negative value, NaN, +Inf, −Inf, zero/
  out-of-domain T, and a nonfinite velocity component, plus value-off leaf/tile
  payloads containing hot, negative, or NaN data;
- per-fuel y_form, y_s, y_cond, χ_r, T_pilot/T_AIT, Q̇_ref, ρ_soot,
  oxidation constants (T_ox, 2.667 kg-O₂/kg, 3.667 kg-CO₂/kg, 32.8 MJ/kg),
  channel scales, calibration dataset/protocol IDs, and scale/resolution
  cross-prediction results;
- the complete `gas_thermochemistry` record/hash from §3.3, including all
  species/inerts, E, W/c_p/h_s/T_ref, ambient/fuel composition and temperature
  (T_inf included), p₀, LHV, and
  atom-balanced primary products;
- the `transport_closure` record/hash from §3.2: μ/k tables and mixture laws,
  unit-Lewis D rule, Pr_t/Sc_t, Vreman C_v/filter widths, wall treatment,
  τ_chem, T_CFT, and every certified property domain;
- `aerosol_humidity_model = none`, the χ_load≤1 % support flag, and the
  condensable record (formula/W_cv, referenced gas/aerosol thermochemistry IDs,
  Δh_cv, s_cv, L_cv, T_sat,ref, p_sat,ref, and chem-HRR convention);
- the `aerosol_thermochemistry` record/hash for carbon and condensed phases,
  including c_p/h_s/common-T_ref, interpolation, provenance, and certified
  domains;
- one versioned optical record: E(m) ID/hash shared by sim and renderer,
  separate cool-carbon and condensed-organic full-spectrum k_m/n/ω/g data,
  their IR extensions, the φ(T) band, differentiable interpolation, and the
  derivative enclosures required by §3.5. Every optical/chem/thermochemistry/
  transport record is an independently schema-versioned RISE-CBOR64-v1 byte
  string embedded **verbatim** as a manifest byte string; its record ID is
  SHA-256 of those exact bytes. An override file contains exactly that record
  byte string, so equality has one preimage (not wrapper or parsed-map
  equality);
- the **simulator-only gas-opacity record**: CO₂/H₂O table ID/hash,
  wavelength/T ranges, p₀, composition basis, pressure/broadening convention,
  interpolation and overlap rule, plus the 380–780 nm negligibility bound;
- the chem record or explicit `chem_model=none`: the state-dependent η_b
  functions/domains, each S_b dataset ID, wavelength units and normalization
  interval, absolute band/spatial calibration evidence, or the measured
  negligible-chem upper-bound record required by §7.0;
- an embedded canonical RISE-CBOR64-v1 `producer_build_v1` byte string pinning simulator source
  revision, executable/module SHA-256, dirty flag plus deterministic dirty-diff
  hash, schema/solver and
  gate-harness versions, compiler and floating-point/fast-math/contraction
  settings, target platform/architecture, and exact dependency versions plus
  loaded-binary hashes, with
  **producer_build_id=SHA-256(exact producer_build_v1 bytes)** recomputed by the
  loader before any manifest-field comparison; and
- producer-owned `source_qualification=predictive_qualified|preview_only`, a
  stable `producer_reason_codes` list, and the exact producer gate-evidence
  record IDs; claimed `source_kind`, whose enum is `rise_simulation`,
  `qualified_external`, or `heuristic_import`, and `physical_mapping`, whose
  value is `absolute_si` or `heuristic:<profile_id>`. The manifest never stores
  the scene's requested render mode or a renderer-derived final status;
  `sequence_id` is a content-integrity digest that transitively covers the frame
  digests and verbatim record blobs above; it is **not authentication**.

Producer qualification is a **declared, integrity-checked manifest field, not
an authenticated one.** The envelope carries `source_kind`
(`rise_simulation` | `qualified_external` | `heuristic_import`),
`physical_mapping` (`absolute_si` | `heuristic:<profile_id>`), the producer
build ID, the producer's own gate-evidence record IDs, and `sequence_id` — a
content-integrity digest transitively covering the frame digests and verbatim
record blobs. **Predictive fidelity requires `source_kind` ∈ {`rise_simulation`,
`qualified_external`} together with `physical_mapping=absolute_si`;
`heuristic_import` or any heuristic mapping is preview-only and stamps
`producer_unqualified`.** A statically authored medium (§9's
`multichannel_heterogeneous_medium`, which has no manifest and therefore no
declared `source_kind`) is `producer_unqualified` by construction — correct
for Phase A, whose gates are analytic rather than predictive. A digest
mismatch anywhere is a load error
(`integrity_digest_mismatch`), never a silent downgrade.

This is deliberately an **integrity** contract, not an authentication one. It
solves the failure this arc actually has — a normalized Mantaflow or otherwise
heuristically-scaled grid being rendered as if it were absolute SI — and it
solves it with a declared field plus a hash. Cryptographic producer
attestation (signed qualification records, an operator-owned trusted-key
registry, key rotation, revocation epochs, anti-rollback transactions) assumes
an adversary who forges provenance; nothing in §1 posits one, and adding that
machinery here would make a fire-rendering design own a repo-wide trust
system. If RISE later needs authenticated provenance it belongs in its own
design, and this contract's `source_kind`/`physical_mapping`/digest fields are
exactly the fields such a scheme would sign.

Every tabulated/polynomial record has a **closed certified domain** over all of
its arguments. Predictive simulation rejects the stage before accepting any
cell lookup outside that domain—no clamp or extrapolation. Preview may proceed
only with a versioned extrapolation rule named in the record and adds
`table_domain_exceeded`; the offending record/cell/range is logged. Q5/Q6 and
the transport record cannot close without these policies and out-of-domain RED
fixtures.
Renderer preflight applies the same rule before workers launch: it checks the
actual NM wavelength support (or every Pel projection quadrature node) and the
loaded temperature/composition channel extrema against optical/chem record
domains. Predictive mode rejects any possible out-of-domain lookup. Preview
may extrapolate non-grid axes only when that exact record embeds a versioned
rule and records
`table_domain_exceeded`; worker-time clamp/extrapolate/error choices are
forbidden. Decoded grid temperature is the unconditional §8 loadability gate
above and is never extrapolated in either mode.

**Loadability precedes fidelity.** Invalid schema/version/lattice or any
declared-content digest mismatch (`integrity_digest_mismatch`),
missing temperature/carbon/condensed or other configuration-required channel,
or absence of a record needed to evaluate the selected path (optics always;
chem when enabled; velocity when blur is enabled) is an unconditional load
error in both modes. Integrity errors produce no render artifact and therefore
never appear as a fidelity/provenance reason; `qualified_record_override` is
reserved for a valid, self-consistent replacement record with a different
identity, not corrupted bytes. "Preview" never invents a missing coefficient. A
`heuristic_import` is loadable only when `physical_mapping` names a versioned
preview profile containing the complete normalized-field→T/aerosol/reaction
plus band-resolved-chem mapping and optical/chem fixtures; otherwise it is also
a load error.

**Fidelity ownership and transition table:** the producer alone derives
`source_qualification` from simulation/import gates, and the renderer verifies
the manifest's declared fields and digests (§8's integrity contract). The
scene/job
alone supplies requested `fidelity_mode`. On every render the renderer ignores
any producer claim of final render status and derives `render_fidelity_status`
plus render reason codes afresh from the declared producer evidence and the
actual integrator, records, overrides, channels, domains, blur fallbacks, and
**primary** output route. `predictive` is permitted only for the
`rise_simulation` or validated `qualified_external` with `absolute_si`, spectral NM transport, matching
frozen optical, chem, condensable, gas thermochemistry, aerosol
thermochemistry, transport-closure, and simulator gas-opacity records, intact hashes, all required
channels, χ_load≤1 %, dry-aerosol scope, a file-loaded scene identity, and
passed predictive gates. A
heuristic/normalized source, a structurally valid whole-record override whose
canonical bytes/identity differ from the embedded qualified record, Pel or
HWSS transport, use of a complete named heuristic preview profile,
out-of-scope loading/water, or
an unqualified `chem_model=none` produces the corresponding preview reason.
When the requested mode is predictive, `source_qualification=preview_only` or
any render/primary-artifact reason is a hard preflight
error; when it is preview the render proceeds with all reasons embedded in
output provenance. Velocity blur is predictive only on the §8 spectral-NM
pure-DT path; Pel's nominal-frame fallback remains preview. The simulator
can therefore qualify only its source evidence, never a downstream render.
Until separately qualified in Phase D, predictive fire preflight also requires
`oidn_denoise=false`, no radiance/contribution clamp, and no path
regularization, and **`sms_enabled=false`**. The spectral PT rasterizer's
internal SMS photon map is distinct from Scene photon maps, and the current
`SMSConfig::biased=true` default is not predictive merely because the outer
integrator is spectral PT; any enabled SMS mode yields `sms_unqualified` until
SMS is separately qualified. The integrator must produce unclamped raw linear spectral
radiance before exposure. The **predictive primary artifact** must be a
losslessly encoded fp32-or-wider scene-linear spectral/XYZ/RGB container (EXR
ZIP/PIZ/uncompressed are permitted; DWAA/DWAB and integer/LDR containers are
not), with only the recorded scalar exposure applied and no white balance,
tone/display transform, clipping, integer/display quantization, or lossy
chroma/compression. Enabling any of those **on the primary route** rejects
requested predictive mode and yields the corresponding primary-artifact reason
only when preview was requested.

Secondary display output is a separate postprocessing transaction, not part of
predictive preflight. It starts only after the lossless primary artifact and
sidecar are finalized, and may tone map/white-balance/quantize/compress into an
artifact with `artifact_fidelity=display_derivative`, artifact-local reasons,
and the tagged linkage below. Those
secondary reasons neither enter `render_reason_codes`, downgrade, nor invalidate
the linked primary; derivative failure leaves the primary valid. Conversely no
derivative is emitted if the primary transaction failed. The derivative may
record `render_fidelity_status=predictive` as the status of the upstream Monte
Carlo result, but its own artifact fidelity is always explicitly
non-predictive—never `predictive_primary`. §3.8 radiometric gates read the raw
linear NM result before exposure.

Derivative linkage is a tagged schema, not singular by assumption. A still/AOV
uses `derivation_kind=single_primary`,
`derived_from_primary={provenance_id,artifact_sha256}`, and an empty
`derived_from_frames`. A movie uses `derivation_kind=frame_sequence`, null
`derived_from_primary`, and canonical
`derived_from_frames=[{frame_index,provenance_id,artifact_sha256},…]`, ordered by
strictly contiguous unique frame index and required to match the encoded movie's
frame order/count. The whole array is inside the movie provenance-ID preimage;
mixing the two variants or omitting a frame artifact digest is invalid.

`render_reason_codes` are schema-v1 enum strings, emitted once each in lexicographic
order: `requested_preview`, `producer_unqualified`, `heuristic_source`, `qualified_record_override`,
`missing_optical_record`, `missing_chem_record`, `chem_none_unqualified`,
`missing_condensable_record`, `missing_gas_opacity_record`,
`missing_thermochemistry_record`, `missing_aerosol_thermochemistry_record`,
`missing_transport_record`,
`table_domain_exceeded`, `missing_channel`, `loading_exceeded`,
`wet_aerosol_unsupported`, `pel_transport`, `hwss_transport`,
`pel_blur_ignored`, `blur_halo_insufficient`, `blur_time_support_out_of_range`,
`nonadvected_source_blur_unsupported`, `keyframed_temporal_sampling_unsupported`,
`programmatic_scene_unqualified`, `unrepresented_scene_mutation`,
`untracked_scene_mutability`,
`oidn_unqualified`, `radiance_clamp_enabled`, `path_regularization_enabled`,
`sms_unqualified`,
`continuation_closure_unsupported`, `sss_volume_nee_unsupported`,
`gate_failure`, and `output_provenance_unavailable`.
`artifact_reason_codes` use the separate enum `lossy_output`,
`display_transform_enabled`, `integer_output`, `white_balance_enabled`, or
`artifact_provenance_unavailable`; they are empty for a predictive primary and
artifact-local for a derivative.
A fully valid explicitly preview-requested
render carries only `requested_preview`; codes are never free-form log text.

**Output provenance: a manifest, not a transaction.** Every predictive still,
animation frame, and AOV artifact writes a sibling provenance sidecar (and
repeats the same fields as EXR attributes where supported) recording: the
`render_fidelity_status` with its reason codes; the `active_fire_media` array
keyed by `(manager_name, binding_kind, binding_owner)` carrying each medium's
sequence ID, selected base-frame index, whole-file digest, `source_kind`,
`physical_mapping`, and effective blur state; the effective render
configuration after default resolution (film/camera/integrator/sampler/depth/
clamp/filter/AOV/output settings — a render-affecting parameter added without a
schema update is a test failure); the renderer build identity (source revision,
dirty flag, feature and dependency versions); and the artifact's own SHA-256,
which lives in the sidecar only, since embedding it in the artifact would
change the bytes being hashed. MOV outputs are always `display_derivative`
linked to their EXR primaries — the encoders in scope are lossy and
display-referred — and carry the `derived_from_frames` digest array. A
predictive primary fails if its sidecar cannot be written.

The purpose is *reproducibility and honest labeling*: given a sidecar you can
tell exactly which grids, build, and settings produced an image, and whether
its fidelity claim was predictive or preview. It is deliberately not a
distributed-transaction or attestation system (see the integrity-vs-authenticity
note above). Round-trip tests cover each output route and verify the artifact
digest, sequence hash, and fidelity reasons.

**Render-preparation dependency.** Phase C's per-frame grid/majorant/CDF swaps
require that scene and render-configuration state be *frozen* for the duration
of a render, with mutations tracked and rejected under the freeze
(§10.3). That mechanism — mutation sinks, prepared inputs, freeze tokens,
generation counters, and the artifact-finalization seam — is a renderer-wide
concern well beyond fire, and is specified in
[RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md). This arc
depends only on: (a) the freeze seam existing on the render entry points, (b)
grid/majorant/CDF swaps occurring strictly between renders, and (c) a scene
mutation during a render being detected rather than silently producing a
mixed-state image.

**Validation probes:** alongside frame grids, the sim writes **sim-rate
centerline probe time series** (T, q̇‴_step, u at fixed stations) for the §3.8
harness, so puffing FFTs and plume fits do not reconstruct signals from
frame-rate grids. **Every probe sample carries its timestamp** (the solver
uses CFL-varying steps, so a nominal Δt cannot define the sampling times);
the harness resamples uniformly before FFTs.

**Scene units** (external-review catch — SI radiometry was never connected
to RISE's scene scale): with one scene unit = s metres, positions scale as
x_scene = x_m/s, and every physical medium quantity converts at
construction — σ_scene = s·σ_SI, ε_scene = s·ε_SI, u_scene = u_SI/s; the
physical CDF power is s²·ΣV_scene·∫ε_scene dλ (the constant cancels in the
pmf but is required for the W/sr claim and cross-medium comparisons).
`scene_unit` today governs only the camera mm conversion
([parser README](../src/Library/Parsers/README.md):~177, which already
names it the future home for physical-quantity scaling) — fire media make
it real: the scale moves into job/scene state readable by every physical
medium, and the gate is the §7.1 metre-vs-centimetre slab-invariance test.

**Format:** OpenVDB only, native in-core in desktop builds behind an optional
capability. The manifest pins `frame_encoding=openvdb-v1`; each listed frame is
one exact OpenVDB file whose whole-file bytes are the frame-digest preimage and
whose named grids/types/transforms must match the manifest. There is **no
portable raw/dense fallback in this arc**: defining one would require a second
container, endian/layout/compression/channel-packing contract that does not
exist in RISE today. A build without OpenVDB rejects the chunk at preflight.
Dense transcode of 512³ multi-channel frames at ≈3.8 GB/frame is not a credible
general pipeline; the OIDN submodule is the optional-extlib precedent, and the
native choice adds `scripts/create_macos_release.sh` staging work per that
precedent. Tiny tests generate OpenVDB fixtures when the capability is present
and skip only with an explicit capability result, never by silently selecting a
different encoding.

**Optional motion blur:** velocity-grid Eulerian blur with an explicit consistency
rule: **one shutter-time sample per camera path**, with density, emission,
transmittance, *and majorants* all evaluated at that same time — mixed-time
lookups make Tr and ε disagree and alias. Lookup is backward
semi-Lagrangian (x − u·(t_s,sim − t_i), signed — shutter samples fall on
both sides of frame time); first-order, smears under rotation at large CFL —
accepted and documented. **Path-time carrier** (external-review catch):
neither `RuntimeContext` nor `Ray` carries a sampled path time today, and
the existing temporal path mutates the Animator per sample — which §10.3
forbids for grids. A **read-only per-path time** field is added to the
per-thread `RuntimeContext` and threaded through every primary,
continuation, shadow, medium, CDF, and NEE lookup; frame indexing is
pinned with an explicit **scene-time → sim-time map** (second external
round — "frame 0 at t = 0" never connected RISE scene time to the SI
seconds velocity and sim cadence live in):
t_sim = t₀ + α·(t_scene − t_scene,0), frame index
**i = i₀ + ⌊(t_sim − t₀)/Δt_frame⌋** (third external round — the r7 form
⌊t_sim/Δt_frame⌋ hit frame zero only when t₀ = 0), with
(t₀, α, t_scene,0, Δt_frame, i₀) serialized together in the §8 manifest,
**t_scene,0 a fixed sequence epoch stored in scene/sequence state** (round 4: defining it as
the render-range start makes the same scene time select different grids
in full-animation, subrange, and single-frame renders — gated by an
identical-frame-selection test across all three entry modes), and α
defaulting to 1 (fire-sequence timelines authored in seconds); apply the
manifest's exact `end_policy` to nominal time always and to shutter support
only when blur remains active, per the manifest rule above, before computing
any shutter advection offset; a
missing/digest-mismatched frame in the explicit manifest is
a hard load error, not a skip. The prepared fire-render path **must not call
`IAnimator::EvaluateAtTime` from a worker or per sample at all**. Until immutable
per-path scene snapshots plus conservative swept TLAS bounds exist, any active
keyframed camera, object transform/bounds, material, light, or other
render-affecting animator whose state would vary over the computed path-time
support is a predictive preflight error
`keyframed_temporal_sampling_unsupported`. Explicit preview holds all such
scene state at nominal time, records that reason, and may still apply the
read-only fire velocity warp; it does not fall back to the racy legacy sampler.
Static animators/properties that prove identical state over the entire support
are allowed. Moving-camera, moving-occluder, and animated-material RED fixtures
assert the rejection/nominal hold and TLAS consistency.
**Residency model, pinned — with nominal and shutter time separated**
(round 5: introducing the index via per-path t_sim conflicted with
single-base residency): the render's **nominal** simulation time —
mapped once per render from the nominal scene time — selects the base
frame i once, with frame epoch t_i = t₀ + (i − i₀)·Δt_frame; one
immutable base grid + majorants + emission CDF for frame i is loaded in
the frame-advance step (§10.3). Each path's sampled shutter time maps
separately, t_s,sim = t₀ + α·(t_s,scene − t_scene,0), and the path
**advects the base by the offset (t_s,sim − t_i)** — an SI-seconds
offset, so α ≠ 1 scene-time deltas are never multiplied into SI velocity
un-converted. Shutter intervals that cross frame cadence do NOT load
neighboring frames (the advected single frame is the stated
approximation, valid for Δt_shutter ≪ Δt_frame; a shutter longer than
the frame interval is rejected with a diagnostic). Here "shutter interval"
means §10.3's full sign-aware path-time support over exposure + scanline +
pixel offsets for the active crop/field, mapped through α—not exposure alone.
Gates: α ≠ 1,
subframe nominal times, and cadence-crossing shutters. Majorants must
bound the field *over the shutter interval*: blocks are dilated by the
**global** max|u|·max|t_s,sim − t_i| at build time (loose but safe;
per-block dilation is a measured optimization) — **and the medium's
traversal AABB is expanded by the same displacement bound**
(`ClipDistanceToBounds` clips to the base AABB today, §5.1) — **and AABB
expansion alone is insufficient** (third external round): a traversal
point outside the base grid looks up velocity *zero* there, so backward
advection can never recover density that moved outward across the
original bounds. The sim therefore exports the velocity grid with a
**padded halo sized for the full frame interval**
(max|u|·Δt_frame — the sim cannot know the camera shutter at export;
round 4), with the **available halo width and the outside-halo
extrapolation policy serialized in metadata**; at render time, if the
shutter's mapped-SI displacement bound
max|u|·max|t_s,sim−t_i| (with shutter duration |α|Δt_scene) exceeds the
available halo the renderer
raises a **predictive preflight error** whenever nonzero blur was requested.
Preview may disable blur for that medium only with
`blur_halo_insufficient` in provenance; the render-global NEE/equiangular
decision is made after preflight from the media that actually remain blurred.
It never silently extrapolates. Lookup order pinned as clamp-to-halo-then-sample;
gates: a constant-velocity slab translating across the base AABB with
integrated emission/extinction preserved, plus exposures bracketing the
halo limit.
Sufficiency:
a backward semi-Lagrangian resample can only *displace* values for
**monotone (trilinear) interpolation** — the claim is false for
Catmull–Rom, whose non-convex basis overshoots local extrema (the majorant
grid already carries the 756/512 ≈ 1.4766 overshoot factor + 2-cell
dilation for exactly this, §5.1). Rule, tightened across two external
rounds: **ALL fire channels are trilinear.** Velocity because it is
signed — Catmull–Rom's 1-D absolute-weight sum peaks at 20/16 at μ = 0.5,
giving a separable 3-D signed-vector bound of (20/16)³ ≈ 1.953, so the
scalar 1.477 factor under-expands AABBs and majorants. And the physical
scalar channels because Catmull–Rom **undershoots as well as overshoots**:
`VolumeAccessor_TriCubic` applies no clamp (~:42), so tricubic
carbon/condensed could go *negative* — negative extinction yields Tr > 1
and invalid tracking probabilities — while clamping would break the
polynomial-exactness that `EvalDeterministicOpticalDepth`'s quadrature
and the §7.1 pure-DT reduction rely on (third external round). A monotone
positive cubic is the recorded upgrade path if trilinear proves visually
insufficient; naive tricubic is excluded for physical channels outright.

**Per-channel temporal semantics are mandatory.** Each manifest channel names
`temporal_semantics=frozen_material_advection|derived_eulerian_source|static`.
The simulator may qualify carbon, condensed organics, and temperature for the
stated first-order frozen-material approximation; it marks `reaction` and all
`chem_*` channels as `derived_eulerian_source`. Arbitrary
`MediumCoefficients::emission` is likewise a source, not a material scalar.
Velocity warping is predictive only if every render-consumed channel in that
medium is `frozen_material_advection` (or `static`). Gate on declared semantics/
presence, **not nominal voxel values**: a render-consumed
`derived_eulerian_source` channel or configured ε_add with requested blur is a
predictive preflight error even when its base grid is identically zero. This arc
defines no producer zero-over-shutter certificate exception;
preview disables blur for the **whole medium**, records
`nonadvected_source_blur_unsupported`, and evaluates every channel at nominal
time. Warping some coefficients but leaving a source nominal is forbidden.
A future bracketed-frame/source-evolution model can add another qualified
semantic; this arc does not infer Dq/Dt=0 from the presence of velocity. RED
gates include a stationary reaction sheet embedded in moving gas (must not
translate) and a time-resolved moving sheet (must trigger the unsupported path,
not pass a self-fulfilling advected-slab reference), plus a zero base chem grid
whose next frame ignites during shutter support (must still reject/disable).

One hard, render-global interaction rule removes every ambiguous MIS state:
**if any active fire medium uses velocity blur, the renderer disables the
entire volume-NEE and equiangular families for that render.** It draws no
pivot U or volume endpoint Y, sets `competitionAvailable = false` at every
vertex, defines p_V = 0, and gives every march-collected thermal-emission hit
weight 1. All medium distances use pure per-wavelength delta tracking. This is
deliberately render-global rather than per-medium: a static emitter connection
may cross a blurred non-emissive medium, and a surface continuation may later
enter a blurred emitter. Static media lose variance reduction in this mode but
not energy. The frame-time emission CDF may remain resident but is never
sampled.

The restriction is necessary because the advection warp composes polynomial
degrees far past §7.1's deterministic-quadrature guarantee (trilinear velocity
makes the warped coordinate degree 3; a trilinear channel through it degree 9;
φ(T(x′)) times carbon degree 36, with cubic grid-plane roots and degree-9
φ-threshold roots). `EvalDistancePdf` therefore cannot represent the actual
warped tracking proposal. For **NM spectral thermal transport**, pure DT is
  exact and its collision score remains (σ_a,total/σ_t)B. The scalarized Pel tracker
does not share that reduction; fire-medium Pel preview consequently evaluates
the nominal frame without velocity blur and logs that limitation. Predictive
blurred output is spectral-only.

Because preflight removes blur from every medium with render-consumed chem or
configured ε_add,
their §7.1 static full-segment estimators remain unchanged and weight 1; no
ratio-tracked “advected source” estimator exists in this arc. Gates: chromatic
Pel must prove it stayed nominal/unblurred; spectral thermal velocity-shear
slabs must match high-accuracy references; and nested static+blurred media,
including a static emitter behind a null-bounded blurred medium, must match
volume-NEE-on blur-off references within confidence intervals. A forced
>1024-distance-event case is a RED test against a capped pure-DT implementation.
This optional subfeature
ships in Phase C only if per-frame cadence proves visually insufficient;
grid interpolation
blur remains rejected because it fabricates density where none advected.

## 9. Scene-language sketch (illustrative, not final)

```
fire_medium
{
	name			campfire_volume
	fidelity_mode		predictive
	sequence_manifest	media/campfire/sequence.rise-fire.cbor
	channel_carbon		carbon
	channel_temperature	temperature
	channel_condensed	condensed
	channel_reaction	reaction			# optional diagnostic; never drives chem
	channel_chem_ch		chem_CH			# all three present or chem_model=none
	channel_chem_c2		chem_C2
	channel_chem_co2	chem_CO2
	channel_velocity	velocity			# optional, enables motion blur (§8)
	# Optional overrides replace WHOLE versioned metadata records. Loose
	# per-parameter soot/smoke/phase tuning is intentionally unsupported.
	optical_preset_override	presets/fire_optics_v1.cbor
	chem_preset_override	presets/methane_chem_v1.cbor
}

global_medium
{
	medium			campfire_volume
}
```

Conventions (verified against the registry): new parameters follow the existing
all-lowercase snake_case descriptor convention. Each override is a
schema-versioned RISE-CBOR64-v1 record under the same schema as the
embedded manifest record—JSON overrides are not accepted. The optical record contains soot E(m) plus
**separate** cool-carbon and condensed-organic k_m/n/ω/g entries; the chem
record contains per-fuel state-dependent η_b functions, the three S_b records,
normalization ranges, absolute spatial calibration evidence, and provenance.
Loading either override logs that grid metadata was replaced, and the record
identity is written to output provenance. Overrides are calibration/ablation
features: a whole-record override is fidelity-neutral only when its canonical
bytes/hash exactly equal the embedded record; predictive mode rejects every
different hash, so a scene cannot tune smoke appearance while retaining the §1
fidelity claim. Spatial placement: the manifest's sequence/channel transforms
are authoritative and every VDB embedded transform is validated against them;
raw-slice input requires explicit
`bbox_min`/`bbox_max` as today. `sequence_manifest` is the sole locator for a
time sequence and contains the ordered frame paths/digests; it does not infer
siblings from a printf pattern. Legacy `volume_pattern`'s slot remains the
Z-slice index — raw-slice *sequences* would need per-frame manifests/directories
and are out of scope.

`fire_medium` creates a **named medium manager entry**; it is inert until bound.
The example installs it as the scene's `global_medium`. A bounded fire volume
instead references the same name through `standard_object.interior_medium` on
a closed boundary object, following the existing innermost-exclusive medium
stack. Defining the chunk alone never implicitly selects global behavior.

Descriptor-driven parser rules apply (one `Describe()` entry + one
`Finalize` read per parameter; see
[src/Library/Parsers/README.md](../src/Library/Parsers/README.md)). A
`fire_medium` chunk wrapping a general `multichannel_heterogeneous_medium`
keeps the physical mapping (soot κ, Planck, chem SPD) in one audited place —
mirroring how `blackbody_painter` packages Planck — while the general chunk
stays available for non-fire uses. New chunks require no scene-version bump
or migration tooling (version-agnostic CST loader; descriptor-driven
registry).

**`multichannel_heterogeneous_medium` is the Phase-A authoring surface, and
lands first.** `fire_medium` above requires a `sequence_manifest` — a Phase-C
artifact produced by the simulator — and an OpenVDB-enabled build, so it
cannot be the vehicle for renderer-only work. The general chunk therefore
takes the same named channels bound to **statically authored sources** rather
than a manifest: per channel either a raw grid (the existing
`volume_pattern`/bbox idiom, one lattice shared by all extinction-relevant
channels per §7.1 step 1) or a baked 3D painter, plus **the complete optical closure as explicit
parameters**. This chunk has no manifest and therefore no preset record, so
every constant `fire_medium` would inherit from §12 must be authored here —
there is no implicit default and no partial specification. Channel values are
read in §8's units (`carbon`/`condensed` in g/m³, `temperature` in K).

Required whenever `channel_carbon` is present (the carbon channel is *both*
constituents — it becomes cool carbon wherever φ(T) < 1, so its cool triple
is never optional):

| parameter | §4 symbol | meaning |
|---|---|---|
| `soot_em` | E(m) | hot-carbon absorption function (§4.1) |
| `soot_density` | ρ_soot | kg/m³, closes f_v = φ·10⁻³·c_carbon/ρ_soot |
| `soot_albedo_hot` | ω_hot | hot-carbon single-scattering albedo; σ_s = ω_hot·σ_a/(1−ω_hot) |
| `soot_g_hot` | g_hot | hot-carbon HG asymmetry |
| `smoke_km_carbon`, `smoke_n_carbon`, `smoke_albedo_carbon`, `smoke_g_carbon` | k_m,carbon, n_carbon, ω_carbon, g_carbon | cool-carbon extinction triple + its HG lobe (§4.3) |

Required only when `channel_condensed` is present:
`smoke_km_cond`, `smoke_n_cond`, `smoke_albedo_cond`, `smoke_g_cond`.

There is **no composite `phase` parameter** — the §4.3 closure is a
σ_s-weighted mixture of *per-constituent* HG lobes, so a single `phase hg g`
would be ambiguous about which constituent it governs and would silently
produce a different scattering distribution than the mixture. Each
constituent names its own g; the mixture is derived, never authored.
Note the deliberate asymmetry with `fire_medium`, which forbids loose
per-parameter tuning and takes whole versioned preset records instead: that
chunk *has* a record to point at, and per-parameter overrides there would let
a predictive render silently mix preset provenance. This chunk has no record,
cannot be predictive, and so authors the constants directly.
Omitting any required parameter is a parse error, not a defaulted value —
these are exactly the constants whose silent substitution §1's fidelity bar
forbids, and a fixture that renders with the wrong ω is worse than one that
fails to load. The §12 fixture values (ω_hot = 0.10, g_hot = 0.5; cool
carbon n = 1.2, ω = 0.6, g = 0.6; condensed n = 0.5, ω = 0.9, g = 0.7) are
the intended test values — as non-predictive fixtures, per §12:

```
multichannel_heterogeneous_medium
{
	name			candle_test
	channel_carbon		painter carbon_profile     # or: volume_pattern ...
	channel_temperature	painter temperature_profile
	bake_resolution		96 96 192                  # one lattice, all channels
	bbox_min		-0.02 0.0  -0.02
	bbox_max		 0.02 0.08  0.02

	soot_em			0.26                       # E(m), Dalzell-Sarofim
	soot_density		1800                       # kg/m3
	soot_albedo_hot		0.10
	soot_g_hot		0.5
	smoke_km_carbon		8.7                        # m2/g @ 633 nm
	smoke_n_carbon		1.2
	smoke_albedo_carbon	0.6
	smoke_g_carbon		0.6
}
```

Grammar note: each `channel_*` value here is a **source** (`painter <name>`
or `volume_pattern <pattern>`), not the channel-name string `fire_medium`
uses to select a grid from its manifest — the two chunks answer different
questions ("where does this field come from" vs "which grid in the sequence
is it"), and the descriptor declares them as distinct tuple kinds.
`bake_resolution` fixes the single shared lattice for every
extinction-relevant channel (§7.1 step 1), following
`painter_heterogeneous_medium`'s existing bake idiom.

It is **preview-only by construction** — hand-authored fields carry no
producer qualification, so §8's predictive gate cannot be met and the medium
stamps `producer_unqualified`. That is exactly right for Phase A, whose job is
to make the estimators correct against analytic targets (§7.1 steps 0/2/6),
not to make predictive claims. `fire_medium` then adds the manifest,
sequence/time semantics, and fidelity machinery on top of an already-working
medium in Phase C.

## 10. Architectural constraints (standing rules this design must respect)

1. **Physical scalars never touch JH uplift** (§4, G1, G4): T, f_v, κ, E(m),
   IOR are `IScalarPainter`-side quantities per
   [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md). Concretely: the
   Planck kernel is extracted from `BlackBodyPainter` as a free function
   (λ, T) → spectral radiance and is **never reached through
   `IPainter::GetColorNM`** — any `GetColorNM` in the fire emission path is
   a bug by definition.
2. **PT-only integrator scope** (§5.2), enforced by the G10 routing rule.
   Re-opening BDPT/VCM for fire goes through the integrator-matrix process.
3. **Scene immutability / thread-safety** ([ARCHITECTURE.md](ARCHITECTURE.md)):
   per-frame grid swaps, majorant rebuilds, and emission-CDF rebuilds happen
   in the between-renders frame-advance step, never during a render. This is a
   concrete **non-const control-plane API**, not a new `mutable` escape hatch
   on `IScene`: `Job` owns an `IRenderPreparationController` registry populated
   when time-varying media are constructed. Append three prepared overloads to
   `IRasterizer`—`RasterizeScenePrepared`, `PredictTimeToRasterizeScenePrepared`,
   and `RasterizeSceneAnimationPrepared`—each taking non-const
   `IRenderPreparationController&`. Still/predict overloads accept only the
   nominal scene time; animation retains its time range and derives each field/
   frame nominal time internally. **Callers never supply open/close bounds.**
   After animator evaluation the rasterizer itself computes
   `RenderTimeSupport{nominal,open,close}` from the now-current camera, active
   crop and field using `ComputePathTimeSupport`, then passes that value to the
   controller. `IJob`, CLI, editor, and `AutoRasterizer` explicitly forward the
   prepared overloads/controller; the latter may not fall back to its current
   three legacy forwarding methods. Legacy direct rasterizer overloads
   retain `{0,0,0}` only for static scenes; `IScene::HasTimeVaryingMedia()` makes
   them hard-error before tracing when preparation is required. Editor/timeline
   calls must pass the scrubbed time explicitly.
   The controller's non-const
   `PrepareMediaForRender(const RenderTimeSupport&)` builds a complete
   immutable frame state off to the side and atomically swaps it before worker
   launch. Its idempotency key is
   `prepared_input_id=SHA-256(RISE-CBOR64-v1(prepared_input_v1))`, where the
   canonical record contains the stable medium binding, exact selected sequence/
   frame digests, medium authored generation, channel bindings, scene-unit and
   placement transform, complete effective optical/chem/condensable records and
   overrides, blur/end/halo policy, majorant/CDF/estimator settings, and the full
   `RenderTimeSupport`. Only an identical full ID may reuse a prepared state;
   sequence identity plus `(nominal,open,close)` alone is insufficient.
   `prepared_input_id` and the resulting prepared-state generation are recorded
   in provenance. Identical-time rerenders after any channel, optical, unit,
   placement, or blur-policy mutation must rebuild and have a different ID.
   `shutterOpen`/`shutterClose` are absolute scene times and come
   from one shared `ComputePathTimeSupport` helper—the same helper used to
   generate primary path times. It takes the active crop/field and computes the
   sign-aware min/max over nominal time, exposure, scanline `scanningRate`, and
   per-pixel `pixelRate`. The controller always receives this truthful full
   camera support, but each medium first resolves whether blur is requested and
   supported: blur-off media use nominal only; blur-on media map both endpoints
   through α (swapping min/max when α<0) before end-policy, halo/majorant, and
   cadence checks. Preview fallbacks that disable blur are committed before the
   render-global estimator mode is selected. Camera exposure alone is not a
   valid bound when blur is active.
   Append a const, side-effect-free
   `IAnimator::ClassifyTimeSupport(open,close)` query (plus the sole
   `Animator` implementation) that returns the sorted stable identities of
   active keyframed targets whose evaluated parameter is not provably constant
   over the closed interval. “Provably constant” means the interpolation
   segment representation itself is constant over every overlapped segment;
   sampling endpoints is forbidden. The prepared helper uses this query for
   the reject/nominal-hold rule in §8 and records those identities in output
   provenance. As an appended virtual, the normal API/build-project checklist
   and an old-caller ABI test apply.
   The shared `PixelBasedRasterizerHelper` request/frame order is fixed to:
   acquire (or for later animation frames retain) the request-wide mutation-sink
   freeze → one main-thread nominal animator evaluation under its private token →
   prove/reject/nominal-hold every animator over the full path-time support →
   complete the under-lease identity/fidelity preflight → object-manager
   `InvalidateSpatialStructure()` **and** `IRayCaster::InvalidateLightSamplers()` →
   controller media preparation/CDF swap →
   `RayCaster::AttachScene` geometry realization → object
   `PrepareForRendering`/TLAS rebuild → private `SetPreparedSceneTime` safe-cache update →
   explicit volume-emission guide rebuild → worker dispatch.
   `SetPreparedSceneTime(time, RenderPreparationUpdate)` sets the nominal scene
   time and refreshes only caches proven not to trace rays or evaluate an
   Animator. It **must not call the legacy `Scene::SetSceneTime` photon-map
   regeneration path**: that path invokes `TracePhotons(..., time, true, ...)`,
   which performs repeated animator evaluations after TLAS/light preparation
   and can leave the scene at an arbitrary photon time. The
   `ClassifyPreparedTransportDependencies` recursive tri-state classifier
   (fail-closed, unknown-dominates, covering nested ops, CSG operands, shared
   DAGs, and cycles) decides whether any render-reachable shader/op consumes a
   Scene photon map, irradiance cache, or SMS — its specification lives in
   [RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md). What
   this arc requires from it: an **active** photon-map or irradiance-cache
   consumer hard-errors (`photon_transport_unprepared` /
   `irradiance_transport_unprepared`) rather than silently rendering with a
   cache built at some other time, while a **dormant** configured cache is
   left bitwise untouched and does not block. Preview use of an active
   consumer stays on the legacy nonprepared path and cannot claim this arc's
   immutable-time guarantee.


   Tests combine a configured dormant Scene photon map and moving occluder,
   recording zero photon traces and exactly one main-thread nominal animator
   evaluation; an active consumer rejects before cache/TLAS mutation. A dormant
   irradiance cache under spectral PT remains unpopulated/unmarked, while an
   active cache consumer rejects before the current irradiance prepass. A
   predictive scene with `sms_enabled=true,sms_biased=true` is RED. Preview
   separately gates pure-rasterizer SMS, integrated `PathTracingShaderOp`, and
   standalone `SMSShaderOp`, asserting zero map builds, zero SMS evaluations,
   and zero SMS emission-suppression decisions on both transport surfaces.
   `InvalidateLightSamplers()` is appended to `IRayCaster` (the helper owns only
   that interface) and implemented by `RayCaster` as an explicit control-plane
   dirty bit; on the
   following same-pointer or new-pointer `AttachScene`, it forces the existing
   complete `RebuildLightSamplers()` path (LuminaryManager, LightSampler, and
   EnvironmentSampler) regardless of `Scene::GetLightTopologyGeneration()`, then
   clears only after success. This unconditional baseline covers animated light
   position/power, environment parameters, and emissive-material power whose
   `RegenerateData()` calls do not currently bump scene light generation. The
   later volume-emission guide rebuild is separate and consumes the prepared
   medium/CDF generation; neither rebuild stands in for the other.
   Both invalidations are mandatory after every animator evaluation in this
   baseline, before `PrepareForRendering`. A future optimization may skip
   **spatial** invalidation only when the animator proves no object transform or
   bounds changed. It may independently skip **light-sampler** invalidation only
   when it proves no analytic-light position/power, luminary membership/weight,
   emissive material, or environment sampling state changed. One proof never
   implies the other. `PrepareForRendering()` alone is not either invalidation
   operation. The appended virtual follows the ABI/API and all-build-project
   checklist and has a mock-`IRayCaster` forwarding test.
   Animation may no longer attach only once before its frame loop; every CDF
   swap reaches this sequence, and the sampler rebuild consumes the controller's
   monotonically increasing generation even when the Scene pointer is unchanged. Every
   still, normal animation frame, interlaced field, explicit single frame or
   subrange, and AOV fallback re-entry uses that same seam; direct PT entry
   points and `PredictTimeToRasterizeScene` (which traces rays) must call the
   helper rather than duplicate the order. This explicit dependency also avoids
   extending the pre-existing conceptually-non-const `IScene::SetSceneTime`
   exception; the private prepared setter is capability-gated and not appended
   to the public interface. Tests compare
   selected frame identity across those entry modes and assert no prepare call
   occurs from a worker or per-sample path; a rolling-scan/pixel-rate fixture
   asserts every sampled path time lies inside the prepared interval and the
   shutter-bounded majorant.
   Additionally (ARCHITECTURE.md:70): `IAnimator::EvaluateAtTime` per-sample
   evaluation is a documented pre-existing data race — **the prepared fire
   path disables that worker-time call for the whole scene, not only volume
   data**. Predictive mode rejects nonconstant keyframed state across the
   support; preview holds it at nominal time. §8's read-only Eulerian velocity
   blur is the only temporal variation in this arc until immutable scene
   snapshots and swept acceleration bounds land. Tests instrument the animator
   call count/thread ID and combine a moving occluder with a translating fire
   slab to prove no worker mutation or stale-TLAS visibility is possible.
   Zero-exposure consecutive-frame tests animate, separately, a light's power
   and position, environment power, and emissive-material power; each asserts
   the unified sampler's probabilities/positions and rendered direct-light
   expectation change at the nominal frame without reattaching a different
   Scene pointer.
4. **Tests are executables** (`tests/`), scenes under `scenes/Tests/Volumes/`;
   numeric gates, not eyeballs
   ([skills/write-highly-effective-tests.md](skills/write-highly-effective-tests.md)).
   Natural homes: extend `VolumeSpectralCoefficientsTest.cpp` (G4 gate),
   `RayCasterVolumeAbsorptionTest.cpp` (Phase A step 0/2 gates),
   `PainterVolumeAccessorTest.cpp` (multi-channel accessors),
   `SceneEditorMediaFullCoverageTest.cpp` (editor stance, §7.1); new scenes
   alongside `pt_chromatic_fog.RISEscene`. New source files touch all five
   build projects (CLAUDE.md checklist). This doc joins the docs/README.md
   index when the first phase lands.
5. **Measurement discipline:** variance/RMSE claims per
   [skills/variance-measurement.md](skills/variance-measurement.md); perf
   claims per
   [skills/performance-work-with-baselines.md](skills/performance-work-with-baselines.md).
   **All numeric gates run on pre-denoise EXRs or with `oidn_denoise FALSE`**
   — the OIDN splat-loss trap is documented in the bdpt-vcm-mis-balance
   skill, and OIDN's behavior on extreme-HDR emissive volumes (no volume aux
   channels exist) is unvalidated; denoiser-on fire output is a Phase D
   evaluation item, not a validation channel or predictive output path.
   **Firefly policy:** validation and predictive renders are *unclamped* and
   unregularized (clamping would corrupt the §3.8/§7.2.7 energy gates); Phase
   B's MIS is the principled variance mechanism, and path
   regularization is considered only if measured variance after Phase B
   still demands it. The integrator-matrix lesson stands: never validate a
   new emission path with a reader that flows through the component under
   test.

## 11. Risks

| risk | mitigation |
|---|---|
| Emissive-volume NEE MIS bugs (the env-IBL arc showed how subtle partition-of-unity is) | full estimator + MIS partition + weight-1 rule specified in §7.2 (not a sketch); PT-only scope; brute-force equality gates incl. receiver-inside-emitter and three-strategy coexistence; instrumentation-first per [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) |
| Pre-existing emissive-medium bias contaminates Phase A baselines | Phase A step 0 gate runs *first*; collision-based estimator supersedes the suspect closed form |
| Emission lands on one transport surface but not the other (G11 — the internal rounds' biggest miss) | §7.1 step 2 names both surfaces + ordering; pure-absorber gates run through both entry routes; HWSS hard-disabled for fire media until the companion fix |
| Sim scope creep toward a general CFD package | §1 non-goals; §3.8 validation table defines "done"; solver features not needed by a validation row don't land |
| Open-boundary artifacts contaminate validation (the classic fire-LES trap) | §3.6 BC spec + mandatory domain-size doubling check per validation row |
| Chromatic majorants degrade tracking efficiency | start conservative max-over-λ; measure null-collision rates; specialize only on evidence (§12, chromatic-majorants decision) |
| Multi-channel grid memory (dense 512³: seven scalar channels plus three velocity components at §8 precisions ≈ 3.8 GB/frame) | fp16 where §8 permits; candle is small-domain DNS; OpenVDB sparsity is required for production plumes |
| Quantization/normalization destroying physical units (the bridge's 8-bit lesson, §5.1) | §8 units + precision table; ingestion round-trip test asserting known values |
| RGB-path fire looks different from spectral-path fire | §7.1 step 4 declares spectral-first; RGB is an approximate preview path with a logged diagnostic (deterministic projection bias, §12 RGB-path decision) |
| Velocity blur silently moves nonmaterial sources or mixes incompatible estimators | §8 requires per-channel temporal semantics, rejects predictive blur whenever render-consumed chem/configured ε_add is present regardless of nominal values, disables the whole medium's blur in preview, and uses render-global NM pure DT only for qualified material fields |
| Existing 1024-step tracker watchdog becomes an estimator cap | Phase A/blur distance sampling uses state-preserving continuation to segment end; forced-cap RED tests cover Pel/NM and both transport surfaces |
| Heavy or wet smoke exceeds the one-fluid dry-aerosol model | χ_load>1 % is a hard predictive error; water/hygroscopic aerosol is explicitly out of scope and recorded in metadata (§1/§3.2/§8) |
| Phase-A procedural flame looks wrong and stalls review | it is a *radiometry* target, not a beauty target; beauty ships with Phase C sims |

## 12. Decisions taken and questions still open

**Closed (decision + where it's recorded):**

- Soot model → **atom/energy-balanced gross formation + oxidation,
  calibrated to the published NET yield with a pinned calibration state +
  cross-prediction gate**, the primary step withholding **both non-burning
  streams** — soot carbon and condensable organics — with their O₂ and
  chemical energy (Δh_c,eff, s_st,eff; r9), oxidation consuming
  transported Y_O, and **two transported aerosol inventories (c_carbon,
  c_condensed) with the hot/cool split derived via φ(T) — no transfer
  operators**; LLJ deferred
  behind a validation gate (§3.4; revised r6 through r12 — r11 defines Z as
  a total-mixture conservative scalar and makes aerosol heat capacity and
  phase latent energy dynamic rather than diagnostic-only; the r7 full
  bookkeeping without a transported oxidizer was the third round's P0).
- Chemiluminescence source → **sim exports separate absolute CH*, C₂*, and
  CO₂* radiant-source channels** from a versioned state-dependent η_b closure
  (§3.3); the renderer combines them with separately unit-normalized S_b
  records (§4.4). The HRR-only `reaction` channel remains diagnostic;
  renderer-side Z-windowing is still rejected as resolution-sensitive.
- Wavelet turbulence → **excluded**; detail comes from solver resolution
  only (§3.7).
- Two-way radiative coupling → **rejected**; the local sink is the
  **budget-partitioned escape-factor model over the total radiating
  support, with the γ post-fire blend and frozen-β discrete closure**
  (§3.5, r6 revised r7 — the r3–r5 max-blend was numerically refuted at
  the design's own f_v range, and r6's reacting-only normalization leaked
  the plume term).
- Emission sampler shape → **two-level cell→voxel CDF** (§7.2.3);
  hierarchical-across-cells only on measured need.
- Reactedness progress variable → **not needed**; reactedness is implicit
  in (Z, Y_F), with the two-route **ignition** gate
  (piloted-with-resolved-T / spontaneous-AIT) and the FDS
  critical-flame-temperature **extinction** test (§3.3). **Amended r8:**
  Y_O *is* now transported — not as a progress variable but as the
  independent oxidizer DOF the soot-burnout bookkeeping requires (the
  third external round's P0: the algebraic Y_O(Z, Y_F) relation has no
  room for a local burnout O₂ sink); the algebraic relation survives as a
  verification diagnostic.
- **E(m) dataset** (was Q1; adopted from external review) → Dalzell–Sarofim
  E = 0.26 is the Phase A analytic default and regression fixture; Phase C
  fidelity adopts a **versioned λ-dependent table** with κ_P(T) evaluated
  numerically (§3.5) — never the 3.83 constant formula with λ-dependent
  E — dataset ID/hash in the §8 metadata.
- **Phase-function form / provisional optics fixtures** (was Q2) → HG is an
  accepted Phase-A/C *functional form*. The numerical records hot soot
  (ω=0.10,g=0.5), fresh smoke (n=1.2,ω=0.6,g=0.6), and organic droplets
  (n=0.5,ω=0.9,g=0.7) are **synthetic non-predictive regression fixtures
  only**, used to exercise chromatic transport and constituent mixing. They
  are not preset v1 and cannot satisfy §1: the cited fresh-biomass SSA
  0.46–0.74 is a mixed aerosol measurement and cannot be assigned to the
  carbon-only constituent (whose mature-soot SSA is ~0.2–0.3). Predictive
  output remains disabled until Q2 freezes separate constituent datasets and
  the total-mixture 8.7 m²/g anchor is passed.
- **Chromatic majorants** (was Q4; adopted) → max-over-λ retained
  initially; the 380–780 nm inefficiency bound is ~2.05× for soot (1/λ)
  and ~2.94× for n = 1.5 smoke — acceptable pre-profiling. Bounds must
  include interpolation overshoot, and HWSS media must be bounded over the
  whole carried bundle, not the hero alone.
- **VDB** (was Q5; adopted) → OpenVDB-only native in-core desktop capability in
  this arc; builds without it reject Phase-C sequences. A portable fallback is
  deferred until its frame-container wire contract is separately designed (§8).
- **TRI** (was Q6; adopted) → accept through Phase C; quantify via filtered
  candle-DNS / resolution studies. A temperature-only subgrid PDF is
  rejected — soot emission depends on the joint (T, f_v) distribution and
  covariance; a joint model is Phase D, and only if the measured error
  justifies it.
- **RGB path** (was Q7; adopted) → kept as an explicitly approximate
  preview path with a logged diagnostic; auto/final fire rendering routes
  spectral. The correct framing is deterministic spectral-projection
  *bias*, not variance (§7.1 step 4). Under velocity blur it evaluates the
  nominal unblurred frame because scalarized tracking lacks the per-wavelength
  pure-DT reduction (§8).
- **Wet smoke** → excluded from this arc. Predictive scope is dry soot carbon
  plus dry condensed organics; water aerosol/hygroscopic growth requires a
  future transported-water and multi-RH validation extension (§1).

**Still open:**

1. **(combustion)** Per-fuel state-dependent η_b closures and the separate
   CH*/C₂*/CO₂* SPD datasets: which absolutely-calibrated spectral
   measurements to adopt and version for §4.4. Selection criteria
   (accumulated rounds 2–6): the dataset must provide **absolute integrated
   power per band/source, spatial profiles or mixture-state coordinates, each
   normalization interval, geometry corrections, and paired HRR**—one global
   spectrum or relative band shapes cannot pin η_b. Round-5 candidate: the Lai
   et al. 2025 radiometrically
   calibrated methane–air hyperspectral study — adoptable for methane iff
   its released data meet the criteria; it cannot establish a universal
   wood/wax/aromatic η_b, which stays per-fuel. (The *method*—band-resolved
   measured spectral power ÷ local primary effective gas HRR, S_b normalized
   on its recorded interval, state-dependent yield fit and independent spatial
   validation—is settled, §4.4.) **Until one record is adopted for a
   fuel, predictive rendering for that fuel is blocked.** `chem_model=none`
   preserves predictive status only with the measured negligibility record and
   thresholds in §7.0; lack of data is not evidence of zero emission. The
   provisional η_b/S_b values are test fixtures, not physical defaults.
2. **(optics)** Provenance and versioning for the adopted optical presets:
   which specific measurement sets become the named v1 presets for soot
   albedo, the **constituent-specific** smoke sets (k_m/n/ω/g for cool
   carbon AND for condensed organics separately — the third round rules
   out one merged smoke preset, matching §3.4/§4.3's split inventories),
   and the λ-dependent E(m) table. Round-5 direction, adopted:
   **Chang–Charalampopoulos (0.2–6.4 µm) is the leading Phase-C
   candidate for the measured portion of the versioned soot E(m) table**,
   but it cannot alone define low-temperature Planck means: at 300 K only
   about 12.8 % of the B_λ/λ integral lies below 6.4 µm. Preset v1 therefore
   requires an explicitly identified, continuity-checked long-wave extension
   (or a second measured dataset) before §3.5 may integrate it. Also,
   **Mulholland–Croarkin's 8.7 ± 1.1 m²/g @ 633 nm serves as a
   total-post-flame validation anchor only** — it cannot split cool
   carbon from condensed organics or determine both constituents'
   ω/g/n; separate constituent data are still required before freezing
   preset v1. Those records must cover absorption over the thermal wavelength
   range used by §3.5 so cool-carbon/organic Kirchhoff emission is defined;
   visible SSA/extinction alone is not enough. Their interpolation must also
   carry certified derivative enclosures proving §3.5's backward-Euler root is
   unique at an accepted Δt; value-only tables do not close the solver. Deliverable form
   (second-round qualification, adopted):
   **a single versioned preset record embedded in every grid sequence**
   (§8 metadata), not loose per-parameter choices.
3. **(combustion/aerosol)** The qualifying y_cond calibration dataset remains
   unselected. Adoption requires every field and the paired particle-plus-gas
   measurement in §3.4, an archived raw-data snapshot/hash, the OC→surrogate
   conversion, one fitted reference crib, and the independent scale plus
   D*/δx cross-prediction results. Until that versioned record exists, fuels
   with nonzero condensable yield cannot produce predictive grids; setting
   y_cond=0 is predictive only for a fuel record that bounds it negligible by
   the same measurement protocol.
4. **(radiometric validation)** Select and archive the absolute flame
   spectral-radiance dataset used by §3.8. Its record must pin fuel, geometry,
   view/slit/solid-angle calibration, atmospheric/path correction, wavelength
   and bandwidth, raw-unit conversion, uncertainty, and the numeric acceptance
   tolerance. The gate reads **raw pre-exposure, pre-tone-map NM radiance** in
   W·m⁻²·sr⁻¹·nm⁻¹ directly from the spectral harness; an XYZ/RGB EXR cannot
   serve as L_λ evidence. Predictive end-to-end radiometry remains blocked
   until this record and tolerance are frozen.
5. **(gas radiation)** Select and archive the simulator-only CO₂/H₂O opacity
   model required by §3.5/§8: spectroscopic source and table-generation code,
   wavelength/T/composition/pressure domains, broadening and band-overlap
   convention, interpolation, independent absolute cooling reference and
   tolerance, deterministic bytes/hash, and the 380–780 nm absorption/emission
   upper-bound test, certified closed domains, and out-of-domain rejection RED
   gate. Predictive Phase C is blocked until this closes; metadata
   fields without the actual table are not a model.
6. **(thermochemistry)** Freeze the baseline air plus per-fuel
   `gas_thermochemistry` records required by §3.3 and the carbon/condensed
   `aerosol_thermochemistry` record required by §3.4: source/provenance for every
   W/c_p/h_s record, common T_ref, ambient/injection composition, fuel formula
   and LHV, and atom-balanced primary products. The record generator must prove
   element balance and the y_form/y_cond admissibility inequalities over every
   accepted fuel preset; certify continuous h_s with h_s(T_ref)=0 and
   dh_s/dT=c_p, strictly positive c_p/C_T bounds, a nonempty common gas+aerosol
   temperature interval, and endpoint bracketing/unique inversion for every
   admissible composition/loading; certify closed property domains; and pass
   discontinuity, derivative-mismatch, nonpositive-c_p, empty-domain,
   unbracketed-energy, and out-of-domain RED gates. Predictive simulation is blocked for a fuel until its
   gas and aerosol record is archived and hashed.
7. **(molecular/SGS transport)** Freeze the §3.2 `transport_closure` record:
   sourced μ_k/k_k tables and certified domains, Wilke and Wassiljewa/Mason–
   Saxena implementations, unit-Lewis D relation, Pr_t/Sc_t, Vreman C_v/filter
   definition, wall treatment, τ_chem, and T_CFT. The manufactured momentum,
   Vreman, DNS molecular-diffusion, and out-of-domain gates must pass before
   predictive Phase C; substituting platform/library defaults is forbidden.

## 13. References (non-exhaustive)

- Nguyen, Fedkiw, Jensen, *Physically Based Modeling and Animation of Fire*, SIGGRAPH 2002.
- McGrattan et al., *Fire Dynamics Simulator Technical Reference Guide* (NIST) — FDS 6 lumped-species/EDC formulation, min-of-timescales mixing time, critical-flame-temperature criterion, low-Mach solver, D* resolution guidance, validation practice.
- Heskestad, *Fire Plumes, Flame Height, and Air Entrainment*, SFPE Handbook (flame height, virtual origin, plume laws).
- McCaffrey, *Purely Buoyant Diffusion Flames: Some Experimental Results*, NBSIR 79-1910 (centerline temperature/velocity dataset — §3.8 absolute gates).
- Cetegen & Ahmed, *Experiments on the periodic instability of buoyant plumes and pool fires*, Combust. Flame 1993 (puffing f ≈ 1.5/√D).
- Tewarson, *Generation of Heat and Chemical Compounds in Fires*, SFPE Handbook (soot yields y_s, radiative fractions χ_r).
- Leung, Lindstedt, Jones, two-equation soot model, Combust. Flame 1991 (Phase-D reference; rejected as baseline per §3.4).
- Frenklach, HACA soot surface growth mechanism.
- Dalzell & Sarofim, soot optical constants, J. Heat Transfer 1969; Chang & Charalampopoulos 1990 (λ-dependent E(m)); Hubbard & Tien 1978 (Planck-mean soot fit — see §3.5 note).
- Köylü & Faeth, soot aggregate optical properties / albedo, J. Heat Transfer 1994.
- Mulholland & Croarkin, specific extinction coefficient of flame-generated smoke, Fire Mater. 2000 (k_m ≈ 8.7 ± 1.1 m²/g, §4.3 — total-post-flame anchor only, per §12).
- Lai et al., radiometrically calibrated methane–air flame hyperspectral measurements, 2025 (candidate §12 Q1 dataset; adoption criteria in §12).
- Sorensen, *Light Scattering by Fractal Aggregates* (RDG-FA review), Aerosol Sci. Tech. 2001.
- Gaydon, *The Spectroscopy of Flames* — CH*, C₂ Swan, CO₂* continuum.
- Vreman, *An eddy-viscosity subgrid-scale model for turbulent shear flow*, Phys. Fluids 2004.
- Kulla & Fajardo, *Importance Sampling Techniques for Path Tracing in Participating Media*, EGSR 2012.
- Novák et al., ratio tracking / residual ratio tracking, SIGGRAPH Asia 2014; null-collision framework: Miller, Georgiev, Jarosz, SIGGRAPH 2019.
- Kutz et al., *Spectral and Decomposition Tracking*, SIGGRAPH 2017 (Phase-D HWSS media).
- Wilkie et al., hero-wavelength spectral sampling, SIGGRAPH Asia 2014.
- Villemin & Hery, *Practical Illumination from Flames*, JCGT 2013 (emissive-volume light sampling in production).
- Fedkiw, Stam, Jensen, *Visual Simulation of Smoke*, SIGGRAPH 2001; Zehnder, Narain, Thomaszewski, *An Advection–Reflection Solver*, SIGGRAPH 2018 (rejected for variable density, §3.7); Kim et al., *Wavelet Turbulence*, SIGGRAPH 2008 (excluded, §3.7).
## 14. Revision history

The full per-revision log — 32 revisions across 4 internal review rounds, 6
external expert rounds, and 20 implementation-review rounds — lives in
[FIRE_SMOKE_DESIGN_HISTORY.md](FIRE_SMOKE_DESIGN_HISTORY.md). **Consult it
before reverting anything here**; many current forms are corrections of
plausible-looking earlier ones.

Arc summary:

- **r1–r5** established the shape: FDS-style transported formulation over
  level-set/state-relation alternatives; soot as a calibrated product species;
  PT-only integrator scope; the collision-based emission estimator; Phase B's
  emissive-volume NEE promoted from a paragraph to a full estimator/MIS design.
- **r6–r10** hardened the physics and the code grounding: two P0 corrections
  (a divergence constraint missing its composition-enthalpy term; a soot
  burnout scheme with no transported oxidizer DOF), the Planck kernel's
  quantity/unit pinning, scene-unit propagation, the dual transport-surface
  discovery (`PathTracingIntegrator` alongside `RayCaster`), and the
  φ(T)-derived constituent aerosol optics.
- **r11–r31** were a long automated convergence loop. It produced real gains —
  band-resolved chemiluminescence, an unbiased chem line estimator, the
  log-domain density contract, FCT-limited conservative transport, and much
  sharper transport-closure rules — but its finding rate never decayed, and it
  drifted into renderer-wide and infrastructure concerns. The r32 restructuring
  removed that drift (see below).
- **r32 (2026-07-28)** — scope restoration. Fixed two self-blocking defects
  introduced by the loop (the continuation-closure allowlist default-denied the
  arc's own fire medium; §3.3 and §3.7 gave incompatible diffusive-flux
  constructions). Replaced ~90 lines of cryptographic producer attestation,
  trusted-key registry, and root-rotation machinery with a declared-field +
  digest **integrity** contract, and ~460 lines of artifact publication
  transaction with a provenance **manifest**. Split the render-preparation
  lifecycle and transport-dependency classifier into
  [RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md) and this
  history into its own file. Compressed §3.7 to decisions and invariants, and
  rewrote §7.0 so phase gates list engineering deliverables while
  measurement-dependent claims gate *output labeling* rather than phase entry.
