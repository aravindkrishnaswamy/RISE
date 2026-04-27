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
				// Symmetric ±gimbal-margin clamp on theta.
				//
				// At theta = ±π/2 the post-orbit forward becomes
				// parallel to `up`; the cross(vForward, up) below
				// collapses to the zero vector and Normalize returns
				// zero, so the basis used by callers degenerates and
				// downstream pan/zoom math propagates NaN.
				//
				// The 1° margin (1.553343 ≈ 89°) reflects the
				// nonlinear conditioning of `Cross` near collinearity
				// — loss of significance starts before exactly π/2,
				// not at it.  Conservative-but-cheap.
				//
				// Historically only the +π/2 side was clamped; the
				// negative side was unbounded and a long
				// drag-up-orbit would push past the south pole and
				// produce NaN.  Clamping symmetrically here means
				// every caller — interactive Orbit, panel theta scrub,
				// keyframe interpolation, future entry points — is
				// protected without each one having to remember.
				static const Scalar kThetaLimit = Scalar( 1.553343 );  // ~89° in rad
				Scalar theta = target_orientation.x;
				if( theta >  kThetaLimit ) theta =  kThetaLimit;
				if( theta < -kThetaLimit ) theta = -kThetaLimit;

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
