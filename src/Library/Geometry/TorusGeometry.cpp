//////////////////////////////////////////////////////////////////////
//
//  TorusGeometry.cpp - Implementation of the torus class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TorusGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

TorusGeometry::TorusGeometry( 
	const Scalar dMajorRadius, 
	const Scalar dMinorRadius 
	) : 
  m_dMajorRadius( dMajorRadius ),
  m_dMinorRadius( dMinorRadius)
{
	RegenerateData();
}

TorusGeometry::~TorusGeometry( )
{
}

void TorusGeometry::GenerateMesh( )
{

}

void TorusGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/, const bool bComputeExitInfo ) const
{
	// Do some inside / outside opto

	HIT	h;
	RayTorusIntersection( ri.ray, h, m_dMajorRadius, m_dMinorRadius, m_sqrP0 );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );

		// Calculate the normal by taking the gradient (partial derivatives)
		// Graphics Gems II pp. 256
		
		Scalar d = sqrt( ri.ptIntersection.x*ri.ptIntersection.x + ri.ptIntersection.z*ri.ptIntersection.z );
		Scalar f = (2.0 * (d - m_p0)) / (d*m_sqrP1);

		ri.vNormal.x = ri.ptIntersection.x * f;
		ri.vNormal.y = (ri.ptIntersection.y * 2)/(m_sqrP1);
		ri.vNormal.z = ri.ptIntersection.z * f;

		ri.vNormal = Vector3Ops::Normalize(ri.vNormal);
		
		if( bComputeExitInfo )
		{
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			//Scalar d2 = sqrt( ri.ptExit.x*ri.ptExit.x + ri.ptExit.z*ri.ptExit.z );
			//Scalar f2 = (2.0 * (d2 - m_p0)) / (d2*m_sqrP1);

			ri.vNormal2.x = ri.ptExit.x * f;
			ri.vNormal2.y = (ri.ptExit.y * 2)/(m_sqrP1);
			ri.vNormal2.z = ri.ptExit.z * f;

			ri.vNormal2 = Vector3Ops::Normalize(ri.vNormal2);
		}

		GeometricUtilities::TorusTextureCoord(
			Vector3Ops::Normalize( Vector3( 0.0, 1.0/m_dMajorRadius, 0.0 ) ), 
			Vector3Ops::Normalize( Vector3( -1.0/m_dMajorRadius, 0.0, 0.0 ) ), 
			ri.ptIntersection,
			ri.vNormal, 
			ri.ptCoord 
			);
	}
}

bool TorusGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/ ) const
{
	// Do some inside / outside opto

	HIT	h;
	RayTorusIntersection( ray, h, m_dMajorRadius, m_dMinorRadius, m_sqrP0 );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void TorusGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = m_dMajorRadius+m_dMinorRadius;
}

BoundingBox TorusGeometry::GenerateBoundingBox() const
{
	return BoundingBox(
		Point3( -m_dMajorRadius-m_dMinorRadius, -m_dMajorRadius-m_dMinorRadius, -m_dMajorRadius-m_dMinorRadius ),
		Point3( m_dMajorRadius+m_dMinorRadius, m_dMajorRadius+m_dMinorRadius, m_dMajorRadius+m_dMinorRadius ) );
}

void TorusGeometry::UniformRandomPoint( Point3* /*point*/, Vector3* /*normal*/, Point2* /*coord*/, const Point3& /*prand*/ ) const
{
	// !@ Implement this!
}

Scalar TorusGeometry::GetArea( ) const
{
	return 4.0*m_dMinorRadius * (m_dMajorRadius-m_dMinorRadius) * PI*PI;
}

static const unsigned int MAJORRADIUS_ID = 100;
static const unsigned int MINORRADIUS_ID = 101;

IKeyframeParameter* TorusGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "major_radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), MAJORRADIUS_ID );
	} else if( name == "minor_radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), MINORRADIUS_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void TorusGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case MAJORRADIUS_ID:
		{
			m_dMajorRadius = *(Scalar*)val.getValue();
		}
		break;
	case MINORRADIUS_ID:
		{
			m_dMinorRadius = *(Scalar*)val.getValue();
		}
		break;

	}
}

void TorusGeometry::RegenerateData( )
{
	m_p1 = ((m_dMajorRadius - m_dMinorRadius) / 2);
	m_p0 = m_p1+m_dMinorRadius;
	m_sqrP1 = m_p1 * m_p1;
	m_sqrP0 = m_p0 * m_p0;
}
