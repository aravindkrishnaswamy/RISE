# DL-01: translucent exit lobe weights

CLOSED 2026-09-12 — fix `1239edf2`. The DL-01 regression run reported
`TranslucentSpectralParityTest: 676 checks, 0 failures`.

## Contract and root cause

`TranslucentSPF.h` describes `pTrans` as the primary-layer transmittance.
Both entry paths already multiply by it. An interior segment therefore
pays Beer extinction `B = exp(-extinction*distance)` and splits that
weight into exit `B*(1-scattering)` and backscatter `B*scattering`.
RGB implements this contract. NM multiplied by transmittance again before
the split, attenuating both children and every additional internal segment.
The fix removes that extra multiplier at the common spectral producer.

This is a decision about the existing stateful lampshade model, not a
claim that its BSDF or every integrator now models a physical volume
exactly. The diffuse exit support/shape gap was subsequently closed by DL-02
(`a041e51d`); full mixture density remains DL-41. Guiding-stack
propagation remains DL-03, and the independent consumer gaps below remain
open. No IOR convention or sampling distribution changes in DL-01.

## Reproduction and verification

The isolated branch started at master `6486656e03baf64dbf8bfca80bb5dcacc894f4e4`.
Test commit `fc371041` was built and run against that unfixed library
before any source edit. The executable exited 1. Representative verbatim
output (all failure lines are also in fix commit `1239edf2`):

```text
case tau=0.4 ext=0.1 distance=1.0 scatter=0.0 N=1
FAIL: NM exit Beer split got 0.361881033 expected 0.904837418
FAIL: NM segment total is Beer, independent of tau got 0.361881033 expected 0.904837418
FAIL: RGB/NM exit parity got 0.361881033 expected 0.904837418
FAIL: NM entry/segment throughput pays tau once got 0.144730843 expected 0.361881033
TranslucentSpectralParityTest: 676 checks, 174 failures
```

The test checks local weights directly, at 450/550/650 nm, using uniform
and per-channel Phong N, tau/scattering endpoints, zero/nonzero extinction,
and zero/nonzero distance. It also chains an actual NM entry-produced
stack into the next scatter and checks that combined throughput pays tau
once. The expected entry tau uses the painter's wavelength value, so an
RGB-to-spectrum approximation is not mistaken for transport bias. The
weight oracle does not depend on the sampled direction; it is not a
rendered radiance comparison. The tolerance is an absolute `1e-3`.

## Bug-pattern audit

Pattern: the spectral interior-segment parent weight reapplies the
primary-layer transmittance already paid at entry, attenuating all children.
The audit searched consumed `kray`/`krayNM`, then the adjacent
`ior_stack`/`delete_stack` ownership state.

| Surface | STATUS | Evidence / disposition |
|---|---|---|
| `Materials/TranslucentSPF.cpp`, RGB entry and uniform/per-channel exit | VERIFIED unchanged | Entry pays tau; all exit children derive from Beer extinction. |
| `Materials/TranslucentSPF.cpp`, NM entry/exit/backscatter | FIXED `1239edf2` | Entry still pays tau; both exit children now inherit Beer only. |
| `Materials/ScatteredRayContainer.cpp` | VERIFIED unchanged | Selection reads supplied weights; no compensating tau. Ownership transfer through `delete_stack` is unchanged. |
| `Materials/CompositeSPF.cpp` | VERIFIED unchanged | Multiplies child weights and separate inter-layer gap extinction, with no compensating primary-layer tau. |
| `Materials/CoatedMaterial.h`, `Materials/FabricMaterial.h` | INAPPLICABLE | Their substrate allowlists exclude translucent materials. |
| `Shaders/PathTracingIntegrator.cpp`, ordinary RGB/NM | VERIFIED unchanged | Consumes supplied SPF weight with selection compensation; no second material tau correction. |
| `Shaders/PathTracingIntegrator.cpp`, HWSS companions | OPEN DL-38 | BSDF fallback cannot reproduce the stateful exit weight. |
| `Shaders/BDPTIntegrator.cpp`, eye/light RGB/NM/HWSS; VCM/MLT shared walks | OPEN DL-38 | Non-delta continuation reevaluates BSDF instead of carrying the SPF amplitude. |
| Generic/global/caustic photon consumers and shader operations | VERIFIED unchanged for DL-01 | Multiply selected supplied weights; no compensating duplicate tau. |
| `PhotonMapping/TranslucentPelPhotonTracer.cpp` and photon-map gather | OPEN DL-39 | Dedicated deposition accounts absorbed energy as stored power. |
| SMS snell/uniform modes | INAPPLICABLE | TranslucentSPF supplies no specular-info override for an analytic translucent chain. |
| `ior_stack` / `delete_stack` | VERIFIED unchanged | The weight edit changes neither stack allocation nor ownership; existing ownership guards remain in TranslucentIORStackTest. |

Paths in this table are relative to `src/Library/`.

## Independent residuals

**DL-38 — stateful translucent BSDF/HWSS repricing.**
`TranslucentSPF` has no `EvaluateKrayNM` override. PT HWSS reads the hero's
`pS->krayNM` but falls back to `TranslucentBSDF::valueNM*cos/pdf` for
companions. That BSDF has no extinction, scattering, or inside-state inputs.
BDPT's `GenerateEyeSubpathImpl` and `GenerateLightSubpathImpl` likewise
reevaluate non-delta weights through the BSDF, including their spectral
companions; VCM/MLT inherit those shared generators. These weights cannot
reproduce an extinction-varying interior exit while ref/tau stay fixed.
Static evidence is confirmed; a numeric transport red-proof remains to be
built under DL-38's ledger recipe. No rendered deficit is asserted here.

**DL-39 — translucent photon deposition accounts absorbed power as stored.**
`TranslucentPelPhotonTracer::TracePhoton` accumulates propagated non-diffuse
weights, then deposits `power*(1-accum_scattered)`. With an interior exit
and scattering zero, it stores all incoming power regardless of extinction.
`TranslucentPelPhotonMap::RadianceEstimate` sums stored power and applies
the BSDF, which cannot recover the missing Beer attenuation. Static evidence
is confirmed; deposited-flux red-proof remains DL-39's separate recipe.

## Corrections to the original ledger

- The scene descriptor accepts `tau`; `transmittance` was explanatory
  terminology incorrectly presented as a parameter in the recipe.
- `TranslucentIORStackTest` did not define a `CROSS_VAL_TOL`. The new test
  declares `kWeightTolerance = 1e-3` explicitly as the recipe's absolute band.
- The extra NM factor affects backscatter as well as the exit ray.
- Local weight parity does not establish whole-render RGB/NM/HWSS parity;
  DL-38 and DL-41 remain distinct blockers to that broader claim.

Master advanced independently to `0b920978` during the work, removing the
checked-in handoff prompt and its ledger link. Merge `9ca3727b` preserved
that documentation-only change in the debt branch.

## Gate measurement repair

The touched-class search selects TranslucentIORStackTest,
CompositeExtinctionTest, SPFBSDFConsistencyTest, SPFPdfConsistencyTest,
BDPTStrategyBalanceTest and VCMStrategyBalanceTest. The last two mention
the class in comments; they are still included in the literal search gate.
No full-suite runner is used.

Commit `a77279f9` applies the requested render hygiene: explicit box
filtering on the two missing VCM reference strings, denoising off on every
rasterizer string, and `std::srand(1729 + renderIndex)` before each render.
The otherwise removed fallback output chunks use relative paths and linear
Rec.709 EXR. Actual statistics read the in-memory linear capture. Repeats
remain non-bit-reproducible because worker scheduling is unsynchronised.

The first VCM run then reported `Passed: 53` / `Failed: 1`: the
orthographic row compared VCM against unpremultiplied PT surface RGB,
although its comment claimed composited radiance. The BDPT twin already
used `base*alpha`. Commit `9758659a` first added an exact two-pixel oracle,
which failed before the statistics fix (exit 1):

```text
=== VCMStrategyBalanceTest ===
  FAIL: capture statistics compose coverage alpha over black
  surface mean R=0.4 composited mean R=0.1 expected R=0.1
```

Commit `3e633c45` repairs the required gate's observable to `base*alpha`,
matching its documented intent and the BDPT twin. It changes no renderer
transport and loosens no tolerance. This is measurement setup needed to
use the gate under the requested box-filter protocol. The initial failing
render run and the exact red-proof are retained with the gate evidence;
they must not be represented as successful runs.

## Focused gate result before review

All seven binaries completed successfully after the measurement repair.
Their final summary lines, verbatim:

```text
TranslucentSpectralParityTest: 676 checks, 0 failures
ALL TESTS PASSED
  All checks passed
All SPF-BSDF consistency tests passed!
All SPF PDF consistency tests passed!
Passed: 66
Failed: 0
Passed: 55
Failed: 0
```

These correspond, in order, to the recipe test and the six named gate
suites above. The standalone delivery report retains the full stdout,
including every per-material counter, the initial failed VCM run, and
both red-proofs. `make all` was run from a fresh worktree and then again
after `make clean`; compiler-warning counts were zero. The clean Xcode
RISE-GUI Deployment build succeeded with zero compiler warnings; its
AppIntents metadata-tool notice is excluded by AGENTS.md's warning gate.
The pinned OIDN binary dependency was populated only in the isolated
worktree before that Xcode build. No warning suppressions were added.

Self-audit before independent review: the principal risks are choosing the
wrong layer for tau, missing the backscatter sibling, mistaking spectral
uplift for a transport oracle, and claiming whole-integrator parity from
local weights. The contract evidence, parent-weight fix, wavelength-aware
entry expectation, and explicit DL-02/DL-38 residuals address those risks.

## Review residual and clarified scope

The transport review found DL-02's description too narrow: inside-state
`Pdf` flips the exit hemisphere, returning zero on the directions sampled
by BOTH RGB and NM, even at N=1. NM additionally has the documented Phong
shape difference. The DL-02 row, recipe, and both source documents now
state this wider density scope; no next-row implementation was started.

**DL-40 — balance-harness nonfinite false-green risk.** The test-integrity
review found both BDPT/VCM `ComputeStats` functions accept nonfinite
captured values and mark nonempty statistics valid. `ChannelsAgree`
rejects only a relative difference greater than tolerance, so a NaN
candidate can pass against a finite reference. This is independently
confirmed by reading both comparators and the capture loop. The recorded
DL-01 summary statistics are finite; no observed gate result is explained
by this defect. Per the one-row scope rule, this separate malformed-input
robustness pattern is recorded as DL-40 with an exact red-proof recipe,
rather than expanded into the translucent weight fix.

The documentation review also identified historical sweep header labels
that still said "this revision" / "previous revision". Those labels now
explicitly identify the historical sweeps.
