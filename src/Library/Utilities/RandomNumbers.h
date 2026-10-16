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

	//! A cheap, race-free seed for a generator constructed on a render
	//! worker (per-call fallback samplers in hot integrator code, the
	//! media's thread_local generators): a thread_local splitmix64 stream
	//! started from a process-wide atomic counter.  It never reads libc --
	//! the number of workers that make a first call is scheduling-
	//! dependent, so a libc read here would move the libc stream the
	//! reproducible seeding sites read next -- and takes no lock (the
	//! default constructor's LockedLibcRand() seed, a global mutex per
	//! construction, cost +34 % user CPU on an SMS scene).  Like the racy
	//! rand() it replaces, which counter value a thread gets depends on
	//! scheduling, so results drawn from it are not reproducible run to
	//! run.
	extern unsigned int ThreadLocalSeed();

	//! Restarts ThreadLocalSeed's per-thread tickets from a base read ONCE
	//! from libc on the calling thread (so std::srand pins it).  A thread
	//! whose FIRST ThreadLocalSeed() call -- e.g. its first heterogeneous-
	//! medium transmittance estimate -- comes after this call, as the
	//! k-th such thread, gets a stream that is a pure function of
	//! (std::srand state at the call, k).  Threads already seeded keep
	//! their streams.  Production never calls it (a libc read there would
	//! move the stream the render's own seeding sites read); a test that
	//! drives a walk on one fresh thread after std::srand calls it to make
	//! that walk's medium generator a property of the seed.  TEST-ONLY.
	//! Call it only while no other thread can make its first
	//! ThreadLocalSeed() call: the base and the ticket counter are separate
	//! atomics, so a concurrent first draw could pair an old base with a
	//! reset ticket and duplicate another thread's stream.
	extern void ReseedThreadLocalSeedEpochForTesting();

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
				// Fallback build only (neither MERSENNE53 nor MERSENNE; the
				// shipped Config.common defines MERSENNE53): this draws from
				// the shared libc stream, serialized but NOT per generator,
				// so draws interleave across threads.
				return LockedLibcRand() * (0.9999999 / double(RAND_MAX));  
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
