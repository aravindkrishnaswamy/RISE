//////////////////////////////////////////////////////////////////////
//
//  OrthographicCamera.cpp - Implementation of the pinhole camera
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
#include "OrthographicCamera.h"
#include "CameraTransforms.h"

using namespace RISE;
using namespace RISE::Implementation;

void OrthographicCamera::Recompute( const unsigned int width, const unsigned int height )
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

OrthographicCamera::OrthographicCamera( 
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Point3& vLookAt_,				///< [in] A point in the world camera is looking at
	const Vector3& vUp_,				///< [in] What is considered up in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Vector2& vpScale,				///< [in] Viewport scale factor
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
  viewportScale( vpScale )
{
	Recompute( width, height );
}

OrthographicCamera::OrthographicCamera( 
	const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Vector2& vpScale,				///< [in] Viewport scale factor
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
  viewportScale( vpScale )
{
	frame = Frame( basis, vPosition, width, height );

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), 0.0 ) );
	const Matrix4 m2 = ComputeScaleFromAR( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;
}

OrthographicCamera::~OrthographicCamera( )
{
}

bool OrthographicCamera::GenerateRay( const RuntimeContext& rc, Ray& ray, const Point2& ptOnScreen ) const
{
	const Scalar x = (frame.GetWidth()/2-ptOnScreen.x)/Scalar(frame.GetWidth()) * viewportScale.x;
	const Scalar y = (ptOnScreen.y - frame.GetHeight()/2)/Scalar(frame.GetHeight()) * viewportScale.y;
	Vector3 v( 0, 0, 1 );

	ray.Set( 
		Point3Ops::mkPoint3( frame.GetOrigin(), Vector3( -x, y, 0.0 ) ),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans,v))
		);

	return true;
}

Matrix4 OrthographicCamera::ComputeScaleFromAR( ) const
{
	const Scalar ar = Scalar(frame.GetWidth()) / Scalar(frame.GetHeight()) * pixelAR;
	return Matrix4Ops::Stretch( Vector3( ar, -1.0, 1.0 ) );
}
