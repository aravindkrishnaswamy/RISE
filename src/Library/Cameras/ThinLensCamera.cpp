//////////////////////////////////////////////////////////////////////
//
//  ThinLensCamera.cpp - Implementation of the thin lens camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 8, 2002
//  Tabs: 4
//  Comments:  Taken from ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ThinLensCamera.h"
#include "../Utilities/GeometricUtilities.h"
#include "CameraTransforms.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Sample a point inside the aperture, returning the 2D position on
	// the lens plane.  Encapsulates polygonal-blade and anamorphic
	// shaping so both GenerateRay paths share one implementation.
	//
	// For a regular n-gon, naive sampling (theta uniform in [0,2pi),
	// r = sqrt(u_y) * rMax(theta)) gives joint Cartesian density
	// 1 / (pi * rMax(theta)^2), which is angle-dependent — corners
	// (rMax = halfAperture) are under-sampled by cos^2(pi/n) relative
	// to edge midpoints.  At 6 blades that's a 25% bokeh-density bias.
	//
	// To get UNIFORM area density we sample theta with density
	// proportional to rMax(theta)^2.  By symmetry, blades are
	// equiprobable; within one blade rMax^2 = c^2 / cos^2(theta_rel)
	// (c = halfAperture * cos(pi/n)), so density ∝ sec^2(theta_rel).
	// The inverse CDF of sec^2 on [-pi/n, +pi/n] is closed-form:
	//     theta_rel = atan( (2*u - 1) * tan(pi/n) )
	// and the resulting joint density in (x,y) is exactly
	// 1 / A_polygon = 1 / ((n/2) * sin(2*pi/n) * halfAperture^2).
	//
	// Continuity in uv (required for PSSMLT): atan is C-infinity on
	// each blade, and across blade seams the mapping is C0 by
	// construction — at u_x' = 1- of blade k, theta lands on
	// (k+1) * bladeArc, exactly where u_x' = 0+ of blade k+1 lands.
	inline Point2 SampleAperture(
		Scalar halfAperture,
		unsigned int blades,
		Scalar rotation,
		Scalar squeeze,
		const Point2& uv )
	{
		Point2 sample;

		if( blades < 3 ) {
			// Disk path — preserves the historical primary-ray
			// distribution exactly when blades == 0 and squeeze == 1.
			sample = GeometricUtilities::PointOnDisk( halfAperture, uv );
		} else {
			// Uniform-area sampling on a regular n-gon inscribed in
			// the aperture circle.  See header comment for derivation.
			const Scalar n         = Scalar( blades );
			const Scalar bladeHalf = PI / n;                      // pi / n
			const Scalar bladeArc  = 2.0 * bladeHalf;             // 2*pi / n
			const Scalar tanHalf   = tan( bladeHalf );

			// Decompose uv.x into a blade index k and a within-blade
			// coordinate uxIn ∈ [0, 1).  Using floor is preferred over
			// (unsigned int)(uv.x * n) because it stays well-defined
			// for the (out-of-spec but observable in PSSMLT mutation
			// edge cases) value uv.x = 1.0 exactly.
			const Scalar scaled    = uv.x * n;
			const Scalar kFloor    = floor( scaled );
			const Scalar uxIn      = scaled - kFloor;             // ∈ [0, 1]

			// Inverse-CDF for theta_rel ∈ [-bladeHalf, +bladeHalf]
			// with density ∝ sec^2(theta_rel).
			const Scalar thetaRel  = atan( ( 2.0 * uxIn - 1.0 ) * tanHalf );

			// Global theta covers [0, 2*pi) as k sweeps 0..n-1.  At
			// uxIn=0 thetaRel=-bladeHalf so theta=k*bladeArc; at uxIn=1
			// thetaRel=+bladeHalf so theta=(k+1)*bladeArc — matches
			// the next blade's uxIn=0 entry, hence C0 across seams.
			const Scalar theta     = ( kFloor + 0.5 ) * bladeArc + thetaRel;

			const Scalar rMax      = halfAperture * cos( bladeHalf ) / cos( thetaRel );
			const Scalar r         = sqrt( uv.y ) * rMax;
			const Scalar a         = theta + rotation;
			sample = Point2( r * cos(a), r * sin(a) );
		}

		// Anamorphic squeeze applies as an x-axis scale on the lens
		// plane.  squeeze == 1.0 is the no-op fast-path.
		if( squeeze != 1.0 ) {
			sample.x *= squeeze;
		}
		return sample;
	}
}

void ThinLensCamera::Recompute( const unsigned int width, const unsigned int height )
{
	// Convert mm-input lens params to scene units so the lens
	// equation v = f·u/(u-f) is unit-consistent with focusDistance
	// (which is already in scene units).  Conversion factor:
	//   1 mm = 0.001 m,    1 scene unit = sceneUnitMeters m
	//   ⇒ 1 mm = 0.001 / sceneUnitMeters scene units.
	// For default (metres scene, sceneUnitMeters=1): 35 mm = 0.035
	// scene units.  For mm scene (sceneUnitMeters=0.001): 35 mm = 35
	// scene units.  Both the editor and the parser keep the user-
	// facing values in mm; this conversion lives entirely inside
	// the camera so the rest of the pipeline doesn't need to know.
	const Scalar mm_to_scene  = 0.001 / sceneUnitMeters;
	const Scalar sensor_scene = sensorSize  * mm_to_scene;
	const Scalar focal_scene  = focalLength * mm_to_scene;
	const Scalar shiftX_scene = shiftX      * mm_to_scene;
	const Scalar shiftY_scene = shiftY      * mm_to_scene;

	// Derive horizontal field of view from sensor + focal.  The ratio
	// is unit-free (sensor and focal cancel), so this matches the old
	// formula whether we passed in mm or scene units — included here
	// for clarity.
	fov = 2.0 * atan( sensor_scene / (2.0 * focal_scene) );

	// Derive physical aperture from f-stop, in scene units.
	//   f-number = focal / aperture_diameter
	// so aperture_diameter = focal / fstop, radius = focal / (2 * fstop).
	aperture     = focal_scene / fstop;
	halfAperture = aperture * 0.5;

	dx = -0.5 * Scalar(width);
	dy = -0.5 * Scalar(height);

	// Lens equation: image plane is at -filmDistance along the optical
	// axis (sensor behind the lens, lens at origin).  Per-pixel sx/sy
	// converts pixel coordinates into image-plane scene units.  Pixel
	// aspect is folded into the camera transform (ComputeScaleFromAR),
	// so sx/sy use the geometric mean of the two image dimensions
	// implicitly via fov-vertical.
	filmDistance = focusDistance * focal_scene / (focusDistance - focal_scene);
	sy = -2.0 * filmDistance * tan( fov / 2.0 ) / Scalar(height);
	sx = -sy;

	// Cache the scene-unit shift values for GenerateRay; the per-ray
	// path is hot, so we don't redo the unit conversion there.  These
	// member variables override the mm-named `shiftX`/`shiftY` for the
	// generation hot path; the mm values stay as the user-facing
	// source-of-truth (used by the editor and keyframe handling).
	shiftX_sceneUnits = shiftX_scene;
	shiftY_sceneUnits = shiftY_scene;

	// Focal-plane equation cache for tilt-shift (Phase 1.1).  The
	// focal plane passes through the on-axis focus point
	// (0, 0, focusDistance) and has normal n in camera-local coords.
	// Without tilt: n = (0, 0, 1), so n·P = z-component, kFocus =
	// focusDistance.  Tilt rotates n: tiltY around y rotates the
	// normal toward x, tiltX around x rotates it toward y.  The
	// canonical Scheimpflug rotation (R_y * R_x applied to (0,0,1)):
	//   n.x =  sin(tiltY) * cos(tiltX)
	//   n.y = -sin(tiltX)
	//   n.z =  cos(tiltY) * cos(tiltX)
	// kFocus = n · (0,0,focusDistance) = n.z * focusDistance, so the
	// plane equation in camera-local coords is n·P = kFocus.
	const Scalar cTx = cos( tiltX );
	const Scalar sTx = sin( tiltX );
	const Scalar cTy = cos( tiltY );
	const Scalar sTy = sin( tiltY );
	nFocusX =  sTy * cTx;
	nFocusY = -sTx;
	nFocusZ =  cTy * cTx;
	kFocus  =  nFocusZ * focusDistance;

	// Construct an OrthoNormalBasis that lines up with the viewing co-ordinates
	if( from_onb ) {
		frame.SetOrigin( vPosition );
	} else {
		Vector3 vNewUp = vUp;
		Point3 ptNewPosition = vPosition;

		CameraTransforms::AdjustCameraForThetaPhi( target_orientation, vPosition, vLookAt, vUp, ptNewPosition, vNewUp );

		Vector3 vForward = Vector3Ops::Normalize(Vector3Ops::mkVector3( vLookAt, ptNewPosition ));
		CameraTransforms::AdjustCameraForOrientation( vForward, vNewUp, vForward, vNewUp, orientation );

		OrthonormalBasis3D	onb;
		onb.CreateFromWV( vForward, vNewUp );

		frame = Frame( onb, ptNewPosition, width, height );
	}

	mxTrans = frame.GetTransformationMatrix( ) * ComputeScaleFromAR( );
}

ThinLensCamera::ThinLensCamera(
	const Point3& vPosition_,
	const Point3& vLookAt_,
	const Vector3& vUp_,
	const Scalar sensorSize_,
	const Scalar focalLength_,
	const Scalar fstop_,
	const Scalar focusDistance_,
	const Scalar sceneUnitMeters_,
	const unsigned int width,
	const unsigned int height,
	const Scalar pixelAR_,
	const Scalar exposure_,
	const Scalar scanningRate,
	const Scalar pixelRate,
	const Vector3& orientation_,
	const Vector2& target_orientation_,
	const unsigned int apertureBlades_,
	const Scalar apertureRotation_,
	const Scalar anamorphicSqueeze_,
	const Scalar tiltX_,
	const Scalar tiltY_,
	const Scalar shiftX_,
	const Scalar shiftY_
	) :
  CameraCommon(
	  vPosition_,
	  vLookAt_,
	  vUp_,
	  pixelAR_,
	  exposure_,
	  scanningRate,
	  pixelRate,
	  orientation_,
	  target_orientation_ ),
  sensorSize( sensorSize_ ),
  focalLength( focalLength_ ),
  fstop( fstop_ ),
  focusDistance( focusDistance_ ),
  sceneUnitMeters( sceneUnitMeters_ ),
  apertureBlades( apertureBlades_ ),
  apertureRotation( apertureRotation_ ),
  anamorphicSqueeze( anamorphicSqueeze_ ),
  tiltX( tiltX_ ),
  tiltY( tiltY_ ),
  shiftX( shiftX_ ),
  shiftY( shiftY_ ),
  fov( 0 ),
  aperture( 0 ),
  halfAperture( 0 ),
  filmDistance( 0 ),
  sx( 0 ),
  sy( 0 ),
  shiftX_sceneUnits( 0 ),
  shiftY_sceneUnits( 0 ),
  nFocusX( 0 ),
  nFocusY( 0 ),
  nFocusZ( 1 ),
  kFocus( 0 )
{
	Recompute( width, height );
}

ThinLensCamera::~ThinLensCamera( )
{
}

bool ThinLensCamera::GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const
{
	const Point2 uv( rc.random.CanonicalRandom(), rc.random.CanonicalRandom() );
	const Point2 xy = SampleAperture( halfAperture, apertureBlades, apertureRotation, anamorphicSqueeze, uv );

	const Point3		ptOnLens( xy.x, xy.y, 0.0 );

	// Image-plane sample (with optional shift, Phase 1.1).  Sensor
	// sits at z = -filmDistance in camera-local coords; sx/sy
	// convert pixel coords to scene-units on the sensor.  Shift
	// values are mm in the editor / scene-file but are cached in
	// scene-units by Recompute (Phase 1.2 unit-correction work).
	//
	// Shift sign convention (matches Blender Cycles, which is the
	// dominant photographer-friendly reference):
	//   shift_x > 0 → lens shifts RIGHT (along +U) → camera "looks
	//                 right", image content moves LEFT in frame.
	//   shift_y > 0 → lens shifts UP (along +V)    → camera "looks
	//                 up",    image content moves DOWN in frame.
	const Scalar x_pix = ptOnScreen.x + dx;
	const Scalar y_pix = ptOnScreen.y + dy;
	const Scalar x_img = x_pix * sx - shiftX_sceneUnits;
	const Scalar y_img = y_pix * sy - shiftY_sceneUnits;

	// Chief-ray intersection with the (possibly tilted) focal plane.
	// Ray P(t) = (1-t)·p_sensor passes through origin at t=1; on
	// the plane n·P = kFocus, so (1-t) = kFocus / (n·p_sensor).
	// For tilt=(0,0): n=(0,0,1), kFocus=focusDistance, p_sensor.z =
	// -filmDistance, so (1-t) = -focusDistance/filmDistance — the
	// standard perpendicular-focus-plane formula.
	const Scalar n_dot_p   = nFocusX * x_img + nFocusY * y_img + nFocusZ * (-filmDistance);
	const Scalar oneMinusT = kFocus / n_dot_p;
	const Point3 focus( oneMinusT * x_img, oneMinusT * y_img, oneMinusT * (-filmDistance) );

	r.Set(
		Point3Ops::Transform(mxTrans,ptOnLens),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans, Vector3Ops::mkVector3(focus, ptOnLens))) );
	return true;
}

bool ThinLensCamera::GenerateRayWithLensSample(
	const RuntimeContext& /*rc*/, Ray& r,
	const Point2& ptOnScreen, const Point2& lensSample ) const
{
	// Identical to GenerateRay except we take the aperture sample
	// directly from lensSample rather than pulling two canonical
	// randoms from rc.random.  This is the entry point MLT calls so
	// that PSSMLT mutations of lensSample translate into continuous
	// aperture moves — without it, small Markov mutations jumped
	// the aperture to completely unrelated points (because the lens
	// was a PRNG-seeded function, not a direct function, of the
	// primary sample) and thin-lens MLT could not converge for DOF.
	const Point2 xy = SampleAperture( halfAperture, apertureBlades, apertureRotation, anamorphicSqueeze, lensSample );

	const Point3		ptOnLens( xy.x, xy.y, 0.0 );

	const Scalar x_pix = ptOnScreen.x + dx;
	const Scalar y_pix = ptOnScreen.y + dy;
	const Scalar x_img = x_pix * sx - shiftX_sceneUnits;   // see GenerateRay for sign convention
	const Scalar y_img = y_pix * sy - shiftY_sceneUnits;

	const Scalar n_dot_p   = nFocusX * x_img + nFocusY * y_img + nFocusZ * (-filmDistance);
	const Scalar oneMinusT = kFocus / n_dot_p;
	const Point3 focus( oneMinusT * x_img, oneMinusT * y_img, oneMinusT * (-filmDistance) );

	r.Set(
		Point3Ops::Transform(mxTrans,ptOnLens),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans, Vector3Ops::mkVector3(focus, ptOnLens))) );
	return true;
}

Matrix4 ThinLensCamera::ComputeScaleFromAR( ) const
{
	return Matrix4Ops::Stretch( Vector3( pixelAR, 1.0, 1.0 ) );
}

static const unsigned int SENSOR_ID            = 100;
static const unsigned int FOCAL_ID             = 101;
static const unsigned int FSTOP_ID             = 102;
static const unsigned int FOCUS_ID             = 103;
static const unsigned int APERTURE_BLADES_ID   = 104;
static const unsigned int APERTURE_ROTATION_ID = 105;
static const unsigned int ANAMORPHIC_ID        = 106;
static const unsigned int TILT_X_ID            = 107;
static const unsigned int TILT_Y_ID            = 108;
static const unsigned int SHIFT_X_ID           = 109;
static const unsigned int SHIFT_Y_ID           = 110;

IKeyframeParameter* ThinLensCamera::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = CameraCommon::KeyframeFromParameters( name, value );

	if( !p ) {
		// Check the name and see if its something we recognize
		if( name == "sensor_size" ) {
			p = new Parameter<Scalar>( value.toDouble(), SENSOR_ID );
		} else if( name == "focal_length" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOCAL_ID );
		} else if( name == "fstop" ) {
			p = new Parameter<Scalar>( value.toDouble(), FSTOP_ID );
		} else if( name == "focus_distance" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOCUS_ID );
		} else if( name == "aperture_blades" ) {
			p = new Parameter<Scalar>( value.toDouble(), APERTURE_BLADES_ID );
		} else if( name == "aperture_rotation" ) {
			p = new Parameter<Scalar>( value.toDouble(), APERTURE_ROTATION_ID );
		} else if( name == "anamorphic_squeeze" ) {
			p = new Parameter<Scalar>( value.toDouble(), ANAMORPHIC_ID );
		} else if( name == "tilt_x" ) {
			p = new Parameter<Scalar>( value.toDouble(), TILT_X_ID );
		} else if( name == "tilt_y" ) {
			p = new Parameter<Scalar>( value.toDouble(), TILT_Y_ID );
		} else if( name == "shift_x" ) {
			p = new Parameter<Scalar>( value.toDouble(), SHIFT_X_ID );
		} else if( name == "shift_y" ) {
			p = new Parameter<Scalar>( value.toDouble(), SHIFT_Y_ID );
		} else {
			return 0;
		}

		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	}

	return p;
}

void ThinLensCamera::SetIntermediateValue( const IKeyframeParameter& val )
{
	CameraCommon::SetIntermediateValue( val );

	switch( val.getID() )
	{
	case SENSOR_ID:
		sensorSize = *(Scalar*)val.getValue();
		break;
	case FOCAL_ID:
		focalLength = *(Scalar*)val.getValue();
		break;
	case FSTOP_ID:
		fstop = *(Scalar*)val.getValue();
		break;
	case FOCUS_ID:
		focusDistance = *(Scalar*)val.getValue();
		break;
	case APERTURE_BLADES_ID:
		// Stored as Scalar in the keyframe pipeline; truncate to int
		// at apply time.  Continuous interpolation between integer
		// blade counts produces non-physical bokeh shapes, so callers
		// should keyframe blade count with STEP interpolation.
		apertureBlades = static_cast<unsigned int>( *(Scalar*)val.getValue() );
		break;
	case APERTURE_ROTATION_ID:
		// Keyframed values are typed in the same unit as the parser
		// scene-file value (degrees); storage is radians.  Mirroring
		// the FOV_ID conversion that PinholeCamera uses for its
		// degree-typed angular parameter.
		apertureRotation = *(Scalar*)val.getValue() * DEG_TO_RAD;
		break;
	case ANAMORPHIC_ID:
		anamorphicSqueeze = *(Scalar*)val.getValue();
		break;
	case TILT_X_ID:
		// Parser/editor surface tilt in degrees; storage is radians.
		tiltX = *(Scalar*)val.getValue() * DEG_TO_RAD;
		break;
	case TILT_Y_ID:
		tiltY = *(Scalar*)val.getValue() * DEG_TO_RAD;
		break;
	case SHIFT_X_ID:
		// Shift is in MM (Phase 1.2 unit convention).  The camera's
		// Recompute() converts mm → scene-units before consumption,
		// so the keyframe value passes through as raw mm — no
		// conversion here.
		shiftX = *(Scalar*)val.getValue();
		break;
	case SHIFT_Y_ID:
		shiftY = *(Scalar*)val.getValue();
		break;
	}
}
