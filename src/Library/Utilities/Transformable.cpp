//////////////////////////////////////////////////////////////////////
//
//  Transformable.cpp - Implements transform functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Transformable.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

Transformable::Transformable( ) :
	m_mxPosition( Matrix4Ops::Identity() ),
	m_mxOrientation( Matrix4Ops::Identity() ),
	m_mxScale( Matrix4Ops::Identity() ),
	m_mxStretch( Matrix4Ops::Identity() )
{
}

Transformable::~Transformable( )
{
}


void Transformable::PushTopTransStack( const Matrix4& mat )
{
	m_transformstack.push_front( mat );
}

void Transformable::PushBottomTransStack( const Matrix4& mat )
{
	m_transformstack.push_back( mat );
}

void Transformable::PopTopTransStack( )
{
	m_transformstack.pop_front( );
}

void Transformable::PopBottomTransStack( )
{
	m_transformstack.pop_back( );
}

void Transformable::ClearAllTransforms( )
{
	m_transformstack.clear( );
	m_mxPosition = Matrix4Ops::Identity();
	m_mxOrientation = Matrix4Ops::Identity();
	m_mxScale = Matrix4Ops::Identity();
	m_mxStretch = Matrix4Ops::Identity();
	m_mxFinalTrans = Matrix4Ops::Identity();
	m_mxInvFinalTrans = Matrix4Ops::Identity();
}

void Transformable::ClearTransformStack( )
{
	m_transformstack.clear( );
}

void Transformable::TranslateObject( const Vector3& vec )
{
	Matrix4 mx = Matrix4Ops::Translation( vec );
	m_transformstack.push_back( mx );
}

void Transformable::RotateObjectXAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::XRotation( nAmount );
	m_transformstack.push_back( mx );
}

void Transformable::RotateObjectYAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::YRotation( nAmount );
	m_transformstack.push_back( mx );
}

void Transformable::RotateObjectZAxis( const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::ZRotation( nAmount );
	m_transformstack.push_back( mx );
}

void Transformable::RotateObjectArbAxis( const Vector3& axis, const Scalar nAmount )
{
	Matrix4 mx = Matrix4Ops::Rotation( axis, nAmount );
	m_transformstack.push_back( mx );
}

void Transformable::SetPosition( const Point3& pos )
{
	m_mxPosition = Matrix4Ops::Translation( Vector3( pos.x, pos.y, pos.z ) );
}

void Transformable::SetOrientation( const Vector3& orient )
{
	m_mxOrientation = Matrix4Ops::XRotation( orient.x ) * 
		Matrix4Ops::YRotation( orient.y ) * 
		Matrix4Ops::ZRotation( orient.z );
}

void Transformable::SetScale( const Scalar nAmount )
{
	m_mxScale = Matrix4Ops::Scale( nAmount );
}

void Transformable::SetStretch( const Vector3& stretch )
{
	m_mxStretch = Matrix4Ops::Stretch( stretch );
}

void Transformable::FinalizeTransformations( )
{
	m_mxFinalTrans = Matrix4Ops::Identity();

	// First apply the scale, orientation and position matrices
	m_mxFinalTrans = m_mxPosition * m_mxOrientation * m_mxStretch * m_mxScale;

	// Go through the transformation stack and multiply the transformations...
	TransformStackType::const_iterator		i;
	for( i=m_transformstack.begin(); i<m_transformstack.end(); i++ ) {
		m_mxFinalTrans = (*i) * m_mxFinalTrans;
	}

	m_mxInvFinalTrans = Matrix4Ops::Inverse( m_mxFinalTrans );
}

static const unsigned int POSITION_ID = 1000;
static const unsigned int ORIENTATION_ID = 1001;
static const unsigned int SCALE_ID = 1002;

IKeyframeParameter* Transformable::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "position" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, POSITION_ID );
		}
	} else if( name == "orientation" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			v.x *= DEG_TO_RAD;
			v.y *= DEG_TO_RAD;
			v.z *= DEG_TO_RAD;
			p = new Vector3Keyframe( v, ORIENTATION_ID );
		}
	} else if( name == "scale" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, SCALE_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void Transformable::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case POSITION_ID:
		{
			Point3	v = *(Point3*)val.getValue();
			SetPosition( v );
		}
		break;
	case ORIENTATION_ID:
		{
			Vector3	v = *(Vector3*)val.getValue();
			SetOrientation( v );
		}
		break;
	case SCALE_ID:
		{
			Vector3	v = *(Vector3*)val.getValue();
			SetStretch( v );
		}
		break;
	}
}

void Transformable::RegenerateData( )
{
	FinalizeTransformations();
}
