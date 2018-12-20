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
			Scalar		dInnerAngle;		// angle for the core of the light
			Scalar		dOuterAngle;		// angle for the very outside reaches of the light
			RISEPel		cColor;
			Scalar		linearAttenuation;
			Scalar		quadraticAttenuation;

			Vector3		vDirection;

			virtual ~SpotLight( );

		public:

			inline bool CanGeneratePhotons() const
			{
				return true;
			}

			inline RISEPel radiantExitance() const
			{
				return (cColor * radiantEnergy * TWO_PI * (TWO_PI*(dOuterAngle/TWO_PI)));
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
			{
				// Find the angle between the light out and vDirection
				const Scalar cost = Vector3Ops::Dot( 
					vLightOut,
					vDirection
					);
				if( cost < 0 ) {
					return RISEPel(0,0,0);
				}

				/*  Test code, We need to compensate for this in the radiantExitance or bad things will happen
				const Scalar acost = acos(cost);
				if( acost < dInnerAngle ) {
					return (cColor * radiantEnergy);
				} else if( acost < dOuterAngle ) {
					return (cColor * radiantEnergy) * ((acost-dInnerAngle)(dOuterAngle-dInnerAngle)));
				}
				*/

				const Scalar acost = acos(cost);
				if( acost <= dOuterAngle ) {
					return (cColor * radiantEnergy);
				}

				return RISEPel(0,0,0);
			}

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray(ptPosition, GeometricUtilities::Perturb(
					vDirection, 
					dOuterAngle*ptrand.x, ptrand.y*TWO_PI)
					);
			}

			SpotLight( 
				const Scalar radiantEnergy_,
				const Point3& target,
				const Scalar inner, 
				const Scalar outer, 
				const RISEPel& c,
				const Scalar linearAtten,
				const Scalar quadraticAtten
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
