//////////////////////////////////////////////////////////////////////
//
//  PoissonDiskSampling2D.cpp - Implementation
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PoissonDiskSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces//ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

PoissonDiskSampling2D::PoissonDiskSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dSep ) :
  dMinimumSeperation( _dSep )
{
	dSpaceWidth = _dSpaceWidth;
	dSpaceHeight = _dSpaceHeight;
}

PoissonDiskSampling2D::~PoissonDiskSampling2D( )
{
}

bool PoissonDiskSampling2D::TooClose( SamplesList2D& samplePoints, const Point2& v ) const
{
	SamplesList2D::const_iterator		i, e;

	for( i=samplePoints.begin(), e=samplePoints.end(); i!=e; i++ ) {
		Vector2	vec = Vector2Ops::mkVector2(v, *i);
		if( Vector2Ops::Magnitude(vec) <= dMinimumSeperation ) {
			return true;
		}
	}

	return false;
}

void PoissonDiskSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	while( samplePoints.size() < numSamples )
	{
		Point2		v  = Point2( random.CanonicalRandom()*dSpaceWidth, 
								  random.CanonicalRandom()*dSpaceHeight );

		// Go through and check against all the other points to make sure its not
		// too close

		if( !TooClose( samplePoints, v ) ) {
			samplePoints.push_back( v );
		}
	}
}

ISampling2D* PoissonDiskSampling2D::Clone( ) const
{
	PoissonDiskSampling2D*	pMe = new PoissonDiskSampling2D( dSpaceWidth, dSpaceHeight, dMinimumSeperation );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
