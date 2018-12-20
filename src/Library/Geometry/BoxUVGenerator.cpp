//////////////////////////////////////////////////////////////////////
//
//  BoxUVGenerator.cpp - Implements the box uv generator
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
#include "BoxUVGenerator.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

BoxUVGenerator::BoxUVGenerator( const Scalar width, const Scalar height, const Scalar depth ) : 
  dOVWidth( 1.0/width ), dOVHeight( 1.0/height ), dOVDepth( 1.0/depth ),
  dWidthOV2( width/2 ), dHeightOV2( height/2 ), dDepthOV2( depth/2 )
{
}

BoxUVGenerator::~BoxUVGenerator( )
{
}

void BoxUVGenerator::GenerateUV( const Point3& ptIntersection, const Vector3& vNormal, Point2& uv ) const
{
	// Side depends on where the point of intersection is
	// assume AA box
	Vector3 vAbsNormal = Vector3( fabs(vNormal.x), fabs(vNormal.y), fabs(vNormal.z) );
	unsigned char side = 0;

	if( vAbsNormal.x > vAbsNormal.y ) {
		if( vAbsNormal.x > vAbsNormal.z ) {
			// X axis,
			if( vNormal.x > 0 ) {
				side = 1;
			} else {
				side = 0;
			}
		} else {
			// Z axis
			if( vNormal.z > 0 ) {
				side = 5;
			} else {
				side = 4;
			}
		}
	} else {
		if( vAbsNormal.y > vAbsNormal.z ) {
			// Y axis
			if( vNormal.y > 0 ) {
				side = 3;
			} else {
				side = 2;
			}
		} else {
			// Z axis
			if( vNormal.z > 0 ) {
				side = 5;
			} else {
				side = 4;
			}
		}
	}

	switch( side )
	{
	case 0:
		// X coords are in +Z, Y coords are in +Y
		uv = Point2( (ptIntersection.z+dDepthOV2)*dOVDepth, 1.0 - (ptIntersection.y+dHeightOV2)*dOVHeight );
		break;
	case 1:
		// X coords are in -Z, Y coords are in +Y
		uv = Point2( 1.0 - (ptIntersection.z+dDepthOV2)*dOVDepth, 1.0 - (ptIntersection.y+dHeightOV2)*dOVHeight );
		break;
	case 2:
		// X coords are in +X, Y coords are in -Z
		uv = Point2( (ptIntersection.x+dWidthOV2)*dOVWidth, 1.0 - (ptIntersection.z+dDepthOV2)*dOVDepth );
		break;
	case 3:
		// X coords are in -X, Y coords are in Z
		uv = Point2( (ptIntersection.x+dWidthOV2)*dOVWidth, (ptIntersection.z+dDepthOV2)*dOVDepth );
		break;
	case 4:
		// X coords are in +Y, Y coords are in -X
		uv = Point2( 1.0 - (ptIntersection.x+dWidthOV2)*dOVWidth, 1.0 - (ptIntersection.y+dHeightOV2)*dOVHeight );
		break;
	case 5:
		// X coords are in -Y, Y coords are in -X
		uv = Point2( (ptIntersection.x+dWidthOV2)*dOVWidth, 1.0 - (ptIntersection.y+dHeightOV2)*dOVHeight );
		break;
	};
}

