import bpy

from . import bridge, material_bake
from .engine import RISEBlenderRenderEngine
from .properties import addon_preferences


def _preferences_and_capabilities(context):
    preferences = addon_preferences(context)
    if not preferences:
        return None, None, "Add-on preferences are unavailable."

    try:
        capabilities = bridge.get_capabilities(preferences.bridge_library_path)
        return preferences, capabilities, None
    except bridge.BridgeError as exc:
        return preferences, None, str(exc)


class _RISEPanel(bpy.types.Panel):
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"
    COMPAT_ENGINES = {RISEBlenderRenderEngine.bl_idname}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES


class RISE_RENDER_PT_settings(_RISEPanel):
    bl_label = "RISE Settings"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        layout.prop(settings, "rasterizer_type")
        layout.prop(settings, "pixel_samples")
        layout.prop(settings, "light_samples")
        layout.prop(settings, "max_recursion")
        layout.prop(settings, "use_world_ambient")
        layout.prop(settings, "choose_one_light")
        layout.prop(settings, "show_lights")
        layout.prop(settings, "use_zsobol")
        layout.prop(settings, "skip_collections_pattern")

        rk = settings.rasterizer_type
        # BDPT / VCM / MLT subpath knobs — collapsible "Advanced"
        # row that only shows for the integrators that consume them.
        if rk in ("3", "4", "5", "6", "7", "8"):
            sub = layout.box()
            sub.label(text="Bidirectional / MLT", icon="LIGHT_DATA")
            sub.prop(settings, "bidir_max_eye_depth")
            sub.prop(settings, "bidir_max_light_depth")
        if rk in ("5", "6"):
            sub = layout.box()
            sub.label(text="VCM", icon="OUTLINER_OB_LIGHTPROBE")
            sub.prop(settings, "vcm_merge_radius")
            sub.prop(settings, "vcm_enable_vc")
            sub.prop(settings, "vcm_enable_vm")
        if rk in ("7", "8"):
            sub = layout.box()
            sub.label(text="MLT", icon="GROUP_VERTEX")
            sub.prop(settings, "mlt_bootstrap")
            sub.prop(settings, "mlt_chains")
            sub.prop(settings, "mlt_mutations_per_pixel")
            sub.prop(settings, "mlt_large_step_prob")


class RISE_RENDER_PT_path_tracing(_RISEPanel):
    bl_label = "Path Tracing"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        layout.enabled = settings.use_path_tracing
        layout.prop(settings, "sms_enabled")

        sms_col = layout.column()
        sms_col.enabled = settings.sms_enabled
        sms_col.prop(settings, "sms_seeding_mode")
        sms_col.prop(settings, "sms_target_bounces")
        sms_col.prop(settings, "sms_max_iterations")
        sms_col.prop(settings, "sms_threshold")
        sms_col.prop(settings, "sms_max_chain_depth")
        sms_col.prop(settings, "sms_biased")
        sms_col.prop(settings, "sms_bernoulli_trials")
        sms_col.prop(settings, "sms_multi_trials")
        sms_col.prop(settings, "sms_two_stage")
        sms_col.prop(settings, "sms_use_levenberg_marquardt")
        sms_col.prop(settings, "sms_photon_count")
        sms_col.prop(settings, "sms_max_photon_seeds")


class RISE_RENDER_PT_adaptive(_RISEPanel):
    bl_label = "Adaptive Sampling"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        layout.prop(settings, "adaptive_max_samples")

        detail_col = layout.column()
        detail_col.enabled = settings.adaptive_max_samples > 0
        detail_col.prop(settings, "adaptive_threshold")
        detail_col.prop(settings, "adaptive_show_map")


class RISE_RENDER_PT_guiding(_RISEPanel):
    bl_label = "Path Guiding"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        _preferences, capabilities, error_message = _preferences_and_capabilities(context)
        supports_guiding = capabilities is not None and capabilities.supports_path_guiding

        if error_message and capabilities is None:
            layout.label(text=error_message, icon="ERROR")

        toggle_row = layout.row()
        toggle_row.enabled = supports_guiding
        toggle_row.prop(settings, "path_guiding_enabled")

        if capabilities is not None and not capabilities.supports_path_guiding:
            layout.label(text="This bridge build does not include OpenPGL.", icon="INFO")

        guiding_col = layout.column()
        guiding_col.enabled = supports_guiding and settings.path_guiding_enabled
        guiding_col.prop(settings, "path_guiding_training_iterations")
        guiding_col.prop(settings, "path_guiding_training_spp")
        guiding_col.prop(settings, "path_guiding_combine_training")
        guiding_col.prop(settings, "path_guiding_online")
        guiding_col.prop(settings, "path_guiding_warmup_iterations")
        guiding_col.prop(settings, "path_guiding_alpha")
        guiding_col.prop(settings, "path_guiding_learned_alpha")
        guiding_col.prop(settings, "path_guiding_max_depth")
        guiding_col.prop(settings, "path_guiding_max_light_depth")
        guiding_col.prop(settings, "path_guiding_sampling_type")
        guiding_col.prop(settings, "path_guiding_ris_candidates")


class RISE_RENDER_PT_stability(_RISEPanel):
    bl_label = "Stability"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        layout.prop(settings, "stability_direct_clamp")
        layout.prop(settings, "stability_indirect_clamp")
        layout.prop(settings, "stability_filter_glossy")
        layout.prop(settings, "stability_rr_min_depth")
        layout.prop(settings, "stability_rr_threshold")
        layout.prop(settings, "stability_max_diffuse_bounce")
        layout.prop(settings, "stability_max_glossy_bounce")
        layout.prop(settings, "stability_max_transmission_bounce")
        layout.prop(settings, "stability_max_translucent_bounce")
        layout.prop(settings, "stability_max_volume_bounce")
        layout.prop(settings, "stability_use_light_bvh")
        layout.prop(settings, "stability_optimal_mis")
        omis_col = layout.column()
        omis_col.enabled = settings.stability_optimal_mis
        omis_col.prop(settings, "stability_optimal_mis_training_iterations")
        omis_col.prop(settings, "stability_optimal_mis_tile_size")


class RISE_RENDER_PT_output(_RISEPanel):
    bl_label = "Output"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        _preferences, capabilities, error_message = _preferences_and_capabilities(context)
        supports_oidn = capabilities is not None and capabilities.supports_oidn

        if error_message and capabilities is None:
            layout.label(text=error_message, icon="ERROR")

        oidn_row = layout.row()
        oidn_row.enabled = supports_oidn
        oidn_row.prop(settings, "oidn_denoise")

        oidn_col = layout.column()
        oidn_col.enabled = supports_oidn and settings.oidn_denoise
        oidn_col.prop(settings, "oidn_quality")
        oidn_col.prop(settings, "oidn_device")
        oidn_col.prop(settings, "oidn_prefilter")

        if capabilities is not None and not capabilities.supports_oidn:
            layout.label(text="This bridge build does not include OIDN.", icon="INFO")

        layout.separator()
        layout.prop(settings, "progressive_enabled")
        prog_col = layout.column()
        prog_col.enabled = settings.progressive_enabled
        prog_col.prop(settings, "progressive_samples_per_pass")

        layout.separator()
        layout.prop(settings, "pixel_filter")
        filter_col = layout.column()
        filter_col.enabled = settings.pixel_filter != "0"
        filter_col.prop(settings, "pixel_filter_width")
        filter_col.prop(settings, "pixel_filter_param_a")
        filter_col.prop(settings, "pixel_filter_param_b")


class RISE_RENDER_PT_bridge(_RISEPanel):
    bl_label = "Bridge"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        preferences = addon_preferences(context)
        if not preferences:
            layout.label(text="Add-on preferences are unavailable.", icon="ERROR")
            return

        layout.prop(preferences, "bridge_library_path")

        resolved_path, error_message = bridge.bridge_status(preferences.bridge_library_path)
        if error_message:
            layout.label(text=error_message, icon="ERROR")
            return

        layout.label(text=resolved_path, icon="CHECKMARK")

        try:
            capabilities = bridge.get_capabilities(preferences.bridge_library_path)
        except bridge.BridgeError as exc:
            layout.label(text=str(exc), icon="ERROR")
            return

        layout.label(text=f"ABI Version: {capabilities.api_version}", icon="INFO")
        layout.label(
            text=f"OIDN: {'Enabled' if capabilities.supports_oidn else 'Unavailable'}",
            icon="CHECKMARK" if capabilities.supports_oidn else "INFO",
        )
        layout.label(
            text=f"Path Guiding: {'Enabled' if capabilities.supports_path_guiding else 'Unavailable'}",
            icon="CHECKMARK" if capabilities.supports_path_guiding else "INFO",
        )
        layout.label(
            text=f"VDB Volumes: {'Enabled' if capabilities.supports_vdb_volumes else 'Unavailable'}",
            icon="CHECKMARK" if capabilities.supports_vdb_volumes else "INFO",
        )


class RISE_OT_bake_materials(bpy.types.Operator):
    """Bake complex Cycles material node graphs (procedural noise,
    AO masks, Mix Shaders, etc.) to PNG image textures so RISE can
    render them with full fidelity.

    For each material in the scene that the classifier deems
    "complex" (see ``docs/BLENDER_MATERIAL_TRANSLATION.md``), this
    operator:
      1. Selects an arbitrary object using that material
      2. Switches the scene render engine to Cycles
      3. Bakes Diffuse / Roughness / Normal to PNGs under
         ``<tmp>/rise_baked/<material_name>_*.png``
      4. Stores the resulting paths as ID properties on the material
         so subsequent RISE renders consume them automatically
      5. Restores the previous render engine

    Re-run this after editing a complex material's node graph; the
    new bake replaces the previous file.  "Simple" materials are
    skipped (the exporter translates their node graphs directly into
    RISE painters).
    """

    bl_idname = "rise.bake_materials"
    bl_label = "Bake Procedural Materials for RISE"
    bl_description = (
        "Bake complex Cycles material graphs (AO, Mix Shader, "
        "procedural noise) to PNG textures.  Run once after material "
        "edits.  Simple graphs are translated directly without baking."
    )
    bl_options = {"REGISTER"}

    bake_resolution: bpy.props.IntProperty(
        name="Bake Resolution",
        description="Square texture resolution (per channel) of the baked PNGs",
        default=1024,
        min=128,
        max=8192,
    )
    only_unbaked: bpy.props.BoolProperty(
        name="Skip Already-Baked Materials",
        description=(
            "When enabled, materials that already carry a "
            "rise_baked_diffuse_path are skipped — useful for "
            "incremental re-bakes after editing one material in a "
            "scene with many"
        ),
        default=False,
    )

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        scene = context.scene
        prev_engine = scene.render.engine
        scene.render.engine = "CYCLES"
        try:
            count = material_bake.bake_complex_materials_in_scene(
                scene,
                resolution=int(self.bake_resolution),
                only_unbaked=bool(self.only_unbaked),
                report=lambda msg: self.report({"INFO"}, msg),
            )
        finally:
            scene.render.engine = prev_engine

        if count == 0:
            self.report(
                {"INFO"},
                "RISE: No complex materials needed baking (or all already baked)",
            )
        else:
            self.report({"INFO"}, f"RISE: Baked {count} complex material(s)")
        return {"FINISHED"}


class RISE_OT_clear_baked_materials(bpy.types.Operator):
    """Clear the rise_baked_* ID properties on all materials so the
    next RISE render falls back to direct procedural translation (or
    forces a re-bake of materials with `only_unbaked=False`).

    Useful when:
      - The user edits a Cycles material and wants to drop the stale
        bake without re-running the bake immediately
      - Investigating differences between the bake path and the
        direct-translation path on a simple material
    """

    bl_idname = "rise.clear_baked_materials"
    bl_label = "Clear RISE Bake Cache"
    bl_description = "Remove rise_baked_* ID properties from all materials"
    bl_options = {"REGISTER"}

    def execute(self, context):
        count = 0
        for mat in bpy.data.materials:
            if material_bake.baked_paths(mat) is not None or material_bake.BAKED_DIFFUSE_KEY in mat:
                material_bake.clear_baked_metadata(mat)
                count += 1
        self.report({"INFO"}, f"RISE: Cleared bake cache on {count} material(s)")
        return {"FINISHED"}


class RISE_RENDER_PT_materials(_RISEPanel):
    """Render-properties panel: the Bake button + cache diagnostics.

    Workflow (see docs/BLENDER_MATERIAL_TRANSLATION.md "Two paths"):

      1. User authors a Cycles material with a complex node graph
         (Mix Shader, AO, procedural noise / Voronoi / ColorRamp …).
      2. User clicks **Bake Procedural Materials** once.  RISE swaps
         to Cycles, bakes Diffuse / Roughness / Normal to PNGs under
         ``<tmp>/rise_baked/<material>_*.png``, stores the paths as
         ID properties, and swaps back.
      3. User clicks Render.  RISE consumes the cached PNGs.
      4. When a node graph is edited, the cache's content-hash flips
         to "stale" — the next Render will abort with a message
         asking the user to re-bake.

    This panel is the place users come to (a) trigger the bake and
    (b) confirm what's in the cache.
    """

    bl_label = "RISE Material Baking"

    def draw(self, context):
        layout = self.layout

        # `needs_bake_attempt` is the single source of truth that
        # `engine.render()`'s gate also uses — panel CTA and render
        # gate stay aligned.
        scene = context.scene
        needs_bake = [
            mat.name
            for mat in bpy.data.materials
            if material_bake.needs_bake_attempt(scene, mat)
        ]
        already_baked: list[tuple[str, str, bool]] = []
        for mat in bpy.data.materials:
            paths = material_bake.baked_paths(mat)
            if paths is not None:
                channels = ", ".join(sorted(paths.keys()))
                stale = material_bake.baked_cache_is_stale(mat)
                already_baked.append((mat.name, channels, stale))

        if needs_bake:
            box = layout.box()
            names = ", ".join(needs_bake[:3])
            if len(needs_bake) > 3:
                names += f", … +{len(needs_bake) - 3} more"
            box.label(
                text=f"{len(needs_bake)} material(s) need baking: {names}",
                icon="ERROR",
            )
            box.label(
                text="Click Bake below before rendering — render will abort otherwise.",
                icon="BLANK1",
            )

        # Primary CTA — make the Bake button big and obvious.
        col = layout.column(align=True)
        col.scale_y = 1.6
        col.operator(
            "rise.bake_materials",
            text="Bake Procedural Materials",
            icon="RENDER_STILL",
        )

        layout.separator()
        layout.label(
            text=(
                "RISE renders simple materials (Image Texture, "
                "Mapping, single Principled BSDF) directly."
            ),
            icon="INFO",
        )
        layout.label(
            text=(
                "Complex graphs (Mix Shader, AO, procedural noise) "
                "are baked once to PNG and reused on every render."
            ),
            icon="BLANK1",
        )
        layout.label(
            text="Re-bake after editing a material's node graph.",
            icon="BLANK1",
        )

        # Diagnostic: cache state — useful for confirming a bake
        # landed and for spotting stale-cache materials before render.
        box = layout.box()
        box.label(text="Bake cache:", icon="MATERIAL")
        if not already_baked:
            box.label(text="  (no baked materials yet)")
        else:
            for name, channels, stale in already_baked:
                stale_tag = " — STALE, re-bake before render" if stale else ""
                box.label(text=f"  {name}: {channels}{stale_tag}")

        layout.operator("rise.clear_baked_materials", icon="X")


class RISE_CAMERA_PT_photographic(bpy.types.Panel):
    """Properties → Camera tab: RISE-specific photographic settings.

    Surfaces ISO (Cycles-incompatible) on the active camera so users
    can drive RISE's EV-compensation pipeline without leaving the
    camera-data panel.  Focal length, sensor size, f-stop, focus
    distance, aperture blades / rotation / ratio, and lens shift are
    all read directly from Blender's stock camera-data fields — the
    existing Cycles UI panels remain the authoring surface for them.
    """

    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "data"
    bl_label = "RISE Photographic"
    bl_options = {"DEFAULT_CLOSED"}
    COMPAT_ENGINES = {RISEBlenderRenderEngine.bl_idname}

    @classmethod
    def poll(cls, context):
        if context.engine not in cls.COMPAT_ENGINES:
            return False
        if context.camera is None:
            return False
        return True

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        camera_data = context.camera
        rise_settings = getattr(camera_data, "rise_camera", None)
        if rise_settings is None:
            layout.label(text="rise_camera property not registered", icon="ERROR")
            return

        layout.label(
            text=(
                "ISO drives RISE's EV-compensation pipeline.  "
                "Other photographic settings are read from the "
                "standard Camera and DOF panels."
            ),
            icon="INFO",
        )
        layout.prop(rise_settings, "iso")

        # Surface a read-only hint about which photographic params
        # are currently wired up (helps users spot when ISO is set
        # but f-stop is at default zero, which disables EV).
        dof = getattr(camera_data, "dof", None)
        info = layout.box()
        info.label(text="Current photographic state:", icon="CAMERA_DATA")
        info.label(text=f"Focal length: {getattr(camera_data, 'lens', 50.0):.2f} mm")
        info.label(text=f"Sensor: {getattr(camera_data, 'sensor_width', 36.0):.1f} × {getattr(camera_data, 'sensor_height', 24.0):.1f} mm  ({getattr(camera_data, 'sensor_fit', 'AUTO')})")
        if dof is not None:
            info.label(text=f"DOF enabled: {bool(getattr(dof, 'use_dof', False))}")
            info.label(text=f"f-stop: {getattr(dof, 'aperture_fstop', 0.0):.2f}")
            info.label(text=f"Focus distance: {getattr(dof, 'focus_distance', 0.0):.3f} m")
        else:
            info.label(text="DOF: (no dof sub-struct)")


CUSTOM_PANELS = (
    RISE_OT_bake_materials,
    RISE_OT_clear_baked_materials,
    RISE_RENDER_PT_settings,
    RISE_RENDER_PT_path_tracing,
    RISE_RENDER_PT_adaptive,
    RISE_RENDER_PT_guiding,
    RISE_RENDER_PT_stability,
    RISE_RENDER_PT_output,
    RISE_RENDER_PT_bridge,
    RISE_RENDER_PT_materials,
    RISE_CAMERA_PT_photographic,
)


def _builtin_panels():
    exclude_panels = {
        "VIEWLAYER_PT_filter",
        "VIEWLAYER_PT_layer_passes",
    }

    panels = []
    for panel in bpy.types.Panel.__subclasses__():
        compat_engines = getattr(panel, "COMPAT_ENGINES", None)
        if compat_engines and "BLENDER_RENDER" in compat_engines and panel.__name__ not in exclude_panels:
            panels.append(panel)
    return panels


def register():
    for panel in CUSTOM_PANELS:
        bpy.utils.register_class(panel)

    for panel in _builtin_panels():
        panel.COMPAT_ENGINES.add(RISEBlenderRenderEngine.bl_idname)


def unregister():
    for panel in _builtin_panels():
        panel.COMPAT_ENGINES.discard(RISEBlenderRenderEngine.bl_idname)

    for panel in reversed(CUSTOM_PANELS):
        bpy.utils.unregister_class(panel)
