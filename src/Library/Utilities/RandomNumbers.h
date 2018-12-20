//////////////////////////////////////////////////////////////////////
//
//  RandomNumbers.h - Utilities for generating random numbers and such
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 8, 2002
//  Tabs: 4
//  Comments:  Influence by ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RANDOM_NUMBERS_
#define RANDOM_NUMBERS_

#include <stdlib.h>
#include "MersenneTwister.h"

namespace RISE
{
	class RandomNumberGenerator
	{
	protected:
	#if ((defined MERSENNE53) || (defined MERSENNE))
		mutable MersenneTwister mt;
	#endif

	public:

	#if ((defined MERSENNE53) || (defined MERSENNE))
		RandomNumberGenerator( unsigned int seed = rand() ) : 
		mt( seed )
		{
		};
	#else
		RandomNumberGenerator( ){};
	#endif

		virtual ~RandomNumberGenerator( )
		{
		};

		inline double CanonicalRandom() const
		{
			#if defined DRAND48
				return drand48();
			#elif defined MERSENNE53
				return mt.genrand_res53();
			#elif defined MERSENNE
				return mt.genrand_real2();
			#else
				return rand() * (0.9999999 / double(RAND_MAX));  
			#endif
		}

		inline double RandomScalar( const double l, const double h ) const 
		{
			return (CanonicalRandom()*(h-l)+l);
		}

		inline int RandomInt( const int l, const int h ) const 
		{
			return int( RandomScalar(0, h-l+1) + l);
		}

		inline unsigned int RandomUInt( const unsigned int l, const unsigned int h ) const
		{
			return (unsigned int)( RandomScalar(0, h-l+1) + l);
		}
	};

	extern RandomNumberGenerator& GlobalRNG();
}

#endif
