//////////////////////////////////////////////////////////////////////
//
//  TextureFootprintCompute.h - Compute the texture-space footprint
//  at a hit point by projecting the incoming ray's screen-space
//  differentials onto the surface UV plane.  Igehy 1999 §4 + the
//  PBRT v4 §10.1.1 closed-form 2x2 solve for du/dx, du/dy, dv/dx,
//  dv/dy.
//
//  Called by triangle-mesh geometry as the last step of intersection,
//  AFTER ri.vNormal, ri.ptIntersection, ri.derivatives.dpdu and
//  ri.derivatives.dpdv have all been populated.
//
//  Other geometries (sphere, plane, ...) can opt in by populating
//  derivatives + calling this helper.  Geometries that don't are
//  silently a no-op (txFootprint stays valid=false; texture
//  painters fall back to base-level sampling).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 3, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEXTURE_FOOTPRINT_COMPUTE_
#define TEXTURE_FOOTPRINT_COMPUTE_

#include "RayIntersectionGeometric.h"
#include "../Utilities/Math3D/Math3D.h"

#include <cmath>

namespace RISE
{
	//! Project the incoming ray's screen-space differentials onto the
	//! surface UV plane and store the result on ri.txFootprint.
	//!
	//! Preconditions:
	//!   - ri.bHit == true
	//!   - ri.vNormal, ri.ptIntersection populated
	//!   - ri.derivatives.valid == true (caller's responsibility)
	//!   - ray.hasDifferentials == true (else this is a no-op)
	//!
	//! Postcondition: ri.txFootprint.valid == true on success;
	//! .valid == false (default) on early-out (no incoming
	//! differentials, parallel auxiliary, or singular UV basis).
	inline void ComputeTextureFootprint( RayIntersectionGeometric& ri, const Ray& ray )
	{
		if( !ray.hasDifferentials || !ri.derivatives.valid ) {
			return;
		}

		// Auxiliary ray origins / directions (offsets from central).
		const Vector3 rxO = Vector3( ray.origin.x + ray.diffs.rxOrigin.x,
		                             ray.origin.y + ray.diffs.rxOrigin.y,
		                             ray.origin.z + ray.diffs.rxOrigin.z );
		const Vector3 rxD = Vector3( ray.Dir().x + ray.diffs.rxDir.x,
		                             ray.Dir().y + ray.diffs.rxDir.y,
		                             ray.Dir().z + ray.diffs.rxDir.z );
		const Vector3 ryO = Vector3( ray.origin.x + ray.diffs.ryOrigin.x,
		                             ray.origin.y + ray.diffs.ryOrigin.y,
		                             ray.origin.z + ray.diffs.ryOrigin.z );
		const Vector3 ryD = Vector3( ray.Dir().x + ray.diffs.ryDir.x,
		                             ray.Dir().y + ray.diffs.ryDir.y,
		                             ray.Dir().z + ray.diffs.ryDir.z );

		// Distance from auxiliary ray origin to the surface plane,
		// measured along the auxiliary ray direction.
		// Plane: dot(P - P0, N) = 0; substituting P = origin + t·dir:
		//   t = dot(P0 - origin, N) / dot(dir, N)
		const Vector3 P0( ri.ptIntersection.x, ri.ptIntersection.y, ri.ptIntersection.z );
		const Vector3 N  = ri.vNormal;

		const Scalar dxNum = ( P0.x - rxO.x ) * N.x + ( P0.y - rxO.y ) * N.y + ( P0.z - rxO.z ) * N.z;
		const Scalar dxDen = rxD.x * N.x + rxD.y * N.y + rxD.z * N.z;
		const Scalar dyNum = ( P0.x - ryO.x ) * N.x + ( P0.y - ryO.y ) * N.y + ( P0.z - ryO.z ) * N.z;
		const Scalar dyDen = ryD.x * N.x + ryD.y * N.y + ryD.z * N.z;

		// Auxiliary ray parallel to surface plane → footprint is
		// undefined (would require infinite mip), graceful fallback.
		if( std::fabs( dxDen ) < Scalar( 1e-20 ) ||
		    std::fabs( dyDen ) < Scalar( 1e-20 ) ) {
			return;
		}

		const Scalar tx = dxNum / dxDen;
		const Scalar ty = dyNum / dyDen;

		const Vector3 Px( rxO.x + rxD.x * tx, rxO.y + rxD.y * tx, rxO.z + rxD.z * tx );
		const Vector3 Py( ryO.x + ryD.x * ty, ryO.y + ryD.y * ty, ryO.z + ryD.z * ty );

		const Vector3 dpdx( Px.x - P0.x, Px.y - P0.y, Px.z - P0.z );
		const Vector3 dpdy( Py.x - P0.x, Py.y - P0.y, Py.z - P0.z );

		// Solve [dpdu | dpdv] · (du/dx, dv/dx)^T = dpdx for the
		// per-axis UV derivatives.  3 equations × 2 unknowns is
		// overdetermined; PBRT picks the 2 axes with smallest |N|
		// component (largest projected basis area) for stability.
		const Vector3 dpdu = ri.derivatives.dpdu;
		const Vector3 dpdv = ri.derivatives.dpdv;

		const Scalar absNx = std::fabs( N.x );
		const Scalar absNy = std::fabs( N.y );
		const Scalar absNz = std::fabs( N.z );

		// Indices into a 3-element vector via auxiliary array.
		Scalar dpdu_a, dpdu_b, dpdv_a, dpdv_b, dpdx_a, dpdx_b, dpdy_a, dpdy_b;
		if( absNx > absNy && absNx > absNz ) {
			dpdu_a = dpdu.y; dpdu_b = dpdu.z;
			dpdv_a = dpdv.y; dpdv_b = dpdv.z;
			dpdx_a = dpdx.y; dpdx_b = dpdx.z;
			dpdy_a = dpdy.y; dpdy_b = dpdy.z;
		} else if( absNy > absNz ) {
			dpdu_a = dpdu.x; dpdu_b = dpdu.z;
			dpdv_a = dpdv.x; dpdv_b = dpdv.z;
			dpdx_a = dpdx.x; dpdx_b = dpdx.z;
			dpdy_a = dpdy.x; dpdy_b = dpdy.z;
		} else {
			dpdu_a = dpdu.x; dpdu_b = dpdu.y;
			dpdv_a = dpdv.x; dpdv_b = dpdv.y;
			dpdx_a = dpdx.x; dpdx_b = dpdx.y;
			dpdy_a = dpdy.x; dpdy_b = dpdy.y;
		}

		const Scalar det = dpdu_a * dpdv_b - dpdv_a * dpdu_b;
		if( std::fabs( det ) < Scalar( 1e-30 ) ) {
			// Degenerate UV mapping (zero-area in this projection
			// direction); leave footprint invalid.
			return;
		}
		const Scalar invDet = Scalar( 1 ) / det;

		ri.txFootprint.dudx = (  dpdv_b * dpdx_a - dpdv_a * dpdx_b ) * invDet;
		ri.txFootprint.dvdx = ( -dpdu_b * dpdx_a + dpdu_a * dpdx_b ) * invDet;
		ri.txFootprint.dudy = (  dpdv_b * dpdy_a - dpdv_a * dpdy_b ) * invDet;
		ri.txFootprint.dvdy = ( -dpdu_b * dpdy_a + dpdu_a * dpdy_b ) * invDet;
		ri.txFootprint.valid = true;
	}
}

#endif
