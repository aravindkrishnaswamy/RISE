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
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pColor;
			const IScalarPainter*	pRoughness;		// physical scalar

		public:
			SheenSPF(
				const IPainter& sheenColor,
				const IScalarPainter& sheenRoughness
				);

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetColor()     const { return *pColor; }
			inline const IScalarPainter& GetRoughness() const { return *pRoughness; }
			void SetColor( const IPainter& v );
			void SetRoughness( const IScalarPainter& v );

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
		};
	}
}

#endif
