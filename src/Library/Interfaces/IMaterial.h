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
#include "SpecularInfo.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	class ISPF;
	class IBSDF;
	class IEmitter;
	class ISubSurfaceDiffusionProfile;
	class RayIntersectionGeometric;
	class IORStack;

	//! Selects which Fresnel model GGX-family materials evaluate internally.
	//! Visible at the public API level so RISE_API_CreateGGXMaterial /
	//! Job::AddGGXMaterial / parser layers can pass through the selection
	//! without dragging in Implementation headers.
	//!
	//!   eFresnelConductor (default — preserves existing behaviour):
	//!     Optics::CalculateConductorReflectance(Ni=1, ior, ext) at the
	//!     half-vector, multiplied with `specular` painter as a tinting
	//!     factor.  Use for hand-authored materials with real ior /
	//!     extinction values.
	//!
	//!   eFresnelSchlickF0:
	//!     Treats `specular` painter as F0 directly; uses Schlick's
	//!     approximation `F = F0 + (1-F0)(1-cosθ_h)^5`.  Required by
	//!     glTF metallicRoughness PBR mapping.  In this mode `ior` /
	//!     `ext` painters are ignored, the diffuse lobe is modulated by
	//!     `(1 - max(F0))` per glTF spec, and the multiscatter lobe uses
	//!     the closed-form Schlick hemispherical Fresnel average
	//!     `F_avg = F0 + (1-F0)/21`.
	enum FresnelMode
	{
		eFresnelConductor = 0,
		eFresnelSchlickF0 = 1
	};

	/// Parameters for random-walk subsurface scattering.
	/// Stored on the material and queried by the integrators.
	struct RandomWalkSSSParams
	{
		RISEPel			sigma_a;		///< Absorption coefficient [1/m]
		RISEPel			sigma_s;		///< Scattering coefficient [1/m]
		RISEPel			sigma_t;		///< Extinction coefficient [1/m]
		Scalar			g;				///< HG asymmetry factor (-1 to 1)
		Scalar			ior;			///< Index of refraction at boundary
		unsigned int	maxBounces;		///< Maximum walk steps
		Scalar			boundaryFilter;	///< Multiplicative weight for boundary
										///< absorption layers (e.g. melanin
										///< double-pass).  Default 1.0.
		Scalar			maxDepth;		///< Maximum walk depth below the entry
										///< surface [scene units].  Walks that
										///< scatter beyond this depth are
										///< terminated (absorbed).  0 = unlimited.
		RandomWalkSSSParams() : g(0), ior(1.0), maxBounces(64), boundaryFilter(1.0), maxDepth(0) {}
	};

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
		/// glass or other transmissive media).
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

		/// \return Parameters for random-walk subsurface scattering.
		/// NULL if this material does not use random-walk SSS.
		/// When non-NULL, the integrator traces a volumetric random
		/// walk inside the mesh instead of using disk-projection
		/// sampling.  Mutually exclusive with GetDiffusionProfile().
		virtual const RandomWalkSSSParams* GetRandomWalkSSSParams() const { return 0; }

		/// Compute wavelength-dependent random-walk SSS parameters.
		/// Returns true if this material provides spectral RW SSS.
		///
		/// Used by materials whose scattering coefficients vary
		/// strongly with wavelength (e.g. BioSpec skin), where
		/// packing 3 wavelengths into RGB channels produces
		/// intolerable per-channel weight variance.  These
		/// materials return NULL from GetRandomWalkSSSParams()
		/// (disabling RGB mode) and implement this method for
		/// the spectral (NM) rendering path.
		///
		/// The output params_out has all 3 RGB channels set to
		/// the same scalar value for the requested wavelength,
		/// so the walk's luminance-derived NM path uses the
		/// correct per-wavelength extinction.
		///
		/// \param nm         Wavelength in nm
		/// \param params_out Filled with coefficients at this wavelength
		/// \return true if spectral RW params are available
		virtual bool GetRandomWalkSSSParamsNM(
			const Scalar nm,
			RandomWalkSSSParams& params_out
			) const { return false; }

		/// \return Information about this material's specular (delta) behavior.
		/// Used by the specular manifold sampling solver to determine constraint
		/// type and IOR at each specular vertex in the chain.
		/// Default returns non-specular.  Materials with delta interactions
		/// (DielectricMaterial, PerfectReflectorMaterial, etc.) override this.
		virtual SpecularInfo GetSpecularInfo(
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack
			) const
		{
			return SpecularInfo();
		}

		/// \return Spectral variant of GetSpecularInfo for wavelength-dependent IOR.
		/// Default delegates to GetSpecularInfo.  DielectricMaterial overrides
		/// to use wavelength-specific IOR for dispersion.
		virtual SpecularInfo GetSpecularInfoNM(
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack,
			const Scalar nm
			) const
		{
			return GetSpecularInfo( ri, ior_stack );
		}

		/// \return The PDF (probability density function) for scattering from
		/// the incoming direction (given by ri.ray.Dir()) to the outgoing
		/// direction wo, in solid angle measure [1/sr].
		/// Delegates to the SPF's Pdf() method.  Materials whose SPF implements
		/// Pdf() (e.g. LambertianSPF) will participate in MIS automatically.
		/// Materials whose SPF returns 0 (the default) effectively disable MIS,
		/// falling back to unweighted NEE — a safe default.
		virtual Scalar Pdf(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack
			) const;

		/// Spectral variant of Pdf.
		virtual Scalar PdfNM(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const Scalar nm,
			const IORStack& ior_stack
			) const;
	};
}

#include "IBSDF.h"
#include "ISPF.h"
#include "IEmitter.h"


#endif
