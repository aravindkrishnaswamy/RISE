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
        description="Use the RISE path tracing shader op for indirect lighting (back-compat — use `Rasterizer` enum for v5+)",
        default=True,
    )
    rasterizer_type: bpy.props.EnumProperty(
        name="Rasterizer",
        description=(
            "Which RISE integrator to use.  Pel = RGB radiance, "
            "Spectral = wavelength-resolved (slower but correct for "
            "dispersion / wavelength-dependent IOR).  PT is the most "
            "robust general-purpose default.  BDPT improves indirect "
            "lighting through small openings.  VCM adds photon "
            "merging on top of BDPT — handles caustics and through-"
            "glass cases (water-with-pebbles, glass spheres) that "
            "PT/BDPT can't connect.  MLT mutates entire paths via "
            "Markov chains and excels with very hard-to-find light "
            "sources / caustics, at the cost of correlated noise."
        ),
        items=(
            ("1", "Path Tracing (Pel)",      "Pure path tracing, RGB radiance — default"),
            ("2", "Path Tracing (Spectral)", "Pure path tracing, spectral (HWSS)"),
            ("3", "BDPT (Pel)",              "Bidirectional path tracing, RGB"),
            ("4", "BDPT (Spectral)",         "Bidirectional path tracing, spectral"),
            ("5", "VCM (Pel)",               "Vertex Connection & Merging, RGB — recovers caustics"),
            ("6", "VCM (Spectral)",          "Vertex Connection & Merging, spectral"),
            ("7", "MLT (Pel)",               "Metropolis Light Transport / PSSMLT, RGB"),
            ("8", "MLT (Spectral)",          "Metropolis Light Transport / PSSMLT, spectral"),
            ("0", "Pixel Rasterizer",        "Legacy shader-op stack, no PT — fastest preview"),
        ),
        default="1",
    )

    # Bidirectional / MLT subpath depth.  Default 0 = "fall back to
    # `max_recursion`" — exposed as separate knobs in case the user
    # wants to disable light-subpath connections (set max_light=0).
    bidir_max_eye_depth: bpy.props.IntProperty(
        name="Bidir Max Eye Depth",
        description="Maximum eye-subpath length for BDPT / VCM / MLT (0 = fall back to Max Recursion)",
        default=0, min=0, max=64,
    )
    bidir_max_light_depth: bpy.props.IntProperty(
        name="Bidir Max Light Depth",
        description="Maximum light-subpath length for BDPT / VCM / MLT (0 = fall back to Max Recursion)",
        default=0, min=0, max=64,
    )

    # VCM-specific knobs.
    vcm_merge_radius: bpy.props.FloatProperty(
        name="VCM Merge Radius",
        description="Photon merge radius in world units (0 = auto from scene bounds)",
        default=0.0, min=0.0, max=10.0,
    )
    vcm_enable_vc: bpy.props.BoolProperty(
        name="VCM Vertex Connection",
        description="Enable BDPT-style vertex connection strategies",
        default=True,
    )
    vcm_enable_vm: bpy.props.BoolProperty(
        name="VCM Vertex Merging",
        description="Enable photon-map merging strategies (caustic recovery)",
        default=True,
    )

    # MLT / PSSMLT knobs.  Defaults of 0 are sentinels that the
    # bridge rewrites to production-tuned values (10000 bootstrap,
    # 8 chains, mutations = pixel_samples, large step 0.3).
    mlt_bootstrap: bpy.props.IntProperty(
        name="MLT Bootstrap",
        description="MLT bootstrap sample count (0 = bridge default 10000)",
        default=0, min=0, max=1000000,
    )
    mlt_chains: bpy.props.IntProperty(
        name="MLT Chains",
        description="MLT Markov-chain count (0 = bridge default 8)",
        default=0, min=0, max=256,
    )
    mlt_mutations_per_pixel: bpy.props.IntProperty(
        name="MLT Mutations / Pixel",
        description="MLT mutations per pixel (0 = use Pixel Samples)",
        default=0, min=0, max=65536,
    )
    mlt_large_step_prob: bpy.props.FloatProperty(
        name="MLT Large-Step Prob",
        description="MLT large-step probability (0 = bridge default 0.3)",
        default=0.0, min=0.0, max=1.0,
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
    show_lights: bpy.props.BoolProperty(
        name="Visible Lights",
        description="Allow light sources to be visible to the camera",
        default=True,
    )

    skip_collections_pattern: bpy.props.StringProperty(
        name="Skip Collections",
        description=(
            "Comma-separated list of collection names whose objects RISE "
            "should NOT export.  Useful for excluding low-poly proxy / "
            "AO-baking shells that obscure the camera in RISE but are "
            "filtered out by Cycles' adaptive-sampling heuristics.  "
            "Example: 'blocks, proxies' would skip every object whose "
            "containing collection is named 'blocks' or 'proxies'.  "
            "Empty (default) = no skipping."
        ),
        default="",
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
    sms_bernoulli_trials: bpy.props.IntProperty(
        name="Bernoulli Trials",
        description="Maximum trials per shading point in unbiased SMS mode (ignored when Biased is on)",
        default=100,
        min=1,
        max=10000,
    )
    sms_multi_trials: bpy.props.IntProperty(
        name="Multi Trials",
        description="Independent Newton solves per evaluation; >1 uncovers separate basins on bumpy surfaces at proportional cost",
        default=1,
        min=1,
        max=64,
    )
    sms_photon_count: bpy.props.IntProperty(
        name="Photon Count",
        description="Photon-aided seed budget; 0 disables. Use for diacaustic / mirror-chain scenes the deterministic seed misses",
        default=0,
        min=0,
        max=10000000,
    )
    sms_max_photon_seeds: bpy.props.IntProperty(
        name="Max Photon Seeds",
        description="Cap on photon seeds run through Newton at each shading point; 0 = unlimited",
        default=16,
        min=0,
        max=1024,
    )
    sms_two_stage: bpy.props.BoolProperty(
        name="Two-Stage Solver",
        description="Two-stage Newton solver (Zeltner 2020 §5). Helps on smooth analytic primitives with normal-map perturbation; regresses on heavily-displaced meshes",
        default=False,
    )
    sms_use_levenberg_marquardt: bpy.props.BoolProperty(
        name="Levenberg-Marquardt",
        description="Damp the Newton Jacobian to recover ~5pp Newton-fail rate on heavy-displacement meshes at ~50-100% solver cost",
        default=False,
    )
    sms_seeding_mode: bpy.props.EnumProperty(
        name="Seeding",
        description="SMS seeding strategy. Snell suits displaced refractive caustics; Uniform suits smooth analytic primitives",
        items=(
            ("0", "Snell", "Deterministic Snell-trace from shading point toward light"),
            ("1", "Uniform", "Mitsuba-faithful uniform-area sample on cached caustic-caster shapes"),
        ),
        default="0",
    )
    sms_target_bounces: bpy.props.IntProperty(
        name="Target Bounces",
        description="Required specular-vertex count per seed chain (Mitsuba m_config.bounces analogue). 0 = no target. Set to natural caustic K (typically 2 for glass shells)",
        default=0,
        min=0,
        max=16,
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
    path_guiding_combine_training: bpy.props.BoolProperty(
        name="Combine Training",
        description="Accumulate every training iteration's pixels into the final image weighted by SPP (Müller 2017 §5)",
        default=True,
    )
    path_guiding_online: bpy.props.BoolProperty(
        name="Online Guiding",
        description="The training loop IS the entire render — every sample feeds both the field and the image. Best for low-SPP regimes",
        default=False,
    )
    path_guiding_warmup_iterations: bpy.props.IntProperty(
        name="Warmup Iterations",
        description="Training iterations rendered with alpha=0 (pure BSDF) before switching to the configured alpha",
        default=1,
        min=0,
        max=64,
    )
    path_guiding_alpha: bpy.props.FloatProperty(
        name="Guiding Alpha",
        description="Blend probability for sampling from the learned guiding distribution",
        default=0.5,
        min=0.0,
        max=1.0,
        precision=3,
    )
    path_guiding_learned_alpha: bpy.props.BoolProperty(
        name="Learned Alpha",
        description="Per-cell Adam-learned α (Müller 2017 v2 / Tom94 practical-path-guiding). ~2% mean-σ² reduction at 256 SPP",
        default=True,
    )
    path_guiding_max_depth: bpy.props.IntProperty(
        name="Guiding Depth",
        description="Maximum eye-path bounce depth that uses guided sampling",
        default=8,
        min=0,
        max=256,
    )
    path_guiding_max_light_depth: bpy.props.IntProperty(
        name="Light Guiding Depth",
        description="Maximum light-subpath bounce depth for guided sampling (0 = disabled). Off by default",
        default=0,
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
    stability_use_light_bvh: bpy.props.BoolProperty(
        name="Light BVH",
        description="Use the light BVH for importance-weighted many-light selection",
        default=True,
    )
    stability_optimal_mis: bpy.props.BoolProperty(
        name="Optimal MIS",
        description="Variance-minimising MIS weights (Kondapaneni 2019) for direct lighting; trains second-moment statistics over a few iterations",
        default=False,
    )
    stability_optimal_mis_training_iterations: bpy.props.IntProperty(
        name="Optimal MIS Training",
        description="Training iterations for optimal MIS",
        default=4,
        min=1,
        max=64,
    )
    stability_optimal_mis_tile_size: bpy.props.IntProperty(
        name="Optimal MIS Tile Size",
        description="Spatial-binning tile size for optimal MIS",
        default=16,
        min=1,
        max=256,
    )

    oidn_denoise: bpy.props.BoolProperty(
        name="OIDN Denoise",
        description="Run Intel Open Image Denoise after rendering when supported by the bridge build",
        default=True,
    )
    oidn_quality: bpy.props.EnumProperty(
        name="OIDN Quality",
        description="OIDN quality preset. Auto picks Fast/Balanced/High based on a render-time heuristic",
        items=(
            ("0", "Auto", "Pick a preset based on render time per megapixel"),
            ("1", "High", "Highest quality, slowest"),
            ("2", "Balanced", "Balanced quality and speed"),
            ("3", "Fast", "Fastest, lower quality"),
        ),
        default="0",
    )
    oidn_device: bpy.props.EnumProperty(
        name="OIDN Device",
        description="OIDN device backend",
        items=(
            ("0", "Auto", "Prefer GPU; silently fall back to CPU"),
            ("1", "CPU", "Force CPU"),
            ("2", "GPU", "Prefer GPU; warn on fallback to CPU"),
        ),
        default="0",
    )
    oidn_prefilter: bpy.props.EnumProperty(
        name="OIDN Aux Source",
        description="Albedo / normal source mode. Accurate preserves caustic / reflection detail at ~2× warm-cache cost",
        items=(
            ("0", "Fast", "First-hit retrace pass (cheap, white at perfect glass / mirror)"),
            ("1", "Accurate", "Inline first-non-delta accumulation + prefilter"),
        ),
        default="1",
    )

    progressive_enabled: bpy.props.BoolProperty(
        name="Progressive Rendering",
        description="Render in multiple progressive passes so the image refines visibly while it accumulates samples",
        default=True,
    )
    progressive_samples_per_pass: bpy.props.IntProperty(
        name="Samples Per Pass",
        description="Samples per pixel each progressive pass adds before refreshing the image",
        default=32,
        min=1,
        max=4096,
    )

    use_zsobol: bpy.props.BoolProperty(
        name="Z-Sobol Sampler",
        description="Use Morton-indexed Sobol for blue-noise error distribution",
        default=True,
    )

    pixel_filter: bpy.props.EnumProperty(
        name="Pixel Filter",
        description="Pixel reconstruction filter (anti-aliasing kernel). Cycles defaults to Blackman-Harris ~1.5 px; we default to Gaussian 1.5 px which is close in look and supported across all RISE integrators",
        items=(
            ("0", "None", "No reconstruction — fastest, but aliased"),
            ("1", "Box", "Constant kernel; fastest filtered option"),
            ("2", "Tent", "Linear / triangle falloff"),
            ("3", "Gaussian", "Gaussian falloff (default, closest match to Cycles)"),
            ("4", "Mitchell-Netravali", "Mitchell B=C=1/3; sharper, slight ringing"),
            ("5", "Catmull-Rom", "Sharpening cubic"),
            ("6", "Cubic B-Spline", "Soft cubic"),
            ("7", "Blackman", "Windowed-sinc, very sharp, more ringing"),
        ),
        default="3",
    )
    pixel_filter_width: bpy.props.FloatProperty(
        name="Filter Width",
        description="Filter kernel width in pixels (for Box/Tent/Gaussian/Blackman). Cycles default is 1.5",
        default=1.5,
        min=0.5,
        max=8.0,
        precision=2,
    )
    pixel_filter_param_a: bpy.props.FloatProperty(
        name="Filter Param A",
        description="Filter-specific parameter A: Gaussian alpha decay (default 2.0) or Mitchell B (default 1/3)",
        default=0.0,
        min=0.0,
        max=10.0,
        precision=3,
    )
    pixel_filter_param_b: bpy.props.FloatProperty(
        name="Filter Param B",
        description="Filter-specific parameter B: Mitchell C (default 1/3)",
        default=0.0,
        min=0.0,
        max=10.0,
        precision=3,
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
