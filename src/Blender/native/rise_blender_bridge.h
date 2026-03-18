#ifndef RISE_BLENDER_BRIDGE_H
#define RISE_BLENDER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define RISE_BLENDER_EXPORT __declspec(dllexport)
#else
#define RISE_BLENDER_EXPORT
#endif

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

typedef struct rise_blender_material {
	const char* name;
	float base_color[4];
	float emission_color[3];
	float emission_strength;
	float metallic;
	float roughness;
	float specular;
	float transmission;
	float ior;
	float alpha;
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

typedef struct rise_blender_render_settings {
	uint32_t pixel_samples;
	uint32_t light_samples;
	uint32_t max_recursion;
	float min_importance;
	int use_path_tracing;
	int choose_one_light;
	int use_ior_stack;
	int show_lights;
} rise_blender_render_settings;

typedef struct rise_blender_scene {
	const rise_blender_camera* camera;
	const rise_blender_material* materials;
	const rise_blender_mesh* meshes;
	const rise_blender_object* objects;
	const rise_blender_light* lights;
	uint32_t num_materials;
	uint32_t num_meshes;
	uint32_t num_objects;
	uint32_t num_lights;
	float world_color[3];
	float world_strength;
	int use_world_ambient;
} rise_blender_scene;

typedef struct rise_blender_render_result {
	float* rgba;
	uint32_t width;
	uint32_t height;
} rise_blender_render_result;

typedef int (*rise_blender_progress_callback)(void* user_data, float progress, const char* title);

RISE_BLENDER_EXPORT int rise_blender_api_version(void);

RISE_BLENDER_EXPORT int rise_blender_render_scene(
	const rise_blender_scene* scene,
	const rise_blender_render_settings* settings,
	rise_blender_progress_callback progress_callback,
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
