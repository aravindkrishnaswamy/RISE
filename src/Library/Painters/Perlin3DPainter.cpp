//////////////////////////////////////////////////////////////////////
//
//  Perlin3DPainter.cpp - Implementation of a 3D perlin noise
//  painter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Perlin3DPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

Perlin3DPainter::Perlin3DPainter( 
								 const Scalar dPersistence_,
								 const unsigned int nOctaves_,
								 const IPainter& cA_,
								 const IPainter& cB_,
								 const Vector3& vScale_,
								 const Vector3& vShift_
								 ) :
  a( cA_ ),
  b( cB_ ),
  vScale( vScale_ ),
  vShift( vShift_ ), 
  dPersistence( dPersistence_ ),
  nOctaves( nOctaves_ ),
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

Perlin3DPainter::~Perlin3DPainter()
{
	safe_release( pFunc );
	safe_release( pInterp );
	safe_release( pColorInterp );

	a.release();
	b.release();
}

RISEPel Perlin3DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	d = (d+1.0)/2.0;
	return pColorInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), d );
}

Scalar Perlin3DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	d = (d+1.0)/2.0;
	return pInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), d );
}


static const unsigned int SCALE_ID = 100;
static const unsigned int SHIFT_ID = 101;

IKeyframeParameter* Perlin3DPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
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

void Perlin3DPainter::SetIntermediateValue( const IKeyframeParameter& val )
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

void Perlin3DPainter::RegenerateData( )
{
	safe_release( pFunc );

	pFunc = new PerlinNoise3D( *pInterp, dPersistence, nOctaves<32?nOctaves:32 );
	GlobalLog()->PrintNew( pFunc, __FILE__, __LINE__, "NoiseFunction" );
}

