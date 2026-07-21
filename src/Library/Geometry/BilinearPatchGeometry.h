//////////////////////////////////////////////////////////////////////
//
//  BilinearPatchGeometry.h - Bilinear patch geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 18, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BILINEARPATCH_GEOMETRY_
#define BILINEARPATCH_GEOMETRY_

#include "Geometry.h"
#include "../Interfaces/IBilinearPatchGeometry.h"
#include "../Octree.h"
#include "../BSPTreeSAH.h"

namespace RISE
{
	namespace Implementation
	{
		class BilinearPatchGeometry : 
			public virtual IBilinearPatchGeometry,
			public virtual Geometry,
			public virtual TreeElementProcessor<const BilinearPatch*>
		{
		public:
			typedef std::vector<BilinearPatch>			BilinearPatchList;
			typedef std::vector<const BilinearPatch*>	BilinearPatchPtrList;


		protected:
			// The BSP tree of bezier patches
				BSPTreeSAH<const BilinearPatch*>*	pBSPTree;

			// The Octree tree of bezier patches
			Octree<const BilinearPatch*>*		pOctree;

			BilinearPatchList		patches;			// List of bezier patches
			unsigned int			nMaxPerOctantNode;	// Maximum number of patches per octant node
			unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree
			bool					bUseBSP;			// Are we using BSP trees ?

			//! Area-light sampling tables, built once in Prepare().
			//! `vAreaCDF` is ONE global cumulative table over every (patch,
			//! cell) pair -- nAreaCells^2 consecutive entries per patch, in
			//! patch order -- so a single binary search picks both the patch
			//! and the cell within it, area-proportionally.  `dTotalArea` is
			//! the same quadrature's total, so 1/GetArea() is exactly the
			//! sampler's density (see GeometricUtilities::
			//! AccumulateBilinearAreaCDF for why that identity, not absolute
			//! quadrature accuracy, is what unbiasedness rests on).
			static const unsigned int nAreaCells = 8;
			std::vector<Scalar>		vAreaCDF;
			Scalar					dTotalArea;

			virtual ~BilinearPatchGeometry( );

		public:
			BilinearPatchGeometry(
				const unsigned int max_polys_per_node, 
				const unsigned char max_recursion_level,
				const bool bUseBSP
				);

			// Adds a new patch to the list
			void AddPatch( const BilinearPatch& patch );

			// Instructs that addition of new patches is complete and that
			// we can prepare for rendering
			void Prepare();

			// Tessellates every stored bilinear patch as a (detail+1) x (detail+1) bilinear grid,
			// concatenated.  Per-patch corner mapping is the CANONICAL RISE convention:
			// pts[0]->UV(0,0), pts[1]->UV(0,1), pts[2]->UV(1,0), pts[3]->UV(1,1) -- NOT
			// row-major.  (This comment previously documented the pre-fix row-major order,
			// which disagreed with the body, EvaluateBilinearPatchAt and
			// RayBilinearPatchIntersection; it misled the 2026-07 area-light work.)
			// Normals recomputed from triangle topology.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			//! \return False when there is no sampleable surface, so this
			//! geometry is never registered as a luminary in that state.
			//! Before 2026-07 this inherited IGeometry's `true` default while
			//! UniformRandomPoint was an unimplemented stub and GetArea
			//! returned a hardcoded 1.0 -- a `bilinearpatch_geometry` bound to
			//! an emissive material became a silently broken area light whose
			//! every NEE sample landed on the object's local origin.
			bool CanBeAreaLight() const { return !patches.empty() && dTotalArea > 0; }

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// From TreeElementProcessor
			typedef const BilinearPatch*		MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const;
				bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const;
				char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif
