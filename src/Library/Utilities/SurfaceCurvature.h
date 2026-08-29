//////////////////////////////////////////////////////////////////////
//
//  SurfaceCurvature.h - The shape operator (Weingarten map) reduced to
//  a signed MEAN CURVATURE, plus the process-wide consumption gate that
//  decides whether a geometry pays to compute curvature at all.
//
//  Phase 1 of docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md (§5.1, §5.4).
//  Header-only: this is ~15 lines of arithmetic plus an atomic counter,
//  and a .cpp would buy nothing but a fifth build-project entry.
//
//  Two things live here because they are the two halves of ONE feature
//  -- "what is the curvature at this hit" and "is anybody asking":
//
//    RISE::SurfaceCurvature::MeanCurvatureFromDerivatives()
//        the shape operator, from (dpdu, dpdv, dndu, dndv).
//    RISE::SurfaceCurvatureDemand
//        the consumption gate (see its own comment for the mechanism
//        and its documented conservatism).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SURFACE_CURVATURE_
#define SURFACE_CURVATURE_

#include <atomic>
#include <cmath>
#include "Math3D/Math3D.h"
#include "FiniteMath.h"
#include "BoundingBox.h"

namespace RISE
{
	namespace SurfaceCurvature
	{
		//! Signed MEAN curvature H from the first and second fundamental
		//! forms, as carried on `RayIntersectionGeometric::derivatives`:
		//!
		//!     E = dpdu.dpdu   F = dpdu.dpdv   G = dpdv.dpdv
		//!     e = dndu.dpdu   f = (dndu.dpdv + dndv.dpdu)/2   g = dndv.dpdv
		//!     H = (e*G - 2*f*F + g*E) / (2*(E*G - F*F))
		//!
		//! SIGN CONVENTION -- pinned by tests/SurfaceCurvatureTest.cpp, not
		//! by this comment.  The classical second fundamental form is
		//! `II = -dn.dp`; this uses the NEGATED convention (`e = +dndu.dpdu`)
		//! so an OUTWARD-normal sphere of radius r reports H = +1/r:
		//!
		//!     positive = convex, negative = concave, zero = flat
		//!
		//! which makes `clamp(curv,0,1)` an edge-wear mask and
		//! `clamp(-curv,0,1)` a crevice mask, with no remapping in between.
		//!
		//! ORIENTATION.  This function takes NO normal, and that is
		//! deliberate.  H's sign is fixed entirely by the orientation of the
		//! normal FIELD that `dndu`/`dndv` differentiate, and RISE keeps that
		//! field sign-paired with the GEOMETRIC normal at every site that
		//! negates one (the 2026-08-29 sign-pairing family: CSG_SUBTRACTION's
		//! three cavity-wall branches and both double-sided-mesh back-face
		//! flips negate `dndu`/`dndv` alongside `vNormal`; see
		//! docs/GEOMETRY_DERIVATIVES.md "World-space transform").  Modifiers
		//! (bump map, normal map) assign into `ri.vNormal` ONLY and never
		//! touch the derivatives, so the result is bump-INVARIANT by
		//! construction -- which is the §14-item-6 requirement, reached
		//! without an explicit `vGeomNormal` orientation check.  Do NOT
		//! "improve" this by orienting off `ri.vNormal`: that would make a
		//! wear mask flip sign wherever a normal map tilts past the geometric
		//! tangent plane, i.e. a texture artifact masquerading as geometry.
		//!
		//! DEGENERACY.  A pinched parameterization (a pole, a zero-area UV
		//! triangle, a collapsed tangent frame) drives `E*G - F*F` to zero.
		//! The guard is RELATIVE, not a bare NEARZERO compare: `E*G` carries
		//! units of length^4, so an absolute 1e-12 floor is a scale-dependent
		//! lie -- a legitimately tiny object would be rejected and a
		//! legitimately huge one would sail through a genuinely singular
		//! frame.  Requiring the determinant to hold a fixed FRACTION of
		//! `E*G` is scale-free (it is exactly "F is not almost +/-sqrt(E*G)",
		//! i.e. dpdu and dpdv are not almost parallel).  Returns false on
		//! degeneracy and on a non-finite result; `outH` is untouched.  Never
		//! returns a NaN.
		//!
		//! \return TRUE and writes outH when the shape operator is
		//!         well-defined; FALSE (outH untouched) otherwise.
		inline bool MeanCurvatureFromDerivatives(
			const Vector3& dpdu,
			const Vector3& dpdv,
			const Vector3& dndu,
			const Vector3& dndv,
			Scalar& outH )
		{
			const Scalar E = Vector3Ops::Dot( dpdu, dpdu );
			const Scalar F = Vector3Ops::Dot( dpdu, dpdv );
			const Scalar G = Vector3Ops::Dot( dpdv, dpdv );

			const Scalar det = E * G - F * F;

			// Relative degeneracy gate -- see the DEGENERACY note above.
			// `E*G` is non-negative by construction (both are squared
			// magnitudes), so the second test also subsumes "either tangent
			// collapsed to zero length": E*G == 0 makes the right-hand side 0
			// and `det > 0` then fails too (det = -F*F <= 0 there).
			static const Scalar kRelDegenerate = Scalar( 1e-12 );
			if( !( det > Scalar( 0 ) ) || !( det > kRelDegenerate * E * G ) ) {
				return false;
			}

			const Scalar e = Vector3Ops::Dot( dndu, dpdu );
			const Scalar f = Scalar( 0.5 ) * ( Vector3Ops::Dot( dndu, dpdv ) + Vector3Ops::Dot( dndv, dpdu ) );
			const Scalar g = Vector3Ops::Dot( dndv, dpdv );

			const Scalar H = ( e * G - Scalar( 2 ) * f * F + g * E ) / ( Scalar( 2 ) * det );

			// The build pairs -ffast-math with -fno-finite-math-only on every
			// platform that enables it (CLAUDE.md, 2026-07-29), so this test
			// is real; IsFiniteDouble is the uniform in-tree helper.
			if( !RISE::IsFiniteDouble( static_cast<double>( H ) ) ) {
				return false;
			}

			outH = H;
			return true;
		}

		//! THE `scaleHint` CONVENTION, in one place: the characteristic
		//! length of a geometry is its BOUNDING-BOX DIAGONAL.  Every family
		//! that stamps `SurfaceDerivativesInfo::scaleHint` goes through here
		//! (or through an equivalent value it already caches, e.g.
		//! SDFGeometry's `m_diagonal`, which is exactly this quantity) so the
		//! dimensionless `curv` means the same thing across the geometry zoo
		//! rather than each primitive picking its own idea of "size".
		//!
		//! UNBOUNDED and DEGENERATE boxes fall back to 1.0 -- the same
		//! "this geometry did not say" default `SurfaceDerivativesInfo`
		//! carries, which makes `curv` collapse to the raw `curvR` instead of
		//! to 0 or to a garbage multiplier.  This is NOT hypothetical: a
		//! default-constructed BoundingBox spans +/-RISE_INFINITY (the
		//! DBL_MAX sentinel), which is what InfinitePlaneGeometry and a
		//! not-yet-built mesh BVH report, and squaring those would overflow.
		inline Scalar ScaleHintFromBoundingBox( const BoundingBox& bb )
		{
			const Vector3 ext = bb.GetExtents();
			if( !RISE::IsFiniteDouble( static_cast<double>( ext.x ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( ext.y ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( ext.z ) ) ) {
				return Scalar( 1 );
			}
			// Reject the DBL_MAX sentinel BEFORE squaring.  Compare each
			// extent, not the diagonal: the squares are what overflow.
			static const Scalar kMaxSaneExtent = Scalar( 1e30 );
			if( std::fabs( ext.x ) > kMaxSaneExtent ||
			    std::fabs( ext.y ) > kMaxSaneExtent ||
			    std::fabs( ext.z ) > kMaxSaneExtent ) {
				return Scalar( 1 );
			}
			const Scalar diag = std::sqrt( ext.x*ext.x + ext.y*ext.y + ext.z*ext.z );
			return ( diag > Scalar( 0 ) ) ? diag : Scalar( 1 );
		}
	}

	//! CONSUMPTION GATE for per-hit curvature computation (design doc
	//! §5.4 bullet 3, "Cost gating").
	//!
	//! WHY.  `curv` / `curvR` are CONTEXT VARIABLES: unlike a lazily-called
	//! builtin, they must be computed BEFORE the painter runs, so something
	//! has to decide up front whether the hit pays.  For a triangle mesh or
	//! an analytic primitive that is free (the derivatives are already on
	//! the record).  For the SDF family it is NOT: mean curvature there is
	//! `div n_hat` by finite difference, three extra `GradientNormal` calls
	//! = ~18 extra `Map()` evaluations per hit, each O(#parts).  That must
	//! cost exactly ZERO on the overwhelming majority of scenes, whose
	//! expressions never mention curvature.
	//!
	//! MECHANISM.  `ExpressionProgram::Builder` already resolves every
	//! identifier at COMPILE time, so a compiled program knows statically
	//! whether its body (or any of its `def` stages) reads `curv`/`curvR` --
	//! see `ExpressionProgram::UsesContextVar`.  The two expression painters
	//! (`ExpressionPainter`, `ExpressionScalarPainter`) register a demand
	//! reference at CONSTRUCTION when their program does, and drop it at
	//! destruction.  Geometry then asks `Any()` at intersection time.
	//!
	//! WHY A PROCESS-WIDE COUNTER RATHER THAN A SCENE FLAG.  A geometry does
	//! not know its scene: `IGeometry::IntersectRay` receives only a
	//! `RayIntersectionGeometric`, which carries no scene / object / material
	//! back-pointer, and RISE deliberately shares one `IGeometry` across
	//! objects and scenes.  Routing a per-scene flag down to the geometry
	//! would need either a new per-cast INPUT field stamped by every caster
	//! (there are many, and a missed one silently disables the feature) or a
	//! painter->material->object->scene aggregation walk that does not exist.
	//! A counter keyed on "does any live expression painter read curvature"
	//! answers the only question the gate actually needs, in one atomic load.
	//!
	//! THE DOCUMENTED CONSERVATISM: a curvature-reading painter ALIVE
	//! ANYWHERE in the process enables SDF curvature for EVERY scene in it
	//! (the GUI / MCP surface holds several scenes at once).  That costs
	//! performance, never correctness -- the computed value is a pure
	//! function of the hit -- and it is the conservative direction: the
	//! failure mode is "computed and unread", not "read and absent".
	//!
	//! THREAD SAFETY.  The counter is `std::atomic<int>` and is mutated ONLY
	//! at painter construction / destruction, i.e. at scene build and
	//! teardown, outside the render phase (docs/ARCHITECTURE.md's
	//! immutability rule). Render threads only ever LOAD it, relaxed -- no
	//! lock, no per-sample synchronization, and no ordering requirement
	//! (a stale read can only mean one hit's worth of extra or missing
	//! curvature at a scene boundary, never a torn value).
	//!
	//! PHASE 2 NOTE: `occlusion()` / `thickness()` are demand-driven
	//! builtins and need no gate of their own; but they DO read
	//! `derivatives.scaleHint` for radius defaulting, which this gate also
	//! controls today.  Widen the predicate (not the mechanism) then.
	namespace SurfaceCurvatureDemand
	{
		//! The single counter.  A function-local static inside an inline
		//! function has exactly one instance across all translation units
		//! (no C++17 inline variable required, matching the rest of the
		//! tree's language level).
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		//! Is any live consumer asking for per-hit curvature?  One relaxed
		//! atomic load; safe to call from every render thread.
		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		//! RAII demand reference.  `active=false` constructs an inert one, so
		//! a painter whose program does not read curvature holds a member of
		//! this type at zero cost and with no branch at the destructor.
		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}
}

#endif
