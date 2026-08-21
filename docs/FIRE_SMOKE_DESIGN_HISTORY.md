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
  r120 trace executes all 24 frozen stages and binds exact digest
  `a4315505...5127f`, unresolved-branch bitmap `0xdffffe`, and
  invalid-denominator bitmap `0x1ffffe`. Force and exact-`+0` source addition
  are resolved; the 5 cell maps, 15 dual maps, and both projection passes have
  unresolved executed branch topology, and every transport map has an interval
  denominator whose lower enclosure crosses zero. The first overlapping branch
  and denominator witnesses are recorded numerically and independently checked
  by the topology walker. Per the predeclared r120 rule, the campaign stops
  before Metal, before fp32/fp64 measurement, and before any `B_fp32` radius is
  admitted. Temporal refinement, eight-slice readmission, and source maps did
  not run. The durable artifact is
  `r122_roundoff_derivation/roundoff_derivation_stop.v1`, SHA-256
  `e52afd58...d98f`. Continuation requires an explicit branch-stable or
  branch-equivalence ruling; no measurement may widen the absent bound.
