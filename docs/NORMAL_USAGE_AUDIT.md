# Normal-Usage Audit: `vNormal` vs `vGeomNormal`

*Audit date: 2026-05-04. Branch: `claude/romantic-hofstadter-bf260a` (worktree off
`master` @ 597c750).*

## Background

`RayIntersectionGeometric` now carries shading (`vNormal`/`vNormal2`,
Phong-interpolated and perturbable by BumpMap / NormalMap) AND geometric
(`vGeomNormal`/`vGeomNormal2`, flat-face) normals. They are identical on
analytical primitives. Triangle-mesh producers populate them separately
([RayIntersectionGeometric.h:60-93](src/Library/Intersection/RayIntersectionGeometric.h:60),
[GeometricNormalPlumbingTest.cpp](tests/GeometricNormalPlumbingTest.cpp)).
Until the SMS validator fix, every consumer silently used the shading normal;
this document audits the remaining 300+ consumer sites.

## Reference rules

- **BSDF eval / sample / pdf and the BSDF cosine factor `cos θ` in
  `f · cos θ / pdf`** → SHADING. BRDF lives in the shading frame and Veach's
  "shading normals" trick is energy-preserving when the cos couples to
  `f` consistently ([PBRT 4e §9.1](https://pbr-book.org/4ed/Reflection_Models/BSDF_Representation);
  [Veach 1997 §5.3.6](https://graphics.stanford.edu/papers/veach_thesis/);
  Mitsuba 3 [`SurfaceInteraction.sh_frame`](https://mitsuba.readthedocs.io/en/stable/src/key_topics/shape_normals.html)).
- **Side-of-surface tests** — front-back, entering vs exiting,
  one-sided culling, medium-stack push/pop, IOR-stack seeding, TIR sign,
  shadow-ray endpoint visibility → GEOMETRIC. These ask "which side of the
  *actual* surface", independent of shading perturbation
  ([PBRT 4e §10.1.1](https://pbr-book.org/4ed/Textures_and_Materials/Material_Interface_and_Implementations#Bump_and_NormalMapping)).
- **Solid-angle ↔ area pdf Jacobian (`cos / d²`)** → GEOMETRIC. The Jacobian is
  `|n_g · ω| dA / d²` where `n_g` is the actual surface-element normal
  ([Veach §8.2](https://graphics.stanford.edu/papers/veach_thesis/);
  [PBRT 4e §13.6.4](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Path-Space_Measurement_Equation)).
- **Half-vector reflect/refract** → SHADING for the *direction*, GEOMETRIC for
  the side-correctness check (Mitsuba 3's `BSDFContext` validates `wi` against
  the geometric frame before evaluation; PBRT 4e §9.5).
- **Ray-offset for self-intersection** → GEOMETRIC ([PBRT 4e §6.8.6](https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error#OffsetRayOrigin)).
  RISE currently offsets along ray direction (no normal involved); not in scope.
- **Photon-map deposit** → record both. SHADING couples to BSDF eval at gather;
  GEOMETRIC drives the thin-surface / opposite-face rejection cone (Jensen 2001).
- **SMS** ([Zeltner, Georgiev, Jakob 2020](https://rgl.epfl.ch/publications/Zeltner2020Specular)):
  half-vector constraint is BSDF-coupled (shading); the *validator* of
  converged chains operates on geometric topology.

`KEEP` = matches literature. `FIX_GEOM` = should be `vGeomNormal`. `FIX_BOTH` =
different normal per sub-use. `INVESTIGATE` = non-obvious.

---

## 1. Path-space integrators

PT, BDPT, VCM, FinalGather, DistributionTracing, AmbientOcclusion, Transparency,
Emission, AreaLight, DirectVolumeRendering, PathVertexEval, BSSRDFEntryAdapters,
SSS shaders.

| file:line | bucket | RISE uses | literature says | verdict | reference |
|---|---|---|---|---|---|
| [PathTracingIntegrator.cpp:1413](src/Library/Shaders/PathTracingIntegrator.cpp:1413), 3231, 4499 | NEE area-pdf MIS Jacobian `cosLight` | shading | geometric | **FIX_GEOM** | PBRT 4e §13.6.4 |
| [PathTracingIntegrator.cpp:1508](src/Library/Shaders/PathTracingIntegrator.cpp:1508), 1623, 3314, 3435 | BSSRDF entry front-face gate | shading | geometric for the gate; shading for the Fresnel `Ft` cosine | **FIX_BOTH** | PBRT 4e §11.4.2 |
| [PathTracingIntegrator.cpp:509](src/Library/Shaders/PathTracingIntegrator.cpp:509) | OpenPGL guiding cosine flip | shading | shading (BSDF-coupled) | KEEP | guiding cosine product is BRDF-coupled |
| PathTracingIntegrator.cpp:1387, 3221, 4490 | `pEmitter->emittedRadiance(... vNormal)` | shading | per-emitter convention; geometric for area emitters | INVESTIGATE | depends on emitter — almost always geometric in practice |
| PathTracingIntegrator.cpp:1954, 2027, 2655 | OIDN normal AOV | shading | shading | KEEP | OIDN's `oidnNormalFilter` documents shading-normal input |
| PathTracingIntegrator.cpp:2100, 4585, 3703 | SMS receiver-frame `N` parameter | shading | shading for the receiver-side BSDF cosine; geometric for chain topology | INVESTIGATE | SMS API conflates the two — pass both |
| PathTracingIntegrator.cpp:2265, 2294, 2387, 3837–3945, 4698 | guiding/RIS BSDF throughput cosine | shading | shading | KEEP | Veach §5.3.6 |
| [BDPTIntegrator.cpp:1750](src/Library/Shaders/BDPTIntegrator.cpp:1750), 2883, 5424 | `SolidAngleToArea` Jacobian + `v.cosAtGen` | shading | geometric | **FIX_GEOM** | PBRT 4e eq. 13.10 / Veach §8.2 |
| [BDPTIntegrator.cpp:1114](src/Library/Shaders/BDPTIntegrator.cpp:1114), 1251 | medium-stack push/pop side test on connection walk | shading | geometric | **FIX_GEOM** | PBRT 4e §11.3.4 (medium boundaries are topological) |
| BDPTIntegrator.cpp:1923, 1986, 3050, 3106, 5588, 5643 | BSSRDF entry front-face gate (BDPT mirror) | shading | geometric (gate) + shading (Fresnel) | **FIX_BOTH** | PBRT 4e §11.4.2 |
| [BDPTIntegrator.cpp:1727](src/Library/Shaders/BDPTIntegrator.cpp:1727), 2860, 5406 | `BDPTVertex.normal := ri.vNormal` propagation | shading | both | **FIX_BOTH (carrier-extension)** | vertex must carry both for downstream sites; see §6 below |
| BDPTIntegrator.cpp:5070 | `cosAtLight` for emitted-radiance throughput Jacobian | upstream-stored shading | geometric | **FIX_GEOM** | PBRT 4e §15.4 |
| BDPTIntegrator.cpp:2263, 3363, 5907, 6937 + 2325, 5990 | scatter throughput cosine and reverse cosine | shading | shading | KEEP | BSDF cosine factor — Veach §5.3.6 |
| VCMIntegrator.cpp:762, 939, 1177 | rebuild emitter `RIG` from `lightStart.normal` | upstream stored shading | geometric needed for emission Jacobian | **FIX_BOTH (carrier-extension)** | mirror of BDPT 5070 |
| [PathVertexEval.h:115](src/Library/Utilities/PathVertexEval.h:115), 176, 261, 306, 386 | `PopulateRIGFromVertex` | only writes `vNormal` | must also write `vGeomNormal` | **FIX_BOTH (carrier-extension)** | central propagator for every BDPT/VCM downstream site |
| [DistributionTracingShaderOp.cpp:88](src/Library/Shaders/DistributionTracingShaderOp.cpp:88), 108, 182 | irradiance-cache key normal | shading | shading is PBRT-default, but bumped surfaces fragment cache | INVESTIGATE | PBRT 4e §15.5.4 |
| FinalGatherShaderOp.cpp:131, 139, 223, 252, 376, 382, 402, 691 | irradiance-cache key + translational gradient `cross(N₁,N₂)` | shading | same as Distribution | INVESTIGATE | Ward 1988 |
| AmbientOcclusionShaderOp.cpp:64, 84, 174 | irradiance-cache key | shading | shading | KEEP | Ward 1988 |
| [EmissionShaderOp.cpp:63](src/Library/Shaders/EmissionShaderOp.cpp:63), 110 | NEE `p_light = d²/(A·cosLight)` MIS denom | shading | geometric | **FIX_GEOM** | PBRT 4e §13.6.4 |
| EmissionShaderOp.cpp:51, 102 | `pEmitter->emittedRadiance(...,vNormal)` | shading | depends on emitter | INVESTIGATE | most area emitters take geometric |
| [TransparencyShaderOp.cpp:56](src/Library/Shaders/TransparencyShaderOp.cpp:56), 90 | one-sided alpha-mask `dot(rayDir,N)>0` | shading | geometric | **FIX_GEOM** | front/back is geometric |
| AreaLightShaderOp.cpp:111, 192 | Phong-style spotlight `(N+1)·cos^N` shading | shading | shading | KEEP | Phong shading is shading-normal by definition |
| DirectVolumeRenderingShader.cpp:240, 392 | volume-box entering test `dot(N,dir)` | shading | geometric | **FIX_GEOM** | bumpy box edge case |
| DirectVolumeRenderingShader.cpp:286–454 | synthesised volume `vNormal` from gradient | producer | producer | KEEP | iso-surface gradient *is* the normal |
| BSSRDFEntryAdapters.h:57, 72, 107, 124, 155 | `cosTheta = dot(wLight, ri.vNormal)` for `Sw` Fresnel weight at SSS entry | shading | shading for Fresnel angular dependence | KEEP | PBRT 4e §11.4.2 |
| FinalGatherInterpolation.h:71, 84, 109, 146 | translational gradient cross product | shading | geometric (sample-locality basis) | **FIX_GEOM** | Ward & Heckbert 1992 |
| SubSurfaceScatteringShaderOp.cpp:177, DonnerJensenSkinSSSShaderOp.cpp:799 | uniform-random-point synthetic `RIG` | analytic | both | KEEP | mesh `UniformRandomPoint` returns geometric anyway |

Tier-1 path-space bugs (Jacobian, NEE MIS, medium-stack, BSSRDF gate,
Transparency culling) are consolidated in the priority list. All require
`BDPTVertex.geomNormal` + `PathVertexEval::PopulateRIGFromVertex` carrier
extension (§6); without it, consumers read default-init zeros.

---

## 2. Photon mapping

PhotonMap.h, all four PhotonMap variants, both PhotonTracer headers,
GlobalPelPhotonTracer, IrradianceCache, SMSPhotonMap.

| file:line | bucket | RISE uses | literature says | verdict | reference |
|---|---|---|---|---|---|
| [PhotonMap.h:678](src/Library/PhotonMapping/PhotonMap.h:678) | thin-surface "ellipsoid" clamp during gather | shading | geometric | **FIX_GEOM** | Jensen 2001 §6.1 — same-physical-surface filter |
| PhotonMap.h:674 | photon hemisphere reject `dot(photonDir,N) > 0.001` | shading | shading (couples to BSDF eval) | KEEP | Jensen 2001 §6.1; reject cone is BSDF-frame |
| GlobalPelPhotonMap.cpp:96, TranslucentPelPhotonMap.cpp:90 | `brdf.value(vNormal, ri)` at gather | shading | shading | KEEP | PBRT 4e §15.5.1 |
| GlobalSpectralPhotonMap.cpp:71, 123; CausticSpectralPhotonMap.cpp:71, 125 | thin-surface clamp (spectral) | shading | geometric | **FIX_GEOM** | mirror of PhotonMap.h:678 |
| GlobalPelPhotonTracer.cpp:115 | photon storage normal | shading | both (deposit shading; record geometric for thin-surface filter) | **FIX_BOTH (record extension)** | Jensen 2001 §6.1 — record needs both |
| PhotonTracer.h:184–186; SpectralPhotonTracer.h:136–138; SMSPhotonMap.cpp:446–448 | synthesised emitter `RIG` only sets `vNormal` | shading slot | both | **FIX_BOTH** (one-line mirror set) | analytic luminaires have shading == geometric anyway, but plumbing is brittle |
| IrradianceCache.cpp:24–39, 159 (and headers) | Ward IC weight `Dot(norm, vNormal)` | shading | shading; bumped surfaces fragment cache | KEEP / INVESTIGATE | Ward 1988 |
| [SMSPhotonMap.cpp:327](src/Library/Utilities/SMSPhotonMap.cpp:327) | `cosI = Dot(rayDir, vNormal)` for `bEntering` | shading | geometric | **FIX_GEOM** | side test is geometric |
| [SMSPhotonMap.cpp:337](src/Library/Utilities/SMSPhotonMap.cpp:337) | photon chain vertex `v.normal := vNormal` | shading | both (validator needs geometric) | **FIX_BOTH (record extension)** | see SMS section |

Critical photon-map finding: `SMSPhotonChainVertex` has no `geomNormal` slot,
so reconstruction at `ManifoldSolver.cpp:5046` falls back to shading — silently
disabling the `ValidateChainPhysics` fix on every photon-aided chain, exactly
the regime the photon path was added to rescue
([CLAUDE.md](CLAUDE.md) High-Value Facts).

---

## 3. Materials / BSDFs / BSSRDFs

PolishedSPF, PolishedBRDF, IsotropicPhongSPF, DataDrivenBSDF, RandomWalkSSS,
BSSRDFSampling, IridescentPainter.

| file:line | bucket | RISE uses | literature says | verdict | reference |
|---|---|---|---|---|---|
| PolishedSPF.cpp:137, 197, 248, 349, 375; PolishedBRDF.cpp:50, 68 | dielectric front/back flip drives Fresnel + `CalculateRefractedRay` | shading | geometric for the side; shading for the lobe | **FIX_BOTH** | PBRT 4e §9.5 / Mitsuba `dielectric` |
| IsotropicPhongSPF.cpp:85, 157, 200, 237 | side test + lobe both shading (consistent) | shading | shading lobe; geometric side gate | INVESTIGATE | strict PBRT prescription is FIX_BOTH; current is internally consistent |
| DataDrivenBSDF.cpp:124–125 | tabulated BRDF θ_i, θ_o lookup | shading | shading | KEEP | PBRT 4e §9.1 |
| IridescentPainter.cpp:48, 56 | view-cosine for thin-film tint | shading | shading | KEEP | thin-film tint is BSDF-coupled |
| [RandomWalkSSS.cpp:64](src/Library/Utilities/RandomWalkSSS.cpp:64) | walk entry refraction at boundary | shading | geometric (boundary side) + shading (refracted dir) | **FIX_BOTH** | PBRT 4e §11.4 / random-walk SSS |
| RandomWalkSSS.cpp:283 | walk exit refraction / TIR | shading | geometric | **FIX_BOTH** | same as :64 |
| BSSRDFSampling.cpp:42–49 | probe axis from `ri.onb` (shading-built); `cosExit` Fresnel | shading | shading axis (PBRT §11.4.2); geometric `cosExit` | **FIX_BOTH** | PBRT 4e §11.4.2 |
| BSSRDFSampling.cpp:150 | candidate-entry record `h.normal := vNormal` | shading | both | **FIX_BOTH (record extension)** | downstream consumers split on use |

Materials Tier-1 bugs: dielectric side flip (PolishedSPF/PolishedBRDF) and
RandomWalkSSS boundary refraction. Both use `n = (dot(vN,dir)>0)?-vN:vN` —
PBRT 4e §9.5 and Mitsuba `dielectric.cpp` use geometric for the side test
and shading for the lobe / refracted direction.

---

## 4. SMS solver + IOR / Optics / Medium-transport utilities

ManifoldSolver, Optics, IORStackSeeding, MediumTransport, GeometricUtilities.

| file:line | bucket | RISE uses | literature says | verdict | reference |
|---|---|---|---|---|---|
| ManifoldSolver.cpp:1726 | `ValidateChainPhysics` side test | **vGeomNormal** with shading fallback | geometric | KEEP | the model fix |
| ManifoldSolver.cpp:632–643 | half-vector constraint `C(x)` | shading | shading | KEEP | Zeltner 2020 §3 — constraint follows BSDF |
| ManifoldSolver.cpp:1063–1090, 2226–2401 | derivative cache `dndu/dndv` | shading-field deriv | shading-field deriv (constraint coupled) | KEEP / INVESTIGATE | Newton consistent with constraint; validator deliberately uses a different field |
| ManifoldSolver.cpp:2043 | re-snap fallback writes shading into `geomNormal` | shading proxy | geometric | **FIX_GEOM (low priority)** | rare path; comment acknowledges proxy |
| [ManifoldSolver.cpp:3353–3354](src/Library/Utilities/ManifoldSolver.cpp:3353), 3672–3674 | `cosI = Dot(dir, mv.normal)` → `bEntering` in BuildSeedChain[Branching] | shading | geometric | **FIX_GEOM** | upstream half of the validator-fix bug |
| ManifoldSolver.cpp:3416–3424, 3488–3490 | refract / reflect direction | shading | shading | KEEP | BSDF-coupled |
| ManifoldSolver.cpp:3843–3850 | uniform-area resample replaces `vi.normal` but not `vi.geomNormal` | shading; geomNormal stale | both | **FIX_GEOM** | one-line fix |
| ManifoldSolver.cpp:5056–5067, 5805–7123 | photon → manifold-vertex reconstruction; `geomNormal := pv.normal` (shading proxy) | shading proxy | geometric | **FIX_BOTH (record extension)** | depends on `SMSPhotonChainVertex` carrying geometric |
| ManifoldSolver.cpp:4028, 4149 | Fresnel `cosI` | shading | shading | KEEP | matches BSDF Fresnel |
| ManifoldSolver.cpp:3944, 3990 | path-integral chain cosine `Dot(chain[i].normal, dir)` | shading | geometric (path-space measure) | INVESTIGATE | Veach §8.2; small effect on bumpy chains |
| Optics.cpp:20–159 | `CalculateReflectedRay` / `CalculateRefractedRay` / `DielectricReflectance` | caller-supplied | caller-supplied | KEEP | library function; correctness lives at every call site |
| [IORStackSeeding.h:142–144](src/Library/Utilities/IORStackSeeding.h:142) | `cosN = Dot(vNormal, probe.Dir())` for "are we exiting an object" | shading | geometric | **FIX_GEOM** | side test |
| LightSampler.cpp:247, 394 | shadow-walk medium stack push/pop | shading | geometric | **FIX_GEOM** | mirror of BDPT 1114 |
| MediumTransport.cpp:131, 168 | volumetric synthesised RIG | n/a | n/a | KEEP | not a surface event |
| GeometricUtilities.cpp:355–372 | local parameter name `vNormal` in sphere-UV helper | n/a | n/a | KEEP | parameter, not field |
| BumpMap.cpp / NormalMap.cpp | producers; do NOT touch `vGeomNormal` | producer | producer | KEEP | source of the divergence |

SMS/utilities Tier-1: IORStackSeeding exit test, `BuildSeedChain[Branching]`
entering decision, LightSampler shadow-walk medium stack, and
`BuildSeedChainBranching` resample's missing `vi.geomNormal` set. Together
these are the *upstream* half of the validator-fix bug — the validator
correctly rejects chains whose `etaI/etaT` were corrupted by these side-test
errors, but the decision belongs at the source.

---

## 5. Lights / Objects / CSG / Rasterizer / Volume / Parser

LightSampler, Object, CSGObject, all *Rasterizer files, OIDNDenoiser,
Volume operators, AsciiSceneParser, UV generators, point/directional/spot
lights.

| file:line | bucket | RISE uses | literature says | verdict |
|---|---|---|---|---|
| AmbientLight.h:79 | BSDF eval at ambient sample | shading | shading | KEEP |
| DirectionalLight.cpp:48, PointLight.cpp:59, SpotLight.cpp:71 | hemisphere visibility cull | shading | shading | KEEP |
| LightSampler.cpp:1063, 1142, 1198, 1330, 1478, 1531, 1579, 1689 | NEE BSDF cosine `Dot(vToLight, vNormal)` and BVH cluster orientation | shading | shading | KEEP |
| LightSampler.cpp:856, 1246, 1624 | synthesised emitter RIG only sets `vNormal` | analytic | both | **FIX_BOTH** (mirror set; trivial) |
| Object.cpp:312 | `pUVGenerator->GenerateUV(pt, vNormal, …)` | shading | geometric (face-projection axis) | **FIX_GEOM (low priority)** |
| Object.cpp:316–365; CSGObject.cpp all 22 sites | inverse-transpose to world space; CSG composition (UNION/INTERSECTION/SUBTRACTION) | both, paired symmetrically | both | KEEP — verified pair-symmetric |
| InteractivePelRasterizer.cpp:295, 342, 393 | preview BSDF eval | shading | shading | KEEP |
| BDPTPelRasterizer.cpp:157, BDPTSpectralRasterizer.cpp:140/359, VCMPelRasterizer.cpp:365, VCMSpectralRasterizer.cpp:384, OIDNDenoiser.cpp:757 | OIDN albedo / normal AOV | shading | shading | KEEP |
| Volume/*Operator.h | density-gradient `vNormal` field on `IGradientEstimator::GRADIENT` (separate struct) | n/a | n/a | KEEP — false-positive grep hit |
| AsciiSceneParser.cpp:4545 | doc string only | n/a | n/a | KEEP |
| IUVGenerator.h, BoxUVGenerator, CylindricalUVGenerator, SphericalUVGenerator | parameter name; UV gen reads caller-supplied normal | shading per Object.cpp:312 | geometric on meshes (face-projection) | INVESTIGATE — bump on mesh + UV-gen override is rare |

CSG composition is **clean**: every one of the 22 sites in
[CSGObject.cpp](src/Library/Objects/CSGObject.cpp) pairs `vNormal` and
`vGeomNormal` on the same source slot with the same sign, including the
subtraction `-` flip and the world-space inverse-transpose at 458–461. No CSG
bug found.

---

## 6. Carrier-extension prerequisites

Several "FIX_GEOM" / "FIX_BOTH" sites are blocked by data structures that carry
only one normal. The prerequisite refactor is mechanical but invasive:

- **[`BDPTVertex`](src/Library/Shaders/BDPTIntegrator.cpp:1727)** — add
  `Vector3 geomNormal`, populate at every construction site (1727, 2860, 5406,
  spectral mirrors), copy through `PathVertexEval::PopulateRIGFromVertex` so
  every downstream BSDF / NEE / MIS site sees both normals on `RIG`.
- **`SMSPhotonChainVertex`** — add `Vector3 geomNormal`. Populate at
  [SMSPhotonMap.cpp:337](src/Library/Utilities/SMSPhotonMap.cpp:337); consume
  at `ManifoldSolver.cpp:5046–5067` and the photon-aided sites 5805–7123.
- **Photon record (`IrradPhoton` and spectral analogues)** — currently stores
  `Ntheta/Nphi` for one normal only. Either add a second pair or store the
  geometric and recover shading from a re-cast at gather (latter avoids per-photon
  growth but doubles intersection cost).
- **`BSSRDFSampling::SampleResult.h.normal`** — add `geomNormal` field for
  symmetric propagation through entry-RI rebuild at PT/BDPT BSSRDF sites.
- **Synthesised emitter RIGs** (PhotonTracer.h:184, SpectralPhotonTracer.h:136,
  SMSPhotonMap.cpp:446, LightSampler.cpp:856/1246/1624) — one-line
  `rig.vGeomNormal = lumNormal;` mirror set; analytic luminaires have
  shading == geometric so this is just plumbing hygiene.

---

## Fixes recommended (priority-ordered)

### Tier 1 — high-confidence correctness bugs

1. **BDPT/VCM `cosAtGen` and `SolidAngleToArea` Jacobian** uses shading normal
   ([BDPTIntegrator.cpp:1750](src/Library/Shaders/BDPTIntegrator.cpp:1750), 2883,
   5424; VCM mirrors 762/939/1177). Biases every interior path-pdf factor.
   Requires `BDPTVertex.geomNormal` extension.
2. **PT NEE area-pdf MIS denominator** uses shading normal
   (PT 1413/3231/4499; [EmissionShaderOp.cpp:63/110](src/Library/Shaders/EmissionShaderOp.cpp:63)).
   Biases BSDF-vs-light MIS at every sampled emission hit.
3. **`SMSPhotonChainVertex` lacks `geomNormal`** —
   [SMSPhotonMap.cpp:337](src/Library/Utilities/SMSPhotonMap.cpp:337). Silently
   disables the `ValidateChainPhysics` fix on every photon-aided chain in
   exactly the regime photons are supposed to help.
4. **`BuildSeedChain` and `BuildSeedChainBranching` entering/exiting decision**
   on shading normal ([ManifoldSolver.cpp:3353/3672](src/Library/Utilities/ManifoldSolver.cpp:3353)).
   Upstream half of the validator-fix bug; produces wrong `etaI/etaT`.
5. **`SMSPhotonMap` photon emit side test** on shading normal
   ([SMSPhotonMap.cpp:327](src/Library/Utilities/SMSPhotonMap.cpp:327)).
   Wrong `bEntering` propagates into the chain record on bumpy reflections.
6. **`IORStackSeeding::SeedFromPoint` exit test** on shading normal
   ([IORStackSeeding.h:142](src/Library/Utilities/IORStackSeeding.h:142)).
   Photons emitted from inside a bumpy enclosure can mis-seed the stack.
7. **BDPT connection-walk and LightSampler shadow-walk medium-stack push/pop**
   on shading normal (BDPT 1114/1251; LightSampler 247/394). Mis-orders medium
   stack on connection rays through dielectrics.
8. **PolishedSPF / PolishedBRDF dielectric side flip** uses shading normal
   (PolishedSPF.cpp 137/197/248/349/375; PolishedBRDF.cpp 50/68). Wrong
   front-back decision can invert Fresnel direction or spuriously trigger TIR.
9. **PT/BDPT BSSRDF entry front-face gate** on shading normal (PT 1508/1623/3314/3435,
   BDPT 1923/1986/3050/3106/5588/5643). Leaks subsurface contribution on bumpy
   skin / leaves where the comment claims it defends against exactly that.
10. **`TransparencyShaderOp` one-sided alpha** on shading normal
    ([TransparencyShaderOp.cpp:56/90](src/Library/Shaders/TransparencyShaderOp.cpp:56)).
    Speckle along bumpy foliage / hair-card silhouettes.
11. **`RandomWalkSSS` boundary refraction** on shading normal
    ([RandomWalkSSS.cpp:64/283](src/Library/Utilities/RandomWalkSSS.cpp:64)).
    Energy leakage on bumpy SSS surfaces.

### Tier 2 — likely correctness issues, lower magnitude

12. Photon-map thin-surface clamp (PhotonMap.h:678 + spectral mirrors). Affects
    caustic-on-bumpy-floor reconstruction.
13. `BuildSeedChainBranching` resample (ManifoldSolver.cpp:3843–3850) misses
    `vi.geomNormal` set; validator silently disabled for reflection-branch
    resamples.
14. DirectVolumeRendering box face entering test (240/392).
15. Object.cpp:312 UV-gen projection axis. Mesh + override UV-gen + bump only.

### Tier 3 — investigation / consistency

16. SMS receiver-frame `N` parameter conflates BSDF-cosine and chain-topology
    (PT 2100/4585/3703).
17. ManifoldSolver path-integral chain cosines (3944/3990).
18. Irradiance-cache key fragmentation under heavy bump (DistributionTracing,
    FinalGather, FinalGatherInterpolation gradient cross-products) — perf,
    not correctness.
19. `IsotropicPhongSPF` side test internally consistent; matches Mitsuba's
    `roughdielectric`.
20. Synthesised emitter RIG `vGeomNormal` defaulting (LightSampler 856/1246/1624,
    PhotonTracer.h, SpectralPhotonTracer.h, SMSPhotonMap.cpp:446) — plumbing
    hygiene.

### Verified KEEP

BSDF eval / sample / pdf in materials and BSDFs (PBRT 4e §9.1); OIDN AOV
writers; CSG composition (22/22 paired); BumpMap / NormalMap producers; volume
gradient operators (false-positive grep hits); `Optics.cpp` library functions
(caller-driven).

---

## Implementer notes

- Carrier-extension fixes must land in this order: field added → producer
  populates → consumer reads. Add a `SquaredModulus(geomNormal) <= NEARZERO`
  fallback at every new consumer (mirrors ValidateChainPhysics).
- For area-pdf Jacobian fixes, the receiver-side geometric cosine and the
  BSDF-side shading cosine coexist on the same vertex record — do not collapse.
- Verify with the [bdpt-vcm-mis-balance](docs/skills/bdpt-vcm-mis-balance.md)
  recipe: identical PT/BDPT/VCM means, max ratios within MC noise, no
  splotches scaling with bump amplitude. A bump-sweep on a Lambertian plane
  is the cheapest discriminator — Tier-1 bugs scale monotonically with
  `disp_scale`.
