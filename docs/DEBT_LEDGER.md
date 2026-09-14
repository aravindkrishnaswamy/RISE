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
| DL-67 | Found during DL-42/DL-43 (debt-guiding slice, 2026-09-13; filed in this slice's own ledger as DL-65, RENUMBERED DL-67 at merge time -- see `## Counts` -- to avoid colliding with rows concurrently filed by sibling debt-cleanup slices: DL-65 is the ggx slice's row, DL-66 the sssenv slice's, DL-68 the translucent slice's) | PT's and BDPT's guided one-sample and RIS proposal/throughput densities are inconsistent across lobe-selection branches at a multi-lobe SPF: neither RIS candidate 0 nor the one-sample/RIS "aggregate BSDF" branches use the TRUE generating density of the direction they price | OPEN-confirmed (static + closed-form counterexample; rendered red-proof pending) | Root cause: at a multi-lobe SPF, candidate 0's (and any kray-based branch's) TRUE generating density for a direction is the mixture `sum_I q_I * p_I` over every lobe `Scatter()` could have emitted, where `q_I = PTScatterSelectWeight(lobe_I) / sum_j PTScatterSelectWeight(lobe_j)` (`PathTracingIntegrator.cpp:1337`, `MaxValue(kray)`-weighted) -- NOT the selected lobe's own `pS->pdf`, and NOT the material's aggregate `ISPF::Pdf()` either, because `ISPF::Pdf()` may re-weight lobes by a different ratio than `PTRandomlySelect` used to pick one. (The original example here was `SchlickSPF::Pdf`, which weighted diffuse/specular by `MaxValue(rd)` vs `MaxValue(rs)` -- the raw painter albedos -- while `PTScatterSelectWeight`/`PTRandomlySelect` weight the SAME two lobes by `MaxValue(diffuse kray)` vs `MaxValue(rho+(1-rho)*fresnel)`. **That example is HISTORICAL as of 2026-09-14**: Slice 0 replaced it with the true generating density (see the Slice 0 note at the end of this cell), so `SchlickSPF::Pdf` is no longer an instance. `IsotropicPhongSPF::Pdf` -- symbol, not line number -- still is: DL-98.). So: (1) candidate 0 (`c.bsdfPdf = pS->pdf`, `PathTracingIntegrator.cpp:3137`; `BDPTIntegrator.cpp:2537` eye / `:6276` light) uses the wrong (single-lobe) density, and candidate 1's aggregate `PTEvalPdfAtSurface`/`PathValueOps::EvalPdfAtVertex` uses a DIFFERENT wrong (aggregate-but-mismatched-weights) density -- both miss the true `sum_I q_I p_I`, so RIS's `sum_i P_i/D_i = M` identity is violated: a closed-form 2-candidate example with `p1=10, p2=0.1` (per-lobe pdfs), guide pdf `p_g=1`, true aggregate `p_agg=5.05` gives per-branch coefficients 1.074 (candidate-0-style) vs 0.256 (aggregate-style) against the correct value, i.e. roughly a 33% low estimate using the true generating density as ground truth. (2) The same inconsistency exists between PT's THREE one-sample/RIS overwrite branches in `PathTracingIntegrator.cpp`'s trained-guiding block: branch (a), this slice's DL-42 fix (`PTScatterKray(*pS) * pS->pdf/(selectProb*combinedPdf)`, per-lobe kray/pdf); branch (c), the one-sample guided-direction-accepted branch (`PTMulDiv(fGuided,cosTheta,combinedPdf)`, aggregate f over aggregate-but-mismatched pdf); and the RIS-accepted branch (b). (a) and (c) do not partition to a consistent total unless every lobe shares one pdf (single-lobe SPF) -- adding `/q_I` to (c) does not fix this; the correct construction uses one `f` and one denominator (the true mixture density `sum_I q_I p_I` over the lobes `Scatter()` actually accepted) in ALL THREE branches, in both PT and BDPT eye/light. Scope: any multi-emit SPF (SchlickSPF, PolishedSPF/PolishedBRDF, TranslucentSPF, CompositeSPF, BioSpecSkinSPF, GenericHumanTissueSPF); `GGXSPF`/`CoatedSPF` are immune (internal single-lobe selection: exactly one `ScatteredRay` per `Scatter()` call, `pdf==mixPdf`, outer `selectProb` trivially 1). A separate, additive asymmetry in the same candidate machinery: candidate 1 is discarded (`c.valid=false`, `risWeight=0`) whenever the material's aggregate pdf is 0 at the guided direction (`PathTracingIntegrator.cpp:3148` candidate 0 / `:3177` candidate 1 / `:3191` fallback; `BDPTIntegrator.cpp:2547`/`:2584`/`:2598` eye, `:6286`/`:6314`/`:6328` light), silently falling back to candidate 0 alone and losing whatever variance-reduction the guide would have offered there -- energy-losing in the sense of discarding a legitimate RIS candidate, tracked as part of the same rework rather than a fourth bug. Recipe: red-prove on `schlick_material` (diffuse+specular, both eligible, overlapping support) with guiding forced to substitute (`guidingAlpha` near 1, a real trained field so `guidedSubstituted>0`, unlike this slice's DL-42 fixture which forced `guidedSubstituted=0`), comparing rendered radiance against unguided PT reference; fix = the one-f/one-denominator construction above in PT (all three branches) and BDPT eye/light RIS. **Slice 0 landed 2026-09-14** (`docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md`, branch `debt-slice0`): `SchlickSPF::Pdf`/`PdfNM` now returns the ACTUAL density of the direction `Scatter` + `RandomlySelect` produce -- the `ISPF::Pdf()` half of this row's own root-cause paragraph. The diffuse lobe's coefficient `C_D = E_{(u,v)}[P(the diffuse ray wins | that specular draw)]` is computed by a DETERMINISTIC 16x16 stratified quadrature that REPLAYS `GenerateSpecularRay` in its own `(xi,b)` inverse-CDF parametrisation (sharing `SchlickSampleHalfVector` with the sampler, and reproducing Scatter's accept-checks and `RandomlySelect`'s `freeidx==1`/underflow branches); the specular coefficient is exact at the query direction; the per-channel branch's three lanes (which share ONE random pair) are handled exactly by inverting the sampler at the query direction and replaying the other two lanes; and a tilted shading normal is modelled by the exact clipped-cosine-hemisphere fraction `A_D=(1+dot(n,geomN))/2`. **This CORRECTS, not extends, the first version of this slice**, which used `MaxValue(rd)/(MaxValue(rd)+SchlickFresnelAvg(rs))` for `C_D`: that is the wrong quantity three times over (measured, see the doc's §3 table): the hemispherical average is under the wrong measure (mild -- 0.9009 vs 0.9048 at `rs=0.9`, but 0.1961 vs 0.0667 at `rs=0.02`/80 degrees); `C_D = E[w_D/(w_D+w_S)]` is a CONVEX function of `w_S`, so substituting any average of `w_S` is a different number by Jensen regardless; and the term that actually dominates is not a reflectance at all but the specular sampler's REJECTION rate -- `Scatter` accepts only 0.739/0.640/0.577 of its specular draws at 30/60/80 degrees incidence, and a rejected specular draw leaves `RandomlySelect` holding one ray, returned with probability 1. Even a rejection-aware closed form (`A*rd/(rd+rs)+(1-A)`) still misses true `C_D` by up to 13.7%, so there was no closed form to reach for. Measured, the first version's aggregate integrated to 0.68-1.02 instead of 1 and its total variation against the real sampler was WORSE than the code it replaced on 6 of 8 configurations. Its two gates could not see this: one compared `Pdf()` against a replica of its own formula, the other asserted `Pdf(w_S) >= q_S p_S`, an identity for any non-negative `C_D`. Both were deleted. Red-proof is now a two-sided normalisation gate (a hemisphere quadrature of `Pdf` against the MEASURED probability that `Scatter` emits any ray, over 600k real calls) plus a total-variation gate against a 600k-draw histogram of real `Scatter`+`RandomlySelect` draws, over 12 configurations, RGB and NM: `Checks: 34 Failures: 34` at `4325f365`, `33` at pre-slice `32824325`, **`0` after**, with every `|int Pdf - emitted|` <= 0.0062 and every TVD <= 0.0081 against a derived MC-noise floor of 0.0051. Two further defects of the same pattern were found while measuring and fixed in the same commit: the anisotropic AZIMUTHAL DENSITY was a different distribution from the one the sampler draws (up to 25x off at isotropy 0.3, measured against a 2e6-draw histogram; recorded as **DL-102**, closed), and `ComputeSchlickSpecularPdf` built its half-vector in a FlipW'd frame on the false premise that `GenerateSpecularRay` does (that function's `onb` parameter was dead; it always used `ri.onb`). `SPFPdfConsistencyTest`'s Schlick row consequently tightened from a 15% integral tolerance to the standard 5% (measured 1.00166/0.999932) and its chi2 is now GATED (783.7/816.3 vs critical 928.3) instead of skipped; its old note blaming "the specular PDF normalisation assumes isotropic roughness" was a misdiagnosis and is corrected in place. Cost: `Pdf()` 17.0 ns -> 690 ns (2003 ns on the 3-lane per-channel branch); whole-render CPU +2.5% (inside a +/-4% spread) on a Cornell box where 1 of 15 objects is schlick, +26% on a variant where every surface is. Rendered frame-mean luminance on that scene moved 0.995781 +/- 0.000318 -> 0.997451 +/- 0.000065 over 4 runs each (+0.168%). This does NOT close the row: candidate 0/1's own PT/BDPT branch-level fix (this row's actual subject) is unstarted. Sibling audit opened DL-98, DL-99, DL-100, DL-101 and closed DL-102; `CookTorranceSPF`/`GGXSPF`/`CoatedSPF`/`PolishedSPF`/Ward/`SchlickBRDF` confirmed immune or already correct on THIS row's pattern. | M-L | physics-bias | user-visible (guided renders of any multi-lobe, multi-emit material) |
| DL-69 | Found during DL-42/DL-67 derivation (debt-guiding slice, 2026-09-13) | BDPT's ORDINARY (non-guiding, unconditional) eye/light non-delta throughput pairs the material's AGGREGATE (all-lobes-summed) BSDF value with a PER-LOBE selection pdf, over-counting by up to Nx at a multi-lobe SPF whose lobes have overlapping support | OPEN-confirmed (static evidence + closed-form BRDF check; rendered red-proof pending) | `BDPTIntegrator.cpp` eye subpath (`GenerateEyeSubpathImpl`, ~2703-2777) computes `scatterPdf = selectProb * effectivePdf` (`effectivePdf` = the ONE stochastically-selected lobe's own `pScat->pdf`, or its guided/RIS-blended replacement -- still a per-lobe/single-candidate density, ~2703), then for non-delta lobes `f = PathValueOps::EvalBSDFAtVertex(...)` -- the material's AGGREGATE `IBSDF::value()`, summed over every lobe (~2763-2766) -- and sets `localScatteringWeight = f * cos / scatterPdf` (~2774-2775). The light subpath (`GenerateLightSubpathImpl`, ~6459-6476) is byte-for-byte the same pattern (`scatterPdf = selectProb*effectivePdf` ~6460, aggregate `f` ~6449-6453, `f*cos/scatterPdf` ~6466-6472). For N accepted non-delta lobes with overlapping support this is `E[throughput] = N * integral(f_agg * cos)` -- an N-times over-count, not the correct per-lobe `kray_I/selectProb` estimator used elsewhere in the same codebase (e.g. this file's delta branch two lines above each site, `KrayValue<Tag>(*pScat) * (.../selectProb)`, and PT's own ordinary initialization, `PTScatterKray<Tag>(*pS) * (1/selectProb)`). Confirmed on `SchlickBRDF::value()` (`return diffuse*INV_PI + (rho+(1-rho)*fresnel)*factor;` -- diffuse and specular summed over the SAME upper hemisphere) paired with `SchlickSPF::Scatter` emitting both a diffuse ray and a specular ray as separate non-exclusive `ScatteredRay`s into the same container -- or, on the anisotropic/per-channel branch, one diffuse plus THREE per-channel specular rays, i.e. up to a 4x over-count from that one vertex alone. (Cited by symbol: the `SchlickSPF.cpp` line numbers this row originally carried were invalidated by DL-67 Slice 0's fix pass on 2026-09-14; `Scatter`'s structure is unchanged.) Invisible on disjoint-support SPFs (TranslucentSPF's hemisphere-piecewise lobes; PolishedSPF's delta coat + non-delta substrate, where only one of the pair is ever non-delta at a time) -- and invisible on `lambertian_material` (only one lobe, so N=1 trivially) or `ggx_material` (immune for a DIFFERENT, confirmed reason than "single-lobe selection": `GGXSPF::Scatter`/`ScatterNM` set EVERY emitted lobe's `.pdf` field to the SAME `mixPdf` -- the 3-lobe aggregate mixture density, `GGXSPF.cpp:221/320/410` etc. -- not a per-lobe conditional pdf, so `effectivePdf` at this BDPT site already equals the aggregate density `EvalBSDFAtVertex`'s `value()` integrates against, and `f_agg/effectivePdf` is the correct aggregate/aggregate pairing by construction; `SchlickSPF`'s per-lobe `.pdf` fields are each lobe's OWN conditional density (cosine/pi for diffuse, the half-vector pdf for specular), which is what creates the mismatch). `coated_material`'s internal single-lobe selection (exactly one `ScatteredRay` per `Scatter()` call) is a separate, also-immunizing reason but not the GGX one. `BDPTStrategyBalanceTest`/`VCMStrategyBalanceTest` use only `lambertian_material` scenes, so this pattern has never been exercised by the existing balance gates. Recipe: add a `schlick_material` (diffuse+specular, both non-delta, overlapping support) topology to `BDPTStrategyBalanceTest` (and/or a standalone red-proof harness) vs a `pathtracing_pel_rasterizer` PT reference on the same scene; red-prove the over-count, then fix by making the non-delta throughput construction consistent -- either per-lobe `f_I` paired with the per-lobe pdf (matching PT's `kray` convention), or the aggregate `f_agg` paired with the TRUE aggregate mixture pdf (see DL-67's `sum_I q_I p_I`) -- applied identically at both eye and light generators. Also corrects the DL-42 ledger row's earlier citation of this exact BDPT code ("does not share this pattern... always uses the aggregate `PathValueOps::EvalBSDFAtVertex`... for its non-delta throughput") as evidence BDPT is unaffected by anything in this area -- that statement is accurate about DL-42's specific pattern (a dropped `selectProb` division under guiding) but was read too broadly; it is not evidence against DL-69, which is this file's ordinary (non-guiding) throughput construction. | M | physics-bias | user-visible (BDPT/VCM/MLT renders of any multi-lobe material with overlapping-hemisphere lobes, e.g. `schlick_material`) |
| DL-98 | Found during DL-67 Slice 0 sibling audit (debt-slice0 worktree, 2026-09-14) | `IsotropicPhongSPF::Pdf`/`PdfNM` weight the diffuse-vs-specular mixture by the raw, angle-independent `MaxValue(rd)`/`MaxValue(rs)` painter albedos, the same shape of mismatch DL-67 Slice 0 fixed in `SchlickSPF` | OPEN-confirmed (static; same bug pattern, unfixed) | `IsotropicPhongSPF::Scatter`/`ScatterNM` (`IsotropicPhongSPF.cpp:141-142`) compute `specular.kray = pRs->GetColor(ri) * ((N+2.0)/(N+1.0)) * r_max(cos_o,0.0)`, `cos_o = dot(specular.ray.Dir(), n)` -- a function of the SPECULAR lobe's own drawn direction, exactly analogous to `SchlickSPF`'s fresnel-boosted specular kray. `diffuse.kray = pRd->GetColor(ri)` (`:184`) is direction-independent, also exactly analogous. `PTScatterSelectWeight`/`PTRandomlySelect` (`PathTracingIntegrator.cpp:1338-1356`) select between these two REALIZED krays the same way for every SPF. But `IsotropicPhongSPF::Pdf`/`PdfNM` (`:293-294`, `:317-318`) weight by `dWeight=MaxValue(rd)` / `sWeight=MaxValue(rs)` -- the raw painter albedo, missing the `(N+2)/(N+1)*cos_o` term entirely -- so the specular coefficient is wrong at every `wo` (not just non-normal incidence: even head-on the `(N+2)/(N+1)` scale factor alone is missing). Unlike `SchlickSPF`'s fix, no averaging approximation is even needed here for the diffuse side (it is already direction-independent and exactly `MaxValue(rd)`); only the specular coefficient needs the exact-at-`wo` correction. `tests/SPFPdfConsistencyTest.cpp`'s existing `IsotropicPhong` row documents "massive cross-val divergence (38k-45k mismatches out of 50k samples)" from this exact cause. **Recipe (REWRITTEN 2026-09-14 — the original prescription here was itself defective and is withdrawn).** The withdrawn text said to fix `sWeight` to the exact-at-`wo` value and leave `dWeight = MaxValue(rd)` as "already exact, no averaging needed", i.e. a diffuse coefficient `dW/(dW+sW(wo))` that VARIES WITH THE QUERY DIRECTION. That is wrong for the same reason DL-67 Slice 0's first attempt was: the diffuse lobe's selection probability is decided by the SPECULAR lobe's independent draw, not by the direction the diffuse lobe happened to produce, so its coefficient is a CONSTANT expectation and can never be direction-dependent. The correct construction (see [DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md) §1-§2) is `C_D = E_{(u,v)~U[0,1]^2}[P(the diffuse ray wins | that specular draw)]`, equivalently `1 - E_{omega_S~p_S}[q_S(omega_S) 1{accepted}]`, estimated by a DETERMINISTIC stratified quadrature that replays `IsotropicPhongSPF`'s own specular sampler (`GenerateSpecularRay`) in its own inverse-CDF parametrisation, reproducing its accept-checks and `RandomlySelect`'s `freeidx==1` short-circuit; `q_S(wo)` alone is exact at the query direction, as `sWeight = MaxValue(rs)*(N+2)/(N+1)*r_max(cos_o(wo),0)`. Note that the specular lobe's REJECTION rate is expected to dominate `C_D` here as it does in Schlick. Red-prove with the two gates `tests/SchlickSPFPdfConsistencyTest.cpp` now uses — a two-sided `|int Pdf - P(Scatter emits anything)| < 1%` hemisphere-quadrature gate, and a total-variation gate against a 600k-draw histogram of real `Scatter`+`RandomlySelect` draws with a threshold derived from the bin noise — NOT the per-lobe lower-bound check that file used to carry, which is an algebraic identity post-fix and was deleted. While there, check `IsotropicPhongSPF::Pdf`'s use of a 3-channel AVERAGE exponent (`Navg`) against `Scatter`'s per-branch `N[0]` / per-channel `N[i]`, the same branch-parity defect Slice 0 fixed in Schlick. See [DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md) §8. | S | physics-bias | user-visible (`phong_material` guided/BDPT/VCM renders and NEE partner MIS weight) |
| DL-99 | Found during DL-67 Slice 0 sibling audit (debt-slice0 worktree, 2026-09-14) | `AshikminShirleyAnisotropicPhongSPF::Pdf`/`PdfNM`'s lobe weights are not the realized selection weights `Scatter` produces: beyond evaluating at the mirror direction, they carry a SPURIOUS `Rs` factor, DROP the `cos_o` factor, and use a diffuse weight that is missing `(1-Rs)` and carries a spurious `1/pi` | OPEN-confirmed (static; partially-mitigated variant of the same pattern, unfixed) | **Description CORRECTED 2026-09-14** — the original row called this "an approximation" (mirror-direction evaluation of an otherwise-right formula). It is not: the weights are wrong by FACTORS, independently of which direction they are evaluated at. Verified by reading both functions. (a) `AshikminShirleyAnisotropicPhongSPF::Pdf` (symbol `Pdf`, the `wSpec` assignment) computes `wSpec = rs_val * specFactor_m`, but `Scatter` (symbol `Scatter`, the `specular.kray` assignment in the non-per-channel branch) sets `specular.kray = RISEPel(1,1,1) * specFactor * cos_o` with NO `Rs` factor -- its own comment says so explicitly ("specularFactor already includes Fresnel (which contains Rs), so no extra Rs multiplication"). So `Pdf` multiplies by `Rs` where `Scatter` does not. (b) `Pdf` also drops the `* cos_o` that `Scatter`'s kray carries. (c) `wDiff = MaxValue(Rd) * (28/(23*pi)) * fromK(cos_i)^2`, but `Scatter`'s `diffuse.kray = Rd * (1-Rs) * (28/23) * fromK1(cos_o) * fromK2(cos_i)` -- so `Pdf` is missing the `(1-Rs)` factor entirely and carries a spurious `1/pi` (the pi cancels against the cosine pdf in the kray derivation and must not reappear in a selection weight), on top of using `fromK(cos_i)^2` where the realized weight uses `fromK1(cos_o)*fromK2(cos_i)`. The mirror-direction evaluation is a separate and much smaller issue than any of these. `tests/SPFPdfConsistencyTest.cpp`'s existing `AshikminShirleyAnisotropicPhong` row still documents "large mismatch (16k-43k)". `tests/SPFPdfConsistencyTest.cpp`'s existing `AshikminShirleyAnisotropicPhong` row still documents "large mismatch (16k-43k)" despite this existing mitigation. Recipe: the same two gates DL-98's rewritten recipe names (two-sided `int Pdf` vs the measured emission probability; TVD against a real `Scatter`+`RandomlySelect` histogram), applied to `AshikminShirleyAnisotropicPhongSPF`. Fix (a)/(b)/(c) first -- they are factor errors and need no new machinery -- then reconstruct `specFactor`/`diffFactor` at the ACTUAL query `wo` and give the diffuse lobe a CONSTANT `C_D` expectation rather than a direction-dependent coefficient (see DL-98's recipe for why). NOT lower priority than DL-98, as this row previously claimed on the strength of the withdrawn "deliberate partial mitigation" reading. See [DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md) §8. | S-M | physics-bias | user-visible (`ashikmin_shirley_material`, lower severity than DL-98) |
| DL-100 | Found during DL-67 Slice 0 fix pass (debt-slice0, 2026-09-14) | `SchlickSPF`'s specular sampler builds its half-vector in the UNFLIPPED `ri.onb`, so on a back-face hit every specular draw is rejected by `Scatter`'s own accept-check and the material silently loses its entire specular lobe | OPEN-confirmed (static; sampler-side energy loss, deliberately NOT fixed in Slice 0) | `SchlickSPF.cpp`'s `GenerateSpecularRay` took an `onb` parameter and never used it: the half-vector was always built from `ri.onb.u()/v()/w()`, while `Scatter`/`ScatterNM` pass the FlipW'd `myonb` and then accept-check against `myonb.w()`. On a back-face hit (`dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO`, i.e. single-sided geometry hit from behind -- double-sided meshes pre-flip `vNormal`, so they never reach it) the two frames differ by sign, the sampled `h` sits in the wrong hemisphere, and `dot(dir, myonb.w()) > 0` fails for every draw. Result: a back-face `schlick_material` hit scatters diffuse-only, at full diffuse weight, with no specular energy at all. Slice 0 REMOVED the dead parameter (so the frame in use is now explicit) and made `ComputeSchlickSpecularPdf` use `ri.onb` too, which makes the DENSITY correct for the sampler as it stands -- it returns 0 there, exactly matching what `Scatter` emits. The sampler itself is untouched, because fixing it changes what renders sample and needs its own red-proof. Recipe: build a single-sided geometry (e.g. `ClippedPlaneGeometry`, or a non-double-sided mesh) with a `schlick_material`, put the camera behind it, and count `ScatteredRay`s of type `eRayReflection` out of `Scatter` at a back-face hit (expect 0 pre-fix); fix by passing the caller's `myonb` into `SchlickSampleHalfVector` instead of reading `ri.onb`, then re-point `ComputeSchlickSpecularPdf` at the same frame and re-run `SchlickSPFPdfConsistencyTest` (which must stay green -- it exercises front-face hits only) plus a new back-face row. | S | energy-loss | user-visible (back-face `schlick_material` hits: specular lobe entirely absent) |
| DL-101 | Found during DL-67 Slice 0 fix pass (debt-slice0, 2026-09-14) | The per-channel specular loop in `SchlickSPF` and both Ward SPFs REUSES one `ScatteredRay` across its three lanes, so a lane whose sampler declines to write a direction pushes the PREVIOUS lane's direction carrying this lane's kray and pdf | OPEN-confirmed (static; distinct pattern from DL-67, unfixed) | `SchlickSPF::Scatter` declares `ScatteredRay d, s;` once and its per-channel branch calls `GenerateSpecularRay(s, ...)` three times in a loop. `GenerateSpecularRay` writes `specular.ray` ONLY when `hdotk > 0`; otherwise the ray is left untouched. On lane 0 that is harmless (a default-constructed `Ray` has direction `(0,0,0)`, so the accept-check rejects it), but on lanes 1 and 2 the untouched ray still holds the PREVIOUS lane's direction -- which generally passes the accept-check -- and the branch then assigns lane `i`'s `kray[i]` and lane `i`'s `ComputeSchlickSpecularPdf(..., rt.v[i], it.v[i])` to it. The container receives a ray at lane `j`'s direction priced as lane `i`. `hdotk <= 0` is reachable for any roughness (the theta warp reaches `pi/2` as `xi -> 0`) and becomes common at grazing incidence with high roughness. Same code shape, verified by reading: `WardIsotropicGaussianSPF::Scatter` and `WardAnisotropicEllipticalGaussianSPF::Scatter` (both conditionally `ray.Set` inside `if(hdotk>0)` and both reuse one `s` across the per-channel loop). NOT present in `IsotropicPhongSPF` (its generator sets the ray unconditionally) or `AshikminShirleyAnisotropicPhongSPF` (its generator returns `false` and the caller gates on it). Slice 0's `Pdf` quadrature models the INTENDED semantics (a lane whose `hdotk <= 0` is rejected), so closing this row removes a modelling gap as well as the sampler bug. Recipe: instrument the per-channel branch to count pushes whose direction is bit-identical to the previous lane's while `hdotk <= 0` for the current lane, on a grazing high-roughness fixture with an `RGBScalarPainter` roughness; fix by declaring the `ScatteredRay` INSIDE the loop (or having `GenerateSpecularRay` return a bool the caller gates on, as `AshikminShirleyAnisotropicPhongSPF` already does) in all three files. | S | physics-bias | user-visible (`schlick_material`/`ward_*_material` with a per-channel roughness painter, at grazing incidence) |
| ~~DL-102~~ | ~~Found during DL-67 Slice 0 fix pass (debt-slice0, 2026-09-14)~~ CLOSED 2026-09-14 — `eb328217`, `SchlickSPFPdfConsistencyTest` `Checks: 34 Failures: 0` | ~~`ComputeSchlickSpecularPdf`'s azimuthal density for `isotropy != 1` was a DIFFERENT DISTRIBUTION from the one `GenerateSpecularRay` draws -- not a mis-scaled version of it~~ The density now matches the sampler's own warp | CLOSED-by-test | The helper returned `p/(2 pi (cos^2 phi + p^2 sin^2 phi))`, with a comment asserting it was "the phi PDF (from the sampling inversion)". The sampler draws, per quadrant, `phi = (pi/2) sqrt(p^2 v^2/(1 - v^2 + v^2 p^2))` with `v ~ U[0,1)`; inverting, `v = t/sqrt(p^2 + t^2(1-p^2))` with `t := phi/(pi/2)` measured from the quadrant's own axis, so the true density is `p^2/(2 pi (p^2 + t^2(1-p^2))^(3/2))`. Both forms integrate to 1 over `[0,2pi)` and both agree at `p=1`, which is how it survived. Against a 2 000 000-draw histogram of the real sampler the shipped form was off by up to 25x (`p=0.3`, `phi=1.702`: 0.4526 vs 0.0181 true; `p=0.6`, same `phi`: 0.2575 vs 0.0674); the corrected form matches the histogram to MC noise at `p` = 1, 0.8, 0.6 and 0.3. Gated by `SchlickSPFPdfConsistencyTest`'s anisotropic rows (isotropy 0.6 and 0.8) via both the normalisation and total-variation gates. Filed as its own row rather than folded into DL-67 because it is a different bug with a different mechanism, and a future reader looking for "why did anisotropic Schlick MIS weights change" needs to find it. | S | physics-bias | user-visible (`schlick_material` with `isotropy != 1`: NEE/BDPT/VCM MIS weights) |
| ~~DL-03~~ | ~~RENDERING_INTEGRATORS.md debt 31 item 1 / REFRACTIVE_RADIANCE_SCALING.md §10.3~~ CLOSED 2026-09-12 — `8a9bdb18`, `TranslucentIORStackTest: ALL TESTS PASSED` | ~~Guided translucent exits lose their popped IOR stack~~ An available selected exit transition is preserved or rejected according to the accepted direction and shared by training/eta consumers; missing entry-state generation remains DL-47 | CLOSED-by-test | Red on unfixed `00bdcef5`: four failed assertions (`d3a5e732`); real trained PT RGB/NM outward substitutions, inward controls and later same-object classification. Additional BDPT eye/light RGB/NM coverage; eye RIS actual-guide limitation remains DL-43. See `DL03_GUIDED_IOR_CONTINUATION.md`. | M | physics-bias | user-visible (eligible guided translucent continuations) |
| ~~DL-04~~ | ~~REFRACTIVE_RADIANCE_SCALING.md §10.1~~ | ~~Unsettled extra eta-square factor for complete SSS events~~ No unmatched factor belongs on the exterior-to-same-exterior event | CLOSED 2026-09-12 (consistency pin) | `1b705ce1`: SSSRadianceScalingTest unchanged-library baseline 572093 checks, 0 failures (both deliberate eta directions fail all six SSS air-channel checks); the baseline moved to **574017** checks, 0 failures after DL-52's probe fix started reaching additional entry points (`00cd6723`, 2026-09-13) and is unchanged by DL-71/DL-72. Independent helper plus matched explicit-volume/diffusion/RW camera matrix; of the normalization/non-air support/MIS defects originally tracked as DL-48 through DL-53, only **DL-49** (non-air relative-index) remains open. See [decision](DL04_SSS_RADIANCE_DECISION.md). | M | physics-bias | convention pinned; separate SSS defects remain open |
| ~~DL-34~~ | ~~CROSS_OBJECT_PROXIMITY_DESIGN.md §10~~ | ~~Published two-sphere union overlap exports only the deeper operand depth~~ | CLOSED 2026-09-12 (recorded regression; conservative contract retained) | `cffa254f`: unchanged ProximitySignalTest red `Passed: 465   Failed: 6`, fixed `Passed: 471   Failed: 0`; supplemental final `Passed: 491   Failed: 0`. Certified inscribed-ball union recovers `sqrt(3.56)` through signed field, manager and interior signal. The original proposed max-depth edit was already the existing signed-min magnitude; arbitrary unions remain non-exact. See [closure and limits](DL34_UNION_INTERIOR_DEPTH.md). | M | physics-bias | user-visible (published union seam regression) |
| ~~DL-37~~ | ~~IMPROVEMENTS.md: GGX low-F0 grazing gain~~ | ~~Angle-flat diffuse split creates grazing gain~~ | CLOSED 2026-09-12 | `000df0b4`: LayeredWhiteFurnaceTest reports `0 of 57 configurations failed` (red: four failed configs). Reciprocal entry/exit transmission covers RGB/NM Schlick, conductor and film. Independent sweep: 150/46 before, 150/3 after; retained specular-only failures are DL-63. See [scope](DL37_GGX_DIFFUSE_TRANSMISSION.md). | M | physics-bias | user-visible |
| ~~DL-42~~ | ~~DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals~~ CLOSED 2026-09-13 — `PathTracingIntegrator.cpp` fix, `PTGuidedSelectProbTest: ALL TESTS PASSED` | ~~PT's BSDF-surviving one-sample guiding branch drops selected-lobe probability compensation~~ The kept-BSDF-direction one-sample branch now divides by `selectProb` too: `scatterThroughput = kray * pS->pdf / (selectProb * combinedPdf)` | CLOSED-by-test | Red-proved with a real multi-lobe `TranslucentMaterial` exit (diffuse-exit + translucent-backscatter lobes, unequal weights via `scattering`), a real trained `PathGuidingField` (`guidingAlpha=1e-6`, engaging the trained-guiding code path while making the guided-direction coin flip land on "keep BSDF direction" on ~4000/4000 trials), and a constant-radiance "sky" continuation with no LightManager (isolating the measured `IntegrateFromHit`/`IntegrateFromHitNM` return to exactly `scatterThroughput * skyRadiance`, no NEE, no area-light MIS reweight). Pre-fix: every diffuse-exit-selected trial (2586/2586 and 3588/3588 across two scattering splits, RGB and NM) matched the buggy `kray*pdf/combinedPdf` closed form and NONE matched the correct one. Post-fix: the reverse, exactly. `GGXBRDF::value()`/`GGXSPF::Pdf()` (`return diffuse+specular;` / "3-lobe mixture PDF") confirmed the OTHER two `PTMulDiv` overwrite sites in the same block (RIS-accepted candidate, one-sample guided-direction-accepted) re-evaluate the material's AGGREGATE BSDF/PDF fresh at the priced direction. **Correction (2026-09-13, same slice, before merge): this row originally continued "...and are therefore already complete, self-contained estimators that must NOT also divide by selectProb -- applying the fix there would double-count." That claim is FALSE and is withdrawn.** `GuidingSupportsSurfaceSampling` admits only non-delta eRayDiffuse/eRayReflection lobes, so at a multi-lobe surface with an ineligible lobe (e.g. this row's own `TranslucentMaterial` EXIT fixture: diffuse-exit eligible, translucent-backscatter not) the aggregate-BSDF branches price ALL lobes while guiding only ever proposed/weighted the eligible subset, and the ineligible lobe is priced AGAIN through the ordinary kray/selectProb path -- and even restricted to an all-eligible material, per-lobe kray/pdf (this fix) and aggregate BSDF/pdf (the other two branches) are two different, mutually-inconsistent RIS/one-sample proposal measures unless the SPF has only one lobe. This is a real, still-open, DIFFERENT bug from the one this row fixes; it is filed as DL-67 (renumbered from this slice's original DL-65 at merge time to avoid colliding with concurrently-filed rows from sibling debt-cleanup slices), not fixed here. Confirmed `ggx_material`/`coated_material` cannot exercise EITHER bug (this row's or DL-67's) at all (`GGXSPF`/`CoatedSPF` each do internal single-lobe selection, emitting exactly one `ScatteredRay` per call, so the outer `selectProb` is trivially 1 for either). `BDPTIntegrator.cpp`'s analogous "kept BSDF direction" one-sample fallback (eye ~2662, light ~6363-ish) does not share DL-42's pattern (a dropped `selectProb` division): it unconditionally folds `selectProb` into `scatterPdf = selectProb*effectivePdf`. It DOES, however, always use the aggregate `PathValueOps::EvalBSDFAtVertex` paired with a PER-LOBE `effectivePdf`, never `pScat->kray` directly, for its non-delta throughput -- a THIRD, independent measure-mismatch pattern (aggregate f over per-lobe pdf, an over-count on overlapping-support multi-lobe materials rather than an under-count), filed as DL-69, not fixed here. **HWSS correction: PT's HWSS body (`IntegrateFromHitHWSS`) contains no guiding block at all (verified: zero occurrences of `pGuidingField`/`GuidingCombinedPdf`/`GuidingRIS` in that function) -- it is unaffected by this fix, not "covered via the shared RGB/NM template" as an earlier version of this row's detailed section claimed; HWSS reaches the fixed line only indirectly, via its per-wavelength fallback calls into `IntegrateFromHitNM` for BSDF materials.** | M | physics-bias | user-visible (path guiding and mixed-lobe materials) |
| ~~DL-43~~ | ~~DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals~~ CLOSED 2026-09-13 — `a69c9ce6`, `TranslucentIORStackTest: ALL TESTS PASSED` | ~~BDPT eye guiding swaps incoming/outgoing directions when evaluating forward candidate PDFs~~ Eye-subpath RIS candidate-1 and one-sample guiding now pass `(-currentRay.Dir(), candidateDirection)` to `PathValueOps::EvalPdfAtVertex`, matching the light-subpath twins and the wrapper's Pdf(outgoing\|incoming) contract | CLOSED-by-test | Red on unfixed HEAD (`42f3dc97`): eye RIS `substituted_out=0` (rejected every guide candidate) and eye one-sample `bad_pdf_value=41` (all 41 outward substitutions evaluated the density of the wrong direction); light-subpath rows already passed. Fixed both call sites (RIS candidate 1, one-sample), each compiled for PelTag/NMTag via the shared `GenerateEyeSubpathImpl` template. Green: eye RIS `substituted_out=13`, all `bad_pdf_value=0`. Sibling audit: the ~14 other `EvalPdfAtVertex` call sites in `BDPTIntegrator.cpp` are Veach MIS reverse-density/connection-strategy conversions (deliberately query the opposite direction) and already use the correct order; `PathTracingIntegrator.cpp` has no `EvalPdfAtVertex` calls (PT's guiding uses the single-argument `PTEvalPdfAtSurface`/`EvalPdfAtSurface` against an already-real `RayIntersectionGeometric`, architecturally immune to this swap). See `PathValueOpsTest.cpp` Test G for the closed-form derivation. | M | physics-bias | user-visible (BDPT eye path guiding) |
| ~~DL-39~~ | ~~DL01_TRANSLUCENT_EXIT_WEIGHT.md: independent residuals~~ | ~~Dedicated translucent photon deposition counts absorbed power as deposited power~~ | CLOSED 2026-09-13 — `1fe5c760`, `TranslucentPhotonEnergyTest` 8 checks / 0 failures (was 6 failures) | `1fe5c760`: `TracePhoton` now sums the diffuse lobe's own kray directly and deposits `power*diffuse_kray` instead of `power*(1-accum_scattered)`. Red-proof against the unfixed tracer: deposited power read the full unattenuated `(1,1,1)` at extinction 0.1 and 1 alike. Post-fix: deposited power equals `incoming*exp(-extinction*distance)*(1-scattering)` exactly, and `absorbed_frac+deposited_frac+traced_frac==1`. See [DL01_TRANSLUCENT_EXIT_WEIGHT.md](DL01_TRANSLUCENT_EXIT_WEIGHT.md). **Scope correction (review round 3, 2026-09-13, `fb2f75af`):** a round-2 revision had generalised the "sum the diffuse lobe's own kray" rule to EVERY diffuse deposit and asserted that as "a genuine accuracy improvement". That was wrong. `TranslucentPelPhotonMap::RadianceEstimate` is the Jensen estimator (`sum(power_i) * brdf.value(...) / (pi*r^2)`), so the STORED quantity must be the flux ARRIVING at the surface — the gather applies that surface's own BSDF itself. Depositing the diffuse kray is correct ONLY where that kray is a transport attenuation, i.e. at a translucent interior exit (`Beer * (1-scattering)`); a Lambertian wall's `eRayDiffuse` kray IS its albedo (`LambertianSPF.cpp`) and TranslucentSPF's own ENTERING-branch diffuse kray is `pRefFront`, a reflectance — storing either makes the gather read `power * albedo^2`. `TracePhoton` now detects the translucent exit via `GetSpecularInfo().hasInterior && ior_stack.containsCurrent()` (captured BEFORE the trace loop, since a recursive call that inherits the same stack re-points its current object) and keeps the pre-DL-39 `power*(1-accum_scattered)` formula at every other hit. `TranslucentPhotonEnergyTest` sub-test 3 now asserts the FULL arriving power at the Lambertian wall; red-proof against the round-2 tracer: `got (0.7,0.5,0.3), expected (1,1,1)`. Test prints `TranslucentPhotonEnergyTest: 14 checks, 0 failures`. | M | physics-bias | user-visible (translucent photon maps) |
| DL-44 | DL36_EMITTER_NEIGHBOUR_PIN.md: review residual | Sampled emitter UV is omitted from LightSample and downstream rebuilt emission records | OPEN-confirmed (static evidence; red-proof pending) | `LightSampler.cpp::SampleLight` sets local RGB `rig.ptCoord = coord`, but `LightSample` carries no UV. BDPT NM/HWSS emission rebuilds and LIGHT root, and VCM sampled-emitter evaluation retain default (0,0); `CheckerPainter` consumes ptCoord. MLT shares BDPT generation. | M | physics-bias | user-visible (UV-textured luminaries under bidirectional/spectral transport) |
| ~~DL-45~~ | ~~DL03_GUIDED_IOR_CONTINUATION.md: tilted-frame residual~~ | ~~TranslucentSPF samples geometrically inward diffuse exits under tilted shading normals and still pops the IOR stack~~ | CLOSED 2026-09-13 — `14b06223`, `TranslucentTiltedExitTest` 33 checks failed pre-fix / all pass post-fix; `TranslucentIORStackTest` 20 checks failed pre-fix / all pass post-fix | `14b06223`: `Scatter`/`ScatterNM`'s exit branch now resamples until the direction is also geometrically valid (`dot(wo,geomNRaw)>0`, the object's unflipped outward direction) and reports the matching NORMALIZED conditional density `cos(theta)/pi / ExitValidFraction(n,geomNRaw)`, `ExitValidFraction = (1+cos(phi))/2` (Malley's-method closed form). Red-proof matched the historically-cited 1021/4096 figure at 60-degree tilt exactly. A sibling ray-anchor bug was found and fixed while wiring the gate: the entry-lobe's ray-anchored `geomN` is the WRONG reference for the exit case (it lands on the inward direction for a ray traveling outward); the exit gate uses the unflipped `geomNRaw` instead. **Superseded 2026-09-13 (same debt-cleanup slice, P1)**: the resample loop above drew a variable 2..64 sampler dimensions per call, which corrupts a fixed-dimension-budget sampler's (`SobolSampler`) later draws in the same bounce phase; it was replaced with an exact, unconditional two-draw closed-form remap onto the same `ExitValidFraction`-normalized density (no rejection, same numbers) — see `SampleValidDiffuseExit`, `TranslucentSPF.cpp`. **Also (same slice, P2-1)**: `geomNRaw`'s "unflipped, unconditionally outward" framing above was itself incomplete — `ri.vGeomNormal` is flipped to face the ray on a double-sided triangle mesh, so the exit gate needs the documented `bGeomNormalOrientedToRay` un-flip to actually reach the object's true outward direction on such a mesh; fixed in the same pass. See [DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md). | M | physics-bias | user-visible (translucent materials with perturbed shading normals) |
| ~~DL-46~~ | ~~DL03_GUIDED_IOR_CONTINUATION.md: initial-containment residual~~ | ~~Camera/light origins inside closed translucent objects lack initial IOR-stack membership and misclassify their first exit as entry~~ | CLOSED 2026-09-13 — `cd43da09`, `TranslucentInitialContainmentTest` 20/20 checks pass | `cd43da09`: `SpecularInfo` gained a `hasInterior` flag for a stateful-but-non-refracting material; `TranslucentMaterial::GetSpecularInfo` reports it; `IORStackSeeding::SeedFromPoint` accepts `canRefract||hasInterior` and re-pushes the caller's current stack top (not a captured constant) for a `hasInterior`-only entry, preserving a nested enclosure's numeric IOR exactly. See [DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md). | M | physics-bias | user-visible (origins inside closed translucent objects) |
| ~~DL-52~~ | ~~DL04_SSS_RADIANCE_DECISION.md: planar probe origin~~ | ~~BSSRDF entry probes skip nearby points on a flat surface by advancing from the projection plane before intersecting~~ | CLOSED 2026-09-13 — `00cd6723`, `BSSRDFPlanarProbeReachTest` | Unfixed: normal-axis probe on a real `InfinitePlaneGeometry` found 0/500 entry points (curved-sphere control 372/500). `BSSRDFSampling::SampleEntryPoint` now traces a single chord starting `probeMaxDist` before the projection plane and travelling through it (PBRT `SeparableBSSRDF::Sample_Sp` convention), instead of two half-lines starting ON the plane. Fixed: 500/500 reached, all coplanar with the exit point; control unchanged at 372/500. `BSSRDFProjectionNormalTest`'s own hand-copied probe oracle updated to the same chord shape to stay in lockstep (unchanged results: active=48/128, axes=33/10/5); `BSSRDFNormalizationTest`, `BSSRDFSamplingTest`, `SSSRadianceScalingTest` unaffected. See [DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md](DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md). | M | physics-bias | user-visible (diffusion SSS on planar or nearly planar geometry) |
| ~~DL-53~~ | ~~DL04_SSS_RADIANCE_DECISION.md: recursive environment MIS~~ | ~~Recursive RayCaster misses bypass environment MIS when passed the global map explicitly~~ | CLOSED 2026-09-13 — `b3de184d`, `RayCasterEnvEscapeMISTest` | Unfixed: `RayCaster::CastRay`/`CastRayNM`/`CastRayHWSS`'s explicit-`pRadianceMap` escape branch returned raw radiance regardless of `bsdfPdf` (48/79 checks fail: e.g. RGB `bsdfPdf=0.1` expected 0.474006, got 0.7 — the unweighted raw value). Fixed by a shared `RayCasterEnvEscapeMISWeight` helper applied in that branch too, gated on the resolved map being pointer-identical to `pScene->GetGlobalRadianceMap()` (the MIS PARTNER RULE, mirroring the PT-integrator fix in [PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md)); a genuinely distinct per-object override map keeps full weight, `bsdfPdf==0` is unaffected. Fixed: 79/79 pass at the time of this fix; a later same-day commit (`0eb7a47e`) added partition-of-unity checks, so the CURRENT total is 91/91 (re-verified this pass, P3-2) — quote 91, not 79, when citing this test's count going forward. Affects the SSS/RW-SSS continuation call sites in `PathTracingIntegrator.cpp` and the internal volume phase-scatter continuation in `RayCaster.cpp` itself (no call-site edits needed — the fix is at the shared layer); BDPT's `RecordGuidingTrainingSampleNM` guiding-training helper also passes through the same fixed branch. See [DL53_RAYCASTER_ENV_ESCAPE_MIS.md](DL53_RAYCASTER_ENV_ESCAPE_MIS.md). | M | physics-bias | user-visible (SSS and eligible recursive environment continuations) |
| ~~DL-71~~ | ~~DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md: probe entry-normal orientation~~ | ~~DL-52's single-chord probe reports an inverted entry normal on near-half hits for geometry that orients normals toward the incoming ray~~ | CLOSED 2026-09-13 — `e416d3bd`, `BSSRDFPlanarProbeReachTest`; **round-2 correction same day** (positional gate replaced with an unconditional rule; see evidence) | Round 1: a coplanar double-sided `TriangleMeshGeometry` quad and `ClippedPlaneGeometry` both returned `outward 0/500  neePositive 0/500`; fixed by negating `h.normal`/`h.geomNormal` for near-half hits only (`distFromChordStart < probeMaxDist`). **Round 2 (this pass) found the near-half gate ITSELF wrong**: the flip predicate is a per-hit fact about approach side, independent of chord position, so a far-half hit on flip-oriented geometry can ALSO need correction and round 1 silently skipped it — new fixtures (`MakeDoubleSidedQuadMeshAtZ`, a far-half quad; `MakeDoubleSidedWallMeshAtX` + tangent-axis forcing, exercising the previously-100%-untested tangent/bitangent branch) reproduced `outward 0/500  neePositive 0/500` on round-1 code. Fixed by applying `oriented ? -raw : raw` UNCONDITIONALLY to `h.geomNormal`, gated only on `bGeomNormalOrientedToRay && !bGeomNormalRayDerived` (the latter a new field, DL-75, excluding `HairGeometry`'s fabricated ray-derived orientation from the recovery — hair has no genuine second side to recover). Also fixed in the same pass (P2-A, found reviewing the far-half fixture): the SHADING normal's independent flip predicate can disagree with the geometric normal's at a grazing crossing, so round 1's lockstep negation of both could leave them in opposite hemispheres; now the shading normal is oriented into the corrected geometric normal's hemisphere instead. Fixed: `outward 500/500  neePositive 500/500` on all six fixtures (2 round-1 coplanar + 2 round-2 far-half/tangent + the pre-existing plane/sphere controls). See [DL71_BSSRDF_PROBE_ENTRY_NORMAL.md](DL71_BSSRDF_PROBE_ENTRY_NORMAL.md) "Round-2 correction". | M | physics-bias | user-visible (SSS entry-point NEE/continuation on double-sided planar geometry; round 2 additionally covers concave/multi-surface far-half crossings and the tangent/bitangent probe axes) |
| ~~DL-72~~ | ~~DL53_RAYCASTER_ENV_ESCAPE_MIS.md: optimal-MIS training gap~~ | ~~REOPENED round-2 (P2-B): the round-1 wiring itself has a moment/count pairing defect~~ | CLOSED 2026-09-14 (round 3, debt-guiding2 slice) | Volume sites (`RayCaster.cpp` RGB+NM `CastRay`/`CastRayNM`): `rs2.bsdfTimesCos = RISEPel(phasePdf,phasePdf,phasePdf)` (was `RISEPel(0,0,0)`) -- correct per `IPhaseFunction::Pdf()==Evaluate()` and no separate cosine term for a volume vertex -- paired with a new `AccumulateCount` call. BSSRDF sites (`PathTracingIntegrator.cpp`, both continuations, both tags): new `PTBssrdfSwTimesCos` helper recovers `Sw*cos = (weight/weightSpatial)*cosinePdf` algebraically from the already-computed `bssrdfWeight`/`bssrdfWeightSpatial` split (no BSSRDF-file edits), paired with a new `AccumulateCount` before Russian roulette. Red-proof, corrected 2026-09-14 (review round 2) -- the earlier wording called `OptimalMISAccumulatorTest`'s Test 12 the red-proof, which it is not: Test 12 (`TestDL72PairedTrainingFires`, 34/0 total, was 27/0) drives the accumulator's PUBLIC API by hand and hand-derives `alpha=0.5` unwired -> `alpha=1/3` buggy (unpaired) -> `alpha=4/10.4` fixed (paired), which pins the ACCUMULATOR'S ARITHMETIC but executes no line of `RayCaster.cpp` or `PathTracingIntegrator.cpp` and so is blind to whether either file calls the API at all -- exactly what both DL-72 regressions were. The real red-proof is the new `tests/OptimalMISTrainingSitesTest.cpp`, which runs BOTH production sites (an optically thin global fog under an environment; a real `SubSurfaceScatteringMaterial` on a real sphere `Object` through production `IntegrateFromHit`) and reads `Solve()`'s branch table back: with the two files reverted to `ddf05c6c` (the round-2 "leave the arm UNWIRED" state) the volume site reads exactly clampMin **0.001** and the BSSRDF site exactly clampMax **0.999**; wired, they read **0.857** and **0.929** -- 7/0. P2-5 (review round 2): the volume sites additionally recorded the moment against the RAW `phasePdf` while the direction came from the guided mixture; `bsdfPdf` is now `effectivePdf` (the true density) and the raw phase pdf moved to the new `bsdfMisPdf` field, where DL-73 requires it -- see the DL-74 row. Gate: `RayCasterEnvEscapeMISTest` 91/91, `SSSRadianceScalingTest` 574017/0, `MISWeightsTest` 59/0, `RasterizerDefaultsConsistencyTest` 164/0, all unchanged; `make -C build/make/rise -j8 all` clean. Review round 3 (same slice): the red-proof asserted only `Solve()`'s alpha, which is a RATIO and so is unmoved by a `bsdfTimesCos` scaled by any constant -- precisely this row's failure shape. `OptimalMISAccumulator` gained a const `GetTileTraining()` accessor and `OptimalMISTrainingSitesTest` now asserts the trained QUANTITY: at the volume site every escape contributes exactly `(L_env*phasePdf)^2/phasePdf^2 = L_env^2`, so the accumulated sum must be a whole multiple of `L_env^2` not exceeding the attempt count (measured **484 x L_env^2 over 726 attempts**), and at BOTH sites re-running identical seeds under a 3x brighter uniform environment must give exactly **9x** (measured 9 and 9). Red-proved by mutating both volume sites to `rs2.bsdfTimesCos = RISEPel(1,1,1)`: sum inflates to **76430.2 = 484 x (4*PI)^2**, two assertions fail (17/2), while the alpha assertion stays green. 19/0 after. **Review round 4 (same slice) corrected the trained QUANTITY itself.** `Sw*cos` is the BSSRDF exit's integrand with the AREA-measure `weightSpatial = Rd*Ft(exit)/pdfSurface` DROPPED, and `LightSampler`'s entry-NEE arm dropped the identical factor from the other side of the same pair (the caller applies it after the call returns); the accumulator's moment is of the INTEGRAND (Kondapaneni 2019), so both halves were scaled by an arbitrary, non-O(1) amount and skewed the alpha of every tile an SSS surface touches. Since `weight = weightSpatial*Sw*PI` and `cosinePdf = cos/PI`, the integrand is exactly `weight*cosinePdf` -- a multiply, not a ratio, so `PTBssrdfSwTimesCos` becomes `PTBssrdfTrainedBsdfTimesCos(weight, cosinePdf)`, its Rd==0 guard disappears and the Pel form stays per-channel instead of collapsing to `MaxValue()`. `EvaluateDirectLighting{,NM}` gain a trailing `neeTrainingScale` (default 1) that scales the TRAINING integrand only, never the returned radiance; the two BSSRDF/random-walk entry-NEE call sites pass `PTSurvivalMagnitude(bssrdfWeightSpatial)`. The alternative offered in review -- leaving the site untrained, as DL-84 leaves PT's in-loop volume site -- was rejected because it silences the BSDF half while the NEE half, governed by a per-scene accumulator pointer rather than a per-call flag, goes on training the same mis-scaled moment UNPAIRED. Red-proof (the `L_env^2` law cannot see this -- a radiance scale multiplies right and wrong numerators alike): a decorator scales the diffusion profile's `Rd` by 2 and forwards every sampling method verbatim, the walk gets a large path importance so `RayCaster`'s own `rs.importance < RC_RR_THRESHOLD` test cannot fire for either run, and the accumulator is wired into the `LightSampler` so the NEE half is measured too; identical attempt counts confirm identical paths, and every moment must then scale by exactly 4 (bounded below 4 by the tile's Rd-independent SSS-surface events). On `145c9259` BOTH ratios were EXACTLY **1**; after, **3.99962** (BSDF) and **3.99567** (NEE). `OptimalMISTrainingSitesTest` **23/0** (21/2 before). See [DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md](DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md) "Round 3" and "Round 5". | S | precision | not user-visible (training-input fix only; render-time weight was already `PowerHeuristic`-correct before and after -- see the row's own prior "not applicable" note) |
| ~~DL-74~~ | ~~DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md: surface env-NEE/escape MIS partition violation under guiding~~ | ~~`PathTracingIntegrator.cpp`'s surface scatter continuation stores the COMBINED `effectiveBsdfPdf`... but surface env-NEE weights against the RAW material pdf~~ | CLOSED 2026-09-14 (debt-guiding2 slice, review round 2) for EVERY guiding mode; **DL-83 closes with it** | **The first repair (same slice, earlier commit) did NOT close the partition and its ledger entry overstated it; this row supersedes that text.** Root cause restated: the guided pdf had two jobs and one home. `RAY_STATE::bsdfPdf` was simultaneously the throughput/optimal-MIS-training denominator (which REQUIRES the true sampling density) and the MIS partner against light sampling (which requires a function `LightSampler`'s NEE arms can evaluate for the same direction — impossible, since NEE runs at PART 2 before PART 3 selects a lobe). Fix: split the roles. `bsdfPdf` keeps the true sampling density; a new `RAY_STATE::bsdfMisPdf` (default -1 = "not set, use `bsdfPdf`", read via `MisPartnerPdf()`) carries a lobe-INDEPENDENT NOMINAL density `alpha_nom*guide(w) + (1-alpha_nom)*p_aggregate(w)` evaluated identically by the env-NEE arms, the area-light NEE arms, the escape weight and the emitter-hit weight. `alpha_nom` = base alpha with the learned per-cell scaling and NO per-lobe halving; the cosine product is applied unconditionally; the guiding MODE does not enter the formula, which is why DL-83 closes here. MIS is unbiased for any weights that partition to one (one-line proof in the doc), so a shared nominal density costs nothing in correctness. Also: ONE distribution per shading point (initialised above PART 2 with ONE `sampler.Get1D()`, shared with PART 3 — removes the extra NEE-side draw AND the second stochastic region lookup; nothing is drawn when guiding is off); a direction with NO BSDF-side partner keeps weight 1 on BOTH sides (a delta lobe; a vertex with no guiding at all) -- **but not a direction merely outside the material's aggregate SAMPLING support, which rounds 2-3 wrongly put in that class; see the round-4 amendment at the end of this row**; the volume continuations get the same split (DL-72 P2-5). Red-proof: new `tests/PTGuidingMISPartitionTest.cpp`, two closed-form furnaces on the live Job/RayCaster/LightSampler. Red on `8fcce0bf` (18/4): (b) `guidingLearnedAlpha=true`, the DEFAULT, +63.3%; (c) `eRayReflection` lobe +47.5%; (d) area-light NEE vs the guided emitter hit -42.7%; (e) RIS mode -65.9%, with the guiding-off reference at +0.08% and control row (a) at +0.16%. Green after: **22/0**, every row within 0.55% of its closed form. **Review round 3 (same slice, same day) found that the round-2 split had left two things undone and fixed both; the row stays CLOSED only because the two new red-proof rows are green.** (P1) Splitting one field into two obliges an audit of every CONSUMER of the old one, and `bsdfMisPdf` was consumed in exactly ONE place (`RayCasterEnvEscapeMISWeight`): `PathTracingShaderOp`'s three entry points still forwarded only `rs.bsdfPdf` into `IntegrateFromHit{,NM,HWSS}` (which then re-derived the partner as the same value, under a comment asserting every caller is non-guided -- false for the one producer round 2 had just taught to differ), and `EmissionShaderOp`'s RGB+NM emitter-hit weights read `rs.bsdfPdf` in the partner role. Global medium + trained VOLUME guiding + an area emitter: **+44.1% over** (row (g), guided 0.0111283 vs the unguided expectation 0.0077211) on the round-2 HEAD `2ebcaff9`, **+0.041%** after. (P2) The round-2 residual list dismissed the NEE-vs-escape IOR-stack divergence as "pre-existing and orthogonal"; both adjectives were wrong -- the escape side never evaluated the aggregate pdf at all before this row, and an albedo-1 furnace at a vertex inside a dielectric read **-14.0% with guiding OFF and -14.8% ON** (row (f)), because `PolishedSPF::Pdf` (Fresnel from `ior_stack.top()`: 0.266774 at top 1.0 vs 0.296169 at 1.5) and `TranslucentSPF::Pdf` (`containsCurrent()`: 0 vs 0.185406) really do vary with the stack. `EvaluateDirectLighting{,NM}` now take the caller's live `IORStack` (PT's PART 2 and HWSS NEE sites, and `DirectLightingShaderOp`, pass theirs; the volume and BSSRDF NEE sites keep the null default because neither of their materials reads a stack). Stated consequence: a guiding-OFF render of a stack-reading material changes -- it was mis-weighted before. `PTGuidingMISPartitionTest` **37/0** (was 22/0; 34/3 on `2ebcaff9`). **Review round 4 (same slice, same day) found no P1 and one more live partition defect at the design's own edge, fixed under this row.** Rounds 2-3 read `p_aggregate(w) <= 0` as "the BSDF-sampling technique never generates `w`" and gave weight 1 to BOTH sides; under guiding that is false, because the continuation is drawn from a MIXTURE whose guide term reaches directions the material's own sampler cannot (PT's one-sample branch traces the guided direction whenever `combinedPdf > NEARZERO`, which `alpha*guide` alone satisfies), so that whole region's energy was counted TWICE. Reachable set: any direction with a nonzero BSDF value but zero aggregate sampling density -- the four full-sphere BSDFs (hair, fabric, weave, translucent), whose NEE hemisphere rejection is deliberately disabled, and any partial-support lobe. Fix: `PTGuidingMisPdf::Eval` returns the full mixture whenever guiding is ACTIVE (passing the aggregate through untouched when it is not, so "no partner" survives where it is true), and the four NEE arms call the hook OUTSIDE their `pdf > 0` gates, gating the weight on the BLENDED value. Red-proof, two new rows on the tilted-hemisphere decorator SPF (`Pdf` EXACTLY zero outside a hemisphere 45 degrees off N, albedo-1 Lambertian BRDF over the whole upper hemisphere): (h) env-NEE vs the env escape with the guide aimed into that wedge, **+10.57 %** on `0860f78f` (+12.23 % at 4x samples, converging on the closed-form doubled-wedge excess `(1-cos45)/2 = 14.6 %`) -> **-0.35 %**; (i) area-NEE vs the emitter hit with the emitter ENTIRELY inside the wedge, **+100.35 %** -> **-0.03 %**, its closed form being `L_e*(R/d)^2*cos(beta)`. Both rows carry a guiding-OFF CONTROL, green before and after. `PTGuidingMISPartitionTest` **63/0** (61/2 on `0860f78f`). Round 4 also forwarded the MIS partner through the fourth HWSS delegation (latent -- no producer can currently reach it with a differing value), and corrected two doc overclaims: the legacy shader chain still does NOT partition (`Reflection`/`Refraction`/`DistributionTracing`/`FinalGather` ops all continue with a default-constructed `RAY_STATE`, so `MisPartnerPdf()` is 0 and `EmissionShaderOp` weights at 1), and the hoisted unconditional `ApplyCosineProduct` changes the guided SAMPLING distribution for glossy lobes as well as the weight (unbiased -- the throughput divides by the post-product `combinedPdf` -- but a variance change, unmeasured). A clay_lights concern raised in review was REFUTED by reading: PART 3's `pSPF` is `pClaySPF` under the override and `ClayNEEMaterial::Pdf` delegates to the same instance, so both sides evaluate one function. See [DL74_ENV_NEE_GUIDING_PARTITION.md](DL74_ENV_NEE_GUIDING_PARTITION.md) §8 and §9. | M | physics-bias | user-visible (guided PT with an environment OR area light, every guiding mode) |
| ~~DL-75~~ | ~~RayIntersectionGeometric.h: `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived`; HairGeometry.cpp ~:1029~~ | ~~SSS on hair geometry has no defined outward entry normal~~ Characterized and narrowed: the entry continuation is built from the shading normal (`vNormal`/Ncyl, a genuine per-hit cylindrical radial-outward direction, NOT the ray-derived ribbon-plane `vGeomNormal`/Nflat DL-75 was filed about), so it can never point into the wrong hemisphere by construction; the disk-to-surface area-Jacobian input (`entryGeomNormal`=Nflat) remains ray-derived and is the accepted, documented residual | CLOSED 2026-09-14 (consistency pin; residual narrowed and documented, not eliminated) | `tests/HairSSSEntryNormalTest.cpp`: a real `HairGeometry` strand + real `SubSurfaceScatteringMaterial` driven through 500 real `BSSRDFSampling::SampleEntryPoint` trials plus a white-box sub-test pinning `HairGeometry`'s own `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived` contract directly — `valid 500/500  badFinite 0  badUnitNormal 0  badContinuationHemisphere 0  badNeeAdapter 0  offStrand 0`.  This is a consistency pin, not a red-proof: the invariant it asserts (`Dot(scatteredRay.Dir(), entryNormal) >= 0`) already held structurally before this pass, because `SampleEntryPoint`'s cosine-weighted continuation is built via `cosineONB.CreateFromW(entryNormal)` — a cosine-hemisphere sample around any axis satisfies that dot-product bound by construction, regardless of what the axis physically means for a sub-micron fibre.  The production NEE adapter (`BSSRDFAdapters::BSSRDFEntryBSDF`) was also confirmed to return a finite, non-negative value on a hair entry record rather than crash or NaN.  **What remains UNDEFINED, accepted rather than fixed**: `entryGeomNormal` (Nflat, ray-derived per `bGeomNormalRayDerived`) drives `SampleEntryPoint`'s `cosN`/`cosT`/`cosB` disk-to-surface area-Jacobian terms, so the SAME physical point on a fibre can report a DIFFERENT Jacobian depending on which probe axis/direction reached it — a `subsurfacescattering_material` on `hair_geometry` therefore renders something well-defined (finite, non-inverted, non-crashing) but its BSSRDF energy/PDF is not claimed physically correct, matching the row's own original recipe option (a) ("characterize... likely inconsistent/incorrect but not crashing") rather than option (b) (a reject/diagnostic path, not implemented — the combination is not forbidden and does not corrupt the walk). See `HairSSSEntryNormalTest.cpp`'s header comment for the full derivation. | S | coverage/precision | latent (SSS-on-hair combination renders a finite, non-inverted result; entry-point spatial density/energy for that combination is not claimed correct — documented, not fixed) |
| ~~DL-63~~ | ~~DL37_GGX_DIFFUSE_TRANSMISSION.md: independent specular-only sweep~~ | ~~GGX height-correlated single scattering receives compensation from a separable-masking energy LUT, producing specular-only furnace gain~~ | CLOSED 2026-09-14 | `052ec469`: `GGXDiffuseTransmissionTest: 150 checks, 0 failures` (red: 3 failures, Schlick iso F0=1 alpha=0.6 theta=80 = 1.0772, alpha=1 theta=60 = 1.0432, alpha=1 theta=80 = 1.1467). Root cause confirmed exactly as diagnosed: `GenerateMicrofacetEnergyLUT.cpp` integrated the per-sample VNDF weight `G1(wo)` (correct for the SEPARABLE model `G1(wi)*G1(wo)`), but GGXBRDF/GGXSPF/CoatedBRDF render single-scatter with Smith HEIGHT-CORRELATED `G2` (`MicrofacetUtils::GGX_G2`/`GGX_G2_Aniso`). Fix added a second height-correlated-G2 accumulator to the SAME generator (weight `G2(wi,wo)/G1(wi)`, the Heitz-2018 VNDF-sampling identity), producing NEW `E_ss_TABLE_G2`/`E_avg_TABLE_G2` tables and `LookupEssG2`/`LookupEavgG2`/`MSLobeZG2`/`SampleMSCosThetaG2`/`MSPdfG2` functions in `MicrofacetEnergyLUT.h`; GGXBRDF.cpp/GGXSPF.cpp/CoatedBRDF.cpp (coat lobe, sibling-audit find, same pattern) repointed to the G2 twins. CookTorranceBRDF/SPF (separable-G model) keep the ORIGINAL, UNCHANGED `LookupEss`/`LookupEavg`/`MSLobeZ`/`MSPdf`/`SampleMSCosTheta` -- regenerating the tool reproduces `E_ss_TABLE`/`E_avg_TABLE` bit-for-bit identical to the checked-in values, confirmed. Independent verification (`tests/GGXHeightCorrelatedEnergyLUTTest.cpp`, new, no shared code with the generator or the LUT header): Monte-Carlo quadrature of the real `MicrofacetUtils::GGX_Lambda`/`GGX_G2` primitives with an independent RNG matches `LookupEssG2` to within 8 SE at the three failing configs and four spot checks (`14 checks, 0 failures`), and confirms `LookupEss` measurably diverges from the same quadrature at those points (e.g. alpha=1 theta=80: `LookupEss=0.52281` vs quadrature `0.66839`). **User-visible impact**: affects `ggx_material`/`coated_material` in every Fresnel mode wherever the multiscatter term is active; head-on incidence unchanged; grazing rim of rough metals/coated surfaces DARKENS (isolated multiscatter-albedo term at Schlick F0=0.9, recomputed from the checked-in tables this pass: alpha=0.3 mu=0.20 `0.1464->0.1284` (-12.3%), alpha=0.3 mu=0.05 `0.1098->0.0543` (-50.6%), alpha=0.6 mu=0.05 `0.1894->0.0760` (-59.9%)). DL-65 is narrower: only renders with `filter_glossy` (`StabilityConfig::filterGlossy > 0`) active on `cook_torrance_material`/`schlick_material` past the first bounce. See [DL-62/DL-64 closure doc, extended](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md) "User-visible impact". A residual left END-CAP defect (isotropic case, alpha=1.0, mu~0.008: `brute Ess=0.9614` vs `LookupEssG2=0.9349`, clamped to the LUT's first bin, ~2.6% furnace gain beyond ~89 deg incidence) and the anisotropic-render deficit are NOT closed by this fix — see DL-77. | M | physics-bias | user-visible (rough GGX specular over-energy) |
| ~~DL-77~~ | ~~DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-77" section; found auditing DL-63's own fix (P2-3, debt-ggx2 slice review)~~ | ~~The DL-63 `LookupEssG2`/`LookupEavgG2` height-correlated compensation is calibrated to an ISOTROPIC Smith model, but `GGXBRDF`/`GGXSPF` look it up at `alphaEff=sqrt(alphaX*alphaY)` while their single-scatter term uses direction-dependent per-axis `MicrofacetUtils::GGX_G2_Aniso` -- for strongly anisotropic `alphaX`/`alphaY` pairs this under-compensates, an energy DEFICIT (not the gain DL-63 fixed) at F0=1~~ | CLOSED 2026-09-14 (debt-ggx3 slice) | Red-proof (unfixed): `GGXDiffuseTransmissionTest`'s `TestAnisotropicKnownFailureDL77` known-failure control read `0.9292+/-0.0033` at `alphaX=.02/alphaY=1.0` F0=1 theta=60 spec-only (the isotropized lookup's under-compensation; the raw `E_ss` gap alone -- 0.533 true vs 0.972 isotropized lookup -- projects to a much larger furnace deficit before the H6 direction-aware selection weight partially masks it). Root cause confirmed exactly as diagnosed, PLUS a second-order effect found while fixing it: a first cut that resolves ONLY the anisotropy-ratio dimension (still azimuthally AVERAGING wi/wo's azimuth into a single `E_ss(ratio,alphaEff,mu)` table) closes the average-case deficit but REGRESSES an existing anisotropic `TestSchlickSweep` row into a GAIN -- `alphaX=.05/alphaY=.5 theta=80 az=90` read `1.0376+/-0.0036` (limit `1.0266`), because the TRUE single-azimuth `E_ss` at that config spans `0.789` (az=0) to `0.879` (az=90) against a `0.841` azimuthal average (an independent per-azimuth VNDF quadrature confirmed this directly). Fix, two parts: (1) `MicrofacetEnergyLUT.h` gained `E_ss_TABLE_G2_ANISO`/`E_avg_TABLE_G2_ANISO` (ratio x alphaEff x cosTheta, ratio log-spaced `[1,100]`) plus `LookupEssG2Aniso`/`LookupEavgG2Aniso`/`MSLobeZG2Aniso`/`SampleMSCosThetaG2Aniso`/`MSPdfG2Aniso` -- azimuth-AVERAGED, used only by the H6 multiscatter-lobe SAMPLER (an importance-sampling proposal shape; precision there is an efficiency, not correctness, concern). (2) `E_ss_TABLE_G2_ANISO_PHI` (ratio x alphaEff x phi x cosTheta, phi endpoint-inclusive `[0,90]` degrees exploiting the ellipse's quarter-period mirror symmetry) plus `LookupEssG2AnisoDirectional` -- per-AZIMUTH, NOT averaged, used at every Ess_i/Ess_o ENERGY-COMPENSATION call site in `GGXBRDF.cpp`/`GGXSPF.cpp` (`value`/`valueNM`'s Ess_o/Ess_i, `Scatter`/`ScatterNM`'s wms selection weight and MS-lobe kray's Ess_o/Ess_i, `Pdf`/`PdfNM`'s wms), passing the queried direction's actual tangent-space x,y. Every new lookup function forwards to the EXACT pre-existing isotropic function when `alphaX==alphaY` (verified byte-identical, `1e-15`-tight, for all five), so isotropic GGX materials are numerically unchanged. `tools/GenerateMicrofacetEnergyLUT.cpp` regenerates `MicrofacetEnergyLUT.h` byte-for-byte (both hand-maintained blocks embedded verbatim, `NUM_SAMPLES_ANISO=150000`/cell, ~54s at the initial ANISO_PHI_SIZE=7 grid, ~292s (4m52s) after the debt-ggx3 review-round-2 ANISO_PHI_SIZE 7->13 bump, single-threaded on both measurements). Fixed: known-failure config now reads `0.9982+/-0.0033`; the previously-regressed `(.05,.5)` az=90 row is fixed alongside it (no longer tested via the isotropic-averaged-only path). `GGXDiffuseTransmissionTest`'s known-failure control was promoted to a real TWO-SIDED gate (`TestAnisotropicFurnaceDL77`/`CheckAnisotropicFurnaceBound`: standard upper bound `1+6SE+.005` PLUS a `0.90` lower floor -- the floor exists specifically so a regression of this fix, which used to read as low as ~0.57-0.93, fails loudly instead of passing a gain-only check again) with 2 added independent `(alphaX,alphaY,theta,azimuth)` rows (ratio=10 az=45 off-grid interpolation test, ratio=9 az=90): `153 checks, 0 failures` (was `151/0` with the deficit invisibly passing). Gate suite (12 tests): `GGXDiffuseTransmissionTest` 153/0, `GGXHeightCorrelatedEnergyLUTTest` 23/0 (unchanged, isotropic-only), `GGXSampleEvaluationConsistencyTest` 48/0, `GGXWhiteFurnaceTest`/`LayeredWhiteFurnaceTest` (0/57)/`ThinFilmBRDFTest` 25/0/`ThinFilmFurnaceTest` 4/0/`SPFPdfConsistencyTest`/`SPFBSDFConsistencyTest`/`CookTorranceMultiscatterTest` 17/0/`GGXMetalRoughGridTest`/`GGXFresnelModeTest` all pass; clean warning-free rebuild. Sibling audit: `CoatedBRDF`'s coat lobe confirmed isotropic-only by construction (`alpha,alpha` always), no change; `grep`'d the whole tree for `GGX_G2_Aniso`/`LookupEssG2`/`LookupEavgG2` consumers -- only `GGXBRDF.cpp`/`GGXSPF.cpp`/`CoatedBRDF.cpp` (exempt) and the header itself; `ThinFilmBRDFTest.cpp`/`ThinFilmFurnaceTest.cpp`/`GGXSampleEvaluationConsistencyTest.cpp`'s own re-derivations only exercise isotropic `alpha` configs, untouched. Render-visible impact (`scenes/Tests/Materials/ggx_anisotropy_sweep.RISEscene`, brushed-metal silver spheres, PT 64spp, `oidn_denoise FALSE`, `pixel_filter box`, EXR `Rec709RGB_Linear`, same seed before/after): per-region mean luminance across the 3x3 anisotropic-ratio grid rose +0.9% to +4.8% (energy deficit closing, as expected; no NaN/Inf introduced). Left end-cap residual (isotropic, `alpha=1.0 mu<0.0156`, ~2.6% furnace gain) recorded separately as DL-86 (not fixed this slice -- touching the isotropic end-cap risks the "isotropic byte-identical" requirement this slice was gated on). **Review follow-up (debt-ggx3, same day, P1 -- a genuine correctness bug in the closure above, not a residual)**: every row cited above has `alphaX < alphaY`, so all of them were structurally blind to a second, independent bug: `AnisoPhiIndex` read the queried direction's azimuth off the caller's raw `(localX,localY)` with NO axis swap, but the table's BAKED phi=0 axis always meant "the smaller-alpha axis" (`alphaX_table=alphaEff/sqrt(ratio)` was always `<= alphaY_table` by construction) -- so a caller passing `alphaX>alphaY` (the swapped labelling; e.g. EVERY glTF `pbrmetallicroughness_material`, `Job.cpp`'s `alphaX>=alphaY` convention, and the silk/satin/denim fabric presets) read a MIRRORED azimuth. Red-proof (unfixed): two new `TestAnisotropicFurnaceDL77` rows with `alphaX>alphaY` read `1.1678+/-0.0028` and `0.7574+/-0.0037` (matching this row's own `1.1699`/`0.7613`); a new `GGXHeightCorrelatedEnergyLUTTest::TestRelabelSymmetry` (7 rows: `LookupEssG2AnisoDirectional(aX,aY,phi)` must equal `(aY,aX,90-phi)` for the same physical direction) failed ALL 7 (diffs up to `0.24`). Fixed by re-parametrizing the table directly on `(alphaX,alphaY)` as two independent grid axes spanning the full `[0.01,1.0]` range each (replacing `(ratio,alphaEff)`), so phi=0 means "aligned with the queried alphaX axis" unconditionally with no swap needed; both furnace rows now read `0.9988`/`0.9924` (re-measured `0.9991+/-0.0029`/`0.9950+/-0.0032` after the debt-ggx3 review-round-2 ANISO_PHI_SIZE bump -- see that round's own note below for every re-taken number) and all 7 relabel-symmetry rows match to `~1e-16` (the fix mirrors the canonical `alphaX<=alphaY` half into the `alphaX>alphaY` half using the SAME Monte-Carlo samples, so the symmetry is exact, not approximate). This re-parametrization also closed two review findings in the same pass: **P2-1** (a seam at `alphaX==alphaY` up to `0.018` in `E_ss`, from the old table's ratio=1 row being an independent, coarser Monte-Carlo bake instead of matching the isotropic curve -- the new table's diagonal is seeded directly from the converged isotropic `E_ss_G2`/`E_avg_G2` tables) and **P2-2** (most nominal `(ratio,alphaEff)` grid cells were physically unreachable -- e.g. ratio=100 forced `alphaEff` to a single point -- wasting resolution; an independent 4000-point sweep against a fresh VNDF quadrature found a worst-case residual of 5.09% at `alphaX=0.0114,alphaY=0.1464,cos=0.1377,phi=69.5`; after the reachability fix, worst case dropped to 4.52%, and after also raising the alpha/cos grid resolution 16x16x7x16 -> 24x24x7x32 (matching the isotropic `LUT_SIZE`), to 3.25% at `alphaX=0.9353,alphaY=0.0752,cos=0.1211,phi=85.5` (mean residual 0.15%) -- NOT closed to the `<=1%` target.  **Review round 2 correction (debt-ggx3, same day)**: this row's "same grazing end-cap DL-86 already tracks" attribution was WRONG for the cited point -- an independent profile at that exact configuration (nowhere near the `c0=0.0156` end-cap clamp) found table-vs-truth 0.1-0.7% at every phi grid NODE but peaking 3.2-3.3% MID-INTERVAL (85-87.5 degrees); snapping phi alone to a node dropped the residual `3.43%->0.79%`, snapping cos alone left `3.24%` -- the actual driver is the 15-degree-coarse `ANISO_PHI_SIZE=7` grid against strong curvature near phi=90 at high anisotropy, with the LINEAR alpha axis (a 5.3x ratio between the first two nodes) as a secondary driver; the cosTheta end-cap (cos<0.0156) IS real and IS DL-86, it just wasn't what this cited point was measuring.  Fixed by raising `ANISO_PHI_SIZE` 7->13 (7.5-degree steps; endpoint-inclusive grid and exact node mirror symmetry both preserved for any N; alpha axis measured and left linear -- a log-spaced axis is a bigger, separately-scoped change per that pass's own note).  Re-measured (independent scratch program, own mt19937_64 stream): the cited point now reads `2.58%` (4e6-sample clean measurement); full 4000-point sweep RESTRICTED to `cos>=0.03` (isolating the phi/alpha-grid residual from the separately-tracked DL-86 end-cap) gives worst case `2.69%` at `alphaX=0.0411,alphaY=0.6853,cos=0.0742,phi=2.9` (mean `0.118%`, 6 >1%, 2 >2%, 0 >5%, of 3908) -- down from the pre-fix `3.25%`/`0.15%`/35/7/0.  UNRESTRICTED worst case is `17.89%` at `alphaX=0.0361,alphaY=0.9627,cos=0.0024,phi=5.0` -- a genuine DL-86 end-cap point (`cos=0.0024` is deep inside the first bin), not a DL-77 regression.  Still NOT closed to the `<=1%` target; the residual's root cause is now measured as phi-interpolation curvature at extreme anisotropy ratios (further narrowed, not eliminated, by the 13-point grid) plus the low-alpha linear-grid coarseness -- both left open per that pass's note, and the separate cosTheta end-cap remains tracked as DL-86, unaffected by this fix.). Also: the 7 large `Scalar` tables in `MicrofacetEnergyLUT.h` were changed from `static const` to `inline constexpr` (C++17, confirmed the standard on all 5 build projects) to stop per-including-TU duplication of the now much larger anisotropic tables (~1.15MB/TU across 5 TUs pre-fix); measured on this repo's LTO-enabled `make` build the final linked `bin/rise` size was unchanged within noise (LTO already dedupes identical read-only data at link time), so the fix's real benefit is to non-LTO builds (Debug configs) where the duplication would otherwise persist -- not independently verified per-platform.  **Review round 2 (debt-ggx3, same day)**: `inline constexpr` was then changed to `inline const` on all 7 tables -- identical C++17 external-linkage/one-definition-rule dedupe, but without obligating compile-time constant evaluation, which avoids MSVC's default `/constexpr:steps 100000` budget failing on the largest table (`E_ss_TABLE_G2_ANISO_PHI`, 24*24*13*32 = 239,616 elements -- 129,024 was the pre-ANISO_PHI_SIZE-bump count, corrected debt-ggx3 review round 3). `GGXDiffuseTransmissionTest`: `155 checks, 0 failures` (was `153/0`); `GGXHeightCorrelatedEnergyLUTTest`: `30 checks, 0 failures` (was `23/0`). Full gate (13 suites) re-run clean: `GGXSampleEvaluationConsistencyTest` 48/0, `GGXWhiteFurnaceTest`/`GGXMetalRoughGridTest` all pass, `LayeredWhiteFurnaceTest` 0/57, `ThinFilmBRDFTest` 25/0, `ThinFilmFurnaceTest` 4/0, `SPFPdfConsistencyTest`/`SPFBSDFConsistencyTest` pass, `CookTorranceMultiscatterTest` 17/0, `FabricRenderTest` 57/0, `WeaveMaterialChunkTest` 296/0; generator regenerates the header byte-for-byte (0-line diff); clean recompile of every TU depending on `MicrofacetEnergyLUT.h` (`GGXBRDF.cpp`/`GGXSPF.cpp`/`CoatedBRDF.cpp`/`CookTorranceBRDF.cpp`/`CookTorranceSPF.cpp`) warning-free.  **Review round 2 fix-pass (debt-ggx3, same day)**: after the `ANISO_PHI_SIZE` 7->13 fix, added one tight two-sided regression guard row to `GGXDiffuseTransmissionTest.cpp` at the exact cited residual point (`alphaX=0.9353,alphaY=0.0752,theta=83.0441,az=85.5`, 400k-sample local measurement to get SE small enough to discriminate: post-fix `0.98434+/-0.00087`). **Round 3 correction (debt-ggx3 review round 3)**: the row as shipped here used a `6*SE` band, which does NOT red-proof -- the pre-fix (`ANISO_PHI_SIZE=7`) header, rebuilt in isolation at the SAME seed/sample count, reads `0.97933+/-0.00088`, which falls INSIDE a `6*SE` band (`[0.97912,0.98956]`), only a ~5.7*SE separation from this run's mean but not enough to clear a 6*SE half-width. Fixed by tightening to a `4*SE` band computed from the test's own RUNTIME `se` (not a copied-in literal): band `[0.98086,0.98782]`, and the pre-fix value `0.97933` now falls `~1.75*SE` below the lower bound -- a genuine, verified FAIL (isolated pre-fix-header rebuild console line pasted into the round-3 fix commit message). `GGXDiffuseTransmissionTest`: `156 checks, 0 failures` (was `155/0`). Also added one `alphaX>alphaY` material-level row (`ax=0.8,ay=0.1`) to `GGXWhiteFurnaceTest.cpp`'s Test 6/Test 7 (`TestMaterialPointwiseConsistency`, conductor and schlick_f0) -- every prior row there had `alphaX<=alphaY`, the exact P1 blind spot; both new rows pass (`spfAlbedo` 0.47-0.49 conductor, 0.53 schlick_f0). Full gate (14 suites) re-run clean at the final `ANISO_PHI_SIZE=13` grid, zero warnings on a clean rebuild: `GGXHeightCorrelatedEnergyLUTTest` 30/0, `GGXSampleEvaluationConsistencyTest` 48/0, `GGXWhiteFurnaceTest`/`GGXMetalRoughGridTest`/`GGXFresnelModeTest` all pass, `LayeredWhiteFurnaceTest` 0/57, `ThinFilmBRDFTest` 25/0, `ThinFilmFurnaceTest` 4/0, `SPFPdfConsistencyTest`/`SPFBSDFConsistencyTest` pass, `CookTorranceMultiscatterTest` 17/0, `FabricRenderTest` 57/0, `WeaveMaterialChunkTest` 296/0. Generator regenerates the header byte-for-byte (0-line diff) at the new grid size. | M | physics-bias | user-visible (anisotropic rough metals now correctly compensated at grazing incidence, e.g. brushed-metal `ggx_material` with `alpha_x != alpha_y`; P1 additionally affects every glTF-imported anisotropic material and the silk/satin/denim fabric presets, all of which author `alphaX>alphaY`) |
| DL-81 | Padded Sobol dimension parity correlates lobe-selection draws across bounces — `Sampling/SobolSequence.h:192` (`SobolSequence::Sample`) / `Utilities/SobolSampler.h:61,100` (`kStreamStride`, `StartStream`); render symptom at `DielectricSPF.cpp` `Scatter` (~:391-404, the RGB dispersion loop) / `DoSingleRGBComponent` / `GenerateScatteredRay` | RGB dispersion carries channel-INDEX-dependent systematic offsets: two channels given the SAME ior render ~12% apart, and neither matches the equivalent non-dispersive render at that ior. Root cause traced past `DielectricSPF` into the sampler: `SobolSequence::Sample` derives its base Sobol value from `dimension & 1` only (padded-Sobol dimension reduction), and `SobolSampler::kStreamStride` (32, even) means every bounce's stream starts at a dimension of matching parity to every other bounce's — so a lobe-selection draw at one bounce and the SAME positional draw at a later bounce share the SAME underlying pre-scramble value, differing only by the Owen-scramble hash seed, which does not fully decorrelate the joint distribution. | OPEN-confirmed, red-proved (found gating DL-29; PRE-EXISTING, nothing to do with the painter; root cause re-diagnosed past DielectricSPF into the sampler, this pass) | **Render-level evidence (unchanged from the original finding).** Fixture: a lossless (`tau 1.0`, `scattering 1000000`) 2-deep `box_geometry` dielectric, white Lambertian luminaire (exitance 1) directly behind, pinhole camera on axis at fov 10, 24x24, `pathtracing_pel_rasterizer`, `oidn_denoise FALSE`, `pixel_filter box`. Closed form per channel is `L·(1-R0(n_c))²`, `L = 1/pi`, `R0(n) = ((n-1)/(n+1))²`. With `ior 1.8532352941 1.8532352941 2.125` the render reads **R=0.281047 G=0.250650 B=0.241661** where the closed form is 0.263925 / 0.263925 / 0.241150 — channels 0 and 1 carry the IDENTICAL index yet differ by 12.1% (channel 0 +6.5%, channel 1 −5.0%); channel 2 is within 0.2%. A non-dispersive control at the identical uniform `ior 1.8532352941` reads 0.264618 achromatic (+0.26% of closed form), so the closed form and the harness are sound and the dispersive path is the outlier. The sum is conserved to +0.57%, i.e. this is a redistribution BETWEEN channels, not an energy loss, and it is fully converged and deterministic (identical at 256 and 2048 spp and at seed bases 1000/2000/3000 — the sampler is QMC). Reproduces byte-identically whether the three iors come from a `scalar_painter { file … }` curve or an inline `r g b` triple, so it is not a painter defect. **Mechanism (this pass).** `SobolSequence::Sample` computes `sobolDim = dimension & 1` (Sampling/SobolSequence.h:192) — of the requested `dimension`, only its parity selects which of the two base Sobol dimensions (0 or 1) is drawn; every dimension then gets its OWN Owen-scramble seed via `HashCombine(seed, dimension)`, but two dimensions of the same parity scramble the identical pre-scramble base value with two different hashes rather than drawing from independent low-discrepancy sequences. `SobolSampler::kStreamStride` = 32 (SobolSampler.h:61, even) and `StartStream(16+depth)` (SobolSampler.h:100, `dimension = streamIndex * kStreamStride`) means bounce depth `d`'s stream starts at dimension `512 + 32d` — consecutive bounces' streams differ by exactly `kStreamStride` = 32, an EVEN number, so the k-th draw of every bounce has the SAME parity as the k-th draw of every other bounce (verified directly by reading both files on this branch: `dimension & 1` is insensitive to adding an even `kStreamStride`). A lobe-selection draw at an entry hit (dimension `512+k`) and the same positional draw at the exit hit one bounce later (dimension `544+k` = `512+32+k`) therefore share their base Sobol value and differ only by the scramble seed — an investigation probe (joint-occupancy of a 3-way channel-selection partition against the product of marginals) reported same-parity-pair occupancy 1.0507/0.9506/0.9975 (R/G/B) vs 1.0004/0.9980/0.9993 for a different-parity control vs 0.9987/1.0122/0.9944 for an independent-seed baseline, with the same-parity G-channel figure (0.9506) matching the live dispersive render's own G-channel deficit (0.9497, i.e. G=0.250650 vs closed-form 0.263925 = 0.9497) to four digits — strong internal corroboration that the render-level bug and the sampler-level parity tie are the same phenomenon; this session verified the CODE mechanism (parity formula, even `kStreamStride`, `StartStream` arithmetic) directly but did not independently re-run the joint-occupancy probe byte-for-byte, so treat the specific occupancy figures as the originating investigation's measurement, not re-derived here. **Consequence: this is broader than RGB dispersion.** ANY repeated stochastic selection at successive bounces under `SobolSampler` — Fresnel reflect/refract choice, multi-lobe selection, Russian roulette — draws from the same correlated positional-parity family, not just `DielectricSPF`'s per-channel loop. Recipe (not yet built): a standalone joint-occupancy test comparing draws at same-parity dimension pairs `(512+k, 544+k)` against different-parity pairs and an independent-seed baseline, over a 3-way (or N-way) partition, asserting the same-parity pair's joint occupancy departs from the product of marginals while the controls do not; plus this row's existing render fixture as the physical-symptom regression. Sibling suspects with the same DOWNSTREAM shape (their own per-channel dispersion loops feeding off the same correlated draws): `PolishedSPF.cpp` ~:187 and `PerfectRefractorSPF.cpp` ~:227. **Fix direction — NOT done in this slice.** Give each stochastic-selection draw a distinct Sobol dimension: either true multi-dimensional Sobol direction numbers (beyond the padded (0,2)-net's 2 base dimensions), or an odd/hashed per-bounce dimension offset so `kStreamStride`'s bounce-to-bounce stride no longer preserves parity. This is a sampler-architecture change with reach far beyond `DielectricSPF`, and is out of scope for a fix here. | M | physics-bias | user-visible (any render using `SobolSampler` with repeated stochastic selection across bounces: dispersive-dielectric RGB rendering is the confirmed instance, and any other repeated Fresnel/lobe/Russian-roulette selection is a suspected instance sharing the same mechanism) |
| DL-82 | Painters/SellmeierScalarPainter.h:39,89 / PolynomialScalarPainter.h:41,74 / Function1DScalarPainter.h:38,51 | The DL-29 pattern, unfixed in three sibling curve painters: each broadcasts ONE representative-wavelength sample into all three channels from `GetValuesAt` and reports `HasPerChannelVariation() = false`, so a Sellmeier or polynomial DISPERSION FORMULA bound to `ior` renders achromatic and non-dispersive under every RGB rasterizer | OPEN-confirmed (static evidence; deliberately NOT fixed in the DL-29 slice, see below) | `SellmeierScalarPainter::GetValuesAt` returns `ScalarTriple( EvalAtNM( 587.6 ) )`, `PolynomialScalarPainter` `ScalarTriple( EvalAtNM( 550.0 ) )`, `Function1DScalarPainter` `ScalarTriple( pFunc->Evaluate( 555.0 ) )` — the exact shape DL-29 removed from `PiecewiseLinearScalarPainter`. A Sellmeier curve IS a dispersion formula by construction, so the case for sampling it at `ScalarPainterRGB::kChannelNM` is at least as strong as for a measured file. In-tree scenes bind these (`scenes/Tests/Spectral/phase6_scalar_painter_forms.RISEscene`, `phase3_dielectric_iscalarpainter.RISEscene`, `scenes/FeatureBased/EnamelWatch/enamel_watch.RISEscene`, `scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene` among them). NOT fixed with DL-29 on purpose: the change would flip those materials onto the RGB dispersion path, which **DL-81 shows carries a ~6-12% per-channel error today** — propagating a known defect to more scenes before it is fixed. Sequencing (honest re-statement, this pass): the plan is still "close DL-81 first, then make this change," but DL-81's root cause was re-diagnosed this pass from an apparently local `DielectricSPF` dispersion-loop bug into a `SobolSampler`-level padded-Sobol dimension-parity correlation (see DL-81's row) — a sampler-architecture fix, not a small local patch, and one whose reach is broader than the dispersion loop alone. "Close DL-81 first" is therefore a LARGER prerequisite than it looked when this row was filed; do not read the unchanged sequencing sentence as implying DL-81 is close to done. Once DL-81 lands, make this change with its own red-proof (per-channel closed form on a Sellmeier slab, plus a cost measurement — it triples the scattered-ray work per dielectric hit) and a re-render of the four scenes above. | S | precision | user-visible (Sellmeier / polynomial / Function1D `ior` renders non-dispersive and achromatic under RGB) |
| DL-86 | docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-63" section, "Known residual: LUT left end-cap"; carried forward from the DL-77 recipe (debt-ggx3 slice, 2026-09-14) | `LookupEssG2`/`LookupEssG2Aniso` (isotropic case) flat-clamp `cosTheta` below the first LUT bin center (`c0=0.5/32~=0.0156`) to that bin's value, under-reading the TRUE (still-rising) `Ess` right at the grazing limit, so the Kulla-Conty compensation adds slightly too much multiscatter energy back -- an isotropic furnace GAIN confined to incidence beyond ~89 degrees | OPEN-confirmed (re-verified this pass; not fixed -- see recipe) | Measured (DL-63 closure, unchanged this pass): `alpha=1.0, cosWi=0.008` (just inside the first bin) -- brute-force independent quadrature `Ess=0.9613-0.9614` vs `LookupEssG2=0.9349`, diff `~0.0263`, an isotropic furnace gain of roughly 2.6% beyond ~89 deg incidence. Deliberately NOT fixed in the debt-ggx3 slice: the slice's own gate required "isotropic tables/behaviour byte-identical" (`GGXHeightCorrelatedEnergyLUTTest` 23/0, `GGXDiffuseTransmissionTest` isotropic rows unchanged) as a hard constraint, and any change to `LookupEssG2`'s end-cap interpolation is a change to isotropic numerics at extreme grazing incidence (even though it happens not to move the 23 currently-gated checks) -- fixing it safely needs its own dedicated red-proof pass verifying no isotropic regression, not a piggyback edit inside an anisotropic-scope slice. Independent of the DL-77 anisotropic-ratio/azimuth residual (this one persists even for `alphaX==alphaY`). Recipe: extend `LUT_SIZE`'s cosTheta resolution near `cosTheta->0` (a non-uniform, finer grid concentrated at low `cosTheta`) or replace the flat end-cap with a one-sided linear/analytic extrapolation from the first two bins (the "cheap" option flagged in the original DL-63 closure note); red-proof = promote the brute-force-vs-lookup comparison above into a gating check in `GGXHeightCorrelatedEnergyLUTTest.cpp` (currently only in this doc's prose) and confirm the existing 23 checks are unaffected before/after. | S | physics-bias | user-visible (isotropic rough GGX/coated materials, extreme grazing incidence beyond ~89 degrees; small and bounded) |
| ~~DL-66~~ | ~~AgentLiveCommitTest.cpp: Test 31 fixture vs. CST brace-on-own-line rule~~ | ~~A regression fixture engineered to test right-side glue-safe Undo restore is rejected outright by an unrelated, apparently-later-hardened parser rule~~ | CLOSED 2026-09-14 (debt-guiding2 slice) | Confirmed a genuine regression, not a fixture relying on since-forbidden syntax: `git show -s --format='%ci' e5d653e0` (the test's own introducing commit) is 2026-07-04, two months BEFORE `bc3c8b71` (the brace-on-own-line hard-reject, 2026-09-03) -- `git merge-base --is-ancestor e5d653e0 bc3c8b71` confirms the ordering. Fixed by redesigning the fixture's separator (test-only change, `tests/AgentLiveCommitTest.cpp`): matA's chunk is now followed by `"\n "` (one newline, one space) instead of zero whitespace before matTarget's chunk. This satisfies the parser (`NextDocContentSharesLine` stops at the first Trivia containing a `\n`, so `closeSameLine` is false) while STILL tripping `DocEraseChunkTidy`'s glue-unsafe path (the tokenizer folds `"\n "` into ONE Trivia leaf whose LAST byte is the trailing space, not `\n`, so `ItemEndsInNewline` on it is still false) -- the exact same `droppedCount==1` precondition the test needs, reached without any same-line brace. Red-proof (this HEAD, via a temporary revert/reapply, not stash): pre-fix run gives `Job::LoadAsciiSceneViaCst:: derive diagnostic: lambertian_material (line 18): chunk braces must be on their own lines` then `FAIL: the zero-whitespace-glued fixture scene loads via the CST path`, `868 passed, 1 failed`; post-fix run gives `884 passed, 0 failed` (0 `FAIL` lines). | S | coverage/test gap | test-only (regression coverage gap closed; no production rendering/state-machine behavior implicated) |
| ~~DL-65~~ | ~~debt-ggx slice sibling audit (docs/skills/audit-by-bug-pattern.md), found while closing DL-62~~ | ~~The DL-62 pattern (SPF widens sampling/density roughness by `ri.glossyFilterWidth`; the paired BRDF's `value`/`valueNM` evaluation does not) is structurally present, unfixed, in two more material families~~ | CLOSED 2026-09-13 | `0ad80d66` (red-proof `5759c8f0`): `CookTorranceSchlickGlossyFilterConsistencyTest: 22 checks, 0 failures` (red: 20 failures, e.g. "spec-only W=0.25 maxRelErr=1.165e+01" CookTorrance RGB). `CookTorranceBRDF::value`/`valueNM` now widen `pMasking`'s alpha by `ri.glossyFilterWidth` identically to `CookTorranceSPF`'s three sample/density paths; `SchlickBRDF::value`/`valueNM` now widen `pRoughness` identically to `SchlickSPF`'s four sample/density paths -- each class's own roughness convention preserved (no GGX alphaX/alphaY split imported). Zero-filter-width controls matched exactly before and after, isolating the divergence to nonzero `glossyFilterWidth`. The DL-64-pattern non-replication finding (raw-F0 lobe-selection weight) is unchanged and was not re-tested (out of this row's scope). | S | physics-bias | user-visible (filtered CookTorrance/Schlick continuation versus direct evaluation) |
| DL-68 | (new; found during DL-45's sibling audit, no source-doc heading) | TranslucentSPF's ENTERING transmission lobe pushes the IOR stack without confirming the sampled direction is geometrically into the solid | OPEN-confirmed (empirical measurement; correctness red-proof pending) | `TranslucentSPF::Scatter`/`ScatterNM`'s entering branch samples `trans` (isotropic and per-channel-N variants) around the flipped shading normal with a Phong `cos^N` lobe and unconditionally pushes `ior_stack`, regardless of whether the sampled direction is geometrically into the solid -- the same failure MODE DL-45 fixed for the exit pop (a state-transitioning lobe skips the geometric-horizon check), but at a DISTINCT site (entry push, not exit pop). At N=1 the Phong lobe IS a plain cosine lobe, so DL-45/DL-68's `ExitValidFraction` Malley's-method closed form transfers EXACTLY (the measured counts below match `(1-cos(phi))/2` -- the complementary "not-into-solid" fraction for a lobe sampled around the flipped/inward normal -- within 1 sigma of binomial sampling noise at every tilt); only N != 1 (the anisotropic, non-cosine Phong case) is open, needing its own P(valid) derivation for a tilted-plane-clipped `cos^N` lobe (or rejection sampling with its own renormalization -- the closed form does not extend to N != 1 because Malley's-method's disk-projection equivalence is specific to plain cosine (N=1) sampling). Direct measurement (isotropic N=1, extinction 0.1, scattering 0, `StubObject`, `RandomNumberGenerator(777)`, 8192 trials/tilt, entering branch; NOT backed by a committed test -- ad hoc, reproduce before relying on the exact counts): fraction of pushed `trans` rays NOT geometrically into the solid (`dot(dir,trueOutward)>=0`) scales with tilt -- 0/8192 at 0deg, 563/8192 at 30deg (cf. `(1-cos30)/2`=0.067), 2032/8192 at 60deg (cf. `(1-cos60)/2`=0.25), 4006/8192 at 89deg (cf. `(1-cos89)/2`=0.491). The exit branch's OWN backscatter `trans` lobe (same unchecked Phong shape, sampled around the flipped normal in the "coming out" branch) shares the pattern but has no stack-transition consequence (documented as "stays inside, no stack change"), so is lower severity -- audit it alongside. | M | physics-bias | user-visible (translucent materials with perturbed shading normals, symmetric to DL-45 but on entry) |
| DL-70 | (new; found during this slice's P2-1/P2-2 vGeomNormal-consumer audit, no source-doc heading) | A small, ENUMERATED set of `vGeomNormal` consumers performs a sign-sensitive entry-vs-exit / front-vs-back test without recovering the double-sided-mesh orientation flip (`RayIntersectionGeometric::bGeomNormalOrientedToRay`) | OPEN-confirmed (static evidence; every site below opened and classified individually, review round 3, 2026-09-13; correctness red-proof pending) | **Re-scoped in review round 3.** The round-2 text claimed "roughly 30 sibling sites" sharing the byte-for-byte `geomNRaw` idiom. That was wrong: the idiom is `const Vector3 geomN = ( Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;`, which RE-ANCHORS the orientation to the ray -- and the double-sided flip is itself defined relative to the ray, so the composite `geomN` is the SAME vector whether or not the geometry flipped. All 59 non-translucent occurrences of that idiom (`grep -rn 'geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw' src/Library` counts 62, of which 3 are `TranslucentSPF.cpp`'s own) are therefore IMMUNE. **Also immune, verified individually:** (a) the "orient the shading normal toward the ray" composite `side = Dot(vGeomNormal, ray.Dir()); n = side > 0 ? -vNormal : vNormal` -- `IsotropicPhongSPF.cpp` :110/:206/:270/:320, `PolishedSPF.cpp` :166/:281/:347/:394/:426/:478/:518, `RandomWalkSSS.cpp` :84 -- because the flip moves `vGeomNormal` and `vNormal` TOGETHER, so the result (a ray-facing `n`) is unchanged; in `PolishedSPF.cpp` every one of the seven `bBackface` values is consumed by exactly that one line and nothing else (`grep -n 'bBackface\|gateBackface' src/Library/Materials/PolishedSPF.cpp`), which specifically refutes the round-2 claim that those three sites were in-pattern. (b) `fabs` / symmetric-band tests: `EmissionShaderOp.cpp` :91/:149 (`fabs`), `PhotonMap.h` :699 + `GlobalSpectralPhotonMap.cpp` :73/:127 + `CausticSpectralPhotonMap.cpp` :73/:129 (`pcos` tested as `(pcos < maxNDist) && (pcos > -maxNDist)`), `InteractivePelRasterizer.cpp` :657 (`if(ndv<0) ndv=-ndv`). (c) `GlintModifier.cpp` :224, which anchors its facet gate to `ri.vNormal` via `geomSign` -- again invariant under a joint flip. (d) `PathTransportUtilities.h` :282 (DL-03's guided-continuation resolver), which compares `Dot(ray.Dir(), normal)` against `Dot(direction, normal)` using the SAME normal, so a global sign flip cancels. (e) **Do NOT "fix"** `EmissionShaderOp.cpp` :56/:131: passing the ray-facing `vGeomNormal` into `emittedRadiance`/`emittedRadianceNM` is what makes a double-sided emitter emit toward the viewer. (f) Not a which-side test at all: the irradiance/AO cache key sites (`FinalGatherShaderOp.cpp` :396/:721, `AmbientOcclusionShaderOp.cpp` :67/:87/:183, `DistributionTracingShaderOp.cpp` :90/:110/:193) and `Object.cpp` :1032 (UV-generator input). **GENUINELY IN-PATTERN (the whole list, 14 sites):** (1) `LightSampler.cpp` :320 and :473 -- medium-stack push/pop on the NEE shadow walk (`ndotd < 0 ? stack.push : stack.remove`); on a double-sided mesh `ndotd` is always negative, so every crossing pushes and nothing is ever removed. (2) `BDPTIntegrator.cpp` :1223 -- the same medium push/pop on the connection walk. (3) `RayCaster.cpp` :2153 -- the transmissive-shadow-ray `bEntering = Dot(dir, vGeomNormal) < 0.0` that selects the Fresnel `(Ni, Nt)` pair; always "entering", so a genuine exit through a double-sided dielectric pane computes air->glass. Notable because the SAME FILE already defines the correct un-flip helper for its self-hit classifier at :487. (4) `DirectVolumeRenderingShader.cpp` :243 and :398 -- `cosine = -Dot(vGeomNormal, ray.Dir()); if (cosine < NEARZERO) /* leaving */`; always positive on a double-sided box, so the volume is never shaded. (5) `SMSPhotonMap.cpp` :347 -- `cosI` stamps `bEntering` on a reflection vertex of the photon chain; always "entering". (6) the BSSRDF front-face gate `cosInGeom > NEARZERO`: `PathTracingIntegrator.cpp` :2447/:2615 and `BDPTIntegrator.cpp` :2309/:2397/:6024/:6111 -- always satisfied, so BSSRDF entry is admitted from the back face. (7) `TransparencyShaderOp.cpp` :61/:100 -- the `bOneSided` alpha-card cull never fires (lowest priority: a double-sided mesh arguably WANTS both sides visible, so this one may be a no-op-by-intent rather than a bug). **SEPARATE, ADJACENT HAZARD (plumbing that DROPS the flag):** `ManifoldSolver.cpp` copies `ri.geometric.vGeomNormal` into `vertex.geomNormal` (:2545, :2569, :2770, :2815, :3982) and later republishes it into a synthetic `rig` whose `bGeomNormalOrientedToRay` defaults to false (:5596, :5673, :5787, :6456, :6613, :7417, :7997, :8074); `BSSRDFSampling.cpp` :152 has the same shape. No sign error today (nothing downstream performs the recovery), but a flipped normal is re-published as an unflipped one, so any future consumer that DOES recover gets the wrong answer. **PER-SITE RECIPE.** For (1)/(2): build a closed DOUBLE-SIDED translucent or dielectric mesh enclosing an emitter, run an NEE/connection shadow ray through it, and assert the medium stack is empty after the walk (pre-fix it holds one entry per crossing). For (3): a double-sided glass pane between a light and a receiver -- assert the transmissive shadow ray's Fresnel uses glass->air on the exit face (instrument `Ni`/`Nt`, or compare transmittance against the single-sided twin, which must match). For (4): a double-sided `box_geometry`-equivalent mesh with an interior medium -- assert the volume renders at all. For (5): a double-sided specular caster in an SMS photon pass -- assert `bEntering` differs between a front-face and a back-face reflection vertex. For (6): a double-sided mesh with a diffusion profile -- assert a BACK-face hit does NOT take the BSSRDF branch. Each fix is the same one-liner the `translucent` slice applied (`const Vector3 trueGeomNormal = ri.bGeomNormalOrientedToRay ? -ri.vGeomNormal : ri.vGeomNormal;`), EXCEPT where the hit may be hair: `HairGeometry` reports a ray-derived normal and advertises it via `RayIntersectionGeometric::bGeomNormalRayDerived` (added independently, and with the same meaning, by this slice and by the concurrent `debt-sssenv` slice for DL-71/DL-75), which such a consumer must honour by skipping rather than recovering. | M | physics-bias | user-visible (double-sided triangle meshes carrying an interior medium, a diffusion profile, a transmissive shadow path, or an SMS photon chain) |
| ~~DL-76~~ | ~~(new; opened by the `translucent` slice's P2-4 fix, review round 3, 2026-09-13; no source-doc heading)~~ | ~~`IORStackSeeding::SeedFromPoint`'s containment test is still a pair of ray probes, so a configuration of SEVERAL open surfaces that presents an away-facing sheet in BOTH probe directions is still read as containment~~ Fixed via option (a) of the row's own recipe: extend the vote to the two other principal axes, requiring unanimous confirmation across all three | CLOSED 2026-09-14 — `IORStackSeeding.h` (the extra X/Y vote in `SeedFromPoint`'s confirmation loop, originally via a per-candidate `IsConfirmedAlongAxis` helper, HOISTED same-slice review follow-up — see the perf addendum below; that helper no longer exists), `TranslucentInitialContainmentTest` sub-test 5 (`(L)`/`(M)`, 41/0) | Red-proved with the row's own prescribed fixture: a SINGLE `Object` (one `TriangleMeshGeometryIndexed`) containing TWO disjoint open quads straddling the seed along Z, one at z=+3 with normal +Z, one at z=-3 with normal -Z (both facing away from the seed at the origin) — the exact "two parallel open cards" counterexample the row named. On the unfixed Z-only P2-4 rule this is red: `FAIL: (L) DL-76 money assertion: a point between two disjoint away-facing blades of the SAME object is not seeded` (`Passed: 40  Failed: 1`); the SAME `pObj` shows positive parity along BOTH the +Z probe (crossing the top blade) and the -Z probe (crossing the bottom blade), satisfying the per-object reverse-probe confirmation even though the seed is not enclosed.  Fix: the SAME object is additionally required to confirm along X and along Y (a forward+reverse probe pair each) before it is pushed onto the stack.  Both blades lie entirely within their own bounded z=const plane, so an X or Y probe from the seed travels through z=0 forever and crosses NEITHER blade (confirmed by a fixture sanity sub-check) — the object fails the unanimity vote and is correctly rejected.  Post-fix: `Passed: 41  Failed: 0`.  A CLOSED translucent box in the SAME scene remains seeded (`(M)`), confirming the extra votes did not just disable seeding.  Gates unchanged: `TranslucentDoubleSidedTest` 44/0, `IORStackSeedingRegressionTest` 15/0, `IORStackBehaviorTest` all passed, `BSSRDFPlanarProbeReachTest` 0 failures — none of these exercise the extra X/Y probes (no fixture in them presents a Z-only-fooling object), so they are unaffected by construction, not merely unchanged by luck.  **Accepted residual, per the row's own recipe**: this is option (a)'s "cheap, still not exact" choice, not option (b)'s exact per-object closedness classification — an adversarial `Object` deliberately built from open pieces straddling the seed along ALL THREE principal axes at once (e.g. six blades, one per axis-half) would still fool the vote.  Documented in `IORStackSeeding.h`'s own comment and `docs/SUBMERGED_CAMERA_IOR_SEEDING.md` item 6's 2026-09-14 update; the pre-existing fixed-`+Z`-only single-axis-direction limitation and grazing-tangent-accumulation caveat in that same item are unrelated and unchanged.  **Same-slice review follow-up (2026-09-14, P2)**: the round-1 fix called a per-candidate `IsConfirmedAlongAxis` helper INSIDE the confirmation loop, re-tracing a fresh X and Y forward+reverse probe pair for every Z-confirmed candidate even though the probe only depends on `pos`/axis, not on which candidate is being checked — worst case up to 8 candidates x 4 probes = 32 extra `TallyProbe` calls per `SeedFromPoint`, on top of the Z pair.  Hoisted: the X/Y pairs are now traced ONCE per `SeedFromPoint` call (gated on at least one Z-confirmed candidate, same as the pre-existing Z-reverse gate) and candidates are checked by an array lookup (`HasPositiveParity`) against the shared arrays; `IsConfirmedAlongAxis` was deleted.  Real bound: **at most 6 `TallyProbe` traces per `SeedFromPoint` call** (Z fwd/rev, X fwd/rev, Y fwd/rev), independent of candidate count — measured red on the unfixed per-candidate code with a 2-nested-closed-box fixture (10 traces: 2 for Z + 4 per candidate x 2) and green post-hoist (`TranslucentInitialContainmentTest` sub-test 6, `(N)`/`(O)`, 43/0).  Same follow-up initially recorded a mirror-image FALSE-NEGATIVE residual (a legitimately closed object whose through-tunnels align with the three fixed probe axes losing the vote) — **retracted 2026-09-14 (debt-misc slice, doc-fix review pass)**: a point strictly inside the solid bounded by a closed orientable manifold has odd (net positive) crossing parity along every generic probe direction regardless of genus or tunnel alignment (the ray starts inside a bounded solid and ends outside it, so it must cross the boundary an odd number of times on any axis), confirmed directly against a real `TorusGeometry(major 3, minor 1, hole axis Y)` probed from a seed on the tube. The only real residual, shared by all three axes now instead of just Z, is a probe that grazes the surface tangentially or threads a face/edge exactly — a pre-existing hazard, not a new class. Corrected in both `IORStackSeeding.h` and `SUBMERGED_CAMERA_IOR_SEEDING.md`. | S | physics-bias | user-visible (a camera or luminaire placed between two disjoint away-facing open surfaces of the SAME object; residual narrowed to an all-three-axes adversarial construction; false-negative counterexample symmetric) |
| DL-84 | DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md: the THIRD volume continuation | `PathTracingIntegrator.cpp`'s own in-loop volume phase-scatter continuation (`IntegrateFromHitTemplated`, the `bsdfPdf = effectivePdf` site reached after a surface bounce) trains optimal-MIS with NEITHER a correct `bsdfTimesCos` NOR an `AccumulateCount`. DL-72 round 3 wired the analogous quantity at `RayCaster`'s two volume sites and PT's two BSSRDF sites, but this one was never in its list. Until 2026-09-14 it carried the PREVIOUS (surface) vertex's `bsdfTimesCos` -- a moment whose numerator and denominator come from different vertices; that is now cleared to zero (conservative: the `f2 > 0` gate simply skips training), so the defect is a training-INPUT GAP, not a wrong moment, and the render-time weight is unaffected. Recipe: set `rs2.bsdfTimesCos`'s in-loop analogue to `phasePdf` broadcast (DL-72's volume ruling: a phase function's "BSDF*cos" is its own value, which for a normalized phase equals its pdf) AND add the paired `AccumulateCount` before the Russian roulette, then extend `OptimalMISTrainingSitesTest` with a fog-box-plus-surface fixture -- its existing volume case drives `RayCaster`'s site, not this one. OPEN-confirmed 2026-09-14 (debt-guiding2 slice, review round 3). | S | precision | not user-visible (optimal-MIS training input only; alpha estimate for `optimal_mis TRUE` scenes with a surface bounce into a medium) |
| ~~DL-83~~ | ~~DL74_ENV_NEE_GUIDING_PARTITION.md: RIS-mode env-NEE/escape MIS partition violation under guiding~~ | ~~The SAME partition-of-unity violation DL-74 fixed for one-sample-MIS guiding also applies under RIS-mode guiding and was deliberately left unfixed there~~ | CLOSED 2026-09-14 (debt-guiding2 slice, review round 2) — closed BY DL-74's second repair, in the same commit | The premise this row was filed on was wrong, and the row says so rather than claiming a separate fix: it assumed the NEE side must reproduce the escape side's SAMPLING density (`risEffectivePdf`), which indeed has no closed form at an externally-fixed direction. MIS does not require that — it requires only that both sides evaluate the SAME function of direction, and that function is free (weights need only partition to one; the true densities enter the estimator solely as the untouched divisors). DL-74's repair therefore defines a NOMINAL density that does not mention the sampling mode at all, and RIS gets the identical treatment as one-sample MIS with no RIS-specific derivation. Red-proof: `tests/PTGuidingMISPartitionTest.cpp` row (e) (`eGuidingRIS`, `guidingAlpha` 0.9, skewed trained field): the env furnace read **0.204389 against a closed form of 0.6 (-65.9%)** on `8fcce0bf` and **0.599093 (-0.15%)** after. | M | physics-bias | user-visible (guided PT with an environment light, RIS sampling mode) |
| DL-05 | CLOTH_FABRIC_DESIGN.md §15 item 27 | Two-layer gapped weave with the light outside: PT under-reads BDPT/VCM by 1.28-1.55x because PT's binary NEE cannot see through the far layer's delta gap lobe; single layer or light inside is exact | OPEN-confirmed | Doc's own measured table (box/planes, gap 0.1/0.3); mechanism traced to `RayCaster::CastShadowRayTransmittance` (definition starts `RayCaster.cpp:2062`, re-derived this sweep — the previously cited ~1980 was drift) being gated to perfect-specular dielectrics only, confirmed present as described this sweep | L | physics-bias | user-visible |
| DL-06 | IMPROVEMENTS.md §"VCM env-IBL" (Session 9-13) / CLAUDE.md "Env-IBL deficit" entry | VCM env+mesh strict-tolerance residual (env-S0 <-> env-NEE MIS partition violation) — Session 13 explicitly decided to STOP and accept the disc-area baseline rather than fix it; `plank_closeup`'s VCM 0.55x (RENDERING_INTEGRATORS.md debt 28) is the same known bias class, not a new bug | OPEN-confirmed (deprioritized, not fixed) | `docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md` "Session 13 outcome"; `IMPROVEMENTS.md` lines ~1030-1046; still true in this tree — no VCM env-branch SA-MIS migration commit exists (`git log --oneline -- src/Library/Shaders/VCMIntegrator.cpp` shows no such commit after Session 13) | L | physics-bias | user-visible |
| DL-07 | CLOTH_FABRIC_DESIGN.md §15 item 17 / WETNESS_COAT_DESIGN.md §12 item 13 | `OrenNayarBRDF::hemisphericalAlbedo` over-estimates (measured ~12.6% high at roughness 0.5, ~25.6% at 1.0), which over-amplifies `fabric_material`'s energy-subtraction and `coated_material`'s Saunderson recycling denominator; not fixable in either wrapper, needs its own bake | OPEN-confirmed | `OrenNayarBRDF.cpp:148-190`'s own doc comment states the bias and that "no clean closed form exists to correct it with"; unchanged this sweep | L | physics-bias | user-visible (rough Oren-Nayar under fabric/coat) |
| DL-08 | RENDERING_INTEGRATORS.md debt 29 | `PSSMLTSampler`'s 49-lane stream layout is exceeded by `BDPTIntegrator`'s eye-subpath walk (`StartStream(16u+depth)`) at eye/volume depth >= 32, aliasing MLT's own film/lens/aperture lanes on stream 48 | OPEN-confirmed | `BDPTIntegrator.cpp:1732`; `PSSMLTSampler.cpp:59,75,146-147` (`kNumStreams` default 49, `idx = streamIndex + kNumStreams*sampleIndex`), read directly this sweep; `maxVolumeBounce` default 64 reaches depth 32 in ordinary deep-volume scenes | L | physics-bias | user-visible (MLT + deep volume/eye recursion) |
| DL-38 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: independent residuals | Translucent exit weights lose their stateful extinction/scattering factors when reevaluated through the BSDF, including HWSS companions | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentSPF` has no `EvaluateKrayNM` override; PT HWSS companions use `TranslucentBSDF::valueNM*cos/pdf`, whose data omit extinction/scattering/inside state. `BDPTIntegrator::GenerateEyeSubpathImpl` / `GenerateLightSubpathImpl` reevaluate non-delta BSDF weights; VCM/MLT share these generators. | L | physics-bias | user-visible (translucent bidirectional/HWSS transport) |
| DL-41 | DL02_TRANSLUCENT_EXIT_DENSITY.md: independent residual | Translucent Pdf/PdfNM omit Phong lobes and selection probabilities; reverse/NEE queries lack the full stateful mixture contract | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentSPF::Pdf` returns only diffuse cosine; BDPT eye/light forward densities include `selectProb`, while reverse `PathValueOps::EvalPdfAtVertex` calls Pdf/PdfNM directly. `LightSampler` area/environment RGB/NM NEE uses empty `defaultIOR`. | L | physics-bias | user-visible (translucent mixed-lobe MIS) |
| DL-47 | DL03_GUIDED_IOR_CONTINUATION.md: guide-created entry residual | A guide can replace translucent entry reflection with transmission without generating the missing entry membership | OPEN-confirmed (static evidence; red-proof pending) | TranslucentSPF entry reflection is diffuse/non-delta with null ior_stack. TranslucentBSDF permits cross-side transmission; a positive guide PDF makes one-sample PT/BDPT accept it, but the null selected-stack case retains absent membership. The next physical exit is classified as entry. This absent-state producer contract predates DL-03. | L | physics-bias | user-visible (guided translucent entry from a reflection proposal) |
| DL-49 | DL04_SSS_RADIANCE_DECISION.md: exterior index | SSS profile and random-walk boundary evaluations assume exterior air while surface sampling uses the actual enclosing IOR | OPEN-confirmed (static evidence; red-proof pending) | Profile Fresnel/GetIOR, RandomWalkSSS entry/exit refraction, PT/BDPT entry factors/adapters and rough SubSurfaceScatteringBSDF evaluations use absolute IOR against 1; SubSurfaceScatteringSPF reflection reads ior_stack.top(). Common scaling of exterior/interior indices changes an otherwise identical relative-index problem. | L | physics-bias | user-visible (SSS surrounded by non-air media) |
| DL-24 | WETNESS_COAT_DESIGN.md §12 item 2 | `CompositeSPF`'s random walk loses ~96% of the energy in the coat-over-diffuse configuration wetness needs; this design routes around it rather than fixing it | OPEN-confirmed | Doc's own measured 96% figure (`WETNESS_COAT_DESIGN.md:101,454-488`); `CompositeSPF.cpp`'s 50/50 `Pdf` and top-wins `GetBSDF` architecture is unchanged by the tree's most recent commits touching that file (`7713e509`/`9e3cf85e`/`f4336aba` fix capacity naming, gap-slant-length and a parameter name, none touch the energy-loss mechanism), confirmed this sweep | L | energy-loss | user-visible (any `composite_material` coat-over-diffuse) |
| DL-09 | REFRACTIVE_RADIANCE_SCALING.md §10.2 | No interior-segment basic-radiance factor: inside a medium whose `ior` painter varies with POSITION, `L / n^2` is conserved ALONG the ray as well as across interfaces, but RISE applies `(eta_before/eta_after)^2` only at scatter events — so a contribution gathered at an interior vertex carries the entry point's index instead of the gather point's. A second, independent inconsistency at the same DielectricSPF call site (the exit-hit Snell trace) widens the row's scope. | OPEN-confirmed; RE-OPENED 2026-09-14 after the round-1 "fix" (`ae475295`) was found to be physically wrong and REVERTED — re-scoped round 3, same day, after a further review found the round-2 recipe's literal reading is ALSO wrong (a mirror-image regression) and a second inconsistency at the same site — read the corrected analysis before re-attempting | **The round-1 diagnosis was wrong and its fix has been reverted.** It substituted the SPF's fresh EXIT-hit ior for `RadianceEtaScale`'s stale `before.top()` (via a new `ScatteredRay::etaBeforeOverride`). But `before.top()` being stale is what makes RISE's full-trip account CORRECT today: for camera(air) -> A(`n_A`) -> B(`n_B`) -> air the physics is `(1/n_A)^2 * (n_A/n_B)^2 * (n_B/1)^2 = 1`, the middle term is the interior-segment factor RISE never applies, and the stale `n_A` at the exit read is exactly that missing term folded into the true exit factor. Substituting `n_B` without adding the interior factor turns the net 1 into `(n_B/n_A)^2` — the round-1 test enshrined a **2.05x brightening of an emitter behind a passive, lossless slab** (it asserted 0.652746 where the physics is `L*T1*T2 = 0.289909`; the value it called "buggy", 0.290109, was the right answer). `tests/RadianceEtaScaleGradedIndexTest.cpp` was rewritten to assert the PHYSICS (10/0: PT and BDPT both 0.290109, ratio-to-physics 1.00069, ratio-to-the-2.25x-value 0.44475) and now pins the invariant against exactly that regression; `RefractiveRadianceScalingTest` re-verified 38/0, byte-identical to `ddf05c6c`. **The genuine residual** is the missing interior-segment factor itself, which only shows where the cancelling exit event never happens: a gather at an INTERIOR vertex C (an NEE connection or a bounce before the walk exits the graded object) carries `(1/n_A)^2` where physics wants `(1/n_C)^2` — an error of `(n_C/n_A)^2` — and likewise for a walk seeded INSIDE a graded medium (`IORStackSeeding::SeedFromPoint` does NOT record the index at the seed point itself; it fires a probe ray from the seed point and records the index at the probe's FIRST SURFACE HIT on the containing object — a point that can be arbitrarily far from the seed point on a graded `ior` object; the first segment out from the seed pays no interior factor either way). **Round 3 correction (same day): the round-2 recipe text above, "on the walk, not at the SPF exit read", is itself wrong if read as "leave the exit read untouched."** The interior factor and a switch of the EXIT crossing's own read (stale `before.top()` -> the SPF's freshly re-fetched exit value) must land TOGETHER: applying the interior factor `(n_A/n_B)^2` alone, with the exit read still stale, double-charges the segment and gives `(1/n_A)^2 * (n_A/n_B)^2 * (n_A/1)^2 = (n_A/n_B)^2` — 0.444 at this row's 1.2/1.8 fixture, the MIRROR IMAGE of the reverted round-1 bug (reciprocals of each other: `(n_B/n_A)^2` vs `(n_A/n_B)^2`) — and `RadianceEtaScaleGradedIndexTest`'s through-slab pin CANNOT tell a half-applied fix from a correct one, because both rows there are pure through-transmission and `(1/n_A)^2*(n_A/n_B)^2*(n_B/1)^2 = 1` regardless of which of the two changes landed, as long as BOTH did. Only an interior-vertex gather (which never reaches the exit read) can distinguish them. **Prerequisite the fix needs and does not have:** `n_C` — the painter's value AT an interior gather vertex — is UNDEFINED for most `IScalarPainter` forms; `GetValuesAt`/`GetValueAtNM` take a `RayIntersectionGeometric`, a SURFACE hit record (UV, geometric normal, etc.), and an interior point never generated one. Only a world-position-only form (`ExpressionPainter` reading `P`, no `u`/`v`) is queryable there; UV-driven forms (`TextureScalarPainter`, `Function2DScalarPainter`, a curve composed with a UV lookup) are not. **Second inconsistency, same site, widens the row (M -> L):** `DielectricSPF::GenerateScatteredRay`'s exit branch computes the exit ray's DIRECTION and TIR classification via `Optics::CalculateRefractedRay(-ri.onb.w(), rIndex=n_B (fresh at the exit hit), exitIOR, refracted)`, while the walk's own IOR-stack model still tracks `n_A` (the entry-time value) as the medium the ray has been travelling in, because nothing updates it along the walk — the identical gap, now visible in the Snell trace rather than in radiance accounting. At this row's 1.2 -> 1.8 fixture: the TRACED medium's (`n_A`=1.2) critical angle is 56.4 deg, but the exit test Snell-checks against `n_B`=1.8 (critical angle 33.7 deg), so an incidence angle between 33.7 deg and 56.4 deg is classified TIR by the SPF's own fresh-value test even though the walk's own tracked account never put the ray in a medium dense enough to trap it there. Fix direction (unchanged in substance, corrected in the pairing): an interior-segment factor `(n_prev/n_C)^2` applied ALONG the walk each time it queries the painter at a new interior point without a scatter event to hang it on, paired with switching `RadianceEtaScale`'s exit-crossing read to the fresh value, restricted (for now) to world-position-only `ior` painters, AND a matching fix to `DielectricSPF`'s exit-hit Snell trace so its direction/TIR classification agrees with whatever medium the walk's corrected model tracks at that point — exactly 1 (and geometrically unchanged) for every uniform-`ior` scene. Recipe: take `RadianceEtaScaleGradedIndexTest`'s graded slab, add an interior gather (a diffuse target inside it, or lower `scattering` so the walk scatters non-deltaically at an interior vertex), and compare against a piecewise-uniform reference — the same object as N concentric shells of CONSTANT `ior` stepping `n_A` -> `n_B`, where every shell boundary is a real interface and the per-crossing factor is correct by construction. The graded render must converge to the multi-shell one as N grows; today it will not. Implement the interior factor and the exit-read switch in the SAME change (a fix that lands only one, in either direction, must fail an interior-gather test even though it passes the existing through-slab one) and extend the same pass to the DielectricSPF exit-hit Snell trace. | L | physics-bias | internal (not user-visible today: no in-tree scene binds a position-dependent `ior` — `scenes/Tests/GUI/panel_stress_params.RISEscene:232`'s `ior panel_scalar_dispersion` is three per-channel CONSTANTS combined algebraically, with no `P` term) |
| ~~DL-10~~ | ~~RENDERING_INTEGRATORS.md debt 28 (fisheye residual)~~ | ~~Fisheye camera's per-pixel solid angle is computed in the pre-stretch local frame while `mxTrans` applies `Stretch(pixelAR,1,1)`~~ | CLOSED 2026-09-14 | `bf74d3ab`/`b083af79`: `tests/CameraImportanceTest.cpp::TestFisheyeFilmResponse` extended to pixelAR ∈ {0.5,1.0,2.0} — red: 966/2 (0.554694 at pixelAR=0.5, 1.511822 at pixelAR=2.0, both far from the closed form 1; pixelAR=1.0 unaffected at 1.000166). Fix: `FisheyeStretchJacobian(pixelAR, v) = pixelAR/|Stretch(pixelAR,1,1)*v|^3` (the standard linear-map-then-renormalize solid-angle Jacobian), folded into `ImportanceFisheye`/`PdfDirectionFisheye` (CameraUtilities.cpp), special-cased to exactly 1 at pixelAR==1 for bit-identical behaviour there. Post-fix `CameraImportanceTest` 968/0 (0.999988 / 1.000166 / 0.999972). Sibling audit: pinhole/thin-lens (`ComputePixelAreaAndDistance`, an affine corner-transform with no per-ray renormalization) and orthographic (no per-ray projection at all) do not share the defect. `CameraUnitConversionTest` 253/0, `SceneEditorAddCameraTest` 53/0, `SobolDimensionBudgetTest` all passed, `TextureFootprintTest` 192/0. | S | precision | user-visible (fisheye camera with non-square `pixelAR`) |
| ~~DL-29~~ | ~~WETNESS_COAT_DESIGN.md §12 item 10~~ | ~~A measured `tau` curve is grey under the default RGB rasterizer because `PiecewiseLinearScalarPainter` broadcasts one 555nm sample and reports no per-channel variation~~ | CLOSED 2026-09-14 | `27645c2a` (test) / `c4ead095` (round-1 fix, CMF integration) / `dfe4a537` (redesign, this slice). **Convention: `GetValuesAt` returns the curve evaluated at the three representative wavelengths `RISE::ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm** (Interfaces/IScalarPainter.h, shared with `DielectricSPF`'s RGB dispersion loop, which used to carry a private `kARChannelNM` copy of the same array); `HasPerChannelVariation()` is true iff those three samples differ, EXACT comparison. The round-1 CMF-integration-under-D65 answer was replaced on review: a colorimetric RGB is correct only for a slot consumed LINEARLY as a transmittance (`tau`), and most slots this painter feeds are not — `ior`/`ext`/`film_ior`/`film_extinction` feed a per-channel Fresnel, `absorption`/`scattering`/`extinction` are rates that go through `exp()`, `roughness` parameterises a lobe — so `f(∫n) ≠ ∫f(n)` and the painter cannot know its slot; `ColorUtils::XYZtoRec709RGB` additionally gamut-PROJECTS an out-of-gamut curve before the matrix multiply (`Color.cpp` ~:214-219 calls `MoveXYZIntoRec709RGBGamut`, `ColorUtils.cpp` ~:351-363), so the round-1 triple was not even the integral it documented. Sampling also needs no snap-to-uniform tolerance (three evaluations of a flat curve are bit-identical) and never clamps. Red-proof (this slice's convention, against the pre-DL-29 555 nm broadcast restored in-tree): `IScalarPainterTest` 72 passed / **9 failed** -> 81/0; `PiecewiseLinearScalarPainterRGBTintTest` 8 passed / **10 failed** -> 18/0 — the `tau` row read R=G=B=0.0771581 (R/B exactly 1) where the per-channel closed form `L·(1-R0(1.5))²·tau(λ_c)²` is 0.128355/0.0725641/0.0221849, and now reads 0.128255/0.0724608/0.0221235 (R/B 5.797 vs 5.786). Two handled consequences. (a) **requireSingle slots keep loading**: a curve at a single-scalar slot (`coated_material` coat_*, `subsurfacescattering_material`/`randomwalk_sss_material`/`hair_material` `ior`, `weave_material` warp/weft_ior and siblings, `fabric_material` sheen_roughness/weave_rotation, `generichumantissue_material` g/sca, `arealight_shaderop` N, `hair_geometry` density/length_painter) would otherwise start hard-failing on `kScalarBoundToPerChannelFmt`; `Job.cpp`'s `ResolveOrDiagnoseScalar` now binds a single-scalar VIEW (`IScalarPainter::MakeSingleScalarSlotView`, new virtual, default nullptr) — the 549 nm sample under RGB, the UNCHANGED curve under the spectral rasterizers — and warns at parse time. An inline `r g b` triple in the same slot still hard-fails: the distinction is authoring intent, not arithmetic. `ScalarPainterParserTest::TestSpectralCurveInSingleScalarSlot` pins both branches (65/0 -> 69/0). (b) **a file `ior` now renders dispersively** under RGB (`disperse = pRIndex->HasPerChannelVariation()`, `DielectricSPF.cpp` ~:394, `PolishedSPF.cpp` ~:187, `PerfectRefractorSPF.cpp` ~:227): 3 refractions + 3 Fresnel reflections per hit, inside `ScatteredRayContainer::kCapacity` = 12 but ~3x the scattered-ray work of the achromatic path. That is intended for a measured dispersion curve; the dispersive path's OWN accuracy is a separate pre-existing defect, filed here as **DL-81**. `PiecewiseLinearScalarPainterRGBTintTest` row 2 renders a file-`ior` glass and pins it against the per-channel closed form (8% band, DL-81) plus an EXACT equivalence control against the same three iors authored inline. Scene check: `scenes/Tests/Materials/Enamel/silver_reference.RISEscene` (`ggx_material` conductor, `ior Ag_n`/`extinction Ag_k` measured curves) re-pointed at `pathtracing_pel_rasterizer`, 128x128, 64 spp, `oidn_denoise FALSE`, `pixel_filter box` — BEFORE (555 broadcast) full-image linear means R=G=B=0.380640, R/B exactly 1.00000; AFTER R=0.380852 G=0.380591 B=0.380238, R/B 1.00161 (centre crop 1.00179). Small because silver is nearly spectrally flat in the visible (R0 at 611/549/465 = 0.98700/0.98290/0.97905); the point is that it is no longer exactly achromatic and the level is unchanged (+0.06%), i.e. tint gained, no energy moved. Gate: `IScalarPainterTest` 81/0, `PiecewiseLinearScalarPainterRGBTintTest` 18/0, `ScalarPainterParserTest` 69/0, `ReferenceGraphTest` 364/0, `JHWhiteGuardSpectralTest` 30/0, `ThinFilmRGBSpectralTest` 7/0, `CstDeriveContractsTest`, `DielectricARTest`, `CoatedMaterialChunkTest`, `WeaveMaterialChunkTest`, `HairMaterialChunkTest`, `FabricMaterialChunkTest` all unchanged. Round 1's new `PiecewiseLinearScalarPainter.cpp` is DELETED (the convention needs no Color/CMF headers, so it is back to a header-only class) and removed from all five build projects. **precision-slice P2-6 addendum (2026-09-14): measured `scenes/FeatureBased/Materials/thinfilm_heattint_showcase.RISEscene`'s Ta sphere (`tf_ta_090`, `fresnel_mode thinfilm`) and found the checked-in scene is UNAFFECTED by this fix, for a reason worth recording.** Ta's bare-conductor F0 genuinely swings hard under this convention — recomputed directly from `colors/thinfilm/substrates/Ta.{n,k}`: 555 nm broadcast F0 = 0.3702 (achromatic) vs `{611,549,465}` nm per-channel F0 = (0.4582, 0.3675, 0.4148), a 24.7% channel spread — and a real render of a BARE Ta conductor (`fresnel_mode conductor`, no oxide layer, scratch scene modelled on `silver_reference.RISEscene`, `pathtracing_pel_rasterizer`, 400 spp, `oidn_denoise FALSE`, `pixel_filter box`, `ddf05c6c`'s `PiecewiseLinearScalarPainter.h` swapped in for BEFORE) confirms it end-to-end: BEFORE R=0.370185 G=0.370186 B=0.370183 (achromatic, matches the 555 nm F0 to 4 digits); AFTER R=0.458155 G=0.367474 B=0.414787 (matches the closed-form per-channel F0 to 3-4 digits). **But the checked-in showcase scene uses `fresnel_mode thinfilm`, not `conductor`, and that path is untouched by DL-29**: `GGXBRDF::Directional`'s thin-film branch (`GGXBRDF.cpp` ~:88-97, `ThinFilm::ReflectanceConductorRGBSpectral`'s `stackAt` callback) already sampled `ior`/`extinction` per-channel via `GetValueAtNM` at each wavelength before this fix — it never reads the (DL-29-affected) `GetValuesAt` triple at all; only the plain-conductor branch (~:107-110) does. Rendering the ACTUAL showcase material (`tf_ta_090`, thin film, same rasterizer swap) confirms this: BEFORE R=0.472613 G=0.378164 B=0.441452, AFTER R=0.472622 G=0.378172 B=0.441457 — a <0.01% change, indistinguishable from MC noise at this sample count. **Conclusion: DL-29 has a large, confirmed, render-verified effect on plain-conductor (`fresnel_mode conductor`) GGX materials bound to a measured curve (this row's `silver_reference.RISEscene` case, and the Ta conductor-only scratch scene above), but no effect on `fresnel_mode thinfilm` materials — including every sphere in `thinfilm_heattint_showcase.RISEscene` — because that Fresnel path was already per-channel-correct.** No production scene file was modified for this measurement (scratch scenes only, not committed). | S | precision | user-visible (measured spectral tint vanishes under RGB pathtracing) |
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
| ~~DL-80~~ | ~~Utilities/Color/ColorUtils.h:17 / Color.h:71 / SpectralPacket.h:20~~ | ~~`Color.h` and `ColorUtils.h` are mutually circular via `SpectralPacket.h`'s own `#include "ColorUtils.h"`, so whichever of the two a translation unit includes FIRST wins the include-guard race and the other's declarations (`ColorUtils::XYZFromNM`, transitively `IFunction1D`) are invisible to `SpectralPacket.h` when entered the losing way~~ Cycle broken at the root: `SpectralPacket.h`/`SpectralPacket_Template.h` no longer `#include "ColorUtils.h"` at all | CLOSED 2026-09-14 (debt-misc slice) | Row filed by the `precision` slice (`11393740`, uncommitted to master at the time this slice started); its own fix commit originally worked around the landmine at its two discovered call sites (`PiecewiseLinearScalarPainter.cpp`, `tests/IScalarPainterTest.cpp`) by including `Color.h` first, not at the root.  **Superseded**: per the `precision` slice's own re-verification, its DL-29 redesign subsequently removed the code paths that needed that workaround, so those two include-order workaround sites no longer exist in its branch either way.  This row's root fix (breaking the `Color.h`/`ColorUtils.h` cycle at `SpectralPacket.h`) supersedes any such workaround regardless of whether the sites survive — the include-order hazard it worked around is gone for every caller, not just those two.  The `precision` branch's own copy of this DL-80 row is reconciled against this one at merge time to avoid a double-filing under the same id. Re-derived independently on current master (`a3aa5b8d`) by reading the three cited lines directly: `ColorUtils.h:17` `#include "Color.h"`; `Color.h:71` `#include "SpectralPacket.h"`; `SpectralPacket.h:20` (pre-fix) `#include "ColorUtils.h"`. Red-proved with two new standalone tests exercising both entry orders directly against `git rev-parse a3aa5b8d`'s unfixed headers: `tests/ColorUtilsBeforeColorIncludeOrderTest.cpp` (includes `ColorUtils.h` then `Color.h`) reproduces the EXACT reported errors — `error: unknown type name 'IFunction1D'` and `error: no member named 'XYZFromNM' in namespace 'RISE::ColorUtils'` at `SpectralPacket.h:109,246` and `SpectralPacket_Template.h:222` — while its sibling `tests/ColorBeforeColorUtilsIncludeOrderTest.cpp` (the order every existing production file happened to use) compiled clean throughout, confirming the race is real and order-dependent, not a general breakage. **Fix**: `SpectralPacket.h` and `SpectralPacket_Template.h` no longer include `ColorUtils.h` — each forward-declares the single function it actually calls (`bool ColorUtils::XYZFromNM(XYZPel&, const Scalar)`) plus `struct XYZPel;`, and `SpectralPacket.h` gained a direct `#include "../../Interfaces/IFunction1D.h"` (previously reached only transitively through the now-removed `ColorUtils.h` edge) for the `IFunction1D*`-constructor overload it defines inline. `ColorUtils.h`'s own `#include "Color.h"` is UNCHANGED (many existing files rely on it transitively) — only the back-edge that closed the cycle was cut, so the fix needs no caller to change its own include order. **Second-order breakage found and fixed in the same pass**: cutting that back-edge also removed a HIDDEN transitive path 9 other files relied on to reach `ColorUtils.h`'s declarations without including it directly (`Color.h` used to reach `ColorUtils.h` via `SpectralPacket.h`, so anything that included `Color.h` got `ColorUtils.h` for free) — a full library rebuild after the header fix failed with `no member named 'SerializeRGBPel'/'DeserializeRGBPel' in namespace 'RISE::ColorUtils'` in `CausticPelPhotonMap.cpp`/`GlobalPelPhotonMap.cpp`; a symbol-level audit (grepping every real, non-comment `ColorUtils::<exact-symbol>` call site against every file that already includes `ColorUtils.h` directly) found 9 total: `DetectorSpheres/IsotropicRGBDetectorSphere.cpp`, `Job.cpp`, `PhotonMapping/{Caustic,Global}{Pel,Spectral}PhotonMap.cpp` (4 files), `PhotonMapping/TranslucentPelPhotonMap.cpp`, `RasterImages/PPMWriter.cpp`, `Shaders/VCMIntegrator.cpp` — each given its own direct `#include ".../Color/ColorUtils.h"` rather than relying on transitive luck (two further textual hits, `Rendering/FrameStoreColorSpace.h` and `Shaders/VCMIntegrator.h`, were confirmed to be COMMENT-only references and needed no fix; `FrameStoreColorSpace.h` got a documentation-anchoring include anyway, harmlessly). Full library rebuild clean (zero warnings) after both the header fix and the 9 call-site fixes. Gate: both include-order tests green; `ColorUtilsTest` (all passed), `FrameStoreColorMathTest` (324/0), `JakobHanikaRoundTripTest` (14/0), `RGBPainterSpectralRoundTripTest` (18/0), `TexCoord1PainterTest` (33/0), `ThinFilmAnodizeSwatchTest` (24/0), `TextureExpressionVMTest` (846/0), `ThinFilmRGBSpectralTest` (7/0), `AgentObjectMapTest` (254/0), `PainterVolumeAccessorTest` (all passed) — every test file matching `grep -l 'SpectralPacket\|ColorUtils' tests/*.cpp`. **Not fixed, and not a new debt**: `SpectralPacket.h`'s own `#if 0 ... #endif` dead-code block (a `GetRGB()` overload calling `ColorUtils::RGBFromNM`, a name that has never existed — the closest real function, `ArbritaryRGBFromNM`, is itself commented out in `ColorUtils.cpp`'s explicitly-labeled "DEAD CODE section") is pre-existing, already self-labeled dead code on both ends, not a hidden landmine, and out of scope. | S | API/bridge gap | internal (fixed before any in-tree file tripped the losing order in production; the `precision` slice's DL-29 fix, if merged as-is, keeps working — its own include-order workaround is now simply redundant, not broken) |
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

**2026-09-14 (debt-slice0, DL-67/DL-69 design note's Slice 0, branched
from `32824325`; UPDATED after the fix pass on the same branch):** One
row closed, four opened. DL-67's row was NOT struck — its own subject
(PT/BDPT guided one-sample/RIS candidate branches) is untouched — but
its "Slice 0 landed" evidence note was REWRITTEN: the first version of
that fix was measured after the fact and found to make `SchlickSPF`'s
aggregate density *worse* than the code it replaced on 6 of 8
configurations, and its two gates were a self-comparison and an
algebraic identity. Both gates were deleted and replaced with a
two-sided normalisation gate and a total-variation gate against the real
sampler; see the row and `docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md` §6.
Rows in this slice's assigned id block: **DL-98** (`IsotropicPhongSPF`,
open — its *recipe* was also rewritten, because the original prescribed
a direction-dependent diffuse coefficient, i.e. the same defect the fix
pass had to undo) and **DL-99** (`AshikminShirleyAnisotropicPhongSPF`,
open — *description* corrected: its weights are wrong by factors, not
merely evaluated at a proxy direction, and it is therefore not lower
priority than DL-98 as the row previously claimed). Three further rows
were needed beyond that block and take the next free ids, **DL-100**
(`SchlickSPF`'s specular sampler uses the unflipped frame, losing the
whole specular lobe on a back-face hit — open), **DL-101** (per-channel
specular loops in `SchlickSPF` and both Ward SPFs reuse one
`ScatteredRay` across lanes — open), and **DL-102** (the anisotropic
azimuthal density was the wrong distribution — CLOSED by the same
commit). If a sibling debt-cleanup slice has been assigned an id block
containing 100-102, renumber these at merge time the way DL-65 was
renumbered to DL-67 (see the note further down this file). Net this
slice: main rows +5, 1 closed, 4 open. A full independent recount of the
whole table was NOT attempted this pass (out of Slice 0's scope; the
file's own row-count vs status-count tallies have drifted from several
unreconciled per-slice notes accumulated since the last "Authoritative
totals" line below, e.g. the 23 separately-numbered `DL-Rxx` rows use a
`CLOSED-by-code`/`CLOSED-by-test` status that is not counted by a bare
`OPEN-confirmed` grep — flagging this as a housekeeping gap for whichever
slice next needs an authoritative total, rather than silently presenting
an unverified number here).

**2026-09-14 (debt-precision slice):** DL-10 and DL-29 CLOSED (see the
table rows and their "Verification recipes" entries above). **DL-09 is
NOT closed** — its round-1 fix was physically wrong, has been REVERTED,
and the row is re-scoped and re-opened against the actual defect: the
missing interior-segment basic-radiance factor inside a
spatially-varying-`ior` medium. A round-3 review (same day, this pass)
found the round-2 recipe's literal wording ("apply the interior factor,
not a substitution at the exit read") is itself wrong — the interior
factor and the exit-read substitution must land TOGETHER or the result
is a mirror-image regression — and a second, independent inconsistency
at the same `DielectricSPF` exit-hit call site (Snell direction/TIR
classification computed against a medium the walk's own tracked model
disagrees with). Row widened **M -> L** accordingly.

Three new rows filed along the way, none fixed here. **DL-80** — a
pre-existing Color.h/ColorUtils.h circular-include landmine, found
building DL-29's round-1 fix and re-verified standalone after that fix
was replaced. **DL-81** — the RGB dispersion path's channel-index-
dependent systematic offsets, found gating DL-29's consequence (b) and
red-proved against a non-dispersive control; pre-existing, reproduces
from an inline `r g b` ior triple with no painter involved. Root cause
re-diagnosed this pass past `DielectricSPF` into `SobolSampler`'s padded
Sobol dimension-parity correlation across bounces (see the row) — broader
than RGB dispersion alone; title and evidence rewritten, size unchanged
(M). **DL-82** — the DL-29 pattern unfixed in three sibling curve
painters (Sellmeier, polynomial, Function1D), deliberately deferred
behind DL-81 rather than propagating DL-81's error to more scenes; this
pass's DL-81 re-diagnosis makes that prerequisite LARGER than it looked
when DL-82 was filed (see DL-82's row).

Main rows, counted directly against the table on THIS branch's HEAD
(this pass; supersedes the prior "72 -> 75" note, which was off by one
against this branch's actual state): **74 total — 49 open, 25 closed**
(71 main rows on the 2026-09-13 master snapshot below, +3 for DL-80/
DL-81/DL-82 added this slice = 74; DL-10 and DL-29 stay counted, now
struck-closed rather than removed). OPEN-confirmed count: closes 2
(DL-10/DL-29), opens 3 (DL-80/81/82) — net +1 open; DL-09 stays open
throughout, unchanged in count though widened in size. This slice's
branch HEAD does not itself update the "Authoritative totals on
`master`" line below; that recount happens at merge.

**Authoritative totals on `master` (recount after each merge; per-slice notes
below are each slice's own snapshot at its branch HEAD and do NOT sum):**
After the debt-guiding2 merge (2026-09-14) — **81 main rows: 43 open, 38 closed**.
Ids in use: DL-01..DL-86 minus DL-35, DL-73, DL-78, DL-79, DL-85.

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

**2026-09-13/14 (debt-ggx2 slice, base `ddf05c6c`):** DL-65 closed
(`0ad80d66`, `CookTorranceSchlickGlossyFilterConsistencyTest: 22 checks,
0 failures`) and DL-63 closed (`052ec469`,
`GGXDiffuseTransmissionTest: 150 checks, 0 failures`,
`GGXHeightCorrelatedEnergyLUTTest: 14 checks, 0 failures` new) — see the
table rows above. No new debts opened. DL-65's original "M" size
estimate is corrected to "S" on this slice's own row (the fix was two
mechanical 3-8 line additions, matching DL-62's own template exactly;
noted per COMMON_RULES "anything in the ledger row you found to be
wrong"). This slice's own snapshot at branch HEAD `052ec469`: of the
rows this slice touched, 2 -> 0 open (both closed); does not change the
master authoritative total above, which is recomputed at merge time.

**2026-09-14 (debt-ggx2 slice, review-findings follow-up, base `8b298537`):**
review of the DL-63/DL-65 closures found four P2s and several P3s (no P1s);
this pass fixes all of them. (1) `GGXHeightCorrelatedEnergyLUTTest.cpp`'s
`TestKullaContyIdentityHoldsForG2` was TAUTOLOGICAL at Schlick F0=1
(`ComputeFms(1, Eavg)` collapses to exactly 1 by pure algebra for any
`Eavg`, so its `total==1` check passed for ANY table content) — replaced
with `TestEssEavgConsistencyG2` (re-derives each `E_avg_TABLE_G2` row from
the checked-in `E_ss_TABLE_G2` values via the generator's own midpoint-rule
discretization; matches to ~2-5e-9) and `TestFurnaceStyleAtF0Point9`
(F0=0.9, where `F_ms` genuinely depends on `Eavg`; asserts the provable
`0 <= total <= F0` bound plus `F_ms < 1`). (2) Added
`TestUniformHemisphereIndependentQuadrature`: a third estimator with its
own re-implementation of GGX `D`/Smith `Lambda`/height-correlated `G2`,
its own RNG, and UNIFORM-hemisphere (not VNDF) sampling, closing the gap
that the existing quadrature and the offline generator share one VNDF
identity; matches `LookupEssG2` at the three reviewer-specified
`(alpha, mu)` points (e.g. alpha=1.0/mu=0.0156: `LookupEssG2=0.93494` vs
`uniformHemisphere=0.93486+/-0.00002`). `GGXHeightCorrelatedEnergyLUTTest:
23 checks, 0 failures` (was `14 checks, 0 failures`). (3) **New debt
opened: DL-77** (isotropic-LUT-vs-anisotropic-render mismatch in
`GGXBRDF`/`GGXSPF`'s DL-63 compensation; see the table row above) —
qualifying comments added at every `LookupEavgG2`/`LookupEssG2` call site
in `GGXBRDF.cpp`/`GGXSPF.cpp` (`CoatedBRDF.cpp`'s coat lobe confirmed
exempt: single scalar `alpha`, never anisotropic), plus a `KNOWN-FAILURE`
control row in `GGXDiffuseTransmissionTest.cpp`
(`TestAnisotropicKnownFailureDL77`) that records the measured deficit
without failing the suite. (4) `MicrofacetEnergyLUT.h`'s "AUTO-GENERATED
... DO NOT EDIT BY HAND" claim was FALSE for the hand-maintained
`MSLobeDetail`/`MSLobeZ*`/`SampleMSCosTheta*`/`MSPdf*` block (the
generator never emitted it, nor the header's `#include <array>`) —
`tools/GenerateMicrofacetEnergyLUT.cpp` now embeds that block verbatim
(`kHandMaintainedH6Block`) and emits the missing include; regenerating
into a scratch file and diffing against the checked-in header now
reproduces it BYTE-FOR-BIT identical (verified this pass; command + fixed
seed `1234567890123456789` recorded in the header's own new provenance
comment). Several P3 doc/comment corrections also landed (see
`DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md`'s "P2-1 correction" note and this
slice's commit messages). This slice's own snapshot at this pass's HEAD:
1 new row opened (DL-77); does not change the master authoritative total
above, which is recomputed at merge time. Ids in use (this slice's local
view; does not account for concurrently-filed sibling-slice rows, exactly
as the DL-65/DL-67/DL-68 renumbering note above warns) now additionally
includes **DL-77**.

**2026-09-14 (debt-misc slice, base `a3aa5b8d`):** three rows closed —
DL-80 (`SpectralPacket.h`/`SpectralPacket_Template.h` forward-declare
`ColorUtils::XYZFromNM` instead of `#include`-ing `ColorUtils.h`, breaking
the `Color.h`<->`ColorUtils.h` circular include at its root, plus 9
consumer files given the direct `#include` they were silently relying on
transitively; two include-order tests added), DL-75 (consistency pin —
`HairSSSEntryNormalTest.cpp`, 500/500, narrowed rather than eliminated:
the entry continuation can never invert by construction, but the
disk-to-surface area-Jacobian input remains ray-derived and undefined for
hair, documented as an accepted residual) and DL-76 (`IsConfirmedAlongAxis`
extends `IORStackSeeding::SeedFromPoint`'s P2-4 reverse-probe confirmation
to the two other principal axes, closing the row's own "two disjoint
away-facing blades of the same Object" counterexample;
`TranslucentInitialContainmentTest` sub-test 5, 41/0, red on the unfixed
Z-only rule at `Passed: 40  Failed: 1`). This slice's DL-80 row itself was
FILED by the `precision` slice (commit `11393740`) on a sibling,
still-unmerged branch (`debt-precision`, based on an older master `e3437f75`)
and therefore did not exist in this slice's own base commit's copy of this
file — re-derived and closed independently rather than merely struck. See
the three table rows above for full derivations. No new debts opened
(`SpectralPacket.h`'s own dead `#if 0` `GetRGB()`/`ColorUtils::RGBFromNM`
block, checked while fixing DL-80, is pre-existing, already explicitly
labeled "DEAD CODE" on both ends — `ColorUtils.cpp` has a matching
commented-out `ArbritaryRGBFromNM` in its own explicitly-labeled "DEAD CODE
section" — so it is not a hidden landmine and was not filed as a new row).
This slice's own snapshot: 3 rows closed (DL-75, DL-76, DL-80), 0 opened;
main rows at this slice's branch HEAD: **77** (DL-80 added); does not
change the master authoritative total above, which is recomputed at merge
time — that recount must also fold in whatever the `precision` slice's own
DL-80 filing does when IT merges, to avoid double-counting the same id.

**2026-09-14 (debt-ggx3 slice, base `a3aa5b8d`):** DL-77 closed (see the
table row above) — `MicrofacetEnergyLUT.h` gained an anisotropy-ratio
table (`E_ss_TABLE_G2_ANISO`/`E_avg_TABLE_G2_ANISO`) plus a per-azimuth
refinement (`E_ss_TABLE_G2_ANISO_PHI`/`LookupEssG2AnisoDirectional`) after
a ratio-only first cut regressed an existing anisotropic
`TestSchlickSweep` row into an energy GAIN; `GGXBRDF.cpp`/`GGXSPF.cpp`
repointed their Kulla-Conty Ess_i/Ess_o/Eavg/MS-lobe-sampler lookups
accordingly, byte-identical at `alphaX==alphaY`. One new row opened this
slice: **DL-86** (the pre-existing, separately-documented LUT left
end-cap residual from DL-63's closure, promoted from doc prose to a
tracked row per the DL-77 recipe's instruction; not fixed — see the table
row's recipe). `GGXDiffuseTransmissionTest`'s DL-77 known-failure control
was promoted to a real two-sided gate with 2 added anisotropic rows
(`151/0` -> `153/0`). This slice's own snapshot at this pass's HEAD: 1
row closed (DL-77), 1 new row opened (DL-86) — **76 main rows: 47 open,
29 closed** at this slice's HEAD; does not change the master authoritative
total above, which is recomputed at merge time. Ids in use (this slice's
local view; does not account for concurrently-filed sibling-slice rows)
now additionally includes **DL-86**.

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
- Main rows: **66** — **47 open**, **19 closed**.
- OPEN-confirmed: **47**, including the two reproduced baseline gate residuals
  DL-60/DL-61. DL-35 remains deliberately absent. Re-sweep 2026-09-12: all
  47 independently re-verified against `876c9a26`; 0 reclassified. Debt-
  cleanup slice `translucent` (2026-09-13) subsequently closed DL-39/45/46
  (47 -> 44 open) and opened DL-68 (a distinct sibling found during DL-45's
  audit: the entering-transmission lobe shares the "state-transitioning
  lobe skips the geometric-horizon check" pattern but at a different site
  with a Phong, not cosine, lobe shape), netting 44 + 1 = 45 open — this
  was the figure the "Main rows" line above reported before this same
  slice's own P2 vGeomNormal-consumer audit (below) opened one more row;
  an earlier draft of this note stopped the arithmetic at "44 open"
  without adding back the newly-opened row, which contradicted the
  Main-rows count above it (fixed 2026-09-13, same slice). The same
  slice's fix round (P1/P2) then opened DL-70 (a sibling of the same
  "double-sided mesh vGeomNormal orientation" pattern found auditing
  consumers of the flag, not fixed in this slice), netting 45 + 1 = 46
  open; its review round 3 then opened DL-76 (the documented residual of
  the P2-4 open-geometry seeding fix), netting 46 + 1 = **47 open**.
  **DL-70 was RE-SCOPED in that round**: the "~30 material files" figure
  it carried was wrong -- the shared `geomNRaw` idiom re-anchors to
  `ri.ray.Dir()` and is invariant under the double-sided flip, so all 59
  of those occurrences are immune. Every site was opened and classified
  individually; the row now lists exactly the 14 genuinely sign-sensitive
  ones (LightSampler x2, BDPTIntegrator medium push/pop, RayCaster's
  transmissive-shadow `bEntering`, DirectVolumeRenderingShader x2,
  SMSPhotonMap's `bEntering`, and the six BSSRDF front-face gates) plus
  `TransparencyShaderOp` x2 as a possible no-op-by-intent, with a
  per-site recipe.
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
  DL-39, `1fe5c760`, 2026-09-13, translucent photon deposition;
  DL-45, `14b06223`, 2026-09-13, translucent exit geometric-horizon gate;
  DL-46, `cd43da09`, 2026-09-13, translucent initial-containment seeding).
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

**DL-09 (RadianceEtaScale spatially-varying ior — interior gather AND
DielectricSPF exit-hit Snell trace).** `tests/RadianceEtaScaleGradedIndexTest.cpp`
now exists and pins the through-slab net at exactly 1 (no eta factor) —
that file's two rows are pure through-transmission and pass today, by
construction, whether or not any fix has landed; they are a REGRESSION
GUARD on the reverted round-1 bug, not a red-proof of the open residual.
The still-open recipe: extend that graded slab with a diffuse gather
target INSIDE the object (or lower `scattering` so the walk scatters
non-deltaically at an interior vertex, restricting the `ior` painter to
a world-position-only `expression` — a UV-driven form has no defined
value at an interior point, see the row's evidence), and compare against
a piecewise-uniform reference built as N concentric shells of CONSTANT
`ior` stepping `n_A -> n_B`, where every shell boundary is a real
interface and RISE's existing per-crossing factor is correct by
construction; the graded render must converge to the multi-shell one as
N grows. Fixed when (a) an interior-segment factor `(n_prev/n_C)^2` is
applied along the walk AND (b) `RadianceEtaScale`'s exit-crossing read
switches from the stale `before.top()` to the SPF's freshly re-fetched
value — BOTH changes, landed together: either alone fails the new
interior-gather test (in opposite directions — see the row's evidence
for why landing only (a) is a mirror-image regression of the reverted
round-1 bug, not a fix) even though either alone still passes the
existing through-slab pin. Also fixed in the same pass: `DielectricSPF`'s
exit-hit `CalculateRefractedRay` call, whose direction/TIR classification
uses the fresh exit-hit `ior` value against a walk that (pre-fix) still
tracks the entry-hit value — make it consistent with whatever medium the
walk's corrected model tracks at that point instead.

**~~DL-10 (fisheye pixelAR Jacobian)~~ CLOSED 2026-09-14 — `bf74d3ab`/`b083af79`,
`CameraImportanceTest` 968/0.** Kept as the regression spec. Author a
fisheye-camera scene with `pixelAR != 1` and a uniform-radiance sphere
filling the frame (the same fixture
`tests/CameraImportanceTest.cpp::TestFisheyeFilmResponse` uses at
`pixelAR = 1`, where it integrates to 1.000166). Expected: at `pixelAR !=
1` the closed-form integral departs from 1 by more than measurement noise
(red: 966/2, 0.554694 at pixelAR=0.5, 1.511822 at pixelAR=2.0). Fixed when
`Importance`'s per-pixel solid-angle term folds in the full Jacobian of
`normalize ∘ Stretch(pixelAR,1,1)` and the extended test converges to 1 at
several `pixelAR` values (post-fix: 0.999988 / 1.000166 / 0.999972 at
pixelAR ∈ {0.5, 1.0, 2.0}). See the table row above for the fix and
sibling audit.

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

**~~DL-29 (measured tau curve grey under RGB)~~ CLOSED 2026-09-14 —
`dfe4a537`/`c4ead095`, `IScalarPainterTest` 81/0,
`PiecewiseLinearScalarPainterRGBTintTest` 18/0,
`ScalarPainterParserTest` 69/0.** Kept as the regression spec, corrected
to the shipped convention (a CMF integral was tried and superseded on
review — see the table row above for why `f(∫n) != ∫f(n)` on most of
this painter's non-`tau` slots makes a colorimetric integral the wrong
answer). Give `PiecewiseLinearScalarPainter` (or a sibling) an RGB-aware
evaluation mode so `HasPerChannelVariation()` can return true for a
measured spectral curve. Fixed when `GetValuesAt` returns the curve
evaluated at the three representative wavelengths
`ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm (not a CMF
integral), `HasPerChannelVariation()` is true iff those three samples
differ (exact comparison), and a render regression shows the curve's
tint surviving under `pathtracing_pel_rasterizer` without the
three-number RGB authoring idiom (red: `IScalarPainterTest` 9 failed,
`PiecewiseLinearScalarPainterRGBTintTest` 10 failed, against the
pre-fix 555 nm broadcast — the `tau` row read R=G=B=0.0771581 where the
per-channel closed form is 0.128355/0.0725641/0.0221849).

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

**DL-68 (translucent entering-transmission geometric horizon).** Build a real closed translucent object (mirroring `TranslucentInitialContainmentTest`/`TranslucentTiltedExitTest`'s fixtures) with the shading normal tilted at an OUTSIDE entry point, and sample the real `Scatter`/`ScatterNM` entering branch many times. Assert every emitted `trans` ray that carries a pushed `ior_stack` is geometrically into the solid (`dot(dir, -trueOutward) > 0`), across a tilt sweep, both the isotropic and per-channel-N (anisotropic) paths, and RGB/NM. For N=1 the entering Phong lobe is a plain cosine lobe, so DL-45/DL-68's own `ExitValidFraction`/exact two-draw remap (see `SampleValidDiffuseExit`, `TranslucentSPF.cpp`) transfers directly -- reuse it, mirrored onto the flipped/inward normal, rather than re-deriving. For N != 1, derive the correct valid-fraction normalization for a `cos^N` lobe clipped by a tilted plane (the disk-projection equivalence Malley's method relies on is specific to N=1; either solve the clipped-Phong integral in closed form for an exact remap in the same spirit as DL-68's fix, or use rejection sampling against the unclipped Phong distribution with the resulting empirically-exact conditional density -- being mindful of DL-68's own finding that a variable-length rejection loop shifts a fixed-dimension-budget sampler's later draws). Audit the exit branch's own backscatter `trans` lobe (same unchecked Phong shape, no stack-transition consequence) as a same-pattern sibling; fix both in the same pass if confirmed. Static + empirical evidence only; no rendered-scene bias measurement yet, and the cited entering-branch counts are not backed by a committed test.

**DL-70 (double-sided `vGeomNormal` orientation in sign-sensitive consumers).** The row itself carries the full classification and a per-site recipe -- start there, and do NOT re-derive the "shared idiom" claim: the `geomNRaw`/`ri.ray.Dir()` idiom is invariant under the flip and all 59 of its occurrences are immune. Pick 2-3 of the 14 confirmed sites (the two medium push/pop families and `RayCaster.cpp` :2153 are the highest-value), build a CLOSED DOUBLE-SIDED mesh fixture per `tests/TranslucentDoubleSidedTest.cpp`'s programmatic pattern, and red-proof each before fixing. The fix is the one-liner `const Vector3 trueGeomNormal = ri.bGeomNormalOrientedToRay ? -ri.vGeomNormal : ri.vGeomNormal;`, except on a path that can hit hair, where `RayIntersectionGeometric::bGeomNormalRayDerived` must be honoured by SKIPPING instead of recovering.

**DL-76 (open-geometry containment in the IOR-stack seeding probe).** Extend `TranslucentInitialContainmentTest`'s sub-test 3 fixture with a SECOND open single-sided translucent card BELOW the seed point, normal pointing -Z, so both probe directions meet an away-facing sheet; assert the seed stack stays empty. Keep the existing positive control (the CLOSED single-sided translucent box must still be seeded) and sub-test 4's hair guard green. Two candidate fixes are spelled out in the row; prefer the build-time closed/open mesh classification if the extra prepare-time pass is acceptable, since the multi-direction vote does not fix an exactly-symmetric two-card case.
