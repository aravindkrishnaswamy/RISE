//////////////////////////////////////////////////////////////////////
//
//  PlanckRadianceTest.cpp - Numeric gates for the absolute Planck kernel
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Utilities/PlanckRadiance.h"

using namespace RISE;

namespace
{
	bool IsRelativeClose( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		return std::fabs( actual - expected ) <= tolerance * std::fabs( expected );
	}

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
	assert( IsRelativeClose( actual, expected, 1e-8 ) );
}

static void TestStefanBoltzmannIntegralIdentity()
{
	static const Scalar stefanBoltzmann = 5.670374419e-8;
	static const Scalar temperatureKelvin = 1800.0;
	const Scalar integratedExitance = PI * IntegratePlanckOverWavelengthNM( temperatureKelvin );
	const Scalar expected = stefanBoltzmann * std::pow( temperatureKelvin, 4.0 );
	std::cout << "pi integral(B dlambda) = " << integratedExitance
		<< ", sigma T^4 = " << expected << std::endl;
	assert( IsRelativeClose( integratedExitance, expected, 1e-8 ) );
}

static void TestNonPositivePhysicalInputs()
{
	assert( PlanckSpectralRadianceNM( 0.0, 1800.0 ) == 0.0 );
	assert( PlanckSpectralRadianceNM( -500.0, 1800.0 ) == 0.0 );
	assert( PlanckSpectralRadianceNM( 500.0, 0.0 ) == 0.0 );
	assert( PlanckSpectralRadianceNM( 500.0, -1800.0 ) == 0.0 );
}

int main()
{
	TestPinnedPointAnchor();
	TestStefanBoltzmannIntegralIdentity();
	TestNonPositivePhysicalInputs();
	std::cout << "All Planck radiance tests passed." << std::endl;
	return 0;
}
