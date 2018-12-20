//////////////////////////////////////////////////////////////////////
//
//  UniformSampling2D.cpp - Implementation
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
#include "UniformSampling2D.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

UniformSampling2D::UniformSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight )
{
	dSpaceWidth = _dSpaceWidth;
	dSpaceHeight = _dSpaceHeight;
}

UniformSampling2D::~UniformSampling2D( )
{
}


void UniformSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	const unsigned int num_samples = (unsigned int)sqrt( (double(numSamples) ) );

	// We assume num_samples to be the number of samples in each axis
	Scalar		dSegmentSizeX = dSpaceWidth / Scalar(num_samples);
	Scalar		dSegmentSizeY = dSpaceHeight / Scalar(num_samples);

	for( unsigned int i=0; i<num_samples; i++ ) {
		for( unsigned int j=0; j<num_samples; j++ ) {
			Point2		vSample = Point2( dSegmentSizeX/2.0 + Scalar(i)*dSegmentSizeX, 
											dSegmentSizeY/2.0 + Scalar(j)*dSegmentSizeY );
			samplePoints.push_back( vSample );
		}
	}
}

ISampling2D* UniformSampling2D::Clone( ) const
{
	UniformSampling2D*	pMe = new UniformSampling2D( dSpaceWidth, dSpaceHeight );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
