//////////////////////////////////////////////////////////////////////
//
//  ISurfaceSignalProvider.h - Per-hit shading signal channel between
//  the expression VM and hit geometries / scene queries.
//
//  Defines:
//    - ISurfaceSignalProvider: geometry-local self-signal interface
//      (occlusion, thickness, convexity, named attributes).
//    - SurfaceSignalInfo: per-hit record carrying object-space provider
//      data and world-space cross-object data (Proximity, Interior,
//      AmbientOcclusion declared here, defined in SurfaceSignalProximity.h
//      to avoid an include cycle with IObjectManager.h).
//    - SurfaceSignalDemand / ProximityDemand: relaxed-atomic demand gates.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_SURFACE_SIGNAL_PROVIDER_
#define I_SURFACE_SIGNAL_PROVIDER_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/SurfaceCurvature.h"
#include "../Utilities/ExpressionMemo.h"
#include "ILog.h"
#include <type_traits>
#include <atomic>

namespace RISE
{
	struct SurfaceSignalInfo;
	// Forward-declared to break the IObjectManager -> IObject -> RayIntersection -> this cycle;
	// cross-object methods are defined in SurfaceSignalProximity.h.
	class IObjectManager;
	class IObject;
	class ISurfaceSignalProvider;

	//! Per-thread self-primitive exclusion state for ray-traced AmbientOcclusion
	//! (trivially zero-initializable TLS for zero overhead on non-AO shadow rays).
	struct SelfPrimExclusionState
	{
		const IObject*                pSelf;
		const ISurfaceSignalProvider* pProvider;
		int                           primIdPlusOne; // 0 == none; primId + 1 otherwise
		bool                          activeForCurrentGeometry;
	};
	static_assert( std::is_trivially_default_constructible<SelfPrimExclusionState>::value,
		"SelfPrimExclusionState must be trivially zero-initializable for zero-overhead TLS" );

	//! Relaxed-atomic active AO self-exclusion scope count (gates TLS access on shadow rays).
	extern std::atomic<unsigned int> g_surfaceSignalSelfExclusionActive;

	inline bool SurfaceSignalSelfExclusionActive()
	{
		return g_surfaceSignalSelfExclusionActive.load( std::memory_order_relaxed ) != 0u;
	}

	//! Single-TU TLS accessors (defined in ExpressionOslBuiltins.cpp to prevent DSO duplication).
	SelfPrimExclusionState& SurfaceSignalSelfExclusion();
	bool& SurfaceSignalInAmbientOcclusion();

	struct ScopedSelfPrimExclusion
	{
		SelfPrimExclusionState prev;
		bool                   active;

		ScopedSelfPrimExclusion( const IObject* pSelf_, const ISurfaceSignalProvider* pProvider_, const int primId_ )
			: prev()
			, active( pSelf_ != nullptr && primId_ >= 0 )
		{
			if( active ) {
				prev = SurfaceSignalSelfExclusion();
				g_surfaceSignalSelfExclusionActive.fetch_add( 1u, std::memory_order_relaxed );
				SelfPrimExclusionState& cur = SurfaceSignalSelfExclusion();
				cur.pSelf = pSelf_;
				cur.pProvider = pProvider_;
				cur.primIdPlusOne = primId_ + 1;
				cur.activeForCurrentGeometry = false;
			}
		}
		~ScopedSelfPrimExclusion()
		{
			if( active ) {
				SurfaceSignalSelfExclusion() = prev;
				g_surfaceSignalSelfExclusionActive.fetch_sub( 1u, std::memory_order_relaxed );
			}
		}

		ScopedSelfPrimExclusion( const ScopedSelfPrimExclusion& ) = delete;
		ScopedSelfPrimExclusion& operator=( const ScopedSelfPrimExclusion& ) = delete;
		ScopedSelfPrimExclusion( ScopedSelfPrimExclusion&& ) = delete;
		ScopedSelfPrimExclusion& operator=( ScopedSelfPrimExclusion&& ) = delete;
	};

	//! Geometry-side interface for object-local occlusion, thickness, convexity, and
	//! named attributes.
	//! - Space & units: queries are in the geometry's object space; `radiusFraction` is
	//!   a dimensionless fraction of bbox diagonal; outputs are dimensionless in [0, 1].
	//! - Refusal: returning false (leaving `outValue` untouched) substitutes the signal's
	//!   neutral value. Baked providers require `bRadiusIsConstant == true` (compile-time proof).
	//! - Thread safety & lifetime: must be concurrency-safe (pure const or mutex-serialized
	//!   deterministic lazy bake), never access the scene, and only stamp non-owning `this`.
	class ISurfaceSignalProvider
	{
	public:
		virtual ~ISurfaceSignalProvider() {}

		//! Cosine-weighted outward-hemisphere self-visibility in [0,1] (1 = unoccluded;
		//! planar and convex features read 1). Returns false if unable to answer.
		virtual bool ComputeOcclusion(
			const SurfaceSignalInfo& hit,	///< [in] Hit in this provider's object space
			const Scalar radiusFraction,	///< [in] Query radius as fraction of geometry characteristic size
			const bool bRadiusIsConstant,	///< [in] Compile-time constant-radius proof
			Scalar& outValue				///< [out] Occlusion in [0,1], 1 = unoccluded
			) const = 0;

		//! Solid thickness behind `ptObject` normalized by query radius: min(w/R, 1)
		//! in [0,1] (1 = thick). Returns false if unable to answer.
		virtual bool ComputeThickness(
			const SurfaceSignalInfo& hit,	///< [in] Hit in this provider's object space
			const Scalar radiusFraction,	///< [in] Query radius as fraction of geometry characteristic size
			const bool bRadiusIsConstant,	///< [in] Compile-time constant-radius proof
			Scalar& outValue				///< [out] Thickness in [0,1], 1 = thick
			) const = 0;

		//! Ball-volume convexity `clamp(2A - 1, 0, 1)` in [0,1] (0 = flat/concave,
		//! 0.5 = 90-deg arris, 0.75 = 3-face corner, 1 = knife edge). Self-signal only.
		virtual bool ComputeConvexity(
			const SurfaceSignalInfo& hit,	///< [in] Hit in this provider's object space
			const Scalar radiusFraction,	///< [in] Query radius as fraction of geometry characteristic size
			const bool bRadiusIsConstant,	///< [in] Compile-time constant-radius proof
			Scalar& outValue				///< [out] Convexity in [0,1], 0 = flat/concave
			) const = 0;

		//! Evaluates a named geometry attribute into linear `outRGBA[0..3]` (default false).
		//! When `pQueryObjPos` is non-null (e.g. ReliefModifier finite-difference tap),
		//! mesh providers project onto the hit triangle plane for shifted barycentrics.
		virtual bool EvaluateNamedAttribute(
			const SurfaceSignalInfo& /*hit*/,
			const char* /*name*/,
			Scalar /*outRGBA*/[4],
			const Point3* /*pQueryObjPos*/ = 0
			) const
		{
			return false;
		}
	};

	//! Per-hit signal record: stamped in object space by hit geometry (left untransformed
	//! by Object/CSGObject) plus world-space scene context stamped by ObjectManager::IntersectRay
	//! (or LightSampler::ProbeEmitterSurface / BDPTVertex rebuild).
	struct SurfaceSignalInfo
	{
		const ISurfaceSignalProvider*	pProvider;	//!< non-owning geometry back-pointer (0 = none)
		Point3							ptObject;	//!< hit point in provider's object space
		Vector3							nObject;	//!< outward unit normal in provider's object space

		//! Hit primitive index in provider (-1 for positional providers like SDFGeometry),
		//! with barycentrics: v0 + (v1-v0)*baryA + (v2-v0)*baryB.
		int								primId;
		Scalar							baryA;
		Scalar							baryB;

		//! Toggled on CSG subtraction subtrahends so SDF queries invert solid/empty sense
		//! (honoured by SDF family; ignored by baked meshes).
		bool							bComplementedField;

		// Cross-object scene context (non-owning; null pScene => neutral 0).
		const IObjectManager*			pScene;		//!< owning object manager (0 = none)
		const IObject*					pSelf;		//!< == ri.pObject
		Point3							ptWorld;	//!< world-space hit point (== ri.ptIntersection)
		Scalar							time;		//!< painter's m_time (0 on scalar painters)

		SurfaceSignalInfo() :
		pProvider( 0 ), ptObject( 0, 0, 0 ), nObject( 0, 0, 0 ),
		primId( -1 ), baryA( 0 ), baryB( 0 ), bComplementedField( false ),
		pScene( 0 ), pSelf( 0 ), ptWorld( 0, 0, 0 ), time( 0 )
		{
		}

		// Neutral ("do-nothing") fallback values when a signal is absent or refused.
		static Scalar NeutralOcclusion() { return Scalar( 1 ); }
		static Scalar NeutralThickness() { return Scalar( 1 ); }
		static Scalar NeutralConvexity() { return Scalar( 0 ); }
		static Scalar NeutralProximity() { return Scalar( 0 ); }
		static Scalar NeutralInterior() { return Scalar( 0 ); }

		//! True iff radiusFraction is finite and strictly positive.
		static bool RadiusUsable( const Scalar radiusFraction )
		{
			return RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) )
				&& radiusFraction > Scalar( 0 );
		}

		//! Signal discriminator for dispatch and L1 memo key separation.
		enum SignalKind { eOcclusion = 0, eThickness = 1, eConvexity = 2, eProximity = 3, eInterior = 4 };

		//! Builds the L1 memo hit key from EVERY field of SurfaceSignalInfo (update
		//! ExpressionMemoTest when adding fields).
		ExpressionMemo::SignalHitKey MemoHitKey() const
		{
			ExpressionMemo::SignalHitKey k;
			k.pProvider = (const void*)pProvider;
			k.ptx = (double)ptObject.x; k.pty = (double)ptObject.y; k.ptz = (double)ptObject.z;
			k.nx  = (double)nObject.x;  k.ny  = (double)nObject.y;  k.nz  = (double)nObject.z;
			k.baryA = (double)baryA;    k.baryB = (double)baryB;
			k.primId = primId;
			k.bComplementedField = bComplementedField;
			k.pScene = (const void*)pScene;
			k.pSelf  = (const void*)pSelf;
			k.wx = (double)ptWorld.x; k.wy = (double)ptWorld.y; k.wz = (double)ptWorld.z;
			k.time = (double)time;
			return k;
		}

		//! Shared L1 memo key builder for self-signals and cross-object signals.
		ExpressionMemo::SignalKey MakeL1Key( const SignalKind fn, const Scalar radius, const bool bRadiusIsConstant ) const
		{
			ExpressionMemo::SignalKey key;
			key.hit = MemoHitKey();
			key.radius = (double)radius;
			key.fn = (int)fn;
			key.bRadiusIsConstant = bRadiusIsConstant;
			return key;
		}

		//! Cross-object proximity `clamp(1 - d/r, 0, 1)` in world units (1 = touching,
		//! 0 = none within r). Defined in SurfaceSignalProximity.h.
		Scalar Proximity( const Scalar radiusWorld ) const;

		//! Cross-object interior depth `clamp(depth/r, 0, 1)` in world units (0 = outside,
		//! 1 = >= r deep). Defined in SurfaceSignalProximity.h.
		Scalar Interior( const Scalar radiusWorld ) const;

		//! Ray-traced ambient occlusion (`osl_ao`), returning unoccluded cosine-weighted
		//! fraction in [0, 1] (1 on fallback/re-entry). `flags`: bit 0 = inside,
		//! bit 1 = only_local (pSelf only, ignoring casts_shadows), bit 2 = has_custom_normal.
		//! Defined in SurfaceSignalProximity.h.
		Scalar AmbientOcclusion(
			const Scalar distanceWorld,
			const int numSamples,
			const Vector3& customNormal,
			const int flags,
			const Vector3& shadingNormal,
			const Vector3& geomNormal,
			const bool geomOpposesArrival
			) const;

		//! Shared L1-memoized self-signal query with neutral fallback and [0,1] clamp.
		Scalar SignalQuery( const SignalKind fn, const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			const ExpressionMemo::SignalKey key = MakeL1Key( fn, radiusFraction, bRadiusIsConstant );

			Scalar memo = Scalar( 0 );
			if( ExpressionMemo::L1Find( key, memo ) ) return memo;

			const Scalar neutral = ( fn == eOcclusion ) ? NeutralOcclusion()
			                     : ( fn == eThickness ) ? NeutralThickness()
			                                            : NeutralConvexity();
			Scalar out = neutral;
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) ) {
				const bool answered = ( fn == eOcclusion ) ? pProvider->ComputeOcclusion( *this, radiusFraction, bRadiusIsConstant, v )
				                    : ( fn == eThickness ) ? pProvider->ComputeThickness( *this, radiusFraction, bRadiusIsConstant, v )
				                                           : pProvider->ComputeConvexity( *this, radiusFraction, bRadiusIsConstant, v );
				if( answered && RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
					out = ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
				}
			}
			ExpressionMemo::L1Insert( key, out );
			return out;
		}

		//! Finite occlusion in [0,1] (NeutralOcclusion() on absence/refusal; bRadiusIsConstant required).
		Scalar Occlusion( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eOcclusion, radiusFraction, bRadiusIsConstant );
		}

		//! Finite thickness in [0,1] (NeutralThickness() on absence/refusal).
		Scalar Thickness( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eThickness, radiusFraction, bRadiusIsConstant );
		}

		//! Finite convexity in [0,1] (NeutralConvexity() on absence/refusal).
		Scalar Convexity( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eConvexity, radiusFraction, bRadiusIsConstant );
		}

		//! Named geometry attribute into linear `outRGBA[0..3]` (zeroed on miss; non-finite -> 0).
		bool NamedAttribute( const char* name, Scalar outRGBA[4], const Point3* pQueryObjPos = 0 ) const
		{
			outRGBA[0] = outRGBA[1] = outRGBA[2] = outRGBA[3] = Scalar( 0 );
			if( !pProvider || !name || !name[0] ) {
				return false;
			}
			Scalar tmp[4] = { Scalar( 0 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ) };
			if( !pProvider->EvaluateNamedAttribute( *this, name, tmp, pQueryObjPos ) ) {
				return false;
			}
			for( int c = 0; c < 4; ++c ) {
				outRGBA[c] = RISE::IsFiniteDouble( static_cast<double>( tmp[c] ) ) ? tmp[c] : Scalar( 0 );
			}
			return true;
		}
	};

	//! Process-wide relaxed-atomic demand counter for self/cross-object signals
	//! (gates LightSampler::ProbeEmitterSurface via EmitterProbeWanted()).
	namespace SurfaceSignalDemand
	{
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}

	//! Process-wide relaxed-atomic demand counter for proximity()/interior(); gates eager
	//! ObjectManager world-AABB snapshot build/checks (NearestOtherSurface still builds
	//! lazily on demand if unregistered).
	namespace ProximityDemand
	{
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}

}

#endif
