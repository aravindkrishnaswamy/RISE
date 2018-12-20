//////////////////////////////////////////////////////////////////////
//
//  OrthographicCamera.h - Declaration of a pinhole camera, ie. a 
//  traditional perspective camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ORTHOGRAPHIC_CAMERA_
#define ORTHOGRAPHIC_CAMERA_

#include "CameraCommon.h"

namespace RISE
{
	namespace Implementation
	{
		class OrthographicCamera : 
			public virtual CameraCommon
		{
		protected:

			Vector2		viewportScale;				///< [in] Viewport scale factor

			virtual ~OrthographicCamera( );

			Matrix4	ComputeScaleFromAR( ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height );

		public:
			//! Sets the camera based on two basis vectors and position
			OrthographicCamera( 
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Point3& vLookAt,				///< [in] A point in the world camera is looking at
				const Vector3& vUp,					///< [in] What is considered up in the world
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Vector2& vpScale,				///< [in] Viewport scale factor
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation	///< [in] Orientation relative to a target
				);

			//! Sets the camera based on a given basis and the position
			OrthographicCamera( 
				const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Vector2& vpScale,				///< [in] Viewport scale factor
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate				///< [in] Pixel rate of the camera
				);

			bool GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const;
		};
	}
}

#endif
