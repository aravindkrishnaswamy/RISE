//////////////////////////////////////////////////////////////////////
//
//  PlanckRadiance.h - Absolute blackbody spectral-radiance kernel
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PLANCK_RADIANCE_
#define PLANCK_RADIANCE_

#include "Math3D/Math3D.h"

namespace RISE
{
	//! Planck spectral radiance per nanometre.
	/// \param wavelengthNM Wavelength in nanometres.
	/// \param temperatureKelvin Absolute temperature in kelvins.
	/// \return B_lambda in W m^-2 sr^-1 nm^-1. Non-positive physical
	///         inputs return zero.
	Scalar PlanckSpectralRadianceNM(
		const Scalar wavelengthNM,
		const Scalar temperatureKelvin );
}

#endif
