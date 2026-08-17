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

			//! How many `csg_object` composites are CONSUMING this object as an
			//! operand.  Orthogonal to `bIsWorldVisible`, which stays the authored
			//! / container visibility -- see IObjectPriv::AddConsumer for why this
			//! cannot be that flag.  Not copied by any clone: a clone's own
			//! AssignObjects re-establishes it.
			unsigned int									nConsumedBy;

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

			//! World-area scaling of the transform's linear part,
			//! |det|^(2/3): exact for rotations / reflections / uniform
			//! scales, geometric-mean approximation for non-uniform
			//! scale or shear.  Cached by FinalizeTransformations();
			//! multiplied into GetArea() so pdfPosition = 1/GetArea()
			//! matches the WORLD-space samples UniformRandomPoint
			//! returns.  0 for degenerate (non-invertible) transforms.
			Scalar											m_worldAreaScale;

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
			// Clear an optional slot back to "unset" (matches a full derive of a chunk that
			// omits the slot / sets it "none").  Now on IObjectPriv -- the CST incremental
			// apply clears REMOVED slots through the interface -- added at the END of that
			// vtable, so unlike the mid-vtable insertion the original P1-#9 note avoided, no
			// existing slot shifts (out-of-tree caller offsets unchanged).
			virtual void ClearShader() override;
			virtual void ClearMaterial() override;
			virtual void ClearModifier() override;
			virtual void ClearRadianceMap() override;
			virtual void ClearGeometry() override;

			virtual void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			virtual bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			//! An object CONSUMED by a `csg_object` is never world-visible, whatever
			//! its own flag says: it is a term in someone else's boolean expression.
			//! The two states are kept apart so a composite's teardown restores
			//! exactly what it took, and takes nothing away from a second composite
			//! that is still consuming the same operand.
			virtual bool IsWorldVisible() const override { return bIsWorldVisible && nConsumedBy == 0; }
			virtual void SetWorldVisible( bool b ) override { bIsWorldVisible = b; }

			virtual void AddConsumer() override { ++nConsumedBy; }
			//! The clamp keeps an unbalanced release from wrapping an `unsigned int`
			//! to 4 billion, which would pin the operand invisible forever.  But a
			//! release with nothing to release is ITSELF the failure this count
			//! exists to prevent, one composite earlier: the balance is off, so some
			//! LATER release will drive a still-consumed operand to zero and it will
			//! render as a standalone shape beside the composite that owns it.
			//! Saturating silently turns that into an unexplained extra shape in a
			//! render; out of line so the diagnostic can reach GlobalLog without
			//! this header pulling it in.
			virtual void RemoveConsumer() override;
			virtual bool IsConsumed() const override { return nConsumedBy != 0; }

			virtual bool DoesCastShadows() const override { return bCastsShadows; }
			virtual bool DoesReceiveShadows() const override { return bReceivesShadows; }

			virtual void SetSurfaceIntersecError( Scalar d ) override { SURFACE_INTERSEC_ERROR = d; }
			virtual bool SetUVGenerator( const IUVGenerator& pUVG ) override;
			virtual void SetShadowParams( const bool bCasts, const bool bReceives ) override;

			virtual const IMaterial* GetMaterial() const override;
			virtual const IShader*   GetShader() const override { return pShader; }
			virtual const IGeometry* GetGeometry() const override { return pGeometry; }
			virtual const IRayIntersectionModifier* GetModifier() const override { return pModifier; }
			virtual const IRadianceMap* GetRadianceMap() const override { return pRadianceMap; }

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

			//! Object overrides ONLY the parent-composed overload, never the
			//! no-argument one: Transformable's no-argument form delegates
			//! here, so there is exactly ONE code path that refreshes
			//! m_mxInvTranspose / m_tangentFrameSign / m_worldAreaScale.  An
			//! override of the no-argument form instead would leave those three
			//! caches STALE for every hierarchy-composed finalize -- which is
			//! wrong normals, wrong mirrored-tangent handedness, and wrong
			//! emitter / SSS position PDFs (the world-area Jacobian bug fixed
			//! 2026-08-13).
			void FinalizeTransformations( const Matrix4& parentWorld ) override;
			using Transformable::FinalizeTransformations;   // keep the no-arg overload visible
		};
	}
}

#endif
