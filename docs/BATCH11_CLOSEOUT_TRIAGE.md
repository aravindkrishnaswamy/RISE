# Batch 11 close-out triage

Proposals for the owner, 2026-10-10. **No verdict below is applied to the debt ledger.** This batch changes no transport. The only additional code here is a DL-482 measurement test; DL-571 is independently merged at `c344a4612`.

| Row | Proposed verdict | Current residual / practical exposure | Pin disposition |
|---|---|---|---|
| DL-331 | PARK | Photon/PT 0.983582 immersed / 0.990501 air (SMS n4, PT n32); six opt-in scenes | Retain high-precision reference check; original low-budget gate currently green |
| DL-367 | CLOSE, residual accepted | Unmatched VM/PT 1.0016534, matched 0.99980868 (n8 each); reference batching, below the existing 0.8% budget | Owner may re-band unmatched probe to that budget; retain matched/replay gates |
| DL-474 | PARK | Corner fraction 0.310329 vs 0.310241; uncertified cuts remain | Keep exact-corner pin red pending acceptance of bounded accuracy |
| DL-475 | PARK | BDPT 0 vs PT 0.0635775 (n8); auto guards explicit bound-medium class | Keep manual-BDPT energy pin red |
| DL-482 | KEEP OPEN | Rough-SSS reflection count 1 interior / 0 endpoint; finite glossy caps | Add the bounded reflection declaration, then require strict parity |
| DL-483 | PARK | Weave gap consumes 1 transmission bounce instead of 0 | Keep gap pin red; mixed guided labels need a causal gate |
| DL-500 | PARK | Extended/PT-off 118.916x SD14.491 / 6.630x SD0.387, n8 pairs each; extended SMS is opt-in, one live test scene | Retain cost diagnostic and positive ownership gates |
| DL-502 | PARK | Positive coat 9 / weave 18 declaration failures; no shipped finite-cap BDPT/VCM | Keep both strict declaration pins red |
| DL-531 | CLOSE, residual accepted | Created frames 0 SD0, n4; correct conservative fallback; intentional conservative fallback | Keep fallback safety test green |
| DL-532 | KEEP OPEN | Generic 9,998 allocations / 4,999 evaluations; exact Object has 0 | Preserve retaining-target test; add audited immediate-consumer counter |
| DL-465(2) | PARK | Zero shipped VCM cameras with nonzero scanning/pixel rate | Add a rolling-shutter merge oracle before a fix |
| DL-465(3) | PARK | Zero shipped spot-target timelines; authored Hermite witness 179.427° vs sampled <=0.573° | Add an overshoot/BVH parity witness before a fix |

## Measurement and exposure method

Measurements use local master library `d50f5c9a2`; master `c344a4612` adds only the console fix, its test and documentation, with identical Library sources. Every executable was rebuilt by its named `make -C build/make/rise -j8 build-test/<Name>` target and its build return code checked. The Library build is warning-free. Our render measurements run sequentially under the shared render lock, with `force_number_of_threads 1`, `render_thread_reserve_count 0`; U also disables progressive radius. Other agents use the same machine; wall-time SD includes uncontrolled background load. Interleaved extended/off pairs measure current cost, not a before/after speedup. Repeated renders use `SobolSamplerTestHooks::ValueSalt`. SD is the sample standard deviation across complete rendered image means, except paired timing ratios as labelled. Deterministic checks are reported as n1 / SD0 by construction, not as measured Monte Carlo uncertainty. The 512 SPF draws below are seeded draws, not 512 independent renders.

The exposure census reads all **487** tracked `.RISEscene` files, including test and feature scenes, strips block comments and full-line `#` comments, then inventories native chunks/parameters/timelines. Commented alternative rasterizers do not count. This is an authored-text census, not scene-graph execution, import resolution or a proof about arbitrary API clients. Absence in this census does not make an authored/API setup unreachable.

Defaults: `StabilityConfig` initializes all four surface-type caps to `UINT_MAX`; `SMSConfig` uses `photonCount=0`, `extended=false`; camera parser exposure/scanning/pixel rates default to zero. Thus none of the capped multi-label residuals or optional SMS modes is enabled by these defaults. There is **no live BDPT/VCM/auto chunk with a finite per-type surface cap** in the census. The only scene explicitly setting finite glossy/transmission/translucent caps is `scenes/Tests/Volumes/pt_volumetric_caustics.RISEscene` (all 2, PT with guiding, not an SSS receiver). Many PT scenes set a diffuse cap; this does not establish bidirectional exposure. Defaults do not exclude manually authored capped or moving-media scenes.

## DL-331 — photon-seeded SMS energy

**Still fails.** The separate high-precision photon/PT comparison is outside three SE in both fixtures; the original low-budget gate currently passes. Immersed photon/PT is **0.9828399** (n4, 200000 photons, 128 spp PT/SMS, 16 seeds, default solver threshold): PT **0.120257790 SD0.00136503656**, photon **0.118194151 SD0.000302596166**; absolute three-SE band **0.00209726031**, within=1. This fresh low-replica deficit is not resolved at that uncertainty; it does not reproduce the older, tighter failing band. Snell/PT **1.008253**, Snell **0.121250256 SD0.00112032802**, also within its band. Air photon/PT **0.9799490**, PT **0.103359574 SD0.00206492685**, photon **0.101287113 SD0.0000784211054**, band **0.00309962316**, within=1. Air Snell/PT **0.9740513**, Snell **0.100677531 SD0.0000661378981**, also within its band. The actual low-budget `--gate` invocation returns **rc0**, contrary to the pre-run expected rc1; no setup/install check failed. A separate SMS-disabled PT reference at **1024 spp / n32** gives immersed **0.120167087 SD0.000560611879** and air **0.102258476 SD0.000348766944**. Against the completed n4 photon samples above, ratios are **0.983581727 / 0.990500905**, differences **-0.001972936 / -0.000971363**, combined three-SE bands **0.000542598 / 0.000219199**: **both fail**, strict calculation rc1. Reference command: `SMSPhotonSeedEnergyTest 32 air 1 1024 16 1 0`; only its PT vector is used. Its one-photon/one-spp SMS diagnostics are discarded, not claimed as energy results. Distinct PT versus photon value-salt slots are used, and counts are not pooled across the two executions. The low-budget test rc0 and this separate-reference rc1 are different checks.

**Affected.** Six live scenes set positive `sms_photon_count`: `scenes/Tests/PathTracing/pt_torus_chain_atrium.RISEscene` and SMS test scenes `sms_k2_glasssphere_tess_disp`, `sms_slab_close_sms`, `sms_teapot_close_sms`, `sms_k2_torus_cross`, `sms_slab_close_pt_sms_hispp`. This identifies use of the affected mode, not proof that each scene contains the failing immersed fixture. The pool VCM scene's apparent photon setting was in a commented alternative. Default photon count zero avoids this mode. Nested/immersed authored casters and photon-assisted caustic workflows can encounter it; the air control also has a negative point estimate.

**Recommendation: PARK.** Native interface-index replay is fixed, but finite root discovery, solver convergence and deduplication are still entangled; a throughput scale would hide the failure. There is no demonstrated small next transport fix. Resume only with a root-set coverage/convergence oracle, not another parameter sweep as a closure.

**Pin.** `SMSPhotonSeedEnergyTest 4 air 200000 128 16 128 0 --gate`; photon/PT immersed and air comparisons use independent SMS-off PT and three combined SE. The actual n4/PT128 invocation is green at current uncertainty. Retain the energy oracle; do not widen its band. The two-budget reference check above is red and should remain a diagnostic pin until transport is corrected or the owner accepts the residual. Increase the PT oracle budget for future normative in-test runs rather than claiming the low-budget green is a physics fix. Snell-only results are printed controls, not enforced by this gate. No pins are removed or bands changed here.

The separate-reference check can be reproduced after capturing the two commands above as `photon.log` and `pt.log`:

```sh
python3 - photon.log pt.log <<'PY'
import math, re, sys
pattern = r"DL331 mode=(\d+) n=(\d+) PT ([\deE.+-]+) SE ([\deE.+-]+) SD ([\deE.+-]+) photon ([\deE.+-]+) SE ([\deE.+-]+) SD ([\deE.+-]+)"
def read(path):
    return {int(v[0]): tuple(map(float, v[1:]))
            for v in re.findall(pattern, open(path).read())}
photons, refs = read(sys.argv[1]), read(sys.argv[2])
assert set(photons) == set(refs) == {0, 2}
within = []
for mode in (0, 2):
    p, r = photons[mode], refs[mode]
    assert p[0] == 4 and r[0] == 32
    band = 3 * math.hypot(p[6] / math.sqrt(p[0]), r[3] / math.sqrt(r[0]))
    within.append(abs(p[4] - r[1]) <= band)
    print(mode, p[4] / r[1], band, within[-1])
raise SystemExit(0 if all(within) else 1)
PY
```


## DL-367 — U reference batching

**Still fails.** At fixed radius 0.02 / 8192 spp, eight salts, unmatched batching gives VM-on/PT **1.0016534** (+0.16534%): PT **0.0356833346 SD9.69374891e-6**, VM-on **0.0357423321 SD1.75109906e-5**, VM-off **0.0356911862 SD1.56415666e-5**; VM/PT absolute three-SE band **2.12292017e-5**, strict gate **27/1 rc1**. Matched one-sample/pass references give **0.99980868**, PT **0.0357491716 SD9.66367448e-6**, same VM-on mean/SD, VM-off **0.0357572234 SD1.80647175e-5**; band **2.12137708e-5**, **28/0 rc0**. These are fresh n8 reruns, not pooled with earlier uses of the same salts.

**Affected.** VM-on forces one sample/pass whereas ordinary PT and VM-off use 32 by default (`ProgressiveConfig`). The unmatched comparator therefore compares different finite-budget sampling sequences even at fixed radius. Shipped VCM users can get different finite-sample readings than PT; current results do not prove that either mean is exact physical truth, and do not establish an estimator bias. This is not just a scene-parser default issue.

**Recommendation: CLOSE as residual accepted**, contingent on the owner accepting the measured finite-budget difference inside U's already established +/-0.8% physical parity budget. Matched references pass the narrower noise test; unmatched differences are statistically resolved but practically small in this fixture. Do not call this an unbiased-estimator proof, blame merge-radius blur, or recenter a band on the observed ratio.

**Pins.** `VCMStrategyBalanceTest --dl367-strict-only` (matched) must remain green. The unmatched `--dl367-unmatched-only` should remain red until the owner rules; if accepted, re-band its normative assertion to the existing 0.8% budget and retain the difference/SE diagnostic. Keep the salted default-U and bit-replay checks; remove none here.

## DL-474 — volume kernel boundaries

**Still fails.** The own-box corner upper fraction is **0.310329**, versus independent 1024x1024 midpoint integration **0.310241**, approximately 8.8e-5 apart; `VCMVolumeBoundaryRadiusTest --strict-exact-corner-pin` is **20/1, rc1**, n1/SD0. The single-sphere and plane fractions are **0.714983 / 0.683594**. The corner reference is numerical, not a certified lower bound. This fraction discrepancy is not a bound on complete scene radiance error.

**Affected.** VCM volume merging near a medium boundary, especially widened thinning kernels. Four live VCM+medium scene files (three `global_medium`, one `interior_medium`) are `scenes/Tests/Volumes/{env_unbounded_thin_vcm,vcm_env_through_fog,env_bounded_fog_vcm}.RISEscene` and `scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene`. File co-occurrence does not prove a nearby cut; the unbounded test need not have one. Certified own-box corners have the implemented conservative correction. Independent objects and uncertified multi-cut geometry remain distinct residuals, not measured closed by this pin. VCM with an authored medium can reach these without an extra boundary-correction opt-in.

**Recommendation: PARK.** The measured corner approximation is small enough to consider accepting its bounded accuracy, but the whole row cannot close on one own-box fraction. A future fix needs a certified clipped-kernel measure for independent/uncertified cuts and an actual energy oracle; scope is not a small one-plane patch.

**Pin.** Keep exact-corner red pending an owner accuracy ruling. If the owner accepts this approximation, replace exact-to-1e-6 demand with the already passing conservative/corner-budget checks (currently 0.002 absolute fraction), retaining kernel/MIS reciprocity and boundary-free identity. That accepts only the own-box approximation, not all boundary geometries. No rebinding here.

## DL-475 — delta-walled one-scatter path

**Still fails.** With the index-matched fog-box fixture and transparent-shadow PT, 256 spp PT/BDPT and 1024 spp VCM, eight salts: **PT 0.0635774787 SD0.000109928939; BDPT 0 SD0; VCM 0.0637908141 SD0.00246591073**. VCM/PT is **1.00335552**; paired VCM-minus-PT mean **0.000213335337 SD0.00243178283**. Manual BDPT misses the path completely; the gate is **9/1, rc1**.

**Affected.** A point-position light illuminates a medium whose only connectible vertex is between delta walls. For C-S-M-S-L the conventional splits are (s,t)=(0,5),(1,4),(2,3),(3,2),(4,1): s=0 cannot hit the point emitter and every other split connects at a delta surface. Ordinary NEE from M to L skips S and visibility rejects it; a generalized strategy would have to insert and price S. This is the construction proof in `BATCH10_DL475_DESIGN.md`, not a missing weight on an existing strategy. The current auto rasterizer detects world-visible transmissive objects with explicit bound media plus positional delta lights, and routes away from BDPT with a warning. Nested bindings hidden from that root-object census remain a limitation; this is not an all-scenes support guarantee. The scene census finds zero live BDPT scene files co-occurring with `interior_medium` and omni/spot light chunks, but authored/API clients can explicitly choose BDPT and bypass auto. Plain media and area-light connections are not established as failing by this fixture.

**Recommendation: PARK.** Manual BDPT is genuinely incomplete for this narrow path set, but the auto workflow is guarded. Adding a constrained one-medium-scatter sampling strategy and its complete `MISWeight` density alternatives is a design-sized change; no demonstrated small strategy is ready to implement.

**Pin.** Keep `BDPTMediumDeltaStrategyTest --strict-energy-pin` red; preserve its green reachability/auto-routing assertions and independent PT/VCM comparison. Do not replace zero with a loose band. No production fix in batch 11.

## DL-482 — residual SSS reflection accounting

**Still fails.** The same rough SSS reflection consumes **one glossy bounce / over-cap** as an interior but **zero / accepted** as a connection endpoint at glossy cap zero, for diffusion and random-walk materials (n1/SD0 metadata). New `SSSReflectionCapCloseoutTest --strict-reflection-pin` has **two failures, rc1**. Actual material SPF draws independently witness reflection labels: **510/512 RGB and 512/512 NM**, zero wrong labels, for each material. These are native SPF draws plus synthetic joined-path metadata, not an emitted complete path or an energy measurement. The positive subsurface-jump metadata gate now passes **5/0** and counts two jumps against cap one; it is not the residual.

**Affected.** Authored BDPT/VCM rough-SSS receivers with a binding `max_glossy_bounce`; default unlimited caps and all shipped bidirectional chunks avoid the refusal. SSS scenes include `rwsss_bdpt`, `rwsss_sphere`, `rwsss_thin_slab`, `sss_comparison_dragon`, but their existence alone does not activate this cap. Off-limits contact/exit code is not needed to characterize this label discrepancy.

**Recommendation: KEEP OPEN.** Concrete next slice: declare the finite reflection BSDF's connection label for diffusion/random-walk SSS and provide its single-label split, without altering the separately counted translucent jump. Estimate **1–2 days**, including an actual production-generated glossy-cap endpoint witness and PT-gated render. First verify every ordinary BSDF-supported endpoint term is reflection before making the declaration; preserve `EndpointBounceType`'s existing `isBSSRDFEntry` exclusion for re-emission/angular exits. Do not tag the subsurface jump glossy. This bounded interface/BSDF work must respect the supervisor's contact-code ownership.

**Pins.** Keep new strict reflection pin red until parity, keep its ordinary witness-validation mode green (also accepts a future corrected count), and retain positive-jump gates. The new test writes none of the off-limits contact fields and changes no renderer code. n1 metadata establishes accounting mismatch, not the size of energy loss.

## DL-483 — weave gaps and mixed guided draws

**Still fails.** `BDPTDepthCapMISTest` row M reads weave-gap transmission count **1 instead of 0**, status over-cap instead of accepted at cap zero (**1/1, rc1**, n1/SD0 metadata). Its single-label guided control is now green: diffuse **0/0**, both strategies accepted. Mixed-label aggregate guided draws still use a diffuse label; their remaining bidirectional radiance effect has no isolated current energy pin. Do not reuse the rejected batch-7 shell render as a quantitative oracle.

**Affected.** A transmission cap on see-through weave gaps; a binding per-type cap on guided mixed-label materials. Shipped weave examples include `FeatureBased/Materials/{sheer_curtain,denim_and_satin_drape,fabric_swatches}.RISEscene`; no finite-cap bidirectional chunk is authored there. Guiding appears mainly in PT tests, including `pt_volumetric_caustics` with caps 2, so an authored guided cap is not categorically unreachable. Default guiding/caps do not activate the residual combination.

**Recommendation: PARK.** Gap accounting is real, but the metadata fixture is not a causal radiance gate and the mixed-guided-label rule needs a reciprocal sampling-label design. Resume after producing an uncapped-independent-oracle path witness; avoid simply switching every aggregate guide draw to a guessed lobe.

**Pin.** Keep `RISE_DL351_ROWS=M BDPTDepthCapMISTest` red for the gap and its single-label control green. Mixed guided behavior needs an additional native mixed-material witness before any closure; no claim that row M currently pins that residual.

## DL-500 — extended SMS cost

**Still fails.** Eight alternating AB/BA pairs per scene, 64 spp, one worker/reserve0, **extended SMS versus SMS-disabled PT**: DL-372 extended **5.82383170 s SD0.01902708**, off **0.04983075 s SD0.00803358**, paired ratio **118.915783 SD14.490668**; resized shipped sphere extended **0.16724254 s SD0.00813731**, off **0.02524535 s SD0.00073844**, paired ratio **6.63034550 SD0.38715040**. Gate **114/0 rc0**. This is wall time including fixture creation/render/output/counters, not isolated solver CPU time. Ratios against fully SMS-disabled PT are not interchangeable with older extended-versus-legacy-SMS comparisons, nor evidence of a new slowdown. The DL-372 fixture uses its native 24x24 film; only the shipped sphere is resized.

**Affected.** `sms_extended` defaults false. The one live opt-in file is `scenes/Tests/SMS/sms_k2_glasssphere_extended.RISEscene`; API users and manually authored extended scenes can encounter the canonical-solve cost. The measurements preserve that scene's geometry/camera/materials but resize its film to 24x18 and use 64 spp; they do not report its full-resolution shipped render time.

**Recommendation: PARK.** Existing RTTI/capture memoization is landed; no further measured, answer-preserving lever of similar size is established. First-root/endpoint-only shortcuts change the owned root set and need an estimator ruling, not a perf-only patch. No speedup is claimed by this batch.

**Pin.** `SMSExtendedPartitionTest --section dl500-baseline` is a cost diagnostic, not a strict cost failure: no agreed time threshold exists. Keep it and the green ownership/domain/HWSS gates. Do not invent a red threshold from machine wall time or remove ownership tests to close a performance row.

## DL-502 — remaining mixed material splits

**Still fails.** `ConnectionTypeSplitTest --dl502-coated-pin` is **36976/9, rc1** for positive coat coverage; `--dl502-weave-pin` is **92431/18, rc1** (n1/SD0 deterministic assertion counts, not radiance SD). Existing exact zero-coverage coat and single-label fabric/hair cases pass their checks. Positive/textured coat, mixed fabric substrates and weave lack a reciprocal per-label connection evaluator.

**Affected.** Binding per-type caps in BDPT/VCM on these receivers; no shipped bidirectional chunk sets one. Materials themselves are shipped, for example `FeatureBased/Materials/{lacquer_and_rain_still_life,rainwet_cobbles,rainwet_courtyard_night,sheer_curtain}.RISEscene`; their normal cap-free rendering is not shown failing by this test. Manually capped point-caustic receivers can lose strategies, so the absence in shipped defaults does not prove harmlessness.

**Recommendation: PARK.** Forward sampling-density shares alone do not solve reverse consistency. The next useful step is a reciprocal labelled-path measure for mixed coat/weave, with walk-vs-quadrature and capped end-to-end gates; no bounded correct split has been demonstrated. Do not thaw composite or the frozen materials.

**Pins.** Keep both strict pins red. Their failure is a declaration/coverage pin, not a current n-salt energy estimate for arbitrary positive coats. Retain positive split identity/MC/quadrature gates and zero-coverage cap-free hashes; remove none here.

## DL-531 — safe fallback outside the clone whitelist

**Still fails.** Four independent salted 8-spp fallback renders all create **zero time-indexed frames, mean0 SD0** and succeed (each rc0). Image mean **0.188009697 SD0.004137193** is a completion diagnostic, not a difference against an independent reference. `TimeIndexedMotionBlurTest C 8` is **2/0 rc0**. This is a still-active, intentional single-thread fallback, not a radiance failure.

**Affected.** Correctly single-threaded motion-blur frames with unsupported geometry/material/painter consumers. Census candidates with explicit painter/geometry timelines are `Tests/Painters/fractal`, `Tests/VCM/pool_caustics_vcm`, `FeatureBased/Caustics/pool_caustics`, `FeatureBased/SDF/sdf_morph_torture`; none has positive camera exposure (omitted means zero). An author enabling blur on fractal, displacement or SDF keyframes can encounter the fallback; it is not active in these files' current camera defaults. Exact shading-only UniformColorPainter/Lambertian bindings already clone.

**Recommendation: CLOSE as residual accepted.** Conservative fallback is a safe supported behavior, not wrong radiance; general geometry/displacement/material cloning is a feature expansion with substantial consumer/lifetime auditing. Accept the warning and single-thread cost outside the exact whitelist. This does not claim fast rendering for every authored animation.

**Pin.** `TimeIndexedMotionBlurTest` section C is intentionally green and must stay green: the fixture with unverified geometry is refused. There is no strict failing energy check to remove or re-band for this residual. The four-salt fallback measurement verifies current refusal, not an old/new image-identity comparison or speedup.

## DL-532 — remaining owned interpolation results

**Still fails.** `KeyframeAllocationTest --zero-allocation-pin` reports **9,998 allocations / 4,999 interpolations, zero bit mismatches**, retained results survive, rc1 (n1/SD0 deterministic counter). `ObjectKeyframeAllocationTest` reports **0 allocations / 4,999, zero mismatches**, rc0. Generic/retaining targets still require owned final results; nonlinear/custom interpolation and other posing costs are separate residuals. The old 57% profile is historical and is not a current-build measurement.

**Affected.** Positive-exposure authored animation evaluating camera/light or other non-whitelisted timelines per sample; no-animation/static frames avoid this loop. Shipped camera/light timelines occur in `Tests/Geometry/teapot_analytic` and watch scenes; geometry/displacement timelines also take the correct conservative old path. Zero default camera exposure avoids per-sample blur interpolation, but normal animation-frame evaluation can still allocate. The synthetic retaining Scalar test does not measure a shipped camera's allocation count or frame slowdown.

**Recommendation: KEEP OPEN.** Concrete next slice: audit immediate-copy setters for exact built-in cameras and point/spot/directional lights, whitelist only non-retaining consumers for scratch final results, and add native allocation counters plus one-thread pixel-hash gates. Estimate **1–2 days**. Leave unknown/retaining/custom targets owned; generic zero-allocation is not a safe universal contract. No current frame speedup estimate is supported by these counters.

**Pin.** Generic zero-allocation stays red as a documented over-broad demand until the owner rules; retaining-value lifetime test stays green. A camera/light immediate-consumer counter should become the normative red-before/green-after gate for the next slice. Preserve current exact Object counter/hash gates. No timing comparison is performed for this row in batch 11.

## Optional DL-465(2)/(3) — reachability, no production change

**(2) PARK.** Census n1/SD0: **zero** live VCM cameras with nonzero scanning/pixel rate. `Tests/VCM/diacaustic_vcm` explicitly has both rates zero; `Tests/Animation/animation_temporal_scanning` uses scanning rate 0.001 but PT. Current rasterizer source explicitly warns that positive exposure plus nonzero rates keeps per-sample eye times and a frame-time merging light store. An author switching a moving rolling-shutter scene to VCM with merging can reach it; default exposure/rates zero avoid it. There is no current salted energy measurement or strict failing pin for this combination. Next requires a shutter-time-indexed merge store or justified temporal kernel and an independent per-pixel shutter oracle; not a bounded console/metadata fix. Add a strict render pin before changing transport.

**(3) PARK.** Census n1/SD0: **zero** authored spotlight-direction/cone timelines. Three files contain both spotlight and timelines, but those timelines animate painter/object values, not spot direction. The sweep samples 65 uniform times plus keyframes and pads sampled bounds; `LightBVH` consumes the resulting motion cone. Sampling does not certify a rapidly overshooting authored interpolation between those times. An independent evaluation of the production `HermiteInterpolator` formula (tension/bias/continuity zero) gives a concrete authored target witness: light at origin, target=(x,0.01,0), keyframes (time,x)=(0,17), (0.5,1), (0.501,1), (1,17), shutter [0,1]. All **66 distinct sweep/keyframe times** have x>=1 and directions **0.0337034°–0.572939°**, with half-largest sampled angular change **0.0614227°**. At time **0.5005**, x=-1 and direction **179.427061°** (n1/SD0 deterministic formula check). `SpotLight` supports target keyframes and regenerates its direction from that target; this demonstrates a supported authored cone outside the sampled envelope. It is not a native full-render/BVH-on-off energy measurement, and no energy magnitude is claimed. Default light BVH is on, so an authored positive-exposure cone can reach the unproved-bound case. No current strict failing cone pin exists; add an interval-bound/overshoot witness before fixing or closing. No claims of energy magnitude, no off-limits source edits.

## Build and status record

All ten named test builds and the new reflection-test rebuild returned **rc0**, no compiler warnings. Actual status record: corner exact **1**; reflection ordinary **0**, strict **1**; positive-jump metadata **0**; weave-gap **1**; generic allocation strict **1**, Object allocation **0**; positive-coat/weave split pins **1/1**; manual-BDPT energy **1**; matched/unmatched U **0/1**; low-budget photon energy **0** (the original expectation of rc1 was not reproduced), PT reference-only measurement **0**, separate-reference comparison **1**; SMS cost **0**; four fallback children and section C **0**. Intentional strict residuals are not presented as positive regression gates. No full suite was run.

The CLI-only production change has two fresh zero-P1 reviews and its own audit at [BATCH11_CLI_EXIT_AUDIT.md](BATCH11_CLI_EXIT_AUDIT.md). These triage recommendations intentionally leave the ledger unchanged. No frozen feature, shipped scene or supervisor-owned contact source is edited, no file is deleted, and nothing is pushed.

Self-review: checked measurement units and actual spp, default/census versus arbitrary authored exposure, numerical fraction versus radiance bounds, native SPF versus synthetic path metadata, and retaining-target lifetime versus allocation goals. The new witness keeps materials alive through each joined-plan query and does not own or dereference contact pointers. No thresholds or production transport were changed.
