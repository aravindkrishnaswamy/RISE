//////////////////////////////////////////////////////////////////////
//
//  ProbabilityDensityFunction.cpp - Implements the
//  ProbabilityDensityFunction class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 15, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ProbabilityDensityFunction.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

ProbabilityDensityFunction::ProbabilityDensityFunction()
{
}

ProbabilityDensityFunction::ProbabilityDensityFunction( const IFunction1D* pFunc, const unsigned int numsteps )
{
	// Build from the function
	Scalar	accrued=0;
	for( unsigned int i=0; i<numsteps; i++ )
	{
		ProbPair prob;
		prob.first = Scalar(i)*(1.0/Scalar(numsteps) );
		prob.second = pFunc->Evaluate( prob.first );
		accrued += prob.second;
		values.push_back( prob );
	}

	Normalize( accrued );
	BuildLUT();
}

ProbabilityDensityFunction::ProbabilityDensityFunction( const SpectralPacket& func )
{
	// Build from the robust spectral packet
	Scalar	accrued=0;
	for( Scalar i=func.minLambda(), e=func.maxLambda(); i<e; i+=func.deltaLambda() )
	{
		// We are only interested in the visible spectrum
		if( i>= 380 && i<= 720 )
		{
			ProbPair prob;
			prob.first = i;
			prob.second = func.ValueAtNM( prob.first );
			accrued += prob.second;
			values.push_back( prob );
		}
	}

	if( func.minLambda() == 400.0 && func.maxLambda() == 700.0 && func.deltaLambda() == 300.0 ) {
		// Dummy spectral packet...
		// Don't really need a LUT, so build a dummy one
		for( int i=0; i<1024; i++ ) {
			LUTwarped[i] = 1.0;
			LUTweight[i] = 1.0;
		}
	} else {
		Normalize( accrued );
		BuildLUT();
	}
}

ProbabilityDensityFunction::~ProbabilityDensityFunction()
{
}

void ProbabilityDensityFunction::Normalize( const Scalar total )
{
	int size = values.size();

	if( size > 0 && total > 0 ) {
		DensityList::iterator i, e;
		for( i=values.begin(), e=values.end(); i!=e; i++ ) {
			(*i).second /= total;
		}

		meanProbability = 1.0 / Scalar( size );
	} else {
		GlobalLog()->PrintSourceWarning( "ProbabilityDensityFunction::Normalize:: size <= 0 or total <= 0", __FILE__, __LINE__ );
	}
}

void ProbabilityDensityFunction::Normalize( )
{
	// Find the total

	if( values.size() > 0 ) {
		DensityList::iterator i, e;
		Scalar	total=0;
		for( i=values.begin(), e=values.end(); i!=e; i++ ) {
			total += (*i).second;
		}

		Normalize( total );
	} else {
		GlobalLog()->PrintSourceWarning( "ProbabilityDensityFunction::Normalize:: values.size <= 0", __FILE__, __LINE__ );
	}
}

Scalar ProbabilityDensityFunction::warp( const Scalar canonical, Scalar& weight, bool bUseLUT ) const
{
	if( bUseLUT ) {
		unsigned int idx = (unsigned int)(canonical*1023.0);
		weight = LUTweight[ idx ];
		return LUTwarped[ idx ];
	}

	// Iterate through the values
	DensityList::const_iterator i, e;
	Scalar CDF = 0;
	for( i=values.begin(), e=values.end(); i!=e; i++ )
	{
		const ProbPair& thisprob = *i;

		if( canonical >= (CDF) && canonical <= (CDF+thisprob.second) && thisprob.second!=0 ) {			
			weight = meanProbability / thisprob.second;
			return thisprob.first;
		}

		CDF += thisprob.second;
	}

	GlobalLog()->PrintSourceWarning( "ProbabilityDensityFunction::warp none of the value points matched, returning alst", __FILE__, __LINE__ );
	return (*(e-1)).first;
}

Scalar ProbabilityDensityFunction::Evaluate( const Scalar variable ) const
{
	Scalar dummy;
	return warp( variable, dummy );
}

void ProbabilityDensityFunction::BuildLUT( )
{
	// Build a look up table
	// that warps from canonical random
	for( int i=0; i<1024; i++ ) {
		Scalar weight;
		LUTwarped[i] = warp( Scalar(i)/1024.0, weight, false );
		LUTweight[i] = weight;
	}
}
