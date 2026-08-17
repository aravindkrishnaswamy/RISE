//////////////////////////////////////////////////////////////////////
//
//  Object.cpp - Implements the Object class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Object.h"
#include "SnapshotLeafClone.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include <atomic>		// P2a: log-once idiom for UniformRandomPoint's null-geometry fallback warning

using namespace RISE;
using namespace RISE::Implementation;

Object::Object( ) :
  pGeometry( 0 ),
  pUVGenerator( 0 ),
  pMaterial( 0 ),
  pModifier( 0 ),
  pShader( 0 ),
  pRadianceMap( 0 ),
  pInteriorMedium( 0 ),
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  nConsumedBy( 0 ),
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 ),
  m_worldAreaScale( 1.0 )
{
}


Object::Object( const IGeometry* pGeometry_ ) :
  pGeometry( pGeometry_ ),
  pUVGenerator( 0 ),
  pMaterial( 0 ),
  pModifier( 0 ),
  pShader( 0 ),
  pRadianceMap( 0 ),
  pInteriorMedium( 0 ),
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  nConsumedBy( 0 ),
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 ),
  m_worldAreaScale( 1.0 )
{
	if( pGeometry ) {
		pGeometry->addref();
	} else {
		GlobalLog()->PrintSourceError( "Object:: Geometry ptr was passed in but is invalid", __FILE__, __LINE__ );
	}
}

Object::~Object( )
{
	safe_release( pGeometry );
	safe_release( pMaterial );
	safe_release( pModifier );
	safe_release( pShader );
	safe_release( pUVGenerator );
	safe_release( pRadianceMap );
	safe_release( pInteriorMedium );
}

void Object::RemoveConsumer()
{
	// SATURATION IS A BUG REPORT, NOT A RECOVERY.  Every AddConsumer has exactly
	// one matching RemoveConsumer (CSGObject::AssignObjects pairs with the
	// destructor and with the outgoing branch of a re-assign), so reaching here
	// at zero means some composite released a claim it never took -- and the
	// balance being off by one in that direction means a LATER release will drive
	// a still-consumed operand to zero and let it render as a standalone shape
	// beside the composite that owns it.  That is precisely the failure the count
	// replaced a bool to prevent, so it must not pass silently; the clamp stays
	// because wrapping an `unsigned int` would pin the operand invisible forever,
	// which is the worse of the two.
	if( !nConsumedBy ) {
		GlobalLog()->PrintSourceError(
			"Object::RemoveConsumer:: unbalanced release -- this object is not consumed by any "
			"csg_object, so a composite has released a claim it never took.  The consumption "
			"count is now under-counted and some operand will later be un-hidden while a live "
			"composite is still using it", __FILE__, __LINE__ );
		return;
	}
	--nConsumedBy;
}

IObjectPriv* Object::CloneFull()
{
	// 87: same container handling as CloneSnapshot -- a container has no
	// geometry, and Object(const IGeometry*) logs a source ERROR for a null
	// one.  World visibility is copied rather than left at the ctor's `true`:
	// a container is created HIDDEN (see RISE_API_CreateObjectOrContainer_), so
	// a clone that came out visible with null geometry would enter every
	// world-visible enumeration containers are deliberately kept out of.
	// (Both clone entry points are currently dead public surface -- no caller
	// repo-wide -- but they are CloneSnapshot's siblings and the whole lesson
	// of this arc is that the sibling is where the defect lives.)  The COMPOSED
	// value for the reason spelled out in CopySnapshotStateInto: the clone has no
	// consumers, so its base flag has to carry the whole answer.
	Object* pClone = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "Clone" );
	pClone->bIsWorldVisible = IsWorldVisible();

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

IObjectPriv* Object::CloneGeometric()
{
	// 87: see CloneFull -- container ctor selection and world visibility.
	Object* pMe = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "cloned object" );
	pMe->bIsWorldVisible = IsWorldVisible();
	return pMe;
}

void Object::CopySnapshotStateInto( Object& dst ) const
{
	// Shared by Object::CloneSnapshot and CSGObject::CloneSnapshot.  Copies
	// every piece of mutable state EXCEPT the geometry (set by the subclass
	// ctor) and, for CSG, the operands (set by the subclass).  `dst` is a
	// freshly-constructed clone; we may touch its protected Object /
	// Transformable members because access to protected members of the same
	// type is permitted.

	// --- Mutable LEAF: material is cloned to an INDEPENDENT instance so a
	//     later in-place painter-slot rebind on the LIVE material (the
	//     editor's SetMaterialProperty path) does NOT bleed into the
	//     snapshot.  CloneMaterialForSnapshot hands back a reference the
	//     caller owns; AssignMaterial addrefs it, so we release our own. ---
	if( pMaterial ) {
		const IMaterial* matClone = CloneMaterialForSnapshot( pMaterial );
		if( matClone ) {
			dst.AssignMaterial( *matClone );
			matClone->release();
		}
	}

	// --- Immutable / non-property-edited leaves: addref-share. ---
	//     (Shader + interior medium are addref-shared in increment A; the
	//     SSS shader-op cache race + in-place medium-coefficient edits are
	//     the documented residual deferred to increment B — see
	//     SnapshotLeafClone.h.)
	if( pModifier )       { dst.AssignModifier( *pModifier ); }
	if( pShader )         { dst.AssignShader( *pShader ); }
	if( pRadianceMap )    { dst.AssignRadianceMap( *pRadianceMap ); }
	if( pInteriorMedium ) { dst.AssignInteriorMedium( *pInteriorMedium ); }
	if( pUVGenerator )    { dst.SetUVGenerator( *pUVGenerator ); }

	// --- Cheap value-typed flags ---
	// `nConsumedBy` is deliberately NOT among them: it counts the LIVE composites
	// consuming this object as a CSG operand, and a clone is consumed by whoever
	// assigns it, not by whoever consumed the original.  CSGObject::CloneSnapshot's
	// own AssignObjects establishes it for the operand clones it makes.
	//
	// WHICH IS EXACTLY WHY THE COMPOSED `IsWorldVisible()` IS COPIED HERE AND NOT THE
	// BASE FLAG.  The clone starts at zero consumers, so the base flag is the ONLY
	// thing left holding its visibility; copying a consumed operand's base flag
	// (which is `true` since 87 step 3b -- being an operand is the COUNT now, not the
	// flag) would hand the clone a world-VISIBLE standalone copy of something that
	// has no existence as a standalone shape.  `Scene::CreateSnapshot` reaches that
	// case directly: it clones every manager item BY NAME, so a `csg_object`'s
	// operands are cloned once on their own account and again, correctly hidden,
	// underneath the composite's own clone.  Copying the composed value restores the
	// pre-3b outcome exactly (before the count, a consumed operand's base flag WAS
	// `false`, so this line already copied `false`), and it stays right for the
	// operand clones CSGObject::CloneSnapshot makes: their AssignObjects consumes
	// them a moment later, so they are hidden either way.
	dst.bIsWorldVisible        = IsWorldVisible();
	dst.bCastsShadows          = bCastsShadows;
	dst.bReceivesShadows       = bReceivesShadows;
	dst.SURFACE_INTERSEC_ERROR = SURFACE_INTERSEC_ERROR;
	dst.m_tangentFrameSign     = m_tangentFrameSign;
	dst.m_worldAreaScale       = m_worldAreaScale;

	// --- Transform BUILDING BLOCKS (Transformable protected state) ---
	// Copying these is what makes the clone independent: a later
	// TranslateObject() on the live object pushes onto the LIVE stack and
	// re-finalizes the LIVE matrices, leaving the clone's copies untouched.
	dst.m_mxPosition      = m_mxPosition;
	dst.m_mxOrientation   = m_mxOrientation;
	dst.m_mxScale         = m_mxScale;
	dst.m_mxStretch       = m_mxStretch;
	CopyTransformMetadataTo( dst );
	dst.m_transformstack  = m_transformstack;   // std::deque<Matrix4> value copy

	// --- Finalized matrices (so the clone is render-ready without a
	//     re-finalize, and exactly reflects the live pose at snap time) ---
	dst.m_mxFinalTrans    = m_mxFinalTrans;
	dst.m_mxInvFinalTrans = m_mxInvFinalTrans;
	dst.m_mxInvTranspose  = m_mxInvTranspose;

	// --- 87 scene graph: the LOCAL matrix and the parent world it was
	//     composed against.  Without these the clone's world matrix would be
	//     right but its local/parent state identity, so the very first
	//     re-finalize on the clone (any absolute setter, a restore, the
	//     animator) would collapse it back to an unparented pose.
	dst.m_mxLocalTrans           = m_mxLocalTrans;
	dst.m_mxParentWorld          = m_mxParentWorld;
	dst.m_mxParentWorldInv       = m_mxParentWorldInv;
	dst.m_bParentWorldInvertible = m_bParentWorldInvertible;
}

Object* Object::CloneSnapshot() const
{
	// See Object.h for the rationale.  Build a fresh Object that shares the
	// immutable geometry leaf (the ctor addrefs it), then deep-copy the
	// mutable state.
	// 87: a CONTAINER has no geometry, and Object(const IGeometry*) logs a
	// source ERROR for a null one -- correct for a leaf, noise for a container.
	// Pick the ctor that matches what this object actually is.
	Object* pClone = pGeometry ? new Object( pGeometry ) : new Object();
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "snapshot clone" );

	CopySnapshotStateInto( *pClone );

	return pClone;
}

bool Object::AssignMaterial( const IMaterial& pMat )
{
	safe_release( pMaterial );

	pMaterial = &pMat;
	pMaterial->addref();

	return false;
}

bool Object::AssignGeometry( const IGeometry& pGeom )
{
	// Runtime geometry swap (interactive editor via SceneEdit::
	// SetObjectGeometry).  Mirrors AssignMaterial: release the prior
	// reference and retain the new one.  The bounding box is derived
	// on demand from pGeometry (getBoundingBox), so the caller
	// (SceneEditor::RunObjectInvariantChain) invalidates the top-level
	// acceleration afterward and the next render rebuilds the TLAS.
	safe_release( pGeometry );

	pGeometry = &pGeom;
	pGeometry->addref();

	return true;
}

bool Object::AssignModifier( const IRayIntersectionModifier& pMod )
{
	safe_release( pModifier );

	pModifier = &pMod;
	pModifier->addref();

	return false;
}

bool Object::AssignShader( const IShader& pShader_ )
{
	safe_release( pShader );

	pShader = &pShader_;
	pShader->addref();

	return true;
}

bool Object::AssignRadianceMap( const IRadianceMap& pRadianceMap_ )
{
	safe_release( pRadianceMap );

	pRadianceMap = &pRadianceMap_;
	pRadianceMap->addref();

	return true;
}

bool Object::SetUVGenerator( const IUVGenerator& pUVG )
{
	safe_release( pUVGenerator );

	pUVGenerator = &pUVG;
	pUVGenerator->addref();

	return true;
}

void Object::SetShadowParams( const bool bCasts, const bool bReceives )
{
	bCastsShadows = bCasts;
	bReceivesShadows = bReceives;
}

bool Object::AssignInteriorMedium( const IMedium& medium )
{
	safe_release( pInteriorMedium );

	pInteriorMedium = &medium;
	pInteriorMedium->addref();

	return true;
}

void Object::ClearInteriorMedium()
{
	safe_release( pInteriorMedium );
	pInteriorMedium = 0;
}

void Object::ClearShader()
{
	safe_release( pShader );
	pShader = 0;
}

void Object::ClearMaterial()
{
	safe_release( pMaterial );
	pMaterial = 0;
}

void Object::ClearModifier()
{
	safe_release( pModifier );
	pModifier = 0;
}

void Object::ClearRadianceMap()
{
	safe_release( pRadianceMap );
	pRadianceMap = 0;
}

void Object::ClearGeometry()
{
	// 87 recursive scene graph: a CONTAINER node is an Object with no geometry.
	// The null-geometry guards throughout this file are what make that safe;
	// see getBoundingBox()'s comment for the (now reachable) call-graph note.
	safe_release( pGeometry );
	pGeometry = 0;
}

const IMaterial* Object::GetMaterial() const
{
	return pMaterial;
}

const IMedium* Object::GetInteriorMedium() const
{
	return pInteriorMedium;
}

bool Object::ComputeAnalyticalDerivatives(
	const Point2& uv,
	Scalar        smoothing,
	Point3&       outWorldPosition,
	Vector3&      outWorldNormal,
	Vector3&      outWorldDpdu,
	Vector3&      outWorldDpdv,
	Vector3&      outWorldDndu,
	Vector3&      outWorldDndv
	) const
{
	if( !pGeometry ) return false;

	// Object-space query
	Point3  oP;
	Vector3 oN, oDpdu, oDpdv, oDndu, oDndv;
	if( !pGeometry->ComputeAnalyticalDerivatives(
			uv, smoothing, oP, oN, oDpdu, oDpdv, oDndu, oDndv ) )
	{
		return false;
	}

	// Apply transform — same convention as the IntersectRay path:
	//  - Position: full forward transform.
	//  - Tangent vectors (dpdu, dpdv): forward transform's linear part
	//    (translation drops out for vector arithmetic).
	//  - Normal and its derivatives (dndu, dndv): inverse-transpose's
	//    linear part — keeps them orthogonal to the transformed surface
	//    under non-uniform scale / shear.
	outWorldPosition = Point3Ops::Transform( m_mxFinalTrans, oP );
	outWorldDpdu     = Vector3Ops::Transform( m_mxFinalTrans, oDpdu );
	outWorldDpdv     = Vector3Ops::Transform( m_mxFinalTrans, oDpdv );
	outWorldNormal   = Vector3Ops::Normalize(
		Vector3Ops::Transform( m_mxInvTranspose, oN ) );
	outWorldDndu     = Vector3Ops::Transform( m_mxInvTranspose, oDndu );
	outWorldDndv     = Vector3Ops::Transform( m_mxInvTranspose, oDndv );
	return true;
}

const BoundingBox Object::getBoundingBox() const
{
	// NULL-GEOMETRY GUARD.  This branch is REACHABLE as of 87 (recursive scene
	// graph): a `standard_object` with no `geometry` is a CONTAINER node -- a
	// pure transform other objects are parented to -- and Job::AddObject
	// creates it with no geometry at all.  (Before 87 the only null-geometry
	// object was a CSGObject, which overrides getBoundingBox() entirely and
	// never reaches here, so this used to be dead defensive code; the
	// 2026-07-31 audit-round comment that said so is superseded.)
	//
	// A container is world-INVISIBLE, so nothing in the render path asks it
	// for a box: CreateBVH/CreateOctree filter on IsWorldVisible() before
	// calling GetElementBoundingBox.  The empty box is what a caller outside
	// that gate gets, matching CSGObject::getBoundingBox's own no-operand
	// fallback.
	if( !pGeometry ) {
		return BoundingBox( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) );
	}

	const BoundingBox bbox = pGeometry->GenerateBoundingBox();

	// Transform all 8 corners of the local bbox and take the AABB of the
	// rotated set.  Transforming only ll and ur produces an AABB that
	// covers a single edge of the rotated cube — for a 50°/120° rotation
	// the resulting world bbox covers ~25% of the actual extent in the
	// rotated axes, so BSP / Octree placement based on this bbox excludes
	// rays that pass through the geometry's true rotated extent.  The
	// downstream symptom is whole strips of a rotated object rendering as
	// background because acceleration-structure traversal never reaches
	// the leaf that holds the object.
	const Point3 corners[8] = {
		Point3( bbox.ll.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ll.z ),
		Point3( bbox.ll.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ll.y, bbox.ur.z ),
		Point3( bbox.ll.x, bbox.ur.y, bbox.ur.z ),
		Point3( bbox.ur.x, bbox.ur.y, bbox.ur.z )
	};

	Point3 wll = Point3Ops::Transform( m_mxFinalTrans, corners[0] );
	Point3 wur = wll;
	for( int i = 1; i < 8; i++ ) {
		const Point3 c = Point3Ops::Transform( m_mxFinalTrans, corners[i] );
		if( c.x < wll.x ) wll.x = c.x;
		if( c.y < wll.y ) wll.y = c.y;
		if( c.z < wll.z ) wll.z = c.z;
		if( c.x > wur.x ) wur.x = c.x;
		if( c.y > wur.y ) wur.y = c.y;
		if( c.z > wur.z ) wur.z = c.z;
	}

	return BoundingBox( wll, wur );
}

void Object::IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// NULL-GEOMETRY GUARD: see getBoundingBox()'s comment above.  Reachable as
	// of 87 for a CONTAINER node, though the world-visible gate in
	// ObjectManager::RayElementIntersection means no ray reaches a container
	// through the normal traversal.  No hit.  P3 CORRECTION (fix round 3): this does
	// NOT match CSGObject::IntersectRay's own no-operand fallback -- CSG's
	// `if( !pObjectA || !pObjectB ) { ...; return; }` fires BEFORE it ever
	// sets `ri.geometric.bHit = false`, so it returns WITHOUT touching
	// bHit at all (relying on the caller's own pre-call initialization).
	// This guard explicitly clears bHit instead, which is the stronger,
	// correct contract for THIS call site (Object::IntersectRay is a
	// public entry point with no such caller-init guarantee) -- the
	// behavior here is right, but the earlier comment's "matching..."
	// claim was not.
	if( !pGeometry ) {
		ri.geometric.bHit = false;
		return;
	}

	// Bring the ray into our frame, first tuck away the original ray value
	const Ray orig = ri.geometric.ray;

	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- this is the direction-true
	// world-to-local distance factor used below (P1 fix: was a +X-axis
	// probe, see the `factor` comment further down).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, orig.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	ri.geometric.ray.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// Landing 2: transform ray differentials into object space alongside
	// origin/dir, otherwise ComputeTextureFootprint would project
	// world-space auxiliaries onto object-space dpdu/dpdv and produce
	// the wrong UV footprint (and therefore the wrong mip LOD).
	//
	// Origins are simple: differentials are OFFSETS between two world
	// points, so they transform as vectors (linear part only — the
	// translation cancels in the diff of two transformed points).
	//
	// Directions are NOT simple.  rxDir / ryDir were established by the
	// camera as the offset between two UNIT-normalized world directions,
	//   rxDir_world = aux_x_world_norm − d_world_norm
	// and the same convention must hold in object space:
	//   rxDir_obj   = aux_x_obj_norm   − d_obj_norm
	// Under any non-identity scale (uniform or not) the obvious
	// `M_inv * rxDir_world` gives an unnormalised vector that does not
	// equal `aux_x_obj_norm − d_obj_norm`.  Reconstruct the auxiliary
	// fully: rebuild `aux = d + diff` in world space, transform, re-
	// normalise, then re-difference against the (already normalised)
	// object-space central direction.
	//
	// SetDir() on the central ray cleared hasDifferentials, so re-set
	// it after we've finished writing.
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

	// factor converts a WORLD-frame distance limit (dHowFar) into the
	// local-frame traversal limit used by the box pre-test and the
	// dHowFar2-comparisons below.  MUST be the magnitude of the
	// TRANSFORMED RAY DIRECTION (dirLocalMag, captured above before it was
	// normalized into ray.dir) -- NOT an arbitrary +X-axis probe (P1 fix).
	// Under non-uniform scale, |M^-1 * v| depends on which direction v
	// points; a +X-only factor mis-scales the limit for every ray not
	// travelling along local +X (e.g. a shadow ray leaking past the light,
	// or a valid hit truncated short of it).  Guard a degenerate transform
	// that collapses this direction to ~0 (singular along this direction)
	// by falling back to an unscaled factor of 1.0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = dHowFar;

	// We can't go farther than infinity, so in this case only reduce the
	// length, never extend: dHowFar==RISE_INFINITY is a large FINITE
	// sentinel (not IEEE inf -- see the ffast-math/no-infinity convention),
	// so scaling it by factor>=1 would overflow into a real infinity for
	// no benefit (there's nothing to shrink); only factor<1 is worth
	// applying.  This reasoning is about the sentinel's magnitude, not
	// about how `factor` itself is derived -- unchanged now that factor is
	// direction-true rather than +X-axis-based.
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	// Compute ray intersection with box.
	//
	// When the ray's ORIGIN is inside the bounding box, RayBoxIntersection
	// flips its contract: hit.dRange reports the distance to EXIT the box
	// (i.e. tmax, since tmin is negative), and hit.dRange2 holds the
	// negative tmin.  We detect "origin inside the box" via `dRange2 < 0`
	// and skip the `dRange > dHowFar2` early-return in that case — the
	// ray may still hit the geometry's surface within dHowFar even though
	// the box it sits inside extends further.
	//
	// Prior to this fix, the early-return fired incorrectly on short
	// probes (e.g. ManifoldSolver::ComputeVertexDerivatives, which
	// shoots a 0.05 probe from +0.05 above a torus surface vertex back
	// down toward it — both probe origin and target sit inside the
	// torus bbox, so the bbox-exit distance ~0.2 always exceeded
	// dHowFar2 = 0.1).  The probe was silently dropped and the SMS
	// solver rejected the whole chain with "ComputeVertexDerivatives
	// failed".  The torus surface was fine; the gate was wrong.
	if( pGeometry->DoPreHitTest() )
	{
		BOX_HIT		hit;
		BoundingBox	bbox = pGeometry->GenerateBoundingBox();
		RayBoxIntersection( ri.geometric.ray, hit, bbox.ll, bbox.ur );
		if( !hit.bHit ) {
			return;
		}

		const bool originInsideBox = ( hit.dRange2 < 0 );
		if( !originInsideBox && hit.dRange > dHowFar2 ) {
			return;
		}
	}

	pGeometry->IntersectRay( ri.geometric, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	if( ri.geometric.bHit )
	{
		// This an overriding UV generator only, it is for geometries that don't know how to compute
		// their UV co-ordinates so the user has specified a geometry object to help them out.
		// Box/Cylinder/Sphere UV projections pick the projection axis
		// from the surface face — use the GEOMETRIC normal so the
		// chosen axis is the actual face orientation, not Phong-
		// interpolated or bump-perturbed.  On analytical primitives
		// shading == geometric so this is a no-op there.
		if( pUVGenerator ) {
			pUVGenerator->GenerateUV( ri.geometric.ptIntersection, ri.geometric.vGeomNormal, ri.geometric.ptCoord );
		}

		// Transform the normals back
		ri.geometric.vNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal ));
		// Geometric normal transforms identically (it's also a normal vector,
		// just describing the actual face orientation rather than the shading
		// approximation).  Renormalize because non-uniform scales can otherwise
		// leave it un-unit.
		ri.geometric.vGeomNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal ));
		// Shading ONB.  By default the tangent (u-axis) is whatever
		// CreateFromW picks from a canonical axis -- fine for isotropic
		// materials, but an arbitrary base for anisotropic GGX.  A
		// geometry can instead request a COHERENT, world-X-aligned tangent
		// (bShadingTangentFromGeometry) so an anisotropic tangent_rotation
		// rotates from the same base on it as on the cartesian_disk mesh
		// (whose constant +Z normal makes CreateFromW yield ±world-X).  We
		// project world-X into the now-world-space shading-normal plane and
		// hand it to CreateFromWU (W fixed = normal, U re-orthonormalized
		// against W); when world-X is parallel to the normal we fall back
		// to world-Y so the projection never degenerates.
		if( ri.geometric.bShadingTangentFromGeometry ) {
			const Vector3& n = ri.geometric.vNormal;
			Vector3 t( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );	// (1,0,0) - n*dot(n,(1,0,0))
			if( Vector3Ops::SquaredModulus( t ) < NEARZERO ) {
				t = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );	// (0,1,0) - n*dot(n,(0,1,0))
			}
			ri.geometric.onb.CreateFromWU( n, t );	// W = n (fixed); V = norm(W x t), U = V x W (double-cross => U = t projected into the W-plane)
		} else {
			ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
		}

		// Transform the per-vertex tangent (v3 storage path) from object
		// space to world space.  Tangents transform with the forward
		// matrix (like positions / dpdu), NOT inverse-transpose -- they
		// are surface-tangent directions, not normals.
		//
		// bitangentSign needs to flip iff the transform reverses
		// orientation (det(M) < 0).  For an orientation-preserving
		// transform, cross(N_world, T_world) gives the same world-space
		// bitangent as transforming the original cross(N_obj, T_obj),
		// so the imported sign carries through unchanged.  For a
		// mirroring transform like `scale -1 1 1` the linear part has
		// negative determinant and cross(N_world, T_world) ends up
		// pointing in the opposite world direction from the
		// transformed-original bitangent -- multiplying the imported
		// sign by m_tangentFrameSign (computed once in
		// FinalizeTransformations) puts it back, so the same source
		// mesh shades correctly under mirrored instancing.
		if( ri.geometric.bHasTangent ) {
			ri.geometric.vTangent = Vector3Ops::Normalize(
				Vector3Ops::Transform( m_mxFinalTrans, ri.geometric.vTangent ) );
			ri.geometric.bitangentSign *= m_tangentFrameSign;
		}

		// Transform surface derivatives from object space to world space.
		// dpdu, dpdv are tangent vectors — transform like positions (use
		// the forward transform m_mxFinalTrans).  dndu, dndv are normals
		// (change-of-normal is itself a normal-like quantity at first
		// order) — transform like normals (inverse-transpose).
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

		// Wireframe view-mode closest-edge point transforms like a
		// position (forward transform) -- exactly as ptIntersection.
		if( ri.geometric.bHasWireEdgeInfo ) {
			ri.geometric.ptWireNearestEdge = Point3Ops::Transform(
				m_mxFinalTrans, ri.geometric.ptWireNearestEdge );
		}

		if( bComputeExitInfo ) {
			ri.geometric.vNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal2 ) );
			ri.geometric.vGeomNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vGeomNormal2 ) );
			ri.geometric.ptObjExit = ri.geometric.ray.PointAtLength( ri.geometric.range2 + SURFACE_INTERSEC_ERROR );
			ri.geometric.ptExit = Point3Ops::Transform( m_mxFinalTrans, ri.geometric.ptObjExit );

			if( ri.geometric.range2 != 0 ) {
				ri.geometric.range2 = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptExit, orig.origin ) );
			}
		}

		// Tell which modifier
		ri.pModifier = pModifier;

		// Tell which material
		ri.pMaterial = pMaterial;

		// Tell which shader
		ri.pShader = pShader;

		// Tell which radiance map
		ri.pRadianceMap = pRadianceMap;

		// Compute the intersection in world space
		ri.geometric.ptObjIntersec = ri.geometric.ray.PointAtLength( ri.geometric.range - SURFACE_INTERSEC_ERROR );
		ri.geometric.ptIntersection = Point3Ops::Transform( m_mxFinalTrans, ri.geometric.ptObjIntersec );
		ri.geometric.range = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptIntersection, orig.origin ) );

		ri.pObject = this;
	}

	// Restore the old ray
	ri.geometric.ray = orig;
}

bool Object::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// NULL-GEOMETRY GUARD: see getBoundingBox()'s comment above.  Reachable as
	// of 87 for a CONTAINER node; the world-visible + casts-shadows gate in
	// ObjectManager::RayElementIntersection_IntersectionOnly keeps shadow rays
	// away from one.  No intersection.
	if( !pGeometry ) {
		return false;
	}

	// Bring the ray into our frame, but use our own copy
	Ray		orig = ray;

	orig.origin = Point3Ops::Transform( m_mxInvFinalTrans, ray.origin );

	// Capture the UNNORMALIZED transformed direction's magnitude before
	// normalizing it into the local-frame ray -- the direction-true
	// world-to-local distance factor used below (P1 fix, mirrors
	// Object::IntersectRay above).
	const Vector3 dirLocalUnnorm = Vector3Ops::Transform( m_mxInvFinalTrans, ray.Dir() );
	const Scalar dirLocalMag = Vector3Ops::Magnitude( dirLocalUnnorm );
	orig.SetDir( Vector3Ops::Normalize( dirLocalUnnorm ) );

	// factor converts a WORLD-frame distance limit (dHowFar) into the
	// local-frame traversal limit.  MUST be the magnitude of the
	// TRANSFORMED RAY DIRECTION (dirLocalMag, captured above) -- NOT an
	// arbitrary +X-axis probe (P1 fix); see Object::IntersectRay above for
	// the full rationale.  Guard a degenerate transform that collapses
	// this direction to ~0 by falling back to an unscaled factor of 1.0.
	const Scalar factor = (dirLocalMag > NEARZERO) ? dirLocalMag : Scalar(1.0);
	Scalar dHowFar2 = dHowFar;

	// We can't go farther than infinity, so in this case only reduce the
	// length, never extend -- see Object::IntersectRay above for why this
	// guard is about the RISE_INFINITY sentinel's magnitude and stays
	// correct regardless of how `factor` is derived.
	if( (dHowFar != RISE_INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	// Do bounding box check first
	if( pGeometry->DoPreHitTest() ) {
		// Compute ray intersection with box
		BOX_HIT		hit;
		BoundingBox	bbox = pGeometry->GenerateBoundingBox();
		RayBoxIntersection( orig, hit, bbox.ll, bbox.ur );
		if( !hit.bHit ) {
			return false;
		}

		if( hit.dRange > dHowFar2 ) {
			// If we are in the box, this is not a valid test...
			if( !GeometricUtilities::IsPointInsideBox( orig.origin, bbox.ll, bbox.ur ) ) {
				return false;
			}
		}
	}

	return pGeometry->IntersectRay_IntersectionOnly( orig, dHowFar2, bHitFrontFaces, bHitBackFaces );
}

void Object::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// NULL-GEOMETRY GUARD (2026-07-31 fix round 2, caller list corrected
	// fix round 3; 87: a geometry-less CONTAINER node is now a second source
	// of a null pGeometry, and it reaches these callers no more than a
	// CSGObject does -- a container is world-INVISIBLE, so it never lands on
	// the luminaries list or in any world-visible scan): pGeometry is null
	// for a CSGObject (see GetArea()'s doc comment above).  Every known caller of UniformRandomPoint on an
	// IObject now refuses to reach here with a null pGeometry:
	// LightSampler (safe by LUMINARIES-LIST MEMBERSHIP -- see GetArea()'s
	// doc comment's class (2) argument, not a local check);
	// SubSurfaceScatteringShaderOp / DonnerJensenSkinSSSShaderOp via their
	// own CanBeAreaLight-or-null gate (local check, fix round 1);
	// ManifoldSolver's SpecularCasterCollector via its own GetGeometry()
	// gate (local check, fix round 1); and ManifoldSolver's
	// surfaceSampleReflectionFallback k=1-mirror gate (local check, fix
	// round 3 -- P1a: this call site was MISSED in rounds 1-2 because its
	// object comes from a live ray hit, not the pre-filtered
	// mSpecularCasters cache).  This is a belt-and-suspenders base-layer
	// guard, not a path exercised in the audited call graph.  Returns a
	// DEFINED (not garbage / NaN) fallback rather than crashing: this
	// object's own local origin transformed to world space, a canonical
	// world +Y normal (transformed the same way UniformRandomPoint's
	// normal always is, below), and a zero UV.  This is deliberately NOT a
	// "sampling contract" value (no surface exists to sample uniformly).
	//
	// P2a (fix round 3, Opus review): unlike GetArea()'s silent 0 (0 is
	// the established "not sampleable" signal every caller already reads
	// correctly), a wrong-POSITION fallback point is not self-announcing
	// to a caller that forgets its own gate -- it looks like a valid
	// sample.  Warn once per process (SplatFilm.cpp's EvaluateFilter-
	// support log-once idiom) so a future regression is loud, not silent.
	if( !pGeometry ) {
		static std::atomic<bool> warnedNullGeometryFallback{ false };
		bool expected = false;
		if( warnedNullGeometryFallback.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Object::UniformRandomPoint:: called on an object with no directly-owned "
				"geometry (e.g. a csg_object) -- every known caller gates this away, so "
				"reaching here means a caller is missing its null-geometry check.  "
				"Returning a FABRICATED fallback point (object origin, +Y normal) rather "
				"than crashing -- this is NOT a valid uniform surface sample; fix the "
				"calling site's gate." );
		}
		if( point )  *point  = Point3Ops::Transform( m_mxFinalTrans, Point3( 0, 0, 0 ) );
		if( normal ) *normal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxFinalTrans, Vector3( 0, 1, 0 ) ) );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	pGeometry->UniformRandomPoint( point, normal, coord, prand );

	if( point ) {
		*point = Point3Ops::Transform( m_mxFinalTrans, (*point) );
	}

	if( normal ) {
		*normal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxFinalTrans, (*normal) ));
	}
}

Scalar Object::GetArea( ) const
{
	// NULL-GEOMETRY GUARD (2026-07-31 fix round 2): pGeometry is null for a
	// CSGObject (its shape is synthesized from two operand objects rather
	// than owned directly -- see CSGObject.h/.cpp, which overrides
	// IntersectRay/getBoundingBox but NOT GetArea()) and, as of 87, for a
	// geometry-less CONTAINER node.  Returns 0 -- an area
	// of zero is the same "cannot be uniformly area-sampled" signal every
	// caller already checks for CanBeAreaLight()==false: `area > 0` gates
	// all of them, so 0 flows through as "not sampleable" without a
	// division anywhere reading pGeometry again.
	//
	// P3 CORRECTION (fix round 3): "every known call site ALSO now
	// null-checks GetGeometry() before calling this" OVERSTATED it -- the
	// call sites split into two DIFFERENT safety arguments, not one:
	//   (1) LOCAL CHECK immediately before the call: EmissionShaderOp.cpp,
	//       PathTracingIntegrator.cpp, BDPTIntegrator.cpp, VCMIntegrator.cpp
	//       (all fix-round-2), and SubSurfaceScatteringShaderOp.cpp /
	//       DonnerJensenSkinSSSShaderOp.cpp (fix-round-1) -- each reads
	//       GetGeometry() and gates on it right there.
	//   (2) LUMINARIES-LIST MEMBERSHIP, not a local check: LightSampler.cpp
	//       (4 call sites, `lumEntry.pLum->GetArea()`), PhotonTracer.h,
	//       SpectralPhotonTracer.h, SMSPhotonMap.cpp -- none of these
	//       re-check GetGeometry() at the call site; they are safe because
	//       `pLum`/the luminary they iterate can ONLY be an object that
	//       already passed LuminaryManager::AddToLuminaryList's null-
	//       geometry gate to get onto the luminaries list in the first
	//       place.  A null-geometry object never reaches these loops at
	//       all -- the safety is upstream admission control, not a
	//       per-call re-verification.
	// This base-layer guard is what makes class (2) actually safe (absent
	// it, membership alone wouldn't help if some OTHER path ever mutated
	// or bypassed the luminaries list) and is belt-and-suspenders for
	// class (1); it exists so a future caller that forgets its own check
	// degrades to "zero area" instead of a null-deref.
	if( !pGeometry ) {
		return Scalar( 0 );
	}

	// WORLD-AREA JACOBIAN (2026-08-13): pGeometry->GetArea() is the
	// OBJECT-space surface area, but UniformRandomPoint() returns points
	// transformed through m_mxFinalTrans -- so every consumer that claims
	// pdfPosition = 1/GetArea() (LightSampler NEE, BDPT/VCM InitLight,
	// photon-emission power normalization, SSS dipole sampling) needs the
	// WORLD-space area or the claimed density is wrong by the transform's
	// area scaling (a `scale 2` emitter previously lit its surroundings at
	// 1/4 the correct NEE energy; measured 0.39x after MIS mixing).
	//
	// m_worldAreaScale = |det(linear part)|^(2/3), cached by
	// FinalizeTransformations(): EXACT for rotations, reflections, and
	// uniform scales (the overwhelmingly common case: det = s^3, area
	// scale = s^2).  For non-uniform scale or shear it is the geometric-
	// mean approximation -- the true area factor varies across the surface
	// with the local normal, and object-space-uniform sampling is then not
	// world-uniform either, so the residual error there sits in the
	// sampler, not just this scalar (LuminaryManager warns once when a
	// non-uniformly-scaled luminaire is admitted).  Exactness needs
	// per-geometry integration; not attempted here.
	//
	// RISE_INFINITY guard: InfinitePlaneGeometry::GetArea() returns the
	// DBL_MAX sentinel; multiplying it by any factor > 1 (a scale, or a
	// rotation whose determinant lands at 1+1ulp) would overflow to +inf,
	// which turns the light-selection alias table's TotalWeight into inf
	// and every pdfSelect into NaN.  Pass the sentinel through untouched.
	const Scalar objArea = pGeometry->GetArea();
	if( objArea <= 0 || objArea >= RISE_INFINITY ) {
		return objArea;
	}
	return objArea * m_worldAreaScale;
}

void Object::Realize() const
{
	if( pGeometry ) {
		pGeometry->Realize();
	}
}

void Object::ResetRuntimeData() const
{
	if( pShader ) {
		pShader->ResetRuntimeData();
	}
}

void Object::FinalizeTransformations( const Matrix4& parentWorld )
{
	Transformable::FinalizeTransformations( parentWorld );

	// Everything below is derived from m_mxFinalTrans, which is now the
	// COMPOSED world matrix `parentWorld * local`.  That is what makes the
	// three caches correct under hierarchy by construction rather than by a
	// separate recompute pass: there is one finalize, and it is this one.
	m_mxInvTranspose = Matrix4Ops::Transpose( m_mxInvFinalTrans );

	// Sign of the chirality flip the world-space transform applies to
	// the tangent frame.  For an affine object transform the 4D
	// determinant equals the upper-3x3 determinant; <0 means the
	// transform reverses orientation (e.g. `scale -1 1 1` or any
	// reflection / mirror), and the imported TANGENT.w needs to be
	// negated at hit time so cross(N_world, T_world) * w still gives
	// the bitangent that's consistent with the mirrored surface.
	// See Object::IntersectRay where this is multiplied into
	// ri.geometric.bitangentSign.
	const Scalar det = Matrix4Ops::Determinant( m_mxFinalTrans );
	m_tangentFrameSign = (det < Scalar( 0 )) ? Scalar( -1 ) : Scalar( 1 );

	// World-area scaling of the linear part, cached here (the transform is
	// immutable during render) so the hot GetArea() path is a single
	// multiply.  |det|^(2/3) is EXACT for rotations / reflections / uniform
	// scales; for non-uniform scale or shear it is the geometric-mean
	// approximation (see GetArea()'s comment).  Degenerate transform → 0,
	// matching the "cannot area-sample" sentinel convention.
	const Scalar absDet = fabs( det );
	m_worldAreaScale = (absDet > Scalar( 0 ))
		? pow( absDet, Scalar( 2.0 / 3.0 ) )
		: Scalar( 0 );
}
