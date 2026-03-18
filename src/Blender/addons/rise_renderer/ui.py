import bpy

from . import bridge
from .engine import RISEBlenderRenderEngine
from .properties import addon_preferences


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
        if resolved_path:
            layout.label(text=resolved_path, icon="CHECKMARK")
        else:
            layout.label(text=error_message, icon="ERROR")


CUSTOM_PANELS = (
    RISE_RENDER_PT_settings,
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
