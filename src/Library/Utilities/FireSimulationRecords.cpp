//////////////////////////////////////////////////////////////////////
//
//  FireSimulationRecords.cpp - Phase-C open physical-property record subsets
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireSimulationRecords.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace RISE
{
	namespace
	{
#include "FireSimulationRecordData.inc"

		const double kUniversalGasConstantJPerKMolK = 8314.46261815324;

		bool Fail( std::string* error, const std::string& message )
		{
			if( error ) *error = message;
			return false;
		}

		const RISECBOR64::Value* Required(
			const RISECBOR64::Value& map,
			const char* key,
			const RISECBOR64::Value::Type type,
			std::string* error
			)
		{
			if( map.GetType() != RISECBOR64::Value::Map ) {
				Fail(error,"fire-simulation semantic value is not a map");
				return 0;
			}
			const RISECBOR64::Value* value = map.Find(key);
			if( !value || value->GetType() != type ) {
				Fail(error,std::string("fire-simulation field '")+key+"' is missing or has the wrong type");
				return 0;
			}
			return value;
		}

		bool ReadText(
			const RISECBOR64::Value& map,
			const char* key,
			std::string& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				map,key,RISECBOR64::Value::Text,error);
			if( !value ) return false;
			result = value->GetText();
			return !result.empty() || Fail(error,std::string("fire-simulation field '")+key+"' is empty");
		}

		bool ReadFloat(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				map,key,RISECBOR64::Value::Float64,error);
			if( !value ) return false;
			result = value->GetFloat();
			return std::isfinite(result) || Fail(error,std::string("fire-simulation field '")+key+"' is non-finite");
		}

		bool ReadFloatArray(
			const RISECBOR64::Value& value,
			std::vector<double>& result,
			std::string* error
			)
		{
			if( value.GetType() != RISECBOR64::Value::Array ) {
				return Fail(error,"fire-simulation numeric array has the wrong type");
			}
			result.clear();
			for( const RISECBOR64::Value& item : value.GetArray() ) {
				if( item.GetType() != RISECBOR64::Value::Float64 ||
					!std::isfinite(item.GetFloat()) ) {
					return Fail(error,"fire-simulation numeric array contains a non-binary64 value");
				}
				result.push_back(item.GetFloat());
			}
			return true;
		}

		bool ReadDomain(
			const RISECBOR64::Value& map,
			const char* key,
			double& minimum,
			double& maximum,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				map,key,RISECBOR64::Value::Array,error);
			std::vector<double> domain;
			if( !value || !ReadFloatArray(*value,domain,error) || domain.size() != 2 ) {
				return Fail(error,"fire-simulation domain must contain two binary64 endpoints");
			}
			minimum = domain[0];
			maximum = domain[1];
			return (minimum > 0.0 && minimum < maximum && std::isfinite(maximum-minimum)) ||
				Fail(error,"fire-simulation domain is not positive and ordered");
		}

		bool ReadNumber(
			const RISECBOR64::Value& value,
			double& result,
			std::string* error
			)
		{
			if( value.GetType() == RISECBOR64::Value::Float64 ) {
				result = value.GetFloat();
			} else if( value.GetType() == RISECBOR64::Value::UnsignedInteger ) {
				result = static_cast<double>(value.GetIntegerArgument());
			} else if( value.GetType() == RISECBOR64::Value::NegativeInteger ) {
				result = -1.0-static_cast<double>(value.GetIntegerArgument());
			} else {
				return Fail(error,"fire-simulation value is not numeric");
			}
			return std::isfinite(result) || Fail(error,"fire-simulation value is non-finite");
		}

		bool ValidateUncertainty(
			const RISECBOR64::Value& uncertainty,
			std::string* error
			)
		{
			std::string kind;
			if( !ReadText(uncertainty,"kind",kind,error) ) return false;
			const std::set<std::string> allowed = {
				"assumption_bound", "computed_range_from_input_sensitivity",
				"design_pinned_exact", "expanded_95", "measured_1sigma",
				"range", "synthetic_exact"
			};
			if( allowed.find(kind) == allowed.end() ) {
				return Fail(error,"fire-simulation uncertainty kind is outside the canonical enum");
			}
			const RISECBOR64::Value* magnitude = uncertainty.Find("magnitude");
			if( !magnitude || magnitude->GetType() == RISECBOR64::Value::Null ) {
				return Fail(error,"fire-simulation uncertainty magnitude is missing");
			}
			auto readInterval = [error]( const RISECBOR64::Value& encoded ) {
				std::vector<double> interval;
				return ReadFloatArray(encoded,interval,error) && interval.size() == 2 &&
					interval[0] <= interval[1];
			};
			if( kind == "design_pinned_exact" || kind == "synthetic_exact" ) {
				double number = 0.0;
				if( !ReadNumber(*magnitude,number,error) || number != 0.0 ) {
					return Fail(error,"fire-simulation exact uncertainty is not zero");
				}
			}
			if( kind == "range" || kind == "computed_range_from_input_sensitivity" ) {
				if( !readInterval(*magnitude) ) {
					return Fail(error,"fire-simulation range uncertainty magnitude is malformed");
				}
			}
			if( kind == "assumption_bound" ) {
				double number = 0.0;
				if( magnitude->GetType() == RISECBOR64::Value::Array ) {
					if( !readInterval(*magnitude) ) {
						return Fail(error,"fire-simulation assumption-bound interval is malformed");
					}
				} else if( !ReadNumber(*magnitude,number,error) || number < 0.0 ) {
					return Fail(error,"fire-simulation assumption-bound magnitude is malformed");
				}
			}
			if( kind == "expanded_95" || kind == "measured_1sigma" ) {
				double number = 0.0;
				if( magnitude->GetType() == RISECBOR64::Value::Text ) {
					if( magnitude->GetText().compare(0,7,"column:") != 0 ||
						magnitude->GetText().size() == 7 ) {
						return Fail(error,"fire-simulation measured uncertainty column reference is malformed");
					}
				} else if( !ReadNumber(*magnitude,number,error) || number < 0.0 ) {
					return Fail(error,"fire-simulation measured uncertainty magnitude is malformed");
				}
			}
			if( kind == "assumption_bound" || kind == "computed_range_from_input_sensitivity" ) {
				std::string basis;
				if( !ReadText(uncertainty,"basis",basis,error) ) {
					return Fail(error,"fire-simulation modeled uncertainty is missing its basis");
				}
			}
			return true;
		}

		bool ValidateProvenance(
			const RISECBOR64::Value& provenance,
			std::string* error
			)
		{
			std::string citation, locator, access;
			return ReadText(provenance,"citation",citation,error) &&
				ReadText(provenance,"locator",locator,error) &&
				ReadText(provenance,"access",access,error) &&
				Required(provenance,"secondary_source",RISECBOR64::Value::Boolean,error);
		}

		bool ValidateSHA256Text(
			const RISECBOR64::Value& map,
			const char* key,
			std::string* error
			)
		{
			std::string digest;
			if( !ReadText(map,key,digest,error) || digest.size() != 64 ) {
				return Fail(error,"fire-simulation source snapshot digest is malformed");
			}
			for( const char character : digest ) {
				if( !(character >= '0' && character <= '9') &&
					!(character >= 'a' && character <= 'f') ) {
					return Fail(error,"fire-simulation source snapshot digest is not lowercase hexadecimal");
				}
			}
			return true;
		}

		bool ValidateMetadataValue(
			const RISECBOR64::Value& metadata,
			std::string* error
			)
		{
			const RISECBOR64::Value* uncertainty = Required(
				metadata,"uncertainty",RISECBOR64::Value::Map,error);
			const RISECBOR64::Value* provenance = Required(
				metadata,"provenance",RISECBOR64::Value::Map,error);
			std::string applicability, policy;
			return uncertainty && provenance &&
				ValidateUncertainty(*uncertainty,error) &&
				ValidateProvenance(*provenance,error) &&
				ReadText(metadata,"applicability",applicability,error) &&
				ReadText(metadata,"out_of_domain_policy",policy,error) && policy == "reject";
		}

		bool ValidateTableMetadata(
			const RISECBOR64::Value& owner,
			std::string* error
			)
		{
			const RISECBOR64::Value* metadata = Required(
				owner,"table_metadata",RISECBOR64::Value::Map,error);
			return metadata && ValidateMetadataValue(*metadata,error);
		}

		bool ValidateSchemaHeader(
			const RISECBOR64::Value& record,
			std::string* error
			)
		{
			const RISECBOR64::Value* version = Required(
				record,"schema_version",RISECBOR64::Value::UnsignedInteger,error);
			if( !version || version->GetIntegerArgument() != 1 ) {
				return Fail(error,"fire-simulation schema version is unsupported");
			}
			return ValidateSHA256Text(record,"source_snapshot_sha256",error);
		}

		bool ValidateMeasuredCondensedOrganics(
			const RISECBOR64::Value& record,
			std::string* error
			)
		{
			const RISECBOR64::Value* table = Required(
				record,"measured_condensed_organics",RISECBOR64::Value::Map,error);
			const RISECBOR64::Value* metadata = Required(
				record,"measured_condensed_organics_metadata",RISECBOR64::Value::Map,error);
			if( !table || !metadata || !ValidateMetadataValue(*metadata,error) ) return false;
			std::string formula, phase;
			const RISECBOR64::Value* columns = Required(
				*table,"columns",RISECBOR64::Value::Array,error);
			const RISECBOR64::Value* rows = Required(
				*table,"rows",RISECBOR64::Value::Array,error);
			if( !ReadText(*table,"formula",formula,error) || formula != "C6H10O5" ||
				!ReadText(*table,"phase",phase,error) || phase != "crystal_2" ||
				!columns || columns->GetArray().size() != 3 || !rows || rows->GetArray().empty() ) {
				return Fail(error,"fire-simulation measured condensed-organic table is malformed");
			}
			const char* expectedColumns[] = {"temperature_K", "cp_J_per_mol_K",
				"expanded_95_J_per_mol_K"};
			for( std::size_t i=0; i<3; ++i ) {
				if( columns->GetArray()[i].GetType() != RISECBOR64::Value::Text ||
					columns->GetArray()[i].GetText() != expectedColumns[i] ) {
					return Fail(error,"fire-simulation measured condensed-organic columns are malformed");
				}
			}
			double previousTemperature = 0.0;
			for( const RISECBOR64::Value& row : rows->GetArray() ) {
				std::vector<double> values;
				if( !ReadFloatArray(row,values,error) || values.size() != 3 ||
					values[0] <= previousTemperature || values[1] <= 0.0 || values[2] <= 0.0 ) {
					return Fail(error,"fire-simulation measured condensed-organic row is invalid");
				}
				previousTemperature = values[0];
			}
			std::vector<double> first, last;
			return (ReadFloatArray(rows->GetArray().front(),first,error) &&
				ReadFloatArray(rows->GetArray().back(),last,error) &&
				first[0] == 5.0 && last[0] == 370.0) ||
				Fail(error,"fire-simulation measured condensed-organic domain is not [5,370] K");
		}

		bool ReadEnvelope(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* envelope = Required(
				map,key,RISECBOR64::Value::Map,error);
			if( !envelope ) return false;
			const RISECBOR64::Value* value = envelope->Find("value");
			const RISECBOR64::Value* uncertainty = Required(
				*envelope,"uncertainty",RISECBOR64::Value::Map,error);
			const RISECBOR64::Value* provenance = Required(
				*envelope,"provenance",RISECBOR64::Value::Map,error);
			std::string applicability;
			return value && ReadNumber(*value,result,error) && uncertainty && provenance &&
				ValidateUncertainty(*uncertainty,error) &&
				ValidateProvenance(*provenance,error) &&
				ReadText(*envelope,"applicability",applicability,error);
		}

		bool ReadBlockers(
			const RISECBOR64::Value& record,
			std::vector<std::string>& blockers,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				record,"predictive_blockers",RISECBOR64::Value::Array,error);
			if( !value ) return false;
			blockers.clear();
			std::set<std::string> unique;
			for( const RISECBOR64::Value& item : value->GetArray() ) {
				if( item.GetType() != RISECBOR64::Value::Text || item.GetText().empty() ||
					!unique.insert(item.GetText()).second ) {
					return Fail(error,"fire-simulation predictive blocker list is malformed");
				}
				blockers.push_back(item.GetText());
			}
			return !blockers.empty() ||
				Fail(error,"preview-only fire-simulation record has no predictive blocker");
		}

		double CpOverR( const double* a, const double temperatureK )
		{
			return a[0]/(temperatureK*temperatureK)+a[1]/temperatureK+a[2]+
				a[3]*temperatureK+a[4]*temperatureK*temperatureK+
				a[5]*temperatureK*temperatureK*temperatureK+
				a[6]*temperatureK*temperatureK*temperatureK*temperatureK;
		}

		double CpDerivativeBound(
			const double* a,
			const double minimumK,
			const double maximumK
			)
		{
			return 2.0*std::fabs(a[0])/(minimumK*minimumK*minimumK)+
				std::fabs(a[1])/(minimumK*minimumK)+std::fabs(a[3])+
				2.0*std::fabs(a[4])*maximumK+
				3.0*std::fabs(a[5])*maximumK*maximumK+
				4.0*std::fabs(a[6])*maximumK*maximumK*maximumK;
		}

		double CertifiedCpLower(
			const FireThermochemistrySegment& segment,
			const double molecularWeight
			)
		{
			const double span = segment.temperatureMaxK-segment.temperatureMinK;
			const std::size_t intervalCount = static_cast<std::size_t>(
				std::min(65536.0,std::max(1.0,std::ceil(span))));
			double minimum = std::numeric_limits<double>::max();
			for( std::size_t interval=0; interval<intervalCount; ++interval ) {
				const double lower = segment.temperatureMinK+
					span*static_cast<double>(interval)/static_cast<double>(intervalCount);
				const double upper = interval+1 == intervalCount ? segment.temperatureMaxK :
					segment.temperatureMinK+span*static_cast<double>(interval+1)/
					static_cast<double>(intervalCount);
				const double midpoint = 0.5*(lower+upper);
				const double bound = CpOverR(segment.coefficients,midpoint)-
					CpDerivativeBound(segment.coefficients,lower,upper)*0.5*(upper-lower);
				minimum = std::min(minimum,bound*kUniversalGasConstantJPerKMolK/molecularWeight);
			}
			return std::nextafter(minimum,-std::numeric_limits<double>::infinity());
		}

		double CpAntiderivativeOverR( const double* a, const double temperatureK )
		{
			return -a[0]/temperatureK+a[1]*std::log(temperatureK)+a[2]*temperatureK+
				a[3]*temperatureK*temperatureK/2.0+
				a[4]*temperatureK*temperatureK*temperatureK/3.0+
				a[5]*temperatureK*temperatureK*temperatureK*temperatureK/4.0+
				a[6]*temperatureK*temperatureK*temperatureK*temperatureK*temperatureK/5.0;
		}

		const FireThermochemistrySegment* FindSegment(
			const FireThermochemistrySpecies& species,
			const double temperatureK
			)
		{
			for( std::size_t i=0; i<species.segments.size(); ++i ) {
				const FireThermochemistrySegment& segment = species.segments[i];
				if( temperatureK >= segment.temperatureMinK &&
					(temperatureK < segment.temperatureMaxK ||
					 (i+1 == species.segments.size() && temperatureK == segment.temperatureMaxK)) ) {
					return &segment;
				}
			}
			return 0;
		}

		std::vector<double> PCHIPSlopes(
			const std::vector<double>& x,
			const std::vector<double>& y
			)
		{
			const std::size_t count = x.size();
			std::vector<double> h(count-1), delta(count-1), slopes(count,0.0);
			for( std::size_t i=0; i+1<count; ++i ) {
				h[i] = x[i+1]-x[i];
				delta[i] = (y[i+1]-y[i])/h[i];
			}
			if( count == 2 ) {
				slopes[0] = delta[0];
				slopes[1] = delta[0];
				return slopes;
			}
			for( std::size_t i=1; i+1<count; ++i ) {
				if( delta[i-1]*delta[i] > 0.0 ) {
					const double w1 = 2.0*h[i]+h[i-1];
					const double w2 = h[i]+2.0*h[i-1];
					slopes[i] = (w1+w2)/(w1/delta[i-1]+w2/delta[i]);
				}
			}
			auto endpoint = []( const double h0, const double h1,
				const double d0, const double d1 ) {
				double value = ((2.0*h0+h1)*d0-h0*d1)/(h0+h1);
				if( value*d0 <= 0.0 ) return 0.0;
				if( d0*d1 < 0.0 && std::fabs(value) > std::fabs(3.0*d0) ) return 3.0*d0;
				return value;
			};
			slopes.front() = endpoint(h[0],h[1],delta[0],delta[1]);
			slopes.back() = endpoint(h[count-2],h[count-3],delta[count-2],delta[count-3]);
			return slopes;
		}

		bool LoadCurve(
			const RISECBOR64::Value& model,
			const std::size_t valueColumn,
			DifferentiableSpectrum& result,
			std::string* error
			)
		{
			std::string kind;
			if( !ReadText(model,"kind",kind,error) || kind != "pchip_monotone_c1_v1" ) {
				return Fail(error,"fire-simulation transport curve kind is unsupported");
			}
			const RISECBOR64::Value* encodedValueColumn = Required(
				model,"value_column",RISECBOR64::Value::UnsignedInteger,error);
			const RISECBOR64::Value* columns = Required(
				model,"columns",RISECBOR64::Value::Array,error);
			if( !encodedValueColumn || encodedValueColumn->GetIntegerArgument() != valueColumn ||
				!columns || columns->GetArray().size() != 3 ||
				columns->GetArray()[0].GetType() != RISECBOR64::Value::Text ||
				columns->GetArray()[0].GetText() != "temperature_K" ||
				columns->GetArray()[valueColumn].GetType() != RISECBOR64::Value::Text ) {
				return Fail(error,"fire-simulation transport table columns are malformed");
			}
			const char* expectedValueColumn = valueColumn == 1 ? "viscosity_Pa_s" :
				"conductivity_W_m_K";
			if( columns->GetArray()[valueColumn].GetText() != expectedValueColumn ||
				!ValidateTableMetadata(model,error) ) return false;
			const RISECBOR64::Value* rows = Required(model,"rows",RISECBOR64::Value::Array,error);
			const RISECBOR64::Value* interpolation = Required(
				model,"interpolation",RISECBOR64::Value::Map,error);
			if( !rows || !interpolation || rows->GetArray().size() < 2 ) return false;
			std::vector<double> x, y;
			for( const RISECBOR64::Value& row : rows->GetArray() ) {
				std::vector<double> fields;
				if( !ReadFloatArray(row,fields,error) || fields.size() != 3 || valueColumn >= fields.size() ||
					fields[valueColumn] <= 0.0 ) {
					return Fail(error,"fire-simulation transport table row is invalid");
				}
				x.push_back(fields[0]);
				y.push_back(fields[valueColumn]);
			}
			for( std::size_t i=1; i<x.size(); ++i ) {
				if( x[i] <= x[i-1] ) {
					return Fail(error,"fire-simulation transport temperatures are not strictly ordered");
				}
			}
			std::vector<double> slopes;
			const RISECBOR64::Value* encodedSlopes = Required(
				*interpolation,"slopes",RISECBOR64::Value::Array,error);
			if( !encodedSlopes || !ReadFloatArray(*encodedSlopes,slopes,error) || slopes.size() != x.size() ) {
				return Fail(error,"fire-simulation PCHIP slope count mismatch");
			}
			const std::vector<double> expected = PCHIPSlopes(x,y);
			for( std::size_t i=0; i<slopes.size(); ++i ) {
				const double tolerance = 64.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(expected[i]));
				if( std::fabs(slopes[i]-expected[i]) > tolerance ) {
					return Fail(error,"fire-simulation PCHIP slope does not match its table");
				}
			}
			const RISECBOR64::Value* encodedEnclosures = Required(
				*interpolation,"derivative_enclosures",RISECBOR64::Value::Array,error);
			if( !encodedEnclosures || encodedEnclosures->GetArray().size()+1 != x.size() ) {
				return Fail(error,"fire-simulation derivative enclosure count mismatch");
			}
			std::vector<SpectralDerivativeEnclosure> enclosures;
			for( std::size_t i=0; i+1<x.size(); ++i ) {
				std::vector<double> fields;
				if( !ReadFloatArray(encodedEnclosures->GetArray()[i],fields,error) || fields.size() != 4 ||
					fields[0] != x[i] || fields[1] != x[i+1] || fields[2] > fields[3] ) {
					return Fail(error,"fire-simulation derivative enclosure is invalid");
				}
				const double h = x[i+1]-x[i];
				const double a = 6.0*(y[i]-y[i+1])/h+3.0*(slopes[i]+slopes[i+1]);
				const double b = 6.0*(y[i+1]-y[i])/h-4.0*slopes[i]-2.0*slopes[i+1];
				auto derivative = [a,b,&slopes,i]( const double t ) {
					return (a*t+b)*t+slopes[i];
				};
				double verifiedMinimum = std::min(derivative(0.0),derivative(1.0));
				double verifiedMaximum = std::max(derivative(0.0),derivative(1.0));
				if( a != 0.0 ) {
					const double stationary = -b/(2.0*a);
					if( stationary > 0.0 && stationary < 1.0 ) {
						verifiedMinimum = std::min(verifiedMinimum,derivative(stationary));
						verifiedMaximum = std::max(verifiedMaximum,derivative(stationary));
					}
				}
				const double tolerance = 128.0*std::numeric_limits<double>::epsilon()*
					std::max({1.0,std::fabs(verifiedMinimum),std::fabs(verifiedMaximum)});
				if( fields[2] > verifiedMinimum+tolerance ||
					fields[3] < verifiedMaximum-tolerance ) {
					return Fail(error,"fire-simulation derivative enclosure is false");
				}
				enclosures.push_back({fields[0],fields[1],fields[2],fields[3]});
			}
			return result.Initialize(x,y,slopes,enclosures,error);
		}

		template<class Record>
		Record LoadEmbedded(
			const unsigned char* bytes,
			const std::size_t size,
			const char* expectedId
			)
		{
			Record result;
			std::string error;
			const RISECBOR64::Bytes encoded(bytes,bytes+size);
			if( !result.LoadCanonicalRecord(encoded,&error) || result.RecordId() != expectedId ) {
				return Record();
			}
			return result;
		}
	}

	FireSimulationThermochemistryRecord::FireSimulationThermochemistryRecord() :
		m_valid(false), m_temperatureMinK(0.0), m_temperatureMaxK(0.0),
		m_referenceTemperatureK(0.0)
	{
	}

	bool FireSimulationThermochemistryRecord::LoadSemanticRecord(
		const RISECBOR64::Value& record,
		std::string* error
		)
	{
		std::string kind, status, schema, version;
		double backgroundPressure = 0.0, ambientTemperature = 0.0;
		if( !ValidateSchemaHeader(record,error) ||
			!ReadText(record,"record_kind",kind,error) ||
				kind != "fire_sim_thermochemistry_property_subset" ||
			!ReadText(record,"record_name",m_recordName,error) ||
			!ReadText(record,"version",version,error) ||
			!ReadText(record,"record_status",status,error) || status != "preview_only" ||
			!ReadText(record,"provenance_schema",schema,error) ||
			schema != "fire-optics-canonical-provenance-schema-v1" ||
			!ReadDomain(record,"common_temperature_domain_K",m_temperatureMinK,m_temperatureMaxK,error) ||
			!ReadEnvelope(record,"reference_temperature_K",m_referenceTemperatureK,error) ||
			!ReadEnvelope(record,"background_pressure_Pa",backgroundPressure,error) ||
			!ReadEnvelope(record,"ambient_temperature_K",ambientTemperature,error) ||
			backgroundPressure <= 0.0 || ambientTemperature < m_temperatureMinK ||
			ambientTemperature > m_temperatureMaxK ||
			!ValidateMeasuredCondensedOrganics(record,error) ||
			!ReadBlockers(record,m_predictiveBlockers,error) ) {
			return false;
		}
		if( m_referenceTemperatureK < m_temperatureMinK ||
			m_referenceTemperatureK > m_temperatureMaxK ) {
			return Fail(error,"fire-simulation reference temperature is outside the common domain");
		}
		const RISECBOR64::Value* encodedSpecies = Required(
			record,"species",RISECBOR64::Value::Array,error);
		const RISECBOR64::Value* ambientMassFractions = Required(
			record,"ambient_mass_fractions",RISECBOR64::Value::Map,error);
		if( !encodedSpecies || encodedSpecies->GetArray().empty() ||
			encodedSpecies->GetArray().size() > 256 || !ambientMassFractions ) return false;
		m_species.clear();
		std::set<std::string> ids;
		std::size_t certificateWork = 0;
		for( const RISECBOR64::Value& encoded : encodedSpecies->GetArray() ) {
			FireThermochemistrySpecies species;
			std::string phase;
			const RISECBOR64::Value* formula = Required(
				encoded,"formula",RISECBOR64::Value::Map,error);
			if( !ReadText(encoded,"species_id",species.id,error) ||
				!ids.insert(species.id).second ||
				!ReadText(encoded,"phase",phase,error) ||
				(phase != "gas" && phase != "aerosol_solid") || !formula || formula->GetMap().empty() ||
				!ReadEnvelope(encoded,"molecular_weight_kg_per_kmol",species.molecularWeightKGPerKMol,error) ||
				!ReadEnvelope(encoded,"formation_enthalpy_J_per_kmol_298p15K",species.formationEnthalpyJPerKMol,error) ||
				species.molecularWeightKGPerKMol <= 0.0 ) {
				return Fail(error,"fire-simulation thermochemistry species metadata is invalid");
			}
			for( const auto& element : formula->GetMap() ) {
				double count = 0.0;
				if( element.first.empty() || !ReadNumber(element.second,count,error) || count <= 0.0 ) {
					return Fail(error,"fire-simulation species formula is invalid");
				}
			}
			const RISECBOR64::Value* model = Required(encoded,"cp_hs_model",RISECBOR64::Value::Map,error);
			const RISECBOR64::Value* segments = model ? Required(
				*model,"segments",RISECBOR64::Value::Array,error) : 0;
			std::string modelKind;
			double modelMinimum = 0.0, modelMaximum = 0.0, modelReference = 0.0;
			if( !model || !segments || segments->GetArray().empty() ||
				segments->GetArray().size() > 64 ||
				!ReadText(*model,"kind",modelKind,error) ||
				modelKind != "nasa9_cp_with_continuous_integrated_hs_v1" ||
				!ReadDomain(*model,"temperature_domain_K",modelMinimum,modelMaximum,error) ||
				!ReadFloat(*model,"reference_temperature_K",modelReference,error) ||
				modelMinimum != m_temperatureMinK || modelMaximum != m_temperatureMaxK ||
				modelReference != m_referenceTemperatureK ||
				!ValidateTableMetadata(*model,error) ) return false;
			for( const RISECBOR64::Value& encodedSegment : segments->GetArray() ) {
				FireThermochemistrySegment segment = {};
				std::vector<double> coefficients;
				const RISECBOR64::Value* encodedCoefficients = Required(
					encodedSegment,"coefficients",RISECBOR64::Value::Array,error);
				if( !ReadFloat(encodedSegment,"temperature_min_K",segment.temperatureMinK,error) ||
					!ReadFloat(encodedSegment,"temperature_max_K",segment.temperatureMaxK,error) ||
					!ReadFloat(encodedSegment,"hs_offset_J_per_kg",segment.sensibleEnthalpyOffsetJPerKG,error) ||
					!ReadFloat(encodedSegment,"certified_cp_lower_J_per_kg_K",segment.certifiedCpLowerJPerKGK,error) ||
					!encodedCoefficients || !ReadFloatArray(*encodedCoefficients,coefficients,error) ||
					coefficients.size() != 9 || segment.temperatureMinK >= segment.temperatureMaxK ||
					segment.temperatureMinK < m_temperatureMinK ||
					segment.temperatureMaxK > m_temperatureMaxK ||
					segment.certifiedCpLowerJPerKGK <= 0.0 ) {
					return Fail(error,"fire-simulation thermochemistry segment is invalid");
				}
				const std::size_t segmentWork = static_cast<std::size_t>(std::min(
					65536.0,std::max(1.0,std::ceil(segment.temperatureMaxK-
					segment.temperatureMinK))));
				if( certificateWork > 1000000-segmentWork ) {
					return Fail(error,"fire-simulation cp certificate work budget is exceeded");
				}
				certificateWork += segmentWork;
				for( std::size_t i=0; i<7; ++i ) segment.coefficients[i] = coefficients[i];
				const double verifiedLower = CertifiedCpLower(segment,species.molecularWeightKGPerKMol);
				const double tolerance = 128.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(verifiedLower));
				if( segment.certifiedCpLowerJPerKGK > verifiedLower+tolerance ) {
					return Fail(error,"fire-simulation cp lower-bound certificate is false");
				}
				species.segments.push_back(segment);
			}
			if( species.segments.front().temperatureMinK != m_temperatureMinK ||
				species.segments.back().temperatureMaxK != m_temperatureMaxK ) {
				return Fail(error,"fire-simulation thermochemistry species does not span the common domain");
			}
			for( std::size_t i=1; i<species.segments.size(); ++i ) {
				const FireThermochemistrySegment& left = species.segments[i-1];
				const FireThermochemistrySegment& right = species.segments[i];
				if( left.temperatureMaxK != right.temperatureMinK ) {
					return Fail(error,"fire-simulation thermochemistry segment has a gap");
				}
				const double temperature = left.temperatureMaxK;
				const double scale = kUniversalGasConstantJPerKMolK/species.molecularWeightKGPerKMol;
				const double hLeft = scale*CpAntiderivativeOverR(left.coefficients,temperature)+
					left.sensibleEnthalpyOffsetJPerKG;
				const double hRight = scale*CpAntiderivativeOverR(right.coefficients,temperature)+
					right.sensibleEnthalpyOffsetJPerKG;
				if( std::fabs(hLeft-hRight) > 1.0e-8*std::max(1.0,std::fabs(hLeft)) ) {
					return Fail(error,"fire-simulation sensible enthalpy is discontinuous");
				}
			}
			const FireThermochemistrySegment* referenceSegment = FindSegment(
				species,m_referenceTemperatureK);
			if( !referenceSegment ) return Fail(error,"fire-simulation reference segment is absent");
			const double referenceH = kUniversalGasConstantJPerKMolK/species.molecularWeightKGPerKMol*
				CpAntiderivativeOverR(referenceSegment->coefficients,m_referenceTemperatureK)+
				referenceSegment->sensibleEnthalpyOffsetJPerKG;
			if( std::fabs(referenceH) > 1.0e-8 ) {
				return Fail(error,"fire-simulation h_s(T_ref) is not zero");
			}
			m_species.push_back(species);
		}
		double ambientSum = 0.0;
		for( const auto& entry : ambientMassFractions->GetMap() ) {
			double fraction = 0.0;
			if( ids.find(entry.first) == ids.end() || !ReadNumber(entry.second,fraction,error) ||
				fraction < 0.0 ) {
				return Fail(error,"fire-simulation ambient composition is invalid");
			}
			ambientSum += fraction;
		}
		if( ambientMassFractions->GetMap().empty() ||
			std::fabs(ambientSum-1.0) > 64.0*std::numeric_limits<double>::epsilon() ) {
			return Fail(error,"fire-simulation ambient composition does not sum to one");
		}
		return true;
	}

	bool FireSimulationThermochemistryRecord::LoadCanonicalRecord(
		const RISECBOR64::Bytes& bytes,
		std::string* error
		)
	{
		FireSimulationThermochemistryRecord candidate;
		RISECBOR64::Value decoded;
		if( !RISECBOR64::DecodeCanonical(bytes,decoded,error) ||
			!candidate.LoadSemanticRecord(decoded,error) ) {
			*this = FireSimulationThermochemistryRecord();
			return false;
		}
		candidate.m_recordBytes = bytes;
		candidate.m_recordId = RISECBOR64::SHA256Hex(bytes);
		candidate.m_valid = true;
		*this = candidate;
		return true;
	}

	const FireSimulationThermochemistryRecord& FireSimulationThermochemistryRecord::OpenSubsetV1()
	{
		static const FireSimulationThermochemistryRecord record =
			LoadEmbedded<FireSimulationThermochemistryRecord>(
				kFireSimThermochemistryOpenV1,kFireSimThermochemistryOpenV1Size,
				kFireSimThermochemistryOpenV1SHA256);
		return record;
	}

	const FireThermochemistrySpecies* FireSimulationThermochemistryRecord::FindSpecies(
		const char* id
		) const
	{
		if( !m_valid || !id ) return 0;
		for( const FireThermochemistrySpecies& species : m_species ) {
			if( species.id == id ) return &species;
		}
		return 0;
	}

	bool FireSimulationThermochemistryRecord::CpJPerKGK(
		const char* speciesId,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		const FireThermochemistrySpecies* species = FindSpecies(speciesId);
		const FireThermochemistrySegment* segment = species ? FindSegment(*species,temperatureK) : 0;
		if( !m_valid || !species ) return Fail(error,"unknown fire-simulation thermochemistry species");
		if( !segment ) return Fail(error,"fire-simulation thermochemistry lookup is out of domain");
		result = CpOverR(segment->coefficients,temperatureK)*
			kUniversalGasConstantJPerKMolK/species->molecularWeightKGPerKMol;
		return (std::isfinite(result) && result >= segment->certifiedCpLowerJPerKGK) ||
			Fail(error,"fire-simulation cp evaluation violated its certificate");
	}

	bool FireSimulationThermochemistryRecord::SensibleEnthalpyJPerKG(
		const char* speciesId,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		const FireThermochemistrySpecies* species = FindSpecies(speciesId);
		const FireThermochemistrySegment* segment = species ? FindSegment(*species,temperatureK) : 0;
		if( !m_valid || !species ) return Fail(error,"unknown fire-simulation thermochemistry species");
		if( !segment ) return Fail(error,"fire-simulation thermochemistry lookup is out of domain");
		result = kUniversalGasConstantJPerKMolK/species->molecularWeightKGPerKMol*
			CpAntiderivativeOverR(segment->coefficients,temperatureK)+
			segment->sensibleEnthalpyOffsetJPerKG;
		return std::isfinite(result) || Fail(error,"fire-simulation h_s evaluation is non-finite");
	}

	bool FireSimulationThermochemistryRecord::MixtureSensibleEnergyJPerM3(
		const std::vector<std::pair<std::string,double> >& massDensities,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		result = 0.0;
		double totalMass = 0.0;
		std::set<std::string> ids;
		for( const auto& entry : massDensities ) {
			if( entry.second < 0.0 || !std::isfinite(entry.second) ||
				!ids.insert(entry.first).second ) {
				return Fail(error,"fire-simulation mixture mass densities are invalid");
			}
			double sensibleEnthalpy = 0.0;
			if( !SensibleEnthalpyJPerKG(entry.first.c_str(),temperatureK,sensibleEnthalpy,error) ) {
				return false;
			}
			result += entry.second*sensibleEnthalpy;
			totalMass += entry.second;
			if( !std::isfinite(result) || !std::isfinite(totalMass) ) {
				return Fail(error,"fire-simulation mixture accumulation overflowed");
			}
		}
		return (totalMass > 0.0 && std::isfinite(result)) ||
			Fail(error,"fire-simulation mixture is empty or non-finite");
	}

	bool FireSimulationThermochemistryRecord::InvertMixtureTemperatureK(
		const std::vector<std::pair<std::string,double> >& massDensities,
		const double sensibleEnergy,
		double& result,
		std::string* error
		) const
	{
		double lowEnergy = 0.0, highEnergy = 0.0;
		if( !std::isfinite(sensibleEnergy) ||
			!MixtureSensibleEnergyJPerM3(massDensities,m_temperatureMinK,lowEnergy,error) ||
			!MixtureSensibleEnergyJPerM3(massDensities,m_temperatureMaxK,highEnergy,error) ) return false;
		if( sensibleEnergy < lowEnergy || sensibleEnergy > highEnergy ) {
			return Fail(error,"fire-simulation sensible energy is outside the certified inversion bracket");
		}
		double lower = m_temperatureMinK;
		double upper = m_temperatureMaxK;
		for( unsigned int iteration=0; iteration<96; ++iteration ) {
			const double midpoint = 0.5*(lower+upper);
			double midpointEnergy = 0.0;
			if( !MixtureSensibleEnergyJPerM3(massDensities,midpoint,midpointEnergy,error) ) return false;
			if( midpointEnergy < sensibleEnergy ) lower = midpoint;
			else upper = midpoint;
		}
		result = 0.5*(lower+upper);
		return true;
	}

	FireSimulationTransportRecord::FireSimulationTransportRecord() :
		m_valid(false), m_temperatureMinK(0.0), m_temperatureMaxK(0.0),
		m_turbulentPrandtl(0.0), m_turbulentSchmidt(0.0), m_vremanCv(0.0),
		m_vremanCnu(0.0), m_chemicalTimeS(0.0), m_criticalFlameTemperatureK(0.0)
	{
	}

	bool FireSimulationTransportRecord::LoadSemanticRecord(
		const RISECBOR64::Value& record,
		std::string* error
		)
	{
		std::string kind, status, schema, version, viscosityMix, conductivityMix;
		std::string lewisLaw, wallStress, wallHeatFlux, filterWidths;
		if( !ValidateSchemaHeader(record,error) ||
			!ReadText(record,"record_kind",kind,error) || kind != "fire_sim_transport_closure" ||
			!ReadText(record,"record_name",m_recordName,error) ||
			!ReadText(record,"version",version,error) ||
			!ReadText(record,"record_status",status,error) || status != "preview_only" ||
			!ReadText(record,"provenance_schema",schema,error) ||
			schema != "fire-optics-canonical-provenance-schema-v1" ||
			!ReadText(record,"viscosity_mixing_law",viscosityMix,error) || viscosityMix != "wilke_v1" ||
			!ReadText(record,"conductivity_mixing_law",conductivityMix,error) ||
				conductivityMix != "wassiljewa_mason_saxena_v1" ||
			!ReadText(record,"unit_lewis_diffusivity",lewisLaw,error) ||
				lewisLaw != "D_mol=k_mol/(rho_g*cp_g)" ||
			!ReadText(record,"wall_stress",wallStress,error) ||
				wallStress != "resolved_molecular_no_slip" ||
			!ReadText(record,"wall_heat_flux",wallHeatFlux,error) || wallHeatFlux != "adiabatic" ||
			!ReadText(record,"filter_widths",filterWidths,error) ||
				filterWidths != "directional_mac_cell_widths" ||
			!ReadDomain(record,"common_temperature_domain_K",m_temperatureMinK,m_temperatureMaxK,error) ||
			!ReadBlockers(record,m_predictiveBlockers,error) ) return false;
		if( !ReadEnvelope(record,"turbulent_prandtl",m_turbulentPrandtl,error) ||
			!ReadEnvelope(record,"turbulent_schmidt",m_turbulentSchmidt,error) ||
			!ReadEnvelope(record,"vreman_Cv",m_vremanCv,error) ||
			!ReadEnvelope(record,"vreman_Cnu",m_vremanCnu,error) ||
			!ReadEnvelope(record,"tau_chem_s",m_chemicalTimeS,error) ||
			!ReadEnvelope(record,"critical_flame_temperature_K",m_criticalFlameTemperatureK,error) ||
			m_turbulentPrandtl <= 0.0 || m_turbulentSchmidt <= 0.0 || m_vremanCv <= 0.0 ||
			m_vremanCnu <= 0.0 || m_chemicalTimeS <= 0.0 ||
			m_criticalFlameTemperatureK <= 0.0 ) return false;
		const RISECBOR64::Value* encodedSpecies = Required(
			record,"species",RISECBOR64::Value::Array,error);
		if( !encodedSpecies || encodedSpecies->GetArray().empty() ) return false;
		m_species.clear();
		std::set<std::string> ids;
		for( const RISECBOR64::Value& encoded : encodedSpecies->GetArray() ) {
			FireTransportSpecies species;
			const RISECBOR64::Value* viscosity = Required(encoded,"viscosity_model",RISECBOR64::Value::Map,error);
			const RISECBOR64::Value* conductivity = Required(encoded,"conductivity_model",RISECBOR64::Value::Map,error);
			if( !ReadText(encoded,"species_id",species.id,error) || !ids.insert(species.id).second ||
				!viscosity || !conductivity ||
				!LoadCurve(*viscosity,1,species.viscosity,error) ||
				!LoadCurve(*conductivity,2,species.conductivity,error) ||
				species.viscosity.Wavelengths().front() != m_temperatureMinK ||
				species.viscosity.Wavelengths().back() != m_temperatureMaxK ||
				species.conductivity.Wavelengths() != species.viscosity.Wavelengths() ) {
				return Fail(error,"fire-simulation transport species is invalid");
			}
			m_species.push_back(species);
		}
		return true;
	}

	bool FireSimulationTransportRecord::LoadCanonicalRecord(
		const RISECBOR64::Bytes& bytes,
		std::string* error
		)
	{
		FireSimulationTransportRecord candidate;
		RISECBOR64::Value decoded;
		if( !RISECBOR64::DecodeCanonical(bytes,decoded,error) ||
			!candidate.LoadSemanticRecord(decoded,error) ) {
			*this = FireSimulationTransportRecord();
			return false;
		}
		candidate.m_recordBytes = bytes;
		candidate.m_recordId = RISECBOR64::SHA256Hex(bytes);
		candidate.m_valid = true;
		*this = candidate;
		return true;
	}

	const FireSimulationTransportRecord& FireSimulationTransportRecord::OpenV1()
	{
		static const FireSimulationTransportRecord record =
			LoadEmbedded<FireSimulationTransportRecord>(
				kFireSimTransportOpenV1,kFireSimTransportOpenV1Size,
				kFireSimTransportOpenV1SHA256);
		return record;
	}

	const FireTransportSpecies* FireSimulationTransportRecord::FindSpecies(
		const char* id
		) const
	{
		if( !m_valid || !id ) return 0;
		for( const FireTransportSpecies& species : m_species ) {
			if( species.id == id ) return &species;
		}
		return 0;
	}

	bool FireSimulationTransportRecord::ViscosityPaS(
		const char* speciesId,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		const FireTransportSpecies* species = FindSpecies(speciesId);
		if( !m_valid || !species ) return Fail(error,"unknown fire-simulation transport species");
		if( !species->viscosity.Contains(temperatureK) ) {
			return Fail(error,"fire-simulation viscosity lookup is out of domain");
		}
		result = species->viscosity.Evaluate(temperatureK);
		return (result > 0.0 && std::isfinite(result)) || Fail(error,"fire-simulation viscosity is invalid");
	}

	bool FireSimulationTransportRecord::ConductivityWPerMK(
		const char* speciesId,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		const FireTransportSpecies* species = FindSpecies(speciesId);
		if( !m_valid || !species ) return Fail(error,"unknown fire-simulation transport species");
		if( !species->conductivity.Contains(temperatureK) ) {
			return Fail(error,"fire-simulation conductivity lookup is out of domain");
		}
		result = species->conductivity.Evaluate(temperatureK);
		return (result > 0.0 && std::isfinite(result)) || Fail(error,"fire-simulation conductivity is invalid");
	}

	namespace
	{
		bool MoleFractions(
			const std::vector<std::pair<std::string,double> >& massFractions,
			const FireSimulationThermochemistryRecord& thermochemistry,
			std::vector<std::string>& ids,
			std::vector<double>& molecularWeights,
			std::vector<double>& moleFractions,
			std::string* error
			)
		{
			if( !thermochemistry.IsValid() ) {
				return Fail(error,"fire-simulation transport requires valid thermochemistry");
			}
			double moleTotal = 0.0;
			std::set<std::string> unique;
			for( const auto& entry : massFractions ) {
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(entry.first.c_str());
				if( !species || entry.second < 0.0 || !std::isfinite(entry.second) ||
					!unique.insert(entry.first).second ) {
					return Fail(error,"fire-simulation transport composition is invalid");
				}
				ids.push_back(entry.first);
				molecularWeights.push_back(species->molecularWeightKGPerKMol);
				moleFractions.push_back(entry.second/species->molecularWeightKGPerKMol);
				moleTotal += moleFractions.back();
			}
			if( moleTotal <= 0.0 ) return Fail(error,"fire-simulation transport composition is empty");
			for( double& fraction : moleFractions ) fraction /= moleTotal;
			return true;
		}

		double WilkePhi(
			const double propertyI,
			const double propertyJ,
			const double molecularWeightI,
			const double molecularWeightJ
			)
		{
			const double numerator = 1.0+std::sqrt(propertyI/propertyJ)*
				std::pow(molecularWeightJ/molecularWeightI,0.25);
			return numerator*numerator/std::sqrt(8.0*(1.0+molecularWeightI/molecularWeightJ));
		}
	}

	bool FireSimulationTransportRecord::MixtureViscosityPaS(
		const std::vector<std::pair<std::string,double> >& massFractions,
		const FireSimulationThermochemistryRecord& thermochemistry,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		std::vector<std::string> ids;
		std::vector<double> weights, x, values;
		if( !MoleFractions(massFractions,thermochemistry,ids,weights,x,error) ) return false;
		for( const std::string& id : ids ) {
			double value = 0.0;
			if( !ViscosityPaS(id.c_str(),temperatureK,value,error) ) return false;
			values.push_back(value);
		}
		result = 0.0;
		for( std::size_t i=0; i<x.size(); ++i ) {
			double denominator = 0.0;
			for( std::size_t j=0; j<x.size(); ++j ) {
				denominator += x[j]*WilkePhi(values[i],values[j],weights[i],weights[j]);
			}
			result += x[i]*values[i]/denominator;
		}
		return (result > 0.0 && std::isfinite(result)) || Fail(error,"fire-simulation Wilke mixture viscosity is invalid");
	}

	bool FireSimulationTransportRecord::MixtureConductivityWPerMK(
		const std::vector<std::pair<std::string,double> >& massFractions,
		const FireSimulationThermochemistryRecord& thermochemistry,
		const double temperatureK,
		double& result,
		std::string* error
		) const
	{
		std::vector<std::string> ids;
		std::vector<double> weights, x, viscosity, conductivity;
		if( !MoleFractions(massFractions,thermochemistry,ids,weights,x,error) ) return false;
		for( const std::string& id : ids ) {
			double mu = 0.0, k = 0.0;
			if( !ViscosityPaS(id.c_str(),temperatureK,mu,error) ||
				!ConductivityWPerMK(id.c_str(),temperatureK,k,error) ) return false;
			viscosity.push_back(mu);
			conductivity.push_back(k);
		}
		result = 0.0;
		for( std::size_t i=0; i<x.size(); ++i ) {
			double denominator = 0.0;
			for( std::size_t j=0; j<x.size(); ++j ) {
				denominator += x[j]*WilkePhi(viscosity[i],viscosity[j],weights[i],weights[j]);
			}
			result += x[i]*conductivity[i]/denominator;
		}
		return (result > 0.0 && std::isfinite(result)) ||
			Fail(error,"fire-simulation WMS mixture conductivity is invalid");
	}
}
