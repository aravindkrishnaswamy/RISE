//////////////////////////////////////////////////////////////////////
//
//  PolishedSPF.cpp - Implementation of the polished SPF
//
//  DL-285 (2026-09-28).  The sampler of `PolishedBRDF`'s ONE function
//  (see PolishedBRDF.h for the model):
//
//    * a DELTA coat ray (mirror coat) carrying `tau F(ci)`;
//    * a GLOSSY coat ray drawn from the coat lobe about the mirror
//      direction (Phong cos^N, or forward-truncated Henyey-Greenstein),
//      carrying its own `f_coat co / p_coat`;
//    * a SUBSTRATE ray drawn cosine-weighted about the side-oriented
//      shading normal, carrying `f_sub co / (co/pi)`.
//
//  Every emitted ray's kray is its own lobe's `f_I co / p_I`, so summing
//  over what Scatter emits (the legacy shader ops) or selecting one with
//  `ScatteredRayContainer::RandomlySelect` and dividing by the realized
//  selection probability (PT/BDPT/VCM/MLT) both estimate
//  integral f co L with the SAME f that `IBSDF::value` returns.
//
//  `Pdf`/`PdfNM` report the density of what Scatter + RandomlySelect
//  actually generate.  Both non-delta lobes carry DIRECTION-DEPENDENT
//  selection weights (the coat through min(F(ci),F(co)) and co, the
//  substrate through 1-F(co)), so each lobe's realized selection
//  probability at wo is an expectation over the OTHER lobe's own random
//  draw -- the DL-99 case, which needs one quadrature PER lobe (not one
//  shared): the substrate's over its cosine-distributed exit cosine
//  (1-D Gauss-Legendre with the exact tilted-horizon azimuth fraction),
//  the coat's over its own (alpha, psi) square.  A delta coat carries a
//  constant weight and is always emitted, so it only shifts the others.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolishedSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Gauss-Legendre nodes on [0,1] for the substrate expectation, in
	//! t with mu = sqrt(t) (cosine-weighted exit: mu^2 is uniform).
	const int kSubQuadN = 8;
	const Scalar kGL8X[8] = {
		0.019855071751231912, 0.10166676129318664, 0.2372337950418355, 0.40828267875217505,
		0.59171732124782495, 0.7627662049581645, 0.89833323870681336, 0.98014492824876809 };
	const Scalar kGL8W[8] = {
		0.050614268145188261, 0.11119051722668723, 0.15685332293894358, 0.18134189168918091,
		0.18134189168918091, 0.15685332293894358, 0.11119051722668723, 0.050614268145188261 };

	//! Deterministic replay of the glossy coat sampler's own draw:
	//! kCoatQuadA midpoints in the sampler's u (the inverse-CDF polar
	//! angle) times kCoatQuadP midpoints over the azimuth ARC that stays
	//! above the shading horizon at that polar angle (the cut-away part is
	//! booked analytically as a rejected draw).
	const int kCoatQuadA = 8;
	const int kCoatQuadP = 8;

	//! `ScatteredRayContainer::RandomlySelect`'s probability of returning
	//! a ray of weight @a w from a container of @a count rays whose
	//! weights sum to @a total.
	inline Scalar SelectProbability( const Scalar w, const int count, const Scalar total )
	{
		if( count == 1 ) {
			return 1;
		}
		return ( total > NEARZERO ) ? ( w / total ) : Scalar(0);
	}

	//! The i-th glossy component of @a L (skipping delta ones).
	inline int GlossyComponent( const PolishedLobes& L, int i )
	{
		for( int k = 0; k < L.K; ++k ) {
			if( !L.delta[k] ) {
				if( i == 0 ) return k;
				--i;
			}
		}
		return 0;
	}

	//! Direction at polar cosine @a cosA and azimuth (@a cp, @a sp) about @a f's w axis.
	inline Vector3 LobeDirection( const OrthonormalBasis3D& f, const Scalar cosA, const Scalar cp, const Scalar sp )
	{
		const Scalar sinA = sqrt( r_max( Scalar(0), Scalar(1) - cosA * cosA ) );
		return Vector3Ops::Normalize( f.w() * cosA + f.u() * ( sinA * cp ) + f.v() * ( sinA * sp ) );
	}

	inline void Store( ScatteredRay& r, const PolishedLobes& L, const Scalar k[3] )
	{
		if( L.nch == 1 ) {
			r.krayNM = k[0];
		} else {
			r.kray = RISEPel( k[0], k[1], k[2] );
		}
	}
}

PolishedSPF::PolishedSPF(
	const IPainter& Rd_,
	const IScalarPainter& tau_,
	const IScalarPainter& Nt_,
	const IScalarPainter& s,
	const bool hg
	) :
  pBRDF( 0 )
{
	pBRDF = new PolishedBRDF( Rd_, tau_, Nt_, s, hg );
	GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "polished BRDF" );
}

PolishedSPF::~PolishedSPF( )
{
	safe_release( pBRDF );
}

void PolishedSPF::ScatterImpl(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	PolishedLobes L;
	pBRDF->Resolve( ri, ior_stack.top(), nm, L );

	// Coat pair then substrate pair, drawn unconditionally so the
	// per-vertex dimension budget does not depend on what is emitted;
	// a third coat draw picks the component only when the lobe shape
	// varies per channel.
	const Scalar u1 = sampler.Get1D();
	const Scalar u2 = sampler.Get1D();
	const Scalar u3 = sampler.Get1D();
	const Scalar u4 = sampler.Get1D();
	const Scalar uK = ( L.nGlossy > 1 ) ? sampler.Get1D() : Scalar(0);

	if( !L.valid ) {
		return;
	}

	Scalar k[3];

	// Delta coat.  Mandatory lobe: where the shading-normal mirror would
	// tunnel through the true geometric surface it is re-derived about
	// the ray-anchored geometric normal (PerfectReflectorSPF's rule),
	// so it is ALWAYS emitted -- the realized-density bookkeeping in
	// PdfImpl relies on that.
	if( L.nDelta > 0 )
	{
		ScatteredRay d;
		d.type = ScatteredRay::eRayReflection;
		d.isDelta = true;
		d.pdf = 1.0;
		Vector3 dir = L.rv;
		if( Vector3Ops::Dot( dir, L.geomN ) <= 0 ) {
			dir = Optics::CalculateReflectedRay( ri.ray.Dir(), L.geomN );
		}
		d.ray.Set( ri.ptIntersection, dir );
		PolishedBRDF::DeltaKray( L, k );
		Store( d, L, k );
		scattered.AddScatteredRay( d );
	}

	// Glossy coat: the component's lobe about the mirror direction.  A
	// draw outside the model's support (below the side-oriented shading
	// normal or the geometric horizon) is a zero sample, not re-drawn.
	if( L.nGlossy > 0 )
	{
		const int pick = r_min( L.nGlossy - 1, int( uK * L.nGlossy ) );
		const int comp = GlossyComponent( L, pick );
		OrthonormalBasis3D f;
		f.CreateFromW( L.rv );
		const Scalar cosA = PolishedBRDF::ComponentCosAlpha( L, comp, u1 );
		const Vector3 wo = LobeDirection( f, cosA, cos( TWO_PI * u2 ), sin( TWO_PI * u2 ) );
		if( PolishedBRDF::Accepted( L, wo ) )
		{
			const Scalar p = PolishedBRDF::GlossyCoatDensity( L, wo );
			if( p > 0 ) {
				ScatteredRay c;
				c.type = ScatteredRay::eRayReflection;
				c.isDelta = false;
				c.ray.Set( ri.ptIntersection, wo );
				c.pdf = p;
				PolishedBRDF::CoatKray( L, wo, k );
				Store( c, L, k );
				scattered.AddScatteredRay( c );
			}
		}
	}

	// Substrate: cosine-weighted about the side-oriented shading normal.
	if( L.emitDiffuse )
	{
		const Vector3 wo = GeometricUtilities::CreateDiffuseVector( L.onb, Point2( u3, u4 ) );
		if( PolishedBRDF::Accepted( L, wo ) )
		{
			ScatteredRay s;
			s.type = ScatteredRay::eRayDiffuse;
			s.isDelta = false;
			s.ray.Set( ri.ptIntersection, wo );
			s.pdf = Vector3Ops::Dot( wo, L.n ) * INV_PI;
			PolishedBRDF::SubstrateKray( L, wo, k );
			Store( s, L, k );
			scattered.AddScatteredRay( s );
		}
	}
}

void PolishedSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, Scalar(-1), scattered, ior_stack );
}

void PolishedSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	ScatterImpl( ri, sampler, nm, scattered, ior_stack );
}

Scalar PolishedSPF::PdfImpl(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	PolishedLobes L;
	pBRDF->Resolve( ri, ior_stack.top(), nm, L );
	if( !L.valid ) {
		return 0;
	}
	const Vector3 wo = Vector3Ops::Normalize( woIn );
	if( !PolishedBRDF::Accepted( L, wo ) ) {
		return 0;
	}
	const Scalar co = Vector3Ops::Dot( wo, L.n );

	Scalar k[3];
	const int    nD = ( L.nDelta > 0 ) ? 1 : 0;		// the delta coat is one always-emitted ray
	Scalar wDelta = 0;
	if( nD ) {
		PolishedBRDF::DeltaKray( L, k );
		wDelta = PolishedBRDF::Reduce( L, k );
	}

	const Scalar pG = ( L.nGlossy > 0 ) ? PolishedBRDF::GlossyCoatDensity( L, wo ) : Scalar(0);
	const Scalar pS = L.emitDiffuse ? co * INV_PI : Scalar(0);

	Scalar result = 0;

	// ---- glossy coat generated wo: expectation over the substrate draw.
	if( pG > 0 )
	{
		PolishedBRDF::CoatKray( L, wo, k );
		const Scalar wG = PolishedBRDF::Reduce( L, k );
		const Scalar selNoSub = SelectProbability( wG, nD + 1, wDelta + wG );
		Scalar q = selNoSub;
		if( L.emitDiffuse )
		{
			// Exact acceptance fraction of the substrate's azimuth against the
			// geometric horizon at each exit cosine (1 when geomN == n).
			const Scalar cosG = r_min( Scalar(1), r_max( Scalar(-1), Vector3Ops::Dot( L.n, L.geomN ) ) );
			const Scalar sinG = sqrt( r_max( Scalar(0), Scalar(1) - cosG * cosG ) );
			q = 0;
			Scalar ks[3];
			for( int j = 0; j < kSubQuadN; ++j ) {
				const Scalar mu = sqrt( kGL8X[j] );
				Scalar a = 1;
				if( sinG > Scalar(1e-9) ) {
					const Scalar s = sqrt( r_max( Scalar(0), Scalar(1) - mu * mu ) );
					if( s > 0 ) {
						const Scalar x = -( mu * cosG ) / ( s * sinG );
						a = acos( r_min( Scalar(1), r_max( Scalar(-1), x ) ) ) * INV_PI;
					} else {
						a = ( mu * cosG > 0 ) ? Scalar(1) : Scalar(0);
					}
				}
				// Substrate weight at exit cosine mu (it depends on mu alone).
				for( int c = 0; c < L.nch; ++c ) {
					const Scalar Fo = PolishedBRDF::Fresnel( mu, L.outer, L.eta[c] );
					ks[c] = ( L.Tavg[c] > 0 ) ? L.rd[c] * ( 1.0 - L.Fi[c] ) * ( 1.0 - Fo ) / L.Tavg[c] : Scalar(0);
				}
				for( int c = L.nch; c < 3; ++c ) ks[c] = 0;
				const Scalar wS = PolishedBRDF::Reduce( L, ks );
				q += kGL8W[j] * ( a * SelectProbability( wG, nD + 2, wDelta + wG + wS ) + ( 1 - a ) * selNoSub );
			}
		}
		result += pG * q;
	}

	// ---- substrate generated wo: expectation over the glossy coat draw.
	if( pS > 0 )
	{
		PolishedBRDF::SubstrateKray( L, wo, k );
		const Scalar wS = PolishedBRDF::Reduce( L, k );
		const Scalar selNoCoat = SelectProbability( wS, nD + 1, wDelta + wS );
		Scalar q = selNoCoat;
		if( L.nGlossy > 0 )
		{
			// Replay frame about the mirror direction: n = ci rv + sRv e1,
			// so a lobe direction at (alpha, psi) has exit cosine
			// mu = ci cos(alpha) + sRv sin(alpha) cos(psi), and its
			// geometric-horizon dot is gz cos(alpha) + sin(alpha)(gx cos(psi)
			// + gy sin(psi)) -- no per-node vector construction.
			const Scalar sRv = sqrt( r_max( Scalar(0), Scalar(1) - L.ci * L.ci ) );
			Vector3 e1;
			if( sRv > Scalar(1e-9) ) {
				e1 = ( L.n - L.rv * L.ci ) * ( Scalar(1) / sRv );
			} else {
				OrthonormalBasis3D fr;
				fr.CreateFromW( L.rv );
				e1 = fr.u();
			}
			const Vector3 e2 = Vector3Ops::Cross( L.rv, e1 );
			const Scalar gz = Vector3Ops::Dot( L.geomN, L.rv );
			const Scalar gx = Vector3Ops::Dot( L.geomN, e1 );
			const Scalar gy = Vector3Ops::Dot( L.geomN, e2 );
			Scalar sum = 0;
			Scalar kc[3];
			for( int i = 0; i < L.nGlossy; ++i ) {
				const int comp = GlossyComponent( L, i );
				for( int a = 0; a < kCoatQuadA; ++a ) {
					const Scalar cosA = PolishedBRDF::ComponentCosAlpha( L, comp, ( a + 0.5 ) / kCoatQuadA );
					const Scalar sinA = sqrt( r_max( Scalar(0), Scalar(1) - cosA * cosA ) );
					// The shading-horizon cut mu > 0 is an ARC in psi at fixed
					// alpha: cos(psi) > -ci cos(alpha) / (sRv sin(alpha)).
					// Integrate the kept arc with its own midpoints and book the
					// cut-away fraction as a rejected coat draw, so the
					// quadrature never straddles the discontinuity (the
					// dominant error of a plain azimuth grid at grazing).
					const Scalar rr = sRv * sinA;
					Scalar psi0 = PI;
					if( rr > Scalar(1e-12) ) {
						const Scalar x = -( L.ci * cosA ) / rr;
						psi0 = ( x <= -1 ) ? PI : ( ( x >= 1 ) ? Scalar(0) : acos( x ) );
					} else if( !( L.ci * cosA > 0 ) ) {
						psi0 = 0;
					}
					const Scalar keep = psi0 * INV_PI;
					Scalar arc = 0;
					if( keep > 0 ) {
						// Half-arc midpoints, each evaluated at +psi and -psi: the
						// exit cosine (hence the weight, for a single component) is
						// even in psi, and only the geometric-horizon test sees the
						// sign, so this is a 2*kCoatQuadP-node rule for the cost of
						// kCoatQuadP weight evaluations.  cos/sin by recurrence.
						const Scalar step = psi0 / Scalar( kCoatQuadP );
						const Scalar cStep = cos( step ), sStep = sin( step );
						Scalar cp = cos( 0.5 * step ), sp = sin( 0.5 * step );
						for( int p = 0; p < kCoatQuadP; ++p ) {
							const Scalar mu = L.ci * cosA + rr * cp;
							const Scalar gEven = gz * cosA + sinA * gx * cp;
							const Scalar gOdd  = sinA * gy * sp;
							Scalar selK1 = -1;		// K == 1: the weight depends on mu alone
							for( int sgn = 0; sgn < 2; ++sgn ) {
								const Scalar gdot = sgn ? ( gEven - gOdd ) : ( gEven + gOdd );
								if( !( mu > 0 && gdot > 0 ) ) {
									arc += selNoCoat;
									continue;
								}
								if( L.K == 1 ) {
									if( selK1 < 0 ) {
										PolishedBRDF::CoatKrayAtExitCosine( L, mu, kc );
										selK1 = SelectProbability( wS, nD + 2, wDelta + PolishedBRDF::Reduce( L, kc ) + wS );
									}
									arc += selK1;
								} else {
									const Vector3 C = Vector3Ops::Normalize(
										L.rv * cosA + e1 * ( sinA * cp ) + e2 * ( sinA * ( sgn ? -sp : sp ) ) );
									PolishedBRDF::CoatKray( L, C, kc );
									arc += SelectProbability( wS, nD + 2, wDelta + PolishedBRDF::Reduce( L, kc ) + wS );
								}
							}
							const Scalar cn = cp * cStep - sp * sStep;
							sp = sp * cStep + cp * sStep;
							cp = cn;
						}
						arc /= Scalar( 2 * kCoatQuadP );
					}
					sum += keep * arc + ( 1 - keep ) * selNoCoat;
				}
			}
			q = sum / Scalar( L.nGlossy * kCoatQuadA );
		}
		result += pS * q;
	}

	return result;
}

Scalar PolishedSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, Scalar(-1), ior_stack );
}

Scalar PolishedSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo, nm, ior_stack );
}

//////////////////////////////////////////////////////////////////////
// HWSS companion evaluation (DL-125 / DL-216).  Per selected lobe at
// wavelength nm:
//   glossy coat:  f_coat(nm)             (EvaluateLobeFNM)
//                 f_coat(nm) co / p_coat (EvaluateKrayNM, 5-arg)
//   substrate:    f_sub(nm)              / f_sub(nm) co / (co/pi)
//   delta coat:   no f (-1)              / tau(nm) F(nm, ci)
// The companion ladders divide by the HERO's RandomlySelect probability
// (PT) or form companion/hero ratios in which it cancels (BDPT/VCM/MLT),
// so the per-lobe values are exactly what they need.
//////////////////////////////////////////////////////////////////////
Scalar PolishedSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	PolishedLobes L;
	pBRDF->Resolve( ri, ior_stack.top(), nm, L );
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	Scalar f[3];
	if( rayType == ScatteredRay::eRayDiffuse ) {
		PolishedBRDF::SubstrateF( L, wo, f );
		return f[0];
	}
	if( rayType == ScatteredRay::eRayReflection ) {
		if( L.nGlossy == 0 ) {
			return -1;		// delta coat: no density-free value exists
		}
		PolishedBRDF::CoatF( L, wo, f );
		return f[0];
	}
	return -1;
}

Scalar PolishedSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	PolishedLobes L;
	pBRDF->Resolve( ri, ior_stack.top(), nm, L );
	if( !L.valid ) {
		return 0;
	}
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	Scalar k[3];
	if( rayType == ScatteredRay::eRayDiffuse ) {
		PolishedBRDF::SubstrateKray( L, wo, k );
		return k[0];
	}
	if( rayType == ScatteredRay::eRayReflection ) {
		if( L.nDelta > 0 ) {
			PolishedBRDF::DeltaKray( L, k );
		} else {
			PolishedBRDF::CoatKray( L, wo, k );
		}
		return k[0];
	}
	return -1;
}

Scalar PolishedSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack,
	Scalar pdfHero
	) const
{
	if( pdfHero <= 0 ) {
		return EvaluateKrayNM( ri, outDir, rayType, nm, ior_stack );
	}
	const Scalar f = EvaluateLobeFNM( ri, outDir, rayType, nm, ior_stack );
	if( f < 0 ) {
		return EvaluateKrayNM( ri, outDir, rayType, nm, ior_stack );
	}
	const Scalar cos_o = fabs( Vector3Ops::Dot( Vector3Ops::Normalize( outDir ), ri.vNormal ) );
	return ( f * cos_o ) / pdfHero;
}
