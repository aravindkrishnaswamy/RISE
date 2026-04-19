//////////////////////////////////////////////////////////////////////
//
//  GerstnerWavePainter.cpp - Implementation of the sum-of-sines
//  water-wave painter.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GerstnerWavePainter.h"
#include "../Animation/KeyframableHelper.h"
#include <cmath>
#include <random>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Deep-water dispersion gravity.  Dispersion is ω = speed * sqrt(g*k).
	const Scalar kGravity = 9.81;

	// Portable [0,1) — mt19937 is deterministic across platforms but
	// std::uniform_real_distribution is not.  Convert the 32-bit raw output
	// manually so two machines running the same seed produce identical waves.
	inline Scalar UniformUnit( std::mt19937& rng )
	{
		return Scalar( rng() ) / Scalar( 4294967296.0 );
	}
}

GerstnerWavePainter::GerstnerWavePainter(
	const IPainter&		a_,
	const IPainter&		b_,
	const unsigned int	numWaves_,
	const Scalar		medianWavelength_,
	const Scalar		wavelengthRange_,
	const Scalar		medianAmplitude_,
	const Scalar		amplitudePower_,
	const Scalar		windDirX_,
	const Scalar		windDirY_,
	const Scalar		directionalSpread_,
	const Scalar		dispersionSpeed_,
	const unsigned int	seed_,
	const Scalar		time_ ) :
	a( a_ ),
	b( b_ ),
	numWaves( numWaves_ > 0 ? numWaves_ : 1 ),
	medianWavelength( medianWavelength_ > 0 ? medianWavelength_ : 1.0 ),
	wavelengthRange( wavelengthRange_ >= 1.0 ? wavelengthRange_ : 1.0 ),
	medianAmplitude( medianAmplitude_ ),
	amplitudePower( amplitudePower_ ),
	windDirX( windDirX_ ),
	windDirY( windDirY_ ),
	directionalSpread( directionalSpread_ ),
	dispersionSpeed( dispersionSpeed_ >= 0 ? dispersionSpeed_ : 0.0 ),
	seed( seed_ ),
	totalAmplitude( 0 ),
	m_time( time_ )
{
	GenerateWaves();

	a.addref();
	b.addref();
}

GerstnerWavePainter::~GerstnerWavePainter()
{
	a.release();
	b.release();
}

void GerstnerWavePainter::GenerateWaves()
{
	waves.clear();
	waves.reserve( numWaves );
	totalAmplitude = 0;

	// Normalize wind direction — users can pass any magnitude.
	const Scalar windLen = std::sqrt( windDirX * windDirX + windDirY * windDirY );
	const Scalar wdx = windLen > 0 ? windDirX / windLen : 1.0;
	const Scalar wdy = windLen > 0 ? windDirY / windLen : 0.0;

	// Distribute wavelengths log-uniformly in [median/range, median*range].
	// For numWaves = 1 the single wave sits at medianWavelength.
	const Scalar logMin = std::log( medianWavelength / wavelengthRange );
	const Scalar logMax = std::log( medianWavelength * wavelengthRange );

	std::mt19937 rng( seed );

	for( unsigned int i = 0; i < numWaves; ++i ) {
		const Scalar t = (numWaves > 1) ? Scalar(i) / Scalar(numWaves - 1) : 0.5;
		const Scalar wavelength = std::exp( logMin + t * (logMax - logMin) );
		const Scalar k = TWO_PI / wavelength;
		const Scalar omega = dispersionSpeed * std::sqrt( kGravity * k );

		// A_i = medianAmplitude * (lambda_i / medianWavelength)^amplitudePower
		const Scalar lambdaRatio = wavelength / medianWavelength;
		const Scalar amp = medianAmplitude * std::pow( lambdaRatio, amplitudePower );

		// Direction jitter: wind direction rotated by random angle in
		// [-directionalSpread, +directionalSpread].
		const Scalar jitter = (UniformUnit(rng) * 2.0 - 1.0) * directionalSpread;
		const Scalar cj = std::cos( jitter );
		const Scalar sj = std::sin( jitter );
		const Scalar dx = wdx * cj - wdy * sj;
		const Scalar dy = wdx * sj + wdy * cj;

		const Scalar phase = UniformUnit(rng) * TWO_PI;

		Wave w;
		w.amplitude    = amp;
		w.frequency    = k;
		w.angularSpeed = omega;
		w.dirX         = dx;
		w.dirY         = dy;
		w.phase        = phase;
		waves.push_back( w );

		totalAmplitude += std::fabs( amp );
	}
}

Scalar GerstnerWavePainter::Evaluate( const Scalar x, const Scalar y ) const
{
	Scalar height = 0;
	const std::size_t n = waves.size();
	for( std::size_t i = 0; i < n; ++i ) {
		const Wave& w = waves[i];
		const Scalar phi = w.frequency * (w.dirX * x + w.dirY * y)
		                 - w.angularSpeed * m_time + w.phase;
		height += w.amplitude * std::sin( phi );
	}
	return height;
}

RISEPel GerstnerWavePainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	const Scalar h = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = totalAmplitude > 0
		? std::max( 0.0, std::min( 1.0, (h / totalAmplitude + 1.0) * 0.5 ) )
		: 0.5;
	return a.GetColor(ri) * (1.0 - t) + b.GetColor(ri) * t;
}

Scalar GerstnerWavePainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar h = Evaluate( ri.ptCoord.x, ri.ptCoord.y );
	const Scalar t = totalAmplitude > 0
		? std::max( 0.0, std::min( 1.0, (h / totalAmplitude + 1.0) * 0.5 ) )
		: 0.5;
	return a.GetColorNM(ri,nm) * (1.0 - t) + b.GetColorNM(ri,nm) * t;
}

static const unsigned int TIME_ID = 200;

IKeyframeParameter* GerstnerWavePainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	if( name == "time" ) {
		Scalar v = atof( value.c_str() );
		p = new Parameter<Scalar>( v, TIME_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void GerstnerWavePainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case TIME_ID:
		m_time = *(Scalar*)val.getValue();
		break;
	}

	// Tell any subscribed consumers (typically a DisplacedGeometry that baked
	// a mesh from this painter's Evaluate values) that our state changed.
	// They are responsible for rebuilding their derived state synchronously.
	NotifyObservers();
}

void GerstnerWavePainter::RegenerateData()
{
	// No-op: time is read live in Evaluate; wave spectrum is static.
}
