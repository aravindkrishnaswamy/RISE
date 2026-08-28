//////////////////////////////////////////////////////////////////////
//
//  HairBSDF.h - Chiang et al. 2016 near-field hair / fur BCSDF.
//
//  Model per Chiang, Bitterli, Tappan & Burley, "A Practical and
//  Controllable Hair and Fur Model for Production Path Tracing",
//  EGSR 2016 (the Disney production model).  Structure validated
//  against PBRT-v4's `HairBxDF` (Apache-2.0); this is an independent
//  implementation adapted to RISE's IBSDF / ISPF interfaces, spectral
//  (NM) pipeline and IPainter / IScalarPainter pipe discipline.
//  Design record: docs/HAIR_FUR_DESIGN.md.
//
//  ------------------------------------------------------------------
//  1.  FIBRE FRAME AND THE NEAR-FIELD OFFSET h
//  ------------------------------------------------------------------
//
//  The BCSDF lives in the fibre frame:
//
//      x = ri.onb.u()   fibre tangent (root -> tip)
//      y = ri.onb.v()
//      z = ri.onb.w()   shading normal (== ri.vNormal; Object.cpp
//                       builds the ONB from vNormal on both the
//                       CreateFromW and CreateFromWU branches)
//
//  (u, v, w) is right-handed (u x v = w), matching PBRT's (x, y, z)
//  shading frame, so a direction maps to PBRT's local coordinates
//  component-for-component.  theta is measured FROM THE NORMAL PLANE
//  (sin theta = w.x), phi = atan2(w.z, w.y).
//
//  The near-field offset h in [-1, 1] is the signed distance from the
//  fibre axis at which the ray crosses the fibre's width.  It rides in
//  the intersection's across-width texture coordinate:
//
//      h = 2 * ri.ptCoord.y - 1        (clamped to +/- 0.9995)
//
//  which is PBRT's `Curve` convention (h = 2v - 1).  Geometry that is
//  not a fibre (a sphere or a mesh, as used by the unit tests) still
//  yields a well-defined h; the model degrades to "the hair BCSDF
//  evaluated at that offset", which is exactly what a fibre would do.
//
//  ! GEOMETRY WITH NO UV CHANNEL IS A TRAP.  `ptCoord` defaults to
//  (0, 0), so h resolves to -1 -- the fibre EDGE -- at every hit.
//  cos(gamma_o) is then 0, the Fresnel term saturates to 1, and the
//  model degenerates to a colourless white mirror (all transmissive
//  orders vanish).  The +/- 0.9995 clamp keeps a sliver of colour
//  rather than a hard white, but the appearance is still near-grazing
//  everywhere and is NOT what the author asked for.  Until
//  `hair_geometry` supplies real across-width coordinates, bind hair
//  only to geometry that generates a v coordinate.
//
//  The fibre TANGENT must be a real, coherent, geometry-supplied
//  direction for a hair render to look right, and `hair_geometry`
//  (HairGeometry) supplies exactly that: alongside
//  `bShadingTangentFromGeometry=true` it also sets `bHasShadingTangent`
//  and writes the OBJECT-space fibre tangent into `vShadingTangent`
//  (RayIntersectionGeometric.h).  `Object::IntersectRay` (Object.cpp,
//  the `bShadingTangentFromGeometry` branch) and, for a CSG-composed
//  hair fibre, `CSGObject::IntersectRay`'s byte-duplicate block each
//  promote it one level -- forward matrix, like `vTangent`, NOT
//  inverse-transpose, because a tangent is a direction ALONG the
//  surface, not a normal -- and WRITE THE PROMOTED VALUE BACK into
//  `vShadingTangent` in place, so a further-nested CSG parent sees the
//  field one promotion short of world space and finishes the job
//  itself.  Once in world space it is projected into the shading-
//  normal plane and handed to `CreateFromWU` to build the ONB.  The
//  legacy world-X-projection path (`bShadingTangentFromGeometry=true`
//  WITHOUT `bHasShadingTangent`, still used by SDFGeometry's
//  heightfield mode) remains the fallback: reached whenever no
//  geometry supplies a tangent, and whenever a supplied tangent turns
//  out degenerate (near-parallel to the normal, or collapsed to
//  near-zero by a singular transform).  The separate `vTangent` /
//  `bHasTangent` pair (RayIntersectionGeometric.h) is a DIFFERENT
//  per-vertex tangent, glTF-only and consumed by the normal-map
//  modifier -- unrelated to the fibre tangent described here.  This
//  file only reads `ri.onb.u()`, so it picks up whatever the fibre-
//  tangent plumbing above produced -- PROVIDED nothing downstream
//  rebuilds the ONB from the normal alone afterward.  RESOLVED, residual
//  wave 2 item A, 2026-08-27: `NormalMap::Modify` (NormalMap.cpp:172) and
//  `BumpMap::Modify` (BumpMap.cpp:69) used to call the unconditional
//  `ri.onb.CreateFromW(ri.vNormal)` after perturbing the normal,
//  discarding whatever tangent was in `ri.onb.u()` -- including the hair
//  fibre tangent.  Both now check `ri.bHasShadingTangent` first and, when
//  set, project the CURRENT `ri.onb.u()` into the perturbed normal's
//  plane and rebuild via `CreateFromWU` -- the same idiom `GlintModifier`
//  already used for its facet tilt -- falling back to `CreateFromW` only
//  on a degenerate projection.  When `bHasShadingTangent` is false the
//  new branch is skipped and the rebuild is byte-identical to before.
//  See docs/HAIR_FUR_DESIGN.md section 4.1.
//
//  ------------------------------------------------------------------
//  2.  THE kray / pdf CONVENTION  (the load-bearing reconciliation)
//  ------------------------------------------------------------------
//
//  RISE's convention, verified against LambertianSPF/LambertianBRDF
//  (kray = reflectance, pdf = cos/pi, f = reflectance/pi) and against
//  the PT integrator (PathTracingIntegrator.cpp:2968 / :3285 multiply
//  `kray` straight into throughput with no further cosine or pdf):
//
//      ScatteredRay::kray / krayNM  ==  f_RISE * |wi . N| / pdf
//      IBSDF::value(wi, ri)         ==  f_RISE                       (NEE
//          multiplies it by |dot(wi, ri.vNormal)| and divides by the
//          light pdf -- LightSampler::EvaluateDirectLighting's three
//          `cosSurface` / `cosEnv` sites, which take the ABSOLUTE value
//          for a material that reports IMaterial::ScattersFullSphere();
//          HairMaterial does.  That capability is REQUIRED for this
//          identity to hold on the transmissive half -- see section 5's
//          "NEE REACHES THE TRANSMISSIVE HALF" entry.)
//
//  NOTE THE COSINE.  |wi . N| is the SHADING cosine (PBRT's
//  AbsCosTheta in the shading frame).  It is NOT cos(theta_i): the
//  fibre's theta is measured from the NORMAL PLANE (the plane
//  perpendicular to the fibre tangent), so cos(theta_i) is 1 exactly
//  where wi is perpendicular to the tangent, while |wi . N| is 0
//  wherever wi is perpendicular to the shading normal.  The two are
//  different functions and only the shading one appears here.
//
//  The Chiang BCSDF, on the other hand, is normalised WITHOUT a
//  cosine: the bare lobe sum obeys
//
//      INTEGRAL_sphere fsum(wo -> wi) dwi = 1     (sigma_a = 0)
//
//  because a fibre's scattering is defined per unit projected fibre
//  cross-section, not per unit projected surface area.  PBRT reconciles
//  the two by dividing `fsum` by the shading |wi . N| inside `f`, so
//  that the integrator's own |wi . N| cancels it.  RISE needs the
//  identical reconciliation, and it is the CONSTRAINT this file is
//  built around:
//
//      CONSTRAINT:   value(wi, ri) = fsum(wo, wi) / |dot(wi, N)|
//                    kray          = fsum(wo, wi) / pdf
//
//  With those two, NEE evaluates  L * value * |cos| / p_light
//  = L * fsum / p_light, and BSDF sampling accumulates fsum / pdf --
//  both are the correct fibre estimators, and the white-furnace test
//  (mean of kray over SPF samples == 1, and the uniform-sphere
//  estimate of INTEGRAL value*|cos| == 1) proves it.  `HairBSDFTest`
//  asserts exactly these two quantities.
//
//  The 1/|wi . N| factor is singular on the great circle where
//  wi . N == 0 -- the circle spanned by the fibre tangent u and the
//  bitangent v, which CONTAINS the fibre tangent.  (It is emphatically
//  NOT the "normal plane" of hair terminology, the v-w plane
//  perpendicular to the tangent; the two are perpendicular to each
//  other.)  The reciprocal is applied only when |wi . N| > 0 (PBRT does
//  the same); every consumer that uses `value` multiplies the same
//  cosine straight back, so the product stays finite and the
//  cancellation is exact.
//
//  ------------------------------------------------------------------
//  3.  SAMPLING PDF IS DELIBERATELY ACHROMATIC
//  ------------------------------------------------------------------
//
//  `ISPF::EvaluateKrayNM` is handed (ri, outDir, rayType, nm) but NOT
//  the hero wavelength nor the hero sampling pdf -- yet its contract is
//  to return exactly the krayNM that `ScatterNM` produced at the hero,
//  re-evaluated at `nm` (PathTracingIntegrator.cpp:4700-4726 divides
//  the BRDF fallback by the hero's `pS->pdf`).  That is only
//  reconstructible if the sampling pdf is wavelength-independent.
//
//  It therefore is, by construction:
//
//    * the lobe-selection PMF A_p is built from an ACHROMATIC proxy
//      absorption -- the component-wise MINIMUM of the RGB sigma_a
//      triple.  Minimum (i.e. MAXIMUM transmittance) buys a genuine
//      ALGEBRAIC BOUND, not merely a heuristic margin.  T_proxy >=
//      T_lambda; ap[0] = f is independent of T and every ap[p >= 1] is
//      strictly increasing in T, so ap_lambda[p] <= ap_proxy[p]
//      termwise, so
//          fsum_lambda / pdf = S * dot(w, ap_lambda) / dot(w, ap_proxy)
//                            <= S = sum_p ap_proxy[p] <= 1,
//      the last step because the four orders telescope to exactly 1 at
//      T = 1 and S falls monotonically in T.  kray therefore provably
//      lies in [0, 1] for every channel and for any non-dispersive
//      wavelength -- no wavelength can fire by landing in a starved
//      lobe.  The full derivation is at HairScatteringBase::SigmaAProxy.
//    * the azimuthal / longitudinal geometry used by `Pdf` uses the
//      achromatic reference IOR `ior->GetValuesAt(ri).v[0]`.
//
//  `valueNM` remains FULLY spectral (sigma_a(lambda) and, if the `ior`
//  painter is dispersive, eta(lambda)).  A non-dispersive `ior` -- the
//  default and the overwhelmingly common case -- makes the sampling
//  geometry and the evaluation geometry bit-identical.  A dispersive
//  `ior` costs variance, never bias: f/pdf is unbiased for ANY strictly
//  positive pdf.  This is the accepted HWSS approximation recorded in
//  docs/HAIR_FUR_DESIGN.md section 3.3.
//
//  ------------------------------------------------------------------
//  3b.  THE MEDULLA  (Yan et al. 2017; Phase 3, OFF by default)
//  ------------------------------------------------------------------
//
//  Animal fur is not a solid cortex cylinder: it carries a large,
//  strongly SCATTERING core -- the medulla -- of radius ratio
//  kappa = r_medulla / r_fibre (up to ~0.9 in rabbit; ~0 in human
//  hair).  Light that crosses the medulla is diffused, which is what
//  gives fur its soft, saturated look; a Chiang-only fur render reads
//  as "thin shiny hair" in close-up.  Yan, Tseng, Jensen & Ramamoorthi
//  add two SCATTERED lobes -- TTs and TRTs -- driven by precomputed
//  medulla scattering profiles.
//
//  Three optional `IScalarPainter` slots turn it on:
//
//      medulla_ratio    kappa in [0, 0.95].  0 (the DEFAULT) = off.
//      medulla_scatter  sigma_m >= 0, the medulla scattering
//                       coefficient in per-fibre-diameter units --
//                       the same units `sigma_a` uses.
//      medulla_g        Henyey-Greenstein anisotropy in (-1, 1).
//
//  All three are read ACHROMATICALLY (`GetValuesAt(ri).v[0]`, never
//  `GetValueAtNM`), exactly like `beta_m` / `alpha` / `ior`, because
//  section 3's wavelength-independent-pdf contract depends on it.
//
//  ! kappa == 0 IS A HARD SHORT CIRCUIT, NOT A LIMIT.  `Resolve`
//  latches `medullaActive`, and every downstream loop, array and
//  branch is bounded by it, so a medulla-off hit executes the
//  pre-Phase-3 arithmetic instruction for instruction -- not "the same
//  thing plus some zeros".  HairBSDFTest group 15 pins that two ways:
//  15a against literals captured from the Phase-2 build, 15b against
//  a sibling material inside the same binary.
//
//  THE ENERGY BOOKKEEPING (why the furnace gate is structural).  The
//  medulla does not add energy; it SPLITS what the TT and TRT orders
//  already carried.  With `q = exp(-tau * sqrt(1 - b^2))` the
//  probability that one medulla crossing is ballistic (tau = the
//  medulla's diametral optical depth, b = the chord's impact parameter
//  in medulla radii -- see HairMedullaProfile.h):
//
//      A_TT   = (1-f)^2 T   * q        A_TTs  = (1-f)^2 T   * (1-q)
//      A_TRT  = (1-f)^2 T^2 f * q^2    A_TRTs = (1-f)^2 T^2 f * (1-q^2)
//
//  (TT crosses the medulla once, TRT twice.)  Each pair sums to the
//  pre-Phase-3 A_p, so `sum_p A_p` -- the quantity the white furnace
//  measures and the quantity section 3's bound leans on -- is
//  unchanged TO ROUNDING.  (Exactly, in the reals; in IEEE, `a*q` and
//  `a*(1-q)` are each rounded and `1-q` is itself rounded, so the sum
//  is `a * (1 + O(eps))`.  HairBSDFTest group 17 accordingly asserts
//  the identity at 1e-12, and group 18's `kray <= 1 + 1e-9` budgets
//  for the same rounding.)  Both scattered lobes are then given a longitudinal
//  M_p and an azimuthal N_p that each integrate to 1, so the sphere
//  integral is unchanged too.
//
//  THE SECTION-3 PROXY BOUND STILL HOLDS, and for the same reason.
//  Under section 3's own proviso -- a NON-DISPERSIVE `ior`, which is
//  what that bound is stated for -- `q` is the same number at every
//  wavelength, so every one of the six A_p above is still a product of
//  a T-independent factor with a strictly INCREASING function of T:
//  (1-q) and (1-q^2) are non-negative constants under that ordering.
//  Hence A_lambda[p] <= A_proxy[p] termwise for T_proxy >= T_lambda,
//  exactly as before, and `sum_p A_proxy[p]` is the same telescoping
//  sum that equals 1 at T = 1 and falls monotonically below it.  kray
//  therefore remains provably in [0, 1]: the medulla split needed NO
//  clamp and NO amendment beyond this paragraph.
//
//  ! DO NOT read "the medulla painters are read achromatically" as the
//  reason.  It is necessary but NOT sufficient.  `q` also depends on
//  eta, through gamma_t: `MakeMedulla` derives both the impact
//  parameter b = sin(gamma_t)/kappa and the optical depth
//  tau = sigma_m * 2 kappa / cos(theta_t) from the `Geom` its caller
//  built, and `EvalFsum` builds that `Geom` with the SPECTRAL eta when
//  the `ior` painter disperses.  A dispersive `ior` therefore makes q
//  per-wavelength -- exactly as it already makes `ap[0]` (the Fresnel
//  term) per-wavelength -- and falls under the SAME variance-not-bias
//  caveat section 3 already records for that case, not under a new
//  one.  The achromatic read of the three medulla painters is what
//  keeps `Pdf` reconstructible inside `EvaluateKrayNM`; it is not what
//  makes the bound true.
//
//  COST.  A medulla-ON hit pays one trilinear profile interpolation
//  (8 cells x 33 floats) per BSDF evaluation, plus two extra lobes in
//  every dot product.  The SAMPLING path pays neither unless it
//  actually draws a scattered lobe -- the profile is materialised
//  lazily there.  Measured on the 128x128 furnace groom at samples=64,
//  three runs each: 952 / 955 / 957 ms at kappa = 0 versus
//  1298 / 1293 / 1305 ms at kappa = 0.7 -- ~1.36x, comfortably inside
//  the paper's own "2-3x Chiang" estimate.
//  A medulla-OFF hit pays ONE predicted branch and nothing else.
//
//  WHAT IS SIMPLIFIED versus Yan 2017 -- the honest list lives in
//  tools/HairMedullaProfileGen.cpp's header (normal-incidence
//  simulation with runtime path stretching; non-absorbing medulla with
//  the cortex sigma_a applied across the whole chord; no cortex
//  re-refraction of the scattered part; histogram smoothing).  Two
//  more belong here, because they are runtime choices rather than bake
//  choices:
//    * THE RESIDUAL LOBE (p == kPMax, everything above TRT) is NOT
//      split.  Yan has no such bucket at all -- it is Chiang's
//      energy-conservation term -- and splitting it would need a
//      medulla crossing count that the lumped order does not have.
//      It keeps its full pre-Phase-3 attenuation.  The error that
//      leaves is small and bounded: at h = 0, theta_o = 0, eta = 1.55,
//      sigma_a = 0 the residual is 0.00206 of a total of 1, i.e.
//      0.2 %, and it falls to 0 at grazing where f -> 1.
//    * TRTs REUSES THE SINGLE-CROSSING PROFILE.  Its energy is
//      correctly `1 - q^2` (scattered on either crossing), but its
//      shape is looked up at the one-crossing tau.  At small tau the
//      dominant event really is a single scatter on one crossing; at
//      large tau both crossings have already converged to the same
//      diffusive profile.  The error lives in the middle.
//    * LONGITUDINAL BROADENING IS ADDITIVE IN VARIANCE.  A scattered
//      lobe uses M_p at `v[parent] + var_medulla` (the Gaussian-
//      convolution approximation), rather than a separately tabulated
//      longitudinal profile.  M_p is normalised at every v, so this
//      cannot leak energy.
//
//  ------------------------------------------------------------------
//  4.  COLOUR TIERS  (exactly one is active)
//  ------------------------------------------------------------------
//
//    1. `eumelanin` / `pheomelanin` concentrations (IScalarPainter):
//       sigma_a(lambda) = c_eu * eps_eu(lambda) + c_ph * eps_ph(lambda),
//       eps from the in-tree OMLC spectroscopy tables shared verbatim
//       out of BioSpecSkinData.h, normalised at a SINGLE anchor so the
//       550 nm value reproduces PBRT's per-unit-concentration GREEN
//       coefficient (0.697 eu / 0.400 ph) -- i.e. concentration ~1.3 is
//       brown-black hair, matching every published Chiang
//       parameterisation.
//       ! THE PBRT MATCH IS GREEN-ONLY.  R and B are then whatever the
//       measured OMLC curve says, and they do NOT reproduce PBRT's other
//       two constants (a differently-derived RGB triple, not the same
//       quantity read at a different wavelength): measured at
//       600 / 450 nm, eumelanin is +23.5 % / -5.6 % and pheomelanin
//       +24.2 % / +1.7 % against PBRT.  Spectral fidelity to the
//       measurement is the deliberate choice; a PBRT cross-render
//       matches in G and runs slightly warm in R.  Numbers and rationale
//       at kEumelaninSigmaAAt550 in the .cpp.
//    2. `sigma_a` directly (IScalarPainter): power users, measured data.
//    3. `color` (IPainter, Albedo kind): Chiang's inversion of the
//       multiple-scattering-averaged reflectance,
//       sigma_a = (ln C / D(beta_n))^2, applied per wavelength in the
//       NM path and per channel in the RGB path.
//
//  ------------------------------------------------------------------
//  5.  CAVEATS A REVIEWER WILL ASK ABOUT
//  ------------------------------------------------------------------
//
//  * NOT RECIPROCAL.  The near-field h-conditioning and the cuticle
//    tilt break f(wo->wi) == f(wi->wo).  PBRT documents and accepts the
//    same.  BDPT/VCM therefore carry a small model-level bias on hair;
//    expect PT-vs-X agreement to be good but not MC-exact BEFORE
//    suspecting MIS (docs/HAIR_FUR_DESIGN.md section 6.2).
//  * NO GEOMETRIC-HORIZON GATE.  Every sibling material in this
//    directory gates sampled/queried directions against the geometric
//    normal.  Hair deliberately does not: a fibre scatters over the
//    FULL sphere (TT exits the far side by construction), so a
//    hemisphere gate would delete the transmission lobes and break the
//    furnace test.
//  * NOT DELTA.  `isDelta` is always false and `GetSpecularInfo` is
//    left at the non-specular default, so SMS never sees hair and the
//    MIS machinery treats every lobe as a continuum lobe.  The
//    beta_m / beta_n floors (section below) keep that honest.
//  * ALL LOBES ARE TAGGED `eRayReflection`.  Verified against
//    PathTransportUtilities.h:170-211: that tag routes to
//    `maxGlossyBounce` (default UINT_MAX) and, critically, is NOT a
//    delta-only tag -- GGX and Cook-Torrance use it for their rough
//    lobes too.  Tagging TT `eRayRefraction` would have burned
//    `max_transmission_bounce`, which authors legitimately cap low for
//    glass interiors.
//  * NEE REACHES THE TRANSMISSIVE HALF -- RESOLVED.  This entry used to
//    read "NEE CANNOT REACH THE TRANSMISSIVE HALF": `LightSampler`
//    rejected every shadow direction with `dot(wToLight, vNormal) <= 0`,
//    so the TT / TTs lobes were unreachable by next-event estimation
//    while the BSDF-sampling side still applied its `w_bsdf < 1` there,
//    leaving the two strategies summing to less than 1 over the whole
//    transmissive hemisphere.  The MECHANISM that closes it is
//    `IMaterial::ScattersFullSphere()` -- a per-material, default-FALSE
//    capability that `HairMaterial` overrides TRUE, and that
//    `LightSampler::EvaluateDirectLighting{,NM}` consults at each of its
//    three surface-cosine sites (delta light, mesh area light,
//    environment map) to use `|cos|` instead of the signed cosine.  The
//    MIS-partition derivation lives in the FULL-SPHERE NEE block comment
//    at the top of `EvaluateDirectLighting`; the short version is that
//    the env-NEE and light-table partner densities on the BSDF-sampling
//    side (PathTracingIntegrator's env-escape and emitter-hit blocks)
//    never had a shading-surface cosine gate of their own, so the
//    partition closes exactly once NEE is allowed to fire.
//
//    Measured, A/B on one build with the capability forced off vs on
//    (tests/HairRenderTest.cpp carries the full tables): sigma_a = 0 env
//    furnace 0.9885 -> 1.0016; the same furnace with `medulla_ratio 0.7`
//    0.8830 -> 1.0015 (the medulla lobes are broad, so far more of their
//    energy lands below the horizon); and a point-lit BACKLIT groom --
//    where essentially all transport is below-horizon -- recovers 6-8x,
//    with PT going from 15.5-16.3x under BDPT to 1.75x.
//
//    NON-full-sphere materials are BIT-IDENTICAL: where the capability
//    is false, each site's expression reduces textually to the
//    pre-change one.  EnvLightBalanceTest (all Lambertian) stays 116/116.
//
//    REMAINING SIBLING -- RESOLVED, residual wave 2 item D, 2026-08-27.
//    `DirectionalLight::ComputeDirectLighting{,NM}` carried the same
//    `fDot <= 0` gate, reached through `EvaluateDirectLighting`'s
//    Step-1 zero-exitance pass, so a groom lit by a `directional_light`
//    could not be lit from behind.  Closed exactly the way this entry
//    predicted: `ILight::ComputeDirectLighting{,NM}` gained a trailing
//    `const bool bFullSphereReceiver = false` parameter (defaulted, so
//    every untouched caller -- any out-of-tree light,
//    `LightManager::ComputeDirectLighting`'s own forwarding loop -- is
//    unaffected), threaded through all four light classes
//    (`DirectionalLight` reads it with `fabs`; `AmbientLight` no-ops it
//    because it applies no cosine gate at all; `PointLight` / `SpotLight`
//    no-op it because their own delta-light NEE is evaluated INLINE by
//    LightSampler's proportional-selection site, already
//    capability-gated there -- this virtual is reached only by Step 1's
//    zero-exitance sweep and BDPT's mirroring s==1 row, and neither
//    ever holds a point/spot light) and both real callers:
//    `LightSampler::EvaluateDirectLighting{,NM}` Step 1 (passes the
//    SAME `bFullSphere` its other NEE sites use) and
//    `BDPTIntegrator.cpp`'s s==1 zero-exitance row (derives its own
//    `eyeEnd.pMaterial->ScattersFullSphere()` -- BDPT's per-eye-vertex
//    material was already right there, no context gap after all).
//  * LEGACY COSINE-OMITTING `value` CONSUMERS.  Section 2's constraint
//    -- value == fsum / |wi . N| -- is only safe for a caller that
//    multiplies |wi . N| back.  Recounted honestly: three legacy shader
//    ops, SEVEN call sites total, do not:
//      - AmbientOcclusionShaderOp.cpp:139, :151, and :245 (the valueNM
//        twin) and FinalGatherShaderOp.cpp:215, :508 accumulate
//        `radiance * pBRDF->value(dir, ri)` with NO cosine, which on
//        hair is an unbounded-variance estimator (the 1/|wi . N| is
//        left uncancelled and diverges as wi approaches the
//        tangent/bitangent great circle).
//      - AreaLightShaderOp.cpp:137, :214 weight by `pow(fDot, pN)`
//        where fDot is the surface cosine and pN is the emitter's
//        Phong exponent; at the common `pN == 0` that term is 1, so
//        the surface cosine is missing entirely and 1/|wi . N| blows
//        up the same way.
//    Slice B (registration) DECISION: documented limitation.  Legacy
//    AO/FinalGather/AreaLight shader-ops pair badly with hair_material
//    (unbounded variance / missing-cosine blowup); NOT clamping
//    `value()` -- a clamp would bias every OTHER consumer (PT's NEE,
//    every BDPT/VCM connection strategy) just to protect three
//    deprecated opt-in paths that were never the target integrators for
//    hair (docs/HAIR_FUR_DESIGN.md section 6.1: PT is the target).
//    Authors should avoid binding hair_material under ao_shaderop /
//    final_gather_shaderop / arealight_shaderop; gating hair out of
//    those ops at the shader-op level, if it becomes a real complaint,
//    is future work, not part of this slice.
//  * REGRESSION SCENE -- RESOLVED as of Slice E (this file's docs
//    closeout).  `scenes/Tests/Hair/` now carries the real per-BSDF,
//    render-level regression scenes MATERIALS.md section 9 asked for:
//    `hair_furnace.RISEscene` (sigma_a=0 energy-conservation gate),
//    `hair_melanin_ladder.RISEscene` (three grooms differing only in
//    eumelanin), `hair_backlit_tt.RISEscene` (light behind the groom,
//    exercising the TT rim, and now the regression scene behind
//    HairRenderTest's test 4b full-sphere-NEE guard), and
//    `hair_styled.RISEscene`
//    (comb/clump/curl/frizz through the parser).  Phase 3 added
//    `fur_medulla.RISEscene` -- three backlit grooms differing ONLY in
//    `medulla_ratio` (0 / 0.5 / 0.9), the human-readable companion to
//    HairBSDFTest groups 16-19.  `tests/
//    HairRenderTest.cpp` renders the equivalent recipes in-process and
//    asserts on the resulting images (white furnace, HWSS invariant,
//    melanin-ladder monotonicity, and a loose-tolerance PT-vs-BDPT
//    sanity check citing the reciprocity caveat above).  The earlier
//    `scenes/Tests/ChunkCoverage/cc_hair_material.RISEscene` fixture,
//    HairBSDFTest.cpp's numeric coverage, and HairMaterialChunkTest.cpp's
//    registration coverage remain in place unchanged.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_BSDF_
#define HAIR_BSDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		//! The painter set a hair BRDF / SPF / Material is built from.
		//!
		//! Exactly ONE colour tier must be supplied:
		//!   tier 1 -- `eumelanin` and/or `pheomelanin` non-null
		//!   tier 2 -- `sigma_a` non-null
		//!   tier 3 -- `color` non-null
		//! The four appearance slots (`beta_m`, `beta_n`, `alpha`,
		//! `ior`) are all mandatory; the caller (Job / the chunk parser
		//! in a later slice) supplies inline-numeric defaults.
		//!
		//! Pointers, not references, purely so the optional tiers can be
		//! null.  HairBRDF / HairSPF addref every non-null pointer.
		struct HairPainters
		{
			const IScalarPainter*	eumelanin;		//!< tier 1, concentration (>= 0)
			const IScalarPainter*	pheomelanin;	//!< tier 1, concentration (>= 0)
			const IScalarPainter*	sigma_a;		//!< tier 2, absorption per unit fibre diameter
			const IPainter*			color;			//!< tier 3, artist reflectance (Albedo kind)

			const IScalarPainter*	beta_m;			//!< longitudinal roughness, clamped to [0.05, 1]
			const IScalarPainter*	beta_n;			//!< azimuthal roughness, clamped to [0.05, 1]
			const IScalarPainter*	alpha;			//!< cuticle scale tilt, DEGREES (2 is the usual default)
			const IScalarPainter*	ior;			//!< fibre IOR (1.55 is the usual default)

			//! Yan 2017 medulla (section 3b).  OPTIONAL -- unlike the
			//! four slots above, a null pointer here is a supported
			//! configuration meaning "no medulla", bit-identical to
			//! binding `medulla_ratio` to 0.
			const IScalarPainter*	medulla_ratio;	//!< kappa, clamped to [0, 0.95]; 0 = OFF (the default)
			const IScalarPainter*	medulla_scatter;//!< sigma_m >= 0, per fibre diameter
			const IScalarPainter*	medulla_g;		//!< HG anisotropy, clamped to (-1, 1)

			HairPainters() :
			  eumelanin( 0 ), pheomelanin( 0 ), sigma_a( 0 ), color( 0 ),
			  beta_m( 0 ), beta_n( 0 ), alpha( 0 ), ior( 0 ),
			  medulla_ratio( 0 ), medulla_scatter( 0 ), medulla_g( 0 )
			{}

			//! Number of colour tiers that have at least one slot bound.
			//! Must be exactly 1.  When it is not, HairBRDF / HairSPF log
			//! an error at construction and cache the verdict in
			//! `bColorTierValid`; EVERY colour resolution then returns the
			//! uniform mid-brown sigma_a, whatever painters happen to be
			//! bound.  (It is deliberately NOT "priority order among the
			//! bound tiers": a misconfigured material must look like the
			//! error the log describes, not like a silently-chosen tier.)
			int ActiveColorTierCount() const
			{
				return ( ( eumelanin || pheomelanin ) ? 1 : 0 ) +
				       ( sigma_a ? 1 : 0 ) +
				       ( color ? 1 : 0 );
			}
		};

		//! Resolved, clamped, per-hit appearance parameters.  Cheap
		//! enough to rebuild per call; holds no painter pointers.
		//! Namespace-scope (rather than nested) so the model math in
		//! HairBSDF.cpp's anonymous namespace can name it.
		struct HairResolvedParams
		{
			Scalar	h;					//!< near-field offset, clamped to
										//!< [-0.9995, +0.9995] (kHEdge in
										//!< HairBSDF.cpp) -- never the raw
										//!< [-1, 1], see the file header,
										//!< section 1
			Scalar	betaM, betaN;		//!< clamped to [kMinBeta, kMaxBeta]
			Scalar	etaRef;				//!< achromatic reference IOR (drives ALL sampling)
			Scalar	v[4];				//!< longitudinal variances, p = 0..3
			Scalar	s;					//!< azimuthal logistic scale
			Scalar	sin2kAlpha[3];		//!< cuticle-tilt rotation recurrence
			Scalar	cos2kAlpha[3];

			//! Yan 2017 medulla (section 3b).  `medullaActive` is the
			//! LATCH the whole Phase-3 extension hangs off: false makes
			//! every array, loop bound and branch below revert to the
			//! pre-Phase-3 shape, which is what makes kappa == 0
			//! bit-identical rather than merely numerically close.  The
			//! other three are meaningless (and left at 0) when it is
			//! false.
			bool	medullaActive;
			Scalar	kappa;				//!< medulla radius ratio, (0, 0.95]
			Scalar	sigmaM;				//!< medulla scattering coefficient, > 0
			Scalar	gHG;				//!< medulla phase-function anisotropy, (-1, 1)
		};

		//! Shared painter ownership + per-hit parameter resolution for
		//! the hair BRDF and SPF.  Not an interface and not reference
		//! counted -- it is a plain protected base so the two public
		//! classes cannot drift in how they read their painters.
		class HairScatteringBase
		{
		protected:
			explicit HairScatteringBase( const HairPainters& p );
			~HairScatteringBase();

			//! The lowest beta the model is allowed to see.  Below this
			//! the trimmed-logistic azimuthal lobe becomes near-delta and
			//! the TRT caustic generates outliers that survive every
			//! downstream clamp -- the classic hair firefly.  Production
			//! models impose the same floor; docs/HAIR_FUR_DESIGN.md
			//! section 6.3.  Applied at EVALUATION (not only at parse)
			//! so a painter-driven roughness map cannot sneak under it.
			static const Scalar		kMinBeta;
			static const Scalar		kMaxBeta;

			typedef HairResolvedParams Resolved;

			void Resolve( const RayIntersectionGeometric& ri, Resolved& out ) const;

			//! sigma_a per RGB channel at this hit (all three tiers).
			//! Returns the uniform mid-brown fallback -- and ONLY that --
			//! when `bColorTierValid` is false.
			void SigmaARGB( const RayIntersectionGeometric& ri,
			                const Scalar betaN, Scalar out[3] ) const;

			//! sigma_a at one wavelength (all three tiers).  Same
			//! `bColorTierValid` fallback as SigmaARGB.
			Scalar SigmaANM( const RayIntersectionGeometric& ri,
			                 const Scalar betaN, const Scalar nm ) const;

			//! The achromatic proxy absorption that drives the sampling
			//! PMF: the component-wise minimum of SigmaARGB.  See the
			//! file header, section 3.
			Scalar SigmaAProxy( const RayIntersectionGeometric& ri,
			                    const Scalar betaN ) const;

			//! Closed-form multiple-scattering-averaged reflectance
			//! implied by this hit's colour tier (NOT the directional-
			//! hemispherical reflectance of this BCSDF itself -- see the
			//! note above `RunInversionRoundTrip` in HairBSDFTest.cpp,
			//! group 6, which measures the single-scatter furnace
			//! throughput at the inverted sigma_a and finds it does NOT
			//! equal the target C), PLUS the achromatic R-lobe
			//! surface term -- tier 3 starts from the painter's colour,
			//! tiers 1-2 invert C = exp( -sqrt(sigma_a) * D(beta_n) ),
			//! and both are then composited as C + (1 - C) * F_avg with
			//! F_avg the normal-incidence dielectric Fresnel for this
			//! hit's eta.  Without that term black hair reports albedo 0
			//! and OIDN de-guides the whole specular highlight.  Clamped
			//! to [0, 1].  Backs `HairBRDF::albedo` (the OIDN albedo AOV).
			void ReflectanceRGB( const RayIntersectionGeometric& ri,
			                     Scalar out[3] ) const;

			//! The BARE Chiang lobe sum -- NO 1/|wi . N| factor,
			//! so it integrates to 1 over the sphere in solid-angle
			//! measure when sigma_a == 0.  This is the quantity `kray`
			//! divides by `pdf`, and the quantity `value` divides by
			//! the SHADING cosine |wi . N| (file header, section 2).
			//!
			//! `bNM == false` fills all three of `out` from the RGB
			//! sigma_a triple at the reference IOR; `bNM == true` fills
			//! `out[0]` only, at sigma_a(nm) and eta(nm).
			//!
			//! `wi` points AWAY from the surface toward the light /
			//! the continuation (PBRT's wi); the view direction is
			//! -ri.ray.Dir().
			void EvalFsum( const RayIntersectionGeometric& ri,
			               const Resolved& R, const Vector3& wi,
			               const bool bNM, const Scalar nm,
			               Scalar out[3] ) const;

			//! The mixture sampling pdf for direction `wi`, solid-angle
			//! measure.  Achromatic by construction (file header,
			//! section 3), so it is identical for every wavelength and
			//! reconstructible inside `EvaluateKrayNM`.
			Scalar EvalPdf( const RayIntersectionGeometric& ri,
			                const Resolved& R, const Vector3& wi ) const;

			const IScalarPainter*	pEumelanin;
			const IScalarPainter*	pPheomelanin;
			const IScalarPainter*	pSigmaA;
			const IPainter*			pColor;
			const IScalarPainter*	pBetaM;
			const IScalarPainter*	pBetaN;
			const IScalarPainter*	pAlpha;
			const IScalarPainter*	pIOR;
			const IScalarPainter*	pMedullaRatio;		//!< may be NULL -- see HairPainters
			const IScalarPainter*	pMedullaScatter;	//!< may be NULL
			const IScalarPainter*	pMedullaG;			//!< may be NULL

			//! Ceiling on `medulla_ratio`.  A medulla that reaches the
			//! cuticle leaves no cortex annulus for the R / TRT Fresnel
			//! geometry to live in; Yan's measured fits top out near
			//! 0.9 (rabbit).
			static const Scalar		kMaxMedullaRatio;
			//! Used when `medulla_ratio` is bound but its companions
			//! are not.
			static const Scalar		kDefaultMedullaScatter;
			static const Scalar		kDefaultMedullaG;

			//! `HairPainters::ActiveColorTierCount() == 1`, decided ONCE
			//! at construction.  False makes every colour resolution
			//! return the mid-brown fallback, which is what the
			//! constructor's error message promises the author.  Cached
			//! so the per-hit path tests one bool instead of four
			//! pointers.
			const bool				bColorTierValid;

		private:
			HairScatteringBase( const HairScatteringBase& );
			HairScatteringBase& operator=( const HairScatteringBase& );
		};

		//! Evaluable-anywhere Chiang BCSDF.  Kept a real IBSDF (rather
		//! than the SPF-only DielectricMaterial pattern) so PT's NEE and
		//! every BDPT / VCM connection strategy can evaluate hair --
		//! a null BSDF reads as black at every connection vertex.
		class HairBRDF :
			public virtual IBSDF,
			public virtual Reference,
			protected HairScatteringBase
		{
		protected:
			virtual ~HairBRDF();

		public:
			explicit HairBRDF( const HairPainters& p );

			//! f_RISE = fsum / |wi . N| (the SHADING cosine, NOT
			//! cos theta_i); see the file header,
			//! section 2.  `vLightIn` points FROM the surface TOWARD the
			//! light (PBRT's wi); the view direction is -ri.ray.Dir().
			RISEPel	value(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri
				) const;

			//! Spectral twin of `value`.  Fully spectral in sigma_a and
			//! (if the painter disperses) in eta.
			Scalar	valueNM(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const Scalar nm
				) const;

			//! Closed-form multiple-scattering-averaged reflectance (see
			//! `ReflectanceRGB`) for the OIDN albedo AOV -- noise-free by
			//! construction.
			RISEPel albedo(
				const RayIntersectionGeometric& ri
				) const;

			//! TEST HOOK -- not called by the renderer, and deliberately
			//! not part of `IBSDF`.  Exposes the per-order apparent
			//! attenuation A_p (4 entries, p = 0..kPMax) and the internal
			//! optical path length at this hit for one absorption
			//! coefficient, so `HairBSDFTest` can check the absorption
			//! FORMULA against its closed form (at h = 0 and theta_o = 0
			//! the path length is exactly 2 fibre diameters, hence the
			//! TT transmittance is exactly exp(-2 sigma_a)) rather than
			//! against a self-consistency identity.  Uses the achromatic
			//! reference IOR, matching `EvalPdf`.
			void TestApAndPathLength(
				const RayIntersectionGeometric& ri,
				const Scalar sigmaA,
				Scalar ap[4],
				Scalar& absorbLen
				) const;

			//! TEST HOOK -- not called by the renderer, and deliberately
			//! not part of `IBSDF`.  The medulla twin of
			//! `TestApAndPathLength`: exposes the SIX-entry attenuation
			//! vector after the Yan 2017 split, plus the split factors
			//! and the medulla geometry that produced it.
			//!
			//! It exists because the medulla's central correctness claim
			//! -- that the split CONSERVES each parent order's energy
			//! exactly (HairBSDF.h section 3b) -- is invisible to every
			//! mixture-level test in HairBSDFTest: the furnace and the
			//! estimator cross-check both measure the SUM, which the
			//! split leaves unchanged by construction, so they would
			//! stay green even if the two halves were swapped, mis-
			//! weighted, or both attached to the wrong parent.  This
			//! hook lets a test read ap[1] / ap[TTs] / ap[2] / ap[TRTs]
			//! individually and pin the bookkeeping term by term.
			//!
			//! `ap` receives 6 entries: the four Chiang orders (with
			//! p == 1 and p == 2 already scaled DOWN by the ballistic
			//! survival) followed by TTs and TRTs.  `nLobes` reports 4
			//! when the medulla is inactive at this hit -- in which case
			//! only the first four entries are written.  Uses the
			//! achromatic reference IOR, matching `EvalPdf`.
			void TestMedullaAp(
				const RayIntersectionGeometric& ri,
				const Scalar sigmaA,
				Scalar ap[6],
				int& nLobes,
				Scalar& q1,
				Scalar& q2,
				Scalar& medullaB,
				Scalar& medullaTau,
				Scalar& medullaLongVariance
				) const;

			//! TEST HOOK -- not called by the renderer.  Calls the REAL
			//! `Resolve()` (the same per-hit resolution the renderer uses,
			//! which reads `alpha` off this material's bound alpha
			//! painter and runs the actual 2k-alpha cuticle-tilt
			//! recurrence), then calls the real `ApplyLobeTilt` (the SAME
			//! function both the evaluation and sampling sides call --
			//! HairBSDF.cpp's `LobeWeights` / `HairSPF::DoScatter`) with
			//! `Resolve()`'s own `sin2kAlpha` / `cos2kAlpha`.  A corruption
			//! ANYWHERE in the painter -> Resolve() -> sin2kAlpha /
			//! cos2kAlpha -> ApplyLobeTilt chain is therefore visible to
			//! this hook, not just a corruption inside ApplyLobeTilt
			//! itself.  Exists because groups 10/11 in HairBSDFTest.cpp
			//! only ever exercise `ApplyLobeTilt` through the p == 0 (R)
			//! branch and only through mixture-level energy/estimator
			//! checks, which are measure-preserving under a tilt-sign flip
			//! on any lobe and structurally cannot see a corruption
			//! isolated to the p == 1 (TT), p == 2 (TRT), or
			//! residual-identity branch.  This white-box hook lets a test
			//! pin all four branches' angle formula and sign directly,
			//! against the full recurrence assembly.
			void TestApplyLobeTilt(
				const RayIntersectionGeometric& ri,
				const int p,
				const Scalar sinThetaO,
				const Scalar cosThetaO,
				Scalar& sinOut,
				Scalar& cosOut
				) const;
		};

		//! Exact Chiang importance sampler.  Populates exactly ONE
		//! ScatteredRay per call: the lobe p is chosen stochastically
		//! from the A_p energies at the actual h, and both M_p and the
		//! trimmed logistic N_p are sampled analytically, so
		//! Scatter / Pdf / value are consistent term-for-term.
		class HairSPF :
			public virtual ISPF,
			public virtual Reference,
			protected HairScatteringBase
		{
		protected:
			virtual ~HairSPF();

			//! Shared body of Scatter / ScatterNM.  `bNM` selects the
			//! spectral evaluation of the throughput (krayNM at `nm`)
			//! versus the RGB triple (kray); the DIRECTION and the pdf
			//! are identical either way, which is what makes
			//! `EvaluateKrayNM` exact (file header, section 3).
			void DoScatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const bool bNM,
				const Scalar nm,
				ScatteredRayContainer& scattered
				) const;

		public:
			explicit HairSPF( const HairPainters& p );

			void	Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			void	ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			//! Exact mixture pdf, solid-angle measure.  Wavelength
			//! independent by construction (file header, section 3).
			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			//! Identical to `Pdf` -- see the file header, section 3.
			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			//! HWSS companion throughput.  Recomputes the FULL BSDF
			//! ratio fsum(nm) / pdf rather than trying to recover the
			//! sampled lobe index p: p is not derivable from
			//! (ri, outDir, rayType) alone, because every hair lobe
			//! shares the `eRayReflection` tag and the lobes overlap in
			//! direction space.  Correct first, fast second -- and it is
			//! still ~1 BSDF evaluation, versus the integrator's own
			//! fallback which costs the same evaluation PLUS a virtual
			//! dispatch through the BRDF.  Exact because `Pdf` is
			//! wavelength independent, so the pdf reconstructed here IS
			//! the hero's pdf.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;
		};
	}
}

#endif
