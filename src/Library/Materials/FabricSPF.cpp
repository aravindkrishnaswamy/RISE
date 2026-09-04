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
	if( cosWi <= 0 ) {
		return 0;
	}

	if( cosWo <= 0 )
	{
		// R8 P1.1 -- THE TRANSMIT SIDE.  Reached only when the substrate
		// scatters over the full sphere; otherwise this is the committed
		// `cosWo <= 0 -> return 0`, textually and bit-for-bit, and no
		// substrate call is made.
		if( !pBRDF->BaseScattersFullSphere() || !( cosWo < 0 ) ) {
			return 0;
		}

		// NO GEOMETRIC-HORIZON GATE here -- this IS the below-horizon
		// transport, on HairMaterial's / WeaveSPF's precedent.
		//
		// The SHEEN branch contributes NOTHING to this side: it is a
		// cosine hemisphere about the ray-facing normal, so it can never
		// draw a direction below that normal.  The mixture's transmit
		// arm is therefore `(1 - w) * q_base` alone, and that is what
		// makes the whole density integrate to
		// `w * 1 + (1 - w) * (substrate's own continuum share)` over the
		// SPHERE -- `w` less than 1 exactly to the extent the substrate
		// reserved mass for its own delta lobe.  SPFPdfConsistencyTest's
		// full-sphere block asserts that identity.
		const Scalar wSel = FabricBRDF::SheenSelectWeight( p.alpha, p.m, cosWi );
		const Scalar qBaseT = ( nm < 0 )
			? pBaseSPF->Pdf( weaveRi, wo, ior_stack )
			: pBaseSPF->PdfNM( weaveRi, wo, nm, ior_stack );
		return r_max( Scalar(0), ( Scalar(1) - wSel ) * qBaseT );
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

	if( sampler.Get1D() < w )
	{
		// --- sheen lobe: cosine-hemisphere, exactly as SheenSPF does.
		//     9.4 explains why D-importance sampling is deferred rather
		//     than adopted.  REFLECTION-ONLY: a cosine draw about the
		//     ray-facing normal is above that normal by construction, so
		//     this branch is untouched by P1.1.
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		const Vector3 wo = GeometricUtilities::CreateDiffuseVector( onb, ptrand );

		const Scalar cosWo = Vector3Ops::Dot( wo, n );
		if( cosWo <= 0 || Vector3Ops::Dot( wo, geomN ) <= 0 ) {
			// Below the GEOMETRIC horizon (the shading one is impossible
			// by construction).  `Pdf` returns 0 for exactly these
			// directions, so emitting nothing keeps the two consistent.
			return;
		}

		const Scalar q = PdfWithParams( ri, wo, nm, p, weave.Get(), ior_stack );
		if( q <= Scalar(1e-12) ) {
			return;
		}

		ScatteredRay sheen;
		// eRayDiffuse is the honest tag for a cosine-sampled, broad,
		// non-delta lobe, and matches what SheenSPF has always reported.
		sheen.type    = ScatteredRay::eRayDiffuse;
		sheen.isDelta = false;
		sheen.ray.Set( ri.ptIntersection, wo );
		sheen.pdf     = q;
		if( nm < 0 ) {
			sheen.kray = pBRDF->ValueWithParams( wo, ri, p, weave.Get() ) * ( cosWo / q );
		} else {
			sheen.krayNM = pBRDF->ValueNMWithParams( wo, ri, nm, p, weave.Get() ) * ( cosWo / q );
		}
		scattered.AddScatteredRay( sheen );
		return;
	}

	// --- substrate branch: the base SPF chooses the direction (and the
	//     lobe tag, which downstream heuristics read), sampling in the
	//     WEAVE-ROTATED frame.  It writes straight into the caller's
	//     container so any IOR-stack state it attaches is preserved; its
	//     `kray` and `pdf` are OVERWRITTEN below with the mixture values.
	if( nm < 0 ) {
		pBaseSPF->Scatter( weave.Get(), sampler, scattered, ior_stack );
	} else {
		pBaseSPF->ScatterNM( weave.Get(), sampler, nm, scattered, ior_stack );
	}

	if( scattered.Count() == before ) {
		// The substrate declined.  It reaches here having added none.
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

	// Rewrite every ray the base added.  Each is priced independently
	// against the FULL mixture, which is what `PdfWithParams` reports for
	// an arbitrary direction with no memory of how it was drawn.
	//
	// PER-RAY RATHER THAN "IF THE FIRST IS INVALID, ZERO THEM ALL", which
	// is what the committed code did.  Unreachable difference in
	// practice -- every allowlisted substrate emits at most ONE ray per
	// Scatter call (FabricSPF.h's lobe budget) -- and the per-ray form is
	// the correct generalisation, since `PdfWithParams` already applies
	// exactly the gates the old pre-pass computed by hand.
	for( unsigned int i = before; i < scattered.Count(); ++i )
	{
		ScatteredRay& s = scattered[i];
		const Vector3 sw   = Vector3Ops::Normalize( s.ray.Dir() );
		const Scalar  scos = Vector3Ops::Dot( sw, n );

		if( s.isDelta )
		{
			// ---- R8 P1.1: A DELTA LOBE FROM THE SUBSTRATE.
			//
			// Today this is exactly one thing: a `transmission thin`
			// `weave_material`'s gap pass-through (WeaveSPF.cpp), which
			// arrives with `isDelta = true`, `pdf = 1` (RISE's delta
			// MARKER, not a density) and `kray = gap / gap = 1`.  Before
			// P1.1 the wrapper zeroed it -- that was the silent
			// extinction debt 22 records.
			//
			// THE DELTA CONVENTION IS KEPT, NOT CONVERTED.  `isDelta`
			// and `pdf` are left exactly as the substrate set them, so
			// PT / BDPT / VCM keep routing this sample around the
			// density (ISPF.h:194) instead of dividing by a Dirac.  Only
			// `kray` is repriced, and the reprice is the whole content
			// of the fix:
			//
			//   kray_fabric = kray_base * T(a,m,n.v) * T(a,m,|n.wo|)
			//                 -------------------------------------
			//                                (1 - w)
			//
			// with `T = FabricBRDF::SheenTransmit`, the SINGLE-CROSSING
			// arm.  The numerator is the fuzz layer crossed TWICE -- it
			// stands between the light and the gap on the way in AND
			// between the gap and the eye on the way out, exactly as the
			// continuum transmit branch's `BaseScaling` says.  The
			// division by `(1 - w)` is this branch's own selection
			// probability in the wrapper's mixture: the substrate
			// already divided by its OWN `gap`, and the wrapper must
			// divide by the extra factor it introduced or the estimator
			// reads low by precisely that factor.
			//
			// NO `1/(1 - m*EhatMean)` RECYCLING FACTOR HERE, and that is
			// the one place this differs from `BaseScaling`.  That
			// denominator is the sum of the multiple-bounce series
			// between the fuzz and the substrate: energy the fuzz
			// intercepts is not deleted, it is re-scattered DIFFUSELY
			// back onto the substrate and re-emerges.  A delta lobe is a
			// measure-ZERO direction, so diffusely redistributed energy
			// has zero probability of landing back on it -- none of that
			// series reaches this sample, and charging it the
			// denominator would BRIGHTEN an aperture above the light
			// that actually arrived at it (measured: 1.13x at alpha 0.3
			// and a normal view, and it grows with roughness).  With the
			// bare two-arm product the wrapper can only ever ATTENUATE a
			// delta pass-through, which is both the physical statement
			// and the bound LayeredWhiteFurnaceTest row 53 asserts in
			// closed form.
			//
			// `1 - w` IS `SheenTransmit(alpha, m, cosWi)` bit-for-bit
			// (compare `SheenSelectWeight`'s body with
			// `SheenTransmit`'s), so the view-side arm cancels the
			// divisor exactly and the surviving factor is
			// `SheenTransmit(|n.wo|)` alone -- that is the PER-DRAW
			// multiplier; the EXPECTED coefficient FabricBRDF.h's
			// derivation states (`gap * (1 - m*Ehat(n.v))^2`, two arms)
			// is this draw's value times the `1 - w` probability of
			// drawing it, which restores the cancelled arm.  Both
			// statements are the same estimator seen from either side
			// of the selection.  Written as the explicit
			// quotient anyway: that is the form whose derivation is
			// checkable against the header, and it stays correct if
			// either helper's clamping ever changes.
			//
			// `w == 1` cannot reach here (the branch is taken only when
			// `Get1D() >= w`, and `Get1D()` is in [0,1)); the floor is
			// there so the expression is total rather than
			// total-by-argument.
			const Scalar muDelta = ( scos < 0 ) ? -scos : scos;
			const Scalar atten   = FabricBRDF::SheenTransmit( p.alpha, p.m, cosWi )
			                     * FabricBRDF::SheenTransmit( p.alpha, p.m, muDelta );
			const Scalar sel     = r_max( Scalar(1e-12), Scalar(1) - w );
			const Scalar reprice = atten / sel;
			s.kray   = s.kray * reprice;
			s.krayNM = s.krayNM * reprice;
			continue;
		}

		const Scalar sq = PdfWithParams( ri, sw, nm, p, weave.Get(), ior_stack );
		if( sq <= Scalar(1e-12) ) {
			// Zero the THROUGHPUT, so a bare-substrate weight cannot
			// escape as if it were a fabric one.  The base's own `pdf`
			// is deliberately LEFT ALONE: the ray carries no energy
			// either way, and a non-delta ray with pdf == 0 is a live
			// 0/0 hazard in a downstream MIS denominator, where it would
			// turn a zero contribution into a NaN one.
			// (CoatedSPF.cpp:211-231's reasoning, verbatim.)
			s.kray   = RISEPel( 0, 0, 0 );
			s.krayNM = 0;
			continue;
		}

		// `|cos|`, not `cos`: a transmit-side draw is below the shading
		// normal and its projected-solid-angle weight is the magnitude.
		// Identical to the committed `scos` wherever `scos > 0`, which
		// is everywhere a reflection-only substrate can land.
		const Scalar absCos = ( scos < 0 ) ? -scos : scos;
		if( nm < 0 ) {
			s.kray = pBRDF->ValueWithParams( sw, ri, p, weave.Get() ) * ( absCos / sq );
		} else {
			s.krayNM = pBRDF->ValueNMWithParams( sw, ri, nm, p, weave.Get() ) * ( absCos / sq );
		}
		s.pdf     = sq;
		// Forcing isDelta FALSE on a ray that reached here is a no-op
		// (the delta case `continue`d above) and is kept only so a
		// substrate that reports a stale flag on a continuum ray cannot
		// leak it -- `f*cos/q` applied to a sample whose density is a
		// Dirac is meaningless.  (CoatedSPF.cpp shares the concern.)
		s.isDelta = false;
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
