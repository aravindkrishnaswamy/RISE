//////////////////////////////////////////////////////////////////////
//
//  WeaveSPF.h - Importance sampler for `weave_material`
//  (docs/CLOTH_FABRIC_DESIGN.md; Zhu et al. 2024 §5.1 attenuation-
//  proportional lobe selection, repriced against the full mixture).
//
//  Estimator & load-bearing invariants:
//  - Sample-then-reprice: choose family k by coverage `ahat_k`, then
//    surface vs volume by `w_k = WeaveBRDF::SurfaceSelectWeight`, and
//    reprice against the full mixture density `q(wo)` so `Scatter` and
//    `Pdf` agree (`kray = f_weave * |cos(wo)| / q(wo)`). Never divide a
//    continuum lobe by its single-branch selection probability.
//  - Surface branch: d'Eon's exact `Mp` inverse + `SampleTrimmedLogistic`
//    trimmed to `SurfaceAzimuthInterval` (exact hemisphere density at
//    zero tilt; under non-zero tilt, buried latitudes emit no sample --
//    variance only, since the cosine volume branch covers the hemisphere).
//  - Volume branch: cosine-hemisphere about the ray-facing normal (trades
//    indirect variance on narrow `2*beta` cones for closed-form density).
//  - Thin transmission (`p.thin`): outer draw with probability `gap` emits
//    delta pass-through (`wi = ri.ray.Dir()`, `isDelta = true`, `pdf = 1`,
//    `kray = 1`, `Pdf() == 0`), scaling continuum `Pdf` by `(1 - gap)`.
//    Within volume, prob `transmit_k` flips to the back-face hemisphere.
//    `!p.thin` consumes the exact opaque RNG sequence with zero extra draws.
//  - Geometric-horizon gate is mirrored between `Scatter` and `Pdf`.
//    Emits at most one ray per `Scatter`. No `GetSpecularInfo` (SMS
//    ignores weave). `EvaluateKrayNM` handles ONLY the achromatic delta
//    gap lobe (returns 1) and declines (`-1`) continuum lobes so HWSS
//    divides by the stored hero `pS->pdf`.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_SPF_
#define WEAVE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Utilities/Reference.h"
#include "WeaveBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class WeaveSPF : public virtual ISPF, public virtual Reference
		{
		public:
			//! @param brdf  Addref'd WeaveBRDF; shared state keeps Scatter/Pdf/value
			//!              and interactive editor rebinds in sync.
			explicit WeaveSPF( const WeaveBRDF& brdf );

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

			//! Expected shadow-ray gap transmittance (`gap` if `transmission thin`,
			//! else 0); mirrors ScatterImpl's gap draw.
			RISEPel DeltaPassThroughTransmittance(
				const RayIntersectionGeometric& ri
				) const;

			Scalar DeltaPassThroughTransmittanceNM(
				const RayIntersectionGeometric& ri,
				const Scalar nm
				) const;

			//! Companion-wavelength kray for the achromatic delta gap lobe
			//! (`eRayRefraction` along `ri.ray.Dir()`, returns 1); returns -1 for
			//! all continuum lobes and unreachable gaps.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			inline const WeaveBRDF& GetBRDF() const { return *pBRDF; }

		protected:
			virtual ~WeaveSPF();

			//! Shared body of Scatter / ScatterNM.  `nm < 0` selects the
			//! RGB regime.  ONE implementation is the structural defence
			//! against the RGB/NM-twin drift
			//! docs/skills/audit-by-bug-pattern.md catalogues.
			void ScatterImpl(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			//! Mixture density with pre-resolved params (avoids re-reading painters
			//! on the hot ScatterImpl path).
			static Scalar PdfWithParams(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const WeaveBRDF::WeaveParams& p
				);

			const WeaveBRDF*	pBRDF;
		};
	}
}

#endif
