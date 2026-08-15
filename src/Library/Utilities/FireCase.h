//////////////////////////////////////////////////////////////////////
// FireCase.h - Canonical fire-case-v1 record and r54 derivations
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_CASE_H
#define RISE_FIRE_CASE_H

#include "RISECBOR64.h"
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	class FireSimulationMethaneRecord;

	namespace FireCase
	{
		struct EnvelopeKnot { double timeS=0.0, value=0.0; };
		struct AuthoredV1
		{
			std::string fuelRecordId;
			std::string sourceKind="pool";
			double poolDiameterM=0.0,patchXM=0.0,patchZM=0.0;
			std::string intensityKind="hrr";
			double heatReleaseRateKW=0.0,fuelMassFluxKGPerM2S=0.0;
			std::vector<EnvelopeKnot> envelope;
			double durationS=0.0;
			std::string quality;
			double numericDStarTier=0.0;
			std::uint64_t seed=0;
			double outputFramesPerS=0.0;
			bool plumeLaw=false;
			bool hasRadiativeFractionOverride=false;
			double radiativeFractionOverride=0.0;
		};

		struct DerivedV1
		{
			double sourceAreaM2=0.0, nominalFuelFluxKGPerM2S=0.0;
			double peakEnvelope=0.0, referenceHeatReleaseRateW=0.0;
			double ambientDensityKGPerM3=0.0, ambientCpJPerKGK=0.0;
			double effectiveRadiativeFraction=0.0;
			double flameHeightM=0.0, effectiveFlameHeightM=0.0;
			double characteristicDiameterM=0.0, cellWidthM=0.0,resolutionTier=0.0;
			std::uint64_t nx=0,ny=0,nz=0;
			double extentXM=0.0,extentYM=0.0,extentZM=0.0;
			double flowThroughTimeS=0.0,preRollOrDiscardS=0.0;
			std::string windowKind;
			std::string perturbationDigest;
			std::string pilotModelVersion;
			std::string pilotMaskRule;
			std::string limiterAcceptanceModelVersion;
			double pilotPowerDensityWPerM3=0.0,pilotDurationMultiplier=0.0;
			double pilotBeginningTemperatureCeilingK=0.0;
		};

		struct RecordV1
		{
			AuthoredV1 authored;
			DerivedV1 derived;
			RISECBOR64::Bytes payloadBytes;
			RISECBOR64::Bytes envelopeBytes;
			std::string caseRecordId;
			std::vector<std::string> referencedRecordIds;
		};

		std::uint64_t SplitMix64(std::uint64_t value);
		bool BuildSourcePattern(
			const AuthoredV1&,const DerivedV1&,std::vector<double>&,
			std::string& error );
		bool BuildPilotMask(
			const AuthoredV1&,const DerivedV1&,std::vector<std::uint8_t>&,
			std::string& error );
		bool EvaluatePilotPowerDensityWPerM3(
			const DerivedV1&,bool maskCell,double simulationTimeS,
			double acceptedBeginningTemperatureK,double& powerDensityWPerM3,
			std::string& error );
		bool BuildMethaneV1(
			const AuthoredV1&,const FireSimulationMethaneRecord&,
			const std::vector<std::string>& referencedRecordIds,
			RecordV1&,std::string& error );
		bool ValidateEnvelopeV1(
			const RISECBOR64::Bytes&,RecordV1&,std::string& error );
		bool ValidateMethaneEnvelopeV1(
			const RISECBOR64::Bytes&,const FireSimulationMethaneRecord&,
			RecordV1&,std::string& error );
		double SelectTimeStepS(
			double cellWidthM,double maximumAxisSpeedMPerS,
			double maximumPositiveReducedGravityMPerS2,
			double maximumActiveDiffusivityM2PerS,double previousStepS );
	}
}

#endif
