//////////////////////////////////////////////////////////////////////
//
//  SobolSampling2D.cpp - Implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 27, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SobolSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

SobolSampling2D::SobolSampling2D(
	Scalar dSpaceWidth_,
	Scalar dSpaceHeight_
	)
{
	dSpaceWidth = dSpaceWidth_;
	dSpaceHeight = dSpaceHeight_;
}

SobolSampling2D::~SobolSampling2D()
{
}

void SobolSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear();

	// Derive a scramble seed from the RNG so that each pixel gets a
	// different permutation of the Sobol sequence.  We draw a single
	// random value and convert to uint32_t.
	const uint32_t seed = static_cast<uint32_t>( random.CanonicalRandom() * double(0xFFFFFFFFu) );

	for( unsigned int i = 0; i < numSamples; i++ )
	{
		// Sobol dim 0 for x, dim 1 for y, both Owen-scrambled
		const double x = SobolSequence::Sample( i, 0, seed );
		const double y = SobolSequence::Sample( i, 1, seed );

		samplePoints.push_back( Point2(
			Scalar(x) * dSpaceWidth,
			Scalar(y) * dSpaceHeight ) );
	}
}

ISampling2D* SobolSampling2D::Clone() const
{
	SobolSampling2D* pMe = new SobolSampling2D( dSpaceWidth, dSpaceHeight );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "clone" );

	pMe->numSamples = numSamples;
	return pMe;
}
