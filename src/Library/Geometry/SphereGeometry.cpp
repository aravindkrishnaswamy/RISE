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
#include "GeometryUtilities.h"
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

bool SphereGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 3 ) {
		return false;
	}

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );
	const unsigned int rowStride = nU + 1;

	// Parameterization matches GeometricUtilities::SphereTextureCoord(vUp=Y, vForward=-X):
	//   phi   = v * PI   (0 at north pole (+Y), PI at south pole (-Y))
	//   theta = u * 2*PI (0 at -X, going through +Z, +X, -Z)
	// Position on unit sphere:
	//   dir = (-sin(phi)*cos(theta), cos(phi), sin(phi)*sin(theta))
	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v      = Scalar(j) / Scalar(nV);
		const Scalar phi    = v * PI;
		const Scalar sinPhi = sin(phi);
		const Scalar cosPhi = cos(phi);

		// At the poles (j=0 north, j=nV south) every i-vertex collapses to the same
		// 3D position with the same normal.  If we let u vary across pole vertices,
		// a non-constant displacement function evaluates to different heights per
		// pole vertex and the cap triangles fan out into radial spikes.  Collapse
		// u to a canonical value at the poles so every pole vertex gets the same
		// displacement and the cap stays closed.
		const bool atPole = (j == 0) || (j == nV);

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = atPole ? 0.0 : Scalar(i) / Scalar(nU);
			const Scalar theta    = u * TWO_PI;
			const Scalar sinTheta = sin(theta);
			const Scalar cosTheta = cos(theta);

			const Vector3 dir( -sinPhi * cosTheta, cosPhi, sinPhi * sinTheta );
			const Point3  pos( m_dRadius * dir.x, m_dRadius * dir.y, m_dRadius * dir.z );

			vertices.push_back( pos );
			normals.push_back( dir );
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

SurfaceDerivatives SphereGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	const Scalar r = m_dRadius;

	// Recover spherical coordinates
	// phi = azimuthal angle (around Y axis, in XZ plane)
	const Scalar phi = atan2( objSpacePoint.z, objSpacePoint.x );

	// theta = polar angle from +Y axis
	const Scalar cosTheta = r > NEARZERO ? objSpacePoint.y / r : 0.0;
	const Scalar clampedCosTheta = cosTheta > 1.0 ? 1.0 : (cosTheta < -1.0 ? -1.0 : cosTheta);
	const Scalar theta = acos( clampedCosTheta );
	const Scalar sinTheta = sin( theta );

	const Scalar cosPhi = cos(phi);
	const Scalar sinPhi = sin(phi);

	// Position partial derivatives
	// dpdu = dP/dphi, dpdv = dP/dtheta
	sd.dpdu = Vector3( -r * sinTheta * sinPhi, 0.0, r * sinTheta * cosPhi );
	sd.dpdv = Vector3( r * clampedCosTheta * cosPhi, -r * sinTheta, r * clampedCosTheta * sinPhi );

	// Normal = (x,y,z)/r, so dN/d* = (dP/d*)/r
	sd.dndu = Vector3( -sinTheta * sinPhi, 0.0, sinTheta * cosPhi );
	sd.dndv = Vector3( clampedCosTheta * cosPhi, -sinTheta, clampedCosTheta * sinPhi );

	sd.uv = Point2( phi, theta );
	sd.valid = true;

	return sd;
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
