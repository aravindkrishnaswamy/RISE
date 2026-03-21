//////////////////////////////////////////////////////////////////////
//
//  IMaterial.h - Defines an interface to a material.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IMATERIAL_
#define IMATERIAL_

#include "IReference.h"

namespace RISE
{
	class ISPF;
	class IBSDF;
	class IEmitter;
	class ISubSurfaceDiffusionProfile;

	//! The IMaterial interface is basically an aggregate of other interfaces.  Though we don't actually
	//! aggregate the interfaces, in essense what is all it does
	/// \sa IBSDF
	/// \sa ISPF
	/// \sa IEmitter
	class IMaterial : public virtual IReference
	{
	protected:
		IMaterial(){};
		virtual ~IMaterial( ){};

	public:

		/// \return The BRDF for this material.  NULL If there is no BRDF
		virtual IBSDF* GetBSDF() const = 0;

		/// \return The SPF for this material.  NULL If there is no SPF
		virtual ISPF* GetSPF() const  = 0;

		/// \return The emission properties for this material.  NULL If there is not an emitter
		virtual IEmitter* GetEmitter() const  = 0;

		/// \return True if light can pass through this material (e.g.
		/// glass, translucent, SSS).  Used by BDPT visibility tests
		/// so that camera connections (t=0, t=1) aren't blocked by
		/// transparent surfaces.
		virtual bool CouldLightPassThrough() const { return false; }

		/// \return True if this material has volumetric transport (e.g. SSS).
		/// When true, BDPT should use kray for throughput instead of BSDF * cos / pdf,
		/// since the SPF's kray already includes Beer-Lambert attenuation and
		/// correct volumetric weighting that the BSDF cannot reproduce.
		virtual bool IsVolumetric() const { return false; }

		/// \return The diffusion profile for subsurface scattering.
		/// NULL if this material does not use BSSRDF-based transport.
		/// When non-NULL, the BDPT integrator performs importance-sampled
		/// probe ray casting to find entry points on the surface, using
		/// this profile's Rd(r) for weighting and sampling.
		virtual ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return 0; }
	};
}

#include "IBSDF.h"
#include "ISPF.h"
#include "IEmitter.h"

#endif
