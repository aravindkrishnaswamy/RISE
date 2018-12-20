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

namespace RISE
{
	namespace Implementation
	{
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
				// Transform the world co-ordinates to the texture lookup
				Vector3 v = Vector3Ops::Transform( mxtransform, ray.dir );
				const Scalar r = 0.159154943*acos(-v.z)/sqrt(v.x*v.x + v.y*v.y);

				RayIntersectionGeometric rig( ray, rast );
				rig.ptCoord.x = 0.5 + v.x * r;
				rig.ptCoord.y = 0.5 - v.y * r;
				
				return pRadianceMap.GetColor( rig ) * dScale;
			}

			//! Returns the radiance from that direction for the given wavelength
			Scalar GetRadianceNM(
				const Ray& ray,
				const RasterizerState& rast,
				const Scalar nm
				) const
			{
				Vector3 v = Vector3Ops::Transform( mxtransform, ray.dir );
				const Scalar r = 0.159154943*acos(-v.z)/sqrt(v.x*v.x + v.y*v.y);

				RayIntersectionGeometric rig( ray, rast );
				rig.ptCoord.x = 0.5 + v.x * r;
				rig.ptCoord.y = 0.5 - v.y * r;
				
				return pRadianceMap.GetColorNM( rig, nm ) * dScale;
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

		};
	}
}


#endif

