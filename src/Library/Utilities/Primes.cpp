//////////////////////////////////////////////////////////////////////
//
//  Primes.cpp- Implementation of primes utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Primes.h"

// Generates a series of primes up to n and sticks in the given vector
// n Must be >= 2!
void RISE::Primes::GeneratePrimes( unsigned int n, std::vector<unsigned int>& primes_array )
{
	primes_array.reserve( n );

	int prev_index, next_index = 2; 
	bool is_prime = true;

	primes_array.push_back( 2 );	// first prime number is defined
	primes_array.push_back( 3 );	// second prime number is defined

	for( unsigned int i=2, possibly_prime = 5; i < n; possibly_prime+=2 )
	{
		is_prime = true;

		for( prev_index = 1; 
			(is_prime && (possibly_prime/primes_array[prev_index] >= primes_array[prev_index]));
			prev_index++ )
		{
			// If the number divides evenly, then it cannot be a prime.
			if( possibly_prime % primes_array[prev_index] == 0 ) {
				is_prime = false;
			}
		}

		if( is_prime ) { 
			primes_array.push_back( possibly_prime );
			next_index++;
			i++;
		}
	}
}

