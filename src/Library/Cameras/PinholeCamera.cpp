//////////////////////////////////////////////////////////////////////
//
//  PinholeCamera.cpp - Implementation of the pinhole camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PinholeCamera.h"
#include "CameraTransforms.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Landing 5: photographic exposure compensation.
	//
	// EV100 = log2(N² × 100 / (ISO × T))
	// evCompensation = -log2(1.2) - EV100      ≈ -0.263 - EV100
	//
	// Formula matches UE5 / Filament / ISO 12232 saturation-based
	// convention: a 100% reflector saturates at scene luminance
	// L = 1.2 × 2^EV100 cd/m².  The returned compensation stacks
	// ADDITIVELY with the LDR output's own exposure_compensation;
	// HDR archival outputs ignore both per Landing 1's "EXR =
	// linear radiance ground truth" invariant.
	//
	// Returns 0 when iso == 0 (physical exposure disabled).  When
	// iso > 0 but fstop or shutter are non-positive, logs an error
	// and returns 0 — the parser is expected to validate these
	// before reaching the camera constructor, but we keep the
	// camera-level guard so direct API users get a clean failure
	// mode instead of a NaN.
	inline RISE::Scalar ComputeExposureCompensationEV(
		const RISE::Scalar iso,
		const RISE::Scalar fstop,
		const RISE::Scalar shutterSeconds,
		const char*        cameraTag )
	{
		if( iso <= RISE::Scalar( 0 ) ) {
			return RISE::Scalar( 0 );
		}
		if( fstop <= RISE::Scalar( 0 ) || shutterSeconds <= RISE::Scalar( 0 ) ) {
			RISE::GlobalLog()->PrintEx( RISE::eLog_Error,
				"%s:: iso > 0 requires fstop > 0 (given %g) and shutter (`exposure`) > 0 (given %g) "
				"for physical exposure computation.  Falling back to no exposure compensation.",
				cameraTag, fstop, shutterSeconds );
			return RISE::Scalar( 0 );
		}
		const RISE::Scalar EV100 = std::log2( fstop * fstop * RISE::Scalar( 100 ) / ( iso * shutterSeconds ) );
		return -std::log2( RISE::Scalar( 1.2 ) ) - EV100;
	}
}

void PinholeCamera::Recompute( const unsigned int width, const unsigned int height )
{
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

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), -1.0 ) );
	const Matrix4 m2 = ComputeScaleFromFOV( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;
}

PinholeCamera::PinholeCamera(
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Point3& vLookAt_,				///< [in] A point in the world camera is looking at
	const Vector3& vUp_,				///< [in] What is considered up in the world
	const Scalar fov_,					///< [in] Entire field of view
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Vector3& orientation_,		///< [in] Orientation (Pitch,Roll,Yaw)
	const Vector2& target_orientation_,	///< [in] Orientation relative to a target
	const Scalar iso,					///< [in] Landing 5: ISO sensitivity (0 = disabled)
	const Scalar fstop					///< [in] Landing 5: f-number (used only for EV; pinhole has no DOF)
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
  fov( fov_ ),
  iso_( iso ),
  fstop_( fstop ),
  evCompensation_( ComputeExposureCompensationEV( iso, fstop, exposure_, "PinholeCamera" ) )
{
	Recompute( width, height );
}

PinholeCamera::PinholeCamera(
	const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Scalar fov_,					///< [in] Entire field of view
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Scalar iso,					///< [in] Landing 5: ISO sensitivity (0 = disabled)
	const Scalar fstop					///< [in] Landing 5: f-number (used only for EV; pinhole has no DOF)
	) :
  CameraCommon(
	  vPosition_,
	  pixelAR_,
	  exposure_,
	  scanningRate,
	  pixelRate
	  ),
  fov( fov_ ),
  iso_( iso ),
  fstop_( fstop ),
  evCompensation_( ComputeExposureCompensationEV( iso, fstop, exposure_, "PinholeCamera" ) )
{
	frame = Frame( basis, vPosition, width, height );

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), -1.0 ) );
	const Matrix4 m2 = ComputeScaleFromFOV( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;
}

PinholeCamera::~PinholeCamera( )
{
}

void PinholeCamera::RegenerateData()
{
	// Geometric state first (the existing CameraCommon contract:
	// Recompute() rebuilds the basis matrix from location / lookAt /
	// up / orientation).
	CameraCommon::RegenerateData();

	// Landing 5: refresh the photographic exposure cache.  exposureTime
	// (= shutter T) lives on CameraCommon and is mutable via the
	// inherited setters and via keyframed EXPOSURE_ID; iso_ and fstop_
	// live on this class and are mutable via SetIsoStored / SetFstop.
	// Any of those edits invalidates evCompensation_ — recompute from
	// current state to keep brightness in sync with motion blur and
	// DOF.  ComputeExposureCompensationEV is a no-op fallback when
	// iso_ == 0 (physical exposure disabled).
	evCompensation_ = ComputeExposureCompensationEV( iso_, fstop_, exposureTime, "PinholeCamera" );
}

bool PinholeCamera::GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const
{
	// Helper: compute the world-space (un-normalised at the boundary,
	// then normalised below) direction from a screen-space point.
	auto computeDir = [&]( const Scalar sx, const Scalar sy ) -> Vector3 {
		const Point3 p( sx, sy, 0.0 );
		const Point3 transP = Point3Ops::Transform( mxTrans, p );
		const Vector3 v = Vector3Ops::mkVector3( frame.GetOrigin(), transP );
		return Vector3Ops::Normalize( v );
	};

	const Vector3 d  = computeDir( ptOnScreen.x,             ptOnScreen.y             );
	r.Set( frame.GetOrigin(), d );

	// Landing 2: populate ray differentials by computing the
	// neighbour-pixel directions and storing the offset.  Per Igehy,
	// the differentials represent a FULL pixel shift, not a 1/spp
	// shift — the per-sample jitter contributes to spatial integration
	// rather than to footprint shrinkage.
	const Vector3 dx = computeDir( ptOnScreen.x + Scalar( 1 ), ptOnScreen.y             );
	const Vector3 dy = computeDir( ptOnScreen.x,               ptOnScreen.y + Scalar( 1 ) );
	r.diffs.rxOrigin = Vector3( 0, 0, 0 );	// pinhole: shared origin
	r.diffs.ryOrigin = Vector3( 0, 0, 0 );
	r.diffs.rxDir = dx - d;
	r.diffs.ryDir = dy - d;
	r.hasDifferentials = true;

	return true;
}

Matrix4 PinholeCamera::ComputeScaleFromFOV( ) const
{
	const Scalar h = 2 * tan( fov * 0.5 );
	const Scalar ar = Scalar(frame.GetWidth()) / Scalar(frame.GetHeight()) * pixelAR;
	return Matrix4Ops::Stretch( Vector3( h/Scalar(frame.GetWidth())*ar, -h/Scalar(frame.GetHeight()), 1.0 ) );;
}

static const unsigned int FOV_ID = 100;

IKeyframeParameter* PinholeCamera::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = CameraCommon::KeyframeFromParameters( name, value );

	if( !p ) {
		// Check the name and see if its something we recognize
		if( name == "fov" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOV_ID );
		} else {
			return 0;
		}

		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	}

	return p;
}

void PinholeCamera::SetIntermediateValue( const IKeyframeParameter& val )
{
	CameraCommon::SetIntermediateValue( val );

	switch( val.getID() )
	{
	case FOV_ID:
		{
			fov = *(Scalar*)val.getValue() * DEG_TO_RAD;
		}
		break;
	}
}

