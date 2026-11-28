//////////////////////////////////////////////////////////////////////
//
//  RadianceMap.h - Implementation of the radiance map
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 16, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RADIANCE_MAP_
#define RADIANCE_MAP_

#include "../Interfaces/IRadianceMap.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Profiling.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
        // Angular-probe source mapping. Keep azimuth separate from the polar
        // angle so subnormal transverse components cannot produce 0/0 or an
        // overflowing reciprocal. -Z is the disk centre; +Z is the disk edge,
        // with canonical azimuth zero when the transverse vector is exactly 0.
        inline Point2 LightProbeTextureCoordinates(const Vector3& direction)
        {
            const Scalar horizontal = r_max(std::abs(direction.x),std::abs(direction.y));
            if (horizontal == 0) return direction.z > 0 ? Point2(1,.5) : Point2(.5,.5);
            int horizontalExponent = 0, polarExponent = 0;
            std::frexp(horizontal,&horizontalExponent);
            const Scalar hx = std::ldexp(direction.x,-horizontalExponent);
            const Scalar hy = std::ldexp(direction.y,-horizontalExponent);
            const Scalar hn = std::hypot(hx,hy);
            std::frexp(r_max(horizontal,std::abs(direction.z)),&polarExponent);
            const Scalar theta = std::atan2(std::ldexp(hn,horizontalExponent-polarExponent),
                -std::ldexp(direction.z,-polarExponent));
            const Scalar radius = theta/TWO_PI;
            return Point2(.5+(hx/hn)*radius,.5-(hy/hn)*radius);
        }

		class RadianceMap : public virtual IRadianceMap, public virtual Reference
		{
		protected:
			const IPainter&				pRadianceMap;
			Scalar						dScale;
			Matrix4						mxtransform;

			virtual ~RadianceMap()
			{
				pRadianceMap.release();
			}

		public:
			RadianceMap( 
				const IPainter& p,
				const Scalar scale
				) :
			pRadianceMap( p ),
			dScale( scale )
			{
				pRadianceMap.addref();
				mxtransform = Matrix4Ops::Identity();
			}

			//! Returns the radiance from that direction in the scene
			/// \return The radiance
			RISEPel GetRadiance(
				const Ray& ray,
				const RasterizerState& rast
				) const
			{
				RISE_PROFILE_PHASE(RadianceMap);
				RISE_PROFILE_INC(nRadianceMapLookups);

				// Transform the world co-ordinates to the texture lookup
				Vector3 v = Vector3Ops::Transform( mxtransform, ray.Dir() );
                RayIntersectionGeometric rig( ray, rast );
                rig.ptCoord = LightProbeTextureCoordinates(v);

				return pRadianceMap.GetRadianceColor( rig ) * dScale;
			}

			//! Returns the radiance from that direction for the given wavelength
			Scalar GetRadianceNM(
				const Ray& ray,
				const RasterizerState& rast,
				const Scalar nm
				) const
			{
				RISE_PROFILE_PHASE(RadianceMap);
				RISE_PROFILE_INC(nRadianceMapLookups);

				Vector3 v = Vector3Ops::Transform( mxtransform, ray.Dir() );
                RayIntersectionGeometric rig( ray, rast );
                rig.ptCoord = LightProbeTextureCoordinates(v);
				
				// A radiance map IS a source term, so the painter is read
				// through GetRadianceNM (Stage C slice 2): the map's RGB is
				// "the colour the sky has", i.e. reflectance-under-D65 times
				// D65.  This covers every painter-backed environment --
				// uniform dome colours (including the AgentSession studio
				// dome), HDR/EXR lat-longs through TexturePainter, and
				// procedural sky painters.  HosekWilkieSpectralRadianceMap
				// is a separate IRadianceMap with a PHYSICAL SPD and does
				// not come through here.
				return pRadianceMap.GetRadianceNM( rig, nm ) * dScale;
			}

			//! Sets the orientation of this map
			void SetOrientation( 
				const Vector3& orient			///< [in] Euler angles for the orientation
				)
			{
				mxtransform = Matrix4Ops::XRotation( orient.x ) * 
				Matrix4Ops::YRotation( orient.y ) * 
				Matrix4Ops::ZRotation( orient.z );
			}

			//! Sets the orientation of this map from the given matrix
			void SetTransformation(
				const Matrix4& mx				///< [in] Transformation matrix for the map
				)
			{
				mxtransform = mx;
			}

			const IPainter& GetPainter() const
			{
				return pRadianceMap;
			}

			Scalar GetScale() const
			{
				return dScale;
			}

			void SetScale( const Scalar scale )
			{
				dScale = scale;
			}

			const Matrix4& GetTransform() const
			{
				return mxtransform;
			}

		};
	}
}


#endif

