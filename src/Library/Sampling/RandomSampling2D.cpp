//////////////////////////////////////////////////////////////////////
//
//  RandomSampling2D.cpp - Implementation
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
#include "RandomSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

RandomSampling2D::RandomSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight )
{
	dSpaceWidth = _dSpaceWidth;
	dSpaceHeight = _dSpaceHeight;
}

RandomSampling2D::~RandomSampling2D( )
{
}

void RandomSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	for( unsigned int i=0; i<numSamples; i++ )
	{
		Point2		v  = Point2( random.CanonicalRandom()*dSpaceWidth, 
								  random.CanonicalRandom()*dSpaceHeight );

		samplePoints.push_back( v );
	}
}

ISampling2D* RandomSampling2D::Clone( ) const
{
	RandomSampling2D*	pMe = new RandomSampling2D( dSpaceWidth, dSpaceHeight );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
