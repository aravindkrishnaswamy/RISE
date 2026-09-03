//////////////////////////////////////////////////////////////////////
//
//  FabricSPF.cpp - Mixture importance sampler for `fabric_material`.
//    See FabricSPF.h for the estimator and why the sample must be
//    repriced against the full mixture rather than the branch.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FabricSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

FabricSPF::FabricSPF( const FabricBRDF& brdf, const ISPF& baseSPF ) :
  pBRDF( &brdf ),
  pBaseSPF( &baseSPF )
{
	pBRDF->addref();
	pBaseSPF->addref();
}

FabricSPF::~FabricSPF()
{
	safe_release( pBRDF );
	safe_release( pBaseSPF );
}

namespace
{
	//! Ray-facing shading frame; identical construction to
	//! SheenSPF::Scatter's FlipW and to FabricBRDF's RayFacingNormal,
	//! so evaluator and sampler share one frame.
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

Scalar FabricSPF::PdfImpl(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	FabricBRDF::FabricParams p;
	pBRDF->ResolveFabric( ri, nm, p );
	const WeaveRotatedRI weave( ri, p.weaveAngle );
	return PdfWithParams( ri, woIn, nm, p, weave.Get(), ior_stack );
}

Scalar FabricSPF::PdfWithParams(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const Scalar nm,
	const FabricBRDF::FabricParams& p,
	const RayIntersectionGeometric& weaveRi,
	const IORStack& ior_stack
	) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n  = onb.w();
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wo = Vector3Ops::Normalize( woIn );

	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	const Scalar cosWo = Vector3Ops::Dot( wo, n );
	if( cosWi <= 0 || cosWo <= 0 ) {
		return 0;
	}
	// The same geometric-horizon rejection the sampler applies, so a
	// direction Scatter can no longer emit carries zero density (MIS
	// consistency -- see SheenSPF::Pdf and CoatedSPF::PdfImpl).
	if( Vector3Ops::Dot( wo, GeomNormal( ri, n ) ) <= 0 ) {
		return 0;
	}

	const Scalar w = FabricBRDF::SheenSelectWeight( p.alpha, p.m, cosWi );

	// Sheen branch: cosine-hemisphere about the ray-facing normal.
	const Scalar qSheen = cosWo * INV_PI;

	// Substrate branch: evaluated against the WEAVE-ROTATED record, the
	// same one ScatterImpl hands the base sampler.
	const Scalar qBase = ( nm < 0 )
		? pBaseSPF->Pdf( weaveRi, wo, ior_stack )
		: pBaseSPF->PdfNM( weaveRi, wo, nm, ior_stack );

	return r_max( Scalar(0), w * qSheen + ( Scalar(1) - w ) * qBase );
}

Scalar FabricSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, Scalar(-1), ior_stack );
}

Scalar FabricSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, nm, ior_stack );
}

void FabricSPF::ScatterImpl(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n     = onb.w();
	const Vector3 geomN = GeomNormal( ri, n );
	const Vector3 wi    = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	if( cosWi <= 0 ) {
		return;
	}

	// ONE resolve and ONE rotated record for the whole call -- see
	// FabricSPF.h's note on PdfWithParams.  Everything below reads these
	// two, so the density the sampler prices against is literally the
	// same object the base sampler drew from.
	FabricBRDF::FabricParams p;
	pBRDF->ResolveFabric( ri, nm, p );
	const WeaveRotatedRI weave( ri, p.weaveAngle );

	// 9.2's selection weight: the sheen lobe's own directional albedo,
	// so the branch probability tracks the energy split.
	const Scalar w = FabricBRDF::SheenSelectWeight( p.alpha, p.m, cosWi );

	const unsigned int before = scattered.Count();
	Vector3 wo;
	ScatteredRay::ScatRayType lobeType = ScatteredRay::eRayDiffuse;
	bool sampled = false;

	if( sampler.Get1D() < w )
	{
		// --- sheen lobe: cosine-hemisphere, exactly as SheenSPF does.
		//     9.4 explains why D-importance sampling is deferred rather
		//     than adopted.
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		wo = GeometricUtilities::CreateDiffuseVector( onb, ptrand );
		// eRayDiffuse is the honest tag for a cosine-sampled, broad,
		// non-delta lobe, and matches what SheenSPF has always reported.
		lobeType = ScatteredRay::eRayDiffuse;
		sampled = true;
	}
	else
	{
		// --- substrate branch: the base SPF chooses the direction (and
		//     the lobe tag, which downstream heuristics read), sampling
		//     in the WEAVE-ROTATED frame.  It writes straight into the
		//     caller's container so any IOR-stack state it attaches is
		//     preserved; its `kray` and `pdf` are OVERWRITTEN below with
		//     the mixture values.
		if( nm < 0 ) {
			pBaseSPF->Scatter( weave.Get(), sampler, scattered, ior_stack );
		} else {
			pBaseSPF->ScatterNM( weave.Get(), sampler, nm, scattered, ior_stack );
		}
		if( scattered.Count() > before ) {
			wo = Vector3Ops::Normalize( scattered[before].ray.Dir() );
			lobeType = scattered[before].type;
			sampled = true;
		}
	}

	if( !sampled ) {
		// Either branch may legitimately produce nothing (a below-
		// horizon cosine draw is impossible, but a substrate whose own
		// gate rejects is not).  It reaches here having added none.
		//
		// UNBIASED FOR HORIZON REJECTION, AND NOT FOR THE SUBSTRATE'S
		// OTHER DROP PATHS.  Worth separating, because the two look
		// identical from here:
		//
		//  * HORIZON.  When the base declines because `wo` fell below
		//    the horizon, its own `Pdf` returns 0 for that direction
		//    too, so `qBase` -- and therefore the mixture `q` this
		//    class reports -- already excludes the mass that was never
		//    realised.  Returning nothing is exactly right.
		//
		//  * EVERYTHING ELSE.  GGXSPF also drops a sampled ray when its
		//    throughput degenerates (a vanishing G1 on the reflection
		//    lobe; a zero multiple-scatter kray), and there its `Pdf`
		//    still reports positive density for that direction.  The
		//    mixture then over-reports `qBase` against what the sampler
		//    can actually produce, and the estimator loses energy.
		//
		// Bare GGX already loses its own contribution on those paths --
		// this is the substrate's defect, not the layer's -- but fabric
		// loses MORE in proportion, because the draw that was discarded
		// would have carried `f_sheen + f_base * scale`, not `f_base`
		// alone.  Fixing it properly means making the substrate's `Pdf`
		// agree with its own drop conditions, which is a change to
		// GGXSPF and needs its own measurement; it cannot be repaired
		// from this side without inventing a density the base does not
		// report.  Rare in practice (both conditions are degenerate-
		// geometry cases), and recorded here so the next reader does not
		// mistake the horizon argument above for covering them.
		return;
	}

	const Scalar cosWo = Vector3Ops::Dot( wo, n );
	const bool   valid = ( cosWo > 0 ) && ( Vector3Ops::Dot( wo, geomN ) > 0 );

	const Scalar q = valid ? PdfWithParams( ri, wo, nm, p, weave.Get(), ior_stack ) : Scalar(0);

	if( !valid || q <= Scalar(1e-12) )
	{
		// Zero the THROUGHPUT of anything the base contributed, so a
		// bare-substrate weight cannot escape as if it were a fabric
		// one.  The base's own `pdf` is deliberately LEFT ALONE: the ray
		// carries no energy either way, and a non-delta ray with
		// pdf == 0 is a live 0/0 hazard in a downstream MIS denominator,
		// where it would turn a zero contribution into a NaN one.
		// (CoatedSPF.cpp:211-231's reasoning, verbatim.)
		for( unsigned int i = before; i < scattered.Count(); ++i ) {
			scattered[i].kray   = RISEPel( 0, 0, 0 );
			scattered[i].krayNM = 0;
		}
		return;
	}

	if( scattered.Count() > before )
	{
		// Substrate branch: rewrite in place.
		for( unsigned int i = before; i < scattered.Count(); ++i )
		{
			ScatteredRay& s = scattered[i];
			const Vector3 sw   = Vector3Ops::Normalize( s.ray.Dir() );
			const Scalar  scos = Vector3Ops::Dot( sw, n );
			const Scalar  sq   = ( i == before ) ? q : PdfWithParams( ri, sw, nm, p, weave.Get(), ior_stack );

			if( scos <= 0 || sq <= Scalar(1e-12) ) {
				// Same reasoning as the block above: kill the
				// throughput, keep the base's density.
				s.kray = RISEPel( 0, 0, 0 );
				s.krayNM = 0;
				continue;
			}

			if( nm < 0 ) {
				s.kray = pBRDF->ValueWithParams( sw, ri, p, weave.Get() ) * ( scos / sq );
			} else {
				s.krayNM = pBRDF->ValueNMWithParams( sw, ri, nm, p, weave.Get() ) * ( scos / sq );
			}
			s.pdf     = sq;
			// Forcing isDelta FALSE is correct only because no
			// allowlisted substrate emits a delta lobe -- Lambertian and
			// Oren-Nayar are pure diffuse, and GGX skips its specular
			// lobe entirely below alphaEff 1e-6 rather than going delta.
			// A future allowlist entry that DID emit one would silently
			// get `f*cos/q` applied to a sample whose density is a Dirac,
			// which is meaningless.  (CoatedSPF.cpp shares this
			// assumption and the same allowlist.)
			s.isDelta = false;
		}
	}
	else
	{
		// Sheen branch: build the ray.
		ScatteredRay sheen;
		sheen.type    = lobeType;
		sheen.isDelta = false;
		sheen.ray.Set( ri.ptIntersection, wo );
		sheen.pdf     = q;
		if( nm < 0 ) {
			sheen.kray = pBRDF->ValueWithParams( wo, ri, p, weave.Get() ) * ( cosWo / q );
		} else {
			sheen.krayNM = pBRDF->ValueNMWithParams( wo, ri, nm, p, weave.Get() ) * ( cosWo / q );
		}
		scattered.AddScatteredRay( sheen );
	}
}

void FabricSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, Scalar(-1), scattered, ior_stack );
}

void FabricSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, nm, scattered, ior_stack );
}
