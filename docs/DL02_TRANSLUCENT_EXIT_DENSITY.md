# DL-02: translucent diffuse exit density

CLOSED 2026-09-12 — fix `a041e51d`. The focused regression reports
`TranslucentSpectralParityTest: 1918 checks, 0 failures`.

## Contract and root cause

The diffuse exit is a cosine-weighted re-emission around the positive
shading normal, in both RGB and NM. At the time this fix landed its
stored conditional density was `max(dot(wo,onb.w()),0)/pi`. `Pdf` instead
negated that cosine when the IOR stack identified an inside hit, so it
returned zero on sampled exit directions and positive values on the
opposite hemisphere. Additionally, NM sampled a Phong exit controlled by
N while RGB already sampled cosine.

The fix removes the inside-state sign flip and uses the RGB cosine
construction for NM exit sampling and its stored density. N still controls
entry transmission and internal backscatter. Throughput, delta flags,
stack pop and ownership are unchanged. No integrator-wide MIS or
physical-volume correctness is claimed: the conditional diffuse density
is only one part of that contract.

**Superseded 2026-09-13 by DL-45/DL-68**: the "entering-only geometric-
horizon gate" this section describes as unchanged no longer describes the
exit lobe. DL-45 found that the plain (unclipped) cosine sampler this fix
installed could still emit a direction that is geometrically INWARD under
a tilted shading normal even though it is above the shading horizon, and
added a matching exit-side geometric-horizon gate; the exit lobe is now a
**clipped cosine sampler**, restricted to the geometrically-valid
sub-hemisphere and renormalized by `ExitValidFraction = (1+cos(phi))/2`
(phi = the angle between the shading and true geometric normal) rather
than the plain `cos(theta)/pi` this section originally described. DL-68
subsequently replaced DL-45's rejection-sampling implementation of that
clipped sampler with an exact, unconditional two-draw closed-form remap
(same density, no resampling) — see `SampleValidDiffuseExit`,
`TranslucentSPF.cpp`, and [DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md).
The DL-01/DL-02 contract this document otherwise describes (Beer
attenuation, entry/backscatter Phong lobes, RGB/NM parity) is unaffected.

## Red proof

The isolated branch started at clean master
`cb9dd0f4741312d8571cecb64e70a45c803ef5f4`. Test commit `adc9a286` was built
and run against that unfixed library before the source edit (exit 1).
The exact summary was:

```text
TranslucentSpectralParityTest: 1918 checks, 324 failures
```

At N=1 both RGB and NM fail support/evaluator checks. N=8 and per-channel
N=5/10/15 also expose the spectral shape error. All 324 failing lines are
in fix commit `a041e51d`; the standalone report includes the complete log.
The same test binary source rebuilt after the fix reports zero failures.

The new checks use fixed variates, cosine inverse CDF (`cos²(theta)=u`),
second moment 1/2, pointwise stored/evaluated density, and separate
hemisphere quadrature. Integrating the positive and negative hemispheres
separately prevents a reversed but normalized PDF from passing. Uniform
N=1/8, per-channel N=5/10/15, scattering=0/0.3/1, normal and tilted shading
frames, and wavelengths 450/550/650 nm are covered. Entry hemisphere and
geometric-horizon controls protect the unrelated reflection gate.
Density checks use an absolute 1e-10 roundoff band; the earlier DL-01
weight checks retain their 1e-3 tolerance.

## Bug-pattern audit

Pattern: the diffuse exit's evaluated hemisphere or shape differs from
its sampled conditional density. The sweep reads consumed `pdf`, then
`pdfRev`, `selectProb`, and `ior_stack`, rather than only scatter calls.
Paths below are relative to `src/Library/`.

| Surface | STATUS | Evidence / disposition |
|---|---|---|
| `Materials/TranslucentSPF.cpp`, RGB diffuse exit | VERIFIED at the time (unchanged sampler); SUPERSEDED 2026-09-13 by DL-45/DL-68 | Was plain cosine around +onb.w() with Pdf support fixed by `a041e51d`; DL-45 clipped it to the geometrically-valid sub-hemisphere (`ExitValidFraction`-normalized) and DL-68 replaced the clipping's rejection loop with an exact two-draw remap — see `SampleValidDiffuseExit`, `TranslucentSPF.cpp`. |
| `Materials/TranslucentSPF.cpp`, NM diffuse exit | FIXED `a041e51d` | Cosine sample and stored density replace final exit Phong construction; PdfNM delegates to corrected Pdf. |
| `Materials/TranslucentSPF.cpp`, RGB uniform/per-channel and NM entry/backscatter | VERIFIED unchanged for this pattern | These lobes intentionally use N-dependent Phong sampling with matching stored conditional densities; their evaluable mixture remains DL-41. |
| `Shaders/PathTracingIntegrator.cpp`, ordinary RGB/NM | VERIFIED unchanged consumer | Uses supplied pS->pdf; no compensation for the old negative support. |
| `Shaders/PathTracingIntegrator.cpp`, HWSS | VERIFIED hero density; OPEN DL-38 companions | Hero consumes stored pdf; companion fallback BSDF*cos/pdf cannot reproduce stateful amplitudes. |
| `Shaders/PathTracingIntegrator.cpp`, guided RGB/NM | CORRECTED producer; DL-03 subsequently CLOSED | Pdf/PdfNM agree with the diffuse exit sampler; guiding state was closed by `8a9bdb18`; selected-lobe compensation was DL-42, CLOSED 2026-09-13. |
| `Shaders/BDPTIntegrator.cpp`, eye/light RGB/NM/HWSS | CORRECTED producer; OPEN DL-38/DL-41 | Uses selectProb*effectivePdf forward; reverse reevaluation converts to predecessor pdfRev. Eye guided-candidate argument order was separately DL-43, CLOSED `a69c9ce6`. |
| `Utilities/PathVertexEval.h` | VERIFIED propagation; OPEN DL-41 full contract | Rebuilds intersection and stack then calls Pdf/PdfNM; does not compensate for the former sign error. |
| `Shaders/VCMIntegrator.cpp` and MLT rasterizers | VERIFIED shared consumer | VCM consumes inherited pdfRev in MIS recurrence; RGB/NM MLT uses BDPT generators. No duplicate translucent sampler. |
| `Lights/LightSampler.cpp`, RGB/NM area/environment NEE | OPEN DL-41 state query | Evaluates material Pdf/PdfNM using defaultIOR rather than current walk stack. |
| `Materials/CompositeSPF.cpp` | VERIFIED child propagation; OPEN DL-24 mixture | Child records propagate; wrapper Pdf is a separate fixed 50/50 approximation. |
| `Materials/CoatedMaterial.h`, `Materials/FabricMaterial.h` | INAPPLICABLE | Substrate allowlists exclude TranslucentMaterial. |
| SMS snell/uniform modes | INAPPLICABLE | TranslucentSPF has no valid analytic specular-info override. |
| Generic/global/caustic photon consumers | VERIFIED unchanged | Consume directions, weights and optional stacks without reevaluating pdf. |
| `PhotonMapping/TranslucentPelPhotonTracer.cpp` | VERIFIED density-independent; CLOSED DL-39 (`1fe5c760`) | Deposition error concerned absorbed power, not the exit density; now deposits the diffuse lobe's own kray directly. |
| `ior_stack` / `delete_stack` | VERIFIED unchanged | Exit still pops the object; backscatter retains the input stack; no lifetime or allocation changes. |

## ~~Independent residual: DL-41~~ — CLOSED 2026-09-18

**DL-41 CLOSED 2026-09-18 (`debt-dl157` slice)** — `TranslucentSPF::Pdf`/
`PdfNM` now report the density of what `Scatter` + `RandomlySelect`
actually generate: `sum_I q_I p_I(w)` over every lobe the side can emit,
both Phong lobes included, with `q_I` exactly `RandomlySelect`'s
realized weights and no replay quadrature needed. See
[DL157_TRANSLUCENT_ONE_FUNCTION.md](DL157_TRANSLUCENT_ONE_FUNCTION.md).

### (struck) original text

**Complete translucent mixture/reverse density and state reconstruction.**
The legacy Pdf/PdfNM API describes only the diffuse conditional lobe:
negative-hemisphere Phong transmission/backscatter still evaluate to zero,
and the positive cosine lacks lobe-selection probability. BDPT eye/light
walks select by kray/total and multiply forward density by selectProb;
reverse evaluation instead calls Pdf/PdfNM directly. Thus a nonzero
backscatter or entry-transmission mixture is not represented by a complete
forward/reverse density pair. NEE also queries with an empty defaultIOR.
This is separate from the BSDF amplitude gap DL-38 and the guiding-state gap
subsequently closed by DL-03 (`8a9bdb18`). Static evidence is confirmed; no numeric transport deficit is claimed
until DL-41's own red proof. The negative-hemisphere-zero assertion in the
DL-02 test pins the existing conditional API, not an ideal full mixture.

## Review residuals

**~~DL-42 — PT one-sample guiding loses lobe-selection compensation.~~
CLOSED 2026-09-13 — `PathTracingIntegrator.cpp` fix,
`tests/PTGuidedSelectProbTest.cpp: ALL TESTS PASSED`.**
The ordinary RGB/NM loop initializes scatterThroughput as kray/selectProb.
When trained one-sample guiding kept the BSDF candidate, it overwrote
that value with kray*pdf/combinedPdf, dropping selectProb. This was
reachable at an ordinary entry reflection on a mixed-lobe material. It is
independent of the translucent exit sign/shape producer and needed no BSDF
reevaluation or substituted stack. Ordinary translucent exits are not a
clean PT fixture because GuidingEffectiveAlpha disables specular arrivals
-- the closing red-proof instead used TranslucentSPF's EXIT branch, whose
diffuse-exit + translucent-backscatter lobes are a real, reachable
multi-lobe pair with unequal weight via `scattering`. (Only the
diffuse-exit lobe is itself guiding-ELIGIBLE --
`GuidingSupportsSurfaceSampling` admits non-delta eRayDiffuse/eRayReflection
only, not eRayTranslucent -- so the fixture's assertions are restricted to
trials where `PTRandomlySelect` happens to pick that one lobe; the
backscatter lobe is what makes `selectProb < 1` at this vertex, not a
second guiding-eligible candidate.) Fixed to
`kray*pS->pdf/(selectProb*combinedPdf)`. The other two trained-guiding
overwrite sites in the same block re-evaluate the material's AGGREGATE
BSDF/PDF, but re-evaluating the aggregate does NOT make them complete,
self-contained estimators immune to `selectProb` -- see DL-67 for the
derivation of why all three branches (this one, RIS-accepted, and
one-sample guided-direction-accepted) remain mutually inconsistent for a
genuinely multi-lobe SPF, and why the "would double-count" claim in an
earlier version of this paragraph was wrong. See
[docs/DEBT_LEDGER.md](DEBT_LEDGER.md) DL-42 for the full derivation,
red/green counters and sibling audit (including the new DL-67 finding).

**~~DL-43 — BDPT eye guided-candidate PDF arguments are reversed.~~ CLOSED
2026-09-13 — `a69c9ce6`, `TranslucentIORStackTest: ALL TESTS PASSED`.**
Both the RIS and one-sample eye guide branches called EvalPdfAtVertex with
(guided direction, -incoming ray direction), although the utility takes
(incoming-away, outgoing-away) and evaluates Pdf(outgoing given incoming).
The light-side twins already had the correct order. The error existed for
an ordinary Lambertian vertex and was independent of translucent mixture
state, so it received its own row rather than being folded into DL-41.
RGB/NM used the same templated eye code, so a single source fix (swapping
the two arguments at both eye-subpath call sites) closed both. Red proof:
extended `tests/TranslucentGuidedStackProbe.h`'s real trained-OpenPGL BDPT
fixture to show eye+RIS rejected every guide candidate outright
(`substituted_out=0`) and eye+one-sample carried the wrong density on every
outward substitution (`bad_pdf_value=41`); light-subpath rows already
passed. See [docs/DEBT_LEDGER.md](DEBT_LEDGER.md) DL-43 for the full
red/green counters and sibling audit.

## Verification and review

The literal touched-class search selects TranslucentIORStackTest,
CompositeExtinctionTest, SPFBSDFConsistencyTest, SPFPdfConsistencyTest,
BDPTStrategyBalanceTest and VCMStrategyBalanceTest, plus the extended
TranslucentSpectralParityTest. The balance tests mention TranslucentSPF
in comments and are included anyway. No full-suite runner is used.
The render harnesses retain explicit box filters, denoising off, linear
Rec.709 EXR fallback outputs relative to RISE_MEDIA_PATH, and per-render
std::srand seeds. Actual statistics come from linear in-memory captures;
worker scheduling means rendered repeats are not bit-reproducible.

All seven focused binaries completed successfully. Summary lines, verbatim:

```text
TranslucentSpectralParityTest: 1918 checks, 0 failures
ALL TESTS PASSED
  All checks passed
All SPF-BSDF consistency tests passed!
All SPF PDF consistency tests passed!
Passed: 66
Failed: 0
Passed: 55
Failed: 0
```

The summary order is TranslucentSpectralParityTest, TranslucentIORStackTest,
CompositeExtinctionTest, SPFBSDFConsistencyTest, SPFPdfConsistencyTest,
BDPTStrategyBalanceTest, and VCMStrategyBalanceTest. Full verbatim output,
including each per-material/per-topology counter, is in the standalone
report and archived logs. The clean make rebuild has zero compiler warnings.
The clean Xcode RISE-GUI Deployment build also succeeded with zero compiler
warnings. Its log contains the two repo-exempt notices: missing local
extlib/oidn/install/lib search path (Homebrew fallback) and skipped
AppIntents metadata extraction. No warning suppressions were added.
No source files were added or removed under src/Library, so no build-project
file lists change. The existing test is discovered by the Makefile wildcard.

Self-audit before independent review: the likely failure modes were a
one-pipe support fix, leaving a Phong density attached to a cosine sample,
accidentally applying the reflection horizon gate to exit transmission,
changing stack ownership, and overclaiming a full mixture/MIS repair.
The paired density checks, tilted-normal/entry controls, existing stack
gate, and explicit DL-03/DL-38/DL-41 residuals address those risks.

Final build and independent review evidence is retained in the standalone
report; review completion does not rely on these self-audit observations.


The first independent review round examined `6fd5413c`. Tests and
documentation reviewers found no P1/P2 issues; transport found no scoped
DL-02 blocker but identified the independent P1 defects now recorded as
DL-42 and DL-43. The supervisor verified both against their producers and
consumers. A proposed inward-guide-normal support defect was not confirmed:
the guide Sample/Pdf use the same modified distribution and the SPF part
of a mixture below alpha=1 retains exit support. Complete conditional-lobe
guiding coverage belongs with the remaining transport work; no unsupported
numeric bias or zero-support claim is recorded for that normal alone.
The final fresh review verdicts are in the standalone report.


Round 2 examined `30ef1e5e` and found no scoped DL-02 P1/P2 issue. Its
transport reviewer requested a DL-03 test-recipe clarification: require
positive interception of a guide-eligible exit, since ordinary PT
specular arrivals disable guiding. The ledger recipe now explicitly fails
on zero interceptions and names seeded PT state or verified BDPT coverage.
This is documentation only; the gated source and tests are unchanged.
