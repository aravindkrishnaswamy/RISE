//////////////////////////////////////////////////////////////////////
//
//  FireProductionTables.h - record-derived fp32 production fire tables
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

	struct FireProductionOpacityTable
	{
		std::string speciesId;
		std::vector<float> gasTemperatureK;
		std::vector<float> radiationTemperatureK;
		std::vector<float> planckMeanM2PerMolecule;
		double maximumRelativeError;

		FireProductionOpacityTable() : maximumRelativeError(0.0) {}
	};

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

		bool ThermochemistryValues(
			const char* speciesId, double temperatureK,
			double& sensibleEnthalpyJPerKG, double& cpJPerKGK,
			std::string* error=0 ) const;
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
