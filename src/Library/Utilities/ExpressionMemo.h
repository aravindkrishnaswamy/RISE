//////////////////////////////////////////////////////////////////////
//
//  ExpressionMemo.h - Thread-local per-hit memo for the texture-
//  expression VM and geometry shading signals.
//
//  References:
//    docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md
//    docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
//    docs/WETNESS_COAT_DESIGN.md
//    docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md
//
//  LEVELS:
//    L1 (signal builtins): keyed on (fn, radius, bRadiusIsConstant,
//      SignalHitKey).  Hits across repeated signal calls at one hit and
//      across ReliefModifier's 4-tap stencil (which moves P/Po/(u,v)
//      while holding `signals` constant).
//    L2 (whole program): keyed on (progId, pipe, ExprEvalContext,
//      SignalHitKey, optional PainterSampleHitKey, optional
//      SpaceTransformHitKey).  Hits across multiple consumers at one hit
//      and per-wavelength spectral GetColorNM.
//    AO (1-way last-hit cache): caches ray-traced `SurfaceSignalInfo::
//      AmbientOcclusion` across ReliefModifier taps and material slots.
//
//  FP & SOUNDNESS CONTRACT:
//    - L2 sits OUTSIDE ExpressionProgram::Eval / EvalVec3 so -ffast-math
//      inlining and FMA contraction inside the VM are not altered.
//    - L1 sits inside SignalQuery (called from CallFunc) and must return
//      stored `double`s bit-for-bit with no arithmetic of its own.
//    - All floating-point key fields compare bit-for-bit via
//      SameDoubleBits / SameFloatBits so -0.0 and +0.0 stay distinct
//      under -ffast-math / -fno-signed-zeros.
//    - Compiled programs are pure functions of (program, ExprEvalContext)
//      with attach-time-immutable bound painter pointers and immutable
//      post-build signal providers.  All storage is fixed-size, trivially
//      constructible `thread_local` (no heap, no scene mutation).
//    - Scene state is immutable within a render pass (docs/ARCHITECTURE.md);
//      between-pass mutations bump the generation counter via Invalidate().
//    - Kill switch: `expression_memo` in RISE_OPTIONS_FILE (default ON).
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
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace RISE
{
	namespace ExpressionMemo
	{
		//! Fully-associative linear-scan table sizes (exact keys, no hash
		//! collisions).  L1 holds up to `kL1Ways` distinct `(fn, radius, hit)`
		//! pairs per hit before cyclic round-robin eviction collapses hits;
		//! L2 stays at 4 to keep miss scans cheap.  `sizeof(Tables)` must stay
		//! within the 4096 B per-thread TLS ceiling checked by `ExpressionMemoTest`.
		const int kL1Ways = 8;
		const int kL2Ways = 4;

		//! Process-wide generation counter.  Starts at 1 so zero-initialised
		//! per-thread tables (`gen == 0`) are always stale on first access.
		inline std::atomic<unsigned long long>& GenerationRef()
		{
			static std::atomic<unsigned long long> g( 1 );
			return g;
		}

		//! Acquire load pairing with `Invalidate()`'s release store so readers
		//! observing a new generation also see prior mutations.
		inline unsigned long long Generation()
		{
			return GenerationRef().load( std::memory_order_acquire );
		}

		//! Invalidates every thread's memo tables (release-ordered increment).
		//! Safe to over-call.  Called at all between-pass mutation seams:
		//!   * `Scene::SetSceneTime` (before — photon-map `Regenerate()` traces
		//!     live rays — and after)
		//!   * `ObjectManager::PrepareForRendering`
		//!   * `RayCaster::AttachScene`
		//!   * `TriangleMeshGeometryIndexed::InvalidateSignalBakes`
		//!   * `PixelBasedRasterizerHelper::{RasterizeScene, RasterizeSceneAnimation,
		//!     RenderFrameOfAnimation}` (pass-start catch-all)
		//!
		//! Always bump AFTER mutation (via `DropOnScopeExit` on throwing seams)
		//! so concurrent readers cannot stamp pre-mutation results with the new
		//! generation.  Painter preview (`PainterPreview.cpp::MakePreviewRi`)
		//! needs no bump while `ri.signals` is null-provider and re-authoring
		//! mints a new `progId`; attaching a live provider there would require
		//! its own bump.  Mid-pass motion-blur `EvaluateAtTime` relies on `time`
		//! in the L2 key and sub-pixel jitter (gated on `pSamples && pFilter`
		//! in `PixelBasedRasterizerHelper::SubSampleRays`).
		inline void Invalidate()
		{
			GenerationRef().fetch_add( 1, std::memory_order_release );
		}

		//! RAII scope-exit invalidator for throwing mutation seams
		//! (`SetSceneTime`, `PrepareForRendering`, `AttachScene`).
		struct DropOnScopeExit
		{
			~DropOnScopeExit() { Invalidate(); }
		};

		//! Test override for the kill switch: -1 = read option, 0 = force off,
		//! 1 = force on.  `SetEnabledOverride` bumps generation so warm tables refresh.
		inline std::atomic<int>& EnableOverrideRef()
		{
			static std::atomic<int> v( -1 );
			return v;
		}

		//! Consulted only on stale-table refresh (once per generation per thread).
		//! Safe from render workers: `GlobalOptions()` is initialized single-threaded
		//! before thread-pool spawn, and the function-local static serializes reads.
		inline bool EnabledSlow()
		{
			const int ov = EnableOverrideRef().load( std::memory_order_relaxed );
			if( ov >= 0 ) return ov != 0;
			static const bool fromOptions = GlobalOptions().ReadBool( "expression_memo", true );
			return fromOptions;
		}

		inline void SetEnabledOverride( const int tri )
		{
			EnableOverrideRef().store( tri, std::memory_order_relaxed );
			Invalidate();
		}

		//! Process-unique monotonic 64-bit program ID (never pointer address,
		//! preventing ABA reuse after recompilation).  Copies of a compiled
		//! program keep the same ID because their code and baked params match.
		inline unsigned long long NewProgramId()
		{
			static std::atomic<unsigned long long> n( 0 );
			return n.fetch_add( 1, std::memory_order_relaxed ) + 1ull;
		}

		//! Bit-exact IEEE-754 double comparison separating `-0.0` from `+0.0`
		//! even under `-ffast-math` / `-fno-signed-zeros`.
		inline bool SameDoubleBits( const double a, const double b )
		{
			static_assert( sizeof( double ) == sizeof( std::uint64_t ),
				"SameDoubleBits requires 64-bit IEEE double" );
			std::uint64_t ba, bb;
			std::memcpy( &ba, &a, sizeof( ba ) );
			std::memcpy( &bb, &b, sizeof( bb ) );
			return ba == bb;
		}

		inline bool SameFloatBits( const float a, const float b )
		{
			static_assert( sizeof( float ) == sizeof( std::uint32_t ),
				"SameFloatBits requires 32-bit IEEE float" );
			std::uint32_t ba, bb;
			std::memcpy( &ba, &a, sizeof( ba ) );
			std::memcpy( &bb, &b, sizeof( bb ) );
			return ba == bb;
		}

		//! Exact-compared `SurfaceSignalInfo` fields (plain scalars so this
		//! header stays below `Interfaces/`).  No hashing: collisions would
		//! silently return the wrong hit's signal.
		struct SignalHitKey
		{
			const void*	pProvider;
			double		ptx, pty, ptz;
			double		nx, ny, nz;
			double		baryA, baryB;
			int			primId;
			bool		bComplementedField;
			//! Cross-object fields (`docs/CROSS_OBJECT_PROXIMITY_DESIGN.md` §5.1)
			//! separating instanced/animated hits for `proximity(r)`.
			const void*	pScene;
			const void*	pSelf;
			double		wx, wy, wz;		//!< ptWorld
			double		time;

			//! Compared field count (11 own-surface + 6 cross-object); summed
			//! into `ProgramKey::kFields`.  Update alongside fields and `Equals`.
			static const int kFields = 17;

			bool Equals( const SignalHitKey& o ) const
			{
				// Cross-object fields compared last (constant within a single hit).
				return pProvider == o.pProvider
					&& SameDoubleBits( ptx, o.ptx ) && SameDoubleBits( pty, o.pty ) && SameDoubleBits( ptz, o.ptz )
					&& primId == o.primId
					&& SameDoubleBits( baryA, o.baryA ) && SameDoubleBits( baryB, o.baryB )
					&& SameDoubleBits( nx, o.nx ) && SameDoubleBits( ny, o.ny ) && SameDoubleBits( nz, o.nz )
					&& bComplementedField == o.bComplementedField
					&& pScene == o.pScene && pSelf == o.pSelf
					&& SameDoubleBits( wx, o.wx ) && SameDoubleBits( wy, o.wy ) && SameDoubleBits( wz, o.wz )
					&& SameDoubleBits( time, o.time );
			}
		};

		//! Extra `RayIntersectionGeometric` fields read by bound painters via
		//! `sample()` / `sample_scalar()` that `ProgramKey` does not already key
		//! (`ptCoord`, `P`, `Po`, `N`, `signals`, `derivatives`, and `txFootprint`
		//! widths are already keyed).  Guarded by `SourceHygieneTest`'s painter
		//! `ri`-read census — extend both if a painter reads a new `ri` field.
		struct PainterSampleHitKey
		{
			//! False when no hit record exists (synthetic fallback); other fields 0.
			bool	hasHit;
			double	dudx, dudy, dvdx, dvdy;
			bool	footprintValid;
			double	rdx, rdy, rdz;			//!< ray.Dir()
			double	u1, v1;					//!< ptCoord1
			bool	hasTexCoord1;
			double	vcr, vcg, vcb;			//!< vColor
			bool	hasVertexColor;

			//! Compared field count (summed into `ProgramKey::kFields`).
			static const int kFields = 16;

			//! Ordered cheapest-discriminator first (UV Jacobian, then ray dir).
			bool Equals( const PainterSampleHitKey& o ) const
			{
				return hasHit == o.hasHit
					&& SameDoubleBits( dudx, o.dudx ) && SameDoubleBits( dudy, o.dudy )
					&& SameDoubleBits( dvdx, o.dvdx ) && SameDoubleBits( dvdy, o.dvdy )
					&& footprintValid == o.footprintValid
					&& SameDoubleBits( rdx, o.rdx ) && SameDoubleBits( rdy, o.rdy ) && SameDoubleBits( rdz, o.rdz )
					&& SameDoubleBits( u1, o.u1 ) && SameDoubleBits( v1, o.v1 )
					&& hasTexCoord1 == o.hasTexCoord1
					&& SameDoubleBits( vcr, o.vcr ) && SameDoubleBits( vcg, o.vcg ) && SameDoubleBits( vcb, o.vcb )
					&& hasVertexColor == o.hasVertexColor;
			}
		};

		//! Per-hit transform, camera-frame, geometry-normal, and `ObjectInfoData`
		//! key for programs calling Group 7 space-transform, object-info, or
		//! ray/facing builtins (`osl_to_*`, `osl_object_*`, `osl_ao`,
		//! `osl_backfacing`, `osl_is_camera_ray`).  Short-circuited when unused.
		struct SpaceTransformHitKey
		{
			const void*			pWorldToObject;
			const void*			pObjectToWorld;
			unsigned long long	globalPoseEpoch;
			unsigned long long	threadPoseEpoch;
			bool				hasCamera;
			signed char			geomOpposesArrival;		//!< -1=opposes, +1=same-side, 0=no hit (`osl_ao` / `osl_true_normal`)
			bool				geomBackfacing;			//!< true geometric backfacing honoring `bGeomNormalOrientedToRay` / `HasTrueGeomSide()`
			bool				isCameraRay;			//!< pHit->bIsCameraRay (`osl_is_camera_ray`, fits in 1-byte alignment hole before particleRand -> 0 struct growth!)
			float				particleRand;			//!< pHit->pObjectInfo->particleRandom
			double				camOx, camOy, camOz;
			double				camRx, camRy, camRz;
			double				camUx, camUy, camUz;
			double				camFx, camFy, camFz;
			double				gnx, gny, gnz;			//!< pHit->vGeomNormal (`osl_ao` facing & standoff)
			double				n0x, n0y, n0z;			//!< pre-modifier shading normal N0 (`osl_ao` / `osl_shading_normal`)
			float				objR, objG, objB, objA;
			float				objIdx, objRand;

			static const int kFields = 33;

			bool Equals( const SpaceTransformHitKey& o ) const
			{
				return pWorldToObject == o.pWorldToObject
					&& pObjectToWorld == o.pObjectToWorld
					&& globalPoseEpoch == o.globalPoseEpoch
					&& threadPoseEpoch == o.threadPoseEpoch
					&& hasCamera == o.hasCamera
					&& geomOpposesArrival == o.geomOpposesArrival
					&& geomBackfacing == o.geomBackfacing
					&& isCameraRay == o.isCameraRay
					&& SameDoubleBits( camOx, o.camOx ) && SameDoubleBits( camOy, o.camOy ) && SameDoubleBits( camOz, o.camOz )
					&& SameDoubleBits( camRx, o.camRx ) && SameDoubleBits( camRy, o.camRy ) && SameDoubleBits( camRz, o.camRz )
					&& SameDoubleBits( camUx, o.camUx ) && SameDoubleBits( camUy, o.camUy ) && SameDoubleBits( camUz, o.camUz )
					&& SameDoubleBits( camFx, o.camFx ) && SameDoubleBits( camFy, o.camFy ) && SameDoubleBits( camFz, o.camFz )
					&& SameDoubleBits( gnx, o.gnx ) && SameDoubleBits( gny, o.gny ) && SameDoubleBits( gnz, o.gnz )
					&& SameDoubleBits( n0x, o.n0x ) && SameDoubleBits( n0y, o.n0y ) && SameDoubleBits( n0z, o.n0z )
					&& SameFloatBits( objR, o.objR ) && SameFloatBits( objG, o.objG ) && SameFloatBits( objB, o.objB )
					&& SameFloatBits( objA, o.objA ) && SameFloatBits( objIdx, o.objIdx )
					&& SameFloatBits( objRand, o.objRand ) && SameFloatBits( particleRand, o.particleRand );
			}
		};

		//! L1 key: the hit, plus WHICH query is being asked of it.
		struct SignalKey
		{
			SignalHitKey	hit;
			double			radius;
			int				fn;				//!< 0 = occlusion, 1 = thickness, 2 = convexity, 3 = proximity, 4 = interior
			bool			bRadiusIsConstant;

			//! FIELDS THIS COMPARES: 3 of its own plus the hit's 17 = 20.
			static const int kFields = 3 + SignalHitKey::kFields;

			bool Equals( const SignalKey& o ) const
			{
				// `fn` first: the working set at one hit is one entry per
				// distinct query the body makes, so this is the field that
				// usually decides a mismatch.
				return fn == o.fn
					&& SameDoubleBits( radius, o.radius )
					&& bRadiusIsConstant == o.bRadiusIsConstant
					&& hit.Equals( o.hit );
			}
		};

		//! Separates colour (`EvalVec3`) and scalar (`Eval`) painter pipes in L2
		//! so a scalar program shared across both factories never cross-serves
		//! results between entry points that `-ffast-math` may contract differently.
		enum EvalPipe
		{
			ePipeColour = 0,	//!< ExpressionPainter (IPainter)
			ePipeScalar = 1		//!< ExpressionScalarPainter (IScalarPainter)
		};

		//! L2 key: `(progId, pipe)` plus every `ExprEvalContext` field.
		//! Adding a variable to `ExprEvalContext` requires updating this struct,
		//! `Equals()`, `kFields`/`kFieldsNonSampling`, and `ExpressionProgram::MakeMemoKey`
		//! (see `ExpressionEval.h`'s `kContextSlot*` checklist).
		struct ProgramKey
		{
			unsigned long long	progId;
			int					pipe;			//!< EvalPipe
			double				u, v;
			double				Px, Py, Pz;
			double				Pox, Poy, Poz;
			double				Nx, Ny, Nz;
			double				fw, fwo, time, curv, curvR;
			SignalHitKey		signals;
			PainterSampleHitKey	sampleHit;		//!< short-circuited when !usesPainterSample
			bool				usesPainterSample;
			SpaceTransformHitKey	spaceHit;	//!< short-circuited when !usesSpaceTransform
			bool				usesSpaceTransform;

			//! Worst-case compared field count (note: `signals.ptWorld` mirrors
			//! `P` so L1/L2 share one `SignalHitKey` layout,
			//! `docs/CROSS_OBJECT_PROXIMITY_DESIGN.md` §5.1).
			static const int kFields = 20 + SignalHitKey::kFields + PainterSampleHitKey::kFields + SpaceTransformHitKey::kFields;

			//! Fields compared for non-sampling, non-space-transform programs;
			//! used as the instruction-count gate in `Builder::ComputeMemoWorthiness`.
			static const int kFieldsNonSampling = 20 + SignalHitKey::kFields;

			bool Equals( const ProgramKey& o ) const
			{
				// Most-discriminating fields first (progId, pipe, P, uv, Po).
				// `sampleHit` and `spaceHit` short-circuit when unused so unread
				// hit fields cannot cause spurious misses/evictions.
				return progId == o.progId
					&& pipe == o.pipe
					&& SameDoubleBits( Px, o.Px ) && SameDoubleBits( Py, o.Py ) && SameDoubleBits( Pz, o.Pz )
					&& SameDoubleBits( u, o.u ) && SameDoubleBits( v, o.v )
					&& SameDoubleBits( Pox, o.Pox ) && SameDoubleBits( Poy, o.Poy ) && SameDoubleBits( Poz, o.Poz )
					&& SameDoubleBits( Nx, o.Nx ) && SameDoubleBits( Ny, o.Ny ) && SameDoubleBits( Nz, o.Nz )
					&& SameDoubleBits( fw, o.fw ) && SameDoubleBits( fwo, o.fwo ) && SameDoubleBits( time, o.time )
					&& SameDoubleBits( curv, o.curv ) && SameDoubleBits( curvR, o.curvR )
					&& signals.Equals( o.signals )
					&& usesPainterSample == o.usesPainterSample
					&& ( !usesPainterSample || sampleHit.Equals( o.sampleHit ) )
					&& usesSpaceTransform == o.usesSpaceTransform
					&& ( !usesSpaceTransform || spaceHit.Equals( o.spaceHit ) );
			}
		};

		//! 1-way last-hit cache key for ray-traced `SurfaceSignalInfo::AmbientOcclusion`.
		struct AOCacheKey
		{
			const void*        pScene;
			const void*        pSelf;
			const void*        pProvider;
			unsigned long long globalPoseEpoch;
			unsigned long long threadPoseEpoch;
			int                primId;
			int                numSamples;
			int                flags;
			double             wx, wy, wz;
			double             ox, oy, oz;
			double             time;
			double             dist;
			double             hnx, hny, hnz;
			double             odx, ody, odz;

			bool Equals( const AOCacheKey& o ) const
			{
				return pScene == o.pScene && pSelf == o.pSelf && pProvider == o.pProvider
					&& globalPoseEpoch == o.globalPoseEpoch && threadPoseEpoch == o.threadPoseEpoch
					&& primId == o.primId && numSamples == o.numSamples && flags == o.flags
					&& SameDoubleBits( wx, o.wx ) && SameDoubleBits( wy, o.wy ) && SameDoubleBits( wz, o.wz )
					&& SameDoubleBits( ox, o.ox ) && SameDoubleBits( oy, o.oy ) && SameDoubleBits( oz, o.oz )
					&& SameDoubleBits( time, o.time ) && SameDoubleBits( dist, o.dist )
					&& SameDoubleBits( hnx, o.hnx ) && SameDoubleBits( hny, o.hny ) && SameDoubleBits( hnz, o.hnz )
					&& SameDoubleBits( odx, o.odx ) && SameDoubleBits( ody, o.ody ) && SameDoubleBits( odz, o.odz );
			}
		};

		//! Per-thread TLS tables.  Must remain trivially default-constructible
		//! and trivially destructible (zero-initialized TLS with no per-access
		//! init guard or `__cxa_thread_atexit`; `gen == 0` triggers `Refresh()`).
		struct Tables
		{
			unsigned long long	gen;		//!< generation these entries were filled under
			bool				enabled;	//!< kill switch, re-read whenever `gen` moves
			bool				aoValid;	//!< 1-way AO cache entry valid under `gen`

			int					l1Next;		//!< round-robin victim
			int					l1Fill;		//!< entries written so far (< kL1Ways while warming)
			SignalKey			l1Key[ kL1Ways ];
			double				l1Val[ kL1Ways ];

			int					l2Next;
			int					l2Fill;
			ProgramKey			l2Key[ kL2Ways ];
			double				l2Val[ kL2Ways ][ 3 ];

			AOCacheKey			aoKey;
			double				aoVal;
		};

		static_assert( std::is_trivially_default_constructible<Tables>::value,
			"ExpressionMemo::Tables must stay trivially default-constructible -- a member "
			"initialiser or a non-trivial member would put a TLS guard variable on every lookup" );
		static_assert( std::is_trivially_destructible<Tables>::value,
			"ExpressionMemo::Tables must stay trivially destructible -- otherwise every thread "
			"registers a TLS destructor at first touch" );

		inline Tables& Local()
		{
			static thread_local Tables t;
			return t;
		}

		//! Syncs this thread's tables to the current generation and returns whether
		//! the memo is enabled (one acquire load + compare on the hot path).
		inline bool Refresh( Tables& t )
		{
			const unsigned long long g = Generation();
			if( t.gen != g ) {
				t.gen     = g;
				t.enabled = EnabledSlow();
				t.aoValid = false;
				t.l1Fill  = 0; t.l1Next = 0;
				t.l2Fill  = 0; t.l2Next = 0;
			}
			return t.enabled;
		}

		//! L1 lookup; returns true and writes `out` on hit.
		inline bool L1Find( const SignalKey& k, Scalar& out )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return false;
			for( int i = 0; i < t.l1Fill; ++i ) {
				if( t.l1Key[i].Equals( k ) ) { out = (Scalar)t.l1Val[i]; return true; }
			}
			return false;
		}

		//! L1 round-robin insert (called on miss only).
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

		inline bool AOFind( const AOCacheKey& k, Scalar& out )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return false;
			if( t.aoValid && t.aoKey.Equals( k ) ) {
				out = (Scalar)t.aoVal;
				return true;
			}
			return false;
		}

		inline void AOInsert( const AOCacheKey& k, const Scalar v )
		{
			Tables& t = Local();
			if( !Refresh( t ) ) return;
			t.aoKey = k;
			t.aoVal = (double)v;
			t.aoValid = true;
		}

		//! Bytes of thread-local storage one render worker pays.  Reported
		//! by ExpressionMemoTest so the memory claim cannot drift from the
		//! struct.
		inline std::size_t BytesPerThread() { return sizeof( Tables ); }
	}
}

#endif
