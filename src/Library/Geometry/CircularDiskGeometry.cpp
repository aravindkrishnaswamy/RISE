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

void CircularDiskGeometry::DiskUVFromPosition( const Point3& pt, Point2& uv ) const
{
	// Inverse of CircularDiskGeometry::TessellateToMesh:
	//   axis 'x':  pos = (0, r·cos(θ), r·sin(θ))
	//   axis 'y':  pos = (r·sin(θ), 0,  r·cos(θ))
	//   axis 'z':  pos = (r·cos(θ), r·sin(θ), 0)
	// with θ = u·2π in [0, 2π) and r = v·R in [0, R].
	Scalar a = 0.0;  // the parameter playing the cos role
	Scalar b = 0.0;  // the parameter playing the sin role
	switch( chAxis )
	{
	case 'x':
		a = pt.y;  // r·cos(θ)
		b = pt.z;  // r·sin(θ)
		break;
	case 'y':
		a = pt.z;  // r·cos(θ)
		b = pt.x;  // r·sin(θ)
		break;
	default:
	case 'z':
		a = pt.x;  // r·cos(θ)
		b = pt.y;  // r·sin(θ)
		break;
	}

	const Scalar rad = std::sqrt( a * a + b * b );
	uv.y = rad * OVRadius;

	if( rad < NEARZERO ) {
		// Disk centre — θ is undefined; canonicalise to u = 0 to match
		// TessellateToMesh's pole-collapse rule (atCenter ⇒ u = 0).
		uv.x = 0.0;
		return;
	}
	Scalar theta = std::atan2( b, a );
	if( theta < 0.0 ) {
		theta += TWO_PI;
	}
	uv.x = theta / TWO_PI;
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

		// Membership test must use the two in-plane axes for the disk's
		// orientation, not always (x, y).  The pre-fix code always tested
		// intersec.x² + intersec.y² which silently let X- and Y-axis disks
		// "hit" points arbitrarily far along their out-of-plane radial
		// direction.
		Scalar inPlaneSqr = 0.0;
		switch( chAxis )
		{
		case 'x':
			inPlaneSqr = intersec.y * intersec.y + intersec.z * intersec.z;
			break;
		case 'y':
			inPlaneSqr = intersec.x * intersec.x + intersec.z * intersec.z;
			break;
		default:
		case 'z':
			inPlaneSqr = intersec.x * intersec.x + intersec.y * intersec.y;
			break;
		}

		if( inPlaneSqr <= sqrRadius )
		{
			ri.bHit = h.bHit;
			ri.range = h.dRange;
			ri.range2 = h.dRange2;
			ri.ptIntersection = intersec;

			if( bFrontSideWasHit ) {
				ri.vNormal = vNormal;
				ri.vGeomNormal = vNormal;	// flat disk: shading == geometric

				if( bComputeExitInfo ) {
					ri.vNormal2 = -vNormal;
					ri.vGeomNormal2 = ri.vNormal2;
				}
			} else {
				ri.vNormal = -vNormal;
				ri.vGeomNormal = -vNormal;	// flat disk: shading == geometric

				if( bComputeExitInfo ) {
					ri.vNormal2 = vNormal;
					ri.vGeomNormal2 = ri.vNormal2;
				}
			}

			// Polar (u, v) matching CircularDiskGeometry::TessellateToMesh:
			//   forward formula:  pos_xaxis = (0, r·cos(θ), r·sin(θ))
			//                     pos_yaxis = (r·sin(θ), 0, r·cos(θ))
			//                     pos_zaxis = (r·cos(θ), r·sin(θ), 0)
			//   with θ = u·2π and r = v·R, so the inverse is
			//   v = sqrt(inPlaneSqr) / R, u = (θ wrapped to [0, 2π)) / (2π).
			DiskUVFromPosition( intersec, ri.ptCoord );
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
		Scalar inPlaneSqr = 0.0;
		switch( chAxis )
		{
		case 'x':
			inPlaneSqr = intersec.y * intersec.y + intersec.z * intersec.z;
			break;
		case 'y':
			inPlaneSqr = intersec.x * intersec.x + intersec.z * intersec.z;
			break;
		default:
		case 'z':
			inPlaneSqr = intersec.x * intersec.x + intersec.y * intersec.y;
			break;
		}
		if( inPlaneSqr > sqrRadius ) {
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

	Point3 pt;
	switch( chAxis )
	{
	case 'x':
		pt = Point3( 0, ptOnDisk.x, ptOnDisk.y );
		break;
	case 'y':
		// PointOnDisk returns 2D (x, y).  For Y-axis disk the in-plane
		// axes are (x, z), but TessellateToMesh treats (a=z, b=x) (so
		// pos = (r·sin(θ), 0, r·cos(θ))).  Map ptOnDisk.y → x and
		// ptOnDisk.x → z so DiskUVFromPosition produces the same UV
		// convention regardless of axis.
		pt = Point3( ptOnDisk.y, 0, ptOnDisk.x );
		break;
	default:
	case 'z':
		pt = Point3( ptOnDisk.x, ptOnDisk.y, 0 );
		break;
	}

	if( point ) {
		*point = pt;
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
		// Polar (u, v) ∈ [0, 1]², matching TessellateToMesh.
		DiskUVFromPosition( pt, *coord );
	}
}

SurfaceDerivatives CircularDiskGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;
	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );
	sd.valid = true;

	// Convention: (dpdu, dpdv, n) right-handed per
	// docs/GEOMETRY_DERIVATIVES.md.  X- and Y-axis disks originally
	// returned left-handed frames; swap the two tangents to fix.
	switch( chAxis )
	{
	case 'x':
		sd.dpdu = Vector3( 0, 1, 0 );
		sd.dpdv = Vector3( 0, 0, 1 );
		sd.uv = Point2( objSpacePoint.y * OVRadius, objSpacePoint.z * OVRadius );
		break;
	case 'y':
		sd.dpdu = Vector3( 0, 0, 1 );
		sd.dpdv = Vector3( 1, 0, 0 );
		sd.uv = Point2( objSpacePoint.z * OVRadius, objSpacePoint.x * OVRadius );
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

