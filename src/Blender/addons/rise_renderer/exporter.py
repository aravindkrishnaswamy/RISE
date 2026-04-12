from __future__ import annotations

import array
import os
import re
import tempfile
from dataclasses import dataclass, field

import bpy
from bpy_extras.node_shader_utils import PrincipledBSDFWrapper
from mathutils import Vector


GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "FONT", "META"}

PAINTER_UNIFORM = 0
PAINTER_TEXTURE_PNG = 1
PAINTER_TEXTURE_EXR = 2
PAINTER_TEXTURE_HDR = 3
PAINTER_TEXTURE_TIFF = 4
PAINTER_BLEND = 5

COLOR_SPACE_LINEAR = 0
COLOR_SPACE_SRGB = 1

FILTER_BILINEAR = 1

MODIFIER_BUMP = 0

MATERIAL_LAMBERT = 0
MATERIAL_GGX = 1
MATERIAL_DIELECTRIC = 2

PHASE_ISOTROPIC = 0
PHASE_HG = 1

MEDIUM_HOMOGENEOUS = 0
MEDIUM_HETEROGENEOUS_VDB = 1

SUPPORTED_IMAGE_KINDS = {
    ".png": PAINTER_TEXTURE_PNG,
    ".exr": PAINTER_TEXTURE_EXR,
    ".hdr": PAINTER_TEXTURE_HDR,
    ".tif": PAINTER_TEXTURE_TIFF,
    ".tiff": PAINTER_TEXTURE_TIFF,
}


@dataclass
class RenderSettingsData:
    width: int
    height: int
    pixel_aspect: float
    pixel_samples: int
    light_samples: int
    max_recursion: int
    use_path_tracing: bool
    choose_one_light: bool
    show_lights: bool
    use_world_ambient: bool
    path_branch: bool
    sms_enabled: bool
    sms_max_iterations: int
    sms_threshold: float
    sms_max_chain_depth: int
    sms_biased: bool
    adaptive_max_samples: int
    adaptive_threshold: float
    adaptive_show_map: bool
    path_guiding_enabled: bool
    path_guiding_training_iterations: int
    path_guiding_training_spp: int
    path_guiding_alpha: float
    path_guiding_max_depth: int
    path_guiding_sampling_type: int
    path_guiding_ris_candidates: int
    stability_direct_clamp: float
    stability_indirect_clamp: float
    stability_filter_glossy: float
    stability_rr_min_depth: int
    stability_rr_threshold: float
    stability_max_diffuse_bounce: int
    stability_max_glossy_bounce: int
    stability_max_transmission_bounce: int
    stability_max_translucent_bounce: int
    stability_max_volume_bounce: int
    oidn_denoise: bool
    temporary_directory: str


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
class PainterData:
    name: str
    kind: int
    color: tuple[float, float, float] = (0.0, 0.0, 0.0)
    path: str = ""
    color_space: int = COLOR_SPACE_LINEAR
    filter_type: int = FILTER_BILINEAR
    lowmemory: bool = False
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)
    shift: tuple[float, float, float] = (0.0, 0.0, 0.0)
    painter_a_name: str | None = None
    painter_b_name: str | None = None
    mask_painter_name: str | None = None


@dataclass
class ModifierData:
    name: str
    kind: int
    source_painter_name: str
    scale: float
    window: float


@dataclass
class MaterialData:
    name: str
    model: int
    diffuse_painter_name: str | None = None
    specular_painter_name: str | None = None
    alpha_x_painter_name: str | None = None
    alpha_y_painter_name: str | None = None
    ior_painter_name: str | None = None
    extinction_painter_name: str | None = None
    tau_painter_name: str | None = None
    scatter_painter_name: str | None = None
    emission_painter_name: str | None = None
    double_sided: bool = True


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
    modifier_name: str | None = None
    interior_medium_name: str | None = None


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
class MediumData:
    name: str
    kind: int
    sigma_a: tuple[float, float, float]
    sigma_s: tuple[float, float, float]
    emission: tuple[float, float, float]
    phase_type: int
    phase_g: float
    source_filepath: str = ""
    source_grid_name: str = ""
    bbox_min: tuple[float, float, float] = (0.0, 0.0, 0.0)
    bbox_max: tuple[float, float, float] = (0.0, 0.0, 0.0)


@dataclass
class SceneData:
    camera: CameraData
    painters: list[PainterData]
    modifiers: list[ModifierData]
    materials: list[MaterialData]
    meshes: list[MeshData]
    objects: list[ObjectData]
    lights: list[LightData]
    mediums: list[MediumData]
    world_color: tuple[float, float, float]
    world_strength: float
    use_world_ambient: bool
    global_medium_name: str | None = None
    warnings: list[str] = field(default_factory=list)


@dataclass
class _MaterialBinding:
    surface_material_name: str
    modifier_name: str | None
    interior_medium_name: str | None
    double_sided: bool


@dataclass
class _VolumeSpec:
    sigma_a: tuple[float, float, float]
    sigma_s: tuple[float, float, float]
    emission: tuple[float, float, float]
    phase_type: int
    phase_g: float
    homogeneous_emission_ignored: bool = False


class _ExportState:
    def __init__(self):
        self.painters: list[PainterData] = []
        self.modifiers: list[ModifierData] = []
        self.materials: list[MaterialData] = []
        self.mediums: list[MediumData] = []
        self.meshes: list[MeshData] = []
        self.objects: list[ObjectData] = []
        self.name_counts: dict[str, int] = {}
        self.material_map: dict[int | None, _MaterialBinding] = {}
        self.geometry_cache: dict[int, list[tuple[int, str, _MaterialBinding]]] = {}
        self.warnings: list[str] = []
        self.warning_set: set[str] = set()
        self.painter_cache: dict[tuple, str] = {}
        self.modifier_cache: dict[tuple, str] = {}
        self.medium_cache: dict[tuple, str] = {}


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


def _clamp01(value: float) -> float:
    return max(0.0, min(float(value), 1.0))


def _node_input(node, name: str):
    if node is None:
        return None
    return node.inputs[name] if name in node.inputs else None


def _socket_default_float(node, name: str, default: float) -> float:
    socket = _node_input(node, name)
    if socket is None:
        return default
    return float(socket.default_value)


def _socket_default_color(node, name: str, default: tuple[float, float, float]) -> tuple[float, float, float]:
    socket = _node_input(node, name)
    if socket is None:
        return default
    value = socket.default_value
    return (float(value[0]), float(value[1]), float(value[2]))


def _matrix_world_bounds(matrix_world, local_bound_box) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    transformed = [matrix_world @ Vector(corner) for corner in local_bound_box]
    xs = [point.x for point in transformed]
    ys = [point.y for point in transformed]
    zs = [point.z for point in transformed]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def _texture_color_space(texture_wrapper, image) -> int:
    if getattr(texture_wrapper, "colorspace_is_data", False):
        return COLOR_SPACE_LINEAR
    if getattr(image.colorspace_settings, "is_data", False):
        return COLOR_SPACE_LINEAR
    if getattr(texture_wrapper, "colorspace_name", "") == "Non-Color":
        return COLOR_SPACE_LINEAR
    if image.colorspace_settings.name == "sRGB":
        return COLOR_SPACE_SRGB
    return COLOR_SPACE_LINEAR


def _resolved_image_path(image) -> str | None:
    if image is None:
        return None
    try:
        resolved = bpy.path.abspath(image.filepath, library=image.library)
    except TypeError:
        resolved = bpy.path.abspath(image.filepath)
    if not resolved:
        return None
    return os.path.realpath(resolved)


def _validate_texture_wrapper(texture_wrapper, state: _ExportState, context_name: str) -> tuple[str, int] | None:
    if texture_wrapper is None or texture_wrapper.image is None:
        return None

    image = texture_wrapper.image
    if image.source != "FILE":
        _warn_once(state, f"RISE only supports external file-backed textures for {context_name}.")
        return None
    if image.packed_file is not None:
        _warn_once(state, f"RISE does not support packed textures for {context_name}.")
        return None
    if getattr(texture_wrapper, "has_mapping_node", None) and texture_wrapper.has_mapping_node():
        _warn_once(state, f"RISE currently ignores Mapping nodes for {context_name}.")
        return None
    if getattr(texture_wrapper, "texcoords", "UV") != "UV":
        _warn_once(state, f"RISE currently supports only UV-mapped textures for {context_name}.")
        return None
    if getattr(texture_wrapper, "projection", "FLAT") != "FLAT":
        _warn_once(state, f"RISE currently supports only flat image projection for {context_name}.")
        return None
    if getattr(texture_wrapper, "extension", "REPEAT") != "REPEAT":
        _warn_once(state, f"RISE currently supports only repeat image extension for {context_name}.")
        return None

    filepath = _resolved_image_path(image)
    if not filepath or not os.path.exists(filepath):
        _warn_once(state, f"RISE could not resolve the image file used for {context_name}.")
        return None

    extension = os.path.splitext(filepath)[1].lower()
    painter_kind = SUPPORTED_IMAGE_KINDS.get(extension)
    if painter_kind is None:
        _warn_once(state, f"RISE supports PNG, EXR, HDR, and TIFF textures for {context_name}.")
        return None

    return filepath, _texture_color_space(texture_wrapper, image)


def _add_uniform_painter(
    state: _ExportState,
    label: str,
    color: tuple[float, float, float],
) -> str:
    key = ("uniform", round(color[0], 6), round(color[1], 6), round(color[2], 6))
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", label)
    payload = PainterData(name=name, kind=PAINTER_UNIFORM, color=color)
    state.painters.append(payload)
    state.painter_cache[key] = name
    return name


def _add_texture_painter(
    state: _ExportState,
    label: str,
    filepath: str,
    kind: int,
    color_space: int,
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    shift: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> str:
    key = (
        "texture",
        kind,
        filepath,
        color_space,
        tuple(round(value, 6) for value in scale),
        tuple(round(value, 6) for value in shift),
    )
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", label)
    payload = PainterData(
        name=name,
        kind=kind,
        path=filepath,
        color_space=color_space,
        filter_type=FILTER_BILINEAR,
        lowmemory=False,
        scale=scale,
        shift=shift,
    )
    state.painters.append(payload)
    state.painter_cache[key] = name
    return name


def _add_blend_painter(
    state: _ExportState,
    label: str,
    painter_a_name: str,
    painter_b_name: str,
    mask_painter_name: str,
) -> str:
    key = ("blend", painter_a_name, painter_b_name, mask_painter_name)
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", label)
    payload = PainterData(
        name=name,
        kind=PAINTER_BLEND,
        painter_a_name=painter_a_name,
        painter_b_name=painter_b_name,
        mask_painter_name=mask_painter_name,
    )
    state.painters.append(payload)
    state.painter_cache[key] = name
    return name


def _color_or_texture_painter(
    state: _ExportState,
    label: str,
    color: tuple[float, float, float],
    texture_wrapper=None,
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    shift: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> str:
    validated = _validate_texture_wrapper(texture_wrapper, state, label)
    if validated:
        filepath, color_space = validated
        kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
        return _add_texture_painter(state, label, filepath, kind, color_space, scale=scale, shift=shift)
    scaled = (
        color[0] * scale[0] + shift[0],
        color[1] * scale[1] + shift[1],
        color[2] * scale[2] + shift[2],
    )
    return _add_uniform_painter(state, label, scaled)


def _scalar_or_texture_painter(
    state: _ExportState,
    label: str,
    value: float,
    texture_wrapper=None,
    scale: float = 1.0,
    shift: float = 0.0,
) -> str:
    validated = _validate_texture_wrapper(texture_wrapper, state, label)
    if validated:
        filepath, color_space = validated
        kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
        return _add_texture_painter(
            state,
            label,
            filepath,
            kind,
            color_space,
            scale=(scale, scale, scale),
            shift=(shift, shift, shift),
        )
    scalar = value * scale + shift
    return _add_uniform_painter(state, label, (scalar, scalar, scalar))


def _black_painter(state: _ExportState) -> str:
    return _add_uniform_painter(state, "black", (0.0, 0.0, 0.0))


def _default_surface_binding(state: _ExportState) -> _MaterialBinding:
    key = None
    if key in state.material_map:
        return state.material_map[key]

    base = _add_uniform_painter(state, "default_base", (0.8, 0.8, 0.8))
    payload = MaterialData(
        name="mat_default",
        model=MATERIAL_LAMBERT,
        diffuse_painter_name=base,
        double_sided=True,
    )
    state.materials.append(payload)
    binding = _MaterialBinding(
        surface_material_name=payload.name,
        modifier_name=None,
        interior_medium_name=None,
        double_sided=True,
    )
    state.material_map[key] = binding
    return binding


def _warn_principled_feature(state: _ExportState, material_name: str, feature: str):
    _warn_once(state, f"RISE ignores Principled BSDF {feature} on '{material_name}' in this Blender bridge.")


def _check_principled_feature(state: _ExportState, node, material_name: str, socket_name: str, default, feature: str):
    socket = _node_input(node, socket_name)
    if socket is None:
        return
    if socket.is_linked:
        _warn_principled_feature(state, material_name, feature)
        return
    value = socket.default_value
    if isinstance(default, tuple):
        current = tuple(float(value[index]) for index in range(len(default)))
        if any(abs(current[index] - default[index]) > 1e-6 for index in range(len(default))):
            _warn_principled_feature(state, material_name, feature)
    else:
        if abs(float(value) - float(default)) > 1e-6:
            _warn_principled_feature(state, material_name, feature)


def _warn_unsupported_principled_features(state: _ExportState, material, wrapper: PrincipledBSDFWrapper):
    node = wrapper.node_principled_bsdf
    material_name = material.name_full
    _check_principled_feature(state, node, material_name, "Coat Weight", 0.0, "clearcoat")
    _check_principled_feature(state, node, material_name, "Sheen Weight", 0.0, "sheen")
    _check_principled_feature(state, node, material_name, "Subsurface Weight", 0.0, "subsurface")
    _check_principled_feature(state, node, material_name, "Anisotropy", 0.0, "anisotropy")
    _check_principled_feature(state, node, material_name, "Specular Tint", (0.0, 0.0, 0.0, 1.0), "specular tint")


def _direct_bump_modifier(material, wrapper: PrincipledBSDFWrapper, state: _ExportState) -> str | None:
    node = wrapper.node_principled_bsdf
    normal_input = _node_input(node, "Normal")
    if normal_input is None or not normal_input.is_linked:
        return None

    normal_node = normal_input.links[0].from_node
    if normal_node.bl_idname == "ShaderNodeNormalMap":
        _warn_once(state, f"RISE does not support tangent-space normal maps on '{material.name_full}'.")
        return None
    if normal_node.bl_idname != "ShaderNodeBump":
        _warn_once(state, f"RISE only supports a direct Bump node on '{material.name_full}'.")
        return None

    normal_source = _node_input(normal_node, "Normal")
    if normal_source is not None and normal_source.is_linked:
        _warn_once(state, f"RISE ignores nested normal inputs feeding the Bump node on '{material.name_full}'.")

    height_input = _node_input(normal_node, "Height")
    if height_input is None or not height_input.is_linked:
        _warn_once(state, f"RISE only supports image-driven Bump nodes on '{material.name_full}'.")
        return None

    image_node = height_input.links[0].from_node
    if image_node.bl_idname != "ShaderNodeTexImage":
        _warn_once(state, f"RISE only supports direct image textures as Bump height sources on '{material.name_full}'.")
        return None

    class _ImageSocketWrapper:
        def __init__(self, node_image):
            self.node_image = node_image
            self.image = node_image.image
            self.colorspace_is_data = True
            self.colorspace_name = "Non-Color"

        def has_mapping_node(self):
            vector_input = self.node_image.inputs["Vector"]
            return vector_input.is_linked and vector_input.links[0].from_node.bl_idname == "ShaderNodeMapping"

        @property
        def texcoords(self):
            vector_input = self.node_image.inputs["Vector"]
            if not vector_input.is_linked:
                return "UV"
            return vector_input.links[0].from_socket.name

        @property
        def projection(self):
            return self.node_image.projection

        @property
        def extension(self):
            return self.node_image.extension

    texture_wrapper = _ImageSocketWrapper(image_node)
    validated = _validate_texture_wrapper(texture_wrapper, state, f"the Bump node on '{material.name_full}'")
    if not validated:
        return None

    filepath, color_space = validated
    kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
    painter_name = _add_texture_painter(
        state,
        f"{material.name_full}_bump_height",
        filepath,
        kind,
        color_space,
    )
    strength = _socket_default_float(normal_node, "Strength", 1.0)
    distance = _socket_default_float(normal_node, "Distance", 1.0)
    modifier_key = ("bump", painter_name, round(strength * distance, 6))
    if modifier_key in state.modifier_cache:
        return state.modifier_cache[modifier_key]

    modifier_name = _unique_name(state, "mod", f"{material.name_full}_bump")
    state.modifiers.append(
        ModifierData(
            name=modifier_name,
            kind=MODIFIER_BUMP,
            source_painter_name=painter_name,
            scale=float(strength * distance),
            window=1.0,
        )
    )
    state.modifier_cache[modifier_key] = modifier_name
    return modifier_name


def _parse_volume_socket(socket, state: _ExportState, context_name: str) -> _VolumeSpec | None:
    if socket is None or not socket.is_linked:
        return None
    node = socket.links[0].from_node

    if node.bl_idname == "ShaderNodeAddShader":
        first = _parse_volume_socket(_node_input(node, "Shader"), state, context_name)
        second = _parse_volume_socket(_node_input(node, "Shader_001"), state, context_name)
        if first is None and second is None:
            return None
        if first is None:
            return second
        if second is None:
            return first
        return _VolumeSpec(
            sigma_a=tuple(first.sigma_a[index] + second.sigma_a[index] for index in range(3)),
            sigma_s=tuple(first.sigma_s[index] + second.sigma_s[index] for index in range(3)),
            emission=tuple(first.emission[index] + second.emission[index] for index in range(3)),
            phase_type=first.phase_type if first.phase_type == second.phase_type else PHASE_HG,
            phase_g=(first.phase_g + second.phase_g) * 0.5,
            homogeneous_emission_ignored=first.homogeneous_emission_ignored or second.homogeneous_emission_ignored,
        )

    def reject_linked_input(name: str) -> bool:
        input_socket = _node_input(node, name)
        if input_socket is not None and input_socket.is_linked:
            _warn_once(state, f"RISE only supports constant inputs for {node.name} on {context_name}.")
            return True
        return False

    if node.bl_idname == "ShaderNodeVolumeScatter":
        if reject_linked_input("Color") or reject_linked_input("Density") or reject_linked_input("Anisotropy"):
            return None
        density = _socket_default_float(node, "Density", 1.0)
        color = _socket_default_color(node, "Color", (0.8, 0.8, 0.8))
        g = _socket_default_float(node, "Anisotropy", 0.0)
        return _VolumeSpec(
            sigma_a=(0.0, 0.0, 0.0),
            sigma_s=tuple(color[index] * density for index in range(3)),
            emission=(0.0, 0.0, 0.0),
            phase_type=PHASE_HG if abs(g) > 1e-6 else PHASE_ISOTROPIC,
            phase_g=max(-0.99, min(0.99, g)),
        )

    if node.bl_idname == "ShaderNodeVolumeAbsorption":
        if reject_linked_input("Color") or reject_linked_input("Density"):
            return None
        density = _socket_default_float(node, "Density", 1.0)
        color = _socket_default_color(node, "Color", (0.8, 0.8, 0.8))
        return _VolumeSpec(
            sigma_a=tuple(color[index] * density for index in range(3)),
            sigma_s=(0.0, 0.0, 0.0),
            emission=(0.0, 0.0, 0.0),
            phase_type=PHASE_ISOTROPIC,
            phase_g=0.0,
        )

    if node.bl_idname == "ShaderNodeEmission":
        if reject_linked_input("Color") or reject_linked_input("Strength"):
            return None
        color = _socket_default_color(node, "Color", (1.0, 1.0, 1.0))
        strength = _socket_default_float(node, "Strength", 1.0)
        return _VolumeSpec(
            sigma_a=(0.0, 0.0, 0.0),
            sigma_s=(0.0, 0.0, 0.0),
            emission=tuple(color[index] * strength for index in range(3)),
            phase_type=PHASE_ISOTROPIC,
            phase_g=0.0,
        )

    if node.bl_idname == "ShaderNodeVolumePrincipled":
        for input_name in (
            "Color",
            "Density",
            "Anisotropy",
            "Absorption Color",
            "Emission Strength",
            "Emission Color",
            "Blackbody Intensity",
            "Temperature",
        ):
            if reject_linked_input(input_name):
                return None
        color_attribute = _node_input(node, "Color Attribute")
        if color_attribute is not None and str(color_attribute.default_value).strip():
            _warn_once(state, f"RISE ignores Principled Volume color attributes on {context_name}.")
            return None
        density_attribute = _node_input(node, "Density Attribute")
        if density_attribute is not None:
            density_attribute_name = str(density_attribute.default_value).strip()
            if density_attribute_name not in {"", "density"}:
                _warn_once(state, f"RISE only supports the default density attribute on {context_name}.")
                return None
        density = _socket_default_float(node, "Density", 1.0)
        scatter_color = _socket_default_color(node, "Color", (0.5, 0.5, 0.5))
        absorption_color = _socket_default_color(node, "Absorption Color", (0.0, 0.0, 0.0))
        g = _socket_default_float(node, "Anisotropy", 0.0)
        emission_color = _socket_default_color(node, "Emission Color", (1.0, 1.0, 1.0))
        emission_strength = _socket_default_float(node, "Emission Strength", 0.0)
        if _socket_default_float(node, "Blackbody Intensity", 0.0) > 1e-6:
            _warn_once(state, f"RISE ignores Principled Volume blackbody emission on {context_name}.")
        return _VolumeSpec(
            sigma_a=tuple(absorption_color[index] * density for index in range(3)),
            sigma_s=tuple(scatter_color[index] * density for index in range(3)),
            emission=tuple(emission_color[index] * emission_strength for index in range(3)),
            phase_type=PHASE_HG if abs(g) > 1e-6 else PHASE_ISOTROPIC,
            phase_g=max(-0.99, min(0.99, g)),
        )

    _warn_once(state, f"RISE only supports Volume Scatter, Volume Absorption, Emission, Principled Volume, and Add Shader on {context_name}.")
    return None


def _export_homogeneous_medium(
    state: _ExportState,
    label: str,
    volume_spec: _VolumeSpec,
) -> str | None:
    sigma_a = tuple(max(0.0, float(value)) for value in volume_spec.sigma_a)
    sigma_s = tuple(max(0.0, float(value)) for value in volume_spec.sigma_s)
    emission = tuple(max(0.0, float(value)) for value in volume_spec.emission)
    if max((*sigma_a, *sigma_s, *emission), default=0.0) <= 1e-6:
        return None
    key = (
        "homogeneous",
        tuple(round(value, 6) for value in sigma_a),
        tuple(round(value, 6) for value in sigma_s),
        tuple(round(value, 6) for value in emission),
        volume_spec.phase_type,
        round(volume_spec.phase_g, 6),
    )
    if key in state.medium_cache:
        return state.medium_cache[key]

    name = _unique_name(state, "med", label)
    state.mediums.append(
        MediumData(
            name=name,
            kind=MEDIUM_HOMOGENEOUS,
            sigma_a=sigma_a,
            sigma_s=sigma_s,
            emission=emission,
            phase_type=volume_spec.phase_type,
            phase_g=volume_spec.phase_g,
        )
    )
    state.medium_cache[key] = name
    return name


def _export_heterogeneous_medium(
    state: _ExportState,
    label: str,
    volume_spec: _VolumeSpec,
    filepath: str,
    grid_name: str,
    bbox_min: tuple[float, float, float],
    bbox_max: tuple[float, float, float],
) -> str | None:
    sigma_a = tuple(max(0.0, float(value)) for value in volume_spec.sigma_a)
    sigma_s = tuple(max(0.0, float(value)) for value in volume_spec.sigma_s)
    emission = tuple(max(0.0, float(value)) for value in volume_spec.emission)
    if max((*sigma_a, *sigma_s, *emission), default=0.0) <= 1e-6:
        return None
    key = (
        "heterogeneous",
        filepath,
        grid_name,
        tuple(round(value, 6) for value in sigma_a),
        tuple(round(value, 6) for value in sigma_s),
        tuple(round(value, 6) for value in emission),
        tuple(round(value, 6) for value in bbox_min),
        tuple(round(value, 6) for value in bbox_max),
        volume_spec.phase_type,
        round(volume_spec.phase_g, 6),
    )
    if key in state.medium_cache:
        return state.medium_cache[key]

    name = _unique_name(state, "med", label)
    state.mediums.append(
        MediumData(
            name=name,
            kind=MEDIUM_HETEROGENEOUS_VDB,
            sigma_a=sigma_a,
            sigma_s=sigma_s,
            emission=emission,
            phase_type=volume_spec.phase_type,
            phase_g=volume_spec.phase_g,
            source_filepath=filepath,
            source_grid_name=grid_name,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
        )
    )
    state.medium_cache[key] = name
    return name


def _find_material_output(material):
    if not material.use_nodes or material.node_tree is None:
        return None
    for node in material.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputMaterial" and getattr(node, "is_active_output", False):
            return node
    for node in material.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputMaterial":
            return node
    return None


def _find_world_output(world):
    if not world.use_nodes or world.node_tree is None:
        return None
    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputWorld" and getattr(node, "is_active_output", False):
            return node
    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputWorld":
            return node
    return None


def _material_payload(material, state: _ExportState) -> _MaterialBinding:
    key = _pointer_key(material)
    if key in state.material_map:
        return state.material_map[key]

    if material is None:
        return _default_surface_binding(state)

    if not material.use_nodes or material.node_tree is None:
        base = _add_uniform_painter(state, f"{material.name_full}_base", _float_color3(material.diffuse_color))
        payload = MaterialData(
            name=_unique_name(state, "mat", material.name_full),
            model=MATERIAL_LAMBERT,
            diffuse_painter_name=base,
            double_sided=not getattr(material, "use_backface_culling", False),
        )
        state.materials.append(payload)
        binding = _MaterialBinding(
            surface_material_name=payload.name,
            modifier_name=None,
            interior_medium_name=None,
            double_sided=payload.double_sided,
        )
        state.material_map[key] = binding
        return binding

    wrapper = PrincipledBSDFWrapper(material, is_readonly=True)
    has_principled = wrapper.node_principled_bsdf is not None
    if not has_principled:
        _warn_once(
            state,
            f"RISE falls back to viewport material values when '{material.name_full}' does not use a Principled BSDF output chain.",
        )
        base = _add_uniform_painter(state, f"{material.name_full}_base", _float_color3(material.diffuse_color))
        payload = MaterialData(
            name=_unique_name(state, "mat", material.name_full),
            model=MATERIAL_LAMBERT,
            diffuse_painter_name=base,
            double_sided=not getattr(material, "use_backface_culling", False),
        )
        state.materials.append(payload)
        binding = _MaterialBinding(
            surface_material_name=payload.name,
            modifier_name=None,
            interior_medium_name=None,
            double_sided=payload.double_sided,
        )
        state.material_map[key] = binding
        return binding

    _warn_unsupported_principled_features(state, material, wrapper)

    base_color = _float_color3(wrapper.base_color)
    base_painter = _color_or_texture_painter(
        state,
        f"{material.name_full}_base",
        base_color,
        texture_wrapper=wrapper.base_color_texture,
    )

    metallic = _clamp01(wrapper.metallic)
    metallic_painter = _scalar_or_texture_painter(
        state,
        f"{material.name_full}_metallic",
        metallic,
        texture_wrapper=wrapper.metallic_texture,
    )

    roughness = _clamp01(wrapper.roughness)
    roughness_texture = wrapper.roughness_texture if wrapper.roughness_texture and wrapper.roughness_texture.image else None
    if roughness_texture is not None:
        _warn_once(
            state,
            f"RISE currently uses roughness textures directly for GGX alpha on '{material.name_full}' instead of squaring them.",
        )
    alpha_value = max(roughness * roughness, 1e-4)
    alpha_painter = _scalar_or_texture_painter(
        state,
        f"{material.name_full}_alpha",
        alpha_value,
        texture_wrapper=roughness_texture,
    )

    specular = _clamp01(wrapper.specular)
    dielectric_f0_painter = _scalar_or_texture_painter(
        state,
        f"{material.name_full}_dielectric_f0",
        specular,
        texture_wrapper=wrapper.specular_texture,
        scale=0.08,
    )
    diffuse_painter = _add_blend_painter(
        state,
        f"{material.name_full}_diffuse",
        _black_painter(state),
        base_painter,
        metallic_painter,
    )
    specular_painter = _add_blend_painter(
        state,
        f"{material.name_full}_specular",
        base_painter,
        dielectric_f0_painter,
        metallic_painter,
    )

    transmission_texture = wrapper.transmission_texture if wrapper.transmission_texture and wrapper.transmission_texture.image else None
    transmission = _clamp01(wrapper.transmission)
    if transmission_texture is not None and transmission < 0.05:
        _warn_once(
            state,
            f"RISE treats '{material.name_full}' as a transmissive material because its Transmission input is texture-driven.",
        )
    transmission_heavy = transmission_texture is not None or transmission >= 0.05

    emission_painter = None
    emission_color = _float_color3(wrapper.emission_color)
    emission_strength = max(0.0, float(wrapper.emission_strength))
    emission_color_texture = wrapper.emission_color_texture if wrapper.emission_color_texture and wrapper.emission_color_texture.image else None
    emission_strength_texture = wrapper.emission_strength_texture if wrapper.emission_strength_texture and wrapper.emission_strength_texture.image else None
    if emission_color_texture is not None and emission_strength_texture is not None:
        _warn_once(
            state,
            f"RISE only supports one textured emission input on '{material.name_full}'. The emission strength texture is ignored.",
        )
        emission_strength_texture = None

    if emission_strength > 1e-6 or emission_color_texture is not None or emission_strength_texture is not None:
        if emission_color_texture is not None:
            emission_painter = _color_or_texture_painter(
                state,
                f"{material.name_full}_emission",
                emission_color,
                texture_wrapper=emission_color_texture,
                scale=(emission_strength, emission_strength, emission_strength),
            )
        elif emission_strength_texture is not None:
            emission_painter = _scalar_or_texture_painter(
                state,
                f"{material.name_full}_emission",
                emission_strength,
                texture_wrapper=emission_strength_texture,
                scale=max(emission_color[0], emission_color[1], emission_color[2]),
            )
        elif max(emission_color) > 1e-6:
            emission_painter = _add_uniform_painter(
                state,
                f"{material.name_full}_emission",
                tuple(emission_color[index] * emission_strength for index in range(3)),
            )

    modifier_name = _direct_bump_modifier(material, wrapper, state)
    alpha_socket = _node_input(wrapper.node_principled_bsdf, "Alpha")
    if alpha_socket is not None:
        alpha_value_socket = float(alpha_socket.default_value)
        if alpha_socket.is_linked or alpha_value_socket < 0.999:
            _warn_once(
                state,
                f"RISE currently ignores Principled alpha and alpha textures on '{material.name_full}'.",
            )

    output_node = _find_material_output(material)
    interior_medium_name = None
    if output_node is not None:
        volume_spec = _parse_volume_socket(_node_input(output_node, "Volume"), state, f"the material '{material.name_full}'")
        if volume_spec is not None:
            interior_medium_name = _export_homogeneous_medium(state, f"{material.name_full}_volume", volume_spec)

    double_sided = not getattr(material, "use_backface_culling", False)

    if transmission_heavy:
        if metallic > 1e-3 or (wrapper.metallic_texture and wrapper.metallic_texture.image is not None):
            _warn_once(state, f"RISE ignores metallic on dielectric-transmission Principled material '{material.name_full}'.")
        tau_painter = _add_blend_painter(
            state,
            f"{material.name_full}_tau",
            base_painter,
            _black_painter(state),
            _scalar_or_texture_painter(
                state,
                f"{material.name_full}_transmission",
                transmission,
                texture_wrapper=transmission_texture,
            ),
        )
        ior_painter = _scalar_or_texture_painter(
            state,
            f"{material.name_full}_ior",
            max(1.0, float(wrapper.ior)),
            texture_wrapper=wrapper.ior_texture,
        )
        scatter_painter = _add_uniform_painter(
            state,
            f"{material.name_full}_scatter",
            (max(roughness, 1e-4), max(roughness, 1e-4), max(roughness, 1e-4)),
        )
        payload = MaterialData(
            name=_unique_name(state, "mat", material.name_full),
            model=MATERIAL_DIELECTRIC,
            tau_painter_name=tau_painter,
            ior_painter_name=ior_painter,
            scatter_painter_name=scatter_painter,
            emission_painter_name=emission_painter,
            double_sided=double_sided,
        )
    else:
        ior_painter = _add_uniform_painter(state, f"{material.name_full}_ggx_ior", (2.45, 2.45, 2.45))
        extinction_painter = _add_uniform_painter(state, f"{material.name_full}_ggx_ext", (3.45, 3.45, 3.45))
        payload = MaterialData(
            name=_unique_name(state, "mat", material.name_full),
            model=MATERIAL_GGX,
            diffuse_painter_name=diffuse_painter,
            specular_painter_name=specular_painter,
            alpha_x_painter_name=alpha_painter,
            alpha_y_painter_name=alpha_painter,
            ior_painter_name=ior_painter,
            extinction_painter_name=extinction_painter,
            emission_painter_name=emission_painter,
            double_sided=double_sided,
        )

    state.materials.append(payload)
    binding = _MaterialBinding(
        surface_material_name=payload.name,
        modifier_name=modifier_name,
        interior_medium_name=interior_medium_name,
        double_sided=double_sided,
    )
    state.material_map[key] = binding
    return binding


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


def _validate_mesh_payload(mesh: MeshData):
    expected_index_count = len(mesh.vertex_indices)

    if len(mesh.vertices) != mesh.num_vertices * 3:
        raise RuntimeError(f"RISE mesh export produced an invalid vertex buffer for '{mesh.name}'.")

    if mesh.normals and len(mesh.normals) != mesh.num_normals * 3:
        raise RuntimeError(f"RISE mesh export produced an invalid normal buffer for '{mesh.name}'.")

    if mesh.uvs and len(mesh.uvs) != mesh.num_uvs * 3:
        raise RuntimeError(f"RISE mesh export produced an invalid UV buffer for '{mesh.name}'.")

    if expected_index_count != mesh.num_triangles * 3:
        raise RuntimeError(f"RISE mesh export produced an invalid triangle index buffer for '{mesh.name}'.")

    if mesh.normal_indices and len(mesh.normal_indices) != expected_index_count:
        raise RuntimeError(f"RISE mesh export produced an invalid normal index buffer for '{mesh.name}'.")

    if mesh.uv_indices and len(mesh.uv_indices) != expected_index_count:
        raise RuntimeError(f"RISE mesh export produced an invalid UV index buffer for '{mesh.name}'.")

    if mesh.vertex_indices and max(mesh.vertex_indices, default=0) >= mesh.num_vertices:
        raise RuntimeError(f"RISE mesh export produced an out-of-range vertex index for '{mesh.name}'.")

    if mesh.normal_indices and max(mesh.normal_indices, default=0) >= mesh.num_normals:
        raise RuntimeError(f"RISE mesh export produced an out-of-range normal index for '{mesh.name}'.")

    if mesh.uv_indices and max(mesh.uv_indices, default=0) >= mesh.num_uvs:
        raise RuntimeError(f"RISE mesh export produced an out-of-range UV index for '{mesh.name}'.")


def _mesh_buckets(eval_object, state: _ExportState) -> list[tuple[int, str, _MaterialBinding]]:
    cached = state.geometry_cache.get(eval_object.as_pointer())
    if cached is not None:
        return cached

    mesh = eval_object.to_mesh()
    if mesh is None:
        state.geometry_cache[eval_object.as_pointer()] = []
        return []

    results: list[tuple[int, str, _MaterialBinding]] = []
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

            binding = _material_payload(material, state)
            mesh_name = _unique_name(state, "mesh", f"{eval_object.name_full}_m{material_index}")
            mesh_payload = MeshData(
                name=mesh_name,
                vertices=vertex_values.tolist(),
                normals=normal_values.tolist(),
                uvs=uv_values.tolist(),
                vertex_indices=bucket["vertex_indices"],
                normal_indices=bucket["normal_indices"],
                uv_indices=bucket["uv_indices"],
                double_sided=binding.double_sided,
                use_face_normals=False,
            )
            _validate_mesh_payload(mesh_payload)
            state.meshes.append(mesh_payload)
            results.append((material_index, mesh_name, binding))
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
    temporary_directory = bpy.app.tempdir or tempfile.gettempdir()

    return RenderSettingsData(
        width=width,
        height=height,
        pixel_aspect=pixel_aspect,
        pixel_samples=int(rise.pixel_samples),
        light_samples=int(rise.light_samples),
        max_recursion=int(rise.max_recursion),
        use_path_tracing=bool(rise.use_path_tracing),
        choose_one_light=bool(rise.choose_one_light),
        show_lights=bool(rise.show_lights),
        use_world_ambient=bool(rise.use_world_ambient),
        path_branch=bool(rise.path_branch),
        sms_enabled=bool(rise.sms_enabled),
        sms_max_iterations=int(rise.sms_max_iterations),
        sms_threshold=float(rise.sms_threshold),
        sms_max_chain_depth=int(rise.sms_max_chain_depth),
        sms_biased=bool(rise.sms_biased),
        adaptive_max_samples=int(rise.adaptive_max_samples),
        adaptive_threshold=float(rise.adaptive_threshold),
        adaptive_show_map=bool(rise.adaptive_show_map),
        path_guiding_enabled=bool(rise.path_guiding_enabled),
        path_guiding_training_iterations=int(rise.path_guiding_training_iterations),
        path_guiding_training_spp=int(rise.path_guiding_training_spp),
        path_guiding_alpha=float(rise.path_guiding_alpha),
        path_guiding_max_depth=int(rise.path_guiding_max_depth),
        path_guiding_sampling_type=int(rise.path_guiding_sampling_type),
        path_guiding_ris_candidates=int(rise.path_guiding_ris_candidates),
        stability_direct_clamp=float(rise.stability_direct_clamp),
        stability_indirect_clamp=float(rise.stability_indirect_clamp),
        stability_filter_glossy=float(rise.stability_filter_glossy),
        stability_rr_min_depth=int(rise.stability_rr_min_depth),
        stability_rr_threshold=float(rise.stability_rr_threshold),
        stability_max_diffuse_bounce=int(rise.stability_max_diffuse_bounce),
        stability_max_glossy_bounce=int(rise.stability_max_glossy_bounce),
        stability_max_transmission_bounce=int(rise.stability_max_transmission_bounce),
        stability_max_translucent_bounce=int(rise.stability_max_translucent_bounce),
        stability_max_volume_bounce=int(rise.stability_max_volume_bounce),
        oidn_denoise=bool(rise.oidn_denoise),
        temporary_directory=str(temporary_directory),
    )


def _volume_grid_filepath(volume_data) -> str | None:
    grids = volume_data.grids
    filepath = ""
    try:
        filepath = grids.frame_filepath
    except AttributeError:
        filepath = ""
    if not filepath:
        filepath = getattr(volume_data, "filepath", "")
    if not filepath:
        return None
    return bpy.path.abspath(filepath, library=volume_data.library)


def _density_grid_name(volume_data, state: _ExportState, object_name: str) -> str | None:
    density_grid = None
    for grid in volume_data.grids:
        if grid.name == "density":
            density_grid = grid
            break
        if density_grid is None and grid.data_type in {"FLOAT", "DOUBLE"}:
            density_grid = grid
    if density_grid is None:
        _warn_once(state, f"RISE could not find a float density grid on volume object '{object_name}'.")
        return None
    if density_grid.name != "density":
        _warn_once(state, f"RISE uses grid '{density_grid.name}' as the density source for volume object '{object_name}'.")
    return density_grid.name


def _world_volume_medium(scene, state: _ExportState) -> str | None:
    world = scene.world
    if world is None or not world.use_nodes or world.node_tree is None:
        return None
    output_node = _find_world_output(world)
    if output_node is None:
        return None
    volume_spec = _parse_volume_socket(_node_input(output_node, "Volume"), state, "the World volume output")
    if volume_spec is None:
        return None
    return _export_homogeneous_medium(state, "world_volume", volume_spec)


def _volume_object_medium(base_object, eval_object, matrix_world, state: _ExportState) -> tuple[str | None, ObjectData | None]:
    volume_data = getattr(eval_object, "data", None)
    if volume_data is None or not getattr(volume_data, "grids", None):
        _warn_once(state, f"RISE could not access grids on volume object '{base_object.name_full}'.")
        return None, None

    filepath = _volume_grid_filepath(volume_data)
    if not filepath or not os.path.exists(filepath):
        _warn_once(state, f"RISE could not resolve the VDB filepath for volume object '{base_object.name_full}'.")
        return None, None

    grid_name = _density_grid_name(volume_data, state, base_object.name_full)
    if grid_name is None:
        return None, None

    material = base_object.active_material
    if material is None and base_object.material_slots:
        material = base_object.material_slots[0].material
    if material is None:
        _warn_once(state, f"RISE requires a volume material on volume object '{base_object.name_full}'.")
        return None, None

    output_node = _find_material_output(material)
    if output_node is None:
        _warn_once(state, f"RISE could not find a material output for the volume material on '{base_object.name_full}'.")
        return None, None

    volume_spec = _parse_volume_socket(_node_input(output_node, "Volume"), state, f"the volume material on '{base_object.name_full}'")
    if volume_spec is None:
        return None, None

    bbox_min, bbox_max = _matrix_world_bounds(matrix_world, base_object.bound_box)
    medium_name = _export_heterogeneous_medium(
        state,
        f"{base_object.name_full}_volume",
        volume_spec,
        filepath=filepath,
        grid_name=grid_name,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
    )
    if medium_name is None:
        return None, None

    return medium_name, None


def export_scene(depsgraph) -> tuple[SceneData, RenderSettingsData]:
    scene = depsgraph.scene
    render_settings = build_render_settings(scene)
    state = _ExportState()

    camera_object = scene.camera.evaluated_get(depsgraph) if scene.camera else None
    camera = _camera_payload(camera_object, render_settings, state)

    mesh_instances = []
    lights: list[LightData] = []
    global_medium_name = _world_volume_medium(scene, state)

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
        elif original_object.type == "VOLUME":
            _volume_object_medium(original_object, object_instance.object, object_instance.matrix_world.copy(), state)

    for index, mesh_instance in enumerate(mesh_instances):
        mesh_buckets = mesh_instance["mesh_buckets"]
        object_name = mesh_instance["object_name"]
        transform = mesh_instance["transform"]

        for material_index, mesh_name, binding in mesh_buckets:
            state.objects.append(
                ObjectData(
                    name=_unique_name(state, "obj", f"{object_name}_{index}_m{material_index}"),
                    geometry_name=mesh_name,
                    material_name=binding.surface_material_name,
                    transform=transform,
                    casts_shadows=True,
                    receives_shadows=True,
                    visible=True,
                    modifier_name=binding.modifier_name,
                    interior_medium_name=binding.interior_medium_name,
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
        painters=state.painters,
        modifiers=state.modifiers,
        materials=state.materials,
        meshes=state.meshes,
        objects=state.objects,
        lights=lights,
        mediums=state.mediums,
        world_color=world_color,
        world_strength=world_strength,
        use_world_ambient=render_settings.use_world_ambient,
        global_medium_name=global_medium_name,
        warnings=state.warnings,
    )

    return scene_data, render_settings
