//////////////////////////////////////////////////////////////////////
//
//  DirectionalLight.h - Declaration of the DirectionalLight class.
//    This is light infinitely far away such that all the light rays
//    are coming parallel
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 15, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIRECTIONAL_LIGHT_
#define DIRECTIONAL_LIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"

namespace RISE
{
	namespace Implementation
	{
		class DirectionalLight : public virtual ILightPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			RISEPel		cColor;
			Vector3		vDirection;

			virtual ~DirectionalLight( );

		public:

			inline bool CanGeneratePhotons() const
			{
				return false;
			}

			inline RISEPel radiantExitance() const
			{
				return RISEPel(0,0,0);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
			{
				return (cColor * radiantEnergy);
			}

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray();
			}

			DirectionalLight( Scalar radiantEnergy_, const RISEPel& c, const Vector3& vDir );

			void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool bReceivesShadows, RISEPel& amount ) const;

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}

#endif
