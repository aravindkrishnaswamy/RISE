//////////////////////////////////////////////////////////////////////
//
//  DisplacedGeometry.h - Composite geometry that wraps any IGeometry,
//  tessellates it via IGeometry::TessellateToMesh, and applies a
//  displacement map along the vertex normals.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DISPLACED_GEOMETRY_
#define DISPLACED_GEOMETRY_

#include "Geometry.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../Utilities/Observable.h"

namespace RISE
{
	namespace Implementation
	{
		// Wraps any IGeometry (including another DisplacedGeometry), tessellates it at
		// construction time via the base's TessellateToMesh, and displaces every vertex along
		// its normal by `displacement(u,v) * disp_scale`.  The resulting mesh is built into an
		// internal TriangleMeshGeometryIndexed and used for all intersection / sampling queries.
		//
		// If the base geometry's TessellateToMesh returns false (e.g. InfinitePlaneGeometry),
		// construction leaves the internal mesh null and IsValid() returns false.  All
		// IGeometry methods degrade to "miss" / empty behavior in that state — the caller is
		// expected to check IsValid() and refuse the geometry at scene-parse time.
		class DisplacedGeometry : public Geometry
		{
		protected:
			IGeometry*                       m_pBase;
			const IFunction2D*               m_pDisplacement;
			Scalar                           m_dispScale;
			unsigned int                     m_detail;
			bool                             m_bDoubleSided;
			bool                             m_bUseFaceNormals;
			ITriangleMeshGeometryIndexed*    m_pMesh;

			// Subscription to the displacement painter's Observable.  When the
			// painter notifies (e.g. a keyframed `time` parameter changed),
			// the callback rebuilds m_pMesh.
			//
			// Destruction ordering note: the destructor BODY runs before any
			// member destructors, so the subscription's own destructor would
			// otherwise fire AFTER m_pDisplacement->release() has potentially
			// deleted the painter.  ~DisplacedGeometry resets this member
			// explicitly at the top of the dtor body, before any release, so
			// Detach executes while the subject is still alive.
			Subscription                     m_displacementSubscription;

			virtual ~DisplacedGeometry();

			// Tessellate base + apply displacement + build internal indexed
			// mesh.  Idempotent: safe to call repeatedly via DestroyMesh()/
			// BuildMesh() to refresh after the displacement painter changes.
			void BuildMesh();
			void DestroyMesh();

			// Tier 1 §3 animation refit path: re-tessellate base + re-apply
			// displacement, then call m_pMesh->UpdateVertices() to swap
			// vertex/normal storage in place and refit the BVH instead of
			// rebuilding.  Falls back to DestroyMesh+BuildMesh if there
			// is no current mesh or topology has changed (which is not
			// currently possible — m_detail is fixed at construction).
			void RefreshMeshVertices();

		public:
			DisplacedGeometry(
				IGeometry*          pBase,
				const unsigned int  detail,
				const IFunction2D*  displacement,
				const Scalar        disp_scale,
				const bool          bDoubleSided,
				const bool          bUseFaceNormals );

			DisplacedGeometry( const DisplacedGeometry& ) = delete;
			DisplacedGeometry& operator=( const DisplacedGeometry& ) = delete;

			// True iff the internal mesh was built (base supported tessellation).
			bool IsValid() const { return m_pMesh != 0; }

			// IGeometry
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const;
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest() const { return true; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea() const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// Keyframable — static after construction; no keyframeable parameters.
			IKeyframeParameter* KeyframeFromParameters( const String& /*name*/, const String& /*value*/ ) { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& /*val*/ ) {}
			void RegenerateData() {}
		};
	}
}

#endif
