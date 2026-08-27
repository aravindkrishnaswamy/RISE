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
//  direction for a hair render to look right, and TODAY NOTHING
//  SUPPLIES ONE.  `bShadingTangentFromGeometry` is a request for a
//  COHERENT tangent, not for a geometry-defined one: `Object::IntersectRay`
//  (Object.cpp:641-655) honours it by projecting WORLD-X into the plane
//  perpendicular to the shading normal, unconditionally -- the flag has
//  no companion tangent field for a geometry to write.  The separate
//  `vTangent` / `bHasTangent` pair (RayIntersectionGeometric.h:224-235)
//  IS a real per-vertex tangent, but it is glTF-only and consumed by the
//  normal-map modifier.  Closing the gap is the `hair_geometry` slice's
//  job (see docs/HAIR_FUR_DESIGN.md section 4.1 for the two options);
//  this file only reads `ri.onb.u()` and will pick up whatever that
//  slice lands.
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
//          light pdf -- LightSampler.cpp:1675-1679, :1776, :1885)
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
//       two constants (which are a colour-matching PROJECTION of the same
//       data, a different quantity): measured at 600 / 450 nm, eumelanin
//       is +23.5 % / -5.6 % and pheomelanin +24.2 % / +1.7 % against
//       PBRT.  Spectral fidelity to the measurement is the deliberate
//       choice; a PBRT cross-render matches in G and runs slightly warm
//       in R.  Numbers and rationale at kEumelaninSigmaAAt550 in the .cpp.
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
//  * NEE CANNOT REACH THE TRANSMISSIVE HALF.  `LightSampler` rejects
//    shadow directions with `dot(wToLight, vNormal) <= 0`, so the TT
//    lobe is unreachable by next-event estimation and its MIS partner
//    weight is consequently too small.  That is an integrator-level
//    limitation, not a material one; it is out of scope for this slice
//    and is recorded in docs/HAIR_FUR_DESIGN.md's risk register.
//  * LEGACY COSINE-OMITTING `value` CONSUMERS.  Section 2's constraint
//    -- value == fsum / |wi . N| -- is only safe for a caller that
//    multiplies |wi . N| back.  Three legacy shader ops do not:
//      - AmbientOcclusionShaderOp.cpp:139, :151 and
//        FinalGatherShaderOp.cpp:215, :508 accumulate
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
//  * REGRESSION SCENE -- PARTIALLY UNBLOCKED as of Slice B (registration:
//    RISE_API_CreateHairMaterial / Job::AddHairMaterial / the
//    `hair_material` chunk parser).  `scenes/Tests/ChunkCoverage/
//    cc_hair_material.RISEscene` exists and parses + registers a
//    material bound to a sphere.  MATERIALS.md section 9's real
//    per-BSDF regression scene (visible strand-level renders exercising
//    the lobes at grazing/backlit angles) still needs actual fibre
//    geometry to bind to and remains BLOCKED on `hair_geometry`
//    (docs/HAIR_FUR_DESIGN.md section 5).  HairBSDFTest.cpp carries the
//    numeric coverage and HairMaterialChunkTest.cpp the registration
//    coverage in the meantime.
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

			HairPainters() :
			  eumelanin( 0 ), pheomelanin( 0 ), sigma_a( 0 ), color( 0 ),
			  beta_m( 0 ), beta_n( 0 ), alpha( 0 ), ior( 0 )
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
			Scalar	h;					//!< near-field offset, [-1, 1]
			Scalar	betaM, betaN;		//!< clamped to [kMinBeta, kMaxBeta]
			Scalar	etaRef;				//!< achromatic reference IOR (drives ALL sampling)
			Scalar	v[4];				//!< longitudinal variances, p = 0..3
			Scalar	s;					//!< azimuthal logistic scale
			Scalar	sin2kAlpha[3];		//!< cuticle-tilt rotation recurrence
			Scalar	cos2kAlpha[3];
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

			//! Closed-form directional-hemispherical reflectance implied
			//! by this hit's colour tier, PLUS the achromatic R-lobe
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

			//! Closed-form directional-hemispherical reflectance for the
			//! OIDN albedo AOV -- noise-free by construction.
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
