# RISE correctness debt ledger

One verified list of the correctness debts genuinely still open in RISE,
produced by re-checking every heading in `CLOTH_FABRIC_DESIGN.md` §15,
`WETNESS_COAT_DESIGN.md` §12, `GEOMETRY_SHADING_SIGNALS_DESIGN.md` §14,
`SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md` §10,
`CROSS_OBJECT_PROXIMITY_DESIGN.md` §10, `RENDERING_INTEGRATORS.md` §7 (debts
27-31), `REFRACTIVE_RADIANCE_SCALING.md` §10, and `IMPROVEMENTS.md` against
the code and tests in this tree. Original sweep: debt-sweep worktree, HEAD
`14bc2cb6`, dated 2026-09-12. **Gap-closure pass** (historical sweep): same
worktree, starting HEAD `c287f5fb`, also dated 2026-09-12 — an independent
verifier found coverage gaps in the first pass (missing items across all
five design-doc ledgers, stale line-number citations, and one row,
DL-14, that was wrong on the facts) and that pass closed them. **Final
completion pass** (historical sweep): same worktree, starting HEAD
`56992663`, also dated 2026-09-12 — a second independent verifier found
the tree otherwise accurate and two remaining gaps: one IMPROVEMENTS.md
heading (the GGX low-F0 grazing gain) not yet carried into the table as
DL-37, and six WETNESS_COAT_DESIGN.md §12 items marked resolved in their
source doc that had not yet been independently re-verified against code
and tests the way `## Already resolved` requires (now DL-R18..DL-R23, one
of which — DL-R20 — turned out to be resolved-in-code but only indirectly
tested, noted as such rather than silently closed); see `## Counts` for
the delta. **Re-sweep** (this pass): debt-resweep worktree, HEAD
`876c9a26`, dated 2026-09-12 — run after a separate cleanup session closed
16 rows (DL-01/02/03/04/34/36/37/48/50/51/54/55/56/57/58/59) and added
DL-38..DL-62 without a further independent check. This pass re-verified
all 47 rows left OPEN against `876c9a26`'s actual source, cross-checked
against the cleanup session's touched-file list, and found the ledger's
OPEN/CLOSED partition already correct (0 rows reclassified); one stale
citation on DL-19 was corrected in place (see `## Counts`). Every verdict
below is cited to a file:symbol, test, or commit — no verdict is taken on
the source document's word alone for anything marked OPEN, and every item
enumerated in any of the seven source ledgers now appears in exactly one
section below (OPEN, Already resolved, Doc-rot, or Not-a-debt).

Sorted by class (physics-bias/energy-loss, precision, API/bridge gap,
coverage/test gap, perf, doc-rot), S before M before L within a class (the
`perf` class was added this pass — see DL-33). Struck headings in the
source ledgers point back to the row here (or vice versa) that closed them.

## Table

| id | source doc:item | one-line claim | verdict | evidence | size | class | visibility |
|----|------------------|-----------------|---------|----------|------|-------|------------|
| ~~DL-01~~ | ~~RENDERING_INTEGRATORS.md debt 31 item 3 / REFRACTIVE_RADIANCE_SCALING.md §10.3~~ | ~~TranslucentSPF RGB/NM exit-weight divergence~~ | CLOSED 2026-09-12 | `1239edf2`: `TranslucentSpectralParityTest: 676 checks, 0 failures` (red: 174 failures); primary-layer tau paid once at entry, Beer-only exit/backscatter parent in both pipes. See [DL-01 closure](DL01_TRANSLUCENT_EXIT_WEIGHT.md). | S | physics-bias | user-visible |
| ~~DL-02~~ | ~~RENDERING_INTEGRATORS.md debt 31 item 4 / REFRACTIVE_RADIANCE_SCALING.md §10.3~~ | ~~TranslucentSPF exit-density support and spectral shape mismatch~~ | CLOSED 2026-09-12 | `a041e51d`: `TranslucentSpectralParityTest: 1918 checks, 0 failures` (red: 324 failures). RGB/NM diffuse exits now share positive-shading-hemisphere cosine sampling/evaluation. Full mixture/reverse density remains DL-41. See [DL-02 closure](DL02_TRANSLUCENT_EXIT_DENSITY.md). | S | physics-bias | user-visible |
| ~~DL-36~~ | ~~SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10~~ | ~~Bounded same-luminary neighbour read lacked a regression~~ | CLOSED 2026-09-12 (consistency pin) | `ac9891f3`: SignalEmitterRecordTest --louvres-only reports `Passed: 16  Failed: 0` against the unchanged library. A real two-blade luminary proves accepted upper-blade proximity=0.5 versus sampled lower-blade proximity=0, with sample geometry preserved. Bounded approximation retained as permitted by recipe. See [DL-36 closure](DL36_EMITTER_NEIGHBOUR_PIN.md). | S | physics-bias | user-visible (bounded approximation retained) |
| ~~DL-48~~ | ~~DL04_SSS_RADIANCE_DECISION.md: Sw normalization~~ | ~~Wrong SSS cosine-hemisphere normalization~~ Shared Schlick normalization now integrates to one | CLOSED 2026-09-12 | `12a7ef3e`: BSSRDFNormalizationTest, `All DL-48 normalization tests passed`; unfixed constant fails 37 checks; the textured-IOR follow-up fails 8 before correction. Actual helper/adapters and RGB/NM sampled ratios cover all five sites. See [closure](DL48_SSS_NORMALIZATION.md). | S | physics-bias | user-visible (diffusion/random-walk subsurface transport) |
| ~~DL-50~~ | ~~DL04_SSS_RADIANCE_DECISION.md: spectral survival~~ | ~~RandomWalkSSS NM exit pays free-flight survival transmittance twice~~ | CLOSED 2026-09-12 | `29ce61c7`: RandomWalkSurvivalTest, `All DL-50 survival tests passed`; unfixed regression fails 13 weight checks with activity/finite/RGB/angular controls passing. Conditional survival, unconditional Beer attenuation, prior collision/reflection, and the NM fallback proposal are covered. See [closure](DL50_RANDOM_WALK_SURVIVAL.md). | S | physics-bias | user-visible (spectral random-walk SSS) |
| ~~DL-51~~ | ~~DL04_SSS_RADIANCE_DECISION.md: dormant exit fallback~~ | ~~Non-absorbing SubSurfaceScatteringSPF inside exit reads the current interior IOR as its destination before popping~~ | CLOSED 2026-09-12 | `34434610`: SubSurfaceExitIORTest, `Checks: 301  Failures: 0` (unfixed: 36 failures). RGB/NM exit optics and transmission now share a popped stack snapshot; reflection retains the input state. Actual absorbing diffusion/random-walk material controls pass. See [closure](DL51_SUBSURFACE_EXIT_IOR.md). | S | physics-bias | latent (standalone non-absorbing SPF; shipped absorption preserved) |
| ~~DL-54~~ | ~~DL48_SSS_NORMALIZATION.md: geometric projection density~~ | ~~BSSRDF disk-to-surface density uses the modified shading normal instead of the geometric normal~~ | CLOSED 2026-09-12 | `ab85092d`: BSSRDFProjectionNormalTest, `All DL-54 projection-normal tests passed` (unfixed: 960 PDF/weight failures). RGB and NM each retain 48/128 matching active sphere hits, with axis activity 33/10/5. All three Jacobians now use entryGeomNormal; the shading frame remains angular. See [closure](DL54_BSSRDF_PROJECTION_NORMAL.md). | S | physics-bias | user-visible (normal-mapped diffusion SSS) |
| ~~DL-55~~ | ~~DL50_RANDOM_WALK_SURVIVAL.md: fallback proposal density~~ | ~~RandomWalkSSS tiny-channel fallback weights do not use the actual collision/RGB survival proposal density~~ | CLOSED 2026-09-12 | `cba4e88c`: RandomWalkFallbackProposalTest, `All DL-55 fallback proposal tests passed` (unfixed: 17 weight failures). RGB effective-rate mixtures and NM physical/proposal collision ratios agree with independent oracles; Beer quadrature has 2948/12288 active exits. Exact NM fallback threshold is covered. See [closure](DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md). | S | physics-bias | user-visible (RGB zero-channel extinction); latent (tiny NM collision coefficients) |
| ~~DL-56~~ | ~~DL51_SUBSURFACE_EXIT_IOR.md: grazing Fresnel residual~~ | ~~Shared dielectric Fresnel helper forces reflection at valid equal-index grazing interfaces~~ | CLOSED 2026-09-12 | `df7e3dad`: DielectricGrazingFresnelTest, `Checks: 338  Failures: 0` (unfixed: `Checks: 128  Failures: 20`). Scaled s/p amplitude ratios preserve matched-index zero reflectance without changing Snell, stack, or eta conventions. See [closure](DL56_GRAZING_FRESNEL.md). | S | physics-bias | user-visible (grazing dielectric and standalone SSS exits) |
| ~~DL-57~~ | ~~DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md: collision density cutoff~~ | ~~RGB random-walk collision guard rejects small positive dimensional densities with non-negligible normalized weights~~ | **CLOSED 2026-09-12** | `c187f2cc`: `RandomWalkDensityCutoffTest`, fixed output `All DL-57 density cutoff tests passed` (red: one missing-large-RGB-exit activity failure). Real closed-sphere RGB scale pair reports spatial weight `1` in every channel and full weight `1.0171875000000001` for the large case; scaled case spatial `1.0000000000000002`, full `1.0171875000000004`; NM spatial `1`, full `1.0171875000000001`. See [DL57 closure](DL57_RANDOM_WALK_DENSITY_CUTOFF.md). | S | physics-bias | latent (tiny extinction coefficients / scene scale) |
| ~~DL-58~~ | ~~DL56_GRAZING_FRESNEL.md: remaining classification work~~ | ~~Snell and cosine-only Fresnel helpers misclassify matched-index extreme-grazing transmission as TIR~~ | CLOSED 2026-09-12 | `1b1909c0`, narrowed fibre compatibility repair `ed8d9c94`: GrazingSnellFresnelTest `Checks: 38  Failures: 0`; MatchedIndexGrazingConsumerTest `Checks: 60  Failures: 0`; GrazingFresnelThroughputTest `Checks: 44  Failures: 0`. Direct red 9 failures, SSS red 10; original throughput red contains 8 valid SMS failures and 3 invalid weave expectations, corrected after its IOR clamp was identified. Supplemental incoming derivative red is 1 valid failure. See [closure](DL58_MATCHED_INDEX_GRAZING.md). | S | physics-bias | latent (extreme-grazing matched-index paths) |
| ~~DL-59~~ | ~~DL58_MATCHED_INDEX_GRAZING.md: remaining unequal-index derivative convention~~ | ~~SMS normal derivative uses the wrong unequal-index ratio/sign convention~~ | CLOSED 2026-09-12 | `a0c4a808`: ManifoldNormalDerivativeTest, `Checks: 141 Failures: 0` (unfixed: `Checks: 141 Failures: 12`). Entering/exiting tangent finite differences and curved analytical/numerical angle-difference Jacobians agree with the unchanged regression. Matched/reflection/TIR controls pass. See [closure](DL59_SMS_NORMAL_DERIVATIVE.md). | S | physics-bias | latent (test-only analytical Jacobian) |
| ~~DL-62~~ | ~~DL37_GGX_DIFFUSE_TRANSMISSION.md: independent glossy-filter audit~~ | ~~GGX sampling and density use increased roughness under glossy filtering while BRDF evaluation uses the unfiltered roughness~~ | CLOSED 2026-09-13 | `dfdd5ee1`: `GGXSampleEvaluationConsistencyTest: 29 checks, 0 failures` (red on `a1db468d`: 23 failures, e.g. "asymmetric saturation W=0.7 maxRelErr=1.910e+01"). `GGXBRDF::value`/`valueNM` now widen alphaX/alphaY by `ri.glossyFilterWidth` identically to `GGXSPF`'s four sample/density paths. Zero-filter control and a same-mechanism `GGXSPF::Pdf` control both matched their reference exactly before and after (relErr=0), isolating the bug to `GGXBRDF` specifically. See [DL-62/DL-64 closure](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md). | S | physics-bias | user-visible (filtered GGX continuation versus direct evaluation) |
| ~~DL-64~~ | ~~DL37_GGX_DIFFUSE_TRANSMISSION.md: zero-F0 support audit~~ | ~~GGX Schlick sampling assigns zero specular and multiscatter probability at F0=0 although both evaluated lobes can be nonzero~~ | CLOSED 2026-09-13 | `dfdd5ee1`: same `GGXSampleEvaluationConsistencyTest` run. `GGXSPF::Scatter`/`ScatterNM`/`Pdf`/`PdfNM` now derive the specular/MS selection weight, in `eFresnelSchlickF0` mode only, from `GGXInterfaceFresnel::Mean()`/`MeanNM()` (conductor/thin-film keep the tint-based weight; `a495a357`) (nonzero at F0=0: `SchlickFresnelAvg(0)=1/21`) instead of raw F0; a deterministic Pdf-at-peak reference (immune to the separate ~2.5e-5 JH-black-uplift epsilon a naive ">cosine" threshold would have been fooled by) went from relErr 0.87-0.99 to exactly 0. Per-direction Fresnel evaluation (the actual sampled throughput) is untouched. See [DL-62/DL-64 closure](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md). | S | physics-bias | user-visible (zero-F0 GGX continuation loses grazing reflection) |
| DL-67 | Found during DL-42/DL-43 (debt-guiding slice, 2026-09-13; filed in this slice's own ledger as DL-65, RENUMBERED DL-67 at merge time -- see `## Counts` -- to avoid colliding with rows concurrently filed by sibling debt-cleanup slices: DL-65 is the ggx slice's row, DL-66 the sssenv slice's, DL-68 the translucent slice's) | PT's and BDPT's guided one-sample and RIS proposal/throughput densities are inconsistent across lobe-selection branches at a multi-lobe SPF: neither RIS candidate 0 nor the one-sample/RIS "aggregate BSDF" branches use the TRUE generating density of the direction they price | OPEN-confirmed (static + closed-form counterexample; rendered red-proof pending) | Root cause: at a multi-lobe SPF, candidate 0's (and any kray-based branch's) TRUE generating density for a direction is the mixture `sum_I q_I * p_I` over every lobe `Scatter()` could have emitted, where `q_I = PTScatterSelectWeight(lobe_I) / sum_j PTScatterSelectWeight(lobe_j)` (`PathTracingIntegrator.cpp:1337`, `MaxValue(kray)`-weighted) -- NOT the selected lobe's own `pS->pdf`, and NOT the material's aggregate `ISPF::Pdf()` either, because `ISPF::Pdf()` internally re-weights lobes by a DIFFERENT ratio than `PTRandomlySelect` used to pick one (`SchlickSPF::Pdf`, `SchlickSPF.cpp:437-447`, weights diffuse/specular by `MaxValue(rd)` vs `MaxValue(rs)` -- the raw painter albedos -- while `PTScatterSelectWeight`/`PTRandomlySelect` weight the SAME two lobes by `MaxValue(diffuse kray)` vs `MaxValue(rho+(1-rho)*fresnel)`, a Fresnel-weighted quantity that differs from `rs` at every non-normal incidence). So: (1) candidate 0 (`c.bsdfPdf = pS->pdf`, `PathTracingIntegrator.cpp:3137`; `BDPTIntegrator.cpp:2537` eye / `:6276` light) uses the wrong (single-lobe) density, and candidate 1's aggregate `PTEvalPdfAtSurface`/`PathValueOps::EvalPdfAtVertex` uses a DIFFERENT wrong (aggregate-but-mismatched-weights) density -- both miss the true `sum_I q_I p_I`, so RIS's `sum_i P_i/D_i = M` identity is violated: a closed-form 2-candidate example with `p1=10, p2=0.1` (per-lobe pdfs), guide pdf `p_g=1`, true aggregate `p_agg=5.05` gives per-branch coefficients 1.074 (candidate-0-style) vs 0.256 (aggregate-style) against the correct value, i.e. roughly a 33% low estimate using the true generating density as ground truth. (2) The same inconsistency exists between PT's THREE one-sample/RIS overwrite branches in `PathTracingIntegrator.cpp`'s trained-guiding block: branch (a), this slice's DL-42 fix (`PTScatterKray(*pS) * pS->pdf/(selectProb*combinedPdf)`, per-lobe kray/pdf); branch (c), the one-sample guided-direction-accepted branch (`PTMulDiv(fGuided,cosTheta,combinedPdf)`, aggregate f over aggregate-but-mismatched pdf); and the RIS-accepted branch (b). (a) and (c) do not partition to a consistent total unless every lobe shares one pdf (single-lobe SPF) -- adding `/q_I` to (c) does not fix this; the correct construction uses one `f` and one denominator (the true mixture density `sum_I q_I p_I` over the lobes `Scatter()` actually accepted) in ALL THREE branches, in both PT and BDPT eye/light. Scope: any multi-emit SPF (SchlickSPF, PolishedSPF/PolishedBRDF, TranslucentSPF, CompositeSPF, BioSpecSkinSPF, GenericHumanTissueSPF); `GGXSPF`/`CoatedSPF` are immune (internal single-lobe selection: exactly one `ScatteredRay` per `Scatter()` call, `pdf==mixPdf`, outer `selectProb` trivially 1). A separate, additive asymmetry in the same candidate machinery: candidate 1 is discarded (`c.valid=false`, `risWeight=0`) whenever the material's aggregate pdf is 0 at the guided direction (`PathTracingIntegrator.cpp:3148` candidate 0 / `:3177` candidate 1 / `:3191` fallback; `BDPTIntegrator.cpp:2547`/`:2584`/`:2598` eye, `:6286`/`:6314`/`:6328` light), silently falling back to candidate 0 alone and losing whatever variance-reduction the guide would have offered there -- energy-losing in the sense of discarding a legitimate RIS candidate, tracked as part of the same rework rather than a fourth bug. Recipe: red-prove on `schlick_material` (diffuse+specular, both eligible, overlapping support) with guiding forced to substitute (`guidingAlpha` near 1, a real trained field so `guidedSubstituted>0`, unlike this slice's DL-42 fixture which forced `guidedSubstituted=0`), comparing rendered radiance against unguided PT reference; fix = the one-f/one-denominator construction above in PT (all three branches) and BDPT eye/light RIS. | M-L | physics-bias | user-visible (guided renders of any multi-lobe, multi-emit material) |
| DL-69 | Found during DL-42/DL-67 derivation (debt-guiding slice, 2026-09-13) | BDPT's ORDINARY (non-guiding, unconditional) eye/light non-delta throughput pairs the material's AGGREGATE (all-lobes-summed) BSDF value with a PER-LOBE selection pdf, over-counting by up to Nx at a multi-lobe SPF whose lobes have overlapping support | OPEN-confirmed (static evidence + closed-form BRDF check; rendered red-proof pending) | `BDPTIntegrator.cpp` eye subpath (`GenerateEyeSubpathImpl`, ~2703-2777) computes `scatterPdf = selectProb * effectivePdf` (`effectivePdf` = the ONE stochastically-selected lobe's own `pScat->pdf`, or its guided/RIS-blended replacement -- still a per-lobe/single-candidate density, ~2703), then for non-delta lobes `f = PathValueOps::EvalBSDFAtVertex(...)` -- the material's AGGREGATE `IBSDF::value()`, summed over every lobe (~2763-2766) -- and sets `localScatteringWeight = f * cos / scatterPdf` (~2774-2775). The light subpath (`GenerateLightSubpathImpl`, ~6459-6476) is byte-for-byte the same pattern (`scatterPdf = selectProb*effectivePdf` ~6460, aggregate `f` ~6449-6453, `f*cos/scatterPdf` ~6466-6472). For N accepted non-delta lobes with overlapping support this is `E[throughput] = N * integral(f_agg * cos)` -- an N-times over-count, not the correct per-lobe `kray_I/selectProb` estimator used elsewhere in the same codebase (e.g. this file's delta branch two lines above each site, `KrayValue<Tag>(*pScat) * (.../selectProb)`, and PT's own ordinary initialization, `PTScatterKray<Tag>(*pS) * (1/selectProb)`). Confirmed on `SchlickBRDF::value()` (`SchlickBRDF.cpp:126`: `return diffuse*INV_PI + (rho+(1-rho)*fresnel)*factor;` -- diffuse and specular summed over the SAME upper hemisphere) paired with `SchlickSPF::Scatter` emitting both a diffuse ray (`SchlickSPF.cpp:288`) and a specular ray (`:313`) as separate non-exclusive `ScatteredRay`s into the same container -- or, on the anisotropic/per-channel path, one diffuse plus THREE per-channel specular rays (`:321-331`), i.e. up to a 4x over-count from that one vertex alone. Invisible on disjoint-support SPFs (TranslucentSPF's hemisphere-piecewise lobes; PolishedSPF's delta coat + non-delta substrate, where only one of the pair is ever non-delta at a time) -- and invisible on `lambertian_material` (only one lobe, so N=1 trivially) or `ggx_material` (immune for a DIFFERENT, confirmed reason than "single-lobe selection": `GGXSPF::Scatter`/`ScatterNM` set EVERY emitted lobe's `.pdf` field to the SAME `mixPdf` -- the 3-lobe aggregate mixture density, `GGXSPF.cpp:221/320/410` etc. -- not a per-lobe conditional pdf, so `effectivePdf` at this BDPT site already equals the aggregate density `EvalBSDFAtVertex`'s `value()` integrates against, and `f_agg/effectivePdf` is the correct aggregate/aggregate pairing by construction; `SchlickSPF`'s per-lobe `.pdf` fields are each lobe's OWN conditional density (cosine/pi for diffuse, the half-vector pdf for specular), which is what creates the mismatch). `coated_material`'s internal single-lobe selection (exactly one `ScatteredRay` per `Scatter()` call) is a separate, also-immunizing reason but not the GGX one. `BDPTStrategyBalanceTest`/`VCMStrategyBalanceTest` use only `lambertian_material` scenes, so this pattern has never been exercised by the existing balance gates. Recipe: add a `schlick_material` (diffuse+specular, both non-delta, overlapping support) topology to `BDPTStrategyBalanceTest` (and/or a standalone red-proof harness) vs a `pathtracing_pel_rasterizer` PT reference on the same scene; red-prove the over-count, then fix by making the non-delta throughput construction consistent -- either per-lobe `f_I` paired with the per-lobe pdf (matching PT's `kray` convention), or the aggregate `f_agg` paired with the TRUE aggregate mixture pdf (see DL-67's `sum_I q_I p_I`) -- applied identically at both eye and light generators. Also corrects the DL-42 ledger row's earlier citation of this exact BDPT code ("does not share this pattern... always uses the aggregate `PathValueOps::EvalBSDFAtVertex`... for its non-delta throughput") as evidence BDPT is unaffected by anything in this area -- that statement is accurate about DL-42's specific pattern (a dropped `selectProb` division under guiding) but was read too broadly; it is not evidence against DL-69, which is this file's ordinary (non-guiding) throughput construction. | M | physics-bias | user-visible (BDPT/VCM/MLT renders of any multi-lobe material with overlapping-hemisphere lobes, e.g. `schlick_material`) |
| ~~DL-03~~ | ~~RENDERING_INTEGRATORS.md debt 31 item 1 / REFRACTIVE_RADIANCE_SCALING.md §10.3~~ CLOSED 2026-09-12 — `8a9bdb18`, `TranslucentIORStackTest: ALL TESTS PASSED` | ~~Guided translucent exits lose their popped IOR stack~~ An available selected exit transition is preserved or rejected according to the accepted direction and shared by training/eta consumers; missing entry-state generation remains DL-47 | CLOSED-by-test | Red on unfixed `00bdcef5`: four failed assertions (`d3a5e732`); real trained PT RGB/NM outward substitutions, inward controls and later same-object classification. Additional BDPT eye/light RGB/NM coverage; eye RIS actual-guide limitation remains DL-43. See `DL03_GUIDED_IOR_CONTINUATION.md`. | M | physics-bias | user-visible (eligible guided translucent continuations) |
| ~~DL-04~~ | ~~REFRACTIVE_RADIANCE_SCALING.md §10.1~~ | ~~Unsettled extra eta-square factor for complete SSS events~~ No unmatched factor belongs on the exterior-to-same-exterior event | CLOSED 2026-09-12 (consistency pin) | `1b705ce1`: SSSRadianceScalingTest unchanged-library baseline 572093 checks, 0 failures (both deliberate eta directions fail all six SSS air-channel checks); the baseline moved to **574017** checks, 0 failures after DL-52's probe fix started reaching additional entry points (`00cd6723`, 2026-09-13) and is unchanged by DL-71/DL-72. Independent helper plus matched explicit-volume/diffusion/RW camera matrix; of the normalization/non-air support/MIS defects originally tracked as DL-48 through DL-53, only **DL-49** (non-air relative-index) remains open. See [decision](DL04_SSS_RADIANCE_DECISION.md). | M | physics-bias | convention pinned; separate SSS defects remain open |
| ~~DL-34~~ | ~~CROSS_OBJECT_PROXIMITY_DESIGN.md §10~~ | ~~Published two-sphere union overlap exports only the deeper operand depth~~ | CLOSED 2026-09-12 (recorded regression; conservative contract retained) | `cffa254f`: unchanged ProximitySignalTest red `Passed: 465   Failed: 6`, fixed `Passed: 471   Failed: 0`; supplemental final `Passed: 491   Failed: 0`. Certified inscribed-ball union recovers `sqrt(3.56)` through signed field, manager and interior signal. The original proposed max-depth edit was already the existing signed-min magnitude; arbitrary unions remain non-exact. See [closure and limits](DL34_UNION_INTERIOR_DEPTH.md). | M | physics-bias | user-visible (published union seam regression) |
| ~~DL-37~~ | ~~IMPROVEMENTS.md: GGX low-F0 grazing gain~~ | ~~Angle-flat diffuse split creates grazing gain~~ | CLOSED 2026-09-12 | `000df0b4`: LayeredWhiteFurnaceTest reports `0 of 57 configurations failed` (red: four failed configs). Reciprocal entry/exit transmission covers RGB/NM Schlick, conductor and film. Independent sweep: 150/46 before, 150/3 after; retained specular-only failures are DL-63. See [scope](DL37_GGX_DIFFUSE_TRANSMISSION.md). | M | physics-bias | user-visible |
| ~~DL-42~~ | ~~DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals~~ CLOSED 2026-09-13 — `PathTracingIntegrator.cpp` fix, `PTGuidedSelectProbTest: ALL TESTS PASSED` | ~~PT's BSDF-surviving one-sample guiding branch drops selected-lobe probability compensation~~ The kept-BSDF-direction one-sample branch now divides by `selectProb` too: `scatterThroughput = kray * pS->pdf / (selectProb * combinedPdf)` | CLOSED-by-test | Red-proved with a real multi-lobe `TranslucentMaterial` exit (diffuse-exit + translucent-backscatter lobes, unequal weights via `scattering`), a real trained `PathGuidingField` (`guidingAlpha=1e-6`, engaging the trained-guiding code path while making the guided-direction coin flip land on "keep BSDF direction" on ~4000/4000 trials), and a constant-radiance "sky" continuation with no LightManager (isolating the measured `IntegrateFromHit`/`IntegrateFromHitNM` return to exactly `scatterThroughput * skyRadiance`, no NEE, no area-light MIS reweight). Pre-fix: every diffuse-exit-selected trial (2586/2586 and 3588/3588 across two scattering splits, RGB and NM) matched the buggy `kray*pdf/combinedPdf` closed form and NONE matched the correct one. Post-fix: the reverse, exactly. `GGXBRDF::value()`/`GGXSPF::Pdf()` (`return diffuse+specular;` / "3-lobe mixture PDF") confirmed the OTHER two `PTMulDiv` overwrite sites in the same block (RIS-accepted candidate, one-sample guided-direction-accepted) re-evaluate the material's AGGREGATE BSDF/PDF fresh at the priced direction. **Correction (2026-09-13, same slice, before merge): this row originally continued "...and are therefore already complete, self-contained estimators that must NOT also divide by selectProb -- applying the fix there would double-count." That claim is FALSE and is withdrawn.** `GuidingSupportsSurfaceSampling` admits only non-delta eRayDiffuse/eRayReflection lobes, so at a multi-lobe surface with an ineligible lobe (e.g. this row's own `TranslucentMaterial` EXIT fixture: diffuse-exit eligible, translucent-backscatter not) the aggregate-BSDF branches price ALL lobes while guiding only ever proposed/weighted the eligible subset, and the ineligible lobe is priced AGAIN through the ordinary kray/selectProb path -- and even restricted to an all-eligible material, per-lobe kray/pdf (this fix) and aggregate BSDF/pdf (the other two branches) are two different, mutually-inconsistent RIS/one-sample proposal measures unless the SPF has only one lobe. This is a real, still-open, DIFFERENT bug from the one this row fixes; it is filed as DL-67 (renumbered from this slice's original DL-65 at merge time to avoid colliding with concurrently-filed rows from sibling debt-cleanup slices), not fixed here. Confirmed `ggx_material`/`coated_material` cannot exercise EITHER bug (this row's or DL-67's) at all (`GGXSPF`/`CoatedSPF` each do internal single-lobe selection, emitting exactly one `ScatteredRay` per call, so the outer `selectProb` is trivially 1 for either). `BDPTIntegrator.cpp`'s analogous "kept BSDF direction" one-sample fallback (eye ~2662, light ~6363-ish) does not share DL-42's pattern (a dropped `selectProb` division): it unconditionally folds `selectProb` into `scatterPdf = selectProb*effectivePdf`. It DOES, however, always use the aggregate `PathValueOps::EvalBSDFAtVertex` paired with a PER-LOBE `effectivePdf`, never `pScat->kray` directly, for its non-delta throughput -- a THIRD, independent measure-mismatch pattern (aggregate f over per-lobe pdf, an over-count on overlapping-support multi-lobe materials rather than an under-count), filed as DL-69, not fixed here. **HWSS correction: PT's HWSS body (`IntegrateFromHitHWSS`) contains no guiding block at all (verified: zero occurrences of `pGuidingField`/`GuidingCombinedPdf`/`GuidingRIS` in that function) -- it is unaffected by this fix, not "covered via the shared RGB/NM template" as an earlier version of this row's detailed section claimed; HWSS reaches the fixed line only indirectly, via its per-wavelength fallback calls into `IntegrateFromHitNM` for BSDF materials.** | M | physics-bias | user-visible (path guiding and mixed-lobe materials) |
| ~~DL-43~~ | ~~DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals~~ CLOSED 2026-09-13 — `a69c9ce6`, `TranslucentIORStackTest: ALL TESTS PASSED` | ~~BDPT eye guiding swaps incoming/outgoing directions when evaluating forward candidate PDFs~~ Eye-subpath RIS candidate-1 and one-sample guiding now pass `(-currentRay.Dir(), candidateDirection)` to `PathValueOps::EvalPdfAtVertex`, matching the light-subpath twins and the wrapper's Pdf(outgoing\|incoming) contract | CLOSED-by-test | Red on unfixed HEAD (`42f3dc97`): eye RIS `substituted_out=0` (rejected every guide candidate) and eye one-sample `bad_pdf_value=41` (all 41 outward substitutions evaluated the density of the wrong direction); light-subpath rows already passed. Fixed both call sites (RIS candidate 1, one-sample), each compiled for PelTag/NMTag via the shared `GenerateEyeSubpathImpl` template. Green: eye RIS `substituted_out=13`, all `bad_pdf_value=0`. Sibling audit: the ~14 other `EvalPdfAtVertex` call sites in `BDPTIntegrator.cpp` are Veach MIS reverse-density/connection-strategy conversions (deliberately query the opposite direction) and already use the correct order; `PathTracingIntegrator.cpp` has no `EvalPdfAtVertex` calls (PT's guiding uses the single-argument `PTEvalPdfAtSurface`/`EvalPdfAtSurface` against an already-real `RayIntersectionGeometric`, architecturally immune to this swap). See `PathValueOpsTest.cpp` Test G for the closed-form derivation. | M | physics-bias | user-visible (BDPT eye path guiding) |
| DL-39 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: independent residuals | Dedicated translucent photon deposition counts absorbed power as deposited power | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentPelPhotonTracer::TracePhoton` sums only propagated non-diffuse `kray`, then stores `power*(1-accum_scattered)`; at an inside exit with scattering zero it stores all power regardless of extinction. `TranslucentPelPhotonMap::RadianceEstimate` does not restore the missing Beer attenuation. | M | physics-bias | user-visible (translucent photon maps) |
| DL-44 | DL36_EMITTER_NEIGHBOUR_PIN.md: review residual | Sampled emitter UV is omitted from LightSample and downstream rebuilt emission records | OPEN-confirmed (static evidence; red-proof pending) | `LightSampler.cpp::SampleLight` sets local RGB `rig.ptCoord = coord`, but `LightSample` carries no UV. BDPT NM/HWSS emission rebuilds and LIGHT root, and VCM sampled-emitter evaluation retain default (0,0); `CheckerPainter` consumes ptCoord. MLT shares BDPT generation. | M | physics-bias | user-visible (UV-textured luminaries under bidirectional/spectral transport) |
| DL-45 | DL03_GUIDED_IOR_CONTINUATION.md: tilted-frame residual | TranslucentSPF samples geometrically inward diffuse exits under tilted shading normals and still pops the IOR stack | OPEN-confirmed (observed defect pin; correctness red-proof pending) | `TranslucentSPF.cpp` explicitly exempts exit re-emission from its geometric-horizon gate; RGB/NM exits sample around onb.w and unconditionally pop. DL-03 real-SPF fixture recorded 1021/4096 unchanged inward exits per RGB/NM unguided run at 60-degree shading-normal tilt. Exit Pdf also omits the geometric gate. | M | physics-bias | user-visible (translucent materials with perturbed shading normals) |
| DL-46 | DL03_GUIDED_IOR_CONTINUATION.md: initial-containment residual | Camera/light origins inside closed translucent objects lack initial IOR-stack membership and misclassify their first exit as entry | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentMaterial` inherits invalid/non-refracting default `GetSpecularInfo`; `IORStackSeeding::SeedFromPoint` accepts only valid canRefract materials. Both TranslucentSPF scatter variants classify exclusively by containsCurrent. Shared PT camera and BDPT eye/light seeds skip this stateful non-refracting material. | M | physics-bias | user-visible (origins inside closed translucent objects) |
| ~~DL-52~~ | ~~DL04_SSS_RADIANCE_DECISION.md: planar probe origin~~ | ~~BSSRDF entry probes skip nearby points on a flat surface by advancing from the projection plane before intersecting~~ | CLOSED 2026-09-13 — `00cd6723`, `BSSRDFPlanarProbeReachTest` | Unfixed: normal-axis probe on a real `InfinitePlaneGeometry` found 0/500 entry points (curved-sphere control 372/500). `BSSRDFSampling::SampleEntryPoint` now traces a single chord starting `probeMaxDist` before the projection plane and travelling through it (PBRT `SeparableBSSRDF::Sample_Sp` convention), instead of two half-lines starting ON the plane. Fixed: 500/500 reached, all coplanar with the exit point; control unchanged at 372/500. `BSSRDFProjectionNormalTest`'s own hand-copied probe oracle updated to the same chord shape to stay in lockstep (unchanged results: active=48/128, axes=33/10/5); `BSSRDFNormalizationTest`, `BSSRDFSamplingTest`, `SSSRadianceScalingTest` unaffected. See [DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md](DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md). | M | physics-bias | user-visible (diffusion SSS on planar or nearly planar geometry) |
| ~~DL-53~~ | ~~DL04_SSS_RADIANCE_DECISION.md: recursive environment MIS~~ | ~~Recursive RayCaster misses bypass environment MIS when passed the global map explicitly~~ | CLOSED 2026-09-13 — `b3de184d`, `RayCasterEnvEscapeMISTest` | Unfixed: `RayCaster::CastRay`/`CastRayNM`/`CastRayHWSS`'s explicit-`pRadianceMap` escape branch returned raw radiance regardless of `bsdfPdf` (48/79 checks fail: e.g. RGB `bsdfPdf=0.1` expected 0.474006, got 0.7 — the unweighted raw value). Fixed by a shared `RayCasterEnvEscapeMISWeight` helper applied in that branch too, gated on the resolved map being pointer-identical to `pScene->GetGlobalRadianceMap()` (the MIS PARTNER RULE, mirroring the PT-integrator fix in [PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md)); a genuinely distinct per-object override map keeps full weight, `bsdfPdf==0` is unaffected. Fixed: 79/79 pass at the time of this fix; a later same-day commit (`0eb7a47e`) added partition-of-unity checks, so the CURRENT total is 91/91 (re-verified this pass, P3-2) — quote 91, not 79, when citing this test's count going forward. Affects the SSS/RW-SSS continuation call sites in `PathTracingIntegrator.cpp` and the internal volume phase-scatter continuation in `RayCaster.cpp` itself (no call-site edits needed — the fix is at the shared layer); BDPT's `RecordGuidingTrainingSampleNM` guiding-training helper also passes through the same fixed branch. See [DL53_RAYCASTER_ENV_ESCAPE_MIS.md](DL53_RAYCASTER_ENV_ESCAPE_MIS.md). | M | physics-bias | user-visible (SSS and eligible recursive environment continuations) |
| ~~DL-71~~ | ~~DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md: probe entry-normal orientation~~ | ~~DL-52's single-chord probe reports an inverted entry normal on near-half hits for geometry that orients normals toward the incoming ray~~ | CLOSED 2026-09-13 — `e416d3bd`, `BSSRDFPlanarProbeReachTest`; **round-2 correction same day** (positional gate replaced with an unconditional rule; see evidence) | Round 1: a coplanar double-sided `TriangleMeshGeometry` quad and `ClippedPlaneGeometry` both returned `outward 0/500  neePositive 0/500`; fixed by negating `h.normal`/`h.geomNormal` for near-half hits only (`distFromChordStart < probeMaxDist`). **Round 2 (this pass) found the near-half gate ITSELF wrong**: the flip predicate is a per-hit fact about approach side, independent of chord position, so a far-half hit on flip-oriented geometry can ALSO need correction and round 1 silently skipped it — new fixtures (`MakeDoubleSidedQuadMeshAtZ`, a far-half quad; `MakeDoubleSidedWallMeshAtX` + tangent-axis forcing, exercising the previously-100%-untested tangent/bitangent branch) reproduced `outward 0/500  neePositive 0/500` on round-1 code. Fixed by applying `oriented ? -raw : raw` UNCONDITIONALLY to `h.geomNormal`, gated only on `bGeomNormalOrientedToRay && !bGeomNormalRayDerived` (the latter a new field, DL-75, excluding `HairGeometry`'s fabricated ray-derived orientation from the recovery — hair has no genuine second side to recover). Also fixed in the same pass (P2-A, found reviewing the far-half fixture): the SHADING normal's independent flip predicate can disagree with the geometric normal's at a grazing crossing, so round 1's lockstep negation of both could leave them in opposite hemispheres; now the shading normal is oriented into the corrected geometric normal's hemisphere instead. Fixed: `outward 500/500  neePositive 500/500` on all six fixtures (2 round-1 coplanar + 2 round-2 far-half/tangent + the pre-existing plane/sphere controls). See [DL71_BSSRDF_PROBE_ENTRY_NORMAL.md](DL71_BSSRDF_PROBE_ENTRY_NORMAL.md) "Round-2 correction". | M | physics-bias | user-visible (SSS entry-point NEE/continuation on double-sided planar geometry; round 2 additionally covers concave/multi-surface far-half crossings and the tangent/bitangent probe axes) |
| ~~DL-72~~ | ~~DL53_RAYCASTER_ENV_ESCAPE_MIS.md: optimal-MIS training gap~~ | ~~REOPENED round-2 (P2-B): the round-1 wiring itself has a moment/count pairing defect~~ | CLOSED 2026-09-14 (round 3, debt-guiding2 slice) | Volume sites (`RayCaster.cpp` RGB+NM `CastRay`/`CastRayNM`): `rs2.bsdfTimesCos = RISEPel(phasePdf,phasePdf,phasePdf)` (was `RISEPel(0,0,0)`) -- correct per `IPhaseFunction::Pdf()==Evaluate()` and no separate cosine term for a volume vertex -- paired with a new `AccumulateCount` call. BSSRDF sites (`PathTracingIntegrator.cpp`, both continuations, both tags): new `PTBssrdfSwTimesCos` helper recovers `Sw*cos = (weight/weightSpatial)*cosinePdf` algebraically from the already-computed `bssrdfWeight`/`bssrdfWeightSpatial` split (no BSSRDF-file edits), paired with a new `AccumulateCount` before Russian roulette. Red-proof, corrected 2026-09-14 (review round 2) -- the earlier wording called `OptimalMISAccumulatorTest`'s Test 12 the red-proof, which it is not: Test 12 (`TestDL72PairedTrainingFires`, 34/0 total, was 27/0) drives the accumulator's PUBLIC API by hand and hand-derives `alpha=0.5` unwired -> `alpha=1/3` buggy (unpaired) -> `alpha=4/10.4` fixed (paired), which pins the ACCUMULATOR'S ARITHMETIC but executes no line of `RayCaster.cpp` or `PathTracingIntegrator.cpp` and so is blind to whether either file calls the API at all -- exactly what both DL-72 regressions were. The real red-proof is the new `tests/OptimalMISTrainingSitesTest.cpp`, which runs BOTH production sites (an optically thin global fog under an environment; a real `SubSurfaceScatteringMaterial` on a real sphere `Object` through production `IntegrateFromHit`) and reads `Solve()`'s branch table back: with the two files reverted to `ddf05c6c` (the round-2 "leave the arm UNWIRED" state) the volume site reads exactly clampMin **0.001** and the BSSRDF site exactly clampMax **0.999**; wired, they read **0.857** and **0.929** -- 7/0. P2-5 (review round 2): the volume sites additionally recorded the moment against the RAW `phasePdf` while the direction came from the guided mixture; `bsdfPdf` is now `effectivePdf` (the true density) and the raw phase pdf moved to the new `bsdfMisPdf` field, where DL-73 requires it -- see the DL-74 row. Gate: `RayCasterEnvEscapeMISTest` 91/91, `SSSRadianceScalingTest` 574017/0, `MISWeightsTest` 59/0, `RasterizerDefaultsConsistencyTest` 164/0, all unchanged; `make -C build/make/rise -j8 all` clean. Review round 3 (same slice): the red-proof asserted only `Solve()`'s alpha, which is a RATIO and so is unmoved by a `bsdfTimesCos` scaled by any constant -- precisely this row's failure shape. `OptimalMISAccumulator` gained a const `GetTileTraining()` accessor and `OptimalMISTrainingSitesTest` now asserts the trained QUANTITY: at the volume site every escape contributes exactly `(L_env*phasePdf)^2/phasePdf^2 = L_env^2`, so the accumulated sum must be a whole multiple of `L_env^2` not exceeding the attempt count (measured **484 x L_env^2 over 726 attempts**), and at BOTH sites re-running identical seeds under a 3x brighter uniform environment must give exactly **9x** (measured 9 and 9). Red-proved by mutating both volume sites to `rs2.bsdfTimesCos = RISEPel(1,1,1)`: sum inflates to **76430.2 = 484 x (4*PI)^2**, two assertions fail (17/2), while the alpha assertion stays green. 19/0 after. **Review round 4 (same slice) corrected the trained QUANTITY itself.** `Sw*cos` is the BSSRDF exit's integrand with the AREA-measure `weightSpatial = Rd*Ft(exit)/pdfSurface` DROPPED, and `LightSampler`'s entry-NEE arm dropped the identical factor from the other side of the same pair (the caller applies it after the call returns); the accumulator's moment is of the INTEGRAND (Kondapaneni 2019), so both halves were scaled by an arbitrary, non-O(1) amount and skewed the alpha of every tile an SSS surface touches. Since `weight = weightSpatial*Sw*PI` and `cosinePdf = cos/PI`, the integrand is exactly `weight*cosinePdf` -- a multiply, not a ratio, so `PTBssrdfSwTimesCos` becomes `PTBssrdfTrainedBsdfTimesCos(weight, cosinePdf)`, its Rd==0 guard disappears and the Pel form stays per-channel instead of collapsing to `MaxValue()`. `EvaluateDirectLighting{,NM}` gain a trailing `neeTrainingScale` (default 1) that scales the TRAINING integrand only, never the returned radiance; the two BSSRDF/random-walk entry-NEE call sites pass `PTSurvivalMagnitude(bssrdfWeightSpatial)`. The alternative offered in review -- leaving the site untrained, as DL-84 leaves PT's in-loop volume site -- was rejected because it silences the BSDF half while the NEE half, governed by a per-scene accumulator pointer rather than a per-call flag, goes on training the same mis-scaled moment UNPAIRED. Red-proof (the `L_env^2` law cannot see this -- a radiance scale multiplies right and wrong numerators alike): a decorator scales the diffusion profile's `Rd` by 2 and forwards every sampling method verbatim, the walk gets a large path importance so `RayCaster`'s own `rs.importance < RC_RR_THRESHOLD` test cannot fire for either run, and the accumulator is wired into the `LightSampler` so the NEE half is measured too; identical attempt counts confirm identical paths, and every moment must then scale by exactly 4 (bounded below 4 by the tile's Rd-independent SSS-surface events). On `145c9259` BOTH ratios were EXACTLY **1**; after, **3.99962** (BSDF) and **3.99567** (NEE). `OptimalMISTrainingSitesTest` **23/0** (21/2 before). See [DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md](DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md) "Round 3" and "Round 5". | S | precision | not user-visible (training-input fix only; render-time weight was already `PowerHeuristic`-correct before and after -- see the row's own prior "not applicable" note) |
| ~~DL-74~~ | ~~DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md: surface env-NEE/escape MIS partition violation under guiding~~ | ~~`PathTracingIntegrator.cpp`'s surface scatter continuation stores the COMBINED `effectiveBsdfPdf`... but surface env-NEE weights against the RAW material pdf~~ | CLOSED 2026-09-14 (debt-guiding2 slice, review round 2) for EVERY guiding mode; **DL-83 closes with it** | **The first repair (same slice, earlier commit) did NOT close the partition and its ledger entry overstated it; this row supersedes that text.** Root cause restated: the guided pdf had two jobs and one home. `RAY_STATE::bsdfPdf` was simultaneously the throughput/optimal-MIS-training denominator (which REQUIRES the true sampling density) and the MIS partner against light sampling (which requires a function `LightSampler`'s NEE arms can evaluate for the same direction — impossible, since NEE runs at PART 2 before PART 3 selects a lobe). Fix: split the roles. `bsdfPdf` keeps the true sampling density; a new `RAY_STATE::bsdfMisPdf` (default -1 = "not set, use `bsdfPdf`", read via `MisPartnerPdf()`) carries a lobe-INDEPENDENT NOMINAL density `alpha_nom*guide(w) + (1-alpha_nom)*p_aggregate(w)` evaluated identically by the env-NEE arms, the area-light NEE arms, the escape weight and the emitter-hit weight. `alpha_nom` = base alpha with the learned per-cell scaling and NO per-lobe halving; the cosine product is applied unconditionally; the guiding MODE does not enter the formula, which is why DL-83 closes here. MIS is unbiased for any weights that partition to one (one-line proof in the doc), so a shared nominal density costs nothing in correctness. Also: ONE distribution per shading point (initialised above PART 2 with ONE `sampler.Get1D()`, shared with PART 3 — removes the extra NEE-side draw AND the second stochastic region lookup; nothing is drawn when guiding is off); a direction with NO BSDF-side partner keeps weight 1 on BOTH sides (a delta lobe; a vertex with no guiding at all) -- **but not a direction merely outside the material's aggregate SAMPLING support, which rounds 2-3 wrongly put in that class; see the round-4 amendment at the end of this row**; the volume continuations get the same split (DL-72 P2-5). Red-proof: new `tests/PTGuidingMISPartitionTest.cpp`, two closed-form furnaces on the live Job/RayCaster/LightSampler. Red on `8fcce0bf` (18/4): (b) `guidingLearnedAlpha=true`, the DEFAULT, +63.3%; (c) `eRayReflection` lobe +47.5%; (d) area-light NEE vs the guided emitter hit -42.7%; (e) RIS mode -65.9%, with the guiding-off reference at +0.08% and control row (a) at +0.16%. Green after: **22/0**, every row within 0.55% of its closed form. **Review round 3 (same slice, same day) found that the round-2 split had left two things undone and fixed both; the row stays CLOSED only because the two new red-proof rows are green.** (P1) Splitting one field into two obliges an audit of every CONSUMER of the old one, and `bsdfMisPdf` was consumed in exactly ONE place (`RayCasterEnvEscapeMISWeight`): `PathTracingShaderOp`'s three entry points still forwarded only `rs.bsdfPdf` into `IntegrateFromHit{,NM,HWSS}` (which then re-derived the partner as the same value, under a comment asserting every caller is non-guided -- false for the one producer round 2 had just taught to differ), and `EmissionShaderOp`'s RGB+NM emitter-hit weights read `rs.bsdfPdf` in the partner role. Global medium + trained VOLUME guiding + an area emitter: **+44.1% over** (row (g), guided 0.0111283 vs the unguided expectation 0.0077211) on the round-2 HEAD `2ebcaff9`, **+0.041%** after. (P2) The round-2 residual list dismissed the NEE-vs-escape IOR-stack divergence as "pre-existing and orthogonal"; both adjectives were wrong -- the escape side never evaluated the aggregate pdf at all before this row, and an albedo-1 furnace at a vertex inside a dielectric read **-14.0% with guiding OFF and -14.8% ON** (row (f)), because `PolishedSPF::Pdf` (Fresnel from `ior_stack.top()`: 0.266774 at top 1.0 vs 0.296169 at 1.5) and `TranslucentSPF::Pdf` (`containsCurrent()`: 0 vs 0.185406) really do vary with the stack. `EvaluateDirectLighting{,NM}` now take the caller's live `IORStack` (PT's PART 2 and HWSS NEE sites, and `DirectLightingShaderOp`, pass theirs; the volume and BSSRDF NEE sites keep the null default because neither of their materials reads a stack). Stated consequence: a guiding-OFF render of a stack-reading material changes -- it was mis-weighted before. `PTGuidingMISPartitionTest` **37/0** (was 22/0; 34/3 on `2ebcaff9`). **Review round 4 (same slice, same day) found no P1 and one more live partition defect at the design's own edge, fixed under this row.** Rounds 2-3 read `p_aggregate(w) <= 0` as "the BSDF-sampling technique never generates `w`" and gave weight 1 to BOTH sides; under guiding that is false, because the continuation is drawn from a MIXTURE whose guide term reaches directions the material's own sampler cannot (PT's one-sample branch traces the guided direction whenever `combinedPdf > NEARZERO`, which `alpha*guide` alone satisfies), so that whole region's energy was counted TWICE. Reachable set: any direction with a nonzero BSDF value but zero aggregate sampling density -- the four full-sphere BSDFs (hair, fabric, weave, translucent), whose NEE hemisphere rejection is deliberately disabled, and any partial-support lobe. Fix: `PTGuidingMisPdf::Eval` returns the full mixture whenever guiding is ACTIVE (passing the aggregate through untouched when it is not, so "no partner" survives where it is true), and the four NEE arms call the hook OUTSIDE their `pdf > 0` gates, gating the weight on the BLENDED value. Red-proof, two new rows on the tilted-hemisphere decorator SPF (`Pdf` EXACTLY zero outside a hemisphere 45 degrees off N, albedo-1 Lambertian BRDF over the whole upper hemisphere): (h) env-NEE vs the env escape with the guide aimed into that wedge, **+10.57 %** on `0860f78f` (+12.23 % at 4x samples, converging on the closed-form doubled-wedge excess `(1-cos45)/2 = 14.6 %`) -> **-0.35 %**; (i) area-NEE vs the emitter hit with the emitter ENTIRELY inside the wedge, **+100.35 %** -> **-0.03 %**, its closed form being `L_e*(R/d)^2*cos(beta)`. Both rows carry a guiding-OFF CONTROL, green before and after. `PTGuidingMISPartitionTest` **63/0** (61/2 on `0860f78f`). Round 4 also forwarded the MIS partner through the fourth HWSS delegation (latent -- no producer can currently reach it with a differing value), and corrected two doc overclaims: the legacy shader chain still does NOT partition (`Reflection`/`Refraction`/`DistributionTracing`/`FinalGather` ops all continue with a default-constructed `RAY_STATE`, so `MisPartnerPdf()` is 0 and `EmissionShaderOp` weights at 1), and the hoisted unconditional `ApplyCosineProduct` changes the guided SAMPLING distribution for glossy lobes as well as the weight (unbiased -- the throughput divides by the post-product `combinedPdf` -- but a variance change, unmeasured). A clay_lights concern raised in review was REFUTED by reading: PART 3's `pSPF` is `pClaySPF` under the override and `ClayNEEMaterial::Pdf` delegates to the same instance, so both sides evaluate one function. See [DL74_ENV_NEE_GUIDING_PARTITION.md](DL74_ENV_NEE_GUIDING_PARTITION.md) §8 and §9. | M | physics-bias | user-visible (guided PT with an environment OR area light, every guiding mode) |
| DL-84 | DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md: the THIRD volume continuation | `PathTracingIntegrator.cpp`'s own in-loop volume phase-scatter continuation (`IntegrateFromHitTemplated`, the `bsdfPdf = effectivePdf` site reached after a surface bounce) trains optimal-MIS with NEITHER a correct `bsdfTimesCos` NOR an `AccumulateCount`. DL-72 round 3 wired the analogous quantity at `RayCaster`'s two volume sites and PT's two BSSRDF sites, but this one was never in its list. Until 2026-09-14 it carried the PREVIOUS (surface) vertex's `bsdfTimesCos` -- a moment whose numerator and denominator come from different vertices; that is now cleared to zero (conservative: the `f2 > 0` gate simply skips training), so the defect is a training-INPUT GAP, not a wrong moment, and the render-time weight is unaffected. Recipe: set `rs2.bsdfTimesCos`'s in-loop analogue to `phasePdf` broadcast (DL-72's volume ruling: a phase function's "BSDF*cos" is its own value, which for a normalized phase equals its pdf) AND add the paired `AccumulateCount` before the Russian roulette, then extend `OptimalMISTrainingSitesTest` with a fog-box-plus-surface fixture -- its existing volume case drives `RayCaster`'s site, not this one. OPEN-confirmed 2026-09-14 (debt-guiding2 slice, review round 3). | S | precision | not user-visible (optimal-MIS training input only; alpha estimate for `optimal_mis TRUE` scenes with a surface bounce into a medium) |
| DL-75 | RayIntersectionGeometric.h: `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived`; HairGeometry.cpp ~:1029 | SSS on hair geometry has no defined outward entry normal: `HairGeometry` sets `bGeomNormalOrientedToRay = true` unconditionally with a ray-DERIVED (fabricated) normal, so `BSSRDFSampling.cpp`'s entry-probe orientation-recovery rule (`oriented ? -raw : raw`, DL-71) cannot apply to it — there is no genuine "other side" to recover | OPEN-confirmed (found auditing DL-71's P1 fix for sibling geometry types, debt-sssenv round-2; not independently red-proved — no hair+SSS test fixture built this pass) | `src/Library/Geometry/HairGeometry.cpp` ~:1029 (`ri.bGeomNormalOrientedToRay = true;`, unconditional on every hit, followed by the new `ri.bGeomNormalRayDerived = true;` this pass) and `HairGeometry.h` ~:193-206 (class-comment rationale: "a hair ribbon is constructed to face the ray... normal was oriented to oppose the ray rather than read off a fixed surface orientation"). `BSSRDFSampling.cpp`'s probe loop now gates its orientation correction on `bGeomNormalOrientedToRay && !bGeomNormalRayDerived`, so hair is correctly EXCLUDED from a correction that would be meaningless for it (negating a ray-derived normal just reports the opposite ray-derived direction, not a physical outward side) — but this means an SSS material assigned to `HairGeometry` (a scene author's choice the parser does not forbid) gets an entry normal straight from the ray-facing convention with NO orientation guarantee at all: `BSSRDFEntryAdapters.h`'s NEE gate (`cosTheta = Dot(vLightIn, vNormal) <= 0`) and the cosine-weighted continuation direction both silently do whatever the raw hair-ribbon normal happens to give, which is defined by the shading geometry's incident-ray convention, not by any physical "outside the fiber" concept. Recipe: decide what "SSS entry into a hair fiber" should even mean physically (the profile-based BSSRDF model assumes a locally-flat semi-infinite slab, which a sub-micron-radius fiber does not remotely resemble — this may be a `SubSurfaceScatteringMaterial`-on-`HairGeometry` combination that should be documented as unsupported/undefined rather than "fixed" to produce a specific number), then either (a) add a dedicated red-proof scene combining a `hair_geometry` object with an SSS material and characterize what actually renders today (likely inconsistent/incorrect but not crashing), or (b) add an explicit diagnostic/warning path when `SubSurfaceScatteringMaterial`/`RandomWalkSSS` is bound to hair geometry, since the underlying entry-normal model does not apply. | S | coverage/precision | latent (requires a scene that binds an SSS material to hair geometry — an unusual but not-forbidden combination; no crash, silently undefined orientation) |
| ~~DL-83~~ | ~~DL74_ENV_NEE_GUIDING_PARTITION.md: RIS-mode env-NEE/escape MIS partition violation under guiding~~ | ~~The SAME partition-of-unity violation DL-74 fixed for one-sample-MIS guiding also applies under RIS-mode guiding and was deliberately left unfixed there~~ | CLOSED 2026-09-14 (debt-guiding2 slice, review round 2) — closed BY DL-74's second repair, in the same commit | The premise this row was filed on was wrong, and the row says so rather than claiming a separate fix: it assumed the NEE side must reproduce the escape side's SAMPLING density (`risEffectivePdf`), which indeed has no closed form at an externally-fixed direction. MIS does not require that — it requires only that both sides evaluate the SAME function of direction, and that function is free (weights need only partition to one; the true densities enter the estimator solely as the untouched divisors). DL-74's repair therefore defines a NOMINAL density that does not mention the sampling mode at all, and RIS gets the identical treatment as one-sample MIS with no RIS-specific derivation. Red-proof: `tests/PTGuidingMISPartitionTest.cpp` row (e) (`eGuidingRIS`, `guidingAlpha` 0.9, skewed trained field): the env furnace read **0.204389 against a closed form of 0.6 (-65.9%)** on `8fcce0bf` and **0.599093 (-0.15%)** after. | M | physics-bias | user-visible (guided PT with an environment light, RIS sampling mode) |
| DL-63 | DL37_GGX_DIFFUSE_TRANSMISSION.md: independent specular-only sweep | GGX height-correlated single scattering receives compensation from a separable-masking energy LUT, producing specular-only furnace gain | OPEN-confirmed (reproduced baseline; unchanged by the DL-62/DL-64 fix) | `GGXDiffuseTransmissionTest`: identical three specular-only failures before/after DL-37 AND before/after DL-62/DL-64 (`dfdd5ee1`, bit-for-bit re-verified against a stashed pre-fix tree): `150 checks, 3 failures` -- Schlick iso F0=1 alpha=0.6 theta=80 (1.0772), alpha=1 theta=60 (1.0432), alpha=1 theta=80 (1.1467). These are F0=1 (pure specular) configs, outside DL-62's filter-width scope and DL-64's F0=0 scope. `GenerateMicrofacetEnergyLUT.cpp` integrates G1(wo), while GGXBRDF uses height-correlated G2; these are different single-scatter models. | M | physics-bias | user-visible (rough GGX specular over-energy) |
| ~~DL-66~~ | ~~AgentLiveCommitTest.cpp: Test 31 fixture vs. CST brace-on-own-line rule~~ | ~~A regression fixture engineered to test right-side glue-safe Undo restore is rejected outright by an unrelated, apparently-later-hardened parser rule~~ | CLOSED 2026-09-14 (debt-guiding2 slice) | Confirmed a genuine regression, not a fixture relying on since-forbidden syntax: `git show -s --format='%ci' e5d653e0` (the test's own introducing commit) is 2026-07-04, two months BEFORE `bc3c8b71` (the brace-on-own-line hard-reject, 2026-09-03) -- `git merge-base --is-ancestor e5d653e0 bc3c8b71` confirms the ordering. Fixed by redesigning the fixture's separator (test-only change, `tests/AgentLiveCommitTest.cpp`): matA's chunk is now followed by `"\n "` (one newline, one space) instead of zero whitespace before matTarget's chunk. This satisfies the parser (`NextDocContentSharesLine` stops at the first Trivia containing a `\n`, so `closeSameLine` is false) while STILL tripping `DocEraseChunkTidy`'s glue-unsafe path (the tokenizer folds `"\n "` into ONE Trivia leaf whose LAST byte is the trailing space, not `\n`, so `ItemEndsInNewline` on it is still false) -- the exact same `droppedCount==1` precondition the test needs, reached without any same-line brace. Red-proof (this HEAD, via a temporary revert/reapply, not stash): pre-fix run gives `Job::LoadAsciiSceneViaCst:: derive diagnostic: lambertian_material (line 18): chunk braces must be on their own lines` then `FAIL: the zero-whitespace-glued fixture scene loads via the CST path`, `868 passed, 1 failed`; post-fix run gives `884 passed, 0 failed` (0 `FAIL` lines). | S | coverage/test gap | test-only (regression coverage gap closed; no production rendering/state-machine behavior implicated) |
| DL-65 | debt-ggx slice sibling audit (docs/skills/audit-by-bug-pattern.md), found while closing DL-62 | The DL-62 pattern (SPF widens sampling/density roughness by `ri.glossyFilterWidth`; the paired BRDF's `value`/`valueNM` evaluation does not) is structurally present, unfixed, in two more material families | OPEN-confirmed (static evidence; red-proof pending) | `CookTorranceSPF::Scatter`/`ScatterNM`/(implicit `Pdf`/`PdfNM` alpha reads) add `ri.glossyFilterWidth` at `CookTorranceSPF.cpp:152-153,365-366,549-550`; `CookTorranceBRDF.cpp` never references `glossyFilterWidth`. `SchlickSPF::Scatter`/`ScatterNM`/`Pdf`/`PdfNM` add it at `SchlickSPF.cpp:296-298,368-369,427-429,483-484`; `SchlickBRDF.cpp` never references it either. Each is its own roughness/Fresnel parameterization (`CookTorranceBRDF.cpp`'s `ToScalarAlpha`, `SchlickBRDF.cpp`'s single roughness scalar) needing its own red-proof, out of the GGX-scoped debt-ggx slice. The DL-64 pattern (raw-F0 lobe-selection weight losing grazing energy at F0=0) was checked against both and does NOT clearly replicate: `CookTorranceSPF` has no Schlick-F0 branch (its `pSpecular` tint directly scales a physically-computed conductor Fresnel, so tint=0 is a deliberate zero-specular material, not lost grazing energy); `SchlickSPF::Scatter`/`ScatterNM` sample their Schlick half-vector lobe unconditionally (`s.kray = rho + (1-rho)*fresnel`) rather than through a raw-F0-weighted lobe-selection probability. | M | physics-bias | user-visible (filtered CookTorrance/Schlick continuation versus direct evaluation) |
| DL-05 | CLOTH_FABRIC_DESIGN.md §15 item 27 | Two-layer gapped weave with the light outside: PT under-reads BDPT/VCM by 1.28-1.55x because PT's binary NEE cannot see through the far layer's delta gap lobe; single layer or light inside is exact | OPEN-confirmed | Doc's own measured table (box/planes, gap 0.1/0.3); mechanism traced to `RayCaster::CastShadowRayTransmittance` (definition starts `RayCaster.cpp:2062`, re-derived this sweep — the previously cited ~1980 was drift) being gated to perfect-specular dielectrics only, confirmed present as described this sweep | L | physics-bias | user-visible |
| DL-06 | IMPROVEMENTS.md §"VCM env-IBL" (Session 9-13) / CLAUDE.md "Env-IBL deficit" entry | VCM env+mesh strict-tolerance residual (env-S0 <-> env-NEE MIS partition violation) — Session 13 explicitly decided to STOP and accept the disc-area baseline rather than fix it; `plank_closeup`'s VCM 0.55x (RENDERING_INTEGRATORS.md debt 28) is the same known bias class, not a new bug | OPEN-confirmed (deprioritized, not fixed) | `docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md` "Session 13 outcome"; `IMPROVEMENTS.md` lines ~1030-1046; still true in this tree — no VCM env-branch SA-MIS migration commit exists (`git log --oneline -- src/Library/Shaders/VCMIntegrator.cpp` shows no such commit after Session 13) | L | physics-bias | user-visible |
| DL-07 | CLOTH_FABRIC_DESIGN.md §15 item 17 / WETNESS_COAT_DESIGN.md §12 item 13 | `OrenNayarBRDF::hemisphericalAlbedo` over-estimates (measured ~12.6% high at roughness 0.5, ~25.6% at 1.0), which over-amplifies `fabric_material`'s energy-subtraction and `coated_material`'s Saunderson recycling denominator; not fixable in either wrapper, needs its own bake | OPEN-confirmed | `OrenNayarBRDF.cpp:148-190`'s own doc comment states the bias and that "no clean closed form exists to correct it with"; unchanged this sweep | L | physics-bias | user-visible (rough Oren-Nayar under fabric/coat) |
| DL-08 | RENDERING_INTEGRATORS.md debt 29 | `PSSMLTSampler`'s 49-lane stream layout is exceeded by `BDPTIntegrator`'s eye-subpath walk (`StartStream(16u+depth)`) at eye/volume depth >= 32, aliasing MLT's own film/lens/aperture lanes on stream 48 | OPEN-confirmed | `BDPTIntegrator.cpp:1732`; `PSSMLTSampler.cpp:59,75,146-147` (`kNumStreams` default 49, `idx = streamIndex + kNumStreams*sampleIndex`), read directly this sweep; `maxVolumeBounce` default 64 reaches depth 32 in ordinary deep-volume scenes | L | physics-bias | user-visible (MLT + deep volume/eye recursion) |
| DL-38 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: independent residuals | Translucent exit weights lose their stateful extinction/scattering factors when reevaluated through the BSDF, including HWSS companions | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentSPF` has no `EvaluateKrayNM` override; PT HWSS companions use `TranslucentBSDF::valueNM*cos/pdf`, whose data omit extinction/scattering/inside state. `BDPTIntegrator::GenerateEyeSubpathImpl` / `GenerateLightSubpathImpl` reevaluate non-delta BSDF weights; VCM/MLT share these generators. | L | physics-bias | user-visible (translucent bidirectional/HWSS transport) |
| DL-41 | DL02_TRANSLUCENT_EXIT_DENSITY.md: independent residual | Translucent Pdf/PdfNM omit Phong lobes and selection probabilities; reverse/NEE queries lack the full stateful mixture contract | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentSPF::Pdf` returns only diffuse cosine; BDPT eye/light forward densities include `selectProb`, while reverse `PathValueOps::EvalPdfAtVertex` calls Pdf/PdfNM directly. `LightSampler` area/environment RGB/NM NEE uses empty `defaultIOR`. | L | physics-bias | user-visible (translucent mixed-lobe MIS) |
| DL-47 | DL03_GUIDED_IOR_CONTINUATION.md: guide-created entry residual | A guide can replace translucent entry reflection with transmission without generating the missing entry membership | OPEN-confirmed (static evidence; red-proof pending) | TranslucentSPF entry reflection is diffuse/non-delta with null ior_stack. TranslucentBSDF permits cross-side transmission; a positive guide PDF makes one-sample PT/BDPT accept it, but the null selected-stack case retains absent membership. The next physical exit is classified as entry. This absent-state producer contract predates DL-03. | L | physics-bias | user-visible (guided translucent entry from a reflection proposal) |
| DL-49 | DL04_SSS_RADIANCE_DECISION.md: exterior index | SSS profile and random-walk boundary evaluations assume exterior air while surface sampling uses the actual enclosing IOR | OPEN-confirmed (static evidence; red-proof pending) | Profile Fresnel/GetIOR, RandomWalkSSS entry/exit refraction, PT/BDPT entry factors/adapters and rough SubSurfaceScatteringBSDF evaluations use absolute IOR against 1; SubSurfaceScatteringSPF reflection reads ior_stack.top(). Common scaling of exterior/interior indices changes an otherwise identical relative-index problem. | L | physics-bias | user-visible (SSS surrounded by non-air media) |
| DL-24 | WETNESS_COAT_DESIGN.md §12 item 2 | `CompositeSPF`'s random walk loses ~96% of the energy in the coat-over-diffuse configuration wetness needs; this design routes around it rather than fixing it | OPEN-confirmed | Doc's own measured 96% figure (`WETNESS_COAT_DESIGN.md:101,454-488`); `CompositeSPF.cpp`'s 50/50 `Pdf` and top-wins `GetBSDF` architecture is unchanged by the tree's most recent commits touching that file (`7713e509`/`9e3cf85e`/`f4336aba` fix capacity naming, gap-slant-length and a parameter name, none touch the energy-loss mechanism), confirmed this sweep | L | energy-loss | user-visible (any `composite_material` coat-over-diffuse) |
| DL-09 | REFRACTIVE_RADIANCE_SCALING.md §10.2 | `RadianceEtaScale` reads the IOR stack's push/pop-time values while `DielectricSPF`/`PerfectRefractorSPF` re-fetch the `ior` painter fresh at each hit; for a spatially-varying `ior` these can differ at an interior vertex (NEE/bounce before exit) | OPEN-confirmed, unexercised | `IORStack.h`'s `RadianceEtaScale` doc comment; re-checked this sweep: `grep -rl 'ior.*scalar_painter'` is NOT empty — `scenes/Tests/GUI/panel_stress_params.RISEscene:232` binds `ior panel_scalar_dispersion`, a `scalar_painter` — but that painter's body (`:182-188`) is three per-channel CONSTANT `param`s combined algebraically (`vec3(ior_r, ior_r+spread*0.5, ior_r+spread)`), with no positional (`P`) term, so it is not actually spatially-varying and does not exercise this row; still no scene binds a position-dependent `ior` | S | precision | internal (no scene exercises it yet) |
| DL-10 | RENDERING_INTEGRATORS.md debt 28 (fisheye residual) | Fisheye camera's per-pixel solid angle is computed in the pre-stretch local frame while `mxTrans` applies `Stretch(pixelAR,1,1)`; at `pixelAR != 1` the world-space solid angle differs by an uncomputed direction-dependent Jacobian | OPEN-confirmed, unexercised | Doc's own analysis; confirmed no in-tree scene pairs `fisheye_camera` with non-square `pixelAR` (`grep -rl fisheye_camera scenes/` cross-checked against pixelAR values) this sweep | S | precision | internal (no scene exercises it yet) |
| DL-29 | WETNESS_COAT_DESIGN.md §12 item 10 | A measured `tau` curve is grey under the default RGB rasterizer because `PiecewiseLinearScalarPainter` broadcasts one 555nm sample and reports no per-channel variation | OPEN-confirmed | `PiecewiseLinearScalarPainter.h:108` (`HasPerChannelVariation() const override { return false; }`), confirmed unchanged this sweep | S | precision | user-visible (measured spectral tint vanishes under RGB pathtracing) |
| DL-11 | CLOTH_FABRIC_DESIGN.md §15 item 18 | Sheen directional-albedo table's middle band (`kMinSheenAlpha` <= n·v < 0.0349) still reads +0.75% over 1 after the round-9 64x64 rebake; the bottleneck moved (α ≈ 0.065 roughness-floor mechanism), not closed, and root-cause work is unstarted | OPEN-confirmed | Doc's own round-9 table; `tests/SheenDirectionalAlbedoTest.cpp` and `tests/LayeredWhiteFurnaceTest.cpp` grazing-check both still assert the *bounded*, not *exact*, posture (checked this sweep — no round-10 commit exists) | M | precision | user-visible (subtle, grazing sheen) |
| DL-12 | CLOTH_FABRIC_DESIGN.md §15 item 15 | Mirrored UV seams flip weave direction: `vShadingTangent` has no `bitangentSign` companion, so a `dpdu`-derived tangent can mirror across a UV seam | OPEN-confirmed | `RayIntersectionGeometric.h:469-470` (`vTangent` paired with `bitangentSign`) vs `:516` (`vShadingTangent`, no sign field) — re-derived this sweep, the previously cited `:296-299`/`:344-345` had drifted | M | precision | user-visible (mirrored-UV mesh + weave) |
| DL-13 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 3 | Displaced geometry curvature is ambiguous between base (`smoothing=1`) and displaced (`smoothing=0`) surface; only `EllipsoidGeometry`/`DisplacedGeometry` override `ComputeAnalyticalDerivatives` at all, and lookdev often wants base curvature the baked path can't isolate tessellation-independently | OPEN-confirmed | `DisplacedGeometry.cpp:471` (`ComputeAnalyticalDerivatives` definition — re-derived this sweep, the previously cited `:407+` had drifted); confirmed no third override or documented convention added since this heading was written | M | precision | user-visible (wear/relief masks on displaced meshes) |
| DL-14 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | Ray differentials (`fw`/`fwo`) are 0 under BDPT/VCM/MLT — the FIELD exists and is carried, but no differentials are ever computed for those walks, so it stays zero; a PT-vs-bidirectional difference on filtered-noise / footprint-driven bands | OPEN-confirmed | **Corrected this sweep**: the previous evidence ("no `BDPTVertex` differential field exists", citing `BDPTIntegrator.h`) was wrong on the facts — `BDPTVertex` (`src/Library/Shaders/BDPTVertex.h`) DOES carry the field: `TextureFootprint txFootprint;` at `:162`, whose own comment reads "pixel footprint; all-zero under today's bidirectional rasterizers (they emit no ray differentials) but carried so a future landing cannot silently reopen the gap". So the open work is plumbing ray differentials through the BDPT/VCM/MLT eye subpath (mirroring the S1 `derivatives`/`signals` widening the same struct already got) so `txFootprint` gets populated, not adding a new field | M | precision | user-visible (footprint-aware texture filtering under BDPT/VCM/MLT) |
| DL-15 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | An eccentric ellipsoid neighbour's distance bound is loose by its semi-axis ratio (4:1 measured: true distance 2.75 reported as 11.0), so `proximity(r)` silently paints nothing unless the author inflates the radius by the ratio | OPEN-confirmed | Doc's own measured example; Phase 3 (shipped 2026-09-09) makes σ exact but "`x sigma_max` remains a bound attained only along the top singular vector" — confirmed the bound, not exactness, is what Phase 3 delivered, by re-reading §10's Phase-3 paragraph this sweep | M | precision | user-visible (unpainted seam near eccentric ellipsoids) |
| DL-31 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | A mesh neighbour is a SHEET (no inside test), unlike every solid family which clamps its signed field at zero; `interior(r)` did not close this either, since a mesh contributes 0 to it too | OPEN-confirmed | Doc's own §10 analysis; no closed-mesh containment test exists on `Object.cpp`'s `DistanceToSurface`/`ObjectManager::DeepestOtherContainment` paths, confirmed this sweep | M | precision | user-visible (a receiver buried inside a closed mesh neighbour reads no contact) |
| DL-16 | CLOTH_FABRIC_DESIGN.md §15 item 4 | `ggx_material.tangent_rotation` stays Color-pipe only; the promised Scalar-pipe alias (so a `fabric_material`'s `weave_rotation` and its substrate's own rotation can share one painter) was never added | OPEN-confirmed | `ChunkParserRegistry.cpp:4495` — `tangent_rotation`'s only descriptor entry is `ParameterPipe::Color`, `p.description` states "a scalar_painter does NOT bind here"; no second `tangent_rotation`-family scalar parameter exists (grepped this sweep) | S | API/bridge gap | user-visible (authoring: can't drive both rotations from one field) |
| DL-17 | CLOTH_FABRIC_DESIGN.md §15 item 12 | glTF PER-TEXEL `anisotropy_rotation` (the direction encoded in the anisotropy texture's R/G channels) is still dropped at import and falls back to the scalar rotation, even though the expression VM's `atan2` (confirmed present) makes the sketched fix executable today; the SCALAR `anisotropy_rotation` IS wired through (`Job::AddPBRMetallicRoughnessMaterial`, `tests/PBRMaterialAPITest.cpp` Test 3) | OPEN-confirmed | `GLTFSceneImporter.cpp:1300-1307`'s comment stands; `ChunkParserRegistry.cpp:4560`'s `anisotropy_rotation` descriptor still reads "Phase 1 reads but does not yet APPLY the rotation" — read directly this sweep | S | API/bridge gap | user-visible (glTF import only) |
| DL-23 | CLOTH_FABRIC_DESIGN.md §15 item 16 (tail) / IMPROVEMENTS.md "Clearcoat over `fabric_material` — not composable, unowned" | `coated_material`'s substrate allowlist does not admit `fabric_material`/`weave_material`, so a coat-over-fabric composition (e.g. waxed canvas) is unreachable | OPEN-confirmed | `CoatedMaterial.h:112-113` (`SubstrateAllowlistText()`: "lambertian_material, orennayar_material, ggx_material, pbr_metallic_roughness_material") and `IsSupportedSubstrate` `:120-134` (the `dynamic_cast` allowlist) — `fabric_material`/`weave_material` absent from both, confirmed this sweep; IMPROVEMENTS.md's entry adds that this is a named glTF-import consequence (`KHR_materials_sheen` + `KHR_materials_clearcoat` together lose the clearcoat layer, warn-and-skip named in `GLTFSceneImporter.cpp`) | S | API/bridge gap | user-visible (authoring: can't compose a coat over fabric) |
| DL-26 | WETNESS_COAT_DESIGN.md §12 item 6c | `add_wetness` and `add_wear` mutually exclude on one material; worn-and-wet, the flagship subject, is unreachable | OPEN-confirmed | `src/Library/Agent/AgentSession.cpp` ~8021/8092/8328 ("add_wear / add_wetness cannot currently be combined on one..."), confirmed present this sweep | S | API/bridge gap | user-visible (agent-authored worn-and-wet materials) |
| DL-28 | WETNESS_COAT_DESIGN.md §12 item 9 | Water-absorption spectral files must be pre-converted to a transmittance base because `dielectric_material`'s `tau` is `pow(tau,distance)`, not `exp(-sigma*distance)`; a pasted-in published sigma_a table is silently wrong | OPEN-confirmed | `DielectricSPF.cpp:318-323` (`pow(tauVals.v[i], distance)`), confirmed unchanged this sweep; no runtime validation or warning exists for a mismatched-convention input file | S | API/bridge gap | user-visible (authoring trap only, silent) |
| DL-32 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `standard_object`'s `scale` written with ONE number (e.g. `scale 0.35`) derives to a degenerate transform silently — no diagnostic, the object vanishes from the render, and `DistanceToSurface`'s `sigma_min<=0` gate then refuses every proximity query against it | OPEN-confirmed | `ChunkParserRegistry.cpp`'s `standard_object` `scale` descriptor (`DoubleVec3`, no partial-fill diagnostic); `Object.cpp:1708` warns only for an ANISOTROPIC transform's `proximity()`, not a degenerate one — confirmed no parser-side warning for a partially-specified `DoubleVec3` this sweep | S | API/bridge gap | user-visible (silent scene-authoring trap) |
| DL-18 | CLOTH_FABRIC_DESIGN.md §15 item 13 | The Blender bridge has no sheen, anisotropic, or velvet mapping at all — Principled's Sheen sockets have no `fabric_material` target | OPEN-confirmed | No `fabric_material`/`sheen` reference found in the Blender bridge sources this sweep (`grep -rl fabric_material` under the Blender add-on tree returns nothing) | M | API/bridge gap | user-visible (Blender-authored scenes only) |
| DL-25 | WETNESS_COAT_DESIGN.md §12 item 6b | Phase 1 cannot darken a textured substrate: the expression VM has no painter-sampling builtin | OPEN-confirmed | `src/Library/Painters/ExpressionEval.h` function table (~lines 1166-1171) has no painter-sample builtin alongside `sin`/`cos`/`atan2`/etc.; confirmed absent this sweep by grep | M | API/bridge gap | user-visible (wet textured substrates can't darken) |
| DL-19 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | Two S3 conversion sites (BDPT's NM-hero `Le` rebuild, the HWSS companion `rigW` rebuild) share the signals-replay helper but have no dedicated red-proof — their contribution is MIS-weighted to a few percent on the money test's scenes, so skipping them moves the suite by <= 5.7% / 0% | OPEN-confirmed (test gap) | Doc's own §10 disclosure; corrected this sweep — the previous citation ("`tests/SignalEmitterRecordTest.cpp` unchanged in this tree") is now stale: `ac9891f3` added `RunBoundedNeighbourRead()` (the DL-36 two-blade fixture, now called unconditionally from `main()`) to that file, but a diff of the change shows it adds no coverage for the BDPT NM-hero `Le` rebuild or HWSS `rigW` rebuild sites this row names — the gap is unchanged, only the file is not | S | coverage/test gap | internal (test-suite blind spot) |
| DL-27 | WETNESS_COAT_DESIGN.md §12 item 7 | The wet-highlight variance cost is unmeasured | OPEN-confirmed | No test or scene mentioning "wet_highlight"/"WetHighlight" found in `tests/` or `docs/*.md` this sweep other than the design doc itself | S | coverage/test gap | internal (measurement gap, not a known defect) |
| DL-30 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 1 (disclosed residual) | A CSG exit-designated subtraction branch's `dndu` pairing is unverified, reachable only through a nested-CSG construction no test currently produces | OPEN-confirmed, untested | Doc's own disclosure (§14 item 1, appended when item 1 was RESOLVED 2026-08-29); no nested-CSG `dndu`-pairing test found in `tests/CsgSurfacePayloadTest.cpp` or elsewhere this sweep | S | coverage/test gap | internal (no scene exercises it yet) |
| DL-40 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: review residual / DL36_EMITTER_NEIGHBOUR_PIN.md: harness sibling | Balance and signal-emitter harness comparisons can accept nonfinite candidate statistics | OPEN-confirmed (static evidence; red-proof pending) | BDPT/VCM `ComputeStats` accepts nonfinite capture values and `ChannelsAgree` rejects only `fabs(a-b)/denom > tolerance` (false for NaN). SignalEmitterRecordTest similarly marks nonempty captures valid and `WorstRelDiff` uses fmax, which can discard NaN differences. Recorded DL-01/DL-02/DL-36 results are finite. | S | coverage/test gap | internal (false-green risk) |
| DL-60 | DL34_UNION_INTERIOR_DEPTH.md: committed scenarios without replay fixtures | Two committed scenarios lack replay fixtures and fail the dynamically enumerated checkpoint suite before scene execution | OPEN-confirmed (reproduced baseline) | `altar_stress.json` and `rainwet_closeup.json` omit replay fixtures; `AgentEvalRunner::RunScenario` returns load_error. AgentEvalCheckTest reproduces 11 and 12 cascading assertions at pre-DL-34 e858b4c9 and compiled 3927ec9c. | S | coverage/test gap | internal (missing replay coverage) |
| DL-20 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 2 | Patch geometries report flat curvature (`valid=false`) while genuinely curved; deferred to Phase 4, no Phase-4 work has landed | OPEN-confirmed | No patch-geometry curvature override exists (only `EllipsoidGeometry`/`DisplacedGeometry` override `ComputeAnalyticalDerivatives`, confirmed this sweep alongside DL-13) | M | coverage/test gap | user-visible (curvature-driven wear on patch geometry reads absent, not wrong) |
| DL-21 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 5 | CSG boundary curvature behaviour is unspecified/undecided (forward the contributing surface's curvature, or invalidate at the seam) | OPEN-confirmed | `tests/CsgSurfacePayloadTest.cpp:833-834` exercises the derivative fields there but does not pin a curvature convention at the boundary — confirmed by reading the referenced lines this sweep | M | coverage/test gap | user-visible (CSG seam wear masks) |
| DL-22 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | BSSRDF entry vertices read default (neutral) `derivatives`/`signals` under both PT and BDPT — integrator-consistent, but a signal-keyed IOR/Fresnel painter at a subsurface entry point is neutral rather than live | OPEN-confirmed | Doc's own §10 first bullet; closing it needs a probe record in `BSSRDFSampling::SampleResult`, confirmed absent this sweep | M | coverage/test gap | user-visible (signal-keyed SSS entry only) |
| DL-61 | DL34_UNION_INTERIOR_DEPTH.md: committed render oracles disagree with replay | Constant-material and SDF reconstruction replay fixtures violate their committed render checkpoints | OPEN-confirmed (baseline mismatch; root cause unresolved) | AgentEvalCheckTest at e858b4c9 and 3927ec9c reproduces three constant_materials_polish mean-luma/aggregate assertions and six image_reconstruct_multi RMSE/aggregate assertions. These scenes have no CSG or cross-object signals. Investigate intended output before changing renderer or bands. | M | coverage/test gap | internal (render oracle disagreement) |
| DL-33 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior(r)`'s TLAS-backed candidate walk (`BVH::ForEachContainingPoint`) has a genuinely different cost shape from `proximity(r)`'s `NearestOtherSurface` (no shrinking-radius prune; visits every node whose box contains the query point) — NOT YET MEASURED | OPEN-confirmed, unmeasured | `BVH.h:2018` (`ForEachContainingPoint`); no distance-call-count or wall-clock measurement of it exists in `tests/` (the doc's own `NearestOtherSurface` figures, e.g. "11.08 distance calls per query on Sponza", have no `interior(r)` counterpart), confirmed this sweep | S | perf | internal (cost-shape unmeasured, not a correctness bug) |

## Already resolved — verified this sweep, cited for completeness

These carry RESOLVED/CLOSED language in their source doc already; this sweep
independently re-verified the code exists as described rather than taking
the heading's word for it.

| id | source | claim | verdict | evidence |
|----|--------|-------|---------|----------|
| DL-R1 | RENDERING_INTEGRATORS.md debt 28 | BDPT/VCM/PT thin-lens t==1 aperture sampling | CLOSED-by-code | `ThinLensCamera::GetApertureWorldArea`/`SampleLensPoint`/`RasterFromLensPoint` exist; `tests/CameraImportanceTest.cpp` present, confirmed this sweep |
| DL-R2 | RENDERING_INTEGRATORS.md debt 30 | Radiance-mode eta^2 factor at dielectric interfaces | CLOSED-by-code | `RISE::RadianceEtaScale` in `Utilities/IORStack.h`; `tests/RefractiveRadianceScalingTest.cpp` present, confirmed this sweep |
| DL-R3 | RENDERING_INTEGRATORS.md debt 31 item 2 | `TranslucentSPF` per-channel-loop `IORStack` leak (unconditional on success, plus overflow-carryover) | CLOSED-by-code | `TranslucentSPF.cpp` ~lines 169-207 re-arms `delete_stack` before every allocation and frees a carried-over stack before reassignment, read directly this sweep |
| DL-R4 | CLOTH_FABRIC_DESIGN.md §15 item 3 | Furnace config 6 IS config 7's defect (`SheenSPF` never reaches the substrate; reflection-only by construction), now measured rather than asserted | CLOSED-by-test | `SheenSPF.cpp:73-99` (cosine-hemisphere draw gated below the geometric horizon); doc's own measured config 6 `{0.0875,0.1293,0.2812,0.5461}` vs config 2 (bare sheen) `{0.0875,0.1283,0.2810,0.5453}` — same curve to MC noise, confirmed unchanged this sweep |
| DL-R5 | CLOTH_FABRIC_DESIGN.md §15 item 19 | `CookTorranceSPF::PdfNM` reported a different mixture from `ScatterNM` (11 orders of magnitude out of family); fixed via achromatic lobe-selection weights + a `kSelFloor=1e-3` reachability floor | CLOSED-by-test | `CookTorranceSPF.cpp:54` (`kSelFloor`), `:75-78` (`ComputeLobeWeights`); `tests/CookTorranceHWSSTest.cpp` present, confirmed this sweep |
| DL-R6 | CLOTH_FABRIC_DESIGN.md §15 item 20 | BDPT/VCM "100-350x over-count" on a backlit sheer weave curtain was PT being the broken reference (the debt-21 self-hit bug deflating PT), not a BDPT/VCM defect | CLOSED-by-test | `tests/FabricRenderTest.cpp::TestBacklitSheerCurtain` (~926) and `::TestTouchingAreaLitCurtainAllIntegrators` (~1207) present with asserted BDPT/PT and VCM/PT bands; `AutoRasterizer.cpp:121` (`CouldLightPassThrough()` alone — the full-sphere exclusion is lifted), confirmed this sweep |
| DL-R7 | CLOTH_FABRIC_DESIGN.md §15 item 21 | Path tracing's diffuse-transmission lobe read super-linear in `transmit` because of a shadow-ray self-intersection epsilon bug in `RayBilinearPatchIntersection` (93.6% of NEE shadow rays spuriously self-shadowed), not an MIS defect | CLOSED-by-test | `RayBilinearPatchIntersection.cpp:160-162` (scale-relative `coordScale`/`tMin`); `tests/FabricRenderTest.cpp::TestAreaLitSheerWeave` (~1099) present, confirmed this sweep |
| DL-R8 | CLOTH_FABRIC_DESIGN.md §15 item 22 | A `fabric_material` over a `transmission thin` weave extinguished the weave's transmission (100% opaque, no diagnostic) because three sites dropped the substrate's `ScattersFullSphere`/`CouldLightPassThrough` flags and hemisphere gating | CLOSED-by-test | `FabricMaterial.h` forwards both flags; `tests/FabricMaterialChunkTest.cpp:1561` (`TestTransmissiveSubstrateForwarding`) present, confirmed this sweep |
| DL-R9 | CLOTH_FABRIC_DESIGN.md §15 item 23 | BDPT/VCM read ~7-10% under PT on a delta-light backlit thin weave because subpath vertex connectibility was decided from the sampled ray's `isDelta` flag instead of the surface's own BSDF availability | CLOSED-by-code | `BDPTIntegrator.cpp:2293` (`vertices.back().isConnectible = ( ri.pMaterial->GetBSDF() != 0 )`), confirmed present this sweep, matching the item's described fix |
| DL-R10 | CLOTH_FABRIC_DESIGN.md §15 item 24 | VCM read +3.0% over PT on every delta-light scene because `EvaluateNEEImpl` zeroed `wCamera` (a light-side strategy density) whenever the light was a delta position, not just `wLight` | CLOSED-by-code | `VCMIntegrator.cpp:1483-1485` (`camFactor` computed unconditionally from the emission-direction area density), confirmed present this sweep |
| DL-R11 | CLOTH_FABRIC_DESIGN.md §15 item 25 | A closed `box_geometry` whose material is a thin-transmissive weave read BDPT/VCM ~0.17x PT; root cause was `box_geometry`'s own ~1e-12 self-hit root on PT's unadvanced NEE shadow ray, not an integrator defect | CLOSED-by-code | `BoxGeometry.cpp:220` (`DropSelfHitRoot`), `:293` (called from `IntersectRay`); `tests/BoxGeometryTest.cpp:369,885-886` (`RunStandoffReentryContract` at scale 1.0 and 1000.0); `tests/CsgSurfacePayloadTest.cpp` present, confirmed this sweep |
| DL-R12 | CLOTH_FABRIC_DESIGN.md §15 item 26 | Legacy `pixelpel_rasterizer` read 2.46x under modern PT on a gapped weave + area light scene; root cause was a DIRECT-LIGHTING-ONLY shader chain casting no continuation ray at all (not a rasterizer defect) | CLOSED-by-test | `tests/BDPTStrategyBalanceTest.cpp:508` (`kRasterizerPT` uses `DefaultPathTracing`) and its topology F (gapped weave + area light), confirmed present this sweep |
| DL-R13 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 10 | Deforming/vertex-animated geometry would stale a bake — the item's ORIGINAL "no `EvaluateAtTime` implementation, so transform-only" assumption was wrong; `TriangleMeshGeometryIndexed::UpdateVertices` mutates vertex/normal arrays in place and refits the BVH, and Phase 3 drops bakes explicitly at every vertex-mutation site | CLOSED-by-code | `TriangleMeshGeometryIndexed.h:359` / `.cpp:338` (`UpdateVertices`), confirmed present this sweep, matching the item's corrected analysis |
| DL-R14 | CLOTH_FABRIC_DESIGN.md §15 item 8 / GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 11 / WETNESS_COAT_DESIGN.md §12 item 1 / CROSS_OBJECT_PROXIMITY_DESIGN.md §10 (bullet 1) | BDPT/VCM/MLT's hand-built vertex records and `LightSampler`'s hand-built NEE/light-subpath-root records read geometry/proximity signals as neutral instead of live | CLOSED-by-test | `BDPTVertex.h` widened with `derivatives`/`signals`/`txFootprint` (S1) and `LightSampler.cpp`'s NEE/light-subpath-root probe (S3); `tests/SignalIntegratorConsistencyTest.cpp` and `tests/SignalEmitterRecordTest.cpp` (77 checks) both present, confirmed this sweep — residuals (a BSSRDF entry vertex, probe-refusal cases, the bounded-neighbour-read) carried forward as DL-22 and DL-36 |
| DL-R15 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior()` saw a TLAS-backed scene's object set through the stale AABB snapshot while `proximity()` walked the live tree — an asymmetry that over-painted a just-added 7th object to `interior()` only | CLOSED-by-code | `ObjectManager.cpp:861` (`DeepestOtherContainment` now uses `BVH::ForEachContainingPoint`, the same TLAS `NearestOtherSurface` uses), confirmed present this sweep, matching `ProximitySignalTest`'s g2 case named in the doc |
| DL-R16 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | A CSG composite refused every proximity/interior query, making its own surface invisible to every neighbour | CLOSED-by-code | `CSGObject.cpp:2681,2758` (`DistanceToSurfaceWithArm`/`DistanceToSurface` implemented — union answers min over operands that answer, intersection/subtraction bracket the composed signed field), confirmed present this sweep (Phase 3 S3) |
| DL-R17 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | Non-uniform transforms used the loose Frobenius/determinant sigma bounds pre-Phase-3 | CLOSED-by-code (partial — residual folded into DL-15) | Phase 3 S2 ships a one-sided Jacobi SVD with the Frobenius/determinant pair only as the non-convergence fallback; doc's own measured `scale (3,1,0.4)` figures (search-radius inflation 8.46667x -> 2.5x); the surviving anisotropy-direction residual (`sigma_max` attained only along the top singular vector) is the same mechanism DL-15 tracks, confirmed this sweep |
| DL-R18 | WETNESS_COAT_DESIGN.md §12 item 3 | `composite_material`'s `extinction` slot was `IPainter` (a physical Beer-Lambert absorption coefficient silently JH-uplifted and clamped to [0,1]); retyped to `IScalarPainter`, CLOSED 2026-09-01 | CLOSED-by-test | Code: `CompositeSPF.h:52` / `CompositeEmitter.h:40` (`const IScalarPainter& extinction;`), `RISE_API.h:1069-1079`'s `RISE_API_CreateCompositeMaterial` and `IJob.h:994`'s `AddCompositeMaterial` both take the scalar-pipe slot, `ChunkParserRegistry.cpp:4352`'s `composite_material` `extinction` descriptor is `ParameterPipe::Scalar`. Test: `tests/CompositeExtinctionTest.cpp` `main()` (~476-492) asserts the NM and RGB attenuation ratios agree post-fix (0.11940 vs 0.11938) inside the shared Beer-Lambert band `[0.05,0.25]`, where the pre-fix `IPainter` routing measured 0.9582 (an order of magnitude off, the JH-uplift-saturation regression band the check guards against) — confirmed present and passing this sweep |
| DL-R19 | WETNESS_COAT_DESIGN.md §12 item 5 | Phase-1's `Rd·Rs·(1−c)` coverage-mask energy dip (up to ~50% loss at grazing, plus an added geometric-horizon coat-lobe drop in `PolishedSPF.cpp:206-207`) is closed by Phase 2's `coat_weight` mixture, which conserves energy at every coverage fraction instead of treating coverage as an attenuator | CLOSED-by-test | Code: `CoatedBRDF::value` (`CoatedBRDF.cpp:269`, mixture at 316-317: `fBase*(K*cp.weight + RISEPel(1,1,1)*(1−cp.weight)) + RISEPel(fCoat)*cp.weight` — coverage selects between a coated and a bare statistical state, not an attenuating factor). Test: `tests/LayeredWhiteFurnaceTest.cpp` config 13 ("Coated water c=0.5 / white Lambertian", 1595, `kPosturePass`, 2% band) asserts ρ = 1 at every angle at half coverage, explicitly contrasted in its own comment against config 9's Phase-1 dip (1529-1530, `kPostureMatchesPrediction`, ρ falls to 0.8265 at 80°) — confirmed present and passing this sweep |
| DL-R20 | WETNESS_COAT_DESIGN.md §12 item 6 | Spectral wet-darkening was unreachable in Phase 1 (the expression VM has no `nm` and `expression_painter` uplifts a computed RGB); closed by Phase 2's `valueNM` performing the darkening as per-wavelength recycling `1/(1 − r_i·R(λ))` transport rather than a separate exponent parameter | CLOSED-by-code, test coverage indirect | Code: `CoatedBRDF.h:38-46`'s doc comment ("SPECTRAL WET DARKENING IS TRANSPORT, NOT AN EXPONENT") and `CoatedBRDF::valueNM` (`CoatedBRDF.cpp:320`, substrate sampled at the hero wavelength via `pBase->valueNM` at 336). Test: `tests/LayeredWhiteFurnaceTest.cpp` config 15 (1694-1695, coloured GGX-PBR substrate, `kPostureMatchesPrediction`) quantifies the identical per-channel recycling formula in the RGB pipe (17x composite's energy at normal incidence, matching the Saunderson analytic form to within 0.001-0.014 at 0-60°); `tests/SPFBSDFConsistencyTest.cpp`'s `Coated_GGX_tinted_absorbing_c0.5` reciprocity row (1553) exercises `valueNM` itself, but only reciprocity at one fixed wavelength (550nm), not a magnitude sweep. **No test sweeps multiple wavelengths on one coloured coated substrate to directly assert the per-wavelength chroma-boost magnitude the item describes** — the spectral claim is verified only by (a) the RGB pipe running the identical per-channel formula and (b) a single-wavelength NM correctness check. Classified resolved (mechanism confirmed in code and exercised, but the doc's specific spectral-sweep claim has no dedicated multi-wavelength test) rather than silently closed |
| DL-R21 | WETNESS_COAT_DESIGN.md §12 item 6a | `polished_material::GetBSDF()` returning a bare `LambertianBRDF(Rd)` (no coat lobe) is a pre-existing defect that made Phase 1's NEE/BDPT/VCM connections see a dry substrate while sampled transport saw the wet split; closed for the wetness use case because Phase 2 re-targets `add_wetness`'s emission from `polished_material` to `coated_material` | CLOSED-by-test (re-target, not a `PolishedMaterial` fix) | Code: `PolishedMaterial.h:49` (`pBRDF = new LambertianBRDF( Rd_ );`) and `:57` (`GetBSDF`) confirmed UNCHANGED this sweep — the underlying defect in `PolishedMaterial` itself still exists and is not itself fixed; the closure is that Phase 2 routes the wetness verb around it entirely (`AgentSession.cpp`'s item-8 re-target, e.g. ~7918-7934/37962-37977/38585-38622, emits a `coated_material` wrapper chunk instead of a bare `polished_material`). Test: `tests/AgentAddWetnessTest.cpp:329,357-358` ("A MONEY: a `coated_material` chunk was minted") asserts the re-target, confirmed present and passing this sweep |
| DL-R22 | WETNESS_COAT_DESIGN.md §12 item 11 | A single per-channel darkening exponent (Phase 1's `pow(base,k)` fit) over-boosts saturation on already-saturated substrates because its implied `k` varies with base albedo; closed by Phase 2 never forming an exponent at all | CLOSED-by-code | `ChunkParserRegistry.cpp:3420-3421`'s `coated_material` descriptor comment states "deliberately NO `substrate_wet_exponent`"; grepped this sweep — no `substrate_wet_exponent`/`wet_exponent` parameter exists anywhere in `src/Library/` or `tests/`, only three comments explaining its deliberate absence (`ChunkParserRegistry.cpp:3420`, `CoatedBRDF.h:44-45`, `AgentSession.cpp:38591`). The replacement mechanism's correctness is the same `LayeredWhiteFurnaceTest` configs 14/15/17/18 analytic cross-checks DL-R20 cites. The item's specific "over-boosts an already-saturated substrate" failure mode is structurally unreachable now (no exponent parameter exists to mis-fit), so no test targets that failure mode by name — noted as a scope observation, not a coverage gap, since the mechanism the exponent approximated no longer exists |
| DL-R23 | WETNESS_COAT_DESIGN.md §12 item 12 | Near-white `coat_tint` values in [0.96,1.0) were spectrally discontinuous under the pre-Stage-C flat-illuminant JH LUT (measured collapse to 1.4e-7 at 780nm for a pure white); CLOSED 2026-09-02 by Stage C putting the reference illuminant into the LUT's forward model, leaving only a wavelength-dependent 1−ε asymptote that still needs the untinted-white guard | CLOSED-by-test | Code: `CoatedBRDF.cpp:107-120` (`out.tinted = ( minTint < Scalar(1) - Scalar(1e-6) )`, the untinted-white guard decided once from the authored RGB triple so the RGB and spectral pipes cannot disagree on "is this coat tinted at all"); `IPainter.h:164` (`IsUntintedWhite`) and `:185` (`GuardedGetColorNM`); `tools/JakobHanikaLUTGen.cpp:145-149` (Stage C's D65-in-forward-model retraining, removing the Bradford step). Test: `tests/CoatedMaterialChunkTest.cpp::TestUntintedIsClearSpectrally` (~524-570) asserts an untinted coat responds identically to an explicit-white and an explicit-neutral-Beer-Lambert coat at every 20nm step from 380-780nm, confirmed present and passing this sweep |

## Doc-rot found and struck this sweep

Struck in place in the source ledger with `CLOSED <date> — <evidence>`; not
reflowed otherwise.

| id | heading | why it was stale | commit |
|----|---------|-------------------|--------|
| DR-01 | CLOTH_FABRIC_DESIGN.md §15 item 1 | "No anisotropic material... in that sweep" — `tests/SPFBSDFConsistencyTest.cpp:1220` (re-derived this sweep — the previously cited `:1174` had drifted) already has `Fabric_GGXaniso_weave45` | `f458f4f3` |
| DR-02 | CLOTH_FABRIC_DESIGN.md §15 item 2 | "Charlie/Neubelt" naming already fixed everywhere, including the descriptor `cd.description` this item worried about | `f458f4f3` |
| DR-03 | CLOTH_FABRIC_DESIGN.md §15 item 7 | CLOSED 2026-09-12 — Zhu 2023/Jin 2022 params were obtained and shipped as `weave_material` | `8378266b` |
| DR-04 | CLOTH_FABRIC_DESIGN.md §15 item 9 | `kCapacity` is named, documented, and `CompositeSPF` warns once on drop | `f4336aba` |
| DR-05 | WETNESS_COAT_DESIGN.md §12 item 4 | `tidepools.RISEscene`'s comment was rewritten to describe the `painter`-bridge capability, no longer claims `tau` can't vary spatially | `aa1f0162c` |
| DR-06 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 6 | CLOSED 2026-09-12 — requirement (`vGeomNormal` orientation, so `curv` and `N` may legitimately disagree on a bump-mapped surface) is implemented and documented in force; re-derived this sweep against `tests/SurfaceCurvatureTest.cpp`'s case (e) `TestCurvInvariantUnderNormalPerturbation` (~398-440, confirms `curv` invariance under an applied `ReliefModifier`) and `SurfaceCurvature::MeanCurvatureFromDerivatives` (`SurfaceCurvature.h` ~87, whose doc comment ~57-71 states the field takes no normal by design) — the previously cited `RayIntersectionGeometric.h:280-296` was the wrong file/symbol entirely | (pre-existing, verified this sweep — no single fix commit) |
| DR-07 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 9 | CLOSED 2026-09-12 — assumption verified: no signal-demand cache couples to `Prepare()`/`SetEnvironmentSampler()` | (verified this sweep by grep; no code change needed) |

## Not-a-debt (design choices / observed-need gates / declined) — reviewed, no action

- CLOTH_FABRIC_DESIGN.md §15 item 5 — anisotropic Charlie sheen deferred; design choice, LTC route noted if ever needed.
- CLOTH_FABRIC_DESIGN.md §15 item 6 — Ashikhmin-Premoze-Shirley velvet normalization unverified; not implemented, not blocking.
- CLOTH_FABRIC_DESIGN.md §15 item 8 / GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 8 — `ScalarToPainterAdapter` doesn't exist; convenience gap, explicitly "not a blocker" in both docs.
- CLOTH_FABRIC_DESIGN.md §15 item 10 — weave aliasing has no automatic mitigation; authoring idiom (`fw` fade), not a defect.
- CLOTH_FABRIC_DESIGN.md §15 item 11 — multi-slot preset hypothesis unmeasured; unbuilt feature, not a defect.
- CLOTH_FABRIC_DESIGN.md §15 item 14 — Tier-1 spectral dye not attempted; future opportunity, non-goal-adjacent.
- CLOTH_FABRIC_DESIGN.md §15 item 16 — `fabric_material`'s own `hemisphericalAlbedo` factorization is closed-form and measured <= 0.64%; the larger residual is DL-07 (Oren-Nayar's own bug), not this item.
- GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 4 — vertex-normal quality is loader-dependent; documented limitation, no fix proposed, none needed.
- GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 7 — `expression_function2d` stays UV-only by design.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — non-convex coplanar clipped-plane refusal: safe-by-design (refusing rather than under-reporting).
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — emissive objects never count toward proximity: Phase-3-gated on observed need, not a defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `kL1Ways` cliff: superseded by Phase 3 S4 (8 ways, measured 0.8967->0.8968 hit rate), closed in-doc already.
- IMPROVEMENTS.md §8B/§8C (correlation-aware / efficiency-aware MIS for BDPT), §7D (deferred null-scattering volume item) — roadmap features explicitly marked deferred, not correctness bugs.
- WETNESS_COAT_DESIGN.md §12 item 8 — heightfield-mode SDF returns neutral occlusion silently. Doc's own words: "Not a bug (the fallback is deliberately do-nothing)... it produces a wrong-looking render with no diagnostic." A UX/diagnostics wish (warn when the target's geometry family cannot answer `occlusion()`), not a correctness defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the refusing-families behavior (RAW meshes, patches, hair, heightfield SDFs; a CSG intersection/subtraction with a sheet/heightfield-SDF operand, a union of two refusing operands, budget-exhausted brackets, a sub-1e-12 seam gradient all read far) is a documented, safe-by-design contract, not a bug — the failure direction is always "reads far / refuses" rather than under-reporting a true contact.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — SDF-neighbour distance is an upper bound by design (never over-reads contact), bounded by the last probe step; a candidate whose crossing is not found in budget reads far. Documented behavior, not a defect.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the reported range is `[0, maxDist)`, not `(0, maxDist]` (zero attained at interpenetration, the radius itself excluded); a stated, continuous convention, not a bug.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the eager snapshot build and per-ray `EnsureBoxSnapshot()` are gated on `ProximityDemand` rather than built unconditionally; a deliberate perf/correctness trade (an unregistered direct caller pays the build cost once under a lock instead of finding it pre-built), documented and accepted.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `proximity(r)` cannot drive `ReliefModifier`'s relief: confirmed by code comment, `ReliefModifier.cpp:279-283` ("DELIBERATELY LEFT ALONE: vNormal, derivatives, signals... holds them invariant under shading-normal [perturbation]"). A deliberate architectural invariant (the same one that makes `curv` bump-invariant), not an oversight.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — per-sample motion-blur staleness of a proximity/interior memo entry (a moving neighbour under a non-keyframed painter, or the scalar pipe's `time=0` stamp relying on the jitter argument alone): disclosed, and the doc's own analysis is that sub-pixel jitter makes a stale hit essentially unreachable in practice; no scene in the design's own corpus exercises it.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the TLAS-backed candidate-scan shape description (a leaf visits up to 4 occupants before pruning; a per-element box pre-test roughly halves that) is a documented perf characterization with a measured 1.094x wall-clock effect on five pairs, not a correctness bug.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — `proximity` is unsigned by design; `interior(r)` supplies the inside half for solid families only (documented scope, not a defect in itself — the mesh-sheet gap this leaves is tracked separately as DL-31).
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the query's read of other objects' live transforms joins the pre-existing per-sample `EvaluateAtTime` race under motion blur, "no wider than `Object::IntersectRay` already does" per the doc's own comparison; not a new or worse hazard.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the memo-eligibility threshold moving with `kFields` (29 -> 35) is bit-identical and perf-only, disclosed as such.
- CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — the agent-facing draft-preview descriptor strings claiming quality:"draft" ignores materials/lighting entirely were wrong (draft actually evaluates the material's full BSDF under a fixed studio rig, only real scene LIGHTING is skipped) — already corrected in the same session in `AgentMcpAdapter.cpp`/`AgentChatCodecs.cpp`/`AgentSession.{h,cpp}`/`AgentRpc.h`; a documentation-accuracy fix already landed, not an open code debt.
- SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 — "No weight formula changes": MIS heuristics per integrator stay exactly as documented in MIS_HEURISTICS.md; a stated non-goal, not a debt.
- SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 / CROSS_OBJECT_PROXIMITY_DESIGN.md §10 — GUI painter preview, realize-time displacement, and `HairGenerator` build their own hit records and read signals as neutral; doc's own words: "not transport; unchanged and already disclosed." These are non-transport, non-comparison paths (a single-sample preview, a one-shot bake, hair-strand generation), not part of the PT/BDPT/VCM consistency contract that DL-14/DL-19/DL-22/DL-30/DL-36 track.
- ~~DL-73~~ (DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md: volume-guiding bsdfPdf composition) — STRUCK 2026-09-13, ruled consistent by derivation (P2-C, debt-sssenv round-2): the row proposed replacing `RayCaster.cpp`'s `rs2.bsdfPdf = phasePdf` (raw, un-combined) with the guided-mixture `combinedPdf`, on the theory that it should match the main surface continuation's `effectiveBsdfPdf` convention. Derivation shows this is backwards: env-NEE at a volume vertex weights via `MediumScatterMaterial::Pdf` (`MediumTransport.cpp`'s `EvaluateInScattering` -> `LightSampler::EvaluateDirectLighting`'s env arm, `LightSampler.cpp` ~:2652), which returns the RAW, un-guided `m_pPhase->Pdf(...)` — the SAME raw `phasePdf` the escape side already uses. Both sides feed `PowerHeuristic` the identical `(phasePdf, envPdf)` pair (opposite argument order), which is `PowerHeuristic(a,b) + PowerHeuristic(b,a) == 1` by construction — UNBIASED as written. `guidingMISWeight = phasePdf / combinedPdf` (folded into `rs2.importance`) already applies the full guiding correction to the sample's contribution; substituting `combinedPdf` into the MIS weight too, as this row prescribed, would double-apply that correction and BREAK the partition. Not a debt; the real, opposite-signed asymmetry is on the surface path, filed separately as DL-74. See [DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md](DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md) "Residual: volume-guiding bsdfPdf composition — DL-73 RULED NOT A DEBT".

## Counts

**Authoritative totals on `master` (recount after each merge; per-slice notes
below are each slice's own snapshot at its branch HEAD and do NOT sum):**
2026-09-13 after the ggx + guiding + sssenv merges — **71 main rows: 48 open, 23 closed**
(16 closed by the 2026-09-12 cleanup; DL-42/DL-43 guiding; DL-62/DL-64 ggx;
DL-52/DL-53/DL-71 sssenv). DL-73 is recorded as not-a-debt. Ids in use:
DL-01..DL-75 minus DL-35, DL-68, DL-70 (reserved for the translucent slice, not yet merged).

**2026-09-14 (debt-guiding2 slice, branched from `ddf05c6c`, the 71/48/23
baseline directly above — verified by an independent recount of the table:
71 non-divider rows before this slice's edits, matching the baseline
exactly).  Figures below are this slice's state AFTER review round 2,
which supersedes the round-1 wording that used to stand here:**
DL-66 CLOSED (test-only fixture repair, red-proved on this exact HEAD:
868/1 before, 884/0 after).  DL-72 CLOSED — round 3 wired the
moment/count pairing correctly, round 4 corrected its red-proof claim
(the row had cited a hand-driven accumulator test that executes no line
of either production file; the real red-proof is the new
`OptimalMISTrainingSitesTest`, red at clampMin/clampMax with the two
files reverted to `ddf05c6c`, 7/0 wired) and fixed P2-5, the volume
moment being divided by the raw phase pdf rather than the density the
direction was drawn from.  DL-74 CLOSED **for every guiding mode**, not
just the default one — round 1's narrower repair (an env-arm-only,
one-sample-only blend on the base alpha) did not close the partition and
had no test that could have seen that; round 2 replaced it with the
two-roles split (`RAY_STATE::bsdfPdf` = true sampling density,
`RAY_STATE::bsdfMisPdf` = one lobe-independent nominal MIS-partner
density evaluated identically on every side) and red-proved it with the
new `PTGuidingMISPartitionTest` (4 of 6 rows red on `8fcce0bf` by −66 %
to +63 %; 22/0 after).  **DL-83** was filed in round 1 for the RIS-mode
residual and is CLOSED in round 2 by that same repair: the nominal
density does not depend on the guiding mode, so the premise DL-83 was
filed on (that the NEE side must reproduce RIS's sampling density) was
itself wrong.  Main rows: 71 -> **72** (DL-83 added, then closed).
OPEN-confirmed: 48 -> **45** (DL-66, DL-72, DL-74 closed; DL-83 opened
and closed within the slice).  CLOSED-by-later-slice (this slice): **4**
(DL-66, DL-72, DL-74, DL-83).  Independently re-verified by direct
recount of the table this pass: 72 non-divider `| DL-` / `| ~~DL-` rows,
45 containing "OPEN-confirmed", 27 starting `| ~~DL-` (struck/closed) —
45 + 27 = 72, consistent.

**Review round 3 (same slice, same day) closed no row; it opened DL-84 (see
below, main rows 72 -> 73).**  Its two other findings were defects IN this slice's own
round-2 work on rows that were already struck, so they were fixed under
those rows rather than filed.  (1) A REGRESSION round 2 introduced — the
`bsdfPdf`/`bsdfMisPdf` split was not carried across the shader-op
boundary, so `PathTracingShaderOp` and `EmissionShaderOp` weighted with
the guided density against an NEE arm that used the raw one: **+44.1 %**
on a global medium + trained volume guiding + an area emitter.  (2) A
residual round 2 had dismissed in writing as "pre-existing and
orthogonal" — the NEE arms evaluating the MIS-partner aggregate pdf
under an `IORStack(1.0)` sentinel while the escape side uses the live
stack: **−14.0 %** on an albedo-1 furnace inside a dielectric, with
guiding OFF as much as ON.  Both are red-proved in
`PTGuidingMISPartitionTest`, now **37/0** (34/3 on the round-2 HEAD
`2ebcaff9`).  Round 3 DID open one NEW row: **DL-84**, the third
volume continuation (`PathTracingIntegrator`'s own in-loop one, reached
only after a surface bounce), whose optimal-MIS training DL-72 never
covered.  Its MIS-PARTNER half was a live bias and is fixed under DL-74
(the stale partner cost **+9.6 %** on VolumeEnvFurnaceTest's
floor-in-fog furnace, 29/0 before the slice -> 26/3 after round 2 ->
29/0 again now); the TRAINING half is left conservative (numerator
cleared to zero rather than carrying the previous vertex's) and filed.
Recount after round 3: main rows 72 -> **73**, OPEN-confirmed 45 ->
**46**, struck 27 — 46 + 27 = 73, consistent.

**Review round 4 (same slice, same day) changed no count either.**  It
found no P1.  Its one live defect was again in this slice's own work on
an already-struck row — rounds 2-3 treated a zero material AGGREGATE pdf
as proof that the BSDF technique cannot reach a direction, which stops
being true the moment a GUIDE is mixed in, so both MIS halves took weight
1 over that region: **+100.4 %** with an area emitter placed entirely
inside a partial-support lobe's blind spot, **+10.6 %** (converging on a
closed-form 14.6 %) on a whole-hemisphere env furnace.  Fixed under
DL-74; `PTGuidingMISPartitionTest` **63/0** (61/2 before).  A second
finding corrected the trained numerator at both halves of the BSSRDF
optimal-MIS pair, fixed under DL-72 (`OptimalMISTrainingSitesTest`
**23/0**, was 21/2 — both pre-fix ratios EXACTLY 1 where the derivation
says 4).  Round 4 opened NO row.  In particular it did **not** file the
`VolumeAbsorptionAttenuationTest` flake it was asked to consider: 40
measured renders of the failing row show it unbiased (mean within 0.6 %
of the closed form) with a 3.13 % run-to-run sd against an 8 % gate —
a 2.6-sigma test failing ~1 % of runs, i.e. a fixture that is
under-averaged for its own tolerance, not an estimator or transport
defect.  Row [R] is now repeat-averaged over 4 independently seeded
renders (sd 2.09 %, worst of 36 loaded runs +4.83 %).  Recount after
round 4: main rows **73**, OPEN-confirmed **46**, struck 27 —
46 + 27 = 73, unchanged.

**2026-09-13 (debt-guiding slice):** DL-43 closed (`a69c9ce6`,
`TranslucentIORStackTest: ALL TESTS PASSED`) and DL-42 closed
(`PTGuidedSelectProbTest: ALL TESTS PASSED`) — see the table rows and
their "Verification recipes" entries above. One row opened while
verifying both, originally filed in this slice's own ledger as DL-65 —
**RENUMBERED DL-67 before merge** (this slice's local id collided with
rows concurrently filed by sibling debt-cleanup slices working the same
merge base: DL-65 is the ggx slice's row, DL-66 the sssenv slice's, DL-68
the translucent slice's; every in-file cross-reference to the original
DL-65 has been updated to DL-67). DL-67 (PT/BDPT guided one-sample and RIS
proposal densities are inconsistent across lobe-selection branches, found
in both PT and BDPT's RIS candidate-0 slot while re-deriving the correct
estimator for DL-42/DL-43; not fixed here) supersedes and corrects an
earlier draft of this note and of the DL-42 row/DL02 doc, which wrongly
described DL-42's two UNCHANGED `PTMulDiv` sites as "already complete,
self-contained estimators" merely because they use the material's
aggregate BSDF/PDF — aggregate re-evaluation does not, by itself, make a
branch immune to `selectProb`; see the corrected DL-42 row and DL-67 for
the derivation. A second, independent row was also found and filed this
pass, **DL-69** (BDPT's ordinary, non-guiding eye/light throughput pairs
an aggregate BSDF value with a per-lobe selection pdf — a different
lobe-selection defect than DL-67's guiding-specific one, discovered while
correcting an over-broad "BDPT does not share this pattern" claim in the
DL-42 row). Main rows: 63 -> 64 (DL-67, née DL-65, added) -> **65** (DL-69
added). OPEN-confirmed: 47 -> 45 (DL-42 and DL-43 closed) -> 46 (DL-67
opened) -> **47** (DL-69 opened). A new "CLOSED-by-later-slice" bucket of
**2** is added below (distinct from the 2026-09-12 cleanup session's 16,
since this slice ran independently afterward against `75f78ba5`).

Updated for the 2026-09-12 DL-34 recorded-overlap closure and independent
DL-60/DL-61 gate residuals, plus the DL-37 diffuse-composition closure and independent DL-62/DL-63/DL-64 residuals. Original sweep counts remain historical.

**Re-sweep** (this pass): debt-resweep worktree, HEAD `876c9a26`, dated
2026-09-12 — an independent verifier re-opened the code (not the doc text)
for all 47 OPEN rows against this HEAD, cross-checked against
`git diff --stat 6486656e..HEAD` (the cleanup session's touched-file list:
`GGXBRDF.{cpp,h}`, `GGXSPF.cpp`, `SubSurfaceScatteringBSDF.cpp`,
`SubSurfaceScatteringSPF.{cpp,h}`, `TranslucentSPF.cpp`, `CSGObject.{cpp,h}`,
`ChunkParserRegistry.cpp`, `BDPTIntegrator.cpp`, `BSSRDFEntryAdapters.h`,
`PathTracingIntegrator.cpp`, `BSSRDFSampling.{cpp,h}`, `ManifoldSolver.cpp`,
`Optics.{cpp,h}`, `PathTransportUtilities.h`, `RandomWalkSSS.{cpp,h}`,
`IJob.h`, `IMaterial.h`, `FibreLobeMath.h`, plus `tests/*`), to find any row
that had actually closed as a side effect of the 16 cleanup fixes without
the ledger being updated. **Verdict: none.** All 47 OPEN rows were directly
re-verified against source at `876c9a26` (symbol/line still present, defect
mechanism still reachable) — none of the 16 cleanup commits touch the
mechanism any OPEN row names, including the ones sharing a source file with
a closed row (DL-38/41/42/43/44/45/46/47/49/52/53 vs the TranslucentSPF/SSS/
BDPT-guiding/RayCaster fixes; DL-21/30/31 vs the CSGObject union-depth fix;
DL-62/63/64 vs the GGX diffuse-transmission fix). One stale citation was
found and fixed: DL-19's evidence claimed `tests/SignalEmitterRecordTest.cpp`
was "unchanged in this tree" — `ac9891f3` (DL-36) had in fact added
`RunBoundedNeighbourRead()` to that file, but it adds no coverage for the
two S3 sites DL-19 names, so the verdict is unchanged and only the citation
text was corrected. All 16 CLOSED-by-cleanup rows were confirmed struck in
both the ledger and their named source-doc heading (§10.1/§10.3 of
REFRACTIVE_RADIANCE_SCALING.md, debt 31 items 1/3/4 of RENDERING_INTEGRATORS.md,
§10 of CROSS_OBJECT_PROXIMITY_DESIGN.md, §10 of
SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md, IMPROVEMENTS.md's GGX low-F0
entry — the other nine, DL-48/50/51/54/55/56/57/58/59, were discovered
mid-sweep rather than sourced from one of the eight original design-doc
ledgers, so they have no separate design-doc heading to strike and are
correctly cited only to their own `DLxx_*.md` closure doc). Ledger ordering
(class, then S<M<L within class), the OPEN/CLOSED/RESOLVED/NOT-A-DEBT/DOC-ROT
partition (every DL-xx id in exactly one section, no duplicates, DL-35
deliberately absent), and every internal `DL-xx` cross-reference were
independently re-derived from the table text this pass and found consistent
— no count below changed AS OF that re-sweep (`876c9a26`, 2026-09-12); the
debt-sssenv slice (2026-09-13, below) made five further changes on top of
it, and a round-2 pass the same day made four more (see below) — the
"Main rows"/"OPEN-confirmed"/"CLOSED-by-cleanup" figures that follow are
the CURRENT totals, not the re-sweep's own.

- Main rows: ~~63~~ ~~64~~ **65** as of 2026-09-13 (DL-67, née DL-65,
  added by the debt-guiding slice; DL-69 also added by the same slice) —
  ~~47 open, 16 closed~~ ~~46 open, 18 closed~~ **47 open, 18 closed**
  (DL-42 and DL-43 closed, DL-67 and DL-69 opened; see the dated note
  above).
- OPEN-confirmed: ~~47~~ ~~46~~ **47** as of 2026-09-13 (DL-42 and DL-43
  closed, DL-67 opened this pass, DL-69 opened this pass), including the
  two reproduced baseline gate residuals DL-60/DL-61. DL-35 remains
  deliberately absent. Re-sweep 2026-09-12: all 47 independently
  re-verified against `876c9a26`; 0 reclassified.
- CLOSED-by-later-slice: **2** (DL-42, `PTGuidedSelectProbTest`; DL-43,
  `a69c9ce6`; both 2026-09-13, debt-guiding slice — run independently
  after the 2026-09-12 cleanup/re-sweep sessions below, against
  `75f78ba5`).
- CLOSED-by-cleanup: **16** (DL-01, `1239edf2`; DL-02, `a041e51d`;
- Main rows: **68** — **49 open**, **19 closed**.
- OPEN-confirmed: **49**, including the two reproduced baseline gate residuals
  DL-60/DL-61. DL-35 remains deliberately absent. Re-sweep 2026-09-12: all
  47 independently re-verified against `876c9a26`; 0 reclassified. Since then
  (debt-sssenv slice, 2026-09-13, round 1): DL-52 and DL-53 closed, bringing OPEN to
  45; DL-66 added — a coverage/test-gap found incidentally while gating this
  slice, unrelated to DL-52/DL-53 — bringing OPEN back to 46; DL-71 and
  DL-72 opened AND closed same-session (found while gating DL-52/DL-53's own
  closure, see below), leaving OPEN at 46; DL-73 opened and left OPEN (a
  static-evidence-only ruling on volume-guiding bsdfPdf composition, found
  gating DL-72's own closure), bringing OPEN to 47. **Round 2 (same day,
  later slice)**: independent review found round 1's DL-71 fix and DL-72
  fix each had their own defects, and DL-73's ruling was backwards. DL-71's
  P1 defect was fixed in place (row stays CLOSED, see its doc's "Round-2
  correction"). DL-72 was REOPENED (fix reverted to an unwired/conservative
  state pending a correct implementation) — CLOSED 46→45(closed), OPEN
  46→47(open) at that step. DL-73 was STRUCK as ruled-consistent
  (not-a-debt) — OPEN 47→46. DL-74 (the real surface-path partition
  violation DL-73's derivation surfaced) and DL-75 (SSS-on-hair entry
  normal undefined, found auditing DL-71's sibling geometry types) were
  filed OPEN.
  **Precise round-2 arithmetic** (starting from round-1's ending OPEN=47,
  CLOSED=20, main rows=67): DL-72 reopened (closed 20→19, open 47→48);
  DL-73 struck to not-a-debt (open 48→47, main rows 67→66); DL-74 filed
  (open 47→48, main rows 66→67); DL-75 filed (open 48→49, main rows
  67→68). **Ending totals: main rows 68, open 49, closed 19.**
- CLOSED-by-cleanup: **19** (DL-01, `1239edf2`; DL-02, `a041e51d`;
  DL-36, `ac9891f3`, consistency pin; DL-03, `8a9bdb18`;
  DL-04, `1b705ce1`, convention pin; DL-48, `12a7ef3e`;
  DL-50, `29ce61c7`; DL-51, `34434610`; DL-54, `ab85092d`;
  DL-55, `cba4e88c`; DL-56, `df7e3dad`; DL-57, `c187f2cc`;
  DL-58, `1b1909c0` / `ed8d9c94`; DL-59, `a0c4a808`;
  DL-34, `cffa254f`, published overlap regression with conservative bounds retained;
  DL-37, `000df0b4`, diffuse composition with DL-63 specular failures retained;
  DL-52, `00cd6723`, planar probe-origin single-chord fix;
  DL-53, `b3de184d`, RayCaster explicit-map MIS-partner fix;
  DL-71, `e416d3bd`, probe entry-normal orientation fix (found gating DL-52's
  own closure; round-2 correction same day replaced the near-half-only
  gate with an unconditional rule, row stays CLOSED)).
  DL-72 was closed by this same round-1 fix (`0c9eccc4`) but was
  REOPENED round-2 (P2-B) when its own wiring was found unsound — it is
  no longer in this CLOSED-by-cleanup list; see the OPEN table.
- CLOSED-by-sweep (heading was open/unlabeled; a sweep found it actually
  fixed and struck it): **7** (DR-01 .. DR-07, unchanged this pass)
- Already RESOLVED in source, independently re-verified: **23** (DL-R1 ..
  DL-R17 from the original sweep and the gap-closure pass; DL-R18..DL-R23
  added this pass, covering WETNESS_COAT_DESIGN.md §12 items 3, 5, 6, 6a,
  11 and 12 — one of these, DL-R20, is resolved-in-code but flagged as
  tested only indirectly rather than by a dedicated regression)
- NOT-A-DEBT: **28** (27 unchanged from the prior pass + DL-73, struck this pass as ruled-consistent-by-derivation)
- UNVERIFIABLE: **0**
- CLOSED-by-this-resweep: **0** (see "Re-sweep" note above; one stale
  citation on DL-19 corrected in place, verdict unchanged)

Total items re-verified or newly placed by the final completion pass: 1 new
OPEN row (DL-37) + 6 new Already-resolved rows (DL-R18..DL-R23) + 1 second
source pointer added (DL-23) + 1 recipe rewritten with a named scene,
protocol, metric and a stated-proposed threshold (DL-27) = **8 items
classified or substantively rewritten**, this pass.

**debt-ggx slice (this pass, 2026-09-13, HEAD `662b976c`; fix `dfdd5ee1`, mode-gated for cost in `a495a357`):** closed DL-62
and DL-64 (`GGXSampleEvaluationConsistencyTest: 48 checks, 0 failures` (29 at `dfdd5ee1`),
red on `a1db468d`: 23 failures); re-verified DL-63 unchanged (bit-for-bit
identical `150 checks, 3 failures` against a stashed pre-fix rebuild) and
recorded its exact per-config numbers in the ledger row for the first
time. Opened DL-65 (M, physics-bias): a sibling-audit pass (per
docs/skills/audit-by-bug-pattern.md) confirmed the DL-62 pattern (SPF
widens roughness by `ri.glossyFilterWidth`; the paired BRDF's `value`/
`valueNM` does not) is structurally present, unfixed, in both
`CookTorranceSPF`/`CookTorranceBRDF` and `SchlickSPF`/`SchlickBRDF` — a
different material family with its own roughness/Fresnel parameterization,
out of this slice's GGX scope, so it was recorded rather than fixed here.
The DL-64 pattern (raw-F0 lobe-selection weight losing grazing energy at
F0=0) was checked against the same two classes and does **not** clearly
replicate: `CookTorranceSPF` has no Schlick-F0 branch (its specular tint
IS the desired multiplier, so tint=0 legitimately means zero specular,
not lost grazing energy), and `SchlickSPF::Scatter`/`ScatterNM` sample
their Schlick half-vector lobe unconditionally rather than through a
raw-F0-weighted lobe-selection probability — no separate DL-65-adjacent
row was opened for DL-64's pattern.

- Main rows after this slice: **64** — **46 open**, **18 closed**.
- CLOSED-by-cleanup: **18** (adds DL-62 `dfdd5ee1` and DL-64 `dfdd5ee1`
  to the 16 listed above).
- New OPEN row this slice: **DL-65** (CookTorrance/Schlick glossy-filter
  sample/evaluation mismatch, M, physics-bias) — see the Table and its
  Verification recipe below.

## Verification recipes

Recipes are retained after closure as regression specifications. The main
table and Counts section above define current status; a retained recipe
does not reopen a closed item.

Concrete "how to know it's fixed" for each DL-xx row above.

**~~DL-01 (TranslucentSPF RGB/NM exit weight divergence).~~ CLOSED
2026-09-12 — `1239edf2`, `TranslucentSpectralParityTest: 676 checks, 0 failures`.**
The test committed as `fc371041` failed with 174 failures against the
unfixed library from `6486656e`. Both pipes now split Beer extinction
between exit and backscatter, with primary-layer tau paid on entry only.
It covers 450/550/650 nm, uniform/per-channel N, tau and scattering
endpoints, and chained spectral entry/exit. The absolute weight tolerance
is explicitly `kWeightTolerance = 1e-3`; the old recipe's claim that
`TranslucentIORStackTest` already had a `CROSS_VAL_TOL` was incorrect.
The scene parameter is `tau`, not the old recipe's `transmittance`.
Diffuse exit-direction/Pdf consistency was subsequently closed by DL-02;
full mixture density remains DL-41 and integrator/HWSS amplitudes DL-38. See [closure and audit](DL01_TRANSLUCENT_EXIT_WEIGHT.md).

**~~DL-02 (TranslucentSPF exit-density support and spectral shape).~~ CLOSED
2026-09-12 — `a041e51d`, `TranslucentSpectralParityTest: 1918 checks, 0 failures`.**
Test `adc9a286` failed 324 checks against the unfixed `cb9dd0f4` library.
RGB/NM exit support, stored/evaluated cosine density, fixed-variate inverse
CDF and separate hemisphere integrals now agree at N=1/8 and per-channel
N=5/10/15. Entry geometric-horizon controls still pass. This closes the
conditional diffuse exit only; full mixture/reverse densities and NEE
state are DL-41, BSDF amplitudes are DL-38, and guiding stack propagation
was subsequently closed by DL-03. See [closure and audit](DL02_TRANSLUCENT_EXIT_DENSITY.md).

**~~DL-03 (TranslucentSPF guided-direction IOR-stack leak).~~ CLOSED
2026-09-12 — `8a9bdb18`, `TranslucentIORStackTest: ALL TESTS PASSED`.**
The trained real-PT regression was red on unfixed `00bdcef5` with four
outward-stack failures. Explicit guide-eligible diffuse arrival avoids
ordinary PT translucent specular-arrival suppression; positive actual
outward substitution counts are required in both guiding modes and both
RGB/NM. The later same-object Scatter verifies entry classification and
popped membership, while inward substitutions retain inside state. Added
BDPT eye/light RGB/NM real-entry/exit coverage passes too; eye RIS retains
SPF directions because DL-43 remains open, so it is not claimed as actual
outward-guide coverage. See [closure and audit](DL03_GUIDED_IOR_CONTINUATION.md).

**~~DL-04 (SSS family eta^2 direction).~~ CLOSED 2026-09-12 —
consistency pin, `1b705ce1`.** The original ratio-of-ratios alone cannot
identify a constant multiplying every SSS event, and the proposed planar
fixture exposes a separate missing-support defect. The revised test retains
the matched explicit-volume/diffusion/RW camera matrix, anchors each raw
observer ratio to 1.33 squared, and adds independent helper and absolute
conservative-furnace bounds. Both deliberately added eta-square directions
fail six SSS air-channel checks; a missing observer transform fails nine.
The unchanged implementation passes. This closes the convention question,
not exact material equality within MC noise: fixed pixel Sobol scrambles
are not independent repetitions. Of the DL-48 through DL-53 range this
sentence originally pointed at, only **DL-49** (non-air relative-index)
remains open — DL-48 (normalization), DL-50/51 (survival/support), and
DL-52/53 (probe reach, recursive-MIS) are all CLOSED; see
[the complete decision record](DL04_SSS_RADIANCE_DECISION.md).

**DL-05 (PT two-layer weave gap, debt 27).** Extend
`tests/BDPTStrategyBalanceTest.cpp` with a topology pairing two
`weave_material transmission thin` layers with a light OUTSIDE both (the
doc's own box/plane fixtures at gap 0.1/0.3). Expected today: PT/BDPT ~=
0.65-0.78 (i.e. BDPT/PT 1.28-1.55x). Fixed when a `HasNonBendingDeltaLobe()`
material query lets `CastShadowRayTransmittance` (or an equivalent walk)
see through the far layer's delta gap for PT's NEE, closing the row to the
suite's standard 8% band, with an added-energy suppression against PT's
own BSDF-sampled delta continuation for AREA lights (to avoid a new
double-count).

**DL-06 (VCM env+mesh SA-MIS partition).** Re-run `EnvLightBalanceTest` at
its strict tolerances (`{0.10, 0.30, 1.00}`); currently 78/80. Fixed when
the monolithic SA-MIS migration (both subpath sides, the shared generator's
env-vertex pdf convention, the 6 BDPT rows, OpenPGL) lands and this reaches
80/80 strict without regressing hwss=true below ~0.82 (its pre-existing,
separately-tracked bias floor).

**DL-07 (OrenNayarBRDF hemisphericalAlbedo bias).** Add a
`tests/OrenNayarHemisphericalAlbedoTest.cpp` comparing
`OrenNayarBRDF::hemisphericalAlbedo()`'s closed form against a brute-force
hemispherical quadrature at roughness in {0, 0.3, 0.5, 1.0}. Expected today:
0% / ~5% / ~12.6% / ~25.6% over. Fixed when a corrected estimator (or a
small baked correction table) brings all four under a stated tolerance
(e.g. 2%), then re-measure `fabric_material`/`coated_material`'s furnace
configs that depend on it (cloth furnace config using Oren-Nayar substrate;
`WETNESS_COAT_DESIGN.md`'s coated-Oren-Nayar furnace row).

**DL-08 (PSSMLT stream aliasing).** Add a case to
`tests/PSSMLTStreamAliasingTest.cpp` that drives a volume-heavy scene to eye
depth >= 32 under `mlt_spectral_rasterizer` (or the RGB MLT rasterizer) and
asserts that stream indices used by the eye walk never collide with
stream 48's film/lens/aperture lanes (e.g. by instrumenting
`PSSMLTSampler::StartStream` to log/assert on `streamIndex >= kNumStreams`
distinctness, or by comparing rendered means with `kNumStreams` raised well
past 32 vs the shipped 49). Fixed when either `kNumStreams` is raised past
any reachable depth or the eye walk's stream assignment is reworked to stay
bounded, and the new test passes where it previously would have aliased.

**DL-09 (RadianceEtaScale spatially-varying ior).** Author a scene with a
graded-index object (`ior` bound to a spatially-varying `scalar_painter`)
and an NEE connection or bounce gathered at an interior vertex. Compare
against a reference computed by an independent means (e.g. a fine-grained
multi-shell approximation with piecewise-uniform `ior`). This is presently
unbuilt — the recipe is to build `tests/RadianceEtaScaleGradedIndexTest.cpp`
first, establish whether the mismatch is measurable, then decide whether
`RadianceEtaScale` needs to read the SPF's freshly-fetched value instead of
the stack's push/pop-time one.

**DL-10 (fisheye pixelAR Jacobian).** Author a fisheye-camera scene with
`pixelAR != 1` and a uniform-radiance sphere filling the frame (the same
fixture `tests/CameraImportanceTest.cpp::TestFisheyeFilmResponse` uses at
`pixelAR = 1`, where it integrates to 1.000166). Expected: at `pixelAR !=
1` the closed-form integral departs from 1 by more than measurement noise.
Fixed when `Importance`'s per-pixel solid-angle term folds in the full
Jacobian of `normalize ∘ Stretch(pixelAR,1,1)` and the extended test
converges to 1 at several `pixelAR` values.

**DL-11 (sheen middle-band residual).** Re-run
`tests/SheenDirectionalAlbedoTest.cpp`'s domination-slack check at the
current 64x64 table; currently the middle band (n·v in [kMinSheenAlpha,
0.0349)) reads +0.75% over 1. Fixed when a root-caused correction near the
roughness floor (α ≈ 0.065) brings that band under the same ~0.2% bar the
other two bands already clear, without another blind resolution doubling.

**DL-12 (mirrored UV weave direction).** Add a mirrored-UV mesh fixture to
`tests/WeaveMaterialChunkTest.cpp` (or a render regression under
`scenes/Tests/`) and compare the weave direction rendered across the seam
against an authored `TANGENT`-accessor re-export. Fixed when a
`bitangentSign` companion on `vShadingTangent` recovers the chirality
`vTangent` already has (`RayIntersectionGeometric.h:469-470` vs `:516`)
and the mirrored half no longer flips weave direction.

**DL-13 (displaced-geometry curvature ambiguity).** Add a
`tests/GeometryCurvatureConventionTest.cpp` case comparing `curv` on a
`displaced_geometry` at `smoothing=0` vs `smoothing=1` against the two
closed-form curvatures (base sphere/plane vs the displaced analytic
surface, using `ComputeAnalyticalDerivatives`). This is a documentation
decision as much as a code one: fixed when the descriptor states which
curvature `smoothing` selects and a test pins both numerically.

**DL-14 (ray differentials 0 under BDPT/VCM/MLT).** The `txFootprint` field
on `BDPTVertex` (`BDPTVertex.h:162`) already exists and is carried through
the struct — this is a plumbing gap, not a missing-field gap. Extend
`tests/BDPTVertexRIGRebuildTest.cpp` (or a new
`tests/BDPTRayDifferentialsTest.cpp`) asserting `txFootprint` is populated
(not all-zero) on a `BDPTVertex` in a scene using footprint-driven texture
filtering. Fixed when ray differentials are computed and threaded through
the BDPT/VCM/MLT eye-subpath walk (mirroring the S1 `derivatives`/`signals`
widening already done for the same struct) and the PT-vs-BDPT filtered-band
difference shrinks to noise on a control scene.

**DL-15 (eccentric ellipsoid proximity bound).** Extend
`tests/CrossObjectProximityTest.cpp` with a 4:1 eccentric ellipsoid and a
probe point at true distance 2.75; assert the reported distance against a
tighter, non-uniform bound (or at minimum that the current ~11.0 reading is
logged/warned per-object as the doc's "quantifies per object" note
promises). Fixed when Phase 3's exact-σ work is extended to report a
tighter bound along more than the single top singular vector, or an
explicit per-object inflation warning is emitted at scene-load time.

**DL-16 (ggx_material tangent_rotation scalar-pipe alias).** Add
`tangent_rotation_scalar` (or an equivalent accept-either-pipe resolution
order) to `ggx_material`'s descriptor in `ChunkParserRegistry.cpp`,
preferring Scalar and keeping the Color binding working-but-deprecated.
Fixed when a `tests/ChunkParserRegistryTest.cpp`-style case binds a
`scalar_painter` to the new slot and a `fabric_material` wrapping the same
GGX base can drive both rotations from one shared painter, matching the
already-shipped `weave_rotation`/`tangent_rotation` additive convention.

**DL-17 (glTF anisotropy_rotation).** Implement the sketch in
`CLOTH_FABRIC_DESIGN.md` §15 item 12 (two `channel_painter`s + an `atan2`
`scalar_painter` bound to `weave_rotation`) in `GLTFSceneImporter.cpp`.
Fixed when a glTF fixture with a per-texel anisotropy rotation texture
imports and renders a spatially-varying anisotropic highlight, verified in
`tests/GLTFClearcoatImportTest.cpp` or a new anisotropy-rotation sibling.

**DL-18 (Blender bridge sheen mapping).** Add Principled BSDF Sheen socket
import to the Blender bridge, targeting `fabric_material`. Fixed when a
round-trip test (Blender fixture -> RISE scene -> render) shows a non-zero
sheen contribution matching the authored Sheen weight/tint within the
bridge's existing tolerance conventions.

**DL-19 (S3 sites without a red-proof).** Build a light-tracing-dominated
spectral scene (a caustic through glass, or an emitter reachable only by
t=1 splats) and add it to `tests/SignalEmitterRecordTest.cpp`. Fixed when
that scene's BDPT NM-hero `Le` rebuild and HWSS `rigW` rebuild sites move
the suite's numbers by more than a few percent when their signal-replay is
disabled — i.e., a real red-proof exists where none did.

**DL-20 (patch geometry flat curvature).** This is a Phase-4 scope item, not
a bug in the current `valid=false` contract. Fixed when Phase 4 adds a
`curv` implementation for patch geometries and a
`tests/PatchCurvatureTest.cpp` compares it against an analytic patch
curvature at a few parametric points.

**DL-21 (CSG boundary curvature).** Add a case to
`tests/CsgSurfacePayloadTest.cpp` at a CSG boundary edge asserting a chosen
convention (forward the contributing surface's curvature, most likely) and
pin the number. Fixed when that assertion exists and passes.

**DL-22 (BSSRDF entry vertex neutral signals).** Add a probe record to
`BSSRDFSampling::SampleResult` carrying `derivatives`/`signals` at the
entry hit, and extend `tests/SignalIntegratorConsistencyTest.cpp` with an
SSS-entry scene keyed on a geometry signal (e.g. `curv`-driven IOR). Fixed
when that scene's PT and BDPT/VCM renders agree with the closed-form
control the way the other five signal kinds already do post-S1/S3.

**DL-23 (coated_material substrate allowlist excludes fabric).** Add
`WeaveMaterial`/`FabricMaterial` (post-DL-R8 forwarding) to
`CoatedMaterial::IsSupportedSubstrate`'s `dynamic_cast` chain and
`SubstrateAllowlistText()`, then extend `CoatedBRDF`/`CoatedSPF`'s
opposite-hemisphere early-outs the same way DL-R8 extended `FabricBRDF`/
`FabricSPF`. Fixed when a `coated_material` over a `transmission thin`
`weave_material` (a waxed sheer curtain) transmits attenuated light rather
than reading opaque, guarded by a `CoatedMaterialChunkTest` case mirroring
`TestTransmissiveSubstrateForwarding`.

**DL-24 (CompositeSPF 96% coat-over-diffuse energy loss).** This is
`coated_material`'s whole reason for existing (WETNESS_COAT_DESIGN.md's own
routing-around decision), so the recipe is architectural, not a small
patch: replace `CompositeSPF`'s 50/50 `Pdf` and top-wins `GetBSDF` random
walk with a closed-form or properly-weighted mixture, guarded by
`tests/LayeredWhiteFurnaceTest.cpp` configs 3 and 7 (the doc's own named
failing configs) reading near 1.0 rather than losing 96%.

**DL-25 (no painter-sampling builtin in the expression VM).** Add a
painter-sample builtin (e.g. `sample(painter_name)` or an implicit
per-channel accessor) to `ExpressionEval.h`'s function table, gated the
same way `occlusion()`/`proximity()` are. Fixed when a
`tests/TextureExpressionVMTest.cpp` case darkens a textured substrate
through `add_wetness`'s recipe and a render regression shows the texture
detail surviving under the wet recipe, closing WETNESS_COAT_DESIGN.md §6.4
clause 2's refusal.

**DL-26 (add_wetness / add_wear mutual exclusion).** Design the composable
representation (WETNESS_COAT_DESIGN.md doesn't sketch one) and extend
`AgentSession.cpp`'s qualifying predicates so both verbs can commit to the
same material. Fixed when `tests/AgentAddWetnessTest.cpp` and its `add_wear`
counterpart both gain a case that applies one verb after the other on the
same target and asserts non-refusal plus both effects visible in the
emitted CST.

**DL-27 (wet-highlight variance unmeasured).** WETNESS_COAT_DESIGN.md §11.1
states no pass threshold of its own — its exact words are: "Unmeasured. It
should be measured on the worked example, with `oidn_denoise FALSE` (§9),
before the recipe's default `scattering` ceiling is fixed." So the recipe
below both names the measurement and **proposes** the threshold the doc
omits. Scene: `scenes/FeatureBased/Materials/rainwet_cobbles.RISEscene`
(§6.5's worked example, the same one the Phase-1 exit gate renders at mean
luma 0.195). Protocol: `docs/skills/variance-measurement.md`'s K-trial
procedure (K >= 16, `samples 32`, `oidn_denoise FALSE`, `adaptive_max_samples
0`, EXR output) run twice — once on the scene as authored (`add_wetness`
applied, `scattering` at its shipped ceiling) and once on a "dry" variant
with the wetness recipe's coat lobe stripped back to the bare substrate
(`coat_weight 0`, or the pre-verb material) — both otherwise identical
(camera, lights, sample count, seed sequence). Metric: `σ²·T`, the
wall-clock-normalized variance CLAUDE.md's integrator-selection work already
uses for exactly this kind of cost comparison — `σ²` from
`bin/tools/HDRVarianceTest.exe` on each K-trial set, `T` the measured
per-trial wall time, reported as the ratio (wet `σ²·T`) / (dry `σ²·T`).
Pass threshold (**proposed, not stated in the design doc**): ratio <= 1.5x
(a near-delta coat lobe may legitimately cost more per unit variance
reduction than a matte Lambertian one, but a cost more than 50% higher
should trip a review of the `scattering` ceiling, not ship silently).
Fixed when that number exists and, if it exceeds the proposed (or a
user-ratified) threshold, the recipe's `scattering` ceiling is lowered with
the measured ratio cited in its place.

**DL-28 (water-absorption file pre-conversion trap).** Add a parse-time or
load-time sanity check on `colors/water_absorption.spectra`-shaped files
(e.g. flag values that look like a raw `sigma_a` table — order-of-magnitude
too large for a `[0,1]` transmittance) or provide an `exp`-based scalar
painter transform so a published table can be dropped in directly. Fixed
when a malformed file produces a diagnostic instead of a silently wrong
render, or the transform closes the gap outright.

**DL-29 (measured tau curve grey under RGB).** Give
`PiecewiseLinearScalarPainter` (or a sibling) an RGB-aware evaluation mode
that integrates the curve against the CMFs instead of point-sampling one
wavelength. Fixed when `HasPerChannelVariation()` can return true for such
a painter and a render regression shows the depth tint surviving under
`pathtracing_pel_rasterizer` without the three-number RGB authoring idiom.

**DL-30 (CSG exit-designated subtraction dndu pairing, untested).** Author
a nested-CSG scene that reaches a subtraction's exit-designated branch
(the doc names it as the one construction that can) and add it to
`tests/CsgSurfacePayloadTest.cpp`, asserting `dndu`/`dndv` stay sign-paired
with `vNormal` there the way the 2026-08-29 sign-pairing family already
covers the other CSG branches. Fixed when that case exists and passes.

**DL-31 (mesh neighbour is a sheet).** Add a robust closed-mesh
containment test (e.g. parity ray-cast or a precomputed winding number)
and wire it into `Object::DistanceToSurface`/`interior()` for mesh
geometry. Fixed when a receiver point buried inside a closed mesh
neighbour reads `interior(r) > 0` in a `ProximitySignalTest` case, matching
every solid family's existing behavior.

**DL-32 (standard_object single-number scale degenerates silently).**
Add a parser-time diagnostic when a `DoubleVec3`-typed `scale` is given a
partial fill (or an explicit single-scalar broadcast convention, if that
is the intended authoring shorthand) on `standard_object`. Fixed when
authoring `scale 0.35` either broadcasts predictably to `(0.35,0.35,0.35)`
with a stated convention, or fails to parse with a clear message, instead
of silently vanishing the object and refusing every proximity query
against it.

**DL-33 (interior's candidate-walk cost shape unmeasured).** Instrument
`BVH::ForEachContainingPoint` the way `NearestOtherSurface` is instrumented
(distance-call / node-visit counters) and measure it on the same Sponza-
class scene the doc's own `11.08`/`5.617` figures come from. Fixed when a
comparable number exists and, if it regresses badly on a pathological
scene (e.g. deeply nested containing volumes), a prune or early-out is
added.

**~~DL-34 (interior under-reads inside a union overlap).~~ CLOSED
2026-09-12 for the published regression — `cffa254f`.** The proposed
`min(f_A,f_B)` to `max(depth_A,depth_B)` edit was a no-op: those already
have the same magnitude, 1.6 in the named example. Certified inscribed-ball
union geometry now supplies the stronger bound, and ProximitySignalTest
pins the analytic `sqrt(3.56)` through direct, manager and interior queries.
The unchanged committed red test goes from 465 passed / 6 failed to
471 passed / 0 failed; supplemental edges bring the final count to
491 passed / 0 failed. Arbitrary union distances remain conservative and
non-exact; see [scope](DL34_UNION_INTERIOR_DEPTH.md).

**DL-60 (missing committed replay fixtures).** Add valid replay fixtures
for altar_stress and rainwet_closeup. Run their real scenario/checkpoint
paths with scene and trajectory activity controls, then AgentEvalCheckTest;
the 23 reproduced load-error assertions must disappear. Do not exclude the
scenarios or weaken their checkpoints to manufacture a pass.

**DL-61 (render checkpoint disagreement).** Reproduce the nine assertions
from constant_materials_polish and image_reconstruct_multi with seeded,
finite, linear measurements. Establish an independent intended-output
reference and fix the renderer or fixture/oracle according to the observed
cause. Rerun AgentEvalCheckTest; preserve lighting/image activity and
reference-shape controls rather than merely widening the current bands.

**~~DL-36 (bounded-neighbour-read residual on a luminary probe).~~ CLOSED
2026-09-12 as a consistency pin — `ac9891f3`, `Passed: 16  Failed: 0`.**
The two-blade fixture in SignalEmitterRecordTest directly proves the
within-band neighbour is accepted with its live channel and that replay
preserves sampled geometry. Unobstructed and reverse-normal controls
bound the behavior. This follows the original recipe's permitted
retain-and-pin option; the unchanged implementation passed, so no red
physics failure or corrected signal discontinuity is claimed. See
[fixture, limits and audit](DL36_EMITTER_NEIGHBOUR_PIN.md).

**DL-37 (GGX low-F0 grazing gain).** Closed by the reciprocal single-pass entry/exit model in [DL-37 evidence](DL37_GGX_DIFFUSE_TRANSMISSION.md). Configs 17/20 now bound all four angles, and mixed conductor/film siblings are bounded too. The original fifteen-site inventory was wrong: seven constant-split formulas existed. One-angle attenuation would violate reciprocity; normalized macro-interface recycling would not prove rough-GGX conservation. The chosen model permits loss. Separate specular-only controls exposed DL-63; their original bound and nonzero exit remain.

**DL-38 (translucent stateful BSDF/HWSS repricing).** Add
`tests/TranslucentTransportWeightTest.cpp` with an inside exit at uniform
Phong N=1 (so the Phong/cosine shape itself agrees), nonzero extinction,
and scattering in {0, 0.3}. Check the PT HWSS companion weight and the
BDPT eye/light non-delta continuation against the independently computed
Beer split used by the SPF. Vary extinction while keeping ref/tau fixed
to expose reevaluation through a BSDF that cannot read extinction. Use
matched RGB/NM/HWSS controls and test VCM/MLT through their shared walks.
Fixed when those stateful weights agree without dropping the stack or
confusing this defect with DL-02's separate density contract. No numeric
transport deficit is asserted until that red-proof is run.

**DL-39 (translucent photon absorption deposited as power).** Add
`tests/TranslucentPhotonEnergyTest.cpp` that drives the dedicated tracer
through an interior segment with scattering zero and extinction in
{0, 0.1, 1}. Inspect deposited flux independently of the gather BSDF;
expected deposition is incoming power times `exp(-extinction*distance)`
for the diffuse exit, not all incoming power. Extend to nonzero
backscatter to assert absorption + outgoing + deposited energy balances.
Fixed when deposited power follows the emitted diffuse lobe and absorbed
energy is never added to the map. Static finding; not yet red-proven.

**DL-40 (nonfinite balance-test statistics accepted).** Add exact
invalid-input checks to the BDPT/VCM balance and SignalEmitterRecordTest
harnesses: finite reference
statistics versus NaN candidate mean/p99/max must disagree, and a capture
containing a nonfinite component must be rejected before statistics or sorting. Use
explicit malformed-input fixtures, not a NaN not-found sentinel. Fixed
when `ComputeStats` rejects nonfinite captured/composited values and
`ChannelsAgree`/`WorstRelDiff` reject nonfinite operands, with the new cases red-proven
against the current harness and the existing finite render gates intact.
This is a separate pre-existing harness robustness defect; the DL-01
runs reported finite statistics and do not exercise it.

**DL-41 (translucent complete mixed-lobe/reverse density).** Add a focused
translucent mixture-density test with nonzero entry reflection/transmission
and inside backscatter. Compare the actual selected-lobe frequencies and
conditional densities with Pdf/PdfNM over both hemispheres; include RGB
per-channel N and spectral wavelengths. Assert BDPT eye/light reverse and
NEE evaluations use the appropriate state and match the same complete
mixture, then exercise VCM/MLT's shared generators. Fix the mixture/state
contract rather than multiplying a guessed constant into the corrected
DL-02 cosine exit. Static evidence only; numeric red proof is pending.

**~~DL-42 (PT guided selected-lobe compensation).~~ CLOSED 2026-09-13 —
`PathTracingIntegrator.cpp` fix, `tests/PTGuidedSelectProbTest.cpp:
ALL TESTS PASSED`.** The ledger's plain-English evidence text named the
two `PTMulDiv()` overwrite sites (RIS-accepted, one-sample
guided-direction-accepted) as illustrative examples of "the trained-guiding
branch," but re-deriving the actual estimator (per this row's own recipe
instruction to "think carefully about what the correct estimator is")
found the real, immediately-fixable defect is narrower: `IBSDF::value()`
and `ISPF::Pdf()` are the material's AGGREGATE (all-lobes) response
(`GGXBRDF::value()`: `return diffuse + specular;`; `GGXSPF::Pdf()`: "3-lobe
mixture PDF weighted by painter albedos"), and the two `PTMulDiv` sites --
which both re-evaluate BSDF/PDF through `PTEvalBSDFAtSurface`/
`PTEvalPdfAtSurface` at the priced direction -- stay entirely inside that
aggregate representation, so unlike the third site below they at least do
not mix per-lobe and aggregate units within themselves.
**Correction (2026-09-13, same slice, before merge): the sentence
originally continuing from here claimed this made the two sites "already
complete, self-contained one-sample/RIS estimators for the WHOLE material"
that "must NOT be divided by `selectProb` again." That claim is FALSE and
is withdrawn; see DL-67, which supersedes it.** `GuidingSupportsSurfaceSampling`
admits only non-delta eRayDiffuse/eRayReflection lobes, so at a multi-lobe
surface with a guiding-ineligible lobe present (e.g. this row's own
`TranslucentMaterial` EXIT fixture: diffuse-exit eligible, translucent-backscatter
not) these two branches price the material's FULL aggregate `value()` --
which covers every lobe, eligible or not -- while guiding only ever
proposed/weighted the eligible subset, and the ineligible lobe is priced
AGAIN whenever `PTRandomlySelect` happens to pick it via the ordinary
kray/selectProb path; separately, even on an all-eligible material, the
fixed branch's per-lobe proposal density and these two branches' aggregate
proposal density do not partition to a consistent total unless the SPF has
only one lobe (DL-67 has the numeric counterexample). The bug this row
DOES fix is the THIRD overwrite site, the "kept BSDF direction" fallback
(`combinedPdf = GuidingCombinedPdf(alpha, guidePdfForBsdf, pS->pdf);
scatterThroughput = PTScatterKray(*pS) * (pS->pdf/combinedPdf);`), which
stays entirely in per-lobe `kray`/`pdf` units (matching the ledger's own
literal evidence cell, "kray*pdf/combinedPdf... without selectProb") and
silently drops the initialization's `/selectProb`. Fixed to
`kray*pS->pdf/(selectProb*combinedPdf)`. `ggx_material`/`coated_material`
cannot exercise this bug (their SPFs do internal single-lobe selection,
emitting exactly one `ScatteredRay` per `Scatter()` call, so the outer
`selectProb` is trivially 1) -- the real-trained-guiding red-proof instead
uses `TranslucentMaterial`'s EXIT branch, whose two non-exclusive,
non-delta rays (diffuse exit + translucent backscatter, unequal weight via
`scattering`) are exactly the "material with multiple nonzero lobes" this
row calls for (only the diffuse-exit lobe of that pair is itself
guiding-eligible; the backscatter lobe is what makes `selectProb < 1`
here, not a second eligible candidate). See the main table row above for
the full red/green counters and the sibling audit. **Correction on the
BDPT-analog check and the HWSS claim, both withdrawn (2026-09-13, same
slice, before merge):** an earlier version of this paragraph said BDPT's
"kept BSDF direction" branch "does not share this pattern" because it
"always uses the aggregate `PathValueOps::EvalBSDFAtVertex`... for its
non-delta throughput" -- true as stated, but that BDPT code pairs that
aggregate value with a PER-LOBE `effectivePdf`, an independent
measure-mismatch (an over-count on overlapping-support lobes, not this
row's under-count) now filed as DL-69; the sentence was accurate about
this row's specific pattern but was read as broader clearance than it is.
Separately, an earlier version claimed "HWSS is covered by the same
shared RGB/NM template (`IntegrateFromHitNM` exercises the identical
fixed line; the test's NM rows are byte-identical red/green to the Pel
rows)." PT's HWSS body, `IntegrateFromHitHWSS`, contains NO guiding block
at all (verified: zero occurrences of `pGuidingField`/`GuidingCombinedPdf`/
`GuidingRIS` across that entire function) -- it is simply unaffected by
this fix, and only reaches the fixed line indirectly, through its
per-wavelength fallback calls into `IntegrateFromHitNM` for BSDF
materials, not through its own scatter loop. Two new debts were found and
filed, not fixed, during this derivation: DL-67 (RIS/one-sample proposal
and throughput density inconsistency across all three trained-guiding
branches, PT and BDPT) and DL-69 (BDPT's ordinary, non-guiding throughput
pairing an aggregate BSDF value with a per-lobe selection pdf).

**~~DL-43 (BDPT eye guiding PDF argument order).~~ CLOSED 2026-09-13 —
`a69c9ce6`, `TranslucentIORStackTest: ALL TESTS PASSED`.** Extended the
existing DL-03 BDPT fixture (real trained OpenPGL field + real
TranslucentSPF + production `BDPTIntegrator` RGB/NM eye/light subpath
generation, `tests/TranslucentGuidedStackProbe.h`) to capture the exit
vertex's first guiding-phase `Pdf()`/`PdfNM()` query per trial (a second,
later query at the same vertex is unrelated MIS reverse-density
bookkeeping and must not be conflated with it) and assert that, for every
outward-substituted trial, the captured density equals the substituted
direction's own outgoing cosine/pi. Red on unfixed `42f3dc97`: eye RIS
`substituted_out=0` (every guide candidate rejected outright — a
swapped-argument density landing on the wrong side of the SPF's
geometric-horizon gate reads zero) and eye one-sample `bad_pdf_value=41`
(all 41 outward substitutions carried the wrong density, independent of
which direction the guide actually proposed); all light-subpath rows
already passed, confirming only the eye-subpath argument order was wrong.
Fixed by swapping the two `PathValueOps::EvalPdfAtVertex` call sites
(RIS candidate 1, one-sample) in `GenerateEyeSubpathImpl` to
`(-currentRay.Dir(), candidateDirection)`, matching the light-subpath
twins; `EvalBSDFAtVertex` calls at the same sites are unchanged (BSDF
value is reciprocal, so argument order doesn't change the result there).
Green: eye RIS `substituted_out=13`, `bad_pdf_value=0` everywhere.
`PathValueOpsTest.cpp` Test G additionally pins the closed form at a real
`LambertianMaterial` vertex (pre-fix eye order reproduces the incoming
direction's cosine/pi regardless of the candidate; light order already
matched the candidate's own outgoing cosine/pi) as documentation, not a
red/green discriminator. Sibling audit (docs/skills/audit-by-bug-pattern.md):
the ~14 other `EvalPdfAtVertex` sites in `BDPTIntegrator.cpp` are Veach MIS
reverse-density/connection-strategy conversions that deliberately query the
opposite direction and were spot-checked as already correct;
`PathTracingIntegrator.cpp` has no `EvalPdfAtVertex` calls at all (PT's
guiding uses the single-direction-argument `PTEvalPdfAtSurface` /
`PathVertexEval::EvalPdfAtSurface` against an already-real
`RayIntersectionGeometric`, architecturally immune to a two-argument
swap) — DL-42 (PT's separate selected-lobe-compensation bug) is unrelated
to this call shape.


**DL-44 (sampled emitter UV propagation).** Add a deterministic two-texel
emitter regression that selects known nonzero UVs through real SampleLight,
then checks RGB emission, NM hero, HWSS companions, LIGHT-root replay and
VCM rebuilt emission against direct evaluation at those sampled UVs. Pin
both texels and a constant-emitter control, with signal demand disabled and
enabled. Red-prove the default-(0,0) mismatch, carry sampled UV through the
shared sample and every consumer, and audit ptCoord one field further into
texture evaluation. MLT shares the BDPT generators. Static evidence only
at discovery; no rendered failure or measured bias is claimed.


**DL-45 (tilted translucent exit geometric support).** Turn the DL-03
observed unchanged-SPF defect pin into a failing correctness regression:
real RGB/NM inside exits at aligned and tilted shading normals must pair
physical boundary state with their directions. Choose and document an
exit sampling policy with matching normalized PDF and weight/energy
accounting; simply dropping or retrying below-surface directions without
those corrections is insufficient. Cover tilt angles, enclosing IOR,
scattering endpoints, and compare a unit-energy directional integral. Keep
guided replacements and unchanged SPF samples distinct so the already-fixed
DL-03 state mapping does not mask this independent sampler defect.


**DL-46 (initial translucent containment missing).** Add a real closed
translucent object with camera/light origins inside it, in air and inside
a distinct refractive enclosure. Exercise production SeedFromPoint and the
first real Scatter/ScatterNM: initial membership must identify that first
physical crossing as exit and preserve the enclosing numeric IOR. Include
outside-origin and pure-reflector/Lambertian controls. Red-prove the current
skip, then distinguish stateful membership tracking from specular refraction
capability; setting canRefract merely to obtain seeding is not an acceptable
representation of this non-delta material. Cover shared PT/BDPT origin paths.


**DL-47 (guide-created translucent entry lacks state).** Add a design note
for material-aware accepted-direction state generation. Red-prove a real
trained guide at an outside entry: select the non-delta reflection lobe,
require actual inward replacements with positive BSDF/combined PDF, and
observe object membership and exit classification at the next physical
boundary. Cover PT RGB/NM and BDPT eye/light, nested enclosing IOR, outward
reflection controls and retained SPF proposals. Decide the state and
mixture contract together: neither guessing a push for every null-stack
material nor discarding positive proposals without probability/energy
accounting is sufficient. Include multiple-lobe/composite ambiguity and
coordinate with DL-41, while retaining DL-03's available-exit-state tests.
This is static evidence with an executable red proof pending; it is not
DL-46's initial-containment seeding failure.


**~~DL-48 (SSS Sw cosine normalization).~~ CLOSED 2026-09-12 —
`12a7ef3e`, BSSRDFNormalizationTest passes after 37 constant-normalization
failures and 8 textured-IOR follow-up failures.**
The completed recipe was: commit a numerical hemisphere
integral test of the actual adapters and BSSRDFSampling directional helper
across relative indices and require integral one; demonstrate the current
non-unit result before changing c. Derive normalization from the same
Fresnel law being evaluated, then update all sampled and reevaluated
weights together (diffusion/RW, RGB/NM, PT/BDPT/VCM/MLT consumers). Confirm
a conservative furnace without adding a separate eta-square multiplier.
See [the closure and audit](DL48_SSS_NORMALIZATION.md); the rendered
furnace remains a coarse convention guard because DL-49 is a separate
open defect (the only one left OPEN in the DL-49..DL-53 range this
sentence originally pointed at — DL-50 through DL-53 are all CLOSED).

**DL-49 (SSS exterior IOR).** Write a design note for carrying the actual
exterior index through profile, random-walk and directional evaluation
contracts. Red-prove co-scaled exterior/interior indices with fixed ratio
using the real integrator and seeded exterior stack, plus a rendered
non-air enclosure case. Hold neutral coefficients, directions and lighting
fixed so observer-interface changes cannot cancel the defect. Cover RGB/NM,
profile variants, rough BSDF versus SPF, and bidirectional reevaluation.
Do not encode a guessed constant or alter the complete-event telescoping
convention to hide relative-index errors.

**DL-50 (RW spectral survival double-count).** Use a real closed object
and neutral coefficients, with deterministic sampler draws that select an
exit before a volume collision. The conditional surviving weight must be
one in both RGB and NM before angular factors; also compare unconditional
Beer attenuation over repeated draws. Red-prove the NM extra transmittance
then align it with the probability already paid by distance sampling.
Keep angular/diffusion approximations separate from this survival weight.

**DL-51 (standalone SSS exit destination IOR).** Construct the real SPF
with bAbsorbBackFace=false and a seeded object stack inside a distinct
enclosing medium. Red-prove Snell direction, Fresnel/TIR and popped state
for RGB/NM at normal and oblique incidence. Resolve the exterior from the
post-pop state when calculating exit optics. Include bAbsorbBackFace=true
controls demonstrating shipped materials still absorb that inside hit;
do not claim that the dormant fallback changes their supported topology.


**~~DL-52 (BSSRDF planar probe-origin omission).~~ CLOSED 2026-09-13 —
`00cd6723`, BSSRDFPlanarProbeReachTest passes after 500/500 unfixed
0/500. A distinct follow-on defect the fix itself introduced (an
inverted entry normal on near-half hits for double-sided/flip-oriented
geometry) was found gating this closure and closed separately as
DL-71.**
The completed recipe was: commit a direct real-object
helper regression whose deterministic normal-axis samples on a broad flat
face must reach the nearby surface. Red-prove omitted support, then design
a finite chord traversal that includes the projection plane and counts
each hit once. Test positive/negative axes, flat/curved objects, thin and
disconnected geometry, and actual mixture PDFs/hit-count weighting. Audit
RGB/NM PT and BDPT eye/light consumers (VCM/MLT share BDPT) and probe caps.
Validate a conservative furnace and geometry convergence without changing
the complete-event eta convention or absorbing Sw normalization into geometry.


**~~DL-53 (recursive explicit-global-map MIS bypass).~~ CLOSED 2026-09-13 —
`b3de184d`, RayCasterEnvEscapeMISTest passes 79/79 after 48/79 unfixed
failures (P3-2: the suite grew to 91 checks the same day via `0eb7a47e`'s
added partition-of-unity assertions — current total is 91/91, quote that
going forward). A distinct follow-on defect (BSSRDF/volume continuations never
set `bsdfTimesCos`, so the optimal-MIS training arm this fix wired up
could never fire for them) was found gating this closure and closed
separately as DL-72.**
The completed recipe was: commit a real
RayCaster miss-ray regression with positive BSDF PDF and compare null
map versus explicit scene-global-map pointers against the same analytic
MIS-weighted radiance. Red-prove RGB/NM/HWSS; preserve PDF-zero delta
behavior and distinct local override-map semantics. Move common escape
weighting to a layer shared by both map-selection paths, then audit every
recursive consumer carrying bsdfPdf and medium survival. Validate complete
SSS environment NEE plus continuation against an independently integrated
angular oracle; do not change Sw or eta convention to hide the extra term.

**~~DL-71 (BSSRDF probe entry-normal inversion on near-half chord hits).~~
CLOSED 2026-09-13 — `e416d3bd`, BSSRDFPlanarProbeReachTest's DL-71 rows
pass 500/500 outward/neePositive after 0/500 unfixed.** See
[DL71_BSSRDF_PROBE_ENTRY_NORMAL.md](DL71_BSSRDF_PROBE_ENTRY_NORMAL.md)
for the full mechanism, repair, and sibling audit.

**~~DL-72 (BSSRDF/volume continuations never trained
RayCasterEnvEscapeMISWeight's optimal-MIS arm).~~ CLOSED 2026-09-13 —
`0c9eccc4`.** No render-time weight was wrong (`PowerHeuristic` is the
correct fallback whenever the optimal-MIS accumulator is absent or not
ready) — this was a training-input completeness gap only. See
[DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md](DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md).

**DL-54 (BSSRDF geometric projection density).** Commit a regression using
an actual curved object and a shading-normal-only entry modifier. Hold the
original exit record, profile and sampler sequence fixed; prove positive
matching probe-hit activity, then compare the reported surface PDF against
an independent geometric-normal disk-to-area Jacobian. Require the density
to remain unchanged when only entry shading normals tilt. Cover RGB/NM
sample paths and their shared PT/BDPT/VCM/MLT consumers. Fix the geometric
projection factor while retaining the shading frame for angular Sw;
audit other uses of entryNormal and entryGeomNormal without folding in
DL-52's separate coplanar-support defect. This is static evidence, not an
executed image-bias measurement.

**DL-55 (RW fallback proposal density).** Use a real closed object with
RGB extinction containing exact zero channels and positive other channels.
Commit tests before execution. Force boundary and collision outcomes and
check weights against the actual mixture of fallback exponential rates,
then compare unconditional pure-absorption RGB means to independent Beer
attenuation. Exercise nonzero scattering for collision weights. For the
NM tiny-extinction fallback, verify geometry range and positive activity
before checking the collision transmittance/proposal ratio. Cover full and
spatial weights and all shared consumers; preserve the DL-50 NM boundary
survival regression. Do not hide a proposal mismatch with a threshold.

**~~DL-56 (grazing dielectric Fresnel guard).~~ CLOSED 2026-09-12 —
`df7e3dad`, DielectricGrazingFresnelTest: `Checks: 338  Failures: 0`
(unfixed: `Checks: 128  Failures: 20`).** The committed-before-execution
direct-helper regression covers matched-index grazing cosines .01/.005/.001,
nearby unequal indices, exact tangent, normal-incidence common-IOR scales
1e-4/1/1e100, and TIR, with independent scalar Fresnel and Snell controls.
It also exercises real RGB/NM DielectricSPF paths and standalone non-absorbing
RGB/NM SubSurfaceScatteringSPF exits using a synthetic intersection, rather
than a new rendered fixture. The repair evaluates scaled physical s/p
amplitude ratios: matched indices retain their exact identity and critical
controls remain explicit; it adds no epsilon fallback. It changes neither
Snell classification, IOR-stack handling, nor eta conventions. See
[closure](DL56_GRAZING_FRESNEL.md). The distinct extremely-grazing
pre-Fresnel Snell/TIR cancellation group is DL-58.

**~~DL-57 (RGB dimensional collision-density cutoff).~~ CLOSED 2026-09-12 —
`c187f2cc`, RandomWalkDensityCutoffTest: `All DL-57 density cutoff tests
passed` (unfixed: one required large-RGB exit activity failure).** Tests were
committed before execution; real closed-sphere geometry proves the collision
and continuation ranges. Scaled RGB, NM and absorption controls passed before
the repair, while normalized weights and the scale-pair comparisons pass on
the unchanged test after the finite-positive density guard. Weight checks
behind the failed activity assertion are not claimed as separate red failures.
The legal sampler support excludes the boundary-survival cutoff as a reachable
sibling. Proposal rates, weighting formulae and throughput pruning are unchanged.
See [closure](DL57_RANDOM_WALK_DENSITY_CUTOFF.md).

**DL-58 (extreme-grazing false TIR). CLOSED 2026-09-12.**
Committed-before-execution direct and real SSS consumer regressions reproduced
9 and 10 failures. The original throughput run had 8 valid SMS failures and
3 invalid weave expectations: authored IOR 1 clamps to 1.001, so the recipe's
matched-index fibre material consumer is unreachable. Direct fibre math owns
that identity; the corrected weave checks cover the existing unequal clamp.
A supplemental still-unfixed incoming derivative case reproduced one failure.
Fixed counters: `GrazingSnellFresnelTest: Checks: 38  Failures: 0`,
`MatchedIndexGrazingConsumerTest: Checks: 60  Failures: 0`, and
`GrazingFresnelThroughputTest: Checks: 44  Failures: 0`.
The repair (`1b1909c0`, fibre compatibility refinement `ed8d9c94`) preserves
matched transmission/Fresnel and SMS derivative identities with stable Snell
cosines. Unequal fibre arithmetic remains bit-identical to its original
strict hair golden. Nearby unequal indices, signed cosine, normal incidence,
critical-angle controls and earlier IOR/Fresnel consumer gates are retained.
See [closure](DL58_MATCHED_INDEX_GRAZING.md) for the full evidence and caveats.

**DL-59 (unequal-index SMS normal derivative). CLOSED 2026-09-12.**
`fcbc69da` committed the regression before execution; the unfixed library
reported `Checks: 141 Failures: 12` (eight direct tangent-derivative errors,
four curved Jacobian entries). `a0c4a808` corrects the entering/exiting ratio
and original-normal sign; the unchanged test reports `Checks: 141 Failures: 0`.
Finite differences use tangent perturbations of unit normals and normalized
output correction; closed-form normal, matched, reflection and TIR controls
pass. The angle-difference consumer is test-only, so no production-render
correction is claimed. Five individual gates and clean Make/Xcode passed;
the source-hygiene environment retry is documented in
[the closure](DL59_SMS_NORMAL_DERIVATIVE.md).

**~~DL-62 (GGX glossy-filter sample/evaluation mismatch).~~ CLOSED
2026-09-13 — `dfdd5ee1` + `a495a357` (Schlick-mode-only weight), `GGXSampleEvaluationConsistencyTest: 48 checks,
0 failures` (29 at `dfdd5ee1`).** Test `a1db468d` failed 23 of 29 checks against the
unfixed library (e.g. "asymmetric saturation W=0.7 (Y saturates)
maxRelErr=1.910e+01"). `GGXBRDF::value`/`valueNM` now widen alphaX/
alphaY by `ri.glossyFilterWidth` (`min(alpha+width,1)`) exactly like
GGXSPF's four sample/density paths. Every RGB/NM probe (isotropic,
anisotropic, spec-only, single- and double-axis saturation) now matches
an independently constructed pre-widened-reference `GGXBRDF` to
relErr=0.000e+00; the zero-filter control and a `GGXSPF::Pdf`
already-consistent control matched both before and after, isolating the
bug to `GGXBRDF` specifically. See [closure and audit](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md).

**DL-63 (GGX specular compensation uses a different masking model).** Retain the three committed `GGXDiffuseTransmissionTest` specular-only red controls and add independent single-scatter integral versus LUT checks. Rebuild the compensation data for the actual height-correlated GGX model, with an explicit decision for anisotropic compensation and any separable-model consumers sharing the current table. Preserve sampled/evaluated MS normalization and generator provenance. Require zero specular-only energy failures without changing the committed statistical bound; gate GGX and every other affected LUT consumer separately. **Re-verified unchanged by the DL-62/DL-64 fix (2026-09-13, `dfdd5ee1`):** `GGXDiffuseTransmissionTest` still reports `150 checks, 3 failures` at the identical F0=1 configs (alpha=0.6 theta=80 -> 1.0772; alpha=1 theta=60 -> 1.0432; alpha=1 theta=80 -> 1.1467), bit-for-bit matching a stashed pre-fix rebuild — these are pure-specular (F0=1) controls, outside both DL-62's filter-width scope and DL-64's F0=0 scope.

**~~DL-64 (zero-F0 Schlick sampling support).~~ CLOSED 2026-09-13 —
`dfdd5ee1`, same `GGXSampleEvaluationConsistencyTest` run.**
`GGXSPF::Scatter`/`ScatterNM`/`Pdf`/`PdfNM` now derive the specular/MS
lobe-selection weight from `GGXInterfaceFresnel::Mean()`/`MeanNM()` (the
lobe's actual hemispherical Fresnel-weighted albedo — nonzero at F0=0,
`SchlickFresnelAvg(0)=1/21`) instead of raw authored F0; per-direction
Fresnel evaluation for the sampled lobe (the true per-wavelength F0,
`wsF0` in `ScatterNM`) is unaffected — no magic F0 floor was added. A
deterministic Pdf-at-the-specular-peak reference (immune to the
unrelated ~2.5e-5 JH-black-uplift epsilon that would have made a naive
">cosine" threshold pass even pre-fix) moved from relErr 0.87-0.99 to
0.000e+00, at both grazing and normal incidence and both zero- and
nonzero-diffuse configurations, RGB and NM. A companion control confirms
`GGXBRDF::valueNM`'s Schlick term (the HWSS-companion path, since
`GGXSPF` does not override `EvaluateKrayNM`) was never affected. Fixing
this exposed a pre-existing test-design assumption in
`tests/ThinFilmBRDFTest.cpp` Test B (pSpecSelect no longer cancels
between thin-film and bare-conductor GGX twins at the same tint, since
their hemispherical Fresnel averages now legitimately differ) — repaired
by explicitly dividing each fixture's own pSpecSelect back out before
ratioing, restoring the original ~1e-16 single-scatter pin (measured
9.821e-16 post-fix). See [closure and audit](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md).

**DL-65 (CookTorrance/Schlick glossy-filter sample/evaluation mismatch).**
For each of `CookTorranceSPF`/`CookTorranceBRDF` and `SchlickSPF`/
`SchlickBRDF`: commit RGB/NM sample-versus-BRDF-evaluation checks with
nonzero `ri.glossyFilterWidth` and a zero-filter control (mirroring
`GGXSampleEvaluationConsistencyTest`'s pre-widened-reference technique —
construct a second fixture whose roughness painter already holds
`min(roughness+width,1)` with `glossyFilterWidth=0`, and assert the
production object's evaluation matches it exactly). Run against the
unfixed library first and paste the failing output into the fix commit.
Add the same widening to `CookTorranceBRDF::value`/`valueNM` and
`SchlickBRDF::value`/`valueNM` that their SPF twins already apply,
preserving each class's own existing roughness convention (do not import
GGX's `alphaX`/`alphaY` split or its `min(x+width,1)` formula verbatim
without checking each class's own parameterization first). Gate
`CookTorranceMultiscatterTest`, `SPFPdfConsistencyTest`,
`SPFBSDFConsistencyTest`, and any suite `grep -l 'CookTorranceSPF\|
CookTorranceBRDF\|SchlickSPF\|SchlickBRDF' tests/*.cpp` returns.

**DL-67 (PT/BDPT guided one-sample and RIS proposal densities are inconsistent across lobe-selection branches; filed in this slice's own ledger as DL-65, renumbered DL-67 at merge -- see `## Counts`).** At a real multi-emit material (`schlick_material`: diffuse + specular, both guiding-eligible, overlapping support), drive RIS-mode and one-sample guiding through the real `PathTracingIntegrator`/`BDPTIntegrator` (eye and light) with `guidingAlpha` forced near 1 and a real trained field so substitution actually fires (`guidedSubstituted>0`, unlike DL-42's own fixture, which forced it to 0). Independently evaluate, at each candidate direction, the TRUE generating density `sum_I q_I * p_I` (`q_I` from `PTScatterSelectWeight`/`MaxValue(kray)`, `p_I` each lobe's own conditional pdf) against (a) `pS->pdf` (candidate 0's single-lobe density), (b) the material's aggregate `ISPF::Pdf()` (candidate 1's and the aggregate-BSDF branches' density), and (c) DL-42's fixed per-lobe branch's own denominator. Assert all three diverge from the true mixture density at some incidence angle (red-proving the mismatch exists in practice), and assert rendered radiance against an unguided PT reference on a scene where substitution fires disagrees by more than noise. Then implement the correct, consistent construction: one `f` and one denominator -- the true mixture density `sum_I q_I p_I` over the lobes `Scatter()` actually accepted -- in ALL THREE PT trained-guiding branches (candidate-0/RIS, one-sample kept-BSDF, one-sample guided-accepted) and both BDPT eye/light RIS instantiations, RGB and NM. Also resolve the candidate-validity asymmetry (candidate 1 discarded outright when the aggregate pdf is 0 at the guided direction, silently losing that RIS candidate) as part of the same rework. Cover PT and both BDPT eye/light instantiations, RGB and NM; verify against a known closed-form RIS estimator (e.g. a two-lobe synthetic material with hand-computed selection/conditional densities) in addition to the rendered comparison. Static evidence and closed-form counterexample only; rendered image-bias measurement pending.

**DL-69 (BDPT's ordinary non-delta throughput pairs an aggregate BSDF value with a per-lobe selection pdf).** Add a `schlick_material` (diffuse + specular, both non-delta, overlapping hemisphere support) topology to `BDPTStrategyBalanceTest` and/or a standalone red-proof harness, rendered against a `pathtracing_pel_rasterizer` PT reference on the identical scene, with guiding disabled (this is BDPT's UNCONDITIONAL non-guiding throughput path, independent of DL-67). Red-prove the over-count predicted by pairing `f_agg = PathValueOps::EvalBSDFAtVertex(...)` (all lobes summed) with `scatterPdf = selectProb * effectivePdf` (one lobe's own density) at both the eye (`GenerateEyeSubpathImpl`) and light (`GenerateLightSubpathImpl`) generators. Then fix by making the non-delta throughput construction internally consistent -- either per-lobe `f_I` paired with the per-lobe pdf (matching the delta branch two lines above each site, and PT's own `kray`/`selectProb` convention), or the aggregate `f_agg` paired with the TRUE aggregate mixture pdf (DL-67's `sum_I q_I p_I`) -- applied identically at eye and light, RGB and NM, and re-verified against DL-67's fix rather than independently, since both rows touch the same lobe-selection contract. Confirm `ggx_material` remains immune post-fix (its lobes already carry the shared `mixPdf`, not a per-lobe conditional density, so this pattern cannot arise there) and that `lambertian_material`/disjoint-support SPFs (TranslucentSPF, PolishedSPF) are unaffected. Static evidence and closed-form BRDF check only; rendered red-proof pending.
