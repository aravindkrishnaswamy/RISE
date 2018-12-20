//////////////////////////////////////////////////////////////////////
//
//  JitteredSampling1D.cpp - Implementation
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
#include "JitteredSampling1D.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

JitteredSampling1D::JitteredSampling1D( Scalar dSpace_ )
{
	dSpace = dSpace_;
}

JitteredSampling1D::~JitteredSampling1D( )
{
}

void JitteredSampling1D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList1D& samplePoints ) const
{
	samplePoints.clear( );

	// Compute the step size
	const Scalar stepsize = dSpace/Scalar(numSamples);

	for( unsigned int i=0; i<numSamples; i++ ) {
		samplePoints.push_back( stepsize*i+(random.CanonicalRandom()*stepsize) );
	}
}

ISampling1D* JitteredSampling1D::Clone( ) const
{
	JitteredSampling1D*	pMe = new JitteredSampling1D( dSpace );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
