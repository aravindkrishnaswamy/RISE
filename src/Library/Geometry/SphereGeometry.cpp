//////////////////////////////////////////////////////////////////////
//
//  SphereGeometry.cpp - Implementation of the sphere class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SphereGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

SphereGeometry::SphereGeometry( Scalar dRadius ) : 
  m_dRadius( dRadius ), m_dSqrRadius( dRadius*dRadius), m_dOVRadius( 1.0 / dRadius )
{
}

SphereGeometry::~SphereGeometry( )
{
}

void SphereGeometry::GenerateMesh( )
{

}

void SphereGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInSphere = GeometricUtilities::IsPointInsideSphere( ri.ray.origin, m_dRadius, Point3(0,0,0) );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInSphere ) {
		return;
	}

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInSphere ) {
		return;
	}

	HIT	h;
	RaySphereIntersection( ri.ray, h, m_dRadius );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		ri.vNormal = Vector3Ops::Normalize(Vector3Ops::mkVector3( ri.ptIntersection, Point3(0,0,0) ));

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			ri.vNormal2 = Vector3Ops::Normalize(Vector3Ops::mkVector3( ri.ptExit, Point3(0,0,0) ));
		}

		// Calculate UV co-ordinates using spherical mapping
		GeometricUtilities::SphereTextureCoord( 
			Vector3( 0.0, 1.0, 0.0 ),
			Vector3( -1.0, 0.0, 0.0 ), 
			ri.vNormal, ri.ptCoord );
	}
}

bool SphereGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInSphere = GeometricUtilities::IsPointInsideSphere( ray.origin, m_dRadius, Point3(0,0,0) );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInSphere ) {
		return false;
	}

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInSphere ) {
		return false;
	}

	HIT	h;
	RaySphereIntersection( ray, h, m_dRadius );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void SphereGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = m_dRadius;
}

BoundingBox SphereGeometry::GenerateBoundingBox() const
{
	return BoundingBox( 
		Point3( -m_dRadius, -m_dRadius, -m_dRadius ),
		Point3( m_dRadius, m_dRadius, m_dRadius ) );
}

void SphereGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point3 pt = GeometricUtilities::PointOnSphere( Point3(0,0,0), m_dRadius, Point2( prand.x, prand.y ) );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		*normal = Vector3Ops::Normalize(Vector3Ops::mkVector3(pt,Point3(0,0,0)));
	}

	if( coord ) {
		if( normal ) {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, 1.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ), *normal, *coord );
		} else {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, m_dOVRadius, 0.0 ), Vector3( -m_dOVRadius, 0.0, 0.0 ), pt, *coord );
		}
	}
}

Scalar SphereGeometry::GetArea( ) const
{
	return (FOUR_PI * m_dSqrRadius);
}

static const unsigned int RADIUS_ID = 100;

IKeyframeParameter* SphereGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), RADIUS_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void SphereGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADIUS_ID:
		{
			m_dRadius = *(Scalar*)val.getValue();
			m_dOVRadius = (m_dRadius != 0) ? 1.0/m_dRadius : 0;
			m_dSqrRadius = m_dRadius*m_dRadius;
		}
		break;
	}
}

void SphereGeometry::RegenerateData( )
{
}
