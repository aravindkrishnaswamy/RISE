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
#include <limits>
#include <string_view>
#include "../Utilities/FiniteMath.h"
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
static const unsigned int THETA_ID = 2008;   // target_orientation.x (elevation, about screen-right)
static const unsigned int PHI_ID   = 2009;   // target_orientation.y (azimuth, about world-up)

IKeyframeParameter* CameraCommon::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = nullptr;
	const std::string_view field( name.c_str() );
	if( field == "location" || field == "lookat" || field == "up" || field == "orientation" ) {
		double components[3];
		if( !ParseStrictVec3( value, components ) ) return nullptr;
		if( field == "location" || field == "lookat" ) {
			p = new Point3Keyframe( Point3(components[0], components[1], components[2]),
				field == "location" ? LOCATION_ID : LOOKAT_ID );
		} else {
			Vector3 vector( components[0], components[1], components[2] );
			if( field == "orientation" ) vector = vector * DEG_TO_RAD;
			p = new Vector3Keyframe( vector, field == "up" ? UP_ID : ORIENTATION_ID );
		}
	} else if( field == "target_orientation" ) {
		double components[2];
		if( !ParseStrictVec2( value, components ) ) return nullptr;
		p = new Parameter<Vector2>( Vector2(components[0] * DEG_TO_RAD, components[1] * DEG_TO_RAD),
			TARGET_ORIENTATION_ID );
	} else {
		Scalar parsed;
		if( !ParseStrictScalar( value, parsed ) ) return nullptr;
		if( field == "theta" || field == "elevation" ) {
			p = new Parameter<Scalar>( parsed * DEG_TO_RAD, THETA_ID );
		} else if( field == "phi" || field == "azimuth" ) {
			p = new Parameter<Scalar>( parsed * DEG_TO_RAD, PHI_ID );
		} else if( field == "exposure" ) {
			p = new Parameter<Scalar>( parsed, EXPOSURE_ID );
		} else if( field == "width" || field == "height" ) {
			if( parsed < 1 || parsed > std::numeric_limits<unsigned int>::max() ) return nullptr;
			p = new Parameter<Scalar>( parsed, field == "width" ? WIDTH_ID : HEIGHT_ID );
		} else {
			return nullptr;
		}
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
	case THETA_ID:
		{
			target_orientation.x = *(Scalar*)val.getValue();
		}
		break;
	case PHI_ID:
		{
			target_orientation.y = *(Scalar*)val.getValue();
		}
		break;
	case EXPOSURE_ID:
		{
			exposureTime = *(Scalar*)val.getValue();
		}
		break;
	case WIDTH_ID:
		{
			const Scalar dimension = *static_cast<const Scalar*>( val.getValue() );
			if( IsFiniteDouble(dimension) && dimension >= 1 && dimension <= std::numeric_limits<unsigned int>::max() )
				frame.SetDimensions( static_cast<unsigned int>(dimension), frame.GetHeight() );
		}
		break;
	case HEIGHT_ID:
		{
			const Scalar dimension = *static_cast<const Scalar*>( val.getValue() );
			if( IsFiniteDouble(dimension) && dimension >= 1 && dimension <= std::numeric_limits<unsigned int>::max() )
				frame.SetDimensions( frame.GetWidth(), static_cast<unsigned int>(dimension) );
		}
		break;
	}
}

void CameraCommon::RegenerateData( )
{
	Recompute( frame.GetWidth(), frame.GetHeight() );
}
