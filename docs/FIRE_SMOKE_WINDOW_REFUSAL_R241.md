# r241 — localize the canonical-inlet window refusal

The r240 window stopped after 178 accepted steps at
0.27414379687979817 s, with stage-0 bitmap 33564816 (bits 4, 7, 11,
13, 25). Maximum velocity was 1.0870400667190552 m/s and RSS
1,882,357,760 bytes. This is neither a completed window nor a movie.
The preserved executable has SHA-256
`f542462b288dee29b2c9c5fbf1ba51cc27c51e14ff5ca0445ec995fd008f4ca8`.
Payloads remain outside git under
`/Volumes/SSD/RISE/fire_production_calibration/r240_tier8_full_window.v1`.

## Diagnostic, not a new acceptance surface

The bitmap alone does not name the first writer: bit 7 is also used by
the FCT donor-state feasibility test, and bit 4 can be a downstream
candidate-seal refusal. Reuse the existing phase witnesses in a scoped,
from-zero tier-8 diagnostic, retaining the full window's case duration,
cadence, seed and physical inputs, but stopping diagnostically at 0.35 s.
No shortened-window empirical claim follows from this prefix.

On a candidate-first refusal, read only the refused low/candidate
payloads and evaluate the existing eleven inequalities in binary64.
Report the offending cell, row, conservative tuple, residual and existing
bound in the row's units. This is a localization witness, not a replacement
for the device predicate or a new certificate. Phase and seal witnesses
remain available. No arithmetic, tolerance, acceptance gate or production
authority changes in this diagnostic.

Next: use the first failed clause and cell to fix the formulation, prove
the regression, review, then restart the full window from zero. No kernel
transition migration is authorized or required.

## First localization

The from-zero diagnostic reproduces all 178 accepted steps and the same
failure. Phase words are physical=0, candidate=144, EOS=144, target=10384,
final=33564816: the donor feasibility clause fires before EOS. At cell 2380,
row 9 (lower sensible energy), the refused donor tuple gives binary64 residual
0.0002715112096258257 J/m3 versus the unchanged bound
0.0002671830480063889 J/m3. The candidate gives
0.00027151120966516587 versus 0.00026718358086751082 J/m3. This is not an
EOS ordering ambiguity. No downstream lineage bit is suppressed.

The first 55 physics-trajectory rows compare byte-identically with r240
(timings and build-derived identities excluded). The diagnostic log SHA256
is `4ab10486c8029c4c1967f5d74a9e87e94e4568d7a2655f5e9fe2d5e7f0f1185a`.
Detailed follow-up reads the same cell's beginning/source energy and six
face contributions after refusal. Retain diagnostic checkpoints every 120 s;
the next full window uses 300 s rather than 900 s retention. Neither change
alters solver arithmetic. All payload bytes remain outside git.

## Producer finding and amendment (before numerical implementation)

The detailed replay repeats the failure at (34,34,0). Beginning energy is
-0.00026372572756372392 J/m3, frozen source energy is exactly zero and
temperature is 300 K. The five non-inlet faces all report the same device
species enthalpies in J/kg:
`0.5, 0.125, 0.015625, -0.03125, 0.25, -0.0390625, 0.5`.
These are cancellation artifacts of the absolute primitive plus reference
offset, not the record's near-zero binary64 sensible enthalpies. The top face
alone carries physical energy flux 0.00016222058911807835 W/m2. Its donor
contribution is -0.000021217632820480503 W/m2. Small spurious cold diffusion
accumulates until the unchanged r60 bound correctly refuses it. r198's broad
termwise enthalpy error allowance did not ensure preservation of this cold
reference equilibrium. Qualification is necessary but this equilibrium is
also a functional requirement; do not widen the donor bound to absorb drift.

Replace physical-flux enthalpy evaluation with a segment-anchored integral.
For segment lower endpoint A, pack the existing binary64 record's h(A) rounded
once to binary32, and evaluate h(T)=h(A)+integral_A^T cp(t)dt. Factor each
power difference as (T-A) times its geometric sum and compute the logarithmic
difference through a range-reduced atanh series, not log(T)-log(A). At T=A
the increment is exactly zero and the producer returns the packed anchor.
No cold-temperature epsilon, state clamp, source change, bound increase or
energy injection is permitted. The original thermo coefficients/offsets stay
available to historical consumers; append 21 anchor floats to the shared
immutable table and account for them in the working-set calculation.

Regression first: an isothermal 300 K composition-gradient fixture must
return each species' binary64-record anchor rounded to binary32 on every
active face. Demonstrate failure on the existing kernel before the change.
Keep the existing per-field physical-flux bounds, donor/MC comparisons and
shared-fN identity checks. Re-run the reviewed-owner gate and continuation
prefix. This is a numerical reformulation, not a bit-identical promotion;
version the production policy and restart the window from zero. No migration.

## Initial repair gate

The old-kernel cold-mixture regression fails (exit 241): methane gives
0.5 J/kg rather than the binary32 projection of 1.862645149230957e-9 J/kg.
With the anchored formulation it passes exactly. Existing physical-flux
per-field enclosures and donor/MC/shared-fN checks pass without changes.
The mixed owner comparison likewise passes every numerical field and every
per-iteration comparison. Its unrelated coefficient-fallback mutation check
then fails solely because this changed trajectory no longer distinguishes
the old coefficient at R0; R1 still distinguishes it. Both stages' actual
CPU/device/walker values remain bit-identical. Scope the mutation witness to
the single shared fallback implementation across the two stages, retaining
all per-stage bit comparisons and requiring a live distinguishing witness.
Do not manufacture a different physical trajectory to satisfy this fixture.

The absolute-primitive binary32 comparison is retained only as an explicitly
historical diagnostic (it now differs); the unchanged binary64 physical-flux
reference and existing per-field enclosures are the numerical gate. The cold
anchor equality test also checks the exact 144 active interior faces, and
the adjacent representable cold temperature is checked against the same
physical-flux bounds. The legacy manifold-only `methane_enthalpy` is not a
flux producer; EOS/source/target use the existing compensated implementation.
No other live energy-flux producer retains the removed absolute cancellation.

The cold owner also passes all numerical field comparisons, but the retired
scalar-error heuristic no longer refuses its now-smaller actual error. That
is not a required solver behavior. Preserve the omission RED by constructing
a binary32 field probe inside the inherited publication enclosure and outside
the retired heuristic, and send it through the same field comparator twice:
the full interval must admit it and the omitted-parent interval must refuse.
Record actual-point behavior separately. No production bound or numerical
comparison is changed; the negative control tests the missing uncertainty,
not a requirement that the producer continue making the old error.

## Qualified increment

The focused build is warning-free; physical-flux, mixed/cold reviewed-owner,
actual launcher/retry, full Metal/fp64 kernel sweep, launcher input and window
publication gates all exit zero. Both owner fixtures report per-iteration=1,
per-field=1, REDs=1, and live/reference accepted-owner identity=1. The scalar
omission probe is accepted with the inherited interval and refused with the
omitted-parent interval. The cold anchor gate checks all 144 active faces.
No full-window or movie completion is claimed by these tests.

Physical sensible-energy flux: maximum sampled residual
0.00096820077317261166 W/m2 with same-face existing bound
18.073612232145148 W/m2; worst residual/local-bound ratio
0.00023829039566474218. Donor and MC bit comparisons, shared fN, branch-map,
lineage and residency REDs pass independently. Historical absolute binary32
enthalpy differs as expected. Policy v3 seals this numerical reformulation.

Evidence (logs under `rendered/fire_production_calibration/r239_burner_projection/`):

| Artifact | SHA256 |
|---|---|
| Detailed refusal log (external `r241_refusal_prefix.v2.log`) | `93fecf3a7420c144b301869db9bbbe5a9735815d0fc2d885eebdf854ffa52702` |
| `r241-cold-red.log` | `8b449800fd499a6330a11d4009b8a75a31641432ff80c0d2aa5369e2a9df7152` |
| Tested executable | `448302e9c0146b4116da7cca2b650ff60af2b18c98009911dd95f12d18607a37` |
| `r241-qualified-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `r241-physical-qualified.log` | `342500cce18b3e17bc85dcf8e4406dbb164ee2a7ca20ba7ea0f7700a6638f6f1` |
| `r241-owner-qualified.log` | `88034bb6c1511fe9b6d57488086057d5bfba06e09a69a9ee82efcca09d66263b` |
| `r241-launcher-qualified.log` | `c6d3c1a1b7e3fe8911ad3a13845c427eafecf02aecc0e870747df4d6529f2479` |
| `r241-sweep-qualified.log` | `78211e616d6a640a56f2c65661055d72775e1733eae5c5b65326cd832fc02a25` |
| `r241-inputs-qualified.log` | `3a9dc882e11f61b7350491f86b40bcbc89d633b6fe5dabcb6385aba3f1e98b47` |
| `r241-publication-qualified.log` | `dcb7df822691a59f6eefcd8379a3843cea0d95c7d028a13e4b4b2691c6cecafc` |

## Independent review follow-up

The contract/provenance review found no P1/P2. The numerical review found a
test-enclosure omission: the owner comparison represented face enthalpy as
one rounded fp64 evaluation, omitting the physical producer's coefficient
packing and intermediate binary32 arithmetic. The broad standalone flux gate
does not discharge that separate owner obligation. This finding is confirmed
by inspection; the preceding successful tests are not claimed to close it.

Repair the test by walking the actual anchored integral's operations using
the existing certified arithmetic, including coefficient/anchor packing.
For range-reduced y in [0,1/3], the omitted logarithmic tail is bounded by
2*y^19/(19*(1-y^2)). Propagate this producer term separately from the existing
cp-bound times input-temperature uncertainty. Recenter on the canonical fp64
functional with its deterministic algebraic evaluation difference recorded.
This is a missing arithmetic contribution, not an empirical multiplier or a
change to any live feasibility/enclosure threshold. Add a hot isothermal
550.375 K device witness and an omitted-producer-term negative control.
Rebuild and re-run the owner and physical-flux gates after the current
diagnostic finishes; no build runs concurrently with it. Fresh review follows.

The repaired from-zero diagnostic completes 225 accepted steps through
0.35000000000582077 s and seals successfully (exit zero), past the old
0.27414379687979817-s refusal. Final RSS is 1,865,711,616 bytes. Velocity is
1.9066133499145508 m/s at the last full-size step, then 4.8051786422729492 m/s
on the endpoint-clipped step; neither is a full-window or empirical claim.
Log SHA256: `68128becbdba5bbe421c18b52b4e0afdf73d3447ce0994652778219c78efd36a`.

The test-only repair builds without warnings and both physical-flux and
mixed/cold owner gates pass. Actual device methane enthalpy at 550.375 K is
659751.625 J/kg against canonical 659752.05120273214 J/kg: residual
0.4262027321383357 J/kg, derived producer bound 2.033713855268815 J/kg.
The omitted-producer one-cast bound is 0.03932428899568445 J/kg and refuses
this witness. All 144 active faces are enclosed. Live/reference owner
identity, per-iteration/per-field comparisons and applicable REDs pass.
No production kernel changed during this review follow-up.

| Follow-up artifact | SHA256 |
|---|---|
| Rebuilt executable | `7b5c95579e608880c0d7da4fed8fc7f3031cf2f86e7dc79dbbfd80f7e79281ba` |
| `r241-review-build.v2.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `r241-review-physical.log` | `660750f5528a9b36f5415f789c9c6bb62a878264578731917fe3e0e602607c61` |
| `r241-review-owner.log` | `29c9ac695cb0911df45d32fdac4d3b4e55fad78d22602ca1ae8bded53a62f650` |

## Reviewed full-window launch

Numerical and test-integrity/provenance reviews of `d8d58855` report zero
P1/P2. The actual launcher/retry, complete device kernel sweep, input scope
and publication gates also pass on the rebuilt executable. Their log hashes:

| Artifact | SHA256 |
|---|---|
| `r241-review-launcher.log` | `1a1d7e22107752a366e80997629aa882d4bf8ba2f01983549a0013edb0cf6ea6` |
| `r241-review-sweep.log` | `cb95c549932eb4511009c94a4141b859e85189dacfbd857880c34614d271cebc` |
| `r241-review-inputs.log` | `3a9dc882e11f61b7350491f86b40bcbc89d633b6fe5dabcb6385aba3f1e98b47` |
| `r241-review-publication.log` | `dcb7df822691a59f6eefcd8379a3843cea0d95c7d028a13e4b4b2691c6cecafc` |

The full from-zero tier-8 run is launched using the frozen executable in
`/Volumes/SSD/RISE/fire_production_calibration/r241_runtime.v2/`.
Run output is `r241_tier8_full_window.v1` in the same calibration parent.
Case ID `764cc88cc43415bc1d893f67a273f90eb251bc4df536551f7cf0287c44938cc9`;
build ID `61813e937ba1d16d5b06e887e464f46a0e0e8c8ba2c68ce8888c1cb442b64702`.
Target 25.091040127156969 s consists of 10.466040127156969 s discard and
14.625 s statistics; cadence remains 0.0625 s. The ordinary production path
has no diagnostic environment flag. Checkpoints are retained every 300 wall
seconds. RSS/footprint ceiling is 32 GiB with the existing next-step reserve
and graceful exit 97; no builds/qualifications/oracle/renders run concurrently.

Launch receipt SHA256
`6ac2f2d7f4d9fd0dca1170e34a48074aad67890ec1b4cf6a3f0ace43ac20fdce`;
host-memory admission SHA256
`710eefabb45e75bddfdc1ffe6d8e19f630fa5448663f8a5cdf4e5472037b31dd`.
Sequential pipeline session 83750 renders only after successful simulation,
then encodes, decodes/verifies 235 frames at exact 16-fps timestamps, then
exports the spectrum. Planned movie is
`r241_tier8_preview.v1/temporal_fire_preview_16fps.mov`; planned spectrum is
`r241_tier8_spectrum.v1`. Neither is claimed complete at launch. The existing
preview-primary label and tier-10-only empirical-row restriction stand.
A narrow read-only app monitor checks every 30 minutes, stays quiet while
healthy, and reports failure or delivers the decoded/inspected movie.
