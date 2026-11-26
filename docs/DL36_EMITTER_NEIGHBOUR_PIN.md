# DL-36: bounded emitter-neighbour consistency pin

CLOSED 2026-09-12 as a consistency pin, test commit `ac9891f3`.
The unchanged production implementation passed the new fixture:
`Passed: 16  Failed: 0`.

## Decision and mechanism

The normal-aligned emitter probe stands off from a sampled point, then
accepts a closest hit within its tolerance on the same luminary. A nearby
second surface of that luminary can intercept the probe, so its live
signal channel is used while the emission geometry stays at the sample.

DL-36's recipe explicitly permits either retaining this bounded-neighbour
behavior with a regression or changing the probe to refuse it. This closure
chooses the former: it closes the missing consistency coverage, not the
physical difference across a signal discontinuity. No production source,
probe tolerance, sampling density, or signal convention changes. The
within-band neighbour remains an accepted approximation; it must not be
reported as an exact sampled-surface payload.

## Fixture and unchanged-baseline evidence

The branch starts at master `1329c68fb4d26ceee052ad0ffa1eecc93952d16b`.
`scenes/Tests/Signals/emitter_louvres.RISEscene` loads one PLY object with
two parallel blades at z=0 and z=0.001. A separate box starts at z=0.0015.
The exact test samples (0.25,-0.25,0) with normal +Z. Its accepted channel
comes from z=0.001, inside both the standoff and acceptance tolerance.
`proximity(0.001)` is zero at the sampled lower point and 0.5 at the
accepted upper point. The actual measured output is:

```text
DL-36: single-luminary two-blade probe consistency
  sample z=0  accepted z=0.001  accepted proximity=0.5  expected=0.5
Passed: 16  Failed: 0
```

The fixture uses the real PLY loader, Object intersection, scene manager,
probe and signal query, not a fabricated hit or a mock signal value. The
assertions check single-object identity, live neighbour proximity, sample
proximity absence, geometry-preserving replay, upper-blade self-read,
lower-blade reverse-normal control, invalid-normal refusal without payload
mutation, and demand-registration release. Float mesh coordinates require
small numeric bands (1e-8 for positions, 1e-6 for proximity).

**No red-proven physics fix is claimed.** Test `ac9891f3` was committed,
built, and run against the unchanged library; it passed on its first
execution. This is the honest consistency-pin disposition allowed by the
user's instructions and the ledger recipe. A missing include path was
corrected during test compilation, before that execution; no production
behavior was altered to obtain a pass.

## Shared producer and downstream audit

Pattern audited: a bounded same-luminary interception supplies the accepted
hit's channel while consumers preserve the sampled emission geometry.
The sweep follows `surface.channel` / `signals`, then `pScene`, `pSelf`,
`ptWorld`, and adjacent geometry/PDF inputs. Paths are relative to
`src/Library/`.

| Surface | STATUS | Evidence / disposition |
|---|---|---|
| `Lights/LightSampler.cpp::ProbeEmitterSurface` | VERIFIED unchanged; directly pinned | One normal-aligned probe serves RGB/NM NEE and SampleLight; accepted channel stores the actual hit position and object/scene identity. |
| `Interfaces/SurfaceSignalProximity.h::SurfaceSignalInfo::Proximity` | VERIFIED unchanged; directly pinned | Queries NearestOtherSurface through the accepted channel triple, then evaluates 1-distance/radius; replay sample position does not replace it. |
| `Lights/LightSampler.cpp::ApplyEmitterSurface` | VERIFIED unchanged; directly pinned | Copies derivatives/signals/footprint; sample position, object position, normals, ONB and UV remain unchanged. |
| `Lights/LightSampler.cpp::SampleLight` | VERIFIED unchanged | Resets payload validity, probes once, applies payload for RGB emission; sampled geometry determines position/direction PDFs. |
| `Lights/LightSampler.cpp` RGB/NM NEE | VERIFIED unchanged | Same producer and replay helper, so no wavelength-specific acceptance rule. |
| `Shaders/BDPTIntegrator.cpp::GenerateLightSubpathImpl` | VERIFIED shared consumer | RGB uses sampled Le; NM hero and HWSS companion rebuilds apply shared payload. LIGHT root copies derivatives/signals/footprint. |
| `Utilities/PathVertexEval.h::PopulateRIGFromVertex` | VERIFIED unchanged | Replays the channel while reconstructing geometry from the vertex's sampled fields. |
| `Shaders/VCMIntegrator.cpp` | VERIFIED shared consumer | Uses BDPT-generated subpaths and shared payload for sampled-emitter evaluation. |
| `Rendering/MLTRasterizer.cpp`, `MLTSpectralRasterizer.cpp` | VERIFIED shared consumer | RGB/NM/HWSS use BDPT light-subpath generation and evaluation. |
| `Shaders/DirectLightingShaderOp.cpp` | VERIFIED unchanged | RGB/NM delegate to paired LightSampler direct-lighting methods. |
| BSDF/SMS/coated/composite/fabric sampling | INAPPLICABLE to this acceptance rule | They do not implement a second emitter-record probe; emitter records continue through the shared lighting interfaces. |

The new fixture directly exercises the producer, replay helper and live
channel evaluation. It is not a rendered end-to-end test of every listed
consumer. Existing SignalEmitterRecordTest render families remain the
broader gate. DL-19's individual NM/HWSS conversion red-proof gap and
DL-22's BSSRDF entry payload remain open; this pin does not close them.
Independent review found a separate sampled-UV propagation defect, filed
as DL-44: SampleLight evaluates RGB emission using its local sampled coord,
but LightSample carried no UV, so BDPT NM/HWSS rebuilds, its LIGHT root,
and VCM rebuilt emission retained default (0,0), which UV-dependent
painters consume; MLT shares BDPT generation. **DL-44 CLOSED 2026-09-14**
(`18892254`) — `LightSample::ptCoord` now carries the sampled UV through
every one of those sites; red-proof `tests/EmitterUVSampleTest.cpp`
measured the predicted closed-form 2.0x bias on a checker-textured
luminary and confirmed it gone post-fix. See
[DL44_LIGHTSAMPLE_UV.md](DL44_LIGHTSAMPLE_UV.md).
The existing DL-40 nonfinite
comparison gap also applied to this test harness: ComputeStats marked
nonempty captures valid without finite checks, and WorstRelDiff used fmax,
which could discard a NaN difference. The recorded render means in this
run were checked finite; no nonfinite-handling correctness was claimed at
the time. **DL-40 CLOSED 2026-09-14 (debt-cov slice)**: `ComputeStats`
now rejects a capture with a nonfinite composited component, and
`WorstRelDiff` now returns `HUGE_VAL` (rather than silently discarding a
NaN operand via `std::fmax`) when either `ImageStats` carries one. See
[docs/DEBT_LEDGER.md](DEBT_LEDGER.md) DL-40.

## Gate and measurement hygiene

Only tests, fixtures and documentation change. No production class changes,
so the focused gate is the complete changed SignalEmitterRecordTest binary;
there is no changed-class reason to run unrelated suites. The new
`--louvres-only` option is a fast exact fixture check, not a replacement for
the complete binary gate. No full-suite runner is used.

The existing harness already disables denoising, uses box filtering and
seeds each render with srand(seedBase+renderIndex). Its removed fallback
output chunk is now relative-path linear Rec.709 EXR instead of absolute
PNG/sRGB; actual statistics use the existing in-memory capture. Repeats
remain subject to thread scheduling. No existing rendering tolerance is
changed. The standalone report preserves all counter lines and complete
stdout. The fresh isolated make build supplies the warning gate; no
Library file is added/removed and none of the five source lists changes.

Full gate/build and independent review evidence is recorded in the
standalone report after completion.


The complete changed suite passed on the branch:

```text
Passed: 93  Failed: 0
```

The initial isolated build and explicit test compilation have zero warning
or error lines. No production class was edited; the initial fresh build
compiled the complete library from this row's master baseline.

Self-audit before review: the likely failures were a mock-only fixture,
a channel indistinguishable from neutral, replay changing emission geometry,
a leaked demand registration invalidating later gate-closed tests, and
mislabeling a baseline pass as a red proof. The real parsed PLY scene,
0.5-versus-zero signal oracle, preserved-geometry assertion, explicit demand
release assertion, and consistency-pin wording address those risks.


The clean Xcode RISE-GUI Deployment build succeeded with zero compiler
warnings. It emitted only the two repository-exempt notices: missing
local OIDN search path (Homebrew fallback) and skipped AppIntents metadata
extraction. No warning suppression was added.
