//////////////////////////////////////////////////////////////////////
//
//  PRISEMeshGeometry.h - Definition of a geometry class that is 
//    made up of simple triangles in a PRISEOctree
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: Deceber 13, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PRISE_MESH_GEOMETRY_
#define PRISE_MESH_GEOMETRY_

#include "../Interfaces/IGeometry.h"
#include "../Interfaces/ISerializable.h"
#include "../Geometry/Geometry.h"
#include "../Polygon.h"
#include <vector>

template< class T >
class PRISEOctree;

class PRISEMeshGeometry : public virtual IGeometry, public virtual ISerializable, public virtual Implementation::Geometry
{
protected:
	virtual ~PRISEMeshGeometry();


public:
	typedef std::vector<Triangle>				MyTriangleList;

protected:
	MyTriangleList			polygons;			// The list of polygons		

	unsigned int			nMaxPerOctantNode;	// Maximum number of polygons per octant node
	unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree

	unsigned int			nMaxPerNodeLevel2;	// Maximum number of polygons in the 2nd level octree
	unsigned char			nMaxRecursionLevel2;	// Maximum level of recursion for the 2nd level octree

	PRISEOctree<Triangle>*	pPolygonsOctree;

public:
	PRISEMeshGeometry( const unsigned int max_polys_per_node, const unsigned char max_recursion_level, const unsigned int max_polys_level2, const unsigned char max_recur_level2 );

	// From ISerializable interface
	void Serialize( IWriteBuffer& buffer ) const;
	void Deserialize( IReadBuffer& buffer );

	// More serialization stuff
	// Asks us to serialize ourselves for the given CPU number
	void SerializeForCPU( IWriteBuffer& buffer, int cpu ) const;

	int CPUFromCallStack( unsigned int nCallStack ) const;

	void GenerateMesh( );
	void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
	bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

	void GenerateBoundingSphere( Point3D& ptCenter, Scalar& radius ) const;
	void GenerateBoundingBox( Point3D& ll, Point3D& ur ) const;
	bool DoPreHitTest( ) const { return true; };

	void getUniformRandomPoint( Point3D* point, Vector3D* normal, Point2D* coord, const Point3D& prand ) const;
	Scalar getArea() const;

	// Functions special to this class

	// Adds a triangle to the existing list of triangles
	void BeginTriangles( );						// I'm going to feed you a bunch of triangles
	void AddTriangle( const Triangle& tri );
	void DoneTriangles( );						// I'm done feeding you a bunch of triangles


	// This asks us to process the octree
	void SegmentOctreeForCPUS( const unsigned int num_cpus );
};

#endif

