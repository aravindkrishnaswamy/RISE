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
#include "../Utilities/ProceduralNoiseCore.h"
#include "../Animation/KeyframableHelper.h"
#include <cmath>

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
  pInterp( 0 ),
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
	safe_release( pInterp );
	safe_release( pColorInterp );

	a.release();
	b.release();
}

// Matches the historical PerlinNoise3D engine bit-for-bit: amplitude/
// frequency per octave are recomputed via std::pow (not iterative
// multiplication) because that is what PerlinNoise3D's constructor-time
// LUT used -- iterative multiplication can differ in the last bit.
Scalar Perlin3DPainter::EvaluateField( const Scalar x, const Scalar y, const Scalar z ) const
{
	const unsigned int cappedOctaves = ( nOctaves < 32 ) ? nOctaves : 32;
	// nOctaves==0 -> cappedOctaves==0 -> n==-1 -> the loop below doesn't run,
	// so this returns 0 (P2-C: intentional, strict improvement over the
	// historical PerlinNoise3D constructor, which allocated `new Scalar[
	// nOctaves-1]` and crashed on octaves==0 instead).
	const int n = (int)cappedOctaves - 1;
	Scalar total = 0;
	for( int i = 0; i < n; ++i ) {
		const Scalar frequency = std::pow( 2.0, Scalar(i) );
		const Scalar amplitude = std::pow( dPersistence, Scalar(i) );
		total += NoiseCore::PerlinOctave3D( x*frequency, y*frequency, z*frequency ) * amplitude;
	}
	return total;
}

RISEPel Perlin3DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar	d = EvaluateField( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	d = (d+1.0)/2.0;
	return pColorInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), d );
}

Scalar Perlin3DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar	d = EvaluateField( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
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
	// EvaluateField recomputes the octave sum from dPersistence/nOctaves
	// directly (via ProceduralNoiseCore); nothing to precompute/cache.
}

