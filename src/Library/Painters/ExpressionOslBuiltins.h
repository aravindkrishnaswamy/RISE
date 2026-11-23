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
//  Voronoi feature or metric, Mix blend mode or clamp flag), and grows
//  by whole groups (safe math next).  So it lives here: its own signature
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
//  group 3: Voronoi; group 4: colour conversions and Mix Color).
//
//    The texture builtins take a vec3 `p` and a scalar `w` and read the
//    coordinates the dimension count names:
//        dims 1 -> (w)      dims 2 -> (p.x, p.y)
//        dims 3 -> p        dims 4 -> (p.x, p.y, p.z, w)
//    The unused one is ignored (pass 0).  `dims`, `type`, `feature`,
//    `metric`, `normalize`, `mode`, `clamp_factor` and `clamp_result`
//    must be integer LITERALS: they are node properties, and a computed
//    value selecting a code path is almost certainly a mistake.
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
			//! zeros.  Stateless and allocation-free: safe from every
			//! render thread at once.
			void Call( int id, const Scalar* a, Scalar* out );
		}
	}
}

#endif
