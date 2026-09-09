//////////////////////////////////////////////////////////////////////
//
//  MeshSignalBake.h - PER-VERTEX self-occlusion, thickness and convexity
//  bakes for triangle meshes: the mesh half of the geometry-derived
//  shading signals (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §7, Phase 3;
//  docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §4 for convexity).
//
//  Two things live here:
//
//    MeshSignalBake::Build    the three bake ALGORITHMS, expressed against
//                             a plain vertex/normal array plus an
//                             any-hit callback, so they know nothing
//                             about mesh classes and can be tested on
//                             their own.
//    MeshSignalBakeCache      the LAZY, geometry-owned find-or-build the
//                             design doc §7.3 settled on -- a `mutable`
//                             table map behind an RMutex, following the
//                             SSS point-set precedent (ARCHITECTURE.md
//                             §Known Exceptions).
//
//  WHY LAZY, restated where the code is: these signals are read ONLY
//  from material shading, so a bake at the Realize seam would be paid by
//  whichever render came first, whether or not that render's materials
//  ever query a signal.  Building on first READ makes the bake fire
//  exactly once, and only when some shading actually asks.  CORRECTED
//  2026-09-08: an earlier draft of this comment claimed the agent's
//  `quality:"draft"` preview "executes no material shading at all" and
//  was therefore bake-free by construction; that is false -- draft
//  shades every hit with the material's own BSDF under a fixed studio
//  rig (InteractivePelRasterizer's PreviewPel), so a draft render of a
//  signal-consuming mesh material DOES reach this bake, once.  What
//  laziness buys is "once, on demand", not "never in draft".
//  MeshSignalBake::BuildCounter() exists so a test can prove the
//  once-on-demand claim instead of restating it.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MESH_SIGNAL_BAKE_
#define MESH_SIGNAL_BAKE_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Threads/Threads.h"
#include <vector>
#include <atomic>
#include <memory>

namespace RISE
{
	namespace MeshSignalBake
	{
		//! Which per-vertex field a table holds.  One table per (geometry,
		//! kind, radius).
		enum Kind
		{
			eOcclusion	= 0,
			eThickness	= 1,
			//! The `convexity(radius)` builtin
			//! (docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §4.2).  A
			//! SEPARATE bake rather than a post-process of eOcclusion, and
			//! that is forced, not stylistic: the occlusion bake samples a
			//! cosine-weighted OUTWARD HEMISPHERE, which saturates at 1 for
			//! a plane and for every convex feature alike and so carries no
			//! convexity information at all.  This one samples the FULL
			//! sphere uniformly, which is the mesh's form of the same
			//! accessibility `A` the SDF family measures by ball volume.
			eConvexity	= 2,
			eKindCount	= 3
		};

		//! Rays cast per vertex, per signal.  A COMPILE-TIME CONSTANT, not a
		//! scene parameter: design doc §9 admits a sample count only as an
		//! advanced argument on stochastic signals, and v1 deliberately
		//! ships without one -- an author tuning ray counts is an author
		//! who has been handed the renderer's problem.  64 is Substance's
		//! own AO-baker default, which is the number the surveyed tools
		//! (and therefore the text an LLM has read) treat as "enough".
		const int kRayCount = 64;

		//! Half-angle of the INWARD cone the thickness bake samples, in
		//! radians (20 degrees).  See Build()'s implementation notes for why
		//! it is a narrow cone and not the full inward hemisphere: a
		//! hemisphere's cosine-weighted mean of 1/cos(theta) is exactly 2,
		//! so a slab of width w would read 2w -- double the interface's
		//! documented "a slab of width w read at radius R gives min(w/R,1)".
		//! At 20 degrees the same mean is 1.031, i.e. a bounded ~3 %
		//! overestimate on an ideal slab, in exchange for seeing the nearest
		//! wall in a NEIGHBOURHOOD rather than along one degenerate
		//! direction.
		const Scalar kThicknessConeHalfAngle = Scalar( 0.34906585039886590 );

		//! Ray-origin offset along the TRACE axis (outward for occlusion and
		//! convexity, inward for thickness), as a fraction of the mesh's
		//! bounding-box diagonal.  A bake ray starts ON the surface, at a
		//! vertex shared by every incident triangle, so it must be lifted off
		//! it or those triangles answer "hit" for free.  Scale-relative for
		//! the same reason every other length in this feature is.
		//!
		//! IT IS ALSO THE SOURCE OF CONVEXITY'S ONE BIAS: on a flat surface a
		//! ray aimed just below the horizon escapes rather than hitting
		//! whenever |cos theta| < this / maxDistance, so a flat mesh reads
		//! convexity ~= 2 * (this / maxDistance) instead of exactly 0.
		//! Bounded, one-sided (never negative, so a mask lights nothing) and
		//! ~0.004 at the default fraction with a 5 %-of-diagonal radius, but
		//! a bias rather than noise -- MeshSignalBakeTest (p) pins it at that
		//! bound.  Lowering this trades the bias against self-hits at the
		//! originating vertex; it is not free in either direction.
		const Scalar kOriginEpsilonFraction = Scalar( 1e-4 );

		//! Answers "does the OWNING mesh's own surface block this ray within
		//! `maxDist`?"  Boolean any-hit, never a closest hit: the bakes need
		//! nothing else, and the mesh's existing BVH already has the cheap
		//! entry point for it.
		//!
		//! SELF-OCCLUSION ONLY, and the narrowness of this interface is how
		//! that is enforced rather than merely intended -- there is no scene
		//! here to reach (design doc §8).
		class ISelfOccluder
		{
		public:
			virtual ~ISelfOccluder() {}
			virtual bool AnyHitWithin(
				const Point3& origin,		///< [in] ray origin, mesh object space
				const Vector3& unitDir,		///< [in] UNIT direction, same space
				const Scalar maxDist		///< [in] search distance
				) const = 0;

			//! Distance to the nearest self-hit within `maxDist`, or FALSE
			//! when nothing is hit.  Only the thickness bake needs it.
			virtual bool NearestHitWithin(
				const Point3& origin,
				const Vector3& unitDir,
				const Scalar maxDist,
				Scalar& outDist
				) const = 0;
		};

		//! Everything a bake reads.  All object space, all borrowed: the
		//! caller owns every array and must keep it alive across Build().
		struct Input
		{
			//! Per-POSITION vertex coordinates (`pPoints` on the indexed
			//! mesh).  The table Build() produces is indexed identically.
			const std::vector<Point3>*	pVertices;
			//! Per-POSITION outward normals, one per entry of `pVertices`,
			//! unit length or zero.  A ZERO normal marks a vertex the caller
			//! could not orient (an isolated or degenerate one); Build()
			//! writes that vertex the signal's NEUTRAL value rather than
			//! inventing a direction to trace.
			const std::vector<Vector3>*	pNormals;
			//! Query distance in object-space units -- already
			//! `radiusFraction * bounding-box diagonal`, resolved by the
			//! caller because only the geometry knows its own box.
			Scalar						maxDistance;
			//! Ray-origin lift, object-space units (see
			//! kOriginEpsilonFraction).
			Scalar						originEpsilon;
			//! The mesh's own any-hit.  Never null.
			const ISelfOccluder*		pOccluder;
		};

		//! Assembles the Input for one radius, ON DEMAND.
		//!
		//! The cache calls both of these from INSIDE its own lock, and that
		//! is the whole reason they are callbacks rather than a struct the
		//! caller fills in up front.  A mesh's input includes derived state
		//! that is built once and reused across radii (a per-POSITION normal
		//! array that its independently indexed `pNormals` cannot supply);
		//! building it eagerly, on every cache HIT, would put the very work
		//! the lazy bake exists to avoid back on the per-sample path.
		//!
		//! That derived array is OWNED BY THE CACHE, not by the source, and
		//! that ownership is load-bearing rather than tidy: it is what lets
		//! one lock acquisition invalidate the tables AND the state they were
		//! derived from.  Split across two objects, the tail of an
		//! invalidation ran unlocked and raced this very build.
		class IInputSource
		{
		public:
			virtual ~IInputSource() {}

			//! Fills `out` with ONE outward orientation per vertex position,
			//! sized to the same vertex array `MakeBakeInput` will publish;
			//! a zero entry marks a position the source could not orient.
			//! Called at most once per cache generation, on the first miss.
			virtual void BuildVertexNormals( std::vector<Vector3>& out ) const = 0;

			//! Assembles the rest of the input around the cache-owned
			//! `vertexNormals` array built above (which the source must
			//! publish as `out.pNormals` -- it stays alive for the build).
			//! \return FALSE when this geometry cannot be baked at all (no
			//!         triangles, no acceleration structure, degenerate box,
			//!         or normals that do not match the vertex array).
			virtual bool MakeBakeInput(
				const Scalar radiusFraction,
				const std::vector<Vector3>& vertexNormals,
				Input& out ) const = 0;
		};

		//! Builds one per-vertex table.  Deterministic: a fixed Hammersley
		//! pattern plus a per-vertex-index golden-ratio rotation, no RNG, no
		//! wall clock, no dependence on which thread ran it -- two runs of
		//! the same scene produce byte-identical tables, which is the
		//! difference between a bake and a source of noise.
		//!
		//! \return TRUE and fills `out` (sized to the vertex count), or
		//!         FALSE on unusable input (no vertices, mismatched normal
		//!         count, non-positive distance, no occluder), leaving `out`
		//!         unspecified.  A FALSE here is CACHED by the caller as a
		//!         null sentinel -- a failed bake must not be retried per
		//!         sample.
		bool Build( const Kind kind, const Input& in, std::vector<float>& out );

		//! Count of bakes ACTUALLY BUILT in this process, ever.  Test-visible
		//! by design: it is what turns "draft mode triggers no bake" and
		//! "eight threads racing build exactly once" from claims into
		//! assertions (design doc §13 Phase-3 exit gate).  Incremented once
		//! per completed Build() call, success or failure.
		std::atomic<unsigned int>& BuildCounter();
	}

	//! The lazy, geometry-owned bake cache.  One of these lives on each
	//! mesh geometry that publishes signals; the geometry's lifetime IS the
	//! cache key, which is what makes invalidation free under the
	//! drop-and-recreate derivation path (design doc §7.2) -- and is why
	//! this is NOT keyed on `IObject*`, whose address is REUSED in place
	//! across a geometry swap (the trap §7.2 names, and the shape of the
	//! SSS `PointSetMap` precedent's one flaw).
	class MeshSignalBakeCache
	{
	public:
		typedef std::vector<float> Table;

		//! What a query holds while it interpolates.  A SHARED, COUNTED
		//! reference, not a raw pointer, and the difference is the one
		//! use-after-free this design can actually reach.
		//!
		//! The cache's contract says invalidation never runs concurrent with
		//! rendering -- and that contract is VIOLATED today by one documented
		//! pre-existing path: motion-blur temporal sampling calls
		//! `IAnimator::EvaluateAtTime` from worker threads
		//! (ARCHITECTURE.md:68-74), so a keyframed displacement painter drives
		//! `DisplacedGeometry::RefreshMeshVertices` -> `UpdateVertices` ->
		//! `InvalidateSignalBakes` on a worker thread.  With a raw `Table*`
		//! that `delete` could land while another worker was mid-interpolation
		//! on the very same table.  A `shared_ptr` makes the table outlive the
		//! cache's reference to it, so the reader finishes on a table that is
		//! still alive and merely STALE.
		//!
		//! What this does NOT fix, said plainly: the same path also mutates
		//! the vertex array and refits the BVH under traversal.  That race is
		//! PRE-EXISTING, is what ARCHITECTURE.md documents, and remains.  This
		//! removes the NEW use-after-free Phase 3 layered on top of it -- it
		//! does not make vertex mutation from a worker thread safe.
		typedef std::shared_ptr<const Table> TableRef;

		MeshSignalBakeCache();
		~MeshSignalBakeCache();

		//! Find-or-build the table for (kind, radiusFraction).
		//!
		//! \return a reference to the immutable table, or an EMPTY reference
		//!         when this (kind, radius) has no bake and never will -- a
		//!         build that failed (cached null sentinel: warned once, never
		//!         retried) or a radius past kMaxTablesPerKind.  The caller
		//!         answers empty with the signal's neutral fallback.
		//!
		//! Serialization: ONE lock_guard spans the whole find-or-build, the
		//! SSS discipline exactly.  A double-checked unlocked find() would
		//! race with the locked insert on a std::vector (reallocation), i.e.
		//! undefined behaviour, for a saving of one uncontended lock on a
		//! path that already costs a barycentric interpolation.  Interpolation
		//! itself happens OUTSIDE the lock, on the returned immutable table.
		//!
		//! COST: exactly ONE refcount pair per query -- the copy taken under
		//! the lock, released when the caller's local goes out of scope.  The
		//! caller must hold that one reference across the whole interpolation
		//! rather than re-copying it per tap.
		TableRef FindOrBuild(
			const MeshSignalBake::Kind kind,
			const Scalar radiusFraction,
			const MeshSignalBake::IInputSource& src
			) const;

		//! Drops every table AND the per-position normals they were derived
		//! from, in ONE lock acquisition.  Required by vertex-level animation
		//! (`ITriangleMeshGeometryIndexed::UpdateVertices` replaces the
		//! vertex array in place, so the geometry OUTLIVES the shape its
		//! bake describes -- the one hole in §7.2's free-invalidation
		//! argument, and §14 item 10's "confirm whether it can happen":
		//! it can).  Must be called between frames, never concurrent with
		//! rendering, which is the same contract UpdateVertices already has.
		//!
		//! \return TRUE when something was actually live and got dropped.
		//!         The caller uses that to decide whether a contract-violation
		//!         assert is warranted: dropping an EMPTY cache (every mesh
		//!         construction path does) cannot strand a reader, while
		//!         dropping a populated one during a render is the violation
		//!         worth failing loudly on.
		bool Invalidate();

		//! Distinct radii bakeable per signal per geometry.  Only radii the
		//! expression compiler PROVED literal ever get here, so this set is
		//! bounded by the scene text and 8 is far past any real authoring;
		//! the cap exists so a pathological scene cannot grow tables without
		//! limit.  Past it, further radii are REFUSED (neutral), and which 8
		//! won is arrival-ordered -- the one non-determinism in this feature,
		//! reachable only by a scene that spells more than 8 distinct
		//! occlusion (or thickness) radii against one mesh.
		static const size_t kMaxTablesPerKind = 8;

		//! Relative tolerance for "is this the radius the table was baked
		//! at".  Two spellings of one literal must match; 5 % and 5.1 % must
		//! not, because answering the second from the first is precisely the
		//! silent wrong-scale substitution §7.1 forbids.
		static const Scalar kRadiusRelTolerance;

		//! Do these two radii name the same bake?
		static bool RadiiMatch( const Scalar a, const Scalar b );

	private:
		struct Entry
		{
			Scalar		radius;
			TableRef	table;		//!< empty == the failed-build null sentinel
		};

		mutable std::vector<Entry>	m_entries[ MeshSignalBake::eKindCount ];
		mutable RMutex				m_mutex;

		//! Per-POSITION orientations the bakes trace along, built once by the
		//! IInputSource on the first miss and reused by every later radius.
		//!
		//! It lives HERE, and not on the geometry, for one reason: it is
		//! derived from the same vertex data the tables are, so it has to be
		//! dropped by the same invalidation -- and an invalidation that
		//! dropped the tables under the lock and then cleared a
		//! geometry-owned array outside it left a tail that raced this very
		//! build.  One owner, one lock, one lifetime.
		mutable std::vector<Vector3>	m_vertexNormals;
		mutable bool					m_vertexNormalsBuilt;
		//! One warning per (geometry, kind) when the cap is reached, not one
		//! per sample.
		mutable bool				m_warnedCap[ MeshSignalBake::eKindCount ];

		MeshSignalBakeCache( const MeshSignalBakeCache& );
		MeshSignalBakeCache& operator=( const MeshSignalBakeCache& );
	};

	//! Barycentric read of a per-vertex table at a hit, in the SAME
	//! convention the mesh intersector uses for normals, UVs and vertex
	//! colours: value = v0 + (v1-v0)*a + (v2-v0)*b.
	//!
	//! Free function rather than a method so both mesh families (and any
	//! test) share one definition of "what the interpolation is".
	inline bool InterpolateMeshSignal(
		const MeshSignalBakeCache::Table& table,
		const size_t i0, const size_t i1, const size_t i2,
		const Scalar a, const Scalar b,
		Scalar& outValue )
	{
		const size_t n = table.size();
		if( i0 >= n || i1 >= n || i2 >= n ) {
			return false;
		}
		const Scalar v0 = Scalar( table[i0] );
		outValue = v0 + ( Scalar( table[i1] ) - v0 ) * a + ( Scalar( table[i2] ) - v0 ) * b;
		return true;
	}
}

#endif
