//////////////////////////////////////////////////////////////////////
//
//  IObject.h - Interface to a rasterizable object in our scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECT_
#define IOBJECT_

#include "IReference.h"
#include "ITransformable.h"
#include "../Utilities/BoundingBox.h"
#include <cmath>			// std::fabs (SelfHitRootFloor's default body)

namespace RISE
{
	class RayIntersection;
	class IMaterial;
	class IMedium;
	class Ray;

	//! An object combines both geometric and material information into a 
	//! meaningful scene element
	/// \sa IGeometry
	/// \sa IMaterial
	class IObject : public virtual IReference, public virtual IBasicTransform
	{
	protected:
		IObject(){};
		virtual ~IObject(){};

	public:
		//! Intersects a ray with the object
		virtual void IntersectRay( 
			RayIntersection& ri,						///< [in/out] Intersection details at point of intersection if there is an intersection
			const Scalar dHowFar,						///< [in] Maximum distance to travel along ray, optimization
			const bool bHitFrontFaces,					///< [in] Should front facing hits be processed?
			const bool bHitBackFaces,					///< [in] Should back facing hits be processed?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! Intersects, but performs intersection test only with object
		/// \return TRUE if there is an intersection, FALSE otherwise
		virtual bool IntersectRay_IntersectionOnly( 
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Is this object visible to the world?
		/// \return TRUE if the object is visible to the world, FALSE otherwise
		virtual bool IsWorldVisible() const = 0;

		//! Does this object cast shadows?
		/// \return TRUE if the object casts shadows, FALSE otherwise
		virtual bool DoesCastShadows() const = 0;

		//! Does this object receive shadows?
		/// \return TRUE if the object receive shadows, FALSE otherwise
		virtual bool DoesReceiveShadows() const = 0;

		//! Retrieves the material associated to this object
		virtual const IMaterial* GetMaterial() const = 0;

		//! Retrieves the underlying geometry, when one is available.
		//! Default impl returns null for ABI safety with out-of-tree
		//! IObject implementers; concrete `Object` overrides to return
		//! its geometry pointer.
		virtual const class IGeometry* GetGeometry() const { return 0; }

		//! World-space wrapper around `IGeometry::ComputeAnalyticalDerivatives`.
		//! Forwards to the underlying geometry in object space and applies the
		//! object's transform: positions by `m_mxFinalTrans`, tangent vectors
		//! by its rotational/scale part, the normal by `m_mxInvTranspose`
		//! (renormalized), and dndu/dndv by the quotient-rule transform of
		//! the renormalized normal field (NOT a plain inverse-transpose --
		//! see docs/GEOMETRY_DERIVATIVES.md "World-space transform").
		//! Default returns false; concrete `Object` overrides.  Used by the
		//! SMS two-stage solver — see `docs/SMS_TWO_STAGE_SOLVER.md`.
		//! Contract: `false` means "this geometry/transform combination
		//! can't answer the query" and the caller MUST fall back (e.g.
		//! ManifoldSolver's smoothing>0 path falls back to single-stage
		//! Newton) -- callers are entitled to treat `true` as full success
		//! and use the output vectors with no further validity check.  This
		//! includes the case where the object's transform is singular along
		//! the world normal direction (no well-defined unit world normal to
		//! differentiate dndu/dndv against): `Object::ComputeAnalyticalDerivatives`
		//! returns `false` there too (P2-1) rather than fabricating a
		//! zero-curvature (flat) Jacobian under a `true` return.
		virtual bool ComputeAnalyticalDerivatives(
			const Point2& uv,
			Scalar        smoothing,
			Point3&       outWorldPosition,
			Vector3&      outWorldNormal,
			Vector3&      outWorldDpdu,
			Vector3&      outWorldDpdv,
			Vector3&      outWorldDndu,
			Vector3&      outWorldDndv
			) const
		{
			(void)uv; (void)smoothing;
			(void)outWorldPosition; (void)outWorldNormal;
			(void)outWorldDpdu; (void)outWorldDpdv;
			(void)outWorldDndu; (void)outWorldDndv;
			return false;
		}

		//! Object-level lift of `IGeometry::SelfHitRootFloor` -- see that
		//! declaration for the full contract.  `localOrigin` / `localDir` /
		//! `localNormal` are in THIS object's own local frame (i.e. already
		//! through `GetFinalInverseTransformMatrix()`, the same frame the
		//! object hands its geometry), `localDir` is unit, and the answer is a
		//! range in those units.
		//!
		//! The lift exists because CSGObject's exit-face payload probe queries
		//! an `IObjectPriv*` operand, which may be a NESTED CSGObject and so has
		//! no single geometry to ask: `CSGObject` overrides this to return the
		//! max over its two operands, each queried in ITS OWN local frame and
		//! the answer mapped back through that child's stretch.  Concrete
		//! `Object` overrides to forward to its geometry.  Default here is the
		//! same generic `NEARZERO * (1 + |localOrigin|_1)` floor as
		//! `IGeometry`'s default, so an out-of-tree IObject implementer that
		//! predates this method still COMPILES and gets that floor.  It is NOT
		//! declared last -- GetShader / GetModifier / GetRadianceMap and more
		//! follow it -- so it makes no vtable-slot claim: this is a
		//! source-compatibility promise only.  (An earlier draft of this comment
		//! asserted both; corrected in the adversarial review of 4b141ad3.)
		virtual Scalar SelfHitRootFloor(
			const Point3&  localOrigin,
			const Vector3& localDir,
			const Vector3& localNormal
			) const
		{
			(void)localDir; (void)localNormal;
			return NEARZERO * ( Scalar(1) +
				std::fabs( localOrigin.x ) + std::fabs( localOrigin.y ) + std::fabs( localOrigin.z ) );
		}

		//! Retrieves the shader associated with this object (or null
		//! if none was assigned).  Default impl returns null for ABI
		//! safety with out-of-tree implementers; concrete `Object`
		//! overrides to return its `pShader` field.  Phase 3 added
		//! this for the right-side panel's editable shader-name row.
		virtual const class IShader* GetShader() const { return 0; }

		//! Retrieves the geometry modifier of this object (NULL if none).  Default
		//! returns null for ABI safety with out-of-tree implementers; concrete `Object`
		//! overrides to return its `pModifier`.  Used by the CST incremental apply to
		//! detect an optional-slot REMOVAL (a `modifier none` value edit), which it then
		//! CLEARS in place post-re-point (workstream #3; 21-stable-apply-and-resolver.md).
		virtual const class IRayIntersectionModifier* GetModifier() const { return 0; }

		//! Retrieves the per-object radiance map of this object (NULL if none).  Default
		//! null for ABI safety; `Object` overrides.  Same incremental-apply removal-
		//! detect-and-clear use as GetModifier.
		virtual const class IRadianceMap* GetRadianceMap() const { return 0; }

		//! Retrieves the interior medium of this object (NULL if vacuum)
		virtual const IMedium* GetInteriorMedium() const = 0;
		
		//! Generates a uniform random point on the object.  Needed to sample luminary geometry surfaces
		//! This function guarantees that for the same prand, the same data is returned
		virtual void UniformRandomPoint( 
			Point3* point,								///< [out] Point on the surface
			Vector3* normal,							///< [out] Normal at the point on the surface
			Point2* coord,								///< [out] Texture co-ordinate at the point on the surface
			const Point3& prand						///< [in] Variables used in point generation
			) const = 0;

		//! Returns the area of the object
		/// \return The area of the object in meters squared
		virtual Scalar GetArea( ) const = 0;

		//! Returns the bounding box of the object
		/// \return The bounding box of the object
		virtual const BoundingBox getBoundingBox() const = 0;

		//! Materialize any deferred build-work this object needs before render.
		//! Called once, single-threaded, from RayCaster::AttachScene's realize
		//! pass BEFORE the parallel rasterize.  That pass enumerates only WORLD-
		//! VISIBLE objects, so a composite (CSGObject) MUST cascade into its
		//! non-enumerated children here.  Default no-op; Object realizes its
		//! geometry.  Declared last + defaulted so the vtable stays ABI-stable.
		virtual void Realize() const {}

		//! SHORTEST DISTANCE from `ptWorld` to this object's surface, in
		//! WORLD units -- the transform-layer half of the `proximity(r)`
		//! query (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).  `Object`
		//! overrides it to map the point into its geometry's own space, ask
		//! `IGeometry::DistanceToSurface`, and map the answer back.
		//!
		//! SAME ONE-SIDED CONTRACT as the geometry-level method: the value
		//! written must be the true distance or an UPPER bound on it, never
		//! a lower one, because over-reading contact is a wrong render
		//! while under-reading it is only an unpainted seam.  That is what
		//! forces the transform layer to convert the radius IN by the
		//! smallest singular value and the answer OUT by the largest, and
		//! to REFUSE outright on a degenerate (non-invertible) transform.
		//!
		//! DEFAULTED to a refusal so an out-of-tree IObject implementer,
		//! and the two IObject stubs in the test tree, compile unchanged
		//! and contribute nothing.  Declared last + defaulted: no vtable
		//! claim on any existing slot.
		//! \return TRUE and writes `outDist` (>= 0), or FALSE with
		//!         `outDist` untouched.
		virtual bool DistanceToSurface(
			const Point3& ptWorld,			///< [in] Query point, WORLD space
			const Scalar maxDistWorld,		///< [in] Search radius, world units; may refuse beyond it
			Scalar& outDist					///< [out] Distance to this object's surface, world units
			) const
		{
			(void)ptWorld; (void)maxDistWorld; (void)outDist;
			return false;
		}

		//! ONE-SHOT LATCH for the refusal diagnostic behind
		//! `DistanceToSurface`.  The design's §2 promises that a neighbour
		//! which cannot answer "says so once in the log"; this is the half
		//! that makes "once" true without a per-candidate lock.
		//!
		//! Split from the printing on purpose.  An object holds the latch
		//! (it is per object, which is the granularity an author can act
		//! on) but carries no NAME -- the object manager owns the
		//! name-to-object map -- so the manager asks this question and does
		//! the printing, naming the chunk and its geometry kind.
		//!
		//! DEFAULTED to `false` -- "I did not win the latch" -- so an
		//! out-of-tree implementer and the test tree's IObject stubs stay
		//! silent rather than printing per candidate per hit.  Declared
		//! last + defaulted: no vtable claim on any existing slot.
		//! \return TRUE exactly once per object, on the first call.
		virtual bool NoteDistanceRefusal() const { return false; }

		//! SIGNED distance LOWER BOUND from `ptWorld` to this object's
		//! surface, in WORLD units -- the transform-layer half of
		//! `IGeometry::SignedDistanceLower`
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).  Read by
		//! `interior(r)` (which consumes only the negative side) and by a
		//! CSG composite's composed field.
		//!
		//! THE TRANSFORM CONVERSION IS THE MIRROR of the unsigned query's
		//! and it is not interchangeable with it: the point goes through
		//! the inverse, the radius in by `/σ_min` as before, but the
		//! magnitude comes back multiplied by `σ_min` rather than
		//! `σ_max` -- `d_w >= σ_min · d_o` is the LOWER bound's safe
		//! direction.  A degenerate transform refuses outright, exactly as
		//! `DistanceToSurface` does.
		//!
		//! `outExact` is forwarded from the geometry ONLY under a
		//! SIMILARITY (`σ_min == σ_max`, the exact fast path): under an
		//! anisotropic transform `×σ_min` is a bound, not the distance, so
		//! the flag must be dropped even for a closed-form family.
		//!
		//! DEFAULTED to a refusal, declared LAST + defaulted: no vtable
		//! claim on any existing slot.
		//! \return TRUE and writes both outputs, or FALSE with
		//!         `outSigned` untouched and `outExact` cleared.
		virtual bool SignedDistanceLower(
			const Point3& ptWorld,			///< [in] Query point, WORLD space
			const Scalar maxDistWorld,		///< [in] Effort budget, world units; NOT a range refusal
			Scalar& outSigned,				///< [out] Signed lower bound (negative inside), world units
			bool& outExact					///< [out] TRUE when the magnitude is the exact distance
			) const
		{
			(void)ptWorld; (void)maxDistWorld; (void)outSigned;
			outExact = false;
			return false;
		}

		//! A SHORT HUMAN NAME for what kind of object this is, for the
		//! proximity refusal diagnostic.  `ObjectManager::LogDistanceRefusal`
		//! used to reach for `typeid(*GetGeometry())`, which answers
		//! "(no geometry)" for a CSG composite -- the one kind whose
		//! refusals an author most needs named.  A composite answers its
		//! OPERATION ("csg subtraction"); an `Object` answers its
		//! geometry's type name.
		//!
		//! Returns a pointer to storage that outlives the call (a literal,
		//! or `typeid::name()`'s static string).  Defaulted so the test
		//! tree's IObject stubs and any out-of-tree implementer compile
		//! unchanged.  Declared last + defaulted: no vtable claim.
		virtual const char* DescribeKind() const { return "(unknown)"; }
	};
}

#include "../Intersection/RayIntersection.h"
#include "IMaterial.h"

#endif
