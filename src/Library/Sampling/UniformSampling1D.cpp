//////////////////////////////////////////////////////////////////////
//
//  UniformSampling1D.cpp - Implementation
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 23, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "UniformSampling1D.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

UniformSampling1D::UniformSampling1D( Scalar dSpace_ )
{
	dSpace = dSpace_;
}

UniformSampling1D::~UniformSampling1D( )
{
}

void UniformSampling1D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList1D& samplePoints ) const
{
	samplePoints.clear( );

	// Compute the step size
	const Scalar stepsize = dSpace/Scalar(numSamples-1);

	for( unsigned int i=0; i<numSamples; i++ ) {
		samplePoints.push_back( stepsize*i );
	}
}

ISampling1D* UniformSampling1D::Clone( ) const
{
	UniformSampling1D*	pMe = new UniformSampling1D( dSpace );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
