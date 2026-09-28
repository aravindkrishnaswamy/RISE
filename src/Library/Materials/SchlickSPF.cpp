//////////////////////////////////////////////////////////////////////
//
//  SchlickSPF.cpp - Implementation of the Schlick SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SchlickSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "SchlickMasking.h"
#include "SchlickDirectionalAlbedo.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Stratified-quadrature resolution PER AXIS for the diffuse lobe's
	//! selection coefficient C_D (see the derivation on
	//! SchlickDiffuseSelectCoefficient below).  kSpecQuadN^2 replays of the
	//! specular sampler per Pdf()/PdfNM() call.  The integrand is bounded in
	//! [0,1] and smooth apart from ONE curve -- the accept boundary, where
	//! Scatter's geometric gate starts rejecting the specular draw -- so the
	//! error is O(1/kSpecQuadN), not O(1/kSpecQuadN^2).  16 was chosen by
	//! measurement over 400 randomised (angle, rd, rs, roughness, isotropy,
	//! tilt) configurations: mean |C_D error| 0.0034, max 0.022, against
	//! 0.0083 / 0.039 at 8 and 0.0013 / 0.0083 at 32 (the cost grows as the
	//! square).  See docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md section 4a.
	const int kSpecQuadN = 16;

	//! The specular lanes `Scatter()` would emit at this shading point: ONE
	//! in the ordinary case (and always in the spectral path), THREE when a
	//! per-channel roughness/isotropy painter drives Scatter()'s per-channel
	//! branch.  `rho` is the lane's selection-weight base -- the value `w`
	//! for which the lane's realized `PTScatterSelectWeight` reads
	//! `rho + (1-rho)*fresnel`.
	struct SchlickLobeSet
	{
		int    count;
		Scalar r[3];
		Scalar p[3];
		Scalar rho[3];
	};

	//! DL-310: the per-channel lanes of the COUPLED diffuse term -- each
	//! channel's own (r, p), exactly as SchlickBRDF::value reads them,
	//! each distinct pair prepared once.
	void SchlickDiffuseLanes( const ScalarTriple& rt, const ScalarTriple& it, SchlickDirectionalAlbedo::Lane lanes[3] )
	{
		for( int ch = 0; ch < 3; ch++ ) {
			int reuse = -1;
			for( int prev = 0; prev < ch; prev++ ) {
				if( rt.v[prev] == rt.v[ch] && it.v[prev] == it.v[ch] ) { reuse = prev; break; }
			}
			if( reuse >= 0 ) {
				lanes[ch] = lanes[reuse];
			} else {
				SchlickDirectionalAlbedo::PrepareLane( lanes[ch], rt.v[ch], it.v[ch] );
			}
		}
	}

	//! DL-310: the diffuse ray's `kray`, min(Rd, 1 - A(i), 1 - A(o)) per
	//! channel -- its own f_D cos / p_D for the cosine-sampled lobe.
	RISEPel SchlickCoupledDiffuseKray( const SchlickDirectionalAlbedo::Lane lanes[3], const RISEPel& rho,
		const RISEPel& rd, const Scalar muI, const Scalar muO )
	{
		RISEPel out;
		for( int ch = 0; ch < 3; ch++ ) {
			out[ch] = SchlickDirectionalAlbedo::CoupledDiffuseAt( lanes[ch], rho[ch], rd[ch], muI, muO );
		}
		return out;
	}

	//! DL-310: what the specular coefficient q_i needs to know about the
	//! diffuse draw.  Before DL-310 the diffuse ray's realized weight was
	//! the constant MaxValue(Rd), so only the probability `aD` that it
	//! survived its geometric gate entered q_i.  It is now
	//! w_D(d) = MaxValue(min(Rd, 1 - A(i), 1 - A(d))), a function of the
	//! drawn direction d, so q_i is an expectation over the diffuse draw
	//! (the DL-99 construction):
	//!     q_i = aD g(W0) + int ds P(s) [ g(w_D(s)) - g(W0) ]
	//! with W0 the unclipped weight and the correction a deterministic
	//! quadrature over the band where some channel clips.  `band.n == 0`
	//! (no channel can clip) is exactly the pre-DL-310 formula.
	struct SchlickDiffuseDraw
	{
		Scalar aD;								//!< P(the diffuse ray passes its gate)
		Scalar W0;								//!< MaxValue of the unclipped weights
		SchlickDirectionalAlbedo::BandNodes band;
		int    count;							//!< channels in `band.W`
	};
}

SchlickSPF::SchlickSPF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IScalarPainter& roughness,
	const IScalarPainter& isotropy
	) :
  pDiffuse( &diffuse ),
  pSpecular( &specular ),
  pRoughness( &roughness ),
  pIsotropy( &isotropy )
{
	pDiffuse->addref();
	pSpecular->addref();
	pRoughness->addref();
	pIsotropy->addref();
}

SchlickSPF::~SchlickSPF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pRoughness );
	safe_release( pIsotropy );
}

void SchlickSPF::SetDiffuse( const IPainter& v )       { v.addref(); safe_release( pDiffuse );   pDiffuse   = &v; }
void SchlickSPF::SetSpecular( const IPainter& v )      { v.addref(); safe_release( pSpecular );  pSpecular  = &v; }
void SchlickSPF::SetRoughness( const IScalarPainter& v ){ v.addref(); safe_release( pRoughness ); pRoughness = &v; }
void SchlickSPF::SetIsotropy( const IScalarPainter& v ) { v.addref(); safe_release( pIsotropy );  pIsotropy  = &v; }

static inline void GenerateDiffuseRay(
		ScatteredRay& diffuse,
		const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& ptrand										///< [in] Random numbers
		)
{
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( onb, ptrand ) );
}

//! The azimuthal half of Schlick's inverse-CDF warp: b in [0,1) -> phi in
//! [0, 2pi).  Extracted verbatim from GenerateSpecularRay so `Pdf()` can
//! replay the sampler EXACTLY (same branches, same arithmetic, same order)
//! rather than approximate it -- see SchlickDiffuseSelectCoefficient.
static inline Scalar SchlickSamplePhi( const Scalar b, const Scalar p )
{
	const Scalar sqr_p = p*p;

	Scalar phi = 0;
	if( b < 0.25 )
	{
		const Scalar val = 4.0 * b;
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
	}
	else if( b < 0.5 )
	{
		const Scalar val = 1.0 - 4*(0.5 - b);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi = PI - phi;
	}
	else if( b < 0.75 )
	{
		const Scalar val = 4*(b - 0.5);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi += PI;
	}
	else
	{
		const Scalar val = 1.0 - 4*(1.0 - b);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi = TWO_PI - phi;
	}

	return phi;
}

//! Inverse of SchlickSamplePhi: phi in [0, 2pi) -> b in [0,1).  Exact (the
//! forward warp is monotone and bijective within each of its four
//! quadrants).  Used only by the per-channel branch's density, which has to
//! recover WHICH random pair produced a queried direction in order to know
//! what the OTHER two lanes drew from the same pair.
static inline bool SchlickInvertPhi( const Scalar phi, const Scalar p, Scalar& outB )
{
	if( p < NEARZERO ) {
		return false;
	}

	// t = the quadrant-local value of phi/(pi/2) that the forward warp
	// produced from `val`; invert t = sqrt(p^2 val^2/(1-val^2+val^2 p^2))
	// as val = t/sqrt(p^2 + t^2 (1-p^2)).
	Scalar t = 0;
	int quadrant = 0;
	if( phi <= PI_OV_TWO )          { t = phi / PI_OV_TWO;            quadrant = 0; }
	else if( phi <= PI )            { t = (PI - phi) / PI_OV_TWO;     quadrant = 1; }
	else if( phi <= PI + PI_OV_TWO ){ t = (phi - PI) / PI_OV_TWO;     quadrant = 2; }
	else                            { t = (TWO_PI - phi) / PI_OV_TWO; quadrant = 3; }

	t = r_max( Scalar(0), r_min( Scalar(1), t ) );
	const Scalar sqr_p = p*p;
	const Scalar den = sqr_p + t*t*(1.0 - sqr_p);
	if( den < NEARZERO ) {
		return false;
	}
	const Scalar val = r_max( Scalar(0), r_min( Scalar(1), t / sqrt(den) ) );

	switch( quadrant ) {
		case 0:  outB = val * 0.25;           break;
		case 1:  outB = (val + 1.0) * 0.25;   break;
		case 2:  outB = val * 0.25 + 0.5;     break;
		default: outB = (val + 3.0) * 0.25;   break;
	}
	return true;
}

//! The azimuthal density `GenerateSpecularRay` ACTUALLY draws `phi` from,
//! in the quadrant-local parametrisation `t_q = |phi from this quadrant's
//! axis| / (pi/2)`:  `p_phi(phi) = p^2 / (2 pi (p^2 + t_q^2 (1-p^2))^{3/2})`.
//! Extracted from `ComputeSchlickSpecularPdf` (which still calls it) so
//! that DL-127's `kray` ratio and the density it is a ratio AGAINST cannot
//! drift.  See DL-67 section 4b for why this is not the closed form the
//! old comment claimed.  Returns 0 on the degenerate denominator.
static inline Scalar SchlickAzimuthalDensityFromPhi( const Scalar phi, const Scalar p )
{
	Scalar t;
	if( phi <= PI_OV_TWO )           { t = phi / PI_OV_TWO; }
	else if( phi <= PI )             { t = (PI - phi) / PI_OV_TWO; }
	else if( phi <= PI + PI_OV_TWO ) { t = (phi - PI) / PI_OV_TWO; }
	else                             { t = (TWO_PI - phi) / PI_OV_TWO; }

	const Scalar sqr_p = p*p;
	const Scalar phi_denom = sqr_p + t*t*(1.0 - sqr_p);
	if( phi_denom < NEARZERO ) {
		return 0;
	}
	return sqr_p / (TWO_PI * phi_denom * sqrt(phi_denom));
}

//! The half-vector's azimuth in the SAMPLING frame, measured from
//! `onb.u()` and wrapped into [0, 2pi).  At the pole (h == n) `hu`/`hv`
//! are rounding noise and `atan2(0,0)` is 0, which lands on the on-axis
//! density -- measure zero either way.
static inline Scalar SchlickAzimuthFromH(
		const Vector3& h,
		const OrthonormalBasis3D& onb
		)
{
	Scalar phi = atan2( Vector3Ops::Dot( h, onb.v() ), Vector3Ops::Dot( h, onb.u() ) );
	if( phi < 0 ) {
		phi += TWO_PI;
	}
	return phi;
}

//! Schlick's azimuthal BRDF factor `A(w) = sqrt(p/(p^2 - p^2 w^2 + w^2))`,
//! with `w` the half-vector's tangential direction projected on `onb.v()`
//! -- spelled EXACTLY as `SchlickBRDF`'s `ComputeFactor` spells it, so the
//! SPF's DL-127 `kray` and the BRDF's `value()` agree to rounding.
static inline Scalar SchlickAzimuthFactorA(
		const Vector3& h,
		const Scalar t,												///< [in] Dot(h, onb.w())
		const OrthonormalBasis3D& onb,
		const Scalar p
		)
{
	const Scalar w = Vector3Ops::Dot( onb.v(), Vector3Ops::Normalize( h - (t*onb.w()) ) );
	const Scalar sqr_p = p*p;
	const Scalar sqr_w = w*w;
	const Scalar den = sqr_p - sqr_p*sqr_w + sqr_w;
	if( den < NEARZERO ) {
		return 0;
	}
	return sqrt( p/den );
}

//! DL-127.  The factor the specular lobe's `kray` carries ON TOP of
//! Schlick's sampling weight `S = rho + (1-rho)*fresnel`, so that
//! `kray * p_S == f_S * cos` exactly -- the contract every consumer of a
//! `ScatteredRay` assumes (PT's `PTScatterKray/selectProb` init, BDPT's
//! and VCM's since DL-69, and the MIS partner NEE evaluates through
//! `SchlickBRDF::value`).
//!
//! DL-178: Schlick 1994 Eq.31 adds G(nv)G(nl), G(c)=c/(r+(1-r)c).
//! DL-225: the masking is min(Eq.31, the Smith projected-area bound of
//! Schlick's own Z*A distribution) -- see SchlickMasking.h -- carried
//! through its finite denominators dv = nv/m(nv) and dl = nl/m(nl)
//! (Eq.31's r+(1-r)c wherever Eq.31 is inside the bound).  Dividing
//! f_S cos by p_S cancels Z; roughness remains in the masking:
//! R = A(h.wi) nl / (2 pi t p_phi dv dl),  nl = 2(h.wi)t - nv.
//! Finite at grazing.
static inline Scalar SchlickKrayRatioFromH(
		const Vector3& h,											///< [in] Unit half-vector
		const Vector3& wi,											///< [in] Unit direction toward the viewer
		const OrthonormalBasis3D& onb,								///< [in] Sampling frame (Scatter's `myonb`)
		const Scalar nv,											///< [in] Dot(onb.w(), wi)
		const Scalar r,
		const Scalar p
		)
{
	const Scalar t = Vector3Ops::Dot( h, onb.w() );
	if( t < NEARZERO || nv < NEARZERO ) {
		return 0;
	}
	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	if( hdotk <= 0 ) {
		return 0;
	}
	const Scalar pphi = SchlickAzimuthalDensityFromPhi( SchlickAzimuthFromH( h, onb ), p );
	if( pphi < NEARZERO ) {
		return 0;
	}
	const Scalar nl = 2.0 * hdotk * t - nv;
	if( nl <= 0 ) return 0;
	const Vector3 l = 2.0 * hdotk * h - wi;
	SchlickMasking::Lane lane;
	SchlickMasking::Prepare( lane, r, p );
	const Scalar dv = SchlickMasking::MaskDen( lane, nv,
		Vector3Ops::Dot( wi, onb.u() ), Vector3Ops::Dot( wi, onb.v() ) );
	const Scalar dl = SchlickMasking::MaskDen( lane, nl,
		Vector3Ops::Dot( l, onb.u() ), Vector3Ops::Dot( l, onb.v() ) );
	return SchlickAzimuthFactorA( h, t, onb, p ) * hdotk * nl
		/ ( TWO_PI * t * pphi * dv * dl );
}

//! `SchlickKrayRatioFromH` at a QUERIED outgoing direction.  Recovers `h`
//! the same way `ComputeSchlickSpecularPdf` and `SchlickBRDF::value` do,
//! so all three read one half-vector.
static inline Scalar SchlickKrayRatio(
		const RayIntersectionGeometric& ri,
		const OrthonormalBasis3D& onb,								///< [in] Sampling frame (Scatter's `myonb`)
		const Vector3& woNorm,										///< [in] Unit outgoing direction
		const Scalar r,
		const Scalar p
		)
{
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 h  = Vector3Ops::Normalize( wi + woNorm );
	return SchlickKrayRatioFromH( h, wi, onb, Vector3Ops::Dot( onb.w(), wi ), r, p );
}

//! Schlick's half-vector warp, (xi,b) -> h.  FRAME (DL-100, fixed
//! 2026-09-17): `onb`, the caller's (possibly FlipW'd) SAMPLING frame --
//! NOT `ri.onb`.  Before the fix this always used `ri.onb`, so on a
//! back-face hit (Scatter's `myonb` FlipW'd to face the incoming ray) the
//! half-vector was built around the UNFLIPPED normal: the reflected
//! direction then landed on the wrong side of `myonb.w()`, and
//! Scatter's own accept-check (`dot(dir, myonb.w()) > 0`) rejected every
//! specular draw -- the whole specular lobe silently vanished on a
//! back-face hit, diffuse-only, full diffuse weight.  `Pdf`/`PdfNM` (via
//! `ComputeSchlickSpecularPdf`, `SchlickInvertSpecular`,
//! `SchlickReplaySpecular` and `SchlickDiffuseSelectCoefficient` below)
//! now all take the SAME `onb` and use it consistently, so the sampler
//! and its density agree in whichever frame Scatter actually sampled.
//! See docs/DL100_SCHLICK_BACKFACE_SPECULAR.md.
static inline Vector3 SchlickSampleHalfVector(
		const OrthonormalBasis3D& onb,
		const Point2& random,
		const Scalar r,
		const Scalar p
		)
{
	const Scalar phi = SchlickSamplePhi( random.y, p );
	const Scalar theta = acos(sqrt(random.x/(r-random.x*r+random.x)));

	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	return Vector3(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z,
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z,
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );
}

static void GenerateSpecularRay(
		ScatteredRay& specular,
		Scalar& fresnel,
		const OrthonormalBasis3D& onb,								///< [in] Sampling frame (Scatter's `myonb`, post-FlipW) -- DL-100
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& random,										///< [in] Two random numbers
		const Scalar r,
		const Scalar p
		)
{
	specular.type = ScatteredRay::eRayReflection;

	// Use the warping function to perturb the reflected ray
	const Vector3	h = SchlickSampleHalfVector( onb, random, r, p );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.Dir());

	fresnel = ::pow(1-hdotk,5);

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.Dir() + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );
	}
}

// Compute the Schlick half-vector PDF for a given half-vector direction
// p_h(theta_h, phi_h) = p_theta(theta_h) * p_phi(phi_h)
// where:
//   p_theta(theta_h) = r / (sin^2(theta_h) + r*cos^2(theta_h))^2 * 2*cos(theta_h)*sin(theta_h)
//                     = r / (1 - cos^2(theta_h)*(1-r))^2 * 2*cos(theta_h)*sin(theta_h)
//   (integrated over theta gives 1 when multiplied by sin(theta) for solid angle)
//
//   For the solid angle measure, the half-vector PDF is:
//   D(h) = r / (pi * p * (1 - t^2 + r*t^2)^2) * Z(phi)
//   where t = cos(theta_h) and Z(phi) accounts for anisotropy
//
// The outgoing direction PDF is: D(h) / (4 * dot(wo, h))
static Scalar ComputeSchlickSpecularPdf(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& onb,									///< [in] Sampling frame (Scatter's `myonb`) -- DL-100
	const Vector3& wo,
	const Scalar r,
	const Scalar p
	)
{
	if( r < NEARZERO ) {
		return 0;
	}

	// FRAME (DL-100, fixed 2026-09-17): `onb`, the SAME frame
	// GenerateSpecularRay now samples `h` in (see SchlickSampleHalfVector).
	// Before the fix `GenerateSpecularRay` always used `ri.onb` regardless
	// of the caller's frame, and this function mirrored that by reading
	// `ri.onb` directly -- both now consistently take the caller's `onb`.
	// On a back-face hit `onb` is Scatter's FlipW'd `myonb`, so a `wo` on
	// the correct (post-flip) side recovers a half-vector with `hdotn > 0`
	// and a nonzero density, matching what the fixed sampler now actually
	// emits there.
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Compute half-vector
	Vector3 h = Vector3Ops::Normalize( wi + woNorm );
	const Scalar hdotn = Vector3Ops::Dot( h, onb.w() );
	if( hdotn <= 0 ) {
		return 0;
	}

	const Scalar hdotwo = Vector3Ops::Dot( h, woNorm );
	if( hdotwo <= 0 ) {
		return 0;
	}

	// Compute cos(theta_h) and sin(theta_h) relative to surface normal
	const Scalar cos_theta_h = hdotn;
	const Scalar cos2_theta_h = cos_theta_h * cos_theta_h;
	const Scalar sin2_theta_h = 1.0 - cos2_theta_h;

	// Theta marginal PDF (for solid angle of h):
	// p_theta(theta_h) in solid angle = r / (sin^2 + r*cos^2)^2
	// But we need the full solid angle PDF including the 1/(2*pi) or anisotropic phi factor.
	//
	// The theta CDF inverts: cos^2(theta) = xi / (r + xi*(1-r))
	// so p(xi) = 1, and dxi/d(cos^2(theta)) = r / (1 - cos^2(theta) + r*cos^2(theta))^2
	// p(cos^2(theta)) = r / (sin^2(theta) + r*cos^2(theta))^2
	// p(theta) = 2*cos(theta)*sin(theta) * r / (sin^2(theta) + r*cos^2(theta))^2

	const Scalar denom_theta = sin2_theta_h + r * cos2_theta_h;
	const Scalar denom_theta_sq = denom_theta * denom_theta;
	if( denom_theta_sq < NEARZERO ) {
		return 0;
	}

	// The theta PDF (for the cos^2 variable) = r / denom^2
	// To get the solid angle measure PDF for h:
	// p(h) = p_theta(theta_h) * p_phi(phi_h) / sin(theta_h)
	// where p_theta(theta_h) = 2*cos*sin * r / denom^2
	// and p_phi(phi_h) is the azimuthal PDF

	// For the azimuthal part with anisotropy parameter p.
	//
	// This is NOT `p/(2 pi (cos^2 phi + p^2 sin^2 phi))`, which is what this
	// helper used to return and what its comment claimed Schlick's warp
	// samples.  It does not: GenerateSpecularRay draws
	//   phi = (pi/2) * sqrt(p^2 v^2 / (1 - v^2 + v^2 p^2)),  v ~ U[0,1)
	// per quadrant, i.e. with t := phi/(pi/2) in [0,1] measured from the
	// quadrant's own axis, v = t/sqrt(p^2 + t^2(1-p^2)), so
	//   p(phi) = |db/dphi| = (1/4)|dv/dt|(2/pi)
	//          = p^2 / (2 pi (p^2 + t^2 (1-p^2))^(3/2)).
	// Both forms integrate to 1 over [0,2pi) and both agree at p=1, which is
	// why this went unnoticed; away from p=1 they are different
	// distributions, off by up to 25x at p=0.3 (measured against a 2e6-draw
	// histogram of the real sampler -- see
	// docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md section 4b).  The corrected
	// form below reproduces that histogram to MC noise at p = 1, .8, .6, .3.
	// Quadrant-local t = |phi measured from this quadrant's axis|/(pi/2);
	// see SchlickAzimuthalDensityFromPhi, which DL-127's kray ratio shares
	// with this function so the two cannot drift.
	const Scalar phi_pdf = SchlickAzimuthalDensityFromPhi( SchlickAzimuthFromH( h, onb ), p );
	if( phi_pdf <= 0 ) {
		return 0;
	}

	// Full half-vector PDF in solid angle measure:
	// p(h) = [2*cos(theta_h)*sin(theta_h) * r / denom^2] * phi_pdf / sin(theta_h)
	//       = 2 * cos(theta_h) * r / denom^2 * phi_pdf
	const Scalar h_pdf = 2.0 * cos_theta_h * r / denom_theta_sq * phi_pdf;

	// Convert from half-vector PDF to outgoing direction PDF
	// p(wo) = p(h) / (4 * dot(wo, h))
	const Scalar pdf = h_pdf / (4.0 * hdotwo);

	return pdf;
}

// ===================================================================
//  The aggregate-density machinery (DL-67 Slice 0).
//
//  Scatter() is a "draw every lobe, then pick one by its REALIZED
//  weight" sampler, so the density of the direction the integrator
//  finally continues along is NOT a fixed mixture of the lobe pdfs.
//  Writing p_D / p_i for the diffuse / i-th specular sampling density,
//  w_D = MaxValue(rd) for the diffuse ray's (direction-INDEPENDENT)
//  realized selection weight and w_i(omega) = rho_i + (1-rho_i)*
//  fresnel(omega), multiplied by the DL-178/DL-225 geometric ratio R, for
//  the i-th specular ray's (direction-DEPENDENT)
//  one, the density of the selected direction is
//
//      f(omega) = C_D * p_D(omega) * 1{omega above the horizon}
//               + sum_i q_i(omega) * p_i(omega)
//
//  with
//
//      C_D = E_{(u,v)~U[0,1]^2}[ P(the diffuse ray wins | that draw) ]
//      q_i(omega) = P(specular lane i wins | lane i drew omega).
//
//  Both expectations run over the OTHER lobes' draws, because
//  RandomlySelect's denominator contains them.  Three consequences the
//  pre-DL-67 code and its first fix both got wrong:
//
//  (a) C_D is NOT w_D/(w_D + E[w_S]).  The relevant measure is p_S,
//      which concentrates near the mirror direction where the Fresnel
//      term is ~0 -- so E_{p_S}[w_S] is within 0.15% of rho itself, and
//      what actually drives C_D is the specular sampler's REJECTION
//      rate: a rejected specular ray leaves the container with one ray,
//      and RandomlySelect then returns it with probability 1 regardless
//      of weight.  Measured int p_S over the accepted region is 0.745 /
//      0.662 / 0.577 at 30 / 60 / 80 degrees incidence, so a third or
//      more of all draws are "diffuse wins outright".  No closed-form
//      hemispherical-average proxy can see that.
//
//  (b) The only honest way to get C_D is therefore to integrate the
//      sampler itself.  SchlickDiffuseSelectCoefficient does that with
//      a DETERMINISTIC stratified quadrature in GenerateSpecularRay's
//      own (xi, b) inverse-CDF parametrisation -- deterministic so that
//      Pdf() stays a pure function of its arguments (an MIS weight that
//      wobbled per call would not partition to one).
//
//  (c) With the exact C_D, `int f = 1` identically whenever the diffuse
//      ray is always accepted, because for every specular draw the two
//      conditional probabilities sum to 1 by construction.  Under a
//      tilted shading normal the diffuse ray is itself sometimes
//      rejected (Scatter's geometric-horizon gate), and then the
//      SPECULAR side has to average over that: q_i picks up a
//      "(1 - A_D) * (specular wins outright)" term, where A_D is the
//      exact cosine-hemisphere clipped fraction (1+cos phi)/2.  `int f`
//      is then P(Scatter emits anything at all) < 1 -- which is correct,
//      not a defect: Scatter really does return an empty container on
//      those draws.
//
//  Full derivation and measurements: docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md
// ===================================================================

//! Replay ONE (xi,b) draw of GenerateSpecularRay for one lane, and report
//! what Scatter() would have done with it.  Shares SchlickSampleHalfVector
//! with the sampler so the two cannot drift.
static inline bool SchlickReplaySpecular(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Point2& random,
	const Scalar r,
	const Scalar p,
	Scalar& outFresnel,
	Scalar& outKrayRatio											///< [out] DL-127 ratio at the replayed direction (0 if rejected)
	)
{
	outKrayRatio = 0;

	const Vector3 h = SchlickSampleHalfVector( myonb, random, r, p );
	const Scalar hdotk = Vector3Ops::Dot( h, -ri.ray.Dir() );
	outFresnel = ::pow(1-hdotk,5);

	if( hdotk <= 0 ) {
		// GenerateSpecularRay leaves the ray untouched; Scatter's
		// accept-check then rejects it.  (In the per-channel branch the
		// untouched ray is the PREVIOUS lane's -- see DL-101; this
		// replay models the intended semantics, not that aliasing.)
		return false;
	}

	const Vector3 dir = Vector3Ops::Normalize( ri.ray.Dir() + 2.0 * hdotk * h );
	if( Vector3Ops::Dot( dir, myonb.w() ) <= 0.0 || Vector3Ops::Dot( dir, geomN ) <= 0.0 ) {
		return false;
	}

	// DL-127: this lane's realized selection weight is `S * ratio`.
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	outKrayRatio = SchlickKrayRatioFromH( h, wi, myonb, Vector3Ops::Dot( myonb.w(), wi ), r, p );
	return true;
}

//! Invert the specular sampler at a queried direction: recover the (xi,b)
//! pair GenerateSpecularRay must have drawn for lane `r,p` to produce
//! `woNorm`.  Only the multi-lane (per-channel) density needs this -- it is
//! how a query direction tells us what the OTHER lanes drew from the same
//! shared random pair.
static bool SchlickInvertSpecular(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& onb,									///< [in] Sampling frame (Scatter's `myonb`) -- DL-100
	const Vector3& woNorm,
	const Scalar r,
	const Scalar p,
	Point2& outRandom
	)
{
	if( r < NEARZERO ) {
		return false;
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 h = Vector3Ops::Normalize( wi + woNorm );

	// Project into the frame the sampler builds h in -- `onb` (Scatter's
	// `myonb`), post-DL-100 -- see SchlickSampleHalfVector's note.
	const Scalar hz = Vector3Ops::Dot( h, onb.w() );
	if( hz <= 0 ) {
		return false;
	}
	const Scalar hx = Vector3Ops::Dot( h, onb.u() );
	const Scalar hy = Vector3Ops::Dot( h, onb.v() );

	// cos^2(theta) = xi/(r + xi(1-r))  =>  xi = c2*r/(1 - c2 + c2*r)
	const Scalar c2 = r_min( Scalar(1), hz*hz );
	const Scalar den = 1.0 - c2 + c2*r;
	if( den < NEARZERO ) {
		return false;
	}
	const Scalar xi = r_max( Scalar(0), r_min( Scalar(1), c2*r/den ) );

	Scalar phi = atan2( hy, hx );
	if( phi < 0 ) {
		phi += TWO_PI;
	}
	Scalar b = 0;
	if( !SchlickInvertPhi( phi, p, b ) ) {
		return false;
	}

	outRandom = Point2( xi, b );
	return true;
}

//! Per-lane quadrature rows.  The (xi,b) grid is a PRODUCT grid, so each
//! lane's kSpecQuadN half-angle values and kSpecQuadN azimuth values are
//! evaluated ONCE per Pdf() call rather than kSpecQuadN^2 times -- that is
//! what keeps every transcendental out of the inner loop, which then costs
//! ~40 flops and no library calls at all.
struct SchlickQuadRows
{
	Scalar cosT[3][kSpecQuadN];
	Scalar sinT[3][kSpecQuadN];
	Scalar cosP[3][kSpecQuadN];
	Scalar sinP[3][kSpecQuadN];
	//! DL-127: `A(phi_b) / (2 pi p_phi(phi_b))` -- the azimuth-only part of
	//! `SchlickKrayRatioFromH`.  Node-exact, so the replay prices the
	//! specular lane's realized selection weight with the SAME `kray`
	//! `Scatter` now emits, at two multiplies and a divide in the inner
	//! loop instead of an `atan2` and two `sqrt`s.
	Scalar azim[3][kSpecQuadN];
};

static void SchlickBuildQuadRows( const SchlickLobeSet& lobes, SchlickQuadRows& rows )
{
	const Scalar inv = 1.0 / Scalar(kSpecQuadN);

	// Plain stratified midpoints in the sampler's OWN (xi,b) unit square.
	// Warping the xi axis (uniform in cos(theta_h), or in theta_h) was tried
	// and measured WORSE: it buys resolution at the tangent end by starving
	// the near-mirror end, which carries most of the probability mass at low
	// roughness -- see docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md section 4a.
	//
	// The three per-channel lanes share ONE random pair, so they must share
	// the nodes too; only their own (r, p) differ.
	for( int j = 0; j < lobes.count; j++ ) {
		const Scalar rj = lobes.r[j];
		for( int a = 0; a < kSpecQuadN; a++ ) {
			const Scalar xi = (Scalar(a) + 0.5) * inv;
			const Scalar den = rj - xi*rj + xi;
			// cos(acos(x)) == x: GenerateSpecularRay's own theta warp with
			// the round trip through acos removed.
			Scalar c = ( den > NEARZERO ) ? sqrt( xi/den ) : Scalar(1);
			c = r_min( Scalar(1), c );
			rows.cosT[j][a] = c;
			rows.sinT[j][a] = sqrt( r_max( Scalar(0), 1.0 - c*c ) );
		}
		const Scalar pj = lobes.p[j];
		for( int b = 0; b < kSpecQuadN; b++ ) {
			const Scalar phi = SchlickSamplePhi( (Scalar(b) + 0.5) * inv, pj );
			rows.cosP[j][b] = cos(phi);
			rows.sinP[j][b] = sin(phi);

			// DL-127.  `A`'s argument `w` is the half-vector's tangential
			// direction projected on `onb.v()`; for this node the local
			// half-vector is `(cos phi sin theta, sin phi sin theta,
			// cos theta)` with `sin theta >= 0`, so `w` is exactly
			// `sin phi` -- no normalize, no dot products.
			const Scalar sw = rows.sinP[j][b];
			const Scalar aden = pj*pj - pj*pj*sw*sw + sw*sw;
			const Scalar A = ( aden > NEARZERO ) ? sqrt( pj/aden ) : Scalar(0);
			const Scalar pphi = SchlickAzimuthalDensityFromPhi( phi, pj );
			rows.azim[j][b] = ( pphi > NEARZERO ) ? ( A / (TWO_PI*pphi) ) : Scalar(0);
		}
	}
}

//! C_D -- the constant coefficient the diffuse sampling density carries in
//! the aggregate.  kSpecQuadN^2 deterministic stratified replays of the
//! specular sampler; see the block comment above for why no closed form
//! exists.
static Scalar SchlickDiffuseSelectCoefficient(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Scalar wD,
	const SchlickLobeSet& lobes
	)
{
	SchlickQuadRows rows;
	SchlickBuildQuadRows( lobes, rows );

	// FRAME (DL-100, fixed 2026-09-17): `myonb`, the SAME frame
	// GenerateSpecularRay now samples `h` in -- this replay has to build
	// `h` the same way the sampler does or it isn't replaying it at all.
	// Before the fix these were `ri.onb`, matching the sampler's
	// pre-DL-100 (buggy) unconditional-`ri.onb` behaviour.
	const Vector3& eu = myonb.u();
	const Vector3& ev = myonb.v();
	const Vector3& ew = myonb.w();
	const Vector3& d  = ri.ray.Dir();
	const Vector3& nW = myonb.w();

	// DL-127: the realized selection weight is now `S * ratio`, and
	// DL-178 cancels nv against G(nv).  DL-225: the masking is the
	// bounded one (SchlickMasking.h); its denominator nv/m(nv) is
	// constant over the quadrature, while nl/m(nl) depends on each
	// accepted direction, whose tangential components are 2(h.v)h - v
	// in the local frame.
	const Vector3 wiView = Vector3Ops::Normalize( -d );
	const Scalar  nvView = Vector3Ops::Dot( nW, wiView );
	const Scalar  vxView = Vector3Ops::Dot( eu, wiView );
	const Scalar  vyView = Vector3Ops::Dot( ev, wiView );
	SchlickMasking::Lane lanes[3];
	Scalar denView[3] = { 1, 1, 1 };
	Scalar slowBelow[3] = { 0, 0, 0 };		// nl below this may need the bounded branch
	for( int j = 0; j < lobes.count; j++ ) {
		SchlickMasking::Prepare( lanes[j], lobes.r[j], lobes.p[j] );
		denView[j] = SchlickMasking::MaskDen( lanes[j], nvView, vxView, vyView );
		slowBelow[j] = lanes[j].bounded ? lanes[j].cFast : Scalar(0);
	}

	Scalar accum = 0;

	for( int a = 0; a < kSpecQuadN; a++ ) {
		for( int bIdx = 0; bIdx < kSpecQuadN; bIdx++ ) {
			Scalar wS = 0;
			int nAcceptedSpec = 0;

			for( int j = 0; j < lobes.count; j++ ) {
				const Scalar st = rows.sinT[j][a];
				const Scalar ax = rows.cosP[j][bIdx] * st;
				const Scalar ay = rows.sinP[j][bIdx] * st;
				const Scalar az = rows.cosT[j][a];

				const Scalar hx = eu.x*ax + ev.x*ay + ew.x*az;
				const Scalar hy = eu.y*ax + ev.y*ay + ew.y*az;
				const Scalar hz = eu.z*ax + ev.z*ay + ew.z*az;

				const Scalar hdotk = -( hx*d.x + hy*d.y + hz*d.z );
				if( hdotk <= 0 ) {
					// GenerateSpecularRay leaves the ray untouched and
					// Scatter's accept-check drops it.
					continue;
				}

				// Only the SIGNS of the two accept dots matter, and
				// normalizing a vector cannot change a sign -- so the
				// replay skips Scatter's Normalize entirely.
				const Scalar k = 2.0 * hdotk;
				const Scalar dx = d.x + k*hx;
				const Scalar dy = d.y + k*hy;
				const Scalar dz = d.z + k*hz;
				if( dx*nW.x + dy*nW.y + dz*nW.z <= 0 ) {
					continue;
				}
				if( dx*geomN.x + dy*geomN.y + dz*geomN.z <= 0 ) {
					continue;
				}

				// (1-hdotk)^5, spelled as multiplies.
				const Scalar t = 1.0 - hdotk;
				const Scalar t2 = t*t;
				const Scalar fresnel = t2*t2*t;

				// DL-127: `kray = S * ratio`, and `RandomlySelect` reads
				// `MaxValue(kray) = ratio * (rho_max + (1-rho_max) F)`
				// because `ratio` is a nonnegative scalar.
				const Scalar nl = 2.0 * hdotk * az - nvView;
				Scalar ratio = 0;
				if( az > NEARZERO && nvView > NEARZERO && nl > 0 ) {
					// SchlickMasking::MaskDen, with its cFast test hoisted
					// per lane: above it the denominator is Eq.31's.
					const Scalar r = lobes.r[j];
					const Scalar den31 = r + (1.0 - r) * nl;
					const Scalar denL = ( nl < slowBelow[j] )
						? SchlickMasking::MaskDenSlow( lanes[j], nl, k*ax - vxView, k*ay - vyView, den31 )
						: den31;
					ratio = rows.azim[j][bIdx] * hdotk * nl / ( az * denView[j] * denL );
				}

				wS += (lobes.rho[j] + (1.0 - lobes.rho[j]) * fresnel) * ratio;
				nAcceptedSpec++;
			}

			if( nAcceptedSpec == 0 ) {
				// RandomlySelect's freeidx==1 short-circuit: the lone
				// diffuse ray is returned whatever its weight.
				accum += 1.0;
				continue;
			}

			const Scalar total = wD + wS;
			if( total > NEARZERO ) {
				accum += wD / total;
			}
			// else RandomlySelect returns nothing at all -- contributes 0.
		}
	}

	return accum / Scalar(kSpecQuadN*kSpecQuadN);
}

//! sum_i q_i(omega) * p_i(omega) -- the specular half of the aggregate.
//! `aD` is the probability Scatter's diffuse ray survived its own
//! geometric-horizon gate (1 whenever the shading and geometric normals
//! agree).
static Scalar SchlickSpecularDensity(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Vector3& woNorm,
	const SchlickDiffuseDraw& diffuse,
	const SchlickLobeSet& lobes
	)
{
	const Scalar aD = diffuse.aD;
	const Scalar wD = diffuse.W0;
	// fresnel at the query direction.  GenerateSpecularRay reflects the
	// incoming ray d about its sampled half-vector h, so wo = d - 2(d.h)h
	// and, with wi=-d, wi+wo = 2(h.wi)h: for every direction the sampler
	// could have emitted (h.wi>0 is its own accept condition),
	// normalize(wi+wo) recovers exactly that h -- and therefore exactly
	// the fresnel GenerateSpecularRay computed.  No approximation.
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 h = Vector3Ops::Normalize( wi + woNorm );
	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	const Scalar fresnelAtWo = ::pow(1-hdotk,5);
	const Scalar nv = Vector3Ops::Dot( myonb.w(), wi );

	Scalar sum = 0;

	for( int i = 0; i < lobes.count; i++ ) {
		const Scalar pdf_i = ComputeSchlickSpecularPdf( ri, myonb, woNorm, lobes.r[i], lobes.p[i] );
		if( pdf_i <= 0 ) {
			continue;
		}

		// DL-127: `RandomlySelect` weights by `MaxValue(kray)`, and since
		// the fix `kray = S * ratio` -- so the realized weight carries the
		// same ratio `Scatter` now stamps on the lobe.
		const Scalar w_i = (lobes.rho[i] + (1.0 - lobes.rho[i]) * fresnelAtWo)
		                 * SchlickKrayRatioFromH( h, wi, myonb, nv, lobes.r[i], lobes.p[i] );

		// What the OTHER lanes drew.  They share lane i's random pair, so
		// they are a deterministic function of the query direction -- but
		// only the multi-lane branch has any.
		Scalar wOther = 0;
		int nAcceptedSpec = 1;
		if( lobes.count > 1 ) {
			Point2 random;
			if( SchlickInvertSpecular( ri, myonb, woNorm, lobes.r[i], lobes.p[i], random ) ) {
				for( int j = 0; j < lobes.count; j++ ) {
					if( j == i ) {
						continue;
					}
					Scalar fresnel = 0;
					Scalar otherRatio = 0;
					if( SchlickReplaySpecular( ri, myonb, geomN, random, lobes.r[j], lobes.p[j], fresnel, otherRatio ) ) {
						wOther += (lobes.rho[j] + (1.0 - lobes.rho[j]) * fresnel) * otherRatio;
						nAcceptedSpec++;
					}
				}
			}
		}

		const Scalar wS = w_i + wOther;

		Scalar q = 0;
		// ...with the diffuse ray present (probability aD).
		const Scalar totalWith = wD + wS;
		const Scalar g0 = ( totalWith > NEARZERO ) ? ( w_i / totalWith ) : Scalar(0);
		q += aD * g0;
		// DL-310: minus/plus where the drawn diffuse direction clips the
		// diffuse weight below W0 (see SchlickDiffuseDraw).
		for( int k = 0; k < diffuse.band.n; k++ ) {
			Scalar wDk = diffuse.band.W[k][0];
			for( int c = 1; c < diffuse.count; c++ ) {
				wDk = r_max( wDk, diffuse.band.W[k][c] );
			}
			const Scalar totalK = wDk + wS;
			const Scalar gk = ( totalK > NEARZERO ) ? ( w_i / totalK ) : Scalar(0);
			q += diffuse.band.weight[k] * ( gk - g0 );
		}
		// ...and without it (probability 1-aD), where a lone specular ray
		// wins outright through RandomlySelect's freeidx==1 short-circuit.
		if( aD < 1.0 ) {
			if( nAcceptedSpec == 1 ) {
				q += (1.0 - aD);
			} else if( wS > NEARZERO ) {
				q += (1.0 - aD) * (w_i / wS);
			}
		}

		sum += q * pdf_i;
	}

	return sum;
}

//! Probability Scatter's diffuse ray survives its geometric-horizon gate:
//! the exact fraction of a cosine-weighted hemisphere about `n` that lies
//! above the plane of `geomN`.  Malley's disk projection turns the clipped
//! region into a half-disk plus a half-ellipse of semi-axes (cos phi, 1),
//! giving (1 + cos phi)/2 -- the same closed form DL-45 uses for
//! TranslucentSPF's tilted exit.
static inline Scalar SchlickDiffuseAcceptFraction(
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN
	)
{
	const Scalar c = Vector3Ops::Dot( myonb.w(), geomN );
	return r_max( Scalar(0), r_min( Scalar(1), 0.5 * (1.0 + c) ) );
}

//! DL-310: fills `draw` for the channels `count` (3 RGB lanes, or 1 for
//! the spectral path) -- see SchlickDiffuseDraw.  `lanes[c]`, `rho[c]`,
//! `rd[c]` are channel c's own lane, specular and diffuse reflectance.
static void BuildSchlickDiffuseDraw(
	const SchlickDirectionalAlbedo::Lane* lanes,
	const Scalar* rho,
	const Scalar* rd,
	const int count,
	const Scalar muI,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	SchlickDiffuseDraw& draw
	)
{
	draw.aD = SchlickDiffuseAcceptFraction( myonb, geomN );
	draw.count = count;
	SchlickDirectionalAlbedo::LaneRow rows[3];
	SchlickDirectionalAlbedo::DiffuseChannels dc;
	dc.count = count;
	Scalar W0 = 0;
	int rowOf[3] = { -1, -1, -1 };	// channel sharing this channel's lane, if its row is built
	for( int c = 0; c < count; c++ ) {
		dc.rho[c] = rho[c];
		dc.active[c] = SchlickDirectionalAlbedo::CanClip( lanes[c], rho[c], rd[c] );
		if( dc.active[c] ) {
			double a0, a5;
			SchlickDirectionalAlbedo::Moments( lanes[c], muI, a0, a5 );
			dc.K[c] = r_max( Scalar(0), r_min( rd[c], Scalar(1) - SchlickDirectionalAlbedo::Albedo( rho[c], a0, a5 ) ) );
			// Channels on the same lane share one row (and one evaluation
			// of it per quadrature node -- see ChannelWeights).
			for( int prev = 0; prev < c; prev++ ) {
				if( rowOf[prev] >= 0 && lanes[prev].base == lanes[c].base && lanes[prev].fr == lanes[c].fr
				    && lanes[prev].fp == lanes[c].fp && lanes[prev].belowR == lanes[c].belowR
				    && lanes[prev].specular == lanes[c].specular ) {
					rowOf[c] = rowOf[prev];
					break;
				}
			}
			if( rowOf[c] < 0 ) {
				rowOf[c] = c;
				SchlickDirectionalAlbedo::BuildRow( lanes[c], rows[c] );
			}
			dc.row[c] = &rows[rowOf[c]];
		} else {
			dc.K[c] = rd[c];
			dc.row[c] = &rows[c];
		}
		W0 = ( c == 0 ) ? dc.K[c] : r_max( W0, dc.K[c] );
	}
	draw.W0 = W0;
	const Scalar gz = Vector3Ops::Dot( geomN, myonb.w() );
	const Scalar gu = Vector3Ops::Dot( geomN, myonb.u() ), gv = Vector3Ops::Dot( geomN, myonb.v() );
	SchlickDirectionalAlbedo::BuildBand( dc, gz, sqrt( gu*gu + gv*gv ), draw.band );
}

void SchlickSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a direction that validates against
	// the (tilted) shading normal can still point below the geometric
	// surface -- the continuation ray then tunnels into the solid.  Oriented
	// to myonb.w() (the normal the lobes are sampled around).  Degenerate
	// vGeomNormal (SquaredModulus guard, matches GlintModifier.cpp) falls
	// back to the shading normal, making the gate a no-op.
	// (ray-anchor sweep: geomN's orientation is anchored to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot flip the gate to the wrong side.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	ScalarTriple rt = pRoughness->GetValuesAt(ri);
	const ScalarTriple it = pIsotropy->GetValuesAt(ri);

	// Glossy filtering: increase effective roughness to blur
	// secondary glossy reflections, reducing caustic noise.
	if( ri.glossyFilterWidth > 0 ) {
		for( int ch = 0; ch < 3; ch++ ) {
			rt.v[ch] = r_min( rt.v[ch] + ri.glossyFilterWidth, Scalar(1.0) );
		}
	}

	ScatteredRay d;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );

	// Accept-check tests myonb.w() (the frame lobes are actually sampled
	// around, post-FlipW) rather than the raw ri.onb.w() -- on a back-face
	// hit the two differ by sign, and testing the unflipped normal here
	// silently dropped every legitimately-sampled back-face lobe.
	if( Vector3Ops::Dot( d.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( d.ray.Dir(), geomN ) > 0.0 ) {
		// Cosine-weighted hemisphere PDF
		const Scalar cosTheta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		// DL-310: the coupled diffuse's own f_D cos / p_D, the SAME
		// function SchlickBRDF::value evaluates.
		SchlickDirectionalAlbedo::Lane lanes[3];
		SchlickDiffuseLanes( rt, it, lanes );
		const Scalar muI = Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( -ri.ray.Dir() ) );
		d.kray = SchlickCoupledDiffuseKray( lanes, pSpecular->GetColor(ri), pDiffuse->GetColor(ri), muI, cosTheta );
		d.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		d.isDelta = false;
		scattered.AddScatteredRay( d );
	}

	if( !pRoughness->HasPerChannelVariation() && !pIsotropy->HasPerChannelVariation() )
	{
		ScatteredRay s;
		Scalar fresnel = 0;
		GenerateSpecularRay( s, fresnel, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), rt.v[0], it.v[0] );

		// Accept-check uses myonb.w() -- see the diffuse-lobe comment above.
		if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
			const RISEPel rho = pSpecular->GetColor(ri);
			const Vector3 woNorm = Vector3Ops::Normalize( s.ray.Dir() );
			// DL-127: `kray` is this lobe's `f_S cos / p_S`, not Schlick's
			// bare sampling weight -- see SchlickKrayRatioFromH.
			const Scalar krayRatio = SchlickKrayRatio( ri, myonb, woNorm, rt.v[0], it.v[0] );
			s.kray = (rho + (RISEPel(1.0,1.0,1.0)-rho) * fresnel) * krayRatio;
			s.pdf = ComputeSchlickSpecularPdf( ri, myonb, woNorm, rt.v[0], it.v[0] );
			s.isDelta = false;
			scattered.AddScatteredRay( s );
		}
	}
	else
	{
		const Point2 ptrand( sampler.Get1D(),sampler.Get1D() );
		const RISEPel rho = pSpecular->GetColor(ri);

		for( int i=0; i<3; i++ ) {
			// DL-101: a FRESH ScatteredRay every iteration.  Before this
			// fix, `s` was declared ONCE outside the loop and reused
			// across all three lanes; GenerateSpecularRay only writes
			// `s.ray` when `hdotk > 0`, so a lane that fails that check
			// left the PREVIOUS lane's direction sitting in `s.ray` while
			// this branch still overwrote `s.kray`/`s.pdf` with the
			// CURRENT lane's values and pushed it -- a ray reaching the
			// integrator at lane j's direction, priced as lane i.  See
			// docs/DL101_PERCHANNEL_SCATTEREDRAY_REUSE.md.
			ScatteredRay s;
			Scalar fresnel = 0;
			GenerateSpecularRay( s, fresnel, myonb, ri, ptrand, rt.v[i], it.v[i] );

			// Accept-check uses myonb.w() -- see the diffuse-lobe comment above.
			if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
				const Vector3 woNorm = Vector3Ops::Normalize( s.ray.Dir() );
				// DL-127, per lane: this lane's own (r,p) give its own
				// density, so its own ratio.
				const Scalar krayRatio = SchlickKrayRatio( ri, myonb, woNorm, rt.v[i], it.v[i] );
				s.kray = 0;
				s.kray[i] = (rho[i] + (1.0-rho[i]) * fresnel) * krayRatio;
				s.pdf = ComputeSchlickSpecularPdf( ri, myonb, woNorm, rt.v[i], it.v[i] );
				s.isDelta = false;
				scattered.AddScatteredRay( s );
			}
		}
	}
}

void SchlickSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a direction that validates against
	// the (tilted) shading normal can still point below the geometric
	// surface -- the continuation ray then tunnels into the solid.  Oriented
	// to myonb.w() (the normal the lobes are sampled around).  Degenerate
	// vGeomNormal (SquaredModulus guard, matches GlintModifier.cpp) falls
	// back to the shading normal, making the gate a no-op.
	// (ray-anchor sweep: geomN's orientation is anchored to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot flip the gate to the wrong side.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	ScatteredRay d, s;
	Scalar fresnel = 0;
	Scalar roughnessNM = pRoughness->GetValueAtNM(ri,nm);
	const Scalar isotropyNM = pIsotropy->GetValueAtNM(ri,nm);

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}

	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );
	GenerateSpecularRay( s, fresnel, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), roughnessNM, isotropyNM );

	// Accept-checks use myonb.w() -- see Scatter()'s comment: it is the
	// frame lobes are actually sampled around (post-FlipW), not the raw
	// ri.onb.w() which differs by sign on a back-face hit.
	if( Vector3Ops::Dot( d.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( d.ray.Dir(), geomN ) > 0.0 ) {
		const Scalar cosTheta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		// DL-310: coupled diffuse, spectral twin of Scatter's.
		SchlickDirectionalAlbedo::Lane lane;
		SchlickDirectionalAlbedo::PrepareLane( lane, roughnessNM, isotropyNM );
		d.krayNM = SchlickDirectionalAlbedo::CoupledDiffuseAt( lane, GuardedGetColorNM( *pSpecular, ri, nm ),
			GuardedGetColorNM( *pDiffuse, ri, nm ),
			Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( -ri.ray.Dir() ) ), cosTheta );
		d.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		d.isDelta = false;
		scattered.AddScatteredRay( d );
	}

	if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
		const Scalar rho = GuardedGetColorNM( *pSpecular, ri, nm );
		const Vector3 woNorm = Vector3Ops::Normalize( s.ray.Dir() );
		// DL-127, spectral twin of Scatter's single-lane branch above.
		const Scalar krayRatio = SchlickKrayRatio( ri, myonb, woNorm, roughnessNM, isotropyNM );
		s.krayNM = (rho + (1.0-rho) * fresnel) * krayRatio;
		s.pdf = ComputeSchlickSpecularPdf( ri, myonb, woNorm, roughnessNM, isotropyNM );
		s.isDelta = false;
		scattered.AddScatteredRay( s );
	}
}

Scalar SchlickSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, myonb.w() );
	if( cosTheta <= 0 ) {
		return 0;
	}

	// Geometric-horizon gate (MIS consistency with Scatter's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}

	// Diffuse PDF: cosine-weighted hemisphere
	const Scalar diffusePdf = cosTheta * INV_PI;

	// The lanes Scatter() would have emitted.  Branch on the SAME predicate
	// Scatter() branches on, and use each lane's own (r,p) rather than a
	// channel average -- a lane's sampling density has to be the density of
	// what that lane actually draws.
	ScalarTriple roughness = pRoughness->GetValuesAt(ri);
	const ScalarTriple isotropy = pIsotropy->GetValuesAt(ri);
	if( ri.glossyFilterWidth > 0 ) {
		for( int ch = 0; ch < 3; ch++ ) {
			roughness.v[ch] = r_min( roughness.v[ch] + ri.glossyFilterWidth, Scalar(1.0) );
		}
	}

	const RISEPel rd = pDiffuse->GetColor(ri);
	const RISEPel rho = pSpecular->GetColor(ri);

	// DL-310: the diffuse ray's realized weight is its coupled kray at
	// THIS direction (MaxValue(Rd) wherever no channel can clip).
	SchlickDirectionalAlbedo::Lane dLanes[3];
	SchlickDiffuseLanes( roughness, isotropy, dLanes );
	const Scalar muI = Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( -ri.ray.Dir() ) );
	const Scalar wD = ColorMath::MaxValue( SchlickCoupledDiffuseKray( dLanes, rho, rd, muI, cosTheta ) );
	SchlickDiffuseDraw draw;
	{
		const Scalar rhoC[3] = { rho[0], rho[1], rho[2] };
		const Scalar rdC[3]  = { rd[0], rd[1], rd[2] };
		BuildSchlickDiffuseDraw( dLanes, rhoC, rdC, 3, muI, myonb, geomN, draw );
	}

	SchlickLobeSet lobes;
	if( !pRoughness->HasPerChannelVariation() && !pIsotropy->HasPerChannelVariation() ) {
		lobes.count  = 1;
		lobes.r[0]   = roughness.v[0];
		lobes.p[0]   = isotropy.v[0];
		// MaxValue(rho + (1-rho)*F) == MaxValue(rho) + (1-MaxValue(rho))*F
		// exactly, for any F in [0,1]: the per-channel difference
		// (rho_i - rho_j)(1 - F) keeps its sign, so the channel that
		// maximises rho maximises the boosted value too.  Since DL-127
		// the realized kray is that boosted value times a NONNEGATIVE
		// SCALAR ratio, which factors straight out of MaxValue -- the
		// two consumers below (SchlickDiffuseSelectCoefficient and
		// SchlickSpecularDensity) multiply it back in themselves.
		lobes.rho[0] = ColorMath::MaxValue(rho);
	} else {
		lobes.count = 3;
		for( int i = 0; i < 3; i++ ) {
			lobes.r[i]   = roughness.v[i];
			lobes.p[i]   = isotropy.v[i];
			// The per-channel lane's kray is zero outside channel i, so
			// its MaxValue is that one channel's boosted reflectance.
			lobes.rho[i] = rho[i];
		}
	}

	const Scalar cD = SchlickDiffuseSelectCoefficient( ri, myonb, geomN, wD, lobes );

	return cD * diffusePdf
	     + SchlickSpecularDensity( ri, myonb, geomN, woNorm, draw, lobes );
}

Scalar SchlickSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, myonb.w() );
	if( cosTheta <= 0 ) {
		return 0;
	}

	// Geometric-horizon gate (MIS consistency with ScatterNM's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}

	// Diffuse PDF
	const Scalar diffusePdf = cosTheta * INV_PI;

	// Spectral twin of Pdf() above; see its comments and the block comment
	// on SchlickDiffuseSelectCoefficient for the derivation.  ScatterNM has
	// no per-channel branch, so there is always exactly one specular lane.
	Scalar r = pRoughness->GetValueAtNM(ri,nm);
	const Scalar p = pIsotropy->GetValueAtNM(ri,nm);
	if( ri.glossyFilterWidth > 0 ) {
		r = r_min( r + ri.glossyFilterWidth, Scalar(1.0) );
	}

	SchlickLobeSet lobes;
	lobes.count  = 1;
	lobes.r[0]   = r;
	lobes.p[0]   = p;
	lobes.rho[0] = GuardedGetColorNM( *pSpecular, ri, nm );

	// DL-310: coupled diffuse weight at this direction, and the diffuse
	// draw's description for the specular coefficient (spectral twin).
	const Scalar rdNM = GuardedGetColorNM( *pDiffuse, ri, nm );
	SchlickDirectionalAlbedo::Lane lane;
	SchlickDirectionalAlbedo::PrepareLane( lane, r, p );
	const Scalar muI = Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( -ri.ray.Dir() ) );
	const Scalar wD = SchlickDirectionalAlbedo::CoupledDiffuseAt( lane, lobes.rho[0], rdNM, muI, cosTheta );
	SchlickDiffuseDraw draw;
	BuildSchlickDiffuseDraw( &lane, &lobes.rho[0], &rdNM, 1, muI, myonb, geomN, draw );

	const Scalar cD = SchlickDiffuseSelectCoefficient( ri, myonb, geomN, wD, lobes );

	return cD * diffusePdf
	     + SchlickSpecularDensity( ri, myonb, geomN, woNorm, draw, lobes );
}

//////////////////////////////////////////////////////////////////////
// EvaluateKrayNM -- DL-125.
//
// Returns the `krayNM` `ScatterNM` itself would have stamped on this
// lobe had `nm` been the hero wavelength, for the SAME outgoing
// direction.  Both lobes are recoverable from `(ri, outDir, nm)`:
//
//   diffuse:   Rd(nm)                                 -- direction-free
//   specular:  (rho(nm) + (1-rho(nm)) * F) * R(wo, r(nm), p(nm))
//
// with `F = (1 - (h.wi))^5` at the half-vector `h = normalize(wi + wo)`
// that `GenerateSpecularRay` sampled (recovered exactly: `wo` is the
// mirror of `-wi` about `h`, so `wi + wo` is parallel to `h`), and `R`
// the DL-127 ratio `SchlickKrayRatio`.
//
// DL-178/DL-225: Z still cancels, but the (bounded) masking depends on
// the wavelength's roughness and isotropy. Read and filter them exactly
// as ScatterNM does.
// Isotropy also enters R through the azimuthal factor and density.
//
// Every wavelength-dependent input is read at `nm`; no sampler draw is
// consumed, so this is safe to call once per companion wavelength per
// bounce.
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// EvaluateLobeFNM -- DL-216.
//
// Evaluates the SELECTED lobe's own spectral BSDF value f_I(wo; nm)
// in [1/sr], without multiplying by cosine and without dividing by
// any sampling density.
//////////////////////////////////////////////////////////////////////
//! DL-310: the diffuse krayNM ScatterNM stamps on a ray along `outDir`
//! -- the coupled min(Rd, 1 - A(i), 1 - A(o)) at `nm`, read and filtered
//! exactly as ScatterNM reads it.
Scalar SchlickSPF::SchlickCoupledDiffuseNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	const Scalar nm
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	Scalar roughnessNM = pRoughness->GetValueAtNM( ri, nm );
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}
	SchlickDirectionalAlbedo::Lane lane;
	SchlickDirectionalAlbedo::PrepareLane( lane, roughnessNM, pIsotropy->GetValueAtNM( ri, nm ) );
	return SchlickDirectionalAlbedo::CoupledDiffuseAt( lane,
		GuardedGetColorNM( *pSpecular, ri, nm ), GuardedGetColorNM( *pDiffuse, ri, nm ),
		Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( -ri.ray.Dir() ) ),
		Vector3Ops::Dot( myonb.w(), Vector3Ops::Normalize( outDir ) ) );
}

Scalar SchlickSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return SchlickCoupledDiffuseNM( ri, outDir, nm ) * INV_PI;
	}

	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;						// not a lobe this SPF emits
	}

	// Rebuild ScatterNM's sampling frame exactly (post-FlipW on a
	// back-face hit -- DL-100).
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 woNorm = Vector3Ops::Normalize( outDir );
	const Vector3 h = Vector3Ops::Normalize( wi + woNorm );

	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	const Scalar hdotn = Vector3Ops::Dot( h, myonb.w() );
	const Scalar cos_o = Vector3Ops::Dot( woNorm, myonb.w() );
	if( hdotk <= 0 || hdotn <= 0 || cos_o <= 0 ) {
		return 0;
	}

	const Scalar isotropyNM = pIsotropy->GetValueAtNM( ri, nm );
	Scalar roughnessNM = pRoughness->GetValueAtNM( ri, nm );
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}
	const Scalar krayRatio = SchlickKrayRatioFromH(
		h, wi, myonb, Vector3Ops::Dot( myonb.w(), wi ), roughnessNM, isotropyNM );
	if( krayRatio <= 0 ) {
		return 0;
	}

	const Scalar fresnel = ::pow( 1.0 - hdotk, 5 );
	const Scalar rho = GuardedGetColorNM( *pSpecular, ri, nm );
	const Scalar kray = ( rho + (1.0 - rho) * fresnel ) * krayRatio;
	const Scalar pdf = ComputeSchlickSpecularPdf( ri, myonb, woNorm, roughnessNM, isotropyNM );

	return ( kray * pdf ) / cos_o;
}

Scalar SchlickSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return SchlickCoupledDiffuseNM( ri, outDir, nm );
	}

	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;						// not a lobe this SPF emits
	}

	// Rebuild ScatterNM's sampling frame exactly (post-FlipW on a
	// back-face hit -- DL-100).
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 woNorm = Vector3Ops::Normalize( outDir );
	const Vector3 h = Vector3Ops::Normalize( wi + woNorm );

	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	if( hdotk <= 0 ) {
		return 0;						// a genuine zero, not "unimplemented"
	}

	const Scalar isotropyNM = pIsotropy->GetValueAtNM( ri, nm );
	Scalar roughnessNM = pRoughness->GetValueAtNM( ri, nm );
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}
	const Scalar krayRatio = SchlickKrayRatioFromH(
		h, wi, myonb, Vector3Ops::Dot( myonb.w(), wi ), roughnessNM, isotropyNM );
	if( krayRatio <= 0 ) {
		return 0;
	}

	const Scalar fresnel = ::pow( 1.0 - hdotk, 5 );
	const Scalar rho = GuardedGetColorNM( *pSpecular, ri, nm );
	return ( rho + (1.0 - rho) * fresnel ) * krayRatio;
}

Scalar SchlickSPF::EvaluateKrayNM(
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

	if( rayType == ScatteredRay::eRayDiffuse ) {
		return SchlickCoupledDiffuseNM( ri, outDir, nm );
	}

	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;
	}

	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 woNorm = Vector3Ops::Normalize( outDir );
	const Scalar cos_o = Vector3Ops::Dot( woNorm, myonb.w() );
	if( cos_o <= 0 ) {
		return 0;
	}

	const Scalar f = EvaluateLobeFNM( ri, outDir, rayType, nm, ior_stack );
	if( f <= 0 ) {
		return 0;
	}

	return ( f * cos_o ) / pdfHero;
}
