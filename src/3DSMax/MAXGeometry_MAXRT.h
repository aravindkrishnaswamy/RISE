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

#ifndef MAX_GEOMETRY_MAXRT_
#define MAX_GEOMETRY_MAXRT_

#include "MAX2RISE_Helpers.h"

class MAXGeometry_MAXRT : 
	public virtual RISE::IGeometry, 
	public virtual RISE::Implementation::Reference
{
protected:
	Mesh* mesh;

	RISE::BoundingBox bbox;
	RISE::Scalar m_dArea;

public:
	MAXGeometry_MAXRT(
		Mesh* pMAXObject_
		) : 
	  mesh( pMAXObject_ ), 
	  m_dArea( 0 )
	{
		bbox = RISE::BoundingBox( RISE::Point3( RISE::INFINITY, RISE::INFINITY, RISE::INFINITY ), RISE::Point3( -RISE::INFINITY, -RISE::INFINITY, -RISE::INFINITY ) );

		// Compute the bounding box
		int i = 0;
		for( i=0; i<mesh->numVerts; i++ ) {
			bbox.Include( MAX2RISEPoint( mesh->verts[i] ) );
		}

		// Compute the area
		for( i=0; i<mesh->numFaces; i++ ) {
			Face* f = &mesh->faces[i];

			const RISE::Point3 v0 = MAX2RISEPoint( mesh->verts[f->v[0]] );
			const RISE::Point3 v1 = MAX2RISEPoint( mesh->verts[f->v[1]] );
			const RISE::Point3 v2 = MAX2RISEPoint( mesh->verts[f->v[2]] );
			RISE::Vector3 vEdgeA = RISE::Vector3Ops::mkVector3( v1, v0 );
			RISE::Vector3 vEdgeB = RISE::Vector3Ops::mkVector3( v2, v0 );
			m_dArea += (RISE::Vector3Ops::Magnitude(RISE::Vector3Ops::Cross(vEdgeA,vEdgeB))) * 0.5;
		}
	}

	~MAXGeometry_MAXRT()
	{
	}

	// Geometry interface
	void GenerateMesh( ) {};

	void IntersectRay( RISE::RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
		// Pass the call through to MAX
		float at=-1.0;
		Point3 norm;
		Ray ray;
		ray.p = Point3( ri.ray.origin.x, ri.ray.origin.y, ri.ray.origin.z );
		ray.dir = Point3( ri.ray.dir.x, ri.ray.dir.y, ri.ray.dir.z );
		mesh->IntersectRay( ray, at, norm );

		if( at > 0 ) {
			ri.bHit = true;
			ri.range = ri.range2 = at;
			ri.vNormal = RISE::Vector3( norm.x, norm.y, norm.z );

			// We still need to do something about texture co-ordinates
		}
	}

	bool IntersectRay_IntersectionOnly( const RISE::Ray& r, const RISE::Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		// Pass the call through to MAX
		float at=-1.0;
		Point3 norm;
		Ray ray;
		ray.p = Point3( r.origin.x, r.origin.y, r.origin.z );
		ray.dir = Point3( r.dir.x, r.dir.y, r.dir.z );
		mesh->IntersectRay( ray, at, norm );

		if( at > 0 ) {
			if( at < dHowFar ) {
				return true;
			}
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
			getNormalsForFace( *mesh, *f, vxnormal );
		}
		
		for( int i=0; i<3; i++ ) {
			triangle.vertices[i] = MAX2RISEPoint( mesh->getVert(f->v[i]) );
			if( normal ) {
				triangle.normals[i] = MAX2RISEVector( vxnormal[i] );
			}

			if( coord ) {
				triangle.coords[i] = MAX2RISEUV( mesh->getTVert(f->v[i]) );
			}
		}

		RISE::GeometricUtilities::PointOnTriangle( point, normal, coord, triangle, prand.x, prand.y );
	}

	RISE::Scalar GetArea() const
	{
		return m_dArea;
	}

	// Keyframable interface
	// do nothing
	RISE::IKeyframeParameter* KeyframeFromParameters( const RISE::String& name, const RISE::String& value ){return 0;}
	void SetIntermediateValue( const RISE::IKeyframeParameter& val ) {}
	void RegenerateData( ) {}
};


#endif

