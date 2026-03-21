//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringBSDF.h - Defines a BSDF for BSSRDF-based
//  subsurface scattering materials.
//
//  With the BSSRDF approach, the BSDF only handles surface
//  reflection for BDPT connection strategies:
//
//  Both outside (front/front): GGX microfacet reflection BRDF
//    (rough surfaces) or diffuse Fresnel approximation (smooth).
//  All other cases: return 0.  The subsurface transport between
//    surface points is handled by the diffusion profile in the
//    integrator, not by the BSDF.
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
