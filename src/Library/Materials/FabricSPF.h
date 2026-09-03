//////////////////////////////////////////////////////////////////////
//
//  FabricSPF.h - Importance sampler for `fabric_material`
//    (docs/CLOTH_FABRIC_DESIGN.md 9.2, "sample one lobe, then price the
//    sample against the FULL mixture").
//
//  MIXTURE SAMPLING, MIXTURE PDF, ONE ESTIMATOR -- CoatedSPF's shape,
//  with a different selection weight and a different top lobe.
//
//  The selection weight is the sheen lobe's own DIRECTIONAL ALBEDO,
//
//      w  =  max3(sheenColor) * E(alpha, n.v)        (clamped to [0,1])
//
//  so the branch probability tracks the actual energy split rather than
//  being an arbitrary constant.  The density is the real two-term
//  mixture, evaluated for an ARBITRARY wo with no memory of how
//  anything was sampled:
//
//      q(wo)  =  w * (cos(wo)/pi)  +  (1 - w) * q_base(wo)
//
//  and the emitted weight is the FULL closed form over that density:
//
//      kray   =  f_fabric(wi, wo) * cos(wo) / q(wo)
//      pdf    =  q(wo)
//
//  DO NOT DIVIDE kray BY THE SELECTION PROBABILITY ALONE.  That
//  convention is correct only for DELTA lobes, where `Pdf()` returns 0
//  by contract (ISPF.h:194) and MIS routes around the density entirely
//  through `GetSpecularInfo`.  NEITHER FABRIC LOBE IS DELTA: Charlie
//  sheen and every allowlisted substrate have full-hemisphere support,
//  so a given `wo` is generically reachable from BOTH branches and the
//  density Scatter must report is the whole mixture.  Reporting
//  `w * pdf_sheen(wo)` for a sheen-branch sample would disagree with an
//  independent `Pdf(ri, wo)` call for that same direction whenever
//  `q_base(wo) > 0` -- precisely the Scatter<->Pdf mismatch
//  tests/SPFPdfConsistencyTest.cpp exists to catch, and which 9.9
//  gate 6 commits this phase to passing.
//
//  SHEEN BRANCH: COSINE-HEMISPHERE, exactly as SheenSPF does today
//  (SheenSPF.cpp:73-74).  9.4 withdrew the D-importance-sampling
//  proposal: Charlie has no published VNDF, its mass sits near grazing
//  half-vectors by construction, and reflecting `v` about such an `h`
//  puts `wo` below the horizon for a substantial fraction of draws --
//  so the accepted-sample density carries a view-dependent acceptance
//  factor C(v) < 1 with no closed form, which fails the shipped pdf
//  harness's integral (Part 2) and chi-squared (Part 3) sub-tests
//  outright.  Cosine sampling is horizon-safe, integrates to 1 by
//  construction, and its density is EXACT -- so the mixture above is
//  exact too, and MIS against NEE carries the variance.
//
//  SUBSTRATE BRANCH: the base SPF chooses the direction, handed the
//  WEAVE-ROTATED hit record (9.5) -- the same record FabricBRDF::value
//  and this class's own `Pdf` use, because a Scatter that samples in a
//  rotated frame while Pdf evaluates in the unrotated one is the same
//  mismatch by another route.
//
//  GEOMETRIC-HORIZON GATE, mirrored between sampler and density, so a
//  direction Scatter can no longer emit carries zero density.  A
//  GlintModifier tilt can otherwise send a continuation ray into the
//  solid (SheenSPF.cpp:86-99).
//
//  LOBE BUDGET.  This SPF emits at most ONE ray per Scatter call: the
//  sheen branch adds one, and every material on the substrate allowlist
//  (Lambertian, Oren-Nayar, GGX) emits at most one itself.  The loop
//  below still rewrites EVERY ray the base added, so no stale
//  base-weight ray can escape if that ever changes.
//
//  NOT DELTA, NO GetSpecularInfo OVERRIDE.  Neither lobe is a delta
//  distribution, so the ISPF default is correct; SMS correctly ignores
//  fabric entirely, on hair's precedent.  See FabricMaterial.h.
//
//  NO EvaluateKrayNM OVERRIDE, deliberately -- CoatedSPF.h point 3's
//  argument transfers verbatim.  The ISPF default (-1) routes
//  PathTracingIntegrator to `valueNM * cos / pS->pdf`, i.e. the pdf
//  stored ON the sampled ray: the true HERO-wavelength mixture density
//  that actually drew it.  An override could only recompute the density
//  at the COMPANION wavelength, which differs the moment any substrate
//  painter is wavelength-dependent -- dividing by the wrong density is
//  a bias, not an approximation.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FABRIC_SPF_
#define FABRIC_SPF_

#include "../Interfaces/ISPF.h"
#include "../Utilities/Reference.h"
#include "FabricBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class FabricSPF : public virtual ISPF, public virtual Reference
		{
		public:
			//! @param brdf     the closed form this sampler is the
			//!                 importance sampler FOR.  Held by
			//!                 reference (addref'd) so `Scatter` and
			//!                 `value` can never disagree, and so the
			//!                 editor's rebinds reach both through one
			//!                 copy of the state.
			//! @param baseSPF  the substrate's own sampler; supplies the
			//!                 substrate branch's directions and its half
			//!                 of the mixture density.
			FabricSPF( const FabricBRDF& brdf, const ISPF& baseSPF );

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

			inline const FabricBRDF& GetBRDF()    const { return *pBRDF; }
			inline const ISPF&       GetBaseSPF() const { return *pBaseSPF; }

		protected:
			virtual ~FabricSPF();

			//! Shared body of Scatter / ScatterNM.  `nm < 0` selects the
			//! RGB regime.  Keeping ONE implementation is the structural
			//! defence against the RGB/NM-twin drift
			//! docs/skills/audit-by-bug-pattern.md catalogues.
			void ScatterImpl(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			//! Shared body of Pdf / PdfNM: resolves the parameters and
			//! the weave-rotated record, then delegates.
			Scalar PdfImpl(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			//! The mixture density with the parameters and the rotated
			//! record ALREADY resolved.
			//!
			//! `ScatterImpl` needs this density up to twice per emitted
			//! ray, and the BRDF value once, all at the same shading
			//! point.  Routing each through the public entry points
			//! re-sampled three painters and rebuilt a ~600-byte
			//! `RayIntersectionGeometric` three to four times per
			//! `Scatter` call -- on the renderer's hottest path, and
			//! precisely for the scenes that set `weave_rotation` (the
			//! denim / silk / satin presets), since a zero angle takes
			//! WeaveRotatedRI's no-copy fast path.  Correctness was never
			//! affected -- painters are deterministic -- but it was ~4x
			//! the layered-material overhead CoatedSPF pays.
			Scalar PdfWithParams(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const FabricBRDF::FabricParams& p,
				const RayIntersectionGeometric& weaveRi,
				const IORStack& ior_stack
				) const;

			const FabricBRDF*	pBRDF;
			const ISPF*			pBaseSPF;
		};
	}
}

#endif
