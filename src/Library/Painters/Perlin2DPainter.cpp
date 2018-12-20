//////////////////////////////////////////////////////////////////////
//
//  Perlin2DPainter.cpp - Implementation of a 2D perlin noise
//  painter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Perlin2DPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

Perlin2DPainter::Perlin2DPainter( 
								 const Scalar dPersistence_,
								 const unsigned int nOctaves_, 
								 const IPainter& cA_,
								 const IPainter& cB_,
								 const Vector2& vScale_, 
								 const Vector2& vShift_
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

Perlin2DPainter::~Perlin2DPainter()
{
	safe_release( pFunc );
	safe_release( pInterp );
	safe_release( pColorInterp );

	a.release();
	b.release();
}

RISEPel Perlin2DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptCoord.x*vScale.x+vShift.x, ri.ptCoord.y*vScale.y+vShift.y );
	d = (d+1.0)/2.0;
	return pColorInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), d );
}

Scalar Perlin2DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar	d = pFunc->Evaluate( ri.ptCoord.x*vScale.x+vShift.x, ri.ptCoord.y*vScale.y+vShift.y );
	d = (d+1.0)/2.0;
	return pInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), d );
}

Scalar Perlin2DPainter::Evaluate( const Scalar x, const Scalar y ) const
{
	return pFunc->Evaluate( x*vScale.x+vShift.x, y*vScale.y+vShift.y );
}

static const unsigned int SCALE_ID = 100;
static const unsigned int SHIFT_ID = 101;

IKeyframeParameter* Perlin2DPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "scale" ) {
		Vector2 v;
		if( sscanf( value.c_str(), "%lf %lf", &v.x, &v.y ) == 2 ) {
			p = new Parameter<Vector2>( v, SCALE_ID );
		}
	} else if( name == "shift" ) {
		Vector2 v;
		if( sscanf( value.c_str(), "%lf %lf", &v.x, &v.y ) == 2 ) {
			p = new Parameter<Vector2>( v, SHIFT_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void Perlin2DPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case SCALE_ID:
		{
			vScale = *(Vector2*)val.getValue();
		}
		break;
	case SHIFT_ID:
		{
			vShift = *(Vector2*)val.getValue();
		}
		break;
	}
}

void Perlin2DPainter::RegenerateData( )
{
	safe_release( pFunc );

	pFunc = new PerlinNoise2D( *pInterp, dPersistence, nOctaves<32?nOctaves:32 );
	GlobalLog()->PrintNew( pFunc, __FILE__, __LINE__, "NoiseFunction" );

}

