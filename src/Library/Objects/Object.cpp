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
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  SURFACE_INTERSEC_ERROR( 1e-12 )
{
}


Object::Object( const IGeometry* pGeometry_ ) :
  pGeometry( pGeometry_ ),
  pUVGenerator( 0 ),
  pMaterial( 0 ),
  pModifier( 0 ),
  pShader( 0 ),
  pRadianceMap( 0 ),
  bIsWorldVisible( true ),
  bCastsShadows( true ),
  bReceivesShadows( true ),
  SURFACE_INTERSEC_ERROR( 1e-12 )
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

const IMaterial* Object::GetMaterial() const
{
	return pMaterial;
}

const BoundingBox Object::getBoundingBox() const
{
	BoundingBox bbox = pGeometry->GenerateBoundingBox();
	BoundingBox b( 
		Point3Ops::Transform( m_mxFinalTrans, bbox.ll ), 
		Point3Ops::Transform( m_mxFinalTrans, bbox.ur )
		);

	// Rotations have been known to flip around the LL and the UR, so we must
	// make sure that everything is sane before returning
	b.SanityCheck();
	return b;
}

void Object::IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// Bring the ray into our frame, first tuck away the original ray value
	const Ray orig = ri.geometric.ray;

	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );
	ri.geometric.ray.dir = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, orig.dir ) );

	const Scalar factor = Vector3Ops::Magnitude( Vector3Ops::Transform( m_mxInvFinalTrans, Vector3(1,0,0) ) );
	Scalar dHowFar2 = dHowFar; 

	// We can't go farther than infinity, so in this case only reduce thelength, never extend
	if( (dHowFar != INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	// Compute ray intersection with box
	if( pGeometry->DoPreHitTest() )
	{
		BOX_HIT		hit;
		BoundingBox	bbox = pGeometry->GenerateBoundingBox();
		RayBoxIntersection( ri.geometric.ray, hit, bbox.ll, bbox.ur );
		if( !hit.bHit ) {
			return;
		}

		if( hit.dRange > dHowFar2 ) {
			return;
		}
	}

	pGeometry->IntersectRay( ri.geometric, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	if( ri.geometric.bHit )
	{
		// This an overriding UV generator only, it is for geometries that don't know how to compute
		// their UV co-ordinates so the user has specified a geometry object to help them out
		if( pUVGenerator ) {
			pUVGenerator->GenerateUV( ri.geometric.ptIntersection, ri.geometric.vNormal, ri.geometric.ptCoord );
		}

		// Transform the normals back
		ri.geometric.vNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal ));
		ri.geometric.onb.CreateFromW( ri.geometric.vNormal );

		if( bComputeExitInfo ) {
			ri.geometric.vNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal2 ) );
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
	orig.dir = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, ray.dir ) );

	const Scalar factor = Vector3Ops::Magnitude( Vector3Ops::Transform( m_mxInvFinalTrans, Vector3(1,0,0) ) );
	Scalar dHowFar2 = dHowFar; 

	// We can't go farther than infinity, so in this case only reduce thelength, never extend
	if( (dHowFar != INFINITY) || (factor < 1.0) ) {
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
}
