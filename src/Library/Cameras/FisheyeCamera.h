//////////////////////////////////////////////////////////////////////
//
//  FisheyeCamera.h - Declaration of a fisheye camera which is a 
//    special type of camera with a 180 degree FOV
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FISHEYE_CAMERA_
#define FISHEYE_CAMERA_

#include "CameraCommon.h"

namespace RISE
{
	namespace Implementation
	{
		class FisheyeCamera : 
			public virtual CameraCommon
		{
		protected:
			virtual ~FisheyeCamera( );

			Scalar scale;

			// For optimization reasons
			Scalar				OVwidth;
			Scalar				OVheight;

			Matrix4	ComputeScaleFromAR( ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height );

		public:
			//! Sets the camera based on two basis vectors and position
			FisheyeCamera( 
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Point3& vLookAt,				///< [in] A point in the world camera is looking at
				const Vector3& vUp,					///< [in] What is considered up in the world
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation,	///< [in] Orientation relative to a target
				const Scalar scale_					///< [in] Scale factor to exagerrate the effects
				);

			//! Sets the camera based on a given basis and the position
			FisheyeCamera( 
				const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Scalar scale_					///< [in] Scale factor to exagerrate the effects
				);

			bool GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const;

			// For keyframing the scale
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}

#endif
