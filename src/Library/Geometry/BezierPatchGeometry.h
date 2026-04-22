//////////////////////////////////////////////////////////////////////
//
//  BezierPatchGeometry.h - Bezier patch geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 17, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BEZIERPATCH_GEOMETRY_
#define BEZIERPATCH_GEOMETRY_

#include "Geometry.h"
#include "../Interfaces/IBezierPatchGeometry.h"
#include "../Octree.h"
#include "../BSPTreeSAH.h"
#include "../Utilities/BoundingBox.h"

namespace RISE
{
	// Element type stored in the per-geometry BSP tree / Octree.  Pairs a
	// non-owning pointer to the underlying BezierPatch with its index and
	// precomputed AABB (the Bezier convex-hull bound over 16 control
	// points).  Equality is id-based so the accelerator's element set
	// behaves correctly when the same patch lives in multiple BSP leaves.
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

	namespace Implementation
	{
		class BezierPatchGeometry : 
			public virtual IBezierPatchGeometry,
			public virtual Geometry,
			public virtual TreeElementProcessor<MYBEZIERPATCH>
		{
		public:	

			typedef std::vector<BezierPatch>				BezierPatchList;
			typedef std::vector<MYBEZIERPATCH>			BezierPatchPtrList;

		protected:
			// BSP tree of bezier patches (one accelerator or the other).
			BSPTreeSAH<MYBEZIERPATCH>*		pBSPTree;

			// Octree of bezier patches (used when bUseBSP is false).
			Octree<MYBEZIERPATCH>*			pOctree;

			BezierPatchList			patches;			// List of bezier patches
			unsigned int			nMaxPerOctantNode;	// Max patches per accelerator leaf
			unsigned char			nMaxRecursionLevel;	// Max accelerator recursion depth
			bool					bUseBSP;			// BSP tree (true) or Octree (false)?

			virtual ~BezierPatchGeometry( );

		public:
			// Rendering is ALWAYS analytic (Kajiya resultant + 2D Newton polish).
			// The old tessellate-on-demand fallback (MRU cache + per-patch
			// `detail` + `face_normals` + polygon-accelerator sub-tree) is gone
			// along with its control parameters.  Scenes that want a tessellated
			// mesh wrap this geometry in a DisplacedGeometry (with
			// disp_scale=0 / displacement=none for a pure-tessellation path).
			// Displacement itself is also owned by DisplacedGeometry.
			BezierPatchGeometry(
				const unsigned int max_patches_per_node,
				const unsigned char max_recursion_level,
				const bool bUseBSP
				);

			// Adds a new patch to the list
			void AddPatch( const BezierPatch& patch );

			// Instructs that addition of new patches is complete and that
			// we can prepare for rendering
			void Prepare();

			// Tessellates every stored Bezier patch into triangles and concatenates them.
			// Per-patch grid is (detail+1) x (detail+1), using the shared patch tessellator.
			// This override does NOT apply the geometry's own stored `displacement` — callers
			// (e.g. DisplacedGeometry) own the displacement pass.  Vertex normals are left at
			// whatever the patch evaluator produces (which for the underlying utility is
			// zero-initialized — callers should recompute normals as needed).
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// From TreeElementProcessor
			typedef MYBEZIERPATCH		MYOBJ;
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
