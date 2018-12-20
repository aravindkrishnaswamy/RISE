//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometrySpecializations.cpp - Specializations for the
//    acceleration structures
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2004
//  Tabs: 4
//  Comments: Moved here from TriangleMeshGeometry.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// MYOBJ specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////
namespace RISE
{
	bool TriangleMeshGeometry::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
	{
		const Triangle&	p = *elem;

		//
		// Trivial acception, any of the points are inside the box
		//
		for( int j=0; j<3; j++ ) {
			if( GeometricUtilities::IsPointInsideBox( p.vertices[j], bbox.ll, bbox.ur ) ) {
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

		ray.origin = p.vertices[0];
		ray.dir = Vector3Ops::mkVector3( p.vertices[1], p.vertices[0] );
		fEdgeLength = Vector3Ops::NormalizeMag(ray.dir);

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}

		// Edge 2
		ray.origin = p.vertices[1];
		ray.dir = Vector3Ops::mkVector3( p.vertices[2], p.vertices[1] );
		fEdgeLength = Vector3Ops::NormalizeMag(ray.dir);

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}
		

		// Edge 3
		ray.origin = p.vertices[2];
		ray.dir = Vector3Ops::mkVector3( p.vertices[0], p.vertices[2] );
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
			bbTri.Include( p.vertices[j] );
		}

		return bbTri.DoIntersect( bbox );
	}

	char TriangleMeshGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
	{
		return GeometricUtilities::WhichSideOfPlane( plane, *elem );
	}

	void TriangleMeshGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		TRIANGLE_HIT	h;
		h.bHit = false;
		h.dRange = INFINITY;

		// We have to intersect against every triangle and find the closest intersection
		// We can omit triangles that aren't facing us (dot product is > 0) since they 
		// can't possibly hit

		const Triangle&	thisTri = *elem;

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...

		// If we are not to hit front faces and we are front facing, then beat it!
		const Vector3 vEdgeA = Vector3Ops::mkVector3( thisTri.vertices[1], thisTri.vertices[0] );
		const Vector3 vEdgeB = Vector3Ops::mkVector3( thisTri.vertices[2], thisTri.vertices[0] );
		const Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );

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
			RayTriangleIntersection( ri.ray, h, thisTri.vertices[0], vEdgeA, vEdgeB );

			// I don't think the range check is necessary, but I could be wrong...
			if( h.bHit /* && h.dRange > 0.01*/ ) {
				ri.bHit = true;
				ri.range = h.dRange;

				const Scalar&	a = h.alpha;
				const Scalar&	b = h.beta;

				ri.vNormal = thisTri.normals[0]+
					(thisTri.normals[1]-thisTri.normals[0])*a+
					(thisTri.normals[2]-thisTri.normals[0])*b;
				ri.ptCoord = Point2Ops::mkPoint2( thisTri.coords[0],
					Vector2Ops::mkVector2(thisTri.coords[1],thisTri.coords[0])*a+
					Vector2Ops::mkVector2(thisTri.coords[2],thisTri.coords[0])*b );
			}
		}
	}

	void TriangleMeshGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
	}

	bool TriangleMeshGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		const Triangle&	thisTri = *elem;

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...

		const Vector3 vEdgeA = Vector3Ops::mkVector3( thisTri.vertices[1], thisTri.vertices[0] );
		const Vector3 vEdgeB = Vector3Ops::mkVector3( thisTri.vertices[2], thisTri.vertices[0] );
		const Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );

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
			RayTriangleIntersection( ray, h, thisTri.vertices[0], vEdgeA, vEdgeB );

			if( h.bHit && (h.dRange > NEARZERO && h.dRange < dHowFar) ) {
				return true;
			}
		}

		return false;
	}

	void TriangleMeshGeometry::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
	{
		// Convert, assume the void* is the pointer to the begining
		unsigned int begining = VoidPtrToUInt( (void*)&(*polygons.begin()) );
		unsigned int ours = VoidPtrToUInt( (const void*)(elem) );

		unsigned int idx = (ours-begining) / sizeof( Triangle );
		buffer.setUInt( idx );
	}

	void TriangleMeshGeometry::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
	{
		// we assume the pCustom is a pointer to the begining of a MYOBJangle* we just do pointer 
		// arithmetic  NO!  Pointer arithmetic does NOT work on some platforms (the SGI Origin 3800 for example)
		// Instead resort to using array tricks.


		unsigned int idx = buffer.getUInt();
		const char* pch = (const char*)(void*)&(*polygons.begin());
		ret = (MYOBJ)&pch[idx*sizeof(Triangle)];
	//	unsigned int ptr = (VoidPtrToUInt( pCustom )) + (idx*sizeof(Triangle));
	//	ret = (MYOBJ)ptr;
	}
}

