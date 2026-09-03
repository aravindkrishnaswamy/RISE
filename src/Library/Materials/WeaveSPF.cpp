//////////////////////////////////////////////////////////////////////
//
//  WeaveSPF.cpp - Four-lobe mixture importance sampler for
//    `weave_material`.  See WeaveSPF.h for the estimator, why the
//    sample must be repriced against the full mixture, and how the
//    surface lobe's fibre-frame density is kept exactly normalised
//    over the HEMISPHERE rather than over the sphere.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WeaveSPF.h"
#include "FibreLobeMath.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/math_utils.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FibreLobeMath;

WeaveSPF::WeaveSPF( const WeaveBRDF& brdf ) :
  pBRDF( &brdf )
{
	pBRDF->addref();
}

WeaveSPF::~WeaveSPF()
{
	safe_release( pBRDF );
}

namespace
{
	//! Ray-facing shading frame; identical construction to
	//! `WeaveBRDF::ResolveWeave`'s, so evaluator and sampler share one
	//! frame.  (`ResolveWeave` reports only the normal it derived; the
	//! sampler needs the whole basis for its cosine branch, and building
	//! it the same way here is what guarantees the two agree.)
	inline RISE::OrthonormalBasis3D RayFacingONB( const RISE::RayIntersectionGeometric& ri )
	{
		RISE::OrthonormalBasis3D onb = ri.onb;
		if( RISE::Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
			onb.FlipW();
		}
		return onb;
	}

	//! Ray-anchored geometric normal for the horizon gate.
	inline RISE::Vector3 GeomNormal( const RISE::RayIntersectionGeometric& ri, const RISE::Vector3& n )
	{
		const RISE::Vector3& raw = ( RISE::Vector3Ops::SquaredModulus( ri.vGeomNormal ) > RISE::Scalar(1e-12) )
			? ri.vGeomNormal : n;
		return ( RISE::Vector3Ops::Dot( raw, ri.ray.Dir() ) < 0 ) ? raw : -raw;
	}
}

Scalar WeaveSPF::PdfWithParams(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const WeaveBRDF::WeaveParams& p )
{
	const Vector3 n    = p.n;
	const Vector3 view = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wi   = Vector3Ops::Normalize( woIn );

	const Scalar cosView = Vector3Ops::Dot( view, n );
	if( cosView <= 0 ) {
		return 0;
	}
	const Scalar cosWi = Vector3Ops::Dot( wi, n );

	const WeaveBRDF::ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar                   cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	if( !p.thin )
	{
		// EXACT P2-A expression, textually unchanged -- see WeaveSPF.h's
		// P2-B section for why `gap(x)` never enters this path at all.
		if( cosWi <= 0 ) {
			return 0;
		}
		// The same geometric-horizon rejection the sampler applies, so a
		// direction Scatter can no longer emit carries zero density (MIS
		// consistency -- SheenSPF::Pdf, CoatedSPF::PdfImpl, FabricSPF::Pdf).
		if( Vector3Ops::Dot( wi, GeomNormal( ri, n ) ) <= 0 ) {
			return 0;
		}

		// The cosine (volume) branch's density is family-independent, but
		// the loop below still adds it PER FAMILY rather than hoisting it,
		// because that is exactly the shape `ScatterImpl`'s selection has:
		// family first, then lobe.  Keeping the two expressions structurally
		// identical is what makes them provably the same mixture.
		const Scalar qCosine = cosWi * INV_PI;

		Scalar q = 0;
		for( int k = 0; k < 2; ++k )
		{
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			const WeaveBRDF::FibreFrame f = WeaveBRDF::MakeFibreFrame( fam[k]->tangent, n );

			// A degenerate frame means the surface branch is unreachable for
			// this family, so its weight must read ZERO here -- and
			// `ScatterImpl` takes the same branch on the same test, which is
			// what keeps the two in step.
			if( !f.valid ) {
				q += cov[k] * qCosine;
				continue;
			}

			const Scalar w = WeaveBRDF::SurfaceSelectWeight( *fam[k], n, view );

			const WeaveBRDF::DirAngles oA = WeaveBRDF::ProjectDir( f, view );
			const WeaveBRDF::DirAngles iA = WeaveBRDF::ProjectDir( f, wi );
			const Scalar qSurf = WeaveBRDF::SurfaceLobePdf( *fam[k], f, oA, iA );

			q += cov[k] * ( w * qSurf + ( Scalar( 1 ) - w ) * qCosine );
		}

		return r_max( Scalar( 0 ), q );
	}

	// ------------------------------------------------------------
	// P2-B: both hemispheres, scaled by the CONTINUUM share
	// `(1 - gap(x))` -- see WeaveSPF.h's P2-B section.  `p.available`
	// IS `1 - gap(x)`: the same field, read once in `ResolveWeave`.
	// ------------------------------------------------------------
	const Scalar contMass = p.available;
	if( !( contMass > 0 ) ) {
		return 0;
	}

	if( cosWi > 0 )
	{
		// REFLECT SIDE.  Same geometric-horizon gate as the `!thin` path;
		// the volume share is split `(1-transmit_k)` reflect /
		// `transmit_k` transmit, so only the reflect share's density
		// lands here.
		if( Vector3Ops::Dot( wi, GeomNormal( ri, n ) ) <= 0 ) {
			return 0;
		}
		const Scalar qCosine = cosWi * INV_PI;

		Scalar q = 0;
		for( int k = 0; k < 2; ++k )
		{
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			const WeaveBRDF::FibreFrame f = WeaveBRDF::MakeFibreFrame( fam[k]->tangent, n );
			const Scalar w = f.valid ? WeaveBRDF::SurfaceSelectWeight( *fam[k], n, view ) : Scalar( 0 );
			Scalar qSurf = 0;
			if( f.valid ) {
				const WeaveBRDF::DirAngles oA = WeaveBRDF::ProjectDir( f, view );
				const WeaveBRDF::DirAngles iA = WeaveBRDF::ProjectDir( f, wi );
				qSurf = WeaveBRDF::SurfaceLobePdf( *fam[k], f, oA, iA );
			}
			const Scalar reflectShare = Scalar( 1 ) - fam[k]->transmit;
			q += cov[k] * ( w * qSurf + ( Scalar( 1 ) - w ) * reflectShare * qCosine );
		}
		return contMass * r_max( Scalar( 0 ), q );
	}

	if( cosWi < 0 )
	{
		// TRANSMIT SIDE (full sphere).  No geometric-horizon gate --
		// this IS the below-horizon transport `ScattersFullSphere()`
		// exists to admit, on HairMaterial's precedent.
		const Scalar qCosine = ( -cosWi ) * INV_PI;

		Scalar q = 0;
		for( int k = 0; k < 2; ++k )
		{
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			const WeaveBRDF::FibreFrame f = WeaveBRDF::MakeFibreFrame( fam[k]->tangent, n );
			const Scalar w = f.valid ? WeaveBRDF::SurfaceSelectWeight( *fam[k], n, view ) : Scalar( 0 );
			q += cov[k] * ( Scalar( 1 ) - w ) * fam[k]->transmit * qCosine;
		}
		return contMass * r_max( Scalar( 0 ), q );
	}

	return 0;
}

Scalar WeaveSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ) const
{
	WeaveBRDF::WeaveParams p;
	pBRDF->ResolveWeave( ri, Scalar( -1 ), p );
	return PdfWithParams( ri, wo, p );
}

Scalar WeaveSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ) const
{
	WeaveBRDF::WeaveParams p;
	pBRDF->ResolveWeave( ri, nm, p );
	return PdfWithParams( ri, wo, p );
}

void WeaveSPF::ScatterImpl(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ) const
{
	const OrthonormalBasis3D onb   = RayFacingONB( ri );
	const Vector3            n     = onb.w();
	const Vector3            geomN = GeomNormal( ri, n );
	const Vector3            view  = Vector3Ops::Normalize( -ri.ray.Dir() );

	if( Vector3Ops::Dot( view, n ) <= 0 ) {
		return;
	}

	// ONE resolve for the whole call -- see WeaveSPF.h's note on
	// PdfWithParams.  Everything below reads this, so the density the
	// sample is priced against is literally the same object that steered
	// the draw.
	WeaveBRDF::WeaveParams p;
	pBRDF->ResolveWeave( ri, nm, p );

	// ---- P2-B: the delta gap lobe, drawn FIRST and ONLY when
	// `p.thin` -- so a `transmission none` material calls
	// `sampler.Get1D()` in the EXACT same order the committed P2-A code
	// did (see WeaveSPF.h's P2-B section for the RNG-stream argument).
	if( p.thin )
	{
		const Scalar gapVal = Scalar( 1 ) - p.available;
		if( gapVal > 0 && sampler.Get1D() < gapVal )
		{
			// Undeviated pass-through: the new ray continues exactly
			// along the ORIGINAL ray's direction (`wi == -view`), the
			// gap having no yarn to deflect it.  Delta convention
			// matches DielectricSPF: `isDelta = true`, `pdf = 1` (a
			// marker, not a density), and `kray = 1` because the lobe's
			// own coefficient (`gap(x)`) and its selection probability
			// are the identical number and cancel exactly.
			ScatteredRay s;
			s.type    = ScatteredRay::eRayRefraction;
			s.isDelta = true;
			s.ray.Set( ri.ptIntersection, ri.ray.Dir() );
			s.pdf     = Scalar( 1 );
			if( nm < 0 ) {
				s.kray = RISEPel( 1, 1, 1 );
			} else {
				s.krayNM = Scalar( 1 );
			}
			scattered.AddScatteredRay( s );
			return;
		}
	}

	// ---- lobe selection, Zhu 2024 5.1's attenuation-proportional pmf,
	//      factored as (family, then lobe).
	const bool pickWarp = ( sampler.Get1D() < p.aWarp );
	const WeaveBRDF::ThreadParams& T = pickWarp ? p.warp : p.weft;

	const WeaveBRDF::FibreFrame f = WeaveBRDF::MakeFibreFrame( T.tangent, n );
	// Same test, same consequence as in `PdfWithParams`: a degenerate
	// frame makes the surface branch unreachable and the weight zero.
	const Scalar w = f.valid ? WeaveBRDF::SurfaceSelectWeight( T, n, view ) : Scalar( 0 );

	Vector3 wi;
	ScatteredRay::ScatRayType lobeType = ScatteredRay::eRayDiffuse;

	if( sampler.Get1D() < w )
	{
		// ---- surface branch: sample in the family's fibre frame.
		const WeaveBRDF::DirAngles oA = WeaveBRDF::ProjectDir( f, view );

		const Scalar u0 = sampler.Get1D();
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();

		// d'Eon's exact longitudinal inverse.  `exp(-2/v)` UNDERFLOWS to
		// 0 for the narrow widths a satin float uses (v ~ 2e-3 gives
		// exp(-1030)), and that is the correct limit rather than a
		// hazard: the expression degenerates to `1 + v log(u)`, which is
		// the v -> 0 form.
		const Scalar v     = T.vSurf;
		const Scalar uSafe = r_max( Scalar( 1e-5 ), u0 );
		const Scalar cosT  = Scalar( 1 ) + v * log( uSafe + ( Scalar( 1 ) - u0 ) * exp( Scalar( -2 ) / v ) );
		const Scalar sinT  = SafeSqrt( Scalar( 1 ) - Sqr( cosT ) );
		const Scalar cosP  = cos( TWO_PI * u1 );

		// Centred on -theta_o, i.e. on the specular cone -- the same
		// sign convention `Mp`'s peak has.
		const Scalar sinThetaI = Clamp( -cosT * oA.sinTheta + sinT * cosP * oA.cosTheta, -1.0, 1.0 );
		const Scalar cosThetaI = SafeSqrt( Scalar( 1 ) - Sqr( sinThetaI ) );

		Scalar lo = 0, hi = 0;
		if( !WeaveBRDF::SurfaceAzimuthInterval( f, sinThetaI, cosThetaI, oA.phi, lo, hi ) ) {
			// This latitude is entirely below the horizon (only reachable
			// under a non-zero tilt).  Emitting nothing is consistent
			// with `Pdf`, which reports zero surface density for every
			// direction at this latitude too.
			return;
		}

		const Scalar phiD = SampleTrimmedLogistic( u2, T.s, lo, hi );
		const Scalar phiI = oA.phi + phiD;

		wi = f.t * sinThetaI + ( f.nk * cos( phiI ) + f.bk * sin( phiI ) ) * cosThetaI;
		wi = Vector3Ops::Normalize( wi );
		// A narrow, non-delta specular-cone lobe: `eRayReflection` is the
		// honest tag for the downstream heuristics that read it, and it
		// is what separates this branch from the cosine one below.
		lobeType = ScatteredRay::eRayReflection;
	}
	else
	{
		// ---- volume branch: cosine-hemisphere about the ray-facing
		//      normal, exactly as FabricSPF's sheen branch does.
		//
		// P2-B: an extra draw, reached ONLY when `p.thin` AND this
		// family's `transmit` is nonzero, retargets a `transmit_k` share
		// of this branch to the TRANSMIT side (the back of the shading
		// normal) instead of the reflect side -- the diffuse
		// transmission lobe.  Skipped entirely otherwise, so it draws no
		// extra random number on a `transmission none` material.
		const bool transmitBranch = ( p.thin && T.transmit > 0 && sampler.Get1D() < T.transmit );

		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		const Vector3 localWi = GeometricUtilities::CreateDiffuseVector( onb, ptrand );
		if( transmitBranch ) {
			wi       = -localWi;
			lobeType = ScatteredRay::eRayTranslucent;
		} else {
			wi       = localWi;
			lobeType = ScatteredRay::eRayDiffuse;
		}
	}

	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	if( lobeType == ScatteredRay::eRayTranslucent )
	{
		// TRANSMIT SIDE: must genuinely land on the back (impossible by
		// construction here -- `-localWi` is always back-facing -- but
		// asserted the same defensive way the reflect branches are).  No
		// geometric-horizon gate: this IS the full-sphere transport.
		if( !( cosWi < 0 ) ) {
			return;
		}
	}
	else if( cosWi <= 0 || Vector3Ops::Dot( wi, geomN ) <= 0 ) {
		// The surface branch's azimuth trimming makes a below-shading-
		// horizon draw impossible, and the cosine branch's is impossible
		// by construction; this catches the GEOMETRIC gate, whose normal
		// can differ from the shading one under a bump/normal map.  `Pdf`
		// returns 0 for exactly these directions, so dropping the sample
		// is what keeps the two consistent.
		return;
	}

	const Scalar q = PdfWithParams( ri, wi, p );
	if( !( q > Scalar( 1e-12 ) ) ) {
		return;
	}

	const Scalar absCosWi = fabs( cosWi );
	ScatteredRay s;
	s.type    = lobeType;
	s.isDelta = false;
	s.ray.Set( ri.ptIntersection, wi );
	s.pdf     = q;
	if( nm < 0 ) {
		s.kray = pBRDF->ValueWithParams( wi, ri, p ) * ( absCosWi / q );
	} else {
		s.krayNM = pBRDF->ValueNMWithParams( wi, ri, nm, p ) * ( absCosWi / q );
	}
	scattered.AddScatteredRay( s );
}

void WeaveSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, Scalar( -1 ), scattered, ior_stack );
}

void WeaveSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, nm, scattered, ior_stack );
}
