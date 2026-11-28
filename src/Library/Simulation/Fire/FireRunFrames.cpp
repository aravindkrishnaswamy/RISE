//////////////////////////////////////////////////////////////////////
//
//  FireRunFrames.cpp - Fire run case records, legacy frames and manifest
//
//  Moved verbatim from tests/FireSequenceTest.cpp (simulation M2, track B).
//  The invalid-frame mutations stay in the test, behind
//  FireLegacyFrameTestMutation.  Compiled without fast-math or FP
//  contraction (see the per-file rules in build/make/rise/Makefile).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireRunFrames.h"
#include "../../Utilities/FireSequence.h"
#include "../../Utilities/FireOptics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <system_error>
#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/io/File.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace FireRun
	{
	namespace
	{
		// Invariant checks carried over verbatim from the test translation unit.
		void Check(const bool condition,const char* message)
		{
			ReportFireRunCheck(condition,message);
		}
	}

	const RISECBOR64::Bytes& ProductionBurnerPolicyBytes()
	{
		static const std::string preimage="rise.fire.production.burner.v5:"
			"wall-bed;prescribed-shared-fct-inlet;chemistry-at-Q0;no-volumetric-fuel;"
			"accepted-represented-flux-ledger;canonical-source-temperature;segment-anchored-physical-enthalpy;"
			"parcel-cft;seeded-one-cell-warm-edge-v1;balanced-output-event-pair-v1";
		static const RISECBOR64::Bytes bytes(preimage.begin(),preimage.end());
		return bytes;
	}

	const std::string& ProductionBurnerPolicyId()
	{
		static const std::string digest=RISECBOR64::SHA256Hex(ProductionBurnerPolicyBytes());
		return digest;
	}

	// r270: the radiatively adiabatic floor is opt-in and identity-bearing, so
	// every derivation of the production case (the run and its manifest) reads
	// the same option.
	bool ApplyFloorEmissivityOption(FireCase::AuthoredV1& authored)
	{
		const char* floorOption=std::getenv("RISE_FIRE_FLOOR_EMISSIVITY");
		if(!floorOption)return true;
		char* end=nullptr;const double emissivity=std::strtod(floorOption,&end);
		if(end==floorOption||*end!='\0'||!std::isfinite(emissivity)||!(emissivity>0.0)||
			emissivity>1.0)return false;
		authored.hasFloorEmissivity=true;authored.floorEmissivity=emissivity;
		return true;
	}

	RISECBOR64::Bytes AerosolRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"carbon_phase",RISECBOR64::Value::MapValue({
				{"common_T_ref_K",RISECBOR64::Value::Float(fuel.ReferenceTemperatureK())},
				{"interpolation",RISECBOR64::Value::String("nasa9_piecewise_cp_hs")},
				{"phase",RISECBOR64::Value::String("solid")},
				{"source_fuel_record_id",RISECBOR64::Value::String(fuel.RecordId())},
				{"species_id",RISECBOR64::Value::String("C(gr)")}
			})},
			{"condensable_stream",RISECBOR64::Value::MapValue({
				{"kind",RISECBOR64::Value::String("none")},
				{"reason",RISECBOR64::Value::String("methane_has_no_condensable_organic_stream")}
			})},
			{"record_kind",RISECBOR64::Value::String("fire-aerosol-thermochemistry-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)},
			{"status",RISECBOR64::Value::String("preview_methane_zero_yield")},
			{"temperature_domain_K",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(fuel.TemperatureMinK()),
				RISECBOR64::Value::Float(fuel.TemperatureMaxK())})}
		}),bytes,&error),"aerosol record encodes");
		return bytes;
	}

	RISECBOR64::Bytes ChemNoneRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"chem_model",RISECBOR64::Value::String("none")},
			{"provenance",RISECBOR64::Value::String("methane r52 no adopted chem record")},
			{"record_kind",RISECBOR64::Value::String("fire-chem-none-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"chem-none record encodes");
		return bytes;
	}

	RISECBOR64::Bytes SyntheticChemRecord()
	{
		using RISECBOR64::Value;
		RISECBOR64::Bytes bytes; std::string error;
		Value::Values bands;
		const char* names[3]={"CH","C2","CO2"};
		const double limits[3][2]={{390.0,440.0},{450.0,570.0},{380.0,780.0}};
		for( unsigned int band=0; band<3u; ++band ) bands.push_back(Value::MapValue({
			{"band",Value::String(names[band])},
			{"normalization_interval_nm",Value::ArrayValue({
				Value::Float(limits[band][0]),Value::Float(limits[band][1])})},
			{"normalization_rule",Value::String("trapezoid_1nm_then_divide_once")},
			{"spd_shape",Value::String("uniform_unit_shape")}
		}));
		Check(RISECBOR64::Encode(Value::MapValue({
			{"absolute_calibration",Value::String("input_absolute_band_power_W_per_m3")},
			{"bands",Value::ArrayValue(bands)},
			{"provenance",Value::String("test-only analytic uniform-SPD estimator fixture")},
			{"record_class",Value::String("SYNTHETIC_NON_PREDICTIVE")},
			{"record_kind",Value::String("fire-chem-synthetic-fixture-v1")},
			{"schema_version",Value::Unsigned(1)},
			{"state_domain",Value::String("finite_nonnegative_absolute_channel_values")},
			{"wavelength_unit",Value::String("nm")}
		}),bytes,&error),"synthetic chem fixture record encodes");
		return bytes;
	}

	RISECBOR64::Value Channel( const char* name, const char* type,
		const char* units, const char* semantics, const std::vector<double>& background,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5 )
	{
		using RISECBOR64::Value;
		Value::Values bg;
		for( const double value : background ) bg.push_back(Value::Float(value));
		const bool velocity = std::strcmp(name,"velocity") == 0;
		const double origin=velocity?-voxelSize:0.0;
		const double lower=-0.5*voxelSize;
		return Value::MapValue({
			{"background_value",Value::ArrayValue(bg)},
			{"core_face_bounds_m",Value::ArrayValue({Value::Float(lower),Value::Float(lower),
				Value::Float(lower),Value::Float((scalarDimensions[0]-0.5)*voxelSize),
				Value::Float((scalarDimensions[1]-0.5)*voxelSize),
				Value::Float((scalarDimensions[2]-0.5)*voxelSize)})},
			{"dimensions",Value::ArrayValue({Value::Unsigned(scalarDimensions[0]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[1]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[2]+(velocity?2u:0u))})},
			{"name",Value::String(name)},
			{"origin_m",Value::ArrayValue({Value::Float(origin),Value::Float(origin),Value::Float(origin)})},
			{"temporal_semantics",Value::String(semantics)},
			{"units",Value::String(units)},
			{"value_type",Value::String(type)},
			{"voxel_size_m",Value::ArrayValue({Value::Float(voxelSize),Value::Float(voxelSize),
				Value::Float(voxelSize)})}
		});
	}

	RISECBOR64::Bytes ManifestBytes( const std::string& firstDigest,
		const std::string& secondDigest, const char* endPolicy,
		const bool useProductionOptics, const bool syntheticChem,
		const std::array<std::uint64_t,3> scalarDimensions,
		const double voxelSize,
		const double simulationTimeOrigin, const double frameStepSeconds,
		const double sceneToSimulationScale, const double sceneTimeOrigin,
		const bool caseGridBound, const double casePoolDiameterM,
		const double caseHeatReleaseRateKW,
		const bool caseHasRadiativeFractionOverride,
		const double caseRadiativeFractionOverride,
		const double caseResolutionTier,const bool casePlumeLaw,
		const std::vector<std::pair<std::string,std::string> >& sequenceFrames,
		const double authoredCaseDurationS,const double authoredCaseFramesPerS,
		const bool prescribedProductionBurner,const bool r244LateralDouble,
		const bool r246TopDouble)
	{
		using RISECBOR64::Value;
		std::string error;
		RISECBOR64::Bytes build; std::string buildId;
		Check(CurrentRendererBuildIdentity(build,buildId),
			"sequence producer embeds the current executable build identity");
		const RISECBOR64::Bytes optics = FireOpticsPreset::PredictiveV1().RecordBytes();
		const RISECBOR64::Bytes thermo =
			FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes();
		const RISECBOR64::Bytes transport =
			FireSimulationTransportRecord::OpenV1().RecordBytes();
		const RISECBOR64::Bytes aerosol = AerosolRecord();
		const RISECBOR64::Bytes opacity =
			FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes();
		const RISECBOR64::Bytes chem = syntheticChem ? SyntheticChemRecord() : ChemNoneRecord();
		const RISECBOR64::Bytes fuel = FireSimulationMethaneRecord::PhysicalV1().RecordBytes();
		const RISECBOR64::Bytes gateEvidenceBytes={'p','h','a','s','e','_','c','_','r','5','3'};
		const std::string gateEvidenceId=RISECBOR64::SHA256Hex(gateEvidenceBytes);
		FireCase::AuthoredV1 caseAuthored;
		caseAuthored.fuelRecordId=FireSimulationMethaneRecord::PhysicalV1().RecordId();
		caseAuthored.poolDiameterM=casePoolDiameterM;
		caseAuthored.heatReleaseRateKW=caseHeatReleaseRateKW;
		caseAuthored.envelope={{0.0,1.0}};
		caseAuthored.durationS=authoredCaseDurationS>0.0?authoredCaseDurationS:std::max(1.0,frameStepSeconds);
		caseAuthored.quality=caseGridBound?"dstar":"standard";
		caseAuthored.numericDStarTier=caseGridBound?caseResolutionTier:0.0; caseAuthored.seed=1234;
		caseAuthored.outputFramesPerS=authoredCaseFramesPerS>0.0?authoredCaseFramesPerS:1.0/frameStepSeconds;
		caseAuthored.plumeLaw=casePlumeLaw;
		if(r246TopDouble&&!r244LateralDouble)return {};
		if(r244LateralDouble)caseAuthored.domainStudy=r246TopDouble?
			"r246-lateral-double-top-double-v1":"r244-lateral-double-v1";
		caseAuthored.hasRadiativeFractionOverride=caseHasRadiativeFractionOverride;
		caseAuthored.radiativeFractionOverride=caseRadiativeFractionOverride;
		// The production manifest must carry the run's case, floor included;
		// non-production callers never derive the floor case.
		if(prescribedProductionBurner&&!ApplyFloorEmissivityOption(caseAuthored))return {};
		FireCase::RecordV1 caseRecord;
		std::vector<std::string> caseReferences={RISECBOR64::SHA256Hex(thermo),RISECBOR64::SHA256Hex(transport),
			 RISECBOR64::SHA256Hex(opacity),RISECBOR64::SHA256Hex(optics),
			 RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem),
			 RISECBOR64::SHA256Hex(fuel)};
		if(prescribedProductionBurner)caseReferences.push_back(ProductionBurnerPolicyId());
		const bool caseBuilt=FireCase::BuildMethaneV1(caseAuthored,
			FireSimulationMethaneRecord::PhysicalV1(),caseReferences,caseRecord,error);
		Check(caseBuilt,"test sequence case record derives canonically");
		Value::Values channels={
			Channel("carbon","float32","g/m3","frozen_material_advection",{0.0},scalarDimensions,voxelSize),
			Channel("temperature","float32","K","frozen_material_advection",{300.0},scalarDimensions,voxelSize),
			Channel("reaction","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize),
			Channel("velocity","vec3_float32","m/s","frozen_material_advection",{0.0,0.0,0.0},scalarDimensions,voxelSize)
		};
		if( syntheticChem ) {
			channels.push_back(Channel("chem_CH","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_C2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_CO2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
		}
		const bool preview=useProductionOptics || syntheticChem;
		Value::Values encodedFrames;
		std::uint64_t firstFrameIndex=4u,lastFrameIndex=5u;
		if(sequenceFrames.empty()){
			encodedFrames={
				Value::MapValue({{"index",Value::Unsigned(4)},
					{"path",Value::String("frame4.vdb")},{"sha256",Value::String(firstDigest)}}),
				Value::MapValue({{"index",Value::Unsigned(5)},
					{"path",Value::String("frame5.vdb")},{"sha256",Value::String(secondDigest)}})};
		}else{
			firstFrameIndex=0u;lastFrameIndex=sequenceFrames.size()-1u;
			encodedFrames.reserve(sequenceFrames.size());
			for(std::size_t frame=0u;frame<sequenceFrames.size();++frame)
				encodedFrames.push_back(Value::MapValue({
					{"index",Value::Unsigned(frame)},
					{"path",Value::String(sequenceFrames[frame].first)},
					{"sha256",Value::String(sequenceFrames[frame].second)}}));
		}
		Value payload = Value::MapValue({
			{"aerosol_thermochemistry_record",Value::BytesValue(aerosol)},
			{"aerosol_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(aerosol))},
			{"case_record",Value::BytesValue(caseRecord.envelopeBytes)},
			{"case_record_id",Value::String(caseRecord.caseRecordId)},
			{"channels",Value::ArrayValue(channels)},
			{"chem_record",Value::BytesValue(chem)},
			{"chem_record_id",Value::String(RISECBOR64::SHA256Hex(chem))},
			{"end_policy",Value::String(endPolicy)},
			{"first_frame_index",Value::Unsigned(firstFrameIndex)},
			{"frame_count",Value::Unsigned(encodedFrames.size())},
			{"frame_encoding",Value::String("openvdb-v1")},
			{"frames",Value::ArrayValue(encodedFrames)},
			{"fuel_record",Value::BytesValue(fuel)},
			{"fuel_record_id",Value::String(RISECBOR64::SHA256Hex(fuel))},
			{"gas_opacity_record",Value::BytesValue(opacity)},
			{"gas_opacity_record_id",Value::String(RISECBOR64::SHA256Hex(opacity))},
			{"gas_thermochemistry_record",Value::BytesValue(thermo)},
			{"gas_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(thermo))},
			{"gate_evidence_ids",Value::ArrayValue({Value::String(gateEvidenceId)})},
			{"last_frame_index",Value::Unsigned(lastFrameIndex)},
			{"optical_record",Value::BytesValue(optics)},
			{"optical_record_id",Value::String(RISECBOR64::SHA256Hex(optics))},
			{"outside_halo_policy",Value::String("reject_outside_declared_halo")},
			{"physical_mapping",Value::String("absolute_si")},
			{"producer_build_id",Value::String(buildId)},
			{"producer_build_v1",Value::BytesValue(build)},
			{"producer_reason_codes",Value::ArrayValue(preview ? (caseGridBound ? Value::Values{
				Value::String("case_grid_bound"),Value::String(syntheticChem ? "synthetic_chem_fixture" :
					"open_subset_records_preview")} : Value::Values{Value::String(syntheticChem ?
					"synthetic_chem_fixture":"open_subset_records_preview")}) : Value::Values{})},
			{"scene_translation_m",Value::ArrayValue({Value::Float(0),Value::Float(0),Value::Float(0)})},
			{"qdot_ref_W",Value::Float(caseRecord.derived.referenceHeatReleaseRateW)},
			{"scene_unit_meters",Value::Float(1.0)},
			{"schema_version",Value::Unsigned(1)},
			{"source_kind",Value::String("rise_simulation")},
			{"source_qualification",Value::String(preview ? "preview_only" : "predictive_qualified")},
			{"temperature_domain_K",Value::ArrayValue({Value::Float(300),Value::Float(2500)})},
			{"time_map",Value::MapValue({
				{"alpha",Value::Float(sceneToSimulationScale)},
				{"delta_t_frame",Value::Float(frameStepSeconds)},
				{"i0",Value::Unsigned(firstFrameIndex)},
				{"t0",Value::Float(simulationTimeOrigin)},
				{"t_scene_0",Value::Float(sceneTimeOrigin)}
			})},
			{"transport_closure_record",Value::BytesValue(transport)},
			{"transport_closure_record_id",Value::String(RISECBOR64::SHA256Hex(transport))},
			{"velocity_halo_width_m",Value::Float(voxelSize)}
		});
		if(prescribedProductionBurner){
			auto members=payload.GetMap();
			members.push_back({"producer_numerical_policy",Value::BytesValue(ProductionBurnerPolicyBytes())});
			members.push_back({"producer_numerical_policy_id",Value::String(ProductionBurnerPolicyId())});
			payload=Value::MapValue(members);
		}
		RISECBOR64::Bytes payloadBytes, envelope;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"sequence payload encodes");
		Check(RISECBOR64::Encode(Value::MapValue({
			{"payload",payload},
			{"sequence_id",Value::String(RISECBOR64::SHA256Hex(payloadBytes))}
		}),envelope,&error),"sequence envelope encodes");
		return envelope;
	}

	bool PartialTemporalManifestEnabled()
	{
		const char* value=std::getenv("RISE_FIRE_PARTIAL_MANIFEST");
		return !value||std::strcmp(value,"0")!=0;
	}
	std::function<bool(const SolverFrameValues&,std::uint64_t)> MakePartialTemporalManifestObserver(
		const std::filesystem::path& runDirectory,const std::filesystem::path& frameDirectory,
		const double resolutionTier,const double frameCadenceS,const double targetTimeS,
		const bool r244LateralDouble,const bool r246TopDouble)
	{
		if(!PartialTemporalManifestEnabled())return std::function<bool(const SolverFrameValues&,std::uint64_t)>();
		return [=](const SolverFrameValues& values,const std::uint64_t frameIndex)->bool{
			if(frameIndex<1u)return true;
			std::vector<double> frameTimesS;
			std::vector<std::pair<std::string,std::string> > manifestFrames;
			for(std::uint64_t frame=0u;frame<=frameIndex;++frame){
				std::ostringstream stem;stem<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame;
				std::ifstream timeRecord(frameDirectory/(stem.str()+".time"));
				double timeS=0.0;std::string digest;
				if(!(timeRecord>>timeS>>digest)||digest.size()!=64u||!std::isfinite(timeS))return false;
				frameTimesS.push_back(timeS);
				manifestFrames.push_back({(std::filesystem::path(frameDirectory.filename())/
					(stem.str()+".vdb")).generic_string(),digest});
			}
			const std::array<std::uint64_t,3> dimensions={{values.dimensions[0],values.dimensions[1],
				values.dimensions[2]}};
			const RISECBOR64::Bytes manifest=ManifestBytes("","","hold",true,true,dimensions,
				values.cellWidthM,frameTimesS.front(),frameCadenceS,1.0,0.0,true,CapstonePoolDiameterM,
				CapstoneHeatReleaseRateKW,false,0.0,resolutionTier,true,manifestFrames,targetTimeS,
				1.0/frameCadenceS,true,r244LateralDouble,r246TopDouble);
			if(manifest.empty())return false;
			FireSequenceManifest verified;std::string error;
			if(!verified.LoadCanonicalEnvelope(manifest,runDirectory.string(),error)||
				verified.CaseRecordId()!=values.caseRecordId){
				std::fprintf(stderr,"partial manifest refused: %s\n",error.c_str());return false;
			}
			const std::filesystem::path temporary=runDirectory/(std::string(PartialTemporalManifestName)+".tmp");
			const std::filesystem::path published=runDirectory/PartialTemporalManifestName;
			{
				std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
				output.write(reinterpret_cast<const char*>(manifest.data()),
					static_cast<std::streamsize>(manifest.size()));
				output.close();
				if(!output)return false;
			}
			std::error_code renameError;
			std::filesystem::rename(temporary,published,renameError);
			return !renameError;
		};
	}

	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error)
	{
		RISECBOR64::Value decoded;
		if(!RISECBOR64::DecodeCanonical(buildRecord,decoded,&error)||
			decoded.GetType()!=RISECBOR64::Value::Map)return false;
		const RISECBOR64::Value* renderer=decoded.Find("renderer_binary");
		const RISECBOR64::Value* sha=renderer&&renderer->GetType()==RISECBOR64::Value::Map?
			renderer->Find("sha256"):nullptr;
		if(!sha||sha->GetType()!=RISECBOR64::Value::Text||sha->GetText().size()!=64u){
			error="renderer build record has no executable SHA-256";return false;
		}
		digest=sha->GetText();return true;
	}

#if defined(RISE_ENABLE_OPENVDB)
	bool WriteLegacyFireFrame(const std::filesystem::path& path,const float carbonValue,
		const bool includeChem,const SolverFrameValues solver,const float chemScale,
		const FireLegacyFrameTestMutation* mutation)
	{
		const bool syntheticColdChemistry=mutation&&mutation->SyntheticColdChemistry();
		// Synthetic light is a unit-render input, never a solver output. Reject
		// the opt-in on the durable capstone publication path, including mistakes
		// in a future caller. The solver fields below remain byte-for-byte intact.
		if(syntheticColdChemistry&&
			(!includeChem||std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")))return false;
		openvdb::initialize();
		const bool solverGrid=solver.dimensions[0]&&solver.dimensions[1]&&solver.dimensions[2];
		const double voxelSize=solverGrid?solver.cellWidthM:0.5;
		const openvdb::math::Transform::Ptr transform =
			openvdb::math::Transform::createLinearTransform(voxelSize);
		openvdb::FloatGrid::Ptr carbon = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr temperature = openvdb::FloatGrid::create(300.0f);
		openvdb::FloatGrid::Ptr reaction = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCH = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemC2 = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCO2 = openvdb::FloatGrid::create(0.0f);
		openvdb::Vec3fGrid::Ptr velocity = openvdb::Vec3fGrid::create(openvdb::Vec3f(0));
		for( const auto& grid : {openvdb::GridBase::Ptr(carbon),openvdb::GridBase::Ptr(temperature),
			openvdb::GridBase::Ptr(reaction),openvdb::GridBase::Ptr(chemCH),
			openvdb::GridBase::Ptr(chemC2),openvdb::GridBase::Ptr(chemCO2)} ) {
			grid->setTransform(transform->copy());
		}
		openvdb::math::Transform::Ptr velocityTransform = transform->copy();
		velocityTransform->postTranslate(openvdb::Vec3d(-voxelSize));
		velocity->setTransform(velocityTransform);
		carbon->setName("carbon"); temperature->setName("temperature");
		reaction->setName("reaction"); velocity->setName("velocity");
		chemCH->setName("chem_CH"); chemC2->setName("chem_C2"); chemCO2->setName("chem_CO2");
		if(solverGrid && solver.temperature.size()==solver.dimensions[0]*solver.dimensions[1]*
			solver.dimensions[2] && solver.reaction.size()==solver.temperature.size() &&
			solver.carbon.size()==solver.temperature.size() &&
			solver.velocity.size()==solver.temperature.size()) {
			for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
				for(std::size_t x=0;x<solver.dimensions[0];++x) {
					const std::size_t index=(z*solver.dimensions[1]+y)*solver.dimensions[0]+x;
					const openvdb::Coord scalar(static_cast<int>(x),static_cast<int>(y),
						static_cast<int>(z));
					// Solver/checkpoint density is kg/m3; the canonical optical
					// sequence channel is g/m3. Convert once, only at export.
					carbon->tree().setValueOn(scalar,1000.0f*solver.carbon[index]);
					temperature->tree().setValueOn(scalar,solver.temperature[index]);
					reaction->tree().setValueOn(scalar,solver.reaction[index]);
					const std::array<float,3>& v=solver.velocity[index];
					velocity->tree().setValueOn(openvdb::Coord(static_cast<int>(x+1),
						static_cast<int>(y+1),static_cast<int>(z+1)),openvdb::Vec3f(v[0],v[1],v[2]));
				}
		} else {
			carbon->tree().setValueOn(openvdb::Coord(0,0,0),carbonValue);
			temperature->tree().setValueOn(openvdb::Coord(0,0,0),solver.temperatureK);
			reaction->tree().setValueOn(openvdb::Coord(0,0,0),solver.reactionWPerM3);
		}
		if( includeChem ) {
			// Fire solver version 4 (docs/SIMULATION_M8_RESULTS.md section 15; the r272 look,
			// docs/FIRE_SMOKE_SOOT_R272.md): the chemistry-light channels are still a
			// SYNTHETIC_NON_PREDICTIVE preview, but they follow the local heat release only, at a
			// fixed absolute scale: chem_CH = 3e-6 W/m3 of band power per W/m3 of reaction, C2 and
			// CO2 at 50/120 and 8/120 of it (the r272 chem_from_reaction fractions).  The old
			// max(reaction/reaction_max, (T-300)/(T_max-300)) weight lit the whole warm plume
			// (the "blue column") and renormalised every frame.  The unit-render cold fixture keeps
			// its uniform light.
			const double chemFractionOfReaction[3]={3.0e-6,3.0e-6*50.0/120.0,3.0e-6*8.0/120.0};
			if(solverGrid) for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
					for(std::size_t x=0;x<solver.dimensions[0];++x) {
						const std::size_t index=(z*solver.dimensions[1]+y)*solver.dimensions[0]+x;
						const openvdb::Coord fixture(static_cast<int>(x),static_cast<int>(y),
							static_cast<int>(z));
						if( syntheticColdChemistry ) {
							chemCH->tree().setValueOn(fixture,chemScale*120.0f);
							chemC2->tree().setValueOn(fixture,chemScale*50.0f);
							chemCO2->tree().setValueOn(fixture,chemScale*8.0f);
							continue;
						}
						const double reactionWPerM3=solver.reaction[index]>0.0f?
							static_cast<double>(solver.reaction[index]):0.0;
						chemCH->tree().setValueOn(fixture,
							static_cast<float>(chemFractionOfReaction[0]*reactionWPerM3));
						chemC2->tree().setValueOn(fixture,
							static_cast<float>(chemFractionOfReaction[1]*reactionWPerM3));
						chemCO2->tree().setValueOn(fixture,
							static_cast<float>(chemFractionOfReaction[2]*reactionWPerM3));
					}
			else {
				chemCH->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*120.0f);
				chemC2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*50.0f);
				chemCO2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*8.0f);
			}
		}
		if(mutation)mutation->Mutate(FireLegacyFrameGrids{carbon,temperature,reaction,velocity,
			chemCH,chemC2,chemCO2});
		openvdb::GridPtrVec grids{carbon,temperature,reaction,velocity};
		if( includeChem ) {
			grids.push_back(chemCH); grids.push_back(chemC2); grids.push_back(chemCO2);
		}
		openvdb::io::File file(path.string());
		file.write(grids);
		file.close();
		std::string canonicalError;
		const bool canonical=CanonicalizeOpenVDBFileIdentity(path.string(),canonicalError);
		Check(canonical,
			"producer replaces OpenVDB's random UUID with a content-derived identity");
		const bool durable=canonical&&DurableSyncFileAndDirectory(path,canonicalError);
		Check(durable,
			"produced sequence frame is durable before the next simulation step");
		return canonical&&durable;
	}
#endif

#if defined(RISE_ENABLE_OPENVDB)
	bool WriteProductionTemporalFrame(const std::filesystem::path& path,
		const SolverFrameValues& values)
	{
		std::string error;
		const std::filesystem::path prepared=path.string()+".preparation.pending";
		const std::filesystem::path preparedSeal=prepared.string()+".payload-v2.json";
		if(std::filesystem::exists(prepared)||std::filesystem::exists(preparedSeal)||
			std::filesystem::is_symlink(prepared)||std::filesystem::is_symlink(preparedSeal))return false;
		const bool saved=WriteLegacyFireFrame(prepared,0.0f,true,values,0.8f)&&
			PublishPreparedPayload(prepared,path,values.caseRecordId,PayloadPublicationPolicy::Immutable,error);
		CleanupPreparedPayload(prepared,saved);return saved;
	}
#endif
	}
}
