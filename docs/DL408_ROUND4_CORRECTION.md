# DL-408 round-4 verification and correction

The owner clarified on 2026-10-11 that the 1.33 / 1.50 / 1.47 diffusion pad/floor ratios were measured on `8df8b6dff`, not `6f4df8775`. Round 4 (`6f4df8775`) already changed closest-hit ownership but was merged without testing or review. This task verifies that original change separately from the additional correction below. DL-557 is reopened, not fixed: an ideal index-1.0 lossless enclosure changing the image by -5.5 to -6% is a bias.

## Design before the bounded correction

The first surface must be the coincident neighbouring face whenever a ray arrives from outside the SSS body. The ray may be inside an adjacent solid: crossing its exit still precedes entry into SSS. The existing `ObjectContainsPoint(candidate, before)` veto incorrectly rejected this case. The initial bounded design removes that veto and preserves the after-point inset/enclosure rule. The additional self-review below refines the coincidence comparison without changing the exit launch. Forward `bComputeExitInfo` when recasting the selected neighbour, retaining the original four-argument helper as an ABI-compatible wrapper. Update the stale header description of the actual same-ray algorithm. No sampler, density, MIS, contact launch, or SSS exit behavior changes.

## Independent review of the original change

A fresh reviewer inspected exactly `8df8b6dff..6f4df8775` and found one P1: adjacent-solid exits are rejected, leaving the first surface dependent on traversal order and potentially losing a transmitting neighbour's IOR transition. Two P2s were also corrected: the caller's exit-data request was discarded, and the header described a different cast algorithm. This prevents closing DL-408 on the original round-4 code alone.

## Regression design

`SSSContactContinuationTest` now embeds the supplied original 40x30 perspective pad/floor scene, with conservative RW and diffusion blocks, no contribution clamp, PT/BDPT/VCM, and the supplied 2e-6 separated twin. The outside-lit room rows use the supplied depth-12 configuration. New radiance rows use eight `SobolSamplerTestHooks::ValueSalt` repeats (the unclamped diffusion VCM row uses sixteen) and a three-standard-error paired band without a tolerance floor. Distinct generated filenames avoid the case-insensitive `_g` / `_G` collision.

A deterministic ray test places an adjacent transmitting dielectric solid below the SSS body and originates the ray inside the neighbour but outside SSS. It checks the helper, complete manager traversal, and requested exit metadata for diffusion/RW under linear/BVH traversal: 512 rays per check. The manager modes are explicitly selected before scene loading. The pad is omitted before loading; the prepared acceleration structure is never mutated. `--ownership-only` runs this test. `--identity-only` hashes 32 separated-scene raw RGBA renders (eight fixtures, four salts), using one worker for before/after bit identity.

## Evidence

All required gates below now pass; final independent review is pending and no closure is claimed yet. Native before/after binaries use the same harness and current library context, replacing only the two production source files changed by `8df8b6dff..6f4df8775` for the historical comparison. The historical harness omits its direct call to the not-yet-existing resolver while retaining actual ObjectManager ownership checks. Focused ASan/UBSan instruments the two changed production translation units and the test; other library objects retain the native build.

### Original round-4 production verification (source-controlled comparison)

The production source comparison (`8df8b6dff` versus `b1176c996`, same current library context) reproduces every supplied failure and shows the already-merged round-4 change fixes those witnesses. Each cell is contact/gap ratio followed by contact/gap raw radiance SD; n=8 paired salted renders, 256 spp PT and room, 128 spp pad BDPT/VCM. These are radiance results, not an approval of the remaining first-surface contract.

| Fixture | Before | Original round 4 on master |
|---|---|---|
| RW pad/floor PT | 0.955910; SD 0.000102495 / 0.000102626 | 1.001810; SD 0.0000920955 / 0.0000608397 |
| RW pad/floor BDPT | 1.074240; SD 0.0000888658 / 0.0000909417 | 1.002840; SD 0.0000580747 / 0.0000668341 |
| RW pad/floor VCM | 1.057030; SD 0.000106532 / 0.000113930 | 0.989361; SD 0.0000870816 / 0.000162003 |
| Diffusion pad/floor PT | 1.320210; SD 0.000215225 / 0.0000889835 | 0.999450; SD 0.0000928808 / 0.000105794 |
| Diffusion pad/floor BDPT | 1.497470; SD 0.000214388 / 0.000126399 | 0.998789; SD 0.000109738 / 0.000105552 |
| Diffusion pad/floor VCM | 1.491000; SD 0.000293583 / 0.000163010 | 1.006160; SD 0.0000991514 / 0.000140640 |
| Outside-lit room BDPT | 1.021800; SD 0.000890192 / 0.000820377 | 1.000490; SD 0.000958616 / 0.001000850 |
| Outside-lit room VCM | 1.024400; SD 0.002999440 / 0.001211760 | 1.000480; SD 0.000855938 / 0.001155840 |

Self-review checked three likely failure modes: admitting a flush inset (the after-point/enclosure veto remains, existing inset render gate); dropping recast exit information (direct-neighbour metadata oracle); changing unrelated renders through the new overload or code generation (32 raw pixel hashes). These are distinct from the pad/floor gain already fixed by round 4.

### Corrected build radiance check

Full-suite salt placement, n=8, same sample budgets as above:

| Fixture | Contact/gap ratio | Ratio SD | Contact SD / reference SD |
|---|---:|---:|---:|
| RW pad/floor PT | 0.998756 | 0.00348256 | 0.0000386371 / 0.0000352688 |
| RW pad/floor BDPT | 1.002620 | 0.00878697 | 0.000113173 / 0.0000909123 |
| RW pad/floor VCM | 0.998994 | 0.01206100 | 0.0000985606 / 0.0001263760 |
| Diffusion pad/floor PT | 1.001650 | 0.00332523 | 0.000113526 / 0.000110922 |
| Diffusion pad/floor BDPT | 0.999516 | 0.00640771 | 0.000177717 / 0.000170681 |
| Diffusion pad/floor VCM | 1.000670 | 0.01515900 | 0.000164591 / 0.000198656 |
| Outside-lit room BDPT | 1.000440 | 0.00211163 | 0.000694214 / 0.001238830 |
| Outside-lit room VCM | 1.001480 | 0.00287254 | 0.001202120 / 0.000791854 |

The additional VCM exterior-light on/off control initially failed at +0.49% (n=8; on 0.358544 SD 0.000864703, off 0.356796 SD 0.000267031; ratio SD 0.00225092). Its auto merge radius changed from about 0.008 to 0.035 because removing the blocked exterior light changes the distribution of traced light segments. The control now fixes `merge_radius 0.005` on both sides to isolate occlusion at the same finite kernel; its three-standard-error band is unchanged. The supplied adaptive-radius contact/gap room row is unchanged and already passes. No VCM estimator or parked radius-related row is modified.

### Self-review: a non-contact sheet was incorrectly promoted

A sheet 2e-6 behind the SSS entry must remain behind it. At x=0 the body won; at x=10,000 the original `1e-3 * delta` window grew to 1e-5 and promoted the sheet (native focused probe rc=1). This is a real non-contact regression, not timing or noise. The correction compares the already-stamped **physical boundary ray parameters**, before the object's published-point back-off and world-distance recomputation. The tolerance is now a floating-point rounding budget (64 epsilon times the input coordinate/parameter scale), independent of the contact-search/launch epsilon. Objects lacking physical boundary metadata cannot certify a tie and are left unchanged. The named regression includes both translations; the final gate results below supersede this in-progress probe.

The distance check also covers grazing arrivals (direction `(1,-1e-4,0)`) at both translations. A fixed parameter tolerance would amplify plane-coordinate rounding into a false non-tie. The final comparison projects the physical-parameter difference onto **both true geometric normals** and compares it with the coordinate rounding budget; the neighbourhood radius only limits candidate search. Eight exact/separated, normal/grazing cases pass. Supported Object/CSG casts publish the physical boundary data; unknown casts without that data or true geometric side information are conservatively left unchanged.

An unclamped VCM diffusion pad repeat (n=8) landed just outside the strict band: ratio 0.988079, ratio SD 0.0111238; difference -0.000107649 versus three-SE band 0.000107316. This row now uses 16 salts; the three-SE criterion and samples per render are unchanged. All other rows still use eight salts. No favourable salt was selected and no tolerance floor was added.

### Full historical build (not the source-controlled adapter)

A separate owned worktree at `8df8b6dff` built the whole native library and the transplanted named harness (`make -C build/make/rise -j8 build-test/SSSContactContinuationTest`, rc=0, no warnings). All supplied pad/floor and room rows are red; rc=1. Each row has eight salts; this independent build confirms the provenance.

| Fixture | Ratio | Ratio SD | Contact mean / SD | Reference mean / SD |
|---|---:|---:|---|---|
| RW pad/floor PT | 0.958786 | 0.00720547 | 0.0123932 / 0.0000998628 | 0.0129259 / 0.0000954571 |
| RW pad/floor BDPT | 1.079180 | 0.00781818 | 0.0139436 / 0.0000901354 | 0.0129206 / 0.0000693327 |
| RW pad/floor VCM | 1.056440 | 0.00814597 | 0.0136661 / 0.0000679001 | 0.0129360 / 0.0000839521 |
| Diffusion pad/floor PT | 1.320070 | 0.01245850 | 0.0117417 / 0.000154562 | 0.00889473 / 0.0000866653 |
| Diffusion pad/floor BDPT | 1.505240 | 0.01763420 | 0.0134887 / 0.000168734 | 0.00896115 / 0.000114497 |
| Diffusion pad/floor VCM | 1.490350 | 0.03094010 | 0.0131797 / 0.000279968 | 0.00884338 / 0.000143620 |
| Outside-lit room BDPT | 1.022500 | 0.00115732 | 0.363972 / 0.000814690 | 0.355963 / 0.000810088 |
| Outside-lit room VCM | 1.026830 | 0.00566455 | 0.367026 / 0.001484980 | 0.357437 / 0.001309360 |
| Outside-light on/off BDPT | 1.020780 | 0.00293509 | 0.363945 / 0.000885272 | 0.356535 / 0.000379805 |
| Outside-light on/off VCM, fixed radius | 1.021470 | 0.00425852 | 0.365090 / 0.001825850 | 0.357418 / 0.000920142 |

### Final conditioned tie build

All 63 render rows pass (rc=0). All rows use eight salts except unclamped diffusion VCM, which uses sixteen; the three-SE bands for the original review fixtures are unchanged.

| Fixture | Ratio | Ratio SD | n |
|---|---:|---:|---:|
| review4 original RW pad-floor PT | 1.00337 | 0.0043399 | 8 |
| review4 original RW pad-floor BDPT | 0.998323 | 0.00562435 | 8 |
| review4 original RW pad-floor VCM | 0.997423 | 0.0165707 | 8 |
| review4 original diffusion pad-floor PT | 0.998682 | 0.00420029 | 8 |
| review4 original diffusion pad-floor BDPT | 0.998541 | 0.00616015 | 8 |
| review4 original diffusion pad-floor VCM | 1.00422 | 0.0137119 | 16 |
| review4 original outside-lit room BDPT | 0.999718 | 0.00142778 | 8 |
| review4 original outside-lit room VCM | 1.00071 | 0.00313378 | 8 |
| review4 original outside-light occlusion BDPT | 0.999338 | 0.00236556 | 8 |
| review4 original outside-light occlusion VCM | 1.00104 | 0.00345777 | 8 |

The final immutable native executable matches all 32 separated-scene raw RGBA hashes against original master (one worker, four salts across eight fixtures). The prior in-progress n=8 table above is retained as history, not substituted for this final build. The ownership test passes all 512 rays per helper/manager/exit-metadata check and all eight normal/grazing, coincident/separated, translated distance cases. Focused ASan/UBSan instruments the two changed production translation units plus the named test and exits 0.

## Required gates on the final production code

Every named target was built with `make -C build/make/rise -j8 build-test/<Name>`; every build returned 0 with no compiler warnings. The final contact executable was rebuilt after test-warning/format fixes; its native ownership check still returns 0. Only named tests were built, never the full suite.

| Gate | Result |
|---|---|
| SSSContactContinuationTest | rc=0; 63 render rows, 512 rays per ownership/metadata check, eight distance cases |
| SSSExteriorIndexInvarianceTest | rc=0; 518 passed, 0 failed |
| BDPTStrategyBalanceTest | rc=0; 370 passed, 0 failed |
| VCMStrategyBalanceTest | rc=0; 220 passed, 0 failed |
| SSSRadianceScalingTest | rc=0; 576256 guards passed, 0 failed (complete-event convention, not exact conservation) |
| SourceHygieneTest | rc=0; 172 passed, 0 failed |
| No-contact image identity | 32/32 exact raw RGBA hash matches, one worker |
| Focused ASan/UBSan | rc=0; changed production translation units and named test instrumented |

Render gates ran sequentially. No timing/speedup claim is made. No shipped scene, frozen feature, DL-557 transport, or BDPTVertex contact field was changed.
