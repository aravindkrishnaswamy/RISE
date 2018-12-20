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
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;

LambertianEmitter::LambertianEmitter( const IPainter& radEx, const Scalar scale_ ) : 
  radExPainter( radEx ),
  scale( scale_ )
{
	radExPainter.addref();

	// Sample the texture space of the radiance exitance to compute the average radiant exitance
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	for( int i=0; i<100; i++ ) {
		rig.ptCoord = Point2( GlobalRNG().CanonicalRandom(), GlobalRNG().CanonicalRandom() );
		averageRadEx = averageRadEx + radEx.GetColor(rig);
		averageSpectrum = averageSpectrum + radEx.GetSpectrum(rig);
	}

	averageRadEx = averageRadEx * (scale/Scalar(100.0));
	averageSpectrum = averageSpectrum * (scale/Scalar(100.0));
}

LambertianEmitter::~LambertianEmitter( )
{
	radExPainter.release();
}

RISEPel LambertianEmitter::emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const
{
	return (radExPainter.GetColor( ri ) * INV_PI * scale);
}

Scalar LambertianEmitter::emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const
{
	return (radExPainter.GetColorNM( ri, nm ) * INV_PI * scale);
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
