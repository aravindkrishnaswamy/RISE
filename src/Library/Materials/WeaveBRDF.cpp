//////////////////////////////////////////////////////////////////////
//
//  WeaveBRDF.cpp - The structured two-thread-family cloth response.
//    See WeaveBRDF.h for the model, its provenance in Sadeghi 2013 /
//    Zhu 2023-24, the energy argument and what Phase 2 slice A
//    deliberately does not do.
//
//    The longitudinal / azimuthal / Fresnel primitives are
//    FibreLobeMath.h's, shared unchanged with `hair_material`, so a
//    coefficient drift in one site cannot silently desync the two
//    fibre models.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WeaveBRDF.h"
#include "FibreLobeMath.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/math_utils.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FibreLobeMath;

const Scalar WeaveBRDF::kMinWidth               = Scalar( 0.005 );
const Scalar WeaveBRDF::kMaxWidth               = Scalar( 1.0 );
const Scalar WeaveBRDF::kMinAzimuth             = Scalar( 0.02 );
const Scalar WeaveBRDF::kMaxAzimuth             = Scalar( 3.0 );
const Scalar WeaveBRDF::kMinIOR                 = Scalar( 1.001 );
const Scalar WeaveBRDF::kMaxIOR                 = Scalar( 3.0 );
const Scalar WeaveBRDF::kMaxGap                 = Scalar( 0.3 );
// 0.17 rad (~9.7 deg) -- see the derivation at the declaration (WeaveBRDF.h):
// bounds the masking pole (at view latitude 90 - tilt_deg) to >= 80 deg.
const Scalar WeaveBRDF::kMaxTilt                = Scalar( 0.17 );
const Scalar WeaveBRDF::kMaskCorrelationSigma   = Scalar( 0.3490658503988659 );	// 20 degrees
const Scalar WeaveBRDF::kPoleBlendWidth         = Scalar( 0.10 );
const Scalar WeaveBRDF::kMaskGateHalfWidth      = Scalar( 0.05 );
const Scalar WeaveBRDF::kMinLobeWeight          = Scalar( 0.15 );
const Scalar WeaveBRDF::kMinAzimuthMass        = Scalar( 0.05 );
//! 2(4 - pi).  The k_d half of the volume lobe's normaliser -- see the
//! derivation at its use site in `ComputeThreadTerms`.
const Scalar WeaveBRDF::kVolumeKdNormaliser    = Scalar( 2.0 * ( 4.0 - PI ) );

namespace
{
	//! Ray-anchored geometric normal for the horizon gate.  A degenerate
	//! `vGeomNormal` falls back to the shading normal, making the gate a
	//! no-op (matches SheenBRDF / FabricBRDF / CoatedSPF).
	inline RISE::Vector3 GeomNormal( const RISE::RayIntersectionGeometric& ri, const RISE::Vector3& n )
	{
		const RISE::Vector3& raw = ( RISE::Vector3Ops::SquaredModulus( ri.vGeomNormal ) > RISE::Scalar(1e-12) )
			? ri.vGeomNormal : n;
		return ( RISE::Vector3Ops::Dot( raw, ri.ray.Dir() ) < 0 ) ? raw : -raw;
	}

	//! Wrap an azimuthal difference into [-pi, pi].
	inline RISE::Scalar WrapPi( RISE::Scalar x )
	{
		x = fmod( x + PI, TWO_PI );
		if( x < 0 ) {
			x += TWO_PI;
		}
		return x - PI;
	}

	//! `!(x >= lo)` is true for NaN as well as for below-range, so this
	//! one form covers a mis-authored painter's NaN and an out-of-range
	//! value together and needs no `isnan`.  FabricBRDF::ResolveFabric's
	//! idiom, applied to every scalar slot here because there are
	//! eighteen of them and any one could carry a NaN into the whole
	//! BRDF.
	inline RISE::Scalar ClampNaNSafe( const RISE::Scalar x, const RISE::Scalar lo, const RISE::Scalar hi )
	{
		return !( x >= lo ) ? lo : ( x > hi ? hi : x );
	}

	//! `J_d(theta)` -- the k_d half of the volume lobe's normalising
	//! integral at fibre-frame latitude `theta`, in closed form:
	//!
	//!   J_d(c) = 2 INT_{-pi/2}^{pi/2} cos^2(t) / (cos t + c) dt,  c = cos(theta)
	//!
	//! Split cos^2/(cos+c) = (cos - c) + c^2/(cos + c).  The first piece
	//! integrates to (2 - c pi).  The second is the standard
	//!   INT dt/(c + cos t) = 2/sqrt(1-c^2) atan( sqrt((1-c)/(1+c)) tan(t/2) ),
	//! which over [-pi/2, pi/2] is 4/sqrt(1-c^2) atan( sqrt((1-c)/(1+c)) ).
	//! Hence
	//!
	//!   J_d(c) = 2[ (2 - c pi) + c^2 * 4/sqrt(1-c^2) * atan(sqrt((1-c)/(1+c))) ]
	//!
	//! with the c -> 1 limit 2(4 - pi) = `kVolumeKdNormaliser` (the
	//! bracket tends to 2 - pi + 2) and J_d(0) = 4.  It GROWS toward
	//! grazing: 1.717 at c = 1, 3.08 at c = cos 80 deg, 4 at c = 0.
	RISE::Scalar VolumeKdIntegral( RISE::Scalar c )
	{
		c = Clamp( c, 0.0, 1.0 );
		const RISE::Scalar s2 = 1 - c * c;
		if( !( s2 > 1e-9 ) ) {
			return RISE::Implementation::WeaveBRDF::kVolumeKdNormaliser;	// the c -> 1 limit
		}
		const RISE::Scalar t = atan( sqrt( ( 1 - c ) / ( 1 + c ) ) );
		return 2 * ( ( 2 - c * PI ) + c * c * 4 / sqrt( s2 ) * t );
	}

	//! `G(s)` -- the surface lobe's own azimuthal shape factor at normal
	//! incidence:
	//!
	//!     G(s) = INT_{-pi/2}^{pi/2} TrimmedLogistic(phi; s, -pi/2, pi/2)
	//!            cos^2(phi) dphi
	//!
	//! One `cos(phi)` is the plane cosine `n.w_i` and the other is
	//! Sadeghi's masking, which at normal incidence reduces EXACTLY to
	//! `cos(phi_i)` (the view arm is 1 and the correlation blend of
	//! `1 * x` with `min(1, x)` is `x`).  The interval is the visible
	//! azimuth at zero tilt, which is what the shipped BRDF renormalises
	//! over, so this is the lobe's realised share and not a bound on it.
	//! `G -> 1` as `s -> 0` and falls as the lobe broadens.
	//!
	//! A C1 "smoothed ramp": equals `max(x,0)` outside
	//! `[-halfWidth,+halfWidth]` and blends smoothly across the kink at
	//! `x=0` inside it, matching both VALUE and SLOPE at the two
	//! boundaries (0 at `-halfWidth` with zero slope; `halfWidth` at
	//! `+halfWidth` with unit slope -- the standard quadratic
	//! mollification of a hinge).  Used to replace `max(cosPhi,0)`'s
	//! kink at the masking term's own geometric horizon (P2R4 review,
	//! finding P1-1) WITHOUT changing its cosine-weighted character away
	//! from that kink.  This is deliberately NOT a saturating gate (a
	//! 0-to-1 smoothstep): `mI`/`mO` are Sadeghi's graded occlusion
	//! factor, equal to `cosPhi` itself once positive, not a front/back
	//! indicator -- a saturating gate would flatten that grading to
	//! ~1 across nearly the whole front hemisphere and inflate the
	//! energy budget (measured: it moved denim's hemispherical albedo up
	//! 25-30%, tripping `TestHemisphericalAlbedoError`, before this
	//! value-matching form replaced it).
	RISE::Scalar SmoothedRamp( const RISE::Scalar x, const RISE::Scalar halfWidth )
	{
		if( x <= -halfWidth ) {
			return 0;
		}
		if( x >= halfWidth ) {
			return x;
		}
		const RISE::Scalar t = x + halfWidth;
		return ( t * t ) / ( 4 * halfWidth );
	}

	//! 64-point midpoint.  Used ONLY by `hemisphericalAlbedo`, which is
	//! not on the render path.
	RISE::Scalar SurfaceAzimuthShape( const RISE::Scalar s )
	{
		const int N = 64;
		RISE::Scalar acc = 0;
		for( int i = 0; i < N; ++i ) {
			const RISE::Scalar phi = -PI_OV_TWO + ( i + RISE::Scalar( 0.5 ) ) * PI / N;
			const RISE::Scalar c = cos( phi );
			acc += TrimmedLogistic( phi, s, -PI_OV_TWO, PI_OV_TWO ) * c * c;
		}
		return acc * PI / N;
	}
}

WeaveBRDF::WeaveBRDF(
	const WeavePatternKind pattern_,
	const WeaveTransmissionKind transmission_,
	const IScalarPainter& weaveScale,
	const IScalarPainter& weaveRotation,
	const IScalarPainter& weftSkew,
	const IScalarPainter* coverage,
	const IScalarPainter& gap,
	const IPainter& warpColor,
	const IScalarPainter& warpIOR,
	const IScalarPainter& warpWidth,
	const IScalarPainter& warpAzimuth,
	const IScalarPainter& warpKd,
	const IScalarPainter& warpTilt,
	const IScalarPainter& warpTransmit,
	const IPainter& weftColor,
	const IScalarPainter& weftIOR,
	const IScalarPainter& weftWidth,
	const IScalarPainter& weftAzimuth,
	const IScalarPainter& weftKd,
	const IScalarPainter& weftTilt,
	const IScalarPainter& weftTransmit
	) :
  pattern( pattern_ ),
  transmission( transmission_ ),
  pScale( &weaveScale ),
  pRotation( &weaveRotation ),
  pSkew( &weftSkew ),
  pCoverage( coverage ),
  pGap( &gap ),
  pWarpColor( &warpColor ),
  pWarpIOR( &warpIOR ),
  pWarpWidth( &warpWidth ),
  pWarpAzimuth( &warpAzimuth ),
  pWarpKd( &warpKd ),
  pWarpTilt( &warpTilt ),
  pWarpTransmit( &warpTransmit ),
  pWeftColor( &weftColor ),
  pWeftIOR( &weftIOR ),
  pWeftWidth( &weftWidth ),
  pWeftAzimuth( &weftAzimuth ),
  pWeftKd( &weftKd ),
  pWeftTilt( &weftTilt ),
  pWeftTransmit( &weftTransmit )
{
	pScale->addref();
	pRotation->addref();
	pSkew->addref();
	if( pCoverage ) {
		pCoverage->addref();
	}
	pGap->addref();
	pWarpColor->addref();
	pWarpIOR->addref();
	pWarpWidth->addref();
	pWarpAzimuth->addref();
	pWarpKd->addref();
	pWarpTilt->addref();
	pWarpTransmit->addref();
	pWeftColor->addref();
	pWeftIOR->addref();
	pWeftWidth->addref();
	pWeftAzimuth->addref();
	pWeftKd->addref();
	pWeftTilt->addref();
	pWeftTransmit->addref();
}

WeaveBRDF::~WeaveBRDF()
{
	safe_release( pWeftTransmit );
	safe_release( pWeftTilt );
	safe_release( pWeftKd );
	safe_release( pWeftAzimuth );
	safe_release( pWeftWidth );
	safe_release( pWeftIOR );
	safe_release( pWeftColor );
	safe_release( pWarpTransmit );
	safe_release( pWarpTilt );
	safe_release( pWarpKd );
	safe_release( pWarpAzimuth );
	safe_release( pWarpWidth );
	safe_release( pWarpIOR );
	safe_release( pWarpColor );
	safe_release( pGap );
	if( pCoverage ) {
		safe_release( pCoverage );
	}
	safe_release( pSkew );
	safe_release( pRotation );
	safe_release( pScale );
}

//////////////////////////////////////////////////////////////////////
// Rebind, for the interactive editor's MaterialIntrospection.
// addref-before-release throughout, so a self-rebind (Set(X) where X is
// already bound) cannot destroy the painter mid-swap --
// LambertianBRDF::SetReflectance's documented contract.
//////////////////////////////////////////////////////////////////////

#define WEAVE_SETTER( Name, Member, Iface )                    \
	void WeaveBRDF::Set##Name( const Iface& v )                \
	{                                                          \
		v.addref();                                            \
		safe_release( Member );                                \
		Member = &v;                                           \
	}

WEAVE_SETTER( WeaveScale,    pScale,       IScalarPainter )
WEAVE_SETTER( WeaveRotation, pRotation,    IScalarPainter )
WEAVE_SETTER( WeftSkew,      pSkew,        IScalarPainter )
WEAVE_SETTER( Gap,           pGap,         IScalarPainter )
WEAVE_SETTER( WarpColor,     pWarpColor,   IPainter )
WEAVE_SETTER( WarpIOR,       pWarpIOR,     IScalarPainter )
WEAVE_SETTER( WarpWidth,     pWarpWidth,   IScalarPainter )
WEAVE_SETTER( WarpAzimuth,   pWarpAzimuth, IScalarPainter )
WEAVE_SETTER( WarpKd,        pWarpKd,      IScalarPainter )
WEAVE_SETTER( WarpTilt,      pWarpTilt,    IScalarPainter )
WEAVE_SETTER( WarpTransmit,  pWarpTransmit,IScalarPainter )
WEAVE_SETTER( WeftColor,     pWeftColor,   IPainter )
WEAVE_SETTER( WeftIOR,       pWeftIOR,     IScalarPainter )
WEAVE_SETTER( WeftWidth,     pWeftWidth,   IScalarPainter )
WEAVE_SETTER( WeftAzimuth,   pWeftAzimuth, IScalarPainter )
WEAVE_SETTER( WeftKd,        pWeftKd,      IScalarPainter )
WEAVE_SETTER( WeftTilt,      pWeftTilt,    IScalarPainter )
WEAVE_SETTER( WeftTransmit,  pWeftTransmit,IScalarPainter )

#undef WEAVE_SETTER

//! `coverage` is the one slot that may have been UNBOUND at
//! construction (it is meaningful only for `weave custom`), so its
//! setter cannot share the macro's unconditional release.  Rebinding it
//! on a built-in pattern is harmless and deliberately permitted: the
//! painter is simply not read until the pattern is `custom`, and
//! refusing would make the editor's slot list depend on the pattern.
void WeaveBRDF::SetCoverage( const IScalarPainter& v )
{
	v.addref();
	if( pCoverage ) {
		safe_release( pCoverage );
	}
	pCoverage = &v;
}

//////////////////////////////////////////////////////////////////////
// Fibre-frame geometry.  Shared with WeaveSPF through the header so
// the evaluator and the sampler cannot disagree about which frame,
// which latitude or which azimuth interval they are in -- the
// Scatter<->Pdf mismatch SPFPdfConsistencyTest exists to catch.
//////////////////////////////////////////////////////////////////////

WeaveBRDF::FibreFrame WeaveBRDF::MakeFibreFrame( const Vector3& tangent, const Vector3& n )
{
	FibreFrame f;
	f.t        = tangent;
	f.sinAlpha = Vector3Ops::Dot( n, tangent );
	f.cosAlpha = 0;
	f.valid    = false;

	const Vector3 perp = n - tangent * f.sinAlpha;
	const Scalar  len  = Vector3Ops::Magnitude( perp );
	if( !( len > Scalar( 1e-9 ) ) ) {
		// The tangent is parallel to the normal.  Unreachable through
		// the shipping paths (the tilt is clamped to 0.6 rad and the
		// untilted tangent lies in the surface plane), but a caller
		// binding a degenerate frame gets a family that contributes
		// nothing rather than a NaN one.
		f.nk = n;
		f.bk = n;
		return f;
	}

	f.nk       = perp * ( Scalar( 1 ) / len );
	f.cosAlpha = len;
	f.bk       = Vector3Ops::Cross( tangent, f.nk );
	f.valid    = true;
	return f;
}

WeaveBRDF::DirAngles WeaveBRDF::ProjectDir( const FibreFrame& f, const Vector3& w )
{
	DirAngles a;
	a.sinTheta = Clamp( Vector3Ops::Dot( w, f.t ), -1.0, 1.0 );
	a.cosTheta = SafeSqrt( 1 - Sqr( a.sinTheta ) );

	const Scalar e1 = Vector3Ops::Dot( w, f.nk );
	const Scalar e2 = Vector3Ops::Dot( w, f.bk );
	const Scalar h  = sqrt( e1 * e1 + e2 * e2 );

	a.phi    = atan2( e2, e1 );
	// cos(atan2(e2,e1)) == e1/hypot, computed directly: the ratio keeps
	// its accuracy where the round trip through atan2/cos does not, and
	// the masking term reads this at exactly the near-axis directions
	// where that matters.
	//
	// POLE CONDITIONING (P2R4 review, finding P1-1).  `h` is the
	// magnitude of the in-plane (perpendicular-to-fibre) projection,
	// and it is EXACTLY `a.cosTheta` above (w is unit, and
	// (sinTheta, e1, e2) is an orthonormal decomposition of it) -- so
	// it vanishes exactly where a direction approaches the fibre axis,
	// the pole of this (theta, phi) parametrisation.  There, "front of
	// the cylinder" versus "back of it" is genuinely undefined, and the
	// raw ratio `e1/h` is numerically UNRELIABLE arbitrarily close to
	// that point: two directions a fraction of a degree apart can
	// straddle the pole and report `cosPhi` near +1 and near -1
	// respectively, even though `h` itself (and hence `a.cosTheta`)
	// varies perfectly smoothly through it. Trusting atan2's sign there
	// fed the masking gate below a near-instantaneous swing -- measured,
	// a 48.9% brightness drop in a 0.5 degree step at an ORDINARY,
	// in-spec view angle (float_tilt = kMaxTilt's old value).
	//
	// Rather than "fix" `phi` (there is no consistent value to give it
	// exactly at the pole), the RATIO itself is blended toward the
	// pole's own neutral value -- 0, "neither front nor back" -- as `h`
	// shrinks below `kPoleBlendWidth`, using `h` as the blend parameter
	// because `h` is what stays smooth through the pole; `phi` does not.
	// At `h = 0` exactly this returns 0 unconditionally, never reading
	// atan2's (arbitrary, at that point) sign at all.
	const Scalar poleRamp = WeaveSmoothStep( Scalar( 0 ), kPoleBlendWidth, h );
	a.cosPhi = ( h > Scalar( 1e-12 ) ) ? ( poleRamp * ( e1 / h ) ) : Scalar( 0 );
	return a;
}

bool WeaveBRDF::VisibleAzimuthHalfRange( const FibreFrame& f,
                                         const Scalar sinTheta, const Scalar cosTheta,
                                         Scalar& phiMax )
{
	// n.w = sinAlpha sin(theta) + cosAlpha cos(theta) cos(phi) > 0
	//   <=>  cos(phi) > -sinAlpha sin(theta) / (cosAlpha cos(theta))
	const Scalar denom = f.cosAlpha * cosTheta;
	if( !( denom > Scalar( 1e-9 ) ) ) {
		// Exactly along the fibre axis: the cross-section term vanishes
		// and the tilt term alone decides.  Measure zero, and every lobe
		// weights it at ~0, but it must not produce a NaN threshold.
		if( f.sinAlpha * sinTheta > 0 ) {
			phiMax = PI;
			return true;
		}
		return false;
	}

	const Scalar c0 = -f.sinAlpha * sinTheta / denom;
	if( c0 >= 1 ) {
		// This whole latitude band is buried below the surface -- only
		// reachable under a non-zero tilt, and it is the ONE place the
		// surface lobe's density loses mass (WeaveSPF.h states the
		// measured size of that loss).
		return false;
	}
	if( c0 <= -1 ) {
		phiMax = PI;
		return true;
	}
	phiMax = acos( c0 );
	return true;
}

bool WeaveBRDF::SurfaceAzimuthInterval( const FibreFrame& f,
                                        const Scalar sinThetaI, const Scalar cosThetaI,
                                        const Scalar phiO,
                                        Scalar& lo, Scalar& hi )
{
	Scalar phiMax = 0;
	if( !VisibleAzimuthHalfRange( f, sinThetaI, cosThetaI, phiMax ) ) {
		return false;
	}
	// phi_i in (-phiMax, phiMax)  <=>  phi_d in (-phiMax - phiO, phiMax - phiO),
	// then intersected with the logistic's own [-pi, pi] domain.
	//
	// The intersection is a NO-OP at zero tilt (phiMax == pi/2 and
	// |phiO| < pi/2, so both endpoints already lie inside [-pi, pi]) and
	// bites only under a tilt large enough to make phiMax > pi/2.  What
	// it discards is the logistic's FAR TAIL -- at |phi_d| = pi the
	// density is exp(-pi/s) of its peak, ~2e-3 at s = 0.5 -- so the
	// density's hemispherical integral falls short by well under a
	// percent rather than by a view-dependent factor.  Measured value in
	// WeaveSPF.h.
	lo = r_max( -PI, -phiMax - phiO );
	hi = r_min(  PI,  phiMax - phiO );
	return hi > lo;
}

Scalar WeaveBRDF::VisibleAzimuthMass( const FibreFrame& f, const ThreadParams& t,
                                      const Scalar sinThetaA, const Scalar cosThetaA,
                                      const Scalar phiB )
{
	Scalar lo = 0, hi = 0;
	if( !SurfaceAzimuthInterval( f, sinThetaA, cosThetaA, phiB, lo, hi ) ) {
		return 0;
	}
	// The trimmed logistic over [a, b] is Logistic/(CDF(b) - CDF(a)), so
	// the FRACTION of the full [-pi, pi] lobe that survives the trim is
	// the ratio of the two normalisers.  Returning that ratio rather than
	// the interval lets one expression serve the density (which trims
	// one-sidedly, because a sampler must) and the BRDF (which cannot).
	const Scalar full = LogisticCDF( PI, t.s ) - LogisticCDF( -PI, t.s );
	if( !( full > 0 ) ) {
		return 0;
	}
	const Scalar part = LogisticCDF( hi, t.s ) - LogisticCDF( lo, t.s );
	return ( part > 0 ) ? ( part / full ) : Scalar( 0 );
}

Scalar WeaveBRDF::AzimuthalTrimBoost( const FibreFrame& f, const ThreadParams& t,
                                      const DirAngles& a, const DirAngles& b )
{
	// THE PROBLEM.  `SurfaceLobePdf` renormalises the azimuthal logistic
	// over the interval of `phi_d` that keeps the SAMPLED direction above
	// the horizon -- it must, or the density would not integrate to 1
	// over the hemisphere.  `value()` used the untrimmed [-pi, pi]
	// normaliser, so the two disagreed and the evaluator's surface lobe
	// integrated to 0.92 (narrow thread) / 0.80 (wide thread) instead of
	// 1: the balance is lobe mass placed below the surface plane, which
	// the horizon gate discards and nothing puts back.
	//
	// WHY THE PDF'S OWN INTERVAL CANNOT SIMPLY BE REUSED.  That interval
	// is built from the SAMPLED direction's latitude and the VIEW's
	// azimuth -- (theta_i, phi_o).  Swapping i and o gives
	// (theta_o, phi_i), a different interval and a different normaliser,
	// so `value()` would stop being reciprocal.  Reciprocity is a
	// non-negotiable gate here (SPFBSDFConsistencyTest Part E, 1e-6), and
	// a BRDF that is 8 % brighter and non-reciprocal is a worse object
	// than one that is 8 % dark and reciprocal.
	//
	// THE SYMMETRISED FORM.  Take the GEOMETRIC MEAN of the two one-sided
	// surviving-mass fractions:
	//
	//     boost = 1 / sqrt( Z(theta_i, phi_o) * Z(theta_o, phi_i) )
	//
	// The product is invariant under an i/o swap -- the two factors
	// simply exchange -- so the BRDF stays reciprocal, and where the two
	// agree (which is the whole zero-tilt case, where phi_max is pi/2 at
	// every latitude) it reduces exactly to the one-sided trim the
	// sampler uses.  It is the same device `Q_sym` uses one layer up, for
	// the same reason.
	//
	// THE FLOOR IS LOAD-BEARING.  `Z` can become small when a large tilt
	// buries most of a latitude band, and an unfloored 1/sqrt(Z_i Z_o)
	// would then multiply the surface lobe without bound at exactly the
	// grazing directions where it is already largest.  0.05 caps the
	// boost at 20x, which no shipped preset approaches (the smallest Z
	// measured over the preset table is 0.62).
	const Scalar zi = VisibleAzimuthMass( f, t, a.sinTheta, a.cosTheta, b.phi );
	const Scalar zo = VisibleAzimuthMass( f, t, b.sinTheta, b.cosTheta, a.phi );
	const Scalar zf = r_max( kMinAzimuthMass, zi ) * r_max( kMinAzimuthMass, zo );
	return ( zf > 0 ) ? ( Scalar( 1 ) / sqrt( zf ) ) : Scalar( 1 );
}

Scalar WeaveBRDF::SurfaceLobePdf( const ThreadParams& t, const FibreFrame& f,
                                  const DirAngles& oA, const DirAngles& iA )
{
	Scalar lo = 0, hi = 0;
	if( !SurfaceAzimuthInterval( f, iA.sinTheta, iA.cosTheta, oA.phi, lo, hi ) ) {
		return 0;
	}
	const Scalar phiD = WrapPi( iA.phi - oA.phi );
	// p(w) = Mp * N.  The Jacobian is exactly the one that makes this
	// come out clean: sampling theta from `Mp(theta_i,theta_o) cos(theta_i)`
	// (which integrates to 1 by d'Eon's normalisation) and phi from the
	// trimmed logistic, with dw = cos(theta) dtheta dphi, gives
	// p(w) = [Mp cos] [N] / cos = Mp N.
	return Mp( iA.cosTheta, oA.cosTheta, iA.sinTheta, oA.sinTheta, t.vSurf )
	     * TrimmedLogistic( phiD, t.s, lo, hi );
}

Scalar WeaveBRDF::SurfaceSelectWeight( const ThreadParams& t, const Vector3& n, const Vector3& wo )
{
	// `r_min` / `r_max` are MACROS (math_utils.h), so every call
	// expression handed to one is evaluated TWICE.  Hoist each into a
	// local first -- `Dot` and `MaxValue` are cheap but this is the
	// render path, and the habit is what keeps a future expensive
	// argument from silently doubling.
	const Scalar rawCos = Vector3Ops::Dot( n, wo );
	const Scalar cosV   = r_max( Scalar( 0 ), rawCos );
	const Scalar F      = FrDielectric( cosV, t.eta );
	const Scalar rawA   = ColorMath::MaxValue( t.tint );
	const Scalar loA    = r_max( Scalar( 0 ), rawA );
	const Scalar A      = r_min( Scalar( 1 ), loA );
	const Scalar denom  = F + ( Scalar( 1 ) - F ) * A;
	const Scalar raw    = ( denom > Scalar( 1e-9 ) ) ? ( F / denom ) : Scalar( 1 );
	const Scalar lo     = r_max( kMinLobeWeight, raw );
	const Scalar hi     = Scalar( 1 ) - kMinLobeWeight;
	return r_min( hi, lo );
}

WeaveBRDF::ThreadTerms WeaveBRDF::ComputeThreadTerms(
	const ThreadParams& t,
	const Vector3& n,
	const Vector3& wi,
	const Vector3& wo )
{
	ThreadTerms out;
	out.valid   = false;
	out.surface = 0;
	out.volume  = 0;

	const FibreFrame f = MakeFibreFrame( t.tangent, n );
	if( !f.valid ) {
		return out;
	}

	const DirAngles a = ProjectDir( f, wi );
	const DirAngles b = ProjectDir( f, wo );

	const Scalar thetaI = SafeASin( a.sinTheta );
	const Scalar thetaO = SafeASin( b.sinTheta );
	const Scalar thetaD = ( thetaI - thetaO ) * Scalar( 0.5 );
	const Scalar phiD   = WrapPi( a.phi - b.phi );

	// Kim 2002's cylinder-geometry effective incidence, credited by
	// Sadeghi Eq. 2.  EVEN in both thetaD and phiD, which is half the
	// reciprocity argument (an i/o swap flips the sign of each).
	const Scalar cosGamma = cos( thetaD ) * cos( phiD * Scalar( 0.5 ) );
	const Scalar F        = FrDielectric( cosGamma, t.eta );

	// Sadeghi Eq. 7-9: same-family masking with Ashikhmin et al. 2000's
	// correlation blend.  The two arms EXCHANGE under an i/o swap and
	// `u` is even in phiD, so the combination is symmetric -- the other
	// half of the reciprocity argument, and the reason this is
	// implemented while his view-only normaliser Q is not.
	//
	// SMOOTHED GATE (P2R4 review, finding P1-1, second half).  The
	// original `max(cosPhi, 0)` is a HARD hinge: even with `cosPhi`
	// itself now well-conditioned through the pole (`ProjectDir`'s
	// blend), the hinge still has a slope discontinuity exactly at the
	// geometric horizon `cosPhi = 0`, which a direction sweep crosses at
	// an ordinary, everyday view/light angle (not just near the pole).
	// Replaced with `SmoothedRamp`, a C1 quadratic mollification of the
	// SAME hinge over `[-kMaskGateHalfWidth, +kMaskGateHalfWidth]` --
	// NOT a saturating smoothstep (see that function's own note: `mI`/
	// `mO` are a graded cosine-weighted occlusion factor, and a
	// saturating gate would flatten it to ~1 across most of the front
	// hemisphere and inflate the energy budget). Applied identically to
	// `mI` (from `wi`) and `mO` (from `wo`), so the i/o exchange
	// argument above is untouched by this change.
	const Scalar mI = SmoothedRamp( a.cosPhi, kMaskGateHalfWidth );
	const Scalar mO = SmoothedRamp( b.cosPhi, kMaskGateHalfWidth );
	const Scalar u  = exp( -( phiD * phiD ) /
	                       ( Scalar( 2 ) * kMaskCorrelationSigma * kMaskCorrelationSigma ) );
	const Scalar mask = ( Scalar( 1 ) - u ) * mI * mO + u * r_min( mI, mO );
	if( !( mask > 0 ) ) {
		return out;
	}

	// --- surface lobe.  UNTINTED: a dielectric's specular reflection
	//     preserves the incident spectrum; only light that enters the
	//     fibre picks up the dye.  See the banner.
	// The azimuthal factor is the FULL [-pi, pi] logistic times a
	// SYMMETRISED trim boost, which is what makes it agree with the
	// sampler's own renormalisation without breaking reciprocity.  See
	// `AzimuthalTrimBoost`.
	out.surface = F
	            * Mp( a.cosTheta, b.cosTheta, a.sinTheta, b.sinTheta, t.vSurf )
	            * TrimmedLogistic( phiD, t.s, -PI, PI )
	            * AzimuthalTrimBoost( f, t, a, b )
	            * mask;

	// --- volume lobe.  The Chandrasekhar denominator is floored: the
	//     direction pair that drives it to zero is the one running along
	//     the yarn axis, where the surface cosine is vanishing anyway,
	//     so the floor bounds a term that carries no energy rather than
	//     truncating one that does.
	const Scalar denom   = r_max( Scalar( 1e-3 ), a.cosTheta + b.cosTheta );
	const Scalar bracket = ( Scalar( 1 ) - t.kd )
	                     * Mp( a.cosTheta, b.cosTheta, a.sinTheta, b.sinTheta, t.vVol )
	                     + t.kd;
	// C_v -- THE NORMALISER, RE-DERIVED.  There is no C_v in Sadeghi's
	// Eq. 3 at all; RISE needs one because it substituted d'Eon's
	// NORMALISED `Mp` for his un-normalised Gaussian `g`, whose amplitude
	// he absorbs into the fitted albedo `A`.  So C_v has to be the exact
	// value of the integral the lobe must divide by to return `A_k`.
	//
	// THE INTEGRAL.  Write the volume lobe's directional albedo at
	// fibre-frame view latitude theta_o, zero tilt, masking set aside:
	//
	//   rho_vol = INT_hemi f_vol (n.w_i) dw_i
	//
	// with dw_i = cos(theta_i) dtheta_i dphi_i and, at zero tilt,
	// n.w_i = cos(theta_i) cos(phi_i).  The azimuth factors out --
	// INT_{-pi/2}^{pi/2} cos(phi) dphi = 2 -- leaving
	//
	//   C_v(k_d, theta_o) = 2 INT_{-pi/2}^{pi/2}
	//                         [ (1-k_d) Mp + k_d ] cos^2(theta_i)
	//                         / ( cos theta_i + cos theta_o ) dtheta_i
	//
	// AT theta_o = 0 both pieces are elementary.
	//
	//   Mp piece: rewrite cos^2/(cos+1) as cos * [cos/(cos+1)].  `Mp` is
	//   concentrated on the specular cone theta_i = -theta_o = 0, where
	//   the bracket is exactly 1/2, and INT Mp cos(theta_i) dtheta_i == 1
	//   is d'Eon's normalisation.  So the piece is 2(1-k_d)(1/2) = (1-k_d).
	//
	//   k_d piece: cos^2/(1+cos) = (cos - 1) + 1/(1+cos), and
	//     INT_{-pi/2}^{pi/2} (cos - 1) dtheta = 2 - pi
	//     INT_{-pi/2}^{pi/2} dtheta/(1+cos) = [tan(theta/2)] = 2
	//   summing to (4 - pi).  So the piece is 2(4 - pi) k_d.
	//
	//   C_v = (1 - k_d) + 2(4 - pi) k_d = 1 + 0.71681 k_d
	//
	// WHAT THIS REPLACED, AND WHY IT MATTERED.  The first cut used
	// `C_v = 2(1 + k_d)`, which was not this integral but a BOUND on it,
	// obtained by replacing cos(theta_i)/(cos theta_i + cos theta_o) by
	// its supremum 1 -- a quantity whose mean over the hemisphere is 1/2,
	// so the bound is exactly 2x loose -- and by taking the k_d term's
	// coefficient as 4 rather than the true 2(4-pi) = 1.717.  Measured
	// against brute-force quadrature it ran 2.00x (k_d = 0) to 2.33x
	// (k_d = 1) too large, uniformly across thread widths.  The volume
	// lobe carries ~93 % of a white weave's energy, so that single factor
	// WAS the "every fabric renders dark" report: white satin at normal
	// incidence measured 0.372 where this form gives 0.719, and Sadeghi's
	// own measured white fabrics span 0.5-0.8.
	//
	// THE theta_o DEPENDENCE IS NOT CARRIED, and that is a deliberate,
	// measured trade.  The true integral grows toward grazing (the
	// denominator relaxes as cos theta_o falls), so a constant taken at
	// theta_o = 0 under-divides there and the volume lobe returns MORE
	// than A_k at a grazing fibre latitude.  That is bounded rather than
	// unbounded -- the closed form for the k_d piece at general theta_o
	// is 2[(2 - pi cos theta_o) + cos^2 theta_o * 4/sin theta_o *
	// atan(sqrt((1-cos)/(1+cos)))], which is 3.08 at theta_o = 80 deg
	// against 1.717 at 0 -- and what actually bounds the material is
	// measured by LayeredWhiteFurnaceTest's white rows, which carry an
	// explicit <= 1.05 ceiling for exactly this reason.  See the debt in
	// docs/CLOTH_FABRIC_DESIGN.md 10.3.
	// THE VIEW DEPENDENCE IS CARRIED, SYMMETRISED.  Taking `J_d` at its
	// theta = 0 value alone -- the constant `kVolumeKdNormaliser` --
	// under-divides at grazing, because the true integral GROWS as the
	// Chandrasekhar denominator relaxes (1.717 at theta 0, 3.08 at 80,
	// 4 at 90).  Measured, that put a white untilted denim at rho = 1.058
	// at theta 88: a genuinely over-unity BRDF, and past the 1.05 ceiling
	// LayeredWhiteFurnaceTest's white rows assert.
	//
	// Using `J_d(theta_o)` alone would fix it and would NOT be
	// reciprocal, so the two latitudes enter through their geometric
	// mean, the same device `AzimuthalTrimBoost` uses: invariant under an
	// i/o swap, and exactly the theta = 0 closed form
	// `(1 - k_d) + 2(4 - pi) k_d` when both latitudes are 0, which is
	// where its derivation above is written and where its value is
	// pinned.
	const Scalar jd = sqrt( VolumeKdIntegral( a.cosTheta ) * VolumeKdIntegral( b.cosTheta ) );
	const Scalar Cv = ( Scalar( 1 ) - t.kd ) + jd * t.kd;
	out.volume = ( Scalar( 1 ) - F ) * bracket / ( denom * Cv ) * mask;

	out.valid = true;
	return out;
}

//////////////////////////////////////////////////////////////////////
// ResolveWeave
//////////////////////////////////////////////////////////////////////

void WeaveBRDF::ResolveWeave( const RayIntersectionGeometric& ri, const Scalar nm, WeaveParams& out ) const
{
	// The ray-facing frame.  `FlipW` negates U as well as W, which keeps
	// the basis right-handed -- and because BOTH the warp tangent and
	// the weft tangent are derived from the SAME flipped basis, a
	// back-face hit sees the weave mirrored rather than skewed, which is
	// what a fabric viewed from behind actually looks like.
	OrthonormalBasis3D onb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), onb.w() ) > NEARZERO ) {
		onb.FlipW();
	}
	out.n = onb.w();

	// ACHROMATIC in both regimes -- see the long note on the
	// declaration.  Every one of these feeds either the mixture density
	// or the frame the density is expressed in.
	const Scalar rotation = pRotation->GetValuesAt( ri ).v[0];
	const Scalar skew     = pSkew->GetValuesAt( ri ).v[0];

	// `IsFiniteDouble` rather than a bare comparison: a NaN angle would
	// otherwise reach `RotateTangent`'s cos/sin and turn the whole frame
	// -- and therefore every lobe in both families -- into NaN.
	const Scalar rotSafe  = RISE::IsFiniteDouble( rotation ) ? rotation : Scalar( 0 );
	const Scalar skewSafe = RISE::IsFiniteDouble( skew )     ? skew     : Scalar( 0 );

	const OrthonormalBasis3D warpONB = MicrofacetUtils::RotateTangent( onb, rotSafe );
	// The weft crosses the warp: a further 90 degrees, plus whatever
	// `weft_skew` says.  A skew of 0 is the orthogonal weave every
	// built-in draft assumes; a non-zero one is a sheared or bias-cut
	// cloth, and it is authored rather than derived because nothing in
	// the draft can imply it.
	const OrthonormalBasis3D weftONB = MicrofacetUtils::RotateTangent( warpONB, PI_OV_TWO + skewSafe );

	// --- the coverage field.
	const Scalar gap = ClampNaNSafe( pGap->GetValuesAt( ri ).v[0], Scalar( 0 ), kMaxGap );
	out.available = Scalar( 1 ) - gap;
	out.thin      = ( transmission == eWeaveTransmissionThin );

	if( pattern == eWeaveCustom ) {
		out.aWarp = pCoverage
			? ClampNaNSafe( pCoverage->GetValuesAt( ri ).v[0], Scalar( 0 ), Scalar( 1 ) )
			: Scalar( 0.5 );
	} else {
		const Scalar scale = ClampNaNSafe( pScale->GetValuesAt( ri ).v[0], Scalar( 0 ), Scalar( 1e6 ) );
		// The pixel footprint in UV.  `worldWidth` is world-space and
		// `dudx..dvdy` are the screen-space UV derivatives, so the UV
		// extent of one pixel is the larger of the two auxiliary-ray
		// offsets' UV magnitudes.  `valid == false` leaves this 0, which
		// `WeaveCoverageAt` reads as "no footprint" and correctly
		// disengages the fade rather than snapping to the mean -- the
		// same convention the expression VM's `fw` uses.
		Scalar fpUV = 0;
		if( ri.txFootprint.valid ) {
			const Scalar ax = sqrt( Sqr( ri.txFootprint.dudx ) + Sqr( ri.txFootprint.dvdx ) );
			const Scalar ay = sqrt( Sqr( ri.txFootprint.dudy ) + Sqr( ri.txFootprint.dvdy ) );
			const Scalar m  = r_max( ax, ay );
			fpUV = RISE::IsFiniteDouble( m ) ? m : Scalar( 0 );
		}
		out.aWarp = WeaveCoverageAt( pattern, ri.ptCoord.x, ri.ptCoord.y, scale, fpUV );
	}

	// --- the two families.
	struct Slots {
		const IPainter*			color;
		const IScalarPainter*	ior;
		const IScalarPainter*	width;
		const IScalarPainter*	azimuth;
		const IScalarPainter*	kd;
		const IScalarPainter*	tilt;
		const IScalarPainter*	transmit;
		const OrthonormalBasis3D* frame;
		ThreadParams*			dst;
	};
	const Slots slots[2] = {
		{ pWarpColor, pWarpIOR, pWarpWidth, pWarpAzimuth, pWarpKd, pWarpTilt, pWarpTransmit, &warpONB, &out.warp },
		{ pWeftColor, pWeftIOR, pWeftWidth, pWeftAzimuth, pWeftKd, pWeftTilt, pWeftTransmit, &weftONB, &out.weft }
	};

	for( int k = 0; k < 2; ++k )
	{
		const Slots& s = slots[k];
		ThreadParams& d = *s.dst;

		const Scalar tilt = ClampNaNSafe( s.tilt->GetValuesAt( ri ).v[0], -kMaxTilt, kMaxTilt );
		// The float tilt rotates the family's tangent about the in-plane
		// perpendicular -- i.e. it leans the yarn out of the surface
		// plane toward the normal, which is Sadeghi's tangent offset
		// reduced to one scalar (P2_ZHU_READ 4.3).  `t` stays unit by
		// construction (an orthonormal pair combined with cos/sin).
		d.tangent = s.frame->u() * cos( tilt ) + out.n * sin( tilt );

		d.eta = ClampNaNSafe( s.ior->GetValuesAt( ri ).v[0], kMinIOR, kMaxIOR );

		const Scalar beta = ClampNaNSafe( s.width->GetValuesAt( ri ).v[0], kMinWidth, kMaxWidth );
		d.vSurf = beta * beta;
		// gamma_v == 2 gamma_s: Table II's ratio holds in all six of its
		// rows, so the volume width is DERIVED rather than authored.
		d.vVol  = Scalar( 4 ) * d.vSurf;

		const Scalar gamma = ClampNaNSafe( s.azimuth->GetValuesAt( ri ).v[0], kMinAzimuth, kMaxAzimuth );
		// The authored `azimuth` is an angular WIDTH (a standard
		// deviation), so it reads on the same scale as `width`.  A
		// logistic of scale s has standard deviation s*pi/sqrt(3), hence
		// s = gamma*sqrt(3)/pi.
		d.s = gamma * Scalar( 0.5513288954217921 );

		d.kd = ClampNaNSafe( s.kd->GetValuesAt( ri ).v[0], Scalar( 0 ), Scalar( 1 ) );

		// P2-B.  `transmit` is read UNCONDITIONALLY OF `out.thin` for
		// simplicity here, but every CONSUMER of it (`ValueWithParams`,
		// `PdfWithParams`, `ScatterImpl`) gates its effect behind
		// `p.thin` at the point of use rather than trusting this field to
		// be zero -- so a `transmission none` material is bit-identical
		// to the committed P2-A code even if a scene mis-authors
		// `warp_transmit` on it.  See WeaveBRDF.h section 2a.
		d.transmit = ClampNaNSafe( s.transmit->GetValuesAt( ri ).v[0], Scalar( 0 ), Scalar( 1 ) );

		// The dye.  CLAMPED to [0,1] per channel, and that clamp is
		// load-bearing rather than defensive: `A_k` multiplies a lobe
		// whose directional albedo is normalised to <= 1, so an authored
		// painter above 1 would make the family return more light than
		// it receives.  Nothing else refuses such a painter.
		const RISEPel rgb = s.color->GetColor( ri );
		if( nm < 0 ) {
			d.tint = rgb;
			ColorMath::Clamp( d.tint, Scalar( 0 ), Scalar( 1 ) );
			d.tintNM = 0;
		} else {
			d.tint = rgb;
			ColorMath::Clamp( d.tint, Scalar( 0 ), Scalar( 1 ) );
			// The white guard, inlined against the `rgb` sample already
			// in hand -- `GuardedGetColorNM` would re-sample `GetColor`
			// just to run `IsUntintedWhite` on it.  Same predicate, same
			// fallback (IPainter.h).
			//
			// `d.tint` is kept populated on the NM path as well, because
			// `SurfaceSelectWeight` reads `max3(A)` and MUST return the
			// same weight in both regimes: the lobe-selection pmf is what
			// `Scatter` stores as the ray's pdf, and a wavelength-
			// dependent one would put that stored hero-wavelength density
			// out of step with a companion-wavelength `Pdf()` call.
			const Scalar raw = IsUntintedWhite( rgb ) ? Scalar( 1 ) : s.color->GetColorNM( ri, nm );
			d.tintNM = ClampNaNSafe( raw, Scalar( 0 ), Scalar( 1 ) );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// value / valueNM
//
// ONE geometry body (ComputeThreadTerms) serves both colour regimes, so
// the RGB and spectral twins cannot drift on the frame, the gates, the
// masking or either lobe -- only WHICH tint multiplies the volume term
// differs.  That is the structural half of
// docs/skills/audit-by-bug-pattern.md's RGB/NM discipline.
//////////////////////////////////////////////////////////////////////

RISEPel WeaveBRDF::ValueWithParams(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const WeaveParams& p ) const
{
	const Vector3 wi = Vector3Ops::Normalize( vLightIn );
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );

	// `wo` must always be on the shading-normal-facing side -- unaffected
	// by P2-B, since `p.n` is already the ray-facing normal the VIEW sits
	// on by construction (ResolveWeave's FlipW).
	if( Vector3Ops::Dot( p.n, wo ) <= NEARZERO ) {
		return RISEPel( 0, 0, 0 );
	}
	const Scalar cosWiN = Vector3Ops::Dot( p.n, wi );

	const ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar        cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	if( cosWiN > NEARZERO )
	{
		// REFLECT SIDE.  BIT-IDENTICAL to the committed P2-A expression:
		// same geometric-horizon gate, same loop, same final
		// `* p.available` -- the only addition is `volumeScale`, which is
		// EXACTLY 1 (a no-op multiply, not merely "close to 1") whenever
		// `!p.thin`, regardless of what a mis-authored `transmit` painter
		// holds.  See WeaveBRDF.h section 2a.
		const Vector3 geomN = GeomNormal( ri, p.n );
		if( Vector3Ops::Dot( wi, geomN ) <= 0 || Vector3Ops::Dot( wo, geomN ) <= 0 ) {
			return RISEPel( 0, 0, 0 );
		}

		RISEPel out( 0, 0, 0 );
		for( int k = 0; k < 2; ++k ) {
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			const ThreadTerms t = ComputeThreadTerms( *fam[k], p.n, wi, wo );
			if( !t.valid ) {
				continue;
			}
			const Scalar volumeScale = p.thin ? ( Scalar( 1 ) - fam[k]->transmit ) : Scalar( 1 );
			out = out + ( RISEPel( t.surface, t.surface, t.surface ) + fam[k]->tint * ( t.volume * volumeScale ) ) * cov[k];
		}
		return out * p.available;
	}

	if( p.thin && cosWiN < -NEARZERO )
	{
		// TRANSMIT SIDE (full sphere).  `i` and `o` are on OPPOSITE sides
		// of the shading normal -- Zhu's Lambertian-shaped diffuse
		// transmission, `(1-gap)*transmit_k*T_k/pi` per family
		// (WeaveBRDF.h section 2a).  No geometric-horizon gate: this IS
		// the below-horizon transport `ScattersFullSphere()` exists to
		// admit, on HairMaterial's precedent ("NO GEOMETRIC-HORIZON GATE"
		// for the far side).
		RISEPel out( 0, 0, 0 );
		for( int k = 0; k < 2; ++k ) {
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			out = out + fam[k]->tint * ( fam[k]->transmit * INV_PI * cov[k] );
		}
		return out * p.available;
	}

	return RISEPel( 0, 0, 0 );
}

Scalar WeaveBRDF::ValueNMWithParams(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar,
	const WeaveParams& p ) const
{
	const Vector3 wi = Vector3Ops::Normalize( vLightIn );
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );

	if( Vector3Ops::Dot( p.n, wo ) <= NEARZERO ) {
		return 0;
	}
	const Scalar cosWiN = Vector3Ops::Dot( p.n, wi );

	const ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar        cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	if( cosWiN > NEARZERO )
	{
		const Vector3 geomN = GeomNormal( ri, p.n );
		if( Vector3Ops::Dot( wi, geomN ) <= 0 || Vector3Ops::Dot( wo, geomN ) <= 0 ) {
			return 0;
		}

		Scalar out = 0;
		for( int k = 0; k < 2; ++k ) {
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			const ThreadTerms t = ComputeThreadTerms( *fam[k], p.n, wi, wo );
			if( !t.valid ) {
				continue;
			}
			// TEXTUALLY PARALLEL to the RGB twin above, parenthesisation
			// included: `tint * (t.volume * volumeScale)` against
			// `tintNM * (t.volume * volumeScale)`.  The chunk test's
			// spectral-parity check measures their agreement at an
			// authored white, so an association difference here would
			// surface there as a guard failure it is not.
			const Scalar volumeScale = p.thin ? ( Scalar( 1 ) - fam[k]->transmit ) : Scalar( 1 );
			out += ( t.surface + fam[k]->tintNM * ( t.volume * volumeScale ) ) * cov[k];
		}
		return out * p.available;
	}

	if( p.thin && cosWiN < -NEARZERO )
	{
		Scalar out = 0;
		for( int k = 0; k < 2; ++k ) {
			if( !( cov[k] > 0 ) ) {
				continue;
			}
			out += fam[k]->tintNM * ( fam[k]->transmit * INV_PI * cov[k] );
		}
		return out * p.available;
	}

	return 0;
}

RISEPel WeaveBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	WeaveParams p;
	ResolveWeave( ri, Scalar( -1 ), p );
	return ValueWithParams( vLightIn, ri, p );
}

Scalar WeaveBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	WeaveParams p;
	ResolveWeave( ri, nm, p );
	return ValueNMWithParams( vLightIn, ri, nm, p );
}

//////////////////////////////////////////////////////////////////////
// albedo -- the OIDN AOV.
//
// A directional estimate, not a measurement: the surface lobe's share
// is approximated by the Fresnel reflectance at the SURFACE normal's
// cosine rather than at the per-family fibre-frame effective angle, and
// the volume lobe's by its dye.  That is the right trade for an AOV --
// it is noise-free, it tracks the view, and it is bounded -- but it is
// not what `value()` integrates to.  See `hemisphericalAlbedo` for the
// exactness class of the sibling quantity, which IS consumed by the
// layering materials.
//////////////////////////////////////////////////////////////////////

RISEPel WeaveBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	WeaveParams p;
	ResolveWeave( ri, Scalar( -1 ), p );

	const Vector3 v      = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar  rawCos = Vector3Ops::Dot( p.n, v );
	const Scalar  loCos  = r_max( Scalar( 0 ), rawCos );
	const Scalar  cosV   = r_min( Scalar( 1 ), loCos );

	const ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar        cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	RISEPel out( 0, 0, 0 );
	for( int k = 0; k < 2; ++k ) {
		if( !( cov[k] > 0 ) ) {
			continue;
		}
		const Scalar F = FrDielectric( cosV, fam[k]->eta );
		out = out + ( RISEPel( F, F, F ) + fam[k]->tint * ( Scalar( 1 ) - F ) ) * cov[k];
	}
	out = out * p.available;

	// IBSDF::albedo's contract is explicit that the AOV must be in [0,1]
	// per channel so OIDN can run with cleanAux = true.
	ColorMath::Clamp( out, Scalar( 0 ), Scalar( 1 ) );
	return out;
}

//////////////////////////////////////////////////////////////////////
// hemisphericalAlbedo{,NM} -- a DERIVED closed form, and its exactness
// class stated before the number.
//
//   out = available * SUM_k a_k * [ F_k(1) G(s_k) + A_k (1 - F_k(1)) pi/4 ]
//
// EVERY FACTOR IS AN INTEGRAL, NOT A FIT.  Evaluate the two lobes'
// directional albedo at NORMAL incidence with zero tilt, where the
// masking term collapses to `cos(phi_i)` exactly (the view arm is 1, and
// the correlation blend of `1 * x` with `min(1, x)` is `x`):
//
//   VOLUME.  rho_vol = INT f_vol M (n.w_i) dw_i.  With
//   dw = cos(theta) dtheta dphi and n.w_i = cos(theta_i) cos(phi_i), the
//   azimuth contributes INT_{-pi/2}^{pi/2} cos^2(phi) dphi = pi/2 where
//   `C_v`'s own derivation (see ComputeThreadTerms) contributed
//   INT cos(phi) dphi = 2.  The ratio is exactly pi/4, and everything
//   else cancels against `C_v` by construction:
//
//       rho_vol(0) = A_k (1 - F_k(1)) * pi/4
//
//   SURFACE.  rho_surf = INT F Mp N (M) (n.w_i) dw_i.  `Mp`'s d'Eon
//   normalisation makes the longitudinal integral 1, the lobe's mass
//   sits where its Fresnel argument cos(theta_d) cos(phi_d/2) -> 1, and
//   the azimuthal integral is `G(s)` above:
//
//       rho_surf(0) = F_k(1) * G(s_k)
//
//   Note the Fresnel is `F(1)`, NOT the hemispherical average `F_bar`.
//   An earlier revision budgeted this term at `F_bar` (0.098 at
//   eta 1.539) where the lobe actually lives at `F(1)` (0.045) -- a
//   factor of 2.2 that then had to be absorbed by a fitted constant.
//   The lobe is narrow and centred on the specular cone; the average
//   over all incidences is the wrong number for it.
//
// THE FITTED CONSTANTS ARE GONE.  Two "realised fraction" scalars (0.18
// and 0.30) used to multiply these terms.  They existed to reconcile a
// LOOSE analytic bound with a BRDF that was itself 2.1x dark from the
// `C_v` defect, and they appeared only here -- so they retuned the
// REPORTED albedo down until it agreed with the wrong render, and the
// test guarding them compared the fit against the same `value()` it was
// fit to.  With `C_v` corrected and the budgets taken at the angles the
// lobes actually occupy, nothing is left to absorb.
//
// EXACTNESS CLASS, PRE-COMMITTED AT +-40 % RELATIVE and stated here
// before it was measured.  The form is EXACT at normal incidence and
// carries NO view falloff, while `IBSDF::hemisphericalAlbedo`'s contract
// asks for the BIHEMISPHERICAL average.  The two differ by however much
// the material's directional albedo varies with view, which for an
// untilted draft is small (it even rises toward grazing, because `C_v`
// is normalised at theta_o = 0 where the Chandrasekhar denominator is
// largest) and for a float-tilted one is not (a tilt buries one family's
// visible face at a grazing view, and the masking term then collapses).
// So this runs HIGH on the tilted presets, which is the hazardous
// direction for `coated_material`'s Saunderson denominator -- the [0,1]
// clamp below and the recorded debt in
// docs/CLOTH_FABRIC_DESIGN.md 10.3 are what bound it.
// tests/WeaveMaterialChunkTest.cpp measures the band on all four
// presets against brute-force quadrature.
//
// It is DELIBERATELY NOT view-dependent and does not read the frame: a
// bihemispherical average cannot depend on a rotation about the normal.
//
// Returns TRUE unconditionally -- unlike `FabricBRDF`'s, which forwards
// a substrate's refusal, this material has no substrate to defer to.
//////////////////////////////////////////////////////////////////////

bool WeaveBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	WeaveParams p;
	ResolveWeave( ri, Scalar( -1 ), p );

	const ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar        cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	out = RISEPel( 0, 0, 0 );
	for( int k = 0; k < 2; ++k ) {
		if( !( cov[k] > 0 ) ) {
			continue;
		}
		const Scalar F1    = FrDielectric( Scalar( 1 ), fam[k]->eta );
		const Scalar sTerm = F1 * SurfaceAzimuthShape( fam[k]->s );
		// P2-B: this is the FRONT-hemisphere REFLECT budget only (what
		// `fabric_material`'s substrate-energy accounting consumes), so
		// the share diverted to diffuse transmission is excluded from it,
		// same `volumeScale` rule as ValueWithParams.  Exactly 1 (a no-op
		// multiply) when `!p.thin`.
		const Scalar volumeScale = p.thin ? ( Scalar( 1 ) - fam[k]->transmit ) : Scalar( 1 );
		const Scalar vTerm = ( Scalar( 1 ) - F1 ) * PI_OV_FOUR * volumeScale;
		out = out + ( RISEPel( sTerm, sTerm, sTerm ) + fam[k]->tint * vTerm ) * cov[k];
	}
	out = out * p.available;
	ColorMath::Clamp( out, Scalar( 0 ), Scalar( 1 ) );
	return true;
}

bool WeaveBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	WeaveParams p;
	ResolveWeave( ri, nm, p );

	const ThreadParams* fam[2] = { &p.warp, &p.weft };
	const Scalar        cov[2] = { p.aWarp, Scalar( 1 ) - p.aWarp };

	out = 0;
	for( int k = 0; k < 2; ++k ) {
		if( !( cov[k] > 0 ) ) {
			continue;
		}
		const Scalar F1 = FrDielectric( Scalar( 1 ), fam[k]->eta );
		const Scalar volumeScale = p.thin ? ( Scalar( 1 ) - fam[k]->transmit ) : Scalar( 1 );
		out += ( F1 * SurfaceAzimuthShape( fam[k]->s )
		       + fam[k]->tintNM * ( Scalar( 1 ) - F1 ) * PI_OV_FOUR * volumeScale ) * cov[k];
	}
	out *= p.available;
	const Scalar loOut = r_max( Scalar( 0 ), out );
	out = r_min( Scalar( 1 ), loOut );
	return true;
}
