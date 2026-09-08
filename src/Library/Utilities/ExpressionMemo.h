//////////////////////////////////////////////////////////////////////
//
//  ExpressionMemo.h - A THREAD-LOCAL, two-level per-hit memo for the
//  texture-expression VM and for the geometry-derived shading signals
//  it calls.
//
//  WHY IT EXISTS.  docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md 6.2
//  measured `plank_closeup` (640x480, 48 spp = 14.7 M camera samples)
//  evaluating its two signals 210.8 M times -- 14.3x per camera sample.
//  Nothing in the scene asks for that multiplicity: ONE expression
//  painter (`expr_plank`) feeds a ramp, two roughness slots and a relief
//  modifier, and each of those consumers re-runs the WHOLE program at
//  the SAME hit.  27 % of the calls are the relief stencil's four taps,
//  which deliberately hold `signals` CONSTANT across the stencil -- so
//  those signal calls cannot affect the image at all (that document's
//  10, last bullet, which also asks for exactly this memo).
//
//  THE TWO LEVELS, and why one level would not do:
//
//    L1 (signal builtins).  Keyed on the query -- function, radius,
//    constant-radius proof -- plus EVERY field of the hit's
//    SurfaceSignalInfo.  It hits for repeated signal calls at one hit
//    AND across the relief stencil's taps, because the stencil moves
//    P / Po / (u,v) but holds `signals` fixed.  L2 CANNOT catch those:
//    the context genuinely differs.
//
//    L2 (whole program).  Keyed on the program's identity plus the
//    ENTIRE ExprEvalContext, field by field.  It hits for repeated
//    consumers at one hit (ramp + alphax + alphay), for the spectral
//    pipe's per-wavelength GetColorNM (which re-runs EvalRGB per
//    wavelength sample with a byte-identical context), and for any
//    material that evaluates one painter more than once per shading
//    event.  L1 CANNOT catch those: it only ever saves the signal
//    builtin, not the noise / ramp / arithmetic around it.
//
//  THE FP CONTRACT THE TWO LEVELS SIT EITHER SIDE OF, and the test that
//  guards it.  L2 is DELIBERATELY not inside ExpressionProgram::Eval /
//  EvalVec3: this is a `-ffast-math` build, and wrapping those bodies
//  changed which one the optimiser inlined into ExpressionPainter, which
//  changed the FMA contraction inside RunAny, which moved the last ulp of
//  a domain-warped fbm (ExpressionProgram::MakeMemoKey's comment tells
//  the story).  L1 has no such freedom -- it sits in SignalQuery, which
//  is a CALLEE of the VM's own `CallFunc`, i.e. INSIDE the arithmetic
//  path the contract covers.  It is safe there because it returns a
//  stored `double` bit for bit and does no arithmetic of its own, and
//  that is a property any change to it must keep.
//
//  HOW MUCH OF THAT CONTRACT IS ACTUALLY TESTED -- stated exactly,
//  because an earlier draft of this comment overstated it and the
//  overstatement was repeated in three other files.  It said
//  TextureExpressionVMTest's "846 checks bit-compare RunAny's results
//  with `==`".
//
//  MEASURED, by counting the file: it has ~254 CheckClose call sites and
//  EVERY ONE OF THEM IS A TOLERANCE -- 1e-9 and 1e-12 dominate, with a
//  handful at 1e-6 and about nineteen at 1e-15.  Not one is an exact
//  compare, 1e-15 included: that band is tight, but a one-ulp shift on an
//  O(1) value is ~2e-16 and passes straight through it.  The `==`
//  comparisons of a VM RESULT are about FIFTEEN, all in ONE block --
//  tests/TextureExpressionVMTest.cpp ~:3463-4444, i.e.
//  TestFbmFootprintFadeBitIdentityAtZero, TestExpressionVMFwEndToEnd, the
//  six TestFbmDomainScale* tests and the four TestPoDomain* twins.  (The
//  file's other `==` uses compare result TYPES, parse-error OFFSETS,
//  param-spec strings, and the stochastic-tile / scatter determinism
//  flags -- not arithmetic.)
//
//  SO THE HONEST CLAIM IS NARROW.  A one-ulp shift inside fbm /
//  turbulence / ridged with a live `fw`, or inside the compile-time
//  domain-scale analysis that feeds them, IS caught.  A one-ulp shift in
//  perlin, worley, ramp, mix, dot, cross, length or normalize -- or in
//  the triplanar / voronoi rows -- is NOT: every one of those lands
//  inside a tolerance and the suite stays green at 846.  No other suite
//  closes the gap either; SurfaceSignalsTest and friends band their own
//  checks at 1e-9 / 1e-12.
//
//  AND THERE IS DELIBERATELY NO WHOLE-VM GOLDEN to close it.  A recorded
//  table of expected bit patterns would be a PLATFORM artefact, not a
//  contract: the production macOS build is `-ffast-math`
//  (build/make/rise/Config.OSX), so the compiler may contract multiplies
//  and adds into FMAs, reassociate, and vectorise differently per target,
//  per compiler version and per inlining decision.  Such a golden would
//  go red on the next Xcode with nothing wrong, and the only way to keep
//  it green would be to re-record it -- which is not a test.  The narrow
//  `==` block above survives precisely because it compares two
//  evaluations WITHIN one translation unit and one build, where those
//  freedoms cancel.
//
//  SO, AFTER ANY CHANGE to L1, to SignalQuery, or to what either of them
//  is inlined into: TextureExpressionVMTest green at its full 846 is
//  NECESSARY BUT NOT SUFFICIENT.  Read the diff for arithmetic that moved
//  into or out of the memo path, and keep L1 doing exactly what it does
//  today -- returning a stored `double` bit for bit, with no arithmetic
//  of its own.
//
//  WHAT MAKES IT SOUND.  A compiled ExpressionProgram is a PURE function
//  of (program, ExprEvalContext): the VM holds no mutable state, every
//  builtin is deterministic, and the SDF signal estimators are const
//  functions of the geometry's immutable state (their per-hit sample-set
//  rotation is a HASH OF THE HIT POSITION, SDFGeometry::
//  SignalRotationIndex, not a draw from an RNG -- so it is part of the
//  key, not a hidden input).  The mesh family answers from a table that
//  is built once and immutable thereafter.  The one thing the key cannot
//  see is a PROVIDER'S OWN STATE changing behind a stable pointer, which
//  is what the generation counter below is for.
//
//  NOTHING HERE MUTATES SCENE STATE.  All storage is `thread_local`,
//  fixed-size, and never heap-allocated; the hit record is untouched, no
//  `mutable` is added anywhere, and the compiled program stays stateless
//  and `const`.  Painters remain pure functions of (hit, scene) -- the
//  memo is an implementation detail of evaluating one, exactly like the
//  stack-local `env[]` array already is.
//
//  GENERATION.  Within one render pass the scene is immutable
//  (docs/ARCHITECTURE.md), so every table is valid for the whole pass.
//  Between passes anything may change, so every seam at which scene
//  state can move bumps a process-wide generation counter and each
//  thread drops its tables the next time it looks.  See Invalidate()'s
//  own comment for the enumerated bump sites.
//
//  KILL SWITCH.  `expression_memo` in the RISE_OPTIONS_FILE (default
//  ON), read the same way `render_thread_reserve_count` is.  It is the
//  A/B lever the memo was accepted on, and it stays as the debugging
//  aid: if a render ever disagrees with itself, `expression_memo false`
//  says in one run whether the memo is why.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_MEMO_
#define EXPRESSION_MEMO_

#include "Math3D/Math3D.h"
#include "../Interfaces/IOptions.h"
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace RISE
{
	namespace ExpressionMemo
	{
		//! ASSOCIATIVITY of the two tables, in entries.  Both are tiny,
		//! fully-associative and scanned linearly -- at these sizes a
		//! linear scan of exactly-compared keys beats any hashing, and it
		//! removes the "two different hits collide in one bucket" failure
		//! mode a direct-mapped table would have.
		//!
		//! MEASURED, not guessed.  On `plank_closeup` (421.5 M L1 probes,
		//! 320.3 M L2 probes, instrumented with a temporary counter that
		//! derived the whole curve from each hit's age):
		//!
		//!            k=1      k=2      k=4      k=8      k=16
		//!    L1     48.1 %   96.2 %   96.2 %   96.2 %   96.2 %
		//!    L2     34.5 %   78.3 %   82.7 %   84.9 %   84.9 %
		//!
		//! L1 SATURATES AT TWO because the working set at one hit is
		//! exactly the two distinct queries the body makes (`occlusion(r)`
		//! and `convexity(r')`), alternating.  It is set to FOUR anyway:
		//! the two spare entries cost nothing measurable (a miss scans at
		//! most four keys and exits on the first mismatching field) and
		//! they are what keeps a three- or four-signal body off the cliff
		//! -- a round-robin table sized exactly to a cyclic working set
		//! drops to 0 % the moment the cycle grows by one.
		//!
		//! L2 SATURATES AT EIGHT, but four already captures 82.7 of the
		//! 84.9 points available.  Four is taken because the last 2.2
		//! points would be paid for by four extra key comparisons on every
		//! MISS -- and a miss is the case that must stay cheap, since it
		//! is what a scene the memo does not suit pays.
		const int kL1Ways = 4;
		const int kL2Ways = 4;

		//! THE PROCESS-WIDE GENERATION.  Starts at 1 so a zero-initialised
		//! per-thread table (generation 0) is always stale on first look
		//! and resets itself without a separate "initialised" flag.
		inline std::atomic<unsigned long long>& GenerationRef()
		{
			static std::atomic<unsigned long long> g( 1 );
			return g;
		}

		//! ACQUIRE, matching Invalidate()'s release.  A thread that sees a
		//! new generation here must also see every write the bumping thread
		//! made before bumping -- which is the whole content of "bump AFTER
		//! the mutation" (see Invalidate()).  Free in practice: on x86-64
		//! and arm64 an acquire load of a naturally-aligned word is the
		//! same instruction as a relaxed one, and this is the only atomic
		//! on the memo's hot path.
		inline unsigned long long Generation()
		{
			return GenerationRef().load( std::memory_order_acquire );
		}

		//! DROP EVERY THREAD'S MEMO.  Call at every seam where scene state
		//! can change between render passes; within a pass the scene is
		//! immutable so nothing needs to call it.  Cheap (one
		//! release-ordered atomic increment) and safe to over-call -- the
		//! cost of a spurious bump is one cold table per thread, never a
		//! wrong answer.
		//!
		//! THE ENUMERATED BUMP SITES:
		//!   * Scene::SetSceneTime            -- every keyframed mutation
		//!                                       between animation frames
		//!   * RayCaster::AttachScene         -- a different scene, or the
		//!                                       same one with freshly
		//!                                       realized geometry
		//!   * ObjectManager::PrepareForRendering
		//!                                    -- objects realized, TLAS
		//!                                       and hierarchy re-baked
		//!   * TriangleMeshGeometryIndexed::InvalidateSignalBakes
		//!                                    -- vertex data moved under a
		//!                                       baked mesh provider, so
		//!                                       that provider's answers
		//!                                       change behind an
		//!                                       unchanged key
		//!   * PixelBasedRasterizerHelper::RasterizeScene /
		//!     ::RasterizeSceneAnimation / ::RenderFrameOfAnimation
		//!                                    -- the belt-and-braces one:
		//!                                       a render pass STARTS, so
		//!                                       whatever the editor or an
		//!                                       agent tool did since the
		//!                                       last one is covered even
		//!                                       if it moved state through
		//!                                       a seam not listed above.
		//!
		//! THE COMPLETENESS ARGUMENT rests on that last site plus the
		//! scene-immutability rule: any mutation must happen BETWEEN
		//! passes, and every pass begins by bumping.  The four specific
		//! sites above it are defence in depth, for a non-render consumer
		//! (a picking query, an agent tool, an editor derive) that mutates
		//! and then reads WITHOUT starting a render pass.
		//!
		//! EVERY ONE OF THOSE FOUR BUMPS AFTER ITS MUTATION, and three of
		//! them BEFORE it as well; each site says which and why.  The
		//! trailing bump is the load-bearing half everywhere; the entry
		//! bump is load-bearing at exactly ONE of the three.
		//!
		//!   * AFTER is the half that cannot be dropped, at any site.  A
		//!     bump that lands only BEFORE the mutation is a hazard: a
		//!     concurrent reader adopts the new generation, misses (its
		//!     tables were just dropped), asks a provider still holding
		//!     the OLD state, and inserts that stale answer stamped NEW --
		//!     where nothing will ever drop it.  Bumping last leaves every
		//!     table filled from old state on the old generation.  It is
		//!     taken on EVERY exit, including an exceptional one, via
		//!     DropOnScopeExit below.
		//!   * BEFORE is load-bearing at Scene::SetSceneTime ONLY, and for
		//!     a specific reason: that seam's own work regenerates the
		//!     photon maps, and `Regenerate()` traces REAL rays through
		//!     the scene -- shading at real hits, against real providers,
		//!     with a populated `ri.signals` -- after the animator has
		//!     already moved the keyframes (SDF part fields included).
		//!     Without the entry bump that tracing could answer from the
		//!     PREVIOUS frame's tables.
		//!   * BEFORE at ObjectManager::PrepareForRendering and
		//!     RayCaster::AttachScene is DEFENCE IN DEPTH, not a fix for
		//!     any mechanism known to exist.  An earlier draft justified
		//!     them by "the realize pass evaluates painters, and a
		//!     displacement painter can be an expression that reads
		//!     geometry signals" -- that mechanism CANNOT reach a stale
		//!     entry.  Realize-time displacement has exactly two
		//!     evaluators: GeometryUtilities.cpp's
		//!     ApplyScalarHeightToObject builds its
		//!     `RayIntersectionGeometric` with `ri.signals`
		//!     DEFAULT-CONSTRUCTED -- null provider, primId -1, as its own
		//!     comment spells out -- and so does HairGenerator's
		//!     MakeRootRi; while ApplyDisplacementMapToObject takes an
		//!     IFunction2D and calls `Evaluate(u,v)`, which has no hit
		//!     record at all.  None of them keys an L1 entry against a
		//!     provider; what they can read is the null-provider NEUTRAL,
		//!     which is a constant.
		//!     Nor can they read a stale L2 entry: `m_time` is IN the L2
		//!     key, and a painter's `param`s are compile-time constants
		//!     folded into a program whose id is minted fresh by every
		//!     Finalize.  The two bumps are kept anyway -- they cost one
		//!     atomic increment at a seam that is rebuilding a TLAS -- but
		//!     do not delete the trailing bump on the strength of them.
		//!
		//! TriangleMeshGeometryIndexed::InvalidateSignalBakes is the one
		//! that has only the trailing bump: it evaluates nothing at all.
		//! Reaching the window the trailing bump closes needs another
		//! thread running during the mutation, which is the documented
		//! mid-pass `EvaluateAtTime` motion-blur path below.
		//!
		//! THE IN-FLIGHT WINDOW THAT ORDERING CANNOT CLOSE.  A lookup is
		//! not atomic with its insert: L1Find/L2Find, then the provider or
		//! VM call, then L1Insert/L2Insert.  A bump landing anywhere in
		//! that gap leaves the insert stamping a PRE-bump value with the
		//! POST-bump generation, and it survives until the next bump.  It
		//! is the same off-thread reachability as the paragraph above --
		//! within a pass nothing bumps, and between passes the mutating
		//! thread is alone -- and closing it would cost a re-read of the
		//! generation and a compare on every insert, on the hot path, to
		//! buy nothing for any caller that respects scene immutability.
		//! Note it, do not pay for it; a caller that DOES mutate under
		//! live readers has already broken a bigger rule.
		//!
		//! THE GUI'S PAINTER PREVIEW is safe, but NOT because of any of the
		//! bumps above -- none of them fires on a preview, which neither
		//! sets a scene time nor prepares an object manager nor attaches a
		//! ray caster.  It is safe for two reasons of its own.  (1)
		//! `SceneEditor/PainterPreview.cpp`'s `MakePreviewRi` leaves
		//! `ri.signals` DEFAULT-constructed -- null provider, primId -1 --
		//! so a preview never keys an L1 entry against a provider at all;
		//! what it memoises is the null-provider NEUTRAL, which is a
		//! constant.  (2) Re-authoring a painter RECOMPILES it, and every
		//! Finalize mints a new program id (NewProgramId below), so a
		//! preview of the edited painter cannot read the old one's L2
		//! entries -- the id, not the address, is what the key carries.
		//!
		//! SO: stamping a REAL provider into `MakePreviewRi` -- previewing
		//! against the selected object's geometry, say -- would put the
		//! preview inside the memo's provider-keyed half, and it would
		//! then need a bump of its own wherever that geometry can move
		//! between previews.  Do not add one without adding the other.
		//!
		//! THE ONE KNOWN HOLE, and it is pre-existing: motion blur's
		//! per-sample `EvaluateAtTime` moves keyframed values DURING a
		//! pass (a documented race that predates this memo).  For an
		//! expression that is a hazard only if a keyframed value could
		//! change the program's output without changing its context --
		//! and it essentially cannot: `param`s are compile-time constants
		//! folded into the program (a re-authored one is a NEW compile
		//! with a NEW id), and ExpressionPainter's only keyframable field
		//! is `time`, which is IN the key.
		//!
		//! WHAT BOUNDS THE REMAINING CASE -- an SDF part's own field moving
		//! behind a key that did not.  It is NOT "a moved geometry moves
		//! `P`/`Po`/`ptObject`": that argument fails for exactly the case
		//! in question, a hit on part *j* while part *i* moves, because
		//! both estimators read the whole field within the query radius R,
		//! so part *i* can change part *j*'s answer without touching
		//! part *j*'s hit position at all.  The real bound is the SAMPLING
		//! one: `EvaluateAtTime` is called from INSIDE the per-sample loop
		//! (PathTracingPelRasterizer.cpp, the `for( sample )` body), and
		//! every sample carries its own sub-pixel jitter, so two temporal
		//! samples that produce a BIT-IDENTICAL `ptObject` -- which is what
		//! a collision needs -- essentially do not occur.  Not provably
		//! impossible; the same window the pre-existing motion-blur race
		//! already opens for everything else on that path.
		//!
		//! THE BUMP IS A RELEASE.  That is what makes "bump AFTER the
		//! mutation" mean anything to a concurrent reader: the release
		//! publishes every write the mutating thread did before it, and
		//! Refresh()'s ACQUIRE load below is the matching half, so a thread
		//! that observes the new generation is guaranteed to observe the
		//! mutation too.  With both sides relaxed the paragraphs above
		//! would be arguing happens-before with nothing to establish it.
		//! On x86-64 and arm64 the acquire load compiles to the same plain
		//! load a relaxed one does, so the hot path pays nothing.
		inline void Invalidate()
		{
			GenerationRef().fetch_add( 1, std::memory_order_release );
		}

		//! RAII form of Invalidate(), for the three seams that must bump on
		//! the way OUT -- Scene::SetSceneTime,
		//! ObjectManager::PrepareForRendering and RayCaster::AttachScene.
		//!
		//! FACTORED HERE because a plain trailing `Invalidate();` statement
		//! is the wrong shape at all three: every one of them does work
		//! that can THROW (a realize pass, a `CreateBVH`, a photon-map
		//! `Regenerate`), and an exception would skip the trailing bump --
		//! leaving exactly the state the AFTER argument above says must not
		//! exist, tables filled from mid-mutation state on a generation
		//! nothing will ever drop.  A destructor takes the bump on every
		//! exit, normal or exceptional.
		struct DropOnScopeExit
		{
			~DropOnScopeExit() { Invalidate(); }
		};

		//! TEST/OVERRIDE hook for the kill switch: -1 = "no override, read
		//! the option", 0 = force off, 1 = force on.  Setting it bumps the
		//! generation so every thread re-reads it -- without that, a table
		//! that is already warm would keep serving hits after the switch
		//! was thrown.
		inline std::atomic<int>& EnableOverrideRef()
		{
			static std::atomic<int> v( -1 );
			return v;
		}

		//! Is the memo enabled?  Consulted ONLY when a thread's table is
		//! stale (once per generation per thread), never per lookup -- the
		//! hot path reads a plain `bool` out of the thread-local table.
		inline bool EnabledSlow()
		{
			const int ov = EnableOverrideRef().load( std::memory_order_relaxed );
			if( ov >= 0 ) return ov != 0;
			// Function-local static: the options file is read exactly once
			// per process, on the first stale-table look, never at static
			// init (where GlobalOptions() would not yet be usable).
			static const bool fromOptions = GlobalOptions().ReadBool( "expression_memo", true );
			return fromOptions;
		}

		inline void SetEnabledOverride( const int tri )
		{
			EnableOverrideRef().store( tri, std::memory_order_relaxed );
			Invalidate();
		}

		//! PROCESS-UNIQUE, MONOTONIC PROGRAM IDENTITY, handed out at
		//! compile time.  It is NOT the program's address: an editor
		//! session frees a painter and allocates the next one at the same
		//! address routinely, and a `this`-keyed memo would then serve the
		//! OLD program's values for the NEW one.  A 64-bit counter cannot
		//! wrap in any realistic session.
		//!
		//! A COPY of a compiled program keeps its source's id, which is
		//! correct: the copy has identical code and identical baked
		//! params, so it is the same function.  That is what lets
		//! ExpressionPainter hold the program BY VALUE and still share
		//! memo entries with the program it was built from.
		inline unsigned long long NewProgramId()
		{
			static std::atomic<unsigned long long> n( 0 );
			return n.fetch_add( 1, std::memory_order_relaxed ) + 1ull;
		}

		//! The part of a hit's SurfaceSignalInfo that a provider's answer
		//! can depend on -- every field of it, compared EXACTLY.  Held as
		//! plain scalars rather than as a SurfaceSignalInfo so this header
		//! stays BELOW the Interfaces layer (ISurfaceSignalProvider.h
		//! includes this, not the other way round).
		//!
		//! EXACT comparison, never a hash: a hash collision here would
		//! serve one hit's occlusion at another hit's position, which is a
		//! silent wrong render.  The keys are small enough that comparing
		//! them costs less than hashing them.
		struct SignalHitKey
		{
			const void*	pProvider;
			double		ptx, pty, ptz;
			double		nx, ny, nz;
			double		baryA, baryB;
			int			primId;
			bool		bComplementedField;

			//! FIELDS THIS COMPARES: 11.  Counted here, next to the body,
			//! and summed into kProgramKeyFields below -- which is what
			//! ExpressionProgram::Builder::ComputeMemoWorthiness uses as
			//! its instruction-count threshold, so the two cannot drift.
			//! ADDING A FIELD ABOVE MEANS ADDING IT TO Equals AND
			//! INCREMENTING THIS.
			static const int kFields = 11;

			bool Equals( const SignalHitKey& o ) const
			{
				return pProvider == o.pProvider
					&& ptx == o.ptx && pty == o.pty && ptz == o.ptz
					&& primId == o.primId
					&& baryA == o.baryA && baryB == o.baryB
					&& nx == o.nx && ny == o.ny && nz == o.nz
					&& bComplementedField == o.bComplementedField;
			}
		};

		//! L1 key: the hit, plus WHICH query is being asked of it.
		struct SignalKey
		{
			SignalHitKey	hit;
			double			radius;
			int				fn;				//!< 0 = occlusion, 1 = thickness, 2 = convexity
			bool			bRadiusIsConstant;

			//! FIELDS THIS COMPARES: 3 of its own plus the hit's 11.
			static const int kFields = 3 + SignalHitKey::kFields;

			bool Equals( const SignalKey& o ) const
			{
				// `fn` first: the working set at one hit is one entry per
				// distinct query the body makes, so this is the field that
				// usually decides a mismatch.
				return fn == o.fn
					&& radius == o.radius
					&& bRadiusIsConstant == o.bRadiusIsConstant
					&& hit.Equals( o.hit );
			}
		};

		//! WHICH PAINTER PIPE filled an L2 entry -- part of the key, so the
		//! two pipes can never share one.
		//!
		//! IT IS NOT DECORATION, and the reason is the FP contract at the
		//! top of this file.  On a SCALAR-typed program the two pipes call
		//! DIFFERENT VM entry points: the colour pipe's
		//! ExpressionPainter::EvalRGB always calls `EvalVec3` (and
		//! broadcasts), while ExpressionScalarPainter::GetValuesAt calls
		//! `Eval` -- deliberately, because each pipe must keep the entry
		//! point it had before the memo existed.  Under `-ffast-math`
		//! those two are free to differ in the last ulp.
		//!
		//! AND ONE PROGRAM REALLY CAN REACH BOTH PIPES.  The API is the
		//! route: RISE_API_CreateExpressionPainter and
		//! RISE_API_CreateExpressionScalarPainter each take an
		//! `ExpressionProgram` by const reference and each painter holds a
		//! COPY -- and a copy keeps its source's id (NewProgramId's
		//! comment says why that is correct), so one compiled scalar-typed
		//! program handed to both factories would, without this field,
		//! give the scalar pipe the colour pipe's `EvalVec3(...).x`.  That
		//! is the entry-point substitution the painters' own comments
		//! forbid.  Keying it apart costs one int compare on a miss (and
		//! 32 bytes of TLS across the table) and removes it outright.
		//!
		//! HOW MUCH OF THAT IS OBSERVED, honestly.  The SUBSTITUTION is
		//! structural and certain -- without the field the two pipes
		//! genuinely share one entry, which is verifiable by reading the
		//! key.  The DIVERGENCE is not: measured 2026-09-07 on this
		//! toolchain, removing this field leaves ExpressionMemoTest (m)
		//! GREEN, because `Eval` and `EvalVec3` reach the same `RunAny`
		//! call over the same `env[]` and produce the same bits here.
		//! So this field is a guard against a COMPILER FREEDOM, not a fix
		//! for a divergence anyone has seen between these two entry
		//! points.  It is kept because that freedom has already bitten
		//! this exact code once (the FP contract at the top of this file
		//! records the ulp move that drove the memo out of Eval/EvalVec3
		//! in the first place), because the cost is a rounding error, and
		//! because "these two happen to agree on the machine I built on"
		//! is not a property a cache key should rest on.  Do not remove it
		//! on the strength of (m) staying green without it.
		enum EvalPipe
		{
			ePipeColour = 0,	//!< ExpressionPainter (IPainter)
			ePipeScalar = 1		//!< ExpressionScalarPainter (IScalarPainter)
		};

		//! L2 key: which program, evaluated at exactly which context, by
		//! which pipe.  Every ExprEvalContext field is here -- u, v, P, Po,
		//! N, fw, fwo, time, curv, curvR and the whole signal channel.
		//! ADDING A CONTEXT VARIABLE TO ExprEvalContext AND NOT ADDING IT
		//! HERE IS A SILENT WRONG RENDER; ExpressionEval.h's kContextSlot*
		//! checklist names ExpressionProgram::MakeMemoKey, which fills
		//! this, among the sites that must be touched.
		struct ProgramKey
		{
			unsigned long long	progId;
			int					pipe;			//!< an EvalPipe -- see above
			double				u, v;
			double				Px, Py, Pz;
			double				Pox, Poy, Poz;
			double				Nx, Ny, Nz;
			double				fw, fwo, time, curv, curvR;
			SignalHitKey		signals;

			//! FIELDS THIS COMPARES: 18 of its own (progId, pipe, u, v,
			//! P.xyz, Po.xyz, N.xyz, fw, fwo, time, curv, curvR) plus the
			//! hit channel's 11 = 29.  THE NUMBER IS LOAD-BEARING, not
			//! decoration: ExpressionProgram::Builder::ComputeMemoWorthiness
			//! uses it as the instruction count at or above which a body
			//! cannot be cheaper to re-run than to look up.  Adding a
			//! context variable means a field here, a compare in the body
			//! below, and a bump of the 18.
			static const int kFields = 18 + SignalHitKey::kFields;

			bool Equals( const ProgramKey& o ) const
			{
				// Ordered most-discriminating first: two consumers at one
				// hit differ ONLY in progId, and the relief stencil's taps
				// differ ONLY in P / Po / (u,v) -- so a miss is usually
				// decided within the first few compares.  `pipe` rides
				// beside progId because the two together are "which
				// function is this".
				return progId == o.progId
					&& pipe == o.pipe
					&& Px == o.Px && Py == o.Py && Pz == o.Pz
					&& u == o.u && v == o.v
					&& Pox == o.Pox && Poy == o.Poy && Poz == o.Poz
					&& Nx == o.Nx && Ny == o.Ny && Nz == o.Nz
					&& fw == o.fw && fwo == o.fwo && time == o.time
					&& curv == o.curv && curvR == o.curvR
					&& signals.Equals( o.signals );
			}
		};

		//! One thread's tables.  `thread_local`, so there is no sharing, no
		//! locking and no false sharing between render workers; and no
		//! heap, so a thread pays exactly sizeof(Tables) once, in its TLS
		//! block, for the life of the process.
		//!
		//! DELIBERATELY A TRIVIALLY-CONSTRUCTIBLE AGGREGATE -- no
		//! constructor, no member initialisers.  A `thread_local` with a
		//! non-trivial constructor is initialised behind a per-access guard
		//! variable, which would put a load-and-branch on the hot path of
		//! every lookup; a trivial one is plain zero-initialised TLS with
		//! no guard at all.  Zero is the right cold state anyway:
		//! `gen == 0` never equals the live generation (which starts at 1),
		//! so the first look is always stale and Refresh() fills it in.
		struct Tables
		{
			unsigned long long	gen;		//!< generation these entries were filled under
			bool				enabled;	//!< kill switch, re-read whenever `gen` moves

			int					l1Next;		//!< round-robin victim
			int					l1Fill;		//!< entries written so far (< kL1Ways while warming)
			SignalKey			l1Key[ kL1Ways ];
			double				l1Val[ kL1Ways ];

			int					l2Next;
			int					l2Fill;
			ProgramKey			l2Key[ kL2Ways ];
			double				l2Val[ kL2Ways ][ 3 ];
		};

		//! THE TWO PROPERTIES THE PARAGRAPH ABOVE CLAIMS, ASSERTED so they
		//! cannot be lost to a well-meaning member initialiser or a
		//! non-trivial member type.  Trivially default-constructible is
		//! what makes the `thread_local` below plain zero-initialised TLS
		//! with NO guard variable on the hot path; trivially destructible
		//! is what keeps it from registering a per-thread destructor
		//! (__cxa_thread_atexit) at first touch.  Neither is a
		//! micro-optimisation to taste: they are why Local() compiles to
		//! an address, and the reason the comment above is allowed to say
		//! there is no guard.
		static_assert( std::is_trivially_default_constructible<Tables>::value,
			"ExpressionMemo::Tables must stay trivially default-constructible -- a member "
			"initialiser or a non-trivial member would put a TLS guard variable on every lookup" );
		static_assert( std::is_trivially_destructible<Tables>::value,
			"ExpressionMemo::Tables must stay trivially destructible -- otherwise every thread "
			"registers a TLS destructor at first touch" );

		//! THE per-thread instance.
		inline Tables& Local()
		{
			static thread_local Tables t;
			return t;
		}

		//! Bring this thread's tables up to date with the current
		//! generation, and report whether the memo is live at all.
		//! Everything below calls this FIRST; in the common case it is one
		//! acquire atomic load and one compare -- and on x86-64 / arm64 an
		//! acquire load of an aligned word is the same instruction a
		//! relaxed one would be, so the ordering Invalidate() needs costs
		//! the hot path nothing.
		inline bool Refresh( Tables& t )
		{
			const unsigned long long g = Generation();
			if( t.gen != g ) {
				t.gen     = g;
				t.enabled = EnabledSlow();
				t.l1Fill  = 0; t.l1Next = 0;
				t.l2Fill  = 0; t.l2Next = 0;
			}
			return t.enabled;
		}

		//! L1 lookup.  Returns true and writes `out` on a hit.
		inline bool L1Find( const SignalKey& k, Scalar& out )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return false;
			for( int i = 0; i < t.l1Fill; ++i ) {
				if( t.l1Key[i].Equals( k ) ) { out = (Scalar)t.l1Val[i]; return true; }
			}
			return false;
		}

		//! L1 insert.  Round-robin victim: the access pattern this memo
		//! exists for is a short BURST of a few distinct keys repeated many
		//! times at one hit, and for that, round-robin over a table big
		//! enough to hold the working set is indistinguishable from LRU and
		//! costs no bookkeeping.  Only a MISS reaches here.
		inline void L1Insert( const SignalKey& k, const Scalar v )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return;
			const int slot = t.l1Next;
			t.l1Key[ slot ] = k;
			t.l1Val[ slot ] = (double)v;
			t.l1Next = ( slot + 1 ) % kL1Ways;
			if( t.l1Fill < kL1Ways ) ++t.l1Fill;
		}

		inline bool L2Find( const ProgramKey& k, Vector3& out )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return false;
			for( int i = 0; i < t.l2Fill; ++i ) {
				if( t.l2Key[i].Equals( k ) ) {
					out.x = (Scalar)t.l2Val[i][0];
					out.y = (Scalar)t.l2Val[i][1];
					out.z = (Scalar)t.l2Val[i][2];
					return true;
				}
			}
			return false;
		}

		inline void L2Insert( const ProgramKey& k, const Vector3& v )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return;
			const int slot = t.l2Next;
			t.l2Key[ slot ] = k;
			t.l2Val[ slot ][0] = (double)v.x;
			t.l2Val[ slot ][1] = (double)v.y;
			t.l2Val[ slot ][2] = (double)v.z;
			t.l2Next = ( slot + 1 ) % kL2Ways;
			if( t.l2Fill < kL2Ways ) ++t.l2Fill;
		}

		//! Bytes of thread-local storage one render worker pays.  Reported
		//! by ExpressionMemoTest so the memory claim cannot drift from the
		//! struct.
		inline std::size_t BytesPerThread() { return sizeof( Tables ); }
	}
}

#endif
