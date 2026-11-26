//////////////////////////////////////////////////////////////////////
//
//  HaltonPoints.h - Creates a series of halton points
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HALTONPOINTS_H
#define HALTONPOINTS_H

#include "../Utilities/Primes.h"
#include "../Utilities/Math3D/Math3D.h"
#include <array>
#include <vector>

namespace RISE
{
	// Generalized Halton permutations based on the SunFlow QMC sampler.
	static constexpr unsigned int QMC_NUM_PRIMES = 256;
	class MultiHalton
	{
		struct Permutations
		{
			std::vector<unsigned int> primes;
			std::array<std::vector<unsigned int>, QMC_NUM_PRIMES> rows;

			Permutations()
			{
				Primes::GeneratePrimes( QMC_NUM_PRIMES, primes );
				std::vector<std::vector<unsigned int>> table( primes.back() + 1 );
				table[2] = { 0, 1 };
				for( unsigned int base = 3; base <= primes.back(); ++base ) {
					auto& row = table[base];
					row.resize( base );
					if( base % 2 == 0 ) {
						const auto& previous = table[base/2];
						for( unsigned int j = 0; j < base/2; ++j ) {
							row[j] = 2 * previous[j];
							row[base/2+j] = 2 * previous[j] + 1;
						}
					} else {
						const auto& previous = table[base-1];
						const unsigned int middle = base/2;
						row[middle] = middle;
						for( unsigned int j = 0; j < middle; ++j ) {
							row[j] = previous[j] + (previous[j] >= middle);
							row[middle+j+1] = previous[middle+j] + (previous[middle+j] >= middle);
						}
					}
				}

				for( unsigned int d = 0; d < QMC_NUM_PRIMES; ++d ) {
					rows[d].swap( table[primes[d]] );
				}
			}
		};

		static const Permutations& SharedPermutations()
		{
			static const Permutations permutations;
			return permutations;
		}

	protected:
		std::array<unsigned int, QMC_NUM_PRIMES> current_num{};

	public:
		void Reset()
		{
			current_num.fill( 0 );
		}

		double halton( unsigned int d, unsigned int i ) const
		{
			// generalized Halton sequence
			switch (d) {
				case 0: {
					i = (i << 16) | (i >> 16);
					i = ((i & 0x00ff00ff) << 8) | ((i & 0xff00ff00) >> 8);
					i = ((i & 0x0f0f0f0f) << 4) | ((i & 0xf0f0f0f0) >> 4);
					i = ((i & 0x33333333) << 2) | ((i & 0xcccccccc) >> 2);
					i = ((i & 0x55555555) << 1) | ((i & 0xaaaaaaaa) >> 1);
					return Scalar(i & 0xffffffffL) / Scalar(0x10000000L);
				} break;

				case 1: {
					double v = 0;
					const double inv = 1.0 / 3;
					double p;
					int n;
					for( p = inv, n = i; n != 0; p *= inv, n /= 3 ) {
						v += (n % 3) * p;
					}
					return v;
				} break;

				default:
					break;
			}

			const auto& permutations = SharedPermutations();
			const unsigned int base = permutations.primes[d];
			const auto& perm = permutations.rows[d];
			double v = 0;
			double inv = 1.0 / base;
			double p;
			int n;

			for( p=inv, n=i; n!=0; p*=inv, n/=base ) {
				v += perm[n % base] * p;
			}

			return v;
		}

		double next_halton( unsigned int d )
		{
			return mod1( halton(d, current_num[d]++ ) );
		}

		static double mod1( double x )
		{
			// assumes x >= 0
			return x - int(x);
		}
	};

}

#endif


