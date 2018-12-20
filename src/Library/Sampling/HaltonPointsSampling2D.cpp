//////////////////////////////////////////////////////////////////////
//
//  HaltonPointsSampling2D.cpp - Implementation
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
#include "HaltonPointsSampling2D.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

HaltonPointsSampling2D::HaltonPointsSampling2D( 
	Scalar dSpaceWidth_, 
	Scalar dSpaceHeight_ 
	)
{
	dSpaceWidth = dSpaceWidth_;
	dSpaceHeight = dSpaceHeight_;
}

HaltonPointsSampling2D::~HaltonPointsSampling2D( )
{
}

void HaltonPointsSampling2D::GenerateSamplePoints( const RandomNumberGenerator&, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	for( unsigned int i=0; i<numSamples; i++ ) {
		samplePoints.push_back( Point2(mh.mod1(mh.halton(0,i))*dSpaceWidth, mh.mod1(mh.halton(1,i))*dSpaceHeight) );
	}
}

ISampling2D* HaltonPointsSampling2D::Clone( ) const
{
	HaltonPointsSampling2D*	pMe = new HaltonPointsSampling2D( dSpaceWidth, dSpaceHeight );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
