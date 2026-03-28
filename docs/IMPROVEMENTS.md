# RISE Improvement Roadmap

A prioritized list of rendering advances (2010-2025) applicable to RISE, ordered by ROI (impact per implementation effort). Each item is structured as an independent sub-task with context, references, and implementation notes specific to RISE's existing architecture.

## Current RISE Baseline

| Feature | Status | Notes |
|---------|--------|-------|
| Spectral rendering | Full | Wavelength-by-wavelength evaluation, XYZ conversion |
| BDPT | Full | All (s,t) strategies, balance heuristic MIS |
| MLT | Full | PSSMLT with progressive rounds |
| SMS | Full | Newton manifold solving (Zeltner 2020) |
| Owen-scrambled Sobol | Full | Hash-based Owen scrambling, dimension stratification |
| Photon mapping | Full | Caustic, global, translucent, shadow (RGB + spectral) |
| Microfacet BRDF | Partial | CookTorrance/GGX, no VNDF, no multiscattering |
| BSSRDF | Full | BioSpec, Donner-Jensen, Burley dipole profiles |
| Volumes | Limited | Direct volume rendering only, no participating media in PT |
| Denoising | None | No OIDN, no AOV output |
| Path guiding | None | No learned sampling distributions |
| VCM | None | No vertex merging |
| Light BVH | None | Power-weighted selection only |
| Hair models | None | No strand-based BSDFs |

---

## Tier 1: High Impact, Moderate Effort

### 1.1 OIDN Denoising Integration

**Impact**: Orders-of-magnitude perceived quality improvement at low sample counts. Turns 16-32 spp BDPT into visually acceptable results that would otherwise require 1000+ spp.

**What it is**: Intel Open Image Denoise is a U-Net neural denoiser that takes a noisy beauty buffer plus auxiliary AOVs (albedo, normals) and produces a clean image. Won a 2025 Sci-Tech Academy Award. OIDN 2.x supports Metal on macOS.

**Key references**:
- Intel OIDN: https://www.openimagedenoise.org/
- Bako et al., "Kernel-Predicting Convolutional Networks for Denoising Monte Carlo Renderings", SIGGRAPH 2017

**Implementation plan**:

1. **AOV output infrastructure**: Add first-hit albedo and first-hit world-space normal buffers alongside the beauty buffer. These are computed at the first non-specular hit along each camera path:
   - Albedo: material reflectance at the first diffuse/glossy vertex, no lighting applied
   - Normal: world-space shading normal at that vertex
   - Both should be written as HDR float buffers matching the beauty resolution

2. **OIDN library integration**: Link against OIDN 2.x. The API is straightforward:
   - Create an `oidn::DeviceRef` (CPU or Metal/GPU)
   - Create an `oidn::FilterRef` with type `"RT"` (ray tracing)
   - Set input images: `"color"` (beauty), `"albedo"`, `"normal"`
   - Set output image buffer
   - Execute filter

3. **Pipeline hookup**: After the rasterizer finishes all samples, run the OIDN filter as a post-process before writing the final image. Optionally write both denoised and raw outputs for comparison.

4. **Quality considerations**:
   - At 1-32 spp: denoising is overwhelmingly beneficial
   - At 64-256 spp: significant help for indirect illumination
   - At 512+ spp: diminishing returns, risk of overblurring
   - Denoisers can remove fine projected patterns and cause color shifts in extreme HDR

**RISE-specific notes**:
- AOV buffers need to work with both `BDPTSpectralRasterizer` and `BDPTPelRasterizer`
- The first-hit data should be extracted from the eye subpath vertex array at the first non-delta vertex
- Consider adding a scene-level toggle (`denoise true/false`) in the `.RISEscene` parser
- Output both raw and denoised images for validation

**Estimated scope**: Small-medium. AOV plumbing through the rasterizer is the bulk of the work; OIDN API itself is minimal.

---

### 1.2 VNDF Sampling (Visible Normal Distribution Function)

**Impact**: Eliminates wasted back-facing microfacet samples and grazing-angle fireflies in all GGX materials. Pure variance reduction with no visual tradeoffs.

**What it is**: Instead of sampling the full NDF D(m), sample only the visible portion D_v(m) = G1(o,m) * max(dot(o,m),0) * D(m) / (4 * dot(o,n)). This ensures every sampled micronormal is visible from the outgoing direction, dramatically reducing variance at grazing angles.

**Key references**:
- Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018 — original exact sampling via ellipsoid projection
- Dupuy & Benyoub, "Sampling Visible GGX Normals with Spherical Caps", CGF/EGSR 2023 — simplified spherical cap method, ~20 lines of C++

**Implementation plan**:

1. **Identify current NDF sampling**: Locate the microfacet sampling routine in `CookTorranceBRDF` / `CookTorranceSPF`. It currently samples the full NDF (likely GGX/Beckmann) and discards back-facing samples.

2. **Replace with VNDF sampling**: Implement the Dupuy-Benyoub spherical cap method:
   - Transform the view direction to the hemisphere configuration (stretch by roughness)
   - Sample a spherical cap around the reflected view direction
   - Transform back to the ellipsoidal configuration
   - The PDF is `D_v(m)` which already accounts for masking

3. **Update PDF computation**: The VNDF PDF replaces the old `D(m) * cos(theta_m)` PDF. This affects MIS weight computation in BDPT — ensure the PDF returned by the BRDF sampling matches what's used in the MIS weight calculation.

4. **Validation**: White furnace test — a rough metallic sphere in a uniform environment should integrate to 1.0 (modulo multiscattering loss, which is separate). Compare variance at grazing angles before/after.

**RISE-specific notes**:
- `CookTorranceBRDF.h` and `CookTorranceSPF.h` are the primary files to modify
- The BDPT integrator's MIS weight computation reads BRDF PDFs — ensure consistency
- This change is purely in the sampling/PDF; the BRDF evaluation function itself doesn't change
- Also check `PolishedBRDF.h` if it wraps CookTorrance sampling internally

**Estimated scope**: Small. ~20-30 lines of core sampling code plus PDF updates.

---

### 1.3 Kulla-Conty Multiscattering Energy Compensation

**Impact**: Fixes up to 60% energy loss at high GGX roughness. Correct energy conservation across all CookTorrance materials. Visible as too-dark rough metals and too-dark rough dielectrics.

**What it is**: Single-scatter microfacet BRDFs only model the first bounce off the microsurface. Light that bounces multiple times between microfacets is lost. Kulla-Conty adds a compensation lobe that accounts for this missing energy.

**Key references**:
- Kulla & Conty, "Revisiting Physically Based Shading at Imageworks", SIGGRAPH 2017 Course
- Heitz et al., "Multiple-Scattering Microfacet BSDFs with the Smith Model", SIGGRAPH 2016 — ground truth random walk
- Turquin, "Practical Multiple Scattering Compensation for Microfacet Models", ILM Tech Report 2019 — simplest approximation

**Implementation plan**:

1. **Precompute directional albedo LUT**: A 2D table `E_ss(cos_theta, roughness)` storing the hemispherical integral of the single-scatter BRDF (with F=1) for each (cosTheta, roughness) pair. This can be computed offline via Monte Carlo integration and baked into a header as a static array. Resolution: 32x32 or 64x64 is sufficient with bilinear interpolation.

2. **Precompute average albedo**: A 1D table `E_avg(roughness)` = cosine-weighted integral of `E_ss` over the hemisphere. Used for the Fresnel averaging term.

3. **Add compensation lobe** (Kulla-Conty full form):
   ```
   f_ms(o, i) = (1 - E_ss(cos_o)) * (1 - E_ss(cos_i)) / (pi * (1 - E_avg))
   ```
   This is added to the single-scatter BRDF evaluation. For conductors with colored Fresnel:
   ```
   F_avg = E_avg  (precomputed)
   F_ms = F_avg^2 * E_avg / (1 - F_avg * (1 - E_avg))
   ```
   Multiply `f_ms` by `F_ms` for colored metals.

4. **Alternative — Turquin shortcut**: Simply scale the single-scatter lobe by `1/E_ss(cos_o)`. Loses reciprocity but trivial to implement. Good enough for most visual purposes.

5. **Sampling**: The compensation lobe is nearly Lambertian — sample it with cosine-weighted hemisphere sampling. Use MIS between the original VNDF-sampled specular lobe and the cosine-sampled compensation lobe based on their relative energies.

**RISE-specific notes**:
- Applies to `CookTorranceBRDF`, `CookTorranceSPF`, and any material using microfacet evaluation
- The LUT can be stored as a static array in a new header (e.g., `MicrofacetEnergyLUT.h`)
- For spectral rendering: the LUT is computed with F=1, so it's achromatic. Fresnel coloring is applied at shading time using the spectral Fresnel value
- Implement after VNDF sampling (1.2) since the LUT should be computed using the correct sampling

**Estimated scope**: Small-medium. LUT generation is offline; runtime is a table lookup + one extra lobe evaluation per shading point.

---

### 1.4 Path Guiding via Intel OpenPGL

**Impact**: Dramatically improves convergence for complex indirect illumination — the exact scenarios where BDPT already shines but still struggles (deep interiors, caustics via glossy chains, light leaking through small openings).

**What it is**: A spatial-directional cache of learned incident radiance distributions. During rendering, radiance samples are collected and fed to the guiding library, which builds a spatial tree with VMM (von Mises-Fisher mixture model) directional distributions at each leaf. At shading time, the learned distribution is queried and combined with the BSDF via MIS for directional sampling.

**Key references**:
- Muller, Gross, Novak, "Practical Path Guiding for Efficient Light-Transport Simulation", EGSR 2017 — SD-tree foundation
- Ruppert, Herholz, Lensch, "Robust Fitting of Parallax-Aware Mixtures for Path Guiding", SIGGRAPH 2020 — VMM approach
- Intel OpenPGL: https://github.com/OpenPathGuidingLibrary/openpgl
- Herholz et al., "Product Importance Sampling for Light Transport Path Guiding", EGSR 2016

**Implementation plan**:

1. **Integrate OpenPGL library**: Build and link OpenPGL. It provides a C API (`openpgl.h`) with these core concepts:
   - `PGLField`: the spatial-directional guiding structure
   - `PGLSurfaceSamplingDistribution`: queried at each shading point
   - `PGLSampleStorage`: collects radiance samples for training

2. **Training phase**: After each rendering iteration (or batch of samples):
   - For each path vertex, create a `PGLSampleData` with position, direction, distance, PDF, and contribution
   - Feed all samples to the `PGLField` via `pglFieldUpdate()`
   - The field rebuilds its spatial tree and VMM fits

3. **Guiding phase**: At each non-specular path vertex:
   - Query the field: `pglFieldInitSurfaceSamplingDistribution(field, position, &distribution)`
   - Sample a direction from the guiding distribution with probability `p_guide`
   - Sample a direction from the BSDF with probability `1 - p_guide`
   - Compute the combined PDF via one-sample MIS: `pdf = p_guide * pdf_guide + (1 - p_guide) * pdf_bsdf`
   - Use the combined PDF for MIS weight computation in BDPT

4. **Multi-pass rendering**: Run N training iterations (typically 4-8) with progressive sample counts, then switch to final rendering using the converged guiding field. OpenPGL supports this workflow natively.

5. **BDPT integration specifics**: Guiding can be applied to the eye subpath sampling. Light subpath sampling can also be guided but is less critical since lights already provide good initial directions.

**RISE-specific notes**:
- The BDPT integrator's eye path sampling currently uses BSDF sampling exclusively — this is where guiding inserts
- The spectral pipeline means radiance samples carry spectral values; OpenPGL works with scalar (luminance) radiance — convert via CIE Y weight
- The Sobol sampler's dimension management needs to account for the additional random numbers consumed by the guiding MIS decision
- Consider adding `pathguiding true/false` as a scene-level toggle
- Multi-pass training naturally fits RISE's progressive rendering model

**Estimated scope**: Medium-large. OpenPGL does the heavy lifting, but integrating it into BDPT's subpath generation and MIS machinery requires careful plumbing.

---

## Tier 2: High Impact, Higher Effort

### 2.1 Hero Wavelength Spectral Sampling (HWSS)

**Impact**: ~4x spectral efficiency. RISE currently evaluates each wavelength as an independent BDPT path. HWSS shares the geometric path across 4 wavelengths, getting 4x the spectral information per ray at ~5-15% overhead.

**What it is**: For each path, sample one hero wavelength uniformly from [lambda_min, lambda_max]. Place 3 companion wavelengths at equidistant spectral offsets with wrap-around. All directional decisions use the hero wavelength exclusively. Companions share the geometric path but carry independent spectral throughput, combined via balance-heuristic MIS over wavelengths.

**Key references**:
- Wilkie, Nawaz, Droske, Weidlich, Hanika, "Hero Wavelength Spectral Sampling", EGSR 2014
- pbrt-v4 implementation (Pharr, Jakob, Humphreys, 2023) — `SampledWavelengths` class
- Meng et al., "Physically Meaningful Rendering using Tristimulus Colours", EGSR 2015 — proves RGB transport introduces systematic color shifts

**Implementation plan**:

1. **Define `SampledWavelengths` type**: A struct carrying N=4 wavelengths and their PDFs. The hero is sampled uniformly; companions are deterministic offsets:
   ```
   lambda[0] = lambda_min + xi * (lambda_max - lambda_min)  // hero
   lambda[i] = lambda_min + fmod(lambda[0] - lambda_min + i * delta, lambda_max - lambda_min)  // i=1,2,3
   ```
   where `delta = (lambda_max - lambda_min) / N`.

2. **Replace per-wavelength path tracing**: Currently `BDPTSpectralRasterizer` runs the full BDPT integrator once per wavelength sample. With HWSS, run BDPT once using the hero wavelength for all directional decisions, but evaluate spectral quantities (BSDF values, emission, transmittance) at all 4 wavelengths simultaneously.

3. **Modify spectral evaluation**: All functions that return spectral values (BSDF::Evaluate, Light::GetRadiance, etc.) now take `SampledWavelengths` and return `SampledSpectrum` (array of 4 floats) instead of a single scalar per wavelength call.

4. **Handle dispersive interfaces**: At specular interfaces with wavelength-dependent IOR (Cauchy/Sellmeier), companion wavelengths would refract differently. Solution: terminate secondary wavelengths at such interfaces — set their throughput to zero. The hero continues alone. This is `lambda.TerminateSecondary()` in pbrt-v4.

5. **XYZ conversion**: Weight each wavelength's contribution by the CIE color matching functions evaluated at that wavelength, divided by the wavelength's PDF (uniform = 1/(lambda_max - lambda_min) for each).

**RISE-specific notes**:
- This is the deepest refactor in the list — it changes the fundamental color type from scalar-per-wavelength to `SampledSpectrum[4]` throughout
- The spectral rasterizer's outer loop over wavelengths becomes an inner loop over 4 packed wavelengths within a single path
- All BSDF interfaces need wavelength-array versions of their evaluate/sample/pdf methods
- The IOR stack (`IORStack`) needs to handle per-wavelength IOR for dispersive materials
- Benefits compound with all other improvements (VNDF, Kulla-Conty, path guiding all become ~4x more spectrally efficient)
- Consider doing this before or alongside other material improvements to avoid double-refactoring

**Estimated scope**: Large. Touches every spectral evaluation path. Plan for incremental migration: start with forward path tracing, validate, then extend to full BDPT.

---

### 2.2 Null-Scattering Volumetric Path Tracing

**Impact**: Enables physically correct heterogeneous participating media (fog, smoke, clouds, atmospheric scattering) integrated into the BDPT pipeline. Currently RISE can only render volumes via direct volume rendering, not as part of light transport.

**What it is**: The null-scattering framework (Miller, Georgiev, Jarosz, SIGGRAPH 2019) augments the volume with fictitious null-scattering events so that the total extinction is a constant majorant. This makes transmittance along path edges trivially evaluable and enables MIS between different free-flight sampling strategies.

**Key references**:
- Miller, Georgiev, Jarosz, "A Null-Scattering Path Integral Formulation of Light Transport", SIGGRAPH 2019
- Novak, Selle, Jarosz, "Residual Ratio Tracking for Estimating Attenuation in Participating Media", SIGGRAPH Asia 2014
- Kutz, Habel, Li, Novak, "Spectral and Decomposition Tracking for Rendering Heterogeneous Volumes", SIGGRAPH 2017
- Kulla & Fajardo, "Importance Sampling Techniques for Path Tracing in Participating Media", EGSR 2012 — equiangular sampling
- Misso et al., "Progressive Null-Tracking for Volumetric Rendering", SIGGRAPH 2023

**Implementation plan**:

1. **Medium representation**: Define a `Medium` interface with:
   - `SampleDistance(ray, majorant, rng)` → distance, weight
   - `GetExtinction(point)` → sigma_t at a point
   - `GetScatteringAlbedo(point)` → sigma_s / sigma_t
   - `GetMajorant(ray_segment)` → upper bound on sigma_t along a ray segment

2. **Majorant grid**: A low-resolution 3D grid where each cell stores the maximum extinction in its region. Rays are decomposed into segments via 3D-DDA traversal; each segment uses only the local majorant for tracking. This dramatically reduces null collisions compared to a global majorant.

3. **Delta tracking**: For distance sampling within a segment:
   - Sample exponential distance `t = -log(1 - xi) / majorant`
   - At sampled point, real collision probability = `sigma_t(x) / majorant`
   - If real collision: scatter or absorb
   - If null collision: continue tracking

4. **Ratio tracking**: For transmittance estimation (shadow rays):
   - Accumulate multiplicative weights `w *= (1 - sigma_t(x_i) / majorant)` at each sampled point
   - Produces continuous-valued estimates (much lower variance than delta tracking's binary 0/1)

5. **Phase functions**: Implement Henyey-Greenstein (standard) and isotropic as starting phase functions.

6. **BDPT integration**: Volume vertices become additional vertex types in the subpath. MIS weights need to account for volume PDFs (free-flight distance PDF × phase function PDF vs surface BSDF PDF).

7. **Equiangular sampling**: For point/spot lights in media, sample distances proportional to 1/r^2 geometry toward the light. Combine with exponential sampling via one-sample MIS.

**RISE-specific notes**:
- RISE's existing `DirectVolumeRenderingShader` with volume accessors (NNB, trilinear, tricubic) can serve as the density source
- The BSP/octree acceleration structure needs to handle rays that may scatter mid-traversal
- Spectral volumes: with HWSS (2.1), use the hero wavelength for distance sampling and apply spectral MIS across the wavelength bundle
- NanoVDB support would be a natural follow-on for production volume data (OpenVDB → NanoVDB)

**Estimated scope**: Large. New subsystem with deep integration into the path tracing loop.

---

### 2.3 Vertex Connection and Merging (VCM)

**Impact**: Unifies BDPT and photon mapping under MIS. Enables robust caustic rendering by combining vertex connections (BDPT) with vertex merging (photon density estimation) — each handles different path types efficiently.

**What it is**: VCM reformulates photon density estimation as a bidirectional sampling technique with an explicit PDF in area-product measure. Standard MIS weights then combine BDPT connections and photon merging, automatically allocating contribution to whichever technique has lower variance for each path type.

**Key references**:
- Georgiev, Krivanek, Davidovic, Slusallek, "Light Transport Simulation with Vertex Connection and Merging", SIGGRAPH Asia 2012
- Hachisuka, Pantaleoni, Jensen, "A Path Space Extension for Robust Light Transport Simulation", SIGGRAPH Asia 2012 — independent derivation (UPS)
- SmallVCM (Georgiev): smallvcm.com — ~2000-line educational C++ implementation

**Implementation plan**:

1. **Study SmallVCM**: The ~2000-line implementation demonstrates the recursive MIS weight computation tracking three running values per vertex: `dVCM`, `dVC`, `dVM`.

2. **Extend BDPT subpath storage**: Light subpath vertices need to be stored (as in photon mapping) for the merging pass. A spatial hash or kd-tree enables efficient radius-based neighbor queries.

3. **Add vertex merging pass**: After generating eye and light subpaths, for each eye vertex, query nearby light vertices within merge radius `r`. Each merge contributes with a kernel weight and MIS weight that accounts for the merging PDF.

4. **Unified MIS weights**: Replace the current balance heuristic weights with VCM's extended weights that include the merging PDF term. The key insight: merging PDF ∝ 1/(pi*r^2) in area measure, which converts photon density estimation into a proper sampling technique.

5. **Merge radius scheduling**: The merge radius `r` should decrease with sample count as `r ∝ 1/sqrt(N)` to maintain consistency. VCM converges to BDPT as `r → 0`.

**RISE-specific notes**:
- RISE already has full BDPT subpath machinery and photon mapping infrastructure — this is a natural unification
- The existing photon map spatial structures (`PointSetOctree`) may be reusable for the vertex merging queries
- `dVCM/dVC/dVM` tracking replaces the current MIS weight computation in `BDPTIntegrator`
- VCM complements SMS: SMS handles single specular chains via Newton iteration; VCM handles multi-bounce caustics via density estimation
- Start with the SmallVCM weight formulas mapped onto RISE's existing BDPT vertex structures

**Estimated scope**: Medium-large. The MIS weight reformulation is the core complexity; spatial queries for merging can reuse existing infrastructure.

---

## Tier 3: Valuable but More Specialized

### 3.1 Random Walk Subsurface Scattering

**Impact**: Correct SSS for thin/curved geometry (ears, nostrils, fingers) and heterogeneous media. Diffusion-based profiles (BioSpec, Donner-Jensen, Burley) assume semi-infinite flat slabs.

**What it is**: Instead of evaluating a diffusion profile at a sampled surface point, perform a volumetric random walk inside the mesh until the path exits. Naturally handles arbitrary geometry thickness, curvature, and heterogeneous scattering parameters.

**Key references**:
- Chiang, Tappan, Burley, "Practical and Controllable Subsurface Scattering for Production Path Tracing", SIGGRAPH 2016
- King, Kulla, Conty, Fajardo, "BSSRDF Importance Sampling", SIGGRAPH 2013 Talks — importance sampling for diffusion profiles
- Christensen & Burley, "Approximate Reflectance Profiles for Efficient Subsurface Scattering", Pixar 2015 — Burley normalized diffusion

**Implementation plan**:

1. **Interior volume representation**: For SSS objects, define scattering parameters (sigma_s, sigma_a, phase function g) either uniformly or from a 3D texture. The sum sigma_t = sigma_s + sigma_a defines the mean free path.

2. **Random walk loop**: From the entry point on the surface:
   - Sample a free-flight distance: `t = -log(1-xi) / sigma_t`
   - If the new point is still inside the mesh: scatter (sample new direction from phase function) or absorb
   - If the new point exits the mesh: ray-intersect from inside to find the exit point. This is the BSSRDF sample point.
   - Apply Fresnel transmission at entry and exit

3. **Inside/outside test**: Requires watertight meshes. Use the existing ray-object intersection to detect exit points (intersect from inside).

4. **Integration with BDPT**: The random walk SSS vertex replaces the diffusion-profile-based BSSRDF sampling. The exit point becomes a new eye subpath vertex with its own position and normal.

**RISE-specific notes**:
- BioSpec's tissue-layer model provides physically measured sigma_s, sigma_a, and g values per layer — these directly parameterize the random walk
- The existing `SubSurfaceScatteringShaderOp` infrastructure handles the two-pass (sample generation + evaluation) pattern — random walk replaces the diffusion evaluation
- For spectral rendering, scattering parameters are wavelength-dependent — HWSS naturally handles this
- Consider keeping diffusion profiles as a fast approximation option alongside random walk

**Estimated scope**: Medium. Core random walk is simple; integration with the existing BSSRDF framework and watertight mesh requirements add complexity.

---

### 3.2 Many-Light Sampling (Light BVH)

**Impact**: Makes scenes with hundreds or thousands of emitters tractable. Current power-weighted light selection ignores spatial proximity and orientation.

**What it is**: A BVH over light sources where each internal node stores aggregate emission bounds. At each shading point, the tree is traversed stochastically, selecting lights proportional to their estimated contribution (accounting for distance, orientation, and emission power).

**Key references**:
- Estevez & Kulla, "Importance Sampling of Many Lights with Adaptive Tree Splitting", SIGGRAPH 2018
- pbrt-v4 light BVH implementation (Pharr, Jakob, Humphreys, 2023)
- Conty & Kulla, "Importance Sampling of Many Lights on the GPU", Chapter in Ray Tracing Gems II, 2021

**Implementation plan**:

1. **Build light BVH**: Construct a binary tree over all emissive primitives (mesh light triangles + analytic lights). Each node stores:
   - Bounding box
   - Total power
   - Bounding cone of emission directions
   - Representative position (centroid or power-weighted center)

2. **Importance-based traversal**: At each shading point, traverse the tree from root. At each internal node, compute the estimated contribution of each child (power × geometric factor × orientation factor) and stochastically choose one. At the leaf, select a specific emitter.

3. **PDF computation**: The selection PDF is the product of all traversal probabilities along the chosen path through the tree.

4. **MIS with BSDF sampling**: The light BVH sample has a well-defined PDF, so standard MIS with BSDF sampling works unchanged.

**RISE-specific notes**:
- `LuminaryManager` currently handles mesh light sampling — extend it with a BVH structure
- For scenes with few lights (< ~16), the overhead isn't worth it — add a threshold to fall back to uniform/power-weighted
- The photon tracers' light selection (proportional to radiant exitance) could also benefit

**Estimated scope**: Medium. Tree construction is straightforward; the importance estimation heuristic at each node needs tuning.

---

### 3.3 Optimal and Correlation-Aware MIS Weights

**Impact**: Up to ~10x lower error in direct illumination (optimal MIS) and measurably lower error in BDPT (correlation-aware MIS) at zero runtime cost — just smarter weight computation.

**What it is**: The balance heuristic is good but not optimal. Kondapaneni et al. derive variance-minimizing weights by solving a linear system of second moments. These optimal weights can be negative, breaking Veach's non-negativity constraint. Grittmann et al. extend this to handle correlated samples in BDPT by down-weighting techniques with high prefix-sharing.

**Key references**:
- Kondapaneni, Vevoda, Grittmann, Skigeorgiev, Slusallek, Krivanek, "Optimal Multiple Importance Sampling", SIGGRAPH 2019
- Grittmann, Georgiev, Slusallek, "Correlation-Aware Multiple Importance Sampling for Bidirectional Rendering Algorithms", EG/CGF 2021
- Grittmann et al., "Efficiency-Aware Multiple Importance Sampling", SIGGRAPH 2022

**Implementation plan**:

1. **Optimal MIS for direct illumination**: Replace the balance heuristic for NEE (next event estimation) with optimal weights computed from second-moment estimates. Requires a short learning phase to estimate the moment matrix, then the weights are fixed for the remainder of the render.

2. **Correlation-aware MIS for BDPT**: In BDPT, different (s,t) techniques share path prefixes, creating correlation. Estimate correlation coefficients between technique pairs and adjust MIS weights to down-weight highly correlated techniques.

3. **Efficiency-aware variant**: Weight techniques by their variance/cost ratio rather than just variance.

**RISE-specific notes**:
- RISE's BDPT uses the balance heuristic throughout — this is a targeted replacement
- The MIS weight computation in `BDPTIntegrator` is centralized, making this a relatively localized change
- Negative weights can introduce bias in pixel values (negative contributions) — may need clamping for display
- Consider implementing correlation-aware first as it's specific to BDPT and doesn't require negative weights

**Estimated scope**: Small-medium for correlation-aware; medium for full optimal MIS.

---

### 3.4 Layered Materials (Belcour 2018) and Clearcoat

**Impact**: Physically correct layered material appearance (coated metals, lacquered wood, car paint) without per-material precomputation.

**What it is**: Belcour's adding-doubling method decomposes layered transport into atomic operators acting on directional statistics (energy, mean direction, variance). Layers are composed analytically. Alternatively, a simpler explicit clearcoat as a second GGX specular lobe handles the most common case.

**Key references**:
- Belcour, "Efficient Rendering of Layered Materials using an Atomic Decomposition with Statistical Operators", SIGGRAPH 2018
- Burley, "Physically Based Shading at Disney", SIGGRAPH 2012 Course — clearcoat parameter
- OpenPBR Surface v1.1, Academy Software Foundation, 2024

**Implementation plan**:

1. **Simple clearcoat** (recommended first): Add a second GGX specular lobe on top of any base material:
   - Fixed IOR ~1.5 (polyurethane clear coat)
   - Independent roughness parameter
   - Energy from the base is attenuated by Fresnel reflection of the clearcoat
   - Sample: choose clearcoat or base proportional to their approximate albedos

2. **Full Belcour layered model** (optional advanced):
   - Represent each layer by its energy, mean, and variance statistics
   - Compose layers via adding-doubling operators
   - Supports textured per-layer parameters
   - More complex but handles arbitrary layer stacks

**RISE-specific notes**:
- The `CompositeSPF` / `CompositeMaterial` infrastructure could be extended for clearcoat
- A new `ClearcoatSPF` wrapping any base SPF would be the cleanest approach
- For the full Belcour model, a new `LayeredMaterial` class composing arbitrary SPF layers

**Estimated scope**: Small for clearcoat; medium-large for full Belcour.

---

## Implementation Order Recommendation

For maximum cumulative impact, work through these in order:

```
Phase 1 — Quick wins (each independent):
  1.2  VNDF Sampling
  1.3  Kulla-Conty Multiscattering
  1.1  OIDN Denoising

Phase 2 — Major convergence improvements:
  1.4  Path Guiding (OpenPGL)
  3.3  Correlation-Aware MIS

Phase 3 — Spectral efficiency:
  2.1  Hero Wavelength Spectral Sampling

Phase 4 — New capabilities:
  2.2  Null-Scattering Volumes
  2.3  VCM
  3.1  Random Walk SSS

Phase 5 — Scene complexity:
  3.2  Light BVH
  3.4  Layered Materials
```

VNDF and Kulla-Conty go first because they're small, self-contained, and improve every scene that uses microfacet materials. OIDN follows because it multiplies the perceived quality of everything else. Path guiding and HWSS are the transformative changes. Volumes and VCM open new scene types. Light BVH and layered materials are scene-complexity features that become important as RISE tackles more ambitious content.
