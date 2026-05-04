//////////////////////////////////////////////////////////////////////
//
//  ControlledSmoothness2DPainter.cpp — see header.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ControlledSmoothness2DPainter.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

ControlledSmoothness2DPainter::ControlledSmoothness2DPainter(
	const IPainter&		cA_,
	const IPainter&		cB_,
	const Scalar		centerU_,
	const Scalar		centerV_,
	const Scalar		radius_,
	const Scalar		amplitude_,
	const SmoothnessMode mode_ ) :
  a( cA_ ),
  b( cB_ ),
  centerU( centerU_ ),
  centerV( centerV_ ),
  radius( radius_ > NEARZERO ? radius_ : Scalar( 1e-6 ) ),
  amplitude( amplitude_ ),
  mode( mode_ )
{
	a.addref();
	b.addref();
}

ControlledSmoothness2DPainter::~ControlledSmoothness2DPainter()
{
	a.release();
	b.release();
}

namespace
{
	// Hermite smoothstep (cubic).  s ∈ [0,1] → s²(3−2s).  At s=0 and
	// s=1 the function value AND first derivative are 0.  Second
	// derivative jumps at the boundaries.
	inline Scalar SmoothstepCubic( const Scalar s )
	{
		return s * s * ( Scalar( 3 ) - Scalar( 2 ) * s );
	}

	// Hermite smootherstep (quintic).  s ∈ [0,1] → s³(6s²−15s+10).
	// At s=0 and s=1 the value, first AND second derivatives are 0.
	// Third derivative jumps at the boundaries.
	inline Scalar SmoothstepQuintic( const Scalar s )
	{
		return s * s * s * ( s * ( s * Scalar( 6 ) - Scalar( 15 ) ) + Scalar( 10 ) );
	}

	// Returns the bump's height at normalised radial coordinate r ∈ [0, ∞).
	// For r ≥ 1 the bump is zero (except for the Gaussian which has a tail).
	Scalar EvaluateBump( const Scalar r, const ControlledSmoothness2DPainter::SmoothnessMode mode )
	{
		switch( mode )
		{
		case ControlledSmoothness2DPainter::eHeaviside:
			return ( r < Scalar( 1 ) ) ? Scalar( 1 ) : Scalar( 0 );

		case ControlledSmoothness2DPainter::eTent:
			return ( r < Scalar( 1 ) ) ? ( Scalar( 1 ) - r ) : Scalar( 0 );

		case ControlledSmoothness2DPainter::eQuadratic:
		{
			if( r >= Scalar( 1 ) ) return Scalar( 0 );
			const Scalar one_minus_r = Scalar( 1 ) - r;
			return one_minus_r * one_minus_r;
		}

		case ControlledSmoothness2DPainter::eCubic:
			return ( r < Scalar( 1 ) ) ? SmoothstepCubic( Scalar( 1 ) - r ) : Scalar( 0 );

		case ControlledSmoothness2DPainter::eQuintic:
			return ( r < Scalar( 1 ) ) ? SmoothstepQuintic( Scalar( 1 ) - r ) : Scalar( 0 );

		case ControlledSmoothness2DPainter::eGaussian:
		{
			// σ = 0.4 places ~97% of the bump's mass within r < 1.
			// Tail extends past r=1 but is small (≈ exp(−6.25) at r=1).
			const Scalar sigma = Scalar( 0.4 );
			const Scalar t = r / sigma;
			return std::exp( -t * t );
		}

		default:
			return Scalar( 0 );
		}
	}
}

Scalar ControlledSmoothness2DPainter::Evaluate( const Scalar x, const Scalar y ) const
{
	const Scalar du = x - centerU;
	const Scalar dv = y - centerV;
	const Scalar r = std::sqrt( du * du + dv * dv ) / radius;
	return amplitude * EvaluateBump( r, mode );
}

RISEPel ControlledSmoothness2DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	// Map the bump value to [0, 1] for color blending (assumes amplitude > 0;
	// painter usage is typically as a displacement function, not as a colour
	// painter, so this branch is a convenience for visualisation).
	const Scalar v = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = ( amplitude > NEARZERO ) ?
		std::min( std::max( v / amplitude, Scalar( 0 ) ), Scalar( 1 ) ) :
		Scalar( 0 );
	return a.GetColor( ri ) * ( Scalar( 1 ) - t ) + b.GetColor( ri ) * t;
}

Scalar ControlledSmoothness2DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar v = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = ( amplitude > NEARZERO ) ?
		std::min( std::max( v / amplitude, Scalar( 0 ) ), Scalar( 1 ) ) :
		Scalar( 0 );
	return a.GetColorNM( ri, nm ) * ( Scalar( 1 ) - t ) + b.GetColorNM( ri, nm ) * t;
}

IKeyframeParameter* ControlledSmoothness2DPainter::KeyframeFromParameters( const String&, const String& )
{
	return 0;	// no animated parameters in v1
}

void ControlledSmoothness2DPainter::SetIntermediateValue( const IKeyframeParameter& )
{
}

void ControlledSmoothness2DPainter::RegenerateData()
{
}
