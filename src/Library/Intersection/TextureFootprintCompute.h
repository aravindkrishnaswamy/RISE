//////////////////////////////////////////////////////////////////////
//
//  TextureFootprintCompute.h - Compute the pixel footprint at a hit
//  point by projecting the incoming ray's screen-space differentials
//  onto the surface tangent plane, and (where the surface carries a
//  UV chart) solving that projection for du/dx, du/dy, dv/dx, dv/dy.
//  Igehy 1999 §4 + the PBRT v4 §10.1.1 closed-form 2x2 solve.
//
//  TWO HALVES, SPLIT AT THE SEAM THE MATH ALREADY HAS
//  (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.2):
//
//    ComputeFootprintVectors( ri, ray )
//      The plane projection.  Needs only the hit point, a normal and
//      the ray's differentials -- NOT dpdu/dpdv.  Writes dpdx, dpdy,
//      worldWidth and sets widthValid.  Runs for EVERY geometry,
//      whether or not it has a UV parameterisation: analytic
//      primitives, SDFs (hence sweeps and skeletons), boxes, disks,
//      planes, patches and hair all get a footprint width from it.
//
//    SolveFootprintUV( ri )
//      The 2x2 solve.  Additionally needs ri.derivatives.valid (a
//      non-degenerate dpdu/dpdv basis), ri.derivatives.texChartValid
//      (the geometry's stated map from its own (u, v) parameters to
//      the (s, t) it stamps into ri.ptCoord) and a successful
//      ComputeFootprintVectors.  Writes dudx..dvdy -- in the TEXCOORD
//      chart -- and sets valid.  A no-op on UV-free geometry, and on
//      geometry that has derivatives but has not stated a chart map,
//      which is exactly the honest answer in both cases -- see the
//      two-flag contract on TextureFootprint.
//
//  ComputeTextureFootprint( ri, ray ) remains as the composite of the
//  two, and is what Object::IntersectRay calls.
//
//  CALL SITE: Object::IntersectRay, immediately after the geometry
//  returns a hit and BEFORE the normal/derivative world promotion --
//  at that instant the ray, the hit range, the normal and
//  derivatives.dpdu/dpdv are all still OBJECT-space, which is the
//  frame this helper assumes.  Object::IntersectRay /
//  CSGObject::IntersectRay promote the result to world afterward.
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
	//! surface tangent plane and store the resulting one-pixel-step
	//! displacement vectors on ri.txFootprint.
	//!
	//! Preconditions:
	//!   - ri.bHit == true
	//!   - ri.vNormal populated, ri.range set
	//!   - ray.hasDifferentials == true (else this is a no-op)
	//!
	//! Postcondition: ri.txFootprint.widthValid == true on success,
	//! with dpdx / dpdy / worldWidth written; left untouched (i.e.
	//! widthValid stays false, worldWidth stays 0) on early-out (no
	//! incoming differentials, or an auxiliary ray parallel to the
	//! tangent plane).
	inline void ComputeFootprintVectors( RayIntersectionGeometric& ri, const Ray& ray )
	{
		if( !ray.hasDifferentials ) {
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
		//
		// IMPORTANT: this helper runs mid-`Object::IntersectRay`,
		// BEFORE that function populates ri.ptIntersection /
		// ri.ptObjIntersec -- those are computed by the Object layer
		// AFTER the geometry returns.  At this point ri.range IS set
		// (the geometry sets it on the accepted hit), so we recover the
		// object-space hit point from the ray and range directly.
		// Reading ri.ptIntersection here would silently use (0, 0, 0)
		// and produce a wildly inflated dpdx -- the bug that caused all
		// glTF-imported textured meshes to render at LOD ~10 (single-
		// pixel mip average) starting with Landing 2 (a78593b,
		// 2026-05-06).  Still load-bearing at the Object-layer call
		// site, for exactly the same reason.
		const Point3 hitPt = ri.ray.PointAtLength( ri.range );
		const Vector3 P0( hitPt.x, hitPt.y, hitPt.z );

		// The SHADING normal, not vGeomNormal: that is what the shipped
		// triangle-mesh path used, and switching would move every mesh
		// pixel.  The plane math is sign-invariant, so a geometry that
		// flips its normal for a back-face hit (ClippedPlaneGeometry) is
		// unaffected.
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

		// Filter width (doc 88 S9): dpdx/dpdy are the offsets from P0 to
		// the surface-plane hit of the +x/+y auxiliary rays, in the
		// FRAME OF `ray` -- i.e. exactly the displacement one pixel step
		// induces at this surface point, in whatever space `ray` is
		// currently expressed.  At the Object-layer call site that frame
		// is OBJECT space, so `worldWidth` below is an OBJECT-space
		// length at the point it is stamped, despite the name --
		// `Object::IntersectRay` / `CSGObject::IntersectRay` promote it
		// to true world units afterward by transforming dpdx/dpdy
		// through their own forward map and re-deriving the mean
		// magnitude (exact for any linear map; see
		// docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md §3.4).  Average the
		// two magnitudes (Apodaca & Gritz-style filterwidth estimate)
		// for a single isotropic scalar; consumed by ExpressionPainter/
		// ExpressionScalarPainter::BuildContext to populate
		// ExprEvalContext::fw, and by ReliefModifier's footprint-aware
		// step -- both AFTER the object-layer promotion, so both see
		// true world units.
		const Scalar lenDpdx = std::sqrt( dpdx.x*dpdx.x + dpdx.y*dpdx.y + dpdx.z*dpdx.z );
		const Scalar lenDpdy = std::sqrt( dpdy.x*dpdy.x + dpdy.y*dpdy.y + dpdy.z*dpdy.z );

		ri.txFootprint.dpdx = dpdx;
		ri.txFootprint.dpdy = dpdy;
		ri.txFootprint.worldWidth = Scalar(0.5) * ( lenDpdx + lenDpdy );
		ri.txFootprint.widthValid = true;
	}

	//! Solve the plane-projected pixel steps for the surface's UV
	//! Jacobian and store it on ri.txFootprint.
	//!
	//! Preconditions:
	//!   - ri.txFootprint.widthValid == true (i.e.
	//!     ComputeFootprintVectors already succeeded on this record)
	//!   - ri.derivatives.valid == true, with dpdu / dpdv populated
	//!   - ri.derivatives.texChartValid == true, i.e. the geometry
	//!     stated how its derivative parameters map to the texture
	//!     coordinate it stamps into ri.ptCoord
	//! Any failing makes this a no-op, which is what upholds the
	//! `valid ⇒ widthValid` invariant and what leaves UV-free geometry
	//! (SDF, box, disk, plane, hair) with an honest width and no
	//! Jacobian.
	//!
	//! TWO CHARTS, AND WHY THE MAP IS LOAD-BEARING.  `dpdu` / `dpdv`
	//! differentiate the GEOMETRY'S OWN parameters — for the analytic
	//! primitives, angles in radians and axial world coordinates, with
	//! the two axes swapped on the cylinder and the torus (see
	//! docs/GEOMETRY_DERIVATIVES.md "Magnitudes and parameter
	//! scaling").  `ri.ptCoord`, which is what a texture is actually
	//! sampled at, is the normalised `[0, 1]^2` chart the matching
	//! `GeometricUtilities::*TextureCoord` produces.  Solving in the
	//! derivative chart and publishing the answer as though it were the
	//! texcoord chart is a pure scale error of 2*pi (sphere azimuth),
	//! pi (sphere polar), the cylinder height, … — i.e. exactly the
	//! kind of error mip LOD's log2 turns into whole levels of blur.
	//! So: solve in the derivative chart, then apply
	//! `ri.derivatives.dsdu…dtdv` to land in the texcoord chart.
	//!
	//! Postcondition: ri.txFootprint.valid == true on success;
	//! left false on early-out or on a singular UV basis.
	inline void SolveFootprintUV( RayIntersectionGeometric& ri )
	{
		if( !ri.txFootprint.widthValid || !ri.derivatives.valid ||
		    !ri.derivatives.texChartValid ) {
			return;
		}

		const Vector3& dpdx = ri.txFootprint.dpdx;
		const Vector3& dpdy = ri.txFootprint.dpdy;
		const Vector3 N = ri.vNormal;

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
			// direction); leave the Jacobian invalid.  The width half
			// stays valid -- it never depended on the UV chart.
			return;
		}
		const Scalar invDet = Scalar( 1 ) / det;

		// The solve, in the geometry's own (u, v) parameter chart.
		const Scalar dPdu_dx = (  dpdv_b * dpdx_a - dpdv_a * dpdx_b ) * invDet;
		const Scalar dPdv_dx = ( -dpdu_b * dpdx_a + dpdu_a * dpdx_b ) * invDet;
		const Scalar dPdu_dy = (  dpdv_b * dpdy_a - dpdv_a * dpdy_b ) * invDet;
		const Scalar dPdv_dy = ( -dpdu_b * dpdy_a + dpdu_a * dpdy_b ) * invDet;

		// Change of chart into the TEXCOORD (s, t) axes `ri.ptCoord`
		// uses, per the geometry's stated map.  Identity for the mesh
		// path, where dpdu already IS d/d(texcoord u).
		const SurfaceDerivativesInfo& d = ri.derivatives;
		ri.txFootprint.dudx = d.dsdu * dPdu_dx + d.dsdv * dPdv_dx;
		ri.txFootprint.dvdx = d.dtdu * dPdu_dx + d.dtdv * dPdv_dx;
		ri.txFootprint.dudy = d.dsdu * dPdu_dy + d.dsdv * dPdv_dy;
		ri.txFootprint.dvdy = d.dtdu * dPdu_dy + d.dtdv * dPdv_dy;

		ri.txFootprint.valid = true;
	}

	//! The composite: plane projection, then the UV solve.  This is
	//! what Object::IntersectRay calls; the UV half self-skips on a
	//! geometry with no usable dpdu/dpdv.
	inline void ComputeTextureFootprint( RayIntersectionGeometric& ri, const Ray& ray )
	{
		ComputeFootprintVectors( ri, ray );
		SolveFootprintUV( ri );
	}
}

#endif
