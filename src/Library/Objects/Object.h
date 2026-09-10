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

#include <atomic>	// the two one-shot proximity diagnostic latches below
#include <typeinfo>	// DescribeKind names the geometry's own type

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

			//! World-LINEAR scaling of the transform's linear part,
			//! |det|^(1/3) -- the length-measure sibling of
			//! m_worldAreaScale's |det|^(2/3), with the same exactness
			//! story (exact for rotations / reflections / uniform scales,
			//! geometric-mean approximation otherwise) and the same 0 for
			//! a degenerate transform.  Cached by FinalizeTransformations()
			//! and folded into the hit record's
			//! `derivatives.scaleHint` (multiplied: a length) and
			//! `derivatives.curvature` (divided: a 1/length) so the
			//! expression VM's `curv` / `curvR` are per-instance-correct
			//! across two instances of one shared geometry at different
			//! world scales.  See docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
			//! §5.2.
			//!
			//! NOT folded into `txFootprint.worldWidth` (it briefly was,
			//! during the relief-modifier arc).  That promotion now
			//! applies the FULL forward map to `dpdx`/`dpdy` and
			//! re-derives the mean magnitude, which is exact for any
			//! linear map -- non-uniform scale and shear included --
			//! where this geometric mean under-counted a
			//! `scale 4 0.05 4` panel by 4.31x.  See the FRAME note on
			//! `TextureFootprint` and
			//! docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.4.
			Scalar											m_worldLinearScale;

			//! EXTREMAL SINGULAR VALUES of the transform's upper 3x3, as
			//! BOUNDS: `m_sigmaMax` is an upper bound on the largest,
			//! `m_sigmaMin` a lower bound on the smallest.  Cached by
			//! FinalizeTransformations(); both 0 for a degenerate
			//! (non-invertible) transform, which makes `DistanceToSurface`
			//! refuse.
			//!
			//! WHY BOUNDS AND NOT THE VALUES.  A point-to-set distance
			//! under an invertible linear map `M` satisfies
			//! `sigmaMin * d_object <= d_world <= sigmaMax * d_object`, and
			//! `proximity`'s one-sided contract (never over-read contact)
			//! needs BOTH ends: the radius goes IN divided by the smallest
			//! and the answer comes OUT multiplied by the largest.  Getting
			//! the true singular values needs an SVD or an eigen-solve, and
			//! this engine has neither anywhere -- `m_worldLinearScale`'s
			//! `|det|^(1/3)` is the only decomposition in the tree and it
			//! is neither bound.  So:
			//!
			//!   * EXACT when `M^T M = s^2 I` within 1e-9 relative -- a
			//!     rotation, a reflection, a uniform scale, or any
			//!     composition of them.  That is every object in every
			//!     scene the proximity design's acceptance set names, and
			//!     `m_sigmaExact` records that it held.  Both bounds are
			//!     then `s` and the transform costs the query nothing.
			//!   * SOUND BUT LOOSE otherwise: `sigmaMax <= ||M||_F` (since
			//!     `||M||_F^2 = sum of sigma_i^2`) and
			//!     `sigmaMin >= |det| / sigmaMax^2` (since
			//!     `|det| = sigmaMin * s2 * s3 <= sigmaMin * sigmaMax^2`),
			//!     which stays valid when the upper bound is substituted
			//!     for the true `sigmaMax`.  Never unsafe, only wider: the
			//!     query searches a larger object-space radius than it must
			//!     and reports a distance no smaller than the truth.  The
			//!     ratio is logged once per object so a scene that pays for
			//!     it can be seen to.
			Scalar											m_sigmaMax;
			Scalar											m_sigmaMin;
			bool											m_sigmaExact;
			//! One-shot latch for the loose-bound diagnostic, so an
			//! anisotropically-scaled object says so once -- for the life of
			//! the object, not once per query on every render thread AND not
			//! once per FinalizeTransformations() call.  NOT re-armed on a
			//! re-finalize (an animation frame, a hierarchy re-bake, an editor
			//! edit): it used to be, which made a parented or keyframed
			//! anisotropic object log the warning once per frame instead of
			//! once per object, contradicting this comment and the design's
			//! promise (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2) -- see
			//! Object::FinalizeTransformations for the fix.
			//!
			//! ATOMIC, and that is not decoration.  `DistanceToSurface` is
			//! called from every render thread at once, so a plain
			//! `mutable bool` here is an unsynchronised write to shared
			//! state -- a data race by the language's own definition, which
			//! a sanitiser reports and which lets two threads both read
			//! false and both print.  `exchange` makes "was I the first"
			//! one indivisible question, and the fast path stays a single
			//! relaxed load.
			mutable std::atomic<bool>						m_sigmaLooseWarned;

			//! The SAME one-shot latch for the REFUSAL diagnostic: an
			//! object whose geometry family has no closed-form distance
			//! (a CSG composite, a Bezier patch, a RAW mesh, a heightfield
			//! SDF, an SDF whose bracket did not close within budget)
			//! contributes nothing to `proximity` and says so ONCE, which
			//! is what the design's §2 promises and what
			//! IObjectManager::NearestOtherSurface's contract comment
			//! repeats.  Per OBJECT rather than per family, because the
			//! author's question is "which chunk in my scene went quiet",
			//! and per object is the granularity that answers it.
			//!
			//! Latched here but PRINTED by the manager
			//! (`NoteDistanceRefusal` only reports whether this call won
			//! the latch): an Object carries no name -- the manager owns
			//! the name-to-object map -- and a diagnostic that cannot name
			//! the chunk is one an author cannot act on.
			mutable std::atomic<bool>						m_distanceRefusalWarned;

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

			//! IObject::SelfHitRootFloor -- forward to the geometry, which is
			//! the thing that actually owns the gate.  Our local frame IS the
			//! geometry's object space (Object::IntersectRay hands the geometry
			//! exactly this frame, with a unit-normalized local direction), so
			//! the arguments pass straight through.  With no geometry assigned
			//! the base default (the generic floor) is the honest answer.
			virtual Scalar SelfHitRootFloor(
				const Point3&  localOrigin,
				const Vector3& localDir,
				const Vector3& localNormal
				) const override
			{
				return pGeometry
					? pGeometry->SelfHitRootFloor( localOrigin, localDir, localNormal )
					: IObject::SelfHitRootFloor( localOrigin, localDir, localNormal );
			}
			//! IObject::DistanceToSurface -- the transform layer of the
			//! `proximity(r)` query (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
			//! §5.2).  Maps the world point into the geometry's own space,
			//! asks the geometry, and maps the answer back through the
			//! sigma bounds cached by FinalizeTransformations.  Refuses on
			//! a degenerate transform, on a geometry with no closed form,
			//! and on a geometry that refuses.
			virtual bool DistanceToSurface(
				const Point3& ptWorld,
				const Scalar maxDistWorld,
				Scalar& outDist
				) const override;

			//! IObject::SignedDistanceLower -- the transform layer of the
			//! SIGNED query (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).
			//! Same point mapping and same `/sigmaMin` radius conversion as
			//! the unsigned query, but the magnitude comes back multiplied
			//! by `sigmaMin` rather than `sigmaMax` -- the lower bound's
			//! safe direction.  The geometry's exactness flag survives ONLY
			//! under a similarity transform.
			virtual bool SignedDistanceLower(
				const Point3& ptWorld,
				const Scalar maxDistWorld,
				Scalar& outSigned,
				bool& outExact
				) const override;

			//! IObject::DescribeKind -- the geometry's own type name, which
			//! is what the proximity refusal diagnostic used to obtain by
			//! calling `typeid` on it directly.
			virtual const char* DescribeKind() const override
			{
				return pGeometry ? typeid( *pGeometry ).name() : "(no geometry)";
			}

			//! IObject::NoteDistanceRefusal -- wins the one-shot latch
			//! exactly once per object.  See `m_distanceRefusalWarned`.
			virtual bool NoteDistanceRefusal() const override
			{
				// Fast path FIRST, and relaxed: after the single winning
				// call this is one load on a line nobody writes again, which
				// is what keeps a scene full of refusing neighbours from
				// paying a read-modify-write per candidate per hit.
				if( m_distanceRefusalWarned.load( std::memory_order_relaxed ) ) {
					return false;
				}
				return !m_distanceRefusalWarned.exchange( true, std::memory_order_relaxed );
			}

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
