# Wrong-Topology Newton Convergences in Specular Manifold Sampling — Literature Survey

Research-only pass (2026-05) characterising how Mitsuba, pbrt, and the
post-2020 SMS literature handle the failure mode RISE calls **wrong-topology
phys-fails**: Newton converges algebraically (‖C‖ < 1e-4) to a chain with wi
and wo on the SAME side of the local face normal at one of the specular
vertices — geometrically a reflection masquerading as the labeled refraction
(or vice versa). Post-seed-overtrace fix
([SMS_SEEDING_OVERTRACE_FIX.md](SMS_SEEDING_OVERTRACE_FIX.md)) the residual
on a Gaussian-falloff bump is ~5 % of all converged chains, and a "smoothness
paradox" is observed: smoother painters give Newton an *easier* landscape,
yet phys-fail rate goes UP with smoothness.

## 1. Summary

The literature **treats wrong-topology rejection as a normal Newton-failure
mode and does not work around it specifically**. Mitsuba's reference SMS
uses an essentially identical predicate to RISE's `ValidateChainPhysics` —
sign product on the *geometric* normal — and on failure the trial is
discarded; the next Bernoulli iteration draws a fresh uniform sample on the
caster (no perturbation, no retry from the rejected seed). Zeltner 2020 §5
two-stage targets a different problem (smooth primitives + normal-mapped
BSDFs); silent on wrong-topology on displaced meshes, matching RISE's
measurements. pbrt-v4 has no manifold solver — caustics go through SPPM.
The "all-roots" alternative is **Specular Polynomials (Mo 2024)**:
enumerates every root of the half-vector polynomial system in closed form,
so the "wrong-topology basin" cannot trap a Newton walk because there is no
walk; each enumerated root is filtered against the same sign check Mitsuba
uses. **SMBS** (large-mutation rescue) and **Manifold Path Guiding**
(learned seed proposals) attack divergence and basin coverage
respectively, not wrong-topology.

## 2. Q1 — Mitsuba's wrong-topology handling

### Q1.1 The validation predicate

Mitsuba's single-scatter solver has the following check at the end of
`newton_solver` in `src/librender/manifold_ss.cpp` (around lines 340–355):

```cpp
Vector3f wx = normalize(si.p - vtx.p);
Vector3f wy = ei.is_directional() ? ei.d : normalize(ei.p - vtx.p);
Float cos_theta_x = dot(vtx.gn, wx),
      cos_theta_y = dot(vtx.gn, wy);
bool refraction = cos_theta_x * cos_theta_y < 0.f;
bool reflection = !refraction;
if ((vtx.eta == 1.f && !reflection) ||
    (vtx.eta != 1.f && !refraction)) {
    return { false, si_current };
}
```

The multi-scatter solver in `manifold_ms.cpp` has the identical predicate per
chain vertex:

```cpp
Float cos_theta_i = dot(m_current_path[i].gn, wi),
      cos_theta_o = dot(m_current_path[i].gn, wo);
bool refraction = cos_theta_i * cos_theta_o < 0.f,
     reflection = !refraction;
if ((m_current_path[i].eta == 1.f && !reflection) ||
    (m_current_path[i].eta != 1.f && !refraction)) {
    return false;
}
```

This is the same predicate as RISE's `ValidateChainPhysics`
(`ManifoldSolver.cpp` line 2064): sign product of (wi · n)(wo · n) labelled
against `vtx.eta == 1` (mirror) vs `vtx.eta != 1` (dielectric).

### Q1.2 Geometric vs shading normal

Mitsuba uses **geometric normal** (`vtx.gn`), not shading. RISE matches: see
the `hasGeom`/`nForTest` block at `ManifoldSolver.cpp:2097-2100`. RISE's
comment there explicitly cites Mitsuba as the reference. The shading normal
would over-reject grazing chains on triangle meshes where Phong-tilt
disagrees with the true face plane.

### Q1.3 Retry strategy on rejection

**None.** When validation fails, both Mitsuba solvers `return false`
immediately. Statistics counter `stats_solver_failed` is incremented and
control returns to the integrator. No perturbation, no different seed at the
same caster point, no fall-through to a different mode.

The retry is at the *outer* layer: the Bernoulli loop in
`specular_manifold_sampling` (`manifold_ss.cpp` ~line 41–157) calls
`sample_path` again, which redraws the seed from scratch via
`shape->sample_position(si.time, sampler->next_2d())` — a **uniform area
sample on the caster**, not a perturbation of the rejected seed. So the rate
at which Mitsuba converges to wrong-topology roots is exactly what's seen in
the Bernoulli `inv_prob_estimate` (1/p) variance.

### Q1.4 Special handling of "Newton converged but wrong topology"

**No.** The Mitsuba code makes no distinction between "Newton diverged",
"Newton converged but ‖C‖ above threshold", and "Newton converged with
‖C‖ ≈ 0 but wrong topology". All three return `{false, ...}` from the same
exit path. Topology rejection is just one branch in the same failure switch.

### Q1.5 Photon-aided seeding (PMS, Weisstein 2024)

Weisstein 2024 PMS deposits photons in a scene-prep light pass and uses them
as Newton seeds at render time. Photons are produced by a *physically traced*
emission walk, so each photon's chain is by construction topology-correct at
emission time. But once it becomes a Newton *seed* with anchors `(shading
point, light)` that may not match the original photon path, Newton can still
walk it into a wrong-topology basin — the same `manifold_ss.cpp` /
`manifold_ms.cpp` validation triggers, with the same `return false`.
Photons help with *basin coverage* (you start in a basin nearby a real
caustic), not with *wrong-topology rejection per se*. This matches RISE's
measurements in [SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md](SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md):
photons restore lost basins on displaced meshes but the residual phys-fail
rate stays in the same band.

## 3. Q2 — Zeltner 2020 paper, basin-aware solver, two-stage

The Zeltner 2020 paper PDF (rgl.epfl.ch / projects.shuangz.com) was too large
for in-line fetch in this pass. The substantive answers come from the
reference *implementation* (which is the paper's published §5 algorithm
embodied in code) plus the project page summary:

### Q2.1 Does the paper acknowledge wrong-topology convergences?

The paper does not name "wrong-topology" as a distinct failure mode in any
section we could verify. The reference code's existence of the per-vertex
sign-product check is the strongest in-codebase evidence that the authors
*knew* about it (otherwise the check wouldn't be there), but their treatment
is "reject and let the Bernoulli loop draw a new seed", not "diagnose the
basin and steer away".

### Q2.2 Two-stage solver scope (§5)

The reference code at `manifold_ss.cpp:234-246` (quoted Q1.5 of the upstream
inquiry) is unambiguous: two-stage uses **`bsdf->lean()`** — LEAN moments
(Olano-Baker 2010) — to derive a smoothed normal field from a normal map.
Stage 1 walks on the smoothed surface; stage 2 refines to the actual normal.
**This is for normal-mapped BSDFs on smooth analytic primitives, not for
displaced meshes.**

```cpp
if (m_config.twostage) {
    Point2f mu_offset(-n_offset[0]/n_offset[2], -n_offset[1]/n_offset[2]);
    auto [mu, sigma] = si_init.bsdf()->lean(si_init, true);
    Point2f slope = SpecularManifold::sample_gaussian(...);
    Normal3f lean_normal_local = normalize(Normal3f(-slope[0], -slope[1], 1.f));
    auto [success_smooth, si_smooth] = newton_solver(si, vtx_init, ei,
                                                      lean_normal_local, 1.f);
}
```

This matches RISE's measurement
([SMS_TWO_STAGE_SOLVER.md](SMS_TWO_STAGE_SOLVER.md)) that two-stage helps on
the smooth primitive + bumpmap regime and *regresses* on
heavily-displaced meshes. Mitsuba's `Figure_9_Twostage` reference scenes use
only smooth primitives + `normalmap` BSDFs;
`Figure_16_Displacement` never engages two-stage.

### Q2.3 "Back-up plan" when Newton converges to invalid root

**No.** Neither paper nor reference implementation has a back-up plan beyond
"redraw a fresh uniform sample". No retry-at-perturbed-seed, no
trust-region reset, no LM damping (Mitsuba is pure Newton with line search
halving, exactly like RISE pre-LM).

The paper's discussion of basin geometry — paraphrased in the survey
("Newton's method is known to produce convergence basins that potentially
have an extremely complex geometric structure and can even be fractal") —
acknowledges the problem in principle but doesn't propose a per-rejection
recovery; the solution is statistical (more Bernoulli trials).

## 4. Q3 — pbrt-v4 / pbrt-v3

`src/pbrt/cpu/integrators.h` enumerates pbrt-v4's integrators:
`PathIntegrator`, `BDPTIntegrator`, `MLTIntegrator`, `SPPMIntegrator`,
`VolPathIntegrator`, plus a few simpler ones. **There is no manifold
integrator, no MNEE, no SMS, no specular path constraint solver.**
A repository-level search confirms: no file containing "manifold", "MNEE",
"SMS", or specular-NEE constraint-solving code.

For caustics, pbrt-v4 relies on:
- **SPPM** (Stochastic Progressive Photon Mapping) — biased, but handles
  arbitrary SDS chains;
- **BDPT/MLT** for the small set of cases where bidirectional connections
  catch caustics by Markov chain.

There is no pbrt analogue of RISE's `ValidateChainPhysics` because there is
no Newton-on-half-vector machinery to validate.

The pbrt-v4 textbook treats the problem of caustic light transport entirely
through the photon-density estimate / VCM lens. Wrong-topology
half-vector roots are a non-issue because the algorithm never solves for one.
This is consistent with pbrt's "general-purpose, predictable variance"
philosophy — manifold methods trade variance for fragility, and pbrt opts
out of the trade.

**Practical implication for RISE:** pbrt is not a source of techniques here.
The relevant production answer pbrt embodies is "use VCM/SPPM for
SDS caustics", which is exactly the literature's Option C
([SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md)).

## 5. Q4 — Specular Polynomials, all-roots enumeration

### Q5.1 Mo 2024 "Specular Polynomials" — what it offers

The paper reformulates the specular-constraint system as a *polynomial*
system using rational coordinate mapping between consecutive vertices, then
applies the **hidden-variable resultant method** to reduce it to a
univariate polynomial root-finding problem. From the paper:

> "Newton solvers do not always converge and are highly sensitive to the
> selection of initial seed paths. Improper seed paths can lead to divergence
> and hence introduce substantial bias or variance to the final rendering."

> "[existing methods] are limited to obtaining at most a single solution each
> time and easily diverge when initialized with improper seeds."

The method "produces a complete set of admissible specular paths" and "all
of the solutions directly from these closed-form equations".

### Q5.2 Polynomial degrees

From the paper's Table 3 (k=2 only):

| Topology | Bivariate degrees |
|---|---|
| RR (k=2 reflection-reflection) | 10, 16 |
| RT or TR (mixed) | 10, 24 |
| TT (refraction-refraction) | 14, 36 |

For k=3 the paper says "the generalization to bounces over two is
straightforward" but provides **no explicit degree table**. The cost of
solving a bivariate system "is proportional to the cubic of the product
degree of the two polynomials" — so the degree-product determines the
asymptote, and 14×36 = 504 for TT k=2 is already the worst case in the table.
For k=3 this product grows roughly multiplicatively per added vertex; the
paper does not give k=3 timings.

### Q5.3 Practical k limit

The paper does **not state a hard limit**. Section 6 acknowledges
"Long specular chains" as a limitation but says "the method is
theoretically applicable to chains of arbitrary length". The
[SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md) literature snapshot is
more candid — the deferred citation captured "Limited to k≤3 in practice
because the resultant elimination scales poorly". This is consistent with
the table only enumerating k=2 cases.

### Q5.4 Topology validity per root

Squaring during reformulation introduces "superfluous solutions [that]
will be immediately filtered out after checking the specular constraints
in path space". This is the specular-polynomials analogue of the
sign-product check — applied to every enumerated root rather than one
Newton-converged candidate. **Enumerating all roots and filtering each
removes the wrong-topology basin trap entirely** — Newton finds the basin
nearest the seed (potentially wrong-topology); Specular Polynomials
returns *every* root and the filter rejects the wrong ones. **This is the
direct architectural answer to RISE's smoothness paradox.**

### Q5.5 Displaced meshes & cost

The paper does **not discuss displaced meshes specifically** — assumes
interpolated vertex normals on triangles. RISE's displaced regime would
need per-triangle local polynomial fits with C¹ stitching at edges. The
`mollnn/spoly` evaluation suite ships only analytic / smooth-mesh
examples; no displaced-mesh figure.

Cost: paper's Section 5.4 not in fetched excerpts. Project-page summary
says "superiority… compared to Newton iteration-based counterparts"
without quantifying. Survey-deferred estimate: comparable wall-clock at
k=2 with much better variance; worse asymptote at k=3+ as resultant
matrices grow.

## 6. Q5 — Other relevant references

- **Manifold Exploration (Jakob & Marschner 2012)** — original manifold-walk
  paper; Newton on half-vector. Acknowledges Newton "does not always
  converge" but treats it as a divergence problem fixed by MCMC proposals,
  not per-iteration topology validation.

- **MNEE (Hanika et al. 2015)** — strict ancestor of SMS; deterministic
  single walk. Same per-vertex constraint validation, no retry beyond what
  the outer integrator does. Production renderers (Hyperion, Cycles) ship
  MNEE *because* it's simpler. Cycles' "shadow caustics" mode requires
  *smooth normals* — consistent with literature consensus that
  bumpy/displaced surfaces break this class of method.

- **SMBS (Jhang & Chang 2022)** — MLT-style large-mutation rescue when
  Newton's line-search halving stalls. **Does not address wrong-topology**
  — rescues *divergence*. The reported ~2× success rate is on the
  Newton-fail population, not the Newton-converged-but-phys-rejected one.

- **Manifold Path Guiding (Fan 2023)** — learned proposal distribution over
  historical sub-paths. Improves *seed coverage*. Could in principle steer
  toward right-topology basins, but only if the training pass observed
  enough successful paths there. Paper does not analyse topology rejection.

- **Slope-Space Integrals (Loubet et al. 2020 / SIGGRAPH Asia)** — sister
  to Zeltner 2020, restricted to k=1. Closed-form integration in slope
  space replaces Newton entirely. Inapplicable to k≥2.

- **Bernstein Bounds (Schaufler 2025)** — interval analysis on
  per-triangle-tuple irradiance. Designed for k≤2 displaced meshes.
  Structurally avoids both basin and topology problems by not solving for
  a half-vector. On the deferred candidate list.

- **PSMS-ReSTIR (Hong 2025)** — temporal reservoir reuse. Hides per-frame
  topology-rejection variance via temporal amortisation. Production answer
  for animations, irrelevant for per-frame correctness.

## 7. Recommendations for RISE

The 5 % residual on Gaussian-bumped Veach is structural to plain Newton
seeded uniformly on the caster — every reference implementation we surveyed
(Mitsuba, SMBS, MPG-augmented Mitsuba, MNEE in Cycles) **accepts this rate
as part of the variance budget**, because no single technique fully
eliminates it without changing the solver class. Three concrete next steps
ranked by leverage and cost:

1. **Specular Polynomials (Mo 2024) for k≤3 displaced caustics.** The
   single architectural change in the surveyed literature that *structurally*
   removes the wrong-topology basin trap: enumerate every root of the
   constraint polynomial, filter each with a sign-product check, sum
   contributions. This is the direct answer to the smoothness paradox —
   smoother surfaces give Newton an easier *fractal* but the polynomial
   system has the same root count regardless of basin geometry. Already
   on the candidate list ([SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md))
   with a 6–10 week estimate; this survey strengthens the case. Open
   question: practical k=3 cost, and per-triangle local polynomial fit
   for displaced meshes.

2. **Confirm the Mitsuba-parity baseline on a shared scene before any new
   solver work.** RISE now has the same `ValidateChainPhysics` logic as
   Mitsuba (geometric normal, sign product, no retry, identical predicate)
   *plus* a small set of extensions Mitsuba lacks (Snell-trace seeding;
   Fresnel-branching was excised in 2026-05). Run RISE in a Mitsuba-
   equivalent configuration (`sms_seeding "uniform"`, `mnee_init=false`,
   `sms_two_stage=false`) on a Veach-egg-class scene and verify the residual
   phys-fail rate matches Mitsuba's measured rate. If RISE is meaningfully
   *worse* than Mitsuba in this configuration, the gap is in our solver, not
   in the algorithm class — investigate before any new method. If RISE
   matches, the residual is the literature-consensus floor and only
   architectural changes (option 1 or option 3) help.

3. **Document the floor; do not bump per-vertex thresholds.** The temptation
   when a phys-fail anatomy lands at 5 % is to widen the sign-product
   tolerance or the constraint norm threshold to bring rejection rate
   down. **Don't.** Both Mitsuba and the precision-fix-the-formulation
   skill treat threshold bumping as a symptom suppressor — the rejected
   chain at ‖C‖ < 1e-4 with same-sign sign product *is* geometrically
   wrong, and accepting it adds energy at the wrong basin
   ([SMS_SEEDING_OVERTRACE_FIX.md](SMS_SEEDING_OVERTRACE_FIX.md) anatomy).
   Instead, accept the floor on plain Newton and either (a) ship Specular
   Polynomials as an alt-solver, or (b) escalate to VCM for the user-facing
   displaced-caustic regime.

A natural sequencing: start with (2) (1–2 days), close the Mitsuba-parity
question, then commit to (1) if the floor matters for the target scene class.

## 8. Things this pass could not determine

- **Zeltner 2020 paper PDF, full text** — fetch hit 10 MB cap on every host.
  The reference code carries the substantive answers (§5 = LEAN-moment
  normal-map two-stage, not displaced-mesh two-stage), but the paper's
  narrative on basin failure modes was not directly quoted. Fetch
  per-section page extracts if verbatim quotes are needed.

- **Specular Polynomials k=3+ practical cost** — Section 5.4 performance
  table not in fetched excerpt. The `mollnn/spoly` reference code could be
  inspected for CPU/GPU timings if k=3 cost gates the implementation
  decision.

- **PMS phys-fail rate breakdown** — Weisstein 2024 slides PDF was a binary
  blob in fetch; paper's quantification of "more seeds in the right basin"
  vs "fewer topology rejections per seed" was not extracted. RISE's own
  measurements ([SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md](SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md))
  are a more direct answer for our regime.

- **Cycles SMS thesis (Uppsala, DiVA)** — request timed out. The author
  would have hit the same topology-rejection rate during integration; the
  practical observations there were not retrieved. Follow up if
  recommendation (2)'s floor measurement lands meaningfully above
  Mitsuba's.
