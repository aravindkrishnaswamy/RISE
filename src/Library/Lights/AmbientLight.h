//////////////////////////////////////////////////////////////////////
//
//  AmbientLight.h - Ambient light is a hacky light that just
//  returns a constant value of illumination.  It is used only in the
//  ray tracer since the ray tracer is not capable of global illumination
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 01, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AMBIENT_LIGHT_
#define AMBIENT_LIGHT_

#include "../Interfaces/ILightPriv.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Transformable.h"
#include "../Animation/KeyframableHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class AmbientLight : 
			public virtual ILightPriv, 
			public virtual Transformable, 
			public virtual Reference
		{
		protected:
			Scalar		radiantEnergy;
			RISEPel		cColor;

			virtual ~AmbientLight( ){};

		public:
			AmbientLight( Scalar radiantEnergy_, const RISEPel& c  ) : radiantEnergy( radiantEnergy_ ), cColor( c ){};

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

			inline void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool, RISEPel& amount ) const
			{
				amount = cColor * radiantEnergy * brdf.value( ri.vNormal, ri );
			}

			inline void	FinalizeTransformations(){Transformable::FinalizeTransformations();};

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{
				IKeyframeParameter* p = 0;

				// Check the name and see if its something we recognize
				if( name == "color" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						p = new Parameter<RISEPel>( RISEPel(d), 1000 );
					}
				} else if( name == "energy" ) {
					p = new Parameter<Scalar>( atof(value.c_str()), 1001 );
				} else {
					return Transformable::KeyframeFromParameters( name, value );
				}

				GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
				return p;
			}

			void SetIntermediateValue( const IKeyframeParameter& val )
			{
				switch( val.getID() )
				{
				case 100:
					{
						cColor = *(RISEPel*)val.getValue();
					}
					break;
				case 101:
					{
						radiantEnergy = *(Scalar*)val.getValue();
					}
					break;
				}

				Transformable::SetIntermediateValue( val );
			}
		};
	}
}

#endif
