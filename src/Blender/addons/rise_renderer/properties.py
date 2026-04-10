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
        description="Base samples per pixel sent to the RISE pixel rasterizer",
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
        max=256,
    )
    use_path_tracing: bpy.props.BoolProperty(
        name="Path Tracing",
        description="Use the RISE path tracing shader op for indirect lighting",
        default=True,
    )
    use_world_ambient: bpy.props.BoolProperty(
        name="Approximate World",
        description="Approximate the Blender world color as a simple ambient light",
        default=False,
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

    path_branch: bpy.props.BoolProperty(
        name="Branch Paths",
        description="Enable path tracer branching when scattering",
        default=True,
    )
    sms_enabled: bpy.props.BoolProperty(
        name="Specular Manifold Sampling",
        description="Enable RISE specular manifold sampling in the path tracing shader op",
        default=False,
    )
    sms_max_iterations: bpy.props.IntProperty(
        name="SMS Iterations",
        description="Maximum Newton iterations for specular manifold sampling",
        default=20,
        min=1,
        max=256,
    )
    sms_threshold: bpy.props.FloatProperty(
        name="SMS Threshold",
        description="Convergence threshold for specular manifold sampling",
        default=0.00001,
        min=0.0,
        max=1.0,
        precision=6,
    )
    sms_max_chain_depth: bpy.props.IntProperty(
        name="SMS Chain Depth",
        description="Maximum specular chain depth for specular manifold sampling",
        default=10,
        min=1,
        max=256,
    )
    sms_biased: bpy.props.BoolProperty(
        name="Biased SMS",
        description="Use the faster biased SMS mode",
        default=True,
    )

    adaptive_max_samples: bpy.props.IntProperty(
        name="Max Samples",
        description="Maximum samples per pixel for adaptive sampling, or 0 to disable it",
        default=0,
        min=0,
        max=65535,
    )
    adaptive_threshold: bpy.props.FloatProperty(
        name="Adaptive Threshold",
        description="Relative error threshold for adaptive sampling convergence",
        default=0.01,
        min=0.000001,
        max=1.0,
        precision=6,
    )
    adaptive_show_map: bpy.props.BoolProperty(
        name="Show Sample Map",
        description="Render the adaptive sampling heat map instead of the final color",
        default=False,
    )

    path_guiding_enabled: bpy.props.BoolProperty(
        name="Path Guiding",
        description="Enable OpenPGL-based path guiding when supported by the bridge build",
        default=False,
    )
    path_guiding_training_iterations: bpy.props.IntProperty(
        name="Training Iterations",
        description="Number of training passes before the final guided render",
        default=4,
        min=1,
        max=128,
    )
    path_guiding_training_spp: bpy.props.IntProperty(
        name="Training SPP",
        description="Samples per pixel during each guiding training pass",
        default=4,
        min=1,
        max=1024,
    )
    path_guiding_alpha: bpy.props.FloatProperty(
        name="Guiding Alpha",
        description="Blend probability for sampling from the learned guiding distribution",
        default=0.5,
        min=0.0,
        max=1.0,
        precision=3,
    )
    path_guiding_max_depth: bpy.props.IntProperty(
        name="Guiding Depth",
        description="Maximum eye-path bounce depth that uses guided sampling",
        default=3,
        min=0,
        max=256,
    )
    path_guiding_sampling_type: bpy.props.EnumProperty(
        name="Sampling",
        description="Directional sampling strategy used while path guiding is active",
        items=(
            ("0", "One-Sample MIS", "Blend the guide and BSDF with one-sample MIS"),
            ("1", "RIS", "Use resampled importance sampling between BSDF and guide"),
        ),
        default="0",
    )
    path_guiding_ris_candidates: bpy.props.IntProperty(
        name="RIS Candidates",
        description="Candidate count for guiding RIS mode",
        default=2,
        min=2,
        max=64,
    )

    stability_direct_clamp: bpy.props.FloatProperty(
        name="Direct Clamp",
        description="Maximum luminance for direct lighting contributions, or 0 to disable clamping",
        default=0.0,
        min=0.0,
        max=1000000.0,
        precision=3,
    )
    stability_indirect_clamp: bpy.props.FloatProperty(
        name="Indirect Clamp",
        description="Maximum luminance for indirect lighting contributions, or 0 to disable clamping",
        default=0.0,
        min=0.0,
        max=1000000.0,
        precision=3,
    )
    stability_filter_glossy: bpy.props.FloatProperty(
        name="Filter Glossy",
        description="Per-bounce roughness increase for glossy BSDFs, or 0 to disable it",
        default=0.0,
        min=0.0,
        max=10.0,
        precision=3,
    )
    stability_rr_min_depth: bpy.props.IntProperty(
        name="RR Min Depth",
        description="Minimum path depth before Russian roulette activates",
        default=3,
        min=0,
        max=256,
    )
    stability_rr_threshold: bpy.props.FloatProperty(
        name="RR Threshold",
        description="Throughput floor used by Russian roulette",
        default=0.05,
        min=0.0,
        max=1.0,
        precision=3,
    )
    stability_max_diffuse_bounce: bpy.props.IntProperty(
        name="Diffuse Bounces",
        description="Maximum diffuse bounces, or 0 for unlimited",
        default=0,
        min=0,
        max=256,
    )
    stability_max_glossy_bounce: bpy.props.IntProperty(
        name="Glossy Bounces",
        description="Maximum glossy or reflection bounces, or 0 for unlimited",
        default=0,
        min=0,
        max=256,
    )
    stability_max_transmission_bounce: bpy.props.IntProperty(
        name="Transmission Bounces",
        description="Maximum transmission or refraction bounces, or 0 for unlimited",
        default=0,
        min=0,
        max=256,
    )
    stability_max_translucent_bounce: bpy.props.IntProperty(
        name="Translucent Bounces",
        description="Maximum translucent bounces, or 0 for unlimited",
        default=0,
        min=0,
        max=256,
    )
    stability_max_volume_bounce: bpy.props.IntProperty(
        name="Volume Bounces",
        description="Maximum volume scattering bounces, or 0 for unlimited",
        default=64,
        min=0,
        max=256,
    )

    oidn_denoise: bpy.props.BoolProperty(
        name="OIDN Denoise",
        description="Run Intel Open Image Denoise after rendering when supported by the bridge build",
        default=False,
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
