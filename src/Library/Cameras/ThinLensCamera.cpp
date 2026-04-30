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
	// Derive horizontal field of view from sensor + focal length.
	// Angle of View = 2 * arctan( sensor / (2 * focal) )
	// The ratio is what matters, so units cancel here — but the lens
	// equation below requires focalLength and focusDistance to be in
	// the same unit, which is why all three lengths share the
	// scene-geometry unit.  Photographic numbers (sensor 36, focal
	// 35) work directly when scene geometry is in mm.
	fov = 2.0 * atan( sensorSize / (2.0 * focalLength) );

	// Derive physical aperture from f-stop.  aperture is the diameter,
	// halfAperture is the radius used by the lens-plane sampler.
	// Inherits the focal_length unit, which is the scene-geometry unit.
	//   f-number = focal / aperture_diameter
	// so aperture_diameter = focal / fstop, radius = focal / (2 * fstop).
	aperture     = focalLength / fstop;
	halfAperture = aperture * 0.5;

	dx = -0.5 * Scalar(width);
	dy = -0.5 * Scalar(height);

	const Scalar filmDistance = focusDistance * focalLength / (focusDistance - focalLength);
	const Scalar f_over_d_minus_f = focalLength / (filmDistance - focalLength);

	const Scalar sy = - 2.0 * filmDistance * tan(fov / 2.0) / Scalar(height);
	const Scalar sx = -sy;
	f_over_d_minus_f_sx = f_over_d_minus_f * sx;
	f_over_d_minus_f_sy = f_over_d_minus_f * sy;
	f_over_d_minus_f_d = f_over_d_minus_f * filmDistance;

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
	const Scalar anamorphicSqueeze_
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
  apertureBlades( apertureBlades_ ),
  apertureRotation( apertureRotation_ ),
  anamorphicSqueeze( anamorphicSqueeze_ ),
  fov( 0 ),
  aperture( 0 ),
  halfAperture( 0 )
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

	const Scalar x = ptOnScreen.x + dx;
	const Scalar y = ptOnScreen.y + dy;

	const Point3		focus(  -x *  f_over_d_minus_f_sx,
								-y *  f_over_d_minus_f_sy,
								f_over_d_minus_f_d );

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

	const Scalar x = ptOnScreen.x + dx;
	const Scalar y = ptOnScreen.y + dy;

	const Point3		focus(  -x *  f_over_d_minus_f_sx,
								-y *  f_over_d_minus_f_sy,
								f_over_d_minus_f_d );

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
	}
}
