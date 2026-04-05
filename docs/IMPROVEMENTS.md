# RISE Renderer Improvements

This document catalogs the most impactful rendering improvements for RISE, informed by the major advances in offline physically based rendering from 2010-2025. Each item is scoped for an agent or contributor to pick up independently. Items are ordered by estimated ROI (impact per implementation effort).

This document covers material models, samplers, volume formulations, transport algorithms, and other systems. It supersedes the earlier `PATH_TRANSPORT_ROADMAP.md`.

## Current RISE Baseline

RISE already has significant infrastructure in place:

- Full bidirectional path tracing with explicit light and eye subpaths
- Spectral rendering (sampled spectrum, per-wavelength PT/BDPT)
- SMS / manifold solving (Zeltner et al. 2020)
- OpenPGL path guiding with RIS and variance-aware adaptive alpha (PT and BDPT eye subpaths)
- BSSRDF-aware transport (Donner-Jensen, BioSpec, Burley diffusion profiles) in both PT and BDPT
- Owen-scrambled Sobol samplers
- PSSMLT via BDPT
- Caustic, global, and spectral photon mapping
- Intel OIDN denoising with albedo + normal AOVs
- Alias table + spatial RIS for many-light sampling
- Homogeneous and heterogeneous volume support with phase functions
- Environment map importance sampling

The improvements below target gaps where the field has advanced beyond what RISE currently implements.

---

## Ranked Improvements

| Rank | Improvement | Category | Effort | Depends On |
|------|------------|----------|--------|------------|
| 1 | GGX microfacet + VNDF + Kulla-Conty multiscattering | Materials | Medium | None |
| 2 | Light subpath guiding in BDPT | Transport | Medium | None (eye guiding complete) |
| 3 | Random-walk subsurface scattering | Materials | Medium | None (disk projection complete) |
| 4 | Light BVH for many-light sampling | Lights | Medium-Large | Roadmap Rank 1 |
| 5 | Hero wavelength spectral sampling (HWSS) | Spectral | Medium-Large | None |
| 6 | Blue-noise screen-space error distribution (ZSobol) | Sampling | Small | None |
| 7 | Null-scattering volume framework | Volumes | Large | Roadmap Ranks 5-6 |
| 8 | Optimal and correlation-aware MIS weights | Transport | Medium | None |
| 9 | VCM (Vertex Connection and Merging) | Transport | Medium-Large | None |
| 10 | Hair/fiber BSDF (Chiang et al. 2016) | Materials | Medium | 1 (GGX foundation) |
| 11 | Jakob-Hanika sigmoid spectral uplifting | Spectral | Small | 5 (HWSS) |

---

## 1. GGX Microfacet Model With VNDF Sampling And Multiscattering Compensation

### Why This Is First

RISE currently has Cook-Torrance, Ward, and Ashikmin-Shirley microfacet models. All predate 2010. GGX/Trowbridge-Reitz with Smith height-correlated masking is the universal standard in every production renderer (Arnold, RenderMan, Hyperion, Cycles, Manuka). This is a self-contained material addition that improves every dielectric and metallic surface in the renderer without touching integrator code.

### What To Implement

#### 1A. GGX NDF and Smith height-correlated masking-shadowing

Add a GGX (Trowbridge-Reitz) NDF with isotropic and anisotropic roughness. Use the Smith height-correlated joint masking-shadowing function G2 = 1 / (1 + Lambda(wi) + Lambda(wo)), which is more physically accurate than the separable form G1(wi) * G1(wo). Validate with the white furnace test (Heitz 2014): a white Lambertian environment should produce unit albedo at all roughness values for a lossless material.

Key references:
- Walter et al. EGSR 2007 (GGX NDF definition)
- Heitz, JCGT 3(2) 2014 (Smith masking unification, white furnace test)

#### 1B. VNDF (Visible Normal Distribution Function) sampling

Replace cosine-weighted or NDF-weighted half-vector sampling with VNDF sampling, which eliminates wasted back-facing samples and grazing-angle fireflies. The Dupuy and Benyoub (CGF/EGSR 2023) spherical-cap method is ~20 lines of C++ and strictly improves on Heitz's 2018 ellipsoidal method.

Key references:
- Heitz, JCGT 2018 (original VNDF sampling)
- Dupuy and Benyoub, CGF/EGSR 2023 (simplified spherical-cap method)

#### 1C. Kulla-Conty multiscattering energy compensation

Standard single-scatter microfacet BRDFs lose up to ~60% of energy at roughness alpha = 1. Precompute a 2D LUT of directional albedo E_ss(cosTheta, roughness) via Monte Carlo integration of the single-scatter BRDF. At shading time, add the Kulla-Conty diffuse compensation lobe: f_ms = (1 - E(mu_o)) * (1 - E(mu_i)) / (pi * (1 - E_avg)). For metals, handle Fresnel absorption across multiple bounces via F_ms = F_avg * E_avg / (1 - F_avg * (1 - E_avg)). Alternatively, use the simpler Turquin (2019) approach: scale the single-scatter lobe by 1/E_ss(mu_o) (loses reciprocity but is trivial to implement).

Key references:
- Heitz et al., SIGGRAPH 2016 (stochastic microsurface random walk, ground truth)
- Kulla and Conty, SIGGRAPH 2017 Course (practical energy compensation)
- Turquin, ILM Technical Report 2019 (simplified 1/E scaling)

### Current RISE Files

- `src/Library/Materials/CookTorranceSPF.h` / `.cpp` (existing microfacet, pattern to follow)
- `src/Library/Materials/CookTorranceBRDF.h` / `.cpp`
- `src/Library/Interfaces/ISPF.h` (scattered photon function interface)
- `src/Library/Interfaces/IBRDF.h` (BRDF interface)
- `src/Library/Utilities/MicrofacetEnergyLUT.h` (may already have LUT infrastructure)
- `src/Library/Utilities/MicrofacetUtils.h`

### Deliverables

- GGX SPF and BRDF classes with isotropic and anisotropic roughness.
- VNDF importance sampling (Dupuy-Benyoub 2023).
- Precomputed 2D energy LUT and Kulla-Conty compensation lobe.
- White furnace test (standalone executable or scene).
- Parser support: new material type in `AsciiSceneParser.cpp`, wired through `Job.cpp` and `RISE_API.h`.
- At least one test scene demonstrating roughness sweep (alpha 0 to 1) with energy conservation.

### Acceptance Criteria

- White furnace test passes: albedo within 1% of unity across all roughness values.
- No grazing-angle fireflies at low roughness (VNDF eliminates these).
- Visual comparison with Cook-Torrance at equivalent roughness shows wider GGX tail.

---

## 2. Light Subpath Guiding In BDPT

### Why This Is Second

RISE already guides eye subpaths in both PT and BDPT using OpenPGL with RIS-based sampling and variance-aware adaptive alpha (Roadmap Rank 8, complete). Light subpaths currently use pure BSDF sampling for bounce directions. Guiding light subpath bounces toward high-contribution regions would reduce variance for difficult indirect caustics and SDS paths, cases where the eye side alone cannot efficiently find the light transport.

This is a natural extension of existing infrastructure. The RIS helpers in `PathTransportUtilities.h`, the guiding field API in `PathGuidingField.h`, and the PDF tracking in `BDPTVertex` are all generalized and ready for light-side use. No changes to the MIS weight algorithm are required since it automatically handles mixed-guiding asymmetry through the PDF ratio chain.

### What To Implement

#### 2A. Light subpath training data collection

During BDPT rendering iterations, record training samples from light subpath bounces in addition to eye subpath bounces. At each light subpath surface vertex, record position, scattered direction, PDF, and radiance contribution. Feed these into the guiding field's training pipeline alongside eye samples.

The key question is whether to use a single shared field or a separate light-space field. Start with the shared field (Option A below) since it is simplest and OpenPGL's incident radiance distribution is approximately reciprocal for diffuse-dominated transport. Evaluate whether a separate field is needed based on convergence results.

**Option A (Shared field, recommended first):** Use the same OpenPGL field for both eye and light subpaths. The incident radiance distribution learned from eye paths encodes "where does light come from at point P" which, by reciprocity, approximates "where should light scatter toward at point P." This is inexact for non-reciprocal materials but correct for the common case.

**Option B (Separate light field):** Train a second `PGLField` from light subpath samples recorded in reverse order (light-to-eye becomes eye-to-light for OpenPGL's incident-radiance semantics). More storage but handles asymmetric transport.

#### 2B. Guided light bounce sampling

In `GenerateLightSubpath`, after the first bounce from the light source, query the guiding field at each surface vertex and blend with BSDF sampling. Two strategies, mirroring the eye subpath implementation:

**One-sample MIS:** `pdfCombined = alpha * guidePdf + (1 - alpha) * bsdfPdf`. Use the same adaptive alpha scheme already implemented for eye subpaths.

**RIS (recommended):** Draw 2 candidates (one BSDF, one guide), resample proportional to the target function. Reuse `GuidingRISCandidate` and `GuidingRISSelectCandidate()` from `PathTransportUtilities.h`.

Update `BDPTVertex::pdfFwd` to store the guided PDF. The MIS weight computation in `MISWeight()` automatically picks this up through the ratio chain: `ri *= pdfRev[i] / pdfFwd[i]`.

#### 2C. Depth limiting and fallback

Apply a `maxLightGuidingDepth` parameter analogous to `maxGuidingDepth` on the eye side. Beyond this depth, fall back to pure BSDF sampling. Expose via parser and `RuntimeContext`.

#### 2D. Validation

- Verify MIS weights remain in [0, 1] for all (s,t) strategy pairs.
- Compare convergence with and without light guiding on caustic scenes.
- Use `CompletePathGuide` diagnostic infrastructure to analyze per-strategy energy shifts.

### Current RISE Files

- `src/Library/Shaders/BDPTIntegrator.cpp` lines ~1056-1607 (`GenerateLightSubpath`)
- `src/Library/Shaders/BDPTIntegrator.h` (add light guiding state)
- `src/Library/Utilities/PathGuidingField.h` / `.cpp` (query API already general-purpose)
- `src/Library/Utilities/PathTransportUtilities.h` lines ~320-450 (RIS helpers)
- `src/Library/Rendering/BDPTRasterizerBase.cpp` (training sample recording, model after eye-path logic)
- `src/Library/Utilities/CompletePathGuide.h` / `.cpp` (diagnostic infrastructure)

### Deliverables

- Light subpath guiding in `GenerateLightSubpath` with RIS or one-sample MIS.
- Training data collection from light subpath bounces.
- `maxLightGuidingDepth` parser parameter.
- Test scene demonstrating improved convergence on indirect caustics (e.g., light through glass onto diffuse surface).

### Acceptance Criteria

- Equal or lower variance compared to eye-only guiding at the same sample count on caustic-heavy scenes.
- No bias introduced: reference renders match within statistical tolerance.
- Surface-only non-caustic scenes do not regress.

---

## 3. Random-Walk Subsurface Scattering

### Why This Is Third

RISE already has excellent diffusion-profile BSSRDF support (Donner-Jensen, BioSpec, Burley normalized diffusion) with disk-projection sampling in both PT and BDPT (Roadmap Stage 4A/4B complete). Random-walk SSS (Chiang, Burley, SIGGRAPH 2016) handles thin geometry (ears, noses, fingers) and high-albedo materials where disk projection fails. It is the default in Arnold, RenderMan, Hyperion, and Cycles.

### What To Implement

#### 3A. Volumetric random walk inside mesh

At a BSSRDF entry point, instead of disk-projection sampling, trace a random walk inside the mesh geometry:

1. Refract into the surface using the material's IOR.
2. Sample free-flight distance from Beer's law: t = -log(xi) / sigma_t.
3. At each scatter point, apply the Henyey-Greenstein phase function (or isotropic) to choose a new direction.
4. When the walk exits the mesh (ray intersects the interior surface from inside), evaluate the BSSRDF exit conditions: compute the surface normal, apply Fresnel transmission, and connect to the exit point.
5. Apply MIS between the disk-projection method and random walk if both are available.

The `HomogeneousMedium` and `HenyeyGreensteinPhaseFunction` classes already provide the volumetric primitives. The main new work is the inside-mesh ray tracing loop and exit-point detection.

#### 3B. Scattering coefficient conversion

Convert diffusion-profile parameters (mean free path, albedo) to volumetric scattering coefficients (sigma_a, sigma_s) for the random walk. Use the searchable inversion from Christensen and Burley (2015) or the direct parameterization from the Chiang et al. (2016) paper.

#### 3C. Per-material selection

Add a per-material flag to choose between disk projection and random walk. Expose via parser. Default to random walk for new materials; preserve disk projection as fallback for compatibility.

### Current RISE Files

- `src/Library/Materials/SubSurfaceScatteringSPF.h` / `.cpp`
- `src/Library/Utilities/BSSRDFSampling.h` / `.cpp` (shared entry/exit utilities)
- `src/Library/Materials/HomogeneousMedium.h` / `.cpp` (volumetric primitives)
- `src/Library/Materials/HenyeyGreensteinPhaseFunction.h`
- `src/Library/Materials/BurleyNormalizedDiffusionProfile.h` / `.cpp`
- `src/Library/Shaders/PathTracingShaderOp.cpp` (PT BSSRDF path)
- `src/Library/Shaders/BDPTIntegrator.cpp` (BDPT BSSRDF path)

### Deliverables

- Random-walk BSSRDF sampling mode in PT and BDPT.
- Coefficient conversion from diffusion-profile parameters.
- Per-material mode selection via parser.
- Comparison scene: thin geometry (ear, finger) rendered with disk projection vs random walk.

### Acceptance Criteria

- Thin geometry renders without the light leaking artifacts that disk projection produces.
- Thick geometry matches disk projection within noise.
- Energy conservation validated on a unit sphere.

---

## 4. Light BVH For Many-Light Sampling

### Why This Is Fourth

RISE's alias table + spatial RIS handles moderate light counts well, but scales poorly to thousands of emitters. A light BVH (as in pbrt-v4) or light tree provides O(log N) importance-weighted selection that accounts for spatial proximity, orientation, and emitter power. This is already identified as Roadmap Stage 1C.

### What To Implement

#### 4A. Light BVH construction

Build a bounding volume hierarchy over all emitters (mesh luminaries and non-mesh lights unified under the shared light abstraction from Roadmap Stage 1A). Each node stores aggregate power, bounding box, and an orientation cone. Construction is a one-time cost at scene build time.

#### 4B. Importance-weighted traversal

At each shading point, traverse the light BVH from the root. At each internal node, compute an importance estimate based on:
- Aggregate power of the subtree.
- Distance from the shading point to the node's bounding box.
- Orientation compatibility (emitter normal cone vs direction to shading point).

Stochastically select left or right child proportional to importance. At a leaf, sample from the emitter. The product of selection probabilities along the traversal path gives the overall PDF.

#### 4C. MIS with BSDF sampling

The light BVH selection PDF replaces the alias-table PDF in the existing MIS framework. Ensure the PDF is evaluable for any emitter (needed for MIS when a BSDF-sampled ray hits an emitter).

### Current RISE Files

- `src/Library/Lights/LightSampler.h` / `.cpp` (current alias table + RIS)
- `src/Library/Managers/LightManager.cpp`
- `src/Library/Rendering/LuminaryManager.cpp`
- `src/Library/Shaders/PathTracingShaderOp.cpp` (NEE consumer)
- `src/Library/Shaders/BDPTIntegrator.cpp` (light subpath emission + connections)

### Deliverables

- Light BVH data structure with aggregate power, bounds, and orientation cones.
- Importance-weighted stochastic traversal with explicit PDF.
- Integration with PT NEE, BDPT light selection, and SMS light selection.
- Many-light test scene (100+ emitters) demonstrating improved convergence vs alias table.

### Acceptance Criteria

- Equal or lower noise on many-light scenes at fixed render time.
- Small light count scenes (< 10 emitters) do not regress.
- PDF is consistent: evaluable for any emitter at any shading point.

---

## 5. Hero Wavelength Spectral Sampling (HWSS)

### Why This Is Fifth

RISE already does spectral rendering with a sampled-spectrum approach, evaluating all wavelengths per path. HWSS (Wilkie et al., EGSR 2014) reduces this to a single hero wavelength driving all directional decisions, with 3 companions sharing the geometric path but carrying independent spectral throughput. Overhead drops to ~5-15% over RGB. At specular dispersive interfaces, secondary wavelengths are terminated, enabling correct dispersion without tracing separate rays per wavelength.

This is listed as a non-goal in the current roadmap ("deep spectral redesigns such as hero-wavelength sampling"), but the survey of the field strongly suggests it is the right long-term architecture. Both pbrt-v4 and Manuka (Avatar sequels) use HWSS as their spectral foundation. It composes well with null-scattering volumes (spectral tracking) and path guiding.

### What To Implement

#### 5A. SampledWavelengths state

Add a `SampledWavelengths` object carrying 4 wavelengths per path. Sample the hero uniformly from the visible range; place 3 companions at equidistant spectral offsets with wrap-around: lambda_i = lambda_min + mod(lambda_h - lambda_min + i * Delta, lambda_max - lambda_min).

#### 5B. Per-path wavelength propagation

Replace the current approach of evaluating all wavelengths at each vertex with hero-only directional decisions. BSDF sampling, NEE direction selection, and light selection all use the hero wavelength. Companion wavelengths evaluate throughput at the shared geometric direction.

#### 5C. Secondary wavelength termination at specular dispersive interfaces

When a path hits a perfectly specular interface with wavelength-dependent IOR (Cauchy/Sellmeier), terminate the 3 companion wavelengths. This degenerates to single-wavelength transport for the dispersive segment. For rough dispersive BSDFs, MIS over wavelengths remains valid.

#### 5D. XYZ conversion

Weight final `SampledSpectrum` contributions with CIE color matching functions evaluated at the sampled wavelengths, divided by wavelength PDFs.

### Current RISE Files

- `src/Library/Utilities/Color/SpectralPacket.h` (current spectral infrastructure)
- `src/Library/Shaders/PathTracingShaderOp.cpp` (`PerformOperationNM`)
- `src/Library/Shaders/BDPTIntegrator.cpp` (spectral BDPT paths)
- `src/Library/Materials/DielectricSPF.h` / `.cpp` (specular with IOR)

### Deliverables

- `SampledWavelengths` class with hero + 3 companions.
- Modified PT and BDPT spectral paths using hero-only directional decisions.
- Secondary wavelength termination at dispersive interfaces.
- Prism or glass dispersion scene demonstrating correct spectral splitting.

### Acceptance Criteria

- Non-dispersive scenes match current spectral output within noise.
- Dispersive scenes show correct rainbow separation through prisms.
- Overhead vs RGB path is under 15%.

---

## 6. Blue-Noise Screen-Space Error Distribution (ZSobol)

### Why This Is Sixth

RISE has Owen-scrambled Sobol, which provides excellent per-pixel stratification. Adding screen-space blue-noise ordering (Ahmed and Wonka, SIGGRAPH Asia 2020) distributes error spatially so that neighboring pixels have complementary sample patterns. This produces visually superior results at low sample counts and is especially beneficial when combined with OIDN denoising (denoisers perform better on blue-noise error than white-noise error). This is the default sampler in pbrt-v4.

### What To Implement

#### 6A. Morton-index pixel ordering

Order pixels via scrambled Morton (Z-curve) indices. Assign consecutive sub-sequences of the existing (0,2) Sobol sequence to adjacent pixels in Morton order. This automatically produces blue-noise diffusion of error across screen space.

#### 6B. Per-pixel scramble seed derivation

Derive Owen scramble seeds from the Morton-ordered pixel index rather than raw pixel coordinates. This is a small change to the existing `SobolSampler` seeding logic.

### Current RISE Files

- `src/Library/Utilities/SobolSampler.h` (current Owen-scrambled Sobol)
- `src/Library/Sampling/SobolSequence.h`
- `src/Library/Rendering/PixelBasedPelRasterizer.cpp` (pixel iteration order)
- `src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.cpp`

### Deliverables

- ZSobol sampler variant with Morton-index ordering.
- Parser option to select ZSobol vs standard Sobol.
- Visual comparison at 16-32 SPP showing blue-noise error distribution.

### Acceptance Criteria

- At low SPP (16-32), visible noise pattern is blue (high-frequency) rather than white.
- OIDN denoised output at 16-32 SPP shows measurably less error than with standard Sobol.
- High SPP (512+) convergence is not degraded.

---

## 7. Null-Scattering Volume Framework

### Why This Is Seventh

RISE has homogeneous and heterogeneous volume support but does not use the null-scattering path integral formulation (Miller, Georgiev, Jarosz, SIGGRAPH 2019). This formulation augments path space with fictitious null-scattering vertices, making the extinction transmittance along each edge trivially evaluable and enabling MIS between different free-flight sampling strategies. It unifies delta tracking, ratio tracking, and spectral tracking under a single framework. This is what pbrt-v4's `VolPathIntegrator` is built on.

This aligns with Roadmap Ranks 5-6 (participating media in PT and BDPT). The recommendation here is to build on the null-scattering formulation from the start rather than implementing ad-hoc tracking methods.

### What To Implement

#### 7A. Majorant grid with DDA traversal

Build a low-resolution grid where each cell stores the maximum density within its region. Decompose rays into segments via 3D-DDA traversal, using only the local majorant for delta/ratio tracking per segment. This dramatically reduces null collisions compared to a global majorant.

#### 7B. Null-scattering path integral

Implement the Miller et al. 2019 formulation: at each sampled point along a ray, make a real-vs-null scattering decision. Null events continue the ray without directional change. The full path PDF is expressible in closed form, enabling MIS between different sampling strategies (e.g., delta tracking vs equiangular sampling).

#### 7C. Ratio tracking for transmittance

Use ratio tracking (Novak et al. 2014) for shadow rays: accumulate multiplicative weights w = prod(1 - mu_t(x_i) / mu_bar) for continuous-valued transmittance estimates instead of delta tracking's binary 0/1. Much lower variance for optically thin media.

#### 7D. Spectral/decomposition tracking

For chromatic volumes with spectral rendering, make per-channel real-vs-null decisions at each interaction point. Combine with HWSS (Improvement 5) by selecting a hero wavelength for distance sampling and applying spectral MIS across the wavelength bundle.

#### 7E. Equiangular sampling for point lights

Implement Kulla and Fajardo (EGSR 2012) equiangular sampling: distribute samples along a ray proportional to the 1/r^2 geometry term toward a point light. Combine with exponential free-flight distance sampling via one-sample MIS.

### Current RISE Files

- `src/Library/Materials/HomogeneousMedium.h` / `.cpp`
- `src/Library/Materials/HeterogeneousMedium.h`
- `src/Library/Utilities/MediumTracking.h`
- `src/Library/Utilities/MediumTransport.h`
- `src/Library/Volume/` (volume data accessors)
- `src/Library/Shaders/PathTracingShaderOp.cpp` (PT medium integration point)

### Deliverables

- Majorant grid data structure with DDA traversal.
- Null-scattering path integral implementation in PT.
- Ratio tracking for shadow rays.
- Equiangular sampling for point lights in media.
- Fog scene and heterogeneous smoke scene.

### Acceptance Criteria

- Homogeneous media results match existing implementation within noise.
- Heterogeneous media converge without excessive null collisions.
- Shadow rays in optically thin media show measurably lower variance with ratio tracking.

---

## 8. Optimal And Correlation-Aware MIS Weights

### Why This Is Eighth

RISE uses the balance heuristic (and power heuristic) throughout PT and BDPT. Optimal MIS (Kondapaneni et al., SIGGRAPH 2019) derives variance-minimizing weights by solving a linear system of second moments. Crucially, optimal weights can be negative, breaking Veach's non-negativity constraint and exceeding his theoretical bounds (up to 9.6x lower error in direct illumination). Correlation-aware MIS (Grittmann et al., EG/CGF 2021) handles correlated samples in BDPT/VCM by down-weighting techniques with high prefix-sharing correlation. Both are zero runtime overhead: they just compute better weights.

### What To Implement

#### 8A. Optimal MIS for direct illumination

In PT NEE, replace the balance heuristic with optimal MIS weights computed from estimated second moments. This requires accumulating second-moment statistics during an initial training pass (similar to path guiding's training iterations). The weight computation itself is a small linear system solve.

#### 8B. Correlation-aware MIS for BDPT

In BDPT's `MISWeight()`, account for the correlation between techniques that share subpath prefixes. Techniques (s, t) and (s-1, t+1) share either the light or eye prefix up to the connection point. Down-weight the shared-prefix technique to avoid double-counting correlated contributions. This is a modification to the existing ratio chain walk, not a new data structure.

#### 8C. Efficiency-aware MIS (optional extension)

Weight techniques by their variance-to-cost ratio rather than variance alone. Cheap techniques (PT NEE) should be favored over expensive ones (high-s BDPT connections) when both produce similar variance. This is a further refinement of the optimal weights.

### Current RISE Files

- `src/Library/Shaders/PathTracingShaderOp.cpp` (PT MIS between NEE and BSDF)
- `src/Library/Shaders/BDPTIntegrator.cpp` `MISWeight()` function (BDPT ratio chain)

### Deliverables

- Optimal MIS weights for PT direct illumination.
- Correlation-aware MIS weights for BDPT connections.
- Comparison renders showing variance reduction on direct illumination and BDPT scenes.

### Acceptance Criteria

- Equal or lower variance on all test scenes (optimal weights are provably never worse than balance heuristic).
- No bias: converged results match balance-heuristic reference.
- BDPT correlation-aware weights do not introduce MIS weight explosions.

---

## 9. VCM (Vertex Connection And Merging)

### Why This Is Ninth

RISE already has both BDPT and photon mapping. VCM (Georgiev et al., SIGGRAPH Asia 2012) unifies them under MIS, subsuming PT, LT, BDPT, and photon mapping as special cases. The marginal implementation effort is lower than for most renderers since both halves already exist. VCM provides robust caustic rendering without relying solely on SMS.

This is listed as a non-goal in the current roadmap, but it is worth reconsidering once the higher-priority items are complete.

### What To Implement

#### 9A. Area-measure photon density estimation PDF

Reformulate photon density estimation as a bidirectional sampling technique with an explicit PDF in area-product measure. This is the key insight from VCM: photon merging becomes just another technique in the MIS framework.

#### 9B. Recursive MIS weight computation

Track three running values per vertex (dVCM, dVC, dVM) as described in Georgiev's SmallVCM. These accumulate the information needed to compute MIS weights for all connection and merging strategies.

#### 9C. Integration with existing BDPT

Extend the existing `MISWeight()` to include the vertex merging PDF terms alongside the current vertex connection terms.

### Reference Implementation

SmallVCM (smallvcm.com) is a ~2000-line C++ educational implementation covering PT, LT, PPM, BPM, BDPT, and VCM. Use as the primary reference.

### Current RISE Files

- `src/Library/Shaders/BDPTIntegrator.h` / `.cpp` (BDPT core)
- `src/Library/PhotonMapping/` (photon map infrastructure)

### Deliverables

- VCM integrator extending BDPT with vertex merging.
- Caustic scene comparison: VCM vs BDPT vs photon mapping vs SMS.

### Acceptance Criteria

- Caustic scenes converge faster than BDPT alone.
- Non-caustic scenes perform comparably to BDPT (merging adds overhead but MIS should prevent regression).

---

## 10. Hair/Fiber BSDF (Chiang Et Al. 2016)

### Why This Is Tenth

RISE has no dedicated hair or fiber BSDF. The Chiang et al. model (Disney/Hyperion, also used in pbrt-v4 and Cycles) is the production standard. It builds on Marschner et al. (2003) with near-field formulation, a single residual lobe for all higher-order internal reflections, and logistic distributions for azimuthal roughness with closed-form CDF. This is a self-contained material addition.

### What To Implement

#### 10A. Longitudinal scattering (M lobes)

Implement the R, TT, TRT, and residual longitudinal scattering functions using shifted Gaussian distributions parameterized by roughness.

#### 10B. Azimuthal scattering (N lobes)

Implement azimuthal scattering using logistic distributions with closed-form CDF for importance sampling.

#### 10C. Near-field formulation

Use the true fiber offset (h parameter) rather than width-averaged far-field approximation. This matters for close-up rendering.

#### 10D. Importance sampling

Sample the combined longitudinal x azimuthal distribution. The logistic CDF enables exact inversion for the azimuthal component.

### Current RISE Files

- `src/Library/Interfaces/ISPF.h` (interface to implement)
- `src/Library/Interfaces/IBRDF.h`
- `src/Library/Materials/` (add new files here)

### Deliverables

- Hair BSDF with R, TT, TRT, and residual lobes.
- Importance sampling with logistic azimuthal inversion.
- Hair rendering test scene (straight and curved fibers under directional light).
- Parser support for hair material parameters.

### Acceptance Criteria

- White furnace test passes for the hair BSDF.
- Importance sampling PDF matches BSDF evaluation (chi-squared test).
- Visual comparison with pbrt-v4 hair reference images.

---

## 11. Jakob-Hanika Sigmoid Spectral Uplifting

### Why This Is Eleventh

The mapping from RGB to spectral reflectance is underdetermined. The Jakob and Hanika (CGF/Eurographics 2019) sigmoid method parameterizes spectra as S(lambda) = sigmoid(c0 * lambda^2 + c1 * lambda + c2), intrinsically bounded in [0,1] and smooth. A precomputed 3D lookup table maps RGB to three coefficients. Evaluation costs ~6 FLOPs per wavelength. Zero round-trip error on the full sRGB gamut. This is the standard used by both pbrt-v4 and Mitsuba 3.

This is most valuable in combination with HWSS (Improvement 5) since the sigmoid coefficients replace RGB texels directly (three floats for three floats).

### What To Implement

#### 11A. Precomputed coefficient table

Generate or import the 3D LUT mapping sRGB to sigmoid coefficients. The tables are publicly available from Mitsuba 3 and pbrt-v4.

#### 11B. Texture load-time conversion

At texture load time, convert RGB texels to sigmoid coefficients. Store as three floats per texel (same memory footprint as RGB).

#### 11C. Runtime evaluation

At shading time, evaluate S(lambda) = sigmoid(c0 * lambda^2 + c1 * lambda + c2) at the sampled wavelengths. ~6 FLOPs per wavelength per texel lookup.

### Current RISE Files

- `src/Library/Utilities/Color/SpectralPacket.h` (spectral evaluation infrastructure)
- `src/Library/Texturing/` (texture loading and evaluation)

### Deliverables

- Sigmoid coefficient LUT (imported or generated).
- Texture conversion pipeline.
- Runtime spectral evaluation from sigmoid coefficients.

### Acceptance Criteria

- Round-trip error: RGB -> sigmoid -> spectrum -> RGB produces the original RGB within floating-point tolerance.
- No visible artifacts under non-D65 illumination.
- Overhead vs direct RGB evaluation is negligible.

---

## Explicit Non-Goals

These are interesting but should not displace the ranked items above:

- **Neural importance sampling:** OpenPGL path guiding with RIS is already the practical production answer. NIS requires dedicated GPU inference and offers worse performance-to-overhead ratios.
- **NeRF / 3D Gaussian Splatting:** Scene acquisition tools, not rendering improvements.
- **Polarization:** 1.5-2x overhead, invisible to humans in standard rendering. Only if a specific scientific visualization need arises.
- **Full MLT overhaul (RJMCMC, delayed rejection):** PSSMLT is already implemented. Marginal gains do not justify the complexity for most scenes.
- **GPU / wavefront execution:** Feature gaps come first, not execution-model gaps.
- **Neural BRDFs / neural radiance caching:** Niche applications (measured material compression, interactive preview). Not a priority for reference-quality offline rendering.

---

## Context On Prior Work

Several items here build on transport work already completed or planned before this document was written:

| Item | Prior status |
|------|-------------|
| Light BVH (Rank 4) | Planned (Stage 1C of prior roadmap) |
| Null-scattering volumes (Rank 7) | Planned (Ranks 5-6 of prior roadmap) |
| Light subpath guiding (Rank 2) | New scope (Stage 8C was deferred) |
| Random-walk SSS (Rank 3) | Planned (Stage 4C of prior roadmap) |

The following validation and correctness work from the prior roadmap remains relevant before starting items that depend on light sampling or spectral correctness:
- Validation harness: focused scenes for many-light, caustic, BSSRDF, and fog transport.
- Light sampling unification: shared sampled-light abstraction across PT, BDPT, and SMS.
- Spectral/SMS correctness fixes: non-mesh light spectral path, SMS visibility, unbiased RR in PT.
- Production stability controls: per-type bounce limits, direct/indirect clamps, glossy filtering.
