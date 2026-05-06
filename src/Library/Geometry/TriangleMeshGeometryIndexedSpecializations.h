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

	bool TriangleMeshGeometryIndexed::GetFloatTriangleVertices(
		const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const
	{
		// Phase 2: extract the three triangle vertices in float for the
		// BVH leaf float-Möller-Trumbore filter.  The PointerTriangle's
		// vertex pointers point into the mesh's pPoints array; this is
		// a straight double→float cast per component.  Conservativeness
		// is handled inside the BVH float Möller-Trumbore via an
		// epsilon-padded barycentric check — the filter never wrong-
		// rejects a hit the double-precision certifier would catch.
		const PointerTriangle& tri = *elem;
		v0[0] = (float)tri.pVertices[0]->x; v0[1] = (float)tri.pVertices[0]->y; v0[2] = (float)tri.pVertices[0]->z;
		v1[0] = (float)tri.pVertices[1]->x; v1[1] = (float)tri.pVertices[1]->y; v1[2] = (float)tri.pVertices[1]->z;
		v2[0] = (float)tri.pVertices[2]->x; v2[1] = (float)tri.pVertices[2]->y; v2[2] = (float)tri.pVertices[2]->z;
		return true;
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

			// Cleanup §1: native closest-hit check.  Previously this
			// unconditionally overwrote ri.range on hit; BSP traversal
			// worked around it by using a local myRI per element +
			// external compare ([BSPTreeSAHNode.h:449]), and the BVH
			// integration used the same pattern in its leaf iteration.
			// With this guard in place, both BSP and BVH leaves can call
			// RayElementIntersection directly with `ri` and rely on it
			// to preserve the closest-hit invariant.  Recovers the
			// per-element-copy overhead Phase 1 §6.2 documented.
			if( h.bHit && h.dRange < ri.range ) {
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
				// Geometric (face) normal — independent of Phong vertex-
				// normal interpolation.  Used by consumers that need the
				// actual triangle plane orientation (e.g. SMS chain
				// physics validation), not the smoothed shading normal.
				// Normalized; oriented consistently with the shading
				// normal so the side-tests downstream don't flip sign.
				{
					Vector3 fn = Vector3Ops::Normalize( vFaceNormal );
					if( Vector3Ops::Dot( fn, ri.vNormal ) < 0 ) {
						fn = Vector3( -fn.x, -fn.y, -fn.z );
					}
					ri.vGeomNormal = fn;
				}
				ri.ptCoord = Point2Ops::mkPoint2(*thisTri.pCoords[0],
					Vector2Ops::mkVector2(*thisTri.pCoords[1],*thisTri.pCoords[0])*a+
					Vector2Ops::mkVector2(*thisTri.pCoords[2],*thisTri.pCoords[0])*b );

				// Per-vertex color interpolation.  pColors is indexed by
				// vertex *position* index (the convention every common
				// exporter produces — see the comment on
				// ITriangleMeshGeometryIndexed2 for why we don't carry a
				// fourth iColors[3]).  Recover the position index from
				// the PointerTriangle by pointer arithmetic into pPoints.
				if( !pColors.empty() && !pPoints.empty() ) {
					const Vertex* pBase = &pPoints[0];
					const size_t i0 = (size_t)( thisTri.pVertices[0] - pBase );
					const size_t i1 = (size_t)( thisTri.pVertices[1] - pBase );
					const size_t i2 = (size_t)( thisTri.pVertices[2] - pBase );
					if( i0 < pColors.size() && i1 < pColors.size() && i2 < pColors.size() ) {
						const VertexColor& c0 = pColors[i0];
						ri.vColor = c0 + (pColors[i1] - c0) * a + (pColors[i2] - c0) * b;
						ri.bHasVertexColor = true;
					} else {
						ri.bHasVertexColor = false;
					}
				} else {
					ri.bHasVertexColor = false;
				}

				// Per-vertex tangent interpolation (v3 ITriangleMeshGeometryIndexed3
				// storage).  Same indexing convention as colors: tangent index
				// follows position index.  glTF TANGENT.w is the bitangent sign
				// (±1) per spec; we store it on RayIntersectionGeometric so the
				// NormalMap modifier can rebuild the bitangent in the correct
				// chirality (mirrored UVs flip the sign).  Since w is binary
				// per-vertex, pick the sign from vertex 0 of the triangle —
				// glTF guarantees w is constant within a connected UV chart, so
				// all three vertices of a triangle share the same w.
				//
				// Tangent vector is interpolated linearly without renormalising;
				// caller (NormalMap) normalises after world-space transform.
				if( !pTangents.empty() && !pPoints.empty() ) {
					const Vertex* pBase = &pPoints[0];
					const size_t i0 = (size_t)( thisTri.pVertices[0] - pBase );
					const size_t i1 = (size_t)( thisTri.pVertices[1] - pBase );
					const size_t i2 = (size_t)( thisTri.pVertices[2] - pBase );
					if( i0 < pTangents.size() && i1 < pTangents.size() && i2 < pTangents.size() ) {
						const Vector3& t0 = pTangents[i0].dir;
						ri.vTangent = t0
							+ (pTangents[i1].dir - t0) * a
							+ (pTangents[i2].dir - t0) * b;
						ri.bitangentSign = pTangents[i0].bitangentSign;
						ri.bHasTangent = true;
					} else {
						ri.bHasTangent = false;
					}
				} else {
					ri.bHasTangent = false;
				}

				// Populate surface derivatives for SMS consumers.
				//
				// Strategy: use the per-vertex (u, v) texture coordinates
				// stored in thisTri.pCoords to invert the triangle's UV
				// Jacobian, yielding derivatives in the STORED
				// PARAMETERIZATION — not in barycentric-edge space.
				//
				// For meshes produced by TessellateToMesh (sphere, torus,
				// ellipsoid, cylinder, …), pCoords ARE the analytical
				// shape's own parameterisation (sphere: (φ, θ)), so the
				// derivatives come out aligned with and scaled like the
				// analytical shape's dpdu / dpdv — to the fidelity of
				// the piecewise-linear mesh approximation.  This
				// CONTINUITY across triangles (shared vertex UVs at
				// shared edges) is what a smooth-surface Newton iteration
				// needs; the old edge-frame variant produced derivatives
				// in a per-triangle barycentric frame that jumped
				// discontinuously across every edge.
				//
				// Derivation: write
				//   u(a, b) = u0 + a·Δu1 + b·Δu2
				//   v(a, b) = v0 + a·Δv1 + b·Δv2
				// where Δu1 = u1 − u0 etc.  Inverting the 2×2:
				//   a = ( Δv2·(u−u0) − Δu2·(v−v0)) / det
				//   b = (−Δv1·(u−u0) + Δu1·(v−v0)) / det
				//   det = Δu1·Δv2 − Δu2·Δv1
				// Chain rule, with ∂P/∂a = P1−P0, ∂P/∂b = P2−P0:
				//   ∂P/∂u = ( (P1−P0)·Δv2 − (P2−P0)·Δv1 ) / det
				//   ∂P/∂v = ( (P2−P0)·Δu1 − (P1−P0)·Δu2 ) / det
				// Same with Ni in place of Pi for the raw normal
				// interpolant.
				//
				// Fallbacks (valid still set to true so downstream SMS
				// code doesn't spike):
				//   - |det| < ε : pole triangle or collinear-UV triangle.
				//     Fall back to edge-frame derivatives.
				//   - pCoords[i] null on any vertex: no UV data; same
				//     fall-back.
				const Vector3 shadingNormal = Vector3Ops::Normalize( ri.vNormal );
				const Vector3 e1 = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
				const Vector3 e2 = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );

				// UV deltas (if vertex UVs are available).
				bool useUVJacobian = false;
				Scalar uvDet = 0.0;
				Scalar duA = 0.0, duB = 0.0, dvA = 0.0, dvB = 0.0;
				if( thisTri.pCoords[0] && thisTri.pCoords[1] && thisTri.pCoords[2] ) {
					duA = thisTri.pCoords[1]->x - thisTri.pCoords[0]->x;
					duB = thisTri.pCoords[2]->x - thisTri.pCoords[0]->x;
					dvA = thisTri.pCoords[1]->y - thisTri.pCoords[0]->y;
					dvB = thisTri.pCoords[2]->y - thisTri.pCoords[0]->y;
					uvDet = duA * dvB - duB * dvA;
					if( fabs( uvDet ) > NEARZERO ) {
						useUVJacobian = true;
					}
				}


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
					// Fall back to edge-frame parameterisation (barycentric).
					dpdu = e1;
					dpdv = e2;
				}

				// Project dpdu, dpdv onto the shading-normal tangent plane.
				// (RISE-only addition; pbrt-v4 and Mitsuba 3 emit raw UV-
				// inverted derivatives without projection.)  Removing this
				// projection is being measured for SMS-Newton accuracy:
				// SMS's UpdateVertexOnSurface predicts new position via
				// `position + dpdu·du + dpdv·dv` and re-snaps to the
				// surface; if dpdu has its shading-N component dropped,
				// the prediction is off in that direction by exactly the
				// shading-vs-geometric-normal angle, which on a coarsely
				// tessellated curved mesh is non-trivial.  NormalMap does
				// its own projection at NormalMap.cpp:129 — independent
				// of this choice.  Set the gate to 0 to remove the
				// projection (match pbrt / Mitsuba conventions).
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

				// Compute ∂N/∂u, ∂N/∂v — same inversion applied to the
				// vertex normal differences, then projected onto the
				// shading tangent plane and normalized by |N_raw| to
				// get the derivative of the unit shading normal.
				Vector3 dndu( 0, 0, 0 );
				Vector3 dndv( 0, 0, 0 );
				if( thisTri.pNormals[0] && thisTri.pNormals[1] && thisTri.pNormals[2] ) {
					const Vector3 dNraw_dA(
						thisTri.pNormals[1]->x - thisTri.pNormals[0]->x,
						thisTri.pNormals[1]->y - thisTri.pNormals[0]->y,
						thisTri.pNormals[1]->z - thisTri.pNormals[0]->z );
					const Vector3 dNraw_dB(
						thisTri.pNormals[2]->x - thisTri.pNormals[0]->x,
						thisTri.pNormals[2]->y - thisTri.pNormals[0]->y,
						thisTri.pNormals[2]->z - thisTri.pNormals[0]->z );

					// Invert to (u, v) parameterisation, or fall back to
					// (A, B) barycentric.
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

				// Landing 2: project ray differentials onto the surface
				// UV plane and store the texture-space footprint.  Costs
				// nothing when ray.hasDifferentials = false (early-out
				// inside the helper).
				ComputeTextureFootprint( ri, ri.ray );
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

