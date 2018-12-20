//////////////////////////////////////////////////////////////////////
//
//  NRooksSampling2D.cpp - Implementation
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
#include "NRooksSampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

NRooksSampling2D::NRooksSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dHowFar ) :
  dHowFar( _dHowFar )
{
	dSpaceWidth = _dSpaceWidth;
	dSpaceHeight = _dSpaceHeight;
}

NRooksSampling2D::~NRooksSampling2D( )
{
}

bool NRooksSampling2D::IsGoodPlace( const SampleElementList& samples, int iRow, int iCol ) const
{
	SampleElementList::const_iterator		i,e;

	for( i=samples.begin(), e=samples.end(); i!=e; i++ ) {
		const SAMPLE_ELEMENT& s = *i;

		if( s.iRow == iRow || s.iCol == iCol ) {
			return false;
		}
	}

	return true;
}

unsigned int NRooksSampling2D::FindAndSet( SampleRowValidList& v, int idx ) const
{
	int whereat = 0;
	SampleRowValidList::iterator i, e;
	for( i=v.begin(), e=v.end(); i!=e; i++ ) {
		if( (*i).valid ) {
			if( idx==whereat ) {
				(*i).valid = false;
				return (*i).col;
			} else {
				whereat++;
			}
		}
	}

	// Should never get here!
	GlobalLog()->PrintSourceError( "FindAndSet (NRooksSampling2D):: got to the part it should never get to", __FILE__, __LINE__ );
	return 0;	
}

void NRooksSampling2D::GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const
{
	samplePoints.clear( );

	// For each strata randomly sample independently in X and Y

	// We assume num_samples to be the number of samples in each axis
	Vector2	 vSegmentSize = Vector2( dSpaceWidth / Scalar(numSamples), 
									dSpaceHeight / Scalar(numSamples) );

	SampleElementList currSamples;
	SampleRowValidList total_cols;

	// Setup total_cols
	{
		for( unsigned int i=0; i<numSamples; i++ ) {
			total_cols.push_back( SAMPLE_ROW_VALID( i, true ) );
		}
	}
	
	// Start with the algorithm where we see if its a good place, this works
	// well when there are lots of free spaces..
	unsigned int i=0;
	for( ; i<numSamples/4; i++ )
	{
		// For each row, try to randomly place a sample until we are successful
		int		iCol;
		do {
			iCol = int( random.CanonicalRandom() * Scalar(numSamples) );
		} while( !IsGoodPlace( currSamples, i, iCol ) );

		// The current iCol should be good so we can place the sample here
		total_cols[iCol].valid = false;

		// Determine the bounds for this strata
		Point2	ptStrataStart = Point2( i*vSegmentSize.x, iCol*vSegmentSize.y );
		Point2	ptStrataCenter = Point2Ops::mkPoint2( ptStrataStart, (vSegmentSize*0.5) );

		Scalar		dRandX = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;
		Scalar		dRandY = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;

		Point2	v = Point2( 
			dRandX*vSegmentSize.x/2 + ptStrataCenter.x, 
			dRandY*vSegmentSize.y/2 + ptStrataCenter.y );

		SAMPLE_ELEMENT		elem;
		{
			elem.iCol = iCol;
			elem.iRow = i;
			elem.vSample = v;
		}
		currSamples.push_back( elem );
	}

	{
		SampleElementList::iterator		i,e;

		for( i=currSamples.begin(), e=currSamples.end(); i!=e; i++ ) {
			samplePoints.push_back( (*i).vSample );
		}
	}

	
	// Switch to the algorithm that finds a more suitable place, this works better
	// when more of the array is full
	for( ; i<numSamples; i++ )
	{
		// For each row, try to randomly place a sample until we are successful
		int		iCol;

		// Pick from what is left
		int fromList = int( random.CanonicalRandom() * Scalar(numSamples-i) );
		iCol = FindAndSet( total_cols, fromList );

		// The current iCol should be good so we can place the sample here

		// Determine the bounds for this strata
		Point2	ptStrataStart = Point2( i*vSegmentSize.x, iCol*vSegmentSize.y );
		Point2	ptStrataCenter = Point2Ops::mkPoint2(ptStrataStart, (vSegmentSize*0.5));

		Scalar		dRandX = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;
		Scalar		dRandY = random.CanonicalRandom() * dHowFar*2.0 - dHowFar;

		Point2	v = Point2( 
			dRandX*vSegmentSize.x/2 + ptStrataCenter.x, 
			dRandY*vSegmentSize.y/2 + ptStrataCenter.y );

		samplePoints.push_back( v );
	}
}

ISampling2D* NRooksSampling2D::Clone( ) const
{
	NRooksSampling2D*	pMe = new NRooksSampling2D( dSpaceWidth, dSpaceHeight, dHowFar );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "Clone" );

	pMe->numSamples = numSamples;

	return pMe;
}
