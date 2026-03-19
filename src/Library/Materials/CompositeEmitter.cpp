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
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;

CompositeEmitter::CompositeEmitter(
	const IEmitter& top_,
	const IEmitter& bottom_,
	const IPainter& extinction_,
	const Scalar thickness_
	) :
  topEmitter( top_ ),
  bottomEmitter( bottom_ ),
  extinction( extinction_ ),
  thickness( thickness_ )
{
	topEmitter.addref();
	bottomEmitter.addref();
	extinction.addref();

	// Compute the average radiant exitance by combining both emitters
	// The bottom emitter's contribution is attenuated by average extinction over the hemisphere
	// For the average, we use a cosine-weighted mean path length of 2*thickness
	// (mean of thickness/cos(theta) weighted by cos(theta) over hemisphere)
	RISEPel topAvg = topEmitter.averageRadiantExitance();

	// Sample extinction over texture space to get an average
	RISEPel avgExtinction;
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	for( int i=0; i<100; i++ ) {
		rig.ptCoord = Point2( GlobalRNG().CanonicalRandom(), GlobalRNG().CanonicalRandom() );
		avgExtinction = avgExtinction + extinction_.GetColor(rig);
	}
	avgExtinction = avgExtinction * (1.0/100.0);

	// Average attenuation: integrate exp(-ext*thickness/cos(theta)) * cos(theta) * sin(theta) dtheta
	// over [0, pi/2], normalized.  For simplicity, use thickness * 2 as the mean path length.
	const RISEPel avgAttenuation = ColorMath::exponential( avgExtinction * (-2.0 * thickness) );

	RISEPel bottomAvg = bottomEmitter.averageRadiantExitance();
	averageRadEx = topAvg + bottomAvg * avgAttenuation;

	// Build the spectral average by iterating over the 40 wavelength bins
	// VisibleSpectralPacket is <Scalar, 380, 780, 40> with delta = (780-380)/(40-1) ~= 10nm
	const Scalar avgExtScalar = (avgExtinction[0] + avgExtinction[1] + avgExtinction[2]) / 3.0;
	const Scalar avgAttenScalar = exp( -avgExtScalar * 2.0 * thickness );
	for( unsigned int i=0; i<40; i++ ) {
		const Scalar nm = Scalar(380 + i * 10);
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
	const RISEPel attenuation = ColorMath::exponential( extinction.GetColor(ri) * (-pathLength) );

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
	const Scalar extinctionNM = extinction.GetColorNM( ri, nm );
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
