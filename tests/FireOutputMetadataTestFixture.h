#ifndef FIREOUTPUTMETADATATESTFIXTURE_H_
#define FIREOUTPUTMETADATATESTFIXTURE_H_

#include "../src/Library/Utilities/RISECBOR64.h"

#include <initializer_list>
#include <string>

namespace FireOutputMetadataTestFixture
{
	inline RISE::RISECBOR64::Value UniformMap(
		const std::initializer_list<const char*> keys,
		const RISE::RISECBOR64::Value& value )
	{
		RISE::RISECBOR64::Value::Members members;
		for( const char* key : keys ) members.push_back(std::make_pair(key,value));
		return RISE::RISECBOR64::Value::MapValue(members);
	}

	inline RISE::RISECBOR64::Bytes ResolvedConfig(
		const unsigned int width,
		const unsigned int height )
	{
		using RISE::RISECBOR64::Value;
		const Value frameSelection = Value::MapValue({
			{ "active", Value::Bool(false) }, { "index", Value::Unsigned(0) } });
		const Value camera = Value::MapValue({
			{ "exposure_compensation_ev", Value::Float(0.0) },
			{ "exposure_time", Value::Float(0.0) }, { "kind", Value::String("none") },
			{ "location", Value::ArrayValue({}) }, { "matrix", Value::ArrayValue({}) },
			{ "pixel_rate", Value::Float(0.0) }, { "projection", Value::MapValue({}) },
			{ "scanning_rate", Value::Float(0.0) } });
		const Value autoProbe = Value::MapValue({
			{ "activation_spp", Value::Unsigned(1) },
			{ "reach_winsor_percentile", Value::Float(0.99) },
			{ "scale", Value::Unsigned(1) }, { "spp", Value::Unsigned(1) },
			{ "tau_bdpt", Value::Float(1.0) }, { "tau_caustic", Value::Float(1.0) },
			{ "tau_reach", Value::Float(1.0) },
			{ "variance_renders", Value::Unsigned(2) } });
		const Value guiding = Value::MapValue({
			{ "alpha", Value::Float(0.0) },
			{ "combine_training_iterations", Value::Bool(false) },
			{ "complete_path_guiding", Value::Bool(false) },
			{ "complete_path_strategy_samples", Value::Unsigned(0) },
			{ "complete_path_strategy_selection", Value::Bool(false) },
			{ "enabled", Value::Bool(false) }, { "learned_alpha", Value::Bool(false) },
			{ "max_guiding_depth", Value::Unsigned(0) },
			{ "max_light_guiding_depth", Value::Unsigned(0) },
			{ "online", Value::Bool(false) }, { "ris_candidates", Value::Unsigned(0) },
			{ "sampling_type", Value::Unsigned(0) },
			{ "training_iterations", Value::Unsigned(0) },
			{ "training_spp", Value::Unsigned(0) },
			{ "warmup_iterations", Value::Unsigned(0) } });
		const Value sms = Value::MapValue({
			{ "bernoulli_trials", Value::Unsigned(0) }, { "biased", Value::Bool(false) },
			{ "enabled", Value::Bool(false) }, { "max_chain_depth", Value::Unsigned(0) },
			{ "max_iterations", Value::Unsigned(0) },
			{ "max_photon_seeds_per_shading_point", Value::Unsigned(0) },
			{ "multi_trials", Value::Unsigned(0) }, { "photon_count", Value::Unsigned(0) },
			{ "seeding_mode", Value::Unsigned(0) }, { "target_bounces", Value::Unsigned(0) },
			{ "threshold", Value::Float(0.0) }, { "two_stage", Value::Bool(false) },
			{ "use_levenberg_marquardt", Value::Bool(false) } });
		const Value sampler = Value::MapValue({
			{ "adaptive", Value::MapValue({ { "max_samples", Value::Unsigned(0) },
				{ "show_map", Value::Bool(false) }, { "threshold", Value::Float(0.0) } }) },
			{ "blue_noise", Value::Bool(false) },
			{ "large_step_probability", Value::Float(0.0) },
			{ "luminary_sampler", Value::String("none") },
			{ "luminary_sampler_param", Value::Float(0.0) },
			{ "mlt_bootstrap_samples", Value::Unsigned(0) },
			{ "mlt_chains", Value::Unsigned(0) },
			{ "mlt_mutations_per_pixel", Value::Unsigned(0) },
			{ "num_luminary_samples", Value::Unsigned(0) },
			{ "pixel_sampler", Value::String("random") },
			{ "pixel_sampler_param", Value::Float(0.0) },
			{ "pixel_samples", Value::Unsigned(1) },
			{ "progressive", Value::MapValue({ { "enabled", Value::Bool(false) },
				{ "samples_per_pass", Value::Unsigned(1) } }) },
			{ "spectral", Value::MapValue({ { "hwss", Value::Bool(false) },
				{ "nm_begin", Value::Float(380.0) }, { "nm_end", Value::Float(780.0) },
				{ "num_wavelengths", Value::Unsigned(1) },
				{ "spectral_samples", Value::Unsigned(1) } }) } });
		const Value record = Value::MapValue({
			{ "animation", Value::MapValue({ { "do_fields", Value::Bool(false) },
				{ "frame_selection", frameSelection }, { "invert_fields", Value::Bool(false) },
				{ "num_frames", Value::Unsigned(1) }, { "time_end", Value::Float(0.0) },
				{ "time_start", Value::Float(0.0) } }) },
			{ "aov", Value::MapValue({ { "channels", Value::ArrayValue({
				Value::String("beauty") }) } }) }, { "camera", camera },
			{ "clamp", UniformMap({ "direct", "indirect" },Value::Float(0.0)) },
			{ "depth", UniformMap({ "max_diffuse_bounce", "max_eye_depth",
				"max_glossy_bounce", "max_light_depth", "max_recursion",
				"max_translucent_bounce", "max_transmission_bounce", "max_volume_bounce" },
				Value::Unsigned(0)) },
			{ "evaluated_camera_states", Value::ArrayValue({}) },
			{ "execution", Value::MapValue({
				{ "effective_worker_task_count", Value::Unsigned(1) },
				{ "force_number_of_threads", Value::Signed(0) },
				{ "maximum_thread_count", Value::Signed(1) },
				{ "random_stream_policy", Value::String("test") },
				{ "render_thread_reserve_count", Value::Signed(0) } }) },
			{ "external_runtime", Value() },
			{ "film", Value::MapValue({ { "height", Value::Unsigned(height) },
				{ "pixel_aspect_ratio", Value::Float(1.0) },
				{ "width", Value::Unsigned(width) } }) },
			{ "filter", Value::MapValue({ { "height", Value::Float(1.0) },
				{ "name", Value::String("box") }, { "param_a", Value::Float(0.0) },
				{ "param_b", Value::Float(0.0) }, { "width", Value::Float(1.0) } }) },
			{ "global_render_options", Value::MapValue({ { "auto_probe", autoProbe },
				{ "vcm", Value::MapValue({
					{ "progressive_radius_enabled", Value::Bool(true) },
					{ "throughput_clamp_multiplier", Value::Float(20.0) },
					{ "throughput_clamp_percentile", Value::Float(0.99) } }) } }) },
			{ "integrator", Value::MapValue({ { "auto_choice", Value::Unsigned(0) },
				{ "auto_probe_enabled", Value::Bool(false) },
				{ "effective_kind", Value::String("pt") },
				{ "enable_vertex_connection", Value::Bool(false) },
				{ "enable_vertex_merging", Value::Bool(false) },
				{ "integrate_rgb", Value::Bool(false) },
				{ "kind", Value::String("pathtracing_spectral") },
				{ "merge_radius", Value::Float(0.0) }, { "path_guiding", guiding },
				{ "show_luminaires", Value::Bool(false) }, { "sms", sms } }) },
			{ "light_sampling", Value::MapValue({ { "rr_threshold", Value::Float(0.0) } }) },
			{ "raster_sequence", Value::MapValue({
				{ "kind", Value::String("rasterizer_default") } }) },
			{ "record_kind", Value::String("resolved_render_configuration_v1") },
			{ "render_region", Value::MapValue({ { "active", Value::Bool(false) },
				{ "bottom", Value::Unsigned(0) }, { "left", Value::Unsigned(0) },
				{ "right", Value::Unsigned(0) }, { "top", Value::Unsigned(0) } }) },
			{ "sampler", sampler }, { "schema_version", Value::Unsigned(1) },
			{ "shader", Value::String("none") },
			{ "stability", Value::MapValue({ { "filter_glossy", Value::Float(0.0) },
				{ "optimal_mis", Value::Bool(false) },
				{ "optimal_mis_tile_size", Value::Unsigned(1) },
				{ "optimal_mis_training_iterations", Value::Unsigned(0) },
				{ "rr_min_depth", Value::Unsigned(0) },
				{ "rr_threshold", Value::Float(0.0) },
				{ "transparent_shadows", Value::Bool(false) },
				{ "use_light_bvh", Value::Bool(false) } }) },
			{ "transport", Value::MapValue({ { "oidn", Value::Bool(false) },
				{ "oidn_device", Value::Unsigned(0) },
				{ "oidn_prefilter", Value::Unsigned(0) },
				{ "oidn_quality", Value::Unsigned(0) },
				{ "radiance_map", Value::MapValue({ { "background", Value::Bool(false) },
					{ "name", Value::String("none") },
					{ "orientation", Value::ArrayValue({ Value::Float(0.0),
						Value::Float(0.0),Value::Float(0.0) }) },
					{ "scale", Value::Float(1.0) } }) } }) } });
		RISE::RISECBOR64::Bytes encoded;
		std::string error;
		RISE::RISECBOR64::Encode(record,encoded,&error);
		return encoded;
	}

	inline RISE::RISECBOR64::Bytes RendererBuild()
	{
		using RISE::RISECBOR64::Value;
		const Value linked = Value::MapValue({
			{ "availability", Value::String("not_linked") },
			{ "linkage", Value::String("not_linked") },
			{ "loaded_binaries", Value::ArrayValue({}) },
			{ "version", Value::String("not_linked") } });
		const Value runtime = Value::MapValue({
			{ "availability", Value::String("not_loaded") },
			{ "linkage", Value::String("runtime_optional") },
			{ "loaded_binaries", Value::ArrayValue({}) },
			{ "version", Value::String("not_loaded") } });
		const Value record = Value::MapValue({
			{ "compiler", Value::MapValue({ { "identity", Value::String("test") },
				{ "language_standard", Value::String("c++17") },
				{ "lto_mode", Value::String("off") },
				{ "optimization_mode", Value::String("disabled") } }) },
			{ "dependency_builds", Value::MapValue({ { "avcodec", runtime },
				{ "avfoundation", runtime }, { "avformat", runtime }, { "avutil", runtime },
				{ "iex", linked }, { "ilmthread", linked }, { "imath", linked },
				{ "oidn", linked }, { "openexr", linked }, { "openpgl", linked },
				{ "png", linked }, { "swscale", runtime }, { "tiff", linked },
				{ "videotoolbox", runtime }, { "x265", runtime }, { "zlib", linked } }) },
			{ "dirty_state", Value::MapValue({
				{ "diff_sha256", Value::String(std::string(64,'0')) },
				{ "state", Value::String("clean") } }) },
			{ "fp_settings", Value::MapValue({
				{ "contraction_mode", Value::String("off") },
				{ "fast_math", Value::Bool(false) },
				{ "finite_math_only", Value::Bool(false) } }) },
			{ "gate_harness_version", Value::String("test") },
			{ "record_kind", Value::String("renderer_build_v1") },
			{ "renderer_binary", Value::MapValue({
				{ "hash_basis", Value::String("file_bytes") },
				{ "kind", Value::String("executable") },
				{ "path", Value::String("test") },
				{ "sha256", Value::String(std::string(64,'0')) } }) },
			{ "renderer_version", Value::String("test") },
			{ "schema_version", Value::Unsigned(1) },
			{ "solver_schema_versions", Value::ArrayValue({ Value::String("test") }) },
			{ "source_revision", Value::String("test-build") },
			{ "target", Value::MapValue({ { "architecture", Value::String("test") },
				{ "platform", Value::String("test") } }) } });
		RISE::RISECBOR64::Bytes encoded;
		std::string error;
		RISE::RISECBOR64::Encode(record,encoded,&error);
		return encoded;
	}
}

#endif
