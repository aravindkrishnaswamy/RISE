//////////////////////////////////////////////////////////////////////
//
//  Object.h - Defines a rasterizable object within our scene.  This
//  Simple object class has only one geometry object, with a 
//  material.  this object is also transformable
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBJECT_
#define OBJECT_

#include "../Interfaces/IObjectPriv.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Utilities/Transformable.h"
#include "../Utilities/RString.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Object : public virtual IObjectPriv, public virtual Transformable, public virtual Reference
		{
		protected:
			const IGeometry*								pGeometry;
			const IUVGenerator*								pUVGenerator;
			const IMaterial*								pMaterial;
			const IRayIntersectionModifier*					pModifier;
			const IShader*									pShader;
			const IRadianceMap*								pRadianceMap;
			const IMedium*									pInteriorMedium;

			bool											bIsWorldVisible;
			bool											bCastsShadows;
			bool											bReceivesShadows;

			Scalar											SURFACE_INTERSEC_ERROR;

			// Transpose of the inverse matrix, used for normal transformations
			// We do( M^-1)^T instead of M*n because this will work with shears and reflections
			Matrix4											m_mxInvTranspose;

			//! Sign of the tangent-frame chirality flip introduced by
			//! the object transform.  +1 for orientation-preserving
			//! transforms (rotation, translation, positive non-uniform
			//! scale); -1 for orientation-reversing transforms (e.g.
			//! `scale -1 1 1`, mirrored instances).  Computed from the
			//! sign of `det(m_mxFinalTrans)` in FinalizeTransformations()
			//! and applied to `ri.geometric.bitangentSign` at hit time
			//! so tangent-space normal maps render correctly on
			//! mirrored object instances of the same source mesh.
			Scalar											m_tangentFrameSign;

			virtual ~Object( );

			//! Copies this object's mutable snapshot state into `dst` (a
			//! freshly-constructed clone): the cloned material leaf, the
			//! addref-shared immutable leaves, the value flags, and the full
			//! transform state.  Does NOT touch geometry (set by the
			//! subclass ctor) or CSG operands (set by CSGObject).  Shared by
			//! Object::CloneSnapshot and CSGObject::CloneSnapshot.
			void CopySnapshotStateInto( Object& dst ) const;

		public:
			Object( );
			Object( const IGeometry* pGeometry_ );

			virtual IObjectPriv* CloneFull() override;
			virtual IObjectPriv* CloneGeometric() override;

			//! feature/gui-snapshot-prototype: deep-copy the MUTABLE state
			//! of this object into a fresh Object that is INDEPENDENT of
			//! later live mutation.
			//!
			//! VIRTUAL (not a new IObjectPriv interface virtual — this is a
			//! method on the internal concrete Implementation::Object, added
			//! at the end of Object's own vtable, so it carries no public /
			//! abstract-interface ABI risk).  It MUST be virtual so a
			//! CSGObject is snapshot-cloned AS a CSGObject (operands +
			//! operation preserved) rather than sliced to a plain Object by
			//! the base implementation — see CSGObject::CloneSnapshot.
			//!
			//! What is COPIED (so a later TranslateObject + finalize on
			//! the live object does NOT change the clone):
			//!   - every transform building block (position / orientation
			//!     / scale / stretch matrices + the whole transform stack)
			//!   - the finalized matrices (m_mxFinalTrans /
			//!     m_mxInvFinalTrans / m_mxInvTranspose / sign)
			//! What is CLONED to an INDEPENDENT instance (mutable LEAF —
			//! the editor rebinds its painter slots in place):
			//!   - material (via CloneMaterialForSnapshot; sub-painters
			//!     addref-shared)
			//! What is SHARED via addref (immutable / non-property-edited
			//! leaves):
			//!   - geometry, modifier, shader, radiance map, interior
			//!     medium, UV generator (see SnapshotLeafClone.h for why
			//!     shader / medium are addref-shared in increment A and the
			//!     residual deferred to increment B).
			//! Plus the cheap value flags (visibility / shadows / eps).
			//!
			//! CloneFull() is unsuitable for a snapshot: it re-runs the
			//! Assign* setters but copies NONE of the transform state, so
			//! a CloneFull'd object starts at identity and would not
			//! reflect the live object's pose at snapshot time.
			virtual Object* CloneSnapshot() const;

			virtual bool AssignMaterial( const IMaterial& pMat ) override;
			virtual bool AssignGeometry( const IGeometry& pGeom ) override;
			virtual bool AssignModifier( const IRayIntersectionModifier& pMod ) override;
			virtual bool AssignShader( const IShader& pShader ) override;
			virtual bool AssignRadianceMap( const IRadianceMap& pRadianceMap ) override;
			virtual bool AssignInteriorMedium( const IMedium& medium ) override;
			virtual void ClearInteriorMedium() override;
			virtual void ClearShader() override;
			virtual void ClearMaterial() override;

			virtual void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			virtual bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			virtual bool IsWorldVisible() const override { return bIsWorldVisible; }
			virtual void SetWorldVisible( bool b ) override { bIsWorldVisible = b; }

			virtual bool DoesCastShadows() const override { return bCastsShadows; }
			virtual bool DoesReceiveShadows() const override { return bReceivesShadows; }

			virtual void SetSurfaceIntersecError( Scalar d ) override { SURFACE_INTERSEC_ERROR = d; }
			virtual bool SetUVGenerator( const IUVGenerator& pUVG ) override;
			virtual void SetShadowParams( const bool bCasts, const bool bReceives ) override;

			virtual const IMaterial* GetMaterial() const override;
			virtual const IShader*   GetShader() const override { return pShader; }
			virtual const IGeometry* GetGeometry() const override { return pGeometry; }

			//! Deferred-realization (IObject): realize our geometry's lazy build
			//! work.  No-op for cheap geometries; bakes a deferred DisplacedGeometry.
			virtual void Realize() const override;
			virtual const IMedium* GetInteriorMedium() const override;

			virtual bool ComputeAnalyticalDerivatives(
				const Point2& uv,
				Scalar        smoothing,
				Point3&       outWorldPosition,
				Vector3&      outWorldNormal,
				Vector3&      outWorldDpdu,
				Vector3&      outWorldDpdv,
				Vector3&      outWorldDndu,
				Vector3&      outWorldDndv
				) const override;
			virtual void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			virtual Scalar GetArea( ) const override;

			virtual const BoundingBox getBoundingBox() const override;

			virtual void ResetRuntimeData() const override;

			void FinalizeTransformations( ) override;
		};
	}
}

#endif
