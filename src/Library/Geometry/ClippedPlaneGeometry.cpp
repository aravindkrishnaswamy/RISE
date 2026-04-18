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
#include "GeometryUtilities.h"
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

bool ClippedPlaneGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 1 ) {
		return false;
	}

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );
	const unsigned int rowStride = nU + 1;

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v = Scalar(j) / Scalar(nV);
		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u = Scalar(i) / Scalar(nU);

			// Bilinear interpolation across the four corners:
			// vP[0] at (0,0), vP[1] at (1,0), vP[2] at (1,1), vP[3] at (0,1).
			const Scalar w00 = (1.0 - u) * (1.0 - v);
			const Scalar w10 = u         * (1.0 - v);
			const Scalar w11 = u         * v;
			const Scalar w01 = (1.0 - u) * v;

			const Point3 pos(
				w00 * vP[0].x + w10 * vP[1].x + w11 * vP[2].x + w01 * vP[3].x,
				w00 * vP[0].y + w10 * vP[1].y + w11 * vP[2].y + w01 * vP[3].y,
				w00 * vP[0].z + w10 * vP[1].z + w11 * vP[2].z + w01 * vP[3].z );

			vertices.push_back( pos );
			normals.push_back( vNormal );
			coords.push_back( Point2( u, v ) );
		}
	}

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a = baseIdx + j     * rowStride + i;
			const unsigned int b = baseIdx + j     * rowStride + (i + 1);
			const unsigned int c = baseIdx + (j+1) * rowStride + i;
			const unsigned int d = baseIdx + (j+1) * rowStride + (i + 1);

			tris.push_back( MakeIndexedTriangleSameIdx( a, b, c ) );
			tris.push_back( MakeIndexedTriangleSameIdx( b, d, c ) );
		}
	}

	return true;
}

void ClippedPlaneGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	const Scalar dDotA = Vector3Ops::Dot( vNormalA, ri.ray.Dir() );
	const Scalar dDotB = Vector3Ops::Dot( vNormalB, ri.ray.Dir() );

	const bool bAllowTriA = (bHitFrontFaces || dDotA >= 0) && ((bHitBackFaces && bDoubleSided) || dDotA <= 0);
	const bool bAllowTriB = (bHitFrontFaces || dDotB >= 0) && ((bHitBackFaces && bDoubleSided) || dDotB <= 0);

	TRIANGLE_HIT hTriA;
	TRIANGLE_HIT hTriB;
	hTriA.bHit = false;
	hTriB.bHit = false;

	if( bAllowTriA ) {
		RayTriangleIntersection( ri.ray, hTriA, vP[0], vEdgesA[0], vEdgesA[1] );
	}

	if( bAllowTriB ) {
		RayTriangleIntersection( ri.ray, hTriB, vP[0], vEdgesB[0], vEdgesB[1] );
	}

	TRIANGLE_HIT h;
	h.bHit = false;
	bool bTriA = false;

	if( hTriA.bHit && hTriB.bHit ) {
		bTriA = hTriA.dRange <= hTriB.dRange;
		h = bTriA ? hTriA : hTriB;
	} else if( hTriA.bHit ) {
		bTriA = true;
		h = hTriA;
	} else if( hTriB.bHit ) {
		bTriA = false;
		h = hTriB;
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		const Vector3& vHitNormal = bTriA ? vNormalA : vNormalB;
		ri.vNormal = vHitNormal;

		if( bComputeExitInfo ) {
			ri.vNormal2 = -vHitNormal;
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
	const Scalar dDotA = Vector3Ops::Dot( vNormalA, ray.Dir() );
	const Scalar dDotB = Vector3Ops::Dot( vNormalB, ray.Dir() );

	const bool bAllowTriA = (bHitFrontFaces || dDotA >= 0) && ((bHitBackFaces && bDoubleSided) || dDotA <= 0);
	const bool bAllowTriB = (bHitFrontFaces || dDotB >= 0) && ((bHitBackFaces && bDoubleSided) || dDotB <= 0);

	bool bHit = false;
	Scalar dClosest = dHowFar;

	if( bAllowTriA ) {
		TRIANGLE_HIT hTriA;
		hTriA.bHit = false;
		RayTriangleIntersection( ray, hTriA, vP[0], vEdgesA[0], vEdgesA[1] );

		if( hTriA.bHit && hTriA.dRange >= NEARZERO && hTriA.dRange <= dClosest ) {
			bHit = true;
			dClosest = hTriA.dRange;
		}
	}

	if( bAllowTriB ) {
		TRIANGLE_HIT hTriB;
		hTriB.bHit = false;
		RayTriangleIntersection( ray, hTriB, vP[0], vEdgesB[0], vEdgesB[1] );

		if( hTriB.bHit && hTriB.dRange >= NEARZERO && hTriB.dRange <= dClosest ) {
			bHit = true;
			dClosest = hTriB.dRange;
		}
	}

	return bHit;
}

void ClippedPlaneGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	int			i;

	Point3		ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3		ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

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
	Point3 ll = Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3 ur = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

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

SurfaceDerivatives ClippedPlaneGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	// Use the first edge as dpdu direction, then derive dpdv from normal x dpdu
	Vector3 edge = Vector3Ops::mkVector3( vP[1], vP[0] );
	Scalar edgeLen = Vector3Ops::Magnitude( edge );
	if( edgeLen > NEARZERO ) {
		sd.dpdu = edge * (1.0 / edgeLen);
	} else {
		sd.dpdu = Vector3( 1, 0, 0 );
	}

	sd.dpdv = Vector3Ops::Cross( objSpaceNormal, sd.dpdu );
	Scalar dpdvLen = Vector3Ops::Magnitude( sd.dpdv );
	if( dpdvLen > NEARZERO ) {
		sd.dpdv = sd.dpdv * (1.0 / dpdvLen);
	}

	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );

	// Compute UV from barycentric position on the quad
	Vector3 toPoint = Vector3Ops::mkVector3( objSpacePoint, vP[0] );
	Scalar uLen = Vector3Ops::Magnitude( vEdgesA[0] );
	Scalar vLen = Vector3Ops::Magnitude( vEdgesB[1] );
	if( uLen > NEARZERO && vLen > NEARZERO ) {
		sd.uv = Point2(
			Vector3Ops::Dot( toPoint, vEdgesA[0] ) / (uLen * uLen),
			Vector3Ops::Dot( toPoint, vEdgesB[1] ) / (vLen * vLen)
		);
	} else {
		sd.uv = Point2( 0, 0 );
	}

	sd.valid = true;
	return sd;
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

	vNormalA = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgesA[0], vEdgesA[1] ));
	vNormalB = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgesB[0], vEdgesB[1] ));
	vNormal = Vector3Ops::Normalize(Vector3Ops::WeightedAverage2(vNormalA, vNormalB, 0.5, 0.5));
}
