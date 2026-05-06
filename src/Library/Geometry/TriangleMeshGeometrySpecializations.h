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
	BoundingBox TriangleMeshGeometry::GetElementBoundingBox( const MYOBJ elem ) const
	{
		BoundingBox bbTri( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
		const Triangle& p = *elem;
		for( int j=0; j<3; j++ ) {
			bbTri.Include( p.vertices[j] );
		}
		return bbTri;
	}

	bool TriangleMeshGeometry::GetFloatTriangleVertices(
		const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const
	{
		// Tier 1 §4: extract triangle vertices in float for the BVH leaf
		// float-Möller-Trumbore filter.  Same pattern as the indexed
		// mesh override.
		const Triangle& tri = *elem;
		v0[0] = (float)tri.vertices[0].x; v0[1] = (float)tri.vertices[0].y; v0[2] = (float)tri.vertices[0].z;
		v1[0] = (float)tri.vertices[1].x; v1[1] = (float)tri.vertices[1].y; v1[2] = (float)tri.vertices[1].z;
		v2[0] = (float)tri.vertices[2].x; v2[1] = (float)tri.vertices[2].y; v2[2] = (float)tri.vertices[2].z;
		return true;
	}

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
		{ Vector3 d = Vector3Ops::mkVector3( p.vertices[1], p.vertices[0] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}

		// Edge 2
		ray.origin = p.vertices[1];
		{ Vector3 d = Vector3Ops::mkVector3( p.vertices[2], p.vertices[1] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

		RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}
		

		// Edge 3
		ray.origin = p.vertices[2];
		{ Vector3 d = Vector3Ops::mkVector3( p.vertices[0], p.vertices[2] ); fEdgeLength = Vector3Ops::NormalizeMag(d); ray.SetDir(d); }

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

	char TriangleMeshGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
	{
		return GeometricUtilities::WhichSideOfPlane( plane, *elem );
	}

	void TriangleMeshGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		TRIANGLE_HIT	h;
		h.bHit = false;
		h.dRange = RISE_INFINITY;

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
			RayTriangleIntersection( ri.ray, h, thisTri.vertices[0], vEdgeA, vEdgeB );

			// Cleanup §1: native closest-hit check.  See indexed-mesh
			// equivalent in TriangleMeshGeometryIndexedSpecializations.h
			// for the rationale.
			if( h.bHit && h.dRange < ri.range ) {
				ri.bHit = true;
				ri.range = h.dRange;

				const Scalar&	a = h.alpha;
				const Scalar&	b = h.beta;

				ri.vNormal = thisTri.normals[0]+
					(thisTri.normals[1]-thisTri.normals[0])*a+
					(thisTri.normals[2]-thisTri.normals[0])*b;
				// Geometric (face) normal — see indexed specialization.
				{
					Vector3 fn = Vector3Ops::Normalize( vFaceNormal );
					if( Vector3Ops::Dot( fn, ri.vNormal ) < 0 ) {
						fn = Vector3( -fn.x, -fn.y, -fn.z );
					}
					ri.vGeomNormal = fn;
				}
				ri.ptCoord = Point2Ops::mkPoint2( thisTri.coords[0],
					Vector2Ops::mkVector2(thisTri.coords[1],thisTri.coords[0])*a+
					Vector2Ops::mkVector2(thisTri.coords[2],thisTri.coords[0])*b );

				// Landing 2: populate surface derivatives + texture
				// footprint so this path participates in mip-LOD
				// sampling on parity with the indexed mesh path.
				// Ported from TriangleMeshGeometryIndexedSpecializations.h;
				// the math is identical, only the data access differs
				// (non-indexed Triangle stores .vertices / .normals /
				// .coords by value; indexed PointerTriangle stores
				// .pVertices / .pNormals / .pCoords by pointer).
				// See the indexed-mesh implementation for the full
				// derivation and design notes; the duplicated comments
				// kept short here to stay readable.
				//
				// Per-vertex color and tangent interpolation are NOT
				// ported — those rely on a per-vertex side-table
				// addressed by pointer arithmetic into a shared
				// vertex pool, which non-indexed meshes don't have.
				// Hand-built / RAW meshes that need vertex colors or
				// glTF-style tangents should be loaded as indexed.
				const Vector3 shadingNormal = Vector3Ops::Normalize( ri.vNormal );
				const Vector3 e1 = Vector3Ops::mkVector3( thisTri.vertices[1], thisTri.vertices[0] );
				const Vector3 e2 = Vector3Ops::mkVector3( thisTri.vertices[2], thisTri.vertices[0] );

				// UV deltas.  Non-indexed Triangle always carries
				// coords by value, so the "no UV data" fall-back from
				// the indexed path (null pCoords pointer) doesn't
				// apply — only the singular-Jacobian fall-back does.
				const Scalar duA = thisTri.coords[1].x - thisTri.coords[0].x;
				const Scalar duB = thisTri.coords[2].x - thisTri.coords[0].x;
				const Scalar dvA = thisTri.coords[1].y - thisTri.coords[0].y;
				const Scalar dvB = thisTri.coords[2].y - thisTri.coords[0].y;
				const Scalar uvDet = duA * dvB - duB * dvA;
				const bool useUVJacobian = ( fabs( uvDet ) > NEARZERO );

				// Compute ∂P/∂u, ∂P/∂v.
				Vector3 dpdu, dpdv;
				if( useUVJacobian ) {
					const Scalar invDet = 1.0 / uvDet;
					dpdu = Vector3(
						( e1.x * dvB - e2.x * dvA ) * invDet,
						( e1.y * dvB - e2.y * dvA ) * invDet,
						( e1.z * dvB - e2.z * dvA ) * invDet );
					dpdv = Vector3(
						( e2.x * duA - e1.x * duB ) * invDet,
						( e2.y * duA - e1.y * duB ) * invDet,
						( e2.z * duA - e1.z * duB ) * invDet );
				} else {
					// Degenerate UV mapping (collinear or zero-area UV
					// triangle); fall back to barycentric edge frame.
					dpdu = e1;
					dpdv = e2;
				}

				// Project onto shading-normal tangent plane (gate kept
				// in lock-step with the indexed-mesh implementation).
#define MESH_PROJECT_DERIVATIVES_TO_TANGENT_PLANE 1
#if MESH_PROJECT_DERIVATIVES_TO_TANGENT_PLANE
				const Scalar dpdu_dot_n = Vector3Ops::Dot( dpdu, shadingNormal );
				const Scalar dpdv_dot_n = Vector3Ops::Dot( dpdv, shadingNormal );
				dpdu = Vector3(
					dpdu.x - shadingNormal.x * dpdu_dot_n,
					dpdu.y - shadingNormal.y * dpdu_dot_n,
					dpdu.z - shadingNormal.z * dpdu_dot_n );
				dpdv = Vector3(
					dpdv.x - shadingNormal.x * dpdv_dot_n,
					dpdv.y - shadingNormal.y * dpdv_dot_n,
					dpdv.z - shadingNormal.z * dpdv_dot_n );
#endif

				// Compute ∂N/∂u, ∂N/∂v from per-vertex normals.
				const Vector3 dNraw_dA(
					thisTri.normals[1].x - thisTri.normals[0].x,
					thisTri.normals[1].y - thisTri.normals[0].y,
					thisTri.normals[1].z - thisTri.normals[0].z );
				const Vector3 dNraw_dB(
					thisTri.normals[2].x - thisTri.normals[0].x,
					thisTri.normals[2].y - thisTri.normals[0].y,
					thisTri.normals[2].z - thisTri.normals[0].z );

				Vector3 dNraw_du, dNraw_dv;
				if( useUVJacobian ) {
					const Scalar invDet = 1.0 / uvDet;
					dNraw_du = Vector3(
						( dNraw_dA.x * dvB - dNraw_dB.x * dvA ) * invDet,
						( dNraw_dA.y * dvB - dNraw_dB.y * dvA ) * invDet,
						( dNraw_dA.z * dvB - dNraw_dB.z * dvA ) * invDet );
					dNraw_dv = Vector3(
						( dNraw_dB.x * duA - dNraw_dA.x * duB ) * invDet,
						( dNraw_dB.y * duA - dNraw_dA.y * duB ) * invDet,
						( dNraw_dB.z * duA - dNraw_dA.z * duB ) * invDet );
				} else {
					dNraw_du = dNraw_dA;
					dNraw_dv = dNraw_dB;
				}

				const Scalar nrawLen = Vector3Ops::Magnitude( ri.vNormal );
				const Scalar invLen = (nrawLen > NEARZERO) ? 1.0 / nrawLen : 0.0;
				const Scalar du_dot_n = Vector3Ops::Dot( shadingNormal, dNraw_du );
				const Scalar dv_dot_n = Vector3Ops::Dot( shadingNormal, dNraw_dv );
				Vector3 dndu(
					(dNraw_du.x - shadingNormal.x * du_dot_n) * invLen,
					(dNraw_du.y - shadingNormal.y * du_dot_n) * invLen,
					(dNraw_du.z - shadingNormal.z * du_dot_n) * invLen );
				Vector3 dndv(
					(dNraw_dv.x - shadingNormal.x * dv_dot_n) * invLen,
					(dNraw_dv.y - shadingNormal.y * dv_dot_n) * invLen,
					(dNraw_dv.z - shadingNormal.z * dv_dot_n) * invLen );

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

				// Project ray differentials onto the surface UV plane
				// and store the texture-space footprint.  No-op when
				// ray.hasDifferentials = false.
				ComputeTextureFootprint( ri, ri.ray );
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
