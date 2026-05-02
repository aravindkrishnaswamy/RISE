//////////////////////////////////////////////////////////////////////
//
//  DirectionalLight.h - Declaration of the DirectionalLight class.
//    This is light infinitely far away such that all the light rays
//    are coming parallel.
//
//    --- Direction convention (READ THIS BEFORE AUTHORING SCENES) ---
//
//    `vDirection` (the chunk's `direction` parameter) is the vector
//    pointing FROM any surface TO the light source -- i.e., it
//    describes WHERE THE LIGHT IS, not where it shines.  A surface
//    is lit when `N · vDirection > 0`.  See ComputeDirectLighting
//    in the .cpp.
//
//    To put a light "above-front-right" of a camera that's at +Z
//    looking at the origin, use direction (0.4, 0.4, 0.7) (positive
//    Z so it lights camera-facing surfaces with normals near +Z).
//    The opposite sign ((-0.4, -0.4, -0.7)) puts the light behind
//    the asset and renders the camera-facing side dark.
//
//    Importers from foreign formats that use the "shine direction"
//    convention (glTF KHR_lights_punctual, Unity, Unreal) MUST
//    negate before passing through to AddDirectionalLight.  See
//    docs/SCENE_CONVENTIONS.md §1 for the full rationale and the
//    historical bugs this caused.
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

			inline Point3 position() const
			{
				return Point3( 0, 0, 0 );
			}

			inline RISEPel   emissionColor() const  { return cColor; }
			inline Scalar    emissionEnergy() const { return radiantEnergy; }
			inline LightType lightType() const      { return LightType::Directional; }

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				return Ray();
			}

			inline Scalar pdfDirection( const Vector3& ) const
			{
				return 0;
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
