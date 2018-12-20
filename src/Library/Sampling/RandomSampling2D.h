//////////////////////////////////////////////////////////////////////
//
//  RandomSampling2D.h - Generates purely random samples
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

#ifndef RANDOM_SAMPLING_2D_
#define RANDOM_SAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates completely random independent samples
	//
	namespace Implementation
	{
		class RandomSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			virtual ~RandomSampling2D( );

		public:
			RandomSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight );

			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;

			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
