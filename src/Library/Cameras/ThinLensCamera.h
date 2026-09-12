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
#include "../Interfaces/ILog.h"

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

			// Landing 5: photographic exposure metadata (independent of
			// the DOF / focus geometry above).  iso == 0 means "physical
			// exposure disabled, behave exactly like pre-L5".  When
			// iso > 0, evCompensation_ is precomputed in the constructor
			// from (iso, fstop, exposureTime) per the UE5 / Filament
			// formula and returned by GetExposureCompensationEV() for
			// stacking into FileRasterizerOutput on LDR outputs.  fstop
			// and exposureTime are reused from the existing geometric
			// camera state (DOF + motion blur respectively); this is
			// physically consistent because the same aperture and
			// shutter that govern the geometry also govern the EV.
			Scalar		iso_;				// ISO sensitivity (0 = disabled)
			Scalar		evCompensation_;	// Pre-computed exposure compensation in EV stops

			Scalar		dx, dy;
			Scalar		filmDistance;	// Image-plane distance from lens (lens equation v = fu/(u-f))
			Scalar		sx, sy;			// Per-pixel image-plane scale (scene units / pixel)
			// Shift cache in SCENE UNITS — mm-to-scene-unit conversion
			// done once per Recompute, not per ray.  The mm-typed
			// `shiftX`/`shiftY` above stay as the user-facing source
			// of truth (editor reads them in mm directly).
			Scalar		shiftX_sceneUnits;
			Scalar		shiftY_sceneUnits;
			// Inverse of `mxTrans`, cached by Recompute (the only place
			// `mxTrans` is assigned for this camera).  The light-tracing
			// inverse projection -- `RasterFromLensPoint`, on the hot
			// t==1 connection path of BDPT and VCM -- needs it once per
			// splat, and a 4x4 inverse per splat is not a per-frame cost
			// worth paying when the matrix only changes on Recompute.
			Matrix4		mxTransInv;
			// Cached 1/pixelAR for the aperture-sample X compensation.
			// `mxTrans` includes a Stretch(pixelAR, 1, 1) — see
			// ComputeScaleFromAR.  The image-plane math already
			// accounts for that (sy → fov via display_aspect, shiftX
			// pre-divided), but the per-ray aperture sample x in
			// SampleAperture/GenerateRay is in raw scene units and
			// would get stretched too, making the aperture disc 2×
			// wider in X for anamorphic 2:1 pixelAR.  Pre-dividing
			// ptOnLens.x by pixelAR keeps the lens axisymmetric in
			// world space.  Cached once in Recompute; default 1.
			Scalar		pixelARInv;
			// Focus-plane equation cache: n · P = kFocus, where P is a
			// world-space point on the focal plane (in camera-local
			// coords).  For tilt = (0,0) this collapses to n = (0,0,1)
			// and kFocus = focusDistance, i.e. a plane perpendicular
			// to the optical axis at z = focusDistance.
			Scalar		nFocusX, nFocusY, nFocusZ;
			Scalar		kFocus;

			Matrix4	ComputeScaleFromAR( ) const;

			//! The whole per-(film sample, lens point) direction
			//! construction: image-plane sample with shift, chief-ray
			//! intersection with the (possibly tilted) focal plane, and
			//! the normalised WORLD direction from `ptOnLens` to that
			//! focus point.  `screenX` / `screenY` are raster
			//! coordinates, `ptOnLens` is the camera-local aperture
			//! point (already pixelAR-compensated).
			//!
			//! Single source of truth for both `GenerateRay` /
			//! `GenerateRayWithLensSample` and for the +x / +y
			//! differential rays, so lens shift, anamorphic squeeze,
			//! focal-plane tilt and aperture-blade shaping are
			//! inherited by the differentials rather than re-derived.
			Vector3 ComputeWorldDirection( const Point3& ptOnLens, const Scalar screenX, const Scalar screenY ) const;

			//! Populate `r` (origin, direction AND ray differentials)
			//! for one film sample through one aperture point.  Shared
			//! tail of both public generators; see the implementation
			//! for what the thin-lens differential measures.
			void EmitRayThroughLens( Ray& r, const Point3& ptOnLens, const Point2& ptOnScreen ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height ) override;

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
				const Scalar shiftY_,				///< [in] Lens shift along y (mm)
				const Scalar iso = Scalar( 0 )		///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled (pre-L5 behaviour preserved).  When > 0, fstop_ and exposure must also be > 0 — both are reused from the existing geometric params.
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
			//! The parser refuses |tilt| >= 80 degrees (see the
			//! thinlens_camera descriptor); the editor property path and
			//! the Blender bridge reach these setters and the constructor
			//! directly, so the same bound is enforced here.  Past it the
			//! focal plane's vanishing line crosses the frame and
			//! ComputeWorldDirection's n_dot_p changes sign mid-image.
			static Scalar ClampTilt( const Scalar v )
			{
				const Scalar kMaxTiltRad = Scalar( 1.396 );	// 80 degrees, matches the parser
				return v > kMaxTiltRad ? kMaxTiltRad : ( v < -kMaxTiltRad ? -kMaxTiltRad : v );
			}
			inline void SetTiltX( Scalar v )                   { tiltX = ClampTilt( v ); }
			inline void SetTiltY( Scalar v )                   { tiltY = ClampTilt( v ); }
			inline void SetShiftX( Scalar v )                  { shiftX = v; }
			inline void SetShiftY( Scalar v )                  { shiftY = v; }
			inline void SetApertureBlades( unsigned int v )    { apertureBlades = v; }
			inline void SetApertureRotation( Scalar v )        { apertureRotation = v; }
			//! The parser rejects `anamorphic_squeeze <= 0` (see the
			//! thinlens_camera descriptor and GetApertureWorldArea()'s
			//! own comment): it collapses or mirrors the aperture,
			//! driving GetApertureWorldArea() to zero or negative, which
			//! makes BDPTCameraUtilities::HasFiniteAperture FALSE and
			//! splits the eye ray (still sampling the collapsed segment)
			//! from the t==1 connection (now imaging through the lens
			//! CENTRE) onto different camera vertices.  The parser only
			//! validates the AUTHORED value, though -- a KEYFRAMED
			//! squeeze interpolating between two positive endpoints can
			//! pass through 0 at a runtime-interpolated time the parser
			//! never sees, and the editor / Blender-bridge property path
			//! reaches this setter directly, bypassing the parser
			//! entirely.  Enforce the same bound here: reject and keep
			//! the last valid value rather than silently splitting the
			//! paths mid-animation.
			inline void SetAnamorphicSqueeze( Scalar v )
			{
				if( v > 0 ) {
					anamorphicSqueeze = v;
				} else {
					GlobalLog()->PrintEx( eLog_Error,
						"ThinLensCamera::SetAnamorphicSqueeze:: rejected non-positive value %f "
						"(a keyframed anamorphic_squeeze crossed <= 0); keeping %f -- see "
						"GetApertureWorldArea()'s contract for why 0 or negative is unsafe.",
						v, anamorphicSqueeze );
				}
			}

			bool GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const override;

			//===============================================================
			// Finite-aperture surface for bidirectional transport (debt 28).
			//
			// A thin lens is the only RISE camera whose importance is
			// emitted from a region of non-zero AREA, so the t==1
			// light-tracing strategy in BDPT / VCM cannot just connect to
			// `GetLocation()`: it has to SAMPLE a point on the aperture
			// with the same shape and density the primary rays use, divide
			// by that density, and work out which film sample images the
			// light vertex THROUGH that point.  These four methods are the
			// whole surface `BDPTCameraUtilities` needs for that; they are
			// deliberately concrete (not ICamera virtuals) for the same
			// reason `GenerateRayWithLensSample` is -- see its comment.
			//===============================================================

			//! WORLD-space area of the entrance aperture.  Polygonal
			//! blades and the anamorphic squeeze are included; the
			//! `pixelAR` pre-stretch is NOT, because it cancels
			//! (`SampleLensPoint` divides x by pixelAR and `mxTrans`
			//! multiplies it back, which is exactly what keeps the lens
			//! axisymmetric in world space).  Zero for a degenerate
			//! aperture (fstop -> infinity), which callers must
			//! treat as the pinhole limit rather than dividing by it.
			//!
			//! `anamorphic_squeeze <= 0` would be the OTHER way to reach
			//! zero here, and the scene parser rejects it rather than allow
			//! it (debt 28 review, A P2-3): a zero area makes
			//! `BDPTCameraUtilities::HasFiniteAperture` false, so the
			//! bidirectional integrators would connect t==1 to the lens
			//! CENTRE while `GenerateRay` went on sampling the collapsed
			//! line segment -- eye and light layers imaging through
			//! different camera vertices.  The parser only validates the
			//! AUTHORED value, though, so `SetAnamorphicSqueeze` (reached
			//! by keyframe interpolation, the editor, and the Blender
			//! bridge) and `RISE_API_CreateThinlensCamera` (reached by any
			//! direct API caller) enforce the same `> 0` bound themselves
			//! (debt 28 round 2, A2 P2-5) -- this contract holds for every
			//! construction path, not only scene-file authoring.
			//!
			//! `SampleLensPoint` is UNIFORM over this area -- the disk
			//! path is uniform by construction and the n-gon path's
			//! sec^2 inverse-CDF was derived to be (see SampleAperture in
			//! the .cpp) -- so the area density of a sampled point is
			//! exactly 1 / GetApertureWorldArea().
			Scalar GetApertureWorldArea() const;

			//! Sample a point on the entrance aperture with the SAME
			//! shape and density `GenerateRay` uses, from two canonical
			//! randoms.  Returns the CAMERA-LOCAL point (z == 0, x
			//! already pixelAR-compensated) -- the coordinates
			//! `RasterFromLensPoint` expects.  Push it through
			//! `LensPointToWorld` for the connection geometry.
			Point3 SampleLensPoint( const Point2& uv ) const;

			//! Camera-local aperture point -> world space.
			Point3 LensPointToWorld( const Point3& ptOnLens ) const;

			//! Inverse of the ray generation THROUGH A GIVEN APERTURE
			//! POINT: the raster coordinates of the film sample whose
			//! generated ray leaves `ptOnLens` in the direction of
			//! `worldPoint`.  FALSE when the point is behind the lens or
			//! lands outside the film.
			//!
			//! Mechanically the exact inverse of `ComputeWorldDirection`:
			//! intersect the (lens point -> world point) ray with the
			//! plane of focus, then project that focus-plane point back
			//! through the lens CENTRE onto the sensor.  Every constant it
			//! uses (dx/dy, sx/sy, the shift cache, the focal-plane
			//! equation, filmDistance) is the one ray generation uses, so
			//! lens shift and focal-plane tilt are inverted rather than
			//! ignored.
			//!
			//! Passing `ptOnLens = (0,0,0)` gives the lens-CENTRE
			//! projection -- what a pinhole of the same field of view
			//! would report, and what `BDPTCameraUtilities::Rasterize`
			//! returns for this camera.  Passing a SAMPLED point is what
			//! gives light-traced contributions the same depth of field
			//! the eye rays have: two lens points image an off-focus world
			//! point to raster positions a circle-of-confusion apart, and
			//! an ON-focus point to the same raster position for every
			//! lens point.
			bool RasterFromLensPoint(
				const Point3& worldPoint,		///< [in] World-space point to image
				const Point3& ptOnLens,			///< [in] Camera-local aperture point (z == 0)
				Point2& rasterOut				///< [out] Raster coordinates
				) const;

			//! The image-plane normalisation the camera's importance and
			//! directional pdf are both built from:
			//!     k = filmDistance^2 / (|sx * sy| * pixelAR)
			//! i.e. (image-plane distance)^2 divided by the WORLD-space
			//! area of one pixel on that plane.  Algebraically identical
			//! to `height^2 / (4 tan^2(fov/2) * pixelAR)`; expressed in
			//! the cached ray-generation constants so it cannot drift
			//! from them.  Callers form `k / cos^3(theta)` for the
			//! solid-angle pdf of one pixel and `k / (W*H*cos^3(theta))`
			//! for the importance.
			Scalar GetImagePlanePixelDensity() const;

			//! Landing 5: photographic exposure compensation in EV stops.
			//! Returns 0 when iso == 0 (physical exposure disabled);
			//! see ICamera.h for the formula and stacking semantics.
			Scalar GetExposureCompensationEV() const override { return evCompensation_; }
			inline Scalar GetIsoStored() const { return iso_; }
			inline void   SetIsoStored( Scalar v ) { iso_ = v; }

			//! Landing 5: also rebuild the photographic exposure cache
			//! after geometric state.  fstop and exposureTime feed
			//! BOTH the geometric (DOF / motion blur) and photographic
			//! (EV) sides of this camera; without this override, edits
			//! to those fields via SetFstop / SetExposureTimeStored or
			//! via the keyframed FSTOP_ID / EXPOSURE_ID handlers would
			//! update the geometry but leave evCompensation_ stale.
			void RegenerateData() override;

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
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
		};
	}
}


#endif
