//////////////////////////////////////////////////////////////////////
//
//  CompositeFunction2DPainter.cpp — see header.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompositeFunction2DPainter.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

CompositeFunction2DPainter::CompositeFunction2DPainter(
	const IPainter&		colorA_,
	const IPainter&		colorB_,
	const IFunction2D&	childA_,
	const IFunction2D&	childB_,
	const CompositeOp	op_,
	const Scalar		weightA_,
	const Scalar		uvScaleAU_,
	const Scalar		uvScaleAV_,
	const Scalar		uvOffsetAU_,
	const Scalar		uvOffsetAV_,
	const Scalar		weightB_,
	const Scalar		uvScaleBU_,
	const Scalar		uvScaleBV_,
	const Scalar		uvOffsetBU_,
	const Scalar		uvOffsetBV_,
	const Scalar		lerpT_,
	const Scalar		outputScale_,
	const Scalar		outputOffset_ ) :
  colA( colorA_ ),
  colB( colorB_ ),
  fA( childA_ ),
  fB( childB_ ),
  op( op_ ),
  weightA( weightA_ ),
  uvScaleAU( uvScaleAU_ ),
  uvScaleAV( uvScaleAV_ ),
  uvOffsetAU( uvOffsetAU_ ),
  uvOffsetAV( uvOffsetAV_ ),
  weightB( weightB_ ),
  uvScaleBU( uvScaleBU_ ),
  uvScaleBV( uvScaleBV_ ),
  uvOffsetBU( uvOffsetBU_ ),
  uvOffsetBV( uvOffsetBV_ ),
  lerpT( std::min( std::max( lerpT_, Scalar( 0 ) ), Scalar( 1 ) ) ),
  outputScale( outputScale_ ),
  outputOffset( outputOffset_ )
{
	colA.addref();
	colB.addref();
	fA.addref();
	fB.addref();
}

CompositeFunction2DPainter::~CompositeFunction2DPainter()
{
	colA.release();
	colB.release();
	fA.release();
	fB.release();
}

Scalar CompositeFunction2DPainter::Evaluate( const Scalar x, const Scalar y ) const
{
	const Scalar xa = uvScaleAU * x + uvOffsetAU;
	const Scalar ya = uvScaleAV * y + uvOffsetAV;
	const Scalar xb = uvScaleBU * x + uvOffsetBU;
	const Scalar yb = uvScaleBV * y + uvOffsetBV;

	const Scalar A = weightA * fA.Evaluate( xa, ya );
	const Scalar B = weightB * fB.Evaluate( xb, yb );

	Scalar result;
	switch( op )
	{
	case eSum:        result = A + B;                                            break;
	case eProduct:    result = A * B;                                            break;
	case eLerp:       result = ( Scalar( 1 ) - lerpT ) * A + lerpT * B;          break;
	case eMax:        result = std::max( A, B );                                 break;
	case eMin:        result = std::min( A, B );                                 break;
	case eDifference: result = A - B;                                            break;
	default:          result = A + B;                                            break;	// unreachable; constructor + factory guard
	}

	return outputScale * result + outputOffset;
}

namespace
{
	// Map composite Evaluate value to a [0,1] colour-interp parameter.
	// The composite has no intrinsic normalisation range — the
	// user-visible contract is that `colorB` corresponds to "high"
	// composite values and `colorA` to "low".  We clamp at the unit
	// interval and let the scene author tune via `output_scale` /
	// `output_offset` if a non-default range is desired.  This
	// matches the ControlledSmoothness2DPainter convention.
	inline Scalar NormaliseForColour( const Scalar v )
	{
		return std::min( std::max( v, Scalar( 0 ) ), Scalar( 1 ) );
	}
}

RISEPel CompositeFunction2DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	const Scalar t = NormaliseForColour( Evaluate( ri.ptCoord.x, ri.ptCoord.y ) );
	return colA.GetColor( ri ) * ( Scalar( 1 ) - t ) + colB.GetColor( ri ) * t;
}

Scalar CompositeFunction2DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar t = NormaliseForColour( Evaluate( ri.ptCoord.x, ri.ptCoord.y ) );
	return colA.GetColorNM( ri, nm ) * ( Scalar( 1 ) - t ) + colB.GetColorNM( ri, nm ) * t;
}

IKeyframeParameter* CompositeFunction2DPainter::KeyframeFromParameters( const String&, const String& )
{
	return 0;	// no animated parameters in v1
}

void CompositeFunction2DPainter::SetIntermediateValue( const IKeyframeParameter& )
{
}

void CompositeFunction2DPainter::RegenerateData()
{
}
