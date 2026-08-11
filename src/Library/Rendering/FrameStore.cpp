//////////////////////////////////////////////////////////////////////
//
//  FrameStore.cpp - Canonical HDR frame buffer implementation.
//
//  See FrameStore.h for the design context, lifetime conventions,
//  and observer-attachment semantics.
//
//  Concurrency: tile-level shared/exclusive locking per
//  docs/FRAMESTORE_DESIGN.md §4.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameStore.h"
#include "../Interfaces/IRenderObserver.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/RISECBOR64.h"
#include "AOVBuffers.h"  // L7 — PropagateAOVsToFrameStore

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>

namespace
{
	struct ObserverCallbackFrame
	{
		const RISE::Implementation::FrameStore* store;
		RISE::IRenderObserver* observer;
		ObserverCallbackFrame* prior;
	};

	thread_local ObserverCallbackFrame* g_observerCallbackFrame = nullptr;

	void RejectReentrantObserverPublication()
	{
		if( g_observerCallbackFrame ) {
			throw std::runtime_error("FrameStore observer dispatch is reentrant");
		}
	}

	unsigned int ObserverActiveCountOnThisThread(
		const RISE::Implementation::FrameStore* store,
		const RISE::IRenderObserver* observer )
	{
		unsigned int count = 0u;
		for( ObserverCallbackFrame* frame = g_observerCallbackFrame;
			frame; frame = frame->prior ) {
			if( frame->store == store && frame->observer == observer ) ++count;
		}
		return count;
	}
}

using namespace RISE;
using namespace RISE::FrameStoreOutput;

namespace
{
	bool IsSHA256Hex( const std::string& value )
	{
		if( value.size() != 64u ) return false;
		for( const char c : value ) {
			if( !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) ) return false;
		}
		return true;
	}

	bool IsStrictlySorted( const std::vector<std::string>& values )
	{
		return std::adjacent_find(values.begin(),values.end(),
			[]( const std::string& lhs, const std::string& rhs ) {
				return lhs >= rhs;
			}) == values.end();
	}

	bool HasRecordHeader(
		const RISECBOR64::Value& value,
		const char* recordKind )
	{
		const RISECBOR64::Value* kind = value.Find("record_kind");
		const RISECBOR64::Value* version = value.Find("schema_version");
		return kind && kind->GetType() == RISECBOR64::Value::Text &&
			kind->GetText() == recordKind && version &&
			version->GetType() == RISECBOR64::Value::UnsignedInteger &&
			version->GetIntegerArgument() == 1u;
	}

	bool ExactMapKeys(
		const RISECBOR64::Value& value,
		const std::initializer_list<const char*> keys,
		const char* context,
		std::string& error )
	{
		if( value.GetType() != RISECBOR64::Value::Map ) {
			error = std::string(context)+" is not a map";
			return false;
		}
		std::set<std::string> expected;
		for( const char* key : keys ) expected.insert(key);
		if( value.GetMap().size() != expected.size() ) {
			error = std::string(context)+" has missing or unknown schema-v1 fields";
			return false;
		}
		for( const auto& member : value.GetMap() ) {
			if( expected.erase(member.first) != 1u ) {
				error = std::string(context)+" has missing or unknown schema-v1 fields";
				return false;
			}
		}
		return expected.empty();
	}

	bool FieldType(
		const RISECBOR64::Value& map,
		const char* key,
		const RISECBOR64::Value::Type type,
		const char* context,
		std::string& error )
	{
		const RISECBOR64::Value* value = map.Find(key);
		if( !value || value->GetType() != type ) {
			error = std::string(context)+" field '"+key+"' has the wrong type";
			return false;
		}
		return true;
	}

	bool NonemptyTextField(
		const RISECBOR64::Value& map,
		const char* key,
		const char* context,
		std::string& error )
	{
		if( !FieldType(map,key,RISECBOR64::Value::Text,context,error) ) return false;
		if( map.Find(key)->GetText().empty() ) {
			error = std::string(context)+" field '"+key+"' is empty";
			return false;
		}
		return true;
	}

	bool TextFieldIn(
		const RISECBOR64::Value& map,
		const char* key,
		const std::initializer_list<const char*> values,
		const char* context,
		std::string& error )
	{
		if( !FieldType(map,key,RISECBOR64::Value::Text,context,error) ) return false;
		const std::string& actual = map.Find(key)->GetText();
		for( const char* value : values ) {
			if( actual == value ) return true;
		}
		error = std::string(context)+" field '"+key+"' is outside schema-v1";
		return false;
	}

	bool UnsignedFieldAtMost(
		const RISECBOR64::Value& map,
		const char* key,
		const std::uint64_t maximum,
		const char* context,
		std::string& error )
	{
		if( !FieldType(map,key,RISECBOR64::Value::UnsignedInteger,context,error) )
			return false;
		if( map.Find(key)->GetIntegerArgument() <= maximum ) return true;
		error = std::string(context)+" field '"+key+"' is outside schema-v1";
		return false;
	}

	bool IntegerField(
		const RISECBOR64::Value& map,
		const char* key,
		const char* context,
		std::string& error )
	{
		const RISECBOR64::Value* value = map.Find(key);
		if( !value || (value->GetType() != RISECBOR64::Value::UnsignedInteger &&
			value->GetType() != RISECBOR64::Value::NegativeInteger) ) {
			error = std::string(context)+" field '"+key+"' is not an integer";
			return false;
		}
		return true;
	}

	bool ArrayElementsAre(
		const RISECBOR64::Value& map,
		const char* key,
		const RISECBOR64::Value::Type type,
		const std::size_t exactSize,
		const char* context,
		std::string& error )
	{
		const RISECBOR64::Value* value = map.Find(key);
		if( !value || value->GetType() != RISECBOR64::Value::Array ||
			(exactSize != static_cast<std::size_t>(-1) &&
			 value->GetArray().size() != exactSize) ) {
			error = std::string(context)+" field '"+key+"' has the wrong array shape";
			return false;
		}
		for( const RISECBOR64::Value& element : value->GetArray() ) {
			if( element.GetType() != type ) {
				error = std::string(context)+" field '"+key+"' has the wrong element type";
				return false;
			}
		}
		return true;
	}

	bool ValidateCameraSchema(
		const RISECBOR64::Value& camera,
		std::string& error )
	{
		if( !ExactMapKeys(camera,{ "exposure_compensation_ev", "exposure_time",
			"kind", "location", "matrix", "pixel_rate", "projection",
			"scanning_rate" },"resolved camera",error) ||
			!FieldType(camera,"exposure_compensation_ev",RISECBOR64::Value::Float64,
				"resolved camera",error) ||
			!FieldType(camera,"exposure_time",RISECBOR64::Value::Float64,
				"resolved camera",error) ||
			!FieldType(camera,"kind",RISECBOR64::Value::Text,"resolved camera",error) ||
			!FieldType(camera,"pixel_rate",RISECBOR64::Value::Float64,
				"resolved camera",error) ||
			!FieldType(camera,"scanning_rate",RISECBOR64::Value::Float64,
				"resolved camera",error) ) return false;
		const RISECBOR64::Value* kind = camera.Find("kind");
		const std::size_t vectorSize = kind->GetText() == "none" ? 0u : 3u;
		const std::size_t matrixSize = kind->GetText() == "none" ? 0u : 16u;
		if( !ArrayElementsAre(camera,"location",RISECBOR64::Value::Float64,
			vectorSize,"resolved camera",error) ||
			!ArrayElementsAre(camera,"matrix",RISECBOR64::Value::Float64,
			matrixSize,"resolved camera",error) ) return false;
		const RISECBOR64::Value* projection = camera.Find("projection");
		if( !projection ) return false;
		if( kind->GetText() == "none" ) {
			return ExactMapKeys(*projection,{},"resolved camera projection",error);
		}
		if( kind->GetText() == "pinhole" ) {
			return ExactMapKeys(*projection,{"fov_radians","fstop","iso"},
				"resolved pinhole projection",error) &&
				FieldType(*projection,"fov_radians",RISECBOR64::Value::Float64,
					"resolved pinhole projection",error) &&
				FieldType(*projection,"fstop",RISECBOR64::Value::Float64,
					"resolved pinhole projection",error) &&
				FieldType(*projection,"iso",RISECBOR64::Value::Float64,
					"resolved pinhole projection",error);
		}
		if( kind->GetText() == "fisheye" ) {
			return ExactMapKeys(*projection,{"scale"},"resolved fisheye projection",error) &&
				FieldType(*projection,"scale",RISECBOR64::Value::Float64,
					"resolved fisheye projection",error);
		}
		if( kind->GetText() == "orthographic" ) {
			return ExactMapKeys(*projection,{"viewport_scale"},
				"resolved orthographic projection",error) &&
				ArrayElementsAre(*projection,"viewport_scale",RISECBOR64::Value::Float64,
					2u,"resolved orthographic projection",error);
		}
		if( kind->GetText() == "thin_lens" ) {
			if( !ExactMapKeys(*projection,{ "anamorphic_squeeze", "aperture_blades",
				"aperture_rotation", "focal_length_mm", "focus_distance_scene_units",
				"fstop", "iso", "scene_unit_meters", "sensor_size_mm", "shift_x_mm",
				"shift_y_mm", "tilt_x_radians", "tilt_y_radians" },
				"resolved thin-lens projection",error) ) return false;
			for( const char* field : { "anamorphic_squeeze", "aperture_rotation",
				"focal_length_mm", "focus_distance_scene_units", "fstop", "iso",
				"scene_unit_meters", "sensor_size_mm", "shift_x_mm", "shift_y_mm",
				"tilt_x_radians", "tilt_y_radians" } ) {
				if( !FieldType(*projection,field,RISECBOR64::Value::Float64,
					"resolved thin-lens projection",error) ) return false;
			}
			return FieldType(*projection,"aperture_blades",
				RISECBOR64::Value::UnsignedInteger,"resolved thin-lens projection",error);
		}
		error = "resolved camera kind is outside schema-v1";
		return false;
	}

	bool ValidateResolvedConfigSchemaV1(
		const RISECBOR64::Value& record,
		std::string& error )
	{
		if( !ExactMapKeys(record,{ "animation", "aov", "camera", "clamp", "depth",
			"evaluated_camera_states", "execution", "external_runtime", "film",
			"filter", "global_render_options", "integrator", "light_sampling",
			"raster_sequence", "record_kind", "render_region", "sampler",
			"schema_version", "shader", "stability", "transport" },
			"resolved render configuration",error) ||
			!HasRecordHeader(record,"resolved_render_configuration_v1") ) return false;
		const RISECBOR64::Value* animation = record.Find("animation");
		const RISECBOR64::Value* frameSelection = animation ?
			animation->Find("frame_selection") : nullptr;
		if( !animation || !ExactMapKeys(*animation,{ "do_fields", "frame_selection",
			"invert_fields", "num_frames", "time_end", "time_start" },
			"resolved animation",error) || !frameSelection ||
			!ExactMapKeys(*frameSelection,{"active","index"},
				"resolved frame selection",error) ||
			!FieldType(*animation,"do_fields",RISECBOR64::Value::Boolean,
				"resolved animation",error) ||
			!FieldType(*animation,"invert_fields",RISECBOR64::Value::Boolean,
				"resolved animation",error) ||
			!FieldType(*animation,"num_frames",RISECBOR64::Value::UnsignedInteger,
				"resolved animation",error) ||
			!FieldType(*animation,"time_end",RISECBOR64::Value::Float64,
				"resolved animation",error) ||
			!FieldType(*animation,"time_start",RISECBOR64::Value::Float64,
				"resolved animation",error) ||
			!FieldType(*frameSelection,"active",RISECBOR64::Value::Boolean,
				"resolved frame selection",error) ||
			!FieldType(*frameSelection,"index",RISECBOR64::Value::UnsignedInteger,
				"resolved frame selection",error) ) return false;

		const RISECBOR64::Value* aov = record.Find("aov");
		if( !aov || !ExactMapKeys(*aov,{"channels"},"resolved AOV",error) ||
			!ArrayElementsAre(*aov,"channels",RISECBOR64::Value::Text,
				static_cast<std::size_t>(-1),"resolved AOV",error) ||
			!ValidateCameraSchema(*record.Find("camera"),error) ) return false;
		std::set<std::string> aovChannels;
		for( const RISECBOR64::Value& channel : aov->Find("channels")->GetArray() ) {
			const std::string& name = channel.GetText();
			if( (name != "beauty" && name != "alpha" && name != "albedo" &&
				name != "normal" && name != "depth" && name != "object_id" &&
				name != "primitive_id") || !aovChannels.insert(name).second ) {
				error = "resolved AOV channel is duplicated or outside schema-v1";
				return false;
			}
		}

		const auto exactFloatMap = [&error]( const RISECBOR64::Value* value,
			const std::initializer_list<const char*> keys, const char* context ) {
			if( !value || !ExactMapKeys(*value,keys,context,error) ) return false;
			for( const char* key : keys ) {
				if( !FieldType(*value,key,RISECBOR64::Value::Float64,context,error) ) return false;
			}
			return true;
		};
		if( !exactFloatMap(record.Find("clamp"),{"direct","indirect"},"resolved clamp") )
			return false;
		const RISECBOR64::Value* depth = record.Find("depth");
		if( !depth || !ExactMapKeys(*depth,{ "max_diffuse_bounce", "max_eye_depth",
			"max_glossy_bounce", "max_light_depth", "max_recursion",
			"max_translucent_bounce", "max_transmission_bounce", "max_volume_bounce" },
			"resolved depth",error) ) return false;
		for( const auto& member : depth->GetMap() ) {
			if( member.second.GetType() != RISECBOR64::Value::UnsignedInteger ) {
				error = "resolved depth field has the wrong type";
				return false;
			}
		}

		const RISECBOR64::Value* cameraStates = record.Find("evaluated_camera_states");
		if( !cameraStates || cameraStates->GetType() != RISECBOR64::Value::Array ) {
			error = "evaluated camera states are not an array";
			return false;
		}
		for( const RISECBOR64::Value& state : cameraStates->GetArray() ) {
			if( !ExactMapKeys(state,{"camera","field","frame_index","time"},
				"evaluated camera state",error) ||
				!ValidateCameraSchema(*state.Find("camera"),error) ||
				!TextFieldIn(state,"field",{"upper","lower","both"},
					"evaluated camera state",error) ||
				!FieldType(state,"frame_index",RISECBOR64::Value::UnsignedInteger,
					"evaluated camera state",error) ||
				!FieldType(state,"time",RISECBOR64::Value::Float64,
					"evaluated camera state",error) ) return false;
		}

		const RISECBOR64::Value* execution = record.Find("execution");
		if( !execution || !ExactMapKeys(*execution,{ "effective_worker_task_count",
			"force_number_of_threads", "maximum_thread_count", "random_stream_policy",
			"render_thread_reserve_count" },"resolved execution",error) ||
			!FieldType(*execution,"effective_worker_task_count",
				RISECBOR64::Value::UnsignedInteger,"resolved execution",error) ||
			!IntegerField(*execution,"force_number_of_threads","resolved execution",error) ||
			!IntegerField(*execution,"maximum_thread_count","resolved execution",error) ||
			!IntegerField(*execution,"render_thread_reserve_count","resolved execution",error) ||
			!TextFieldIn(*execution,"random_stream_policy",{
				"per_dispatch_task_mersenne53_seeded_from_c_rand",
				"per_dispatch_task_mersenne_seeded_from_c_rand",
				"process_shared_drand48", "process_shared_c_rand" },
				"resolved execution",error) ) return false;

		const RISECBOR64::Value* external = record.Find("external_runtime");
		if( !external ) return false;
		if( external->GetType() != RISECBOR64::Value::Null ) {
			const RISECBOR64::Value* region = external->Find("region");
			if( !ExactMapKeys(*external,{ "camera_override", "clay_override",
				"idle_max_passes", "idle_mode", "indirect_only", "live_samples_per_pass",
				"max_path_depth", "preview_scale", "progressive_on_idle", "region",
				"tile_order", "variant_pipeline", "view_mode",
				"view_mode_caster_installed", "xray" },"resolved external runtime",error) ||
				!region || !ExactMapKeys(*region,{"active","bottom","left","right","top"},
					"resolved external region",error) ) return false;
			for( const char* key : { "camera_override", "clay_override", "idle_mode",
				"indirect_only", "progressive_on_idle", "variant_pipeline",
				"view_mode_caster_installed", "xray" } ) {
				if( !FieldType(*external,key,RISECBOR64::Value::Boolean,
					"resolved external runtime",error) ) return false;
			}
			for( const char* key : { "idle_max_passes", "live_samples_per_pass",
				"max_path_depth", "preview_scale", "tile_order" } ) {
				if( !FieldType(*external,key,RISECBOR64::Value::UnsignedInteger,
					"resolved external runtime",error) ) return false;
			}
			if( !UnsignedFieldAtMost(*external,"tile_order",2u,
				"resolved external runtime",error) ||
				!NonemptyTextField(*external,"view_mode",
				"resolved external runtime",error) ||
				!FieldType(*region,"active",RISECBOR64::Value::Boolean,
					"resolved external region",error) ) return false;
			for( const char* key : {"bottom","left","right","top"} ) {
				if( !FieldType(*region,key,RISECBOR64::Value::UnsignedInteger,
					"resolved external region",error) ) return false;
			}
		}

		const RISECBOR64::Value* film = record.Find("film");
		const RISECBOR64::Value* filter = record.Find("filter");
		if( !film || !filter ||
			!ExactMapKeys(*film,{"height","pixel_aspect_ratio","width"},
			"resolved film",error) ||
			!FieldType(*film,"height",RISECBOR64::Value::UnsignedInteger,
				"resolved film",error) ||
			!FieldType(*film,"width",RISECBOR64::Value::UnsignedInteger,
				"resolved film",error) ||
			!FieldType(*film,"pixel_aspect_ratio",RISECBOR64::Value::Float64,
				"resolved film",error) ||
			!ExactMapKeys(*filter,{"height","name","param_a","param_b","width"},
				"resolved filter",error) ||
			!FieldType(*filter,"name",RISECBOR64::Value::Text,
				"resolved filter",error) ) return false;
		for( const char* key : {"height","param_a","param_b","width"} )
			if( !FieldType(*filter,key,RISECBOR64::Value::Float64,
				"resolved filter",error) ) return false;

		const RISECBOR64::Value* global = record.Find("global_render_options");
		const RISECBOR64::Value* autoProbe = global ? global->Find("auto_probe") : nullptr;
		const RISECBOR64::Value* vcm = global ? global->Find("vcm") : nullptr;
		if( !global || !ExactMapKeys(*global,{"auto_probe","vcm"},
			"resolved global options",error) || !autoProbe || !vcm ||
			!ExactMapKeys(*autoProbe,{ "activation_spp", "reach_winsor_percentile",
				"scale", "spp", "tau_bdpt", "tau_caustic", "tau_reach",
				"variance_renders" },"resolved auto probe",error) ||
			!ExactMapKeys(*vcm,{ "progressive_radius_enabled", "throughput_clamp_multiplier",
				"throughput_clamp_percentile" },"resolved VCM options",error) ) return false;
		for( const char* key : {"activation_spp","scale","spp","variance_renders"} ) {
			if( !FieldType(*autoProbe,key,RISECBOR64::Value::UnsignedInteger,
				"resolved auto probe",error) ) return false;
		}
		for( const char* key : { "reach_winsor_percentile", "tau_bdpt", "tau_caustic",
			"tau_reach" } ) if( !FieldType(*autoProbe,key,RISECBOR64::Value::Float64,
				"resolved auto probe",error) ) return false;
		if( !FieldType(*vcm,"progressive_radius_enabled",RISECBOR64::Value::Boolean,
			"resolved VCM options",error) ||
			!FieldType(*vcm,"throughput_clamp_multiplier",RISECBOR64::Value::Float64,
				"resolved VCM options",error) ||
			!FieldType(*vcm,"throughput_clamp_percentile",RISECBOR64::Value::Float64,
				"resolved VCM options",error) ) return false;

		const RISECBOR64::Value* integrator = record.Find("integrator");
		const RISECBOR64::Value* guiding = integrator ? integrator->Find("path_guiding") : nullptr;
		const RISECBOR64::Value* sms = integrator ? integrator->Find("sms") : nullptr;
		if( !integrator || !ExactMapKeys(*integrator,{ "auto_choice", "auto_probe_enabled",
			"effective_kind", "enable_vertex_connection", "enable_vertex_merging",
			"integrate_rgb", "kind", "merge_radius", "path_guiding", "show_luminaires",
			"sms" },"resolved integrator",error) || !guiding || !sms ||
			!ExactMapKeys(*guiding,{ "alpha", "combine_training_iterations",
				"complete_path_guiding", "complete_path_strategy_samples",
				"complete_path_strategy_selection", "enabled", "learned_alpha",
				"max_guiding_depth", "max_light_guiding_depth", "online", "ris_candidates",
				"sampling_type", "training_iterations", "training_spp", "warmup_iterations" },
				"resolved path guiding",error) ||
			!ExactMapKeys(*sms,{ "bernoulli_trials", "biased", "enabled", "max_chain_depth",
				"max_iterations", "max_photon_seeds_per_shading_point", "multi_trials",
				"photon_count", "seeding_mode", "target_bounces", "threshold", "two_stage",
				"use_levenberg_marquardt" },"resolved SMS",error) ) return false;
		if( !UnsignedFieldAtMost(*integrator,"auto_choice",3u,
			"resolved integrator",error) ) return false;
		for( const char* key : { "auto_probe_enabled", "enable_vertex_connection",
			"enable_vertex_merging", "integrate_rgb", "show_luminaires" } )
			if( !FieldType(*integrator,key,RISECBOR64::Value::Boolean,
				"resolved integrator",error) ) return false;
		if( !TextFieldIn(*integrator,"kind",{
			"interactive_pel_rasterizer", "pixelpel_rasterizer",
			"pixelintegratingspectral_rasterizer", "pathtracing_pel_rasterizer",
			"pathtracing_spectral_rasterizer", "auto_rasterizer",
			"auto_spectral_rasterizer", "bdpt_pel_rasterizer",
			"bdpt_spectral_rasterizer", "vcm_pel_rasterizer",
			"vcm_spectral_rasterizer", "mlt_rasterizer",
			"mlt_spectral_rasterizer" },"resolved integrator",error) ||
			!TextFieldIn(*integrator,"effective_kind",{
			"pt", "bdpt", "vcm", "interactive_pel_rasterizer",
			"pixelpel_rasterizer", "pixelintegratingspectral_rasterizer",
			"pathtracing_pel_rasterizer", "pathtracing_spectral_rasterizer",
			"bdpt_pel_rasterizer", "bdpt_spectral_rasterizer",
			"vcm_pel_rasterizer", "vcm_spectral_rasterizer",
			"mlt_rasterizer", "mlt_spectral_rasterizer" },
			"resolved integrator",error) ) return false;
		if( !FieldType(*integrator,"merge_radius",RISECBOR64::Value::Float64,
			"resolved integrator",error) ) return false;
		const std::string& integratorKind = integrator->Find("kind")->GetText();
		const std::string& effectiveKind = integrator->Find("effective_kind")->GetText();
		const bool autoKind = integratorKind == "auto_rasterizer" ||
			integratorKind == "auto_spectral_rasterizer";
		if( (autoKind && effectiveKind != "pt" && effectiveKind != "bdpt" &&
			effectiveKind != "vcm") || (!autoKind && effectiveKind != integratorKind) ) {
			error = "resolved integrator kind/effective_kind pairing is outside schema-v1";
			return false;
		}
		for( const auto& member : guiding->GetMap() ) {
			const std::set<std::string> floatFields = {"alpha"};
			const std::set<std::string> boolFields = { "combine_training_iterations",
				"complete_path_guiding", "complete_path_strategy_selection", "enabled",
				"learned_alpha", "online" };
			const RISECBOR64::Value::Type expected = floatFields.count(member.first) ?
				RISECBOR64::Value::Float64 : boolFields.count(member.first) ?
				RISECBOR64::Value::Boolean : RISECBOR64::Value::UnsignedInteger;
			if( member.second.GetType() != expected ) {
				error = "resolved path-guiding field has the wrong type";
				return false;
			}
		}
		if( !UnsignedFieldAtMost(*guiding,"sampling_type",1u,
			"resolved path guiding",error) ) return false;
		for( const auto& member : sms->GetMap() ) {
			const std::set<std::string> boolFields = {
				"biased","enabled","two_stage","use_levenberg_marquardt"};
			const RISECBOR64::Value::Type expected = member.first == "threshold" ?
				RISECBOR64::Value::Float64 : boolFields.count(member.first) ?
				RISECBOR64::Value::Boolean : RISECBOR64::Value::UnsignedInteger;
			if( member.second.GetType() != expected ) {
				error = "resolved SMS field has the wrong type";
				return false;
			}
		}
		if( !UnsignedFieldAtMost(*sms,"seeding_mode",1u,
			"resolved SMS",error) ) return false;

		if( !exactFloatMap(record.Find("light_sampling"),{"rr_threshold"},
			"resolved light sampling") ) return false;
		const RISECBOR64::Value* sequence = record.Find("raster_sequence");
		const RISECBOR64::Value* sequenceKind = sequence ? sequence->Find("kind") : nullptr;
		if( !sequence || !sequenceKind || sequenceKind->GetType() != RISECBOR64::Value::Text ) {
			error = "resolved raster sequence kind is unavailable";
			return false;
		}
		const std::string& sequenceName = sequenceKind->GetText();
		if( sequenceName == "morton" ) {
			if( !ExactMapKeys(*sequence,{"kind","tile_size"},"resolved raster sequence",error) ||
				!FieldType(*sequence,"tile_size",RISECBOR64::Value::UnsignedInteger,
					"resolved raster sequence",error) ) return false;
		} else if( sequenceName == "block" ) {
			if( !ExactMapKeys(*sequence,{ "height", "kind", "order", "shuffle_seed",
				"shuffle_seed_active", "width" },"resolved raster sequence",error) ) return false;
			for( const char* key : {"height","order","shuffle_seed","width"} )
				if( !FieldType(*sequence,key,RISECBOR64::Value::UnsignedInteger,
					"resolved raster sequence",error) ) return false;
			if( !FieldType(*sequence,"shuffle_seed_active",RISECBOR64::Value::Boolean,
				"resolved raster sequence",error) ) return false;
			if( !UnsignedFieldAtMost(*sequence,"order",8u,
				"resolved raster sequence",error) ||
				!UnsignedFieldAtMost(*sequence,"shuffle_seed",0xffffffffu,
					"resolved raster sequence",error) ) return false;
			const std::uint64_t order = sequence->Find("order")->GetIntegerArgument();
			const std::uint64_t shuffleSeed =
				sequence->Find("shuffle_seed")->GetIntegerArgument();
			const bool shuffleSeedActive =
				sequence->Find("shuffle_seed_active")->GetBoolean();
			if( shuffleSeedActive != (order == 1u) ||
				(!shuffleSeedActive && shuffleSeed != 0u) ) {
				error = "resolved block raster sequence has inconsistent shuffle state";
				return false;
			}
		} else if( sequenceName == "hilbert" ) {
			if( !ExactMapKeys(*sequence,{"depth","kind"},"resolved raster sequence",error) ||
				!FieldType(*sequence,"depth",RISECBOR64::Value::UnsignedInteger,
					"resolved raster sequence",error) ) return false;
		} else if( sequenceName == "scanline" || sequenceName == "rasterizer_default" ) {
			if( !ExactMapKeys(*sequence,{"kind"},"resolved raster sequence",error) ) return false;
		} else {
			error = "resolved raster sequence kind is outside schema-v1";
			return false;
		}

		const RISECBOR64::Value* region = record.Find("render_region");
		if( !region || !ExactMapKeys(*region,{"active","bottom","left","right","top"},
			"resolved render region",error) ||
			!FieldType(*region,"active",RISECBOR64::Value::Boolean,
				"resolved render region",error) ) return false;
		for( const char* key : {"bottom","left","right","top"} )
			if( !FieldType(*region,key,RISECBOR64::Value::UnsignedInteger,
				"resolved render region",error) ) return false;

		const RISECBOR64::Value* sampler = record.Find("sampler");
		const RISECBOR64::Value* adaptive = sampler ? sampler->Find("adaptive") : nullptr;
		const RISECBOR64::Value* progressive = sampler ? sampler->Find("progressive") : nullptr;
		const RISECBOR64::Value* spectral = sampler ? sampler->Find("spectral") : nullptr;
		if( !sampler || !ExactMapKeys(*sampler,{ "adaptive", "blue_noise",
			"large_step_probability", "luminary_sampler", "luminary_sampler_param",
			"mlt_bootstrap_samples", "mlt_chains", "mlt_mutations_per_pixel",
			"num_luminary_samples", "pixel_sampler", "pixel_sampler_param", "pixel_samples",
			"progressive", "spectral" },"resolved sampler",error) ||
			!adaptive || !progressive || !spectral ||
			!ExactMapKeys(*adaptive,{"max_samples","show_map","threshold"},
				"resolved adaptive sampler",error) ||
			!ExactMapKeys(*progressive,{"enabled","samples_per_pass"},
				"resolved progressive sampler",error) ||
			!ExactMapKeys(*spectral,{ "hwss", "nm_begin", "nm_end", "num_wavelengths",
				"spectral_samples" },"resolved spectral sampler",error) ) return false;
		for( const char* key : {"blue_noise"} ) if( !FieldType(*sampler,key,
			RISECBOR64::Value::Boolean,"resolved sampler",error) ) return false;
		for( const char* key : {"large_step_probability","luminary_sampler_param",
			"pixel_sampler_param"} ) if( !FieldType(*sampler,key,RISECBOR64::Value::Float64,
				"resolved sampler",error) ) return false;
		for( const char* key : {"luminary_sampler","pixel_sampler"} )
			if( !FieldType(*sampler,key,RISECBOR64::Value::Text,
				"resolved sampler",error) ) return false;
		for( const char* key : { "mlt_bootstrap_samples", "mlt_chains",
			"mlt_mutations_per_pixel", "num_luminary_samples", "pixel_samples" } )
			if( !FieldType(*sampler,key,RISECBOR64::Value::UnsignedInteger,
				"resolved sampler",error) ) return false;
		if( !FieldType(*adaptive,"max_samples",RISECBOR64::Value::UnsignedInteger,
			"resolved adaptive sampler",error) ||
			!FieldType(*adaptive,"show_map",RISECBOR64::Value::Boolean,
				"resolved adaptive sampler",error) ||
			!FieldType(*adaptive,"threshold",RISECBOR64::Value::Float64,
				"resolved adaptive sampler",error) ||
			!FieldType(*progressive,"enabled",RISECBOR64::Value::Boolean,
				"resolved progressive sampler",error) ||
			!FieldType(*progressive,"samples_per_pass",RISECBOR64::Value::UnsignedInteger,
				"resolved progressive sampler",error) ||
			!FieldType(*spectral,"hwss",RISECBOR64::Value::Boolean,
				"resolved spectral sampler",error) ||
			!FieldType(*spectral,"nm_begin",RISECBOR64::Value::Float64,
				"resolved spectral sampler",error) ||
			!FieldType(*spectral,"nm_end",RISECBOR64::Value::Float64,
				"resolved spectral sampler",error) ||
			!FieldType(*spectral,"num_wavelengths",RISECBOR64::Value::UnsignedInteger,
				"resolved spectral sampler",error) ||
			!FieldType(*spectral,"spectral_samples",RISECBOR64::Value::UnsignedInteger,
				"resolved spectral sampler",error) ) return false;

		if( !FieldType(record,"shader",RISECBOR64::Value::Text,
			"resolved render configuration",error) ) return false;
		const RISECBOR64::Value* stability = record.Find("stability");
		if( !stability || !ExactMapKeys(*stability,{ "filter_glossy", "optimal_mis",
			"optimal_mis_tile_size", "optimal_mis_training_iterations", "rr_min_depth",
			"rr_threshold", "transparent_shadows", "use_light_bvh" },
			"resolved stability",error) ||
			!FieldType(*stability,"filter_glossy",RISECBOR64::Value::Float64,
				"resolved stability",error) ||
			!FieldType(*stability,"rr_threshold",RISECBOR64::Value::Float64,
				"resolved stability",error) ) return false;
		for( const char* key : {"optimal_mis","transparent_shadows","use_light_bvh"} )
			if( !FieldType(*stability,key,RISECBOR64::Value::Boolean,
				"resolved stability",error) ) return false;
		for( const char* key : { "optimal_mis_tile_size",
			"optimal_mis_training_iterations", "rr_min_depth" } )
			if( !FieldType(*stability,key,RISECBOR64::Value::UnsignedInteger,
				"resolved stability",error) ) return false;

		const RISECBOR64::Value* transport = record.Find("transport");
		const RISECBOR64::Value* radiance = transport ? transport->Find("radiance_map") : nullptr;
		if( !transport || !ExactMapKeys(*transport,{ "oidn", "oidn_device",
			"oidn_prefilter", "oidn_quality", "radiance_map" },"resolved transport",error) ||
			!radiance || !ExactMapKeys(*radiance,{"background","name","orientation","scale"},
				"resolved radiance map",error) ||
			!FieldType(*transport,"oidn",RISECBOR64::Value::Boolean,
				"resolved transport",error) ||
			!FieldType(*transport,"oidn_device",RISECBOR64::Value::UnsignedInteger,
				"resolved transport",error) ||
			!FieldType(*transport,"oidn_prefilter",RISECBOR64::Value::UnsignedInteger,
				"resolved transport",error) ||
			!FieldType(*transport,"oidn_quality",RISECBOR64::Value::UnsignedInteger,
				"resolved transport",error) ||
			!FieldType(*radiance,"background",RISECBOR64::Value::Boolean,
				"resolved radiance map",error) ||
			!FieldType(*radiance,"name",RISECBOR64::Value::Text,
				"resolved radiance map",error) ||
			!ArrayElementsAre(*radiance,"orientation",RISECBOR64::Value::Float64,3u,
				"resolved radiance map",error) ||
			!FieldType(*radiance,"scale",RISECBOR64::Value::Float64,
				"resolved radiance map",error) ) return false;
		if( !UnsignedFieldAtMost(*transport,"oidn_device",2u,
			"resolved transport",error) ||
			!UnsignedFieldAtMost(*transport,"oidn_prefilter",1u,
				"resolved transport",error) ||
			!UnsignedFieldAtMost(*transport,"oidn_quality",3u,
				"resolved transport",error) ) return false;
		return true;
	}

	bool ValidateRendererBuildSchemaV1(
		const RISECBOR64::Value& record,
		std::string& error )
	{
		if( !ExactMapKeys(record,{ "compiler", "dependency_builds", "dirty_state",
			"fp_settings", "gate_harness_version", "record_kind", "renderer_binary",
			"renderer_version", "schema_version", "solver_schema_versions",
			"source_revision", "target" },"renderer build identity",error) ||
			!HasRecordHeader(record,"renderer_build_v1") ) return false;
		const RISECBOR64::Value* compiler = record.Find("compiler");
		const RISECBOR64::Value* dirty = record.Find("dirty_state");
		const RISECBOR64::Value* binary = record.Find("renderer_binary");
		const RISECBOR64::Value* fp = record.Find("fp_settings");
		const RISECBOR64::Value* target = record.Find("target");
		if( !compiler || !dirty || !binary || !fp || !target ||
			!ExactMapKeys(*compiler,{"identity","language_standard","lto_mode",
				"optimization_mode"},"renderer compiler",error) ||
			!ExactMapKeys(*dirty,{"diff_sha256","state"},"renderer dirty state",error) ||
			!ExactMapKeys(*binary,{"hash_basis","kind","path","sha256"},
				"renderer binary",error) ||
			!ExactMapKeys(*fp,{"contraction_mode","fast_math","finite_math_only"},
				"renderer FP settings",error) ||
			!ExactMapKeys(*target,{"architecture","platform"},"renderer target",error) )
			return false;
		if( !NonemptyTextField(*compiler,"identity","renderer compiler",error) ||
			!TextFieldIn(*compiler,"language_standard",{"c++17"},
				"renderer compiler",error) ||
			!TextFieldIn(*compiler,"lto_mode",{"off","thin","full","compiler_default"},
				"renderer compiler",error) ||
			!TextFieldIn(*compiler,"optimization_mode",{
				"disabled","O1","O2","O3","Os","Oz","compiler_default"},
				"renderer compiler",error) ) return false;
		for( const char* key : {"diff_sha256","state"} ) if( !FieldType(*dirty,key,
			RISECBOR64::Value::Text,"renderer dirty state",error) ) return false;
		if( !TextFieldIn(*binary,"hash_basis",{
			"file_bytes","dyld_shared_cache_and_image_uuids","apk_stored_entry_bytes"},
			"renderer binary",error) ||
			!TextFieldIn(*binary,"kind",{"executable","module"},
				"renderer binary",error) ||
			!NonemptyTextField(*binary,"path","renderer binary",error) ||
			!NonemptyTextField(*binary,"sha256","renderer binary",error) ) return false;
		const std::string& dirtyState = dirty->Find("state")->GetText();
		if( (dirtyState != "clean" && dirtyState != "dirty") ||
			!IsSHA256Hex(dirty->Find("diff_sha256")->GetText()) ||
			!IsSHA256Hex(binary->Find("sha256")->GetText()) ||
			!TextFieldIn(*fp,"contraction_mode",{
				"off","on","fast","compiler_default"},
				"renderer FP settings",error) ||
			!FieldType(*fp,"fast_math",RISECBOR64::Value::Boolean,
				"renderer FP settings",error) ||
			!FieldType(*fp,"finite_math_only",RISECBOR64::Value::Boolean,
				"renderer FP settings",error) ) return false;
		if( !TextFieldIn(*target,"architecture",{"arm64","x86_64"},
			"renderer target",error) ||
			!TextFieldIn(*target,"platform",{"windows","android","macos","linux"},
				"renderer target",error) ||
			!TextFieldIn(record,"gate_harness_version",{"phase_a_gate_harness_v1"},
				"renderer build identity",error) ) return false;
		for( const char* key : { "renderer_version", "source_revision" } )
			if( !NonemptyTextField(record,key,"renderer build identity",error) ) return false;
		if( !ArrayElementsAre(record,"solver_schema_versions",RISECBOR64::Value::Text,
			static_cast<std::size_t>(-1),"renderer build identity",error) ) return false;
		const RISECBOR64::Value* solverVersions = record.Find("solver_schema_versions");
		if( solverVersions->GetArray().size() != 2u ||
			solverVersions->GetArray()[0].GetText() != "fire_optics_schema_v3" ||
			solverVersions->GetArray()[1].GetText() != "fire_output_provenance_schema_v1" ) {
			error = "renderer solver schema versions are outside schema-v1";
			return false;
		}
		const RISECBOR64::Value* dependencies = record.Find("dependency_builds");
		if( !dependencies || !ExactMapKeys(*dependencies,{ "avcodec", "avfoundation",
			"avformat", "avutil", "iex", "ilmthread", "imath", "oidn", "openexr",
			"openpgl", "png", "swscale", "tiff", "videotoolbox", "x265", "zlib" },
			"renderer dependency builds",error) ) {
			error = "renderer dependency builds are unavailable";
			return false;
		}
		for( const auto& dependency : dependencies->GetMap() ) {
			if( !ExactMapKeys(dependency.second,{"availability","linkage",
				"loaded_binaries","version"},"renderer dependency",error) ||
				!FieldType(dependency.second,"availability",RISECBOR64::Value::Text,
					"renderer dependency",error) ||
				!FieldType(dependency.second,"linkage",RISECBOR64::Value::Text,
					"renderer dependency",error) ||
				!FieldType(dependency.second,"version",RISECBOR64::Value::Text,
					"renderer dependency",error) ) return false;
			const RISECBOR64::Value* binaries = dependency.second.Find("loaded_binaries");
			if( !binaries || binaries->GetType() != RISECBOR64::Value::Array ) {
				error = "renderer dependency binaries are not an array";
				return false;
			}
			const std::string& availability = dependency.second.Find("availability")->GetText();
			const std::string& linkage = dependency.second.Find("linkage")->GetText();
			const std::string& version = dependency.second.Find("version")->GetText();
			const bool dependencyStateValid =
				(availability == "not_linked" && linkage == "not_linked" &&
					binaries->GetArray().empty() && version == "not_linked") ||
				(availability == "linked" && linkage == "embedded" &&
					binaries->GetArray().empty() && !version.empty()) ||
				(availability == "linked" && linkage == "dynamic" &&
					!binaries->GetArray().empty() && !version.empty()) ||
				(availability == "not_loaded" && linkage == "runtime_optional" &&
					binaries->GetArray().empty() && version == "not_loaded") ||
				(availability == "loaded" && linkage == "runtime_loaded" &&
					!binaries->GetArray().empty() && !version.empty());
			if( !dependencyStateValid ) {
				error = "renderer dependency availability and linkage are inconsistent";
				return false;
			}
			std::string previousBinaryPath;
			for( const RISECBOR64::Value& loaded : binaries->GetArray() ) {
				if( !ExactMapKeys(loaded,{"hash_basis","path","sha256"},
					"renderer dependency binary",error) ) return false;
				for( const char* key : {"hash_basis","path","sha256"} )
					if( !FieldType(loaded,key,RISECBOR64::Value::Text,
						"renderer dependency binary",error) ) return false;
				if( !IsSHA256Hex(loaded.Find("sha256")->GetText()) ) {
					error = "renderer dependency binary SHA-256 is malformed";
					return false;
				}
				const std::string& hashBasis = loaded.Find("hash_basis")->GetText();
				if( (hashBasis != "file_bytes" &&
					hashBasis != "dyld_shared_cache_and_image_uuids" &&
					hashBasis != "apk_stored_entry_bytes") ||
					loaded.Find("path")->GetText().empty() ) {
					error = "renderer dependency binary identity is incomplete";
					return false;
				}
				if( !previousBinaryPath.empty() &&
					loaded.Find("path")->GetText() <= previousBinaryPath ) {
					error = "renderer dependency binaries are duplicated or unsorted";
					return false;
				}
				previousBinaryPath = loaded.Find("path")->GetText();
			}
			if( linkage == "runtime_loaded" ) {
				const std::string prefix = "runtime_binaries_v1:";
				if( version.compare(0u,prefix.size(),prefix) != 0 ) {
					error = "renderer runtime dependency version is outside schema-v1";
					return false;
				}
				std::size_t cursor = prefix.size();
				for( std::size_t i=0u; i<binaries->GetArray().size(); ++i ) {
					const RISECBOR64::Value& loaded = binaries->GetArray()[i];
					const std::string& path = loaded.Find("path")->GetText();
					const std::size_t slash = path.find_last_of("/\\");
					const std::string tokenPrefix = path.substr(
						slash == std::string::npos ? 0u : slash+1u)+"@version=";
					if( version.compare(cursor,tokenPrefix.size(),tokenPrefix) != 0 ) {
						error = "renderer runtime dependency version does not name its binary";
						return false;
					}
					cursor += tokenPrefix.size();
					const std::string hashSuffix = "@sha256="+
						loaded.Find("sha256")->GetText();
					const std::size_t hashPosition = version.find(hashSuffix,cursor);
					if( hashPosition == std::string::npos || hashPosition == cursor ) {
						error = "renderer runtime dependency version is missing its binary version/hash";
						return false;
					}
					cursor = hashPosition+hashSuffix.size();
					if( i+1u < binaries->GetArray().size() ) {
						if( cursor >= version.size() || version[cursor] != ',' ) {
							error = "renderer runtime dependency version list is malformed";
							return false;
						}
						++cursor;
					}
				}
				if( cursor != version.size() ) {
					error = "renderer runtime dependency version has unmatched data";
					return false;
				}
			}
		}
		return true;
	}
}

bool RISE::FrameStoreOutput::IsAllowedFireRenderReasonCode(
	const std::string& reason )
{
	static const char* const allowed[] = {
		"requested_preview", "producer_unqualified", "heuristic_source",
		"qualified_record_override", "missing_optical_record", "missing_chem_record",
		"chem_none_unqualified", "missing_condensable_record", "missing_gas_opacity_record",
		"missing_thermochemistry_record", "missing_aerosol_thermochemistry_record",
		"missing_transport_record", "table_domain_exceeded", "missing_channel",
		"loading_exceeded", "wet_aerosol_unsupported", "pel_transport",
		"hwss_transport", "pel_blur_ignored", "blur_halo_insufficient",
		"blur_time_support_out_of_range", "nonadvected_source_blur_unsupported",
		"keyframed_temporal_sampling_unsupported", "programmatic_scene_unqualified",
		"unrepresented_scene_mutation", "untracked_scene_mutability", "oidn_unqualified",
		"radiance_clamp_enabled", "path_regularization_enabled", "sms_unqualified",
		"continuation_closure_unsupported", "sss_volume_nee_unsupported", "gate_failure",
		"output_provenance_unavailable", "condensed_organics_ir_unclosed",
		"unsupported_integrator_for_fire_media"
	};
	for( const char* value : allowed ) {
		if( reason == value ) return true;
	}
	return false;
}

bool RISE::FrameStoreOutput::ValidateFireOutputMetadata(
	const Metadata& metadata,
	std::string& error )
{
	error.clear();
	if( metadata.renderFidelityStatus != "preview" ) {
		error = "fire render fidelity status is not preview";
		return false;
	}
	if( metadata.renderReasonCodes.empty() ||
		!IsStrictlySorted(metadata.renderReasonCodes) ) {
		error = "fire render reason codes are empty, duplicated, or unsorted";
		return false;
	}
	for( const std::string& reason : metadata.renderReasonCodes ) {
		if( !IsAllowedFireRenderReasonCode(reason) ) {
			error = "fire render reason code is outside the fixed enum: "+reason;
			return false;
		}
	}
	if( std::find(metadata.renderReasonCodes.begin(),metadata.renderReasonCodes.end(),
		"producer_unqualified") == metadata.renderReasonCodes.end() ) {
		error = "static fire media require the producer_unqualified render reason";
		return false;
	}
	if( metadata.activeFireOpticsRecordIds.empty() ||
		!IsStrictlySorted(metadata.activeFireOpticsRecordIds) ) {
		error = "active fire optical record IDs are empty, duplicated, or unsorted";
		return false;
	}
	for( const std::string& id : metadata.activeFireOpticsRecordIds ) {
		if( !IsSHA256Hex(id) ) {
			error = "active fire optical record ID is not lowercase SHA-256";
			return false;
		}
	}
	if( metadata.activeFireMedia.empty() ) {
		error = "active fire media are empty";
		return false;
	}
	std::set<std::string> mediumRecordIds;
	std::map<std::string,std::string> authoredDigestOwners;
	std::string previousBinding;
	for( const ActiveFireMedium& medium : metadata.activeFireMedia ) {
		if( medium.mediaKind != "static_authored" || medium.managerName.empty() ||
			(medium.bindingKind != "global_medium" &&
			 medium.bindingKind != "object_interior_medium") ||
			medium.bindingOwner.empty() || !IsSHA256Hex(medium.authoredConfigDigest) ||
			medium.opticalRecordIds.empty() || !IsStrictlySorted(medium.opticalRecordIds) ) {
			error = "active fire medium is not a complete static_authored tagged variant";
			return false;
		}
		const std::string binding = medium.managerName+'\0'+medium.bindingKind+'\0'+
			medium.bindingOwner;
		if( !previousBinding.empty() && binding <= previousBinding ) {
			error = "active fire media binding keys are duplicated or unsorted";
			return false;
		}
		previousBinding = binding;
		const auto digestOwner = authoredDigestOwners.find(medium.authoredConfigDigest);
		if( digestOwner != authoredDigestOwners.end() &&
			digestOwner->second != medium.managerName ) {
			error = "different authored fire media share one authored_config_digest";
			return false;
		}
		authoredDigestOwners[medium.authoredConfigDigest] = medium.managerName;
		for( const std::string& id : medium.opticalRecordIds ) {
			if( !IsSHA256Hex(id) ) {
				error = "active fire medium optical record ID is not lowercase SHA-256";
				return false;
			}
			mediumRecordIds.insert(id);
		}
	}
	if( std::vector<std::string>(mediumRecordIds.begin(),mediumRecordIds.end()) !=
		metadata.activeFireOpticsRecordIds ) {
		error = "active fire media optical IDs do not equal the aggregate record IDs";
		return false;
	}
	RISECBOR64::Value resolvedConfig;
	if( metadata.resolvedRenderConfigCoreV1.empty() ||
		!RISECBOR64::DecodeCanonical(metadata.resolvedRenderConfigCoreV1,
			resolvedConfig,&error) || resolvedConfig.GetType() != RISECBOR64::Value::Map ||
		!ValidateResolvedConfigSchemaV1(resolvedConfig,error) ||
		resolvedConfig.Find("output") ) {
		error = "resolved render configuration core is unavailable or outside schema-v1";
		return false;
	}
	RISECBOR64::Value rendererBuild;
	if( metadata.rendererBuildV1.empty() ||
		!RISECBOR64::DecodeCanonical(metadata.rendererBuildV1,rendererBuild,&error) ||
		rendererBuild.GetType() != RISECBOR64::Value::Map ||
		!ValidateRendererBuildSchemaV1(rendererBuild,error) ||
		!IsSHA256Hex(metadata.rendererBuildId) ||
		metadata.rendererBuildId != RISECBOR64::SHA256Hex(metadata.rendererBuildV1) ) {
		error = "renderer build identity is unavailable or outside schema-v1";
		return false;
	}
	const bool anyPrimary = !metadata.primaryProvenanceId.empty() ||
		!metadata.primaryArtifactSha256.empty() ||
		!metadata.primaryArtifactFidelity.empty();
	if( anyPrimary && (!IsSHA256Hex(metadata.primaryProvenanceId) ||
		!IsSHA256Hex(metadata.primaryArtifactSha256) ||
		metadata.primaryArtifactFidelity != "preview_primary") ) {
		error = "retained fire primary linkage is incomplete or inconsistent";
		return false;
	}
	return true;
}

namespace RISE
{
	namespace Implementation
	{

		// ─────────────────────────────────────────────────────────────
		// BeautyRasterImageView — IRasterImage shim onto the Beauty
		// channel.  Defined here (above the FrameStore constructor)
		// so the constructor's eager `new BeautyRasterImageView(*this)`
		// sees the complete type.  Class is private and nested inside
		// FrameStore, so its declaration in the header is just a
		// forward-decl; this is the actual class body.
		// ─────────────────────────────────────────────────────────────
		class FrameStore::BeautyRasterImageView : public virtual IRasterImage,
		                                          public virtual Reference
		{
		public:
			explicit BeautyRasterImageView( FrameStore& parent )
				: parent_( parent )
			{
			}

			// Forward addref/release to the parent FrameStore so
			// any caller that addrefs the view (e.g. legacy code
			// that stores an IRasterImage* with refcount-aware
			// lifetime) effectively keeps the parent alive.  The
			// view is non-independently-owned: its lifetime is
			// tied to the FrameStore's unique_ptr<beautyView_>.
			// Without this forwarding, an external addref creates
			// a refcount-vs-unique_ptr-ownership mismatch — the
			// FrameStore destructs (via its own release reaching 0)
			// and unique_ptr deletes the view, but the external
			// holder has a dangling IRasterImage*.  Forwarding
			// makes external addref → parent stays alive → view
			// stays alive.  See L1 adversarial review HIGH-5.
			void addref() const override { parent_.addref(); }
			bool release() const override { return parent_.release(); }
			unsigned int refcount() const override { return parent_.refcount(); }

			RISEColor GetPEL( const unsigned int x, const unsigned int y ) const override
			{
				if ( !parent_.beauty_ ) return RISEColor();
				const RISEPel pel = parent_.beauty_->At( x, y );
				const Chel    a   = parent_.alpha_ ? parent_.alpha_->At( x, y ) : Chel( 1 );
				return RISEColor( pel, a );
			}

			void SetPEL( const unsigned int x, const unsigned int y,
			             const RISEColor& p ) override
			{
				if ( parent_.beauty_ ) parent_.beauty_->At( x, y ) = p.base;
				if ( parent_.alpha_  ) parent_.alpha_->At( x, y ) = p.a;
			}

			void Clear( const RISEColor& c, const Rect* rc ) override
			{
				if ( !parent_.beauty_ ) return;
				// L6c-1 P0 fix — bounds semantics must match
				// `RISERasterImage::Clear` (RasterImage.h:156-174)
				// which iterates `<= rc->bottom / right` (INCLUSIVE).
				// The pre-fix view used exclusive `<` bounds, which
				// missed the last row + column of every sub-rect
				// clear.  The bug was masked while no production
				// code path called Clear-with-rc on the view, but
				// L6c routes the rasterizer's
				// `PrepareImageForNewRender` through the view, and
				// that path passes a non-null pRect for animation
				// frames (PixelBasedRasterizerHelper.cpp:1396).  Per
				// the canonical Rect-bounds reference at
				// ViewportFrameStore.cpp:374-393, RISE's `Rect` is
				// inclusive at the IRasterImage boundary; FrameStore-
				// internal code converts to half-open at its own
				// boundary.  Mirror RasterImage's `<=`:
				const unsigned int x0 = rc ? rc->left
				                            : 0;
				const unsigned int y0 = rc ? rc->top
				                            : 0;
				const unsigned int xEnd = rc
				    ? rc->right + 1u   // inclusive → exclusive end
				    : static_cast<unsigned int>( parent_.width_ );
				const unsigned int yEnd = rc
				    ? rc->bottom + 1u  // inclusive → exclusive end
				    : static_cast<unsigned int>( parent_.height_ );
				const unsigned int xClamp = std::min( xEnd,
				    static_cast<unsigned int>( parent_.width_ ) );
				const unsigned int yClamp = std::min( yEnd,
				    static_cast<unsigned int>( parent_.height_ ) );
				for ( unsigned int y = y0; y < yClamp; ++y ) {
					for ( unsigned int x = x0; x < xClamp; ++x ) {
						parent_.beauty_->At( x, y ) = c.base;
						if ( parent_.alpha_ ) parent_.alpha_->At( x, y ) = c.a;
					}
				}
			}

			void DumpImage( IRasterImageWriter* pWriter ) const override
			{
				// Drives the writer with the same row-major pixel
				// walk pattern as RasterImage_Template::DumpImage
				// (RasterImage.h, "DumpImage" body).  Same order +
				// same per-pixel WriteColor input → byte-identical
				// output to the legacy
				// FileRasterizerOutput::WriteImageToFile pipeline,
				// which is the L2 IFrameEncoder regression gate.
				//
				// Alpha is read at full Chel (double) precision —
				// matches the legacy Color_Template<RISEPel>::a
				// type.  See L2 adversarial review HIGH-1.
				//
				// Concurrency: this method ACQUIRES every per-tile
				// shared_mutex for the duration of the dump, so
				// concurrent rasterizer-thread writes via
				// BeginTile/EndTile block until the dump completes.
				// This makes encoders (L2 IFrameEncoder, L3
				// FileEncoderObserver, L4 ViewportFrameStore::SaveAs)
				// safe to call mid-render — exactly the use case
				// the GUI's "Save As" menu needs.  The cost is a
				// brief rasterizer pause for the encode duration;
				// per-tile shared-lock acquisition itself is ~50 ns
				// per tile (cf. L4 adversarial review HIGH-1).
				//
				// We must hold ALL tile locks for the full row-major
				// walk (not lock-per-tile) because the WriteColor
				// sequence must remain row-major to preserve byte
				// identity — encoders that maintain stateful
				// compression (PNG deflate, EXR PIZ) produce
				// different bytes if the order changes.
				if ( !pWriter || !parent_.beauty_ ) return;

				const unsigned int w = static_cast<unsigned int>( parent_.width_ );
				const unsigned int h = static_cast<unsigned int>( parent_.height_ );

				// Acquire all per-tile shared_locks before the walk.
				// std::shared_lock construction blocks if a writer
				// holds the corresponding exclusive lock; once the
				// writer releases (EndTile), our acquire proceeds.
				// All locks are released on scope exit at the end
				// of this method.
				std::vector<std::shared_lock<std::shared_mutex>> tileLocks;
				const size_t tileCount =
					parent_.tileCountX_ * parent_.tileCountY_;
				tileLocks.reserve( tileCount );
				for ( size_t ty = 0; ty < parent_.tileCountY_; ++ty ) {
					for ( size_t tx = 0; tx < parent_.tileCountX_; ++tx ) {
						tileLocks.emplace_back(
							parent_.TileLockAt( tx, ty ).mtx );
					}
				}

				pWriter->BeginWrite( w, h );
				for ( unsigned int j = 0; j < h; ++j ) {
					for ( unsigned int i = 0; i < w; ++i ) {
						const RISEPel pel = parent_.beauty_->At( i, j );
						const Chel    a   = parent_.alpha_
						                    ? parent_.alpha_->At( i, j )
						                    : Chel( 1 );
						pWriter->WriteColor(
							RISEColor( pel, a ),
							i, j );
					}
				}
				pWriter->EndWrite();
			}

			void LoadImage( IRasterImageReader* /*pReader*/ ) override
			{
				assert( false && "FrameStore beauty view cannot LoadImage" );
			}

			unsigned int GetWidth()  const override
			{
				return static_cast<unsigned int>( parent_.width_ );
			}
			unsigned int GetHeight() const override
			{
				return static_cast<unsigned int>( parent_.height_ );
			}

		public:
			// Destructor is public (rather than protected per the
			// Reference convention) so the FrameStore-owning
			// std::unique_ptr can invoke it.  This view never
			// participates in IReference-style addref/release sharing
			// directly — addref/release forward to the parent — so
			// the protected-dtor "free via release()" rule doesn't
			// apply.  See L1 adversarial review HIGH-5.
			virtual ~BeautyRasterImageView() {}

		private:
			FrameStore& parent_;
		};

		// ─────────────────────────────────────────────────────────────
		// Construction / destruction
		// ─────────────────────────────────────────────────────────────

		FrameStore::FrameStore( const Spec& spec )
			: width_( spec.width )
			, height_( spec.height )
			, tileEdge_( spec.tileEdge > 0 ? spec.tileEdge : 32 )
			, tileCountX_( ( spec.width  + ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) - 1 )
			               / ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) )
			, tileCountY_( ( spec.height + ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) - 1 )
			               / ( spec.tileEdge > 0 ? spec.tileEdge : 32 ) )
			, presence_( static_cast<size_t>( ChannelId::COUNT ), false )
			, completedFrame_( spec.meta.frame )
			, meta_( spec.meta )
		{
			// Beauty + Alpha are always allocated.  Beauty holds the
			// RISEPel radiance; Alpha is a separate float channel.
			// Keeping Alpha as its own typed channel rather than a
			// 4th component of Beauty matches the rest of the
			// codebase (RISEColor splits .base / .a) and lets future
			// rasterizers populate Alpha independently.
			if ( width_ > 0 && height_ > 0 ) {
				beauty_ = std::make_unique<Channel<RISEPel>>( width_, height_ );
				alpha_  = std::make_unique<Channel<Chel>>(    width_, height_ );

				// Initial state: black + opaque (alpha=1).  Matches
				// the rasterizer's "begin from clear" expectation.
				beauty_->Fill( RISEPel( 0.0, 0.0, 0.0 ) );
				alpha_->Fill( Chel( 1 ) );

				presence_[ static_cast<uint32_t>( ChannelId::Beauty ) ] = true;
				presence_[ static_cast<uint32_t>( ChannelId::Alpha  ) ] = true;
			}

			// Optional AOV channels.  Each switch case sets its own
			// presence_[id] = true AFTER successful allocation, so a
			// future ChannelId added to the enum but not handled here
			// will fail the assert (debug) and leave presence_ at
			// false (release) — the safer behaviour than the previous
			// "set true unconditionally outside the switch" pattern,
			// which would tell HasChannel(NewId) "yes, exists" while
			// no storage was allocated.  See L1 adversarial review HIGH-6.
			for ( ChannelId id : spec.aovChannels ) {
				if ( id == ChannelId::Beauty || id == ChannelId::Alpha ) {
					// Already allocated; allow listing without error
					// for clients that explicitly enumerate.
					continue;
				}
				if ( width_ == 0 || height_ == 0 ) continue;

				switch ( id ) {
					case ChannelId::Albedo:
						albedo_ = std::make_unique<Channel<RISEPel>>( width_, height_ );
						albedo_->Fill( RISEPel( 0.0, 0.0, 0.0 ) );
						presence_[ static_cast<uint32_t>( ChannelId::Albedo ) ] = true;
						break;
					case ChannelId::Normal:
						normal_ = std::make_unique<Channel<Vector3>>( width_, height_ );
						normal_->Fill( Vector3( 0.0, 0.0, 0.0 ) );
						presence_[ static_cast<uint32_t>( ChannelId::Normal ) ] = true;
						break;
					case ChannelId::Depth:
						depth_ = std::make_unique<Channel<float>>( width_, height_ );
						depth_->Fill( 0.0f );
						presence_[ static_cast<uint32_t>( ChannelId::Depth ) ] = true;
						break;
					case ChannelId::ObjectId:
						objectId_ = std::make_unique<Channel<uint32_t>>( width_, height_ );
						objectId_->Fill( 0u );
						presence_[ static_cast<uint32_t>( ChannelId::ObjectId ) ] = true;
						break;
					case ChannelId::PrimitiveId:
						primitiveId_ = std::make_unique<Channel<uint32_t>>( width_, height_ );
						primitiveId_->Fill( 0u );
						presence_[ static_cast<uint32_t>( ChannelId::PrimitiveId ) ] = true;
						break;
					default:
						// Unhandled enum value — assert in debug, ignore in
						// release.  presence_ remains false so HasChannel
						// reports the truth: storage was not allocated.
						assert( false && "Unhandled AOV ChannelId in FrameStore::Spec — "
						                 "did you add an enum value without wiring "
						                 "it through the constructor switch?" );
						break;
				}
			}

			// Per-tile reader/writer locks.  std::shared_mutex is
			// non-movable so std::vector can't host TileLock
			// directly; heap-array via unique_ptr<T[]>.
			const size_t tileCount = tileCountX_ * tileCountY_;
			if ( tileCount > 0 ) {
				tileLocks_ = std::make_unique<TileLock[]>( tileCount );
			}

			// Pre-construct the IRasterImage shim view so
			// AsBeautyRasterImage() is a pure read with no
			// const_cast / lazy-init race.  See L1 adversarial
			// review LOW-6.
			beautyView_ = std::unique_ptr<BeautyRasterImageView>(
				new BeautyRasterImageView( *this ) );
		}

		FrameStore::~FrameStore()
		{
			// unique_ptr<Channel<T>> + unique_ptr<TileSeq[]> clean
			// up automatically.  observers_ is non-owning; nothing
			// to do.  beautyView_ is unique_ptr too.
		}

		// ─────────────────────────────────────────────────────────────
		// Write-side API
		// ─────────────────────────────────────────────────────────────

		void FrameStore::BeginTile( size_t tileX, size_t tileY )
		{
			RejectReentrantObserverPublication();
			// Acquire the tile's exclusive lock.  Subsequent pixel
			// writes happen under this lock; readers (Render) take
			// the shared lock and block until the writer releases.
			// This is C++-standard data-race-free, in contrast to the
			// previous atomic-seqlock which had UB on the non-atomic
			// pixel storage.  See L1 adversarial review P1.
			assert( tileX < tileCountX_ && tileY < tileCountY_ );
			TileLockAt( tileX, tileY ).mtx.lock();
		}

		void FrameStore::EndTile( size_t tileX, size_t tileY )
		{
			assert( tileX < tileCountX_ && tileY < tileCountY_ );
			TileLockAt( tileX, tileY ).mtx.unlock();

			// Bump global generation; readers gate viewport repaints
			// on this counter changing.  Release ordering pairs with
			// the acquire load on the reader side.
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			NotifyTileComplete(tileX,tileY,gen);
		}

		void FrameStore::NotifyTileComplete(
			size_t tileX, size_t tileY, uint64_t generation )
		{
			// Compute the tile rect for the observer callback.
			const unsigned int x0 = static_cast<unsigned int>( tileX * tileEdge_ );
			const unsigned int y0 = static_cast<unsigned int>( tileY * tileEdge_ );
			const unsigned int x1 = static_cast<unsigned int>(
				std::min( ( tileX + 1 ) * tileEdge_, width_ ) );
			const unsigned int y1 = static_cast<unsigned int>(
				std::min( ( tileY + 1 ) * tileEdge_, height_ ) );
			const Rect roi( y0, x0, y1, x1 );

			// Fire OnTileComplete.  Note: callbacks run on the
			// writer's thread per the IRenderObserver contract;
			// observers must marshal to UI threads themselves.
			//
			// We snapshot the observer list under the mutex, then
			// release the mutex BEFORE invoking observers.  This
			// avoids two failure modes: (a) an observer that
			// self-detaches (calls RemoveObserver(this)) inside its
			// callback would otherwise deadlock on the non-recursive
			// mutex.  Callbacks remain synchronous, so a slow observer
			// still back-pressures the publishing writer by contract.  The
			// snapshot also lets registration changes proceed while the
			// callback is running; per-observer lifetime tracking below
			// keeps those raw pointers safe.
			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnTileComplete( roi, generation );
			} );
		}

		void FrameStore::CopyTileFromRasterImage( size_t tileX, size_t tileY,
		                                          const IRasterImage& src,
		                                          const Rect& srcRect )
		{
			// Phase-1 ingest path: copy beauty pixels + alpha from
			// the existing rasterizer's IRasterImage into our typed
			// Beauty/Alpha channels under tile-seqlock protection.
			//
			// srcRect is the area of `src` to read (typically the
			// rasterizer's just-completed tile rect); the
			// corresponding FrameStore region is computed from
			// (tileX, tileY) + tileEdge_.

			if ( !beauty_ ) return;  // 0×0 store

			const unsigned int dstX0 = static_cast<unsigned int>( tileX * tileEdge_ );
			const unsigned int dstY0 = static_cast<unsigned int>( tileY * tileEdge_ );
			const unsigned int dstX1 = static_cast<unsigned int>(
				std::min( ( tileX + 1 ) * tileEdge_, width_ ) );
			const unsigned int dstY1 = static_cast<unsigned int>(
				std::min( ( tileY + 1 ) * tileEdge_, height_ ) );

			BeginTile( tileX, tileY );

			for ( unsigned int dy = dstY0; dy < dstY1; ++dy ) {
				const unsigned int sy = srcRect.top + ( dy - dstY0 );
				for ( unsigned int dx = dstX0; dx < dstX1; ++dx ) {
					const unsigned int sx = srcRect.left + ( dx - dstX0 );
					if ( sx >= src.GetWidth() || sy >= src.GetHeight() ) continue;

					const RISEColor pel = src.GetPEL( sx, sy );
					beauty_->At( dx, dy ) = pel.base;
					if ( alpha_ ) {
						alpha_->At( dx, dy ) = pel.a;
					}
				}
			}

			EndTile( tileX, tileY );
		}

		void FrameStore::MarkFrameComplete( unsigned frame )
		{
			RejectReentrantObserverPublication();
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				completedFrame_.store(frame,std::memory_order_relaxed);
				meta_.denoisedContent = false;
			}
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnFrameComplete( frame, gen );
			} );
		}

		void FrameStore::MarkPreDenoiseComplete( unsigned frame )
		{
			RejectReentrantObserverPublication();
			// Update meta so observers reading Meta().frame inside
			// the callback see the current frame, not whatever the
			// previous MarkFrameComplete left.  See L1 adversarial
			// review MED-4.
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				completedFrame_.store(frame,std::memory_order_relaxed);
				meta_.denoisedContent = false;
			}
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnPreDenoiseComplete( frame, gen );
			} );
		}

		void FrameStore::MarkDenoiseComplete( unsigned frame )
		{
			RejectReentrantObserverPublication();
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				completedFrame_.store(frame,std::memory_order_relaxed);
				meta_.denoisedContent = true;
			}
			const uint64_t gen = globalGeneration_.fetch_add( 1, std::memory_order_release ) + 1;

			DispatchObservers( [&]( IRenderObserver* obs ) {
				obs->OnDenoiseComplete( frame, gen );
			} );
		}

		// ─────────────────────────────────────────────────────────────
		// Render-state snapshot used to roll back incomplete fire primaries.
		// ─────────────────────────────────────────────────────────────

		FrameStore::Snapshot FrameStore::CaptureSnapshot() const
		{
			std::vector<std::shared_lock<std::shared_mutex>> tileLocks;
			tileLocks.reserve(tileCountX_ * tileCountY_);
			for( size_t ty=0; ty<tileCountY_; ++ty ) {
				for( size_t tx=0; tx<tileCountX_; ++tx ) {
					tileLocks.emplace_back(TileLockAt(tx,ty).mtx);
				}
			}

			Snapshot snapshot;
			snapshot.metadata = Meta();
			const auto copyChannel = []( const auto* channel, auto& destination ) {
				if( channel ) destination.assign(channel->Data(),channel->Data()+channel->Size());
			};
			copyChannel(beauty_.get(),snapshot.beauty);
			copyChannel(alpha_.get(),snapshot.alpha);
			copyChannel(albedo_.get(),snapshot.albedo);
			copyChannel(normal_.get(),snapshot.normal);
			copyChannel(depth_.get(),snapshot.depth);
			copyChannel(objectId_.get(),snapshot.objectId);
			copyChannel(primitiveId_.get(),snapshot.primitiveId);
			return snapshot;
		}

		bool FrameStore::RestoreSnapshot( const Snapshot& snapshot )
		{
			const auto matchesChannel = []( const auto* channel, const auto& source ) {
				return channel ? source.size() == channel->Size() : source.empty();
			};
			if( !matchesChannel(beauty_.get(),snapshot.beauty) ||
				!matchesChannel(alpha_.get(),snapshot.alpha) ||
				!matchesChannel(albedo_.get(),snapshot.albedo) ||
				!matchesChannel(normal_.get(),snapshot.normal) ||
				!matchesChannel(depth_.get(),snapshot.depth) ||
				!matchesChannel(objectId_.get(),snapshot.objectId) ||
				!matchesChannel(primitiveId_.get(),snapshot.primitiveId) ) return false;

			std::vector<std::unique_lock<std::shared_mutex>> tileLocks;
			tileLocks.reserve(tileCountX_ * tileCountY_);
			for( size_t ty=0; ty<tileCountY_; ++ty ) {
				for( size_t tx=0; tx<tileCountX_; ++tx ) {
					tileLocks.emplace_back(TileLockAt(tx,ty).mtx);
				}
			}
			const auto restoreChannel = []( auto* channel, const auto& source ) {
				if( channel ) std::copy(source.begin(),source.end(),channel->Data());
			};
			restoreChannel(beauty_.get(),snapshot.beauty);
			restoreChannel(alpha_.get(),snapshot.alpha);
			restoreChannel(albedo_.get(),snapshot.albedo);
			restoreChannel(normal_.get(),snapshot.normal);
			restoreChannel(depth_.get(),snapshot.depth);
			restoreChannel(objectId_.get(),snapshot.objectId);
			restoreChannel(primitiveId_.get(),snapshot.primitiveId);
			SetMetadata(snapshot.metadata);
			globalGeneration_.fetch_add(1,std::memory_order_release);
			return true;
		}

		// FrameStoreBulkBracket — RAII guard for full-image writes.
		// See FrameStore.h for the contract.
		FrameStoreBulkBracket::FrameStoreBulkBracket( FrameStore* fs, const IRasterImage& image )
			: mFs( nullptr )
		{
			if( !fs ) return;
			// Identity check: only bracket when `image` is the actual
			// FrameStore beauty view.  A dim-only check would falsely
			// fire on private RISERasterImages whose dims happen to
			// match camera dims (e.g. BDPT path-guiding training image,
			// `pPreDenoised` scratch in the OIDN path).
			if( &image != &fs->AsBeautyRasterImage() ) return;

			const size_t ntx = fs->TileCountX();
			const size_t nty = fs->TileCountY();

			// Partial-acquire safety: `std::shared_mutex::lock` is
			// allowed to throw `std::system_error` (resource
			// exhaustion, EAGAIN on POSIX).  If we throw partway
			// through the loop without unwinding, the destructor (mFs
			// is null until all-acquired) leaves N partial locks held
			// forever → process-wide deadlock.  Track per-tile
			// progress and release-in-reverse on any throw before
			// rethrowing.  See L6e-1.1 final adversarial review P0.
			size_t acquired = 0;
			try {
				for( size_t ty = 0; ty < nty; ++ty ) {
					for( size_t tx = 0; tx < ntx; ++tx ) {
						fs->BeginTile( tx, ty );
						++acquired;
					}
				}
			} catch( ... ) {
				// Construction never exposed a writable guard, so unwind the
				// acquired prefix without publishing false tile completions.
				while( acquired > 0u ) {
					--acquired;
					fs->TileLockAt(acquired%ntx,acquired/ntx).mtx.unlock();
				}
				throw;
			}
			mFs = fs;  // record only after every BeginTile succeeded
		}

		FrameStoreBulkBracket::~FrameStoreBulkBracket() noexcept
		{
			if( !mFs ) return;
			const size_t ntx = mFs->TileCountX();
			const size_t nty = mFs->TileCountY();
			// A bulk observer may read the whole image. Release every lock
			// before the first callback so that read cannot self-deadlock.
			for( size_t ty = 0; ty < nty; ++ty ) {
				for( size_t tx = 0; tx < ntx; ++tx ) {
					mFs->TileLockAt(tx,ty).mtx.unlock();
				}
			}
			for( size_t ty = 0; ty < nty; ++ty ) {
				for( size_t tx = 0; tx < ntx; ++tx ) {
					const uint64_t generation =
						mFs->globalGeneration_.fetch_add(1,std::memory_order_release)+1;
					try {
						mFs->NotifyTileComplete(tx,ty,generation);
					} catch( ... ) {
						GlobalLog()->PrintEasyError(
							"FrameStore bulk tile observer threw; notification was contained" );
					}
				}
			}
		}

		// ─────────────────────────────────────────────────────────────
		// L7 — AOV propagation utility.
		// Originally lived as a private member of
		// `PixelBasedRasterizerHelper` (`PropagateAOVsToFrameStore_`);
		// L7 follow-up extracted to a free function so MLTRasterizer
		// (which inherits from `Rasterizer`, not the helper) can also
		// populate the canonical FrameStore's AOV channels.  See
		// FrameStore.h doc + commit messages for the contract.
		// ─────────────────────────────────────────────────────────────
		void PropagateAOVsToFrameStore(
			FrameStore* fs,
			const AOVBuffers& aov,
			const Rect* region
			)
		{
			if( !fs ) return;

			const unsigned int aovW = aov.GetWidth();
			const unsigned int aovH = aov.GetHeight();
			if( aovW == 0 || aovH == 0 ) return;

			const unsigned int fsW = static_cast<unsigned int>( fs->Width() );
			const unsigned int fsH = static_cast<unsigned int>( fs->Height() );
			if( aovW != fsW || aovH != fsH ) return;
			unsigned int startX = 0, startY = 0, endX = aovW - 1, endY = aovH - 1;
			if( region ) {
				if( region->left > region->right || region->top > region->bottom ||
					region->left >= aovW || region->top >= aovH ) return;
				startX = region->left;
				startY = region->top;
				endX = r_min( region->right, aovW - 1 );
				endY = r_min( region->bottom, aovH - 1 );
			}

			auto* albedoCh = fs->GetChannel<FrameStoreOutput::ChannelId::Albedo>();
			auto* normalCh = fs->GetChannel<FrameStoreOutput::ChannelId::Normal>();
			auto* depthCh = fs->GetChannel<FrameStoreOutput::ChannelId::Depth>();
			if( !albedoCh && !normalCh && !depthCh ) return;

			const float* albedoSrc = aov.GetAlbedoPtr();
			const float* normalSrc = aov.GetNormalPtr();
			const float* depthSrc = aov.GetDepthPtr();

			{
				FrameStoreBulkBracket bracket( fs, fs->AsBeautyRasterImage() );
				for( unsigned int y = startY; y <= endY; ++y ) {
					for( unsigned int x = startX; x <= endX; ++x ) {
						const size_t idx = ( static_cast<size_t>( y ) * aovW + x ) * 3u;
						if( albedoCh && albedoSrc ) {
							RISEPel& pel = albedoCh->At( x, y );
							pel.r = static_cast<Chel>( albedoSrc[idx + 0] );
							pel.g = static_cast<Chel>( albedoSrc[idx + 1] );
							pel.b = static_cast<Chel>( albedoSrc[idx + 2] );
						}
						if( normalCh && normalSrc ) {
							Vector3& n = normalCh->At( x, y );
							n.x = static_cast<Scalar>( normalSrc[idx + 0] );
							n.y = static_cast<Scalar>( normalSrc[idx + 1] );
							n.z = static_cast<Scalar>( normalSrc[idx + 2] );
						}
						if( depthCh && depthSrc ) {
							depthCh->At( x, y ) = depthSrc[ static_cast<size_t>( y ) * aovW + x ];
						}
					}
				}
			}
		}

		AOVBuffers::Plan MakeAOVPlan( const FrameStore* fs, bool denoiserAux )
		{
			AOVBuffers::Plan plan( denoiserAux, denoiserAux, false );
			if( fs ) {
				plan.albedo = plan.albedo || fs->HasChannel( FrameStoreOutput::ChannelId::Albedo );
				plan.normal = plan.normal || fs->HasChannel( FrameStoreOutput::ChannelId::Normal );
				plan.depth = fs->HasChannel( FrameStoreOutput::ChannelId::Depth );
			}
			return plan;
		}

		// Serialize raw-pointer observer callbacks within this FrameStore,
		// snapshot its list, then claim each callback under the
		// store mutex immediately before invocation.
		//
		// IMPORTANT — per-iteration recheck against observers_:
		// the same-thread RemoveObserver path skips the wait-for-
		// in-flight (otherwise it would self-deadlock).  That means
		// observer A's callback can remove-and-destroy observer B
		// that's LATER in this snapshot.  Without a recheck before
		// invoking each entry we'd dereference B's freed pointer on
		// the next iteration.  We re-acquire the mutex briefly per
		// iteration to verify the observer is still registered;
		// remove-during-dispatch entries are silently skipped.
		// External cross-thread removal waits on the claimed observer's own
		// callback count. A removal attempted from any observer callback
		// fails before mutation when another callback owns the target; this
		// prevents A-removes-B / B-removes-A cycles while allowing independent
		// stores to publish concurrently. Reentrant publication is rejected.
		//
		// This protocol guarantees:
		//   1. No recursive lock: observers can call AddObserver /
		//      RemoveObserver inside their callbacks.
		//   2. No iterator invalidation: we iterate a stack copy.
		//   3. No UAF on freed observers: per-iteration recheck.
		//   4. Cross-thread RemoveObserver-then-destroy is safe: the
		//      wait pairs with the removed observer's callback count.
		//   5. Callback-side cross-store removal cannot block in a cycle.
		template <typename Fn>
		void FrameStore::DispatchObservers( Fn&& fn )
		{
			RejectReentrantObserverPublication();
			std::lock_guard<std::mutex> dispatchLock(observerCallbackDispatchMutex_);
			std::vector<IRenderObserver*> snapshot;
			{
				std::lock_guard<std::mutex> lock( observerMutex_ );
				snapshot = observers_;
			}
			for ( IRenderObserver* obs : snapshot ) {
				bool claimed = false;
				{
					std::unique_lock<std::mutex> lock( observerMutex_ );
					observerDispatchDone_.wait(lock,[this,obs]{
						return std::find(observerRemovalsPrepared_.begin(),
							observerRemovalsPrepared_.end(),obs) ==
							observerRemovalsPrepared_.end();
					});
					if( std::find(observers_.begin(),observers_.end(),obs) !=
						observers_.end() ) {
						++observerCallbacksInFlight_[obs];
						claimed = true;
					}
				}
				if( !claimed ) continue;

				ObserverCallbackFrame callbackFrame { this,obs,g_observerCallbackFrame };
				g_observerCallbackFrame = &callbackFrame;
				try {
					fn( obs );
				} catch ( ... ) {
					g_observerCallbackFrame = callbackFrame.prior;
					{
						std::lock_guard<std::mutex> lock( observerMutex_ );
						auto active = observerCallbacksInFlight_.find(obs);
						if( active != observerCallbacksInFlight_.end() &&
							--active->second == 0u ) observerCallbacksInFlight_.erase(active);
					}
					observerDispatchDone_.notify_all();
					throw;
				}
				g_observerCallbackFrame = callbackFrame.prior;
				{
					std::lock_guard<std::mutex> lock( observerMutex_ );
					auto active = observerCallbacksInFlight_.find(obs);
					if( active != observerCallbacksInFlight_.end() &&
						--active->second == 0u ) observerCallbacksInFlight_.erase(active);
				}
				observerDispatchDone_.notify_all();
			}
		}

		// ─────────────────────────────────────────────────────────────
		// Observer registration
		// ─────────────────────────────────────────────────────────────

		void FrameStore::AddObserver( IRenderObserver* observer )
		{
			if ( !observer ) return;
			std::lock_guard<std::mutex> lock( observerMutex_ );
			// Reject duplicates silently; matches IRasterizerOutput
			// behaviour (callers that re-register an observer on
			// scene reload don't need to first remove).
			auto it = std::find( observers_.begin(), observers_.end(), observer );
			if ( it == observers_.end() ) {
				if( observers_.size()+observerRegistrationReservations_ >=
					observers_.capacity() ) {
					observers_.reserve(
						observers_.size()+observerRegistrationReservations_+1u);
				}
				observers_.push_back( observer );
			}
		}

		void FrameStore::RemoveObserver( IRenderObserver* observer )
		{
			RemoveObserverImpl(observer);
		}

		bool FrameStore::RemoveObserverImpl( IRenderObserver* observer )
		{
			if ( !observer ) return false;
			std::unique_lock<std::mutex> lock( observerMutex_ );
			observerDispatchDone_.wait(lock,[this,observer]{
				return std::find(observerRemovalsPrepared_.begin(),
					observerRemovalsPrepared_.end(),observer) ==
					observerRemovalsPrepared_.end();
			});
			// Wait for callbacks that have already claimed precisely
			// this observer.  Snapshot-only pointers are harmless: every
			// dispatcher rechecks registration before it claims a call.
			//
			// Self-detach cannot wait for its current callback. The callback
			// frame is keyed by both store and observer so a callback on one
			// store cannot exempt an active callback on another store.
			const unsigned int localCallbacks =
				ObserverActiveCountOnThisThread(this,observer);
			const auto activeBeforeRemoval = observerCallbacksInFlight_.find(observer);
			if( g_observerCallbackFrame && activeBeforeRemoval != observerCallbacksInFlight_.end() &&
				activeBeforeRemoval->second > localCallbacks ) {
				throw std::runtime_error(
					"FrameStore observer removal would wait on another callback" );
			}
			auto it = std::find( observers_.begin(), observers_.end(), observer );
			if ( it == observers_.end() ) return false;
			observers_.erase( it );
			observerDispatchDone_.wait( lock, [this,observer,localCallbacks]{
				const auto active = observerCallbacksInFlight_.find(observer);
				return active == observerCallbacksInFlight_.end() ||
					active->second <= localCallbacks;
			} );
			return true;
		}

		FrameStore::ObserverMutationToken::ObserverMutationToken(
			FrameStore& owner, const Kind kind, IRenderObserver* observer )
			: owner_(&owner), kind_(kind), observer_(observer),
			  lock_(owner.observerMutex_)
		{
		}

		FrameStore::ObserverMutationToken::ObserverMutationToken(
			ObserverMutationToken&& other ) noexcept
			: owner_(other.owner_), kind_(other.kind_), observer_(other.observer_),
			  lock_(std::move(other.lock_))
		{
			other.owner_ = nullptr;
			other.observer_ = nullptr;
		}

		void FrameStore::ObserverMutationToken::Reset() noexcept
		{
			owner_ = nullptr;
			observer_ = nullptr;
			if( lock_.owns_lock() ) lock_.unlock();
		}

		FrameStore::ObserverMutationToken::~ObserverMutationToken() noexcept
		{
			if( !owner_ ) return;
			FrameStore* owner = owner_;
			if( !lock_.owns_lock() ) lock_.lock();
			if( kind_ == Kind::Registration ) {
				assert(owner->observerRegistrationReservations_ != 0u);
				if( owner->observerRegistrationReservations_ != 0u ) {
					--owner->observerRegistrationReservations_;
				}
			} else if( observer_ ) {
				auto prepared = std::find(owner->observerRemovalsPrepared_.begin(),
					owner->observerRemovalsPrepared_.end(),observer_);
				if( prepared != owner->observerRemovalsPrepared_.end() ) {
					owner->observerRemovalsPrepared_.erase(prepared);
				}
			}
			Reset();
			owner->observerDispatchDone_.notify_all();
		}

		FrameStore::ObserverMutationToken FrameStore::PrepareObserverRegistration()
		{
			ObserverMutationToken token(*this,
				ObserverMutationToken::Kind::Registration,nullptr);
			if( observers_.size()+observerRegistrationReservations_ >=
				observers_.capacity() ) {
				observers_.reserve(
					observers_.size()+observerRegistrationReservations_+1u);
			}
			++observerRegistrationReservations_;
			token.lock_.unlock();
			return token;
		}

		FrameStore::ObserverMutationToken FrameStore::PrepareObserverRemoval(
			IRenderObserver* observer )
		{
			ObserverMutationToken token(*this,
				ObserverMutationToken::Kind::Removal,observer);
			if( !observer ) {
				token.Reset();
				return token;
			}
			const unsigned int localCallbacks =
				ObserverActiveCountOnThisThread(this,observer);
			const auto activeBeforeRemoval = observerCallbacksInFlight_.find(observer);
			if( g_observerCallbackFrame && activeBeforeRemoval !=
				observerCallbacksInFlight_.end() &&
				activeBeforeRemoval->second > localCallbacks ) {
				throw std::runtime_error(
					"FrameStore observer removal would wait on another callback" );
			}
			if( std::find(observers_.begin(),observers_.end(),observer) ==
				observers_.end() ) {
				token.Reset();
				return token;
			}
			if( std::find(observerRemovalsPrepared_.begin(),
				observerRemovalsPrepared_.end(),observer) !=
				observerRemovalsPrepared_.end() ) {
				throw std::runtime_error(
					"FrameStore observer removal already prepared" );
			}
			observerRemovalsPrepared_.push_back(observer);
			observerDispatchDone_.wait(token.lock_,
				[this,observer,localCallbacks]{
					const auto active = observerCallbacksInFlight_.find(observer);
					return active == observerCallbacksInFlight_.end() ||
						active->second <= localCallbacks;
				});
			token.lock_.unlock();
			return token;
		}

		void FrameStore::LockPreparedObserverMutations(
			const std::vector<ObserverMutationToken*>& tokens,
			const std::function<void(size_t)>& afterLock )
		{
			std::vector<ObserverMutationToken*> distinctStores;
			distinctStores.reserve(tokens.size());
			for( ObserverMutationToken* token : tokens ) {
				if( !token || !token->owner_ || token->lock_.owns_lock() ) {
					throw std::runtime_error(
						"FrameStore observer mutation token set mismatch");
				}
				const bool represented = std::any_of(
					distinctStores.begin(),distinctStores.end(),
					[token]( const ObserverMutationToken* candidate ) {
						return candidate->owner_ == token->owner_;
					});
				if( !represented ) distinctStores.push_back(token);
			}
			std::sort(distinctStores.begin(),distinctStores.end(),
				[]( const ObserverMutationToken* lhs,
					const ObserverMutationToken* rhs ) {
					return std::less<FrameStore*>()(lhs->owner_,rhs->owner_);
				});
			size_t acquired = 0u;
			try {
				for( ObserverMutationToken* token : distinctStores ) {
					token->lock_.lock();
					++acquired;
					if( afterLock ) afterLock(acquired);
				}
			} catch( ... ) {
				while( acquired != 0u ) {
					--acquired;
					distinctStores[acquired]->lock_.unlock();
				}
				throw;
			}
		}

		void FrameStore::CommitPreparedObserverRemoval(
			ObserverMutationToken& token ) noexcept
		{
			assert(token.owner_ == this &&
				token.kind_ == ObserverMutationToken::Kind::Removal &&
				token.lock_.owns_lock());
			if( token.owner_ != this || !token.lock_.owns_lock() ) return;
			auto observer = std::find(observers_.begin(),observers_.end(),
				token.observer_);
			if( observer != observers_.end() ) observers_.erase(observer);
			auto prepared = std::find(observerRemovalsPrepared_.begin(),
				observerRemovalsPrepared_.end(),token.observer_);
			if( prepared != observerRemovalsPrepared_.end() ) {
				observerRemovalsPrepared_.erase(prepared);
			}
			token.Reset();
			observerDispatchDone_.notify_all();
		}

		void FrameStore::CommitPreparedObserverRegistration(
			ObserverMutationToken& token, IRenderObserver* observer ) noexcept
		{
			assert(token.owner_ == this && token.lock_.owns_lock() &&
				token.kind_ == ObserverMutationToken::Kind::Registration);
			if( token.owner_ != this || !token.lock_.owns_lock() ) return;
			if( observer && std::find(observers_.begin(),observers_.end(),observer) ==
				observers_.end() ) {
				assert(observers_.size() < observers_.capacity());
				observers_.push_back(observer);
			}
			assert(observerRegistrationReservations_ != 0u);
			if( observerRegistrationReservations_ != 0u ) {
				--observerRegistrationReservations_;
			}
			token.Reset();
		}

		void FrameStore::CommitPreparedObserverReplacement(
			ObserverMutationToken& registration,
			ObserverMutationToken& removal,
			IRenderObserver* observer ) noexcept
		{
			assert(registration.owner_ == this && removal.owner_ == this &&
				registration.kind_ == ObserverMutationToken::Kind::Registration &&
				removal.kind_ == ObserverMutationToken::Kind::Removal &&
				registration.lock_.owns_lock() && !removal.lock_.owns_lock());
			if( registration.owner_ != this || removal.owner_ != this ||
				!registration.lock_.owns_lock() || removal.lock_.owns_lock() ) return;
			auto oldObserver = std::find(observers_.begin(),observers_.end(),
				removal.observer_);
			if( oldObserver != observers_.end() ) observers_.erase(oldObserver);
			auto prepared = std::find(observerRemovalsPrepared_.begin(),
				observerRemovalsPrepared_.end(),removal.observer_);
			if( prepared != observerRemovalsPrepared_.end() ) {
				observerRemovalsPrepared_.erase(prepared);
			}
			if( observer && std::find(observers_.begin(),observers_.end(),observer) ==
				observers_.end() ) {
				assert(observers_.size() < observers_.capacity());
				observers_.push_back(observer);
			}
			assert(observerRegistrationReservations_ != 0u);
			if( observerRegistrationReservations_ != 0u ) {
				--observerRegistrationReservations_;
			}
			removal.Reset();
			registration.Reset();
			observerDispatchDone_.notify_all();
		}

		// ─────────────────────────────────────────────────────────────
		// Read-side API
		// ─────────────────────────────────────────────────────────────

		// Encode one pixel.  Inline-friendly — called per-pixel from
		// Render's inner loop.  See ApplyViewTransformLinear (L0) for
		// the shared pipeline; this function adds Stage 5 (transfer)
		// + Stage 6 (quantise into target layout).
		void FrameStore::EncodePixel(
			const RISEPel& linearPel,
			double alpha,
			void* dst,
			TargetFormat fmt,
			const ViewTransform& xform ) const
		{
			const TargetFormatInfo& info = GetTargetFormatInfo( fmt );

			// Stages 1-4 (exposure, white balance, primaries, tone curve).
			// Tone curve runs only when target is LDR fixed.
			double r, g, b;
			ApplyViewTransformLinear( xform, info.colorSpace,
			                          info.isLDRFixed,
			                          linearPel.r, linearPel.g, linearPel.b,
			                          r, g, b );

			// Stage 5: transfer function.
			r = ApplyTransfer( info.transferFn, r );
			g = ApplyTransfer( info.transferFn, g );
			b = ApplyTransfer( info.transferFn, b );

			// Alpha handling:
			//   - FIXED-point targets (8-bit / 16-bit unsigned, both
			//     LDR-fixed sRGB AND L5c HDR10 PQ): sanitise NaN/Inf/
			//     negative to 0 and clamp >1 to 1 so the quantiser has
			//     well-defined input.  Without the pre-clamp, NaN
			//     alpha would survive into the Q8/Q16 lambdas where
			//     `if (v < 0.0)` and `if (v > 65535.0)` both evaluate
			//     false for NaN and `static_cast<uint16_t>(NaN)` is UB.
			//   - HDR FLOAT targets (RGBA32F_*, RGBA16F_*): pass
			//     alpha through bit-identically.  Archival paths
			//     (EXR / .hdr) need to preserve out-of-range alpha
			//     for round-trip fidelity with reconstruction-
			//     filter ringing or premultiplied workflows.
			//
			// L5c — gate generalised from `isLDRFixed` to `!isFloat`.
			// Pre-L5c the gate was `isLDRFixed` because every fixed-
			// point format was also LDR (8/16-bit sRGB).  HDR10 PQ
			// 16-bit FIXED breaks that coupling: it's HDR (no tone
			// curve) but quantised (needs alpha clamp).  The new
			// gate matches the downstream quantiser branch structure.
			//
			// The shared finite predicate materialises its input through
			// volatile before checking its IEEE-754 exponent, so it stays
			// live under -ffast-math.  L1 adversarial review MED-8 + P3.
			double a = alpha;
			if ( !info.isFloat ) {
				if ( !RISE::IsFiniteDouble( a ) || !( a > 0.0 ) ) a = 0.0;
				if ( a > 1.0 ) a = 1.0;
			}

			// Stage 6: pack into target pixel layout.
			if ( !info.isFloat ) {
				// 8-bit / 16-bit fixed point.  Quantise via round-to-nearest.
				if ( info.bytesPerPixel == 4 || info.bytesPerPixel == 3 ) {
					// 8-bit
					auto Q8 = []( double x ) -> uint8_t {
						double v = x * 255.0 + 0.5;
						if ( v < 0.0 )   v = 0.0;
						if ( v > 255.0 ) v = 255.0;
						return static_cast<uint8_t>( v );
					};
					const uint8_t R = Q8( r );
					const uint8_t G = Q8( g );
					const uint8_t B = Q8( b );
					const uint8_t A = Q8( a );
					uint8_t* p = static_cast<uint8_t*>( dst );
					switch ( info.channelOrder ) {
						case ChannelOrder::RGBA: p[0]=R; p[1]=G; p[2]=B; p[3]=A; break;
						case ChannelOrder::RGB:  p[0]=R; p[1]=G; p[2]=B;          break;
						case ChannelOrder::BGRA: p[0]=B; p[1]=G; p[2]=R; p[3]=A;  break;
					}
				} else {
					// 16-bit unsigned.  Pre-L5c only RGBA16_sRGB
					// landed here (4-channel RGBA).  L5c added
					// RGBA16_BT2020_PQ (still 4-channel RGBA, just
					// HDR PQ-encoded) and RGB16_BT2020_PQ (3-channel
					// RGB, HDR10 PNG without alpha).  The branch now
					// dispatches on `channelCount` + `channelOrder`
					// matching the 8-bit branch's pattern above.
					auto Q16 = []( double x ) -> uint16_t {
						double v = x * 65535.0 + 0.5;
						if ( v < 0.0 )      v = 0.0;
						if ( v > 65535.0 )  v = 65535.0;
						return static_cast<uint16_t>( v );
					};
					const uint16_t R = Q16( r );
					const uint16_t G = Q16( g );
					const uint16_t B = Q16( b );
					const uint16_t A = Q16( a );
					uint16_t* p = static_cast<uint16_t*>( dst );
					switch ( info.channelOrder ) {
						case ChannelOrder::RGBA:
							assert( info.channelCount == 4 );
							p[0]=R; p[1]=G; p[2]=B; p[3]=A;
							break;
						case ChannelOrder::RGB:
							assert( info.channelCount == 3 );
							p[0]=R; p[1]=G; p[2]=B;
							break;
						case ChannelOrder::BGRA:
							// Reserved for future Win32-DIB-16 variants.
							assert( info.channelCount == 4 );
							p[0]=B; p[1]=G; p[2]=R; p[3]=A;
							break;
					}
				}
			} else if ( info.isHalfFloat ) {
				// 16-bit float (4-channel; only RGBA16F formats exist).
				// Use the *Bits helpers from FrameStoreColorSpace so
				// the float→half conversion never goes through a
				// `float`-typed temporary.  Under -ffast-math the
				// compiler may fold operations on float-typed values
				// assuming they're finite, so we keep the entire
				// pipeline in uint32_t bit-space.  See L1 adversarial
				// review HIGH-1.
				auto DoubleToHalfBits = []( double x ) -> uint16_t {
					const float f = static_cast<float>( x );
					uint32_t fbits;
					std::memcpy( &fbits, &f, sizeof(fbits) );
					return FloatBitsToHalf( fbits );
				};
				const uint16_t R = DoubleToHalfBits( r );
				const uint16_t G = DoubleToHalfBits( g );
				const uint16_t B = DoubleToHalfBits( b );
				const uint16_t A = DoubleToHalfBits( a );
				uint16_t* p = static_cast<uint16_t*>( dst );
				p[0]=R; p[1]=G; p[2]=B; p[3]=A;
			} else {
				// 32-bit float (RGBA32F or RGB32F).
				float* p = static_cast<float*>( dst );
				p[0] = static_cast<float>( r );
				p[1] = static_cast<float>( g );
				p[2] = static_cast<float>( b );
				if ( info.hasAlpha ) {
					p[3] = static_cast<float>( a );
				}
			}
		}

		void FrameStore::Render(
			void* dst,
			size_t dstStride,
			const Rect& roi,
			TargetFormat fmt,
			const ViewTransform& xform,
			bool nonBlocking ) const
		{
			if ( !dst || !beauty_ ) return;

			const unsigned int x0 = roi.left;
			const unsigned int y0 = roi.top;
			const unsigned int x1 = std::min( roi.right,  static_cast<unsigned int>( width_ ) );
			const unsigned int y1 = std::min( roi.bottom, static_cast<unsigned int>( height_ ) );
			if ( x1 <= x0 || y1 <= y0 ) return;

			const TargetFormatInfo& info = GetTargetFormatInfo( fmt );
			const size_t bpp = info.bytesPerPixel;

			// Iterate by tile so the per-tile shared_mutex works at
			// the right granularity.  Pixels outside the tile-aligned
			// clipped region are ignored.
			const size_t tx0 = x0 / tileEdge_;
			const size_t ty0 = y0 / tileEdge_;
			const size_t tx1 = ( x1 + tileEdge_ - 1 ) / tileEdge_;
			const size_t ty1 = ( y1 + tileEdge_ - 1 ) / tileEdge_;

			for ( size_t ty = ty0; ty < ty1; ++ty ) {
				for ( size_t tx = tx0; tx < tx1; ++tx ) {
					const unsigned int tileX0 = static_cast<unsigned int>( std::max<size_t>( tx * tileEdge_, x0 ) );
					const unsigned int tileY0 = static_cast<unsigned int>( std::max<size_t>( ty * tileEdge_, y0 ) );
					const unsigned int tileX1 = static_cast<unsigned int>( std::min<size_t>( ( tx + 1 ) * tileEdge_, x1 ) );
					const unsigned int tileY1 = static_cast<unsigned int>( std::min<size_t>( ( ty + 1 ) * tileEdge_, y1 ) );

					// Take the tile's shared lock.  Multiple Render
					// readers can hold this concurrently; if a
					// writer holds the exclusive lock (BeginTile/
					// EndTile window) we block here until they
					// release — UNLESS `nonBlocking` is true, in
					// which case we try_lock_shared and SKIP the
					// tile on contention (the caller's `dst` buffer
					// retains whatever pixels were there before;
					// see Render() header doc for the polling-poll
					// use case).
					std::shared_lock<std::shared_mutex> readLock(
						TileLockAt( tx, ty ).mtx, std::defer_lock );
					if ( nonBlocking ) {
						if ( !readLock.try_lock() ) {
							continue;  // tile held exclusive, skip
						}
					} else {
						readLock.lock();
					}

					// Copy + transform pixels in [tileX0, tileX1) × [tileY0, tileY1).
					for ( unsigned int y = tileY0; y < tileY1; ++y ) {
						const RISEPel* beautyRow = beauty_->Row( y );
						const Chel*    alphaRow  = alpha_ ? alpha_->Row( y ) : nullptr;
						uint8_t* dstRow = static_cast<uint8_t*>( dst )
						                  + static_cast<size_t>( y - y0 ) * dstStride
						                  + static_cast<size_t>( tileX0 - x0 ) * bpp;
						for ( unsigned int x = tileX0; x < tileX1; ++x ) {
							const double a = alphaRow ? alphaRow[x] : 1.0;
							EncodePixel( beautyRow[x], a, dstRow, fmt, xform );
							dstRow += bpp;
						}
					}
				}
			}
		}

		// ─────────────────────────────────────────────────────────────
		// Back-compat IRasterImage shim accessors.
		// (The class body itself lives at the top of this file so it
		// is complete before the FrameStore constructor's eager
		// `new BeautyRasterImageView(*this)` instantiation.)
		// ─────────────────────────────────────────────────────────────

		IRasterImage& FrameStore::AsBeautyRasterImage()
		{
			// View is constructed eagerly in the FrameStore
			// constructor — no lazy-init race.
			return *beautyView_;
		}

		const IRasterImage& FrameStore::AsBeautyRasterImage() const
		{
			return *beautyView_;
		}

	} // namespace Implementation
} // namespace RISE
