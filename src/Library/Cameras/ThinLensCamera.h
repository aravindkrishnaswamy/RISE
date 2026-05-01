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

			// Source-of-truth photographic parameters.
			//
			// `sensorSize`, `focalLength`, `shiftX`, `shiftY` are in
			// MILLIMETRES (the photographic convention).  This keeps
			// the editor surface unit-stable: a 35mm lens is always
			// `focal_length = 35` regardless of the scene's geometry
			// unit.  At ray-generation time `Recompute()` converts mm
			// to scene-units via `sceneUnitMeters` so the lens
			// equation v = f·u/(u-f) is unit-consistent with
			// `focusDistance` (which IS in scene units, matching
			// geometry coordinates).
			//
			// `sceneUnitMeters` (= 1.0 for metres scenes by default)
			// is the per-scene scale set by the `scene_options`
			// chunk; the camera caches it so editor edits and
			// keyframed mutations keep the conversion consistent
			// without consulting parser-level state.
			Scalar			sensorSize;			// Sensor width (mm)
			Scalar			focalLength;		// Lens focal length (mm)
			Scalar			fstop;				// f-number (working aperture; dimensionless)
			Scalar			focusDistance;		// Focus plane (scene units; must be > focal_in_scene_units)
			Scalar			sceneUnitMeters;	// Meters per scene unit (default 1.0 = metres)

			// Aperture-shape (Phase 1.0 enrichments).
			unsigned int	apertureBlades;		// 0 = perfect disk; 5..9 typical
			Scalar			apertureRotation;	// Polygon rotation in radians
			Scalar			anamorphicSqueeze;	// Aperture x-axis scale (1.0 = circular)

			// Tilt-shift (Phase 1.1).  All four default to 0, in which
			// case the camera is plain perpendicular-focus-plane
			// thin-lens (bit-identical to Phase 1.0).  Tilt rotates
			// the FOCAL plane (not the lens or sensor) — Scheimpflug
			// formulation, see Recompute().  Shift translates the
			// IMAGE plane (architectural correction).
			Scalar			tiltX;				// Tilt around camera x-axis (radians); positive = top of focal plane tilts toward camera
			Scalar			tiltY;				// Tilt around camera y-axis (radians); positive = right side of focal plane tilts toward camera
			Scalar			shiftX;				// Lens shift along camera x (mm); positive = lens-right
			Scalar			shiftY;				// Lens shift along camera y (mm); positive = lens-up

			// Derived caches — rebuilt by Recompute() from the params
			// above.  fov is in radians.  aperture is the diameter
			// (focalLength / fstop); halfAperture is the radius used by
			// the disk / polygon sampler.
			Scalar			fov;				/// Derived horizontal field of view
			Scalar			aperture;			// Derived aperture diameter
			Scalar			halfAperture;		// Derived aperture radius

			Scalar		dx, dy;
			Scalar		filmDistance;	// Image-plane distance from lens (lens equation v = fu/(u-f))
			Scalar		sx, sy;			// Per-pixel image-plane scale (scene units / pixel)
			// Shift cache in SCENE UNITS — mm-to-scene-unit conversion
			// done once per Recompute, not per ray.  The mm-typed
			// `shiftX`/`shiftY` above stay as the user-facing source
			// of truth (editor reads them in mm directly).
			Scalar		shiftX_sceneUnits;
			Scalar		shiftY_sceneUnits;
			// Focus-plane equation cache: n · P = kFocus, where P is a
			// world-space point on the focal plane (in camera-local
			// coords).  For tilt = (0,0) this collapses to n = (0,0,1)
			// and kFocus = focusDistance, i.e. a plane perpendicular
			// to the optical axis at z = focusDistance.
			Scalar		nFocusX, nFocusY, nFocusZ;
			Scalar		kFocus;

			Matrix4	ComputeScaleFromAR( ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height );

		public:
			ThinLensCamera(
				const Point3& vPosition,
				const Point3& vLookAt,
				const Vector3& vUp,
				const Scalar sensorSize_,			///< [in] Sensor width (mm)
				const Scalar focalLength_,			///< [in] Lens focal length (mm)
				const Scalar fstop_,				///< [in] f-number (dimensionless; aperture diameter = focalLength_/fstop_)
				const Scalar focusDistance_,		///< [in] Focus plane (scene units; must be > focal_in_scene_units)
				const Scalar sceneUnitMeters_,		///< [in] Meters per scene unit (1.0 = metres scene; 0.001 = mm scene; etc.)
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
				const Scalar anamorphicSqueeze_,	///< [in] Aperture x-axis scale (1.0 = circular)
				const Scalar tiltX_,				///< [in] Focal-plane tilt around x-axis (radians)
				const Scalar tiltY_,				///< [in] Focal-plane tilt around y-axis (radians)
				const Scalar shiftX_,				///< [in] Lens shift along x (mm)
				const Scalar shiftY_				///< [in] Lens shift along y (mm)
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
			inline Scalar GetSensorSize()              const { return sensorSize; }      // mm
			inline Scalar GetFocalLengthStored()       const { return focalLength; }     // mm
			inline Scalar GetFstop()                   const { return fstop; }
			inline Scalar GetFocusDistanceStored()     const { return focusDistance; }   // scene units
			inline Scalar GetSceneUnitMeters()         const { return sceneUnitMeters; } // m / scene unit
			inline unsigned int GetApertureBlades()    const { return apertureBlades; }
			inline Scalar GetApertureRotation()        const { return apertureRotation; }
			inline Scalar GetAnamorphicSqueeze()       const { return anamorphicSqueeze; }
			inline Scalar GetTiltX()                   const { return tiltX; }
			inline Scalar GetTiltY()                   const { return tiltY; }
			inline Scalar GetShiftX()                  const { return shiftX; }           // mm
			inline Scalar GetShiftY()                  const { return shiftY; }           // mm
			inline void SetSensorSize( Scalar v )              { sensorSize = v; }
			inline void SetFocalLengthStored( Scalar v )       { focalLength = v; }
			inline void SetFstop( Scalar v )                   { fstop = v; }
			inline void SetFocusDistanceStored( Scalar v )     { focusDistance = v; }
			inline void SetSceneUnitMeters( Scalar v ) {
				// Guard against zero / negative — `Recompute()` divides
				// by sceneUnitMeters, so 0 produces inf/NaN and
				// negatives flip the sign of every mm-to-scene
				// conversion.  Parser already rejects invalid values,
				// so this is belt-and-braces for any out-of-tree
				// caller.
				if( v > 0 ) sceneUnitMeters = v;
			}
			inline void SetTiltX( Scalar v )                   { tiltX = v; }
			inline void SetTiltY( Scalar v )                   { tiltY = v; }
			inline void SetShiftX( Scalar v )                  { shiftX = v; }
			inline void SetShiftY( Scalar v )                  { shiftY = v; }
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
