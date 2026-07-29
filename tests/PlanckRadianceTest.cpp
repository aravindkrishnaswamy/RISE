//////////////////////////////////////////////////////////////////////
//
//  PlanckRadianceTest.cpp - Numeric gates for the absolute Planck kernel
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/PlanckRadiance.h"

using namespace RISE;

namespace
{
	int failures = 0;

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			++failures;
			std::cerr << "FAIL: " << message << std::endl;
		}
	}

	bool IsFiniteBits( const Scalar value )
	{
		static_assert( sizeof(Scalar) == sizeof(std::uint64_t),
			"Planck gates require binary64 Scalar" );
		std::uint64_t bits = 0;
		std::memcpy( &bits, &value, sizeof(bits) );
		return ( bits & UINT64_C(0x7ff0000000000000) ) !=
			UINT64_C(0x7ff0000000000000);
	}

	bool IsRelativeClose( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		return IsFiniteBits( actual ) && IsFiniteBits( expected ) &&
			std::fabs( actual - expected ) <= tolerance * std::fabs( expected );
	}

	// Give the legacy one-past-end lookup a deterministic nonzero guard value.
	// The production packet still sees exactly num_freq valid bins; the extra
	// allocation exists only so this boundary regression is observable without
	// relying on allocator contents.
	class GuardedSpectralPacket : public SpectralPacket
	{
	public:
		GuardedSpectralPacket() : SpectralPacket( 400.0, 700.0, 3 )
		{
			GlobalLog()->PrintDelete( amplitudes, __FILE__, __LINE__ );
			delete [] amplitudes;
			amplitudes = new Scalar[num_freq + 1];
			GlobalLog()->PrintNew( amplitudes, __FILE__, __LINE__, "guarded amplitudes" );
			for( unsigned int i=0; i<num_freq; ++i ) {
				amplitudes[i] = Scalar(i + 1);
			}
			amplitudes[num_freq] = 12345.0;
		}
	};

	Scalar IntegratePlanckOverWavelengthNM( const Scalar temperatureKelvin )
	{
		// Composite Simpson integration in log(lambda). This covers the
		// effectively infinite wavelength domain without concentrating samples
		// in the long Rayleigh-Jeans tail. Since lambda=exp(u), d lambda=lambda du.
		static const Scalar lowerNM = 1e-3;
		static const Scalar upperNM = 1e8;
		static const unsigned int intervals = 2000;
		const Scalar lowerU = std::log( lowerNM );
		const Scalar upperU = std::log( upperNM );
		const Scalar deltaU = ( upperU - lowerU ) / intervals;

		auto integrand = [temperatureKelvin]( const Scalar u ) {
			const Scalar wavelengthNM = std::exp( u );
			return PlanckSpectralRadianceNM( wavelengthNM, temperatureKelvin ) * wavelengthNM;
		};

		Scalar sum = integrand( lowerU ) + integrand( upperU );
		for( unsigned int i=1; i<intervals; ++i ) {
			sum += ( i % 2 == 0 ? 2.0 : 4.0 ) * integrand( lowerU + i * deltaU );
		}
		return sum * deltaU / 3.0;
	}
}

static void TestPinnedPointAnchor()
{
	const Scalar actual = PlanckSpectralRadianceNM( 500.0, 1800.0 );
	const Scalar expected = 0.43477836;
	std::cout << "B(500 nm, 1800 K) = " << actual << std::endl;
	Check( IsRelativeClose( actual, expected, 1e-8 ),
		"B(500 nm, 1800 K) matches the pinned per-nm radiance anchor" );
}

static void TestStefanBoltzmannIntegralIdentity()
{
	static const Scalar stefanBoltzmann = 5.670374419e-8;
	static const Scalar temperatureKelvin = 1800.0;
	const Scalar integratedExitance = PI * IntegratePlanckOverWavelengthNM( temperatureKelvin );
	const Scalar expected = stefanBoltzmann * std::pow( temperatureKelvin, 4.0 );
	std::cout << "pi integral(B dlambda) = " << integratedExitance
		<< ", sigma T^4 = " << expected << std::endl;
	Check( IsRelativeClose( integratedExitance, expected, 1e-8 ),
		"pi integral(B dlambda) matches sigma T^4" );
}

static void TestNonPositivePhysicalInputs()
{
	Check( PlanckSpectralRadianceNM( 0.0, 1800.0 ) == 0.0,
		"zero wavelength returns zero" );
	Check( PlanckSpectralRadianceNM( -500.0, 1800.0 ) == 0.0,
		"negative wavelength returns zero" );
	Check( PlanckSpectralRadianceNM( 500.0, 0.0 ) == 0.0,
		"zero temperature returns zero" );
	Check( PlanckSpectralRadianceNM( 500.0, -1800.0 ) == 0.0,
		"negative temperature returns zero" );
}

static void TestNonFiniteComparisonCannotPass()
{
	std::uint64_t nanBits = UINT64_C(0x7ff8000000000000);
	Scalar quietNaN = 0.0;
	std::memcpy( &quietNaN, &nanBits, sizeof(quietNaN) );
	Check( !IsRelativeClose( quietNaN, 1.0, 1e-8 ),
		"numeric gate rejects NaN under fast-math" );
}

static RayIntersectionGeometric MakeDummyIntersection()
{
	return RayIntersectionGeometric( Ray(), nullRasterizerState );
}

static void TestBlackBodyPainterBoundaryAndNormalization()
{
	const RayIntersectionGeometric ri = MakeDummyIntersection();

	IPainter* absolutePainter = 0;
	Check( RISE_API_CreateBlackBodyPainter(
		&absolutePainter, 1800.0, 400.0, 700.0, 3, false, 2.0 ) && absolutePainter,
		"absolute blackbody painter is created" );
	if( absolutePainter ) {
		const Scalar expected = 2.0 * PI * 1e9 *
			PlanckSpectralRadianceNM( 500.0, 1800.0 );
		Check( IsRelativeClose( absolutePainter->GetColorNM( ri, 500.0 ), expected, 1e-12 ),
			"blackbody painter converts shared per-nm radiance to per-metre exitance" );
		const SpectralPacket spectrum = absolutePainter->GetSpectrum( ri );
		Check( IsRelativeClose( spectrum.ValueAtNM( 500.0 ), expected, 1e-12 ),
			"blackbody packet uses the same shared-kernel boundary conversion" );
		absolutePainter->release();
	}

	static const Scalar wienDisplacementNMKelvin = 2.897771955e6;
	const Scalar temperature = wienDisplacementNMKelvin / 500.0;
	IPainter* normalizedPainter = 0;
	Check( RISE_API_CreateBlackBodyPainter(
		&normalizedPainter, temperature, 400.0, 700.0, 3, true, 2.0 ) && normalizedPainter,
		"normalized blackbody painter is created" );
	if( normalizedPainter ) {
		const Scalar expectedPeak = 2.0;
		Check( IsRelativeClose( normalizedPainter->GetColorNM( ri, 500.0 ), expectedPeak, 1e-12 ),
			"normalized GetColorNM preserves the authored peak scale" );
		SpectralPacket spectrum = normalizedPainter->GetSpectrum( ri );
		Check( IsRelativeClose( spectrum.ValueAtNM( 500.0 ), expectedPeak, 1e-12 ),
			"normalized GetSpectrum agrees with GetColorNM at a packet bin" );

		normalizedPainter->RegenerateData();
		normalizedPainter->RegenerateData();
		Check( IsRelativeClose( normalizedPainter->GetColorNM( ri, 500.0 ), expectedPeak, 1e-12 ),
			"repeated regeneration does not attenuate normalized GetColorNM" );
		spectrum = normalizedPainter->GetSpectrum( ri );
		Check( IsRelativeClose( spectrum.ValueAtNM( 500.0 ), expectedPeak, 1e-12 ),
			"repeated regeneration does not attenuate normalized GetSpectrum" );
		normalizedPainter->release();
	}

	const GuardedSpectralPacket guardedSpectrum;
	Check( guardedSpectrum.ValueAtNM( 700.0 ) == 0.0,
		"spectral packet upper endpoint is outside its stored bins" );
}

int main()
{
	TestPinnedPointAnchor();
	TestStefanBoltzmannIntegralIdentity();
	TestNonPositivePhysicalInputs();
	TestNonFiniteComparisonCannotPass();
	TestBlackBodyPainterBoundaryAndNormalization();
	if( failures == 0 ) {
		std::cout << "All Planck radiance tests passed." << std::endl;
	}
	return failures == 0 ? 0 : 1;
}
