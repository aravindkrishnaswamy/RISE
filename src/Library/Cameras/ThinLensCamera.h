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

			// Source-of-truth photographic parameters.  All three
			// lengths (sensor, focal, focus) MUST be in the same unit
			// as scene geometry — the FOV formula sensor/focal is
			// unit-free, but the lens equation v = f*u/(u-f) requires
			// matching units.  Photographic numbers (sensor 36, focal
			// 35) work directly when scene geometry is in mm; for
			// metre-scale scenes scale all three lengths by 1/1000.
			Scalar			sensorSize;			// Sensor width (scene units)
			Scalar			focalLength;		// Lens focal length (scene units; same unit as sensorSize and focusDistance)
			Scalar			fstop;				// f-number (working aperture; dimensionless)
			Scalar			focusDistance;		// Focus plane (scene units; must be > focalLength)

			// Aperture-shape (Phase 1.0 enrichments).
			unsigned int	apertureBlades;		// 0 = perfect disk; 5..9 typical
			Scalar			apertureRotation;	// Polygon rotation in radians
			Scalar			anamorphicSqueeze;	// Aperture x-axis scale (1.0 = circular)

			// Derived caches — rebuilt by Recompute() from the params
			// above.  fov is in radians.  aperture is the diameter
			// (focalLength / fstop); halfAperture is the radius used by
			// the disk / polygon sampler.
			Scalar			fov;				/// Derived horizontal field of view
			Scalar			aperture;			// Derived aperture diameter
			Scalar			halfAperture;		// Derived aperture radius

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
				const Scalar sensorSize_,			///< [in] Sensor width (scene units; same unit as focalLength_/focusDistance_)
				const Scalar focalLength_,			///< [in] Lens focal length (scene units; same unit as sensorSize_/focusDistance_)
				const Scalar fstop_,				///< [in] f-number (dimensionless; aperture diameter = focalLength_/fstop_)
				const Scalar focusDistance_,		///< [in] Focus plane (scene units; must be > focalLength_)
				const unsigned int width,
				const unsigned int height,
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation,	///< [in] Orientation relative to a target
				const unsigned int apertureBlades_,	///< [in] Polygonal aperture blades; 0 = disk
				const Scalar apertureRotation_,		///< [in] Polygon rotation (radians)
				const Scalar anamorphicSqueeze_		///< [in] Aperture x-axis scale (1.0 = circular)
				);

			// Getters/setters for the descriptor-driven properties
			// panel.  All photographic params are stored as the user
			// provided them; derived caches (fov, aperture, halfAperture,
			// dx/dy, f_over_d_minus_f_*) are rebuilt by Recompute().
			//
			// CONTRACT: setters write the source-of-truth field ONLY.
			// The caller MUST invoke RegenerateData() (which dispatches
			// to Recompute()) before the next render, otherwise the
			// derived caches will be stale and the rendered result will
			// reflect the old camera while the field reads the new
			// value.  This batch-then-regenerate pattern matches the
			// CameraCommon mutators (SetLocation/SetLookAt/etc.) and
			// lets a logical update touching multiple fields incur
			// exactly one Recompute().  CameraIntrospection::SetProperty
			// (the live-editor entry point) and the keyframe pipeline
			// both call RegenerateData() at the end of each batch, so
			// editor edits and animations are safe out of the box; this
			// contract applies to any future direct caller of these
			// setters.
			inline Scalar GetSensorSize()              const { return sensorSize; }
			inline Scalar GetFocalLengthStored()       const { return focalLength; }
			inline Scalar GetFstop()                   const { return fstop; }
			inline Scalar GetFocusDistanceStored()     const { return focusDistance; }
			inline unsigned int GetApertureBlades()    const { return apertureBlades; }
			inline Scalar GetApertureRotation()        const { return apertureRotation; }
			inline Scalar GetAnamorphicSqueeze()       const { return anamorphicSqueeze; }
			inline void SetSensorSize( Scalar v )              { sensorSize = v; }
			inline void SetFocalLengthStored( Scalar v )       { focalLength = v; }
			inline void SetFstop( Scalar v )                   { fstop = v; }
			inline void SetFocusDistanceStored( Scalar v )     { focusDistance = v; }
			inline void SetApertureBlades( unsigned int v )    { apertureBlades = v; }
			inline void SetApertureRotation( Scalar v )        { apertureRotation = v; }
			inline void SetAnamorphicSqueeze( Scalar v )       { anamorphicSqueeze = v; }

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
