//////////////////////////////////////////////////////////////////////
//
//  PolynomialFunction2DPainter.cpp — see header.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolynomialFunction2DPainter.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Integer power.  unsigned exponent; we never need negative.
	// Open-coded (rather than std::pow) because (1) we want exact
	// 1.0 for 0^0 / x^0, (2) avoids any FPU drift on the loop body
	// across builds, and (3) is faster for small exponents (which
	// is the common case here).
	inline Scalar IntPow( Scalar x, unsigned int p )
	{
		Scalar result = Scalar( 1 );
		for( unsigned int i = 0; i < p; ++i ) {
			result *= x;
		}
		return result;
	}

	// Avoid /0 on user-supplied `scale`.  Matches the
	// `radius_ > NEARZERO ? radius_ : 1e-6` pattern in
	// ControlledSmoothness2DPainter.
	inline Scalar ClampScale( Scalar s )
	{
		return ( std::abs( s ) > NEARZERO ) ? s : Scalar( 1e-6 );
	}
}

PolynomialFunction2DPainter::PolynomialFunction2DPainter(
	const IPainter&			cA_,
	const IPainter&			cB_,
	const PolynomialType	type_,
	const Scalar			centerU_,
	const Scalar			centerV_,
	const Scalar			scaleU_,
	const Scalar			scaleV_,
	const Scalar			amplitude_,
	const unsigned int		degree_,
	const unsigned int		powerX_,
	const unsigned int		powerY_,
	const Scalar*			pCoeffs,
	const unsigned int		nCoeffs ) :
  a( cA_ ),
  b( cB_ ),
  type( type_ ),
  centerU( centerU_ ),
  centerV( centerV_ ),
  scaleU( ClampScale( scaleU_ ) ),
  scaleV( ClampScale( scaleV_ ) ),
  amplitude( amplitude_ ),
  degree( degree_ ),
  powerX( powerX_ ),
  powerY( powerY_ )
{
	a.addref();
	b.addref();

	if( type == eBivariate && pCoeffs && nCoeffs > 0 ) {
		bivariateCoeffs.assign( pCoeffs, pCoeffs + nCoeffs );
	}
}

PolynomialFunction2DPainter::~PolynomialFunction2DPainter()
{
	a.release();
	b.release();
}

Scalar PolynomialFunction2DPainter::Evaluate( const Scalar u, const Scalar v ) const
{
	const Scalar x = ( u - centerU ) / scaleU;
	const Scalar y = ( v - centerV ) / scaleV;

	switch( type )
	{
	case eRadialBump:
	{
		const Scalar r2 = x * x + y * y;
		const Scalar t  = Scalar( 1 ) - r2;
		if( t <= Scalar( 0 ) ) {
			return Scalar( 0 );
		}
		return amplitude * IntPow( t, degree );
	}

	case eMonomial:
		return amplitude * IntPow( x, powerX ) * IntPow( y, powerY );

	case eParaboloid:
		return amplitude * ( x * x + y * y );

	case eHyperbolicSaddle:
		return amplitude * ( x * x - y * y );

	case eMonkeySaddle:
		return amplitude * ( x * x * x - Scalar( 3 ) * x * y * y );

	case eBivariate:
	{
		// Sum a_{ij} x^i y^j for i + j ≤ degree.  Coefficient order
		// matches the header docstring: by total degree (low→high),
		// then by descending power of x.  Within total-degree td,
		// the inner index `k` walks (x^td·y^0, x^(td-1)·y^1, …,
		// x^0·y^td) so the i-th term inside td has x-exponent (td − i)
		// and y-exponent i.  Missing coefficients are zero; extras
		// past the triangle are ignored.
		Scalar result = Scalar( 0 );
		unsigned int k = 0;
		const unsigned int nCoeffs = static_cast<unsigned int>( bivariateCoeffs.size() );
		for( unsigned int td = 0; td <= degree; ++td )
		{
			for( unsigned int i = 0; i <= td; ++i )
			{
				if( k >= nCoeffs ) {
					return amplitude * result;	// rest of triangle treated as 0
				}
				const unsigned int xPow = td - i;
				const unsigned int yPow = i;
				result += bivariateCoeffs[ k ] * IntPow( x, xPow ) * IntPow( y, yPow );
				++k;
			}
		}
		return amplitude * result;
	}

	default:
		return Scalar( 0 );
	}
}

namespace
{
	// Map polynomial value to a [0,1] colour-interp parameter.
	//
	// Contract (documented at GetColor / GetColorNM): colorB is the
	// POSITIVE-end colour, colorA is the ZERO-OR-NEGATIVE-end colour.
	// The mapping is independent of the SIGN of `amplitude` — we
	// normalise by `|amplitude|` so a polynomial value of v stays
	// keyed to v's sign, not amplitude's sign.
	//
	// Why this matters: an "inverted bump" authored with
	// `amplitude = -1` evaluates to v = -1 at the centre.  Dividing
	// by amplitude (not |amplitude|) would give t = +1 → colorB,
	// implying the centre is the POSITIVE peak of the function —
	// the opposite of what's true.  Dividing by |amplitude| gives
	// t = -1, clamped to 0 → colorA, which faithfully reports that
	// the centre is the function's most-negative point.
	//
	// If a scene author wants colorB at the deformation extremum
	// regardless of sign, they should pick `colorA` and `colorB`
	// according to the sign of the amplitude they're authoring —
	// not have the painter silently flip the mapping.
	//
	// Clamping matches ControlledSmoothness2DPainter and
	// CompositeFunction2DPainter.
	inline Scalar NormaliseForColour( const Scalar v, const Scalar amplitude )
	{
		const Scalar absAmplitude = std::abs( amplitude );
		const Scalar t = ( absAmplitude > NEARZERO ) ?
			( v / absAmplitude ) :
			Scalar( 0 );
		return std::min( std::max( t, Scalar( 0 ) ), Scalar( 1 ) );
	}
}

RISEPel PolynomialFunction2DPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	const Scalar v = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = NormaliseForColour( v, amplitude );
	return a.GetColor( ri ) * ( Scalar( 1 ) - t ) + b.GetColor( ri ) * t;
}

Scalar PolynomialFunction2DPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar v = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = NormaliseForColour( v, amplitude );
	return a.GetColorNM( ri, nm ) * ( Scalar( 1 ) - t ) + b.GetColorNM( ri, nm ) * t;
}

IKeyframeParameter* PolynomialFunction2DPainter::KeyframeFromParameters( const String&, const String& )
{
	return 0;	// no animated parameters in v1
}

void PolynomialFunction2DPainter::SetIntermediateValue( const IKeyframeParameter& )
{
}

void PolynomialFunction2DPainter::RegenerateData()
{
}
