# HyLIoS design refresh: evidence and open questions

**Audit date:** 2026-11-25. **Code baseline:** `be3e7ed74`.
Companion to [HYLIOS_WORKPLAN.md](HYLIOS_WORKPLAN.md). This is a design audit, not implementation validation or a claim that every underlying dataset has been obtained.

## 1. Source inventory and access status

| Source | Inspected material | Role / limitation |
|---|---|---|
| [Chen et al., 2015, Hyperspectral Modeling of Skin Appearance](https://www.npsg.uwaterloo.ca/resources/docs/a31-chen.pdf), DOI 10.1145/2701416 | Model description, equations, algorithms, experiment descriptions; visually checked pp. 31:6–31:7 | Primary definition of the reference model |
| [Online appendix](https://www.npsg.uwaterloo.ca/resources/docs/a31-chen-app.pdf) | Figure 26, Tables III–IV and spectral-IOR description | Presets, fixed parameters and original data references; plotted curves are not complete machine-readable tables |
| [Chen PhD thesis](https://www.npsg.uwaterloo.ca/resources/docs/theses/Chen_Tenn_PhD_Thesis.pdf) | Chapters 3–4, relevant Chapter 6 comparisons, Appendix A; visually checked printed p.95 (PDF page 126) | More derivation context, including dimensional ambiguities; tanning chapter is outside this project |
| [NPSG model/publication index](https://www.npsg.uwaterloo.ca/pubs/models.php) | HyLIoS and related skin publications | Discovery of primary literature, not a source-code repository |
| [Current NPSG model catalog](https://www.npsg.uwaterloo.ca/models.php) and [extended HyLIoS endpoint](https://www.npsg.uwaterloo.ca/models/hyliosEx.php) | Public catalog and login/access page | Execution is credential-gated; the page explicitly says code is not provided. No access request, email, or model job was submitted. |
| [OMLC water collection](https://omlc.org/spectra/water/abs/index.html) | Dataset index and measurement/compilation distinctions | Candidate water absorption datasets and separate refractive-index sources; selecting a splice is still work |
| [OMLC hemoglobin](https://omlc.org/spectra/hemoglobin/) and [numeric compilation](https://omlc.org/spectra/hemoglobin/summary.html) | Units, conversion convention and numeric availability | A useful reference, but not automatically identical to the model's chosen molar convention |
| [OMLC melanin](https://omlc.org/spectra/melanin/extcoeff.html) | Extinction basis and linked data | Reuse candidates; insufficient by themselves for 250–2500 nm |
| [OMLC lipid measurements](https://omlc.org/spectra/fat/) and [processed table](https://omlc.org/spectra/fat/fat.txt) | Provenance, processing note and numeric table | Mammalian fat data, not proof of full SWIR coverage or a skin-specific lipid mixture |
| [Gomes et al., Hyperspectral Imaging Database of Human Facial Skin](https://pmc.ncbi.nlm.nih.gov/articles/PMC11823275/) | Acquisition, range and participant summary | Newer external validation candidate: 29 faces, 400–720 nm at 10 nm steps. Visible only, with substantial group imbalance; not a full-domain or population-wide oracle. Raw dataset acquisition remains to be done. |
| [VIS–SWIR biological absorbers dataset record](https://data.library.wustl.edu/record/108249) | Search-index description; direct access returned 403 | Candidate contemporary cross-check only. No numerical values or license were verified; cannot yet be a dependency. |

The downloaded paper/appendix/thesis were read locally for equations that text extraction can distort. They are not vendored into RISE. SHA-256 of the inspected downloads:

```text
paper     9b72cc0887fc931ea64a554faabb2705acef58d59935423365d37cba6c3e8637
appendix  83ddcaf906c58280cfe585a57df235ff31b0fea530ea1e5174ed266f75b7ab38
thesis    a90c8212f861eb34e80ffbbf0ed75b88e4f1f7244fafc0312303f8d0ca9a35cb
```

No local HyLIoS source implementation or reference PDFs were found in the repository search; the existing workplan was the local design source. This review does not claim access to private author code, numeric online-model responses, paywalled underlying measurements, or the supplementary video. P0 must obtain or explicitly disposition the remaining inputs.

## 2. Source reconciliation ledger

The entries below distinguish observed publication statements from our engineering decisions. They are implementation blockers only for the affected feature; synthetic mechanics tests can proceed independently.

| ID | Evidence / issue | Required implementation disposition and test |
|---|---|---|
| H01 | Paper §4 models layer and particle events, not the old plan's generic epidermal Mie channel and BioSpec baseline | Exclude those additions from the reference definition. Make any later alternative a different model revision; use ablation tests rather than visual tuning. |
| H02 | Paper §4.3: bottom reflection includes diffuse perturbation | Implement an explicit diffuse reflecting closure. Unit reflectance alone is not a specular/TIR boundary. Angular moment test distinguishes them. |
| H03 | Paper §4.2.2 and thesis §4.2.2: fiber contribution once per papillary pass | Add pass eligibility state. Define what resets it at reflection/re-entry; this reset detail needs reconciliation, not an ordinary unlimited Poisson scatter loop. |
| H04 | Paper Algorithm 2 has `3*sqrt(6)/8`; old plan transcribed `sqrt(2)` | Correct the reference envelope and validate the normalized sampled law. A smaller constant may change rejection efficiency without changing shape, so test both distribution and envelope rather than declaring all old samples biased. |
| H05 | Paper Eq.7 and thesis Eq.4.7 print `(eta²-1)/(eta²+1)` | Preserve this in the reproduction candidate. Do not silently replace it by the familiar sphere-polarizability `+2` expression. Reconcile with the cited derivation; record the selected convention and quantitative sensitivity. |
| H06 | Paper §4.2.1 tapers the mean perturbation angle, not simply encounter probability; 1300/approximately 1400 nm wording leaves endpoint interpretation | Pin an explicit taper and zero-angle branch. Report sensitivity of competing interpretations; do not delete melanin absorption with the angular scattering. |
| H07 | Orientation and fold expressions need their sampling measures and normalization established | Derive support and Jacobians; compare quadrature and histograms. Do not treat a printed polar expression as a normalized solid-angle PDF by inspection. |
| H08 | Paper Table I and appendix distinguish complexes from isolated particles; dimensions vary with layer in the results setup | Presets must encode organization and layer-specific size changes. Resolve full axes versus semi-axes and the complex interaction-count rounding rule. Generic volume fraction alone is insufficient. |
| H09 | Internal melanosome traversal includes chord absorption, boundary Fresnel and repeated internal perturbations | A single Beer test plus a forward turn is incomplete. Track real chord segments, inward-normal conventions and repeat statistics; test overlap and layer-boundary rejection. |
| H10 | Appendix uses water-dependent spectral layer indices anchored at 1300 nm | Need water real-index data in addition to absorption. Derive the chosen mixture law and test the anchor; constant IOR is only an explicitly labeled approximation. |
| H11 | Thesis Appendix A printed p.95 says scale the 4 µm keratin measurement by `4*10^4` | Dimensional calculation gives `1/(4e-4 cm) = 2500 cm^-1`, a factor 16 difference. Do not copy the printed multiplier. Check original measurement normalization and record any deliberate legacy-reproduction variant. |
| H12 | Thesis Appendix A's DNA/UCA expressions do not fully state reference concentration normalization; UCA notation calls a mol/L quantity a molar weight | Reconstruct from the measured absorptance, reference path length and concentration. Old plan incorrectly assigned the keratin 4 µm path to DNA. Validate units before creating coefficients. |
| H13 | Thesis Appendix A gives Hb molar mass 66500 g/mol; OMLC's table conversion uses 64500 g/mol | This is approximately a 3.1% coefficient difference for the same concentration/table. Record a deliberate matched data/conversion pair. Never swap the constant while claiming identical reproduction. |
| H14 | Paper Eq.2 visually appears to reuse the sulfhemoglobin volume symbol beside carboxyhemoglobin | Resolve by constituent identity and the source derivation. Separate tests must show each chromophore concentration changes only its own contribution. Treat this as a notation ambiguity, not evidence of author-code behavior. |
| H15 | Neither source inspection nor a Fresnel implementation proves the complete forward model reciprocal | Derive adjoint/connection semantics before camera-path integration. Measure source/detector swaps, including surface and rejection rules. Report a discrepancy rather than silently symmetrizing it. |
| H16 | `BioSpecSkinSPF::ComputeExtinction` uses `ext * concentration`; thesis Appendix A.1 explicitly specifies `epsilon * ln(10) * c` for melanin | Use the thesis convention for the HyLIoS reference implementation after verifying the imported array basis. Add independent coefficient and Beer-transmission fixtures for colloidal and melanosomal melanin. Compare publication spectra as corroboration, not as a reason to fit away the conversion. BioSpec semantics are not changed by this design review. |
| H17 | Geometric encounter rate, finite particle chords and predecessor/boundary rejection together determine the realized point process | Define whether sampled distances are measured from an exit surface, entry point or another reference, whether interior chords advance bulk distance, and which competing bulk rates remain active. Measure occupied path fraction and chord/gap statistics. Do not apply `1/(1-f)` without deriving it for the chosen process; a Poisson encounter rate with excluded volume is not automatically a hard-particle medium. |
| H18 | Paper §4.3/thesis §4.3 discuss normal perturbation in the reflected-light context but do not specify all four entry/exit R/T sampling rules unambiguously | Resolve this in P0, not P4. Record a versioned publication interpretation separately from `rise_reciprocal_boundary_v1`. BioSpec's planar entry/exit and reflected-ray perturbation are predecessor evidence, not Chen implementation evidence. A named reciprocal boundary does not establish reciprocity of the interior process. |

Resolution records should contain the question, competing interpretations, dimensional/normalization analysis, selected rule, reference evidence, expected observable impact and a test. If author clarification becomes necessary, the outstanding question can be prepared without sending a message; external contact requires the user's explicit instruction.

A practical provisional IOR formulation to investigate is `n_layer(lambda) = n_layer(1300) + f_water * (n_water(lambda)-n_water(1300))`, assuming a nondispersive dry constituent and a compatible volume mixture. This is **our proposed interpretation**, not a verified equation supplied by the paper. Its applicability and constituent basis must be settled in P0.

For H11–H12, the general dimensionally consistent starting point is `mu = -ln(1-A_ref)/d_ref * c/c_ref` when a linear concentration law applies. Solid keratin's volume weighting is separate. This equation does not establish that every published plotted value used the same preparation or normalization.

## 3. Optical-data inventory

The existing `src/Library/Materials/BioSpecSkinData.h` wavelength arrays were inspected directly. Their domains alone disprove the old plan's assumption that the six inherited absorbers already cover the entire target range.

| Existing array family | Sample count | Stored wavelength domain (nm) | Reuse decision |
|---|---:|---:|---|
| Eumelanin | 31 | 209.99–820.26 | Source/conversion audit; requires justified long-wave continuation |
| Pheomelanin | 49 | 213.54–829.42 | Same; do not invent a flat SWIR tail |
| Oxy/deoxy Hb shared grid | 376 | 250–1000 | Verify molar convention; missing longer-wave policy |
| Bilirubin | 1842 | 239.75–700 | Verify extinction basis and tails |
| Beta-carotene | 1922 | 219.75–700 | Same |

Counts describe the wavelength arrays, not a validation of paired coefficient values. Existing tables are reuse candidates, not yet independently certified HyLIoS data.

Additional required inputs:

| Input | Acquisition route | Outstanding verification |
|---|---|---|
| Water absorption and real refractive index | Original Palmer–Williams/Pope–Fry references; OMLC collection for accessible numeric data | Separate `n` from `mu_a`, actual range, splice boundaries, temperature and publication fidelity |
| Lipids | Original measurements cited by the appendix plus OMLC data | SWIR extension, units, tissue-mixture basis and overlap agreement |
| DNA, keratin, urocanic acid | Original experiments; thesis/appendix plots if numeric data cannot be obtained | Path length, concentration basis, absorptance percentage, digitization uncertainty, H11/H12 |
| Carboxy-, met-, sulfhemoglobin | Original references named in appendix | Species definition, mass/molar/heme-equivalent basis, spectral range and tail policy |
| Reference model spectra | Author-provided numeric output if accessible; otherwise controlled digitization of published curves | Model revision, exact experiment and uncertainty; no online execution verified |
| Independent skin measurements | Published NCSU/NIST and other experiments used in the original evaluation; newer visible database as held-out check | Raw availability, acquisition geometry, units, license and specimen metadata |

No data import may infer Napierian versus decadic semantics from the word “extinction” alone. A generic `cm^-1` label on a graph does not resolve all normalization questions. No missing curve may be filled with a constant merely to make the build succeed.

The modern datasets are useful checks against overfitting to old illustrations. They are not silent replacements for the reference model's inputs, and a visible dataset cannot validate UV/SWIR behavior.

## 4. Current RISE integration evidence

These are source locations inspected in the audit. Prefer symbols over old line-number instructions because the branch evolves.

| Source | Observed behavior | Design implication |
|---|---|---|
| `Materials/BioSpecSkinMaterial.h` | SPF-only aggregate, scalar painter inputs, path-dependence forwarding | No `BioSpecSkinRWMaterial` template to copy |
| `Interfaces/IMaterial.h` | Homogeneous RW parameter struct; NM query receives wavelength but no hit; leaf capability predicates exist | Cannot encode layered spatial fields with this struct or discover a new spectral-only transport through old default leaf predicates |
| `Utilities/RandomWalkSSS.{h,cpp}` | Homogeneous coefficients/HG walk and existing SSS exit contract | Reuse lessons/helpers selectively, not the model representation |
| `Utilities/BSSRDFSampling.h` | Separable diffusion weights, cosine exit density, surface-area density, endpoint metadata | Metadata needs carrying forward; estimator-specific weights cannot simply be reused |
| `Shaders/PathTracingIntegrator.cpp` | Shared Pel/NM implementation; separate HWSS per-lane fallbacks for SPF-only and SSS at first/mid-path hits | Plan all three variants and continuation state, not only `ScatterNM` |
| `Interfaces/ISPF.h` | `kray` excludes external eta²; `pdf` is directional density and `isDelta` is semantic | Unknown density must not be disguised as a delta; internal absorption/Fresnel and external eta factors need clear ownership |
| `Rendering/PathTracingSpectralRasterizer.cpp` | NM/HWSS results converted with `XYZFromNM`, then visible normalization | Range selection is not raw spectral export |
| `Utilities/Color/SampledWavelengths.h` | Arbitrary interval carrier, discrete-grid snapping, active-lane averaging | Useful sampling carrier; not a calibrated sensor or proof of out-of-visible scene support |
| `Utilities/PiecewiseLinearFunction.h` | Returns endpoint values outside tabulated range | Wrap with checked coverage for HyLIoS; do not globally change historical interpolation |
| `Interfaces/IScalarPainter.h` | Physical scalars separated from color uplifting; spatial/spectral variation supported | Use the right type, but still restrict biological fields to appropriate dependencies |
| `Parsers/ChunkParserRegistry.cpp`, `Cst/Cst.cpp` | Descriptor/finalize and CST derive architecture | The old streaming-parser instructions are obsolete |
| `Job.cpp` | Scalar reference resolution, validation and construction; zero-volume RW validation | Add new capability-aware validation instead of piggybacking blindly on RW detection |
| `SceneEditor/MaterialIntrospection.cpp`, `MaterialPreview.cpp` | Explicit material recognition and SSS preview behavior | Add deliberate preview and editing support |
| `Rendering/AutoRasterizer.cpp` | Integrator selection and spectral factories | New unsupported-transport capability must affect routing and explicit-mode diagnostics |

Related design context read or cross-checked: repository README/map, materials and legacy-surface policy, integrator refactor status, parser architecture, and the SSS regression/debt-document inventory. The obsolete plan's claim that all this requires “no changes” is not supported by these source contracts.

## 5. Design alternatives considered

| Alternative | Decision |
|---|---|
| Copy BioSpec SPF and add more curves | Insufficient: risks inheriting different scattering and boundary assumptions. Reuse verified numerical primitives only. |
| Homogenize all layers into current RW SSS | Rejected as the reference model. Could be a separately named future approximation only after error measurement. |
| Put the whole exact walk inside `ScatterNM` and declare completion | Useful restricted prototype; loses a clear nonlocal/connection contract and does not solve delta-light evaluation. |
| Direct joint transport capability | Preferred spatial architecture; requires adjoint and connection proof before general production support. |
| Fit a diffusion profile | Optional approximation, not full joint angular/spatial HyLIoS. Requires validation of separability and measured residuals. |
| Precomputed spectral response | Preferred first general local-lighting adapter after oracle validation. Explicit approximation, immutable versioned data, matched sample/eval/PDF and held-out error tests. |
| Build six offset shells automatically for any mesh | Rejected as the default character workflow. Analytic controls precede a validated intrinsic layer-coordinate body; explicit nested meshes remain certified fixtures. |
| Learn a neural surrogate immediately | Deferred: model uncertainty and training-domain validation would obscure reference errors. |
| Upgrade existing XYZ output by changing its wavelength range | Rejected: raw radiometry and sensor integration are distinct from visible color conversion. |

The major remaining uncertainty is not class naming or parser wiring. It is whether the chosen interpretation of the forward stochastic model admits the desired camera-path/connection estimator, and whether the optical data can support the advertised wavelength range. The phase gates make both questions visible before production code depends on their answers.

## 6. External review disposition (2026-11-25)

Reviewed all 13 supplied findings against the same code baseline and retained paper/thesis extracts. The review identifies necessary design work, but several severity labels and quantitative claims exceed the evidence. These dispositions concern the **plan**; no renderer defect was fixed or experimentally reproduced in this pass. Source symbols below were inspected; benchmark, estimator and geometry claims still require the planned tests.

| Review ID | Disposition | Evidence, correction and plan change |
|---|---|---|
| P0-1: nonlocal NEE | **Accept the missing estimator contract; qualify exclusivity.** | DL-381 documents an existing cosine-exit model limitation, not a proof that only one algorithm exists. §6.2 now chooses a conditional boundary-event NEE feasibility experiment with its contribution and conditional MIS proposals. No marginal over all internal histories is required for that extended-state strategy. Deriving the actual fold transmission kernel remains a P4 gate. Smooth Snell/delta-light support is explicitly excluded pending a separate connection method. A 6D table is not promised for arbitrary meshes. |
| P0-2: spectral sources | **Accept.** | `LightSampler::Prepare` admits lights by RGB-derived exitance; `EnvironmentSampler::Build` uses `GetRadianceColor`; point/spot/directional lights use visible-only `RGBIlluminantSpectrum`. P6a now supplies physical SPDs and support-preserving band-aware proposals, their MIS PDFs, cache keys and roulette policy. A small positive probability is not automatically underflow, and losing NEE does not necessarily zero all area/environment continuation. Both support loss and excessive variance need tests. |
| P0-3: raw film | **Accept.** | `ProgressiveFilm`/`FilteredFilm` store XYZ sums, spectral adaptive sampling reads XYZ Y, and FrameStore has closed-map schema checks. P6b now uses a dedicated scalar/band rasterizer, film, variance, raw writer/schema and separate display preview. It does not insert raw bands into an XYZ buffer. |
| P0-4: melanin convention | **Accept explicit reconciliation/test; intended formula already documented.** | `BioSpecSkinSPF.h` omits the factor through `ComputeExtinction`, while retained thesis p.93 Eq.A.1 includes it. H16 records this distinction. Verify raw versus transformed table units, then apply the thesis formula once. This is not evidence that Chen's unavailable implementation inherited BioSpec's convention, nor a reason to change BioSpec in this task. |
| P1-1: surface partition | **Accept for the new design.** | Existing SSS branches launch `PTCastRay<Tag>` and later process surface behavior; they are not a safe template for HyLIoS. §6.4 now selects one continuation using the same fold/IOR law for both branches, or the complete baked mixture. No additional generic GGX reflection or Fresnel coat. NEE remains an additive estimator. This review does not reclassify or rewrite all existing SSS transport. |
| P1-2: response-table domain | **Accept.** | A bake is conditional on specimen and exterior medium. §6.2 restricts first production tables to explicit assets keyed by complete parameters and spectral ambient IOR; overrides require rebaking, maps/unsupported exteriors fail validation. Construction-time or per-hit implicit baking is removed from the design. No unmeasured “minutes” bake-time claim is adopted. |
| P1-3: early weighted transport | **Accept, with termination qualification.** | P2/P3 now require a weighted estimator and compensated roulette checked against analog controls. Per-lane resumable work quanta bound latency, not physical history length. A hard cutoff cannot be made unbiased simply by calling it roulette. Long-tail variance and completion accounting remain gates; optional P9 acceleration is separate. Full-range HWSS bundles are not the mandatory measurement workflow. |
| P1-4: nested mesh geometry | **Accept the risk and change the primary target; reject inevitability claim.** | A fixed `BSSRDF_RAY_EPSILON = 1e-6` is unsuitable for microscopic internal traversal. Curvature/chord sag estimates do not prove that all nested triangulations intersect, and a model-owned geometry policy need not add hidden TLAS objects. §7 now targets a certified intrinsic coordinate volume, retaining analytic/nested fixtures; local precision, chart injectivity and minimum layer separation are explicit gates. Actual vertex/transform precision must be audited instead of assuming every relevant representation is FP32. |
| P1-5: engine gateways | **Accept.** | Confirmed `AuditedMaterialGraph`, `HasSubsurfaceLeaf`/`HasRandomWalkSSSLeaf`, Job DL-409, Mix varying-SSS checks, AgentSession material-role rules, descriptor pipes and scheduling/keyword counts. §11 now names each gateway and distinguishes nonlocal geometry requirements from the homogeneous-RW capability. Wrapper/CSG census feeds integrator refusals without abusing path dependence. |
| P2-1: guide projection | **Accept.** | `EvaluateMaterialAOVGuides` returns white for SPF-only materials. §11 now defines a shared versioned D65/incident-angle reflectance guide for preview/AOV/Mix consumers, keyed to the bake. It is a display/denoiser approximation; neither it nor a specific working-RGB label is an independent transport model. |
| P2-2: excluded-volume process | **Accept investigation; correct preset and avoid automatic rescaling.** | H17 adds gap/chord/packing semantics and occupancy tests. Published appendix Table III S4 has **10%** melanosome content in each epidermal layer, not the review's 30–43% basal value. For the illustrative alternating gap/chord process, occupancy is `mu*mean_chord/(1+mu*mean_chord)`; that alone does not establish the paper's distance convention or justify a universal `1/(1-f)` correction. Higher fractions are separate stress tests when physically admitted. |
| P2-3: recursion and clamps | **Accept for new transport.** | Existing SSS uses recursive `PTCastRay<Tag>` handoffs. §6.4 requires iterative or explicit-stack nonlocal continuation and preparation-time rejection of nonzero clamps/unsupported bias controls for scientific runs. No global refactor of unrelated SSS is included. |
| P2-4: interpolation/splices | **Accept early comparison; reject automatic log-linear superiority.** | P1 now measures coefficient and observable error for linear/log-linear alternatives and dataset splices. Convex-curve secant bias is real when the underlying shape is known; sparse measured samples alone do not prove it, and log interpolation does not repair missing peaks, noisy/zero samples or incompatible datasets. |

Additional checks added to the plan: baked-key mismatch, pure out-of-visible and mixed emitters, missed narrow-band proposal support, raw roundtrip/resume, conditional NEE/MIS agreement, fold-free/delta-light refusal, long-tail lane accounting, microscopic coordinate precision and mechanically enforced scientific settings.

The conditional-estimator and roulette discussion is an engineering design based on explicit path histories and probability measures, not a reproduced HyLIoS result. [PBRT's path-tracing derivation](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/Path_Tracing) provides the general path-sampling/roulette background and explains why generic continuous sampling misses delta distributions. The source-specific boundary and adjoint derivations still have to be supplied and tested in P4.

**Review outcome:** all 13 findings have a disposition and corresponding design action or qualification. No claim of zero implementation bugs, full hyperspectral support, or solved boundary physics follows from this document update.

## 7. Round 2 review disposition (2026-11-25)

All six follow-up findings merit design changes. This section supplements the round-1 record; the workplan is the current contract where its new boundary-variant policy supersedes the earlier mandatory-publication-BTDF gate. No implementation or numeric validation is claimed here.

| Review ID | Disposition | Verified evidence and change |
|---|---|---|
| R2 P0-1: initial reflection NEE | **Accepted.** | The earlier conditional estimator covered internal-to-external transmission but left initial rough reflection unspecified. §6.4 now evaluates marginalized `K_r` before a facet is sampled, with branch-inclusive reflection PDF and light MIS, discrete-light handling, and isolated surface-only tests. A reflection half-vector Jacobian is required for the microfacet mapping but is not itself a BSDF; another procedural law needs its own mapping. |
| R2 P0-2: boundary-law deadlock | **Accepted with evidence limits.** | Re-read retained paper §4.3 and thesis §4.3. Both describe perturbed normals in the reflected-light discussion; neither inspected passage settles all transmission rules. Current BioSpec uses planar entry/exit refraction (`BioSpecSkinSPF.h` boundary helpers), perturbs its reflected ray and separately scatters within SC (`ProcessSCInteraction`). This does not prove Chen's implementation does the same. H18 moves reconciliation to P0. The plan permits a separately named/versioned reciprocal renderer boundary, while preserving publication interpretations and requiring a full-model adjoint gate. |
| R2 P1-1: pause/resume boundary contract | **Accepted; qualify TIR optimization.** | §4 specifies `Advance`/`ResumeBoundary`, owned state, event tokens, terminal/domain outcomes and work quanta. The driver handles scene queries and NEE; shared pure boundary code handles sampling. Multiple boundary returns cannot duplicate NEE or RNG draws. Skip NEE only for certified-zero transmission of the entire active kernel: TIR against the macro normal or one sampled facet does not prove a rough boundary has no transmitting orientations. |
| R2 P1-2: emitter packets and freeze | **Accepted.** | `LambertianEmitter`, `PhongEmitter` and `CompositeEmitter::averageRadiantExitanceNM` read `averageSpectrum`; its `VisibleSpectralPacket` type is fixed to 380–780 nm with 40 samples. Pointwise `radiantExitanceAtNM` provides the appropriate path to audit for arbitrary bands. P6a now explicitly bypasses the visible average, uses object-aware surface records, and owns immutable proposal tables per render context rather than mutating shared scene emitters/maps. |
| R2 P2-1: baked assets | **Accepted.** | `ChunkDescriptor.h` has a `Filename` parameter kind documented for `RISE_MEDIA_PATH` resolution. §11 adds `response_asset`, CST/dependency behavior, hashes/provenance and validation. It supplies calculated table sizing, explicit configurable disk/resident budgets, load-time sampling-distribution construction and an offline tiny-fixture versus generated-production-asset Git policy. The review's 50–250 MB estimate is workload-dependent, not an established measurement. |
| R2 P2-2: finite lateral domain | **Accepted.** | §7 now defines an extruded polygon footprint with absorbing sides for the initial finite-host PT mode. Side crossings participate in the event minimum and produce a separate domain-loss tally; a top exit requires a backed host intersection. No projection, resampling or continuation outside the footprint is allowed. Infinite-slab measurement remains separate; footprint-convergence tests quantify the finite-domain effect. |

The first-surface microfacet density mapping and the distinction between reflection/transmission evaluation are supported by [PBRT's rough dielectric derivation](https://pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF). Applying a particular microfacet model to RISE is an explicit renderer extension, not a derivation of Chen's Eq.8. Its roughness mapping, masking/energy treatment and full interior adjoint behavior remain implementation gates.

New required tests: pure reflection with absorbing interior and delta lights; repeated outer-boundary R,R,T yields; deterministic pause/resume equivalence; rough-interface transmission support despite macro-normal TIR; visible-packet-zero versus pointwise-SWIR-positive emission; isolated two-band render contexts; missing/stale/oversized assets; finite-footprint side loss and valid top-exit mapping.

## 8. Final P2: visible guides without baked assets

**Accepted, non-blocking for starting implementation.** The prior §11 wording tied every guide cache to a baked specimen despite forbidding `response_asset` in non-baked modes. The workplan now gives nonlocal/diagnostic modes a fixed-seed, visible-wavelength slab estimate at preparation, sharing the same projection and immutable guide representation as baked-local mode. Cache identity includes the specimen, exterior, boundary/data revisions and guide-estimator settings. The guide is a preview/AOV approximation, not a replacement for transport; its preparation cost must be measured rather than assumed to be milliseconds. P5/P7 tests cover asset-free preparation, determinism, invalidation and cross-source statistical agreement.

## 9. Implementation acquisition update (2026-11-25)

The original inventory above was incomplete: the [NPSG human-skin data index](https://www.npsg.uwaterloo.ca/data/skin.php) publishes downloadable MATLAB scripts containing numeric tables for the absorbers. Their availability is separate from the credential-gated online model. Scripts were downloaded and inspected as text, **not executed**. The [water index](https://www.npsg.uwaterloo.ca/data/water.php) also supplies the Palmer–Williams real refractive index and alternative UV sources. URLs, hashes, covered ranges and interpretation notes are recorded in `data/hylios/source_manifest.json`; the source downloads are not vendored. No author implementation was accessed.

This closes the *location* gap for many inputs; it does not certify their interpolation error, tails, redistribution terms, or publication reproduction. The author scripts' resampled output can lose detail present in the original irregular grids. Import should preserve those grids and explicitly implement the documented transforms.

Three particularly important findings from the downloaded files:

- `keratin_abs.m`, `dna_abs.m` and `uro_acid_abs.m` describe absorptance in their comments, but their `fprintf` writes the interpolated **decadic absorbance**. The plot alone transforms it with `(1-10.^-data)*100`. Treating the text values as fractional absorptance would apply the wrong conversion.
- `dna_abs.m` normalizes by its twelfth input value, at 257.9556 nm; the comment describes this as A260 = 1. That comment must not be treated as a guarantee that the interpolated 260 nm sample is exactly one. The author index explicitly states a 50 µg/mL, 1 cm reference sample. UCA uses 15 µmol/L over 1 cm. These are evidence for dimensional concentration-ratio conversions, not confirmation that the thesis's abbreviated A.4/A.5 were implemented that way.
- The keratin file confirms the 4 µm path, but does not resolve why the thesis prescribes a multiplier sixteen times its inverse in centimetres. Keep a corrected interpretation distinct from literal-thesis reproduction. None of the new downloads justifies silently extending every absorber over 250–2500 nm.

The user directed implementation to proceed using public data and audited reusable BioSpec data, with a clean-room interpretation where necessary. Author clarification is a last resort for a specific unresolved input. `presets_source.json` transcribes Tables III and IV in original units; its explicit non-executable status remains until optical data and model decisions are resolved. The initial C++ optical-data module supplies validation and conversion mechanisms only, not a skin material or transport estimator.
