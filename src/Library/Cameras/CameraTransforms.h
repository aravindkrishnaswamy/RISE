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

			static Vector3 SafeUnitForward_( const Vector3& forward )
			{
				const Scalar length = Vector3Ops::Magnitude( forward );
				return length > 0 ? forward * ( 1.0 / length ) : Vector3( 0, 0, -1 );
			}

			static Vector3 SafeUnitUp_( const Vector3& unitForward, const Vector3& up )
			{
				Vector3 candidate = up;
				if( !( Vector3Ops::Magnitude( candidate ) > 0 ) ) {
					// Choose the cardinal axis least aligned with forward. Its
					// cross product is bounded away from zero without an epsilon.
					const Scalar ax = std::fabs( unitForward.x );
					const Scalar ay = std::fabs( unitForward.y );
					const Scalar az = std::fabs( unitForward.z );
					candidate = ax <= ay && ax <= az ? Vector3( 1, 0, 0 )
					          : ay <= az             ? Vector3( 0, 1, 0 )
					                                 : Vector3( 0, 0, 1 );
				}
				candidate = Vector3Ops::Normalize( candidate );
				Vector3 right = Vector3Ops::Cross( unitForward, candidate );
				if( !( Vector3Ops::Magnitude( right ) > 0 ) ) {
					const Scalar ax = std::fabs( unitForward.x );
					const Scalar ay = std::fabs( unitForward.y );
					const Scalar az = std::fabs( unitForward.z );
					candidate = ax <= ay && ax <= az ? Vector3( 1, 0, 0 )
					          : ay <= az             ? Vector3( 0, 1, 0 )
					                                 : Vector3( 0, 0, 1 );
					right = Vector3Ops::Cross( unitForward, candidate );
				}
				right = Vector3Ops::Normalize( right );
				return Vector3Ops::Normalize( Vector3Ops::Cross( right, unitForward ) );
			}

		public:
			static void AdjustCameraForOrientation( 
				const Vector3& vForward,
				const Vector3& vUp,
				Vector3& vNewForward, 
				Vector3& vNewUp,
				const Vector3 orientation 
				)
			{
				const Vector3 unitForward = SafeUnitForward_( vForward );
				const Vector3 unitUp = SafeUnitUp_( unitForward, vUp );
				vNewForward = unitForward;
				vNewUp = unitUp;
				if( orientation.x != 0 ) {
					const Matrix4 mxRot =
						Matrix4Ops::Rotation( Vector3Ops::Cross( unitForward, unitUp ), orientation.x );

					vNewForward = Vector3Ops::Transform( mxRot, vNewForward );
					vNewUp = Vector3Ops::Transform( mxRot, vNewUp );
				}

				if( orientation.y != 0 ) {
					const Matrix4 mxRot = 
						Matrix4Ops::Rotation( unitUp, orientation.y );

					vNewForward = Vector3Ops::Transform( mxRot, vNewForward );
				}

				if( orientation.z != 0 ) {
					const Matrix4 mxRot = 
						Matrix4Ops::Rotation( unitForward, orientation.z );

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

				const Vector3 vForward = SafeUnitForward_( Vector3Ops::mkVector3( lookat, position ) );
				const Vector3 vUp = SafeUnitUp_( vForward, up );

				const Matrix4 mxRot =
					Matrix4Ops::Rotation( vUp, phi ) *
					Matrix4Ops::Rotation( Vector3Ops::Cross( vForward, vUp ), -theta );

				ptNewPosition = Point3Ops::mkPoint3( lookat, 
						Vector3Ops::Transform( mxRot, Vector3Ops::mkVector3( position, lookat ) ) );

				vNewUp = Vector3Ops::Transform( mxRot, vUp );
			}

			
		};
	}
}

#endif
