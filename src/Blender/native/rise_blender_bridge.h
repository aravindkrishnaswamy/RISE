#ifndef RISE_BLENDER_BRIDGE_H
#define RISE_BLENDER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define RISE_BLENDER_EXPORT __declspec(dllexport)
#else
#define RISE_BLENDER_EXPORT
#endif

#define RISE_BLENDER_API_VERSION 8

#ifdef __cplusplus
extern "C" {
#endif

enum rise_blender_projection_type {
	RISE_BLENDER_CAMERA_PERSPECTIVE = 0,
	RISE_BLENDER_CAMERA_ORTHOGRAPHIC = 1
};

enum rise_blender_light_type {
	RISE_BLENDER_LIGHT_POINT = 0,
	RISE_BLENDER_LIGHT_SPOT = 1,
	RISE_BLENDER_LIGHT_SUN = 2,
	RISE_BLENDER_LIGHT_AREA = 3,
	RISE_BLENDER_LIGHT_AMBIENT = 4
};

enum rise_blender_painter_kind {
	RISE_BLENDER_PAINTER_UNIFORM = 0,
	RISE_BLENDER_PAINTER_TEXTURE_PNG = 1,
	RISE_BLENDER_PAINTER_TEXTURE_EXR = 2,
	RISE_BLENDER_PAINTER_TEXTURE_HDR = 3,
	RISE_BLENDER_PAINTER_TEXTURE_TIFF = 4,
	RISE_BLENDER_PAINTER_BLEND = 5,
	RISE_BLENDER_PAINTER_TEXTURE_JPEG = 6,
	// UV-transform wrapper.  `painter_a_name` is the source painter
	// (typically a texture painter); `uv_offset_*`, `uv_rotation`,
	// `uv_scale_*` are the affine transform applied to (u, v) before
	// the source is sampled.  Matches the KHR_texture_transform
	// convention RISE's UVTransformPainter implements.  Used by the
	// exporter to honour Blender's Mapping node (vector_type=POINT)
	// between a Texture Coordinate and an Image Texture node.
	RISE_BLENDER_PAINTER_UV_TRANSFORM = 7
};

enum rise_blender_color_space {
	RISE_BLENDER_COLOR_LINEAR = 0,
	RISE_BLENDER_COLOR_SRGB = 1,
	// ROMM-Linear bypasses both gamma decoding and the
	// Rec709->ROMM colour matrix — required for normal-map painters
	// so the RGB-encoded surface tangent stays bit-exact.
	RISE_BLENDER_COLOR_ROMM_LINEAR = 2
};

enum rise_blender_modifier_kind {
	RISE_BLENDER_MODIFIER_BUMP = 0,
	RISE_BLENDER_MODIFIER_NORMAL_MAP = 1
};

enum rise_blender_material_model {
	RISE_BLENDER_MATERIAL_LAMBERT = 0,
	RISE_BLENDER_MATERIAL_GGX = 1,
	RISE_BLENDER_MATERIAL_DIELECTRIC = 2,
	RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS = 3
};

enum rise_blender_pixel_filter {
	RISE_BLENDER_PIXEL_FILTER_NONE = 0,
	RISE_BLENDER_PIXEL_FILTER_BOX = 1,
	RISE_BLENDER_PIXEL_FILTER_TENT = 2,
	RISE_BLENDER_PIXEL_FILTER_GAUSSIAN = 3,
	RISE_BLENDER_PIXEL_FILTER_MITCHELL = 4,
	RISE_BLENDER_PIXEL_FILTER_CATMULL_ROM = 5,
	RISE_BLENDER_PIXEL_FILTER_CUBIC_BSPLINE = 6,
	RISE_BLENDER_PIXEL_FILTER_BLACKMAN = 7
};

enum rise_blender_phase_type {
	RISE_BLENDER_PHASE_ISOTROPIC = 0,
	RISE_BLENDER_PHASE_HG = 1
};

enum rise_blender_medium_kind {
	RISE_BLENDER_MEDIUM_HOMOGENEOUS = 0,
	RISE_BLENDER_MEDIUM_HETEROGENEOUS_VDB = 1
};

enum rise_blender_oidn_quality {
	RISE_BLENDER_OIDN_QUALITY_AUTO = 0,
	RISE_BLENDER_OIDN_QUALITY_HIGH = 1,
	RISE_BLENDER_OIDN_QUALITY_BALANCED = 2,
	RISE_BLENDER_OIDN_QUALITY_FAST = 3
};

enum rise_blender_oidn_device {
	RISE_BLENDER_OIDN_DEVICE_AUTO = 0,
	RISE_BLENDER_OIDN_DEVICE_CPU = 1,
	RISE_BLENDER_OIDN_DEVICE_GPU = 2
};

enum rise_blender_oidn_prefilter {
	RISE_BLENDER_OIDN_PREFILTER_FAST = 0,
	RISE_BLENDER_OIDN_PREFILTER_ACCURATE = 1
};

enum rise_blender_sms_seeding {
	RISE_BLENDER_SMS_SEEDING_SNELL = 0,
	RISE_BLENDER_SMS_SEEDING_UNIFORM = 1
};

enum rise_blender_path_guiding_sampling {
	RISE_BLENDER_PG_ONE_SAMPLE_MIS = 0,
	RISE_BLENDER_PG_RIS = 1
};

// Rasterizer / integrator selection.  Matches the Mac/Windows GUI
// dropdown; the legacy bridge `use_path_tracing` boolean is now
// derived from this enum for back-compat (kept for v4 ABI parity
// — v5 callers should set `rasterizer_kind` directly).
//
//  PIXELPEL     — Legacy IRasterizer with shader-op chain (no PT).
//                 Cheapest direct-only rasterizer; no SMS, no MIS,
//                 no bidirectional connections.  Useful for fast
//                 viewport-quality previews.
//  PT_PEL       — Pure path tracing (RGB).  Default since the bridge
//                 first shipped.  Owns NEE / MIS / SMS / direct +
//                 indirect in one integrator.
//  PT_SPECTRAL  — Spectral path tracing (Hero Wavelength Sampling).
//                 Use for dispersion / wavelength-dependent IORs.
//  BDPT_PEL     — Bidirectional path tracing (RGB).  Beats PT on
//                 indirect lighting through small openings (church
//                 windows, glass-block corridors).
//  BDPT_SPECTRAL— Spectral BDPT.
//  VCM_PEL      — Vertex Connection & Merging (RGB).  Adds photon
//                 merging on top of BDPT — handles caustics and
//                 "through glass" cases (water-with-pebbles, glass
//                 spheres) that BDPT can't connect.
//  VCM_SPECTRAL — Spectral VCM.
//  MLT_PEL      — Metropolis Light Transport / PSSMLT (RGB).
//                 Best on scenes with very small light sources or
//                 hard-to-find caustics; mutates entire paths via
//                 Markov chains.
//  MLT_SPECTRAL — Spectral MLT.
enum rise_blender_rasterizer_kind {
	RISE_BLENDER_RASTERIZER_PIXELPEL      = 0,
	RISE_BLENDER_RASTERIZER_PT_PEL        = 1,
	RISE_BLENDER_RASTERIZER_PT_SPECTRAL   = 2,
	RISE_BLENDER_RASTERIZER_BDPT_PEL      = 3,
	RISE_BLENDER_RASTERIZER_BDPT_SPECTRAL = 4,
	RISE_BLENDER_RASTERIZER_VCM_PEL       = 5,
	RISE_BLENDER_RASTERIZER_VCM_SPECTRAL  = 6,
	RISE_BLENDER_RASTERIZER_MLT_PEL       = 7,
	RISE_BLENDER_RASTERIZER_MLT_SPECTRAL  = 8,
	RISE_BLENDER_RASTERIZER_AUTO_PEL       = 9,
	RISE_BLENDER_RASTERIZER_AUTO_SPECTRAL  = 10
};

typedef struct rise_blender_camera {
	int projection_type;
	float location[3];
	float forward[3];
	float up[3];
	float fov_y_radians;
	float ortho_scale;
	uint32_t width;
	uint32_t height;
	float pixel_aspect;
	float shift_x;
	float shift_y;
	// ABI v7 — realistic-camera fields, mirroring Blender's
	// `camera.data` Cycles surface.  When `use_dof` is non-zero the
	// bridge dispatches to AddThinlensCamera with the full optical
	// model; when zero (and `iso` is also zero) the existing pinhole
	// path is taken bit-identically to v6.  Setting `iso > 0` enables
	// the photographic-EV pipeline (Landing 5 of the cameras
	// roadmap) on the pinhole path too — `fstop` is then used for
	// the EV computation but no aperture-disc DOF is simulated.
	//
	// All distances are in Blender's scene units; `scene_unit_meters`
	// is the global "Unit Scale" from the Blender scene's unit
	// settings (e.g. 1.0 for metres, 0.001 for millimetres) so RISE
	// can convert focal/sensor (mm) to scene-units consistently.
	float focal_length_mm;	// camera.data.lens
	float sensor_width_mm;	// camera.data.sensor_width
	float sensor_height_mm;	// camera.data.sensor_height (only used when sensor_fit_vertical != 0)
	int   sensor_fit_vertical;	// 0 = AUTO/HORIZONTAL→use sensor_width; 1 = VERTICAL→use sensor_height.  Mirrors Blender's `camera.data.sensor_fit`.
	int   use_dof;			// camera.data.dof.use_dof — 0 = pinhole, non-zero = thinlens
	float fstop;			// camera.data.dof.aperture_fstop
	float focus_distance;	// camera.data.dof.focus_distance (scene units)
	int   aperture_blades;	// camera.data.dof.aperture_blades — 0 = circular disc, >=3 = polygon
	float aperture_rotation_radians;	// camera.data.dof.aperture_rotation
	float aperture_ratio;	// camera.data.dof.aperture_ratio (1.0 = circular, anamorphic squeeze)
	float iso;			// RISE-specific photographic ISO; 0 = disabled (default).  When > 0, requires `fstop > 0` and an exposure > 0; computes ISO-12232 EV that stacks into LDR outputs.  Cycles has no native ISO concept — exposed via an add-on property.
	float scene_unit_meters;	// bpy.context.scene.unit_settings.scale_length — metres per scene unit
} rise_blender_camera;

typedef struct rise_blender_painter {
	const char* name;
	int kind;
	float color[3];
	const char* path;
	int color_space;
	int filter_type;
	int lowmemory;
	float scale[3];
	float shift[3];
	const char* painter_a_name;
	const char* painter_b_name;
	const char* mask_painter_name;
	// UV-transform painter (kind = RISE_BLENDER_PAINTER_UV_TRANSFORM):
	// the source painter is `painter_a_name`; sampling happens at
	//   u' =  cos(rot) * scale_u * u + sin(rot) * scale_v * v + ofs_u
	//   v' = -sin(rot) * scale_u * u + cos(rot) * scale_v * v + ofs_v
	// (matches RISE's UVTransformPainter, which itself follows the
	// KHR_texture_transform sign convention — positive rotation
	// rotates the IMAGE clockwise).  Unused / left zero by every
	// other painter kind.
	float uv_offset_u;
	float uv_offset_v;
	float uv_rotation;
	float uv_scale_u;
	float uv_scale_v;
} rise_blender_painter;

typedef struct rise_blender_modifier {
	const char* name;
	int kind;
	const char* source_painter_name;
	float scale;
	float window;
} rise_blender_modifier;

typedef struct rise_blender_material {
	const char* name;
	int model;
	// Lambert / GGX / Dielectric slots
	const char* diffuse_painter_name;
	const char* specular_painter_name;
	const char* alpha_x_painter_name;
	const char* alpha_y_painter_name;
	const char* ior_painter_name;
	const char* extinction_painter_name;
	const char* tau_painter_name;
	const char* scatter_painter_name;
	const char* emission_painter_name;
	int double_sided;
	// PBR Metallic-Roughness slots (KHR_materials core + KHR_materials_anisotropy)
	const char* base_color_painter_name;
	const char* metallic_painter_name;
	const char* roughness_painter_name;
	const char* specular_factor_painter_name;       // NULL = "1.0"
	const char* specular_color_painter_name;        // NULL = "none" (untinted dielectric F0)
	const char* anisotropy_factor_painter_name;     // NULL = "0.0"
	const char* anisotropy_rotation_painter_name;   // NULL = "0.0"
	double emissive_scale;
} rise_blender_material;

typedef struct rise_blender_mesh {
	const char* name;
	const float* vertices;
	const float* normals;
	const float* uvs;
	const uint32_t* vertex_indices;
	const uint32_t* normal_indices;
	const uint32_t* uv_indices;
	uint32_t num_vertices;
	uint32_t num_normals;
	uint32_t num_uvs;
	uint32_t num_triangles;
	int double_sided;
	int use_face_normals;
} rise_blender_mesh;

typedef struct rise_blender_object {
	const char* name;
	const char* geometry_name;
	const char* material_name;
	float transform[16];
	int casts_shadows;
	int receives_shadows;
	int visible;
	const char* modifier_name;
	const char* interior_medium_name;
} rise_blender_object;

typedef struct rise_blender_light {
	const char* name;
	int type;
	float color[3];
	float intensity;
	float position[3];
	float direction[3];
	float spot_size;
	float spot_blend;
} rise_blender_light;

typedef struct rise_blender_medium {
	const char* name;
	int kind;
	float sigma_a[3];
	float sigma_s[3];
	float emission[3];
	int phase_type;
	float phase_g;
	const char* source_filepath;
	const char* source_grid_name;
	float bbox_min[3];
	float bbox_max[3];
} rise_blender_medium;

typedef struct rise_blender_render_settings {
	uint32_t width;
	uint32_t height;
	uint32_t pixel_samples;
	uint32_t light_samples;
	uint32_t max_recursion;
	int use_path_tracing;
	int choose_one_light;
	int show_lights;

	// SMS — Specular Manifold Sampling (Zeltner 2020 + RISE extensions)
	int sms_enabled;
	uint32_t sms_max_iterations;
	float sms_threshold;
	uint32_t sms_max_chain_depth;
	int sms_biased;
	uint32_t sms_bernoulli_trials;
	uint32_t sms_multi_trials;
	uint32_t sms_photon_count;
	uint32_t sms_max_photon_seeds;
	int sms_two_stage;
	int sms_use_levenberg_marquardt;
	uint32_t sms_seeding_mode;     // rise_blender_sms_seeding
	uint32_t sms_target_bounces;

	// Adaptive sampling
	uint32_t adaptive_max_samples;
	float adaptive_threshold;
	int adaptive_show_map;

	// Path guiding (OpenPGL)
	int path_guiding_enabled;
	uint32_t path_guiding_training_iterations;
	uint32_t path_guiding_training_spp;
	int path_guiding_combine_training;
	int path_guiding_online;
	uint32_t path_guiding_warmup_iterations;
	float path_guiding_alpha;
	int path_guiding_learned_alpha;
	uint32_t path_guiding_max_depth;
	uint32_t path_guiding_max_light_depth;
	uint32_t path_guiding_sampling_type;
	uint32_t path_guiding_ris_candidates;

	// Stability — clamping, RR, bounce limits, light BVH, optimal MIS
	float stability_direct_clamp;
	float stability_indirect_clamp;
	float stability_filter_glossy;
	uint32_t stability_rr_min_depth;
	float stability_rr_threshold;
	uint32_t stability_max_diffuse_bounce;
	uint32_t stability_max_glossy_bounce;
	uint32_t stability_max_transmission_bounce;
	uint32_t stability_max_translucent_bounce;
	uint32_t stability_max_volume_bounce;
	int stability_use_light_bvh;
	int stability_optimal_mis;
	uint32_t stability_optimal_mis_training_iterations;
	uint32_t stability_optimal_mis_tile_size;

	// OIDN — denoiser quality / device / prefilter mode
	int oidn_denoise;
	uint32_t oidn_quality;         // rise_blender_oidn_quality
	uint32_t oidn_device;          // rise_blender_oidn_device
	uint32_t oidn_prefilter;       // rise_blender_oidn_prefilter

	// Progressive multi-pass rendering
	int progressive_enabled;
	uint32_t progressive_samples_per_pass;

	// Sampler — Morton-indexed Sobol (blue-noise error distribution)
	int use_zsobol;

	// Pixel reconstruction filter — anti-aliasing kernel.  Default is
	// gaussian (1.5 px); "none" matches the pre-v4 behaviour.
	uint32_t pixel_filter;             // rise_blender_pixel_filter
	float pixel_filter_width;          // ignored when filter is gaussian / catmull-rom / cubic_bspline / mitchell
	float pixel_filter_param_a;        // gaussian: alpha decay (~2.0); mitchell: B; otherwise 0
	float pixel_filter_param_b;        // mitchell: C; otherwise 0

	const char* temporary_directory;

	// Rasterizer / integrator selection (ABI v5).  See
	// `rise_blender_rasterizer_kind`.  When set to anything other
	// than the legacy PIXELPEL / PT_PEL values, the bridge ignores
	// the `use_path_tracing` boolean above.
	uint32_t rasterizer_kind;          // rise_blender_rasterizer_kind

	// BDPT / VCM / MLT — maximum subpath depths.  Default of 0
	// instructs the bridge to fall back to `max_recursion`.
	uint32_t bidir_max_eye_depth;
	uint32_t bidir_max_light_depth;

	// VCM — photon merging radius (0 = scene-auto fallback) and
	// strategy toggles.  enable_vc + enable_vm both 0 disables VCM
	// entirely; use one to get pure BDPT or pure SPPM behaviour.
	float vcm_merge_radius;
	int vcm_enable_vc;
	int vcm_enable_vm;

	// MLT / PSSMLT — bootstrap / chain / mutation budget.  Defaults
	// (0) get rewritten to sensible production values inside the
	// bridge (n_bootstrap=10000, n_chains=8, mutations_per_pixel=
	// pixel_samples, large_step_prob=0.3).
	uint32_t mlt_bootstrap;
	uint32_t mlt_chains;
	uint32_t mlt_mutations_per_pixel;
	float mlt_large_step_prob;
} rise_blender_render_settings;

typedef struct rise_blender_scene {
	const rise_blender_camera* camera;
	const rise_blender_painter* painters;
	const rise_blender_modifier* modifiers;
	const rise_blender_material* materials;
	const rise_blender_mesh* meshes;
	const rise_blender_object* objects;
	const rise_blender_light* lights;
	const rise_blender_medium* mediums;
	uint32_t num_painters;
	uint32_t num_modifiers;
	uint32_t num_materials;
	uint32_t num_meshes;
	uint32_t num_objects;
	uint32_t num_lights;
	uint32_t num_mediums;
	float world_color[3];
	float world_strength;
	int use_world_ambient;
	const char* global_medium_name;
	// World HDRI / environment map (image-based lighting).  When
	// world_radiance_painter_name is non-null and non-empty, the
	// bridge installs it as the global RadianceMapConfig on the
	// rasterizer.  The painter itself must be registered in
	// rise_blender_scene.painters.
	const char* world_radiance_painter_name;
	float world_radiance_scale;
	float world_radiance_orientation[3];    // radians
	int world_radiance_is_background;
} rise_blender_scene;

typedef struct rise_blender_capabilities {
	uint32_t api_version;
	int supports_oidn;
	int supports_path_guiding;
	int supports_vdb_volumes;
} rise_blender_capabilities;

typedef struct rise_blender_render_result {
	float* rgba;
	uint32_t width;
	uint32_t height;
	// Auto-dispatcher resolution (ABI v8): when the active rasterizer is the
	// auto_rasterizer, is_auto=1 and these report the concrete integrator it
	// resolved to ("pt"/"bdpt"/"vcm") + the one-line reason.  Empty / 0 for a
	// normal rasterizer.
	int is_auto;
	char resolved_integrator[16];
	char resolve_reason[256];
} rise_blender_render_result;

typedef int (*rise_blender_progress_callback)(void* user_data, float progress, const char* title);
typedef int (*rise_blender_image_callback)(
	void* user_data,
	const unsigned short* rgba16,
	uint32_t region_width,
	uint32_t region_height,
	uint32_t full_width,
	uint32_t full_height,
	uint32_t rc_top,
	uint32_t rc_left
);

RISE_BLENDER_EXPORT int rise_blender_api_version(void);

RISE_BLENDER_EXPORT int rise_blender_get_capabilities(
	rise_blender_capabilities* capabilities
);

RISE_BLENDER_EXPORT int rise_blender_render_scene(
	const rise_blender_scene* scene,
	const rise_blender_render_settings* settings,
	rise_blender_progress_callback progress_callback,
	rise_blender_image_callback image_callback,
	void* user_data,
	rise_blender_render_result* result,
	char* error_message,
	size_t error_message_size
);

RISE_BLENDER_EXPORT void rise_blender_free_render_result(rise_blender_render_result* result);

RISE_BLENDER_EXPORT void rise_blender_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
