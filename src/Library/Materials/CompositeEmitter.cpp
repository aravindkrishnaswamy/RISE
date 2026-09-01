//////////////////////////////////////////////////////////////////////
//
//  CompositeEmitter.cpp - Implements the composite emitter that
//  combines emission from two layers with Beer's law absorption
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompositeEmitter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

// Same exposure as CompositeSPF: `thickness` feeds the Beer-Lambert exponent
// unguarded (the -2.0*thickness mean-path term below, and the per-direction
// thickness/cosTheta in emittedRadiance{,NM}), so a negative value turns the
// bottom layer's attenuation into GAIN.  Here it is worse than in the SPF --
// the amplified value also lands in averageRadiantExitance, which drives
// light-importance weights and photon budgets.  Clamp identically.
// The test is NEGATED (`!(thickness >= 0)`) so a NaN -- false against every
// comparison, and therefore invisible to a plain `< 0` -- clamps as well.
// Here that also protects `averageRadEx` / `averageSpectrum`, i.e. the
// light-importance weights and photon budgets, from a NaN.
static Scalar ClampCompositeThickness( const Scalar thickness )
{
	if( !( thickness >= 0 ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"CompositeEmitter:: inter-layer thickness (%g) is not >= 0 -- a negative path length makes the Beer-Lambert term amplify rather than absorb, and a NaN poisons it -- clamping to 0",
			thickness );
		return 0;
	}
	return thickness;
}

CompositeEmitter::CompositeEmitter(
	const IEmitter& top_,
	const IEmitter& bottom_,
	const IScalarPainter& extinction_,
	const Scalar thickness_
	) :
  topEmitter( top_ ),
  bottomEmitter( bottom_ ),
  extinction( extinction_ ),
  thickness( ClampCompositeThickness( thickness_ ) )
{
	topEmitter.addref();
	bottomEmitter.addref();
	extinction.addref();

	// Compute the average radiant exitance by combining both emitters
	// The bottom emitter's contribution is attenuated by average extinction over the hemisphere
	// For the average, we use a cosine-weighted mean path length of 2*thickness
	// (mean of thickness/cos(theta) weighted by cos(theta) over hemisphere)
	RISEPel topAvg = topEmitter.averageRadiantExitance();

	// Sample extinction over texture space to get an average.
	//
	// BOTH averages below come from the SAME scalar painter, read through the
	// SAME grid: `GetValuesAt` for the RGB average and `GetValueAtNM` for the
	// per-wavelength one.  That is the point of this block.  Before the
	// IScalarPainter retyping the RGB average came from `IPainter::GetColor`
	// (unsaturated) while `emittedRadianceNM` read `IPainter::GetColorNM`
	// (Jakob-Hanika albedo uplift, clamped to [0,1]) -- so at any extinction
	// above ~1 the emitter's own average and its per-hit spectral radiance
	// described different materials.  Neither read touches colourspace now,
	// and the spectral average is sampled per-bin instead of being collapsed
	// to the mean of the three RGB channels, so a wavelength-varying painter
	// (e.g. an inline per-channel triple, which interpolates across the
	// visible band) is averaged at the wavelength it will actually be
	// evaluated at.
	RISEPel avgExtinction;
	Scalar  avgExtinctionNM[40] = { Scalar(0) };
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	// Deterministic 10x10 stratified UV grid (cell centres), NOT 100 GlobalRNG samples: reproducible and
	// consumes no render-RNG at parse (this runs at emitter construction).  averageRadEx/averageSpectrum feed
	// light-importance weights (LightSampler) + photon power/budget (PhotonTracer / SpectralPhotonTracer /
	// SMSPhotonMap), NOT the emitted radiance (emittedRadiance reads the painter at the hit point) -- so this
	// is not a DIRECT emitted-radiance change.  For a UNIFORM emissive painter the grid mean is bit-identical
	// to the old RNG mean; for a NON-uniform (textured) painter it is a different deterministic estimate of
	// the same integral, so a FINITE photon-map / SMS render of a textured emitter can differ slightly
	// (converging to the same result), and a regular grid can alias a painter whose period resonates with
	// the 0.1-UV pitch.  The determinism is required for a reproducible parse (the v6->v7 cutover gate).
	for( int gy=0; gy<10; gy++ ) for( int gx=0; gx<10; gx++ ) {
		rig.ptCoord = Point2( (Scalar(gx)+Scalar(0.5))/Scalar(10), (Scalar(gy)+Scalar(0.5))/Scalar(10) );
		const ScalarTriple e = extinction_.GetValuesAt(rig);
		avgExtinction = avgExtinction + RISEPel( e.v[0], e.v[1], e.v[2] );
		for( unsigned int i=0; i<40; i++ ) {
			avgExtinctionNM[i] += extinction_.GetValueAtNM( rig, Scalar(380 + i * 10) );
		}
	}
	avgExtinction = avgExtinction * (1.0/100.0);
	for( unsigned int i=0; i<40; i++ ) {
		avgExtinctionNM[i] *= (1.0/100.0);
	}

	// Average attenuation: integrate exp(-ext*thickness/cos(theta)) * cos(theta) * sin(theta) dtheta
	// over [0, pi/2], normalized.  For simplicity, use thickness * 2 as the mean path length.
	const RISEPel avgAttenuation = ColorMath::exponential( avgExtinction * (-2.0 * thickness) );

	RISEPel bottomAvg = bottomEmitter.averageRadiantExitance();
	averageRadEx = topAvg + bottomAvg * avgAttenuation;

	// Build the spectral average by iterating over the 40 wavelength bins
	// VisibleSpectralPacket is <Scalar, 380, 780, 40> with delta = (780-380)/(40-1) ~= 10nm
	for( unsigned int i=0; i<40; i++ ) {
		const Scalar nm = Scalar(380 + i * 10);
		const Scalar avgAttenScalar = exp( -avgExtinctionNM[i] * 2.0 * thickness );
		const Scalar topVal = topEmitter.averageRadiantExitanceNM( nm );
		const Scalar bottomVal = bottomEmitter.averageRadiantExitanceNM( nm );
		averageSpectrum.SetIndex( i, topVal + bottomVal * avgAttenScalar );
	}
}

CompositeEmitter::~CompositeEmitter()
{
	topEmitter.release();
	bottomEmitter.release();
	extinction.release();
}

RISEPel CompositeEmitter::emittedRadiance(
	const RayIntersectionGeometric& ri,
	const Vector3& out,
	const Vector3& N
	) const
{
	// Top layer's emission exits directly
	RISEPel result = topEmitter.emittedRadiance( ri, out, N );

	// Bottom layer's emission is attenuated by Beer's law through the medium
	const Scalar cosTheta = fabs( Vector3Ops::Dot( out, N ) );
	const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
	const ScalarTriple e = extinction.GetValuesAt(ri);
	const RISEPel attenuation = ColorMath::exponential( RISEPel( e.v[0], e.v[1], e.v[2] ) * (-pathLength) );

	result = result + bottomEmitter.emittedRadiance( ri, out, N ) * attenuation;

	return result;
}

Scalar CompositeEmitter::emittedRadianceNM(
	const RayIntersectionGeometric& ri,
	const Vector3& out,
	const Vector3& N,
	const Scalar nm
	) const
{
	Scalar result = topEmitter.emittedRadianceNM( ri, out, N, nm );

	const Scalar cosTheta = fabs( Vector3Ops::Dot( out, N ) );
	const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
	const Scalar extinctionNM = extinction.GetValueAtNM( ri, nm );
	const Scalar attenuation = exp( -extinctionNM * pathLength );

	result += bottomEmitter.emittedRadianceNM( ri, out, N, nm ) * attenuation;

	return result;
}

RISEPel CompositeEmitter::averageRadiantExitance() const
{
	return averageRadEx;
}

Scalar CompositeEmitter::averageRadiantExitanceNM( const Scalar nm ) const
{
	return averageSpectrum.ValueAtNM( int(nm) );
}

Vector3 CompositeEmitter::getEmmittedPhotonDir(
	const RayIntersectionGeometric& ri,
	const Point2& random
	) const
{
	// Choose which emitter to emit from, weighted by their relative exitances
	const Scalar topWeight = ColorMath::MaxValue( topEmitter.averageRadiantExitance() );
	const Scalar bottomWeight = ColorMath::MaxValue( bottomEmitter.averageRadiantExitance() );
	const Scalar totalWeight = topWeight + bottomWeight;

	if( totalWeight < NEARZERO ) {
		return GeometricUtilities::CreateDiffuseVector( ri.onb, random );
	}

	// Use the x component of random for layer selection, y for the actual direction
	if( random.x * totalWeight < topWeight ) {
		return topEmitter.getEmmittedPhotonDir( ri, Point2( random.x * totalWeight / topWeight, random.y ) );
	} else {
		return bottomEmitter.getEmmittedPhotonDir( ri, Point2( (random.x * totalWeight - topWeight) / bottomWeight, random.y ) );
	}
}
