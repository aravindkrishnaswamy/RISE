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
	//! HISTORY (updated 2026-07-29): this helper exists because RISE used to
	//! build with bare -ffast-math, whose implied -ffinite-math-only permits
	//! the optimiser to assume a double operand is finite.  Under that
	//! assumption std::isfinite(), range comparisons, and even a direct
	//! memcpy exponent test were folded incorrectly.  Materialising through
	//! volatile severs that assumption; the exponent test is integer-only.
	//!
	//! Every macOS configuration now also passes -fno-finite-math-only, so
	//! plain std::isfinite() WORKS.  Do NOT read this comment as a claim that
	//! it would fold today -- it would not.  This helper is retained as the
	//! single hardened predicate because it keeps the ~73 call sites uniform
	//! and it survives a future -Ofast (which re-implies -ffast-math and thus
	//! -ffinite-math-only) anywhere in the build.
	//!
	//! It is NOT free: measured at 0.38-0.40 ns/call versus 0.01 ns for
	//! std::isfinite (~35-39x), because the volatile barrier forces a stack
	//! round-trip and blocks vectorisation of the enclosing loop.  That is
	//! immaterial at current call-site density, but a per-sample site in a
	//! genuinely hot loop should now prefer plain std::isfinite.
	//! See docs/INTEGRATOR_BUGFIX_FINDINGS.md "SUPERSEDED 2026-07-29".
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
}

#endif
