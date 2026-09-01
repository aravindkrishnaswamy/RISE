//////////////////////////////////////////////////////////////////////
//
//  CoatedSPF.h - Importance sampler for `coated_material`
//    (docs/WETNESS_COAT_DESIGN.md 7.5, Phase 2 items 1 and 5).
//
//  MIXTURE SAMPLING, MIXTURE PDF, ONE ESTIMATOR.
//
//  One stochastic single-lobe pick with the kray / selectProb
//  correction, matching the convention every RISE integrator has used
//  since deterministic path-splitting was excised in 2026-05.  The
//  selection weight is 7.5's:  p_coat = coat_weight * Fresnel(coat).
//
//    q(wo)  =  p_coat * q_coat(wo)  +  (1 - p_coat) * q_base(wo)
//
//  and the emitted weight is the FULL layered response over that
//  mixture density:
//
//    kray   =  f_coated(wi, wo) * cos(wo) / q(wo)
//
//  with `f_coated` coming from CoatedBRDF -- the same closed form NEE
//  and BDPT/VCM connections evaluate.  Three consequences worth
//  stating, because they are the reasons for this shape:
//
//   1. `Pdf` is the REAL mixture PDF, never `composite_material`'s
//      50/50 placeholder (CompositeSPF.cpp:297-309).  7.5 calls this
//      "the correctness line that separates the two" and
//      tests/SPFPdfConsistencyTest.cpp is the guard.
//
//   2. `kray * pdf == value * cos` holds EXACTLY, by construction, for
//      every emitted ray -- so this SPF passes
//      SPFBSDFConsistencyTest's pointwise (Part D) check as a
//      single-lobe material even though it samples a mixture.
//
//   3. NO `EvaluateKrayNM` OVERRIDE, deliberately -- and this is the
//      case where the ISPF default is strictly BETTER than an
//      override.  The default returns -1, which routes
//      PathTracingIntegrator to `valueNM * cos / pS->pdf`: the pdf
//      stored ON THE SAMPLED RAY, i.e. the true HERO-wavelength
//      mixture density that actually drew it.  An override here could
//      only recompute the density at the COMPANION wavelength, which
//      is a different number the moment any coat or substrate painter
//      is wavelength-dependent (a dispersive `coat_ior`, a spectral
//      substrate reflectance) -- and dividing by the wrong density is
//      a bias, not an approximation.  An earlier revision of this file
//      shipped exactly that override; it is gone.  The `value`/
//      `Scatter` agreement above is what makes the default path exact.
//
//  LOBE BUDGET (7.5).  This SPF emits at most ONE ray per Scatter
//  call, so the 6-slot ScatteredRayContainer is never a constraint --
//  the coat's transmission is folded into the substrate ray's
//  throughput analytically rather than being emitted as a ray, exactly
//  as 7.5 specifies.
//
//  NOT DELTA.  The coat GGX alpha is floored at
//  CoatedLayer::kMinCoatAlpha, so no lobe here is ever a delta
//  distribution and `GetSpecularInfo` is deliberately left at the
//  ISPF default.  See CoatedMaterial.h for why.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COATED_SPF_
#define COATED_SPF_

#include "../Interfaces/ISPF.h"
#include "../Utilities/Reference.h"
#include "CoatedBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class CoatedSPF : public virtual ISPF, public virtual Reference
		{
		public:
			//! @param brdf     the layered closed form this sampler is
			//!                 the importance sampler FOR.  Held by
			//!                 reference (addref'd) so `Scatter` and
			//!                 `value` can never disagree.
			//! @param baseSPF  the substrate's own sampler; supplies
			//!                 the substrate branch's directions and
			//!                 its half of the mixture density.
			CoatedSPF( const CoatedBRDF& brdf, const ISPF& baseSPF );

			void Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			void ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			Scalar Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			Scalar PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			// NOTE: no EvaluateKrayNM override.  See point 3 in this
			// file's header -- the ISPF default (-1, "fall back to BSDF
			// evaluation") is strictly more correct here than anything
			// this class could compute.

			inline const CoatedBRDF& GetBRDF()    const { return *pBRDF; }
			inline const ISPF&       GetBaseSPF() const { return *pBaseSPF; }

		protected:
			virtual ~CoatedSPF();

			//! Shared body of Scatter / ScatterNM.  `nm < 0` selects
			//! the RGB regime.  Keeping one implementation is the
			//! structural defence against the RGB/NM-twin drift
			//! docs/skills/audit-by-bug-pattern.md catalogues.
			void ScatterImpl(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			//! Shared body of Pdf / PdfNM.
			Scalar PdfImpl(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			const CoatedBRDF*	pBRDF;
			const ISPF*			pBaseSPF;
		};
	}
}

#endif
