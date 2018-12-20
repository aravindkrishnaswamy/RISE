//////////////////////////////////////////////////////////////////////
//
//  ClippedPlaneGeometry.cpp - Implementation of the 
//  ClippedPlaneGeometry class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 16, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClippedPlaneGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

ClippedPlaneGeometry::ClippedPlaneGeometry(
	const Point3 (&vP_)[4],
	const bool bDoubleSided_
	) : 
  bDoubleSided( bDoubleSided_ )
{
	vP[0] = vP_[0];
	vP[1] = vP_[1];
	vP[2] = vP_[2];
	vP[3] = vP_[3];

	RegenerateData();
}

ClippedPlaneGeometry::~ClippedPlaneGeometry( )
{
}

void ClippedPlaneGeometry::GenerateMesh( )
{
}

void ClippedPlaneGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	TRIANGLE_HIT	h;

	// Early rejection is based on whether we are to consider front facing triangles or 
	// back facing triangles and so on...

	// If we are not to hit front faces and we are front facing, then beat it!
	if( !bHitFrontFaces  ) {
		if( Vector3Ops::Dot(vNormal, ri.ray.dir) < 0 ) {
			return;
		}
	}

	// If we are not to hit back faces and we are back facing, then also beat it
	if( !bHitBackFaces || !bDoubleSided ) {
		if( Vector3Ops::Dot(vNormal, ri.ray.dir) > 0 ) {
			return;
		}
	}
	
	bool	bTriA = false;
	// Check against the two triangles
	RayTriangleIntersection( ri.ray, h, vP[0], vEdgesA[0], vEdgesA[1] );

	if( h.bHit ) {
		bTriA = true;
	} else {
		RayTriangleIntersection( ri.ray, h, vP[0], vEdgesB[0], vEdgesB[1] );
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.vNormal = vNormal;

		if( bComputeExitInfo ) {
			ri.vNormal2 = -vNormal;
		}

		Point2		uv[3];

		if( bTriA )
		{
			uv[0] = Point2( 1.0, 0.0 );
			uv[1] = Point2( 0.0, 0.0 );
			uv[2] = Point2( 0.0, 1.0 );
		}
		else
		{
			uv[0] = Point2( 1.0, 0.0 );
			uv[1] = Point2( 0.0, 1.0 );
			uv[2] = Point2( 1.0, 1.0 );
		}

		// Texture co-ordinates
		ri.ptCoord = Point2Ops::mkPoint2(uv[0],
			Vector2Ops::mkVector2(uv[1],uv[0])*h.alpha+
			Vector2Ops::mkVector2(uv[2],uv[0])*h.beta );
	}
	
}

bool ClippedPlaneGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	TRIANGLE_HIT	h;

	// If we are not to hit front faces and we are front facing, then beat it!
	if( !bHitFrontFaces  ) {
		if( Vector3Ops::Dot(vNormal, ray.dir) < 0 ) {
			return false;
		}
	}

	// If we are not to hit back faces and we are back facing, then also beat it
	if( !bHitBackFaces || !bDoubleSided ) {
		if( Vector3Ops::Dot(vNormal, ray.dir) > 0 ) {
			return false;
		}
	}

	{
		h.bHit = false;
		// Check against the two triangles
		RayTriangleIntersection( ray, h, vP[0], vEdgesA[0], vEdgesA[1] );

		if( !h.bHit ) {
			RayTriangleIntersection( ray, h, vP[0], vEdgesB[0], vEdgesB[1] );
		}
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void ClippedPlaneGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	int			i;

	Point3		ptMin( INFINITY, INFINITY, INFINITY );
	Point3		ptMax( -INFINITY, -INFINITY, -INFINITY );

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	for( i=0; i<4; i++ )
	{
		if( vP[i].x < ptMin.x ) {
			ptMin.x = vP[i].x;
		}
		if( vP[i].y < ptMin.y ) {
			ptMin.y = vP[i].y;
		}
		if( vP[i].z < ptMin.z ) {
			ptMin.z = vP[i].z;
		}
		if( vP[i].x > ptMax.x ) {
			ptMax.x = vP[i].x;
		}
		if( vP[i].y > ptMax.y ) {
			ptMax.y = vP[i].y;
		}
		if( vP[i].z > ptMax.z ) {
			ptMax.z = vP[i].z;
		}
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMax, ptMin, 0.5 );
	radius = 0;

	// Go through all the points again, and calculate the radius of the sphere
	// Which is the largest magnitude of the vector from the center to each point
	for( i=0; i<4; i++ )
	{			
		Vector3			r = Vector3Ops::mkVector3( vP[i], ptCenter );
		const Scalar	d = Vector3Ops::Magnitude(r);

		if( d > radius ) {
			radius = d;
		}
	}
}

BoundingBox ClippedPlaneGeometry::GenerateBoundingBox() const
{
	// The Bbox is basically the four points
	Point3 ll = Point3( INFINITY, INFINITY, INFINITY );
	Point3 ur = Point3( -INFINITY, -INFINITY, -INFINITY );

	for( unsigned int i=0; i<4; i++ )
	{
		if( vP[i].x < ll.x ) {
			ll.x = vP[i].x;
		}
		if( vP[i].x > ur.x ) {
			ur.x = vP[i].x;
		}

		if( vP[i].y < ll.y ) {
			ll.y = vP[i].y;
		}
		if( vP[i].y > ur.y ) {
			ur.y = vP[i].y;
		}

		if( vP[i].z < ll.z ) {
			ll.z = vP[i].z;
		}
		if( vP[i].z > ur.z ) {
			ur.z = vP[i].z;
		}
	}

	// Add a little fudge, just to get around numerical problems
	return BoundingBox( 
		Point3( ll.x + (-0.001), ll.y + (-0.001), ll.z + (-0.001) ),
		Point3( ur.x + (0.001), ur.y + (0.001), ur.z + (0.001) ) );
}

void ClippedPlaneGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	if( point ) {
		*point = Point3Ops::mkPoint3(vP[0], vEdgesA[0]*prand.x + vEdgesB[1]*prand.y);

		// Pull the point out just a little in the direction of the normal
		// So that when back facing and a clipped plane is a light source, 
		// it occludes anyone behind it... 
		*point = Point3Ops::mkPoint3(*point, vNormal * 0.00001);
	}

	if( normal ) {
		*normal = vNormal;
	}

	if( coord ) {
		*coord = Point2( prand.x, prand.y );	
	}
}

Scalar ClippedPlaneGeometry::GetArea( ) const
{
	// The area is the area of the, which is width * height
	return (Vector3Ops::Magnitude(vEdgesA[0]) * Vector3Ops::Magnitude(vEdgesB[1]));
}

static const unsigned int PTA_ID = 100;
static const unsigned int PTB_ID = 101;
static const unsigned int PTC_ID = 102;
static const unsigned int PTD_ID = 103;

IKeyframeParameter* ClippedPlaneGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "pta" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTA_ID );
		}
	} else if( name == "ptb" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTB_ID );
		}
	} else if( name == "ptc" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTC_ID );
		}
	} else if( name == "ptd" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTD_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void ClippedPlaneGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case PTA_ID:
	case PTB_ID:
	case PTC_ID:
	case PTD_ID:
		{
			vP[val.getID()-PTA_ID] = *(Point3*)val.getValue();
		}
		break;
	}
}

void ClippedPlaneGeometry::RegenerateData( )
{
	vEdgesA[0] = Vector3Ops::mkVector3( vP[1], vP[0] );
	vEdgesA[1] = Vector3Ops::mkVector3( vP[2], vP[0] );

	vEdgesB[0] = Vector3Ops::mkVector3( vP[2], vP[0] );
	vEdgesB[1] = Vector3Ops::mkVector3( vP[3], vP[0] );

	vNormal = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgesA[0], vEdgesA[1] ));
}

