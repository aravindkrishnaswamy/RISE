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
	if( pObjectA ) {
		pObjectA->SetWorldVisible( true );
	}

	if( pObjectB ) {
		pObjectB->SetWorldVisible( true );
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

	// AssignObjects addrefs both operands (and hides them via
	// SetWorldVisible(false), matching the live CSG).  We then drop our own
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

	if( pObjectA ) {
		pObjectA->SetWorldVisible( true );
	}

	if( pObjectB ) {
		pObjectB->SetWorldVisible( true );
	}

	safe_release( pObjectA );
	safe_release( pObjectB );

	pObjectA = objA;
	pObjectB = objB;

	pObjectA->addref();
	pObjectB->addref();

	pObjectA->SetWorldVisible( false );
	pObjectB->SetWorldVisible( false );
	
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
	//! texture coordinates, surface derivatives, texture footprint, vertex
	//! color, tangent frame, and the object-space intersection point used
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
		dst.txFootprint = src.txFootprint;
		dst.vColor = src.vColor;
		dst.bHasVertexColor = src.bHasVertexColor;
		dst.vTangent = src.vTangent;
		dst.bitangentSign = src.bitangentSign;
		dst.bHasTangent = src.bHasTangent;
		dst.bShadingTangentFromGeometry = src.bShadingTangentFromGeometry;

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
		Scalar exitRangeCsgLocal )
	{
		if( !operand || exitRangeCsgLocal <= 0 || exitRangeCsgLocal == RISE_INFINITY ) {
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
		const Scalar margin = std::max( Scalar(1e-12), kUlpFactor * dirWeightedAbs );
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

		operand->IntersectRay( probe, maxAcceptRange, true, true, false );

		if( !probe.geometric.bHit || probe.geometric.range > maxAcceptRange ) {
			return false;
		}

		AdoptCsgSurfacePayload( dst, probe.geometric );
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
	const bool exactBoundaryMode = ri.geometric.minimumSurfaceRange >= 0;
	if( exactBoundaryMode ) {
		// Child objects report their exact ranges in this CSG's local frame,
		// so the caller-frame representability bound needs the same conversion
		// as dHowFar before it can reject the just-processed root.
		riObjA.geometric.minimumSurfaceRange =
			factor * ri.geometric.minimumSurfaceRange;
		riObjB.geometric.minimumSurfaceRange =
			factor * ri.geometric.minimumSurfaceRange;
	}

	pObjectA->IntersectRay( riObjA, dHowFar2, true, true, true );
	pObjectB->IntersectRay( riObjB, dHowFar2, true, true, true );

	if( exactBoundaryMode ) {
		// The CSG interval algebra below was written in terms of range/range2.
		// In topology mode make those its exact working coordinates, so every
		// comparison and every branch assignment composes the same geometric
		// boundaries.  Ordinary shading retains its historical offset ranges.
		if( riObjA.geometric.bHit ) {
			riObjA.geometric.range = riObjA.geometric.surfaceRange;
			if( riObjA.geometric.range2 != 0 ) {
				riObjA.geometric.range2 = riObjA.geometric.surfaceRange2;
			}
		}
		if( riObjB.geometric.bHit ) {
			riObjB.geometric.range = riObjB.geometric.surfaceRange;
			if( riObjB.geometric.range2 != 0 ) {
				riObjB.geometric.range2 = riObjB.geometric.surfaceRange2;
			}
		}
	}

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
					if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectB, riObjB.geometric.range2 ) ) {
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
					if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectA, riObjA.geometric.range2 ) ) {
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
						ri = riObjB;
						ri.geometric.range = riObjB.geometric.range2;
						ri.geometric.range2 = riObjA.geometric.range2;
						// Reverse the normal
						ri.geometric.vNormal = -riObjB.geometric.vNormal2;
						ri.geometric.vGeomNormal = -riObjB.geometric.vGeomNormal2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
						ri.geometric.vGeomNormal2 = riObjA.geometric.vGeomNormal2;
						if( !AdoptCsgExitFacePayloadViaProbe( ri.geometric, pObjectB, riObjB.geometric.range2 ) ) {
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
		ri.geometric.vNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal ));
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
			Vector3 t( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );	// (1,0,0) - n*dot(n,(1,0,0))
			if( Vector3Ops::SquaredModulus( t ) < NEARZERO ) {
				t = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );	// (0,1,0) - n*dot(n,(0,1,0))
			}
			ri.geometric.onb.CreateFromWU( n, t );	// W = n (fixed); V = norm(W x t), U = V x W
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
			ri.geometric.bitangentSign *= m_tangentFrameSign;
		}

		// Transform surface derivatives (P2-d) from THIS CSG object's local
		// frame to world space -- mirrors Object::IntersectRay's
		// derivatives block exactly.  dpdu / dpdv are tangent vectors
		// (forward transform); dndu / dndv are normal-like quantities at
		// first order (inverse-transpose).
		if( ri.geometric.derivatives.valid ) {
			ri.geometric.derivatives.dpdu = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdu );
			ri.geometric.derivatives.dpdv = Vector3Ops::Transform(
				m_mxFinalTrans, ri.geometric.derivatives.dpdv );
			ri.geometric.derivatives.dndu = Vector3Ops::Transform(
				m_mxInvTranspose, ri.geometric.derivatives.dndu );
			ri.geometric.derivatives.dndv = Vector3Ops::Transform(
				m_mxInvTranspose, ri.geometric.derivatives.dndv );
		}

		// Compute the intersection in world space.  In exact-boundary mode the
		// switch composed range/range2 from exact child intervals; preserve
		// those boundary points before applying the legacy shading offsets.
		if( exactBoundaryMode ) {
			const Point3 ptLocalSurface =
				ri.geometric.ray.PointAtLength( ri.geometric.range );
			const Point3 ptWorldSurface =
				Point3Ops::Transform( m_mxFinalTrans, ptLocalSurface );
			ri.geometric.surfaceRange = Vector3Ops::Magnitude(
				Vector3Ops::mkVector3( ptWorldSurface, orig.origin ) );
			if( ri.geometric.range2 != 0 &&
				ri.geometric.range2 < RISE_INFINITY )
			{
				const Point3 ptLocalSurface2 =
					ri.geometric.ray.PointAtLength( ri.geometric.range2 );
				const Point3 ptWorldSurface2 =
					Point3Ops::Transform( m_mxFinalTrans, ptLocalSurface2 );
				ri.geometric.surfaceRange2 = Vector3Ops::Magnitude(
					Vector3Ops::mkVector3( ptWorldSurface2, orig.origin ) );
			} else {
				ri.geometric.surfaceRange2 = RISE_INFINITY;
			}
		}

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
