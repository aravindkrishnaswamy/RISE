
//////////////////////////////////////////////////////////////////////
//
//  BezierValueGenerator.cpp - Takes a specialized MYBEZIERPATCH
//    and generates the actual patch from it.  Used by the MRUcache
//    and BezierPatchGeometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 20, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BEZIER_VALUE_GENERATOR
#define BEZIER_VALUE_GENERATOR

#include "BezierTesselation.h"
#include "../Utilities/MRUCache.h"
#include "TriangleMeshGeometryIndexed.h"
#include "GeometryUtilities.h"
#include "../Interfaces/IFunction2D.h"

namespace RISE
{
	struct MYBEZIERPATCH
	{
		BezierPatch*				pPatch;
		unsigned int				id;
		BoundingBox					bbox;

		bool operator==( const MYBEZIERPATCH& other ) const
		{
			return other.id == id;
		}
	};

	class BezierValueGenerator : 
		public ValueGenerator<MYBEZIERPATCH, ITriangleMeshGeometryIndexed>
	{
	protected:
		unsigned int			nMaxPerOctantNode;	// Maximum number of polygons per octant node
		unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree

		bool					bDoubleSided;		// Are the polygons all double sided?
		bool					bUseBSP;			// Are we using BSP trees ?

		bool					bUseFaceNormals;	// Are we going to use computed face normals rather than interpolated vertex normals?

		unsigned int			detail;				// Tesselation level

	public:
		const IFunction2D*		displacement;		// Displacement function
		Scalar					disp_scale;			// Scale for displacement

		BezierValueGenerator(
			const unsigned int max_polys_per_node, 
			const unsigned char max_recursion_level, 
			const bool bDoubleSided_,
			const bool bUseBSP_,
			const bool bUseFaceNormals_,
			const unsigned int detail_,
			const IFunction2D* displacement_,
			const Scalar disp_scale_
			) : 
		nMaxPerOctantNode( max_polys_per_node ),
		nMaxRecursionLevel( max_recursion_level), 
		bDoubleSided( bDoubleSided_ ),
		bUseBSP( bUseBSP_ ),
		bUseFaceNormals( bUseFaceNormals_ ),
		detail( detail_ ),
		displacement( displacement_ ),
		disp_scale( disp_scale_ )
		{
		}
		
		virtual ~BezierValueGenerator()
		{
		}

		ITriangleMeshGeometryIndexed * Get(const MYBEZIERPATCH& k)
		{
			ITriangleMeshGeometryIndexed* ret = new Implementation::TriangleMeshGeometryIndexed(
				nMaxPerOctantNode, nMaxRecursionLevel, bDoubleSided, bUseBSP, bUseFaceNormals );
			GlobalLog()->PrintNew( ret, __FILE__, __LINE__, "triangle mesh geometry indexed" );

			ret->BeginIndexedTriangles();

			IndexTriangleListType			indtris;
			VerticesListType				vertices;
			NormalsListType					normals;
			TexCoordsListType				coords;

			GeneratePolygonsFromBezierPatch( indtris, vertices, normals, coords, *k.pPatch, detail );

			if( !bUseFaceNormals ) {
				CalculateVertexNormals( indtris, normals, vertices );
			}

			if( displacement ) {
				RemapTextureCoords( coords );
				ApplyDisplacementMapToObject( indtris, vertices, normals, coords, *displacement, disp_scale );
			}

			ret->AddVertices( vertices );
			ret->AddNormals( normals );
			ret->AddTexCoords( coords );
			ret->AddIndexedTriangles( indtris );

			ret->DoneIndexedTriangles();

			return ret;
		}

		void Return(const MYBEZIERPATCH& k, ITriangleMeshGeometryIndexed * s)
		{
			safe_release( s );	
		}
	};
}

#endif
