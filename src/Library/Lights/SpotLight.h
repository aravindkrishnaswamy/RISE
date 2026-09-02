//////////////////////////////////////////////////////////////////////
//
//  SpotLight.h - Declaration of the PointLight class.
//    This is an infinitismally small spot light
//    that casts light in a particular direction and has a particular
//    angle of influence
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 15, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPOTLIGHT_
#define SPOTLIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"
#include "../Utilities/GeometricUtilities.h"

namespace RISE
{
	namespace Implementation
	{
		class SpotLight : public virtual ILightPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			Point3		ptPosition;
			Point3		ptTarget;
			Scalar		dInnerAngle;		// full cone angle for the core of the light
			Scalar		dOuterAngle;		// full cone angle for the outside reaches of the light
			RISEPel		cColor;
			bool		bShootPhotons;		///< Should this light shoot photons for photon mapping?

			Vector3		vDirection;

			//! `cColor` uplifted as a RADIANCE SOURCE (Stage C slice 2).
			//! See PointLight::cSpectrum for the convention and for the
			//! staleness argument (colour is writable only through
			//! `SetIntermediateValue( COLOR_ID )`, which both keyframes and
			//! the SceneEditor / agent light-edit tools funnel through).
			RGBIlluminantSpectrum	cSpectrum;

			//! Rebuild `cSpectrum` from `cColor`.  Call after ANY write to
			//! `cColor`.
			void RefreshSpectrum();

			virtual ~SpotLight( );

		public:

			inline bool CanGeneratePhotons() const override
			{
				return bShootPhotons;
			}

			inline void SetCanGeneratePhotons( bool b ) override { bShootPhotons = b; }

			inline bool IsPositionalLight() const override { return true; }

			inline Vector3 emissionDirection() const override { return vDirection; }
			inline Scalar emissionConeHalfAngle() const override { return dOuterAngle / 2.0; }

			inline RISEPel radiantExitance() const override
			{
				// Integrate emittedRadiance over the emission solid angle.
				// dInnerAngle/dOuterAngle are full cone angles; half-angles
				// are used for the actual cone geometry.
				const Scalar halfInner = dInnerAngle / 2.0;
				const Scalar halfOuter = dOuterAngle / 2.0;

				// Inner cone: full power, solid angle = 2pi(1 - cos(halfInner))
				Scalar solidAngle = 1.0 - cos( halfInner );

				// Falloff zone: t^2 weighting from halfInner to halfOuter
				if( halfOuter > halfInner ) {
					const unsigned int N = 32;
					const Scalar dTheta = (halfOuter - halfInner) / Scalar(N);
					Scalar falloffIntegral = 0;
					for( unsigned int i = 0; i < N; i++ ) {
						const Scalar theta = halfInner + (Scalar(i) + 0.5) * dTheta;
						const Scalar t = (halfOuter - theta) / (halfOuter - halfInner);
						falloffIntegral += t * t * sin( theta ) * dTheta;
					}
					solidAngle += falloffIntegral;
				}

				return cColor * radiantEnergy * TWO_PI * solidAngle;
			}

			inline Point3 position() const override
			{
				return ptPosition;
			}

			inline RISEPel   emissionColor() const override  { return cColor; }
			inline Scalar    emissionEnergy() const override { return radiantEnergy; }
			inline LightType lightType() const override      { return LightType::Spot; }
			inline Point3    emissionTarget() const override { return ptTarget; }
			inline Scalar    emissionInnerAngle() const override { return dInnerAngle; }
			inline Scalar    emissionOuterAngle() const override { return dOuterAngle; }

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const override
			{
				// Find the angle between the light out and vDirection.
				// dInnerAngle/dOuterAngle are full cone angles, so we
				// compare against half of each (matching ComputeDirectLighting).
				const Scalar cost = Vector3Ops::Dot(
					vLightOut,
					vDirection
					);
				if( cost < 0 ) {
					return RISEPel(0,0,0);
				}

				const Scalar halfInner = dInnerAngle / 2.0;
				const Scalar halfOuter = dOuterAngle / 2.0;

				const Scalar acost = acos(cost);
				if( acost <= halfInner ) {
					return (cColor * radiantEnergy);
				} else if( acost <= halfOuter ) {
					const Scalar t = (halfOuter - acost) / (halfOuter - halfInner);
					return (cColor * radiantEnergy) * (t * t);
				}

				return RISEPel(0,0,0);
			}

			//! Spectral twin of `emittedRadiance` (Stage C slice 2).  The
			//! cone geometry below MUST stay identical to the RGB version
			//! above; only the colour term differs (cached illuminant
			//! spectrum at `nm` instead of the RGB triple).
			inline Scalar emittedRadianceNM( const Vector3& vLightOut, const Scalar nm ) const override
			{
				const Scalar cost = Vector3Ops::Dot( vLightOut, vDirection );
				if( cost < 0 ) {
					return Scalar(0);
				}

				const Scalar halfInner = dInnerAngle / 2.0;
				const Scalar halfOuter = dOuterAngle / 2.0;

				const Scalar acost = acos(cost);
				if( acost <= halfInner ) {
					return cSpectrum.Eval( nm ) * radiantEnergy;
				} else if( acost <= halfOuter ) {
					const Scalar t = (halfOuter - acost) / (halfOuter - halfInner);
					return cSpectrum.Eval( nm ) * radiantEnergy * (t * t);
				}

				return Scalar(0);
			}

			inline Ray generateRandomPhoton( const Point3& ptrand ) const override
			{
				// Uniform solid angle sampling within the outer half-cone
				const Scalar halfOuter = dOuterAngle / 2.0;
				const Scalar cosAlpha = cos( halfOuter );
				const Scalar cosTheta = 1.0 - ptrand.x * (1.0 - cosAlpha);
				const Scalar sinTheta = sqrt( r_max( 0.0, 1.0 - cosTheta * cosTheta ) );
				const Scalar phi = TWO_PI * ptrand.y;

				// Local direction in cone frame (z = cone axis)
				const Vector3 localDir( cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta );

				// Transform to world space using ONB around vDirection
				OrthonormalBasis3D onb;
				onb.CreateFromW( vDirection );
				return Ray( ptPosition, Vector3(
					onb.u().x*localDir.x + onb.v().x*localDir.y + onb.w().x*localDir.z,
					onb.u().y*localDir.x + onb.v().y*localDir.y + onb.w().y*localDir.z,
					onb.u().z*localDir.x + onb.v().z*localDir.y + onb.w().z*localDir.z ) );
			}

			inline Scalar pdfDirection( const Vector3& dir ) const override
			{
				const Scalar halfOuter = dOuterAngle / 2.0;
				const Scalar cost = Vector3Ops::Dot( dir, vDirection );
				if( cost <= 0 ) return 0;
				if( acos( r_min( 1.0, cost ) ) > halfOuter ) return 0;
				return Scalar(1.0) / (TWO_PI * (1.0 - cos( halfOuter )));
			}

			SpotLight(
				const Scalar radiantEnergy_,
				const Point3& target,
				const Scalar inner,
				const Scalar outer,
				const RISEPel& c,
				const bool shootPhotons
				);

			//! `bFullSphereReceiver` (residual wave 2 item D): no-op, for
			//! the identical reason PointLight's own override is a
			//! no-op -- see PointLight.h's doc.
			//!
			//! `bVolumeReceiver` (residual-ledger item 10 of
			//! docs/PT_ENV_MIS_DOUBLECOUNT.md, 2026-08-27) IS implemented,
			//! again exactly as PointLight's is and for the same
			//! sibling-site reason (docs/skills/audit-by-bug-pattern.md).
			//! When true the receiver cosine and its hemisphere gate are
			//! dropped; `invDistSq` AND the spot CONE FALLOFF are both
			//! KEPT, since each is a property of the EMITTER (its
			//! inverse-square geometry and its angular emission profile),
			//! not of the receiver's orientation.  A volume element inside
			//! the cone is lit by exactly the cone-shaped emission a
			//! surface patch there would see.
			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount, const bool bFullSphereReceiver = false, const bool bVolumeReceiver = false ) const override;

			//! Per-wavelength direct lighting (cone falloff + wavelength-
			//! specific transparent-shadow Fresnel).  See PointLight /
			//! DirectionalLight; overrides the ILight RGB-projection default.
			//! `bFullSphereReceiver`: no-op, see the RGB override above.
			//! `bVolumeReceiver`: implemented, see the RGB override above.
			Scalar	ComputeDirectLightingNM( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, const Scalar nm, const bool bFullSphereReceiver = false, const bool bVolumeReceiver = false ) const override;

			// Overrides the PARENT-COMPOSED overload only -- Transformable's
			// no-argument form delegates here, so ptPosition / vDirection are
			// refreshed on every finalize, hierarchy-composed or not.
			void	FinalizeTransformations( const Matrix4& parentWorld ) override;
			using Transformable::FinalizeTransformations;

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData() override
			{
				Transformable::RegenerateData();
				vDirection = Vector3Ops::Normalize(Vector3Ops::mkVector3(ptTarget,ptPosition));
			}
		};
	}
}

#endif
