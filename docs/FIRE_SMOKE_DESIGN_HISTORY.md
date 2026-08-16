# Fire & Smoke Design — Revision History

Companion to [FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md). Extracted
2026-07-28 so the design document carries the *current* design rather than the
path taken to it. Each entry records what a review round changed and, more
usefully, **why the previous form was wrong** — several entries document
corrections that are easy to unknowingly re-introduce (the E(m) sign, the
Planck kernel's quantity/units, the τ_mix laminar limit, the no-scatter
emission double-count, the withheld-stream heat accounting).

Read this before proposing a change that reverts something: the odds are good
it was already tried and refuted here.

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
- **r14 (2026-07-28):** after the third fresh convergence review of committed
  r13. Sim: added an exhaustive hashed gas-species/thermochemistry schema,
  made ignition a memoryless CFT-qualified connected-component problem, gated
  effective fuel coefficients for positivity/atom balance, and made β>1 or
  zero emissivity during burning a predictive error. Transport: scoped
  collision pickup to support-compatible Kirchhoff emission, gave independent
  additive medium emission a full-segment estimator, corrected mixed light/
  medium pivot weights to physical band power including scene-unit area, and
  fixed the chem-importance rationale. Pipeline: made the sequence manifest an
  explicit deterministic-CBOR/SHA-256 wire contract with exact end semantics,
  added scene-owned nominal time and rolling-scan-aware preparation bounds,
  specified portable per-artifact provenance, and reconciled override hashes.
  §12 now tracks the gas-opacity and thermochemistry records as explicit
  predictive blockers.
- **r15 (2026-07-28):** after the fourth fresh convergence review of committed
  r14. CFD: pinned the conservative momentum/stress equation, molecular and
  Vreman transport laws/records, immutable-stage two-stage schedule, separate
  high-order ℋ_s reconstruction, and nonzero momentum/SGS/velocity validation.
  Transport: made unbounded segments take pure DT instead of a DBL_MAX uniform
  fallback, stopped calling the support-inflated emission weight physical
  power, and made insufficient blur halo fail predictive preflight. Pipeline:
  injected preparation through explicit prepared `IRasterizer` overloads,
  restored the codebase-required attach/realize/TLAS order with per-frame CDF
  guide refresh, completed manifest index/lattice/domain invariants, removed
  the EXR self-digest cycle, standardized CBOR overrides and reason codes, and
  added the transport record as §12 Q7.
- **r16 (2026-07-28):** after the fifth fresh convergence review of committed
  r15. CFD: separated the Heun transport integrator from one finite-step local
  source map, added a stiff exponential-decay gate, coupled primary combustion
  and soot oxidation through one proportional O₂ allocator, and added the
  missing aerosol-thermochemistry record. Transport: replaced the
  error-estimated adaptive chem quadrature with an explicitly unbiased
  support-mixture line estimator. Pipeline: made rasterizers—not callers—own
  post-animation shutter-support computation; required `AutoRasterizer` and all
  entry surfaces to forward the prepared controller path; pinned axis-aligned
  voxel/placement/vector-transform semantics; separated unconditional
  loadability from preview fidelity; attempted source authentication through
  the content-digest envelope (superseded by r19's detached attestation);
  defined non-self-referential manifest and output-provenance envelopes; pinned
  the RISE-CBOR64-v1 wire profile and independent record preimages; required
  pre-worker table-domain checks; and separated mandatory time-varying sequence
  preparation from optional velocity blur.
- **r17 (2026-07-28):** after the sixth fresh convergence review of committed
  r16 (one P0, nine P1, two P2 across the three axes). CFD: replaced the
  impossible fixed-volume source-then-project schedule with one provisional
  finite-step source packet consumed inside a coupled projected conservative
  expansion/remap; split algebraic and constant-pressure validation; pinned the
  CFT trial, backward-Euler radiation root, full pressure-open/scalar boundary
  map, and reversing-face gates. Radiometry/transport: replaced the single
  HRR×SPD chem approximation with three state-derived absolute CH*/C₂*/CO₂*
  source channels and band/spatial gates; excluded ε_add explicitly from
  thermal NEE/MIS and gave it the blur-mode ratio estimator; fixed the
  scene-unit convention for medium importance. Pipeline: separated producer
  source qualification from renderer-derived fidelity, scoped frames to an
  exact OpenVDB-only desktop capability, pinned every output-provenance hash
  preimage, made blur-off sequence bounds nominal-only, and corrected heuristic
  profile ownership.
- **r18 (2026-07-28):** after the seventh fresh convergence review of committed
  r17 (twelve P1, no P0/P2). CFD: replaced the ambiguous projected-Heun prose
  with a named Q/M three-projection tableau, combined-flux FCT solves, pressure
  multiplier ownership, and ordering-specific manufactured gate; made open
  faces a converged projected-sign active set with full-speed total head; and
  required a certified F′>0 enclosure before the radiation root. Transport:
  defined exact center-bin CDF support/lookup/background rules; restricted
  velocity warping to producer-qualified material channels and rejected/
  disabled blur for nonzero chem or arbitrary additive sources; pinned chem
  yields to pre-reaction state. Pipeline: separated fatal integrity failures
  from valid qualified-record overrides; required unprocessed unclamped output
  for predictive mode; made provenance per-medium and render-config-complete;
  scoped programmatic jobs to preview; hardened CBOR against non-text/duplicate
  keys and noncanonical input; and restored explicit animator-driven TLAS
  invalidation.
- **r19 (2026-07-28):** after the eighth fresh convergence review of committed
  r18 (eleven P1; P2 intentionally excluded from this closeout). CFD: restored
  common-velocity phase-transfer momentum, closed the separate R0/R1 projected
  stage iterations and final accepted endpoint projection, specified the
  nonlinear total-head boundary solve, and certified a unique bracketed
  gas+aerosol enthalpy inverse. Transport: removed the stale chem-blur estimator
  contradiction and defined channel-specific sparse-grid backgrounds.
  Pipeline: disabled worker-time Animator mutation for prepared fire renders;
  restricted predictive primaries to lossless floating scene-linear output;
  replaced self-hash “authentication” with detached trusted qualification;
  added producer/renderer build identity and post-load scene-mutation identity;
  and made display derivatives explicitly non-predictive.
- **r20 (2026-07-28):** after the ninth fresh P1-only review of committed r19
  (four P1; transport/radiometry clean). CFD: classified the final projection
  multiplier as step-average pressure rather than endpoint pressure, changed
  pressure validation accordingly, and replaced the incompatible uniform
  periodic phase-change gate with a zero-mean Galilean manufactured case.
  Pipeline: made qualification-registry revocation anti-rollback and
  time-validity fail-closed, and forced the complete existing light/environment/
  luminary sampler rebuild after nominal animation independently of the volume
  emission guide rebuild.
- **r21 (2026-07-28):** after the tenth fresh P1-only review of committed r20
  (nine P1). CFD: removed retained π₀ from the Heun momentum bases, paired
  step-average π₂ with trapezoidal kinetic head, and corrected the Galilean
  gate to compare shifted solutions plus a local packet invariant. Transport:
  added unconditional decoded-VDB finite/sign/domain scans. Pipeline: fully
  specified domain-separated attestation/build/registry preimages, dual-signed
  root rotation and linearizable anti-rollback authorization; made direct
  manager/item mutations enforceably tracked/frozen; exposed light-sampler
  invalidation through `IRayCaster` with separate spatial/light skip proofs; and
  separated primary predictive preflight from non-predictive secondary artifact
  reasons/status.
- **r22 (2026-07-28):** after the eleventh fresh P1-only review of committed
  r21 (four P1; transport clean). CFD: made π₂'s integrated open-face head use
  the actual R0/R1 inflow indicators across class switches. Pipeline: made an
  accepted dual-signed root-rotation certificate reusable after the new root is
  pinned; added an atomic private parser-load baseline transaction; and replaced
  singular derivative linkage with exact single-primary versus ordered
  frame-sequence provenance variants carrying every primary artifact digest.
- **r23 (2026-07-28):** after the twelfth fresh P1-only review of committed r22
  (seven P1). CFD: added the gas constituent-diffusion momentum flux with the
  same combined FCT acceptance, restricted S_div,commit to nonadvective terms,
  and made active-set sign agreement explicitly deadband-aware. Transport:
  rejected non-background value-off VDB payloads with topology-aware sampling,
  and gated derived source blur by semantics/presence rather than nominal value.
  Pipeline: aligned parser baselining with the real constructor-initializes-first
  lifecycle and extended mutation tracking/freeze through the live rasterizer,
  configuration, FrameStore, output, and encoder graph.
- **r24 (2026-07-28):** after the thirteenth fresh P1-only review of committed
  r23 (seven P1). CFD: paired limiter-active gas advective mass and momentum
  fluxes with a discrete kinetic-energy/free-stream identity, and added an R2
  diagnostic endpoint-divergence solve without feeding it back into the Heun
  commit. Transport: made march MIS use the exact cap- and roulette-aware
  continuation subdensity and split capped direct-only lobes into a weight-1
  NEE term. Pipeline: defined epoch-safe `ClearAll()`/editor rederive,
  prohibited photon-map tracing from prepared time updates, moved platform
  artifact finalization inside the request-wide lease, and made every external
  mutation during freeze fail fast to prevent callback self-deadlock.
- **r25 (2026-07-28):** after the fourteenth fresh P1-only review of committed
  r24 (nine P1). CFD: defined accepted-primal-to-dual MAC flux/density operators
  with an exact commuting identity and pinned the virtual flux-only FCT budgets
  for R1/R2. Transport: made cap-aware MIS implementable through an immutable
  Pel/NM per-lobe continuation closure and contained unsupported BSSRDF-entry
  paths. Pipeline: excluded unqualified SMS, owned callback lifetime and safe
  cancellation, made mutation epochs survive Job destruction as revoked
  tombstones, gated irradiance-cache reachability, and keyed media preparation
  on every preparation-affecting input.
- **r26 (2026-07-28):** after the fifteenth fresh P1-only review of committed
  r25 (seven P1). CFD: made the scalar finite-volume incidence/area/volume
  operator explicit and restricted phase-transfer momentum sources with the
  same MAC density operator. Transport: extended immutable continuation
  closures to medium vertices while disabling competing volume guiding, and
  replaced blanket built-in support with an exact allowlist that excludes
  stochastic `CompositeSPF`. Pipeline: classified/suppressed SMS across both
  transport surfaces, made finalizers owned pre-freeze inputs, bound cancellation
  to one request identity, and made artifact+sidecar publication a staged,
  marked, crash-recoverable transaction.
- **r27 (2026-07-28):** after the sixteenth fresh P1-only review of committed
  r26 (eight P1). CFD: pinned one centered physical nonadvective flux shared by
  both FCT candidates and made the MAC projection use the stored face density
  exactly. Transport: replaced the nominal adapter allowlist with a closed
  default-deny Pel/NM type table and extended SSS containment through nonlocal
  shader ops. Pipeline: added tri-state recursive shader/op dependency queries
  with one RuntimeContext execution policy, and replaced per-finalizer
  publication with a durable, interprocess-locked, journaled required-cohort
  state machine plus separately reported optional derivatives.
- **r28 (2026-07-28):** after the seventeenth fresh P1-only review of committed
  r27 (eight P1). CFD: added explicit 0≤Z≤1 FCT rows and pinned exact hashed
  nullspace-basis bytes/rank/order. Transport: made closure construction
  material-owned, represented geometric-horizon rejection as an explicit null
  atom, and audited luminaire/clay delegation and mismatch rejection. Pipeline:
  made unknown dominate dependency joins, added FAILED_PRECOMMIT, gave Job sole
  opaque-handle staging/canonical-rename authority, and required consumer
  validation through exact artifact-to-required-cohort marker membership.
- **r29 (2026-07-28):** after the eighteenth fresh P1-only review of committed
  r28 (nine P1). CFD: certified nullspace-basis completeness and made the common
  nonadvective flux include a full mass/Z-tangent diffusion projection.
  Transport: made exact material types independently enforceable, rejected
  invalid closure parameters, and added a closed medium/phase continuation
  capability table. Pipeline: made dependency traversal include hidden CSG
  operands, sealed Job-owned staging before hashing, specified discoverable
  canonical marker variants, and made Auto/ViaCst share one transaction-owned
  immutable source blob.
- **r30 (2026-07-28):** after the nineteenth fresh high-threshold P1-only review
  of committed r29 (six P1). CFD: replaced the invalid approximate-null-column
  rank argument with exact upper/lower rank certificates and corrected the
  loaded-mixture diffusion coefficient from ρ_gD to ρ_totD. Transport: required
  per-segment no-event proposal compensation, eliminating the explicit 2^-k
  null-boundary bias. Pipeline: installed per-target cross-directory recovery
  intents, made cohort IDs CSPRNG transaction IDs with immutable group markers,
  and moved scene-load eligibility admission under the exclusive mutation
  lease.
- **r31 (2026-07-28):** after the twentieth fresh high-threshold P1-only review
  of committed r30 (three P1; transport/radiometry clean). CFD: certified the
  pinned fp64 projector against an exact rational nullspace projector, closing
  a correct-rank/wrong-subspace counterexample. Pipeline: made intent cleanup
  durably precede journal deletion and bound scene-load CAS/admission to one
  retained, still-current mutation epoch under its exclusive lease.

- **r32 (2026-07-28):** scope restoration after an independent assessment of
  the r11–r31 loop. That loop's finding rate never decayed (its last twelve
  rounds reported 4, 9, 4, 7, 7, 9, 7, 8, 8, 9, 6, 3 P1s) and it had drifted
  ~26 % of the document into material unrelated to fire or smoke. **Two
  self-blocking defects fixed:** the continuation-closure allowlist
  default-denied the arc's own fire medium — and its "exact
  `HenyeyGreensteinPhaseFunction`" condition could never be met by the
  σ_s-weighted constituent mixture §4.3 pins — so Phase B's emissive-volume
  NEE would have been disabled inside the only medium it exists to light;
  and §3.3/§3.7 gave incompatible diffusive-flux constructions (reconciled:
  §3.3's exactness is structural/exact-arithmetic, the §3.7 projection
  enforces it against fp64 and limiter-tolerance residual). **Removed:**
  cryptographic producer attestation, the operator-owned trusted-key
  registry, dual-signed root rotation, anti-rollback epochs, and the
  CSPRNG/journaled artifact-publication transaction — replaced by a declared
  field + digest *integrity* contract, which is what the arc's actual failure
  mode (a normalized grid rendered as absolute SI) requires. **Split out:**
  `RENDER_PREPARATION_LIFECYCLE.md` (repo-wide mutation/freeze/publication),
  `FIRE_SMOKE_SOLVER_SPEC.md` (the §3.7 operator schedule, tableau, and
  basis certificates), and this history. **Rewrote §7.0**, which had inverted
  from sequencing the work to blocking it: phase gates are now that phase's
  own engineering exit criteria, measurement-dependent requirements gate the
  *predictive label* rather than phase entry, and a Phase-A execution order
  names the minimal end-to-end slice. Added
  `multichannel_heterogeneous_medium` as Phase A's authoring surface
  (`fire_medium` needs a Phase-C manifest and so cannot serve renderer-only
  work). 5079 → ~3900 lines; no physics, transport, or radiometry removed.
  A follow-up review pass caught that the attestation removal had been applied
  at its definition site but not its five consumer sites, and that the
  quantitative `chem_model=none` criterion had been weakened in the §7.0
  rewrite; both repaired in the same revision.

- **r33 (2026-07-29):** unblocked Phase-A step 4, which the implementation
  agent correctly refused to start. §4.2's Pel emission source
  ε_c = ∫R_c σ_a B_λ dλ (added r32) depended on `R_c(λ)`, which the design
  sourced from a "versioned `band_preset`" that was **never defined
  anywhere** — two mentions in the document, both passing it as an argument;
  no schema, no asset, no default, and no grammar slot in
  `multichannel_heterogeneous_medium`, whose §9 rule forbids implicit
  defaults. The Pel gate was therefore uncomputable. **Resolved by deleting
  the phantom rather than building it**: RISE already has exactly one
  spectral-to-Pel response — `XYZFromNM` (CIE 1931 2°) composed with
  `XYZtoRec709RGB`, normalized by `CIE_Y_Integral` — and it is the response
  that forms every spectral image today. A per-medium preset would have
  admitted a scene whose two fire media disagree about the camera, and a
  second asset could silently diverge from the real film response. The
  §9 no-implicit-defaults rule was clarified to govern a medium's own
  physical constants, not the shared film response. Also split the two roles
  R_c was conflating: **projection** (σ̄_a,c, σ̄_s,c, ε_c) uses R_c with its
  negative lobes, which is correct colour science and matches what the
  spectral path produces; **sampling** (mixture weights, proposal density)
  uses the nonnegative CMF sum W = x̄+ȳ+z̄, since a density must be
  nonnegative and its choice affects variance, not correctness. The
  luminance/photopic prohibition was clarified to bind projection only.
  Separately repaired an ambiguous r32 sentence — "for a grey medium the two
  coincide" read as though a Pel triple could be compared to a
  wavelength-valued radiance; what coincides is the Pel target and the
  *projection of* the spectral target.

- **r34 (2026-07-29):** corrected r33's own code claim, again found by the
  implementation agent refusing to improvise. r33 defined
  R_c = XYZtoRec709RGB ∘ XYZFromNM and required "negative lobes intact" — but
  `ColorUtils::XYZtoRec709RGB` calls `MoveXYZIntoRec709RGBGamut` **before**
  the matrix (`Color.cpp:218`), so it never exposes the signed response
  (at 500 nm the raw matrix gives ≈(−0.63, 0.62, 0.22); the shipped function
  gamut-maps that away). The disqualifying reason is stronger than the lost
  lobes: gamut mapping is **nonlinear**, so composing it per wavelength does
  not yield a linear functional, and ∫R_c f dλ would not be a projection of
  anything. The film's use of the same function is sound only because it is
  applied once to an already-accumulated XYZ (`FilteredFilm.cpp:93`).
  **Fix:** R_c uses a new matrix-only entry point
  (`XYZtoRec709RGBMatrixOnly`, a thin wrapper over the existing file-local
  `XYZtoRGBMatrixMultiply<Rec709RGBPel>`); the gamut-mapped function keeps
  every current caller. Changing `XYZtoRec709RGB` itself was considered and
  rejected as an unmotivated renderer-wide colour-pipeline change.
  **Also added, from a check r33 should have made:** a signed R_c integrated
  against a *narrowband* σ could produce a negative channel coefficient, and
  negative extinction breaks tracking outright. It does not arise for the
  broadband power-law extinction this design admits (a 1/λ absorber projects
  to ≈(0.20, 0.19, 0.22), all positive), so σ̄_a,c ≥ 0 and σ̄_s,c ≥ 0 are now
  asserted at medium construction rather than assumed. ε_c is exempt —
  emission is accumulated, never exponentiated, so a negative channel there
  is ordinary out-of-gamut colour. Softened r33's claim that the Pel path
  "agrees with" the spectral image: it is the closest linear analogue, and
  exact agreement is impossible while one path gamut-maps a final XYZ and the
  other has only coefficients — which is why RGB stays preview-only.

- **r35 (2026-07-29):** third consecutive Pel-path stop, third real defect —
  and this one was in a claim r32 said it had *verified*. The Pel
  coefficients were defined as unnormalized response-weighted integrals,
  σ̄_a,c = ∫R_c σ_a dλ. For a grey medium that transmits exp(−σK_cL) instead
  of exp(−σL), where K_c = ∫R_c dλ ≈ (1.20, 0.95, 0.91) — not (1,1,1),
  because equal-energy XYZ is not Rec.709's D65 white. The grey slab
  therefore missed the projected spectral target by −7.9 %, +2.1 %, +4.0 %
  per channel at σL = 1: a *grey* medium acquiring a colour cast from a
  projection with nothing chromatic to project. **Why r32's "verified to
  1e-12" missed it:** that check used a normalized box response with
  ∫R dλ = 1 — exactly the special case in which K_c cancels and the bug is
  invisible. A verification that assumes away the property under test proves
  nothing; the real response was never substituted in.
  **Fix — the two quantities project differently, and the asymmetry is
  forced by what they are:** σ is an *intensive* per-length rate that enters
  an exponent, so its channel value is a response-weighted **mean**
  (÷K_c); ε is an *extensive* radiance density that is accumulated, so its
  channel value is a response-weighted **integral**. K_c then cancels
  identically and §7.1 step 2's grey identity holds by construction. This
  was preferred over the three options offered (drop the grey gate; defer
  projected coefficients to the chromatic closure; renormalize R_c per
  channel): the first discards a gate that had just proved its worth by
  catching this, the second leaves step 4 not exercising the coefficient
  path it exists to prove, and the third would corrupt ε_c, which correctly
  wants the unnormalized integral. The positivity assertion now also covers
  the normalizer (K_c > 0, checked once at startup); the §4.3 sampling
  weights need no normalizer since S_jc/S_c and S_c/Σ_dS_d are ratios.

- **r36 (2026-07-29):** **spectral is the target; Pel is preview** — a
  scope decision taken after r33–r35 spent three consecutive design
  revisions on the Pel projection without producing a line of transport
  code. Each of those findings was real (a phantom `band_preset`; a
  gamut-mapped conversion that is not a linear functional and so cannot
  define a projection; coefficients projected as integrals rather than
  means, giving a *grey* medium a −7.9 %/+2.1 %/+4.0 % colour cast). None
  was on the path to a physically correct flame. The math r33–r35 produced
  is kept — it is right, and the grey identity is what caught the last
  defect — but the *bar* it is held to changed:
  - **Every absolute radiometric gate is defined in NM.** The isothermal
    slab, the pure-absorber slab, and scene-unit invariance are spectral
    gates; the Pel path is not held to any of them.
  - **Pel carries consistency gates only:** it runs without assert/NaN/
    negative extinction; the structural grey identity L_c = ∫R_c L_λ dλ
    holds (free, since K_c cancels); and its divergence from the projected
    spectral render on the reference scene stays inside a *measured,
    recorded* regression bound — a tripwire for a broken preview, not a
    certificate for a correct one. There is deliberately **no absolute Pel
    radiance target**: a Pel triple is not a radiance and predictive output
    is spectral-only.
  - **Pel moves to the end of Phase A's critical path** (new step 7).
    Steps 1–6 are all spectral. Until step 7 lands, the RGB rasterizers
    **reject fire media with a diagnostic** rather than rendering something
    unvalidated, and no gate or phase may depend on the Pel path.
  The general lesson, recorded because this arc keeps relearning it: hold a
  path to the bar its output actually claims. Pel claimed preview and was
  being gated as if it claimed predictive.

- **r37 (2026-07-29):** pinned the wavelength quadrature — a debt from the
  r11–r31 loop that an internal round-4 review had flagged as
  referenced-but-unspecified and graded P2; the Phase-B implementation agent
  proved it P1 with numbers (for B_λ(1800 K)·(500/λ) over [380,780] nm: a
  10-bin left rule gives 909.99, a 40-bin left rule 1044.04, the true
  integral 1090.41 — a ~20 % spread in CDF weights, selection pmfs, and
  labeled densities across plausible readings). All three values reproduced
  independently before adoption. **Decision (the agent's option 1):** the
  single-interval 21-point Gauss–Legendre rule mapped onto [380, 780] nm,
  using the binary64 node/weight table already in `MicrofacetEnergyLUT.h`,
  promoted to a shared utility. Verified: the table matches true GL nodes to
  all printed digits, and the rule matches a high-resolution reference to
  ~10⁻¹³ for the smooth thermal integrands this CDF admits (ε_chem is
  excluded from Φ by design). The pin includes a one-rule-everywhere
  requirement — Ĩ_v, W_m/A_m, and `EstimateVisibleBandPower()` share one
  implementation, since these numbers enter as ratios and a mixed-rule
  implementation biases the partition even when each rule alone is
  accurate — and a scope caveat that the accuracy claim does not extend to
  narrow-band SPDs. Process note: the P2 grade in round 4 was wrong
  precisely because 'reference-only' specifications look harmless until
  someone has to compute with them.

- **r38 (2026-07-29):** two coverage gaps raised by the project owner,
  audited and confirmed against the document. **(1) Animation authoring:**
  playback was thoroughly designed (§8 time machinery, §10.3 animation loop,
  MOV linkage) but a fire that *changes over a shot* was inexpressible —
  §3.6's source was a constant ṁ″_F. **(2) User-facing parameters:** the
  physical data layer existed (fuel records, versioned presets) but the
  user→simulator surface was never designed; §7.3 specified outputs only.
  Added **§3.9, the case specification**: a small authored surface (named
  fuel record; pool/patch source with D_eq convention; intensity as ṁ″_F or
  target HRR via ṁ″_F = Q̇/(A·Δh_c); the **time-varying source envelope**
  e(t) — piecewise-linear, knots ≥ one puffing period apart, evaluated at
  the §3.7 beginning-of-stage source time, constant on every
  validation/calibration case; duration; quality tiers draft/standard/high
  = D*/δx 4/10/16 plus a numeric `dstar` form so §3.4's {6,10,14}
  calibration runs are expressible; seed; cadence) with **everything else
  derived, not asked** (domain from §3.6, δx from D*, Δt policy,
  discard/spin-up — including the pre-roll rule for e(0)>0 shots and the
  no-discard rule for fires that catch on camera — the seeded perturbation,
  and Q̇_ref as the envelope's peak). The complete case file is hashed into
  a new §8 `case_record_id` manifest field (Q̇_ref moved out of the
  per-fuel group, where it never belonged — a fuel record cannot know a
  case's HRR). Looping declared out of scope explicitly. A review pass
  found four P1s in the draft (Q̇_ref ownership; underivable discard for
  from-zero envelopes; patch sources breaking D-keyed rules; calibration
  resolutions inexpressible) — all fixed before commit, plus the
  conversion-basis nuance (withheld soot energy is mostly recovered at
  burnout; the permanent deficit is condensables + escapes).

- **r39 (2026-07-29):** pinned the null boundary's repository realization —
  the fifth implementation-agent stop, and like the others a genuine gap:
  §7.2.2 specified the class behaviorally ("dedicated interface class,
  unit deterministic transmission, no shading/depth/roulette") but the only
  in-tree candidate, `NullMaterial`/`"none"`, is *terminating* (no SPF ⇒
  the integrator breaks the path) and is the renderer-wide default
  sentinel, so repurposing it (agent option 1) would silently make every
  default-material object a pass-through. Option 3 (a non-material boundary
  interface) invents an object/parser model for one class. **Adopted the
  agent's option 2:** a distinct exact type `NullBoundaryMaterial` with
  `RISE_API_CreateNullBoundaryMaterial`, an `IJob` wrapper, and a
  `null_boundary_material` chunk; `"none"` unchanged. The design now also
  pins the traversal semantics both transport surfaces must implement:
  exact-dynamic-type check (subclasses are not null boundaries, matching
  the allowlist posture); same ray, no depth/roulette/shading/emission;
  medium-stack transition as the only effect via the existing
  innermost-exclusive walk; no IOR-stack entry (IOR-matched by
  definition); and class-level transparency to shadow and volume-NEE rays
  independent of shadow flags.

- **r40 (2026-07-29):** corrected r39's self-contradiction, found by the
  implementation agent on its sixth stop: r39 demanded the null boundary
  perform the medium-stack transition while "never entering the IOR
  stack" — but RISE has no independent medium stack (`MediumTracking`
  resolves the active medium from `ior_stack.topObject()`; verified), so
  both requirements could not hold. **Adopted the agent's option 1**: an
  internal split of `IORStack` into an **optical stack** (Snell/Fresnel/TIR;
  dielectric boundaries only — null boundaries never appear, which is what
  "no optical effect" means mechanically) and an **enclosure stack**
  (medium resolution and shadow-walk seeding; dielectric boundaries update
  both, null boundaries only this one, ambient IOR unchanged). External
  signature and copy semantics unchanged — nothing new threads through
  transport calls; the optical stack is a subsequence of the enclosure
  stack, asserted as a debug invariant; `IORStackSeeding::SeedFromPoint`
  seeds both so shadow-walk origins inside null-bounded media start from
  true boundary state. Rejected: a fully separate `MediumStack` threaded
  through every transport call (large diff surface and a standing risk of
  the two stacks disagreeing about enclosure), and pushing null-boundary
  entries into the unified stack with unchanged IOR (no optical effect
  from Snell's law with n₁=n₂, but stack-shape-sensitive code — boundary
  counting, parity, TIR bookkeeping — would see phantom entries).

- **r41 (2026-07-30):** resolved the terminal-path-depth contradiction, the
  implementation agent's seventh stop and again genuine: §7.1's
  source-before-depth rule (deliberate — the emission score precedes every
  σ_s/max-bounce/depth/RR gate, and the terminal outgoing segment is
  sampled and marched for source pickup) coexisted with a §7.2.7 gate
  sentence declaring the terminal continuation "impossible with
  p_march = 0" — but p_march is defined as the density of that sampled
  strategy, and a sampled strategy's density is by definition nonzero.
  **Adopted the agent's option 1** (preserve source-before-depth): terminal
  depth forbids processing any *downstream vertex*; the source-only
  segment keeps its genuine nonzero p_march (p_ω·p_t/r² with survival
  factors) and competes with volume NEE under the standard partition; the
  gate wording is corrected and §7.2.2 gains a canonical terminal-vertex
  statement pinning that "p_march = 0" is structural only (beyond non-null
  interfaces, outside chain support), never a depth-cap consequence.
  Rejected: gating depth before outgoing sampling (reverses pinned Phase-A
  behavior and loses terminal-segment emission), and disabling volume NEE
  at terminal vertices (contradicts the always-attempted rule and wastes
  the estimator precisely where paths end most often). Prerequisites
  landed by the agent before the stop: the optical/enclosure IORStack
  split (26208c16) and exact NullBoundaryMaterial (2a422986), both
  reviewed to zero P1 with 232/232 and 233/233 suites.

- **r42 (2026-07-30):** resolved the collision between r41's terminal rule
  and the pinned f_A/f_D mixed-lobe design — the agent's eighth stop, and
  rooted in an r41 overreach: r41 wrote "total/path-depth **or per-type
  lobe cap**" into the terminal source-only-segment rule, but the reviewed
  availability spec deliberately puts a per-type-capped lobe *outside* the
  sampleable set with its response confined to the NEE-only weight-1 f_D
  term. Worse, the availability spec's own definition ("A = lobes the
  later continuation may sample") made even *total* depth empty A,
  contradicting r41's nonzero-p_march rule from the other side. **Fix — the
  agent's option-1 mechanism with option-2 semantics:** two availability
  sets. A_vertex (the old A, every cap applied) governs downstream-vertex
  processing; A_march (identical except the total/path cap is ignored;
  per-type caps still exclude) governs the path-final source-only segment,
  whose density is the A_march marginal *without roulette-survival factors*
  (nothing to survive). A per-type-capped lobe is in **neither** set — it
  exists to stop paying for a lobe class, and its emission coverage is
  NEE's job. The f_A/f_D split is now governed by A_march, which equals
  A_vertex at every non-terminal vertex, so the entire construction reduces
  to the previous spec everywhere except path-final vertices. Cap-derived
  p_march = 0 exists only as empty-A_march (empty support, not a
  sampled-strategy contradiction). The agent's literal option 1 (capped
  lobes keep march support mid-path) was declined: it would revise the
  extensively reviewed mixed-lobe gate to buy marginal variance on emission
  NEE already covers, at per-vertex cost the caps exist to remove.

- **r43 (2026-07-30):** clarified the §7.2.7 canonical-geometry list — the
  agent's ninth stop: "emitter enclosing the receiver, receiver inside the
  emitter" named the same topology twice. The original intent (recoverable
  from §7.2.4's own cross-reference to the weak-equiangular immersed case)
  was two distinct cases distinguished by σ_t at the receiver: an emissive
  shell around a hollow cavity with the receiver at a standoff (σ_t = 0 at
  the receiver; full-sphere pivot selection), and a receiver immersed in
  the emissive volume (σ_t ≠ 0; equiangular concentration weakens and the
  DT half of the 50/50 must carry it). The agent's option 2 ("receiver
  enclosing the emitter") was declined as redundant — that is the ordinary
  exterior view, already the headline flame-lit-smoke gate — and option 3
  was wrong on inspection: the :3205 smoke-scatter-receiver gate is a
  mechanics axis (guiding/caps/RR at a medium vertex), not a topology.

- **r44 (2026-07-31):** phase-split the §7.2.7 unsupported-material gate —
  the agent's tenth stop: the gate demanded "reject predictive mode before
  sampling," but predictive mode (`fidelity_mode`,
  `render_fidelity_status`, reason codes) is a Phase-C deliverable with no
  Phase-B existence, so a Phase-B gate depended on machinery from a later
  phase. Verified: none of the fidelity identifiers exist under
  src/tests/scenes; the Phase-B code implements exactly the preview half
  (exact allowlist at closure construction; unsupported vertices fall back
  to competitionAvailable=false + legacy collision march at weight 1).
  **Adopted the agent's option 1**: Phase B gates the preview half now
  (allowlist rejection at construction, the §7.2.2 fallback with a debug
  diagnostic, NEE-on/off equality on that fallback); Phase C re-gates the
  *identical fixture list* in predictive mode, demanding pre-worker
  fail-closed rejection with `continuation_closure_unsupported`. Rejected:
  pulling the fidelity seam into Phase B (far wider than the transport
  increment), and a test-only predictive flag (would not exercise the
  authored scene/job state or the pre-worker fail-closed path — the
  agent's own analysis, correct). Also fixed a duplicated clause the
  original sentence split left behind.

- **r45 (2026-07-31):** phase-split the SSS predictive gate — the agent's
  eleventh stop, and the same defect class as r44 one sentence further
  down: §7.2.2 demanded "Predictive Phase B rejects ... with
  `sss_volume_nee_unsupported`" and §7.2.7 demanded predictive rejection
  plus unknown-dependency preflight, while the fidelity seam those require
  is Phase C (verified: no fidelity identifiers in the tree; the r44 split
  covered only the adjacent unsupported-material sentence). Adopted the
  agent's option 1, identical in shape to r44: Phase B owns the
  classification (SSS materials, both named shader ops,
  unknown-nested-dependency-as-SSS) and the preview containment fixtures;
  Phase C re-gates the identical fixtures demanding pre-worker fail-closed
  rejection. **Because this class bit twice, r45 adds the general rule to
  §7.2.7**: every predictive-mode rejection demanded anywhere in §7.2 is a
  Phase-C re-gate of a Phase-B preview fixture — no Phase-B gate depends
  on the seam, no fixture is written twice. A sweep of §7.2 confirmed the
  two SSS sites were the last instances.

- **r46 (2026-08-01):** closed the Phase-A chem SPD binding gap — the
  agent's twelfth stop: §4.4 defines ε_chem from unit-normalized per-band
  SPDs, and the manifest-less Phase-A chunk bound the three chem *channels*
  but provided no way to supply S_b(λ), its normalization interval, or
  units — with §9's no-implicit-defaults rule correctly forbidding the
  loader from inventing them. **Adopted the agent's option 1**: per band,
  the chunk requires `chem_spd_<b>` (a named `IFunction1D` spectral curve —
  the same reference mechanism `homogeneous_medium` already uses for
  `absorption_spectral`) and `chem_interval_<b>` (the declared [λa,b]).
  The loader normalizes the curve to unit integral over the declared
  interval at construction using a **pinned 1 nm trapezoid** — explicitly
  not r37's 21-point Gauss–Legendre, whose own scope caveat excludes
  narrow-band SPDs — so §4.4's convention holds by construction from an
  authored *shape*; this load-time normalization is distinct from the
  forbidden renderer-band renormalization (clipping to [380,780] still
  loses out-of-band power without redistribution). Channels read in W/m³.
  Everything stays preview-only fixture material until Q1 adopts real
  records via `fire_medium`'s Phase-C chem record. Landed by the agent
  before the stop: the condensed constituent (70f00d81) and the φ-aware
  root-split 7-point quadrature (23bf6e33), both zero-P1 with green
  suites.

- **r47 (2026-08-01):** recorded the implementation's Pel/NEE scope choice
  at the Phase-A completion handback: volume NEE — all of §7.2's
  machinery — is NM-only; the Pel preview transports volume emission
  through the collision march at weight 1. The agent implemented this
  (consistent with r36's preview rule and the consistency-only Pel gates)
  but §7.2.4's "always attempted" carried no measure qualifier, leaving a
  design/implementation divergence a future reviewer would flag. Phase A
  is complete as of this revision: condensed constituent, φ-aware
  quadrature, chem SPD binding with scene migrations, the auto-rasterizer
  PT rule, and the Pel preview (measured divergence from the projected
  spectral reference 1.126/0.725/0.084 % R/G/B against the recorded 5 %
  bound), all zero-P1 with green suites.


- **r48 (2026-08-08):** the dataset-ratification revision — landed by the
  data/records workstream at the owner's direction, closing out its
  Q1/Q2 arc. **Q2 (optics) closed for the visible band**: adopted
  `mac_equivalent_E` (C&C 1990 shape normalized to MAC(550)=8.0 m²/g at
  pinned ρ=1.8 g/cm³; MAC-normative, density hashed with the table;
  E_eff ≈ 0.39–0.51 visible; raw C&C and Dalzell–Sarofim demoted to named
  ablation records) plus the four constituent presets — hot soot
  ω=0.10/g=0.22 @550 (RDG-FA over table-verified morphology, validated
  three ways), cool carbon 8.7/0.25/1.0–1.2/0.58 with the §4.3 anchor
  check passed to <1 % (B&B MAC ↔ M&C extinction imply ω=0.251), condensed
  organics by validated Mie ("fresh, dry, near-source, flaming" domain).
  §12 fixtures moved to a distinct SYNTHETIC_NON_PREDICTIVE record.
  Still open within Q2: >780 nm/IR closure (long-wave E(m) axis for §3.5;
  `condensed_organics_ir_unclosed`) and the §3.5 derivative enclosures.
  **§7.0 chem gate language revised** to "pinned evidence record" — a
  derived record over traceable measured inputs qualifies iff it carries
  measurement/covariance/digitization/geometry/spectral-truncation/
  domain-transfer/model-form uncertainties; the wax derived record was
  built and REFUTES negligibility at the 1 % threshold (central 1.02 %,
  95 % bound 17.1 %; failure physical, not conservatism) — predictive
  sooty fuels need real §4.4 chem records. **Q1 state recorded**:
  composite-source per-fuel records allowed (separately hashed
  subrecords, intersected domains); methane deferred (Lai 2025 read and
  evaluated — CH*/C₂* adoptable with author contact, 390 nm/CO₂*
  structurally absent); methanol preview for v1 with a named reopen
  trigger; leg (ii) recorded as gated by §12 item 4. **§8 ratifications**:
  the canonical provenance-field schema (envelopes inside the hashed
  payload; uncertainty-kind enum; per-record out_of_domain_policy;
  aggregates carry component policies only) and the four output-provenance
  completions of FIRE_OUTPUT_PROVENANCE_PIN_V1.md (preview_primary;
  tagged active_fire_media; renderer_build_v1 + parameter-surface
  ratchet; one-preimage provenance_id with attribute-stripped EXR
  mirroring), all implementation-consumed with green suites (236/236)
  before ratification. Evidence trail: FIRE_OPTICS_PRESET_V1.md,
  FIRE_CHEM_RECORDS_V1.md, FIRE_DATASET_PULL_MANIFEST.md,
  FIRE_SOURCE_ALTERNATIVES_AUDIT_2026-08-06.md, docs/data/*.

- **r49 (2026-08-08):** phase-ownership correction, raised by the Phase-B
  implementation agent's ninth stop (no code changed before the ruling).
  Phase B gate 5 as written — "per-frame invalidation and rebuild of the
  emission structures" — presupposed a per-frame grid producer, but the
  only Phase-B fire medium is statically authored (CDF built at
  construction); the sequence contract, `fire_medium`, and the
  freeze/prepared-input seam with `IRenderPreparationController` are all
  Phase C, and §10.3's controller registry is populated by time-varying
  media that do not exist in Phase B. Ruling: **split, not weaken** —
  Phase B retains the non-vacuous core (between-renders mutation of a
  fire medium's emission/extinction invalidates its CDF/majorants,
  rebuild before next render, fail-closed on staleness, with a
  mutate→render regression), while the per-frame scheduled rebuild
  re-gates to Phase C gate 5 attached to its producer. The substance is
  unchanged: no stale emission structure is ever consumed. The
  alternatives — pulling the renderer-wide preparation seam into Phase B,
  or inventing a Phase-B frame-source API with no consumer — were
  rejected as scope creep and speculative architecture respectively.

- **r50 (2026-08-11):** gas-opacity architecture pinned, raised by the
  Phase-C implementation agent's increment-2 stop. The agent's Voigt-LBL
  generator needed 2 self-fractions × 105 temperatures = 92.5 billion
  line-state evaluations for the EM2C cross-check against a 5-billion cap,
  and offered three options (sharded LBL / correlated-k / unbounded table).
  **All three were declined**: each pays for a line-shape dimension the
  consumed quantity does not have. §3.5 consumes Planck means in the
  optically-thin limit, Planck means are linear in κ_λ, and therefore
  broadening cancels exactly — line shape is not an axis of this record.
  The adopted dataset stores a temperature-independent (ν, E″) moment
  histogram that reconstructs Σ S(T) analytically at any temperature; the
  same state grid costs ~4×10⁷ evaluations instead of 9.25×10¹⁰.
  **The decisive argument was correctness, not cost:** the agent's
  knot-only lookup with interior-state rejection cannot serve §3.5 at all,
  because the bracketed backward-Euler solve evaluates both Planck means at
  *arbitrary trial temperatures* on every iteration. The exponential-sum
  form is continuous and analytically differentiable in T, which is also
  what the certified F′(T) enclosure wants — so the enclosure is now
  derived analytically rather than by finite differences. Also amended
  §3.5/§3.8's V5 wording: the gas slab comparison is explicitly the
  **thin-limit Planck-mean** one (ε → κ_P·pL to first order, shape-free,
  run where κ_P·pL ≲ 0.05), because comparing a thin-limit cooling closure
  against optically-thick total emissivity tests a claim the model never
  makes; the per-column κ_P·L monitor is what detects a broken thin
  assumption, and Planck-mean-specific references are preferred over
  emissivity datasets as the primary check. Dataset landed in `ef8332bd`
  with full provenance (FIRE_GAS_OPACITY_DATASET.md): 440,501,248 HITEMP
  records ingested with zero parse failures and both counts reproducing
  HITRAN's published totals; validated to 0.1–3.2 % against two independent
  line-by-line references while reproducing RADCAL's known CO₂ offset;
  9.4 GB of line lists reduced to 188 KB of operational data. Item 5's
  remaining work is the certified §8 record built from that data, not the
  data itself. Full Voigt LBL is recorded as out of scope for this record
  (a future transmission/band-resolved capability), and the CO₂ visible
  bound is explicitly a 565–780 nm bound with a physical-negligibility
  argument required for 380–565 nm.

- **r51 (2026-08-11):** solver bring-up rule, from the Phase-C agent's
  increment-3 stop. The thermochemistry generator emits a property subset
  lacking the element matrix, atom-balanced closure, LHV, product
  coefficients, injected compositions, and the N_A/N_C closures; the agent
  proposed either a strictly synthetic verification closure or expanding
  the physical record first. **Neither as framed.** Two facts settle it.
  (1) N_A (`conservative_reconstruction_v1`) and N_C
  (`nonadvective_flux_projection_v1`) are *pure linear algebra over the
  species set* — orthonormal nullspace bases for A and C=[A;(0,1,…,1)] with
  exact rational rank factorizations and pivot-minor certificates. They are
  computed, never measured, and were therefore never blocked on owner-gated
  data; the blocker is only that the generator does not yet emit a complete
  fuel record. (2) **Methane is fully open and fully ungated**: every
  species is in the Apache-2.0 NASA Glenn set, LHV follows from the
  record's own ΔfH (closing the energy ledger self-consistently), the
  element matrix and products are arithmetic, and methane has no
  condensable organic stream — so the owner-gated levoglucosan and
  condensed-organic-c_p fields simply do not occur in it. Bring-up
  therefore uses a **complete physical methane record**, with synthetic
  closures restricted to contrived V-tier fixtures (rank deficiency,
  manufactured solutions, RED cases) under distinct record IDs. Rationale
  for refusing a synthetic-primary path: rank/projector certificates
  verified only against a contrived matrix can mask a structural property
  of a real element matrix — the carbon column shared by CO₂, CO and soot
  is the obvious candidate — and a parallel synthetic artifact is free to
  drift from the physical record, which is exactly the failure the
  fixture-versus-preset ID separation exists to prevent. Wax and wood stay
  fail-closed on their condensable streams while their gas-phase sides are
  completed. Methane is also the item-4 radiance-gate candidate and the
  design's own DNS-resolvable laminar class, so this ordering costs nothing
  downstream.

- **r52 (2026-08-12):** per-fuel constant taxonomy corrected, from the
  Phase-C agent's increment-3 stop on missing methane operational
  constants. Sourcing them surfaced two design errors, not just gaps.
  **(1) T_pilot is not a per-fuel measured property.** A gaseous fuel has
  no piloted-ignition temperature — piloted ignition of a gas is governed
  by flammability limits and pilot energy, not a bulk temperature
  threshold — and the ≈600 K figure the design carried is the
  piloted-ignition *surface* temperature of *solids* (wood, paper), a
  different quantity entirely; it is also far below every measured methane
  AIT (810–873 K). It is retained as a numerical gate constant whose only
  structural requirement is T_pilot < T_AIT, now labelled as such. FDS's
  analogue corroborates the classification: its AUTO_IGNITION_TEMPERATURE
  defaults to a value that disables the gate, and its guide says the value
  "may need to be lowered for cases where the grid size is greater than
  10 cm" — a parameter retuned with grid resolution is a closure, not a
  property. **(2) χ_r is not a fuel constant.** Measured methane values
  span 0.07–0.28 across burner size and heat-release rate, and FDS states
  outright that "there is no single value of radiative fraction for a given
  fuel." Because §3.5's budget and the §3.8 gate both consume it, the fuel
  record now carries χ_r as a declared default with applicability and
  spread, overridable per §3.9 case. Also pinned: T_ox (~1300 K) is
  fuel-independent and justified by empirical burnout quench (Kent & Wagner
  1984, Glassman 1988) rather than NSC kinetics, whose calibration begins
  only at 1273 K and assumes O₂ is the oxidant; methane y_s = **0.00**
  measured (Köylü/Sivathanu/Faeth — "emitted no soot"), with FDS's 0.01
  explicitly a modelling convention since FDS ships no per-fuel soot yield;
  T_AIT = 810 K with the 810–873 K spread recorded, apparatus-dependent and
  pre-ASTM-E659; and ρ_soot referenced from the optics record rather than
  duplicated, since that density is identity-bearing there. Recorded
  consequence: with y_s = 0 a predictive methane flame emits almost no
  visible thermal continuum — correct physics, and it makes methane an
  excellent solver bring-up fuel but a poor visual one until §4.4 chem is
  enabled. Record: `docs/data/fire_fuel_methane_v1.draft.json`.

- **r53 (2026-08-13):** **Phase C engineering is complete** — all five §7.0
  Phase C gates green at `e7ff026d`, recorded at the completion handback
  (the r47 convention). Gates 1–3: the §3.2–§3.7 solver core (low-Mach
  variable-density projection with 3D pressure-open geometric multigrid;
  the single owning conservative R0/R1/R2 Heun/FCT advance consuming frozen
  reaction/radiation packets with the exact N_C flux projection; two-rate
  reaction closure with flood-fill ignition; withheld-stream soot ledger;
  budgeted escape-factor radiation) verified by the full V1–V6 tier,
  including the adversarial cases doing their jobs — V3(c)'s negative
  control fails for the debug MacCormack path as designed, V3(d) holds
  ≥1.8-order convergence with active high-order fluxes, V5's hot-carbon
  leg reproduces the f_v(T⁵−T∞⁵) law against an independent wavelength
  integral and its gas leg runs the r50 thin-limit Planck-mean comparison
  against the adopted HITEMP record, V6 proves eligibility-graph
  determinism under reconstructed state. Gate 4: the §8 sequence contract
  (canonical manifest with one-preimage sequence_id, unconditional
  loadability with reject-never-repair RED fixtures, time mapping,
  fire_medium binding, residency, and the sequence_backed provenance
  variant finally exercised with real data). Gate 5: the freeze/
  prepared-input seam with the per-frame grid/majorant/emission-CDF
  rebuild scheduled in the frame-advance step — closing the obligation
  r49 re-gated here from Phase B. Solver bring-up ran on the complete
  physical methane record per r51/r52. Evidence at the milestone: 241
  tests reconciled green, warning-free make + Xcode Deployment + Opto,
  three fresh orthogonal reviews at zero P1s. The arc's dataset
  decisions (r50–r52) held through implementation without amendment.
  **Predictive-label status is unchanged by this milestone**: output
  remains preview with reason codes — the label still waits on the §12
  data items exactly as §7.0 separates the two. Next: the §3.9 case
  contract (case_record_id) and the first end-to-end preview-labelled
  methane sim→grid→renderer run.

- **r54 (2026-08-13):** §3.9 determinism pins, from the Phase-C agent's
  capstone stop: the case grammar was explicitly "not final" and eight
  underdeterminations (ranges like "2–3 D" and "≥5", `≲`/`∝` timestep
  forms, an unspecified perturbation algorithm, a Heskestad small-case
  hole, an unresolved §3.9↔r52 χ_r conflict, and unplaced thread-count
  identity) made a canonical `case_record_id` impossible without invented
  semantics — confirmed by two independent audits. Pinning principles:
  take the design's own conservative range-ends deterministically; prefer
  requirements-on-output over implementation choices; nothing on the
  identity path carries a `≲`. The pins: (1) case schema v1 final, with
  the standard one-preimage envelope; (2) lateral exactly 3 D, top exactly
  2 L_f_eff (5 L_f_eff plume-law), with **L_f_eff = max(L_f, D)** closing
  the nonpositive-Heskestad hole; disc source masked by cell centers;
  (3) δx = D*/tier exactly, extents round UP to integer multiples — the
  extent grows to fit δx, never the reverse; (4) exact Δt coefficients
  (0.5 advective, 0.5 buoyant, 1/8 diffusive against the δx²/6ν 3D bound,
  ×1.1 growth limit), mirrored into the solver spec since Δt selection is
  identity-bearing; (5) t_ft = H/√(g·D*) — a priori computable and erring
  conservative — with discard/pre-roll exactly 5·t_ft; (6) the seeded
  perturbation fully specified (SplitMix64 over lattice indices, top-53-bit
  mapping, mean-subtracted 1 % multiplicative source-flux pattern, constant
  for the whole run); (7) the χ_r conflict resolved by the r52 taxonomy
  itself — the no-override rule now binds classes (a)/(b) only, and class
  (c) gets an optional case-level `chi_r` field inside the hashed payload;
  (8) thread count and reduction mode are **not** identity-bearing: the
  solver must produce bit-identical sequences at any thread count via
  fixed-order reductions (the discipline the V-tier ledgers already
  assume), enforced by a 1-vs-N determinism fixture, with threads recorded
  in producer/run metadata only.

- **r55 (2026-08-13):** the pilot pinned as a physical energy source, from
  the capstone's cold-start stop. The agent's diagnosis eliminated every
  numerical suspect (bed tuple exactly in the constraint space with zero
  affine residual; state-scaled tolerance; a 3×3×3 production fixture
  passing without step reduction; an fp64 endpoint-inversion mismatch found
  and fixed with a 2×-envelope RED), leaving a genuine design hole: §3.3
  named a "fixed pilot-source mask" without defining it, and the
  piloted-with-resolved-T gate is circular for a cold gas burner — piloted
  eligibility needs T > T_pilot, and a 300 K domain with 300 K injection
  has no heat source until reaction starts. Pin: the pilot is what a real
  pilot is — a small flame — modelled as a prescribed volumetric energy
  source through the ordinary source-packet/ℋ_s ledger, never a state
  overwrite. Mask derived canonically (first layer above the bed, centers
  in the annulus [D/2, D/2+2δx] — the fuel/air interface, where the
  eligibility graph can actually build vertices); power exactly 0.01·Q̇_ref
  uniform over the mask; active for exactly 1·t_ft from run start, so the
  5·t_ft discard/pre-roll window guarantees no pilot energy touches any
  measured statistic; ledgered in ℋ_s but excluded from Q̇_tot (it is not
  heat of combustion — χ_r's budget and ε_Q see combustion only). No new
  authored field; the block is derived, echoed, and inside
  `case_record_id`. Case schema v1 amended in place — no persisted record
  predates the pin. Eligibility, seeding, and extinction semantics are
  unchanged: the pilot supplies the resolved heat the gate always
  presumed. Alternatives rejected: an FDS-style gate-bypass zone (changes
  the gate semantics the design chose deliberately, and ignites without
  resolved heat — exactly the non-physics §3.3 was written to avoid) and a
  temperature overwrite (violates the conservative-ledger discipline every
  V-tier gate certifies).

- **r56 (2026-08-13):** physical-kernel consistency pinned, from the
  capstone's sustained-combustion stop — the deepest catch of the
  implementation arc, and the affine envelope doing precisely its job.
  The rank-4 canonical-binary64 A passed every r51 certificate yet its
  exact-dyadic nullspace missed the atom-balanced methane reaction
  direction: A and the reaction stoichiometry had been derived through
  different arithmetic paths from the same atomic weights. Signature:
  per-step A-residual ~7×10⁻¹⁸ with systematic sign, linear accumulation
  to the ~2.17×10⁻¹² envelope over sustained burning, and a projector
  displacement of ~2×10⁻⁷ on the first reacting step — the 10¹¹
  amplification implying a near-dependency in the kernel at ~10⁻¹¹ (an
  exact rational row dependency, mass = Σ elements, broken by independent
  rounding). Disposition: **regenerate, never correct at run time** — the
  agent's option 2 (a defined source-packet correction) was rejected
  because a systematic 2×10⁻⁷ transfer of ρZ/CH₄ per step is nonphysical
  mass/element motion that would silently corrupt the V4 ledger and the
  reaction closure. Three new generator-time certificates: one exact-
  rational source for A, all fuel stoichiometric directions, and b(Z),
  with A_ℚ·Δq_r = 0 proved in ℚ before rounding; stored-row independence
  (dependent constraints declared symbolically, never stored as
  separately rounded rows); and a certified positive lower bound on the
  stored row basis's smallest singular value. Plus a sustained-combustion
  fixture asserting unbiased (√steps) residual growth. N_A, N_C, the
  methane record, and case identities regenerate downstream —
  pre-release, no compatibility surface. The run-time envelope stays
  fixed and fail-closed: it caught this.

- **r57 (2026-08-14):** capstone scale ruling and pilot amendment, from
  the sustained-combustion stop. The tested 10 mm cases were outside the
  model's validity envelope, and the numbers prove it three ways: at
  tier 6 a 10 mm burner gets 2.5 cells across its diameter (δx/D = 0.40)
  — the §3.3 closure is a plume-scale mixing-limited model, not a laminar
  flame-structure model; the observed integrated χ_r of 0.423 against a
  declared 0.07 is the γ-branch's intentionally unbudgeted post-fire
  cooling engaging during marginal burning and accelerating the quench
  (the machinery behaving correctly on an inadmissible case); and the
  tiny case was *stiffer and costlier per physical second* than a
  properly scaled one. Disposition: the agent's option 2, sharpened —
  the capstone becomes **McCaffrey's own configuration** (0.30 m methane
  burner, Q̇_ref = 33.0 kW, plume-law tagged), which pins the McCaffrey
  centerline row at its measured configuration, gives a directly
  checkable 2.7 Hz puffing expectation, and costs only ~214k cells at
  tier 6. χ_r stays at the fuel default 0.20, no override, gated against
  the measured 0.07–0.28 spread. Option 3 (accepting extinction as the
  capstone) rejected — it validates fail-closed machinery but does not
  deliver the arc's goal; option 1 alone (pilot tweaks) rejected as
  masking a validity-envelope problem with ignition crutches. Two
  supporting pins: **case admissibility δx ≤ D/4** (fail generation with
  the smallest admissible tier named; laminar candle-class micro-flames
  are a separate future case class with their own resolution rule — the
  §2.4 DNS-resolved regime), and the **r55 pilot power re-pinned as an
  intensive 1 MW/m³ density** over the mask (the fractional 0.01·Q̇_ref
  form diluted with burner size and never crossed T_pilot on wide
  annuli; a density heats any mask cell identically at any scale, and
  total pilot power then scales with ring volume exactly as a physical
  pilot ring does). Also noted from the same stop: the five red V2/V3/V6
  manufactured gates after r56 regeneration are fixtures carrying
  constants derived from the OLD kernel — the fix is record-derived
  fixtures, never tolerance changes; the agent's refusal to weaken
  tolerances was correct.

- **r58 (2026-08-14):** pilot thermostat, from the capstone's
  opacity-domain stop — and a case where the certified domain caught a
  physics bug rather than a data gap. The McCaffrey runs ignited and then
  reached 2406–2500 K, exceeding methane–air's adiabatic flame
  temperature (~2230 K); no radiating diffusion flame can be
  super-adiabatic, so the state itself was unphysical. Mechanism: the
  r55/r57 pilot kept applying 1 MW/m³ to mask cells that were already
  burning — at 2000 K product density that is ~4400 K/s of additional
  heating stacked on combustion, comfortably explaining the +180–270 K
  excess. Fix: **a per-cell thermostat at exactly 900 K** on the accepted
  beginning-of-step temperature — the pilot heats a cell only while it is
  below the ceiling (pure threshold, no hysteresis, deterministic). The
  ceiling exceeds every in-scope T_AIT (≤873 K), so ignition still seeds
  into the spontaneous-eligibility regime, and sits far below any flame
  temperature, so products are never pilot-heated; physically it is the
  thermostat idealization of a pilot flame that ignites reactants and is
  irrelevant once the main flame holds. Explicitly REJECTED: extending
  the certified opacity domain (it would have let the super-adiabatic
  artifact pass silently — the domain guards physical validity, and every
  physical methane state sits far inside 2500 K) and any provisional
  out-of-domain radiation treatment (clamping physics under another
  name). The agent's refusal to clamp, weaken feasibility, or add an
  early shutoff was correct on all three counts.

- **r59 (2026-08-14):** two-class limiter acceptance, from the tier-10
  capstone's Picard Zeno stop. The coupled R0 solve converged every
  continuous quantity (S_div ≈10⁻¹¹, face mass ≈10⁻¹², transport
  coefficients ≈10⁻¹³, active set stable) while one FCT face
  coefficient cycled 0.18–0.30, and the fail-closed Δt reduction drove
  accepted steps below 10⁻⁵ s without escape. Diagnosis: α is a
  *derived* output of the R0 map (nothing in the loop consumes it), and
  the §3.7 limiter R_cr=min(1,Q_cr/P_cr) with strict a_r·A_cf>0
  activation indicators is discontinuous exactly on
  constraint-activation boundaries — codimension-1 surfaces that time
  refinement relocates but generically re-crosses. Demanding Cauchy
  convergence of a discontinuous derived map was a proxy certificate; a
  hope, not a theorem. Fix: **α leaves the convergence gate; acceptance
  classifies the limiter** — continuous class (verification
  re-evaluation agrees within tolerance → accept it, unchanged
  behaviour) or discontinuous class (accept the pointwise face infimum
  min(α_next, α_ver)). The certificate is direct and already latent in
  §3.7's construction: all constraint rows are affine and P_cr sums
  positive parts, so the limiter value is the *maximal* admissible
  blend and the entire interval [0, α̂] is admissible — pointwise
  reduction is conservative, never unsafe. The corrected state under
  the accepted α must still pass low-order admissibility and the
  certified affine constraints, fail-closed into Δt reduction.
  Explicitly REJECTED: prescribed under-relaxation (a tunable with no
  certificate — convex combinations of coefficients from different
  iterates are not bounded by α̂ at the accepted state, and damping a
  discontinuous map still need not converge) and raising the α
  tolerance (a 0.3 jump is not "almost converged"; it is a different
  class and the design names it). The rule is parameter-free,
  deterministic, identity-bearing (case records regenerate), and the
  accepted class is recorded per step in run diagnostics. The agent's
  refusal to invent an unpinned convergence rule or weaken tolerance
  was correct.

- **r60 (2026-08-15):** accepted-state feasibility envelope, from the
  tier-10 capstone's Δt-independent infeasibility stop. The run earned
  honest cold start, ignition, pilot-off at 2.085 s, and sustained
  combustion with peak 2225.24 K — under the 2300 K physicality bound
  and consistent with methane–air T_ad ≈ 2230 K, confirming the r58
  thermostat produced physical peaks. It then died at step attempt 750,
  t ≈ 2.717 s: an exhausted constituent converged to −5.40472×10⁻¹³ as
  twenty halvings drove Δt from 3.966×10⁻³ to 7.565×10⁻⁹ s. The agent's
  Δt-independence diagnosis was exact: the violation rode in the
  *accepted beginning state*, so step N accepted bytes that step N+1's
  gate rejected forever. Concrete mismatch found in code review of the
  stop: the limiter's outward correction budget is 1024·ε·scale while
  the feasibility gate is invoked with 4096·ε·scale′ on different
  vectors with different scale functions — at unit scale those are
  2.3×10⁻¹³ and 9.1×10⁻¹³, and the observed −5.4×10⁻¹³ sits *between*
  them. Deeper cause: an inventory exhausted by accumulated subtraction
  has a roundoff floor of order ε·(accumulated magnitude); a gate that
  scales by the near-zero result reads that floor as an unbounded
  relative violation. Fix (§3.7 r60 block): ONE gate predicate — single
  implementation, one κ, accumulation-based forward-error scale —
  shared verbatim by every feasibility gate; the envelope is DERIVED
  from the union of producer certificates, never tuned (widening it to
  swallow an observation is clamping's cousin, and verifying the
  observed excursion sits inside the derived envelope is required);
  composability (acceptance certifies the stored bytes; the next step's
  precondition is the same pure predicate, so accepted states cannot be
  retroactively rejected); consumers total on the envelope (stored
  envelope-scale negatives carried honestly, rate/availability logic
  consumes max(0,q), affine consumers certified by inspection).
  Clamping and ledger-mutating projection remain forbidden. Also
  ordered: the fail-fast defect — downstream stages continued after
  solver failure and exited 139 — is a separate implementation bug; a
  solver failure must abort the pipeline with a structured error, RED
  test required. The agent's conduct was again correct: no restart, no
  tier substitution, partial output quarantined out of the final
  directory.

- **r61 (2026-08-15):** run persistence, from the capstone's
  incremental-persistence stop. r60 landed clean (single predicate
  across 1D/periodic-3D/open-3D; producer union derived at 2384·ε and
  rounded to κ = 4096; the recorded −5.40472×10⁻¹³ exhaustion trace is
  0.5943 of the minimal envelope, covered without tuning; signed
  envelope values stored, no clamping; structured solver errors gate
  downstream writes; zero P1s; 617/617 green), but the agent correctly
  refused to launch tier 10: the run holds every frame memory-resident
  and writes only after all solver calls return, so a late death
  preserves nothing — and the buffering itself is a memory-pressure
  death risk on a ~1M-cell day-scale run. Disposition: **authorize the
  seam** — §3.9 pin 8a places frame streaming and checkpoint/resume in
  pin 8's class (run infrastructure, NOT identity-bearing), with the
  requirement on the output: bit-transparency, enforced by a
  complete-vs-(checkpoint+kill+resume-at-different-thread-count)
  fixture demanding identical frame digests. Checkpoint cadence and
  indices are run metadata only; checkpoint writes are pure
  serialization of accepted inter-step state and never perturb the
  trajectory; sufficiency is certified by the fixture, not a field
  list. Reproduction-only stays a valid mode (determinism is the
  ultimate fallback), but day-scale runs use the seam.
  Reproduction-only launch was REJECTED because one mid-run death had
  already cost a day and the memory-resident structure makes a second
  more likely, not less. Also accepted: r60's verification against the
  recorded scalar and the universally minimal scale (the quarantined
  r59 VDB lacks the full conservative cell) — any true accumulation
  scale only increases margin, and the exhaustion fixture carries the
  forward-looking guarantee.

- **r62 (2026-08-15):** isothermal pilot kernel, from the first
  genuinely buoyant run — and an explicit anti-rathole reflection
  requested by the owner. Background: the harness had left gravity at
  zero (fixed by the agent in b8781fdd with fail-fast qualification),
  which means every prior "successful ignition" (r55–r60) was validated
  in a quiescent regime with infinite residence time. The empirical
  gates caught the zero-g run exactly as designed — puffing 0.478 Hz vs
  2.739 Hz expected, McCaffrey T/u errors ~100 %, while the HRR ledger
  was round-off clean: a ledger-perfect, physically wrong run rejected
  on measurements, which is the entire point of the V-tier. With
  gravity on, the r57 1 MW/m³ pilot plateaued at ~548 K: heated gas
  develops buoyancy by ~400 K, clears the first layer in ~0.1 s
  (~65 K/pass against ~830 K/s heating), and no duration reaches the
  600 K gate — a steady-state shortfall, not a marginal one. Rathole
  diagnosis: this was the pilot power constant's third failure (r55
  fractional, r57 intensive, now buoyant), each failure retuning a
  rate; and the r58 thermostat had already made power a *saturating*
  parameter — any sufficient rate merely holds the 900 K ceiling. r62
  deletes the saturated constant and keeps the setpoint: the pilot is a
  finite-step local map **T ← max(T_accepted, 900 K)** on the mask at
  fixed composition, its realizing ℋ_s increment differenced into the
  source packet and ledgered as pilot energy (excluded from Q̇_tot as
  before). This is advection-robust by construction, scale-invariant in
  the way r57 intended, more physical (a real pilot is a region held at
  temperature by its own chemistry, not a volumetric wattage), and
  removes the last tuned rate constant from the ignition path. The r58
  products protection survives verbatim in the max() form. Rejected:
  retuning power upward (converges to the hold with one extra arbitrary
  constant), extending duration (steady-state shortfall), lowering the
  600 K gate (measured record value), and skipping cold-start ignition
  (the statistics window starts at 5·t_ft ≈ 10.4 s and the pilot dies
  at 2.1 s, so ignition is narrative — but it is the narrative that
  exercises §3.3's two-route gate honestly). Also answered: the
  capstone arc itself is not the rathole — the gates keep catching real
  defects, and the pilot was the last invented component in the physics
  chain.
