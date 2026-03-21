//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringBSDF.h - Defines a BSDF for subsurface
//  scattering materials, enabling direct lighting evaluation in
//  BDPT connection strategies.
//
//  Front face: diffuse Fresnel approximation (Schlick R0)
//  Back face (inside medium): HG phase function evaluation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_BSDF_
#define SUBSURFACE_SCATTERING_BSDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringBSDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~SubSurfaceScatteringBSDF();

			const IPainter&		ior;
			const IPainter&		absorption;
			const IPainter&		scattering;
			const Scalar		g;
			const Scalar		roughness;
			const Scalar		alpha;		// GGX alpha = roughness^2

		public:
			SubSurfaceScatteringBSDF(
				const IPainter& ior_,
				const IPainter& absorption_,
				const IPainter& scattering_,
				const Scalar g_,
				const Scalar roughness_
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif
