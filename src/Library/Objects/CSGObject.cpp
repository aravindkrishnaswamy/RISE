//////////////////////////////////////////////////////////////////////
//
//  CSGObject.cpp - Implements the CSGObject class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <cmath>			// std::isfinite / std::fabs (the probe's range2 + margin guards, CSGObject::SelfHitRootFloor's ownership window)
#include "CSGObject.h"

using namespace RISE;
using namespace RISE::Implementation;

CSGObject::CSGObject( const CSG_OP& op_ ) :
  pObjectA( 0 ),
  pObjectB( 0 ),
  op( op_ )
{
}

CSGObject::~CSGObject( )
{
	// RELEASE OUR CONSUMPTION, not "make it visible again".  Since 87 step 3b an
	// operand is routinely shared by N composites (a `source` instance of a subtree
	// containing a `csg_object` re-Finalizes it with `obja` / `objb` unchanged), and
	// an unconditional re-show here made the FIRST teardown resurrect an operand the
	// other N-1 composites were still consuming -- it rendered as a standalone shape
	// beside the composites that own it, and became parentable, since
	// ObjectManager::SetObjectParent identifies an operand as "hidden and has
	// geometry".  See IObjectPriv::AddConsumer.
	if( pObjectA ) {
		pObjectA->RemoveConsumer();
	}

	if( pObjectB ) {
		pObjectB->RemoveConsumer();
	}

	safe_release( pObjectA );
	safe_release( pObjectB );
}

IObjectPriv* CSGObject::CloneFull()
{
	CSGObject*	pClone = new CSGObject( op );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "clone" );

	pClone->AssignObjects( pObjectA, pObjectB );

	if( pMaterial ) {
		pClone->AssignMaterial( *pMaterial );
	}

	if( pModifier ) {
		pClone->AssignModifier( *pModifier );
	}

	if( pShader ) {
		pClone->AssignShader( *pShader );
	}

	if( pRadianceMap ) {
		pClone->AssignRadianceMap( *pRadianceMap );
	}

	return pClone;
}

IObjectPriv* CSGObject::CloneGeometric()
{
	CSGObject*	pClone = new CSGObject( op );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "clone" );

	pClone->AssignObjects( pObjectA, pObjectB );
	return pClone;
}

namespace
{
	// Snapshot-clone one CSG operand to an INDEPENDENT object.  Operands are
	// IObjectPriv*; downcast to the concrete Object to reach the virtual
	// CloneSnapshot (a CSGObject operand recurses correctly through it).  The
	// returned object carries one reference the caller owns.  Falls back to
	// addref if the operand is somehow not an Implementation::Object.
	IObjectPriv* SnapshotCloneOperand( IObjectPriv* operand )
	{
		if( !operand ) {
			return 0;
		}
		if( Object* concrete = dynamic_cast<Object*>( operand ) ) {
			return concrete->CloneSnapshot();   // refcount 1, recurses for nested CSG
		}
		operand->addref();
		return operand;
	}
}

Object* CSGObject::CloneSnapshot() const
{
	// Snapshot-clone the operands to INDEPENDENT objects FIRST, then build a
	// fresh CSGObject with the same operation and assign them.  This is what
	// makes the clone a real CSGObject (not a sliced plain Object) with its
	// own operand subtree.
	IObjectPriv* cloneA = SnapshotCloneOperand( pObjectA );
	IObjectPriv* cloneB = SnapshotCloneOperand( pObjectB );

	CSGObject* pClone = new CSGObject( op );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "snapshot CSG clone" );

	// AssignObjects addrefs both operands (and CONSUMES them, which keeps them
	// out of every world-visible walk, matching the live CSG).  We then drop our own
	// references so the clone is the sole owner of its operand clones.
	if( cloneA && cloneB ) {
		pClone->AssignObjects( cloneA, cloneB );
	}
	safe_release( cloneA );
	safe_release( cloneB );

	// Copy the shared mutable state (material clone + addref'd leaves +
	// flags + transform).  CSGObject has no geometry of its own, so there is
	// nothing geometry-specific to copy beyond what the base helper handles.
	CopySnapshotStateInto( *pClone );

	return pClone;
}

const BoundingBox CSGObject::getBoundingBox() const
{
	if( !pObjectA || !pObjectB ) {
		GlobalLog()->PrintSourceWarning( "CSGObject::getBoundingBox:: No subobjects for this CSG object, returning an empty box", __FILE__, __LINE__ );
		return BoundingBox( Point3(0,0,0), Point3(0,0,0) );
	}

	BoundingBox bbox = pObjectA->getBoundingBox();

	// Unions and intersections must enclose both child volumes. Subtraction
	// can never extend beyond object A.
	if( op != CSG_SUBTRACTION ) {
		bbox.Include( pObjectB->getBoundingBox() );
	}

	const Point3 corners[8] = {
		Point3( bbox.ll.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ur.z )
	};

	BoundingBox transformed(
		Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
		Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY )
	);

	for( unsigned int i=0; i<8; i++ ) {
		transformed.Include( Point3Ops::Transform( m_mxFinalTrans, corners[i] ) );
	}

	transformed.SanityCheck();
	return transformed;
}

bool CSGObject::AssignObjects( IObjectPriv* objA, IObjectPriv* objB )
{
	// Check the parameters
	if( !objA || !objB ) {
		GlobalLog()->PrintEx( eLog_Error, "CSGObject::AssignObjects:: bad objects" );
		return false;
	}

	// Symmetric with the destructor: give up the consumption we held on the OUTGOING
	// pair (which may leave them consumed by other composites) before claiming the
	// incoming one.
	if( pObjectA ) {
		pObjectA->RemoveConsumer();
	}

	if( pObjectB ) {
		pObjectB->RemoveConsumer();
	}

	safe_release( pObjectA );
	safe_release( pObjectB );

	pObjectA = objA;
	pObjectB = objB;

	pObjectA->addref();
	pObjectB->addref();

	pObjectA->AddConsumer();
	pObjectB->AddConsumer();

	return true;
}

void CSGObject::SetOperation( const CSG_OP& op_ )
{
	op = op_;
}

namespace
{
	//! Adopt the FULL per-surface payload from `src` into `dst` -- every
	//! field a child geometry stamps at intersection time OTHER than the
	//! boundary-identifying range/normal fields (range, range2, vNormal,
	//! vNormal2, vGeomNormal, vGeomNormal2), which each CSG branch below
	//! computes explicitly per the CSG algebra.
	//!
	//! INVARIANT: the reported boundary surface's auxiliary payload --
	//! texture coordinates, surface derivatives, geometry-derived signal
	//! channel, texture footprint, vertex color, tangent frame, and the
	//! object-space intersection point used
	//! by solid-texturing consumers -- must come WHOLLY from the operand
	//! that owns that surface.  CSGObject::IntersectRay starts most
	//! branches with an efficient whole-record `ri = riObjA` (or `riObjB`)
	//! copy and then overrides only the boundary-identifying fields per
	//! the CSG algebra.  When the algebra attributes the reported boundary
	//! to the OTHER operand, the auxiliary fields must be re-adopted from
	//! the record that actually owns that surface -- otherwise the hit
	//! ships with, e.g., operand B's normal paired with operand A's
	//! UV / derivatives / tangent / vertex color, a mixed-surface payload
	//! that silently corrupts texturing, bump mapping, mip LOD, and any
	//! solid-texturing painter keyed off the object-space hit point
	//! downstream.  Call sites pass `riObjA` or `riObjB` -- whichever
	//! operand's entry face the branch is about to report -- even when
	//! that happens to already be `dst`'s current owner (a harmless
	//! self-copy that keeps every boundary-defining branch going through
	//! the same explicit, reviewable adoption).
	//!
	//! EXIT-DESIGNATED BOUNDARIES: `src`'s auxiliary payload was computed
	//! by the child geometry for `src`'s ENTRY hit only -- no RISE
	//! geometry computes a second UV / derivatives / tangent / vertex-
	//! color set for the exit hit (only the exit NORMAL is available, via
	//! vNormal2 / vGeomNormal2).  So when a branch reports an operand's
	//! EXIT face as the composite's entry boundary (SUBTRACTION's "the
	//! carve's near wall is B's exit face" case), the adopted payload is
	//! honestly the wrong face of the RIGHT operand.  That is a real,
	//! documented limitation of the intersection data model -- do not
	//! try to fabricate exit-face UVs/derivatives/tangents here.
	//!
	//! NOT copied (out of scope for this invariant): `ptObjExit` has no
	//! consumers today; `glossyFilterWidth` / `ambientIOR` are stamped by
	//! the integrator/ray-caster layer above CSGObject (or, for
	//! glossyFilterWidth, simply never propagated into the freshly-
	//! constructed `riObjA` / `riObjB` records at all) and are unaffected
	//! by which operand owns the reported surface.
	//!
	//! `pCustom` (the legacy 3DS-shader refcounted payload) IS copied
	//! (P2-4 fix) -- the same per-surface, child-geometry-stamped payload
	//! category as the fields above, previously left as whichever operand
	//! `dst` started life as a whole-record copy of.  Mirrors
	//! RayIntersectionGeometric::operator='s own pCustom handling: release
	//! the old reference, take the new one, addref if non-null.
	void AdoptCsgSurfacePayload( RayIntersectionGeometric& dst, const RayIntersectionGeometric& src )
	{
		// Per-surface identity, same category as the fields below:
		// whether the reported vGeomNormal was oriented to oppose the ray
		// (double-sided meshes) belongs to whichever operand's surface is
		// actually being reported, not whichever operand `dst` started
		// life as a whole-record copy of (external review round 7,
		// item 4).  See RayIntersectionGeometric::bGeomNormalOrientedToRay's
		// doc comment.
		dst.bGeomNormalOrientedToRay = src.bGeomNormalOrientedToRay;

		dst.ptCoord = src.ptCoord;
		dst.ptCoord1 = src.ptCoord1;
		dst.bHasTexCoord1 = src.bHasTexCoord1;
		dst.derivatives = src.derivatives;
		// Phase-2 geometry-derived signals: the same per-surface payload
		// category as `derivatives` immediately above -- the provider
		// back-pointer AND the object-space (point, normal) it is to be
		// queried at were stamped by ONE child geometry for ITS OWN
		// surface.  Left un-adopted, a boundary the algebra credits to
		// operand B would ask operand A's field about a point that is not
		// on it, and `occlusion()` would silently report the wrong solid's
		// cavities.  Note this deliberately carries the CHILD's own local
		// frame (like ptObjIntersec below) -- which is exactly the frame
		// that child's provider expects, since both signals are
		// dimensionless and never cross the transform boundary.
		dst.signals = src.signals;
		dst.txFootprint = src.txFootprint;
		dst.vColor = src.vColor;
		dst.bHasVertexColor = src.bHasVertexColor;
		dst.vTangent = src.vTangent;
		dst.bitangentSign = src.bitangentSign;
		dst.bHasTangent = src.bHasTangent;
		dst.bShadingTangentFromGeometry = src.bShadingTangentFromGeometry;
		// C2: same per-surface payload category as vTangent/bHasTangent just
		// above -- a geometry-supplied fibre tangent (HairGeometry) belongs to
		// whichever operand's surface is actually being reported.  Without
		// this, a boundary branch that re-adopts bShadingTangentFromGeometry
		// from `src` while `dst` keeps stale vShadingTangent/bHasShadingTangent
		// from whichever operand it was whole-record-copied from would pair a
		// "true" flag with the WRONG (or absent) supplied tangent -- exactly
		// the mixed-surface contamination this function exists to prevent.
		dst.vShadingTangent = src.vShadingTangent;
		dst.bHasShadingTangent = src.bHasShadingTangent;

		// Wireframe view-mode edge info is per-surface payload too (GUI
		// render modes P1): the closest-edge point belongs to the SAME
		// triangle/operand as the reported boundary, or the wireframe
		// shader measures a cross-operand distance.
		dst.ptWireNearestEdge = src.ptWireNearestEdge;
		dst.bHasWireEdgeInfo = src.bHasWireEdgeInfo;

		// Same contamination pattern as the fields above (a per-surface
		// value baked in by the child geometry at intersection time,
		// silently stale after `ri = riObj<other>` when the boundary
		// crosses operand identity), and it has live consumers
		// (GlintModifier::FindFacet, Voronoi3DPainter) that key solid
		// texturing off it.  Note this is still the CHILD object's own
		// local-frame point (Object::IntersectRay computes it before the
		// CSG-level transform); CSGObject reports `this` as `ri.pObject`
		// for IOR-stack/shading identity, so ptObjIntersec's frame does
		// not match ri.pObject's transform for ANY CSG hit, buggy branch
		// or not -- that is a separate, pre-existing frame-semantics gap,
		// not something this adoption fixes or worsens.
		dst.ptObjIntersec = src.ptObjIntersec;

		// P2-4: pCustom is refcounted (legacy 3DS-shader payload) -- mirror
		// RayIntersectionGeometric::operator='s own handling exactly.
		safe_release( dst.pCustom );
		dst.pCustom = src.pCustom;
		if( dst.pCustom ) {
			dst.pCustom->addref();
		}
	}

	//! Adopt the RayIntersection-LEVEL binding pointers -- material,
	//! modifier, shader, radiance map -- from the operand that actually
	//! OWNS the reported boundary surface.  These are siblings of
	//! AdoptCsgSurfacePayload's geometric-payload fields but live one
	//! level up, on RayIntersection rather than RayIntersectionGeometric
	//! (see RayIntersection.h), so they need their own tiny helper.
	//!
	//! Same contamination pattern: a CSG_INTERSECTION "outside both"
	//! branch starts with an efficient whole-record `ri = riObjA` copy,
	//! then (via AdoptCsgSurfacePayload) re-adopts the geometric payload
	//! from riObjB when the algebra attributes the reported boundary to
	//! B -- but without this helper the four binding pointers silently
	//! stayed A's.  Per-operand-material CSG (e.g. `csg_object` combining
	//! two objects with different materials) then shaded B's boundary
	//! with A's material.  The CSGObject's OWN bindings (if it has any)
	//! still take final precedence -- see the conditional overrides at
	//! the bottom of IntersectRay -- this helper only fixes which
	//! OPERAND's binding is the fallback.
	void AdoptCsgSurfaceBindings( RayIntersection& dst, const RayIntersection& src )
	{
		dst.pMaterial = src.pMaterial;
		dst.pModifier = src.pModifier;
		dst.pShader = src.pShader;
		dst.pRadianceMap = src.pRadianceMap;
	}

	//! EXIT-designated boundary payload recovery (P2-e).  A CSG branch
	//! that reports operand `operand`'s EXIT face as the composite's
	//! boundary has only that operand's ENTRY-hit auxiliary payload
	//! available -- no RISE geometry computes a second UV / derivatives /
	//! tangent / vertex-color / wire-edge set for an exit hit, only the
	//! exit NORMAL (vNormal2 / vGeomNormal2).  Recover the REAL payload
	//! for the exit face with a reverse probe: a short ray starting just
	//! beyond the exit point (along the ORIGINAL ray direction, so the
	//! origin sits just outside the operand's solid) and travelling in
	//! the REVERSED direction back toward the exit point.  Its first hit
	//! lands on the exact same geometric face, approached from the
	//! outside, with a fully-stamped ENTRY payload for THAT face -- and
	//! by construction (entering the solid from just outside it), that
	//! probe entry NORMAL should equal the branch's already-computed
	//! reversed exit normal (-vNormal2 / -vGeomNormal2); the caller keeps
	//! its own normal fields and this helper only ever touches the
	//! auxiliary payload, so that equality is never load-bearing, just a
	//! sanity invariant worth noting for anyone instrumenting this path.
	//!
	//! `exitRangeCsgLocal` must already be in CSG-LOCAL units (i.e.
	//! `riObjX.geometric.range2` computed with bComputeExitInfo=true --
	//! see the P1-c fix).  `dst.ray` is read as the CSG-local ray (every
	//! child restores its `ray` field to what was passed in, so any
	//! `riObjA`/`riObjB`/`ri` record mid-switch carries the same local
	//! ray) and `dst`'s pre-mutation glossyFilterWidth /
	//! bWantsWireEdgeInfo are propagated onto the probe so it honours the
	//! same cast-time inputs as the main children did.
	//!
	//! On success, adopts the probe's payload into `dst` (via
	//! AdoptCsgSurfacePayload -- the probe IS an entry hit, so it carries
	//! exactly that field set, including possibly-valid wire-edge info --
	//! no need to clear it, unlike the entry-face-payload fallback) and
	//! returns true.  On a probe miss (grazing ray / numeric edge at the
	//! ε-margin) OR a hit that lands too far away to be the SAME face
	//! (P2-3, see the range-gate comment below), leaves `dst` untouched
	//! and returns false; the caller falls back to the previous behavior
	//! (the operand's own entry-hit payload already sitting in `dst`, with
	//! wire-edge info cleared).
	bool AdoptCsgExitFacePayloadViaProbe(
		RayIntersectionGeometric& dst,
		IObjectPriv* operand,
		Scalar exitRangeCsgLocal,
		const Vector3& exitGeomNormalCsgLocal )
	{
		if( !operand || !std::isfinite( exitRangeCsgLocal ) || exitRangeCsgLocal <= 0 || exitRangeCsgLocal == RISE_INFINITY ) {
			// (A triangle-mesh operand publishes an IEEE inf range2, which
			// the RISE_INFINITY (DBL_MAX) compare does not catch.)
			return false;
		}

		const Vector3 dir = dst.ray.Dir();
		const Point3 ptExitLocal = dst.ray.PointAtLength( exitRangeCsgLocal );

		// Margin -- FP-representability-based (P2-4 fix), the same bound
		// RayCaster::ResolveXrayView_ uses for its adaptive skip epsilon,
		// NOT a fraction of the operand's world bounding-box diagonal.
		// The bbox-diagonal formula was wrong the same way the x-ray
		// nudge's was: a REMOTE lobe on a large or multi-lobed operand
		// (e.g. a torus' far wall, or any decoy face many units away)
		// inflates the diagonal, which inflates both this margin and the
		// same-face acceptance radius (maxAcceptRange below) far past
		// what's needed to clear ptExitLocal -- large enough for the
		// probe to reach past the intended face and adopt a nearby DECOY
		// face's payload instead; an unbounded operand (infinite/clipped
		// plane) reports an infinite bbox and fell back to an
		// exit-range-scaled reference, reintroducing camera-range
		// dependence.  Deriving the margin from ptExitLocal's own
		// coordinate magnitude sidesteps both: it is proportional only to
		// what double precision can represent AT THAT POINT, independent
		// of the operand's overall size or the camera.  Deleting the bbox
		// lookup also removes the only remaining consumer of the P2-3
		// diagonal/safeDiag fallback machinery.
		//
		// External review round 4, item 2 (P2), same disease as the
		// RayCaster P1 above: max-abs-COMPONENT is TRANSVERSE-coordinate-
		// coupled -- an exit point with a huge coordinate on an axis the
		// probe barely travels along inflates the margin (and therefore
		// the same-face acceptance radius) far past what's needed, wide
		// enough to reach a decoy face that a tighter, direction-aware
		// margin would never touch.  Weight each axis's contribution by
		// how much the PROBE's travel direction moves along it.  The
		// probe's own ray direction is `-dir` (reversed, see probeRay
		// below), but the margin displacement that matters here happens
		// BEFORE that reversal -- probeOrigin is ptExitLocal pushed
		// forward by `dir * margin` (see immediately below) -- so the
		// weighting uses the parent ray's own (un-reversed) `dir`, the
		// direction that displacement actually travels along:
		//
		//   margin = max( 1e-12, kUlpFactor * ( |ptExitLocal.x|*|dir.x| + |ptExitLocal.y|*|dir.y| + |ptExitLocal.z|*|dir.z| ) )
		//
		// Unlike RayCaster::ResolveXrayView_, this probe is one-shot with a
		// graceful fallback (return false -> caller keeps the entry-face
		// payload) rather than a retry ladder -- there is no cancellation-
		// absorbing follow-up here, by design: a probe miss is already a
		// handled, non-fatal outcome (see the function's own doc comment
		// above), so there is nothing a retry would be recovering FROM.
		// Keep it one-shot.
		const Scalar dirWeightedAbs =
			std::fabs( ptExitLocal.x ) * std::fabs( dir.x ) +
			std::fabs( ptExitLocal.y ) * std::fabs( dir.y ) +
			std::fabs( ptExitLocal.z ) * std::fabs( dir.z );
		constexpr Scalar kUlpFactor = 64.0 * 2.2204460492503131e-16; // 64 * DBL_EPSILON, see RayCaster::ResolveXrayView_
		// Floor 1e-12 (external review round 6, item 2 -- REVERTS the r5
		// bump to 1e-9): r5 raised this floor to match RayCaster::
		// ResolveXrayView_'s eps floor "identically", on the theory that
		// both nudges face the same local->world-stretch-amplified standoff
		// problem.  They do NOT.  RayCaster's nudge starts from
		// `ri.geometric.ptIntersection`, which Object::IntersectRay
		// publishes BACKWARD-biased -- `range - SURFACE_INTERSEC_ERROR`
		// (Object.cpp, entry-point publication) -- i.e. SHORT of the true
		// surface, so a stretch amplifies a gap the nudge still has to
		// cross, and a bigger floor is genuinely load-bearing there.  THIS
		// probe's `ptExitLocal` instead comes from `exitRangeCsgLocal`
		// (the operand's own published `range2`), which Object::
		// IntersectRay publishes FORWARD-biased -- `range2 +
		// SURFACE_INTERSEC_ERROR` (see `ptObjExit`), then range2 itself is
		// recomputed from that already-advanced `ptExit` -- i.e. PAST the
		// true face, in the SAME direction this probe's own margin pushes.
		// The producer-side bias already clears the amplified standoff
		// before this function ever runs; stacking a second, much larger
		// (1e-9) margin on top doesn't clear anything further, it only
		// inflates the same-face acceptance radius (maxAcceptRange below,
		// ~2.1e-9 at the r5 floor vs ~2.1e-12 here) far past what the
		// producer bias needs -- wide enough to re-open decoy-payload
		// adoption for a second face separated from the true exit by less
		// than ~2e-9 (exactly the tiny-gap case this floor was supposed to
		// guard against, now on the WRONG side of the trade).  At 1e-12 the
		// acceptance window (~2.1e-12) shares a magnitude with the
		// operand's own SURFACE_INTERSEC_ERROR self-hit gate; marginal
		// cases miss the probe and take the graceful entry-payload
		// fallback (quality only, never a decoy-payload correctness bug).
		//
		// Second floor (2026-09-05, debt-25 review rounds 1-2): the probe
		// must ALSO clear the operand primitive's own self-hit band, or
		// the primitive treats the probe origin as the published hit
		// point of the very face it is trying to re-hit and drops that
		// root (BoxGeometry::DropSelfHitRoot; there is no provenance to
		// tell a deliberate standoff from Object::IntersectRay's 1e-12
		// back-off -- identical geometry).  The band is, per face axis in
		// OPERAND-LOCAL units,
		//   eps = 4 * NEARZERO + kUlpFactor * |localOrigin.axis|
		// and the probe origin's distance to the face plane in that frame
		// is  margin * |M^-1 dir . n|  ~=  margin * stretch * |dir . n|,
		// stretch = |M^-1 dir| (local units per world unit along dir).
		// So the floor is the band, doubled, mapped back through the
		// operand's inverse stretch and the exit angle:
		//   floor = 2 * (4*NEARZERO + kUlp * |localExit . nLocal|) / rate,
		//   rate  = |M^-1 dir . nLocal| (see the "Rate, exactly" note below)
		// with the rate clamped at 1/20 of |M^-1 dir| (steeper exits miss
		// the probe and take the graceful entry-payload fallback, the
		// marginal outcome the paragraph above already accepts).  The ulp term reads ONLY
		// the exit point's component along the face normal (mapped into
		// the operand frame with the transpose of the forward matrix, the
		// way normals go world->local), because that is the one
		// coordinate whose rounding can move the plane distance -- the
		// same per-axis rule the box applies.  It carries NO other
		// coordinate-magnitude term: round 2's P1 was an L1-of-ptExit
		// floor that became EIGHT world units at X = 1e12 (Test 14, the
		// r4 transverse-coupling guard), and a max-component variant of
		// this floor failed the same test one level down, because the
		// operand there is a NESTED CSG whose local frame is the world
		// frame.  At unit scale / normal incidence the floor is 8e-12, so
		// the ~2.1x acceptance window (~1.7e-11) stays ~100x under the
		// ~2e-9 decoy-face radius the r6 revert rejected (Test 15) -- at
		// THAT scale and incidence: the window grows with 1/rate (20x at
		// the grazing clamp, so ~3e-10, still under Test 15's 5e-10 gap;
		// 1000x for an operand scaled up 1000x, where it is the same
		// 1.7e-11 in the operand's own units and the decoy radius scales
		// with it).  A scaled operand (stretch 1e-3) gets a 1000x larger
		// world margin, which is the same local distance.  Only the operand's OWN
		// transform is visible here: a scaled leaf inside a nested CSG
		// operand still reads stretch 1 and may miss the probe (graceful
		// fallback), same as any other marginal case.
		//
		// Rate, exactly (review round 3, P2-1): the operand-local plane
		// distance per world unit of standoff is |M^-1 dir . n_local|,
		// the local ray direction projected on the local face normal.  The
		// earlier factorisation stretch * |cos_world| (|M^-1 dir| times
		// the WORLD cosine) under-estimates it by s_j * |M^-1 dir| on an
		// anisotropic stretch whose stretched-up axis is the face normal
		// -- a non-cubic operand (a slab) stretched 4x along its exit
		// normal and hit obliquely already pushed the probe back inside
		// the band; no rotation is needed, and a cube cannot show it (the
		// exit face is then the max-|M^-1 dir| axis, which bounds the
		// shortfall at sqrt(3)).  Both ingredients were already in
		// hand; the clamp is 1/20 of the local direction's length, the
		// same grazing floor as before expressed in the local frame.
		// With NO usable exit normal (a geometry that leaves vGeomNormal2
		// unset) the floor degrades to the bare doubled band at rate 1:
		// no coordinate term at all -- never the max-component formula.
		//
		// THIRD floor (2026-09-05, adversarial review of a8bef210 -- the P1):
		// the band above is the BOX's gate, and it was standing in for every
		// operand.  Since a8bef210 the OTHER primitives gained scale-relative
		// ROOT floors of their own -- a sphere rejects roots below
		// NEARZERO*(1 + |o|_1 + radius), a quadric/mesh below
		// NEARZERO*(1 + |o|_1) -- and those OVERTAKE the box band as soon as
		// the operand is more than a few units across (sphere R >~ 3.5,
		// ellipsoid semi-axis >~ 8).  Past that the probe stood off by LESS
		// than the operand's own gate, the operand dropped the probe's root as
		// a self-hit, and every such CSG boundary silently took the
		// entry-face-payload fallback: measured end to end on
		// `CSG_SUBTRACTION(box half-extent 20, sphere R=4 at its front face)`,
		// the far cavity wall reported the ENTRY face's UV (0.0049, 0.4798)
		// instead of the truth (0.4950, 0.4794) -- the ANTIPODAL point, i.e. a
		// texture read from the opposite side of the carving sphere.  R = 1..3
		// were correct, which is why this survived the debt-25 rounds.
		//
		// So ASK the operand instead of assuming: IObject::SelfHitRootFloor
		// (IGeometry::SelfHitRootFloor under it, CSGObject's own max-over-
		// operands override for a nested composite) reports the gate in the
		// operand's OWN local frame, as a RANGE along a unit local direction.
		//
		// The two terms map back differently and must not be conflated:
		//   * the box BAND is a PLANE DISTANCE, and a standoff of `m` along
		//     `dir` covers `m * rate` of plane distance -- divide by `rate`;
		//   * a ROOT floor is a RANGE, and a standoff of `m` along `dir` is
		//     `m * stretch` of local range -- divide by `stretch`.
		// Both keep the same 2x headroom, and the 1/20 grazing clamp stays on
		// `rate` (the geometry-side query does its own clamping where its gate
		// is angle-dependent -- see BoxGeometry::SelfHitRootFloor).  Taking
		// the max of the two is deliberately conservative: for a box operand
		// the two terms are algebraically identical (band/|cos| / stretch ==
		// band / rate), and for every other operand the box band remains a
		// harmless lower bound that nothing depends on.
		Scalar selfHitFloor = Scalar(8) * NEARZERO;
		{
			const Matrix4 mxInv = operand->GetFinalInverseTransformMatrix();
			const Vector3 dirLocal = Vector3Ops::Transform( mxInv, dir );
			const Scalar stretch = Vector3Ops::Magnitude( dirLocal );
			const Point3 exitLocal = Point3Ops::Transform( mxInv, ptExitLocal );
			const Matrix4 mxFwdT = Matrix4Ops::Transpose( operand->GetFinalTransformMatrix() );
			const Vector3 nLocalUnnorm = Vector3Ops::Transform( mxFwdT, exitGeomNormalCsgLocal );
			const Scalar nLocalMag = Vector3Ops::Magnitude( nLocalUnnorm );
			if( nLocalMag > NEARZERO && stretch > NEARZERO ) {
				const Scalar alongNormalLocal =
					std::fabs( Vector3Ops::Dot( Vector3( exitLocal.x, exitLocal.y, exitLocal.z ), nLocalUnnorm ) ) / nLocalMag;
				const Scalar rate = std::max(
					std::fabs( Vector3Ops::Dot( dirLocal, nLocalUnnorm ) ) / nLocalMag,
					Scalar(0.05) * stretch );
				const Scalar bandLocal = Scalar(4) * NEARZERO + kUlpFactor * alongNormalLocal;
				selfHitFloor = Scalar(2) * bandLocal / rate;

				const Vector3 dirLocalUnit( dirLocal.x / stretch, dirLocal.y / stretch, dirLocal.z / stretch );
				const Vector3 nLocalUnit( nLocalUnnorm.x / nLocalMag, nLocalUnnorm.y / nLocalMag, nLocalUnnorm.z / nLocalMag );
				const Scalar rootFloorLocal = operand->SelfHitRootFloor( exitLocal, dirLocalUnit, nLocalUnit );
				selfHitFloor = std::max( selfHitFloor, Scalar(2) * rootFloorLocal / stretch );
			}
		}
		// (The r6 paragraph above's "~2.1e-12 acceptance window" figure is
		// the 1e-12 term's own; with selfHitFloor in the max() the window
		// is ~1.7e-11 at unit scale -- the figure that now governs.)
		const Scalar margin = std::max( std::max( Scalar(1e-12), selfHitFloor ), kUlpFactor * dirWeightedAbs );

		// NON-FINITE GUARD (adversarial review of 4b141ad3, P1-3).  A floor of
		// +inf makes `margin` infinite, `probeOrigin` NaN on every axis where
		// `dir` has a zero component (0 * inf), and -- worse -- makes the
		// `range > maxAcceptRange` rejection below UNREACHABLE, because every
		// comparison against a NaN is false: the probe would then adopt whatever
		// the operand happened to report, at any distance, and stamp NaN
		// coordinates into the composite hit.  The one known producer (an
		// unbuilt / unbounded collection bbox) is fixed at its own layer in
		// `Geometry::BoundingBoxRootFloor`; this is the caller-side backstop, and
		// it degrades to the SAME graceful outcome as any other probe miss --
		// the caller keeps the operand's entry-face payload.
		if( !std::isfinite( selfHitFloor ) || !std::isfinite( margin ) ) {
			return false;
		}

		const Point3 probeOrigin(
			ptExitLocal.x + dir.x * margin,
			ptExitLocal.y + dir.y * margin,
			ptExitLocal.z + dir.z * margin );

		const Ray probeRay( probeOrigin, Vector3( -dir.x, -dir.y, -dir.z ) );

		RayIntersection probe( probeRay, nullRasterizerState );
		probe.geometric.PropagateCastInputs( dst );

		// Screen-space differentials (Landing 2 / Igehy 1999): without
		// this the probe's hasDifferentials defaults to false and the
		// recovered payload's txFootprint stays invalid (base-mip
		// fallback) -- see RayCaster::ResolveXrayView_ for the sibling
		// copy-diffs-onto-a-continuation-ray pattern.
		//
		// Origin deltas (rxOrigin/ryOrigin) are NOT a plain copy (P2-4
		// fix): dst.ray's diffs are expressed relative to dst.ray's OWN
		// origin, but the probe's origin sits a free-space distance
		// `t = exitRangeCsgLocal + margin` FORWARD of dst.ray's origin
		// along dst.ray's own (un-reversed) direction -- the same
		// straight-line propagation RayCaster::ResolveXrayView_ applies to
		// its own continuation rays (Igehy 1999 §3.1): the auxiliary ray
		// advances alongside the central ray by `t`, so re-expressed
		// relative to the new origin, dO' = dO + t*dD.  This transfer MUST
		// use dst's FORWARD rxDir/ryDir (not yet negated) -- the auxiliary
		// ray genuinely traveled forward, alongside dst.ray, to reach the
		// probe's origin; only the probe's own central ray (fired FROM
		// that origin) points backward.
		//
		// Direction deltas (rxDir/ryDir) are offsets added to the ray's
		// OWN Dir() (see TextureFootprintCompute.h) -- since the probe's
		// central direction is REVERSED (-dir, not dir), the deltas must
		// be negated, or they'd describe a near-antipodal auxiliary ray
		// instead of a small angular perturbation around the probe's own
		// direction.  This negation applies only to the DIRECTION field;
		// the origin transfer above already used the un-negated (forward)
		// values because that's the frame the propagation actually
		// happened in.
		probe.geometric.ray.hasDifferentials = dst.ray.hasDifferentials;
		if( dst.ray.hasDifferentials ) {
			const Scalar t = exitRangeCsgLocal + margin;
			probe.geometric.ray.diffs.rxOrigin = dst.ray.diffs.rxOrigin + dst.ray.diffs.rxDir * t;
			probe.geometric.ray.diffs.ryOrigin = dst.ray.diffs.ryOrigin + dst.ray.diffs.ryDir * t;
			probe.geometric.ray.diffs.rxDir = Vector3( -dst.ray.diffs.rxDir.x, -dst.ray.diffs.rxDir.y, -dst.ray.diffs.rxDir.z );
			probe.geometric.ray.diffs.ryDir = Vector3( -dst.ray.diffs.ryDir.x, -dst.ray.diffs.ryDir.y, -dst.ray.diffs.ryDir.z );
		}

		// Bound the accepted probe hit to the SAME face (P2-3/P2-4 fix):
		// the probe only needs to travel back ~margin to re-land on the
		// exit point, so a hit much farther than that is a DIFFERENT lobe
		// of the operand (a decoy face), not the intended one.  `slack` is
		// deliberately an order of magnitude below `margin` itself (both
		// now ulp-scale-derived, shrinking together with the tighter
		// margin above) so it absorbs surface curvature near the probe
		// without opening the gate to another lobe.  This also bounds the
		// search: no reason to trace past the acceptance radius.
		const Scalar slack = margin * Scalar(0.1);
		const Scalar maxAcceptRange = margin * Scalar(2.0) + slack;

		// Same non-finite guard as `margin` above (adversarial review of
		// 7923bf2f, P3).  `margin` being finite does not by itself make
		// `2.1 * margin` finite -- it overflows for a margin within a factor of
		// 2.1 of DBL_MAX -- and `maxAcceptRange` is what BOTH the trace bound
		// and the same-face rejection below are expressed in.  A NaN or inf
		// there makes `range > maxAcceptRange` unreachable (every comparison
		// against NaN is false), which is exactly the failure the `margin` guard
		// was added to close, reached one line later.
		if( !std::isfinite( maxAcceptRange ) ) {
			return false;
		}

		operand->IntersectRay( probe, maxAcceptRange, true, true, false );

		if( !probe.geometric.bHit || probe.geometric.range > maxAcceptRange ) {
			return false;
		}

		AdoptCsgSurfacePayload( dst, probe.geometric );

		// RE-PAIR the adopted normal-derivative fields with the normal field
		// the CALLER is actually reporting (the geometry-shading-signals arc,
		// 2026-08-29).  The payload just adopted is the PROBE's, and the probe
		// reported ITS OWN entry normal at that face; `dst.vNormal` was set by
		// the calling branch a few lines before this call and is either
		// `+operand.vNormal2` (the two CSG_UNION inside-an-operand branches) or
		// `-operand.vNormal2` (the CSG_SUBTRACTION exit-designated branch).
		// `dndu`/`dndv` -- and the SDF family's direct `curvature` -- are
		// derivatives OF a normal field, so they are only meaningful paired
		// with the field that is being reported; a sign mismatch ships a
		// convex-reading curvature on a concave cavity wall.
		//
		// This is the case the P1-1 audit note at the CSG_SUBTRACTION call site
		// flagged as "would need the same negation IF the probe's own normal
		// agrees in sign with vNormal2 -- unreachable today because only
		// triangle meshes populate derivatives".  Analytic primitives now
		// populate them at intersection time (design doc 5.4), and their normal
		// field IS direction-independent, so the case is reachable.  Testing the
		// dot product rather than hard-coding a negation keeps all three call
		// sites correct without each having to know which way its own branch
		// flipped: a positive dot is a no-op.
		if( Vector3Ops::Dot( probe.geometric.vNormal, dst.vNormal ) < Scalar( 0 ) ) {
			if( dst.derivatives.valid ) {
				dst.derivatives.dndu = -dst.derivatives.dndu;
				dst.derivatives.dndv = -dst.derivatives.dndv;
			}
			if( dst.derivatives.curvatureValid ) {
				dst.derivatives.curvature = -dst.derivatives.curvature;
			}
			// Same re-pairing for the signal-provider's own normal (Phase 2).
			// `dst.signals` was just adopted from the probe above, so
			// `signals.nObject` is the probe's own OUTWARD normal -- paired
			// with `probe.geometric.vNormal`, not with the flipped
			// `dst.vNormal` the caller is reporting.  A mismatch here means
			// the composite is reporting the OPPOSITE face from the one the
			// provider's normal describes, so occlusion()/thickness() would
			// march into the wrong side of the surface unless nObject flips
			// along with dndu/dndv/curvature above.
			if( dst.signals.pProvider ) {
				dst.signals.nObject = -dst.signals.nObject;
				// And the SENSE of "solid" inverts with it: the empty region
				// is now the provider's own INTERIOR.  occlusion()/convexity()
				// read the SIGN of the field, which no normal flip can
				// reverse, so the inversion travels as its own flag -- see
				// SurfaceSignalInfo::bComplementedField.  Toggled, not set, so
				// nested subtractions compose.
				dst.signals.bComplementedField = !dst.signals.bComplementedField;
			}
		}
		return true;
	}
}

void CSGObject::IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool, const bool, const bool ) const
{
	// The hitting of front and back faces are IGNORED for CSG objects!

	if( !pObjectA || !pObjectB ) {
		GlobalLog()->PrintSourceWarning( "CSGObject::IntersectRay:: No subobjects for this CSG object, ignoring hit request", __FILE__, __LINE__ );
		return;
	}

	// Bring the ray into our frame, first tuck away the original ray value
	Ray		orig = ri.geometric.ray;

	ri.geometric.bHit = false;
	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- the direction-true
	// world-to-local distance factor used below to convert the caller's
	// dHowFar (expressed in the CALLER's frame) into dHowFar2 (expressed
	// in THIS CSG's local frame), which is what the operand IntersectRay
	// calls below expect (P1 fix, mirrors Object::IntersectRay /
	// CSGObject::IntersectRay_IntersectionOnly).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, orig.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	ri.geometric.ray.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// Landing 2: transform ray differentials into object space alongside
	// origin/dir.  See Object::IntersectRay for the full rationale —
	// origins transform as offsets between world points (linear part of
	// the matrix), but rxDir / ryDir are offsets between UNIT-normalized
	// directions and require full reconstruct-transform-renormalise-
	// re-difference under any non-identity scale.  Without this fix,
	// ComputeTextureFootprint inside the CSG child mesh projects bad
	// auxiliaries onto object-space dpdu/dpdv and produces wrong UV
	// footprints (and wrong mip LOD) for any transformed CSG-textured
	// object.
	if( orig.hasDifferentials ) {
		ri.geometric.ray.diffs.rxOrigin = Vector3Ops::Transform( m_mxInvFinalTrans, orig.diffs.rxOrigin );
		ri.geometric.ray.diffs.ryOrigin = Vector3Ops::Transform( m_mxInvFinalTrans, orig.diffs.ryOrigin );

		const Vector3 d_world = orig.Dir();
		const Vector3 aux_x_world( d_world.x + orig.diffs.rxDir.x,
		                           d_world.y + orig.diffs.rxDir.y,
		                           d_world.z + orig.diffs.rxDir.z );
		const Vector3 aux_y_world( d_world.x + orig.diffs.ryDir.x,
		                           d_world.y + orig.diffs.ryDir.y,
		                           d_world.z + orig.diffs.ryDir.z );
		const Vector3 aux_x_obj = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, aux_x_world ) );
		const Vector3 aux_y_obj = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, aux_y_world ) );
		const Vector3 d_obj     = ri.geometric.ray.Dir();
		ri.geometric.ray.diffs.rxDir = Vector3( aux_x_obj.x - d_obj.x, aux_x_obj.y - d_obj.y, aux_x_obj.z - d_obj.z );
		ri.geometric.ray.diffs.ryDir = Vector3( aux_y_obj.x - d_obj.x, aux_y_obj.y - d_obj.y, aux_y_obj.z - d_obj.z );
		ri.geometric.ray.hasDifferentials = true;
	}

	// factor converts the caller's-frame distance limit dHowFar into the
	// CSG-LOCAL traversal limit the operand calls below expect (P1 fix).
	// The operands' own IntersectRay re-derives their local dHowFar2 from
	// whatever we pass here by multiplying by THEIR OWN direction-true
	// factor -- if we hand them the raw caller-frame dHowFar instead of
	// the CSG-local value, that second multiplication compounds against
	// the wrong starting frame.  Under a compressing CSG transform this
	// under-searches (an in-range occluder / closer surface is culled by
	// a too-small converted limit); under an expanding transform it
	// over-searches.  Same direction-true-factor convention as
	// Object::IntersectRay and CSGObject::IntersectRay_IntersectionOnly;
	// guard a degenerate transform that collapses this direction to ~0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = dHowFar;
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	RayIntersection		riObjA( ri.geometric.ray, ri.geometric.rast );
	riObjA.geometric.PropagateCastInputs( ri.geometric );
	RayIntersection		riObjB( ri.geometric.ray, ri.geometric.rast );
	riObjB.geometric.PropagateCastInputs( ri.geometric );

	pObjectA->IntersectRay( riObjA, dHowFar2, true, true, true );
	pObjectB->IntersectRay( riObjB, dHowFar2, true, true, true );

	/* Not necessary, should never happen!
	if( riObjA.geometric.bHit && riObjA.geometric.range2 == RISE_INFINITY ) {
		riObjA.geometric.range2 = 0;
		__debugbreak();
	}

	if( riObjB.geometric.bHit && riObjB.geometric.range2 == RISE_INFINITY ) {
		riObjB.geometric.range2 = 0;
		__debugbreak();
	}
	*/

	// Do different things depending on the type of CSG operation
	switch( op )
	{
	default:
	case CSG_UNION:
		// In the event of a union, we take the intersection that is closest
		// in range.
		if( riObjA.geometric.bHit && riObjB.geometric.bHit )
		{
			const bool insideA = (riObjA.geometric.range2 == 0);
			const bool insideB = (riObjB.geometric.range2 == 0);

			if( insideA && insideB )
			{
				// Inside both objects — inside the union.
				// Exit at the farther boundary.
				if( riObjA.geometric.range >= riObjB.geometric.range ) {
					ri = riObjA;
				} else {
					ri = riObjB;
				}
				// range2 is already 0, signaling "inside"
			}
			else if( insideA && !insideB )
			{
				// Inside A — inside the union.
				// Exit at A's boundary, unless B overlaps and extends further.
				if( riObjB.geometric.range < riObjA.geometric.range &&
					riObjB.geometric.range2 > riObjA.geometric.range )
				{
					// B overlaps and extends past A — exit at B's far side.
					// The reported "entry" of the composite hit is actually
					// B's exit boundary (we've been inside the union all along).
					ri = riObjB;
					ri.geometric.range = riObjB.geometric.range2;
					ri.geometric.vNormal = riObjB.geometric.vNormal2;
					ri.geometric.vGeomNormal = riObjB.geometric.vGeomNormal2;
					// EXIT-designated boundary (union, origin inside an
					// operand): recover B's real payload for this exit face
					// via a reverse probe (P2-e); on a probe miss, fall back
					// to B's entry-hit payload with wire-edge info cleared
					// (it belongs to the wrong face in that fallback).
					if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectB, riObjB.geometric.range2, riObjB.geometric.vGeomNormal2 ) ) {
						ri.geometric.bHasWireEdgeInfo = false;
					}
				} else {
					ri = riObjA;
				}
				ri.geometric.range2 = 0;
			}
			else if( !insideA && insideB )
			{
				// Inside B — inside the union.
				if( riObjA.geometric.range < riObjB.geometric.range &&
					riObjA.geometric.range2 > riObjB.geometric.range )
				{
					ri = riObjA;
					ri.geometric.range = riObjA.geometric.range2;
					ri.geometric.vNormal = riObjA.geometric.vNormal2;
					ri.geometric.vGeomNormal = riObjA.geometric.vGeomNormal2;
					// EXIT-designated boundary (union, origin inside an
					// operand): recover A's real payload for this exit face
					// via a reverse probe (P2-e); on a probe miss, fall back
					// to A's entry-hit payload with wire-edge info cleared
					// (it belongs to the wrong face in that fallback).
					if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectA, riObjA.geometric.range2, riObjA.geometric.vGeomNormal2 ) ) {
						ri.geometric.bHasWireEdgeInfo = false;
					}
				} else {
					ri = riObjB;
				}
				ri.geometric.range2 = 0;
			}
			else
			{
				// Outside both objects.
				// If B begins after A ends, then take A (disjoint)
				if( riObjB.geometric.range > riObjA.geometric.range2 ) {
					ri = riObjA;
				}
				// If A begins after B ends, then take B (disjoint)
				else if( riObjA.geometric.range > riObjB.geometric.range2 ) {
					ri = riObjB;
				}
				// Overlapping: A enters first
				else if( riObjA.geometric.range <= riObjB.geometric.range ) {
					ri = riObjA;
					// Exit at the farther boundary
					if( riObjB.geometric.range2 > riObjA.geometric.range2 ) {
						ri.geometric.range2 = riObjB.geometric.range2;
						ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjB.geometric.vGeomNormal2;
					}
				}
				// Overlapping: B enters first
				else {
					ri = riObjB;
					if( riObjA.geometric.range2 > riObjB.geometric.range2 ) {
						ri.geometric.range2 = riObjA.geometric.range2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
					}
				}
			}
		} else if( riObjA.geometric.bHit && !riObjB.geometric.bHit ) {
			ri = riObjA;
		} else if( !riObjA.geometric.bHit && riObjB.geometric.bHit ) {
			ri = riObjB;
		}
		break;
	case CSG_INTERSECTION:
		// If both the objects don't intersect, then there can't be an
		// intersection
		if( riObjA.geometric.bHit && riObjB.geometric.bHit )
		{
			const bool insideA = (riObjA.geometric.range2 == 0);
			const bool insideB = (riObjB.geometric.range2 == 0);

			if( insideA && insideB )
			{
				// Inside both objects — we are inside the CSG intersection.
				// Exit at whichever boundary we reach first.
				if( riObjA.geometric.range <= riObjB.geometric.range ) {
					ri = riObjA;
				} else {
					ri = riObjB;
				}
				// range2 is already 0 from child, signaling "inside"
			}
			else if( insideA && !insideB )
			{
				// Inside A, outside B — not inside the CSG intersection.
				// Can enter the intersection at B's entry if it's before A's exit.
				if( riObjB.geometric.range < riObjA.geometric.range )
				{
					ri = riObjB;
					ri.geometric.range = riObjB.geometric.range;
					ri.geometric.vNormal = riObjB.geometric.vNormal;
					ri.geometric.vGeomNormal = riObjB.geometric.vGeomNormal;
					// Exit at whichever boundary is closer
					if( riObjA.geometric.range <= riObjB.geometric.range2 ) {
						ri.geometric.range2 = riObjA.geometric.range;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal;
					} else {
						ri.geometric.range2 = riObjB.geometric.range2;
						ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjB.geometric.vGeomNormal2;
					}
				}
			}
			else if( !insideA && insideB )
			{
				// Outside A, inside B — not inside the CSG intersection.
				// Can enter the intersection at A's entry if it's before B's exit.
				if( riObjA.geometric.range < riObjB.geometric.range )
				{
					ri = riObjA;
					ri.geometric.range = riObjA.geometric.range;
					ri.geometric.vNormal = riObjA.geometric.vNormal;
					ri.geometric.vGeomNormal = riObjA.geometric.vGeomNormal;
					// Exit at whichever boundary is closer
					if( riObjB.geometric.range <= riObjA.geometric.range2 ) {
						ri.geometric.range2 = riObjB.geometric.range;
						ri.geometric.vNormal2 = riObjB.geometric.vNormal;
						ri.geometric.vGeomNormal2 = riObjB.geometric.vGeomNormal;
					} else {
						ri.geometric.range2 = riObjA.geometric.range2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
					}
				}
			}
			else
			{
				// Outside both objects.
				// CSG intersection entry = max(A.range, B.range)
				// CSG intersection exit  = min(A.range2, B.range2)
				if(
					(riObjA.geometric.range <= riObjB.geometric.range) &&
					(riObjB.geometric.range <= riObjA.geometric.range2) )
				{
					// A enters first, B enters while inside A -- the
					// composite's entry boundary is wholly B's surface,
					// even though `ri` started as A's whole record above.
					ri = riObjA;
					AdoptCsgSurfacePayload( ri.geometric, riObjB.geometric );
					AdoptCsgSurfaceBindings( ri, riObjB );
					ri.geometric.range = riObjB.geometric.range;
					ri.geometric.vNormal = riObjB.geometric.vNormal;
					ri.geometric.vGeomNormal = riObjB.geometric.vGeomNormal;
					if( riObjA.geometric.range2 <= riObjB.geometric.range2 ) {
						ri.geometric.range2 = riObjA.geometric.range2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
					} else {
						ri.geometric.range2 = riObjB.geometric.range2;
						ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjB.geometric.vGeomNormal2;
					}
				}
				else if(
					(riObjB.geometric.range <= riObjA.geometric.range) &&
					(riObjA.geometric.range <= riObjB.geometric.range2) )
				{
					// B enters first, A enters while inside B -- the
					// composite's entry boundary is wholly A's surface,
					// even though `ri` started as B's whole record above.
					ri = riObjB;
					AdoptCsgSurfacePayload( ri.geometric, riObjA.geometric );
					AdoptCsgSurfaceBindings( ri, riObjA );
					ri.geometric.range = riObjA.geometric.range;
					ri.geometric.vNormal = riObjA.geometric.vNormal;
					ri.geometric.vGeomNormal = riObjA.geometric.vGeomNormal;
					if( riObjB.geometric.range2 <= riObjA.geometric.range2 ) {
						ri.geometric.range2 = riObjB.geometric.range2;
						ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjB.geometric.vGeomNormal2;
					} else {
						ri.geometric.range2 = riObjA.geometric.range2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
					}
				}
			}
		}
		break;
	case CSG_SUBTRACTION:
		// If we don't hit A, then no possible intersection
		if( riObjA.geometric.bHit )
		{
			if( riObjA.geometric.range2 == 0 && riObjB.geometric.range2 == 0 ) {
				// If we are inside both objects, then as long as we hit B before leaving A
				if( riObjB.geometric.range < riObjA.geometric.range ) {
					ri = riObjB;
					ri.geometric.range2 = riObjA.geometric.range;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
					ri.geometric.vGeomNormal = -riObjB.geometric.vGeomNormal;
					// P1-1: `ri` is a whole-record copy of B, so it still
					// carries B's dndu/dndv -- derivatives of B's UN-negated
					// vNormal.  The reported shading normal above is -vNormal,
					// so its derivative must be the derivative of -vNormal,
					// i.e. -dndu/-dndv (same reasoning at the other two
					// CSG_SUBTRACTION entry-flip branches below).  dpdu/dpdv
					// and uv are left untouched -- they still parameterize the
					// SAME point on B's surface; only the reported NORMAL
					// FIELD is negated, so only its derivatives flip with it.
					//
					// REACHABILITY (CsgSurfacePayloadTest.cpp Test 18/19 audit):
					// this branch requires BOTH riObjA.range2==0 AND
					// riObjB.range2==0.  The geometries that report range2=0
					// for an inside-origin hit are all ANALYTIC ones -- the
					// sphere, torus, and quadric/ellipsoid intersectors
					// hard-code it, and SDFGeometry sets it directly -- while
					// TriangleMeshGeometry(Indexed)::IntersectRay ignores
					// bComputeExitInfo entirely, leaving a mesh operand's
					// range2 at its RISE_INFINITY default, which can never
					// read as exactly 0.  SUPERSEDED 2026-08-29
					// (geometry-shading-signals Phase 1): this note used to
					// continue "and NO analytic geometry populates
					// derivatives.valid, so the negation below is UNREACHABLE".
					// Sphere / Ellipsoid / Torus / Cylinder now DO populate
					// derivatives at intersection time (design doc 5.4), and
					// they are exactly the geometries that report range2 == 0
					// for an inside-origin hit (BoxGeometry joined them
					// 2026-09-05: first for an origin ON one of its faces, via
					// DropSelfHitRoot's promoted root, then for any strictly
					// interior origin) -- so this branch is now
					// reachable WITH valid derivatives and the negation below
					// is LIVE, not defensive.  The SDF family reaches it too,
					// through the direct `curvature` field rather than
					// dndu/dndv, which is why that negation is gated separately.
					if( ri.geometric.derivatives.valid ) {
						ri.geometric.derivatives.dndu = -ri.geometric.derivatives.dndu;
						ri.geometric.derivatives.dndv = -ri.geometric.derivatives.dndv;
					}
					// Same re-pairing, for the DIRECT per-hit curvature the SDF
					// family reports instead of dndu/dndv (design doc 5.4).  It
					// is signed against the same normal field, so a reported
					// -vNormal makes a convex operand read as the concave cavity
					// wall it now is -- which is the physically right answer, and
					// is exactly what the dndu/dndv negation above achieves for
					// the geometries that carry the derivative pair.  Its own
					// `curvatureValid` gate is separate from `valid` (an SDF sets
					// one and not the other), so this cannot ride inside the
					// block above.
					if( ri.geometric.derivatives.curvatureValid ) {
						ri.geometric.derivatives.curvature = -ri.geometric.derivatives.curvature;
					}
					// Same re-pairing for the signal-provider's own normal
					// (design doc §6 / geometry-shading-signals Phase 2).
					// `ri` is still a whole-record copy of B, so
					// `signals.nObject` is B's OUTWARD normal (away from B's
					// own solid) -- but the composite is now a cavity wall,
					// and the empty region the viewer stands in is B's
					// INTERIOR, not its exterior.  Leaving nObject un-flipped
					// would make occlusion()/thickness() march into B's solid
					// instead of the cavity; negating it re-pairs the query
					// direction with the flipped vNormal above, exactly as
					// dndu/dndv and curvature already do.
					if( ri.geometric.signals.pProvider ) {
						ri.geometric.signals.nObject = -ri.geometric.signals.nObject;
						// ... and the SENSE of "solid" with it: see
						// SurfaceSignalInfo::bComplementedField.  The normal
						// flip alone repairs the MARCHING signal (thickness);
						// the sign-reading ones (occlusion, convexity) need
						// this.  Toggled so nested subtractions compose.
						ri.geometric.signals.bComplementedField = !ri.geometric.signals.bComplementedField;
					}
					ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
					ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
				}
			} else if( riObjA.geometric.range2 == 0 ) {
				// If we are inside A but not inside B
				// Then as long as we hit B before dHowFar then its all good
				// should always be true
				if( riObjB.geometric.bHit && riObjB.geometric.range < riObjA.geometric.range ) {
					ri = riObjB;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
					ri.geometric.vGeomNormal = -riObjB.geometric.vGeomNormal;
					// P1-1: see the sign-negation rationale at the sibling
					// branch above -- the reported normal here is -B.vNormal,
					// so the derivatives inherited from the `ri = riObjB`
					// whole-record copy (still derivatives of +B.vNormal)
					// must flip sign to match.
					if( ri.geometric.derivatives.valid ) {
						ri.geometric.derivatives.dndu = -ri.geometric.derivatives.dndu;
						ri.geometric.derivatives.dndv = -ri.geometric.derivatives.dndv;
					}
					// Same re-pairing, for the DIRECT per-hit curvature the SDF
					// family reports instead of dndu/dndv (design doc 5.4).  It
					// is signed against the same normal field, so a reported
					// -vNormal makes a convex operand read as the concave cavity
					// wall it now is -- which is the physically right answer, and
					// is exactly what the dndu/dndv negation above achieves for
					// the geometries that carry the derivative pair.  Its own
					// `curvatureValid` gate is separate from `valid` (an SDF sets
					// one and not the other), so this cannot ride inside the
					// block above.
					if( ri.geometric.derivatives.curvatureValid ) {
						ri.geometric.derivatives.curvature = -ri.geometric.derivatives.curvature;
					}
					// Same re-pairing for the signal-provider's own normal --
					// see the sibling branch above for the full rationale.
					if( ri.geometric.signals.pProvider ) {
						ri.geometric.signals.nObject = -ri.geometric.signals.nObject;
						// ... and the SENSE of "solid" with it: see
						// SurfaceSignalInfo::bComplementedField.  The normal
						// flip alone repairs the MARCHING signal (thickness);
						// the sign-reading ones (occlusion, convexity) need
						// this.  Toggled so nested subtractions compose.
						ri.geometric.signals.bComplementedField = !ri.geometric.signals.bComplementedField;
					}
				} else {
					ri = riObjA;
				}
				ri.geometric.range2 = 0;
			} else if( riObjB.geometric.range2 == 0 ) {
				// If we are inside B but not inside A, then as long as we hit A before exiting B
				if( riObjA.geometric.range < riObjB.geometric.range ) {
					ri = riObjB;
					ri.geometric.range2 = riObjA.geometric.range2;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
					ri.geometric.vGeomNormal = -riObjB.geometric.vGeomNormal;
					// P1-1: same sign-negation rationale as the two branches
					// above.
					//
					// REACHABILITY (CsgSurfacePayloadTest.cpp Test 18/19 audit):
					// this branch requires riObjB.range2==0 (origin inside B),
					// which -- same reasoning as the "inside both" branch above
					// -- only an ANALYTIC B (sphere/torus/quadric/SDF) can
					// report.  UPDATED 2026-08-29 (geometry-shading-signals Phase 1):
					// those geometries NOW populate derivatives at intersection time
					// (design doc 5.4), so the negation below is LIVE here -- the
					// earlier "no analytic geometry sets derivatives.valid, therefore
					// UNREACHABLE" reading is superseded.  The vNormal/vGeomNormal
					// negation above was always reachable (sphere-B nested inside a
					// farther sphere-A boundary) and IS regression-tested.
					if( ri.geometric.derivatives.valid ) {
						ri.geometric.derivatives.dndu = -ri.geometric.derivatives.dndu;
						ri.geometric.derivatives.dndv = -ri.geometric.derivatives.dndv;
					}
					// Same re-pairing, for the DIRECT per-hit curvature the SDF
					// family reports instead of dndu/dndv (design doc 5.4).  It
					// is signed against the same normal field, so a reported
					// -vNormal makes a convex operand read as the concave cavity
					// wall it now is -- which is the physically right answer, and
					// is exactly what the dndu/dndv negation above achieves for
					// the geometries that carry the derivative pair.  Its own
					// `curvatureValid` gate is separate from `valid` (an SDF sets
					// one and not the other), so this cannot ride inside the
					// block above.
					if( ri.geometric.derivatives.curvatureValid ) {
						ri.geometric.derivatives.curvature = -ri.geometric.derivatives.curvature;
					}
					// Same re-pairing for the signal-provider's own normal --
					// see the first sibling branch above for the full
					// rationale.
					if( ri.geometric.signals.pProvider ) {
						ri.geometric.signals.nObject = -ri.geometric.signals.nObject;
						// ... and the SENSE of "solid" with it: see
						// SurfaceSignalInfo::bComplementedField.  The normal
						// flip alone repairs the MARCHING signal (thickness);
						// the sign-reading ones (occlusion, convexity) need
						// this.  Toggled so nested subtractions compose.
						ri.geometric.signals.bComplementedField = !ri.geometric.signals.bComplementedField;
					}
					ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
					ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
				}
			} else {
				// Neither A nor B contains the ray origin -- both hits are
				// plain entry/exit interval pairs [range, range2].
				if( !riObjB.geometric.bHit ) {
					// Never hit B at all -- composite is just A.
					ri = riObjA;
				} else {
					// We hit B too.  Test DISJOINTNESS FIRST (P1-b): B's
					// interval [range, range2] either doesn't overlap A's
					// interval at all (B begins after A ends, or B ends
					// before A begins), in which case nothing is carved out
					// of A's visible span and the composite is exactly A --
					// or it does overlap, in which case exactly one of the
					// two overlap branches below applies.  The two overlap
					// conditions were PREVIOUSLY tested before disjointness,
					// so a disjoint B could still satisfy one of them by
					// accident and produce a phantom surface (see the two
					// bugs documented at each branch below, both now
					// unreachable once disjointness is excluded up front).
					if( (riObjB.geometric.range >= riObjA.geometric.range2) ||
						(riObjB.geometric.range2 <= riObjA.geometric.range) )
					{
						ri = riObjA;
					}
					else if( riObjA.geometric.range < riObjB.geometric.range )
					{
						// A enters first and (excluded disjoint above) B's
						// interval overlaps A's -- so B's entry necessarily
						// lands strictly inside A's span.  The carve's near
						// wall (B's entry) becomes the visible surface's
						// exit; this covers B fully contained in A AND B
						// overhanging A's tail (B.range2 > A.range2) --
						// either way the visible portion of A-minus-B still
						// starts at A's entry and ends at B's entry.
						//
						// PRE-FIX BUG: without the disjointness test above,
						// this branch's condition (`A.range < B.range`) is
						// trivially true whenever B is wholly AFTER A ends
						// too, which set range2 = B.range regardless -- an
						// extended phantom interval reaching all the way out
						// to a B that never touches A.
						ri = riObjA;
						ri.geometric.range2 = riObjB.geometric.range;
						ri.geometric.vNormal2 = -riObjB.geometric.vNormal;
						ri.geometric.vGeomNormal2 = -riObjB.geometric.vGeomNormal;
					}
					else if( riObjB.geometric.range2 < riObjA.geometric.range2 )
					{
						// B enters first (or at the same range as A) and
						// (excluded disjoint above) overlaps A's interval,
						// AND exits before A does -- so B's exit necessarily
						// lands strictly inside A's span (B doesn't swallow
						// A whole).  The carve's far wall (B's exit) is the
						// visible surface's entry, extending to A's exit.
						//
						// PRE-FIX BUG: without the disjointness test above,
						// this branch's condition (`B.range2 < A.range2`) is
						// trivially true whenever B is wholly BEFORE A
						// begins too (B ends long before A even starts) --
						// reporting a phantom surface at B's back face in
						// empty space, nowhere near A.
						//
						// EXIT-designated boundary: the composite's entry
						// here is B's EXIT face (B.vNormal2), not B's entry
						// face.  `ri = riObjB` still carries B's auxiliary
						// payload from B's ENTRY hit -- no RISE geometry
						// computes a second payload set for the exit hit,
						// only the exit NORMAL is available.  Recover B's
						// REAL exit-face payload via a reverse probe (P2-e);
						// on a probe miss, fall back to B's entry-hit
						// payload with wire-edge info cleared (it belongs to
						// the wrong face in that fallback).
						//
						// dndu/dndv SIGN, RESOLVED 2026-08-29 (was a disclosed P1-1
						// residual): the probe's payload carries derivatives of the
						// PROBE's own reported normal, while this branch reports
						// -riObjB.vNormal2.  For a direction-independent normal field
						// (every analytic primitive, and the SDF family) those two
						// disagree in sign, which would ship a convex-reading curvature
						// on a concave cavity wall.  That case was UNREACHABLE while only
						// triangle meshes populated derivatives; analytic primitives now
						// do (design doc 5.4), so it is live -- and
						// AdoptCsgExitFacePayloadViaProbe now re-pairs the adopted
						// dndu/dndv (and the SDF's direct `curvature`) against the
						// caller's already-set vNormal via a dot-product test.  See that
						// function's tail.  NOTE the probe-MISS fallback is unchanged and
						// still leaves B's ENTRY-face derivatives in place for an
						// EXIT-face boundary; that is a pre-existing, separately-disclosed
						// residual (the callers clear only bHasWireEdgeInfo), not
						// something this arc introduced.
						ri = riObjB;
						ri.geometric.range = riObjB.geometric.range2;
						ri.geometric.range2 = riObjA.geometric.range2;
						// Reverse the normal
						ri.geometric.vNormal = -riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal = -riObjB.geometric.vGeomNormal2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
						if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectB, riObjB.geometric.range2, riObjB.geometric.vGeomNormal2 ) ) {
							ri.geometric.bHasWireEdgeInfo = false;
						}
					}
					// else: B enters at/before A AND does not exit before A
					// (range2B >= range2A) -- B fully contains A's interval,
					// so A-minus-B is empty across this whole span.  Leave
					// `ri` unhit (matches the pre-existing fallthrough: none
					// of the branches assign `ri`, so `ri.geometric.bHit`
					// stays false from the top of IntersectRay).
				}
			}
		}
		break;
	};

	if( ri.geometric.bHit )
	{
		// Transform the normal back
		//
		// Also capture the PRE-normalization magnitude of the transformed
		// shading normal (norm of M^-T n_obj) -- mirrors Object::IntersectRay's
		// identical capture.  The derivatives block below (THIS CSG level's
		// own transform) reuses both this magnitude and the resulting unit
		// world normal to apply the quotient-rule transform to dndu/dndv;
		// see docs/GEOMETRY_DERIVATIVES.md "World-space transform".
		Vector3 vNormalWorldUnnorm = Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal );
		const Scalar dShadingNormalWorldMag = Vector3Ops::NormalizeMag( vNormalWorldUnnorm );
		ri.geometric.vNormal = vNormalWorldUnnorm;
		ri.geometric.vNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal2 ));
		ri.geometric.vGeomNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal ));
		ri.geometric.vGeomNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal2 ));

		// Shading ONB (P2, mirrors Object::IntersectRay's own honouring of
		// bShadingTangentFromGeometry).  A child geometry (currently only
		// SDFGeometry heightfield mode) can request a COHERENT,
		// world-X-aligned tangent instead of the arbitrary axis
		// `CreateFromW` picks -- see Object::IntersectRay for the full
		// rationale.  Before this fix, CSGObject unconditionally called
		// CreateFromW here regardless of the flag, so an anisotropic
		// `tangent_rotation` on a heightfield SDF wrapped in CSG rotated
		// from a DIFFERENT base tangent than the same geometry rendered
		// standalone.  Project world-X into the now-world-space shading-
		// normal plane and hand it to CreateFromWU, falling back to
		// world-Y when world-X is (near-)parallel to the normal so the
		// projection never degenerates.
		if( ri.geometric.bShadingTangentFromGeometry ) {
			const Vector3& n = ri.geometric.vNormal;
			Vector3 t;
			bool bHaveSuppliedTangent = false;

			// C2 (mirrors Object::IntersectRay's identical block): a geometry that
			// ALSO supplies a real fibre tangent (currently only HairGeometry, via
			// bHasShadingTangent) gets it promoted one more level to THIS CSG
			// object's world/parent space -- forward matrix, not inverse-transpose,
			// same reasoning as vTangent below (a tangent is a direction ALONG the
			// surface, not a normal).  If the tangent arrived via a nested child
			// Object::IntersectRay it is already promoted one level (exactly like
			// vTangent), so this transform brings it the rest of the way.  The
			// result is projected into the (now world-space) shading-normal plane,
			// mirroring the world-X projection this branch already does.
			// Degenerate (near-parallel to the normal) falls back to that legacy
			// world-X projection below, so a pathological hit never produces a NaN
			// ONB.
			//
			// Two DISTINCT degeneracies (mirrors Object::IntersectRay's identical
			// split): tWorld itself near-zero means THIS level's transform was
			// singular along the tangent's incoming direction -- the promoted
			// value is garbage, not just locally unusable, so clear
			// bHasShadingTangent and skip the write-back rather than hand a
			// further-nested CSG parent a "valid" zero vector.  Only tProj
			// near-zero (tWorld valid, parallel to the normal) still writes
			// tWorld back -- a parent's own transform may un-degenerate it --
			// and falls back to the legacy world-X projection for THIS level's
			// ONB alone.
			if( ri.geometric.bHasShadingTangent ) {
				const Vector3 tWorld = Vector3Ops::Normalize(
					Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.vShadingTangent ) );
				if( Vector3Ops::SquaredModulus( tWorld ) < NEARZERO ) {
					ri.geometric.bHasShadingTangent = false;
				} else {
					// Write-back (not just a local variable), mirroring
					// Object::IntersectRay's identical write-back: a CSG nested one
					// level deeper (CSG-of-CSG) needs THIS level's promotion applied
					// to the field itself, not just consumed locally for the onb.
					// Written UNPROJECTED -- the nesting invariant composes raw
					// transforms one level at a time; the projection below into
					// THIS level's shading-normal plane is a purely local ONB
					// concern.
					ri.geometric.vShadingTangent = tWorld;
					const Vector3 tProj = tWorld - n * Vector3Ops::Dot( n, tWorld );
					if( Vector3Ops::SquaredModulus( tProj ) >= NEARZERO ) {
						t = tProj;
						bHaveSuppliedTangent = true;
					}
				}
			}

			if( !bHaveSuppliedTangent ) {
				t = Vector3( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );	// (1,0,0) - n*dot(n,(1,0,0))
				if( Vector3Ops::SquaredModulus( t ) < NEARZERO ) {
					t = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );	// (0,1,0) - n*dot(n,(0,1,0))
				}
			}
			ri.geometric.onb.CreateFromWU( n, t );	// W = n (fixed); V = norm(W x t), U = V x W

			// P1 fix (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round), mirrors
			// Object::IntersectRay's identical correction -- see that
			// function's own (longer) comment for the full two-case
			// rationale.  Summary: for a real supplied tangent, V =
			// cross(n_world, t_world) mixes an inverse-transpose-promoted
			// normal with a forward-promoted tangent, the same mismatched
			// pair `bitangentSign *= m_tangentFrameSign` (below) corrects for
			// NormalMap's cross(N,T) bitangent -- without this, a mirrored
			// CSG operand's (or the composite's own mirrored transform's)
			// `tangent_rotation` rotates the opposite sense from an
			// unmirrored instance of the same child.  For the legacy
			// world-X fallback (SDFGeometry heightfield mode), there is no
			// forward/inverse-transpose mismatch in THIS `t`, but flipping V
			// here too is what keeps the SDFGeometry-heightfield /
			// cartesian_disk pairing's `tangent_rotation` sense coherent
			// when either half is mirrored, matching what the mesh half
			// (the supplied-tangent branch) gets once it carries a real UV
			// tangent.  U is untouched -- it is the promoted tangent
			// direction itself, which needs no correction.  A no-op
			// (m_tangentFrameSign == +1) for an orientation-preserving transform;
			// the legacy CreateFromW `else` branch is unaffected either way.
			if( m_tangentFrameSign < Scalar( 0 ) ) {
				ri.geometric.onb.FlipV();
			}
		} else {
			ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
		}

		// Transform the per-vertex tangent (P2-d) from THIS CSG object's
		// local frame to world space -- exactly like vNormal above, and
		// mirroring Object::IntersectRay's own tangent-promotion block.
		// A child's Object::IntersectRay already promoted its tangent from
		// ITS OWN local frame up to the frame it was called in (this CSG's
		// local frame), the same way it already promotes vNormal / vTangent
		// / derivatives one level per nesting -- so by the time any switch
		// branch above copies a child record into `ri` (directly via
		// `ri = riObjX`, or re-adopted via AdoptCsgSurfacePayload / the
		// exit-face probe), vTangent/derivatives sit in THIS CSG's local
		// frame, one promotion short of world space.  Tangents transform
		// with the forward matrix (like positions / dpdu), NOT inverse-
		// transpose, since they are surface-tangent directions, not
		// normals.  bitangentSign picks up THIS CSG object's own
		// orientation-reversal sign (m_tangentFrameSign, computed once in
		// FinalizeTransformations), composing with whatever sign flip each
		// nested child already applied for its own transform.
		if( ri.geometric.bHasTangent ) {
			ri.geometric.vTangent = Vector3Ops::Normalize(
				Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.vTangent ) );
		}
		// doc 89 slice C: UNCONDITIONAL, outside the bHasTangent gate -- the sibling
		// of the same move in Object::IntersectRay, for the same reason.  The sign
		// describes THIS transform's handedness, and NormalMap's derivative fallback
		// (no imported TANGENT, dpdu-derived frame) is the consumer that needs it.
		// It initialises to 1.0 and each nesting level multiplies its own, so the
		// composition through a CSG chain is unchanged for the tangented case.
		ri.geometric.bitangentSign *= m_tangentFrameSign;

		// Transform surface derivatives (P2-d) from THIS CSG object's local
		// frame to world space -- mirrors Object::IntersectRay's derivatives
		// block exactly (see that function's comment for the full
		// derivation).  dpdu / dpdv are tangent vectors (forward transform).
		// dndu / dndv are derivatives of the SHADING normal (vNormal,
		// already renormalized to THIS CSG level's world/parent space
		// above) and need the quotient-rule transform, not a plain
		// inverse-transpose:
		//   dn_w/du = (I - n_w n_w^T) . (M^-T dndu_local) / ||M^-T n_local||
		// using THIS level's own m_mxInvTranspose and the
		// dShadingNormalWorldMag/vNormal captured just above -- CSG nesting
		// composes correctly because each level applies its own transform
		// to the level-local normal in turn, exactly like the tangent /
		// vNormal promotions elsewhere in this function.  This composition
		// claim depends on `ri.geometric.derivatives` already being paired
		// with the SAME normal field `ri.geometric.vNormal` reports at the
		// point this block runs -- true for a plain `ri = riObjX` passthrough,
		// but NOT automatically true wherever a branch above reports a
		// NEGATED normal (CSG_SUBTRACTION's three entry-flip branches:
		// "inside both", "inside A only", "inside B only").  Those branches
		// now negate `ri.geometric.derivatives.dndu/dndv` alongside vNormal
		// (P1-1: a whole-record `ri = riObjB` copy carries B's UN-negated
		// derivatives, which are derivatives of +vNormal, not the reported
		// -vNormal) specifically so this level's quotient-rule transform
		// below still differentiates the correct (already-negated) normal
		// field.  Without that per-branch negation this composition claim
		// would be false for every subtraction cavity wall, not just
		// technically imprecise.
		if( ri.geometric.derivatives.valid ) {
			ri.geometric.derivatives.dpdu = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdu );
			ri.geometric.derivatives.dpdv = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdv );

			if( dShadingNormalWorldMag > NEARZERO ) {
				const Vector3& n_w = ri.geometric.vNormal;
				const Scalar invMag = Scalar(1.0) / dShadingNormalWorldMag;

				const Vector3 dndu_lin = Vector3Ops::Transform(
					m_mxInvTranspose, ri.geometric.derivatives.dndu );
				ri.geometric.derivatives.dndu =
					( dndu_lin - n_w * Vector3Ops::Dot( n_w, dndu_lin ) ) * invMag;

				const Vector3 dndv_lin = Vector3Ops::Transform(
					m_mxInvTranspose, ri.geometric.derivatives.dndv );
				ri.geometric.derivatives.dndv =
					( dndv_lin - n_w * Vector3Ops::Dot( n_w, dndv_lin ) ) * invMag;
			} else {
				// THIS level's transform is ill-conditioned or degenerate
				// along the normal direction -- mirrors Object::IntersectRay's
				// identical guard (see its comment for why NormalizeMag's
				// OWN internal guard, mag > 0.0, is looser than this NEARZERO
				// gate: `ri.geometric.vNormal` above may already be a unit,
				// just ill-conditioned-direction, vector rather than a literal
				// zero vector here).  Either way there is no well-defined unit
				// world shading-normal to differentiate against; mark invalid
				// rather than divide by ~0.
				ri.geometric.derivatives.valid = false;
			}
		}

		// WORLD-MEASURE FOLD for the two curvature-facing scalars -- the exact
		// mirror of Object::IntersectRay's block (see it for the full
		// rationale): scaleHint is a LENGTH (multiply by this level's
		// |det M|^(1/3)), curvature is a 1/LENGTH (divide by it), both sit
		// OUTSIDE the `derivatives.valid` gate because an SDF operand sets
		// `curvatureValid` without setting `valid`.  CSG nesting composes for
		// the same reason the normal-field promotions above do: each level
		// applies its OWN factor once to whatever the level below already
		// promoted.
		if( m_worldLinearScale > Scalar( 0 ) ) {
			ri.geometric.derivatives.scaleHint *= m_worldLinearScale;
			if( ri.geometric.derivatives.curvatureValid ) {
				ri.geometric.derivatives.curvature /= m_worldLinearScale;
			}
		} else {
			ri.geometric.derivatives.curvatureValid = false;
		}

		// WORLD-MEASURE PROMOTION for txFootprint -- the exact mirror of
		// Object::IntersectRay's block (see it for the affine line∩plane
		// commutation argument that makes this promotion EXACT for any
		// linear map, and docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.4).
		// The CSG-nesting invariant is UNCHANGED, only the per-level
		// operator: `AdoptCsgSurfacePayload` copies the winning child
		// operand's txFootprint verbatim, already promoted by THAT child
		// Object's own forward map into THIS object's local frame, so THIS
		// level composes by applying its OWN forward map once more -- the
		// same "each level applies its own map once to whatever the level
		// below already promoted" rule scaleHint/curvature follow.  Composing
		// forward maps is what makes the nesting exact rather than merely
		// consistent: M_outer · M_inner is the true object-to-world map.
		//
		// `objectWidth` is deliberately NOT touched here, and that is
		// the CORRECT answer rather than an omission (2026-09-06).  A
		// CSG hit's `ptObjIntersec` is the winning CHILD operand's own
		// object-space point -- `AdoptCsgSurfacePayload` copies it, and
		// this whole txFootprint struct, untransformed -- so `Po` is in
		// the child's frame at every nesting depth.  The child's
		// `Object::IntersectRay` captured `objectWidth` in exactly that
		// frame.  Promoting it here would break the pairing that makes
		// `fbm(Po*k, ...)` filter correctly; leaving it alone preserves
		// it.  This is the one field of the record where the CSG frame
		// mismatch documented on `pmxWorldToObject` below works OUT,
		// because the field it must agree with is mismatched the same
		// way.
		if( ri.geometric.txFootprint.widthValid ) {
			const Vector3 dx = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdx );
			const Vector3 dy = Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.txFootprint.dpdy );
			ri.geometric.txFootprint.dpdx = dx;
			ri.geometric.txFootprint.dpdy = dy;
			ri.geometric.txFootprint.worldWidth =
				Scalar(0.5) * ( Vector3Ops::Magnitude( dx ) + Vector3Ops::Magnitude( dy ) );
		}

		// Compute the intersection in world space
		ri.geometric.ptIntersection = Point3Ops::Transform( m_mxFinalTrans,	ri.geometric.ray.PointAtLength( ri.geometric.range - SURFACE_INTERSEC_ERROR ) );
		ri.geometric.ptExit = Point3Ops::Transform( m_mxFinalTrans,	ri.geometric.ray.PointAtLength( ri.geometric.range2 + SURFACE_INTERSEC_ERROR ) );

		// The child's Object::IntersectRay landed ptWireNearestEdge in
		// THIS CSG object's local frame (it transforms by the child's own
		// forward matrix, exactly like ptIntersection) -- promote it to
		// world with the same transform ptIntersection gets above, or the
		// wireframe shader would measure a distance between two points in
		// different frames on any placed csg_object.
		if( ri.geometric.bHasWireEdgeInfo ) {
			ri.geometric.ptWireNearestEdge = Point3Ops::Transform(
				m_mxFinalTrans, ri.geometric.ptWireNearestEdge );
		}

		// Re-compute the ranges in world space
		ri.geometric.range = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptIntersection, orig.origin ) );

		if( ri.geometric.range2 != 0 ) {
			ri.geometric.range2 = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptExit, orig.origin ) );
		}

		if( pMaterial ) {
			ri.pMaterial = pMaterial;
		}

		if( pModifier ) {
			ri.pModifier = pModifier;
		}

		// WORLD -> OBJECT step map: honestly UNKNOWN for a CSG hit.
		//
		// `ptObjIntersec` on a composite hit is the CHILD operand's own
		// object-space point -- AdoptCsgSurfacePayload copies it
		// untransformed and its comment names the resulting frame
		// mismatch as a deliberate, pre-existing gap -- so the map from
		// world into THAT frame is the child's inverse composed with this
		// composite's inverse (and with every enclosing composite's, under
		// nesting).  No stored member holds that product and a `const
		// Matrix4*` cannot express it.  The child's Object::IntersectRay
		// stamped its OWN inverse a moment ago, which is wrong by exactly
		// this level's transform; stamping `m_mxInvFinalTrans` here would
		// be wrong by exactly the child's.  Clear it, and let the consumer
		// take its documented degraded path (move the object-space point
		// by the world step and warn once) rather than silently trusting a
		// matrix that is wrong by a transform.
		ri.geometric.pmxWorldToObject = 0;

		if( pShader ) {
			ri.pShader = pShader;
		}

		if( pRadianceMap ) {
			ri.pRadianceMap = pRadianceMap;
		}

		// The composite CSG object is the refractive/shading identity seen by
		// the rest of the renderer. Reporting a hidden child object here breaks
		// IOR-stack tracking for dielectric CSG paths.
		ri.pObject = this;
	}

	// Restore the old ray
	ri.geometric.ray = orig;
}

bool CSGObject::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool , const bool ) const
{
	if( !pObjectA || !pObjectB ) {
		GlobalLog()->PrintSourceWarning( "CSGObject::IntersectRay_IntersectionOnly:: No subobjects for this CSG object, ignoring hit request", __FILE__, __LINE__ );
		return false;
	}

	// Bring the ray into our frame, but use our own copy
	Ray		orig = ray;

	orig.origin = Point3Ops::Transform( m_mxInvFinalTrans, ray.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- the direction-true
	// world-to-local distance factor used below (P1 fix, mirrors
	// Object::IntersectRay / Object::IntersectRay_IntersectionOnly).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, ray.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	orig.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// factor converts a WORLD-frame distance limit (dHowFar) into the
	// local-frame traversal limit used by the switch below.  MUST be the
	// magnitude of the TRANSFORMED RAY DIRECTION (dirLocalMag, captured
	// above) -- NOT an arbitrary +X-axis probe (P1 fix).  Under
	// non-uniform scale, |M^-1 * v| depends on which direction v points; a
	// +X-only factor mis-scales the limit for every ray not travelling
	// along local +X, letting a CSG shadow ray leak past the light (or
	// wrongly occlude beyond it) whenever the composite carries a
	// non-uniform scale.  Guard a degenerate transform that collapses this
	// direction to ~0 by falling back to an unscaled factor of 1.0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = RISE_INFINITY;

	// We can't go farther than infinity, so in this case only reduce the
	// length, never extend -- see Object::IntersectRay for why this guard
	// is about the RISE_INFINITY sentinel's magnitude and stays correct
	// regardless of how `factor` is derived.
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	RayIntersection		riObjA( orig, nullRasterizerState );
	RayIntersection		riObjB( orig, nullRasterizerState );

	// For CSG objects, we still need the ranges, even if it is for intersection only!
	//
	// bComputeExitInfo MUST be true (P1-c): a child Object::IntersectRay
	// only re-expresses range2 in the CALLER's frame (via ptExit -> world
	// -> magnitude-from-origin) inside its `if( bComputeExitInfo )` block
	// -- with bComputeExitInfo=false, range2 is left exactly as the
	// underlying IGeometry wrote it, which is a raw parametric distance
	// in the CHILD's own local-frame ray-parameter units.  The switch
	// below compares that value directly against `range` (which IS
	// always re-expressed in the caller's/CSG-local frame, unconditionally)
	// and against the OTHER child's range/range2 -- a mismatch of units
	// whenever the operand carries a non-uniform or non-unit uniform
	// scale.  The range2==0 "origin is inside" sentinel is unaffected
	// either way (set directly by the geometry, untouched by the
	// bComputeExitInfo-gated re-expression), so this fix costs the extra
	// exit-normal/exit-point computation but changes no sentinel
	// semantics.  Matches the main IntersectRay above, which already
	// passes true for both children.
	//
	// dHowFar2, NOT dHowFar (P1 fix): dHowFar2 above already converted the
	// caller's-frame limit into THIS CSG's local frame -- exactly the
	// frame the operand calls expect their own limit expressed in (see
	// the `factor` derivation above and the mirrored comment on the main
	// IntersectRay).  Passing the raw caller-frame dHowFar here silently
	// skipped that conversion on the shadow-ray path: under a compressing
	// CSG transform an in-range occluder could be missed (light leak);
	// under an expanding one, occlusion could wrongly extend past the
	// light.
	pObjectA->IntersectRay( riObjA, dHowFar2, true, true, true );
	pObjectB->IntersectRay( riObjB, dHowFar2, true, true, true );

	// Do different things depending on the type of CSG operation
	switch( op )
	{
	default:
	case CSG_UNION:
		if( riObjA.geometric.bHit || riObjB.geometric.bHit ) {
			return ((riObjA.geometric.range < dHowFar2) || (riObjB.geometric.range < dHowFar2));
		}
		break;
	case CSG_INTERSECTION:
		// If both the objects don't intersect, then there can't be an
		// intersection
		if( riObjA.geometric.bHit && riObjB.geometric.bHit ) {
			const bool insideA = (riObjA.geometric.range2 == 0);
			const bool insideB = (riObjB.geometric.range2 == 0);

			if( insideA && insideB ) {
				// Inside both — inside the CSG intersection.
				// The exit is at whichever boundary we reach first.
				const Scalar exitRange = (riObjA.geometric.range <= riObjB.geometric.range)
					? riObjA.geometric.range : riObjB.geometric.range;
				return (exitRange < dHowFar2 && exitRange > 0);
			} else if( insideA && !insideB ) {
				// Inside A, outside B. Enter CSG at B's entry if before A's exit.
				if( riObjB.geometric.range < riObjA.geometric.range ) {
					return (riObjB.geometric.range < dHowFar2 && riObjB.geometric.range > 0);
				}
			} else if( !insideA && insideB ) {
				// Outside A, inside B. Enter CSG at A's entry if before B's exit.
				if( riObjA.geometric.range < riObjB.geometric.range ) {
					return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
				}
			} else {
				// Outside both. CSG entry = max(A.range, B.range).
				if( (riObjA.geometric.range <= riObjB.geometric.range) && (riObjB.geometric.range <= riObjA.geometric.range2) ) {
					return (riObjB.geometric.range < dHowFar2 && riObjB.geometric.range > 0);
				}
				else if( (riObjB.geometric.range <= riObjA.geometric.range) && (riObjA.geometric.range <= riObjB.geometric.range2) ) {
					return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
				}
			}
		}
		break;
	case CSG_SUBTRACTION:
		// If we don't hit A, then no possible intersection
		if( riObjA.geometric.bHit )
		{
			if( riObjA.geometric.range2 == 0 && riObjB.geometric.range2 == 0 ) {
				// If we are inside both objects, then as long as we hit A before leaving B
				// B before we exit A we are ok
				return (riObjB.geometric.range < riObjA.geometric.range && riObjB.geometric.range < dHowFar2 );
			} else if( riObjA.geometric.range2 == 0 ) {
				// If we are inside A but not inside B
				// Then as long as we hit B before dHowFar2 then its all good
				// should always be true
				return true;
//				return (riObjB.geometric.range < dHowFar2 );
			} else if( riObjB.geometric.range2 == 0 ) {
				// If we are inside B but not inside A, then as long as we exit B before dHowFar2 its all good
				return (riObjB.geometric.range < dHowFar2 );
			} else {
				// If we never hit B, or if B begins after A ends, or if B ends before A can begin
				// then we only have A
				if( !riObjB.geometric.bHit ||
					(riObjB.geometric.range >= riObjA.geometric.range2) || 
					(riObjB.geometric.range2 <= riObjA.geometric.range) ) {
					return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
				} else {
					if( riObjA.geometric.range < riObjB.geometric.range && riObjA.geometric.range2 != 0 ) {
						return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
					}
					else if( riObjB.geometric.range2 < riObjA.geometric.range2 ) {
						return (riObjB.geometric.range2 < dHowFar2 && riObjB.geometric.range2 > 0);
					}
				}
			}
		}
		break;
	};

	return false;
}

void CSGObject::Realize() const
{
	// Our two operands are registered in the ObjectManager but world-INVISIBLE
	// (AssignObjects sets SetWorldVisible(false)), so RayCaster's realize pass —
	// which walks EnumerateObjects, filtering on IsWorldVisible() — never reaches
	// them.  Cascade explicitly.  Children dispatch virtually: a plain Object
	// realizes its geometry; a nested CSGObject recurses.  Const + idempotent,
	// so this is safe to call on every render.
	if( pObjectA ) {
		pObjectA->Realize();
	}
	if( pObjectB ) {
		pObjectB->Realize();
	}
}

void CSGObject::ResetRuntimeData() const
{
	Object::ResetRuntimeData();
	if( pObjectA ) {
		pObjectA->ResetRuntimeData();
	}
	if( pObjectB ) {
		pObjectB->ResetRuntimeData();
	}
}

// Any unit vector perpendicular to the unit `n`, built from the coordinate axis
// `n` is least aligned with -- so the cross product's magnitude is never below
// sqrt(2/3) and the normalization is always well-conditioned.  Used to give the
// ownership ray below a TRANSVERSE jitter.
static Vector3 AnyPerpendicularUnit_( const Vector3& n )
{
	const Scalar ax = std::fabs( n.x ), ay = std::fabs( n.y ), az = std::fabs( n.z );
	const Vector3 helper = ( ax <= ay && ax <= az ) ? Vector3( 1, 0, 0 )
	                     : ( ay <= az )             ? Vector3( 0, 1, 0 )
	                                                : Vector3( 0, 0, 1 );
	Vector3 t = Vector3Ops::Cross( n, helper );
	const Scalar m = Vector3Ops::Magnitude( t );
	if( !( m > NEARZERO ) ) {
		return Vector3( 0, 0, 1 );			// unreachable for a unit `n`; defensive
	}
	return Vector3( t.x / m, t.y / m, t.z / m );
}

// IObject::SelfHitRootFloor for a composite -- see the header's note.
//
// Frame bookkeeping (the whole content of this function): the arguments are in
// THIS composite's local frame; a child geometry's gate is expressed in the
// CHILD's local frame.  For a child whose inverse transform is M^-1:
//
//   * the origin maps as a point;
//   * the UNIT direction maps to M^-1 d, whose LENGTH `s` is child-local units
//     per unit of ours along that direction -- so it must be re-normalized
//     before the query (the interface takes a unit direction) and the child's
//     answer, a range in child units, comes back to ours by DIVIDING by `s`;
//   * the normal maps with the transpose of the FORWARD matrix (normals go
//     world->local that way), re-normalized.
//
// A child whose transform collapses this direction (s ~ 0) is skipped rather
// than divided by: it cannot be re-hit along this ray at all, so it contributes
// no requirement.  Recursion terminates on the operand tree, which AssignObjects
// keeps acyclic.
//
// OWNERSHIP FILTER (adversarial review of 4b141ad3, P2-1).  Taking the max over
// BOTH operands unconditionally lets an operand that has nothing to do with the
// face being re-hit set the composite's floor.  Measured (CsgProbeFloorTest
// Test 2): a nested CSG_UNION of a box (half-extent 1, own floor 4.014e-12)
// with a triangle-mesh lobe parked 1e4 units away (bbox-corner floor 3.008e-8
// -- correct FOR THAT MESH, and 7494x the box's) made the parent claim the
// MESH's figure on the BOX's face.  The probe doubles its margin and adds 10 %
// slack, so the same-face acceptance window went to ~1.3e-7 -- ~250x WIDER than
// the 5e-10 decoy gap CsgSurfacePayloadTest Test 15 exists to guard, i.e.
// straight back into the decoy-payload adoption trade that test bounds.
//
// The fix is to charge only operands that could actually own the face at
// `localOrigin`: stand off on the INSIDE of the face (along -normal) and fire a
// short ray back out through it.  A child whose surface passes through that
// window owns (or shares) the face and its gate is a real requirement; a child
// that does not is 1e4 units away and its gate is not.  The window straddles
// the face so a child whose surface sits fractionally either side of the
// reported point still counts, and both face flags are passed because the owner
// may present either facing (a SUBTRACTION's carve wall is the subtrahend's
// OUTWARD face reported inverted).
//
// THE WINDOW IS PER CHILD (adversarial review of 7923bf2f, P2-1).  A single
// `delta = 1e-6 * (1 + |localOrigin|_1)` window is smaller than some children's
// OWN self-hit floor, and such a child can never be hit inside it -- so the one
// operand whose gate matters most is exactly the one the filter drops.
// Measured: `CSG_UNION(SDF sphere R=4, box 2x2x8)` queried at their coincident
// -Z face reported the box's 4.06e-12 and dropped the SDF's 2.77e-4, 68 MILLION
// times under (delta there is 5e-6, well inside March's 2*m_eps step-off band,
// so the probe ray is marched straight past the sphere and reports a miss); a
// thin large torus R=1000 r=0.05 is the same shape of failure at 4e-3 vs 1e-3.
// The composite then stands its exit probe off less than the SDF needs, the box
// wins the re-hit inside the window, and the union adopts the WRONG operand's
// payload on a face the SDF may own -- the very defect 4b141ad3 exists to stop,
// re-introduced one layer up.
//
// So the window is `max(delta, 2 * floorChild)`, using the floor the child has
// already been asked for.  The 2x is not decoration: a standoff of exactly
// `floorChild` puts the surface at the SMALLEST ray parameter the child accepts,
// a knife edge in floating point, and for an SDF the step-off band fires on
// `|Map| <= 2*m_eps` INCLUSIVELY, so a standoff of exactly the claimed floor is
// still inside it and still marches past.  It matches the headroom the probe
// itself uses on the same number.
//
// The alternative the review offered -- treat `floorChild >= delta` as an
// automatic owner -- is REJECTED: it is blanket rather than geometric, and it
// re-opens the very defect this filter closed.  Test 2's far mesh claims 3.008e-8
// against a delta of ~2e-6, so it survives today; park the same mesh at 1e6
// instead of 1e4 and its floor is 3e-6 > delta, and a sibling with nothing to do
// with the face would be adopted as an owner on the strength of being big and
// far away.  Widening the window keeps the test geometric -- the child's surface
// must still actually pass through it.
//
// TANGENCY: THE TRANSVERSE JITTER.  A single ray cannot settle a face two
// operands share exactly.  Measured on two boxes meeting along an edge (a unit
// box spanning [-1,1]^3 and a 4e6-long box spanning x >= 1, y >= 1), queried on
// the shared edge with the long box's +X face normal: at exactly y = 1 the long
// box's slab test MISSES and the unit box hits, and a hair below (y = 1 - 1e-15)
// the verdict flips.  Neither reversing the ray's orientation nor lengthening it
// a millionfold cures that -- both were measured and both still miss -- because
// the coplanarity is in a TRANSVERSE axis, which nothing done ALONG the ray can
// touch.
//
// So a child the ray misses is given a second chance from rays whose ORIGIN is
// displaced TRANSVERSELY -- by `window` along each of +-t1, +-t2, an orthonormal
// pair perpendicular to the face normal.  Both signs on both axes, because which
// side of the shared edge lies inside the grazed operand is not known here: at
// the measured edge only the -Y displacement enters the long box, the +Y one
// leaves it.  On that geometry every displacement from 1e-15 to 1e-6 finds the
// long box; `window` is used so the jitter carries the same scale-relative size
// as the test it belongs to, and so the whole probe stays inside a
// `window`-sized neighbourhood of the queried point.
//
// THE FOUR AXIAL DISPLACEMENTS ARE NOT ENOUGH AT A VERTEX (adversarial review of
// 384e3752, P1-1).  An EDGE is one shared plane and the axial displacements
// straddle it, but at a shared VERTEX two boundary planes meet, and each axial
// displacement lands EXACTLY ON one of them or steps outside the operand
// altogether -- so every retry grazes and the co-owner is dropped again.
// Measured: the same 4e6-long box (x in [-4e6+1, 1], y and z in [-1,1]) with the
// small box lifted to [1,3]^3 so the two share only the corner (1,1,1), queried
// there with the long box's +X normal.  With n = +X the pair is t1 = +Z,
// t2 = -Y, so the four displacements are +-Y and +-Z: -Y lands at z = 1 exactly,
// -Z lands at y = 1 exactly, and +Y / +Z leave the box.  The long box's 2.84e-8
// was dropped for the small box's 4.01e-12 -- 7081x under -- while the EDGE
// control at (1,1,0) was charged correctly.
//
// The cure is the four DIAGONALS, `(+-t1 +- t2) / sqrt(2) * window`: the
// eight samples then sit 45 degrees apart around the point, so any co-owner
// whose transverse cross-section spans MORE than 45 degrees of arc receives
// a strictly interior sample -- a 90-degree box corner always does (at
// the measured corner, `(-t1 + t2) / sqrt(2)` = (0,-1,-1)/sqrt(2) puts the
// origin at y < 1 AND z < 1, inside the long box's cross-section, and the ray
// hits its +X face).  A co-owner narrower than 45 degrees (an acute
// INTERSECTION of two slabs meeting at the point, measured: a 43-degree arc
// is missed by all eight, a 49-degree one is found) is still dropped -- the
// graceful entry-payload fallback, accepted and recorded in
// docs/CLOTH_FABRIC_DESIGN.md debt 25.  Eight directions in all, still all
// at radius `window`, so the neighbourhood argument above is unchanged.
// They are fired only after the
// four axial retries have missed, so nothing that used to be settled cheaply
// pays for them.
//
// A cap on `window` (adversarial review of 384e3752, P1-2).  `window` is
// `2 * floorChild`, so a child whose floor is pathological fires an ownership
// ray long enough to reach an operand that has nothing to do with the face, and
// charges that floor there.  Measured: an SDF part authored with a zero scale
// axis has a 1e-9 Lipschitz shrink and claimed 5.66e4 on a field 2.83 units
// across; inside `UNION(box, that field)` the union reports 5657 (its own
// SelfHitRootFloor cap), and `SUBTRACTION(box [-1,1]^3, that union)` -- a
// subtraction is bounded by operand A, so the composite is 3.46 across -- fired
// an 11315-wide window that found the union's own box 8 units past the queried
// face and put 5657 on operand A's face, 1.4e15x A's real 4.01e-12.
//
// A window wider than the whole composite cannot be discriminating, so it is
// capped at the composite's own local-frame bounding-box DIAGONAL.  (The
// pathology is also cured at its source, in `SDFGeometry::SelfHitRootFloor`'s
// own diagonal cap and in that parser's scale clamp; this is the caller-side
// backstop, and it bounds ANY geometry's floor pathology, not just an SDF's --
// above it is a nested COMPOSITE that carries the number.)  An unbuilt or
// unbounded box -- the +-DBL_MAX default an empty mesh reports, or an infinite
// plane operand -- disables the cap rather than poisoning it, exactly as
// `Geometry::BoundingBoxRootFloor` does.  The box costs one `getBoundingBox` per
// call (which recurses on a nested operand), paid once for both children rather
// than per retry, on a path that already fires up to nine rays per child.
//
// This REPLACES an earlier backstop that asked whether `localOrigin` lay within
// `window` of the child's axis-aligned BOUNDING-BOX SURFACE.  That test was not
// geometric: a bounding box is not a surface.  Measured -- `CSG_UNION(SDF sphere
// R=4 at the origin, box slab x in [2.9,4.9], z in [-4.201,-4.001])` queried at
// (3.9, 0, -4.001) with the slab's +Z normal.  The slab's own face lies on the
// SDF's PADDED AABB plane z = -4.001, so the shell test charged the SDF's
// 2.77e-4 on a face whose nearest SDF surface is 1.587 WORLD UNITS away -- the
// composite floor came out 6.8e7x the owning slab's 4.06e-12.  End to end that
// widened the exit probe's same-face window far enough to adopt a decoy face
// 1e-4 past the real one.  A torus's AABB is a solid cube of +-(R+r) and an SDF's
// is padded past its own field, so the shell of either can pass arbitrarily far
// from anything it actually bounds.  The jitter cannot do that: it still has to
// HIT the child, within `window` of the point.
//
// The ray is fired in THIS composite's own local frame, NOT a child's:
// `IObjectPriv::IntersectRay_IntersectionOnly` takes the ray in its CALLER's
// frame and applies its own inverse transform internally (see Object::
// IntersectRay_IntersectionOnly), exactly as the main IntersectRay hands its
// CSG-local ray to both operands.  Only the FLOOR query needs the child frame.
//
// If NO child claims the face -- which should not happen for a genuine exit
// face, but can if `localNormal` is unusable (a geometry that never set
// vGeomNormal2) or the face sits on a seam the short probe misses -- fall back
// to the max over both, i.e. exactly the previous behaviour.  Over-stating is
// the safe direction for the contract; the filter only removes a requirement
// when it can positively show the requirement is someone else's.
Scalar CSGObject::SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const
{
	// Seeded at ZERO, deliberately NOT at IObject's generic
	// `NEARZERO * (1 + |localOrigin|_1)` default: a composite has no surface
	// of its own, so it has no gate of its own to contribute, and charging
	// the generic one would re-import exactly the TRANSVERSE-coordinate
	// coupling the probe's r4 fix removed -- at the world X = 1e12 of
	// CsgSurfacePayloadTest's Test 14 that default is a full WORLD UNIT,
	// which inflates the probe margin (and with it the same-face acceptance
	// radius) far enough to adopt a decoy face's payload.  Every real
	// requirement comes from a leaf, and a leaf reports its own gate against
	// the coordinate that actually matters to it (a box reads only the face
	// axis; see BoxGeometry::SelfHitRootFloor).
	Scalar worst = Scalar(0);			// max over children that OWN the face
	Scalar worstAny = Scalar(0);		// max over every child -- the fallback
	bool anyOwner = false;

	// Base containment window, in THIS composite's frame (see the note above).
	// `delta = 1e-6 * (1 + |localOrigin|_1)` is scale-relative for the same
	// reason every gate here is -- an absolute epsilon is below the
	// representable granularity of a coordinate at 1e12 (Test 14's own scale)
	// and the probe would degenerate to a zero-length ray.  Each child widens
	// it to its own floor below.
	const Scalar delta = Scalar(1e-6) * ( Scalar(1) +
		std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) );
	const Scalar nMagLocal = Vector3Ops::Magnitude( localNormal );
	const bool canTestOwnership = ( nMagLocal > NEARZERO ) && std::isfinite( delta );
	const Vector3 nUnit = canTestOwnership
		? Vector3( localNormal.x / nMagLocal, localNormal.y / nMagLocal, localNormal.z / nMagLocal )
		: Vector3( 0, 0, 1 );

	// The ownership window's ceiling: this composite's own extent, in its own
	// LOCAL frame (the frame the arguments and the ownership ray live in), so
	// the children's boxes are taken as they are and `m_mxFinalTrans` is NOT
	// applied -- unlike `getBoundingBox`, which answers in the PARENT's frame.
	// A subtraction can never extend past operand A, matching that function.
	// `maxWindow <= 0` means "no usable box" and disables the cap.
	//
	// THE DIAGONAL COMES FROM `LocalBoxDiagonal`, which is this block
	// extracted verbatim (Phase 3 of the cross-object arc): the proximity
	// bracket needs exactly the same number for its probe step, and two
	// copies of a screen this fiddly -- A-only for a subtraction, the
	// `isfinite` / `< 1e30` extent test, the `diag > 0` guard -- would be
	// free to drift.  `maxWindow == 0` keeps its meaning here ("no usable
	// box", cap disabled), which is what the helper's `false` maps to.
	Scalar maxWindow = Scalar(0);
	{
		Scalar diagLocal = Scalar(0);
		if( LocalBoxDiagonal( diagLocal ) ) {
			maxWindow = diagLocal;
		}
	}

	IObjectPriv* const operands[2] = { pObjectA, pObjectB };
	for( int i = 0; i < 2; i++ ) {
		IObjectPriv* const child = operands[i];
		if( !child ) {
			continue;
		}
		const Matrix4 mxInv = child->GetFinalInverseTransformMatrix();
		const Vector3 dChildUnnorm = Vector3Ops::Transform( mxInv, localDir );
		const Scalar s = Vector3Ops::Magnitude( dChildUnnorm );
		if( !( s > NEARZERO ) ) {
			continue;
		}
		const Point3 oChild = Point3Ops::Transform( mxInv, localOrigin );
		const Vector3 dChild( dChildUnnorm.x / s, dChildUnnorm.y / s, dChildUnnorm.z / s );

		const Matrix4 mxFwdT = Matrix4Ops::Transpose( child->GetFinalTransformMatrix() );
		const Vector3 nChildUnnorm = Vector3Ops::Transform( mxFwdT, localNormal );
		const Scalar nMag = Vector3Ops::Magnitude( nChildUnnorm );
		const Vector3 nChild = ( nMag > NEARZERO )
			? Vector3( nChildUnnorm.x / nMag, nChildUnnorm.y / nMag, nChildUnnorm.z / nMag )
			: localNormal;

		const Scalar floorChild = child->SelfHitRootFloor( oChild, dChild, nChild ) / s;
		worstAny = std::max( worstAny, floorChild );

		// A child that cannot be tested for ownership -- no usable normal, or a
		// floor that is not a finite number -- is left out of `worst` and kept
		// in `worstAny`, so it still governs through the no-owner fallback
		// below.  That is the conservative direction, and the probe has its own
		// non-finite guard for what comes back either way.
		if( !canTestOwnership || !std::isfinite( floorChild ) ) {
			continue;
		}

		// Per-child window: never narrower than the child's own gate (with the
		// probe's own 2x headroom), or the child can never be hit inside it.
		Scalar window = std::max( delta, Scalar(2) * floorChild );
		if( maxWindow > Scalar(0) && window > maxWindow ) {
			window = maxWindow;			// see the cap's note above
		}
		if( !std::isfinite( window ) ) {
			continue;
		}

		const Point3 base(
			localOrigin.x - nUnit.x * window,
			localOrigin.y - nUnit.y * window,
			localOrigin.z - nUnit.z * window );

		bool owns = child->IntersectRay_IntersectionOnly( Ray( base, nUnit ), Scalar(2) * window, true, true );

		// Only if the straight shot missed: the eight transverse retries (see
		// the note above).  `owns` short-circuits, so a child the ray finds
		// costs exactly what it did before, and the four DIAGONALS -- which
		// settle a shared vertex, where the four axial ones all graze -- are
		// only reached once those four have missed.
		if( !owns ) {
			const Vector3 t1 = AnyPerpendicularUnit_( nUnit );
			const Vector3 t2 = Vector3Ops::Cross( nUnit, t1 );		// unit: both are
			const Scalar  h  = Scalar(1) / std::sqrt( Scalar(2) );	// keeps the diagonals unit-length
			const Vector3 offs[8] = {
				Vector3(  t1.x,  t1.y,  t1.z ), Vector3( -t1.x, -t1.y, -t1.z ),
				Vector3(  t2.x,  t2.y,  t2.z ), Vector3( -t2.x, -t2.y, -t2.z ),
				Vector3( ( t1.x + t2.x ) * h, ( t1.y + t2.y ) * h, ( t1.z + t2.z ) * h ),
				Vector3( ( t1.x - t2.x ) * h, ( t1.y - t2.y ) * h, ( t1.z - t2.z ) * h ),
				Vector3( ( -t1.x + t2.x ) * h, ( -t1.y + t2.y ) * h, ( -t1.z + t2.z ) * h ),
				Vector3( ( -t1.x - t2.x ) * h, ( -t1.y - t2.y ) * h, ( -t1.z - t2.z ) * h ) };
			for( int j = 0; j < 8 && !owns; j++ ) {
				const Ray jitRay(
					Point3( base.x + offs[j].x * window,
					        base.y + offs[j].y * window,
					        base.z + offs[j].z * window ),
					nUnit );
				owns = child->IntersectRay_IntersectionOnly( jitRay, Scalar(2) * window, true, true );
			}
		}

		if( owns ) {
			anyOwner = true;
			worst = std::max( worst, floorChild );
		}
	}

	return anyOwner ? worst : worstAny;
}

//////////////////////////////////////////////////////////////////////
//
//  THE CROSS-OBJECT PROXIMITY QUERIES FOR A COMPOSITE
//  (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.6, Phase 3).
//
//  Until Phase 3 a composite REFUSED both queries -- `Object::
//  DistanceToSurface` forwards to the geometry and a `CSGObject` has none
//  -- so its own surface was invisible to every neighbour's query and a
//  scene whose contact surface was a CSG result needed a non-CSG proxy.
//
//  THE OPERANDS ARE NOT IN WORLD SPACE.  `IntersectRay` maps the ray by
//  this composite's own inverse and THEN calls each operand, whose
//  `Object::IntersectRay` applies its own inverse on top -- the rule
//  `SelfHitRootFloor` states as "the arguments are in THIS composite's
//  local frame".  So both queries below implement their own transform
//  layer exactly as `Object` does: map the world point through the
//  composite's inverse, convert the radius by `/sigmaMin`, recurse into
//  the operands with the LOCAL point (each applies its own transform; a
//  nested composite repeats the same two conversions), and convert the
//  answer back -- `x sigmaMax` for the unsigned upper bound, `x sigmaMin`
//  for the signed lower one.
//
//  WHY `min(operands)` IS THE WRONG ANSWER FOR TWO OF THE THREE OPS.  For
//  a point outside both operands of a UNION the true distance to the
//  union's surface IS `min(d_A, d_B)`: every union-boundary point lies on
//  one operand's boundary, so `d >= min`; and if the nearest point of
//  `dA` lies inside B, the segment to it enters B first at a point on
//  `dB` outside A -- a boundary point no farther than `d_A` -- so
//  `d <= min`.  For an INTERSECTION or a SUBTRACTION the nearest
//  operand-surface point may not be on the composite's surface at all, so
//  `min` is only a LOWER bound: the forbidden direction, contact painted
//  where there is none.  Those two compose the operands' SIGNED LOWER
//  BOUNDS into a field whose sign is exact and whose magnitude is a lower
//  bound, and run Phase 1's bracket on it.
//
//  AND `f <= 0` IS NOT THE LANDING TEST.  `{max(f_A, -f_B) <= 0}` is
//  `closure(A) intersect complement(interior B)`, which contains every
//  point where the two boundaries merely TOUCH -- a phantom set no ray
//  can hit.  `glass_pavilion`'s flute slot is exactly that case: the slot
//  is as deep as the column's diameter, so its faces are TANGENT to the
//  cylinder, and an `f <= 0` test would report a 1 cm chord where the
//  nearest real surface is 4.21 cm away.  So a landing is admitted only
//  when the OPERANDS' OWN SIGNS prove it, with NO TOLERANCE on either
//  side -- a tolerance re-admits the phantom, because the nearby points
//  of one operand's surface may all lie inside the other.
//
//  Three budget constants, sized exactly as `SDFGeometry`'s are and for
//  the same reasons; see that file's header comment.
//
//////////////////////////////////////////////////////////////////////

namespace
{
	const int    kCsgDescentIters = 6;
	const int    kCsgProbeSteps   = 40;
	const Scalar kCsgBackoff      = Scalar( 2 );

	//! The composite's probe step, as a fraction of its own local
	//! bounding-box diagonal, floored so a collapsed box still steps.
	//! The SDF family's rule, with the SDF's author-settable `m_epsFrac`
	//! replaced by a fixed 5e-5 -- so a composite may probe FINER than an
	//! operand SDF's own surface band, which is harmless.
	const Scalar kCsgEpsFrac  = Scalar( 5e-5 );
	const Scalar kCsgEpsFloor = Scalar( 1e-6 );

	//! At a `max`/`min` SEAM two nearly opposed operand gradients can
	//! cancel the composite's.  Below this the candidate REFUSES -- an
	//! under-paint, never a wrong answer.  The SDF's fabricated `(0,1,0)`
	//! fallback is deliberately NOT reused: there is no better direction
	//! to invent, and inventing one here would walk a landing into the
	//! phantom set.
	const Scalar kCsgSeamGradient = Scalar( 1e-12 );

	//! RISE's own unbounded-coordinate sentinel, the same screen
	//! `Geometry::BoundingBoxRootFloor` and `SelfHitRootFloor` apply.
	const Scalar kCsgMaxSaneExtent = Scalar( 1e30 );
}

bool CSGObject::LocalBoxDiagonal( Scalar& outDiag ) const
{
	outDiag = Scalar( 0 );
	if( !pObjectA ) {
		return false;
	}
	// For a SUBTRACTION the box is A's alone, matching the visible extent
	// `getBoundingBox` reports: a subtraction can never extend past its
	// minuend.  A nested operand's box recurses through its own
	// `getBoundingBox`, which answers in ITS parent's frame -- this
	// composite's local frame, since CSG operands cannot be parented.
	//
	// A-ONLY IS A SETTLED TRADE, carried here from `SelfHitRootFloor`
	// (which now calls this function rather than repeating it).  The
	// closing review of 269a5ad2 asked for BOTH operands' boxes, because a
	// subtrahend whose own step-off band exceeds the minuend (an SDF blade
	// with an authored epsilon of 0.02 carving a box smaller than 0.4
	// units) is capped below its own floor and dropped -- but widening to
	// B's box re-admits exactly the reach `CsgFloorOwnershipTest` Test 9
	// pins: a degenerate subtrahend (radius 4000, floor 5657) whose box
	// would let its ownership ray charge that floor on A's face.  The two
	// cannot both hold with a size-only cap; the degenerate one is the
	// dangerous direction (over-statement) and the coarse-epsilon one only
	// under-states into a graceful fallback, so A's box stays the bound
	// and the other is an accepted residual (docs/CLOTH_FABRIC_DESIGN.md
	// debt 25).
	BoundingBox bbLocal = pObjectA->getBoundingBox();
	if( op != CSG_SUBTRACTION && pObjectB ) {
		bbLocal.Include( pObjectB->getBoundingBox() );
	}
	const Scalar ex = bbLocal.ur.x - bbLocal.ll.x;
	const Scalar ey = bbLocal.ur.y - bbLocal.ll.y;
	const Scalar ez = bbLocal.ur.z - bbLocal.ll.z;
	if( !( ex >= Scalar( 0 ) && ey >= Scalar( 0 ) && ez >= Scalar( 0 ) ) ||
	    !std::isfinite( ex ) || !std::isfinite( ey ) || !std::isfinite( ez ) ||
	    ex >= kCsgMaxSaneExtent || ey >= kCsgMaxSaneExtent || ez >= kCsgMaxSaneExtent ) {
		return false;
	}
	const Scalar diag = std::sqrt( ex*ex + ey*ey + ez*ez );
	if( !std::isfinite( diag ) || !( diag > Scalar( 0 ) ) ) {
		return false;
	}
	outDiag = diag;
	return true;
}

bool CSGObject::ComposedSignedLocal( const Point3& ptLocal, const Scalar maxDistLocal,
	Scalar& outF, bool& outExact,
	Scalar& outFA, bool& outExactA, Scalar& outFB, bool& outExactB ) const
{
	outF = Scalar( 0 );
	outExact = outExactA = outExactB = false;
	outFA = outFB = Scalar( 0 );

	if( !pObjectA || !pObjectB ) {
		return false;
	}
	// EITHER operand refusing refuses the composite.  A sheet (any plane,
	// disk, open cylinder, mesh, patch or hair) refuses the signed query
	// outright -- not merely "lacks the exactness flag", which a BOUND
	// operand also does without forcing a refusal.
	if( !pObjectA->SignedDistanceLower( ptLocal, maxDistLocal, outFA, outExactA ) ) {
		return false;
	}
	if( !pObjectB->SignedDistanceLower( ptLocal, maxDistLocal, outFB, outExactB ) ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( (double)outFA ) || !RISE::IsFiniteDouble( (double)outFB ) ) {
		return false;
	}

	switch( op )
	{
	case CSG_UNION:
		// Outside, `min(f_A, f_B) <= min(d_A, d_B) = d`; inside, the depth
		// to leave `A union B` is at least `max(depth_A, depth_B)`, which
		// is `|min(f_A, f_B)|`.  Both are lower bounds, which is the
		// contract.
		//
		// A UNION NEVER EXPORTS EXACTNESS, and this is a CORRECTION to
		// §5.6, which says it may when both operands do and its own sigma
		// is exact.  That rule is unsound on ABUTTING operands, and the
		// review round that found it traced the whole failure:
		// `min(f_A, f_B)` is 0 not only on the union's boundary but on
		// every INTERIOR point where the two operands' boundaries meet
		// from opposite sides -- a shared face, a set of positive AREA.
		// Two boxes stacked to make a cube read `min = 0` all over the
		// seam plane, where the true signed distance is the depth of the
		// cube.  Exported with the flag set, that lets a PARENT
		// subtraction's boundary arm (`exB && f_A < 0 && f_B >= 0`) admit
		// a landing that is strictly INSIDE the subtrahend -- not in the
		// real solid at all -- which is the round-3 phantom one level up
		// and an OVER-READ of contact, the one direction this signal must
		// never fail in.  Measured on `box(2,2,1)@-0.5 union box(2,2,1)@+0.5`
		// subtracted from a 4-cube: reported 0.25 against a true 0.75.
		//
		// Dropping the flag closes it completely, because every OTHER
		// consumer of this field only ever reads a STRICT sign: the strict
		// arms need `< 0` or `> 0` (an interior seam reads 0 and is
		// refused), `interior` counts only `f < 0` (an interior seam
		// contributes 0 depth -- an under-read, the safe direction), and a
		// parent's own `max` composition keeps the correct
		// outside-or-on sign there.  The cost is that a union of exact
		// operands now always sends a parent to the probe, which §5.6
		// already names as the safe direction for a missed boundary
		// landing.
		outF = std::min( outFA, outFB );
		break;
	case CSG_INTERSECTION:
		outF = std::max( outFA, outFB );
		break;
	case CSG_SUBTRACTION:
		outF = std::max( outFA, -outFB );
		break;
	default:
		return false;
	}
	// NO COMPOSITE EXPORTS EXACTNESS.  `max(a, b)` under-reads near a seam
	// even over exact operands and its zero set is the phantom touching
	// set; `min(a, b)` is 0 on a union's INTERIOR seams (see the union
	// case above).  Either way a parent's boundary arm must not be allowed
	// to land on a composite's zero set, so `outExact` is left false --
	// initialised false at the top of this function and never set.
	return RISE::IsFiniteDouble( (double)outF );
}

bool CSGObject::LandingAdmits( const Point3& qLocal, const Scalar maxDistLocal,
	CsgLandingArm& outArm, bool& outOperandRefused ) const
{
	outArm = CsgLandingArm::None;
	outOperandRefused = false;

	Scalar f = 0, fA = 0, fB = 0;
	bool ex = false, exA = false, exB = false;
	if( !ComposedSignedLocal( qLocal, maxDistLocal, f, ex, fA, exA, fB, exB ) ) {
		outOperandRefused = true;
		return false;
	}

	// A UNION NEVER REACHES HERE -- it answers `min` directly and never
	// runs the bracket -- and saying so structurally rather than letting
	// it fall into the subtraction's disjuncts is the point: the two arms
	// below are written as an `intersection`/`else` pair, and an `else`
	// that silently means "subtraction OR union" is the shape this file's
	// own `ComposedSignedLocal` switch exists to avoid.
	if( op != CSG_INTERSECTION && op != CSG_SUBTRACTION ) {
		return false;
	}

	// THE STRICT ARM.  A NEGATIVE lower bound proves the point is strictly
	// inside its operand; a POSITIVE one proves it strictly outside.  Both
	// together put `q` in the solid's INTERIOR, so the segment from the
	// query point to it crosses the boundary and the chord is at least the
	// true distance.
	const bool strict = ( op == CSG_INTERSECTION )
		? ( fA < Scalar( 0 ) && fB < Scalar( 0 ) )
		: ( fA < Scalar( 0 ) && fB > Scalar( 0 ) );
	if( strict ) {
		outArm = CsgLandingArm::Strict;
		return true;
	}

	// THE BOUNDARY ARM admits a landing exactly ON one operand's surface,
	// and only when THAT operand's signed distance is EXACT -- the `<= 0`
	// / `>= 0` side must be the exact one, and the other side stays
	// STRICT.  A point in `closure(A)` strictly outside `B` is in the
	// closure of `A \ B` (interior if inside A; on `dA` a neighbourhood
	// outside B holds interior points of A, all in the solid), and a
	// point strictly inside `A` on or outside `dB` likewise.
	//
	// NO TOLERANCE, on either side, deliberately.  The descent lands on
	// the exact operand's surface only to ROUNDING, and a `+1 ulp` miss
	// fails this arm -- the probe then steps and the answer is `d` plus
	// that step instead of `d`, which is the SAFE direction.  A tolerance
	// looked harmless and is not: the nearby points of an operand's
	// surface may all lie INSIDE the other operand, so a landing within
	// `tau` of a surface can be within `tau` of the PHANTOM, not of the
	// real solid.  On `glass_pavilion`'s flute a tolerance of 5e-12
	// admitted a quarter of the stations within a few micrometres of the
	// slot's axis, each reporting a 1.00 cm chord where the real solid is
	// 4.21 cm away.
	//
	// NEITHER ARM ADMITS `f_A == f_B == 0` -- the strict side rejects it
	// in every disjunct -- which is what keeps the EXACT tangency out.
	bool boundary = false;
	if( op == CSG_INTERSECTION ) {
		boundary = ( exA && fA <= Scalar( 0 ) && fB <  Scalar( 0 ) )
		        || ( exB && fA <  Scalar( 0 ) && fB <= Scalar( 0 ) );
	} else {
		boundary = ( exA && fA <= Scalar( 0 ) && fB >  Scalar( 0 ) )
		        || ( exB && fA <  Scalar( 0 ) && fB >= Scalar( 0 ) );
	}
	if( boundary ) {
		outArm = CsgLandingArm::Boundary;
		return true;
	}
	return false;
}

bool CSGObject::BracketDistanceLocal( const Point3& ptLocal, const Scalar maxDistLocal,
	Scalar& outDist, CsgLandingArm& outArm ) const
{
	outArm = CsgLandingArm::None;

	Scalar f0 = 0, fA0 = 0, fB0 = 0;
	bool ex0 = false, exA0 = false, exB0 = false;
	if( !ComposedSignedLocal( ptLocal, maxDistLocal, f0, ex0, fA0, exA0, fB0, exB0 ) ) {
		return false;
	}

	// INSIDE THE COMPOSITE IS CONTACT (design 2), and the clamp is on a
	// STRICTLY NEGATIVE composed sign: `f == 0` is also the phantom set,
	// so it goes through the landing test below like any other candidate
	// and reads 0 only when an arm actually admits it.
	if( f0 < Scalar( 0 ) ) {
		outDist = Scalar( 0 );
		outArm = CsgLandingArm::None;
		return true;
	}

	Scalar diag = Scalar( 0 );
	const Scalar eps = LocalBoxDiagonal( diag )
		? std::max( kCsgEpsFloor, kCsgEpsFrac * diag )
		: kCsgEpsFloor;

	// DESCEND.  `p <- p - f * grad_hat(p)`.  The composed field is still
	// 1-Lipschitz -- an operand's transformed lower bound
	// `sigmaMin * d_o(M^-1 x)` has gradient at most `sigmaMin/sigmaMin`,
	// the ellipsoid's `dUnit x min(a,b,c)` likewise, and `max`/`min`
	// preserve it -- so a step of `|f|` along `-grad_hat` cannot overshoot
	// the zero set, which is what keeps the landing an honest bracket
	// rather than a guess.
	Point3 p = ptLocal;
	Scalar f = f0;
	Vector3 g( 0, 1, 0 );
	bool haveGradient = false;
	for( int it = 0; it < kCsgDescentIters; ++it ) {
		if( f <= kCsgBackoff * eps ) {
			break;
		}
		// SIX composite evaluations, each recursing into both operands.
		Scalar d[3] = { 0, 0, 0 };
		bool gradOk = true;
		for( int axis = 0; axis < 3 && gradOk; ++axis ) {
			Point3 lo = p, hi = p;
			if( axis == 0 )      { lo.x -= eps; hi.x += eps; }
			else if( axis == 1 ) { lo.y -= eps; hi.y += eps; }
			else                 { lo.z -= eps; hi.z += eps; }
			Scalar fl = 0, fh = 0, tA = 0, tB = 0;
			bool tex = false, teA = false, teB = false;
			if( !ComposedSignedLocal( lo, maxDistLocal, fl, tex, tA, teA, tB, teB )
			 || !ComposedSignedLocal( hi, maxDistLocal, fh, tex, tA, teA, tB, teB ) ) {
				gradOk = false;
				break;
			}
			d[axis] = ( fh - fl ) / ( Scalar( 2 ) * eps );
		}
		if( !gradOk ) {
			return false;
		}
		const Scalar gm = std::sqrt( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
		// A SEAM: two nearly opposed operand gradients cancelling the
		// composite's.  Refuse rather than walk an invented direction.
		if( !std::isfinite( gm ) || !( gm > kCsgSeamGradient ) ) {
			return false;
		}
		g = Vector3( d[0]/gm, d[1]/gm, d[2]/gm );
		haveGradient = true;

		const Point3 next( p.x - f * g.x, p.y - f * g.y, p.z - f * g.z );
		Scalar fn = 0, nA = 0, nB = 0;
		bool nex = false, neA = false, neB = false;
		// NO SEPARATE `!IsFiniteDouble(fn)` CHECK HERE, UNLIKE
		// `SDFGeometry::DistanceToSurface`'s twin (its `Map(next)` is a raw,
		// possibly-non-finite call that BREAKS out of the descent and still
		// lets the probe below try from the last good `p`/`f` -- a
		// break-and-probe). `ComposedSignedLocal` already screens `outF` for
		// finiteness at its own return (`return
		// RISE::IsFiniteDouble( (double)outF );`, CSGObject.cpp ~2425) and
		// returns FALSE on a non-finite composed field, which the line
		// immediately above turns into an outright REFUSAL of this whole
		// query -- never a break that still reaches the probe. The two
		// behave differently on purpose, not by parity: an `SDFGeometry`
		// leaf can recover from one non-finite `Map` sample because the
		// probe re-evaluates the SAME cheap function from a nearby point,
		// while a composite's `fn` already reflects two operand recursions
		// (each of which may itself walk an operand's own bracket), so a
		// non-finite composed field is treated as a harder signal here and
		// refused rather than salvaged -- the safe direction, and the one
		// this landing test's callers already expect from any refusal.
		if( !ComposedSignedLocal( next, maxDistLocal, fn, nex, nA, neA, nB, neB ) ) {
			return false;
		}
		// A step that did not reduce the field means the local gradient is
		// lying to us (a seam, a scaled operand); stop rather than wander.
		// The probe below still gets its chance from here.  Exiting HERE
		// -- and on the backoff test above -- is also what keeps the
		// degenerate central difference AT a tangency (a V-valley of value
		// 0) from ever being taken from a descended landing.
		if( fn >= f ) {
			break;
		}
		p = next;
		f = fn;
	}

	// THE PROBE'S `!haveGradient` BRANCH: a query point that STARTS inside
	// the backoff band exits the descent on its first test, and `g` would
	// still hold its arbitrary `(0,1,0)` initialiser.  Take a real
	// gradient here.  At a tangency this central difference straddles the
	// valley -- and that is precisely the case the landing test refuses
	// rather than answers, since it demands operand-sign proof.
	if( !haveGradient ) {
		Scalar d[3] = { 0, 0, 0 };
		for( int axis = 0; axis < 3; ++axis ) {
			Point3 lo = p, hi = p;
			if( axis == 0 )      { lo.x -= eps; hi.x += eps; }
			else if( axis == 1 ) { lo.y -= eps; hi.y += eps; }
			else                 { lo.z -= eps; hi.z += eps; }
			Scalar fl = 0, fh = 0, tA = 0, tB = 0;
			bool tex = false, teA = false, teB = false;
			if( !ComposedSignedLocal( lo, maxDistLocal, fl, tex, tA, teA, tB, teB )
			 || !ComposedSignedLocal( hi, maxDistLocal, fh, tex, tA, teA, tB, teB ) ) {
				return false;
			}
			d[axis] = ( fh - fl ) / ( Scalar( 2 ) * eps );
		}
		const Scalar gm = std::sqrt( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
		if( !std::isfinite( gm ) || !( gm > kCsgSeamGradient ) ) {
			return false;
		}
		g = Vector3( d[0]/gm, d[1]/gm, d[2]/gm );
	}

	// PROBE.  The descended point is tested FIRST -- a landing that the
	// boundary arm admits has `gap = 0`, which is the whole reason that
	// arm exists -- and only then does the doubling walk start.
	bool refused = false;
	Point3 q = p;
	bool admitted = LandingAdmits( p, maxDistLocal, outArm, refused );
	if( refused ) {
		return false;
	}
	Scalar step = std::max( eps, ( f > Scalar( 0 ) ) ? f : eps );
	Scalar walked = Scalar( 0 );
	for( int s = 0; !admitted && s < kCsgProbeSteps; ++s ) {
		walked += step;
		// A CHEAP budget test, not the authoritative one: overshooting it
		// only ends the walk early (a refusal, which under-paints), and
		// the final `d > maxDistLocal` test below is what actually decides
		// whether the answer is in range.
		if( f0 + walked > maxDistLocal ) {
			break;
		}
		q = Point3( p.x - walked * g.x, p.y - walked * g.y, p.z - walked * g.z );
		admitted = LandingAdmits( q, maxDistLocal, outArm, refused );
		if( refused ) {
			return false;
		}
		step *= Scalar( 2 );
	}

	// NO ADMITTED LANDING WITHIN BUDGET: REFUSE.  Never the unproven
	// point's distance, which could be smaller than the truth and so
	// over-read contact.
	if( !admitted ) {
		outArm = CsgLandingArm::None;
		return false;
	}

	const Scalar d = Vector3Ops::Magnitude( Vector3Ops::mkVector3( q, ptLocal ) );
	if( !RISE::IsFiniteDouble( (double)d ) || d > maxDistLocal ) {
		outArm = CsgLandingArm::None;
		return false;
	}
	outDist = d;
	return true;
}

bool CSGObject::DistanceToSurfaceWithArm( const Point3& ptWorld, const Scalar maxDistWorld,
	Scalar& outDist, CsgLandingArm& outArm ) const
{
	outArm = CsgLandingArm::None;
	if( !pObjectA || !pObjectB ) {
		return false;
	}
	if( !( m_sigmaMin > Scalar( 0 ) ) || !( m_sigmaMax > Scalar( 0 ) ) ) {
		return false;
	}

	Scalar maxDistLocal = maxDistWorld / m_sigmaMin;
	if( !RISE::IsFiniteDouble( (double)maxDistLocal ) || maxDistLocal > RISE_INFINITY ) {
		maxDistLocal = RISE_INFINITY;
	}
	const Point3 ptLocal = Point3Ops::Transform( m_mxInvFinalTrans, ptWorld );

	Scalar dLocal = Scalar( 0 );
	if( op == CSG_UNION ) {
		// THE UNION'S UNSIGNED ANSWER is `min` over the operands that
		// ANSWER -- `d <= d_A <= u_A` holds whatever B does -- so a union
		// refuses only when BOTH operands refuse.  That is what lets a
		// union accept a SHEET operand (a mesh, a plane): its unsigned
		// answer is all the proof this needs.  It cannot say whether the
		// point is INSIDE a sheet operand, which is why the SIGNED query
		// below still demands both, and why a point inside a mesh operand
		// of a union reads a positive distance -- the under-paint
		// direction, disclosed.
		//
		// No composed sign is needed: the operands' own clamps already
		// return 0 for a point inside either of them.
		bool any = false;
		Scalar best = Scalar( 0 );
		for( int i = 0; i < 2; ++i ) {
			const IObjectPriv* const child = ( i == 0 ) ? pObjectA : pObjectB;
			Scalar u = Scalar( 0 );
			if( child->DistanceToSurface( ptLocal, maxDistLocal, u )
			 && RISE::IsFiniteDouble( (double)u ) && u >= Scalar( 0 ) ) {
				if( !any || u < best ) { best = u; any = true; }
			}
		}
		if( !any ) {
			return false;
		}
		dLocal = best;
	} else {
		if( !BracketDistanceLocal( ptLocal, maxDistLocal, dLocal, outArm ) ) {
			return false;
		}
	}

	// AND THE ANSWER COMES OUT MULTIPLIED BY sigmaMax, the safe direction
	// for an upper bound, exactly as `Object::DistanceToSurface` does.
	//
	// NO RANGE REFUSAL HERE (found in review): `Object::DistanceToSurface`
	// deliberately has none -- its own `dWorld` finiteness check is the
	// whole gate, and a merely-far answer is left to the CALLER's `d < best`
	// comparison (`ObjectManager::NearestOtherSurface`/`ProximityCandidateDistance`)
	// to discard. A `dWorld > maxDistWorld` refusal here used to spend this
	// composite's ONE-SHOT refusal latch (`NoteDistanceRefusal` /
	// `LogDistanceRefusal`, see the design's own "the one-shot latch is
	// spent by the first such point" rule) on a composite that is simply
	// too far away to matter for THIS query -- not on a composite that
	// genuinely cannot answer -- so a later query point where the same
	// composite's bracket truly fails to close found the latch already
	// spent and printed nothing. Matching `Object::DistanceToSurface`'s
	// shape returns the honest (if merely far) distance instead and leaves
	// range-pruning to the manager, exactly as every other family's
	// `DistanceToSurface` already does.
	const Scalar dWorld = dLocal * m_sigmaMax;
	if( !RISE::IsFiniteDouble( (double)dWorld ) ) {
		return false;
	}
	outDist = dWorld;
	return true;
}

bool CSGObject::DistanceToSurface( const Point3& ptWorld, const Scalar maxDistWorld, Scalar& outDist ) const
{
	CsgLandingArm arm = CsgLandingArm::None;
	return DistanceToSurfaceWithArm( ptWorld, maxDistWorld, outDist, arm );
}

bool CSGObject::SignedDistanceLower( const Point3& ptWorld, const Scalar maxDistWorld,
	Scalar& outSigned, bool& outExact ) const
{
	outExact = false;
	if( !pObjectA || !pObjectB ) {
		return false;
	}
	if( !( m_sigmaMin > Scalar( 0 ) ) || !( m_sigmaMax > Scalar( 0 ) ) ) {
		return false;
	}

	Scalar maxDistLocal = maxDistWorld / m_sigmaMin;
	if( !RISE::IsFiniteDouble( (double)maxDistLocal ) || maxDistLocal > RISE_INFINITY ) {
		maxDistLocal = RISE_INFINITY;
	}
	const Point3 ptLocal = Point3Ops::Transform( m_mxInvFinalTrans, ptWorld );

	Scalar f = 0, fA = 0, fB = 0;
	bool ex = false, exA = false, exB = false;
	if( !ComposedSignedLocal( ptLocal, maxDistLocal, f, ex, fA, exA, fB, exB ) ) {
		return false;
	}

	// `x sigmaMin`, the LOWER bound's safe direction.  The `&& m_sigmaExact`
	// is DEFENCE IN DEPTH, not a live path: `ComposedSignedLocal` leaves
	// `ex` false for every operation (see its union case for why a union
	// cannot export it either), so this conjunction is already false.  It
	// is written out anyway so that the similarity condition
	// `Object::SignedDistanceLower` carries is stated here too, and so
	// that a future composed field that COULD be exact would still have to
	// clear it.
	const Scalar fWorld = f * m_sigmaMin;
	if( !RISE::IsFiniteDouble( (double)fWorld ) ) {
		return false;
	}
	outSigned = fWorld;
	outExact  = ex && m_sigmaExact;
	return true;
}

const char* CSGObject::DescribeKind() const
{
	switch( op )
	{
	case CSG_UNION:			return "csg union";
	case CSG_INTERSECTION:	return "csg intersection";
	case CSG_SUBTRACTION:	return "csg subtraction";
	default:				return "csg (unknown operation)";
	}
}
