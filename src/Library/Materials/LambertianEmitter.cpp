//////////////////////////////////////////////////////////////////////
//
//  LambertianEmitter.cpp - Implements the lambertian
//  emitter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LambertianEmitter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

LambertianEmitter::LambertianEmitter( const IPainter& radEx, const Scalar scale_ ) :
  pRadEx( &radEx ),
  scale( scale_ )
{
	pRadEx->addref();
	RefreshAverages();
}

LambertianEmitter::~LambertianEmitter( )
{
	safe_release( pRadEx );
}

void LambertianEmitter::RefreshAverages()
{
	// Re-initialise then accumulate — important on Set so we don't add
	// on top of the previous painter's contribution.
	averageRadEx = RISEPel( 0, 0, 0 );
	averageSpectrum = VisibleSpectralPacket();

	// Sample the texture space of the radiance exitance to compute the average radiant exitance
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	// Deterministic 10x10 stratified UV grid (cell centres), NOT 100 GlobalRNG samples: reproducible and
	// consumes no render-RNG at parse (this runs at emitter construction). It feeds only a light-importance
	// weight, so the converged render is unchanged -- a grid is also a lower-variance estimate of the same
	// painter average.
	for( int gy=0; gy<10; gy++ ) for( int gx=0; gx<10; gx++ ) {
		rig.ptCoord = Point2( (Scalar(gx)+Scalar(0.5))/Scalar(10), (Scalar(gy)+Scalar(0.5))/Scalar(10) );
		averageRadEx = averageRadEx + pRadEx->GetColor(rig);
		averageSpectrum = averageSpectrum + pRadEx->GetSpectrum(rig);
	}

	averageRadEx = averageRadEx * (scale/Scalar(100.0));
	averageSpectrum = averageSpectrum * (scale/Scalar(100.0));
}

void LambertianEmitter::SetRadEx( const IPainter& v )
{
	v.addref();
	safe_release( pRadEx );
	pRadEx = &v;
	RefreshAverages();
}

RISEPel LambertianEmitter::emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const
{
	// One-sided emission: only emit from the front hemisphere (where
	// the outgoing direction is on the same side as the surface normal).
	if( Vector3Ops::Dot( out, N ) <= 0 ) {
		return RISEPel( 0, 0, 0 );
	}
	return (pRadEx->GetColor( ri ) * INV_PI * scale);
}

Scalar LambertianEmitter::emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const
{
	if( Vector3Ops::Dot( out, N ) <= 0 ) {
		return 0;
	}
	return (pRadEx->GetColorNM( ri, nm ) * INV_PI * scale);
}

RISEPel LambertianEmitter::averageRadiantExitance() const
{
	return averageRadEx;
}

Scalar LambertianEmitter::averageRadiantExitanceNM( const Scalar nm ) const 
{
	return averageSpectrum.ValueAtNM( int(nm) );
}

Vector3 LambertianEmitter::getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random ) const
{
	return GeometricUtilities::CreateDiffuseVector( ri.onb, random );
}
