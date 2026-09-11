//////////////////////////////////////////////////////////////////////
//
//  FisheyeCamera.cpp - Implementation of the fisheye camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FisheyeCamera.h"
#include "CameraTransforms.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

void FisheyeCamera::Recompute( const unsigned int width, const unsigned int height )
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
	const Matrix4 m2 = ComputeScaleFromAR( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;
}

FisheyeCamera::FisheyeCamera( 
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Point3& vLookAt_,				///< [in] A point in the world camera is looking at
	const Vector3& vUp_,				///< [in] What is considered up in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Vector3& orientation_,		///< [in] Orientation (Pitch,Roll,Yaw)
	const Vector2& target_orientation_,	///< [in] Orientation relative to a target
	const Scalar scale_					///< [in] Scale factor to exagerrate the effects
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
    scale( scale_ )

{
	Recompute( width, height );

	OVwidth = 1.0 / frame.GetWidth();
	OVheight = 1.0 / frame.GetHeight();
}

FisheyeCamera::FisheyeCamera( 
	const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Scalar scale_					///< [in] Scale factor to exagerrate the effects
	) : 
  CameraCommon( 
	  vPosition_,
	  pixelAR_, 
	  exposure_, 
	  scanningRate,
	  pixelRate ),
    scale( scale_ )
{
	frame = Frame( basis, vPosition, width, height );

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), -1.0 ) );
	const Matrix4 m2 = ComputeScaleFromAR( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;

	OVwidth = 1.0 / frame.GetWidth();
	OVheight = 1.0 / frame.GetHeight();
}

FisheyeCamera::~FisheyeCamera( )
{
}

bool FisheyeCamera::ComputeWorldDirection(
	const Scalar screenX, const Scalar screenY, Vector3& dir ) const
{
	const Scalar x = (scale/2) - scale*screenX*OVwidth;
	const Scalar y = scale*screenY*OVheight - (scale/2);

	const Scalar radius = sqrt( x*x+y*y );

	if( radius > 1.0 ) {
		// No ray
		return false;
	}

	const Scalar theta = atan2( y, x );
	Vector3 v( radius * cos(theta), radius * sin(theta), sqrt( 1.0 - radius*radius ) );

	dir = Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans,v));

	return true;
}

bool FisheyeCamera::GenerateRay( const RuntimeContext& rc, Ray& ray, const Point2& ptOnScreen ) const
{
	Vector3 d;
	if( !ComputeWorldDirection( ptOnScreen.x, ptOnScreen.y, d ) ) {
		// No ray
		return false;
	}

	ray.Set(
		frame.GetOrigin(),
		d
		);

	// Ray differentials.  Every primary ray shares the frame origin,
	// so the ORIGIN offsets are exactly zero and the whole footprint
	// lives in the direction offsets — the mirror image of
	// OrthographicCamera, and the same shape as PinholeCamera /
	// ThinLensCamera.
	//
	// The mapping is NONLINEAR, and that is fine: the convention here
	// (Igehy 1999, and what PinholeCamera does on its linear map) is a
	// ONE-FULL-PIXEL FINITE DIFFERENCE OF THE EXACT MAPPING, not a
	// linearisation of it.  Re-entering `ComputeWorldDirection` at
	// pixel + 1 therefore gives the correct answer without needing a
	// pixel-to-direction Jacobian to exist in closed form — and it
	// automatically inherits the pixelAR stretch and the frame
	// rotation, which a hand-rolled angular pitch would have to
	// re-derive and could silently drift from.
	//
	// What the resulting footprint MEASURES, since the pixel solid
	// angle grows toward the rim: exactly the chord between the
	// central ray and the ray this camera would really generate for
	// the neighbouring pixel.  The projection is `r = sin(theta)`
	// (image radius = the SINE of the angle off the optical axis —
	// an ORTHOGRAPHIC fisheye, not the equidistant `r = theta` older
	// comments claimed), so on axis the chord is
	// `2*sin(asin(scale/width)/2)` and NOT `scale/width`: the two
	// differ by a relative 1.2e-6 at the shipped 500-px / scale 1.6
	// settings, which is why TextureFootprintTest 15a pins the exact
	// form.  Off axis the step is longer, correctly reporting the
	// coarser angular sampling out there.
	//
	// THE RIM.  A pixel whose OWN radius is inside the unit disc can
	// have its +x or +y neighbour outside it — there is no direction
	// at the neighbour, so there is no honest differential.  Rather
	// than fabricate one (clamping to the rim would under-report the
	// footprint by an unbounded factor, and extrapolating would put
	// the auxiliary on a direction the camera never generates), leave
	// `hasDifferentials` FALSE for that ray.  Downstream that is the
	// documented neutral fallback: `ComputeFootprintVectors` early-
	// outs, `widthValid` stays false, `fw` reads 0, and the texture
	// point-samples exactly as it did before this change.  Both
	// auxiliaries are required — a half-populated `diffs` would let
	// `ComputeFootprintVectors` average a live dpdx against a stale
	// dpdy.  The main ray is unaffected either way; only the
	// footprint is withheld.
	//
	// Must follow `ray.Set`, which clears hasDifferentials.
	Vector3 dAuxX, dAuxY;
	if( ComputeWorldDirection( ptOnScreen.x + Scalar( 1 ), ptOnScreen.y, dAuxX ) &&
	    ComputeWorldDirection( ptOnScreen.x, ptOnScreen.y + Scalar( 1 ), dAuxY ) ) {
		ray.diffs.rxOrigin = Vector3( 0, 0, 0 );	// shared frame origin
		ray.diffs.ryOrigin = Vector3( 0, 0, 0 );
		ray.diffs.rxDir = dAuxX - d;
		ray.diffs.ryDir = dAuxY - d;
		ray.hasDifferentials = true;
	}

	return true;
}

Matrix4 FisheyeCamera::ComputeScaleFromAR( ) const
{
	return Matrix4Ops::Stretch( Vector3( pixelAR, 1.0, 1.0 ) );
}

static const unsigned int SCALE_ID = 100;

IKeyframeParameter* FisheyeCamera::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = CameraCommon::KeyframeFromParameters( name, value );

	if( !p ) {
		// Check the name and see if its something we recognize
		if( name == "scale" ) {
			p = new Parameter<Scalar>( value.toDouble(), SCALE_ID );
		} else {
			return 0;
		}

		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	}

	return p;
}

void FisheyeCamera::SetIntermediateValue( const IKeyframeParameter& val )
{
	CameraCommon::SetIntermediateValue( val );

	switch( val.getID() )
	{
	case SCALE_ID:
		{
			// `scale` is an image-plane extent in sine units (see
			// ComputeWorldDirection), NOT an angle -- the parser path
			// (Job::AddFisheyeCamera -> RISE_API_CreateFisheyeCamera
			// -> the constructor) and the editor setter
			// (SetScaleStored) both take it raw.  This used to
			// multiply by DEG_TO_RAD, making an animated/keyframed
			// scale ~57.3x smaller than the same number in a scene
			// file for no unit reason -- fixed 2026-09-10.
			scale = *(Scalar*)val.getValue();
		}
		break;
	}
}


