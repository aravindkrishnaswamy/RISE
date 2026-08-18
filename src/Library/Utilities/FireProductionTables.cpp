//////////////////////////////////////////////////////////////////////
//
//  FireProductionTables.cpp - record-derived fp32 production fire tables
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionTables.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace RISE
{
	namespace
	{
		const char* const kCompilerVersion="fire-production-table-compiler-r83-v1";

		bool Fail( std::string* error, const std::string& message )
		{
			if( error ) *error=message;
			return false;
		}

		std::uint32_t FloatBits( float value )
		{
			std::uint32_t bits=0;
			std::memcpy(&bits,&value,sizeof(bits));
			return bits;
		}

		RISECBOR64::Value FloatBitsValue( const std::vector<float>& values )
		{
			RISECBOR64::Value::Values encoded;
			encoded.reserve(values.size());
			for( const float value : values )
				encoded.push_back(RISECBOR64::Value::Unsigned(FloatBits(value)));
			return RISECBOR64::Value::ArrayValue(encoded);
		}

		bool EvaluateThermo( const FireSimulationMethaneRecord& record,
			const char* species, double temperatureK, double& enthalpy, double& cp,
			std::string* error )
		{
			return record.SensibleEnthalpyJPerKG(species,temperatureK,enthalpy,error)&&
				record.CpJPerKGK(species,temperatureK,cp,error)&&
				std::isfinite(enthalpy)&&std::isfinite(cp)&&cp>0.0;
		}

		bool AppendThermoInterval( const FireSimulationMethaneRecord& record,
			const FireThermochemistrySpecies& species, double lowerK, double upperK,
			double cpLower, unsigned depth, FireProductionThermochemistryTable& table,
			std::string* error )
		{
			if( !(lowerK<upperK) || depth>24u )
				return Fail(error,"production thermochemistry table subdivision did not converge");
			double lowerH=0.0,lowerCp=0.0,upperH=0.0,upperCp=0.0;
			double middleH=0.0,middleCp=0.0;
			const double middleK=0.5*(lowerK+upperK);
			if( !EvaluateThermo(record,species.id.c_str(),lowerK,lowerH,lowerCp,error)||
				!EvaluateThermo(record,species.id.c_str(),upperK,upperH,upperCp,error)||
				!EvaluateThermo(record,species.id.c_str(),middleK,middleH,middleCp,error) )
				return false;

			const float lowerHF=static_cast<float>(lowerH);
			const float upperHF=static_cast<float>(upperH);
			const float lowerCpF=static_cast<float>(lowerCp);
			const float upperCpF=static_cast<float>(upperCp);
			const double interpolatedH=0.5*(static_cast<double>(lowerHF)+upperHF);
			const double interpolatedCp=0.5*(static_cast<double>(lowerCpF)+upperCpF);
			const double enthalpyError=std::fabs(interpolatedH-middleH);
			const double cpIntegratedError=
				std::fabs(interpolatedCp-middleCp)*0.5*(upperK-lowerK);
			const double allowed=cpLower*0.25;
			if( enthalpyError>allowed || cpIntegratedError>allowed ) {
				if( !AppendThermoInterval(record,species,lowerK,middleK,cpLower,depth+1u,
					table,error) ) return false;
				return AppendThermoInterval(record,species,middleK,upperK,cpLower,depth+1u,
					table,error);
			}

			if( table.temperatureK.empty() ) {
				table.temperatureK.push_back(static_cast<float>(lowerK));
				table.sensibleEnthalpyJPerKG.push_back(lowerHF);
				table.cpJPerKGK.push_back(lowerCpF);
				table.maximumEnthalpyErrorJPerKG=std::max(
					table.maximumEnthalpyErrorJPerKG,std::fabs(static_cast<double>(lowerHF)-lowerH));
				table.maximumCpIntegratedErrorJPerKG=std::max(
					table.maximumCpIntegratedErrorJPerKG,
					std::fabs(static_cast<double>(lowerCpF)-lowerCp)*0.5*(upperK-lowerK));
			}
			table.temperatureK.push_back(static_cast<float>(upperK));
			table.sensibleEnthalpyJPerKG.push_back(upperHF);
			table.cpJPerKGK.push_back(upperCpF);
			table.maximumEnthalpyErrorJPerKG=std::max(table.maximumEnthalpyErrorJPerKG,
				std::max(enthalpyError,std::fabs(static_cast<double>(upperHF)-upperH)));
			table.maximumCpIntegratedErrorJPerKG=std::max(
				table.maximumCpIntegratedErrorJPerKG,std::max(cpIntegratedError,
				std::fabs(static_cast<double>(upperCpF)-upperCp)*0.5*(upperK-lowerK)));
			return true;
		}

		bool BuildThermoTable( const FireSimulationMethaneRecord& record,
			const FireThermochemistrySpecies& species,
			FireProductionThermochemistryTable& table, std::string* error )
		{
			table=FireProductionThermochemistryTable();
			table.speciesId=species.id;
			if( species.segments.empty() )
				return Fail(error,"production thermochemistry species has no segments");
			for( const FireThermochemistrySegment& segment : species.segments ) {
				const double lower=std::max(segment.temperatureMinK,record.TemperatureMinK());
				const double upper=std::min(segment.temperatureMaxK,record.TemperatureMaxK());
				if( lower>=upper ) continue;
				if( !AppendThermoInterval(record,species,lower,upper,
					segment.certifiedCpLowerJPerKGK,0u,table,error) ) return false;
			}
			return table.temperatureK.size()>=2u;
		}

		std::vector<double> RefinedAxis( const std::vector<double>& source,
			unsigned subdivisions )
		{
			std::vector<double> result;
			if( source.size()<2u ) return result;
			result.reserve((source.size()-1u)*subdivisions+1u);
			for( std::size_t i=0;i+1u<source.size();++i ) {
				for( unsigned j=0;j<subdivisions;++j )
					result.push_back(source[i]+(source[i+1u]-source[i])*
						static_cast<double>(j)/static_cast<double>(subdivisions));
			}
			result.push_back(source.back());
			return result;
		}

		bool SampleOpacity( const FireSimulationGasOpacityRecord& record,
			const char* species, double gasK, double radiationK, double& value,
			std::string* error )
		{
			double gasDerivative=0.0,radiationDerivative=0.0;
			return record.PlanckMeanCrossSectionM2PerMolecule(species,gasK,radiationK,
				value,gasDerivative,radiationDerivative,error)&&std::isfinite(value)&&value>=0.0;
		}

		bool BuildOpacityAtSubdivision( const FireSimulationGasOpacityRecord& record,
			const FireGasOpacitySpecies& species, unsigned subdivisions,
			FireProductionOpacityTable& table, bool& acceptable, std::string* error )
		{
			const std::vector<double> gasAxis=RefinedAxis(species.gasTemperatureAxisK,subdivisions);
			const std::vector<double> radiationAxis=
				RefinedAxis(species.radiationTemperatureAxisK,subdivisions);
			if( gasAxis.size()<2u || radiationAxis.size()<2u )
				return Fail(error,"production opacity table has an invalid source axis");
			table=FireProductionOpacityTable();
			table.speciesId=species.id;
			table.gasTemperatureK.reserve(gasAxis.size());
			table.radiationTemperatureK.reserve(radiationAxis.size());
			for( const double value : gasAxis ) table.gasTemperatureK.push_back(static_cast<float>(value));
			for( const double value : radiationAxis )
				table.radiationTemperatureK.push_back(static_cast<float>(value));
			table.planckMeanM2PerMolecule.resize(gasAxis.size()*radiationAxis.size());
			std::vector<double> exact(table.planckMeanM2PerMolecule.size(),0.0);
			for( std::size_t g=0;g<gasAxis.size();++g ) for( std::size_t r=0;r<radiationAxis.size();++r ) {
				double value=0.0;
				if( !SampleOpacity(record,species.id.c_str(),gasAxis[g],radiationAxis[r],value,error) )
					return false;
				const std::size_t index=g*radiationAxis.size()+r;
				exact[index]=value;
				table.planckMeanM2PerMolecule[index]=static_cast<float>(value);
			}

			double maximumRelative=0.0;
			for( std::size_t i=0;i<exact.size();++i ) {
				const double scale=std::max(std::fabs(exact[i]),
					std::fabs(static_cast<double>(table.planckMeanM2PerMolecule[i])));
				if( scale>0.0 ) maximumRelative=std::max(maximumRelative,
					std::fabs(static_cast<double>(table.planckMeanM2PerMolecule[i])-exact[i])/scale);
			}
			for( std::size_t g=0;g+1u<gasAxis.size();++g ) for( std::size_t r=0;r+1u<radiationAxis.size();++r ) {
				const double middleGas=0.5*(gasAxis[g]+gasAxis[g+1u]);
				const double middleRadiation=0.5*(radiationAxis[r]+radiationAxis[r+1u]);
				double middle=0.0;
				if( !SampleOpacity(record,species.id.c_str(),middleGas,middleRadiation,middle,error) )
					return false;
				const std::size_t stride=radiationAxis.size();
				const double interpolated=0.25*(
					static_cast<double>(table.planckMeanM2PerMolecule[g*stride+r])+
					table.planckMeanM2PerMolecule[(g+1u)*stride+r]+
					table.planckMeanM2PerMolecule[g*stride+r+1u]+
					table.planckMeanM2PerMolecule[(g+1u)*stride+r+1u]);
				const double scale=std::max(std::fabs(middle),std::max(std::fabs(interpolated),
					std::max(std::fabs(exact[g*stride+r]),
						std::fabs(exact[(g+1u)*stride+r+1u]))));
				if( scale<=0.0 ) {
					if( middle!=interpolated ) return Fail(error,"zero-scale production opacity cell differs");
				} else maximumRelative=std::max(maximumRelative,std::fabs(interpolated-middle)/scale);
			}
			table.maximumRelativeError=maximumRelative;
			acceptable=maximumRelative<=0.005;
			return true;
		}

		bool BuildOpacityTable( const FireSimulationGasOpacityRecord& record,
			const FireGasOpacitySpecies& species, FireProductionOpacityTable& table,
			std::string* error )
		{
			for( unsigned level=0;level<=8u;++level ) {
				bool acceptable=false;
				if( !BuildOpacityAtSubdivision(record,species,1u<<level,table,acceptable,error) )
					return false;
				if( acceptable ) return true;
			}
			return Fail(error,"production opacity table subdivision did not reach 0.5 percent");
		}

		RISECBOR64::Value ThermoManifestValue(
			const FireProductionThermochemistryTable& table )
		{
			using RISECBOR64::Value;
			return Value::MapValue({
				{"cp_bits_f32",FloatBitsValue(table.cpJPerKGK)},
				{"maximum_cp_integrated_error_J_per_kg",Value::Float(table.maximumCpIntegratedErrorJPerKG)},
				{"maximum_enthalpy_error_J_per_kg",Value::Float(table.maximumEnthalpyErrorJPerKG)},
				{"sensible_enthalpy_bits_f32",FloatBitsValue(table.sensibleEnthalpyJPerKG)},
				{"species_id",Value::String(table.speciesId)},
				{"temperature_bits_f32",FloatBitsValue(table.temperatureK)}
			});
		}

		RISECBOR64::Value OpacityManifestValue( const FireProductionOpacityTable& table )
		{
			using RISECBOR64::Value;
			return Value::MapValue({
				{"gas_temperature_bits_f32",FloatBitsValue(table.gasTemperatureK)},
				{"maximum_relative_error",Value::Float(table.maximumRelativeError)},
				{"planck_mean_bits_f32",FloatBitsValue(table.planckMeanM2PerMolecule)},
				{"radiation_temperature_bits_f32",FloatBitsValue(table.radiationTemperatureK)},
				{"species_id",Value::String(table.speciesId)}
			});
		}

		template<typename Table>
		const Table* FindTable( const std::vector<Table>& tables, const char* speciesId )
		{
			if( !speciesId ) return 0;
			for( const Table& table : tables ) if( table.speciesId==speciesId ) return &table;
			return 0;
		}

		bool Bracket( const std::vector<float>& axis, double value,
			std::size_t& lower, double& fraction )
		{
			if( axis.size()<2u || value<axis.front() || value>axis.back() ) return false;
			const std::vector<float>::const_iterator found=
				std::lower_bound(axis.begin(),axis.end(),static_cast<float>(value));
			if( found==axis.begin() ) lower=0u;
			else if( found==axis.end() ) lower=axis.size()-2u;
			else lower=static_cast<std::size_t>(found-axis.begin()-1);
			const double a=axis[lower],b=axis[lower+1u];
			fraction=(value-a)/(b-a);
			return fraction>=0.0&&fraction<=1.0;
		}
	}

	bool BuildFireProductionTablePackage(
		const FireSimulationMethaneRecord& thermochemistry,
		const FireSimulationGasOpacityRecord& opacity,
		FireProductionTablePackage& result, std::string* error )
	{
		if( !thermochemistry.IsValid() || !opacity.IsValid() )
			return Fail(error,"production table compiler requires valid source records");
		FireProductionTablePackage candidate;
		for( const std::string& speciesId : thermochemistry.SpeciesOrder() ) {
			const FireThermochemistrySpecies* species=thermochemistry.FindSpecies(speciesId.c_str());
			if( !species ) return Fail(error,"production thermochemistry species order is unresolved");
			FireProductionThermochemistryTable table;
			if( !BuildThermoTable(thermochemistry,*species,table,error) ) return false;
			candidate.m_thermochemistry.push_back(table);
		}
		const char* const opacitySpecies[]={"CO2","H2O"};
		for( const char* speciesId : opacitySpecies ) {
			const FireGasOpacitySpecies* species=opacity.FindSpecies(speciesId);
			if( !species ) return Fail(error,"production opacity source species is missing");
			FireProductionOpacityTable table;
			if( !BuildOpacityTable(opacity,*species,table,error) ) return false;
			candidate.m_opacity.push_back(table);
		}

		using RISECBOR64::Value;
		Value::Values thermoValues,opacityValues;
		for( const FireProductionThermochemistryTable& table : candidate.m_thermochemistry )
			thermoValues.push_back(ThermoManifestValue(table));
		for( const FireProductionOpacityTable& table : candidate.m_opacity )
			opacityValues.push_back(OpacityManifestValue(table));
		const Value payload=Value::MapValue({
			{"compiler_version",Value::String(kCompilerVersion)},
			{"gas_opacity_record_id",Value::String(opacity.RecordId())},
			{"opacity_tables",Value::ArrayValue(opacityValues)},
			{"record_kind",Value::String("fire-production-table-package-v1")},
			{"schema_version",Value::Unsigned(1)},
			{"thermochemistry_record_id",Value::String(thermochemistry.RecordId())},
			{"thermochemistry_tables",Value::ArrayValue(thermoValues)}
		});
		RISECBOR64::Bytes payloadBytes;
		if( !RISECBOR64::Encode(payload,payloadBytes,error) ) return false;
		candidate.m_tablePackageId=RISECBOR64::SHA256Hex(payloadBytes);
		if( !RISECBOR64::Encode(Value::MapValue({
			{"payload",payload},{"table_package_id",Value::String(candidate.m_tablePackageId)}}),
			candidate.m_canonicalEnvelope,error) ) return false;
		result=candidate;
		return true;
	}

	bool FireProductionTablePackage::ThermochemistryValues(
		const char* speciesId, double temperatureK, double& sensibleEnthalpyJPerKG,
		double& cpJPerKGK, std::string* error ) const
	{
		const FireProductionThermochemistryTable* table=FindTable(m_thermochemistry,speciesId);
		if( !table ) return Fail(error,"production thermochemistry table species is unavailable");
		std::size_t lower=0u;double fraction=0.0;
		if( !Bracket(table->temperatureK,temperatureK,lower,fraction) )
			return Fail(error,"production thermochemistry lookup is outside the compiled domain");
		sensibleEnthalpyJPerKG=(1.0-fraction)*table->sensibleEnthalpyJPerKG[lower]+
			fraction*table->sensibleEnthalpyJPerKG[lower+1u];
		cpJPerKGK=(1.0-fraction)*table->cpJPerKGK[lower]+fraction*table->cpJPerKGK[lower+1u];
		return std::isfinite(sensibleEnthalpyJPerKG)&&std::isfinite(cpJPerKGK)&&cpJPerKGK>0.0;
	}

	bool FireProductionTablePackage::PlanckMeanM2PerMolecule(
		const char* speciesId, double gasTemperatureK, double radiationTemperatureK,
		double& result, std::string* error ) const
	{
		const FireProductionOpacityTable* table=FindTable(m_opacity,speciesId);
		if( !table ) return Fail(error,"production opacity table species is unavailable");
		std::size_t gasLower=0u,radiationLower=0u;double gasFraction=0.0,radiationFraction=0.0;
		if( !Bracket(table->gasTemperatureK,gasTemperatureK,gasLower,gasFraction)||
			!Bracket(table->radiationTemperatureK,radiationTemperatureK,radiationLower,
				radiationFraction) )
			return Fail(error,"production opacity lookup is outside the compiled domain");
		const std::size_t stride=table->radiationTemperatureK.size();
		const double lower=(1.0-radiationFraction)*
			table->planckMeanM2PerMolecule[gasLower*stride+radiationLower]+
			radiationFraction*table->planckMeanM2PerMolecule[gasLower*stride+radiationLower+1u];
		const double upper=(1.0-radiationFraction)*
			table->planckMeanM2PerMolecule[(gasLower+1u)*stride+radiationLower]+
			radiationFraction*table->planckMeanM2PerMolecule[(gasLower+1u)*stride+radiationLower+1u];
		result=(1.0-gasFraction)*lower+gasFraction*upper;
		return std::isfinite(result)&&result>=0.0;
	}
}
