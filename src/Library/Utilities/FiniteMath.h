//////////////////////////////////////////////////////////////////////
//
//  FiniteMath.h - Optimisation-safe IEEE-754 finiteness checks.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FINITE_MATH_H
#define FINITE_MATH_H

#include <cstdint>
#include <cstring>

namespace RISE
{
	//! Returns true iff @a value is neither NaN nor +/-infinity.
	//!
	//! RISE is built with -ffast-math, which permits the optimiser to assume
	//! a double operand is finite.  Under that assumption std::isfinite(),
	//! range comparisons, and even a direct memcpy exponent test can be folded
	//! incorrectly.  Materialising through volatile first severs that
	//! assumption; the exponent test after the load is integer-only.
	inline bool IsFiniteDouble( const double value )
	{
		static_assert( sizeof( double ) == sizeof( std::uint64_t ),
			"IsFiniteDouble requires an IEEE-754 binary64 double" );
		volatile double barrier = value;
		const double materialised = barrier;
		std::uint64_t bits = 0;
		std::memcpy( &bits, &materialised, sizeof( bits ) );
		return ( ( bits >> 52 ) & 0x7FFULL ) != 0x7FFULL;
	}

	//! Returns true iff @a value is exactly +infinity.  Same volatile
	//! materialisation rationale as IsFiniteDouble — the volatile
	//! round-trip is what severs the optimiser's finiteness assumption;
	//! the subsequent comparison is integer-only against the unique
	//! +inf bit pattern.  Lets callers that already know a value is
	//! non-finite distinguish +inf (often "saturate high") from NaN
	//! and -inf without an FP comparison on the non-finite operand.
	inline bool IsPositiveInfinityDouble( const double value )
	{
		static_assert( sizeof( double ) == sizeof( std::uint64_t ),
			"IsPositiveInfinityDouble requires an IEEE-754 binary64 double" );
		volatile double barrier = value;
		const double materialised = barrier;
		std::uint64_t bits = 0;
		std::memcpy( &bits, &materialised, sizeof( bits ) );
		return bits == 0x7FF0000000000000ULL;
	}

	//! Returns true iff @a value is either signed representation of zero.
	//! The volatile materialisation is required for the same -ffast-math
	//! reason as IsFiniteDouble: a floating comparison may assume that a
	//! positive product cannot underflow to zero.
	inline bool IsZeroDouble( const double value )
	{
		static_assert( sizeof( double ) == sizeof( std::uint64_t ),
			"IsZeroDouble requires an IEEE-754 binary64 double" );
		volatile double barrier = value;
		const double materialised = barrier;
		std::uint64_t bits = 0;
		std::memcpy( &bits, &materialised, sizeof( bits ) );
		return (bits & 0x7FFFFFFFFFFFFFFFULL) == 0;
	}
}

#endif
