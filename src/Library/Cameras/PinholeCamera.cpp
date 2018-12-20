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

using namespace RISE;
using namespace RISE::Implementation;

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
	const Vector2& target_orientation_	///< [in] Orientation relative to a target
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
  fov( fov_ )
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
	const Scalar pixelRate				///< [in] Pixel rate of the camera
	) : 
  CameraCommon(
	  vPosition_,
	  pixelAR_, 
	  exposure_,
	  scanningRate,
	  pixelRate
	  ),
  fov( fov_ )
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

bool PinholeCamera::GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const
{
	const Point3	p( ptOnScreen.x, ptOnScreen.y, 0.0 );
	const Point3 transP = Point3Ops::Transform( mxTrans, p );
	const Vector3 v = Vector3Ops::mkVector3( frame.GetOrigin(), transP );
	r.Set( frame.GetOrigin(), Vector3Ops::Normalize(v) );

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

