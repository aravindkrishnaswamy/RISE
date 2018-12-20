//////////////////////////////////////////////////////////////////////
//
//  CylinderGeometry.cpp - Implementation of the cylinder class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CylinderGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

CylinderGeometry::CylinderGeometry( const int chAxis, const Scalar dRadius, const Scalar dHeight ) : 
  m_chAxis( chAxis ),
  m_dRadius( dRadius ),
  m_dHeight( dHeight )
{
	RegenerateData();
}

CylinderGeometry::~CylinderGeometry( )
{
}

void CylinderGeometry::GenerateMesh( )
{

}

void CylinderGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool , const bool , const bool bComputeExitInfo ) const
{
	/* IMPLEMENT THIS OPTO!
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInCylinder = IsPointInsideCylinder( ri.ray.origin, m_dRadius, m_ptCenter );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInCylinder )
		return;

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInCylinder )
		return;
	*/

	HIT	h;
	bool bHitFarSide = false;
	switch( m_chAxis )
	{
	case 'x':
	default:
		RayXCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'y':
		RayYCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'z':
		RayZCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		GeometricUtilities::CylinderNormal( ri.ptIntersection, m_chAxis, ri.vNormal );

		if( bHitFarSide ) {
			ri.vNormal = -ri.vNormal;
		}

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			GeometricUtilities::CylinderNormal( ri.ptExit, m_chAxis, ri.vNormal2 );
		}

		// Calculate UV co-ordinates
		GeometricUtilities::CylinderTextureCoord( ri.ptIntersection, m_chAxis, m_dOVRadius, m_dAxisMin, m_dAxisMax, ri.ptCoord );
	}
}

bool CylinderGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool , const bool ) const
{
	/* IMPLEMENT THIS OPTO!
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInCylinder = IsPointInsideCylinder( ray.origin, m_dRadius, m_ptCenter );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInCylinder )
		return false;

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInCylinder )
		return false;
		*/

	HIT	h;
	bool bHitFarSide = false;
	switch( m_chAxis )
	{
	case 'x':
		RayXCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'y':
		RayYCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'z':
		RayZCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void CylinderGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0, 0, 0 );
	radius = m_dRadius > m_dHeight ? m_dRadius : m_dHeight;
}

BoundingBox CylinderGeometry::GenerateBoundingBox() const
{
	// Bounding box will depend on the axis of the cylinder
	Point3 ll, ur;
	switch( m_chAxis )
	{
	case 'x':
		ll = Point3( m_dAxisMin, -m_dRadius, -m_dRadius );
		ur = Point3( m_dAxisMax, m_dRadius, m_dRadius );
		break;
	case 'y':
		ll = Point3( -m_dRadius, m_dAxisMin, -m_dRadius );
		ur = Point3( m_dRadius, m_dAxisMax, m_dRadius );
		break;
	case 'z':
		ll = Point3( -m_dRadius, -m_dRadius, m_dAxisMin );
		ur = Point3( m_dRadius, m_dRadius, m_dAxisMax );
		break;
	}

	return BoundingBox( ll, ur );
}

void CylinderGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point3 pt;
	GeometricUtilities::PointOnCylinder( Point2(prand.x,prand.y), m_chAxis, m_dRadius, m_dAxisMin, m_dAxisMax, pt );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		GeometricUtilities::CylinderNormal( pt, m_chAxis, *normal );
	}

	if( coord ) {
		GeometricUtilities::CylinderTextureCoord( pt, m_chAxis, m_dOVRadius, m_dAxisMin, m_dAxisMax, *coord );
	}
}

Scalar CylinderGeometry::GetArea( ) const
{
	return m_dRadius * m_dRadius * PI * m_dHeight;
}

static const unsigned int RADIUS_ID = 100;
static const unsigned int HEIGHT_ID = 101;

IKeyframeParameter* CylinderGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "pta" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), RADIUS_ID );
	} else if( name == "ptb" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), HEIGHT_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void CylinderGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADIUS_ID:
		{
			m_dRadius = *(Scalar*)val.getValue();
		}
		break;
	case HEIGHT_ID:
		{
			m_dHeight = *(Scalar*)val.getValue();
		}
		break;
	}
}

void CylinderGeometry::RegenerateData( )
{
	m_dAxisMin = -m_dHeight/2;
	m_dAxisMax = m_dHeight/2;

	if( m_dRadius > 0 ) {
		m_dOVRadius = 1.0 / m_dRadius;
	} else {
		GlobalLog()->PrintSourceError( "CylinderGeometry:: m_dRadius is <= 0", __FILE__, __LINE__ );
	}
}

