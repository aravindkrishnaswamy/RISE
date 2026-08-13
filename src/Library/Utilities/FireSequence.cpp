//////////////////////////////////////////////////////////////////////
//
//  FireSequence.cpp - Canonical Phase-C fire sequence contract
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireSequence.h"
#include "FireSimulationRecords.h"
#include "../Rendering/FrameStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#endif

namespace RISE
{
	namespace Implementation
	{
		namespace
		{
			using RISECBOR64::Value;

			bool Fail( std::string& error, const std::string& message )
			{
				error = message;
				return false;
			}

			bool ExactKeys( const Value& value, std::initializer_list<const char*> keys )
			{
				if( value.GetType() != Value::Map || value.GetMap().size() != keys.size() ) {
					return false;
				}
				for( const char* key : keys ) if( !value.Find(key) ) return false;
				return true;
			}

			bool ReadFinite( const Value* value, double& result )
			{
				if( !value || value->GetType() != Value::Float64 ) return false;
				result = value->GetFloat();
				return std::isfinite(result);
			}

			bool ReadSigned( const Value* value, std::int64_t& result )
			{
				if( !value ) return false;
				if( value->GetType() == Value::UnsignedInteger ) {
					if( value->GetIntegerArgument() > static_cast<std::uint64_t>(
						std::numeric_limits<std::int64_t>::max()) ) return false;
					result = static_cast<std::int64_t>(value->GetIntegerArgument());
					return true;
				}
				if( value->GetType() != Value::NegativeInteger ||
					value->GetIntegerArgument() >= static_cast<std::uint64_t>(
						std::numeric_limits<std::int64_t>::max()) ) return false;
				result = -1-static_cast<std::int64_t>(value->GetIntegerArgument());
				return true;
			}

			bool IsDigest( const std::string& digest )
			{
				if( digest.size() != 64u ) return false;
				for( char c : digest ) if( !((c >= '0' && c <= '9') ||
					(c >= 'a' && c <= 'f')) ) return false;
				return true;
			}

			template<std::size_t N>
			bool ReadFloatArray( const Value* value, std::array<double,N>& output );

			bool ValidateAerosolThermochemistryRecord(
				const RISECBOR64::Bytes& bytes,
				const FireSimulationMethaneRecord& fuel,
				double& minimumK, double& maximumK, std::string& error )
			{
				Value record;
				if( !RISECBOR64::DecodeCanonical(bytes,record,&error) ||
					!ExactKeys(record,{"carbon_phase","condensable_stream","record_kind",
						"schema_version","status","temperature_domain_K"}) ) return Fail(error,
						"fire sequence aerosol thermochemistry record schema is invalid");
				const Value* kind=record.Find("record_kind"), *schema=record.Find("schema_version"),
					*status=record.Find("status"), *carbon=record.Find("carbon_phase"),
					*condensable=record.Find("condensable_stream"), *domain=record.Find("temperature_domain_K");
				std::array<double,2> limits{};
				const Value* species=carbon ? carbon->Find("species_id") : nullptr;
				const Value* phase=carbon ? carbon->Find("phase") : nullptr;
				const Value* source=carbon ? carbon->Find("source_fuel_record_id") : nullptr;
				const Value* interpolation=carbon ? carbon->Find("interpolation") : nullptr;
				const Value* commonReference=carbon ? carbon->Find("common_T_ref_K") : nullptr;
				double referenceK=0.0, cp=0.0, sensible=0.0;
				if( !kind || kind->GetType()!=Value::Text || kind->GetText()!="fire-aerosol-thermochemistry-v1" ||
					!schema || schema->GetType()!=Value::UnsignedInteger || schema->GetIntegerArgument()!=1u ||
					!status || status->GetType()!=Value::Text || status->GetText()!="preview_methane_zero_yield" ||
					!carbon || !ExactKeys(*carbon,{"common_T_ref_K","interpolation","phase",
						"source_fuel_record_id","species_id"}) ||
					!species || species->GetType()!=Value::Text || species->GetText()!="C(gr)" ||
					!phase || phase->GetType()!=Value::Text || phase->GetText()!="solid" ||
					!source || source->GetType()!=Value::Text || source->GetText()!=fuel.RecordId() ||
					!interpolation || interpolation->GetType()!=Value::Text ||
						interpolation->GetText()!="nasa9_piecewise_cp_hs" ||
					!ReadFinite(commonReference,referenceK) || referenceK!=fuel.ReferenceTemperatureK() ||
					!condensable || !ExactKeys(*condensable,{"kind","reason"}) ||
					!condensable->Find("kind") || condensable->Find("kind")->GetType()!=Value::Text ||
						condensable->Find("kind")->GetText()!="none" ||
					!condensable->Find("reason") || condensable->Find("reason")->GetType()!=Value::Text ||
						condensable->Find("reason")->GetText()!="methane_has_no_condensable_organic_stream" ||
					!ReadFloatArray(domain,limits) || limits[0]!=fuel.TemperatureMinK() ||
					limits[1]!=fuel.TemperatureMaxK() ||
					!fuel.CpJPerKGK("C(gr)",referenceK,cp,&error) ||
					!fuel.SensibleEnthalpyJPerKG("C(gr)",referenceK,sensible,&error) ||
					!std::isfinite(cp) || cp<=0.0 || !std::isfinite(sensible) ) return Fail(error,
						"fire sequence aerosol thermochemistry record semantics are invalid");
				minimumK=limits[0]; maximumK=limits[1]; return true;
			}

			bool ValidateChemRecord( const RISECBOR64::Bytes& bytes, bool& hasChem,
				std::array<std::array<double,2>,3>& intervalsNM, std::string& error )
			{
				Value record;
				if( !RISECBOR64::DecodeCanonical(bytes,record,&error) || record.GetType()!=Value::Map )
					return Fail(error,"fire sequence chem record schema is invalid");
				const Value* kind=record.Find("record_kind");
				const Value* schema=record.Find("schema_version");
				if( !kind || kind->GetType()!=Value::Text || !schema ||
					schema->GetType()!=Value::UnsignedInteger || schema->GetIntegerArgument()!=1u )
					return Fail(error,"fire sequence chem record kind/version is invalid");
				if( kind->GetText()=="fire-chem-none-v1" ) {
					if( !ExactKeys(record,{"chem_model","provenance","record_kind","schema_version"}) )
						return Fail(error,"fire sequence chem-none record schema is invalid");
					const Value* model=record.Find("chem_model"),*provenance=record.Find("provenance");
					if( !model || model->GetType()!=Value::Text || model->GetText()!="none" ||
						!provenance || provenance->GetType()!=Value::Text || provenance->GetText().empty() )
						return Fail(error,"fire sequence chem-none record semantics are invalid");
					hasChem=false;
					intervalsNM={{{{0.0,0.0}},{{0.0,0.0}},{{0.0,0.0}}}};
					return true;
				}
				if( kind->GetText()!="fire-chem-synthetic-fixture-v1" ||
					!ExactKeys(record,{"absolute_calibration","bands","provenance","record_class",
						"record_kind","schema_version","state_domain","wavelength_unit"}) )
					return Fail(error,"fire sequence non-none chem record is not an approved fixture schema");
				const Value* recordClass=record.Find("record_class");
				const Value* bands=record.Find("bands");
				const Value* calibration=record.Find("absolute_calibration");
				const Value* provenance=record.Find("provenance");
				const Value* stateDomain=record.Find("state_domain");
				const Value* wavelengthUnit=record.Find("wavelength_unit");
				if( !recordClass || recordClass->GetType()!=Value::Text ||
					recordClass->GetText()!="SYNTHETIC_NON_PREDICTIVE" || !bands ||
					bands->GetType()!=Value::Array || bands->GetArray().size()!=3u ||
					!calibration || calibration->GetType()!=Value::Text ||
					calibration->GetText()!="input_absolute_band_power_W_per_m3" ||
					!provenance || provenance->GetType()!=Value::Text || provenance->GetText().empty() ||
					!stateDomain || stateDomain->GetType()!=Value::Text ||
					stateDomain->GetText()!="finite_nonnegative_absolute_channel_values" ||
					!wavelengthUnit || wavelengthUnit->GetType()!=Value::Text ||
					wavelengthUnit->GetText()!="nm" )
					return Fail(error,"fire sequence synthetic chem fixture classification is invalid");
				static const char* names[3]={"CH","C2","CO2"};
				for( unsigned int band=0; band<3u; ++band ) {
					const Value& entry=bands->GetArray()[band];
					if( !ExactKeys(entry,{"band","normalization_interval_nm","normalization_rule",
						"spd_shape"}) )
						return Fail(error,"fire sequence synthetic chem band schema is invalid");
					const Value* name=entry.Find("band"),*shape=entry.Find("spd_shape"),
						*interval=entry.Find("normalization_interval_nm"),
						*normalization=entry.Find("normalization_rule");
					if( !name || name->GetType()!=Value::Text || name->GetText()!=names[band] ||
						!shape || shape->GetType()!=Value::Text || shape->GetText()!="uniform_unit_shape" ||
						!normalization || normalization->GetType()!=Value::Text ||
						normalization->GetText()!="trapezoid_1nm_then_divide_once" ||
						!interval || interval->GetType()!=Value::Array || interval->GetArray().size()!=2u ||
						!ReadFinite(&interval->GetArray()[0],intervalsNM[band][0]) ||
						!ReadFinite(&interval->GetArray()[1],intervalsNM[band][1]) ||
						intervalsNM[band][0]<380.0 || intervalsNM[band][1]>780.0 ||
						intervalsNM[band][1]<=intervalsNM[band][0] )
						return Fail(error,"fire sequence synthetic chem band semantics are invalid");
				}
				hasChem=true;
				return true;
			}

			bool ReadTextArray( const Value* value, std::vector<std::string>& output )
			{
				if( !value || value->GetType() != Value::Array ) return false;
				output.clear();
				for( const Value& item : value->GetArray() ) {
					if( item.GetType() != Value::Text ) return false;
					output.push_back(item.GetText());
				}
				return true;
			}

			template<std::size_t N>
			bool ReadFloatArray( const Value* value, std::array<double,N>& output )
			{
				if( !value || value->GetType() != Value::Array ||
					value->GetArray().size() != N ) return false;
				for( std::size_t i=0; i<N; ++i ) {
					if( !ReadFinite(&value->GetArray()[i],output[i]) ) return false;
				}
				return true;
			}

			bool ReadDimensions( const Value* value, std::array<std::uint64_t,3>& output )
			{
				if( !value || value->GetType() != Value::Array ||
					value->GetArray().size() != 3u ) return false;
				for( std::size_t i=0; i<3u; ++i ) {
					const Value& v = value->GetArray()[i];
					if( v.GetType() != Value::UnsignedInteger || v.GetIntegerArgument() < 2u ||
						v.GetIntegerArgument() > 0x7fffffffu ) return false;
					output[i] = v.GetIntegerArgument();
				}
				return true;
			}

			bool CheckedVoxelCount( const std::array<std::uint64_t,3>& dimensions,
				const std::uint64_t limit, std::uint64_t& count )
			{
				count = 1u;
				for( const std::uint64_t dimension : dimensions ) {
					if( dimension == 0u || count > limit/dimension ) return false;
					count *= dimension;
				}
				return count <= limit;
			}

			bool SafeRelativePath( const std::string& text )
			{
				if( text.empty() || !RISECBOR64::IsUTF8NFC(text) ) return false;
				const std::filesystem::path path(text);
				if( path.is_absolute() || path.has_root_name() || path.has_root_directory() ) {
					return false;
				}
				for( const auto& component : path ) {
					if( component == ".." || component == "." || component.empty() ) return false;
				}
				return true;
			}

			bool SameBits( const double a, const double b )
			{
				std::uint64_t aa = 0, bb = 0;
				std::memcpy(&aa,&a,sizeof(a));
				std::memcpy(&bb,&b,sizeof(b));
				return aa == bb;
			}

			bool ValidateScalarValue( const std::string& channel, const double value,
				const double temperatureMinimum, const double temperatureMaximum,
				std::string& error )
			{
				if( !std::isfinite(value) ) return Fail(error,
					"fire sequence channel contains a non-finite stored value: "+channel);
				if( channel == "temperature" ) {
					if( value <= 0.0 || value < temperatureMinimum ||
						value > temperatureMaximum ) return Fail(error,
						"fire sequence temperature is outside the common certified domain");
				} else if( channel == "carbon" || channel == "condensed" ||
					channel == "reaction" || channel.rfind("chem_",0u) == 0u ) {
					if( value < 0.0 ) return Fail(error,
						"fire sequence nonnegative channel contains a negative stored value: "+channel);
				}
				return true;
			}

#if defined(RISE_ENABLE_OPENVDB)
			bool ValidateTransform( const openvdb::GridBase& grid,
				const FireSequenceChannelDescriptor& descriptor, std::string& error )
			{
				const openvdb::Vec3d p0 = grid.transform().indexToWorld(openvdb::Vec3d(0,0,0));
				const openvdb::Vec3d px = grid.transform().indexToWorld(openvdb::Vec3d(1,0,0));
				const openvdb::Vec3d py = grid.transform().indexToWorld(openvdb::Vec3d(0,1,0));
				const openvdb::Vec3d pz = grid.transform().indexToWorld(openvdb::Vec3d(0,0,1));
				for( unsigned int axis=0; axis<3u; ++axis ) {
					if( !SameBits(p0[axis],descriptor.originMeters[axis]) ) return Fail(error,
						"fire sequence OpenVDB origin differs from the manifest lattice");
				}
				const openvdb::Vec3d dx = px-p0, dy = py-p0, dz = pz-p0;
				const openvdb::Vec3d steps[3] = {dx,dy,dz};
				for( unsigned int column=0; column<3u; ++column ) {
					for( unsigned int row=0; row<3u; ++row ) {
						const double expected = row == column ?
							descriptor.voxelSizeMeters[column] : 0.0;
						if( !SameBits(steps[column][row],expected) ) return Fail(error,
							"fire sequence OpenVDB transform is not the manifest axis-aligned lattice");
					}
				}
				return true;
			}

			template<class GridType>
			bool LoadScalarGrid( const typename GridType::Ptr& grid,
				const FireSequenceChannelDescriptor& descriptor,
				const double temperatureMinimum, const double temperatureMaximum,
				FireSequenceDenseChannel& output, std::string& error )
			{
				const double background = static_cast<double>(grid->background());
				if( descriptor.background.size() != 1u ||
					!SameBits(background,descriptor.background[0]) ||
					!ValidateScalarValue(descriptor.name,background,temperatureMinimum,
						temperatureMaximum,error) ) return error.empty() ? Fail(error,
						"fire sequence scalar background differs from its manifest") : false;

				std::uint64_t count64 = 0;
				if( !CheckedVoxelCount(descriptor.dimensions,0x7fffffffu,count64) ) return Fail(error,
					"fire sequence dense renderer lattice exceeds the supported voxel count");
				output.name = descriptor.name;
				output.vectorValues = false;
				output.dimensions = descriptor.dimensions;
				output.values.assign(static_cast<std::size_t>(count64),background);
				output.minimum = background;
				output.maximum = background;

				for( auto iter=grid->tree().cbeginValueAll(); iter; ++iter ) {
					const double value = static_cast<double>(*iter);
					if( !ValidateScalarValue(descriptor.name,value,temperatureMinimum,
						temperatureMaximum,error) ) return false;
					if( !iter.isValueOn() && !SameBits(value,background) ) return Fail(error,
						"fire sequence stored value-off scalar payload differs from background");
					output.minimum = std::min(output.minimum,value);
					output.maximum = std::max(output.maximum,value);
					if( !iter.isValueOn() ) continue;
					const openvdb::CoordBBox bbox = iter.getBoundingBox();
					if( bbox.min().x() < 0 || bbox.min().y() < 0 || bbox.min().z() < 0 ||
						bbox.max().x() >= static_cast<int>(descriptor.dimensions[0]) ||
						bbox.max().y() >= static_cast<int>(descriptor.dimensions[1]) ||
						bbox.max().z() >= static_cast<int>(descriptor.dimensions[2]) ) return Fail(error,
						"fire sequence active scalar topology exceeds the manifest dimensions");
					for( openvdb::CoordBBox::Iterator<true> voxel=bbox.begin(); voxel; ++voxel ) {
						const openvdb::Coord c = *voxel;
						const std::size_t linear = static_cast<std::size_t>(c.x())+
							static_cast<std::size_t>(descriptor.dimensions[0])*(
							static_cast<std::size_t>(c.y())+
							static_cast<std::size_t>(descriptor.dimensions[1])*static_cast<std::size_t>(c.z()));
						output.values[linear] = value;
					}
				}
				return true;
			}

			bool LoadVectorGrid( const openvdb::Vec3fGrid::Ptr& grid,
				const FireSequenceChannelDescriptor& descriptor,
				FireSequenceDenseChannel& output, std::string& error )
			{
				const openvdb::Vec3f background = grid->background();
				if( descriptor.background.size() != 3u ) return Fail(error,
					"fire sequence velocity background is not a three-vector");
				for( unsigned int component=0; component<3u; ++component ) {
					const double value = static_cast<double>(background[component]);
					if( !std::isfinite(value) || !SameBits(value,descriptor.background[component]) ) {
						return Fail(error,"fire sequence velocity background differs from its manifest");
					}
				}
				std::uint64_t count64 = 0;
				if( !CheckedVoxelCount(descriptor.dimensions,0x7fffffffu/3u,count64) ) return Fail(error,
					"fire sequence dense velocity lattice exceeds the supported voxel count");
				output.name = descriptor.name;
				output.vectorValues = true;
				output.dimensions = descriptor.dimensions;
				output.values.resize(static_cast<std::size_t>(count64)*3u);
				for( std::size_t i=0; i<static_cast<std::size_t>(count64); ++i ) {
					for( unsigned int c=0; c<3u; ++c ) output.values[3u*i+c] = background[c];
				}
				output.minimum = std::min({double(background[0]),double(background[1]),double(background[2])});
				output.maximum = std::max({double(background[0]),double(background[1]),double(background[2])});
				for( auto iter=grid->tree().cbeginValueAll(); iter; ++iter ) {
					const openvdb::Vec3f value = *iter;
					for( unsigned int c=0; c<3u; ++c ) {
						const double component = value[c];
						if( !std::isfinite(component) ) return Fail(error,
							"fire sequence velocity contains a non-finite component");
						if( !iter.isValueOn() && !SameBits(component,double(background[c])) ) return Fail(error,
							"fire sequence stored value-off velocity payload differs from background");
						output.minimum = std::min(output.minimum,component);
						output.maximum = std::max(output.maximum,component);
					}
					if( !iter.isValueOn() ) continue;
					const openvdb::CoordBBox bbox = iter.getBoundingBox();
					if( bbox.min().x() < 0 || bbox.min().y() < 0 || bbox.min().z() < 0 ||
						bbox.max().x() >= static_cast<int>(descriptor.dimensions[0]) ||
						bbox.max().y() >= static_cast<int>(descriptor.dimensions[1]) ||
						bbox.max().z() >= static_cast<int>(descriptor.dimensions[2]) ) return Fail(error,
						"fire sequence active velocity topology exceeds the manifest dimensions");
					for( openvdb::CoordBBox::Iterator<true> voxel=bbox.begin(); voxel; ++voxel ) {
						const openvdb::Coord c = *voxel;
						const std::size_t linear = static_cast<std::size_t>(c.x())+
							static_cast<std::size_t>(descriptor.dimensions[0])*(
							static_cast<std::size_t>(c.y())+
							static_cast<std::size_t>(descriptor.dimensions[1])*static_cast<std::size_t>(c.z()));
						for( unsigned int component=0; component<3u; ++component )
							output.values[3u*linear+component] = value[component];
					}
				}
				return true;
			}
#endif
		}

		bool FireSequenceManifest::LoadCanonicalEnvelope(
			const RISECBOR64::Bytes& envelope, const std::string& manifestDirectory,
			std::string& error )
		{
			*this = FireSequenceManifest();
			Value root;
			if( !RISECBOR64::DecodeCanonical(envelope,root,&error) ||
				!ExactKeys(root,{"payload","sequence_id"}) ) return Fail(error,
				"fire sequence envelope is not canonical {payload,sequence_id}");
			const Value* payload = root.Find("payload");
			const Value* sequenceId = root.Find("sequence_id");
			if( !payload || payload->GetType() != Value::Map || !sequenceId ||
				sequenceId->GetType() != Value::Text || !IsDigest(sequenceId->GetText()) ||
				payload->Find("sequence_id") ) return Fail(error,
				"fire sequence envelope has an invalid identity placement");
			if( !RISECBOR64::Encode(*payload,canonicalPayload_,&error) ||
				RISECBOR64::SHA256Hex(canonicalPayload_) != sequenceId->GetText() ) return Fail(error,
				"fire sequence_id does not bind the canonical payload bytes");

			if( !ExactKeys(*payload,{"aerosol_thermochemistry_record","aerosol_thermochemistry_record_id","case_record_id",
				"channels","chem_record","end_policy","frames","fuel_record","fuel_record_id",
				"chem_record_id",
				"first_frame_index","frame_count",
				"frame_encoding",
				"gas_opacity_record","gas_opacity_record_id","gas_thermochemistry_record",
				"gas_thermochemistry_record_id","gate_evidence_ids",
				"last_frame_index",
				"optical_record","optical_record_id","outside_halo_policy","physical_mapping",
				"producer_build_id","producer_build_v1","producer_reason_codes",
				"scene_translation_m","scene_unit_meters",
				"schema_version","source_kind","source_qualification","temperature_domain_K",
				"time_map","transport_closure_record","transport_closure_record_id",
				"velocity_halo_width_m"}) ) return Fail(error,
				"fire sequence payload is outside schema_version 1");
			const Value* schema = payload->Find("schema_version");
			if( !schema || schema->GetType() != Value::UnsignedInteger ||
				schema->GetIntegerArgument() != 1u ) return Fail(error,
				"fire sequence schema_version is unsupported");

			const Value* sourceKind = payload->Find("source_kind");
			const Value* mapping = payload->Find("physical_mapping");
			const Value* qualification = payload->Find("source_qualification");
			if( !sourceKind || sourceKind->GetType() != Value::Text ||
				!mapping || mapping->GetType() != Value::Text ||
				!qualification || qualification->GetType() != Value::Text ) return Fail(error,
				"fire sequence producer qualification fields are not text");
			sourceKind_ = sourceKind->GetText();
			physicalMapping_ = mapping->GetText();
			sourceQualification_ = qualification->GetText();
			if( sourceKind_ != "rise_simulation" && sourceKind_ != "qualified_external" &&
				sourceKind_ != "heuristic_import" ) return Fail(error,"fire sequence source_kind is unknown");
			// No normalized-field profile has been ratified in this renderer.  A
			// free-form heuristic tag cannot turn normalized bytes into SI values.
			if( physicalMapping_ != "absolute_si" ) return Fail(error,
				"fire sequence physical_mapping profile is unsupported");
			if( sourceQualification_ != "predictive_qualified" &&
				sourceQualification_ != "preview_only" ) return Fail(error,
				"fire sequence source_qualification is unknown");
			if( sourceQualification_ == "predictive_qualified" &&
				(sourceKind_ == "heuristic_import" || physicalMapping_ != "absolute_si") ) return Fail(error,
				"predictive-qualified sequence has a heuristic source or mapping");
			const Value* frameEncoding = payload->Find("frame_encoding");
			if( !frameEncoding || frameEncoding->GetType() != Value::Text ||
				frameEncoding->GetText() != "openvdb-v1" ) return Fail(error,
				"fire sequence frame_encoding is unsupported");

			const Value* producerBuild = payload->Find("producer_build_v1");
			const Value* producerBuildId = payload->Find("producer_build_id");
			Value decodedBuild;
			if( !producerBuild || producerBuild->GetType() != Value::ByteString ||
				!producerBuildId || producerBuildId->GetType() != Value::Text ||
				!RISECBOR64::DecodeCanonical(producerBuild->GetBytes(),decodedBuild,&error) ||
				decodedBuild.GetType() != Value::Map ||
				!FrameStoreOutput::ValidateRendererBuildIdentityV1(
					producerBuild->GetBytes(),error) ||
				RISECBOR64::SHA256Hex(producerBuild->GetBytes()) != producerBuildId->GetText() ) {
				return Fail(error,"fire sequence producer_build_id does not bind canonical build bytes");
			}
			producerBuildId_ = producerBuildId->GetText();
			if( !ReadTextArray(payload->Find("gate_evidence_ids"),gateEvidenceIds_) ||
				!std::is_sorted(gateEvidenceIds_.begin(),gateEvidenceIds_.end()) ||
				std::adjacent_find(gateEvidenceIds_.begin(),gateEvidenceIds_.end()) != gateEvidenceIds_.end() ) {
				return Fail(error,"fire sequence gate-evidence IDs are not sorted unique text");
			}
			if( sourceQualification_ == "predictive_qualified" && gateEvidenceIds_.empty() ) return Fail(error,
				"predictive-qualified sequence lacks gate evidence");
			for( const std::string& id : gateEvidenceIds_ ) if( !IsDigest(id) ) return Fail(error,
				"fire sequence gate-evidence ID is not a SHA-256 digest");
			std::vector<std::string> producerReasons;
			if( !ReadTextArray(payload->Find("producer_reason_codes"),producerReasons) ||
				!std::is_sorted(producerReasons.begin(),producerReasons.end()) ||
				std::adjacent_find(producerReasons.begin(),producerReasons.end()) != producerReasons.end() ) {
				return Fail(error,"fire sequence producer reason codes are not sorted unique text");
			}
			for( const std::string& reason : producerReasons ) if( reason.empty() ) return Fail(error,
				"fire sequence producer reason code is empty");
			if( sourceQualification_ == "predictive_qualified" && !producerReasons.empty() ) return Fail(error,
				"predictive-qualified sequence carries producer reason codes");
			if( sourceQualification_ == "preview_only" && producerReasons.empty() ) return Fail(error,
				"preview-only sequence lacks a producer reason code");

			const std::pair<const char*,const char*> embeddedRecords[] = {
				{"aerosol_thermochemistry_record","aerosol_thermochemistry_record_id"},
				{"chem_record","chem_record_id"},{"gas_opacity_record","gas_opacity_record_id"},
				{"fuel_record","fuel_record_id"},
				{"gas_thermochemistry_record","gas_thermochemistry_record_id"},
				{"optical_record","optical_record_id"},
				{"transport_closure_record","transport_closure_record_id"}
			};
			for( const auto& fields : embeddedRecords ) {
				const Value* bytes = payload->Find(fields.first);
				const Value* id = payload->Find(fields.second);
				Value record;
				if( !bytes || bytes->GetType() != Value::ByteString || bytes->GetBytes().empty() ||
					!RISECBOR64::DecodeCanonical(bytes->GetBytes(),record,&error) ||
					record.GetType() != Value::Map || !id || id->GetType() != Value::Text ||
					id->GetText() != RISECBOR64::SHA256Hex(bytes->GetBytes()) ) return Fail(error,
					std::string("fire sequence embedded record identity is invalid: ")+fields.first);
				embeddedRecords_[fields.first] = bytes->GetBytes();
			}
			opticalRecord_ = payload->Find("optical_record")->GetBytes();
			FireSimulationMethaneRecord fuelRecord;
			if( !fuelRecord.LoadCanonicalRecord(embeddedRecords_["fuel_record"],&error) ) {
				return Fail(error,"fire sequence fuel record is not semantically valid: "+error);
			}
			const double ambientTemperatureK = fuelRecord.ReferenceTemperatureK();
			double aerosolMinimumK=0.0,aerosolMaximumK=0.0;
			if( !ValidateAerosolThermochemistryRecord(
				embeddedRecords_["aerosol_thermochemistry_record"],fuelRecord,aerosolMinimumK,
				aerosolMaximumK,error) || !ValidateChemRecord(embeddedRecords_["chem_record"],
				hasChemChannels_,chemNormalizationIntervalsNM_,error) ) return false;
			if( hasChemChannels_ && sourceQualification_!="preview_only" ) return Fail(error,
				"synthetic chemistry fixture cannot qualify a predictive sequence");
			if( !ReadFinite(payload->Find("scene_unit_meters"),sceneUnitMeters_) ||
				sceneUnitMeters_ <= 0.0 ||
				!ReadFloatArray(payload->Find("scene_translation_m"),sceneTranslation_) ) {
				return Fail(error,"fire sequence scene-unit/translation placement is invalid");
			}
			for( const char* key : {"case_record_id","fuel_record_id"} ) {
				const Value* id = payload->Find(key);
				if( !id || id->GetType() != Value::Text || !IsDigest(id->GetText()) ) return Fail(error,
					std::string("fire sequence record ID is invalid: ")+key);
			}

			const Value* timeMap = payload->Find("time_map");
			if( !timeMap || !ExactKeys(*timeMap,{"alpha","delta_t_frame","i0",
				"t0","t_scene_0"}) ||
				!ReadFinite(timeMap->Find("t0"),timeMap_.simulationTimeOrigin) ||
				!ReadFinite(timeMap->Find("alpha"),timeMap_.sceneToSimulationScale) ||
				!ReadFinite(timeMap->Find("t_scene_0"),timeMap_.sceneTimeOrigin) ||
				!ReadFinite(timeMap->Find("delta_t_frame"),timeMap_.frameStepSeconds) ||
				!ReadSigned(timeMap->Find("i0"),timeMap_.firstFrameIndex) ||
				timeMap_.sceneToSimulationScale == 0.0 || timeMap_.frameStepSeconds <= 0.0 ) {
				return Fail(error,"fire sequence time map is invalid");
			}
			const Value* endPolicy = payload->Find("end_policy");
			const Value* outsideHalo = payload->Find("outside_halo_policy");
			if( !endPolicy || endPolicy->GetType() != Value::Text ||
				(endPolicy->GetText() != "hold" && endPolicy->GetText() != "error") ||
				!outsideHalo || outsideHalo->GetType() != Value::Text ||
				outsideHalo->GetText() != "reject_outside_declared_halo" ||
				!ReadFinite(payload->Find("velocity_halo_width_m"),velocityHaloWidthMeters_) ||
				velocityHaloWidthMeters_ < 0.0 ) return Fail(error,
				"fire sequence end or velocity-halo policy is invalid");
			endPolicy_ = endPolicy->GetText();
			outsideHaloPolicy_ = outsideHalo->GetText();
			std::array<double,2> temperatureDomain{};
			if( !ReadFloatArray(payload->Find("temperature_domain_K"),temperatureDomain) ||
				temperatureDomain[0] <= 0.0 || temperatureDomain[1] <= temperatureDomain[0] ) {
				return Fail(error,"fire sequence temperature domain is invalid");
			}
			temperatureDomainMinimumK_ = temperatureDomain[0];
			temperatureDomainMaximumK_ = temperatureDomain[1];
			if( temperatureDomainMinimumK_ < aerosolMinimumK ||
				temperatureDomainMaximumK_ > aerosolMaximumK ) return Fail(error,
				"fire sequence common temperature domain exceeds aerosol thermochemistry support");

			const Value* channels = payload->Find("channels");
			if( !channels || channels->GetType() != Value::Array || channels->GetArray().empty() ) {
				return Fail(error,"fire sequence channels are empty");
			}
			std::set<std::string> channelNames;
			for( const Value& item : channels->GetArray() ) {
				if( !ExactKeys(item,{"background_value","core_face_bounds_m","dimensions",
					"name","origin_m","temporal_semantics","units","value_type","voxel_size_m"}) ) {
					return Fail(error,"fire sequence channel descriptor is outside schema_version 1");
				}
				FireSequenceChannelDescriptor descriptor;
				const Value* name = item.Find("name");
				const Value* type = item.Find("value_type");
				const Value* units = item.Find("units");
				const Value* semantics = item.Find("temporal_semantics");
				if( !name || name->GetType() != Value::Text || !type || type->GetType() != Value::Text ||
					!units || units->GetType() != Value::Text || !semantics ||
					semantics->GetType() != Value::Text || !channelNames.insert(name->GetText()).second ) {
					return Fail(error,"fire sequence channel name/type/units/semantics are invalid");
				}
				descriptor.name = name->GetText();
				descriptor.valueType = type->GetText();
				descriptor.units = units->GetText();
				descriptor.temporalSemantics = semantics->GetText();
				if( (descriptor.valueType != "float32" && descriptor.valueType != "vec3_float32") ||
					(descriptor.temporalSemantics != "frozen_material_advection" &&
					 descriptor.temporalSemantics != "derived_eulerian_source" &&
					 descriptor.temporalSemantics != "static") ||
					!ReadDimensions(item.Find("dimensions"),descriptor.dimensions) ||
					!ReadFloatArray(item.Find("origin_m"),descriptor.originMeters) ||
					!ReadFloatArray(item.Find("voxel_size_m"),descriptor.voxelSizeMeters) ||
					!ReadFloatArray(item.Find("core_face_bounds_m"),descriptor.coreFaceBoundsMeters) ) {
					return Fail(error,"fire sequence channel lattice is invalid");
				}
				const bool materialChannel = descriptor.name=="carbon" ||
					descriptor.name=="temperature" || descriptor.name=="condensed";
				const bool sourceChannel = descriptor.name=="reaction" ||
					descriptor.name.rfind("chem_",0u)==0u;
				if( (materialChannel && descriptor.temporalSemantics!="frozen_material_advection") ||
					(sourceChannel && descriptor.temporalSemantics!="derived_eulerian_source") ||
					(descriptor.name=="velocity" &&
						descriptor.temporalSemantics!="frozen_material_advection" &&
						descriptor.temporalSemantics!="static") ) {
					return Fail(error,"fire sequence channel temporal semantics differ from its role");
				}
				for( unsigned int axis=0; axis<3u; ++axis ) if( descriptor.voxelSizeMeters[axis] <= 0.0 ) {
					return Fail(error,"fire sequence channel voxel size is non-positive");
				}
				const Value* background = item.Find("background_value");
				if( !background || background->GetType() != Value::Array ||
					background->GetArray().size() != (descriptor.valueType == "vec3_float32" ? 3u : 1u) ) {
					return Fail(error,"fire sequence channel background has the wrong shape");
				}
				for( const Value& component : background->GetArray() ) {
					double value = 0.0;
					if( !ReadFinite(&component,value) ) return Fail(error,
						"fire sequence channel background is non-finite");
					descriptor.background.push_back(value);
				}
				if( descriptor.valueType == "float32" && !ValidateScalarValue(descriptor.name,
					descriptor.background[0],temperatureDomainMinimumK_,temperatureDomainMaximumK_,error) ) {
					return false;
				}
				if( descriptor.name == "temperature" ) {
					if( !SameBits(descriptor.background[0],ambientTemperatureK) ) return Fail(error,
						"fire sequence temperature background is not the fuel-record ambient temperature");
				} else if( descriptor.name == "velocity" ) {
					for( const double component : descriptor.background ) if( !SameBits(component,0.0) )
						return Fail(error,"fire sequence velocity background is not +0");
				} else {
					if( !SameBits(descriptor.background[0],0.0) ) return Fail(error,
						"fire sequence scalar background is not +0");
				}
				channels_.push_back(descriptor);
			}
			if( !channelNames.count("carbon") || !channelNames.count("temperature") ) return Fail(error,
				"fire sequence lacks the mandatory carbon/temperature channels");
			const std::set<std::string> allowedChannels = {"carbon","temperature","condensed",
				"reaction","chem_CH","chem_C2","chem_CO2","velocity"};
			for( const std::string& name : channelNames ) if( !allowedChannels.count(name) ) return Fail(error,
				"fire sequence declares an unknown channel");
			const bool anyChem = channelNames.count("chem_CH") || channelNames.count("chem_C2") ||
				channelNames.count("chem_CO2");
			if( anyChem && !(channelNames.count("chem_CH") && channelNames.count("chem_C2") &&
				channelNames.count("chem_CO2")) ) return Fail(error,
				"fire sequence chem channels are not the required all-or-none triplet");
			if( anyChem != hasChemChannels_ ) return Fail(error,
				"fire sequence chem channels do not match the embedded chem record variant");
			const auto channelByName = [this](const char* name) -> const FireSequenceChannelDescriptor* {
				for( const FireSequenceChannelDescriptor& channel : channels_ )
					if( channel.name == name ) return &channel;
				return nullptr;
			};
			const auto requireScalar = [&error,&channelByName](const char* name, const char* units) {
				const FireSequenceChannelDescriptor* channel = channelByName(name);
				if( !channel ) return true;
				if( channel->valueType != "float32" || channel->units != units ) {
					error = std::string("fire sequence channel has invalid type/units: ")+name;
					return false;
				}
				return true;
			};
			if( !requireScalar("carbon","g/m3") || !requireScalar("temperature","K") ||
				!requireScalar("condensed","g/m3") || !requireScalar("reaction","W/m3") ||
				!requireScalar("chem_CH","W/m3") || !requireScalar("chem_C2","W/m3") ||
				!requireScalar("chem_CO2","W/m3") ) return false;
			const FireSequenceChannelDescriptor* velocity = channelByName("velocity");
			if( velocity && (velocity->valueType != "vec3_float32" || velocity->units != "m/s") )
				return Fail(error,"fire sequence velocity channel has invalid type/units");
			const FireSequenceChannelDescriptor* extinction = channelByName("carbon");
			for( const char* name : {"temperature","condensed","chem_CH","chem_C2","chem_CO2"} ) {
				const FireSequenceChannelDescriptor* channel = channelByName(name);
				if( channel && (channel->dimensions != extinction->dimensions ||
					channel->originMeters != extinction->originMeters ||
					channel->voxelSizeMeters != extinction->voxelSizeMeters ||
					channel->coreFaceBoundsMeters != extinction->coreFaceBoundsMeters) ) return Fail(error,
					"fire sequence renderer scalar channels do not share one exact lattice");
			}
			for( const FireSequenceChannelDescriptor& descriptor : channels_ ) {
				if( descriptor.name == "velocity" ) continue;
				for( unsigned int axis=0; axis<3u; ++axis ) {
					const double expectedMinimum = descriptor.originMeters[axis]-0.5*descriptor.voxelSizeMeters[axis];
					const double expectedMaximum = descriptor.originMeters[axis]+
						(static_cast<double>(descriptor.dimensions[axis])-0.5)*descriptor.voxelSizeMeters[axis];
					if( descriptor.coreFaceBoundsMeters[axis] != expectedMinimum ||
						descriptor.coreFaceBoundsMeters[axis+3u] != expectedMaximum ) return Fail(error,
						"fire sequence core face bounds disagree with its center lattice");
				}
			}
			if( velocity ) {
				if( velocity->voxelSizeMeters != extinction->voxelSizeMeters ||
					velocity->coreFaceBoundsMeters != extinction->coreFaceBoundsMeters ) return Fail(error,
					"fire sequence velocity core lattice differs from extinction lattice");
				for( unsigned int axis=0; axis<3u; ++axis ) {
					const double haloCellsExact = velocityHaloWidthMeters_/velocity->voxelSizeMeters[axis];
					const double haloCellsRounded = std::round(haloCellsExact);
					if( !std::isfinite(haloCellsExact) || !std::isfinite(haloCellsRounded) ||
						haloCellsExact != haloCellsRounded || haloCellsRounded < 0.0 ||
						haloCellsRounded > 0x7fffffffu ) return Fail(error,
						"fire sequence velocity halo cell count is outside the exact lattice domain");
					const std::uint64_t haloCells=static_cast<std::uint64_t>(haloCellsRounded);
					if( haloCells > (0x7fffffffu-extinction->dimensions[axis])/2u ||
						velocity->dimensions[axis] != extinction->dimensions[axis]+
							2u*haloCells ||
						velocity->originMeters[axis] != extinction->originMeters[axis]-velocityHaloWidthMeters_ )
						return Fail(error,"fire sequence velocity halo does not match its declared width");
				}
			}

			const Value* frames = payload->Find("frames");
			if( !frames || frames->GetType() != Value::Array || frames->GetArray().empty() ) {
				return Fail(error,"fire sequence frame list is empty");
			}
			const std::size_t maximumOffset=frames->GetArray().size()-1u;
			if( maximumOffset>static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
				timeMap_.firstFrameIndex>std::numeric_limits<std::int64_t>::max()-
					static_cast<std::int64_t>(maximumOffset) ) return Fail(error,
					"fire sequence frame-index range exceeds signed 64-bit arithmetic");
			std::set<std::string> paths;
			for( std::size_t position=0; position<frames->GetArray().size(); ++position ) {
				const Value& item = frames->GetArray()[position];
				if( !ExactKeys(item,{"index","path","sha256"}) ) return Fail(error,
					"fire sequence frame descriptor is outside schema_version 1");
				FireSequenceFrameDescriptor descriptor;
				const Value* path = item.Find("path");
				const Value* digest = item.Find("sha256");
				if( !ReadSigned(item.Find("index"),descriptor.index) || !path ||
					path->GetType() != Value::Text || !digest || digest->GetType() != Value::Text ||
					!SafeRelativePath(path->GetText()) || !IsDigest(digest->GetText()) ||
					!paths.insert(path->GetText()).second || descriptor.index !=
						timeMap_.firstFrameIndex+static_cast<std::int64_t>(position) ) return Fail(error,
					"fire sequence frame indices/paths/digests are not contiguous and unique");
				descriptor.relativePath = path->GetText();
				descriptor.sha256 = digest->GetText();
				frames_.push_back(descriptor);
			}
			std::int64_t declaredFirst = 0, declaredLast = 0;
			const Value* frameCount = payload->Find("frame_count");
			if( !frameCount || frameCount->GetType() != Value::UnsignedInteger ||
				frameCount->GetIntegerArgument() != frames_.size() ||
				!ReadSigned(payload->Find("first_frame_index"),declaredFirst) ||
				!ReadSigned(payload->Find("last_frame_index"),declaredLast) ||
				declaredFirst != frames_.front().index || declaredLast != frames_.back().index ) {
				return Fail(error,"fire sequence declared frame count/range is inconsistent");
			}
			sequenceId_ = sequenceId->GetText();
			manifestDirectory_ = manifestDirectory;
			valid_ = true;
			return true;
		}

		bool FireSequenceManifest::MapSceneTime( const double sceneTime,
			FireSequenceMappedTime& mapped, std::string& error ) const
		{
			mapped = FireSequenceMappedTime();
			if( !valid_ || !std::isfinite(sceneTime) ) return Fail(error,
				"fire sequence cannot map an invalid scene time");
			const double simulationTime = timeMap_.simulationTimeOrigin+
				timeMap_.sceneToSimulationScale*(sceneTime-timeMap_.sceneTimeOrigin);
			const double end = timeMap_.simulationTimeOrigin+
				static_cast<double>(frames_.size())*timeMap_.frameStepSeconds;
			if( !std::isfinite(simulationTime) || !std::isfinite(end) ) return Fail(error,
				"fire sequence mapped time overflowed");
			double evaluation = simulationTime;
			if( endPolicy_ == "error" ) {
				if( evaluation < timeMap_.simulationTimeOrigin || evaluation >= end ) return Fail(error,
					"fire sequence mapped time is outside authored support");
			} else {
				if( evaluation < timeMap_.simulationTimeOrigin ) {
					evaluation = timeMap_.simulationTimeOrigin;
					mapped.held = true;
				} else if( evaluation > end ) {
					evaluation = end;
					mapped.held = true;
				}
			}
			const double framePosition = (evaluation-timeMap_.simulationTimeOrigin)/
				timeMap_.frameStepSeconds;
			const std::size_t offset = std::min(frames_.size()-1u,
				static_cast<std::size_t>(std::max(0.0,std::floor(framePosition))));
			mapped.baseFrameIndex = frames_[offset].index;
			mapped.simulationTime = evaluation;
			mapped.baseFrameTime = timeMap_.simulationTimeOrigin+
				static_cast<double>(offset)*timeMap_.frameStepSeconds;
			mapped.advectionOffsetSeconds = evaluation-mapped.baseFrameTime;
			return true;
		}

		bool FireSequenceManifest::LoadFrame( const std::int64_t frameIndex,
			FireSequencePreparedFrame& frame, std::string& error ) const
		{
			frame = FireSequencePreparedFrame();
			if( !valid_ ) return Fail(error,"fire sequence manifest is invalid");
			const auto found = std::find_if(frames_.begin(),frames_.end(),
				[frameIndex](const FireSequenceFrameDescriptor& item){ return item.index == frameIndex; });
			if( found == frames_.end() ) return Fail(error,"fire sequence requested frame is absent");
#if !defined(RISE_ENABLE_OPENVDB)
			(void)frame;
			return Fail(error,"fire_sequence_openvdb_capability_unavailable");
#else
			const std::filesystem::path path = std::filesystem::path(manifestDirectory_)/found->relativePath;
			std::ifstream verifiedInput(path,std::ios::binary);
			std::string fileDigest;
			if( !verifiedInput || !RISECBOR64::SHA256StreamHex(verifiedInput,fileDigest) ||
				fileDigest != found->sha256 ) {
				return Fail(error,"fire sequence frame whole-file digest mismatch");
			}
			openvdb::initialize();
			try {
				verifiedInput.clear();
				verifiedInput.seekg(0,std::ios::beg);
				if( !verifiedInput ) return Fail(error,"fire sequence verified frame stream is not seekable");
				openvdb::io::Stream stream(verifiedInput,false);
				openvdb::GridPtrVecPtr grids = stream.getGrids();
				std::set<std::string> fileNames;
				std::map<std::string,openvdb::GridBase::Ptr> byName;
				for( const openvdb::GridBase::Ptr& grid : *grids ) {
					if( !grid || !fileNames.insert(grid->getName()).second ) {
						return Fail(error,"fire sequence frame contains duplicate grid names");
					}
					byName[grid->getName()] = grid;
				}
				std::set<std::string> declaredNames;
				for( const FireSequenceChannelDescriptor& descriptor : channels_ )
					declaredNames.insert(descriptor.name);
				if( fileNames != declaredNames ) {
					return Fail(error,"fire sequence frame channel set differs from its manifest");
				}
				FireSequencePreparedFrame candidate;
				candidate.frameIndex = frameIndex;
				candidate.wholeFileSha256 = found->sha256;
				for( const FireSequenceChannelDescriptor& descriptor : channels_ ) {
					openvdb::GridBase::Ptr base = byName[descriptor.name];
					if( !base || !ValidateTransform(*base,descriptor,error) ) {
						return base ? false : Fail(error,"fire sequence frame lacks a declared grid");
					}
					FireSequenceDenseChannel channel;
					bool loaded = false;
					if( descriptor.valueType == "float32" ) {
						openvdb::FloatGrid::Ptr grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
						loaded = grid && LoadScalarGrid<openvdb::FloatGrid>(grid,descriptor,
							temperatureDomainMinimumK_,temperatureDomainMaximumK_,channel,error);
					} else if( descriptor.valueType == "vec3_float32" ) {
						openvdb::Vec3fGrid::Ptr grid = openvdb::gridPtrCast<openvdb::Vec3fGrid>(base);
						loaded = grid && LoadVectorGrid(grid,descriptor,channel,error);
					}
					if( !loaded ) {
						return error.empty() ? Fail(error,
							"fire sequence grid type differs from its manifest descriptor") : false;
					}
					candidate.channels.emplace(descriptor.name,std::move(channel));
				}
				// Decode and provenance must name the same immutable byte preimage.  Keeping
				// one open descriptor prevents path replacement between the two operations;
				// the second digest also rejects an in-place writer racing the decode.
				verifiedInput.clear();
				verifiedInput.seekg(0,std::ios::beg);
				std::string postDecodeDigest;
				if( !verifiedInput || !RISECBOR64::SHA256StreamHex(
					verifiedInput,postDecodeDigest) || postDecodeDigest != found->sha256 ) {
					return Fail(error,"fire sequence frame changed during verified decode");
				}
				frame = std::move(candidate);
				return true;
			} catch( const std::exception& exception ) {
				return Fail(error,std::string("fire sequence OpenVDB load failed: ")+exception.what());
			}
#endif
		}

		bool FireSequenceManifest::PreflightAllFrames( std::string& error ) const
		{
			if( !valid_ ) return Fail(error,"fire sequence manifest is invalid");
			for( const FireSequenceFrameDescriptor& descriptor : frames_ ) {
				FireSequencePreparedFrame frame;
				if( !LoadFrame(descriptor.index,frame,error) ) return false;
			}
			return true;
		}

		FireSequencePreparationController::RenderLease::RenderLease(
			FireSequencePreparationController& owner ) : owner_(&owner)
		{
			std::lock_guard<std::mutex> lock(owner.mutex_);
			generation_ = owner.generation_;
			preparedInputId_ = owner.preparedInputId_;
			++owner.activeRenderLeases_;
		}

		FireSequencePreparationController::RenderLease::RenderLease(
			RenderLease&& other ) noexcept : owner_(other.owner_), generation_(other.generation_),
			preparedInputId_(std::move(other.preparedInputId_))
		{
			other.owner_ = nullptr;
		}

		FireSequencePreparationController::RenderLease&
		FireSequencePreparationController::RenderLease::operator=(RenderLease&& other) noexcept
		{
			if( this == &other ) return *this;
			if( owner_ ) owner_->ReleaseLease();
			owner_ = other.owner_;
			generation_ = other.generation_;
			preparedInputId_ = std::move(other.preparedInputId_);
			other.owner_ = nullptr;
			return *this;
		}

		FireSequencePreparationController::RenderLease::~RenderLease()
		{
			if( owner_ ) owner_->ReleaseLease();
		}

		bool FireSequencePreparationController::RenderLease::StateStayedFrozen() const
		{
			if( !owner_ ) return false;
			std::lock_guard<std::mutex> lock(owner_->mutex_);
			return owner_->generation_ == generation_ &&
				owner_->preparedInputId_ == preparedInputId_;
		}

		void FireSequencePreparationController::ReleaseLease()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if( activeRenderLeases_ ) --activeRenderLeases_;
		}

		bool FireSequencePreparationController::SetFrameInstaller(
			const std::function<bool(const FireSequencePreparedFrame&)>& installer,
			const std::string& installerIdentity,
			std::string& error )
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if( activeRenderLeases_ ) return Fail(error,"mutation_frozen");
			if( frame_ || generation_ != 0u || !installer || !IsDigest(installerIdentity) ) return Fail(error,
				"fire sequence frame installer must be bound before preparation");
			frameInstaller_ = installer;
			frameInstallerIdentity_ = installerIdentity;
			return true;
		}

		bool FireSequencePreparationController::SetActiveBindingIdentity(
			const std::string& bindingIdentity, std::string& error )
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if( activeRenderLeases_ ) return Fail(error,"mutation_frozen");
			if( !IsDigest(bindingIdentity) ) return Fail(error,
				"fire sequence active binding identity is invalid");
			activeBindingIdentity_ = bindingIdentity;
			return true;
		}

		bool FireSequencePreparationController::PrepareMediaForRender(
			const FireSequenceRenderTimeSupport& support, const bool effectiveVelocityBlur,
			std::string& error )
		{
			if( !manifest_ || !manifest_->IsValid() || !std::isfinite(support.nominalSceneTime) ||
				!std::isfinite(support.shutterOpenSceneTime) ||
				!std::isfinite(support.shutterCloseSceneTime) ||
				support.shutterOpenSceneTime > support.shutterCloseSceneTime ) return Fail(error,
				"fire sequence render time support is invalid");
			FireSequenceMappedTime nominal, open, close;
			if( !manifest_->MapSceneTime(support.nominalSceneTime,nominal,error) ) return false;
			if( effectiveVelocityBlur &&
				(!manifest_->MapSceneTime(support.shutterOpenSceneTime,open,error) ||
				 !manifest_->MapSceneTime(support.shutterCloseSceneTime,close,error)) ) return false;
			using RISECBOR64::Value;
			if( frameInstallerIdentity_.empty() || activeBindingIdentity_.empty() ) return Fail(error,
				"fire sequence prepared identity inputs are incomplete");
			const auto descriptor = std::find_if(manifest_->Frames().begin(),manifest_->Frames().end(),
				[&nominal](const FireSequenceFrameDescriptor& item) {
					return item.index == nominal.baseFrameIndex;
				});
			if( descriptor == manifest_->Frames().end() ) return Fail(error,
				"fire sequence selected frame is absent from prepared identity");
			RISECBOR64::Bytes identityBytes;
			const Value identity = Value::MapValue({
				{"active_binding_identity",Value::String(activeBindingIdentity_)},
				{"effective_velocity_blur",Value::Bool(effectiveVelocityBlur)},
				{"frame_index",Value::Signed(nominal.baseFrameIndex)},
				{"frame_installer_identity",Value::String(frameInstallerIdentity_)},
				{"frame_whole_file_digest",Value::String(descriptor->sha256)},
				{"nominal",Value::Float(support.nominalSceneTime)},
				{"prepared_input_schema",Value::Unsigned(1)},
				{"sequence_id",Value::String(manifest_->SequenceId())},
				{"shutter_close",Value::Float(support.shutterCloseSceneTime)},
				{"shutter_open",Value::Float(support.shutterOpenSceneTime)}
			});
			if( !RISECBOR64::Encode(identity,identityBytes,&error) ) return false;
			const std::string identityId = RISECBOR64::SHA256Hex(identityBytes);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if( activeRenderLeases_ ) return Fail(error,"mutation_frozen");
				if( frame_ && preparedInputId_ == identityId ) return true;
			}
			FireSequencePreparedFrame loaded;
			if( !manifest_->LoadFrame(nominal.baseFrameIndex,loaded,error) ) return false;
			std::shared_ptr<const FireSequencePreparedFrame> immutable(
				new FireSequencePreparedFrame(std::move(loaded)));
			std::lock_guard<std::mutex> lock(mutex_);
			if( activeRenderLeases_ ) return Fail(error,"mutation_frozen");
			if( frameInstaller_ && !frameInstaller_(*immutable) ) return Fail(error,
				"fire sequence frame/majorant/emission-CDF transaction failed");
			frame_ = immutable;
			preparedInputId_ = identityId;
			++generation_;
			++majorantGeneration_;
			++emissionCDFGeneration_;
			return true;
		}

		FireSequencePreparationController::RenderLease
		FireSequencePreparationController::AcquireRenderLease( std::string& error )
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if( !frame_ || preparedInputId_.empty() ) {
				error = "fire sequence has no prepared immutable frame";
				return RenderLease();
			}
			RenderLease lease;
			lease.owner_ = this;
			lease.generation_ = generation_;
			lease.preparedInputId_ = preparedInputId_;
			++activeRenderLeases_;
			return lease;
		}

		std::shared_ptr<const FireSequencePreparedFrame>
		FireSequencePreparationController::PreparedFrame() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return frame_;
		}

		std::string FireSequencePreparationController::PreparedInputId() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return preparedInputId_;
		}

		std::uint64_t FireSequencePreparationController::Generation() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return generation_;
		}

		std::uint64_t FireSequencePreparationController::MajorantGeneration() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return majorantGeneration_;
		}

		std::uint64_t FireSequencePreparationController::EmissionCDFGeneration() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return emissionCDFGeneration_;
		}

		bool FireSequencePreparationController::HasActiveRenderLease() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return activeRenderLeases_ != 0u;
		}
	}
}
