//////////////////////////////////////////////////////////////////////
//
//  RayPlaneIntersection.cpp - Implements a ray plane intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 25, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"

namespace RISE
{

	void RayPlaneIntersection( const Ray& ray, HIT& hit, const Vector3& vPlaneNormal )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		const Scalar		Vd = Vector3Ops::Dot( ray.Dir(), vPlaneNormal );

		if( Vd < NEARZERO && Vd > -NEARZERO ) {
			return;
		}

		hit.dRange = Vector3Ops::Dot(vPlaneNormal, Vector3(-ray.origin.x, -ray.origin.y, -ray.origin.z)) / Vd;

		// Scale-relative self-intersection floor -- same mechanism and
		// same floor as RayBilinearPatchIntersection (debt 21) and
		// RaySphereIntersection (see its note): a ray published from a
		// hit on this plane (1e-12 back-off along the incoming ray) that
		// CROSSES the plane has its self root at t = 1e-12 * |cos in| /
		// |cos out|, which the fixed NEARZERO gate lets through whenever
		// the outgoing ray is more grazing than the incoming one.
		// Measured 2026-09-05 (docs/CLOTH_FABRIC_DESIGN.md section 15,
		// debt 25 sibling audit): an infiniteplane_geometry / circular-
		// disk carrying a full-sphere transmissive weave lit from behind
		// read PT 1/354 of BDPT at unit scale -- PT's NEE shadow rays were
		// almost all self-occluded -- while a clippedplane_geometry twin
		// (already on the scale-relative floor) read 1.000.
		//
		// The L1 of the WHOLE origin, not just its perpendicular component.
		// An adversarial review of a8bef210 proposed tightening this to
		// |n . o| on the theory that `dRange = dot(n, -o) / Vd` only carries
		// round-off from the coordinate along the normal, the two in-plane
		// ones cancelling exactly in the dot product.  That reasoning is
		// sound about ROUND-OFF and still gets the wrong answer, because
		// round-off is not the dominant term here: the self root this gate
		// exists to reject comes from Object::IntersectRay's DELIBERATE
		// SURFACE_INTERSEC_ERROR = 1e-12 back-off along the INCOMING ray,
		// which lifts the published point off the plane by 1e-12*|cos in|
		// and therefore sits at t = 1e-12 * |cos in| / |cos out| along an
		// outgoing ray that crosses back -- unbounded as the outgoing ray
		// grazes, and completely independent of where the point is.  The L1
		// term is what buys headroom against that ratio (a point a few units
		// out on the plane gets a floor of ~3e-12 to ~1e-11, covering the
		// cos ratios a render actually produces).  Tightening to |n . o|
		// leaves ~1e-12 and was MEASURED to put tests/PrimitiveSelfHitTest's
		// circular-disk and infinite-plane rows straight back to their
		// pre-a8bef210 BDPT/PT ratio of ~367 (from ~1.00) -- i.e. it undoes
		// the fix.  Keep the L1.
		//
		// The reason the review wanted the tightening -- a plane or disk CSG
		// operand offset several units from its local origin could no longer
		// clear CSGObject's exit-face probe margin -- is real, and is fixed
		// where it belongs: the probe now ASKS the operand for its floor
		// (IGeometry::SelfHitRootFloor) instead of assuming a box-derived
		// band, so it stands off far enough whatever the operand reports.
		const Scalar coordScale = fabs( ray.origin.x ) + fabs( ray.origin.y ) + fabs( ray.origin.z );
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

		if( hit.dRange > tMin ) {
			hit.bHit = true;
			hit.dRange2 = hit.dRange;
		}
	}

	int RayPlaneIntersectionSimple(
									const Vector3& point, 
									const Vector3& vPlaneNormal,
									const Scalar planeD
									)
	{
		const Scalar		Vd = Vector3Ops::Dot( point, vPlaneNormal ) + planeD;

		if( Vd < -NEARZERO ) {
			return -1;
		} else if( Vd > NEARZERO ) {
			return 1;
		}

		return 0;
	}

}

