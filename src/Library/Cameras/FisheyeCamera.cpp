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

bool FisheyeCamera::GenerateRay( const RuntimeContext& rc, Ray& ray, const Point2& ptOnScreen ) const
{
	const Scalar x = (scale/2) - scale*ptOnScreen.x*OVwidth;
	const Scalar y = scale*ptOnScreen.y*OVheight - (scale/2);

	const Scalar radius = sqrt( x*x+y*y );

	if( radius > 1.0 ) {
		// No ray
		return false;
	}

	const Scalar theta = atan2( y, x );
	Vector3 v( radius * cos(theta), radius * sin(theta), sqrt( 1.0 - radius*radius ) );

	ray.Set( 
		frame.GetOrigin(),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans,v))
		);

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
			scale = *(Scalar*)val.getValue() * DEG_TO_RAD;
		}
		break;
	}
}


