//////////////////////////////////////////////////////////////////////
//
//  FireProductionTables.h - record-derived fp32 production fire tables
//
//  Compiles certified binary64 NASA-9 thermochemistry and HITEMP gas
//  opacity records into deterministic fp32 lookup tables packaged with
//  a canonical CBOR/SHA-256 manifest. Shared by CPU and Metal fire
//  backends; units are SI (K, J/kg, J/(kg*K), m^2/molecule).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONTABLES_
#define FIREPRODUCTIONTABLES_

#include "FireSimulationRecords.h"

#include <string>
#include <vector>

namespace RISE
{
	//! Adaptive 1D fp32 lookup table of sensible enthalpy h(T) and Cp(T) for one
	//! species, with certified worst-case interpolation + quantization bounds.
	struct FireProductionThermochemistryTable
	{
		std::string speciesId;
		std::vector<float> temperatureK;
		std::vector<float> sensibleEnthalpyJPerKG;
		std::vector<float> cpJPerKGK;
		double maximumEnthalpyErrorJPerKG;
		double maximumCpIntegratedErrorJPerKG;

		FireProductionThermochemistryTable() : maximumEnthalpyErrorJPerKG(0.0),
			maximumCpIntegratedErrorJPerKG(0.0) {}
	};

	//! Uniformly refined 2D fp32 Planck-mean cross-section table over
	//! (gasTemperatureK x radiationTemperatureK), row-major in gas temperature.
	struct FireProductionOpacityTable
	{
		std::string speciesId;
		std::vector<float> gasTemperatureK;
		std::vector<float> radiationTemperatureK;
		std::vector<float> planckMeanM2PerMolecule;
		double maximumRelativeError;

		FireProductionOpacityTable() : maximumRelativeError(0.0) {}
	};

	//! Immutable bundle of compiled fp32 thermochemistry and opacity tables
	//! bound by a SHA-256 package ID over its canonical CBOR payload.
	class FireProductionTablePackage
	{
		std::string m_tablePackageId;
		RISECBOR64::Bytes m_canonicalEnvelope;
		std::vector<FireProductionThermochemistryTable> m_thermochemistry;
		std::vector<FireProductionOpacityTable> m_opacity;

	public:
		const std::string& TablePackageId() const { return m_tablePackageId; }
		const RISECBOR64::Bytes& CanonicalEnvelope() const { return m_canonicalEnvelope; }
		const std::vector<FireProductionThermochemistryTable>& Thermochemistry() const
		{
			return m_thermochemistry;
		}
		const std::vector<FireProductionOpacityTable>& Opacity() const { return m_opacity; }

		//! Piecewise-linear lookup of sensible enthalpy and Cp within the compiled T range.
		bool ThermochemistryValues(
			const char* speciesId, double temperatureK,
			double& sensibleEnthalpyJPerKG, double& cpJPerKGK,
			std::string* error=0 ) const;
		//! Bilinear lookup of Planck-mean absorption cross-section (m^2/molecule).
		bool PlanckMeanM2PerMolecule(
			const char* speciesId, double gasTemperatureK,
			double radiationTemperatureK, double& result,
			std::string* error=0 ) const;

		friend bool BuildFireProductionTablePackage(
			const FireSimulationMethaneRecord&,
			const FireSimulationGasOpacityRecord&,
			FireProductionTablePackage&, std::string* );
	};

	//! Deterministic compiler.  Error thresholds and subdivision rules are r83
	//! record semantics; callers cannot supply tolerances or grids.
	bool BuildFireProductionTablePackage(
		const FireSimulationMethaneRecord& thermochemistry,
		const FireSimulationGasOpacityRecord& opacity,
		FireProductionTablePackage& result,
		std::string* error=0 );
}

#endif
