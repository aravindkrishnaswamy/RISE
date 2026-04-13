# HyLIoS Hyperspectral Skin Material -- Implementation Workplan

## Context

Implement the HyLIoS (Hyperspectral Light Impingement on Skin) model from Chen et al. 2015 (ACM TOG 34:3, Article 31) as a new skin material in RISE. HyLIoS is the first skin appearance model covering the full 250-2500nm hyperspectral range (UV-Vis-IR). Compared to the existing BioSpec model it adds:
- 6 skin sublayers (vs BioSpec's 4): stratum corneum (SC), stratum granulosum, stratum spinosum, stratum basale, papillary dermis, reticular dermis
- Explicit prolate-spheroid melanosome geometry with detour/sieve effects
- Additional chromophores: DNA, keratin, urocanic acid (UV absorbers), water, lipids (IR absorbers), carboxyhemoglobin, methemoglobin, sulfhemoglobin
- Connective fiber Rayleigh scattering in papillary dermis
- Per-layer iterative random walk through the slab (not analytical approximation)

The material has an SPF-only interface (like `BioSpecSkinMaterial`) with the HyLIoS per-layer slab random walk implemented inside `ScatterNM()`. It also provides `GetRandomWalkSSSParamsNM()` for 3D mesh-based random walk rendering of spatial subsurface scattering distribution.

### Key Design Principles

- **Readable subfunctions**: Follow `BioSpecSkinSPF.cpp` style where each physical process is a named helper function (e.g., `ComputeSCAbsorption`, `ProcessMelanosomeAbsorptionTest`, `SampleMelanosomeScatteringAngle`). Maximum readability and educational value.
- **Documented equations**: Every key equation from the paper is cited in code comments with equation number, variable mapping, and units. Future readers can verify the implementation against the paper directly.
- **Minimal painters**: Most parameters are scalar constants (fixed per material instance). Only parameters that genuinely vary spatially across the mesh are painters:
  - **Painters** (spatially varying): melanosome volume fractions per epidermal sublayer (`melanosomes_granulosum`, `melanosomes_spinosum`, `melanosomes_basale`), and colloidal melanin content per sublayer
  - **Scalars** (constant): all layer thicknesses, IORs, chromophore concentrations, blood fractions, water/lipid content, fiber parameters, roughness, etc.
- **Novel random walk**: The per-layer slab random walk inside the SPF is new infrastructure -- no existing RISE material does geometric intersection-based random walk within an SPF. This requires careful incremental testing: start with a single-layer walk, verify, then add layers one at a time.

### Reference Materials

- Paper: Chen et al. 2015, "Hyperspectral Modeling of Skin Appearance", ACM TOG 34:3
- Supplementary appendix: `a31-chen-app.pdf` (Tables III, IV with all parameter values; Figure 26 with chromophore absorption spectra)
- Thesis: Chen, "On the Modelling of Hyperspectral Light and Skin Interactions..." (Chapters 3-4, Appendix A)
- BioSpec reference scene: `scenes/Internal/roberto_biospec.RISEscene` (original BioSpec SPF-only model)
- Existing BioSpec RW implementation: `src/Library/Materials/BioSpecSkinRWMaterial.h/cpp`
- Existing BioSpec SPF implementation: `src/Library/Materials/BioSpecSkinSPF.h/cpp`

---

## Phase 1: Chromophore Data Tables

**Goal**: Create `HyLIoSSkinData.h` with absorption spectra for all chromophores not already in BioSpec.

**Depends on**: Nothing (can start immediately)

### Task 1.1: Identify data already available in BioSpec

**File**: `src/Library/Materials/BioSpecSkinData.h`

Already present (reuse directly from `RISE::SkinData` namespace):
- Eumelanin extinction (`omlc_eumelanin_wavelengths` / `omlc_eumelanin_ext_mgml`)
- Pheomelanin extinction (`omlc_pheomelanin_wavelengths` / `omlc_pheomelanin_ext_mgml`)
- Oxyhemoglobin extinction (`omlc_prahl_hemoglobin_wavelengths` / `omlc_prahl_oxyhemoglobin`)
- Deoxyhemoglobin extinction (same wavelength array / `omlc_prahl_deoxyhemoglobin`)
- Bilirubin extinction (`omlc_prahl_bilirubin_wavelengths` / `omlc_prahl_bilirubin`)
- Beta-carotene extinction (`omlc_prahl_betacarotene_wavelengths` / `omlc_prahl_betacarotene`)

### Task 1.2: Source and tabulate missing chromophore data

**New file**: `src/Library/Materials/HyLIoSSkinData.h`

Data needed (from appendix Figure 26 and Table IV references):

| Chromophore | Source | Units | Wavelength Range | Priority |
|---|---|---|---|---|
| **DNA** | Sutherland & Griffin 1981; Clendening 2002 | absorptance (%) over 4um pathlength at 50 ug/mL | 250-400nm | High |
| **Keratin** | Bendit & Ross 1961 | absorptance (%) for solid over 4um | 250-400nm | High |
| **Urocanic acid** | Young 1997; Oudhia 2012 | absorptance at 15 umole/L over 1cm | 250-400nm | High |
| **Water** | Palmer & Williams 1974; Pope & Fry 1997 | specific absorption coeff (cm^-1) | 250-2500nm | High |
| **Lipids** | Altshuler 2003; Prahl 2004; van Veen 2004 | specific absorption coeff (cm^-1) | 500-2500nm | High |
| **Methemoglobin** | Randeberg 2004; Siggaard-Andersen 1972 | molar extinction (cm^-1/mole/L) | 450-700nm | Medium |
| **Sulfhemoglobin** | Yarynovska & Bilyi 2006 | molar extinction (cm^-1/mole/L) | 500-700nm | Medium |
| **Carboxyhemoglobin** | Prahl 1999 | molar extinction (cm^-1/mole/L) | 400-1000nm | Medium |

**Implementation steps**:
1. Create `HyLIoSSkinData.h` in `RISE::SkinData` namespace following `BioSpecSkinData.h` pattern
2. For each chromophore: `static const Scalar xxx_wavelengths[]` and `static const Scalar xxx_extinction[]`
3. Where digitized data is not available from OMLC or published tables, flag as **DATA_NEEDED** with source reference -- these will be fetched separately in Phase 7
4. Water and lipids data is available from OMLC (Prahl's compilations) and should be obtainable
5. DNA, keratin, urocanic acid curves can be digitized from the thesis Figure 26 or from original sources

### Task 1.3: Verify spectral infrastructure compatibility

Check that `IPiecewiseFunction1D` and `SampledWavelengths` (in `src/Library/Utilities/Color/SampledWavelengths.h`) handle 250-2500nm without issues. The existing `pixelintegratingspectral_rasterizer` already supports arbitrary `nmbegin`/`nmend`, so this should work. Verify flat extrapolation behavior at LUT boundaries.

### Verification
- Build project with new header included
- Write a test program that creates `IPiecewiseFunction1D` from each new data table, evaluates at boundary wavelengths (250, 380, 550, 700, 1000, 1500, 2500nm), and prints values
- Compare sampled values against published figures

---

## Phase 2: HyLIoS SPF -- Per-Layer Slab Random Walk

**Goal**: Implement the core HyLIoS model as an ISPF with per-layer iterative random walk inside `ScatterNM()`.

**Depends on**: Phase 1 (chromophore data tables)

### Task 2.1: Create HyLIoSSkinSPF.h

**New file**: `src/Library/Materials/HyLIoSSkinSPF.h`

Class declaration: `HyLIoSSkinSPF : public virtual ISPF, public virtual Reference`

**Constructor parameters** -- split into painters (spatially varying) and scalars (constant):

**Painters** (`const IPainter&`, for spatially-varying quantities):
- `melanosomes_granulosum`, `melanosomes_spinosum`, `melanosomes_basale` (volume fraction %, can vary across face/body)
- `colloidal_melanin_granulosum`, `colloidal_melanin_spinosum`, `colloidal_melanin_basale` (colloidal melanin content %, can vary)

**Scalars** (constant across material, no default values in the header):
- Layer thicknesses (6): `thickness_SC`, `thickness_granulosum`, `thickness_spinosum`, `thickness_basale`, `thickness_papillary`, `thickness_reticular` (cm)
- IORs (5): `ior_SC`, `ior_epidermis`, `ior_papillary`, `ior_reticular`, `ior_melanin`
- Melanin concentrations: `concentration_eumelanin`, `concentration_pheomelanin` (mg/mL)
- Melanosome geometry: `melanosome_a`, `melanosome_b` (semi-minor/major axes, um)
- Blood: `hb_ratio`, `blood_papillary`, `blood_reticular` (%), `hb_concentration` (mg/mL)
- Dysfunctional Hb: `cohb_concentration`, `methb_concentration`, `sulfhb_concentration` (mg/mL)
- Other chromophores: `bilirubin_concentration`, `betacarotene_SC`, `betacarotene_epidermis`, `betacarotene_dermis`
- Water content: `water_SC`, `water_epidermis`, `water_papillary`, `water_reticular` (%)
- Lipid content: `lipid_SC`, `lipid_epidermis`, `lipid_papillary`, `lipid_reticular` (%)
- UV absorbers: `keratin_SC` (%), `urocanic_acid_SC` (mol/L), `dna_density` (mg/mL)
- Connective fibers: `fiber_radius` (cm), `fiber_volume_fraction`, `fiber_ior_ratio`
- Surface: `surface_roughness` (Trowbridge-Reitz s parameter), `max_walk_bounces` (unsigned int)

Internal state: 14 `IFunction1D*` chromophore LUTs (6 reused from BioSpec data + 8 new from HyLIoSSkinData).

### Task 2.2: Create HyLIoSSkinSPF.cpp -- Constructor

**New file**: `src/Library/Materials/HyLIoSSkinSPF.cpp`

Constructor pattern follows `BioSpecSkinSPF.cpp`:
1. Store painter references (6 melanosome painters), addref each
2. Store all scalar parameters as member variables
3. Build 14 chromophore LUTs from data tables via `RISE_API_CreatePiecewiseLinearFunction1D`
4. Compute and log derived quantities at diagnostic wavelengths (450, 550, 650nm)

### Task 2.3: Per-layer absorption -- named helper functions

Each layer gets its own named function following BioSpecSkinSPF's style. Every function documents the equation number from Chen et al. 2015 and the units.

**`ComputeSCAbsorption(nm)`** -- Stratum corneum absorption (cm^-1)
```cpp
/// Stratum corneum volumetric absorption coefficient.
/// Eq 2 pattern from Chen et al. 2015.  SC contains keratin (UV),
/// DNA (UV), urocanic acid (UV), water, lipids, beta-carotene,
/// and a wavelength-dependent baseline.
/// Units: cm^-1
Scalar ComputeSCAbsorption(Scalar nm) const;
```

**`ComputeEpidermisAbsorption(nm, colloidal_fraction)`** -- Shared by granulosum/spinosum/basale
```cpp
/// Non-melanosome absorption in an epidermal sublayer.
/// The melanosome contribution is handled separately as an
/// attenuator in the random walk (see ProcessMelanosomeInteraction).
/// Colloidal melanin (melanin dust outside melanosomes) is
/// aggregated into the bulk absorption here.
/// Units: cm^-1
Scalar ComputeEpidermisAbsorption(Scalar nm, Scalar colloidal_fraction) const;
```

**`ComputeDermisAbsorption(nm, blood_fraction)`** -- Shared by papillary/reticular dermis (Eq 2)
```cpp
/// Dermal volumetric absorption coefficient (Eq 2, Chen et al. 2015).
/// mu_a = sum of blood pigment contributions weighted by blood volume
/// fraction, plus tissue baseline for the non-blood fraction.
/// Blood pigments: oxyHb, deoxyHb, carboxyHb, metHb, sulfHb,
///                 bilirubin, beta-carotene
/// Tissue: water, lipids, baseline
/// Units: cm^-1
Scalar ComputeDermisAbsorption(Scalar nm, Scalar blood_fraction) const;
```

**`ComputeSkinBaseline(nm)`** -- Reuse BioSpec formula
```cpp
/// Wavelength-dependent baseline absorption for skin tissue.
/// 0.244 + 85.3 * exp(-(nm - 154) / 66.2)  [cm^-1]
static Scalar ComputeSkinBaseline(Scalar nm);
```

### Task 2.4: Per-layer scattering -- named helper functions

**`ComputeMelanosomeAttenuation(v_melanosome)`** -- Geometric attenuation (Eq 3-4)
```cpp
/// Geometric attenuation coefficient for melanosomes modeled as
/// prolate spheroids (Eq 3, Chen et al. 2015).
///   mu_g = (S/V) * v / 4
/// where S/V is the surface-to-volume ratio of the prolate spheroid
/// (Eq 4):
///   S/V = (3/2a) * (a/b + arcsin(c)/c),  c = sqrt(1 - a^2/b^2)
/// and v is the volume fraction occupied by melanosomes.
/// Units: cm^-1
Scalar ComputeMelanosomeAttenuation(Scalar v_melanosome) const;
```

**`ComputeConnectiveFiberScattering(nm)`** -- Rayleigh scattering (Eq 7)
```cpp
/// Rayleigh scattering coefficient from connective fibers in the
/// papillary dermis (Eq 7, Chen et al. 2015).
///   mu_s^R = (128*pi^5*r^6*v_f) / (3*lambda^4*(4/3*pi*r^3))
///            * ((eta^2 - 1)/(eta^2 + 1))^2
/// Default: r=100nm, eta=1.5/1.33, v_f=0.22 (Jacques 1996)
/// Units: cm^-1
Scalar ComputeConnectiveFiberScattering(Scalar nm) const;
```

**`ComputeEpidermisMieScattering(nm)`** -- Mie-like power law
```cpp
/// Reduced scattering coefficient for SC and epidermal sublayers.
/// Mie-like power law: sigma_sp = 73.7 * (nm/500)^(-2.33)
/// From Krishnaswamy & Baranoski 2004.
/// Units: cm^-1
Scalar ComputeEpidermisMieScattering(Scalar nm) const;
```

### Task 2.5: Melanosome interaction model

Implement the detailed melanosome model from Section 4.2.1 of the paper. Each step should be a named helper function.

**`ProcessMelanosomeInteraction()`** -- Full melanosome light interaction:
1. Generate prolate spheroid orientation per PDFs (Eq 5-6)
   - Polar: `P_m(alpha) = (chi1*(1-|cos(a)|) + chi2*|cos(a)|) / (chi1+chi2)` where chi1, chi2 are cross-sectional areas associated with minor and major axes
   - Azimuthal: `P_m(beta) = 1/(2*pi)` (uniform)
2. Compute distance to melanosome surface using geometric attenuation `mu_g`
3. Test absorption inside melanosome using melanin extinction coefficients
4. If not absorbed: scatter with forward-peaked exponential distribution
   - Mean scattering angle theta_o = 5 degrees (Chedekel 1995)
   - Sampling via Algorithm 1 from paper (exponential perturbation with rejection)
   - Wavelength-dependent scattering efficiency: full below 780nm, linear fade 780-1300nm, zero above 1400nm
5. Rejection sampling to ensure perturbed ray stays inside melanosome (dot product test)

**`ProcessMelanosomeComplexInteraction()`** -- For lightly pigmented skin:
- Melanosome complexes: spherical encapsulation with n_m melanosomes
- Complex S/V = 3/r_s (sphere geometry)
- Each interaction tests up to n_m enclosed melanosomes
- Apply melanosome orientation selection and absorption test per melanosome

**`SampleMelanosomeScatteringAngle(nm, sampler)`** -- Algorithm 1 from paper:
```cpp
/// Sample forward-peaked scattering angle from a melanosome.
/// Algorithm 1 from Chen et al. 2015.
/// Mean polar scattering angle theta_o = 5 degrees (Chedekel 1995).
/// Scattering efficiency decreases linearly 780-1300nm, zero >1400nm.
///
/// phi = arctan(theta_o)
/// max = (1/theta_o) * exp(-phi/theta_o) * sin(phi)
/// repeat:
///   xi3, xi4 = random[0,1)
///   theta_m = pi * xi3
/// until max * xi4 <= (1/theta_o) * exp(-theta_m/theta_o) * sin(theta_m)
/// return theta_m
Scalar SampleMelanosomeScatteringAngle(Scalar nm, ...) const;
```

**`SampleRayleighScatteringAngle(sampler)`** -- Algorithm 2 from paper:
```cpp
/// Sample Rayleigh scattering angle for connective fibers.
/// Algorithm 2 from Chen et al. 2015 (McCartney 1976).
/// repeat:
///   xi5, xi6 = random[0,1)
///   theta_R = pi * xi5
/// until xi6 <= (3*sqrt(2)/8) * (1 + cos^2(theta_R)) * sin(theta_R)
/// return theta_R
Scalar SampleRayleighScatteringAngle(...) const;
```

### Task 2.6: Per-layer slab random walk (NOVEL INFRASTRUCTURE)

**This is the most critical and novel piece.** No existing RISE material performs a geometric random walk with layer boundary intersections inside an SPF. The walk is a 1D slab model (rays traverse semi-infinite planar layers), NOT a 3D mesh walk. Layer boundaries are horizontal planes at cumulative depth positions. This must be built and tested incrementally.

**Incremental development approach**:

**Step 2.6a: Single-layer homogeneous walk (SC only)**
- Implement `PerformSlabWalk()` with just the SC layer
- SC has only bulk absorbers and Mie scattering (no melanosomes)
- Layer boundaries: top = skin surface (depth=0), bottom = SC/granulosum interface (depth=d_SC)
- Free-flight distance: `d = -ln(xi) / mu(lambda)` where mu is the total attenuation coefficient (Eq 1)
- At event: test absorption vs scattering based on `mu_a / (mu_a + mu_s)` ratio
- At boundary: Fresnel test (transmitted or reflected)
- **Test**: Render flat slab with SC-only walk, verify reasonable reflectance

**Step 2.6b: Add remaining 5 layers**
- Extend `PerformSlabWalk()` to track which layer the ray is in
- Layer depth array: `[0, d_SC, d_SC+d_gran, ..., total_thickness]`
- Each layer has its own absorption and scattering coefficients
- When ray crosses a boundary: compute Fresnel at interface IOR ratio
- Hypodermis boundary (bottom of reticular dermis): total internal reflection (reflectance = 1, paper states dermal-hypodermal junction has reflectance of 1)
- **Test**: Render slab with all 6 layers, verify reflectance changes vs single-layer

**Step 2.6c: Add melanosome attenuators (epidermis sublayers only)**
- In granulosum/spinosum/basale layers, add melanosome attenuation channel
- Three competing distances: `d_absorber`, `d_melanosome`, `d_boundary`
- When melanosome is hit: call `ProcessMelanosomeInteraction()` (Task 2.5)
- **Test**: Verify melanin absorption effect on reflectance spectrum (lower blue reflectance)

**Step 2.6d: Add connective fiber scattering (papillary dermis only)**
- In papillary dermis, add Rayleigh scattering channel from connective fibers
- Three competing distances: `d_absorber`, `d_fiber`, `d_boundary`
- When fiber is hit: sample Rayleigh scatter direction via Algorithm 2
- **Test**: Verify more diffuse dermis scattering

**Named helper functions for the walk**:

**`PerformSlabWalk(ray_dir, depth, nm, ri, sampler)`** -- Main walk loop
```cpp
/// Iterative random walk through the 6-layer skin slab.
/// Follows Figure 6 flowchart from Chen et al. 2015.
///
/// The slab is modeled as horizontal layers stacked in depth.
/// The ray's z-component (in the local shading frame) determines
/// which direction it travels (into skin = +z, out of skin = -z).
/// Layer boundaries are at cumulative depth positions.
///
/// Returns: true if ray exits through top surface, false if absorbed.
/// On exit, exit_dir contains the exit direction in local frame.
bool PerformSlabWalk(...) const;
```

**`ComputeDistanceToLayerBoundary(depth, dir_z, layer)`** -- Geometric intersection
```cpp
/// Distance from current depth to the nearest layer boundary
/// in the ray's travel direction.  The slab geometry is 1D:
/// boundaries are horizontal planes at fixed depth positions.
/// dir_z > 0 means traveling deeper (toward hypodermis),
/// dir_z < 0 means traveling toward skin surface.
Scalar ComputeDistanceToLayerBoundary(Scalar depth, Scalar dir_z, int layer) const;
```

**`DetermineLayer(depth)`** -- Which layer contains this depth
```cpp
/// Determine which skin layer the given depth falls within.
/// Returns layer index 0-5 (SC=0, granulosum=1, ..., reticular=5).
int DetermineLayer(Scalar depth) const;
```

**`TestFresnelAtBoundary(cos_theta, ior_above, ior_below, sampler)`** -- Fresnel coin flip
```cpp
/// Fresnel reflection/transmission test at a layer boundary.
/// Uses exact Fresnel equations for unpolarized light.
/// Returns true if the ray is reflected, false if transmitted.
bool TestFresnelAtBoundary(Scalar cos_theta, Scalar ior_above, Scalar ior_below, ...) const;
```

**`SelectAttenuationEvent(layer, nm, v_melanosome, d_boundary, sampler)`** -- Event selection
```cpp
/// Generate distances to competing attenuation events using Eq 1:
///   d = -1/mu(lambda) * ln(xi)
/// and select the nearest one.
///
/// Event types per layer:
///   SC:                    absorber, Mie scatter, boundary
///   Granulosum/Spinosum/Basale: absorber, melanosome, Mie scatter, boundary
///   Papillary dermis:      absorber, connective fiber, boundary
///   Reticular dermis:      absorber, boundary
///
/// Returns the event type and the distance to it.
enum AttenuationEvent { eAbsorbed, eMelanosome, eScattered, eBoundary };
AttenuationEvent SelectAttenuationEvent(..., Scalar& d_event) const;
```

### Task 2.7: ScatterNM() main entry point

```cpp
/// Main spectral scattering evaluation for HyLIoS skin model.
///
/// The evaluation proceeds in three stages:
///
/// 1. SURFACE ROUGHNESS: Sample a perturbed surface normal from the
///    Trowbridge-Reitz distribution (Eq 8, Chen et al. 2015) using
///    the surface fold aspect ratio parameter s.
///
/// 2. FRESNEL BOUNDARY: Test reflection vs transmission at the
///    air-SC boundary using exact Fresnel equations with the
///    perturbed normal.  If reflected, return the specular reflected
///    ray immediately.
///
/// 3. SUBSURFACE WALK: If transmitted into skin, refract the ray
///    into the SC layer using Snell's law, then run PerformSlabWalk().
///    If the walk exits through the top surface, return a diffuse
///    scattered ray with krayNM = 1.  If absorbed, return no ray.
void ScatterNM(ri, random, scattered, ior_stack, nm);
```

### Verification (incremental, matching steps 2.6a-d)
- **Step 2.6a test**: Flat slab, SC-only walk. Render at 550nm, verify ~5-10% surface reflectance (Fresnel) plus some subsurface. Compare against BioSpec baseline absorption.
- **Step 2.6b test**: All 6 layers, no melanosomes/fibers. Higher reflectance from dermis scattering. Verify wavelength-dependent reflectance shape.
- **Step 2.6c test**: Add melanosomes. Reflectance should decrease in blue (melanin absorbs more at short wavelengths). Compare spectral shape against paper Figure 8.
- **Step 2.6d test**: Add connective fibers. More diffuse scattering in dermis. Compare full model against paper Figure 8 (NCSU curves 117 and 113).
- **Full validation**: Render with datasets S1-S4 from appendix Table III to verify skin type variation.
- **UV/IR tests**: Render at 250-400nm (verify DNA/keratin dominance) and 700-2500nm (verify water absorption bands at ~970, ~1200, ~1450, ~1930nm).

---

## Phase 3: HyLIoS Material Class

**Goal**: Wire the SPF into a material that also provides effective random walk parameters for 3D mesh rendering.

**Depends on**: Phase 2 (SPF)

### Task 3.1: Create HyLIoSSkinMaterial.h

**New file**: `src/Library/Materials/HyLIoSSkinMaterial.h`

```cpp
class HyLIoSSkinMaterial : public virtual IMaterial, public virtual Reference
{
    HyLIoSSkinSPF*     pSPF;          // full per-layer slab walk
    // ... chromophore LUTs for GetRandomWalkSSSParamsNM
    // ... biophysical scalars (same set as SPF)
    
    IBSDF* GetBSDF() const { return 0; }           // SPF-only
    ISPF* GetSPF() const { return pSPF; }
    IEmitter* GetEmitter() const { return 0; }
    bool CouldLightPassThrough() const { return false; }
    bool IsVolumetric() const { return false; }
    ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return 0; }
    
    // spectral only -- RGB walk disabled
    const RandomWalkSSSParams* GetRandomWalkSSSParams() const { return 0; }
    bool GetRandomWalkSSSParamsNM(Scalar nm, RandomWalkSSSParams& p) const;
    
    SpecularInfo GetSpecularInfoNM(ri, ior_stack, nm) const;
};
```

### Task 3.2: Create HyLIoSSkinMaterial.cpp

**New file**: `src/Library/Materials/HyLIoSSkinMaterial.cpp`

**Constructor**:
1. Store all scalar parameters and painter references
2. Build chromophore LUTs (same set as SPF for `GetRandomWalkSSSParamsNM`)
3. Create `HyLIoSSkinSPF` with all parameters
4. Log construction parameters and diagnostic coefficients at 450, 550, 650nm

**`ComputeEffectiveCoefficients(nm)`**: Named helper computing homogeneous walk params from 6-layer model:
```cpp
/// Compute effective homogeneous walk coefficients at wavelength nm
/// by thickness-weighted averaging of all 6 layers' absorption and
/// reduced scattering coefficients, analogous to
/// BioSpecSkinRWMaterial::ComputeEffectiveCoefficients().
///
/// Also computes the melanin boundary filter: the double-pass
/// transmittance through the SC and all three epidermal sublayers.
///   melaninFilter = exp(-4 * sum(sigma_a_layer * d_layer))
/// for layers SC through basale.  The factor of 4 accounts for
/// the diffuse slab double-pass (entry + exit, each 2x thickness).
void ComputeEffectiveCoefficients(Scalar nm,
    Scalar& sigma_a_eff, Scalar& sigma_sp_eff,
    Scalar& melaninFilterTransmittance) const;
```

**`GetRandomWalkSSSParamsNM(nm, params)`**:
- Call `ComputeEffectiveCoefficients(nm)` for effective sigma_a, sigma_sp
- Convert cm^-1 to m^-1 (multiply by 100)
- Set g=0 (similarity principle), ior=ior_SC, maxBounces, boundaryFilter, maxDepth

### Verification
- Render `roberto_hylios.RISEscene` with spectral path tracing rasterizer
- Compare visually against `roberto_biospec.RISEscene` (should be similar but not identical -- HyLIoS has more detail)
- Verify skin-like appearance with correct warm tone

---

## Phase 4: Parser and API Integration

**Goal**: Wire the material into scene files, parser, and RISE_API.

**Depends on**: Phase 3 (material class)

### Task 4.1: RISE_API

**Modify** `src/Library/RISE_API.h`: Add `RISE_API_CreateHyLIoSSkinMaterial()` declaration.

**Modify** `src/Library/RISE_API.cpp`: Add factory implementation following the pattern of `RISE_API_CreateBioSpecSkinRWMaterial()` (around line 1114). The function takes the 6 painter references + all scalar parameters + roughness + maxBounces, creates and returns a `HyLIoSSkinMaterial*`.

### Task 4.2: IJob / Job

**Modify** `src/Library/Interfaces/IJob.h`: Add pure virtual `AddHyLIoSSkinMaterial()` method.

**Modify** `src/Library/Job.h`: Add method declaration.

**Modify** `src/Library/Job.cpp`: Add `Job::AddHyLIoSSkinMaterial()` implementation (~130 lines, follows the pattern of `Job::AddBioSpecSkinRWMaterial()` at line 1711). This method:
1. Takes string names for the 6 painter parameters (resolved via `pPntManager`)
2. Takes scalar values for all other parameters
3. Calls `RISE_API_CreateHyLIoSSkinMaterial()`
4. Adds the result to `pMatManager`

### Task 4.3: Scene parser

**Modify** `src/Library/Parsers/AsciiSceneParser.cpp`:
1. Add `HyLIoSSkinMaterialAsciiChunkParser` struct (follows `BioSpecSkinRWMaterialAsciiChunkParser` pattern)
2. Register at the material chunk parser table: `chunks["hylios_skin_material"] = new HyLIoSSkinMaterialAsciiChunkParser();` (near line 7309)

Scene file block -- painters only for spatially-varying melanosome parameters, all others are scalars:
```
hylios_skin_material
{
    name                        skin

    # Painters (spatially varying -- can reference texture painters)
    melanosomes_granulosum      <painter_name_or_value>
    melanosomes_spinosum        <painter_name_or_value>
    melanosomes_basale          <painter_name_or_value>
    colloidal_melanin_granulosum <painter_name_or_value>
    colloidal_melanin_spinosum  <painter_name_or_value>
    colloidal_melanin_basale    <painter_name_or_value>

    # Scalars (all remaining parameters)
    thickness_SC                0.001       # cm (Table III)
    thickness_granulosum        0.0033
    thickness_spinosum          0.0033
    thickness_basale            0.0033
    thickness_papillary         0.02
    thickness_reticular         0.1
    ior_SC                      1.55        # Table IV
    ior_epidermis               1.4
    ior_papillary               1.39
    ior_reticular               1.41
    ior_melanin                 1.7
    concentration_eumelanin     90.0        # mg/mL (Table III)
    concentration_pheomelanin   4.0
    melanosome_a                0.41        # um, semi-minor axis
    melanosome_b                0.17        # um, semi-major axis
    hb_ratio                    0.75        # oxygenated fraction (Table III)
    blood_papillary             0.002       # volume fraction (Table III)
    blood_reticular             0.002
    cohb_concentration          1.5         # mg/mL (Table IV)
    methb_concentration         1.5
    sulfhb_concentration        0.0
    bilirubin_concentration     0.003
    betacarotene_SC             2.1e-4      # mg/mL (Table IV)
    betacarotene_epidermis      2.1e-4
    betacarotene_dermis         7.0e-5
    water_SC                    35.0        # % (Table IV)
    water_epidermis             60.0
    water_papillary             75.0
    water_reticular             75.0
    lipid_SC                    20.0        # % (Table IV)
    lipid_epidermis             15.1
    lipid_papillary             17.33
    lipid_reticular             17.33
    keratin_SC                  65.0        # % (Table IV)
    urocanic_acid_SC            0.01        # mol/L (Table IV)
    dna_density                 0.185       # mg/mL (Table IV)
    fiber_radius                1e-5        # cm (= 100nm)
    fiber_volume_fraction       0.22        # (Jacques 1996)
    fiber_ior_ratio             1.128       # 1.5/1.33
    roughness                   0.1         # Trowbridge-Reitz s
    max_bounces                 256
}
```
Default values are from Table IV of the supplementary appendix (Chen et al. 2014).

### Task 4.4: Build system

**Modify** `build/make/rise/Filelist`: Add new source files:
```
$(PATHLIBRARY)Materials/HyLIoSSkinMaterial.cpp
$(PATHLIBRARY)Materials/HyLIoSSkinSPF.cpp
```

### Verification
- `make -C build/make/rise -j8 all` builds cleanly with zero warnings
- Parse a scene file with `hylios_skin_material` block without errors
- Render and produce a valid output image

---

## Phase 5: Test Scenes

**Goal**: Create comprehensive test scenes using the roberto model.

**Depends on**: Phase 4 (parser integration)

### Task 5.1: Primary test scene

**New file**: `scenes/Tests/HyLIoS/roberto_hylios.RISEscene`

Based on `scenes/Internal/roberto_biospec.RISEscene` structure:
- `pixelintegratingspectral_rasterizer` with nmbegin=380, nmend=720, num_wavelengths=8, samples=256
- `blackbody_painter` at 6500K for illumination (D65-approximating)
- Roberto mesh (`models/risemesh/internal/roberto.risemesh`) with `hylios_skin_material`
- Default parameters from Table IV: light Fitzpatrick type skin (dataset S3 from Table III)
- Compare visually against `scenes/Internal/roberto_biospec.RISEscene` (original BioSpec SPF model)

### Task 5.2: Skin type variation scenes

**New files**: `scenes/Tests/HyLIoS/roberto_hylios_S{1,2,3,4}.RISEscene`

Four scenes using datasets S1-S4 from appendix Table III:
- **S1**: Very light skin, melanosome complexes (melanosomes 1.0, 1.0, 1.0% in granulosum/spinosum/basale)
- **S2**: Light skin, melanosome complexes (melanosomes 0, 0, 3.75%)
- **S3**: Medium skin, melanosome complexes (melanosomes 0, 0, 3.0%)
- **S4**: Dark skin, individually dispersed melanosomes (melanosomes 10, 10, 10%, larger spheroids 0.69x0.28 um)

### Task 5.3: Slab validation scene

**New file**: `scenes/Tests/HyLIoS/hylios_slab.RISEscene`

Flat slab (clipped plane) geometry with HyLIoS material for spectral reflectance validation against paper's Figure 8 (comparison with NCSU measured data). Use high sample count (e.g., 10^5) for quantitative comparison.

### Task 5.4: UV and IR test scenes

**New files**: `scenes/Tests/HyLIoS/roberto_hylios_uv.RISEscene`, `roberto_hylios_ir.RISEscene`
- UV scene: nmbegin=250, nmend=400. Verify DNA/keratin/urocanic acid dominance.
- IR scene: nmbegin=700, nmend=2500. Verify water absorption bands at ~970, ~1200, ~1450, ~1930nm. Verify softer, more diffuse IR appearance.

### Verification
- All scenes parse and render without crashes
- Visual inspection: skin appearance matches expected characteristics per skin type
- UV scene shows enhanced melanin/pigmentation irregularity visibility
- IR scene shows softer, more diffuse appearance
- Spectral reflectance from slab scene matches paper's Figure 8 within expected tolerance

---

## Phase 6: Regression Testing

**Goal**: Ensure no existing functionality is broken.

**Depends on**: All implementation phases complete

### Task 6.1: BioSpec regression

Render `scenes/Internal/roberto_biospec.RISEscene` before and after all changes. Verify identical output. Since all changes are additive (no existing code modified), the output should be bit-for-bit identical.

### Task 6.2: Verify additive-only changes

All files modified outside the new material are strictly additive:
- `RISE_API.h/cpp` -- new function added (no existing functions changed)
- `IJob.h`, `Job.h/cpp` -- new virtual + implementation added
- `AsciiSceneParser.cpp` -- new chunk parser + registration line added
- `Filelist` -- new source files added

**No regression risk to existing materials.** However, verify:
- BioSpec SPF material (`biospec_skin_material`) renders correctly
- BioSpec BSSRDF material (`biospec_skin_bssrdf_material`) renders correctly
- BioSpec RW material (`biospec_skin_rw_material`) renders correctly
- Generic SSS material (`subsurfacescattering_material`) renders correctly

### Task 6.3: Combined scene test

Create a scene with both BioSpec and HyLIoS materials on separate objects (e.g., two roberto heads side by side) to verify they coexist without interference.

---

## Phase 7: Data Sourcing (Parallel Stage)

**Goal**: Acquire chromophore absorption spectra that aren't readily available.

**Depends on**: Nothing (runs independently, needed by Phase 1)

### Required data with sources

1. **Water absorption** (250-2500nm): OMLC (Prahl compilation of Palmer & Williams 1974, Pope & Fry 1997). Well-published tabulated data, should be straightforward to obtain.

2. **Lipid absorption** (500-2500nm): van Veen et al. 2004 published VIS-NIR absorption coefficients. Prahl 2004 OMLC tech report has spectral data by category.

3. **DNA absorption** (250-400nm): Sutherland & Griffin 1981 published UV DNA absorption for wavelengths >300nm. Clendening 2002 covers UV spectrophotometric analysis.

4. **Keratin absorption** (250-400nm): Bendit & Ross 1961 published UV absorption of solid keratin over 4um pathlength.

5. **Urocanic acid** (250-400nm): Young 1997 provides data. Trans/cis forms have distinct peaks (~270nm and ~310nm respectively).

6. **Carboxyhemoglobin** (400-1000nm): Prahl 1999 OMLC compilation includes COHb alongside other hemoglobin variants.

7. **Methemoglobin** (400-700nm): Randeberg et al. 2004 published MetHb spectrum. Also available from Siggaard-Andersen 1972.

8. **Sulfhemoglobin** (500-700nm): Yarynovska & Bilyi 2006 is the primary source. Limited wavelength range. Low priority since default concentration = 0 mg/mL (Table IV).

### Fallback strategy
If exact tabulated data is unavailable for some chromophores:
- Digitize curves from thesis Figure 26 (all curves are plotted with axis scales)
- Use analytical approximations where available (e.g., melanin baseline formula, Rayleigh 1/lambda^4)
- For sulfhemoglobin: lowest priority since it's typically set to zero in all datasets

---

## File Summary

### New files (7+)
| File | Purpose | Estimated Size |
|---|---|---|
| `src/Library/Materials/HyLIoSSkinData.h` | Chromophore absorption spectra tables | ~30-50 KB |
| `src/Library/Materials/HyLIoSSkinSPF.h` | SPF class declaration | ~8 KB |
| `src/Library/Materials/HyLIoSSkinSPF.cpp` | Per-layer random walk implementation | ~40-60 KB |
| `src/Library/Materials/HyLIoSSkinMaterial.h` | Material class declaration | ~8 KB |
| `src/Library/Materials/HyLIoSSkinMaterial.cpp` | Material + effective RW coefficients | ~15 KB |
| `scenes/Tests/HyLIoS/roberto_hylios.RISEscene` | Primary test scene | ~3 KB |
| `scenes/Tests/HyLIoS/hylios_slab.RISEscene` | Slab validation scene | ~2 KB |

### Modified files (7, all additive changes only)
| File | Change |
|---|---|
| `src/Library/RISE_API.h` | Add `RISE_API_CreateHyLIoSSkinMaterial` declaration |
| `src/Library/RISE_API.cpp` | Add factory implementation (~30 lines) |
| `src/Library/Interfaces/IJob.h` | Add `AddHyLIoSSkinMaterial` virtual method |
| `src/Library/Job.h` | Add method declaration |
| `src/Library/Job.cpp` | Add factory method (~130 lines) |
| `src/Library/Parsers/AsciiSceneParser.cpp` | Add chunk parser struct + registration |
| `build/make/rise/Filelist` | Add 2 new .cpp files |

### Reused existing infrastructure (no changes needed)
- `BioSpecSkinData.h` -- existing 6 chromophore LUTs reused directly
- `RandomWalkSSS.h` -- 3D mesh walk (via `GetRandomWalkSSSParamsNM`)
- `IMaterial.h` -- existing interface suffices
- `ISPF.h` -- existing SPF interface suffices
- `IPiecewiseFunction1D` -- tabulated data interpolation
- `SampledWavelengths.h` -- spectral sampling infrastructure
- `BioSpecDiffusionProfile::ComputeBeta()` -- may be reused for Rayleigh formula comparison

---

## Implementation Order

1. **Phase 7** (Data sourcing) -- start immediately in parallel, identifies what data we have vs need to fetch
2. **Phase 1** (Data tables) -- create `HyLIoSSkinData.h` with available data, stub missing entries with DATA_NEEDED markers
3. **Phase 2** (SPF) -- the core implementation, largest phase, built incrementally per steps 2.6a-d
4. **Phase 3** (Material class) -- wire SPF + effective RW params
5. **Phase 4** (Parser/API) -- scene file integration
6. **Phase 5** (Test scenes) -- validation scenes
7. **Phase 6** (Regression) -- safety verification

Phases 2-4 are sequential. Phases 5-6 can be done incrementally as each phase completes.
