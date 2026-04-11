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

			virtual ~SpotLight( );

		public:

			inline bool CanGeneratePhotons() const
			{
				return bShootPhotons;
			}

			inline bool IsPositionalLight() const { return true; }

			inline Vector3 emissionDirection() const { return vDirection; }
			inline Scalar emissionConeHalfAngle() const { return dOuterAngle / 2.0; }

			inline RISEPel radiantExitance() const
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

			inline Point3 position() const
			{
				return ptPosition;
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
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

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
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

			inline Scalar pdfDirection( const Vector3& dir ) const
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

			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount ) const;
			void	FinalizeTransformations();

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData()
			{
				Transformable::RegenerateData();
				vDirection = Vector3Ops::Normalize(Vector3Ops::mkVector3(ptTarget,ptPosition));
			}
		};
	}
}

#endif
