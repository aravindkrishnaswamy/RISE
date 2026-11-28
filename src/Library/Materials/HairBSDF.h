//////////////////////////////////////////////////////////////////////
//
//  HairBSDF.h - Chiang et al. 2016 near-field hair / fur BCSDF, with
//  optional Yan et al. 2017 medulla scattering (`medulla_ratio > 0`).
//
//  Model per Chiang, Bitterli, Tappan & Burley (EGSR 2016), validated
//  against PBRT-v4's `HairBxDF` (Apache-2.0); independent implementation
//  for RISE's IBSDF / ISPF interfaces and spectral (NM) pipeline.
//  Design record: docs/HAIR_FUR_DESIGN.md.
//
//  Load-bearing invariants:
//  - Fibre frame (right-handed): x = ri.onb.u() (root->tip tangent from
//    vShadingTangent/bHasShadingTangent), y = ri.onb.v(), z = ri.onb.w()
//    (shading normal ri.vNormal). sin(theta) = w.x, phi = atan2(w.z, w.y).
//    Near-field offset h = 2*ri.ptCoord.y - 1 clamped to +/-0.9995 (trap:
//    geometry without UVs gives ptCoord=(0,0) -> h=-1 grazing mirror).
//  - kray/pdf reconciliation: Chiang fsum integrates to 1 over the sphere
//    without a cosine, so value(wi, ri) = fsum / |dot(wi, N)| (SHADING
//    cosine, NOT cos(theta_i)) and kray = fsum / pdf. Singular on wi.N==0;
//    callers multiply |wi.N| back (legacy cosine-omitting AO / FinalGather
//    / AreaLight shader ops blow up on hair).
//  - Achromatic sampling PDF: lobe PMF uses component-wise min of RGB
//    sigma_a and achromatic reference IOR so kray is provably in [0, 1]
//    for non-dispersive IOR and EvaluateKrayNM reconstructs hero PDF.
//    valueNM remains fully spectral (dispersive IOR adds variance, not bias).
//  - Medulla (Yan 2017): medulla_ratio == 0 hard-short-circuits bit-identically
//    to Chiang-only. When active, TT and TRT split into ballistic (q, q^2)
//    and scattered (1-q, 1-q^2) lobes conserving sum A_p; residual lobe is
//    unsplit, TRTs reuses single-crossing profile, longitudinal broadening
//    is additive in variance.
//  - Colour tiers (exactly one active): (1) eumelanin/pheomelanin (OMLC
//    curves anchored at 550 nm to PBRT green), (2) direct sigma_a, or
//    (3) Chiang color inversion.
//  - Caveats: non-reciprocal (h-conditioning + cuticle tilt); no geometric-
//    horizon gate (full-sphere scattering, HairMaterial::ScattersFullSphere()
//    == true enables full-sphere NEE); never delta (no SMS); all lobes
//    tagged eRayReflection.
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
		//! Painter bundle for HairBRDF / HairSPF / HairMaterial. Exactly one
		//! colour tier must be bound (melanin, sigma_a, or color); beta_m,
		//! beta_n, alpha, and ior are mandatory; medulla slots are optional.
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

			//! Active colour tier count (must be 1; invalid configs log an error
			//! and fall back uniformly to mid-brown sigma_a via `bColorTierValid`).
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

			//! Yan 2017 medulla latch: when false, kappa == 0 short-circuits
			//! bit-identically to Chiang-only and the three fields below are 0.
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

			//! Closed-form multiple-scattering-averaged reflectance for OIDN albedo
			//! AOV, composited with normal-incidence dielectric Fresnel (`C + (1-C)*F0`)
			//! so black hair retains specular guide energy. Clamped to [0, 1].
			void ReflectanceRGB( const RayIntersectionGeometric& ri,
			                     Scalar out[3] ) const;

			//! Bare Chiang lobe sum (no 1/|wi . N| factor; integrates to 1 over
			//! sphere when sigma_a == 0). `bNM == false` fills RGB `out[0..2]`;
			//! `bNM == true` fills `out[0]` at (sigma_a(nm), eta(nm)). `wi` points
			//! away from the surface; view is -ri.ray.Dir().
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

            // Every HairSPF draw, including TT/TTs, carries reflection.
            bool valueByScatterType(const Vector3& wo,const RayIntersectionGeometric& ri,
                const IORStack*,RISEPel out[5]) const {
                for(int t=0;t<5;++t) out[t]=RISEPel(0,0,0);
                out[ScatteredRay::eRayReflection]=value(wo,ri);return true;
            }
            bool valueByScatterTypeNM(const Vector3& wo,const RayIntersectionGeometric& ri,
                Scalar nm,const IORStack*,Scalar out[5]) const {
                for(int t=0;t<5;++t) out[t]=0;
                out[ScatteredRay::eRayReflection]=valueNM(wo,ri,nm);return true;
            }

			//! Closed-form multiple-scattering-averaged reflectance (see
			//! `ReflectanceRGB`) for the OIDN albedo AOV -- noise-free by
			//! construction.
			RISEPel albedo(
				const RayIntersectionGeometric& ri
				) const;

			//! Test hook: exposes per-order A_p[4] and internal optical path length
			//! (using achromatic reference IOR) for closed-form absorption checks.
			void TestApAndPathLength(
				const RayIntersectionGeometric& ri,
				const Scalar sigmaA,
				Scalar ap[4],
				Scalar& absorbLen
				) const;

			//! Test hook: exposes the 6-entry Yan 2017 post-split attenuation
			//! vector (4 Chiang orders with ballistic survival applied, then TTs/TRTs),
			//! nLobes (4 if inactive), survival factors, and medulla geometry.
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

			//! Test hook: runs Resolve() + ApplyLobeTilt() for lobe p so tests can
			//! verify all four cuticle-tilt angle branches and signs directly.
			void TestApplyLobeTilt(
				const RayIntersectionGeometric& ri,
				const int p,
				const Scalar sinThetaO,
				const Scalar cosThetaO,
				Scalar& sinOut,
				Scalar& cosOut
				) const;

			//! TEST HOOK -- not called by the renderer.  Exposes the
			//! protected `SigmaARGB` / `SigmaANM` (HairScatteringBase)
			//! directly, so a test can check the tier-3 (`color`)
			//! reflectance-to-absorption inversion's GuardedGetColorNM
			//! white-corner guard (HairBSDF.cpp SigmaANM, ~line 1011) end
			//! to end without going through the whole BCSDF evaluation --
			//! mirrors `TestApAndPathLength` / `TestMedullaAp` above.
			void TestSigmaA(
				const RayIntersectionGeometric& ri,
				const Scalar betaN,
				Scalar rgbOut[3],
				Scalar& nmOut,
				const Scalar nm
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

			//! Spectral lobe BSDF value f_I(wo; nm) [1/sr] without cosine or pdf
			//! division (avoids EvalPdf in subpath throughput ratios).
			Scalar EvaluateLobeFNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! HWSS companion throughput fsum(nm) / pdf (exact since Pdf is
			//! wavelength-independent; lobes share eRayReflection and overlap).

			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! DL-216. Unbiased companion weight evaluated with the HERO
			//! wavelength's sampling density: f_I(nm) / pdfHero.
			//! Note: curve geometry incorporates projected area directly,
			//! so no cosine factor is multiplied here.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack,
				Scalar pdfHero
				) const;
		};
	}
}

#endif
