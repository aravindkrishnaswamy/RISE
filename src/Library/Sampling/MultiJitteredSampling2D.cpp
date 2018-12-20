//////////////////////////////////////////////////////////////////////
//
//  MultiJitteredSampling2D.cpp - Implementation
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MultiJitteredSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

MultiJitteredSampling2D::MultiJitteredSampling2D( Scalar dSpaceWidth_, Scalar dSpaceHeight_ )
{
	dSpaceWidth = dSpaceWidth_;
	dSpaceHeight = dSpaceHeight_;
}

MultiJitteredSampling2D::~MultiJitteredSampling2D( )
{
}

void MultiJitteredSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	const unsigned int m = (unsigned int)sqrt( (double(numSamples) ) );
	const unsigned int n = m;

	const unsigned int numSamples = m*n;

	Scalar subcell_width = 1.0 / (numSamples);

	// Initialize samples to the canonical multi-jittered pattern
	samplePoints.resize( numSamples, Point2(0,0) );

	unsigned int i, j;
	
	for( i=0; i<m; i++ ) {
		for( j=0; j<n; j++ ) {
			samplePoints[i*n+j] = Point2( 
				i*n*subcell_width + j*subcell_width + random.CanonicalRandom()*subcell_width,
				j*m*subcell_width + i*subcell_width + random.CanonicalRandom()*subcell_width );
		}
	}

	// Shuffle the x co-ordinates within each column of cells
	for( i=0; i<m; i++ ) {
		for( j=0; j<n; j++ ) {
			const int k = random.RandomInt( j, n-1 );
			const Scalar t = samplePoints[i*n+j].x;
			samplePoints[i*n+j].x = samplePoints[i*n+k].x;
			samplePoints[i*n+k].x = t;
		}
	}

	// Shuffle the y co-ordinates within each row of cells
	for( i=0; i<n; i++ ) {
		for( j=0; j<m; j++ ) {
			const int k = random.RandomInt( j, m-1 );
			const Scalar t = samplePoints[j*n+i].y;
			samplePoints[j*n+i].y = samplePoints[k*n+i].y;
			samplePoints[k*n+i].y = t;
		}
	}

	// Make sure all the sample points are in the width and heigh intervals
	if( dSpaceWidth != 1.0 || dSpaceHeight != 1.0 ) {
		for( SamplesList2D::iterator b=samplePoints.begin(); b!=samplePoints.end(); b++ ) {
			b->x *= dSpaceWidth;
			b->y *= dSpaceHeight;
		}
	}
}

ISampling2D* MultiJitteredSampling2D::Clone( ) const
{
	MultiJitteredSampling2D*	pMe = new MultiJitteredSampling2D( dSpaceWidth, dSpaceHeight );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "Clone" );

	pMe->numSamples = numSamples;

	return pMe;
}
