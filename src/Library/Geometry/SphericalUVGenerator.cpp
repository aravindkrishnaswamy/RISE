//////////////////////////////////////////////////////////////////////
//
//  SphericalUVGenerator.cpp - Implements the spherical uv generator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SphericalUVGenerator.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

SphericalUVGenerator::SphericalUVGenerator( const Scalar radius ) : m_OVRadius( 1.0 / radius )
{
}

SphericalUVGenerator::~SphericalUVGenerator( )
{
}

void SphericalUVGenerator::GenerateUV( const Point3& ptIntersection, const Vector3&, Point2& uv ) const
{
	GeometricUtilities::SphereTextureCoord( 
			Vector3( 0.0, m_OVRadius, 0.0 ),
			Vector3( -m_OVRadius, 0.0, 0.0 ),
			ptIntersection, uv );
}

