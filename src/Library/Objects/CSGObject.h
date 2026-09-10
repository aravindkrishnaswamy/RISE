//////////////////////////////////////////////////////////////////////
//
//  CSGObject.h - A CSG object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CSG_OBJECT_
#define CSG_OBJECT_

#include "Object.h"
#include "../Utilities/RString.h"

namespace RISE
{
	namespace Implementation
	{
		enum CSG_OP
		{
			CSG_UNION			= 0,
			CSG_INTERSECTION	= 1,
			CSG_SUBTRACTION		= 2
		};

		//! WHICH ARM of the landing test admitted a bracket's endpoint
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.6).  An
		//! intersection's or a subtraction's composed field has no
		//! usable zero test -- `{f <= 0}` contains the PHANTOM touching
		//! set where the operands' boundaries merely graze, which no ray
		//! can hit -- so a landing is admitted only when the OPERANDS'
		//! OWN SIGNS prove it lies in the closure of the real solid.
		//! `Strict` proves it interior; `Boundary` admits a landing
		//! exactly ON an operand's surface, and only when THAT operand's
		//! signed distance is EXACT.  Reported through
		//! `DistanceToSurfaceWithArm` so a test can say which one fired
		//! rather than inferring it.
		enum class CsgLandingArm { None = 0, Strict = 1, Boundary = 2 };

		//! Map the wire CSG-op code (0 Union / 1 Intersection / 2 Subtraction; any other -> Union,
		//! matching RISE_API_CreateCSGObject) to the typed enum.  Shared by the create path and the
		//! CST incremental re-point (Job::AddCSGObject -> SetOperation) so the two cannot drift.
		inline CSG_OP CsgOpFromChar( char op )
		{
			switch( op ) {
				case 1:  return CSG_INTERSECTION;
				case 2:  return CSG_SUBTRACTION;
				default: return CSG_UNION;
			}
		}

		class CSGObject : public virtual Object
		{
		protected:
			IObjectPriv*							pObjectA;
			IObjectPriv*							pObjectB;

			CSG_OP									op;

			virtual ~CSGObject( );

		public:

			CSGObject( const CSG_OP& op_ );

			bool AssignObjects( IObjectPriv* objA, IObjectPriv* objB );

			//! Re-set the CSG operation in place (CST incremental re-point).  `op` is a read-only
			//! runtime field (read by getBoundingBox + IntersectRay), so this is a plain field set --
			//! no cached/derived state to rebuild.
			void SetOperation( const CSG_OP& op_ );

			//! The current operands (CST incremental apply: compare against the edited chunk's
			//! obja/objb to DETECT an operand-reference change -- which is refused, since re-binding
			//! un-hides the dropped operand and that is wrong if the operand is shared with another CSG).
			IObjectPriv* GetOperandA() const { return pObjectA; }
			IObjectPriv* GetOperandB() const { return pObjectB; }

			IObjectPriv* CloneFull();
			IObjectPriv* CloneGeometric();

			//! feature/gui-snapshot-prototype: snapshot-clone AS a CSGObject.
			//! The base Object::CloneSnapshot would slice a CSGObject to a
			//! plain Object (operands + operation lost, null geometry).  This
			//! overrides it to snapshot-clone the operation + BOTH operands
			//! (recursively, via each operand's virtual CloneSnapshot) and
			//! then copy the shared mutable state via CopySnapshotStateInto.
			//! NOTE: deliberately NOT marked `override` to match this class's
			//! existing no-`override` style — adding the keyword to one method
			//! wakes -Winconsistent-missing-override on the 7 sibling virtuals.
			Object* CloneSnapshot() const;

			const BoundingBox getBoundingBox() const;
			void IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void ResetRuntimeData() const;

			//! IObject::SelfHitRootFloor -- a CSG composite has no geometry of
			//! its own, so answer with the worst of what the operands that
			//! actually OWN the face at `localOrigin` would answer, each asked in
			//! ITS OWN local frame and the answer brought back into ours.
			//! Ownership is settled with a short two-sided probe through the face
			//! (P2-1; see the .cpp for the derivation and the 7494x sibling
			//! inflation that motivated it); a face no operand claims falls back
			//! to the max over both, since over-stating is the safe direction.
			//! (No `override` -- see the CloneSnapshot note above: this class
			//! deliberately omits the keyword throughout.)
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const;

			// Deferred-realization (IObject): the realize pass enumerates only
			// world-VISIBLE objects, but AssignObjects() hides our two operands,
			// so they are never reached directly.  Cascade into them here so a
			// deferred geometry (e.g. displaced) used as a CSG operand is baked.
			void Realize() const;

			//! IObject::DistanceToSurface -- the cross-object proximity
			//! query for a COMPOSITE (design 5.6).  Until Phase 3 a
			//! composite refused: `Object::DistanceToSurface` forwards to
			//! the geometry and a `CSGObject` has none, so nothing in the
			//! scene could measure its distance to a CSG result.
			//!
			//! A UNION answers `min` over the operands that ANSWER -- the
			//! true distance when both operands' unsigned answers are
			//! exact AND this composite's own transform is a similarity
			//! (the `x sigmaMax` below is a bound, not the distance, under
			//! anisotropy), since every union-boundary point lies on one
			//! operand's boundary.  Nothing consumes an unsigned
			//! exactness, so no flag is carried for it.  An INTERSECTION
			//! or a SUBTRACTION
			//! cannot: the nearest operand-surface point may not be on the
			//! composite's surface at all, so `min` there is a LOWER bound
			//! -- the forbidden direction.  Those two compose the operands'
			//! SIGNED LOWER BOUNDS into a field and run Phase 1's bracket
			//! on it, admitting a landing only under the two arms above.
			//!
			//! (No `override` -- see the CloneSnapshot note above: this
			//! class deliberately omits the keyword throughout.)
			bool DistanceToSurface( const Point3& ptWorld, const Scalar maxDistWorld, Scalar& outDist ) const;

			//! IObject::SignedDistanceLower for a COMPOSITE -- what a PARENT
			//! composite and `interior(r)` both read.  A union exports
			//! `min(f_A, f_B)`; an intersection `max(f_A, f_B)`; a
			//! subtraction `max(f_A, -f_B)`.  NO COMPOSITE CARRIES THE
			//! EXACTNESS FLAG -- a CORRECTION to §5.6, which allows a union
			//! to when both operands do.  `max(a, b)` under-reads near a
			//! seam even over exact operands and its zero set IS the
			//! phantom touching set; and `min(a, b)` is 0 on a union's
			//! INTERIOR seams, wherever two operands ABUT (two boxes
			//! stacked into a cube read 0 all over the shared face).  A
			//! parent's boundary arm landing on either would be admitting a
			//! point that is not in the real solid -- the round-3 phantom
			//! one level up.  See `ComposedSignedLocal`'s union case for
			//! the traced failure and the measured numbers.
			bool SignedDistanceLower( const Point3& ptWorld, const Scalar maxDistWorld,
				Scalar& outSigned, bool& outExact ) const;

			//! IObject::DescribeKind -- "csg union" / "csg intersection" /
			//! "csg subtraction".  A composite reaches the proximity
			//! refusal log as an `IObjectPriv*` with NO geometry, so the
			//! log's old `typeid(*GetGeometry())` named it "(no geometry)"
			//! -- the one kind whose refusals an author most needs named.
			const char* DescribeKind() const;

			//! THE SAME QUERY with the landing arm reported.  Exists
			//! because 8's oblique-station gate asks the test to record
			//! WHICH arm fired rather than infer it from the number, and
			//! because "the (3,1,0.4) operand forces the strict arm" is
			//! not observable any other way.  `DistanceToSurface` is a
			//! one-line forward to it, so there is one code path.
			bool DistanceToSurfaceWithArm( const Point3& ptWorld, const Scalar maxDistWorld,
				Scalar& outDist, CsgLandingArm& outArm ) const;

		protected:
			//! THE DIAGONAL OF THIS COMPOSITE'S OWN LOCAL BOX -- A's box
			//! alone for a subtraction (which can never extend past its
			//! minuend), A united with B otherwise -- with the same
			//! "is this a real, built, finite box" screen
			//! `Geometry::BoundingBoxRootFloor` applies, since an unbuilt
			//! mesh and an infinite plane both report +-RISE_INFINITY.
			//! Answers in THIS composite's LOCAL frame, so
			//! `m_mxFinalTrans` is NOT applied -- unlike `getBoundingBox`,
			//! which answers in the PARENT's.
			//!
			//! COMPUTED LAZILY PER CALL, NEVER CACHED, and that is
			//! deliberate: the operands' boxes move under
			//! `FinalizeTransformations` (every animation frame and
			//! hierarchy re-bake) and under an operand's own incremental
			//! re-point, and the CST re-point path calls `AssignObjects`
			//! BEFORE `SetOperation`, so any cache filled at assignment is
			//! stale in the OVER-READ direction (a stale-large diagonal
			//! inflates the probe step).  `SelfHitRootFloor` pays the same
			//! `getBoundingBox` per call for the same reason.
			//! \return FALSE when no usable box exists.
			bool LocalBoxDiagonal( Scalar& outDiag ) const;

			//! The composed SIGNED field at a point in THIS composite's
			//! LOCAL frame, plus the per-operand values the landing test
			//! needs.  Refuses when either operand refuses the signed
			//! query -- which is what makes an intersection or subtraction
			//! with a SHEET operand (a plane, a disk, an open cylinder, a
			//! mesh) refuse rather than report a chord to a face the
			//! composite removes nothing at.
			bool ComposedSignedLocal( const Point3& ptLocal, const Scalar maxDistLocal,
				Scalar& outF, bool& outExact,
				Scalar& outFA, bool& outExactA, Scalar& outFB, bool& outExactB ) const;

			//! Does `qLocal` lie in the CLOSURE OF THE REAL SOLID, proved
			//! by the operands' own signs?  See `CsgLandingArm`.  Sets
			//! `outOperandRefused` when an operand could not answer, which
			//! aborts the whole query rather than merely rejecting this
			//! landing.
			bool LandingAdmits( const Point3& qLocal, const Scalar maxDistLocal,
				CsgLandingArm& outArm, bool& outOperandRefused ) const;

			//! The bracket itself, entirely in this composite's LOCAL
			//! frame: descend along the composed field's central-difference
			//! gradient, then probe with a doubling step until a landing
			//! the two arms admit, and report the chord from `ptLocal`.
			bool BracketDistanceLocal( const Point3& ptLocal, const Scalar maxDistLocal,
				Scalar& outDist, CsgLandingArm& outArm ) const;
		};
	}
}

#endif
