//////////////////////////////////////////////////////////////////////
//
//  FireOptics.cpp - Versioned fire constituent optical records
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 6, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireOptics.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace RISE
{
	namespace
	{
#include "FireOpticsRecordData.inc"

		const double kPi = 3.141592653589793238462643383279502884;
		const char kPhiConsistency[] =
			"the hot-soot and cool-carbon records must carry bit-identical phi_T fields at record freeze (one optical record in the final SS8 CBOR encoding)";
		const char kPhiForm[] =
			"phi(T) = smoothstep over T in [700, 900] K: 0 below, 1 above. FIRE_SMOKE_DESIGN.md SS3.4 says 'smoothstep' unqualified; the cubic Hermite 3t^2-2t^3 (C1) is the assumed canonical form - if the SS3.5 backward-Euler solver requires C2 (smootherstep), that is a design-loop question, not an implementation choice";
		const char kPhiProvenance[] =
			"FIRE_SMOKE_DESIGN.md SS3.4 (r45+; lines ~807-808): design-owned closure parameter of the derived hot/cool carbon partition c_hot = phi(T)*c_carbon. NOT a literature constant - its authority is the design decision itself. SS8 requires the phi(T) band in the versioned optical record; this field closes that gap in the draft layer (added 2026-08-06 at the implementation agent's provenance stop).";
		const char kCoolOutOfDomainPolicy[] =
			"NO preview extrapolation rule is named for this record. Per SS8, out-of-domain lookups therefore REJECT in both predictive and preview modes. The certified [380,780] matches the renderer's NM band exactly, so no in-band lookup can be out-of-domain; IR (Kirchhoff/SS3.5) use remains separately blocked per the existing gaps entry.";
		const char kProvenanceSchemaSHA256[] =
			"6ec2e55262c519904625764c65f5d6934d942276301c15cde30eaafe376db159";

		bool Fail( std::string* error, const std::string& message )
		{
			if( error ) {
				*error = message;
			}
			return false;
		}

		bool IsFinite( const double value )
		{
			return std::isfinite(value) != 0;
		}

		bool NearlyEqual( const double a, const double b, const double tolerance )
		{
			return std::fabs(a-b) <= tolerance;
		}

		double MaximumValue( const std::vector<double>& values )
		{
			return *std::max_element(values.begin(), values.end());
		}

		double MinimumValue( const std::vector<double>& values )
		{
			return *std::min_element(values.begin(), values.end());
		}

		const RISECBOR64::Value* Required(
			const RISECBOR64::Value& map,
			const char* key,
			const RISECBOR64::Value::Type type,
			std::string* error
			)
		{
			if( map.GetType() != RISECBOR64::Value::Map ) {
				Fail(error, "fire-optics semantic value is not a map");
				return 0;
			}
			const RISECBOR64::Value* value = map.Find(key);
			if( !value ) {
				Fail(error, std::string("fire-optics record is missing '")+key+"'");
				return 0;
			}
			if( value->GetType() != type ) {
				Fail(error, std::string("fire-optics field '")+key+"' has the wrong type");
				return 0;
			}
			return value;
		}

		bool ReadFloat(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				map, key, RISECBOR64::Value::Float64, error );
			if( !value ) {
				return false;
			}
			result = value->GetFloat();
			return IsFinite(result) || Fail(error, std::string("non-finite '")+key+"'");
		}

		bool ReadNumberValue(
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
				return Fail(error,"fire-optics source value is not numeric");
			}
			return IsFinite(result) || Fail(error,"fire-optics source value is non-finite");
		}

		bool ReadSourceNumber(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = map.Find(key);
			return value ? ReadNumberValue(*value,result,error) :
				Fail(error,std::string("fire-optics source is missing '")+key+"'");
		}

		bool ValidateSourceEnvelope(
			const RISECBOR64::Value& envelope,
			double& value,
			std::string* error
			);

		bool ReadSourceEnvelopeNumber(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* envelope = Required(
				map,key,RISECBOR64::Value::Map,error );
			return envelope && ValidateSourceEnvelope(*envelope,result,error);
		}

		bool ReadText(
			const RISECBOR64::Value& map,
			const char* key,
			std::string& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* value = Required(
				map, key, RISECBOR64::Value::Text, error );
			if( !value ) {
				return false;
			}
			result = value->GetText();
			return true;
		}

		bool IsExactZero( const RISECBOR64::Value& value )
		{
			return (value.GetType() == RISECBOR64::Value::UnsignedInteger &&
				value.GetIntegerArgument() == 0) ||
				(value.GetType() == RISECBOR64::Value::Float64 &&
				value.GetFloat() == 0.0);
		}

		bool ValidateUncertainty(
			const RISECBOR64::Value& uncertainty,
			const char* requiredKind,
			std::string* error
			)
		{
			std::string kind;
			if( !ReadText(uncertainty, "kind", kind, error) ) {
				return false;
			}
			const char* allowedKinds[] = {
				"assumption_bound", "computed_range_from_input_sensitivity",
				"design_pinned_exact", "expanded_95", "measured_1sigma",
				"range", "synthetic_exact"
			};
			bool allowed = false;
			for( std::size_t i=0; i<sizeof(allowedKinds)/sizeof(allowedKinds[0]); ++i ) {
				allowed = allowed || kind == allowedKinds[i];
			}
			if( !allowed || (requiredKind && kind != requiredKind) ) {
				return Fail(error, "fire-optics uncertainty kind is not schema draft-2");
			}
			const RISECBOR64::Value* magnitude = uncertainty.Find("magnitude");
			if( !magnitude || magnitude->GetType() == RISECBOR64::Value::Null ||
				magnitude->GetType() == RISECBOR64::Value::Map ||
				magnitude->GetType() == RISECBOR64::Value::Boolean ||
				magnitude->GetType() == RISECBOR64::Value::ByteString ) {
				return Fail(error, "fire-optics uncertainty magnitude is missing");
			}
			if( (kind == "design_pinned_exact" || kind == "synthetic_exact") &&
				!IsExactZero(*magnitude) ) {
				return Fail(error, "fire-optics exact uncertainty magnitude is not zero");
			}
			if( kind == "assumption_bound" &&
				!Required(uncertainty, "basis", RISECBOR64::Value::Text, error) ) {
				return Fail(error, "fire-optics assumption bound is missing its basis");
			}
			return true;
		}

		bool ValidateFixtureEnvelope(
			const RISECBOR64::Value& envelope,
			double& value,
			std::string* error
			)
		{
			const RISECBOR64::Value* uncertainty = Required(
				envelope, "uncertainty", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* provenance = Required(
				envelope, "provenance", RISECBOR64::Value::Map, error );
			if( !uncertainty || !provenance ||
				!ReadFloat(envelope, "value", value, error) ||
				!Required(envelope, "applicability", RISECBOR64::Value::Text, error) ||
				!Required(*provenance, "citation", RISECBOR64::Value::Text, error) ||
				!Required(*provenance, "locator", RISECBOR64::Value::Text, error) ||
				!Required(*provenance, "access", RISECBOR64::Value::Text, error) ||
				!Required(*provenance, "secondary_source",
					RISECBOR64::Value::Boolean, error) ||
				!ValidateUncertainty(*uncertainty, "synthetic_exact", error) ) {
				return Fail(error, "fire-optics synthetic fixture metadata envelope is incomplete");
			}
			return true;
		}

		bool ValidateSourceEnvelope(
			const RISECBOR64::Value& envelope,
			double& value,
			std::string* error
			)
		{
			const RISECBOR64::Value* uncertainty = Required(
				envelope,"uncertainty",RISECBOR64::Value::Map,error );
			const RISECBOR64::Value* provenance = Required(
				envelope,"provenance",RISECBOR64::Value::Map,error );
			if( !uncertainty || !provenance ||
				!ReadSourceNumber(envelope,"value",value,error) ||
				!Required(envelope,"applicability",RISECBOR64::Value::Text,error) ||
				!Required(*provenance,"citation",RISECBOR64::Value::Text,error) ||
				!Required(*provenance,"locator",RISECBOR64::Value::Text,error) ||
				!Required(*provenance,"access",RISECBOR64::Value::Text,error) ||
				!Required(*provenance,"secondary_source",
					RISECBOR64::Value::Boolean,error) ||
				!ValidateUncertainty(*uncertainty,0,error) ) {
				return Fail(error,
					"fire-optics source scalar metadata envelope is incomplete");
			}
			return true;
		}

		bool ReadEnvelopedFixtureFloat(
			const RISECBOR64::Value& map,
			const char* key,
			double& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* envelope = Required(
				map, key, RISECBOR64::Value::Map, error );
			return envelope && ValidateFixtureEnvelope(*envelope, result, error);
		}

		bool ValidateTableMetadata(
			const RISECBOR64::Value& metadata,
			std::string* error
			)
		{
			std::string granularity;
			const RISECBOR64::Value* uncertainty = Required(
				metadata, "uncertainty", RISECBOR64::Value::Map, error );
			if( !ReadText(metadata, "granularity", granularity, error) ||
				granularity != "per_table" ||
				!Required(metadata, "applicability", RISECBOR64::Value::Text, error) ||
				!Required(metadata, "provenance", RISECBOR64::Value::Text, error) ||
				!uncertainty || !ValidateUncertainty(*uncertainty, 0, error) ) {
				return Fail(error, "fire-optics table metadata is incomplete");
			}
			return true;
		}

		bool CanonicallyEqual(
			const RISECBOR64::Value& a,
			const RISECBOR64::Value& b,
			std::string* error
			)
		{
			RISECBOR64::Bytes encodedA, encodedB;
			return RISECBOR64::Encode(a, encodedA, error) &&
				RISECBOR64::Encode(b, encodedB, error) && encodedA == encodedB;
		}

		bool MembersCanonicallyEqual(
			const RISECBOR64::Value& a,
			const char* aKey,
			const RISECBOR64::Value& b,
			const char* bKey,
			std::string* error
			)
		{
			const RISECBOR64::Value* aValue = Required(
				a, aKey, RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* bValue = Required(
				b, bKey, RISECBOR64::Value::Map, error );
			return aValue && bValue && CanonicallyEqual(*aValue, *bValue, error);
		}

		bool ValidateSourceRecordHeader(
			const RISECBOR64::Value& source,
			std::string* error
			)
		{
			return Required(source, "schema_version", RISECBOR64::Value::Text, error) &&
				Required(source, "record_kind", RISECBOR64::Value::Text, error) &&
				Required(source, "record_name", RISECBOR64::Value::Text, error) &&
				Required(source, "version", RISECBOR64::Value::Text, error) &&
				Required(source, "record_status", RISECBOR64::Value::Text, error);
		}

		bool ValidateProvenanceSchema(
			const RISECBOR64::Value& schema,
			std::string* error
			)
		{
			std::string name, version, granularity;
			const RISECBOR64::Value* kinds = Required(
				schema, "uncertainty_kind_enum", RISECBOR64::Value::Map, error );
			if( !ReadText(schema, "record_name", name, error) ||
				name != "fire-optics-canonical-provenance-schema-v1" ||
				!ReadText(schema, "schema_version", version, error) ||
				version != "draft-2" ||
				!ReadText(schema, "table_metadata_granularity", granularity, error) ||
				!Required(schema, "aggregate_required", RISECBOR64::Value::Text, error) ||
				!kinds ) {
				return Fail(error, "fire-optics canonical provenance schema is not draft-2");
			}
			const char* requiredKinds[] = {
				"assumption_bound", "computed_range_from_input_sensitivity",
				"design_pinned_exact", "expanded_95", "measured_1sigma",
				"range", "synthetic_exact"
			};
			for( std::size_t i=0; i<sizeof(requiredKinds)/sizeof(requiredKinds[0]); ++i ) {
				if( !Required(*kinds, requiredKinds[i], RISECBOR64::Value::Text, error) ) {
					return Fail(error, "fire-optics canonical uncertainty enum is incomplete");
				}
			}
			RISECBOR64::Bytes encoded;
			if( !RISECBOR64::Encode(schema,encoded,error) ||
				RISECBOR64::SHA256Hex(encoded) != kProvenanceSchemaSHA256 ) {
				return Fail(error,
					"fire-optics canonical provenance schema does not match pinned draft-2");
			}
			return true;
		}

		bool SourceStatusMatches(
			const RISECBOR64::Value& componentStatuses,
			const char* component,
			const RISECBOR64::Value& source,
			std::string* error
			)
		{
			std::string declared, actual;
			return ReadText(componentStatuses, component, declared, error) &&
				ReadText(source, "record_status", actual, error) &&
				(declared == actual ||
					Fail(error, "fire-optics component record status does not match its source"));
		}

		bool ReadFloatArray(
			const RISECBOR64::Value& map,
			const char* key,
			std::vector<double>& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* array = Required(
				map, key, RISECBOR64::Value::Array, error );
			if( !array ) {
				return false;
			}
			result.clear();
			for( std::size_t i=0; i<array->GetArray().size(); i++ ) {
				const RISECBOR64::Value& value = array->GetArray()[i];
				if( value.GetType() != RISECBOR64::Value::Float64 ||
					!IsFinite(value.GetFloat()) ) {
					return Fail(error, std::string("fire-optics array '")+key+"' is not binary64");
				}
				result.push_back(value.GetFloat());
			}
			return true;
		}

		bool ReadDomain(
			const RISECBOR64::Value& map,
			double& minimum,
			double& maximum,
			std::string* error
			)
		{
			std::vector<double> values;
			if( !ReadFloatArray(map, "domain_nm", values, error) || values.size() != 2 ) {
				return Fail(error, "fire-optics domain_nm must have two values");
			}
			minimum = values[0];
			maximum = values[1];
			return minimum < maximum || Fail(error, "fire-optics domain is not increasing");
		}

		bool ReadPhiPartition(
			const RISECBOR64::Value& hot,
			const RISECBOR64::Value& cool,
			double& minimumTemperature,
			double& maximumTemperature,
			std::string* error
			)
		{
			const RISECBOR64::Value* hotPhi = Required(
				hot, "phi_T_partition", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* coolPhi = Required(
				cool, "phi_T_partition", RISECBOR64::Value::Map, error );
			if( !hotPhi || !coolPhi ) {
				return false;
			}
			RISECBOR64::Bytes hotBytes, coolBytes;
			if( !RISECBOR64::Encode(*hotPhi, hotBytes, error) ||
				!RISECBOR64::Encode(*coolPhi, coolBytes, error) ||
				hotBytes != coolBytes ) {
				return Fail(error,
					"hot-soot and cool-carbon phi_T_partition fields differ");
			}
			const RISECBOR64::Value* uncertainty = Required(
				*hotPhi, "uncertainty", RISECBOR64::Value::Map, error );
			std::string form, consistencyRequirement, provenance;
			std::vector<double> band;
			if( !uncertainty ||
				!ValidateUncertainty(*uncertainty, "design_pinned_exact", error) ||
				!ReadText(*hotPhi, "form", form, error) ||
				!ReadText(*hotPhi, "consistency_requirement", consistencyRequirement, error) ||
				!ReadText(*hotPhi, "provenance", provenance, error) ||
				consistencyRequirement != kPhiConsistency ||
				form != kPhiForm || provenance != kPhiProvenance ||
				!ReadFloatArray(*hotPhi, "hot_fraction_temperature_band_K", band, error) ||
				band.size() != 2 || band[0] >= band[1] ) {
				return Fail(error,
					"fire-optics phi_T_partition is not cubic-Hermite C1 smoothstep");
			}
			minimumTemperature = band[0];
			maximumTemperature = band[1];
			return true;
		}

		bool ReadRows(
			const RISECBOR64::Value& map,
			const std::size_t columnCount,
			std::vector<std::vector<double> >& rows,
			std::string* error
			)
		{
			const RISECBOR64::Value* encodedRows = Required(
				map, "rows", RISECBOR64::Value::Array, error );
			if( !encodedRows ) {
				return false;
			}
			rows.clear();
			for( std::size_t r=0; r<encodedRows->GetArray().size(); r++ ) {
				const RISECBOR64::Value& encodedRow = encodedRows->GetArray()[r];
				if( encodedRow.GetType() != RISECBOR64::Value::Array ||
					encodedRow.GetArray().size() != columnCount ) {
					return Fail(error, "fire-optics table row has the wrong width");
				}
				std::vector<double> row;
				for( std::size_t c=0; c<columnCount; c++ ) {
					const RISECBOR64::Value& value = encodedRow.GetArray()[c];
					if( value.GetType() != RISECBOR64::Value::Float64 ||
						!IsFinite(value.GetFloat()) ) {
						return Fail(error, "fire-optics table contains a non-binary64 value");
					}
					row.push_back(value.GetFloat());
				}
				rows.push_back(row);
			}
			return true;
		}

		bool ReadSourceRows(
			const RISECBOR64::Value& map,
			const std::size_t columnCount,
			std::vector<std::vector<double> >& rows,
			std::string* error
			)
		{
			const RISECBOR64::Value* encodedRows = Required(
				map,"rows",RISECBOR64::Value::Array,error );
			if( !encodedRows ) return false;
			rows.clear();
			for( const RISECBOR64::Value& encodedRow : encodedRows->GetArray() ) {
				if( encodedRow.GetType() != RISECBOR64::Value::Array ||
					encodedRow.GetArray().size() != columnCount ) {
					return Fail(error,"fire-optics source table row has the wrong width");
				}
				std::vector<double> row;
				for( const RISECBOR64::Value& encodedValue : encodedRow.GetArray() ) {
					double value = 0.0;
					if( !ReadNumberValue(encodedValue,value,error) ) return false;
					row.push_back(value);
				}
				rows.push_back(row);
			}
			return true;
		}

		bool RowsExactlyEqual(
			const std::vector<std::vector<double> >& operational,
			const std::vector<std::vector<double> >& source
			)
		{
			return operational == source;
		}

		bool ReadSourceNumberArray(
			const RISECBOR64::Value& map,
			const char* key,
			std::vector<double>& values,
			std::string* error
			)
		{
			const RISECBOR64::Value* array = Required(
				map,key,RISECBOR64::Value::Array,error );
			if( !array ) return false;
			values.clear();
			for( const RISECBOR64::Value& encoded : array->GetArray() ) {
				double value = 0.0;
				if( !ReadNumberValue(encoded,value,error) ) return false;
				values.push_back(value);
			}
			return true;
		}

		bool ValidateColumns(
			const RISECBOR64::Value& table,
			const char* const* expected,
			const std::size_t count,
			const char* tableName,
			std::string* error
			)
		{
			const RISECBOR64::Value* columns = Required(
				table,"columns",RISECBOR64::Value::Array,error );
			if( !columns || columns->GetArray().size() != count ) {
				return Fail(error,std::string("fire-optics ")+tableName+
					" columns do not match the frozen unit-bearing schema");
			}
			for( std::size_t i=0; i<count; ++i ) {
				const RISECBOR64::Value& column = columns->GetArray()[i];
				if( column.GetType() != RISECBOR64::Value::Text ||
					column.GetText() != expected[i] ) {
					return Fail(error,std::string("fire-optics ")+tableName+
						" columns do not match the frozen unit-bearing schema");
				}
			}
			return true;
		}

		bool PhiSourceMatchesOperational(
			const RISECBOR64::Value& source,
			const RISECBOR64::Value& operational,
			std::string* error
			)
		{
			std::string sourceForm, sourceConsistency, sourceProvenance;
			std::string operationalForm, operationalConsistency, operationalProvenance;
			std::vector<double> sourceBand, operationalBand;
			if( !ReadText(source,"form",sourceForm,error) ||
				!ReadText(source,"consistency_requirement",sourceConsistency,error) ||
				!ReadText(source,"provenance",sourceProvenance,error) ||
				!ReadSourceNumberArray(source,"hot_fraction_temperature_band_K",
					sourceBand,error) ||
				!ReadText(operational,"form",operationalForm,error) ||
				!ReadText(operational,"consistency_requirement",operationalConsistency,error) ||
				!ReadText(operational,"provenance",operationalProvenance,error) ||
				!ReadSourceNumberArray(operational,"hot_fraction_temperature_band_K",
					operationalBand,error) ||
				sourceForm != operationalForm ||
				sourceConsistency != operationalConsistency ||
				sourceProvenance != operationalProvenance ||
				sourceBand != operationalBand ) {
				return Fail(error,
					"fire-optics operational phi_T_partition differs from its source record");
			}
			return true;
		}

		bool ValidatePredictiveSourceBindings(
			const RISECBOR64::Value& sourceEffective,
			const RISECBOR64::Value& sourceHot,
			const RISECBOR64::Value& sourceCool,
			const RISECBOR64::Value& sourceCondensed,
			const std::vector<std::vector<double> >& effectiveRows,
			const double density,
			const std::vector<std::vector<double> >& hotRows,
			const double coolKm,
			const double coolExponent,
			const double coolOmega,
			const double coolG,
			const std::vector<double>& supportedRange,
			const std::vector<double>& certifiedDomain,
			const std::string& outOfDomainPolicy,
			const std::vector<std::vector<double> >& condensedRows,
			const double condensedExponent,
			const std::string& condensedApplicability,
			const std::string& condensedIRStatus,
			const std::string& condensedReason,
			std::string* error
			)
		{
			const char* effectiveColumns[] = {
				"lambda_nm", "E_eff", "MAC_m2_per_g" };
			const RISECBOR64::Value* effectiveTable = Required(
				sourceEffective,"table",RISECBOR64::Value::Map,error );
			const RISECBOR64::Value* effectiveDefinition = Required(
				sourceEffective,"definition",RISECBOR64::Value::Map,error );
			std::vector<std::vector<double> > sourceEffectiveRows;
			double sourceDensity = 0.0;
			if( effectiveTable && !ValidateColumns(*effectiveTable,effectiveColumns,3,
				"source effective-absorption table",error) ) return false;
			if( !effectiveTable || !effectiveDefinition ||
				!ReadSourceRows(*effectiveTable,3,sourceEffectiveRows,error) ) {
				return Fail(error,
					"fire-optics operational effective absorption differs from its source record");
			}
			if( !ReadSourceEnvelopeNumber(*effectiveDefinition,"pinned_density_g_cm3",
				sourceDensity,error) ) return false;
			if( sourceDensity != density ||
				!RowsExactlyEqual(effectiveRows,sourceEffectiveRows) ) {
				return Fail(error,
					"fire-optics operational effective absorption differs from its source record");
			}

			const RISECBOR64::Value* computed = Required(
				sourceHot,"computed_outputs",RISECBOR64::Value::Map,error );
			const RISECBOR64::Value* sourceSpectrum = computed ? Required(
				*computed,"spectral_young_dp30_N50",RISECBOR64::Value::Map,error ) : 0;
			const RISECBOR64::Value* adopted = computed ? Required(
				*computed,"young_in_flame_550nm",RISECBOR64::Value::Map,error ) : 0;
			std::vector<std::vector<double> > sourceHotRows;
			double adoptedOmega = 0.0, adoptedG = 0.0;
			const char* hotColumns[] = { "lambda_nm", "omega", "g" };
			if( sourceSpectrum && !ValidateColumns(*sourceSpectrum,hotColumns,3,
				"source hot-soot table",error) ) return false;
			if( !sourceSpectrum || !adopted ||
				!ReadSourceRows(*sourceSpectrum,3,sourceHotRows,error) ||
				!ReadSourceNumber(*adopted,"omega_central",adoptedOmega,error) ||
				!ReadSourceNumber(*adopted,"g_central",adoptedG,error) ) {
				return Fail(error,"fire-optics hot-soot source mapping is incomplete");
			}
			for( std::vector<double>& row : sourceHotRows ) {
				if( row[0] == 550.0 ) {
					row[1] = adoptedOmega;
					row[2] = adoptedG;
				}
			}
			if( !RowsExactlyEqual(hotRows,sourceHotRows) ) {
				return Fail(error,
					"fire-optics operational hot-soot table differs from its source record");
			}

			const RISECBOR64::Value* coolValues = Required(
				sourceCool,"values",RISECBOR64::Value::Map,error );
			double sourceCoolKm = 0.0, sourceCoolExponent = 0.0;
			double sourceCoolOmega = 0.0, sourceCoolG = 0.0;
			const RISECBOR64::Value* sourceExponent = coolValues ? Required(
				*coolValues,"n_spectral_exponent",RISECBOR64::Value::Map,error ) : 0;
			std::vector<double> sourceRange, sourceDomain;
			std::string sourcePolicy;
			if( !coolValues || !sourceExponent ) {
				return Fail(error,
					"fire-optics operational cool-carbon values differ from their source record");
			}
			if( !ReadSourceEnvelopeNumber(*coolValues,
					"k_m_extinction_633nm_m2_per_g",sourceCoolKm,error) ||
				!ReadSourceEnvelopeNumber(*coolValues,"omega_633nm",sourceCoolOmega,error) ||
				!ReadSourceEnvelopeNumber(*coolValues,"g_asymmetry",sourceCoolG,error) ||
				!ReadSourceEnvelopeNumber(*coolValues,"n_spectral_exponent",
					sourceCoolExponent,error) ) return false;
			const RISECBOR64::Value* sourceExponentUncertainty = Required(
				*sourceExponent,"uncertainty",RISECBOR64::Value::Map,error );
			if( !sourceExponentUncertainty ) return false;
			if(
				!ReadSourceNumberArray(*sourceExponentUncertainty,"magnitude",
					sourceRange,error) ||
				!ReadSourceNumberArray(*sourceExponent,"validity_nm",sourceDomain,error) ||
				!ReadText(*sourceExponent,"out_of_domain_policy",sourcePolicy,error) ||
				sourceCoolKm != coolKm || sourceCoolExponent != coolExponent ||
				sourceCoolOmega != coolOmega || sourceCoolG != coolG ||
				sourceRange != supportedRange || sourceDomain != certifiedDomain ||
				sourcePolicy != outOfDomainPolicy ) {
				return Fail(error,
					"fire-optics operational cool-carbon values differ from their source record");
			}

			const RISECBOR64::Value* condensedComputed = Required(
				sourceCondensed,"computed_outputs_full_mie",RISECBOR64::Value::Map,error );
			const RISECBOR64::Value* condensedTable = condensedComputed ? Required(
				*condensedComputed,"table",RISECBOR64::Value::Map,error ) : 0;
			const RISECBOR64::Value* irClosure = Required(
				sourceCondensed,"ir_closure",RISECBOR64::Value::Map,error );
			std::vector<std::vector<double> > sourceCondensedRows;
			double sourceCondensedExponent = 0.0;
			std::string sourceApplicability, sourceIRStatus, sourceReason;
			const char* condensedColumns[] = {
				"lambda_nm", "k_ext_m2_per_g", "omega", "g" };
			if( condensedTable && !ValidateColumns(*condensedTable,condensedColumns,4,
				"source condensed-organics table",error) ) return false;
			if( !condensedComputed || !condensedTable || !irClosure ||
				!ReadSourceRows(*condensedTable,4,sourceCondensedRows,error) ||
				!ReadSourceNumber(*condensedComputed,
					"extinction_angstrom_exponent_450_633",sourceCondensedExponent,error) ||
				!ReadText(sourceCondensed,"applies_to",sourceApplicability,error) ||
				!ReadText(*irClosure,"status",sourceIRStatus,error) ||
				!ReadText(*irClosure,"predictive_reason_code",sourceReason,error) ||
				!RowsExactlyEqual(condensedRows,sourceCondensedRows) ||
				sourceCondensedExponent != condensedExponent ||
				sourceApplicability != condensedApplicability ||
				sourceIRStatus != "BLOCKED" || condensedIRStatus != "blocked" ||
				sourceReason != condensedReason ) {
				return Fail(error,
					"fire-optics operational condensed-organics values differ from their source record");
			}
			return true;
		}

		std::vector<double> PCHIPSlopes(
			const std::vector<double>& wavelengths,
			const std::vector<double>& values
			)
		{
			const std::size_t count = wavelengths.size();
			std::vector<double> slopes(count, 0.0);
			std::vector<double> intervals(count-1);
			std::vector<double> secants(count-1);
			for( std::size_t i=0; i+1<count; i++ ) {
				intervals[i] = wavelengths[i+1]-wavelengths[i];
				secants[i] = (values[i+1]-values[i])/intervals[i];
			}
			if( count == 2 ) {
				slopes[0] = slopes[1] = secants[0];
				return slopes;
			}
			for( std::size_t i=1; i+1<count; i++ ) {
				if( secants[i-1]*secants[i] <= 0.0 ) {
					slopes[i] = 0.0;
				} else {
					const double w1 = 2.0*intervals[i]+intervals[i-1];
					const double w2 = intervals[i]+2.0*intervals[i-1];
					slopes[i] = (w1+w2)/(w1/secants[i-1]+w2/secants[i]);
				}
			}
			auto endpoint = []( const double h0, const double h1,
				const double d0, const double d1 ) {
				double value = ((2.0*h0+h1)*d0-h0*d1)/(h0+h1);
				if( value*d0 <= 0.0 ) {
					return 0.0;
				}
				if( d0*d1 < 0.0 && std::fabs(value) > std::fabs(3.0*d0) ) {
					return 3.0*d0;
				}
				return value;
			};
			slopes[0] = endpoint(intervals[0], intervals[1], secants[0], secants[1]);
			slopes[count-1] = endpoint(
				intervals[count-2], intervals[count-3],
				secants[count-2], secants[count-3] );
			return slopes;
		}

		void DerivativeExtrema(
			const double x0, const double x1,
			const double y0, const double y1,
			const double slope0, const double slope1,
			double& minimum, double& maximum
			)
		{
			const double h = x1-x0;
			const double a = 2.0*y0-2.0*y1+h*(slope0+slope1);
			const double b = -3.0*y0+3.0*y1-h*(2.0*slope0+slope1);
			const double c = h*slope0;
			auto derivative = [a,b,c,h]( const double u ) {
				return (3.0*a*u*u+2.0*b*u+c)/h;
			};
			minimum = std::min(derivative(0.0), derivative(1.0));
			maximum = std::max(derivative(0.0), derivative(1.0));
			if( a != 0.0 ) {
				const double vertex = -b/(3.0*a);
				if( vertex > 0.0 && vertex < 1.0 ) {
					minimum = std::min(minimum, derivative(vertex));
					maximum = std::max(maximum, derivative(vertex));
				}
			}
		}

		bool ReadInterpolation(
			const RISECBOR64::Value& map,
			const char* key,
			const std::vector<double>& wavelengths,
			const std::vector<double>& values,
			DifferentiableSpectrum& result,
			std::string* error
			)
		{
			const RISECBOR64::Value* interpolation = Required(
				map, key, RISECBOR64::Value::Map, error );
			if( !interpolation ) {
				return false;
			}
			std::vector<double> slopes;
			if( !ReadFloatArray(*interpolation, "slopes", slopes, error) ||
				slopes.size() != wavelengths.size() ) {
				return Fail(error, "fire-optics interpolation slope count mismatch");
			}
			const std::vector<double> expectedSlopes = PCHIPSlopes(wavelengths, values);
			for( std::size_t i=0; i<slopes.size(); i++ ) {
				const double tolerance = 64.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0, std::fabs(expectedSlopes[i]));
				if( !NearlyEqual(slopes[i], expectedSlopes[i], tolerance) ) {
					return Fail(error, "fire-optics PCHIP slope does not match its table");
				}
			}
			const RISECBOR64::Value* encodedEnclosures = Required(
				*interpolation, "derivative_enclosures", RISECBOR64::Value::Array, error );
			if( !encodedEnclosures || encodedEnclosures->GetArray().size()+1 != wavelengths.size() ) {
				return Fail(error, "fire-optics derivative enclosure count mismatch");
			}
			std::vector<SpectralDerivativeEnclosure> enclosures;
			for( std::size_t i=0; i<encodedEnclosures->GetArray().size(); i++ ) {
				const RISECBOR64::Value& encoded = encodedEnclosures->GetArray()[i];
				if( encoded.GetType() != RISECBOR64::Value::Array || encoded.GetArray().size() != 4 ) {
					return Fail(error, "fire-optics derivative enclosure has the wrong width");
				}
				double fields[4];
				for( std::size_t j=0; j<4; j++ ) {
					if( encoded.GetArray()[j].GetType() != RISECBOR64::Value::Float64 ) {
						return Fail(error, "fire-optics derivative enclosure is not binary64");
					}
					fields[j] = encoded.GetArray()[j].GetFloat();
				}
				if( fields[0] != wavelengths[i] || fields[1] != wavelengths[i+1] ||
					!IsFinite(fields[2]) || !IsFinite(fields[3]) || fields[2] > fields[3] ) {
					return Fail(error, "fire-optics derivative enclosure has invalid bounds");
				}
				double actualMinimum, actualMaximum;
				DerivativeExtrema(
					wavelengths[i], wavelengths[i+1], values[i], values[i+1],
					slopes[i], slopes[i+1], actualMinimum, actualMaximum );
				if( actualMinimum < fields[2] || actualMaximum > fields[3] ) {
					return Fail(error, "fire-optics derivative is outside its enclosure at segment "+
						std::to_string(i)+" actual ["+std::to_string(actualMinimum)+", "+
						std::to_string(actualMaximum)+"] stored ["+std::to_string(fields[2])+", "+
						std::to_string(fields[3])+"]");
				}
				SpectralDerivativeEnclosure enclosure = {
					fields[0], fields[1], fields[2], fields[3] };
				enclosures.push_back(enclosure);
			}
			return result.Initialize(wavelengths, values, slopes, enclosures, error);
		}

		RISECBOR64::Value FloatArray( const std::vector<double>& values )
		{
			RISECBOR64::Value::Values encoded;
			for( std::size_t i=0; i<values.size(); i++ ) {
				encoded.push_back(RISECBOR64::Value::Float(values[i]));
			}
			return RISECBOR64::Value::ArrayValue(encoded);
		}

		RISECBOR64::Value Map( const RISECBOR64::Value::Members& members )
		{
			return RISECBOR64::Value::MapValue(members);
		}

		RISECBOR64::Value FrozenPhiPartition()
		{
			using RISECBOR64::Value;
			return Map({
				{ "consistency_requirement", Value::String(kPhiConsistency) },
				{ "form", Value::String(kPhiForm) },
				{ "hot_fraction_temperature_band_K", FloatArray({700.0, 900.0}) },
				{ "provenance", Value::String(kPhiProvenance) },
				{ "uncertainty", Map({
					{ "kind", Value::String("design_pinned_exact") },
					{ "magnitude", Value::Unsigned(0) }
				}) }
			});
		}

		FireOpticsPreset LoadEmbedded(
			const unsigned char* bytes,
			const std::size_t byteCount,
			const char* expectedId
			)
		{
			FireOpticsPreset result;
			RISECBOR64::Bytes encoded(bytes, bytes+byteCount);
			std::string error;
			if( !result.LoadCanonicalRecord(encoded, &error) || result.RecordId() != expectedId ) {
				return FireOpticsPreset();
			}
			return result;
		}
	}

	DifferentiableSpectrum::DifferentiableSpectrum() : m_valid(false)
	{
	}

	bool DifferentiableSpectrum::Initialize(
		const std::vector<double>& wavelengths,
		const std::vector<double>& values,
		const std::vector<double>& slopes,
		const std::vector<SpectralDerivativeEnclosure>& enclosures,
		std::string* error
		)
	{
		m_valid = false;
		if( wavelengths.size() < 2 || values.size() != wavelengths.size() ||
			slopes.size() != wavelengths.size() || enclosures.size()+1 != wavelengths.size() ) {
			return Fail(error, "invalid differentiable spectrum dimensions");
		}
		for( std::size_t i=0; i<wavelengths.size(); i++ ) {
			if( !IsFinite(wavelengths[i]) || !IsFinite(values[i]) || !IsFinite(slopes[i]) ||
				(i && wavelengths[i] <= wavelengths[i-1]) ) {
				return Fail(error, "invalid differentiable spectrum knot");
			}
		}
		m_wavelengths = wavelengths;
		m_values = values;
		m_slopes = slopes;
		m_enclosures = enclosures;
		m_valid = true;
		return true;
	}

	bool DifferentiableSpectrum::Contains( const double wavelengthNM ) const
	{
		return m_valid && wavelengthNM >= m_wavelengths.front() &&
			wavelengthNM <= m_wavelengths.back();
	}

	double DifferentiableSpectrum::Evaluate( const double wavelengthNM ) const
	{
		if( !m_valid ) {
			return 0.0;
		}
		if( wavelengthNM <= m_wavelengths.front() ) {
			return m_values.front();
		}
		if( wavelengthNM >= m_wavelengths.back() ) {
			return m_values.back();
		}
		const std::size_t upper = std::upper_bound(
			m_wavelengths.begin(), m_wavelengths.end(), wavelengthNM )-m_wavelengths.begin();
		const std::size_t lower = upper-1;
		const double h = m_wavelengths[upper]-m_wavelengths[lower];
		const double t = (wavelengthNM-m_wavelengths[lower])/h;
		const double t2 = t*t;
		const double t3 = t2*t;
		return (2.0*t3-3.0*t2+1.0)*m_values[lower]+
			(t3-2.0*t2+t)*h*m_slopes[lower]+
			(-2.0*t3+3.0*t2)*m_values[upper]+
			(t3-t2)*h*m_slopes[upper];
	}

	double DifferentiableSpectrum::Derivative( const double wavelengthNM ) const
	{
		if( !m_valid ) {
			return 0.0;
		}
		if( wavelengthNM <= m_wavelengths.front() ) {
			return m_slopes.front();
		}
		if( wavelengthNM >= m_wavelengths.back() ) {
			return m_slopes.back();
		}
		const std::size_t upper = std::upper_bound(
			m_wavelengths.begin(), m_wavelengths.end(), wavelengthNM )-m_wavelengths.begin();
		const std::size_t lower = upper-1;
		const double h = m_wavelengths[upper]-m_wavelengths[lower];
		const double t = (wavelengthNM-m_wavelengths[lower])/h;
		return ((6.0*t*t-6.0*t)/h)*m_values[lower]+
			(3.0*t*t-4.0*t+1.0)*m_slopes[lower]+
			((-6.0*t*t+6.0*t)/h)*m_values[upper]+
			(3.0*t*t-2.0*t)*m_slopes[upper];
	}

	FireOpticsPreset::FireOpticsPreset() :
		m_valid(false), m_recordClass(InvalidRecord), m_domainMinNM(0.0),
		m_domainMaxNM(0.0), m_hotFractionMinK(0.0), m_hotFractionMaxK(0.0),
		m_densityGCM3(0.0), m_constantEffectiveAbsorption(0.0),
		m_coolKm633(0.0), m_coolExponent(0.0), m_coolOmega(0.0), m_coolG(0.0),
		m_coolDomainMinNM(0.0), m_coolDomainMaxNM(0.0),
		m_condensedFixtureKm633(0.0), m_condensedFixtureExponent(0.0),
		m_condensedFixtureOmega(0.0), m_condensedFixtureG(0.0),
		m_condensedPreviewExponent(0.0)
	{
	}

	bool FireOpticsPreset::LoadCanonicalRecord(
		const RISECBOR64::Bytes& bytes,
		std::string* error
		)
	{
		*this = FireOpticsPreset();
		RISECBOR64::Value record;
		if( !RISECBOR64::DecodeCanonical(bytes, record, error) ) {
			return false;
		}
		if( !LoadSemanticRecord(record, error) ) {
			*this = FireOpticsPreset();
			return false;
		}
		m_recordBytes = bytes;
		m_recordId = RISECBOR64::SHA256Hex(bytes);
		m_valid = true;
		return true;
	}

	bool FireOpticsPreset::LoadSemanticRecord(
		const RISECBOR64::Value& record,
		std::string* error
		)
	{
		const RISECBOR64::Value* schema = Required(
			record, "schema_version", RISECBOR64::Value::UnsignedInteger, error );
		std::string kind, recordClass, interpolation, aggregateVersion, aggregateStatus;
		if( !schema || schema->GetIntegerArgument() != 1 ||
			!ReadText(record, "record_kind", kind, error) || kind != "fire_optics_preset" ||
			!ReadText(record, "record_name", m_recordName, error) ||
			!ReadText(record, "version", aggregateVersion, error) ||
			!ReadText(record, "record_status", aggregateStatus, error) ||
			!ReadText(record, "record_class", recordClass, error) ||
			!ReadText(record, "interpolation", interpolation, error) ) {
			return Fail(error, "unsupported fire-optics record header");
		}
		const bool predictiveHeader =
			m_recordName == "fire-optics-predictive-v1" &&
			recordClass == "predictive_optical_preset" &&
			interpolation == "pchip_monotone_c1_v1";
		const bool syntheticHeader =
			m_recordName == "fire-optics-synthetic-regression-v1" &&
			recordClass == "synthetic_regression_fixture" &&
			interpolation == "analytic_fixture_v1";
		const bool explicitSyntheticHeader =
			m_recordName == "fire-optics-explicit-synthetic-fixture" &&
			recordClass == "synthetic_regression_fixture" &&
			interpolation == "analytic_fixture_v1";
		if( !predictiveHeader && !syntheticHeader && !explicitSyntheticHeader ) {
			return Fail(error,"unsupported fire-optics record name/class combination");
		}
		const RISECBOR64::Value* sourceRecords = 0;
		const RISECBOR64::Value* sourceEffective = 0;
		const RISECBOR64::Value* sourceHot = 0;
		const RISECBOR64::Value* sourceCool = 0;
		const RISECBOR64::Value* sourceCondensed = 0;
		const RISECBOR64::Value* sourceFixtures = 0;
		const RISECBOR64::Value* sourceExplicitFixtures = 0;
		{
			const RISECBOR64::Value* provenanceSchema = Required(
				record, "provenance_schema", RISECBOR64::Value::Map, error );
			sourceRecords = Required(
				record, "source_records", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* componentStatuses = Required(
				record, "component_record_statuses", RISECBOR64::Value::Map, error );
			if( !provenanceSchema || !sourceRecords || !componentStatuses ) {
				return Fail(error, "fire-optics canonical provenance schema is missing");
			}
			if( !ValidateProvenanceSchema(*provenanceSchema,error) ) return false;
			if( explicitSyntheticHeader ) {
				sourceExplicitFixtures = Required(*sourceRecords,
					"explicit_synthetic_fixtures",RISECBOR64::Value::Map,error );
				std::string sourceVersion, sourceStatus, sourceName;
				if( !sourceExplicitFixtures ||
					!ValidateSourceRecordHeader(*sourceExplicitFixtures,error) ||
					!ReadText(*sourceExplicitFixtures,"record_name",sourceName,error) ||
					!ReadText(*sourceExplicitFixtures,"version",sourceVersion,error) ||
					!ReadText(*sourceExplicitFixtures,"record_status",sourceStatus,error) ||
					sourceName != "fire-optics-explicit-synthetic-fixtures-v1" ||
					sourceVersion != aggregateVersion || sourceStatus != aggregateStatus ||
					!SourceStatusMatches(*componentStatuses,"explicit_synthetic_fixtures",
						*sourceExplicitFixtures,error) ) {
					return Fail(error,
						"fire-optics explicit synthetic fixture provenance is missing");
				}
			} else {
			sourceEffective = Required(*sourceRecords, "effective_absorption",
				RISECBOR64::Value::Map, error);
			sourceHot = Required(*sourceRecords, "hot_soot",
				RISECBOR64::Value::Map, error);
			sourceCool = Required(*sourceRecords, "cool_carbon",
				RISECBOR64::Value::Map, error);
			sourceCondensed = Required(*sourceRecords, "condensed_organics",
				RISECBOR64::Value::Map, error);
			if( !sourceEffective || !sourceHot || !sourceCool || !sourceCondensed ||
				!ValidateSourceRecordHeader(*sourceEffective, error) ||
				!ValidateSourceRecordHeader(*sourceHot, error) ||
				!ValidateSourceRecordHeader(*sourceCool, error) ||
				!ValidateSourceRecordHeader(*sourceCondensed, error) ||
				!SourceStatusMatches(*componentStatuses, "effective_absorption",
					*sourceEffective, error) ||
				!SourceStatusMatches(*componentStatuses, "hot_soot", *sourceHot, error) ||
				!SourceStatusMatches(*componentStatuses, "cool_carbon", *sourceCool, error) ||
				!SourceStatusMatches(*componentStatuses, "condensed_organics",
					*sourceCondensed, error) ) {
				return Fail(error, "fire-optics canonical source record metadata is incomplete");
			}
			std::string effectiveVersion, hotVersion, coolVersion, condensedVersion;
			std::string effectiveStatus;
			if( !ReadText(*sourceEffective, "version", effectiveVersion, error) ||
				!ReadText(*sourceHot, "version", hotVersion, error) ||
				!ReadText(*sourceCool, "version", coolVersion, error) ||
				!ReadText(*sourceCondensed, "version", condensedVersion, error) ||
				!ReadText(*sourceEffective, "record_status", effectiveStatus, error) ||
				hotVersion != effectiveVersion || coolVersion != effectiveVersion ||
				condensedVersion != effectiveVersion ||
				(m_recordName == "fire-optics-predictive-v1" &&
					aggregateVersion != effectiveVersion) ) {
				return Fail(error, "fire-optics aggregate version does not match its sources");
			}
			const RISECBOR64::Value* sourceHotPhi = Required(
				*sourceHot, "phi_T_partition", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* sourceCoolPhi = Required(
				*sourceCool, "phi_T_partition", RISECBOR64::Value::Map, error );
			if( !sourceHotPhi || !sourceCoolPhi ||
				!CanonicallyEqual(*sourceHotPhi, *sourceCoolPhi, error) ) {
				return Fail(error,
					"hot-soot and cool-carbon source phi_T_partition fields differ");
			}
			const RISECBOR64::Value* effectiveTable = Required(
				*sourceEffective, "table", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* hotComputed = Required(
				*sourceHot, "computed_outputs", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* condensedComputed = Required(
				*sourceCondensed, "computed_outputs_full_mie",
				RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* effectiveMetadata = effectiveTable ? Required(
				*effectiveTable, "table_metadata", RISECBOR64::Value::Map, error ) : 0;
			const RISECBOR64::Value* hotMetadata = hotComputed ? Required(
				*hotComputed, "spectral_young_dp30_N50_metadata",
				RISECBOR64::Value::Map, error ) : 0;
			const RISECBOR64::Value* condensedMetadata = condensedComputed ? Required(
				*condensedComputed, "table_metadata", RISECBOR64::Value::Map, error ) : 0;
			if( !effectiveMetadata || !hotMetadata || !condensedMetadata ||
				!ValidateTableMetadata(*effectiveMetadata, error) ||
				!ValidateTableMetadata(*hotMetadata, error) ||
				!ValidateTableMetadata(*condensedMetadata, error) ) {
				return Fail(error, "fire-optics source table metadata is incomplete");
			}
			if( m_recordName == "fire-optics-synthetic-regression-v1" ) {
				sourceFixtures = Required(
					*sourceRecords, "synthetic_fixtures", RISECBOR64::Value::Map, error );
				std::string fixtureName, fixtureStatus, fixtureVersion;
				if( !sourceFixtures || !ValidateSourceRecordHeader(*sourceFixtures, error) ||
					!ReadText(*sourceFixtures, "record_name", fixtureName, error) ||
					!ReadText(*sourceFixtures, "record_status", fixtureStatus, error) ||
					!ReadText(*sourceFixtures, "version", fixtureVersion, error) ||
					fixtureName != "fire-optics-synthetic-fixtures-v1" ||
					fixtureStatus !=
						"SYNTHETIC_NON_PREDICTIVE__REGRESSION_FIXTURES_ONLY" ||
					aggregateVersion != fixtureVersion || aggregateStatus != fixtureStatus ||
					!SourceStatusMatches(*componentStatuses, "synthetic_fixtures",
						*sourceFixtures, error) ) {
					return Fail(error, "fire-optics synthetic fixture provenance is missing");
				}
			} else if( aggregateStatus != effectiveStatus ) {
				return Fail(error, "fire-optics predictive aggregate status is not source-backed");
			}
			}
		}
		const RISECBOR64::Value* effective = Required(
			record, "effective_absorption", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* hot = Required(
			record, "hot_soot", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* cool = Required(
			record, "cool_carbon", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* condensed = Required(
			record, "condensed_organics", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* operationalHotPhi = hot ? hot->Find(
			"phi_T_partition") : 0;
		const RISECBOR64::Value* operationalCoolPhi = cool ? cool->Find(
			"phi_T_partition") : 0;
		if( !effective || !hot || !cool || !condensed ||
			!operationalHotPhi || !operationalCoolPhi ||
			operationalHotPhi->GetType() != RISECBOR64::Value::Map ||
			operationalCoolPhi->GetType() != RISECBOR64::Value::Map ||
			!ReadPhiPartition(*hot, *cool, m_hotFractionMinK,
				m_hotFractionMaxK, error) ||
			!ReadDomain(*effective, m_domainMinNM, m_domainMaxNM, error) ||
			!ReadFloat(*effective, "pinned_density_g_cm3", m_densityGCM3, error) ||
			m_densityGCM3 <= 0.0 ) {
			return false;
		}
		if( sourceHot && sourceCool ) {
			const RISECBOR64::Value* sourceHotPhi = Required(
				*sourceHot,"phi_T_partition",RISECBOR64::Value::Map,error );
			const RISECBOR64::Value* sourceCoolPhi = Required(
				*sourceCool,"phi_T_partition",RISECBOR64::Value::Map,error );
			if( !sourceHotPhi || !sourceCoolPhi ||
				!PhiSourceMatchesOperational(*sourceHotPhi,*operationalHotPhi,error) ||
				!PhiSourceMatchesOperational(*sourceCoolPhi,*operationalCoolPhi,error) ) {
				return false;
			}
		}
		if( recordClass == "predictive_optical_preset" &&
			interpolation == "pchip_monotone_c1_v1" ) {
			m_recordClass = PredictiveOpticalPreset;
			std::string normative;
			if( !ReadText(*effective, "normative_quantity", normative, error) ||
				normative != "MAC_lambda_m2_per_g" ) {
				return Fail(error, "fire-optics normative quantity is not MAC");
			}
			const RISECBOR64::Value* effectiveMetadata = Required(
				*effective, "table_metadata", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* hotMetadata = Required(
				*hot, "table_metadata", RISECBOR64::Value::Map, error );
			const RISECBOR64::Value* condensedMetadata = Required(
				*condensed, "table_metadata", RISECBOR64::Value::Map, error );
			if( !effectiveMetadata || !hotMetadata || !condensedMetadata ||
				!ValidateTableMetadata(*effectiveMetadata, error) ||
				!ValidateTableMetadata(*hotMetadata, error) ||
				!ValidateTableMetadata(*condensedMetadata, error) ||
				!Required(*hotMetadata, "adopted_550nm", RISECBOR64::Value::Map, error) ||
				!Required(*hotMetadata, "adoption_ruling", RISECBOR64::Value::Map, error) ) {
				return Fail(error, "fire-optics operational table metadata is incomplete");
			}
			std::vector<std::vector<double> > effectiveRows;
			const char* effectiveColumns[] = {
				"lambda_nm", "E_eff", "MAC_m2_per_g" };
			if( !ValidateColumns(*effective,effectiveColumns,3,
				"operational effective-absorption table",error) ) return false;
			if( !ReadRows(*effective, 3, effectiveRows, error) ||
				effectiveRows.size() != 81 ) {
				return Fail(error, "fire-optics E/MAC table must contain 81 rows");
			}
			std::vector<double> wavelengths, effectiveValues, macValues;
			for( std::size_t i=0; i<effectiveRows.size(); i++ ) {
				wavelengths.push_back(effectiveRows[i][0]);
				effectiveValues.push_back(effectiveRows[i][1]);
				macValues.push_back(effectiveRows[i][2]);
				if( effectiveRows[i][0] != 380.0+5.0*static_cast<double>(i) ||
					effectiveRows[i][1] <= 0.0 || effectiveRows[i][2] <= 0.0 ) {
					return Fail(error, "fire-optics E/MAC row is not the frozen positive grid");
				}
			}
			if( !ReadInterpolation(*effective, "mac_interpolation", wavelengths,
				macValues, m_mac, error) ) {
				return false;
			}
			std::vector<std::vector<double> > hotRows;
			const char* hotColumns[] = { "lambda_nm", "omega", "g" };
			if( !ValidateColumns(*hot,hotColumns,3,
				"operational hot-soot table",error) ) return false;
			if( !ReadRows(*hot, 3, hotRows, error) || hotRows.size() != 5 ) {
				return Fail(error, "fire-optics hot-soot table must contain five rows");
			}
			std::vector<double> hotWavelengths, hotOmega, hotG;
			for( std::size_t i=0; i<hotRows.size(); i++ ) {
				hotWavelengths.push_back(hotRows[i][0]);
				hotOmega.push_back(hotRows[i][1]);
				hotG.push_back(hotRows[i][2]);
				if( hotRows[i][1] < 0.0 || hotRows[i][1] >= 1.0 ||
					hotRows[i][2] <= -1.0 || hotRows[i][2] >= 1.0 ) {
					return Fail(error, "fire-optics hot-soot row is outside its physical range");
				}
			}
			if( hotWavelengths.front() != m_domainMinNM ||
				hotWavelengths.back() != m_domainMaxNM ) {
				return Fail(error, "fire-optics hot-soot table does not cover the record domain");
			}
			if( !ReadInterpolation(*hot, "omega_interpolation", hotWavelengths,
				hotOmega, m_hotOmega, error) ||
				!ReadInterpolation(*hot, "g_interpolation", hotWavelengths,
				hotG, m_hotG, error) ) {
				return false;
			}
			if( !ReadFloat(*cool, "k_m_extinction_633nm_m2_per_g", m_coolKm633, error) ||
				!ReadFloat(*cool, "n_spectral_exponent", m_coolExponent, error) ||
				!ReadFloat(*cool, "omega_633nm", m_coolOmega, error) ||
				!ReadFloat(*cool, "g_633nm", m_coolG, error) ) {
				return false;
			}
			std::vector<double> supportedRange;
			std::vector<double> certifiedDomain;
			if( !ReadFloatArray(*cool, "n_supported_range", supportedRange, error) ||
				!ReadFloatArray(*cool, "certified_domain_nm", certifiedDomain, error) ||
				!ReadText(*cool, "out_of_domain_policy",
					m_coolOutOfDomainPolicy, error) ) {
				return false;
			}
			if( supportedRange.size() != 2 || supportedRange[0] != 1.0 ||
				supportedRange[1] != 1.2 ) {
				return Fail(error,"fire-optics cool-carbon exponent range is not frozen v1");
			}
			if( certifiedDomain.size() != 2 || certifiedDomain[0] != 380.0 ||
				certifiedDomain[1] != 780.0 ) {
				return Fail(error,"fire-optics cool-carbon certified domain is not frozen v1");
			}
			if( m_coolOutOfDomainPolicy != kCoolOutOfDomainPolicy ) {
				return Fail(error,"fire-optics cool-carbon out-of-domain policy is not frozen v1");
			}
			if( m_coolKm633 <= 0.0 ||
				m_coolExponent < supportedRange[0] ||
				m_coolExponent > supportedRange[1] || m_coolOmega < 0.0 ||
				m_coolOmega >= 1.0 || m_coolG <= -1.0 || m_coolG >= 1.0 ) {
				return Fail(error,"fire-optics cool-carbon optical values are outside v1 ranges");
			}
			m_coolDomainMinNM = certifiedDomain[0];
			m_coolDomainMaxNM = certifiedDomain[1];
			std::vector<std::vector<double> > condensedRows;
			const char* condensedColumns[] = {
				"lambda_nm", "k_ext_m2_per_g", "omega", "g" };
			if( !ValidateColumns(*condensed,condensedColumns,4,
				"operational condensed-organics table",error) ) return false;
			if( !ReadRows(*condensed, 4, condensedRows, error) ||
				condensedRows.size() != 5 ) {
				return Fail(error, "fire-optics condensed-organics table must contain five rows");
			}
			std::vector<double> condensedWavelengths, condensedKm, condensedOmega, condensedG;
			for( std::size_t i=0; i<condensedRows.size(); i++ ) {
				condensedWavelengths.push_back(condensedRows[i][0]);
				condensedKm.push_back(condensedRows[i][1]);
				condensedOmega.push_back(condensedRows[i][2]);
				condensedG.push_back(condensedRows[i][3]);
				if( condensedRows[i][1] <= 0.0 || condensedRows[i][2] < 0.0 ||
					condensedRows[i][2] >= 1.0 || condensedRows[i][3] <= -1.0 ||
					condensedRows[i][3] >= 1.0 ) {
					return Fail(error,
						"fire-optics condensed-organic row is outside its physical range");
				}
			}
			if( condensedWavelengths.front() != m_domainMinNM ||
				condensedWavelengths.back() != m_domainMaxNM ) {
				return Fail(error,
					"fire-optics condensed-organic table does not cover the record domain");
			}
			if( !ReadInterpolation(*condensed, "k_ext_interpolation", condensedWavelengths,
				condensedKm, m_condensedKm, error) ||
				!ReadInterpolation(*condensed, "omega_interpolation", condensedWavelengths,
				condensedOmega, m_condensedOmega, error) ||
				!ReadInterpolation(*condensed, "g_interpolation", condensedWavelengths,
				condensedG, m_condensedG, error) ||
				!ReadFloat(*condensed, "extinction_angstrom_exponent_450_633",
					m_condensedPreviewExponent, error) ||
				!ReadText(*condensed, "applicability", m_condensedApplicability, error) ||
				!ReadText(*condensed, "ir_closure_status", m_condensedIRClosureStatus, error) ||
				!ReadText(*condensed, "predictive_reason_code", m_condensedPredictiveReason, error) ) {
				return false;
			}
			if( m_hotFractionMinK != 700.0 || m_hotFractionMaxK != 900.0 ) {
				return Fail(error, "fire-optics predictive v1 phi(T) band gate failed");
			}
			if( m_densityGCM3 != 1.8 ) {
				return Fail(error, "fire-optics predictive v1 density gate failed");
			}
			if( !NearlyEqual(MAC(550.0), 8.0, 1.0e-12) ) {
				return Fail(error, "fire-optics predictive v1 MAC(550 nm) gate failed");
			}
			if( !NearlyEqual(MAC(632.8), 6.647, 5.0e-4) ) {
				return Fail(error, "fire-optics predictive v1 MAC(632.8 nm) gate failed");
			}
			if( !NearlyEqual(effectiveValues.front()/effectiveValues.back(),
				1.311, 5.0e-4) ) {
				return Fail(error, "fire-optics predictive v1 E_eff shape gate failed");
			}
			if( !NearlyEqual(-std::log(MAC(380.0)/MAC(780.0))/
				std::log(380.0/780.0), 1.377, 5.0e-4) ) {
				return Fail(error, "fire-optics predictive v1 visible AAE gate failed");
			}
			for( std::size_t i=0; i<effectiveRows.size(); i++ ) {
				const double derived = effectiveRows[i][2]*(m_densityGCM3*1000000.0)*
					effectiveRows[i][0]*1.0e-9/(6.0*kPi);
				if( !NearlyEqual(derived, effectiveRows[i][1], 5.0e-6) ) {
					return Fail(error, "fire-optics E_eff row disagrees with normative MAC");
				}
			}
			if( !NearlyEqual(HotAlbedo(550.0), 0.10, 2.0e-3) ||
				!NearlyEqual(HotG(550.0), 0.22, 1.0e-12) ) {
				return Fail(error, "fire-optics predictive v1 hot-soot gate failed");
			}
			if( !NearlyEqual(m_coolKm633, 8.7, 1.0e-12) ||
				!NearlyEqual(m_coolOmega, 0.25, 1.0e-12) ||
				!NearlyEqual(m_coolG, 0.58, 1.0e-12) ) {
				return Fail(error, "fire-optics predictive v1 cool-carbon gate failed");
			}
			if( m_condensedPredictiveReason != "condensed_organics_ir_unclosed" ||
				m_condensedIRClosureStatus != "blocked" ) {
				return Fail(error, "fire-optics predictive v1 condensed-organics gate failed");
			}
			if( !sourceEffective || !sourceHot || !sourceCool || !sourceCondensed ||
				!ValidatePredictiveSourceBindings(
					*sourceEffective,*sourceHot,*sourceCool,*sourceCondensed,
					effectiveRows,m_densityGCM3,hotRows,m_coolKm633,m_coolExponent,
					m_coolOmega,m_coolG,supportedRange,certifiedDomain,
					m_coolOutOfDomainPolicy,condensedRows,m_condensedPreviewExponent,
					m_condensedApplicability,m_condensedIRClosureStatus,
					m_condensedPredictiveReason,error) ) {
				return false;
			}
			return true;
		}

		if( recordClass == "synthetic_regression_fixture" &&
			interpolation == "analytic_fixture_v1" ) {
			m_recordClass = SyntheticRegressionFixture;
			double hotOmega, hotGValue;
			if( sourceExplicitFixtures ) {
				const RISECBOR64::Value* fixtureValues = Required(
					*sourceExplicitFixtures,"values",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* effectiveEnvelope = Required(
					*effective,"E_eff",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* densityMetadata = Required(
					*effective,"pinned_density_metadata",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* coolKmMetadata = Required(
					*cool,"k_m_extinction_metadata",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* condensedKmMetadata = Required(
					*condensed,"k_m_extinction_metadata",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* sourceHotPhi = Required(
					*sourceExplicitFixtures,"hot_phi_T_partition",
					RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* sourceCoolPhi = Required(
					*sourceExplicitFixtures,"cool_phi_T_partition",
					RISECBOR64::Value::Map,error );
				if( !fixtureValues || !effectiveEnvelope || !densityMetadata ||
					!coolKmMetadata || !condensedKmMetadata || !sourceHotPhi ||
					!sourceCoolPhi ||
					!MembersCanonicallyEqual(*fixtureValues,"E_eff",*effective,"E_eff",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"density_g_cm3",*effective,
						"pinned_density_metadata",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"hot_omega",*hot,"omega",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"hot_g",*hot,"g",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"cool_k_m_633",*cool,
						"k_m_extinction_metadata",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"cool_n",*cool,
						"n_spectral_exponent",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"cool_omega",*cool,"omega",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"cool_g",*cool,"g",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"condensed_k_m_633",*condensed,
						"k_m_extinction_metadata",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"condensed_n",*condensed,
						"n_spectral_exponent",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"condensed_omega",*condensed,
						"omega",error) ||
					!MembersCanonicallyEqual(*fixtureValues,"condensed_g",*condensed,"g",error) ||
					!CanonicallyEqual(*sourceHotPhi,*sourceCoolPhi,error) ||
					!CanonicallyEqual(*sourceHotPhi,*hot->Find("phi_T_partition"),error) ||
					!ReadEnvelopedFixtureFloat(*effective,"E_eff",
						m_constantEffectiveAbsorption,error) ||
					!ReadEnvelopedFixtureFloat(*hot,"omega",hotOmega,error) ||
					!ReadEnvelopedFixtureFloat(*hot,"g",hotGValue,error) ||
					!ReadFloat(*cool,"k_m_extinction_633nm_m2_per_g",m_coolKm633,error) ||
					!ReadEnvelopedFixtureFloat(*cool,"n_spectral_exponent",m_coolExponent,error) ||
					!ReadEnvelopedFixtureFloat(*cool,"omega",m_coolOmega,error) ||
					!ReadEnvelopedFixtureFloat(*cool,"g",m_coolG,error) ||
					!ReadFloat(*condensed,"k_m_extinction_633nm_m2_per_g",
						m_condensedFixtureKm633,error) ||
					!ReadEnvelopedFixtureFloat(*condensed,"n_spectral_exponent",
						m_condensedFixtureExponent,error) ||
					!ReadEnvelopedFixtureFloat(*condensed,"omega",
						m_condensedFixtureOmega,error) ||
					!ReadEnvelopedFixtureFloat(*condensed,"g",m_condensedFixtureG,error) ) {
					return Fail(error,
						"fire-optics explicit synthetic operational provenance is incomplete");
				}
				double sourceDensity = 0.0, sourceCoolKm = 0.0, sourceCondensedKm = 0.0;
				if( !ValidateFixtureEnvelope(*densityMetadata,sourceDensity,error) ||
					!ValidateFixtureEnvelope(*coolKmMetadata,sourceCoolKm,error) ||
					!ValidateFixtureEnvelope(*condensedKmMetadata,sourceCondensedKm,error) ||
					sourceDensity != m_densityGCM3 || sourceCoolKm != m_coolKm633 ||
					sourceCondensedKm != m_condensedFixtureKm633 ) {
					return Fail(error,
						"fire-optics explicit synthetic scalar differs from its source envelope");
				}
			} else if( sourceFixtures ) {
				const RISECBOR64::Value* fixtureValues = Required(
					*sourceFixtures, "values", RISECBOR64::Value::Map, error );
				const RISECBOR64::Value* sourceEffectiveDefinition = Required(
					*sourceEffective,"definition",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* sourceDensityEnvelope = sourceEffectiveDefinition ?
					Required(*sourceEffectiveDefinition,"pinned_density_g_cm3",
						RISECBOR64::Value::Map,error) : 0;
				const RISECBOR64::Value* sourceCoolRecordValues = Required(
					*sourceCool,"values",RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* sourceCoolKmEnvelope = sourceCoolRecordValues ?
					Required(*sourceCoolRecordValues,"k_m_extinction_633nm_m2_per_g",
						RISECBOR64::Value::Map,error) : 0;
				const RISECBOR64::Value* sourceCondensedComputed = Required(
					*sourceCondensed,"computed_outputs_full_mie",
					RISECBOR64::Value::Map,error );
				const RISECBOR64::Value* sourceCondensedTable = sourceCondensedComputed ?
					Required(*sourceCondensedComputed,"table",RISECBOR64::Value::Map,error) : 0;
				const RISECBOR64::Value* sourceCondensedMetadata = sourceCondensedComputed ?
					Required(*sourceCondensedComputed,"table_metadata",
						RISECBOR64::Value::Map,error) : 0;
				const RISECBOR64::Value* sourceEffectiveEnvelope = fixtureValues ? Required(
					*fixtureValues, "E_eff_fixture", RISECBOR64::Value::Map, error ) : 0;
				const RISECBOR64::Value* sourceHotValues = fixtureValues ? Required(
					*fixtureValues, "hot_soot", RISECBOR64::Value::Map, error ) : 0;
				const RISECBOR64::Value* sourceCoolValues = fixtureValues ? Required(
					*fixtureValues, "fresh_smoke_cool_carbon",
					RISECBOR64::Value::Map, error ) : 0;
				const RISECBOR64::Value* sourceCondensedValues = fixtureValues ? Required(
					*fixtureValues, "organic_droplets_condensed",
					RISECBOR64::Value::Map, error ) : 0;
				const RISECBOR64::Value* effectiveEnvelope = Required(
					*effective, "E_eff", RISECBOR64::Value::Map, error );
				if( !sourceDensityEnvelope || !sourceCoolKmEnvelope ||
					!sourceCondensedTable || !sourceCondensedMetadata ||
					!sourceEffectiveEnvelope || !sourceHotValues || !sourceCoolValues ||
					!sourceCondensedValues || !effectiveEnvelope ||
					!CanonicallyEqual(*sourceEffectiveEnvelope, *effectiveEnvelope, error) ||
					!MembersCanonicallyEqual(*hot, "omega", *sourceHotValues,
						"omega", error) ||
					!MembersCanonicallyEqual(*hot, "g", *sourceHotValues, "g", error) ||
					!MembersCanonicallyEqual(*cool, "n_spectral_exponent",
						*sourceCoolValues, "n_exponent", error) ||
					!MembersCanonicallyEqual(*cool, "omega", *sourceCoolValues,
						"omega", error) ||
					!MembersCanonicallyEqual(*cool, "g", *sourceCoolValues, "g", error) ||
					!MembersCanonicallyEqual(*condensed, "n_spectral_exponent",
						*sourceCondensedValues, "n_exponent", error) ||
					!MembersCanonicallyEqual(*condensed, "omega", *sourceCondensedValues,
						"omega", error) ||
					!MembersCanonicallyEqual(*condensed, "g", *sourceCondensedValues,
						"g", error) ||
					!Required(*effective, "pinned_density_metadata",
						RISECBOR64::Value::Map, error) ||
					!Required(*cool, "k_m_extinction_metadata",
						RISECBOR64::Value::Map, error) ||
					!Required(*condensed, "k_m_extinction_metadata",
						RISECBOR64::Value::Map, error) ) {
					return Fail(error,
						"fire-optics synthetic operational provenance is incomplete");
				}
				if( !ReadEnvelopedFixtureFloat(*effective, "E_eff",
						m_constantEffectiveAbsorption, error) ||
					!ReadEnvelopedFixtureFloat(*hot, "omega", hotOmega, error) ||
					!ReadEnvelopedFixtureFloat(*hot, "g", hotGValue, error) ||
					!ReadFloat(*cool, "k_m_extinction_633nm_m2_per_g", m_coolKm633, error) ||
					!ReadEnvelopedFixtureFloat(*cool, "n_spectral_exponent",
						m_coolExponent, error) ||
					!ReadEnvelopedFixtureFloat(*cool, "omega", m_coolOmega, error) ||
					!ReadEnvelopedFixtureFloat(*cool, "g", m_coolG, error) ||
					!ReadFloat(*condensed, "k_m_extinction_633nm_m2_per_g",
						m_condensedFixtureKm633, error) ||
					!ReadEnvelopedFixtureFloat(*condensed, "n_spectral_exponent",
						m_condensedFixtureExponent, error) ||
					!ReadEnvelopedFixtureFloat(*condensed, "omega",
						m_condensedFixtureOmega, error) ||
					!ReadEnvelopedFixtureFloat(*condensed, "g",
						m_condensedFixtureG, error) ) {
					return false;
				}
				double sourceDensity = 0.0, sourceCoolKm = 0.0;
				std::vector<std::vector<double> > sourceCondensedRows;
				double sourceCondensedKm = -1.0;
				const RISECBOR64::Value* densityMetadata = effective->Find(
					"pinned_density_metadata");
				const RISECBOR64::Value* coolKmMetadata = cool->Find(
					"k_m_extinction_metadata");
				const RISECBOR64::Value* condensedKmMetadata = condensed->Find(
					"k_m_extinction_metadata");
				if( !densityMetadata || !coolKmMetadata || !condensedKmMetadata ||
					!CanonicallyEqual(*densityMetadata,*sourceDensityEnvelope,error) ||
					!CanonicallyEqual(*coolKmMetadata,*sourceCoolKmEnvelope,error) ||
					!CanonicallyEqual(*condensedKmMetadata,*sourceCondensedMetadata,error) ||
					!ValidateSourceEnvelope(*sourceDensityEnvelope,sourceDensity,error) ||
					!ValidateSourceEnvelope(*sourceCoolKmEnvelope,sourceCoolKm,error) ||
					!ValidateTableMetadata(*sourceCondensedMetadata,error) ||
					!ReadSourceRows(*sourceCondensedTable,4,sourceCondensedRows,error) ) {
					return Fail(error,
						"fire-optics synthetic scalar source binding is incomplete");
				}
				for( const std::vector<double>& row : sourceCondensedRows ) {
					if( row[0] == 633.0 ) sourceCondensedKm = row[1];
				}
				if( sourceDensity != m_densityGCM3 || sourceCoolKm != m_coolKm633 ||
					sourceCondensedKm != m_condensedFixtureKm633 ) {
					return Fail(error,
						"fire-optics synthetic scalar differs from its source record");
				}
			} else if( !ReadFloat(*effective, "E_eff", m_constantEffectiveAbsorption, error) ||
				!ReadFloat(*hot, "omega", hotOmega, error) ||
				!ReadFloat(*hot, "g", hotGValue, error) ||
				!ReadFloat(*cool, "k_m_extinction_633nm_m2_per_g", m_coolKm633, error) ||
				!ReadFloat(*cool, "n_spectral_exponent", m_coolExponent, error) ||
				!ReadFloat(*cool, "omega", m_coolOmega, error) ||
				!ReadFloat(*cool, "g", m_coolG, error) ||
				!ReadFloat(*condensed, "k_m_extinction_633nm_m2_per_g",
					m_condensedFixtureKm633, error) ||
				!ReadFloat(*condensed, "n_spectral_exponent",
					m_condensedFixtureExponent, error) ||
				!ReadFloat(*condensed, "omega", m_condensedFixtureOmega, error) ||
				!ReadFloat(*condensed, "g", m_condensedFixtureG, error) ) {
				return false;
			}
			if( !ReadText(*condensed, "predictive_reason_code",
				m_condensedPredictiveReason, error) ) {
				return false;
			}
			std::vector<double> fixtureWavelengths;
			fixtureWavelengths.push_back(m_domainMinNM);
			fixtureWavelengths.push_back(m_domainMaxNM);
			std::vector<double> omegaValues(2, hotOmega);
			std::vector<double> gValues(2, hotGValue);
			std::vector<double> zeroSlopes(2, 0.0);
			std::vector<SpectralDerivativeEnclosure> zeroEnclosures(1);
			zeroEnclosures[0] = { m_domainMinNM, m_domainMaxNM, 0.0, 0.0 };
			if( !m_hotOmega.Initialize(fixtureWavelengths, omegaValues, zeroSlopes,
				zeroEnclosures, error) || !m_hotG.Initialize(fixtureWavelengths, gValues,
				zeroSlopes, zeroEnclosures, error) ) {
				return false;
			}
			m_condensedPreviewExponent = m_condensedFixtureExponent;
			m_condensedIRClosureStatus = "blocked";
			m_condensedApplicability = "explicitly synthetic regression fixture";
			m_coolDomainMinNM = m_domainMinNM;
			m_coolDomainMaxNM = m_domainMaxNM;
			const bool validFixture =
				m_hotFractionMinK == 700.0 && m_hotFractionMaxK == 900.0 &&
				m_constantEffectiveAbsorption >= 0.0 && m_densityGCM3 > 0.0 &&
				hotOmega >= 0.0 && hotOmega < 1.0 && hotGValue > -1.0 &&
				hotGValue < 1.0 && m_coolKm633 >= 0.0 && m_coolExponent >= 0.0 &&
				m_coolOmega >= 0.0 && m_coolOmega <= 1.0 && m_coolG > -1.0 &&
				m_coolG < 1.0 && m_condensedFixtureKm633 >= 0.0 &&
				m_condensedFixtureExponent >= 0.0 && m_condensedFixtureOmega >= 0.0 &&
				m_condensedFixtureOmega <= 1.0 && m_condensedFixtureG > -1.0 &&
				m_condensedFixtureG < 1.0;
			if( m_condensedPredictiveReason != "condensed_organics_ir_unclosed" ) {
				return Fail(error,
					"fire-optics synthetic fixture has an unknown predictive reason code");
			}
			return validFixture || Fail(error,
				"fire-optics synthetic fixture is outside its physical range");
		}

		return Fail(error, "unsupported fire-optics record class or interpolation");
	}

	const FireOpticsPreset& FireOpticsPreset::PredictiveV1()
	{
		static const FireOpticsPreset preset = LoadEmbedded(
			kPredictiveFireOpticsV1, kPredictiveFireOpticsV1Size,
			kPredictiveFireOpticsV1SHA256 );
		return preset;
	}

	const FireOpticsPreset& FireOpticsPreset::SyntheticRegressionV1()
	{
		static const FireOpticsPreset preset = LoadEmbedded(
			kSyntheticFireOpticsV1, kSyntheticFireOpticsV1Size,
			kSyntheticFireOpticsV1SHA256 );
		return preset;
	}

	const FireOpticsPreset* FireOpticsPreset::Named( const char* name )
	{
		if( !name ) {
			return 0;
		}
		const FireOpticsPreset& predictive = PredictiveV1();
		if( predictive.IsValid() && (predictive.RecordName() == name ||
			std::string(name) == "fire_optics_v1") ) {
			return &predictive;
		}
		const FireOpticsPreset& synthetic = SyntheticRegressionV1();
		if( synthetic.IsValid() && (synthetic.RecordName() == name ||
			std::string(name) == "synthetic_regression_v1") ) {
			return &synthetic;
		}
		return 0;
	}

	FireOpticsPreset FireOpticsPreset::CreateExplicitSyntheticFixture(
		const double sootEffectiveAbsorption,
		const double sootDensityKgM3,
		const double hotAlbedo,
		const double hotG,
		const double coolKm633,
		const double coolExponent,
		const double coolAlbedo,
		const double coolG,
		const double condensedKm633,
		const double condensedExponent,
		const double condensedAlbedo,
		const double condensedG,
		std::string* error
		)
	{
		using RISECBOR64::Value;
		RISECBOR64::Value frozen;
		std::string decodeError;
		const FireOpticsPreset& frozenFixture = SyntheticRegressionV1();
		if( !frozenFixture.IsValid() ||
			!RISECBOR64::DecodeCanonical(frozenFixture.RecordBytes(),frozen,&decodeError) ) {
			Fail(error,"could not load the canonical synthetic metadata template");
			return FireOpticsPreset();
		}
		const Value* provenanceSchema = frozen.Find("provenance_schema");
		if( !provenanceSchema ) {
			Fail(error,"canonical synthetic metadata template has no provenance schema");
			return FireOpticsPreset();
		}
		auto envelope = []( const double value, const char* locator ) {
			return Map({
				{ "applicability", Value::String(
					"explicitly synthetic non-predictive regression fixture only") },
				{ "provenance", Map({
					{ "access", Value::String("runtime caller-supplied") },
					{ "citation", Value::String(
						"RISE explicit synthetic fire-optics construction API") },
					{ "locator", Value::String(locator) },
					{ "secondary_source", Value::Bool(false) }
				}) },
				{ "uncertainty", Map({
					{ "kind", Value::String("synthetic_exact") },
					{ "magnitude", Value::Unsigned(0) }
				}) },
				{ "value", Value::Float(value) }
			});
		};
		const Value phiPartition = FrozenPhiPartition();
		const Value effectiveEnvelope = envelope(
			sootEffectiveAbsorption,"CreateExplicitSyntheticFixture::sootEffectiveAbsorption");
		const Value densityEnvelope = envelope(
			sootDensityKgM3/1000.0,"CreateExplicitSyntheticFixture::sootDensityKgM3");
		const Value hotOmegaEnvelope = envelope(
			hotAlbedo,"CreateExplicitSyntheticFixture::hotAlbedo");
		const Value hotGEnvelope = envelope(
			hotG,"CreateExplicitSyntheticFixture::hotG");
		const Value coolKmEnvelope = envelope(
			coolKm633,"CreateExplicitSyntheticFixture::coolKm633");
		const Value coolExponentEnvelope = envelope(
			coolExponent,"CreateExplicitSyntheticFixture::coolExponent");
		const Value coolOmegaEnvelope = envelope(
			coolAlbedo,"CreateExplicitSyntheticFixture::coolAlbedo");
		const Value coolGEnvelope = envelope(
			coolG,"CreateExplicitSyntheticFixture::coolG");
		const Value condensedKmEnvelope = envelope(
			condensedKm633,"CreateExplicitSyntheticFixture::condensedKm633");
		const Value condensedExponentEnvelope = envelope(
			condensedExponent,"CreateExplicitSyntheticFixture::condensedExponent");
		const Value condensedOmegaEnvelope = envelope(
			condensedAlbedo,"CreateExplicitSyntheticFixture::condensedAlbedo");
		const Value condensedGEnvelope = envelope(
			condensedG,"CreateExplicitSyntheticFixture::condensedG");
		const Value effective = Map({
			{ "E_eff", effectiveEnvelope },
			{ "domain_nm", FloatArray({380.0, 780.0}) },
			{ "model", Value::String("constant_E_eff") },
			{ "pinned_density_g_cm3", Value::Float(sootDensityKgM3/1000.0) },
			{ "pinned_density_metadata", densityEnvelope }
		});
		const Value hot = Map({
			{ "g", hotGEnvelope },
			{ "omega", hotOmegaEnvelope },
			{ "phi_T_partition", phiPartition }
		});
		const Value cool = Map({
			{ "g", coolGEnvelope },
			{ "k_m_extinction_633nm_m2_per_g", Value::Float(coolKm633) },
			{ "k_m_extinction_metadata", coolKmEnvelope },
			{ "n_spectral_exponent", coolExponentEnvelope },
			{ "omega", coolOmegaEnvelope },
			{ "phi_T_partition", phiPartition }
		});
		const Value condensed = Map({
			{ "g", condensedGEnvelope },
			{ "k_m_extinction_633nm_m2_per_g", Value::Float(condensedKm633) },
			{ "k_m_extinction_metadata", condensedKmEnvelope },
			{ "n_spectral_exponent", condensedExponentEnvelope },
			{ "omega", condensedOmegaEnvelope },
			{ "predictive_reason_code", Value::String("condensed_organics_ir_unclosed") }
		});
		const char explicitStatus[] =
			"SYNTHETIC_NON_PREDICTIVE__EXPLICIT_FIXTURE_ONLY";
		const Value explicitSource = Map({
			{ "cool_phi_T_partition", phiPartition },
			{ "hot_phi_T_partition", phiPartition },
			{ "record_kind", Value::String("fire_optics_synthetic_fixture") },
			{ "record_name", Value::String(
				"fire-optics-explicit-synthetic-fixtures-v1") },
			{ "record_status", Value::String(explicitStatus) },
			{ "schema_version", Value::String("draft-2") },
			{ "values", Map({
				{ "E_eff", effectiveEnvelope },
				{ "condensed_g", condensedGEnvelope },
				{ "condensed_k_m_633", condensedKmEnvelope },
				{ "condensed_n", condensedExponentEnvelope },
				{ "condensed_omega", condensedOmegaEnvelope },
				{ "cool_g", coolGEnvelope },
				{ "cool_k_m_633", coolKmEnvelope },
				{ "cool_n", coolExponentEnvelope },
				{ "cool_omega", coolOmegaEnvelope },
				{ "density_g_cm3", densityEnvelope },
				{ "hot_g", hotGEnvelope },
				{ "hot_omega", hotOmegaEnvelope }
			}) },
			{ "version", Value::String("1.0.0-explicit-test") }
		});
		const Value record = Map({
			{ "component_record_statuses", Map({
				{ "explicit_synthetic_fixtures", Value::String(explicitStatus) }
			}) },
			{ "condensed_organics", condensed },
			{ "cool_carbon", cool },
			{ "effective_absorption", effective },
			{ "hot_soot", hot },
			{ "interpolation", Value::String("analytic_fixture_v1") },
			{ "provenance_schema", *provenanceSchema },
			{ "record_class", Value::String("synthetic_regression_fixture") },
			{ "record_kind", Value::String("fire_optics_preset") },
			{ "record_name", Value::String("fire-optics-explicit-synthetic-fixture") },
			{ "record_status", Value::String(explicitStatus) },
			{ "schema_version", Value::Unsigned(1) },
			{ "source_records", Map({
				{ "explicit_synthetic_fixtures", explicitSource }
			}) },
			{ "version", Value::String("1.0.0-explicit-test") }
		});
		RISECBOR64::Bytes encoded;
		FireOpticsPreset result;
		if( !RISECBOR64::Encode(record, encoded, error) ||
			!result.LoadCanonicalRecord(encoded, error) ) {
			return FireOpticsPreset();
		}
		return result;
	}

	double FireOpticsPreset::HotFraction( const double temperatureK ) const
	{
		if( temperatureK <= m_hotFractionMinK ) return 0.0;
		if( temperatureK >= m_hotFractionMaxK ) return 1.0;
		const double u = (temperatureK-m_hotFractionMinK)/
			(m_hotFractionMaxK-m_hotFractionMinK);
		return u*u*(3.0-2.0*u);
	}

	double FireOpticsPreset::MAC( const double wavelengthNM ) const
	{
		if( m_recordClass == PredictiveOpticalPreset ) {
			return m_mac.Evaluate(wavelengthNM);
		}
		return 6.0*kPi*m_constantEffectiveAbsorption/
			(m_densityGCM3*1000000.0*wavelengthNM*1.0e-9);
	}

	double FireOpticsPreset::EffectiveAbsorption( const double wavelengthNM ) const
	{
		if( m_recordClass == PredictiveOpticalPreset ) {
			return MAC(wavelengthNM)*(m_densityGCM3*1000000.0)*wavelengthNM*1.0e-9/
				(6.0*kPi);
		}
		return m_constantEffectiveAbsorption;
	}

	double FireOpticsPreset::EffectiveAbsorptionDerivative( const double wavelengthNM ) const
	{
		if( m_recordClass != PredictiveOpticalPreset ) {
			return 0.0;
		}
		return (m_densityGCM3*1000000.0)*1.0e-9/(6.0*kPi)*
			(MAC(wavelengthNM)+wavelengthNM*m_mac.Derivative(wavelengthNM));
	}

	double FireOpticsPreset::HotAbsorptionMass( const double wavelengthNM ) const
	{
		const double volumeFractionPerGM3 = 1.0e-3/SootDensityKgM3();
		return 6.0*kPi*EffectiveAbsorption(wavelengthNM)*volumeFractionPerGM3/
			(wavelengthNM*1.0e-9);
	}

	double FireOpticsPreset::HotAlbedo( const double wavelengthNM ) const
	{
		return m_hotOmega.Evaluate(wavelengthNM);
	}

	double FireOpticsPreset::HotG( const double wavelengthNM ) const
	{
		return m_hotG.Evaluate(wavelengthNM);
	}

	double FireOpticsPreset::HotExtinctionMass( const double wavelengthNM ) const
	{
		return HotAbsorptionMass(wavelengthNM)/(1.0-HotAlbedo(wavelengthNM));
	}

	double FireOpticsPreset::CoolExtinctionMass( const double wavelengthNM ) const
	{
		return m_coolKm633*std::pow(633.0/wavelengthNM, m_coolExponent);
	}

	double FireOpticsPreset::CoolAlbedo( const double ) const
	{
		return m_coolOmega;
	}

	double FireOpticsPreset::CoolG( const double ) const
	{
		return m_coolG;
	}

	double FireOpticsPreset::CondensedExtinctionMass( const double wavelengthNM ) const
	{
		if( m_recordClass == PredictiveOpticalPreset ) {
			return m_condensedKm.Evaluate(wavelengthNM);
		}
		return m_condensedFixtureKm633*
			std::pow(633.0/wavelengthNM, m_condensedFixtureExponent);
	}

	double FireOpticsPreset::CondensedAlbedo( const double wavelengthNM ) const
	{
		return m_recordClass == PredictiveOpticalPreset ?
			m_condensedOmega.Evaluate(wavelengthNM) : m_condensedFixtureOmega;
	}

	double FireOpticsPreset::CondensedG( const double wavelengthNM ) const
	{
		return m_recordClass == PredictiveOpticalPreset ?
			m_condensedG.Evaluate(wavelengthNM) : m_condensedFixtureG;
	}

	double FireOpticsPreset::MaximumExtinctionMassVisible() const
	{
		const double maximumHot = MaximumHotAbsorptionMassVisible()/
			(1.0-MaximumValue(m_hotOmega.Values()));
		const double maximumCool = std::max(
			CoolExtinctionMass(m_domainMinNM), CoolExtinctionMass(m_domainMaxNM) );
		const double maximumCondensed = m_recordClass == PredictiveOpticalPreset ?
			MaximumValue(m_condensedKm.Values()) : std::max(
				CondensedExtinctionMass(m_domainMinNM),
				CondensedExtinctionMass(m_domainMaxNM) );
		return std::max(maximumHot, std::max(maximumCool, maximumCondensed));
	}

	double FireOpticsPreset::MaximumHotAbsorptionMassVisible() const
	{
		return m_recordClass == PredictiveOpticalPreset ? MaximumValue(m_mac.Values()) :
			std::max(HotAbsorptionMass(m_domainMinNM),
				HotAbsorptionMass(m_domainMaxNM));
	}

	double FireOpticsPreset::MaximumCoolAbsorptionMassVisible() const
	{
		return std::max(CoolExtinctionMass(m_domainMinNM),
			CoolExtinctionMass(m_domainMaxNM))*(1.0-m_coolOmega);
	}

	double FireOpticsPreset::MaximumCondensedAbsorptionMassVisible() const
	{
		const double maximumExtinction = m_recordClass == PredictiveOpticalPreset ?
			MaximumValue(m_condensedKm.Values()) : std::max(
				CondensedExtinctionMass(m_domainMinNM),
				CondensedExtinctionMass(m_domainMaxNM) );
		const double minimumAlbedo = m_recordClass == PredictiveOpticalPreset ?
			MinimumValue(m_condensedOmega.Values()) : m_condensedFixtureOmega;
		return maximumExtinction*(1.0-minimumAlbedo);
	}

	double FireOpticsPreset::PelApproximateCondensedKm633() const
	{
		return CondensedExtinctionMass(633.0);
	}

	double FireOpticsPreset::PelApproximateCondensedExponent() const
	{
		return m_recordClass == PredictiveOpticalPreset ?
			m_condensedPreviewExponent : m_condensedFixtureExponent;
	}

	double FireOpticsPreset::PelApproximateCondensedAlbedo() const
	{
		return CondensedAlbedo(633.0);
	}

	double FireOpticsPreset::PelApproximateCondensedG() const
	{
		return CondensedG(633.0);
	}

	FireFidelityEvaluation FireOpticsPreset::EvaluateFidelity(
		const bool predictiveRequested,
		const bool hasNonzeroCondensedInventory,
		const bool hasChemChannels
		) const
	{
		FireFidelityEvaluation result;
		result.predictiveAllowed = false;
		result.renderFidelityStatus = "preview";
		if( hasChemChannels ) {
			result.reasonCodes.push_back("missing_chem_record");
		} else {
			result.reasonCodes.push_back("chem_none_unqualified");
		}
		if( hasNonzeroCondensedInventory ) {
			result.reasonCodes.push_back(m_condensedPredictiveReason);
		}
		result.reasonCodes.push_back("producer_unqualified");
		if( !predictiveRequested ) {
			result.reasonCodes.push_back("requested_preview");
		}
		if( IsSynthetic() ) {
			result.reasonCodes.push_back("qualified_record_override");
		}
		std::sort(result.reasonCodes.begin(), result.reasonCodes.end());
		result.reasonCodes.erase(
			std::unique(result.reasonCodes.begin(), result.reasonCodes.end()),
			result.reasonCodes.end() );
		return result;
	}

	bool FireOpticsPreset::SupportsWavelengthRange(
		const double minimumNM,
		const double maximumNM
		) const
	{
		return m_valid && IsFinite(minimumNM) && IsFinite(maximumNM) &&
			minimumNM <= maximumNM && minimumNM >= m_domainMinNM &&
			maximumNM <= m_domainMaxNM && minimumNM >= m_coolDomainMinNM &&
			maximumNM <= m_coolDomainMaxNM;
	}
}
