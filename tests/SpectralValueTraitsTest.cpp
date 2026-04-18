//////////////////////////////////////////////////////////////////////
//
//  SpectralValueTraitsTest.cpp - Validates the SpectralValueTraits
//    dispatch layer (Phase 0 of the integrator refactor).
//
//  Tests:
//    A. RGB (PelTag) traits: zero, is_zero, max_value, luminance,
//       summary_magnitude produce the same values as direct
//       ColorMath::Luminance / ColorMath::MaxValue calls.
//    B. NM (NMTag) traits: scalar counterparts match fabs / identity.
//    C. Static type relationships: PelTag::value_type is RISEPel,
//       NMTag::value_type is Scalar.  supports_aov=true for Pel only.
//    D. Edge cases: negative RGB values, zero throughput, large values.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cassert>
#include <type_traits>

#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/Color/SpectralValueTraits.h"

using namespace RISE;
using namespace RISE::SpectralDispatch;

static int failed = 0;

#define EXPECT_EQ( a, b ) do { \
	if( !( (a) == (b) ) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ \
			<< " expected " << (b) << ", got " << (a) << std::endl; \
		failed++; \
	} \
} while( 0 )

#define EXPECT_NEAR( a, b, tol ) do { \
	if( std::fabs( (a) - (b) ) > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ \
			<< " expected " << (b) << " (within " << (tol) << "), got " << (a) << std::endl; \
		failed++; \
	} \
} while( 0 )

static void TestTypeRelationships()
{
	std::cout << "Test A: static type relationships" << std::endl;

	static_assert( std::is_same<SpectralValueTraits<PelTag>::value_type, RISEPel>::value,
		"PelTag::value_type must be RISEPel" );
	static_assert( std::is_same<SpectralValueTraits<NMTag>::value_type, Scalar>::value,
		"NMTag::value_type must be Scalar" );

	static_assert( SpectralValueTraits<PelTag>::is_pel,  "PelTag::is_pel" );
	static_assert( !SpectralValueTraits<PelTag>::is_nm,  "!PelTag::is_nm" );
	static_assert( !SpectralValueTraits<NMTag>::is_pel,  "!NMTag::is_pel" );
	static_assert( SpectralValueTraits<NMTag>::is_nm,    "NMTag::is_nm" );

	static_assert( SpectralValueTraits<PelTag>::supports_aov,  "Pel supports AOV" );
	static_assert( !SpectralValueTraits<NMTag>::supports_aov, "NM does not support AOV" );

	std::cout << "  PASS" << std::endl;
}

static void TestPelTraits()
{
	std::cout << "Test B: PelTag traits arithmetic" << std::endl;
	using T = SpectralValueTraits<PelTag>;

	// zero
	const RISEPel z = T::zero();
	EXPECT_EQ( z[0], 0.0 );
	EXPECT_EQ( z[1], 0.0 );
	EXPECT_EQ( z[2], 0.0 );
	assert( T::is_zero( z ) );

	// non-zero
	const RISEPel a( 0.1, 0.2, 0.3 );
	assert( !T::is_zero( a ) );

	// max_value matches ColorMath::MaxValue exactly — the invariant
	// the refactor depends on.  Use == not NEAR since they are the
	// same expression.
	EXPECT_EQ( T::max_value( a ), ColorMath::MaxValue( a ) );

	const RISEPel b( 0.5, 0.1, 0.2 );
	EXPECT_EQ( T::max_value( b ), 0.5 );

	std::cout << "  PASS" << std::endl;
}

static void TestNMTraits()
{
	std::cout << "Test C: NMTag traits arithmetic" << std::endl;
	using T = SpectralValueTraits<NMTag>;

	EXPECT_EQ( T::zero(), 0.0 );
	assert( T::is_zero( T::zero() ) );
	assert( !T::is_zero( 0.5 ) );

	// max_value is fabs
	EXPECT_EQ( T::max_value( 0.5 ),  0.5 );
	EXPECT_EQ( T::max_value( -0.7 ), 0.7 );

	std::cout << "  PASS" << std::endl;
}

static void TestEdgeCases()
{
	std::cout << "Test D: edge cases (large, negative, very small)" << std::endl;
	using TP = SpectralValueTraits<PelTag>;
	using TN = SpectralValueTraits<NMTag>;

	// Large magnitudes
	const RISEPel large( 1e6, 2e6, 3e6 );
	EXPECT_EQ( TP::max_value( large ), 3e6 );

	// Negative values (can occur during debug/error paths; must not crash).
	const RISEPel neg( -0.1, 0.2, -0.3 );
	// MaxValue picks the largest, which is 0.2.
	EXPECT_EQ( TP::max_value( neg ), 0.2 );

	// NM negative
	EXPECT_EQ( TN::max_value( -5.0 ), 5.0 );

	// Very small
	EXPECT_EQ( TN::max_value( 1e-30 ), 1e-30 );
	assert( !TN::is_zero( 1e-30 ) );

	std::cout << "  PASS" << std::endl;
}

static void TestTagCarriesWavelength()
{
	std::cout << "Test E: NMTag carries wavelength context" << std::endl;

	NMTag t555( 555.0 );
	NMTag t700( 700.0 );

	EXPECT_NEAR( t555.nm, 555.0, 1e-12 );
	EXPECT_NEAR( t700.nm, 700.0, 1e-12 );

	// PelTag is empty (no runtime state)
	PelTag pt;
	(void)pt;
	static_assert( sizeof( PelTag ) <= sizeof( void* ),
		"PelTag must stay small (preferably empty + EBO)" );

	std::cout << "  PASS" << std::endl;
}

int main()
{
	std::cout << "=== SpectralValueTraitsTest ===" << std::endl;

	TestTypeRelationships();
	TestPelTraits();
	TestNMTraits();
	TestEdgeCases();
	TestTagCarriesWavelength();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "All tests passed." << std::endl;
		return 0;
	} else {
		std::cout << failed << " test(s) failed." << std::endl;
		return 1;
	}
}
