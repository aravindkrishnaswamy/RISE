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

	struct FireCertifiedNullspace
	{
		std::vector<std::string> stateOrder;
		std::vector<std::string> constraintRowOrder;
		std::size_t constraintRows;
		std::size_t stateDimension;
		std::size_t declaredRank;
		std::size_t nullity;
		std::vector<double> constraintMatrix;
		std::vector<double> orthonormalBasis;

		FireCertifiedNullspace() : constraintRows(0), stateDimension(0),
			declaredRank(0), nullity(0) {}

		bool Project(
			const std::vector<double>& input,
			std::vector<double>& output,
			std::string* error = 0
			) const;
	};

	//! Canonical synthetic RED inputs.  These records are deliberately not
	//! loadable as physical fuel presets.
	struct FireSimulationSolverFixtureRecords
	{
		static const RISECBOR64::Bytes& NearRankDeficientV1();
		static const RISECBOR64::Bytes& CorrectRankWrongSubspaceV1();
	};

	//! Complete r51 methane substrate.  Unlike OpenSubsetV1 this record owns
	//! the physical species/order, element/reaction arithmetic and both §3.7
	//! certified nullspaces; wax/wood owner-gated stubs are not members.
	class FireSimulationMethaneRecord
	{
		bool m_valid;
		std::string m_recordName;
		std::string m_recordId;
		RISECBOR64::Bytes m_recordBytes;
		double m_temperatureMinK;
		double m_temperatureMaxK;
		double m_referenceTemperatureK;
		double m_pressurePa;
		double m_lowerHeatingValueJPerKG;
		double m_stoichiometricOxygenKGPerKGFuel;
		double m_sootOxygenKGPerKGCarbon;
		double m_sootCO2KGPerKGCarbon;
		double m_sootHeatReleaseJPerKGCarbon;
		std::vector<std::string> m_speciesOrder;
		std::vector<std::string> m_elementOrder;
		std::vector<double> m_elementMassFractionMatrix;
		std::vector<double> m_ambientMassFractions;
		std::vector<double> m_injectedMassFractions;
		std::vector<double> m_primaryReactionDelta;
		FireCertifiedNullspace m_reconstruction;
		FireCertifiedNullspace m_nonadvectiveFluxProjection;
		std::vector<std::string> m_predictiveBlockers;

		bool LoadSemanticRecord(
			const RISECBOR64::Value& record,
			std::string* error
			);

	public:
		FireSimulationMethaneRecord();

		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		static const FireSimulationMethaneRecord& PhysicalV1();

		bool IsValid() const { return m_valid; }
		bool IsPredictiveQualified() const
		{
			return m_valid && m_predictiveBlockers.empty();
		}
		const std::string& RecordName() const { return m_recordName; }
		const std::string& RecordId() const { return m_recordId; }
		const RISECBOR64::Bytes& RecordBytes() const { return m_recordBytes; }
		double TemperatureMinK() const { return m_temperatureMinK; }
		double TemperatureMaxK() const { return m_temperatureMaxK; }
		double ReferenceTemperatureK() const { return m_referenceTemperatureK; }
		double ThermodynamicPressurePa() const { return m_pressurePa; }
		double LowerHeatingValueJPerKG() const { return m_lowerHeatingValueJPerKG; }
		double StoichiometricOxygenKGPerKGFuel() const
		{
			return m_stoichiometricOxygenKGPerKGFuel;
		}
		double SootOxygenKGPerKGCarbon() const { return m_sootOxygenKGPerKGCarbon; }
		double SootCO2KGPerKGCarbon() const { return m_sootCO2KGPerKGCarbon; }
		double SootHeatReleaseJPerKGCarbon() const
		{
			return m_sootHeatReleaseJPerKGCarbon;
		}
		const std::vector<std::string>& SpeciesOrder() const { return m_speciesOrder; }
		const std::vector<std::string>& ElementOrder() const { return m_elementOrder; }
		const std::vector<double>& ElementMassFractionMatrix() const
		{
			return m_elementMassFractionMatrix;
		}
		const std::vector<double>& AmbientMassFractions() const
		{
			return m_ambientMassFractions;
		}
		const std::vector<double>& InjectedMassFractions() const
		{
			return m_injectedMassFractions;
		}
		const std::vector<double>& PrimaryReactionDelta() const
		{
			return m_primaryReactionDelta;
		}
		const FireCertifiedNullspace& ConservativeReconstruction() const
		{
			return m_reconstruction;
		}
		const FireCertifiedNullspace& NonadvectiveFluxProjection() const
		{
			return m_nonadvectiveFluxProjection;
		}
		const std::vector<std::string>& PredictiveBlockers() const
		{
			return m_predictiveBlockers;
		}
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

	struct FireGasOpacityCell
	{
		double coefficients[4][4];
		double gasDerivativeMinimum;
		double gasDerivativeMaximum;
		double radiationDerivativeMinimum;
		double radiationDerivativeMaximum;
	};

	struct FireGasOpacitySpecies
	{
		std::string id;
		std::vector<double> gasTemperatureAxisK;
		std::vector<double> radiationTemperatureAxisK;
		std::vector<FireGasOpacityCell> cells;
	};

	//! Certified simulator-only optically-thin CO2/H2O Planck means.
	class FireSimulationGasOpacityRecord
	{
		bool m_valid;
		std::string m_recordName;
		std::string m_recordId;
		RISECBOR64::Bytes m_recordBytes;
		double m_temperatureMinK;
		double m_temperatureMaxK;
		std::vector<FireGasOpacitySpecies> m_species;

		bool LoadSemanticRecord(
			const RISECBOR64::Value& record,
			std::string* error
			);

	public:
		FireSimulationGasOpacityRecord();

		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		static const FireSimulationGasOpacityRecord& HITEMPPlanckMeanV1();

		bool IsValid() const { return m_valid; }
		const std::string& RecordName() const { return m_recordName; }
		const std::string& RecordId() const { return m_recordId; }
		const RISECBOR64::Bytes& RecordBytes() const { return m_recordBytes; }
		double TemperatureMinK() const { return m_temperatureMinK; }
		double TemperatureMaxK() const { return m_temperatureMaxK; }

		const FireGasOpacitySpecies* FindSpecies( const char* id ) const;

		//! Returns sigma_P and its two analytic partial derivatives.
		bool PlanckMeanCrossSectionM2PerMolecule(
			const char* speciesId,
			double gasTemperatureK,
			double radiationTemperatureK,
			double& result,
			double& gasTemperatureDerivative,
			double& radiationTemperatureDerivative,
			std::string* error = 0
			) const;

		//! Encloses both partial derivatives over a closed temperature rectangle.
		bool PlanckMeanDerivativeEnclosure(
			const char* speciesId,
			double gasTemperatureMinimumK,
			double gasTemperatureMaximumK,
			double radiationTemperatureMinimumK,
			double radiationTemperatureMaximumK,
			double& gasDerivativeMinimum,
			double& gasDerivativeMaximum,
			double& radiationDerivativeMinimum,
			double& radiationDerivativeMaximum,
			std::string* error = 0
			) const;
	};
}

#endif
