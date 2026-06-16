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
#include <atomic>
#include <mutex>

namespace RISE
{
	namespace Implementation
	{
		// Wraps any IGeometry (including another DisplacedGeometry), tessellates it via the
		// base's TessellateToMesh, and displaces every vertex along its normal by
		// `displacement(u,v) * disp_scale`.  The resulting mesh is built into an internal
		// TriangleMeshGeometryIndexed and used for all intersection / sampling queries.
		//
		// DEFERRED REALIZATION (2026-06-13): the tessellate + displace + mesh-build work
		// (BuildMesh) is NOT done in the constructor.  It is deferred to Realize(), which the
		// render pipeline calls once, single-threaded, from RayCaster::AttachScene BEFORE the
		// parallel rasterize.  This means a displaced geometry that is never bound to a
		// rendered object is never baked (e.g. the GuillocheWatch dial has 6 displaced dials
		// but only 1 is active — only that 1 bakes).  Direct (non-pipeline) consumers — unit
		// tests, tools — MUST call Realize() after construction before using the geometry; it
		// is single-threaded-safe to do so.
		//
		// If the base geometry's TessellateToMesh returns false (e.g. InfinitePlaneGeometry),
		// Realize() logs an error and leaves the internal mesh null; all IGeometry query
		// methods then degrade to "miss" / empty behavior (the release-mode guard-and-fail).
		// IsValid() reports RECIPE validity (base non-null), known at construction.
		class DisplacedGeometry : public Geometry
		{
		protected:
			IGeometry*                       m_pBase;
			const IFunction2D*               m_pDisplacement;
			Scalar                           m_dispScale;
			unsigned int                     m_detail;
			bool                             m_bDoubleSided;
			bool                             m_bUseFaceNormals;
			bool                             m_bSeamFold;	//!< tent-fold the UV before evaluating displacement (closed wrap-seam surfaces); FALSE = raw UV (open Cartesian fields)

			// The baked mesh and the realized flag are the deferred-realization
			// state.  They are `mutable` because Realize() is a const method
			// (it materializes a build-time cache that is a pure function of
			// this geometry's recipe — base + displacement + scale + detail —
			// and does not change the observable surface; same legitimate
			// `mutable` lazy-cache pattern as ObjectManager's mutable pBVH).
			// m_pMesh is the SOLE OWNER of the mesh (freed in DestroyMesh + the
			// dtor).  Realization is single-threaded (the freeze guard asserts
			// this in debug), so the unlocked mutable write is safe.
			mutable ITriangleMeshGeometryIndexed*    m_pMesh;
			mutable std::atomic<bool>                m_bRealized;
			// Serializes the actual bake so a GUI viewport render's AttachScene
			// cannot race a UI-thread PrepareForRendering/picking into a double
			// BuildMesh() of the same instance.  Uncontended in normal use; the
			// hot path (IntersectRay) never takes it (reads the post-bake m_pMesh).
			mutable std::mutex                       m_realizeMutex;

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
			// `const` (mutates only the mutable mesh members) so it can be
			// invoked from the const Realize() path.  CASCADES: realizes the
			// base before tessellating it (a displaced base must be baked
			// before TessellateToMesh re-emits its mesh).
			void BuildMesh() const;
			void DestroyMesh() const;

			// Tier 1 §3 animation refit path: re-tessellate base + re-apply
			// displacement, then call m_pMesh->UpdateVertices() to swap
			// vertex/normal storage in place and refit the BVH instead of
			// rebuilding.  Falls back to DestroyMesh+BuildMesh if there
			// is no current mesh or topology has changed (which is not
			// currently possible — m_detail is fixed at construction).
			// Single-threaded editor / observer path only.
			void RefreshMeshVertices();

		public:
			DisplacedGeometry(
				IGeometry*          pBase,
				const unsigned int  detail,
				const IFunction2D*  displacement,
				const Scalar        disp_scale,
				const bool          bDoubleSided,
				const bool          bUseFaceNormals,
				const bool          bSeamFold = true );	//!< FALSE for open Cartesian displacement fields (no UV mirror)

			DisplacedGeometry( const DisplacedGeometry& ) = delete;
			DisplacedGeometry& operator=( const DisplacedGeometry& ) = delete;

			// True iff the RECIPE is valid (base geometry is non-null), known
			// at construction.  This is now a cheap recipe check, NOT a
			// mesh-presence check — the mesh is built lazily by Realize(), so
			// post-construction (pre-realize) the mesh is null but IsValid()
			// is already true.  (A base that can NEVER tessellate, e.g.
			// InfinitePlaneGeometry, makes IsValid() FALSE — refused at parse via
			// the cheap CanTessellate() capability check, not deferred to bake.)
			bool IsValid() const { return m_pBase != 0 && m_pBase->CanTessellate(); }

			// IRealizable (IGeometry): deferred-realization entry point.  Realize()
			// bakes the mesh once (idempotent via the internal m_bRealized flag).  A
			// failed bake (base TessellateToMesh false) leaves m_pMesh null and every
			// query guard-fails to miss/zero, so callers must null-check the mesh.
			void Realize() const override;

			// A displaced geometry tessellates (re-emits its baked mesh) iff its
			// base can — nested displaced-of-non-tessellatable is refused at parse.
			bool CanTessellate() const override { return m_pBase != 0 && m_pBase->CanTessellate(); }

			// Diagnostic: process-wide count of BuildMesh() invocations (every
			// actual tessellate + bake).  Used by DeferredRealizeTest to prove
			// the realize pass bakes only render-reachable displaced
			// geometries (the GuillocheWatch dial: 1 baked, not 6) and skips
			// the unbound ones.  Atomic so the count is well-defined even if a
			// future caller realizes off-thread; in practice realization is
			// single-threaded.  Same lightweight static-counter pattern as
			// TriangleMeshGeometryIndexed's s_nextGeometryId.
			static unsigned int GetBuildMeshCount();
			static void         ResetBuildMeshCount();

			// IGeometry
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest() const override { return true; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! Smoothing-aware analytical query.  Composes the BASE geometry's
			//! analytical derivatives with the displacement painter via the
			//! standard chain rule, scaling `disp_scale` by `(1 - smoothing)`.
			//! At smoothing=1 the displacement contribution vanishes and the
			//! result equals the base's analytical (recursing for nested
			//! displaceds).  At smoothing=0 the result matches the actual
			//! tessellated mesh.  Used by SMS two-stage solver — see
			//! `docs/SMS_TWO_STAGE_SOLVER.md`.
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
		};
	}
}

#endif
