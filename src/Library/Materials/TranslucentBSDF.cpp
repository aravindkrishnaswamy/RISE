//////////////////////////////////////////////////////////////////////
//
//  TranslucentBSDF.cpp - Implements the translucent BSDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentBSDF.h"
#include "TranslucentSPF.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Implementation::TranslucentSPFDetail;

TranslucentBSDF::TranslucentBSDF( const IPainter& rF, const IPainter& T, const IScalarPainter& exp,
	const IScalarPainter& ext, const IScalarPainter& scat ) :
  pRefFront( &rF ), pTrans( &T ), pExponent( &exp ), pExtinction( &ext ), pScat( &scat )
{
	pRefFront->addref();
	pTrans->addref();
	pExponent->addref();
	pExtinction->addref();
	pScat->addref();
}

TranslucentBSDF::~TranslucentBSDF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExponent );
	safe_release( pExtinction );
	safe_release( pScat );
}

void TranslucentBSDF::SetRefFront( const IPainter& v )         { v.addref(); safe_release( pRefFront );   pRefFront   = &v; }
void TranslucentBSDF::SetTrans( const IPainter& v )            { v.addref(); safe_release( pTrans );      pTrans      = &v; }
void TranslucentBSDF::SetN( const IScalarPainter& v )          { v.addref(); safe_release( pExponent );   pExponent   = &v; }
void TranslucentBSDF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }
void TranslucentBSDF::SetScat( const IScalarPainter& v )       { v.addref(); safe_release( pScat );       pScat       = &v; }

//////////////////////////////////////////////////////////////////////
//  DL-157 (2026-09-18) -- ONE FUNCTION PER SIDE.
//
//  What was here before: a four-way `GetReflectedSide` switch keyed on
//  the SIGNS of `Dot(n, -ray.Dir())` and `Dot(n, vLightIn)`, whose four
//  cells returned `tau * pow(sd,N) * INV_PI` (case 0 -- used for BOTH
//  the entry transmission and the interior exit, two different lobes
//  with two different weights), `ref * INV_PI` (case 1 -- the interior
//  BACKSCATTER, priced with a painter that lobe does not carry at all),
//  the DL-112-corrected entry front reflection (case 2), and zero.
//  Over the SPF's own draws the invariant the two MIS'd techniques share,
//  `E[kray] == E[value*cos/pdf]`, read 6.00 (entry transmission), 6.98
//  (interior exit) and 0.647 (interior backscatter) AT ZERO TILT, on both
//  pipes -- i.e. BDPT/VCM connections and PT's interior-vertex NEE were
//  integrating a different function from the one the sampler draws.
//
//  What is here now: the SAME per-hit lobe set `TranslucentSPF::Scatter`
//  draws from and `TranslucentSPF::Pdf` reports the density of
//  (`TranslucentSPFDetail::BuildLobeSet`), read a third way --
//
//      f(w) = sum_I  kray_I * p_I(w) / |cos(w, n)|
//
//  -- with the cosine cancelled analytically inside `EvalLobe` (every
//  lobe's axis is +n or -n, so `Dot(w, axis) == |cos(w,n)| > 0` on its
//  own support) rather than divided out.  The sum is over at most one
//  cosine lobe and one Phong group, which occupy COMPLEMENTARY
//  half-spaces, so no direction is ever priced twice.
//
//  The cosine convention is the integrators': `LightSampler.cpp`
//  multiplies `brdf.value` by `Dot(vToLight, ri.vNormal)` (its magnitude
//  under `ScattersFullSphere`), and `BDPTUtilities::GeometricTerm` takes
//  `fabs` of both cosines, so `|cos(w, ri.onb.w())|` is the right
//  denominator for both families.
//
//  NOT RECIPROCAL, and that is the model rather than this fix: both
//  Phong lobes' value depends on the EVALUATED direction alone (a
//  `cos^N` re-emission about the normal), so `f(a->b) != f(b->a)` in
//  general.  Every caller therefore has to evaluate at the direction it
//  means -- which is what makes `BuildLobeSet` take the side from the
//  IOR stack rather than from `ri.ray.Dir()`; see its own comment.
//////////////////////////////////////////////////////////////////////

RISEPel TranslucentBSDF::valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri,
	const IORStack* pIORStack ) const
{
	const Vector3 w = Vector3Ops::Normalize( vLightIn );

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pExponent, *pScat,
		ri, pIORStack, false, 0, set );

	RISEPel f( 0, 0, 0 );
	for( int i = 0; i < set.count; i++ ) {
		Scalar pdf = 0, fOverKray = 0;
		if( !EvalLobe( set.lobes[i], w, pdf, fOverKray ) ) continue;
		f = f + set.lobes[i].kray * fOverKray;
	}
	return f;
}

Scalar TranslucentBSDF::valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri,
	const Scalar nm, const IORStack* pIORStack ) const
{
	const Vector3 w = Vector3Ops::Normalize( vLightIn );

	LobeSet set;
	BuildLobeSet( *pRefFront, *pTrans, *pExtinction, *pExponent, *pScat,
		ri, pIORStack, true, nm, set );

	Scalar f = 0;
	for( int i = 0; i < set.count; i++ ) {
		Scalar pdf = 0, fOverKray = 0;
		if( !EvalLobe( set.lobes[i], w, pdf, fOverKray ) ) continue;
		f += set.lobes[i].krayNM * fOverKray;
	}
	return f;
}

RISEPel TranslucentBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	return valueStateful( vLightIn, ri, 0 );
}

Scalar TranslucentBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return valueStatefulNM( vLightIn, ri, nm, 0 );
}

RISEPel TranslucentBSDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Only the reflective lobe returns energy back toward the camera --
	// transmitted energy reaches the OIDN beauty pass via what's behind
	// the surface, not via this BSDF's albedo AOV.
	return pRefFront->GetColor( ri );
}
