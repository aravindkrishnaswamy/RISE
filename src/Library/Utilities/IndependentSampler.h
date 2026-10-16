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
#include "RandomNumbers.h"
#include <new>

namespace RISE
{
	namespace Implementation
	{
		class IndependentSampler :
			public ISampler
		{
		protected:
			const RandomNumberGenerator& random;

		public:
			virtual ~IndependentSampler(){};

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

		//! `preferred` when it is non-null, otherwise an IndependentSampler
		//! over a generator of its own -- and that generator is constructed
		//! ONLY in the fallback case (DL-625 review).  Integrator entry
		//! points used to default-construct a RandomNumberGenerator on every
		//! call just in case the caller passed no sampler: a Mersenne
		//! Twister initialization plus a read of the (serialized) libc
		//! stream per connection, on every render worker.
		class FallbackSampler
		{
			alignas( RandomNumberGenerator ) unsigned char rngStorage[sizeof( RandomNumberGenerator )];
			alignas( IndependentSampler ) unsigned char samplerStorage[sizeof( IndependentSampler )];
			ISampler* p;
			bool owns;

			FallbackSampler( const FallbackSampler& );
			FallbackSampler& operator=( const FallbackSampler& );

		public:
			explicit FallbackSampler( ISampler* preferred ) :
				p( preferred ),
				owns( preferred == 0 )
			{
				if( owns ) {
					RandomNumberGenerator* rng = new( rngStorage ) RandomNumberGenerator( ThreadLocalSeed() );
					p = new( samplerStorage ) IndependentSampler( *rng );
				}
			}

			~FallbackSampler()
			{
				if( owns ) {
					reinterpret_cast<IndependentSampler*>( samplerStorage )->~IndependentSampler();
					reinterpret_cast<RandomNumberGenerator*>( rngStorage )->~RandomNumberGenerator();
				}
			}

			ISampler& Get() { return *p; }
		};
	}
}

#endif
