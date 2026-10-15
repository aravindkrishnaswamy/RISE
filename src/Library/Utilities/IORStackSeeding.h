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
		// E).  One step adds at most one entry, so kMaxProbeSteps entries
		// can never overflow.
		static const std::size_t kMaxProbeEntries = static_cast<std::size_t>( kMaxProbeSteps );

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
		}

		inline void DebugResetProbeTraceCount() {
			Diagnostics::ProbeTraceCounter() = 0;
		}

		inline std::size_t DebugGetProbeTraceCount() {
			return Diagnostics::ProbeTraceCounter();
		}

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

		//! DL-618: one certified open refractive sheet of the scene, with the
		//! geometry the sheet-normal convention and its cheap rejections use.
		struct OpenSheetCatalogEntry
		{
			const IObject* pObj;
			Vector3 normal;			//!< the sheet's representative true normal (see DescribeOpenSheet)
			Point3 p0;				//!< a representative point on the sheet
			Scalar heightAbove;		//!< max over the sheet's bounds of dot(x - p0, normal)
			BoundingBox box;		//!< the sheet's world bounds
			Matrix4 xform;			//!< the object's world transform when described (its pose)
		};

		//! DL-618: describes @a object if it is a certified open refractive
		//! sheet (returns false otherwise).
		//!
		//! The object ALONE is probed from outside its bounds along each
		//! principal axis, toward its centre and four quarter points (15
		//! rays); a hit is classified by `ClassifyMembershipHit`, and a sheet
		//! certifies every hit, so any one sheet hit decides membership.
		//!
		//! The representative NORMAL is that of the least-squares plane through
		//! a 9 x 9 grid of hits along the axis whose hits meet the sheet most
		//! squarely (largest mean |N . axis|), and `p0` their centroid (DL-618
		//! review round 2, P2-1).  Recording the first hit's normal let an X ray
		//! grazing one flank of a displaced sea surface tilt the recorded
		//! normal toward horizontal, so a point under the sea was not seeded,
		//! and which flank won decided the answer.  For a planar sheet every
		//! hit normal is the plane's; for a gently curved one (a sea surface,
		//! a curved open mesh) the plane fit follows its large-scale tilt and
		//! not the local slopes of its relief.  When the hits along that axis
		//! span too thin a band for the fit to be conditioned (1:100
		//! eigenvalue ratio of their spread), the mean hit normal is kept.
		inline bool DescribeOpenSheet( const IObject& object, OpenSheetCatalogEntry& out )
		{
			const BoundingBox box = object.getBoundingBox();
			const Point3 c = box.GetCenter();
			const Vector3 e = box.GetExtents();
			if( !std::isfinite( c.x ) || !std::isfinite( c.y ) || !std::isfinite( c.z ) ||
				!std::isfinite( e.x ) || !std::isfinite( e.y ) || !std::isfinite( e.z ) ) {
				return false;
			}
			const Scalar reach = Vector3Ops::Magnitude( e ) + Scalar( 1 );
			static const Scalar kOff[5][2] = { { 0, 0 }, { 0.25, 0.25 }, { -0.25, 0.25 }, { 0.25, -0.25 }, { -0.25, -0.25 } };
			bool anyHit = false, sheet = false;
			Scalar bestScore = -1;
			int bestAxis = -1;
			Vector3 bestNormal( 0, 0, 0 );
			Point3 bestPoint = c;
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
					hits++;
				}
				if( hits > 0 && sumFacing / hits > bestScore && Vector3Ops::Magnitude( sumN ) > NEARZERO ) {
					bestScore = sumFacing / hits;
					bestAxis = axis;
					bestNormal = Vector3Ops::Normalize( sumN );
					bestPoint = Point3( sumX / hits, sumY / hits, sumZ / hits );
				}
			}
			if( !sheet || bestScore < 0 ) {
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
						const Scalar q[3] = { ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z };
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
			out.pObj = &object;
			out.normal = bestNormal;
			out.p0 = bestPoint;
			out.box = box;
			out.xform = object.GetFinalTransformMatrix();
			out.heightAbove = -RISE_INFINITY;
			for( int corner = 0; corner < 8; corner++ ) {
				const Point3 q( ( corner & 1 ) ? box.ur.x : box.ll.x, ( corner & 2 ) ? box.ur.y : box.ll.y, ( corner & 4 ) ? box.ur.z : box.ll.z );
				out.heightAbove = std::max( out.heightAbove, Vector3Ops::Dot( Vector3Ops::mkVector3( q, bestPoint ), bestNormal ) );
			}
			return true;
		}

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
			//! Re-posed descriptions of listed sheets (motion blur), one per
			//! sheet object, each valid for the pose it records.
			std::vector<OpenSheetCatalogEntry> posed;

			OpenSheetCatalog() : pObjects( 0 ), generation( 0 ), epoch( 0 ), itemCount( 0 ) {}
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
			catalog.posed.clear();

			struct Collect : IEnumCallback<IObject>
			{
				std::vector<OpenSheetCatalogEntry>* out;
				bool operator()( const IObject& object ) override
				{
					if( !object.IsWorldVisible() || !MaterialMayRefract( object.GetMaterial() ) ) {
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
			pObjects->EnumerateObjects( collect );
			return catalog.sheets;
		}

		//! DL-618 round 2 (P3-b): @a entry at the calling thread's pose.
		//!
		//! Round 3 (P2-1): only a sheet whose OWN pose differs from the one it
		//! was described at is re-described -- a time-indexed pass is entered
		//! for any keyframed frame (a moving camera alone sets the thread's
		//! slot table), and re-describing every sheet on every seed cost ~100
		//! object rays per sheet per seed.  A re-posed description is kept
		//! per thread, keyed on the sheet's world transform, so a pixel
		//! sample's seeds (hero, companions, subpath roots, crossings) share
		//! it and a static sheet costs one transform read and compare.
		inline OpenSheetCatalogEntry PosedOpenSheet( const OpenSheetCatalogEntry& entry )
		{
			if( !TimeIndexed::tlsSlots ) {
				return entry;
			}
			const Matrix4 now = entry.pObj->GetFinalTransformMatrix();
			if( std::memcmp( &now, &entry.xform, sizeof( Matrix4 ) ) == 0 ) {
				return entry;
			}
			std::vector<OpenSheetCatalogEntry>& posed = ThreadOpenSheetCatalog().posed;
			for( std::size_t i = 0; i < posed.size(); i++ ) {
				if( posed[i].pObj == entry.pObj ) {
					if( std::memcmp( &now, &posed[i].xform, sizeof( Matrix4 ) ) != 0 ) {
						OpenSheetCatalogEntry fresh;
						if( !DescribeOpenSheet( *entry.pObj, fresh ) ) {
							return entry;
						}
						posed[i] = fresh;
					}
					return posed[i];
				}
			}
			OpenSheetCatalogEntry fresh;
			if( !DescribeOpenSheet( *entry.pObj, fresh ) ) {
				return entry;
			}
			posed.push_back( fresh );
			return fresh;
		}

		//! DL-618: the catalog's description of @a pObj, or a fresh one when
		//! it is not listed.  False when @a pObj is not an open sheet.
		inline bool FindOpenSheet( const IObjectManager* pObjects, const IObject* pObj, OpenSheetCatalogEntry& out )
		{
			if( pObjects ) {
				const std::vector<OpenSheetCatalogEntry>& sheets = OpenSheetsOf( pObjects );
				for( std::size_t i = 0; i < sheets.size(); i++ ) {
					if( sheets[i].pObj == pObj ) {
						out = PosedOpenSheet( sheets[i] );
						return true;
					}
				}
			}
			return pObj && DescribeOpenSheet( *pObj, out );
		}

		static const std::size_t kMaxSeededOpenSheets = 8;

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
			const Scalar nm = 0
			)
		{
			Diagnostics::ProbeTraceCounter()++;
			std::size_t count = 0;
			const Scalar kSeedEps = Scalar( 1e-4 );
			Ray probe( pos, dir );

			for( int step = 0; step < kMaxProbeSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					break;
				}

				// Side-of-surface decision: use the GEOMETRIC normal,
				// not the shading normal — the question is "is the seed
				// point physically inside this object", which is a
				// topology query about the actual surface.  A bump-mapped
				// or normal-mapped enclosure boundary at a grazing probe
				// angle can flip the shading-normal cosN sign while the
				// geometric crossing is unambiguous; using shading there
				// silently leaves the seed stack empty (PBRT 4e §10.1.1).
				//
				// DL-46 double-sided-mesh follow-up: `vGeomNormal` is NOT
				// unconditionally the true surface-facing normal.  A
				// double-sided triangle mesh (TriangleMeshGeometry{,
				// Indexed}::IntersectRay), and BezierPatchGeometry /
				// ClippedPlaneGeometry on a back-face hit, flip it to face
				// whichever side the probe struck, recording that in
				// `bGeomNormalOrientedToRay`; on such a surface
				// `Dot(vGeomNormal, probe.Dir())` is ALWAYS negative, on
				// both a true entry and a true exit — `cosN > 0` (exit)
				// never fires, parity only ever decrements, and the object
				// is never seeded.  Recover the TRUE geometric normal with
				// the documented un-flip (RayIntersectionGeometric.h)
				// before dotting.
				//
				// P2-4 (review round 3): that recovery is only meaningful
				// when the reported normal is a static property of the
				// surface.  `HairGeometry` reports a RAY-DERIVED normal
				// (and sets `bGeomNormalOrientedToRay` unconditionally),
				// so un-flipping it gives "always away from the ray" =
				// "always an exit" at every strand the probe crosses — a
				// hair curtain between the seed and infinity would tally
				// arbitrarily large positive parity.  A 1-D curve has no
				// interior to be inside, so skip such a hit entirely
				// rather than tally a meaningless crossing.  (Harmless
				// today only because HairMaterial reports no
				// SpecularInfo; this guard makes it structural.)
				const Scalar cosN = ri.geometric.TrueGeomFacing( probe.Dir() );

				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( ri, probe.Dir(), nm, info );

				// Closed-parity tally.  Track refractive materials (own
				// numeric IOR) AND hasInterior-only stateful materials
				// (DL-46, no distinct IOR, just membership tracking) —
				// pure reflectors (mirrors) and Lambertian surfaces report
				// neither and have no "interior" the ray travels through.
				// A certified open sheet has no interior either (DL-618).
				if( membership == kMembershipTransparent )
				{
					// Find or create per-object entry.  Linear scan is
					// fine — at most kMaxProbeEntries (32) entries.
					ProbeEntry* e = 0;
					for( std::size_t d = 0; d < count; d++ ) {
						if( out[d].pObj == ri.pObject ) {
							e = &out[d];
							break;
						}
					}
					if( !e && count < kMaxProbeEntries ) {
						e = &out[count++];
						e->pObj = ri.pObject;
						e->ior = info.ior;
						e->parity = 0;
						e->firstExitStep = -1;
						e->firstEntryStep = -1;
						e->repushParentIor = !info.canRefract;
					}
					if( e )
					{
						// +1 per exit, -1 per entry.  Positive net =
						// seed is inside this object.
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

				// Step past the hit to find the next one.
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

		//! DL-618: the sheet-normal confirmation of the convention above.
		//! Walks the ray from @a pos along @a normal (the candidate sheet's
		//! own true normal) and returns true iff the first non-transparent
		//! event is @a pSheet's BACK face.  When @a passedSheets /
		//! @a crossedClosed are given, records (up to @a cap each; more sets
		//! @a overflow) the other open sheets whose back faces and the closed
		//! media whose boundaries the ray crossed before reaching @a pSheet,
		//! for nesting order.
		//!
		//! The seed point's OWN surface does not block (round 2, P2-3): a
		//! BDPT light root lies on its luminaire, and from a point on the
		//! lower half of an opaque sphere luminaire the normal ray meets the
		//! sphere's upper half from inside.  A hit closer than a self-hit
		//! tolerance is skipped, and so is the ray's FIRST opaque event when
		//! it is an exit (back face) from an object whose surface passes
		//! through `pos` (an object-only ray from just inside, back toward
		//! `pos`, meets it within the tolerance) -- the ray started inside
		//! that body.  A later event, even on the same object, blocks.  A
		//! point strictly inside an opaque closed object (a camera in a
		//! room) is not on its surface, so the room still blocks.
		inline bool OpenSheetNormalRayReaches(
			const IObjectManager* pObjects,
			const Point3& pos,
			const Vector3& normal,
			const IObject* pSheet,
			const Scalar nm,
			const IObject** passedSheets = 0,
			std::size_t* passedCount = 0,
			const IObject** crossedClosed = 0,
			std::size_t* crossedCount = 0,
			const std::size_t cap = 0,
			Scalar* sheetIor = 0,
			bool* overflow = 0
			)
		{
			Diagnostics::ProbeTraceCounter()++;
			if( passedCount ) *passedCount = 0;
			if( crossedCount ) *crossedCount = 0;
			const Scalar kSeedEps = Scalar( 1e-4 );
			const Scalar scale = Scalar( 1 ) + std::fabs( pos.x ) + std::fabs( pos.y ) + std::fabs( pos.z );
			const Scalar selfTol = Scalar( 1e-5 ) * scale;
			bool ownChecked = false;
			Ray probe( pos, normal );
			for( int step = 0; step < kMaxProbeSteps; step++ )
			{
				RayIntersection ri( probe, nullRasterizerState );
				pObjects->IntersectRay( ri, true, true, false );
				if( !ri.geometric.bHit ) {
					return false;
				}
				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( ri, probe.Dir(), nm, info );
				if( membership != kMembershipSkip && ri.pObject == pSheet ) {
					if( sheetIor ) *sheetIor = info.ior;
					return membership == kMembershipSheetBack;
				}
				if( membership == kMembershipBlock ) {
					// Only the ray's FIRST opaque event can be the seed's own
					// body, and only if it is an EXIT (a back face): the ray
					// then started inside the solid whose surface `pos` lies
					// on.  Anything after that -- including a later ENTRY into
					// the same object (a thick-walled room's roof above a light
					// flush with its floor, round 3 P3-1) -- blocks as usual.
					bool own = step == 0 && ri.geometric.range < selfTol;
					if( !own && !ownChecked && ri.pObject && ri.geometric.TrueGeomFacing( probe.Dir() ) > 0 ) {
						RayIntersection back( Ray( Point3Ops::mkPoint3( pos, normal * selfTol ), -normal ), nullRasterizerState );
						ri.pObject->IntersectRay( back, Scalar( 2 ) * selfTol, true, true, false );
						own = back.geometric.bHit && back.geometric.range <= Scalar( 2 ) * selfTol;
					}
					ownChecked = true;
					if( !own ) {
						return false;
					}
				} else if( membership == kMembershipSheetFront ) {
					return false;
				} else if( membership == kMembershipSheetBack && passedSheets && passedCount ) {
					if( *passedCount < cap ) {
						passedSheets[( *passedCount )++] = ri.pObject;
					} else if( overflow ) {
						*overflow = true;
					}
				} else if( membership == kMembershipTransparent && crossedClosed && crossedCount ) {
					bool seen = false;
					for( std::size_t i = 0; i < *crossedCount; i++ ) {
						seen = seen || crossedClosed[i] == ri.pObject;
					}
					if( !seen ) {
						if( *crossedCount < cap ) {
							crossedClosed[( *crossedCount )++] = ri.pObject;
						} else if( overflow ) {
							*overflow = true;
						}
					}
				}
				probe = Ray( ri.geometric.ptIntersection, probe.Dir() );
				probe.Advance( kSeedEps );
			}
			return false;
		}

		//! DL-618: does sheet @a sheet's medium hold @a pos under the
		//! sheet-normal convention?  Cheap rejections first (round 2, P2-4),
		//! each sound for the walk it saves: `pos` above every point of the
		//! sheet along its normal; the normal ray missing the sheet's bounds;
		//! the sheet ALONE not met, or met on its front face, first along the
		//! normal ray (the scene walk returns at the sheet's first hit, so the
		//! object-only first hit is the one it would judge).  Then the scene
		//! walk, `OpenSheetNormalRayReaches`.
		inline bool OpenSheetHoldsPoint(
			const OpenSheetCatalogEntry& sheet,
			const IObjectManager* pObjects,
			const Point3& pos,
			const Scalar nm,
			const IObject** passedSheets = 0,
			std::size_t* passedCount = 0,
			const IObject** crossedClosed = 0,
			std::size_t* crossedCount = 0,
			const std::size_t cap = 0,
			Scalar* sheetIor = 0,
			bool* overflow = 0
			)
		{
			const Vector3& n = sheet.normal;
			if( Vector3Ops::Dot( Vector3Ops::mkVector3( pos, sheet.p0 ), n ) >= sheet.heightAbove ) {
				return false;
			}
			// Slab test of the normal ray against the sheet's bounds.
			{
				const Scalar o[3] = { pos.x, pos.y, pos.z };
				const Scalar d[3] = { n.x, n.y, n.z };
				const Scalar lo[3] = { sheet.box.ll.x, sheet.box.ll.y, sheet.box.ll.z };
				const Scalar hi[3] = { sheet.box.ur.x, sheet.box.ur.y, sheet.box.ur.z };
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
			}
			{
				RayIntersection alone( Ray( pos, n ), nullRasterizerState );
				sheet.pObj->IntersectRay( alone, RISE_INFINITY, true, true, false );
				if( !alone.geometric.bHit || !alone.geometric.HasTrueGeomSide() ||
					alone.geometric.TrueGeomFacing( n ) <= 0 ) {
					return false;
				}
			}
			return OpenSheetNormalRayReaches( pObjects, pos, n, sheet.pObj, nm,
				passedSheets, passedCount, crossedClosed, crossedCount, cap, sheetIor, overflow );
		}

		// DL-76: a single axis's forward+reverse probe PAIR is only a
		// containment test for a surface that is closed ALONG THAT AXIS.
		// A single `Object` built from two disjoint open pieces that
		// straddle the seed point on opposite sides of one axis, each
		// piece's normal facing away from the seed (e.g. a two-blade
		// louvre mesh with one blade above and one below along Z, both
		// part of the same triangle mesh `Object`), independently
		// registers an "exit" for that ONE `pObj` in both the forward AND
		// the reverse probe along that axis -- the per-object identity
		// check in `SeedFromPoint` below cannot distinguish that from a
		// genuine enclosure, because both probes really do see the same
		// object with positive parity.  Extending the vote to a SECOND,
		// non-coplanar axis rejects that specific counterexample: the two
		// blades lie in the plane perpendicular to the axis they straddle,
		// so a probe along either OTHER principal axis is coplanar with
		// both blades and (barring a coincidental edge-on hit) crosses
		// neither -- that axis then reports zero parity for the object,
		// failing the unanimity vote.  A genuinely closed manifold agrees
		// along every direction by construction, so this is a no-op for
		// the enclosures the mechanism targets.
		//
		// PERF (2026-09-14, review follow-up): the X/Y probe pairs used
		// to be re-traced by a per-candidate `IsConfirmedAlongAxis` call
		// INSIDE `SeedFromPoint`'s confirmation loop below -- but the
		// probe only depends on `pos` and the fixed axis, never on which
		// candidate object is being checked, so re-tracing it per
		// candidate was pure waste: up to 8 nested candidates x 4 probes
		// (X fwd/rev, Y fwd/rev) = up to 32 extra `TallyProbe` calls per
		// `SeedFromPoint`.  The X/Y pairs are now traced ONCE, exactly
		// like the Z-reverse probe already was, and candidates are
		// checked by looking their `pObj` up in the resulting arrays.
		// `HasPositiveParity` is that lookup -- a linear scan of an
		// already-traced array, no ray tracing at all.
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

		/// Populate `stack` with the dielectric objects that physically
		/// contain `pos`, so that subsequent scatters at the first
		/// enclosing boundary see bFromInside==true, and (DL-618) with the
		/// certified open sheets whose medium contains `pos` under the
		/// sheet-normal convention documented above `ClassifyMembershipHit`.
		///
		/// Safe to call with a camera position even when the camera is
		/// not inside anything — the probe will simply find no exit
		/// hits and leave the stack as its caller initialized it.
		///
		/// @a seedOpenSheets false seeds closed media only (round 2, P2-4:
		/// SMSDomainReplay::Capture reads only the closed membership and
		/// need not pay for the sheets).
		inline void SeedFromPoint(
			IORStack& stack,
			const Point3& pos,
			const IScene& scene,
            const Scalar nm = 0,
			const bool seedOpenSheets = true
			)
		{
			stack.SetMediumProbeScene( &scene );

			// Emergency off-switch for perf regression triage — set
			// RISE_DISABLE_IOR_STACK_SEEDING=1 in the environment to
			// restore the legacy empty-stack behaviour.  Kept as an
			// env check (not a scene param) so a single toggle covers
			// every BDPT/VCM invocation without touching scene files.
			static const bool sDisabled =
				( std::getenv( "RISE_DISABLE_IOR_STACK_SEEDING" ) != 0 );
			if( sDisabled ) {
				return;
			}

			const IObjectManager* pObjects = scene.GetObjects();
			if( !pObjects ) {
				return;
			}

			// +Z is arbitrary; any fixed direction avoids per-seed RNG
			// and keeps results deterministic across threads.  Every
			// probe below is a closed-parity walk; DL-618 open sheets are
			// seeded separately from the scene's catalog of them.
			ProbeEntry containing[kMaxProbeEntries];
			const std::size_t containingCount =
				TallyProbe( pObjects, pos, Vector3( 0, 0, 1 ), containing, nm );

			// P2-4 (review round 3): a single probe's parity is only a
			// containment test for a CLOSED surface.  Every
			// `translucent_material` object is now a probe participant
			// (DL-46), and translucent is exactly the material authors put
			// on OPEN sheets -- a leaf, a curtain, a lampshade panel, a
			// paper card.  An open single-sided card sitting above the
			// seed with its normal pointing up registers one "exit" and no
			// entry, so parity reads +1 and the seed is falsely declared
			// inside it: the camera's very first hit then runs
			// TranslucentSPF's EXIT branch (Beer extinction + a pop of an
			// IOR that was never pushed) instead of the entry branch.
			//
			// Rule: require positive parity along the probe AND along its
			// REVERSE.  For a closed manifold both directions agree by
			// construction (the seed is inside or it is not), so this is a
			// no-op for every case the mechanism was built for; for an
			// open surface the two disagree (the reverse probe never meets
			// it) and the object is rejected.  (A CERTIFIED open REFRACTIVE
			// sheet is no longer a parity participant at all: its medium
			// is decided by DL-618's sheet-normal convention below, under
			// which a refractive sheet above the seed facing up DOES hold
			// the seed in its medium when the seed sees it along its
			// normal -- a translucent card, which does not refract, keeps
			// this rule.)
			//
			// DL-76 (this rule's own documented residual, closed below):
			// a single-axis pair alone still cannot tell a genuine
			// enclosure from a SINGLE Object built out of several open
			// pieces that straddle the seed on opposite sides of just
			// that one axis, each piece's normal facing away from the
			// seed (e.g. a two-blade louvre mesh, one blade above and one
			// below along Z, both part of the same triangle mesh Object)
			// -- both probes independently see an "exit" for that one
			// pObj, so the same-object confirmation above accepts it.
			// The vote below extends confirmation to the two OTHER
			// principal axes (X, Y) for whichever objects the Z pair
			// already accepts: a probe along an axis coplanar with a flat
			// pair of blades crosses neither, so the false positive is
			// caught by requiring ALL THREE axes to agree.  This is a
			// closed manifold's natural behaviour (it agrees along every
			// direction, so the extra votes are a no-op there); it is a
			// bounded, cheap improvement — not a general winding-number /
			// solid-angle containment test — so an adversarial Object
			// deliberately built from open pieces straddling the seed
			// along all three principal axes at once would still fool
			// it (accepted FALSE-POSITIVE limit).
			//
			// There is no mirror-image false-negative from tunnel
			// alignment: a point strictly inside the solid bounded by a
			// closed orientable manifold has ODD (net positive) crossing
			// parity along every generic probe direction, regardless of
			// genus, tunnels, or convexity — the ray starts inside and
			// ends at infinity (outside a bounded solid), so it must
			// cross the boundary an odd number of times no matter which
			// axis it follows, including one that happens to run through
			// a hole.  (Verified directly: a torus with its hole aligned
			// to Y, probed from a seed point on the tube itself, reads
			// positive parity on all six +-X/+-Y/+-Z probes.)  The one
			// real residual is a probe that grazes the surface exactly
			// tangentially or threads a face/edge boundary at the sampled
			// precision, a pre-existing degenerate-alignment hazard.
			// See docs/DEBT_LEDGER.md DL-76 and
			// docs/SUBMERGED_CAMERA_IOR_SEEDING.md for the accepted
			// scope.
			//
			// COST (DL-618 review P2-1): exactly the pre-DL-618 budget for
			// closed media -- the reverse probe only when +Z found a
			// candidate, the X/Y pairs only when one is Z-confirmed -- plus
			// one normal-ray walk per open refractive sheet in the scene
			// (`OpenSheetsOf`), none in a scene without one.
			ProbeEntry reverse[kMaxProbeEntries];
			std::size_t reverseCount = 0;
			bool anyCandidate = false;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity > 0 ) { anyCandidate = true; break; }
			}
			if( anyCandidate ) {
				reverseCount = TallyProbe( pObjects, pos, Vector3( 0, 0, -1 ), reverse, nm );
			}

			// DL-76 perf fix (2026-09-14): trace the X and Y confirmation
			// pairs ONCE per SeedFromPoint call, and only when at least one
			// object is already Z-confirmed.
			ProbeEntry xForward[kMaxProbeEntries], xReverse[kMaxProbeEntries];
			ProbeEntry yForward[kMaxProbeEntries], yReverse[kMaxProbeEntries];
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
				xForwardCount = TallyProbe( pObjects, pos, Vector3( 1, 0, 0 ), xForward, nm );
				xReverseCount = TallyProbe( pObjects, pos, Vector3( -1, 0, 0 ), xReverse, nm );
				yForwardCount = TallyProbe( pObjects, pos, Vector3( 0, 1, 0 ), yForward, nm );
				yReverseCount = TallyProbe( pObjects, pos, Vector3( 0, -1, 0 ), yReverse, nm );
			}

			// Closed containing objects (parity > 0 on all six probes),
			// stack ORDER: bottom = outermost, top = innermost.  Along an
			// outward-going probe, the FIRST exit of an object is at its
			// INNERMOST surface, the LAST exit at its OUTERMOST.  Objects
			// with smaller firstExitStep are inner — push them last.
			// Insertion-sort to OUTERMOST-FIRST (largest firstExitStep
			// first); buffer is at most kMaxProbeEntries entries.
			//
			// Objects with parity == 0 (probe passed through) and
			// parity < 0 (unbalanced entries — only possible from
			// pathological geometry hitting the step cap) are skipped.
			ProbeEntry* ordered[kMaxProbeEntries];
			std::size_t orderedCount = 0;
			for( std::size_t i = 0; i < containingCount; i++ ) {
				if( containing[i].parity <= 0 ) {
					continue;
				}
				// P2-4: confirm against the reverse probe (see above).
				if( !HasPositiveParity( reverse, reverseCount, containing[i].pObj ) ) {
					continue;
				}
				// DL-76: require the two other principal axes to confirm
				// this exact object too.
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

			// DL-618: confirm each of the scene's open sheets by its normal ray
			// (see the convention above `ClassifyMembershipHit`), recording
			// what the ray passed so the confirmed sheets can be ordered.  More
			// than kMaxSeededOpenSheets confirmed sheets (or a nesting record
			// that overflows) cannot be ordered soundly, so the open sheets are
			// declined rather than truncated (round 2, P3-a).
			struct ConfirmedSheet
			{
				const IObject* pObj;
				Scalar ior;
				const IObject* passed[kMaxSeededOpenSheets];
				std::size_t passedCount;
				const IObject* crossed[kMaxProbeEntries];
				std::size_t crossedCount;
				std::size_t closedInner;	// confirmed closed media inner to this sheet
				bool keep;
			};
			ConfirmedSheet confirmed[kMaxSeededOpenSheets];
			std::size_t confirmedCount = 0;
			bool declineSheets = !seedOpenSheets;
			const std::vector<OpenSheetCatalogEntry>* pSheets = seedOpenSheets ? &OpenSheetsOf( pObjects ) : 0;
			for( std::size_t i = 0; pSheets && i < pSheets->size() && !declineSheets; i++ ) {
				ConfirmedSheet scratch;
				ConfirmedSheet& c = confirmedCount < kMaxSeededOpenSheets ? confirmed[confirmedCount] : scratch;
				const OpenSheetCatalogEntry sheet = PosedOpenSheet( ( *pSheets )[i] );
				c.pObj = sheet.pObj;
				c.ior = 0;
				bool overflow = false;
				if( !OpenSheetHoldsPoint( sheet, pObjects, pos, nm,
						c.passed, &c.passedCount, c.crossed, &c.crossedCount, kMaxSeededOpenSheets, &c.ior, &overflow ) ) {
					continue;
				}
				if( overflow || confirmedCount >= kMaxSeededOpenSheets ) {
					declineSheets = true;
					break;
				}
				c.closedInner = 0;
				for( std::size_t k = 0; k < c.crossedCount; k++ ) {
					for( std::size_t m = 0; m < orderedCount; m++ ) {
						if( ordered[m]->pObj == c.crossed[k] ) {
							c.closedInner++;
							break;
						}
					}
				}
				c.keep = true;
				confirmedCount++;
			}

			// Pairwise relations: `a` nested inside `b` when `a` is on
			// `b`'s normal ray before `b`.  Siblings (neither) of one
			// material keep the nearer-proposed one; siblings of different
			// materials, and mutual nesting, decline every sheet.
			auto passes = []( const ConfirmedSheet& on, const IObject* pObj ) {
				for( std::size_t k = 0; k < on.passedCount; k++ ) {
					if( on.passed[k] == pObj ) return true;
				}
				return false;
			};
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

			// Merge OUTERMOST-FIRST.  A sheet with `closedInner` = k sits
			// just outside the k innermost confirmed closed media; among
			// sheets, the one nested inside another goes after it.  Sheets
			// are sorted by (slot = orderedCount - closedInner, nesting),
			// then each closed slot s is preceded by the sheets of slot s.
			const ConfirmedSheet* sheetOrder[kMaxSeededOpenSheets];
			std::size_t sheetOrderCount = 0;
			if( !declineSheets ) {
				for( std::size_t i = 0; i < confirmedCount; i++ ) {
					if( !confirmed[i].keep ) {
						continue;
					}
					const ConfirmedSheet* s = &confirmed[i];
					const std::size_t slot = orderedCount - s->closedInner;
					std::size_t j = sheetOrderCount;
					while( j > 0 ) {
						const ConfirmedSheet* t = sheetOrder[j-1];
						const std::size_t tSlot = orderedCount - t->closedInner;
						// `s` goes before `t` when its slot is lower, or the
						// same slot with `t` nested inside `s`.
						const bool sBeforeT = slot < tSlot ||
							( slot == tSlot && passes( *s, t->pObj ) );
						if( !sBeforeT ) {
							break;
						}
						sheetOrder[j] = sheetOrder[j-1];
						j--;
					}
					sheetOrder[j] = s;
					sheetOrderCount++;
				}
			}

			std::size_t nextSheet = 0;
			for( std::size_t i = 0; i <= orderedCount; i++ ) {
				while( nextSheet < sheetOrderCount &&
					orderedCount - sheetOrder[nextSheet]->closedInner <= i ) {
					stack.SetCurrentObject( sheetOrder[nextSheet]->pObj );
					stack.pushOpenSheet( sheetOrder[nextSheet]->ior );
					nextSheet++;
				}
				if( i == orderedCount ) {
					break;
				}
				stack.SetCurrentObject( ordered[i]->pObj );
				// DL-46: a hasInterior-only entry (e.g. TranslucentSPF) has
				// no distinct IOR of its own -- re-push whatever is
				// CURRENTLY on top (set by the previous, more-outer push,
				// or the stack's initial environment IOR if this is the
				// outermost entry), matching Scatter()'s own
				// `push(ior_stack.top())`.  An ordinary canRefract medium
				// pushes its own captured `ior` as before.
				stack.push( ordered[i]->repushParentIor ? stack.top() : ordered[i]->ior );
			}

			// DL-09: every entry above was recorded at its probe's FIRST
			// SURFACE HIT -- a boundary point that can be arbitrarily far
			// from `pos`.  For a constant-index medium that is the same
			// number; for a medium whose `ior` is a world-position field it
			// is not, and the walk that starts here must start from the
			// index AT the seed point, or its first interior-segment factor
			// (GradedIndexMedium::Advance) is priced from the wrong end.
			// Only the innermost medium matters: a deeper entry is re-read
			// only after the walk pops back into it, and the next Advance
			// then telescopes from whatever it holds.  No-op unless the
			// innermost medium is graded.
			GradedIndexMedium::RecordAt( stack, pos );
		}

		//! DL-618 (round 3, P3-c): the stack an HWSS COMPANION lane delegates
		//! with when the hero walk hands its first segment (from @a origin to
		//! the hit) to per-wavelength walks.  The lanes used to be seeded at
		//! the segment's MIDPOINT while the hero kept a stack seeded at (or
		//! carried to) @a origin; for an open sheet the two points can lie on
		//! opposite sides of its normal-prism boundary (or an occluder's
		//! shadow along its normal), so hero and companions disagreed on the
		//! medium.  Same rule, same point: a lane seeded at @a origin whose
		//! MEMBERSHIP (objects and open-sheet flags, not the possibly
		//! dispersive indices) equals @a hero's is used.  Otherwise the hero's
		//! stack is not a seed at @a origin (it carries walk history, or
		//! @a origin lies on a surface, where containment is ambiguous) and
		//! the lane falls back to the midpoint seed as before.
		//!
		//! @a tryOrigin false skips the origin attempt where @a origin is a
		//! surface vertex of the walk (mid-path; round 3 P3-2), where it
		//! would rarely reproduce the hero's history-carried stack and only
		//! cost a second seed.
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

		//! DL-345: does object @a pY ENCLOSE the crossing at @a p (the walk
		//! travelling along @a dir)?  One ray against ONE object: an EXIT
		//! hit on Y ahead means the walk is still inside Y after the
		//! crossing; a miss, an ENTRY hit, or a surface with no true side
		//! (hair) means Y is not around it.  For a closed, outward-wound Y
		//! the first hit from inside is always an exit, concave or not.
		//! The SMS seed walk's unpushed-exit rule (DL-290 section 11) is the
		//! same probe.
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
		//! convention (see above `ClassifyMembershipHit`)?  Keyed on the
		//! OBJECT, with the catalog's description of it -- the same normal
		//! and the same test `SeedFromPoint` uses (round 2, P2-2: reading the
		//! normal from the nearest axis hit instead disagreed with the seed
		//! for a narrow tilted strip, which no axis ray from the point meets).
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

		//! DL-382 / DL-618: does open sheet @a pY's medium continue past the
		//! crossing at @a p (walk travelling along @a dir, leaving sheet
		//! @a pSheet through its back face while pY is the stack top)?
		//!
		//! First, the sibling rule (DL-382): along @a dir, the first hit on
		//! pY itself or on another CERTIFIED OPEN sheet made of pY's
		//! material (@a pSheet and hits with no true side skipped) decides
		//! it -- met from its back, the walk is still between pY's faces.
		//! Only open sheets are siblings: a closed object that happens to share
		//! the material is not one of pY's faces and is walked through, like
		//! every other trackable medium.  The search walks THROUGH opaque
		//! surfaces (round 2, P2-5): a walk legitimately in a sea's medium
		//! that leaves an underwater pane beneath a rock overhang is still in
		//! the sea, which the sea's back face beyond the rock shows; only a
		//! search that MISSES falls back to the convention below.
		//!
		//! A walk leaving through a face of pY's own material (@a pSheet is
		//! then pY's sibling) with no further face of it ahead has left pY's
		//! region: false, without the fallback below.
		//!
		//! Otherwise, when no sibling decided it (e.g. an oblique exit inside a
		//! lone sheet's back half-space, where no face of pY lies ahead),
		//! the point just past the crossing is tested against pY's medium
		//! with the SAME sheet-normal convention seeding uses
		//! (`PointInOpenSheetMedium`), keyed on the object pY -- so a walk
		//! and a seed agree on where pY's medium is.  @a pSheet is NOT
		//! skipped there: if it is the closing face of a two-sheet slab
		//! entered through pY, pY's normal ray from past the crossing meets
		//! pSheet's FRONT face and is blocked.
		inline bool OpenSheetEnclosesCrossing(
			const IObject* pY,
			const IObject* pSheet,
			const IScene* pScene,
			const Point3& p,
			const Vector3& dir
			)
		{
			if( !pY ) {
				return false;
			}
			if( !pScene || !pScene->GetObjects() ) {
				return ObjectEnclosesCrossing( pY, p, dir );
			}
			const IObjectManager* pObjects = pScene->GetObjects();
			const IMaterial* pMat = pY->GetMaterial();
			const Scalar scale = Scalar( 1 ) + std::fabs( p.x ) + std::fabs( p.y ) + std::fabs( p.z );
			Ray probe( Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-7 ) * scale ) ), dir );
			for( int step = 0; step < 64; ++step ) {
				RayIntersection pri( probe, nullRasterizerState );
				pObjects->IntersectRay( pri, true, true, false );
				if( !pri.geometric.bHit ) {
					break;
				}
				if( !pri.geometric.HasTrueGeomSide() || pri.pObject == pSheet ) {
					probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
					continue;
				}
				if( pri.pObject == pY ) {
					return pri.geometric.TrueGeomFacing( dir ) >= 0;
				}
				SpecularInfo info;
				const MembershipHit membership = ClassifyMembershipHit( pri, dir, 0, info );
				const bool sibling = ( membership == kMembershipSheetBack || membership == kMembershipSheetFront ) &&
					pMat && pri.pMaterial == pMat;
				if( sibling ) {
					return membership == kMembershipSheetBack;
				}
				probe.origin = Point3Ops::mkPoint3( pri.geometric.ptIntersection, dir * ( Scalar( 1e-6 ) * scale ) );
			}

			// Leaving through a face of pY's OWN material with no further face
			// of it ahead is leaving pY's region (a two-sheet slab's far
			// sheet): decided without the fallback's extra walk, the cost the
			// slab exit had before DL-618 (review P2-1).
			if( pSheet && pMat && pSheet->GetMaterial() == pMat ) {
				return false;
			}
			const Point3 pAfter = Point3Ops::mkPoint3( p, dir * ( Scalar( 1e-5 ) * scale ) );
			return PointInOpenSheetMedium( pY, pObjects, pAfter );
		}

		//! DL-382: resolves an unpushed exit on certified sheet @a pSheet.
		//! Pops top entry Y while it does not contain the front region.
		inline void ResolveUnpushedExit(
			const RayIntersectionGeometric& ri,
			const IObject* pSheet,
			IORStack& dest
			)
		{
			const Vector3& dir = ri.ray.Dir();
			const Point3& pt = ri.ptIntersection;
			const IScene* pScene = dest.MediumProbeScene();

			while( const IObject* pY = dest.topObject() ) {
				if( pY == pSheet ) {
					break;
				}
				bool bEncloses = false;
				if( dest.topIsOpenSheet() ) {
					bEncloses = OpenSheetEnclosesCrossing( pY, pSheet, pScene, pt, dir );
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

		//! DL-345: one crossing of a PROVABLY OPEN transmissive sheet
		//! (`RayIntersectionGeometric::bProvablyNoInterior`).
		//!
		//! THE RULING.  An open sheet is an interface whose FRONT side (the
		//! side its TRUE geometric normal points to) is the surrounding
		//! medium and whose BACK side is the sheet's material.  A crossing
		//! is decided by its FACE, never by the IOR stack: front -> back
		//! ENTERS (`etaFrom` = the walk's medium, `etaTo` = the sheet's
		//! index), back -> front EXITS (`etaFrom` = the sheet's index,
		//! `etaTo` = the medium beyond).  This is the only rule under which
		//! a walk and its reverse refract the same way at every sheet, so
		//! the eye walk (PT, BDPT/VCM eye subpaths), the light walk
		//! (BDPT/VCM light subpaths, photons) and SMS's seed walk all
		//! sample ONE path space.  The stack-based rule it replaces for
		//! these hits ("entering iff not already contained") bent the eye
		//! walk at the second sheet of a two-sheet slab it crossed and the
		//! light walk at the other (up to 3x between integrators).
		//!
		//! The stack is kept consistent where it can be:
		//!  - front hit while the sheet is already on the stack (the walk
		//!    left the back region around the sheet's edge): the stale
		//!    entry is dropped, then the sheet is pushed;
		//!  - back hit with the sheet on the stack: popped;
		//!  - back hit WITHOUT it (an "unpushed exit"): the stack top Y is
		//!    either a sibling sheet the walk entered a slab through
		//!    (popped: leaving the slab leaves Y's entry) or a medium that
		//!    encloses the crossing (kept) -- told apart by
		//!    ResolveUnpushedExit (DL-382).
		//!    With no sibling, the walk reached the sheet's back WITHOUT
		//!    crossing it (a receiver under a lone sheet, seen directly):
		//!    the crossing still refracts from the sheet's index, and the
		//!    caller records that on the scattered stack
		//!    (IORStack::SetCrossingEtaFrom) so RadianceEtaScale prices the
		//!    refraction actually performed.
		//!
		//! @param dest  a COPY of the walk's stack whose current object is
		//!              the sheet; on return, the stack after transmission.
		//! @return      whether the crossing enters, and the index pair it refracts between.
		inline OpenSheetCrossing ResolveOpenSheetCrossing(
			const RayIntersectionGeometric& ri,
			const Scalar sheetIOR,
			IORStack& dest
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
				dest.pushOpenSheet( sheetIOR );
				c.etaTo = sheetIOR;
			} else {
				if( dest.containsCurrent() ) {
					dest.pop();
				} else {
					ResolveUnpushedExit( ri, dest.currentObject(), dest );
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
		//! that differs from the walk's own top (see
		//! IORStack::crossingEtaFrom).  Caller owns the result.
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
