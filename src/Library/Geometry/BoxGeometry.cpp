//////////////////////////////////////////////////////////////////////
//
//  BoxGeometry.cpp - Implementation of the box class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BoxGeometry.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

BoxGeometry::BoxGeometry( Scalar dWidth_, Scalar dHeight_, Scalar dDepth_ ) : 
  dWidth( dWidth_ ), dHeight( dHeight_ ), dDepth( dDepth_ ), 
  dOVWidth( 0 ), dOVHeight( 0 ), dOVDepth( 0 ),
  dWidthOV2( dWidth_*0.5 ), dHeightOV2( dHeight_*0.5 ), dDepthOV2( dDepth_*0.5 )
{
	RegenerateData();
}

BoxGeometry::~BoxGeometry( )
{
}

bool BoxGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 1 ) {
		return false;
	}

	const Scalar hw = dWidthOV2;
	const Scalar hh = dHeightOV2;
	const Scalar hd = dDepthOV2;
	const Scalar w  = dWidth;
	const Scalar h  = dHeight;
	const Scalar d  = dDepth;

	// Per-face (origin, edgeU, edgeV, normal) matching the UV convention in IntersectRay.
	struct FaceDef {
		Point3  origin;
		Vector3 edgeU;
		Vector3 edgeV;
		Vector3 normal;
	};
	const FaceDef faces[6] = {
		// 0: -X face; u = (z+d/2)/d, v = 1-(y+h/2)/h
		{ Point3( -hw,  hh, -hd ), Vector3(  0.0,  0.0,  d   ), Vector3(  0.0, -h,    0.0 ), Vector3( -1.0,  0.0,  0.0 ) },
		// 1: +X face; u = 1-(z+d/2)/d, v = 1-(y+h/2)/h
		{ Point3(  hw,  hh,  hd ), Vector3(  0.0,  0.0, -d   ), Vector3(  0.0, -h,    0.0 ), Vector3(  1.0,  0.0,  0.0 ) },
		// 2: -Y face; u = (x+w/2)/w, v = 1-(z+d/2)/d
		{ Point3( -hw, -hh,  hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0,  0.0, -d   ), Vector3(  0.0, -1.0,  0.0 ) },
		// 3: +Y face; u = (x+w/2)/w, v = (z+d/2)/d
		{ Point3( -hw,  hh, -hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0,  0.0,  d   ), Vector3(  0.0,  1.0,  0.0 ) },
		// 4: -Z face; u = 1-(x+w/2)/w, v = 1-(y+h/2)/h
		{ Point3(  hw,  hh, -hd ), Vector3( -w,    0.0,  0.0 ), Vector3(  0.0, -h,    0.0 ), Vector3(  0.0,  0.0, -1.0 ) },
		// 5: +Z face; u = (x+w/2)/w, v = 1-(y+h/2)/h
		{ Point3( -hw,  hh,  hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0, -h,    0.0 ), Vector3(  0.0,  0.0,  1.0 ) },
	};

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int rowStride = nU + 1;

	for( int f = 0; f < 6; f++ ) {
		const FaceDef& face = faces[f];
		const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );

		for( unsigned int j = 0; j <= nV; j++ ) {
			const Scalar v = Scalar(j) / Scalar(nV);
			for( unsigned int i = 0; i <= nU; i++ ) {
				const Scalar u = Scalar(i) / Scalar(nU);

				const Point3 pos(
					face.origin.x + u * face.edgeU.x + v * face.edgeV.x,
					face.origin.y + u * face.edgeU.y + v * face.edgeV.y,
					face.origin.z + u * face.edgeU.z + v * face.edgeV.z );

				vertices.push_back( pos );
				normals.push_back( face.normal );
				coords.push_back( Point2( u, v ) );
			}
		}

		for( unsigned int j = 0; j < nV; j++ ) {
			for( unsigned int i = 0; i < nU; i++ ) {
				const unsigned int a = baseIdx + j     * rowStride + i;
				const unsigned int b = baseIdx + j     * rowStride + (i + 1);
				const unsigned int c = baseIdx + (j+1) * rowStride + i;
				const unsigned int d_idx = baseIdx + (j+1) * rowStride + (i + 1);

				tris.push_back( MakeIndexedTriangleSameIdx( a, c, b ) );
				tris.push_back( MakeIndexedTriangleSameIdx( b, c, d_idx ) );
			}
		}
	}

	return true;
}

void BoxGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	Point3		ptLowerLeft = Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 );
	Point3		ptUpperRight = Point3( dWidthOV2, dHeightOV2, dDepthOV2 );

	// If the point is inside the box and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInBox = GeometricUtilities::IsPointInsideBox( ri.ray.origin, ptLowerLeft, ptUpperRight );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInBox ) {
		return;
	}

	// Accordingly, if we are outside the box but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInBox ) {
		return;
	}

	BOX_HIT	h;
	RayBoxIntersection( ri.ray, h, ptLowerLeft, ptUpperRight );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;
	
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );

		Point3&	it = ri.ptIntersection;

		// Compute normal and texture coords
		switch( h.sideA )
		{
		case 0:
			ri.vNormal = Vector3( -1.0, 0.0, 0.0 );
			// X coords are in +Z, Y coords are in +Y
			ri.ptCoord = Point2( (it.z+dDepthOV2)*dOVDepth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 1:
			ri.vNormal = Vector3( 1.0, 0.0, 0.0 );
			// X coords are in -Z, Y coords are in +Y
			ri.ptCoord = Point2( 1.0 - (it.z+dDepthOV2)*dOVDepth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 2:
			ri.vNormal = Vector3( 0.0, -1.0, 0.0 );
			// X coords are in +X, Y coords are in -Z
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.z+dDepthOV2)*dOVDepth );
			break;
		case 3:
			ri.vNormal = Vector3( 0.0, 1.0, 0.0 );
			// X coords are in -X, Y coords are in Z
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, (it.z+dDepthOV2)*dOVDepth );
			break;
		case 4:
			ri.vNormal = Vector3( 0.0, 0.0, -1.0 );
			// X coords are in +Y, Y coords are in -X
			ri.ptCoord = Point2( 1.0 - (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 5:
			ri.vNormal = Vector3( 0.0, 0.0, 1.0 );
			// X coords are in -Y, Y coords are in -X
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		};
		ri.vGeomNormal = ri.vNormal;	// box face is flat: shading == geometric

		if( bComputeExitInfo )
		{
			// Compute the normal at the point of exit
			switch( h.sideB )
			{
			case 0:
				ri.vNormal2 = Vector3( -1.0, 0.0, 0.0 );
				break;
			case 1:
				ri.vNormal2 = Vector3( 1.0, 0.0, 0.0 );
				break;
			case 2:
				ri.vNormal2 = Vector3( 0.0, -1.0, 0.0 );
				break;
			case 3:
				ri.vNormal2 = Vector3( 0.0, 1.0, 0.0 );
				break;
			case 4:
				ri.vNormal2 = Vector3( 0.0, 0.0, -1.0 );
				break;
			case 5:
				ri.vNormal2 = Vector3( 0.0, 0.0, 1.0 );
				break;
			};
			ri.vGeomNormal2 = ri.vNormal2;	// box face is flat: shading == geometric
		}
	}
}

bool BoxGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	Point3		ptLowerLeft = Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 );
	Point3		ptUpperRight = Point3( dWidthOV2, dHeightOV2, dDepthOV2 );

	// If the point is inside the box and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInBox = GeometricUtilities::IsPointInsideBox( ray.origin, ptLowerLeft, ptUpperRight );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInBox ) {
		return false;
	}

	// Accordingly, if we are outside the box but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInBox ) {
		return false;
	}

	BOX_HIT	h;
	RayBoxIntersection( ray, h, ptLowerLeft, ptUpperRight );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void BoxGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0.0, 0.0, 0.0 );
	radius = Vector3Ops::Magnitude( Vector3( dWidth/2, dHeight/2, dDepth/2 ) );
}

BoundingBox BoxGeometry::GenerateBoundingBox() const
{
	return BoundingBox( Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 ),
						Point3( dWidthOV2, dHeightOV2, dDepthOV2 ) );
	
}

void BoxGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	int idx = int(floor(prand.z * 6.0));
	if( idx < 0 ) idx = 0;
	if( idx > 5 ) idx = 5;

	Point3 pt;
	switch( idx ) {
		default:
		case 0:
			// -X face
			pt = Point3( -dWidthOV2, prand.x*dHeight-dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 1:
			// +X face
			pt = Point3( dWidthOV2, prand.x*dHeight-dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 2:
			// -Y face
			pt = Point3( prand.x*dWidth-dWidthOV2, -dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 3:
			// +Y face
			pt = Point3( prand.x*dWidth-dWidthOV2, dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 4:
			// -Z face
			pt = Point3( prand.x*dWidth-dWidthOV2, prand.y*dHeight-dHeightOV2, -dDepthOV2 );
			break;
		case 5:
			// +Z face
			pt = Point3( prand.x*dWidth-dWidthOV2, prand.y*dHeight-dHeightOV2, dDepthOV2 );
			break;
	}

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		switch( idx ) {
			default:
			case 0:
				*normal = Vector3( -1.0, 0.0, 0.0 );
				break;
			case 1:
				*normal = Vector3( 1.0, 0.0, 0.0 );
				break;
			case 2:
				*normal = Vector3( 0.0, -1.0, 0.0 );
				break;
			case 3:
				*normal = Vector3( 0.0, 1.0, 0.0 );
				break;
			case 4:
				*normal = Vector3( 0.0, 0.0, -1.0 );
				break;
			case 5:
				*normal = Vector3( 0.0, 0.0, 1.0 );
				break;
		}
	}

	if( coord ) {
		switch( idx ) {
			default:
			case 0:
				// -X face: same as IntersectRay case 0
				*coord = Point2( (pt.z+dDepthOV2)*dOVDepth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 1:
				// +X face: same as IntersectRay case 1
				*coord = Point2( 1.0 - (pt.z+dDepthOV2)*dOVDepth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 2:
				// -Y face: same as IntersectRay case 2
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.z+dDepthOV2)*dOVDepth );
				break;
			case 3:
				// +Y face: same as IntersectRay case 3
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, (pt.z+dDepthOV2)*dOVDepth );
				break;
			case 4:
				// -Z face: same as IntersectRay case 4
				*coord = Point2( 1.0 - (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 5:
				// +Z face: same as IntersectRay case 5
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
		}
	}
}

SurfaceDerivatives BoxGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;
	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );
	sd.valid = true;

	// Determine which face from the normal direction
	const Scalar ax = fabs( objSpaceNormal.x );
	const Scalar ay = fabs( objSpaceNormal.y );
	const Scalar az = fabs( objSpaceNormal.z );

	if( ax > ay && ax > az )
	{
		// X face
		if( objSpaceNormal.x > 0 ) {
			// +X face
			sd.dpdu = Vector3( 0, 0, -1 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.z + dDepthOV2) * dOVDepth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		} else {
			// -X face
			sd.dpdu = Vector3( 0, 0, 1 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( (objSpacePoint.z + dDepthOV2) * dOVDepth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		}
	}
	else if( ay > az )
	{
		// Y face — swap (u, v) so (dpdu × dpdv, n) is right-handed per
		// docs/GEOMETRY_DERIVATIVES.md.
		if( objSpaceNormal.y > 0 ) {
			// +Y face
			sd.dpdu = Vector3( 0, 0, 1 );
			sd.dpdv = Vector3( 1, 0, 0 );
			sd.uv = Point2( (objSpacePoint.z + dDepthOV2) * dOVDepth, (objSpacePoint.x + dWidthOV2) * dOVWidth );
		} else {
			// -Y face
			sd.dpdu = Vector3( 0, 0, -1 );
			sd.dpdv = Vector3( 1, 0, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.z + dDepthOV2) * dOVDepth, (objSpacePoint.x + dWidthOV2) * dOVWidth );
		}
	}
	else
	{
		// Z face
		if( objSpaceNormal.z > 0 ) {
			// +Z face
			sd.dpdu = Vector3( 1, 0, 0 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( (objSpacePoint.x + dWidthOV2) * dOVWidth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		} else {
			// -Z face
			sd.dpdu = Vector3( -1, 0, 0 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.x + dWidthOV2) * dOVWidth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		}
	}

	return sd;
}

Scalar BoxGeometry::GetArea( ) const
{
	const Scalar faceA = dWidth*dHeight;
	const Scalar faceB = dWidth*dDepth;
	const Scalar faceC = dHeight*dDepth;

	return faceA*2 + faceB*2 + faceC*2;
}

static const unsigned int WIDTH_ID = 100;
static const unsigned int HEIGHT_ID = 101;
static const unsigned int DEPTH_ID = 102;

IKeyframeParameter* BoxGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "width" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), WIDTH_ID );
	} else if( name == "height" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), HEIGHT_ID );
	} else if( name == "depth" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), DEPTH_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void BoxGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case WIDTH_ID:
		{
			dWidth = *(Scalar*)val.getValue();
			dWidthOV2 = dWidth * 0.5;
		}
		break;
	case HEIGHT_ID:
		{
			dHeight = *(Scalar*)val.getValue();
			dHeightOV2 = dHeight * 0.5;
		}
		break;
	case DEPTH_ID:
		{
			dDepth = *(Scalar*)val.getValue();
			dDepthOV2 = dDepth * 0.5;
		}
		break;
	}
}

void BoxGeometry::RegenerateData( )
{
	if( dWidth > 0 ) {
		dOVWidth = 1.0 / dWidth;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Width is 0", __FILE__, __LINE__ );
	}

	if( dHeight > 0 ) {
		dOVHeight = 1.0 / dHeight;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Height is 0", __FILE__, __LINE__ );
	}

	if( dDepth > 0 ) {
		dOVDepth = 1.0 / dDepth;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Depth is 0", __FILE__, __LINE__ );
	}
}


