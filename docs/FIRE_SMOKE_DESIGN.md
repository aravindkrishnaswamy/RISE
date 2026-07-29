# Fire & Smoke — Physics Simulation and Rendering Design

**Status:** DRAFT (revision 13 — after internal review rounds 1–4,
**six external expert review rounds**, and two post-r11 implementation-review
rounds; see §14). No fire *feature* code has
landed; one Phase-A prerequisite has (the trilinear-accessor repair, commit
`2fba2b48`, in master). Phase gating per §7.0.
**Date:** 2026-07-28 (r6–r13; r1–r5 were 2026-07-27).
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

**Fidelity state is enforceable, not descriptive prose.** Every simulator run
and `fire_medium` requests `fidelity_mode = predictive|preview`. Predictive is
fail-closed: any unmet predictive prerequisite in §7.0/§8 aborts before fields
are accepted or render workers launch; it never silently downgrades. Preview
may proceed, but derives `fidelity_status=preview` plus a stable list of reason
codes. The requested mode, derived status, and reasons are serialized into grid
and image provenance. §8 gives the complete transition table.

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
reaction-rate field, with a line/band SPD — a genuinely spectral effect an
RGB pipeline can only fake.

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
- **Momentum** with buoyancy formulated against the ambient hydrostatic
  state: body force (ρ_g − ρ∞) g, so the unignited far field is exactly
  quiescent and no spurious hydrostatic adjustment flows arise.
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
  (§3.7). Constituent sensible-enthalpy diffusion, radiative loss, and latent
  exchange enter S_div at their accepted end-of-stage values (§3.4–§3.5),
  keeping scalar, energy, and constraint operators consistent within a stage.
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
Because b(Z) is affine, the discrete elemental flux assembled from the same
face gradients is exactly J_b=−ρ_totD∇b(Z); J_Z is not evaluated by an
independent stencil. This is what makes the summed elemental and Z fluxes
identical when aerosol loading varies.

| field | symbol | equation / source | sink | consumed by |
|---|---|---|---|---|
| mixture fraction | ρ_tot Z | conservative total-mixture scalar; fuel-inflow BC | — | property lookups; elemental feasible set; diagnostics (incl. the Y_O consistency check below) |
| fuel gas mass | q_F=ρ_gY_F | fuel-inflow BC; total-mixture diffusion | reaction (below) | reaction rate |
| oxygen gas mass | **q_O=ρ_gY_O (prognostic — r8)** | ambient/entrainment BC; total-mixture diffusion | gas reaction (s_st,eff per §3.4); soot burnout (2.667·Δc_ox) | reaction rate; burnout; products/W̄ element balance |
| radiating product gas masses | q_CO2, q_H2O | primary reaction and soot burnout, atom-balanced; total-mixture diffusion | — | W̄/EOS; gas-band κ_P from actual history (§3.5), never Z alone |
| total sensible-energy density | ℋ_s = ρ_gΣY_kh_{s,k}+Σc_ih_{s,i} | q̇‴_step + q̇‴_lat; conservative enthalpy transport | q̇‴_rad (§3.5) | **T = T(ℋ_s, gas+aerosol composition)** → EOS, buoyancy, Planck emission (§4.2) |
| carbon aerosol mass concentration | c_carbon | gross formation (§3.4) | oxidation (§3.4) | exported; renderer blends hot/cool optical regimes with φ(T), while both absorptive regimes emit by Kirchhoff (§4.1–§4.3) |
| condensed organics mass concentration | c_condensed | condensation of Y_cv per the §3.4 saturation rule | re-evaporation per the same rule | exported; droplet-preset scattering (§4.3) |
| condensable-vapor gas mass | q_cv=ρ_gY_cv | y_cond·δm_F with reaction; re-evaporation from c_condensed (§3.4 saturation rule) | condensation to c_condensed | conserved condensable inventory |
| reaction rates (derived) | q̇‴_gas, q̇‴_sox | conservative step rates (closure below — q̇‴_inst is closure-internal) | — | total q̇‴_step = gas+sox → S_div, ℋ_s, Q̇_tot, §3.5; **q̇‴_gas exported as `reaction`** → chemiluminescence (§4.4) |
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

This expression chooses the reacted fuel mass
δm_F = ρ_g,begin·ΔY_F; it is **not** applied by subtracting mass fractions in
place. Fuel, O₂, gas products, gross carbon, condensable vapor, gas total mass,
and ℋ_s are updated as conservative mass/energy densities from that one δm_F,
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
diagnostics use the **total q̇‴_step**; **chemiluminescence and the
exported `reaction` channel use q̇‴_gas only** — excited CH*/C₂*/CO₂*
radicals come from the gas flame, and driving the blue sheet with
soot-burnout heat would put chemiluminescence in the wrong place. §3.8's verification tier includes a **timestep-refinement
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
  - **Piloted route:** eligibility is T > T_pilot (**T_pilot ≈ 600 K**,
    per-fuel input) — reached only via the resolved heat-transport PDE —
    and connectivity is evaluated by a **same-step connected-component
    flood fill** over currently-eligible cells, seeded at the pilot region
    and at reacting cells, using the **6-neighbor face-adjacency stencil**.
    (Second external round's catch: r6's previous-step adjacency was still
    Δt-dependent — a connected region already above T_pilot ignited one
    cell per step, so refining Δt accelerated the front. With flood fill,
    eligibility is a *state* property spread by the PDE, and a
    pre-heated connected region igniting at once is the correct limit,
    not an artifact.) Front convergence remains gated by the §3.8
    timestep-refinement test.
  - **Spontaneous route:** an isolated cell reacts only above the true
    auto-ignition threshold T_AIT (≈ 700–900 K for common gaseous fuels;
    wood volatiles have literature AITs down to ~500–600 K — per-fuel
    input).

  This is what prevents the fuel-bed rim and re-entrained cold pockets
  from burning spontaneously while letting the flame spread at a physical
  rate.
- **Extinction gate (FDS critical-flame-temperature test):** a cell that
  *would* react is suppressed if adiabatic combustion of its mixed contents
  cannot reach T_CFT (empirical, ≈1700 K for hydrocarbons) — this kills
  vitiated/over-diluted mixtures.

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
C/H/O polytope nor unit-consistent across gas fractions and aerosol
concentrations): feasibility is defined by the **element matrix E** over
the full state — gas species mass fractions plus aerosol concentrations
converted as c/ρ_g — requiring componentwise E·y ≥ 0 and the C/H/O
totals fixed by **(1 + χ_load)b(Z)** under the total-mixture convention
above; enforcement is the **invariant-domain-preserving coupled
finite-volume FCT scheme pinned in §3.7** (one face coefficient shared by
the full state, hence globally element-conservative by construction), with
reaction/burnout updates capping consumption by
availability (§3.3/§3.4). V3 gains a multicomponent fuel/air/aerosol
interface test checking local realizability and global C/H/O totals.

**The gas-phase rate q̇‴_gas is exported as the `reaction` grid channel** — it both
locates the chemiluminescent sheet for the renderer (§4.4; renderer-side
Z-windowing was rejected as resolution-sensitive) and serves as the
flame-height diagnostic (§3.8). Export semantics: **instantaneous snapshot
at frame time; sub-frame aliasing accepted and documented** (24 fps is
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
  Soot advects, then
  **mixing-limited oxidation with a stated rate equation** consumes the
  *transported* Y_O (§3.3):

  > Δc_ox = min( c_carbon , ρ_g·Y_O/2.667 ) · (1 − e^{−Δt/τ_mix}),
  > active only where T > T_ox (≈ 1300 K — soot oxidation effectively
  > freezes below it),

  releasing Δh_soot·Δc_ox into ℋ_s (= q̇‴_sox, §3.3) and its CO₂ into the
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
    surrogate; per-fuel override in metadata), formation and sensible
    enthalpies for the ℋ_s bookkeeping, a heat of combustion Δh_cv for
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
    One split-first-order pass per stage evaluates saturation at the
    beginning-of-stage T; halving Δt must halve the split error. The
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

Production, advection, oxidation, and condensation/evaporation are applied
in a fixed documented operator order so escaped-soot mass is well-defined.

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
residual. The γ term handles the post-fire regime continuously: as
Q̇_tot → 0, γ → 1 and the sink reverts to the unscaled optically-thin
e(x) (a dying fire's hot smoke keeps cooling; χ_r is meaningless without
heat release). Degenerate cases pinned: Σe = 0 ⇒ q̇‴_rad ≡ 0; β > 1 is
*allowed* but logged (it means the modeled emissivity cannot deliver the
measured χ_r — a model-inconsistency diagnostic, not a silent clamp).
**Discrete closure — a split-first-order scheme, named as such** (third
external round: beginning-of-stage normalization is exact only *before*
the semi-implicit temperature update, so the *accepted* sink deviates from
the budget by a first-order-in-Δt splitting error): β and γ are computed
**once per stage from beginning-of-stage state and frozen**; the per-cell
semi-implicit integration runs against frozen β; the *accepted*
post-integration sink is the single value consumed identically by the ℋ_s
update, S_div, and diagnostics (the same one-rate discipline as q̇‴_step,
§3.3). No global iteration; the splitting error is gated, not assumed:
**per-step accepted-vs-budget deviation ≤ 2 %, and halving Δt must halve
it** (first-order convergence check in the V-tier) — the gate applies
**only while β ≥ γ** (in the γ-dominated post-fire branch Q̇_tot → 0 while
plume cooling remains, so relative-to-budget deviation is intentionally
unbounded there; round 4). Harness
gates: integrated radiative fraction vs χ_r, and a per-column
optical-thickness monitor (κ_P·L across the flame) so the
optically-thin-shape assumption is measured, not presumed.
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
  appropriate standard of practice. The sink is integrated per-cell
  semi-implicitly (linearized in T) because the T⁵ soot term is stiff under
  explicit Euler (§3.7).

### 3.6 Boundary conditions and domain

The most notorious practical trap in fire LES; specified accordingly:

- **Fuel bed (bottom, source patch):** prescribed fuel mass flux ṁ″_F with
  prescribed injection enthalpy (composition and temperature of the
  volatiles); the bed plane outside the patch is a no-slip, **adiabatic**
  wall (an isothermal-cold bed measurably changes flame standoff; adiabatic
  is the deliberate, stated choice at this fidelity).
- **Lateral faces: pressure-based open boundaries** — ambient air enters at
  sim-determined entrainment rates; the boundary pressure is the ambient
  hydrostatic state (consistent with the (ρ_g−ρ∞)g formulation, which makes
  the far field exactly quiescent), with the standard inflow/outflow
  asymmetry on the dynamic-pressure condition (FDS's Bernoulli-type H
  treatment: total head prescribed on entraining inflow, static on
  outflow — the distinction is what stops open-boundary reflections).
- **Top: open outflow** with the same hydrostatic-consistent treatment.
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

  which is equivalent to E q=ρ_totb(Z) because b(Z) is affine. Compute and
  freeze an orthonormal nullspace basis N_A for A at schema construction;
  encode each cell as ξ=N_AᵀQ, apply the MC reconstruction to ξ, and decode
  every face state as Q_f=N_Aξ_f. Thus both high- and low-order vector fluxes,
  and therefore their difference, are tangent to every exact mass/C/H/O/Z
  equality before limiting. Projecting an independently reconstructed vector
  after the fact is forbidden because it changes its stencil and can violate
  face conservation. Their difference is the antidiffusive face flux. The limiter is a
  **multiconstraint Zalesak nodal-budget algorithm, not independent face-local
  clipping**: first compute the complete low-order update and all raw incident
  antidiffusive corrections A_cf. Exact equalities are already satisfied by
  construction and are **not** represented as two zero-budget half-spaces.
  For every remaining inequality a_r·q≤b_r, only incident corrections with
  a_r·A_cf>0 consume the upper budget Q_cr=b_r−a_r·q_L; its lower bound is
  represented as a separate inequality and treated identically. Accumulate
  P_cr=Σ_f max(0,a_r·A_cf) and set R_cr=min(1,Q_cr/P_cr), with P_cr=0 defined
  as R_cr=1. Each face receives **one α_face shared by every
  component**, equal to the minimum applicable donor/receiver R over all
  inequalities whose projected correction is positive. Thus the aggregate
  of all incident corrections—not each face in isolation—fits every nodal
  budget. The constraint rows include the §3.3 element polytope, inventory
  nonnegativity, and the linear sensible-energy bounds

  > ℋ_s ≥ Σ_j q_j h_{s,j}(T_amb),
  > ℋ_s ≤ Σ_j q_j h_{s,j}(T_ad).

  The low-order state must already be feasible; otherwise the step is rejected
  before antidiffusion.
  The same signed α_face correction is applied to both cells, so each species,
  C/H/O, and total sensible energy are globally conservative to fp64 reduction
  tolerance. Independent per-component clipping or post-advection temperature
  clamping is forbidden: either destroys the coupled element/energy ledger.
  Semi-Lagrangian MacCormack remains a visually useful **debug-only** fallback
  and may not be used for validation or predictive grids.
- **Advection — momentum** (unstated in revision 2, flagged): second-order
  central, kinetic-energy-conserving discretization on the staggered grid —
  the standard LES practice; upwinded/dissipative momentum advection kills
  puffing exactly as surely as dissipative scalar advection.
  **Advection–reflection is dropped**: it is derived for constant-density
  incompressible flow (orthogonal L² projection onto a divergence-free
  space); neither assumption holds here, and no published variable-density
  validation exists. Semi-Lagrangian remains a debug fallback.
- **Conservation accounting:** the harness tracks global fuel, C/H/O, aerosol,
  and total-sensible-energy budgets. On a closed or periodic domain, advection
  error must be at fp64 reduction tolerance; the ≤2 % flow-through tolerance
  applies only to the complete open-boundary/reacting balance after explicitly
  accounting for boundary fluxes, reaction heat, latent exchange, radiation,
  and resident chemical potential. It is not permission for scalar-advection
  drift.
- **Time stepping:** two-stage predictor–corrector (FDS-style), **projection
  applied at every stage**. Step size: min of advective CFL, the
  buoyant-acceleration limit Δt ≲ √(2δx/g′₊) (g′₊ = max(g′, 0) per §3.3 —
  the signed form goes non-real over cold dense fuel; the limit is simply
  inactive where g′₊ = 0), and
  the explicit diffusion limit Δt ∝ δx²/ν where diffusion is explicit (the
  candle treats diffusion semi-implicitly — the δx² limit at 0.25 mm cells
  is prohibitive). Radiative sink: per-cell semi-implicit (§3.5). Sim Δt is
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
  manufactured solution** — the S_div-from-discrete-updates gate.
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
  than donor-cell. This prevents an all-α=0 implementation from passing the
  conservation checks while silently collapsing to first order. Every case
  checks local feasibility and global mass, gas species, aerosol inventories,
  C/H/O, ρ_totZ, and ℋ_s to fp64 reduction tolerance. V2/V3 additionally run
  a periodic uniform-temperature mixture with unequal constituent c_p:
  diffusion must leave T uniform while conserving ℋ_s, repeated under two
  different T_ref choices to expose a missing J_h term.
- V4. Closed reacting box: atom, gas+aerosol mass, ℋ_s, chemical potential,
  and EOS balance to round-off at **three checkpoints** — immediately after
  primary formation, after partial soot burnout, and at the accepted final
  state. At every checkpoint the invariant is

  > Q_released + M_carbon Δh_soot
  >   + (M_cv + M_condensed) Δh_cv = M_fuel,reacted Δh_c.

  The corresponding invariant is
  **M_O2,consumed + 2.667 M_carbon + s_cv(M_cv+M_condensed)
  = s_st M_fuel,reacted**. The test includes inert-aerosol heating/cooling
  at χ_load=1 %, homogeneous soot formation/burnout with unchanged Z,
  isothermal and adiabatic vapor↔condensate cycles (including the saturation
  cap), and closed-box gas↔aerosol EOS/phase-transfer checks. “Released heat =
  LHV” is required only after **all** withheld chemical inventories have been
  oxidized by a test-only completion step; the production model does not burn
  condensables.
- V5. Radiative cooling of a single cell vs an **independent numerical
  wavelength integral** of 4π∫κ_λ[B_λ(T)−B_λ(T∞)]dλ, plus the analytic
  near-ambient derivative. For a hot-carbon-only φ=1 fixture with
  κ_λ∝1/λ the gate requires the f_v(T⁵−T∞⁵) law; merely integrating the implementation's own §3.5 ODE is
  not an independent test. A separate homogeneous CO₂/H₂O fixture requires
  absolute band-integrated cooling against an independent reference for the
  pinned gas-table state, plus the same-Z/different-product-history check.
- V6. **Timestep-refinement convergence** of heat-release and
  ignition-front histories at fixed grid (§3.3's discrete-realization and
  ignition-gate requirements).

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
   fit with a virtual origin z₀, on the taller §3.6 plume domain.
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
>              σ_s = ω·σ_a/(1−ω), plus §4.2 emission;
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

### 4.4 Chemiluminescence: a second, line-spectrum emission field

A separate emission term anchored to the exported reaction-rate channel
(§3.3):

> ε_chem(x, λ) = (η_chem / 4π) · q̇‴_gas(x) · S_chem(λ),  (q̇‴_gas = the
> exported `reaction` channel, §3.3 — gas-flame rate, not soot burnout),
> with **∫_{λa}^{λb} S_chem(λ) dλ ≡ 1** (S_chem in nm⁻¹) over the SPD
> dataset's stated normalization interval [λa, λb],

dimensionally pinned per the external review: η_chem is the fraction of
heat release emitted as chemiluminescence (isotropic, hence the 4π), and
the unit-normalized SPD makes the units of ε_chem exactly those of §4.2's ε
— without the normalization convention, implementations can differ by 4π
or by 10⁹ (metre-vs-nanometre SPD). S_chem is assembled from CH* (431 nm,
390 nm), C₂* Swan bands, and the broad CO₂* continuum (§2.3 table).
**η_chem is defined over that same [λa, λb]** — total-vs-band ambiguity
was an external catch: if the renderer clips to its own visible band it
renders only the in-band fraction and **must not renormalize** (clipping
must lose the out-of-band power, not redistribute UV/IR into the
visible). **The HRR denominator convention is pinned too** (round 4:
q̇‴_gas is Δh_c,eff-based, so an η calibrated against *total* measured
HRR under-lights the blue sheet by the withheld fraction — ~15 % at
aromatic yields): **η_chem is defined against the primary effective
(gas-phase) HRR**, and measured total-HRR-based values are converted at
dataset-adoption time by the Δh_c/Δh_c,eff ratio; the metadata records
the convention alongside η itself. η_chem, the SPD dataset identifier, its wavelength units,
[λa, λb], and the per-band fractions all ride in the §8 metadata — a free
scale knob would break the §1 "exposure only" claim. The ∼10⁻⁴-of-HRR anchor (§2.3) is an
order-of-magnitude prior for η_chem; per-fuel calibration from absolutely
calibrated spectral measurements is §12 still-open item 1. This makes the
flame
base blue *for the right reason*, and premixed pilot flames renderable with
zero soot.

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

### 7.0 Phase gating (adopted from the review verdicts, r6–r13)

Mechanical multi-channel-grid scaffolding, the pinned Planck kernel, and
the collision-estimator work may start any time. **Predictive radiometry
(Phase A proper) starts only after** the implementation matches: the
Planck kernel + E(m) dataset pinning (§4.2), a frozen constituent optical
preset v1 satisfying §12 Q2 (the numerical values currently listed there are
non-predictive fixtures), scene-unit propagation and
the g/m³→SI conversion gates **in both §8 and the §4.3 optics formulas**
(round 5 caught a surviving 1000× line), the repaired trilinear accessor
(**landed**, commit `2fba2b48`; §7.1 step 0 verifies the fire path uses
it), the **constituent aerosol inventories with derived φ(T) optics, the
φ-aware 7-point quadrature/panel-splitting rule, the exact per-collision
**`MakePhaseClosure(x,λ)` / Pel-band closure API** carrying local constituent
weights and wavelength-bound g, **and the pinned φ-sup extinction-majorant bound**
(§3.4/§4.3, §7.1 steps 1/4, §7.2.3), the chem spectral contract with the
length-allocated Gauss–Kronrod budget controller and the effective-HRR η
convention (§4.4, §7.1 step 3), **plus a frozen absolutely-calibrated per-fuel
chem record and absolute chem-slab power gate whenever chem is enabled**. Until
§12 Q1 closes for a declared fuel, `chem_model=none` is predictive **only**
when a pinned measurement reports a 95 %-confidence upper bound below both
1 % of that fuel/case's measured 380–780 nm radiant power and the absolute
radiance gate's uncertainty at every gated wavelength. Otherwise a missing
calibrated chem record is a predictive hard error; synthetic η/SPD records are
estimator tests and preview assets only. Also required: the no-silent-cap Pel/NM
distance-sampler repair, removal of all density floors via the log-domain
contract, and forced-cap/tail gates in §7.1 step 2, the trilinear-only rule for physical
channels (§8), the completed versioned optical/metadata contract with
constituent-specific presets and the condensable record (§8/§12),
dual-surface coverage with correct event ordering + HWSS hard-disable
(G11), and the absolute slab/unit-invariance tests (§7.1 gates). **Phase B** additionally
requires: the replacement segment-wise shadow walk with no silent caps
(§7.2.1, incl. the wrong-origin and step-cap-continuation tests),
direction-independent lobe preselection with the marginal Σs_ℓp_ℓ
directional pdf **and both the RIS and surface-guiding disables at
competing vertices** (§7.2.2, round 5), the strict null-boundary class
with all non-null interfaces blocking (§7.2.2), the **two-bit weight-1
state (competitionAvailable, continuationSingular)** (§7.2.2, round 5),
independent pivot/endpoint draws (u_m vs Y), shared-guide-state ordering,
p_march support/survival, distinct q_m^V versus equiangular a coefficients,
the near-collinear equiangular repair, labeled medium selection — plus the §7.2.7
configuration matrix. **Phase C sim and motion-sequence
work** additionally require: the r8 P0 correction (transported Y_O +
withheld-stream primary coefficients — extended r9 to condensables),
the **r11 phase-transfer/EOS/energy closure** (gas-density definition,
total-mixture ρ_totZ, conservative phase-transfer masses, latent power,
aerosol heat capacity in ℋ_s/C_T and S_div, closed-box gas↔aerosol tests), the
condensable pseudo-species thermochemistry and joint elemental feasible
set (§3.3/§3.4), the y_form/y_cond multi-resolution calibration protocols
with their scale cross-prediction gates (§3.4), §3.2's discrete S_div derivation, the §3.3
two-rate/flood-fill/exponential-update specs, §3.5's total-support budget
closure with the branch-conditioned split-first-order gate, §3.8's
executable verification tier and absolute/CI-based end-to-end gates —
including: the corrected ṁ‴_gas (no S_div double-count), the χ_load≤1 %
hard predictive boundary, the all-intermediate-state chemical-potential V4
gate, the mass-conservative saturation law and condensable record, the
conservative finite-volume coupled FCT scheme, and the calibration cases —
including r13's conservative-density owner, constituent sensible-enthalpy
diffusion, invariant-coordinate FCT reconstruction and nondegeneracy/order
gates, plus the versioned simulator gas-opacity record — and §8's
fixed sequence epoch with nominal/shutter time separation,
halo-serialization/blur-disable policy, the render-global blur rule (no
volume NEE/equiangular, uncapped NM pure DT, Pel nominal-only, independently
seeded cap-free ratio-tracked chem),
timestamped probes, residency model, and trilinear rules.
The §8 fidelity state machine, complete digested sequence manifest, and
single pre-worker `PrepareMediaForRender` lifecycle hook are also Phase-C
entry gates rather than integration details left to implementers.

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
1. **Multi-channel heterogeneous media** (G2): named scalar channels per
   medium (`carbon`, `temperature`, `condensed`, `reaction`), each a `Volume<>`
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
2. **Collision-based spectral emission** (G1 + G9). One general scoring
   rule, valid for every distance sampler in use: at the sampled scatter
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

   **The no-scatter outcome scores no emission — and the existing
   no-scatter emission block is deleted.** The rule is exactly unbiased as
   stated *because* the scatter-sample expectation over [0, L] already
   integrates the full segment emission (E = ∫₀^L p·[ε_thermal·T_det/p]dt =
   ∫₀^L T_det·ε_thermal dt); the current code's no-scatter branch
   (RayCaster.cpp:1228–1260) adds a full-segment analytic integral whose
   role is fully absorbed by that expectation — an implementer who "keeps
   the branch and upgrades the scoring" double-counts by
   ≈Tr(L)·∫T_det·ε_thermal.
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
   without adding extinction), so it is collected by **deterministic line
   integration along every marched segment**:
   ∫ T_det(t)·ε_chem(t,λ) dt, at MIS weight 1 (see §7.2.3 for why NEE
   never competes for it). **Quadrature honesty** (external-review
   catches, both rounds): per-knot Gauss–Legendre is exact for the
   *optical-depth* integrand but NOT for e^{−τ(t)}·ε_chem(t) — at
   per-panel optical thickness 20 the 5-point rule is ~2.5 % low, at 50 it
   is ~43 % low — **and optical-depth-driven subdivision alone is
   insufficient** (a thin reaction sheet in a nearly transparent medium
   has τ ≈ 0 but a sharply peaked integrand). The committed scheme:
   composite quadrature whose panels traverse the **union of the
   reaction-channel and extinction-channel knot lattices**, subdividing on
   an embedded nested-pair error estimate with the **pinned per-panel
   acceptance test** (third external round — "scaled to the contribution
   floor" was implementation-divergent prose):

   > |I_h − I_l| ≤ a_λ,panel + 10⁻³·|I_h|,
   > a_λ,panel = 10⁻⁴ · Î_seg(λ) · (L_panel,initial / L_seg),

   where I_h/I_l are the **Gauss–Kronrod 7/15 embedded pair**'s panel
   estimates and Î_seg(λ) is a pilot one-pass estimate of the whole
   segment's chem integral (units: spectral radiance,
   W·m⁻²·sr⁻¹·nm⁻¹). The absolute budget is allocated **by initial panel
   length, fixed before any subdivision** (children split their parent's
   allowance pro rata) — round 5: dividing by the *current* panel count
   let early-accepted panels keep stale larger allowances, making total
   accepted error traversal-order-dependent and unbounded relative to
   the 10⁻⁴·Î_seg target; the length-allocated fixed budget makes the
   summed accepted error ≤ a true global budget. This is **controlled bias within the
   declared tolerance — not unbiasedness**; the randomized-quadrature
   alternative (T·ε/q) is recorded as the fallback if the tolerance
   proves contentious. Segment
   semantics, stated precisely because they are where implementations
   diverge: the integral runs over the **full segment**
   [0, min(surface hit, medium exit)] **regardless of the scatter
   outcome** — it estimates the RTE's additive emission term independently
   of the scatter sample, and the continuation segment leaving a scatter
   vertex (new direction) creates no overlap; truncating at the sampled
   scatter distance undercounts. Likewise, **every early-out in the
   distance-sampling code (e.g. the equiangular zero-contribution return)
   must still score the segment's chem integral** — dropping it there
   biases exactly the zero-soot showcase this step exists for. Collected
   with controlled bias (the declared quadrature tolerance above) for
   every path with a segment through the medium; no collisions required.
   This deterministic Gauss–Kronrod construction is **blur-off only**. When
   velocity blur is active, §8 replaces it with the independent uniform-distance
   plus ratio-tracked-transmittance estimator; the two estimators are never
   combined on one segment. Estimator tests may use a clearly marked synthetic
   SPD, but predictive enablement requires the §7.0 per-fuel record and an
   optically thin uniform reaction slab whose integrated spectral power equals
   η_chem times the pinned effective gas HRR over [λ_a,λ_b] within the stated
   numerical tolerance.
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
  collision, with three externally-reviewed refinements to its density:
  - **p_ω is the actual direction proposal — with the ordering and
    RIS constraints that make that evaluable** (second external round):
    volume NEE runs at the scatter vertex *before* the guided continuation
    is sampled (verified, `PathTracingIntegrator.cpp` ~:1761ff), so the
    NEE-side counterfactual p_march can only match the later march
    proposal if the **guide state for the vertex is initialized before
    NEE and shared** between the NEE weight evaluation and the actual
    march sampling — a named work item, not an assumption. `RayCaster`
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
    before NEE) — and the march directional pdf is the **full marginal
    p_ω(ω) = Σ_ℓ s_ℓ·p_ℓ(ω)**, with the **total BSDF** evaluated over
    that marginal in both the NEE numerator and the continuation
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
    diffuse+glossy and coated-material NEE-on-vs-off equality tests,
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
    p_march(y) = p_ω(ω)/‖x−y‖² · (Π_{i<k} P_{0,i}) · p_{t,k}(s_k), where
    P_{0,i} is segment i's no-event probability — under the 50/50 mixture
    that is 0.5·S_i with **S_i = e^{−τ_i}, segment i's deterministic
    transmittance** (the DT no-scatter atom's mass), since only the
    delta-tracking branch owns the no-scatter atom (the equiangular branch
    always proposes a scatter).
  - p_t per segment is the pdf of the sampler actually in use — pure DT
    (σ_t·exp(−τ)) or the 50/50 balance mixture (§5.1) — evaluated
    deterministically via `EvalDeterministicOpticalDepth`, the same
    deterministic-denominator discipline the existing MIS uses.
- **Volume NEE**: p_V(y) from 7.2.3.

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

March-collected emission takes MIS weight p_march²/(p_march²+p_V²) iff
competitionAvailable ∧ ¬continuationSingular; weight 1 otherwise (camera
primaries, delta continuations, NEE-disabled vertices) — else the
directly-viewed flame dims or glossy marches double-count. (Revision 2's "the weights do the
bookkeeping, no flag needed" was wrong — the flag *selects which partition
applies*.) Explicitly scoped out: **flame seen through a refractive
boundary** — NEE cannot reach through refraction (straight shadow rays), so
those paths are march-only at weight 1 by this same rule; VCM-class
transport for volumetric SDS is out of scope (§5.2).

**7.2.3 The emission importance structure — two-level, thermal-aerosol only.**
Chem remains excluded below. For each voxel, a fixed 2×2×2 Gauss rule in
space plus the pinned wavelength quadrature computes the nonnegative proposal
estimate

> Ĩ_v = V_v ∫ ε̃_thermal,v(λ)dλ.

over the camera-visible band. This is an importance approximation, not a
radiometric integral used in the contribution. The channel/temperature
interval bounds also produce a finite conservative emissive-power upper bound
U_v over the same voxel and band. U_v>0 whenever trilinear interpolation can
produce nonzero thermal emission, even if the quadrature nodes all miss a hot
corner. Define η=2⁻¹⁰ and

> w_v=(1−η)Ĩ_v+ηU_v,
> W_m=Σ_v w_v,  q_v=w_v/W_m.

If W_m=0 the medium has no thermal-emission strategy. Otherwise q_v>0 wherever
ε_thermal can be nonzero, including a hot corner with a cold/zero-emission
voxel center. Per-cell weights on the majorant topology are
Φ_c=Σ_{v∈c}w_v
(units W/sr — a radiance-derived weight; multiply by 4π for emitted power
when comparing against §2.3-style power anchors; the pmf is unaffected by
the constant). Majorant grids are capped at 32 *cells per axis* (§5.1) —
at 512³ a cell spans 16³ voxels whose ε varies by orders of magnitude
across the flame sheet, so within-cell *uniform* sampling would leave large
variance the cell CDF cannot address. The structure is therefore
**two-level: cell CDF (Φ_c/W_m), then per-voxel w_v/Φ_c CDF within the cell, then
uniform within the voxel** — the per-voxel pass that builds Φ_c yields the
second level at the same O(N) cost. Since Σ_cΦ_c=W_m,
**p_V(y)=q_v/V_v**, an O(1) lookup whose construction exactly matches its
sampler. **p_V is λ-independent by construction**
(band-integrated importance), so it is a valid — merely suboptimal —
importance for every per-λ path; radiometric, not photometric, weighting
(the spectral pipeline's NEE importance shouldn't bake in a photopic
curve). **ε_chem is excluded from Φ and from the NEE estimator entirely**:
chemiluminescent power is ∼10⁻⁴ of soot thermal power (§2.3), so its
*illumination* of the surroundings is negligible and it is collected solely
by the deterministic line integral of §7.1 step 3 at weight 1 — which is
also what makes that line integral MIS-free (NEE never competes for the
chem term, so there is no double count by construction). Extinction-majorant
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
vertex whenever at least one emissive medium has W_m>0**: it chooses a labeled
medium with q_m^V=W_m/Σ_nW_n and then an independent endpoint from p_m. This
attempt sets `competitionAvailable` even if the sample is inactive, occluded,
or zero-valued. Separately, the existing equiangular strategy pivots on a
positional-light/medium mixture with coefficients a_i and a_m. A medium's
weight is its emitted-power estimate **4πW_m in watts**; W_m itself is the
§7.2.3 radiance-derived W/sr quantity, and the 4π is required when comparing
it with positional-light powers. The constant cancels only in a medium-only
normalization. The flame thus joins as one more entry in this **equiangular
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
as-is. Compute perpendicular D² with a nonnegative robust difference and let
S=max(L,|pivot−rayOrigin|). If D≤32√ε_machine·S, both `Sample` and `Pdf` take
the same uniform-on-[0,L] fallback with density 1/L; otherwise both use the
same robust D² in the equiangular formula. The branch predicate is shared
code. Mixing a floored D in sampling with raw or negative D² in evaluation is
forbidden. On-ray, near-on-ray on both sides of the threshold, and large-scene-
scale tests require finite nonnegative samples/PDFs, numerical normalization,
and Sample/Pdf agreement.

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
distribution), a one-hot-corner/seven-cold-corners voxel whose center is
non-emissive but whose q_v must remain positive, and the multi-media
labeled-density check (7.2.5). The point-light+flame fixture additionally
forces the equiangular label to the point light while confirming that the
independent volume endpoint was still attempted and march MIS used q_m^V;
on-ray/near-on-ray point pivots exercise the finite normalized fallback in
§7.2.4.
Also gated here: the **shadow-walk step-field tests** (>4 overlapping
media, >16 boundary crossings, the **wrong-origin interrupted-global-
medium case**, and **forced step-cap continuation** past the ratio
tracker's 1024-step budget — §7.2.1's no-silent-caps replacement walk)
and the **diffuse+glossy and coated-material NEE-on-vs-off equality
tests** (§7.2.2's direction-independent lobe preselection).
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
existing animation render workflow. Progression: laminar candle (DNS) →
puffing pool fire → turbulent plume, gating each on §3.8.

**Frontends.** The standalone §3 simulator is the only first-class predictive
Phase-C source. Blender/Mantaflow interoperability is explicitly
**non-predictive preview** by default: its normalized `flame` and `temperature`
grids do not contain an invertible mapping to W/m³ and kelvin, and float
transport fixes quantization rather than restoring that lost scale. Such an
import is tagged `physical_mapping = heuristic`, cannot use a versioned
predictive optical/chem record, and cannot pass §3.8's absolute gates. A future
exporter may qualify only by writing absolute T, carbon/condensed mass,
reaction power, velocity, and the complete §8 sidecar at the simulation source;
the importer then performs round-trip unit/hash tests without fitting an
affine scale in RISE. The existing bridge's 8-bit path remains disqualified.
Mac/Windows GUI and Android are explicitly deferred—fire renders from scene
files everywhere; native UI follows once the format stabilizes.

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
| `reaction` | q̇‴_gas (§3.3) | W/m³ | **fp32** (spans to ∼10⁸; overflows fp16's 65504 max) |
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
it requires the O(N) per-voxel pass of §7.2.3, which runs once per frame in
the frame-advance step. (Revision 3 claimed the CDF avoided a
full-resolution pass; that contradicted §7.2.3 and was wrong.)

**Metadata completeness rule:** two physically different grids must never
carry indistinguishable metadata. Each sequence stores:

- world transform, sim Δt/frame rate, and a versioned sequence manifest. The
  manifest contains the complete time map
  (t₀,α,t_scene,0,Δt_frame,i₀), an ordered explicit frame-index/file list
  (not merely a printf pattern), frame count, first/last indices, a digest for
  every frame, the end-clamp policy, velocity-halo width, and outside-halo
  policy. Clamp applies to the nominal and shutter-mapped time **before** the
  §8 advection offset is formed; an index inside the declared range but absent
  from the explicit list is a hard load error;
- per-fuel y_form, y_s, y_cond, χ_r, T_pilot/T_AIT, Q̇_ref, ρ_soot,
  oxidation constants (T_ox, 2.667 kg-O₂/kg, 3.667 kg-CO₂/kg, 32.8 MJ/kg),
  channel scales, calibration dataset/protocol IDs, and scale/resolution
  cross-prediction results;
- `aerosol_humidity_model = none`, the χ_load≤1 % support flag, and the
  condensable record (formula/W_cv, formation and sensible enthalpy
  references, Δh_cv, s_cv, L_cv, T_sat,ref, p_sat,ref, and chem-HRR convention);
- one versioned optical record: E(m) ID/hash shared by sim and renderer,
  separate cool-carbon and condensed-organic full-spectrum k_m/n/ω/g data,
  their IR extensions, and the φ(T) band;
- the **simulator-only gas-opacity record**: CO₂/H₂O table ID/hash,
  wavelength/T ranges, p₀, composition basis, pressure/broadening convention,
  interpolation and overlap rule, plus the 380–780 nm negligibility bound;
- the chem record or explicit `chem_model=none`: η_chem, SPD dataset ID,
  wavelength units, [λ_a,λ_b], per-band fractions, or the measured negligible-
  chem upper-bound record required by §7.0; and
- requested `fidelity_mode`, derived `fidelity_status`, stable reason-code
  list, and a provenance hash over the grids and every record above. Hashing
  uses one canonical schema encoding (UTF-8, sorted object keys, fixed binary
  float encoding, no insignificant whitespace); validators reject unknown
  schema versions rather than hashing implementation-native JSON.

**Fidelity transition table:** `predictive` is permitted only for the
standalone absolute-unit simulator source, spectral NM transport, matching
frozen optical/chem/condensable/gas records, intact hashes, all required
channels, χ_load≤1 %, dry-aerosol scope, and passed predictive gates. A
heuristic/normalized source, any record override or hash mismatch, Pel or
HWSS transport, missing required data/channel, out-of-scope loading/water, or
an unqualified `chem_model=none` produces the corresponding preview reason.
When the requested mode is predictive any such reason is a hard preflight
error; when it is preview the render proceeds with all reasons embedded in
output provenance. Velocity blur is predictive only on the §8 spectral-NM
pure-DT path; Pel's nominal-frame fallback remains preview. The simulator
applies the same rule before writing a predictive grid, so an invalid sequence
cannot acquire a predictive label downstream.

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

**Formats:** OpenVDB, **native in-core on desktop behind an optional build
capability, with an offline float transcode fallback** for mobile and small
test assets (adopted from the external review, closing the former §12 Q5:
dense
transcode of 512³ multi-channel frames at ≈2.7 GB/frame is not a credible
general pipeline; the OIDN submodule is the extlib precedent, and the
native choice adds `scripts/create_macos_release.sh` staging work per that
precedent).

**Motion blur:** velocity-grid Eulerian blur with an explicit consistency
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
defaulting to 1 (fire-sequence timelines authored in seconds); clamp against
the manifest's declared first/last indices before computing the shutter
advection offset; a missing/digest-mismatched frame in the explicit manifest is
a hard load error, not a skip. **Residency model, pinned — with nominal and shutter time separated**
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
the frame interval is rejected with a diagnostic). Gates: α ≠ 1,
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
**disables blur for that medium with a logged diagnostic** rather than
silently extrapolating. Lookup order pinned as clamp-to-halo-then-sample;
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

Chemiluminescence uses a separate unbiased blur-mode estimator instead of the
static deterministic quadrature. On every full marched segment [0,L], draw one
t∼Uniform(0,L), independently of the collision sampler, and score

> L · ε_chem(x′(t),λ) · T̂_ratio(0,t),

where x′ is the shutter-time warp and T̂_ratio is an independent spectral
ratio-tracking estimate using the same shutter-bounded majorant. “Independent”
means a disjoint RNG substream from the uniform t draw and from collision
tracking. The current `EvalTransmittanceNM` 1024-candidate partial-product
return is forbidden: Phase C owns a cap-free/state-preserving spectral ratio
primitive that continues from each watchdog chunk until t, even if Phase B's
shadow-walk repair has not landed. This remains valid when σ_t=0 and is averaged
if more than one chem sample is requested.
It has MIS weight 1 because the render-global rule disables volume NEE. No
distance-sampler early-out may skip the draw. Gates: chromatic Pel must prove
it stayed nominal/unblurred; spectral thermal and chem-only velocity-shear
slabs must match high-accuracy references; and nested static+blurred media,
including a static emitter behind a null-bounded blurred medium, must match
volume-NEE-on blur-off references within confidence intervals. A forced
>1024-ratio-event case is a RED test against the current partial-product
behavior and must converge to the cap-free reference. Ships in Phase
C only if per-frame cadence proves visually insufficient; grid interpolation
blur remains rejected because it fabricates density where none advected.

## 9. Scene-language sketch (illustrative, not final)

```
fire_medium
{
	name			campfire_volume
	fidelity_mode		predictive
	grid_sequence		media/campfire/frame_%04d.vdb
	channel_carbon		carbon
	channel_temperature	temperature
	channel_condensed	condensed
	channel_reaction	reaction			# optional, enables chem sheet
	channel_velocity	velocity			# optional, enables motion blur (§8)
	# Optional overrides replace WHOLE versioned metadata records. Loose
	# per-parameter soot/smoke/phase tuning is intentionally unsupported.
	optical_preset_override	presets/fire_optics_v1.json
	chem_preset_override	presets/methane_chem_v1.json
}

global_medium
{
	medium			campfire_volume
}
```

Conventions (verified against the registry): new parameters follow the existing
all-lowercase snake_case descriptor convention. Each override is a
schema-versioned, hashed record: the optical record contains soot E(m) plus
**separate** cool-carbon and condensed-organic k_m/n/ω/g entries; the chem
record contains per-fuel η_chem, SPD, normalization range, and provenance.
Loading either override logs that grid metadata was replaced, and the record
identity is written to output provenance. Overrides are calibration/ablation
features: predictive mode rejects a record whose hash differs from the grid
metadata, so a scene cannot tune smoke appearance while retaining the §1
fidelity claim. Spatial placement: for VDB
input the grid transform is authoritative; raw-slice input requires explicit
`bbox_min`/`bbox_max` as today. The printf-slot semantics change is called
out explicitly: in `grid_sequence` the slot is the **frame index** (VDB
packs Z internally), whereas legacy `volume_pattern`'s slot is the Z-slice
index — raw-slice *sequences* would need a second slot or per-frame
directories, and are out of scope.

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
   concrete lifecycle API: append
   `IScene::PrepareMediaForRender(nominalSceneTime,shutterOpen,shutterClose)
   const`; `Scene` owns a registry of render-preparable media populated by
   `Job` when a time-varying medium is constructed. The method builds a complete
   immutable frame state off to the side and atomically swaps it before worker
   launch; repeated calls with the same sequence identity and time triple are
   idempotent. The shared `PixelBasedRasterizerHelper` preparation seam calls
   it after animator evaluation, object `PrepareForRendering`, and
   `SetSceneTime`, but before `RayCaster::AttachScene`/worker dispatch. Every
   still, normal animation frame, interlaced field, explicit single frame or
   subrange, and AOV fallback re-entry uses that same seam; direct PT entry
   points must call the helper rather than duplicate the order. Tests compare
   selected frame identity across those entry modes and assert no prepare call
   occurs from a worker or per-sample path.
   Additionally (ARCHITECTURE.md:70): `IAnimator::EvaluateAtTime` per-sample
   evaluation is a documented pre-existing data race — **volume data must
   never join the per-sample animation path**; §8's read-only Eulerian
   velocity blur is the compatible design and this is its justification.
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
   evaluation item, not a validation channel. **Firefly policy:** validation
   renders are *unclamped* (clamping would corrupt the §3.8/§7.2.7 energy
   gates); Phase B's MIS is the principled variance mechanism, and path
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
| Multi-channel grid memory (dense 512³, 7 scalar fields — velocity is 3 — per §8 precisions ≈ 2.7 GB/frame) | fp16 where §8 permits; candle is small-domain DNS; sparse grids in Phase D |
| Quantization/normalization destroying physical units (the bridge's 8-bit lesson, §5.1) | §8 units + precision table; ingestion round-trip test asserting known values |
| RGB-path fire looks different from spectral-path fire | §7.1 step 4 declares spectral-first; RGB is an approximate preview path with a logged diagnostic (deterministic projection bias, §12 RGB-path decision) |
| Velocity blur silently mixes incompatible distance estimators | §8 disables volume NEE/equiangular render-globally, uses NM pure DT, keeps Pel nominal/unblurred, and gives chem an independent ratio-tracked estimator; nested static+blurred gates |
| Existing 1024-step tracker watchdog becomes an estimator cap | Phase A distance sampling and Phase C chem ratio tracking use state-preserving continuation to segment end; forced-cap RED tests cover Pel/NM and both transport surfaces |
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
- Chemiluminescence source → **sim exports the reaction-rate channel**
  (§3.3); amplitude is the metadata-pinned η_chem with a unit-normalized
  SPD (§4.4, r6); renderer-side Z-windowing rejected as
  resolution-sensitive.
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
- **VDB** (was Q5; adopted) → native in-core on desktop behind an optional
  build capability + offline float transcode fallback (§8).
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

1. **(combustion)** Per-fuel η_chem and the chemiluminescence SPD dataset:
   which absolutely-calibrated spectral measurements to adopt and version
   for the §4.4 band fractions. Selection criteria (accumulated rounds
   2–5): the dataset must provide **absolute integrated spectral power,
   its normalization limits [λa, λb], geometry corrections, and paired
   HRR** — relative band shapes or calibrated images alone cannot pin
   η_chem. Round-5 candidate: the Lai et al. 2025 radiometrically
   calibrated methane–air hyperspectral study — adoptable for methane iff
   its released data meet the criteria; it cannot establish a universal
   wood/wax/aromatic η_chem, which stays per-fuel. (The *method* —
   measured spectral power ÷ HRR, SPD normalized over [λa, λb], band
   fractions from the same measurement, denominator = primary effective
   gas-phase HRR — is settled, §4.4.) **Until one record is adopted for a
   fuel, predictive rendering for that fuel is blocked.** `chem_model=none`
   preserves predictive status only with the measured negligibility record and
   thresholds in §7.0; lack of data is not evidence of zero emission. The
   provisional η/SPD values are test fixtures, not physical defaults.
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
   visible SSA/extinction alone is not enough. Deliverable form
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

- **r1 (2026-07-27):** initial draft.
- **r2 (2026-07-27):** after internal review round 1 (4 parallel reviewers:
  combustion, transport, CFD, codebase). Committed to the FDS-style
  transported formulation; replaced LLJ soot with fixed yield; two-channel
  radiative loss; BC/domain and numerics specs added; E(m) sign, soot-albedo
  regimes, smoke spectral laws, chemiluminescence bands corrected; embers
  added; stale BDPT claim fixed; auto-rasterizer routing gap (G10) and
  emission-estimator gap (G9) added; Phase B expanded to a full design;
  Blender/Mantaflow made first-class; per-block majorant contract;
  OIDN/firefly policies.
- **r3 (2026-07-27):** after internal review round 2 (4 fresh reviewers).
  Sim: mixing time corrected to the FDS min-of-timescales form (r2's form
  could not burn the DNS candle — P0); W̄ composition term restored to the
  divergence constraint; Y_O relation written with joint realizability
  clamp; critical-flame-temperature ignition criterion adopted (closes the
  third-scalar question); soot reworked as a product species (production
  T-window removed — it contradicted sheet temperatures); χ_r/Planck-mean
  loss changed from region-switched sum to max-blend (double-count + seam);
  C₀ defined and tied to E(m), Planck-mean coefficient corrected 3.72→3.83
  for internal consistency; momentum advection specified; conservation
  budgets added; D*/δx band corrected to [4,16]; plume-row domain, bed
  thermal BC, seeded symmetry-breaking, determinism phrasing, probe
  time-series export. Transport: emission estimator unified to
  real-collision (σ_a/σ_t)·B_λ scoring with mixture-pdf division (r2 stated
  three inconsistent versions); receiver terms aligned with in-tree
  throughput factoring (+|cosθ|, no σ_s double-count); weight-1 rule added
  (volumetric considerEmission analog — r2's "no flag needed" was wrong);
  flame-through-glass scoped march-only; pivot-before-coin-flip conditioning
  and three-strategy selection specified; multiple/nested emissive media
  added; two-level cell→voxel emission CDF (majorant cells too coarse);
  fp32/fp16 channel table (fp16 f_v was subnormal-broken); smoke channel
  given physical units via Mulholland k_m; VDB min/max wording corrected;
  scene-editor surfaces and Phase C build wiring named; aromatic soot-yield
  tier; spark char-oxidation term; §12 renumbered.
- **r4 (2026-07-27):** after internal review round 3 (4 fresh reviewers;
  the codebase reviewer reported zero P0/P1 — all sampled cites accurate).
  Sim: ignition and extinction split into two criteria (r3's CFT-only test
  is an extinction model and cannot block cold fresh mixtures — an AIT /
  flame-connected gate added); τ_adv replaced by the subgrid-velocity
  τ_u = Δ/√(2k_sgs) (Galilean invariance; closure relabeled "FDS-style"
  with deviations stated, τ_chem given a value); radiative sink gains the
  −T∞⁴ ambient-equilibrium term; smoke Ångström exponent corrected to
  n ≈ 1–1.5 for flaming smoke (r3's 2–4 was self-contradictory), §9
  default updated. Transport: emission scoring unified to the single rule
  σ_a·B_λ·T_det/p at the sampled distance (r3's "real collision ÷ mixture
  pdf" phrasing double-compensated Tr on the DT branch and was undefined
  on the equiangular branch; the pure-DT (σ_a/σ_t)·B_λ form is now derived
  as its special case); **chemiluminescence given a transport estimator**
  (deterministic line integral along segments — r3 defined the source but
  no estimator, so a zero-soot methanol flame would have rendered black —
  and explicitly excluded from NEE/Φ with the 10⁻⁴-power justification);
  §7.2.3/§8 CDF-build contradiction resolved (blocks serve majorants only;
  the CDF keeps its O(N) per-frame pass); equiangular activation gate
  widened for flame-only scenes; family-weight pivot conditioning stated;
  Φ units corrected to W/sr; c_smk stored in g/m³ (fp16 subnormals);
  flame-height gate rethresholded on the luminosity proxy; open-boundary
  inflow/outflow H treatment, ε_abs guidance, T_ox value, c_p/sink-iterate
  neglect statements; memory row recounted (velocity = 3 scalars).
- **r5 (2026-07-27):** after internal review round 4 (3 fresh reviewers:
  merged fire-physics, transport, full-document consistency). No P0s;
  round-4 transport review independently verified the §7.1 step 2 rule's
  exact unbiasedness against the code's actual sampler normalizations
  (equiangular density on the segment, delta tracking defective with a
  no-scatter atom — the expectation still lands on the segment emission
  target). Fixes: stale §12 bullet still calling CFT an ignition criterion;
  pivot draw moved to once-per-vertex before volume NEE (the NEE-side
  family weight needs the pivot in shared state); chem line-integral
  segment semantics pinned (full segment regardless of scatter outcome;
  early-outs still score); no-scatter branch explicitly deleted (its role
  is absorbed by the scatter-sample expectation — keeping it would
  double-count); smoke units unified to g/m³ across §3.3/§4.3/§8 with the
  c_soot→f_v conversion owner (sim, ρ_soot ≈ 1800 kg/m³ in metadata);
  extinction channels constrained to one grid resolution (preserves
  quadrature exactness); τ_buoy g′ deviation + k_sgs provenance + DNS
  disabling of subgrid timescales stated; flame-connectedness pinned to
  previous-step adjacency; χ_r global-budget residual admitted; motion-blur
  × frame-built CDF note; §7.1.4 notation normalized; §9 velocity channel.
  A final exit round (2 fresh reviewers, physics and transport+consistency)
  then reported **no remaining P0/P1** — both independently re-derived the
  load-bearing formulas (divergence constraint, 3.83 Planck mean, Y_O
  inversion, §7.1 step 2 unbiasedness against the code's actual sampler
  normalizations) — and their cosmetic P2s (deviation count, C_ν = 0.1
  pinned, AIT range note, throughput-pattern wording, one cite offset,
  early-out note in step 0) are folded in. **Internal review loop closed;
  sent to external expert review.**
- **r6 (2026-07-28):** after **external expert review** (1 P0, 12 P1,
  1 P2 — every code claim spot-verified against the tree before adoption;
  all confirmed). The headline misses of the internal rounds, now fixed:
  **(P0)** the divergence constraint lacked the composition-*enthalpy*
  term that transported-h_s→T inversion requires — full
  (W̄/W_k − h_{s,k}/(c_pT))·S_k form adopted, with discrete-update-derived
  S_div and mixing-box/manufactured-solution gates (§3.2). **(scoping)**
  the internal audit covered RayCaster while the PT rasterizers actually
  run `PathTracingIntegrator` — a second transport surface with
  guiding-aware pdfs, a zero-σ_s early-out that eats emission, and a
  *biased HWSS companion division*; new gap G11, dual-surface work items,
  HWSS hard-disabled for fire media (§5.1/§6/§7.1). **(radiometry)** the
  BlackBodyPainter kernel is exitance-per-metre (2πhc²), a π×10⁹ mismatch
  if consumed as per-nm radiance — new pinned kernel + numeric anchors
  (§4.2); chemiluminescence dimensionally normalized (η_chem/4π,
  unit-normalized SPD, metadata-pinned) with honest composite-quadrature
  bias control (§4.4/§7.1); scene-unit scale connected to SI radiometry
  with an invariance gate (§8); soot inventory reworked to gross-formation
  calibrated to NET published yields with a conserved, disjoint soot/smoke
  partition and σ_s-from-albedo definition (§3.4/§4.3). **(sim)** the
  max-blend radiative sink was numerically refuted in-flame at the
  design's own f_v — replaced by the budget-partitioned escape-factor
  model with integrated-χ_r and optical-thickness gates (§3.5); the
  reaction closure gained the exact exponential-relaxation discrete update
  and a Δt-convergent two-route ignition gate; g′ clamped nonnegative
  (§3.3); a verification tier (V1–V6) and absolute empirical/renderer
  gates added (§3.8/§7.1). **(transport)** volume NEE gained explicit
  geometric visibility + a pinned transparent-shadows policy (§7.2.1);
  p_march gained boundary-survival factors, guiding-aware p_ω, and a
  surface-terminated support rule (§7.2.2); the pivot scheme was
  Rao–Blackwellized — unconditional per-vertex pivot draw — replacing r5's
  incoherent marginal/conditional hybrid (§7.2.4); multi-medium selection
  pmf and innermost-exclusive zero-without-renormalize rule (§7.2.5);
  motion blur gained the path-time carrier, frame-indexing pins, AABB
  expansion, and the trilinear-default/tricubic-overshoot-bound rule (§8).
  RGB path reframed as deterministic projection bias / preview-only
  (§7.1 step 4). External answers to Q1/Q2/Q4/Q5/Q6/Q7 adopted into §12;
  §7.0 phase gating records the reviewer's minimum-changes verdict.
- **r7 (2026-07-28):** after **external review round 2** (0 P0, 13 P1,
  2 P2 — code claims re-verified: NEE-before-guiding ordering in
  `PathTracingIntegrator` confirmed, tricubic signed-weight bound (20/16)³
  recomputed and confirmed). Sim: radiative budget renormalized over the
  **total** radiating support with the γ post-fire blend and a frozen-β
  discrete closure (r6's reacting-only split leaked the plume term and
  its ≤900 K rationale contradicted the doc's own plume temperatures);
  soot burnout bookkeeping carried **in full** (r6's neglect was
  arithmetically wrong — ~3 % of wood HRR, ~15 % aromatic — and
  contradicted V4), with a stated oxidation ODE, the condensable-vapor
  precursor scalar Y_cv, and a **smooth φ(T) mass partition** replacing
  the hard T_switch (whose 1.8× opacity step would have rendered as a
  temperature shell); two named heat-release rates (q̇‴_inst closure-only
  vs q̇‴_step for every consumer); piloted ignition moved to same-step
  flood fill over T_pilot-eligible cells (previous-step adjacency was
  still Δt-dependent); g′₊ fixed in the §3.7 timestep limit; V3
  strengthened, dataset/tolerance pinning rule and end-to-end
  spectral-radiance + smoke-transmittance gates added. Transport:
  equiangular pivots u_m and NEE endpoint Y made **separate independent
  draws** (r6's shared name permitted a biased single-draw reading);
  shared-guide-state-before-NEE ordering + RIS-disabled-at-competing-
  vertices rule; **identity medium boundary** defined, direction-bending
  dielectrics block NEE regardless of shadow flags; visibility added to
  the displayed §7.2.1 integral; §7.2.7 configuration matrix. Pipeline:
  scene-time→sim-time map (t₀, α, Δt_frame) serialized; velocity locked
  to trilinear (signed Catmull–Rom bound is 1.953, not 1.477); metadata
  completeness rule + versioned optical preset record + chem
  normalization interval; chem quadrature moved to
  reaction∪extinction-knot panels with an embedded error estimate,
  "controlled bias" wording throughout; h_{s,k} defined as temperature
  integrals (no Dc_p/Dt term exists to neglect). §7.0 gating and §12
  qualifications updated to the round-2 verdict. A post-incorporation
  consistency pass then closed three seams the surgical edits left: the
  φ(T) partition made explicitly **two-way** (state relation with
  rebalancing — reheated aerosol glows again; a one-way transfer would
  be history-dependent), the realized rate split into
  **q̇‴_gas + q̇‴_sox = q̇‴_step** with chemiluminescence and the exported
  `reaction` channel pinned to q̇‴_gas (soot-burnout heat must not drive
  the blue sheet), and the stale Dc_p/Dt twin in the projection bullet
  removed; ε_Q defined, T_cond/Q̇_ref added to metadata.
- **r8 (2026-07-28):** after **external review round 3** (1 P0, 9 P1,
  1 P2 — all four new code claims verified in-tree before adoption: the
  shadow walk's documented 4-media/16-crossing caps and Tr hard-zero, the
  ratio tracker's step-cap partial return, the tricubic accessor's
  missing clamp, and the direction-dependent post-NEE lobe selection).
  **(P0)** r7's soot burnout was incompatible with the committed chemical
  state — Y_O was algebraically slaved to (Z, Y_F), leaving no DOF for a
  local burnout O₂ sink, and the primary step double-counted the withheld
  soot's heat: fixed by **transporting Y_O** (algebraic relation demoted
  to diagnostic) and **atom/energy-balanced gross primary coefficients**
  (Δh_c,eff = Δh_c − y_form·Δh_soot, s_st,eff = s_st − 2.667·y_form; V4
  gates total heat against the fuel LHV). **(aerosol)** the merged smoke
  channel was unrecoverable on reheating and contradicted the
  constituent presets: replaced by two transported inventories
  (c_carbon, c_condensed) with the hot/cool split **derived** via φ(T) at
  evaluation time — no transfer operators at all — constituent-summed
  optics with σ_s-weighted phase mixture, and a cool→reheat→cool
  conservation gate. **(transport)** the inherited shadow walk declared
  not-usable-as-is (silent caps → biased); Phase B specifies the
  segment-wise active-medium replacement walk with no-silent-caps and
  >4-media/>16-crossing tests; direction-independent lobe preselection at
  volume-NEE-competing surface vertices (disabling RIS was insufficient —
  post-NEE direction-dependent lobe selection also breaks the
  counterfactual pdf); the identity boundary tightened to a strict
  **null-boundary class** (unit deterministic transmission, no
  reflection/tint/emission/shading/depth/roulette — IOR-matched-but-
  tinted/coated interfaces either carry identical factors in both
  strategies or block). **(pipeline)** physical channels locked to
  trilinear (tricubic undershoot ⇒ negative extinction; clamping would
  break quadrature exactness); frame indexing corrected
  (i = i₀ + ⌊(t_sim − t₀)/Δt_frame⌋) with the single-immutable-base-grid
  residency model pinned; velocity halo export required (AABB expansion
  alone cannot recover outward-moving density). **(validation)** candle
  row pinned to Hamins–Bundy–Dillon 2005; intermittency observable
  concretized; absolute L_λ gate added to the pyrometry check; stochastic
  gates restated as 95 % CIs; the chem quadrature acceptance test pinned
  (|I_h−I_l| ≤ a_λ,panel + 10⁻³|I_h|, a_λ,panel = 10⁻⁴·Î_seg/N_panels);
  the frozen-β radiative closure named split-first-order with a
  ≤2 %-and-halving convergence gate. §7.0/§12 updated to the round-3
  verdict, including constituent-specific optical presets.
- **r9 (2026-07-28):** after **external review round 4** (0 P0, 11 P1,
  3 P2 — both new in-tree claims verified before adoption: the trilinear
  accessor's reversed z blend + modf negative fractions, and the λ-blind
  `IPhaseFunction`; the accessor defect is a *pre-existing shipping bug*
  now also tracked as standalone work). Sim: **phase-transfer/EOS
  closure** — ρ defined as gas density, aerosols pressureless with
  phase-transfer mass sources in continuity/S_div, momentum/enthalpy
  neglects bounded, closed-box gas↔soot and vapor↔condensate gates;
  condensable stream given real thermochemistry (pseudo-species
  formula/W/enthalpies, latent heat, saturation-rate rule) and **withheld
  from the primary coefficients like soot** (r8 had recreated the
  double-count class for y_cond); element/W̄ closure extended over the
  full state with gas-only pressure; realizability upgraded to a joint
  elemental feasible-set projection; y_form given a pinned calibration
  state + cross-prediction gate. Renderer: 10⁻³ g/m³→SI factor fixed with
  a unit gate; φ(T) recognized as extinction-relevant — T joins the
  shared lattice, panels split at φ-clamp roots, quadrature upgraded to
  7-point GL (degree-12 integrand exceeded degree-9 exactness);
  medium-level wavelength-aware phase interface added (λ-dependent
  mixture weights are inexpressible through `IPhaseFunction`); trilinear
  accessor repair pulled into Phase A step 0 with
  ramp/continuity/hull gates; march directional pdf corrected to the
  full marginal Σs_ℓp_ℓ with total-BSDF evaluation; the equal-transfer
  option for non-null straight interfaces **removed** (structural double
  count — all non-null interfaces block, transparent-chain exception
  recorded as future work); η_chem pinned to the effective-HRR
  denominator with dataset-conversion rule. Pipeline: t_scene,0 fixed as
  a sequence epoch (render-mode-independent frame selection test); halo
  sized for the full frame interval with serialized width/policy and a
  blur-disable diagnostic; probe samples timestamped; β-vs-γ branch
  conditioning on the radiative gate; §7.2.5 stale walk citation fixed;
  shadow-walk tests extended (wrong-origin, step-cap continuation); RIS
  config-matrix wording corrected. §7.0/§12 updated to the round-4
  verdict.
- **r10 (2026-07-28):** after **external review round 5** (0 P0, 13 P1,
  2 P2 — the two new code claims verified in-tree: lobe-dependent
  guiding eligibility/α/transform at PathTracingIntegrator ~:2961, and
  `PathTracingRayType` classifying non-delta glossy as `eRaySpecular`
  at ~:531; commit `2fba2b48` confirmed as a master ancestor). Round 5
  also exposed that an r9 editing-script failure had silently dropped
  five intended fixes (V4 wording, the named-rates LHV parenthetical,
  the metadata condensable set, the Q1 denominator note, an S_div
  cross-reference) — all now applied in their r10 forms. Sim: the
  ṁ‴_gas/ρ term REMOVED from S_div (with conservative S_k the
  coefficient-sum constraint already carries it — r9 double-counted) and
  ṁ‴_gas pinned with the corrected +Δc_ox burnout gain (not 3.667×);
  the aerosol-loading neglect's false 10⁻³ bound replaced by a
  monitored χ_load ≤ 1 % supported regime with aerosol sensible enthalpy
  carried in the accounting; V4 restated as released heat + residual
  condensable potential = LHV; the saturation law, latent-heat sign, and
  coupling policy pinned quantitatively; the feasibility projection
  replaced by an element-matrix invariant-domain FCT limiter (globally
  conservative by flux form); the calibration protocol pinned to the
  0.30 m heptane reference with a 0.60 m ±25 % cross-prediction, and
  y_cond given its own OC/EC-based protocol. Transport/renderer: the
  §4.3 hot-carbon formula's surviving 1000× unit error fixed; velocity
  blur now disables deterministic-distance-MIS competitors (the warp
  drives integrand degree to ~36, breaking `EvalDistancePdf`'s match to
  the tracking proposal) — blur-on runs pure DT; surface guiding
  disabled at competing vertices (the actual proposal is a lobe×guide
  marginal with per-lobe α and init fallbacks); the weight-1 rule
  expanded to the two-bit (competitionAvailable, continuationSingular)
  state (eRaySpecular includes non-delta glossy; delta lobes from mixed
  materials need weight 1; competition set on NEE attempt, not
  visibility); the phase interface upgraded to a per-collision closure
  carrying local constituent weights + mean cosine; the extinction-
  majorant bound pinned as the φ-sup form (the "T-independent majorants"
  claim was false under φ(T); corner-composed bounds underbound
  anticorrelated fields); nominal vs shutter time separated (base frame
  from nominal time; paths advect by SI-seconds offsets; α ≠ 1 safe);
  the chem quadrature budget re-allocated by initial panel length under
  a Gauss–Kronrod 7/15 pair (current-N division was traversal-order-
  dependent). Status header corrected: the trilinear-accessor
  prerequisite has landed (`2fba2b48`); no fire *feature* code has.
  §12 Q1/Q2 updated with the round-5 dataset candidates (Lai 2025;
  Chang–Charalampopoulos as leading E(m) table, Mulholland–Croarkin as
  total-anchor-only) and their adoption criteria.
- **r11 (2026-07-28):** after **external review round 6** (0 P0, 10 P1
  families, 2 P2; every cited transport surface rechecked against the branch).
  Sim: resolved the FCT/MacCormack contradiction by pinning conservative
  finite-volume coupled FCT and making MacCormack debug-only; defined Z as a
  conservative total-mixture scalar ρ_totZ with the gas-normalized
  (1+χ_load)b(Z) element constraint; replaced diagnostic-only aerosol
  enthalpy with dynamic total sensible energy ℋ_s, C_T, and the corresponding
  complete divergence identity; made χ_load>1 % a hard predictive-scope
  error; made condensable transfer mass-conservative, capped saturation
  pressure, and inserted accepted latent power into energy/S_div; corrected
  the primary+burnout LHV/O₂ wording and expanded V4 to carry both soot and
  condensable chemical potential at intermediate states; strengthened
  y_form/y_cond scale+resolution calibration and pinned OC-to-surrogate mass
  conversion/sampling requirements; corrected non-gray ambient exchange to
  separate κ_P(T) and κ_P(T∞) means and made V5 independently spectral.
  Renderer: pinned one immutable MakePhaseClosure(x,λ) API including
  wavelength-bound g; made velocity blur disable volume NEE/equiangular
  render-globally with explicit weight state, restricted Pel to nominal
  unblurred preview, and added an unbiased ratio-tracked chem blur estimator;
  corrected every blur displacement to mapped simulation seconds; replaced
  loose smoke parameters with whole-record constituent preset overrides.
  Scope/contract: dry aerosol is explicit, wet/hygroscopic smoke is deferred;
  metadata carries that boundary, calibration provenance, and distinct
  constituent optics; Chang's 6.4 µm endpoint now requires a named long-wave
  extension before low-temperature Planck means are predictive.
- **r12 (2026-07-28):** after the first independent implementation-review
  pass over r11 (three fresh axes: CFD/energy, transport/MIS, and
  radiometry/fidelity; 13 P1, 2 P2, 0 P0). Sim: constituent diffusion moved to
  total-mixture fractions so the Z/element affine invariant survives loading
  gradients; FCT gained aggregate nodal correction budgets rather than
  face-local admissibility; conservative state moved to fp64 to make its
  gates achievable; V3 gained deforming-flow and pure-diffusion negative
  controls; gas-band opacity now uses transported CO₂/H₂O history. Radiometry:
  φ now blends hot/cool carbon optical models while **all** absorption emits
  by Kirchhoff; sim and renderer use the same hot+cool+organic spectrum and
  require IR preset coverage. Transport: the emission CDF gained a strict
  upper-bound support component; collision emission moved before bounce/depth
  gates; Pel/NM distance tracking and blur-chem ratio tracking must continue
  past the current 1024-candidate watchdogs. Fidelity/pipeline: normalized
  Mantaflow imports are preview-only; numerical constituent/chem values are
  explicitly non-predictive fixtures until Q1/Q2 close; predictive gates now
  require frozen records; the scene sketch gained a medium name and explicit
  global/bounded binding semantics; metadata was normalized into a complete
  record list.
- **r13 (2026-07-28):** after the second fresh three-axis implementation
  review of committed r12. Sim: made Σq_k the sole gas-density owner with an
  EOS-residual rejection gate; restored sensible enthalpy carried by every
  constituent diffusion flux; moved MUSCL reconstruction into the affine
  invariant nullspace and added limiter nondegeneracy/second-order gates;
  renamed the saturation reference separately from derived dew point. Scoped
  CO₂/H₂O bands honestly as a frozen simulator-only cooling model with absolute
  validation and full provenance, leaving renderer prediction to visible
  aerosol+chem until gas channels land. Transport: removed PDF floors through
  a log-domain density contract, repaired near-collinear equiangular sampling,
  separated always-attempted volume selection q_m^V from equiangular pivot
  weights a, and fully specified Pel phase projection/compensation. Pipeline:
  added a canonical digested frame manifest, one pre-worker immutable-medium
  preparation hook, an enforceable predictive/preview state machine, native-v7
  comments in the scene sketch, and explicit open blockers for y_cond and
  absolute raw-NM flame radiance. Missing chem data no longer masquerades as a
  predictive `chem_model=none`.
