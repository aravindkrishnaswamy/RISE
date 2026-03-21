//////////////////////////////////////////////////////////////////////
//
//  IndependentSampler.h - Independent sampler implementation that
//    wraps a RandomNumberGenerator to produce i.i.d. uniform samples.
//    StartStream is a no-op; future MLT samplers will override it
//    to support primary sample space mutations.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef INDEPENDENT_SAMPLER_
#define INDEPENDENT_SAMPLER_

#include "ISampler.h"
#include "Reference.h"
#include "RandomNumbers.h"

namespace RISE
{
	namespace Implementation
	{
		class IndependentSampler :
			public virtual ISampler,
			public virtual Reference
		{
		protected:
			const RandomNumberGenerator& random;

			virtual ~IndependentSampler(){};

		public:
			IndependentSampler( const RandomNumberGenerator& random_ ) :
				random( random_ )
			{
			};

			//! Returns a single uniform random sample in [0,1)
			Scalar Get1D()
			{
				return random.CanonicalRandom();
			}

			//! Returns a 2D uniform random sample in [0,1)^2
			Point2 Get2D()
			{
				return Point2( random.CanonicalRandom(), random.CanonicalRandom() );
			}

			//! No-op for independent sampling
			void StartStream( int /*streamIndex*/ )
			{
			}
		};
	}
}

#endif
