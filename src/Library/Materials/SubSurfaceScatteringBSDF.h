//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringBSDF.h - Defines a BSDF for BSSRDF-based
//  subsurface scattering materials.
//
//  With the BSSRDF approach, the BSDF only handles surface
//  reflection for BDPT connection strategies:
//
//  Both outside (front/front): GGX microfacet reflection BRDF
//    (rough surfaces); smooth reflection is delta, with zero finite value.
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
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IScalarPainter*	pIOR;
			const Scalar			g;
			const Scalar			roughness;
			const Scalar			alpha;		// GGX alpha = roughness^2

		public:
			SubSurfaceScatteringBSDF(
				const IScalarPainter& ior_,
				const Scalar g_,
				const Scalar roughness_
				);

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const override;
			Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const override;
			RISEPel albedo( const RayIntersectionGeometric& ri ) const override;
			//! The SPF labels every ordinary finite term as reflection. Jump
			//! transport is absent from this BSDF and must not enter the split.
			bool valueByScatterType(const Vector3& w, const RayIntersectionGeometric& ri,
				const IORStack*, RISEPel out[5]) const override {
				for(unsigned t=0;t<5;++t) out[t]=RISEPel(0,0,0);
				out[ScatteredRay::eRayReflection]=value(w,ri);
				return true;
			}
			bool valueByScatterTypeNM(const Vector3& w, const RayIntersectionGeometric& ri,
				Scalar nm, const IORStack*, Scalar out[5]) const override {
				for(unsigned t=0;t<5;++t) out[t]=0;
				out[ScatteredRay::eRayReflection]=valueNM(w,ri,nm);
				return true;
			}


			//! Read-back + rebind for the interactive editor.
			inline const IScalarPainter& GetIOR() const { return *pIOR; }
			void SetIOR( const IScalarPainter& v );
		};
	}
}

#endif
