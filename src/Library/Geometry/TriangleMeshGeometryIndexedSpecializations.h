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
	BoundingBox TriangleMeshGeometryIndexed::GetElementBoundingBox( const MYOBJ elem ) const
	{
		BoundingBox bbTri( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
		const PointerTriangle& p = *elem;
		for( int j=0; j<3; j++ ) {
			bbTri.Include( *p.pVertices[j] );
		}
		return bbTri;
	}

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
		{ Vector3 d = Vector3Ops::mkVector3( *p.pVertices[1], *p.pVertices[0] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}

		// Edge 2
		ray.origin = *p.pVertices[1];
		{ Vector3 d = Vector3Ops::mkVector3( *p.pVertices[2], *p.pVertices[1] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}
		

		// Edge 3
		ray.origin = *p.pVertices[2];
		{ Vector3 d = Vector3Ops::mkVector3( *p.pVertices[0], *p.pVertices[2] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

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
		return GetElementBoundingBox( elem ).DoIntersect( bbox );	
	}

	char TriangleMeshGeometryIndexed::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
	{
		return GeometricUtilities::WhichSideOfPlane( plane, *elem );
	}

	void TriangleMeshGeometryIndexed::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		// Mailboxing: skip triangles already tested for this ray
#ifdef RISE_ENABLE_MAILBOXING
		{
			MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
			const unsigned int triIdx = static_cast<unsigned int>( elem - &ptr_polygons[0] );
			if( mb.stamps[triIdx] == mb.rayId ) {
				return;
			}
			mb.stamps[triIdx] = mb.rayId;
		}
#endif

		TRIANGLE_HIT	h;
		h.bHit = false;
		h.dRange = RISE_INFINITY;

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
			if( Vector3Ops::Dot(vFaceNormal, ri.ray.Dir()) < 0 ) {
				return;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( Vector3Ops::Dot(vFaceNormal, ri.ray.Dir()) > 0 ) {
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

				// Populate surface derivatives for SMS consumers.
				// Free to do here because we already have the hit
				// triangle and barycentric (a, b).  See
				// docs/GEOMETRY_DERIVATIVES.md for the tangency convention
				// and TriangleMeshGeometryIndexed.cpp's helper for the
				// standalone (walk-the-mesh) variant.
				const Vector3 shadingNormal = Vector3Ops::Normalize( ri.vNormal );
				const Vector3 e1 = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
				const Vector3 e2 = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );
				// Project edges onto the shading-normal tangent plane.
				const Scalar e1_dot_n = Vector3Ops::Dot( e1, shadingNormal );
				const Scalar e2_dot_n = Vector3Ops::Dot( e2, shadingNormal );
				Vector3 dpdu(
					e1.x - shadingNormal.x * e1_dot_n,
					e1.y - shadingNormal.y * e1_dot_n,
					e1.z - shadingNormal.z * e1_dot_n );
				Vector3 dpdv(
					e2.x - shadingNormal.x * e2_dot_n,
					e2.y - shadingNormal.y * e2_dot_n,
					e2.z - shadingNormal.z * e2_dot_n );

				Vector3 dndu( 0, 0, 0 );
				Vector3 dndv( 0, 0, 0 );
				if( thisTri.pNormals[0] && thisTri.pNormals[1] && thisTri.pNormals[2] ) {
					const Vector3 dNraw_du(
						thisTri.pNormals[1]->x - thisTri.pNormals[0]->x,
						thisTri.pNormals[1]->y - thisTri.pNormals[0]->y,
						thisTri.pNormals[1]->z - thisTri.pNormals[0]->z );
					const Vector3 dNraw_dv(
						thisTri.pNormals[2]->x - thisTri.pNormals[0]->x,
						thisTri.pNormals[2]->y - thisTri.pNormals[0]->y,
						thisTri.pNormals[2]->z - thisTri.pNormals[0]->z );
					const Scalar nrawLen = Vector3Ops::Magnitude( ri.vNormal );
					const Scalar invLen = (nrawLen > NEARZERO) ? 1.0 / nrawLen : 0.0;
					const Scalar du_dot_n = Vector3Ops::Dot( shadingNormal, dNraw_du );
					const Scalar dv_dot_n = Vector3Ops::Dot( shadingNormal, dNraw_dv );
					dndu = Vector3(
						(dNraw_du.x - shadingNormal.x * du_dot_n) * invLen,
						(dNraw_du.y - shadingNormal.y * du_dot_n) * invLen,
						(dNraw_du.z - shadingNormal.z * du_dot_n) * invLen );
					dndv = Vector3(
						(dNraw_dv.x - shadingNormal.x * dv_dot_n) * invLen,
						(dNraw_dv.y - shadingNormal.y * dv_dot_n) * invLen,
						(dNraw_dv.z - shadingNormal.z * dv_dot_n) * invLen );
				}
				// Enforce right-handedness against shading normal.
				const Vector3 cross = Vector3Ops::Cross( dpdu, dpdv );
				if( Vector3Ops::Dot( cross, shadingNormal ) < 0.0 ) {
					dpdv = Vector3( -dpdv.x, -dpdv.y, -dpdv.z );
					dndv = Vector3( -dndv.x, -dndv.y, -dndv.z );
				}
				ri.derivatives.dpdu = dpdu;
				ri.derivatives.dpdv = dpdv;
				ri.derivatives.dndu = dndu;
				ri.derivatives.dndv = dndv;
				ri.derivatives.valid = true;
			}
		}
	}

	void TriangleMeshGeometryIndexed::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
	}

	bool TriangleMeshGeometryIndexed::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		// Mailboxing: skip triangles already tested for this ray
#ifdef RISE_ENABLE_MAILBOXING
		{
			MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
			const unsigned int triIdx = static_cast<unsigned int>( elem - &ptr_polygons[0] );
			if( mb.stamps[triIdx] == mb.rayId ) {
				return false;
			}
			mb.stamps[triIdx] = mb.rayId;
		}
#endif

		const PointerTriangle&	thisTri = *elem;

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...

		const Vector3 vEdgeA = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
		const Vector3 vEdgeB = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );
		Vector3 vFaceNormal = Vector3Ops::Cross( vEdgeA, vEdgeB );

		// If we are not to hit front faces and we are front facing, then beat it!
		if( !bHitFrontFaces  ) {
			if( Vector3Ops::Dot(vFaceNormal, ray.Dir()) < 0 ) {
				return false;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( Vector3Ops::Dot(vFaceNormal, ray.Dir()) > 0 ) {
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

