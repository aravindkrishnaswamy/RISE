//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDF.cpp - Implementation of the polished_material
//    reflectance function (DL-285).  See PolishedBRDF.h for the model
//    and its reciprocity / energy derivation.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl285`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolishedBRDF.h"
#include "../Utilities/Optics.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Phong exponent at or above which the coat is a mirror, and the HG
	//! asymmetry at or above which it is.  The same thresholds as
	//! `PolishedSPF::GetSpecularInfo` (the delta decision must be ONE
	//! decision everywhere).
	const Scalar kPhongDeltaThreshold = 1000000.0;
	const Scalar kHGDeltaThreshold    = 1.0;
	//! Below |g| < kHGIsotropic the truncated-HG inverse CDF loses digits
	//! to cancellation (its 1/(2g) prefactor); the lobe is then treated as
	//! the uniform forward hemisphere, by the sampler and the density alike.
	const Scalar kHGIsotropic         = 1e-3;
	//! HG asymmetries below -kHGMax are clamped (g = -1 is degenerate).
	const Scalar kHGMax               = 0.999;

	//! 32-node Gauss-Legendre rule on [0,1].
	const Scalar kGL32X[32] = {
		0.0013680690752592151, 0.0071942442273658647, 0.017618872206246805, 0.032546962031130167,
		0.051839422116973954, 0.075316193133715015, 0.10275810201602881, 0.13390894062985514,
		0.16847786653489238, 0.20614212137961885, 0.24655004553388532, 0.28932436193468236,
		0.33406569885893617, 0.38035631887393145, 0.42776401920860174, 0.47584616715613082,
		0.52415383284386918, 0.57223598079139826, 0.61964368112606849, 0.66593430114106389,
		0.71067563806531764, 0.75344995446611462, 0.79385787862038115, 0.83152213346510762,
		0.86609105937014486, 0.89724189798397114, 0.92468380686628504, 0.94816057788302599,
		0.96745303796886983, 0.98238112779375319, 0.99280575577263419, 0.99863193092474078 };
	const Scalar kGL32W[32] = {
		0.0035093050047351809, 0.0081371973654532428, 0.012696032654631364, 0.017136931456510816,
		0.021417949011113463, 0.02549902963118798, 0.02934204673926773, 0.0329111113881808,
		0.0361728970544241, 0.039096947893535058, 0.041655962113473312, 0.043826046502201843,
		0.045586939347881855, 0.046922199540402193, 0.047819360039637299, 0.048270044257363788,
		0.048270044257363788, 0.047819360039637299, 0.046922199540402193, 0.045586939347881855,
		0.043826046502201843, 0.041655962113473312, 0.039096947893535058, 0.0361728970544241,
		0.0329111113881808, 0.02934204673926773, 0.02549902963118798, 0.021417949011113463,
		0.017136931456510816, 0.012696032654631364, 0.0081371973654532428, 0.0035093050047351809 };

	//! Hemispherical transmittance 1 - F_avg for an EXTERNAL interface of
	//! relative index eta > 1.  Closed form (the standard unpolarized
	//! Fresnel integral; verified against a 4e6-point quadrature of
	//! `Optics::CalculateDielectricReflectanceCosine`'s own formula to
	//! ~1e-12 over eta in [1.02, 1e4]).  It cancels catastrophically as
	//! eta -> 1, so below eta = 1.02 a 32-node Gauss-Legendre rule in
	//! mu = t^2 integrates the model's own Fresnel function directly
	//! (~1e-12 relative there).
	Scalar ExternalTransmittance( const Scalar eta )
	{
		if( eta < 1.02 ) {
			Scalar s = 0;
			for( int i = 0; i < 32; ++i ) {
				const Scalar t  = kGL32X[i];
				const Scalar mu = t * t;
				// d(mu) = 2t dt, weight 2 mu (1 - F).
				s += kGL32W[i] * 2.0 * mu * ( 1.0 - PolishedBRDF::Fresnel( mu, 1.0, eta ) ) * 2.0 * t;
			}
			return s;
		}
		const Scalar n  = eta;
		const Scalar n2 = n * n;
		const Scalar n4 = n2 * n2;
		const Scalar Favg =
			0.5
			+ ( n - 1.0 ) * ( 3.0 * n + 1.0 ) / ( 6.0 * ( n + 1.0 ) * ( n + 1.0 ) )
			+ n2 * ( n2 - 1.0 ) * ( n2 - 1.0 ) / ( ( n2 + 1.0 ) * ( n2 + 1.0 ) * ( n2 + 1.0 ) ) * log( ( n - 1.0 ) / ( n + 1.0 ) )
			- 2.0 * n2 * n * ( n2 + 2.0 * n - 1.0 ) / ( ( n2 + 1.0 ) * ( n4 - 1.0 ) )
			+ 8.0 * n4 * ( n4 + 1.0 ) / ( ( n2 + 1.0 ) * ( n4 - 1.0 ) * ( n4 - 1.0 ) ) * log( n );
		return r_min( Scalar(1), r_max( Scalar(0), Scalar(1) - Favg ) );
	}

	inline Scalar HGDensity( const Scalar g, const Scalar mu )
	{
		const Scalar d = 1.0 + g * g - 2.0 * g * mu;
		return ( 1.0 - g * g ) / ( FOUR_PI * d * sqrt( d ) );
	}

	//! P(cos alpha <= mu) of the untruncated HG phase function, g != 0.
	inline Scalar HGCdf( const Scalar g, const Scalar mu )
	{
		return ( 1.0 - g * g ) / ( 2.0 * g ) * ( 1.0 / sqrt( 1.0 + g * g - 2.0 * g * mu ) - 1.0 / ( 1.0 + g ) );
	}
}

PolishedBRDF::PolishedBRDF(
	const IPainter& Rd_,
	const IScalarPainter& tau_,
	const IScalarPainter& Nt_,
	const IScalarPainter& s,
	const bool hg
	) :
  pRd( &Rd_ ),
  pTau( &tau_ ),
  pNt( &Nt_ ),
  pScat( &s ),
  bHG( hg )
{
	pRd->addref();
	pTau->addref();
	pNt->addref();
	pScat->addref();
}

PolishedBRDF::~PolishedBRDF()
{
	safe_release( pRd );
	safe_release( pTau );
	safe_release( pNt );
	safe_release( pScat );
}

void PolishedBRDF::SetDiffuseReflectance( const IPainter& v ) { v.addref(); safe_release( pRd ); pRd = &v; }
void PolishedBRDF::SetTransmittance( const IScalarPainter& v ) { v.addref(); safe_release( pTau ); pTau = &v; }
void PolishedBRDF::SetIOR( const IScalarPainter& v )           { v.addref(); safe_release( pNt ); pNt = &v; }
void PolishedBRDF::SetScattering( const IScalarPainter& v )    { v.addref(); safe_release( pScat ); pScat = &v; }

// The unpolarized dielectric Fresnel reflectance in the classic g-form
// (Cook & Torrance 1982; Walter et al. 2007 eq. 22): with the relative
// index eta = coat / outer and g^2 = eta^2 - 1 + mu^2,
//   F = (g-mu)^2 / (2 (g+mu)^2) * ( 1 + ((mu(g+mu) - 1)/(mu(g-mu) + 1))^2 ),
// and total internal reflection (g^2 <= 0) reflects everything.  The same
// function as `Optics::CalculateDielectricReflectanceCosine` (worst
// absolute difference 1.1e-12, at matched-index grazing, over the grid in
// tests/PolishedBRDFConsistencyTest.cpp gate 0a) at a fraction of its
// cost, which matters because PolishedSPF::Pdf's replay evaluates it at
// every quadrature node.  Matched indices return exactly 0, invalid input
// 1 (an opaque interface), both as Optics does.
Scalar PolishedBRDF::Fresnel( const Scalar mu, const Scalar outer, const Scalar coat )
{
	if( !( outer > 0 ) || !( coat > 0 ) || !IsFiniteDouble( outer ) || !IsFiniteDouble( coat ) ) {
		return 1;
	}
	if( outer == coat ) {
		return 0;
	}
	const Scalar c   = r_min( Scalar(1), fabs( mu ) );
	const Scalar eta = coat / outer;
	const Scalar g2  = eta * eta - 1.0 + c * c;
	if( !( g2 > 0 ) ) {
		return 1;
	}
	const Scalar g = sqrt( g2 );
	const Scalar a = ( g - c ) / ( g + c );
	const Scalar b = ( c * ( g + c ) - 1.0 ) / ( c * ( g - c ) + 1.0 );
	return r_min( Scalar(1), 0.5 * a * a * ( 1.0 + b * b ) );
}

Scalar PolishedBRDF::HemisphericalTransmittance( const Scalar outer, const Scalar coat )
{
	if( !( outer > 0 ) || !( coat > 0 ) || !IsFiniteDouble( outer ) || !IsFiniteDouble( coat ) ) {
		return 0;		// Fresnel() reports 1 (opaque interface) for invalid input
	}
	if( outer == coat ) {
		return 1;
	}
	// One-entry per-thread memo: a scene's polished coats rarely change
	// index from one shading point to the next, and the closed form costs
	// two logarithms.  Keyed on the exact inputs, so a hit is bit-for-bit
	// the recomputed value.
	static thread_local Scalar memoOuter = -1, memoCoat = -1, memoT = 0;
	if( outer == memoOuter && coat == memoCoat ) {
		return memoT;
	}
	const Scalar eta = coat / outer;
	// Internal interface (eta < 1): the reciprocity / etendue identity
	// T_int(eta) = eta^2 T_ext(1/eta) (verified numerically to ~1e-8
	// at eta = 1/1.5, 1/1.33, 0.9 against direct quadrature with TIR).
	const Scalar T = ( eta > 1 ) ? ExternalTransmittance( eta ) : eta * eta * ExternalTransmittance( 1.0 / eta );
	memoOuter = outer;
	memoCoat  = coat;
	memoT     = T;
	return T;
}

void PolishedBRDF::Resolve( const RayIntersectionGeometric& ri, const Scalar outerIn, const Scalar nm, PolishedLobes& L ) const
{
	// Side of the surface: GEOMETRIC (PolishedSPF's bBackface convention);
	// the shading normal flips with it, so a back-face hit is priced
	// exactly like a front-face one on the side the ray arrived from.
	const bool bBackface = Vector3Ops::Dot( ri.vGeomNormal, ri.ray.Dir() ) > 0;
	L.n = Vector3Ops::Normalize( bBackface ? -ri.vNormal : ri.vNormal );
	L.onb = ri.onb;
	if( Vector3Ops::Dot( L.onb.w(), L.n ) < 0 ) {
		L.onb.FlipW();
	}
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : L.n;
	L.geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	L.wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	L.ci = Vector3Ops::Dot( L.wi, L.n );
	L.rv = Vector3Ops::Normalize( Optics::CalculateReflectedRay( ri.ray.Dir(), L.n ) );
	L.outer = ( outerIn > 0 ) ? outerIn : 1.0;
	L.hg = bHG;
	L.valid = L.ci > 0;

	// Channel parameters.  RGB keeps the pre-DL-285 dispersion rule: the
	// coat index and the lobe shape vary per channel only when their
	// painter reports per-channel variation (tau is multiplicative
	// attenuation, never a dispersion source, and always per channel).
	bool perChannelScat = false;
	if( nm < 0 ) {
		L.nch = 3;
		const ScalarTriple tauV  = pTau->GetValuesAt( ri );
		const ScalarTriple iorV  = pNt->GetValuesAt( ri );
		const ScalarTriple scatV = pScat->GetValuesAt( ri );
		const RISEPel      rdV   = pRd->GetColor( ri );
		const bool perChannelIOR = pNt->HasPerChannelVariation();
		perChannelScat = pScat->HasPerChannelVariation();
		for( int c = 0; c < 3; ++c ) {
			L.tau[c] = tauV.v[c];
			L.rd[c]  = rdV[c];
			L.eta[c] = perChannelIOR ? iorV.v[c] : iorV.v[0];
		}
		L.K = perChannelScat ? 3 : 1;
		for( int k = 0; k < L.K; ++k ) {
			L.scat[k] = scatV.v[k];
		}
	} else {
		L.nch = 1;
		L.tau[0]  = pTau->GetValueAtNM( ri, nm );
		L.rd[0]   = GuardedGetColorNM( *pRd, ri, nm );
		L.eta[0]  = pNt->GetValueAtNM( ri, nm );
		L.K = 1;
		L.scat[0] = pScat->GetValueAtNM( ri, nm );
	}

	L.etaShared = true;
	for( int c = 1; c < L.nch; ++c ) {
		if( L.eta[c] != L.eta[0] ) {
			L.etaShared = false;
		}
	}
	const Scalar ciF = r_max( Scalar(0), L.ci );
	L.emitDiffuse = false;
	for( int c = 0; c < L.nch; ++c ) {
		if( c > 0 && L.etaShared ) {
			L.Fi[c]   = L.Fi[0];
			L.Tavg[c] = L.Tavg[0];
		} else {
			L.Fi[c]   = Fresnel( ciF, L.outer, L.eta[c] );
			L.Tavg[c] = HemisphericalTransmittance( L.outer, L.eta[c] );
		}
		if( L.Fi[c] < 1.0 ) {
			L.emitDiffuse = true;
		}
	}

	L.nDelta = 0;
	L.nGlossy = 0;
	for( int k = 0; k < L.K; ++k ) {
		Scalar s = L.scat[k];
		L.hgMass[k] = 1;
		L.hgC0[k] = 0;
		if( bHG ) {
			L.delta[k] = ( s >= kHGDeltaThreshold );
			if( !L.delta[k] ) {
				s = r_max( s, -kHGMax );
				L.scat[k] = s;
				if( fabs( s ) >= kHGIsotropic ) {
					L.hgC0[k]   = HGCdf( s, 0.0 );
					L.hgMass[k] = 1.0 - L.hgC0[k];
				}
			}
		} else {
			L.delta[k] = ( s >= kPhongDeltaThreshold );
			if( !L.delta[k] ) {
				L.scat[k] = r_max( s, Scalar(0) );		// a negative exponent is not a lobe
			}
		}
		if( L.delta[k] ) {
			++L.nDelta;
		} else {
			++L.nGlossy;
		}
	}
}

Scalar PolishedBRDF::ComponentDensity( const PolishedLobes& L, const int k, const Scalar cosAlpha )
{
	if( !( cosAlpha > 0 ) || L.delta[k] ) {
		return 0;
	}
	const Scalar mu = r_min( Scalar(1), cosAlpha );
	if( L.hg ) {
		const Scalar g = L.scat[k];
		if( fabs( g ) < kHGIsotropic ) {
			return INV_PI * 0.5;
		}
		return HGDensity( g, mu ) / L.hgMass[k];
	}
	const Scalar N = L.scat[k];
	return ( N + 1.0 ) * INV_PI * 0.5 * pow( mu, N );
}

Scalar PolishedBRDF::ComponentCosAlpha( const PolishedLobes& L, const int k, const Scalar u )
{
	if( L.hg ) {
		const Scalar g = L.scat[k];
		if( fabs( g ) < kHGIsotropic ) {
			return r_max( Scalar(0), r_min( Scalar(1), u ) );
		}
		// Standard HG inversion, restricted to the forward hemisphere by
		// remapping u onto [C(0), 1] (the pre-DL-285 sampler redrew from
		// the SAME u in a `while( alpha > pi/2 )` loop, which never
		// terminated for u below the threshold).
		const Scalar xi = L.hgC0[k] + u * L.hgMass[k];
		const Scalar inner = ( 1.0 - g * g ) / ( 1.0 - g + 2.0 * g * xi );
		const Scalar mu = ( 1.0 + g * g - inner * inner ) / ( 2.0 * g );
		return r_max( Scalar(0), r_min( Scalar(1), mu ) );
	}
	return pow( u, 1.0 / ( L.scat[k] + 1.0 ) );
}

Scalar PolishedBRDF::GlossyCoatDensity( const PolishedLobes& L, const Vector3& wo )
{
	if( L.nGlossy == 0 ) {
		return 0;
	}
	const Scalar cosAlpha = Vector3Ops::Dot( wo, L.rv );
	Scalar p = 0;
	for( int k = 0; k < L.K; ++k ) {
		if( !L.delta[k] ) {
			p += ComponentDensity( L, k, cosAlpha );
		}
	}
	return p / Scalar( L.nGlossy );
}

bool PolishedBRDF::Accepted( const PolishedLobes& L, const Vector3& wo )
{
	return L.valid && Vector3Ops::Dot( wo, L.n ) > 0 && Vector3Ops::Dot( wo, L.geomN ) > 0;
}

void PolishedBRDF::CoatF( const PolishedLobes& L, const Vector3& wo, Scalar out[3] )
{
	out[0] = out[1] = out[2] = 0;
	if( L.nGlossy == 0 || !Accepted( L, wo ) ) {
		return;
	}
	const Scalar co = Vector3Ops::Dot( wo, L.n );
	const Scalar cosAlpha = Vector3Ops::Dot( wo, L.rv );
	const Scalar geo = 2.0 / ( L.ci + co );
	Scalar Fo0 = -1;
	for( int c = 0; c < L.nch; ++c ) {
		const int k = ( L.K == 1 ) ? 0 : c;
		if( L.delta[k] ) {
			continue;
		}
		const Scalar P = ComponentDensity( L, k, cosAlpha );
		if( P <= 0 ) {
			continue;
		}
		Scalar Fo;
		if( L.etaShared ) {
			if( Fo0 < 0 ) Fo0 = Fresnel( co, L.outer, L.eta[0] );
			Fo = Fo0;
		} else {
			Fo = Fresnel( co, L.outer, L.eta[c] );
		}
		out[c] = L.tau[c] * r_min( L.Fi[c], Fo ) * P * geo;
	}
}

void PolishedBRDF::SubstrateF( const PolishedLobes& L, const Vector3& wo, Scalar out[3] )
{
	out[0] = out[1] = out[2] = 0;
	if( !L.emitDiffuse || !Accepted( L, wo ) ) {
		return;
	}
	const Scalar co = Vector3Ops::Dot( wo, L.n );
	Scalar Fo0 = -1;
	for( int c = 0; c < L.nch; ++c ) {
		if( !( L.Tavg[c] > 0 ) ) {
			continue;
		}
		Scalar Fo;
		if( L.etaShared ) {
			if( Fo0 < 0 ) Fo0 = Fresnel( co, L.outer, L.eta[0] );
			Fo = Fo0;
		} else {
			Fo = Fresnel( co, L.outer, L.eta[c] );
		}
		out[c] = L.rd[c] * ( 1.0 - L.Fi[c] ) * ( 1.0 - Fo ) * INV_PI / L.Tavg[c];
	}
}

void PolishedBRDF::CoatKray( const PolishedLobes& L, const Vector3& wo, Scalar out[3] )
{
	// Single component (the lobe shape is shared by every channel): the
	// component density P appears once in f_coat and once in the
	// sampling density, so it cancels exactly and the weight is
	// tau min(F(ci), F(co)) 2 co / (ci + co) -- no pow() needed.  This
	// is the path every Pdf quadrature node takes.
	if( L.K == 1 )
	{
		out[0] = out[1] = out[2] = 0;
		if( L.nGlossy == 0 || !Accepted( L, wo ) || !( Vector3Ops::Dot( wo, L.rv ) > 0 ) ) {
			return;
		}
		CoatKrayAtExitCosine( L, Vector3Ops::Dot( wo, L.n ), out );
		return;
	}
	CoatF( L, wo, out );
	const Scalar p = GlossyCoatDensity( L, wo );
	const Scalar co = Vector3Ops::Dot( wo, L.n );
	for( int c = 0; c < 3; ++c ) {
		out[c] = ( p > 0 && co > 0 ) ? out[c] * co / p : Scalar(0);
	}
}

void PolishedBRDF::CoatKrayAtExitCosine( const PolishedLobes& L, const Scalar co, Scalar out[3] )
{
	out[0] = out[1] = out[2] = 0;
	if( L.K != 1 || L.nGlossy == 0 || !( co > 0 ) ) {
		return;
	}
	const Scalar geo = 2.0 * co / ( L.ci + co );
	Scalar Fo0 = -1;
	for( int c = 0; c < L.nch; ++c ) {
		Scalar Fo;
		if( L.etaShared ) {
			if( Fo0 < 0 ) Fo0 = Fresnel( co, L.outer, L.eta[0] );
			Fo = Fo0;
		} else {
			Fo = Fresnel( co, L.outer, L.eta[c] );
		}
		out[c] = L.tau[c] * r_min( L.Fi[c], Fo ) * geo;
	}
}

void PolishedBRDF::SubstrateKray( const PolishedLobes& L, const Vector3& wo, Scalar out[3] )
{
	SubstrateF( L, wo, out );
	for( int c = 0; c < 3; ++c ) {
		out[c] *= PI;
	}
}

void PolishedBRDF::DeltaKray( const PolishedLobes& L, Scalar out[3] )
{
	out[0] = out[1] = out[2] = 0;
	if( !L.valid ) {
		return;
	}
	for( int c = 0; c < L.nch; ++c ) {
		const int k = ( L.K == 1 ) ? 0 : c;
		if( L.delta[k] ) {
			out[c] = L.tau[c] * L.Fi[c];
		}
	}
}

Scalar PolishedBRDF::Reduce( const PolishedLobes& L, const Scalar k[3] )
{
	if( L.nch == 1 ) {
		return k[0];
	}
	return r_max( r_max( k[0], k[1] ), k[2] );
}

RISEPel PolishedBRDF::EvalRGB( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar outer ) const
{
	PolishedLobes L;
	Resolve( ri, outer, Scalar(-1), L );
	if( !L.valid ) {
		return RISEPel( 0, 0, 0 );
	}
	const Vector3 wo = Vector3Ops::Normalize( vLightIn );
	Scalar fc[3], fs[3];
	CoatF( L, wo, fc );
	SubstrateF( L, wo, fs );
	return RISEPel( fc[0] + fs[0], fc[1] + fs[1], fc[2] + fs[2] );
}

Scalar PolishedBRDF::EvalNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const Scalar outer ) const
{
	PolishedLobes L;
	Resolve( ri, outer, nm, L );
	if( !L.valid ) {
		return 0;
	}
	const Vector3 wo = Vector3Ops::Normalize( vLightIn );
	Scalar fc[3], fs[3];
	CoatF( L, wo, fc );
	SubstrateF( L, wo, fs );
	return fc[0] + fs[0];
}

RISEPel PolishedBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	return valueStateful( vLightIn, ri, 0 );
}

Scalar PolishedBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return valueStatefulNM( vLightIn, ri, nm, 0 );
}

// The outer medium is the one the SPF's Fresnel reads: the LIVE stack's
// top when the caller has one (PT NEE, BDPT/VCM vertex evaluation), else
// the ambient index the integrator stamped on the hit from that same
// top at hit-production time.
RISEPel PolishedBRDF::valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* pIORStack ) const
{
	return EvalRGB( vLightIn, ri, pIORStack ? pIORStack->top() : ri.ambientIOR );
}

Scalar PolishedBRDF::valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* pIORStack ) const
{
	return EvalNM( vLightIn, ri, nm, pIORStack ? pIORStack->top() : ri.ambientIOR );
}

// OIDN albedo AOV: the directional albedo at the view direction with the
// coat priced at its delta-limit value tau F(ci) (an upper bound on the
// glossy coat's, by the pairing argument in PolishedBRDF.h).
RISEPel PolishedBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	PolishedLobes L;
	Resolve( ri, ri.ambientIOR, Scalar(-1), L );
	RISEPel a;
	for( int c = 0; c < 3; ++c ) {
		const Scalar F = Fresnel( fabs( L.ci ), L.outer, L.eta[c] );
		a[c] = r_max( Scalar(0), r_min( Scalar(1), L.tau[c] * F + L.rd[c] * ( 1.0 - F ) ) );
	}
	return a;
}

// Reflectance under a uniform incident field.  Exact for the substrate
// (Rd (1 - F_avg): the substrate's directional albedo is Rd (1 - F(ci))
// exactly) and for a DELTA coat (tau F_avg); for a glossy coat tau F_avg
// is the delta limit and an upper bound.  Does not read ri.ray.
bool PolishedBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	const ScalarTriple tauV = pTau->GetValuesAt( ri );
	const ScalarTriple iorV = pNt->GetValuesAt( ri );
	const RISEPel rdV = pRd->GetColor( ri );
	const bool perChannelIOR = pNt->HasPerChannelVariation();
	const Scalar outer = ri.ambientIOR > 0 ? ri.ambientIOR : 1.0;
	for( int c = 0; c < 3; ++c ) {
		const Scalar T = HemisphericalTransmittance( outer, perChannelIOR ? iorV.v[c] : iorV.v[0] );
		out[c] = tauV.v[c] * ( 1.0 - T ) + rdV[c] * T;
	}
	return true;
}

bool PolishedBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	const Scalar outer = ri.ambientIOR > 0 ? ri.ambientIOR : 1.0;
	const Scalar T = HemisphericalTransmittance( outer, pNt->GetValueAtNM( ri, nm ) );
	out = pTau->GetValueAtNM( ri, nm ) * ( 1.0 - T ) + GuardedGetColorNM( *pRd, ri, nm ) * T;
	return true;
}
