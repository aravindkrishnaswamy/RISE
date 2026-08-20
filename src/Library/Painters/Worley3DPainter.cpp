//////////////////////////////////////////////////////////////////////
//
//  Worley3DPainter.cpp - Implementation of a 3D Worley (cellular)
//  noise painter.  Uses distance to nearest feature points on
//  a jittered grid to produce cell, bubble, and vein patterns.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Worley3DPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Utilities/ProceduralNoiseCore.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// WorleyDistanceMetric/WorleyOutputMode (Noise/WorleyNoise.h) and
	// NoiseCore::WorleyMetric/WorleyMode share the same 0/1/2 ordinal
	// meaning by construction; these just spell the cast site out so a
	// future reordering of either enum fails loudly instead of silently.
	inline NoiseCore::WorleyMetric ToCoreMetric( WorleyDistanceMetric m )
	{
		switch( m ) {
		case eWorley_Manhattan: return NoiseCore::eMetricManhattan;
		case eWorley_Chebyshev: return NoiseCore::eMetricChebyshev;
		case eWorley_Euclidean: default: return NoiseCore::eMetricEuclidean;
		}
	}
	inline NoiseCore::WorleyMode ToCoreMode( WorleyOutputMode o )
	{
		switch( o ) {
		case eWorley_F2: return NoiseCore::eModeF2;
		case eWorley_F2minusF1: return NoiseCore::eModeF2MinusF1;
		case eWorley_F1: default: return NoiseCore::eModeF1;
		}
	}
}

Worley3DPainter::Worley3DPainter(
								 const Scalar dJitter_,
								 const WorleyDistanceMetric eMetric_,
								 const WorleyOutputMode eOutput_,
								 const IPainter& cA_,
								 const IPainter& cB_,
								 const Vector3& vScale_,
								 const Vector3& vShift_
								 ) :
  a( cA_ ),
  b( cB_ ),
  vScale( vScale_ ),
  vShift( vShift_ ),
  dJitter( dJitter_ ),
  eMetric( eMetric_ ),
  eOutput( eOutput_ ),
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

Worley3DPainter::~Worley3DPainter()
{
	safe_release( pInterp );
	safe_release( pColorInterp );

	a.release();
	b.release();
}

// Bit-identical to the historical WorleyNoise3D::Evaluate: same 3x3x3
// jittered-grid search + hash chain (ProceduralNoiseCore::WorleySample3D)
// and the same per-(metric,mode) normalization table
// (ProceduralNoiseCore::WorleyNormalize).
Scalar Worley3DPainter::EvaluateField( const Scalar x, const Scalar y, const Scalar z ) const
{
	const NoiseCore::WorleyMetric coreMetric = ToCoreMetric( eMetric );
	Scalar f1, f2; int cx, cy, cz;
	NoiseCore::WorleySample3D( x, y, z, dJitter, coreMetric, f1, f2, cx, cy, cz );

	Scalar raw;
	switch( eOutput ) {
	case eWorley_F2: raw = f2; break;
	case eWorley_F2minusF1: raw = f2 - f1; break;
	case eWorley_F1: default: raw = f1; break;
	}
	return NoiseCore::WorleyNormalize( raw, coreMetric, ToCoreMode( eOutput ) );
}

RISEPel Worley3DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	Scalar	d = EvaluateField( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	return pColorInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), d );
}

Scalar Worley3DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar	d = EvaluateField( ri.ptIntersection.x*vScale.x+vShift.x, ri.ptIntersection.y*vScale.y+vShift.y, ri.ptIntersection.z*vScale.z+vShift.z );
	return pInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), d );
}


static const unsigned int SCALE_ID = 100;
static const unsigned int SHIFT_ID = 101;

IKeyframeParameter* Worley3DPainter::KeyframeFromParameters( const String& name, const String& value )
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

void Worley3DPainter::SetIntermediateValue( const IKeyframeParameter& val )
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

void Worley3DPainter::RegenerateData( )
{
	// EvaluateField reads dJitter/eMetric/eOutput directly (via
	// ProceduralNoiseCore); nothing to precompute/cache.
}
