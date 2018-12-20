//////////////////////////////////////////////////////////////////////
//
//  CameraCommon.cpp - Common stuff shared between cameras
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 28, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CameraCommon.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

CameraCommon::CameraCommon(
    const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Point3& vLookAt_,				///< [in] A point in the world camera is looking at
	const Vector3& vUp_,				///< [in] What is considered up in the world
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate_,			///< [in] Scanning rate of the camera
	const Scalar pixelRate_,			///< [in] Pixel rate of the camera
	const Vector3& orientation_,		///< [in] Orientation (Pitch,Roll,Yaw)
	const Vector2& target_orientation_	///< [in] Orientation relative to a target
	) : 
  vPosition( vPosition_ ),
  vLookAt( vLookAt_ ),
  vUp( vUp_ ),
  pixelAR( pixelAR_ ),
  exposureTime( exposure_ ),
  scanningRate( scanningRate_ ),
  pixelRate( pixelRate_ ),
  orientation( orientation_ ),
  target_orientation( target_orientation_ ),
  from_onb( false )
{
}

CameraCommon::CameraCommon(
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera	
	const Scalar scanningRate_,			///< [in] Scanning rate of the camera
	const Scalar pixelRate_				///< [in] Pixel rate of the camera
	) : 
  vPosition( vPosition_ ),
  pixelAR( pixelAR_ ),
  exposureTime( exposure_ ),
  scanningRate( scanningRate_ ),
  pixelRate( pixelRate_ ),
  from_onb( true )
{
}

static const unsigned int LOCATION_ID = 2000;
static const unsigned int LOOKAT_ID = 2001;
static const unsigned int UP_ID = 2002;
static const unsigned int ORIENTATION_ID = 2003;
static const unsigned int TARGET_ORIENTATION_ID = 2004;
static const unsigned int EXPOSURE_ID = 2005;
static const unsigned int WIDTH_ID = 2006;
static const unsigned int HEIGHT_ID = 2007;

IKeyframeParameter* CameraCommon::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "location" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, LOCATION_ID );
		}
	} else if( name == "lookat" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, LOOKAT_ID );
		}
	} else if( name == "up" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, UP_ID );
		}
	} else if( name == "orientation" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			v.x *= DEG_TO_RAD;
			v.y *= DEG_TO_RAD;
			v.z *= DEG_TO_RAD;
			p = new Vector3Keyframe( v, ORIENTATION_ID );
		}
	} else if( name == "target_orientation" ) {
		Vector2 v;
		if( sscanf( value.c_str(), "%lf %lf", &v.x, &v.y ) == 2 ) {
			v.x *= DEG_TO_RAD;
			v.y *= DEG_TO_RAD;
			p = new Parameter<Vector2>( v, TARGET_ORIENTATION_ID );
		}
	} else if( name == "exposure" ) {
		p = new Parameter<Scalar>( value.toDouble(), EXPOSURE_ID );
	} else if( name == "width" ) {
		p = new Parameter<Scalar>( value.toDouble(), WIDTH_ID );
	} else if( name == "height" ) {
		p = new Parameter<Scalar>( value.toDouble(), HEIGHT_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void CameraCommon::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case LOCATION_ID:
		{
			vPosition = *(Point3*)val.getValue();
		}
		break;
	case LOOKAT_ID:
		{
			vLookAt = *(Point3*)val.getValue();
		}
		break;
	case UP_ID:
		{
			vUp = *(Vector3*)val.getValue();
		}
		break;
	case ORIENTATION_ID:
		{
			orientation = *(Vector3*)val.getValue();
		}
		break;
	case TARGET_ORIENTATION_ID:
		{
			target_orientation = *(Vector2*)val.getValue();
		}
		break;
	case EXPOSURE_ID:
		{
			exposureTime = *(Scalar*)val.getValue();
		}
		break;
	case WIDTH_ID:
		{
			frame.SetDimensions( (unsigned int)*(Scalar*)val.getValue(), frame.GetHeight() );
		}
		break;
	case HEIGHT_ID:
		{
			frame.SetDimensions( frame.GetWidth(), (unsigned int)*(Scalar*)val.getValue() );
		}
		break;
	}
}

void CameraCommon::RegenerateData( )
{
	Recompute( frame.GetWidth(), frame.GetHeight() );
}
