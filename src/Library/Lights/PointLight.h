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
			inline bool CanGeneratePhotons() const
			{
				return bShootPhotons;
			}

			inline RISEPel radiantExitance() const
			{
				return (cColor * radiantEnergy * FOUR_PI);
			}

			inline RISEPel emittedRadiance( const Vector3& vLightOut ) const
			{
				return (cColor * radiantEnergy);
			}

			inline Point3 position() const
			{
				return ptPosition;
			}

			inline Ray generateRandomPhoton( const Point3& ptrand ) const
			{
				// Uniform sampling on the full sphere
				const Scalar cosTheta = 1.0 - 2.0 * ptrand.x;
				const Scalar sinTheta = sqrt( r_max( 0.0, 1.0 - cosTheta * cosTheta ) );
				const Scalar phi = TWO_PI * ptrand.y;
				return Ray( ptPosition,
					Vector3( cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta ) );
			}

			inline Scalar pdfDirection( const Vector3& ) const
			{
				return Scalar(1.0) / FOUR_PI;
			}

			PointLight(
				const Scalar radiantEnergy_,
				const RISEPel& c,
				const bool shootPhotons
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
