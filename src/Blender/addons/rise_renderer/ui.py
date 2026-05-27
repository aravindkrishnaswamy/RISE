import bpy

from . import bridge
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
    RISE_RENDER_PT_settings,
    RISE_RENDER_PT_path_tracing,
    RISE_RENDER_PT_adaptive,
    RISE_RENDER_PT_guiding,
    RISE_RENDER_PT_stability,
    RISE_RENDER_PT_output,
    RISE_RENDER_PT_bridge,
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
