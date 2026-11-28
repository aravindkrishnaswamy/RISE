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
#include "ClippedPlaneGeometry.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../Utilities/Observable.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace RISE
{
	namespace Implementation
	{
		// Wraps any tessellatable IGeometry, refines smooth indexed-mesh bases to
		// (base bbox diagonal / detail) without altering the authored silhouette/creases
		// (capped at 1.5M triangles), displaces along vertex normals, welds coincident
		// crease vertices per BaseWeldPolicy, and transports smooth shading normals.
		//
		// Two mutually exclusive height routes (multiplied by disp_scale):
		//   (a) `displacement` (IFunction2D) evaluated at (u, v);
		//   (b) `height` (IScalarPainter) evaluated in OBJECT space (P == Po coincide).
		//
		// Deferred realization: BuildMesh() runs once in Realize() (called by
		// RayCaster::AttachScene; direct C++ consumers must call Realize() before use).
		// Failed tessellation leaves m_pMesh null and degrades queries to miss/empty.
		class DisplacedGeometry : public Geometry
		{
		protected:
			IGeometry*                       m_pBase;
			const IFunction2D*               m_pDisplacement;
			const IScalarPainter*            m_pHeight;		//!< route (b); mutually exclusive with m_pDisplacement
			Scalar                           m_dispScale;
			unsigned int                     m_detail;
			bool                             m_bDoubleSided;
			bool                             m_bUseFaceNormals;
			bool                             m_bSeamFold;	//!< tent-fold UV on wrap seams; false = raw Cartesian UV
			const ClippedPlaneGeometry*      m_pPlaneBase;	//!< non-null if m_pBase is a clipped plane (borrowed from m_pBase)

			// Deferred-realization cache (const Realize() writes under m_realizeMutex;
			// hot path reads post-bake m_pMesh lock-free). m_pMesh is owned solely here.
			mutable ITriangleMeshGeometryIndexed*    m_pMesh;
			mutable std::atomic<bool>                m_bRealized;
			mutable std::mutex                       m_realizeMutex;

			// Observer subscription on the bound height source; reset first in
			// ~DisplacedGeometry() before releasing m_pDisplacement / m_pHeight.
			Subscription                     m_displacementSubscription;

			virtual ~DisplacedGeometry();

			//! Cascading bake: realizes base, tessellates/refines, displaces, builds m_pMesh.
			void BuildMesh() const;
			void DestroyMesh() const;

			//! True iff a height source is bound and disp_scale != 0.
			bool WillDisplaceVertices() const;

			//! Tessellates base and refines smooth indexed-mesh bases to spacing (bbox_diag / m_detail)
			//! when vertices will displace (preserving authored grids/caps, periodicity, polar vertices).
			//! Populates undisplaced `shading0`/`geom0` for normal transport, `uvFlags`, and `weldDomains`.
			bool TessellateAndRefine(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords,
				const bool             bLog,
				NormalsListType&       shading0,
				NormalsListType&       geom0,
				std::vector<unsigned char>& uvFlags,
				std::vector<unsigned int>&  weldDomains ) const;

			//! Watertight crease displacement welding for smooth-normal indexed-mesh bases:
			//! averages displacement vectors of coincident base vertices per base policy
			//! (all groups for lathe/sweep/skin; <120-deg cone for imports; per-instance domains
			//! for path_instances; -1 = no weld) while keeping separate shading normals.
			int  BaseWeldPolicy() const;
			void WeldCoincidentDisplacement( const VerticesListType& undisplaced, const NormalsListType& baseNormals, const std::vector<unsigned int>& domains, const int policy, VerticesListType& displaced ) const;

			//! FNV-1a topology signature of m_pMesh; mismatch forces full rebuild instead of refit.
			mutable std::uint64_t                    m_topologySignature;

			//! Applies the bound height route to vertices; returns true iff positions moved.
			bool ApplyHeightField(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords ) const;

			//! Single-threaded observer refit: updates m_pMesh vertices in place if topology matches,
			//! otherwise falls back to DestroyMesh() + BuildMesh().
			void RefreshMeshVertices();

		public:
			DisplacedGeometry(
				IGeometry*            pBase,
				const unsigned int    detail,
				const IFunction2D*    displacement,
				const Scalar          disp_scale,
				const bool            bDoubleSided,
				const bool            bUseFaceNormals,
				const bool            bSeamFold = true,	//!< FALSE for open Cartesian displacement fields (no UV mirror)
				const IScalarPainter* pHeight = 0 );	//!< route (b); pass at most one of `displacement` / `pHeight`

			DisplacedGeometry( const DisplacedGeometry& ) = delete;
			DisplacedGeometry& operator=( const DisplacedGeometry& ) = delete;

			//! Recipe validity (known at construction without realizing).
			bool IsValid() const { return m_pBase != 0 && m_pBase->CanTessellate(); }

			//! True when the baked mesh shades with computed face normals.
			bool UsesFaceNormals() const { return m_bUseFaceNormals; }

			//! Direct access to base geometry.
			const IGeometry* GetBaseGeometry() const { return m_pBase; }

			//! Idempotent deferred-realization entry point; bakes m_pMesh once.
			void Realize() const override;

			bool CanTessellate() const override { return m_pBase != 0 && m_pBase->CanTessellate(); }

            bool HasUncertainSMSNormalOrientation() const;
            //! Neither certified closed nor certified open (uncertified transmissive surfaces keep IOR-stack rule).
            bool HasUncertifiedOpenness(bool allowGeneralProof=true) const;
            bool IsCertifiedWatertight() const;
            bool IsProvablyOpenSheet(bool allowGeneralProof=true) const;
            //! Topological openness (see GeometricUtilities::TriangleMeshOpenness).
            int TopologicalOpenness() const;
            bool HasLegacyOpenSheetProof() const;
            bool HasNegativeOpenSignedVolume() const;
			static unsigned int GetBuildMeshCount();
			static void         ResetBuildMeshCount();

			//! Process-wide count of TessellateToMeshEx full-mesh copies (diagnostic).
			static unsigned int GetTessellateCopyCount();
			static void         ResetTessellateCopyCount();

			//! Read-only view of the baked mesh (null before Realize() or after failed bake); call between renders.
			const ITriangleMeshGeometryIndexed* GetBakedMesh() const { return m_pMesh; }

			// IGeometry
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			//! TessellateToMesh plus per-vertex UV periodicity bits and weld domains for nested displacement.
			bool TessellateToMeshEx( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail, std::vector<unsigned char>* pUVPeriodic, std::vector<unsigned int>* pWeldDomains = 0 ) const;
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest() const override { return true; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;
			bool IsDoubleSided() const override { return m_bDoubleSided; }

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! Calls Realize() and delegates to the baked mesh (sheet semantics).
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! Calls Realize() and delegates to the baked mesh (valid for certified-watertight bakes).
			bool SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
				Scalar& outSigned, bool& outExact ) const override;

			//! Delegates to the baked mesh when realized, else base default.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				return m_pMesh
					? m_pMesh->SelfHitRootFloor( localOrigin, localDir, localNormal )
					: IGeometry::SelfHitRootFloor( localOrigin, localDir, localNormal );
			}

			//! Smoothing-aware analytical derivatives for SMS two-stage solver: composes base
			//! derivatives with the displacement/height field via chain rule, scaling disp_scale
			//! by (1 - smoothing) (smoothing=1 yields base curvature; smoothing=0 yields displaced).
			bool ComputeAnalyticalDerivatives(
				const Point2& uv,
				Scalar        smoothing,
				Point3&       outPosition,
				Vector3&      outNormal,
				Vector3&      outDpdu,
				Vector3&      outDpdv,
				Vector3&      outDndu,
				Vector3&      outDndv
				) const override;

			// Keyframable — static after construction; no keyframeable parameters.
			IKeyframeParameter* KeyframeFromParameters( const String& /*name*/, const String& /*value*/ ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& /*val*/ ) override {}
			void RegenerateData() override {}
			unsigned long long ShapeRevision() const override;
		};
	}
}

#endif
