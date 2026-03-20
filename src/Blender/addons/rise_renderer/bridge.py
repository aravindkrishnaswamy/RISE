from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass


class BridgeError(RuntimeError):
    pass


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


class _Material(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("base_color", ctypes.c_float * 4),
        ("emission_color", ctypes.c_float * 3),
        ("emission_strength", ctypes.c_float),
        ("metallic", ctypes.c_float),
        ("roughness", ctypes.c_float),
        ("specular", ctypes.c_float),
        ("transmission", ctypes.c_float),
        ("ior", ctypes.c_float),
        ("alpha", ctypes.c_float),
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


class _RenderSettings(ctypes.Structure):
    _fields_ = [
        ("pixel_samples", ctypes.c_uint32),
        ("light_samples", ctypes.c_uint32),
        ("max_recursion", ctypes.c_uint32),
        ("min_importance", ctypes.c_float),
        ("use_path_tracing", ctypes.c_int),
        ("choose_one_light", ctypes.c_int),
        ("use_ior_stack", ctypes.c_int),
        ("show_lights", ctypes.c_int),
    ]


class _Scene(ctypes.Structure):
    _fields_ = [
        ("camera", ctypes.POINTER(_Camera)),
        ("materials", ctypes.POINTER(_Material)),
        ("meshes", ctypes.POINTER(_Mesh)),
        ("objects", ctypes.POINTER(_Object)),
        ("lights", ctypes.POINTER(_Light)),
        ("num_materials", ctypes.c_uint32),
        ("num_meshes", ctypes.c_uint32),
        ("num_objects", ctypes.c_uint32),
        ("num_lights", ctypes.c_uint32),
        ("world_color", ctypes.c_float * 3),
        ("world_strength", ctypes.c_float),
        ("use_world_ambient", ctypes.c_int),
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

    raise BridgeError("The RISE bridge library was not found. Build src/Blender/native and set the add-on Bridge Library path if needed.")


def bridge_status(preferred_path: str | None = None) -> tuple[str | None, str | None]:
    try:
        return resolve_bridge_path(preferred_path), None
    except BridgeError as exc:
        return None, str(exc)


def _load_library(preferred_path: str | None = None):
    global _LOADED_LIBRARY, _LOADED_PATH

    bridge_path = resolve_bridge_path(preferred_path)
    if _LOADED_LIBRARY is not None and _LOADED_PATH == bridge_path:
        return _LOADED_LIBRARY

    library = ctypes.CDLL(bridge_path)
    library.rise_blender_api_version.restype = ctypes.c_int
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
    library.rise_blender_shutdown.argtypes = []

    _LOADED_LIBRARY = library
    _LOADED_PATH = bridge_path
    return library


class _SceneHandle:
    def __init__(self, scene, settings):
        self.keepalive = []
        self.camera = self._marshal_camera(scene.camera)
        self.materials = self._marshal_array(scene.materials, _Material, self._marshal_material)
        self.meshes = self._marshal_array(scene.meshes, _Mesh, self._marshal_mesh)
        self.objects = self._marshal_array(scene.objects, _Object, self._marshal_object)
        self.lights = self._marshal_array(scene.lights, _Light, self._marshal_light)
        self.settings = self._marshal_settings(settings)
        self.scene = _Scene()
        self.scene.camera = ctypes.pointer(self.camera)
        self.scene.materials = self.materials
        self.scene.meshes = self.meshes
        self.scene.objects = self.objects
        self.scene.lights = self.lights
        self.scene.num_materials = len(scene.materials)
        self.scene.num_meshes = len(scene.meshes)
        self.scene.num_objects = len(scene.objects)
        self.scene.num_lights = len(scene.lights)
        self.scene.world_color = (ctypes.c_float * 3)(*scene.world_color)
        self.scene.world_strength = float(scene.world_strength)
        self.scene.use_world_ambient = int(scene.use_world_ambient)

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

    def _marshal_material(self, material):
        payload = _Material()
        payload.name = self._cstring(material.name)
        payload.base_color = (ctypes.c_float * 4)(*material.base_color)
        payload.emission_color = (ctypes.c_float * 3)(*material.emission_color)
        payload.emission_strength = float(material.emission_strength)
        payload.metallic = float(material.metallic)
        payload.roughness = float(material.roughness)
        payload.specular = float(material.specular)
        payload.transmission = float(material.transmission)
        payload.ior = float(material.ior)
        payload.alpha = float(material.alpha)
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
        payload.num_vertices = int(len(mesh.vertices) // 3)
        payload.num_normals = int(len(mesh.normals) // 3)
        # RISE's indexed mesh API expects UV coordinates with a 3-float stride.
        payload.num_uvs = int(len(mesh.uvs) // 3)
        payload.num_triangles = int(len(mesh.vertex_indices) // 3)
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

    def _marshal_settings(self, settings):
        payload = _RenderSettings()
        payload.pixel_samples = int(settings.pixel_samples)
        payload.light_samples = int(settings.light_samples)
        payload.max_recursion = int(settings.max_recursion)
        payload.min_importance = float(settings.min_importance)
        payload.use_path_tracing = int(settings.use_path_tracing)
        payload.choose_one_light = int(settings.choose_one_light)
        payload.use_ior_stack = int(settings.use_ior_stack)
        payload.show_lights = int(settings.show_lights)
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
        message = error_buffer.value.decode("utf-8", errors="replace").strip() or "The RISE bridge failed to render the scene."
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
