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

			inline Point3 position() const
			{
				return Point3( 0, 0, 0 );
			}

			inline RISEPel   emissionColor() const  { return cColor; }
			inline Scalar    emissionEnergy() const { return radiantEnergy; }
			inline LightType lightType() const      { return LightType::Ambient; }

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray();
			}

			inline Scalar pdfDirection( const Vector3& ) const
			{
				return 0;
			}

			inline void	ComputeDirectLighting( const RayIntersectionGeometric& ri, const IRayCaster&, const IBSDF& brdf, const bool, RISEPel& amount ) const
			{
				amount = cColor * radiantEnergy * brdf.value( ri.vNormal, ri );
			}

			//! Per-wavelength evaluation: project the light color to
			//! luminance (flat-E projection) and multiply by the per-NM
			//! BSDF.  Matches the RGB version but uses brdf.valueNM,
			//! preserving the surface's spectral character (which the
			//! previous Luminance(amount_RGB) projection collapsed to
			//! white because amount_RGB had the per-NM BSDF replaced by
			//! its RGB equivalent).
			inline Scalar ComputeDirectLightingNM(
				const RayIntersectionGeometric& ri,
				const IRayCaster&,
				const IBSDF& brdf,
				const bool,
				const Scalar nm
				) const
			{
				const Scalar lightLum =
					Scalar(0.2126) * cColor.r +
					Scalar(0.7152) * cColor.g +
					Scalar(0.0722) * cColor.b;
				return lightLum * radiantEnergy * brdf.valueNM( ri.vNormal, ri, nm );
			}

			inline void	FinalizeTransformations(){Transformable::FinalizeTransformations();};

			// For keyframamble interface
			// Keyframe parameter IDs.  The IDs MUST match between
			// `KeyframeFromParameters` (allocator) and
			// `SetIntermediateValue` (consumer) — historically this
			// class shipped with the allocator using 1000/1001 and
			// the consumer using 100/101, so every ambient color /
			// energy edit silently no-op'd while reporting success
			// (the keyframe got built, but the consumer's switch
			// fell through and changed nothing).  Match PointLight's
			// 100/101 convention now that the divergence is fixed.
			static const unsigned int kColorID  = 100;
			static const unsigned int kEnergyID = 101;

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{
				IKeyframeParameter* p = 0;

				// Check the name and see if its something we recognize
				if( name == "color" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						p = new Parameter<RISEPel>( RISEPel(d), kColorID );
					}
				} else if( name == "energy" ) {
					p = new Parameter<Scalar>( atof(value.c_str()), kEnergyID );
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
				case kColorID:
					{
						cColor = *(RISEPel*)val.getValue();
					}
					break;
				case kEnergyID:
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
