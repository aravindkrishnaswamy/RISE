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
	// the perturbed normal, not the original geometric one.
	ri.onb.CreateFromW( ri.vNormal );
}
