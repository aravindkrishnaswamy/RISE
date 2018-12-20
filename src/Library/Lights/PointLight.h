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
			Scalar		linearAttenuation;
			Scalar		quadraticAttenuation;

			virtual ~PointLight( );

		public:
			inline bool CanGeneratePhotons() const
			{
				return true;
			}

			inline RISEPel radiantExitance() const
			{
				return (cColor * radiantEnergy * FOUR_PI);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
			{
				return (cColor * radiantEnergy);
			}

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray(ptPosition, GeometricUtilities::Perturb(
					Vector3( 0, 1, 0 ), 
					PI*ptrand.x, ptrand.y*TWO_PI)
					);
			}

			PointLight( 
				const Scalar radiantEnergy_,
				const RISEPel& c,
				const Scalar linearAtten,
				const Scalar quadraticAtten
				);

			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount ) const;
			void	FinalizeTransformations();

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}

#endif
