from __future__ import annotations

import array
import ctypes
import math
import os
import re
import tempfile
from dataclasses import dataclass, field

import bpy
from bpy_extras.node_shader_utils import PrincipledBSDFWrapper
from mathutils import Vector

from . import hair_file_writer, procedural_expr
from .bridge import resolve_rasterizer_selection
from .hair_material_math import (
    anisotropic_rotation_turns_to_radians,
    melanin_to_eumelanin_pheomelanin,
    offset_radians_to_alpha_degrees,
)
from .blender_compat import rna_key, uses_nodes
from .light_math import (
    AREA_LIGHT_DISK_SEGMENTS,
    area_light_exitance,
    area_light_polygon_area,
    unnormalized_power_scale,
)
from .principled_math import (
    THIN_FILM_THICKNESS_CUTOFF_NM,
    dielectric_film_substrate,
    dispersion_keeps_positive_ior,
    effective_abbe_number,
)


GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "FONT", "META"}
# NOTE: Blender's modern hair/fur system uses object type "CURVES"
# (bpy.types.Curves data), distinct from the legacy NURBS/Bezier
# "CURVE" type already in GEOMETRY_TYPES above.  CURVES objects are
# NOT added to GEOMETRY_TYPES -- `to_mesh()` (the whole GEOMETRY_TYPES
# path) doesn't apply to a strand groom, so they're handled by their
# own branch in export_scene() / _export_hair_object() below.

PAINTER_UNIFORM = 0
PAINTER_TEXTURE_PNG = 1
PAINTER_TEXTURE_EXR = 2
PAINTER_TEXTURE_HDR = 3
PAINTER_TEXTURE_TIFF = 4
PAINTER_BLEND = 5
PAINTER_TEXTURE_JPEG = 6
PAINTER_UV_TRANSFORM = 7
PAINTER_EXPRESSION = 8

COLOR_SPACE_LINEAR = 0
COLOR_SPACE_SRGB = 1

FILTER_BILINEAR = 1

MODIFIER_BUMP = 0
MODIFIER_NORMAL_MAP = 1
MODIFIER_RELIEF = 2
MODIFIER_STACK = 3
MODIFIER_BLEND = 4

# Deprecated Stage-A alias retained only for ABI v4 enum completeness;
# post-Stage-B (`RISEPel = Rec709RGBPel`) all non-colour and normal-map
# painters use `COLOR_SPACE_LINEAR` (`0` = `Rec709RGB_Linear`).
COLOR_SPACE_ROMM_LINEAR = 2

MATERIAL_LAMBERT = 0
MATERIAL_GGX = 1
MATERIAL_DIELECTRIC = 2

PHASE_ISOTROPIC = 0
PHASE_HG = 1

MEDIUM_HOMOGENEOUS = 0
MEDIUM_HETEROGENEOUS_VDB = 1

MATERIAL_PBR_METALLIC_ROUGHNESS = 3
# ABI v13 (DL-186, docs/DEBT_LEDGER.md).  A SEPARATE model from
# MATERIAL_PBR_METALLIC_ROUGHNESS, mirroring MATERIAL_DIELECTRIC's own
# mutual exclusivity with it below (transmission_heavy) -- Blender's
# Principled BSDF blends diffuse/SSS by weight rather than layering
# SSS under a specular stack, so RISE models it the same way: one
# surface material, not a wrap.  See rise_blender_bridge.h's own
# comment on the enumerator.
MATERIAL_RANDOMWALK_SSS = 4
# ABI v21 — RISE `mix_material` for native ShaderNodeMixShader trees.
MATERIAL_MIX = 5

# ABI v14 (DL-193, docs/DEBT_LEDGER.md) -- rise_blender_alpha_mode.
ALPHA_MODE_OPAQUE = 0
ALPHA_MODE_CLIP = 1
ALPHA_MODE_BLEND = 2

SUPPORTED_IMAGE_KINDS = {
    ".png": PAINTER_TEXTURE_PNG,
    ".exr": PAINTER_TEXTURE_EXR,
    ".hdr": PAINTER_TEXTURE_HDR,
    ".tif": PAINTER_TEXTURE_TIFF,
    ".tiff": PAINTER_TEXTURE_TIFF,
    ".jpg": PAINTER_TEXTURE_JPEG,
    ".jpeg": PAINTER_TEXTURE_JPEG,
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
    sms_enabled: bool
    sms_max_iterations: int
    sms_threshold: float
    sms_max_chain_depth: int
    sms_biased: bool
    sms_bernoulli_trials: int
    sms_multi_trials: int
    sms_photon_count: int
    sms_max_photon_seeds: int
    sms_two_stage: bool
    sms_use_levenberg_marquardt: bool
    sms_seeding_mode: int
    sms_target_bounces: int
    sms_extended: bool
    adaptive_max_samples: int
    adaptive_threshold: float
    adaptive_show_map: bool
    path_guiding_enabled: bool
    path_guiding_training_iterations: int
    path_guiding_training_spp: int
    path_guiding_combine_training: bool
    path_guiding_online: bool
    path_guiding_warmup_iterations: int
    path_guiding_alpha: float
    path_guiding_learned_alpha: bool
    path_guiding_max_depth: int
    path_guiding_max_light_depth: int
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
    stability_use_light_bvh: bool
    stability_optimal_mis: bool
    stability_optimal_mis_training_iterations: int
    stability_optimal_mis_tile_size: int
    oidn_denoise: bool
    oidn_quality: int
    oidn_device: int
    oidn_prefilter: int
    progressive_enabled: bool
    progressive_samples_per_pass: int
    use_zsobol: bool
    pixel_filter: int
    pixel_filter_width: float
    pixel_filter_param_a: float
    pixel_filter_param_b: float
    temporary_directory: str
    # ABI v5 — rasterizer selection + per-rasterizer config.  Default
    # 0 makes the bridge fall back to the legacy use_path_tracing
    # boolean so older addons keep working.
    rasterizer_kind: int = 0
    bidir_max_eye_depth: int = 0
    bidir_max_light_depth: int = 0
    vcm_merge_radius: float = 0.0
    vcm_enable_vc: bool = True
    vcm_enable_vm: bool = True
    mlt_bootstrap: int = 0
    mlt_chains: int = 0
    mlt_mutations_per_pixel: int = 0
    mlt_large_step_prob: float = 0.0
    # ABI v19 — render region (top-down inclusive RISE film coords) and
    # transparent film background (`scene.render.film_transparent`).
    use_region: bool = False
    region_left: int = 0
    region_top: int = 0
    region_right: int = 0
    region_bottom: int = 0
    film_transparent: bool = False


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
    # ABI v7 — realistic camera fields.  When ``use_dof`` is non-zero the
    # bridge dispatches to AddThinlensCamera with full optical model;
    # otherwise the pinhole path is taken (preserving v6 behaviour bit-
    # identically when these fields are zero).  Distances in scene units;
    # ``scene_unit_meters`` is Blender's ``unit_settings.scale_length``.
    focal_length_mm: float = 50.0
    sensor_width_mm: float = 36.0
    sensor_height_mm: float = 24.0
    sensor_fit_vertical: int = 0
    use_dof: int = 0
    fstop: float = 0.0
    focus_distance: float = 0.0
    aperture_blades: int = 0
    aperture_rotation_radians: float = 0.0
    aperture_ratio: float = 1.0
    iso: float = 0.0
    scene_unit_meters: float = 1.0
    # ABI v23 — planar near-clip distance (Blender camera.data.clip_start).
    clip_start: float = 0.0


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
    # UV-transform painter (kind=PAINTER_UV_TRANSFORM).  `painter_a_name`
    # is the source painter; these five carry the affine transform.
    uv_offset_u: float = 0.0
    uv_offset_v: float = 0.0
    uv_rotation: float = 0.0
    uv_scale_u: float = 1.0
    uv_scale_v: float = 1.0
    # ABI v19 — Expression painter (kind=PAINTER_EXPRESSION).
    expression: str | None = None
    # ABI v23 — Texture wrap mode (0=clamp, 1=repeat, 2=mirrored_repeat).
    wrap_mode: int = 0


@dataclass
class ModifierData:
    name: str
    kind: int
    source_painter_name: str | None = None
    scale: float = 1.0
    window: float = 0.0
    # ABI v11.  MODIFIER_BUMP only: True selects the bridge's
    # window-INDEPENDENT amplitude fold (RISE_API_CreateBumpMapModifierEx's
    # normalizeGradient=true, `scale' = -scale`) instead of the legacy
    # window-COUPLED one (`scale' = -scale*2*window`) that `IJob::
    # AddBumpMapModifier` is ABI-frozen to.  Ignored by MODIFIER_NORMAL_MAP.
    normalize: bool = False
    # ABI v23 — MODIFIER_STACK / MODIFIER_BLEND fields.
    stack_member_names: tuple[str, ...] = ()
    blend_a_name: str | None = None
    blend_b_name: str | None = None
    blend_weight_painter_name: str | None = None


@dataclass
class MixChildData:
    material_name: str | None
    weight_painter_name: str
    modifier_name: str | None = None


@dataclass
class MeshAttributeData:
    name: str
    rgba: list[float]


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
    # PBR Metallic-Roughness slots
    base_color_painter_name: str | None = None
    metallic_painter_name: str | None = None
    roughness_painter_name: str | None = None
    specular_factor_painter_name: str | None = None
    specular_color_painter_name: str | None = None
    anisotropy_factor_painter_name: str | None = None
    anisotropy_rotation_painter_name: str | None = None
    emissive_scale: float = 1.0
    # ABI v12 (DL-18, docs/DEBT_LEDGER.md; source heading
    # CLOTH_FABRIC_DESIGN.md §15 item 13).  PBR_METALLIC_ROUGHNESS only:
    # `sheen_color_painter_name` set (non-None) means the bridge wraps
    # this PBR material in a `fabric_material` sheen layer -- None (the
    # default) means no sheen, bit-identical to a pre-v12 payload.
    # `sheen_roughness` is the numeric Charlie-alpha fallback;
    # `sheen_roughness_texture_painter_name` is set ONLY when Sheen
    # Roughness is texture-driven (the one v12 texture exception,
    # mirroring HairMaterialData's v10 beta_m/beta_n/ior fields above).
    sheen_color_painter_name: str | None = None
    sheen_roughness: float = 0.5
    sheen_roughness_texture_painter_name: str | None = None
    # ABI v13 (DL-186, docs/DEBT_LEDGER.md; source heading DL-151's own
    # sibling audit).  PBR_METALLIC_ROUGHNESS only.  `coat_weight_-
    # painter_name` set (non-None) means the bridge wraps this PBR
    # material in a `coated_material` layer -- None (the default)
    # means no coat, bit-identical to a pre-v13 payload.  Mirrors the
    # sheen fields immediately above: `coat_weight`/`coat_roughness`
    # are numeric fallbacks, the `*_texture_painter_name` fields are
    # set ONLY when the corresponding Blender socket is texture-driven.
    # `coat_tint_painter_name` is a colour painter name (None = no
    # tint).  `coat_ior` is a plain numeric IOR, socket-default only.
    coat_weight_texture_painter_name: str | None = None
    coat_weight: float = 0.0
    coat_tint_painter_name: str | None = None
    coat_roughness_texture_painter_name: str | None = None
    coat_roughness: float = 0.03
    coat_ior: float = 1.5
    # ABI v13 (DL-186).  Set together with `model=MATERIAL_RANDOMWALK_-
    # SSS` (a SEPARATE model -- see that constant's own comment above).
    # `subsurface_absorption`/`subsurface_scattering` are ALREADY-
    # CONVERTED inline "r g b" numeric literal strings (the Radius/
    # Scale/base-colour -> sigma_a/sigma_s conversion happens in
    # `_material_payload` below, in Python, because that is where the
    # raw untextured base-colour triple this conversion needs is
    # already in hand -- see docs/BLENDER_MATERIAL_TRANSLATION.md
    # "Subsurface").
    subsurface_absorption: str | None = None
    subsurface_scattering: str | None = None
    subsurface_ior: float = 1.4
    subsurface_g: float = 0.0
    subsurface_roughness: float = 0.0
    # ABI v14 (DL-192, docs/DEBT_LEDGER.md).  PBR_METALLIC_ROUGHNESS
    # ONLY, and only meaningful when `coat_weight_texture_painter_name`/
    # `coat_weight` above actually contribute a coat layer: Principled
    # BSDF's Coat Normal socket (a Normal Map node, textured) ->
    # `coated_material`'s coat-lobe-only tangent-space normal map.
    # None (default) = no perturbation, bit-identical to a pre-v14
    # payload.  `coat_normal_scale` is the Normal Map node's own
    # "Strength" slider.
    coat_normal_painter_name: str | None = None
    coat_normal_scale: float = 1.0
    # ABI v14 (DL-193, docs/DEBT_LEDGER.md).  Principled BSDF's Alpha
    # socket.  `alpha_mode` (RISE_BLENDER_ALPHA_*, bridge.py) selects
    # OPAQUE (default -- the other three fields are ignored, bit-
    # identical to a pre-v14 payload) / CLIP / BLEND; see
    # docs/BLENDER_MATERIAL_TRANSLATION.md "Alpha".
    alpha: float = 1.0
    alpha_texture_painter_name: str | None = None
    alpha_mode: int = 0
    alpha_threshold: float = 0.5
    # ABI v18.  Principled "Thin Film Thickness" (nm) / "Thin Film IOR"
    # (Blender 4.2+).  thickness <= 0 = no film (bit-identical to v17).
    # PBR path: the base becomes a `ggx_material` in `thinfilm` Fresnel
    # mode over a dielectric (Specular-IOR-Level-adjusted index) or a
    # metal (F0 + F82 tint, turned
    # into n(lambda), k(lambda) by the native bridge); DIELECTRIC path: a
    # one-layer AR coating.  See rise_blender_bridge.h.
    thin_film_thickness: float = 0.0
    thin_film_ior: float = 1.33
    thin_film_conductor: bool = False
    thin_film_dielectric_ior: float = 1.5
    thin_film_metal_f0: tuple[float, float, float] = (1.0, 1.0, 1.0)
    thin_film_metal_tint: tuple[float, float, float] = (1.0, 1.0, 1.0)
    # ABI v18.  Principled "Transmission Dispersion Abbe Number" /
    # "Transmission Dispersion Scale" (Blender 5.3, OpenPBR 1.1.1),
    # folded to the effective V_d = abbe / scale.  DIELECTRIC only;
    # <= 0 = no dispersion.
    dispersion_abbe: float = 0.0
    # ABI v21 — RISE_BLENDER_MATERIAL_MIX (`mix_material`).
    mix_children: list[MixChildData] = field(default_factory=list)
    mix_passthrough_painter_name: str | None = None


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
    tangent_attribute: list[float] = field(default_factory=list)
    tangent_is_shader_direction: bool = False
    # ABI v21 — Named per-corner RGBA attributes (`mesh_attr3` / `mesh_attr_alpha`).
    named_attributes: list[MeshAttributeData] = field(default_factory=list)

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
    # ABI v14 (DL-193, docs/DEBT_LEDGER.md): the alpha-aware
    # `advanced_shader` name `_material_payload` registered for the
    # bound material (via `_MaterialBinding.shader_name`), or None for
    # an opaque material -- see `rise_blender_object.shader_name`'s own
    # comment in rise_blender_bridge.h for the full contract.
    shader_name: str | None = None
    # ABI v20 — Per-object Cycles Object Info attributes (`osl_object_*`).
    object_color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0)
    object_index: float = 0.0
    object_random: float = 0.0
    # ABI v23 — Per-particle-instance random (`osl_particle_random()`).
    particle_random: float = 0.0


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
class HairMaterialData:
    """Mirrors the `hair_material` scene chunk (Chiang et al. 2016)
    parameter set -- see ChunkParserRegistry.cpp's
    HairMaterialAsciiChunkParser -- one-to-one, in terms of the
    exporter's own painter-reference model.  `tier` names which ONE of
    the three colour tiers is bound (`hair_material` itself enforces
    "exactly one"): "melanin" (eumelanin/pheomelanin painters set),
    "sigma_a" (sigma_a painter set), or "color" (color painter set).

    TWO REPRESENTATIONS, AND WHY.  Most parameters appear twice: as a
    painter NAME (the `*_painter_name` fields, registered in
    `state.painters`) and, for everything except `color`, as a plain
    NUMBER.  The live-render bridge consumes the NUMBERS.  That is not
    redundancy for its own sake -- `hair_material`'s `sigma_a` /
    `eumelanin` / `pheomelanin` / `beta_m` / `beta_n` / `alpha` / `ior`
    are `IScalarPainter` slots (docs/ISCALARPAINTER_REFACTOR.md), and
    `Job::AddHairMaterial` rejects a plain `IPainter` name bound to one
    of them outright.  Inline numeric literals ARE accepted there, so
    numbers are what the bridge carries by default.  `color` is a
    genuine `IPainter` slot and keeps full texture support.

    THE THREE EXCEPTIONS (ABI v10).  `beta_m_texture_painter_name` /
    `beta_n_texture_painter_name` / `ior_texture_painter_name` are set
    ONLY when a real texture chain resolved on the corresponding
    Blender socket, and the native bridge wraps the named colour painter
    into an `IScalarPainter` (`PainterChannelScalarPainter`, channel R)
    before handing its name to `AddHairMaterial`.  So a texture-driven
    Roughness / Radial Roughness / IOR now reaches the renderer as a
    real spatially-varying value.  When no texture is bound the field
    stays `None` and the numeric field is used verbatim, unchanged from
    before -- no extra painter, no extra indirection.

    What still flattens to a constant: `eumelanin` / `pheomelanin` /
    `sigma_a` (a melanin or absorption image has no defined
    concentration-per-texel convention in RISE, unlike a roughness or
    IOR map which is read as a literal physical value) and `alpha`
    (Blender's `Offset` socket is never texture-sampled here -- a linked
    Offset is read at its default and warned about).  See
    docs/BLENDER_MATERIAL_TRANSLATION.md's hair section.
    """

    name: str
    tier: str
    eumelanin_painter_name: str | None = None
    pheomelanin_painter_name: str | None = None
    sigma_a_painter_name: str | None = None
    color_painter_name: str | None = None
    alpha_painter_name: str | None = None
    # ABI v10 -- set ONLY when the socket is genuinely texture-driven.
    beta_m_texture_painter_name: str | None = None
    beta_n_texture_painter_name: str | None = None
    ior_texture_painter_name: str | None = None
    # Numeric form -- what `bridge.py` marshals into the native ABI.
    eumelanin: float = 0.0
    pheomelanin: float = 0.0
    sigma_a: tuple[float, float, float] = (0.0, 0.0, 0.0)
    beta_m: float = 0.3
    beta_n: float = 0.3
    alpha_degrees: float = 2.0
    ior: float = 1.55
    # Melanin tier only.  When true (the default) the native bridge
    # rescales the two concentrations for Blender visual parity; clear
    # it to get RISE's own OMLC-anchored absorption instead.
    apply_melanin_parity_rescale: bool = True


@dataclass
class HairObjectData:
    """One hair/fur groom exported from a Blender Curves object: a
    binary `.hair` file (Cem Yuksel format, written by
    `hair_file_writer.write_hair_file`) staged to disk, plus the
    object/transform/material binding the bridge turns into an
    `AddHairGeometryFromFile` + `AddObject` pair on the RISE side (ABI
    v9 -- `rise_blender_hair_object`).  The geometry itself is
    registered natively under `<name>::hairgeom`; nothing here names it,
    because a `.hair` file is written per Curves object and so there is
    no groom to share.
    """

    name: str
    file_path: str
    material_name: str
    transform: list[float]
    width_root_scale: float = 1.0
    width_tip_scale: float = 1.0
    source_object_name: str = ""


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
    world_radiance_painter_name: str | None = None
    world_radiance_scale: float = 1.0
    world_radiance_orientation: tuple[float, float, float] = (0.0, 0.0, 0.0)
    world_radiance_is_background: bool = True
    warnings: list[str] = field(default_factory=list)
    # Hair/fur grooms -- see HairObjectData / HairMaterialData above.
    hair_objects: list[HairObjectData] = field(default_factory=list)
    hair_materials: list[HairMaterialData] = field(default_factory=list)


@dataclass
class _MaterialBinding:
    surface_material_name: str
    modifier_name: str | None
    interior_medium_name: str | None
    double_sided: bool
    # ABI v14 (DL-193, docs/DEBT_LEDGER.md): the `<name>.shader`
    # advanced_shader `_material_payload` registered when Alpha is
    # active, or None for an opaque material -- computed once per
    # material (mirrors `modifier_name`'s own per-material-computed,
    # per-object-applied shape) and threaded into every `ObjectData`
    # bound to this material.
    shader_name: str | None = None
    # ABI v21 — Named geometry attributes referenced by this material's
    # expression painters (`mesh_attr3` / `mesh_attr_alpha`).
    mesh_attributes: tuple[str, ...] = ()


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
        # Cache of unpacked (or otherwise resolved-to-disk) image
        # filepaths, keyed by the image's RNA pointer (rna_key).  Some scenes embed
        # the HDRI / texture inside the .blend; we transparently
        # round-trip them through a temp file because RISE's painter
        # loaders only accept filepaths.
        self.image_path_cache: dict[int, str] = {}
        self.hair_objects: list[HairObjectData] = []
        self.hair_materials: list[HairMaterialData] = []
        # Keyed by `_pointer_key(material)` (int | None) for real
        # materials, plus the string sentinel `"__default_hair__"` that
        # `_default_hair_material` caches its shared fallback under.
        self.hair_material_map: dict[int | str | None, str] = {}


def _warn_once(state: _ExportState, message: str):
    if message not in state.warning_set:
        state.warning_set.add(message)
        state.warnings.append(message)


def _safe_name(name: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z_.-]+", "_", name or "")
    cleaned = cleaned.strip("._")
    return cleaned or "item"


def _safe_ident(name: str) -> str:
    """Identifier-safe name (`[A-Za-z0-9_]+`, no `.` or `-`) so painter names
    are always valid bare identifiers in RISE `sample(name)` / `sample_uv(...)`."""
    cleaned = re.sub(r"[^0-9A-Za-z_]+", "_", name or "").strip("_")
    return cleaned or "item"


def _tex_image_wrap_mode(image_node) -> int:
    """Map Blender `ShaderNodeTexImage.extension` to RISE `eRasterWrapMode`
    (0 = ClampToEdge, 1 = Repeat, 2 = MirroredRepeat)."""
    ext = getattr(image_node, "extension", "REPEAT")
    if ext == "REPEAT":
        return 1
    if ext == "MIRROR":
        return 2
    return 0


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
    cs = getattr(image, "colorspace_settings", None)
    if cs is not None:
        if getattr(cs, "is_data", False):
            return COLOR_SPACE_LINEAR
        cs_name = getattr(cs, "name", "")
        if cs_name == "sRGB":
            return COLOR_SPACE_SRGB
        if cs_name:
            return COLOR_SPACE_LINEAR
    if getattr(texture_wrapper, "colorspace_is_data", False):
        return COLOR_SPACE_LINEAR
    if getattr(texture_wrapper, "colorspace_name", "") == "Non-Color":
        return COLOR_SPACE_LINEAR
    if getattr(texture_wrapper, "colorspace_name", "") == "sRGB":
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


_PACKED_UNPACK_DIR_NAME = "rise_blender_unpack"


def _unpack_image_to_disk(image, state: _ExportState | None) -> str | None:
    """Write a packed bpy.types.Image's raw bytes to a temp file so the
    RISE bridge (filepath-based loaders) can read it.

    For .hdr / .exr / .png / .jpg / .tiff sources the packed bytes ARE
    the original file content — we just dump them verbatim.  Result is
    cached per state so repeated references to the same image don't
    re-write the file.
    """
    if image is None or image.packed_file is None:
        return None

    cache = state.image_path_cache if state is not None else None
    if cache is not None:
        cached = cache.get(rna_key(image))
        if cached and os.path.exists(cached):
            return cached

    extension = os.path.splitext(image.filepath_raw or image.name or "")[1].lower()
    if extension not in SUPPORTED_IMAGE_KINDS:
        # No usable extension on filepath — guess from the image's
        # detected format if available.  Default to .hdr because
        # packed env textures are by far the most common case.
        format_name = getattr(image, "file_format", "") or ""
        format_map = {
            "HDR": ".hdr",
            "OPEN_EXR": ".exr",
            "OPEN_EXR_MULTILAYER": ".exr",
            "PNG": ".png",
            "JPEG": ".jpg",
            "JPEG2000": ".jpg",
            "TIFF": ".tif",
        }
        extension = format_map.get(format_name.upper(), ".hdr")

    out_dir = os.path.join(bpy.app.tempdir or tempfile.gettempdir(), _PACKED_UNPACK_DIR_NAME)
    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError:
        return None

    safe_stem = _safe_name(image.name_full or image.name or "image")
    out_path = os.path.join(out_dir, f"{safe_stem}_{rna_key(image):x}{extension}")

    try:
        # PackedFile.data is a bytes-like attribute in Blender 3.x+;
        # `bytes(...)` works on it directly.
        with open(out_path, "wb") as handle:
            handle.write(bytes(image.packed_file.data))
    except OSError as exc:
        if state is not None:
            _warn_once(state, f"RISE could not unpack image '{image.name_full}' to disk: {exc}.")
        return None

    if cache is not None:
        cache[rna_key(image)] = out_path
    return out_path


def _resolve_image_for_bridge(image, state: _ExportState | None) -> str | None:
    """Return an on-disk filepath for an image, unpacking it if needed.

    Priority:
        1. The user-supplied filepath, if it exists on disk.
        2. The packed bytes round-tripped through a temp file.
    """
    direct = _resolved_image_path(image)
    if direct and os.path.exists(direct):
        return direct
    if image is not None and image.packed_file is not None:
        return _unpack_image_to_disk(image, state)
    return direct  # may be a missing-path string for downstream warning


_MAPPING_VECTOR_TYPE_SUPPORTED = {"POINT", "TEXTURE"}


def _extract_mapping_transform(image_node, state: _ExportState | None, context_name: str):
    """Inspect the chain feeding `image_node.Vector` for a
    ShaderNodeMapping → ShaderNodeTexCoord (UV) pair, and return the
    affine UV transform tuple `(offset_u, offset_v, rotation_z,
    scale_u, scale_v)` ready to feed RISE's UVTransformPainter.

    Returns:
        - the transform tuple when an unlinked Mapping node sits
          between the Image and a Texture Coordinate → UV chain
        - ``None`` when there's no Mapping node (caller skips the
          UV-wrap step entirely)
        - ``"reject"`` when the chain is present but unsupported
          (linked socket values, non-POINT/TEXTURE vector_type,
          non-UV source) — caller should fall back to no-texture
    """

    vector_input = image_node.inputs.get("Vector") if "Vector" in image_node.inputs else None
    if vector_input is None or not vector_input.is_linked:
        return None
    vlinks = [lk for lk in vector_input.links if not getattr(lk, "is_muted", False)]
    if not vlinks:
        return None

    src, _src_sock = _skip_reroutes_and_muted_with_socket(vlinks[0].from_node, vlinks[0].from_socket)
    if src is None or src.bl_idname != "ShaderNodeMapping":
        # Direct Texture Coordinate or similar — texcoords check
        # downstream handles UV-vs-other.  No transform to extract.
        return None

    mapping = src
    vec_type = getattr(mapping, "vector_type", "POINT")
    if vec_type not in _MAPPING_VECTOR_TYPE_SUPPORTED:
        _warn_once(state, f"RISE supports only POINT / TEXTURE Mapping nodes for {context_name} (got '{vec_type}').")
        return "reject"

    # Mapping's own Vector input should come from Texture Coordinate.UV
    # for the standard UV-transform workflow.  Anything else (Generated
    # / Object / Window / Reflection / Normal / Camera) is out of scope.
    map_vec_in = mapping.inputs.get("Vector")
    if map_vec_in is not None and map_vec_in.is_linked:
        mlinks = [lk for lk in map_vec_in.links if not getattr(lk, "is_muted", False)]
        if mlinks:
            up_node, up_sock = _skip_reroutes_and_muted_with_socket(mlinks[0].from_node, mlinks[0].from_socket)
            if up_node is not None and up_node.bl_idname == "ShaderNodeTexCoord":
                if getattr(up_sock, "name", "") != "UV":
                    _warn_once(state, f"RISE only supports UV-mapped textures for {context_name} (Mapping reads from '{getattr(up_sock, 'name', '')}').")
                    return "reject"
        # Other upstream node types (NodeReroute, Mix, etc.) — accept
        # optimistically; if the chain ultimately produces non-UV
        # coords the texture footprint will be wrong but renderable.

    # Each transform socket must be unlinked so we can read its
    # constant default_value.  Linked sockets imply per-point /
    # procedural transforms this direct painter cannot translate. This
    # restriction is separate from manual channel and Tangent child baking.
    def _socket_vec3(name: str, fallback: tuple[float, float, float]):
        sock = mapping.inputs.get(name)
        if sock is None or sock.is_linked:
            if sock is not None and sock.is_linked:
                _warn_once(state, f"RISE only supports constant Mapping {name} on {context_name}; reading the linked socket's default.")
            return fallback
        v = sock.default_value
        return (float(v[0]), float(v[1]), float(v[2]))

    location = _socket_vec3("Location", (0.0, 0.0, 0.0))
    rotation = _socket_vec3("Rotation", (0.0, 0.0, 0.0))
    scale    = _socket_vec3("Scale",    (1.0, 1.0, 1.0))

    if abs(rotation[0]) > 1e-6 or abs(rotation[1]) > 1e-6:
        _warn_once(state, f"RISE supports only Z-axis rotation in Mapping nodes for {context_name}; X/Y components ignored.")
    if abs(location[2]) > 1e-6 or abs(scale[2] - 1.0) > 1e-6:
        # Z components only matter for 3D texture spaces.  For 2D
        # image textures (the only kind RISE consumes) they're a
        # no-op; silently drop them.
        pass

    return (location[0], location[1], rotation[2], scale[0], scale[1])


def _is_identity_uv_transform(t: tuple[float, float, float, float, float]) -> bool:
    """Skip the UV-transform wrapper when the Mapping node's defaults
    are the canonical identity (0, 0, 0, 1, 1).  Matches the early-
    out inside `UVTransformPainter::ApplyTransform`."""
    ofs_u, ofs_v, rot, scale_u, scale_v = t
    return (
        abs(ofs_u) < 1e-9 and abs(ofs_v) < 1e-9
        and abs(rot) < 1e-9
        and abs(scale_u - 1.0) < 1e-9 and abs(scale_v - 1.0) < 1e-9
    )


def _add_uv_transform_painter(
    state: _ExportState,
    label: str,
    source_painter_name: str,
    transform: tuple[float, float, float, float, float],
) -> str:
    """Register a UV-transform painter wrapping `source_painter_name`.
    Caches by (source, transform-tuple)."""
    ofs_u, ofs_v, rot, scale_u, scale_v = transform
    key = (
        "uv_transform",
        source_painter_name,
        round(ofs_u, 6), round(ofs_v, 6),
        round(rot, 6),
        round(scale_u, 6), round(scale_v, 6),
    )
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", label)
    payload = PainterData(
        name=name,
        kind=PAINTER_UV_TRANSFORM,
        painter_a_name=source_painter_name,
        uv_offset_u=ofs_u,
        uv_offset_v=ofs_v,
        uv_rotation=rot,
        uv_scale_u=scale_u,
        uv_scale_v=scale_v,
    )
    state.painters.append(payload)
    state.painter_cache[key] = name
    return name


def _validate_texture_wrapper(
    texture_wrapper,
    state: _ExportState,
    context_name: str,
    *,
    for_expression: bool = False,
) -> tuple[str, int] | None:
    if texture_wrapper is None or texture_wrapper.image is None:
        return None

    image = texture_wrapper.image
    if image.source not in ("FILE", "GENERATED"):
        _warn_once(state, f"RISE only supports file-backed or packed textures for {context_name}.")
        return None
    if not for_expression:
        # Direct texture painters sample primary mesh UV (optionally wrapped
        # in UVTransformPainter for a constant Mapping node).  Expression
        # images handle arbitrary Vector inputs via `sample_uv(p, u, v)`.
        if getattr(texture_wrapper, "texcoords", "UV") not in ("UV", "Vector"):
            _warn_once(state, f"RISE currently supports only UV-mapped textures for {context_name}.")
            return None
    if getattr(texture_wrapper, "projection", "FLAT") != "FLAT":
        _warn_once(state, f"RISE currently supports only flat image projection for {context_name}.")
        return None
    allowed_ext = ("REPEAT", "EXTEND", "MIRROR", "CLIP") if for_expression else ("REPEAT", "EXTEND", "MIRROR")
    if getattr(texture_wrapper, "extension", "REPEAT") not in allowed_ext:
        _warn_once(state, f"RISE currently supports only repeat/extend/mirror image extension for {context_name}.")
        return None

    filepath = _resolve_image_for_bridge(image, state)
    if not filepath or not os.path.exists(filepath):
        _warn_once(state, f"RISE could not resolve the image file used for {context_name}.")
        return None

    extension = os.path.splitext(filepath)[1].lower()
    painter_kind = SUPPORTED_IMAGE_KINDS.get(extension)
    if painter_kind is None:
        _warn_once(state, f"RISE supports PNG, JPG, EXR, HDR, and TIFF textures for {context_name}.")
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


def _add_expression_painter(
    state: _ExportState,
    label: str,
    expression: str,
) -> str:
    key = ("expression", expression)
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", label)
    payload = PainterData(name=name, kind=PAINTER_EXPRESSION, expression=expression)
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
    wrap_mode: int = 0,
) -> str:
    key = (
        "texture",
        kind,
        filepath,
        color_space,
        tuple(round(value, 6) for value in scale),
        tuple(round(value, 6) for value in shift),
        int(wrap_mode),
    )
    if key in state.painter_cache:
        return state.painter_cache[key]

    name = _unique_name(state, "pnt", _safe_ident(label))
    payload = PainterData(
        name=name,
        kind=kind,
        path=filepath,
        color_space=color_space,
        filter_type=FILTER_BILINEAR,
        lowmemory=False,
        scale=scale,
        shift=shift,
        wrap_mode=int(wrap_mode),
    )
    state.painters.append(payload)
    state.painter_cache[key] = name
    return name


class _DeferredExprImageResolver:
    """Image resolver callback for ``procedural_expr`` compilation.

    Validates ``ShaderNodeTexImage`` nodes and reserves identifier-safe RISE
    texture painter names without mutating ``state.painters`` until ``commit()``
    is called after compilation succeeds (avoiding orphaned painters on failed
    compilation while guaranteeing every texture painter is registered before
    the ``ExpressionPainter`` that calls ``sample()`` / ``sample_uv()`` on it).
    """

    def __init__(self, state: _ExportState, label_prefix: str, *, force_linear: bool = False):
        self._state = state
        self._label_prefix = label_prefix
        self._force_linear = force_linear
        self._pending: list[tuple[tuple, str, str, int, int, int]] = []
        self._by_key: dict[tuple, str] = {}

    def __call__(self, image_node) -> str | None:
        image = getattr(image_node, "image", None)
        if image is None:
            return None
        colorspace_name = "Non-Color" if self._force_linear else str(
            getattr(getattr(image, "colorspace_settings", None), "name", "sRGB") or "sRGB"
        )
        wrapper = _ImageSocketWrapper(
            image_node,
            colorspace_is_data=self._force_linear,
            colorspace_name=colorspace_name,
        )
        validated = _validate_texture_wrapper(
            wrapper,
            self._state,
            f"expression image on '{self._label_prefix}'",
            for_expression=True,
        )
        if not validated:
            return None
        filepath, color_space = validated
        if self._force_linear:
            color_space = COLOR_SPACE_LINEAR
        kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
        wrap_mode = _tex_image_wrap_mode(image_node)
        cache_key = (
            "texture",
            kind,
            filepath,
            color_space,
            (1.0, 1.0, 1.0),
            (0.0, 0.0, 0.0),
            int(wrap_mode),
        )
        existing = self._state.painter_cache.get(cache_key)
        if existing is not None:
            return existing
        pending_name = self._by_key.get(cache_key)
        if pending_name is not None:
            return pending_name
        img_label = _safe_ident(f"{self._label_prefix}_{getattr(image, 'name', 'tex')}")
        name = _unique_name(self._state, "pnt", img_label)
        self._by_key[cache_key] = name
        self._pending.append((cache_key, name, filepath, kind, color_space, int(wrap_mode)))
        return name

    def commit(self) -> None:
        for cache_key, name, filepath, kind, color_space, wrap_mode in self._pending:
            if cache_key in self._state.painter_cache:
                continue
            payload = PainterData(
                name=name,
                kind=kind,
                path=filepath,
                color_space=color_space,
                filter_type=FILTER_BILINEAR,
                lowmemory=False,
                scale=(1.0, 1.0, 1.0),
                shift=(0.0, 0.0, 0.0),
                wrap_mode=wrap_mode,
            )
            self._state.painters.append(payload)
            self._state.painter_cache[cache_key] = name
        self._pending.clear()


def _is_direct_simple_image_socket(socket, *, want_output: str = "Color") -> bool:
    """True iff ``socket`` links (through reroutes / muted passthroughs only)
    directly to ``ShaderNodeTexImage.<want_output>`` with ``projection == 'FLAT'``,
    ``extension in ('REPEAT', 'EXTEND', 'MIRROR')``, and either unmodified
    primary UV or a constant zero-rotation ``POINT`` Mapping explicitly fed by
    ``TexCoord.UV`` (so ``TEXTURE`` mode, rotations, unlinked Mapping vectors,
    ``CLIP``, ``UVMap``, and filters all route through ``procedural_expr`` for
    exact Cycles parity)."""
    if socket is None or not getattr(socket, "is_linked", False):
        return False
    links = [lk for lk in socket.links if not getattr(lk, "is_muted", False)]
    if not links:
        return False
    node = links[0].from_node
    out_sock = links[0].from_socket
    seen: set[tuple[int, str]] = set()
    steps = 0
    while node is not None:
        steps += 1
        if steps > 16:
            return False
        out_key = procedural_expr.canonical_output_name(node, out_sock)
        vkey = (rna_key(node), out_key if getattr(node, "mute", False) else "")
        if vkey in seen:
            return False
        seen.add(vkey)
        if getattr(node, "bl_idname", "") == "NodeReroute":
            if not node.inputs or not node.inputs[0].is_linked:
                return False
            rlinks = [lk for lk in node.inputs[0].links if not getattr(lk, "is_muted", False)]
            if not rlinks:
                return False
            node = rlinks[0].from_node
            out_sock = rlinks[0].from_socket
            continue
        if getattr(node, "mute", False):
            pass_inp = procedural_expr.resolve_muted_input_socket(node, out_key)
            if pass_inp is None or not pass_inp.is_linked:
                return False
            plinks = [lk for lk in pass_inp.links if not getattr(lk, "is_muted", False)]
            if not plinks:
                return False
            node = plinks[0].from_node
            out_sock = plinks[0].from_socket
            continue
        if getattr(node, "bl_idname", "") != "ShaderNodeTexImage":
            return False
        if getattr(out_sock, "name", "") != want_output:
            return False
        if getattr(node, "projection", "FLAT") != "FLAT":
            return False
        if getattr(node, "extension", "REPEAT") not in ("REPEAT", "EXTEND", "MIRROR"):
            return False
        vec_in = node.inputs.get("Vector") if "Vector" in node.inputs else None
        if vec_in is None or not vec_in.is_linked:
            return True
        vlinks = [lk for lk in vec_in.links if not getattr(lk, "is_muted", False)]
        if not vlinks:
            return True
        vsrc, vsock = _skip_reroutes_and_muted_with_socket(vlinks[0].from_node, vlinks[0].from_socket)
        if vsrc is None:
            return False
        vbl = getattr(vsrc, "bl_idname", "")
        if vbl == "ShaderNodeTexCoord":
            return (not getattr(vsrc, "from_instancer", False)) and getattr(vsock, "name", "") == "UV"
        if vbl == "ShaderNodeMapping":
            # Only direct for constant POINT Mapping with zero rotation explicitly
            # reading TexCoord.UV (UVTransformPainter uses KHR clockwise rotation
            # and does not implement TEXTURE inverse or unlinked default Vector).
            if getattr(vsrc, "vector_type", "POINT") != "POINT":
                return False
            for mname in ("Location", "Rotation", "Scale"):
                ms = vsrc.inputs.get(mname) if mname in vsrc.inputs else None
                if ms is not None and any(not getattr(lk, "is_muted", False) for lk in getattr(ms, "links", ())):
                    return False
            rot_sock = vsrc.inputs.get("Rotation") if "Rotation" in vsrc.inputs else None
            if rot_sock is not None:
                rv = getattr(rot_sock, "default_value", (0.0, 0.0, 0.0))
                if any(not (abs(float(c)) <= 1e-6) for c in rv[:3]):
                    return False
            mv = vsrc.inputs.get("Vector") if "Vector" in vsrc.inputs else None
            if mv is None or not mv.is_linked:
                return False
            mvlinks = [lk for lk in mv.links if not getattr(lk, "is_muted", False)]
            if not mvlinks:
                return False
            mvsrc, mvsock = _skip_reroutes_and_muted_with_socket(mvlinks[0].from_node, mvlinks[0].from_socket)
            return (
                mvsrc is not None
                and getattr(mvsrc, "bl_idname", "") == "ShaderNodeTexCoord"
                and not getattr(mvsrc, "from_instancer", False)
                and getattr(mvsock, "name", "") == "UV"
            )
        return False
    return False


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


def _maybe_wrap_with_uv_transform(
    state: _ExportState,
    label: str,
    base_painter_name: str,
    texture_wrapper,
) -> str:
    """If the `texture_wrapper` (`_ImageSocketWrapper` or
    PrincipledBSDFWrapper-supplied `ShaderImageTextureWrapper`) has a
    Mapping node feeding its `Vector` socket, extract the transform
    and wrap `base_painter_name` in a UVTransformPainter.  Otherwise
    return the base painter name unchanged.

    A `"reject"` from `_extract_mapping_transform` means the chain is
    present but unsupported — we return the base painter (image
    sampled at unmodified UV) plus the diagnostic the extractor
    already emitted."""

    node_image = getattr(texture_wrapper, "node_image", None)
    if node_image is None:
        # PrincipledBSDFWrapper's native ShaderImageTextureWrapper
        # exposes the actual node via `tex_image_node` rather than
        # `node_image` — try that too so the wrapper's own walker
        # path also benefits.
        node_image = getattr(texture_wrapper, "tex_image_node", None)
    if node_image is None:
        return base_painter_name

    transform = _extract_mapping_transform(node_image, state, label)
    if transform is None or transform == "reject":
        return base_painter_name
    if _is_identity_uv_transform(transform):
        return base_painter_name

    return _add_uv_transform_painter(state, f"{label}_uvxf", base_painter_name, transform)


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
        node_image = getattr(texture_wrapper, "node_image", None) or getattr(texture_wrapper, "tex_image_node", None)
        wrap_mode = _tex_image_wrap_mode(node_image) if node_image is not None else 1
        base = _add_texture_painter(state, label, filepath, kind, color_space, scale=scale, shift=shift, wrap_mode=wrap_mode)
        return _maybe_wrap_with_uv_transform(state, label, base, texture_wrapper)
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
        node_image = getattr(texture_wrapper, "node_image", None) or getattr(texture_wrapper, "tex_image_node", None)
        wrap_mode = _tex_image_wrap_mode(node_image) if node_image is not None else 1
        base = _add_texture_painter(
            state,
            label,
            filepath,
            kind,
            color_space,
            scale=(scale, scale, scale),
            shift=(shift, shift, shift),
            wrap_mode=wrap_mode,
        )
        return _maybe_wrap_with_uv_transform(state, label, base, texture_wrapper)
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


def _warn_unsupported_principled_features(
    state: _ExportState,
    material,
    wrapper: PrincipledBSDFWrapper,
    translated_sockets: set[str] | None = None,
):
    node = wrapper.node_principled_bsdf
    material_name = material.name_full
    # Anisotropy ("Anisotropic"/"Anisotropic Rotation"), Specular IOR
    # Level, and Specular Tint (DL-151, docs/DEBT_LEDGER.md) now flow
    # through AddPBRMetallicRoughnessMaterial, and Sheen now flows
    # through fabric_material (DL-18, docs/DEBT_LEDGER.md), so none of
    # these are warned about anymore.  (This comment previously claimed
    # "specular tint" was already wired here -- it was not: nothing in
    # this function read the "Specular Tint" socket until DL-151.)
    # Coat ("Coat Weight"/"Coat Tint"/"Coat Roughness"/"Coat IOR") and
    # Subsurface ("Subsurface Weight"/"Radius"/"Scale"/"IOR") now flow
    # through coated_material / randomwalk_sss_material respectively
    # (DL-186, docs/DEBT_LEDGER.md) -- removed from this generic
    # warn-only sweep since both are read (and warned about
    # PER-SOCKET, where relevant) below in `_material_payload`.
    # "Coat Normal" is translated too (coat_normal_painter_name, ABI v14,
    # DL-192), and "Tangent" is read by the independent DL-213 shader-
    # direction producer (which is why the procedural-texture warning
    # below skips that socket).
    #
    # Sockets newer than the PBR model RISE translates onto (Blender
    # 4.3 Diffuse Roughness, 5.3 Thin Wall).  Thin Film (4.2) and
    # Transmission Dispersion (5.3) ARE translated (ABI v18) and are
    # not listed here.
    if node is None:
        return

    def _matters(socket_name: str, default: float) -> bool:
        socket = _node_input(node, socket_name)
        return socket is not None and (socket.is_linked or float(socket.default_value) > default)

    # Diffuse Roughness only shapes the PBR diffuse lobe.  The branches
    # without one: fully metallic, the dielectric branch (Transmission
    # >= 0.05, `transmission_heavy`) and the random-walk SSS branch.
    def _constant_at_least(socket_name: str, threshold: float) -> bool:
        socket = _node_input(node, socket_name)
        return socket is not None and not socket.is_linked and float(socket.default_value) >= threshold

    has_diffuse = not (
        _constant_at_least("Metallic", 1.0 - 1.0e-4)
        or _constant_at_least("Transmission Weight", 0.05)
        or _constant_at_least("Subsurface Weight", 1.0e-4)
    )
    if has_diffuse and _matters("Diffuse Roughness", 1.0e-4):
        _warn_once(
            state,
            f"RISE ignores Principled Diffuse Roughness on '{material_name}'; "
            f"the base diffuse lobe stays Lambertian.",
        )
    thin_wall = _node_input(node, "Thin Wall")
    if (thin_wall is not None and (thin_wall.is_linked or bool(thin_wall.default_value))
            and _matters("Transmission Weight", 0.0)):
        _warn_once(
            state,
            f"RISE ignores Principled Thin Wall on '{material_name}'; "
            f"transmission is rendered as a solid (refracting) interface.",
        )
    # A single "Dispersion" socket is not part of the 5.2/5.3 Principled
    # node; if a Blender version exposes one, say so rather than drop it.
    if _matters("Dispersion", 0.0):
        _warn_once(
            state,
            f"RISE does not translate the Principled 'Dispersion' socket on '{material_name}'; "
            f"use Blender 5.3's Transmission Dispersion Scale / Abbe Number.",
        )

    # Procedural textures with no direct translation.  They classify as
    # "simple" so the material keeps its direct path (routing them to the
    # manual bake would replace metals / glass / coats with the bake's
    # synthetic metallic-0 PBR), which means the input they feed loses
    # their contribution -- say so, per input.
    def _live_links(socket):
        return [link for link in socket.links if not getattr(link, "is_muted", False)]

    for socket in node.inputs:
        if socket.name == "Tangent":
            continue        # the Tangent producer bakes its whole linked graph (DL-213)
        if translated_sockets and socket.name in translated_sockets:
            continue        # natively compiled to an expression_painter
        links = _live_links(socket) if socket.is_linked else []
        if not links:
            continue
        stack = [links[0].from_node]
        seen: set[int] = set()
        while stack:
            upstream = stack.pop()
            if upstream is None or rna_key(upstream) in seen or len(seen) > 64:
                continue
            seen.add(rna_key(upstream))
            if getattr(upstream, "mute", False):
                continue
            label = _UNTRANSLATED_PROCEDURAL_TEXTURES.get(upstream.bl_idname)
            if label is not None:
                _warn_once(
                    state,
                    f"RISE does not translate the procedural {label} texture feeding '{socket.name}' "
                    f"on '{material_name}'; that input loses the texture's contribution (bake the "
                    f"texture to an image in Blender to keep it).",
                )
                continue
            for upstream_input in upstream.inputs:
                upstream_links = _live_links(upstream_input) if upstream_input.is_linked else []
                if upstream_links:
                    stack.append(upstream_links[0].from_node)


# Procedural texture nodes the classifier admits as "simple".  Noise,
# Voronoi, and White Noise compile natively to RISE `expression_painter`
# expressions on supported Principled sockets (including procedural Bump
# heights via MODIFIER_RELIEF); this table drives the per-socket fallback
# warning whenever one of them feeds an untranslated socket (e.g. Alpha)
# or an unsupported node/mode (e.g. Checker).
_UNTRANSLATED_PROCEDURAL_TEXTURES = {
    "ShaderNodeTexNoise": "Noise",
    "ShaderNodeTexVoronoi": "Voronoi",
    "ShaderNodeTexWhiteNoise": "White Noise",
    "ShaderNodeTexChecker": "Checker",
    "ShaderNodeTexGradient": "Gradient",
    "ShaderNodeTexBrick": "Brick",
    "ShaderNodeAttribute": "Attribute",
    "ShaderNodeVertexColor": "Vertex Color",
    "ShaderNodeNewGeometry": "Geometry",
    "ShaderNodeParticleInfo": "Particle Info",
    "ShaderNodeAmbientOcclusion": "Ambient Occlusion",
    "ShaderNodeObjectInfo": "Object Info",
}


@dataclass
class _ThinFilmSubstrate:
    conductor: bool
    dielectric_ior: float
    metal_f0: tuple[float, float, float]
    metal_tint: tuple[float, float, float]
    film_tint: tuple[float, float, float] | None


def _thin_film_substrate(state: _ExportState, material, node, base_color,
                         metallic: float, specular_ior_level: float, anisotropy: float):
    """Substrate of the `ggx_material` `thinfilm` base that replaces the
    PBR base when a Principled Thin Film is present (ABI v18).

    Both cases follow what Cycles renders once a film is present
    (``intern/cycles/kernel/closure/bsdf_microfacet.h``):

    - Dielectric (Metallic < 0.5): the Specular-IOR-Level-adjusted index
      (k = 0) and Specular Tint as the film-reflectance tint, clamped to
      [0, 1] (``principled_math.dielectric_film_substrate``).
    - Conductor (Metallic >= 0.5): F0 = Base Color and the F82 tint =
      Specular Tint, numerically; the native bridge applies Cycles'
      Gulbrandsen estimate at every wavelength of their uplifted spectra
      (``register_thin_film_metal_substrate``).

    The film needs ONE substrate, so a partial or textured Metallic is
    resolved to the nearer end with a warning, and every input it reads
    numerically is read at its socket default.
    """

    name = material.name_full
    metallic_socket = _node_input(node, "Metallic")
    if metallic_socket is not None and metallic_socket.is_linked:
        _warn_once(state, f"RISE resolves textured Metallic on thin-film '{name}' to its socket default "
                          f"({metallic:.2f}); the film needs a single substrate.")
    elif 1.0e-3 < metallic < 1.0 - 1.0e-3:
        _warn_once(state, f"RISE resolves partial Metallic {metallic:.2f} on thin-film '{name}' to a "
                          f"{'conductor' if metallic >= 0.5 else 'dielectric'} substrate.")
    if anisotropy > 1.0e-4:
        _warn_once(state, f"RISE renders the thin film on '{name}' over an isotropic GGX base; "
                          f"Anisotropic is ignored there.")

    if metallic >= 0.5:
        for socket_name in ("Base Color", "Specular Tint"):
            socket = _node_input(node, socket_name)
            if socket is not None and socket.is_linked:
                _warn_once(state, f"RISE derives the thin-film metal substrate on '{name}' from the "
                                  f"{socket_name} socket default; a linked value is not sampled.")
        tint_socket = _node_input(node, "Specular Tint")
        if tint_socket is not None and tint_socket.type != "VALUE":
            tint = _socket_default_color(node, "Specular Tint", (1.0, 1.0, 1.0))
        else:
            tint = (1.0, 1.0, 1.0)      # pre-4.0 float tint has no F82 meaning
        return _ThinFilmSubstrate(conductor=True, dielectric_ior=1.5,
                                  metal_f0=tuple(float(c) for c in base_color),
                                  metal_tint=tuple(float(c) for c in tint), film_tint=None)

    for socket_name in ("IOR", "Specular IOR Level", "Specular Tint"):
        socket = _node_input(node, socket_name)
        if socket is not None and socket.is_linked:
            _warn_once(state, f"RISE derives the thin-film dielectric on '{name}' from the "
                              f"{socket_name} socket default; a linked value is not sampled.")
    # As in Cycles: Specular IOR Level adjusts the index the film sits on,
    # and Specular Tint (clamped to [0, 1], see dielectric_film_substrate)
    # scales the film reflectance.
    tint_socket = _node_input(node, "Specular Tint")
    if tint_socket is not None and tint_socket.type != "VALUE":
        tint = _socket_default_color(node, "Specular Tint", (1.0, 1.0, 1.0))
    else:
        tint = (1.0, 1.0, 1.0)
    if any(c > 1.0 + 1.0e-6 for c in tint):
        _warn_once(state, f"RISE caps the Specular Tint on thin-film '{name}' at 1 per channel; "
                          f"a brighter tint would push the film reflectance past 1.")
    eta, film_tint = dielectric_film_substrate(
        _socket_default_float(node, "IOR", 1.5), specular_ior_level, tint)
    return _ThinFilmSubstrate(conductor=False, dielectric_ior=eta,
                              metal_f0=(1.0, 1.0, 1.0), metal_tint=(1.0, 1.0, 1.0),
                              film_tint=film_tint)


class _ImageSocketWrapper:
    """Minimal wrapper that lets _validate_texture_wrapper inspect a
    raw ShaderNodeTexImage as if it were a node_shader_utils texture
    wrapper.  Used by both the bump and normal-map paths."""

    def __init__(self, node_image, colorspace_is_data: bool, colorspace_name: str):
        self.node_image = node_image
        self.image = node_image.image
        self.colorspace_is_data = colorspace_is_data
        self.colorspace_name = colorspace_name

    def has_mapping_node(self) -> bool:
        vector_input = self.node_image.inputs["Vector"]
        if not vector_input.is_linked:
            return False
        live = [lk for lk in vector_input.links if not getattr(lk, "is_muted", False)]
        if not live:
            return False
        src_node, _ = _skip_reroutes_and_muted_with_socket(live[0].from_node, live[0].from_socket)
        return src_node is not None and src_node.bl_idname == "ShaderNodeMapping"

    @property
    def texcoords(self) -> str:
        vector_input = self.node_image.inputs["Vector"]
        if not vector_input.is_linked:
            return "UV"
        live = [lk for lk in vector_input.links if not getattr(lk, "is_muted", False)]
        if not live:
            return "UV"
        src_node, src_sock = _skip_reroutes_and_muted_with_socket(live[0].from_node, live[0].from_socket)
        if src_node is not None and src_node.bl_idname == "ShaderNodeAttribute":
            return "Attribute"
        if src_node is not None and src_node.bl_idname == "ShaderNodeUVMap":
            if bool(getattr(src_node, "uv_map", "") or "") or bool(getattr(src_node, "from_instancer", False)):
                return "UVMap"
            return "UV"
        if src_sock is not None:
            return src_sock.name
        return live[0].from_socket.name

    @property
    def projection(self) -> str:
        return self.node_image.projection

    @property
    def extension(self) -> str:
        return self.node_image.extension


# Legacy fallback pass-through nodes used ONLY when expression compilation fails
# and the direct-image walker searches for an underlying texture.  (In the
# primary Phase 2 path, RGBCurve / HSV / BrightContrast / Gamma / Invert /
# ValToRGB are compiled faithfully into RISE ExpressionPainters.)
_SOCKET_PASS_THROUGH_NODES = {
    "NodeReroute",
    "ShaderNodeRGBCurve",
    "ShaderNodeHueSaturation",
    "ShaderNodeBrightContrast",
    "ShaderNodeGamma",
    "ShaderNodeInvert",
    "ShaderNodeValToRGB",     # Colour Ramp
}

# Mix-style nodes.  We follow the "primary" input (A / Color1) — this is
# an approximation: the artist's intent on these nodes is genuinely a
# blend that RISE cannot represent through a single painter.  Picking A
# preserves the base texture, which is the usual primary signal; the
# mix factor / B input is lost.
_SOCKET_MIX_NODES = {
    "ShaderNodeMix",
    "ShaderNodeMixRGB",
}

# Preferred socket names to follow on each mix variant, in priority order.
# Blender 4.x ShaderNodeMix uses "A"/"B" generic naming; the legacy
# ShaderNodeMixRGB uses "Color1"/"Color2"; some old node libraries still
# emit "Color".
_MIX_PRIMARY_SOCKET_NAMES = ("A", "Color1", "Color")


def _walk_socket_for_image_node(socket, *, max_depth: int = 16):
    """Walk backward from a shader-graph input socket searching for a
    terminal ``ShaderNodeTexImage``.

    Returns the image-texture node (with ``.image`` possibly None — the
    caller is expected to re-validate) or ``None`` if the chain hits an
    unsupported node, an unlinked socket, or exceeds ``max_depth``.

    Traverses:
        * ``NodeReroute`` — the most common reason
          ``PrincipledBSDFWrapper.X_texture`` silently returns the socket
          default; an inserted reroute breaks the wrapper's direct-link
          assumption and the entire texture is lost.
        * ``_SOCKET_PASS_THROUGH_NODES`` — colour / scalar filters that
          the wrapper also doesn't follow.  We don't bake the filter,
          but we at least find the underlying image.
        * ``_SOCKET_MIX_NODES`` — picks the A / Color1 / Color branch.

    Unknown node types short-circuit to ``None`` so the caller falls
    back to the socket default colour — this is intentionally
    conservative: blindly following arbitrary inputs (e.g. through a
    Bump node's Height input) would produce wrong textures.
    """

    if socket is None or not socket.is_linked:
        return None
    root_links = [lk for lk in socket.links if not getattr(lk, "is_muted", False)]
    if not root_links:
        return None

    visited: set[tuple[int, str]] = set()
    root_from = root_links[0].from_node
    stack = [(root_from, procedural_expr.canonical_output_name(root_from, root_links[0].from_socket), 0)]
    while stack:
        node, out_key, depth = stack.pop()
        if depth > max_depth:
            continue
        node_id = rna_key(node)
        vkey = (node_id, out_key if getattr(node, "mute", False) else "")
        if vkey in visited:
            continue
        visited.add(vkey)

        if getattr(node, "mute", False):
            pass_inp = procedural_expr.resolve_muted_input_socket(node, out_key)
            if pass_inp is not None and pass_inp.is_linked:
                plinks = [lk for lk in pass_inp.links if not getattr(lk, "is_muted", False)]
                if plinks:
                    pnode = plinks[0].from_node
                    stack.append((pnode, procedural_expr.canonical_output_name(pnode, plinks[0].from_socket), depth + 1))
            continue

        bl = node.bl_idname
        if bl == "ShaderNodeTexImage":
            return node

        if bl == "NodeReroute":
            if node.inputs and node.inputs[0].is_linked:
                rlinks = [lk for lk in node.inputs[0].links if not getattr(lk, "is_muted", False)]
                if rlinks:
                    rnode = rlinks[0].from_node
                    stack.append((rnode, procedural_expr.canonical_output_name(rnode, rlinks[0].from_socket), depth + 1))
            continue

        if bl in _SOCKET_PASS_THROUGH_NODES:
            # These nodes typically have one primary signal input
            # (Color / Fac / Value) plus auxiliary parameters like
            # curve points or gamma values — but the auxiliary inputs
            # are nearly always unlinked, so a broad RGBA/VALUE/VECTOR
            # sweep is safe in practice.
            for inp in node.inputs:
                if inp.is_linked and inp.type in {"RGBA", "VALUE", "VECTOR"}:
                    ilinks = [lk for lk in inp.links if not getattr(lk, "is_muted", False)]
                    if ilinks:
                        inode = ilinks[0].from_node
                        stack.append((inode, procedural_expr.canonical_output_name(inode, ilinks[0].from_socket), depth + 1))
            continue

        if bl in _SOCKET_MIX_NODES:
            chosen_link = None
            for candidate in _MIX_PRIMARY_SOCKET_NAMES:
                if candidate in node.inputs:
                    sock = node.inputs[candidate]
                    if sock.is_linked:
                        clinks = [lk for lk in sock.links if not getattr(lk, "is_muted", False)]
                        if clinks:
                            chosen_link = clinks[0]
                            break
            if chosen_link is None:
                # Fall back: first linked colour or value input that
                # isn't the factor.
                for inp in node.inputs:
                    if inp.is_linked and inp.type in {"RGBA", "VALUE"} and inp.name.lower() != "factor":
                        clinks = [lk for lk in inp.links if not getattr(lk, "is_muted", False)]
                        if clinks:
                            chosen_link = clinks[0]
                            break
            if chosen_link is not None:
                cnode = chosen_link.from_node
                stack.append((cnode, procedural_expr.canonical_output_name(cnode, chosen_link.from_socket), depth + 1))
            continue

        # Unknown node — stop the walk for this branch.
        continue

    return None


class _FallbackPrincipledWrapper:
    """Stand-in for ``PrincipledBSDFWrapper`` used when the official
    walker can't trace the Material Output Surface back to a single
    Principled BSDF — i.e. when artists wire a Mix Shader / Add
    Shader between the Principled and the output (as in
    `ripple_dreams_fields`'s `worn_wooden_box`, which mixes two
    Principled BSDFs).

    Reads each socket's `.default_value` directly off whichever
    Principled BSDF the caller hands us (the first one found in the
    graph by ``_find_any_principled_bsdf``).  Texture accessors all
    return ``None`` — the caller's ``_maybe_resolve_socket_texture``
    fall-through uses the manual node-graph walker against the
    Principled BSDF's inputs, so reroute / pass-through / Mix chains
    on those slots still resolve correctly.

    This is a best-effort approximation: the mix factor between two
    Principled BSDFs is lost, and the second BSDF is silently
    discarded.  It is still vastly better than the prior fallback,
    which used `material.diffuse_color` (typically the viewport
    flat-grey 0.8) — that produced a `chrome` material as plain
    grey, a `wooden_box` as off-white, etc.
    """

    def __init__(self, node):
        self.node_principled_bsdf = node

    @property
    def base_color(self):
        return _socket_default_color(self.node_principled_bsdf, "Base Color", (0.8, 0.8, 0.8))

    @property
    def metallic(self):
        return _socket_default_float(self.node_principled_bsdf, "Metallic", 0.0)

    @property
    def roughness(self):
        return _socket_default_float(self.node_principled_bsdf, "Roughness", 0.5)

    @property
    def specular(self):
        return _socket_default_float(self.node_principled_bsdf, "Specular IOR Level", 0.5)

    @property
    def transmission(self):
        return _socket_default_float(self.node_principled_bsdf, "Transmission Weight", 0.0)

    @property
    def emission_color(self):
        return _socket_default_color(self.node_principled_bsdf, "Emission Color", (0.0, 0.0, 0.0))

    @property
    def emission_strength(self):
        return _socket_default_float(self.node_principled_bsdf, "Emission Strength", 0.0)

    @property
    def ior(self):
        return _socket_default_float(self.node_principled_bsdf, "IOR", 1.45)

    # All texture wrapper accessors return None — the caller routes
    # texture extraction through `_maybe_resolve_socket_texture`,
    # which falls back to the manual graph walker against the
    # Principled BSDF input sockets.
    base_color_texture = None
    metallic_texture = None
    roughness_texture = None
    specular_texture = None
    transmission_texture = None
    emission_color_texture = None
    emission_strength_texture = None
    ior_texture = None


def _find_any_principled_bsdf(material):
    """Search a material's node graph for the FIRST Principled BSDF.
    Use when PrincipledBSDFWrapper can't trace one (Mix Shader / Add
    Shader chains hide the connection).  Returns the node, or None
    if the material has no Principled BSDF at all."""

    if not uses_nodes(material):
        return None
    for node in material.node_tree.nodes:
        if node.bl_idname == "ShaderNodeBsdfPrincipled":
            return node
    return None


def _maybe_resolve_socket_texture(
    principled_node,
    socket_name: str,
    wrapper_texture,
    *,
    colorspace_is_data: bool,
):
    """Return a texture wrapper for a Principled BSDF input.

    Priority:
        1. The ``PrincipledBSDFWrapper``-supplied texture wrapper, when
           it actually carries an image (the easy direct-link case).
        2. A manual node-graph walk (reroute / pass-through / mix-aware)
           that produces an ``_ImageSocketWrapper`` on success.
        3. ``None`` — caller falls back to the socket's default value.

    ``colorspace_is_data`` controls how ``_texture_color_space`` treats
    the resulting wrapper: ``True`` forces linear (correct for
    metallic / roughness / specular-factor / transmission / IOR /
    emission-strength), ``False`` lets the image's own colorspace
    metadata drive (correct for base colour, emission colour).
    """

    if wrapper_texture is not None and wrapper_texture.image is not None:
        return wrapper_texture

    socket = _node_input(principled_node, socket_name)
    if socket is None or not socket.is_linked:
        return None

    image_node = _walk_socket_for_image_node(socket)
    if image_node is None or image_node.image is None:
        return None

    colorspace_name = "Non-Color" if colorspace_is_data else "sRGB"
    return _ImageSocketWrapper(image_node, colorspace_is_data, colorspace_name)


def _skip_reroutes(node, *, max_depth: int = 16):
    """Walk through ``NodeReroute`` nodes back to the upstream "real"
    node feeding the chain.  Returns ``None`` if the chain dead-ends in
    a disconnected reroute or exceeds ``max_depth``."""

    seen = set()
    while node is not None and node.bl_idname == "NodeReroute":
        if rna_key(node) in seen or len(seen) > max_depth:
            return None
        seen.add(rna_key(node))
        if not node.inputs or not node.inputs[0].is_linked:
            return None
        rlinks = [lk for lk in node.inputs[0].links if not getattr(lk, "is_muted", False)]
        if not rlinks:
            return None
        node = rlinks[0].from_node
    return node


def _skip_reroutes_and_muted_with_socket(node, out_sock=None, *, max_depth: int = 16):
    """Walk backward through ``NodeReroute`` and muted nodes (following
    ``internal_links`` via ``resolve_muted_input_socket``) to the upstream
    active ``(node, out_sock)`` pair.  Returns ``(None, None)`` if the chain
    dead-ends or exceeds ``max_depth``."""
    seen: set[tuple[int, str]] = set()
    steps = 0
    while node is not None:
        steps += 1
        if steps > max_depth:
            return None, None
        out_key = procedural_expr.canonical_output_name(node, out_sock) if out_sock is not None else ""
        key = (rna_key(node), out_key if getattr(node, "mute", False) else "")
        if key in seen:
            return None, None
        seen.add(key)
        if getattr(node, "bl_idname", "") == "NodeReroute":
            if not node.inputs or not node.inputs[0].is_linked:
                return None, None
            rlinks = [lk for lk in node.inputs[0].links if not getattr(lk, "is_muted", False)]
            if not rlinks:
                return None, None
            node = rlinks[0].from_node
            out_sock = rlinks[0].from_socket
            continue
        if getattr(node, "mute", False):
            pass_inp = procedural_expr.resolve_muted_input_socket(node, out_key or out_sock)
            if pass_inp is None or not pass_inp.is_linked:
                return None, None
            plinks = [lk for lk in pass_inp.links if not getattr(lk, "is_muted", False)]
            if not plinks:
                return None, None
            node = plinks[0].from_node
            out_sock = plinks[0].from_socket
            continue
        return node, out_sock
    return None, None


def _skip_reroutes_and_muted(node, out_sock=None, *, max_depth: int = 16):
    """Walk backward through ``NodeReroute`` and muted nodes to the upstream
    active node.  Returns ``None`` if the chain dead-ends or exceeds ``max_depth``."""
    src_node, _ = _skip_reroutes_and_muted_with_socket(node, out_sock, max_depth=max_depth)
    return src_node


def _add_modifier_stack(
    state: _ExportState,
    label: str,
    member_names: tuple[str, ...],
) -> str:
    # Flatten nested single-element or direct members cleanly
    key = ("stack", member_names)
    if key in state.modifier_cache:
        return state.modifier_cache[key]
    modifier_name = _unique_name(state, "mod", f"{label}_stack")
    state.modifiers.append(
        ModifierData(
            name=modifier_name,
            kind=MODIFIER_STACK,
            stack_member_names=member_names,
        )
    )
    state.modifier_cache[key] = modifier_name
    return modifier_name


def _add_blended_modifier(
    state: _ExportState,
    label: str,
    mod_a_name: str | None,
    mod_b_name: str | None,
    weight_spec: str,
) -> str:
    key = ("blend", mod_a_name, mod_b_name, weight_spec)
    if key in state.modifier_cache:
        return state.modifier_cache[key]
    modifier_name = _unique_name(state, "mod", f"{label}_blend")
    state.modifiers.append(
        ModifierData(
            name=modifier_name,
            kind=MODIFIER_BLEND,
            blend_a_name=mod_a_name,
            blend_b_name=mod_b_name,
            blend_weight_painter_name=weight_spec,
        )
    )
    state.modifier_cache[key] = modifier_name
    return modifier_name


def _mix_normal_sockets(node):
    """Return ``(fac_sock, a_sock, b_sock)`` for a normal-blending
    ``ShaderNodeMix`` (``RGBA``/``VECTOR`` with uniform scalar factor,
    ``blend_type=='MIX'`` for ``RGBA``, no ``clamp_result``, clamped factor)
    or ``ShaderNodeMixRGB`` (``MIX``, no ``use_clamp``), or ``None`` if
    unsupported."""
    bl = getattr(node, "bl_idname", "")
    if bl == "ShaderNodeMixRGB":
        if getattr(node, "blend_type", "MIX") != "MIX" or getattr(node, "use_clamp", False):
            return None
        return (
            _node_input(node, "Fac"),
            _node_input(node, "Color1"),
            _node_input(node, "Color2"),
        )
    if bl == "ShaderNodeMix":
        dt = getattr(node, "data_type", "FLOAT")
        if dt not in ("RGBA", "VECTOR"):
            return None
        if dt == "RGBA":
            if getattr(node, "blend_type", "MIX") != "MIX" or getattr(node, "clamp_result", False):
                return None
        elif getattr(node, "factor_mode", "UNIFORM") != "UNIFORM":
            return None
        want_type = "RGBA" if dt == "RGBA" else "VECTOR"
        fac_sock = None
        a_sock = None
        b_sock = None
        for inp in getattr(node, "inputs", ()):
            sname = getattr(inp, "name", "")
            stype = getattr(inp, "type", "")
            if sname == "Factor" and stype == "VALUE" and fac_sock is None:
                fac_sock = inp
            elif sname == "A" and stype == want_type and a_sock is None:
                a_sock = inp
            elif sname == "B" and stype == want_type and b_sock is None:
                b_sock = inp
        if fac_sock is None:
            fac_sock = _node_input(node, "Factor")
        if a_sock is None or b_sock is None:
            return None
        if not getattr(node, "clamp_factor", True):
            flinks = [lk for lk in fac_sock.links if not getattr(lk, "is_muted", False)] if (fac_sock is not None and fac_sock.is_linked) else []
            if flinks:
                return None
            try:
                fv = float(fac_sock.default_value) if fac_sock is not None else 0.5
                if not (0.0 <= fv <= 1.0):
                    return None
            except (TypeError, ValueError):
                return None
        return (fac_sock, a_sock, b_sock)
    return None


def _build_normal_modifier_tree(
    material,
    normal_input,
    state: _ExportState,
    *,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    handled_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
    depth: int = 0,
    seen: frozenset[int] = frozenset(),
) -> str | None:
    """Recursively translate a shader ``Normal`` input tree of ``NormalMap``,
    ``Bump`` (with optional nested ``Bump.Normal`` -> ``ModifierStack``), and
    ``Mix``/``MixRGB`` (``MIX`` mode -> ``BlendedModifier``) into a RISE
    modifier name (or ``None`` for the unperturbed surface normal)."""
    if depth > 16:
        _warn_once(state, f"RISE normal-modifier graph exceeded max depth on '{material.name_full}'.")
        return None
    normal_links = (
        [lk for lk in normal_input.links if not getattr(lk, "is_muted", False)]
        if (normal_input is not None and normal_input.is_linked) else []
    )
    if not normal_links:
        if handled_out is not None:
            handled_out.append(True)
        return None

    normal_node = _skip_reroutes_and_muted(normal_links[0].from_node, normal_links[0].from_socket)
    if normal_node is None:
        if handled_out is not None:
            handled_out.append(True)
        return None

    nid = rna_key(normal_node)
    if nid in seen:
        _warn_once(state, f"RISE detected a cyclic normal-modifier graph on '{material.name_full}'.")
        return None
    next_seen = seen | {nid}

    bl = getattr(normal_node, "bl_idname", "")
    if bl == "ShaderNodeNormalMap":
        mod = _build_normal_map_modifier(
            material,
            normal_node,
            state,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=used_generated_out,
            mesh_attrs_out=mesh_attrs_out,
            attr_types=attr_types,
        )
        if mod is not None and handled_out is not None:
            handled_out.append(True)
        return mod

    if bl == "ShaderNodeBump":
        nested_normal = _node_input(normal_node, "Normal")
        base_handled: list[bool] = []
        base_mod = _build_normal_modifier_tree(
            material,
            nested_normal,
            state,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=used_generated_out,
            handled_out=base_handled,
            mesh_attrs_out=mesh_attrs_out,
            attr_types=attr_types,
            depth=depth + 1,
            seen=next_seen,
        )
        bump_handled: list[bool] = []
        bump_mod = _build_bump_modifier(
            material,
            normal_node,
            state,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=used_generated_out,
            handled_out=bump_handled,
            mesh_attrs_out=mesh_attrs_out,
            attr_types=attr_types,
            allow_nested_normal=True,
        )
        if base_handled and bump_handled and handled_out is not None:
            handled_out.append(True)
        if base_mod is not None and bump_mod is not None:
            return _add_modifier_stack(state, material.name_full, (base_mod, bump_mod))
        return bump_mod if bump_mod is not None else base_mod

    mix_socks = _mix_normal_sockets(normal_node)
    if mix_socks is not None:
        fac_sock, a_sock, b_sock = mix_socks
        fac_links = (
            [lk for lk in fac_sock.links if not getattr(lk, "is_muted", False)]
            if (fac_sock is not None and fac_sock.is_linked) else []
        )
        const_fac: float | None = None
        weight_spec: str | None = None
        if not fac_links:
            try:
                const_fac = _clamp01(float(fac_sock.default_value)) if fac_sock is not None else 0.5
            except (TypeError, ValueError):
                const_fac = 0.5
            weight_spec = f"{const_fac:.6f}"
        else:
            fac_resolver = _DeferredExprImageResolver(state, f"{material.name_full}_normmix")
            fac_comp = procedural_expr.compile_socket_expression(
                fac_sock,
                want_type="scalar",
                bbox_min=bbox_min,
                bbox_max=bbox_max,
                material_pass_index=material_pass_index,
                attr_types=attr_types,
                image_resolver=fac_resolver,
            )
            if fac_comp is None:
                _warn_once(state, f"RISE could not translate the normal-mix Factor graph on '{material.name_full}'.")
                return None
            fac_resolver.commit()
            if used_generated_out is not None and fac_comp.uses_generated:
                used_generated_out.append(True)
            if mesh_attrs_out is not None and fac_comp.mesh_attributes:
                mesh_attrs_out.update(fac_comp.mesh_attributes)
            if _is_pure_float_literal(fac_comp.expression):
                const_fac = _clamp01(float(fac_comp.expression.strip()))
                weight_spec = f"{const_fac:.6f}"
            else:
                weight_spec = _add_expression_painter(
                    state, f"{material.name_full}_normmix_w", fac_comp.expression
                )

        # Constant factor fast paths (0 -> A only, 1 -> B only): skip the
        # unused branch cleanly (e.g. lone-monk's `leather - book - cover 3`
        # has Mix Factor=1.0 -> Bump.001 directly).
        if const_fac is not None and const_fac <= 1e-6:
            return _build_normal_modifier_tree(
                material, a_sock, state,
                bbox_min=bbox_min, bbox_max=bbox_max,
                material_pass_index=material_pass_index,
                used_generated_out=used_generated_out,
                handled_out=handled_out,
                mesh_attrs_out=mesh_attrs_out,
                attr_types=attr_types,
                depth=depth + 1, seen=next_seen,
            )
        if const_fac is not None and const_fac >= 1.0 - 1e-6:
            return _build_normal_modifier_tree(
                material, b_sock, state,
                bbox_min=bbox_min, bbox_max=bbox_max,
                material_pass_index=material_pass_index,
                used_generated_out=used_generated_out,
                handled_out=handled_out,
                mesh_attrs_out=mesh_attrs_out,
                attr_types=attr_types,
                depth=depth + 1, seen=next_seen,
            )

        a_handled: list[bool] = []
        b_handled: list[bool] = []
        mod_a = _build_normal_modifier_tree(
            material, a_sock, state,
            bbox_min=bbox_min, bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=used_generated_out,
            handled_out=a_handled,
            mesh_attrs_out=mesh_attrs_out,
            attr_types=attr_types,
            depth=depth + 1, seen=next_seen,
        )
        mod_b = _build_normal_modifier_tree(
            material, b_sock, state,
            bbox_min=bbox_min, bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=used_generated_out,
            handled_out=b_handled,
            mesh_attrs_out=mesh_attrs_out,
            attr_types=attr_types,
            depth=depth + 1, seen=next_seen,
        )
        if a_handled and b_handled and handled_out is not None:
            handled_out.append(True)
        if mod_a is None and mod_b is None:
            return None
        if mod_a == mod_b:
            return mod_a
        return _add_blended_modifier(state, material.name_full, mod_a, mod_b, weight_spec or "0.5")

    _warn_once(state, f"RISE only supports Bump, Normal Map, or Mix(MIX) normal modifier trees on '{material.name_full}'.")
    return None


def _direct_normal_modifier_for_node(
    material,
    node,
    state: _ExportState,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    handled_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
) -> str | None:
    """Translate a shader node's ``Normal`` socket (NormalMap, Bump, stacked
    Bump.Normal, or Mix(MIX) blended normals) into a RISE modifier."""
    return _build_normal_modifier_tree(
        material,
        _node_input(node, "Normal"),
        state,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        material_pass_index=material_pass_index,
        used_generated_out=used_generated_out,
        handled_out=handled_out,
        mesh_attrs_out=mesh_attrs_out,
        attr_types=attr_types,
    )


def _direct_normal_modifier(
    material,
    wrapper: PrincipledBSDFWrapper,
    state: _ExportState,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    handled_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
) -> str | None:
    return _direct_normal_modifier_for_node(
        material,
        wrapper.node_principled_bsdf,
        state,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        material_pass_index=material_pass_index,
        used_generated_out=used_generated_out,
        handled_out=handled_out,
        mesh_attrs_out=mesh_attrs_out,
        attr_types=attr_types,
    )


def _bump_modifier_scale(strength: float, distance: float, invert: bool) -> float:
    """The signed amplitude a Blender Bump node exports to a RISE bump
    modifier.  Pure: no bpy, so `test_hair_export.py` can pin it.

    SIGN, which is the whole reason this is a named function
    (docs/RELIEF_MODIFIER_DESIGN.md 12, "Fix round 2"):

    Blender's Bump node with Invert OFF tilts the shading normal AWAY
    from the up-slope -- `N - Strength*Distance*grad(h)`, the ordinary
    Blinn/PBRT convention in which the height field is an ELEVATION.
    RISE agrees at the `ReliefModifier` layer: `Modify` computes
    `N - (T*h_T + B*h_B)*scale`, so a POSITIVE `scale` is the
    Invert-OFF look.

    But this exporter does not reach `ReliefModifier` directly.  It
    goes through the ABI-frozen bump shim, whose `normalize=True` fold
    is `scale' = -scale` (RISE_API_CreateBumpMapModifierEx), because
    the removed `bumpmap_modifier` used the OPPOSITE, "the field is
    depth" convention.  One negation in the chain, so this function
    supplies the matching one:

        Invert OFF   ->  -Strength*Distance   -> shim -> +S -> N - S*grad
        Invert ON    ->  +Strength*Distance   -> shim -> -S -> N + S*grad

    A negative `Strength` or `Distance` (Blender allows both) flows
    through as-is and composes with `invert` exactly as it does in
    Cycles -- two negations cancel.
    """

    magnitude = float(strength) * float(distance)
    return magnitude if invert else -magnitude


def _relief_modifier_scale(strength: float, distance: float, invert: bool) -> float:
    """Signed amplitude for a procedural ShaderNodeBump exported as
    `MODIFIER_RELIEF` (RISE's 3D surface-domain `ReliefModifier`, ABI v20).

    Unlike `_bump_modifier_scale` (which carries an extra negation to
    cancel the legacy `MODIFIER_BUMP` shim's depth convention),
    `ReliefModifier` uses Blinn's elevation convention directly:
    `N' = normalize(N - scale * grad(h))`.  Matches Cycles `svm_node_bump`
    (`strength = max(strength, 0.0f)`, `scale *= -1.0f` when `invert` is set):

        Invert OFF   ->  +max(0, Strength) * Distance
        Invert ON    ->  -max(0, Strength) * Distance
    """

    magnitude = max(0.0, float(strength)) * float(distance)
    return -magnitude if invert else magnitude


def _build_bump_modifier(
    material,
    normal_node,
    state: _ExportState,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    handled_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
    allow_nested_normal: bool = False,
) -> str | None:
    if not allow_nested_normal:
        nested_normal = _node_input(normal_node, "Normal")
        nested_links = [lk for lk in nested_normal.links if not getattr(lk, "is_muted", False)] if (nested_normal is not None and nested_normal.is_linked) else []
        if nested_links:
            _warn_once(state, f"RISE ignores nested normal inputs feeding the Bump node on '{material.name_full}'.")

    height_input = _node_input(normal_node, "Height")
    height_links = [lk for lk in height_input.links if not getattr(lk, "is_muted", False)] if (height_input is not None and height_input.is_linked) else []
    if not height_links:
        if handled_out is not None:
            handled_out.append(True)
        return None

    image_node = _walk_socket_for_image_node(height_input) if _is_direct_simple_image_socket(height_input) else None
    if image_node is not None and image_node.bl_idname == "ShaderNodeTexImage":
        texture_wrapper = _ImageSocketWrapper(image_node, colorspace_is_data=True, colorspace_name="Non-Color")
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
            wrap_mode=_tex_image_wrap_mode(image_node),
        )
        painter_name = _maybe_wrap_with_uv_transform(
            state, f"{material.name_full}_bump_height", painter_name, texture_wrapper)
        strength = _socket_default_float(normal_node, "Strength", 1.0)
        distance = _socket_default_float(normal_node, "Distance", 1.0)
        scale = _bump_modifier_scale(strength, distance, bool(getattr(normal_node, "invert", False)))
        modifier_key = ("bump", painter_name, round(scale, 6))
        if modifier_key in state.modifier_cache:
            if handled_out is not None:
                handled_out.append(True)
            return state.modifier_cache[modifier_key]

        modifier_name = _unique_name(state, "mod", f"{material.name_full}_bump")
        state.modifiers.append(
            ModifierData(
                name=modifier_name,
                kind=MODIFIER_BUMP,
                source_painter_name=painter_name,
                scale=scale,
                # Texture-space HALF-STEP for the central difference, in UV units.
                #
                # This was 1.0, which is not a small step -- it is the whole UV
                # range.  RISE's bridge samples the height painter through
                # `Painter::Evaluate`, which CLAMPS (u, v) to [0, 1], so a
                # half-step of 1.0 made every central difference f(1, v) - f(0, v):
                # the same two texels everywhere on the surface, i.e. a constant
                # tilt of the shading normal rather than a bump.  0.005 is roughly a
                # texel on a 200px map and a fraction of one on anything larger,
                # which is what a finite-difference derivative estimate wants.
                # (Fixed 2026-09-06 alongside the `bumpmap_modifier` removal; the
                # bridge entry point is now a shim over `relief_modifier`, and the
                # amplitude fold means the perturbation scales with this value --
                # see docs/RELIEF_MODIFIER_DESIGN.md 7.2/7.5.)
                window=0.005,
                # normalize=True: without it, the shim's legacy fold COUPLES the
                # delivered tilt to `window` (tilt ~ scale*2*window*grad), so
                # shrinking `window` from 1.0 to 0.005 above -- correct on its own
                # terms -- would ALSO have silently divided the tilt by ~200
                # (2*0.005 / 2*1.0). normalize=True selects the bridge's
                # window-independent fold instead (tilt ~ scale*grad, so the
                # magnitude matches Blender's Bump node: Strength*Distance*
                # gradient), so this window shrink is a pure step-size
                # refinement, not an amplitude change.  See
                # docs/RELIEF_MODIFIER_DESIGN.md 7.5 "Phase B review" for the
                # fold, and 12 "Fix round 2" for the SIGN, which
                # `_bump_modifier_scale` above carries and which the fold
                # deliberately does not touch.
                normalize=True,
            )
        )
        state.modifier_cache[modifier_key] = modifier_name
        if handled_out is not None:
            handled_out.append(True)
        return modifier_name

    # Procedural / expression Bump Height -> RISE 3D surface-domain ReliefModifier.
    bump_img_resolver = _DeferredExprImageResolver(state, f"{material.name_full}_bump")
    compiled = procedural_expr.compile_bump_height_expression(
        normal_node,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=bump_img_resolver,
    )
    if compiled is None:
        _warn_once(
            state,
            f"RISE could not translate the Bump height source on '{material.name_full}' "
            "(only image textures and supported procedural expression graphs are supported).",
        )
        return None
    bump_img_resolver.commit()

    if used_generated_out is not None and compiled.uses_generated:
        used_generated_out.append(True)
    if mesh_attrs_out is not None and compiled.mesh_attributes:
        mesh_attrs_out.update(compiled.mesh_attributes)
    if handled_out is not None:
        handled_out.append(True)

    strength_input = _node_input(normal_node, "Strength")
    if strength_input is not None and any(not getattr(lk, "is_muted", False) for lk in strength_input.links):
        _warn_once(
            state,
            f"RISE ignores linked Bump Strength on '{material.name_full}' and uses its default value.",
        )
    distance_input = _node_input(normal_node, "Distance")
    if distance_input is not None and any(not getattr(lk, "is_muted", False) for lk in distance_input.links):
        _warn_once(
            state,
            f"RISE ignores linked Bump Distance on '{material.name_full}' and uses its default value.",
        )

    strength = _socket_default_float(normal_node, "Strength", 1.0)
    distance = _socket_default_float(normal_node, "Distance", 1.0)
    scale = _relief_modifier_scale(strength, distance, bool(getattr(normal_node, "invert", False)))
    if not math.isfinite(scale) or abs(scale) == 0.0:
        return None

    painter_name = _add_expression_painter(
        state,
        f"{material.name_full}_bump_height_expr",
        compiled.expression,
    )
    modifier_key = ("relief", painter_name, float(scale))
    if modifier_key in state.modifier_cache:
        return state.modifier_cache[modifier_key]

    modifier_name = _unique_name(state, "mod", f"{material.name_full}_relief")
    state.modifiers.append(
        ModifierData(
            name=modifier_name,
            kind=MODIFIER_RELIEF,
            source_painter_name=painter_name,
            scale=scale,
            # window <= 0 tells ReliefModifier (domain="surface") to derive
            # the central-difference half-step automatically from the pixel
            # footprint (fw/2, with 1e-3 fallback when no footprint exists).
            window=0.0,
            normalize=False,
        )
    )
    state.modifier_cache[modifier_key] = modifier_name
    return modifier_name


def _build_coat_normal_painter(
    material,
    normal_map_node,
    state: _ExportState,
    *,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
) -> tuple[str | None, float]:
    """Translate a Principled BSDF Coat Normal's ShaderNodeNormalMap
    into a registered COLOUR painter (NOT a modifier -- DL-192's
    `coat_normal_painter_name` is a MATERIAL parameter, consumed only
    by `coated_material`'s own coat lobe, unlike the surface's Normal
    input which perturbs the whole shading point via an object-level
    modifier).  Mirrors `_build_normal_map_modifier`'s own validation
    and painter-registration steps, supporting both direct image textures
    and expression-compiled color graphs (e.g. DirectX Invert G groups)."""
    if getattr(normal_map_node, "space", "TANGENT") != "TANGENT":
        _warn_once(state, f"RISE only supports tangent-space Normal Map nodes on '{material.name_full}' (Coat Normal).")
        return None, 1.0

    color_input = _node_input(normal_map_node, "Color")
    if color_input is None or not color_input.is_linked:
        _warn_once(state, f"RISE only supports image-driven Normal Map nodes on '{material.name_full}' (Coat Normal).")
        return None, 1.0

    strength = float(_socket_default_float(normal_map_node, "Strength", 1.0))

    if not _is_direct_simple_image_socket(color_input):
        img_resolver = _DeferredExprImageResolver(state, f"{material.name_full}_coat_normal")
        compiled = procedural_expr.compile_socket_expression(
            color_input,
            want_type="vec3",
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=img_resolver,
        )
        if compiled is not None:
            img_resolver.commit()
            if used_generated_out is not None and compiled.uses_generated:
                used_generated_out.append(True)
            if mesh_attrs_out is not None and compiled.mesh_attributes:
                mesh_attrs_out.update(compiled.mesh_attributes)
            painter_name = _add_expression_painter(state, f"{material.name_full}_coat_normal_expr", compiled.expression)
            return painter_name, strength

    image_node = _walk_socket_for_image_node(color_input)
    if image_node is None or image_node.bl_idname != "ShaderNodeTexImage":
        _warn_once(state, f"RISE only supports direct image textures as Coat Normal sources on '{material.name_full}'.")
        return None, 1.0

    texture_wrapper = _ImageSocketWrapper(image_node, colorspace_is_data=True, colorspace_name="Non-Color")
    validated = _validate_texture_wrapper(texture_wrapper, state, f"the Coat Normal on '{material.name_full}'")
    if not validated:
        return None, 1.0

    filepath, _color_space_ignored = validated
    kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
    # Same Rec709RGB_Linear (`COLOR_SPACE_LINEAR = 0`) verbatim-passthrough
    # convention `_build_normal_map_modifier` uses for the object-level
    # normal map -- see that function's own comment.
    painter_name = _add_texture_painter(
        state,
        f"{material.name_full}_coat_normal",
        filepath,
        kind,
        COLOR_SPACE_LINEAR,
        wrap_mode=_tex_image_wrap_mode(image_node),
    )
    painter_name = _maybe_wrap_with_uv_transform(
        state, f"{material.name_full}_coat_normal", painter_name, texture_wrapper)
    return painter_name, strength


def _build_normal_map_modifier(
    material,
    normal_map_node,
    state: _ExportState,
    *,
    bbox_min=None,
    bbox_max=None,
    material_pass_index: float = 0.0,
    used_generated_out: list[bool] | None = None,
    mesh_attrs_out: set[str] | None = None,
    attr_types: dict[str, str] | None = None,
) -> str | None:
    """Translate a ShaderNodeNormalMap into a RISE NormalMap modifier.

    Mirrors glTF KHR normalTexture semantics: the painter must be
    loaded as Rec709RGB_Linear (`COLOR_SPACE_LINEAR = 0`) so the
    RGB-encoded tangent stays bit-exact, and the strength scalar maps
    to normalTexture.scale.  Supports both direct image textures and
    expression-compiled normal color graphs (e.g. DirectX Invert G groups)."""

    if getattr(normal_map_node, "space", "TANGENT") != "TANGENT":
        _warn_once(state, f"RISE only supports tangent-space Normal Map nodes on '{material.name_full}'.")
        return None

    color_input = _node_input(normal_map_node, "Color")
    if color_input is None or not color_input.is_linked:
        _warn_once(state, f"RISE only supports image-driven Normal Map nodes on '{material.name_full}'.")
        return None

    painter_name: str | None = None
    if not _is_direct_simple_image_socket(color_input):
        img_resolver = _DeferredExprImageResolver(state, f"{material.name_full}_normal")
        compiled = procedural_expr.compile_socket_expression(
            color_input,
            want_type="vec3",
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=img_resolver,
        )
        if compiled is not None:
            img_resolver.commit()
            if used_generated_out is not None and compiled.uses_generated:
                used_generated_out.append(True)
            if mesh_attrs_out is not None and compiled.mesh_attributes:
                mesh_attrs_out.update(compiled.mesh_attributes)
            painter_name = _add_expression_painter(state, f"{material.name_full}_normal_expr", compiled.expression)

    if painter_name is None:
        image_node = _walk_socket_for_image_node(color_input)
        if image_node is None or image_node.bl_idname != "ShaderNodeTexImage":
            _warn_once(state, f"RISE only supports direct image textures as Normal Map sources on '{material.name_full}'.")
            return None

        texture_wrapper = _ImageSocketWrapper(image_node, colorspace_is_data=True, colorspace_name="Non-Color")
        validated = _validate_texture_wrapper(texture_wrapper, state, f"the Normal Map on '{material.name_full}'")
        if not validated:
            return None

        filepath, _color_space_ignored = validated
        kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
        # Normal-map painters MUST be loaded as Rec709RGB_Linear
        # (`COLOR_SPACE_LINEAR = 0`) -- RISE's native working space -- to
        # bypass both gamma decoding AND any colour-space primary matrix
        # that would warp the encoded tangent (matching GLTFSceneImporter).
        painter_name = _add_texture_painter(
            state,
            f"{material.name_full}_normal",
            filepath,
            kind,
            COLOR_SPACE_LINEAR,
            wrap_mode=_tex_image_wrap_mode(image_node),
        )
        painter_name = _maybe_wrap_with_uv_transform(
            state, f"{material.name_full}_normal", painter_name, texture_wrapper)

    strength = _socket_default_float(normal_map_node, "Strength", 1.0)
    modifier_key = ("normal_map", painter_name, round(strength, 6))
    if modifier_key in state.modifier_cache:
        return state.modifier_cache[modifier_key]

    modifier_name = _unique_name(state, "mod", f"{material.name_full}_normal")
    state.modifiers.append(
        ModifierData(
            name=modifier_name,
            kind=MODIFIER_NORMAL_MAP,
            source_painter_name=painter_name,
            scale=float(strength),
            window=0.0,
        )
    )
    state.modifier_cache[modifier_key] = modifier_name
    return modifier_name


# Back-compat alias (older code path called the bump-only helper).
def _direct_bump_modifier(material, wrapper: PrincipledBSDFWrapper, state: _ExportState) -> str | None:
    return _direct_normal_modifier(material, wrapper, state)


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
    from .tangent_bake import find_material_output
    return find_material_output(material)


def _find_world_output(world):
    if not uses_nodes(world):
        return None
    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputWorld" and getattr(node, "is_active_output", False):
            return node
    for node in world.node_tree.nodes:
        if node.bl_idname == "ShaderNodeOutputWorld":
            return node
    return None


def _material_payload(
    material,
    state: _ExportState,
    mesh_bbox=None,
    attr_types: dict[str, str] | None = None,
) -> _MaterialBinding:
    key = _pointer_key(material)
    bbox_key = None
    if mesh_bbox is not None:
        bmin, bmax = mesh_bbox
        bbox_key = tuple(round(float(v), 6) for v in (*bmin, *bmax))
    float2_sig = tuple(sorted(k for k, v in (attr_types or {}).items() if v == "FLOAT2"))
    if ("gen_attr", key, bbox_key, float2_sig) in state.material_map:
        return state.material_map[("gen_attr", key, bbox_key, float2_sig)]
    if ("attr", key, float2_sig) in state.material_map:
        return state.material_map[("attr", key, float2_sig)]
    if ("gen", key, bbox_key) in state.material_map:
        return state.material_map[("gen", key, bbox_key)]
    if key in state.material_map:
        return state.material_map[key]

    if material is None:
        return _default_surface_binding(state)

    if not uses_nodes(material):
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

    # Consumer of the explicit manual channel-image cache; no automatic bake.
    # Only an existing diffuse PNG selects this synthetic dielectric branch;
    # roughness and normal are optional. Any-channel cache can affect the
    # operator skip without satisfying this branch. No freshness hash is checked.
    # DIFFUSE COLOR is not complete shader response or conductor F0. Metallic0
    # and the three-channel approximation drop metallic/transmission/sheen/
    # coat/emission/subsurface/alpha lobes instead of reconstructing them.
    try:
        from . import material_bake as _material_bake
        baked = _material_bake.baked_paths(material)
    except ImportError:
        baked = None
    if baked is not None and "diffuse" in baked:
        diffuse_painter = _add_texture_painter(
            state,
            f"{material.name_full}_baked_diffuse",
            baked["diffuse"],
            PAINTER_TEXTURE_PNG,
            COLOR_SPACE_SRGB,
        )
        # Roughness — optional baked channel.  When absent, the
        # synthesised Principled uses the constant 0.5 default so the
        # baked diffuse isn't paired with a too-mirror-like surface.
        if "roughness" in baked:
            roughness_painter = _add_texture_painter(
                state,
                f"{material.name_full}_baked_roughness",
                baked["roughness"],
                PAINTER_TEXTURE_PNG,
                COLOR_SPACE_LINEAR,
            )
        else:
            roughness_painter = _add_uniform_painter(
                state, f"{material.name_full}_baked_rough_const", (0.5, 0.5, 0.5))
        # Normal map — when absent, no normal-map modifier is wired.
        modifier_name: str | None = None
        if "normal" in baked:
            normal_painter = _add_texture_painter(
                state,
                f"{material.name_full}_baked_normal",
                baked["normal"],
                PAINTER_TEXTURE_PNG,
                COLOR_SPACE_LINEAR,
            )
            modifier_name = _unique_name(state, "mod", f"{material.name_full}_normalmap")
            state.modifiers.append(
                ModifierData(
                    name=modifier_name,
                    kind=MODIFIER_NORMAL_MAP,
                    source_painter_name=normal_painter,
                    scale=1.0,
                    window=0.0,
                )
            )

        # Supply the required bridge slot as metallic0. DIFFUSE COLOR does
        # not absorb conductor F0; a fully metallic shader can bake black.
        # This synthetic dielectric is a lossy approximation, not the original
        # material's full response. No metallic-channel bake is implemented.
        metallic_painter = _add_uniform_painter(
            state, f"{material.name_full}_baked_metallic_const", (0.0, 0.0, 0.0))

        payload = MaterialData(
            name=_unique_name(state, "mat", material.name_full),
            model=MATERIAL_PBR_METALLIC_ROUGHNESS,
            base_color_painter_name=diffuse_painter,
            metallic_painter_name=metallic_painter,
            roughness_painter_name=roughness_painter,
            double_sided=not getattr(material, "use_backface_culling", False),
        )
        state.materials.append(payload)
        binding = _MaterialBinding(
            surface_material_name=payload.name,
            modifier_name=modifier_name,
            interior_medium_name=None,
            double_sided=payload.double_sided,
        )
        state.material_map[key] = binding
        _warn_once(
            state,
            f"RISE: '{material.name_full}' using baked PBR textures "
            f"({', '.join(sorted(baked.keys()))}) — re-run the "
            f"Bake operator after material edits.",
        )
        return binding

    wrapper = PrincipledBSDFWrapper(material, is_readonly=True)
    # Official wrapper scans node order without renderer-target semantics.
    # Bind its read-only accessors to the SAME selected Surface/socket as the
    # classifier and direction producer, retaining supported texture access.
    from .material_bake import (
        MixLeafSpec,
        _find_principled_through_surface,
        find_simple_bsdf_through_surface,
        flatten_mix_shader_surface,
    )
    selected_output = _find_material_output(material)
    wrapper.node_out = selected_output
    wrapper.node_principled_bsdf = _find_principled_through_surface(selected_output)
    if wrapper.node_principled_bsdf is None:
        # Standalone Diffuse / Glossy (Anisotropic) / Emission surface root.
        simple_bsdf = find_simple_bsdf_through_surface(selected_output)
        if simple_bsdf is not None:
            leaf_kind, bsdf_node = simple_bsdf
            leaf_spec = MixLeafSpec(kind=leaf_kind, node=bsdf_node, factors=())
            mat_name, mod_name, used_generated_coords, mesh_attrs = _export_simple_bsdf_leaf(
                material,
                leaf_spec,
                state,
                label_prefix=material.name_full,
                mesh_bbox=mesh_bbox,
                attr_types=attr_types,
            )
            if mat_name is not None:
                binding = _MaterialBinding(
                    surface_material_name=mat_name,
                    modifier_name=mod_name,
                    interior_medium_name=None,
                    double_sided=not getattr(material, "use_backface_culling", False),
                    mesh_attributes=tuple(sorted(mesh_attrs)),
                )
                uses_mesh_attrs = bool(binding.mesh_attributes)
                if used_generated_coords and uses_mesh_attrs:
                    cache_key = ("gen_attr", key, bbox_key, float2_sig)
                elif uses_mesh_attrs:
                    cache_key = ("attr", key, float2_sig)
                elif used_generated_coords:
                    cache_key = ("gen", key, bbox_key)
                else:
                    cache_key = key
                state.material_map[cache_key] = binding
                return binding

        # ABI v21: native ShaderNodeMixShader -> RISE `mix_material` with
        # per-leaf modifiers and pass-through support.
        mix_leaves = flatten_mix_shader_surface(selected_output)
        if mix_leaves is not None:
            mix_result = _export_mix_shader_material(
                material, mix_leaves, state, mesh_bbox=mesh_bbox, attr_types=attr_types
            )
            if mix_result is not None:
                binding, used_generated_coords = mix_result
                # R2 P1-1: any material referencing a ShaderNodeAttribute must
                # key on float2_sig (even when empty) so a non-FLOAT2 mesh
                # cannot poison bare `key` for a later FLOAT2 mesh sharing the material.
                uses_mesh_attrs = bool(binding.mesh_attributes)
                if used_generated_coords and uses_mesh_attrs:
                    cache_key = ("gen_attr", key, bbox_key, float2_sig)
                elif uses_mesh_attrs:
                    cache_key = ("attr", key, float2_sig)
                elif used_generated_coords:
                    cache_key = ("gen", key, bbox_key)
                else:
                    cache_key = key
                state.material_map[cache_key] = binding
                return binding

        # PrincipledBSDFWrapper couldn't trace from Material Output
        # Surface back to a single Principled BSDF — usually because
        # the Material Output is fed by a Mix Shader / Add Shader
        # combining multiple BSDFs.  Fall back to the FIRST Principled
        # BSDF anywhere in the graph; lossy (we drop the mix factor)
        # but produces sensible per-material results, vs. the prior
        # behaviour of using the viewport-grey `material.diffuse_color`.
        has_outputs = any(n.bl_idname == 'ShaderNodeOutputMaterial' for n in material.node_tree.nodes)
        any_principled = _find_any_principled_bsdf(material) if selected_output is not None or not has_outputs else None
        if any_principled is not None:
            _warn_once(
                state,
                f"RISE approximated the Mix-Shader chain on '{material.name_full}' with its first Principled BSDF.",
            )
            wrapper = _FallbackPrincipledWrapper(any_principled)
        else:
            _warn_once(
                state,
                f"RISE falls back to viewport material values when '{material.name_full}' has no supported Principled BSDF chain on the selected Cycles/ALL output.",
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

    binding, used_generated_coords = _export_principled_binding(
        material,
        wrapper,
        state,
        label_prefix=material.name_full,
        mesh_bbox=mesh_bbox,
        allow_volume=True,
        allow_dielectric=True,
        attr_types=attr_types,
    )
    uses_mesh_attrs = bool(binding.mesh_attributes)
    if used_generated_coords and uses_mesh_attrs:
        cache_key = ("gen_attr", key, bbox_key, float2_sig)
    elif uses_mesh_attrs:
        cache_key = ("attr", key, float2_sig)
    elif used_generated_coords:
        cache_key = ("gen", key, bbox_key)
    else:
        cache_key = key
    state.material_map[cache_key] = binding
    return binding


def _is_pure_float_literal(expr: str) -> bool:
    if "\n" in expr or not expr.strip():
        return False
    try:
        v = float(expr.strip())
        return math.isfinite(v)
    except ValueError:
        return False


def _export_simple_bsdf_leaf(
    material,
    leaf,
    state: _ExportState,
    *,
    label_prefix: str,
    mesh_bbox=None,
    attr_types: dict[str, str] | None = None,
) -> tuple[str | None, str | None, bool, set[str]]:
    """Export one non-Principled leaf of a ``ShaderNodeMixShader`` tree.

    Returns ``(material_name_or_none, modifier_name_or_none, used_generated, mesh_attrs)``.
    """
    bbox_min = mesh_bbox[0] if mesh_bbox is not None else None
    bbox_max = mesh_bbox[1] if mesh_bbox is not None else None
    material_pass_index = float(getattr(material, "pass_index", 0))
    double_sided = not getattr(material, "use_backface_culling", False)
    used_generated = False
    mesh_attrs: set[str] = set()

    if leaf.kind == "absorber" or leaf.node is None:
        return (None, None, False, mesh_attrs)

    node = leaf.node

    def _leaf_expr(socket_name: str, suffix: str, want_type: str, scale=None) -> str | None:
        nonlocal used_generated
        sock = _node_input(node, socket_name)
        img_resolver = _DeferredExprImageResolver(state, f"{label_prefix}_{suffix}")
        compiled = procedural_expr.compile_socket_expression(
            sock,
            want_type=want_type,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            scale=scale,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=img_resolver,
        )
        if compiled is None:
            return None
        img_resolver.commit()
        if compiled.uses_generated:
            used_generated = True
        if compiled.mesh_attributes:
            mesh_attrs.update(compiled.mesh_attributes)
        return _add_expression_painter(state, f"{label_prefix}_{suffix}", compiled.expression)

    if leaf.kind == "diffuse":
        diff_rough = _socket_default_float(node, "Roughness", 0.0)
        diff_rough_sock = _node_input(node, "Roughness")
        if diff_rough > 1e-4 or (diff_rough_sock is not None and diff_rough_sock.is_linked):
            _warn_once(
                state,
                f"RISE approximates Diffuse BSDF Roughness as Lambertian on '{material.name_full}'.",
            )
        col = _socket_default_color(node, "Color", (0.8, 0.8, 0.8))
        col_sock = _node_input(node, "Color")
        col_expr = None if _is_direct_simple_image_socket(col_sock) else _leaf_expr("Color", "diff_expr", "vec3")
        col_tex = None if col_expr is not None else _maybe_resolve_socket_texture(node, "Color", None, colorspace_is_data=False)
        if col_tex is None and col_expr is None and col_sock is not None and any(
            not getattr(lk, "is_muted", False) for lk in col_sock.links
        ):
            _warn_once(
                state,
                f"RISE could not translate the linked Diffuse Color on '{material.name_full}'; using its default value.",
            )
        col_painter = col_expr if col_expr is not None else _color_or_texture_painter(
            state, f"{label_prefix}_diff", col, texture_wrapper=col_tex
        )
        norm_gen: list[bool] = []
        mod_name = _direct_normal_modifier_for_node(
            material, node, state,
            bbox_min=bbox_min, bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=norm_gen, mesh_attrs_out=mesh_attrs,
            attr_types=attr_types,
        )
        if norm_gen:
            used_generated = True
        mat_name = _unique_name(state, "mat", label_prefix)
        state.materials.append(MaterialData(
            name=mat_name,
            model=MATERIAL_LAMBERT,
            diffuse_painter_name=col_painter,
            double_sided=double_sided,
        ))
        return (mat_name, mod_name, used_generated, mesh_attrs)

    if leaf.kind == "glossy":
        aniso_val = _socket_default_float(node, "Anisotropy", 0.0)
        aniso_sock = _node_input(node, "Anisotropy")
        tang_sock = _node_input(node, "Tangent")
        if abs(aniso_val) > 1e-4 or (aniso_sock is not None and aniso_sock.is_linked) or (tang_sock is not None and tang_sock.is_linked):
            _warn_once(
                state,
                f"RISE ignores Glossy/Anisotropic BSDF Anisotropy/Tangent on '{material.name_full}'.",
            )
        col = _socket_default_color(node, "Color", (0.8, 0.8, 0.8))
        col_sock = _node_input(node, "Color")
        col_expr = None if _is_direct_simple_image_socket(col_sock) else _leaf_expr("Color", "glossy_col_expr", "vec3")
        col_tex = None if col_expr is not None else _maybe_resolve_socket_texture(node, "Color", None, colorspace_is_data=False)
        col_has_links = bool(col_sock is not None and col_sock.is_linked and any(not getattr(lk, "is_muted", False) for lk in col_sock.links))
        if col_has_links and col_expr is None and col_tex is None:
            _warn_once(
                state,
                f"RISE reads Glossy BSDF Color on '{material.name_full}' at its socket default; "
                f"the linked graph could not be translated.",
            )
        col_painter = col_expr if col_expr is not None else _color_or_texture_painter(
            state, f"{label_prefix}_glossy_col", col, texture_wrapper=col_tex
        )
        rough = _clamp01(_socket_default_float(node, "Roughness", 0.5))
        rough_sock = _node_input(node, "Roughness")
        rough_expr = None if _is_direct_simple_image_socket(rough_sock) else _leaf_expr("Roughness", "glossy_rough_expr", "scalar")
        rough_tex = None if rough_expr is not None else _maybe_resolve_socket_texture(node, "Roughness", None, colorspace_is_data=True)
        rough_has_links = bool(rough_sock is not None and rough_sock.is_linked and any(not getattr(lk, "is_muted", False) for lk in rough_sock.links))
        if rough_has_links and rough_expr is None and rough_tex is None:
            _warn_once(
                state,
                f"RISE reads Glossy BSDF Roughness on '{material.name_full}' at its socket default; "
                f"the linked graph could not be translated.",
            )
        rough_painter = rough_expr if rough_expr is not None else _scalar_or_texture_painter(
            state, f"{label_prefix}_glossy_rough", rough, texture_wrapper=rough_tex
        )
        metallic_painter = _add_uniform_painter(state, f"{label_prefix}_glossy_metal", (1.0, 1.0, 1.0))
        norm_gen = []
        mod_name = _direct_normal_modifier_for_node(
            material, node, state,
            bbox_min=bbox_min, bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            used_generated_out=norm_gen, mesh_attrs_out=mesh_attrs,
            attr_types=attr_types,
        )
        if norm_gen:
            used_generated = True
        mat_name = _unique_name(state, "mat", label_prefix)
        state.materials.append(MaterialData(
            name=mat_name,
            model=MATERIAL_PBR_METALLIC_ROUGHNESS,
            base_color_painter_name=col_painter,
            metallic_painter_name=metallic_painter,
            roughness_painter_name=rough_painter,
            double_sided=double_sided,
        ))
        return (mat_name, mod_name, used_generated, mesh_attrs)

    if leaf.kind == "emission":
        ec = _socket_default_color(node, "Color", (1.0, 1.0, 1.0))
        es = max(0.0, _socket_default_float(node, "Strength", 1.0))
        ec_sock = _node_input(node, "Color")
        es_sock = _node_input(node, "Strength")
        em_painter = None
        if ec_sock is not None and es_sock is not None and ec_sock.is_linked and es_sock.is_linked:
            pair_resolver = _DeferredExprImageResolver(state, f"{label_prefix}_emit_pair")
            paired = procedural_expr.compile_socket_product_expression(
                ec_sock,
                es_sock,
                bbox_min=bbox_min,
                bbox_max=bbox_max,
                material_pass_index=material_pass_index,
                attr_types=attr_types,
                image_resolver=pair_resolver,
            )
            if paired is not None:
                pair_resolver.commit()
                if paired.uses_generated:
                    used_generated = True
                if paired.mesh_attributes:
                    mesh_attrs.update(paired.mesh_attributes)
                em_painter = _add_expression_painter(state, f"{label_prefix}_emit_expr", paired.expression)
        if em_painter is None:
            ec_expr = (
                None if _is_direct_simple_image_socket(ec_sock)
                else (_leaf_expr("Color", "emit_expr", "vec3", scale=(es, es, es)) if es > 1e-6 else None)
            )
            ec_tex = None if ec_expr is not None else _maybe_resolve_socket_texture(node, "Color", None, colorspace_is_data=False)
            es_expr = (
                None if (ec_expr is not None or ec_tex is not None or _is_direct_simple_image_socket(es_sock))
                else _leaf_expr("Strength", "emit_str_expr", "scalar", scale=ec)
            )
            es_tex = None if (ec_expr is not None or ec_tex is not None or es_expr is not None) else _maybe_resolve_socket_texture(node, "Strength", None, colorspace_is_data=True)
            if ec_expr is not None:
                em_painter = ec_expr
            elif ec_tex is not None:
                em_painter = _color_or_texture_painter(
                    state, f"{label_prefix}_emit", ec, texture_wrapper=ec_tex, scale=(es, es, es)
                )
            elif es_expr is not None:
                em_painter = es_expr
            elif es_tex is not None:
                em_painter = _scalar_or_texture_painter(
                    state, f"{label_prefix}_emit", es, texture_wrapper=es_tex, scale=max(ec)
                )
            elif es > 1e-6 and max(ec) > 1e-6:
                em_painter = _add_uniform_painter(
                    state, f"{label_prefix}_emit", (ec[0] * es, ec[1] * es, ec[2] * es)
                )
        black_painter = _add_uniform_painter(state, f"{label_prefix}_emit_black", (0.0, 0.0, 0.0))
        mat_name = _unique_name(state, "mat", label_prefix)
        state.materials.append(MaterialData(
            name=mat_name,
            model=MATERIAL_LAMBERT,
            diffuse_painter_name=black_painter,
            emission_painter_name=em_painter,
            double_sided=double_sided,
        ))
        return (mat_name, None, used_generated, mesh_attrs)

    return (None, None, False, mesh_attrs)


def _export_mix_shader_material(
    material,
    mix_leaves,
    state: _ExportState,
    *,
    mesh_bbox=None,
    attr_types: dict[str, str] | None = None,
) -> tuple[_MaterialBinding, bool] | None:
    """Export a flattened ``ShaderNodeMixShader`` tree into RISE ``MATERIAL_MIX``."""
    bbox_min = mesh_bbox[0] if mesh_bbox is not None else None
    bbox_max = mesh_bbox[1] if mesh_bbox is not None else None
    material_pass_index = float(getattr(material, "pass_index", 0))
    double_sided = not getattr(material, "use_backface_culling", False)

    # Compile ALL leaf weight expressions AND combined passthrough expression
    # upfront so any unsupported factor or VM slot overflow fails cleanly
    # without emitting orphaned child materials into state.materials (R1 B-P1-2).
    compiled_weights: list[procedural_expr.CompiledExpression] = []
    transparent_factor_lists: list[list] = []
    mix_img_resolver = _DeferredExprImageResolver(state, f"{material.name_full}_mix")
    for leaf in mix_leaves:
        compiled = procedural_expr.compile_mix_weight_expression(
            list(leaf.factors),
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=mix_img_resolver,
        )
        if compiled is None:
            return None
        compiled_weights.append(compiled)
        if leaf.kind == "transparent":
            transparent_factor_lists.append(list(leaf.factors))

    compiled_passthrough: procedural_expr.CompiledExpression | None = None
    if transparent_factor_lists:
        compiled_passthrough = procedural_expr.compile_mix_passthrough_expression(
            transparent_factor_lists,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=mix_img_resolver,
        )
        if compiled_passthrough is None:
            return None
    mix_img_resolver.commit()

    used_generated_coords = False
    used_mesh_attributes: set[str] = set()
    mix_children: list[MixChildData] = []

    for idx, (leaf, w_comp) in enumerate(zip(mix_leaves, compiled_weights)):
        if leaf.kind == "transparent":
            continue
        if w_comp.uses_generated:
            used_generated_coords = True
        if w_comp.mesh_attributes:
            used_mesh_attributes.update(w_comp.mesh_attributes)

        leaf_prefix = f"{material.name_full}_leaf{idx}"
        if leaf.kind == "principled":
            leaf_binding, leaf_gen = _export_principled_binding(
                material,
                _FallbackPrincipledWrapper(leaf.node),
                state,
                label_prefix=leaf_prefix,
                mesh_bbox=mesh_bbox,
                allow_volume=False,
                allow_dielectric=False,
                attr_types=attr_types,
            )
            leaf_mat_name = leaf_binding.surface_material_name
            leaf_mod_name = leaf_binding.modifier_name
            if leaf_gen:
                used_generated_coords = True
            if leaf_binding.mesh_attributes:
                used_mesh_attributes.update(leaf_binding.mesh_attributes)
        else:
            leaf_mat_name, leaf_mod_name, leaf_gen, leaf_attrs = _export_simple_bsdf_leaf(
                material,
                leaf,
                state,
                label_prefix=leaf_prefix,
                mesh_bbox=mesh_bbox,
                attr_types=attr_types,
            )
            if leaf_gen:
                used_generated_coords = True
            if leaf_attrs:
                used_mesh_attributes.update(leaf_attrs)

        if _is_pure_float_literal(w_comp.expression):
            weight_spec = w_comp.expression.strip()
        else:
            weight_spec = _add_expression_painter(
                state, f"{material.name_full}_mix_w{idx}", w_comp.expression
            )
        mix_children.append(
            MixChildData(
                material_name=leaf_mat_name,
                weight_painter_name=weight_spec,
                modifier_name=leaf_mod_name,
            )
        )

    if not mix_children:
        # Pure transparent-only MixShader tree (e.g. Mix(TransparentA, TransparentB)):
        # MixMaterial requires >= 1 child, so emit a 0-weight absorber child while
        # `passthrough_spec` carries the full straight-through transmission weight.
        mix_children.append(
            MixChildData(
                material_name=None,
                weight_painter_name="0.0",
                modifier_name=None,
            )
        )

    passthrough_spec: str | None = None
    if compiled_passthrough is not None:
        if compiled_passthrough.uses_generated:
            used_generated_coords = True
        if compiled_passthrough.mesh_attributes:
            used_mesh_attributes.update(compiled_passthrough.mesh_attributes)
        if _is_pure_float_literal(compiled_passthrough.expression):
            pt_val = float(compiled_passthrough.expression.strip())
            passthrough_spec = compiled_passthrough.expression.strip() if pt_val > 0.0 else None
        else:
            passthrough_spec = _add_expression_painter(
                state, f"{material.name_full}_mix_passthrough", compiled_passthrough.expression
            )

    payload = MaterialData(
        name=_unique_name(state, "mat", material.name_full),
        model=MATERIAL_MIX,
        double_sided=double_sided,
        mix_children=mix_children,
        mix_passthrough_painter_name=passthrough_spec,
    )
    state.materials.append(payload)
    binding = _MaterialBinding(
        surface_material_name=payload.name,
        modifier_name=None,
        interior_medium_name=None,
        double_sided=double_sided,
        shader_name=None,
        mesh_attributes=tuple(sorted(used_mesh_attributes)),
    )
    return (binding, used_generated_coords)


def _export_principled_binding(
    material,
    wrapper,
    state: _ExportState,
    *,
    label_prefix: str,
    mesh_bbox=None,
    allow_volume: bool = True,
    allow_dielectric: bool = True,
    attr_types: dict[str, str] | None = None,
) -> tuple[_MaterialBinding, bool]:
    # All per-socket lookups go through `_maybe_resolve_socket_texture`
    # so reroute / RGB-Curves / Mix chains feeding the Principled BSDF
    # still produce per-material textures.  `PrincipledBSDFWrapper`'s
    # `.X_texture` accessors only handle direct image-to-socket links
    # and silently return the socket *default* when anything else sits
    # in between — that was the root cause of every material in the
    # lone-monk demo scene collapsing onto a single shared painter via
    # `_add_uniform_painter`'s colour-keyed cache.
    #
    # When no image texture is found on a linked socket, try compiling the
    # upstream procedural shader subgraph (Voronoi, Noise, White Noise,
    # Mix, Hue/Sat, Mapping, TexCoord, ColorRamp, ...) directly into a RISE
    # `expression_painter` (ABI v19).
    principled_node = wrapper.node_principled_bsdf
    bbox_min = mesh_bbox[0] if mesh_bbox is not None else None
    bbox_max = mesh_bbox[1] if mesh_bbox is not None else None
    material_pass_index = float(getattr(material, "pass_index", 0))
    translated_sockets: set[str] = set()
    used_generated_coords = False
    used_mesh_attributes: set[str] = set()

    def _try_expr_painter(
        socket_name: str,
        label: str,
        want_type: str,
        scale=None,
    ) -> str | None:
        nonlocal used_generated_coords
        sock = _node_input(principled_node, socket_name)
        img_resolver = _DeferredExprImageResolver(state, label)
        compiled = procedural_expr.compile_socket_expression(
            sock,
            want_type=want_type,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            scale=scale,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=img_resolver,
        )
        if compiled is None:
            return None
        img_resolver.commit()
        translated_sockets.add(socket_name)
        if compiled.uses_generated:
            used_generated_coords = True
        if compiled.mesh_attributes:
            used_mesh_attributes.update(compiled.mesh_attributes)
        return _add_expression_painter(state, label, compiled.expression)

    def _resolve_color_or_expr(
        socket_name: str,
        label: str,
        default_color: tuple[float, float, float],
        wrapper_tex,
        scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ):
        sock = _node_input(principled_node, socket_name)
        if not _is_direct_simple_image_socket(sock):
            expr_pnt = _try_expr_painter(socket_name, f"{label}_expr", "vec3", scale=scale)
            if expr_pnt is not None:
                return expr_pnt, None
        tex = _maybe_resolve_socket_texture(principled_node, socket_name, wrapper_tex, colorspace_is_data=False)
        return _color_or_texture_painter(state, label, default_color, texture_wrapper=tex, scale=scale), tex

    def _resolve_scalar_or_expr(
        socket_name: str,
        label: str,
        default_val: float,
        wrapper_tex,
        scale: float = 1.0,
    ):
        sock = _node_input(principled_node, socket_name)
        if not _is_direct_simple_image_socket(sock):
            expr_pnt = _try_expr_painter(socket_name, f"{label}_expr", "scalar", scale=scale)
            if expr_pnt is not None:
                return expr_pnt, None
        tex = _maybe_resolve_socket_texture(principled_node, socket_name, wrapper_tex, colorspace_is_data=True)
        return _scalar_or_texture_painter(state, label, default_val, texture_wrapper=tex, scale=scale), tex

    base_color = _float_color3(wrapper.base_color)
    base_painter, base_color_texture = _resolve_color_or_expr(
        "Base Color", f"{material.name_full}_base", base_color, wrapper.base_color_texture
    )
    base_expr = base_painter if "Base Color" in translated_sockets else None

    metallic = _clamp01(wrapper.metallic)
    metallic_painter, metallic_texture = _resolve_scalar_or_expr(
        "Metallic", f"{material.name_full}_metallic", metallic, wrapper.metallic_texture
    )

    # RISE_API_CreatePBRMetallicRoughnessMaterial squares roughness
    # internally into the GGX alpha, so we hand it the raw roughness
    # value (or texture) — no need to pre-square here.
    roughness = _clamp01(wrapper.roughness)
    roughness_painter, roughness_texture = _resolve_scalar_or_expr(
        "Roughness", f"{material.name_full}_roughness", roughness, wrapper.roughness_texture
    )

    # Specular IOR Level slot — KHR_materials_specular `specular_factor`.
    # Cycles' default is 0.5 which produces F0=0.04 for dielectrics; the
    # PBR-MR factory uses `0.04 * specular_factor`, so a Cycles value of
    # 0.5 needs to map to RISE's specular_factor=1.0.  Multiplying by 2
    # bridges the conventions exactly.
    specular = _clamp01(wrapper.specular)
    specular_factor_painter, specular_texture = _resolve_scalar_or_expr(
        "Specular IOR Level", f"{material.name_full}_specular_factor", specular, wrapper.specular_texture, scale=2.0
    )

    # Specular Tint slot -- KHR_materials_specular `specularColorFactor`
    # (DL-151, docs/DEBT_LEDGER.md; no source-doc heading, opened by the
    # debt-dl18 slice's sibling audit while confirming DL-18's own
    # closure).  `specular_color_painter_name` has been in the ABI since
    # Landing 7 and `add_pbr_metallic_roughness_material` already
    # forwards it straight through to `Job::AddPBRMetallicRoughnessMaterial`'s
    # `specular_color` argument -- see that function's KHR_materials_specular
    # blend chain (`F0_dielectric = 0.04 * specular_color * specular_factor`)
    # and `GLTFSceneImporter.cpp`'s identical `KHR_materials_specular`
    # handling, which this mapping mirrors.  The gap was exporter-side
    # only: nothing here ever read Blender's "Specular Tint" socket.
    #
    # Blender 4.x's Principled BSDF exposes "Specular Tint" as an RGB
    # colour socket, authored directly (default white = no tint).
    # Blender 3.x's Principled BSDF used the SAME socket NAME for a
    # float 0..1 slider that blends white -> base colour instead --
    # matching Blender's own Cycles shader (`node_principled_bsdf.osl`
    # / `svm_node_principled` pre-4.0): `spec_tint = mix(white,
    # base_color, specular_tint)`.  This add-on's `bl_info` (`__init__.py`)
    # declares Blender 4.0 as the minimum supported version, so the
    # float branch is not reachable through this add-on today -- kept
    # for robustness (dispatched on the socket's own `.type`, not a
    # `bpy.app.version` check, matching this file's existing
    # `inp.type == "RGBA"` convention elsewhere) since a live Blender
    # 3.x Principled node has not been independently re-verified from
    # this sandbox (no `bpy` available here -- same hedge as the
    # "Velvet (legacy `Velvet BSDF` node)" section of
    # docs/BLENDER_MATERIAL_TRANSLATION.md).
    specular_tint_socket = _node_input(principled_node, "Specular Tint")
    specular_color_painter = None
    if specular_tint_socket is not None and specular_tint_socket.type == "VALUE":
        if specular_tint_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Specular Tint on '{material.name_full}' at its socket default; "
                f"a linked/textured pre-4.0 float tint is not sampled.",
            )
        specular_tint_amount = _clamp01(_socket_default_float(principled_node, "Specular Tint", 0.0))
        if specular_tint_amount > 1e-4:
            tinted_color = tuple(
                1.0 + specular_tint_amount * (base_color[index] - 1.0) for index in range(3)
            )
            specular_color_painter = _add_uniform_painter(
                state, f"{material.name_full}_specular_tint", tinted_color,
            )
    elif specular_tint_socket is not None:
        specular_tint_default = _socket_default_color(principled_node, "Specular Tint", (1.0, 1.0, 1.0))
        specular_tint_expr = (
            None if _is_direct_simple_image_socket(specular_tint_socket)
            else _try_expr_painter("Specular Tint", f"{material.name_full}_specular_tint_expr", "vec3")
        )
        if specular_tint_expr is not None:
            specular_color_painter = specular_tint_expr
        else:
            specular_tint_texture = _maybe_resolve_socket_texture(
                principled_node, "Specular Tint", None, colorspace_is_data=False,
            )
            is_default_white = (
                specular_tint_texture is None
                and abs(specular_tint_default[0] - 1.0) < 1e-4
                and abs(specular_tint_default[1] - 1.0) < 1e-4
                and abs(specular_tint_default[2] - 1.0) < 1e-4
            )
            if not is_default_white:
                specular_color_painter = _color_or_texture_painter(
                    state,
                    f"{material.name_full}_specular_tint",
                    specular_tint_default,
                    texture_wrapper=specular_tint_texture,
                )

    # Anisotropy and rotation — read sockets directly because the
    # PrincipledBSDFWrapper doesn't surface them.
    anisotropy_value = _clamp01(_socket_default_float(principled_node, "Anisotropic", 0.0))
    # DL-208 (docs/DEBT_LEDGER.md): Blender's "Anisotropic Rotation"
    # socket is a [0,1] FRACTION OF A FULL TURN (Cycles applies
    # `2*pi*value` internally) -- NOT radians, unlike every other
    # angle-typed socket this file reads (e.g. Principled Hair BSDF's
    # "Offset" a few hundred lines below, already radians; a
    # ShaderNodeVectorRotate "Angle" socket below, also radians).
    # RISE's `tangent_rotation`/`tangent_rotation_scalar` are radians
    # (`MicrofacetUtils::RotateTangent` calls raw cos/sin on the value;
    # `weave_rotation`'s own descriptor says radians), so the turn
    # fraction is converted here, at the read, before anything composes
    # with it.
    anisotropy_rotation_value = anisotropic_rotation_turns_to_radians(
        _socket_default_float(principled_node, "Anisotropic Rotation", 0.0)
    )
    anisotropy_factor_painter = _add_uniform_painter(
        state,
        f"{material.name_full}_anisotropy",
        (anisotropy_value, anisotropy_value, anisotropy_value),
    ) if anisotropy_value > 1e-4 else None
    # DL-213: the mesh producer handles every linked Tangent graph. Vector
    # Rotate belongs to that graph (world coordinates), not to this material's
    # rotation slot; adding its angle here would rotate the baked basis twice.
    # Principled's Anisotropic Rotation above remains a separate frame rotation.

    anisotropy_rotation_painter = _add_uniform_painter(
        state,
        f"{material.name_full}_anisotropy_rotation",
        (anisotropy_rotation_value, anisotropy_rotation_value, anisotropy_rotation_value),
    ) if abs(anisotropy_rotation_value) > 1e-4 else None

    # Sheen (DL-18, docs/DEBT_LEDGER.md; source heading
    # CLOTH_FABRIC_DESIGN.md §15 item 13) — Blender 4.x Principled
    # sockets "Sheen Weight" / "Sheen Tint" (colour) / "Sheen Roughness".
    # Mirrors GLTFSceneImporter.cpp's KHR_materials_sheen handling:
    # `sheen_weight * sheen_tint` becomes `fabric_material`'s
    # `sheen_color` in one slot — the max channel there IS the
    # energy-split weight (see fabric_material's own descriptor), the
    # same single-slot contract glTF's `sheenColorFactor` already uses,
    # so folding Blender's separate Weight into the colour reproduces it
    # exactly.  "Sheen Weight" is read at its socket default only (like
    # "Coat Weight"/"Absorption Coefficient" elsewhere in this file) —
    # RISE has no multiply-painter primitive to combine an independently
    # textured weight with an independently textured tint, and Blender
    # artists overwhelmingly paint the TINT, not the weight.
    sheen_weight = _clamp01(_socket_default_float(principled_node, "Sheen Weight", 0.0))
    sheen_weight_socket = _node_input(principled_node, "Sheen Weight")
    if sheen_weight_socket is not None and sheen_weight_socket.is_linked:
        _warn_once(
            state,
            f"RISE reads Principled Sheen Weight on '{material.name_full}' at its socket default; "
            f"a linked/textured weight is not sampled.",
        )

    sheen_color_painter = None
    sheen_roughness_value = 0.5
    sheen_roughness_texture_painter = None
    if sheen_weight > 1e-4:
        sheen_tint = _socket_default_color(principled_node, "Sheen Tint", (1.0, 1.0, 1.0))
        sheen_color_painter, _ = _resolve_color_or_expr(
            "Sheen Tint",
            f"{material.name_full}_sheen_color",
            sheen_tint,
            None,
            scale=(sheen_weight, sheen_weight, sheen_weight),
        )

        sheen_roughness_value = _clamp01(_socket_default_float(principled_node, "Sheen Roughness", 0.5))
        sheen_rough_sock = _node_input(principled_node, "Sheen Roughness")
        if not _is_direct_simple_image_socket(sheen_rough_sock):
            sheen_roughness_texture_painter = _try_expr_painter(
                "Sheen Roughness", f"{material.name_full}_sheen_roughness_expr", "scalar"
            )
        if sheen_roughness_texture_painter is None:
            sheen_roughness_texture = _maybe_resolve_socket_texture(
                principled_node, "Sheen Roughness", None, colorspace_is_data=True,
            )
            if sheen_roughness_texture is not None:
                sheen_roughness_texture_painter = _scalar_or_texture_painter(
                    state,
                    f"{material.name_full}_sheen_roughness",
                    sheen_roughness_value,
                    texture_wrapper=sheen_roughness_texture,
                )

    # Coat (DL-186, docs/DEBT_LEDGER.md; source heading DL-151's own
    # sibling audit) -- Blender's Principled "Coat Weight" / "Coat
    # Tint" (colour) / "Coat Roughness" / "Coat IOR" ->
    # `coated_material`.  Mirrors GLTFSceneImporter.cpp's own
    # KHR_materials_clearcoat handling: coat_weight <->
    # clearcoat_factor, coat_roughness is Blender's PERCEPTUAL
    # roughness [0,1] (the native bridge SQUARES it into a GGX alpha
    # before calling AddCoatedMaterial -- see rise_blender_bridge.cpp's
    # own comment on `coat_roughness`, matching
    # `clearcoat_roughness_factor^2`).  Unlike glTF clearcoat (which
    # has no IOR concept of its own and hardcodes 1.5), Blender's
    # Principled DOES expose a real "Coat IOR" socket, read here.
    coat_weight_value = _clamp01(_socket_default_float(principled_node, "Coat Weight", 0.0))
    coat_weight_socket = _node_input(principled_node, "Coat Weight")
    coat_weight_texture_painter = None
    if coat_weight_socket is not None and coat_weight_socket.is_linked:
        if not _is_direct_simple_image_socket(coat_weight_socket):
            coat_weight_texture_painter = _try_expr_painter(
                "Coat Weight", f"{material.name_full}_coat_weight_expr", "scalar"
            )
        if coat_weight_texture_painter is None:
            coat_weight_texture = _maybe_resolve_socket_texture(
                principled_node, "Coat Weight", None, colorspace_is_data=True,
            )
            if coat_weight_texture is not None:
                coat_weight_texture_painter = _scalar_or_texture_painter(
                    state,
                    f"{material.name_full}_coat_weight",
                    coat_weight_value,
                    texture_wrapper=coat_weight_texture,
                )
        if coat_weight_texture_painter is None:
            _warn_once(
                state,
                f"RISE reads Principled Coat Weight on '{material.name_full}' at its socket default; "
                f"a linked/textured weight not resolvable to an image texture is not sampled.",
            )

    coat_tint_painter = None
    coat_roughness_value = 0.03
    coat_roughness_texture_painter = None
    coat_ior_value = 1.5
    coat_normal_painter = None
    coat_normal_scale = 1.0
    coat_contributes = coat_weight_value > 1e-4 or coat_weight_texture_painter is not None
    if coat_contributes:
        coat_tint_default = _socket_default_color(principled_node, "Coat Tint", (1.0, 1.0, 1.0))
        coat_tint_sock = _node_input(principled_node, "Coat Tint")
        coat_tint_expr = (
            _try_expr_painter("Coat Tint", f"{material.name_full}_coat_tint_expr", "vec3")
            if not _is_direct_simple_image_socket(coat_tint_sock) else None
        )
        if coat_tint_expr is not None:
            coat_tint_painter = coat_tint_expr
        else:
            coat_tint_texture = _maybe_resolve_socket_texture(
                principled_node, "Coat Tint", None, colorspace_is_data=False,
            )
            is_default_white = (
                coat_tint_texture is None
                and abs(coat_tint_default[0] - 1.0) < 1e-4
                and abs(coat_tint_default[1] - 1.0) < 1e-4
                and abs(coat_tint_default[2] - 1.0) < 1e-4
            )
            if not is_default_white:
                coat_tint_painter = _color_or_texture_painter(
                    state,
                    f"{material.name_full}_coat_tint",
                    coat_tint_default,
                    texture_wrapper=coat_tint_texture,
                )

        coat_roughness_value = _clamp01(_socket_default_float(principled_node, "Coat Roughness", 0.03))
        coat_rough_sock = _node_input(principled_node, "Coat Roughness")
        if not _is_direct_simple_image_socket(coat_rough_sock):
            coat_roughness_texture_painter = _try_expr_painter(
                "Coat Roughness", f"{material.name_full}_coat_roughness_expr", "scalar"
            )
        if coat_roughness_texture_painter is None:
            coat_roughness_texture = _maybe_resolve_socket_texture(
                principled_node, "Coat Roughness", None, colorspace_is_data=True,
            )
            if coat_roughness_texture is not None:
                coat_roughness_texture_painter = _scalar_or_texture_painter(
                    state,
                    f"{material.name_full}_coat_roughness",
                    coat_roughness_value,
                    texture_wrapper=coat_roughness_texture,
                )

        coat_ior_socket = _node_input(principled_node, "Coat IOR")
        if coat_ior_socket is not None and coat_ior_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Coat IOR on '{material.name_full}' at its socket default; "
                f"a linked/textured IOR is not sampled.",
            )
        coat_ior_value = max(1.0, float(_socket_default_float(principled_node, "Coat IOR", 1.5)))

        # Coat Normal (DL-192, docs/DEBT_LEDGER.md): a Normal Map node,
        # textured -> `coated_material`'s coat-lobe-only tangent-space
        # normal map.  Registered as a PAINTER (not a modifier -- unlike
        # the surface's own Normal input, this one is a material
        # PARAMETER, `coat_normal_painter_name`, consumed only by the
        # coat's own GGX lobe).  Mirrors `_direct_normal_modifier`'s own
        # "direct Normal Map node, or warn" gate.
        coat_normal_input = _node_input(principled_node, "Coat Normal")
        coat_normal_links = (
            [lk for lk in coat_normal_input.links if not getattr(lk, "is_muted", False)]
            if (coat_normal_input is not None and coat_normal_input.is_linked) else []
        )
        if coat_normal_links:
            coat_normal_source = _skip_reroutes_and_muted(
                coat_normal_links[0].from_node, coat_normal_links[0].from_socket
            )
            if coat_normal_source is not None:
                if coat_normal_source.bl_idname == "ShaderNodeNormalMap":
                    coat_norm_gen: list[bool] = []
                    coat_normal_painter, coat_normal_scale = _build_coat_normal_painter(
                        material,
                        coat_normal_source,
                        state,
                        bbox_min=bbox_min,
                        bbox_max=bbox_max,
                        material_pass_index=material_pass_index,
                        used_generated_out=coat_norm_gen,
                        mesh_attrs_out=used_mesh_attributes,
                        attr_types=attr_types,
                    )
                    if coat_norm_gen:
                        used_generated_coords = True
                else:
                    _warn_once(
                        state,
                        f"RISE only supports a direct Normal Map node on Coat Normal for '{material.name_full}'.",
                    )

        # LAYERING DECISION (docs/BLENDER_MATERIAL_TRANSLATION.md "Coat
        # and Subsurface"), UPDATED 2026-09-27: this USED TO warn and
        # drop the coat when Sheen also contributed, mirroring
        # GLTFSceneImporter.cpp's own (also since-updated)
        # KHR_materials_clearcoat + KHR_materials_sheen handling --
        # `coated_material`'s substrate allowlist did not accept a
        # `fabric_material` (the sheen result).  DL-23
        # (docs/DEBT_LEDGER.md, closed 2026-09-14) lifted that
        # allowlist refusal, and the native bridge
        # (rise_blender_bridge.cpp's `add_pbr_metallic_roughness_material`)
        # now composes a `coated_material` wrapping the sheen's
        # `fabric_material` result instead of skipping it -- so no
        # warning belongs here any more; both fields simply combine.
        # (No `_material_payload` code change was otherwise needed:
        # `coat_*` and `sheen_*` fields are forwarded independently and
        # the native bridge alone decides how to compose them.)

    # Subsurface (DL-186, docs/DEBT_LEDGER.md) -- Blender's Principled
    # "Subsurface Weight" / "Subsurface Radius" (RGB vector, scene
    # units) / "Subsurface Scale" / "Subsurface IOR" / "Subsurface
    # Anisotropy" -> `randomwalk_sss_material`.  A SEPARATE model from
    # PBR_METALLIC_ROUGHNESS (see MATERIAL_RANDOMWALK_SSS's own
    # comment above) -- Blender's own Principled blends diffuse/SSS by
    # weight rather than layering SSS under a specular stack.
    #
    # THE CONVERSION.  RandomWalkSSS wants sigma_a/sigma_s (absorption/
    # scattering coefficients, 1/length), while Blender authors Radius
    # (a mean-free-path-like distance per channel) + Scale + a base
    # colour (the single-scattering albedo).  This uses the classic
    # single-scattering-albedo relation -- NOT PBRT's/Blender's own
    # photon-beam-diffusion inversion (Christensen-Burley), which needs
    # a lookup-table fit this bridge does not carry.  Per channel:
    #   mfp   = max(radius * scale, epsilon)
    #   sigma_t = 1 / mfp
    #   albedo  = clamp(base_color, 0, 0.999)      # avoid the albedo=1 singularity
    #   sigma_s = albedo * sigma_t
    #   sigma_a = (1 - albedo) * sigma_t
    # This is a documented APPROXIMATION (docs/BLENDER_MATERIAL_-
    # TRANSLATION.md "Subsurface") -- it reproduces the right ORDER of
    # magnitude and the right per-channel colour bias (redder radius ->
    # more red scattering) but will not photometrically match Cycles'
    # own diffusion-profile fit.
    subsurface_weight_value = _clamp01(_socket_default_float(principled_node, "Subsurface Weight", 0.0))
    subsurface_weight_socket = _node_input(principled_node, "Subsurface Weight")
    if subsurface_weight_socket is not None and subsurface_weight_socket.is_linked:
        _warn_once(
            state,
            f"RISE reads Principled Subsurface Weight on '{material.name_full}' at its socket default; "
            f"a linked/textured weight is not sampled.",
        )

    subsurface_contributes = subsurface_weight_value > 1e-4
    subsurface_absorption_literal = None
    subsurface_scattering_literal = None
    subsurface_ior_value = 1.4
    subsurface_g_value = 0.0
    if subsurface_contributes:
        radius_socket = _node_input(principled_node, "Subsurface Radius")
        radius_default = (1.0, 0.2, 0.1)
        if radius_socket is not None:
            if radius_socket.is_linked:
                _warn_once(
                    state,
                    f"RISE reads Principled Subsurface Radius on '{material.name_full}' at its socket "
                    f"default; a linked/textured radius is not sampled.",
                )
            radius_default = tuple(float(radius_socket.default_value[index]) for index in range(3))

        subsurface_scale_value = max(
            0.0, float(_socket_default_float(principled_node, "Subsurface Scale", 0.05)))
        subsurface_scale_socket = _node_input(principled_node, "Subsurface Scale")
        if subsurface_scale_socket is not None and subsurface_scale_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Subsurface Scale on '{material.name_full}' at its socket "
                f"default; a linked/textured scale is not sampled.",
            )

        subsurface_ior_socket = _node_input(principled_node, "Subsurface IOR")
        if subsurface_ior_socket is not None and subsurface_ior_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Subsurface IOR on '{material.name_full}' at its socket "
                f"default; a linked/textured IOR is not sampled.",
            )
        # DL-291: exported verbatim as the material's ABSOLUTE index (Blender
        # authors it against air).  RISE prices the SSS boundary at the
        # RELATIVE index against the live exterior (`ambientIOR`), so this is
        # deliberately NOT divided by any enclosing medium here -- the export
        # assumes air by construction, and RISE does the rest at render time.
        subsurface_ior_value = max(1.0, float(_socket_default_float(principled_node, "Subsurface IOR", 1.4)))

        subsurface_anisotropy_socket = _node_input(principled_node, "Subsurface Anisotropy")
        if subsurface_anisotropy_socket is not None and subsurface_anisotropy_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Subsurface Anisotropy on '{material.name_full}' at its socket "
                f"default; a linked/textured anisotropy is not sampled.",
            )
        subsurface_g_value = max(-1.0, min(1.0, float(
            _socket_default_float(principled_node, "Subsurface Anisotropy", 0.0))))

        albedo_source = base_color
        if base_color_texture is not None or base_expr is not None:
            _warn_once(
                state,
                f"RISE approximates Subsurface on '{material.name_full}' using Base Color's socket "
                f"default (not its texture) as the single-scattering albedo; a textured base colour "
                f"with Subsurface Weight > 0 does not vary the SSS colour across the surface.",
            )
            albedo_source = _socket_default_color(principled_node, "Base Color", (0.8, 0.8, 0.8))

        _SSS_EPS = 1.0e-6
        sigma_a_channels = []
        sigma_s_channels = []
        for channel_index in range(3):
            mfp = max(radius_default[channel_index] * subsurface_scale_value, _SSS_EPS)
            sigma_t = 1.0 / mfp
            albedo = max(0.0, min(0.999, float(albedo_source[channel_index])))
            sigma_s_channels.append(albedo * sigma_t)
            sigma_a_channels.append((1.0 - albedo) * sigma_t)
        subsurface_absorption_literal = "{:.9g} {:.9g} {:.9g}".format(*sigma_a_channels)
        subsurface_scattering_literal = "{:.9g} {:.9g} {:.9g}".format(*sigma_s_channels)

    # ----- Thin film (Blender 4.2+) and transmission dispersion (5.3) -----
    # ABI v18.  Both are read at their socket defaults; a linked input is
    # diagnosed rather than silently sampled at one value.  The film's
    # placement depends on the branch chosen below (GGX `thinfilm`
    # Fresnel over the opaque base; a one-layer AR coating on the
    # dielectric); dispersion only exists on the dielectric branch, as
    # in Cycles/OpenPBR where it modulates transmission only.
    for linked_name in ("Thin Film Thickness", "Thin Film IOR",
                        "Transmission Dispersion Scale", "Transmission Dispersion Abbe Number"):
        linked_socket = _node_input(principled_node, linked_name)
        if linked_socket is not None and linked_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled {linked_name} on '{material.name_full}' at its socket default; "
                f"a linked/textured value is not sampled.",
            )
    thin_film_thickness_value = max(0.0, _socket_default_float(principled_node, "Thin Film Thickness", 0.0))
    if thin_film_thickness_value <= THIN_FILM_THICKNESS_CUTOFF_NM:
        thin_film_thickness_value = 0.0     # Cycles ignores films this thin
    thin_film_ior_value = max(1.0e-3, _socket_default_float(principled_node, "Thin Film IOR", 1.33))
    dispersion_abbe_value = effective_abbe_number(
        _socket_default_float(principled_node, "Transmission Dispersion Abbe Number", 20.0),
        _socket_default_float(principled_node, "Transmission Dispersion Scale", 0.0),
    )

    transmission_texture = _maybe_resolve_socket_texture(
        principled_node, "Transmission Weight",
        wrapper.transmission_texture,
        colorspace_is_data=True,
    )
    transmission = _clamp01(wrapper.transmission)
    if transmission_texture is not None and transmission < 0.05:
        _warn_once(
            state,
            f"RISE treats '{material.name_full}' as a transmissive material because its Transmission input is texture-driven.",
        )
    transmission_heavy = allow_dielectric and (transmission_texture is not None or transmission >= 0.05)

    emission_painter = None
    emission_color = _float_color3(wrapper.emission_color)
    emission_strength = max(0.0, float(wrapper.emission_strength))
    ec_sock = _node_input(principled_node, "Emission Color")
    es_sock = _node_input(principled_node, "Emission Strength")
    ec_direct_img = _is_direct_simple_image_socket(ec_sock)
    es_direct_img = _is_direct_simple_image_socket(es_sock)
    emission_color_texture = (
        _maybe_resolve_socket_texture(
            principled_node, "Emission Color",
            wrapper.emission_color_texture,
            colorspace_is_data=False,
        )
        if ec_direct_img else None
    )
    emission_strength_texture = (
        _maybe_resolve_socket_texture(
            principled_node, "Emission Strength",
            wrapper.emission_strength_texture,
            colorspace_is_data=True,
        )
        if es_direct_img else None
    )

    ec_has_links = bool(ec_sock is not None and ec_sock.is_linked and any(not getattr(lk, "is_muted", False) for lk in ec_sock.links))
    es_has_links = bool(es_sock is not None and es_sock.is_linked and any(not getattr(lk, "is_muted", False) for lk in es_sock.links))
    emission_paired_expr = None
    if ec_has_links and es_has_links:
        paired_img_resolver = _DeferredExprImageResolver(state, f"{label_prefix}_emission_expr")
        paired = procedural_expr.compile_socket_product_expression(
            ec_sock,
            es_sock,
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=paired_img_resolver,
        )
        if paired is not None:
            paired_img_resolver.commit()
            translated_sockets.add("Emission Color")
            translated_sockets.add("Emission Strength")
            if paired.uses_generated:
                used_generated_coords = True
            if paired.mesh_attributes:
                used_mesh_attributes.update(paired.mesh_attributes)
            emission_paired_expr = _add_expression_painter(
                state,
                f"{label_prefix}_emission_expr",
                paired.expression,
            )
            emission_color_texture = None
            emission_strength_texture = None

    if emission_paired_expr is None and emission_color_texture is not None and emission_strength_texture is not None:
        _warn_once(
            state,
            f"RISE only supports one textured emission input on '{material.name_full}'. The emission strength texture is ignored.",
        )
        emission_strength_texture = None

    emission_color_expr = (
        _try_expr_painter(
            "Emission Color",
            f"{material.name_full}_emission_expr",
            "vec3",
            scale=(emission_strength, emission_strength, emission_strength),
        )
        if (emission_paired_expr is None and emission_color_texture is None and emission_strength > 1e-6) else None
    )
    if emission_color_expr is not None and emission_strength_texture is not None:
        _warn_once(
            state,
            f"RISE only supports one textured emission input on '{material.name_full}'. The emission strength texture is ignored.",
        )
        emission_strength_texture = None
    if (
        emission_paired_expr is None
        and emission_color_texture is None
        and emission_strength_texture is None
        and emission_strength <= 1e-6
    ):
        # When Emission Strength is unlinked and 0, emission is disabled;
        # suppress a spurious fallback warning if Emission Color itself is a
        # supported procedural/image-expression graph (read-only probe: never
        # unpacks packed images or emits warnings).
        def _probe_expr_img(img_node) -> str | None:
            if getattr(img_node, "bl_idname", "") != "ShaderNodeTexImage":
                return None
            if getattr(img_node, "projection", "FLAT") != "FLAT":
                return None
            if getattr(img_node, "extension", "REPEAT") not in procedural_expr.SUPPORTED_TEX_IMAGE_EXTENSIONS:
                return None
            img = getattr(img_node, "image", None)
            if img is None or getattr(img, "source", "FILE") not in ("FILE", "GENERATED"):
                return None
            return "__probe_tex"

        if procedural_expr.compile_socket_expression(
            ec_sock,
            want_type="vec3",
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=_probe_expr_img,
        ) is not None:
            translated_sockets.add("Emission Color")
    emission_strength_expr = (
        _try_expr_painter(
            "Emission Strength",
            f"{material.name_full}_emission_str_expr",
            "scalar",
            scale=emission_color,
        )
        if (
            emission_paired_expr is None
            and emission_color_texture is None
            and emission_color_expr is None
            and emission_strength_texture is None
        ) else None
    )
    # Fallback to legacy image walker if expression compilation failed on a linked non-direct socket.
    if (
        emission_paired_expr is None
        and emission_color_texture is None
        and emission_color_expr is None
        and not ec_direct_img
        and emission_strength > 1e-6
    ):
        emission_color_texture = _maybe_resolve_socket_texture(
            principled_node, "Emission Color",
            wrapper.emission_color_texture,
            colorspace_is_data=False,
        )
    if emission_paired_expr is None and emission_strength_texture is None and emission_strength_expr is None and not es_direct_img:
        emission_strength_texture = _maybe_resolve_socket_texture(
            principled_node, "Emission Strength",
            wrapper.emission_strength_texture,
            colorspace_is_data=True,
        )
        if emission_color_texture is not None and emission_strength_texture is not None:
            _warn_once(
                state,
                f"RISE only supports one textured emission input on '{material.name_full}'. The emission strength texture is ignored.",
            )
            emission_strength_texture = None

    if (
        emission_strength > 1e-6
        or emission_strength_texture is not None
        or emission_paired_expr is not None
        or emission_color_expr is not None
        or emission_strength_expr is not None
    ):
        if emission_color_texture is not None and emission_strength > 1e-6:
            emission_painter = _color_or_texture_painter(
                state,
                f"{material.name_full}_emission",
                emission_color,
                texture_wrapper=emission_color_texture,
                scale=(emission_strength, emission_strength, emission_strength),
            )
        elif emission_paired_expr is not None:
            emission_painter = emission_paired_expr
        elif emission_color_expr is not None:
            emission_painter = emission_color_expr
        elif emission_strength_texture is not None:
            emission_painter = _scalar_or_texture_painter(
                state,
                f"{material.name_full}_emission",
                emission_strength,
                texture_wrapper=emission_strength_texture,
                scale=max(emission_color[0], emission_color[1], emission_color[2]),
            )
        elif emission_strength_expr is not None:
            emission_painter = emission_strength_expr
        elif max(emission_color) > 1e-6:
            emission_painter = _add_uniform_painter(
                state,
                f"{material.name_full}_emission",
                tuple(emission_color[index] * emission_strength for index in range(3)),
            )

    normal_used_generated: list[bool] = []
    normal_handled: list[bool] = []
    modifier_name = _direct_normal_modifier(
        material,
        wrapper,
        state,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        material_pass_index=material_pass_index,
        used_generated_out=normal_used_generated,
        handled_out=normal_handled,
        mesh_attrs_out=used_mesh_attributes,
        attr_types=attr_types,
    )
    if normal_used_generated:
        used_generated_coords = True
    if normal_handled:
        translated_sockets.add("Normal")

    # Alpha (ABI v14, DL-214): the bridge binds material intersection
    # coverage. OPAQUE ignores the socket, CLIP applies alpha_threshold,
    # and BLEND/HASHED use stochastic coverage across transport paths.
    # A linked Alpha texture replaces the unlinked numeric socket value.
    #
    # Blender 4.2's EEVEE-Next replaced `blend_method` with
    # `surface_render_method` ('BLENDED'/'DITHERED') for the raster
    # pipeline; `getattr` falls back to that when `blend_method` itself
    # is absent, matching this file's existing defensive-getattr
    # convention (`use_backface_culling` above).  Neither is guaranteed
    # to exist depending on Blender version/render engine, so a
    # material with NEITHER attribute is treated as OPAQUE (bit-
    # identical to a pre-v14 payload) rather than guessing.
    blend_method = getattr(material, "blend_method", None)
    if blend_method is None:
        render_method = getattr(material, "surface_render_method", None)
        blend_method = {
            "BLENDED": "BLEND",
            "DITHERED": "HASHED",
        }.get(render_method, "OPAQUE")

    alpha_value = 1.0
    alpha_texture_painter = None
    alpha_mode_value = ALPHA_MODE_OPAQUE
    alpha_threshold_value = 0.5

    if blend_method != "OPAQUE":
        alpha_socket = _node_input(principled_node, "Alpha")
        if alpha_socket is not None:
            alpha_value = _clamp01(float(alpha_socket.default_value))
            alpha_has_links = bool(
                alpha_socket.is_linked
                and any(not getattr(lk, "is_muted", False) for lk in alpha_socket.links)
            )
            if alpha_has_links:
                if not _is_direct_simple_image_socket(alpha_socket, want_output="Alpha"):
                    alpha_texture_painter = _try_expr_painter(
                        "Alpha", f"{material.name_full}_alpha_expr", "scalar"
                    )
                alpha_texture = (
                    _maybe_resolve_socket_texture(
                        principled_node, "Alpha", None, colorspace_is_data=True,
                    )
                    if alpha_texture_painter is None else None
                )
                if alpha_texture is not None:
                    # Read CHAN_A from the raw texture (matching
                    # GLTFSceneImporter.cpp's own `BuildAlphaPainter`
                    # comment): the native bridge extracts the ALPHA
                    # channel via AddChannelPainter, so this registers
                    # the whole (typically RGBA) image, not a
                    # scalar-flattened view of it.
                    validated = _validate_texture_wrapper(
                        alpha_texture, state, f"the Alpha texture on '{material.name_full}'")
                    if validated:
                        filepath, color_space = validated
                        kind = SUPPORTED_IMAGE_KINDS[os.path.splitext(filepath)[1].lower()]
                        alpha_texture_painter = _add_texture_painter(
                            state, f"{material.name_full}_alpha", filepath, kind, color_space,
                            wrap_mode=_tex_image_wrap_mode(getattr(alpha_texture, "node_image", None)),
                        )
                        alpha_texture_painter = _maybe_wrap_with_uv_transform(
                            state, f"{material.name_full}_alpha", alpha_texture_painter, alpha_texture)
                if alpha_texture_painter is None:
                    _warn_once(
                        state,
                        f"RISE reads Principled Alpha on '{material.name_full}' at its socket default; "
                        f"a linked/textured alpha not resolvable to an image texture is not sampled.",
                    )

        if blend_method == "CLIP":
            alpha_mode_value = ALPHA_MODE_CLIP
            alpha_threshold_value = float(getattr(material, "alpha_threshold", 0.5))
        else:
            # BLEND or HASHED -> RISE's single stochastic-transparency path.
            alpha_mode_value = ALPHA_MODE_BLEND

        # DL-214: the native bridge installs scalar material coverage for
        # every integrator. No incompatible-rasterizer warning is needed.

    _warn_unsupported_principled_features(state, material, wrapper, translated_sockets)


    output_node = _find_material_output(material) if allow_volume else None
    interior_medium_name = None
    if allow_volume and output_node is not None:
        volume_spec = _parse_volume_socket(_node_input(output_node, "Volume"), state, f"the material '{material.name_full}'")
        if volume_spec is not None:
            interior_medium_name = _export_homogeneous_medium(state, f"{material.name_full}_volume", volume_spec)

    double_sided = not getattr(material, "use_backface_culling", False)

    # Subsurface is mutually exclusive with Transmission (matching this
    # function's own pre-existing PBR-vs-DIELECTRIC exclusivity for
    # transmission_heavy above) -- glass and skin are not the same
    # Principled configuration, and RISE has no combined model.
    # Transmission wins the conflict (it already owned the "replace the
    # whole surface" precedent before this debt existed).
    subsurface_heavy = subsurface_contributes and not transmission_heavy
    if subsurface_contributes and transmission_heavy:
        _warn_once(
            state,
            f"RISE: '{material.name_full}' declares both Subsurface Weight and a heavy Transmission; "
            f"the dielectric transmission model wins and Subsurface is skipped (no combined model).",
        )

    if transmission_heavy:
        metallic_for_warn = _maybe_resolve_socket_texture(
            principled_node, "Metallic",
            wrapper.metallic_texture,
            colorspace_is_data=True,
        )
        if metallic > 1e-3 or metallic_for_warn is not None:
            _warn_once(state, f"RISE ignores metallic on dielectric-transmission Principled material '{material.name_full}'.")

        # Dielectric `tau`, `ior`, `scattering` slots are physical
        # scalars (IScalarPainter) since the ISCALARPAINTER refactor
        # — see docs/ISCALARPAINTER_REFACTOR.md.  Job.cpp's
        # `ResolveScalarPainterArg` parses inline numeric strings
        # ("v" → UniformScalarPainter, "r g b" → RGBScalarPainter)
        # so we pass numerics-as-strings here rather than registering
        # IPainter chunks (which the library will reject with
        # "this slot now requires a scalar_painter").
        #
        # Texture-driven scalars (e.g. IOR map, scattering map,
        # textured transmission tint) are NOT supported on this path
        # yet — they would need a dedicated AddUniformScalarPainter /
        # AddTextureScalarPainter API in the bridge.  We warn and
        # fall back to the uniform value for now.
        transmission_value = transmission
        if transmission_texture is not None:
            _warn_once(state, f"RISE currently ignores textured Transmission on '{material.name_full}'; using the uniform value.")
        # ABI v21: pass a registered IPainter name (procedural expression or
        # image texture) straight through as `tau_painter_name`; the native
        # bridge wraps it in PainterRGBScalarPainter so glass/water tint
        # varies spatially across the surface (e.g. Geometry Nodes `distance`
        # attribute -> ColorRamp -> Base Color on `water`).
        if base_expr is not None:
            if abs(transmission_value - 1.0) <= 1e-6:
                tau_spec = base_expr
            else:
                scaled_tau = _try_expr_painter(
                    "Base Color",
                    f"{label_prefix}_tau_expr",
                    "vec3",
                    scale=(transmission_value, transmission_value, transmission_value),
                )
                tau_spec = scaled_tau if scaled_tau is not None else base_expr
        elif base_color_texture is not None:
            tau_spec = _color_or_texture_painter(
                state,
                f"{label_prefix}_tau",
                base_color,
                texture_wrapper=base_color_texture,
                scale=(transmission_value, transmission_value, transmission_value),
            )
        else:
            tau_r = base_color[0] * transmission_value
            tau_g = base_color[1] * transmission_value
            tau_b = base_color[2] * transmission_value
            tau_spec = f"{tau_r:.6f} {tau_g:.6f} {tau_b:.6f}"

        ior_texture_check = _maybe_resolve_socket_texture(
            principled_node, "IOR",
            wrapper.ior_texture,
            colorspace_is_data=True,
        )
        if ior_texture_check is not None:
            _warn_once(state, f"RISE currently ignores textured IOR on '{material.name_full}'; using the uniform value.")
        ior_value = max(1.0, float(wrapper.ior))
        ior_inline = f"{ior_value:.6f}"
        if dispersion_abbe_value > 0.0 and not dispersion_keeps_positive_ior(ior_value, dispersion_abbe_value):
            _warn_once(
                state,
                f"RISE drops the transmission dispersion on '{material.name_full}': an effective Abbe "
                f"number of {dispersion_abbe_value:.3g} makes the IOR curve non-positive in 300-900 nm.",
            )
            dispersion_abbe_value = 0.0

        # Convert Blender's Principled BSDF Roughness (0 = sharp, 1 =
        # diffuse) to RISE's dielectric `scattering` parameter, which
        # is the Phong-cone exponent for the refraction lobe:
        #   alpha = acos( random^(1/(scat+1)) )
        # Sharp glass needs `scat → ∞` (acos(1) = 0 → no deviation).
        # Diffuse / milky glass needs `scat → 0` (acos(rand) → uniform
        # [0, π/2]).  Production RISE scenes (pt_jewel_vault) use
        # 100000 for sharp glass, matching that convention here.
        #
        # The prior naive mapping `scat = max(roughness, 1e-4)` made
        # water (roughness = 0) export as scat = 0.0001 — uniform
        # random refraction, which scrambled every camera ray and
        # produced a dark "ripples" surface where Cycles cleanly
        # showed the pebbles below.
        if roughness < 1.0e-4:
            scatter_value = 1.0e6   # essentially delta (sharp glass)
        else:
            # GGX-to-Phong-ish conversion: n = 1/α² - 1 with α = roughness.
            # Caps at 1.0e6 to match the production-scene convention
            # and avoid overflow / under-precision in the acos branch.
            scatter_value = max(1.0, min(1.0e6, 1.0 / (roughness * roughness) - 1.0))
        scatter_inline = f"{scatter_value:.6f}"

        payload = MaterialData(
            name=_unique_name(state, "mat", label_prefix),
            model=MATERIAL_DIELECTRIC,
            tau_painter_name=tau_spec,
            ior_painter_name=ior_inline,
            scatter_painter_name=scatter_inline,
            emission_painter_name=emission_painter,
            double_sided=double_sided,
            # ABI v18: the film is a one-layer AR coating on the
            # dielectric interface; dispersion turns `ior` into a
            # Cauchy curve pinned at n(587.6 nm) = IOR.
            thin_film_thickness=thin_film_thickness_value,
            thin_film_ior=thin_film_ior_value,
            dispersion_abbe=dispersion_abbe_value,
            alpha=alpha_value,
            alpha_texture_painter_name=alpha_texture_painter,
            alpha_mode=alpha_mode_value,
            alpha_threshold=alpha_threshold_value,
        )
    elif subsurface_heavy:
        # Subsurface -> randomwalk_sss_material, a SEPARATE model (see
        # MATERIAL_RANDOMWALK_SSS's own comment above) -- replaces the
        # opaque PBR branch entirely rather than layering under it.
        # Coat/Sheen do not apply here (neither has a PBR base to wrap
        # when this branch fires); warn if either was also authored.
        if coat_contributes:
            _warn_once(
                state,
                f"RISE: '{material.name_full}' declares both Subsurface Weight and Coat Weight; "
                f"randomwalk_sss_material has no coat layer, so Coat is skipped.",
            )
        if sheen_weight > 1e-4:
            _warn_once(
                state,
                f"RISE: '{material.name_full}' declares both Subsurface Weight and Sheen; "
                f"randomwalk_sss_material has no sheen layer, so Sheen is skipped.",
            )
        if thin_film_thickness_value > 0.0:
            _warn_once(
                state,
                f"RISE: '{material.name_full}' declares a Thin Film on a Subsurface material; "
                f"randomwalk_sss_material has no thin-film interface, so the film is skipped.",
            )
        payload = MaterialData(
            name=_unique_name(state, "mat", label_prefix),
            model=MATERIAL_RANDOMWALK_SSS,
            emissive_scale=1.0,
            emission_painter_name=emission_painter,
            double_sided=double_sided,
            subsurface_absorption=subsurface_absorption_literal,
            subsurface_scattering=subsurface_scattering_literal,
            subsurface_ior=subsurface_ior_value,
            subsurface_g=subsurface_g_value,
            subsurface_roughness=roughness,
            alpha=alpha_value,
            alpha_texture_painter_name=alpha_texture_painter,
            alpha_mode=alpha_mode_value,
            alpha_threshold=alpha_threshold_value,
        )
    else:
        film = None
        if thin_film_thickness_value > 0.0:
            film = _thin_film_substrate(
                state, material, principled_node, base_color, metallic,
                specular, anisotropy_value,
            )
            # The film base reads `specular_color_painter_name` as the tint
            # on the film reflectance (unused by the conductor base).
            if film.film_tint is not None:
                specular_color_painter = (
                    None if all(abs(v - 1.0) < 1e-6 for v in film.film_tint)
                    else _add_uniform_painter(state, f"{material.name_full}_film_tint", film.film_tint)
                )

        # Opaque Principled BSDF — route through
        # RISE_API_CreatePBRMetallicRoughnessMaterial, which handles
        # the metallic/dielectric blend, F0=0.04 dielectric Fresnel,
        # roughness² alpha, anisotropy, and emission natively (so we
        # don't need the Luminaire wrapper).
        payload = MaterialData(
            name=_unique_name(state, "mat", label_prefix),
            model=MATERIAL_PBR_METALLIC_ROUGHNESS,
            base_color_painter_name=base_painter,
            metallic_painter_name=metallic_painter,
            roughness_painter_name=roughness_painter,
            specular_factor_painter_name=specular_factor_painter,
            specular_color_painter_name=specular_color_painter,
            anisotropy_factor_painter_name=anisotropy_factor_painter,
            anisotropy_rotation_painter_name=anisotropy_rotation_painter,
            emissive_scale=1.0,
            emission_painter_name=emission_painter,
            double_sided=double_sided,
            sheen_color_painter_name=sheen_color_painter,
            sheen_roughness=sheen_roughness_value,
            sheen_roughness_texture_painter_name=sheen_roughness_texture_painter,
            coat_weight_texture_painter_name=coat_weight_texture_painter,
            coat_weight=coat_weight_value,
            coat_tint_painter_name=coat_tint_painter,
            coat_roughness_texture_painter_name=coat_roughness_texture_painter,
            coat_roughness=coat_roughness_value,
            coat_ior=coat_ior_value,
            coat_normal_painter_name=coat_normal_painter,
            coat_normal_scale=coat_normal_scale,
            thin_film_thickness=thin_film_thickness_value if film is not None else 0.0,
            thin_film_ior=thin_film_ior_value,
            thin_film_conductor=film.conductor if film is not None else False,
            thin_film_dielectric_ior=film.dielectric_ior if film is not None else 1.5,
            thin_film_metal_f0=film.metal_f0 if film is not None else (1.0, 1.0, 1.0),
            thin_film_metal_tint=film.metal_tint if film is not None else (1.0, 1.0, 1.0),
            alpha=alpha_value,
            alpha_texture_painter_name=alpha_texture_painter,
            alpha_mode=alpha_mode_value,
            alpha_threshold=alpha_threshold_value,
        )

    state.materials.append(payload)
    # Preserve ABI v14's deterministic shader binding name. The bridge
    # installs ordinary emission/direct ops here; alpha itself belongs to
    # material intersection coverage and is not applied by this chain.
    shader_name = f"{payload.name}.shader" if alpha_mode_value != ALPHA_MODE_OPAQUE else None
    binding = _MaterialBinding(
        surface_material_name=payload.name,
        modifier_name=modifier_name,
        interior_medium_name=interior_medium_name,
        double_sided=double_sided,
        shader_name=shader_name,
        mesh_attributes=tuple(sorted(used_mesh_attributes)),
    )
    return (binding, used_generated_coords)


# ---------------------------------------------------------------------------
# Hair / fur export (Blender Curves objects -> Cem Yuksel .hair files +
# hair_material translation).  See docs/BLENDER_MATERIAL_TRANSLATION.md's
# hair section for the full design writeup; this block is deliberately
# self-contained (its own painter-resolution + classification logic)
# rather than woven into `_material_payload` above, because a hair
# material's inputs (Melanin / Absorption Coefficient / Offset / ...)
# have nothing in common with a Principled BSDF's, and hair has NO bake
# fallback at all (baking is meaningless for curve geometry) -- so the
# "force complex -> bake" escape hatch the mesh path relies on doesn't
# exist here; an unsupported hair graph is refused outright, with a
# warning, in favour of a physically-reasonable default groom colour.
# ---------------------------------------------------------------------------

_HAIR_BSDF_BLIDNAME = "ShaderNodeBsdfHairPrincipled"

# ShaderNodeBsdfHairPrincipled.parametrization enum identifiers.
# These are Blender's RNA enum IDENTIFIERS, not the UI labels shown in
# the dropdown ("Absorption Coefficient", "Melanin Concentration",
# "Direct Coloring") -- the identifiers are the short forms "COLOR" /
# "MELANIN" / "ABSORPTION".
_HAIR_PARAM_COLOR = "COLOR"
_HAIR_PARAM_MELANIN = "MELANIN"
_HAIR_PARAM_ABSORPTION = "ABSORPTION"

# Inputs that encode PER-STRAND random variation.  RISE's hair_material
# is one BCSDF instance for the whole groom (no per-strand attribute
# plumbing), so these have no representation at all -- warn once and
# ignore, rather than silently dropping the variation with no trace.
_HAIR_UNSUPPORTED_RANDOM_SOCKETS = ("Random Color", "Random Roughness", "Random")

_HAIR_EXPORT_DIR_NAME = "rise_blender_hair"


def _hair_staging_dir() -> str | None:
    """Directory `.hair` files are written into -- same convention as
    `_unpack_image_to_disk`'s `_PACKED_UNPACK_DIR_NAME` (a named
    subdirectory of Blender's own temp dir, created on demand).

    Mirrors `_unpack_image_to_disk`'s contract: returns `None` on an
    `OSError` (read-only temp dir, permissions, out of space, ...)
    rather than letting the exception propagate, so a filesystem
    hiccup skips the affected groom instead of aborting the whole
    export.  Callers must check for `None`.
    """
    out_dir = os.path.join(bpy.app.tempdir or tempfile.gettempdir(), _HAIR_EXPORT_DIR_NAME)
    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError:
        return None
    return out_dir


def _warn_legacy_particle_hair(original_object, state: _ExportState):
    """Legacy (pre-Curves) particle-hair systems -- `ParticleSettings.
    type == 'HAIR'` -- are NOT exported.  Getting their rendered strand
    geometry requires either the deprecated per-particle `co_hair()`
    API (removed from newer Blender versions' render-time access) or
    driving `bpy.ops.object.modifier_convert` / the "Convert Hair to
    New System" operator to materialise a real Curves object first --
    an `bpy.ops` call, which `material_bake.py`'s own docstring
    documents as hazardous to invoke from inside a render callback
    (context-override requirements, hidden-object refusals, teardown
    crashes -- see that module's "Workflow" section).  Converting mid-
    export would import that same risk into every render.  The
    supported path is therefore: the ARTIST converts once, up front,
    via Blender's own UI (Particle properties > Convert, or Object >
    Convert > Curves) -- after which the result is a normal Curves
    object and exports through `_export_hair_object` like any other
    groom.  This function only makes sure the artist isn't left
    guessing why their particle hair didn't render: one warning per
    object, via the same `_warn_once` idiom every other unsupported-
    feature notice in this module uses."""

    for psys in getattr(original_object, "particle_systems", []) or []:
        settings = getattr(psys, "settings", None)
        if settings is not None and getattr(settings, "type", None) == "HAIR":
            _warn_once(
                state,
                f"RISE does not export legacy particle-hair systems ('{psys.name}' on "
                f"'{original_object.name_full}'). Convert it to a Curves object first "
                "(Particle properties → Convert, or Object → Convert → Curves) "
                "and RISE will export the resulting groom like any other Curves object.",
            )


def _extract_curves_arrays(curves_data):
    """BPY-GLUE, MANUALLY VALIDATED ONLY -- no bpy in this repo's test
    environment, so this function (and it alone, of the hair-export
    pipeline) has no unit-test coverage; see
    docs/BLENDER_MATERIAL_TRANSLATION.md's hair section for the
    honesty note on this.  Pulls raw per-point arrays out of a
    ``bpy.types.Curves`` datablock via ``foreach_get``, matching the
    mesh export's bulk-read convention (`_mesh_buckets` above), then
    hands them to the bpy-free `hair_file_writer.
    build_hair_strands_from_curve_arrays` for the actual translation
    logic (which IS unit-tested).

    Returns ``(positions_flat, curve_offsets, radii_flat_or_None)`` --
    see that function's docstring for the exact shapes.
    """
    num_points = len(curves_data.points)
    num_curves = len(curves_data.curves)
    if num_points == 0 or num_curves == 0:
        return [], [0], None

    positions = array.array("f", [0.0]) * (num_points * 3)
    curves_data.points.foreach_get("position", positions)

    first_point = array.array("i", [0]) * num_curves
    curves_data.curves.foreach_get("first_point_index", first_point)
    offsets = list(first_point) + [num_points]

    # `radius` is a first-class per-point attribute on every Curves
    # datablock (used by Blender's own hair rendering for fibre
    # thickness) -- present iff the artist (or a "Set Curve Radius"
    # geometry-nodes setup) ever touched it.  Absent radius data is
    # normal (e.g. curves authored purely by a hand-drawn Curves
    # sketch) -- fall back to None -> no thickness array in the .hair
    # file -> RISE's own human-hair default at import.
    radii = None
    radius_attr = curves_data.attributes.get("radius") if hasattr(curves_data, "attributes") else None
    if radius_attr is not None:
        raw = array.array("f", [0.0]) * num_points
        curves_data.points.foreach_get("radius", raw)
        radii = raw.tolist()

    return positions.tolist(), offsets, radii


def _export_hair_object(object_instance, original_object, state: _ExportState):
    """Export one Blender Curves object as a RISE hair groom: write its
    strands to a Cem Yuksel `.hair` file in the add-on's staging
    directory, resolve its material into a HairMaterialData, and
    record the binding in `state.hair_objects`.  Warns (via
    `_warn_once`) and returns without adding anything on any failure
    -- a groom that can't be exported is a skipped object, not an
    aborted render."""

    eval_object = object_instance.object
    curves_data = getattr(eval_object, "data", None) if eval_object is not None else None
    if curves_data is None:
        return

    try:
        positions, offsets, radii = _extract_curves_arrays(curves_data)
    except Exception as exc:  # pragma: no cover -- bpy glue, defensive
        _warn_once(state, f"RISE could not read hair curves data from '{original_object.name_full}': {exc}.")
        return

    strands = hair_file_writer.build_hair_strands_from_curve_arrays(positions, offsets, radii)
    if not strands:
        _warn_once(
            state,
            f"RISE skipped '{original_object.name_full}': its hair curves have no strand with >= 2 points.",
        )
        return

    out_dir = _hair_staging_dir()
    if out_dir is None:
        _warn_once(
            state,
            "RISE could not create its hair staging directory; skipping hair object "
            f"'{original_object.name_full}'.",
        )
        return
    safe_stem = _safe_name(original_object.name_full)
    file_path = os.path.join(out_dir, f"{safe_stem}_{rna_key(original_object):x}.hair")
    try:
        hair_file_writer.write_hair_file(
            file_path,
            strands,
            info=f"RISE Blender export: {original_object.name_full}"[:88],
        )
    except (OSError, hair_file_writer.HairFileError) as exc:
        _warn_once(state, f"RISE could not write the .hair file for '{original_object.name_full}': {exc}.")
        return

    material = eval_object.active_material
    hair_material_name = _hair_material_payload(material, state)

    state.hair_objects.append(
        HairObjectData(
            name=_unique_name(state, "hairobj", original_object.name_full),
            file_path=file_path,
            material_name=hair_material_name,
            transform=_flatten_matrix(object_instance.matrix_world.copy()),
            source_object_name=original_object.name_full,
        )
    )


def _find_hair_bsdf_through_surface(output_node):
    """Sibling of `material_bake._find_principled_through_surface`,
    looking for `ShaderNodeBsdfHairPrincipled` instead of the regular
    Principled BSDF.  Traverses `NodeReroute` only -- any other node
    between the hair BSDF and Material Output.Surface (a Mix Shader
    blending hair with something else, an Add Shader, a second BSDF)
    is unsupported: hair has no bake fallback to fall back to, so the
    caller refuses the whole material rather than guessing which
    shader "is" the hair."""

    if output_node is None:
        return None
    surface_socket = output_node.inputs.get("Surface")
    if surface_socket is None or not surface_socket.is_linked:
        return None

    visited: set[int] = set()
    cursor = surface_socket.links[0].from_node
    depth = 0
    while depth < 16:
        depth += 1
        node_id = rna_key(cursor)
        if node_id in visited:
            return None
        visited.add(node_id)
        if cursor.bl_idname == _HAIR_BSDF_BLIDNAME:
            return cursor
        if cursor.bl_idname == "NodeReroute":
            inp = cursor.inputs[0] if cursor.inputs else None
            if inp is None or not inp.is_linked:
                return None
            cursor = inp.links[0].from_node
            continue
        return None
    return None


def _hair_graph_supported(node, material_name: str, state: _ExportState) -> bool:
    """Walk every node feeding the hair BSDF's inputs; refuse (return
    False, with one warning naming the offending node) if any of them
    falls outside `material_bake.SUPPORTED_UPSTREAM_NODES` -- the SAME
    set the regular mesh-material classifier uses to decide "simple".
    There is no hair-specific node support beyond what that set
    already covers (a Value node, an Image Texture, a Color Ramp, ...
    all translate the same way regardless of which slot they land in);
    the only difference from the mesh path is that failing here has no
    bake escape hatch, so it is a hard refusal instead of a fallback
    to the explicit manual channel approximation or existing fallback.
    That mesh approximation does not reconstruct arbitrary BSDF lobes.

    Socket-scoped exemption: the three `_HAIR_UNSUPPORTED_RANDOM_
    SOCKETS` inputs are never read into the translated payload at all
    -- `_hair_material_payload` only checks whether they're linked (to
    emit its own "ignored" warning below), it never walks their
    upstream graph.  Whatever feeds them is therefore irrelevant to
    correctness, so this walk doesn't enter those sockets in the first
    place.  This is what makes the standard Cycles wiring (Hair Info ->
    "Random Color" / "Random Roughness") -- `ShaderNodeHairInfo` isn't
    in `SUPPORTED_UPSTREAM_NODES` -- degrade to the doc-promised warn-
    and-ignore instead of refusing the whole material: without this
    exemption the walk would reach `ShaderNodeHairInfo` from a socket
    whose value is discarded anyway and refuse a material that
    translates just fine.  A node that ALSO feeds a real (non-ignored)
    socket is still fully validated via that other socket's entry in
    the loop below -- this only exempts subgraphs that terminate
    exclusively inside the three ignored sockets.
    """

    from . import material_bake as _material_bake

    supported = _material_bake.SUPPORTED_UPSTREAM_NODES

    stack = []
    for socket in node.inputs:
        if socket.name in _HAIR_UNSUPPORTED_RANDOM_SOCKETS:
            continue
        if socket.is_linked:
            stack.append((socket.links[0].from_node, 0))

    visited: set[int] = set()
    while stack:
        current, depth = stack.pop()
        if depth > 16:
            _warn_once(
                state,
                f"RISE refuses hair material '{material_name}': an input chain exceeds 16 nodes. "
                "Using the default hair material.",
            )
            return False
        node_id = rna_key(current)
        if node_id in visited:
            continue
        visited.add(node_id)

        bl = current.bl_idname
        if bl not in supported:
            _warn_once(
                state,
                f"RISE refuses hair material '{material_name}': unsupported node '{bl}' feeds the "
                "Principled Hair BSDF (hair has no bake fallback). Using the default hair material.",
            )
            return False

        for socket in current.inputs:
            if socket.is_linked:
                stack.append((socket.links[0].from_node, depth + 1))

    return True


def _default_hair_material(state: _ExportState) -> str:
    """A plausible brown-black groom (melanin tier, eumelanin ~= 1.3
    per the `hair_material` chunk's own description of that value) --
    used whenever a hair-curves object's material can't be translated:
    no material assigned, no node graph, no Principled Hair BSDF
    reaching the surface, or an unsupported upstream graph."""

    cache = state.hair_material_map
    sentinel = "__default_hair__"
    cached = cache.get(sentinel)
    if cached is not None:
        return cached

    eumelanin = _add_uniform_painter(state, "hair_default_eumelanin", (1.3, 1.3, 1.3))
    pheomelanin = _add_uniform_painter(state, "hair_default_pheomelanin", (0.0, 0.0, 0.0))
    alpha = _add_uniform_painter(state, "hair_default_alpha", (2.0, 2.0, 2.0))

    # No `*_texture_painter_name` here: this material is RISE's own
    # default, so beta_m / beta_n / ior are constants by construction and
    # travel as the numbers below.
    payload = HairMaterialData(
        name=_unique_name(state, "hairmat", "default"),
        tier="melanin",
        eumelanin_painter_name=eumelanin,
        pheomelanin_painter_name=pheomelanin,
        alpha_painter_name=alpha,
        eumelanin=1.3,
        pheomelanin=0.0,
        beta_m=0.3,
        beta_n=0.3,
        alpha_degrees=2.0,
        ior=1.55,
        # This groom is RISE's own default, not a translation of an
        # artist's Cycles material -- there is no Blender appearance to
        # match, so the parity rescale would only darken a value the
        # `hair_material` chunk itself documents as brown-black.
        apply_melanin_parity_rescale=False,
    )
    state.hair_materials.append(payload)
    cache[sentinel] = payload.name
    return payload.name


def _hair_material_payload(material, state: _ExportState) -> str:
    """Resolve the material bound to a hair-curves object into a
    HairMaterialData, returning its name.  Mirrors `_material_payload`
    structurally (pointer-keyed cache, one entry per distinct
    material) but is otherwise independent -- see this section's
    banner comment for why."""

    cache = state.hair_material_map
    key = _pointer_key(material)
    if key in cache:
        return cache[key]

    def _fallback() -> str:
        name = _default_hair_material(state)
        cache[key] = name
        return name

    if material is None:
        return _fallback()

    if not uses_nodes(material):
        _warn_once(
            state,
            f"RISE hair export needs a node-based Principled Hair BSDF material; "
            f"'{material.name_full}' has no node tree. Using the default hair material.",
        )
        return _fallback()

    output_node = _find_material_output(material)
    node = _find_hair_bsdf_through_surface(output_node)
    if node is None:
        _warn_once(
            state,
            f"RISE hair export needs a Principled Hair BSDF reaching Material Output.Surface on "
            f"'{material.name_full}' (Mix Shader / other BSDF chains ahead of it are not supported "
            "for hair -- there is no bake fallback). Using the default hair material.",
        )
        return _fallback()

    material_name = material.name_full

    if not _hair_graph_supported(node, material_name, state):
        return _fallback()

    for socket_name in _HAIR_UNSUPPORTED_RANDOM_SOCKETS:
        socket = _node_input(node, socket_name)
        if socket is not None and (socket.is_linked or abs(float(socket.default_value)) > 1e-6):
            _warn_once(
                state,
                f"RISE ignores Principled Hair BSDF '{socket_name}' on '{material_name}' -- "
                "per-strand randomisation is not representable by RISE's hair_material (one "
                "BCSDF instance shades the whole groom).",
            )

    # Shared across all three parametrizations.  Each is captured as its
    # constant socket value AND, when the socket is genuinely
    # texture-driven, as a registered painter name the bridge turns into
    # an IScalarPainter (ABI v10 -- see HairMaterialData's docstring).
    #
    # The painter is registered ONLY in the textured case.  With no
    # texture bound, `_scalar_or_texture_painter` would synthesize a
    # uniform painter carrying the same constant the numeric field
    # already carries, and sending its name would buy an extra painter
    # plus a scalar wrapper per material to say exactly what the float
    # says -- so `None` is passed instead and the number is used.
    #
    # The gate is the RESOLVED texture, not a validated one: if a chain
    # resolves to an image `_validate_texture_wrapper` then rejects
    # (non-UV coords, a projection/extension RISE has no form for, an
    # unsupported file type -- each already warned about there),
    # `_scalar_or_texture_painter` degrades to a uniform painter carrying
    # this same constant.  That is numerically identical to the numeric
    # field (uniform painters are registered Rec709-linear, i.e.
    # verbatim), just one painter more expensive -- correct, and not
    # worth a second validation pass here to avoid.
    beta_m_value = _socket_default_float(node, "Roughness", 0.3)
    beta_m_texture = _maybe_resolve_socket_texture(node, "Roughness", None, colorspace_is_data=True)
    beta_m = (
        _scalar_or_texture_painter(state, f"{material_name}_beta_m", beta_m_value, beta_m_texture)
        if beta_m_texture
        else None
    )

    beta_n_value = _socket_default_float(node, "Radial Roughness", 0.3)
    beta_n_texture = _maybe_resolve_socket_texture(node, "Radial Roughness", None, colorspace_is_data=True)
    beta_n = (
        _scalar_or_texture_painter(state, f"{material_name}_beta_n", beta_n_value, beta_n_texture)
        if beta_n_texture
        else None
    )

    ior_value = _socket_default_float(node, "IOR", 1.55)
    ior_texture = _maybe_resolve_socket_texture(node, "IOR", None, colorspace_is_data=True)
    ior = (
        _scalar_or_texture_painter(state, f"{material_name}_ior", ior_value, ior_texture)
        if ior_texture
        else None
    )

    offset_socket = _node_input(node, "Offset")
    if offset_socket is not None and offset_socket.is_linked:
        _warn_once(
            state,
            f"RISE reads Principled Hair BSDF 'Offset' on '{material_name}' as a constant; "
            "a linked Offset is not supported.",
        )
    offset_radians = _socket_default_float(node, "Offset", 0.034906585)  # Blender default: 2 degrees
    alpha_degrees = offset_radians_to_alpha_degrees(offset_radians)
    alpha = _add_uniform_painter(state, f"{material_name}_alpha", (alpha_degrees, alpha_degrees, alpha_degrees))

    parametrization = getattr(node, "parametrization", _HAIR_PARAM_MELANIN)

    if parametrization == _HAIR_PARAM_COLOR:
        color_texture = _maybe_resolve_socket_texture(node, "Color", None, colorspace_is_data=False)
        color_default = _socket_default_color(node, "Color", (0.03, 0.02, 0.015))
        color_painter = _color_or_texture_painter(state, f"{material_name}_hair_color", color_default, color_texture)
        payload = HairMaterialData(
            name=_unique_name(state, "hairmat", material_name),
            tier="color",
            color_painter_name=color_painter,
            alpha_painter_name=alpha,
            beta_m_texture_painter_name=beta_m,
            beta_n_texture_painter_name=beta_n,
            ior_texture_painter_name=ior,
            beta_m=beta_m_value,
            beta_n=beta_n_value,
            alpha_degrees=alpha_degrees,
            ior=ior_value,
        )

    elif parametrization == _HAIR_PARAM_ABSORPTION:
        sigma_socket = _node_input(node, "Absorption Coefficient")
        if sigma_socket is not None and sigma_socket.is_linked:
            _warn_once(
                state,
                f"RISE reads Principled Hair BSDF 'Absorption Coefficient' on '{material_name}' as a "
                "constant colour; a texture-driven input is read at its socket default only.",
            )
        sigma_default = _socket_default_color(node, "Absorption Coefficient", (0.245, 0.46, 1.6))
        sigma_painter = _add_uniform_painter(state, f"{material_name}_sigma_a", sigma_default)
        payload = HairMaterialData(
            name=_unique_name(state, "hairmat", material_name),
            tier="sigma_a",
            sigma_a_painter_name=sigma_painter,
            alpha_painter_name=alpha,
            beta_m_texture_painter_name=beta_m,
            beta_n_texture_painter_name=beta_n,
            ior_texture_painter_name=ior,
            sigma_a=tuple(float(channel) for channel in sigma_default),
            beta_m=beta_m_value,
            beta_n=beta_n_value,
            alpha_degrees=alpha_degrees,
            ior=ior_value,
        )

    elif parametrization == _HAIR_PARAM_MELANIN:
        # The default parametrization, and Blender's own default.
        melanin_socket = _node_input(node, "Melanin")
        redness_socket = _node_input(node, "Melanin Redness")
        if (melanin_socket is not None and melanin_socket.is_linked) or (
            redness_socket is not None and redness_socket.is_linked
        ):
            _warn_once(
                state,
                f"RISE reads Principled Hair BSDF 'Melanin'/'Melanin Redness' on '{material_name}' as "
                "constants; linked inputs are read at their socket defaults only.",
            )
        melanin = _socket_default_float(node, "Melanin", 0.8)
        redness = _socket_default_float(node, "Melanin Redness", 0.0)
        eumelanin_value, pheomelanin_value = melanin_to_eumelanin_pheomelanin(melanin, redness)
        eumelanin_painter = _add_uniform_painter(
            state, f"{material_name}_eumelanin", (eumelanin_value, eumelanin_value, eumelanin_value)
        )
        pheomelanin_painter = _add_uniform_painter(
            state, f"{material_name}_pheomelanin", (pheomelanin_value, pheomelanin_value, pheomelanin_value)
        )
        payload = HairMaterialData(
            name=_unique_name(state, "hairmat", material_name),
            tier="melanin",
            eumelanin_painter_name=eumelanin_painter,
            pheomelanin_painter_name=pheomelanin_painter,
            alpha_painter_name=alpha,
            beta_m_texture_painter_name=beta_m,
            beta_n_texture_painter_name=beta_n,
            ior_texture_painter_name=ior,
            eumelanin=eumelanin_value,
            pheomelanin=pheomelanin_value,
            beta_m=beta_m_value,
            beta_n=beta_n_value,
            alpha_degrees=alpha_degrees,
            ior=ior_value,
            # A real translation of an artist's Cycles material: match
            # what they dialled in.  The native bridge applies the
            # per-pigment rescale (see rise_blender_bridge.cpp).
            apply_melanin_parity_rescale=True,
        )

    else:
        # A parametrization value we don't recognise -- e.g. a future
        # Blender version adds a fourth option.  The old `else: #
        # MELANIN` catch-all silently mis-bound anything unrecognised
        # to the melanin tier at whatever stale melanin/redness
        # defaults happened to be read; that's a silent mistranslation
        # of a real artist-authored value.  Degrade loudly instead:
        # warn once, fall back to the default groom, exactly like every
        # other "can't translate this" branch in this function.
        _warn_once(
            state,
            f"RISE does not recognise Principled Hair BSDF parametrization "
            f"'{parametrization}' on '{material_name}'. Using the default hair material.",
        )
        return _fallback()

    state.hair_materials.append(payload)
    cache[key] = payload.name
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

    # Derive the vertical FOV from Blender's actual view-frame.
    # `camera_data.angle_y` only describes the *sensor*-based vertical
    # FOV (= 2·atan(sensor_h/(2·lens))) and ignores sensor_fit / aspect
    # ratio interactions, so passing it directly gave wrong projections
    # for the default `sensor_fit = AUTO` case: Cycles' AUTO resolves
    # to HORIZONTAL fit on default-sensor (36×24) landscape / square
    # images, where the effective vertical FOV is the *horizontal*
    # FOV divided by image aspect.  view_frame(scene=…) returns the
    # actual frustum corners that Cycles uses, sidestepping all of
    # this — half-height / |z| is exactly tan(vfov/2) for the render.
    #
    # IMPORTANT: when ``shift_y != 0`` the frame is ASYMMETRIC around
    # the camera-local origin — top and bottom corners differ in
    # |y|.  `max(abs(c.y))` then picks the larger absolute value,
    # producing a FOV that's too wide by the shift amount (lone-monk
    # has shift_y = 0.071 → 64° instead of the correct 56°).  Use
    # `(max_y - min_y) / 2` instead so the half-height is centred on
    # the actual viewport mid-line regardless of lens-shift.  RISE's
    # ``AddPinholeCamera`` doesn't currently honour the principal-
    # point offset itself; the resulting render will still be a few
    # pixels off-centre vs Cycles, but the FOV is now correct.
    if projection_type == 0:
        try:
            frame = camera_data.view_frame(scene=bpy.context.scene)
        except TypeError:
            # Defensive fallback for older Blender APIs that don't
            # accept the `scene` kwarg; produces a render-aspect-
            # ignorant frame but at least returns valid numbers.
            frame = camera_data.view_frame()
        ys = [c.y for c in frame]
        half_height = (max(ys) - min(ys)) * 0.5
        depth = max(abs(frame[0].z), 1e-8)
        fov_y_radians = 2.0 * math.atan(half_height / depth)
    else:
        fov_y_radians = float(getattr(camera_data, "angle_y", 0.78539816339))

    # Realistic-camera fields (ABI v7).  ``dof`` is an optional sub-
    # struct on the camera data — guard against API absence in older
    # / stub Blender contexts (some headless test rigs synthesise
    # cameras without it).  When DOF is OFF, ``use_dof = 0`` and the
    # bridge takes the pinhole path; when ON, the bridge routes
    # through AddThinlensCamera with the photographic optical model.
    dof = getattr(camera_data, "dof", None)
    use_dof = 1 if (dof is not None and bool(getattr(dof, "use_dof", False))) else 0
    fstop = float(getattr(dof, "aperture_fstop", 0.0)) if dof is not None else 0.0
    aperture_blades = int(getattr(dof, "aperture_blades", 0)) if dof is not None else 0
    aperture_rotation = float(getattr(dof, "aperture_rotation", 0.0)) if dof is not None else 0.0
    aperture_ratio = float(getattr(dof, "aperture_ratio", 1.0)) if dof is not None else 1.0

    # Focus distance.  Per Blender's `BKE_camera_object_dof_distance`
    # (source/blender/blenkernel/intern/camera.cc):
    #
    #     if (cam->dof.focus_object) {
    #         // ... compute dot of (target - camera) with view_dir
    #         return max_ff(fabsf(dot), 1e-5f);
    #     }
    #     return cam->dof.focus_distance;
    #
    # Critically: once `focus_object` is set, Blender ALWAYS uses the
    # camera-axis-projected distance (clamped to 1e-5 minimum) and
    # NEVER falls back to the numeric `dof.focus_distance` field.  A
    # focus target sitting in the camera's plane (projection ≈ 0)
    # gives Blender's near-focus 1e-5 clamp, not the stale numeric
    # value (adversarial review round 11, P2).
    #
    # Fallback to `dof.focus_distance` happens ONLY when:
    #   - `focus_object` is None (no override set), or
    #   - the focus-object world matrix is unreachable (API error /
    #     stub object) — defensive guard, not Blender behaviour.
    #
    # `focus_subtarget` on armature focus objects resolves the bone's
    # live posed head position.
    focus_distance = 0.0
    resolved_from_focus_object = False
    if dof is not None:
        focus_object = getattr(dof, "focus_object", None)
        if focus_object is not None:
            try:
                # Resolve target world position, honouring
                # focus_subtarget on armature focus objects.
                target_world = None
                subtarget = str(getattr(dof, "focus_subtarget", "") or "")
                if (
                    subtarget
                    and getattr(focus_object, "type", None) == "ARMATURE"
                ):
                    pose = getattr(focus_object, "pose", None)
                    bone = (
                        pose.bones.get(subtarget) if pose is not None else None
                    )
                    if bone is not None:
                        # `bone.head` is local to the armature; the
                        # armature object's matrix_world brings it to
                        # world space.  `bone.head_local` is in armature
                        # rest space — we want the live posed location.
                        target_world = focus_object.matrix_world @ bone.head
                if target_world is None:
                    target_world = focus_object.matrix_world.translation

                cam_pos = camera_object.matrix_world.translation
                # Camera-axis depth = |dot(forward, target - camera)|,
                # clamped to 1e-5.  Matches BKE_camera_object_dof_distance.
                dxv = float(target_world.x) - float(cam_pos.x)
                dyv = float(target_world.y) - float(cam_pos.y)
                dzv = float(target_world.z) - float(cam_pos.z)
                projected = (
                    forward[0] * dxv
                    + forward[1] * dyv
                    + forward[2] * dzv
                )
                focus_distance = max(abs(projected), 1e-5)
                resolved_from_focus_object = True
            except (AttributeError, TypeError):
                # API hiccup (stub object without matrix_world, etc.):
                # leave resolved_from_focus_object False so we fall
                # back to focus_distance below.
                pass
        if not resolved_from_focus_object:
            focus_distance = float(getattr(dof, "focus_distance", 0.0))

    # Resolve Blender's `sensor_fit` into a definitive HORIZONTAL /
    # VERTICAL flag so the bridge doesn't have to redo Cycles' AUTO
    # rule.  Per Blender's `BKE_camera_sensor_fit(sensor_fit, sizex,
    # sizey)` (source/blender/blenkernel/intern/camera.cc), AUTO is
    # decided by the RENDER GATE aspect — NOT the physical sensor
    # aspect.  The function is called with the rendered image's pixel-
    # aspect-aware extents:
    #
    #     sizex = render_w * pixel_aspect_x
    #     sizey = render_h * pixel_aspect_y
    #
    # then picks HORIZONTAL when sizex >= sizey, else VERTICAL.  Our
    # earlier image_aspect-vs-sensor_aspect rule was wrong: on a 4:3
    # or 1:1 render with the default 36×24 sensor it would have
    # picked VERTICAL fit (because 1.33 < 1.5 and 1.0 < 1.5) when
    # Blender actually picks HORIZONTAL (because 4:3 and 1:1 render
    # gates are still "wider-or-equal" by gate-aspect rule).
    sensor_fit_str = str(getattr(camera_data, "sensor_fit", "AUTO"))
    if sensor_fit_str == "VERTICAL":
        sensor_fit_vertical = 1
    elif sensor_fit_str == "HORIZONTAL":
        sensor_fit_vertical = 0
    else:
        gate_x = render_settings.width * float(getattr(render_settings, "pixel_aspect", 1.0))
        gate_y = render_settings.height
        sensor_fit_vertical = 0 if gate_x >= gate_y else 1

    # ISO: Cycles has no native concept, but RISE's photographic model
    # uses it for EV computation.  We surface it as an add-on property
    # on the RISE camera settings (registered alongside the rasterizer
    # selector) — fall back to 0 (= disabled) when the property doesn't
    # exist yet (older add-on installs).
    rise_camera_settings = getattr(camera_data, "rise_camera", None)
    iso = float(getattr(rise_camera_settings, "iso", 0.0)) if rise_camera_settings is not None else 0.0

    scene_unit_meters = 1.0
    try:
        scene_unit_meters = float(bpy.context.scene.unit_settings.scale_length)
    except (AttributeError, TypeError):
        pass
    if scene_unit_meters <= 0.0:
        scene_unit_meters = 1.0

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
        focal_length_mm=float(getattr(camera_data, "lens", 50.0)),
        sensor_width_mm=float(getattr(camera_data, "sensor_width", 36.0)),
        sensor_height_mm=float(getattr(camera_data, "sensor_height", 24.0)),
        sensor_fit_vertical=sensor_fit_vertical,
        use_dof=use_dof,
        fstop=fstop,
        focus_distance=focus_distance,
        aperture_blades=aperture_blades,
        aperture_rotation_radians=aperture_rotation,
        aperture_ratio=aperture_ratio,
        iso=iso,
        scene_unit_meters=scene_unit_meters,
        clip_start=max(0.0, float(getattr(camera_data, "clip_start", 0.0))),
    )


def _light_payload(light_object, matrix_world, state: _ExportState) -> LightData | None:
    """Translate a Blender light to a RISE LightData.

    Radiometric calibration (audit item #10):

    - RISE PointLight / SpotLight expect ``radiantEnergy`` in W/sr
      (radiant intensity per steradian — see PointLight.cpp line 75:
      `emittedRadiance = cColor * radiantEnergy [watts/sr]`).
    - Cycles' point/spot `light.energy` is total radiant flux (W)
      treated as if emitted isotropically over the sphere.  So
      converting needs a 1/(4π) factor.
    - RISE DirectionalLight expects ``radiantEnergy`` as direct
      irradiance (no 1/d² applied — see DirectionalLight.cpp line 63).
      Cycles' sun `light.energy` is already irradiance (W/m²), so no
      conversion is needed.

    AREA lights return None here; the caller is expected to invoke
    `_register_area_light_mesh` which materialises the area light as
    an emissive mesh-luminary instead (so soft shadows, falloff, and
    shape-correct sampling match Cycles).
    """

    light = light_object.data
    if light.type == "AREA":
        return None

    light_type = {
        "POINT": 0,
        "SPOT": 1,
        "SUN": 2,
    }.get(light.type, 4)

    color = list(light.color)
    if getattr(light, "use_temperature", False):
        temperature_color = list(light.temperature_color)
        color[0] *= temperature_color[0]
        color[1] *= temperature_color[1]
        color[2] *= temperature_color[2]

    direction = (-matrix_world.col[2].to_3d()).normalized()
    position = matrix_world.translation

    # Blender energy + Cycles exposure (stops) → physical light units.
    raw_energy = float(light.energy) * (2.0 ** float(getattr(light, "exposure", 0.0)))
    raw_energy *= unnormalized_power_scale(light)
    if light.type in ("POINT", "SPOT"):
        # Cycles total-flux (W) → RISE radiant intensity (W/sr).
        # 1/(4π) ≈ 0.0795775.
        intensity = raw_energy / (4.0 * math.pi)
    else:
        # SUN / fallthrough: energy is already irradiance.
        intensity = raw_energy

    return LightData(
        name=_unique_name(state, "light", light_object.name_full),
        type=light_type,
        color=(float(color[0]), float(color[1]), float(color[2])),
        intensity=intensity,
        position=(float(position.x), float(position.y), float(position.z)),
        direction=(float(direction.x), float(direction.y), float(direction.z)),
        spot_size=float(getattr(light, "spot_size", 0.0)),
        spot_blend=float(getattr(light, "spot_blend", 0.0)),
    )


def _area_light_geometry(shape: str, size_x: float, size_y: float, segments: int = AREA_LIGHT_DISK_SEGMENTS):
    """Build a planar mesh (vertices, normals, uvs, indices) for an area
    light in the light's LOCAL space.  The mesh faces -Z (Blender's
    convention for area-light shine direction)."""

    verts: list[float] = []
    normals: list[float] = [0.0, 0.0, -1.0]
    uvs: list[float] = []
    vidx: list[int] = []
    nidx: list[int] = []
    uidx: list[int] = []

    if shape in ("SQUARE", "RECTANGLE"):
        hx = size_x * 0.5
        hy = size_y * 0.5
        verts.extend([-hx, -hy, 0.0,
                       hx, -hy, 0.0,
                       hx,  hy, 0.0,
                      -hx,  hy, 0.0])
        uvs.extend([0.0, 0.0, 0.0,
                    1.0, 0.0, 0.0,
                    1.0, 1.0, 0.0,
                    0.0, 1.0, 0.0])
        # Two triangles, wound so the +Z face is the back side
        # (single-sided emission goes out the -Z face).
        vidx.extend([0, 2, 1,  0, 3, 2])
        nidx.extend([0, 0, 0,  0, 0, 0])
        uidx.extend([0, 2, 1,  0, 3, 2])
    else:
        # DISK / ELLIPSE — triangle fan around the centre.
        hx = size_x * 0.5
        hy = size_y * 0.5
        verts.extend([0.0, 0.0, 0.0])
        uvs.extend([0.5, 0.5, 0.0])
        for i in range(segments):
            theta = (i / segments) * 2.0 * math.pi
            x = hx * math.cos(theta)
            y = hy * math.sin(theta)
            verts.extend([x, y, 0.0])
            uvs.extend([0.5 + 0.5 * math.cos(theta), 0.5 + 0.5 * math.sin(theta), 0.0])
        for i in range(segments):
            v0 = 0
            v1 = 1 + i
            v2 = 1 + ((i + 1) % segments)
            # Wound so emission goes -Z.
            vidx.extend([v0, v2, v1])
            nidx.extend([0, 0, 0])
            uidx.extend([v0, v2, v1])

    return verts, normals, uvs, vidx, nidx, uidx


def _register_area_light_mesh(light_object, matrix_world, state: _ExportState) -> ObjectData | None:
    """Materialise a Blender area light as an emissive mesh luminary.

    Returns the ObjectData for the emissive surface, or None when the
    light shape isn't a recognised area type.  The mesh + emissive
    material are added to ``state`` so the export caller doesn't need
    to plumb them separately.
    """

    light = light_object.data
    shape = getattr(light, "shape", "SQUARE")

    if shape == "RECTANGLE":
        size_x, size_y = float(light.size), float(light.size_y)
    elif shape == "ELLIPSE":
        size_x, size_y = float(light.size), float(light.size_y)
    else:
        # SQUARE / DISK / anything else uses the single size.
        size_x = size_y = float(light.size)

    if size_x <= 0.0 or size_y <= 0.0:
        return None

    # Emitted flux must equal Cycles'.  The mesh RISE emits from is the
    # inscribed polygon for DISK/ELLIPSE, and it is placed with the full
    # object matrix, so its WORLD area is the local polygon area times
    # the in-plane area scale |X x Y| of that matrix.
    #   Normalize ON  : flux = power                 -> M = power / world polygon area
    #   Normalize OFF : flux = power * world true area -> M = power * true / polygon
    # Measured against Cycles 5.3: the old `power / local true area`
    # rendered an object-scaled (x2) panel 4.0x too bright and disk /
    # ellipse lights 0.56% dark.
    plane_scale = matrix_world.col[0].to_3d().cross(matrix_world.col[1].to_3d()).length
    if area_light_polygon_area(shape, size_x, size_y) <= 0.0 or plane_scale <= 0.0:
        return None

    spread = float(getattr(light, "spread", math.pi))
    if spread < math.pi - 1.0e-3:
        _warn_once(
            state,
            f"RISE ignores the Spread ({math.degrees(spread):.0f} deg) of area light '{light_object.name_full}'; "
            f"it emits as a full Lambertian (180 deg) panel.",
        )

    # Cycles area-light energy is total radiant flux (W).  We encode
    # the painter as per-area radiant exitance M = energy / area
    # (W/m²); RISE's LambertianEmitter then applies the standard
    # Lambertian 1/π normalisation internally
    # (LambertianEmitter.cpp line 54:
    #   emittedRadiance = radExPainter.GetColor(ri) * INV_PI * scale).
    # Total emitted flux integrates back to ∫ L cosθ dω dA = M × A
    # = energy, matching Cycles.
    color = list(light.color)
    if getattr(light, "use_temperature", False):
        temperature_color = list(light.temperature_color)
        color[0] *= temperature_color[0]
        color[1] *= temperature_color[1]
        color[2] *= temperature_color[2]
    power = float(light.energy) * (2.0 ** float(getattr(light, "exposure", 0.0)))
    intensity = area_light_exitance(power, shape, size_x, size_y, plane_scale,
                                    bool(getattr(light, "normalize", True)))
    emission_color = (color[0] * intensity, color[1] * intensity, color[2] * intensity)

    base_name = _safe_name(light_object.name_full)
    emission_painter = _add_uniform_painter(state, f"{base_name}_area_emission", emission_color)
    black_painter = _black_painter(state)

    mat_name = _unique_name(state, "mat", f"{light_object.name_full}_area")
    state.materials.append(
        MaterialData(
            name=mat_name,
            model=MATERIAL_LAMBERT,
            diffuse_painter_name=black_painter,
            emission_painter_name=emission_painter,
            double_sided=False,
        )
    )

    verts, normals, uvs, vidx, nidx, uidx = _area_light_geometry(shape, size_x, size_y)
    mesh_name = _unique_name(state, "mesh", f"{light_object.name_full}_area")
    state.meshes.append(
        MeshData(
            name=mesh_name,
            vertices=verts,
            normals=normals,
            uvs=uvs,
            vertex_indices=vidx,
            normal_indices=nidx,
            uv_indices=uidx,
            double_sided=False,
            use_face_normals=False,
        )
    )

    object_name = _unique_name(state, "obj", f"{light_object.name_full}_area")
    obj = ObjectData(
        name=object_name,
        geometry_name=mesh_name,
        material_name=mat_name,
        transform=_flatten_matrix(matrix_world),
        casts_shadows=True,
        receives_shadows=True,
        visible=True,
    )
    state.objects.append(obj)
    return obj


def _validate_mesh_payload(mesh: MeshData):
    expected_index_count = len(mesh.vertex_indices)
    if mesh.tangent_attribute and len(mesh.tangent_attribute) != expected_index_count * 4:
        raise RuntimeError(f"RISE mesh export produced an invalid tangent buffer for '{mesh.name}'.")

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

    for attr in mesh.named_attributes:
        if not attr.name or len(attr.rgba) != expected_index_count * 4:
            raise RuntimeError(
                f"RISE mesh export produced an invalid named attribute '{attr.name}' for '{mesh.name}'."
            )


def _extract_named_mesh_attributes(
    mesh,
    attr_names: tuple[str, ...],
    vertex_indices: list[int],
    loop_indices: list[int],
    polygon_indices: list[int],
    state: _ExportState,
) -> list[MeshAttributeData]:
    """Extract requested GEOMETRY attributes from an evaluated Blender ``Mesh``
    into linear per-triangle-corner RGBA buffers matching ``vertex_indices``."""
    if not attr_names:
        return []
    attributes = getattr(mesh, "attributes", None)

    num_corners = len(vertex_indices)
    if num_corners == 0:
        return []

    extracted: list[MeshAttributeData] = []
    for attr_name in attr_names:
        if attr_name.startswith(procedural_expr.UV_MAP_ATTR_PREFIX):
            uv_name = attr_name[len(procedural_expr.UV_MAP_ATTR_PREFIX):]
            uv_layers = getattr(mesh, "uv_layers", None)
            uv_layer = uv_layers.get(uv_name) if uv_layers is not None else None
            if uv_layer is None or len(uv_layer.data) == 0:
                continue
            uv_count = len(uv_layer.data)
            uv_buf = array.array("f", [0.0]) * (uv_count * 2)
            uv_layer.data.foreach_get("uv", uv_buf)
            rgba_uv = array.array("f", [0.0]) * (num_corners * 4)
            for c_i, src_i in enumerate(loop_indices):
                base = c_i * 4
                if 0 <= src_i < uv_count:
                    s = src_i * 2
                    rgba_uv[base] = uv_buf[s]
                    rgba_uv[base + 1] = uv_buf[s + 1]
                rgba_uv[base + 2] = 0.0
                rgba_uv[base + 3] = 1.0
            extracted.append(MeshAttributeData(name=attr_name, rgba=rgba_uv.tolist()))
            continue

        if attr_name == procedural_expr.ISLAND_RANDOM_ATTR:
            # Connected-component island random per vertex matching Cycles
            # `blender/mesh.cpp:534-578` + `util/disjoint_set.h` + `hash_uint_to_float`:
            # rank-based DisjointSet over `mesh.edges`, then `hash_uint_to_float(find(v))`.
            num_verts = len(getattr(mesh, "vertices", ()))
            if num_verts <= 0:
                continue
            parents = list(range(num_verts))
            ranks = [0] * num_verts

            def _find(x: int) -> int:
                root = x
                while parents[root] != root:
                    root = parents[root]
                while parents[x] != root:
                    parent = parents[x]
                    parents[x] = root
                    x = parent
                return root

            edges = getattr(mesh, "edges", ())
            for edge in edges:
                everts = getattr(edge, "vertices", ())
                if len(everts) >= 2:
                    xr = _find(int(everts[0]))
                    yr = _find(int(everts[1]))
                    if xr == yr:
                        continue
                    if ranks[xr] < ranks[yr]:
                        xr, yr = yr, xr
                    parents[yr] = xr
                    if ranks[xr] == ranks[yr]:
                        ranks[xr] += 1

            vert_random = [_cycles_hash_uint_to_float(_find(v)) for v in range(num_verts)]
            rgba: list[float] = []
            for vi in vertex_indices:
                rv = vert_random[vi] if 0 <= vi < num_verts else 0.0
                rgba.extend((rv, rv, rv, 1.0))
            extracted.append(MeshAttributeData(name=attr_name, rgba=rgba))
            continue

        if attributes is None:
            continue

        lookup_name = attr_name
        if attr_name == procedural_expr.ACTIVE_COLOR_ATTR:
            color_attrs = getattr(mesh, "color_attributes", None)
            lookup_name = str(
                getattr(color_attrs, "default_color_name", "")
                or getattr(color_attrs, "active_color_name", "")
                or ""
            )
            if not lookup_name:
                continue

        attr = attributes.get(lookup_name)
        if attr is None:
            continue
        domain = getattr(attr, "domain", "")
        data_type = getattr(attr, "data_type", "")
        count = len(attr.data)
        if count <= 0:
            continue

        if domain == "POINT":
            corner_indices = vertex_indices
        elif domain == "CORNER":
            corner_indices = loop_indices
        elif domain == "FACE":
            corner_indices = [p for p in polygon_indices for _ in range(3)]
        else:
            _warn_once(
                state,
                f"RISE skips mesh attribute '{attr_name}' on unsupported domain '{domain}' "
                "(only POINT, CORNER, and FACE domains are supported).",
            )
            continue

        if len(corner_indices) != num_corners:
            continue

        rgba = array.array("f", [0.0]) * (num_corners * 4)
        if data_type == "FLOAT":
            buf = array.array("f", [0.0]) * count
            attr.data.foreach_get("value", buf)
            for c_i, src_i in enumerate(corner_indices):
                v = buf[src_i] if 0 <= src_i < count else 0.0
                base = c_i * 4
                rgba[base] = v
                rgba[base + 1] = v
                rgba[base + 2] = v
                rgba[base + 3] = 1.0
        elif data_type == "INT":
            buf = array.array("i", [0]) * count
            attr.data.foreach_get("value", buf)
            for c_i, src_i in enumerate(corner_indices):
                v = float(buf[src_i]) if 0 <= src_i < count else 0.0
                base = c_i * 4
                rgba[base] = v
                rgba[base + 1] = v
                rgba[base + 2] = v
                rgba[base + 3] = 1.0
        elif data_type == "INT8":
            # Blender RNA `ByteIntAttributeValue.value` is PROP_RAW_INT8 (1-byte signed).
            buf = array.array("b", [0]) * count
            attr.data.foreach_get("value", buf)
            for c_i, src_i in enumerate(corner_indices):
                v = float(buf[src_i]) if 0 <= src_i < count else 0.0
                base = c_i * 4
                rgba[base] = v
                rgba[base + 1] = v
                rgba[base + 2] = v
                rgba[base + 3] = 1.0
        elif data_type == "BOOLEAN":
            buf = [False] * count
            attr.data.foreach_get("value", buf)
            for c_i, src_i in enumerate(corner_indices):
                v = (1.0 if buf[src_i] else 0.0) if 0 <= src_i < count else 0.0
                base = c_i * 4
                rgba[base] = v
                rgba[base + 1] = v
                rgba[base + 2] = v
                rgba[base + 3] = 1.0
        elif data_type == "FLOAT2":
            buf = array.array("f", [0.0]) * (count * 2)
            attr.data.foreach_get("vector", buf)
            for c_i, src_i in enumerate(corner_indices):
                base = c_i * 4
                if 0 <= src_i < count:
                    s = src_i * 2
                    rgba[base] = buf[s]
                    rgba[base + 1] = buf[s + 1]
                rgba[base + 2] = 0.0
                rgba[base + 3] = 1.0
        elif data_type == "FLOAT_VECTOR":
            buf = array.array("f", [0.0]) * (count * 3)
            attr.data.foreach_get("vector", buf)
            for c_i, src_i in enumerate(corner_indices):
                base = c_i * 4
                if 0 <= src_i < count:
                    s = src_i * 3
                    rgba[base] = buf[s]
                    rgba[base + 1] = buf[s + 1]
                    rgba[base + 2] = buf[s + 2]
                rgba[base + 3] = 1.0
        elif data_type in ("FLOAT_COLOR", "BYTE_COLOR"):
            # Both FLOAT_COLOR.color and BYTE_COLOR.color return scene-linear
            # RGBA floats in Blender 4.0+/5.3 RNA (verified live on 5.3).
            buf = array.array("f", [0.0]) * (count * 4)
            attr.data.foreach_get("color", buf)
            for c_i, src_i in enumerate(corner_indices):
                base = c_i * 4
                if 0 <= src_i < count:
                    s = src_i * 4
                    rgba[base] = buf[s]
                    rgba[base + 1] = buf[s + 1]
                    rgba[base + 2] = buf[s + 2]
                    rgba[base + 3] = buf[s + 3]
        else:
            _warn_once(
                state,
                f"RISE skips mesh attribute '{attr_name}' with unsupported data type '{data_type}'.",
            )
            continue

        extracted.append(MeshAttributeData(name=attr_name, rgba=rgba.tolist()))

    return extracted


def _mesh_buckets(eval_object, state: _ExportState, matrix_world=None, instance_info=None) -> list[tuple[int, str, _MaterialBinding]]:
    # GeometryNodes-instanced geometry reuses a small pool of
    # eval_object prototypes across many instances and rebinds their
    # materials in-place during depsgraph iteration.  Caching by
    # eval_object.as_pointer() alone collides — e.g., the tadpoles
    # demo flips one prototype between 'head' and 'tail' partway
    # through 10 000 instances.  Mix the per-instance material
    # signature into the cache key so head and tail bind correctly.
    slot_mat_sig = tuple(_pointer_key(s.material) for s in eval_object.material_slots)
    data_obj = getattr(eval_object, "data", None)
    data_ptr = data_obj.as_pointer() if (data_obj is not None and hasattr(data_obj, "as_pointer")) else 0
    data_mat_sig = tuple(_pointer_key(m) for m in getattr(data_obj, "materials", ()))
    from . import tangent_bake
    materials = [slot.material for slot in eval_object.material_slots]
    materials.extend(getattr(data_obj, "materials", []))
    linked_tangent = any((socket := tangent_bake.find_tangent_socket(mat)) is not None
                         and socket.is_linked for mat in materials)
    matrix_world = matrix_world if matrix_world is not None else eval_object.matrix_world
    # Object/world-dependent graph bakes are instance-local, including identical
    # transforms. Generated mesh materials also participate in this decision.
    # Include both eval_object.data pointer and data.materials pointers so
    # Geometry Nodes instances reusing an eval_object wrapper with different
    # mesh data / attributes / materials never collide (R1 B-P1-8).
    cache_key = (eval_object.as_pointer(), data_ptr, slot_mat_sig, data_mat_sig)
    cached = state.geometry_cache.get(cache_key)
    if cached is not None and not linked_tangent:
        return cached

    mesh = eval_object.to_mesh()
    if mesh is None:
        state.geometry_cache[cache_key] = []
        return []

    results: list[tuple[int, str, _MaterialBinding]] = []
    try:
        mesh.calc_loop_triangles()
        triangle_count = len(mesh.loop_triangles)
        if triangle_count == 0:
            state.geometry_cache[cache_key] = []
            return []

        vertex_values = array.array("f", [0.0]) * (len(mesh.vertices) * 3)
        mesh.vertices.foreach_get("co", vertex_values)
        mesh_bbox = None
        if len(vertex_values) >= 3:
            xs = vertex_values[0::3]
            ys = vertex_values[1::3]
            zs = vertex_values[2::3]
            mesh_bbox = (
                (float(min(xs)), float(min(ys)), float(min(zs))),
                (float(max(xs)), float(max(ys)), float(max(zs))),
            )

        normal_values = array.array("f", [0.0]) * (len(mesh.loops) * 3)
        if len(mesh.loops):
            mesh.corner_normals.foreach_get("vector", normal_values)

        uv_values = array.array("f")
        primary_uv_layer = next(
            (layer for layer in mesh.uv_layers if getattr(layer, "active_render", False)),
            mesh.uv_layers.active,
        )
        has_uv = primary_uv_layer is not None and len(primary_uv_layer.data) == len(mesh.loops)
        if has_uv:
            raw_uv_values = array.array("f", [0.0]) * (len(mesh.loops) * 2)
            primary_uv_layer.data.foreach_get("uv", raw_uv_values)
            uv_values = array.array("f", [0.0]) * (len(mesh.loops) * 3)
            uv_values[0::3] = raw_uv_values[0::2]
            uv_values[1::3] = raw_uv_values[1::2]

        triangle_vertices = array.array("I", [0]) * (triangle_count * 3)
        triangle_loops = array.array("I", [0]) * (triangle_count * 3)
        triangle_materials = array.array("I", [0]) * triangle_count
        triangle_polygons = array.array("I", [0]) * triangle_count
        mesh.loop_triangles.foreach_get("vertices", triangle_vertices)
        mesh.loop_triangles.foreach_get("loops", triangle_loops)
        mesh.loop_triangles.foreach_get("material_index", triangle_materials)
        mesh.loop_triangles.foreach_get("polygon_index", triangle_polygons)

        buckets: dict[int, dict[str, list[int]]] = {}
        for tri_index, material_index in enumerate(triangle_materials):
            bucket = buckets.setdefault(
                int(material_index),
                {"vertex_indices": [], "normal_indices": [], "uv_indices": [], "polygon_indices": []},
            )
            start = tri_index * 3
            bucket["vertex_indices"].extend(int(v) for v in triangle_vertices[start:start + 3])
            bucket["normal_indices"].extend(int(v) for v in triangle_loops[start:start + 3])
            if has_uv:
                bucket["uv_indices"].extend(int(v) for v in triangle_loops[start:start + 3])
            bucket["polygon_indices"].append(int(triangle_polygons[tri_index]))

        mesh_attr_types = {
            a.name: getattr(a, "data_type", "")
            for a in getattr(mesh, "attributes", ())
        }

        for material_index, bucket in buckets.items():
            # GeometryNodes-generated meshes put per-instance materials
            # on the mesh data, not on the host object's material_slots
            # (the original geonodes object often has zero slots).
            # Read the mesh's own slot first; fall back to the object
            # override for cases where the mesh slot is None.
            material = None
            if material_index < len(mesh.materials):
                material = mesh.materials[material_index]
            if material is None and material_index < len(eval_object.material_slots):
                slot_material = eval_object.material_slots[material_index].material
                if slot_material is not None:
                    material = slot_material

            binding = _material_payload(material, state, mesh_bbox=mesh_bbox, attr_types=mesh_attr_types)
            directions = tangent_bake.corner_tangents(mesh, material, matrix_world, eval_object, instance_info, bucket["normal_indices"], allow_parallel=bool(binding.modifier_name))
            named_attrs = _extract_named_mesh_attributes(
                mesh,
                binding.mesh_attributes,
                bucket["vertex_indices"],
                bucket["normal_indices"],
                bucket["polygon_indices"],
                state,
            )
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
                tangent_is_shader_direction=bool(directions),
                tangent_attribute=([component for corner in bucket["normal_indices"]
                                    for component in directions[corner]] if directions else []),
                named_attributes=named_attrs,
            )
            _validate_mesh_payload(mesh_payload)
            state.meshes.append(mesh_payload)
            results.append((material_index, mesh_name, binding))
    finally:
        eval_object.to_mesh_clear()

    if not linked_tangent:
        state.geometry_cache[cache_key] = results
    return results


def compute_render_region(
    full_width: int,
    full_height: int,
    use_border: bool,
    border_min_x: float,
    border_max_x: float,
    border_min_y: float,
    border_max_y: float,
) -> tuple[bool, int, int, int, int]:
    """Convert Blender's bottom-up fractional border rect to RISE's top-down
    inclusive pixel region ``(use_region, left, top, right, bottom)``.

    Mirrors Blender's ``pipeline.cc`` (``re_init_resolution`` / ``RE_InitState``):
      * ``border_*`` are stored in single-precision float DNA fields and
        truncated to int after multiplying by ``winx`` / ``winy``.
      * A full ``[0, 1] x [0, 1]`` border disables border mode.
      * Bottom-up ``[ymin, ymax)`` maps to top-down inclusive
        ``[full_height - ymax, full_height - 1 - ymin]``.
    """
    if not use_border or full_width <= 0 or full_height <= 0:
        return (False, 0, 0, 0, 0)

    bmin_x = ctypes.c_float(float(border_min_x)).value
    bmax_x = ctypes.c_float(float(border_max_x)).value
    bmin_y = ctypes.c_float(float(border_min_y)).value
    bmax_y = ctypes.c_float(float(border_max_y)).value

    if bmin_x == 0.0 and bmax_x == 1.0 and bmin_y == 0.0 and bmax_y == 1.0:
        return (False, 0, 0, 0, 0)

    # Match Blender C++ `int(r->border.xmin * re->winx)` where `border.*` is
    # 32-bit float and `winx`/`winy` promote to 32-bit float before multiply.
    fw = ctypes.c_float(float(full_width)).value
    fh = ctypes.c_float(float(full_height)).value
    xmin = max(0, min(full_width, int(ctypes.c_float(bmin_x * fw).value)))
    xmax = max(0, min(full_width, int(ctypes.c_float(bmax_x * fw).value)))
    ymin = max(0, min(full_height, int(ctypes.c_float(bmin_y * fh).value)))
    ymax = max(0, min(full_height, int(ctypes.c_float(bmax_y * fh).value)))
    if xmax <= xmin or ymax <= ymin:
        return (False, 0, 0, 0, 0)

    rise_left = xmin
    rise_right = xmax - 1
    rise_top = full_height - ymax
    rise_bottom = full_height - 1 - ymin
    return (True, rise_left, rise_top, rise_right, rise_bottom)


def build_render_settings(scene) -> RenderSettingsData:
    render = scene.render
    width = max((int(render.resolution_x) * int(render.resolution_percentage)) // 100, 1)
    height = max((int(render.resolution_y) * int(render.resolution_percentage)) // 100, 1)
    pixel_aspect = float(render.pixel_aspect_x) / max(float(render.pixel_aspect_y), 1e-8)
    rise = scene.rise
    temporary_directory = bpy.app.tempdir or tempfile.gettempdir()

    use_region, region_left, region_top, region_right, region_bottom = compute_render_region(
        width,
        height,
        bool(getattr(render, "use_border", False)),
        float(getattr(render, "border_min_x", 0.0)),
        float(getattr(render, "border_max_x", 1.0)),
        float(getattr(render, "border_min_y", 0.0)),
        float(getattr(render, "border_max_y", 1.0)),
    )
    film_transparent = bool(getattr(render, "film_transparent", False))

    rasterizer_kind, use_path_tracing = resolve_rasterizer_selection(
        getattr(rise, "rasterizer_type", "1"), rise.use_path_tracing)

    return RenderSettingsData(
        width=width,
        height=height,
        pixel_aspect=pixel_aspect,
        pixel_samples=int(rise.pixel_samples),
        light_samples=int(rise.light_samples),
        max_recursion=int(rise.max_recursion),
        use_path_tracing=use_path_tracing,
        choose_one_light=bool(rise.choose_one_light),
        show_lights=bool(rise.show_lights),
        use_world_ambient=bool(rise.use_world_ambient),
        sms_enabled=bool(rise.sms_enabled),
        sms_max_iterations=int(rise.sms_max_iterations),
        sms_threshold=float(rise.sms_threshold),
        sms_max_chain_depth=int(rise.sms_max_chain_depth),
        sms_biased=bool(rise.sms_biased),
        sms_bernoulli_trials=int(rise.sms_bernoulli_trials),
        sms_multi_trials=int(rise.sms_multi_trials),
        sms_photon_count=int(rise.sms_photon_count),
        sms_max_photon_seeds=int(rise.sms_max_photon_seeds),
        sms_two_stage=bool(rise.sms_two_stage),
        sms_use_levenberg_marquardt=bool(rise.sms_use_levenberg_marquardt),
        sms_seeding_mode=int(rise.sms_seeding_mode),
        sms_target_bounces=int(rise.sms_target_bounces),
        sms_extended=bool(rise.sms_extended),
        adaptive_max_samples=int(rise.adaptive_max_samples),
        adaptive_threshold=float(rise.adaptive_threshold),
        adaptive_show_map=bool(rise.adaptive_show_map),
        path_guiding_enabled=bool(rise.path_guiding_enabled),
        path_guiding_training_iterations=int(rise.path_guiding_training_iterations),
        path_guiding_training_spp=int(rise.path_guiding_training_spp),
        path_guiding_combine_training=bool(rise.path_guiding_combine_training),
        path_guiding_online=bool(rise.path_guiding_online),
        path_guiding_warmup_iterations=int(rise.path_guiding_warmup_iterations),
        path_guiding_alpha=float(rise.path_guiding_alpha),
        path_guiding_learned_alpha=bool(rise.path_guiding_learned_alpha),
        path_guiding_max_depth=int(rise.path_guiding_max_depth),
        path_guiding_max_light_depth=int(rise.path_guiding_max_light_depth),
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
        stability_use_light_bvh=bool(rise.stability_use_light_bvh),
        stability_optimal_mis=bool(rise.stability_optimal_mis),
        stability_optimal_mis_training_iterations=int(rise.stability_optimal_mis_training_iterations),
        stability_optimal_mis_tile_size=int(rise.stability_optimal_mis_tile_size),
        oidn_denoise=bool(rise.oidn_denoise),
        oidn_quality=int(rise.oidn_quality),
        oidn_device=int(rise.oidn_device),
        oidn_prefilter=int(rise.oidn_prefilter),
        progressive_enabled=bool(rise.progressive_enabled),
        progressive_samples_per_pass=int(rise.progressive_samples_per_pass),
        use_zsobol=bool(rise.use_zsobol),
        pixel_filter=int(rise.pixel_filter),
        pixel_filter_width=float(rise.pixel_filter_width),
        pixel_filter_param_a=float(rise.pixel_filter_param_a),
        pixel_filter_param_b=float(rise.pixel_filter_param_b),
        temporary_directory=str(temporary_directory),
        # Rasterizer EnumProperty stores the int as a string; resolved
        # above (an explicit "0" also clears use_path_tracing so the
        # native bridge renders the legacy pixel rasterizer it names).
        rasterizer_kind=rasterizer_kind,
        bidir_max_eye_depth=int(getattr(rise, "bidir_max_eye_depth", 0)),
        bidir_max_light_depth=int(getattr(rise, "bidir_max_light_depth", 0)),
        vcm_merge_radius=float(getattr(rise, "vcm_merge_radius", 0.0)),
        vcm_enable_vc=bool(getattr(rise, "vcm_enable_vc", True)),
        vcm_enable_vm=bool(getattr(rise, "vcm_enable_vm", True)),
        mlt_bootstrap=int(getattr(rise, "mlt_bootstrap", 0)),
        mlt_chains=int(getattr(rise, "mlt_chains", 0)),
        mlt_mutations_per_pixel=int(getattr(rise, "mlt_mutations_per_pixel", 0)),
        mlt_large_step_prob=float(getattr(rise, "mlt_large_step_prob", 0.0)),
        use_region=use_region,
        region_left=region_left,
        region_top=region_top,
        region_right=region_right,
        region_bottom=region_bottom,
        film_transparent=film_transparent,
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


def _walk_world_for_environment(output_node):
    """Walk the world shader graph from `output_node.Surface`, returning
    ``(env_node, background, is_camera_visible, uniform_fallback)``.

    `env_node` is the first Environment Texture reached and the
    surrounding `background` Background node.  `is_camera_visible`
    records whether the Env Tex is reachable from camera rays.

    `uniform_fallback` is `(color_rgb, strength, camera_visible)` when
    no Environment Texture is found but a Background node with an
    UNLINKED Color was reachable — this is the default Blender scene
    case (plain grey world), where Cycles still integrates the
    Background's solid colour as a uniform hemispherical IBL.  We
    surface that so the caller can install it as a uniform-colour
    radiance map.  `None` when no such background exists either.

    Handles the very common Cycles pattern:

        World Output ← Mix Shader (Factor=Light Path.Is Camera Ray)
                          ├── Shader 0  (used when factor=0,
                          │              i.e. indirect rays):  HDRI
                          └── Shader 1  (used when factor=1,
                                         i.e. camera rays):    black / colour

    plus plain ``Background → Environment Texture`` chains and a few
    pass-through nodes (RGB Curves, Hue/Saturation, Bright/Contrast,
    Gamma) that artists drop between the HDRI and the Background.
    """

    PASS_THROUGH = {
        "ShaderNodeRGBCurve",
        "ShaderNodeBrightContrast",
        "ShaderNodeHueSaturation",
        "ShaderNodeGamma",
        "ShaderNodeInvert",
    }

    surface = _node_input(output_node, "Surface")
    if surface is None or not surface.is_linked:
        return None, None, True, None

    # BFS over the shader graph, tagging each visited node with whether
    # it's reachable from camera rays / from indirect rays.  Start at
    # the Surface output, which is reachable from both.
    visited: dict[int, tuple[bool, bool, "Background | None"]] = {}
    queue: list[tuple[object, bool, bool, object]] = [
        (surface.links[0].from_node, True, True, None)
    ]
    env_found = None
    env_camera_visible = False
    env_background = None
    uniform_fallback = None       # (rgb, strength, camera_visible) — unlinked-Color Background
    procedural_fallback = None    # (background_node, strength, camera_visible) — linked-Color Background that doesn't reach an Env Tex (Sky Texture, Noise, etc.)

    while queue:
        node, cam, ind, last_background = queue.pop(0)
        key = rna_key(node)
        prev_cam, prev_ind, prev_bg = visited.get(key, (False, False, None))
        new_cam = prev_cam or cam
        new_ind = prev_ind or ind
        if new_cam == prev_cam and new_ind == prev_ind and prev_bg == last_background:
            continue
        visited[key] = (new_cam, new_ind, last_background)

        bl = node.bl_idname

        if bl == "ShaderNodeTexEnvironment":
            if env_found is None or (not env_camera_visible and new_ind):
                env_found = node
                env_camera_visible = new_cam
                env_background = last_background
            continue

        if bl == "ShaderNodeMixShader":
            factor = node.inputs[0] if len(node.inputs) > 0 else None
            shader_a = node.inputs[1] if len(node.inputs) > 1 else None
            shader_b = node.inputs[2] if len(node.inputs) > 2 else None
            gated_by_camera = False
            if factor is not None and factor.is_linked:
                src = factor.links[0]
                if (src.from_node.bl_idname == "ShaderNodeLightPath"
                        and src.from_socket.name == "Is Camera Ray"):
                    gated_by_camera = True
            if gated_by_camera:
                # factor=0 → Shader 0 used for indirect rays (and
                # transmission, glossy, etc. — all non-camera).
                # factor=1 → Shader 1 used for camera rays.
                if shader_a is not None and shader_a.is_linked:
                    queue.append((shader_a.links[0].from_node, False, ind, last_background))
                if shader_b is not None and shader_b.is_linked:
                    queue.append((shader_b.links[0].from_node, cam, False, last_background))
            else:
                # Unknown / non-Light-Path factor — both branches
                # contribute equally to both reachabilities.
                for s in (shader_a, shader_b):
                    if s is not None and s.is_linked:
                        queue.append((s.links[0].from_node, cam, ind, last_background))
            continue

        if bl == "ShaderNodeAddShader":
            for s in node.inputs[:2]:
                if s.is_linked:
                    queue.append((s.links[0].from_node, cam, ind, last_background))
            continue

        if bl == "ShaderNodeBackground":
            color_in = _node_input(node, "Color")
            # Strength is the same in both branches — capture it now.
            strength_val = 1.0
            strength_in = _node_input(node, "Strength")
            if strength_in is not None and not strength_in.is_linked:
                try:
                    strength_val = float(strength_in.default_value)
                except TypeError:
                    pass
            if color_in is not None and color_in.is_linked:
                # Linked Color → procedural chain.  Even if we keep
                # walking and find an Env Tex downstream, capture this
                # as a fallback in case we don't.  This catches Sky
                # Texture / Noise / Voronoi / etc. — anything the
                # walker can't trace to a baked image.
                if procedural_fallback is None or (not procedural_fallback[2] and cam):
                    procedural_fallback = (node, strength_val, cam)
                queue.append((color_in.links[0].from_node, cam, ind, node))
            else:
                # Unlinked Color → uniform-colour world.  Capture it
                # as a fallback in case we don't find an Env Tex
                # anywhere else in the graph.  This is the default
                # Blender scene (Background with a plain grey RGB),
                # which Cycles integrates as hemispherical IBL.
                if uniform_fallback is None or (not uniform_fallback[2] and cam):
                    rgb_default = (0.05, 0.05, 0.05)
                    if color_in is not None:
                        try:
                            value = color_in.default_value
                            rgb_default = (float(value[0]), float(value[1]), float(value[2]))
                        except (TypeError, IndexError):
                            pass
                    uniform_fallback = (rgb_default, strength_val, cam)
            continue

        if bl in PASS_THROUGH:
            for inp in node.inputs:
                if inp.is_linked and inp.type == "RGBA":
                    queue.append((inp.links[0].from_node, cam, ind, last_background))
            continue

        # Unknown node — follow every linked colour/shader input
        # conservatively.  Avoids missing an HDRI tucked behind a
        # MixRGB or similar.
        for inp in node.inputs:
            if inp.is_linked and inp.type in {"RGBA", "SHADER", "VECTOR", "VALUE"}:
                queue.append((inp.links[0].from_node, cam, ind, last_background))

    return env_found, env_background, env_camera_visible, uniform_fallback, procedural_fallback


def _find_sky_texture_node(start_node, *, max_depth: int = 16):
    """Walk back from a Background-style node's Color input looking for a
    ``ShaderNodeTexSky``.  Traverses ``NodeReroute`` and the standard
    pass-through colour filters so we still recognise the sky behind a
    RGB-Curves / HSV / Gamma chain that a lighting artist may insert.

    Returns the ``ShaderNodeTexSky`` node, or ``None`` if no sky texture
    is reachable.
    """

    if start_node is None:
        return None

    visited: set[int] = set()
    # Seed the walk from the Color (or first RGBA) input of `start_node`.
    color_socket = _node_input(start_node, "Color")
    if color_socket is None or not color_socket.is_linked:
        return None

    stack = [(color_socket.links[0].from_node, 0)]
    while stack:
        node, depth = stack.pop()
        if depth > max_depth:
            continue
        if rna_key(node) in visited:
            continue
        visited.add(rna_key(node))

        bl = node.bl_idname
        if bl == "ShaderNodeTexSky":
            return node

        if bl == "NodeReroute":
            if node.inputs and node.inputs[0].is_linked:
                stack.append((node.inputs[0].links[0].from_node, depth + 1))
            continue

        if bl in _SOCKET_PASS_THROUGH_NODES:
            for inp in node.inputs:
                if inp.is_linked and inp.type in {"RGBA", "VALUE", "VECTOR"}:
                    stack.append((inp.links[0].from_node, depth + 1))
            continue

        # Unknown — stop this branch.
        continue

    return None


def _synthesise_nishita_sun_light(sky_node, state: _ExportState, *, scale: float = 1.0) -> LightData | None:
    """Translate a Blender Nishita Sky Texture's built-in sun disc into a
    RISE DirectionalLight.

    Without this, scenes that rely solely on a procedural Sky Texture
    (no explicit Sun light, no HDRI) get only the neutral-grey IBL
    fallback installed by `_world_radiance_map`, producing flat shading.
    The Nishita model bakes the directional sun + atmospheric scatter
    into a single shader that RISE cannot evaluate at runtime — so we
    extract just the directional sun and emit it as an explicit RISE
    DirectionalLight, leaving the IBL fallback to approximate the
    atmospheric scatter component.

    Geometry:
      Blender's Nishita parameters are ``sun_elevation`` (radians above
      horizon, ∈ [-π/2, π/2]) and ``sun_rotation`` (azimuth radians,
      CCW from the +Y axis when looking down).  The world-space vector
      FROM ground TO sun is:

          ux = -cos(elev) * sin(rot)
          uy =  cos(elev) * cos(rot)
          uz =  sin(elev)

      The bridge's directional-light handler (see
      `rise_blender_bridge.cpp` near `RISE_BLENDER_LIGHT_SUN`) negates
      `LightData.direction` before passing it to RISE — it expects the
      "shine direction" (Blender convention: from light TO surface) and
      flips it to RISE's "FROM surface TO light" convention.  We
      therefore emit the *negated* unit vector so the bridge's negation
      lands on the correct FROM-surface-TO-sun in RISE:

          direction = (-ux, -uy, -uz) = (cos(elev)*sin(rot),
                                          -cos(elev)*cos(rot),
                                          -sin(elev))

      Sanity (FROM-ground-TO-sun after bridge negation):
          elev=0, rot=0   →  (0,  1, 0)  — sun at the +Y horizon.
          elev=π/2        →  (0,  0, 1)  — zenith.
          elev=0, rot=π/2 →  (-1, 0, 0)  — -X horizon.

    Radiometry:
      `sun_intensity` in Cycles is a unitless multiplier on the
      Nishita-predicted solar irradiance.  At ``sun_intensity=1`` the
      Nishita atmospheric model produces a peak sun-disc radiance of
      ~120–220 in linear Rec.709 (verified via EXR dump on the
      lone-monk demo scene: max R=223, p95=27, median=4.3).  To match
      that brightness window — important so Blender's Filmic + scene
      exposure produces a comparable tonemapped image — we deliver a
      directional sun at irradiance ~30·sun_intensity.  Combined with
      a Lambertian albedo around 0.5 and cos(45°)≈0.7 angle, that
      yields outgoing radiance ~3·sun_intensity, in the same order of
      magnitude as Cycles' median.  The caller can additionally tune
      via the `scale` argument (e.g. to bias warm overcast scenes).

    Colour:
      We use a fixed warm-white (1.00, 0.95, 0.88) — a reasonable
      proxy for the Nishita-modelled colour at mid elevations.  Hard-
      coding rather than re-implementing the Nishita atmospheric
      transmission model end-to-end; the loss in fidelity vs Cycles
      is well-bounded (the actual Nishita output varies ~5 % around
      this in the [20°, 70°] elevation range).
    """

    sun_elevation = float(getattr(sky_node, "sun_elevation", 0.5))
    sun_rotation = float(getattr(sky_node, "sun_rotation", 0.0))
    sun_intensity = max(0.0, float(getattr(sky_node, "sun_intensity", 1.0)))
    sun_disc = getattr(sky_node, "sun_disc", True)
    if not sun_disc or sun_intensity < 1e-6:
        return None

    ce, se = math.cos(sun_elevation), math.sin(sun_elevation)
    sr, cr = math.sin(sun_rotation), math.cos(sun_rotation)
    # Pre-negate so the bridge's `light.direction = -light.direction`
    # for SUN-type lights produces RISE's FROM-surface-TO-sun vector.
    dir_x =  ce * sr
    dir_y = -ce * cr
    dir_z = -se

    # Calibrated against Cycles' Nishita output on lone-monk
    # (cycles median radiance ≈ 4.3, max ≈ 223 with sun_intensity=1).
    # Pre-fix RISE was delivering median 0.43 / max 1.06 — ~10× too
    # dim for the same Filmic + exposure pipeline.  Boosted to 30 W/m².
    irradiance = 30.0 * sun_intensity * scale

    return LightData(
        name=_unique_name(state, "light", "world_nishita_sun"),
        type=2,  # DirectionalLight (see _light_payload's mapping)
        color=(1.00, 0.95, 0.88),
        intensity=irradiance,
        position=(0.0, 0.0, 0.0),
        direction=(dir_x, dir_y, dir_z),
        spot_size=0.0,
        spot_blend=0.0,
    )


def _synthesise_world_lights(scene, state: _ExportState) -> list[LightData]:
    """Synthesise any extra RISE lights implied by the world shader.

    Today: a single Nishita Sky Texture sun disc.  Future hooks for
    Preetham/Hosek-Wilkie sky models or photon-aided directional sources
    drop in here.

    Returns an empty list when the world has no recognisable directional
    contribution; the caller still gets the IBL via `_world_radiance_map`.
    """

    if not uses_nodes(scene.world):
        return []

    output_node = _find_world_output(scene.world)
    if output_node is None:
        return []

    _env, _bg, _vis, _uniform, procedural = _walk_world_for_environment(output_node)
    if procedural is None:
        return []

    background_node = procedural[0]
    sky_node = _find_sky_texture_node(background_node)
    if sky_node is None:
        return []

    sky_type = getattr(sky_node, "sky_type", "NISHITA")
    # Blender 5.x renamed `NISHITA` to `SINGLE_SCATTERING` (the simpler
    # of two new physical models); `MULTI_SCATTERING` is the higher-
    # fidelity successor.  Both expose the same sun_disc / sun_elevation
    # / sun_rotation / sun_intensity API as legacy Nishita, so we treat
    # them identically.
    NISHITA_LIKE = {"NISHITA", "SINGLE_SCATTERING", "MULTI_SCATTERING"}
    if sky_type not in NISHITA_LIKE:
        # Hosek-Wilkie / Preetham don't expose a sun disc; their
        # directional component is baked into the dome and would
        # need a different extraction path.  Future work.
        _warn_once(
            state,
            f"RISE only synthesises a sun light from Nishita-class Sky Textures; '{sky_type}' sky_type was found.",
        )
        return []

    sun = _synthesise_nishita_sun_light(sky_node, state)
    if sun is None:
        return []
    return [sun]


def _world_radiance_map(scene, state: _ExportState) -> tuple[str | None, float, tuple[float, float, float], bool]:
    """Walk the world's Surface output for image-based lighting.

    Returns ``(painter_name, strength, orientation_xyz_radians,
    is_background)``.  ``painter_name`` is None when no Environment
    Texture, uniform Background or procedural sky is reachable from the
    world's surface output; `export_scene` then exports a non-node world's
    colour as a uniform environment, and the deprecated `use_world_ambient`
    ambient approximation applies only when the user enabled it.

    Supports the trivial ``Background → Environment Texture`` chain
    and the standard Cycles ``Mix Shader gated by Light Path.Is
    Camera Ray`` pattern (HDRI for indirect, separate colour for the
    camera-visible background).
    """
    if not uses_nodes(scene.world):
        return None, 1.0, (0.0, 0.0, 0.0), True

    output_node = _find_world_output(scene.world)
    if output_node is None:
        return None, 1.0, (0.0, 0.0, 0.0), True

    env_node, background_node, is_camera_visible, uniform_fallback, procedural_fallback = _walk_world_for_environment(output_node)

    if env_node is None:
        # No HDRI in the graph.  Two fallback paths in priority order:
        # 1. Procedural shader chain (Sky Texture, Noise, etc.) →
        #    warn the user and use a neutral grey IBL as a placeholder
        #    so the scene at least lights up.  Pre-baking the world
        #    to an Environment Texture from inside our `render()`
        #    callback would require calling `bpy.ops.render.render()`
        #    recursively, which Blender ignores the engine override on
        #    and re-enters our engine — infinite recursion, stack
        #    overflow, crash.  The robust path (subprocess-based bake
        #    or a user-driven "Bake World" button) is a future round.
        # 2. Plain uniform Background → register a uniform-colour
        #    painter (cheapest path, no bake needed).
        if procedural_fallback is not None:
            _bg_node, strength, camera_visible = procedural_fallback
            _warn_once(
                state,
                "RISE: World contains a procedural shader (Sky Texture, Noise, etc.) "
                "which RISE can't evaluate directly.  Using a sky-tinted IBL fallback "
                "plus a synthesised directional sun (Nishita only) so the scene gets "
                "both ambient and directional illumination — to recover the exact sky, "
                "bake your World shader to an HDR Environment Texture (File → External "
                "Data → Bake World to HDR, or render the world with Cycles to an EXR "
                "and plug that EXR into a Background → Environment Texture chain)."
            )
            # HDR sky-blue ambient.  Calibrated to bring RISE radiance
            # into the same window as Cycles' Nishita output (median
            # ~4 W/m·sr, p95 ~17 in blue channel).  Values >1 are
            # essential — clamped-to-1 IBLs make Filmic + exposure
            # produce a flat, muddy image because there's no HDR
            # headroom for tone-mapping to play with.  The chroma
            # ratio (R:G:B = 0.55:0.73:1.00) approximates atmospheric
            # Rayleigh scattering's blue tilt at zenith.
            painter_name = _add_uniform_painter(
                state,
                "world_radiance_procedural_fallback",
                (3.30, 4.40, 6.05),
            )
            return painter_name, float(strength), (0.0, 0.0, 0.0), bool(camera_visible)
        if uniform_fallback is not None:
            rgb, strength, camera_visible = uniform_fallback
            painter_name = _add_uniform_painter(
                state,
                "world_radiance_uniform",
                (rgb[0], rgb[1], rgb[2]),
            )
            return painter_name, float(strength), (0.0, 0.0, 0.0), bool(camera_visible)
        _warn_once(
            state,
            "RISE could not find a usable World shader graph, so no environment light is exported "
            "(the world renders black; Film > Transparent only hides the background, it never "
            "supplies light).  Connect a Background (colour or Environment Texture) to the World "
            "Output; the deprecated 'Approximate World' option still substitutes an unshadowed "
            "ambient light from the world viewport colour.",
        )
        return None, 1.0, (0.0, 0.0, 0.0), True

    # Background strength scales the HDRI.  When the HDRI sits in the
    # indirect-only branch of a Mix Shader, the camera-visible branch
    # (often a black or tinted Background) is currently dropped — the
    # camera will see the HDRI itself as the background.  Override via
    # is_background = False so we leave camera rays unaffected.
    strength = 1.0
    if background_node is not None:
        strength_socket = _node_input(background_node, "Strength")
        if strength_socket is not None and not strength_socket.is_linked:
            strength = float(strength_socket.default_value)

    image = env_node.image
    if image is None:
        return None, 1.0, (0.0, 0.0, 0.0), True

    if image.source not in ("FILE", "GENERATED"):
        _warn_once(state, f"RISE only supports file-backed or packed Environment Textures (source={image.source!r}).")
        return None, 1.0, (0.0, 0.0, 0.0), True

    filepath = _resolve_image_for_bridge(image, state)
    if not filepath or not os.path.exists(filepath):
        _warn_once(state, "RISE could not resolve the Environment Texture's image file (and unpack-to-temp also failed).")
        return None, 1.0, (0.0, 0.0, 0.0), True

    extension = os.path.splitext(filepath)[1].lower()
    kind = SUPPORTED_IMAGE_KINDS.get(extension)
    if kind is None:
        _warn_once(state, "RISE supports HDR / EXR / PNG / JPG / TIFF Environment Textures.")
        return None, 1.0, (0.0, 0.0, 0.0), True

    # The Environment Texture's Vector input optionally feeds through
    # a Mapping node for rotation.  We map the Mapping node's Rotation
    # input (Euler XYZ in radians) into RadianceMapConfig.orientation.
    orientation = (0.0, 0.0, 0.0)
    vector_socket = _node_input(env_node, "Vector")
    if vector_socket is not None and vector_socket.is_linked:
        mapping = vector_socket.links[0].from_node
        if mapping.bl_idname == "ShaderNodeMapping":
            rotation_socket = _node_input(mapping, "Rotation")
            if rotation_socket is not None and not rotation_socket.is_linked:
                rot = rotation_socket.default_value
                orientation = (float(rot[0]), float(rot[1]), float(rot[2]))

    # Image colour space — env textures are typically linear HDR /
    # EXR, but PNG / TIFF that the user marks as a colour image must
    # still gamma-decode on read.
    color_space = COLOR_SPACE_LINEAR
    if kind in (PAINTER_TEXTURE_PNG, PAINTER_TEXTURE_TIFF):
        if not getattr(image.colorspace_settings, "is_data", False) and image.colorspace_settings.name == "sRGB":
            color_space = COLOR_SPACE_SRGB

    painter_name = _add_texture_painter(
        state,
        "world_radiance",
        filepath,
        kind,
        color_space,
    )
    return painter_name, strength, orientation, bool(is_camera_visible)


def _world_volume_medium(scene, state: _ExportState) -> str | None:
    world = scene.world
    if not uses_nodes(world):
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


def _cycles_hash_string(name: str) -> int:
    """Matches Cycles `util/hash.h` `hash_string(const char*)` (signed char)."""
    i = 0
    for byte in name.encode("utf-8"):
        c = byte if byte < 128 else (byte - 256)
        i = (i * 37 + c) & 0xFFFFFFFF
    return i


def _cycles_hash_uint2(kx: int, ky: int) -> int:
    """Matches Cycles `util/hash.h` Jenkins lookup3 `hash_uint2(kx, ky)`."""
    mask = 0xFFFFFFFF

    def _rot(x: int, k: int) -> int:
        return ((x << k) & mask) | (x >> (32 - k))

    a = b = c = (0xDEADBEEF + (2 << 2) + 13) & mask
    b = (b + (ky & mask)) & mask
    a = (a + (kx & mask)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 14)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 11)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 25)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 16)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 4)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 14)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 24)) & mask
    return c


def _cycles_hash_uint(kx: int) -> int:
    """Matches Cycles `util/hash.h` Jenkins lookup3 `hash_uint(kx)`."""
    mask = 0xFFFFFFFF

    def _rot(x: int, k: int) -> int:
        return ((x << k) & mask) | (x >> (32 - k))

    a = b = c = (0xDEADBEEF + (1 << 2) + 13) & mask
    a = (a + (kx & mask)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 14)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 11)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 25)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 16)) & mask
    a = (a ^ c) & mask
    a = (a - _rot(c, 4)) & mask
    b = (b ^ a) & mask
    b = (b - _rot(a, 14)) & mask
    c = (c ^ b) & mask
    c = (c - _rot(b, 24)) & mask
    return c


def _cycles_hash_uint_to_float(kx: int) -> float:
    import struct
    h = _cycles_hash_uint(int(kx) & 0xFFFFFFFF)
    inv = struct.unpack("<f", struct.pack("<f", 1.0 / float(0xFFFFFFFF)))[0]
    return float(struct.unpack("<f", struct.pack("<f", float(h) * inv))[0])


def _cycles_hash_uint2_to_float(kx: int, ky: int) -> float:
    import struct
    h = _cycles_hash_uint2(int(kx) & 0xFFFFFFFF, int(ky) & 0xFFFFFFFF)
    inv = struct.unpack("<f", struct.pack("<f", 1.0 / float(0xFFFFFFFF)))[0]
    return float(struct.unpack("<f", struct.pack("<f", float(h) * inv))[0])


_CYCLES_DUMMY_PARTICLE_RANDOM = _cycles_hash_uint2_to_float(0, 0)


def _cycles_particle_random(object_instance) -> float:
    """Compute Cycles' `Particle Info -> Random` scalar in [0, 1].
    Mirrors Cycles `blender/particles.cpp` (`pa.index = persistent_id[0]`,
    skipping child particles with `pid >= len(psys.particles)`) +
    `scene/particles.cpp` (`kparticles[0]` dummy particle -> `hash_uint2_to_float(0, 0)`
    for non-particle objects)."""
    if not getattr(object_instance, "is_instance", False):
        return _CYCLES_DUMMY_PARTICLE_RANDOM
    psys = getattr(object_instance, "particle_system", None)
    if psys is None:
        return _CYCLES_DUMMY_PARTICLE_RANDOM
    pid_seq = getattr(object_instance, "persistent_id", None)
    if not pid_seq:
        return _CYCLES_DUMMY_PARTICLE_RANDOM
    pid = int(pid_seq[0])
    particles = getattr(psys, "particles", None)
    if pid < 0 or (particles is not None and pid >= len(particles)):
        return _CYCLES_DUMMY_PARTICLE_RANDOM
    return _cycles_hash_uint2_to_float(pid, 0)


def _cycles_object_random(name: str, is_instance: bool, random_id: int) -> float:
    """Compute Cycles' `Object Info -> Random` scalar in [0, 1].

    Mirrors Cycles `blender/object.cpp` (`sync_object`) + `scene/object.cpp`
    (`device_update_object_transform`):
      - For instances (`is_instance` True): use `DepsgraphObjectInstance.random_id`,
        reinterpreting Blender Python's signed 32-bit int as uint32.
      - For regular objects: `hash_uint2(hash_string(obj.name), 0)`.
      - Evaluate `(float)rid * (1.0f / (float)0xFFFFFFFF)` in IEEE 754 float32
        (where `(float)0xFFFFFFFF == 4294967296.0f`) to match Cycles bit-for-bit.
    """
    import struct

    if is_instance:
        rid = int(random_id) & 0xFFFFFFFF
    else:
        rid = _cycles_hash_uint2(_cycles_hash_string(name), 0)
    f_rid = struct.unpack("<f", struct.pack("<f", float(rid)))[0]
    inv_u32 = struct.unpack("<f", struct.pack("<f", 1.0 / 4294967296.0))[0]
    return float(struct.unpack("<f", struct.pack("<f", f_rid * inv_u32))[0])


def export_scene(depsgraph) -> tuple[SceneData, RenderSettingsData]:
    scene = depsgraph.scene
    render_settings = build_render_settings(scene)
    state = _ExportState()

    camera_object = scene.camera.evaluated_get(depsgraph) if scene.camera else None
    camera = _camera_payload(camera_object, render_settings, state)

    mesh_instances = []
    lights: list[LightData] = []
    global_medium_name = _world_volume_medium(scene, state)

    # Collection-skip list from the RISE addon settings.  Lower-cased
    # for case-insensitive comparison.  Used to filter out low-poly
    # proxy / AO-baking shells (e.g. the lone-monk demo's `blocks`
    # collection) that obscure the camera in RISE — Cycles silently
    # skips them via its own adaptive-sampling heuristics, but RISE's
    # straight-ahead path-tracer respects them as opaque surfaces.
    rise_settings = getattr(scene, "rise", None)
    skip_patterns = set()
    if rise_settings is not None:
        raw = getattr(rise_settings, "skip_collections_pattern", "") or ""
        for part in raw.split(","):
            cleaned = part.strip().lower()
            if cleaned:
                skip_patterns.add(cleaned)

    def _object_in_skipped_collection(obj) -> bool:
        if not skip_patterns:
            return False
        for col in obj.users_collection:
            if col.name.lower() in skip_patterns:
                return True
        return False

    for object_instance in depsgraph.object_instances:
        is_instance = getattr(object_instance, "is_instance", False) and getattr(object_instance, "instance_object", None) is not None
        base_object = (
            object_instance.instance_object
            if is_instance
            else object_instance.object
        )
        if base_object is None:
            continue

        original_object = getattr(base_object, "original", base_object)
        # Skip hidden-from-render originals — BUT only for non-instance
        # objects.  For GeoNodes / particle instances, the source mesh
        # is intentionally hidden (it's a template that exists only to
        # be instanced); its `hide_render=True` means "don't render the
        # source", NOT "don't render the instances".  Cycles handles
        # this by-design; we have to mirror it explicitly here.
        # Motivating scene: ripple_dreams_fields — `instance_cube`,
        # `instance_sphere_1/2` and `material_placeholder` are all
        # hide_render=True templates feeding `geonodes_bubbles` /
        # `geonodes_pebbles` / `geonodes_floating`.  Previously every
        # GeoNodes-instanced bubble / pebble / gemstone was dropped.
        if not is_instance and getattr(original_object, "hide_render", False):
            continue
        # For instance generators (objects with a GeoNodes / particle
        # modifier that EMIT instances), the depsgraph emits the
        # generator itself as an instance with is_instance=False —
        # don't skip it if it has its own renderable geometry, but DO
        # skip if the artist hid the generator and Cycles wouldn't
        # render its own mesh either.
        if _object_in_skipped_collection(original_object):
            continue

        if original_object.type in GEOMETRY_TYPES:
            eval_object = object_instance.object
            if eval_object is None:
                continue

            _warn_legacy_particle_hair(original_object, state)

            mesh_buckets = _mesh_buckets(eval_object, state, object_instance.matrix_world.copy(), {"is_instance": object_instance.is_instance, "random_id": object_instance.random_id,
                "orco": tuple(object_instance.orco), "uv": tuple(object_instance.uv), "parent": object_instance.parent,
                "particle_settings": object_instance.particle_system.settings if object_instance.particle_system else None,
                "private_instance_attributes": bool(object_instance.is_instance and object_instance.parent and any(m.type == 'NODES' for m in object_instance.parent.modifiers))})
            if not mesh_buckets:
                continue

            raw_color = getattr(eval_object, "color", (1.0, 1.0, 1.0, 1.0))
            obj_color = (
                float(raw_color[0]),
                float(raw_color[1]),
                float(raw_color[2]),
                float(raw_color[3]) if len(raw_color) > 3 else 1.0,
            )
            obj_index = float(getattr(eval_object, "pass_index", 0))
            obj_random = _cycles_object_random(
                getattr(eval_object, "name", original_object.name),
                is_instance,
                int(getattr(object_instance, "random_id", 0)),
            )
            obj_particle_random = _cycles_particle_random(object_instance)

            mesh_instances.append(
                {
                    "mesh_buckets": list(mesh_buckets),
                    "object_name": original_object.name_full,
                    "transform": _flatten_matrix(object_instance.matrix_world.copy()),
                    "object_color": obj_color,
                    "object_index": obj_index,
                    "object_random": obj_random,
                    "particle_random": obj_particle_random,
                }
            )
        elif original_object.type == "CURVES":
            _export_hair_object(object_instance, original_object, state)
        elif original_object.type == "LIGHT":
            light_data = _light_payload(object_instance.object, object_instance.matrix_world.copy(), state)
            if light_data is not None:
                lights.append(light_data)
            elif object_instance.object.data.type == "AREA":
                # Area lights become emissive mesh-luminaries so the
                # PT integrator's NEE sees them with real falloff +
                # soft shadows + shape-correct sampling.
                _register_area_light_mesh(
                    object_instance.object,
                    object_instance.matrix_world.copy(),
                    state,
                )
        elif original_object.type == "VOLUME":
            _volume_object_medium(original_object, object_instance.object, object_instance.matrix_world.copy(), state)

    for index, mesh_instance in enumerate(mesh_instances):
        mesh_buckets = mesh_instance["mesh_buckets"]
        object_name = mesh_instance["object_name"]
        transform = mesh_instance["transform"]
        obj_color = mesh_instance["object_color"]
        obj_index = mesh_instance["object_index"]
        obj_random = mesh_instance["object_random"]
        obj_particle_random = mesh_instance["particle_random"]

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
                    shader_name=binding.shader_name,
                    object_color=obj_color,
                    object_index=obj_index,
                    object_random=obj_random,
                    particle_random=obj_particle_random,
                )
            )

    # World — image-based lighting takes priority; the ambient
    # approximation is only used when no Environment Texture is
    # configured.
    world_radiance_painter, world_radiance_scale, world_radiance_orientation, world_radiance_is_background = \
        _world_radiance_map(scene, state)

    # Some scenes (e.g. lone-monk_cycles_and_exposure-node_demo) have
    # zero explicit lights and rely entirely on the world shader's
    # built-in sun disc.  `_world_radiance_map` covers ambient via the
    # IBL fallback but silently drops the directional sun, so we add
    # a synthetic DirectionalLight here to restore primary lighting
    # for any sky type we can read parameters from.
    lights.extend(_synthesise_world_lights(scene, state))

    # A world that does not use nodes renders its viewport colour as a
    # uniform background in Cycles.  `_world_radiance_map` returns None for
    # it, so without this the world was simply dropped unless the deprecated
    # `use_world_ambient` approximation was on.  With that toggle OFF (the
    # default; legacy-deprecation Phase 2 slice C, owner ruling #5) the world
    # colour becomes a uniform environment light -- the same
    # `world_radiance_uniform` painter a node-based uniform Background gets.
    # A .blend that explicitly enabled `use_world_ambient` keeps the ambient
    # approximation below, unchanged.
    if (world_radiance_painter is None and not render_settings.use_world_ambient
            and scene.world is not None and not uses_nodes(scene.world)):
        world_rgb = _float_color3(scene.world.color)
        if any(c > 0.0 for c in world_rgb):
            world_radiance_painter = _add_uniform_painter(state, "world_radiance_uniform", world_rgb)
            world_radiance_scale = 1.0
            world_radiance_orientation = (0.0, 0.0, 0.0)
            world_radiance_is_background = True

    world_color = (0.0, 0.0, 0.0)
    world_strength = 0.0
    if world_radiance_painter is None and render_settings.use_world_ambient and scene.world is not None:
        if uses_nodes(scene.world):
            _warn_once(
                state,
                "RISE reduces non-image-based world shaders to the world viewport colour for ambient approximation; plug an Environment Texture into Background.Color for proper IBL.",
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
        world_radiance_painter_name=world_radiance_painter,
        world_radiance_scale=world_radiance_scale,
        world_radiance_orientation=world_radiance_orientation,
        world_radiance_is_background=world_radiance_is_background,
        warnings=state.warnings,
        hair_objects=state.hair_objects,
        hair_materials=state.hair_materials,
    )

    return scene_data, render_settings
