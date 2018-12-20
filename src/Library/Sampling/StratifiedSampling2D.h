//////////////////////////////////////////////////////////////////////
//
//  StratifiedSampling2D.h - Generates stratified samples
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

#ifndef STRATIFIED_SAMPLING_2D_
#define STRATIFIED_SAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates jittered samples (random samples within a strata)
	//
	namespace Implementation
	{
		class StratifiedSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			Scalar		dHowFar;		// How far from the center of strata are we allowed
										// to generate samples to ?

			virtual ~StratifiedSampling2D( );

		public:
			StratifiedSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dHowFar );

			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
