//////////////////////////////////////////////////////////////////////
//
//  ExpressionOslBuiltins.h - The expression VM's `osl_` builtins: the
//    shading-language functions Cycles and OSL define, exposed so a
//    translated Blender node graph (and a hand author) can call them.
//
//  WHY A SEPARATE TABLE AND A SEPARATE DISPATCH.  ExpressionEval.h's
//  FnSig table carries at most four arguments, its scalar-returning
//  function-id band is full, and its CallFunc / CallFuncVec3 bodies are pinned for
//  floating-point reproducibility (ExpressionMemo.h names the set).  The
//  `osl_` family needs up to thirteen arguments, compile-time literal
//  arguments (a node's dimension count, fractal type, normalize flag,
//  Voronoi feature or metric, Mix blend mode or clamp flag, Safe Math /
//  Vector Math / Clamp / Map Range / Vector Rotate op/type/clamp/invert
//  flags), and spans all seven groups.  So it lives here: its own signature
//  table, its own id space, and its own out-of-line Call that
//  the VM reaches through two opcodes of its own (Compiled::kOsl /
//  kOslV3).  No existing builtin's case, id or arithmetic moves.
//
//  NAMING.  `osl_` says "the Open Shading Language / Cycles definition"
//  -- these differ from RISE's own perlin / fbm / worley_*, which keep
//  their meanings.  Unlike fbm(), no `osl_` noise fades its octaves by
//  pixel footprint: Cycles does not, and matching Cycles' pattern is the
//  point.  Hand authors who want anti-aliased noise should prefer fbm().
//
//  THE BUILTINS (group 1: gradient and fractal noise; group 2: white noise;
//  group 3: Voronoi; group 4: colour conversions and Mix Color; group 5:
//  Safe Math, Vector Math, Clamp and Map Range; group 6: componentwise
//  vec3 ops and Vector Rotate; group 7: live space transforms and per-object
//  Object Info attributes).
//
//    The texture builtins take a vec3 `p` and a scalar `w` and read the
//    coordinates the dimension count names:
//        dims 1 -> (w)      dims 2 -> (p.x, p.y)
//        dims 3 -> p        dims 4 -> (p.x, p.y, p.z, w)
//    The unused one is ignored (pass 0).  `dims`, `type`, `feature`,
//    `metric`, `normalize`, `mode`, `op`, `clamp`, `clamp_factor`,
//    `clamp_result` and `invert` must be integer LITERALS: they are node
//    properties, and a computed value selecting a code path is almost
//    certainly a mistake.
//
//    osl_snoise(p, w, dims) -> scalar
//        Signed gradient noise, roughly [-1, 1] (Cycles snoise_Nd).
//    osl_fractal(p, w, dims, type, detail, roughness, lacunarity,
//                offset, gain, normalize) -> scalar
//        Cycles' fractal noise (noise_select).  type: 0 multifractal,
//        1 fBM, 2 hybrid multifractal, 3 ridged multifractal,
//        4 heterogeneous terrain (Cycles' NodeNoiseType numbering).
//        detail is clamped to at most 15; a negative detail is passed
//        through as Cycles' noise_select does (one octave plus a partial
//        one for detail in (-1, 0)).  normalize is read by fBM only.
//    osl_noise_texture(p, w, dims, type, detail, roughness, lacunarity,
//                      offset, gain, distortion, normalize) -> scalar
//        The Noise Texture node's Fac output, after its Scale multiply
//        (the caller passes p*scale and w*scale): detail clamped to
//        [0, 15], roughness to >= 0, then the distortion warp.
//    osl_noise_texture_color(...the same arguments...) -> vec3
//        The node's Color output.  Its first channel is Fac, bit for
//        bit; the other two are the same fractal at two fixed offsets.
//        So a node whose Fac and Color are both used lowers to ONE call
//        in a `def`, reading Fac as its `.x` -- three fractal
//        evaluations, not the four that two separate calls would cost
//        (design doc §5.2(b), "several results").
//
//    osl_white_noise(p, w, dims) -> scalar
//    osl_white_noise_color(p, w, dims) -> vec3
//        The White Noise node's Value and Color outputs: lookup3 hashes of
//        the coordinates' 32-bit float bit patterns, in [0, 1].  Exact
//        against Cycles when the float coordinates are, but not continuous:
//        a one-ulp change gives an unrelated value, so a coordinate that is
//        not exact in float (most of them, in a render) matches Cycles in
//        distribution, not per pixel.  Color's first channel is Value.
//
//    osl_voronoi_distance(p, w, dims, feature, metric, scale, detail,
//                         roughness, lacunarity, smoothness, exponent,
//                         randomness, normalize) -> scalar
//    osl_voronoi_color(...the same arguments...) -> vec3
//    osl_voronoi_position(...) -> vec3
//    osl_voronoi_w(...) -> scalar
//    osl_voronoi_radius(...) -> scalar
//        The Voronoi Texture node's Distance, Color, Position, W and
//        Radius outputs (OslVoronoi.h).  feature: 0 F1, 1 F2, 2 Smooth F1,
//        3 Distance to Edge, 4 N-Sphere Radius; metric: 0 Euclidean,
//        1 Manhattan, 2 Chebychev, 3 Minkowski (Cycles' numbering; the
//        metric is read by F1, F2 and Smooth F1 in 2D to 4D only).
//        UNLIKE the noise builtins, the caller passes p and w BEFORE the
//        Scale multiply and the Scale itself: the node divides Position
//        and W back by it, so the kernel applies both in float, as Cycles
//        does.  An output a feature does not produce reads 0, as in Cycles
//        (Distance to Edge gives only Distance, N-Sphere Radius only
//        Radius).  Position is 0 in 1D, where the point is in W; W is 0 in
//        2D and, in 3D, a value Blender hides; these zeros carry Cycles'
//        sign (-0 at a negative Scale; OslVoronoi.h, Result).  Each call
//        runs the whole node, so using several outputs of one node costs one
//        node evaluation per output used; bind each output used more than
//        once to a `def` so it is evaluated once.
//
//    osl_rgb2hsv(c) -> vec3
//    osl_hsv2rgb(c) -> vec3
//    osl_rgb2hsl(c) -> vec3
//    osl_hsl2rgb(c) -> vec3
//        Cycles' RGB/HSV/HSL colour-space conversions (util/color.h,
//        OslColor.h), as used by Separate Color, Combine Color and
//        Hue/Saturation/Value.  Inputs are passed through unclamped, as
//        in Cycles.
//    osl_mix_color(mode, t, a, b, clamp_factor, clamp_result) -> vec3
//        Cycles' Mix Color node (svm_node_mix_color, OslColor.h).
//        `mode` (integer literal, 0..18) selects one of Cycles' 19
//        NodeMix blend modes: 0 Mix/Blend, 1 Add, 2 Multiply, 3 Subtract,
//        4 Screen, 5 Divide, 6 Difference, 7 Darken, 8 Lighten, 9 Overlay,
//        10 Color Dodge, 11 Color Burn, 12 Hue, 13 Saturation, 14 Value,
//        15 Color, 16 Soft Light, 17 Linear Light, 18 Exclusion.
//        `clamp_factor` (0..1) clamps `t` to [0, 1] before mixing;
//        `clamp_result` (0..1) clamps each output channel to [0, 1] after
//        mixing (legacy MixRGB is clamp_factor=1, clamp_result=use_clamp).
//        Not memo-worthy: arithmetic only, no loops or hashes.
//
//    osl_math(op, a, b, c) -> scalar
//        Cycles' 41 ShaderNodeMath operations (`op` literal 0..40,
//        OslMath::MathOp) with Cycles' zero-on-undefined domain guards
//        (safe divide/pow/log/sqrt/inv_sqrt/modulo/floored_modulo/asin/
//        acos/atan2).
//    osl_vmath_scalar(op, a, b) -> scalar
//        Cycles' scalar-output Vector Math operations (`op` literal 7..9:
//        7 Dot Product, 8 Distance, 9 Length).
//    osl_vmath(op, a, b, c, scale) -> vec3
//        Cycles' vector-output Vector Math operations (`op` literal 0..29,
//        OslMath::VectorMathOp; scalar ops 7..9 yield (0, 0, 0)).
//    osl_clamp(type, v, mn, mx) -> scalar
//        Cycles' Clamp node (`type` literal 0=MINMAX, 1=RANGE).
//    osl_map_range(type, clamp, v, fmin, fmax, tmin, tmax, steps) -> scalar
//    osl_vmap_range(type, clamp, v, fmin, fmax, tmin, tmax, steps) -> vec3
//        Cycles' Float and Vector Map Range nodes (`type` literal 0=LINEAR,
//        1=STEPPED, 2=SMOOTHSTEP, 3=SMOOTHERSTEP; `clamp` literal 0..1,
//        ignored for SMOOTHSTEP/SMOOTHERSTEP).
//
//    osl_vmin(a, b) -> vec3
//    osl_vmax(a, b) -> vec3
//    osl_vabs(a) -> vec3
//    osl_vfloor(a) -> vec3
//    osl_vfract(a) -> vec3
//    osl_vpow(a, b) -> vec3
//    osl_vclamp(a, mn, mx) -> vec3
//        Componentwise float3 operations matching Cycles' util/math_float3.h
//        (including Cycles' `(a < b) ? a : b` min/max/clamp NaN rules and
//        safe_powf).
//    osl_rotate(v, axis, angle) -> vec3
//        Rodrigues rotation around `axis` by `angle` radians (returns `v`
//        when `len(axis) == 0`).
//    osl_vector_rotate(type, invert, v, center, axis, angle, euler) -> vec3
//        Cycles' Vector Rotate node (`type` literal 0=AXIS, 1=X, 2=Y, 3=Z,
//        4=EULER_XYZ; `invert` literal 0..1).
//        All Group 5 and Group 6 builtins are arithmetic/trig only (no
//        loops or hashes) and not memo-worthy.
//
//    osl_to_object_point(v) / osl_to_object_vector(v) / osl_to_object_normal(v) -> vec3
//    osl_to_world_point(v)  / osl_to_world_vector(v)  / osl_to_world_normal(v)  -> vec3
//    osl_to_camera_point(v) / osl_to_camera_vector(v) / osl_to_camera_normal(v) -> vec3
//    osl_camera_to_world_point(v) / osl_camera_to_world_vector(v) /
//    osl_camera_to_world_normal(v) -> vec3
//        Live object<->world and world<->camera space transforms (Cycles
//        ShaderNodeVectorTransform and ShaderNodeTexCoord.Normal / .Camera).
//        Object transforms read `pHit->pmxWorldToObject` and
//        `pHit->pmxObjectToWorld` (inverse-transpose + safe-normalize for
//        normals; identity when absent).  Camera transforms read the live
//        rigid camera frame (+X right, +Y up, +Z forward) from
//        `pHit->pCamera->GetCameraFrame(...)` so camera moves never require
//        recompiling expressions.  Require context vars enabled; not
//        memo-worthy on their own, and keyed in ExpressionMemo::SpaceTransformHitKey.
//
//    osl_object_color() -> vec3
//    osl_object_alpha() -> scalar
//    osl_object_index() -> scalar
//    osl_object_random() -> scalar
//        Per-object metadata (Cycles ShaderNodeObjectInfo: Color, Alpha,
//        Object Index, Random).  Reads `pHit->pObjectInfo` (stamped by
//        Object::IntersectRay / CSGObject::IntersectRay from
//        `IObjectPriv::SetObjectInfo`; returns 0 when absent, matching
//        Cycles' OBJECT_NONE).  Location is lowered as
//        `osl_to_world_point(vec3(0, 0, 0))` and Material Index as a
//        per-material compile-time constant.  Require context vars enabled;
//        not memo-worthy on their own, and keyed in
//        ExpressionMemo::SpaceTransformHitKey.
//
//    osl_layer_weight(blend, normal, flags) -> scalar
//    osl_fresnel(ior, normal, has_normal) -> scalar
//    osl_is_camera_ray() -> scalar
//        Cycles ShaderNodeLayerWeight (Fresnel/Facing), ShaderNodeFresnel,
//        and ShaderNodeLightPath.Is Camera Ray.  `osl_layer_weight` /
//        `osl_fresnel` read `pHit->ray.Dir()` and true geometric backfacing
//        (keyed in PainterSampleHitKey + SpaceTransformHitKey via
//        `needsViewRay`); `osl_is_camera_ray` reads `pHit->bIsCameraRay`
//        (keyed in `SpaceTransformHitKey::isCameraRay`).
//
//    osl_ao(distance, samples, normal, flags) -> scalar
//        Ray-traced ambient occlusion (Cycles ShaderNodeAmbientOcclusion,
//        docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md Part III).  `distance` is
//        world-space ray length (<= 0 -> 1.0); `samples` is an integer literal
//        in [1, 128]; `normal` is a world-space vec3 used iff `(flags & 4)`
//        is set; `flags` is an integer literal bitmask in [0, 7]:
//          bit 0 (1) = inside (flip hemisphere normal)
//          bit 1 (2) = only_local (test pSelf only, ignoring casts_shadows)
//          bit 2 (4) = has_custom_normal (use `normal` instead of default N).
//        Memo-worthy, requires hit context, and opens SurfaceSignalDemand so
//        LightSampler probes emitters.
//
//    osl_backfacing() -> scalar
//        1.0 when the ray hit the true geometric back side of the surface
//        (honoring DL-70's `bGeomNormalOrientedToRay` flip on double-sided
//        meshes and returning 0.0 when `!HasTrueGeomSide()`, e.g. hair curves),
//        else 0.0.  Requires hit context; keyed in
//        SpaceTransformHitKey::geomBackfacing.
//    osl_true_normal() -> vec3
//        World-space geometric normal oriented toward the arriving ray
//        (Cycles Geometry.True Normal).  Requires hit context.
//    osl_shading_normal() -> vec3
//        Pre-modifier smooth shading normal N0 (Cycles Geometry.Normal /
//        sd->N, before normal-map / bump perturbation).  Requires hit context;
//        keyed in SpaceTransformHitKey::n0{x,y,z}.
//    osl_particle_random() -> scalar
//        Per-particle-instance random in [0, 1] (`pHit->pObjectInfo->particleRandom`,
//        0 for non-particle RISE objects; Blender bridge populates Cycles'
//        dummy-particle hash when not a particle instance).  Requires hit context.
//    osl_gradient(p, type) -> scalar
//        Cycles Gradient Texture (type literal 0..6 matching Cycles'
//        NodeGradientType enum: 0 Linear, 1 Quadratic, 2 Easing, 3 Diagonal,
//        4 Radial, 5 Quadratic Sphere, 6 Spherical), saturated to [0, 1].
//    osl_brick(p, mortar_size, mortar_smooth, bias, brick_width, row_height,
//              offset_amount, offset_freq, squash_amount, squash_freq) -> vec3
//        Cycles Brick Texture kernel (`svm_brick`): caller passes `co * scale`
//        as `p`; returns `vec3(tint, mortar_fac, 0)`.
//
//  A result that is not finite (a negative roughness can divide by zero
//  in fBM's normalisation, or an out-of-gamut RGB in osl_rgb2hsl can
//  divide by zero, as in Cycles) reads 0.
//
//  PRECISION.  The kernels are strict IEEE single precision, equal to a
//  strict build of Cycles' sources bit for bit on the tested inputs (the
//  Voronoi rows that call powf to 1e-6 off Apple arm64, whose libm the
//  golden table was dumped with; OslVoronoi.h), within
//  the documented clamps and guards (detail at most 15; a non-finite
//  coordinate reads 0, except in White Noise, which hashes it as Cycles
//  does; a Voronoi octave whose coordinate is not finite ends the fractal
//  -- OslVoronoi.h lists the Voronoi departures).  Against Cycles as
//  Blender builds it the noise agrees to float rounding where float
//  rounding stays small.  The high octaves amplify rounding by roughly
//  (roughness * lacunarity)^octave: on outputs Cycles evaluates at shifted
//  coordinates (Color G/B, distortion), high roughness with high detail
//  parts RISE from Cycles by amounts that reach order one at roughness 1
//  and more above it -- and, inferred from the strict-vs-Blender-flags
//  gap, Cycles' builds from each other -- and in a render every output is
//  sensitive to the float coordinate the two renderers start from.
//  OslNoise.h tabulates the measurements and their conditions.
//
//  Voronoi differs in kind.  A rounding difference can change which point
//  is nearest, so on cell boundaries Color and Position jump to another
//  cell's and N-Sphere Radius can jump to another pair's, while the
//  distances stay within float rounding.  Separately, a 1D coordinate of
//  -0 hashes differently in Blender's -fno-signed-zeros build, and on
//  Cycles' SIMD path a 3D or 4D cell id above int32 converts differently;
//  both move distances too.  OslVoronoi.h, PRECISION, has the
//  measurements.
//
//  Colour conversions and Mix Color use only correctly rounded IEEE 754
//  operations (no transcendentals), match Cycles' strict build bit for
//  bit on all 1098 golden rows, and stay within float rounding (<= 5.07e-7
//  relative to max(1, |v|)) of the Blender-flags builds on the table;
//  OslColor.h, PRECISION and COST, has the figures.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_OSL_BUILTINS_
#define EXPRESSION_OSL_BUILTINS_

#include <string>
#include "../Utilities/Math3D/Math3D.h"	// Scalar

namespace RISE
{
	class ICamera;
	class RayIntersectionGeometric;
	struct SurfaceSignalInfo;

	namespace Implementation
	{
		namespace OslBuiltins
		{
			//! How one SYNTACTIC argument is parsed.  A kIntLiteral argument
			//! is pushed on the value stack like any scalar; the compiler
			//! additionally proves it is one integer literal in
			//! [litMin, litMax], so Call can read it as an int.
			enum ArgKind { kScalarArg, kVec3Arg, kIntLiteralArg };

			static const int kMaxArgs = 13;

			struct Sig
			{
				const char*	name;
				int			id;
				int			nArgs;
				ArgKind		kind[ kMaxArgs ];
				int			litMin[ kMaxArgs ];
				int			litMax[ kMaxArgs ];
				bool		retVec3;
				//! Expensive enough that a memo lookup is cheaper than
				//! re-running the body (ExpressionProgram::MemoWorthy).
				bool		memoWorthy;
				//! Reads live hit/transform/camera/object-info state from `pHit`
				//! (Group 7 space transforms, `osl_object_*`, and `osl_ao`).  Gated on
				//! EnableContextVars and keyed in ExpressionMemo::SpaceTransformHitKey.
				bool		needsHitContext;
				//! Reads scene-level surface signals (`pSignals->pScene`/`pSelf`, e.g.
				//! `osl_ao`).  Activates `SurfaceSignalDemand` so `LightSampler` probes
				//! emissive surfaces and marks `ExpressionScalarPainter` position-dependent.
				bool		needsSceneSignals = false;
				//! Reads incoming ray direction (`pHit->ray.Dir()`, e.g. `osl_layer_weight`,
				//! `osl_fresnel`).  Activates `PainterSampleHitKey` keying in L2 memo
				//! so different arrival angles at the same hit point never alias.
				bool		needsViewRay = false;
				//! Part 0 containment bitmask (`PathDependence::Flags`, 0 = physically based).
				unsigned int pathDependence = 0u;
				//! DL-292: true iff builtin derives world-space position (e.g. `osl_to_world_point`).
				bool		readsWorldPosition = false;
			};

			//! The signature for `name`, or null when it is not an `osl_`
			//! builtin.
			const Sig* Find( const std::string& name );

			//! The signature with id `id`, or null.
			const Sig* FindById( int id );

			//! Scalars the call pops: 3 per vec3 argument, 1 otherwise.
			int ScalarArity( const Sig& s );

			//! Evaluate builtin `id` on its popped arguments `a` (in
			//! argument order, a vec3 as three scalars).  `out` has room
			//! for three values; writes out[0], or out[0..2] for a
			//! vec3-returning builtin, and an unknown id writes three
			//! zeros.  `pHit` is read only by `needsHitContext` builtins
			//! (Group 7: null falls back to identity for point/vector
			//! transforms, safe-normalized input for normal transforms,
			//! and 0 for `osl_object_*`).
			//! Stateless and allocation-free: safe from every render thread
			//! at once.
			void Call( int id, const Scalar* a, Scalar* out,
				const RayIntersectionGeometric* pHit = nullptr,
				const SurfaceSignalInfo* pSignals = nullptr,
				const Vector3* pFallbackNormal = nullptr );

			//! Queries the live rigid camera frame (+X right, +Y up, +Z forward,
			//! origin = eye position) without growing the `ICamera` vtable
			//! (dispatches via `dynamic_cast<const CameraCommon*>`).
			bool QueryCameraFrame(
				const ICamera* pCamera,
				Point3& origin,
				Vector3& right,
				Vector3& up,
				Vector3& forward );
		}
	}
}

#endif
