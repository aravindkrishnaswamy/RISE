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

		inline unsigned long long Generation()
		{
			return GenerationRef().load( std::memory_order_relaxed );
		}

		//! DROP EVERY THREAD'S MEMO.  Call at every seam where scene state
		//! can change between render passes; within a pass the scene is
		//! immutable so nothing needs to call it.  Cheap (one relaxed
		//! atomic increment) and safe to over-call -- the cost of a
		//! spurious bump is one cold table per thread, never a wrong
		//! answer.
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
		//! passes, and every pass begins by bumping.  The three specific
		//! sites above it are defence in depth, and they are what makes a
		//! non-render consumer (the GUI's painter preview, a picking
		//! query) safe as well.
		//!
		//! THE ONE KNOWN HOLE, and it is pre-existing: motion blur's
		//! per-sample `EvaluateAtTime` moves keyframed values DURING a
		//! pass (a documented race that predates this memo).  For an
		//! expression that is a hazard only if a keyframed value could
		//! change the program's output without changing its context --
		//! and it essentially cannot: `param`s are compile-time constants
		//! folded into the program (a re-authored one is a NEW compile
		//! with a NEW id), ExpressionPainter's only keyframable field is
		//! `time`, which is IN the key, and a moved geometry moves
		//! `P`/`Po`/`ptObject`, which are in the key too.  What a mid-pass
		//! keyframe CAN move invisibly is an SDF part's own field behind an
		//! unchanged `ptObject` -- vanishingly unlikely (the hit position
		//! moves with the part) but not provably impossible, and it is the
		//! same window the pre-existing race already opens for everything
		//! else on that path.
		inline void Invalidate()
		{
			GenerationRef().fetch_add( 1, std::memory_order_relaxed );
		}

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

		//! L2 key: which program, evaluated at exactly which context.
		//! Every ExprEvalContext field is here -- u, v, P, Po, N, fw, fwo,
		//! time, curv, curvR and the whole signal channel.  ADDING A
		//! CONTEXT VARIABLE TO ExprEvalContext AND NOT ADDING IT HERE IS A
		//! SILENT WRONG RENDER; ExpressionEval.h's kContextSlot* checklist
		//! names ExpressionProgram::MakeMemoKey, which fills this, among
		//! the sites that must be touched.
		struct ProgramKey
		{
			unsigned long long	progId;
			double				u, v;
			double				Px, Py, Pz;
			double				Pox, Poy, Poz;
			double				Nx, Ny, Nz;
			double				fw, fwo, time, curv, curvR;
			SignalHitKey		signals;

			bool Equals( const ProgramKey& o ) const
			{
				// Ordered most-discriminating first: two consumers at one
				// hit differ ONLY in progId, and the relief stencil's taps
				// differ ONLY in P / Po / (u,v) -- so a miss is usually
				// decided within the first few compares.
				return progId == o.progId
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

		//! THE per-thread instance.
		inline Tables& Local()
		{
			static thread_local Tables t;
			return t;
		}

		//! Bring this thread's tables up to date with the current
		//! generation, and report whether the memo is live at all.
		//! Everything below calls this FIRST; in the common case it is one
		//! relaxed atomic load and one compare.
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
