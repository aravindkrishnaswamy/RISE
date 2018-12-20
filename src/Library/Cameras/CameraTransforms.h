//////////////////////////////////////////////////////////////////////
//
//  CameraTransforms.h - Transformation helpers for cameras
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAMERA_TRANSFORMS_
#define CAMERA_TRANSFORMS_

namespace RISE
{
	namespace Implementation
	{
		class CameraTransforms
		{
		protected:
			CameraTransforms(){};
			virtual ~CameraTransforms(){};

		public:
			static void AdjustCameraForOrientation( 
				const Vector3& vForward,
				const Vector3& vUp,
				Vector3& vNewForward, 
				Vector3& vNewUp,
				const Vector3 orientation 
				)
			{
				vNewForward = vForward;
				vNewUp = vUp;
				if( orientation.x != 0 ) {
					const Matrix4 mxRot =
						Matrix4Ops::Rotation( Vector3Ops::Cross( vForward, vUp ), orientation.x );

					vNewForward = Vector3Ops::Transform( mxRot, vNewForward );
					vNewUp = Vector3Ops::Transform( mxRot, vNewUp );
				}

				if( orientation.y != 0 ) {
					const Matrix4 mxRot = 
						Matrix4Ops::Rotation( vUp, orientation.y );

					vNewForward = Vector3Ops::Transform( mxRot, vNewForward );
				}

				if( orientation.z != 0 ) {
					const Matrix4 mxRot = 
						Matrix4Ops::Rotation( vForward, orientation.z );

					vNewUp = Vector3Ops::Transform( mxRot, vNewUp );
				}
			}

			static void AdjustCameraForThetaPhi( 
				const Vector2& target_orientation, 
				const Point3& position, 
				const Point3& lookat,
				const Vector3& up,
				Point3& ptNewPosition,
				Vector3& vNewUp
				)
			{
				Scalar theta = target_orientation.x;
				theta = theta > PI_OV_TWO ? PI_OV_TWO : theta;

				const Scalar phi = target_orientation.y;

				const Vector3 vForward = Vector3Ops::Normalize(Vector3Ops::mkVector3( lookat, position ));

				const Matrix4 mxRot =
					Matrix4Ops::Rotation( up, phi ) * 
					Matrix4Ops::Rotation( Vector3Ops::Cross( vForward, up ), -theta );

				ptNewPosition = Point3Ops::mkPoint3( lookat, 
						Vector3Ops::Transform( mxRot, Vector3Ops::mkVector3( position, lookat ) ) );

				vNewUp = Vector3Ops::Transform( mxRot, up );
			}

			
		};
	}
}

#endif
