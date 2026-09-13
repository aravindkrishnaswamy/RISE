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
the delta. Every verdict below is cited to a file:symbol, test, or commit
— no verdict is taken on the source document's word alone for anything
marked OPEN, and every item enumerated in any of the seven source ledgers
now appears in exactly one section below (OPEN, Already resolved,
Doc-rot, or Not-a-debt).

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
| ~~DL-03~~ | ~~RENDERING_INTEGRATORS.md debt 31 item 1 / REFRACTIVE_RADIANCE_SCALING.md §10.3~~ CLOSED 2026-09-12 — `8a9bdb18`, `TranslucentIORStackTest: ALL TESTS PASSED` | ~~Guided translucent exits lose their popped IOR stack~~ An available selected exit transition is preserved or rejected according to the accepted direction and shared by training/eta consumers; missing entry-state generation remains DL-47 | CLOSED-by-test | Red on unfixed `00bdcef5`: four failed assertions (`d3a5e732`); real trained PT RGB/NM outward substitutions, inward controls and later same-object classification. Additional BDPT eye/light RGB/NM coverage; eye RIS actual-guide limitation remains DL-43. See `DL03_GUIDED_IOR_CONTINUATION.md`. | M | physics-bias | user-visible (eligible guided translucent continuations) |
| ~~DL-04~~ | ~~REFRACTIVE_RADIANCE_SCALING.md §10.1~~ | ~~Unsettled extra eta-square factor for complete SSS events~~ No unmatched factor belongs on the exterior-to-same-exterior event | CLOSED 2026-09-12 (consistency pin) | `1b705ce1`: SSSRadianceScalingTest unchanged-library baseline 572093 checks, 0 failures; both deliberate eta directions fail all six SSS air-channel checks. Independent helper plus matched explicit-volume/diffusion/RW camera matrix; distinct normalization/non-air support/MIS defects are tracked as DL-48 through DL-53. See [decision](DL04_SSS_RADIANCE_DECISION.md). | M | physics-bias | convention pinned; separate SSS defects remain open |
| DL-34 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `interior(r)` UNDER-READS inside a UNION composite's overlap: the exported `min(f_A,f_B)` is a lower bound everywhere but exact nowhere inside the seam, where the true depth is `max(depth_A,depth_B)` | OPEN-confirmed | Doc's own measured example (two R=2 spheres 1.5 apart, point (0.4,0,0): operand depths 1.6/0.9, exported 1.6, true union depth 1.886796); `ObjectManager::DeepestOtherContainment` (`ObjectManager.cpp:861`) and `CSGObject`'s union path confirmed this sweep to still export the operand min, not the deeper operand, inside an overlap | M | physics-bias | user-visible (under-painted contact inside a union seam) |
| DL-37 | IMPROVEMENTS.md "GGX low-F0 grazing gain — FIRST MEASURED 2026-09-01, unowned" | `ggx_material` in `eFresnelSchlickF0` mode goes over unity at grazing incidence (ρ = 1.1573 at 80°) because the glTF diffuse-energy split weights the diffuse lobe by the angle-flat `1 − max(F0)` while the Schlick specular term it is meant to complement rises toward 1 as `cos θ → 0` | OPEN-confirmed | `tests/LayeredWhiteFurnaceTest.cpp:1788-1789` (config 17, "White GGX-PBR base alone", `kPostureKnownFailure`) measures ρ = {0.9988, 0.9994, 1.0251, 1.1573} at θ = {0°,30°,60°,80°}; mechanism read directly in `GGXBRDF::albedo` (`GGXBRDF.cpp:514-520`: `diffColor * max(0, 1 − maxF0) + F(θ)`, doc comment at 508-513 stating the Schlick branch evaluates Fresnel at the actual outgoing cosine while diffuse keeps the constant glTF split) and reproduced at sample time in `GGXSPF::Scatter`/`ScatterNM` (`GGXSPF.cpp:216-219`, six analogous sites at 214/260/364/500/545/638) and `GGXBRDF::value`/`valueNM` (nine analogous sites at 209/276/330/399/451/490/514/595/638) — same `1 − maxF0` constant used at every one, confirmed this sweep | M | physics-bias | user-visible (low-F0 GGX at grazing incidence) |
| DL-42 | DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals | PT's BSDF-surviving one-sample guiding branch drops selected-lobe probability compensation | OPEN-confirmed (static evidence; red-proof pending) | `PathTracingIntegrator.cpp` initializes `scatterThroughput = kray/selectProb`, then the trained-guiding BSDF branch replaces it with `kray*pdf/combinedPdf` without selectProb. Shared RGB/NM loop; ordinary mixed-lobe entry reflection is reachable. | M | physics-bias | user-visible (path guiding and mixed-lobe materials) |
| DL-43 | DL02_TRANSLUCENT_EXIT_DENSITY.md: review residuals | BDPT eye guiding swaps incoming/outgoing directions when evaluating forward candidate PDFs | OPEN-confirmed (static evidence; red-proof pending) | `BDPTIntegrator.cpp` eye RIS and one-sample guide candidates pass `(gDir,-currentRay.Dir())` to `PathValueOps::EvalPdfAtVertex`; its contract is Pdf(outgoing given incoming). Light twins pass `(-currentRay.Dir(),gDir)`. Both RGB/NM instantiate the eye code. | M | physics-bias | user-visible (BDPT eye path guiding) |
| DL-39 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: independent residuals | Dedicated translucent photon deposition counts absorbed power as deposited power | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentPelPhotonTracer::TracePhoton` sums only propagated non-diffuse `kray`, then stores `power*(1-accum_scattered)`; at an inside exit with scattering zero it stores all power regardless of extinction. `TranslucentPelPhotonMap::RadianceEstimate` does not restore the missing Beer attenuation. | M | physics-bias | user-visible (translucent photon maps) |
| DL-44 | DL36_EMITTER_NEIGHBOUR_PIN.md: review residual | Sampled emitter UV is omitted from LightSample and downstream rebuilt emission records | OPEN-confirmed (static evidence; red-proof pending) | `LightSampler.cpp::SampleLight` sets local RGB `rig.ptCoord = coord`, but `LightSample` carries no UV. BDPT NM/HWSS emission rebuilds and LIGHT root, and VCM sampled-emitter evaluation retain default (0,0); `CheckerPainter` consumes ptCoord. MLT shares BDPT generation. | M | physics-bias | user-visible (UV-textured luminaries under bidirectional/spectral transport) |
| DL-45 | DL03_GUIDED_IOR_CONTINUATION.md: tilted-frame residual | TranslucentSPF samples geometrically inward diffuse exits under tilted shading normals and still pops the IOR stack | OPEN-confirmed (observed defect pin; correctness red-proof pending) | `TranslucentSPF.cpp` explicitly exempts exit re-emission from its geometric-horizon gate; RGB/NM exits sample around onb.w and unconditionally pop. DL-03 real-SPF fixture recorded 1021/4096 unchanged inward exits per RGB/NM unguided run at 60-degree shading-normal tilt. Exit Pdf also omits the geometric gate. | M | physics-bias | user-visible (translucent materials with perturbed shading normals) |
| DL-46 | DL03_GUIDED_IOR_CONTINUATION.md: initial-containment residual | Camera/light origins inside closed translucent objects lack initial IOR-stack membership and misclassify their first exit as entry | OPEN-confirmed (static evidence; red-proof pending) | `TranslucentMaterial` inherits invalid/non-refracting default `GetSpecularInfo`; `IORStackSeeding::SeedFromPoint` accepts only valid canRefract materials. Both TranslucentSPF scatter variants classify exclusively by containsCurrent. Shared PT camera and BDPT eye/light seeds skip this stateful non-refracting material. | M | physics-bias | user-visible (origins inside closed translucent objects) |
| DL-52 | DL04_SSS_RADIANCE_DECISION.md: planar probe origin | BSSRDF entry probes skip nearby points on a flat surface by advancing from the projection plane before intersecting | OPEN-confirmed (rendered symptom and static mechanism; correctness red-proof pending) | BSSRDFSampling::SampleEntryPoint starts both probe directions on the exit tangent plane and advances by 1e-6 before tracing. A normal probe skips the coplanar near face; distant bottom/side hits do not restore its profile mass. PT and BDPT eye/light RGB/NM share the helper. | M | physics-bias | user-visible (diffusion SSS on planar or nearly planar geometry) |
| DL-53 | DL04_SSS_RADIANCE_DECISION.md: recursive environment MIS | Recursive RayCaster misses bypass environment MIS when passed the global map explicitly | OPEN-confirmed (static evidence; red-proof pending) | RayCaster RGB/NM/HWSS explicit pRadianceMap branches return raw radiance before their global-map MIS blocks. Complete PT diffusion/RW continuations pass the global map and a positive cosine PDF after adding weighted environment NEE, producing an extra contribution. Iterative PT already checks map identity correctly. | M | physics-bias | user-visible (SSS and eligible recursive environment continuations) |
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
| DL-17 | CLOTH_FABRIC_DESIGN.md §15 item 12 | glTF `anisotropy_rotation` is still dropped at import, even though the expression VM's `atan2` (confirmed present) makes the sketched fix executable today | OPEN-confirmed | `GLTFSceneImporter.cpp:1300-1307`'s comment stands; `ChunkParserRegistry.cpp:4560`'s `anisotropy_rotation` descriptor still reads "Phase 1 reads but does not yet APPLY the rotation" — read directly this sweep | S | API/bridge gap | user-visible (glTF import only) |
| DL-23 | CLOTH_FABRIC_DESIGN.md §15 item 16 (tail) / IMPROVEMENTS.md "Clearcoat over `fabric_material` — not composable, unowned" | `coated_material`'s substrate allowlist does not admit `fabric_material`/`weave_material`, so a coat-over-fabric composition (e.g. waxed canvas) is unreachable | OPEN-confirmed | `CoatedMaterial.h:112-113` (`SubstrateAllowlistText()`: "lambertian_material, orennayar_material, ggx_material, pbr_metallic_roughness_material") and `IsSupportedSubstrate` `:120-134` (the `dynamic_cast` allowlist) — `fabric_material`/`weave_material` absent from both, confirmed this sweep; IMPROVEMENTS.md's entry adds that this is a named glTF-import consequence (`KHR_materials_sheen` + `KHR_materials_clearcoat` together lose the clearcoat layer, warn-and-skip named in `GLTFSceneImporter.cpp`) | S | API/bridge gap | user-visible (authoring: can't compose a coat over fabric) |
| DL-26 | WETNESS_COAT_DESIGN.md §12 item 6c | `add_wetness` and `add_wear` mutually exclude on one material; worn-and-wet, the flagship subject, is unreachable | OPEN-confirmed | `src/Library/Agent/AgentSession.cpp` ~8021/8092/8328 ("add_wear / add_wetness cannot currently be combined on one..."), confirmed present this sweep | S | API/bridge gap | user-visible (agent-authored worn-and-wet materials) |
| DL-28 | WETNESS_COAT_DESIGN.md §12 item 9 | Water-absorption spectral files must be pre-converted to a transmittance base because `dielectric_material`'s `tau` is `pow(tau,distance)`, not `exp(-sigma*distance)`; a pasted-in published sigma_a table is silently wrong | OPEN-confirmed | `DielectricSPF.cpp:318-323` (`pow(tauVals.v[i], distance)`), confirmed unchanged this sweep; no runtime validation or warning exists for a mismatched-convention input file | S | API/bridge gap | user-visible (authoring trap only, silent) |
| DL-32 | CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | `standard_object`'s `scale` written with ONE number (e.g. `scale 0.35`) derives to a degenerate transform silently — no diagnostic, the object vanishes from the render, and `DistanceToSurface`'s `sigma_min<=0` gate then refuses every proximity query against it | OPEN-confirmed | `ChunkParserRegistry.cpp`'s `standard_object` `scale` descriptor (`DoubleVec3`, no partial-fill diagnostic); `Object.cpp:1708` warns only for an ANISOTROPIC transform's `proximity()`, not a degenerate one — confirmed no parser-side warning for a partially-specified `DoubleVec3` this sweep | S | API/bridge gap | user-visible (silent scene-authoring trap) |
| DL-18 | CLOTH_FABRIC_DESIGN.md §15 item 13 | The Blender bridge has no sheen, anisotropic, or velvet mapping at all — Principled's Sheen sockets have no `fabric_material` target | OPEN-confirmed | No `fabric_material`/`sheen` reference found in the Blender bridge sources this sweep (`grep -rl fabric_material` under the Blender add-on tree returns nothing) | M | API/bridge gap | user-visible (Blender-authored scenes only) |
| DL-25 | WETNESS_COAT_DESIGN.md §12 item 6b | Phase 1 cannot darken a textured substrate: the expression VM has no painter-sampling builtin | OPEN-confirmed | `src/Library/Painters/ExpressionEval.h` function table (~lines 1166-1171) has no painter-sample builtin alongside `sin`/`cos`/`atan2`/etc.; confirmed absent this sweep by grep | M | API/bridge gap | user-visible (wet textured substrates can't darken) |
| DL-19 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | Two S3 conversion sites (BDPT's NM-hero `Le` rebuild, the HWSS companion `rigW` rebuild) share the signals-replay helper but have no dedicated red-proof — their contribution is MIS-weighted to a few percent on the money test's scenes, so skipping them moves the suite by <= 5.7% / 0% | OPEN-confirmed (test gap) | Doc's own §10 disclosure, confirmed current (no new red-proof test added for these two sites since — `tests/SignalEmitterRecordTest.cpp` unchanged in this tree per `git log` this sweep) | S | coverage/test gap | internal (test-suite blind spot) |
| DL-27 | WETNESS_COAT_DESIGN.md §12 item 7 | The wet-highlight variance cost is unmeasured | OPEN-confirmed | No test or scene mentioning "wet_highlight"/"WetHighlight" found in `tests/` or `docs/*.md` this sweep other than the design doc itself | S | coverage/test gap | internal (measurement gap, not a known defect) |
| DL-30 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 1 (disclosed residual) | A CSG exit-designated subtraction branch's `dndu` pairing is unverified, reachable only through a nested-CSG construction no test currently produces | OPEN-confirmed, untested | Doc's own disclosure (§14 item 1, appended when item 1 was RESOLVED 2026-08-29); no nested-CSG `dndu`-pairing test found in `tests/CsgSurfacePayloadTest.cpp` or elsewhere this sweep | S | coverage/test gap | internal (no scene exercises it yet) |
| DL-40 | DL01_TRANSLUCENT_EXIT_WEIGHT.md: review residual / DL36_EMITTER_NEIGHBOUR_PIN.md: harness sibling | Balance and signal-emitter harness comparisons can accept nonfinite candidate statistics | OPEN-confirmed (static evidence; red-proof pending) | BDPT/VCM `ComputeStats` accepts nonfinite capture values and `ChannelsAgree` rejects only `fabs(a-b)/denom > tolerance` (false for NaN). SignalEmitterRecordTest similarly marks nonempty captures valid and `WorstRelDiff` uses fmax, which can discard NaN differences. Recorded DL-01/DL-02/DL-36 results are finite. | S | coverage/test gap | internal (false-green risk) |
| DL-20 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 2 | Patch geometries report flat curvature (`valid=false`) while genuinely curved; deferred to Phase 4, no Phase-4 work has landed | OPEN-confirmed | No patch-geometry curvature override exists (only `EllipsoidGeometry`/`DisplacedGeometry` override `ComputeAnalyticalDerivatives`, confirmed this sweep alongside DL-13) | M | coverage/test gap | user-visible (curvature-driven wear on patch geometry reads absent, not wrong) |
| DL-21 | GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 5 | CSG boundary curvature behaviour is unspecified/undecided (forward the contributing surface's curvature, or invalidate at the seam) | OPEN-confirmed | `tests/CsgSurfacePayloadTest.cpp:833-834` exercises the derivative fields there but does not pin a curvature convention at the boundary — confirmed by reading the referenced lines this sweep | M | coverage/test gap | user-visible (CSG seam wear masks) |
| DL-22 | SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §10 | BSSRDF entry vertices read default (neutral) `derivatives`/`signals` under both PT and BDPT — integrator-consistent, but a signal-keyed IOR/Fresnel painter at a subsurface entry point is neutral rather than live | OPEN-confirmed | Doc's own §10 first bullet; closing it needs a probe record in `BSSRDFSampling::SampleResult`, confirmed absent this sweep | M | coverage/test gap | user-visible (signal-keyed SSS entry only) |
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

## Counts

Updated for the 2026-09-12 DL-59 SMS normal-derivative closure. The original sweep counts
remain historical in the header; the current table counts are below.

- Main rows: **58** — **44 open**, **14 closed**.
- OPEN-confirmed: **44** (the original 36 minus DL-01/DL-02/DL-36/DL-03/DL-04, plus independent
  residuals DL-38 through DL-59 minus closed DL-48/DL-50/DL-51/DL-54/DL-55/DL-56/DL-57/DL-58/DL-59; DL-35 remains deliberately absent)
- CLOSED-by-cleanup: **14** (DL-01, `1239edf2`; DL-02, `a041e51d`;
  DL-36, `ac9891f3`, consistency pin retaining the bounded approximation;
  DL-03, `8a9bdb18`; DL-04, `1b705ce1`, convention consistency pin;
  DL-48, `12a7ef3e`; DL-50, `29ce61c7`; DL-51, `34434610`; DL-54, `ab85092d`; DL-55, `cba4e88c`; DL-56, `df7e3dad`; DL-57, `c187f2cc`; DL-58, `1b1909c0` / `ed8d9c94`; DL-59, `a0c4a808`)
- CLOSED-by-sweep (heading was open/unlabeled; a sweep found it actually
  fixed and struck it): **7** (DR-01 .. DR-07, unchanged this pass)
- Already RESOLVED in source, independently re-verified: **23** (DL-R1 ..
  DL-R17 from the original sweep and the gap-closure pass; DL-R18..DL-R23
  added this pass, covering WETNESS_COAT_DESIGN.md §12 items 3, 5, 6, 6a,
  11 and 12 — one of these, DL-R20, is resolved-in-code but flagged as
  tested only indirectly rather than by a dedicated regression)
- NOT-A-DEBT: **27** (unchanged this pass)
- UNVERIFIABLE: **0**

Total items re-verified or newly placed by the final completion pass: 1 new
OPEN row (DL-37) + 6 new Already-resolved rows (DL-R18..DL-R23) + 1 second
source pointer added (DL-23) + 1 recipe rewritten with a named scene,
protocol, metric and a stated-proposed threshold (DL-27) = **8 items
classified or substantively rewritten**, this pass.

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
are not independent repetitions, and DL-48 through DL-53 retain the distinct
normalization, relative-index, survival, fallback, support and recursive-MIS
issues. See [the complete decision record](DL04_SSS_RADIANCE_DECISION.md).

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

**DL-34 (interior under-reads inside a union overlap).** Change the union
composite's interior/depth export inside the zero-set overlap from
`min(f_A,f_B)` to `max(depth_A,depth_B)` (matching the doc's own worked
example), guarded by a `CrossObjectProximityTest`/`ProximitySignalTest`
case at the doc's exact two-sphere configuration asserting the exported
depth is within measurement tolerance of the analytically-derived
1.886796, not the under-reading 1.6.

**~~DL-36 (bounded-neighbour-read residual on a luminary probe).~~ CLOSED
2026-09-12 as a consistency pin — `ac9891f3`, `Passed: 16  Failed: 0`.**
The two-blade fixture in SignalEmitterRecordTest directly proves the
within-band neighbour is accepted with its live channel and that replay
preserves sampled geometry. Unobstructed and reverse-normal controls
bound the behavior. This follows the original recipe's permitted
retain-and-pin option; the unchanged implementation passed, so no red
physics failure or corrected signal discontinuity is claimed. See
[fixture, limits and audit](DL36_EMITTER_NEIGHBOUR_PIN.md).

**DL-37 (GGX low-F0 grazing gain).** Flip `tests/LayeredWhiteFurnaceTest.cpp`
config 17's posture from `kPostureKnownFailure` (1788) to an expected-pass
posture (`kPosturePass` or `kPostureMatchesPrediction`) asserting ρ <= 1 +
tolerance at every one of the four angles, not just the three that already
pass. State the physically right weighting before flipping it — two
candidates, both already precedented elsewhere in this file: (a) a
per-angle Fresnel-weighted diffuse term `1 − F(θ)` in place of the
angle-flat `1 − max(F0)`, applied at every one of the fifteen call sites
DL-37's evidence lists (six in `GGXSPF.cpp`, nine in `GGXBRDF.cpp`); or (b)
a Kulla-Conty-style product-law compensation, matching the pattern
`FabricBRDF.h:204-206` already uses to blend a reflect and a transmit arm
by directional albedo (`scale(l,v) = (1 − m·Ehat(a,|n·v|))·(1 −
m·Ehat(a,|n·l|)) / (1 − m·EhatMean(a))`) rather than a constant subtraction
— the same shape would make the diffuse weight fall off toward grazing
instead of staying pinned at 0.96 while the specular term rises under it.
Either fix must be extended to the conductor-mode bare row IMPROVEMENTS.md's
"Scope note" flags as unmeasured (add a config 17-shaped conductor case to
the same test) before this row closes, since a fix scoped to
`eFresnelSchlickF0` alone would leave that row unverified. Re-measure
config 20 (the anisotropic GGX twin, which shares the same disposition per
its own comment at 1898-1900) at the same time — a fix that doesn't move it
too is incomplete.

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

**DL-42 (PT guided selected-lobe compensation).** Add a regression through
trained one-sample guiding with an ordinary entry reflection on a material
with multiple nonzero lobes. Force the BSDF proposal branch and compare
throughput against `kray*conditionalPdf/(selectProb*combinedPdf)`, using
different lobe probabilities. Cover RGB/NM and verify guided/RIS siblings
against their own actual proposal measures; inspect HWSS separately.
Avoid relying on ordinary translucent exit reachability: PT disables
guiding for specular-classified arrivals. Static evidence; red proof pending.

**DL-43 (BDPT eye guiding PDF argument order).** Drive the real eye
RIS and one-sample guided candidate paths at a Lambertian vertex with
non-normal outgoing direction and assert the evaluated PDF equals that
outgoing cosine over pi, not the incoming cosine. Cover RGB/NM and use
light-side candidate evaluations as a control. Fix incoming/outgoing
argument order at the actual producer and inspect all sibling candidate
and reverse-density calls. Static evidence; red proof pending.


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
furnace remains a coarse convention guard because DL-49 through DL-53
are separate open defects.

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


**DL-52 (BSSRDF planar probe-origin omission).** Commit a direct real-object
helper regression whose deterministic normal-axis samples on a broad flat
face must reach the nearby surface. Red-prove omitted support, then design
a finite chord traversal that includes the projection plane and counts
each hit once. Test positive/negative axes, flat/curved objects, thin and
disconnected geometry, and actual mixture PDFs/hit-count weighting. Audit
RGB/NM PT and BDPT eye/light consumers (VCM/MLT share BDPT) and probe caps.
Validate a conservative furnace and geometry convergence without changing
the complete-event eta convention or absorbing Sw normalization into geometry.


**DL-53 (recursive explicit-global-map MIS bypass).** Commit a real
RayCaster miss-ray regression with positive BSDF PDF and compare null
map versus explicit scene-global-map pointers against the same analytic
MIS-weighted radiance. Red-prove RGB/NM/HWSS; preserve PDF-zero delta
behavior and distinct local override-map semantics. Move common escape
weighting to a layer shared by both map-selection paths, then audit every
recursive consumer carrying bsdfPdf and medium survival. Validate complete
SSS environment NEE plus continuation against an independently integrated
angular oracle; do not change Sw or eta convention to hide the extra term.

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
