//////////////////////////////////////////////////////////////////////
//
//  PointLight.h - Declaration of the PointLight class.
//    This is an infinitismally small point light
//    that casts light isotropically in all directions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POINTLIGHT_
#define POINTLIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"
#include "../Utilities/GeometricUtilities.h"

namespace RISE
{
	namespace Implementation
	{
		class PointLight : public virtual ILightPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			Point3		ptPosition;
			RISEPel		cColor;
			bool		bShootPhotons;		///< Should this light shoot photons for photon mapping?

			virtual ~PointLight( );

		public:
			inline bool CanGeneratePhotons() const override
			{
				return bShootPhotons;
			}

			inline void SetCanGeneratePhotons( bool b ) override { bShootPhotons = b; }

			inline bool IsPositionalLight() const override { return true; }

			inline RISEPel radiantExitance() const override
			{
				return (cColor * radiantEnergy * FOUR_PI);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const override
			{
				return (cColor * radiantEnergy);
			}

			inline Point3 position() const override
			{
				return ptPosition;
			}

			inline RISEPel   emissionColor() const override  { return cColor; }
			inline Scalar    emissionEnergy() const override { return radiantEnergy; }
			inline LightType lightType() const override      { return LightType::Point; }

			inline Ray generateRandomPhoton( const Point3& ptrand ) const override
			{
				// Uniform sampling on the full sphere
				const Scalar cosTheta = 1.0 - 2.0 * ptrand.x;
				const Scalar sinTheta = sqrt( r_max( 0.0, 1.0 - cosTheta * cosTheta ) );
				const Scalar phi = TWO_PI * ptrand.y;
				return Ray( ptPosition,
					Vector3( cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta ) );
			}

			inline Scalar pdfDirection( const Vector3& ) const override
			{
				return Scalar(1.0) / FOUR_PI;
			}

			PointLight(
				const Scalar radiantEnergy_,
				const RISEPel& c,
				const bool shootPhotons
				);

			//! `bFullSphereReceiver` (residual wave 2 item D) is a no-op
			//! here: this virtual is never the path a full-sphere
			//! material's point-light NEE goes through -- LightSampler's
			//! proportional-selection site evaluates delta-position
			//! lights (point/spot) INLINE and is already capability-
			//! gated there (see EvaluateDirectLighting's FULL-SPHERE NEE
			//! site 1 of 3).  Only Step 1's zero-exitance sweep
			//! (ambient/directional, radiantExitance() == 0) and BDPT's
			//! mirroring s==1 row call this virtual at all, and neither
			//! ever holds a point/spot light -- both always have nonzero
			//! exitance.  Accepted for interface conformance only.
			//!
			//! `bVolumeReceiver` (residual-ledger item 10 of
			//! docs/PT_ENV_MIS_DOUBLECOUNT.md, 2026-08-27) is a different
			//! story and IS implemented for real below, even though the
			//! same unreachability argument applies to it verbatim.  The
			//! reason is the sibling-site bug pattern
			//! docs/skills/audit-by-bug-pattern.md exists to prevent: a
			//! defaulted flag whose contract one implementor silently
			//! violates is a trap for the next caller who wires it up.
			//! When true, the receiver cosine `fDot` and its `<= 0`
			//! hemisphere gate are both dropped (a phase-function vertex
			//! has no normal to project onto); `invDistSq` is KEPT, since
			//! the inverse-square falloff is a property of the EMITTER's
			//! geometry, not of the receiver's orientation, and applies
			//! to a volume element exactly as it does to a surface patch.
			//! This is the same shape as LightSampler's own inline
			//! delta-light row, which forces `cosSurface = 1.0` under
			//! `isVolumeScatter` and keeps its `invDistSq`.
			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount, const bool bFullSphereReceiver = false, const bool bVolumeReceiver = false ) const override;

			//! Per-wavelength direct lighting.  Overrides the ILight default
			//! (which projects the RGB ComputeDirectLighting to luminance and
			//! discards the wavelength): queries brdf.valueNM at @a nm and, when
			//! transparent_shadows is enabled, attenuates by the WAVELENGTH-
			//! SPECIFIC Fresnel transmittance (CastShadowRayAuto bNM=true) rather
			//! than a representative RGB IOR.  Matches DirectionalLight /
			//! AmbientLight; keeps every light's spectral NEE consistent.
			//! `bFullSphereReceiver`: no-op, see the RGB override above.
			//! `bVolumeReceiver`: implemented, see the RGB override above.
			Scalar	ComputeDirectLightingNM( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, const Scalar nm, const bool bFullSphereReceiver = false, const bool bVolumeReceiver = false ) const override;

			// Overrides the PARENT-COMPOSED overload only -- see SpotLight.h.
			void	FinalizeTransformations( const Matrix4& parentWorld ) override;
			using Transformable::FinalizeTransformations;

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
		};
	}
}

#endif
