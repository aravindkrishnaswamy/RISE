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
#include <vector>

#ifdef NO_HALTON
#include "../Utilities/RandomNumbers.h"
#endif

namespace RISE
{
	/*
	//
	// A Halton point sequence
	//
	class Halton
	{
	public:
		Scalar value, inv_base;

		// Initializes the Halton point sequence at any number for any base
		void Number( long i, const int base )
		{
			Scalar f = inv_base = 1.0/base;
			value = 0.0;
			while ( i > 0 ) {
				value += f * Scalar(i % base);
				i /= base;
				f *= inv_base;
			}
		}

		// Computes the next point in the sequence
		void Next( )
		{
	#ifdef NO_HALTON
			value = GlobalRNG().CanonicalRandom();
	#else 
			const Scalar r = 1.0 - value - 0.0000000001;
			if (inv_base < r) {
				value += inv_base;
			} else {
				Scalar h = inv_base, hh;
				do {
					hh = h;
					h *= inv_base;
				} while ( h >= r );
				value += hh + h - 1.0;
			}
	#endif
		}

		// Returns current point in the sequence
		operator double() { return value; }
	};


	//
	// A multi dimensional Halton point sequence
	//
	class MultiHalton
	{
	private:
		std::vector<Halton> haltons;

	public:
		MultiHalton(
			const unsigned int dimensions
			)
		{
			std::vector<unsigned int> primes;
			Primes::GeneratePrimes( dimensions, primes );

			haltons.reserve( dimensions );
			for( unsigned int i=0; i<dimensions; i++ ) {
				Halton h;
				h.Number( i+3, primes[i] );
				haltons.push_back( h );
			}
		}

		double next( int dimension )
		{
			haltons[dimension].Next();
			return haltons[dimension];
		}
	};
	*/

	//
	// A new QMC class based off the QMC class in the SunFlow renderer
	//
	static const int QMC_NUM_PRIMES = 256;
	class MultiHalton
	{
	protected:
		// For next_halton()
		unsigned int current_num[QMC_NUM_PRIMES];

	public:
		std::vector<unsigned int> primes;
		unsigned int* SIGMA[QMC_NUM_PRIMES];

		// Search for and return the next prime number after p
		int NextPrime( int p ) 
		{
			p = p + (p & 1) + 1;

			while( true ) {
				int div = 3;
				bool bPrime = true;

				while( bPrime && ((div * div) <= p) ) {
					bPrime = ((p % div) != 0);
					div += 2;
				}

				if( bPrime ) {
					return p;
				}
				p += 2;
			}
		}

		MultiHalton()
		{
			// Build the tables
			// build table of primes
			Primes::GeneratePrimes( QMC_NUM_PRIMES, primes );
			
			unsigned int** table = new unsigned int*[primes[primes.size()-1] + 1];

			table[2] = new unsigned int[2];
			table[2][0] = 0;
			table[2][1] = 1;

			for( unsigned int i = 3; i <= primes[primes.size()-1]; i++ )
			{
				table[i] = new unsigned int[i];

				if ((i & 1) == 0) {
					unsigned int prev_length = i >> 1;
					unsigned int* prev = table[prev_length];

					for (unsigned int j = 0; j < prev_length; j++) {
						table[i][j] = 2 * prev[j];
					}

					for (unsigned int j = 0; j < prev_length; j++) {
						table[i][prev_length + j] = 2 * prev[j] + 1;
					}
				} else {
					unsigned int prev_length = i-1;
					unsigned int* prev = table[prev_length];
					unsigned int med = (i - 1) >> 1;

					for (unsigned int j = 0; j < med; j++) {
						table[i][j] = prev[j] + ((prev[j] >= med) ? 1 : 0);
					}

					table[i][med] = med;
					for (unsigned int j = 0; j < med; j++) {
						table[i][med + j + 1] = prev[j + med] + ((prev[j + med] >= med) ? 1 : 0);
					}
				}
			}

			// Copy into the SIGMA array
			for( int i = 0; i < QMC_NUM_PRIMES; i++ ) {
				int p = primes[i];
				SIGMA[i] = new unsigned int[p];
				memcpy(SIGMA[i],table[p], p*sizeof(unsigned int));
				delete [] table[p];
			}
			delete [] table;

			memset( current_num, 0, sizeof(unsigned int)*QMC_NUM_PRIMES );
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

			unsigned int base = primes[d];
			unsigned int* perm = SIGMA[d];
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

	// statically consruct so that there is every only one and it is immutable
	static const MultiHalton multihalton;
}

#endif


