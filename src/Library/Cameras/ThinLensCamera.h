//////////////////////////////////////////////////////////////////////
//
//  ThinLensCamera.h - Declaration of a thin lens camera, ie. with a 
//  lens that focusses the incoming light.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 8, 2002
//  Tabs: 4
//  Comments: This was taken directly from ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINLENS_CAMERA_
#define THINLENS_CAMERA_

#include "CameraCommon.h"

namespace RISE
{
	namespace Implementation
	{
		class ThinLensCamera : 
			public virtual CameraCommon 
		{
		protected:
			virtual ~ThinLensCamera( );
			Scalar			fov;				/// The field of view
			Scalar			aperture;			// Aperture size
			Scalar			halfAperture;		// Half aperture		halfA
			Scalar			focalLength;		// focal length
			Scalar			focusDistance;		// focus distance

			Scalar			filmWidth;			// width of the film
			Scalar			filmHeight;			// height of the film

			Scalar		dx, dy;
			Scalar		f_over_d_minus_f_sx;
			Scalar		f_over_d_minus_f_sy;
			Scalar		f_over_d_minus_f_d;

			Matrix4	ComputeScaleFromAR( ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height );

		public:
			ThinLensCamera( 
				const Point3& vPosition,
				const Point3& vLookAt,
				const Vector3& vUp,
				const Scalar fov_,
				const unsigned int width,
				const unsigned int height,
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation,	///< [in] Orientation relative to a target
				const Scalar dAperture_,
				const Scalar focalLength_,
				const Scalar focusDistance_
				);

			bool GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const;

			// Non-virtual, class-specific.  Not part of ICamera and
			// deliberately NOT on the vtable — adding a virtual would
			// break ABI for out-of-tree camera objects compiled
			// against the old interface.  MLT finds this method via
			// dynamic_cast at the call site (see MLTRasterizer's
			// GenerateCameraRayWithLensSample helper).
			//
			// Uses lensSample.x/y DIRECTLY for the aperture disk
			// sample, so a PSSMLT mutation on lensSample produces a
			// continuous aperture move — preserving the small-step
			// locality that makes Metropolis sampling efficient for
			// depth-of-field paths.  GenerateRay still exists for
			// non-Metropolis integrators and reads from rc.random.
			bool GenerateRayWithLensSample( const RuntimeContext& rc, Ray& r,
				const Point2& ptOnScreen, const Point2& lensSample ) const;

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}


#endif
