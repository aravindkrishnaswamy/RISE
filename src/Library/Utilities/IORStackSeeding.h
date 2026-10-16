//////////////////////////////////////////////////////////////////////
//
//  IORStackSeeding.h - Seed a light / eye subpath's IORStack so that
//    `containsCurrent()` reports the correct inside/outside state
//    when the subpath's origin is physically inside one or more
//    dielectric (or otherwise refractive) objects.
//
//  Why this exists
//
//    BDPT (and integrators that reuse its light subpaths, e.g. VCM)
//    used to initialize the IOR stack to just the environment IOR
//    and rely on the stack to reflect the ray's current medium.  That
//    works for rays starting in free space — the stack grows as the
//    ray enters dielectrics and shrinks as it exits them.  It FAILS
//    silently for light subpaths whose origin is a luminaire sealed
//    inside nested dielectrics, and for eye subpaths whose camera
//    sits inside a dielectric (submerged camera, camera inside a
//    medium-filled room, etc.).
//
//    Symptom: the first hit on an enclosing boundary comes back with
//    bFromInside=false (the stack doesn't know we started inside),
//    which routes DielectricSPF through its "entering" branch.  For
//    an IOR-matched inner boundary (e.g. an `air_cavity` dielectric
//    with ior=1.0 surrounding a light in a glass egg), that branch's
//    Fresnel formula returns floating-point cancellation noise
//    instead of exact zero, the transmission lobe fails the sign
//    check that assumes refracted ≠ incoming, and the path is left
//    with a reflection whose kray equals that noise.  Throughput dies
//    by ~32 orders of magnitude per bounce and no photon reaches the
//    rest of the scene.
//
//  Algorithm
//
//    Shoot a probe ray in a fixed direction from the seed point and
//    count, PER OBJECT, the net parity of exits-vs-entries along the
//    probe.  An object contains the seed iff its net parity is
//    positive (with inward closed-solid winding normalized by signed containment;
//    more exits than entries — the probe must cross its
//    boundary one more time outward than inward to leave).
//
//    Counting exits-only is INCORRECT.  A probe that passes
//    THROUGH an object (camera in front of a glass sphere, probe
//    direction going +Z through it) sees one entry and one exit;
//    counting only the exit would treat the object as containing
//    the seed.  That bug caused every BDPT/VCM/MLT sphere render
//    where the camera sat behind a glass object to compute the
//    first refraction at the sphere with reversed IORs (glass→air
//    instead of air→glass), missing ~66% of the scene radiance.
//    See `tests/IORStackSeedingRegressionTest.cpp` for the pinning
//    regression test.
//
//    Per-object tallying:
//      - cosN > 0 (probe aligned with outward normal): EXIT, parity++.
//      - cosN < 0 (probe opposite to outward normal):  ENTRY, parity--.
//
//    A single probe only decides containment for a CLOSED surface,
//    so an object is seeded only when BOTH the probe and its REVERSE
//    report positive parity (P2-4).  A closed manifold agrees in both
//    directions by construction; an OPEN sheet (a translucent leaf,
//    curtain or card -- every `translucent_material` object is a probe
//    participant since DL-46) is met by only one of the two and is
//    rejected.  The reverse probe runs only when the forward one found
//    a candidate at all.  See the rule's documented limit at its
//    implementation in `SeedFromPoint`.
//
//    Hits whose geometric normal is RAY-DERIVED rather than a static
//    surface property (`RayIntersectionGeometric::bGeomNormalRayDerived`
//    -- `HairGeometry`) are skipped outright: un-flipping such a normal
//    reads "exit" at every crossing, so a hair curtain between the seed
//    and infinity would tally unbounded positive parity.  A 1-D curve
//    has no interior to be inside.
//
//    Objects confirmed in both directions are pushed in OUTERMOST-FIRST
//    order (stack convention: bottom = outermost, top = innermost).
//    Order is determined by each containing object's FIRST exit's
//    probe-step index: smaller index = closer to seed = innermost.
//    Insertion-sort by firstExitStep descending and push in that order.
//
//    A small iteration cap prevents pathological geometry (e.g.
//    coincident surfaces, self-intersecting objects) from spinning
//    forever.  Probe direction +Z is arbitrary but deterministic; any
//    fixed direction that isn't degenerate for the scene works, and
//    we accept the rare case where a probe grazes a tangent — the
//    worst outcome is a slightly-incorrect stack at that specific
//    pathological ray, which the rest of the path walk then corrects
//    as it enters/exits real boundaries.
//
//    The probe only considers materials whose GetSpecularInfo reports
//    canRefract=true OR hasInterior=true (DL-46).  Pure reflectors
//    (mirrors) and lambertian surfaces report neither and are skipped
//    — they are not media and their "interior" is not a place rays
//    travel through with a different IOR.  `hasInterior` covers a
//    second, narrower case: a material that is NOT specular (its
//    lobes are stochastic, never delta) and carries no distinct IOR
//    of its own, but still classifies entry/exit from
//    ior_stack.containsCurrent() exactly like a refractor does —
//    TranslucentSPF's diffuse lampshade model is the motivating
//    example.  For a `canRefract` object the probe pushes that
//    object's OWN captured `ior`; for a `hasInterior`-only object it
//    instead re-pushes whatever IOR is already on the stack (see the
//    push loop below), because such a material never introduces a
//    new numeric medium — only membership changes.
//
//    DL-618: a CERTIFIED OPEN refractive sheet (`bProvablyNoInterior`)
//    encloses no volume, so it takes no part in the parity vote above.
//    Its medium (the sheet's BACK side, DL-345) holds the seed point by
//    a separate convention -- the ray from the point along the sheet's
//    own normal reaches the sheet's back face with nothing but other
//    media in between -- documented above `ClassifyMembershipHit` and
//    shared with the walks' unpushed-exit resolution
//    (`OpenSheetEnclosesCrossing`).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 23, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOR_STACK_SEEDING_
#define IOR_STACK_SEEDING_

#include "IORStack.h"
#include "GradedIndexMedium.h"
#include "../Animation/TimeIndexedView.h"
#include <algorithm>
#include <limits>
#include <cstring>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <cmath>
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/SpecularInfo.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	namespace IORStackSeeding
	{
		// Per-object containment is determined by per-object PARITY of
		// exits-vs-entries along a probe ray.  An object contains the
		// seed point iff the probe sees one more exit than entry (i.e.
		// it must cross the boundary one more time outward than inward
		// to leave the object).
		//
		// Counting exits only (or deduping per object) is INCORRECT
		// because a probe that simply *passes through* an object —
		// camera in front of a glass sphere with probe direction
		// going through it — sees one entry and one exit.  Without
		// parity tracking, that probe would record the sphere as
		// "containing" the camera and pre-seed the stack with it,
		// causing the first eye-ray hit on the sphere to be treated
		// as an exit (refraction direction computed glass→air
		// instead of air→glass).
		//
		// The `parity` field is +1 per exit, -1 per entry.  A net
		// positive value means the seed is inside that object along
		// this probe.
		struct ProbeEntry {
			const IObject* pObj;
			Scalar ior;
			int parity;
			int firstExitStep;  // probe step of FIRST exit, for stack-order sort
			int firstEntryStep; // inward-wound closed solids reverse these faces
			// DL-46: true for a `hasInterior`-only (non-refracting,
			// stateful) material -- push re-pushes the stack's CURRENT
			// top instead of this entry's captured `ior`, matching
			// TranslucentSPF::Scatter's own `push(ior_stack.top())`.
			// False for an ordinary `canRefract` medium, which pushes
			// its own distinct `ior` as before.
			bool repushParentIor;
		};

		// Safety cap on the surface hits one probe walks: guards against
		// pathological geometry (coincident faces, self-intersections)
		// that could loop indefinitely.
		static const int kMaxProbeSteps = 32;

		// Per-probe entry table, stack-allocated.  Sized to the STEP cap,
		// not to a nesting depth (DL-09 slice, 2026-09-28): the table holds
		// one entry per DISTINCT trackable object the probe MEETS, and a
		// probe meets every refractor between the seed and the scene edge
		// -- including the ones in front of it that do not contain it at
		// all.  An 8-entry table (the old `kMaxNestingDepth`) filled up with
		// those and silently dropped the containing objects the probe only
		// reaches afterwards: a camera inside 5 of 17 nested boxes, 12 of
		// them above it along +z, seeded as bare AIR, and the render read
		// 0.39x its closed form (tests/GradedIndexInteriorFactorTest.cpp row
		// E).  One step adds at most one entry, so a table with as many
		// entries as the probe has steps can never overflow.  The probe's
		// step budget is `MaxSceneWalkSteps` (DL-622: kMaxProbeSteps plus
		// two per open sheet in the scene, since every sheet a probe passes
		// costs a step); `SeedFromPointImpl` uses stack tables of this size
		// while that budget fits (every scene without open sheets: the
		// pre-DL-622 cost) and heap tables sized to the budget otherwise,
		// and `TallyProbe` never walks more steps than its table holds.
		static const std::size_t kMaxProbeEntries = static_cast<std::size_t>( kMaxProbeSteps );

		//! Probe-table entries `SeedFromPointImpl` keeps on the stack: the
		//! base budget plus 16 open sheets (6 tables x 64 x 32 bytes), so a
		//! scene with a handful of sheets seeds without heap tables.
		static const std::size_t kStackProbeEntries = 2 * kMaxProbeEntries;

		// Probe-trace counter (2026-09-14, DL-76 perf follow-up).  This
		// compiles and increments UNCONDITIONALLY in every build -- it
		// is not behind a test-only `#ifdef`, so production code takes
		// the exact same path the regression test measures.  Cost: one
		// thread_local `std::size_t` increment per scene ray walk,
		// sitting next to the ray-vs-scene intersection query that walk
		// performs -- negligible by construction relative to that query,
		// which walks the object manager's acceleration structure doing
		// real ray-primitive tests.  Thread-local: no atomics or
		// locking, correct under the render thread pool since each
		// thread seeds its own subpaths independently.  Counts EVERY
		// scene ray walk `SeedFromPoint` makes -- `TallyProbe` calls
		// and the DL-618 sheet-normal confirmations (`OpenSheetNormalRayReaches`)
		// -- so regression tests can assert its total probe-trace budget
		// stays bounded.  Its consumers are
		// `TranslucentInitialContainmentTest` and
		// `OpenSheetHalfSpaceSeedingTest`.
		namespace Diagnostics
		{
			inline std::size_t& ProbeTraceCounter()
			{
				static thread_local std::size_t counter = 0;
				return counter;
			}

			//! DL-622 review: every scene IntersectRay this header makes
			//! (probe steps, normal-ray and detour walks, exit resolution),
			//! for cost regression tests.  Same thread-local, always-on
			//! convention as ProbeTraceCounter.
			inline std::size_t& SceneRayCounter()
			{
				static thread_local std::size_t counter = 0;
				return counter;
			}
		}

		inline void DebugResetProbeTraceCount() {
			Diagnostics::ProbeTraceCounter() = 0;
		}

		inline std::size_t DebugGetProbeTraceCount() {
			return Diagnostics::ProbeTraceCounter();
		}

		//! DL-622 review: the free-space detour certificate
		//! (`HasFreeSpaceDetourToSheet`) is a PURE function of (seed point,
		//! sheet): no shared ray budget decides it, so `SeedFromPoint` and an
		//! exit resolution (`PointInOpenSheetMedium`) asking about the same
		//! sheet at the same point always agree, and a sheet confirmed by a
		//! clear normal ray never depends on another sheet's detour.  Its cost
		//! is bounded by construction -- one column walk plus at most eight
		//! waypoints, each a lateral and a normal walk of at most
		//! `MaxSceneWalkSteps` steps -- and a co-normal group whose shared
		//! normal ray is blocked runs ONE such certificate for all of its
		//! sheets (`HasFreeSpaceDetourToSheets`), which answers each sheet
		//! exactly as its own certificate would.

		//////////////////////////////////////////////////////////////////
		// DL-618: medium membership of a certified OPEN refractive sheet.
		//
		// An open sheet (`RayIntersectionGeometric::bProvablyNoInterior`
		// on a refracting material) is an interface whose BACK side is
		// the sheet's medium (DL-345's face rule).  It encloses no
		// volume, so "is `pos` inside it" has no parity answer; the
		// walks decide it by history -- a walk that crossed the sheet
		// front -> back carries its index until it crosses back.  A seed
		// point has no history, so it gets the following CONVENTION,
		// which is the one `SeedFromPoint` and `OpenSheetEnclosesCrossing`
		// share (so the eye walk, the light walk and an exit resolution
		// agree on every point):
		//
		//   `pos` is in sheet S's medium iff the ray from `pos` along S's
		//   OWN true geometric normal N (back -> front: "the sheet's
		//   normal ray") reaches S on its BACK face, with every surface
		//   it meets first being MEMBERSHIP-TRANSPARENT.
		//
		// Membership-transparent: a hit with no true side (hair, skipped
		// outright), a trackable closed medium (`canRefract` with ior > 0,
		// or `hasInterior`, without `bProvablyNoInterior` -- a glass
		// sphere or a translucent card under the sheet sits INSIDE its
		// medium, it does not bound it), and another open sheet met on
		// its BACK face (leaving that sheet's medium, which may itself be
		// inside S's: nested lone sheets).  Everything else BLOCKS: an
		// opaque / non-trackable surface (a ceiling between `pos` and the
		// sheet -- the room below it is not the sea above it), and any
		// open sheet met on its FRONT face (a two-sheet slab seen from
		// outside).
		//
		// Consequences, stated so they are not rediscovered as bugs:
		//  - A point that cannot see S along S's normal is never seeded
		//    into S: neither through an opaque surface, nor from beside
		//    the sheet (outside its normal prism), nor from its front.
		//  - For a planar S the predicate depends on `pos` and S only --
		//    not on any probe axis -- so it is invariant under rotating
		//    the scene, and it changes only where `pos` crosses the
		//    boundary of S's normal prism (or an occluder's shadow along
		//    N), which IS the medium boundary of this convention.
		//  - CANDIDATES are the scene's open refractive sheets, listed once
		//    per scene state by `OpenSheetCatalog` (no per-seed discovery,
		//    so every sheet is tested whatever its orientation, and a scene
		//    with none pays nothing).  N is each sheet's true normal where
		//    the catalog's probe met it: exact for a planar sheet, an
		//    approximation of "its normal" for a curved one (displaced,
		//    Bezier, non-planar mesh).
		//  - Two confirmed sheets are NESTED when one's back face is on
		//    the other's normal ray before it (the passed one is inner).
		//    Two confirmed sheets neither of which is on the other's ray
		//    are SIBLINGS bounding one region (a two-sheet slab, or two
		//    sheets meeting at a wedge): a walk entering that region
		//    pushes ONE of them, so siblings of one material seed one
		//    entry; siblings of DIFFERENT materials have a walk-history-
		//    dependent interior index (which sheet was crossed last), so
		//    seeding declines every open sheet at `pos` rather than pick
		//    one arbitrarily (closed media are still seeded).  Mutually
		//    nested sheets (curved pathologies) decline the same way.
		//  - Relative to closed media: if S's normal ray leaves a closed
		//    medium C that contains `pos` before reaching S, C sits inside
		//    S's half-space and S is OUTER to C; otherwise S is inside C.
		//////////////////////////////////////////////////////////////////

		//! DL-618: how one hit affects open-sheet medium membership along a
		//! probe travelling @a dir.  See the convention above.
		enum MembershipHit
		{
			kMembershipSkip,		//!< no true side (hair): ignore
			kMembershipBlock,		//!< opaque / non-trackable surface
			kMembershipTransparent,	//!< trackable closed medium (either face)
			kMembershipSheetBack,	//!< certified open refractive sheet, back face
			kMembershipSheetFront	//!< certified open refractive sheet, front face
		};

		//! DL-618: classifies @a ri for open-sheet membership.  @a info
		//! receives the material's SpecularInfo when the hit has a
		//! material (unchanged otherwise).
		inline MembershipHit ClassifyMembershipHit(
			const RayIntersection& ri,
			const Vector3& dir,
			const Scalar nm,
			SpecularInfo& info
			)
		{
			if( !ri.geometric.HasTrueGeomSide() ) {
				return kMembershipSkip;
			}
			if( !ri.pObject || !ri.pMaterial ) {
				return kMembershipBlock;
			}
			IORStack queryStack( Scalar( 1.0 ) );
			info = nm > 0 ? ri.pMaterial->GetSpecularInfoNM( ri.geometric, queryStack, nm )
				: ri.pMaterial->GetSpecularInfo( ri.geometric, queryStack );
			const bool bTrackable = info.valid &&
				( info.canRefract ? ( info.ior > 0 ) : info.hasInterior );
			if( !bTrackable ) {
				return kMembershipBlock;
			}
			if( ri.geometric.bProvablyNoInterior && info.canRefract ) {
				return ri.geometric.TrueGeomFacing( dir ) > 0 ? kMembershipSheetBack : kMembershipSheetFront;
			}
			return kMembershipTransparent;
		}

		//! DL-618 / DL-622: one certified open refractive sheet of the scene,
		//! with the geometry the sheet-normal convention, its 2D normal-group
		//! culling, and its multi-normal fold handling use.
		struct OpenSheetCatalogEntry
		{
			const IObject* pObj;
			Vector3 normal;					//!< the sheet's representative true normal (see DescribeOpenSheet)
			Vector3 planeU;					//!< orthonormal tangent U orthogonal to `normal`
			Vector3 planeV;					//!< orthonormal tangent V = normal x planeU
			Point3 p0;						//!< a representative point on the sheet
			Scalar heightAbove;				//!< max over the sheet's bounds of dot(x - p0, normal)
			Scalar uMin, uMax;				//!< 2D bounds along planeU relative to p0
			Scalar vMin, vMax;				//!< 2D bounds along planeV relative to p0
			BoundingBox box;				//!< the sheet's world bounds
			Matrix4 xform;					//!< the object's world transform when described (its pose)
			std::vector<Vector3> auxNormals;//!< distinct facet normals when !isPlanar (up to 8)
			bool isPlanar;					//!< true when sampled hits lie in one plane and share `normal`
			bool valid;						//!< false: a cached FAILED re-description (negative cache)

			OpenSheetCatalogEntry() :
			  pObj( 0 ),
			  planeU( 1, 0, 0 ),
			  planeV( 0, 1, 0 ),
			  heightAbove( 0 ),
			  uMin( -RISE_INFINITY ),
			  uMax( RISE_INFINITY ),
			  vMin( -RISE_INFINITY ),
			  vMax( RISE_INFINITY ),
			  isPlanar( false ),
			  valid( true )
			{}
		};

		//! Value comparisons (+0 == -0) for the pose key.
		inline bool SamePose( const Matrix4& a, const Matrix4& b )
		{
			return a._00 == b._00 && a._01 == b._01 && a._02 == b._02 && a._03 == b._03 &&
				a._10 == b._10 && a._11 == b._11 && a._12 == b._12 && a._13 == b._13 &&
				a._20 == b._20 && a._21 == b._21 && a._22 == b._22 && a._23 == b._23 &&
				a._30 == b._30 && a._31 == b._31 && a._32 == b._32 && a._33 == b._33;
		}

		inline bool SameBox( const BoundingBox& a, const BoundingBox& b )
		{
			return a.ll.x == b.ll.x && a.ll.y == b.ll.y && a.ll.z == b.ll.z &&
				a.ur.x == b.ur.x && a.ur.y == b.ur.y && a.ur.z == b.ur.z;
		}

		//! Deterministic radical inverse in @a base for Halton surface probes.
		inline Scalar HaltonRadicalInverse( unsigned int index, const unsigned int base )
		{
			Scalar result = Scalar( 0 );
			Scalar f = Scalar( 1 ) / Scalar( base );
			unsigned int i = index;
			while( i > 0 ) {
				result += f * Scalar( i % base );
				i /= base;
				f /= Scalar( base );
			}
			return result;
		}

		//! Orthonormal basis (outU, outV) orthogonal to unit vector @a n.
		inline void MakeOrthonormalTangents( const Vector3& n, Vector3& outU, Vector3& outV )
		{
			const Vector3 ref = std::fabs( n.x ) < Scalar( 0.9 ) ? Vector3( 1, 0, 0 ) : Vector3( 0, 1, 0 );
			outU = Vector3Ops::Normalize( Vector3Ops::Cross( n, ref ) );
			outV = Vector3Ops::Cross( n, outU );
		}

		//! DL-622: probes @a object directly to obtain a representative
		//! RayIntersection on its surface (from @a anchor along the 26 cube
		//! directions -- 6 principal axes, 12 face-diagonals, 8 body-diagonals --
		//! toward its bounds centre, or via UniformRandomPoint / grid
		//! sampling).  Used by RefreshLegacySeedFallbackNM, SMSCaptureUncached
		//! and ReindexStackForWavelength when no EntryContext was recorded on
		//! the stack.  Object-only rays (no scene walk), at most
		//! kMaxSurfaceProbeRays of them: 26 + 1 + 16 Halton samples + a
		//! 3 x 3 grid per axis (27).  The first stage stops at the first
		//! principal-axis hit, which is the common case (an anchor inside or
		//! beside the object).
		static const int kMaxSurfaceProbeRays = 26 + 1 + 16 + 27;

		inline bool ProbeObjectSurfaceHit(
			const IObject& object,
			const Point3& anchor,
			RayIntersection& outRi
			)
		{
			static const Scalar s2 = Scalar( 0.70710678118654752440 );
			static const Scalar s3 = Scalar( 0.57735026918962576451 );
			static const Vector3 kDirs26[26] = {
				Vector3(  1,  0,  0 ), Vector3( -1,  0,  0 ),
				Vector3(  0,  1,  0 ), Vector3(  0, -1,  0 ),
				Vector3(  0,  0,  1 ), Vector3(  0,  0, -1 ),
				Vector3(  s2,  s2,   0 ), Vector3(  s2, -s2,   0 ),
				Vector3( -s2,  s2,   0 ), Vector3( -s2, -s2,   0 ),
				Vector3(  s2,   0,  s2 ), Vector3(  s2,   0, -s2 ),
				Vector3( -s2,   0,  s2 ), Vector3( -s2,   0, -s2 ),
				Vector3(   0,  s2,  s2 ), Vector3(   0,  s2, -s2 ),
				Vector3(   0, -s2,  s2 ), Vector3(   0, -s2, -s2 ),
				Vector3(  s3,  s3,  s3 ), Vector3(  s3,  s3, -s3 ),
				Vector3(  s3, -s3,  s3 ), Vector3(  s3, -s3, -s3 ),
				Vector3( -s3,  s3,  s3 ), Vector3( -s3,  s3, -s3 ),
				Vector3( -s3, -s3,  s3 ), Vector3( -s3, -s3, -s3 )
			};
			Scalar bestDist = RISE_INFINITY;
			bool found = false;
			for( int a = 0; a < 26; ++a ) {
				RayIntersection ri( Ray( anchor, kDirs26[a] ), nullRasterizerState );
				object.IntersectRay( ri, RISE_INFINITY, true, true, false );
				if( ri.geometric.bHit && ri.geometric.range < bestDist ) {
					bestDist = ri.geometric.range;
					outRi = ri;
					found = true;
					if( a < 6 ) {
						break;
					}
				}
			}
			if( found ) {
				return true;
			}
			const BoundingBox box = object.getBoundingBox();
			const Point3 c = box.GetCenter();
			const Vector3 e = box.GetExtents();
			if( std::isfinite( c.x ) && std::isfinite( c.y ) && std::isfinite( c.z ) ) {
				const Vector3 toC = Vector3Ops::mkVector3( c, anchor );
				if( Vector3Ops::SquaredModulus( toC ) > NEARZERO ) {
					RayIntersection ri( Ray( anchor, Vector3Ops::Normalize( toC ) ), nullRasterizerState );
					object.IntersectRay( ri, RISE_INFINITY, true, true, false );
					if( ri.geometric.bHit ) {
						outRi = ri;
						return true;
					}
				}
			}
			if( !std::isfinite( e.x ) || !std::isfinite( e.y ) || !std::isfinite( e.z ) ) {
				return false;
			}
			const Scalar reach = Vector3Ops::Magnitude( e ) + Scalar( 1 );
			if( object.GetGeometry() != 0 ) {
				for( unsigned int s = 1; s <= 16; ++s ) {
					const Point3 prand(
						HaltonRadicalInverse( s, 2 ),
						HaltonRadicalInverse( s, 3 ),
						HaltonRadicalInverse( s, 5 ) );
					Point3 ptSample( 0, 0, 0 );
					Vector3 nrmSample( 0, 1, 0 );
					object.UniformRandomPoint( &ptSample, &nrmSample, 0, prand );
					if( !std::isfinite( ptSample.x ) || !std::isfinite( ptSample.y ) || !std::isfinite( ptSample.z ) ) {
						continue;
					}
					const Vector3 d = Vector3Ops::Magnitude( nrmSample ) > NEARZERO
						? Vector3Ops::Normalize( -nrmSample ) : Vector3( 0, 1, 0 );
					RayIntersection ri( Ray( Point3Ops::mkPoint3( ptSample, d * ( -reach ) ), d ), nullRasterizerState );
					object.IntersectRay( ri, RISE_INFINITY, true, true, false );
					if( ri.geometric.bHit ) {
						outRi = ri;
						return true;
					}
				}
			}
			const Scalar ctr[3] = { c.x, c.y, c.z };
			const Scalar ext[3] = { e.x, e.y, e.z };
			for( int axis = 0; axis < 3; ++axis ) {
				const int ua = ( axis + 1 ) % 3, va = ( axis + 2 ) % 3;
				Scalar dir[3] = { 0, 0, 0 };
				dir[axis] = 1;
				const Vector3 d( dir[0], dir[1], dir[2] );
				for( int gi = 0; gi <= 2; ++gi ) {
					for( int gj = 0; gj <= 2; ++gj ) {
						Scalar aim[3] = { ctr[0], ctr[1], ctr[2] };
						aim[ua] += ( Scalar( gi ) / Scalar( 2 ) - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[ua];
						aim[va] += ( Scalar( gj ) / Scalar( 2 ) - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[va];
						const Point3 target( aim[0], aim[1], aim[2] );
						RayIntersection ri( Ray( Point3Ops::mkPoint3( target, d * ( -reach ) ), d ), nullRasterizerState );
						object.IntersectRay( ri, RISE_INFINITY, true, true, false );
						if( ri.geometric.bHit ) {
							outRi = ri;
							return true;
						}
					}
				}
			}
			return false;
		}

		inline bool ProbeObjectSurfaceHit(
			const IObject* pObj,
			const Point3& anchor,
			RayIntersection& outRi
			)
		{
			return pObj && ProbeObjectSurfaceHit( *pObj, anchor, outRi );
		}

		//! DL-618 / DL-622: describes @a object if it is a certified open
		//! refractive sheet (returns false otherwise).
		//!
		//! Stage 1 probes @a object ALONE from outside its bounds along each
		//! principal axis, toward its centre and four quarter points (15
		//! rays).  Stage 2 (DL-622 Gap 6) runs only when all 15 Stage-1 rays
		//! miss @a object (e.g. a hollow window frame, off-centre corner
		//! shard, or thin diagonal strip), drawing 32 deterministic Halton
		//! surface samples via `UniformRandomPoint` (or an 11 x 11 perimeter-
		//! inclusive grid across the 3 axes for CSG composites where
		//! `GetGeometry() == 0`).
		//!
		//! The representative NORMAL is that of the least-squares plane through
		//! a 9 x 9 grid of hits along the axis whose hits meet the sheet most
		//! squarely (largest mean |N . axis|), and `p0` their centroid (DL-618
		//! review round 2, P2-1), supplemented by the Stage 1/2 hits when the
		//! 9 x 9 grid meets fewer than 3 points.  Also records whether the
		//! sheet is planar (`isPlanar`) and, for non-planar/folded sheets, up
		//! to 8 clustered auxiliary facet normals (`auxNormals`).
		inline bool DescribeOpenSheet( const IObject& object, OpenSheetCatalogEntry& out )
		{
			const BoundingBox box = object.getBoundingBox();
			const Point3 c = box.GetCenter();
			const Vector3 e = box.GetExtents();
			if( !std::isfinite( c.x ) || !std::isfinite( c.y ) || !std::isfinite( c.z ) ||
				!std::isfinite( e.x ) || !std::isfinite( e.y ) || !std::isfinite( e.z ) ) {
				return false;
			}
			const Scalar diag = Vector3Ops::Magnitude( e );
			const Scalar reach = diag + Scalar( 1 );
			static const Scalar kOff[5][2] = { { 0, 0 }, { 0.25, 0.25 }, { -0.25, 0.25 }, { 0.25, -0.25 }, { -0.25, -0.25 } };
			bool anyHit = false, sheet = false;
			Scalar bestScore = -1;
			int bestAxis = -1;
			Vector3 bestNormal( 0, 0, 0 );
			Point3 bestPoint = c;
			std::vector<Point3> sampledPts;
			std::vector<Vector3> sampledNrms;
			sampledPts.reserve( 112 );
			sampledNrms.reserve( 112 );

			for( int axis = 0; axis < 3; axis++ ) {
				Vector3 sumN( 0, 0, 0 );
				Scalar sumX = 0, sumY = 0, sumZ = 0, sumFacing = 0;
				int hits = 0;
				for( int k = 0; k < 5; k++ ) {
					Scalar aim[3] = { c.x, c.y, c.z };
					const Scalar ext[3] = { e.x, e.y, e.z };
					aim[( axis + 1 ) % 3] += kOff[k][0] * ext[( axis + 1 ) % 3];
					aim[( axis + 2 ) % 3] += kOff[k][1] * ext[( axis + 2 ) % 3];
					Scalar dir[3] = { 0, 0, 0 };
					dir[axis] = 1;
					const Point3 target( aim[0], aim[1], aim[2] );
					const Vector3 d( dir[0], dir[1], dir[2] );
					RayIntersection ri( Ray( Point3Ops::mkPoint3( target, d * ( -reach ) ), d ), nullRasterizerState );
					object.IntersectRay( ri, RISE_INFINITY, true, true, false );
					if( !ri.geometric.bHit || !ri.geometric.HasTrueGeomSide() ) {
						continue;
					}
					SpecularInfo info;
					const MembershipHit membership = ClassifyMembershipHit( ri, d, 0, info );
					if( !anyHit ) {
						anyHit = true;
						sheet = ( membership == kMembershipSheetBack || membership == kMembershipSheetFront );
						if( !sheet ) {
							return false;
						}
					}
					const Vector3 n = ri.geometric.UnflippedGeomNormal();
					sumN = sumN + n;
					sumFacing += std::fabs( Vector3Ops::Dot( n, d ) );
					sumX += ri.geometric.ptIntersection.x;
					sumY += ri.geometric.ptIntersection.y;
					sumZ += ri.geometric.ptIntersection.z;
					sampledPts.push_back( ri.geometric.ptIntersection );
					sampledNrms.push_back( n );
					hits++;
				}
				if( hits > 0 && sumFacing / hits > bestScore && Vector3Ops::Magnitude( sumN ) > NEARZERO ) {
					bestScore = sumFacing / hits;
					bestAxis = axis;
					bestNormal = Vector3Ops::Normalize( sumN );
					bestPoint = Point3( sumX / hits, sumY / hits, sumZ / hits );
				}
			}

			// DL-622 Gap 6: Stage 2 fallback when all 15 Stage-1 axis probes
			// missed the object (e.g. a hollow frame, corner piece, or thin
			// diagonal ribbon).
			if( !anyHit ) {
				Vector3 sumN( 0, 0, 0 );
				Scalar sumX = 0, sumY = 0, sumZ = 0;
				int hits = 0;
				auto recordFallbackHit = [&]( const RayIntersection& ri, const Vector3& d ) -> bool {
					if( !ri.geometric.bHit || !ri.geometric.HasTrueGeomSide() ) {
						return true;
					}
					SpecularInfo info;
					const MembershipHit membership = ClassifyMembershipHit( ri, d, 0, info );
					if( !anyHit ) {
						anyHit = true;
						sheet = ( membership == kMembershipSheetBack || membership == kMembershipSheetFront );
						if( !sheet ) {
							return false;
						}
					}
					const Vector3 n = ri.geometric.UnflippedGeomNormal();
					if( hits > 0 && Vector3Ops::Dot( n, sumN ) < 0 ) {
						sumN = sumN - n;
					} else {
						sumN = sumN + n;
					}
					sumX += ri.geometric.ptIntersection.x;
					sumY += ri.geometric.ptIntersection.y;
					sumZ += ri.geometric.ptIntersection.z;
					sampledPts.push_back( ri.geometric.ptIntersection );
					sampledNrms.push_back( n );
					hits++;
					return true;
				};

				if( object.GetGeometry() != 0 ) {
					for( unsigned int s = 1; s <= 32; ++s ) {
						const Point3 prand(
							HaltonRadicalInverse( s, 2 ),
							HaltonRadicalInverse( s, 3 ),
							HaltonRadicalInverse( s, 5 ) );
						Point3 ptSample( 0, 0, 0 );
						Vector3 nrmSample( 0, 1, 0 );
						object.UniformRandomPoint( &ptSample, &nrmSample, 0, prand );
						if( !std::isfinite( ptSample.x ) || !std::isfinite( ptSample.y ) || !std::isfinite( ptSample.z ) ) {
							continue;
						}
						const Vector3 d = Vector3Ops::Magnitude( nrmSample ) > NEARZERO
							? Vector3Ops::Normalize( -nrmSample ) : Vector3( 0, 1, 0 );
						RayIntersection ri( Ray( Point3Ops::mkPoint3( ptSample, d * ( -reach ) ), d ), nullRasterizerState );
						object.IntersectRay( ri, RISE_INFINITY, true, true, false );
						if( ri.geometric.bHit && ri.geometric.HasTrueGeomSide() ) {
							if( !recordFallbackHit( ri, d ) ) {
								return false;
							}
						} else {
							for( int axis = 0; axis < 3; ++axis ) {
								Scalar dir[3] = { 0, 0, 0 };
								dir[axis] = 1;
								const Vector3 da( dir[0], dir[1], dir[2] );
								RayIntersection ria( Ray( Point3Ops::mkPoint3( ptSample, da * ( -reach ) ), da ), nullRasterizerState );
								object.IntersectRay( ria, RISE_INFINITY, true, true, false );
								if( ria.geometric.bHit && ria.geometric.HasTrueGeomSide() ) {
									if( !recordFallbackHit( ria, da ) ) {
										return false;
									}
									break;
								}
							}
						}
					}
				}
				if( !anyHit ) {
					const Scalar ctr[3] = { c.x, c.y, c.z };
					const Scalar ext[3] = { e.x, e.y, e.z };
					for( int axis = 0; axis < 3; ++axis ) {
						const int ua = ( axis + 1 ) % 3, va = ( axis + 2 ) % 3;
						Scalar dir[3] = { 0, 0, 0 };
						dir[axis] = 1;
						const Vector3 d( dir[0], dir[1], dir[2] );
						for( int gi = 0; gi <= 10; ++gi ) {
							for( int gj = 0; gj <= 10; ++gj ) {
								Scalar aim[3] = { ctr[0], ctr[1], ctr[2] };
								aim[ua] += ( Scalar( gi ) / Scalar( 10 ) - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[ua];
								aim[va] += ( Scalar( gj ) / Scalar( 10 ) - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[va];
								const Point3 target( aim[0], aim[1], aim[2] );
								RayIntersection ri( Ray( Point3Ops::mkPoint3( target, d * ( -reach ) ), d ), nullRasterizerState );
								object.IntersectRay( ri, RISE_INFINITY, true, true, false );
								if( ri.geometric.bHit && ri.geometric.HasTrueGeomSide() ) {
									if( !recordFallbackHit( ri, d ) ) {
										return false;
									}
								}
							}
						}
					}
				}
				if( hits > 0 && Vector3Ops::Magnitude( sumN ) > NEARZERO ) {
					bestNormal = Vector3Ops::Normalize( sumN );
					bestPoint = Point3( sumX / hits, sumY / hits, sumZ / hits );
					const Scalar nx = std::fabs( bestNormal.x );
					const Scalar ny = std::fabs( bestNormal.y );
					const Scalar nz = std::fabs( bestNormal.z );
					bestAxis = ( nx >= ny && nx >= nz ) ? 0 : ( ( ny >= nz ) ? 1 : 2 );
					bestScore = std::max( nx, std::max( ny, nz ) );
				}
			}

			if( !sheet || bestScore < 0 || bestAxis < 0 ) {
				return false;
			}
			// Least-squares PLANE through a 9 x 9 grid of hits along the
			// face-on axis: a = c0 + c1 u + c2 v in that axis's frame.  For a
			// planar sheet this is its plane; for a height field (a displaced
			// sea) the fitted slope is the sheet's large-scale tilt, which
			// wave slopes perturb only through their covariance with
			// position across the whole extent, not pointwise as a sampled
			// normal does.  Falls back to the mean normal above when the fit
			// is degenerate.
			{
				const int a = bestAxis, ua = ( bestAxis + 1 ) % 3, va = ( bestAxis + 2 ) % 3;
				const Scalar ctr[3] = { c.x, c.y, c.z };
				const Scalar ext[3] = { e.x, e.y, e.z };
				Scalar n = 0, su = 0, sv = 0, sa = 0, suu = 0, svv = 0, suv = 0, sua = 0, sva = 0;
				for( int gi = 0; gi < 9; gi++ ) {
					for( int gj = 0; gj < 9; gj++ ) {
						Scalar aim[3] = { ctr[0], ctr[1], ctr[2] };
						aim[ua] += ( Scalar( gi ) / 8 - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[ua];
						aim[va] += ( Scalar( gj ) / 8 - Scalar( 0.5 ) ) * Scalar( 0.9 ) * ext[va];
						Scalar dir[3] = { 0, 0, 0 };
						dir[a] = 1;
						const Vector3 d( dir[0], dir[1], dir[2] );
						RayIntersection ri( Ray( Point3Ops::mkPoint3( Point3( aim[0], aim[1], aim[2] ), d * ( -reach ) ), d ), nullRasterizerState );
						object.IntersectRay( ri, RISE_INFINITY, true, true, false );
						if( !ri.geometric.bHit ) {
							continue;
						}
						if( ri.geometric.HasTrueGeomSide() ) {
							sampledPts.push_back( ri.geometric.ptIntersection );
							sampledNrms.push_back( ri.geometric.UnflippedGeomNormal() );
						}
						const Scalar q[3] = { ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z };
						const Scalar u = q[ua] - ctr[ua], v = q[va] - ctr[va], h = q[a] - ctr[a];
						n += 1; su += u; sv += v; sa += h; suu += u * u; svv += v * v; suv += u * v; sua += u * h; sva += v * h;
					}
				}
				if( n < 3 && sampledPts.size() >= 3 ) {
					n = 0; su = 0; sv = 0; sa = 0; suu = 0; svv = 0; suv = 0; sua = 0; sva = 0;
					for( std::size_t idx = 0; idx < sampledPts.size(); ++idx ) {
						const Scalar q[3] = { sampledPts[idx].x, sampledPts[idx].y, sampledPts[idx].z };
						const Scalar u = q[ua] - ctr[ua], v = q[va] - ctr[va], h = q[a] - ctr[a];
						n += 1; su += u; sv += v; sa += h; suu += u * u; svv += v * v; suv += u * v; sua += u * h; sva += v * h;
					}
				}
				if( n >= 3 ) {
					const Scalar mu = su / n, mv = sv / n, ma = sa / n;
					const Scalar cuu = suu / n - mu * mu, cvv = svv / n - mv * mv, cuv = suv / n - mu * mv;
					const Scalar cua = sua / n - mu * ma, cva = sva / n - mv * ma;
					const Scalar det = cuu * cvv - cuv * cuv;
					// Conditioning (round 3, P3-4): the fit's slope across the
					// sample spread's NARROW direction is unreliable when the
					// hits span a thin band (a narrow strip, or a sheet the grid
					// mostly misses); below a 1:100 eigenvalue ratio keep the
					// mean hit normal.
					const Scalar tr = cuu + cvv;
					const Scalar disc = std::sqrt( std::max( Scalar( 0 ), tr * tr - 4 * det ) );
					const Scalar lmax = ( tr + disc ) / 2, lmin = ( tr - disc ) / 2;
					if( det > NEARZERO * ( Scalar( 1 ) + cuu * cvv ) && lmin >= Scalar( 0.01 ) * lmax ) {
						const Scalar c1 = ( cua * cvv - cva * cuv ) / det;
						const Scalar c2 = ( cva * cuu - cua * cuv ) / det;
						Scalar nn[3];
						nn[a] = 1; nn[ua] = -c1; nn[va] = -c2;
						Vector3 fit = Vector3Ops::Normalize( Vector3( nn[0], nn[1], nn[2] ) );
						if( Vector3Ops::Dot( fit, bestNormal ) < 0 ) {
							fit = -fit;
						}
						bestNormal = fit;
						Scalar pc[3];
						pc[a] = ctr[a] + ma; pc[ua] = ctr[ua] + mu; pc[va] = ctr[va] + mv;
						bestPoint = Point3( pc[0], pc[1], pc[2] );
					}
				}
			}

			// DL-622 Gap 2: sample surface facets directly via UniformRandomPoint
			// so folded / overhanging facets hidden from the single-axis grid
			// also contribute their points and normals to the planarity check
			// and auxiliary normal clusters.
			if( object.GetGeometry() != 0 ) {
				const Scalar probeBack = std::max( Scalar( 1e-2 ), Scalar( 0.05 ) * diag );
				for( unsigned int s = 1; s <= 16; ++s ) {
					const Point3 prand(
						HaltonRadicalInverse( s, 2 ),
						HaltonRadicalInverse( s, 3 ),
						HaltonRadicalInverse( s, 5 ) );
					Point3 ptS( 0, 0, 0 );
					Vector3 nrmS( 0, 1, 0 );
					object.UniformRandomPoint( &ptS, &nrmS, 0, prand );
					if( !std::isfinite( ptS.x ) || !std::isfinite( ptS.y ) || !std::isfinite( ptS.z ) ||
						Vector3Ops::Magnitude( nrmS ) <= NEARZERO ) {
						continue;
					}
					const Vector3 d = Vector3Ops::Normalize( -nrmS );
					RayIntersection ri( Ray( Point3Ops::mkPoint3( ptS, d * ( -probeBack ) ), d ), nullRasterizerState );
					object.IntersectRay( ri, Scalar( 2.0 ) * probeBack, true, true, false );
					if( ri.geometric.bHit && ri.geometric.HasTrueGeomSide() ) {
						sampledPts.push_back( ri.geometric.ptIntersection );
						sampledNrms.push_back( ri.geometric.UnflippedGeomNormal() );
					}
				}
			}

			out.pObj = &object;
			out.normal = bestNormal;
			MakeOrthonormalTangents( bestNormal, out.planeU, out.planeV );
			out.p0 = bestPoint;
			out.box = box;
			out.xform = object.GetFinalTransformMatrix();
			out.heightAbove = -RISE_INFINITY;
			out.uMin = RISE_INFINITY;
			out.uMax = -RISE_INFINITY;
			out.vMin = RISE_INFINITY;
			out.vMax = -RISE_INFINITY;
			for( int corner = 0; corner < 8; corner++ ) {
				const Point3 q( ( corner & 1 ) ? box.ur.x : box.ll.x, ( corner & 2 ) ? box.ur.y : box.ll.y, ( corner & 4 ) ? box.ur.z : box.ll.z );
				const Vector3 rel = Vector3Ops::mkVector3( q, bestPoint );
				out.heightAbove = std::max( out.heightAbove, Vector3Ops::Dot( rel, bestNormal ) );
				const Scalar u = Vector3Ops::Dot( rel, out.planeU );
				const Scalar v = Vector3Ops::Dot( rel, out.planeV );
				out.uMin = std::min( out.uMin, u );
				out.uMax = std::max( out.uMax, u );
				out.vMin = std::min( out.vMin, v );
				out.vMax = std::max( out.vMax, v );
			}
			const Scalar uPad = Scalar( 1e-6 ) * ( Scalar( 1 ) + std::fabs( out.uMin ) + std::fabs( out.uMax ) );
			const Scalar vPad = Scalar( 1e-6 ) * ( Scalar( 1 ) + std::fabs( out.vMin ) + std::fabs( out.vMax ) );
			if( std::isfinite( uPad ) ) { out.uMin -= uPad; out.uMax += uPad; }
			if( std::isfinite( vPad ) ) { out.vMin -= vPad; out.vMax += vPad; }

			// Planarity and auxiliary facet normal clustering (DL-622 Gap 2).
			const Scalar planeTol = Scalar( 1e-5 ) * std::max( Scalar( 1 ), diag );
			out.isPlanar = !sampledPts.empty();
			out.auxNormals.clear();
			for( std::size_t idx = 0; idx < sampledPts.size(); ++idx ) {
				const Scalar off = std::fabs( Vector3Ops::Dot( Vector3Ops::mkVector3( sampledPts[idx], bestPoint ), bestNormal ) );
				const Scalar cosN = Vector3Ops::Dot( sampledNrms[idx], bestNormal );
				if( off > planeTol || cosN < Scalar( 0.9999 ) ) {
					out.isPlanar = false;
				}
			}
			if( !out.isPlanar ) {
				static const std::size_t kMaxAuxNormals = 8;
				for( std::size_t idx = 0; idx < sampledNrms.size() && out.auxNormals.size() < kMaxAuxNormals; ++idx ) {
					const Vector3& nm = sampledNrms[idx];
					if( Vector3Ops::Dot( nm, bestNormal ) >= Scalar( 0.99 ) ) {
						continue;
					}
					bool duplicate = false;
					for( std::size_t cIdx = 0; cIdx < out.auxNormals.size(); ++cIdx ) {
						if( Vector3Ops::Dot( nm, out.auxNormals[cIdx] ) >= Scalar( 0.98 ) ) {
							duplicate = true;
							break;
						}
					}
					if( !duplicate ) {
						out.auxNormals.push_back( nm );
					}
				}
			}
			return true;
		}

		//! DL-622 Gap 5: a group of planar open sheets sharing a common unit
		//! normal (within cosine 1 - 1e-10), enabling a single shared normal-ray
		//! scene walk (`ConfirmSharedNormalRay`) across all active sheets in the
		//! group instead of N independent BVH ray walks.
		struct OpenSheetNormalGroup
		{
			Vector3 normal;
			Vector3 planeU;
			Vector3 planeV;
			Point3 origin;
			std::vector<std::size_t> sheetIndices;
			std::vector<Scalar> heights;
			std::vector<Scalar> uMin, uMax, vMin, vMax;
			Scalar maxHeight;

			OpenSheetNormalGroup() :
			  normal( 0, 1, 0 ),
			  planeU( 1, 0, 0 ),
			  planeV( 0, 0, 1 ),
			  origin( 0, 0, 0 ),
			  maxHeight( -RISE_INFINITY )
			{}
		};

		//! DL-618 (review P2-1): the scene's certified open refractive sheets,
		//! found ONCE per scene state instead of by per-seed discovery walks
		//! (which made `SeedFromPoint` -- run per camera sample, per HWSS lane
		//! and per subpath -- cost several extra BVH traversals in EVERY scene;
		//! measured +66 % CPU on an open scene with no sheet at all).
		//!
		//! Built by `DescribeOpenSheet` on every world-visible object whose
		//! material can refract (round 2, P2-4 / P3-e: an object whose own
		//! material reports no refraction is skipped without a ray; an object
		//! without a material of its own, e.g. a CSG composite, is probed).
		//!
		//! Thread-local, keyed on the object manager, its spatial-structure
		//! generation (process-unique, advanced by every spatial edit) and its
		//! item count, and on `OpenSheetCatalogEpoch`, which the object
		//! manager's PrepareForRendering advances so a MATERIAL-only edit
		//! (opaque <-> glass, invisible to the spatial generation) rebuilds
		//! the listing at the next render pass.  An edit made between
		//! passes and observed before the next PrepareForRendering keeps
		//! the old listing until then.
		//!
		//! MOTION BLUR (round 2, P3-b): inside a time-indexed pass every
		//! render thread re-poses its own clones per pixel sample while the
		//! generation stays put, so the LIST (which objects are sheets; a pose
		//! does not change it) is kept but each listed sheet's geometry is
		//! re-described at the thread's current pose where it is used
		//! (`PosedOpenSheet`).
		struct OpenSheetCatalog
		{
			const IObjectManager* pObjects;
			unsigned long long generation;
			unsigned long long epoch;
			unsigned int itemCount;
			std::vector<OpenSheetCatalogEntry> sheets;
			//! Planar sheets grouped by shared unit normal (DL-622 Gap 5).
			std::vector<OpenSheetNormalGroup> normalGroups;
			//! Re-posed descriptions of listed sheets (motion blur), one per
			//! sheet object, each valid for the pose it records.
			std::vector<OpenSheetCatalogEntry> posed;
			//! Every enumerated scene object, sorted (identity checks for
			//! stack keys that must not be dereferenced unless they are one).
			std::vector<const IObject*> objects;
			//! Diagonal of the union of the world-visible objects' bounds: the
			//! scene's length scale for self-hit tolerances.
			Scalar sceneDiagonal;

			OpenSheetCatalog() : pObjects( 0 ), generation( 0 ), epoch( 0 ), itemCount( 0 ), sceneDiagonal( 1 ) {}
		};

		inline OpenSheetCatalog& ThreadOpenSheetCatalog()
		{
			static thread_local OpenSheetCatalog catalog;
			return catalog;
		}

		//! DL-618 (round 3): a process-wide epoch every thread's catalog is
		//! keyed on.  `ObjectManager::PrepareForRendering` advances it, so an
		//! edit the spatial generation does not see -- an object's MATERIAL
		//! swapped opaque <-> glass -- is picked up by the next render pass
		//! (every rasterizer prepares the object manager before rendering).
		inline std::atomic<unsigned long long>& OpenSheetCatalogEpoch()
		{
			static std::atomic<unsigned long long> epoch( 1 );
			return epoch;
		}

		inline void InvalidateOpenSheetCatalogs()
		{
			OpenSheetCatalogEpoch().fetch_add( 1, std::memory_order_acq_rel );
		}

		inline bool MaterialMayRefract( const IMaterial* pMaterial )
		{
			if( !pMaterial ) {
				return true;
			}
			RayIntersectionGeometric rig( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
			rig.bHit = true;
			rig.vNormal = Vector3( 0, 0, 1 );
			rig.vGeomNormal = Vector3( 0, 0, 1 );
			IORStack queryStack( Scalar( 1.0 ) );
			const SpecularInfo info = pMaterial->GetSpecularInfo( rig, queryStack );
			return !info.valid || info.canRefract;
		}

		inline const std::vector<OpenSheetCatalogEntry>& OpenSheetsOf( const IObjectManager* pObjects )
		{
			OpenSheetCatalog& catalog = ThreadOpenSheetCatalog();
			const unsigned long long generation = pObjects->GetSpatialStructureGeneration();
			const unsigned int itemCount = pObjects->getItemCount();
			const unsigned long long epoch = OpenSheetCatalogEpoch().load( std::memory_order_acquire );
			if( catalog.pObjects == pObjects && catalog.generation == generation &&
				catalog.epoch == epoch && catalog.itemCount == itemCount ) {
				return catalog.sheets;
			}
			catalog.pObjects = pObjects;
			catalog.generation = generation;
			catalog.epoch = epoch;
			catalog.itemCount = itemCount;
			catalog.sheets.clear();
			catalog.normalGroups.clear();
			catalog.posed.clear();
			catalog.objects.clear();

			struct Collect : IEnumCallback<IObject>
			{
				std::vector<OpenSheetCatalogEntry>* out;
				std::vector<const IObject*>* objects;
				BoundingBox bounds;
				bool any;
				bool operator()( const IObject& object ) override
				{
					objects->push_back( &object );
					if( !object.IsWorldVisible() ) {
						return true;
					}
					const BoundingBox b = object.getBoundingBox();
					// Unbounded geometry (an infinite plane reports
					// +-RISE_INFINITY = +-DBL_MAX, finite but overflowing any
					// extent) does not size the scene.
					const Scalar kHuge = Scalar( 1e300 );
					if( std::fabs( b.ll.x ) < kHuge && std::fabs( b.ll.y ) < kHuge && std::fabs( b.ll.z ) < kHuge &&
						std::fabs( b.ur.x ) < kHuge && std::fabs( b.ur.y ) < kHuge && std::fabs( b.ur.z ) < kHuge ) {
						if( !any ) {
							bounds = b;
							any = true;
						} else {
							bounds.Include( b );
						}
					}
					if( !MaterialMayRefract( object.GetMaterial() ) ) {
						return true;
					}
					OpenSheetCatalogEntry entry;
					if( DescribeOpenSheet( object, entry ) ) {
						out->push_back( entry );
					}
					return true;
				}
			} collect;
			collect.out = &catalog.sheets;
			collect.objects = &catalog.objects;
			collect.any = false;
			pObjects->EnumerateObjects( collect );
			std::sort( catalog.objects.begin(), catalog.objects.end() );
			catalog.sceneDiagonal = collect.any ? std::max( Scalar( 1e-6 ), Vector3Ops::Magnitude( collect.bounds.GetExtents() ) ) : Scalar( 1 );

			// DL-622 Gap 5: group planar sheets with parallel normals so
			// SeedFromPoint can cull and confirm an entire stack of parallel
			// panes with a single shared normal-ray walk.
			for( std::size_t idx = 0; idx < catalog.sheets.size(); ++idx ) {
				const OpenSheetCatalogEntry& s = catalog.sheets[idx];
				if( !s.isPlanar ) {
					continue;
				}
				std::size_t gIdx = catalog.normalGroups.size();
				for( std::size_t g = 0; g < catalog.normalGroups.size(); ++g ) {
					if( Vector3Ops::Dot( catalog.normalGroups[g].normal, s.normal ) >= Scalar( 1.0 - 1e-10 ) ) {
						gIdx = g;
						break;
					}
				}
				if( gIdx == catalog.normalGroups.size() ) {
					OpenSheetNormalGroup grp;
					grp.normal = s.normal;
					grp.planeU = s.planeU;
					grp.planeV = s.planeV;
					grp.origin = s.p0;
					grp.maxHeight = -RISE_INFINITY;
					catalog.normalGroups.push_back( grp );
				}
				OpenSheetNormalGroup& grp = catalog.normalGroups[gIdx];
				Scalar hMaxS = -RISE_INFINITY;
				Scalar uMinS = RISE_INFINITY, uMaxS = -RISE_INFINITY;
				Scalar vMinS = RISE_INFINITY, vMaxS = -RISE_INFINITY;
				for( int corner = 0; corner < 8; ++corner ) {
					const Point3 q(
						( corner & 1 ) ? s.box.ur.x : s.box.ll.x,
						( corner & 2 ) ? s.box.ur.y : s.box.ll.y,
						( corner & 4 ) ? s.box.ur.z : s.box.ll.z );
					const Vector3 rel = Vector3Ops::mkVector3( q, grp.origin );
					hMaxS = std::max( hMaxS, Vector3Ops::Dot( rel, grp.normal ) );
					const Scalar u = Vector3Ops::Dot( rel, grp.planeU );
					const Scalar v = Vector3Ops::Dot( rel, grp.planeV );
					uMinS = std::min( uMinS, u );
					uMaxS = std::max( uMaxS, u );
					vMinS = std::min( vMinS, v );
					vMaxS = std::max( vMaxS, v );
				}
				const Scalar uPad = Scalar( 1e-6 ) * ( Scalar( 1 ) + std::fabs( uMinS ) + std::fabs( uMaxS ) );
				const Scalar vPad = Scalar( 1e-6 ) * ( Scalar( 1 ) + std::fabs( vMinS ) + std::fabs( vMaxS ) );
				if( std::isfinite( uPad ) ) { uMinS -= uPad; uMaxS += uPad; }
				if( std::isfinite( vPad ) ) { vMinS -= vPad; vMaxS += vPad; }
				grp.sheetIndices.push_back( idx );
				grp.heights.push_back( hMaxS );
				grp.uMin.push_back( uMinS );
				grp.uMax.push_back( uMaxS );
				grp.vMin.push_back( vMinS );
				grp.vMax.push_back( vMaxS );
				grp.maxHeight = std::max( grp.maxHeight, hMaxS );
			}
			return catalog.sheets;
		}

		//! DL-618 round 2 (P3-b): @a entry at the calling thread's pose.
		//!
		//! A sheet is re-described only when its pose differs from the one it
		//! was described at; the pose is its world transform AND its world
		//! bounds (round 3 review, P2-b: a keyframed clipped plane's corners
		//! move the sheet without moving its transform).  The compare runs
		//! whether or not the thread is in a time-indexed pass (the
		//! single-threaded motion-blur fallback re-animates the SHARED scene
		//! per sample).  A re-description -- or a failed one (P3-5, negative
		//! cache) -- is kept per thread per sheet for that pose, so a static
		//! sheet costs one transform and bounds read and compare.
		//! Returns false -- the sheet is ABSENT at this pose -- when its
		//! re-description fails (round 3 review, P3-4), never a stale entry.
		inline bool PosedOpenSheet( const OpenSheetCatalogEntry& entry, OpenSheetCatalogEntry& out )
		{
			const Matrix4 now = entry.pObj->GetFinalTransformMatrix();
			const BoundingBox box = entry.pObj->getBoundingBox();
			if( SamePose( now, entry.xform ) && SameBox( box, entry.box ) ) {
				out = entry;
				return true;
			}
			std::vector<OpenSheetCatalogEntry>& posed = ThreadOpenSheetCatalog().posed;
			std::size_t slot = posed.size();
			for( std::size_t i = 0; i < posed.size(); i++ ) {
				if( posed[i].pObj == entry.pObj ) {
					if( SamePose( now, posed[i].xform ) && SameBox( box, posed[i].box ) ) {
						out = posed[i];
						return posed[i].valid;
					}
					slot = i;
					break;
				}
			}
			OpenSheetCatalogEntry fresh;
			if( !DescribeOpenSheet( *entry.pObj, fresh ) ) {
				fresh = entry;
				fresh.valid = false;
				fresh.xform = now;
				fresh.box = box;
			}
			if( slot < posed.size() ) {
				posed[slot] = fresh;
			} else {
				posed.push_back( fresh );
			}
			out = fresh;
			return fresh.valid;
		}

		//! True when @a pObj is one of the scene's objects (a stack key that
		//! may be dereferenced).  Uses the catalog's sorted object set.
		inline bool IsSceneObject( const IObjectManager* pObjects, const IObject* pObj )
		{
			if( !pObjects || !pObj ) {
				return false;
			}
			OpenSheetsOf( pObjects );
			const std::vector<const IObject*>& objs = ThreadOpenSheetCatalog().objects;
			return std::binary_search( objs.begin(), objs.end(), pObj );
		}

		//! The scene's length scale (bounds diagonal) for self-hit tolerances.
		inline Scalar SceneLengthScale( const IObjectManager* pObjects )
		{
			if( !pObjects ) {
				return Scalar( 1 );
			}
			OpenSheetsOf( pObjects );
			return ThreadOpenSheetCatalog().sceneDiagonal;
		}

		//! DL-618: the catalog's description of @a pObj, or a fresh one when
		//! it is not listed.  False when @a pObj is not an open sheet.
		inline bool FindOpenSheet( const IObjectManager* pObjects, const IObject* pObj, OpenSheetCatalogEntry& out )
		{
			if( pObjects ) {
				const std::vector<OpenSheetCatalogEntry>& sheets = OpenSheetsOf( pObjects );
				for( std::size_t i = 0; i < sheets.size(); i++ ) {
					if( sheets[i].pObj == pObj ) {
						return PosedOpenSheet( sheets[i], out );
					}
				}
			}
			return pObj && DescribeOpenSheet( *pObj, out );
		}

		//! DL-622: dynamic ray-march step budget scaling with the scene's open
		//! sheet count so deep stacks of open sheets (e.g. 12 or 50 panes)
		//! neither truncate normal-ray confirmations nor starve closed-object
		//! parity probes.
		inline int MaxSceneWalkSteps( const IObjectManager* pObjects )
		{
			const std::size_t nSheets = pObjects ? OpenSheetsOf( pObjects ).size() : 0;
			return kMaxProbeSteps + static_cast<int>( 2 * nSheets );
		}

		//! DL-622 review round 3: several wavelengths seeded by ONE probe walk.
		//! The probes' paths and every membership decision depend on the
		//! wavelength only through each hit's classification
		//! (`ClassifyMembershipHit`: trackable or not, refracting or a re-push)
		//! and the index it reports.  `SeedFromPointImpl` given a LaneSeed
		//! classifies every hit at each lane's wavelength too; when every
		//! classification agrees with the primary one (`diverged` false) each
		//! lane's seed has the primary's membership exactly, and only the
		//! indices differ -- recorded per lane at the same hit the primary
		//! took its index from.  Open sheets are not covered (their
		//! confirmation compares indices between sheets): a scene with any
		//! sets `diverged`, and the caller seeds each lane on its own.
		static const unsigned int kMaxSeedLanes = 8;
		struct LaneSeed
		{
			unsigned int K;
			Scalar nm[kMaxSeedLanes];
			bool diverged;
			struct Rec
			{
				const IObject* pObj;
				bool repush;
				Scalar ior[kMaxSeedLanes];
			};
			std::vector<Rec> recs;
			LaneSeed() : K( 0 ), diverged( false ) {}
		};

		/// Walks one probe ray from `pos` along `dir` and fills `out` with
		/// the per-object exit/entry parity tally of the CLOSED trackable
		/// objects it meets (certified open refractive sheets have no
		/// interior and are not tallied -- DL-618 seeds them by the
		/// sheet-normal convention).  Returns the number of entries written.
		inline std::size_t TallyProbe(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& dir,
			ProbeEntry* out,
			const Scalar nm = 0,
			const std::size_t capacity = kMaxProbeEntries,
			const int walkSteps = -1,
			LaneSeed* pLanes = 0,
			const bool recordLanes = false
			)
		{
			Diagnostics::ProbeTraceCounter()++;
			std::size_t count = 0;
			const Scalar kSeedEps = Scalar( 1e-4 );
			Ray probe( pos, dir );
			// One step adds at most one entry: never walk more steps than
			// the table holds, so no met object is ever silently dropped
			// (the caller sizes the table to MaxSceneWalkSteps, which it
			// passes as @a walkSteps to spare a catalog lookup per probe).
			const int budget = walkSteps >= 0 ? walkSteps : MaxSceneWalkSteps( pObjects );
			const int maxSteps = static_cast<int>( std::min<std::size_t>(
				static_cast<std::size_t>( budget ), capacity ) );

			for( int step = 0; step < maxSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					break;
				}

				const Scalar cosN = ri.geometric.TrueGeomFacing( probe.Dir() );

				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( ri, probe.Dir(), nm, info );
				Scalar laneIor[kMaxSeedLanes];
				if( pLanes && !pLanes->diverged ) {
					for( unsigned int k = 0; k < pLanes->K; ++k ) {
						SpecularInfo infoK;
						const MembershipHit mk = ClassifyMembershipHit( ri, probe.Dir(), pLanes->nm[k], infoK );
						laneIor[k] = infoK.ior;
						if( mk != membership ||
							( membership == kMembershipTransparent && infoK.canRefract != info.canRefract ) ) {
							pLanes->diverged = true;
						}
					}
				}

				if( membership == kMembershipTransparent )
				{
					ProbeEntry* e = 0;
					for( std::size_t d = 0; d < count; d++ ) {
						if( out[d].pObj == ri.pObject ) {
							e = &out[d];
							break;
						}
					}
					if( !e && count < capacity ) {
						e = &out[count++];
						e->pObj = ri.pObject;
						e->ior = info.ior;
						e->parity = 0;
						e->firstExitStep = -1;
						e->firstEntryStep = -1;
						e->repushParentIor = !info.canRefract;
						if( recordLanes && pLanes && !pLanes->diverged ) {
							LaneSeed::Rec rec;
							rec.pObj = ri.pObject;
							rec.repush = !info.canRefract;
							for( unsigned int k = 0; k < pLanes->K; ++k ) {
								rec.ior[k] = laneIor[k];
							}
							pLanes->recs.push_back( rec );
						}
					}
					if( e )
					{
						if( cosN > 0 ) {
							e->parity++;
							if( e->firstExitStep < 0 ) {
								e->firstExitStep = step;
							}
						} else {
							e->parity--;
							if( e->firstEntryStep < 0 ) {
								e->firstEntryStep = step;
							}
						}
					}
				}

				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
			// DL-444: a closed inward-wound mesh has negative signed face
			// parity even when the point is inside. Normalize that sign only
			// when the object's independent signed-distance query certifies
			// containment.
			for( std::size_t i = 0; i < count; ++i ) {
				ProbeEntry& entry = out[i];
				if( entry.parity >= 0 ) continue;
				const IGeometry* geometry = entry.pObj->GetGeometry();
				if( !geometry ) continue;
				const Point3 local = Point3Ops::Transform( entry.pObj->GetFinalInverseTransformMatrix(), pos );
				const BoundingBox box = geometry->GenerateBoundingBox();
				const Scalar reach = Point3Ops::Distance( local, box.ll ) + Vector3Ops::Magnitude( box.GetExtents() );
				Scalar signedDistance = 0; bool exact = false;
				if( geometry->SignedDistanceLower( local, reach, signedDistance, exact ) && exact && signedDistance < 0 ) {
					entry.parity = -entry.parity;
					entry.firstExitStep = entry.firstEntryStep;
				}
			}
			return count;
		}

		//! DL-622 Gap 2: tests @a pSheet ALONE along Ray(pos, dir).
		//! Requires the first hit to be a back face (`TrueGeomFacing > 0`) and
		//! the net signed crossing parity across all folds of @a pSheet along
		//! the ray (`n_back - n_front`) to be strictly positive.  Writes the
		//! first hit's normal alignment `Dot(n_hit, dir)` to @a outHitCos and
		//! the total number of crossings of @a pSheet along the ray to
		//! @a outTotalCrossings.
		inline bool CheckSheetAloneAlongRay(
			const IObject* pSheet,
			const Point3& pos,
			const Vector3& dir,
			const bool isPlanar = false,
			Scalar* outHitCos = 0,
			int* outTotalCrossings = 0
			)
		{
			if( !pSheet ) {
				return false;
			}
			if( isPlanar ) {
				RayIntersection alone( Ray( pos, dir ), nullRasterizerState );
				pSheet->IntersectRay( alone, RISE_INFINITY, true, true, false );
				if( !alone.geometric.bHit || !alone.geometric.HasTrueGeomSide() ||
					alone.geometric.TrueGeomFacing( dir ) <= 0 ) {
					return false;
				}
				if( outHitCos ) {
					*outHitCos = Vector3Ops::Dot( alone.geometric.UnflippedGeomNormal(), dir );
				}
				if( outTotalCrossings ) {
					*outTotalCrossings = 1;
				}
				return true;
			}
			const Scalar scale = Scalar( 1 ) + std::fabs( pos.x ) + std::fabs( pos.y ) + std::fabs( pos.z );
			const Scalar stepEps = Scalar( 1e-6 ) * scale;
			Ray probe( pos, dir );
			int netBackMinusFront = 0;
			int totalCrossings = 0;
			Scalar firstCos = 0;
			for( int step = 0; step < 32; ++step ) {
				RayIntersection alone( probe, nullRasterizerState );
				pSheet->IntersectRay( alone, RISE_INFINITY, true, true, false );
				if( !alone.geometric.bHit || !alone.geometric.HasTrueGeomSide() ) {
					break;
				}
				const Scalar facing = alone.geometric.TrueGeomFacing( dir );
				if( totalCrossings == 0 ) {
					if( facing <= 0 ) {
						return false;
					}
					firstCos = Vector3Ops::Dot( alone.geometric.UnflippedGeomNormal(), dir );
				}
				if( facing > 0 ) {
					++netBackMinusFront;
				} else {
					--netBackMinusFront;
				}
				++totalCrossings;
				probe.origin = Point3Ops::mkPoint3( alone.geometric.ptIntersection, dir * stepEps );
			}
			if( totalCrossings == 0 || netBackMinusFront <= 0 ) {
				return false;
			}
			if( outHitCos ) {
				*outHitCos = firstCos;
			}
			if( outTotalCrossings ) {
				*outTotalCrossings = totalCrossings;
			}
			return true;
		}

		inline bool OpenSheetNormalRayReaches(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const IObject* pSheet,
			const Scalar nm,
			std::vector<const IObject*>* passedSheets = 0,
			std::vector<const IObject*>* crossedClosed = 0,
			Scalar* sheetIor = 0,
			const IObject* pSeedObject = 0,
			IORStack::EntryContext* outEntryCtx = 0,
			const int expectedSheetCrossings = 1,
			const bool allowDetour = true,
			const bool countProbeTrace = true
			);

		//! DL-622 Gap 1: 2-segment free-space detour witness around a finite
		//! submerged opaque obstacle @a pObstacle (`pos -> w_m -> pSheet`).
		//! Preserves closed-room blocking (both single `box_geometry` rooms and
		//! 6-quad rooms `A23`/`A24`) and self-reentry blocking (`A18` torus
		//! hole) while seeding points beneath submerged opaque bodies (`A27`).
		//! The lateral leg `pos -> w_m` must cross NO surface with a side (a
		//! sealed habitat's glass window is not a way out, `A34`).
		inline bool HasFreeSpaceDetourToSheet(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const IObject* pSheet,
			const int expectedSheetCrossings,
			const IObject* pObstacle,
			const RayIntersection& obstacleHit,
			const Scalar selfTol,
			const Scalar nm,
			std::vector<const IObject*>* passedSheets,
			std::vector<const IObject*>* crossedClosed,
			Scalar* sheetIor,
			const IObject* pSeedObject,
			IORStack::EntryContext* outEntryCtx
			)
		{
			if( !pObjects || !pSheet || !pObstacle || obstacleHit.geometric.range <= selfTol ) {
				return false;
			}
			// Never detour around the seed's own object on a second hit.
			if( pSeedObject && pObstacle == pSeedObject ) {
				return false;
			}
			// An EXIT hit (`TrueGeomFacing > 0`) on a non-open obstacle means
			// `pos` is strictly inside a closed opaque room (`A1`, `A12`,
			// `A21`, `A22`, `A25`, `A26`): reject in 0 rays.
			if( obstacleHit.geometric.TrueGeomFacing( normal ) > 0 &&
				!obstacleHit.geometric.bProvablyNoInterior &&
				!obstacleHit.geometric.bOpenSheet ) {
				return false;
			}
			// Without `pSeedObject`, check if `pos` sits on `pObstacle`'s own
			// surface (`A18` standing torus hole backed off by 1e-12): a hit on
			// `pObstacle` within `selfTol` along `-normal` identifies `pObstacle`
			// as the seed's own body being re-entered across a concavity.
			if( !pSeedObject ) {
				RayIntersection backSelf(
					Ray( Point3Ops::mkPoint3( pos, normal * selfTol ), -normal ),
					nullRasterizerState );
				pObstacle->IntersectRay( backSelf, Scalar( 2 ) * selfTol, true, true, false );
				if( backSelf.geometric.bHit && backSelf.geometric.range <= Scalar( 2 ) * selfTol ) {
					return false;
				}
			}

			const std::vector<const IObject*> prePassed = passedSheets ? *passedSheets : std::vector<const IObject*>();
			const std::vector<const IObject*> preCrossed = crossedClosed ? *crossedClosed : std::vector<const IObject*>();
			std::vector<const IObject*> vertPassed = prePassed;
			std::vector<const IObject*> vertCrossed = preCrossed;

			BoundingBox obsBox = pObstacle->getBoundingBox();
			struct OpaqueParity { const IObject* pObj; int parity; };
			std::vector<OpaqueParity> opaqueObjs;
			if( !obstacleHit.geometric.bProvablyNoInterior && !obstacleHit.geometric.bOpenSheet ) {
				opaqueObjs.push_back( { pObstacle, -1 } );
			}

			const Scalar kSeedEps = Scalar( 1e-4 );
			const int maxSteps = MaxSceneWalkSteps( pObjects );
			Ray vertProbe( obstacleHit.geometric.ptIntersection, normal );
			vertProbe.Advance( kSeedEps );
			int vertSheetCrossings = 0;
			int vertSheetParity = 0;
			bool vertReachedSheet = false;
			for( int step = 0; step < maxSteps; ++step ) {
				RayIntersection vri( vertProbe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				pObjects->IntersectRay( vri, true, true, false );
				if( !vri.geometric.bHit ) {
					break;
				}
				SpecularInfo vInfo;
				const MembershipHit vMem = ClassifyMembershipHit( vri, normal, nm, vInfo );
				if( vMem != kMembershipSkip && vri.pObject == pSheet ) {
					++vertSheetCrossings;
					if( vertSheetCrossings == 1 && vMem != kMembershipSheetBack ) {
						return false;
					}
					if( vMem == kMembershipSheetBack ) {
						++vertSheetParity;
					} else if( vMem == kMembershipSheetFront ) {
						--vertSheetParity;
					} else {
						return false;
					}
					if( vertSheetCrossings >= expectedSheetCrossings ) {
						if( vertSheetParity > 0 ) {
							vertReachedSheet = true;
							break;
						}
						return false;
					}
				} else if( vMem == kMembershipBlock ) {
					if( pSeedObject && vri.pObject == pSeedObject ) {
						return false;
					}
					obsBox.Include( vri.pObject->getBoundingBox() );
					if( !vri.geometric.bProvablyNoInterior && !vri.geometric.bOpenSheet ) {
						const int deltaParity = ( vri.geometric.TrueGeomFacing( normal ) > 0 ) ? +1 : -1;
						bool found = false;
						for( std::size_t k = 0; k < opaqueObjs.size(); ++k ) {
							if( opaqueObjs[k].pObj == vri.pObject ) {
								opaqueObjs[k].parity += deltaParity;
								if( opaqueObjs[k].parity > 0 ) {
									return false;
								}
								found = true;
								break;
							}
						}
						if( !found ) {
							if( deltaParity > 0 ) {
								return false;
							}
							opaqueObjs.push_back( { vri.pObject, deltaParity } );
						}
					}
				} else if( vMem == kMembershipSheetFront ) {
					return false;
				} else if( vMem == kMembershipSheetBack ) {
					if( std::find( vertPassed.begin(), vertPassed.end(), vri.pObject ) == vertPassed.end() ) {
						vertPassed.push_back( vri.pObject );
					}
				} else if( vMem == kMembershipTransparent ) {
					if( std::find( vertCrossed.begin(), vertCrossed.end(), vri.pObject ) == vertCrossed.end() ) {
						vertCrossed.push_back( vri.pObject );
					}
				}
				vertProbe = Ray( vri.geometric.ptIntersection, normal );
				vertProbe.Advance( kSeedEps );
			}
			if( !vertReachedSheet ) {
				return false;
			}
			for( std::size_t k = 0; k < opaqueObjs.size(); ++k ) {
				if( opaqueObjs[k].parity != 0 ) {
					return false;
				}
			}

			const Scalar kHuge = Scalar( 1e100 );
			if( !std::isfinite( obsBox.ll.x ) || !std::isfinite( obsBox.ll.y ) || !std::isfinite( obsBox.ll.z ) ||
				!std::isfinite( obsBox.ur.x ) || !std::isfinite( obsBox.ur.y ) || !std::isfinite( obsBox.ur.z ) ||
				std::fabs( obsBox.ll.x ) > kHuge || std::fabs( obsBox.ll.y ) > kHuge || std::fabs( obsBox.ll.z ) > kHuge ||
				std::fabs( obsBox.ur.x ) > kHuge || std::fabs( obsBox.ur.y ) > kHuge || std::fabs( obsBox.ur.z ) > kHuge ) {
				return false;
			}

			Vector3 U( 1, 0, 0 ), V( 0, 1, 0 );
			MakeOrthonormalTangents( normal, U, V );
			Scalar uMin = RISE_INFINITY, uMax = -RISE_INFINITY;
			Scalar vMin = RISE_INFINITY, vMax = -RISE_INFINITY;
			for( int corner = 0; corner < 8; ++corner ) {
				const Point3 q(
					( corner & 1 ) ? obsBox.ur.x : obsBox.ll.x,
					( corner & 2 ) ? obsBox.ur.y : obsBox.ll.y,
					( corner & 4 ) ? obsBox.ur.z : obsBox.ll.z );
				const Vector3 rel = Vector3Ops::mkVector3( q, pos );
				const Scalar u = Vector3Ops::Dot( rel, U );
				const Scalar v = Vector3Ops::Dot( rel, V );
				uMin = std::min( uMin, u );
				uMax = std::max( uMax, u );
				vMin = std::min( vMin, v );
				vMax = std::max( vMax, v );
			}
			const Scalar span = std::max( uMax - uMin, vMax - vMin );
			const Scalar delta = std::max( Scalar( 1e-3 ), Scalar( 0.05 ) * span );
			if( Scalar( 0 ) < uMin - delta || Scalar( 0 ) > uMax + delta ||
				Scalar( 0 ) < vMin - delta || Scalar( 0 ) > vMax + delta ) {
				return false;
			}

			struct WaypointOffset { Scalar du, dv, distSq; };
			WaypointOffset cardinals[4] = {
				{ uMin - delta, Scalar( 0 ), ( uMin - delta ) * ( uMin - delta ) },
				{ uMax + delta, Scalar( 0 ), ( uMax + delta ) * ( uMax + delta ) },
				{ Scalar( 0 ), vMin - delta, ( vMin - delta ) * ( vMin - delta ) },
				{ Scalar( 0 ), vMax + delta, ( vMax + delta ) * ( vMax + delta ) }
			};
			std::sort( cardinals, cardinals + 4, []( const WaypointOffset& a, const WaypointOffset& b ) {
				return a.distSq < b.distSq;
			} );
			const Scalar uLo = uMin - delta, uHi = uMax + delta;
			const Scalar vLo = vMin - delta, vHi = vMax + delta;
			const WaypointOffset waypoints[8] = {
				cardinals[0], cardinals[1], cardinals[2], cardinals[3],
				{ uLo, vLo, uLo * uLo + vLo * vLo },
				{ uHi, vLo, uHi * uHi + vLo * vLo },
				{ uLo, vHi, uLo * uLo + vHi * vHi },
				{ uHi, vHi, uHi * uHi + vHi * vHi }
			};

			const bool sheetIsPlanar = ( expectedSheetCrossings == 1 );
			for( int m = 0; m < 8; ++m ) {
				const Point3 wm = Point3Ops::mkPoint3(
					Point3Ops::mkPoint3( pos, U * waypoints[m].du ),
					V * waypoints[m].dv );
				int wExpectedCrossings = 1;
				if( !CheckSheetAloneAlongRay( pSheet, wm, normal, sheetIsPlanar, 0, &wExpectedCrossings ) ) {
					continue;
				}

				// Segment 1: lateral ray from `pos` to `wm`, through free space only.
				const Vector3 segVec = Vector3Ops::mkVector3( wm, pos );
				const Scalar segLen = Vector3Ops::Magnitude( segVec );
				if( segLen <= NEARZERO ) {
					continue;
				}
				const Vector3 segDir = segVec * ( Scalar( 1 ) / segLen );
				Ray segProbe( pos, segDir );
				Scalar travelled = Scalar( 0 );
				bool segBlocked = false;
				bool ownChecked = false;
				bool selfHitSkipped = false;
				std::vector<const IObject*> segPassedSheets = vertPassed;
				std::vector<const IObject*> segCrossedClosed = vertCrossed;

				for( int step = 0; step < maxSteps && travelled < segLen; ++step ) {
					RayIntersection sri( segProbe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				pObjects->IntersectRay( sri, true, true, false );
					if( !sri.geometric.bHit || travelled + sri.geometric.range >= segLen ) {
						break;
					}
					SpecularInfo sInfo;
					const MembershipHit sMem = ClassifyMembershipHit( sri, segDir, nm, sInfo );
					if( sMem == kMembershipBlock ) {
						if( pSeedObject && sri.pObject == pSeedObject ) {
							if( !ownChecked && !selfHitSkipped && sri.geometric.range < selfTol ) {
								selfHitSkipped = true;
								travelled += sri.geometric.range + kSeedEps;
								segProbe = Ray( sri.geometric.ptIntersection, segDir );
								segProbe.Advance( kSeedEps );
								continue;
							}
							if( !ownChecked && sri.geometric.TrueGeomFacing( segDir ) > 0 ) {
								ownChecked = true;
								travelled += sri.geometric.range + kSeedEps;
								segProbe = Ray( sri.geometric.ptIntersection, segDir );
								segProbe.Advance( kSeedEps );
								continue;
							}
						}
						segBlocked = true;
						break;
					} else if( sMem != kMembershipSkip ) {
						// Segment 1 must be FREE space.  A closed medium or
						// another sheet crossed sideways separates `pos` from
						// the waypoint just as an opaque wall does: the walk
						// across a sealed habitat's glass window does not
						// carry the sea's index into the habitat, it carries
						// the habitat's out (DL-622 review).  Along the
						// sheet's normal those surfaces stay transparent (the
						// convention above); sideways they are a boundary.
						segBlocked = true;
						break;
					}
					travelled += sri.geometric.range + kSeedEps;
					segProbe = Ray( sri.geometric.ptIntersection, segDir );
					segProbe.Advance( kSeedEps );
				}
				if( segBlocked ) {
					continue;
				}

				// Segment 2: normal ray from `wm` along `normal` to `pSheet` (no recursive detour).
				std::vector<const IObject*> wPassed;
				std::vector<const IObject*> wCrossed = segCrossedClosed;
				Scalar wIor = 0;
				IORStack::EntryContext wCtx;
				if( OpenSheetNormalRayReaches(
						pObjects, wm, normal, pSheet, nm,
						passedSheets ? &wPassed : 0,
						crossedClosed ? &wCrossed : 0,
						&wIor, 0, &wCtx, wExpectedCrossings, false, false ) ) {
					if( passedSheets ) {
						std::vector<const IObject*> mergedPassed = segPassedSheets;
						for( std::size_t k = 0; k < wPassed.size(); ++k ) {
							if( std::find( mergedPassed.begin(), mergedPassed.end(), wPassed[k] ) == mergedPassed.end() ) {
								mergedPassed.push_back( wPassed[k] );
							}
						}
						*passedSheets = mergedPassed;
					}
					if( crossedClosed ) {
						std::vector<const IObject*> mergedCrossed = vertCrossed;
						for( std::size_t k = 0; k < wCrossed.size(); ++k ) {
							if( std::find( mergedCrossed.begin(), mergedCrossed.end(), wCrossed[k] ) == mergedCrossed.end() ) {
								mergedCrossed.push_back( wCrossed[k] );
							}
						}
						*crossedClosed = mergedCrossed;
					}
					if( sheetIor ) *sheetIor = wIor;
					if( outEntryCtx ) *outEntryCtx = wCtx;
					return true;
				}
			}
			return false;
		}

		//! The most scene rays one group certificate spends (see
		//! HasFreeSpaceDetourToSheets).
		static const int kGroupDetourRayBudget = 1024;

		namespace Diagnostics
		{
			//! Test hook: the group certificate's ray budget on this thread
			//! (defaults to kGroupDetourRayBudget).
			inline int& GroupDetourRayBudget()
			{
				static thread_local int budget = kGroupDetourRayBudget;
				return budget;
			}
		}

		//! One sheet's answer from `HasFreeSpaceDetourToSheets`.
		struct DetourResult
		{
			bool ok;
			Scalar ior;
			std::vector<const IObject*> passed;
			std::vector<const IObject*> crossed;
			IORStack::EntryContext ctx;
			DetourResult() : ok( false ), ior( 0 ) {}
		};

		//! DL-622 review: `HasFreeSpaceDetourToSheet` for SEVERAL planar co-
		//! normal sheets at once (a normal group whose shared normal ray from
		//! @a pos is blocked by @a pObstacle), answering each sheet exactly as
		//! its own certificate would, in one column walk plus, per distinct
		//! obstacle box, at most eight waypoints each with one lateral and one
		//! normal walk -- instead of one full certificate per sheet (2652 scene
		//! rays per seed for 50 panes over a rock at the DL-622 commit).
		//!
		//! Why the answers are identical: the single-sheet certificate for
		//! sheet k walks the column from the obstacle and stops at k; every
		//! decision it makes before k (an opaque parity going positive, any
		//! other sheet's front face, the seed object) is a property of that
		//! PREFIX of one shared column walk, so k's verdict, obstacle box and
		//! passed/crossed lists are a snapshot taken when the shared walk
		//! reaches k.  Its waypoints depend only on that box, so sheets
		//! reached with the same box share waypoints and lateral legs; and its
		//! normal walk from a waypoint is again a prefix of one shared walk,
		//! confirming each candidate (one whose own sheet-alone test passes
		//! at that waypoint) at its back face.  A sheet takes the FIRST
		//! waypoint that confirms it, as in the single-sheet loop.
		//! @a out receives one result per entry of @a sheets.
		inline void HasFreeSpaceDetourToSheets(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const std::vector<const IObject*>& sheets,
			const IObject* pObstacle,
			const RayIntersection& obstacleHit,
			const Scalar selfTol,
			const Scalar nm,
			const std::vector<const IObject*>& prePassed,
			const std::vector<const IObject*>& preCrossed,
			const IObject* pSeedObject,
			std::vector<DetourResult>& out
			)
		{
			const std::size_t n = sheets.size();
			out.assign( n, DetourResult() );
			if( !pObjects || n == 0 || !pObstacle || obstacleHit.geometric.range <= selfTol ) {
				return;
			}
			if( pSeedObject && pObstacle == pSeedObject ) {
				return;
			}
			if( obstacleHit.geometric.TrueGeomFacing( normal ) > 0 &&
				!obstacleHit.geometric.bProvablyNoInterior &&
				!obstacleHit.geometric.bOpenSheet ) {
				return;
			}
			if( !pSeedObject ) {
				RayIntersection backSelf(
					Ray( Point3Ops::mkPoint3( pos, normal * selfTol ), -normal ),
					nullRasterizerState );
				pObstacle->IntersectRay( backSelf, Scalar( 2 ) * selfTol, true, true, false );
				if( backSelf.geometric.bHit && backSelf.geometric.range <= Scalar( 2 ) * selfTol ) {
					return;
				}
			}
			auto indexOf = [&]( const IObject* p ) -> std::size_t {
				for( std::size_t i = 0; i < n; ++i ) {
					if( sheets[i] == p ) return i;
				}
				return n;
			};
			auto addUnique = []( std::vector<const IObject*>& v, const IObject* p ) {
				if( std::find( v.begin(), v.end(), p ) == v.end() ) v.push_back( p );
			};

			// Column walk, snapshotting each sheet when it is reached.
			struct Snapshot
			{
				bool reached;
				BoundingBox box;
				std::vector<const IObject*> passed;
				std::vector<const IObject*> crossed;
				Snapshot() : reached( false ) {}
			};
			std::vector<Snapshot> snap( n );
			std::vector<bool> resolved( n, false );
			std::size_t open = n;
			std::vector<const IObject*> vertPassed = prePassed;
			std::vector<const IObject*> vertCrossed = preCrossed;
			BoundingBox obsBox = pObstacle->getBoundingBox();
			struct OpaqueParity { const IObject* pObj; int parity; };
			std::vector<OpaqueParity> opaqueObjs;
			if( !obstacleHit.geometric.bProvablyNoInterior && !obstacleHit.geometric.bOpenSheet ) {
				opaqueObjs.push_back( { pObstacle, -1 } );
			}
			const Scalar kSeedEps = Scalar( 1e-4 );
			const int maxSteps = MaxSceneWalkSteps( pObjects );
			int raysSpent = 0;
			const int rayBudget = Diagnostics::GroupDetourRayBudget();
			Ray vertProbe( obstacleHit.geometric.ptIntersection, normal );
			vertProbe.Advance( kSeedEps );
			for( int step = 0; step < maxSteps && open > 0; ++step ) {
				RayIntersection vri( vertProbe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				raysSpent++;
				pObjects->IntersectRay( vri, true, true, false );
				if( !vri.geometric.bHit ) {
					break;
				}
				SpecularInfo vInfo;
				const MembershipHit vMem = ClassifyMembershipHit( vri, normal, nm, vInfo );
				const std::size_t t = ( vMem != kMembershipSkip ) ? indexOf( vri.pObject ) : n;
				if( t < n && !resolved[t] && vMem != kMembershipBlock ) {
					resolved[t] = true;
					open--;
					if( vMem == kMembershipSheetBack ) {
						bool parityClear = true;
						for( std::size_t k = 0; k < opaqueObjs.size(); ++k ) {
							if( opaqueObjs[k].parity != 0 ) parityClear = false;
						}
						if( parityClear ) {
							snap[t].reached = true;
							snap[t].box = obsBox;
							snap[t].passed = vertPassed;
							snap[t].crossed = vertCrossed;
						}
						addUnique( vertPassed, vri.pObject );
					} else {
						// Its first crossing is not a back face: this sheet
						// fails; to every later sheet it is another sheet's
						// front face (or a non-sheet), which stops the column.
						break;
					}
				} else if( vMem == kMembershipBlock ) {
					// A group sheet not trackable here (e.g. its ior reads <= 0
					// at this wavelength) fails, and is an opaque obstacle to
					// every later sheet, exactly as in each later sheet's own
					// certificate.
					if( t < n && !resolved[t] ) {
						resolved[t] = true;
						open--;
					}
					if( pSeedObject && vri.pObject == pSeedObject ) {
						break;
					}
					obsBox.Include( vri.pObject->getBoundingBox() );
					if( !vri.geometric.bProvablyNoInterior && !vri.geometric.bOpenSheet ) {
						const int deltaParity = ( vri.geometric.TrueGeomFacing( normal ) > 0 ) ? +1 : -1;
						bool found = false, stop = false;
						for( std::size_t k = 0; k < opaqueObjs.size(); ++k ) {
							if( opaqueObjs[k].pObj == vri.pObject ) {
								opaqueObjs[k].parity += deltaParity;
								stop = opaqueObjs[k].parity > 0;
								found = true;
								break;
							}
						}
						if( !found ) {
							if( deltaParity > 0 ) {
								stop = true;
							} else {
								opaqueObjs.push_back( { vri.pObject, deltaParity } );
							}
						}
						if( stop ) {
							break;
						}
					}
				} else if( vMem == kMembershipSheetFront ) {
					break;
				} else if( vMem == kMembershipSheetBack ) {
					addUnique( vertPassed, vri.pObject );
				} else if( vMem == kMembershipTransparent ) {
					addUnique( vertCrossed, vri.pObject );
				}
				vertProbe = Ray( vri.geometric.ptIntersection, normal );
				vertProbe.Advance( kSeedEps );
			}

			// Waypoints, per distinct obstacle box.  Interleaved opaque
			// obstacles (glass shelves carrying vases) give every sheet its
			// own box, and when waypoints keep failing trying them all costs
			// O(N * 8 * MaxSceneWalkSteps) scene rays.  So the certificate
			// spends at most kGroupDetourRayBudget scene rays (column walk
			// included); once they are spent every sheet it has not yet
			// confirmed is REFUSED (not in its medium).  The refusal is
			// deterministic in (point, group), touches only this group's
			// detour-confirmed sheets (a sheet its shared normal ray reached
			// directly is unaffected), and seeds and unpushed-exit resolutions
			// both decide membership through ConfirmOpenSheetsAt, so they
			// refuse alike.
			std::vector<bool> boxDone( n, false );
			Vector3 U( 1, 0, 0 ), V( 0, 1, 0 );
			MakeOrthonormalTangents( normal, U, V );
			const Scalar kHuge = Scalar( 1e100 );
			for( std::size_t lead = 0; lead < n; ++lead ) {
				if( !snap[lead].reached || boxDone[lead] ) {
					continue;
				}
				std::vector<std::size_t> cluster;
				for( std::size_t k = lead; k < n; ++k ) {
					if( snap[k].reached && !boxDone[k] && SameBox( snap[k].box, snap[lead].box ) ) {
						cluster.push_back( k );
						boxDone[k] = true;
					}
				}
				if( raysSpent >= rayBudget ) {
					break;
				}
				const BoundingBox& box = snap[lead].box;
				if( !std::isfinite( box.ll.x ) || !std::isfinite( box.ll.y ) || !std::isfinite( box.ll.z ) ||
					!std::isfinite( box.ur.x ) || !std::isfinite( box.ur.y ) || !std::isfinite( box.ur.z ) ||
					std::fabs( box.ll.x ) > kHuge || std::fabs( box.ll.y ) > kHuge || std::fabs( box.ll.z ) > kHuge ||
					std::fabs( box.ur.x ) > kHuge || std::fabs( box.ur.y ) > kHuge || std::fabs( box.ur.z ) > kHuge ) {
					continue;
				}
				Scalar uMin = RISE_INFINITY, uMax = -RISE_INFINITY;
				Scalar vMin = RISE_INFINITY, vMax = -RISE_INFINITY;
				for( int corner = 0; corner < 8; ++corner ) {
					const Point3 q(
						( corner & 1 ) ? box.ur.x : box.ll.x,
						( corner & 2 ) ? box.ur.y : box.ll.y,
						( corner & 4 ) ? box.ur.z : box.ll.z );
					const Vector3 rel = Vector3Ops::mkVector3( q, pos );
					const Scalar u = Vector3Ops::Dot( rel, U );
					const Scalar v = Vector3Ops::Dot( rel, V );
					uMin = std::min( uMin, u );
					uMax = std::max( uMax, u );
					vMin = std::min( vMin, v );
					vMax = std::max( vMax, v );
				}
				const Scalar span = std::max( uMax - uMin, vMax - vMin );
				const Scalar delta = std::max( Scalar( 1e-3 ), Scalar( 0.05 ) * span );
				if( Scalar( 0 ) < uMin - delta || Scalar( 0 ) > uMax + delta ||
					Scalar( 0 ) < vMin - delta || Scalar( 0 ) > vMax + delta ) {
					continue;
				}
				struct WaypointOffset { Scalar du, dv, distSq; };
				WaypointOffset cardinals[4] = {
					{ uMin - delta, Scalar( 0 ), ( uMin - delta ) * ( uMin - delta ) },
					{ uMax + delta, Scalar( 0 ), ( uMax + delta ) * ( uMax + delta ) },
					{ Scalar( 0 ), vMin - delta, ( vMin - delta ) * ( vMin - delta ) },
					{ Scalar( 0 ), vMax + delta, ( vMax + delta ) * ( vMax + delta ) }
				};
				std::sort( cardinals, cardinals + 4, []( const WaypointOffset& a, const WaypointOffset& b ) {
					return a.distSq < b.distSq;
				} );
				const Scalar uLo = uMin - delta, uHi = uMax + delta;
				const Scalar vLo = vMin - delta, vHi = vMax + delta;
				const WaypointOffset waypoints[8] = {
					cardinals[0], cardinals[1], cardinals[2], cardinals[3],
					{ uLo, vLo, uLo * uLo + vLo * vLo },
					{ uHi, vLo, uHi * uHi + vLo * vLo },
					{ uLo, vHi, uLo * uLo + vHi * vHi },
					{ uHi, vHi, uHi * uHi + vHi * vHi }
				};
				for( int m = 0; m < 8; ++m ) {
					const Point3 wm = Point3Ops::mkPoint3(
						Point3Ops::mkPoint3( pos, U * waypoints[m].du ),
						V * waypoints[m].dv );
					std::vector<bool> candidate( n, false );
					bool anyCandidate = false;
					for( std::size_t c = 0; c < cluster.size(); ++c ) {
						const std::size_t k = cluster[c];
						if( !out[k].ok && CheckSheetAloneAlongRay( sheets[k], wm, normal, true ) ) {
							candidate[k] = true;
							anyCandidate = true;
						}
					}
					if( !anyCandidate ) {
						continue;
					}

					// Segment 1: free space only (see HasFreeSpaceDetourToSheet).
					const Vector3 segVec = Vector3Ops::mkVector3( wm, pos );
					const Scalar segLen = Vector3Ops::Magnitude( segVec );
					if( segLen <= NEARZERO ) {
						continue;
					}
					const Vector3 segDir = segVec * ( Scalar( 1 ) / segLen );
					Ray segProbe( pos, segDir );
					Scalar travelled = Scalar( 0 );
					bool segBlocked = false;
					bool ownChecked = false;
					bool selfHitSkipped = false;
					for( int step = 0; step < maxSteps && travelled < segLen; ++step ) {
						if( raysSpent >= rayBudget ) {
							segBlocked = true;
							break;
						}
						RayIntersection sri( segProbe, nullRasterizerState );
						Diagnostics::SceneRayCounter()++;
						raysSpent++;
						pObjects->IntersectRay( sri, true, true, false );
						if( !sri.geometric.bHit || travelled + sri.geometric.range >= segLen ) {
							break;
						}
						SpecularInfo sInfo;
						const MembershipHit sMem = ClassifyMembershipHit( sri, segDir, nm, sInfo );
						if( sMem == kMembershipBlock && pSeedObject && sri.pObject == pSeedObject &&
							!ownChecked && ( ( !selfHitSkipped && sri.geometric.range < selfTol ) ||
								sri.geometric.TrueGeomFacing( segDir ) > 0 ) ) {
							if( !selfHitSkipped && sri.geometric.range < selfTol ) {
								selfHitSkipped = true;
							} else {
								ownChecked = true;
							}
							travelled += sri.geometric.range + kSeedEps;
							segProbe = Ray( sri.geometric.ptIntersection, segDir );
							segProbe.Advance( kSeedEps );
							continue;
						}
						if( sMem != kMembershipSkip ) {
							segBlocked = true;
							break;
						}
						travelled += sri.geometric.range + kSeedEps;
						segProbe = Ray( sri.geometric.ptIntersection, segDir );
						segProbe.Advance( kSeedEps );
					}
					if( segBlocked ) {
						continue;
					}

					// Segment 2: one normal walk from `wm` confirming each
					// candidate at its back face (OpenSheetNormalRayReaches with
					// no detour and no seed object, as a prefix per sheet).
					std::vector<const IObject*> wPassed;
					std::vector<const IObject*> wCrossed;
					Ray probe( wm, normal );
					for( int step = 0; step < maxSteps && raysSpent < rayBudget; ++step ) {
						RayIntersection ri( probe, nullRasterizerState );
						Diagnostics::SceneRayCounter()++;
						raysSpent++;
						pObjects->IntersectRay( ri, true, true, false );
						if( !ri.geometric.bHit ) {
							break;
						}
						SpecularInfo info;
						const MembershipHit membership = ClassifyMembershipHit( ri, normal, nm, info );
						const std::size_t t = ( membership != kMembershipSkip ) ? indexOf( ri.pObject ) : n;
						if( t < n && candidate[t] ) {
							candidate[t] = false;
							if( membership != kMembershipSheetBack ) {
								break;
							}
							DetourResult& r = out[t];
							r.ok = true;
							r.ior = info.ior;
							r.ctx = IORStack::EntryContext::FromGeometric( ri.geometric, ri.pMaterial, false );
							r.passed = snap[t].passed;
							for( std::size_t k = 0; k < wPassed.size(); ++k ) addUnique( r.passed, wPassed[k] );
							r.crossed = snap[t].crossed;
							for( std::size_t k = 0; k < wCrossed.size(); ++k ) addUnique( r.crossed, wCrossed[k] );
							addUnique( wPassed, ri.pObject );
						} else if( membership == kMembershipBlock || membership == kMembershipSheetFront ) {
							break;
						} else if( membership == kMembershipSheetBack ) {
							addUnique( wPassed, ri.pObject );
						} else if( membership == kMembershipTransparent ) {
							addUnique( wCrossed, ri.pObject );
						}
						bool more = false;
						for( std::size_t c = 0; c < cluster.size(); ++c ) {
							if( candidate[cluster[c]] ) { more = true; break; }
						}
						if( !more ) {
							break;
						}
						probe = Ray( ri.geometric.ptIntersection, normal );
						probe.Advance( kSeedEps );
					}
				}
			}
		}

		//! DL-618 / DL-622: the sheet-normal confirmation of the convention
		//! above.  Walks the ray from @a pos along @a normal (the candidate
		//! sheet's own true normal or an auxiliary facet normal) and returns
		//! true iff @a pSheet is reached on its BACK face with net positive
		//! back-minus-front parity across @a expectedSheetCrossings crossings.
		//!
		//! DL-622 Gap 4: only an explicit @a pSeedObject (passed by luminaire
		//! light-root and photon callers) may be exempted on its initial self-hit
		//! or first exit face; callers with `pSeedObject == 0` never guess own-body
		//! membership geometrically (`A22`).
		//!
		//! DL-622 Gap 1: when @a allowDetour is true and the normal ray hits a
		//! non-exempt opaque obstacle, tests `HasFreeSpaceDetourToSheet` around
		//! the obstacle's lateral bounds.
		inline bool OpenSheetNormalRayReaches(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const IObject* pSheet,
			const Scalar nm,
			std::vector<const IObject*>* passedSheets,
			std::vector<const IObject*>* crossedClosed,
			Scalar* sheetIor,
			const IObject* pSeedObject,
			IORStack::EntryContext* outEntryCtx,
			const int expectedSheetCrossings,
			const bool allowDetour,
			const bool countProbeTrace
			)
		{
			if( countProbeTrace ) {
				Diagnostics::ProbeTraceCounter()++;
			}
			if( passedSheets ) passedSheets->clear();
			if( crossedClosed && allowDetour ) crossedClosed->clear();
			const Scalar kSeedEps = Scalar( 1e-4 );
			const Scalar posMax = std::max( { Scalar( 1 ), std::fabs( pos.x ), std::fabs( pos.y ), std::fabs( pos.z ) } );
			const Scalar selfTol = std::min( Scalar( 1e-7 ) * posMax, Scalar( 1e-5 ) * SceneLengthScale( pObjects ) );
			bool ownChecked = false;
			bool selfHitSkipped = false;
			int sheetCrossingsSeen = 0;
			int sheetNetParity = 0;
			const int maxSteps = MaxSceneWalkSteps( pObjects );
			Ray probe( pos, normal );

			for( int step = 0; step < maxSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					return false;
				}
				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( ri, probe.Dir(), nm, info );
				if( membership != kMembershipSkip && ri.pObject == pSheet ) {
					++sheetCrossingsSeen;
					if( sheetCrossingsSeen == 1 ) {
						if( membership != kMembershipSheetBack ) {
							return false;
						}
						if( sheetIor ) *sheetIor = info.ior;
						if( outEntryCtx ) {
							*outEntryCtx = IORStack::EntryContext::FromGeometric( ri.geometric, ri.pMaterial, false );
						}
					}
					if( membership == kMembershipSheetBack ) {
						++sheetNetParity;
					} else if( membership == kMembershipSheetFront ) {
						--sheetNetParity;
					} else {
						return false;
					}
					if( sheetCrossingsSeen >= expectedSheetCrossings ) {
						return sheetNetParity > 0;
					}
					probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
					probe.Advance( kSeedEps );
					continue;
				}
				if( membership == kMembershipBlock ) {
					// DL-622 Gap 4: only an explicit `pSeedObject` is exempt on
					// its near-origin self-hit or first exit face.
					if( pSeedObject && ri.pObject == pSeedObject ) {
						if( !ownChecked && !selfHitSkipped && ri.geometric.range < selfTol ) {
							selfHitSkipped = true;
							probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
							probe.Advance( kSeedEps );
							continue;
						}
						if( !ownChecked && ri.geometric.TrueGeomFacing( probe.Dir() ) > 0 ) {
							ownChecked = true;
							probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
							probe.Advance( kSeedEps );
							continue;
						}
					}
					ownChecked = true;
					if( allowDetour && sheetCrossingsSeen == 0 ) {
						return HasFreeSpaceDetourToSheet(
							pObjects, pos, normal, pSheet, expectedSheetCrossings,
							ri.pObject, ri, selfTol, nm,
							passedSheets, crossedClosed, sheetIor, pSeedObject, outEntryCtx );
					}
					return false;
				} else if( membership == kMembershipSheetFront ) {
					return false;
				} else if( membership == kMembershipSheetBack && passedSheets ) {
					if( std::find( passedSheets->begin(), passedSheets->end(), ri.pObject ) == passedSheets->end() ) {
						passedSheets->push_back( ri.pObject );
					}
				} else if( membership == kMembershipTransparent && crossedClosed ) {
					if( std::find( crossedClosed->begin(), crossedClosed->end(), ri.pObject ) == crossedClosed->end() ) {
						crossedClosed->push_back( ri.pObject );
					}
				}
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
			return false;
		}

		//! Slab test of Ray(pos, dir) against @a box.
		inline bool RayMeetsSheetBounds( const Point3& pos, const Vector3& dir, const BoundingBox& box )
		{
			const Scalar o[3] = { pos.x, pos.y, pos.z };
			const Scalar d[3] = { dir.x, dir.y, dir.z };
			const Scalar lo[3] = { box.ll.x, box.ll.y, box.ll.z };
			const Scalar hi[3] = { box.ur.x, box.ur.y, box.ur.z };
			Scalar t0 = 0, t1 = RISE_INFINITY;
			for( int a = 0; a < 3; a++ ) {
				const Scalar pad = Scalar( 1e-6 ) * ( Scalar( 1 ) + std::fabs( lo[a] ) + std::fabs( hi[a] ) );
				if( std::fabs( d[a] ) < NEARZERO ) {
					if( o[a] < lo[a] - pad || o[a] > hi[a] + pad ) {
						return false;
					}
					continue;
				}
				Scalar ta = ( lo[a] - pad - o[a] ) / d[a];
				Scalar tb = ( hi[a] + pad - o[a] ) / d[a];
				if( ta > tb ) std::swap( ta, tb );
				t0 = std::max( t0, ta );
				t1 = std::min( t1, tb );
				if( t0 > t1 ) {
					return false;
				}
			}
			return true;
		}

		static const Scalar kAuxNormalAlignCos = Scalar( 0.95 );

		//! DL-618 / DL-622: does sheet @a sheet's medium hold @a pos under the
		//! sheet-normal convention (including multi-crossing fold parity,
		//! auxiliary facet normals on non-planar sheets, and free-space detour
		//! witnesses around submerged opaque bodies)?
		inline bool OpenSheetHoldsPoint(
			const OpenSheetCatalogEntry& sheet,
			const IObjectManager* pObjects,
			const Point3& pos,
			const Scalar nm,
			std::vector<const IObject*>* passedSheets = 0,
			std::vector<const IObject*>* crossedClosed = 0,
			Scalar* sheetIor = 0,
			const IObject* pSeedObject = 0,
			IORStack::EntryContext* outEntryCtx = 0
			)
		{
			const Vector3& n = sheet.normal;
			if( Vector3Ops::Dot( Vector3Ops::mkVector3( pos, sheet.p0 ), n ) < sheet.heightAbove &&
				RayMeetsSheetBounds( pos, n, sheet.box ) ) {
				int expectedCrossings = 1;
				if( CheckSheetAloneAlongRay( sheet.pObj, pos, n, sheet.isPlanar, 0, &expectedCrossings ) ) {
					if( OpenSheetNormalRayReaches(
							pObjects, pos, n, sheet.pObj, nm,
							passedSheets, crossedClosed, sheetIor, pSeedObject, outEntryCtx,
							expectedCrossings, true, true ) ) {
						return true;
					}
				} else if( sheet.isPlanar ) {
					return false;
				}
			} else if( sheet.isPlanar ) {
				return false;
			}

			// DL-622 Gap 2: for non-planar / folded sheets (`!sheet.isPlanar`),
			// test the clustered auxiliary facet normals `auxNormals`.  Each
			// auxiliary normal `n_j` must hit a facet whose own true geometric
			// normal aligns with `n_j` (`cos >= 0.95`) so an auxiliary normal
			// from one facet cannot falsely confirm an oblique hit on another.
			for( std::size_t j = 0; j < sheet.auxNormals.size(); ++j ) {
				const Vector3& nj = sheet.auxNormals[j];
				if( !RayMeetsSheetBounds( pos, nj, sheet.box ) ) {
					continue;
				}
				Scalar hitCos = 0;
				int expectedCrossings = 1;
				if( !CheckSheetAloneAlongRay( sheet.pObj, pos, nj, false, &hitCos, &expectedCrossings ) ||
					hitCos < kAuxNormalAlignCos ) {
					continue;
				}
				if( OpenSheetNormalRayReaches(
						pObjects, pos, nj, sheet.pObj, nm,
						passedSheets, crossedClosed, sheetIor, pSeedObject, outEntryCtx,
						expectedCrossings, true, true ) ) {
					return true;
				}
			}
			return false;
		}

		inline bool HasPositiveParity(
			const ProbeEntry* entries,
			std::size_t count,
			const IObject* pObj
			)
		{
			for( std::size_t i = 0; i < count; i++ ) {
				if( entries[i].pObj == pObj && entries[i].parity > 0 ) {
					return true;
				}
			}
			return false;
		}

		struct ConfirmedSheet
		{
			const IObject* pObj;
			Scalar ior;
			std::vector<const IObject*> passed;
			std::vector<const IObject*> crossed;
			std::size_t closedInner;	// confirmed closed media inner to this sheet
			IORStack::EntryContext entryCtx;
			bool keep;

			ConfirmedSheet() : pObj( 0 ), ior( 0 ), closedInner( 0 ), keep( false ) {}
		};

		//! DL-622 Gap 5: confirms all active candidate sheets in a planar
		//! `OpenSheetNormalGroup` with ONE shared scene ray walk along
		//! `grp.normal` (instead of M separate BVH ray walks).
		inline void ConfirmSharedNormalRay(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const std::vector<const IObject*>& activeObjects,
			const Scalar nm,
			const IObject* pSeedObject,
			ProbeEntry* const* ordered,
			const std::size_t orderedCount,
			std::vector<ConfirmedSheet>& confirmedOut
			)
		{
			if( activeObjects.empty() ) {
				return;
			}
			if( activeObjects.size() == 1 ) {
				ConfirmedSheet c;
				c.pObj = activeObjects[0];
				if( OpenSheetNormalRayReaches(
						pObjects, pos, normal, c.pObj, nm,
						&c.passed, &c.crossed, &c.ior, pSeedObject, &c.entryCtx,
						1, true, true ) ) {
					c.closedInner = 0;
					for( std::size_t k = 0; k < c.crossed.size(); ++k ) {
						for( std::size_t m = 0; m < orderedCount; ++m ) {
							if( ordered[m]->pObj == c.crossed[k] ) {
								c.closedInner++;
								break;
							}
						}
					}
					c.keep = true;
					confirmedOut.push_back( c );
				}
				return;
			}

			Diagnostics::ProbeTraceCounter()++;
			const Scalar kSeedEps = Scalar( 1e-4 );
			const Scalar posMax = std::max( { Scalar( 1 ), std::fabs( pos.x ), std::fabs( pos.y ), std::fabs( pos.z ) } );
			const Scalar selfTol = std::min( Scalar( 1e-7 ) * posMax, Scalar( 1e-5 ) * SceneLengthScale( pObjects ) );
			bool ownChecked = false;
			bool selfHitSkipped = false;
			const int maxSteps = MaxSceneWalkSteps( pObjects );
			Ray probe( pos, normal );

			std::vector<const IObject*> passedSoFar;
			std::vector<const IObject*> crossedSoFar;
			std::vector<bool> done( activeObjects.size(), false );
			std::size_t remaining = activeObjects.size();

			for( int step = 0; step < maxSteps && remaining > 0; ++step ) {
				RayIntersection ri( probe, nullRasterizerState );
				Diagnostics::SceneRayCounter()++;
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					break;
				}
				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( ri, probe.Dir(), nm, info );
				if( membership == kMembershipSkip ) {
					probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
					probe.Advance( kSeedEps );
					continue;
				}
				if( membership == kMembershipBlock ) {
					if( pSeedObject && ri.pObject == pSeedObject ) {
						if( !ownChecked && !selfHitSkipped && ri.geometric.range < selfTol ) {
							selfHitSkipped = true;
							probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
							probe.Advance( kSeedEps );
							continue;
						}
						if( !ownChecked && ri.geometric.TrueGeomFacing( probe.Dir() ) > 0 ) {
							ownChecked = true;
							probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
							probe.Advance( kSeedEps );
							continue;
						}
					}
					ownChecked = true;
					// One detour certificate for every remaining candidate
					// (each answered exactly as its own certificate would be).
					std::vector<const IObject*> remainingObjs;
					for( std::size_t idx = 0; idx < activeObjects.size(); ++idx ) {
						if( !done[idx] ) remainingObjs.push_back( activeObjects[idx] );
					}
					std::vector<DetourResult> results;
					HasFreeSpaceDetourToSheets(
						pObjects, pos, normal, remainingObjs,
						ri.pObject, ri, selfTol, nm,
						passedSoFar, crossedSoFar, pSeedObject, results );
					for( std::size_t r = 0; r < remainingObjs.size(); ++r ) {
						if( !results[r].ok ) continue;
						ConfirmedSheet c;
						c.pObj = remainingObjs[r];
						c.ior = results[r].ior;
						c.passed = results[r].passed;
						c.crossed = results[r].crossed;
						c.entryCtx = results[r].ctx;
						c.closedInner = 0;
						for( std::size_t k = 0; k < c.crossed.size(); ++k ) {
							for( std::size_t m = 0; m < orderedCount; ++m ) {
								if( ordered[m]->pObj == c.crossed[k] ) {
									c.closedInner++;
									break;
								}
							}
						}
						c.keep = true;
						confirmedOut.push_back( c );
					}
					break;
				} else if( membership == kMembershipSheetFront ) {
					break;
				} else if( membership == kMembershipSheetBack ) {
					for( std::size_t idx = 0; idx < activeObjects.size(); ++idx ) {
						if( !done[idx] && activeObjects[idx] == ri.pObject ) {
							done[idx] = true;
							--remaining;
							ConfirmedSheet c;
							c.pObj = ri.pObject;
							c.ior = info.ior;
							c.passed = passedSoFar;
							c.crossed = crossedSoFar;
							c.entryCtx = IORStack::EntryContext::FromGeometric( ri.geometric, ri.pMaterial, false );
							c.closedInner = 0;
							for( std::size_t k = 0; k < c.crossed.size(); ++k ) {
								for( std::size_t m = 0; m < orderedCount; ++m ) {
									if( ordered[m]->pObj == c.crossed[k] ) {
										c.closedInner++;
										break;
									}
								}
							}
							c.keep = true;
							confirmedOut.push_back( c );
							break;
						}
					}
					passedSoFar.push_back( ri.pObject );
					if( remaining == 0 ) {
						break;
					}
				} else if( membership == kMembershipTransparent ) {
					if( std::find( crossedSoFar.begin(), crossedSoFar.end(), ri.pObject ) == crossedSoFar.end() ) {
						crossedSoFar.push_back( ri.pObject );
					}
				}
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
		}

		//! DL-618 / DL-622: the certified open sheets whose medium holds @a pos
		//! under the sheet-normal convention, with the pairwise relations the
		//! seed uses (same-material siblings keep one; different-material
		//! siblings or mutual nesting set @a declineSheets).  Shared by
		//! `SeedFromPointImpl` and `OpenSheetEnclosesCrossing`, so a seed and
		//! an unpushed-exit resolution at the same point agree by construction
		//! (DL-622 review round 3).  @a ordered / @a orderedCount (the closed
		//! media containing @a pos) only feed each sheet's `closedInner`.
		inline void ConfirmOpenSheetsAt(
			const IObjectManager* pObjects,
			const std::vector<OpenSheetCatalogEntry>& sceneSheets,
			const Point3& pos,
			const Scalar nm,
			const IObject* pSeedObject,
			ProbeEntry* const* ordered,
			const std::size_t orderedCount,
			std::vector<ConfirmedSheet>& confirmed,
			bool& declineSheets,
			const IObject* pExclude = 0
			)
		{
			// DL-618 / DL-622: confirm each of the scene's open sheets by its
			// normal ray (using shared normal-ray walks for multi-sheet planar
			// groups and dynamic vectors so arbitrary sheet counts are ordered
			// without an 8-sheet cap).
			const std::vector<OpenSheetCatalogEntry>* pSheets = &sceneSheets;
			if( pSheets && !pSheets->empty() && !declineSheets ) {
				const std::vector<OpenSheetNormalGroup>& groups = ThreadOpenSheetCatalog().normalGroups;
				std::vector<bool> handledByGroup( pSheets->size(), false );

				for( std::size_t gIdx = 0; gIdx < groups.size(); ++gIdx ) {
					const OpenSheetNormalGroup& grp = groups[gIdx];
					if( grp.sheetIndices.size() <= 1 ) {
						continue;
					}
					// Verify no member sheet was re-posed since catalog build (`A20`).
					bool poseValid = true;
					for( std::size_t k = 0; k < grp.sheetIndices.size(); ++k ) {
						const OpenSheetCatalogEntry& ce = ( *pSheets )[grp.sheetIndices[k]];
						if( !SamePose( ce.pObj->GetFinalTransformMatrix(), ce.xform ) ||
							!SameBox( ce.pObj->getBoundingBox(), ce.box ) ) {
							poseValid = false;
							break;
						}
					}
					if( !poseValid ) {
						continue;
					}
					for( std::size_t k = 0; k < grp.sheetIndices.size(); ++k ) {
						handledByGroup[grp.sheetIndices[k]] = true;
					}
					const Vector3 relPos = Vector3Ops::mkVector3( pos, grp.origin );
					const Scalar hPos = Vector3Ops::Dot( relPos, grp.normal );
					if( hPos >= grp.maxHeight ) {
						continue;
					}
					const Scalar uPos = Vector3Ops::Dot( relPos, grp.planeU );
					const Scalar vPos = Vector3Ops::Dot( relPos, grp.planeV );
					std::vector<const IObject*> activeObjects;
					for( std::size_t k = 0; k < grp.sheetIndices.size(); ++k ) {
						if( hPos >= grp.heights[k] ||
							uPos < grp.uMin[k] || uPos > grp.uMax[k] ||
							vPos < grp.vMin[k] || vPos > grp.vMax[k] ) {
							continue;
						}
						const OpenSheetCatalogEntry& ce = ( *pSheets )[grp.sheetIndices[k]];
						if( ce.pObj != pExclude && CheckSheetAloneAlongRay( ce.pObj, pos, grp.normal, true ) ) {
							activeObjects.push_back( ce.pObj );
						}
					}
					if( !activeObjects.empty() ) {
						ConfirmSharedNormalRay(
							pObjects, pos, grp.normal, activeObjects,
							nm, pSeedObject, ordered, orderedCount, confirmed );
					}
				}

				for( std::size_t i = 0; i < pSheets->size() && !declineSheets; i++ ) {
					if( handledByGroup[i] || ( *pSheets )[i].pObj == pExclude ) {
						continue;
					}
					OpenSheetCatalogEntry sheet;
					if( !PosedOpenSheet( ( *pSheets )[i], sheet ) ) {
						continue;
					}
					ConfirmedSheet c;
					c.pObj = sheet.pObj;
					if( !OpenSheetHoldsPoint(
							sheet, pObjects, pos, nm,
							&c.passed, &c.crossed, &c.ior, pSeedObject, &c.entryCtx ) ) {
						continue;
					}
					c.closedInner = 0;
					for( std::size_t k = 0; k < c.crossed.size(); k++ ) {
						for( std::size_t m = 0; m < orderedCount; m++ ) {
							if( ordered[m]->pObj == c.crossed[k] ) {
								c.closedInner++;
								break;
							}
						}
					}
					c.keep = true;
					confirmed.push_back( c );
				}
			}

			// Pairwise relations: `a` nested inside `b` when `a` is on
			// `b`'s normal ray before `b`.  Siblings (neither) of one
			// material keep the nearer-proposed one; siblings of different
			// materials, and mutual nesting, decline every sheet.
			auto passes = []( const ConfirmedSheet& on, const IObject* pObj ) {
				for( std::size_t k = 0; k < on.passed.size(); k++ ) {
					if( on.passed[k] == pObj ) return true;
				}
				return false;
			};
			const std::size_t confirmedCount = confirmed.size();
			for( std::size_t a = 0; a < confirmedCount && !declineSheets; a++ ) {
				for( std::size_t b = a + 1; b < confirmedCount && !declineSheets; b++ ) {
					const bool aInB = passes( confirmed[b], confirmed[a].pObj );
					const bool bInA = passes( confirmed[a], confirmed[b].pObj );
					if( aInB && bInA ) {
						declineSheets = true;
					} else if( !aInB && !bInA ) {
						const IMaterial* ma = confirmed[a].pObj->GetMaterial();
						const IMaterial* mb = confirmed[b].pObj->GetMaterial();
						if( ma && ma == mb && confirmed[a].ior == confirmed[b].ior ) {
							if( confirmed[b].keep ) {
								confirmed[b].keep = false;
							}
						} else {
							declineSheets = true;
						}
					}
				}
			}

		}

		/// Populate `stack` with the dielectric objects that physically
		/// contain `pos`, so that subsequent scatters at the first
		/// enclosing boundary see bFromInside==true, and (DL-618 / DL-622)
		/// with the certified open sheets whose medium contains `pos`.
		inline void SeedFromPointImpl(
			IORStack& stack,
			const Point3& pos,
			const IScene& scene,
			const Scalar nm = 0,
			const bool seedOpenSheets = true,
			const IObject* pSeedObject = 0,
			LaneSeed* pLanes = 0
			)
		{
			stack.SetMediumProbeScene( &scene );

			static const bool sDisabled =
				( std::getenv( "RISE_DISABLE_IOR_STACK_SEEDING" ) != 0 );
			if( sDisabled ) {
				return;
			}

			const IObjectManager* pObjects = scene.GetObjects();
			if( !pObjects ) {
				return;
			}

			// Six probe tables, each sized to the probe's step budget: on the
			// stack while that budget fits kStackProbeEntries (no more than
			// 16 open sheets in the scene), on the heap otherwise.
			// One catalog lookup per seed: the open-sheet list (DL-618, also
			// used below) and the walk budget it implies.
			const std::vector<OpenSheetCatalogEntry>& sceneSheets = OpenSheetsOf( pObjects );
			const int walkSteps = kMaxProbeSteps + static_cast<int>( 2 * sceneSheets.size() );
			if( pLanes && seedOpenSheets && !sceneSheets.empty() ) {
				pLanes->diverged = true;	// open sheets: lanes seed on their own
				pLanes = 0;
			}
			const std::size_t probeCap = static_cast<std::size_t>( walkSteps );
			ProbeEntry stackTables[6][kStackProbeEntries];
			std::vector<ProbeEntry> heapTables;
			ProbeEntry* tables[6];
			if( probeCap <= kStackProbeEntries ) {
				for( int t = 0; t < 6; t++ ) tables[t] = stackTables[t];
			} else {
				heapTables.resize( 6 * probeCap );
				for( int t = 0; t < 6; t++ ) tables[t] = &heapTables[t * probeCap];
			}
			const std::size_t tableCap = probeCap <= kStackProbeEntries ? kStackProbeEntries : probeCap;
			ProbeEntry* const containing = tables[0];
			const std::size_t containingCount =
				TallyProbe( pObjects, pos, Vector3( 0, 0, 1 ), containing, nm, tableCap, walkSteps, pLanes, true );

			ProbeEntry* const reverse = tables[1];
			std::size_t reverseCount = 0;
			bool anyCandidate = false;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity > 0 ) { anyCandidate = true; break; }
			}
			if( anyCandidate ) {
				reverseCount = TallyProbe( pObjects, pos, Vector3( 0, 0, -1 ), reverse, nm, tableCap, walkSteps, pLanes, false );
			}

			ProbeEntry* const xForward = tables[2];
			ProbeEntry* const xReverse = tables[3];
			ProbeEntry* const yForward = tables[4];
			ProbeEntry* const yReverse = tables[5];
			std::size_t xForwardCount = 0, xReverseCount = 0;
			std::size_t yForwardCount = 0, yReverseCount = 0;
			bool anyZConfirmed = false;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity > 0 &&
					HasPositiveParity( reverse, reverseCount, containing[i].pObj ) ) {
					anyZConfirmed = true;
					break;
				}
			}
			if( anyZConfirmed ) {
				xForwardCount = TallyProbe( pObjects, pos, Vector3( 1, 0, 0 ), xForward, nm, tableCap, walkSteps, pLanes, false );
				xReverseCount = TallyProbe( pObjects, pos, Vector3( -1, 0, 0 ), xReverse, nm, tableCap, walkSteps, pLanes, false );
				yForwardCount = TallyProbe( pObjects, pos, Vector3( 0, 1, 0 ), yForward, nm, tableCap, walkSteps, pLanes, false );
				yReverseCount = TallyProbe( pObjects, pos, Vector3( 0, -1, 0 ), yReverse, nm, tableCap, walkSteps, pLanes, false );
			}

			ProbeEntry* orderedStack[kStackProbeEntries];
			std::vector<ProbeEntry*> orderedHeap;
			if( tableCap > kStackProbeEntries ) {
				orderedHeap.resize( tableCap );
			}
			ProbeEntry** const ordered = tableCap > kStackProbeEntries ? &orderedHeap[0] : orderedStack;
			std::size_t orderedCount = 0;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity <= 0 ) {
					continue;
				}
				if( !HasPositiveParity( reverse, reverseCount, containing[i].pObj ) ) {
					continue;
				}
				if( !HasPositiveParity( xForward, xForwardCount, containing[i].pObj ) ||
					!HasPositiveParity( xReverse, xReverseCount, containing[i].pObj ) ||
					!HasPositiveParity( yForward, yForwardCount, containing[i].pObj ) ||
					!HasPositiveParity( yReverse, yReverseCount, containing[i].pObj ) ) {
					continue;
				}
				std::size_t j = orderedCount;
				while( j > 0 && ordered[j-1]->firstExitStep <
					containing[i].firstExitStep )
				{
					ordered[j] = ordered[j-1];
					j--;
				}
				ordered[j] = &containing[i];
				orderedCount++;
			}

			// DL-618 / DL-622: the open sheets whose medium holds `pos`.
			std::vector<ConfirmedSheet> confirmed;
			bool declineSheets = !seedOpenSheets;
			if( seedOpenSheets ) {
				ConfirmOpenSheetsAt( pObjects, sceneSheets, pos, nm, pSeedObject,
					ordered, orderedCount, confirmed, declineSheets );
			}
			auto passes = []( const ConfirmedSheet& on, const IObject* pObj ) {
				for( std::size_t k = 0; k < on.passed.size(); k++ ) {
					if( on.passed[k] == pObj ) return true;
				}
				return false;
			};
			const std::size_t confirmedCount = confirmed.size();

			// Merge OUTERMOST-FIRST.
			std::vector<const ConfirmedSheet*> sheetOrder;
			if( !declineSheets && confirmedCount > 0 ) {
				sheetOrder.reserve( confirmedCount );
				for( std::size_t i = 0; i < confirmedCount; i++ ) {
					if( !confirmed[i].keep ) {
						continue;
					}
					const ConfirmedSheet* s = &confirmed[i];
					const std::size_t slot = orderedCount - s->closedInner;
					sheetOrder.push_back( s );
					std::size_t j = sheetOrder.size() - 1;
					while( j > 0 ) {
						const ConfirmedSheet* t = sheetOrder[j-1];
						const std::size_t tSlot = orderedCount - t->closedInner;
						const bool sBeforeT = slot < tSlot ||
							( slot == tSlot && passes( *s, t->pObj ) );
						if( !sBeforeT ) {
							break;
						}
						sheetOrder[j] = sheetOrder[j-1];
						j--;
					}
					sheetOrder[j] = s;
				}
			}

			std::size_t nextSheet = 0;
			const std::size_t sheetOrderCount = sheetOrder.size();
			for( std::size_t i = 0; i <= orderedCount; i++ ) {
				while( nextSheet < sheetOrderCount &&
					orderedCount - sheetOrder[nextSheet]->closedInner <= i ) {
					stack.SetCurrentObject( sheetOrder[nextSheet]->pObj );
					stack.pushOpenSheet( sheetOrder[nextSheet]->ior, sheetOrder[nextSheet]->entryCtx );
					nextSheet++;
				}
				if( i == orderedCount ) {
					break;
				}
				stack.SetCurrentObject( ordered[i]->pObj );
				stack.push( ordered[i]->repushParentIor ? stack.top() : ordered[i]->ior );
			}

			GradedIndexMedium::RecordAt( stack, pos );
		}

		//! Functor wrapper for `SeedFromPoint` so both 3..6-argument direct
		//! calls and `&IORStackSeeding::SeedFromPoint` (e.g.
		//! `tests/SSSBoundaryFurnaceTest.cpp`) compile cleanly.
		struct SeedFromPointFn
		{
			inline void operator()(
				IORStack& stack,
				const Point3& pos,
				const IScene& scene,
				const Scalar nm = 0,
				const bool seedOpenSheets = true,
				const IObject* pSeedObject = 0
				) const
			{
				SeedFromPointImpl( stack, pos, scene, nm, seedOpenSheets, pSeedObject );
			}

			static inline void Call4(
				IORStack& stack,
				const Point3& pos,
				const IScene& scene,
				const Scalar nm
				)
			{
				SeedFromPointImpl( stack, pos, scene, nm, true, 0 );
			}

			typedef void (*Fn4)( IORStack&, const Point3&, const IScene&, const Scalar );
			inline Fn4 operator&() const { return &Call4; }
		};

		static constexpr SeedFromPointFn SeedFromPoint{};

		//! Returns true when @a pObj is a top-level object in @a pObjects
		//! (so it is a genuine scene `IObject*` rather than an opaque/synthetic
		//! stack key such as `CompositeSPF::BottomKey`).
		inline bool IsObjectInManager(
			const IObjectManager* pObjects,
			const IObject* pObj
			)
		{
			return IsSceneObject( pObjects, pObj );
		}

		//! True when @a stack holds an entry pushed through an open sheet's
		//! face (DL-345) -- the one kind of membership a seed point cannot
		//! reconstruct from geometry alone.
		inline bool StackHasOpenSheet( const IORStack& stack )
		{
			for( std::size_t i = 1; i < stack.Depth(); ++i ) {
				if( stack.isOpenSheetAt( i ) ) {
					return true;
				}
			}
			return false;
		}

		//! DL-622: @a source's MEMBERSHIP (objects, order, open-sheet flags)
		//! with each entry's index re-evaluated at companion wavelength @a nm.
		//! False (and @a dest unspecified) when an entry cannot be evaluated;
		//! the caller then falls back to seeding.
		//!
		//! Per entry, outermost first, with the stack's current object set to
		//! the entry and the partial stack below it as the query stack (so a
		//! non-refracting `hasInterior` entry -- DL-46's translucent re-push --
		//! takes its refreshed parent's index, exactly as SeedFromPoint does):
		//!  - a GRADED medium (its ior is a world-position field, DL-09) keeps
		//!    @a source's recorded value: the field is wavelength-independent,
		//!    and that value is the one the walk tracked (GradedIndexMedium::
		//!    Advance / DL-335's RecordEnclosingAt), not the entry point's;
		//!  - an entry with a recorded EntryContext (open-sheet pushes) is
		//!    evaluated at that recorded crossing, in 0 rays;
		//!  - any other scene object is evaluated at a surface hit found by
		//!    probing the object ALONE from @a anchor (ProbeObjectSurfaceHit:
		//!    object-only rays, no scene walk), as SeedFromPoint evaluates a
		//!    closed medium at the probe's hit on it;
		//!  - a key that is not a scene object (a composite's internal layer)
		//!    is never dereferenced: false.
		inline bool ReindexStackForWavelength(
			const IORStack& source,
			IORStack& dest,
			const Scalar nm,
			const Point3& anchor,
			const IScene* pScene = 0
			)
		{
			dest = source;
			const IScene* pProbeScene = pScene ? pScene : dest.MediumProbeScene();
			if( pProbeScene ) {
				dest.SetMediumProbeScene( pProbeScene );
			}
			const IObjectManager* pObjects = pProbeScene ? pProbeScene->GetObjects() : 0;
			const std::size_t depth = dest.Depth();
			IORStack queryStack( dest.EnvironmentIOR() );
			queryStack.SetMediumProbeScene( pProbeScene );
			for( std::size_t i = 1; i < depth; ++i ) {
				const IObject* pObj = dest.ObjectAt( i );
				if( !pObj || !pObjects || !IsObjectInManager( pObjects, pObj ) ) {
					return false;
				}
				const IORStack::EntryContext& ctx = dest.ContextAt( i );
				Scalar newIdx = source.IORAt( i );
				if( !GradedIndexMedium::FieldOf( pObj ) ) {
					RayIntersection hitRi( Ray( anchor, Vector3( 0, 0, 1 ) ), nullRasterizerState );
					const IMaterial* pMat = 0;
					if( ctx.bHasContext ) {
						ctx.PopulateGeometric( hitRi.geometric );
						pMat = ctx.pMaterial ? ctx.pMaterial : pObj->GetMaterial();
					}
					if( !pMat ) {
						// No context, or a composite object (no material of its
						// own) the recorded crossing did not name one for.
						RayIntersection probeRi( Ray( anchor, Vector3( 0, 0, 1 ) ), nullRasterizerState );
						if( ProbeObjectSurfaceHit( pObj, anchor, probeRi ) ) {
							pMat = probeRi.pMaterial ? probeRi.pMaterial : pObj->GetMaterial();
							if( !ctx.bHasContext ) {
								hitRi = probeRi;
							}
						}
					}
					if( !pMat ) {
						return false;
					}
					queryStack.SetCurrentObject( pObj );
					const SpecularInfo info = nm > 0
						? pMat->GetSpecularInfoNM( hitRi.geometric, queryStack, nm )
						: pMat->GetSpecularInfo( hitRi.geometric, queryStack );
					if( !info.valid ) {
						return false;
					}
					newIdx = info.canRefract ? info.ior : queryStack.top();
					if( !( newIdx > 0 ) || !std::isfinite( newIdx ) ) {
						return false;
					}
				}
				dest.SetIORAt( i, newIdx );
				queryStack.SetCurrentObject( pObj );
				if( dest.isOpenSheetAt( i ) ) {
					queryStack.pushOpenSheet( newIdx );
				} else {
					queryStack.push( newIdx );
				}
			}
			GradedIndexMedium::AdoptTrackedTop( dest, source );
			return true;
		}

		//! DL-618 / DL-622: the stack an HWSS COMPANION lane delegates with
		//! when the hero walk hands its segment to per-wavelength walks.
		//!
		//! A hero stack holding an OPEN-SHEET entry carries walk-history
		//! membership no seed point reproduces (a walk that crossed a sheet
		//! and moved outside its normal prism, or under an overhang): the
		//! lane takes the hero's membership re-indexed at @a nm
		//! (ReindexStackForWavelength, no scene walk), falling back to the
		//! seeding below only if an entry cannot be evaluated.  Every other
		//! stack keeps the pre-DL-622 rule unchanged: the hero's seed point
		//! and rule when they reproduce the hero's membership, else the
		//! segment's midpoint (DL-622 review: a re-index would change HWSS
		//! output on walk-dependent closed stacks -- uncertified meshes,
		//! translucent re-pushes, DL-370 neighbour crossings).
		inline void SeedCompanionLane(
			IORStack& lane,
			const IORStack& hero,
			const Point3& origin,
			const Point3& midpoint,
			const IScene& scene,
			const Scalar nm,
			const bool tryOrigin = true
			)
		{
			if( StackHasOpenSheet( hero ) &&
				ReindexStackForWavelength( hero, lane, nm, midpoint, &scene ) ) {
				return;
			}
			if( !tryOrigin ) {
				lane = IORStack( hero.EnvironmentIOR() );
				SeedFromPoint( lane, midpoint, scene, nm );
				return;
			}
			IORStack atOrigin( hero.EnvironmentIOR() );
			SeedFromPoint( atOrigin, origin, scene, nm );
			bool same = atOrigin.Depth() == hero.Depth();
			for( std::size_t i = 0; same && i < hero.Depth(); i++ ) {
				same = atOrigin.ObjectAt( i ) == hero.ObjectAt( i ) && atOrigin.isOpenSheetAt( i ) == hero.isOpenSheetAt( i );
			}
			if( same ) {
				lane = atOrigin;
				return;
			}
			lane = IORStack( hero.EnvironmentIOR() );
			SeedFromPoint( lane, midpoint, scene, nm );
		}

		//! DL-622 review round 3: seeds @a K stacks at @a pos, one per
		//! wavelength in @a nms, with ONE probe walk when that is provably what
		//! @a K separate `SeedFromPoint` calls would produce (see LaneSeed),
		//! and with K separate calls otherwise.  @a out points at K stacks,
		//! each freshly constructed with the environment index.
		inline void SeedLanesFromPoint(
			IORStack* const* out,
			const unsigned int K,
			const Scalar* nms,
			const Point3& pos,
			const IScene& scene
			)
		{
			if( K == 0 ) {
				return;
			}
			if( K > kMaxSeedLanes ) {
				for( unsigned int k = 0; k < K; ++k ) {
					SeedFromPoint( *out[k], pos, scene, nms[k] );
				}
				return;
			}
			LaneSeed lanes;
			lanes.K = K;
			for( unsigned int k = 0; k < K; ++k ) {
				lanes.nm[k] = nms[k];
			}
			SeedFromPointImpl( *out[0], pos, scene, nms[0], true, 0, &lanes );
			if( lanes.diverged ) {
				for( unsigned int k = 1; k < K; ++k ) {
					SeedFromPoint( *out[k], pos, scene, nms[k] );
				}
				return;
			}
			for( unsigned int k = 1; k < K; ++k ) {
				IORStack& lane = *out[k];
				const IORStack& primary = *out[0];
				lane.SetMediumProbeScene( &scene );
				bool ok = true;
				for( std::size_t i = 1; i < primary.Depth() && ok; ++i ) {
					const IObject* pObj = primary.ObjectAt( i );
					const LaneSeed::Rec* rec = 0;
					for( std::size_t r = 0; r < lanes.recs.size(); ++r ) {
						if( lanes.recs[r].pObj == pObj ) {
							rec = &lanes.recs[r];
							break;
						}
					}
					if( !rec ) {
						ok = false;
						break;
					}
					lane.SetCurrentObject( pObj );
					lane.push( rec->repush ? lane.top() : rec->ior[k] );
				}
				if( !ok ) {
					lane = IORStack( primary.EnvironmentIOR() );
					SeedFromPoint( lane, pos, scene, nms[k] );
					continue;
				}
				GradedIndexMedium::RecordAt( lane, pos );
			}
		}

		//! `SeedCompanionLane` for every active lane at once (@a active[k],
		//! wavelength @a nms[k], K lanes): one shared probe walk per seed point
		//! where `SeedLanesFromPoint` proves the lanes' membership equal, the
		//! per-lane rule otherwise.  @a lanes holds K stacks constructed with
		//! the environment index; inactive ones are left untouched.
		inline void SeedCompanionLanes(
			IORStack* lanes,
			const bool* active,
			const Scalar* nms,
			const unsigned int K,
			const IORStack& hero,
			const Point3& origin,
			const Point3& midpoint,
			const IScene& scene,
			const bool tryOrigin = true
			)
		{
			unsigned int idx[kMaxSeedLanes];
			Scalar lnm[kMaxSeedLanes];
			unsigned int n = 0;
			for( unsigned int k = 0; k < K; ++k ) {
				if( active[k] ) {
					if( n == kMaxSeedLanes || StackHasOpenSheet( hero ) ) {
						n = kMaxSeedLanes + 1;
						break;
					}
					idx[n] = k;
					lnm[n] = nms[k];
					++n;
				}
			}
			if( n > kMaxSeedLanes ) {
				for( unsigned int k = 0; k < K; ++k ) {
					if( active[k] ) {
						SeedCompanionLane( lanes[k], hero, origin, midpoint, scene, nms[k], tryOrigin );
					}
				}
				return;
			}
			if( n == 0 ) {
				return;
			}
			IORStack* targets[kMaxSeedLanes];
			for( unsigned int j = 0; j < n; ++j ) {
				targets[j] = &lanes[idx[j]];
			}
			IORStack* midTargets[kMaxSeedLanes];
			Scalar midNm[kMaxSeedLanes];
			unsigned int m = 0;
			if( tryOrigin ) {
				SeedLanesFromPoint( targets, n, lnm, origin, scene );
				for( unsigned int j = 0; j < n; ++j ) {
					const IORStack& seeded = *targets[j];
					bool same = seeded.Depth() == hero.Depth();
					for( std::size_t i = 0; same && i < hero.Depth(); i++ ) {
						same = seeded.ObjectAt( i ) == hero.ObjectAt( i ) && seeded.isOpenSheetAt( i ) == hero.isOpenSheetAt( i );
					}
					if( !same ) {
						*targets[j] = IORStack( hero.EnvironmentIOR() );
						midTargets[m] = targets[j];
						midNm[m] = lnm[j];
						++m;
					}
				}
			} else {
				for( unsigned int j = 0; j < n; ++j ) {
					midTargets[j] = targets[j];
					midNm[j] = lnm[j];
				}
				m = n;
			}
			SeedLanesFromPoint( midTargets, m, midNm, midpoint, scene );
		}

		//! DL-345: does object @a pY ENCLOSE the crossing at @a p (the walk
		//! travelling along @a dir)?
		inline bool ObjectEnclosesCrossing( const IObject* pY, const Point3& p, const Vector3& dir )
		{
			if( !pY ) {
				return false;
			}
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			const Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			RayIntersection pri( probe, nullRasterizerState );
			pY->IntersectRay( pri, RISE_INFINITY, true, true, false );
			if( pri.geometric.bHit && pri.geometric.HasTrueGeomSide() ) {
				return Vector3Ops::Dot( dir, pri.geometric.UnflippedGeomNormal() ) > 0;
			}
			return false;
		}

		//! DL-382: does closed object @a pY ENCLOSE the crossing at @a p (walk
		//! travelling along @a dir)?  Computed by the PARITY of pY's own crossings
		//! along the probe (odd = inside), which is winding-independent (T5c).
		inline bool ClosedObjectEnclosesCrossing( const IObject* pY, const Point3& p, const Vector3& dir )
		{
			if( !pY ) {
				return false;
			}
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			int crossings = 0;
			for( int step = 0; step < 64; ++step ) {
				RayIntersection pri( probe, nullRasterizerState );
				pY->IntersectRay( pri, RISE_INFINITY, true, true, false );
				if( !pri.geometric.bHit || !pri.geometric.HasTrueGeomSide() ) {
					break;
				}
				++crossings;
				probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
			}
			return ( crossings % 2 ) == 1;
		}

		//! DL-618: is @a p in open sheet @a pY's medium under the sheet-normal
		//! convention?
		inline bool PointInOpenSheetMedium(
			const IObject* pY,
			const IObjectManager* pObjects,
			const Point3& p
			)
		{
			OpenSheetCatalogEntry sheet;
			if( !FindOpenSheet( pObjects, pY, sheet ) ) {
				return false;
			}
			return OpenSheetHoldsPoint( sheet, pObjects, p, 0 );
		}

		//! The open sheets a seed just past an unpushed-exit crossing would hold
		//! (`ConfirmOpenSheetsAt`), computed ONCE per crossing and asked about
		//! each stack entry `ResolveUnpushedExit` considers popping (DL-622
		//! review round 3: one confirmation per pop, each with its own detours,
		//! cost ~2.6k scene rays per pop under 50 panes over a rock).
		struct CrossingMedia
		{
			std::vector<ConfirmedSheet> confirmed;
			bool decline;
			bool done;
			CrossingMedia() : decline( false ), done( false ) {}

			void Confirm( const IObjectManager* pObjects, const Point3& p, const Vector3& dir, const Scalar nm,
				const IObject* pCrossed )
			{
				if( done ) {
					return;
				}
				done = true;
				const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
				const Point3 pAfter = Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-5 ) * scale ) );
				// The sheet just crossed is left out: an exit through its back face
				// puts pAfter on its front side, outside its medium.
				ConfirmOpenSheetsAt( pObjects, OpenSheetsOf( pObjects ), pAfter, nm, 0, 0, 0, confirmed, decline, pCrossed );
			}

			//! Does open sheet @a pY (on @a pStack) stay past the crossing?
			bool Keeps( const IObject* pY, const IObject* pSheet, const IORStack* pStack ) const
			{
				if( decline ) {
					return false;
				}
				Scalar yIor = -1;
				if( pStack ) {
					for( std::size_t i = pStack->Depth(); i-- > 1; ) {
						if( pStack->ObjectAt( i ) == pY ) {
							yIor = pStack->IORAt( i );
							break;
						}
					}
			}
			const IMaterial* pMat = pY->GetMaterial();
			for( std::size_t c = 0; c < confirmed.size(); ++c ) {
				const ConfirmedSheet& cs = confirmed[c];
				if( cs.pObj == pY ) {
					return true;
				}
				if( cs.pObj == pSheet || !pMat || cs.pObj->GetMaterial() != pMat ) {
					continue;
				}
				if( yIor > 0 && cs.ior != yIor ) {
					continue;
				}
				bool onStack = false;
				for( std::size_t i = 1; pStack && i < pStack->Depth(); ++i ) {
					if( pStack->ObjectAt( i ) == cs.pObj ) {
						onStack = true;
						break;
					}
				}
				if( !onStack ) {
					return true;
				}
			}
			return false;
			}
		};

		//! DL-382 / DL-618: does open sheet @a pY's medium continue past the
		//! crossing at @a p (walk travelling along @a dir, leaving sheet
		//! @a pSheet through its back face while pY is on the stack)?
		//!
		//! The answer is the SEED's at the point just past the crossing: the
		//! same sheet confirmation `SeedFromPoint` runs there
		//! (`ConfirmOpenSheetsAt`, at the walk's wavelength @a nm) holds pY, or
		//! holds a sheet S of pY's material and index that is NOT already on the
		//! walk's stack @a pStack -- the seed keeps one entry for same-material
		//! siblings, so pY standing for S's medium prices the same index; an S
		//! the stack already carries (a same-material sheet pY is nested in)
		//! must not be counted twice (DL-622 review round 3: a stale inner pane
		//! kept alive by its own outer pane left the walk at the inner pane's
		//! index after the outer one was exited).  Nothing depends on where the
		//! walk entered pY and nothing walks along the walk's direction, so a PT
		//! eye walk, a BDPT/VCM light walk and a seed price every point with one
		//! index; the seed's declines (ambiguous siblings) pop pY.  A key that is
		//! not (or no longer) a certified open sheet bounds a volume and is
		//! decided as a closed entry is (`ClosedObjectEnclosesCrossing`).
		inline bool OpenSheetEnclosesCrossing(
			const IObject* pY,
			const IObject* pSheet,
			const IScene* pScene,
			const Point3& p,
			const Vector3& dir,
			const IORStack* pStack = 0,
			const Scalar nm = 0
			)
		{
			if( !pY ) {
				return false;
			}
			if( !pScene || !pScene->GetObjects() ) {
				return ObjectEnclosesCrossing( pY, p, dir );
			}
			const IObjectManager* pObjects = pScene->GetObjects();
			OpenSheetCatalogEntry sheetY;
			if( !FindOpenSheet( pObjects, pY, sheetY ) ) {
				return ClosedObjectEnclosesCrossing( pY, p, dir );
			}
			CrossingMedia media;
			media.Confirm( pObjects, p, dir, nm, pSheet );
			return media.Keeps( pY, pSheet, pStack );
		}

		//! DL-382: resolves an unpushed exit on certified sheet @a pSheet.
		//! Pops top entry Y while it does not contain the front region.
		inline void ResolveUnpushedExit(
			const RayIntersectionGeometric& ri,
			const IObject* pSheet,
			IORStack& dest,
			const Scalar nm = 0
			)
		{
			const Vector3& dir = ri.ray.Dir();
			const Point3& pt = ri.ptIntersection;
			const IScene* pScene = dest.MediumProbeScene();
			CrossingMedia media;	// one sheet confirmation per crossing

			while( const IObject* pY = dest.topObject() ) {
				if( pY == pSheet ) {
					break;
				}
				bool bEncloses = false;
				if( dest.topIsOpenSheet() ) {
					OpenSheetCatalogEntry sheetY;
					if( !pScene || !pScene->GetObjects() ) {
						bEncloses = ObjectEnclosesCrossing( pY, pt, dir );
					} else if( !FindOpenSheet( pScene->GetObjects(), pY, sheetY ) ) {
						bEncloses = ClosedObjectEnclosesCrossing( pY, pt, dir );
					} else {
						media.Confirm( pScene->GetObjects(), pt, dir, nm, pSheet );
						bEncloses = media.Keeps( pY, pSheet, &dest );
					}
				} else {
					bEncloses = ClosedObjectEnclosesCrossing( pY, pt, dir );
				}

				if( bEncloses ) {
					break;
				}

				dest.SetCurrentObject( pY );
				dest.pop();
				if( pSheet ) {
					dest.SetCurrentObject( pSheet );
				}
			}
		}

		//! Result of ResolveOpenSheetCrossing (below).
		struct OpenSheetCrossing
		{
			bool   bEntering;
			Scalar etaFrom;
			Scalar etaTo;
		};

		//! DL-345 / DL-622: one crossing of a PROVABLY OPEN transmissive sheet
		//! (`RayIntersectionGeometric::bProvablyNoInterior`).
		inline OpenSheetCrossing ResolveOpenSheetCrossing(
			const RayIntersectionGeometric& ri,
			const Scalar sheetIOR,
			IORStack& dest,
			const IMaterial* pMat = 0,
			const Scalar nm = 0
			)
		{
			OpenSheetCrossing c;
			const Vector3& d = ri.ray.Dir();
			const bool bFront = Vector3Ops::Dot( d, ri.UnflippedGeomNormal() ) < 0;
			if( bFront ) {
				if( dest.containsCurrent() ) {
					dest.pop();
				}
				c.bEntering = true;
				c.etaFrom = dest.top();
				// The crossing's record, for a companion lane's re-index.  A
				// null @a pMat is resolved by the consumer (which checks the
				// key is a scene object before dereferencing it).
				dest.pushOpenSheet( sheetIOR, IORStack::EntryContext::FromGeometric( ri, pMat, false ) );
				c.etaTo = sheetIOR;
			} else {
				if( dest.containsCurrent() ) {
					dest.pop();
				} else {
					ResolveUnpushedExit( ri, dest.currentObject(), dest, nm );
				}
				c.bEntering = false;
				c.etaFrom = sheetIOR;
				c.etaTo = dest.top();
			}
			return c;
		}

		//! DL-345: true when @a ri struck a provably open sheet from BEHIND
		//! (the exit side under ResolveOpenSheetCrossing's ruling).
		inline bool OpenSheetBackFace( const RayIntersectionGeometric& ri )
		{
			return Vector3Ops::Dot( ri.ray.Dir(), ri.UnflippedGeomNormal() ) >= 0;
		}

		//! The stack a transmissive SPF hands its TRANSMITTED ray.  Without
		//! an open-sheet resolution (@a pOpenSheet null) it is the walk's
		//! stack pushed (entry) or popped (exit), as before DL-345; with one
		//! it is the resolved stack, carrying the crossing's `etaFrom` when
		//! that differs from the walk's own top (see IORStack::crossingEtaFrom).
		//! Caller owns the result.
		inline IORStack* NewTransmittedStack(
			const IORStack& walk,
			const IORStack* pOpenSheet,
			const Scalar openSheetEtaFrom,
			const bool bEntering,
			const Scalar objectIOR
			)
		{
			IORStack* p = new IORStack( pOpenSheet ? *pOpenSheet : walk );
			GlobalLog()->PrintNew( p, __FILE__, __LINE__, "ior stack" );
			if( pOpenSheet ) {
				if( openSheetEtaFrom != walk.top() ) {
					p->SetCrossingEtaFrom( openSheetEtaFrom );
				}
			} else if( bEntering ) {
				p->push( objectIOR );
			} else {
				p->pop();
			}
			return p;
		}
	}
}

#endif
