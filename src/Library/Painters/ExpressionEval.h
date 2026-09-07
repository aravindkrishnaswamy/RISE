//////////////////////////////////////////////////////////////////////
//
//  ExpressionEval.h - A small, self-contained math-expression engine
//  for procedural fields authored IN the scene file.
//
//  It compiles an expression string ONCE (recursive-descent parse to a
//  flat postfix instruction list) and evaluates it fast per query with
//  a stack machine -- no per-eval allocation, no AST pointer chasing,
//  no runtime type tag: every instruction's scalar-vs-vec3 shape is
//  fixed at compile time, so Eval never branches on "what type is this
//  value" -- it just moves the right number of Scalars around a flat
//  Scalar stack.  A compiled program's Eval methods are const and touch
//  no mutable state, so one compiled program is safe to call from many
//  render threads concurrently (each call gets its own stack-local
//  env/stack arrays).
//
//  Variables:  u, v            -- the query coordinates (always present)
//              P, Po, N        -- vec3 context: world position, object
//                                 position, shading normal (0 unless the
//                                 caller supplies an ExprEvalContext)
//              curv, curvR     -- scalar surface curvature at the hit.
//                                 `curvR` is the raw signed MEAN curvature
//                                 in 1/world-length; `curv` is the same
//                                 value normalized by the hit geometry's
//                                 world bounding-box diagonal, so it reads
//                                 O(1) at object scale on any scene scale.
//                                 POSITIVE = convex, negative = concave,
//                                 0 = flat -- and 0 is also the honest
//                                 "this geometry reports no curvature"
//                                 answer (planar primitives, patch stubs),
//                                 the same convention fw uses.  Computed
//                                 from the GEOMETRIC normal field, so a
//                                 bump / normal map does not move them and
//                                 `curv` and `N` legitimately disagree on a
//                                 bump-mapped surface.
//              fw              -- scalar filter width, a WORLD-space
//                                 length in the same units as `P`; 0.0 =
//                                 point sample, no filter info -- the
//                                 honest answer on any surface that
//                                 doesn't populate a footprint (secondary
//                                 bounces, non-pinhole cameras -- doc 88
//                                 S9).  fbm/turbulence/ridged rescale it
//                                 into their OWN domain automatically
//                                 (see those builtins below), so a body
//                                 that scales the position argument does
//                                 NOT have to compensate by hand.
//              time            -- scalar; 0.0 unless supplied
//              + any named `params` (constants) and `defs` (named
//                sub-expressions / let-bindings) registered before
//                compiling.  A def may reference u, v, the context
//                vars, earlier params, and earlier defs -- so a complex
//                field (e.g. guilloche) is built up as readable named
//                steps instead of one giant string.  A def's type
//                (scalar or vec3) is inferred from its own body.
//  Types:      scalar and vec3.  `vec3(x,y,z)` constructs one from
//              three scalars; `.x`/`.y`/`.z` (postfix, e.g. `P.x`)
//              extracts a component back to scalar.  Componentwise
//              + - * (with scalar broadcast) apply to vec3; `dot`,
//              `cross`, `length`, `normalize` are provided as
//              functions.  Every OTHER builtin (trig, noise, ramp's
//              position/`t` argument, comparisons, `^`, `%`) is
//              scalar-only -- passing a vec3 is a COMPILE error.
//              `mix` is the one polymorphic builtin: (scalar,scalar,
//              scalar) or (vec3,vec3,scalar).
//  Operators:  + - * / %  ^(right-assoc)  unary -  and the comparisons
//              < > <= >= == !=  (yield 1.0 / 0.0; scalar operands only).
//  Functions:  sin cos tan asin acos atan exp log sqrt abs floor ceil
//              frac sign  (unary, scalar);  atan2 mod min max pow
//              hypot step (binary, scalar);  clamp smoothstep select
//              (ternary, scalar);  mix (polymorphic, see above).
//              vec3: dot(v,v)->s, cross(v,v)->v, length(v)->s,
//              normalize(v)->v.
//              Noise (all in ProceduralNoiseCore.h -- shared with
//              Perlin3DPainter/Worley3DPainter, no separate lattice
//              math lives here): perlin(v)->s in [-1,1]; fbm/turbulence/
//              ridged(v,octaves,gain,lacunarity)->s (octaves clamped to
//              [1,10], validated at compile time when written as a
//              literal) -- these three implicitly read the context `fw`
//              (doc 88 S9) and fade an octave's contribution toward 0 as
//              the footprint * lacunarity^octave approaches/exceeds the
//              Nyquist cutoff, killing shimmer from footprint-
//              unresolvable detail; fw==0 (the default, and the only
//              value pre-S9) reproduces the un-faded sum exactly.
//              DOMAIN SCALE IS AUTOMATIC: the compiler differentiates
//              the position argument with respect to `P` and folds the
//              largest singular value of that Jacobian into a per-call-
//              site multiplier on `fw`, so `fbm(P*40, ...)` fades at a
//              domain footprint of 40*fw and `fbm(P, ...)` at exactly
//              fw (multiplier 1.0, bit-identical to a hand-passed fw).
//              The analysis covers everything AFFINE in P -- P, P.x/y/z,
//              vec3(), literals, `param`/`def` names, unary -, + - * /
//              by a compile-time constant.  An argument built from
//              anything else (u/v/Po/N, a noise warp, ^ or %, a call)
//              contributes no derivative: a SUM keeps the affine part's
//              scale (so `fbm(P*8 + warp*fbm(...), ...)` still fades at
//              8*fw) and an argument with no affine part at all falls
//              back to multiplier 1.0, the pre-2026-09-06 behaviour.
//              An argument PROVABLY independent of P (a literal vec3)
//              gets multiplier 0 -- it cannot alias, so it never fades.
//              worley_f1/f2/f2f1/id(v,jitter)->s; cellhash(s)
//              GEOMETRY SIGNALS (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
//              Phase 2): occlusion(radius)->s in [0,1], 1 = unoccluded;
//              thickness(radius)->s in [0,1], 1 = thick.  `radius` is a
//              FRACTION of the hit geometry's characteristic size (its
//              bounding-box diagonal), so occlusion(0.05) reads as "5 %
//              of the object" on any scene scale.  Both are LAZY -- they
//              cost nothing unless the body calls them -- and they answer
//              only on geometry that publishes an ISurfaceSignalProvider
//              (the SDF family today; meshes in Phase 3).  Anywhere else,
//              and for any radius <= 0, they return their documented
//              NEUTRAL values (occlusion 1 = unoccluded, thickness 1 =
//              thick), the same honest-absence convention fw and curv
//              use.  A LITERAL radius <= 0 is a COMPILE error.
//              ->s.  ramp(t, pos0,val0, pos1,val1, ...)->(scalar or
//              vec3, matching the stop values) -- variadic, >=2 stops,
//              clamped ends, positions checked ascending at compile
//              time when literal.
//
//  Designed for displacement bakes (compiled once, ~10^5 evals at parse
//  time) and small procedural textures.  Not a hot per-sample path
//  replacement for a hand-written C++ field, by design -- it trades a
//  little speed for authoring patterns without recompiling the engine.
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
#include <cmath>
#include <cstdlib>
#include "../Utilities/Math3D/Math3D.h"	// Scalar, Vector3
#include "../Utilities/FiniteMath.h"
#include "../Utilities/ProceduralNoiseCore.h"
#include "../Interfaces/ISurfaceSignalProvider.h"	// occlusion()/thickness() dispatch channel

namespace RISE
{
	namespace Implementation
	{
		//! Extra per-eval inputs beyond (u,v) -- world/object position,
		//! shading normal, filter width, time.  Everything defaults to
		//! zero, matching what the plain Eval(u,v) overload implicitly
		//! supplies (so an expression that only reads u,v behaves
		//! identically whichever overload evaluates it).
		struct ExprEvalContext
		{
			Scalar	u, v;
			Vector3	P;
			Vector3	Po;
			Vector3	N;
			Scalar	fw;
			Scalar	time;
			//! Signed MEAN curvature at the hit (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
			//! 5.1-5.2).  POSITIVE = convex, negative = concave, 0 = flat -- and 0 is
			//! also the honest "this geometry reports no curvature" answer, exactly
			//! like fw's 0.  `curvR` is raw, in 1/world-length; `curv` is the same
			//! value multiplied by the hit geometry's world bounding-box diagonal, so
			//! clamp(curv,0,1) is an edge-wear mask and clamp(-curv,0,1) a crevice
			//! mask at ANY scene scale.  Both come from the GEOMETRIC normal field
			//! and are therefore invariant under bump / normal maps.
			Scalar	curv;
			Scalar	curvR;

			//! The `occlusion()` / `thickness()` dispatch channel for THIS
			//! hit (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.1) -- the
			//! geometry's signal provider plus the object-space point and
			//! normal to query it at.
			//!
			//! Unlike every field above it, this is NOT bound into an env
			//! slot: these are ARG-TAKING BUILTINS, evaluated lazily when
			//! the body calls them, not context variables computed up
			//! front.  It is threaded to the evaluator as a separate
			//! `const SurfaceSignalInfo*` alongside `env` (see Eval /
			//! RunAny / CallFunc), which is what keeps the compiled
			//! program itself stateless and `const`: nothing per-hit is
			//! ever stored on the program, so one program stays safe to
			//! evaluate from every render thread at once.
			//!
			//! Default-constructed (no provider) reads as the honest
			//! "this surface publishes no signals" and the builtins return
			//! their neutral values -- so an expression is evaluable
			//! exactly as before wherever no geometry answers.
			SurfaceSignalInfo	signals;

			ExprEvalContext() :
				u(0), v(0), P(0,0,0), Po(0,0,0), N(0,0,0), fw(0), time(0), curv(0), curvR(0)
			{}
			ExprEvalContext( const Scalar u_, const Scalar v_ ) :
				u(u_), v(v_), P(0,0,0), Po(0,0,0), N(0,0,0), fw(0), time(0), curv(0), curvR(0)
			{}
		};

		//! Compiled program over a shared variable environment.  Build it
		//! through ExpressionProgram::Builder: register params + defs, then
		//! Finalize an expression.  Eval sets the context vars and runs
		//! every def in order followed by the final expression.
		class ExpressionProgram
		{
		public:
			//! scalar vs vec3, fixed at compile time for every slot/def/
			//! final expression -- Eval never inspects this; it only
			//! drives how many env/stack slots something occupies.
			enum VType { kScalar = 0, kVec3 = 1 };

			//! One compiled expression: a postfix instruction list referencing
			//! environment slots (variables) by index.
			struct Compiled
			{
				enum Op {
					kConst, kVar, kAdd, kSub, kMul, kDiv, kMod, kPow, kNeg,
					kLt, kGt, kLe, kGe, kEq, kNe, kFunc,
					// vec3 arithmetic -- operands are pre-broadcast to vec3
					// (kSplat3) by the compiler when one side is scalar, so
					// these always see two matching 3-wide operands.
					kSplat3, kAddV, kSubV, kMulV, kDivV, kNegV,
					kSwizzle,			//!< vec3 -> scalar component select (idx = 0/1/2)
					kFuncV3,			//!< function call returning vec3 (cross/normalize/vec3-mix)
					kRamp				//!< variadic ramp(t, pos,val, ...) -- see ParseRampCall
				};
				//! `arity` = scalars popped, meaningful (and required) for
				//! kFunc/kFuncV3/kRamp only -- every other op's stack effect
				//! is implied by the opcode itself, so this replaces the
				//! historical "recover arity from the function id band"
				//! scheme (banding could not express dot()'s 6-scalar arity
				//! or ramp()'s variadic arity).  For kRamp, `idx` holds the
				//! stop count and `fn` holds the per-stop value width (1 or
				//! 3) instead of a function id.
				//!
				//! `val` is the literal for kConst.  On a kFunc it carries
				//! that call site's `fw` MULTIPLIER instead (1.0 for every
				//! builtin but fbm/turbulence/ridged, and for those too
				//! whenever the domain scale is not provable) -- see
				//! Builder::NoiseFwScale.  It rides on the INSTRUCTION
				//! rather than on the program because it is a property of
				//! one call site: two fbm() calls in the same body can sit
				//! in differently-scaled domains.
				struct Instr { Op op; Scalar val; int idx; int fn; int arity; };
				std::vector<Instr> code;
				int writeSlot;	//!< env slot this expression writes (a def), or -1 (the final expr)
				VType type;		//!< result type of this compiled expression

				// A default-constructed Compiled (e.g. ExpressionProgram::m_final before
				// Finalize ever runs, as in ExpressionProgram::Invalid()) used to leave
				// writeSlot/type as indeterminate memory -- a caller that reads
				// ResultType()/EvalVec3() on an invalid/unfinalized program was reading
				// uninitialized state (P2-C, review round 1).  kScalar/-1 match what an
				// empty, never-compiled program should report.
				Compiled() : writeSlot(-1), type(kScalar) {}
			};

			//! One `occlusion(...)` / `thickness(...)` CALL SITE in this
			//! program, recorded at compile time (design doc §7.1's
			//! constant-radius contract, whose consumer is Phase 3's baked
			//! mesh path).
			//!
			//! `radiusIsLiteral` is TRUE only when the radius argument was a
			//! bare numeric literal (optionally signed) -- see
			//! Builder::PeekLiteralScalarArg for exactly how narrow that
			//! claim is.  FALSE never means "not constant"; it means "this
			//! compiler did not prove it constant", which the baked path must
			//! treat as dynamic (and therefore, per §7.1's mismatch contract,
			//! answer with the signal's neutral fallback rather than
			//! substituting the baked radius).
			struct SignalRadiusCall
			{
				int    fn;				//!< kFnOcclusion or kFnThickness
				bool   radiusIsLiteral;
				Scalar radiusLiteral;	//!< meaningful only when radiusIsLiteral
			};

			//! Slot / stack / parse-depth caps -- a compiled program is
			//! rejected at build time if it would exceed them, so Eval can use
			//! fixed stack-allocated buffers (no per-call heap, no overflow).
			static const int kMaxSlots      = 512;	//!< named vars (context + params + defs), each scalar=1/vec3=3 slots
			static const int kStackCap      = 512;	//!< value-stack depth
			static const int kMaxParseDepth = 200;	//!< recursive-descent nesting
			static const int kMaxOctaves    = NoiseCore::kMaxOctaves;
			static const int kMaxRampStops  = 64;

			//! Function ids of the two GEOMETRY-DERIVED SIGNAL builtins
			//! (design doc Phase 2).  Named constants rather than bare
			//! numbers because three places must agree on them -- the FnSig
			//! table, CallFunc's cases, and ParseCall's per-argument
			//! literal-radius validation -- and a silent mismatch between
			//! them would mis-route a call to a different builtin.  They
			//! continue the scalar-returning id band (cellhash = 50).
			static const int kFnOcclusion   = 51;
			static const int kFnThickness   = 52;
			//! INTERNAL twins of the two ids above, emitted in their place
			//! when the compiler could NOT prove the radius argument a bare
			//! numeric literal.  They are not builtin NAMES -- nothing in
			//! the FnSig table maps to them and no scene text can spell
			//! them; they exist only so the emitted instruction carries the
			//! compile-time proof down to CallFunc, which forwards it to
			//! SurfaceSignalInfo::Occlusion / ::Thickness.
			//!
			//! Why a second id rather than a flag on the instruction: the
			//! proof IS part of which operation this call site performs (a
			//! baked provider answers one and refuses the other), the
			//! dispatch is already a switch on the id, and this way no
			//! signature in the evaluator changes -- so no future builtin
			//! can accidentally inherit a stale "constant" bit from a
			//! neighbouring field.  See ISurfaceSignalProvider's
			//! `bRadiusIsConstant` note for why the proof must travel at
			//! all.
			static const int kFnOcclusionDynR = 53;
			static const int kFnThicknessDynR = 54;
			//! Reserved context-variable slot layout (env[0..kContextSlotCount-1]):
			//!   u=0, v=1, P=2..4, Po=5..7, N=8..10, fw=kContextSlotFw(11),
			//!   time=kContextSlotTime(12), curv=kContextSlotCurv(13),
			//!   curvR=kContextSlotCurvR(14).
			//! P2-a (S9 review round 1): this block is the SOLE place the slot
			//! indices and the reserved-slot count are spelled out.  EVERY site
			//! below references these constants instead of carrying its own copy
			//! of the literals -- and every one of them must be touched when a
			//! context var is added:
			//!   1. LookupContextVar            (name -> slot/type)
			//!   2. Builder's ctor              (m_names reserve, kContextSlotCount)
			//!   3. Builder::Finalize           (the out.m_*Slot pins + the
			//!                                   "N are reserved for ..." error text)
			//!   4. ExpressionProgram's member list + default member init
			//!   5. BindEnv                     (the ONE definition) and ALL FOUR
			//!      of its call sites: Eval(u,v), Eval(ctx), EvalVec3(ctx), and
			//!      EvalDefStage(ctx,...) -- the last is the easy one to miss
			//!   6. ExprEvalContext             (field + both ctors)
			//!   7. RunAny/CallFunc's static (no `this`) fw read, if the new var
			//!      is one a builtin reads implicitly
			//! Adding one WITHOUT touching all of them is exactly the silent
			//! desynchronization this comment exists to prevent.
			//!
			//! kContextSlotP is the first of P's three slots (P.x/P.y/P.z at
			//! +0/+1/+2).  It got a name once a second site came to depend
			//! on the literal 2: besides Builder::Finalize's `out.m_PSlot`
			//! pin, the compile-time domain-scale analysis
			//! (Builder::SlotLinear) differentiates a noise call's position
			//! argument with respect to exactly these three slots.
			static const int kContextSlotP     = 2;
			static const int kContextSlotFw    = 11;
			static const int kContextSlotTime  = 12;
			//! curv / curvR -- the geometry-derived shading signal
			//! (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md Phase 1).  Gated behind
			//! EnableContextVars(true) exactly like P/N/fw, so
			//! expression_function2d's frozen UV-only surface never sees them.
			static const int kContextSlotCurv  = 13;
			static const int kContextSlotCurvR = 14;
			//! Total reserved context-var slots (u,v,P,Po,N,fw,time,curv,curvR).
			static const int kContextSlotCount = kContextSlotCurvR + 1;
			//! Bit position of a context var within the `UsesContextVar` mask --
			//! its first env slot.  kContextSlotCount stays well under 32, which
			//! is what lets the mask be a plain unsigned int.
			static const int kContextVarMaskBits = 32;

			//! Finiteness test hardened for the production -ffast-math build.
			//! IsFiniteDouble materialises the value through volatile before its
			//! IEEE-754 exponent test, so this hot-path guard remains real even
			//! when inlined into the evaluator.
			static bool IsFinite( const Scalar x )
			{
				return RISE::IsFiniteDouble( static_cast<double>( x ) );
			}

			//! Reset the environment + bind u, v (context vars stay zero,
			//! matching a default-constructed ExprEvalContext), then run
			//! the program.  Byte-compatible with every pre-vec3 caller.
			Scalar Eval( const Scalar u, const Scalar v ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, u, v, Vector3(0,0,0), Vector3(0,0,0), Vector3(0,0,0), Scalar(0), Scalar(0), Scalar(0), Scalar(0), 0 );
				Scalar out[3];
				// No hit record here, so no signal provider: occlusion() /
				// thickness() fall back to their neutral values, exactly as
				// the zero context vars above do.
				RunAny( m_final, env, out, 0 );
				return out[0];
			}

			//! Full-context evaluation (u, v, P, Po, N, fw, time).  Returns
			//! the first component when the final expression is vec3-typed.
			Scalar Eval( const ExprEvalContext& ctx ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.time, ctx.curv, ctx.curvR, &ctx.signals );
				Scalar out[3];
				RunAny( m_final, env, out, &ctx.signals );
				return out[0];
			}

			//! Same as Eval(ctx), but returns the full vec3 result.  Valid
			//! for any program; a scalar-typed final expression broadcasts
			//! its single value to (s,s,s), matching the language's scalar
			//! -> vec3 broadcast rule everywhere else.
			Vector3 EvalVec3( const ExprEvalContext& ctx ) const
			{
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.time, ctx.curv, ctx.curvR, &ctx.signals );
				Scalar out[3];
				RunAny( m_final, env, out, &ctx.signals );
				if( m_final.type == kVec3 ) return Vector3( out[0], out[1], out[2] );
				return Vector3( out[0], out[0], out[0] );
			}

			VType ResultType() const { return m_final.type; }

			//! Does this compiled program READ the context variable whose first
			//! env slot is `firstSlot` (use a kContextSlot* constant)?  Resolved
			//! entirely at COMPILE time by the Builder -- every identifier is
			//! bound to a slot when the body is parsed -- so this is a static
			//! property of the program, not a per-eval check, and covers `def`
			//! stage bodies as well as the final expression.
			//!
			//! It is CONSERVATIVE in exactly one direction: a var read only
			//! inside a `def` the final expression never uses still reports
			//! true.  That is the safe direction for a cost gate (the failure
			//! mode is "computed and unread", never "read and absent").
			//!
			//! Its consumer is the curvature cost gate (SurfaceCurvature.h's
			//! SurfaceCurvatureDemand, design doc 5.4): ~18 extra SDF field
			//! evaluations per hit must cost zero when no expression in the
			//! scene mentions `curv`.
			bool UsesContextVar( int firstSlot ) const
			{
				if( firstSlot < 0 || firstSlot >= kContextVarMaskBits ) return false;
				return ( m_ctxUsedMask & ( 1u << firstSlot ) ) != 0;
			}

			//! Convenience for the gate's actual question: does the body read
			//! EITHER curvature variable?
			bool UsesSurfaceCurvature() const
			{
				return UsesContextVar( kContextSlotCurv ) || UsesContextVar( kContextSlotCurvR );
			}

			//! Does this program call `occlusion()` or `thickness()`
			//! anywhere -- final expression or any `def` stage?  Resolved at
			//! COMPILE time, same as UsesContextVar.
			//!
			//! Unlike UsesSurfaceCurvature this is NOT wired to a cost gate:
			//! the per-hit provider install is a pointer plus six scalars and
			//! the estimators are lazy by construction, so there is nothing
			//! to gate (see SurfaceSignalInfo's own doc for the full
			//! argument).  It exists for introspection and for Phase 3's bake
			//! trigger, which genuinely does need to know up front.
			bool UsesSurfaceSignals() const { return !m_signalCalls.empty(); }

			//! Every `occlusion()` / `thickness()` call site, in parse order
			//! (def stages first, in registration order, then the final
			//! expression).  Phase 3's baked mesh path reads this to decide
			//! WHICH radius to bake and whether it may answer at all.
			const std::vector<SignalRadiusCall>& SurfaceSignalCalls() const { return m_signalCalls; }

			//! Number of compiled `def` stages (registration order == the
			//! chunk's `def` line order, since AddDef pushes onto m_defs in
			//! call order and Builder::Finalize copies it verbatim).  0 for
			//! a program with no defs.  doc 88 S10: lets a caller bound a
			//! def-stage preview index without guessing.
			int DefCount() const { return (int)m_defs.size(); }

			//! doc 88 S10 (Tier-2 def-stage thumbnail preview): evaluate the
			//! program only THROUGH def index `defIdx` (0-based, registration
			//! order) and report that def's own result -- the same
			//! intermediate value a later def or the final expression would
			//! see at that slot, without running anything after it.  Returns
			//! false (outVal/outType untouched) when `defIdx` is out of
			//! [0, DefCount()) -- e.g. a program with no defs, or a stale
			//! index after the author edits the chain; the caller must not
			//! infer "zero" from a false return.
			//!
			//! A scalar-typed def's result broadcasts to (s,s,s) in outVal,
			//! matching EvalVec3's broadcast convention -- so a caller that
			//! always reads outVal.x gets the right number regardless of
			//! outType, and one that wants the true triple can check
			//! outType first.
			//!
			//! Const, no heap allocation, no mutable state touched -- same
			//! stack-machine contract as Eval/EvalVec3, so this is safe to
			//! call from many concurrent preview-render threads sharing one
			//! compiled program.
			bool EvalDefStage( const ExprEvalContext& ctx, int defIdx, Vector3& outVal, VType& outType ) const
			{
				if( defIdx < 0 || (size_t)defIdx >= m_defs.size() ) return false;
				Scalar env[ kMaxSlots ];
				BindEnv( env, ctx.u, ctx.v, ctx.P, ctx.Po, ctx.N, ctx.fw, ctx.time, ctx.curv, ctx.curvR, &ctx.signals, defIdx );
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
			//! Byte offset of the error within the expression string that
			//! produced it, or -1 when the error isn't localized to one
			//! position.
			ptrdiff_t ErrorOffset() const { return m_errorOffset; }

			//////////////////////////////////////////////////////////
			// Builder
			//////////////////////////////////////////////////////////
			class Builder
			{
			public:
				Builder() : m_errorOffset(-1), m_hasBuilderError(false), m_contextVarsEnabled(false),
					m_ctxUsed(0)
				{
					// Reserve the fixed context-variable slots (u=0, v=1,
					// P=2..4, Po=5..7, N=8..10, fw=11, time=12, curv=13,
					// curvR=14 -- see the kContextSlot* block) WITHOUT
					// registering their names in m_index.  A param/def is
					// free to reuse any of these names -- ParseAtom checks
					// m_index (user params/defs) first and falls back to
					// LookupContextVar only when the name isn't user-bound,
					// so a same-named param SHADOWS the context meaning for
					// that expression rather than colliding with it.  This
					// is load-bearing back-compat: real scenes already
					// declare `param N <value>` as an ordinary scalar
					// constant (enamel_watch.RISEscene et al.) that must
					// keep meaning exactly that, not the new shading normal.
					m_names.assign( kContextSlotCount, std::string() );
				}

				//! A named numeric constant (scalar only -- matches the
				//! scene-file `param <name> <number>` grammar).  Returns false
				//! (and sets error/offset) when `name` collides with an earlier
				//! param/def of a DIFFERENT type -- see RejectIfDuplicate.  A
				//! same-type redefinition (e.g. two `param a` lines) is NOT
				//! rejected: it is "last wins" (the later value simply replaces
				//! the earlier one in the same slot), matching Cst.cpp's `let`
				//! chunk semantics (CollectLetBindings feeds every binding --
				//! duplicates included -- through AddParam in document order,
				//! and CstLetTest locks in "duplicate let name is last-wins").
				bool AddParam( const std::string& name, const Scalar value )
				{
					if( RejectIfDuplicate( name, kScalar ) ) return false;
					const int s = Slot( name, kScalar );
					m_init[s] = value;
					// A param IS a compile-time constant, which is what
					// lets `param k 40` + `fbm(P*k, ...)` resolve its
					// domain scale exactly like the literal `P*40` does.
					// Unless a def owns the slot -- see m_slotIsDef.
					if( m_slotIsDef.find( s ) == m_slotIsDef.end() ) {
						const LinScalar lin = LinScalar::Const( value );
						RecordSlotLinear( s, 1, &lin );
					}
					return true;
				}

				//! A named sub-expression (let-binding), evaluated in
				//! registration order; visible to later defs + the final expr.
				//! Its type (scalar or vec3) is inferred from `expr`.
				//! Returns false (and sets error/offset) on a parse failure OR
				//! when `name` collides with an earlier param/def of a
				//! DIFFERENT type -- see RejectIfDuplicate (a same-type
				//! redefinition is allowed; see AddParam's doc comment).
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
					// Carry the def's own domain scale forward, so the very
					// common `def q P*40` / `expr fbm(q, ...)` split resolves
					// exactly as the inlined `fbm(P*40, ...)` would.  A body
					// the analysis cannot follow records as unknown, which
					// leaves every noise call reading it on the
					// multiplier-1.0 fallback.
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

				//! Compile the final expression and produce the program.
				bool Finalize( const std::string& expr, ExpressionProgram& out )
				{
					if( m_hasBuilderError ) {
						// A prior AddParam/AddDef already failed (e.g. a duplicate
						// name, P1-A) -- that failure is STICKY: Finalize must not
						// silently succeed just because the final expr itself compiles.
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
							" total variable slots (" + std::to_string( kContextSlotCount ) + " are reserved for u,v,P,Po,N,fw,time,curv,curvR)", -1 );
						out.m_valid = false; out.m_error = m_error; out.m_errorOffset = m_errorOffset;
						return false;
					}
					c.writeSlot = -1;
					c.type = t;
					out.m_uSlot = 0; out.m_vSlot = 1;
					out.m_PSlot = kContextSlotP; out.m_PoSlot = 5; out.m_NSlot = 8;
					out.m_fwSlot = kContextSlotFw; out.m_timeSlot = kContextSlotTime;
					out.m_curvSlot = kContextSlotCurv; out.m_curvRSlot = kContextSlotCurvR;
					// Compile-time consumption record (design doc 5.4): which context
					// vars did any def body or the final expression actually resolve?
					// Accumulated by ParseAtom across every Compile() this Builder
					// ran, so it covers `def` stages as well as the final expr.
					out.m_ctxUsedMask = m_ctxUsed;
					// Same accumulate-across-every-Compile discipline as m_ctxUsed:
					// covers `def` stage bodies as well as the final expression.
					out.m_signalCalls = m_sigCalls;
					out.m_initEnv.assign( m_names.size(), Scalar(0) );
					for( std::map<int,Scalar>::const_iterator it = m_init.begin(); it != m_init.end(); ++it ) {
						out.m_initEnv[ it->first ] = it->second;
					}
					out.m_defs = m_defs;
					out.m_final = c;
					out.m_valid = true;
					return true;
				}

				const std::string& Error() const { return m_error; }
				ptrdiff_t ErrorOffset() const { return m_errorOffset; }

				//! Context vars (P, Po, N, fw, time, curv, curvR) are OFF by default -- ParseAtom
				//! treats those seven names as ordinary unknown identifiers unless this
				//! is turned on (u and v are never gated; they are the query
				//! coordinates every surface has always had).  This keeps the
				//! document-level `expr(...)` / `let` sublanguage (Cst.cpp's
				//! EvalExprBody, AgentSession.cpp's LocalEvalExprBody -- both predate
				//! the S1 vec3/context-var VM extension) byte-for-byte pre-vec3-VM:
				//! `time`, `P`, etc. stay hard "unknown variable" compile errors there
				//! UNLESS the author let-binds a same-named param, which shadows via
				//! m_index regardless of this flag (see the Builder() ctor comment).
				//! The texture-authoring surface (expression_painter,
				//! Job.cpp / scalar_painter{expression}, ChunkParserRegistry.cpp)
				//! turns this ON -- that surface is what P/Po/N/fw/time were
				//! added FOR (doc 88).  expression_function2d (also
				//! ChunkParserRegistry.cpp) is the OLDER, UV-only surface and
				//! must stay OFF -- see ExpressionPainter.h's
				//! BuildExpressionProgramFromChunkFields doc comment for the
				//! frozen-contract rationale.
				void EnableContextVars( bool enable = true ) { m_contextVarsEnabled = enable; }

			private:
				std::vector<std::string> m_names;	// slot index -> name (vec3 reserves 3 consecutive)
				std::map<std::string,int> m_index;	// name -> first slot
				std::map<int,VType> m_slotType;	// first slot -> type
				std::map<int,Scalar> m_init;		// param initial values (scalar slots only)
				std::vector<Compiled> m_defs;
				std::string m_error;
				ptrdiff_t m_errorOffset;
				bool m_hasBuilderError;	// sticky: once true, Finalize always fails (P1-A dup-name guard)
				bool m_contextVarsEnabled;	// see EnableContextVars() doc comment
				//! Bit i set == some body compiled by THIS builder resolved the
				//! context var whose FIRST env slot is i.  Written only by
				//! ParseAtom's context branch; copied into the program by Finalize
				//! as m_ctxUsedMask.  (Named differently from the program-side
				//! field on purpose: Builder is a NESTED class, so a same-named
				//! member would shadow-collide with the enclosing
				//! ExpressionProgram's own -- which is a hard compile error, not a
				//! silent shadow, but a confusing one.)
				unsigned int m_ctxUsed;
				//! Every occlusion()/thickness() call site compiled by THIS
				//! builder, in parse order; copied into the program by Finalize
				//! as m_signalCalls.  (Named differently from the program-side
				//! field for exactly the reason m_ctxUsed is -- Builder is a
				//! nested class, and a same-named member would collide with the
				//! enclosing ExpressionProgram's own.)
				std::vector<ExpressionProgram::SignalRadiusCall> m_sigCalls;

				void SetError( const std::string& msg, ptrdiff_t offset ) { m_error = msg; m_errorOffset = offset; }

				// P1-A: refuse a duplicate param/def name whose TYPE doesn't
				// match the earlier registration, instead of silently reusing
				// the earlier slot (Slot() below used to do exactly that,
				// type-blind, regardless of type) -- a same-named `param` +
				// `def` of DIFFERENT types clobbered the earlier binding's env
				// cells, read the wrong type, and could write past the vec3
				// def's 3 slots into whatever the LAST allocated slot happened
				// to be (an ASan-confirmed stack overflow at BindEnv).  That
				// cross-type case has no sensible semantics (unlike a real
				// language's block-scoped shadowing, every param/def in one
				// program shares a single flat namespace) and is a hard compile
				// error.  A SAME-type redefinition (e.g. two `param a` lines,
				// or two `def a` lines) is memory-safe -- Slot() returns the
				// already-correctly-sized existing slot either way -- and is
				// deliberately ALLOWED here as "last wins": Cst.cpp's `let`
				// chunk semantics (CollectLetBindings feeds every binding,
				// duplicates included, through AddParam in document order) and
				// CstLetTest ("duplicate let name is last-wins") depend on it.
				// The error is STICKY (see m_hasBuilderError) because
				// AddParam/AddDef are void-ish fire-and-forget calls at most
				// call sites -- Finalize is where every caller already checks a
				// return value.
				bool RejectIfDuplicate( const std::string& name, VType vt )
				{
					std::map<std::string,int>::const_iterator it = m_index.find( name );
					if( it == m_index.end() ) return false;
					if( m_slotType[ it->second ] == vt ) return false;	// same-type: allowed, "last wins"
					if( !m_hasBuilderError ) {
						SetError( "duplicate name `" + name + "` redeclared with a different type (was " +
							( m_slotType[it->second]==kVec3 ? "vec3" : "scalar" ) + ", now " + ( vt==kVec3 ? "vec3" : "scalar" ) + ")", -1 );
						m_hasBuilderError = true;
					}
					return true;
				}

				// Fixed context-variable name -> (slot, type).  Checked only
				// when `name` isn't a user param/def (see the Builder()
				// constructor comment on why user names shadow these).  ALL
				// NINE names resolve here regardless of m_contextVarsEnabled
				// -- the caller (ParseAtom) is what gates
				// P/Po/N/fw/time/curv/curvR on that flag; u/v are never
				// gated.  See EnableContextVars().
				static bool LookupContextVar( const std::string& name, int& slot, VType& vt )
				{
					if( name == "u" )    { slot = 0;  vt = kScalar; return true; }
					if( name == "v" )    { slot = 1;  vt = kScalar; return true; }
					if( name == "P" )    { slot = 2;  vt = kVec3;   return true; }
					if( name == "Po" )   { slot = 5;  vt = kVec3;   return true; }
					if( name == "N" )    { slot = 8;  vt = kVec3;   return true; }
					if( name == "fw" )    { slot = kContextSlotFw;    vt = kScalar; return true; }
					if( name == "time" )  { slot = kContextSlotTime;  vt = kScalar; return true; }
					if( name == "curv" )  { slot = kContextSlotCurv;  vt = kScalar; return true; }
					if( name == "curvR" ) { slot = kContextSlotCurvR; vt = kScalar; return true; }
					return false;
				}

				int Slot( const std::string& name, VType vt )
				{
					std::map<std::string,int>::const_iterator it = m_index.find( name );
					if( it != m_index.end() ) {
						// Defensive only: every current caller (AddParam/AddDef) already
						// rejects a duplicate name via RejectIfDuplicate before reaching
						// here, so this branch reusing a slot is unreachable today.  Kept
						// as a belt for a future caller that bypasses that guard --
						// silently returning a slot whose recorded type doesn't match
						// `vt` is exactly the P1-A bug (env clobber + stack overflow), so
						// surface it as a builder-level error instead.
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

				// --- function table (name -> id/syntactic-arg-types/return) ---
				// Legacy scalar ids (1..33) are UNCHANGED from the pre-vec3
				// engine -- CallFunc's cases for them are byte-identical, only
				// the arity is now carried explicitly on the instruction
				// instead of recovered from the id band.
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
						// vec3-domain, scalar-returning
						{"dot",40,2,{V,V,S,S},S}, {"length",41,1,{V,S,S,S},S},
						{"perlin",42,1,{V,S,S,S},S},
						{"fbm",43,4,{V,S,S,S},S}, {"turbulence",44,4,{V,S,S,S},S}, {"ridged",45,4,{V,S,S,S},S},
						{"worley_f1",46,2,{V,S,S,S},S}, {"worley_f2",47,2,{V,S,S,S},S},
						{"worley_f2f1",48,2,{V,S,S,S},S}, {"worley_id",49,2,{V,S,S,S},S},
						{"cellhash",50,1,{S,S,S,S},S},
						// geometry-derived shading signals -- one scalar
						// argument, the query radius as a FRACTION of the hit
						// geometry's characteristic size (design doc Phase 2).
						// Gated on EnableContextVars in ParseCall for the same
						// reason P/N/curv are gated in ParseAtom: they are
						// surface queries, and expression_function2d's frozen
						// UV-only contract must not grow them.
						{"occlusion",ExpressionProgram::kFnOcclusion,1,{S,S,S,S},S},
						{"thickness",ExpressionProgram::kFnThickness,1,{S,S,S,S},S},
						// vec3-returning
						{"cross",60,2,{V,V,S,S},V}, {"normalize",61,1,{V,S,S,S},V},
					};
					for( size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i ) {
						if( name == table[i].n ) return &table[i];
					}
					return 0;
				}
				// fn ids reserved for functions parsed with bespoke logic
				// rather than the FnSig table (kept out of the table so a
				// name collision there is a compile error, not a silent
				// shadow): vec3()/ramp()/mix() -- see ParseAtom/ParseCall.
				static const int kFnMixScalar = 32;
				static const int kFnMixVec3   = 62;

				//////////////////////////////////////////////////
				// Recursive-descent parser -> postfix emit
				//////////////////////////////////////////////////
				struct Tok { enum T { Num, Ident, Op, LP, RP, Comma, Dot, End } t; Scalar num; std::string s; size_t off; };

				bool Tokenize( const std::string& e, std::vector<Tok>& out )
				{
					size_t i = 0, n = e.size();
					while( i < n ) {
						const char c = e[i];
						if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) { ++i; continue; }
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
					// opcode, except kFunc/kFuncV3/kRamp whose arity varies --
					// those read it straight off the instruction.
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

				void EmitConst( Scalar v ) { Compiled::Instr in; in.op=Compiled::kConst; in.val=v; in.idx=-1; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitVarSlot( int idx ) { Compiled::Instr in; in.op=Compiled::kVar; in.val=0; in.idx=idx; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitOp( Compiled::Op o ) { Compiled::Instr in; in.op=o; in.val=0; in.idx=-1; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitSwizzle( int comp ) { Compiled::Instr in; in.op=Compiled::kSwizzle; in.val=0; in.idx=comp; in.fn=-1; in.arity=0; m_emit->code.push_back(in); }
				void EmitSplat3() { EmitOp( Compiled::kSplat3 ); }
				//! `fwScale` lands on the instruction's `val` and multiplies
				//! the context `fw` at dispatch (RunAny's kFunc case).  1.0
				//! -- the default, and what every non-noise builtin keeps
				//! forever -- is an exact IEEE identity, so leaving it alone
				//! reproduces the pre-domain-scale engine bit for bit.
				//!
				//! ONLY THE SCALAR FORM CONSUMES IT.  `isVec3` emits
				//! kFuncV3, whose RunAny case dispatches through
				//! CallFuncVec3 -- which takes no `fw` at all and never
				//! reads `in.val`.  Passing a scale here for a vec3-
				//! returning builtin would therefore be silently dropped,
				//! un-fading it.  Harmless today (every fw-consuming
				//! builtin -- fbm/turbulence/ridged -- is scalar-returning,
				//! so the only caller that passes anything but 1.0 emits
				//! kFunc), but a future vec3 noise builtin must plumb `fw`
				//! into CallFuncVec3 and read `in.val` in the kFuncV3 case
				//! BEFORE it can pass a scale through here.
				void EmitFuncCall( int fn, int arity, bool isVec3, Scalar fwScale = Scalar(1) )
				{
					Compiled::Instr in; in.op = isVec3 ? Compiled::kFuncV3 : Compiled::kFunc;
					in.val=fwScale; in.idx=-1; in.fn=fn; in.arity=arity;
					m_emit->code.push_back(in);
				}

				// Emits the vec3 var reference at slot `s` (3 consecutive kVar).
				void EmitVec3Var( int s ) { EmitVarSlot(s); EmitVarSlot(s+1); EmitVarSlot(s+2); }

				//////////////////////////////////////////////////
				// COMPILE-TIME DOMAIN SCALE (forward-mode Jacobian)
				//
				// `fw` is a WORLD-space length, but fbm/turbulence/ridged
				// fade octaves in the domain of THEIR OWN position
				// argument -- and virtually every real body scales that
				// argument (`fbm(P*40, ...)`).  Handing the noise a
				// world-space fw therefore mis-set the Nyquist threshold
				// by exactly the domain scale, which for the scales scenes
				// actually use (7 .. 820) meant the fade never engaged.
				//
				// The fix is a forward-mode derivative pass over the
				// position argument's ALREADY-EMITTED postfix code, run
				// once at COMPILE time: interpret the instructions over
				// linear forms instead of numbers, recover the 3x3
				// Jacobian d(argument)/dP, and fold its largest singular
				// value -- the worst-case factor by which the argument
				// stretches a world-space footprint -- into a per-call-
				// site multiplier on `fw`.
				//
				// Why over the emitted CODE rather than the token stream:
				// the code is the single representation that already has
				// operator precedence, scalar->vec3 broadcast and `def`
				// slot references resolved, so the analysis cannot drift
				// from what the VM will actually evaluate.  A second
				// parse of the source text could.
				//////////////////////////////////////////////////

				//! One scalar value's linear form with respect to the world
				//! position P.  `gradKnown` says d(value)/dP is `g`;
				//! `gradExact` says nothing was dropped getting there (see
				//! LinAddSub); `constKnown` says the value itself is the
				//! compile-time constant `c`.  INVARIANT: constKnown implies
				//! gradKnown with g == 0, which is what lets LinMul treat a
				//! constant factor as a scale rather than a variable.
				struct LinScalar
				{
					bool   gradKnown;
					bool   gradExact;
					bool   constKnown;
					Scalar c;
					Scalar g[3];
					LinScalar() : gradKnown(false), gradExact(false), constKnown(false), c(0)
					{ g[0] = g[1] = g[2] = Scalar(0); }
					static LinScalar Const( Scalar v )
					{
						LinScalar r; r.gradKnown = true; r.gradExact = true; r.constKnown = true; r.c = v;
						return r;
					}
				};

				//! d(-a)/dP == -da/dP; a constant negates to a constant.
				static LinScalar LinNeg( const LinScalar& a )
				{
					LinScalar r = a;
					r.c = -a.c;
					for( int k = 0; k < 3; ++k ) r.g[k] = -a.g[k];
					return r;
				}

				//! a + b (or a - b).  The one APPROXIMATING rule in the
				//! whole analysis, and it applies to ADDITION ONLY: when
				//! exactly one side of a SUM has a known gradient, the
				//! result keeps that side's gradient and drops the other,
				//! flagging the answer inexact.  That is what makes the
				//! domain-warp idiom
				//! `fbm(P*8 + amp*vec3(fbm(...),...), ...)` fade at 8*fw
				//! instead of falling back to 1*fw: the warp's own slope is
				//! unknowable at compile time, and the idiom's whole point
				//! is that the affine part is the term that sets the
				//! frequency.
				//!
				//! WHAT THE APPROXIMATION IS AND IS NOT (fix round, review
				//! of the domain-scale change).  Dropping a term is NOT
				//! provably an UNDER-estimate -- an earlier draft of this
				//! comment claimed it "errs toward the pre-existing
				//! (un-faded) behaviour", which is false: the dropped
				//! term's slope has an unknown SIGN, so it can cancel the
				//! kept one.  Under SUBTRACTION that is not a corner case
				//! but the natural reading of the construct, and it is
				//! unbounded: `fbm(P*40 - q + vec3(0.5,0.5,0.5), ...)`
				//! with `q` an opaque (kFunc-built) copy of `P*40` is a
				//! CONSTANT position -- true stretch 0 -- yet the old rule
				//! reported 40, over-blurring a field that cannot alias at
				//! all.  So a subtraction with an unknown operand now
				//! reports NO gradient, which lands the call site on the
				//! multiplier-1.0 fallback: the pre-change behaviour, the
				//! one answer that is never worse than doing nothing.
				//!
				//! The residual, stated exactly rather than papered over:
				//! for `plus`, a dropped term whose true slope opposes the
				//! kept one still yields an OVER-estimate (the same
				//! cancellation, spelled `a + (-b)` or `a + (-1)*b`, where
				//! the negation is buried inside the unknown and cannot be
				//! seen here).  That is accepted, not overlooked -- it is
				//! the price of resolving the shipped domain-warp idiom at
				//! all, the warp amplitudes real bodies use are small
				//! against the affine scale, and the failure mode is a
				//! preview/render that is over-filtered, never one that
				//! aliases.
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
						for( int k = 0; k < 3; ++k ) r.g[k] = plus ? ( a.g[k] + b.g[k] ) : ( a.g[k] - b.g[k] );
					} else if( plus && a.gradKnown ) {
						r.gradKnown = true; r.gradExact = false;
						for( int k = 0; k < 3; ++k ) r.g[k] = a.g[k];
					} else if( plus && b.gradKnown ) {
						r.gradKnown = true; r.gradExact = false;
						for( int k = 0; k < 3; ++k ) r.g[k] = b.g[k];
					}
					// else: a subtraction missing either operand's gradient
					// -- deliberately left unknown (see above).
					return r;
				}

				//! a * b.  Affine only when one factor is a compile-time
				//! constant -- d(f*g) = f*dg + g*df needs RUNTIME values
				//! otherwise, so a product of two P-varying terms reports
				//! no gradient at all rather than a guess.
				static LinScalar LinMul( const LinScalar& a, const LinScalar& b )
				{
					LinScalar r;
					if( a.constKnown && b.constKnown ) return LinScalar::Const( a.c * b.c );
					if( a.constKnown && b.gradKnown ) {
						r.gradKnown = true; r.gradExact = b.gradExact;
						for( int k = 0; k < 3; ++k ) r.g[k] = a.c * b.g[k];
						return r;
					}
					if( b.constKnown && a.gradKnown ) {
						r.gradKnown = true; r.gradExact = a.gradExact;
						for( int k = 0; k < 3; ++k ) r.g[k] = b.c * a.g[k];
						return r;
					}
					return r;
				}

				//! a / b, affine only for a constant divisor.  The
				//! zero-divisor branch mirrors RunAny's kDiv exactly (it
				//! yields 0, it does not trap), so the analysis and the VM
				//! agree about what that expression IS.
				static LinScalar LinDiv( const LinScalar& a, const LinScalar& b )
				{
					LinScalar r;
					if( !b.constKnown ) return r;
					if( b.c == Scalar(0) ) return LinScalar::Const( Scalar(0) );
					if( a.constKnown ) return LinScalar::Const( a.c / b.c );
					if( a.gradKnown ) {
						r.gradKnown = true; r.gradExact = a.gradExact;
						for( int k = 0; k < 3; ++k ) r.g[k] = a.g[k] / b.c;
					}
					return r;
				}

				//! The linear form of whatever lives in env slot `slot`.
				//! P's own three slots are the differentiation variable;
				//! params and defs answer from m_slotLin (recorded in
				//! registration order, so a name always reports the binding
				//! in force at this point in the program); every other
				//! context var (u, v, Po, N, fw, time, curv, curvR) is
				//! honestly unknown -- their relationship to world P is a
				//! per-hit fact, not a compile-time one.
				LinScalar SlotLinear( int slot ) const
				{
					if( slot >= ExpressionProgram::kContextSlotP && slot < ExpressionProgram::kContextSlotP + 3 ) {
						LinScalar r;
						r.gradKnown = true; r.gradExact = true;
						r.g[ slot - ExpressionProgram::kContextSlotP ] = Scalar(1);
						return r;
					}
					std::map<int,LinScalar>::const_iterator it = m_slotLin.find( slot );
					if( it != m_slotLin.end() ) return it->second;
					return LinScalar();
				}

				//! Abstract-interpret `code[begin,end)` over linear forms,
				//! mirroring RunAny's stack effects instruction for
				//! instruction.  Returns false when the range does not
				//! leave exactly `expectWidth` values (which would mean the
				//! analysis and the VM disagree about the stack -- never
				//! observed, but the caller then falls back to multiplier
				//! 1.0 rather than trusting a desynchronized answer).
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
						// %, ^ and the comparisons are not affine in P for
						// any input worth tracking, so their result reports
						// neither a value nor a gradient.  A position
						// argument built through one of them lands on the
						// multiplier-1.0 fallback, exactly as it did before
						// this analysis existed.
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
							if( in.idx < 0 || in.idx > 2 ) return false;	// EmitSwizzle only ever emits 0/1/2
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
						default:
							return false;
						}
						if( st.size() > (size_t)ExpressionProgram::kStackCap ) return false;
					}
					if( st.size() != (size_t)expectWidth ) return false;
					for( int k = 0; k < expectWidth; ++k ) out[k] = st[(size_t)k];
					return true;
				}

				//! Largest singular value of the 3x3 whose ROW i is
				//! rows[i].g -- the worst-case stretch the argument applies
				//! to a world-space footprint, and therefore the right
				//! single number to scale an isotropic `fw` by.
				static Scalar JacobianSpectralNorm( const LinScalar rows[3] )
				{
					Scalar J[3][3];
					for( int i = 0; i < 3; ++i ) {
						for( int k = 0; k < 3; ++k ) J[i][k] = rows[i].g[k];
					}
					// DIAGONAL fast path, and it is not merely an
					// optimisation: a diagonal matrix's singular values ARE
					// |d_i|, computed with no arithmetic at all, so the
					// identity yields EXACTLY 1.0 and `fw * 1.0 == fw`
					// keeps an unscaled `fbm(P, ...)` bit-identical to the
					// pre-change engine.  An iterative eigen-solve would
					// return 1.0 only to within rounding.  Every ordinary
					// domain scale lands here: P, P*k, P/s, -P*k,
					// vec3(P.x*a, P.y*b, P.z*c).
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
					// General case (a rotated / sheared domain): the
					// largest eigenvalue of the symmetric PSD matrix J^T J,
					// by cyclic Jacobi rotations.  Deterministic and
					// start-vector free, unlike power iteration -- and this
					// runs ONCE per call site at compile time, so the sweep
					// count is free.
					//
					// This branch is ACCURATE, not exact: the sweep
					// converges to ~1 ulp, so `fw * s` here is a rounded
					// product even when the true singular value is a
					// round number (a 45-degree rotation times 40 comes
					// back as 40 to within ~1e-13, not bit-exactly 40).
					// That is why the diagonal case above is a separate
					// closed form rather than "the Jacobi path also
					// handles it": bit-identity for the un-scaled and
					// axis-scaled bodies -- which is every shipped body --
					// is a promise only the closed form can keep.  A test
					// pinning THIS branch must therefore compare with a
					// tolerance (TextureExpressionVMTest test 69).
					// Reaching it at all takes a domain argument with
					// LITERAL off-diagonal coefficients: `cos`/`sin` are
					// kFunc calls, which AnalyzeLinear reports as unknown,
					// so a rotation written with them falls back to 1.0
					// instead.
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
								const int r = 3 - p - q;	// the index neither rotation axis touches
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

				//! The `fw` multiplier for a noise call whose vec3 position
				//! argument occupies `[begin, end)` of the code emitted so
				//! far.  1.0 whenever the domain scale is not provable --
				//! which is precisely the pre-change behaviour, so an
				//! un-analysable body is never made worse, only left alone.
				Scalar NoiseFwScale( size_t begin, size_t end ) const
				{
					LinScalar rows[3];
					if( !AnalyzeLinear( m_emit->code, begin, end, 3, rows ) ) return Scalar(1);
					for( int i = 0; i < 3; ++i ) {
						if( !rows[i].gradKnown ) return Scalar(1);
						for( int k = 0; k < 3; ++k ) {
							if( !ExpressionProgram::IsFinite( rows[i].g[k] ) ) return Scalar(1);
						}
					}
					const Scalar s = JacobianSpectralNorm( rows );
					if( !ExpressionProgram::IsFinite( s ) || s < Scalar(0) ) return Scalar(1);
					if( s == Scalar(0) ) {
						// A position argument that provably does not move
						// with P -- `fbm(vec3(0.3,0.7,1.4), ...)` -- is
						// CONSTANT across the footprint and so has nothing
						// to alias: the honest fade is none at all.  But
						// only when the Jacobian is exact; an approximate
						// zero means the one term that actually varies was
						// the dropped one, and there the safe answer is the
						// pre-change 1.0.
						const bool exact = rows[0].gradExact && rows[1].gradExact && rows[2].gradExact;
						return exact ? Scalar(0) : Scalar(1);
					}
					return s;
				}

				//! Record what env slot `slot` (width `w`) will hold, for
				//! later SlotLinear lookups.  Called in registration order
				//! from AddParam / AddDef so a redefinition simply
				//! overwrites -- matching BindEnv, which seeds params then
				//! runs every def in order, last writer winning.
				void RecordSlotLinear( int slot, int w, const LinScalar* lin )
				{
					for( int k = 0; k < w; ++k ) m_slotLin[ slot + k ] = lin ? lin[k] : LinScalar();
				}

				//! Env slot -> the linear form of what that slot holds.
				//! Declared HERE, beside the analysis that owns it, rather
				//! than up with the parser's members: LinScalar has to be a
				//! complete type at the point of a member DECLARATION (only
				//! member function BODIES get the complete-class context
				//! that would let it float).
				std::map<int,LinScalar> m_slotLin;
				//! Slots some `def` writes.  BindEnv seeds every param and
				//! THEN runs every def, so a def unconditionally wins a slot
				//! it shares with a same-named param -- no matter which was
				//! registered last.  AddParam consults this so a late
				//! `param` cannot overwrite the live def's linear form with
				//! a constant the VM will never actually see.
				std::map<int,bool> m_slotIsDef;

				// Broadcasts the operand whose code occupies [0, insertPos) -- i.e.
				// the operand parsed BEFORE this call, typically the LEFT-hand side
				// of a binary op (`insertPos` is the code index where the RIGHT-hand
				// operand's instructions begin, == right after the left operand's
				// code ends) -- from scalar to vec3, by inserting a kSplat3
				// instruction right after that left operand's code.  (P1-D: this
				// comment previously said the opposite -- "the operand parsed most
				// recently" -- which would be the RIGHT operand; the insert always
				// targets the LEFT one.)  Used when combining a scalar with a vec3
				// in +,-,*,/ where the LEFT side is the scalar (see EmitArith).
				void BroadcastAt( size_t insertPos )
				{
					Compiled::Instr in; in.op=Compiled::kSplat3; in.val=0; in.idx=-1; in.fn=-1; in.arity=0;
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
				// unary := (-|+)? pow
				// P2-B: recurses directly into itself for a chain of leading
				// +/- signs (e.g. `---1`) WITHOUT going through ParseCmp, so it
				// used to bypass ParseCmp's m_parseDepth guard entirely -- a
				// long run of unary signs (e.g. 500k '-' tokens) blew the C++
				// call stack (ASan-confirmed) before ever reaching the
				// nesting-depth check.  Now bounded exactly like ParseCmp.
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
							// CALL position dispatches on the identifier being
							// immediately followed by '(' BEFORE any m_index/context
							// lookup runs -- so a bound param/def name can never
							// shadow a builtin function of the same name in call
							// position (e.g. a `param sin 1` still leaves `sin(x)`
							// meaning the builtin; only bare `sin` -- no call --
							// would resolve to the param).  Contract-only note
							// (P2-C); no behaviour change.
							return ParseCall( name, nameOff, type );
						}
						// variable / param / def reference (checked first so a
						// user-declared name shadows a context var of the
						// same name -- see LookupContextVar).  This binds against
						// m_index AS OF right now (compile-of-this-def time): a
						// param/def registered LATER cannot retroactively rebind an
						// identifier already resolved in an earlier def's body.
						// Not scene-reachable today -- ChunkParserRegistry.cpp's
						// expression_function2d parser always emits every `param`
						// before any `def` -- but is true of the language in
						// general, so documented here rather than assumed (P2-C).
						std::map<std::string,int>::const_iterator it = m_index.find( name );
						if( it != m_index.end() ) {
							const int s = it->second;
							type = m_slotType[s];
							if( type == kVec3 ) EmitVec3Var( s ); else EmitVarSlot( s );
							return true;
						}
						// context reference: u, v always available; P, Po, N, fw,
						// time only when this Builder opted in via
						// EnableContextVars (P2-A) -- see its doc comment.
						{
							int ctxSlot; VType ctxType;
							if( LookupContextVar( name, ctxSlot, ctxType ) ) {
								const bool alwaysAvailable = ( name == "u" || name == "v" );
								if( alwaysAvailable || m_contextVarsEnabled ) {
									// Record the reference for the compile-time consumption
									// query (design doc 5.4), so a geometry can skip an
									// expensive per-hit signal nothing in the scene reads.
									// Set ONLY on the branch that actually emits the slot
									// read -- a name that falls through to "unknown
									// variable", or one shadowed by a user param (resolved
									// earlier via m_index), must not count.
									if( ctxSlot < kContextVarMaskBits ) {
										m_ctxUsed |= ( 1u << ctxSlot );
									}
									type = ctxType;
									if( type == kVec3 ) EmitVec3Var( ctxSlot ); else EmitVarSlot( ctxSlot );
									return true;
								}
								// P/Po/N/fw/time/curv/curvR on a surface that didn't opt
								// in: fall
								// through and treat exactly like any other unknown
								// identifier below.
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

					const FnSig* sig = FindSig( name );
					if( !sig ) { SetError( "unknown function `" + name + "`", (ptrdiff_t)nameOff ); return false; }

					// GEOMETRY-DERIVED SIGNAL BUILTINS (design doc Phase 2).
					// Gated on the same EnableContextVars flag that gates
					// P/Po/N/fw/time/curv in ParseAtom, and for the same
					// reason: these are SURFACE queries, and
					// expression_function2d is a frozen UV-only contract
					// (design doc §14 item 7) that must not grow them.  A
					// dedicated diagnostic rather than "unknown function",
					// because the name IS real -- it is the surface that
					// doesn't have one.
					const bool isSignalFn = ( sig->id == ExpressionProgram::kFnOcclusion ||
					                          sig->id == ExpressionProgram::kFnThickness );
					if( isSignalFn && !m_contextVarsEnabled ) {
						SetError( "`" + name + "()` needs the 3D surface context -- available in expression_painter "
							"and scalar_painter { expression ... }, not in expression_function2d (a UV-only field)",
							(ptrdiff_t)nameOff );
						return false;
					}

					// CONSTANT-RADIUS GROUNDWORK for Phase 3 (design doc
					// §7.1): a baked mesh field can only answer ONE radius, so
					// the baked path needs to know at COMPILE time whether the
					// radius argument is a constant.  Phase 2 records the
					// cheap half -- "is it a bare numeric literal, and what
					// is its value" -- per call site; resolving a
					// `param`/`def` NAME back to a compile-time constant is
					// explicitly Phase-3 work and is NOT attempted here.
					bool literalRadiusArg = false;
					Scalar literalRadiusVal = Scalar(0);

					// DOMAIN SCALE (see the forward-mode Jacobian block
					// above).  fbm/turbulence/ridged fade octaves in the
					// domain of argument 0, so remember exactly which
					// instructions that argument emits and differentiate
					// them once the call is fully parsed.
					const bool isNoiseFn = ( sig->id == 43 || sig->id == 44 || sig->id == 45 );
					size_t posArgStart = 0, posArgEnd = 0;

					int scalarArity = 0;
					int got = 0;
					if( Cur().t != Tok::RP ) {
						for( ;; ) {
							const size_t argOff = CurOff();
							// P1-B: only treat the octaves argument as a checkable
							// literal when it is EXACTLY one Num token -- i.e. the
							// token right after it is `,` or `)`.  The old check
							// (`Cur().t==Tok::Num` alone) peeked just the FIRST
							// token of the argument, so a COMPOUND expression that
							// happens to start with a small in-range number --
							// e.g. `fbm(p, 2*1e20, 0.5, 2.0)` -- passed this
							// compile-time [1,10] check on the leading `2` while the
							// actual runtime value (2e20) then hit an unguarded
							// (int) cast in CallFunc (UBSan-confirmed).  A non-
							// literal octave argument still compiles fine here; it
							// is clamped at RUNTIME instead, by OctavesFromScalar
							// (see CallFunc cases 43/44/45).
							const bool literalOctaveArg = ( ( sig->id==43 || sig->id==44 || sig->id==45 ) && got==1 && Cur().t==Tok::Num &&
								m_pos+1 < m_toks->size() && ( (*m_toks)[m_pos+1].t == Tok::Comma || (*m_toks)[m_pos+1].t == Tok::RP ) );
							const Scalar literalOctaveVal = literalOctaveArg ? Cur().num : Scalar(0);
							// Same "is this argument EXACTLY one numeric
							// literal" discipline as the octave check above,
							// widened by one token so a leading sign counts
							// (`occlusion(-0.1)` must be REJECTED at compile
							// time, and `-0.1` tokenizes as Op then Num).
							if( isSignalFn && got == 0 ) {
								literalRadiusArg = PeekLiteralScalarArg( literalRadiusVal );
							}
							// Recorded around ParseCmp rather than from a
							// saved index afterwards, because EmitArith's
							// left-operand broadcast INSERTS a kSplat3
							// inside the argument's own range as it parses.
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
								// A literal radius that isn't strictly positive
								// has no meaning for either signal and is
								// almost always a units mistake (a world
								// length typed where a FRACTION belongs, then
								// negated).  Catch it here rather than let it
								// silently degrade to the neutral fallback at
								// render time -- a flat mask that "works" is
								// the expensive failure.  A COMPUTED radius is
								// still checked at runtime, by
								// SurfaceSignalInfo::RadiusUsable.
								if( literalRadiusArg && got == 0 && !( literalRadiusVal > Scalar(0) ) ) {
									SetError( std::string(name) + "() radius must be > 0 -- it is a FRACTION of the "
										"object's own size (0.05 = 5% of its bounding-box diagonal), not a world length",
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
					// A signal builtin whose radius we could NOT prove literal
					// is emitted as its `...DynR` twin, so the instruction
					// itself carries the (absence of a) proof down to
					// CallFunc -- see ExpressionProgram::kFnOcclusionDynR.
					// Everything else emits its own id unchanged.
					int emitId = sig->id;
					if( isSignalFn && !literalRadiusArg ) {
						emitId = ( sig->id == ExpressionProgram::kFnOcclusion )
							? ExpressionProgram::kFnOcclusionDynR
							: ExpressionProgram::kFnThicknessDynR;
					}
					// Only a noise builtin reads `fw`; every other call site
					// keeps the exact 1.0 multiplier EmitFuncCall defaults
					// to.  A noise call whose domain scale is not provable
					// gets 1.0 from NoiseFwScale for the same reason.
					const Scalar fwScale = isNoiseFn ? NoiseFwScale( posArgStart, posArgEnd ) : Scalar(1);
					EmitFuncCall( emitId, scalarArity, sig->ret == kVec3, fwScale );
					if( isSignalFn ) {
						// Recorded ONLY here, on the branch that actually
						// emitted the call -- a body that mentions the name
						// without calling it, or one that failed to parse,
						// must not register.  Accumulates across every
						// Compile() this Builder runs, so the list covers
						// `def` stage bodies as well as the final expression,
						// in parse order.
						ExpressionProgram::SignalRadiusCall rec;
						rec.fn = sig->id;
						rec.radiusIsLiteral = literalRadiusArg;
						rec.radiusLiteral = literalRadiusArg ? literalRadiusVal : Scalar(0);
						m_sigCalls.push_back( rec );
					}
					outType = sig->ret;
					return true;
				}

				//! Is the argument starting at the CURRENT token exactly one
				//! numeric literal (with an optional leading sign) -- i.e. is
				//! the token after it `,` or `)`?  Does NOT consume anything;
				//! the caller still parses the argument normally.
				//!
				//! Deliberately narrow, and narrower than "is this argument
				//! constant": `2*0.05` and `param r 0.05` are both genuinely
				//! constant and both report false here.  That is the honest
				//! boundary of what a token peek can prove, and Phase 3 --
				//! which needs the full `param`/`def` constant resolution for
				//! the baked-mesh precondition (design doc §7.1) -- is where
				//! the rest belongs.  Reporting false is always SAFE: it means
				//! "treat the radius as dynamic", which is correct behaviour,
				//! just not the fastest.
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
					in.idx = numStops; in.fn = ( valueType==kVec3 ) ? 3 : 1; in.arity = totalArity;
					m_emit->code.push_back( in );
					outType = valueType;
					return true;
				}
			};

		private:
			int m_uSlot, m_vSlot, m_PSlot, m_PoSlot, m_NSlot, m_fwSlot, m_timeSlot;
			int m_curvSlot, m_curvRSlot;
			//! Compile-time record of which context vars this program reads --
			//! bit i set == the var whose first env slot is i.  See
			//! UsesContextVar().
			unsigned int m_ctxUsedMask;
			//! Compile-time record of every occlusion()/thickness() call site
			//! (design doc 7.1's constant-radius contract).  See
			//! SurfaceSignalCalls().
			std::vector<SignalRadiusCall> m_signalCalls;
			std::vector<Scalar> m_initEnv;
			std::vector<Compiled> m_defs;
			Compiled m_final;
			bool m_valid;
			std::string m_error;
			ptrdiff_t m_errorOffset;

			ExpressionProgram() :
				m_uSlot(0), m_vSlot(1), m_PSlot(2), m_PoSlot(5), m_NSlot(8), m_fwSlot(kContextSlotFw), m_timeSlot(kContextSlotTime),
				m_curvSlot(kContextSlotCurv), m_curvRSlot(kContextSlotCurvR), m_ctxUsedMask(0),
				m_valid(false), m_errorOffset(-1)
			{}
			friend class Builder;

			//! `stopAfterDef` -1 (default, via the two callers below) runs
			//! every def; a non-negative value runs only defs [0, stopAfterDef]
			//! inclusive -- the EvalDefStage entry point's partial-eval path.
			//! An out-of-range non-negative value (>= m_defs.size()) is
			//! treated as "run all", matching Eval's normal full-program
			//! behaviour (EvalDefStage itself never passes such a value; this
			//! is a defensive fallback, not a documented caller contract).
			void BindEnv( Scalar* env, const Scalar u, const Scalar v, const Vector3& P, const Vector3& Po, const Vector3& N, const Scalar fw, const Scalar time, const Scalar curv, const Scalar curvR, const SurfaceSignalInfo* pSignals, int stopAfterDef = -1 ) const
			{
				const size_t n = m_initEnv.size();
				for( size_t i = 0; i < n; ++i ) env[i] = m_initEnv[i];
				env[ m_uSlot ] = u; env[ m_vSlot ] = v;
				env[ m_PSlot+0 ] = P.x;  env[ m_PSlot+1 ] = P.y;  env[ m_PSlot+2 ] = P.z;
				env[ m_PoSlot+0 ] = Po.x; env[ m_PoSlot+1 ] = Po.y; env[ m_PoSlot+2 ] = Po.z;
				env[ m_NSlot+0 ] = N.x;  env[ m_NSlot+1 ] = N.y;  env[ m_NSlot+2 ] = N.z;
				env[ m_fwSlot ] = fw;
				env[ m_timeSlot ] = time;
				env[ m_curvSlot ] = curv;
				env[ m_curvRSlot ] = curvR;
				const size_t defLimit = ( stopAfterDef >= 0 && (size_t)stopAfterDef < m_defs.size() )
					? (size_t)stopAfterDef + 1 : m_defs.size();
				for( size_t i = 0; i < defLimit; ++i ) {
					Scalar out[3];
					RunAny( m_defs[i], env, out, pSignals );
					const int w = ( m_defs[i].type == kVec3 ) ? 3 : 1;
					for( int c = 0; c < w; ++c ) env[ m_defs[i].writeSlot + c ] = out[c];
				}
			}

		public:
			//! Default-constructed program is invalid until a Builder fills it.
			static ExpressionProgram Invalid() { return ExpressionProgram(); }

			// P1-B: saturate a runtime (non-compile-time-literal) octaves
			// argument into a safe int BEFORE casting, in Scalar (double)
			// space -- `(int)a[3]` directly on an arbitrary Scalar (e.g. 2e20
			// from `fbm(p, 2*1e20, ...)`, or NaN) is UB (UBSan-confirmed).
			// Shared by every fbm/turbulence/ridged call site (CallFunc cases
			// 43/44/45) so the guard can't regress independently at just one
			// of the three.  NoiseCore::Fbm3D/Turbulence3D/Ridged3D each also
			// clamp internally (ClampOctaves, int-space) -- this is the layer
			// that makes the CAST itself safe, which has to happen first.
			static int OctavesFromScalar( Scalar v )
			{
				if( !IsFinite( v ) ) return 1;
				if( v < Scalar(1) ) return 1;
				if( v > Scalar(kMaxOctaves) ) return kMaxOctaves;
				return (int)v;
			}

			//! `pSignals` is the per-eval geometry-signal channel (0 when the
			//! caller has no hit record).  It is a PARAMETER, never program
			//! state: the compiled program stays stateless and `const`, so one
			//! program is still safe to evaluate concurrently on many threads.
			//! Every pre-Phase-2 builtin ignores it.
			static Scalar CallFunc( int fn, const Scalar* a, Scalar fw, const SurfaceSignalInfo* pSignals )
			{
				switch( fn )
				{
				// --- legacy scalar functions: byte-identical to the pre-vec3 engine ---
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
				// --- vec3-domain, scalar-returning ---
				case 40: return a[0]*a[3] + a[1]*a[4] + a[2]*a[5];		// dot(a,b)
				case 41: return std::sqrt( a[0]*a[0]+a[1]*a[1]+a[2]*a[2] );	// length(v)
				case 42: return NoiseCore::PerlinOctave3D( a[0], a[1], a[2] );
				// fw (doc 88 S9): the reserved context slot, always at
				// env[kContextSlotFw] regardless of program (Builder::Finalize
				// pins m_fwSlot to kContextSlotFw) -- threaded in from RunAny
				// below so fbm/turbulence/ridged can fade high octaves
				// the sample footprint can't resolve.
				//
				// It arrives ALREADY IN THIS CALL SITE'S DOMAIN: RunAny
				// multiplies the world-space context fw by the instruction's
				// compile-time domain scale, so `fbm(P*40, ...)` sees 40*fw
				// here, which is what NoiseCore::Fbm3D's contract ("fw in the
				// same units as x,y,z") requires.  Nothing to compensate for
				// below -- and CallFunc is deliberately kept ignorant of how
				// that number was arrived at.
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
				// --- geometry-derived shading signals (design doc Phase 2) ---
				// Both take ONE argument: a query radius expressed as a
				// FRACTION of the hit geometry's characteristic size (its
				// bounding-box diagonal), so `occlusion(0.05)` means "5 % of
				// the object" and reads identically at any scene scale and on
				// any instance of the same geometry.
				//
				// The whole answer -- provider-absent fallback, unusable
				// radius, provider refusal, non-finite guard and the [0,1]
				// clamp -- lives in SurfaceSignalInfo so the VM, and any
				// future non-VM consumer, cannot disagree about the
				// conventions.  A null `pSignals` (Eval(u,v), or a hit on
				// geometry that publishes no provider) yields the neutral
				// value, exactly like `fw`'s honest 0.
				//
				// The `...DynR` twins are the SAME builtin evaluated at a
				// call site whose radius the compiler could not prove
				// constant; the only difference is the proof they forward,
				// which the baked (mesh) providers require and the live
				// (SDF) ones ignore.
				case kFnOcclusion:
					return pSignals ? pSignals->Occlusion( a[0], true ) : SurfaceSignalInfo::NeutralOcclusion();
				case kFnOcclusionDynR:
					return pSignals ? pSignals->Occlusion( a[0], false ) : SurfaceSignalInfo::NeutralOcclusion();
				case kFnThickness:
					return pSignals ? pSignals->Thickness( a[0], true ) : SurfaceSignalInfo::NeutralThickness();
				case kFnThicknessDynR:
					return pSignals ? pSignals->Thickness( a[0], false ) : SurfaceSignalInfo::NeutralThickness();
				default: return Scalar(0);
				}
			}

			//! Vec3-returning half of the dispatch.  Carries `pSignals` for
			//! symmetry with CallFunc so a future vec3-valued geometry signal
			//! (bent normals are the obvious Phase-4 candidate) needs no
			//! signature churn; no builtin in this switch reads it today.
			static void CallFuncVec3( int fn, const Scalar* a, Scalar* out, const SurfaceSignalInfo* pSignals )
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
				default:
					out[0] = out[1] = out[2] = Scalar(0);
					break;
				}
			}

		private:
			// Runs a compiled program on `env`, writing its result (1 or 3
			// scalars, per c.type) into `out[0..]`.
			static void RunAny( const Compiled& c, const Scalar* env, Scalar* out, const SurfaceSignalInfo* pSignals )
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
						// `in.val` is this call site's compile-time domain
						// scale (Builder::NoiseFwScale): it turns the
						// world-space context fw into a footprint in the
						// noise's OWN domain.  1.0 -- what every non-noise
						// builtin and every unprovable noise call carries --
						// is an exact IEEE identity, so nothing that did not
						// scale its domain changes by a single bit.
						stack[sp] = CallFunc( in.fn, &stack[sp], env[ kContextSlotFw ] * in.val, pSignals );
						++sp;
					} break;
					case Compiled::kFuncV3:
					{
						const int ar = in.arity;
						sp -= ar;
						Scalar out3[3];
						// NOTE: `in.val` (the kFunc case's domain-scale
						// multiplier) is deliberately NOT read here --
						// CallFuncVec3 takes no `fw`, because no vec3-
						// returning builtin consumes one.  Adding such a
						// builtin means threading `fw` through
						// CallFuncVec3 AND reading `env[kContextSlotFw] *
						// in.val` here; until then EmitFuncCall's contract
						// (see its comment) is that a vec3 call site
						// carries the identity 1.0.
						CallFuncVec3( in.fn, &stack[sp], out3, pSignals );
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
							// Defensive clamp (P2-C): the compile-time ascending-position
							// check only fires when BOTH neighbouring stop positions are
							// literal constants (ParseRampCall) -- a non-literal position
							// (a param/def/expression) isn't checked, so a runtime-computed
							// stop list that isn't actually monotonic could otherwise send
							// `frac` outside [0,1] and extrapolate past the segment's own
							// v0/v1 instead of interpolating between them.
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
					}
				}
				const int w = ( c.type == kVec3 ) ? 3 : 1;
				for( int k = 0; k < w; ++k ) out[k] = ( sp >= w ) ? stack[sp-w+k] : Scalar(0);
			}
		};
	}
}

#endif
