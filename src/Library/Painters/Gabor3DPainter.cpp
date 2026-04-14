//////////////////////////////////////////////////////////////////////
//
//  Gabor3DPainter.cpp - Implementation of 3D Gabor noise painter.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Gabor3DPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

Gabor3DPainter::Gabor3DPainter(
								 const Scalar dFrequency_,
								 const Scalar dBandwidth_,
								 const Vector3& vOrientation_,
								 const Scalar dImpulseDensity_,
								 const IPainter& cA_,
								 const IPainter& cB_,
								 const Vector3& vScale_,
								 const Vector3& vShift_
								 ) :
  a( cA_ ),
  b( cB_ ),
  vScale( vScale_ ),
  vShift( vShift_ ),
  dFrequency( dFrequency_ ),
  dBandwidth( dBandwidth_ ),
  vOrientation( vOrientation_ ),
  dImpulseDensity( dImpulseDensity_ ),
  pFunc( 0 ),
  pColorInterp( 0 )
{
	pInterp = new RealLinearInterpolator( );
	GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "RealInterpolator" );

	pColorInterp = new CosineInterpolator<RISEPel>( );
	GlobalLog()->PrintNew( pColorInterp, __FILE__, __LINE__, "ColorInterpolator" );

	RegenerateData();

	a.addref();
	b.addref();
}

Gabor3DPainter::~Gabor3DPainter()
{
	safe_release( pFunc );
	safe_release( pInterp );
	safe_release( pColorInterp );

	a.release();
	b.release();
}

RISEPel Gabor3DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	return pColorInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), d );
}

Scalar Gabor3DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	return pInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), d );
}


static const unsigned int SCALE_ID = 100;
static const unsigned int SHIFT_ID = 101;

IKeyframeParameter* Gabor3DPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	if( name == "scale" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, SCALE_ID );
		}
	} else if( name == "shift" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, SHIFT_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void Gabor3DPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case SCALE_ID:
		{
			vScale = *(Vector3*)val.getValue();
		}
		break;
	case SHIFT_ID:
		{
			vShift = *(Vector3*)val.getValue();
		}
		break;
	}
}

void Gabor3DPainter::RegenerateData( )
{
	safe_release( pFunc );

	pFunc = new GaborNoise3D( dFrequency, dBandwidth, vOrientation, dImpulseDensity, 42 );
	GlobalLog()->PrintNew( pFunc, __FILE__, __LINE__, "NoiseFunction" );
}
