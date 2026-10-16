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
	//! libc rand(), serialized (DL-625 review).  A default-constructed
	//! RandomNumberGenerator seeds from libc rand(), and render workers
	//! construct generators concurrently (thread_local generators in media
	//! and AOV passes; formerly per-call fallback samplers in the
	//! integrators, now FallbackSampler / ThreadLocalSeed): calls to rand()
	//! from several threads at once race on its hidden state (undefined
	//! behaviour).  The default seed and the render-time seeding sites read
	//! the libc stream through this one mutex, so it stays the stream
	//! std::srand controls while concurrent readers no longer race.  (Code
	//! outside the render path -- detector-sphere tools, AutoRasterizer's
	//! fixed-probe test hook, the Win32 UI -- still calls rand() / srand()
	//! directly.)  Which concurrent reader gets which value still depends on
	//! scheduling: sites whose result must be reproducible read once on the
	//! constructing thread and derive per-tile / per-row seeds
	//! (RasterizeBlockDispatcher, the VCM light pass, AOV passes).
	extern unsigned int LockedLibcRand();

	//! A cheap, race-free seed for a generator constructed per call on a
	//! render worker (fallback samplers in hot integrator code): a
	//! thread_local splitmix64 stream seeded ONCE per thread from
	//! LockedLibcRand().  No lock after the thread's first call, unlike the
	//! default constructor's LockedLibcRand() seed (a global mutex per
	//! construction cost +34 % user CPU on an SMS scene).  Like the racy
	//! rand() it replaces, the values a thread sees depend on scheduling,
	//! so results drawn from it are not reproducible run to run.
	extern unsigned int ThreadLocalSeed();

	//! A seed derived from (base, key) by the splitmix64 finalizer: distinct
	//! keys (tiles, rows, workers) of one base get decorrelated seeds, and
	//! the result is a pure function of its arguments.
	inline unsigned int DeriveSeed( const unsigned int base, const unsigned long long key )
	{
		unsigned long long z = ( static_cast<unsigned long long>( base ) << 32 ) ^ ( ( key + 1ULL ) * 0x9E3779B97F4A7C15ULL );
		z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
		z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
		z ^= z >> 31;
		return static_cast<unsigned int>( z ^ ( z >> 32 ) );
	}

	class RandomNumberGenerator
	{
	protected:
	#if ((defined MERSENNE53) || (defined MERSENNE))
		mutable MersenneTwister mt;
	#endif

	public:

	#if ((defined MERSENNE53) || (defined MERSENNE))
		RandomNumberGenerator( unsigned int seed = LockedLibcRand() ) : 
		mt( seed )
		{
		};
	#else
		RandomNumberGenerator( ){};
	#endif

		virtual ~RandomNumberGenerator( )
		{
		};

        // Reset the existing generator in place; retain its object identity.
        inline void Reseed( unsigned int seed )
        {
#if defined DRAND48
            srand48(seed);
#elif defined MERSENNE53 || defined MERSENNE
            mt.init_genrand(seed);
#else
            srand(seed);
#endif
        }

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
