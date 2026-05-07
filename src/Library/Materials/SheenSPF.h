//////////////////////////////////////////////////////////////////////
//
//  SheenSPF.h - Scattering / sampling counterpart to SheenBRDF.
//    Uses cosine-weighted hemisphere sampling because Charlie's
//    distribution doesn't have a clean closed-form importance
//    sample.  PDF mismatch with the BRDF lobe is handled by MIS at
//    the integrator level (BDPT/PT both compute pdf via SPF::Pdf
//    and weight against light-sampling proposals).
//
//  See SheenBRDF.h for the math.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_SPF_
#define SHEEN_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SheenSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			virtual ~SheenSPF();

			const IPainter& pColor;
			const IPainter& pRoughness;

		public:
			SheenSPF(
				const IPainter& sheenColor,
				const IPainter& sheenRoughness
				);

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

			//! Sheen always wants the Khronos additive composition when
			//! it is the top layer of a CompositeSPF — the random walk
			//! never reaches the base for upward-emitting cosine-
			//! hemisphere sampling.  Returning true unconditionally
			//! (rather than gating on sheenColor != 0) keeps "black
			//! sheen" texels behaving as `pure base BRDF` rather than
			//! silently re-engaging the broken random-walk fallback.
			bool UsesAdditiveLayering() const override { return true; }

			//! Layer albedo for the Khronos additive sheen-over-base
			//! composition.  Returns sheenColor · E_sheen(NdotV, α) where
			//! E_sheen is the directional albedo of the V-cavities Charlie
			//! BRDF baked into a 2D LUT at first use.
			RISEPel GetLayerAlbedo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const override;

			Scalar GetLayerAlbedoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const override;

			//! Static LUT lookup exposed for SheenBRDF and CompositeBRDF.
			//! Returns E_sheen(NdotV, α) ∈ [0, 1] — the directional albedo
			//! of the V-cavities Charlie BRDF (without sheenColor).  The
			//! LUT is built lazily on first call (thread-safe via
			//! function-local-static "magic statics") and is stable across
			//! runs (deterministic seed).
			static Scalar AlbedoLookup( Scalar nDotV, Scalar alpha );
		};
	}
}

#endif
