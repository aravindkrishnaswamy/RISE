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
        layout.prop(settings, "pixel_samples")
        layout.prop(settings, "light_samples")
        layout.prop(settings, "max_recursion")
        layout.prop(settings, "min_importance")
        layout.prop(settings, "use_path_tracing")
        layout.prop(settings, "use_world_ambient")
        layout.prop(settings, "choose_one_light")
        layout.prop(settings, "use_ior_stack")
        layout.prop(settings, "show_lights")


class RISE_RENDER_PT_path_tracing(_RISEPanel):
    bl_label = "Path Tracing"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        settings = context.scene.rise
        layout.enabled = settings.use_path_tracing
        layout.prop(settings, "path_branch")
        layout.prop(settings, "sms_enabled")

        sms_col = layout.column()
        sms_col.enabled = settings.sms_enabled
        sms_col.prop(settings, "sms_max_iterations")
        sms_col.prop(settings, "sms_threshold")
        sms_col.prop(settings, "sms_max_chain_depth")
        sms_col.prop(settings, "sms_biased")


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
        guiding_col.prop(settings, "path_guiding_alpha")
        guiding_col.prop(settings, "path_guiding_max_depth")
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

        if capabilities is not None and not capabilities.supports_oidn:
            layout.label(text="This bridge build does not include OIDN.", icon="INFO")


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


CUSTOM_PANELS = (
    RISE_RENDER_PT_settings,
    RISE_RENDER_PT_path_tracing,
    RISE_RENDER_PT_adaptive,
    RISE_RENDER_PT_guiding,
    RISE_RENDER_PT_stability,
    RISE_RENDER_PT_output,
    RISE_RENDER_PT_bridge,
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
