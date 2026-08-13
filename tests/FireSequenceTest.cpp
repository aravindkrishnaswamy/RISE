#include "../src/Library/Utilities/FireSequence.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/HenyeyGreensteinPhaseFunction.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Painters/Perlin3DPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Utilities/FireSimulationRecords.h"
#include "../src/Library/Utilities/Reference.h"
#include "FireOutputMetadataTestFixture.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int failures = 0;

	class FrozenPainterProbe final : public Perlin3DPainter
	{
	public:
		FrozenPainterProbe(const IPainter& a,const IPainter& b) :
			Perlin3DPainter(0.5,3,a,b,Vector3(1,1,1),Vector3(0,0,0)) {}
		Vector3 Scale() const { return vScale; }
		const void* Function() const { return pFunc; }
		~FrozenPainterProbe() override=default;
	};
	class FrozenUniformProbe final : public UniformColorPainter
	{
	public:
		FrozenUniformProbe() : UniformColorPainter(RISEPel(0.25,0.5,0.75)) {}
		RISEPel Value() const { return C; }
		~FrozenUniformProbe() override=default;
	};

	class FrozenMutationOutput final :
		public virtual IRasterizerOutput,
		public virtual IFireRasterizerOutputRoute,
		public virtual Reference
	{
	public:
		explicit FrozenMutationOutput(IJob& job) : job_(job) {}
		void OutputIntermediateImage(const IRasterImage&,const Rect*) override {}
		void OutputImage(const IRasterImage&,const Rect*,unsigned int) override
		{
			attempted=true;
			rejected=!job_.ClearAll() && !job_.SetFilm(2,2,1.0) &&
				!job_.SetGlobalMedium("sequence_fire") &&
				!job_.SetPrimaryAcceleration(true,false,4,32) &&
				!job_.SetFireFidelityMode("preview") &&
				!job_.SetLightSampleRRThreshold(0.25) &&
				!job_.ClearGlobalRadianceMap();
		}
		FireArtifactRouteKind FireArtifactRoute() const override
			{ return FireArtifactRouteKind::DisplayOnly; }
		bool attempted=false;
		bool rejected=false;
	protected:
		~FrozenMutationOutput() override=default;
	private:
		IJob& job_;
	};

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			std::fprintf(stderr,"FAIL: %s\n",message);
			++failures;
		}
	}

	float FloatFromBits( const std::uint32_t bits )
	{
		float value = 0.0f;
		std::memcpy(&value,&bits,sizeof(value));
		return value;
	}

	RISECBOR64::Bytes CanonicalRecord( const char* kind )
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"record_kind",RISECBOR64::Value::String(kind)},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"synthetic embedded record encodes canonically");
		return bytes;
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

	RISECBOR64::Value ReplaceMember( const RISECBOR64::Value& map,
		const char* key, const RISECBOR64::Value& replacement )
	{
		RISECBOR64::Value::Members members = map.GetMap();
		for( auto& member : members ) if( member.first == key ) member.second = replacement;
		return RISECBOR64::Value::MapValue(members);
	}

	RISECBOR64::Bytes EnvelopeForPayload( const RISECBOR64::Value& payload )
	{
		RISECBOR64::Bytes payloadBytes, envelope;
		std::string error;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"mutated payload encodes");
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"payload",payload},{"sequence_id",RISECBOR64::Value::String(
				RISECBOR64::SHA256Hex(payloadBytes))}}),envelope,&error),"mutated envelope encodes");
		return envelope;
	}

	RISECBOR64::Value Channel( const char* name, const char* type,
		const char* units, const char* semantics, const std::vector<double>& background )
	{
		using RISECBOR64::Value;
		Value::Values bg;
		for( const double value : background ) bg.push_back(Value::Float(value));
		const bool velocity = std::strcmp(name,"velocity") == 0;
		return Value::MapValue({
			{"background_value",Value::ArrayValue(bg)},
			{"core_face_bounds_m",Value::ArrayValue({Value::Float(-0.25),Value::Float(-0.25),
				Value::Float(-0.25),Value::Float(0.75),Value::Float(0.75),Value::Float(0.75)})},
			{"dimensions",Value::ArrayValue({Value::Unsigned(velocity ? 4 : 2),Value::Unsigned(velocity ? 4 : 2),Value::Unsigned(velocity ? 4 : 2)})},
			{"name",Value::String(name)},
			{"origin_m",Value::ArrayValue({Value::Float(velocity ? -0.5 : 0),Value::Float(velocity ? -0.5 : 0),Value::Float(velocity ? -0.5 : 0)})},
			{"temporal_semantics",Value::String(semantics)},
			{"units",Value::String(units)},
			{"value_type",Value::String(type)},
			{"voxel_size_m",Value::ArrayValue({Value::Float(0.5),Value::Float(0.5),Value::Float(0.5)})}
		});
	}

	RISECBOR64::Bytes ManifestBytes( const std::string& firstDigest,
		const std::string& secondDigest, const char* endPolicy="hold",
		const bool useProductionOptics=false, const bool syntheticChem=false )
	{
		using RISECBOR64::Value;
		const RISECBOR64::Bytes build = FireOutputMetadataTestFixture::RendererBuild();
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
		const std::string hash64(64u,'a');
		Value::Values channels={
			Channel("carbon","float32","g/m3","frozen_material_advection",{0.0}),
			Channel("temperature","float32","K","frozen_material_advection",{300.0}),
			Channel("reaction","float32","W/m3","derived_eulerian_source",{0.0}),
			Channel("velocity","vec3_float32","m/s","frozen_material_advection",{0.0,0.0,0.0})
		};
		if( syntheticChem ) {
			channels.push_back(Channel("chem_CH","float32","W/m3","derived_eulerian_source",{0.0}));
			channels.push_back(Channel("chem_C2","float32","W/m3","derived_eulerian_source",{0.0}));
			channels.push_back(Channel("chem_CO2","float32","W/m3","derived_eulerian_source",{0.0}));
		}
		const bool preview=useProductionOptics || syntheticChem;
		const Value payload = Value::MapValue({
			{"aerosol_thermochemistry_record",Value::BytesValue(aerosol)},
			{"aerosol_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(aerosol))},
			{"case_record_id",Value::String(hash64)},
			{"channels",Value::ArrayValue(channels)},
			{"chem_record",Value::BytesValue(chem)},
			{"chem_record_id",Value::String(RISECBOR64::SHA256Hex(chem))},
			{"end_policy",Value::String(endPolicy)},
			{"first_frame_index",Value::Unsigned(4)},
			{"frame_count",Value::Unsigned(2)},
			{"frame_encoding",Value::String("openvdb-v1")},
			{"frames",Value::ArrayValue({
				Value::MapValue({{"index",Value::Unsigned(4)},
					{"path",Value::String("frame4.vdb")},{"sha256",Value::String(firstDigest)}}),
				Value::MapValue({{"index",Value::Unsigned(5)},
					{"path",Value::String("frame5.vdb")},{"sha256",Value::String(secondDigest)}})
			})},
			{"fuel_record",Value::BytesValue(fuel)},
			{"fuel_record_id",Value::String(RISECBOR64::SHA256Hex(fuel))},
			{"gas_opacity_record",Value::BytesValue(opacity)},
			{"gas_opacity_record_id",Value::String(RISECBOR64::SHA256Hex(opacity))},
			{"gas_thermochemistry_record",Value::BytesValue(thermo)},
			{"gas_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(thermo))},
			{"gate_evidence_ids",Value::ArrayValue({Value::String(hash64)})},
			{"last_frame_index",Value::Unsigned(5)},
			{"optical_record",Value::BytesValue(optics)},
			{"optical_record_id",Value::String(RISECBOR64::SHA256Hex(optics))},
			{"outside_halo_policy",Value::String("reject_outside_declared_halo")},
			{"physical_mapping",Value::String("absolute_si")},
			{"producer_build_id",Value::String(RISECBOR64::SHA256Hex(build))},
			{"producer_build_v1",Value::BytesValue(build)},
			{"producer_reason_codes",Value::ArrayValue(preview ? Value::Values{
				Value::String(syntheticChem ? "synthetic_chem_fixture" :
					"open_subset_records_preview")} : Value::Values{})},
			{"scene_translation_m",Value::ArrayValue({Value::Float(0),Value::Float(0),Value::Float(0)})},
			{"scene_unit_meters",Value::Float(1.0)},
			{"schema_version",Value::Unsigned(1)},
			{"source_kind",Value::String("rise_simulation")},
			{"source_qualification",Value::String(preview ? "preview_only" : "predictive_qualified")},
			{"temperature_domain_K",Value::ArrayValue({Value::Float(300),Value::Float(2500)})},
			{"time_map",Value::MapValue({
				{"alpha",Value::Float(2.0)},
				{"delta_t_frame",Value::Float(0.25)},
				{"i0",Value::Unsigned(4)},
				{"t0",Value::Float(1.0)},
				{"t_scene_0",Value::Float(10.0)}
			})},
			{"transport_closure_record",Value::BytesValue(transport)},
			{"transport_closure_record_id",Value::String(RISECBOR64::SHA256Hex(transport))},
			{"velocity_halo_width_m",Value::Float(0.5)}
		});
		RISECBOR64::Bytes payloadBytes, envelope;
		std::string error;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"sequence payload encodes");
		Check(RISECBOR64::Encode(Value::MapValue({
			{"payload",payload},
			{"sequence_id",Value::String(RISECBOR64::SHA256Hex(payloadBytes))}
		}),envelope,&error),"sequence envelope encodes");
		return envelope;
	}

	std::string DigestFile( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		input.seekg(0,std::ios::end);
		const std::streampos end = input.tellg();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if( end > 0 ) input.read(reinterpret_cast<char*>(bytes.data()),end);
		return RISECBOR64::SHA256Hex(bytes);
	}

#if defined(RISE_ENABLE_OPENVDB)
	struct FrameMutation
	{
		enum Kind { Valid, NegativeActiveCarbon, HotInactiveTemperature,
			NegativeInactiveCarbon, NonfiniteInactiveCarbon, NonfiniteVelocity,
			PositiveInfinityReaction, NegativeInfinityReaction, ZeroTemperature,
			OutOfDomainTemperature, NegativeActiveTile, HotInactiveTile,
			NaNInactiveTile, NegativeActiveChem, NaNInactiveChem } kind = Valid;
	};

	void WriteFrame( const std::filesystem::path& path, const FrameMutation mutation,
		const float carbonValue, const bool includeChem=false )
	{
		openvdb::initialize();
		const openvdb::math::Transform::Ptr transform =
			openvdb::math::Transform::createLinearTransform(0.5);
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
		velocityTransform->postTranslate(openvdb::Vec3d(-0.5));
		velocity->setTransform(velocityTransform);
		carbon->setName("carbon"); temperature->setName("temperature");
		reaction->setName("reaction"); velocity->setName("velocity");
		chemCH->setName("chem_CH"); chemC2->setName("chem_C2"); chemCO2->setName("chem_CO2");
		carbon->tree().setValueOn(openvdb::Coord(0,0,0),carbonValue);
		temperature->tree().setValueOn(openvdb::Coord(0,0,0),900.0f);
		if( includeChem ) {
			chemCH->tree().setValueOn(openvdb::Coord(0,0,0),120.0f);
			chemC2->tree().setValueOn(openvdb::Coord(0,0,0),50.0f);
			chemCO2->tree().setValueOn(openvdb::Coord(0,0,0),8.0f);
		}
		if( mutation.kind == FrameMutation::NegativeActiveCarbon ) {
			carbon->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::HotInactiveTemperature ) {
			temperature->tree().setValueOff(openvdb::Coord(1,0,0),1200.0f);
		} else if( mutation.kind == FrameMutation::NegativeInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NonfiniteInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		} else if( mutation.kind == FrameMutation::NonfiniteVelocity ) {
			velocity->tree().setValueOn(openvdb::Coord(0,0,0),
				openvdb::Vec3f(FloatFromBits(0x7f800000u),0,0));
		} else if( mutation.kind == FrameMutation::PositiveInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0x7f800000u));
		} else if( mutation.kind == FrameMutation::NegativeInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0xff800000u));
		} else if( mutation.kind == FrameMutation::ZeroTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),0.0f);
		} else if( mutation.kind == FrameMutation::OutOfDomainTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),2501.0f);
		} else if( mutation.kind == FrameMutation::NegativeActiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),-1.0f,true);
		} else if( mutation.kind == FrameMutation::HotInactiveTile ) {
			temperature->tree().addTile(1,openvdb::Coord(0,0,0),1200.0f,false);
		} else if( mutation.kind == FrameMutation::NaNInactiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),FloatFromBits(0x7fc00001u),false);
		} else if( mutation.kind == FrameMutation::NegativeActiveChem ) {
			chemC2->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NaNInactiveChem ) {
			chemCO2->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		}
		openvdb::GridPtrVec grids{carbon,temperature,reaction,velocity};
		if( includeChem ) {
			grids.push_back(chemCH); grids.push_back(chemC2); grids.push_back(chemCO2);
		}
		openvdb::io::File file(path.string());
		file.write(grids);
		file.close();
	}
#endif
}

int main()
{
	PointLight* directLight=new PointLight(1.0,RISEPel(1,1,1),false);
	IKeyframeParameter* lightEnergy=directLight->KeyframeFromParameters("energy","7");
	UniformColorPainter* colorA=new UniformColorPainter(RISEPel(0,0,0));
	UniformColorPainter* colorB=new UniformColorPainter(RISEPel(1,1,1));
	FrozenPainterProbe* painter=new FrozenPainterProbe(*colorA,*colorB);
	FrozenUniformProbe* uniform=new FrozenUniformProbe();
	colorA->release(); colorB->release();
	IKeyframeParameter* painterScale=painter->KeyframeFromParameters("scale","3 4 5");
	IKeyframeParameter* uniformColor=uniform->KeyframeFromParameters("risepel","1 0 0");
	const void* originalFunction=painter->Function();
	Transformable::BeginPreparedMutationFreeze();
	directLight->SetIntermediateValue(*lightEnergy);
	directLight->SetCanGeneratePhotons(true);
	painter->SetIntermediateValue(*painterScale);
	painter->RegenerateData();
	uniform->SetIntermediateValue(*uniformColor);
	Check(directLight->emissionEnergy()==1.0 && !directLight->CanGeneratePhotons() &&
		painter->Scale().x==1.0 &&
		painter->Function()==originalFunction && uniform->Value().g==0.5,
		"direct light and painter keyframe/data mutations fail fast under prepared freeze");
	Transformable::EndPreparedMutationFreeze();
	lightEnergy->release(); painterScale->release(); uniformColor->release();
	directLight->release(); painter->release(); uniform->release();
#if !defined(RISE_ENABLE_OPENVDB)
	std::string error;
	FireSequenceManifest manifest;
	const std::string digest(64u,'a');
	const RISECBOR64::Bytes envelope=ManifestBytes(digest,digest);
	FireSequenceMappedTime mapped;
	FireSequencePreparedFrame frame;
	Check(manifest.LoadCanonicalEnvelope(envelope,".",error) &&
		manifest.MapSceneTime(10.0,mapped,error),
		"capability-independent manifest/time contract remains available");
	Check(!manifest.LoadFrame(4,frame,error) &&
		error=="fire_sequence_openvdb_capability_unavailable",
		"OpenVDB-disabled build fails with the explicit capability result");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),
		"capability-disabled envelope still decodes canonically");
	const RISECBOR64::Value* decodedPayload=decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),".",error),
		"capability-disabled build still enforces the exact manifest schema");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),".",error),
		"capability-disabled build still rejects unratified physical mappings");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex==5,
		"capability-disabled build still applies the complete time map");
	FireSequencePreparationController disabledController(manifest);
	Check(disabledController.SetFrameInstaller([](const FireSequencePreparedFrame&) {
		return true;
	},std::string(64u,'b'),error) && disabledController.SetActiveBindingIdentity(
		std::string(64u,'c'),error),
		"capability-disabled controller accepts only its immutable identity inputs");
	Check(!disabledController.PrepareMediaForRender(
		FireSequenceRenderTimeSupport{10.0,10.0,10.0},false,error) &&
		error=="fire_sequence_openvdb_capability_unavailable" &&
		disabledController.Generation()==0u,
		"capability-disabled preparation fails transactionally before publication");
	std::printf("FireSequenceTest: OpenVDB capability-unavailable gates passed\n");
	return failures ? 1 : 0;
#else
	const std::filesystem::path root = std::filesystem::temp_directory_path()/
		("rise-fire-sequence-test-"+std::to_string(static_cast<unsigned long long>(
			std::chrono::high_resolution_clock::now().time_since_epoch().count())));
	std::filesystem::create_directories(root);
	const std::filesystem::path frame4 = root/"frame4.vdb";
	const std::filesystem::path frame5 = root/"frame5.vdb";
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{},2.0f);

	std::string error;
	FireSequenceManifest manifest;
	RISECBOR64::Bytes envelope = ManifestBytes(DigestFile(frame4),DigestFile(frame5));
	Check(manifest.LoadCanonicalEnvelope(envelope,root.string(),error),
		"canonical sequence envelope loads");
	Check(manifest.SequenceId().size() == 64u && manifest.SourceKind() == "rise_simulation" &&
		manifest.PhysicalMapping() == "absolute_si" && manifest.Frames().size() == 2u,
		"sequence identity and producer qualification survive canonical decode");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),"baseline envelope decodes");
	RISECBOR64::Bytes wrongIdentity;
	Check(RISECBOR64::Encode(ReplaceMember(decodedEnvelope,"sequence_id",
		RISECBOR64::Value::String(std::string(64u,'0'))),wrongIdentity,&error) &&
		!FireSequenceManifest().LoadCanonicalEnvelope(wrongIdentity,root.string(),error),
		"sequence_id must hash the exact canonical payload preimage");
	const RISECBOR64::Value* decodedPayload = decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_count",
			RISECBOR64::Value::Unsigned(3))),root.string(),error),
		"self-consistently rehashed inconsistent frame count rejects semantically");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"producer_build_id",
			RISECBOR64::Value::String(std::string(64u,'0')))),root.string(),error),
		"self-consistently rehashed producer-build identity mutation rejects");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),root.string(),error),
		"frame encoding is exact OpenVDB v1");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),root.string(),error),
		"unknown normalized-to-physical mapping profiles reject");
	if( decodedPayload ) {
		const RISECBOR64::Bytes incompleteBuild=CanonicalRecord("producer_build_v1");
		RISECBOR64::Value badBuild=ReplaceMember(*decodedPayload,"producer_build_v1",
			RISECBOR64::Value::BytesValue(incompleteBuild));
		badBuild=ReplaceMember(badBuild,"producer_build_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(incompleteBuild)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badBuild),
			root.string(),error),"hash-valid incomplete producer build identity rejects");
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		if( channelValue ) {
			auto channels=channelValue->GetArray();
			channels[0]=ReplaceMember(channels[0],"temporal_semantics",
				RISECBOR64::Value::String("derived_eulerian_source"));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"material/source temporal roles cannot be exchanged");
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*decodedPayload,"velocity_halo_width_m",
				RISECBOR64::Value::Float(std::numeric_limits<double>::max()))),root.string(),error),
			"finite halo widths whose derived cell count overflows reject before conversion");
		const RISECBOR64::Value* timeMap=decodedPayload->Find("time_map");
		if( timeMap ) {
			const RISECBOR64::Value hugeTimeMap=ReplaceMember(*timeMap,"i0",
				RISECBOR64::Value::Signed(std::numeric_limits<std::int64_t>::max()));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"time_map",hugeTimeMap)),root.string(),error),
				"frame ranges that would overflow signed index arithmetic reject first");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Bytes arbitrary=CanonicalRecord("arbitrary_record_v1");
		RISECBOR64::Value badAerosol=ReplaceMember(*decodedPayload,
			"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(arbitrary));
		badAerosol=ReplaceMember(badAerosol,"aerosol_thermochemistry_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badAerosol),
			root.string(),error),"hash-valid arbitrary aerosol semantics reject");
		RISECBOR64::Value badChem=ReplaceMember(*decodedPayload,"chem_record",
			RISECBOR64::Value::BytesValue(arbitrary));
		badChem=ReplaceMember(badChem,"chem_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badChem),
			root.string(),error),"hash-valid arbitrary chemistry semantics reject");
		RISECBOR64::Value aerosolRecord;
		const RISECBOR64::Value* aerosolBytes=decodedPayload->Find(
			"aerosol_thermochemistry_record");
		if( aerosolBytes && RISECBOR64::DecodeCanonical(aerosolBytes->GetBytes(),
			aerosolRecord,&error) ) {
			RISECBOR64::Value carbon=*aerosolRecord.Find("carbon_phase");
			carbon=ReplaceMember(carbon,"source_fuel_record_id",
				RISECBOR64::Value::String(std::string(64u,'0')));
			aerosolRecord=ReplaceMember(aerosolRecord,"carbon_phase",carbon);
			RISECBOR64::Bytes badAerosolBytes;
			Check(RISECBOR64::Encode(aerosolRecord,badAerosolBytes,&error),
				"aerosol reference mutation encodes");
			RISECBOR64::Value badReference=ReplaceMember(*decodedPayload,
				"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(badAerosolBytes));
			badReference=ReplaceMember(badReference,"aerosol_thermochemistry_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(badAerosolBytes)));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badReference),
				root.string(),error),"aerosol carbon thermochemistry binds the methane fuel record");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		for( unsigned int channelIndex=0; channelValue && channelIndex<4u; ++channelIndex ) {
			auto channels=channelValue->GetArray();
			auto background=channels[channelIndex].Find("background_value")->GetArray();
			background[0]=RISECBOR64::Value::Float(channelIndex==1u ? 301.0 : 1.0);
			channels[channelIndex]=ReplaceMember(channels[channelIndex],"background_value",
				RISECBOR64::Value::ArrayValue(background));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"mandatory inactive-channel backgrounds reject mutation");
		}
	}

	FireSequenceMappedTime mapped;
	Check(manifest.MapSceneTime(10.0,mapped,error) && mapped.baseFrameIndex == 4 &&
		mapped.simulationTime == 1.0 && mapped.advectionOffsetSeconds == 0.0,
		"time map applies t0/alpha/t_scene_0 exactly");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex == 5 &&
		mapped.advectionOffsetSeconds == 0.25,
		"hold endpoint selects the last frame with one full-frame advection offset");
	Check(manifest.MapSceneTime(20.0,mapped,error) && mapped.baseFrameIndex == 5 && mapped.held,
		"hold policy clamps beyond authored support");
	FireSequenceManifest errorManifest;
	Check(errorManifest.LoadCanonicalEnvelope(
		ManifestBytes(DigestFile(frame4),DigestFile(frame5),"error"),root.string(),error) &&
		!errorManifest.MapSceneTime(10.25,mapped,error),
		"error policy rejects the half-open authored endpoint");

	FireSequencePreparedFrame loaded;
	Check(manifest.LoadFrame(4,loaded,error) && loaded.channels.size() == 4u &&
		loaded.channels.at("carbon").maximum == 1.0 &&
		loaded.channels.at("temperature").minimum == 300.0,
		"frame digest, topology, backgrounds, active values, and extrema preflight");

	const FrameMutation::Kind badKinds[] = {
		FrameMutation::NegativeActiveCarbon, FrameMutation::HotInactiveTemperature,
		FrameMutation::NegativeInactiveCarbon, FrameMutation::NonfiniteInactiveCarbon,
		FrameMutation::NonfiniteVelocity, FrameMutation::PositiveInfinityReaction,
		FrameMutation::NegativeInfinityReaction, FrameMutation::ZeroTemperature,
		FrameMutation::OutOfDomainTemperature, FrameMutation::NegativeActiveTile,
		FrameMutation::HotInactiveTile, FrameMutation::NaNInactiveTile };
	for( const FrameMutation::Kind kind : badKinds ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f);
		FireSequenceManifest bad;
		Check(bad.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
			root.string(),error) && !bad.LoadFrame(4,loaded,error),
			"active and stored value-off defects reject before fidelity/derived structures");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	FireSequenceManifest chemManifest;
	Check(chemManifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true),root.string(),error) && chemManifest.HasChemChannels() &&
		chemManifest.PreflightAllFrames(error),
		"preview-only synthetic chem record enables and preflights the complete triplet");
	RISECBOR64::Value chemDecoded;
	const RISECBOR64::Bytes chemEnvelopeForLattice=ManifestBytes(DigestFile(frame4),
		DigestFile(frame5),"hold",false,true);
	Check(RISECBOR64::DecodeCanonical(chemEnvelopeForLattice,chemDecoded,&error),
		"chem envelope decodes for lattice RED");
	const RISECBOR64::Value* chemPayload=chemDecoded.Find("payload");
	const RISECBOR64::Value* chemChannels=chemPayload ? chemPayload->Find("channels") : nullptr;
	if( chemPayload && chemChannels ) {
		auto shifted=chemChannels->GetArray();
		for( auto& channel : shifted ) if( channel.Find("name") &&
			channel.Find("name")->GetText()=="chem_CH" ) {
			channel=ReplaceMember(channel,"origin_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.5),RISECBOR64::Value::Float(0.0),
				RISECBOR64::Value::Float(0.0)}));
			channel=ReplaceMember(channel,"core_face_bounds_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.25),RISECBOR64::Value::Float(-0.25),
				RISECBOR64::Value::Float(-0.25),RISECBOR64::Value::Float(1.25),
				RISECBOR64::Value::Float(0.75),RISECBOR64::Value::Float(0.75)}));
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*chemPayload,"channels",RISECBOR64::Value::ArrayValue(shifted))),
			root.string(),error),
			"chem lattices cannot be shifted while the renderer maps them onto carbon space");
	}
	FireSequencePreparedFrame chemFrame;
	Check(chemManifest.LoadFrame(4,chemFrame,error) && chemFrame.channels.size()==7u &&
		chemFrame.channels.at("chem_CH").maximum==120.0 &&
		chemFrame.channels.at("chem_C2").maximum==50.0 &&
		chemFrame.channels.at("chem_CO2").maximum==8.0,
		"chem triplet survives digest-bound OpenVDB decoding with absolute W/m3 values");
	HenyeyGreensteinPhaseFunction* chemPhase=new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* chemMedium=new MultichannelHeterogeneousMedium(
		chemFrame,chemManifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),1.0,
		FireOpticsPreset::PredictiveV1(),*chemPhase);
	chemPhase->release();
	Check(chemMedium->IsValid() && chemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"sequence-backed medium installs the chem accessors and normalized fixture SPD");
	chemMedium->release();
	for( const FrameMutation::Kind kind : {FrameMutation::NegativeActiveChem,
		FrameMutation::NaNInactiveChem} ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f,true);
		FireSequenceManifest badChemFrame;
		Check(badChemFrame.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),
			DigestFile(frame5),"hold",false,true),root.string(),error) &&
			!badChemFrame.LoadFrame(4,loaded,error),
			"negative and stored value-off nonfinite chem payloads reject before fidelity");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	const std::filesystem::path chemManifestPath=root/"sequence_chem_fixture.rise-fire.cbor";
	const RISECBOR64::Bytes chemEnvelope=ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true);
	{
		std::ofstream output(chemManifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(chemEnvelope.data()),
			static_cast<std::streamsize>(chemEnvelope.size()));
	}
	IJob* chemJob=nullptr;
	Check(RISE_CreateJob(&chemJob) && chemJob && chemJob->AddFireMediumBound(
		"chem_sequence",chemManifestPath.string().c_str(),"carbon","temperature","",
		"reaction","chem_CH","chem_C2","chem_CO2","velocity",false),
		"Job consumes the preview-only non-none chem record through the complete binding path");
	const MultichannelHeterogeneousMedium* jobChemMedium=chemJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(chemJob->GetMedium("chem_sequence")) : nullptr;
	Check(jobChemMedium && jobChemMedium->IsValid() &&
		jobChemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"Job-installed sequence medium retains nonzero absolute chemistry emission");
	if( chemJob ) chemJob->release();
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{FrameMutation::NegativeActiveCarbon},2.0f);
	FireSequenceManifest laterMalformed;
	Check(laterMalformed.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error) && !laterMalformed.PreflightAllFrames(error),
		"loadability preflight scans later frames before any frame is activated");
	WriteFrame(frame5,FrameMutation{},2.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"restored valid manifest loads");
	std::fstream truncate(frame4,std::ios::binary|std::ios::in|std::ios::out);
	truncate.seekp(0,std::ios::end);
	const std::streampos originalSize = truncate.tellp();
	truncate.close();
	std::filesystem::resize_file(frame4,static_cast<std::uintmax_t>(originalSize)-1u);
	Check(!manifest.LoadFrame(4,loaded,error),"truncated frame is rejected by whole-file digest");
	WriteFrame(frame4,FrameMutation{},1.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"controller fixture manifest loads");

	FireSequencePreparationController controller(manifest);
	unsigned int installedFrames = 0u;
	const std::string preparedComponentIdentity(64u,'b');
	Check(controller.SetFrameInstaller(
		[&installedFrames](const FireSequencePreparedFrame& candidate) {
			++installedFrames;
			return candidate.channels.at("temperature").minimum > 0.0;
		},preparedComponentIdentity,error) && controller.SetActiveBindingIdentity(
			std::string(64u,'c'),error),"prepared owner binds one transactional frame installer");
	FireSequenceRenderTimeSupport support{10.0,10.0,10.0};
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.MajorantGeneration() == 1u &&
		controller.EmissionCDFGeneration() == 1u,
		"frame advance transaction rebuilds majorant and emission CDF once");
	Check(installedFrames == 1u,"first prepared generation performs one real install callback");
	const std::string firstPreparedId = controller.PreparedInputId();
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.PreparedInputId() == firstPreparedId,
		"identical complete prepared-input identity reuses immutable state");
	Check(installedFrames == 1u,"identical prepared input does not rebuild derived structures");
	FireSequencePreparationController::RenderLease lease = controller.AcquireRenderLease(error);
	Check(lease.IsValid() && lease.StateStayedFrozen(),"render lease captures immutable prepared state");
	FireSequenceRenderTimeSupport next{10.125,10.125,10.125};
	Check(!controller.PrepareMediaForRender(next,false,error) && error == "mutation_frozen" &&
		lease.StateStayedFrozen(),"mid-render frame/majorant/CDF swap fails immediately");
	lease = FireSequencePreparationController::RenderLease();
	Check(controller.PrepareMediaForRender(next,false,error) && controller.Generation() == 2u &&
		controller.PreparedInputId() != firstPreparedId,
		"between-render frame advance atomically publishes a new prepared generation");
	Check(installedFrames == 2u,"frame advance performs exactly one new install callback");
	const std::string secondPreparedId=controller.PreparedInputId();
	Check(controller.SetActiveBindingIdentity(std::string(64u,'e'),error) &&
		controller.PrepareMediaForRender(next,false,error) && controller.Generation()==3u &&
		controller.PreparedInputId()!=secondPreparedId && installedFrames==3u,
		"identical-time binding mutation changes the complete prepared identity and rebuilds");

	FireSequencePreparedFrame physicalInitial;
	Check(manifest.LoadFrame(4,physicalInitial,error),"physical medium fixture reloads frame 4");
	HenyeyGreensteinPhaseFunction* phase = new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* medium = new MultichannelHeterogeneousMedium(
		physicalInitial,manifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),
		1.0,FireOpticsPreset::PredictiveV1(),*phase);
	phase->release();
	Check(medium->IsValid(),"manifest frame constructs the production fire medium");
	FireSequencePreparationController physicalController(manifest);
	Check(medium->BindSequencePreparationController(physicalController,
		preparedComponentIdentity,error) && physicalController.SetActiveBindingIdentity(
			std::string(64u,'d'),error),
		"physical medium is the prepared owner's sole frame installer");
	const unsigned long long initialMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long initialEmission = medium->ForTest_FireEmissionGeneration();
	Check(physicalController.PrepareMediaForRender(support,false,error) &&
		medium->FireDerivedStructuresCurrent() &&
		medium->ForTest_FireMajorantGeneration() == initialMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == initialEmission+1u,
		"prepared frame install performs the medium's real majorant and emission-CDF rebuild");
	const unsigned long long firstPhysicalMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long firstPhysicalEmission = medium->ForTest_FireEmissionGeneration();
	FireSequencePreparationController::RenderLease physicalLease =
		physicalController.AcquireRenderLease(error);
	Check(!physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission,
		"mid-render mutation cannot reach grid, majorant, or emission CDF");
	physicalLease = FireSequencePreparationController::RenderLease();
	Check(physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission+1u,
		"between-render frame advance rebuilds both real derived structures once");
	medium->release();

	const std::filesystem::path manifestPath = root/"sequence.rise-fire.cbor";
	const RISECBOR64::Bytes productionEnvelope = ManifestBytes(
		DigestFile(frame4),DigestFile(frame5),"hold",true);
	FireSequenceManifest productionManifest;
	Check(productionManifest.LoadCanonicalEnvelope(productionEnvelope,root.string(),error),
		"production-shaped Job manifest validates independently");
	{
		std::ofstream output(manifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(productionEnvelope.data()),
			static_cast<std::streamsize>(productionEnvelope.size()));
	}
	{
		RISECBOR64::Value productionDecoded;
		Check(RISECBOR64::DecodeCanonical(productionEnvelope,productionDecoded,&error),
			"production envelope decodes for cross-record RED");
		const RISECBOR64::Value* payload=productionDecoded.Find("payload");
		const RISECBOR64::Bytes syntheticOptics=FireOpticsPreset::SyntheticRegressionV1().RecordBytes();
		if( payload ) {
			RISECBOR64::Value mismatch=ReplaceMember(*payload,"optical_record",
				RISECBOR64::Value::BytesValue(syntheticOptics));
			mismatch=ReplaceMember(mismatch,"optical_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(syntheticOptics)));
			const std::filesystem::path mismatchPath=root/"sequence_mismatched_optics.rise-fire.cbor";
			const RISECBOR64::Bytes mismatchEnvelope=EnvelopeForPayload(mismatch);
			std::ofstream output(mismatchPath,std::ios::binary);
			output.write(reinterpret_cast<const char*>(mismatchEnvelope.data()),
				static_cast<std::streamsize>(mismatchEnvelope.size()));
			output.close();
			IJob* mismatchJob=nullptr;
			Check(RISE_CreateJob(&mismatchJob) && mismatchJob &&
				!mismatchJob->AddFireMedium("mismatch",mismatchPath.string().c_str()),
				"fuel soot-density reference rejects a different valid optics identity");
			if( mismatchJob ) mismatchJob->release();
		}
	}
	IJob* job = nullptr;
	Check(RISE_CreateJob(&job) && job,"sequence binding test creates a Job");
	Check(job && job->AddFireMedium("sequence_fire",manifestPath.string().c_str()) &&
		job->SetGlobalMedium("sequence_fire"),
		"fire_medium creates a named manager entry and binds through global_medium");
	if( job ) job->release();
	const std::filesystem::path scenePath = root/"sequence_scene.RISEscene";
	const std::filesystem::path renderBase = root/"sequence_render";
#if defined(_WIN32)
	_putenv_s("RISE_MEDIA_PATH",(root.string()+"/").c_str());
#else
	setenv("RISE_MEDIA_PATH",(root.string()+"/").c_str(),1);
#endif
	{
		std::ofstream scene(scenePath);
		scene << "RISE ASCII SCENE 7\n\n"
			<< "scene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
			<< "standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			<< "pathtracing_spectral_rasterizer\n{\nsamples 1\nnmbegin 380\n"
			<< "nmend 780\nnum_wavelengths 1\nspectral_samples 1\nhwss false\n"
			<< "pixel_filter box\noidn_denoise false\n}\n\n"
			<< "file_rasterizeroutput\n{\npattern sequence_render"
			<< "\ntype EXR\nbpp 32\ncolor_space Rec709RGB_Linear"
			<< "\nexposure 0\ndisplay_transform none\nexr_compression piz\n}\n\n"
			<< "film\n{\nwidth 3\nheight 3\n}\n\n"
			<< "pinhole_camera\n{\nname camera\nlocation 0 0 -2\nlookat 0 0 0\n"
			<< "up 0 1 0\nfov 45\nexposure 0.04\nscanning_rate -0.1\n"
			<< "pixel_rate 0.02\n}\n\nfire_medium\n{\n"
			<< "name sequence_fire\nfidelity_mode preview\nsequence_manifest "
			<< manifestPath.string() << "\nchannel_carbon carbon\n"
			<< "channel_temperature temperature\nchannel_reaction reaction\n"
			<< "chem_model none\nchannel_velocity velocity\n}\n\n"
			<< "global_medium\n{\nmedium sequence_fire\n}\n";
	}
	IJobPriv* parsedJob = nullptr;
	Check(RISE_CreateJobPriv(&parsedJob) && parsedJob &&
		parsedJob->LoadAsciiSceneViaCst(scenePath.string().c_str()),
		"descriptor-driven fire_medium chunk binds the sequence through the existing global-medium seam");
	const RenderTimeSupport fullSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_BOTH) :
		RenderTimeSupport();
	const RenderTimeSupport oddFieldSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_LOWER) :
		RenderTimeSupport();
	Check(std::fabs(fullSupport.open-9.88)<1e-12 &&
		std::fabs(fullSupport.close-10.16)<1e-12 &&
		std::fabs(oddFieldSupport.open-9.98)<1e-12 &&
		std::fabs(oddFieldSupport.close-10.06)<1e-12,
		"prepared time support is sign-aware over exposure, scan, pixels, and field parity");
	Check(parsedJob && parsedJob->SetFilm(1,1,1.0),
		"render fixture reduces to one pixel after qualifying time-support geometry");
	const MultichannelHeterogeneousMedium* parsedMedium = parsedJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(parsedJob->GetMedium("sequence_fire")) : nullptr;
	const unsigned long long beforeRenderMajorant = parsedMedium ?
		parsedMedium->ForTest_FireMajorantGeneration() : 0u;
	Check(parsedJob && parsedJob->SetAnimationOptions(10.0,10.25,2,false,false) &&
		parsedJob->Rasterize(),
		"sequence-backed Job render enters the prepared rasterizer seam and completes");
	const IRasterizer* parsedRasterizer = parsedJob ? parsedJob->GetRasterizer() : nullptr;
	const FrameStore* parsedStore = parsedRasterizer ? parsedRasterizer->GetFrameStore() : nullptr;
	const FrameStore::Metadata parsedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	const bool publishedSequence = parsedStore && parsedMetadata.activeFireMedia.size()==1u &&
		parsedMetadata.activeFireMedia[0].mediaKind=="sequence_backed" &&
		parsedMetadata.activeFireMedia[0].sequenceId==productionManifest.SequenceId() &&
		parsedMetadata.activeFireMedia[0].selectedBaseFrameIndex==4 &&
		parsedMetadata.activeFireMedia[0].wholeFileDigest==DigestFile(frame4) &&
		!parsedMetadata.activeFireMedia[0].preparedInputId.empty();
	Check(publishedSequence,
		"actual prepared render publishes the sequence-backed provenance variant");
	RISECBOR64::Bytes renderedSidecar;
	{
		const std::filesystem::path sidecar=renderBase.string()+".exr.provenance.cbor";
		std::ifstream input(sidecar,std::ios::binary);
		input.seekg(0,std::ios::end);
		const std::streampos size=input.tellg();
		input.seekg(0,std::ios::beg);
		if( size > 0 ) {
			renderedSidecar.resize(static_cast<std::size_t>(size));
			input.read(reinterpret_cast<char*>(renderedSidecar.data()),size);
		}
	}
	RISECBOR64::Value renderedEnvelope;
	const bool renderedEnvelopeValid=RISECBOR64::DecodeCanonical(
		renderedSidecar,renderedEnvelope,&error);
	const RISECBOR64::Value* renderedPayload=renderedEnvelopeValid ?
		renderedEnvelope.Find("payload") : nullptr;
	const RISECBOR64::Value* renderedMedia=renderedPayload ?
		renderedPayload->Find("active_fire_media") : nullptr;
	Check(renderedMedia && renderedMedia->GetType()==RISECBOR64::Value::Array &&
		renderedMedia->GetArray().size()==1u &&
		renderedMedia->GetArray()[0].Find("sequence_id") &&
		renderedMedia->GetArray()[0].Find("sequence_id")->GetText()==
			productionManifest.SequenceId() &&
		renderedMedia->GetArray()[0].Find("whole_file_digest") &&
		renderedMedia->GetArray()[0].Find("prepared_input_id"),
		"real sequence render writes the ratified sequence_backed provenance sidecar");
	Check(parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+1u,
		"actual render schedules exactly one per-frame majorant/CDF rebuild");
	const std::string jobFirstPrepared = parsedStore &&
		!parsedMetadata.activeFireMedia.empty() ?
		parsedMetadata.activeFireMedia[0].preparedInputId : std::string();
	Check(parsedJob && parsedJob->SetAnimationOptions(10.25,10.25,1,false,false) &&
		parsedJob->Rasterize(),
		"between-render scene-time advance prepares the next immutable frame");
	const FrameStore::Metadata advancedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	Check(parsedStore && advancedMetadata.activeFireMedia.size()==1u &&
		advancedMetadata.activeFireMedia[0].selectedBaseFrameIndex==5 &&
		advancedMetadata.activeFireMedia[0].preparedInputId!=jobFirstPrepared &&
		parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"frame advance atomically swaps grid, majorant, CDF, and provenance between renders");
	FrozenMutationOutput* mutationOutput=parsedJob ? new FrozenMutationOutput(*parsedJob) : nullptr;
	if( parsedRasterizer && mutationOutput )
		const_cast<IRasterizer*>(parsedRasterizer)->AddRasterizerOutput(mutationOutput);
	Check(parsedJob && parsedJob->Rasterize() && mutationOutput && mutationOutput->attempted &&
		mutationOutput->rejected && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"mid-render Job mutation is detected while the prepared grid/majorant/CDF stay frozen");
	if( mutationOutput ) mutationOutput->release();
	Check(parsedJob && !parsedJob->RasterizeAnimation(10.0,10.25,2,true,false) &&
		parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"cadence-crossing interlaced fields reject before one artifact can misstate its base frame");
	Check(parsedJob && parsedJob->RasterizeAnimation(10.25,10.25,1,true,false) &&
		parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"same-base-frame interlaced fields reuse one truthful prepared identity");
	const IFireRasterizerState* preparedState=parsedRasterizer ?
		dynamic_cast<const IFireRasterizerState*>(parsedRasterizer) : nullptr;
	if( parsedRasterizer && parsedJob && parsedJob->GetScene() )
		parsedRasterizer->RasterizeScene(*parsedJob->GetScene(),nullptr,nullptr);
	Check(preparedState && !preparedState->LastRenderCompleted() && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"legacy direct rasterizer entry cannot bypass sequence preparation or mutate state");
	if( parsedJob ) parsedJob->release();

	FrameStoreOutput::Metadata metadata;
	metadata.renderFidelityStatus = "preview";
	metadata.renderReasonCodes = {"requested_preview"};
	metadata.activeFireOpticsRecordIds = {std::string(64u,'b')};
	FrameStoreOutput::ActiveFireMedium sequenceMedium;
	sequenceMedium.mediaKind = "sequence_backed";
	sequenceMedium.managerName = "fire";
	sequenceMedium.bindingKind = "global_medium";
	sequenceMedium.bindingOwner = "scene";
	sequenceMedium.opticalRecordIds = metadata.activeFireOpticsRecordIds;
	sequenceMedium.sequenceId = manifest.SequenceId();
	sequenceMedium.selectedBaseFrameIndex = controller.PreparedFrame()->frameIndex;
	sequenceMedium.wholeFileDigest = controller.PreparedFrame()->wholeFileSha256;
	sequenceMedium.sourceKind = manifest.SourceKind();
	sequenceMedium.physicalMapping = manifest.PhysicalMapping();
	sequenceMedium.effectiveBlurState = "disabled";
	sequenceMedium.preparedInputId = controller.PreparedInputId();
	sequenceMedium.preparedStateGeneration = controller.Generation();
	metadata.activeFireMedia.push_back(sequenceMedium);
	metadata.resolvedRenderConfigCoreV1 = FireOutputMetadataTestFixture::ResolvedConfig(2,2);
	metadata.rendererBuildV1 = FireOutputMetadataTestFixture::RendererBuild();
	metadata.rendererBuildId = RISECBOR64::SHA256Hex(metadata.rendererBuildV1);
	Check(FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"ratified active_fire_media sequence_backed tagged variant validates");
	metadata.activeFireMedia[0].authoredConfigDigest = std::string(64u,'c');
	Check(!FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"sequence_backed provenance rejects a structurally present static-only field");

	std::filesystem::remove_all(root);
	std::printf("FireSequenceTest: canonical sequence/loadability/preparation gates passed\n");
	return failures ? 1 : 0;
#endif
}
