//////////////////////////////////////////////////////////////////////
//
//  CylindricalUVGenerator.cpp - Implements the cylinder uv generator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 14, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CylindricalUVGenerator.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

CylindricalUVGenerator::CylindricalUVGenerator( const Scalar radius, const char axis, const Scalar size ) :
  m_OVRadius( 1.0 / radius ), m_chAxis( axis ), m_dAxisMin( -size/2 ), m_dAxisMax( size/2 )
{
}

CylindricalUVGenerator::~CylindricalUVGenerator( )
{
}

void CylindricalUVGenerator::GenerateUV( const Point3& ptIntersection, const Vector3&, Point2& uv ) const
{
	GeometricUtilities::CylinderTextureCoord( ptIntersection, m_chAxis, m_OVRadius, m_dAxisMin, m_dAxisMax, uv );
}

