//////////////////////////////////////////////////////////////////////
//
//  ExpressionOslBuiltins.h - Expression VM `osl_*` builtins exposing
//  Cycles / OSL shading functions via dedicated opcodes (Compiled::kOsl
//  / kOslV3), separate from ExpressionEval.h's 4-arg FP-pinned table.
//
//  Conventions & invariants:
//  - `osl_` denotes Cycles / OSL semantics (distinct from RISE perlin /
//    fbm / worley_*; no pixel-footprint octave fade on noise). Non-finite
//    outputs clamp to 0.
//  - Mode/property arguments (dims, type, feature, metric, normalize,
//    mode, op, clamp*, invert, ao samples/flags) must be integer literals.
//  - Texture coordinate convention: dims 1->(w), 2->(p.x,p.y), 3->p,
//    4->(p.x,p.y,p.z,w). Noise callers pre-multiply p/w by scale; Voronoi
//    callers pass UNSCALED p/w plus scale (Position/W divide back in FP32).
//    `osl_noise_texture_color.x` == `osl_noise_texture` bit-for-bit.
//  - Builtin groups (full signatures in ExpressionOslBuiltins.cpp):
//    1. Noise: osl_snoise, osl_fractal, osl_noise_texture{,_color}
//    2. White noise: osl_white_noise{,_color} (lookup3 bit-pattern hash)
//    3. Voronoi: osl_voronoi_{distance,color,position,w,radius} (OslVoronoi.h)
//    4. Color: osl_{rgb2hsv,hsv2rgb,rgb2hsl,hsl2rgb,mix_color} (OslColor.h)
//    5. Math: osl_math (41 ops), osl_vmath{,_scalar}, osl_clamp, osl_{v,}map_range
//    6. Vec3/rotate: osl_v{min,max,abs,floor,fract,pow,clamp}, osl_rotate,
//       osl_vector_rotate
//    7. Context/misc: osl_{to_object,to_world,to_camera,camera_to_world}_
//       {point,vector,normal}, osl_object_{color,alpha,index,random},
//       osl_layer_weight, osl_fresnel, osl_is_camera_ray, osl_ao (see
//       docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md), osl_backfacing,
//       osl_true_normal, osl_shading_normal, osl_particle_random,
//       osl_gradient, osl_brick.
//  - Precision: strict IEEE FP32 kernels matching Cycles strict builds
//    (see OslNoise.h, OslVoronoi.h, OslColor.h for exact tolerances).
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

			//! Evaluates builtin `id` on popped scalar args `a` (vec3 as 3 scalars),
			//! writing out[0] or out[0..2] (zeros on unknown id). `pHit` is used
			//! only by `needsHitContext` builtins (null -> identity/0 fallbacks).
			//! Stateless and allocation-free (thread-safe).
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
