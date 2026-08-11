//////////////////////////////////////////////////////////////////////
//
//  FireSimulationRecords.h - Phase-C open physical-property record subsets
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIRESIMULATIONRECORDS_
#define FIRESIMULATIONRECORDS_

#include "FireOptics.h"
#include "RISECBOR64.h"
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	struct FireThermochemistrySegment
	{
		double temperatureMinK;
		double temperatureMaxK;
		double coefficients[7];
		double sensibleEnthalpyOffsetJPerKG;
		double certifiedCpLowerJPerKGK;
	};

	struct FireThermochemistrySpecies
	{
		std::string id;
		double molecularWeightKGPerKMol;
		double formationEnthalpyJPerKMol;
		std::vector<FireThermochemistrySegment> segments;
	};

	class FireSimulationThermochemistryRecord
	{
		bool m_valid;
		std::string m_recordName;
		std::string m_recordId;
		RISECBOR64::Bytes m_recordBytes;
		double m_temperatureMinK;
		double m_temperatureMaxK;
		double m_referenceTemperatureK;
		std::vector<FireThermochemistrySpecies> m_species;
		std::vector<std::string> m_predictiveBlockers;

		bool LoadSemanticRecord(
			const RISECBOR64::Value& record,
			std::string* error
			);

	public:
		FireSimulationThermochemistryRecord();

		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		static const FireSimulationThermochemistryRecord& OpenSubsetV1();

		bool IsValid() const { return m_valid; }
		bool IsPredictiveQualified() const
		{
			return m_valid && m_predictiveBlockers.empty();
		}
		const std::string& RecordName() const { return m_recordName; }
		const std::string& RecordId() const { return m_recordId; }
		const RISECBOR64::Bytes& RecordBytes() const { return m_recordBytes; }
		const std::vector<std::string>& PredictiveBlockers() const
		{
			return m_predictiveBlockers;
		}
		double TemperatureMinK() const { return m_temperatureMinK; }
		double TemperatureMaxK() const { return m_temperatureMaxK; }
		double ReferenceTemperatureK() const { return m_referenceTemperatureK; }

		const FireThermochemistrySpecies* FindSpecies( const char* id ) const;
		bool CpJPerKGK(
			const char* speciesId,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool SensibleEnthalpyJPerKG(
			const char* speciesId,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool MixtureSensibleEnergyJPerM3(
			const std::vector<std::pair<std::string,double> >& massDensitiesKGPerM3,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool InvertMixtureTemperatureK(
			const std::vector<std::pair<std::string,double> >& massDensitiesKGPerM3,
			double sensibleEnergyJPerM3,
			double& result,
			std::string* error = 0
			) const;
	};

	struct FireTransportSpecies
	{
		std::string id;
		DifferentiableSpectrum viscosity;
		DifferentiableSpectrum conductivity;
	};

	class FireSimulationTransportRecord
	{
		bool m_valid;
		std::string m_recordName;
		std::string m_recordId;
		RISECBOR64::Bytes m_recordBytes;
		double m_temperatureMinK;
		double m_temperatureMaxK;
		double m_turbulentPrandtl;
		double m_turbulentSchmidt;
		double m_vremanCv;
		double m_vremanCnu;
		double m_chemicalTimeS;
		double m_criticalFlameTemperatureK;
		std::vector<FireTransportSpecies> m_species;
		std::vector<std::string> m_predictiveBlockers;

		bool LoadSemanticRecord(
			const RISECBOR64::Value& record,
			std::string* error
			);

	public:
		FireSimulationTransportRecord();

		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		static const FireSimulationTransportRecord& OpenV1();

		bool IsValid() const { return m_valid; }
		bool IsPredictiveQualified() const
		{
			return m_valid && m_predictiveBlockers.empty();
		}
		const std::string& RecordName() const { return m_recordName; }
		const std::string& RecordId() const { return m_recordId; }
		const RISECBOR64::Bytes& RecordBytes() const { return m_recordBytes; }
		const std::vector<std::string>& PredictiveBlockers() const
		{
			return m_predictiveBlockers;
		}
		double TemperatureMinK() const { return m_temperatureMinK; }
		double TemperatureMaxK() const { return m_temperatureMaxK; }
		double TurbulentPrandtl() const { return m_turbulentPrandtl; }
		double TurbulentSchmidt() const { return m_turbulentSchmidt; }
		double VremanCv() const { return m_vremanCv; }
		double VremanCnu() const { return m_vremanCnu; }
		double ChemicalTimeS() const { return m_chemicalTimeS; }
		double CriticalFlameTemperatureK() const
		{
			return m_criticalFlameTemperatureK;
		}

		const FireTransportSpecies* FindSpecies( const char* id ) const;
		bool ViscosityPaS(
			const char* speciesId,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool ConductivityWPerMK(
			const char* speciesId,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool MixtureViscosityPaS(
			const std::vector<std::pair<std::string,double> >& massFractions,
			const FireSimulationThermochemistryRecord& thermochemistry,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool MixtureConductivityWPerMK(
			const std::vector<std::pair<std::string,double> >& massFractions,
			const FireSimulationThermochemistryRecord& thermochemistry,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		bool VremanEddyViscosityM2PerS(
			const double velocityGradientPerS[3][3],
			const double directionalWidthsM[3],
			double& result,
			std::string* error = 0
			) const;
		bool EffectiveTransport(
			double molecularViscosityPaS,
			double molecularConductivityWPerMK,
			double gasDensityKGPerM3,
			double gasCpJPerKGK,
			double eddyViscosityM2PerS,
			bool dns,
			double& molecularDiffusivityM2PerS,
			double& sgsDiffusivityM2PerS,
			double& totalDiffusivityM2PerS,
			double& effectiveViscosityPaS,
			double& effectiveConductivityWPerMK,
			std::string* error = 0
			) const;
	};
}

#endif
