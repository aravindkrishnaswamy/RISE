//////////////////////////////////////////////////////////////////////
//
//  BumpMap.cpp - Implementation of the BumpMap ray intersection
//  modifier.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BumpMap.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

BumpMap::BumpMap( const IFunction2D& func, const Scalar dScale_, const Scalar dWindow_, const bool bNormalizeGradient_ ) :
  pFunction( func ), dScale( dScale_ ), dWindow( dWindow_), bNormalizeGradient( bNormalizeGradient_ )
{
	pFunction.addref();
}

BumpMap::~BumpMap()
{
	pFunction.release();
}

void BumpMap::Modify( RayIntersectionGeometric& ri ) const
{
	// Central-difference of the height field across the texture window.
	// Expression kept verbatim from the legacy form so a non-normalized bump
	// (bNormalizeGradient == false) is byte-identical to pre-flag behaviour
	// (no FP reassociation of the `*dScale` distribution).
	Scalar bumpU = (pFunction.Evaluate( ri.ptCoord.x + dWindow, ri.ptCoord.y ) * dScale) -
				   (pFunction.Evaluate( ri.ptCoord.x - dWindow, ri.ptCoord.y ) * dScale);

	Scalar bumpV = (pFunction.Evaluate( ri.ptCoord.x , ri.ptCoord.y + dWindow ) * dScale) -
				   (pFunction.Evaluate( ri.ptCoord.x , ri.ptCoord.y - dWindow ) * dScale);

	// `normalize_gradient TRUE`: divide by the central-difference span (2*dWindow)
	// so the result is the actual height-field slope d(f)/d(uv) and `dScale` is a
	// window-INDEPENDENT amplitude.  Without this, the perturbation magnitude is
	// dScale*(f(+w)-f(-w)) ~ dScale*2*dWindow*slope, i.e. amplitude silently
	// couples to the finite-difference window — so the SAME dScale produces an
	// ~2*dWindow-times-weaker bump as the window shrinks (the trap that made a
	// fine-window dial read flat).  Default FALSE preserves every legacy scene
	// (e.g. the Veach-egg Perlin bump tuned to the coupled magnitude) byte-for-byte.
	if( bNormalizeGradient && dWindow > 0 ) {
		const Scalar invSpan = Scalar(1) / ( Scalar(2) * dWindow );
		bumpU *= invSpan;
		bumpV *= invSpan;
	}

	// Perturb the normal using the surface tangent vectors from the ONB,
	// rather than static basis vectors which fail when the normal is
	// aligned with one of them (e.g. horizontal surfaces with N=(0,1,0)).
	const Vector3 vTangentU = ri.onb.u();
	const Vector3 vTangentV = ri.onb.v();

	ri.vNormal = Vector3Ops::Normalize(ri.vNormal + vTangentU * bumpU + vTangentV * bumpV);

	// Also rebuild the ONB so that SPFs (refraction/reflection) use
	// the perturbed normal, not the original geometric one.  When the
	// hit carries ANY geometry-supplied coherent tangent
	// (ri.bHasShadingTangent -- HairGeometry's fiber tangent, SDFGeometry
	// heightfield mode, or (docs/CLOTH_FABRIC_DESIGN.md 9.1) an analytic
	// primitive's dpdu, a UV-mapped mesh's dpdu, or an imported glTF
	// TANGENT; see the field's doc in RayIntersectionGeometric.h),
	// ri.onb.u() at this point already IS that tangent: Object::IntersectRay
	// / CSGObject::IntersectRay promoted it to world space and built the
	// ONB from it before this modifier ran.  A plain CreateFromW would
	// silently discard it for an arbitrary canonical-axis tangent and
	// break the coherent frame HairBSDF / anisotropic `tangent_rotation`
	// depend on -- the same failure mode GlintModifier.cpp avoids for its
	// facet tilt.  Project the CURRENT u onto the new normal's tangent
	// plane and rebuild with CreateFromWU instead; fall back to
	// CreateFromW only if that projection degenerates (the perturbed
	// normal swung onto the old tangent).  When bHasShadingTangent is
	// false, this is skipped entirely and behaviour is byte-identical to
	// before.
	//
	// Handedness (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round, P1 follow-on):
	// CreateFromWU ALWAYS emits a right-handed (u,v,w) triple, but a
	// mirrored-instance hit's incoming `ri.onb` may deliberately be
	// LEFT-handed here -- Object::IntersectRay's own P1 fix flips `v`
	// (OrthonormalBasis3D::FlipV) to correct `tangent_rotation`'s sense
	// under a negative-determinant transform.  Naively rebuilding with
	// CreateFromWU would silently discard that correction for any
	// mirrored, tangent-bearing hit that also carries a bump map.
	// Capture the incoming handedness (sign of u.(v x w)) and restore it
	// after rebuilding, so this modifier composes with the mirror fix
	// instead of undoing it.
	if( ri.bHasShadingTangent ) {
		const Vector3 oldU = ri.onb.u();
		const Scalar oldHandedness = Vector3Ops::Dot( oldU,
			Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );
		const Vector3 uProj = oldU - ri.vNormal * Vector3Ops::Dot( oldU, ri.vNormal );
		if( Vector3Ops::SquaredModulus( uProj ) > Scalar(1e-12) ) {
			ri.onb.CreateFromWU( ri.vNormal, uProj );
			if( oldHandedness < Scalar(0) ) {
				ri.onb.FlipV();
			}
		} else {
			ri.onb.CreateFromW( ri.vNormal );
		}
	} else {
		ri.onb.CreateFromW( ri.vNormal );
	}
}
