//////////////////////////////////////////////////////////////////////
//
//  PlanckRadiance.cpp - Absolute blackbody spectral-radiance kernel
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PlanckRadiance.h"

#include <cmath>

using namespace RISE;

Scalar RISE::PlanckSpectralRadianceNM(
	const Scalar wavelengthNM,
	const Scalar temperatureKelvin )
{
	if( wavelengthNM <= 0.0 || temperatureKelvin <= 0.0 ) {
		return 0.0;
	}

	// Exact SI constants (2019 SI definitions). The final factor converts
	// radiance per metre of wavelength to radiance per nanometre.
	static const Scalar planckConstant = 6.62607015e-34;
	static const Scalar speedOfLight = 299792458.0;
	static const Scalar boltzmannConstant = 1.380649e-23;
	static const Scalar nanometresToMetres = 1e-9;

	const Scalar wavelengthM = wavelengthNM * nanometresToMetres;
	const Scalar wavelengthM2 = wavelengthM * wavelengthM;
	const Scalar wavelengthM5 = wavelengthM2 * wavelengthM2 * wavelengthM;
	const Scalar exponent = planckConstant * speedOfLight /
		( wavelengthM * boltzmannConstant * temperatureKelvin );
	const Scalar perMetre = 2.0 * planckConstant * speedOfLight * speedOfLight /
		( wavelengthM5 * std::expm1( exponent ) );

	return nanometresToMetres * perMetre;
}
