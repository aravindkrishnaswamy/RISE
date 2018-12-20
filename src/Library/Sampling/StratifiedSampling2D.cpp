//////////////////////////////////////////////////////////////////////
//
//  StratifiedSampling2D.cpp - Implementation
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
#include "StratifiedSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

StratifiedSampling2D::StratifiedSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dHowFar ) :
  dHowFar( _dHowFar )
{
	dSpaceWidth = _dSpaceWidth;
	dSpaceHeight = _dSpaceHeight;
}

StratifiedSampling2D::~StratifiedSampling2D( )
{
}

void StratifiedSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	const unsigned int num_samples = (unsigned int)sqrt( (double(numSamples) ) );

	// For each strata randomly sample independently in X and Y

	// We assume num_samples to be the number of samples in each axis
	Vector2	vSegmentSize = Vector2( dSpaceWidth / Scalar(num_samples), 
										 dSpaceHeight / Scalar(num_samples) );


	for( unsigned int i=0; i<num_samples; i++ )
	{
		for( unsigned int j=0; j<num_samples; j++ )
		{
			// Determine the bounds for this strata
			Point2		ptStrataStart = Point2( i*vSegmentSize.x, j*vSegmentSize.y );
			Point2		ptStrataCenter = Point2Ops::mkPoint2(ptStrataStart, (vSegmentSize*0.5));

			Scalar		dRandX = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;
			Scalar		dRandY = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;

			Point2		v = Point2( 
				dRandX*vSegmentSize.x/2 + ptStrataCenter.x,
                dRandY*vSegmentSize.y/2 + ptStrataCenter.y );

			samplePoints.push_back( v );
		}
	}
}

ISampling2D* StratifiedSampling2D::Clone( ) const
{
	StratifiedSampling2D*	pMe = new StratifiedSampling2D( dSpaceWidth, dSpaceHeight, dHowFar );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
