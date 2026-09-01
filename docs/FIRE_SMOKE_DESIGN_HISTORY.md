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

- **r63 (2026-08-16):** expansion-capped pilot approach and the
  source-expansion admissibility contract, from the tier-6 stop on the
  r62 map — and a defect in r62 as pinned, owned as such: the max()
  hold was the design's first Δt-*independent* (projection-type) local
  map, and the source-packet doctrine had only ever implicitly assumed
  Δt-continuous (relaxation-type) maps whose packets vanish with Δt. A
  fixed-composition 300→900 K jump has EOS volume ratio V ≈ 3, giving
  Δt·S_div = V−1 = 2 at EVERY Δt, so the donor update negated the
  cell's inventories (observed O₂ = −0.273, N₂ = −0.899 ≈ −qⁿ,
  invariant under 20 halvings from 2.106 s to 4.0×10⁻⁶ s) — the agent
  correctly identified this as Δt-independent and earlier than mixing
  or eligibility, and correctly refused to invent semantics. Fix, and
  the simplification reflection re-run at the owner's request: (1) the
  pilot map self-limits its per-step expansion to an exact **5/4 EOS
  volume-ratio cap** — a §3.9 pin-4-class stability coefficient
  (advective ½ + expansion ¼ ≤ ¾ combined donor drain, below 1 with
  margin, same justification pattern as the diffusive 1/8), shaping a
  ~5-step millisecond transient (300→375→469→586→732→900) and never
  load-bearing for gate crossing; (2) the general contract: per cell,
  packets must satisfy Δt·S_div ≤ ½ exactly, fail-closed —
  relaxation-class maps reach it via ordinary Δt reduction,
  projection-class maps must self-limit, and a projection map without a
  self-limit is a design error. REJECTED alternatives: relaxation to
  the setpoint with time constant τ (τ is load-bearing — the
  τ-vs-advection steady balance holds ~700 K at τ = 0.05 s but ~495 K
  at τ = 0.2 s: the power-constant rathole reborn under a new name); a
  hot annular co-flow pilot inlet (no expansion spike, but new inlet
  geometry + a velocity constant + a mass-ledger change — larger
  surgery for no added physics); subcycling the map (the same cap
  hidden inside a loop); fixed-volume application with post-hoc
  projection (already forbidden by the spec). The setpoint, mask,
  timing, thermostat semantics, ledger, and exclusion rules of r62 all
  survive unchanged.

- **r64 (2026-08-16):** exact finite-source pair, from the tier-6 stop
  on the r63 capped map — the exact EOS gate's second correct catch in
  a row, and the final layer of the projection-map semantics opened by
  r62. The linearized pair (Δt·S_div = V′−1 with the full fixed-volume
  packet) is inconsistent for a finite jump twice over. Analysis with
  constant c_p exposes the sharper half: donor drains 25 % of mass and
  enthalpy while the packet restores ALL the drained enthalpy, landing
  ON the ideal-gas manifold but at 400 K instead of the capped 375 K
  target — a setpoint overshoot that could spiral; the record's
  T-dependent h_{s,j} then breaks even that accidental manifold
  consistency, producing the observed Δt-invariant 0.00168 residual
  (confirmed over 20 halvings, 2.106 s → 4.0×10⁻⁶ s, inventories
  positive, the r63 drain check passing — every prior pin behaving).
  Fix: projection maps emit a **jointly-exact, target-derived pair** —
  Δt·S_div = 1 − 1/V′ (exact rational relation, never the
  linearization) and packet = target minus survivor: Δq_j = 0 (pure
  energy; expelled gas carries beginning composition, elemental
  closure trivial), Δℋ_src = (1/V′)·Σ q_j[h_{s,j}(T′) − h_{s,j}(T₀)]
  exact from the record tables. Verified closed-form: mass 0.8ρ₀,
  enthalpy 300·c_p·ρ₀ → exactly (ρ₀/V′, 375 K, P₀) — the accepted
  state IS the target state by construction. The exact EOS gate is
  unchanged as verifier; the r63 cap and ≤½ drain check are unchanged
  (drain at cap becomes 0.2). Relaxation maps keep the linearized
  relation (second-order defect in Δt·rate, governed by the existing
  gate + Δt reduction — no semantics change to chemistry/radiation).
  REJECTED: relaxing/tolerancing the EOS gate; shrinking the cap until
  the residual hides below the r60 envelope (envelope-burial); state
  overwrite; Δt reduction (Δt-invariant defect). Arc note: r62 (what),
  r63 (how much per step), r64 (exactly how) — the pilot projection
  map is now fully determined: setpoint, mask, timing, thermostat,
  cap, and pair, with nothing left underdetermined.

- **r65 (2026-08-16):** frozen-drain tableau participation, from the
  r64/Heun contract stop. The exact pair is correct locally but the
  projected-Heun commit, realizing the drain through stage-state
  advection, composes it to 1−e+e²/2 — the truncated exponential
  (0.82 vs the required 1/V′ = 0.80 at the cap; observed 9.08 K
  target miss and 1.75×10⁻⁴ EOS residual, Δt-invariant over 20
  halvings). The agent's refusal to select a tableau implicitly was
  correct: two of its three candidate options genuinely define new
  tableaux. The ruling is the third — and it is not new semantics but
  the completion of packet freezing, using two guarantees the spec
  already makes: sources are consumed exactly once inside Heun, and
  the shared centered nonadvective flux is identical across
  R0/R1/commit and never FCT-limited (low/high differ only in
  advection). The projection map's drain becomes a frozen
  beginning-state donor flux in that slot — e·qⁿ per mask cell out the
  canonical top (+z) face, computed once, ridden verbatim by both
  stages and the commit — replacing (never supplementing) the
  stage-advective realization of the map's S_div share at the mask
  cell, while the S_div share stays in the frozen projection target so
  momentum and neighbors respond. The binding arbiter is the owner
  fixture: one mask cell, quiescent ambient, FULL production tableau,
  accepted state exactly the target within the r60 envelope. REJECTED:
  stage-specific divergence (a second tableau by another name);
  a distinct conservative finite-map commit (its own
  momentum/projection story, adjacent to forbidden
  fixed-volume-then-project); the Heun-preimage drain
  ê = 1−√(2/V′−1) (algebraically lands 0.800 exactly but only in the
  isolated-cell model — it bakes the tableau's composition polynomial
  into the pair and fails in company with ordinary advection);
  cap-shrinking to bury the e²/2 defect below the envelope.
  Calibration note, owned: r64's closing claim that any further
  pilot-adjacent stop would indicate a different subsystem was
  miscalibrated — "exactly how" also had to say "through which
  tableau." The r62→r65 sequence is one design debt paid in layers
  (map → step → pair → commit), and the commit is the last layer:
  after r65 the projection map's journey from emission to accepted
  state has no remaining seams.

- **r66 (2026-08-16):** production gates over isolated-endpoint
  reproduction, from the r65 decomposition stop — and the round where
  the rathole was the FIXTURE, not the solver. The decisive fact in
  the agent's report: with the frozen drain implemented, PRODUCTION
  acceptance is green — the r65 mechanism removed the real defect
  (9.08 K → 0.0199 K target miss; EOS residual 1.75×10⁻⁴ → 2.34×10⁻⁷,
  now inside the unchanged production EOS tolerance) — and the only
  red is the r65 owner fixture demanding the coupled production step
  reproduce the isolated-map endpoint to the r60 envelope (~10⁻¹³).
  That fixture pin was a category error, owned as mine: the remaining
  residual is e²-order momentum feedback (M*† legitimately carries the
  R0 pilot response) plus ordinary molecular transport — genuine,
  bounded, Δt-invariant but NON-accumulating physics (the map is a
  feedback controller: each step re-emits its target from the accepted
  state, re-absorbing coupling residuals instead of compounding them).
  Both escalation options the fixture was forcing (a parallel no-pilot
  shadow trajectory; suppressing source-cell crossflow) are an
  infinite regress — every feedback level would demand another frozen
  shadow at e³, e⁴, … Fix is a subtraction: no production semantics
  change, no new machinery. The decomposition algebra keeps an
  exactness fixture only at unit level, where exactness is defined;
  the binding production arbiters become: unchanged EOS/feasibility
  acceptance green through the hold; pilot ledger bit-exact vs emitted
  packet + frozen drain; active mask cells in the exact hold band
  [899 K, 900 K] once the approach completes (~50× margin over the
  coupling scale, 300 K from the ignition gate, and coarse enough that
  an r64-class 9 K defect trips it — the band is the regression net);
  conservation ledgers to fp64 reduction tolerance. REJECTED: shadow
  trajectory (regress), crossflow suppression (alters real physics to
  satisfy a fixture), post-transport retargeting (r64-forbidden),
  widening any production tolerance (production never needed it — it
  was already green). No case-record regeneration beyond the agent's
  preliminary a917258f… — r66 changes fixtures and gates, not map
  semantics. Answer to the standing simplification question: the
  simplification was subtraction — the pilot arc closes not with a
  fifth layer of machinery but by deleting an unattainable
  requirement; production had already converged.

- **r67 (2026-08-16):** ordinary tableau participation — the r65
  frozen drain is reverted, from the gravity-on tier-6 receiver stop.
  The frozen flux delivered a per-step-finite dose of beginning-state
  gas to the cell above the ring that is not velocity/stage-consistent,
  reappearing the Δt-invariance one cell downstream: EOS residual
  1.01×10⁻³ vs the 1.0×10⁻³ gate at cell (25,20,1), invariant through
  20 halvings, pre-ignition. The agent's two audited receiver-side
  repairs both quantified the same conclusion (re-counting the received
  flux in S_div → 0.2; unrestricted pressure response → 0.158): every
  correction was patching a channel that should not exist. The decisive
  evidence pair, read across two stops: the r64 implementation WITHOUT
  the frozen drain measured 1.75×10⁻⁴ in production — green under the
  gate with 5.7× margin — and had been declared failing only against
  the r65 exactness fixture that r66 later deleted as a category error;
  and ordinary stage-consistent advection demonstrably passes these
  gates through 1700 K fronts (the 2.7 s zero-g burn). Sequencing error
  owned: r65 built a mechanism to satisfy a fixture, r66 removed the
  fixture but kept the mechanism, and the mechanism then caused the
  next failure. r67 removes the mechanism: the r64 exact pair is
  unchanged at emission; its drain rides ordinary stage advection; the
  Heun composition (0.82 vs 0.80 at the worst approach step) is a
  bounded, non-accumulating deviation governed by the r66 gates; the
  hold band closes because the map is a controller with quadratically
  shrinking per-step miss near the setpoint; the pilot ledger line is
  the packet, bit-exact, with the drain covered by the global
  conservation ledgers. The r64 pair-emission REDs stay (they guard the
  real r63 defect at 1.68×10⁻³); the r65 mechanism fixtures are deleted
  with the mechanism. Machinery after r67: setpoint map + cap + exact
  pair emission + production gates — four pieces, each with a
  gate-failing counterexample justifying it. REJECTED: receiver-side
  mixing-share corrections (patching downstream of the defect; and the
  frozen/stage mismatch component is not computable at emission time);
  widening the EOS gate (Δt-invariant floor would be buried, r60
  lesson); freezing the column or suppressing crossflow (alters real
  physics); switching to a co-flow inlet pilot (its velocity constant
  is load-bearing for the achieved kernel temperature — the r55/r57
  power rathole in velocity units; the isothermal hold exists precisely
  because it is the parameter-free member of this family).

- **r68 (2026-08-16):** cap re-sized to 17/16 by the EOS-deviation
  bound, from the r67 tier-6 lateral-receiver stop — and the round
  where the arc's residual pattern finally named itself. Three stops
  put the same number at the next-worst cell: r63 mask 1.68×10⁻³
  (inconsistency — fixed exactly by the r64 pair), r65 receiver above
  1.01×10⁻³ (stage-inconsistent channel — removed by r67), and now an
  unmasked floor-layer ambient receiver laterally outside the ring at
  1.04×10⁻³ vs the 1.0×10⁻³ gate, Δt-invariant, via ORDINARY
  advection with consistent emission. That last datum falsifies r67's
  empirical anchor as stated: the zero-g burn's fronts passed the
  gates because their driving source was a relaxation map (halve Δt,
  halve the dose); a projection map's per-step-finite expansion makes
  every neighbor's dose Δt-invariant during the approach, so
  finite-dose mixing with nonlinear thermochemistry leaves a
  ~10⁻³-class manifold deviation SOMEWHERE in the ring's neighborhood
  regardless of channel. The fixes so far relocated the class; r68
  reduces it: the deviation scales with the per-step expansion e
  (dose ∝ e, per-step contrast ∝ e), and the cap is the one knob that
  is not load-bearing — it shapes only the approach transient (the
  controller reaches the setpoint regardless; hold-phase re-assertion
  doses are Δt-scaled since advective cooling per step ∝ Δt, so the
  Δt-invariant window is the approach only). Cap re-pinned 5/4 →
  **17/16** (= 1 + 2⁻⁴, binary exact; e = 1/17 ≈ 0.0588; ~19 approach
  steps, still milliseconds): linear scaling from the measured ledger
  predicts ≤ 3.1×10⁻⁴ (3.3× under the gate), quadratic ~9×10⁻⁵
  (11×). Boundary with the r64 cap-shrink rejection drawn explicitly:
  that rejection stands for INCONSISTENCIES (a wrong discrete
  relation is fixed exactly, never buried); sizing the remaining
  consistent, irreducible coupling term under a production tolerance
  is what stability coefficients are FOR — the diffusive-1/8 pattern.
  Falsifiable prediction recorded: if tier 6 fails again at ~10⁻³
  with cap 17/16, the class does not scale with e and a genuinely
  different defect is present. REJECTED: widening the EOS gate
  (Δt-invariant floor burial); receiver-side S_div mixing shares
  (patching downstream; the r67 stop showed the class surfaces at
  whichever neighbor is worst); freezing/suppressing anything
  (alters real physics).

- **r69 (2026-08-16):** manifold restoration — the root cause of the
  entire r65/r67/r68 receiver-stop family, found by reading the solver
  rather than modeling it, at the owner's direction to make the design
  solid before further implementation ping-pong. The r68 falsification
  was decisive and correct: shrinking e from 0.2 to 1/17 moved the
  rejection limit only 1.0365×10⁻³ → 1.0310×10⁻³, and both gravity
  runs died at the same ~537 K ring temperature. Code reading then
  found the mechanism: the EOS residual is the ACCEPTED STATE's
  |P_rep/P₀ − 1| (fire_simulator_core.h:320), and the solver had two
  divergence-target paths with different constraint discipline — the
  finite-increment packet path refers its cells to the ABSOLUTE P₀
  manifold each step, (V_candidate − 1)/Δt, with an in-tree comment
  naming step-by-step accumulation as the reason; but its
  zero-increment branch returned exactly 0 (fire_simulator_core.h:1309)
  and the rate path is tangent-only, so packet-free cells NEVER shed
  accumulated off-manifold deviation. First-order dose errors at the
  pilot's receivers telescope — the accumulated total scales with
  total expansion, not per-step size — which is precisely why the r68
  cap change could not help, why failures recur at the same ring
  temperature, why floor-layer cells fail first, why accepted
  residuals crept monotonically (8.47×10⁻⁴ by step 10), why r64's
  quiescent mask-cell fixtures were clean, and why the zero-g burn
  (chemistry packets restoring every burning cell; Δt-scaled neighbor
  errors ≈ 5×10⁻⁵ total) never tripped. Fix: every cell's divergence
  target carries exactly one absolute P₀ reference per step —
  assembly = tangent rate terms + (V(Qⁿ + ΔU_src) − 1)/Δt with ΔU_src
  possibly zero; frozen, stage-identical, realized through ordinary
  projection/advection; no state overwrite; EOS gate unchanged;
  double-restoration RED; the binding mutation RED is restoring the
  zero branch and reproducing the secular-creep signature. The r68
  cap justification is superseded (the 17/16 value stands on donor
  survival, retained against churn). Recorded alongside: the
  constraint-closure audit — elemental constraints (nullspace
  projection), momentum/density compatibility (commuting identity),
  inventory bounds (FCT + r60 envelope) all had restoration or exact
  preservation; the P₀ manifold was the only constraint enforced by
  detector alone. A future constraint added with only a gate is a
  design error by this audit. This closes the class, not the
  instance: no receiver-by-receiver ruling can recur, because no cell
  of any kind can accumulate manifold deviation anymore.

- **r70 (2026-08-16):** continuous pilot command ramp + manifold-exact
  acceptance, from the instrumented tier-6 budget — the first stop in
  this arc resolved by measurement rather than modeling, after the
  owner directed a stop to the ruling ping-pong. An isolated-worktree
  probe reproduced the failure exactly (limit 1.03122×10⁻³ to all
  printed digits; dump reproduces the solver update to ≤7×10⁻²¹) and
  decomposed the failing step at cell (22,21,0). Findings: (1) the
  bed-wall hypothesis is REFUTED — the z=0 face contributes exactly
  zero; the floor fails first only because the annulus lives there and
  a wall removes one dilution face. (2) r69's restoration is flawless
  in its own model — with self-donor (cell-mean) transport the cell
  lands at |dev| < 7×10⁻⁵. (3) The invariant limit is an identity:
  1.031×10⁻³ = donor-vs-self advective volume anomaly = 8.7×10⁻⁴
  h(T)-convexity mixing bias + 1.6×10⁻⁴ off-manifold MUSCL face
  states, recreated per step at Δt-invariant exchange Courants
  (~0.02/hot face). (4) The Courants are Δt-invariant because the
  17/16 cap doses per ACCEPTED STEP — a 1/Δt stiffness engine that
  also drives the projected velocities into the CFL selector,
  collapsing Δt 2.1 s → 1 ms with the ×1.1 growth cap preventing
  recovery. Two pins follow. **Ramp:** the pilot holds to
  T_cmd(t) = T_amb·(900/T_amb)^min(1,10t/t_ft) — approach in exactly
  t_ft/10, then the hold; per-step increments ∝ Δt in both phases, so
  the pilot becomes a genuine relaxation-class map (the r63
  projection class is now unpopulated but stays pinned), ramp
  velocities are physical (~0.2 m/s), and the death spiral is gone.
  Distinguished from the r63 τ-rejection: that was a power balance
  whose steady temperature depended on τ; a moving state-command is
  re-asserted regardless of losses, so the ramp constant shapes only
  the approach window and is derived (t_ft/10), not tuned.
  **Manifold-exact acceptance:** the Picard target recompute includes
  the advective volume anomaly, converging the divergence target to
  the value at which the candidate accepted state lies ON the P₀
  manifold — the scalar/EOS analog of the momentum compatibility
  identity D_i I_i = I_ρ,i D that the design already mandates; r69's
  absolute term is the fixed point's first iterate; mixing convexity,
  off-manifold reconstruction, and cp(T) nonlinearity are closed by
  construction at any contrast. Recorded moot/not-required: separate
  manifold-consistent reconstruction; the Vreman 1/Δt-strain exposure.
  The probe's instrumentation (env-gated RISE_FIRE_BUDGET_PROBE
  shortcut, per-face budget dumps) lives in the investigation
  worktree for adoption as a permanent diagnostic. The agent's
  stop-and-report classification ("per-step inconsistency, not
  license to weaken the gate") was again correct.

- **r71 (2026-08-16):** hold-band measurement point, from the first
  r70 gravity run — a stop whose evidence was mostly triumph: manifold
  closure plateaued at 0.20–3.57×10⁻⁶ (three orders under the gate,
  the r70 fixed point working), Δt stayed physical at 7–13 ms (death
  spiral gone), and the kernel IGNITED at 0.31 s inside the pilot
  window — the arc's first honest gravity-on ignition. The only red:
  accepted mask temperatures at the hold were 896.1–897.7 K against
  the r66 production band [899, 900] K. Diagnosis: genuine physics —
  the map re-asserts T_cmd, then ordinary Heun transport at Courant
  ~0.25 against a ~600 K contrast removes 2–4 K before acceptance,
  Δt-refinable and harmless. The r66 band is hereby owned as a second
  instance of the r65 category error: its "50× margin" was computed
  from the quiescent coupling scale and applied to the coupled
  accepted state, making the band a hidden Δt bound. The agent's
  refusal to choose among accepted-state compensation, a
  gate-serving Δt bound, or a reinterpreted band was correct — all
  three are rejected. Fix is the measurement point: command fidelity
  is gated by the quiescent unit fixture ([899, 900] K valid there,
  catching r64-class defects); production gates the RECORD-DERIVED
  floor — accepted mask temperatures during the hold must exceed
  873 K exactly, the highest in-scope T_AIT, so the kernel remains a
  spontaneous-eligibility seed (observed clears by ~23 K) — plus the
  unchanged 900 K command ceiling and the functional gate: first
  sustained heat release inside the 1·t_ft pilot window (already
  achieved at 0.31 s). No compensation, no new constants (the floor
  comes from the fuel record), no semantics change. Also noted from
  the agent's report: an R1 limiter-publication bug (α now published
  from the verified flux) was found and fixed in their worktree during
  r70 implementation.

- **r72 (2026-08-16):** hold-floor derivation corrected to T_pilot,
  from the r71 tier-6 gate failure — a mask cell's accepted hold
  temperature reached 871.44 K at t = 0.228 s (pre-ignition, as
  entrainment strengthens), under the r71 floor of 873 K; EOS stayed
  bounded at ≤2.6×10⁻⁶ and nothing else was wrong. The floor's
  derivation was the defect, owned as the third slip in this gate's
  history: r71 selected the SPONTANEOUS-route threshold (highest
  in-scope T_AIT, 873 K) by importing r58's "seeds robustly into
  spontaneous eligibility" nicety as if it were the pilot's function.
  The pilot's defined function in §3.3's eligibility graph is the
  PILOT route, whose source the mask is by definition; the matching
  record value is T_pilot = 600 K (design_pinned_exact). Corrected
  floor: accepted hold-phase mask minimum strictly above 600 K —
  cleared by ~271 K observed, and structurally consistent at the
  degenerate extreme (accepted T ≥ 900 − C·600 with advective CFL
  C ≤ ½ meets 600 K exactly). Ceiling and functional ignition gate
  unchanged; quiescent [899, 900] fixture unchanged. Rule pinned in
  design and spec: a gate's value must derive from the function the
  gated object serves. The agent's conduct — no tolerance change, no
  timestep change, no floor change without ruling — remains exactly
  right.

- **r73 (2026-08-16):** ceiling measurement point, from the tier-6
  stop where pilot-to-900 superposed with same-packet ignition
  (combustion 6.85 MW/m³) carried mask cell (24,29,0) to 910.8 K
  against the r66 "accepted ≤ 900 K" ceiling. The map was behaving
  exactly as pinned (beginning 858 K < 900, capped increment
  19094 J/m³); the gate was measuring the wrong object — the same
  error class as r71/r72, on the ceiling side. r58's thermostat is
  and always was a constraint on the PILOT (a pilot never superheats
  products), not on what chemistry does on top: a flame in the ring
  exceeding 900 K is physical and bounded by T_ad and the 2300 K
  physicality gate. Corrected: the ceiling is gated on the bit-exact
  pilot ledger (per-cell increment ≤ energy-to-reach-900, exactly
  zero at ≥900 K); the accepted-state ≤900 check lives only in the
  quiescent fixture; production's temperature ceiling is the
  physicality bound. This is the final owner ruling of the
  one-stop-one-ruling era: design-amendment authority for capstone
  completion is delegated to the implementation agent by the r73
  charter (recorded in the continuation prompt), with owner-locked
  invariants listed there.

- **r74 (2026-08-16):** physical ceiling ownership, from the first
  completed r73 gravity-on tier-6 run. The run was otherwise healthy:
  ignition at 0.310107861 s, reaction still active after the exact
  2.10637286 s pilot endpoint, EOS residual bounded by
  3.571783974×10⁻⁶, and physical timesteps throughout. After the pilot
  was off, however, cell (26,24,1) reached 2319.18033 K (with intermediate
  accepted values 2271.23895, 2293.45171, 2309.36046, and 2317.94324 K)
  against the owner-locked T_max < 2300 K capstone gate. The harness had
  supplied 2500 K to `PeriodicTransportConfig::adiabaticTemperatureK`:
  that number is the opacity table's certified evaluation-domain maximum,
  not methane's physical accepted-state ceiling. Ruling: case schema v1
  gains the identity-bearing derived echo
  `maximum_accepted_temperature_K=2300.0`; the existing conservative
  energy-polytope upper row consumes it at every canonical admissibility
  site, so over-energetic packets reject/reduce Δt before acceptance.
  Rejected: clamping or projecting accepted state (ledger corruption), a
  pilot/chemistry-only cap (wrong layer and incomplete), widening the
  physical gate, or conflating a record's numerical evaluation domain with
  a physical flame bound. This changes `case_record_id`; the opacity domain
  and the 1.0×10⁻³ EOS gate are unchanged.

- **r75 (2026-08-16):** reaction energy-headroom availability, from the
  r74 confirmation replay. The r74 gate first worked exactly: it rejected
  a 2.25567544 ms candidate above 2300 K and accepted the rebuilt
  1.12783772 ms packet at 2298.19213 K without clamping. The next cell
  history then proved rejection alone incomplete: at cell (26,24,1),
  accepted T advanced 2299.98262 → 2299.99350 K while trial Δt collapsed
  from 9.38220002×10⁻⁵ through 1.46596875×10⁻⁶ s and the reaction
  source remained outward at about 3.07 MW/m³. This is a cumulative
  boundary Zeno, not fp noise and not a reason to weaken the gate. Ruling:
  the upper energy row joins fuel, oxygen, and soot as a source-availability
  constraint. After shared-O₂ allocation, primary and soot extents take
  the same largest-representable factor λ_E whose directly evaluated frozen
  post-packet row is strictly negative; all species/energy/rate ledgers are
  rebuilt from that extent and the unreacted inventory remains stored. Full
  candidates that fit or point inward remain bit-identical. Rejected:
  timestep reduction or minimum-Δt bypass (cannot progress honestly), state
  clamping/projection (ledger corruption), widening 2300 K, invented cooling,
  and a tuned temperature cutoff (discontinuous, composition-blind rathole).
  The model-version echo changes `case_record_id`; r60, the EOS gate, and
  the opacity domain are unchanged.

- **r76 (2026-08-16):** strict binary64 ceiling representation, from the
  first r75 unit RED. The uncapped 2299 K reacting fixture reached
  3337.261156 K; headroom limiting made the directly evaluated 2300 K
  energy row negative, but thermochemical inversion still rounded the
  derived result to exactly 2300 K, violating the owner-locked strict
  `T_max < 2300 K` relation. Ruling: evaluate the source-availability
  endpoint at `nextafter(2300.0, -∞)`, the unique greatest binary64 value
  satisfying that relation. The case echo stays exactly 2300.0. Rejected:
  tuned margins, widened tolerances, accepting equality, and clamping the
  inverted temperature. This merely makes r75's already-pinned strict
  semantics representable; it adds no field and does not change
  `case_record_id`.

- **r77 (2026-08-16):** canonical emitted-byte headroom bracket, from the
  fresh r75/r76 Opto review. Exact measured counterexamples falsified the
  r75 global-maximality certificate: at Z=0.0001, T=2297.68 K a negative-row
  island remained 617 ulps above the bisection boundary, and at Z=0.005,
  T=2280 K 46 such islands appeared in the next million extents. The stronger
  promised equivalence also failed: at Z=0.00015829486134645083,
  T=2297.3099999999713 K the emitted row was
  -5.8207660913467407e-11 while the production inversion returned exactly
  2300 K. Ruling: selection is the invariant-preserving ordered-binary64
  lower bracket of the actual emitted packet evaluated by one canonical
  non-inlined source inversion; its feasible packet bytes are stored and
  emitted, never algebraically reconstructed. The full candidate remains
  byte-identical when strict. Pilot-only infeasibility rejects, and case
  generation requires its derived 900 K setpoint below the 2300 K physical
  ceiling. Rejected: exhaustive global scans, tuned margins, surrogate row
  certificates, pilot-command capping, and accepted-state clamping. The
  headroom model echo and `case_record_id` change; r76's predecessor remains
  an accepted-polytope endpoint, not an inversion certificate.

- **r78 (2026-08-17):** certified checkpoint build migration and the class-A
  performance boundary, from the first honest tier-10 wall-time stop. The
  r77 build reached durable checkpoint 9 at step 410 and t=1.46088723 s with
  maximum EOS residual 3.14507019x10^-6 and no physics-gate failure, but its
  measured continuation cost was 23.1 s/accepted step, only 16.5% parallel
  efficiency, and approximately 79 h remaining at the then-current dt. A
  sampling profile plus env-gated counters attributed 20--23% of a step to
  the pure per-cell manifold-exact target, about 8% to the pure per-face
  nonpressure momentum RHS, 0.5--1 s to repeated per-cell species-name
  lookup, 4--9% to roughly 10,600 spawn/join gangs (about 169,000 thread
  creations) per step, 7--11% to parallelizable projection maps and repeated
  stage-invariant preconditioner construction, and 1--1.5 s to state-only
  quantities rebuilt inside Picard. The largest structural sink was about
  41,600 multigrid smoother sweeps per step.

  Ruling: the producer-build SHA-256 remains mandatory, but a new executable
  may adopt an old checkpoint after an isolated, canonical
  resume-equivalence certificate advances old and new binaries from copies
  of the exact checkpoint for at least eight accepted steps and proves
  bit-identical dt, T_max, EOS maximum, and continuation frame digests. The
  certificate binds the checkpoint digest and both build hashes; production
  validates it before foreign-checkpoint admission, and records migration in
  run metadata without changing case identity. Performance changes are
  class A only when they pass that certificate and the 1-vs-N fixture
  unchanged, one change at a time. Smoother-count or solver-strategy changes
  are recorded as a future class-B, from-zero campaign. Rejected: the
  profiler worktree's `allow_foreign_build` environment bypass (unproved
  binary substitution), tolerance-based migration (not bit-exact), batching
  optimizations (no attribution), parallel FP dot products (reordered
  arithmetic), and changing multigrid strategy merely to finish this run.
  Migration events are pin-8 run metadata and do not regenerate
  `case_record_id`.

- **r79 (2026-08-17):** class-A performance campaign disposition, measured
  from the exact checkpoint-449 continuation under the r78 certificate. A
  clean eight-step baseline had median cost 22.105 s/accepted step (the
  earlier sampling profile measured 23.1 s). Six separately committed,
  bit-exact changes reduced the median to 15.728 s: a persistent indexed
  worker gang for the high-frequency fixed-range maps (21.952 s), parallel
  BiCGStab element maps with serial FP dot products plus reused smoother
  diagonals (21.385 s), stage-cached molecular transport with per-iterate
  Vreman retained (20.401 s), unchanged-kernel chunked manifold restoration
  (15.844 s), and solve-local reuse of the final-projection multigrid
  hierarchy (15.728 s). Every admitted point reproduced checkpoint steps
  450--457 bit-for-bit in dt, T_max, EOS maximum, and diagnostic-frame
  digest. The final measured speedup is 28.9% from the clean baseline and
  31.9% from the original profile.

  Ruling: these changes are class A and may migrate the durable run only
  through the final r78 certificate. All other attempted work remains out of
  the capstone continuation. A direct parallel rewrite of the manifold map
  was fast (17.25 s on its first measured step) but changed step 451 dt by
  three ulps; retaining the certified serial kernel inside fixed chunks was
  the admissible formulation. Direct momentum-RHS parallelism improved its
  trial step by about 5.8% but changed continuation dt by seven ulps. Direct
  species-index call-site substitution changed the same continuation bits;
  a caller-preserving record lookup was bit-exact but 0.6% slower after the
  molecular cache removed that path from the wall-time critical path. A
  stage-wide projection cache was bit-exact but 0.5% slower, parallelizing
  the residual max norm was neutral, and moving the remaining low-frequency
  explicit gangs onto the parked pool was 0.7% slower; each was reverted.
  Rejected: ulp-tolerant equivalence (not pin-8 identity), retaining a fast
  but divergent candidate, combining changes to hide attribution, changing
  the 41,600-sweep multigrid strategy, or shortening/substituting the tier-10
  evidence. At the final class-A rate the profiler's approximately 79 h
  continuation projects to about 53.8 h, still beyond the standing 40 h
  launch ceiling. The durable checkpoint remains the recovery root; the
  next campaign, if authorized, is class B and starts from zero. No case or
  solver semantics changed, so `case_record_id` is unchanged.

- **r80 (2026-08-18):** canonical two-class acceptance for the certified
  solver's pressure-open R0/R1 active set. The stopped tier-10 oracle log
  recorded repeated augmented active-set cycles at accepted-step candidates
  spanning 6.99×10^-5 s down through 4.86×10^-5 s; reducing Δt moved the
  switching surface but did not create a Cauchy fixed point. The durable
  step-3479 checkpoint at t=2.8854439500002069 s and its streamed prefix are
  preserved as golden reference data (checkpoint SHA-256
  `1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`).

  Ruling: the active set is a derived discrete control. Stable classifications
  retain the prior behaviour. A detected cycle canonically chooses the solved
  cycle branch minimizing the maximum deadband complementarity violation,
  with exact ties resolved lexicographically outflow-first, re-solves that
  branch frozen, and retains the existing divergence and total-head gates.
  The outer coupled Picard and verification gates contain only continuous
  residuals; disagreeing classifications are compared frozen at the accepted
  target by the same ordering before those residuals are rechecked. Diagnostics
  record class, cycle length, differing faces, and discrepancy. Rejected:
  first/last wins (seed/order dependence), Boolean union/intersection (possibly
  unsolved synthetic branch), a wider velocity deadband (hides reversal), and
  relaxation/averaging (no certified Boolean meaning). R2 is a one-shot
  endpoint diagnostic and is not a sibling. The eliminated 3-D and dense 1-D
  routines are verification-only independent comparators, never R0/R1 owner
  paths; they deliberately retain cycle rejection so the old pathology remains
  directly observable rather than copying the production selector into its
  oracle. Periodic projection has no open active set. The rule is reference-solver run
  semantics, not physical case authorship; it is recorded in producer/run
  metadata and does not change `case_record_id`, preserving the stopped prefix
  as an oracle under its original producer identity.

- **r81 (2026-08-18):** cycle proof, diagnostic persistence, and scalar-donor
  separation for the r80 reference-solver active-set rule. Fresh adversarial
  review measured the compact cycling fixture's selected pressure branch at a
  2.9506166530252633e-3 m/s complementarity discrepancy against the 1e-10 m/s
  deadband. The implementation then reused that Boolean pressure bit as the
  scalar donor, which could copy ambient material outward or interior material
  inward at resolved speed. The same review showed that the outer Picard path
  called any acceptance/verification disagreement a two-cycle, even on a first
  ordinary transition, and allowed later stable solves to erase earlier cycle
  diagnostics.

  Ruling: a cycle requires a repeated solved classification. First visits and
  verification-only changes continue the ordinary Picard history; after a
  repeat, every distinct cycle state is solved frozen at the accepted target and
  ordered by r80. Pressure classification governs the pressure equation only.
  Scalar upwinding follows the accepted velocity sign outside the deadband and
  retains the pressure class only inside it. Cycle diagnostics accumulate
  monotonically through stage, step, checkpoint, and run metadata under a
  versioned algorithm tag. Rejected: first-transition canonicalization (changes
  stable paths), pressure-bit scalar donation (wrong resolved donor), velocity
  clipping (changes the accepted solve), and last-solve-only diagnostics (loses
  trajectory-changing evidence). This is reference-run semantics and does not
  change `case_record_id`.

  Implementation closure extended the scalar-sign separation through the
  nonzero diffusive/conductive boundary term and the boundary ghost used by
  high-order reconstruction. Checkpoint format v8 preserves the current and
  prior active-set algorithm tags; v5/v6 checkpoints are admitted only as
  explicitly tagged legacy history, while v7/v8 checkpoints must carry the
  exact current tag. Thread-identity disagreement now fails before run
  accumulation or checkpoint publication. The immutable step-3479 v6 golden
  checkpoint then advanced eight full production-owner steps to step 3487 with
  accepted dt=5.4851762335687565e-5 s, ten discontinuous events, cycle length 2,
  four differing faces, and maximum discrepancy
  1.1766913664658803e-3 m/s; its source SHA-256 remained unchanged. Independent
  REDs reject union/intersection, pin the equal-discrepancy outflow-first tie,
  exercise nonzero diffusion/conduction and the high-order ghost, and reject a
  checksummed checkpoint with a mutated algorithm tag.

- **r82 (2026-08-18):** two-tier production-solver architecture. The certified
  fp64 solver remains the reference oracle; its stopped 2.8854439500002069 s
  tier-10 prefix and V-tier fixtures become golden validation data rather than
  a production-throughput path. The production solver is a separate fp32,
  Metal-GPU-first implementation with a conservative semi-Lagrangian flux-form
  PPM remap, local semi-implicit sources, record-compiled thermochemistry and
  Planck-mean tables, and exactly one variable-density projection per scheduled
  step. Finite deviations are monitored and gated by an oracle calibration
  package, never converted into fail-closed dt halving; nonfinite/runtime/I/O
  failures still abort structurally.

  The target is the 976,272-cell, 25.032480502915522 s tier-10 methane case in
  at most one hour on the recorded 40-core M4 Max GPU. A 1/480 s maximum step
  gives about 12,016 steps; the pinned 250 ms p95 budget yields about 50.1
  minutes for stepping and 9.9 minutes for I/O and handoff. State plus working
  storage is capped at 2 GiB and remains GPU-resident. The validation band is
  derived from oracle block variability, adjacent refinement difference, and
  independently measured fp32 quantization, then capped by explicit scientific
  ceilings; a calibration wider than a ceiling rejects the model rather than
  widening the gate. Production outputs retain the existing case identity,
  sequence-backed provenance, preview-primary labels, and sidecars, with a
  distinct solver/kernel/table/device producer record.

  Rejected: porting the full fp64 certificate stack into the fast path (misses
  budget and duplicates the oracle), CPU-first execution (measured 23.1 s/step
  and 16.5% efficiency), MacCormack/BFECC with clipping (nonconservative), more
  than one projection (dominant cost), runtime tolerance widening or halving
  (restores stalls), and changing the oracle's measured 41,600 multigrid sweeps
  per step (separate class-B campaign). The companion contract is
  `FIRE_SMOKE_PRODUCTION_SOLVER.md`.

- **r83 (2026-08-18):** production P0 capability and certified-table landing
  pins. A credible GPU baseline must execute work: the platform capability API
  compiles and dispatches an embedded fp32 identity kernel and verifies returned
  bytes, while non-Metal platforms expose an honest unavailable stub. Device
  name/registry/family, threadgroup width, and unified-memory status are
  producer evidence. Discovery-only probes were rejected because they cannot
  establish command/pipeline viability; a test-only Objective-C++ executable
  was rejected because later production kernels need one owned library seam.

  The table compiler consumes only canonical thermochemistry/opacity records.
  Species h_s/cp midpoint error is capped at cp_min times 0.25 K, one quarter of
  a 1 K-equivalent P0 gate; Planck-mean relative error is capped at 0.5%, one
  quarter of V5's 2% ceiling. These are derived compiler semantics, not tuning
  arguments. The one-preimage manifest binds source IDs, domains, every fp32
  knot/value byte, measured fp64 error bounds, and compiler version. Runtime
  extrapolation is rejected. Hand-copied constants, discovery without dispatch,
  caller-selected tolerances, and silent CPU fallback under a Metal producer
  tag are rejected.

- **r84 (2026-08-18):** production P1 conservative-remap semantics. The fp32
  fast path uses a flux-form semi-Lagrangian PPM sweep: every face is backtraced
  at midpoint velocity and integrates the complete donor parabola interval,
  including all crossed cells, so the shared face flux remains conservative
  above unit Courant number. The fourth-order interface stencil and
  cell-average-preserving parabola are limited by one cell coefficient shared
  across the complete conservative tuple. This makes species, mixture
  fraction, elements, and sensible enthalpy use identical weights instead of
  relying on a post-update repair.

  Periodic wrap adds exact whole-domain integrals before the canonical
  remainder; pressure-open ghost selection follows accepted velocity sign,
  with ambient inflow and nearest-interior outflow, and walls have zero swept
  interval. Fixed SoA order, disjoint face/cell writes, one padded Blelloch
  prefix tree, and disabled fast-math pin same-device determinism. Rejected:
  MacCormack/BFECC (nonconservative), adjacent-cell CFL truncation (invalid at
  the target schedule), component-wise limiters (break affine common weights),
  atomics/subgroup-dependent scans (nondeterministic), and post-remap clipping
  (hidden state repair). P1 must demonstrate >=1.8 manufactured order and the
  45 ms tier-10 p95 allocation before projection work begins.

- **r85 (2026-08-18):** production P1 executable swept-interval closure after
  the first fresh implementation review.  Measured counterexamples were an
  ordinary `0.1f`, `C=0.3` free stream drifting across several ulps; periodic
  seam fluxes differing despite equal endpoint velocities (and an unequal-seam
  case changing total 36 to 32); a folded `C=0.75` departure map producing
  `-0.5` from a nonnegative unit cell; finite `FLT_MAX` inputs publishing NaNs;
  and an admission formula permitting about 2.67 GiB while claiming two.
  The ruling factors fractional integrals by interval length, accumulates
  crossed donors canonically, makes face zero the byte-canonical periodic seam,
  and requires a finite nondecreasing backtraced face map selected before
  dispatch.  It also makes output finiteness and complete checked Metal-buffer
  accounting structural admission rules.  Absolute Courant remains unbounded
  by adjacent-cell CFL, so uniform sweeps across cells and whole domains remain
  valid.  Rejected: clipping, a fixture-serving outgoing-flux repair, duplicate
  seam evaluation, acceptance of folded maps followed by dt retry, and deferred
  nonfinite detection.  None changes case identity: these are pre-release
  production-kernel semantics and validation gates.

- **r86 (2026-08-18):** production P2 one-projection executable contract.  P1
  closed at exact commit `d9e7263d` after three fresh reviewers reached zero
  P1; independent M4 Max measurements ranged from 3.70 to 4.37 ms device p95
  and 20.95 to 23.48 ms completed-call p95, materially below the 45 ms remap
  allocation.  The adversarial closure required local interval arithmetic,
  an ordered periodic seam, a bounded finite-C quotient/remainder, exact-fp32
  departure-map admission, complete 2 GiB accounting, and structural finite
  output checks before projection work was permitted to begin.

  Ruling: P2 is one matrix-free fp32 variable-density MAC projection with
  arithmetic-mean face density and the same centered `D/G` pair as the oracle.
  Walls prescribe exact zero normal velocity.  Pressure-open faces freeze one
  pre-solve class: static-gauge outflow or a provisional-velocity total-head
  inflow linearization, with the factor-two half-cell Dirichlet coefficient.
  The solve is twelve fixed geometric-multigrid V-cycles from zero pressure,
  three `omega=2/3` Jacobi pre/post sweeps, 32 coarsest sweeps, volume-weighted
  odd-grid restriction, trilinear prolongation, and fixed padded reductions
  only before/after the schedule.  Finite residual misses are returned as
  monitored validation evidence; malformed/nonfinite/command failures abort
  structurally.  The milestone must satisfy the independent manufactured
  `5e-3 U/L` ceiling and both device and completed-call 120 ms p95 gates before
  coupled transport begins.

  Rejected: Krylov dot-product solves (global ordering and synchronization),
  adaptive cycles or retry projection (hidden acceptance loop), CPU coarse
  fallback (breaks residency and provenance), harmonic density (unearned
  oracle departure), nonlinear boundary iteration (multiple projections in
  substance), and fail-closed halving for a finite residual (restores the
  throughput stall).  P2 is pre-release production algorithm semantics; case
  authorship and `case_record_id` remain unchanged.

  P2 landed as a GPU-resident schedule with no CPU solve or fallback behind the
  Metal producer tag.  The independently manufactured odd-grid comparison
  measured maximum CPU/Metal differences of 1.19209e-7 Pa and 2.98023e-8 m/s;
  the fp32 comparator residual was 3.06303e-5 s^-1 against the certified fp64
  oracle's 2.65183e-5 s^-1 (ratio 1.15506).  Across repeated 976,272-cell M4
  Max trials, command p95 measured 10.55--11.09 ms and completed-call p95
  21.28--23.94 ms, below the 120 ms allocation with more than fourfold margin.
  The admission certificate counts simultaneous caller vectors, Metal input
  and output buffers, every hierarchy payload, boundary evidence, fixed-tree
  scratch, diagnostics, and serialized level parameters under the two-GiB
  cap; unsupported platforms return an explicit no-result error.

  The first fresh Metal review then exposed six implementation-certificate
  gaps without changing the r86 algorithm: the peak accounting omitted the
  simultaneous device and caller-owned inflow-classification bytes; the
  Metal validation band measured corrected velocity only; schedule counters
  were inferred rather than counted; allocation failure did not enclose the
  whole API boundary; same-device repetition omitted non-pressure payloads;
  and nonzero open/wall semantics had only CPU witnesses.  The corrected
  certificate counts both live classification copies and repins the closest
  independently searched boundary pair at 2,147,483,472 bytes admitted and
  2,147,483,796 bytes rejected.  Actual encoder counters, full-payload repeat
  equality, persistent-allocation and injected command-failure REDs, the
  provisional-plus-corrected velocity scale, and Metal executions of the
  sinusoid, six walls, expansion, both total-head signs, and both fp64 oracle
  comparisons now bind those claims.  The repaired on-device run retained
  the scientific ratio 1.15506 and measured 10.967 ms device p95 and 23.923 ms
  completed-call p95.

  A second fresh review found one comparator inconsistency and two evidence
  gaps before P2 was closed.  Metal's monitored-acceptance scale included raw
  wall-normal momentum even though the prescribed wall map had already
  discarded it; a closed box with residual `0.4 s^-1` and arbitrarily large
  wall input could therefore fail on CPU and pass on Metal.  The ruling is to
  measure provisional velocity only after the same wall classification on
  both paths.  The replacement RED fills and inspects every face on all six
  wall planes, then combines discarded `1e20` wall momentum with the
  incompatible residual and requires identical monitored rejection.
  Independent recomputation now binds pre/post residuals and pressure-open
  complementarity, the cancellation-sensitive mean runs on Metal, and the
  published density/momentum/velocity relation is checked directly.  Separate
  post-commit status and output-corruption seams bind command execution failure
  and nonfinite-output clearing without stale diagnostics.  The repaired M4
  Max run preserved the oracle ratio `1.15506`, with `10.855 ms` device p95 and
  `23.306 ms` completed-call p95.  These are validation-evidence repairs, not
  new operator semantics; case identity remains unchanged.

  The final mutation pass rejected zero-valued diagnostic witnesses as
  insufficient: a hardwired zero would have passed both the original
  complementarity and mean-removal comparisons.  The settled gates use the
  measured pressure-open role reversal (`0.0941987 m/s` complementarity) and
  the closed-box incompatible target whose fixed-tree right-hand-side mean is
  exactly `40 s^-2`; both are independently recomputed and executed on Metal.
  The final on-device run measured `10.575 ms` device p95 and `23.316 ms`
  completed-call p95 with the scientific ratio unchanged.

- **r87 (2026-08-18):** production P3 coupled-transport order. Evidence from
  P1/P2 showed that the two kernels separately fit their allocations, but the
  architecture still left load-bearing choices open: sequential versus
  unsplit directional updates, when Vreman coefficients are frozen, which
  velocity transports dual-volume momentum, and whether forces precede or
  follow the only projection. The ruling is one immutable beginning snapshot:
  wall-aware face velocity; one beginning-state Vreman/molecular/gravity
  assembly; x/y/z remap fluxes all traced from those same bytes and accumulated
  in fixed axis order; one frozen nonpressure momentum increment; then exactly
  one r86 projection using remapped density and the authored `S_div`. Momentum
  uses the same swept-profile operator on its dual volumes with arithmetic
  beginning-MAC transport velocity. Chemistry, radiation, pilot, phase change,
  and bed flux remain P4.

  Validation binds V1 rest, V2 variable-density/Vreman/source-divergence
  manufacture, V3 conservative/common-weight transport, an unsplit cross term,
  a dual-volume momentum pulse, and at least eight preserved certified-prefix
  slices. P3 receives a 200 ms tier-10 completed-call allocation (45 remap,
  20 coefficient/force assembly, 120 projection, 15 orchestration). Rejected:
  sequential directional splitting, post-remap momentum repair, coefficient
  recomputation after an axis, cell-centered momentum transport, implicit
  viscosity iteration, and a second projection. These are pre-release
  production-kernel semantics; case authorship and `case_record_id` do not
  change.

- **r88 (2026-08-18):** P3 multidimensional, dual-grid, stability, residency,
  and oracle-slice closure before implementation. Fresh review falsified r87's
  additive “unsplit” model with an isolated cell: individually admissible
  `C_x=C_y=0.75` outflows produce `q'=-0.5q`, while unit x/y Courants omit the
  translated corner entirely. The ruling replaces it with the fixed symmetric
  palindrome `x(dt/2),y(dt/2),z(dt),y(dt/2),x(dt/2)`, composing P1's arbitrary-
  Courant conservation and monotonicity without a clamp. A sum-CFL gate was
  rejected because it restores the throughput wall; a new multidimensional
  swept-volume kernel was rejected because it duplicates P1 before the
  production path exists.

  Momentum is now executable on three separately sized dual lattices. Each
  component shares its limiter only with its collocated dual density; carrier
  interpolation and periodic/wall/open rules are pinned for every transported-
  component/sweep-axis pair. The force stencil inherits the certified open
  reference exactly in fp32: relative face gravity, centered ghosted gradient,
  `mu=rho(nu_mol+nu_vreman)`, deviatoric stress, and its normal/transverse face
  divergence. A derived `3/8` explicit deviatoric bound selects and records
  deterministic viscous substeps; a single unstable frozen RHS and implicit
  solve were rejected.

  Separate P1/P2 memory claims were also rejected as noncompositional. P3 must
  expose an interval-lifetime working-set certificate for resident state,
  palindrome/dual scratch, forces, and the complete projection peak, with no
  inter-stage full-grid readback and a checked two-GiB boundary witness. Finally,
  “eight prefix slices” is fixed to certified steps 3480–3487 extracted by pure
  serialization from the immutable step-3479 checkpoint. Bands follow r82's
  `max(3 sigma,1.25 Delta_refine,B_fp32)` rule; zero oracle thread variance may
  zero only sigma, never the fp32 comparison allowance. These pre-release
  corrections do not change case identity or the 200 ms budget.

- **r89 (2026-08-18):** P3 executable dual ownership and certificate closure.
  Review showed that r88 still demanded an impossible unit-Courant identity
  from two nonlinear half-remaps, treated publication seams and open/wall MAC
  endpoints as uniform dual cells, made remapped dual density compete with
  r86's cell-derived face density, misstated the certified wall ghost as
  free-slip, and applied a constant-coefficient Fourier bound to the actual
  variable-coefficient operator. The ruling moves exact diagonal translation
  to total `(2,2,1)` Courants, where every palindrome submap is an integer
  shift, and uses an independent five-pass oracle for fractional Courants.

  Unique periodic DOFs, publication duplicates, prescribed wall planes,
  pressure-open reservoir endpoints, cross-carrier ghosts, and corner
  precedence are pinned for all 54 component/sweep/side orientations. Dual
  density is limiter-only and discarded; remapped cell density's r86 arithmetic
  face mean is the sole viscosity/projection/accepted-velocity denominator.
  Every wall velocity component uses the certified odd no-slip reflection.
  Viscous work now derives `N_nu` from the actual assembled operator's maximum
  absolute row sum, with the exact fp32 ceil association, an eight-substep
  capability ceiling, and a tier-10 timing proof; the unproved `3/8` shortcut
  is rejected.

  P3 also gains a resident P2 encoder seam, an allocation high-water ledger,
  no-interstage-readback capture, and process-wide projection counters. The
  exact steps-3480--3487 package schema cross-binds r80 dt/Tmax/EOS/frame
  evidence, separates later source maps, and carries operation-count/operand
  `B_fp32` plus restriction-matched refinement inputs. These changes are still
  pre-release production semantics and do not alter case identity, 200 ms, or
  two-GiB limits.

- **r90 (2026-08-18):** P3 outward stability and independent evidence. Review
  found no remaining physical-operator conflict, but ordinary fp32 row sums,
  ceil products, and `dt/N` could round toward an unsafe one-fewer-substep
  result. The ruling promotes stored coefficient bytes exactly, rounds every
  row addition and schedule product outward with `nextafter`, then certifies
  the represented fp32 `dt_sub` against the outward row bound and increments
  the pre-dispatch integer until `dt_sub Lambda_up<=2`. Threshold pairs for
  N=1..8, downward row-sum rounding, upward division, and the 8/9 capability
  edge are RED.

  The impossible “reverse palindrome” mutant is replaced by the genuinely
  different axis-reversed palindrome; an independent oracle must first prove a
  nonzero delta. Boundary evidence becomes a full component/sweep/side/role
  bitmap with separate open signs. Finally, kernel operation counts and tree
  depths come from agreement between a trace build and an independent topology
  walker, not the allowance manifest itself; disagreement, `n epsilon>=1`, or
  a bound above the scientific ceiling fails generation. This is certificate
  and test governance only; P3 semantics, case identity, 200 ms, and two GiB
  are unchanged.

  Runtime review additionally pinned open-endpoint participation: exclude a
  pressure-open normal endpoint only from its normal sweep, retain transverse
  remap and relative gravity, omit the reference's nonexistent endpoint
  viscous increment, then project it normally. Resident full-grid resources
  are Private Metal buffers; Shared storage is restricted to fixed diagnostics
  and scheduled publication staging, closing the direct-host-dereference hole
  in encoder-only evidence. Finally, the golden extractor runs a discarded
  zero-source certified shadow from each immutable beginning state; source
  packets cannot be subtracted from the nonlinear sourced step. The ordinary
  continuation separately reproduces r80 evidence and the shadow never affects
  accepted state or checkpoints.

- **r91 (2026-08-18):** executable P3 Metal preflight and isolated oracle
  shadow. Review found that r90's per-row binary64 certificate could not run in
  a resident Apple Metal kernel, whose full grids are Private and whose shader
  language has no fp64. The ruling accumulates actual stored fp32 coefficient
  rows outward with `nextafter` on GPU, fixed-max reduces one fp32 bound, and
  transfers only that scalar; binary64 schedule selection and represented
  `dt_sub` certification then run on the host. A high-precision fixture oracle
  and no-private-grid-readback guard bind the split.

  The `N_nu<=8` capability is qualified only by a tier-10-shaped request that
  selects exactly eight and completes preflight, eight updates, one gravity
  addition, and synchronization within 20 ms; N=7 and pre-dispatch-rejected N=9
  controls prevent an easier case from qualifying. Finally, the discarded P3
  shadow zeroes the certified solver's separate fuel-bed boundary tuple and
  restores underlying wall geometry in addition to zeroing source packets.
  Original bed inputs remain serialized as excluded P4 evidence, while the
  sourced continuation still reproduces r80. No physical operator, identity,
  or budget changes.

- **r92 (2026-08-18):** line-resolved pressure-open ambient tuples. The first
  mixed-boundary dual-momentum implementation exposed a representation gap:
  r89 requires normal ambient momentum `rho_amb u_boundary`, but the frozen
  carrier varies by boundary line while P1 accepted only one ambient value per
  component. Lower and upper open sides can also carry different roles and
  values in the same batch. The ruling adds optional, distinct lower/upper
  `(component,line)` tuples to the shared P1 CPU/Metal kernel, with exact
  `component*lineCount+line` indexing; the component-only path is preserved
  byte-for-byte.

  Averaging a side was rejected because it changes conservative boundary
  flux; dispatching each line separately was rejected because it breaks the
  resident batching and command budget; and encoding ambient data as ghost
  conservative state was rejected because it changes limiter ownership. A
  side/line-distinct exact-flux fixture and malformed/nonfinite REDs bind the
  seam. This closes an internal representation detail of the already pinned
  P3 operator and changes no case identity, physical semantics, or budget.

  Fresh implementation review found that r85's requested-byte admission was
  not its promised actual-buffer certificate: the legacy below-cap witness was
  936 requested bytes under two GiB but 147,456 bytes over after M4 allocation
  rounding. The implementation now outward-rounds every one of the eleven P1
  Metal buffers to 16 KiB, independently sums every runtime `allocatedSize`,
  and rejects before command creation if either the certificate or the cap is
  exceeded. Requested-length accounting and reliance on allocation failure
  were rejected because neither bounds the admitted peak. Standalone Metal
  result publication is also atomic under persistent allocation denial. This
  restores r85's existing resource/fail-closed rules; it is not a new operator
  or identity change.

- **r93 (2026-08-18):** measured Metal frozen-force comparison. Fresh review
  found that the provisional `3e-5*max(1,|a|,|b|)` CPU/Metal gate became a
  broad absolute allowance for every sub-unit force and viscosity value. An
  on-device ordered-binary32 audit over zero flow, nonlinear periodic flow,
  every wall/open orientation, every periodic seam, variable-density
  authority, and deviatoric compression found exact zeros and seam copies,
  with a worst nonzero difference of 8 ULP at x-viscous face 25 (`0.778665`
  versus `0.778666` at printed precision). The ruling pins an 8-ULP gate on
  every published value and retains exact-byte gates for analytic zeros,
  canonical seams, and repeat execution.

  The absolute band was rejected because it hid millions of ULPs at small
  magnitudes; universal byte identity was rejected because it contradicts the
  measured repeatable target/compiler result; and a maximum-only diagnostic
  was rejected because it does not gate each cell. Vreman association/order,
  the strict `2/3` coefficient, all twelve actual Metal allocations, and nil
  command/encoder failure paths receive separate binding REDs. This closes the
  comparison evidence for the existing r89--r91 operator. It does not change
  case identity, physics, provenance, or the 200 ms/two-GiB budgets.

- **r94 (2026-08-19):** resident force certificate and transfer boundary.
  Implementation planning exposed a resource contradiction in r91's literal
  sparse-row wording. Tier 10 has 2,958,916 MAC faces: only 27 stored fp32
  coefficients per face cost 319.56 MB, 54 cost 639.13 MB, and 96 cost
  1.136 GB before resident state and projection. The operator itself is
  matrix-free. The ruling retains its authoritative stored density, frozen
  viscosity, spacing, and boundary bytes and uses the analytic global bound
  `24*max(mu_eff)*max(1/rho_face)/dx^2`, with every reduction, reciprocal, and
  product rounded outward. A strict-fp32 small-grid column oracle independently
  assembles periodic and mixed wall/open variable-density matrices and must lie
  below the scalar envelope; N=7/8/9 gates retain the r90--r91 schedule.

  Residency now has an observed transfer ledger: one fixed `Lambda_up` scalar
  may cross before scheduling; the full viscous loop, boundary publication,
  one gravity addition, and one projection have zero host transfers; optional
  Private intermediate snapshots stage together only after command completion
  for canonical CPU FNV evidence. Sparse matrix materialization was rejected
  for peak/bandwidth cost, a tuned global margin for lack of proof, per-substep
  readback for changing the validated architecture, and serial GPU FNV for
  inserting a single-lane full-grid pass. The measured starting point is
  11.3556/23.6152 ms device/completed-call p95 for projection and
  21.1372/35.2078 ms for the tier-10 palindrome. This is an executable
  certificate/evidence amendment only; physics, identity, validation bands,
  two-GiB cap, and 200 ms budget are unchanged.

- **r95 (2026-08-19):** measured eight-substep force composition bound. The
  resident N=8 periodic fixture measured a maximum final absolute CPU/Metal
  drift of exactly `2^-25` (2.98023223876953125e-8). Ordinary values had a
  maximum 2-ULP drift, below the naive 64-ULP sum of eight r93 ceilings, but a
  cancellation cell crossed zero (`-4.47471e-10` versus `+7.09406e-10`) and
  therefore produced a meaningless ordered distance of 1,614,348,289 ULP.
  The ruling gates every composed face by `ULP <= 64 OR abs <= 2^-25`, with
  exact bytes retained for analytic zeros, wall prescriptions, and periodic
  publication. Widening the per-kernel rule, raw ULP alone, and a universal
  absolute band were rejected respectively for contradicting unchanged r93
  evidence, zero-crossing singularity, and weakening ordinary magnitudes up to
  14.4414. This measured comparison rule changes no physics, bytes,
  validation contract, identity, or budget.

- **r96 (2026-08-19):** mixed-boundary composed-force comparison correction.
  Fresh review disproved r95 outside its periodic uniform-density fixture. A
  160-case exact-N8 matrix crossed periodic, all-open, all eight mixed
  wall/open masks, `Cv={0,0.07}`, and eight variable-state phases. Twenty faces
  exceeded r95. Raw maximum distance was 8192 ULP at `2^-25`; global maximum
  absolute drift was `2^-21` at 8 ULP; and the maximum absolute drift among
  faces over 64 ULP was exactly `2^-23`. The binding witness is mixed
  x-wall/open, y-open/wall, z-wall/open, phase 4, `Cv=0`, y-face 18: CPU
  `-0x1.6c96p-10`, Metal `-0x1.6c9ep-10`, 1024 ULP and `2^-23`, at
  `Lambda_up=83.52005767822266 s^-1`, `dt=0.17959757149219513 s`.
  The corrected gate is `ULP <= 64 OR abs <= 2^-23`; analytic zeros, wall
  prescriptions, and seam copies remain byte exact. The 1024-ULP alternative
  was rejected because it weakened ordinary-value evidence. No kernel bytes,
  runtime acceptance, case identity, or budget changed.

- **r97 (2026-08-19):** resident force--projection transaction and golden P2
  evidence. The eight frozen-viscosity updates and one gravity addition now
  hand their Private packed momentum allocation directly to one resident P2
  solve. Observed command/access counters require one scalar preflight
  transfer, zero interstage full-grid reads or Private-to-Shared blits, zero projection uploads, one
  projection invocation, and one terminal stage; injected access or a hidden
  second projection fails atomically. On the exact tier-10 shape, five N=8
  force--projection calls measured 42.8205--47.4189 ms device and
  66.1993--71.5982 ms completed p95, with 512,093,336 observed bytes below the
  conservative 524,688,024-byte
  certificate. The analytic rest transaction is exact `+0` for pressure,
  momentum, and velocity.

  The immutable step-3479 checkpoint (SHA-256 `1b944176a1dad4937872b0b63057`
  `854659cb37672ff833635c3d1827cbcb4947`) now owns a shared-periodic
  production/fp64 projection comparison. The production residual is exact
  `0x1.48p-15` after an exact `0x1.3ap-11` pre-residual; the fp64 residual is
  `6.708440414004929e-5`, from a `6.035506397530279e-4` oracle pre-residual;
  final residual ratio is 0.5828574834 and reduction-factor ratio is
  0.5873762212. Maximum projected
  velocity disagreement is `1.2527614002610932e-5 m/s`, below the existing
  `3e-5` comparator, and the production residual is below the unchanged
  validation band. The contract therefore absorbs the measured fp32 floor;
  no tolerance or physics change is made.

- **r98 (2026-08-19):** resident ownership and transfer-observer closure.
  Fresh review found that the resident P2 API trusted opaque Metal buffers and
  that a caller-labelled observer recognized only Private-to-Shared copies.
  The seam now requires full-grid Private density/target resources and one
  canonical packed Private momentum allocation with exact axis offsets and
  bounds before any projection command or invocation. Transfer phase is an
  RAII scope: uploads and terminal publication are explicit, while every
  other blit is interstage and any host-visible/Private crossing in either
  direction is forbidden. Private-to-Shared and Shared-to-Private injected
  blits, plus Shared, short, and aliased resident inputs, are independent
  atomic-failure REDs.

  Every Metal allocation is also observed at its creation wrapper and the
  terminal count/`allocatedSize` sum must equal the independently enumerated
  resident, upload, borrowed, and staging topology. Named local work roles are
  checked against their required Private/Shared modes, raw allocation sites are
  confined to the observing factories, and force reconciles again at the
  publication boundary. This closes omissions in either the implementation or
  its two-GiB ledger. The exact tier-10 N=8
  transaction measured 42.8285/66.8613 ms device/completed p95 and retained
  512,093,336 observed bytes below the 524,688,024-byte certificate; standalone
  P2 measured 10.5484/25.1084 ms. No numerical operator, comparison bound,
  validation tolerance, checkpoint byte, identity, or budget changed.

- **r99 (2026-08-19):** golden divergence steady-state floor. Sixteen complete
  resident force--projection cycles start from the immutable step-3479 state
  and reuse its fixed density, MAC bytes, dt, periodic geometry, and authored
  divergence target. The force leg is neutral (`nu_mol=Cv=g=0`) to isolate the
  single-projection recurrence while still exercising the actual resident
  handoff. Each cycle selects N=1, observes no interstage transfer, invokes one
  projection, and passes monitored validation. The residual never exceeds the
  accepted r97 floor `0x1.48p-15 = 3.910064697265625e-5 s^-1`; it finishes at
  `0x1.48p-16` and repeats a bounded fp32 oscillation rather than creeping.
  The full sixteen-value trace is exact-pinned. No additional projection,
  timestep response, tolerance change, or operator change is authorized.

- **r100 (2026-08-19):** resident transport and full-step ownership. The
  production order is fixed as force, advection, source maps, then one
  projection. Force-updated momentum and the beginning nine-channel cell tuple
  enter the dual/cell palindromes, while the beginning accepted MAC velocity
  remains the frozen carrier for all five submaps. Transported dual density is
  limiter-only; r86 reconstructs its authoritative face density from the
  post-source cell density. Source increments are explicit resident operands
  and are exact zero only for the isolated golden shadow until the next
  thermo/source-map milestone supplies them.

  Full grids remain Private from the boundary upload through transport and P2;
  scoped ledgers observe all transfers, commands, allocations, and the combined
  two-GiB high-water mark. Certified comparison is physics-class rather than
  ULP-class: conservative ledgers, steep-front envelopes, and section-5.1
  oracle-variability bands are frozen before production inspection. Eight fixed
  golden slices measure the composed step, and monitored deviations publish
  evidence without retry, dt response, clipping, or an extra projection. This
  ruling changes ownership/order only, not the oracle, checkpoint, identity, or
  validation tolerance.

- **r101 (2026-08-19):** resident mixed-boundary dual transport. Nine frozen
  component/sweep carrier and line-ambient layouts are uploaded once at the
  step boundary, after which the force-updated packed Private momentum executes
  all fifteen dual submaps in one command with zero host access. The result is
  one canonical packed Private allocation ready for r98 P2. On-device mixed
  wall/open comparison is deterministic and matches the independent strict-
  fp32 oracle within the existing Metal comparator band. The outward-rounded
  resident certificate is exactly 2,147,483,648 bytes at `80 x 195 x 1000`;
  the adjacent 2,149,646,336-byte request rejects before payload inspection.
  No operator, case, or validation tolerance changed.

- **r102 (2026-08-19):** first full resident step. Frozen force, cell and dual
  transport, explicit exact-zero source operands, and one r98 P2 now compose
  without an interstage full-grid transfer. A mixed-boundary CPU composition is
  the independent numerical oracle. The exact tier-10 N=8 path measured
  73.8315 ms resident device p95 and 242.727 ms wall p95 in the deliberately
  staged validation wrapper, below the respective 200 ms P3 and 300 ms/step
  one-hour envelopes. The combined certificate is 1,235,662,396 bytes versus
  a 975,303,556-byte observed upper bound. Nonzero source maps and the golden
  eight-slice physics campaign remain subsequent milestones.

- **r103 (2026-08-19):** authoritative production gas-density extraction.
  Golden checkpoint packing exposed that `rho_tot Z` is not gas density. The
  resident source stage now adds the full tuple first, then sums the six gas
  constituent channels in strict record order for P2; carbon aerosol,
  `rho_tot Z`, and enthalpy are excluded. The owner independently checks the
  same sum against the beginning force density, and a non-aliased tuple RED
  rejects the old component-zero mapping. Corrected tier-10 measurements are
  72.6851 ms device p95 and 243.495 ms staged wall p95 with the unchanged
  1,235,662,396-byte certificate. No solver tolerance or checkpoint changed.

- **r104 (2026-08-19):** pressure-open production multigrid stabilization.
  The first immutable golden composition slice proved the old twelve-cycle
  pressure-open schedule missed its unchanged divergence band by `42.663x`;
  this was a coarse-mode oscillation, not the r99 fp32 floor. Undamped fixed
  stops bottomed at `0.1599879861 s^-1` on cycle four and then worsened.
  Pressure-open hierarchies now use a binary32 `0.75f` coarse-correction factor
  and sixteen fixed cycles, reaching `0.0051319599 s^-1` on that slice.
  Periodic/wall hierarchies remain twelve-cycle and undamped because global
  damping degraded their independent oracle ratio.

  All eight step-3480--3487 beginnings are SHA-bound. Their exact production
  residual trace is pinned and plateaus between `0.0049898624` and
  `0.0051319599 s^-1`, with zero monitored misses and no creep. The full
  periodic/wall/open projection suite remains green. No tolerance, checkpoint,
  timestep response, extra projection, or runtime convergence branch was
  introduced; physics-class transport calibration is still a subsequent gate.
  The corrected composed allocation observer reports `1,233,957,860` bytes
  below its `1,235,662,396`-byte certificate. Device time remains `73.0817 ms`;
  the full-state oracle wrapper measured `303.829 ms`, then `402.264 ms` in a
  cold/contended review run. r105 therefore removes that wrapper's accidental
  wall acceptance cap: only its timing is reported. The separate persistent-
  driver `300 ms/step` production wall contract remains explicitly unclosed
  rather than being proxied or widened by a staging-heavy comparator.

- **r105 (2026-08-19):** resident composition evidence and calibration stop.
  The full owner now rejects the combined two-GiB peak before payload access,
  preflights the complete force request before Metal work, counts its own live
  allocations alongside every child, and rejects nonfinite or nonpositive
  terminal state before atomic publication. The eight SHA-bound golden slices
  execute with one projection, zero interstage transfers, zero monitored
  validation misses, and no divergence creep. Independent post residuals stay
  in `0.00498755--0.00514045 s^-1`; matched production/oracle pre residuals
  prove the production reduction factor is `14.37--14.75x` weaker than the
  oracle's, but still inside the unchanged production band. Fixed plume/pilot
  probes retain exact nonzero contrast with no overshoot. Physics acceptance
  is intentionally blocked: scalar, velocity, and ledger differences exceed
  the available slice-matched `1.25 Delta_dt` terms by `83.51x`, `27.73x`, and
  `8.67x`,
  while the required tier-6 adjacent-grid and analytic `B_fp32` evidence is
  absent. The fixture accumulates all eight deviations and fails at validation
  time; it does not tune a tolerance from production output.

- **r106 (2026-08-19):** calibration protocol frozen before new evidence.
  Short-horizon acceptance now uses the outward triangle sum of production
  spatial/time Richardson distances, oracle spatial/time Richardson distances,
  and an analytic production `B_fp32`; the former max-of-variability rule is
  retained only for the long-horizon statistical/integral class. Tiers 5/6/7
  and `dt,dt/2,dt/4` are mandatory so each solver/metric measures its own order,
  capped by formal order, on a centered physical-overlap support. Component
  scalar `L1`, velocity `L2`, and physical ledgers are dimensioned separately;
  steep-front `L_inf` remains a structural envelope gate rather than a
  Richardson observable. The signed
  continuum estimates must also be mutually compatible.

  Fresh case-bound tier states are taken at exactly `0.32 s`; this is fixed
  before the campaign because it contains both pilot-ring and plume-edge
  gradients without borrowing a production observation.

  A separately hashed input manifest owns state, `S_div`, source, boundary,
  topology, metric, compiler, and schedule bytes and forbids result fields.
  The fp64 production mirror is generated from the strict production CPU
  bodies and keeps five cell, fifteen dual, N-nu frozen-force, source, and one
  fixed sixteen-cycle projection topology. `B_fp32` comes from the independent
  operation-graph recurrence `B' = nextUp(kappa B + beta)` with `u=2^-24`;
  fp32-versus-fp64 evidence may only confirm it. Tier-10 receives separately
  instantiated bounds. The r89/r90 operation-trace language was a requirement,
  not an implemented artifact; r106 makes its absence an explicit gate and
  separates a no-Metal derivation process from immutable Metal confirmation.
  Scalar channels and ledgers remain dimensioned and
  separate. The old eight restartable taps are explicitly local-slice evidence;
  a short trajectory chains each solver's own state for at most eight steps,
  while long chaotic horizons use only V/empirical/prefix statistical and
  integral gates.

- **r107 (2026-08-19, superseded as a gate by r108):** the strict binary64,
  exact-common-support beginning-state manifest is
  `338d7c66ee83c1c43e8f12d311389261af320335b70476203c42ff85dc66c3f5`
  and explicitly is not the full campaign manifest. The manifest and its
  three bound checkpoints are preserved under
  `rendered/fire_production_calibration/r107_state_family/`; obsolete v1/v2
  artifacts are excluded. It binds certified tier-5/6/7 state hashes
  `ce0b47fe...`, `7e53de9f...`, and `3f9f1eaf...`.
  On the one mutual physical support, the admissible ratio for `0<p<=2` is
  `[1.18274896,1.65846154]`. `rho_total_Z` and `CH4` yield `1.1396311811`
  and `1.1647909263`; `CO2` and `H2O` both yield `0.9498604709`, with the
  tier-6/7 difference larger than tier-5/6. O2/N2/enthalpy are capped at
  formal `p=2`; near-zero CO/C(gr) are unidentifiable. Consequently the
  beginning-state family is not asymptotic. Fresh review correctly found that
  this is diagnostic rather than dispositive: r106 applies Richardson to each
  solver's evolved `U5/U6/U7`, and did not freeze a pre-solver admission gate.
  The old exit-145 stop was withdrawn before any tolerance was certified.

- **r108 (2026-08-19):** the corrected evolved-output gate legitimately stops
  calibration. A dedicated strict executable whose link excludes all four
  production Metal objects and `Metal.framework` advances the exact tier-5/6/7 certified
  states through four fixed `0.0005 s` source-free steps and seals every
  resulting `S_div` byte in manifest `a2bb4c834a...1b20e2`; the comparison
  process reruns and byte-verifies those targets before inspecting outputs.
  On the common support, evolved oracle `rho_total_Z` and `CH4` ratios are
  `1.1395988915` and `1.1648221780`, below the positive-order floor
  `1.1827489635`; CO2 and H2O ratios are `0.9480801338`, so their fine-pair
  differences grow. Thus four required oracle spatial addends are undefined.
  Production refinement, `B_fp32`, Metal confirmation, velocity/ledger terms,
  and eight-slice readmission do not run. Exit `162` is the monitored stop;
  no solver, tolerance, or ceiling changed, and the checkpoint remains
  `1b944176...`.

- **r109 protocol freeze (2026-08-19, before evidence):** r108 is retained but
  its adjacent-tier instrument is retired. The ratios `1.2` and `7/6` compress
  positive orders `0<p<=2` into the fragile difference-ratio interval
  `[1.18274896,1.65846154]`, while the old observable was exposed to front
  placement and a `0.32--0.322 s` evolved trajectory. The replacement owns
  exact dyadic pairs `{5,10}` and `{6,12}` on one `4D* x 4D* x 6D*` periodic
  analytic smooth state, eight steps to `t_ft/64`, and a fixed tensor cubic
  B-spline mollifier with scale `h5` and support radius `2h5`. It consumes the
  already verified V2 minimum order `p=1.8`; independent extrapolated-limit
  balls must overlap and refinement must approach the other pair's limit.
  The tier-6 distance is the outward maximum of the direct and rescaled
  estimates. These choices, including exact polynomial cell integration and
  a tier-5 observation lattice, are frozen before any r109 solver output.
  Failure of any required filtered channel stops the campaign; success resumes
  production refinement, analytic `B_fp32`, Metal confirmation, and the
  eight-slice gate. The tier-10 checkpoint is not an input to this instrument
  and remains byte-untouched.

- **r109 instrument rejection / r110 freeze (2026-08-19):** r109's dyadic,
  short-horizon, mollified run made the instrument defect explicit. The five
  excited channels passed independent-limit overlap and cross-pair approach;
  CO2, H2O, CO, and `C(gr)` failed only at `1e-21` because the convex
  ambient/injected state gave them no physical signal. This is not admitted as
  an oracle finding. r110 freezes strictly positive smooth mass fractions for
  all seven species, with N2 the exact remainder, while retaining every other
  r109 choice and the already committed decision rule. The under-excited r109
  artifacts remain diagnostic-only; no tolerance or solver changes.

- **r110 structural rejection / r111 freeze (2026-08-19):** r110's directly
  authored product fractions correctly failed the certified elemental-affine
  input gate before the first advance. r111 constructs the fully excited state
  only from affine-admissible directions: the ambient/injected mixture line,
  `0.2` limiting primary reaction, then small reverse CO- and soot-oxidation
  partitions using record molecular weights. No solver evidence exists for
  r110. The dyadic pairs, short horizon, mollifier, `p=1.8`, and independent
  limit-ball rule remain frozen and unchanged.

- **r111 structural rejection / r112 freeze (2026-08-19):** the affine-valid
  smooth state has nonzero mean thermodynamic expansion and was correctly
  rejected by the periodic projection compatibility gate before output. r112
  switches only to six pressure-open sides and canonical open-MAC endpoints.
  The fixed B-spline is evaluated on the tier-5 interior indices two cells in
  from every side, so its complete `2h5` support never consumes a ghost or
  wrap. No r111 solver evidence exists; all numerical acceptance rules remain
  unchanged.

- **r112 oracle acceptance / r113 production stop / r114 completeness repair
  (2026-08-19):** the sealed dyadic open-boundary instrument is now asymptotic
  in all nine filtered conservative channels. Its protocol and target manifests are respectively
  `42185c882c52...15a4ed` and `d4947cb8eedb...e958b`; both independent
  limit-ball and cross-pair-approach tests pass. Representative tier-pair
  distances are `2.7470874096e-5 / 1.9110121791e-5` for `rho_total_Z`,
  `4.0231720361e-5 / 2.7979909266e-5` for O2, and
  `27.2136975348 / 19.0562311507` for sensible enthalpy. This accepts the
  redesigned oracle instrument and retires r108's adjacent-tier refusal as an
  instrument-design result.

  Fresh milestone review found that r113 had written but not replay-checked the
  four analytic-state digests and had omitted the already-contracted velocity
  and inventory observable classes. Before those values were inspected, r114
  sealed supplemental metric manifest `86369b69d37f...9ba09`: arithmetic
  MAC-to-cell velocity followed by the fixed physical B-spline and vector RMS
  L2, plus Kahan-reduced final component inventory per physical volume. The
  complete no-Metal rerun SHA-checks every reconstructed analytic state. Its
  velocity pair is `0.00396317808623 / 0.00326416521728` with limit difference
  `0.000407222051929`; every one of the nine inventory channels also passes
  the independent-limit-ball and cross-pair-approach rules. All 57 scalar,
  velocity, and inventory evidence values are exact-bound.

  The first strict-fp32 resident production step then exposed two production
  integration defects before a production Richardson term could be formed.
  First, the make path had omitted the source-local strict-FP rule for
  `FireProductionAdvectionMac.mm`; `-ffast-math` changed the host packed-gas
  ownership sum by one ULP. The make rule and both Xcode source phases now own
  `-fno-fast-math -ffp-contract=off`; a path-bound RED covers the shipping Opto
  entries as well. With that repaired, tier-5 step zero returns a finite,
  positive conservative payload whose exact digest is
  `03faf5aad21e...64e50`, but its certified affine row 2 has maximum scaled
  residual `5.2451771873310863e-8` at cell 4915. This is only `0.879995`
  binary32 ULP, or `0.4399973532 epsilon32` under r60's
  `numeric_limits<float>::epsilon()` convention, yet it is `57,671.33x` the existing fp64 accepted-
  state envelope, so temperature inversion and the next molecular-viscosity
  request structurally reject. Exit `190` is reachable only after the sole
  projection passes its validation contract and its exact pre/post residual,
  open-complementarity, and removed-mean bytes match the recorded first-step
  evidence (`0.1860706061`, `6.8208464654e-7`, `0.01105802413`, `+0`).
  Exit `190` is the exact monitored r113 stop.
  No nullspace projection, tolerance widening, solver change, or post-hoc
  continuation was introduced. Production refinement, analytic `B_fp32`,
  Metal confirmation, eight-slice readmission, and thermo/source maps remain
  unrun pending an owner ruling that makes fp32 resident output a chainable
  thermochemical state.

- **r115 precision-class feasibility / r116 EOS stop (2026-08-20):** r60's
  single accumulation-scaled accepted-state predicate now selects its unit
  roundoff from producer metadata. Binary64 retains the immutable record's
  `kappa64=4096`. The resident binary32 producer union is remap `256`, composed
  force `64` (subsuming r93's `8`), and projection `256` eps32 units: union
  `576`, rounded by the r60 rule to `kappa32=1024`. Source is exact +0 at this
  pre-thermo seam; nonzero or negative-zero Binary32 source application is
  structurally rejected until thermo/source maps derive their producer term.
  The former cell-4915 observation is `0.8799947063` ULP,
  equivalently `0.4399973532 epsilon32` under r60's convention, and
  `2327.2867x` inside the derived unit-scale envelope; an above-envelope RED
  still fails closed. Temperature, EOS, positive-part divergence/source-
  expansion and radiation properties, and molecular viscosity all consume the
  state's precision metadata. No state repair,
  resident promotion, or fp64 widening is permitted, and the feasibility
  envelope remains separate from the future accuracy term `B_fp32`. Run
  checkpoint v9 persists one homogeneous producer precision across the grid,
  rejects mixed-class publication, and restores that class into the resumed
  owner; historical v5--v8 checkpoints decode as binary64, leaving the
  immutable v8 golden artifact byte-untouched.

  Exit `190` is retired by the derived ruling. The production campaign chains
  31 additional states before exact monitored exit `191`: tier 12 step 7 cell
  2256 has finite reconstructed temperature `348.53712185868289 K`, total
  viscosity evaluation, and EOS residual `0.0011434014099940271`, exceeding
  the unchanged `0.001` gate. Payload digest is `e5a8cdfd...c5b70`; maximum
  affine excursion `2.1925594524305645e-7` is inside the fp32 envelope. This is
  a new EOS-consistency finding, not a feasibility or `B_fp32` term. The
  four-tier campaign is incomplete and admits no production Richardson distance
  pending targeted instrumentation.

- **r117 EOS-drift probe protocol (2026-08-20, frozen before evidence):** the
  exit-191 campaign is instrumented without changing the solver, divergence
  target, EOS ceiling, or accepted-state gate.  On the tier-12 trajectory the
  signed observable is `d_n = V(Q_n)-1`; all eight transitions (campaign
  indices 0--7, with index 7 the failure) record the fixed failing cell 2256
  and the independently searched field maximum before and after the resident
  step.  Monotone near-linear growth classifies secular accumulation;
  a fast nonzero plateau classifies per-step generation against finite drain.
  The target audit is structural: an opaque pre-extracted oracle target is not
  credited as production restoration unless the production request itself
  contains `(V(Q_n)-1)/dt`, and no r70 counterpart is credited unless the
  target is recomputed from the production transported candidate.

  Drain is measured by one counterfactual, not fitted. At the end of tier-12
  campaign step 6 (displayed transition 7), an otherwise byte-identical resident call adds the strict-fp32
  absolute-reference term `float(d_n/float(dt))` to each authored target.  Its
  conservative output must be byte-identical to baseline because projection is
  terminal; only its projected MAC state may differ.  That MAC state drives one
  ordinary campaign-step-7 transport (the failing displayed transition 8) with
  the unchanged sealed target. At cell 2256, `G=d_8-d_7`,
  `drain=d_8-d_8^restored`, `r=drain/d_6`, and the diagnosed steady value is
  `G/r`. Nonfinite values,
  zero reference/drain, a failed projection, an interstage transfer, or changed
  step-6 conservative bytes invalidate the probe.  This is diagnostic evidence
  only: it cannot relax the `1.0e-3` gate or authorize a fix.  The golden
  checkpoint remains outside this campaign and must retain SHA-256
  `1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`.

  If the full-gain step-6 projection itself misses validation, the primary
  implementation-qualification probe is invalid and no restoration fix may
  land. A secondary capacity readout may nevertheless carry that provisional
  MAC field through exactly one step-7 transport to evaluate the same frozen
  `G/r` formula. It must label both projection-valid bits and retain the
  residency/byte-identity checks. Those numbers describe fixed-schedule drain
  capacity only; they cannot be reclassified as accepted solver evidence.

  Before implementation, a separate eight-step shadow starts from the same
  immutable analytic tier-12 state and adds that identical absolute-reference
  term at every step. It records the signed probe and field maximum, projection
  validation and residual, and residency count after each call. The shadow is
  viable only if all eight projections validate, every published state remains
  consumer-total, and its measured plateau agrees with the independently
  defined `G/r` value while staying below `1.0e-3`. A provisional shadow may be
  continued after a validation miss for diagnosis, but cannot authorize a fix.

  Evidence: the baseline probe grows monotonically
  `1.62435e-4 -> 2.90527e-4 -> 4.22101e-4 -> 5.57589e-4 -> 6.97411e-4 ->
  8.41544e-4 -> 9.90210e-4 -> 1.14340e-3`; cell 2256 becomes the field maximum
  on transition 3. The target audit finds the known hole reborn: the production
  request copies the sealed oracle target and has neither its own r69 absolute
  term nor an r70 production-candidate closure. The originally recorded
  one-shot `G/r` used transition 7 and is retired. The corrected transition-8
  capacity-only values are `G=1.5319163029481331e-4`,
  `r=1.0030530061213276`, and `G/r=1.5272535883939468e-4`. Both one-off
  projection-valid bits are zero, with exact post residuals `0x1.ac2p-12` and
  `0x1.acbf72p-12`. The eight-step shadow likewise validates only 3/8 calls.
  Restoration therefore predicts a state-space plateau well below the EOS
  ceiling, but exceeds the current fixed projection schedule's admissible
  target capacity. Gain, a manifold-residual-only second projection, and a new
  derived ceiling remain explicit owner-level alternatives. No solver or gate
  changed; production Richardson remains stopped. Durable evidence/plot hashes
  are `ea7943ef...848c` and `c8a2677d...a1a0`.

  Replay is an independently binding outcome. Exact
  `RISE_FIRE_EOS_DRIFT_PROBE=1` returns `215` only after the frozen arrays,
  corrected failing-transition capacity values, and both one-off
  projection-valid bits match. A malformed nonempty activation returns `216`;
  the ordinary uninstrumented campaign retains exit `191`.

- **r118 manifold-restoration protocol (2026-08-20, frozen before long-run
  evidence):** the owner ruling selects a second resident projection and
  rejects both restoration gain and a derived EOS ceiling. The first pass owns
  the sealed physical target and its unchanged `0.005 U/L` validation. Its
  Private momentum is borrowed without staging by a correction-only second
  pass whose sole authored divergence increment is
  `float((V(Q^n)-1)/float(dt))`; the second pass owns the independent criterion
  `max|Delta div-R| <= 0.005 max|R|`, homogeneous open pressure correction, and
  an identity-bound Private target. Deadbeat `gamma=1` is structural. A fitted
  gain and a widened EOS ceiling are both rejected because an observed
  gamma-window contraction does not transfer across density, boundary, and
  target regimes. r70's advective-anomaly closure remains intentionally absent:
  r117's `G/r=1.5272535883939468e-4` is 15.2725% of the unchanged ceiling, a
  6.55-fold margin, and is the prediction to test rather than a new constant.

  A schedule admission probe showed that the new restored successor is a real
  new main-pass regime: 16 physical V-cycles leave about `4.09e-4 s^-1` and
  miss the existing band, while the first deterministic extension, 17 cycles,
  passes it; restoration retains 16 cycles under its own scale. No tolerance
  moved. The predeclared evidence run is 104 tier-12 resident steps, thirteen
  repetitions of the sealed eight-target phase, with exact +0 sources. It pins
  the maximum absolute cell-2256 deviation over the final 32 steps, the
  final-32 field maximum, all 208 validation bits/residuals, envelope and
  payload digests, zero full-grid interstage transfers, allocation maxima, and
  device/wall p95 over the final 96 calls. The old eight-value blowthrough is
  the removal RED; physical-target substitution is the mis-wiring RED. The
  binary32 feasibility union becomes `256+64+2*256=832 epsilon32`, still
  rounding to `kappa32=1024`. The golden checkpoint remains outside this
  controlled capacity trajectory and byte-untouched. Production Richardson,
  `B_fp32`, Metal confirmation, eight-slice readmission, and thermo/source maps
  remain sequenced after this gate.

- **r118 restoration evidence:** the predeclared 104-step campaign validates
  both projection passes on every step and observes zero full-grid interstage
  transfers. Cell 2256 has final-32 maximum absolute deviation
  `1.5435381821271577e-4`, within 1.0663% of the independent r117 `G/r`
  prediction and 6.4786 times below the unchanged ceiling. The independently
  searched field maximum is `6.524281258450948e-4`. The exact diagnostic and
  payload trace digest is `2b7071e6...2fc68`; final state is
  `b3e17108...dc76e`. Allocation is 229,518,420 observed-upper bytes against
  248,479,780 certified bytes. After eight warm-ups, 96 calls measure 27.8510
  ms device p95 and 71.2383 ms wall p95 against the 200 ms step allocation.
  The durable evidence artifact hashes to `adaa3fc4...fa5b7`. The architecture
  is admitted; the sequence resumes at production Richardson without changing
  the EOS ceiling or introducing r70 closure.

- **r119 production dyadic spatial evidence (2026-08-20):** after r118, the
  sealed eight-step resident campaign completes at tiers 5, 10, 6, and 12.
  The predeclared fixed-physical B-spline observable and verified order 1.8
  accept all nine scalar channels, filtered MAC velocity, and all nine
  inventory channels: both independent dyadic limit balls overlap and both
  fine solutions approach the opposite pair's extrapolated limit. Exact
  `D5_10`, `D6_12`, and limit-difference values are source-bound and preserved
  in `r119_production_spatial/production_spatial_evidence.v1` (SHA-256
  `10229d0f...345f4`). This defines
  only `E_h,prod`; temporal refinement and analytic `B_fp32` remain separate,
  subsequent gates.

- **r120 two-projection `B_fp32` protocol (2026-08-20, frozen before
  evidence):** r106's trace/topology derivation is extended to the exact r118
  stage graph: force substeps, 5 cell maps, 15 dual maps, source addition,
  physical P2(17), and correction-only restoration P2(16). The two projections
  own separate local terms. A no-Metal derivation must reconcile an arithmetic
  trace with an independent topology walker and reject unresolved branch
  margins or `n*u>=1`; only then may a separate Metal process compare identical
  fp32 request bytes with the generated fp64 same-scheme mirror. Bounds reduce
  directly into the r112/r119 observable norms and are instantiated separately
  for every golden slice. No measurement, feasibility factor, or tier-6 value
  may select or stand in for an analytic radius.

- **r121 burning-state restoration prediction (2026-08-20, frozen before
  source evidence):** the first nonzero-source capacity campaign is bound to
  the untouched tier-10 step-3480 checkpoint (`1b944176...4947`). It measures
  a source-only EOS generation increment `G_src` against an otherwise identical
  exact-`+0` call and a burning-state self-donor drain `r_burn`; the predicted
  plateau is `(G_0+G_src)/r_burn`, with signed cell values retained until the
  final maximum. The future 104-step frozen-source diagnostic must agree with
  that prediction, validate both projections, transfer no interstage grids,
  and remain below `1.0e-3`. The existing `G_0` and `r_0` are calibration
  evidence, not substitutes for the burning-state measurements.

- **r122 independent roundoff derivation refusal (2026-08-20):** the no-Metal
  r120 trace executes all 24 frozen stages and binds exact diagnostic digest
  `8f3e709a...bf93e`, unresolved-branch bitmap `0xdffffe`, and
  invalid-denominator bitmap `0x1ffffe`. Force and exact-`+0` source addition
  are resolved; the 5 cell maps, 15 dual maps, and both projection passes have
  unresolved executed branch topology, and every transport map has an interval
  denominator whose lower enclosure crosses zero. Independently, the fail-fast
  walker repacks the public SoA state and reconstructs the first x-half-step PPM
  edge DAG: line 1/cell 6/component 3 reaches `quadratic!=0` with
  `-7.7486038219110043e-7 +/- 1.2337798327030971e-6` against exact zero. A
  half-radius undercount mutant separates the intervals. Per the predeclared
  r120 rule, this independent executed-branch witness stops the campaign
  before Metal, before fp32/fp64 measurement, and before any `B_fp32` radius is
  admitted. Temporal refinement, eight-slice readmission, and source maps did
  not run. The durable artifact is
  `r122_roundoff_derivation/roundoff_derivation_stop.v1`, SHA-256
  `939e95f8...b5d5ac`. Continuation requires an explicit branch-stable or
  branch-equivalence ruling; no measurement may widen the absent bound.

- **r123 branch-obligation ruling (2026-08-20, frozen before discharge):**
  r120 is amended only to replace blanket zero-crossing rejection with the r59
  direct-certificate principle. Every ambiguous executed comparison emits an
  identity-bound obligation. It closes through either a proof that both paths
  coincide on the switching surface plus an outward Lipschitz/divergence hull
  over the complete ambiguity width, or a recorded branch-stable reformulation.
  Non-equivalent sites must be reformulated; empirical branch selection,
  tolerance tuning, and measurement-derived bounds remain prohibited. Any
  undischarged obligation keeps exit `237`. The first owner is PPM's
  `quadratic!=0`: exact zero is the linear-polynomial endpoint case, while the
  nonzero stationary-point path must be enveloped over the r122
  `1.2337798327030971e-6` radius or reformulated. A successful full resident
  walk must publish the complete obligation census before `B_fp32`, Metal,
  temporal refinement, eight-slice readmission, or source maps may run.

- **r123 branch-obligation evidence / shared-limiter stop (2026-08-20):** the
  independent PPM identity is `P_q(s)-P_0(s)=-q s(1-s)`, so the r122 ambiguity
  width `2.008640214894198e-6` closes with the outward `|q|/4` term
  `5.021600537235496e-7`; separately rounded coefficient/stationary/Horner work
  adds an independent `gamma_4`/underflow residual
  `2.6783670818887366e-6`, for total `3.1805271356122864e-6`. No observed
  successor enters the proof. All 1,364 quadratic, 3,785 lower-stationary, and
  2,619 upper-stationary dynamic obligations close, including stationary
  predicates beneath resolved nonzero quadratic parents; every executed
  continuous min/max selector is also ordinal-counted and hulled or explicitly
  owned by its parent certificate. The completed 24-stage census records
  4,161,080 dynamic obligations (468,869 discharged; 3,692,211 pending). It
  also records conditional division/domain obligations in all twenty transport
  stage (`invalid_bitmap=0x1ffffe`); these are pending rather than admitted as
  bounds. It then stops at the first site proved non-equivalent and therefore
  requiring the mandated reformulation fallback:
  first non-equivalent shared-tuple limiter case: stage 7 / executed-comparison
  ordinal 5,957 has predicate
  `-4.1921933491284591e-6 +/- 5.7555189193234835e-6`, zero available
  numerator versus required `10.986253083780237`, and successor alpha bytes
  `1` versus `0`. Strict component monotonicity, maximal inactive behavior, and
  a single common alpha are not jointly continuous at zero headroom. A local
  flat-stencil rewrite was rejected because the executed obstruction is
  non-plateau; resolving it requires an explicit limiter-semantics choice, not
  a tolerance. Exit `237` remains fail-closed; `B_fp32` and later campaigns did
  not run. The durable evidence SHA is `2743b634...cb3971`; golden checkpoint
  `1b944176...4947` is untouched.

- **r124 continuous shared-alpha ruling (2026-08-21, frozen before production
  evidence):** r59's admissible-interval theorem decides the invariant triage
  at r123's non-equivalent limiter switch.  Shared tuple alpha is retained
  because it is the section 3.7 conservation coupling; strict component
  monotonicity is retained because it is physical admissibility; only maximal
  inactivity, an optimality property, yields inside the certified ambiguity
  neighbourhood.  Per-component alpha, relaxed monotonicity, and a binary-site
  certification carve-out are rejected.  On the frozen r123 graph the largest
  outward limiter-predicate widths are `9292.2824737527444 u32*s` (positive)
  and `8645.4622206683161 u32*s` (negative), where
  `s=max(FLT_MIN,abs(center),abs(envelope),abs(signed deviation))` and
  `u32=2^-24`.  Rounding the union upward to the next power of two fixes
  `w=2^14*u32*s=2^-10*s`.  The pre-registered cap is one below `-w`, a
  continuous `(Q+max(-d,0))/w` ramp inside `[-w,w]`, and the legacy `Q/d`
  cap above `+w`, each clamped to one.  Both joins and the zero surface are
  continuous, `c*d<=Q` for positive `d`, and legacy bytes are required outside
  the width.  The independent walker, not production measurement, must prove
  every limiter enclosure fits its local width and must discharge all new join
  obligations.  The full remaining census is still blocked until every source
  site class has one reusable equivalence certificate or a recorded continuous
  reformulation; exit `237` remains the incomplete-proof result.

- **r124 continuous transition evidence / site-class proof stop
  (2026-08-21):** the compact shared-alpha transition retains common tuple
  coupling and strict monotonicity, yields only maximal inactivity inside
  `w=2^-10*s`, and is byte-identical to the legacy limiter outside that width.
  The half-width mutation fails. The affected r118/r119 campaigns were rerun:
  restoration plateau `1.5452375598545842e-4`, field plateau
  `6.5195550111418754e-4`, and all production dyadic scalar/velocity/inventory
  classifications remain accepted; their durable evidence hashes are now
  `7fa0872d...3696d8` and `201b67b7...8d2c6`.

  Fresh boundary review then invalidated the attempted generic bulk discharge:
  assigning the same `4*M*ambiguity` expression to six branch classes did not
  independently walk their alternate paths. That certificate was removed.
  Of `4,340,821` obligations, the existing independent PPM/selection proofs
  discharge `652,411` and `3,688,410` remain pending (floor `373,245`, flat
  integral `620,491`, remaining `1,623,168`, Courant `346,080`, fraction
  `371,760`, inflow `353,664`, projection guards `2`). Invalid bitmap is zero;
  unresolved bitmap is `0xdffffe`. Exit `237` remains fail-closed before
  `B_fp32` or Metal measurement. This correction is itself the review-loop
  evidence: the operator landed, but topology certification did not get
  promoted on a tautological proof. The golden checkpoint remains untouched.

- **r125 floor-partition class discharge (2026-08-21):** the first class in
  the seven-class campaign uses an independent two-path proof rather than a
  site label.  Fractional-tail and whole-cell partitions integrate the same
  PPM polynomial at an integer boundary; the outward exact term is
  `2 M delta`.  The separately counted alternate realization contributes
  `gamma_(32+24N) M (N+4)`, with `(32+24N) FLT_MIN` added for FTZ.  Mutants
  halving ambiguity, deleting the rounded term, or omitting the full-cycle
  topology all RED.  All `373,245` floor obligations close, moving the census
  from `652,411 / 4,340,821` to `1,025,656 / 4,340,821`; `3,315,165` remain
  and exact exit `237` is retained.  The projection graph's finite but very
  loose maximum floor envelope (`4.486687686924483e302`) is recorded honestly:
  topology discharge does not pre-approve an eventual validation-contract
  `B_fp32`.

- **r126 remaining-positive class discharge (2026-08-21):** the loop body
  integrates a zero-width PPM slice at `remaining=0`; across ambiguity
  `delta`, the exact term is `M delta`.  The independent forty-operation body
  supplies `gamma_40 M (4+delta)` plus `40 FLT_MIN`.  Width-halving,
  missing-cell-body, and missing-FTZ mutants RED.  All `1,623,168` instances
  close, reducing pending obligations from `3,315,165` to `1,691,997` while
  exact exit `237` remains in force.  The largest class envelope is
  `0.49575328199529184`.

- **r127 inflow-sign continuous reformulation (2026-08-21):** the old
  pressure-open donor switch is non-equivalent at zero, so the r59/r123/r124
  invariant triage applies rather than an asserted equivalence. The donor is a
  convex nearest-to-ambient transition over
  `w=u32*max(FLT_MIN,abs(v),abs(dx/dt))`; monotone donor admissibility is
  retained and only binary donor optimality yields inside the derived width.
  The frozen ambiguity is `1.7632415612658968e-38`, or
  `1.3426012991064678e-32` units of `u32*s`, so the pre-registered upward
  power-of-two factor is the minimum stable value one. The independent
  envelope separates the exact donor-contrast Lipschitz term, a twelve-op
  rounded term, and twelve FTZ terms. Half-width, missing-contrast,
  missing-rounded, and binary-branch mutants RED; CPU/MSL topology and an
  interior full-remap byte fixture bind both implementations. Removing the
  old branch topology eliminates `353,664` pending obligations. The new
  census is `2,633,990 / 3,972,323`, leaving `1,338,333` pending and exact
  exit `237`. The r118 probe/field plateaus re-pin within noise to
  `1.5439012582030287e-4` / `6.5237316812827295e-4`, and all r119 limit balls
  remain accepted. Their durable hashes are `2752dc20...af451` and
  `0c481de8...eb8de`; class evidence is `5d95d4d0...fba198`. The golden
  checkpoint is untouched.

- **r128 Courant-orientation class discharge (2026-08-21):** both swept
  orientations vanish at Courant zero and their local donor slopes differ by
  at most `2M`, giving exact term `2 M delta`. The independently enumerated
  longer successor has 48 scalar operations, so the rounded/FTZ terms are
  `gamma_48 M (8+delta)` and `48 FLT_MIN`. The `>=` topology canonicalizes
  both signed zeros onto the positive path. Half-ambiguity,
  missing-negative-path, missing-FTZ, and signed-zero mutants RED. All
  `346,080` instances close; the census is now
  `2,980,070 / 3,972,323`, with `992,253` pending and exact exit `237`.
  Maximum class envelope is `1.1830324528164238`; durable evidence is
  `482b58d0...925e7e`. No production arithmetic or golden checkpoint changed.

- **r129 fractional-tail class discharge (2026-08-21):** the optional
  trailing PPM slice has zero measure at fraction zero, giving exact term
  `M delta`. Its independently enumerated 24-operation polynomial/accumulation
  path contributes `gamma_24 M (4+delta)` and `24 FLT_MIN`. Half-ambiguity,
  missing-tail, and missing-FTZ mutants RED. All `371,760` instances close;
  `3,351,830 / 3,972,323` are discharged and `620,493` remain. Exact exit
  `237` and the block on B_fp32 remain. Maximum class envelope is
  `0.29684226235298722`; durable evidence is `7ed9b0be...6c6b1db`.

- **r130 flat-integral class discharge (2026-08-21):** the curved PPM
  integral and flat shortcut coincide at a flat profile. Direct coefficient
  collection gives exact divergence `8D` for endpoint deviation `D`; the
  independent 32-operation curved path adds `gamma_32 M (8+D)` and
  `32 FLT_MIN`. Half-deviation, missing-polynomial, missing-FTZ,
  discontinuous-shortcut, and quadratic-coefficient/source-topology mutants
  RED. All `620,491` instances close;
  `3,972,321 / 3,972,323` are discharged and only two projection-reduction
  guards remain. Exact exit `237` stays fail-closed. The maximum class
  envelope `128175.45885830303` is not promoted to `B_fp32`; durable evidence
  is `1468fed5...db608`.

- **r131 projection-reduction proof and branch-campaign closure
  (2026-08-21):** the physical and restoration residual maxima are both
  positive-zero-seeded `max(abs(residual))` reductions. The independent graph
  proof makes the guard's negative alternate unreachable, so exact, rounded,
  and FTZ divergence are each zero. Signed-leaf, negative-seed, subtractive,
  and false raw-provenance mutants RED; only abs/max may propagate the tag.
  Source gates bind the actual seed/reduction/two consumers.
  The last two obligations close: `3,972,323 / 3,972,323`, pending zero,
  unresolved/invalid bitmaps `0x000000`. Exit `240` replaces incomplete-proof
  exit `237`; `B_fp32` is unlocked but remains undefined. Durable evidence is
  `d6cdacdb...f7c8a4`. No production or golden-checkpoint bytes changed.

- **r132 fixed-grid projection interpolation proof (2026-08-21):** the first
  composed-bound attempt caught a proof-instrument error before measurement.
  Projection prolongation floors depend only on exact integer grid geometry,
  but the generic transport certificate had multiplied them by the pressure
  enclosure, creating a false `4.486687686924483e302` branch term.  The
  independent walker now evaluates `((2*i+1)*Nc-Nf)/(2*Nf)` exactly and checks
  its boundary side against the sequential binary32 expression.  The tier-6
  hierarchy contains 45 exact boundaries per cycle: 765 for the 17-cycle
  physical solve and 720 for the 16-cycle restoration solve.  All certify with
  zero branch divergence; coordinate and reassociation mutants RED.  Both
  projection output radii become `1.352840804874779e-7`; the zero-pending
  `3,972,323 / 3,972,323` census and exit `240` remain.  This is a topology
  correction, not a `B_fp32` bound.  Durable evidence is `d8df1a96...9198`;
  production and golden-checkpoint bytes are unchanged.

- **r133 metric-level B_fp32 derivation refusal (2026-08-21):** local branch
  envelopes now attach only to the swept integral that owns them, and metric
  reducers retain channel mean/RMS/count evidence.  Canonicalizing NaN radii
  to positive infinity exposed what the former `std::max` summary hid: naive
  dependency intervals through both fixed multigrid schedules are unbounded
  for every velocity face (`21600,21600,21312` per solve), although the actual
  centers and rounded outputs remain finite.  Three obligations re-open—one
  unknown comparison and the two projection-validation predicates—so the
  census is `3,972,323 / 3,972,326`, both projection bits are set in the
  unresolved/invalid maps (`0xc00000`), and exact exit `237` is restored.
  This is a walker/condition-proof defect, not a measured Metal miss; no Metal
  comparison ran and no `B_fp32` term exists.  A finite independent
  per-sweep multigrid amplification derivation is required next.  Durable
  evidence is `1512191c...853e`; production and golden-checkpoint bytes are
  unchanged.

- **r134 projection a-posteriori certificate (2026-08-21):** the r133
  iterative-solve obstruction is resolved by bounding the accepted solution
  from its validated residual rather than by dependency-folding every
  multigrid sweep.  The independent operator proof uses
  `A=G^T rho_f^-1 G`, the rational pressure-open Poincare bound, and the
  certified density interval to obtain `lambda_min>=8.9899266871598034`,
  `||A^-1||<=0.11123561234690459`, and velocity gain
  `0.47780222212961759`.  Fresh review rejected cell-centered terminal
  accumulation and unqualified clearing of the shared invalid-domain bit.
  The repaired proof provenance-counts only multigrid-cycle interval events,
  carries unique-face L2 through the Hodge bound, and independently evaluates
  the rounded pressure in the exact-promoted operator.  That cross defect
  includes coefficient/open-RHS perturbations, while the direct face term
  covers density and boundary-pressure arithmetic; the open active set is
  exact-matched.  A second review rejected borrowing the binary32 validation
  tolerance as the binary64 residual bound and inward rounded density extrema.
  The final walker uses outward `center+-radius` density bounds and solves the
  physical binary64 validation inequality self-consistently; its feedback is
  `0.23426947265986475`, while the restoration feedback is
  `1.5955915929254457e-11`.  A third fresh review rejected the provisional
  gamma-only terminal term because it did not enumerate total-head/open-face
  arithmetic.  The final proof instead walks the complete terminal binary64
  DAG with independent outward intervals on every unique face and retains its
  L2 envelope outside `A^-1`.  The resulting projection-local velocity RMS terms are
  `1.6499365420669603e-4` (physical) and `2.3357153961206769e-4`
  (restoration).  Separate predicate envelopes prove
  residual/tolerance margins `2.6143109675737545e-4` and
  `4.4094270347925889e-4`; the prior unknown velocity guard is discharged by
  nonnegative-reduction provenance.  The census is
  `3,972,326 / 3,972,326`, both bitmaps zero, exact exit `241`.  This is a
  projection-local term, not the composed `B_fp32`, and no Metal/fp64
  measurement has run.  Durable evidence is `18f01152...b651ec2`; production
  and golden-checkpoint bytes are unchanged.

- **r135 full-step analytic B_fp32 candidate (2026-08-22; rejected by r136):** before any Metal
  measurement, the independent walker composes the five cell remaps, source
  publication, force output, fifteen dual submaps, physical projection, and
  restoration projection.  Shared-alpha monotone conservative maps supply the
  structural L1/L2 nonexpansive gains.  Projection composition includes gas-
  density coefficient error, pressure-open total-head forcing, the physical
  output feeding restoration, and both distinct r134 local terms.  The nine
  scalar/inventory bounds are `3.2719950722423746e-4` through
  `5.2181563701838843e2`; final velocity RMS is
  `2.1145425678289562e-2 m/s`.  Missing-cell, missing-density, and missing-
  projection-feedthrough mutants all underbound and RED.  Exact exit `242`
  binds trace `086c6b06...86c284`; durable derivation evidence is
  `843e1f02...9f32c1`.  Metal measurement and all later acceptance decisions
  remain unrun; the golden checkpoint is unchanged.  Fresh r136 review later
  proved that the candidate's propagation assumptions were insufficient, so
  none of these values is admitted to the contract.
- **r136 full-step composition proof refusal (2026-08-22):** the derive-first
  boundary worked: review rejected r135 before Metal.  The exact proof-gap
  bitmap is `0xff`: nonlinear shared-alpha cross-component/compressive gains;
  missing density Linf and variable-coefficient projection resolvent; missing
  localized pressure-open quadratic control; missing source momentum and gas-
  reduction metrics; only slice zero instantiated; and trace-trusted rather
  than shape-derived cardinalities.  Independent counterexamples pin
  compression L2 gain `sqrt(2)`, localized product RMS `2>1`, and nonzero
  shared-alpha cross-component response.  The historical `2.1145425678e-2`
  velocity candidate is explicitly non-certified.  Exact exit `237` and
  durable evidence `19732a38...ff33ad` keep measurement, additive tolerance,
  temporal refinement, and readmission blocked.  No Metal evidence ran; the
  golden checkpoint remains unchanged.
- **r137 subdominance protocol amendment (2026-08-22):** owner ruling replaces
  only a-priori *full-step* composition with an a-posteriori same-scheme gate;
  streaming walker envelopes remain diagnostics.  Before measurement, the
  protocol seals `|P32-P64| <= 2^-3 E_P` independently per slice and quantity,
  with strict binary64 production mirror `P64` and no oracle participation.
  Scheme fidelity is separately `|P64-O| <= E_P+E_O`.  `E_P` is the outward
  maximum of r119's `{5,10}` distance rescaled to tier 6 and its `{6,12}`
  distance at verified order `1.8`.  The velocity values are
  `E_P=5.890459160549289e-3` and `B=7.363073950686612e-4 m/s`; all nine scalar
  and inventory values are sealed in artifact `833137b5...d63922`.  The old
  `3e-5` velocity guard remains preliminary pending explicit supersession with
  both numbers.  Source maps retain exact `+0` until the same mechanism
  certifies their producer term.  Once Metal source maps execute, an honest
  `preview_primary`, `sequence_backed`, uncertified tier-6 animation is
  authorized in parallel with validation.  No measurement ran in this entry;
  golden SHA remains `1b944176...4947`.
- **r138 same-scheme subdominance measurement (2026-08-22):** the strict
  binary64 production mirror and resident Metal binary32 path execute the
  identical two-projection tier-6 step on eight independently restarted,
  SHA-bound slices.  All 152 scalar/velocity/inventory precision gates satisfy
  the pre-registered `|P32-P64| <= 2^-3 E_P` rule.  Maximum velocity delta is
  `1.1165273069908068e-9 m/s` against `7.363073950686612e-4`, a
  `659462.05745125038` margin; minimum scalar and inventory margins are
  `1542.2120195226826` and `2361.071848300599`.  Both projections validate
  `8/8`, the resident seam reports zero interstage transfers, and the oracle
  is absent.  The measurement also fits the old `3e-5 m/s` preliminary guard
  by `26869.024888297707`, but supersession remains pending the golden-slice
  measurement.  Exact exit `243`,
  trace `f90a2508...551cebf`, and durable evidence
  `ffeeaa6e...1e50e0e` bind the tier-6 pilot.  Fresh review later established
  that the repeated analytic beginning does not certify the promised golden
  slice class, so it closes neither `B_fp32` nor Metal confirmation.  Golden
  SHA stays `1b944176...4947`.
- **r139 temporal instrument pre-registration (2026-08-22):** before temporal
  evidence, the tier-6 smooth beginning, pressure-open topology, exact `+0`
  sources, r112 filters, and scalar/velocity/inventory metrics are frozen.
  The baseline request step is the represented binary32
  `0x1.e54eeep-10 s`; exact dyadic halves with `8/16/32` steps share horizon
  `0x1.e54eeep-7 s`, eliminating endpoint drift.  Production uses the strict
  binary64 same-scheme mirror at formal temporal order one; oracle uses the
  certified Heun solver at formal order two.  Each level consumes a separately
  sealed, capability-isolated divergence-target schedule.  Positive decreasing
  differences and the unmodified Richardson formula are mandatory.  No
  temporal evidence or Metal run occurs in this entry; golden SHA remains
  `1b944176...4947`.
- **r138a golden-slice precision input repair (2026-08-22):** fresh review
  rejected r138's tier-6 analytic eight-target pilot as certification of the
  r137 `shared_golden_beginning_per_slice` class.  The pilot remains useful
  diagnostic evidence, but it does not close `B_fp32` and therefore does not
  unlock r139 evidence.  Before inspecting any golden-slice precision result,
  this entry seals the immutable tier-10 root plus all seven r95 continuation
  hashes, the eight represented time steps, mixed open/wall topology, exact
  positive-zero sources, r112 filter/lattice, and the same-scheme P32/P64
  pairing.  The input manifest is the sole durable artifact in
  `r138_golden_subdominance_inputs`; measurement is explicitly false.  Golden
  SHA remains `1b944176...4947`.
- **r140 golden restoration-validation refusal (2026-08-22):** the corrected
  eight-beginning measurement stops on sealed slice zero before admitting a
  precision result.  The 17-cycle physical pass validates
  (`37.872448 -> 0.00374865532 s^-1`); the 16-cycle restoration pass does not:
  `9.97165444e-6` exceeds its independent `1.06855828e-6 s^-1` band by
  `9.3318769993`.  No precision result is admitted after that failed
  prerequisite.  Exact exit `244` is restricted to slice zero and bit-pins
  the physical/restoration diagnostics, cycle topology, and post-run golden
  hash; the ordinary macOS suite replays it.  No criterion, solver, `B_fp32`,
  temporal evidence, or golden bytes changed.  This is an owner-level
  validation-contract decision, not grounds to widen the precision bound.
- **r141 restoration plateau-control protocol (2026-08-22; pre-evidence):**
  owner ruling identifies r118's `0.005*target` restoration band as a design
  derivation error: “derived the same way” was read too literally from the
  physical solve's fractional residual instead of the restoration mechanism's
  plateau function.  Before new Metal evidence, the replacement protocol pins
  EOS ceiling `1e-3`, stability headroom `h=2^-2`, and therefore plateau limit
  `7.5e-4`.  In each separately classified cold/burning regime,
  `G_field=max_cell |d_removed^(n+1)-d_beginning^n|`,
  `r_req=G_field/(1e-3*(1-h))`, and the mechanism band is
  `residual <= (1-r_req)*max|R_n|`.  Identical-input restoration runs for
  cycles 1 through 16 define the per-cycle contraction and the minimum derived
  count if 16 is insufficient; no count or tolerance may be tuned after the
  curve.  The 104-step final-32 field gate `<=7.5e-4` is the load-bearing
  physics oracle.  The protocol artifact records `measurement_performed=false`;
  no production criterion, solver, or golden byte changed in this entry.
- **r142 burning plateau capacity stop (2026-08-22):** the exact r141
  campaign closes the design-level band question by finding that no admissible
  restoration band or cycle count exists for the frozen burning slice.  The
  removed-restoration field generation is `G_field=2.5328069638265172e-3`
  at cell `3227`, versus `1.2031080315666465e-4` in the cold tier-12 slice:
  a `21.052198949485707` regime increase.  With `C(1-h)=7.5e-4`, burning
  requires drain fraction `r_req=3.3770759517686897`; even mathematical
  deadbeat drain `r=1` cannot meet the plateau.  Sixteen cycles deliver
  `r_16=0.9533406144549903` and end at `9.971654435503297e-6`, but additional
  contraction cannot remove generation already larger than the complete
  field allowance.  Cold would require only `0.1604144042088862` and its
  16-cycle drain is `0.99562928290235475`.  Exact exit `253` binds both
  1-through-16 curves, cell/sign witnesses, sweep counts, source identities,
  and the unchanged golden SHA.  The preregistered long shadow is not run:
  its necessary generation bound is already false.  No validation criterion,
  cycle count, `B_fp32`, temporal term, or default production arithmetic
  changed; the only production-source delta is the fail-closed, environment-
  authorized evidence seam.  This is
  the owner-requested architecture-capacity stop, not a residual-floor finding.
  Fresh review found and repaired an initial counterfactual-topology defect:
  the accepted artifact is produced only after the removed-restoration control
  executes the same 17-cycle physical solve as production (`1156` burning and
  `1054` cold sweeps) and matches its pre/post diagnostics on every curve run.
  The retained r138 pilot was rerun after adding the isolated probe seam and
  reproduced exact exit `243` and trace `f90a2508...551cebf`.
- **r143 manifold-timestep protocol (2026-08-22; pre-evidence):** the owner
  resolves r142 in the existing section-3.9 pin-4 remedy class.  The measured
  `21.052198949485707` burning/cold generation ratio is classified as physical
  front contrast: for a fixed regime, manifold generation scales with the
  per-step advective dose, so the timestep—not the EOS ceiling or restoration
  gain—owns capacity.  With `h=2^-2`, an accepted step publishes `(dt,G,r)` and
  the next candidate is
  `dt_manifold=dt*((1-h)*1e-3*r)/G`, entering the same minimum as advective,
  buoyant, explicit-diffusion, and `1.1x` growth limits.  A run's first step
  has no prior manifold observation and therefore uses the CFL-family limits
  alone.  Each new step derives its restoration band from its own measured
  `G`, and independently fails if the realized EOS plateau exceeds `7.5e-4`;
  this catches a regime transition that outruns the one-step predictor.
  r142 predicts `1.589201814710624e-5 s`, `3.542360307075882x` below its
  previous `5.629525428363875e-5 s`, but these numbers remain predictions until
  the Metal campaign.  Restoration gain is rejected because its admissible
  gamma window is regime-dependent; widening the EOS ceiling confuses solver
  capacity with r60 admissibility; per-step repair mutates ledgers; and an r70
  Picard anomaly pass remains only a budget-triggered follow-up.  Artifact
  `ba03dbda...e5b5f0` is pre-measurement and changes no production behavior.
- **r144 manifold-predictor stop (2026-08-22):** the r143 selector is
  implemented and its terminal step-boundary EOS reduction independently
  reproduces the accepted-state volume ratio without a resident full-grid
  interstage transfer.  The frozen r142 observation selects represented
  `dt=1.5892017472651787e-5 s`, `3.5423604574130363x` below the golden CFL
  step.  The preregistered proportional predictor is falsified at the burning
  front: realized `G=2.5081625752932935e-3`, `99.02699302058174%` of the old
  value rather than about `28%`.  The terminal field reaches
  `2.5081625764804549e-3`, `3.3442167686406066x` the `7.5e-4` allowance.
  Restoration drains `0.97489008508207653`, but the required fraction remains
  impossible at `3.3442167670577247`; the derived band is empty.  The normal
  owner path returns a fully default result, while the exact diagnostic returns
  `254`.  Five-trial observations are `74.0774 ms` device and `711.1436 ms`
  wall p95, projecting to `32.37/310.75 h` respectively for tier-10 times 25 s.
  No long shadow or later calibration stage runs because the first limited
  step already fails the function-level detector.  Durable evidence is
  `93409b4d...8ace2b8`; golden bytes remain unchanged.
- **r145 manifold-predictor review closure (2026-08-22):** fresh review leaves
  the r144 numerical verdict unchanged and makes its lifecycle claims
  load-bearing.  The terminal manifold measurement now sends the complete
  nine-component Binary32 state through the single r60 predicate before EOS
  inversion; arbitrary below/above-bracket energy and affine-row mutants RED.
  Plateau-evidence activation is exactly `1` or fails before Metal, and the
  normal rejection poisons then verifies every public result field.  Only a
  Binary32 result with both projections validated, two resident invocations,
  zero interstage transfers, a passing `7.5e-4` function gate, and finite
  derived diagnostics can publish the `(dt,G,r)` observation used by the next
  selector.  Checkpoint format 11 persists that observation; versions 5--10
  decode it as unavailable, preserving old golden bytes.

  Retained r138 reproduces trace `f90a2508...551cebf` and exit `243`; retained
  r142 reproduces exit `253`.  The r136 trace is re-pinned to
  `c917ea32...ccfcf94` after the r143 force API entered its generated source
  manifest; all numerical stage pins and the `0xff` refusal are unchanged.
  The strengthened r144 replay again exits `254` with identical selector,
  generation, field, and drain values.  Its new timing observation is
  `70.0796 ms` device / `600.4716 ms` wall p95, or `30.62/262.39 h` for
  tier-10 times 25 s.  The closure artifact is `3e8bf30a...3fa8c06`; golden
  SHA remains `1b944176...4947`.  Because the first limited burning step still
  requires drain `3.3442>1`, the long shadow and subsequent contract campaign
  remain correctly blocked.
- **r146 accepted-observation lifecycle correction (2026-08-22):** a second
  fresh review found that r145 had serialized the observation without making
  it authoritative.  Publication trusted caller-authored required drain and
  band fields, checkpoint v10 did not bind the observation timestep to the
  accepted step, a reused v5--v9 destination could retain stale metadata, and
  the retained resident campaign did not feed accepted `(dt,G,r)` into its
  next selector.  All four seams now fail closed: publication independently
  recomputes the plateau mechanism and rejects forged results; v11 requires
  `observation.dt==previousStep==lastAcceptedStep`; legacy decoding clears the
  tuple; and r118 executes the real publish--select lifecycle before every
  resident step.  Owner-only publication is deliberately omitted from the
  generated arithmetic mirrors, moving the source-bound r136 trace to
  `4cb7e6cf...75ef3bcb` without changing its arithmetic census or refusal.

  The retained numerical evidence is unchanged.  r118 reproduces plateau
  `1.5439012582030287e-4`, field plateau `6.5237316812827295e-4`, trace
  `719ee45e...f68cb`, and final digest `d9a1a0ea...c9dc2`; r119 again accepts
  every scalar, velocity, and inventory channel; r138 retains
  `f90a2508...551cebf`; r142 and r144 retain exact exits `253/254`.  The
  lifecycle rerun does expose a separate performance fact: r118 device p95 is
  stable at `27.6619 ms`, but wall p95 is `329.4053 ms`, so its historical
  `200 ms` wall acceptance is not re-certified.  A one-variable parallel EOS
  reduction experiment measured `328.0007 ms` and was reverted as noise-level
  benefit with bit-identical outputs.  The burning stop remains decisive:
  the limited step reaches `2.5081625764804549e-3`, requires drain
  `3.3442167670577247`, and atomically rejects.  Durable correction evidence
  is `1bc3ff98...be06cc`; golden remains `1b944176...4947`.
- **r147 producer-owned observation authority (2026-08-22):** fresh review
  corrects r146's remaining public-authority gaps without changing the r144
  stop.  A successful resident step now records its represented binary32
  timestep and receives a private, copy-clearing token only after both
  projections validate and its mechanism and field plateau gates pass.  The
  token binds physical residual evidence and a field-tagged, length-delimited
  resident-payload digest plus timestep, `G`, terminal field deviation,
  required/delivered drain, and mechanism band.  Publication consumes it;
  scalar, vector-boundary, physical-validation, and coherent diagnostic
  mutations, copying, and replay therefore RED.  The published observation is
  opaque outside the producer and the complete library-owned checkpoint codec,
  and retains the digest so post-publication mutation cannot reach state
  application.  Authority cannot be regained by rewriting `(dt,G,r)`.  The
  production selector also rejects unavailable metadata whenever
  `previousStepS>0`, so v5--v9 production resume cannot silently bypass the
  manifold limit.  The binary64 oracle retains its separate five-argument CFL
  selector and never inherits that production-only stop.  Owner-only
  publication declarations are absent from both generated arithmetic mirrors.

  Exact exit `255` now exercises a real two-step Metal lifecycle: first-step
  token publication, single-use/copy/payload/diagnostic REDs, represented timing
  update, sealed v11 checkpoint, reload, and resumed selection.  Writer-side and
  checksum-valid loader-side timestep mismatch REDs bind both v11 equalities;
  changing an otherwise finite tuple while replaying the authentic seal also REDs.
  It pins `G=1.2031080315688669e-4`,
  `r=0.99562928290235475`, and resumed CFL selection
  `0.0018513042677754073 s`.  Retained r118 physics remains byte-identical
  (`719ee45e...f68cb`, `d9a1a0ea...c9dc2`), though wall p95 is still a failed
  `325.3877 ms` observation against the historical `200 ms` budget.  r119
  accepts every channel; r138 retains `f90a2508...551cebf`; r142/r144 retain
  exact `253/254`.  The owner-only header change moves the source-bound r136
  trace to `26b12e46...c7fd9` without changing any arithmetic pin or the
  `0xff` refusal.  The burning result remains tokenless and rejected at field
  `2.5081625764804549e-3`, required drain `3.3442167670577247`, delivered
  `0.97489008508207653`.  Durable evidence is the SHA-bound r147 artifact; golden
  remains `1b944176...4947`.  Later contract stages remain blocked at the same
  function-level capacity finding.
- **r148 checkpoint-state authority closure (2026-08-22):** fresh review
  rejected r147's unkeyed `(dt,G,r)` seal as integrity metadata masquerading as
  authority.  Format 13 removes the raw tuple factory and binds the opaque
  observation to the normalized producer-consumed Binary32 state: represented
  shape, component-major conservative values, and terminal restoration momentum
  and velocity, with field tags and lengths.  Persisted temperature must equal
  the single canonical Binary32 inversion of those conservative values, and
  production timestep selection promotes the represented Binary32 cell width;
  neither is an independent state coordinate.  The writer reconstructs that
  state and digest from the applied checkpoint; the library-owned reader consumes
  the complete normalized state and lifecycle view, reopens and verifies the
  payload, its domain-separated prefix binding, accepted-step count/history/timing,
  and the reconstructed state digest before restoring the observation.  The selector
  also consumes the current state view and derives its digest internally; no caller-
  supplied digest scalar can preserve stale authority.  Accepted history exactly
  reconstructs simulation time.  The first step is owned by the Binary64 analytic
  beginning; no zero-step state is persistable, so coordinated clearing of
  observation/count/time/history cannot relabel accepted bytes as a first step.  The
  prefix checksum is deliberately described only as integrity evidence, not a
  keyed authenticator.  A transplanted observation, accepted-history/zero-dt
  alias, temperature/velocity transplant, cleared-observation owner alias,
  selector-side state transplant, coordinated metadata clear, non-tail history/time
  transplant, and every v9/v10/v11 Binary32 resume all RED.
  The live owner itself requires homogeneous Binary64 when accepted count is zero;
  the coordinated-clear RED reaches that exact selector helper as well as v13 writer
  and checksum-valid loader paths.  Legacy Binary32 formats 9--11 reject symmetrically
  at writer and loader, with accepted and all-zero cases bound separately on both sides.
  Retagging the cleared accepted state Binary64 also REDs: the live owner rebuilds
  the inferred tier's analytic beginning and byte-identifies it, while writer and
  checksum-valid loader reject every zero-step checkpoint independent of precision.
  The legacy v9/v10/v11 matrix instantiates those all-zero writer and checksum-valid
  loader REDs separately for Binary32 and Binary64, so neither version nor precision
  can scope the prohibition away.
  The accepted timeline predicate is now one executable rule used by the live
  selector, writer, and loader.  Format 13 adds an opaque, payload-bound Binary64
  origin authority issued only by the Binary64 owner; r60 revalidation remains a
  feasibility prerequisite, not provenance.  Modern Binary64 formats 9--12 lack
  that authority and are retired for resume, while historical formats 5--8 remain
  grandfathered by their pre-resident format lineage.  Current writers cannot emit
  any accepted Binary64 checkpoint without the origin authority, and an intact
  accepted-state retag RED reaches the live owner.  A last-step-only mutation RED
  reaches owner, writer, and checksum-valid loader.  The Binary64 authority is
  computed by the canonical format-13 writer in digest-only mode over the complete
  serialized resume prefix, not by a second hand-maintained field list.  Frame-value,
  accumulated-integral, and statistics-duration mutants independently RED at writer
  and checksum-valid loader, binding every persisted resume input through the one
  serialization topology.

  Review also required the token-mint rule at the actual Metal owner rather than
  only its extracted predicate.  A preflighted exact probe now executes the real
  physical and restoration projections, changes only the physical validation
  result before minting, proves the result tokenless and unpublishable, and
  byte-compares its resident payload to the normal twin.  Malformed activation
  fails before command submission.  Exact lifecycle exit `255` retains
  `G=1.2031080315688669e-4`, `r=0.99562928290235475`, and resumed CFL step
  `0.0018513042677754071 s` after canonical Binary32 cell-width promotion.  The source-bound r136 trace moves to
  `727a9b39...00ff2ed`; arithmetic and exact refusal `237` are unchanged.
  r144 still exits `254` at field `2.5081625764804549e-3`, required drain
  `3.3442167670577247`, and delivered drain `0.97489008508207653`, so no later
  contract stage runs.  Golden remains `1b944176...4947`.
- **r149 manifold stage-budget protocol (2026-08-22, pre-evidence):** r145's
  Δt falsification is accepted rather than tuned around.  The manifold limit
  remains in the selector, while the wall-cost repair moves the per-cell EOS
  deviation map and exact nonnegative maximum reduction onto Metal with no new
  full-grid transfer.  The golden state is then swept at CFL, CFL/2, and CFL/4
  and decomposed across remap/advection, physical projection, and restoration
  projection using `max |V_after-V_before|`; adjacent log2 ratios report each
  stage's Δt exponent.  The response is frozen before values: a dominant
  timestep-invariant remap term triggers manifold-consistent temperature/
  composition reconstruction with rebuilt energy; dominant restoration
  self-generation triggers the two-pass anomaly-aware target; failure of both
  to hold `7.5e-4` requires a contract-level production-ceiling ruling.
- **r150 burning stage budget and ceiling stop (2026-08-22):** the device-side
  per-cell EOS map and exact nonnegative max reduction replace the CPU field
  walk without adding a full-grid transfer.  At CFL, CFL/2, and CFL/4 the
  remap/advection generation is `2.5327801704406738e-3`,
  `2.5155544281005859e-3`, and `2.5067925453186035e-3`, with adjacent exponents
  `0.009845460522765278` and `0.005033797039351196`; both projection-stage
  scalar contributions are exact zero.  Thus remap owns the complete directly
  measured budget and the burning term is timestep-invariant.  The same device
  evaluator measures cold `G=1.2048172357026488e-4`, making the burning/cold
  contrast `21.0221110x`.

  The frozen decision branch was executed by the retained diagnostic-only
  exact-`245` comparator.  A shared-alpha ten-tuple reconstruction transported
  `rho*T` with the nine conservative components and rebuilt energy while
  preserving shared-alpha and monotonicity.  Binary64 thermochemistry before
  final binary32 energy publication made this a favorable reconstruction
  rather than an fp32-rounding penalty.  Its three maxima were
  `2.5328069638265172e-3`, `2.5155729299433105e-3`, and
  `2.5068855498342479e-3`: slightly above the device baseline, so the
  reconstruction provided no reduction and was kept out of production.  The
  anomaly-aware restoration branch is inapplicable under the
  pre-registered rule because restoration scalar generation is zero rather than
  dominant.  With the CFL term still `3.3770759518x` the `7.5e-4` allowance,
  r150 requires the contract-level production-ceiling ruling and stops before
  long shadow, golden `B_fp32`, guard supersession, temporal refinement,
  readmission, source maps, or first light.

  The exact-`254` timing gate uses one warmup and five samples, taking the
  maximum as p95; it requires device p95 <= `75 ms` and wall p95 below the
  prior `600.471584 ms`.  The calibrating observation is `65.7983333 ms`
  device and `196.349959 ms` wall: the
  former meets the requested approximately-70-ms resident target and the latter
  is a truthful remaining regression, though it is `67.3007%` below the prior
  `600.471584 ms`.  The represented tier-10 x 25 s wall projection is therefore
  corrected from `262.3922 h` to `85.8004 h` (device-only `28.7524 h`), not
  claimed as a 70-ms wall result.  Durable evidence is the r150 stage-budget
  artifact; the golden checkpoint remains unchanged.  Mirroring the resident
  result schema moves the source-bound r136 digest to
  `a0e42abe...b85989e` without changing its arithmetic refusal.
- **r151 accepted-map reconstruction stop (2026-08-23):** fresh review found
  that r150's `rho_total*T` tracer was not a manifold-consistent
  reconstruction when molecular weight varies across a burning front.  That
  diagnostic conclusion is retired; the r150 device stage ownership and
  timestep-scaling measurements remain valid.  The replacement retained
  exact-`245` diagnostic runs the actual nine-component shared-alpha remap,
  leaves components 0 through 7 byte-identical, and searches the binary32
  sensible-energy lattice for the two adjacent values bracketing `V=1` under
  the authoritative r60 accepted-volume map.  This is the favorable
  manifold projection: it tests the real endpoint tolerance and fp32
  representability rather than an auxiliary tracer.

  The reconstruction reduces the CFL/CFL/2/CFL/4 maxima to
  `1.302156695613399e-3`, `1.2866699325340125e-3`, and
  `1.2788512525973017e-3` (48.59%, 48.85%, and 48.99% reductions), but the
  CFL value remains `1.7362089275x` the unchanged `7.5e-4` allowance.  The
  limiting cell is 3478; its beginning deviation is
  `3.0191404931656507e-12`, while the nearest accepted ratios straddle the
  switching gap at `0.99869784330740574` and `1.0014969001088954`.  Thus no
  representable energy at that fixed composition holds the required
  plateau.  The anomaly-aware restoration branch remains inapplicable:
  physical- and restoration-projection scalar generation are both exactly
  zero.  Under the pre-registered r149 decision rule this is the requested
  contract-level production-ceiling stop; production arithmetic is unchanged.

  The resident timing gate is also corrected to the load-bearing completed-
  call budget: five warm samples use the maximum order statistic and require
  device `<=75 ms` and wall `<=200 ms`, not merely improvement over the old
  `600.471584 ms` regression.  The calibrating observation is
  `66.3667917 ms` device and `199.188458 ms` wall.  The completed-call budget
  passes, but the requested approximately-70-ms wall target does not; the
  corrected tier-10 x 25 s projection is `87.0408 h` wall (`29.0008 h`
  device-only), versus the prior `262.3922 h`.  Durable evidence is
  `r151_accepted_map_reconstruction_stop/accepted_map_reconstruction_evidence.v1`;
  the golden checkpoint remains byte-identical.
- **r152 conservative face-reconstruction stop (2026-08-23):** fresh boundary
  review correctly rejects r151's post-remap energy substitution as a primary-
  ledger repair.  Its fixed-composition representability result is retained as
  a diagnostic but cannot decide the ruled reconstruction branch.  The
  replacement exact-`245` trial moves the intervention to the conservative
  reconstruction/flux seam.  Its single shared-alpha tuple is `rhoZ`, the
  seven constituents, and `n*T=P/R`; each swept slug obtains
  `T_face=F_(nT)/sum_i(F_i/M_i)`, and the energy flux is
  `sum_i F_i*h_i(T_face)`.  Cell energy changes only by the right-minus-left
  face-flux divergence through all five palindrome passes.  Therefore shared-
  alpha, transported-variable monotonicity, and the energy inventory ledger
  are preserved; no cell-average repair exists in the trial.  Alpha and
  composition are explicitly free to differ from the baseline.

  The historical diagnostic produced CFL/CFL/2/CFL/4 maxima
  `2.5328069638265172e-3`, `2.5155729299433105e-3`, and
  `2.5068855498342479e-3`: no reduction.  Cell 3227 remains limiting, from
  beginning deviation `-1.1871614802316799e-12` to terminal deviation
  `-2.5328069650136786e-3`.  The CFL result is `3.3770759518x` the unchanged
  `7.5e-4` allowance.  Projection-stage scalar generation remains exactly
  zero, so the anomaly-aware restoration branch is still inapplicable.  Fresh
  review later found that the algebraic auxiliary evolved after the first
  pass, so this legal-topology capacity inference is superseded by r153.
  Production remained unchanged.  r151's timing correction remains
  valid (`<=75 ms` device, `<=200 ms` completed wall; observed
  `66.3667917/199.188458 ms`).  Durable evidence is
  `r152_conservative_face_reconstruction_stop/conservative_face_reconstruction_evidence.v1`;
  golden remains byte-identical.
- **r153 fixed-pressure reconstruction/domain stop (2026-08-23):** fresh
  review invalidates r152's capacity inference because its `n*T` auxiliary was
  evolved after the first sweep.  The corrected exact-`245` diagnostic resets
  `n*T=P/R` before every x/2,y/2,z,y/2,x/2 pass, independently derives the
  geometric fixed-pressure face integral, and binds reconstruction/production
  field hashes, limiter-plus-energy-flux trace hashes, a nonzero field delta,
  endpoint counts, and the boundary-flux energy ledger.

  The corrected topology encounters the certified thermochemistry boundary:
  conservative compression requires face temperatures below `300 K` on
  `2,307,844`, `2,326,957`, and `2,356,525` evaluations at CFL/CFL/2/CFL/4,
  with maximum excursions `0.3536066003`, `0.1761488694`, and
  `0.08734068083 K`.  Those values scale approximately with dt and are not a
  rounding-bound calibration.  Shared-alpha or monotonicity relaxation and
  uncertified thermochemistry extrapolation remain rejected.

  A diagnostic-only favorable endpoint projection preserves the conservative
  energy ledger to at worst `1.4020231210267571e-10` relative and produces
  byte-distinct fields, but leaves G exactly at
  `2.5328069638265172e-3`, `2.5155729299433105e-3`, and
  `2.5068855498342479e-3`.  The CFL value is still `3.3770759518x` the
  allowance.  Because physical- and restoration-projection scalar G remain
  exactly zero, the anomaly-aware restoration branch is inapplicable.  The
  high-order endpoint is a limiter obligation, so r153's capacity inference is
  retired by r154.  No endpoint projection was admitted into production.  The
  corrected 75/200-ms residency gate observes
  `66.4652500/194.687208 ms`; the corresponding `29.0438/85.0738 h` tier-10
  device/wall projections remain binding.  Durable
  evidence is `r153_fixed_pressure_reconstruction_stop/`
  `fixed_pressure_reconstruction_evidence.v1`; golden remains byte-identical.
- **r154 composed low-order manifold capacity stop (2026-08-23):** retired by
  r155.  Its current-pass alpha-zero donor inherited greedy earlier passes,
  and its endpoint width used a temperature scale rather than r60's
  composition-dependent energy scale.  Its SHA-bound fields and ledgers remain
  historical diagnostics, not capacity evidence.
- **r155 globally low-order reconstruction stop (2026-08-23):** retired by
  r156.  It removed r154's greedy predecessor but reversed the heat-capacity
  proof direction: `tolerance/Cp_lower` is a necessary outside bound, not a
  sufficient inside bound.  Its SHA-bound fields and ledgers remain historical
  diagnostics only.
- **r156 coupled-alpha thermochemistry stop (2026-08-23):** retired by r157.
  The endpoint width used an independent absolute-polynomial `Cp_upper` over
  every certified segment.  Maximum widths are
  `0.06678539755/0.06678528278/0.06678524324 K`; the all-alpha-zero shadow first
  exceeds them in passes `1/1/3` at CFL/CFL/2/CFL/4.

  The CFL witness is pass 1, line 29, donor 42, cell 3641 at
  `299.92254298172884 K`.  Greedy and all-zero histories give identical donor
  bytes (`2ac03ae9...346d3`).  An independent affine interval then releases
  both adjacent pass-0 face alphas separately across their entire low/high
  ranges—a superset of the required shared-alpha topology.  Even that superset
  has minimum molar density `0.040632479709723168 kmol/m3`, above
  `P/(R*Tmin)=0.040621987915680717 kmol/m3`.  No coupled/backtracked monotone
  alpha can form the next face state inside the certified thermochemistry
  domain.

  A favorable endpoint-projected diagnostic continues the all-low-order
  ledger, but is not admitted.  It leaves G exactly
  `2.5328069638265172e-3/2.5155729299433105e-3/2.5068855498342479e-3`, still
  `3.3770759518x` the CFL allowance.  Both projection-owned G terms are zero,
  so the anomaly-aware branch is inapplicable.  The invariant-preserving
  reconstruction resists the coupled route and its favorable relaxation still
  fails the plateau; the pre-registered production ceiling/thermochemistry
  contract boundary is reached.  Production remains unchanged and later work
  stays blocked.  Exact evidence is
  `r156_coupled_alpha_thermochemistry_stop/coupled_alpha_thermochemistry_evidence.v1`;
  golden remains byte-identical.
- **r157 r60 reconstruction/capacity stop (2026-08-23):** retired by r158.
  Fresh review rejects
  r156's final implication: `tolerance/Cp_upper` is a sufficient inclusion
  width, but exceeding it is inconclusive and cannot prove exclusion.  No Cp
  quotient is load-bearing in r157.

  The corrected exact-`245` trial implements the ruled reconstruction with
  fixed `n*T=P/R`, one shared alpha, conservative face-energy fluxes, and the
  r60 precision-boundary completion: an out-of-domain face uses the certified
  endpoint enthalpy rather than extrapolated thermochemistry.  Every one of
  `4,926,768` nonzero swept faces and all `4,881,360` intermediate cells after
  the five pass boundaries validate under the authoritative Binary32 r60
  predicate, independently at each of CFL/CFL/2/CFL/4.  Field and trace hashes
  plus the energy ledger prevent bypass.

  The all-low-order shadow also retains a local API RED: its first pass-1
  `299.99999426911722 K` exact-temperature lookup must fail with the
  authoritative out-of-domain diagnostic.  This records why the endpoint
  completion exists, but is explicitly not a global-alpha exclusion and does
  not support the capacity verdict.

  The fully r60-admissible endpoint completion nevertheless leaves field-max G
  at `2.5328069638265172e-3`, `2.5155729299433105e-3`, and
  `2.5068855498342479e-3`; CFL is `3.3770759518x` the `7.5e-4` allowance.
  Physical- and restoration-projection G remain exactly zero, so the
  anomaly-aware branch is inapplicable.  Under the pre-registered automatic
  rule the reconstruction remedy has now been executed and fails, reaching
  the production-ceiling/thermochemistry contract boundary.  This is a stop,
  not a ceiling derivation or widening: production stays unchanged and later
  milestones remain blocked.  Fresh timing is `66.0780417/197.288084 ms`
  device/wall, projecting `28.8746/86.2105 h` at tier-10 x 25 s.  Durable
  evidence is `r157_r60_reconstruction_capacity_stop/`
  `r60_reconstruction_capacity_evidence.v1`; golden remains byte-identical.
- **r158 producer-rounded reconstruction stop (2026-08-23):** fresh review
  catches two producer-authority defects in r157: its global alpha rectangle
  omitted Binary32 update rounding and the current face alpha, while its face
  predicate checked double energy before the float flux was published.  Both
  claims are retired.  r158 makes no global-alpha exclusion and validates the
  actual `float` energy-flux payload divided by swept volume.

  The exact-`245` rerun validates `4,926,768` producer-rounded faces and
  `4,881,360` intermediate pass cells per timestep level under the single
  Binary32 r60 predicate.  The reconstruction fields, traces, conservative
  energy ledger, endpoint counts, and G values remain exact: CFL/CFL/2/CFL/4
  G is `2.5328069638265172e-3`, `2.5155729299433105e-3`, and
  `2.5068855498342479e-3`.  CFL remains `3.3770759518x` the `7.5e-4`
  allowance.  The local below-domain lookup is retained only as a diagnostic
  explaining the endpoint completion; it is explicitly not load-bearing.

  Projection-owned G remains exactly zero, so the anomaly-aware branch is
  inapplicable.  The pre-registered reconstruction remedy has now been
  executed with producer-rounded admissibility and fails its function-level
  plateau gate.  The sequence therefore stops at the requested contract-level
  ruling boundary without deriving or widening a ceiling.  Production remains
  unchanged; fresh timing is `66.0780417/197.288084 ms`, projecting
  `28.8746/86.2105 h`.  Durable evidence is
  `r158_producer_rounded_reconstruction_stop/`
  `producer_rounded_reconstruction_evidence.v1`; golden remains byte-identical.
- **r159 timestep-velocity audit and function-derived ceiling stop
  (2026-08-23):** the frozen `5.6295254283638751e-5 s` accepted step was
  incorrectly being read as a production CFL step, which implies
  `217.37616398903009 m/s`.  The exact on-device audit instead finds the
  golden transport field at `7.4333348274230957 m/s`, the 17-cycle physical
  projection at `7.371121883392334 m/s`, and the terminal field at
  `7.371121883392334 m/s`.  The largest restoration correction is only
  `1.7818529158830643e-6 m/s` (`1.0030986299568027e-10 m/s*s` impulse), or
  `2.4173429012179363e-7` of the physical maximum, so neither the physical
  field nor the finite-per-step correction explains 217 m/s.

  The preserved oracle console log supplies the actual lineage: the immutable
  checkpoint predates r80/r81 and was accepted after repeated R0/R1 augmented
  active-set cycle retries.  r59's two-class doctrine was subsequently applied
  to that Zeno class by r80/r81; no new oracle fix is needed and the golden
  physical state is healthy.  The production selector is re-evaluated from the
  transport field alone—per-step-finite projection/restoration corrections are
  excluded under the r70 death-spiral rule—and selects the advective CFL at
  `1.6462660045688639e-3 s` (`1.6601606404766957e-3 s` from the physical-
  projection maximum).  The complete selector's measured reduced-gravity and
  active-diffusivity candidates are looser at `0.015812081290725255 s` and
  `0.024674291069445888 s`; the selected step is represented as
  `1.6462659696117043e-3 s` with one force substep.  Because every frozen slice
  restarts from the shared golden beginning, no prior production observation
  or growth cap applies.  The old step was a retry-history surrogate, not a
  CFL measurement.

  Only after that physical-reference audit is the ceiling ruling applied.
  Following r72, the gate derives from its function: r60's accepted thermo/
  temperature-inversion pressure-deviation tolerance is `1e-3`; with the
  pinned stability headroom `h=2^-2`, the bounded-plateau ceiling is exactly
  `(1-h)*1e-3=7.5e-4`.  The earlier inheritance of that number without this
  function derivation is recorded as a design-level derivation error, not an
  invitation to change the value.  The producer-rounded reconstruction's
  calibrating plateau `2.5328069638265172e-3` is `3.3770759517686897x` above
  the derived ceiling.  The ceiling is not widened: this is the requested
  thermo-domain finding and a real stop; physical fidelity remains owned by
  the oracle-comparison contract.

  The field-G/drain path remains an exact Binary32 per-cell Metal map plus max
  reduction with one scalar and zero full-grid device-to-host transfers.
  Parallelizing the nine independent dual-layout packs on the topology-aware
  global thread pool, under `render_thread_reserve_count 0`, reduces the
  controlled same-step completed-wall p95 from `202.659166` to
  `154.74187499999999 ms`; device p95 moves from `73.046958423219621` to
  `73.379833251237869 ms`.  That is a `23.644275235989082%` wall reduction and
  projects the audited 25.03248-s tier-10 stepping work to
  `0.6535957666507461 h` wall (`0.3099403336721022 h` device).  The remaining
  `81.36204174876212 ms` host residual—preflight/layout, uploads, command
  submission/waits, and postprocessing—is named work; the `200 ms` gate is a
  ceiling, not the target.
  The timed result itself binds the represented selected step and one-substep
  duration; every warmup and measured serial/parallel execution byte-matches
  the baseline complete payload and every non-timing semantic diagnostic.  Legacy
  `force_all_threads_low_priority` execution retains a serial packing route so
  a saturated render pool cannot enter the documented non-stealing nested-pool
  deadlock.
  The production solver is therefore not claimed faster than the oracle in
  validated practice while the thermo stop remains.  Long shadow, `B_fp32`,
  guard supersession, temporal refinement, readmission, source maps, and first
  light remain blocked.  Durable evidence is
  `r159_timestep_velocity_ceiling_stop/timestep_velocity_ceiling_evidence.v1`;
  golden remains byte-identical.
- **r160 corrected low-Mach ownership and audited-step refusal (2026-08-23):**
  r159's final derivation is retired.  The `1e-3` EOS residual comparison is an
  oracle validity detector, not a production thermochemistry/T-inversion
  domain property.  Enumeration of every production consumer finds no
  pressure-deviation domain limit, so r72 requires a production-owned gate.
  The replacement is exact `2^-5` from low-Mach asymptotic validity, plus a
  separately measured non-secular long-shadow detector.  Restoration's local
  mechanism is nonamplifying; measured drain remains predictor data rather
  than an invented one-step removal requirement.

  The old-step `2.5328069638265172e-3` observation is 12.3381x below the new
  ceiling, but the audited `1.6462659696117043e-3 s` physical CFL application
  produces `G=field_max=0.085895776748657227`, delivered drain
  `0.66657990322152694`, and a valid restoration residual reduction from
  `7.3080245783785358e-6` to `2.4366422621824313e-6`.  The field is
  `2.7486648559570312x` above `2^-5`; exact exit 252 therefore withholds the
  accepted token before the 104-step shadow can start.  No ceiling is widened.
  The manifold floor remains a pre-registered additive-contract/readmission
  risk, but that verdict cannot run past this formulation stop.  B_fp32,
  temporal refinement, readmission, source maps, and first light remain
  blocked; r159's 81.36 ms host residual stays on the device-bound backlog.
  Golden remains byte-identical.  Durable evidence is
  `r160_low_mach_audited_step_refusal/low_mach_audited_step_refusal.v1`.
- **r161 two-pass advective-anomaly closure and target-schedule stop
  (2026-08-23):** r160's refusal is retained as the closure-disabled RED, not
  the final design conclusion.  The measurement history is reconciled by a
  two-regime model `G=G_floor+k*dt`.  A least-squares fit to r158's
  CFL/CFL/2/CFL/4 observations gives `G_floor=0.0024982685328926446` and the
  floor-regime dose coefficient `k=0.6137015145338649 s^-1` (maximum fit
  residual `3.015564319693714e-8`).  At r160's audited physical CFL, the
  unclosed dose branch has `k=50.65858722417441 s^-1`; this accounts for
  `G=0.085895776748657227` without changing either dataset.

  The pre-registered two-pass remedy is now resident.  The predictor remaps
  the nine-component scalar ledger, evaluates the exact Binary32 manifold map
  on-device, and folds `(V_predictor-V_beginning)/dt` into the restoration
  divergence target.  The restoration pressure solve retains its Private
  velocity; a corrector reruns the same five scalar submaps from the original
  beginning state and applies the same source operand before the sole terminal
  publication.  The dual ledger is unchanged.  The complete path is ten cell
  submaps, fifteen dual submaps, two source commands, two projections, two
  scalar reductions, and zero interstage full-grid transfers.  Its working-set
  certificate includes both simultaneously live cell-palindrome allocations
  and the retained restoration state.

  At `dt=0.0016462659696117043 s`, closure lowers G to
  `0.066569089889526367` (`0.7749984039880868` of the disabled result), an
  effective dose coefficient `38.91887613503045 s^-1`, but still exceeds the
  exact `2^-5` hard ceiling.  One warmup plus five byte-stable samples measure
  `92.4832500750199/179.9965 ms` device/wall p95, projecting tier-10 x 25 s to
  `0.39012213338718343/0.75927931301360085 h`; the two-hour wall criterion is
  met at this audited-CFL control, but its field is inadmissible.  The amended
  25%-headroom predictor derives
  `0.00057953997747972608 s`.  The first candidate evidence incorrectly kept
  the audited-CFL Heun divergence target while changing the represented step;
  fresh boundary review rejected that measurement because the external target
  schedule is dt-dependent.  With the binary64 oracle target correctly
  re-derived at the limiter step, its frozen R0 open conservative Picard solve
  does not converge: first residual `7.41824`, last/minimum `1.44776`, target
  `0.561256`, mass `1.44776`, coefficient `0.017278`, active set `1`, and
  tolerance `0.000479545`.  No limited G, plateau, timing, or wall projection
  is therefore admissible.

  The zero-anomaly RED remains a complete on-device A/B: after device reconstruction of
  the beginning manifold value, closure-active and single-pass runs publish
  byte-identical accepted payloads and the active route records one predictor
  pass with no corrector palindrome.

  Exact exit 219 records a target-schedule protocol blocker: the binary64
  oracle cannot form the limiter step's external physical target under its
  frozen Picard topology, before any limited production request exists.  This
  is not a production remap or reconstruction-class finding.  The
  approximately two-hour rule is not evaluated;
  no accepted token is minted and the 104-step shadow does not start.
  Owner ruling on target generation/fixed-point topology is required.  B_fp32,
  guard supersession, temporal refinement, readmission, source maps, and first
  light remain blocked.  Golden remains byte-identical.
- **r162 equal-time golden composition and remap-scheme stop (2026-08-23):**
  equal-step comparison is retired.  A coupled Picard fixed point has a
  timestep-dependent contraction radius, so the old protocol silently
  required a shared step-size domain that neither solver promised.  The
  binary64 oracle is now the flow reference: it advances from the exact golden
  beginning to production's endpoint with its own converged schedule, while
  production takes one limiter-derived step.

  A pre-measurement sweep at limiter `dt`, `dt/2`, `dt/4`, and `dt/8` finds
  proved two-class active-set cycles at the first three levels.  Their failing
  terminal residuals are `1.4477584866157618`, `0.72567590358972345`, and
  `0.56139851539581598 s^-1`; they are not a smooth contraction stall.  The
  existing r59/r80 two-class treatment succeeds at
  `dt/8=7.244249718496576e-5 s`, where R0/R1/R2 end at
  `2.5305532581487711e-4`, `4.6488187337700992e-4`, and
  `6.0149754458578642e-5 s^-1`.  Trace `900a7acc...a051c` binds the complete
  curves and active-set census.

  Eight exact binary64 substeps reach the production endpoint
  `0.00057953997747972608 s`; the terminal target is tagged with that endpoint
  and schedule digest `e4472da7...c97e0` matches in serial and parallel.
  The producer-rounded terminal target is separately bound by
  `d198eaaa...351ec`; the actual penultimate vector substituted at the current
  endpoint is RED.  Mismatched endpoint and stale-substep target REDs fail.  Minutes per slice
  are accepted fixture cost and are deliberately not optimized.  The amended
  additive contract is scheme distance + oracle temporal distance + production
  temporal distance + subdominance, all at one end time; temporal refinement
  moves inside this protocol.

  Equal-time composition clears r161's target-schedule blocker, but the
  limited production result does not clear the next contract boundary.
  Predictor `G=0.020501971244812012`; corrected
  `G=field_max=0.024358630180358887`; the 25%-headroom allowance below `2^-5`
  is `0.0234375`.  The token is withheld and the backstop derives
  `dt_next=0.00055743221913055079 s`.  Device/wall p95 are
  `89.583708089776337/175.805542 ms`, projecting the current larger
  step to `1.073453270061155/2.1066222640131049 h`.  Further timestep
  reduction cannot recover a fixed-work two-hour wall budget.  Exact exit 213
  is therefore the pre-registered remap-scheme finding; no ceiling or budget
  moves.  Long shadow and every later arithmetic/readmission/source/preview
  gate remain blocked.  Golden remains byte-identical.
- **r163 predictive-initial-step refusal (2026-08-24):** the remap-scheme
  change is rejected.  r162's limiter fixed point near `5.57e-4 s` is the
  intended operating class; its first-step miss is classified as a transient
  and the excess wall time remains the named host residual.  The contraction
  sweep is retained as an oracle property: steep states enter proved
  two-class R0/R1 cycling at the three tested `dt`, `dt/2`, and `dt/4`
  values.  `1.4488499436993152e-4 s` is the smallest tested cycling step and
  `7.244249718496576e-5 s` the largest tested convergent step; no continuous
  monotonic boundary is inferred.  The sealed eight-substep equal-time
  schedule remains the standing per-slice answer.

  The owner-mandated first-step predictor is implemented and source-bound as
  `dt0=dt_audit*allowance/G_audit`.  With the sealed r162 observation
  `dt_audit=5.7953997747972608e-4 s`, `G_audit=2.4358630180358887e-2`,
  and allowance `2.34375e-2`, it derives binary64
  `5.576244690940563e-4 s`, represented in production as
  `5.5762444389984012e-4 s`.  The SHA-bound golden fixture owns and records
  all three operands; no generic resident request is claimed to authenticate
  a caller-authored calibration tuple.  The old CFL-initialized exact-252
  branch remains RED.

  Equal-time replay exposes a small but real nonlinearity that the mandated
  proportional predictor does not cover.  Predictor G is
  `1.9734203815460205e-2`, but corrected G and the realized plateau are
  `2.3458600044250488e-2`, exceeding the allowance by
  `2.1100044250488281e-5`.  The hard `2^-5` ceiling is respected and the
  backstop derives `5.5692791475544124e-4 s`, but the requested first accepted
  step is not legal: no token is minted and exact exit 215 records the
  refusal.  The measured `89.7549167/178.69375 ms` device/wall values and
  `1.1177739/2.2253845 h` projections are diagnostic only.  Host profiling is
  technically possible on rejected byte-stable replays, but the owner ordered
  that campaign after a legal predictive first step, so it is not advanced in
  this entry.  Long shadow and accepted-state milestones remain blocked
  without widening or silently substituting the backstop step.  Durable evidence is
  `r163_predictive_initial_step_refusal/predictive_initial_step_evidence.v1`;
  golden remains byte-identical.
- **r164 drain-aware retry acceptance and host-residual campaign
  (2026-08-24):** r163's `0.09002685546875%` headroom excess is predictor
  precision, not a new transient class or a reason to widen the allowance.
  “Step 1 already legal” is clarified to mean that the first production step
  starts in the limiter operating class; an ordinary fail-closed refusal is
  still permitted.  Candidate 0 therefore reproduces the sealed r163 refusal
  at `5.5762444389984012e-4 s`, and its drain-aware suggestion is retried under
  the one shared 20-attempt production/capstone rejection cap instead of blind
  halving.  The attempt seam returns the refused diagnostics and no token;
  ordinary `Advance` still returns false with a default result.  Candidate 1
  runs at represented `5.5692793102934957e-4 s`, measures
  `G=field_max=0.023429989814758301`, clears the `0.0234375` allowance by
  `7.510185241699219e-6`, respects the `0.03125` hard ceiling, and mints the
  accepted token.  Its next limiter prediction is
  `5.5690890514272363e-4 s`.  No floor-aware analytic corrector is added.
  The accepted/retry/rejected disposition and shared cap are production-owned;
  every in-range candidate can advance or accept. Candidate range is checked
  before increment, and acceptance revalidates the producer token against the
  current complete payload, closing overflow and post-attempt mutation aliases.
  The mutation RED retains the authentic token while changing one payload bit,
  and a no-Metal candidate-1 refusal RED executes the owner's actual
  classify/branch/recursive-continuation seam and requires candidate 2.
  The equal-time campaign owner only
  regenerates the sealed target at the production-selected retry duration.

  The earlier `323 ms` value is retained only as an uncalibrated pilot.  The
  same-binary controlled serial wall p95 is `229.521625 ms`; production-parallel
  p95 is `151.894375 ms`, a `33.821322936346412%` reduction.  A versioned,
  order-sensitive, field-tagged/length-delimited live-state digest preserves
  the legacy checkpoint digest; independent digest fields and owner validation
  run in parallel.  Dual-static/force/cell preparation and the independent
  corrector/restoration-publication branches overlap on their existing queues;
  legacy low-priority execution stays serial.  Kernels and complete payload
  bytes are unchanged, while queue scheduling intentionally changes and is
  guarded by exact-247 serial/parallel payload equivalence.

  The corrected device quantity is the queue-DAG span, not a sum of overlapping
  command durations.  Final accepted p95 is `117.86270828451961/151.894375 ms`
  device-span/wall, with paired residual p95 `34.631833361461759 ms`, and the
  tier-10 x 25 s projection is
  `1.4696534042399729/1.89400098260743 h`.  The verbatim normalized replay
  lines are retained beside and SHA-bound by the certificate. This clears the earlier two-hour
  stop class but misses the approximately `1.3 h` target; the residual remains
  named critical-path work, and source maps will add device time. Durable evidence is
  `r164_drain_aware_retry_acceptance/drain_aware_retry_acceptance.v1`; golden
  remains byte-identical.  The 104-step shadow and later milestones await the
  fresh r164 contract-boundary review.
- **r165 validation operating point and long-shadow activation
  (2026-08-25):** the owner decouples the performance rung from the milestone
  ladder.  The accepted `5.5692793102934957e-4 s` operating point projects to
  `1.4696534042399729/1.89400098260743 h` device-span/wall for tier-10 x 25 s;
  that cost is accepted for validation.  The approximately one-hour design
  target moves to one device-critical-path campaign after thermo/source maps
  land, and the paired `34.631833361461759 ms` host residual remains backlog.
  The apparent `89.8 -> 117.9 ms` device-p95 growth is a metric-window
  correction: the earlier value summed active command durations, while r164
  measures the complete earliest-start to latest-end queue-DAG span including
  inter-command gaps.  Candidate-zero and candidate-one spans are comparable,
  so the refused retry is not accumulated into each accepted step.

  The 104-step campaign starts from the immutable golden state, uses the
  accepted candidate-zero-to-one transition once, and thereafter derives each
  candidate from the prior accepted `(dt,G,r)` observation.  Timing replays are
  excluded from the shadow.  Both projection validations, all 104 EOS
  deviations, the non-secular classifier result, trace digest, final state
  digest, and final represented step are the pre-registered evidence.  The
  subsequent contract/readmission/source/preview milestones remain ordered and
  cannot be claimed from this entry until their measurements complete.

  Execution produced three accepted diagnostic states before the non-secular
  window could be formed.  The third candidate began at
  `5.756302853114903e-4 s`, reached field `0.062683582305908203`, and needed
  six tokenless refusals before a `1.7358525656163692e-4 s` attempt reached
  `0.023434281349182129`.  Fresh review then invalidated the operating-point
  inference: the alleged advective dose was the Eulerian cellwise
  terminal-minus-beginning deviation and therefore included translation of an
  already nonuniform plateau.  That observable now has authority only when the
  beginning deviation field is bit-exact zero or the complete transport
  velocity is bit-exact at rest.  A moving nonuniform plateau remains tokenless
  until a material/transported baseline is derived, and ordinary application
  rejects it atomically even when the plateau and both projections pass.

  The `4.7152/6.0767 h` projection is withdrawn as well: it omitted the six
  refused attempts and used a prior single-attempt p95.  The measured final
  accepted attempt alone gives the conditional linear extrapolation
  `4.131755301914921/7.162223294085453 h`, not a lower bound or a complete
  retry-aware projection, and the raw record did not bind each attempt's
  equal-time schedule/terminal target.  The observation remains pinned as
  calibrating evidence only.

  A separate ownership repair is certified.  Exact exit 209 starts the
  physical open solve at 12 V-cycles, derives retries 12 -> 13 -> 14 from the
  measured contraction, validates at 14, and only then classifies the preserved
  tokenless manifold refusal.  The fp64 mirror and roundoff trace also carry a
  non-default request-owned 19-cycle schedule.  The 104-step shadow and every
  later rung remain unclaimed; the blocker is the missing material anomaly
  observable, not a newly established trajectory cost.
- **r166 distribution-invariant long shadow (2026-08-26):** the attempted
  transported/material baseline is rejected.  It would add a new advected
  field solely to answer a stability question that order statistics answer
  without new state or transport machinery.  The Eulerian per-cell
  `abs(terminal-beginning)` observable keeps exactly its narrower authority:
  it may constrain a following timestep only when the beginning deviation is
  bit-exact zero or the transport velocity is bit-exact stationary.  A moving
  accepted state remains payload-authenticated, but publishes zero for that
  unauthoritative timestep operand.

  Long-shadow boundedness is evaluated on the spatial distribution of
  `abs(EOS volume ratio - 1)`.  Every accepted step records maximum, p95, and
  p50.  Maximum retains the existing atomic reduction; p95 and p50 are exact
  Binary32 order statistics selected on Metal by a two-stage 16-bit radix
  histogram.  Only the three scalars cross the diagnostic boundary; the
  deviation field remains resident.  The r160 non-secular classifier is
  applied independently to all three 104-value trajectories, and both
  projections plus the per-step allowance/ceiling gates remain mandatory.
  A flat maximum with secular p95 growth is RED, including the prior-window
  outlier-masking case already covered by r160.  A translated fixed
  distribution is GREEN because all three order statistics are unchanged.

  The performance rung remains decoupled.  The legacy 45 ms remap check is not
  reinterpreted as a prerequisite for this physics campaign; retirement or
  re-pinning belongs to the post-source-map device-critical-path campaign.

  The first on-device campaign accepted seven production steps and exercised
  the three distribution reductions, but it did not reach the 104-step
  classifier.  At slice 7 the equal-time Binary64 target generator failed its
  R1 solve at 8, 16, 32, and 64 reference substeps.  The final 64-substep
  attempt stopped at substep 8 with residual 0.0191989 against tolerance
  0.000479545.  This is an equal-time reference-schedule capacity stop, not a
  secular-plateau verdict: the three trajectory classifiers were not run, and
  B_fp32 plus every later rung remain blocked.  The durable r166 artifact and
  raw transcript bind the seven accepted max/p95/p50 observations and the
  complete 8 -> 16 -> 32 -> 64 failure sequence.
- **r167 closure-convergence ruling (2026-08-26):** r166 resolves the prior
  dose-only model.  Production generation is modeled as
  `dose(delta_t) + feedback(deviation)`.  The six evolved r166 pairs give
  feedback slope `0.7925875134206254` and Pearson `0.7129565317645592`; the
  seven accepted fields hover at percent scale while delta-t contracts
  `148.6430318897442x`.  Together with r145's
  `0.9902699302058174` retained fraction, this records the operating-point
  error: initializing at the allowance seeded the near-unit feedback basin.
  Subdominance, not plateau-at-allowance, should have governed acceptance.

  The pre-registered deciding experiment runs the golden CFL request at fixed
  closure counts 1 through 8 in both Metal Binary32 and the same-scheme
  Binary64 mirror.  The identity-bearing tolerance is
  `0.00065 * 2^-3 = 8.125e-5`.  The curve first falls from
  `0.08589577674865723` to `0.06656908988952637`, then oscillates upward to
  `0.16477346420288086` at pass 8.  The mirror follows it at
  `0.08589567236102069`, `0.06656946601494673`, and
  `0.16477412949642256`.  The close fp32/fp64 agreement rules out an fp32
  floor; the multi-pass map itself is non-contractive and ends more than
  2,027x above tolerance.  The artifact binds the raw values and their
  log-scale SVG.  The zero-anomaly path remains byte-identical and
  terminates after one pass, while malformed pass identity fails before Metal.

  The geometric-convergence branch is therefore not selected.  No converged
  closure acceptance, no CFL restoration, and no replacement token policy are
  installed.  r167 is the genuine architecture stop required by the ruling;
  the hover-policy/reconstruction alternative must now be designed against
  the measured curve.  Pass-2 p95 is `122.15441651642323/141.788333 ms`
  device/wall and pass-8 p95 is `417.5576251000166/438.83425 ms`; those are
  diagnostic costs only.  The shadow, B_fp32, readmission, maps, and first
  light remain blocked.
- **r168 monitored-manifold production charter (2026-08-27):** the enforcement
  campaign is complete.  r167 proves the closure map non-contractive in both
  precisions; r162 locates the steep-state oracle generator in an approximately
  `1e-4 s` contraction class; r166/r167 measure feedback slope
  `0.7925875134206254`; and r160 enumerates every production consumer without
  finding an absolute P0-consistency/domain requirement.  Together these show
  that enforcing the absolute manifold makes production pay oracle-scale
  timestep or iteration cost, defeating the chartered two-tier split.

  Production therefore returns to monitored-manifold policy.  Every accepted
  step records device-reduced max/p95/p50 absolute EOS deviation and the
  allowance/`2^-5` threshold-crossing events.  Those are SHA-bound fidelity
  diagnostics and never stalls.  Absolute-reference restoration, anomaly
  closure, and the manifold timestep limiter default off but remain executable
  instrumentation.  The tangent `S_div` physics is unchanged.  A monitored
  accepted observation carries authenticated state/payload authority but zero
  manifold timestep authority, so CFL/buoyancy/diffusion/growth continue to
  own production selection.

  Conservation ledgers, r60 Binary32 affine feasibility, projection
  validation, CFL, nonfinite detection, and admissibility are not demoted.
  They remain atomic rejection gates and are independently revalidated before
  a token is issued or a result is applied.  Requesting enforcement without
  diagnostic monitoring fails before Metal.  The Binary64 mirror and roundoff
  trace follow the same one-projection default, rather than silently comparing
  against the retired restoration topology.

  The retained one-step GREEN runs at represented CFL
  `0.0016462659696117043 s` and deliberately measures
  `max=0.08589577674865723`, `p95=0.0007141828536987305`, and
  `p50=1.1920928955078125e-7`: both legacy markers cross, yet the physical
  projection validates, restoration count is zero, the state is authenticated
  and applied, and exact exit `195` records acceptance.  Equal-time oracle
  work remains a short-horizon contract fixture and is excluded from
  production timing.

  The completed 104-step shadow has no secular/plateau pass condition.  Every
  physical projection validates, restoration runs zero times, and all 104
  accepted states are authenticated and applied.  The absolute-deviation max
  peaks at `1.423297643661499` and ends at `0.8262996673583984`; p95 peaks at
  `0.0011827945709228516` and ends at `0.00028055906295776367`; p50 peaks at
  `2.574920654296875e-5` and ends at `9.238719940185547e-6`.  Both historical
  markers cross on all 104 steps and remain diagnostics.  The complete
  trajectory trace is
  `d468729944d82973293ff2afa3f8248be10be3f16f875f77e292805702500d8c`.

  Contact with the first evolved Binary32 state exposed one last inheritance:
  `AdvanceConservative3D` still applied the oracle's absolute-P0 detector while
  forming a production target, and its coupled Picard step did not contract
  through `dt/64`.  The shadow therefore owns an instantaneous tangent target
  from each accepted production beginning, using the existing certified
  temperature inversion, molecular transport, boundary flux, and full
  thermal-expansion `S_div` kernels without advancing an oracle state.  This is
  not an equal-time fidelity reference; the equal-time oracle remains unchanged
  for short horizons.  A pressure-only RED distinguishes the demoted oracle
  detector, while an affine-row RED proves r60 remains fail-closed.

  CFL/buoyancy/diffusion/growth selection is rerun at each accepted beginning.
  It starts at `0.0016462659696117043 s`, averages
  `0.0007690361974779919 s`, and ends at `6.441490404540673e-5 s` as physical
  maximum velocity reaches `219.97698974609375 m/s`.  Device/wall p95 are
  `91.62970818579197/179.505958 ms`, projecting to
  `0.8274219341607761/1.6209499069950497 h` for tier-10 x 25 s.  The requested
  `0.4-0.6 h` wall expectation is not met; performance remains decoupled, with
  the physical-CFL collapse recorded as the controlling cost.

  Long-horizon fidelity remains the original statistical
  contract: empirical rows, filtered fields, and tier-10 prefix integrals.
  Failure there may reopen enforcement only when the monitored trajectory
  attributes that measured failure to manifold drift.  The ladder resumes
  with short-horizon equal-time B_fp32/subdominance, readmission, thermo/source
  maps, and first light after this shadow is sealed.
- **r169 outlier-bounded monitored manifold (2026-08-27):** r168 closes the
  r160 enumeration gap.  No production consumer needs absolute `P0`
  consistency, but density/temperature inconsistency feeds `M/rho_g` velocity
  and buoyancy; at order one it caused the observed 220 m/s/CFL-collapse chain.
  Production now derives, before Metal, a conditional restoration target only
  for cells beyond `|V-1|=2^-3`, opposing the signed excess and leaving every
  other cell at exact zero target.  The existing second projection runs only
  for a nonempty tail.  Count, excess sum, and exchanged volume are bound into
  accepted authority.  A beginning above `2^-2` rejects before Metal; a
  terminal crossing withholds authority and the ordinary API rejects
  atomically.  These are stability-class constants, not tolerances widened to
  accept a measurement.

  The r168 replay crosses `2^-3` at step 16 and 8.47 m/s, whereas 100 m/s is
  first reached at step 81.  r169 engages at step 17 and 8.75 m/s, satisfying
  the causal RED.  Threshold zero is the sealed r166 bulk-restoration mutant
  and retains the 0.7926 feedback/hover signature.  Nevertheless the requested
  GREEN fails: 33 steps accept, 16 use targeted restoration, velocity stays
  below 10.70 m/s, but the accepted max reaches 0.23147 and step 33 realizes
  0.25728172063827515.  Both projections validate; the hard dynamics bound
  alone refuses, no token is minted, and the ordinary result is default.  The
  accepted prefix records peak tail population 102 and total exchange
  `2.3257764777146186e-4 m^3`.  The measured prefix p95 is
  110.68/146.90 ms device/wall, but its conditional 0.49/0.65 h extrapolation
  is not a 104-step claim.  This is the pre-registered genuine finding, so the
  shadow and every later ladder rung remain blocked.
- **r170 two-dose tail margin (2026-08-27):** the policy is unchanged, but its
  margins are corrected from the r169 measurement.  Engagement moves from
  `2^-3` to `2^-4`: `2^-2 - 2*0.09 = 0.07`, so the dyadic stability coefficient
  reserves at least two worst measured local doses beneath the unchanged
  `2^-2` dynamics bound.  Cells drain only their signed excess above `2^-4`;
  the exchange measure remains a tail sum, not global restoration.

  A hard-bound terminal crossing now carries a device-reduced suggestion based
  on local dose/headroom and enters the existing 20-attempt ordinary rejection
  classifier.  This reduction is justified because local advective dose scales
  with `dt`; it does not revive the retired global manifold limiter, whose
  feedback floor was measured timestep-invariant.  The r169 state reproduces
  its exact candidate-zero refusal under the retired threshold, and a second
  retained RED takes that refusal through candidate one at
  `0.0011418721405789256 s`, accepting at `0.24757766723632812` with both
  projections valid.

  The new policy completes all 104 steps with zero hard-bound retries.  Step 33
  accepts at `0.14402782917022705`; the campaign peak is
  `0.15430498123168945`, peak velocity is `10.871506690979004 m/s`, and all 104
  physical plus 100 conditional restoration projections validate.  Tail
  population peaks at 9,698 and total exchanged volume is
  `0.063814808515304383 m^3`.  Device/wall p95 are
  `112.8718750551343/149.257125 ms`, projecting to
  `0.5996346149904227/0.7929321510804586 h`; the wall result is reported above
  the forecast rather than adjusted.  Trace `e3273037...07fa` and final state
  `b885eec0...ce3b` seal the shadow.  The ladder is unblocked only to the next
  golden-slice subdominance rung.
- **r171 golden-slice subdominance and guard supersession (2026-08-27):** the
  unchanged golden root and seven deterministic current-build continuations
  are independently SHA-bound by canonical physical-state/lifecycle bytes
  before the admitted measurement.  Whole checkpoint bytes are not used as
  continuation identity because their executable digest changes on relink;
  the root remains independently whole-file gated.  The missing
  historical r138a payload bytes are not silently substituted and their old
  hashes remain historical.  A dedicated exact generator owns the current
  selection; an earlier root-only diagnostic is discarded.

  The monitored golden states have an empty `2^-4` tail, so the same-scheme
  comparison is one 16-cycle terminal projection in both Metal fp32 and the
  strict fp64 mirror.  The mirror also retains the conditional two-projection
  path for a nonempty tail.  All 152 filtered-scalar, filtered-velocity, and
  inventory inequalities `|P32-P64| <= 2^-3 E_P` pass.  Maximum velocity
  delta is `3.2429213131399156e-9 m/s` against the derived
  `7.363073950686612e-4 m/s` term (`227050.65093168768x` margin); minimum
  scalar and inventory margins are `7972.9650118056461` and
  `8894.785377013457`.  Trace `1e48343a...00b61` binds all values, bounds,
  margins, and topology.

  The preliminary `3e-5 m/s` velocity guard is superseded by the derived
  `7.363073950686612e-4 m/s` B_fp32 term.  The bound comes from distance to the
  mutual limit rather than the observed fp32/fp64 delta, so this is protocol
  completion rather than measurement-driven widening.  The additive
  contract's last term is closed; equal-time eight-slice readmission is next.
- **r172 tier-6 temporal-refinement stop (2026-08-27):** the r139 temporal
  protocol is finally executed as the prerequisite embedded by the equal-time
  amendment.  The no-Metal calibration owner generates three oracle target
  schedules as write-once payloads; a manifest, each payload, the r139 analytic
  beginning SHA, and the consumed all-cell Binary64 producer class are checked
  before const replay in the Metal-capable evaluator.  Penultimate-target and
  producer-class mutations are RED.  The generated production mirror uses binary64 stage
  arithmetic with the production binary32 publication boundary between
  substeps.  Eighteen of nineteen quantity classes contract and obtain their
  registered temporal terms.  Production sensible energy is first order with
  `E_dt=2.7328226439472538`, and the production/oracle velocity terms are
  `1.5098888236479335e-5/4.1666621096787029e-5 m/s`.

  Oracle sensible energy is the sole refusal:
  `D(dt,dt/2)=0.0012312438866646748` while
  `D(dt/2,dt/4)=0.001273209006325096`.  The ratio
  `0.9670398815497335` is noncontracting, so r139 forbids a temporal order,
  distance, fallback, or post-observation tolerance.  Exact exit `193` seals
  the complete matrix and requires the exact refusal counts `0/1`.  The additive contract therefore remains incomplete;
  the eight-slice verdict, manifold-floor attribution, thermo/source maps, and
  first light do not run.  The unchanged golden checkpoint remains
  `1b944176...4947`.
- **r173 filtered temporal-floor protocol (2026-08-28):** the r172 observable
  is confirmed to have already used r112's SHA-bound tensor cubic B-spline at
  the exact physical width before differencing.  Its `0.9670398815497335`
  ratio and near-equal filtered differences are therefore classified as the
  dt-independent front branch-flip floor anticipated by the r123 census,
  while V2's formal-order gates own temporal consistency.  Before rerunning,
  r173 freezes two outcomes for the observed r172 tuple: contracting
  differences use Richardson; only the exact oracle sensible-energy pair
  `(0.0012312438866646748, 0.001273209006325096)` may use
  `nextUp(max(D_coarse,D_fine))` as an upper bound on the measured filtered
  floor.  Every other noncontracting pair refuses.  The authorized quantity
  is an identity-bearing bound, not a generic fallback or fitted order, and
  adds no constant.
  The exact rerun accepts every channel.  Oracle sensible energy alone uses
  the floor branch, with bound `0.0012732090063250962`; the other 37 terms use
  Richardson across 19 paired production/oracle rows.  Refusal counts are
  `0/0`, so the temporal contribution to the
  equal-time additive contract is now complete.  Artifact and raw transcript
  are retained under `r173_filtered_temporal`; the eight-slice verdict remains
  the next unexecuted rung.
- **r174 equal-time eight-slice readmission (2026-08-28):** the complete
  additive contract is applied uniformly to the eight SHA-sealed r171
  beginnings.  The r112 tensor cubic B-spline and physical width precede every
  difference.  Production takes one represented step; the Binary64 reference
  takes seven nominal eighth-steps plus a positive final representable
  remainder, so its accumulated endpoint and terminal target are exactly the
  production endpoint.  The r112/r139 evidence owns worker-count identity, so
  this admitted measurement does not repeat the retired serial-oracle copy.

  All 152 scalar, velocity, and inventory inequalities pass.  Maximum contract
  ratios are `0.0018329622432418256`, `0.0081254983789433733`, and
  `0.0032325000146012773`; the original `83x/28x/8.7x` excess classes now pass
  under the completed equal-time contract.  The manifold-floor fidelity flag
  is evaluated and not implicated.  Exact exit `188`, trace
  `75817882...cfc19`, and the complete transcript are retained under
  `r174_equal_time_readmission`.  Thermo/source maps and first light are now
  next; neither is claimed by this entry.
- **r175 Binary32 thermo/source maps and preview release (2026-08-28):** the
  source boundary is promoted from exact `+0` to a certified Binary32 packet.
  Its `128 eps32` source term brings the complete producer union to
  `960 eps32`, still below the existing `1024 eps32` r60 envelope.  Mass,
  affine rows, frozen ledgers, representability, terminal admissibility,
  projection validation, CFL, and NaN gates remain fail-closed.

  At the sealed golden beginning and audited CFL step, the canonical ignition
  map selects 219 source cells and realizes `15289.762218506474 W`.  The
  independent fp32/fp64 source term passes `2^-3` subdominance with a minimum
  `112.55273459563601x` margin.  The production step accepts with terminal
  deviation max/p95/p50 `0.085888981819152832 / 0.00071436166763305664 /
  1.1920928955078125e-7`, below the `2^-2` dynamics bound.  Because the sealed
  beginning has no tail outliers, only the terminal projection is invoked and
  validates.  The next accepted beginning closes the burning-tail check: one
  cell engages, `3.4288999032069217e-7 m3` is drained, both projections
  validate, terminal max is `0.079523563385009766`, and the hard bound remains
  clear.  The paired tier-10 x 25 s projection is
  `0.40872554073287515/0.63840048687616735 h` device/wall.  Exact exit `184`
  binds both source/target/schedule records and the one-step causal tail
  transition.  First light is authorized only as `preview_primary`.

  The released tier-6 capstone reaches `2.3170101413580206 s` in 523 accepted
  steps, with a `2284.8533 K` pilot-off physical peak and a `1784.60397 K`,
  `1.27718934e7 W/m3` terminal state.  The primary EXR is
  `rendered/fire_production_first_light/r175_preview_tier6/methane_preview.exr`
  (SHA-256 `bf02ffa4...82c9`), accompanied by canonical CBOR provenance
  (`5e6187b9...44e`).  Eight scene-linear EXR frames, each with a
  canonical sidecar, are the animation's `preview_primary` evidence.  The
  visible blue-plume PNG (`53b0cb29...3b3`) and looping 8-frame/8-fps
  ImageIO GIF87a (`994a5b1a...77f9`) are correctly classified as
  `display_derivative` and
  linked to those primaries.  They use a display-only +6 EV ACES-to-sRGB
  transform.  The preview-only volume spatializes the sealed reaction and
  thermal-excess fields while preserving their recorded maxima; that
  visualization mapping remains distinct from the physical source-map and
  checkpoint claims.  The headless AVFoundation route returned
  `AVErrorCannotEncode`, so no MOV is claimed.  Publication now waits for and
  verifies complete fresh artifact/sidecar pairs in a unique stage for every
  requested frame; the stale-prior identity RED and sidecar-first
  delayed-artifact REDs prevent the former silent-success race.  The committed
  sidecars record source revision `f3b56b90...6e30` and dirty diff
  `a82c5699...348c`; evidence binds those exact producer facts.
  The animation is explicitly a preview camera move: the same terminal
  monitored state is rendered through a sealed 20-degree orbit and dolly.
  All eight frames contain a bounded blue plume, every adjacent decoded frame
  differs, the lit-area range exceeds five percent, and the lit-mask centroid
  moves by at least two display pixels.  The rejected seven-black-frame/
  one-plume schedule and static-geometry digest/area-flicker schedule are
  retained as classifier REDs; no eight-timestep physical evolution is claimed.
- **r176/r177 true temporal tier-6 preview and puffing-spectrum diagnosis
  (2026-08-29):** the tier-6 production trajectory completes the full
  `5 t_ft` discard plus 40 expected puffing periods, publishes 235 physical-time
  frames at `0.0625 s`, and produces the first truthful temporal fire preview.
  The follow-on spectrum publishes both complete signals, not only a selected
  peak.  Centerline heat release is the full-height cell-volume integral averaged
  over the central two-by-two columns.  Display lit area counts pixels whose
  maximum sRGB channel exceeds 127.  Each irregular signal is linearly resampled
  to 512 points over its exact observation span, least-squares affine detrended,
  Hann-windowed, and evaluated by direct one-sided DFT.

  Both observables peak at `0.13648504273426187 Hz`.  The expected
  `2.7386127875258306 Hz` component is present at the nearest
  `2.7297008546852375 Hz` bin but subdominant: expected-bin/peak power is
  `0.0013755911450037712` for centerline heat release and
  `0.001295326657846992` for lit area.  Integrated `2.5--3.0 Hz` power fractions
  are `0.0113695656449402` and `0.002445681094526029`.  The verdict is therefore
  “present but subdominant to a slow domain mode,” not “absent.”  This is also
  the pre-registered resolution outcome: r57 admitted a general case at four
  burner cells, tier 6 resolves the `0.30 m` burner with
  `7.3545957039557281` cells, and the puffing row was reserved for the refined
  approximately-ten-cell tier.

  The cost audit corrects a second inherited extrapolation.  The accepted
  tier-6 run took `8388.969329416 s / 15662 = 535.625694123 ms` per step, not a
  900-ms rendering charge.  Resident attempts averaged `115.223978036 ms`; the
  remaining `420.401716087 ms` was owner-side preparation and state work.
  Durable VDB output is only `34.131667 ms` p95 per emitted frame, so in-loop
  frame publication is not the step-cost driver.  There were 7,183 physical
  projection retries, but their aggregate remains inside the measured resident
  term; hard-bound retries were zero.

  A complete tier-10 owner profile then exposes request layout (`628.994 ms`),
  target formation (`306.259 ms`), and resident publication/application
  (`492.809 ms`) inside a `2008.334 ms` cold baseline.  Fixed-partition request
  construction, canonical-temperature reuse, and molecular-property reuse are
  scheduling/staging changes only.  Their REDs compare the full request bytes,
  all 976,272 stored/reinverted temperatures, and the complete divergence-target
  vector against the former paths.  After warmup, nine steps average
  `1116.682666667 ms`; request layout is `99.410111111 ms`, target formation
  `199.215777778 ms`, and resident publication/application `430.550444444 ms`.
  The honest 25.156864-s tier-10 projection is therefore `4.74005935697 h`
  before later-state retry cost.  The older `0.6384 h` value measured only a
  resident slice and is retired as a complete-run projection.  The tier-10
  physics run remains authorized, now under this complete-owner cost account.
- **r178 tier-10 density/velocity physics stop (2026-08-29):** the authorized
  tier-10 full-window run does not reach its `5 t_ft` discard or statistics
  interval.  At accepted step 1,542 and `2.1426204254821641 s`, the recoverable
  format-13 checkpoint records a minimum accepted step of
  `3.0300463549792767e-5 s`.  A read-only checkpoint audit finds
  `255.64169311523438 m/s` maximum physical velocity, giving an advective CFL
  candidate of `4.786874268372288e-5 s`.  The maximum positive reduced gravity
  is elevated (`62.179819320204423 m/s2`) but its independent buoyant candidate
  is `0.01402869021009805 s`, so buoyancy selection is not the direct limiter.
  Gas density spans `0.15965474117547274--1.2099052290432155 kg/m3` around the
  `1.1719579191113527 kg/m3` ambient value.  Together these identify the causal
  bundle provisionally as density corruption coupled to momentum growth,
  producing the 255-m/s field and collapsing the advective step.  r179 below
  supersedes the narrower implication that division by density alone caused
  the velocity: the stored momentum/velocity compatibility identity closes.

  The combustion ledger does not explain the failure.  A Binary32 source probe
  at the checkpoint produces `94466.977979105024 W`; methane consumption times
  LHV is `94467.009612291862 W`, relative error
  `3.3485973103748931e-7`.  The manifold distribution is already broad
  (`max/p95/p50 = 0.11917137460802585 / 0.009886252187254363 /
  1.1382639254042815e-5`), supplying the requested attribution evidence.
  Because failure precedes the observation window, no tier-10 puffing spectrum,
  empirical-row claim, or animation is formable.  The run was stopped rather
  than extrapolated into a multi-day invalid trajectory.  Format 13 does not
  persist retry counters or per-step cost histories; r178 records that omission
  and does not invent later-state wall statistics.  The tier-6 full spectra and
  r177 complete-owner cost remain valid, but the pre-registered tier-10 branch
  is the genuine production-physics stop.
- **r179 r178 momentum decomposition (2026-08-29):** the recoverable r178
  checkpoint is audited before any restoration policy change.  The minimum
  density cell is `0.15965474117547274 kg/m3` at `1981.9763228118441 K`, with
  signed manifold deviation `-0.060475840911561773`; it is not a
  manifold-consistent low-density state.  At the `255.64169311523438 m/s`
  face, adjacent states are `0.23534662136808038 kg/m3, 1614.7138732295894 K,
  +0.11917137460802585` and `0.21136353025212884 kg/m3,
  1643.7593009811521 K, +0.02378781404850927`.  The face carries
  `57.098869323730469 kg/(m2 s)` at `0.22335506975650787 kg/m3`; the checkpoint
  maximum `|M-rho_f u|` residual is only `9.639436029829085e-6`.  r178 is
  therefore reclassified as coupled off-manifold density and real physical-
  momentum runaway, not a stale momentum/scalar pairing and not pure density
  division.

  An eight-accepted-step Metal replay pairs every normal targeted-restoration
  attempt with the identical request under the existing restoration-removed
  diagnostic.  Transported-dual momentum bytes must match before comparison.
  Over represented `dt = 4.7868743422441185e-5--6.0848105931654572e-5 s`,
  physical velocity is `204.94--254.58 m/s`; restoration changes it by only
  `6.11376953125--12.380699157714844 m/s`.  The maximum correction/physical
  ratio is `0.058162335108278389`, already below the proposed `2^-3 = 0.125`
  cap.  Maximum restoration impulse is `0.17676903757991816` of the physical-
  projection impulse, and terminal compatibility residual remains below
  `1.876e-6`.  Restoration neither dominates nor violates the proposed cap.
  The pre-registered cap branch is rejected as a slack no-op; its r159 lineage
  remains true—excluding per-step-finite correction velocity from CFL was
  necessary, but this replay shows that correction impulse is not the runaway
  owner.  Tier 10 stays blocked pending a physical-projection/preprojection
  momentum budget; the requested r178-cap, uncapped-runaway, and r170-shadow
  RED campaign does not apply because no cap is landed.
- **r180 force-inclusive projection budget and matched-state preservation stop
  (2026-08-29):** an eight-step replay at the r178 checkpoint decomposes every
  vertical face in runaway column `(38,42)` into stress, buoyancy, advection,
  source, physical-pressure, and restoration rates.  The operator schedule is
  not the proposed defect: force-inclusive momentum is remapped, receives the
  face-source impulse, and is physically projected in the same step before the
  restoration projection.  Column pressure-gradient maxima are
  `1.49526e5--2.66097e5 kg/(m2 s2)` and advection maxima are
  `7.99667e4--1.76610e5`; buoyancy is only `9.482--9.587`.  The pre-registered
  pressure-under-response premise is therefore false, so neither a second
  projection nor a tighter tolerance is authorized.  The preserved oracle
  console proves samples near `2.1 s` existed but contains no field state; the
  current oracle checkpoint is the later step-3,479 state at
  `2.8854439500002069 s`.  Rather than manufacture a matched comparison from
  that later state, r180 records a preservation stop.  Full density/species,
  staggered-momentum, and projection state near oracle steps 1,100--1,120 must
  be recovered or regenerated before one term can be named as the cross-solver
  defect.  No production fix, tier-10 continuation, spectrum, empirical row,
  or animation is claimed.
- **r181 retained onset and resolution diagnosis (2026-08-29):** the production
  capstone is rerun from zero with an accepted-step max-velocity trajectory,
  immutable periodic checkpoints, and one-shot `(38,42)` column budgets at the
  first 15/30/60-m/s crossings.  The campaign itself catches and fixes an
  instrumentation false stop: the 15-m/s budget had been published before the
  controller consulted a still-false step acceptance flag.  Budgets are now
  accepted-attempt-only; resident intermediate projections are compared by one
  restoration-disabled run through the same Metal path, never a CPU re-solve.
  With that repair,
  tier 10 goes `16.1409645 -> 30.4028950 -> 63.6867218 m/s` between
  `2.1312820005` and `2.1378899626 s`, while represented dt falls
  `0.870582 -> 0.209345 ms`.  At the same velocity-owning face, advection grows
  `1.08176e3 -> 3.61668e3 -> 1.46489e4 kg/(m2 s2)`; pressure opposes it at
  `-0.75252e3 -> -2.57092e3 -> -9.90979e3`, so projection under-response is
  not the observed class.  Restoration is secondary.  Vreman is nonzero and
  rises to `0.0166124 m2/s`, but stress remains only 1.38--1.74% of advection.
  Tier 6 completes 2.2 s with a `13.5221453 m/s` peak.  The optional tier-8
  control stays below `10.2921 m/s` through `1.82649 s` even while its scalar
  tail begins dynamics-bound retries; its aligned advection/pressure/
  restoration rates are `397.969/400.239/-273.497`, separating that policy
  event from the tier-10 momentum runaway.  This resolution split
  selects the pre-registered advective-focusing/remap-dissipation class, not a
  viscosity knob: the r63 `tau` lesson applies equally to a dissipation
  coefficient chosen to suppress one observed trajectory.  The binary64
  reference is independently regenerated with retained checkpoints and exact
  R0/R1 momentum operands.  Immutable step 1,024 at `2.1079791976 s` stays at
  `4.89--5.06 m/s`; its largest aligned advection is `63.3396`, versus
  production's `1,081.7586` at the first 16.14-m/s crossing (17.08x), while
  pressure is active in both.  The oracle velocity face has alpha one, but the
  same column contains scalar-front alpha cuts down to `0.2337--0.3894`.  A
  coefficient-free front-coupled dual-remap trial therefore ran from zero.  It
  delayed onset to `2.14092 s` but still crossed `15.16/32.17/61.40 m/s` by
  `2.14853 s`; a reconstructed velocity-envelope trial changed the terminal
  time by only `1.45e-8 s`.  Both candidates are rejected and removed from
  ordinary production.  The r181 finding is now flux-level: cell-average
  limiter and ratio caps do not control the tier-10 conservative advective
  focusing mode.  The next architecture is an onset-derived conservative flux
  correction or adaptive front/column dissipation, validated against the
  retained matched oracle state rather than tuned as a viscosity constant.
  Oracle regeneration stops honestly at `2.15671067 s` / step 1,070 on its
  temperature/Picard-Zeno boundary, so 2.2 s is not claimed.  Spectrum,
  empirical rows, animation, and report remain blocked behind that flux-level
  remedy.
- **r182 §3.7 compatible momentum conformance and corrected onset stop (2026-08-30):**
  the retained diagnostic candidate's five scalar palindrome passes retain the
  exact accepted Binary32 gas-mass face dose after the shared-alpha limiter.
  Momentum and its staggered auxiliary density consume those same Private fields
  through the boundary-aware MAC restriction, with
  `K_i=I_i(Phi_hat_g)*(u_i,L+u_i,R)/2`.  This is coefficient-free oracle
  lineage: momentum inherits the scalar front's limiter dissipation, and the
  commuting identity preserves a uniform velocity to roundoff.  Pressure-open
  and wall faces require half of a transverse interior dose because their
  ambient half is fixed; a new boundary RED closes that restriction and
  supersedes the erroneous full-dose v1 experiment.  The five retained fields
  exist only behind an explicit diagnostic API: ordinary production allocates
  none and no process environment can activate the candidate.  The diagnostic
  passes the resident request by reference and adds no host-side full-payload
  copy; its working-set certificate therefore covers the added Private Metal
  fields without omitting an extra host replica.  The old
  independent high-order momentum reconstruction is the focusing mechanism;
  r181's scalar-alpha cap and reconstructed velocity-envelope cap remain
  rejected.  The compatible path is cleanly expressible in the resident dual
  architecture (one command, 15 logical submaps, zero host transfer), so the
  fallback alpha-coupled momentum limiter is not selected.

  The original r182 comparison is withdrawn: it followed the moving production
  maximum rather than the oracle's fixed `(38,42)` column, used a terminal
  velocity as a trajectory maximum, and carried the bad open-boundary
  restriction.  Fresh review also proved the first “corrected” from-zero file
  was byte-identical to r181 ordinary production and its continuation switched
  operators only after step 1,400; those hybrid claims and files are withdrawn.
  The replacement run activates the compatible API from the analytic beginning
  and SHA-binds mode, producer build/executable, trajectory, and final
  checkpoint.  It crosses 15 m/s at step 885 / `1.3978566413 s`, with aligned
  advection `914.3010693`.  Step 1,222 is the campaign's configured first
  accepted `>=60 m/s` stop.  Seven hard-bound retries precede that accepted
  state, with represented dt `4.21569474e-10 s` and velocity `4,768,055 m/s`;
  no next-step CFL selection was measured.  Its pressure-gradient and
  restoration maxima (`1.0998092339e16` and `1.6466909209e15`) dwarf its
  `282.7749564` advection maximum, so this terminal retry/projection event is
  not used as an advective-mechanism claim.  It stops at `1.7316493935 s`,
  short of the sealed `2.2 s` target; the earlier 15 m/s budget and the fixed
  matched-column comparison carry the compatible-form rejection.

  The full pre-registered criterion still does not pass.  A caller-SHA-bound
  step-1,400 checkpoint is replayed for 28 steps at the fixed `(38,42)` column.
  Production at `2.10773021 s` has maximum aligned advection
  `321.3419554 kg/(m2 s2)`; the nearest retained oracle beginning at
  `2.10797920 s` has `159.0089232`, a 2.0209x ratio.  The previously cited
  `63.3396109` sample belongs to `2.11358242 s`, not the claimed matched time.
  Thus neither oracle-class compatible balance nor sustained onset health is
  recovered.  The
  tier-6/tier-8, r170-shadow, and readmission obligations are not
  promoted from the invalid v1 run, and all
  tier-10 window/spectrum/row/animation/report work remains blocked.  r182 is a
  measured compatible-flux residual-balance finding, not a successful
  remedy claim.  The candidate is retained behind its exact diagnostic
  activation but is not adopted by ordinary production; the previous admitted
  remap remains the default.  The immutable r181 crossing transcript remains
  its unconformed-momentum RED.
- **r183 tier-8 stop and projected-Heun bootstrap (2026-08-31):** Track A starts
  the requested tier-8 full-window run but stops before the statistics discard
  ends.  It reaches step 1,980 / `2.70156268 s`, records 819 hard-bound rejected
  candidates and a minimum logged `Delta t=5.57543046e-7 s`, and produces zero
  frames.  The retained step-2,149 checkpoint measures `429.0182 m/s` at
  `(axis 2, 34,34,45)` with zero realized heat release, so neither a spectrum
  nor a temporal animation is claimed.  The exact tier-8 evidence artifact is
  inherited as Track A's stop rather than relabelled as a deliverable.

  Track B proves that the existing tokenless single-stage compatible-FCT
  diagnostic is both the wrong time tableau and far too expensive to promote:
  its first two tier-10 steps cost roughly `2.50--2.53 s` resident device and
  `3.09--3.17 s` wall each.  A source-stage refusal also catches an r60 affine-
  envelope ownership mistake; signed-mixture sensible energy now validates the
  already-admissible tuple without clipping its tiny negative roundoff.

  The r183 code change is deliberately a bootstrap.  It factors scalar FCT
  into source-free Build/Average/Solve operations with a fresh averaged-pair
  alpha, adds a `Delta t`-independent resident nonpressure momentum RHS, and
  adds projection ownership for sealed stage classifications, sealed
  integrated R0/R1 pressure-open head, and endpoint class publication.  CPU and
  Metal REDs bind source-once semantics, fresh-alpha identity, no interstage
  host publication, endpoint seed/head separation, and exact working-set
  accounting.  A reviewed standalone CPU prerequisite additionally publishes
  `f_N/J_g` from fixed binary32 stage operands: the `N_C N_C^T` projection is
  verified in fp64 with the record-derived forward-error envelope, binary32
  publication has its own bound, `J_g` is recomputed from the published bytes,
  PhysicalV1's inclusive `[300,5000] K` domain is enforced, open/wall/periodic
  boundaries are sealed, and `4(12C+11F)+B` is the exact pre-payload resource
  gate.  Metal is explicitly unsupported for this prerequisite rather than
  being credited with an fp64 identity it does not yet implement.

  Ordinary production remains on the admitted r182 predecessor:
  no projected-Heun owner is enabled, no onset success is claimed, and all
  tier-10 deliverables remain blocked.  `Phi_g` retention/FCT composition,
  packet-derived `S_div`, accepted-state EOS inversion, and caller-command
  projection/FCT encoders are the named missing seams before the full R0/R1/R2
  Picard schedule can be compared kernel-by-kernel against the fp64 oracle.
  This records progress without reviving the rejected form-level graft or
  laundering the single-stage diagnostic into §3.7 conformance.
- **r184 authenticated accepted-state EOS prerequisite (2026-08-31):** adds a
  CPU-only, identity-bearing inversion gate for the distinct `Q*` and
  `Q^(n+1)` roles.  Its bounds are derived by validating the canonical
  fire-case-v1 envelope against PhysicalV1; a syntactically plausible case ID
  or caller-authored `T_max` cannot authorize the capstone's strict `2300 K`
  ceiling.  The record-level arithmetic preserves signed envelope-negative
  constituents for sensible-energy inversion, uses positive-part gas density
  for pressure, and evaluates the published-temperature pressure ratio in the
  oracle's exact gas-density/molar-density/mean-weight operation order.

  Fresh adversarial review rejects the first draft's inherited `1e-3`
  production refusal.  r159 already proved that number is the oracle validity
  detector, not a production thermo-domain limit.  The corrected prerequisite
  records P0 deviation as identity-bound diagnostics and accepts a
  one-percent, r60-feasible scaling; stability/fidelity policy remains owned by
  the separately derived production contract.  Temperature-domain and affine
  infeasibility still refuse atomically.  The exact logical payload is `40C`
  bytes, near/over two-GiB neighbors are bound before conservative payload
  access, and the generated fp64/trace manifests now include their live
  FireSimulationRecords and FireCase dependencies.

  This closes only the CPU EOS prerequisite.  Metal inversion and
  producer-equivalence, `Phi_g/J_g` retention and averaging, packet-derived
  `S_div`, caller-command projection/FCT encoders, the complete R0/R1/R2 owner,
  onset revalidation, and all tier-10 deliverables remain blocked and are not
  claimed.
- **r185 exact scalar-flux composition prerequisite (2026-08-31):** adds the
  CPU-only, identity-bearing bridge from the source-free donor/MC pair and the
  certified physical producer to the exact section 3.7 retained stage. The
  first draft was rejected because it accepted a public mutable physical
  result as authority and would have reused the legacy compatible seam's
  `high-low` subtraction. The corrected composer consumes raw same-stage
  requests, requires byte-identical `Q/u/ambient/open/boundary` operands,
  invokes both producers internally, rechecks the physical affine bound, and
  recomputes `J_g` from components 1--6 in producer order.

  The persistent payload is exactly `30F`: composite low `9F`, advective delta
  `9F`, physical mass `8F`, physical energy `F`, `J_g F`, advective gas low
  `F`, and advective gas delta `F`. Physical flux is added to the scalar low
  pair once; the delta remains advective. R0/R1 stage role, attempt, raw-input,
  frozen-velocity, and content identities are sealed. Every one-ULP mutation
  of the retained classes refuses atomically. R0/R1 average all 30 fields
  independently and carry no alpha. A second review rejected pair-only solve
  authority, under-bound average lineage, propagated physical attestation, and
  caller-authored alpha. The corrected path requires identical shared FCT
  contracts, binds both parent composition IDs, derives new affine and `J_g`
  averaging bounds including binary32 publication error, and mints a
  fresh-alpha token from a solve that validates the R0 request and averaged
  stage. The average's three live payloads are `360F` bytes, with exact
  adjacent two-GiB shapes and mismatched-large-second preflight recorded.

  Compatible momentum gains a separate direct-delta CPU consumer computing
  `Phi_g^L+alpha*DeltaPhi_g+J_g`. A binary32 cancellation fixture proves the
  retained delta is not interchangeable with `(low+delta)-low`. Each original
  stage's velocity, parent, attempt, and fresh-alpha identities must match;
  mutated or stale alpha bytes refuse. The future owner must evaluate R0 and
  R1 momentum separately with the final alpha and average their rates. Hybrid
  periodic/nonperiodic topology remains fail-closed because that compatible
  kernel has no oracle. Metal
  composition, packet-derived `S_div`, the complete projected-Heun owner,
  onset revalidation, and tier-10 deliverables remain blocked.
- **r186 canonical frozen-source authority prerequisite (2026-08-31):** moves
  the canonical methane source chain out of the tool layer into one shared
  `src/Library` implementation and adds a non-inline compiled authority that
  alone can mint the opaque production seal.  A first validator-shaped draft
  was rejected: algebraic validation of caller-authored dose/pilot/radiation
  fields is not source provenance.  A second draft was also rejected because
  its complete friend class still had only inline definitions in the tools
  header, leaving the mint replaceable and the production library unresolved.
  The accepted boundary exposes raw authenticated `Q`, case, timing, pilot-mask,
  mixing, and radiation-policy inputs only; ignition, reaction, exact pilot
  `1/V'`, global radiation, ledgers, and expansion are derived internally.

  The source path uses r184's shared record inversion and therefore preserves
  its inclusive lower publication semantics rather than inventing a source-
  only rejection.  The strict case ceiling remains fail-closed.  Six parent/
  content identities bind the complete packet.  Worker scheduling is excluded
  from physical identity and serial/parallel packets compare byte-for-byte.
  Fresh review also exposed two general worker/resource defects: partial pool
  growth could retain threads after creation failure, and task exceptions
  escaped the worker.  Growth now commits atomically, exceptions propagate
  after the barrier, the pool is reusable, and worker count is topology-bounded.
  Because the global pool retains its high-water threads, every admission now
  charges conservative stack reservations for the full topology capacity plus
  an explicitly bounded case envelope.  Exact adjacent and high-worker-to-low-
  worker working-set tests and early over-cap REDs bind the certificate.

  This remains a CPU source prerequisite.  Packet-derived `S_div`, Metal
  command encoding, the coupled projected-Heun owner, onset validation, and
  tier-10 deliverables remain blocked.

- **r187 packet-derived divergence-target publication (2026-08-31):** retains
  the per-cell finite-volume expansion already computed by the canonical r186
  authority instead of asking a later projection owner to reconstruct it.
  The published binary32 target is exactly
  `(Delta t*S_thermo+I_pilot)/Delta t`; the source packet's pilot energy is
  excluded from `S_thermo` and its exact `1-1/V'` integral is added once.
  The `C` values enter packet-content identity and seal validation, and a RED
  reconstructs every packet from the sealed dose/pilot fields and requires
  bit-identical target publication.  The source working set is re-derived for
  `10F+8D` retained output.  Metal command encoding, projection consumption,
  the complete R0/R1/R2 owner, onset validation, and tier-10 claims remain
  blocked.

- **r188 authenticated base physical divergence target (2026-08-31):** adds an
  opaque CPU-only R0/R1 base-target producer. It rebuilds the r185 stage from
  raw operands, requires the r186 packet's exact source dose and attempt/shape/
  timestep parents, and implements the pinned split
  `dV(Q_stage)[D(f_N^stage)] + S_div^source(Q_n,Delta Q_source)`. The source
  term is the r187 beginning-referenced absolute finite-source target, frozen
  identically across R0/R1; the physical term is the exact record-owned EOS
  tangent at the current stage. A reviewed first draft that folded `D(f_N)`
  into the nonlinear finite-source map was rejected: that construction made
  the source contribution stage-dependent and did not implement the pinned
  tableau. Zero source dose deliberately leaves `(V(Q_n)-1)/Delta t` rather
  than deleting absolute restoration.

  The first implementation exposed a one-bit authority hazard: reinverting R0
  with the record's wider temperature bracket is not guaranteed to reproduce
  the source producer's case-bounded inversion. The source seal therefore
  retains its exact canonical beginning-temperature bytes by moving the
  already-live producer buffer; retained output becomes `11F+8D` and the exact
  source-build peak advances from `19F` to `20F` without a duplicate, while R0
  recomputes and matches the source beginning-state identity. R1 is role-
  separated and uses the shared record inversion for its current stage; its
  later projection eligibility still requires the Q-star accepted-candidate
  lineage.

  The publication identity covers target bytes, source packet, flux-stage
  composition, role, attempt, record, and maximum absolute scaled expansion,
  but no projection API consumes the seal. Independent tangent-plus-source
  reconstruction, distinct-stage R1, zero-dose/nonmanifold absolute-source,
  nonzero-physical-flux, stale-source, stale-temperature (including traced
  scalar equality), role-separation, and pre-payload two-GiB REDs pass. This
  closes only the base-target prerequisite:
  separately sealed r70 correction and terminal verification, Metal commands,
  the R0/R1/R2 owner, onset validation, and tier-10 remain blocked.

- **r189 authenticated projection target and Metal-host ordering (2026-08-31):**
  introduces the opaque iteration-zero target between r188 and projection. The
  r188 base seal now retains and identity-binds the boundary topology of the
  stage that produced it; the wrapper inherits that topology and has no caller
  boundary argument. A valid-but-different topology RED closes the mismatch
  found in review.

  The CPU consumer refuses a nonempty caller-authored target, then installs the
  authenticated bytes into a by-value projection request. Its matched-input
  result is bit-identical to the raw CPU projection oracle. Preauthored-target,
  valid-different-topology, and unsealed-parent REDs fail atomically. The raw
  function remains a public calibration oracle and carries no accepted-step
  capability; the claim is intentionally scoped to the authenticated consumer.

  Review also rejected a standalone r70 child producer: an admissible r184 EOS
  result lacks the immediate projection/flux parent and could be a stale but
  self-consistent Picard candidate. Correction therefore moves into the full
  owner, where its candidate can be constructed and bound rather than asserted.
  That owner must derive constant-nullspace compatibility from the absence of
  pressure-open faces, covering wall/periodic closed mixtures as well as fully
  periodic grids. Terminal Picard, R2, accepted-step authority, and device
  consumption remain unimplemented.

  Because the current host has no Metal device, r189 also publishes a SHA-bound
  run manifest rather than implying execution. Its binding order makes Track A
  tier 8 first on the returning M4 worker: full statistics window, full
  centerline/lit-area spectrum, preview-primary temporal movie, HDR `colr`
  verification, and immediate owner delivery. Track B's device kernel sweep and
  tier-10 onset follow, then the r170/readmission re-derivation and tier-10
  queue. Renders and simulations remain sequential. Manifest SHA:
  `c5dcaacd6c5f858fbaefaaa611c02fe213481a2dbde0ba45355c67b3a7ee42d5`.

- **r190 complete CPU R0/R1/R2 owner (2026-08-31):** moves the r70
  correction into the only scope that possesses its real parent.  The owner
  starts from raw `Q^n`, `M^n`, the opaque r186 source packet, and invariant
  scalar/force contracts.  Every Picard iteration first projects the fixed
  stage provisional momentum, then requests transport coefficients against an
  identity containing that exact projection, rebuilds donor/MC plus physical
  flux, solves the applicable shared limiter, and only then mints the next
  target.  The target-chain v2 identity binds its parent target, the exact
  accepted-candidate identity, and the correction ordinal.  The correcting
  authority is private and friends only the complete owner; there is still no
  standalone public r70 producer.

  Fresh review rejected the first draft's one-shot R2, averaged endpoint
  target, wrong-sign normal-only open head, first-class freeze, missing r59
  verification, correction-only mean removal, unchecked projection result,
  forgeable coefficient echo, zero-state mirror claim, and non-atomic result
  copy. The corrected owner removes the mean of the complete closed target,
  owns r80/r81 active-set history and r59 pointwise-infimum alpha acceptance,
  and refuses every failed projection validation.

  R0 publishes `Q*` and `M*dagger`; R1 publishes `Q^{n+1}` and the separately
  compatible R0/R1 Heun momentum. R2 fixes that state and provisional momentum
  while iterating `u2 -> f_N,2 -> S_div,2 -> projection`. Its integrated head is
  `-rho_ambient (I0 |u0|^2 + I1 |u1|^2)/4`, including tangential velocity.
  Source dose is applied exactly once; R2 has no scalar commit.

  `Begin` binds source/case lineage and preflights the combined live set.
  Coefficient identity covers projected state, temperature, velocity, and
  payload bytes. Candidate identity is recomputed at the private correction
  boundary. The terminal verifier covers the complete public payload, including
  every stage projection, flux, limiter, nonpressure rate, EOS temperature,
  target, diagnostic, nested topology/schedule metadata, and exact source
  packet. A fresh authority review found that direct aggregate assignment could
  partially mutate the caller on allocation failure. The amended publication
  path deep-copies into a private temporary, injects failure after the first
  payload copy, proves the caller is wholly default on refusal, and commits both
  internal and external publications only through statically non-throwing moves;
  the owner remains retryable after that refusal.
  A later fresh authority pass closed two remaining authority gaps. The result's
  acceptance identity is now a private owner-minted seal with a read-only
  accessor, so public payload bytes cannot be mutated and re-signed by replaying
  the visible hash algorithm. Each R0/R1/R2 call also enters an explicit
  in-progress state before invoking transport; scoped rollback restores the
  prior stable state on refusal, blocking callback reentry while preserving the
  ordinary retry path.
  The next fresh test/evidence pass made the atomic-refusal RED exhaustive over
  every nested EOS, projection, physical-flux, FCT, nonpressure, target,
  active-set, and publication field; `complete_default` no longer means a
  selected-field proxy. The same pass corrected the deferred Metal manifest:
  tier-8 and tier-10 spectrum commands carry an explicit tier that is checked
  against the checkpoint grid, and each animation stage now includes the
  headless Rec.709-linear-to-Rec.2020/PQ ProRes 4444 encode, the required `colr`
  atom probe, and a SHA sidecar binding the movie to every primary-frame
  provenance sidecar. These are queued commands only; no Metal execution is
  claimed on this host.
  Result consumption requires the expected sealed source, so an intact prior
  attempt refuses. REDs cover stale candidate, stale coefficient payload with
  current echoes, forged parent, stage order, closed constant mode, mutations
  of pressure/stage velocity/alpha/EOS temperature and nested metadata, the
  live `Begin` working-set refusal, every projection-validation branch including
  canonical cycle members in both R0/R1 and R2, exact refusal diagnostics plus
  state-preserving retry for stale/order/forged cases, an
  exercised two-class cycle, the nonuniform r59 selected-alpha path, and atomic
  retry. Active-set least-discrepancy selection reprojects every cycle member
  against one current target, applies the configured velocity deadband, and
  publishes the maximum discrepancy over the trajectory. The r59
  pointwise minimum is revalidated through r60 and the commuting identity, and
  the terminal Heun token carries that exact alpha. This proof remains local to
  the complete owner. r190 truthfully changes the r189 transport files to add
  the authenticated target-chain payload and compatible-momentum delta; those
  additions expose no standalone accepted-step authority. The complete owner
  declaration now exists at the target boundary, closing the incomplete-friend
  counterfeit found by fresh review.

  A source-active pressure-open `4^3` case uses a canonical one-ULP pilot packet
  at `dt=1e-4 s` and matches every projected stage of the independently
  implemented fp64 `AdvanceConservative3D` owner. State, velocity, and momentum
  remain inside a 64-epsilon binary32 forward envelope. The former local
  `2 dx^2 epsilon_Picard/dt` pressure claim is rejected because it omitted the
  discrete inverse. Independent oracle tolerances `1`, `1/16`, and `1/256`
  instead establish a converged reference: coarse/fine is
  `2.988941126e-4 Pa`, fine/finer is `4.302731804e-5 Pa`, and production/finer
  is `3.396462939e-4 Pa`. The pinned comparison bound is the one-bit-headroom
  dyadic envelope `2^-10 Pa`; it is explicitly a fixture measurement, not an
  operator-wide formula. The generated binary64 owner is retained as a separate
  all-publication roundoff check, including a nonuniform periodic fixture. The
  signed r60 gas-density differential rejects the former per-constituent clamp,
  and wall positive-zero checks inspect the complete binary64 word. Metal execution
  remains unclaimed on this host. The r136 source-bound trace digest moves from
  the historical `19371e6ef60fb1c78e6feeb0616b5952993ee375a7b9f7d97afd16b544182326`
  to `9c6f87644dcb221bca4ec1ceeb0e7e42f067131ba80e71b5edc0c04e297572ce`.
  This is a manifest re-derivation, not a numerical re-baseline: the encoding
  prefixes the Transport/Force source hashes, and a detached build of
  `0e70f17f1` reproduces the historical digest and exit 237 while the reviewed
  owner sources reproduce the new digest and exit 237. All 3,972,326 branch
  obligations, every frozen metric, and the `0xff` refusal are identical; the
  separately bound `r136_trace_repin_evidence.v1` preserves both digests and
  the comparison. Final review found that the digest prefix omitted the four
  dependency fields for `FireSimulationRecords` and `FireCase`; r190 now
  consumes all 16 generated manifest fields through a shared enumerator, and a
  per-field mutation RED proves each is identity-bearing. The same review
  corrected R2's trajectory diagnostic to score both bootstrap and iterative
  classes actually projected, with separate known-flip REDs, and extended the fp32/fp64 mirror through the complete
  R2 endpoint physical-flux payload and certificates. r189's tier-8-first
  manifest remains binding.
