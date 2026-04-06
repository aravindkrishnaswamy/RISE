//////////////////////////////////////////////////////////////////////
//
//  GGXSPF.h - GGX microfacet SPF with anisotropic VNDF importance
//    sampling (Dupuy & Benyoub 2023), height-correlated Smith G2
//    (Heitz 2014), and Kulla-Conty multiscattering (2017).
//
//  Three-lobe mixture model:
//    1. Diffuse: cosine hemisphere sampling
//    2. Specular: anisotropic VNDF sampling
//    3. Multiscatter: cosine hemisphere sampling
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GGX_SPF_
#define GGX_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"
#include "GGXBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class GGXSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			virtual ~GGXSPF();

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pAlphaX;
			const IPainter& pAlphaY;
			const IPainter& pIOR;
			const IPainter& pExtinction;

		public:
			GGXSPF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IPainter& alphaX,
				const IPainter& alphaY,
				const IPainter& ior,
				const IPainter& ext
				);

			void	Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack* const ior_stack
				) const;

			void	ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack* const ior_stack
				) const;

			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack* const ior_stack
				) const;

			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack* const ior_stack
				) const;
		};
	}
}

#endif
