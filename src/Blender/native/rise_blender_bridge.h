#ifndef RISE_BLENDER_BRIDGE_H
#define RISE_BLENDER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define RISE_BLENDER_EXPORT __declspec(dllexport)
#else
#define RISE_BLENDER_EXPORT
#endif

#define RISE_BLENDER_API_VERSION 2

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
	RISE_BLENDER_PAINTER_BLEND = 5
};

enum rise_blender_color_space {
	RISE_BLENDER_COLOR_LINEAR = 0,
	RISE_BLENDER_COLOR_SRGB = 1
};

enum rise_blender_modifier_kind {
	RISE_BLENDER_MODIFIER_BUMP = 0
};

enum rise_blender_material_model {
	RISE_BLENDER_MATERIAL_LAMBERT = 0,
	RISE_BLENDER_MATERIAL_GGX = 1,
	RISE_BLENDER_MATERIAL_DIELECTRIC = 2
};

enum rise_blender_phase_type {
	RISE_BLENDER_PHASE_ISOTROPIC = 0,
	RISE_BLENDER_PHASE_HG = 1
};

enum rise_blender_medium_kind {
	RISE_BLENDER_MEDIUM_HOMOGENEOUS = 0,
	RISE_BLENDER_MEDIUM_HETEROGENEOUS_VDB = 1
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
	int path_branch;
	int sms_enabled;
	uint32_t sms_max_iterations;
	float sms_threshold;
	uint32_t sms_max_chain_depth;
	int sms_biased;
	uint32_t adaptive_max_samples;
	float adaptive_threshold;
	int adaptive_show_map;
	int path_guiding_enabled;
	uint32_t path_guiding_training_iterations;
	uint32_t path_guiding_training_spp;
	float path_guiding_alpha;
	uint32_t path_guiding_max_depth;
	uint32_t path_guiding_sampling_type;
	uint32_t path_guiding_ris_candidates;
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
	int oidn_denoise;
	const char* temporary_directory;
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
