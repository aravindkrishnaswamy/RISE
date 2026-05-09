//////////////////////////////////////////////////////////////////////
//
//  ILight.h - Interface of the Light class.  This class represents
//  lights that are specific to computer graphics (ie. hack lights)
//  Some examples of these kinds of lights are infinite point lights, 
//  infinite point, spot lights, and directional lights.  
//
//  Note that Ambient light is also a subclass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILIGHT_
#define ILIGHT_

#include "IReference.h"
#include "IKeyframable.h"
#include "IBSDF.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class IRayCaster;

	//! This is a 'hacky' light that are not physically based, such as point lights
	//! spot lights or directional lights
	class ILight :
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		virtual ~ILight(){};
		ILight(){};

	public:

		//! Asks if the light can generate photons for the purpose of photon mapping
		virtual bool CanGeneratePhotons() const = 0;

		//! Asks the light for its radiant exitance
		virtual RISEPel radiantExitance() const = 0;

		//! Asks the light for its emitted radiance in a particular direction
		virtual RISEPel emittedRadiance( const Vector3& vLightOut ) const = 0;

		//! Returns the light's world-space position (for spatial importance estimation)
		virtual Point3 position() const = 0;

		//! Asks the light to generate a random emitted photon
		virtual Ray generateRandomPhoton( const Point3& ptrand ) const = 0;

		//! Returns the solid angle PDF for the directional sampling used by generateRandomPhoton
		virtual Scalar pdfDirection( const Vector3& dir ) const = 0;

		//! Is this a positional (point/spot) light suitable for equiangular sampling?
		//! Returns false for directional and ambient lights.
		virtual bool IsPositionalLight() const { return false; }

		//! Returns the primary emission direction (unit vector).
		//! Default is (0,1,0); overridden by directional lights (e.g. spot).
		virtual Vector3 emissionDirection() const { return Vector3(0,1,0); }

		//! Returns the half-angle (radians) of the emission cone.
		//! PI means full-sphere (isotropic, e.g. point lights).
		virtual Scalar emissionConeHalfAngle() const { return PI; }

		//! Read-back accessors for the editable properties surfaced
		//! through `LightIntrospection`.  Default impls return
		//! sentinel values (white / unit / zero) so subclasses that
		//! haven't opted in yet still link.  Concrete light types
		//! override to return their stored color / energy fields.
		virtual RISEPel emissionColor() const { return RISEPel( 1, 1, 1 ); }
		virtual Scalar  emissionEnergy() const { return Scalar( 1 ); }

		//! Discriminator for `LightIntrospection`'s per-type rendering.
		//! Each concrete light type overrides to identify itself; the
		//! introspection layer dispatches on this rather than RTTI to
		//! keep the per-type cases declarative.  Default = Unknown so
		//! a third-party ILight subclass falls back to the common
		//! property set (position / energy / color).
		enum class LightType
		{
			Unknown      = 0,
			Point        = 1,
			Spot         = 2,
			Directional  = 3,
			Ambient      = 4,
		};
		virtual LightType lightType() const { return LightType::Unknown; }

		//! Spot-light-specific: the world-space target the cone is
		//! aimed at (`SpotLight::ptTarget`).  PRECONDITION for the
		//! returned value to be meaningful: `lightType() ==
		//! LightType::Spot`.  Default impl returns origin so callers
		//! that bypass the discriminator don't crash, but the value
		//! is a sentinel — not a real position.  `LightIntrospection`
		//! only surfaces the `target` row when the discriminator
		//! says Spot, so the panel never displays the sentinel.
		virtual Point3 emissionTarget() const { return Point3( 0, 0, 0 ); }

		//! Spot-light-specific: inner / outer cone full angles in
		//! radians.  Defaults match an isotropic point light (full
		//! sphere both).  `emissionConeHalfAngle()` already covers
		//! the outer half-angle for non-spot lights, but
		//! `LightIntrospection` needs both inner AND outer for the
		//! spot-light-specific UI.
		virtual Scalar emissionInnerAngle() const { return PI; }
		virtual Scalar emissionOuterAngle() const { return PI; }

		//! Computes direct lighting
		virtual void ComputeDirectLighting(
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,						///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,								///< [in] BRDF of the object
			const bool bReceivesShadows,					///< [in] Should shadow checking be performed?
			RISEPel& amount									///< [out] Amount of lighting
			) const = 0;

		//! Per-wavelength direct-lighting contribution at wavelength
		//! `nm`.  Used by the spectral integrators (LightSampler /
		//! BDPTIntegrator NM path) to evaluate a non-mesh (delta-
		//! position or delta-direction) light contribution with the
		//! per-NM BSDF (`brdf.valueNM`) rather than the RGB BSDF.
		//!
		//! The spectral character comes from the BSDF: each light's
		//! emission is treated as a flat scalar (Illuminant E
		//! projection of its RGB color via `ColorMath::Luminance`),
		//! matching RISE's JH-LUT-trained-with-flat-E convention
		//! (see tools/JakobHanikaLUTGen.cpp:160-174 for the rationale).
		//! This is the per-NM analog of the RGB `ComputeDirectLighting`.
		//!
		//! Default impl falls back to running the RGB version and
		//! projecting to luminance — preserves the previous (incorrect
		//! for spectral) behaviour for any out-of-tree light type.
		//! Concrete RISE light types (Ambient / Directional / Point /
		//! Spot) override this to multiply per-NM BSDF correctly.
		virtual Scalar ComputeDirectLightingNM(
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,						///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,								///< [in] BSDF of the object (per-NM eval via valueNM)
			const bool bReceivesShadows,					///< [in] Should shadow checking be performed?
			const Scalar nm									///< [in] Wavelength (nm) at which to evaluate
			) const
		{
			RISEPel amount( 0, 0, 0 );
			ComputeDirectLighting( ri, pCaster, brdf, bReceivesShadows, amount );
			(void)nm;  // default fallback discards wavelength
			return Scalar(0.2126) * amount.r + Scalar(0.7152) * amount.g + Scalar(0.0722) * amount.b;
		}
	};
}

#include "IRayCaster.h"

#endif
