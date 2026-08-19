//////////////////////////////////////////////////////////////////////
//
//  FireProductionForce.h - strict-binary32 production force primitives
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIRE_PRODUCTION_FORCE_H
#define FIRE_PRODUCTION_FORCE_H

#include <array>
#include <cstdint>
#include <string>

namespace RISE
{
	struct FireProductionVremanInput
	{
		std::array<float,9> velocityGradientPerS;
		std::array<float,3> directionalWidthsM;
		float coefficient;

		FireProductionVremanInput() : coefficient(0.07f)
		{
			velocityGradientPerS.fill(0.0f);
			directionalWidthsM.fill(1.0f);
		}
	};

	//! Strict-binary32 transcription of the record Vreman expression. The
	//! caller supplies the coefficient obtained from the certified record.
	bool EvaluateFireProductionVremanEddyViscosity(
		const FireProductionVremanInput& input,
		float& eddyViscosityM2PerS,
		std::string* error=0 );

	struct FireProductionViscousSchedule
	{
		std::uint32_t substepCount;
		float substepTimeS;
		double outwardWork;
		double representedProductUpper;

		FireProductionViscousSchedule() : substepCount(0u),substepTimeS(0.0f),
			outwardWork(0.0),representedProductUpper(0.0) {}
	};

	//! Host half of the r90/r91 outward stability certificate. Both inputs are
	//! stored fp32 bytes; selection certifies the represented fp32 dt/N and
	//! rejects a required ninth substep before any physical update.
	bool SelectFireProductionViscousSchedule(
		float timeStepS,
		float outwardLambdaPerS,
		FireProductionViscousSchedule& schedule,
		std::string* error=0 );
}

#endif
