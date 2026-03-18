import bpy


class RISEAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    bridge_library_path: bpy.props.StringProperty(
        name="Bridge Library",
        description="Path to the native RISE bridge library built from src/Blender/native",
        subtype="FILE_PATH",
        default="",
    )

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(self, "bridge_library_path")


class RISERenderSettings(bpy.types.PropertyGroup):
    pixel_samples: bpy.props.IntProperty(
        name="Pixel Samples",
        description="Samples per pixel sent to the RISE pixel rasterizer",
        default=8,
        min=1,
        max=4096,
    )
    light_samples: bpy.props.IntProperty(
        name="Light Samples",
        description="Samples per light evaluation",
        default=1,
        min=1,
        max=1024,
    )
    max_recursion: bpy.props.IntProperty(
        name="Max Recursion",
        description="Maximum recursive ray depth",
        default=6,
        min=1,
        max=64,
    )
    min_importance: bpy.props.FloatProperty(
        name="Min Importance",
        description="Stop tracing paths below this importance threshold",
        default=0.01,
        min=0.000001,
        max=1.0,
        precision=6,
    )
    use_path_tracing: bpy.props.BoolProperty(
        name="Path Tracing",
        description="Add the RISE path tracing shader op for indirect lighting",
        default=True,
    )
    use_world_ambient: bpy.props.BoolProperty(
        name="Approximate World",
        description="Approximate the Blender world color as a simple ambient light",
        default=True,
    )
    choose_one_light: bpy.props.BoolProperty(
        name="Choose One Light",
        description="Use RISE's single-random-light luminaire sampling mode",
        default=False,
    )
    use_ior_stack: bpy.props.BoolProperty(
        name="IOR Stack",
        description="Enable RISE's index-of-refraction stack",
        default=True,
    )
    show_lights: bpy.props.BoolProperty(
        name="Visible Lights",
        description="Allow light sources to be visible to the camera",
        default=True,
    )


CLASSES = (
    RISEAddonPreferences,
    RISERenderSettings,
)


def addon_preferences(context=None):
    context = context or bpy.context
    addon = context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)

    bpy.types.Scene.rise = bpy.props.PointerProperty(
        name="RISE",
        description="RISE renderer settings",
        type=RISERenderSettings,
    )


def unregister():
    del bpy.types.Scene.rise

    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)

