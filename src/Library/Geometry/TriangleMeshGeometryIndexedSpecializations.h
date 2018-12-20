//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometryIndexedSpecializations.cpp - Specializations for the
//    acceleration structures
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2004
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// MYOBJ specialization required for the octree/bsptree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////
namespace RISE
{
	bool TriangleMeshGeometryIndexed::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
	{
		const PointerTriangle&	p = *elem;

		//
		// Trivial acception, any of the points are inside the box
		//
		for( int j=0; j<3; j++ ) {
			if( GeometricUtilities::IsPointInsideBox( *p.pVertices[j], bbox.ll, bbox.ur ) ) {
				// Then this polygon qualifies
				return true;
			}
		}
			
		//
		// Check if any of the triangle's edges intersect the box
		//

		// Edge 1
		BOX_HIT		h;
		Ray		ray;
		Scalar fEdgeLength;

		ray.origin = *p.pVertices[0];
		ray.dir = Vector3Ops::mkVector3( *p.pVertices[1], *p.pVertices[0] );
		fEdgeLength = Vector3Ops::NormalizeMag(ray.dir);

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}

		// Edge 2
		ray.origin = *p.pVertices[1];
		ray.dir = Vector3Ops::mkVector3( *p.pVertices[2], *p.pVertices[1] );
		fEdgeLength = Vector3Ops::NormalizeMag(ray.dir);

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}
		

		// Edge 3
		ray.origin = *p.pVertices[2];
		ray.dir = Vector3Ops::mkVector3( *p.pVertices[0], *p.pVertices[2] );
		fEdgeLength = Vector3Ops::NormalizeMag(ray.dir);

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}


		//
		// We know the none of the triangle's points lie in the box, we know
		// none of its edges intersect the box
		// That leaves just one more case, and that is the box cuts the triangle
		// completely internally
		//

		// Cheat and use two BBs
		BoundingBox bbTri( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY));
		for( int j=0; j<3; j++ ) {
			bbTri.Include( *p.pVertices[j] );
		}

		return bbTri.DoIntersect( bbox );	
	}

	char TriangleMeshGeometryIndexed::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
	{
		return GeometricUtilities::WhichSideOfPlane( plane, *elem );
	}

	void TriangleMeshGeometryIndexed::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		TRIANGLE_HIT	h;
		h.bHit = false;
		h.dRange = INFINITY;

		// We have to intersect against every triangle and find the closest intersection
		// We can omit triangles that aren't facing us (dot product is > 0) since they 
		// can't possibly hit

		const PointerTriangle&	thisTri = *elem;

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...
		const Vector3 vEdgeA = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
		const Vector3 vEdgeB = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );
		const Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );

		// If we are not to hit front faces and we are front facing, then beat it!
		if( !bHitFrontFaces  ) {
			if( Vector3Ops::Dot(vFaceNormal, ri.ray.dir) < 0 ) {
				return;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( Vector3Ops::Dot(vFaceNormal, ri.ray.dir) > 0 ) {
				return;
			}
		}

		{	
			RayTriangleIntersection( ri.ray, h, *thisTri.pVertices[0], vEdgeA, vEdgeB );

			if( h.bHit /* && h.dRange > 0.01*/ ) {
				ri.bHit = true;
				ri.range = h.dRange;

				const Scalar&	a = h.alpha;
				const Scalar&	b = h.beta;

				if( thisTri.pNormals[0] ) {
					ri.vNormal = *thisTri.pNormals[0]+
						(*thisTri.pNormals[1]-*thisTri.pNormals[0])*a+
						(*thisTri.pNormals[2]-*thisTri.pNormals[0])*b;
				} else {
					ri.vNormal = vFaceNormal;
				}
				ri.ptCoord = Point2Ops::mkPoint2(*thisTri.pCoords[0],
					Vector2Ops::mkVector2(*thisTri.pCoords[1],*thisTri.pCoords[0])*a+
					Vector2Ops::mkVector2(*thisTri.pCoords[2],*thisTri.pCoords[0])*b );
			}
		}
	}

	void TriangleMeshGeometryIndexed::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
	}

	bool TriangleMeshGeometryIndexed::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		const PointerTriangle&	thisTri = *elem;

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...

		const Vector3 vEdgeA = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
		const Vector3 vEdgeB = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );
		Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );

		// If we are not to hit front faces and we are front facing, then beat it!
		if( !bHitFrontFaces  ) {
			if( Vector3Ops::Dot(vFaceNormal, ray.dir) < 0 ) {
				return false;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( Vector3Ops::Dot(vFaceNormal, ray.dir) > 0 ) {
				return false;
			}
		}

		{
			TRIANGLE_HIT h;
			RayTriangleIntersection( ray, h, *thisTri.pVertices[0], vEdgeA, vEdgeB );

			if( h.bHit && (h.dRange > NEARZERO && h.dRange < dHowFar) ) {
				return true;
			}
		}

		return false;
	}

	void TriangleMeshGeometryIndexed::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
	{
		// Convert, assume the void* is the pointer to the begining
		unsigned int begining = VoidPtrToUInt( (void*)&(*ptr_polygons.begin()) );
		unsigned int ours = VoidPtrToUInt( (void*)(elem) );

		unsigned int idx = (ours-begining) / sizeof( PointerTriangle );
		buffer.setUInt( idx );
	}

	void TriangleMeshGeometryIndexed::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
	{
		// we assume the pCustom is a pointer to the begining of a MyTriangle* we just do pointer 
		// arithmetic.  NO!  Pointer arithmetic does NOT work on some platforms (the SGI Origin 3800 for example)
		// Instead resort to using array tricks.

		unsigned int idx = buffer.getUInt();
		const char* pch = (const char*)(void*)&(*ptr_polygons.begin());
		ret = (MYOBJ)&pch[idx*sizeof(PointerTriangle)];
	//	unsigned int ptr = (VoidPtrToUInt( pCustom )) + (idx*sizeof(PointerTriangle));
	//	ret = (MYOBJ)ptr;
	}
}


