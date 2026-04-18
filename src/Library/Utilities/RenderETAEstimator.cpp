//////////////////////////////////////////////////////////////////////
//
//  RenderETAEstimator.cpp - Implementation. See header for rationale.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-17
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "RenderETAEstimator.h"

#include <chrono>
#include <cstdio>
#include <algorithm>

using namespace RISE;

RenderETAEstimator::Config::Config()
	: warmupSeconds( 2.0 )
	, warmupFraction( 0.03 )
	, resetThreshold( 0.10 )
	, displayAlpha( 0.25 )
{
}

RenderETAEstimator::RenderETAEstimator()
	: cfg()
	, t0( 0.0 )
	, pMax( 0.0 )
	, displayedRem( 0.0 )
	, tLastCorrect( 0.0 )
	, started( false )
	, displayedInit( false )
{
}

RenderETAEstimator::RenderETAEstimator( const Config& c )
	: cfg( c )
	, t0( 0.0 )
	, pMax( 0.0 )
	, displayedRem( 0.0 )
	, tLastCorrect( 0.0 )
	, started( false )
	, displayedInit( false )
{
}

RenderETAEstimator::~RenderETAEstimator()
{
}

double RenderETAEstimator::NowSeconds() const
{
	using Clock = std::chrono::steady_clock;
	static const Clock::time_point epoch = Clock::now();
	const Clock::duration d = Clock::now() - epoch;
	return std::chrono::duration<double>( d ).count();
}

void RenderETAEstimator::Begin()
{
	const double now = NowSeconds();
	t0 = now;
	pMax = 0.0;
	displayedRem = 0.0;
	tLastCorrect = now;
	started = true;
	displayedInit = false;
}

void RenderETAEstimator::Update( const double progress, const double total )
{
	if( !started ) {
		Begin();
	}

	double frac = 0.0;
	if( total > 0.0 ) {
		frac = progress / total;
	}
	if( frac < 0.0 ) frac = 0.0;
	if( frac > 1.0 ) frac = 1.0;

	const double now = NowSeconds();

	// Large backward jump — treat as a real reset (e.g. MLT bootstrap
	// finishing and the mutation phase restarting progress at 0).
	// Restart timing so the rate computation for the new phase is
	// based on that phase's own elapsed.
	if( frac + cfg.resetThreshold < pMax ) {
		t0 = now;
		pMax = frac;
		tLastCorrect = now;
		displayedInit = false;
		displayedRem = 0.0;
		return;
	}

	// Small backward or equal sample — the rasterizer has multiple
	// workers whose Progress() calls race through the serialization
	// mutex out of order, so a slightly-lower idx is normal. Ignore
	// without touching any state; a forward sample will follow.
	if( frac <= pMax ) {
		return;
	}

	pMax = frac;

	// Warmup gates.
	const double elapsed = now - t0;
	if( elapsed < cfg.warmupSeconds ) return;
	if( pMax < cfg.warmupFraction ) return;

	if( pMax >= 1.0 ) {
		displayedRem = 0.0;
		tLastCorrect = now;
		displayedInit = true;
		return;
	}

	// Raw estimate uses only overall average rate, which is immune to
	// tile-level jitter. Works out to elapsed * (1 - pMax) / pMax.
	const double raw = elapsed * ( 1.0 - pMax ) / pMax;

	if( !displayedInit ) {
		displayedRem = raw;
		displayedInit = true;
	} else {
		// Countdown between corrections: since the last correction
		// the true remaining has decreased by that many seconds. Nudge
		// the countdown toward the new raw estimate.
		const double timeSince = now - tLastCorrect;
		double countdown = displayedRem - timeSince;
		if( countdown < 0.0 ) countdown = 0.0;
		const double a = cfg.displayAlpha;
		displayedRem = a * raw + ( 1.0 - a ) * countdown;
	}
	tLastCorrect = now;
}

double RenderETAEstimator::ElapsedSeconds() const
{
	if( !started ) return 0.0;
	const double e = NowSeconds() - t0;
	return e < 0.0 ? 0.0 : e;
}

double RenderETAEstimator::ProgressFraction() const
{
	return pMax;
}

bool RenderETAEstimator::RemainingSeconds( double& outSeconds ) const
{
	if( !displayedInit ) return false;

	// Between Update() ticks, the displayed value counts down in real
	// time so the UI shows a smooth timer rather than a stale number
	// frozen until the next callback arrives.
	const double timeSince = NowSeconds() - tLastCorrect;
	double out = displayedRem - timeSince;
	if( out < 0.0 ) out = 0.0;
	outSeconds = out;
	return true;
}

std::string RenderETAEstimator::FormatDuration( const double seconds )
{
	double s = seconds;
	if( s < 0.0 ) s = 0.0;
	if( s > 359999.0 ) s = 359999.0;  // 99:59:59 cap for display
	const long total = static_cast<long>( s + 0.5 );
	const long hh = total / 3600;
	const long mm = ( total % 3600 ) / 60;
	const long ss = total % 60;

	char buf[16];
	if( hh > 0 ) {
		std::snprintf( buf, sizeof( buf ), "%02ld:%02ld:%02ld", hh, mm, ss );
	} else {
		std::snprintf( buf, sizeof( buf ), "%ld:%02ld", mm, ss );
	}
	return std::string( buf );
}
