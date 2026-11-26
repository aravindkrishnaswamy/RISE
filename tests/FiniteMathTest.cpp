//////////////////////////////////////////////////////////////////////
//
//  FiniteMathTest.cpp - Regression coverage for the FiniteMath.h
//    helpers (IsFiniteDouble, IsPositiveInfinityDouble) under the
//    production -ffast-math build.
//
//////////////////////////////////////////////////////////////////////

#include <cstdlib>
#include <iostream>

#include "../src/Library/Utilities/FiniteMath.h"

namespace
{
	int gPassed = 0;
	int gFailed = 0;

	void Check( const bool condition, const char* const label )
	{
		if( condition ) {
			++gPassed;
		} else {
			++gFailed;
			std::cerr << "FAIL: " << label << std::endl;
		}
	}

	#if defined(__GNUC__) || defined(__clang__)
	__attribute__((noinline))
	#endif
	bool CheckAtValueBoundary( const double value )
	{
		return RISE::IsFiniteDouble( value );
	}

	#if defined(__GNUC__) || defined(__clang__)
	__attribute__((noinline))
	#endif
	bool CheckPosInfAtValueBoundary( const double value )
	{
		return RISE::IsPositiveInfinityDouble( value );
	}
}

int main()
{
	std::cout << "=== FiniteMathTest ===" << std::endl;

	// strtod is a runtime library call: unlike a NaN/infinity literal, these
	// values cannot be erased by the test translation unit's -ffast-math.
	const double nan  = std::strtod( "nan", nullptr );
	const double pinf = std::strtod( "1e999", nullptr );
	const double ninf = std::strtod( "-1e999", nullptr );

	Check( CheckAtValueBoundary( 0.0 ), "zero is finite" );
	Check( CheckAtValueBoundary( -3.5 ), "negative finite value is finite" );
	Check( CheckAtValueBoundary( 1.0e300 ), "large finite value is finite" );
	Check( !CheckAtValueBoundary( nan ), "runtime NaN is rejected" );
	Check( !CheckAtValueBoundary( pinf ), "runtime +infinity is rejected" );
	Check( !CheckAtValueBoundary( ninf ), "runtime -infinity is rejected" );

	Check( CheckPosInfAtValueBoundary( pinf ), "runtime +infinity is recognised as +inf" );
	Check( !CheckPosInfAtValueBoundary( ninf ), "runtime -infinity is not +inf" );
	Check( !CheckPosInfAtValueBoundary( nan ), "runtime NaN is not +inf" );
	Check( !CheckPosInfAtValueBoundary( 1.0e300 ), "large finite value is not +inf" );

	std::cout << gPassed << "/" << ( gPassed + gFailed ) << " checks passed" << std::endl;
	return gFailed == 0 ? 0 : 1;
}
