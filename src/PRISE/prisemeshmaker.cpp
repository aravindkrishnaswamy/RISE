//////////////////////////////////////////////////////////////////////
//
//  prisemeshmaker.cpp - This takes a regular mesh and makes a 
//    prisemesh file, which can be used with prise. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  NOTE: this processes a mesh for a particular
//    number of processors before hand, this should be done is a more
//    robust and efficient way, but...
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


// We use the triangle mesh interface to load data from the 3ds loader
// then convert to plain triangles which the PRISEMeshGeometry can use
#include <iostream>
#include "../src/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Utilities/Reference.h"
#include "../src/PRISE/PRISEMeshGeometry.h"
#include "../deadcode/TriangleMeshLoaderASE.h"
#include "../src/Geometry/TriangleMeshLoader3DS.h"
#include "../src/Geometry/Geometry.h"
#include "../src/Utilities/MemoryBuffer.h"

using namespace Implementation;

class TriangleMeshPolyConverter : public virtual ITriangleMeshGeometry, public virtual Implementation::Reference, public virtual Implementation::Geometry
{
protected:
	virtual ~TriangleMeshPolyConverter()
	{
	}
	
	TriangleListType			polygons;			// The list of polygons
	IndexTriangleListType		indexedtris;		// List of indexed polygons
	VerticesListType			pPoints;			// The list of points
	NormalsListType				pNormals;			// The list of normals
	TexCoordsListType			pCoords;			// The list of coords

public:

	TriangleMeshPolyConverter()
	{
	}

	// Adds a triangle to the existing list of triangles
	virtual void BeginTriangles()
	{
	}

	virtual void AddTriangle( const Triangle& tri )
	{
		polygons.push_back( tri );
	}

	virtual void DoneTriangles()
	{
	}
	
	// Adds indexed triangle lists
	virtual void BeginIndexedTriangles()
	{
	}

	virtual void AddVertex( const Vertex& point )
	{
		pPoints.push_back( point );
	}

	virtual void AddNormal( const Normal& normal )
	{
		pNormals.push_back( normal );
	}

	virtual void AddTexCoord( const TexCoord& coord )
	{
		pCoords.push_back( coord );
	}

	virtual void AddVertices( const VerticesListType& points )
	{
		pPoints.insert( pPoints.end(), points.begin(), points.end() );
	}

	virtual void AddNormals( const NormalsListType& normals )
	{
		pNormals.insert( pNormals.end(), normals.begin(), normals.end() );
	}

	virtual void AddTexCoords( const TexCoordsListType& coords )
	{
		pCoords.insert( pCoords.end(), coords.begin(), coords.end() );
	}

	virtual void AddIndexedTriangle( const IndexedTriangle& tri )
	{
		indexedtris.push_back( tri );
	}

	virtual void AddIndexedTriangles( const IndexTriangleListType& tris )
	{
		indexedtris.insert( indexedtris.end(), tris.begin(), tris.end() );
	}

	unsigned int numPoints( )			{ return pPoints.size(); }
	unsigned int numNormals( )			{ return pNormals.size(); }
	unsigned int numCoords( )			{ return pCoords.size(); }

	virtual void DoneIndexedTriangles()
	{
	}

	virtual void LoadTriangleMesh( PRISEMeshGeometry* pGeom )
	{
		pGeom->BeginTriangles();

		IndexTriangleListType::const_iterator	i, e;
		
		for( i=indexedtris.begin(), e=indexedtris.end(); i!=e; i++ )
		{
			const IndexedTriangle& itri = *i;
			Triangle	tri;

			for( int i=0; i<3; i++ ) {
				tri.vertices[i] = pPoints[ itri.iVertices[i] ];
				tri.normals[i] = pNormals[ itri.iNormals[i] ];
				tri.coords[i] = pCoords[ itri.iCoords[i] ];
			}

			pGeom->AddTriangle( tri );
		}

		TriangleListType::const_iterator m, n;
		for( m=polygons.begin(), n=polygons.end(); m!=n; m++ ) {
			pGeom->AddTriangle( *m );
		}

		pGeom->DoneTriangles();
	}

	virtual void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
	{
	}
	virtual bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		return false;
	}
	virtual void GenerateBoundingSphere( Point3D& ptCenter, Scalar& radius ) const
	{
	}
	virtual void GenerateBoundingBox( Point3D& ll, Point3D& ur ) const
	{
	}
	virtual bool DoPreHitTest( ) const
	{
		return false;
	}
	virtual void getUniformRandomPoint( Point3D* point, Vector3D* normal, Point2D* coord, const Point3D& prand ) const
	{
	}
	virtual Scalar getArea( ) const
	{
		return 1.0;
	}
	virtual void Serialize( IWriteBuffer& buffer ) const
	{
	}
	virtual void Deserialize( IReadBuffer& buffer )
	{
	}
};

int main( int argc, char** argv )
{
	if( argc < 8 ) {
		std::cout << "Usage: prisemeshmaker <input mesh> <output pattern> <max polys/node (level1)> <max node levels (level1)> <num cpus> <max polys/node (level2)> <max node levels (level2)>" << std::endl;
		std::cout << "Example: meshconverter in.3ds out 15000 2  4   100 6 " << std::endl;
		return 1;
	}

	TriangleMeshPolyConverter*		conv = new TriangleMeshPolyConverter();
//	TriangleMeshLoader3DS*			loader = new TriangleMeshLoader3DS( argv[1] );
	TriangleMeshLoaderASE*			loader = new TriangleMeshLoaderASE( argv[1] );

	loader->LoadTriangleMesh( conv );
	loader->RemoveRef();

	PRISEMeshGeometry*				geom = new PRISEMeshGeometry( atoi(argv[3]), atoi(argv[4]), atoi(argv[6]), atoi(argv[7]) );
	conv->LoadTriangleMesh( geom );
	conv->RemoveRef();

	int numcpus = atoi(argv[5]);
	geom->SegmentOctreeForCPUS( numcpus );

	for( int i=-1; i<numcpus; i++ ) {
		MemoryBuffer*	mb = new MemoryBuffer();
		mb->Resize( 0x10000000 );
		geom->SerializeForCPU( *mb, i );

		char	filename[1024];
		sprintf( filename, "%s_%d.prisemesh", argv[2], i );

		mb->DumpToFileToCursor( filename );
		mb->RemoveRef();
	}

	geom->RemoveRef();

	return 1;
}