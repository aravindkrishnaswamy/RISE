from __future__ import annotations

import array
import re
from dataclasses import dataclass, field

import bpy
from bpy_extras.node_shader_utils import PrincipledBSDFWrapper


GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "FONT", "META"}


@dataclass
class RenderSettingsData:
    width: int
    height: int
    pixel_aspect: float
    pixel_samples: int
    light_samples: int
    max_recursion: int
    min_importance: float
    use_path_tracing: bool
    choose_one_light: bool
    use_ior_stack: bool
    show_lights: bool
    use_world_ambient: bool


@dataclass
class CameraData:
    projection_type: int
    location: tuple[float, float, float]
    forward: tuple[float, float, float]
    up: tuple[float, float, float]
    fov_y_radians: float
    ortho_scale: float
    width: int
    height: int
    pixel_aspect: float
    shift_x: float
    shift_y: float


@dataclass
class MaterialData:
    name: str
    base_color: tuple[float, float, float, float]
    emission_color: tuple[float, float, float]
    emission_strength: float
    metallic: float
    roughness: float
    specular: float
    transmission: float
    ior: float
    alpha: float
    double_sided: bool


@dataclass
class MeshData:
    name: str
    vertices: list[float]
    normals: list[float]
    uvs: list[float]
    vertex_indices: list[int]
    normal_indices: list[int]
    uv_indices: list[int]
    double_sided: bool
    use_face_normals: bool

    @property
    def num_vertices(self) -> int:
        return len(self.vertices) // 3

    @property
    def num_normals(self) -> int:
        return len(self.normals) // 3

    @property
    def num_uvs(self) -> int:
        return len(self.uvs) // 3

    @property
    def num_triangles(self) -> int:
        return len(self.vertex_indices) // 3


@dataclass
class ObjectData:
    name: str
    geometry_name: str
    material_name: str
    transform: list[float]
    casts_shadows: bool
    receives_shadows: bool
    visible: bool


@dataclass
class LightData:
    name: str
    type: int
    color: tuple[float, float, float]
    intensity: float
    position: tuple[float, float, float]
    direction: tuple[float, float, float]
    spot_size: float
    spot_blend: float


@dataclass
class SceneData:
    camera: CameraData
    materials: list[MaterialData]
    meshes: list[MeshData]
    objects: list[ObjectData]
    lights: list[LightData]
    world_color: tuple[float, float, float]
    world_strength: float
    use_world_ambient: bool
    warnings: list[str] = field(default_factory=list)


class _ExportState:
    def __init__(self):
        self.materials: list[MaterialData] = []
        self.meshes: list[MeshData] = []
        self.objects: list[ObjectData] = []
        self.name_counts: dict[str, int] = {}
        self.material_map: dict[int | None, MaterialData] = {}
        self.geometry_cache: dict[int, list[tuple[int, str, str]]] = {}
        self.warnings: list[str] = []
        self.warning_set: set[str] = set()


def _warn_once(state: _ExportState, message: str):
    if message not in state.warning_set:
        state.warning_set.add(message)
        state.warnings.append(message)


def _safe_name(name: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z_.-]+", "_", name or "")
    cleaned = cleaned.strip("._")
    return cleaned or "item"


def _unique_name(state: _ExportState, prefix: str, name: str) -> str:
    base = f"{prefix}_{_safe_name(name)}"
    count = state.name_counts.get(base, 0)
    state.name_counts[base] = count + 1
    return base if count == 0 else f"{base}_{count:03d}"


def _pointer_key(id_block):
    if id_block is None:
        return None
    original = getattr(id_block, "original", id_block)
    return original.as_pointer()


def _float_color3(value) -> tuple[float, float, float]:
    return (float(value[0]), float(value[1]), float(value[2]))


def _validate_mesh_payload(mesh: MeshData):
    expected_index_count = len(mesh.vertex_indices)

    if len(mesh.vertices) != mesh.num_vertices * 3:
        raise RuntimeError(
            f"RISE mesh export produced an invalid vertex buffer for '{mesh.name}'."
        )

    if mesh.normals and len(mesh.normals) != mesh.num_normals * 3:
        raise RuntimeError(
            f"RISE mesh export produced an invalid normal buffer for '{mesh.name}'."
        )

    # RISE's indexed triangle mesh API expects texture coordinates with a
    # 3-float stride even though it only consumes the first two components.
    if mesh.uvs and len(mesh.uvs) != mesh.num_uvs * 3:
        raise RuntimeError(
            f"RISE mesh export produced an invalid UV buffer for '{mesh.name}'."
        )

    if expected_index_count != mesh.num_triangles * 3:
        raise RuntimeError(
            f"RISE mesh export produced an invalid triangle index buffer for '{mesh.name}'."
        )

    if mesh.normal_indices and len(mesh.normal_indices) != expected_index_count:
        raise RuntimeError(
            f"RISE mesh export produced an invalid normal index buffer for '{mesh.name}'."
        )

    if mesh.uv_indices and len(mesh.uv_indices) != expected_index_count:
        raise RuntimeError(
            f"RISE mesh export produced an invalid UV index buffer for '{mesh.name}'."
        )

    if mesh.vertex_indices and max(mesh.vertex_indices, default=0) >= mesh.num_vertices:
        raise RuntimeError(
            f"RISE mesh export produced an out-of-range vertex index for '{mesh.name}'."
        )

    if mesh.normal_indices and max(mesh.normal_indices, default=0) >= mesh.num_normals:
        raise RuntimeError(
            f"RISE mesh export produced an out-of-range normal index for '{mesh.name}'."
        )

    if mesh.uv_indices and max(mesh.uv_indices, default=0) >= mesh.num_uvs:
        raise RuntimeError(
            f"RISE mesh export produced an out-of-range UV index for '{mesh.name}'."
        )


def _material_payload(material, state: _ExportState) -> str:
    key = _pointer_key(material)
    if key in state.material_map:
        return state.material_map[key].name

    if material is None:
        payload = MaterialData(
            name="mat_default",
            base_color=(0.8, 0.8, 0.8, 1.0),
            emission_color=(0.0, 0.0, 0.0),
            emission_strength=0.0,
            metallic=0.0,
            roughness=0.5,
            specular=0.5,
            transmission=0.0,
            ior=1.45,
            alpha=1.0,
            double_sided=True,
        )
        state.material_map[key] = payload
        state.materials.append(payload)
        return payload.name

    wrapper = PrincipledBSDFWrapper(material, is_readonly=True) if material.use_nodes and material.node_tree else None
    has_principled = wrapper is not None and wrapper.node_principled_bsdf is not None

    if material.use_nodes and not has_principled:
        _warn_once(
            state,
            "RISE falls back to viewport material values when a material does not use a Principled BSDF output chain.",
        )

    if has_principled:
        base_rgb = _float_color3(wrapper.base_color)
        alpha = float(wrapper.alpha)
        emission_color = _float_color3(wrapper.emission_color)
        emission_strength = float(wrapper.emission_strength)
        metallic = float(wrapper.metallic)
        roughness = float(wrapper.roughness)
        specular = float(wrapper.specular)
        transmission = float(wrapper.transmission)
        ior = float(wrapper.ior)
    else:
        base_rgb = _float_color3(material.diffuse_color)
        alpha = float(material.diffuse_color[3])
        emission_color = (0.0, 0.0, 0.0)
        emission_strength = 0.0
        metallic = float(getattr(material, "metallic", 0.0))
        roughness = float(getattr(material, "roughness", 0.5))
        specular = float(getattr(material, "specular_intensity", 0.5))
        transmission = 0.0
        ior = 1.45

    if alpha < 0.999:
        _warn_once(
            state,
            "RISE currently ignores Blender material alpha and node-based transparency for this sidecar renderer.",
        )

    payload = MaterialData(
        name=_unique_name(state, "mat", material.name_full),
        base_color=(base_rgb[0], base_rgb[1], base_rgb[2], alpha),
        emission_color=emission_color,
        emission_strength=emission_strength,
        metallic=metallic,
        roughness=roughness,
        specular=specular,
        transmission=transmission,
        ior=ior,
        alpha=alpha,
        double_sided=not getattr(material, "use_backface_culling", False),
    )

    state.material_map[key] = payload
    state.materials.append(payload)
    return payload.name


def _flatten_matrix(matrix_world) -> list[float]:
    return [float(value) for row in matrix_world for value in row]


def _camera_payload(camera_object, render_settings: RenderSettingsData, state: _ExportState) -> CameraData:
    if camera_object is None:
        raise RuntimeError("The RISE renderer requires an active scene camera.")

    camera_data = camera_object.data
    if camera_data.type not in {"PERSP", "ORTHO"}:
        _warn_once(
            state,
            f"Camera type '{camera_data.type}' is not supported directly; falling back to a perspective camera.",
        )

    matrix_world = camera_object.matrix_world
    location = matrix_world.translation
    forward = (-matrix_world.col[2].to_3d()).normalized()
    up = matrix_world.col[1].to_3d().normalized()

    projection_type = 1 if camera_data.type == "ORTHO" else 0
    fov_y_radians = float(getattr(camera_data, "angle_y", getattr(camera_data, "angle", 0.78539816339)))

    return CameraData(
        projection_type=projection_type,
        location=(float(location.x), float(location.y), float(location.z)),
        forward=(float(forward.x), float(forward.y), float(forward.z)),
        up=(float(up.x), float(up.y), float(up.z)),
        fov_y_radians=fov_y_radians,
        ortho_scale=float(getattr(camera_data, "ortho_scale", 1.0)),
        width=render_settings.width,
        height=render_settings.height,
        pixel_aspect=render_settings.pixel_aspect,
        shift_x=float(getattr(camera_data, "shift_x", 0.0)),
        shift_y=float(getattr(camera_data, "shift_y", 0.0)),
    )


def _light_payload(light_object, matrix_world, state: _ExportState) -> LightData:
    light = light_object.data
    light_type = {
        "POINT": 0,
        "SPOT": 1,
        "SUN": 2,
        "AREA": 3,
    }.get(light.type, 4)

    if light.type == "AREA":
        _warn_once(
            state,
            "Blender area lights are currently approximated as point lights in the RISE sidecar renderer.",
        )

    color = list(light.color)
    if getattr(light, "use_temperature", False):
        temperature_color = list(light.temperature_color)
        color[0] *= temperature_color[0]
        color[1] *= temperature_color[1]
        color[2] *= temperature_color[2]

    direction = (-matrix_world.col[2].to_3d()).normalized()
    position = matrix_world.translation

    return LightData(
        name=_unique_name(state, "light", light_object.name_full),
        type=light_type,
        color=(float(color[0]), float(color[1]), float(color[2])),
        intensity=float(light.energy) * (2.0 ** float(getattr(light, "exposure", 0.0))),
        position=(float(position.x), float(position.y), float(position.z)),
        direction=(float(direction.x), float(direction.y), float(direction.z)),
        spot_size=float(getattr(light, "spot_size", 0.0)),
        spot_blend=float(getattr(light, "spot_blend", 0.0)),
    )


def _mesh_buckets(eval_object, state: _ExportState) -> list[tuple[int, str, str]]:
    cached = state.geometry_cache.get(eval_object.as_pointer())
    if cached is not None:
        return cached

    mesh = eval_object.to_mesh()
    if mesh is None:
        state.geometry_cache[eval_object.as_pointer()] = []
        return []

    results: list[tuple[int, str, str]] = []
    try:
        mesh.calc_loop_triangles()
        triangle_count = len(mesh.loop_triangles)
        if triangle_count == 0:
            state.geometry_cache[eval_object.as_pointer()] = []
            return []

        vertex_values = array.array("f", [0.0]) * (len(mesh.vertices) * 3)
        mesh.vertices.foreach_get("co", vertex_values)

        normal_values = array.array("f", [0.0]) * (len(mesh.loops) * 3)
        if len(mesh.loops):
            mesh.corner_normals.foreach_get("vector", normal_values)

        uv_values = array.array("f")
        has_uv = mesh.uv_layers.active is not None and len(mesh.uv_layers.active.data) == len(mesh.loops)
        if has_uv:
            raw_uv_values = array.array("f", [0.0]) * (len(mesh.loops) * 2)
            mesh.uv_layers.active.data.foreach_get("uv", raw_uv_values)

            # RISE expects UV coordinates to be packed with a 3-float stride.
            uv_values = array.array("f", [0.0]) * (len(mesh.loops) * 3)
            uv_values[0::3] = raw_uv_values[0::2]
            uv_values[1::3] = raw_uv_values[1::2]

        triangle_vertices = array.array("I", [0]) * (triangle_count * 3)
        triangle_loops = array.array("I", [0]) * (triangle_count * 3)
        triangle_materials = array.array("I", [0]) * triangle_count
        mesh.loop_triangles.foreach_get("vertices", triangle_vertices)
        mesh.loop_triangles.foreach_get("loops", triangle_loops)
        mesh.loop_triangles.foreach_get("material_index", triangle_materials)

        buckets: dict[int, dict[str, list[int]]] = {}
        for tri_index, material_index in enumerate(triangle_materials):
            bucket = buckets.setdefault(
                int(material_index),
                {"vertex_indices": [], "normal_indices": [], "uv_indices": []},
            )
            start = tri_index * 3
            bucket["vertex_indices"].extend(int(v) for v in triangle_vertices[start:start + 3])
            bucket["normal_indices"].extend(int(v) for v in triangle_loops[start:start + 3])
            if has_uv:
                bucket["uv_indices"].extend(int(v) for v in triangle_loops[start:start + 3])

        for material_index, bucket in buckets.items():
            material = None
            if material_index < len(eval_object.material_slots):
                material = eval_object.material_slots[material_index].material

            material_name = _material_payload(material, state)
            material_payload = state.material_map[_pointer_key(material)]
            mesh_name = _unique_name(state, "mesh", f"{eval_object.name_full}_m{material_index}")

            mesh_payload = MeshData(
                name=mesh_name,
                vertices=vertex_values.tolist(),
                normals=normal_values.tolist(),
                uvs=uv_values.tolist(),
                vertex_indices=bucket["vertex_indices"],
                normal_indices=bucket["normal_indices"],
                uv_indices=bucket["uv_indices"],
                double_sided=material_payload.double_sided,
                use_face_normals=False,
            )
            _validate_mesh_payload(mesh_payload)
            state.meshes.append(mesh_payload)
            results.append((material_index, mesh_name, material_name))
    finally:
        eval_object.to_mesh_clear()

    state.geometry_cache[eval_object.as_pointer()] = results
    return results


def build_render_settings(scene) -> RenderSettingsData:
    render = scene.render
    scale = render.resolution_percentage / 100.0
    width = max(int(render.resolution_x * scale), 1)
    height = max(int(render.resolution_y * scale), 1)
    pixel_aspect = float(render.pixel_aspect_x) / max(float(render.pixel_aspect_y), 1e-8)
    rise = scene.rise

    return RenderSettingsData(
        width=width,
        height=height,
        pixel_aspect=pixel_aspect,
        pixel_samples=int(rise.pixel_samples),
        light_samples=int(rise.light_samples),
        max_recursion=int(rise.max_recursion),
        min_importance=float(rise.min_importance),
        use_path_tracing=bool(rise.use_path_tracing),
        choose_one_light=bool(rise.choose_one_light),
        use_ior_stack=bool(rise.use_ior_stack),
        show_lights=bool(rise.show_lights),
        use_world_ambient=bool(rise.use_world_ambient),
    )


def export_scene(depsgraph) -> tuple[SceneData, RenderSettingsData]:
    scene = depsgraph.scene
    render_settings = build_render_settings(scene)
    state = _ExportState()

    camera_object = scene.camera.evaluated_get(depsgraph) if scene.camera else None
    camera = _camera_payload(camera_object, render_settings, state)

    mesh_instances = []
    lights: list[LightData] = []

    for object_instance in depsgraph.object_instances:
        base_object = (
            object_instance.instance_object
            if getattr(object_instance, "is_instance", False) and getattr(object_instance, "instance_object", None)
            else object_instance.object
        )
        if base_object is None:
            continue

        original_object = getattr(base_object, "original", base_object)
        if getattr(original_object, "hide_render", False):
            continue

        if original_object.type in GEOMETRY_TYPES:
            eval_object = object_instance.object
            if eval_object is None:
                continue

            # `DepsgraphObjectInstance` values are only valid while iterating.
            # Snapshot the instance transform and resolve mesh buckets immediately.
            mesh_buckets = _mesh_buckets(eval_object, state)
            if not mesh_buckets:
                continue

            mesh_instances.append(
                {
                    "mesh_buckets": list(mesh_buckets),
                    "object_name": original_object.name_full,
                    "transform": _flatten_matrix(object_instance.matrix_world.copy()),
                }
            )
        elif original_object.type == "LIGHT":
            lights.append(_light_payload(object_instance.object, object_instance.matrix_world.copy(), state))

    for index, mesh_instance in enumerate(mesh_instances):
        mesh_buckets = mesh_instance["mesh_buckets"]
        object_name = mesh_instance["object_name"]
        transform = mesh_instance["transform"]

        for material_index, mesh_name, material_name in mesh_buckets:
            state.objects.append(
                ObjectData(
                    name=_unique_name(state, "obj", f"{object_name}_{index}_m{material_index}"),
                    geometry_name=mesh_name,
                    material_name=material_name,
                    transform=transform,
                    casts_shadows=True,
                    receives_shadows=True,
                    visible=True,
                )
            )

    world_color = (0.0, 0.0, 0.0)
    world_strength = 0.0
    if render_settings.use_world_ambient and scene.world is not None:
        if lights:
            _warn_once(
                state,
                "RISE disables the world ambient approximation when explicit lights are present because the ambient fallback can flatten shading noticeably.",
            )
        else:
            if scene.world.use_nodes:
                _warn_once(
                    state,
                    "RISE currently reduces Blender world nodes to the world viewport color for ambient approximation.",
                )
            world_color = _float_color3(scene.world.color)
            world_strength = 1.0

    scene_data = SceneData(
        camera=camera,
        materials=state.materials,
        meshes=state.meshes,
        objects=state.objects,
        lights=lights,
        world_color=world_color,
        world_strength=world_strength,
        use_world_ambient=render_settings.use_world_ambient,
        warnings=state.warnings,
    )

    return scene_data, render_settings
