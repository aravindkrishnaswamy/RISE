# HyLIoS in RISE: refreshed design and implementation plan

**Status:** implementation started with checked optical-data primitives, source-evidence tooling, and a six-layer absorption/dielectric transport control. The full particle/fiber skin transport and renderer integration are not yet implemented. Phase exit gates below remain requirements, not completed milestones.
**Refreshed:** 2026-11-25, against branch `hyperspectral-skin`, HEAD `be3e7ed74`.
**Supersedes:** the original plan introduced by `3e40fe1d1` (retained in Git).
**External review dispositions:** [round 1](HYLIOS_RESEARCH_NOTES.md#6-external-review-disposition-2026-11-25) and [round 2](HYLIOS_RESEARCH_NOTES.md#7-round-2-review-disposition-2026-11-25).
**Evidence and unresolved source questions:** [HYLIOS_RESEARCH_NOTES.md](HYLIOS_RESEARCH_NOTES.md).

The initial implementation lives in `src/Library/Materials/HyLIoS/HyLIoSOpticalData.{h,cpp}`. It provides checked, non-extrapolating immutable curves and explicit unit conversions, tested with synthetic analytic controls. `data/hylios/` records published parameter values and source identities; these records are not executable physiological presets. Run `python3 tools/check_hylios_sources.py --check` to check the transcription and `python3 -m unittest discover -s tools -p 'test_hylios_sources.py'` for its mutation/units tests. See the implementation acquisition update in the research notes for newly located author-published numeric tables.

`HyLIoSOptics.{h,cpp}` now supplies isolated prolate surface/volume, orientation, exponential free-flight/forward-scatter and Rayleigh primitives, with bounded one-proposal sampling calls. These are not yet a complete walk. The [equation worksheet](HYLIOS_EQUATIONS.md) separates candidate interpretations from source statements. [Source-data instructions](../data/hylios/README.md) cover explicit checksum-verified fetching and conversion of the 14 absorber tables plus water real index. Imports preserve source grids and reference-sample units; they do not supply missing tails or resolve a complete physiological specimen.

The recommendation is to implement a wavelength-resolved, layered transport kernel first, validate it independently of image rendering, and then integrate it into RISE through an explicit transport contract. Do **not** implement the old plan's thickness-averaged homogeneous random walk as HyLIoS. It removes the layer/particle structure that motivates this work and can add an unrelated epidermal attenuation factor on top of absorption already counted in the walk.

`HyLIoSSlabTransport.{h,cpp}` implements the explicitly named `slab-control-v1`: six constant-coefficient layers, smooth dielectric interfaces, a diffuse reflecting bottom, analog/weighted absorption, optional compensated roulette, and resumable outer-boundary events. It preserves lateral displacement and per-layer absorption/path tallies. It has no particles, fibers, folds, or finite footprint; it is an analytic control for the forthcoming model, not an executable S1–S4 specimen. Its tests compare absorption against an independent slab integral and compare estimators statistically.

The separately labelled `hylios_cleanroom_approximation_v1` data profile now supplies explicit spectral tails and water substitutions; reference curves remain non-extrapolating. Its bundle and queries carry the profile identity, source hashes, content fingerprint and per-curve rules. These are unvalidated approximations, not publication reproduction. See the [source-data instructions](../data/hylios/README.md). This changes data availability for exploratory rendering, not the transport or renderer acceptance gates.

`HyLIoSParticleGeometry.{h,cpp}` supplies projected-area hit sampling, spheroid chords and outward normals. It does not yet implement stochastic particle placement, predecessor exclusion, internal scattering, or complexes. `tools/hylios_compose_specimen.py` resolves the six-layer coefficients and organelle parameters from raw source quantities using the named [composition contract](HYLIOS_COMPOSITION.md); strict evaluation rejects missing active coverage, while approximate evaluation requires an explicit bundle. The resolved record retains the selected assumptions, raw query traces and content identities. A resolved specimen is input preparation, not evidence that the full walk exists.

`HyLIoSParticleOverlap.{h,cpp}` adds the separate [predecessor overlap predicate](HYLIOS_OVERLAP.md). It uses bounded outward-interval contact tests, exact sphere/aligned controls and explicit uncertainty/range outcomes. The predicate alone does not define the stochastic placement law. Numerical uncertainty must propagate as a diagnostic rather than being interpreted as physical overlap or silently retried.

The experimental [individual-spheroid forward kernel](HYLIOS_FORWARD_TRANSPORT.md), `HyLIoSForwardTransport`, now connects these primitives with host/particle absorption, internal dielectric events and papillary fiber-pass state. It requires an explicit placement policy: fixed-event diagnostic or the separately named null-collision interpretation. Complexes and surface folds remain unsupported. Post-exit perturbations that turn inward are reported with live unresolved weight, not silently discarded or counted as absorption. This is not a completed visible skin model or P2/P3 exit gate.

This is a research implementation with a production integration path. A plausible portrait is a demonstration, not its acceptance criterion. The scientific target is the 2015 model, with individually documented interpretations where the publications are insufficient. Newer data, curved geometry, and faster approximations must remain identifiable extensions.

## 1. Intended deliverables and scope

Deliver three separately testable capabilities:

1. **Reference model:** reproducible monochromatic simulation over the supported portion of 250–2500 nm, reporting directional-hemispherical reflectance, angular distributions, spatial exit distributions, and absorption by layer. Full-range status requires complete, justified optical data coverage.
2. **RISE rendering:** visible spectral images and quantitative UV/NIR/SWIR band images with correct transport weights, light support, wavelength normalization, and explicitly declared integrator compatibility.
3. **Spatial skin transport:** preserve the joint exit position/direction distribution on planar layers, followed by a geometry-aware layered extension for curved bodies. A local material that discards exit displacement is a useful intermediate, not completion of the spatial model.

Keep BioSpec and Donner–Jensen unchanged. Do not replace their parameters, data, or historical semantics. Exclude tanning dynamics, fluorescence, polarization, thermal emission, inverse fitting, melanoma simulation, and clinical interpretation from this implementation. Reflected radiation up to 2500 nm is not a thermal-camera model.

Boundary variants are explicit from P0:

- `hylios_2015`: publication-reproduction **interpretation**, with an immutable boundary-definition revision. The inspected paper §4.3/thesis §4.3 describe perturbed surface normals in the discussion of reflected light; they do not unambiguously specify entry and exit transmission sampling. P0 must separately record external reflection, external transmission, internal reflection and internal transmission, including when Fresnel is evaluated. A provisional interpretation may run as such, but cannot be labeled identical to unavailable author code.
- `rise_reciprocal_boundary_v1`: an explicitly permitted renderer extension using a two-sided reciprocal dielectric microfacet boundary, consistent reflection/transmission evaluation and sampling, and a documented roughness definition. Use a validated existing RISE boundary where suitable; audit spectral/exterior-IOR behavior and energy treatment before reuse. Do not identify its roughness with the paper's fold aspect ratio without a measured mapping. Masking/multiple-scattering treatment, residual energy loss and eta conventions are part of its versioned specification.

If P0 finds a planar/delta or non-reciprocal reference exit rule, preserve it in measurement mode and use the named extension for supported camera rendering. This avoids making P7 depend on a nonexistent reference rough BTDF. A reciprocal boundary does **not** prove the particle/interior model reciprocal: P4 still needs a valid adjoint for the complete renderer model. Both variants carry their names in bakes, scenes, output and comparison reports; quantify their differences rather than presenting the extension as reproduction.

Use named, equation-linked functions and readable state transitions. Model capability improvements should make the derivation, tests, and review more rigorous; they do not remove missing-data or estimator-design work.

## 2. What changed since the original design

| Old assumption | Current evidence / revised decision |
|---|---|
| Copy `BioSpecSkinRWMaterial` | Neither its header nor implementation exists in this checkout. `BioSpecSkinMaterial.h` supplies a legacy SPF and no homogeneous RW override. Use it for historical comparison, not as an integration template. |
| Add parsing in `AsciiSceneParser.cpp` | Native CST v7 scenes derive through `Cst.cpp`; accepted parameters belong in `ChunkParserRegistry.cpp` descriptors and `Finalize`. |
| Physical fields are `IPainter` colors | Use `IScalarPainter` for applicable physical scalar inputs; avoid RGB uplift for concentrations, fractions, dimensions, and IOR. |
| Implement PT in the header | Current Pel/NM PT is shared through tag-templated code in `PathTracingIntegrator.cpp`; HWSS has distinct fallback logic. |
| `SampledWavelengths` accepting a range means hyperspectral output works | It can carry the range, but `PathTracingSpectralRasterizer::IntegratePixelSpectral` converts samples through CIE XYZ and visible normalization. A new radiometric output path is required. |
| Reuse BioSpec curves across the entire range | Audit their sources, units, coverage and endpoint behavior first. Existing piecewise-linear evaluation holds endpoints outside the table. That is not a defensible default for tissue spectra. |
| Add Mie scattering and a BioSpec baseline | These are not established HyLIoS processes merely because another skin model uses them. Exclude from the reference kernel unless source reconciliation establishes them. |
| Mean coefficients plus `exp(-4 sum(mu_a d))` reproduce spatial HyLIoS | They do not preserve layered trajectories or melanosome interactions. No such adapter in the reference material. |
| Current SSS result type can be reused unchanged | `BSSRDFSampling::SampleResult` encodes a separable profile, cosine exit proposal and `Sw` weighting. HyLIoS needs a joint position/direction contract. |
| Update only the make file | Library additions require all five build-project files, plus tests, CST, editor, preview, snapshot and API surfaces. |

Some current prose in `docs/MATERIALS.md` still describes BioSpec RW overrides absent from the source. This plan treats inspected code as authoritative. Correct those paragraphs when landing the integration documentation; do not infer a capability from that prose.

## 3. Architecture and ownership

Proposed new components (names are design choices, not existing APIs):

| Component | Responsibility | Must not do |
|---|---|---|
| `HyLIoSOpticalData` | Immutable spectra, provenance IDs, coverage policies, unit conversion | Guess tails, silently substitute datasets, create duplicate LUTs per SPF/material |
| `HyLIoSParameters` | Validated specimen description, per-layer structure, resolved defaults | Store mutable per-ray state or mix percent/fraction units |
| `HyLIoSOptics` | Absorption/IOR evaluation and particle/fiber sampling primitives | Resolve scene names, read UI state, touch film |
| `HyLIoSTransport` | Monochromatic walk and explicit outcome/diagnostic records | Know camera color spaces, manufacture an exit PDF |
| `HyLIoSMeasurement` | Forward experiments, independent reductions and reference comparisons | Render a photograph as the sole correctness test |
| `HyLIoSSkinMaterial` and adapter | Bind scalar fields, expose capabilities, connect validated transport to PT | Pretend to be homogeneous `RandomWalkSSSParams` |
| Geometry policy | Planar interfaces first; later explicit curved layer boundaries | Teleport an exit onto a convenient nearby triangle |
| Spectral measurement output | Raw monochromatic/band radiance, sensor integration, provenance | Route UV/IR through CIE XYZ, AgX or OIDN |

Place model-specific code under `src/Library/Materials/HyLIoS/`; place the measurement executable under `tools/`. Keep geometry-independent math callable by standalone tests. Only extract shared utilities when a second real consumer exists.

Use immutable shared data and construction-time precomputation. Walk state, wavelength coefficients, event flags and particle state are local values. Use `ISampler`; never introduce hidden RNGs or new TLS caches. Preserve reference-count ownership across manager insertion, failure, cloning and destruction.

### 3.1 Parameter and units contract

Canonical kernel units: metres, inverse metres, wavelength in nm, angles in radians, fractions in [0,1]. Explicit conversions at import boundaries: cm to m is ×0.01, inverse cm to inverse m is ×100, µm to m is ×1e-6. Concentration conversions retain their original mass/molar basis; mg/mL equals g/L numerically.

Represent six layers as an ordered array, with separate absorption mixtures, spectral IORs and applicable attenuation processes. Represent melanosome dimensions as **semi-axes** internally. Resolve the source full-axis versus semi-axis convention explicitly before conversion; any selected interpretation is recorded with provenance and applied once. Particle organization is an authored enum (individual / loose complex / compact complex), not inferred from a person's skin-tone label.

Start with constant physical specimens, including named S1–S4 presets. Presets expand into fully resolved values; record model and data revisions. Default S3 is a reproducibility convenience, not a universal skin default. Store both source percent values and converted fractions in the preset-generation evidence. Do not force all composition values to sum to one: some source quantities describe different or overlapping bases. Enforce sum constraints only for actual mutually exclusive constituents.

For artist-facing spatial controls, support single-valued `IScalarPainter` slots for pigment fractions, blood content/oxygenation and surface folds only after their sampling location is defined. A biological concentration must not depend on sampled wavelength just because scalar painters can. Restrict such inputs to wavelength-independent fields or explicitly define and reject unsupported expressions. Spectral IOR and absorption curves are separate optical data.

A diagnostic local walk may evaluate a homogeneous specimen at the shading hit and label this approximation. The production baked-response mode initially accepts only constant specimens with matching baked assets (§6.2); it does not support arbitrary per-hit parameter maps. Nonlocal transport must **not** freeze a texture at whichever endpoint the camera happened to hit: that changes the physical medium when the path is reversed. Its field must have a direction-independent interior/surface-coordinate definition, with transport sampling that handles spatially varying rates. Defer spatial maps in the reference release until this exists. Forward all child `GetPathDependence()` flags; use existing containment rules for path-dependent shading and reject it in scientific measurement mode.

### 3.2 Data contract

Every curve must carry source URL/DOI, source quantity and units, concentration/path length, measured range, temperature where relevant, interpolation/extrapolation rule, transformation script/version, checksum, uncertainty and redistribution status. Keep original permitted inputs and deterministic conversion scripts separate from generated C++ arrays.

Availability states are `verified`, `digitized`, `substituted`, and `missing`. A missing active constituent or uncovered wavelength makes a scientific run fail with a precise diagnostic. A physically justified zero tail is data policy, not the default. An inactive constituent need not require its table. Digitized curves carry resolution/error estimates and are never labeled original numeric data.

Do not change the global piecewise-function extrapolation policy. Add a checked model-specific spectral view. P1 must compare linear and log-linear interpolation against independent dense measurements or justified analytic controls before locking each curve. Positive steep spectra are candidates for log-linear interpolation; zeros and signed/complex optical quantities need separate rules. Record error in both coefficients and resulting optical depth/reflectance, and inspect value and slope behavior at water-data splices. A smooth-looking splice is not evidence of physical continuity. Use linear interpolation only where its error budget passes; neither interpolation can recover unknown structure between sparse measurements. No narrow absorption peak may disappear through a convenient coarse uniform grid.

Conversion primitives must be separately tested:

- Decadic extinction: `mu_a = ln(10) * epsilon * concentration`, with molar conversion when required.
- Measured absorptance: `mu_a = -log1p(-A) / path_length`, followed by the applicable concentration ratio. Percent is converted before the logarithm.
- Already Napierian absorption coefficients receive neither another `ln(10)` nor a second concentration conversion.
- An absorptance sample rounded to 100% is censored data, not permission to clamp to an arbitrary finite coefficient.

The source ambiguities in the research notes are a P0 work item, not optional cleanup after rendering. In particular, thesis Appendix A.1 explicitly includes `ln(10)` for melanin, whereas current BioSpec helpers multiply extinction by concentration without it. HyLIoS uses the documented decadic conversion once; verify whether imported arrays are raw or preconverted and test both melanosomal and colloidal contributions. Do not copy BioSpec helper semantics or tune this factor to match a portrait (H16).

## 4. Transport kernel design

The state is `(position, direction, wavelength, layer, particle state, fiber-pass state, weight, counters)`. Planar layers constrain boundary depth but do not reduce position to one dimension: retain both lateral coordinates and the actual exit direction.

For the first oracle use analog absorption and stochastic Fresnel decisions. A surviving unit-power history is already weighted by its probability of survival; do not multiply Beer attenuation or Fresnel a second time. Implement a separate weighted estimator with explicit target/proposal ratios and compensated roulette during P2, with analog-versus-weighted mean-agreement tests in P3. Keep the simple analog oracle for tractable controls. Basic survival weighting and long-walk management are prerequisites, not P9 acceleration.

At each segment, compare the nearest layer boundary with sampled eligible attenuation events. For a homogeneous eligible channel of rate `mu`, use `-log1p(-u)/mu`; handle exactly zero rate as no event without dividing. At an interface carry explicit side/layer ownership; do not use repeated floating-point nudges to determine which layer owns the ray. Define tie handling, tangent rays, matched indices and critical-angle behavior. Reset memoryless proposals only where valid; fiber eligibility and particle exclusions are additional state.

From P2, implement the kernel as an explicit resumable state machine, not an opaque run-to-completion call or a C++ coroutine requirement. Proposed contract:

```text
Advance(state, sampler, workQuantum)
  -> OuterBoundaryEncounter | DomainBoundaryEncounter | YieldBudget | Terminal
ResumeBoundary(state, encounterToken, decision)
  -> updated state
```

`OuterBoundaryEncounter` exposes physical location, geometric frame, interior direction, wavelength, accumulated weight, boundary-law identity, incident/target IOR information and conditional transport state. It pauses **before** the boundary random choice. A monotonically identified token prevents duplicate NEE or resuming the same event twice. The renderer adapter resolves scene/exterior context, evaluates NEE with scene visibility, calls the shared pure boundary-law sampler, then resumes reflection or launches transmission. The standalone measurement driver resolves its configured exterior and calls the same pure law without NEE. The kernel owns no scene, caster, light sampler or live `IORStack` pointer.

Persist layer/particle/pass state, sample-stream position and pending event across work-quantum yields. Quanta do not restart sampling, discard weight or duplicate a boundary encounter. Explicit outcomes include external escape, physical absorption and domain-boundary loss as well as cancellation/failure. A boundary decision carries event identity, direction, target side and target/proposal weight with a documented owner of eta factors. Test pause/resume equivalence and repeated R,R,T boundary visits against uninterrupted runs.

Skip transmission NEE only when the active boundary law certifies zero transmission support. Planar smooth-interface TIR provides such a certificate. Macro-normal TIR or TIR at one sampled facet does **not** certify zero support for a rough boundary marginalized over other normals. No shadow rays are cast for certified-zero kernels.

The implementation worksheet must assign every equation a source location, units, probability measure, normalization and limiting case. Required processes include absorption mixtures, geometric encounters, spheroid chords and internal absorption/reflection, complexes, wavelength-dependent perturbation, fiber events, spectral interfaces, surface folds and bottom-boundary behavior. The source-specific corrections and unresolved interpretations are listed in the research notes.

Do not turn on generic HG/Mie scattering or empirical background absorption to make the image look familiar. Do not replace the published surface-fold rule by GGX VNDF without treating that as a separate model variant. In particular, a normalized sampling procedure alone does not establish a reciprocal BSDF.

### 4.1 Result and termination semantics

Return an explicit outcome:

- `surface_reflected`, `subsurface_returned`, `bottom_transmitted` where the configured experiment permits it, or `absorbed`;
- `cancelled`, `budget_exceeded`, `invalid_parameters`, or `numerical_failure` for nonphysical termination.

An escaped result retains physical position/direction, boundary identity, path length, weight and event history summary. A numerical failure is never counted as absorption. Accumulate incident, escaped, absorbed and unresolved weight separately. A reference run with unresolved histories cannot silently publish a complete reflectance estimate; report a bound or fail its acceptance gate.

The physical reflecting bottom is not a safety depth cap. A production work limit is not a biological absorption process. Use unbiased roulette with compensation for weighted estimators where appropriate, cancellable batches for long analog histories, and explicit unresolved-work accounting for hard limits. Test nearly conservative media and loops at internal interfaces. Weighted roulette is statistical termination, not an unresolved history; do not label its killed weight as physical absorption. Weighted absorption tallies need their own estimator. Use bounded execution quanta and resumable per-lane state for latency/cancellation; a hard history/work limit remains an explicit failed or unresolved measurement, never a zero sample. HWSS must finish or account for every sampled lane; no dropping slow lanes or normalizing by the lanes that happened to finish. Separate monochromatic sweeps are the default scientific workflow; full-range four-lane HWSS is an optional validated renderer mode.

### 4.2 Numerical work that deserves independent derivation

Derive the spherical limit of the spheroid area/volume expression, stable chord intersections, normalization and sampling measures for orientation and surface folds, and the zero-angle limit of the exponential perturbation. Define the current-particle versus predecessor exclusion precisely. Test near-boundary particles larger than a layer: reject invalid configurations or derive a documented alternate rule, never loop forever.

Use geometric normals for layer topology and scene boundaries. Surface folds are an optical process, not a replacement for topology normals. Use existing robust Fresnel helpers only after verifying their ambient-IOR and direction conventions against this kernel. Compile and test under shipping Opto settings as well as ordinary builds; do not fix cancellation by inserting arbitrary epsilons.

## 5. Scientific harness before scene rendering

Build `tools/hylios_measure.cpp` (proposed target `bin/tools/hylios_measure`). Inputs: resolved specimen, data revision, wavelength list, incident direction, launch footprint, boundary experiment, seed and history count. Outputs: machine-readable results plus a human-readable run manifest.

Minimum outputs are wavelength, incident/escaped/absorbed/unresolved counts or weights, layer absorption, reflectance with confidence interval, and event/rejection statistics. Optional angular and spatial histograms must retain their measure: solid-angle bin, surface-area bin, and their joint distribution. Hemispherical reflectance is dimensionless; BRDF and BSSRDF estimates require the appropriate cosine and bin-measure normalization. Singular surface reflection must not be hidden in a finite-density histogram without specifying bin integration.

Use deterministic history IDs so batching/thread scheduling does not alter which experiment was sampled. Use fixed seeds for regression and multiple independent seeds for uncertainty. Archive full resolved parameters, source/data hashes, compiler/build configuration, counts and comparison scripts.

Validation has three distinct layers:

1. **Implementation correctness:** analytic limits, normalization, conservation and independent numerical checks.
2. **Publication reproduction:** published experiment geometry/presets and independently sourced or digitized model curves. Plot-reading uncertainty is part of the tolerance.
3. **Empirical fidelity:** measured skin data with matching acquisition geometry and stated specimen uncertainty. Unknown physiology prevents treating a fit as a unique parameter recovery.

Agree numerical tolerances before looking at the final comparison. For binomial reflectance use Wilson intervals or exact bounds at zero/one; for weighted estimators use replicated estimates. A sensible starting internal cross-estimator gate is `abs(delta) <= 5 * combined_SE + 1e-4` on a preregistered small set of cases, then adjust from an explicit error budget, not to pass a failed test. This is a proposed engineering tolerance, not a published accuracy claim. Account for multiple comparisons in full spectral sweeps. External-data tolerances include measurement and digitization errors and cannot be replaced by that internal threshold.

## 6. RISE integration: establish the estimator contract first

### 6.1 Forward simulation is not automatically a camera-path sampler

The reference model launches incident light forward. Camera PT needs an adjoint sampler or a proven reciprocal response with the correct measures. Stochastic particle placement/rejection, fiber-pass memory and the surface-fold rule deserve particular scrutiny. Reciprocal Fresnel alone does not prove the whole kernel reciprocal.

Before using the walk backwards, derive the forward and adjoint throughput conventions and test reciprocal source/detector swaps. Preserve a faithful forward oracle even if an adjoint or normalized renderer variant differs. If the reference rules cannot support the proposed backward estimator, keep measurement mode available and implement a forward light-transport connection strategy or an explicitly characterized response-table adapter. Do not silently symmetrize data and call it exact reproduction.

### 6.2 Local and nonlocal modes

**Local preview/material mode:** integrate out exit displacement. An SPF-only prototype can test continuation under area/environment lighting, after the adjoint gate. It has no evaluable response for arbitrary next-event connections; a point/directional light cannot be reached by random continuation. Therefore it is a diagnostic milestone with explicit light restrictions, not the final general material.

For usable local rendering, use **explicitly baked specimen assets** in the first production mode. Add a `hylios_measure bake-local` operation (proposed syntax), not a blocking bake in `Job` construction or during shading. Its asset key includes all resolved specimen parameters, fold law, model/data revisions, wavelength grid, exterior spectral IOR, interpolation version and estimator settings. Constant overrides require a matching rebake; mismatches fail preparation with an actionable diagnostic. Spatial painter maps, continuously animated parameters and changing exterior media are unsupported in this mode. A finite palette may be added only as an explicitly defined mixture, not as supposedly exact interpolation of biology.

Air-baked tables cannot serve submerged skin. Allow separately baked constant exterior-IOR spectra; reject unrepresented contact/graded/time-varying exteriors. Exterior-index tests verify correctly keyed assets and rejection of mismatches, not magical invariance of one air table. Analytic boundary/interior factorization is a later research option, not an assumed fast solution.

For these supported assets, build an evaluable spectral response representation. Preferred first production route: a versioned, nonnegative angular response table generated from the validated oracle, with an independently tested sampler and **the same interpolated function** used by `valueNM`, sampling and `PdfNM`. Store reflection mass, absorption mass and any delta component separately. Refine incident/outgoing angular and wavelength grids until held-out comparisons meet a declared error budget. A table is an approximation; preserve provenance, interpolation policy, energy checks and any reciprocity residual. Do not fit only hemispherical albedo and attach a cosine lobe.

**Nonlocal planar mode:** retain joint exit position and direction. Introduce an explicit subsurface transport result rather than reusing diffusion `weightSpatial`, cosine PDF or `Sw`. Its contract separates `sample`, `evaluate/connection capability`, path throughput, and densities in their respective measures. Unknown marginal density is a capability absence, not a fabricated cosine PDF or a delta label. A continuation-only sampler remains a diagnostic with the same light restrictions and no competing NEE discount. A six-dimensional joint table is not the production solution for arbitrary curved/textured bodies.

**Selected P4 feasibility experiment: conditional boundary-event NEE.** Keep the microscopic history explicit and pause at the interior side of an outer boundary encounter, before sampling the external direction. Let `h` be that history, `beta(h)` its correctly weighted adjoint throughput, and `K_t(w_int,w_ext;lambda)` the outer-boundary transmission kernel including the external cosine and transport-mode factors. The boundary kernel and its sampling law must be derived from the chosen fold model; this notation is a contract, not a claim that Eq.8 already supplies a BTDF. For a light sample with full selection/direction density `q_L`, the conditional contribution is:

```text
C_L(h) = beta(h) * K_t(w_int,w_L;lambda) * L_i(w_L) * V / q_L(w_L).
```

For a discrete delta light, use its selection probability and discrete incident contribution, not a fictitious solid-angle density. Sample one continuing boundary event from the reflection/transmission law; its weight is the physical kernel divided by the full event/direction proposal. Reflection continues the internal walk; transmission resumes external PT. A direct emitter hit after transmission competes with NEE **at that same boundary history**, using the conditional proposal `q_B(event=T,w_ext|h)` including branch probability. Retain this PDF/state for the immediate light hit, then follow ordinary PT bookkeeping. For overlapping continuous strategies apply MIS to `q_L` and `q_B`; for a delta light reached by a finite boundary kernel, its NEE estimator has no continuous hit competitor. The unknown marginal PDF integrated over all skin histories is not used. No second NEE estimate is added at a completed skin jump.

P0 pins the four-sided boundary interpretation and selects the named renderer variant; P4 must derive or verify its normal/direction measure, reflection and refraction mappings, rejection/horizon rules, reflection/transmission masses and adjoint factors, including any latent sampling that affects the density. A ray-perturbation law uses its perturbation density/Jacobian, not an assumed microfacet half-vector mapping. Conditioning on a sampled delta microfacet alone does **not** provide an evaluable rough transmission kernel. Require normalization, source/detector, equal-mean and MIS-on/off tests at individual boundary events and full paths. If this cannot be established for the publication interpretation, retain its measurement-only/restricted status and validate `rise_reciprocal_boundary_v1` explicitly. If neither complete model has a valid adjoint/connection estimator, P4 records failure; naming an extension does not waive correctness gates.

**Smooth boundary limit:** the transmission kernel is a Snell delta. Boundary-only NEE then cannot connect a generic point/spot/directional light through that event. The initial nonlocal camera mode supports only configurations for which the conditional estimator has support; fold-free/delta-boundary plus delta-light configurations are rejected conservatively. Smooth-boundary area/environment continuation remains available after the adjoint gate, without invented NEE competitors. General smooth-interface delta-light support needs a separately designed refracted connection from an interior event, manifold strategy, or forward/light-transport method. It is a named unimplemented extension, not an unconditional P7 promise. Local baked-response mode has its own evaluated-scattering support and does not share this boundary restriction. Singular pure-mirror-to-delta paths still obey ordinary renderer support limits.

This choice addresses the missing estimator contract without claiming it is the only possible algorithm. Read [DL-381](DL381_RANDOM_WALK_RECIPROCITY.md) before implementing it: that investigation demonstrates the existing cosine-exit model's limitation, not an impossibility theorem for all nonlocal methods.

Scope the interface as an optional material capability (proposed `IHyperspectralSkinTransport`) with a prepared scene/material capability record. Resolve it once during preparation; avoid per-ray broad RTTI checks and changes to hot intersection layout. Do not add a HyLIoS-specific query to `ICamera`. Decide the precise public interface layout during P4; `IJob` additions remain tail-appended.

### 6.3 Compatibility policy

| Consumer | First supported policy | Required proof before widening |
|---|---|---|
| Standalone forward NM | Canonical reference | Data/equation gates and measurement agreement |
| Pure PT NM | Primary renderer target after adjoint/connection gate | Correct surface/volume partition, throughput, light endpoints, cap accounting |
| HWSS | Run each active lane through its own NM transport, preserving per-lane IOR state and bundle normalization | First-hit and mid-path handoffs; no shared hero geometry for wavelength-dependent walks |
| Pel | Explicit preview only; scientific requests fail or explicitly choose spectral mode | Documented preview generation under a named illuminant; no independent R/G/B walk directions fused into one ray |
| Auto spectral | Route to supported PT when any HyLIoS leaf requires it | Wrapper/instance discovery and diagnostic tests |
| Auto Pel | Preview only or diagnostic requiring spectral selection | Never silently reinterpret the user's color pipeline |
| BDPT / VCM | Refuse unsupported HyLIoS transport at preparation | Reverse densities, spatial Jacobians, connection/merge support and strategy-partition proof |
| MLT / frozen shader-dispatch | No new supported path in this project | Explicit later scope decision, not assumed legacy fallback |
| SMS / guiding | Do not treat the internal walk as a specular manifold or train on unknown densities | Independently valid surface capability or connection model |
| Mix / coat / alpha / modifiers | Start with explicit refusals where not modeled; preserve ordinary opaque material use | Every wrapper forwards capability and path dependence; endpoint choice, coverage and boundary accounting tests |

These restrictions need a transport-capability mechanism; do not misuse `PathDependence` flags to classify a physically valid material as nonphysical. Refusals must be consistent across parser, direct API, CLI, auto routing, GUI and snapshots. Existing scenes without HyLIoS must retain behavior.

### 6.4 PT correctness checklist

**Initial external reflection NEE.** At every exterior skin hit (including the first camera hit), before conditioning on a particular fold normal, evaluate the active boundary's marginalized reflection kernel `K_r(w_o,w_L;lambda)` with its external cosine included. For continuous light sampling the contribution is `beta * K_r * L_i * V / q_L`; the competing continuation density is `q_B(event=R,w_L|x,w_o)`, including the reflection-branch probability. Carry that density to the immediate emitter/environment hit. Delta-light NEE uses its discrete selection measure and has no continuous continuation competitor. Do this exactly once at the hit; the interior-boundary `K_t` estimator handles a different event and does not replace it.

For a microfacet reflection mapping, convert the sampled normal density with `1/(4*abs(dot(w_o,h_r)))` and the actual branch probability; include normalization/rejection effects of the chosen sampling law. This Jacobian is a density transform, not the entire reflection BSDF. Derive `K_r` from the same physical boundary law as transmission; if the reference interpretation instead perturbs rays, derive that mapping separately. Sampling a mirror facet first and attempting ordinary NEE conditioned on it misses the continuous rough reflection kernel. In the smooth limit, retain the delta measure and the declared specular/delta-light support restrictions.

Add isolated first-surface tests: black interior under point/spot/directional lights and small area/environment sources, sample/eval/PDF agreement, NEE/continuation mean comparisons for finite emitters, and a no-double-count furnace. These are independent of the subsurface-exit tests.

**One continuation at the initial interface.** For the nonlocal renderer variant, sample a single event from its complete, consistent boundary law using the same exterior/interior indices. If that law samples facets, Fresnel reflection/transmission probabilities at each facet sum to one; account for their likelihood once. Masking, invalid directions and any multiscatter compensation also belong to the declared law and its energy tests; facet R+T alone does not prove macroscopic unit energy. For any biased branch proposal, divide by its actual probability. Do not pair a GGX surface BSDF with a different folded entry law, and do not copy the existing SSS branch's additional recursive continuation. NEE is an additive estimator, not a second continuation path.

For local baked mode, the response already includes surface reflection and subsurface return. Select from its validated complete mixture, preserving separately represented delta mass. Do not put a second Fresnel coat over the total baked response. Furnace tests cover all partition modes, ambient indices and roughness limits.

New nonlocal continuation should remain in the PT iterative state machine (or a bounded explicit work stack), with exit intersection/medium state, counters, AOV and MIS state rebuilt deliberately. Do not route each skin bounce through recursive `PTCastRay<Tag>`/`RayCaster::CastRayNM`. Test repeated skin-to-skin events and `castRRCompensation_` ownership. Scientific render preparation must reject nonzero direct/indirect contribution clamps, unsupported biased depth truncation and incompatible denoising/display settings; emit the resolved settings in the manifest. Weighted roulette is permitted. A user-authored scene is not sufficient evidence that bias controls are disabled.

Audit `PathTracingIntegrator.cpp`, `RayCaster.cpp`, light sampling and continuation handoffs together. A sampleable non-delta material with unknown density must not receive a false MIS discount. Apply entry/exit Fresnel exactly once, account for ambient medium indices and basic-radiance eta scaling according to RISE's complete-event convention, and restore the external medium state on return. Do not multiply a closed skin excursion by a second external eta factor.

Preserve emitter self-exclusion, alpha acceptance, geometric horizons, exit object identity, bounce categories, render masks, clay modes, AOV selection, cancellation and event counters. Internal micro-events are not each a scene-level subsurface bounce. Surface reflection and a nonlocal skin event need distinct accounting. Read the SSS debt-closure documents before adapting the current plumbing; resemblance is not proof of an identical estimator.

Pel and NM share code where the semantics actually match. HWSS tests must cover both first-hit and mid-path fallback. A supported local table material also needs its own audited HWSS rule: wavelength-dependent sampling/absorption cannot inherit a generic companion reweighting path by accident.

## 7. Curved geometry and spatial fields

An infinite layered slab is the scientific baseline. A generic closed mesh does not specify six internal layers. Nearest-point depth or entry-normal planes are not globally consistent material coordinates on noses, ears, folds or touching surfaces.

Implement the spatial extension in this order:

1. Planar analytic layers with physical exit positions and finite domains whose side/bottom conditions are explicit in both measurement and PT. Initial finite-host mode is a polygon footprint extruded through the layer depth, with **absorbing side walls** as an authored physical boundary condition. Intersect the side walls alongside layer/event distances; on a side crossing terminate with a distinct domain-loss tally, not tissue absorption. Particle placement must respect the footprint as well as layer boundaries. No walk may leave the footprint, continue in an infinite slab and later re-enter unnoticed. Top exits must map to a real host face with valid UV/barycentric data and side ownership; an unmappable exit is an error, not a floating-space vertex. Never clamp, project, wrap or resample the exit onto the host. Reject other finite-side policies until implemented. Infinite-slab measurement remains a separate domain; use increasing footprint sizes to quantify absorbing-edge bias against it. Transmitting side walls later require actual side-surface geometry and a fully specified exit law.
2. Analytic concentric layered bodies for geometric tests; compare the large-radius limit against the slab.
3. For the first production curved-body design, use one outer mesh plus an authored, direction-independent **intrinsic layer-coordinate volume** (validated prism/curved-shell charts are the initial prototype). Define the outer chart, depth function, layer interfaces, chart transitions and dermal closure in one body-owned geometry policy; do not register five hidden ordinary scene objects. Trace internal interfaces using that coordinate volume and geometric normals. A nearest-surface SDF is an alternative only inside a certified region with unique coordinates and a correct distance/gradient contract.
4. Validate chart injectivity, curvature/reach bounds, seams, thin features, self-intersections and collapsed layers before use. Refuse unsupported regions rather than choosing the entry-normal plane or nearest triangle on the fly. Automatic chart/shell generation for arbitrary production meshes is separate work. Explicit nested watertight meshes remain optional authored fixtures where nonintersection and resolution are certified; they are not the default character-asset workflow.

Microscopic coordinates and intersections use local high-precision state. Do not propagate the scene-scale `BSSRDF_RAY_EPSILON` into particle or inter-layer traversal. Carry topological side ownership and use error-bounded scene-ray spawning only at the exterior handoff. Test translated bodies, submicron particle chords, minimum layer separation and actual vertex/transform precision. Neither an SDF nor a chart removes precision and topology obligations. The primary representation is chosen in P4's geometry spike and cannot enter P7 with an unvalidated mapping.

Pin physical scale explicitly: skin thickness and particle dimensions must stay correct under object and instance transforms, including nonuniform scaling. A regression must show that changing scene-coordinate units while preserving physical dimensions preserves transport. Do not assume the historic `[1/m]` SSS comment establishes every asset's scale.

For textured fields, define coefficients at an interior world/material point independent of the ray's entry. Use integrated optical depth or a validated majorant/null-collision method for variable rates. A free-flight sample from a frozen endpoint rate is not generally valid. Define how anatomical layer coordinates reach UVs, how seams behave, and when invalid mapping is rejected.

The paper's reflecting hypodermal closure is a model boundary, not a substitute for all real anatomy. Transmitting thin ears and contact between separate bodies require an explicitly configured geometry extension; do not claim those effects from the closed-bottom slab. Cross-object continuation must be deliberately implemented or refused, never inherited from homogeneous RW contact code.

## 8. Hyperspectral measurement output

Do not widen the global CIE or RGB-upsample domain to make UV/IR images appear. Add a dedicated `HyperspectralRadiometricRasterizer` and scalar/band film (proposed names), reusing validated camera/PT sampling rather than subclassing the colorimetric accumulation path. Call `IntegrateRayNM` only after the source/selection audit below. The raw film owns float output storage, high-precision accumulation, counts, scalar Welford variance, cancellation and checkpoint state. It bypasses `mYNormalization`, `XYZFromNM`, `ProgressiveFilm`/`FilteredFilm` color sums and RGB resolves, and display `ViewTransform`.

Use a separate versioned radiometric result/schema and raw writer. FrameStore may hold a derived RGB preview and a link to the raw result; it must not masquerade raw bands as `RISEPel` beauty. Update closed-map provenance validation (`ExactMapKeys`) deliberately for any new reference/metadata fields; do not weaken existing validation. Test checkpoint/resume and raw serialization independently of the preview. Fixed-sample scalar accumulation ships first; enable adaptive stopping only after scalar band-variance tests, including low-signal cases, prove it does not inherit the XYZ-Y 64-sample shortcut.

### 8.1 Spectral source and proposal prerequisites (P6a)

Current light/environment proposals use visible RGB energy, and point/spot/directional implementations use `RGBIlluminantSpectrum` with visible-only D65. Add optional physical SPD inputs to those lights, with explicit units and scale, through API/Job/descriptors, lifetime, animation/snapshots and NM emission. Retain current RGB authoring unchanged. Audit both direct-light evaluation and sampled emission; a field used only by `emittedRadianceNM` is incomplete. Inspect the current sky's visible-only support and reject it for unsupported measurement bands unless replaced with an independently supported spectral source.

Do **not** use current `IEmitter::averageRadiantExitanceNM` as the source of out-of-visible power: Lambertian, Phong and Composite emitters return an `averageSpectrum` stored as `VisibleSpectralPacket<380,780,40>`. Integrate `radiantExitanceAtNM(rig,nm)` over valid emitter surface records and the requested wavelength quadrature instead, preserving object coordinates, UVs, transforms, scale, time and sidedness, as the object-aware RGB averaging path does. Audit wrappers and emitted directional radiance separately; a correct new proposal does not repair a visible-only emission evaluator. Add a fixture whose pointwise SWIR exitance is positive while its legacy average packet is zero.

Band-specific LightSampler/EnvironmentSampler tables belong to a render-pass context owned by the rasterizer/caster, constructed during preparation and immutable during sampling. Do not write band caches onto shared frozen `ILight`, `IEmitter` or `IRadianceMap` objects or mutate another pass's proposal. Rebuild per-context caches on the declared keys; preparation and rendering lifetimes must retain the scene snapshot and referenced assets. Test two live contexts for distinct bands on the same immutable scene using small unit fixtures, without launching concurrent full-scene renders.

Prepare band-aware emitter and environment proposals using nonnegative radiometric weights over the requested wavelength domain, including geometry/area and sidedness as appropriate. For fixed-wavelength sweeps a wavelength-specific proposal is possible; a shared HWSS proposal must cover every active lane. Positive spectral support cannot be excluded because its RGB projection is zero. Approximate spectral/spatial quadrature cannot certify absence: preserve support with a documented positive fallback/uniform mixture or conservative bound. Ensure the same proposal/mixture PDF is returned to emitter-hit MIS, environment NEE, light-BVH and RIS consumers; invalidate caches on band, SPD, time or geometry changes.

Update light-sample roulette to use a compatible spectral estimate with compensated positive survival, or disable it in the first radiometric mode. A tiny visible tail is primarily a variance/support hazard, not automatically numerical underflow. Tests must include a pure UV/SWIR emitter, a visible emitter mixed with a tiny-visible-tail SWIR emitter, spatial spectral environment patches, and a narrow line missed by coarse proposal quadrature. Verify NEE/continuation agreement and actual NM emission. These changes precede any out-of-visible material integration claim.

### 8.2 Raw bands and derived display (P6b)

First implement deterministic wavelength sweeps: one floating-point image per wavelength plus a JSON manifest. Use existing suitable linear floating-point I/O if it can bypass color transforms; otherwise add a dedicated writer. A single multichannel EXR cube can follow; current FrameStore documentation does not establish that capability as shipped.

Declare whether samples are spectral radiance density `L_lambda` per nm, band-integrated radiance, band-average radiance, or response-weighted sensor output. For arbitrary wavelength sampling the estimator includes `1/p(lambda)` with density measured per nm. For a sensor response `s(lambda)`, energy detection integrates `L_lambda*s(lambda)`; photon detection additionally uses `lambda/(h*c)` with consistent length units. Normalized sensor averages divide by the response integral only when that is the requested observable. No CIE Y normalization in raw measurements.

Require explicit optical coverage for illuminants, environment maps, sensor and material. RGB-upsampled lights/environment maps do not become calibrated UV/IR sources. Use tabulated radiometric SPDs and declared emitter scale; audit `GetRadianceNM` versus `GetColorNM` and scalar coefficients. Validate white/black targets and a known spectral source before skin.

Use fixed samples first. Later band-adaptive sampling must measure band variance, not visible luminance. Disable tone mapping, display exposure, denoising, firefly clamping and negative-lobed reconstruction for scientific comparisons. Present false color/grayscale as derived visualization with a recorded mapping; never overwrite raw results. Visible beauty rendering retains RISE's normal color pipeline and OIDN/FilteredFilm invariant.

Sweep wavelengths and render scenes sequentially. Estimate output storage and memory before launching a cube; stream bands rather than retaining all full-resolution planes.

## 9. Implementation sequence and exit gates

Every row is a coherent implementation/review increment, not a calendar estimate. Data reconciliation and estimator proofs dominate uncertainty; no credible fixed delivery date exists yet.

| Phase | Concrete change | Dependencies | Exit gate |
|---|---|---|---|
| P0: evidence freeze | Equation/units ledger, four-sided boundary interpretation and renderer-variant decision, data coverage manifest, explicit ambiguity resolutions, authoritative presets and test experiment definitions | None | Every mandatory process has a source or labeled interpretation; no unresolved dimensional contradiction in executable defaults |
| P1: data and primitives | Immutable dataset, conversion generator, absorption/IOR functions, spheroid and angular samplers | P0 decisions for each primitive | Independent dimensional, normalization, boundary and limit tests; no fabricated missing curves |
| P2: forward kernel | Analog and weighted/roulette estimators, layer/particle state, interfaces, fiber-pass state, physical bottom, resumable work | P1 | Conservation and analog/weighted mean agreement; long-walk and cancellation accounting; zero sanitizer failures |
| P3: measurement oracle | CLI, spectra and joint histograms, S1–S4 experiment fixtures, statistical comparison reports | P2 + data coverage | Repeatable reports; quantitative reproduction in declared bands; full-range label withheld until complete |
| P4: renderer contract | Adjoint proof, external reflection and interior transmission NEE derivations (§6.2/§6.4), baked-local asset contract, single-event partition, geometry-coordinate spike, capability census | P3 | Local bake and nonlocal boundary estimators independently validated; smooth/delta restrictions explicit; no invented marginal PDF |
| P5: spectral PT material | API/Job/parser/CST/editor/preview plumbing, baked local assets and PT/HWSS behavior | P4; P6a additionally for out-of-visible lights | Supported area/env/delta-light tests, bake-key rejection, sample/eval/PDF agreement, non-HyLIoS regressions |
| P6a: spectral sources | Physical SPDs for delta lights; band-aware emitter/environment proposals, PDF propagation, support-preserving fallbacks | Independent engine work after P0; controls need no skin material | Pure UV/SWIR and mixed-source tests, correct selection/MIS support, no visible-energy roulette failure |
| P6b: radiometric output | Dedicated raw rasterizer/film, variance, writer, versioned schema and preview separation | P6a; P3 for skin comparisons; may precede P5 using controls | Bandwidth/normalization, raw roundtrip/resume and UV/IR calibration without CIE conversion |
| P7: spatial transport | Conditional boundary-event PT, planar/analytic curved tests and validated intrinsic layer-coordinate body | P4 estimator/geometry gates + P5 plumbing + P6b | Joint/conditional estimator tests, scale/precision/topology gates; supported rough-boundary lights pass; smooth-boundary delta-light refusal tested |
| P8: release validation | Publication/measurement reports, showcases, full build/test gates, performance characterization | P5–P7 | Documented supported modes/bands; fresh independent review; reproducible evidence bundle |
| P9: optional acceleration | Better proposals, validated response compression, progressive caches or GPU experiments | P8 baseline | Equal-mean tests, held-out error bounds and wall-time-normalized gains |

P1/P2 can use synthetic spectra to exercise mechanics while data is collected; label them synthetic and do not promote them to specimen defaults. P3 may report a narrower verified wavelength domain. P5 is a useful visible milestone, but P8 is not complete without the planned spatial and hyperspectral capabilities. P8 may document the explicit nonlocal smooth-boundary/delta-light exclusion; it may not claim general-lighting support for that case. P7 may use the explicitly named renderer boundary extension if the reference boundary cannot support its estimator; comparison evidence must show that model change. Failure to validate the complete chosen renderer model still leaves P7 blocked rather than satisfying it with a continuation-only demo. P9 covers optional acceleration beyond P2/P3 weighted transport, not basic termination correctness.

Do not begin with a neural surrogate or a giant precomputed parameter grid. Their training domain, interpolation error, memory cost and derivatives would create another model to validate. Immutable offline tables are reasonable only once the exact observable and accepted approximation error are pinned.

## 10. Test plan

Proposed executable names below follow RISE's standalone-test convention; they are not present yet. Register targets, schedule tiers and relevant platform test builds when implementing them.

| Proposed test family | Discriminating cases |
|---|---|
| `HyLIoSDataTest` | `ln(10)` exactly once; cm/m/µm conversions; percent vs fraction; mass/molar basis; Hb conventions; absorptance zero/censoring; coverage edges and missing active constituents |
| `HyLIoSOpticsTest` | Sphere limit; normalized orientation/angle laws and sampled moments; projected-area versus surface-area sampling; zero-angle forward limit; real spheroid chord distribution; matched-index and critical-angle boundaries |
| `HyLIoSTransportTest` | Vacuum/matched slabs; analytic Beer transmission; conservative R+T+A; absorbing and reflecting bottoms; one fiber event per defined pass; particle overlap/layer rejection; no double absorption; cancellation distinct from absorption |
| `HyLIoSMeasurementTest` | Known Lambertian BRDF/BSSRDF bin normalization; independent incident/exit accounting; histogram integration equals total reflectance; fixed-seed batching; statistical replica agreement |
| `HyLIoSAdjointTest` | Source/detector swaps at multiple incident/outgoing angles and spatial offsets; ambient IOR changes; surface-fold and particle rejection cases; forward versus camera prediction |
| `HyLIoSIntegrationTest` | Parser/API/CST equivalence, preset expansion, rename/clone/lifetime, scalar validation, unsupported integrator/refusal, wrapped leaves, alpha/coat/mix policy, sample/eval/PDF agreement |
| `HyLIoSSpectralTest` | NM versus active HWSS lanes at first and later hits, dispersed boundary IOR, terminated lanes, zero-result bundles, band means and variance, preserved prior path state |
| `HyLIoSSourceAndFilmTest` | UV/SWIR delta/area/environment sources; support-preserving selection and MIS PDFs; tiny-visible-tail and narrow-line cases; scalar variance, raw schema/roundtrip/resume; mechanical bias-control rejection |
| `HyLIoSSpatialTest` | Planar lateral spread, joint position/angle correlation, thin/wide analytic bodies, nested layers, nonuniform transforms, same-object and other-object boundaries, invalid topology |
| `HyperspectralOutputTest` | Monochromatic control, flat spectrum under differently sized bands, nonuniform wavelength PDF, sensor energy/photon weighting, raw roundtrip, no tone/XYZ normalization leakage |

Use an independently written small calculation for analytic or quadrature references, not another call into the implementation being tested. Perturb/remove a process to show that the relevant test fails. In particular, test clustered versus homogeneous melanin at equal pigment amount, and layer rearrangements at fixed totals: these must distinguish the new model from the old proposed shortcut.

Reuse existing regression families where applicable: `ExteriorIndexInvarianceTest`, `SSSBoundaryFurnaceTest`, `SSSBoundarySpectralTest`, `SSSExteriorIndexInvarianceTest`, `SSSHWSSCompanionTest`, `RandomWalkSurvivalTest`, `SpectralSplatIntegralNormalizationTest`, `SpectralSplatActiveCountTest`, `SpectralOnlyEmitterTest`, `CstRenderEquivalence.h`, `MaterialPreviewTest`, `SceneEditorSuggestionsTest`, `IJobVtableManifest.txt` and `SourceHygieneTest`. Passing old homogeneous-SSS tests alone does not validate HyLIoS.

Fast CI uses analytic cases and modest statistically justified samplers. Extended tests hold spectral sweeps and reference comparisons. Render fixtures live under `scenes/Tests/Materials/HyLIoS`; curated portraits/band comparisons under `scenes/FeatureBased/HyLIoS`. Start with procedural geometry; optional Roberto assets must not be a mandatory reproducibility dependency.

## 11. Public authoring and implementation surfaces

Reserve `hylios_skin_material` as the scene keyword. Use native v7 syntax, own-line braces, `Describe()`/`Finalize()` and diagnostic-bearing `Job` construction. A new additive chunk alone need not bump `CURRENT_SCENE_VERSION`; explicitly assess this when adding any new document-level output semantics.

Prefer a versioned parameter structure over dozens of positional factory arguments. Proposed factory: `RISE_API_CreateHyLIoSSkinMaterial`; proposed tail-appended job method: `AddHyLIoSSkinMaterial`. Separate resolved physical parameters from authored references. Specify struct initialization, version/size handling, painter ownership and diagnostics before exposing the API. There is no need to rename or reorder existing methods.

Authoring needs explicit `model_revision`, `data_revision`, `boundary_mode`, boundary-definition revision, `preset`, transport mode, physical scale, organization and parameter units. Baked-local mode requires `response_asset` (proposed parameter), declared as descriptor `Filename` with normal `RISE_MEDIA_PATH` resolution; other modes reject irrelevant assets. Preserve the authored path through CST round trips, register it in any applicable asset/dependency enumeration, and distinguish file paths from named-entity references in rename handling. Resolve/open the asset at preparation, validate its header and complete bake key, and retain its immutable content identity for the render. Provenance records authored/resolved path, content SHA-256, format version, grid/encoding, model/data/boundary revisions and full bake key. Missing/stale assets fail explicitly; no network fetch or implicit rebake during rendering.

P4 must preflight bake size and runtime memory before allocating. Example sizing only: `256 wavelengths * 32 incident polar * 32 outgoing polar * 64 relative azimuth * 4 bytes = 64 MiB` for values alone; CDFs, metadata and work buffers are additional. Grid resolution is set by error tests, not this example. Initial configurable safety budgets are 256 MiB per file and 1 GiB resident memory per loaded asset, with an aggregate render budget; overflow or excess fails before allocation and asks for an explicit budget/configuration change. Share identical immutable assets across materials within a render. Start with float32 values and reconstruct sampling distributions at load under the memory budget; finite, nonnegative, energy/PDF-normalized validation is mandatory. Float16/compression or lazy rows require separate error and memory tests, not automatic precision reduction to fit.

Keep tiny deterministic analytic fixtures (target at most 2 MiB each) in Git; production MC bakes and caches are generated/ignored artifacts. Check in bake recipes, manifests and hashes for S1–S4, not large generated tables. CI unit tests must work offline with the tiny fixtures; extended reproduction can use explicitly provisioned, hash-verified assets. Persist MC uncertainty/replicate counts and grid error evidence with a bake; a large table is not a noise-free oracle. A preset plus overrides must produce a reproducible resolved configuration. Keep interpretation choices in versioned model definitions, not a collection of unexplained boolean switches. Surface a smaller common parameter group in the inspector, with advanced layer/data details available separately.

The implementation checklist includes:

- `RISE_API.{h,cpp}`, `Interfaces/IJob.h`, `Job.{h,cpp}`, vtable manifest and construction diagnostics.
- `Parsers/ChunkParserRegistry.cpp`, descriptor references and validation; `Cst.cpp` reference extraction, rename and graph-affecting parameters for any custom string references.
- `MaterialIntrospection`, `EntityTemplates`, `ConnectionLegality`, `MaterialPreview`, `PainterPreview`, shared `InteractivePelRasterizer`, `SnapshotLeafClone`, CST render-equivalence coverage and agent-readable capability diagnostics.
- `ObjectManager::AuditedMaterialGraph` exact-type audit and prepared capability census across CSG/instances, `MixMaterial`, coat/fabric and luminaire wrappers. Wire the census into `IObjectManager`/`ObjectManager`, Auto and explicit BDPT/VCM/MLT Pel/spectral refusal paths without false `PathDependence` bits.
- `HasSubsurfaceLeaf` must recognize nonlocal HyLIoS. Add a geometry-requirement capability to Job's DL-409 validation rather than claiming the new model is homogeneous RW; preserve `MixMaterial::GetPathDependence` varying-SSS checks. Local baked mode and planar diagnostics have different geometry requirements.
- `AgentSession.cpp` subsurface/material-role validation, descriptor `ParameterSemantics.pipe = Scalar` for scalar sockets, and `SourceHygieneTest` prose/enumeration guards.
- `SceneEditorSuggestionsTest.cpp` pinned keyword count, `tests/test_schedule.tsv` classes (`single`, `wide:N`, `exclusive`, or default `pool`) and `build/cmake/rise-tests/CMakeLists.txt` Windows registration. Full-core renders run sequentially and rendering test executables require appropriate scheduling.
- `AutoRasterizer`, unsupported explicit integrators, `RayCaster`, PT, new radiometric rasterizer/film, light source/proposal paths and FrameStore/output provenance. Add AOV and sensor/film schema validation where new parameters require it.
- Reference-counted ownership and immutable prepared-state lifetime; no storing scene pointers on mutable managers.
- All five library build files: `build/make/rise/Filelist`, `build/cmake/rise-android/rise_sources.cmake`, `build/VS2022/Library/Library.vcxproj`, its `.filters`, and `build/XCode/rise/rise.xcodeproj/project.pbxproj`.
- Data attribution under `licenses/`, source/data provenance generator and third-party notice checks as applicable.

Define one versioned visible-guide projection shared by `EvaluateMaterialAOVGuides`, Pel/interactive preview, and `MixMaterial::EvaluateExpectedGuides`: D65-weighted, white-normalized visible hemispherical reflectance with a declared incident-angle convention (initially normal incidence), converted through the existing working-RGB color pipeline. It is a bounded denoiser/preview guide, not the physical transport response. The guide source is mode-specific: baked-local mode derives it from the validated response asset; nonlocal and diagnostic modes compute a small fixed-seed slab estimate of hemispherical reflectance over a declared visible wavelength grid during material/render preparation, without a `response_asset` or a 4D angular bake. This one-dimensional spectral guide is a labeled local appearance approximation for nonlocal transport, not a spatial transport measurement. Both routes produce the same guide representation and projection convention. Cache the immutable guide by resolved specimen, model/data/boundary revisions, exterior-IOR spectrum, and guide grid/seed/sample-count/algorithm revision. Keep preparation RNG separate from render sampling; do not mutate a shared frozen material or recompute per UI pixel. Use only supported constant specimens/exteriors initially; spatial maps require a separately defined guide policy. Set a measured preparation budget and quality target, account for unfinished histories, and report preparation failure or unavailable guide explicitly rather than treating truncated walks as absorption. Do not assume millisecond preparation cost before benchmarking. Bypass the current SPF-only white-guide fallback and surface-only albedo for this capability. Regression-test guide equality across these consumers (including `MaterialPreview`), mixed leaves and spectral beauty, without depending on OIDN to hide transport errors. Add a nonlocal preparation fixture with no response asset, deterministic guide reproduction, parameter/exterior-IOR cache invalidation, and statistical agreement between baked and estimated guide sources for the same specimen; do not require bit equality between independent estimators.

Interactive material previews use this cached visible approximation from the same versioned specimen, with quality/mode labeling. They must not launch a costly microscopic walk per UI pixel. Share preview behavior across macOS, Windows and Android, leaving bridges responsible only for delivery and lifecycle.

## 12. Review, performance and release criteria

For each nontrivial implementation increment follow [implementation-review-loop](skills/implementation-review-loop.md), [adversarial-code-review](skills/adversarial-code-review.md), [audit-by-bug-pattern](skills/audit-by-bug-pattern.md) and [write-highly-effective-tests](skills/write-highly-effective-tests.md). Reviewer axes should include physical/data fidelity, estimator/adjoint correctness, and integration/test completeness. Fresh review must examine the current increment after fixes.

Run focused executables, relevant fast/extended regressions, and the full suite for release. Clean make and Xcode Deployment builds must be warning-free subject to documented third-party/missing-checkout exclusions; shipping Opto additionally gates FP/LTO behavior. Use sanitizers for geometry, ownership and state-machine changes. Compile all new platform lists or run their available consistency gates; state untested platforms explicitly.

Record histories/second, median and tail event counts, particle rejection counts, unresolved work, time per spectral band, memory per worker/material and image variance at fixed wall time. Use `render_thread_reserve_count 0` for benchmarks and run renders sequentially. Follow [performance-work-with-baselines](skills/performance-work-with-baselines.md) and [variance-measurement](skills/variance-measurement.md) before making speed or variance claims. Do not promise real-time reference transport.

Release acceptance requires a source/data manifest; resolved ambiguity ledger; verified supported bands; quantitative model reports; raw spectral outputs; planar and curved spatial evidence; PT/NM/HWSS correctness; supported light types; explicit exclusions; build/test/review evidence; and user-facing documentation distinguishing reference, approximation and geometry extension.

**Recommended first implementation increment:** P0 plus P1's units/coverage primitives and analytic fixtures. The first scientific milestone is a reproducible S3 reflectance experiment with uncertainty and complete provenance—not a portrait and not a new material factory alone.
