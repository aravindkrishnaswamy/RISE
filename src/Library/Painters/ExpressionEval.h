//////////////////////////////////////////////////////////////////////
//
//  ExpressionEval.h - Self-contained math-expression compiler and
//  stack-machine VM for scene-authored procedural fields.
//
//  Compiles once (recursive-descent -> flat postfix bytecode with
//  compile-time scalar/vec3 typing; no per-eval heap allocation).
//  Eval* methods are const and touch no mutable state (thread-safe).
//
//  Variables:
//    u, v         -- query coordinates (always available)
//    P, Po, N     -- world pos, object pos, shading normal (vec3)
//    fw, fwo      -- world / object filter width (0 = point sample)
//    curv, curvR  -- bbox-normalized / raw signed mean curvature
//                    (>0 convex, <0 concave, 0 flat/absent; geometric N)
//    time         -- frame time (0 unless supplied)
//    + named `params` (scalar constants) and `defs` (let-bindings).
//    P/Po/N/fw/fwo/curv/curvR/time require EnableContextVars(true).
//
//  Types & ops: scalar and vec3 (`vec3(x,y,z)`, `.x/.y/.z`, `+-*/`
//  with scalar broadcast, `mix` polymorphic; comparisons, `%`, `^`,
//  and remaining math/trig builtins are scalar-only).
//
//  Builtins:
//    - Math/vec3: sin cos tan asin acos atan exp log sqrt abs floor
//      ceil frac sign atan2 mod min max pow hypot step clamp
//      smoothstep select mix dot cross length normalize ramp.
//    - Noise (ProceduralNoiseCore.h): perlin, fbm/turbulence/ridged
//      (octaves in [1,10]; auto compile-time affine Jacobian scaling
//      of `fw`/`fwo` for Nyquist octave fade; fallback (1,0), const
//      position (0,0)), worley_f1/f2/f2f1/id, cellhash.
//    - Geometry signals (lazy; neutral fallbacks when absent or r<=0;
//      literal r<=0 is a compile error; see
//      docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md,
//      docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md,
//      docs/CROSS_OBJECT_PROXIMITY_DESIGN.md):
//      occlusion/thickness/convexity (radius = fraction of bbox diag;
//      neutrals 1/1/0), proximity/interior (radius = world length;
//      neutrals 0/0).
//    - Painter & mesh queries: sample, sample_scalar, sample_alpha,
//      sample_uv, sample_uv_alpha, mesh_attr3, mesh_attr_alpha.
//    - OSL/Cycles (`osl_*` via ExpressionOslBuiltins.h, no fw fade).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_EVAL_
#define EXPRESSION_EVAL_

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include <cstdlib>
#include "../Utilities/Math3D/Math3D.h"	// Scalar, Vector3
#include "../Utilities/FiniteMath.h"
#include "../Utilities/ProceduralNoiseCore.h"
#include "../Interfaces/ISurfaceSignalProvider.h"	// occlusion()/thickness()/convexity() dispatch channel
#include "../Interfaces/SurfaceSignalProximity.h"	// proximity()'s and interior()'s bodies -- CallFunc CALLS both, so the definitions must be here
#include "../Utilities/ExpressionMemo.h"	// the two-level per-hit memo (this file supplies its L2 key; see MakeMemoKey)
#include "../Interfaces/IPainter.h"		// DL-25: sample(name)'s dispatch target (also pulls in RayIntersectionGeometric.h)
#include "../Interfaces/IScalarPainter.h"	// DL-25: sample_scalar(name)'s dispatch target -- IScalarPainter, never GetColorNM (no JH uplift)
#include "../Interfaces/ICamera.h"		// Group 7: live camera frame for osl_to_camera_* / osl_camera_to_world_* memo key
#include "../Utilities/Transformable.h"	// Group 7: global/thread transform-pose epochs for SpaceTransformHitKey
#include "ExpressionOslBuiltins.h"	// the `osl_` builtins: their own table, ids and dispatch (Compiled::kOsl / kOslV3)

namespace RISE
{
	namespace Implementation
	{
		//! Per-eval context inputs beyond (u,v); all default to zero/absent.
		struct ExprEvalContext
		{
			Scalar	u, v;
			Vector3	P;
			Vector3	Po;
			Vector3	N;
			Scalar	fw;			//!< World-space pixel footprint (0 = none)
			Scalar	fwo;		//!< Object-space pixel footprint in `Po` frame (0 = none)
			Scalar	time;
			Scalar	curv;		//!< World-bbox-normalized signed mean curvature (>0 convex; geometric N)
			Scalar	curvR;		//!< Raw signed mean curvature in 1/world-length

			//! Lazy geometry-signal channel (threaded as a separate parameter to
			//! RunAny/CallFunc so compiled programs stay stateless and const).
			SurfaceSignalInfo	signals;

			//! Borrowed hit record for `sample*()` / Group 7 space transforms.
			//! Contract: must match this context's u,v,P,Po,N,signals (leave null
			//! if shifted away from the record, falling back to BuildSyntheticRi).
			const RayIntersectionGeometric*	pHit;

			ExprEvalContext() :
				u(0), v(0), P(0,0,0), Po(0,0,0), N(0,0,0), fw(0), fwo(0), time(0), curv(0), curvR(0), pHit(0)
			{}
			ExprEvalContext( const Scalar u_, const Scalar v_ ) :
				u(u_), v(v_), P(0,0,0), Po(0,0,0), N(0,0,0), fw(0), fwo(0), time(0), curv(0), curvR(0), pHit(0)
			{}
		};

		//! Shared ref-counted ownership of attach-time-resolved painters for
		//! `sample*()` so ExpressionProgram copies share one addref/release pair.
		//! Never contains null entries (constructed only by BindPainterRefs).
		class BoundPainterRefs
		{
		public:
			std::vector<IPainter*>       color;
			std::vector<IScalarPainter*> scalar;
			~BoundPainterRefs()
			{
				for( std::size_t i = 0; i < color.size(); ++i )  if( color[i] )  color[i]->release();
				for( std::size_t i = 0; i < scalar.size(); ++i ) if( scalar[i] ) scalar[i]->release();
			}
		};

		//! Compiled program over a shared variable environment (built via Builder).
		class ExpressionProgram
		{
		public:
			enum VType { kScalar = 0, kVec3 = 1 };

			struct Compiled
			{
				enum Op {
					kConst, kVar, kAdd, kSub, kMul, kDiv, kMod, kPow, kNeg,
					kLt, kGt, kLe, kGe, kEq, kNe, kFunc,
					// vec3 ops (scalar operands pre-broadcast via kSplat3)
					kSplat3, kAddV, kSubV, kMulV, kDivV, kNegV,
					kSwizzle,			//!< vec3 -> scalar component select (idx = 0/1/2)
					kFuncV3,			//!< function call returning vec3
					kRamp,				//!< variadic ramp (idx = stop count, fn = per-stop width 1 or 3)
					kOsl,				//!< osl_ builtin returning scalar (fn = OslBuiltins id)
					kOslV3,				//!< osl_ builtin returning vec3
					kMeshAttr3,			//!< mesh_attr3("name") -> vec3 linear RGB
					kMeshAttrAlpha,		//!< mesh_attr_alpha("name") -> scalar alpha
					kSampleAlpha,		//!< sample_alpha(painter) -> scalar alpha
					kSampleUv,			//!< sample_uv(painter, u, v) -> vec3 RGB
					kSampleUvAlpha		//!< sample_uv_alpha(painter, u, v) -> scalar alpha
				};
				//! `val`: literal for kConst, `fw` domain scale for kFunc.
				//! `valo`: `fwo` domain scale for kFunc (0 for non-noise / P-only).
				//! `arity`: scalars popped (used by kFunc/kFuncV3/kRamp/kOsl/kOslV3).
				struct Instr { Op op; Scalar val; Scalar valo; int idx; int fn; int arity; };
				std::vector<Instr> code;
				int writeSlot;	//!< env slot written (def), or -1 (final expr)
				VType type;

				Compiled() : writeSlot(-1), type(kScalar) {}
			};

			//! Compile-time record of a geometry-signal call site (`radiusIsLiteral`
			//! is true only for a bare optionally-signed numeric literal).
			struct SignalRadiusCall
			{
				int    fn;				//!< kFnOcclusion, kFnThickness, kFnConvexity, kFnProximity, or kFnInterior
				bool   radiusIsLiteral;
				Scalar radiusLiteral;	//!< valid only when radiusIsLiteral
			};

			static const int kMaxSlots      = 512;	//!< named vars (scalar=1, vec3=3 slots)
			static const int kStackCap      = 512;	//!< value-stack depth
			static const int kMaxParseDepth = 200;	//!< recursive-descent nesting
			static const int kMaxOctaves    = NoiseCore::kMaxOctaves;
			static const int kMaxRampStops  = 64;

			//! Contiguous noise-family ID band [kFnNoiseFirst, kFnNoiseLast] used by
			//! ComputeMemoWorthiness and pinned to CallFunc's bare `case 42..50:` labels.
			//! kFnCellHash (50) sits just past the band and is excluded from memo gating.
			static const int kFnPerlin      = 42;
			static const int kFnFbm         = 43;
			static const int kFnTurbulence  = 44;
			static const int kFnRidged      = 45;
			static const int kFnWorleyF1    = 46;
			static const int kFnWorleyF2    = 47;
			static const int kFnWorleyF2F1  = 48;
			static const int kFnWorleyId    = 49;
			static const int kFnNoiseFirst  = kFnPerlin;
			static const int kFnNoiseLast   = kFnWorleyId;
			static const int kFnCellHash    = 50;

			static_assert( kFnFbm        == kFnPerlin + 1
			            && kFnTurbulence == kFnPerlin + 2
			            && kFnRidged     == kFnPerlin + 3
			            && kFnWorleyF1   == kFnPerlin + 4
			            && kFnWorleyF2   == kFnPerlin + 5
			            && kFnWorleyF2F1 == kFnPerlin + 6
			            && kFnWorleyId   == kFnPerlin + 7,
				"the eight noise builtins must stay ONE contiguous id run -- "
				"Builder::ComputeMemoWorthiness tests them as a range [kFnNoiseFirst, kFnNoiseLast]" );
			static_assert( kFnNoiseLast - kFnNoiseFirst == 7,
				"the noise band is exactly the eight builtins above -- widen it deliberately, "
				"and only together with ComputeMemoWorthiness's own reasoning" );
			static_assert( kFnCellHash == kFnNoiseLast + 1,
				"cellhash must stay immediately past the noise band: it is the first id the "
				"memo gate deliberately EXCLUDES, and the only thing expressing that exclusion "
				"is kFnNoiseLast's value" );
			static_assert( kFnPerlin     == 42, "CallFunc's `case 42:` is perlin" );
			static_assert( kFnFbm        == 43, "CallFunc's `case 43:` is fbm" );
			static_assert( kFnTurbulence == 44, "CallFunc's `case 44:` is turbulence" );
			static_assert( kFnRidged     == 45, "CallFunc's `case 45:` is ridged" );
			static_assert( kFnWorleyF1   == 46, "CallFunc's `case 46:` is worley_f1" );
			static_assert( kFnWorleyF2   == 47, "CallFunc's `case 47:` is worley_f2" );
			static_assert( kFnWorleyF2F1 == 48, "CallFunc's `case 48:` is worley_f2f1" );
			static_assert( kFnWorleyId   == 49, "CallFunc's `case 49:` is worley_id" );
			static_assert( kFnCellHash   == 50, "CallFunc's `case 50:` is cellhash" );

			//! Self-signal builtins (fractional radius) + internal DynR twins emitted
			//! when radius is not a bare literal (for baked mesh provider gating).
			static const int kFnOcclusion   = 51;
			static const int kFnThickness   = 52;
			static const int kFnConvexity   = 55;
			static const int kFnOcclusionDynR = 53;
			static const int kFnThicknessDynR = 54;
			static const int kFnConvexityDynR = 56;
			//! Cross-object signals (world-length radius; live scan only, NO DynR
			//! twins — ParseCall's DynR remap must guard against remapping them).
			static const int kFnProximity   = 57;
			static_assert( kFnProximity == 57, "CallFunc's `case kFnProximity:` continues the "
				"scalar-returning band past kFnConvexityDynR (56); 58 is interior, 59 free, "
				"60+ are CallFuncVec3's" );
			static const int kFnInterior    = 58;
			static_assert( kFnInterior == 58, "the scalar-returning band is FULL: kFnInterior is 58 "
				"and DL-25's kFnSamplePainterScalar took 59, the last id before CallFuncVec3's 60+ band" );
			//! Zero-arity painter-sampling builtins (`in.idx` indexes BoundPainterRefs).
			//! Scalar band [..59] is FULL; CallFuncVec3 owns 60+.
			static const int kFnSamplePainterScalar = 59;
			static const int kFnSamplePainter = 63;
			//! Reserved context-var slots env[0..kContextSlotCount-1] (append-only;
			//! never renumber existing slots):
			//!   u=0, v=1, P=2..4, Po=5..7, N=8..10, fw=11, time=12,
			//!   curv=13, curvR=14, fwo=15.
			//! When adding a context var, update ALL of: LookupContextVar, Builder(),
			//! Builder::Finalize, ExpressionProgram members/ctor, BindEnv (+ all 4
			//! Eval* callers), ExprEvalContext, RunAny/CallFunc (if implicit), and
			//! MakeMemoKey (omitting from MakeMemoKey causes silent memo collisions).
			static const int kContextSlotP     = 2;
			static const int kContextSlotPo    = 5;
			static const int kContextSlotFw    = 11;
			static const int kContextSlotTime  = 12;
			static const int kContextSlotCurv  = 13;
			static const int kContextSlotCurvR = 14;
			static const int kContextSlotFwo   = 15;
			static const int kContextSlotCount = kContextSlotFwo + 1;
			static const int kContextVarMaskBits = 32;

			//! -ffast-math-safe finiteness test (materializes via volatile in IsFiniteDouble).
			static bool IsFinite( const Scalar x )
			{
				return RISE::IsFiniteDouble( static_cast<double>( x ) );
			}

			Scalar Eval( const Scalar u, const Scalar v ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, u, v, Vector3(0,0,0), Vector3(0,0,0), Vector3(0,0,0), Scalar(0), Scalar(0), Scalar(0), Scalar(0), Scalar(0), 0, m_boundRefs.get(), -1, 0 );
				Scalar out[3];
				RunAny( m_final, env, out, 0, m_boundRefs.get(), 0, &m_meshAttrNames );
				return out[0];
			}

			Scalar Eval( const ExprEvalContext& ctx ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.fwo, ctx.time, ctx.curv, ctx.curvR, &ctx.signals, m_boundRefs.get(), -1, ctx.pHit );
				Scalar out[3];
				RunAny( m_final, env, out, &ctx.signals, m_boundRefs.get(), ctx.pHit, &m_meshAttrNames );
				return out[0];
			}

			//! Evaluates full vec3 (broadcasts scalar result to (s,s,s)).
			Vector3 EvalVec3( const ExprEvalContext& ctx ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.fwo, ctx.time, ctx.curv, ctx.curvR, &ctx.signals, m_boundRefs.get(), -1, ctx.pHit );
				Scalar out[3];
				RunAny( m_final, env, out, &ctx.signals, m_boundRefs.get(), ctx.pHit, &m_meshAttrNames );
				if( m_final.type == kVec3 ) return Vector3( out[0], out[1], out[2] );
				return Vector3( out[0], out[0], out[0] );
			}

			//! Process-unique compile-time identity (0 = invalid).
			unsigned long long ProgramId() const { return m_id; }

			//! Compile-time L2-memo worthiness gate (true if cost exceeds key compare).
			bool MemoWorthy() const { return m_memoWorthy; }

			//! Builds the L2 memo key from EVERY ExprEvalContext field + caller `pipe`.
			//! Memo lookup lives in ExpressionPainter/ExpressionScalarPainter rather
			//! than inside Eval/EvalVec3 so -ffast-math inlining/FMA in RunAny stays
			//! bit-identical.
			void MakeMemoKey( const ExprEvalContext& ctx, const ExpressionMemo::EvalPipe pipe,
				ExpressionMemo::ProgramKey& k ) const
			{
				k.progId = m_id;
				k.pipe = (int)pipe;
				k.u = (double)ctx.u;   k.v = (double)ctx.v;
				k.Px = (double)ctx.P.x;   k.Py = (double)ctx.P.y;   k.Pz = (double)ctx.P.z;
				k.Pox = (double)ctx.Po.x; k.Poy = (double)ctx.Po.y; k.Poz = (double)ctx.Po.z;
				k.Nx = (double)ctx.N.x;   k.Ny = (double)ctx.N.y;   k.Nz = (double)ctx.N.z;
				k.fw = (double)ctx.fw;    k.fwo = (double)ctx.fwo;  k.time = (double)ctx.time;
				k.curv = (double)ctx.curv; k.curvR = (double)ctx.curvR;
				k.signals = ctx.signals.MemoHitKey();
				k.usesPainterSample = UsesPainterSample() || UsesViewRay();
				k.sampleHit = MakeSampleHitKey( k.usesPainterSample ? ctx.pHit : 0 );
				// Also key spaceHit when sampling a child painter (it may call Group 7 space transforms).
				k.usesSpaceTransform = UsesSpaceTransform() || k.usesPainterSample;
				k.spaceHit = MakeSpaceTransformHitKey( k.usesSpaceTransform ? ctx.pHit : 0 );
			}

			//! Space-transform half of the L2 memo key (Group 7).
			static ExpressionMemo::SpaceTransformHitKey MakeSpaceTransformHitKey( const RayIntersectionGeometric* pHit )
			{
				ExpressionMemo::SpaceTransformHitKey k;
				k.globalPoseEpoch = pHit ? Transformable::GlobalTransformPoseEpoch() : 0ull;
				k.threadPoseEpoch = pHit ? Transformable::ThreadTransformPoseEpoch() : 0ull;
				if( !pHit ) {
					k.pWorldToObject = 0;
					k.pObjectToWorld = 0;
					k.hasCamera = false;
					k.geomOpposesArrival = 0;
					k.geomBackfacing = false;
					k.isCameraRay = false;
					k.camOx = k.camOy = k.camOz = 0.0;
					k.camRx = k.camRy = k.camRz = 0.0;
					k.camUx = k.camUy = k.camUz = 0.0;
					k.camFx = k.camFy = k.camFz = 0.0;
					k.gnx = k.gny = k.gnz = 0.0;
					k.n0x = k.n0y = k.n0z = 0.0;
					k.objR = k.objG = k.objB = k.objA = 0.0f;
					k.objIdx = k.objRand = k.particleRand = 0.0f;
					return k;
				}
				k.pWorldToObject = (const void*)pHit->pmxWorldToObject;
				k.pObjectToWorld = (const void*)pHit->pmxObjectToWorld;
				const bool opposesArrival = pHit->GeomNormalOpposesArrival();
				k.geomOpposesArrival = opposesArrival ? -1 : 1;
				k.geomBackfacing = pHit->HasTrueGeomSide()
					&& ( pHit->bGeomNormalOrientedToRay ? opposesArrival : !opposesArrival );
				k.isCameraRay = pHit->bIsCameraRay;
				k.gnx = (double)pHit->vGeomNormal.x;
				k.gny = (double)pHit->vGeomNormal.y;
				k.gnz = (double)pHit->vGeomNormal.z;
				const Vector3 n0 = pHit->bHasNormalMapFrame ? pHit->normalMapOnb.w()
					: ( pHit->bHasCoatDecodeOnb ? pHit->coatDecodeOnb.w() : pHit->vNormal );
				k.n0x = (double)n0.x;
				k.n0y = (double)n0.y;
				k.n0z = (double)n0.z;
				Point3 origin;
				Vector3 right, up, forward;
				if( OslBuiltins::QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
					k.hasCamera = true;
					k.camOx = (double)origin.x;  k.camOy = (double)origin.y;  k.camOz = (double)origin.z;
					k.camRx = (double)right.x;   k.camRy = (double)right.y;   k.camRz = (double)right.z;
					k.camUx = (double)up.x;      k.camUy = (double)up.y;      k.camUz = (double)up.z;
					k.camFx = (double)forward.x; k.camFy = (double)forward.y; k.camFz = (double)forward.z;
				} else {
					k.hasCamera = false;
					k.camOx = k.camOy = k.camOz = 0.0;
					k.camRx = k.camRy = k.camRz = 0.0;
					k.camUx = k.camUy = k.camUz = 0.0;
					k.camFx = k.camFy = k.camFz = 0.0;
				}
				if( pHit->pObjectInfo ) {
					k.objR = pHit->pObjectInfo->color[0];
					k.objG = pHit->pObjectInfo->color[1];
					k.objB = pHit->pObjectInfo->color[2];
					k.objA = pHit->pObjectInfo->alpha;
					k.objIdx = pHit->pObjectInfo->objectIndex;
					k.objRand = pHit->pObjectInfo->random;
					k.particleRand = pHit->pObjectInfo->particleRandom;
				} else {
					k.objR = k.objG = k.objB = k.objA = 0.0f;
					k.objIdx = k.objRand = k.particleRand = 0.0f;
				}
				return k;
			}

			//! The painter-sample half of the key above, split out so the
			//! enumeration lives in one function rather than inline in
			//! MakeMemoKey's body.
			static ExpressionMemo::PainterSampleHitKey MakeSampleHitKey( const RayIntersectionGeometric* pHit )
			{
				ExpressionMemo::PainterSampleHitKey k;
				if( !pHit ) {
					k.hasHit = false;
					k.dudx = k.dudy = k.dvdx = k.dvdy = 0.0;
					k.footprintValid = false;
					k.rdx = k.rdy = k.rdz = 0.0;
					k.u1 = k.v1 = 0.0;
					k.hasTexCoord1 = false;
					k.vcr = k.vcg = k.vcb = 0.0;
					k.hasVertexColor = false;
					return k;
				}
				k.hasHit = true;
				k.dudx = (double)pHit->txFootprint.dudx;
				k.dudy = (double)pHit->txFootprint.dudy;
				k.dvdx = (double)pHit->txFootprint.dvdx;
				k.dvdy = (double)pHit->txFootprint.dvdy;
				k.footprintValid = pHit->txFootprint.valid;
				const Vector3& d = pHit->ray.Dir();
				k.rdx = (double)d.x; k.rdy = (double)d.y; k.rdz = (double)d.z;
				k.u1 = (double)pHit->ptCoord1.x;
				k.v1 = (double)pHit->ptCoord1.y;
				k.hasTexCoord1 = pHit->bHasTexCoord1;
				k.vcr = (double)pHit->vColor[0];
				k.vcg = (double)pHit->vColor[1];
				k.vcb = (double)pHit->vColor[2];
				k.hasVertexColor = pHit->bHasVertexColor;
				return k;
			}

			VType ResultType() const { return m_final.type; }

			//! Compile-time check whether any def or final expr reads context slot `firstSlot` (conservative).
			bool UsesContextVar( int firstSlot ) const
			{
				if( firstSlot < 0 || firstSlot >= kContextVarMaskBits ) return false;
				return ( m_ctxUsedMask & ( 1u << firstSlot ) ) != 0;
			}

			bool UsesSurfaceCurvature() const
			{
				return UsesContextVar( kContextSlotCurv ) || UsesContextVar( kContextSlotCurvR );
			}

			//! True if any def/final calls surface signals, mesh_attr*, or scene-signal OSL builtins.
			bool UsesSurfaceSignals() const { return !m_signalCalls.empty() || !m_meshAttrNames.empty() || m_usesSceneSignals; }
			bool UsesMeshAttributes() const { return !m_meshAttrNames.empty(); }
			const std::vector<std::string>& MeshAttributeNames() const { return m_meshAttrNames; }

			//! True if program calls proximity() or interior() (gates object-manager AABB snapshot).
			bool UsesCrossObject() const
			{
				for( std::size_t i = 0; i < m_signalCalls.size(); ++i ) {
					if( m_signalCalls[i].fn == kFnProximity ) return true;
					if( m_signalCalls[i].fn == kFnInterior )  return true;
				}
				return false;
			}

			const std::vector<SignalRadiusCall>& SurfaceSignalCalls() const { return m_signalCalls; }
			const std::vector<std::string>& ColorPainterRefNames() const { return m_colorPainterRefNames; }
			const std::vector<std::string>& ScalarPainterRefNames() const { return m_scalarPainterRefNames; }
			bool UsesPainterSample() const { return !m_colorPainterRefNames.empty() || !m_scalarPainterRefNames.empty(); }
			bool UsesSpaceTransform() const { return m_usesSpaceTransform; }
			bool UsesViewRay() const { return m_usesViewRay; }

			//! PathDependence::Flags bitmask (docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md).
			unsigned int GetPathDependence() const
			{
				unsigned int mask = m_pathDependence;
				if( m_boundRefs ) {
					for( size_t i = 0; i < m_boundRefs->color.size(); ++i ) {
						if( m_boundRefs->color[i] ) {
							mask |= m_boundRefs->color[i]->GetPathDependence();
						}
					}
					for( size_t i = 0; i < m_boundRefs->scalar.size(); ++i ) {
						if( m_boundRefs->scalar[i] ) {
							mask |= m_boundRefs->scalar[i]->GetPathDependence();
						}
					}
				}
				return mask;
			}

			bool ReadsWorldPosition() const
			{
				if( UsesContextVar( kContextSlotP ) || m_readsWorldPosition ) {
					return true;
				}
				if( m_boundRefs ) {
					for( size_t i = 0; i < m_boundRefs->color.size(); ++i ) {
						if( m_boundRefs->color[i] && m_boundRefs->color[i]->ReadsWorldPosition() ) {
							return true;
						}
					}
					for( size_t i = 0; i < m_boundRefs->scalar.size(); ++i ) {
						if( m_boundRefs->scalar[i] && m_boundRefs->scalar[i]->ReadsWorldPosition() ) {
							return true;
						}
					}
				}
				return false;
			}

			bool IsPainterRefsBound() const { return !UsesPainterSample() || ( m_boundRefs != nullptr ); }

			//! Attach-time resolution of `sample*()` names (index-aligned with
			//! Color/ScalarPainterRefNames, all non-null; addrefs each pointer).
			//! Returns false binding nothing on size mismatch or null entry.
			//! STALE-POINTER WARNING: snapshots attach-time pointers with no rebind
			//! hook; safe only while Cst::DeriveToJobIncremental refuses Painter
			//! chunks (any future incremental Painter apply must add RebindPainterRefs).
			bool BindPainterRefs( const std::vector<IPainter*>& colorPtrs, const std::vector<IScalarPainter*>& scalarPtrs )
			{
				if( colorPtrs.size() != m_colorPainterRefNames.size() ) return false;
				if( scalarPtrs.size() != m_scalarPainterRefNames.size() ) return false;
				for( std::size_t i = 0; i < colorPtrs.size(); ++i )  if( !colorPtrs[i] )  return false;
				for( std::size_t i = 0; i < scalarPtrs.size(); ++i ) if( !scalarPtrs[i] ) return false;
				std::shared_ptr<BoundPainterRefs> refs = std::make_shared<BoundPainterRefs>();
				refs->color.reserve( colorPtrs.size() );
				for( std::size_t i = 0; i < colorPtrs.size(); ++i ) { colorPtrs[i]->addref(); refs->color.push_back( colorPtrs[i] ); }
				refs->scalar.reserve( scalarPtrs.size() );
				for( std::size_t i = 0; i < scalarPtrs.size(); ++i ) { scalarPtrs[i]->addref(); refs->scalar.push_back( scalarPtrs[i] ); }
				m_boundRefs = refs;
				return true;
			}

			int DefCount() const { return (int)m_defs.size(); }

			//! Evaluates defs [0..defIdx] only; scalar result broadcasts to (s,s,s).
			//! Returns false without writing outVal/outType if defIdx is out of range.
			bool EvalDefStage( const ExprEvalContext& ctx, int defIdx, Vector3& outVal, VType& outType ) const
			{
				if( defIdx < 0 || (size_t)defIdx >= m_defs.size() ) return false;
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.fwo, ctx.time, ctx.curv, ctx.curvR, &ctx.signals, m_boundRefs.get(), defIdx, ctx.pHit );
				const Compiled& d = m_defs[ (size_t)defIdx ];
				if( d.type == kVec3 ) {
					outVal = Vector3( env[ d.writeSlot+0 ], env[ d.writeSlot+1 ], env[ d.writeSlot+2 ] );
				} else {
					const Scalar s = env[ d.writeSlot ];
					outVal = Vector3( s, s, s );
				}
				outType = d.type;
				return true;
			}

			bool IsValid() const { return m_valid; }
			const std::string& Error() const { return m_error; }
			ptrdiff_t ErrorOffset() const { return m_errorOffset; }

			class Builder
			{
			public:
				Builder() : m_errorOffset(-1), m_hasBuilderError(false), m_contextVarsEnabled(false),
					m_usesSpaceTransform(false), m_usesSceneSignals(false), m_usesViewRay(false), m_readsWorldPosition(false), m_pathDependence(0u), m_ctxUsed(0)
				{
					// Reserve context slots without registering in m_index so user
					// params/defs (e.g. `param N`) shadow context names for back-compat.
					m_names.assign( kContextSlotCount, std::string() );
				}

				//! Scalar constant; same-type redefinition is last-wins, cross-type is a sticky error.
				bool AddParam( const std::string& name, const Scalar value )
				{
					if( RejectIfDuplicate( name, kScalar ) ) return false;
					const int s = Slot( name, kScalar );
					m_init[s] = value;
					if( m_slotIsDef.find( s ) == m_slotIsDef.end() ) {
						const LinScalar lin = LinScalar::Const( value );
						RecordSlotLinear( s, 1, &lin );
					}
					return true;
				}

				//! Named sub-expression (type inferred; evaluated in registration order).
				bool AddDef( const std::string& name, const std::string& expr )
				{
					Compiled c; VType t;
					if( !Compile( expr, c, t ) ) {
						return false;
					}
					if( RejectIfDuplicate( name, t ) ) return false;
					const int s = Slot( name, t );
					c.writeSlot = s;
					c.type = t;
					{
						const int w = ( t == kVec3 ) ? 3 : 1;
						LinScalar lin[3];
						const bool ok = AnalyzeLinear( c.code, 0, c.code.size(), w, lin );
						RecordSlotLinear( s, w, ok ? lin : 0 );
						for( int k = 0; k < w; ++k ) m_slotIsDef[ s + k ] = true;
					}
					m_defs.push_back( c );
					return true;
				}

				//! Compile final expression and populate `out` (fails if any prior builder error occurred).
				bool Finalize( const std::string& expr, ExpressionProgram& out )
				{
					if( m_hasBuilderError ) {
						out.m_valid = false; out.m_error = m_error; out.m_errorOffset = m_errorOffset;
						return false;
					}
					Compiled c; VType t;
					if( !Compile( expr, c, t ) ) {
						out.m_valid = false;
						out.m_error = m_error;
						out.m_errorOffset = m_errorOffset;
						return false;
					}
					if( (int)m_names.size() > kMaxSlots ) {
						SetError( "too many variables (context + param + def); user-declared names may use "
							"at most " + std::to_string( kMaxSlots - kContextSlotCount ) + " of the " + std::to_string( kMaxSlots ) +
							" total variable slots (" + std::to_string( kContextSlotCount ) + " are reserved for u,v,P,Po,N,fw,fwo,time,curv,curvR)", -1 );
						out.m_valid = false; out.m_error = m_error; out.m_errorOffset = m_errorOffset;
						return false;
					}
					c.writeSlot = -1;
					c.type = t;
					out.m_uSlot = 0; out.m_vSlot = 1;
					out.m_PSlot = kContextSlotP; out.m_PoSlot = kContextSlotPo; out.m_NSlot = 8;
					out.m_fwSlot = kContextSlotFw; out.m_fwoSlot = kContextSlotFwo;
					out.m_timeSlot = kContextSlotTime;
					out.m_curvSlot = kContextSlotCurv; out.m_curvRSlot = kContextSlotCurvR;
					out.m_ctxUsedMask = m_ctxUsed;
					out.m_signalCalls = m_sigCalls;
					out.m_colorPainterRefNames = m_colorPainterRefNames;
					out.m_scalarPainterRefNames = m_scalarPainterRefNames;
					out.m_meshAttrNames = m_meshAttrNames;
					out.m_initEnv.assign( m_names.size(), Scalar(0) );
					for( std::map<int,Scalar>::const_iterator it = m_init.begin(); it != m_init.end(); ++it ) {
						out.m_initEnv[ it->first ] = it->second;
					}
					out.m_defs = m_defs;
					out.m_final = c;
					// Assign id only after all validation passes so invalid programs keep id 0.
					out.m_id = ExpressionMemo::NewProgramId();
					out.m_usesSpaceTransform = m_usesSpaceTransform;
					out.m_usesSceneSignals = m_usesSceneSignals;
					out.m_usesViewRay = m_usesViewRay;
					out.m_readsWorldPosition = m_readsWorldPosition;
					out.m_pathDependence = m_pathDependence;
					out.m_memoWorthy = ComputeMemoWorthiness( out );
					out.m_valid = true;
					return true;
				}

				//! Compile-time L2-memo worthiness: qualifies on painter samples,
				//! geometry signals, memoWorthy `osl_*` calls, noise builtins
				//! [kFnNoiseFirst, kFnNoiseLast], or instruction count >=
				//! ProgramKey::kFieldsNonSampling (NOT kFields, which includes
				//! short-circuited sampling/space-transform fields).
				static bool ComputeMemoWorthiness( const ExpressionProgram& p )
				{
					if( p.UsesPainterSample() ) return true;
					if( !p.m_signalCalls.empty() ) return true;

					std::size_t instrs = p.m_final.code.size();
					for( std::size_t d = 0; d < p.m_defs.size(); ++d ) instrs += p.m_defs[d].code.size();

					for( std::size_t pass = 0; pass <= p.m_defs.size(); ++pass ) {
						const Compiled& body = ( pass == 0 ) ? p.m_final : p.m_defs[ pass - 1 ];
						for( std::size_t i = 0; i < body.code.size(); ++i ) {
							const Compiled::Instr& in = body.code[i];
							if( in.op == Compiled::kOsl || in.op == Compiled::kOslV3 ) {
								const OslBuiltins::Sig* os = OslBuiltins::FindById( in.fn );
								if( os && os->memoWorthy ) return true;
								continue;
							}
							if( in.op != Compiled::kFunc ) continue;
							if( in.fn >= ExpressionProgram::kFnNoiseFirst
							 && in.fn <= ExpressionProgram::kFnNoiseLast ) return true;
						}
					}

					return instrs >= (std::size_t)ExpressionMemo::ProgramKey::kFieldsNonSampling;
				}

				const std::string& Error() const { return m_error; }
				ptrdiff_t ErrorOffset() const { return m_errorOffset; }

				//! Enables 3D surface context vars (P, Po, N, fw, fwo, time, curv, curvR)
				//! and surface/painter query builtins.  OFF by default so document-level
				//! `expr(...)` and `expression_function2d` stay UV-only.
				void EnableContextVars( bool enable = true ) { m_contextVarsEnabled = enable; }

				struct SampleRef { bool isScalar; std::string fnName; std::string painterName; size_t identOff; size_t identLen; };

				//! Token-stream scan for `sample*(<ident>...)` call sites (shared tokenizer
				//! with ParseSampleCall).  Returns false on tokenize error (partial `out` kept).
				static bool ExtractSampleRefs( const std::string& expr, std::vector<SampleRef>& out )
				{
					Builder b;
					std::vector<Tok> toks;
					const bool ok = b.Tokenize( expr, toks );
					for( size_t i = 0; i + 3 < toks.size(); ++i ) {
						if( toks[i].t != Tok::Ident ) continue;
						const std::string& fn = toks[i].s;
						const bool oneArg = ( fn == "sample" || fn == "sample_scalar" || fn == "sample_alpha" );
						const bool uvArg  = ( fn == "sample_uv" || fn == "sample_uv_alpha" );
						if( !oneArg && !uvArg ) continue;
						if( toks[i+1].t != Tok::LP ) continue;
						if( toks[i+2].t != Tok::Ident ) continue;
						if( oneArg && toks[i+3].t != Tok::RP ) continue;
						if( uvArg && toks[i+3].t != Tok::Comma ) continue;
						SampleRef r;
						r.isScalar = ( fn == "sample_scalar" );
						r.fnName = fn;
						r.painterName = toks[i+2].s;
						r.identOff = toks[i+2].off;
						r.identLen = toks[i+2].s.size();
						out.push_back( r );
					}
					return ok;
				}

				//! Idempotent single-pass rename of `sample*(oldName...)` matching `isScalar`.
				static std::string RewriteSampleCallRefs( const std::string& expr, bool isScalar,
					const std::string& oldName, const std::string& newName )
				{
					Builder b;
					std::vector<Tok> toks;
					if( !b.Tokenize( expr, toks ) ) return expr;
					std::string out;
					out.reserve( expr.size() );
					size_t last = 0;
					for( size_t i = 0; i + 3 < toks.size(); ++i ) {
						if( toks[i].t != Tok::Ident ) continue;
						const std::string& fn = toks[i].s;
						const bool thisIsScalar = ( fn == "sample_scalar" );
						const bool thisIsOneColor = ( fn == "sample" || fn == "sample_alpha" );
						const bool thisIsUvColor = ( fn == "sample_uv" || fn == "sample_uv_alpha" );
						if( !thisIsScalar && !thisIsOneColor && !thisIsUvColor ) continue;
						if( thisIsScalar != isScalar ) continue;
						if( toks[i+1].t != Tok::LP ) continue;
						if( toks[i+2].t != Tok::Ident ) continue;
						if( ( thisIsScalar || thisIsOneColor ) && toks[i+3].t != Tok::RP ) continue;
						if( thisIsUvColor && toks[i+3].t != Tok::Comma ) continue;
						if( toks[i+2].s != oldName ) continue;
						out.append( expr, last, toks[i+2].off - last );
						out.append( newName );
						last = toks[i+2].off + toks[i+2].s.size();
					}
					out.append( expr, last, expr.size() - last );
					return out;
				}

			private:
				std::vector<std::string> m_names;	// slot index -> name (vec3 reserves 3 consecutive)
				std::map<std::string,int> m_index;	// name -> first slot
				std::map<int,VType> m_slotType;	// first slot -> type
				std::map<int,Scalar> m_init;		// param initial values (scalar slots only)
				std::vector<Compiled> m_defs;
				std::string m_error;
				ptrdiff_t m_errorOffset;
				bool m_hasBuilderError;	// sticky: once true, Finalize always fails
				bool m_contextVarsEnabled;
				bool m_usesSpaceTransform;
				bool m_usesSceneSignals;
				bool m_usesViewRay;
				bool m_readsWorldPosition;
				unsigned int m_pathDependence;
				unsigned int m_ctxUsed;
				std::vector<ExpressionProgram::SignalRadiusCall> m_sigCalls;
				std::vector<std::string> m_colorPainterRefNames;
				std::map<std::string,int> m_colorPainterRefIndex;
				std::vector<std::string> m_scalarPainterRefNames;
				std::map<std::string,int> m_scalarPainterRefIndex;
				std::vector<std::string> m_meshAttrNames;
				std::map<std::string,int> m_meshAttrIndex;

				static int RegisterPainterRef( std::vector<std::string>& names, std::map<std::string,int>& index, const std::string& nm )
				{
					std::map<std::string,int>::const_iterator it = index.find( nm );
					if( it != index.end() ) return it->second;
					const int idx = (int)names.size();
					names.push_back( nm );
					index[ nm ] = idx;
					return idx;
				}

				void SetError( const std::string& msg, ptrdiff_t offset ) { m_error = msg; m_errorOffset = offset; }

				// Same-type redefinition is allowed ("last wins"); cross-type redefinition
				// is a sticky compile error (prevents env clobber / stack overflow).
				bool RejectIfDuplicate( const std::string& name, VType vt )
				{
					std::map<std::string,int>::const_iterator it = m_index.find( name );
					if( it == m_index.end() ) return false;
					if( m_slotType[ it->second ] == vt ) return false;
					if( !m_hasBuilderError ) {
						SetError( "duplicate name `" + name + "` redeclared with a different type (was " +
							( m_slotType[it->second]==kVec3 ? "vec3" : "scalar" ) + ", now " + ( vt==kVec3 ? "vec3" : "scalar" ) + ")", -1 );
						m_hasBuilderError = true;
					}
					return true;
				}

				static bool LookupContextVar( const std::string& name, int& slot, VType& vt )
				{
					if( name == "u" )    { slot = 0;  vt = kScalar; return true; }
					if( name == "v" )    { slot = 1;  vt = kScalar; return true; }
					if( name == "P" )    { slot = kContextSlotP;  vt = kVec3;   return true; }
					if( name == "Po" )   { slot = kContextSlotPo; vt = kVec3;   return true; }
					if( name == "N" )    { slot = 8;  vt = kVec3;   return true; }
					if( name == "fw" )    { slot = kContextSlotFw;    vt = kScalar; return true; }
					if( name == "fwo" )   { slot = kContextSlotFwo;   vt = kScalar; return true; }
					if( name == "time" )  { slot = kContextSlotTime;  vt = kScalar; return true; }
					if( name == "curv" )  { slot = kContextSlotCurv;  vt = kScalar; return true; }
					if( name == "curvR" ) { slot = kContextSlotCurvR; vt = kScalar; return true; }
					return false;
				}

				int Slot( const std::string& name, VType vt )
				{
					std::map<std::string,int>::const_iterator it = m_index.find( name );
					if( it != m_index.end() ) {
						const int s = it->second;
						if( m_slotType[ s ] != vt && !m_hasBuilderError ) {
							SetError( "internal error: slot `" + name + "` reused with a different type", -1 );
							m_hasBuilderError = true;
						}
						return s;
					}
					const int s = (int)m_names.size();
					m_names.push_back( name );
					if( vt == kVec3 ) { m_names.push_back( name + ".y" ); m_names.push_back( name + ".z" ); }
					m_index[ name ] = s;
					m_slotType[ s ] = vt;
					return s;
				}

				// Builtin signature table (legacy scalar ids 1..33 unchanged).
				struct FnSig { const char* n; int id; int nArgs; VType argT[4]; VType ret; };
				static const FnSig* FindSig( const std::string& name )
				{
					static const VType S = kScalar, V = kVec3;
					static const FnSig table[] = {
						{"sin",1,1,{S,S,S,S},S}, {"cos",2,1,{S,S,S,S},S}, {"tan",3,1,{S,S,S,S},S},
						{"asin",4,1,{S,S,S,S},S}, {"acos",5,1,{S,S,S,S},S}, {"atan",6,1,{S,S,S,S},S},
						{"exp",7,1,{S,S,S,S},S}, {"log",8,1,{S,S,S,S},S}, {"sqrt",9,1,{S,S,S,S},S},
						{"abs",10,1,{S,S,S,S},S}, {"floor",11,1,{S,S,S,S},S}, {"ceil",12,1,{S,S,S,S},S},
						{"frac",13,1,{S,S,S,S},S}, {"sign",14,1,{S,S,S,S},S},
						{"atan2",20,2,{S,S,S,S},S}, {"mod",21,2,{S,S,S,S},S}, {"min",22,2,{S,S,S,S},S},
						{"max",23,2,{S,S,S,S},S}, {"pow",24,2,{S,S,S,S},S}, {"hypot",25,2,{S,S,S,S},S},
						{"step",26,2,{S,S,S,S},S},
						{"clamp",30,3,{S,S,S,S},S}, {"smoothstep",31,3,{S,S,S,S},S}, {"select",33,3,{S,S,S,S},S},
						{"dot",40,2,{V,V,S,S},S}, {"length",41,1,{V,S,S,S},S},
						{"perlin",ExpressionProgram::kFnPerlin,1,{V,S,S,S},S},
						{"fbm",ExpressionProgram::kFnFbm,4,{V,S,S,S},S},
						{"turbulence",ExpressionProgram::kFnTurbulence,4,{V,S,S,S},S},
						{"ridged",ExpressionProgram::kFnRidged,4,{V,S,S,S},S},
						{"worley_f1",ExpressionProgram::kFnWorleyF1,2,{V,S,S,S},S},
						{"worley_f2",ExpressionProgram::kFnWorleyF2,2,{V,S,S,S},S},
						{"worley_f2f1",ExpressionProgram::kFnWorleyF2F1,2,{V,S,S,S},S},
						{"worley_id",ExpressionProgram::kFnWorleyId,2,{V,S,S,S},S},
						{"cellhash",ExpressionProgram::kFnCellHash,1,{S,S,S,S},S},
						{"occlusion",ExpressionProgram::kFnOcclusion,1,{S,S,S,S},S},
						{"thickness",ExpressionProgram::kFnThickness,1,{S,S,S,S},S},
						{"convexity",ExpressionProgram::kFnConvexity,1,{S,S,S,S},S},
						{"proximity",ExpressionProgram::kFnProximity,1,{S,S,S,S},S},
						{"interior",ExpressionProgram::kFnInterior,1,{S,S,S,S},S},
						{"cross",60,2,{V,V,S,S},V}, {"normalize",61,1,{V,S,S,S},V},
					};
					for( size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i ) {
						if( name == table[i].n ) return &table[i];
					}
					return 0;
				}
				static const int kFnMixScalar = 32;
				static const int kFnMixVec3   = 62;

				//////////////////////////////////////////////////
				// Recursive-descent parser -> postfix emit
				//////////////////////////////////////////////////
				struct Tok { enum T { Num, Ident, Str, Op, LP, RP, Comma, Dot, End } t; Scalar num; std::string s; size_t off; };

				bool Tokenize( const std::string& e, std::vector<Tok>& out )
				{
					size_t i = 0, n = e.size();
					while( i < n ) {
						const char c = e[i];
						if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) { ++i; continue; }
						if( c == '"' ) {
							const size_t startOff = i++;
							std::string s;
							bool closed = false;
							auto hexVal = []( char ch ) -> int {
								if( ch >= '0' && ch <= '9' ) return ch - '0';
								if( ch >= 'a' && ch <= 'f' ) return 10 + ( ch - 'a' );
								if( ch >= 'A' && ch <= 'F' ) return 10 + ( ch - 'A' );
								return -1;
							};
							while( i < n ) {
								const char ch = e[i++];
								if( ch == '"' ) { closed = true; break; }
								if( ch == '\\' ) {
									if( i >= n ) { SetError( "unterminated escape in string literal", (ptrdiff_t)startOff ); return false; }
									const char esc = e[i++];
									if( esc == '"' || esc == '\\' ) { s.push_back( esc ); }
									else if( esc == 't' ) { s.push_back( '\t' ); }
									else if( esc == 'x' ) {
										if( i + 1 >= n ) { SetError( "incomplete \\xHH escape in string literal", (ptrdiff_t)(i - 2) ); return false; }
										const int hi = hexVal( e[i] );
										const int lo = hexVal( e[i + 1] );
										if( hi < 0 || lo < 0 ) { SetError( "invalid hex digits in \\xHH escape", (ptrdiff_t)(i - 2) ); return false; }
										const unsigned char byte = static_cast<unsigned char>( ( hi << 4 ) | lo );
										if( byte == 0 ) { SetError( "\\x00 NUL byte is not allowed in string literal", (ptrdiff_t)(i - 2) ); return false; }
										s.push_back( static_cast<char>( byte ) );
										i += 2;
									}
									else { SetError( "unknown escape sequence in string literal (supported: \\\", \\\\, \\t, \\xHH)", (ptrdiff_t)(i - 2) ); return false; }
								} else {
									s.push_back( ch );
								}
							}
							if( !closed ) { SetError( "unterminated string literal", (ptrdiff_t)startOff ); return false; }
							Tok t; t.t = Tok::Str; t.s = std::move( s ); t.off = startOff;
							out.push_back( t );
							continue;
						}
						if( c == '.' && i+1 < n && ( ( e[i+1]>='a'&&e[i+1]<='z' ) || ( e[i+1]>='A'&&e[i+1]<='Z' ) ) ) {
							Tok t; t.t = Tok::Dot; t.off = i; out.push_back( t ); ++i; continue;
						}
						if( ( c >= '0' && c <= '9' ) || c == '.' ) {
							const char* start = e.c_str() + i;
							char* end = 0;
							const double val = strtod( start, &end );
							if( end == start ) { SetError( "bad number", (ptrdiff_t)i ); return false; }
							Tok t; t.t = Tok::Num; t.num = Scalar(val); t.off = i;
							out.push_back( t );
							i += ( end - start );
							continue;
						}
						if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' ) {
							size_t j = i;
							while( j < n && ( ( e[j]>='a'&&e[j]<='z' ) || ( e[j]>='A'&&e[j]<='Z' ) || ( e[j]>='0'&&e[j]<='9' ) || e[j]=='_' ) ) ++j;
							Tok t; t.t = Tok::Ident; t.s = e.substr( i, j - i ); t.off = i;
							out.push_back( t );
							i = j;
							continue;
						}
						if( c == '(' ) { Tok t; t.t = Tok::LP; t.off = i; out.push_back(t); ++i; continue; }
						if( c == ')' ) { Tok t; t.t = Tok::RP; t.off = i; out.push_back(t); ++i; continue; }
						if( c == ',' ) { Tok t; t.t = Tok::Comma; t.off = i; out.push_back(t); ++i; continue; }
						// operators, incl. two-char comparisons
						Tok t; t.t = Tok::Op; t.off = i;
						if( ( c=='<'||c=='>'||c=='='||c=='!' ) && i+1<n && e[i+1]=='=' ) { t.s = e.substr(i,2); i+=2; }
						else { t.s = std::string(1,c); ++i; }
						out.push_back( t );
					}
					Tok t; t.t = Tok::End; t.off = n;
					out.push_back( t );
					return true;
				}

				// parser state
				const std::vector<Tok>* m_toks;
				size_t m_pos;
				Compiled* m_emit;
				int m_parseDepth;

				const Tok& Cur() const { return (*m_toks)[ m_pos ]; }
				void Advance() { ++m_pos; }
				size_t CurOff() const { return Cur().off; }

				bool Expect( Tok::T t, const char* msg, size_t offOnFail )
				{
					if( Cur().t != t ) { SetError( msg, (ptrdiff_t)( Cur().t == Tok::End ? offOnFail : CurOff() ) ); return false; }
					Advance();
					return true;
				}

				bool Compile( const std::string& expr, Compiled& out, VType& outType )
				{
					std::vector<Tok> toks;
					if( !Tokenize( expr, toks ) ) return false;
					m_toks = &toks; m_pos = 0; m_emit = &out; m_parseDepth = 0;
					out.code.clear();
					if( !ParseCmp( outType ) ) return false;
					if( Cur().t != Tok::End ) { SetError( "trailing tokens in `" + expr + "`", (ptrdiff_t)CurOff() ); return false; }
					// compile-time value-stack bound: simulate the postfix stack
					// effect so a deferred-operand expression can never overflow
					// the fixed Eval stack.  Net stack delta is implied by the
					// opcode, except kFunc/kFuncV3/kRamp/kOsl/kOslV3 whose arity
					// varies -- those read it straight off the instruction.
					int sp = 0, mx = 0;
					for( size_t i = 0; i < out.code.size(); ++i ) {
						const Compiled::Instr& in = out.code[i];
						switch( in.op ) {
						case Compiled::kConst: case Compiled::kVar: sp += 1; break;
						case Compiled::kNeg: case Compiled::kNegV: break;
						case Compiled::kSplat3: sp += 2; break;
						case Compiled::kSwizzle: sp -= 2; break;
						case Compiled::kAddV: case Compiled::kSubV: case Compiled::kMulV: case Compiled::kDivV: sp -= 3; break;
						case Compiled::kFunc: sp += ( 1 - in.arity ); break;
						case Compiled::kFuncV3: sp += ( 3 - in.arity ); break;
						case Compiled::kRamp: sp += ( in.fn - in.arity ); break;
						case Compiled::kOsl: sp += ( 1 - in.arity ); break;
						case Compiled::kOslV3: sp += ( 3 - in.arity ); break;
						case Compiled::kMeshAttr3: sp += 3; break;
						case Compiled::kMeshAttrAlpha: sp += 1; break;
						case Compiled::kSampleAlpha: sp += 1; break;
						case Compiled::kSampleUv: sp += 1; break;
						case Compiled::kSampleUvAlpha: sp -= 1; break;
						default: sp -= 1; break;	// binary scalar ops
						}
						// A negative sp here means SOME opcode's declared stack effect
						// (the switch above) doesn't match what the emitter actually
						// produced -- i.e. a compiler-internal bug (a future opcode
						// added to Compiled::Op without a matching case here, or a
						// mismatched arity), not a bad user expression.  Catch it as a
						// compile error instead of letting RunAny index the runtime
						// stack array with a computed-negative offset later.
						if( sp < 0 ) { SetError( "internal error: expression compiler stack underflow (this is a compiler bug, not a bad expression)", -1 ); return false; }
						if( sp > mx ) mx = sp;
					}
					if( mx > ExpressionProgram::kStackCap ) { SetError( "expression too large (value-stack depth exceeds 512)", -1 ); return false; }
					return true;
				}

				// depth-bounded entry to the recursive descent (rejects
				// pathologically nested input before it overflows the C++ stack).
				bool ParseCmp( VType& type )
				{
					if( ++m_parseDepth > ExpressionProgram::kMaxParseDepth ) { SetError( "expression nesting too deep", (ptrdiff_t)CurOff() ); --m_parseDepth; return false; }
					const bool ok = ParseCmpImpl( type );
					--m_parseDepth;
					return ok;
				}

				void EmitConst( Scalar v ) { Compiled::Instr in; in.op=Compiled::kConst; in.val=v; in.valo=0; in.idx=-1; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitVarSlot( int idx ) { Compiled::Instr in; in.op=Compiled::kVar; in.val=0; in.valo=0; in.idx=idx; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitOp( Compiled::Op o ) { Compiled::Instr in; in.op=o; in.val=0; in.valo=0; in.idx=-1; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitSwizzle( int comp ) { Compiled::Instr in; in.op=Compiled::kSwizzle; in.val=0; in.valo=0; in.idx=comp; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitSplat3() { EmitOp( Compiled::kSplat3 ); }
				//! `fwScale`/`fwoScale` are consumed only by scalar kFunc (CallFunc);
				//! kFuncV3 ignores them.
				void EmitFuncCall( int fn, int arity, bool isVec3, Scalar fwScale = Scalar(1), Scalar fwoScale = Scalar(0) )
				{
					Compiled::Instr in; in.op = isVec3 ? Compiled::kFuncV3 : Compiled::kFunc;
					in.val=fwScale; in.valo=fwoScale; in.idx=-1; in.fn=fn; in.arity=arity;
					m_emit->code.push_back(in);
				}

				void EmitVec3Var( int s ) { EmitVarSlot(s); EmitVarSlot(s+1); EmitVarSlot(s+2); }

				// Forward-mode compile-time Jacobian over emitted postfix code for
				// noise domain scaling: computes spectral norms sP = ||d(arg)/dP||_2
				// and sPo = ||d(arg)/dPo||_2; runtime uses `sP * fw + sPo * fwo`.
				static const int kLinVars = 6;		//!< g[0..2] = d/dP, g[3..5] = d/dPo
				static const int kLinPoBase = 3;

				//! Affine form w.r.t. (P, Po); invariant: constKnown => gradKnown with g == 0.
				struct LinScalar
				{
					bool   gradKnown;
					bool   gradExact;
					bool   constKnown;
					Scalar c;
					Scalar g[kLinVars];
					LinScalar() : gradKnown(false), gradExact(false), constKnown(false), c(0)
					{ for( int k = 0; k < kLinVars; ++k ) g[k] = Scalar(0); }
					static LinScalar Const( Scalar v )
					{
						LinScalar r; r.gradKnown = true; r.gradExact = true; r.constKnown = true; r.c = v;
						return r;
					}
				};

				static LinScalar LinNeg( const LinScalar& a )
				{
					LinScalar r = a;
					r.c = -a.c;
					for( int k = 0; k < kLinVars; ++k ) r.g[k] = -a.g[k];
					return r;
				}

				//! Addition with one unknown side keeps the known affine gradient
				//! (inexact) to support domain warps (`P*8 + warp*fbm(...)`);
				//! subtraction with an unknown side drops to unknown (avoids
				//! over-filtering cancelling differences like `P*40 - q`).
				static LinScalar LinAddSub( const LinScalar& a, const LinScalar& b, bool plus )
				{
					LinScalar r;
					if( a.constKnown && b.constKnown ) {
						r.constKnown = true;
						r.c = plus ? ( a.c + b.c ) : ( a.c - b.c );
					}
					if( a.gradKnown && b.gradKnown ) {
						r.gradKnown = true;
						r.gradExact = a.gradExact && b.gradExact;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = plus ? ( a.g[k] + b.g[k] ) : ( a.g[k] - b.g[k] );
					} else if( plus && a.gradKnown ) {
						r.gradKnown = true; r.gradExact = false;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = a.g[k];
					} else if( plus && b.gradKnown ) {
						r.gradKnown = true; r.gradExact = false;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = b.g[k];
					}
					return r;
				}

				static LinScalar LinMul( const LinScalar& a, const LinScalar& b )
				{
					LinScalar r;
					if( a.constKnown && b.constKnown ) return LinScalar::Const( a.c * b.c );
					if( a.constKnown && b.gradKnown ) {
						r.gradKnown = true; r.gradExact = b.gradExact;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = a.c * b.g[k];
						return r;
					}
					if( b.constKnown && a.gradKnown ) {
						r.gradKnown = true; r.gradExact = a.gradExact;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = b.c * a.g[k];
						return r;
					}
					return r;
				}

				//! Mirrors RunAny's kDiv (division by 0 yields 0).
				static LinScalar LinDiv( const LinScalar& a, const LinScalar& b )
				{
					LinScalar r;
					if( !b.constKnown ) return r;
					if( b.c == Scalar(0) ) return LinScalar::Const( Scalar(0) );
					if( a.constKnown ) return LinScalar::Const( a.c / b.c );
					if( a.gradKnown ) {
						r.gradKnown = true; r.gradExact = a.gradExact;
						for( int k = 0; k < kLinVars; ++k ) r.g[k] = a.g[k] / b.c;
					}
					return r;
				}

				LinScalar SlotLinear( int slot ) const
				{
					if( slot >= ExpressionProgram::kContextSlotP && slot < ExpressionProgram::kContextSlotP + 3 ) {
						LinScalar r;
						r.gradKnown = true; r.gradExact = true;
						r.g[ slot - ExpressionProgram::kContextSlotP ] = Scalar(1);
						return r;
					}
					if( slot >= ExpressionProgram::kContextSlotPo && slot < ExpressionProgram::kContextSlotPo + 3 ) {
						LinScalar r;
						r.gradKnown = true; r.gradExact = true;
						r.g[ kLinPoBase + slot - ExpressionProgram::kContextSlotPo ] = Scalar(1);
						return r;
					}
					std::map<int,LinScalar>::const_iterator it = m_slotLin.find( slot );
					if( it != m_slotLin.end() ) return it->second;
					return LinScalar();
				}

				//! Abstract-interprets `code[begin,end)` over LinScalar, mirroring RunAny's stack effects.
				bool AnalyzeLinear( const std::vector<Compiled::Instr>& code, size_t begin, size_t end,
					int expectWidth, LinScalar* out ) const
				{
					std::vector<LinScalar> st;
					for( size_t i = begin; i < end; ++i ) {
						const Compiled::Instr& in = code[i];
						switch( in.op )
						{
						case Compiled::kConst: st.push_back( LinScalar::Const( in.val ) ); break;
						case Compiled::kVar:   st.push_back( SlotLinear( in.idx ) ); break;
						case Compiled::kNeg:
							if( st.empty() ) return false;
							st.back() = LinNeg( st.back() );
							break;
						case Compiled::kAdd: case Compiled::kSub:
						{
							if( st.size() < 2 ) return false;
							const LinScalar rhs = st.back(); st.pop_back();
							st.back() = LinAddSub( st.back(), rhs, in.op == Compiled::kAdd );
						} break;
						case Compiled::kMul:
						{
							if( st.size() < 2 ) return false;
							const LinScalar rhs = st.back(); st.pop_back();
							st.back() = LinMul( st.back(), rhs );
						} break;
						case Compiled::kDiv:
						{
							if( st.size() < 2 ) return false;
							const LinScalar rhs = st.back(); st.pop_back();
							st.back() = LinDiv( st.back(), rhs );
						} break;
						case Compiled::kMod: case Compiled::kPow:
						case Compiled::kLt: case Compiled::kGt: case Compiled::kLe:
						case Compiled::kGe: case Compiled::kEq: case Compiled::kNe:
						{
							if( st.size() < 2 ) return false;
							st.pop_back();
							st.back() = LinScalar();
						} break;
						case Compiled::kSplat3:
						{
							if( st.empty() ) return false;
							const LinScalar v = st.back();
							st.push_back( v ); st.push_back( v );
						} break;
						case Compiled::kAddV: case Compiled::kSubV:
						case Compiled::kMulV: case Compiled::kDivV:
						{
							if( st.size() < 6 ) return false;
							const size_t l = st.size() - 6;
							for( int k = 0; k < 3; ++k ) {
								const LinScalar& a = st[l+k];
								const LinScalar& b = st[l+3+k];
								LinScalar v;
								if( in.op == Compiled::kAddV )      v = LinAddSub( a, b, true );
								else if( in.op == Compiled::kSubV ) v = LinAddSub( a, b, false );
								else if( in.op == Compiled::kMulV ) v = LinMul( a, b );
								else                                v = LinDiv( a, b );
								st[l+k] = v;
							}
							st.resize( l + 3 );
						} break;
						case Compiled::kNegV:
						{
							if( st.size() < 3 ) return false;
							const size_t l = st.size() - 3;
							for( int k = 0; k < 3; ++k ) st[l+k] = LinNeg( st[l+k] );
						} break;
						case Compiled::kSwizzle:
						{
							if( st.size() < 3 ) return false;
							if( in.idx < 0 || in.idx > 2 ) return false;
							const LinScalar v = st[ st.size() - 3 + (size_t)in.idx ];
							st.resize( st.size() - 3 );
							st.push_back( v );
						} break;
						case Compiled::kFunc:
						{
							if( in.arity < 0 || st.size() < (size_t)in.arity ) return false;
							st.resize( st.size() - (size_t)in.arity );
							st.push_back( LinScalar() );
						} break;
						case Compiled::kFuncV3:
						{
							if( in.arity < 0 || st.size() < (size_t)in.arity ) return false;
							st.resize( st.size() - (size_t)in.arity );
							for( int k = 0; k < 3; ++k ) st.push_back( LinScalar() );
						} break;
						case Compiled::kRamp:
						{
							if( in.arity < 0 || st.size() < (size_t)in.arity ) return false;
							st.resize( st.size() - (size_t)in.arity );
							for( int k = 0; k < in.fn; ++k ) st.push_back( LinScalar() );
						} break;
						case Compiled::kOsl:
						case Compiled::kOslV3:
						{
							if( in.arity < 0 || st.size() < (size_t)in.arity ) return false;
							st.resize( st.size() - (size_t)in.arity );
							const int w = ( in.op == Compiled::kOslV3 ) ? 3 : 1;
							for( int k = 0; k < w; ++k ) st.push_back( LinScalar() );
						} break;
						case Compiled::kMeshAttr3:
						case Compiled::kMeshAttrAlpha:
						case Compiled::kSampleAlpha:
						{
							const int w = ( in.op == Compiled::kMeshAttr3 ) ? 3 : 1;
							for( int k = 0; k < w; ++k ) st.push_back( LinScalar() );
						} break;
						case Compiled::kSampleUv:
						case Compiled::kSampleUvAlpha:
						{
							if( st.size() < 2 ) return false;
							st.resize( st.size() - 2 );
							const int w = ( in.op == Compiled::kSampleUv ) ? 3 : 1;
							for( int k = 0; k < w; ++k ) st.push_back( LinScalar() );
						} break;
						default:
							return false;
						}
						if( st.size() > (size_t)ExpressionProgram::kStackCap ) return false;
					}
					if( st.size() != (size_t)expectWidth ) return false;
					for( int k = 0; k < expectWidth; ++k ) out[k] = st[(size_t)k];
					return true;
				}

				//! Largest singular value of the 3x3 Jacobian slice at `gBase` (0=P, 3=Po).
				//! Diagonal fast path gives exact |d_i| (keeps P/axis-scaled fbm bit-identical);
				//! general case runs 32-sweep cyclic Jacobi on J^T J.
				static Scalar JacobianSpectralNorm( const LinScalar rows[3], const int gBase )
				{
					Scalar J[3][3];
					for( int i = 0; i < 3; ++i ) {
						for( int k = 0; k < 3; ++k ) J[i][k] = rows[i].g[gBase+k];
					}
					if( J[0][1] == Scalar(0) && J[0][2] == Scalar(0) &&
					    J[1][0] == Scalar(0) && J[1][2] == Scalar(0) &&
					    J[2][0] == Scalar(0) && J[2][1] == Scalar(0) ) {
						Scalar m = std::fabs( J[0][0] );
						const Scalar m1 = std::fabs( J[1][1] );
						const Scalar m2 = std::fabs( J[2][2] );
						if( m1 > m ) m = m1;
						if( m2 > m ) m = m2;
						return m;
					}
					Scalar A[3][3];
					for( int i = 0; i < 3; ++i ) {
						for( int k = 0; k < 3; ++k ) {
							A[i][k] = J[0][i]*J[0][k] + J[1][i]*J[1][k] + J[2][i]*J[2][k];
						}
					}
					for( int sweep = 0; sweep < 32; ++sweep ) {
						const Scalar off = A[0][1]*A[0][1] + A[0][2]*A[0][2] + A[1][2]*A[1][2];
						const Scalar diag = A[0][0]*A[0][0] + A[1][1]*A[1][1] + A[2][2]*A[2][2];
						if( !( off > Scalar(1e-30) * ( diag + Scalar(1) ) ) ) break;
						for( int p = 0; p < 2; ++p ) {
							for( int q = p+1; q < 3; ++q ) {
								if( A[p][q] == Scalar(0) ) continue;
								const Scalar theta = ( A[q][q] - A[p][p] ) / ( Scalar(2) * A[p][q] );
								const Scalar sgn = ( theta >= Scalar(0) ) ? Scalar(1) : Scalar(-1);
								const Scalar t = sgn / ( std::fabs( theta ) + std::sqrt( theta*theta + Scalar(1) ) );
								const Scalar cs = Scalar(1) / std::sqrt( t*t + Scalar(1) );
								const Scalar sn = t * cs;
								const Scalar apq = A[p][q];
								A[p][p] = A[p][p] - t*apq;
								A[q][q] = A[q][q] + t*apq;
								A[p][q] = A[q][p] = Scalar(0);
								const int r = 3 - p - q;
								const Scalar arp = A[r][p], arq = A[r][q];
								A[r][p] = A[p][r] = cs*arp - sn*arq;
								A[r][q] = A[q][r] = sn*arp + cs*arq;
							}
						}
					}
					Scalar lam = A[0][0];
					if( A[1][1] > lam ) lam = A[1][1];
					if( A[2][2] > lam ) lam = A[2][2];
					if( !( lam > Scalar(0) ) ) return Scalar(0);
					return std::sqrt( lam );
				}

				//! Computes (fw, fwo) multipliers for a noise position argument in [begin, end).
				//! Exact constant -> (0, 0); unprovable -> fallback (1, 0).
				void NoiseFwScales( size_t begin, size_t end, Scalar& outP, Scalar& outPo ) const
				{
					outP = Scalar(1); outPo = Scalar(0);

					LinScalar rows[3];
					if( !AnalyzeLinear( m_emit->code, begin, end, 3, rows ) ) return;
					for( int i = 0; i < 3; ++i ) {
						if( !rows[i].gradKnown ) return;
						for( int k = 0; k < kLinVars; ++k ) {
							if( !ExpressionProgram::IsFinite( rows[i].g[k] ) ) return;
						}
					}
					const Scalar sP  = JacobianSpectralNorm( rows, 0 );
					const Scalar sPo = JacobianSpectralNorm( rows, kLinPoBase );
					if( !ExpressionProgram::IsFinite( sP )  || sP  < Scalar(0) ) return;
					if( !ExpressionProgram::IsFinite( sPo ) || sPo < Scalar(0) ) return;
					if( sP == Scalar(0) && sPo == Scalar(0) ) {
						const bool exact = rows[0].gradExact && rows[1].gradExact && rows[2].gradExact;
						if( exact ) { outP = Scalar(0); outPo = Scalar(0); }
						return;
					}
					outP = sP; outPo = sPo;
				}

				void RecordSlotLinear( int slot, int w, const LinScalar* lin )
				{
					for( int k = 0; k < w; ++k ) m_slotLin[ slot + k ] = lin ? lin[k] : LinScalar();
				}

				std::map<int,LinScalar> m_slotLin;
				std::map<int,bool> m_slotIsDef;	//!< BindEnv runs defs after params, so defs win shared slots

				// Inserts kSplat3 at `insertPos` (end of LEFT scalar operand) when broadcasting scalar+vec3.
				void BroadcastAt( size_t insertPos )
				{
					Compiled::Instr in; in.op=Compiled::kSplat3; in.val=0; in.valo=0; in.idx=-1; in.fn=-1; in.arity=0;
					m_emit->code.insert( m_emit->code.begin() + insertPos, in );
				}

				// Combines two already-emitted operands (types lt, rt) with a
				// scalar-scalar op or a vec3-vec3 op, broadcasting whichever
				// side is scalar when the other is vec3.  rhsStart is the
				// code index where the right operand's instructions begin
				// (== code index right after the left operand finished).
				bool EmitArith( VType& lt, VType rt, size_t rhsStart, Compiled::Op scalarOp, Compiled::Op vecOp )
				{
					if( lt == kScalar && rt == kScalar ) { EmitOp( scalarOp ); return true; }
					if( lt == kVec3   && rt == kVec3   ) { EmitOp( vecOp );    return true; }
					if( lt == kScalar && rt == kVec3 ) {
						BroadcastAt( rhsStart );	// left operand: scalar -> vec3
						EmitOp( vecOp );
						lt = kVec3;
						return true;
					}
					// lt == kVec3 && rt == kScalar
					EmitSplat3();				// right operand (top of stack): scalar -> vec3
					EmitOp( vecOp );
					return true;
				}

				// cmp := add ( (<|>|<=|>=|==|!=) add )?
				bool ParseCmpImpl( VType& type )
				{
					if( !ParseAdd( type ) ) return false;
					if( Cur().t == Tok::Op ) {
						const std::string& o = Cur().s;
						Compiled::Op op;
						if( o=="<" ) op=Compiled::kLt; else if( o==">" ) op=Compiled::kGt;
						else if( o=="<=" ) op=Compiled::kLe; else if( o==">=" ) op=Compiled::kGe;
						else if( o=="==" ) op=Compiled::kEq; else if( o=="!=" ) op=Compiled::kNe;
						else return true;
						const size_t errOff = CurOff();
						Advance();
						VType rtype;
						if( !ParseAdd( rtype ) ) return false;
						if( type != kScalar || rtype != kScalar ) { SetError( "comparison operators require scalar operands", (ptrdiff_t)errOff ); return false; }
						EmitOp( op );
						type = kScalar;
					}
					return true;
				}
				// add := mul ( (+|-) mul )*
				bool ParseAdd( VType& type )
				{
					if( !ParseMul( type ) ) return false;
					while( Cur().t == Tok::Op && ( Cur().s=="+" || Cur().s=="-" ) ) {
						const bool plus = ( Cur().s=="+" );
						Advance();
						VType rtype;
						const size_t rhsStart = m_emit->code.size();
						if( !ParseMul( rtype ) ) return false;
						if( !EmitArith( type, rtype, rhsStart, plus?Compiled::kAdd:Compiled::kSub, plus?Compiled::kAddV:Compiled::kSubV ) ) return false;
					}
					return true;
				}
				// mul := unary ( (*|/|%) unary )*
				bool ParseMul( VType& type )
				{
					if( !ParseUnary( type ) ) return false;
					while( Cur().t == Tok::Op && ( Cur().s=="*" || Cur().s=="/" || Cur().s=="%" ) ) {
						const std::string o = Cur().s;
						const size_t errOff = CurOff();
						Advance();
						VType rtype;
						const size_t rhsStart = m_emit->code.size();
						if( !ParseUnary( rtype ) ) return false;
						if( o == "%" ) {
							if( type != kScalar || rtype != kScalar ) { SetError( "`%` requires scalar operands", (ptrdiff_t)errOff ); return false; }
							EmitOp( Compiled::kMod );
							continue;
						}
						if( !EmitArith( type, rtype, rhsStart, o=="*"?Compiled::kMul:Compiled::kDiv, o=="*"?Compiled::kMulV:Compiled::kDivV ) ) return false;
					}
					return true;
				}
				// unary := (-|+)? pow (depth-bounded against long chains of leading signs)
				bool ParseUnary( VType& type )
				{
					if( ++m_parseDepth > ExpressionProgram::kMaxParseDepth ) { SetError( "expression nesting too deep", (ptrdiff_t)CurOff() ); --m_parseDepth; return false; }
					bool ok;
					if( Cur().t == Tok::Op && ( Cur().s=="-" || Cur().s=="+" ) ) {
						const bool neg = ( Cur().s=="-" );
						Advance();
						ok = ParseUnary( type );
						if( ok && neg ) EmitOp( type==kVec3 ? Compiled::kNegV : Compiled::kNeg );
					} else {
						ok = ParsePow( type );
					}
					--m_parseDepth;
					return ok;
				}
				// pow := postfix (^ unary)?    right-assoc, scalar-only
				bool ParsePow( VType& type )
				{
					if( !ParsePostfix( type ) ) return false;
					if( Cur().t == Tok::Op && Cur().s=="^" ) {
						const size_t errOff = CurOff();
						Advance();
						VType rtype;
						if( !ParseUnary( rtype ) ) return false;
						if( type != kScalar || rtype != kScalar ) { SetError( "`^` requires scalar operands", (ptrdiff_t)errOff ); return false; }
						EmitOp( Compiled::kPow );
					}
					return true;
				}
				// postfix := atom ( '.' (x|y|z) )*      -- vec3 swizzle
				bool ParsePostfix( VType& type )
				{
					if( !ParseAtom( type ) ) return false;
					while( Cur().t == Tok::Dot ) {
						const size_t errOff = CurOff();
						Advance();	// consume '.'
						if( Cur().t != Tok::Ident || Cur().s.size() != 1 ||
							( Cur().s[0] != 'x' && Cur().s[0] != 'y' && Cur().s[0] != 'z' ) ) {
							SetError( "expected .x, .y, or .z", (ptrdiff_t)errOff );
							return false;
						}
						if( type != kVec3 ) {
							SetError( "`." + Cur().s + "` on a scalar (only vec3 has components)", (ptrdiff_t)errOff );
							return false;
						}
						const int comp = ( Cur().s[0]=='x' ) ? 0 : ( Cur().s[0]=='y' ? 1 : 2 );
						Advance();
						EmitSwizzle( comp );
						type = kScalar;
					}
					return true;
				}
				// atom := NUM | IDENT | IDENT '(' args ')' | '(' cmp ')'
				bool ParseAtom( VType& type )
				{
					const Tok& t = Cur();
					if( t.t == Tok::Num ) { EmitConst( t.num ); type = kScalar; Advance(); return true; }
					if( t.t == Tok::LP ) {
						Advance();
						if( !ParseCmp( type ) ) return false;
						if( Cur().t != Tok::RP ) { SetError( "missing )", (ptrdiff_t)CurOff() ); return false; }
						Advance();
						return true;
					}
					if( t.t == Tok::Ident ) {
						const std::string name = t.s;
						const size_t nameOff = t.off;
						Advance();
						if( Cur().t == Tok::LP ) {
							// Call position dispatches before m_index/context lookup (params cannot shadow builtins).
							return ParseCall( name, nameOff, type );
						}
						// User param/def lookup first (shadows context vars); binds against current m_index.
						std::map<std::string,int>::const_iterator it = m_index.find( name );
						if( it != m_index.end() ) {
							const int s = it->second;
							type = m_slotType[s];
							if( type == kVec3 ) EmitVec3Var( s ); else EmitVarSlot( s );
							return true;
						}
						{
							int ctxSlot; VType ctxType;
							if( LookupContextVar( name, ctxSlot, ctxType ) ) {
								const bool alwaysAvailable = ( name == "u" || name == "v" );
								if( alwaysAvailable || m_contextVarsEnabled ) {
									if( ctxSlot < kContextVarMaskBits ) {
										m_ctxUsed |= ( 1u << ctxSlot );
									}
									type = ctxType;
									if( type == kVec3 ) EmitVec3Var( ctxSlot ); else EmitVarSlot( ctxSlot );
									return true;
								}
							}
						}
						// allow a couple of math constants
						if( name == "pi" )  { EmitConst( Scalar(3.14159265358979323846) ); type = kScalar; return true; }
						if( name == "tau" ) { EmitConst( Scalar(6.28318530717958647692) ); type = kScalar; return true; }
						if( name == "e" )   { EmitConst( Scalar(2.71828182845904523536) ); type = kScalar; return true; }
						SetError( "unknown variable `" + name + "`", (ptrdiff_t)nameOff );
						return false;
					}
					SetError( "unexpected token", (ptrdiff_t)CurOff() );
					return false;
				}

				// Parses the '(' args ')' of a call already positioned right
				// after the identifier `name` (nameOff is the identifier's
				// offset, used for arity/unknown-function errors).
				bool ParseCall( const std::string& name, size_t nameOff, VType& outType )
				{
					Advance();	// '('
					if( name == "vec3" ) return ParseVec3Ctor( nameOff, outType );
					if( name == "ramp" ) return ParseRampCall( nameOff, outType );
					if( name == "mix" )  return ParseMixCall( nameOff, outType );
					if( name == "sample" || name == "sample_scalar" || name == "sample_alpha"
						|| name == "sample_uv" || name == "sample_uv_alpha" ) {
						return ParseSampleCall( name, nameOff, outType );
					}
					if( name == "mesh_attr3" || name == "mesh_attr_alpha" ) return ParseMeshAttrCall( name, nameOff, outType );

					const FnSig* sig = FindSig( name );
					if( !sig ) {
						if( const OslBuiltins::Sig* os = OslBuiltins::Find( name ) ) return ParseOslCall( *os, nameOff, outType );
						SetError( "unknown function `" + name + "`", (ptrdiff_t)nameOff );
						return false;
					}

					// Geometry signal builtins (gated on m_contextVarsEnabled).
					// Adding an id here also requires reviewing the radius unit diagnostic,
					// m_sigCalls registration, and the DynR remap guard below.
					const bool isSignalFn = ( sig->id == ExpressionProgram::kFnOcclusion ||
					                          sig->id == ExpressionProgram::kFnThickness ||
					                          sig->id == ExpressionProgram::kFnConvexity ||
					                          sig->id == ExpressionProgram::kFnProximity ||
					                          sig->id == ExpressionProgram::kFnInterior );
					if( isSignalFn && !m_contextVarsEnabled ) {
						SetError( "`" + name + "()` needs the 3D surface context -- available in expression_painter "
							"and scalar_painter { expression ... }, not in expression_function2d (a UV-only field)",
							(ptrdiff_t)nameOff );
						return false;
					}

					bool literalRadiusArg = false;
					Scalar literalRadiusVal = Scalar(0);

					const bool isNoiseFn = ( sig->id == ExpressionProgram::kFnFbm ||
					                         sig->id == ExpressionProgram::kFnTurbulence ||
					                         sig->id == ExpressionProgram::kFnRidged );
					size_t posArgStart = 0, posArgEnd = 0;

					int scalarArity = 0;
					int got = 0;
					if( Cur().t != Tok::RP ) {
						for( ;; ) {
							const size_t argOff = CurOff();
							// Check octaves at compile time only when the argument is a single Num token.
							const bool literalOctaveArg = ( ( sig->id==43 || sig->id==44 || sig->id==45 ) && got==1 && Cur().t==Tok::Num &&
								m_pos+1 < m_toks->size() && ( (*m_toks)[m_pos+1].t == Tok::Comma || (*m_toks)[m_pos+1].t == Tok::RP ) );
							const Scalar literalOctaveVal = literalOctaveArg ? Cur().num : Scalar(0);
							if( isSignalFn && got == 0 ) {
								literalRadiusArg = PeekLiteralScalarArg( literalRadiusVal );
							}
							const size_t emitStart = m_emit->code.size();
							VType at;
							if( !ParseCmp( at ) ) return false;
							if( isNoiseFn && got == 0 ) {
								posArgStart = emitStart;
								posArgEnd   = m_emit->code.size();
							}
							if( got < sig->nArgs ) {
								if( at != sig->argT[got] ) {
									SetError( std::string(name) + "() argument " + std::to_string(got+1) + " must be " + ( sig->argT[got]==kVec3 ? "vec3" : "scalar" ), (ptrdiff_t)argOff );
									return false;
								}
								scalarArity += ( at == kVec3 ) ? 3 : 1;
								if( literalOctaveArg && ( literalOctaveVal < Scalar(1) || literalOctaveVal > Scalar(ExpressionProgram::kMaxOctaves) ) ) {
									SetError( std::string(name) + "() octaves must be between 1 and " + std::to_string(ExpressionProgram::kMaxOctaves), (ptrdiff_t)argOff );
									return false;
								}
								if( literalRadiusArg && got == 0 && !( literalRadiusVal > Scalar(0) ) ) {
									SetError( std::string(name) + "() radius must be > 0 -- "
										+ ( sig->id == ExpressionProgram::kFnProximity
											? std::string( "it is a WORLD LENGTH (proximity(0.002) = 2 mm), the distance "
												"at which a neighbouring object's surface stops registering as contact -- "
												"NOT a fraction of anything" )
										: sig->id == ExpressionProgram::kFnInterior
											? std::string( "it is a WORLD LENGTH (interior(0.002) = 2 mm), the depth of "
												"burial inside a neighbouring object at which the signal saturates -- "
												"NOT a fraction of anything" )
											: std::string( "it is a FRACTION of the object's own size (0.05 = 5% of its "
												"bounding-box diagonal), not a world length" ) ),
										(ptrdiff_t)argOff );
									return false;
								}
							}
							++got;
							if( Cur().t == Tok::Comma ) { Advance(); continue; }
							break;
						}
					}
					if( got != sig->nArgs ) { SetError( name + "() expects " + std::to_string(sig->nArgs) + " argument(s)", (ptrdiff_t)nameOff ); return false; }
					if( Cur().t != Tok::RP ) { SetError( "missing ) in " + name + "()", (ptrdiff_t)CurOff() ); return false; }
					Advance();
					// Remap non-literal self-signals to DynR twins; guard final arm so
					// proximity/interior (no DynR twins) keep their own ids.
					int emitId = sig->id;
					if( isSignalFn && !literalRadiusArg ) {
						emitId = ( sig->id == ExpressionProgram::kFnOcclusion ) ? ExpressionProgram::kFnOcclusionDynR
						       : ( sig->id == ExpressionProgram::kFnThickness ) ? ExpressionProgram::kFnThicknessDynR
						       : ( sig->id == ExpressionProgram::kFnConvexity ) ? ExpressionProgram::kFnConvexityDynR
						                                                       : sig->id;
					}
					Scalar fwScale = Scalar(1), fwoScale = Scalar(0);
					if( isNoiseFn ) NoiseFwScales( posArgStart, posArgEnd, fwScale, fwoScale );
					EmitFuncCall( emitId, scalarArity, sig->ret == kVec3, fwScale, fwoScale );
					if( isSignalFn ) {
						ExpressionProgram::SignalRadiusCall rec;
						rec.fn = sig->id;
						rec.radiusIsLiteral = literalRadiusArg;
						rec.radiusLiteral = literalRadiusArg ? literalRadiusVal : Scalar(0);
						m_sigCalls.push_back( rec );
					}
					outType = sig->ret;
					return true;
				}

				//! Parses an `osl_` builtin (up to OslBuiltins::kMaxArgs; validates
				//! kIntLiteralArg range at compile time; no fw domain fade).
				bool ParseOslCall( const OslBuiltins::Sig& sig, size_t nameOff, VType& outType )
				{
					const std::string name( sig.name );
					if( sig.needsHitContext && !m_contextVarsEnabled ) {
						SetError( name + "() requires surface context variables", (ptrdiff_t)nameOff );
						return false;
					}
					int got = 0;
					if( Cur().t != Tok::RP ) {
						for( ;; ) {
							const size_t argOff = CurOff();
							if( got >= sig.nArgs ) {
								SetError( name + "() expects " + std::to_string( sig.nArgs ) + " argument(s)", (ptrdiff_t)nameOff );
								return false;
							}
							const OslBuiltins::ArgKind kind = sig.kind[got];
							if( kind == OslBuiltins::kIntLiteralArg ) {
								Scalar lit = Scalar(0);
								const bool isLit = PeekLiteralScalarArg( lit );
								const int lo = sig.litMin[got], hi = sig.litMax[got];
								// A number followed by something other than `,`
								// or `)` is a syntax error, not a bad literal:
								// say so rather than blame a valid value.
								size_t strayOff = argOff;
								if( !isLit && PeekSignedNumberThenStray( strayOff ) ) {
									SetError( "missing `,` or `)` after argument " + std::to_string( got + 1 ) + " of " + name + "()",
										(ptrdiff_t)strayOff );
									return false;
								}
								if( !isLit || lit != std::floor( lit ) || lit < Scalar( lo ) || lit > Scalar( hi ) ) {
									SetError( name + "() argument " + std::to_string( got + 1 ) + " must be an integer literal from "
										+ std::to_string( lo ) + " to " + std::to_string( hi ), (ptrdiff_t)argOff );
									return false;
								}
								// L3: reject scalar-only Vector Math ops (7=Dot, 8=Distance, 9=Length) in vector-returning osl_vmath().
								if( name == "osl_vmath" && got == 0 && ( lit == Scalar( 7 ) || lit == Scalar( 8 ) || lit == Scalar( 9 ) ) ) {
									SetError( "osl_vmath() op " + std::to_string( static_cast<int>( lit ) ) +
										" is a scalar operation; use osl_vmath_scalar() instead", (ptrdiff_t)argOff );
									return false;
								}
							}
							VType at;
							if( !ParseCmp( at ) ) return false;
							const VType want = ( kind == OslBuiltins::kVec3Arg ) ? kVec3 : kScalar;
							if( at != want ) {
								SetError( name + "() argument " + std::to_string( got + 1 ) + " must be " + ( want == kVec3 ? "vec3" : "scalar" ), (ptrdiff_t)argOff );
								return false;
							}
							++got;
							if( Cur().t == Tok::Comma ) {
								const size_t commaOff = CurOff();
								Advance();
								if( Cur().t == Tok::RP ) {
									SetError( "unexpected `,` before `)` in " + name + "()", (ptrdiff_t)commaOff );
									return false;
								}
								continue;
							}
							if( Cur().t != Tok::RP ) {
								SetError( "missing `,` or `)` after argument " + std::to_string( got ) + " of " + name + "()", (ptrdiff_t)CurOff() );
								return false;
							}
							break;
						}
					}
					if( got != sig.nArgs ) { SetError( name + "() expects " + std::to_string( sig.nArgs ) + " argument(s)", (ptrdiff_t)nameOff ); return false; }
					if( Cur().t != Tok::RP ) { SetError( "missing ) in " + name + "()", (ptrdiff_t)CurOff() ); return false; }
					Advance();
					if( sig.needsHitContext ) {
						m_usesSpaceTransform = true;
					}
					if( sig.needsSceneSignals ) {
						m_usesSceneSignals = true;
					}
					if( sig.needsViewRay ) {
						m_usesViewRay = true;
					}
					if( sig.readsWorldPosition ) {
						m_readsWorldPosition = true;
					}
					if( sig.pathDependence != 0u ) {
						m_pathDependence |= sig.pathDependence;
					}
					Compiled::Instr in;
					in.op = sig.retVec3 ? Compiled::kOslV3 : Compiled::kOsl;
					in.val = Scalar(1); in.valo = Scalar(0); in.idx = -1;
					in.fn = sig.id; in.arity = OslBuiltins::ScalarArity( sig );
					m_emit->code.push_back( in );
					outType = sig.retVec3 ? kVec3 : kScalar;
					return true;
				}

				//! True if the current argument starts with an optional sign + number
				//! followed by a non-continuation token (used for ParseOslCall diagnostics).
				bool PeekSignedNumberThenStray( size_t& strayOff ) const
				{
					size_t i = m_pos;
					if( i < m_toks->size() && (*m_toks)[i].t == Tok::Op && ( (*m_toks)[i].s == "-" || (*m_toks)[i].s == "+" ) ) ++i;
					if( i >= m_toks->size() || (*m_toks)[i].t != Tok::Num ) return false;
					++i;
					if( i >= m_toks->size() ) return true;
					const Tok::T after = (*m_toks)[i].t;
					strayOff = (*m_toks)[i].off;
					return after != Tok::Comma && after != Tok::RP && after != Tok::Op && after != Tok::Dot;
				}

				//! Non-consuming peek for a bare optionally-signed numeric literal
				//! followed immediately by `,` or `)` (false = treat as dynamic).
				bool PeekLiteralScalarArg( Scalar& outVal ) const
				{
					size_t i = m_pos;
					if( i >= m_toks->size() ) return false;
					Scalar sign = Scalar(1);
					const Tok& t0 = (*m_toks)[i];
					if( t0.t == Tok::Op && ( t0.s == "-" || t0.s == "+" ) ) {
						if( t0.s == "-" ) sign = Scalar(-1);
						++i;
					}
					if( i >= m_toks->size() || (*m_toks)[i].t != Tok::Num ) return false;
					const Scalar val = (*m_toks)[i].num;
					++i;
					if( i >= m_toks->size() ) return false;
					const Tok::T after = (*m_toks)[i].t;
					if( after != Tok::Comma && after != Tok::RP ) return false;
					outVal = sign * val;
					return true;
				}

				bool ParseVec3Ctor( size_t nameOff, VType& outType )
				{
					for( int i = 0; i < 3; ++i ) {
						const size_t argOff = CurOff();
						VType at;
						if( !ParseCmp( at ) ) return false;
						if( at != kScalar ) { SetError( "vec3() arguments must be scalar", (ptrdiff_t)argOff ); return false; }
						if( i < 2 ) { if( !Expect( Tok::Comma, "vec3() expects 3 scalar arguments", nameOff ) ) return false; }
					}
					if( Cur().t != Tok::RP ) { SetError( "missing ) in vec3()", (ptrdiff_t)CurOff() ); return false; }
					Advance();
					outType = kVec3;	// no opcode: the 3 scalars are already in place on the stack
					return true;
				}

				//! Parses `sample*(painter_name[, u, v])` (docs/WETNESS_COAT_DESIGN.md).
				//! Reads a bare identifier resolved at attach time via BindPainterRefs;
				//! gated on m_contextVarsEnabled; stores ref-table index in `in.idx`.
				bool ParseSampleCall( const std::string& name, size_t nameOff, VType& outType )
				{
					const bool isScalar = ( name == "sample_scalar" );
					const bool isAlpha  = ( name == "sample_alpha" );
					const bool isUv     = ( name == "sample_uv" );
					const bool isUvAlpha = ( name == "sample_uv_alpha" );
					if( !m_contextVarsEnabled ) {
						SetError( "`" + name + "()` needs the 3D surface context -- available in expression_painter "
							"and scalar_painter { expression ... }, not in expression_function2d (a UV-only field)",
							(ptrdiff_t)nameOff );
						return false;
					}
					if( Cur().t != Tok::Ident ) {
						SetError( name + "() expects a bare painter name, e.g. `" + name + "(rock_albedo)` -- "
							"not a quoted string, a number, or a sub-expression: the name is resolved against "
							"the scene's painter chunks by name, not evaluated as a variable", (ptrdiff_t)CurOff() );
						return false;
					}
					const std::string painterName = Cur().s;
					Advance();
					if( isUv || isUvAlpha ) {
						const std::string expectUvMsg = name + "() expects (painter_name, u, v)";
						if( !Expect( Tok::Comma, expectUvMsg.c_str(), nameOff ) ) return false;
						const size_t uOff = CurOff();
						VType tu;
						if( !ParseCmp( tu ) ) return false;
						if( tu != kScalar ) { SetError( name + "() argument 2 (u) must be scalar", (ptrdiff_t)uOff ); return false; }
						if( !Expect( Tok::Comma, expectUvMsg.c_str(), nameOff ) ) return false;
						const size_t vOff = CurOff();
						VType tv;
						if( !ParseCmp( tv ) ) return false;
						if( tv != kScalar ) { SetError( name + "() argument 3 (v) must be scalar", (ptrdiff_t)vOff ); return false; }
						if( Cur().t != Tok::RP ) { SetError( "missing ) in " + name + "() -- expected (painter_name, u, v)", (ptrdiff_t)CurOff() ); return false; }
						Advance();
						const int refIdx = RegisterPainterRef( m_colorPainterRefNames, m_colorPainterRefIndex, painterName );
						Compiled::Instr in;
						in.op = isUv ? Compiled::kSampleUv : Compiled::kSampleUvAlpha;
						in.val = 0; in.valo = 0;
						in.idx = refIdx; in.fn = 0; in.arity = 2;
						m_emit->code.push_back( in );
						outType = isUv ? kVec3 : kScalar;
						return true;
					}
					if( Cur().t != Tok::RP ) { SetError( "missing ) in " + name + "() -- exactly one argument expected", (ptrdiff_t)CurOff() ); return false; }
					Advance();
					const int refIdx = isScalar
						? RegisterPainterRef( m_scalarPainterRefNames, m_scalarPainterRefIndex, painterName )
						: RegisterPainterRef( m_colorPainterRefNames, m_colorPainterRefIndex, painterName );
					if( isAlpha ) {
						Compiled::Instr in;
						in.op = Compiled::kSampleAlpha;
						in.val = 0; in.valo = 0;
						in.idx = refIdx; in.fn = 0; in.arity = 0;
						m_emit->code.push_back( in );
						outType = kScalar;
						return true;
					}
					EmitFuncCall( isScalar ? ExpressionProgram::kFnSamplePainterScalar : ExpressionProgram::kFnSamplePainter,
						/*arity=*/0, /*isVec3=*/!isScalar );
					m_emit->code.back().idx = refIdx;
					outType = isScalar ? kScalar : kVec3;
					return true;
				}

				//! Named geometry-attribute query: `mesh_attr3("name") -> vec3`
				//! and `mesh_attr_alpha("name") -> scalar`.  Reads per-corner
				//! linear RGBA from the hit geometry's ISurfaceSignalProvider.
				bool ParseMeshAttrCall( const std::string& name, size_t nameOff, VType& outType )
				{
					const bool isAlpha = ( name == "mesh_attr_alpha" );
					if( !m_contextVarsEnabled ) {
						SetError( "`" + name + "()` needs the 3D surface context -- available in expression_painter "
							"and scalar_painter { expression ... }, not in expression_function2d (a UV-only field)",
							(ptrdiff_t)nameOff );
						return false;
					}
					if( Cur().t != Tok::Str || Cur().s.empty() ) {
						SetError( name + "() expects a non-empty quoted attribute name, e.g. `" + name + "(\"distance\")`",
							(ptrdiff_t)CurOff() );
						return false;
					}
					const std::string attrName = Cur().s;
					Advance();
					if( Cur().t != Tok::RP ) {
						SetError( "missing ) in " + name + "() -- exactly one quoted attribute name expected",
							(ptrdiff_t)CurOff() );
						return false;
					}
					Advance();
					const int attrIdx = RegisterPainterRef( m_meshAttrNames, m_meshAttrIndex, attrName );
					Compiled::Instr in;
					in.op = isAlpha ? Compiled::kMeshAttrAlpha : Compiled::kMeshAttr3;
					in.val = 0; in.valo = 0;
					in.idx = attrIdx; in.fn = 0; in.arity = 0;
					m_emit->code.push_back( in );
					outType = isAlpha ? kScalar : kVec3;
					return true;
				}

				bool ParseMixCall( size_t nameOff, VType& outType )
				{
					VType t0, t1, t2;
					if( !ParseCmp( t0 ) ) return false;
					if( !Expect( Tok::Comma, "mix() expects 3 arguments", nameOff ) ) return false;
					if( !ParseCmp( t1 ) ) return false;
					if( !Expect( Tok::Comma, "mix() expects 3 arguments", nameOff ) ) return false;
					if( !ParseCmp( t2 ) ) return false;
					if( Cur().t != Tok::RP ) { SetError( "missing ) in mix()", (ptrdiff_t)CurOff() ); return false; }
					Advance();
					if( t0==kScalar && t1==kScalar && t2==kScalar ) {
						EmitFuncCall( kFnMixScalar, 3, false );
						outType = kScalar;
						return true;
					}
					if( t0==kVec3 && t1==kVec3 && t2==kScalar ) {
						EmitFuncCall( kFnMixVec3, 7, true );
						outType = kVec3;
						return true;
					}
					SetError( "mix() expects (scalar,scalar,scalar) or (vec3,vec3,scalar)", (ptrdiff_t)nameOff );
					return false;
				}

				// ramp(t, pos0,val0, pos1,val1, ...)  -- >=2 stops, value
				// type (scalar or vec3) taken from the stops and must match
				// across all of them; positions checked strictly ascending
				// at compile time wherever both neighbours are literals.
				bool ParseRampCall( size_t nameOff, VType& outType )
				{
					VType tt;
					if( !ParseCmp( tt ) ) return false;
					if( tt != kScalar ) { SetError( "ramp() first argument (t) must be scalar", (ptrdiff_t)nameOff ); return false; }
					if( !Expect( Tok::Comma, "ramp() expects t, pos0,val0, pos1,val1, ...", nameOff ) ) return false;

					int numStops = 0;
					VType valueType = kScalar;
					bool haveValueType = false;
					bool prevPosLiteral = false;
					Scalar prevPosVal = 0;
					int totalArity = 1;	// t

					for( ;; ) {
						if( numStops >= ExpressionProgram::kMaxRampStops ) { SetError( "ramp() has too many stops (limit 64)", (ptrdiff_t)nameOff ); return false; }
						const size_t posOff = CurOff();
						const bool isLitPos = ( Cur().t == Tok::Num );
						const Scalar litPosVal = isLitPos ? Cur().num : Scalar(0);
						VType pt;
						if( !ParseCmp( pt ) ) return false;
						if( pt != kScalar ) { SetError( "ramp() stop position must be scalar", (ptrdiff_t)posOff ); return false; }
						if( isLitPos ) {
							if( prevPosLiteral && !( litPosVal > prevPosVal ) ) {
								SetError( "ramp() stop positions must be strictly ascending", (ptrdiff_t)posOff );
								return false;
							}
							prevPosLiteral = true; prevPosVal = litPosVal;
						} else {
							prevPosLiteral = false;
						}
						totalArity += 1;

						if( !Expect( Tok::Comma, "ramp() stop is missing its value", posOff ) ) return false;

						const size_t valOff = CurOff();
						VType vt;
						if( !ParseCmp( vt ) ) return false;
						if( !haveValueType ) { valueType = vt; haveValueType = true; }
						else if( vt != valueType ) { SetError( "ramp() stop values must all be the same type (scalar or vec3)", (ptrdiff_t)valOff ); return false; }
						totalArity += ( vt == kVec3 ) ? 3 : 1;
						++numStops;

						if( Cur().t == Tok::Comma ) { Advance(); continue; }
						break;
					}
					if( numStops < 2 ) { SetError( "ramp() needs at least 2 stops", (ptrdiff_t)nameOff ); return false; }
					if( Cur().t != Tok::RP ) { SetError( "missing ) in ramp()", (ptrdiff_t)CurOff() ); return false; }
					Advance();

					Compiled::Instr in;
					in.op = Compiled::kRamp; in.val = 0;
					in.valo = 0;
					in.idx = numStops; in.fn = ( valueType==kVec3 ) ? 3 : 1; in.arity = totalArity;
					m_emit->code.push_back( in );
					outType = valueType;
					return true;
				}
			};

		private:
			int m_uSlot, m_vSlot, m_PSlot, m_PoSlot, m_NSlot, m_fwSlot, m_timeSlot;
			int m_curvSlot, m_curvRSlot, m_fwoSlot;
			unsigned int m_ctxUsedMask;
			std::vector<SignalRadiusCall> m_signalCalls;
			std::vector<std::string> m_colorPainterRefNames;
			std::vector<std::string> m_scalarPainterRefNames;
			std::vector<std::string> m_meshAttrNames;
			std::shared_ptr<BoundPainterRefs> m_boundRefs;
			std::vector<Scalar> m_initEnv;
			std::vector<Compiled> m_defs;
			Compiled m_final;
			bool m_valid;
			std::string m_error;
			ptrdiff_t m_errorOffset;
			unsigned long long m_id;
			bool m_memoWorthy;
			bool m_usesSpaceTransform;
			bool m_usesSceneSignals;
			bool m_usesViewRay;
			bool m_readsWorldPosition;
			unsigned int m_pathDependence;

			ExpressionProgram() :
				m_uSlot(0), m_vSlot(1), m_PSlot(kContextSlotP), m_PoSlot(kContextSlotPo), m_NSlot(8),
				m_fwSlot(kContextSlotFw), m_timeSlot(kContextSlotTime),
				m_curvSlot(kContextSlotCurv), m_curvRSlot(kContextSlotCurvR), m_fwoSlot(kContextSlotFwo),
				m_ctxUsedMask(0),
				m_valid(false), m_errorOffset(-1), m_id(0), m_memoWorthy(false), m_usesSpaceTransform(false), m_usesSceneSignals(false), m_usesViewRay(false), m_readsWorldPosition(false), m_pathDependence(0u)
			{}
			friend class Builder;

			//! Seeds env with params + context vars and runs defs [0..stopAfterDef] (-1 = all).
			void BindEnv( Scalar* env, const Scalar u, const Scalar v, const Vector3& P, const Vector3& Po, const Vector3& N, const Scalar fw, const Scalar fwo, const Scalar time, const Scalar curv, const Scalar curvR, const SurfaceSignalInfo* pSignals, const BoundPainterRefs* pPainterRefs = nullptr, int stopAfterDef = -1, const RayIntersectionGeometric* pHit = nullptr ) const
			{
				const size_t n = m_initEnv.size();
				for( size_t i = 0; i < n; ++i ) env[i] = m_initEnv[i];
				env[ m_uSlot ] = u; env[ m_vSlot ] = v;
				env[ m_PSlot+0 ] = P.x;  env[ m_PSlot+1 ] = P.y;  env[ m_PSlot+2 ] = P.z;
				env[ m_PoSlot+0 ] = Po.x; env[ m_PoSlot+1 ] = Po.y; env[ m_PoSlot+2 ] = Po.z;
				env[ m_NSlot+0 ] = N.x;  env[ m_NSlot+1 ] = N.y;  env[ m_NSlot+2 ] = N.z;
				env[ m_fwSlot ] = fw;
				env[ m_fwoSlot ] = fwo;
				env[ m_timeSlot ] = time;
				env[ m_curvSlot ] = curv;
				env[ m_curvRSlot ] = curvR;
				const size_t defLimit = ( stopAfterDef >= 0 && (size_t)stopAfterDef < m_defs.size() )
					? (size_t)stopAfterDef + 1 : m_defs.size();
				for( size_t i = 0; i < defLimit; ++i ) {
					Scalar out[3];
					RunAny( m_defs[i], env, out, pSignals, pPainterRefs, pHit, &m_meshAttrNames );
					const int w = ( m_defs[i].type == kVec3 ) ? 3 : 1;
					for( int c = 0; c < w; ++c ) env[ m_defs[i].writeSlot + c ] = out[c];
				}
			}

		public:
			static ExpressionProgram Invalid() { return ExpressionProgram(); }

			//! Saturates a runtime Scalar into [1, kMaxOctaves] before casting to int (avoids float-to-int UB).
			static int OctavesFromScalar( Scalar v )
			{
				if( !IsFinite( v ) ) return 1;
				if( v < Scalar(1) ) return 1;
				if( v > Scalar(kMaxOctaves) ) return kMaxOctaves;
				return (int)v;
			}

			//! Fallback partial hit record built from `env` when ExprEvalContext::pHit == nullptr.
			static RayIntersectionGeometric BuildSyntheticRi( const Scalar* env )
			{
				RayIntersectionGeometric ri( Ray(), nullRasterizerState );
				ri.bHit = true;
				ri.ptCoord = Point2( env[0], env[1] );
				ri.ptIntersection = Point3( env[ kContextSlotP+0 ], env[ kContextSlotP+1 ], env[ kContextSlotP+2 ] );
				ri.ptObjIntersec  = Point3( env[ kContextSlotPo+0 ], env[ kContextSlotPo+1 ], env[ kContextSlotPo+2 ] );
				ri.vNormal = Vector3( env[8+0], env[8+1], env[8+2] );
				return ri;
			}

			//! Thread-local recursion/cycle guard for nested `sample*()` calls (keyed on painter pointer).
			static const int kMaxSampleDepth = 32;
			struct SampleGuardState { const void* stack[ kMaxSampleDepth ]; int depth; };
			static SampleGuardState& ThreadLocalSampleGuard()
			{
				static thread_local SampleGuardState s = { {}, 0 };
				return s;
			}
			static bool SampleGuardEnter( const void* key )
			{
				SampleGuardState& g = ThreadLocalSampleGuard();
				for( int i = 0; i < g.depth; ++i ) if( g.stack[i] == key ) return false;
				if( g.depth >= kMaxSampleDepth ) return false;
				g.stack[ g.depth++ ] = key;
				return true;
			}
			static void SampleGuardExit()
			{
				SampleGuardState& g = ThreadLocalSampleGuard();
				if( g.depth > 0 ) --g.depth;
			}

			//! RAII wrapper ensuring balanced SampleGuardEnter/Exit on early returns and exceptions.
			class SampleGuard
			{
			public:
				explicit SampleGuard( const void* key ) : m_entered( SampleGuardEnter( key ) ) {}
				~SampleGuard() { if( m_entered ) SampleGuardExit(); }
				bool Entered() const { return m_entered; }
			private:
				SampleGuard( const SampleGuard& );
				SampleGuard& operator=( const SampleGuard& );
				const bool m_entered;
			};

			//! Scalar builtin dispatch (per-eval state passed as params to keep program stateless/const).
			static Scalar CallFunc( int fn, const Scalar* a, Scalar fw, const SurfaceSignalInfo* pSignals,
				const Scalar* env = nullptr, const BoundPainterRefs* pPainterRefs = nullptr, int idxArg = -1,
				const RayIntersectionGeometric* pHit = nullptr )
			{
				switch( fn )
				{
				case 1:  return std::sin(a[0]);
				case 2:  return std::cos(a[0]);
				case 3:  return std::tan(a[0]);
				case 4:  return std::asin(a[0]);
				case 5:  return std::acos(a[0]);
				case 6:  return std::atan(a[0]);
				case 7:  return std::exp(a[0]);
				case 8:  return std::log(a[0]);
				case 9:  return std::sqrt(a[0]);
				case 10: return std::fabs(a[0]);
				case 11: return std::floor(a[0]);
				case 12: return std::ceil(a[0]);
				case 13: return a[0] - std::floor(a[0]);				// frac
				case 14: return ( a[0] > 0 ) ? Scalar(1) : ( a[0] < 0 ? Scalar(-1) : Scalar(0) );
				case 20: return std::atan2(a[0],a[1]);
				case 21: { const Scalar m = a[1]; return ( m != 0 ) ? ( a[0] - std::floor( a[0]/m ) * m ) : Scalar(0); }	// floor-mod
				case 22: return std::min(a[0],a[1]);
				case 23: return std::max(a[0],a[1]);
				case 24: return std::pow(a[0],a[1]);
				case 25: return std::sqrt(a[0]*a[0]+a[1]*a[1]);		// hypot (ffast-math-safe form)
				case 26: return ( a[1] < a[0] ) ? Scalar(0) : Scalar(1);	// step(edge, x)
				case 30: return std::min( std::max( a[0], a[1] ), a[2] );	// clamp(x,lo,hi)
				case 31: { const Scalar t = std::min( std::max( ( a[2]-a[0] )/( a[1]-a[0] ), Scalar(0) ), Scalar(1) ); return t*t*(Scalar(3)-Scalar(2)*t); }	// smoothstep(e0,e1,x)
				case 32: return a[0] + ( a[1]-a[0] ) * a[2];			// mix(a,b,t)
				case 33: return ( a[0] != 0 ) ? a[1] : a[2];			// select(cond,a,b)
				case 40: return a[0]*a[3] + a[1]*a[4] + a[2]*a[5];		// dot(a,b)
				case 41: return std::sqrt( a[0]*a[0]+a[1]*a[1]+a[2]*a[2] );	// length(v)
				case 42: return NoiseCore::PerlinOctave3D( a[0], a[1], a[2] );
				// `fw` arrives pre-scaled into this call site's domain (`sP*fw + sPo*fwo`).
				case 43: return NoiseCore::Fbm3D( a[0],a[1],a[2], OctavesFromScalar(a[3]), a[4], a[5], fw );
				case 44: return NoiseCore::Turbulence3D( a[0],a[1],a[2], OctavesFromScalar(a[3]), a[4], a[5], fw );
				case 45: return NoiseCore::Ridged3D( a[0],a[1],a[2], OctavesFromScalar(a[3]), a[4], a[5], fw );
				case 46: case 47: case 48:
				{
					Scalar f1, f2; int cx,cy,cz;
					NoiseCore::WorleySample3D( a[0],a[1],a[2], a[3], NoiseCore::eMetricEuclidean, f1, f2, cx, cy, cz );
					if( fn == 46 ) return NoiseCore::WorleyNormalize( f1, NoiseCore::eMetricEuclidean, NoiseCore::eModeF1 );
					if( fn == 47 ) return NoiseCore::WorleyNormalize( f2, NoiseCore::eMetricEuclidean, NoiseCore::eModeF2 );
					return NoiseCore::WorleyNormalize( f2-f1, NoiseCore::eMetricEuclidean, NoiseCore::eModeF2MinusF1 );
				}
				case 49:
				{
					Scalar f1, f2; int cx,cy,cz;
					NoiseCore::WorleySample3D( a[0],a[1],a[2], a[3], NoiseCore::eMetricEuclidean, f1, f2, cx, cy, cz );
					return NoiseCore::WorleyIdOf( cx, cy, cz );
				}
				case 50: return NoiseCore::CellHash( a[0] );
				// Geometry-derived self-signals (fractional radius; DynR = non-literal radius).
				case kFnOcclusion:
					return pSignals ? pSignals->Occlusion( a[0], true ) : SurfaceSignalInfo::NeutralOcclusion();
				case kFnOcclusionDynR:
					return pSignals ? pSignals->Occlusion( a[0], false ) : SurfaceSignalInfo::NeutralOcclusion();
				case kFnThickness:
					return pSignals ? pSignals->Thickness( a[0], true ) : SurfaceSignalInfo::NeutralThickness();
				case kFnThicknessDynR:
					return pSignals ? pSignals->Thickness( a[0], false ) : SurfaceSignalInfo::NeutralThickness();
				case kFnConvexity:
					return pSignals ? pSignals->Convexity( a[0], true ) : SurfaceSignalInfo::NeutralConvexity();
				case kFnConvexityDynR:
					return pSignals ? pSignals->Convexity( a[0], false ) : SurfaceSignalInfo::NeutralConvexity();
				// Cross-object signals (world-length radius; live scan, no DynR twins).
				case kFnProximity:
					return pSignals ? pSignals->Proximity( a[0] ) : SurfaceSignalInfo::NeutralProximity();
				case kFnInterior:
					return pSignals ? pSignals->Interior( a[0] ) : SurfaceSignalInfo::NeutralInterior();
				// sample_scalar(name): IScalarPainter at kSingleSampleChannel (549 nm green lane).
				case kFnSamplePainterScalar:
				{
					if( !pPainterRefs || !env ) return Scalar(0);
					if( idxArg < 0 || (std::size_t)idxArg >= pPainterRefs->scalar.size() ) return Scalar(0);
					IScalarPainter* p = pPainterRefs->scalar[ (std::size_t)idxArg ];
					if( !p ) return Scalar(0);
					const SampleGuard guard( p );
					if( !guard.Entered() ) return Scalar(0);
					Scalar v = Scalar(0);
					if( pHit ) {
						v = p->GetValuesAt( *pHit ).v[ ScalarPainterRGB::kSingleSampleChannel ];
					} else {
						const RayIntersectionGeometric ri = BuildSyntheticRi( env );
						v = p->GetValuesAt( ri ).v[ ScalarPainterRGB::kSingleSampleChannel ];
					}
					return IsFinite( v ) ? v : Scalar(0);
				}
				default: return Scalar(0);
				}
			}

			//! Vec3 builtin dispatch (does not consume fw/fwo domain scales).
			static void CallFuncVec3( int fn, const Scalar* a, Scalar* out, const SurfaceSignalInfo* pSignals,
				const Scalar* env = nullptr, const BoundPainterRefs* pPainterRefs = nullptr, int idxArg = -1,
				const RayIntersectionGeometric* pHit = nullptr )
			{
				(void)pSignals;
				switch( fn )
				{
				case 60:	// cross(a,b)
					out[0] = a[1]*a[5] - a[2]*a[4];
					out[1] = a[2]*a[3] - a[0]*a[5];
					out[2] = a[0]*a[4] - a[1]*a[3];
					break;
				case 61:	// normalize(v)
				{
					const Scalar len2 = a[0]*a[0]+a[1]*a[1]+a[2]*a[2];
					const Scalar invLen = ( len2 > Scalar(1e-24) ) ? ( Scalar(1) / std::sqrt(len2) ) : Scalar(0);
					out[0] = a[0]*invLen; out[1] = a[1]*invLen; out[2] = a[2]*invLen;
					break;
				}
				case 62:	// mix(vec3 a, vec3 b, scalar t)
					out[0] = a[0] + (a[3]-a[0]) * a[6];
					out[1] = a[1] + (a[4]-a[1]) * a[6];
					out[2] = a[2] + (a[5]-a[2]) * a[6];
					break;
				case kFnSamplePainter:
				{
					out[0] = out[1] = out[2] = Scalar(0);
					if( !pPainterRefs || !env ) break;
					if( idxArg < 0 || (std::size_t)idxArg >= pPainterRefs->color.size() ) break;
					IPainter* p = pPainterRefs->color[ (std::size_t)idxArg ];
					if( !p ) break;
					const SampleGuard guard( p );
					if( !guard.Entered() ) break;
					RISEPel c( 0, 0, 0 );
					if( pHit ) {
						c = p->GetColor( *pHit );
					} else {
						const RayIntersectionGeometric ri = BuildSyntheticRi( env );
						c = p->GetColor( ri );
					}
					out[0] = IsFinite( c[0] ) ? c[0] : Scalar(0);
					out[1] = IsFinite( c[1] ) ? c[1] : Scalar(0);
					out[2] = IsFinite( c[2] ) ? c[2] : Scalar(0);
					break;
				}
				default:
					out[0] = out[1] = out[2] = Scalar(0);
					break;
				}
			}

		private:
			static void RunAny( const Compiled& c, const Scalar* env, Scalar* out, const SurfaceSignalInfo* pSignals,
				const BoundPainterRefs* pPainterRefs = nullptr, const RayIntersectionGeometric* pHit = nullptr,
				const std::vector<std::string>* pMeshAttrNames = nullptr )
			{
				Scalar stack[ kStackCap ];
				int sp = 0;
				for( size_t i = 0; i < c.code.size(); ++i ) {
					const Compiled::Instr& in = c.code[i];
					switch( in.op )
					{
					case Compiled::kConst: stack[sp++] = in.val; break;
					case Compiled::kVar:   stack[sp++] = env[ in.idx ]; break;
					case Compiled::kNeg:   stack[sp-1] = -stack[sp-1]; break;
					case Compiled::kAdd:   stack[sp-2] = stack[sp-2] + stack[sp-1]; --sp; break;
					case Compiled::kSub:   stack[sp-2] = stack[sp-2] - stack[sp-1]; --sp; break;
					case Compiled::kMul:   stack[sp-2] = stack[sp-2] * stack[sp-1]; --sp; break;
					case Compiled::kDiv:   stack[sp-2] = ( stack[sp-1] != 0 ) ? ( stack[sp-2] / stack[sp-1] ) : Scalar(0); --sp; break;
					case Compiled::kMod:   { const Scalar m = stack[sp-1]; stack[sp-2] = ( m != 0 ) ? ( stack[sp-2] - std::floor( stack[sp-2]/m ) * m ) : Scalar(0); --sp; } break;
					case Compiled::kPow:   stack[sp-2] = std::pow( stack[sp-2], stack[sp-1] ); --sp; break;
					case Compiled::kLt:    stack[sp-2] = ( stack[sp-2] <  stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kGt:    stack[sp-2] = ( stack[sp-2] >  stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kLe:    stack[sp-2] = ( stack[sp-2] <= stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kGe:    stack[sp-2] = ( stack[sp-2] >= stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kEq:    stack[sp-2] = ( stack[sp-2] == stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kNe:    stack[sp-2] = ( stack[sp-2] != stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kFunc:
					{
						const int ar = in.arity;
						sp -= ar;
						// Combine world `fw` and object `fwo` with compile-time domain scales.
						stack[sp] = CallFunc( in.fn, &stack[sp],
							env[ kContextSlotFw ] * in.val + env[ kContextSlotFwo ] * in.valo, pSignals,
							env, pPainterRefs, in.idx, pHit );
						++sp;
					} break;
					case Compiled::kFuncV3:
					{
						const int ar = in.arity;
						sp -= ar;
						Scalar out3[3];
						CallFuncVec3( in.fn, &stack[sp], out3, pSignals, env, pPainterRefs, in.idx, pHit );
						stack[sp] = out3[0]; stack[sp+1] = out3[1]; stack[sp+2] = out3[2];
						sp += 3;
					} break;
					case Compiled::kSplat3:
					{
						const Scalar v = stack[sp-1];
						stack[sp] = v; stack[sp+1] = v;
						sp += 2;
					} break;
					case Compiled::kAddV: for(int k=0;k<3;++k) stack[sp-6+k] = stack[sp-6+k] + stack[sp-3+k]; sp -= 3; break;
					case Compiled::kSubV: for(int k=0;k<3;++k) stack[sp-6+k] = stack[sp-6+k] - stack[sp-3+k]; sp -= 3; break;
					case Compiled::kMulV: for(int k=0;k<3;++k) stack[sp-6+k] = stack[sp-6+k] * stack[sp-3+k]; sp -= 3; break;
					case Compiled::kDivV: for(int k=0;k<3;++k) { const Scalar d = stack[sp-3+k]; stack[sp-6+k] = ( d != 0 ) ? ( stack[sp-6+k] / d ) : Scalar(0); } sp -= 3; break;
					case Compiled::kNegV: stack[sp-3] = -stack[sp-3]; stack[sp-2] = -stack[sp-2]; stack[sp-1] = -stack[sp-1]; break;
					case Compiled::kSwizzle:
					{
						const Scalar v = stack[sp-3+in.idx];
						sp -= 3;
						stack[sp++] = v;
					} break;
					case Compiled::kRamp:
					{
						const int numStops = in.idx;
						const int W = in.fn;
						const int ar = in.arity;
						sp -= ar;
						const Scalar* a = &stack[sp];
						const Scalar t = a[0];
						const int stride = 1 + W;
						const int outBase = sp;
						const Scalar firstPos = a[1];
						const Scalar lastPos = a[ 1 + (numStops-1)*stride ];
						if( t <= firstPos ) {
							for( int c = 0; c < W; ++c ) stack[outBase+c] = a[1+1+c];
						} else if( t >= lastPos ) {
							const int base = 1 + (numStops-1)*stride;
							for( int c = 0; c < W; ++c ) stack[outBase+c] = a[base+1+c];
						} else {
							int seg = 0;
							for( int s = 0; s < numStops-1; ++s ) {
								const Scalar p0 = a[1+s*stride];
								const Scalar p1 = a[1+(s+1)*stride];
								if( t >= p0 && t <= p1 ) { seg = s; break; }
							}
							const Scalar p0 = a[1+seg*stride];
							const Scalar p1 = a[1+(seg+1)*stride];
							const Scalar denom = p1 - p0;
							Scalar frac = ( denom != 0 ) ? ( t - p0 ) / denom : Scalar(0);
							// Clamp against non-monotonic runtime-computed stop positions.
							if( frac < Scalar(0) ) frac = Scalar(0);
							if( frac > Scalar(1) ) frac = Scalar(1);
							for( int c = 0; c < W; ++c ) {
								const Scalar v0 = a[1+seg*stride+1+c];
								const Scalar v1 = a[1+(seg+1)*stride+1+c];
								stack[outBase+c] = v0 + ( v1 - v0 ) * frac;
							}
						}
						sp = outBase + W;
					} break;
					case Compiled::kOsl:
					{
						sp -= in.arity;
						Scalar r[3];
						const Vector3 fallbackN( env[8], env[9], env[10] );
						OslBuiltins::Call( in.fn, &stack[sp], r, pHit, pSignals, &fallbackN );
						stack[sp++] = r[0];
					} break;
					case Compiled::kOslV3:
					{
						sp -= in.arity;
						Scalar r[3];
						const Vector3 fallbackN( env[8], env[9], env[10] );
						OslBuiltins::Call( in.fn, &stack[sp], r, pHit, pSignals, &fallbackN );
						stack[sp] = r[0]; stack[sp+1] = r[1]; stack[sp+2] = r[2];
						sp += 3;
					} break;
					case Compiled::kMeshAttr3:
					case Compiled::kMeshAttrAlpha:
					{
						Scalar rgba[4] = { Scalar(0), Scalar(0), Scalar(0), Scalar(0) };
						if( pSignals && pMeshAttrNames && in.idx >= 0 && (size_t)in.idx < pMeshAttrNames->size() ) {
							const Point3* pQueryObjPos = 0;
							Point3 queryObj;
							if( pHit && ( pHit->vReliefObjStep.x != Scalar(0) ||
								pHit->vReliefObjStep.y != Scalar(0) ||
								pHit->vReliefObjStep.z != Scalar(0) ) )
							{
								queryObj = Point3Ops::mkPoint3( pSignals->ptObject, pHit->vReliefObjStep );
								pQueryObjPos = &queryObj;
							}
							pSignals->NamedAttribute( (*pMeshAttrNames)[(size_t)in.idx].c_str(), rgba, pQueryObjPos );
						}
						if( in.op == Compiled::kMeshAttr3 ) {
							stack[sp] = rgba[0]; stack[sp+1] = rgba[1]; stack[sp+2] = rgba[2];
							sp += 3;
						} else {
							stack[sp++] = rgba[3];
						}
					} break;
					case Compiled::kSampleAlpha:
					{
						Scalar alpha = Scalar(1);
						if( pPainterRefs && in.idx >= 0 && (std::size_t)in.idx < pPainterRefs->color.size() ) {
							IPainter* p = pPainterRefs->color[ (std::size_t)in.idx ];
							if( p ) {
								const SampleGuard guard( p );
								if( guard.Entered() ) {
									const Scalar a = pHit ? p->GetAlpha( *pHit ) : p->GetAlpha( BuildSyntheticRi( env ) );
									alpha = IsFinite( a ) ? a : Scalar(0);
								}
							}
						}
						stack[sp++] = alpha;
					} break;
					case Compiled::kSampleUv:
					case Compiled::kSampleUvAlpha:
					{
						const Scalar sv = stack[sp - 1];
						const Scalar su = stack[sp - 2];
						sp -= 2;
						const Scalar uSafe = IsFinite( su ) ? su : Scalar(0);
						const Scalar vSafe = IsFinite( sv ) ? sv : Scalar(0);
						if( in.op == Compiled::kSampleUv ) {
							Scalar r0 = Scalar(0), r1 = Scalar(0), r2 = Scalar(0);
							if( pPainterRefs && in.idx >= 0 && (std::size_t)in.idx < pPainterRefs->color.size() ) {
								IPainter* p = pPainterRefs->color[ (std::size_t)in.idx ];
								if( p ) {
									const SampleGuard guard( p );
									if( guard.Entered() ) {
										RayIntersectionGeometric riUv = pHit ? *pHit : BuildSyntheticRi( env );
										riUv.ptCoord = Point2( uSafe, vSafe );
										riUv.txFootprint.valid = false;
										const RISEPel col = p->GetColor( riUv );
										r0 = IsFinite( col[0] ) ? col[0] : Scalar(0);
										r1 = IsFinite( col[1] ) ? col[1] : Scalar(0);
										r2 = IsFinite( col[2] ) ? col[2] : Scalar(0);
									}
								}
							}
							stack[sp] = r0; stack[sp+1] = r1; stack[sp+2] = r2;
							sp += 3;
						} else {
							Scalar alpha = Scalar(1);
							if( pPainterRefs && in.idx >= 0 && (std::size_t)in.idx < pPainterRefs->color.size() ) {
								IPainter* p = pPainterRefs->color[ (std::size_t)in.idx ];
								if( p ) {
									const SampleGuard guard( p );
									if( guard.Entered() ) {
										RayIntersectionGeometric riUv = pHit ? *pHit : BuildSyntheticRi( env );
										riUv.ptCoord = Point2( uSafe, vSafe );
										riUv.txFootprint.valid = false;
										const Scalar a = p->GetAlpha( riUv );
										alpha = IsFinite( a ) ? a : Scalar(0);
									}
								}
							}
							stack[sp++] = alpha;
						}
					} break;
					}
				}
				const int w = ( c.type == kVec3 ) ? 3 : 1;
				for( int k = 0; k < w; ++k ) out[k] = ( sp >= w ) ? stack[sp-w+k] : Scalar(0);
			}
		};
	}
}

#endif
