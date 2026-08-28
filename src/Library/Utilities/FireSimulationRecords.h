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
	enum class FireStateProducerPrecision : unsigned char
	{
		Unknown=0u,
		Binary64=1u,
		Binary32=2u
	};

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
		//! Allocation-free projection for fixed-size solver face tuples.
		bool Project(
			const double* input,
			std::size_t inputCount,
			double* output,
			std::size_t outputCount,
			std::string* error = 0
			) const;
	};

	struct FireAcceptedStateFeasibilityEnvelope
	{
		double limiterOutwardFactorEpsilon64;
		double rowAccumulationFactorEpsilon64;
		double nullspaceProjectionFactorEpsilon64;
		double sourcePacketFactorEpsilon64;
		double ledgerReductionFactorEpsilon64;
		double derivedUnionFactorEpsilon64;
		double kappaEpsilon64;
		double remapFactorEpsilon32;
		double composedForceFactorEpsilon32;
		double projectionFactorEpsilon32;
		double sourcePacketFactorEpsilon32;
		double derivedUnionFactorEpsilon32;
		double kappaEpsilon32;

		FireAcceptedStateFeasibilityEnvelope() :
			limiterOutwardFactorEpsilon64(0.0), rowAccumulationFactorEpsilon64(0.0),
			nullspaceProjectionFactorEpsilon64(0.0), sourcePacketFactorEpsilon64(0.0),
			ledgerReductionFactorEpsilon64(0.0), derivedUnionFactorEpsilon64(0.0),
			kappaEpsilon64(0.0),remapFactorEpsilon32(0.0),
			composedForceFactorEpsilon32(0.0),projectionFactorEpsilon32(0.0),
			sourcePacketFactorEpsilon32(0.0),
			derivedUnionFactorEpsilon32(0.0),kappaEpsilon32(0.0) {}
	};

	//! Canonical synthetic RED inputs.  These records are deliberately not
	//! loadable as physical fuel presets.
	struct FireSimulationSolverFixtureRecords
	{
		static const RISECBOR64::Bytes& NearRankDeficientV1();
		static const RISECBOR64::Bytes& CorrectRankWrongSubspaceV1();
		//! Runs a pinned synthetic candidate through the same §3.7 validator
		//! used by the physical record and succeeds only when that validator
		//! rejects the deliberately false certificate for the intended reason.
		static bool RejectsCandidateCertificate(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);
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
		double m_pilotTemperatureK;
		double m_autoignitionTemperatureK;
		double m_sootOxidationTemperatureK;
		double m_sootYieldKGPerKGFuel;
		double m_defaultRadiativeFraction;
		double m_radiativeFractionMinimum;
		double m_radiativeFractionMaximum;
		std::string m_sootDensityOpticsRecordName;
		std::string m_sootDensityOpticsRecordId;
		std::vector<FireThermochemistrySpecies> m_thermochemistrySpecies;
		std::vector<std::string> m_speciesOrder;
		std::vector<std::string> m_elementOrder;
		std::vector<double> m_elementMassFractionMatrix;
		std::vector<double> m_ambientMassFractions;
		std::vector<double> m_injectedMassFractions;
		std::vector<double> m_primaryReactionDelta;
		std::vector<double> m_sootOxidationDelta;
		FireCertifiedNullspace m_reconstruction;
		FireCertifiedNullspace m_nonadvectiveFluxProjection;
		FireAcceptedStateFeasibilityEnvelope m_acceptedStateFeasibilityEnvelope;
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
		double PilotTemperatureK() const { return m_pilotTemperatureK; }
		double AutoignitionTemperatureK() const { return m_autoignitionTemperatureK; }
		double SootOxidationTemperatureK() const { return m_sootOxidationTemperatureK; }
		double SootYieldKGPerKGFuel() const { return m_sootYieldKGPerKGFuel; }
		double DefaultRadiativeFraction() const { return m_defaultRadiativeFraction; }
		bool ResolveRadiativeFraction(
			const char* caseRecordId,
			bool hasCaseOverride,
			double caseOverride,
			double& result,
			std::string* error = 0
			) const;
		bool ResolveSootDensityKGPerM3(
			const FireOpticsPreset& optics,
			double& result,
			std::string* error = 0
			) const;
		const std::vector<std::string>& SpeciesOrder() const { return m_speciesOrder; }
		const FireThermochemistrySpecies* FindSpecies( const char* id ) const;
		bool CpJPerKGK( const char* speciesId, double temperatureK,
			double& result, std::string* error = 0 ) const;
		//! Evaluates cp for the complete record order without name lookups or allocation.
		bool CpBySpeciesOrderJPerKGK(
			double temperatureK, double* result, std::size_t count,
			std::string* error = 0 ) const;
		bool SensibleEnthalpyJPerKG( const char* speciesId, double temperatureK,
			double& result, std::string* error = 0 ) const;
		//! Allocation-free species lookup in the record's exact SpeciesOrder().
		bool SensibleEnthalpyBySpeciesOrderJPerKG(
			std::size_t speciesIndex,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		//! Evaluates the complete record order with shared temperature powers.
		bool SensibleEnthalpiesBySpeciesOrderJPerKG(
			double temperatureK,
			double* result,
			std::size_t count,
			std::string* error = 0
			) const;
		bool MixtureSensibleEnergyJPerM3(
			const std::vector<std::pair<std::string,double> >& massDensitiesKGPerM3,
			double temperatureK, double& result, std::string* error = 0 ) const;
		//! Allocation-free equivalent for solver cells in the record's exact SpeciesOrder().
		bool MixtureSensibleEnergyBySpeciesOrderJPerM3(
			const double* massDensitiesKGPerM3, std::size_t count,
			double temperatureK, double& result, std::string* error = 0 ) const;
		bool InvertMixtureTemperatureK(
			const std::vector<std::pair<std::string,double> >& massDensitiesKGPerM3,
			double sensibleEnergyJPerM3, double& result, std::string* error = 0 ) const;
		//! Allocation-free equivalent for solver cells in the record's exact SpeciesOrder().
		bool InvertMixtureTemperatureBySpeciesOrderK(
			const double* massDensitiesKGPerM3, std::size_t count,
			double sensibleEnergyJPerM3, double& result, std::string* error = 0 ) const;
		//! The single r60 accepted-state predicate over the production component
		//! order [rhoTotalZ, seven species, sensible energy].
		bool AcceptedConservativeStateAdmissibleByComponentOrder(
			const double* conservativeValues, std::size_t count,
			const double* lowerSensibleEnthalpyJPerKG,
			const double* upperSensibleEnthalpyJPerKG,
			std::size_t enthalpyCount,
			FireStateProducerPrecision producerPrecision,
			std::string* error = 0 ) const;
		//! Accepted-state EOS volume ratio used by the resident manifold gate.
		//! This first executes the single r60 predicate above; signed stored
		//! densities participate in energy inversion and positive-part gas
		//! availability participates in the ideal-gas volume.
		bool AcceptedConservativeVolumeRatioByComponentOrder(
			const double* conservativeValues, std::size_t count,
			FireStateProducerPrecision producerPrecision,
			double& result, std::string* error = 0 ) const;
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
		const std::vector<double>& SootOxidationDelta() const
		{
			return m_sootOxidationDelta;
		}
		const FireCertifiedNullspace& ConservativeReconstruction() const
		{
			return m_reconstruction;
		}
		const FireCertifiedNullspace& NonadvectiveFluxProjection() const
		{
			return m_nonadvectiveFluxProjection;
		}
		const FireAcceptedStateFeasibilityEnvelope& AcceptedStateFeasibilityEnvelope() const
		{
			return m_acceptedStateFeasibilityEnvelope;
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
		bool MixtureViscosityPaS(
			const std::vector<std::pair<std::string,double> >& massFractions,
			const FireSimulationMethaneRecord& thermochemistry,
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
		bool MixtureConductivityWPerMK(
			const std::vector<std::pair<std::string,double> >& massFractions,
			const FireSimulationMethaneRecord& thermochemistry,
			double temperatureK,
			double& result,
			std::string* error = 0
			) const;
		//! Allocation-free Wilke/WMS evaluation in the methane record's species order.
		bool MixturePropertiesBySpeciesOrder(
			const double* massFractions, std::size_t count,
			const FireSimulationMethaneRecord& thermochemistry,
			double temperatureK, double& viscosityPaS,
			double& conductivityWPerMK, std::string* error = 0
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
