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
#include "GeometryUtilities.h"
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

bool CircularDiskGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 3 ) {
		return false;
	}

	const unsigned int nU = detail;  // angular sectors
	const unsigned int nV = detail;  // radial rings
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );
	const unsigned int rowStride = nU + 1;

	Vector3 axisNormal;
	switch( chAxis ) {
		case 'x':
			axisNormal = Vector3( 1.0, 0.0, 0.0 );
			break;
		case 'y':
			axisNormal = Vector3( 0.0, 1.0, 0.0 );
			break;
		case 'z':
		default:
			axisNormal = Vector3( 0.0, 0.0, 1.0 );
			break;
	}

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v = Scalar(j) / Scalar(nV);
		const Scalar r = v * radius;

		// At the center (v=0, r=0) every i-vertex collapses to the origin with the
		// same axis normal.  If we let u vary across those vertices, a non-constant
		// displacement function gives each one a different height along the normal
		// and the center fan opens into a star-shaped crack.  Canonicalise u at the
		// center so every collapsed vertex gets the same displacement.
		const bool atCenter = (j == 0);

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = atCenter ? 0.0 : Scalar(i) / Scalar(nU);
			const Scalar theta    = u * TWO_PI;
			const Scalar cosTheta = cos(theta);
			const Scalar sinTheta = sin(theta);

			Point3 pos;
			switch( chAxis ) {
				case 'x':
					pos = Point3( 0.0,          r * cosTheta, r * sinTheta );
					break;
				case 'y':
					pos = Point3( r * sinTheta, 0.0,          r * cosTheta );
					break;
				case 'z':
				default:
					pos = Point3( r * cosTheta, r * sinTheta, 0.0 );
					break;
			}

			vertices.push_back( pos );
			normals.push_back( axisNormal );
			coords.push_back( Point2( u, v ) );
		}
	}

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a = baseIdx + j     * rowStride + i;
			const unsigned int b = baseIdx + j     * rowStride + (i + 1);
			const unsigned int c = baseIdx + (j+1) * rowStride + i;
			const unsigned int d = baseIdx + (j+1) * rowStride + (i + 1);

			tris.push_back( MakeIndexedTriangleSameIdx( a, c, b ) );
			tris.push_back( MakeIndexedTriangleSameIdx( b, c, d ) );
		}
	}

	return true;
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
	radius = RISE_INFINITY;
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

SurfaceDerivatives CircularDiskGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;
	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );
	sd.valid = true;

	switch( chAxis )
	{
	case 'x':
		sd.dpdu = Vector3( 0, 0, 1 );
		sd.dpdv = Vector3( 0, 1, 0 );
		sd.uv = Point2( objSpacePoint.z * OVRadius, objSpacePoint.y * OVRadius );
		break;
	case 'y':
		sd.dpdu = Vector3( 1, 0, 0 );
		sd.dpdv = Vector3( 0, 0, 1 );
		sd.uv = Point2( objSpacePoint.x * OVRadius, objSpacePoint.z * OVRadius );
		break;
	default:
	case 'z':
		sd.dpdu = Vector3( 1, 0, 0 );
		sd.dpdv = Vector3( 0, 1, 0 );
		sd.uv = Point2( objSpacePoint.x * OVRadius, objSpacePoint.y * OVRadius );
		break;
	}

	return sd;
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

