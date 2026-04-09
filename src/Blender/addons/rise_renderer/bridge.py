from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass


_EXPECTED_API_VERSION = 2


class BridgeError(RuntimeError):
    pass


@dataclass(frozen=True)
class BridgeCapabilities:
    api_version: int
    supports_oidn: bool
    supports_path_guiding: bool
    supports_vdb_volumes: bool


@dataclass
class RenderImage:
    width: int
    height: int
    rgba: list[float]


class _Camera(ctypes.Structure):
    _fields_ = [
        ("projection_type", ctypes.c_int),
        ("location", ctypes.c_float * 3),
        ("forward", ctypes.c_float * 3),
        ("up", ctypes.c_float * 3),
        ("fov_y_radians", ctypes.c_float),
        ("ortho_scale", ctypes.c_float),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixel_aspect", ctypes.c_float),
        ("shift_x", ctypes.c_float),
        ("shift_y", ctypes.c_float),
    ]


class _Painter(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("color", ctypes.c_float * 3),
        ("path", ctypes.c_char_p),
        ("color_space", ctypes.c_int),
        ("filter_type", ctypes.c_int),
        ("lowmemory", ctypes.c_int),
        ("scale", ctypes.c_float * 3),
        ("shift", ctypes.c_float * 3),
        ("painter_a_name", ctypes.c_char_p),
        ("painter_b_name", ctypes.c_char_p),
        ("mask_painter_name", ctypes.c_char_p),
    ]


class _Modifier(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("source_painter_name", ctypes.c_char_p),
        ("scale", ctypes.c_float),
        ("window", ctypes.c_float),
    ]


class _Material(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("model", ctypes.c_int),
        ("diffuse_painter_name", ctypes.c_char_p),
        ("specular_painter_name", ctypes.c_char_p),
        ("alpha_x_painter_name", ctypes.c_char_p),
        ("alpha_y_painter_name", ctypes.c_char_p),
        ("ior_painter_name", ctypes.c_char_p),
        ("extinction_painter_name", ctypes.c_char_p),
        ("tau_painter_name", ctypes.c_char_p),
        ("scatter_painter_name", ctypes.c_char_p),
        ("emission_painter_name", ctypes.c_char_p),
        ("double_sided", ctypes.c_int),
    ]


class _Mesh(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("vertices", ctypes.POINTER(ctypes.c_float)),
        ("normals", ctypes.POINTER(ctypes.c_float)),
        ("uvs", ctypes.POINTER(ctypes.c_float)),
        ("vertex_indices", ctypes.POINTER(ctypes.c_uint32)),
        ("normal_indices", ctypes.POINTER(ctypes.c_uint32)),
        ("uv_indices", ctypes.POINTER(ctypes.c_uint32)),
        ("num_vertices", ctypes.c_uint32),
        ("num_normals", ctypes.c_uint32),
        ("num_uvs", ctypes.c_uint32),
        ("num_triangles", ctypes.c_uint32),
        ("double_sided", ctypes.c_int),
        ("use_face_normals", ctypes.c_int),
    ]


class _Object(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("geometry_name", ctypes.c_char_p),
        ("material_name", ctypes.c_char_p),
        ("transform", ctypes.c_float * 16),
        ("casts_shadows", ctypes.c_int),
        ("receives_shadows", ctypes.c_int),
        ("visible", ctypes.c_int),
        ("modifier_name", ctypes.c_char_p),
        ("interior_medium_name", ctypes.c_char_p),
    ]


class _Light(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("type", ctypes.c_int),
        ("color", ctypes.c_float * 3),
        ("intensity", ctypes.c_float),
        ("position", ctypes.c_float * 3),
        ("direction", ctypes.c_float * 3),
        ("spot_size", ctypes.c_float),
        ("spot_blend", ctypes.c_float),
    ]


class _Medium(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("sigma_a", ctypes.c_float * 3),
        ("sigma_s", ctypes.c_float * 3),
        ("emission", ctypes.c_float * 3),
        ("phase_type", ctypes.c_int),
        ("phase_g", ctypes.c_float),
        ("source_filepath", ctypes.c_char_p),
        ("source_grid_name", ctypes.c_char_p),
        ("bbox_min", ctypes.c_float * 3),
        ("bbox_max", ctypes.c_float * 3),
    ]


class _RenderSettings(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixel_samples", ctypes.c_uint32),
        ("light_samples", ctypes.c_uint32),
        ("max_recursion", ctypes.c_uint32),
        ("min_importance", ctypes.c_float),
        ("use_path_tracing", ctypes.c_int),
        ("choose_one_light", ctypes.c_int),
        ("use_ior_stack", ctypes.c_int),
        ("show_lights", ctypes.c_int),
        ("path_branch", ctypes.c_int),
        ("sms_enabled", ctypes.c_int),
        ("sms_max_iterations", ctypes.c_uint32),
        ("sms_threshold", ctypes.c_float),
        ("sms_max_chain_depth", ctypes.c_uint32),
        ("sms_biased", ctypes.c_int),
        ("adaptive_max_samples", ctypes.c_uint32),
        ("adaptive_threshold", ctypes.c_float),
        ("adaptive_show_map", ctypes.c_int),
        ("path_guiding_enabled", ctypes.c_int),
        ("path_guiding_training_iterations", ctypes.c_uint32),
        ("path_guiding_training_spp", ctypes.c_uint32),
        ("path_guiding_alpha", ctypes.c_float),
        ("path_guiding_max_depth", ctypes.c_uint32),
        ("path_guiding_sampling_type", ctypes.c_uint32),
        ("path_guiding_ris_candidates", ctypes.c_uint32),
        ("stability_direct_clamp", ctypes.c_float),
        ("stability_indirect_clamp", ctypes.c_float),
        ("stability_filter_glossy", ctypes.c_float),
        ("stability_rr_min_depth", ctypes.c_uint32),
        ("stability_rr_threshold", ctypes.c_float),
        ("stability_max_diffuse_bounce", ctypes.c_uint32),
        ("stability_max_glossy_bounce", ctypes.c_uint32),
        ("stability_max_transmission_bounce", ctypes.c_uint32),
        ("stability_max_translucent_bounce", ctypes.c_uint32),
        ("stability_max_volume_bounce", ctypes.c_uint32),
        ("oidn_denoise", ctypes.c_int),
        ("temporary_directory", ctypes.c_char_p),
    ]


class _Scene(ctypes.Structure):
    _fields_ = [
        ("camera", ctypes.POINTER(_Camera)),
        ("painters", ctypes.POINTER(_Painter)),
        ("modifiers", ctypes.POINTER(_Modifier)),
        ("materials", ctypes.POINTER(_Material)),
        ("meshes", ctypes.POINTER(_Mesh)),
        ("objects", ctypes.POINTER(_Object)),
        ("lights", ctypes.POINTER(_Light)),
        ("mediums", ctypes.POINTER(_Medium)),
        ("num_painters", ctypes.c_uint32),
        ("num_modifiers", ctypes.c_uint32),
        ("num_materials", ctypes.c_uint32),
        ("num_meshes", ctypes.c_uint32),
        ("num_objects", ctypes.c_uint32),
        ("num_lights", ctypes.c_uint32),
        ("num_mediums", ctypes.c_uint32),
        ("world_color", ctypes.c_float * 3),
        ("world_strength", ctypes.c_float),
        ("use_world_ambient", ctypes.c_int),
        ("global_medium_name", ctypes.c_char_p),
    ]


class _Capabilities(ctypes.Structure):
    _fields_ = [
        ("api_version", ctypes.c_uint32),
        ("supports_oidn", ctypes.c_int),
        ("supports_path_guiding", ctypes.c_int),
        ("supports_vdb_volumes", ctypes.c_int),
    ]


class _RenderResult(ctypes.Structure):
    _fields_ = [
        ("rgba", ctypes.POINTER(ctypes.c_float)),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


_PROGRESS_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_float, ctypes.c_char_p)
_IMAGE_CALLBACK = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint16),
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.c_uint32,
)

_LOADED_LIBRARY = None
_LOADED_PATH = None
_LOADED_CAPABILITIES = None


def _bridge_filenames():
    if sys.platform == "darwin":
        return ["rise_blender_bridge.dylib"]
    if os.name == "nt":
        return ["rise_blender_bridge.dll"]
    return ["rise_blender_bridge.so"]


def resolve_bridge_path(preferred_path: str | None = None) -> str:
    candidates = []

    env_path = os.environ.get("RISE_BLENDER_BRIDGE")
    if env_path:
        candidates.append(env_path)

    if preferred_path:
        candidates.append(preferred_path)

    search_dirs = []
    for base_dir in {
        os.path.dirname(__file__),
        os.path.dirname(os.path.realpath(__file__)),
    }:
        search_dirs.extend(
            [
                base_dir,
                os.path.join(base_dir, "native"),
                os.path.abspath(os.path.join(base_dir, "..", "..", "native")),
            ]
        )

    for filename in _bridge_filenames():
        for search_dir in search_dirs:
            candidates.append(os.path.join(search_dir, filename))

    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            return os.path.abspath(candidate)

    raise BridgeError(
        "The RISE bridge library was not found. Build src/Blender/native and set the add-on Bridge Library path if needed."
    )


def _decode_error(error_buffer) -> str:
    return error_buffer.value.decode("utf-8", errors="replace").strip()


def bridge_status(preferred_path: str | None = None) -> tuple[str | None, str | None]:
    try:
        bridge_path = resolve_bridge_path(preferred_path)
        _load_library(preferred_path)
        return bridge_path, None
    except BridgeError as exc:
        return None, str(exc)


def _load_library(preferred_path: str | None = None):
    global _LOADED_LIBRARY, _LOADED_PATH, _LOADED_CAPABILITIES

    bridge_path = resolve_bridge_path(preferred_path)
    if _LOADED_LIBRARY is not None and _LOADED_PATH == bridge_path:
        return _LOADED_LIBRARY

    try:
        library = ctypes.CDLL(bridge_path)
    except OSError as exc:
        raise BridgeError(f"Failed to load the RISE bridge library at '{bridge_path}': {exc}") from exc

    library.rise_blender_api_version.argtypes = []
    library.rise_blender_api_version.restype = ctypes.c_int
    actual_version = int(library.rise_blender_api_version())
    if actual_version != _EXPECTED_API_VERSION:
        raise BridgeError(
            f"The RISE bridge ABI version at '{bridge_path}' is {actual_version}, but the Blender add-on expects {_EXPECTED_API_VERSION}. "
            "Rebuild src/Blender/native and reload the add-on."
        )

    library.rise_blender_get_capabilities.argtypes = [ctypes.POINTER(_Capabilities)]
    library.rise_blender_get_capabilities.restype = ctypes.c_int
    library.rise_blender_render_scene.argtypes = [
        ctypes.POINTER(_Scene),
        ctypes.POINTER(_RenderSettings),
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(_RenderResult),
        ctypes.POINTER(ctypes.c_char),
        ctypes.c_size_t,
    ]
    library.rise_blender_render_scene.restype = ctypes.c_int
    library.rise_blender_free_render_result.argtypes = [ctypes.POINTER(_RenderResult)]
    library.rise_blender_free_render_result.restype = None
    library.rise_blender_shutdown.argtypes = []
    library.rise_blender_shutdown.restype = None

    raw_capabilities = _Capabilities()
    if not library.rise_blender_get_capabilities(ctypes.byref(raw_capabilities)):
        raise BridgeError(f"The RISE bridge at '{bridge_path}' did not return capabilities.")

    _LOADED_LIBRARY = library
    _LOADED_PATH = bridge_path
    _LOADED_CAPABILITIES = BridgeCapabilities(
        api_version=int(raw_capabilities.api_version),
        supports_oidn=bool(raw_capabilities.supports_oidn),
        supports_path_guiding=bool(raw_capabilities.supports_path_guiding),
        supports_vdb_volumes=bool(raw_capabilities.supports_vdb_volumes),
    )
    return library


def get_capabilities(preferred_path: str | None = None) -> BridgeCapabilities:
    _load_library(preferred_path)
    return _LOADED_CAPABILITIES


class _SceneHandle:
    def __init__(self, scene, settings):
        self.keepalive = []
        self.camera = self._marshal_camera(scene.camera)
        self.painters = self._marshal_array(scene.painters, _Painter, self._marshal_painter)
        self.modifiers = self._marshal_array(scene.modifiers, _Modifier, self._marshal_modifier)
        self.materials = self._marshal_array(scene.materials, _Material, self._marshal_material)
        self.meshes = self._marshal_array(scene.meshes, _Mesh, self._marshal_mesh)
        self.objects = self._marshal_array(scene.objects, _Object, self._marshal_object)
        self.lights = self._marshal_array(scene.lights, _Light, self._marshal_light)
        self.mediums = self._marshal_array(scene.mediums, _Medium, self._marshal_medium)
        self.settings = self._marshal_settings(settings)

        self.scene = _Scene()
        self.scene.camera = ctypes.pointer(self.camera)
        self.scene.painters = self.painters
        self.scene.modifiers = self.modifiers
        self.scene.materials = self.materials
        self.scene.meshes = self.meshes
        self.scene.objects = self.objects
        self.scene.lights = self.lights
        self.scene.mediums = self.mediums
        self.scene.num_painters = len(scene.painters)
        self.scene.num_modifiers = len(scene.modifiers)
        self.scene.num_materials = len(scene.materials)
        self.scene.num_meshes = len(scene.meshes)
        self.scene.num_objects = len(scene.objects)
        self.scene.num_lights = len(scene.lights)
        self.scene.num_mediums = len(scene.mediums)
        self.scene.world_color = (ctypes.c_float * 3)(*scene.world_color)
        self.scene.world_strength = float(scene.world_strength)
        self.scene.use_world_ambient = int(scene.use_world_ambient)
        self.scene.global_medium_name = self._cstring(scene.global_medium_name)

    def _cstring(self, value: str | None):
        if value is None:
            return None
        buffer = ctypes.create_string_buffer(value.encode("utf-8"))
        self.keepalive.append(buffer)
        return ctypes.cast(buffer, ctypes.c_char_p)

    def _float_pointer(self, values):
        if not values:
            return None
        array_type = ctypes.c_float * len(values)
        array_value = array_type(*values)
        self.keepalive.append(array_value)
        return array_value

    def _uint_pointer(self, values):
        if not values:
            return None
        array_type = ctypes.c_uint32 * len(values)
        array_value = array_type(*values)
        self.keepalive.append(array_value)
        return array_value

    def _marshal_array(self, items, ctype, marshaller):
        if not items:
            return None
        array_type = ctype * len(items)
        array_value = array_type(*(marshaller(item) for item in items))
        self.keepalive.append(array_value)
        return array_value

    def _marshal_camera(self, camera):
        payload = _Camera()
        payload.projection_type = int(camera.projection_type)
        payload.location = (ctypes.c_float * 3)(*camera.location)
        payload.forward = (ctypes.c_float * 3)(*camera.forward)
        payload.up = (ctypes.c_float * 3)(*camera.up)
        payload.fov_y_radians = float(camera.fov_y_radians)
        payload.ortho_scale = float(camera.ortho_scale)
        payload.width = int(camera.width)
        payload.height = int(camera.height)
        payload.pixel_aspect = float(camera.pixel_aspect)
        payload.shift_x = float(camera.shift_x)
        payload.shift_y = float(camera.shift_y)
        return payload

    def _marshal_painter(self, painter):
        payload = _Painter()
        payload.name = self._cstring(painter.name)
        payload.kind = int(painter.kind)
        payload.color = (ctypes.c_float * 3)(*painter.color)
        payload.path = self._cstring(painter.path or None)
        payload.color_space = int(painter.color_space)
        payload.filter_type = int(painter.filter_type)
        payload.lowmemory = int(painter.lowmemory)
        payload.scale = (ctypes.c_float * 3)(*painter.scale)
        payload.shift = (ctypes.c_float * 3)(*painter.shift)
        payload.painter_a_name = self._cstring(painter.painter_a_name)
        payload.painter_b_name = self._cstring(painter.painter_b_name)
        payload.mask_painter_name = self._cstring(painter.mask_painter_name)
        return payload

    def _marshal_modifier(self, modifier):
        payload = _Modifier()
        payload.name = self._cstring(modifier.name)
        payload.kind = int(modifier.kind)
        payload.source_painter_name = self._cstring(modifier.source_painter_name)
        payload.scale = float(modifier.scale)
        payload.window = float(modifier.window)
        return payload

    def _marshal_material(self, material):
        payload = _Material()
        payload.name = self._cstring(material.name)
        payload.model = int(material.model)
        payload.diffuse_painter_name = self._cstring(material.diffuse_painter_name)
        payload.specular_painter_name = self._cstring(material.specular_painter_name)
        payload.alpha_x_painter_name = self._cstring(material.alpha_x_painter_name)
        payload.alpha_y_painter_name = self._cstring(material.alpha_y_painter_name)
        payload.ior_painter_name = self._cstring(material.ior_painter_name)
        payload.extinction_painter_name = self._cstring(material.extinction_painter_name)
        payload.tau_painter_name = self._cstring(material.tau_painter_name)
        payload.scatter_painter_name = self._cstring(material.scatter_painter_name)
        payload.emission_painter_name = self._cstring(material.emission_painter_name)
        payload.double_sided = int(material.double_sided)
        return payload

    def _marshal_mesh(self, mesh):
        payload = _Mesh()
        payload.name = self._cstring(mesh.name)
        payload.vertices = self._float_pointer(mesh.vertices)
        payload.normals = self._float_pointer(mesh.normals)
        payload.uvs = self._float_pointer(mesh.uvs)
        payload.vertex_indices = self._uint_pointer(mesh.vertex_indices)
        payload.normal_indices = self._uint_pointer(mesh.normal_indices)
        payload.uv_indices = self._uint_pointer(mesh.uv_indices)
        payload.num_vertices = int(mesh.num_vertices)
        payload.num_normals = int(mesh.num_normals)
        payload.num_uvs = int(mesh.num_uvs)
        payload.num_triangles = int(mesh.num_triangles)
        payload.double_sided = int(mesh.double_sided)
        payload.use_face_normals = int(mesh.use_face_normals)
        return payload

    def _marshal_object(self, obj):
        payload = _Object()
        payload.name = self._cstring(obj.name)
        payload.geometry_name = self._cstring(obj.geometry_name)
        payload.material_name = self._cstring(obj.material_name)
        payload.transform = (ctypes.c_float * 16)(*obj.transform)
        payload.casts_shadows = int(obj.casts_shadows)
        payload.receives_shadows = int(obj.receives_shadows)
        payload.visible = int(obj.visible)
        payload.modifier_name = self._cstring(obj.modifier_name)
        payload.interior_medium_name = self._cstring(obj.interior_medium_name)
        return payload

    def _marshal_light(self, light):
        payload = _Light()
        payload.name = self._cstring(light.name)
        payload.type = int(light.type)
        payload.color = (ctypes.c_float * 3)(*light.color)
        payload.intensity = float(light.intensity)
        payload.position = (ctypes.c_float * 3)(*light.position)
        payload.direction = (ctypes.c_float * 3)(*light.direction)
        payload.spot_size = float(light.spot_size)
        payload.spot_blend = float(light.spot_blend)
        return payload

    def _marshal_medium(self, medium):
        payload = _Medium()
        payload.name = self._cstring(medium.name)
        payload.kind = int(medium.kind)
        payload.sigma_a = (ctypes.c_float * 3)(*medium.sigma_a)
        payload.sigma_s = (ctypes.c_float * 3)(*medium.sigma_s)
        payload.emission = (ctypes.c_float * 3)(*medium.emission)
        payload.phase_type = int(medium.phase_type)
        payload.phase_g = float(medium.phase_g)
        payload.source_filepath = self._cstring(medium.source_filepath or None)
        payload.source_grid_name = self._cstring(medium.source_grid_name or None)
        payload.bbox_min = (ctypes.c_float * 3)(*medium.bbox_min)
        payload.bbox_max = (ctypes.c_float * 3)(*medium.bbox_max)
        return payload

    def _marshal_settings(self, settings):
        payload = _RenderSettings()
        payload.width = int(settings.width)
        payload.height = int(settings.height)
        payload.pixel_samples = int(settings.pixel_samples)
        payload.light_samples = int(settings.light_samples)
        payload.max_recursion = int(settings.max_recursion)
        payload.min_importance = float(settings.min_importance)
        payload.use_path_tracing = int(settings.use_path_tracing)
        payload.choose_one_light = int(settings.choose_one_light)
        payload.use_ior_stack = int(settings.use_ior_stack)
        payload.show_lights = int(settings.show_lights)
        payload.path_branch = int(settings.path_branch)
        payload.sms_enabled = int(settings.sms_enabled)
        payload.sms_max_iterations = int(settings.sms_max_iterations)
        payload.sms_threshold = float(settings.sms_threshold)
        payload.sms_max_chain_depth = int(settings.sms_max_chain_depth)
        payload.sms_biased = int(settings.sms_biased)
        payload.adaptive_max_samples = int(settings.adaptive_max_samples)
        payload.adaptive_threshold = float(settings.adaptive_threshold)
        payload.adaptive_show_map = int(settings.adaptive_show_map)
        payload.path_guiding_enabled = int(settings.path_guiding_enabled)
        payload.path_guiding_training_iterations = int(settings.path_guiding_training_iterations)
        payload.path_guiding_training_spp = int(settings.path_guiding_training_spp)
        payload.path_guiding_alpha = float(settings.path_guiding_alpha)
        payload.path_guiding_max_depth = int(settings.path_guiding_max_depth)
        payload.path_guiding_sampling_type = int(settings.path_guiding_sampling_type)
        payload.path_guiding_ris_candidates = int(settings.path_guiding_ris_candidates)
        payload.stability_direct_clamp = float(settings.stability_direct_clamp)
        payload.stability_indirect_clamp = float(settings.stability_indirect_clamp)
        payload.stability_filter_glossy = float(settings.stability_filter_glossy)
        payload.stability_rr_min_depth = int(settings.stability_rr_min_depth)
        payload.stability_rr_threshold = float(settings.stability_rr_threshold)
        payload.stability_max_diffuse_bounce = int(settings.stability_max_diffuse_bounce)
        payload.stability_max_glossy_bounce = int(settings.stability_max_glossy_bounce)
        payload.stability_max_transmission_bounce = int(settings.stability_max_transmission_bounce)
        payload.stability_max_translucent_bounce = int(settings.stability_max_translucent_bounce)
        payload.stability_max_volume_bounce = int(settings.stability_max_volume_bounce)
        payload.oidn_denoise = int(settings.oidn_denoise)
        payload.temporary_directory = self._cstring(settings.temporary_directory)
        return payload


def render_scene(scene, settings, bridge_path: str | None = None, progress=None, image_update=None) -> RenderImage:
    library = _load_library(bridge_path)
    handle = _SceneHandle(scene, settings)
    error_buffer = ctypes.create_string_buffer(2048)
    result = _RenderResult()

    progress_callback = None
    progress_pointer = None
    if progress is not None:
        def _bridge_progress(_user_data, progress_value, title):
            title_text = title.decode("utf-8") if title else ""
            return 1 if progress(float(progress_value), title_text) else 0

        progress_callback = _PROGRESS_CALLBACK(_bridge_progress)
        progress_pointer = ctypes.cast(progress_callback, ctypes.c_void_p)

    image_callback = None
    image_pointer = None
    if image_update is not None:
        def _bridge_image(_user_data, rgba16, region_width, region_height, full_width, full_height, rc_top, rc_left):
            value_count = int(region_width) * int(region_height) * 4
            rgba16_bytes = ctypes.string_at(rgba16, value_count * ctypes.sizeof(ctypes.c_uint16))
            return 1 if image_update(
                rgba16_bytes,
                int(region_width),
                int(region_height),
                int(full_width),
                int(full_height),
                int(rc_top),
                int(rc_left),
            ) else 0

        image_callback = _IMAGE_CALLBACK(_bridge_image)
        image_pointer = ctypes.cast(image_callback, ctypes.c_void_p)

    success = library.rise_blender_render_scene(
        ctypes.byref(handle.scene),
        ctypes.byref(handle.settings),
        progress_pointer,
        image_pointer,
        None,
        ctypes.byref(result),
        error_buffer,
        len(error_buffer),
    )

    if not success:
        message = _decode_error(error_buffer) or "The RISE bridge failed to render the scene."
        raise BridgeError(message)

    try:
        value_count = int(result.width) * int(result.height) * 4
        if value_count == 0:
            return RenderImage(width=0, height=0, rgba=[])

        array_type = ctypes.c_float * value_count
        rgba = list(ctypes.cast(result.rgba, ctypes.POINTER(array_type)).contents)
        return RenderImage(width=int(result.width), height=int(result.height), rgba=rgba)
    finally:
        library.rise_blender_free_render_result(ctypes.byref(result))


def shutdown():
    if _LOADED_LIBRARY is not None:
        _LOADED_LIBRARY.rise_blender_shutdown()
