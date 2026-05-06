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
#include "../Interfaces/ILog.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"

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
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 )
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
  SURFACE_INTERSEC_ERROR( 1e-12 ),
  m_tangentFrameSign( 1.0 )
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

IObjectPriv* Object::CloneFull()
{
	Object* pClone = new Object( pGeometry );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "Clone" );

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
	Object* pMe = new Object( pGeometry );
	GlobalLog()->PrintNew( pMe, __FILE__, __LINE__, "cloned object" );
	return pMe;
}

bool Object::AssignMaterial( const IMaterial& pMat )
{
	safe_release( pMaterial );

	pMaterial = &pMat;
	pMaterial->addref();

	return false;
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
	// Bring the ray into our frame, first tuck away the original ray value
	const Ray orig = ri.geometric.ray;

	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );
	ri.geometric.ray.SetDir(Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, orig.Dir() ) ));

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

	const Scalar factor = Vector3Ops::Magnitude( Vector3Ops::Transform( m_mxInvFinalTrans, Vector3(1,0,0) ) );
	Scalar dHowFar2 = dHowFar; 

	// We can't go farther than infinity, so in this case only reduce thelength, never extend
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
		ri.geometric.onb.CreateFromW( ri.geometric.vNormal );

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
	// Bring the ray into our frame, but use our own copy
	Ray		orig = ray;

	orig.origin = Point3Ops::Transform( m_mxInvFinalTrans, ray.origin );
	orig.SetDir(Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, ray.Dir() ) ));

	const Scalar factor = Vector3Ops::Magnitude( Vector3Ops::Transform( m_mxInvFinalTrans, Vector3(1,0,0) ) );
	Scalar dHowFar2 = dHowFar; 

	// We can't go farther than infinity, so in this case only reduce thelength, never extend
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
	return pGeometry->GetArea();
}

void Object::ResetRuntimeData() const
{
	if( pShader ) {
		pShader->ResetRuntimeData();
	}
}

void Object::FinalizeTransformations( )
{
	Transformable::FinalizeTransformations();
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
}
