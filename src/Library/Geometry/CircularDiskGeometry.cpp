//////////////////////////////////////////////////////////////////////
//
//  CircularDiskGeometry.cpp - Implementation of the 
//  CircularDiskGeometry class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 24, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CircularDiskGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

CircularDiskGeometry::CircularDiskGeometry(
	const Scalar radius_, 
	const unsigned char chAxis_ 
	) :
  radius( radius_ ),
  chAxis( chAxis_ ),
  sqrRadius( radius_*radius_ )
{
	if( radius_ > 0 ){
		OVRadius = 1.0 / radius_;
	} else {
		GlobalLog()->PrintSourceError( "CircularDiskGeometry:: radius is <= 0", __FILE__, __LINE__ );
	}
}

CircularDiskGeometry::~CircularDiskGeometry( )
{
}

void CircularDiskGeometry::GenerateMesh( )
{
}

void CircularDiskGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	HIT	h;
	
	bool bFrontSideWasHit = false;
	Vector3	vNormal;

	switch( chAxis )
	{
	case 'x':
		vNormal = Vector3( 1, 0, 0 );
		break;
	case 'y':
		vNormal = Vector3( 0, 1, 0 );
		break;
	default:
	case 'z':
		vNormal = Vector3( 0, 0, 1 );
		break;
	};

	if( bHitFrontFaces ) {
		RayPlaneIntersection( ri.ray, h, vNormal );	
		bFrontSideWasHit = h.bHit;
	}

	if( !h.bHit && bHitBackFaces ) {
		RayPlaneIntersection( ri.ray, h, -vNormal );	
	}

	if( h.bHit )
	{
		Point3 intersec = ri.ray.PointAtLength( h.dRange );
		if( intersec.x*intersec.x+intersec.y*intersec.y <= sqrRadius )
		{
			ri.bHit = h.bHit;
			ri.range = h.dRange;
			ri.range2 = h.dRange2;

			if( bFrontSideWasHit ) {
				ri.vNormal = vNormal;

				if( bComputeExitInfo ) {
					ri.vNormal2 = -vNormal;
				}
			} else {
				ri.vNormal = -vNormal;

				if( bComputeExitInfo ) {
					ri.vNormal2 = vNormal;
				}
			}

			switch( chAxis )
			{
			case 'x':
				ri.ptCoord.x = intersec.z * OVRadius;
				ri.ptCoord.y = intersec.y * OVRadius;
				break;
			case 'y':
				ri.ptCoord.x = intersec.x * OVRadius;
				ri.ptCoord.y = intersec.z * OVRadius;
				break;
			default:
			case 'z':
				ri.ptCoord.x = intersec.x * OVRadius;
				ri.ptCoord.y = intersec.y * OVRadius;
				break;
			}
		}
		else
			ri.bHit = false;
	}
}

bool CircularDiskGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	HIT	h;
	h.bHit = false;

	Vector3	vNormal;

	switch( chAxis )
	{
	case 'x':
		vNormal = Vector3( 1, 0, 0 );
		break;
	case 'y':
		vNormal = Vector3( 0, 1, 0 );
		break;
	default:
	case 'z':
		vNormal = Vector3( 0, 0, 1 );
		break;
	};

	if( bHitFrontFaces ) {
		RayPlaneIntersection( ray, h, vNormal );		
	}

	if( !h.bHit && bHitBackFaces ) {
		RayPlaneIntersection( ray, h, -vNormal );	


	}

	if( h.bHit ) {
		Point3 intersec = ray.PointAtLength( h.dRange );
		if( intersec.x*intersec.x+intersec.y*intersec.y > sqrRadius ) {
			h.bHit = false;
		}
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void CircularDiskGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0, 0, 0 );
	radius = INFINITY;
}

BoundingBox CircularDiskGeometry::GenerateBoundingBox() const
{
	// we don't set bounding boxes... the circular disk geometry prefers to be
	// called directly, since it is so fast naturally

	// However in order to be complete, we still implement it...
	Point3 ll, ur;
	switch( chAxis )
	{
	case 'x':
		ll = Point3( -0.001, -radius, -radius );
		ur = Point3( 0.001, radius, radius );
		break;
	case 'y':
		ll = Point3( -radius, -0.001, -radius );
		ur = Point3( radius, 0.001, radius );
		break;
	default:
	case 'z':
		ll = Point3( -radius, -radius, -0.001 );
		ur = Point3( radius, radius, 0.001 );
		break;
	};

	return BoundingBox( ll, ur );
}

void CircularDiskGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point2 ptOnDisk = GeometricUtilities::PointOnDisk( radius, Point2( prand.x, prand.y ) );
	
	if( point ) {
		switch( chAxis )
		{
		case 'x':
			*point = Point3( 0, ptOnDisk.x, ptOnDisk.y ); 
			break;
		case 'y':
			*point = Point3( ptOnDisk.x, 0, ptOnDisk.y );
			break;
		default:
		case 'z':
			*point = Point3( ptOnDisk.x, ptOnDisk.y, 0 );
			break;
		};
	}

	if( normal ) {
		switch( chAxis )
		{
		case 'x':
			*normal = Vector3( 1, 0, 0 );
			break;
		case 'y':
			*normal = Vector3( 0, 1, 0 );
			break;
		default:
		case 'z':
			*normal = Vector3( 0, 0, 1 );
			break;
		};
	}

	if( coord ) {
		coord->x = ptOnDisk.x * OVRadius;
		coord->y = ptOnDisk.y * OVRadius;
	}
}

Scalar CircularDiskGeometry::GetArea( ) const
{
	return PI*sqrRadius;
}

static const unsigned int RADIUS_ID = 100;

IKeyframeParameter* CircularDiskGeometry::KeyframeFromParameters( const String& name, const String& value )
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

void CircularDiskGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADIUS_ID:
		{
			radius = *(Scalar*)val.getValue();
			OVRadius = (radius != 0) ? 1.0/radius : 0;
			sqrRadius = radius*radius;
		}
		break;
	}
}

void CircularDiskGeometry::RegenerateData( )
{
}

