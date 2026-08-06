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
		std::string kind, recordClass, interpolation;
		if( !schema || schema->GetIntegerArgument() != 1 ||
			!ReadText(record, "record_kind", kind, error) || kind != "fire_optics_preset" ||
			!ReadText(record, "record_name", m_recordName, error) ||
			!ReadText(record, "record_class", recordClass, error) ||
			!ReadText(record, "interpolation", interpolation, error) ) {
			return Fail(error, "unsupported fire-optics record header");
		}
		const RISECBOR64::Value* effective = Required(
			record, "effective_absorption", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* hot = Required(
			record, "hot_soot", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* cool = Required(
			record, "cool_carbon", RISECBOR64::Value::Map, error );
		const RISECBOR64::Value* condensed = Required(
			record, "condensed_organics", RISECBOR64::Value::Map, error );
		std::vector<double> hotFractionBand;
		if( !effective || !hot || !cool || !condensed ||
			!ReadFloatArray(record, "hot_fraction_temperature_band_K",
				hotFractionBand, error) || hotFractionBand.size() != 2 ||
			hotFractionBand[0] >= hotFractionBand[1] ||
			!ReadDomain(*effective, m_domainMinNM, m_domainMaxNM, error) ||
			!ReadFloat(*effective, "pinned_density_g_cm3", m_densityGCM3, error) ||
			m_densityGCM3 <= 0.0 ) {
			return false;
		}
		m_hotFractionMinK = hotFractionBand[0];
		m_hotFractionMaxK = hotFractionBand[1];

		if( recordClass == "predictive_optical_preset" &&
			interpolation == "pchip_monotone_c1_v1" ) {
			m_recordClass = PredictiveOpticalPreset;
			std::string normative;
			if( !ReadText(*effective, "normative_quantity", normative, error) ||
				normative != "MAC_lambda_m2_per_g" ) {
				return Fail(error, "fire-optics normative quantity is not MAC");
			}
			std::vector<std::vector<double> > effectiveRows;
			if( !ReadRows(*effective, 3, effectiveRows, error) || effectiveRows.size() != 81 ) {
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
				const double derived = effectiveRows[i][2]*(m_densityGCM3*1000000.0)*
					effectiveRows[i][0]*1.0e-9/(6.0*kPi);
				if( !NearlyEqual(derived, effectiveRows[i][1], 5.0e-6) ) {
					return Fail(error, "fire-optics E_eff row disagrees with normative MAC");
				}
			}
			if( !ReadInterpolation(*effective, "mac_interpolation", wavelengths,
				macValues, m_mac, error) ) {
				return false;
			}
			std::vector<std::vector<double> > hotRows;
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
			if( !ReadFloatArray(*cool, "n_supported_range", supportedRange, error) ||
				supportedRange.size() != 2 || supportedRange[0] != 1.0 ||
				supportedRange[1] != 1.2 || m_coolKm633 <= 0.0 ||
				m_coolExponent < supportedRange[0] ||
				m_coolExponent > supportedRange[1] || m_coolOmega < 0.0 ||
				m_coolOmega >= 1.0 || m_coolG <= -1.0 || m_coolG >= 1.0 ) {
				return Fail(error, "fire-optics cool-carbon exponent range is not frozen v1");
			}
			std::vector<std::vector<double> > condensedRows;
			if( !ReadRows(*condensed, 4, condensedRows, error) || condensedRows.size() != 5 ) {
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

			if( m_hotFractionMinK != 700.0 || m_hotFractionMaxK != 900.0 ||
				m_densityGCM3 != 1.8 || !NearlyEqual(MAC(550.0), 8.0, 1.0e-12) ||
				!NearlyEqual(MAC(632.8), 6.647, 5.0e-4) ||
				!NearlyEqual(EffectiveAbsorption(380.0)/EffectiveAbsorption(780.0),
					1.311, 5.0e-4) ||
				!NearlyEqual(-std::log(MAC(380.0)/MAC(780.0))/
					std::log(380.0/780.0), 1.377, 5.0e-4) ||
				!NearlyEqual(HotAlbedo(550.0), 0.10, 2.0e-3) ||
				!NearlyEqual(HotG(550.0), 0.22, 1.0e-12) ||
				!NearlyEqual(m_coolKm633, 8.7, 1.0e-12) ||
				!NearlyEqual(m_coolOmega, 0.25, 1.0e-12) ||
				!NearlyEqual(m_coolG, 0.58, 1.0e-12) ||
				m_condensedPredictiveReason != "condensed_organics_ir_unclosed" ||
				m_condensedIRClosureStatus != "blocked" ) {
				return Fail(error, "fire-optics predictive v1 numeric gate failed");
			}
			return true;
		}

		if( recordClass == "synthetic_regression_fixture" &&
			interpolation == "analytic_fixture_v1" ) {
			m_recordClass = SyntheticRegressionFixture;
			double hotOmega;
			if( !ReadFloat(*effective, "E_eff", m_constantEffectiveAbsorption, error) ||
				!ReadFloat(*hot, "omega", hotOmega, error) ) {
				return false;
			}
			double hotGValue;
			if( !ReadFloat(*hot, "g", hotGValue, error) ||
				!ReadFloat(*cool, "k_m_extinction_633nm_m2_per_g", m_coolKm633, error) ||
				!ReadFloat(*cool, "n_spectral_exponent", m_coolExponent, error) ||
				!ReadFloat(*cool, "omega", m_coolOmega, error) ||
				!ReadFloat(*cool, "g", m_coolG, error) ||
				!ReadFloat(*condensed, "k_m_extinction_633nm_m2_per_g", m_condensedFixtureKm633, error) ||
				!ReadFloat(*condensed, "n_spectral_exponent", m_condensedFixtureExponent, error) ||
				!ReadFloat(*condensed, "omega", m_condensedFixtureOmega, error) ||
				!ReadFloat(*condensed, "g", m_condensedFixtureG, error) ||
				!ReadText(*condensed, "predictive_reason_code", m_condensedPredictiveReason, error) ) {
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
		const Value effective = Map({
			{ "E_eff", Value::Float(sootEffectiveAbsorption) },
			{ "domain_nm", FloatArray({380.0, 780.0}) },
			{ "model", Value::String("constant_E_eff") },
			{ "pinned_density_g_cm3", Value::Float(sootDensityKgM3/1000.0) }
		});
		const Value hot = Map({
			{ "g", Value::Float(hotG) },
			{ "omega", Value::Float(hotAlbedo) }
		});
		const Value cool = Map({
			{ "g", Value::Float(coolG) },
			{ "k_m_extinction_633nm_m2_per_g", Value::Float(coolKm633) },
			{ "n_spectral_exponent", Value::Float(coolExponent) },
			{ "omega", Value::Float(coolAlbedo) }
		});
		const Value condensed = Map({
			{ "g", Value::Float(condensedG) },
			{ "k_m_extinction_633nm_m2_per_g", Value::Float(condensedKm633) },
			{ "n_spectral_exponent", Value::Float(condensedExponent) },
			{ "omega", Value::Float(condensedAlbedo) },
			{ "predictive_reason_code", Value::String("condensed_organics_ir_unclosed") }
		});
		const Value record = Map({
			{ "condensed_organics", condensed },
			{ "cool_carbon", cool },
			{ "effective_absorption", effective },
			{ "hot_fraction_temperature_band_K", Value::ArrayValue({
				Value::Float(700.0), Value::Float(900.0) }) },
			{ "hot_soot", hot },
			{ "interpolation", Value::String("analytic_fixture_v1") },
			{ "record_class", Value::String("synthetic_regression_fixture") },
			{ "record_kind", Value::String("fire_optics_preset") },
			{ "record_name", Value::String("fire-optics-explicit-synthetic-fixture") },
			{ "schema_version", Value::Unsigned(1) }
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
}
