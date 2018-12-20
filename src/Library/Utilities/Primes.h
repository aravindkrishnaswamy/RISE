//////////////////////////////////////////////////////////////////////
//
//  Primes.h - Utilities for generatnig prime number sequences
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PRIMES_H
#define PRIMES_H

#include <vector>

namespace RISE
{
	namespace Primes
	{
		// Generates a series of primes up to n and sticks in the given vector
		// n Must be >= 2!
		extern void GeneratePrimes( unsigned int n, std::vector<unsigned int>& primes_array );
	}
}

#endif
