//////////////////////////////////////////////////////////////////////
//
//  MAXGeometry.h - 3D Studio MAX geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 13, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAX_GEOMETRY_
#define MAX_GEOMETRY_

#include "MAX2RISE_Helpers.h"
#include "HitInfo.h"

namespace RISE
{
	static const Scalar		error_delta_box_size = 0.0001;
}
#include <BSPTree.h>

class MAXGeometry : 
	public virtual RISE::IGeometry, 
	public virtual RISE::Implementation::Reference,
	public virtual RISE::TreeElementProcessor<const int>
{
protected:
	Instance* instance;
	Mesh* mesh;

	RISE::BoundingBox bbox;
	RISE::Scalar m_dArea;

	// The bsp tree to accelerate our polygons
	RISE::BSPTree<const int>*		pBSPTree;



public:
	MAXGeometry(
		Instance* pMAXObject_
		) : 
	  instance( pMAXObject_ ), 
	  m_dArea( 0 ),
	  pBSPTree( 0 )
	{
		bbox = RISE::BoundingBox( RISE::Point3( RISE::INFINITY, RISE::INFINITY, RISE::INFINITY ), RISE::Point3( -RISE::INFINITY, -RISE::INFINITY, -RISE::INFINITY ) );

		mesh = instance->GetMesh();

		// Compute the bounding box
		int i = 0;
		for( i=0; i<mesh->numVerts; i++ ) {
			bbox.Include( MAX2RISEPoint( mesh->verts[i] ) );
		}

		std::vector<const int> temp;

		// Compute the area and copy the faces to the temporary vector
		for( i=0; i<mesh->numFaces; i++ ) {
			Face* f = &mesh->faces[i];

			const RISE::Point3 v0 = MAX2RISEPoint( mesh->verts[f->v[0]] );
			const RISE::Point3 v1 = MAX2RISEPoint( mesh->verts[f->v[1]] );
			const RISE::Point3 v2 = MAX2RISEPoint( mesh->verts[f->v[2]] );
			RISE::Vector3 vEdgeA = RISE::Vector3Ops::mkVector3( v1, v0 );
			RISE::Vector3 vEdgeB = RISE::Vector3Ops::mkVector3( v2, v0 );
			m_dArea += (RISE::Vector3Ops::Magnitude(RISE::Vector3Ops::Cross(vEdgeA,vEdgeB))) * 0.5;

			temp.push_back( i );
		}

		// Setup the BSP tree
		pBSPTree = new RISE::BSPTree<const int>( *this, bbox, 10 );
		RISE::GlobalLog()->PrintNew( pBSPTree, __FILE__, __LINE__, "max bsptree" );

		pBSPTree->AddElements( temp, 24 );
	}

	~MAXGeometry()
	{
		RISE::safe_release( pBSPTree );
	}

	// Geometry interface
	void GenerateMesh( ) {};

	void IntersectRay( RISE::RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		// Pass call through to the BSP tree
		if( pBSPTree ) {
			pBSPTree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
		}
	}

	bool IntersectRay_IntersectionOnly( const RISE::Ray& r, const RISE::Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		// Pass call through to the BSP tree
		if( pBSPTree ) {
			return pBSPTree->IntersectRay_IntersectionOnly( r, dHowFar, bHitFrontFaces, bHitBackFaces );
		}

		return false;
	}

	void GenerateBoundingSphere( RISE::Point3& ptCenter, RISE::Scalar& radius ) const
	{
		ptCenter = bbox.GetCenter();
		RISE::Vector3 radii = bbox.GetExtents()*0.5;
		radius = std::max<double>( std::max<double>( radii.x, radii.y ), radii.z );
	}

	RISE::BoundingBox GenerateBoundingBox() const
	{
		return bbox;
	}

	inline bool DoPreHitTest( ) const 
	{ 
		return true;
	};

	void UniformRandomPoint( RISE::Point3* point, RISE::Vector3* normal, RISE::Point2* coord, const RISE::Point3& prand ) const
	{
		// Select a random face
		const unsigned int findex = (unsigned int)floor(prand.z * mesh->numFaces);
		Face* f = &mesh->faces[findex];

		// Now use the other two random numbers to pick a random location on this face
		RISE::Triangle triangle;

		Point3 vxnormal[3];

		if( normal ) {
			getNormalsForFace( *instance, findex, vxnormal );
		}
		
		for( int i=0; i<3; i++ ) {
			triangle.vertices[i] = MAX2RISEPoint( mesh->getVert(f->v[i]) );
			if( normal ) {
				triangle.normals[i] = MAX2RISEVector( vxnormal[i] );
			}

			if( coord && mesh->numTVerts && mesh->tVerts ) {
				triangle.coords[i] = MAX2RISEUV( mesh->getTVert(f->v[i]) );
			}
		}

		RISE::GeometricUtilities::PointOnTriangle( point, normal, coord, triangle, prand.x, prand.y );
	}

	RISE::Scalar GetArea() const
	{
		return m_dArea;
	}

	// From TreeElementProcessor
	typedef const int MYOBJ;
	void RayElementIntersection( RISE::RayIntersectionGeometric& ri, const MYOBJ findex, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		Face& f = mesh->faces[findex];
		RISE::Point3 v0, v1, v2;
		// Get the vertices
		if( !instance->TestFlag(INST_TM_NEGPARITY) ) {
			v0 = MAX2RISEPoint( mesh->getVert(f.getVert(0)) );
			v1 = MAX2RISEPoint( mesh->getVert(f.getVert(1)) );
			v2 = MAX2RISEPoint( mesh->getVert(f.getVert(2)) );
		} else {
			// Scaling is negative, get the vertives
			// counter clockwise.
			v0 = MAX2RISEPoint( mesh->getVert(f.getVert(2)) );
			v1 = MAX2RISEPoint( mesh->getVert(f.getVert(1)) );
			v2 = MAX2RISEPoint( mesh->getVert(f.getVert(0)) );
		}

		RISE::TRIANGLE_HIT	h;
		h.bHit = false;
		h.dRange = RISE::INFINITY;

		// We have to intersect against every triangle and find the closest intersection
		// We can omit triangles that aren't facing us (dot product is > 0) since they 
		// can't possibly hit

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...
		const RISE::Vector3 vEdgeA = RISE::Vector3Ops::mkVector3( v1, v0 );
		const RISE::Vector3 vEdgeB = RISE::Vector3Ops::mkVector3( v2, v0 );
		const RISE::Vector3 vFaceNormal = RISE::Vector3Ops::Cross( vEdgeA, vEdgeB );

		// If we are not to hit front faces and we are front facing, then beat it!
		if( !bHitFrontFaces  ) {
			if( RISE::Vector3Ops::Dot(vFaceNormal, ri.ray.dir) < 0 ) {
				return;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( RISE::Vector3Ops::Dot(vFaceNormal, ri.ray.dir) > 0 ) {
				return;
			}
		}

		{	
			RISE::RayTriangleIntersection( ri.ray, h, v0, vEdgeA, vEdgeB );

			if( h.bHit /* && h.dRange > 0.01*/ ) {
				ri.bHit = true;
				ri.range = h.dRange;

				const RISE::Scalar&	a = h.alpha;
				const RISE::Scalar&	b = h.beta;

				// Compute the normal
				{
					Point3 vxnormal[3];
					getNormalsForFace( *instance, findex, vxnormal );

					ri.vNormal = MAX2RISEVector(vxnormal[0])+
						(MAX2RISEVector(vxnormal[1])-MAX2RISEVector(vxnormal[0]))*a+
						(MAX2RISEVector(vxnormal[2])-MAX2RISEVector(vxnormal[0]))*b;
				}

				/* Extra sanity, shouldn't really be required
				if( RISE::Vector3Ops::Dot( vFaceNormal, ri.vNormal ) < 0 ) {
					ri.vNormal = -ri.vNormal;
				}
				*/

				// Compute the texture co-ordinate
				if( mesh->numTVerts && mesh->tVerts ) {
//				UVVert* tverts = mesh->mapVerts(0);
//				TVFace* tvf = &mesh->mapFaces(0)[findex];
//				if( tverts && tvf )
//				{
//					const RISE::Point2 uv0 = MAX2RISEUV( tverts[tvf->t[0]] );
//					const RISE::Point2 uv1 = MAX2RISEUV( tverts[tvf->t[1]] );
//					const RISE::Point2 uv2 = MAX2RISEUV( tverts[tvf->t[2]] );

					const RISE::Point2 uv0 = MAX2RISEUV( mesh->getTVert(f.v[0]) );
					const RISE::Point2 uv1 = MAX2RISEUV( mesh->getTVert(f.v[1]) );
					const RISE::Point2 uv2 = MAX2RISEUV( mesh->getTVert(f.v[2]) );

					ri.ptCoord = RISE::Point2Ops::mkPoint2(uv0,
						RISE::Vector2Ops::mkVector2(uv1,uv0)*a+
						RISE::Vector2Ops::mkVector2(uv2,uv0)*b );
				} else {
					ri.ptCoord = RISE::Point2( 0.0, 0.0 );
				}

				// Set the custom information
				HitInfo* hit = new HitInfo;
				RISE::GlobalLog()->PrintNew( hit, __FILE__, __LINE__, "HitInfo" );

				hit->baryCoord = Point3( 1.0-(a+b), a, b );
				hit->faceNum = findex;
				hit->instance = instance;
				ri.pCustom = hit;
			}
		}
	}

	void RayElementIntersection( RISE::RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
	}

	bool RayElementIntersection_IntersectionOnly( const RISE::Ray& ray, const RISE::Scalar dHowFar, const MYOBJ findex, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		Face& f = mesh->faces[findex];
		RISE::Point3 v0, v1, v2;
		// Get the vertices
		if( !instance->TestFlag(INST_TM_NEGPARITY) ) {
			v0 = MAX2RISEPoint( mesh->getVert(f.getVert(0)) );
			v1 = MAX2RISEPoint( mesh->getVert(f.getVert(1)) );
			v2 = MAX2RISEPoint( mesh->getVert(f.getVert(2)) );
		} else {
			// Scaling is negative, get the vertives
			// counter clockwise.
			v0 = MAX2RISEPoint( mesh->getVert(f.getVert(2)) );
			v1 = MAX2RISEPoint( mesh->getVert(f.getVert(1)) );
			v2 = MAX2RISEPoint( mesh->getVert(f.getVert(0)) );
		}

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...

		// Early rejection is based on whether we are to consider front facing triangles or 
		// back facing triangles and so on...
		const RISE::Vector3 vEdgeA = RISE::Vector3Ops::mkVector3( v1, v0 );
		const RISE::Vector3 vEdgeB = RISE::Vector3Ops::mkVector3( v2, v0 );
		const RISE::Vector3 vFaceNormal = RISE::Vector3Ops::Cross( vEdgeA, vEdgeB );

		// If we are not to hit front faces and we are front facing, then beat it!
		if( !bHitFrontFaces  ) {
			if( RISE::Vector3Ops::Dot(vFaceNormal, ray.dir) < 0 ) {
				return false;
			}
		}

		// If we are not to hit back faces and we are back facing, then also beat it
		if( !bHitBackFaces ) {
			if( RISE::Vector3Ops::Dot(vFaceNormal, ray.dir) > 0 ) {
				return false;
			}
		}

		{
			RISE::TRIANGLE_HIT h;
			RISE::RayTriangleIntersection( ray, h, v0, vEdgeA, vEdgeB );

			if( h.bHit && (h.dRange > RISE::NEARZERO && h.dRange < dHowFar) ) {
				return true;
			}
		}

		return false;
	}

	bool ElementBoxIntersection( const MYOBJ findex, const RISE::BoundingBox& bbox ) const
	{
		const Face& f = mesh->faces[findex];
		RISE::Point3 v[3];
		for( int i=0; i<3; i++ ) {
			v[i] = MAX2RISEPoint(mesh->verts[f.v[i]]);
		}

		//
		// Trivial acception, any of the points are inside the box
		//
		for( int j=0; j<3; j++ ) {
			if( RISE::GeometricUtilities::IsPointInsideBox( v[i], bbox.ll, bbox.ur ) ) {
				// Then this polygon qualifies
				return true;
			}
		}
			
		//
		// Check if any of the triangle's edges intersect the box
		//

		// Edge 1
		RISE::BOX_HIT		h;
		RISE::Ray			ray;
		RISE::Scalar		fEdgeLength;

		ray.origin = v[0];
		ray.dir = RISE::Vector3Ops::mkVector3( v[1], v[0] );
		fEdgeLength = RISE::Vector3Ops::NormalizeMag(ray.dir);

		RISE::RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}

		// Edge 2
		ray.origin = v[1];
		ray.dir = RISE::Vector3Ops::mkVector3( v[2], v[1] );
		fEdgeLength = RISE::Vector3Ops::NormalizeMag(ray.dir);

		RISE::RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
		if( h.bHit && h.dRange <= fEdgeLength ) {
			return true;
		}
		

		// Edge 3
		ray.origin = v[2];
		ray.dir = RISE::Vector3Ops::mkVector3( v[0], v[2] );
		fEdgeLength = RISE::Vector3Ops::NormalizeMag(ray.dir);

		RISE::RayBoxIntersection( ray, h, bbox.ll, bbox.ur );
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
		RISE::BoundingBox bbTri( RISE::Point3(RISE::INFINITY,RISE::INFINITY,RISE::INFINITY), RISE::Point3(-RISE::INFINITY,-RISE::INFINITY,-RISE::INFINITY));
		for( int j=0; j<3; j++ ) {
			bbTri.Include( v[j] );
		}

		return bbTri.DoIntersect( bbox );	
	}

	char WhichSideofPlaneIsElement( const MYOBJ findex, const RISE::Plane& plane ) const
	{
		const Face& f = mesh->faces[findex];
		const RISE::Point3 v0 = MAX2RISEPoint(mesh->verts[f.v[0]]);
		const RISE::Point3 v1 = MAX2RISEPoint(mesh->verts[f.v[1]]);
		const RISE::Point3 v2 = MAX2RISEPoint(mesh->verts[f.v[2]]);

		const RISE::Scalar d0 = plane.Distance( v0 );

		if( d0 < RISE::NEARZERO ) {
			const RISE::Scalar d1 = plane.Distance( v1 );

			if( d1 < RISE::NEARZERO ) {
				const RISE::Scalar d2 = plane.Distance( v2 );

				if( d2 < RISE::NEARZERO ) {
					return 0;
				} else {
					return 2;
				}
			} else {
				return 2;
			}

		} else if( d0 > RISE::NEARZERO ) {
			const RISE::Scalar d1 = plane.Distance( v1 );

			if( d1 > RISE::NEARZERO ) {
				const RISE::Scalar d2 = plane.Distance( v2 );

				if( d2 > RISE::NEARZERO ) {
					return 1;
				} else {
					return 2;
				}
			} else {
				return 2;
			}

		} else {
			return 2;
		}
	}

	void SerializeElement( RISE::IWriteBuffer& buffer, const MYOBJ elem ) const
	{
	}
	void DeserializeElement( RISE::IReadBuffer& buffer, MYOBJ& ret ) const
	{
	}

	// Keyframable interface
	// do nothing
	RISE::IKeyframeParameter* KeyframeFromParameters( const RISE::String& name, const RISE::String& value ){return 0;}
	void SetIntermediateValue( const RISE::IKeyframeParameter& val ) {}
	void RegenerateData( ) {}
};


#endif

