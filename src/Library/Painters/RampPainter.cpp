//////////////////////////////////////////////////////////////////////
//
//  RampPainter.cpp - Implementation of RampPainter (doc 88 P2.2, S3).
//  See RampPainter.h for the design rationale.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RampPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

RampPainter::RampPainter( const IPainter& input_, const Channel channel_,
	const Interpolation interp_, const std::vector<Stop>& stops_ )
	: input( input_ ), channel( channel_ ), interp( interp_ )
{
	input.addref();
	stops.reserve( stops_.size() );
	for( std::size_t i = 0; i < stops_.size(); ++i ) {
		stops.push_back( StopEval( stops_[i] ) );
	}
}

RampPainter::~RampPainter()
{
	input.release();
}

Scalar RampPainter::SampleT( const RayIntersectionGeometric& ri ) const
{
	Scalar raw;
	if( channel == Channel_A ) {
		raw = input.GetAlpha( ri );
	} else {
		const RISEPel c = input.GetColor( ri );
		raw = c[ (unsigned int)channel ];
	}

	const Scalar lo = stops.front().pos;
	const Scalar hi = stops.back().pos;
	// A NaN input would sail through both ordered comparisons below (both
	// false for NaN) and poison the interpolation weight; map it to the
	// first stop so a NaN field sample yields a deterministic colour
	// instead of a NaN pixel.  (+/-inf already clamp via the ordered
	// comparisons.)
	if( std::isnan( raw ) ) return lo;
	if( raw < lo ) return lo;
	if( raw > hi ) return hi;
	return raw;
}

void RampPainter::Locate( const Scalar t, std::size_t& i0, std::size_t& i1, Scalar& u ) const
{
	// Find the largest index j such that stops[j].pos <= t (t is
	// already clamped into [stops.front().pos, stops.back().pos] by
	// SampleT, so this always terminates with a valid j).  This is the
	// single locate step shared by all three interpolation modes -- see
	// the file header comment for why it upholds "exact stop value at
	// an exact stop position" for constant/linear/smooth alike.
	std::size_t j = 0;
	while( j + 1 < stops.size() && t >= stops[j + 1].pos ) ++j;

	i0 = j;
	i1 = ( j + 1 < stops.size() ) ? j + 1 : j;

	u = Scalar( 0 );
	if( i0 != i1 ) {
		const Scalar denom = stops[i1].pos - stops[i0].pos;
		if( denom > Scalar( 1e-12 ) ) {
			u = ( t - stops[i0].pos ) / denom;
			if( u < Scalar( 0 ) ) u = Scalar( 0 );
			if( u > Scalar( 1 ) ) u = Scalar( 1 );
		}
	}

	switch( interp ) {
		case Interp_Constant:
			u = Scalar( 0 );	// hold-left: always the exact i0 stop
			break;
		case Interp_Smooth:
			u = u * u * ( Scalar( 3 ) - Scalar( 2 ) * u );	// smoothstep, u already in [0,1]
			break;
		case Interp_Linear:
		default:
			break;
	}
}

RISEPel RampPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	std::size_t i0, i1;
	Scalar u;
	Locate( SampleT( ri ), i0, i1, u );
	return stops[i0].color * ( Scalar( 1 ) - u ) + stops[i1].color * u;
}

Scalar RampPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	std::size_t i0, i1;
	Scalar u;
	Locate( SampleT( ri ), i0, i1, u );
	// Linear combination of the two bracketing stops' precomputed
	// spectra -- same technique BlendPainter::GetColorNM uses to blend
	// two operand spectra by a mask fraction (see file header comment).
	return stops[i0].spec.Eval( nm ) * ( Scalar( 1 ) - u ) + stops[i1].spec.Eval( nm ) * u;
}

SpectralPacket RampPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	std::size_t i0, i1;
	Scalar u;
	Locate( SampleT( ri ), i0, i1, u );

	const Scalar lambda_begin = Scalar( 380 );
	const Scalar lambda_end   = Scalar( 780 );
	const unsigned int nbins  = 81;
	SpectralPacket sp( lambda_begin, lambda_end, nbins );
	const Scalar delta = ( lambda_end - lambda_begin ) / Scalar( nbins );

	const Scalar w0 = Scalar( 1 ) - u;
	const Scalar w1 = u;
	for( unsigned int k = 0; k < nbins; ++k ) {
		const Scalar lambda = lambda_begin + Scalar( k ) * delta;
		sp.SetAtIndex( k, stops[i0].spec.Eval( lambda ) * w0 + stops[i1].spec.Eval( lambda ) * w1 );
	}
	return sp;
}
