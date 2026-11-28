//////////////////////////////////////////////////////////////////////
//
//  FabricSPF.h - Importance sampler for `fabric_material`
//  (docs/CLOTH_FABRIC_DESIGN.md §9.2: sample one lobe, reprice against
//  the full mixture).
//
//  Estimator & load-bearing invariants:
//  - Selection weight w = clamp(max3(sheenColor) * Ehat(alpha, n.v), 0, 1);
//    mixture density q(wo) = w * (cos(wo)/pi) + (1 - w) * q_base(wo),
//    emitted kray = f_fabric * |cos(wo)| / q(wo), pdf = q(wo). Never
//    divide continuum samples by the branch selection probability alone.
//  - Sheen branch: cosine-hemisphere (horizon-safe, exact density; Charlie
//    D-sampling lacks a closed-form horizon-acceptance factor).
//  - Substrate branch: delegates to `baseSPF` with `WeaveRotatedRI` (same
//    frame as FabricBRDF::value and Pdf). Geometric-horizon gate is mirrored
//    between Scatter and Pdf. Emits at most one ray per Scatter.
//  - Transmissive substrate (`BaseScattersFullSphere()`): continuum back-face
//    rays reprice with q(wo) = (1 - w) * q_base(wo) and |cos(wo)|; delta gap
//    rays preserve isDelta = true, pdf = 1 and reprice kray by two single-
//    crossing sheen arms over (1 - w) (no recycling denominator for a
//    measure-zero direction). Continuum integral over sphere = w + (1-w)*S.
//  - No `GetSpecularInfo` (SMS ignores fabric). `EvaluateKrayNM` handles
//    ONLY a substrate delta pass-through and returns -1 for continuum rays
//    so HWSS divides by the stored hero `pS->pdf`.
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
			//! @param brdf     Addref'd FabricBRDF (shared state with value/rebinds).
			//! @param baseSPF  Substrate sampler supplying base directions and PDF.
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

			//! DL-05: the substrate's delta pass-through (a thin weave's
			//! gap) as ScatterImpl re-prices it, in expectation over this
			//! wrapper's own branch selection -- see
			//! ISPF::DeltaPassThroughTransmittance.
			RISEPel DeltaPassThroughTransmittance(
				const RayIntersectionGeometric& ri
				) const;

			Scalar DeltaPassThroughTransmittanceNM(
				const RayIntersectionGeometric& ri,
				const Scalar nm
				) const;

			//! Companion-wavelength kray for a substrate delta pass-through ray
			//! (`eRayRefraction`); returns -1 for all continuum rays.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
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

			//! Mixture density with pre-resolved params and weave-rotated RI
			//! (avoids re-sampling painters and copying RI on the hot Scatter path).
			Scalar PdfWithParams(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const FabricBRDF::FabricParams& p,
				const RayIntersectionGeometric& weaveRi,
				const IORStack& ior_stack
				) const;

			//! Expected scalar re-pricing factor for a substrate delta pass-through
			//! ray; writes the substrate's weave rotation into `weaveAngleOut`.
			Scalar DeltaPassThroughScale(
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				Scalar& weaveAngleOut
				) const;

			const FabricBRDF*	pBRDF;
			const ISPF*			pBaseSPF;
		};
	}
}

#endif
