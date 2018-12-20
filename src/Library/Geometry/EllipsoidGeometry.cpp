//////////////////////////////////////////////////////////////////////
//
//  EllipsoidGeometry.cpp - Implementation of the ellipsoid class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 7, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EllipsoidGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

EllipsoidGeometry::EllipsoidGeometry( const Vector3& vRadius ) : 
  m_vRadius( vRadius )
{
	RegenerateData();
}

EllipsoidGeometry::~EllipsoidGeometry( )
{
}

void EllipsoidGeometry::GenerateMesh( )
{

}

void EllipsoidGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	HIT	h;
	RayQuadricIntersection( ri.ray, h, Point3(0,0,0), Q );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		ri.vNormal = Vector3Ops::Normalize(
			Vector3( Q._00*ri.ptIntersection.x, 
					 Q._11*ri.ptIntersection.y,
					 Q._22*ri.ptIntersection.z )
			);

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			ri.vNormal2 = Vector3Ops::Normalize(
				Vector3( Q._00*ri.ptExit.x, 
						 Q._11*ri.ptExit.y,
						 Q._22*ri.ptExit.z )
				);
		}

		// Use spherical UV
		GeometricUtilities::SphereTextureCoord( 
			Vector3( 0.0, m_OVmaxRadius, 0.0 ), 
			Vector3( -m_OVmaxRadius, 0.0, 0.0 ), 
			ri.vNormal, ri.ptCoord 
			);
	}
}

bool EllipsoidGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	HIT	h;
	RayQuadricIntersection( ray, h, Point3(0,0,0), Q );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void EllipsoidGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = r_max( r_max(m_vRadius.x, m_vRadius.y), m_vRadius.z );
}

BoundingBox EllipsoidGeometry::GenerateBoundingBox() const
{
	return BoundingBox( 
		Point3( -m_vRadius.x, -m_vRadius.y, -m_vRadius.z ),
		Point3( m_vRadius.x, m_vRadius.y, m_vRadius.z ) );
}

void EllipsoidGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point3 pt = GeometricUtilities::PointOnEllipsoid( Point3(0,0,0), m_vRadius, Point2( prand.x, prand.y ) );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		*normal = Vector3Ops::Normalize(
					Vector3(	Q._00*pt.x, 
								Q._11*pt.y,
								Q._22*pt.z )
					);
	}

	if( coord ) {
		if( normal ) {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, m_OVmaxRadius, 0.0 ), Vector3( -m_OVmaxRadius, 0.0, 0.0 ), *normal, *coord );
		} else {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, m_OVmaxRadius, 0.0 ), Vector3( -m_OVmaxRadius, 0.0, 0.0 ), pt, *coord );
		}
	}
}

Scalar EllipsoidGeometry::GetArea( ) const
{
	// This is an approximation taken from:
	// http://home.att.net/~numericana/answer/ellipsoid.htm
	const Scalar p = log(3.0)/log(2.0);
	
	const Scalar ap = pow( m_vRadius.x, p );
	const Scalar bp = pow( m_vRadius.y, p );
	const Scalar cp = pow( m_vRadius.z, p );

	return TWO_PI * (ap*bp + ap*cp + bp*cp);
}

static const unsigned int RADII_ID = 100;

IKeyframeParameter* EllipsoidGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "radii" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, RADII_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void EllipsoidGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADII_ID:
		{
			m_vRadius = *(Vector3*)val.getValue();
		}
		break;
	}
}

void EllipsoidGeometry::RegenerateData( )
{
	Q = Matrix4Ops::Identity();
	Q._00 = 1.0/((m_vRadius.x/2)*(m_vRadius.x/2));
	Q._11 = 1.0/((m_vRadius.y/2)*(m_vRadius.y/2));
	Q._22 = 1.0/((m_vRadius.z/2)*(m_vRadius.z/2));
	Q._33 = -1.0;

	m_OVmaxRadius = 1.0 / (r_max( r_max(m_vRadius.x, m_vRadius.y), m_vRadius.z ));
}

