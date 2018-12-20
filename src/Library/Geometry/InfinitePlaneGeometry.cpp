//////////////////////////////////////////////////////////////////////
//
//  InfinitePlaneGeometry.cpp - Implementation of the 
//  InfinitePlaneGeometry class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "InfinitePlaneGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

InfinitePlaneGeometry::InfinitePlaneGeometry( const Scalar xTile_, const Scalar yTile_ ) :
  xTile( xTile_ ), yTile( yTile_ ), OVXTile( 0 ), OVYTile( 0 )
{
	RegenerateData();
}

InfinitePlaneGeometry::~InfinitePlaneGeometry( )
{
}

void InfinitePlaneGeometry::GenerateMesh( )
{
}

void InfinitePlaneGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	HIT	h;
	bool	bFrontSideWasHit = false;

	if( bHitFrontFaces ) {
		RayPlaneIntersection( ri.ray, h, Vector3( 0, 0, 1 ) );
		bFrontSideWasHit = true;
	}

	if( !bFrontSideWasHit && bHitBackFaces ) {
		RayPlaneIntersection( ri.ray, h, Vector3( 0, 0, -1 ) );
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		if( bFrontSideWasHit ) {
			ri.vNormal = Vector3( 0, 0, 1 );

			if( bComputeExitInfo ) {
				ri.vNormal2 = Vector3( 0, 0, -1 );
			}
		} else {
			ri.vNormal = Vector3( 0, 0, -1 );

			if( bComputeExitInfo ) {
				ri.vNormal2 = Vector3( 0, 0, 1 );
			}
		}

		Point3 intersec = ri.ray.PointAtLength( ri.range );

		// Texture co-ordinates
		if( intersec.y > 0 && intersec.x <= 0) {
			ri.ptCoord = Point2( 1.0 - fmod(fabs(intersec.x),xTile)*OVXTile, 1.0 - fmod(fabs(intersec.y),yTile)*OVYTile );
		} else if( intersec.x > 0 && intersec.y <=0 ) {
			ri.ptCoord = Point2( fmod(fabs(intersec.x),xTile)*OVXTile, fmod(fabs(intersec.y),yTile)*OVYTile );
		} else if( intersec.y > 0 && intersec.x > 0 ) {
			ri.ptCoord = Point2( fmod(fabs(intersec.x),xTile)*OVXTile, 1.0 - fmod(fabs(intersec.y),yTile)*OVYTile );
		} else {
			ri.ptCoord = Point2( 1.0 - fmod(fabs(intersec.x),xTile)*OVXTile, fmod(fabs(intersec.y),yTile)*OVYTile );
		}
	}
}

bool InfinitePlaneGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	HIT	h;

	if( bHitFrontFaces ) {
		RayPlaneIntersection( ray, h, Vector3( 0, 0, 1 ) );
	}

	if( !h.bHit && bHitBackFaces ) {
		RayPlaneIntersection( ray, h, Vector3( 0, 0, -1 ) );
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void InfinitePlaneGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0, 0, 0 );
	radius = INFINITY;
}

BoundingBox InfinitePlaneGeometry::GenerateBoundingBox() const
{
	return BoundingBox();
}

void InfinitePlaneGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point2 pt = Point2( -INFINITY/2 + prand.x*INFINITY, -INFINITY/2 + prand.y*INFINITY );

	if( point ) {
		*point = Point3( pt.x, pt.y, 0 );
	}
	
	if( normal ) {
		*normal = Vector3( 0, 0, 1 );
	}
	
	if( coord )
	{
		if( pt.y > 0 && pt.x <= 0) {
			*coord = Point2( 1.0 - fmod(fabs(pt.x),xTile)*OVXTile, 1.0 - fmod(fabs(pt.y),yTile)*OVYTile );
		} else if( pt.x > 0 && pt.y <=0 ) {
			*coord = Point2( fmod(fabs(pt.x),xTile)*OVXTile, fmod(fabs(pt.y),yTile)*OVYTile );
		} else if( pt.y > 0 && pt.x > 0 ) {
			*coord = Point2( fmod(fabs(pt.x),xTile)*OVXTile, 1.0 - fmod(fabs(pt.y),yTile)*OVYTile );
		} else {
			*coord = Point2( 1.0 - fmod(fabs(pt.x),xTile)*OVXTile, fmod(fabs(pt.y),yTile)*OVYTile );
		}
	}
}

Scalar InfinitePlaneGeometry::GetArea( ) const
{
	return INFINITY;
}

static const unsigned int XTILE_ID = 100;
static const unsigned int YTILE_ID = 101;

IKeyframeParameter* InfinitePlaneGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "xtile" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), XTILE_ID );
	} else if( name == "ytile" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), YTILE_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void InfinitePlaneGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case XTILE_ID:
		{
			xTile = *(Scalar*)val.getValue();
		}
		break;
	case YTILE_ID:
		{
			yTile = *(Scalar*)val.getValue();
		}
		break;
	}
}

void InfinitePlaneGeometry::RegenerateData( )
{
	if( xTile >  0 ) {
		OVXTile = 1.0 / xTile;
	} else {
		GlobalLog()->PrintSourceError( "InfinitePlaneGeometry:: xTile <= 0", __FILE__, __LINE__ );
	}

	if( yTile > 0 ) {
		OVYTile = 1.0 / yTile;
	} else {
		GlobalLog()->PrintSourceError( "InfinitePlaneGeometry:: yTile <= 0", __FILE__, __LINE__ );
	}
}

