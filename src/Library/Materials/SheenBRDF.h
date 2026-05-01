//////////////////////////////////////////////////////////////////////
//
//  SheenBRDF.h - "Charlie" sheen BRDF for fabric / cloth-like
//    surfaces (the high-grazing-angle scatter that's characteristic
//    of velvet, suede, satin, brushed metal under glancing light).
//
//  Implements the Estevez & Kulla 2017 "Production Friendly
//  Microfacet Sheen BRDF" (Imageworks course notes) with a
//  Charlie microfacet distribution and the Ashikhmin / "neubelt"
//  visibility approximation.  Designed as an additive lobe on top
//  of a base material, layered via CompositeMaterial in glTF
//  KHR_materials_sheen scenes.
//
//  Math (using α = roughness clamped to [1e-3, 1]):
//
//    sin²θ_h = 1 - (n·h)²
//    D_charlie(α, n·h) = (2 + 1/α) / (2π) · sin(θ_h)^(1/α)
//    V_neubelt(n·l, n·v) = 1 / (4 · (n·l + n·v − n·l · n·v))
//    f_sheen(l, v) = sheenColor · D · V
//
//  No diffuse lobe and no Fresnel on the sheen layer itself --
//  glTF's spec treats sheen as a colour-tinted grazing addition.
//
//  References:
//    - Estevez & Kulla, "Production Friendly Microfacet Sheen BRDF",
//      Imageworks SIGGRAPH 2017.
//    - Khronos KHR_materials_sheen extension spec.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_BRDF_
#define SHEEN_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SheenBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~SheenBRDF();

			const IPainter& pColor;
			const IPainter& pRoughness;

		public:
			SheenBRDF(
				const IPainter& sheenColor,
				const IPainter& sheenRoughness
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
