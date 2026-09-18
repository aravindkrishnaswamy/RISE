//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongSPF.cpp - Implementation of the SPF
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
#include "AshikminShirleyAnisotropicPhongSPF.h"
#include "AshikminShirleyAnisotropicPhongBRDF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Stratified-quadrature resolution PER AXIS for the diffuse lobe's
	//! selection coefficient C_D -- same role as SchlickSPF.cpp's
	//! kSpecQuadN / IsotropicPhongSPF.cpp's kPhongQuadN.  See
	//! docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 4.
	const int kAshQuadN = 16;

	//! The specular lanes Scatter()/ScatterNM() would emit: ONE ordinarily
	//! (and always in the spectral path), THREE when a per-channel Nu/Nv
	//! painter drives Scatter()'s per-channel branch.
	struct AshikminLobeSet
	{
		int    count;
		Scalar NU[3];
		Scalar NV[3];
		Scalar Rs[3];
	};
}

//! Forward azimuth warp, extracted verbatim from GenerateSpecularRay so the
//! quadrature and the inversion below replay it exactly.  `phi_root` is
//! sqrt((NU+1)/(NV+1)) for whichever lane is being evaluated.
static inline Scalar AshikminSamplePhi( const Scalar x, const Scalar phi_root )
{
	Scalar phi = 0;
	if( x < 0.25 ) {
		const Scalar val = 4.0 * x;
		phi = atan( phi_root * tan(PI_OV_TWO * val) );
	} else if( x < 0.5 ) {
		const Scalar val = 1.0 - 4*(0.5 - x);
		phi = atan( phi_root * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	} else if( x < 0.75 ) {
		const Scalar val = 4*(x - 0.5);
		phi = atan( phi_root * tan(PI_OV_TWO * val) );
		phi += PI;
	} else {
		const Scalar val = 1.0 - 4*(1.0 - x);
		phi = atan( phi_root * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}
	return phi;
}

//! Inverse of AshikminSamplePhi: phi in [0,2pi) -> x in [0,1).  Exact --
//! the forward warp is atan(phi_root*tan(pi/2 * val)), monotone and
//! bijective within each quadrant, so val = (2/pi)*atan(tan(phi_local)/
//! phi_root) inverts it.  Only the multi-lane (per-channel) density needs
//! this, to recover which random pair a queried direction came from so the
//! OTHER lanes' draws (from the SAME pair) can be reconstructed.
static inline bool AshikminInvertPhi( const Scalar phi, const Scalar phi_root, Scalar& outX )
{
	if( phi_root < NEARZERO ) {
		return false;
	}

	Scalar phi_local;
	int quadrant;
	if( phi <= PI_OV_TWO )            { phi_local = phi;          quadrant = 0; }
	else if( phi <= PI )              { phi_local = PI - phi;     quadrant = 1; }
	else if( phi <= PI + PI_OV_TWO )  { phi_local = phi - PI;     quadrant = 2; }
	else                              { phi_local = TWO_PI - phi; quadrant = 3; }

	Scalar val;
	if( phi_local >= PI_OV_TWO - Scalar(1e-9) ) {
		// tan() diverges at the quadrant boundary; the limit is val=1.
		val = 1.0;
	} else {
		val = (2.0/PI) * atan( tan(phi_local) / phi_root );
		val = r_max( Scalar(0), r_min( Scalar(1), val ) );
	}

	// val == 1 is the OPEN end of this quadrant's x-interval: x =
	// quadrant*0.25 + 0.25 is the next quadrant's CLOSED start, and
	// AshikminSamplePhi maps it to that quadrant's val=0 endpoint (pi, for
	// quadrant 0) rather than back to phi_local = pi/2.  Returning it would
	// reconstruct every sibling lane's draw in the wrong quadrant -- exact
	// at phi = pi/2 (hu == 0, hv > 0) and at the other three boundaries.
	// Step just inside the interval instead; AshikminSamplePhi is
	// continuous there, so the reconstructed phi_j is within ~1e-9 rad of
	// the true limit.
	const Scalar kOpenEnd = 1.0 - Scalar(1e-9);
	outX = quadrant*0.25 + r_min( val, kOpenEnd )*0.25;
	return true;
}

//! C_D(wo) -- the coefficient the diffuse sampling density carries in the
//! aggregate density, EXACT AT THE QUERY wo (unlike Schlick/Phong, the
//! diffuse selection weight `wD` here is itself a function of wo -- see
//! the caller).  kAshQuadN^2 deterministic stratified replays of
//! GenerateSpecularRay's own (x,y) unit square, using the closed form
//! `kray_j(direction) = fresnel(Rs_j,hdotk) * max(cos_o,0) /
//! max(cos_i,cos_o)` derived in docs/DL98_DL99_PHONG_PDF_WEIGHTS.md
//! section 5 (the NU/NV terms in the BRDF and the inverse-pdf cancel
//! exactly, leaving no dependence on the sampled half-vector's azimuth or
//! polar angle beyond hdotk and cos_o -- so no separate "other lanes"
//! reconstruction is even needed to REPLAY the grid here, only to query a
//! specific wo's siblings, done in AshikminSpecularDensity below).
//!
//! `wiu,wiv` are wi projected into (u,v); `cos_i` is wi's n-component.
//! `gu,gv,gw` / `wiDotGeomN` are geomN's projection and wi's geomN
//! component, needed to replicate Scatter's OWN geomN accept-check
//! (`dot(k2,geomN)<=0` rejects the lane from the container) via the
//! closed form `dot(k2,X) = -dot(wi,X) + 2*hdotk*dot(h,X)`.
static Scalar AshikminDiffuseSelectCoefficient(
	const Scalar wD,
	const AshikminLobeSet& lobes,
	const Scalar cos_i, const Scalar wiu, const Scalar wiv,
	const Scalar wiDotGeomN, const Scalar gu, const Scalar gv, const Scalar gw
	)
{
	Scalar cosPhi[3][kAshQuadN], sinPhi[3][kAshQuadN], expo[3][kAshQuadN];
	const Scalar inv = 1.0 / Scalar(kAshQuadN);

	for( int j = 0; j < lobes.count; j++ ) {
		const Scalar phi_root = sqrt( (lobes.NU[j]+1.0) / (lobes.NV[j]+1.0) );
		for( int a = 0; a < kAshQuadN; a++ ) {
			const Scalar x = (Scalar(a) + 0.5) * inv;
			const Scalar phi = AshikminSamplePhi( x, phi_root );
			const Scalar cp = cos(phi), sp = sin(phi);
			cosPhi[j][a] = cp;
			sinPhi[j][a] = sp;
			expo[j][a] = 1.0 / ( cp*cp*lobes.NU[j] + sp*sp*lobes.NV[j] + 1.0 );
		}
	}

	Scalar accum = 0;
	for( int a = 0; a < kAshQuadN; a++ ) {
		for( int b = 0; b < kAshQuadN; b++ ) {
			const Scalar y = (Scalar(b) + 0.5) * inv;
			Scalar wS = 0;
			int nAccepted = 0;

			for( int j = 0; j < lobes.count; j++ ) {
				const Scalar ct = pow( y, expo[j][a] );
				const Scalar st = sqrt( r_max( Scalar(0), 1.0 - ct*ct ) );
				const Scalar hdotk = ct*cos_i + st*( cosPhi[j][a]*wiu + sinPhi[j][a]*wiv );
				if( hdotk <= 0 ) {
					// GenerateSpecularRay's own accept-check (hdotk<0 ->
					// return false, ray never built).
					continue;
				}
				const Scalar cosO = -cos_i + 2.0*hdotk*ct;
				if( cosO < 0 ) {
					continue;
				}
				const Scalar geomOk = -wiDotGeomN + 2.0*hdotk*( cosPhi[j][a]*st*gu + sinPhi[j][a]*st*gv + ct*gw );
				if( geomOk <= 0 ) {
					continue;
				}
				nAccepted++;
				const Scalar fresnel = lobes.Rs[j] + (1.0-lobes.Rs[j]) * pow(1.0-hdotk, 5.0);
				wS += fresnel * cosO / r_max( cos_i, cosO );
			}

			if( nAccepted == 0 ) {
				// RandomlySelect's freeidx==1 short-circuit.
				accum += 1.0;
				continue;
			}
			const Scalar total = wD + wS;
			if( total > NEARZERO ) {
				accum += wD / total;
			}
		}
	}

	return accum / Scalar(kAshQuadN*kAshQuadN);
}

//! E_{diffuse draw D}[ 1{D rejected} * (freeidx==1 ? 1 : wNumerator/wSTotal)
//!   + 1{D accepted} * wNumerator/(wSTotal + wD(D)) ]
//!
//! This is q_i(wo)'s own defining expectation, and it is a SEPARATE
//! quadrature from AshikminDiffuseSelectCoefficient's, not a reuse of it --
//! the two lobes' selection weights are BOTH direction-dependent random
//! variables here (unlike Schlick/Phong, where the diffuse weight is a
//! constant), so `wD` cannot be evaluated "at wo" and folded into a single
//! `aD`-weighted formula the way DL-67 Slice 0's Schlick construction
//! does: the diffuse candidate competing against a hypothesized specular
//! draw wo is a DIFFERENT ray with its OWN independent random direction,
//! whose weight must be averaged over separately.  (An earlier version of
//! this fix used wD(wo) directly here and integrated to 0.96-1.05 instead
//! of 1 on every anisotropic test configuration -- see the fix commit
//! message.)  `wNumerator` is lane i's own realized weight w_i(wo);
//! `wSTotal` is w_i(wo) plus the OTHER specular lanes' realized weights at
//! their own (deterministically reconstructed) directions -- both fixed
//! per call, since the specular pool's composition doesn't depend on the
//! diffuse's draw.  `nAcceptedSpec` is how many specular lanes are in the
//! pool (RandomlySelect's freeidx==1 short-circuit fires when the diffuse
//! is rejected and exactly one specular lane remains).
static Scalar AshikminSpecularSelectCoefficient(
	const Scalar wNumerator, const Scalar wSTotal, const int nAcceptedSpec,
	const Scalar wDBase, const Scalar cos_i,
	const Scalar gu, const Scalar gv, const Scalar gw
	)
{
	static const Scalar diffuseNorm = 28.0 / 23.0;
	const Scalar fromK2 = 1.0 - pow( 1.0 - cos_i*0.5, 5.0 );
	const Scalar inv = 1.0 / Scalar(kAshQuadN);

	Scalar accum = 0;
	for( int a = 0; a < kAshQuadN; a++ ) {
		const Scalar px = (Scalar(a) + 0.5) * inv;
		const Scalar phiD = TWO_PI * px;
		const Scalar cosPhiD = cos(phiD), sinPhiD = sin(phiD);
		for( int b = 0; b < kAshQuadN; b++ ) {
			const Scalar py = (Scalar(b) + 0.5) * inv;
			// CreateDiffuseVector's own inverse-CDF: cost=sqrt(1-py),
			// sint=sqrt(py) (see GeometricUtilities::CreateDiffuseVector).
			const Scalar cost = sqrt( r_max( Scalar(0), 1.0 - py ) );
			const Scalar sint = sqrt( r_max( Scalar(0), py ) );
			const Scalar geomOk = cost*gw + sint*( cosPhiD*gu + sinPhiD*gv );

			if( geomOk <= 0 ) {
				// Diffuse rejected -- specular pool competes alone.
				if( nAcceptedSpec == 1 ) {
					accum += 1.0;
				} else if( wSTotal > NEARZERO ) {
					accum += wNumerator / wSTotal;
				}
				continue;
			}

			const Scalar fromK1 = 1.0 - pow( 1.0 - cost*0.5, 5.0 );
			const Scalar wD = wDBase * diffuseNorm * fromK1 * fromK2;
			const Scalar total = wSTotal + wD;
			if( total > NEARZERO ) {
				accum += wNumerator / total;
			}
		}
	}

	return accum / Scalar(kAshQuadN*kAshQuadN);
}

//! sum_i q_i(wo) * p_i(wo) -- the specular half of the aggregate, exact at
//! the query wo.  `h`'s (hdotk,hn,hu,hv) and `cosO` are the caller's own
//! (computed once, shared across lanes since h depends only on wi/wo, not
//! on NU/NV).  `wDBase` is the direction-INDEPENDENT part of the diffuse
//! lobe's realized selection weight -- `MaxValue(Rd*(1-Rs))` for RGB (the
//! exact quantity RandomlySelect reduces; see Pdf()'s own comment on why
//! the reduction order matters), `rd*(1-rho)` for spectral -- and feeds
//! AshikminSpecularSelectCoefficient's own diffuse-side quadrature.
static Scalar AshikminSpecularDensity(
	const Scalar hdotk, const Scalar hn, const Scalar hu, const Scalar hv, const Scalar cosO,
	const Scalar cos_i, const Scalar wiu, const Scalar wiv,
	const Scalar wiDotGeomN, const Scalar gu, const Scalar gv, const Scalar gw,
	const Scalar wDBase,
	const AshikminLobeSet& lobes
	)
{
	const Scalar sinThetaSq = r_max( Scalar(0), 1.0 - hn*hn );
	const Scalar sinTheta = sqrt( sinThetaSq );

	// `hn` does not depend on the lane, so this is a whole-call early-out,
	// not a per-lane skip: h below the sampling frame's horizon is a
	// half-vector GenerateSpecularRay cannot have produced from ANY lane.
	if( hn <= 0 ) {
		return 0;
	}

	Scalar sum = 0;

	for( int i = 0; i < lobes.count; i++ ) {
		Scalar exponent_i = 0;
		if( sinThetaSq > NEARZERO ) {
			exponent_i = ( lobes.NU[i]*hu*hu + lobes.NV[i]*hv*hv ) / sinThetaSq;
		}
		const Scalar factor1 = sqrt( (lobes.NU[i]+1.0)*(lobes.NV[i]+1.0) ) / TWO_PI;
		const Scalar factor2 = pow( hn, exponent_i );
		const Scalar pdf_i = (factor1*factor2) / (4.0*hdotk);
		if( pdf_i <= 0 ) {
			continue;
		}

		// Lane i's own realized weight, exact at wo (see the closed form
		// in AshikminDiffuseSelectCoefficient's comment).
		const Scalar fresnel_i = lobes.Rs[i] + (1.0-lobes.Rs[i]) * pow(1.0-hdotk, 5.0);
		const Scalar w_i = fresnel_i * cosO / r_max( cos_i, cosO );

		Scalar wOther = 0;
		int nAccepted = 1; // lane i itself

		if( lobes.count > 1 && sinTheta > NEARZERO ) {
			Scalar phi_i = atan2( hv, hu );
			if( phi_i < 0 ) {
				phi_i += TWO_PI;
			}
			const Scalar phi_root_i = sqrt( (lobes.NU[i]+1.0) / (lobes.NV[i]+1.0) );
			Scalar x;
			if( AshikminInvertPhi( phi_i, phi_root_i, x ) ) {
				// Invert cos_theta = pow(y, e_fwd), e_fwd = 1/(exponent_i+1)
				// (exponent_i is the DENSITY exponent, NU*hu^2+NV*hv^2 in
				// the (u,v) frame with the sin_theta_h_sq factored out --
				// see the exponent_i derivation above; e_fwd is
				// GenerateSpecularRay's OWN forward sampling exponent,
				// 1/(cos_phi^2*NU+sin_phi^2*NV+1) -- so y = hn^(1/e_fwd) =
				// hn^(exponent_i+1), NOT hn^(1/exponent_i).
				const Scalar y = pow( r_max( Scalar(0), r_min( Scalar(1), hn ) ), exponent_i + 1.0 );

				for( int j = 0; j < lobes.count; j++ ) {
					if( j == i ) {
						continue;
					}
					const Scalar phi_root_j = sqrt( (lobes.NU[j]+1.0) / (lobes.NV[j]+1.0) );
					const Scalar phi_j = AshikminSamplePhi( x, phi_root_j );
					const Scalar cpj = cos(phi_j), spj = sin(phi_j);
					const Scalar expo_j = 1.0 / ( cpj*cpj*lobes.NU[j] + spj*spj*lobes.NV[j] + 1.0 );
					const Scalar ctj = pow( y, expo_j );
					const Scalar stj = sqrt( r_max( Scalar(0), 1.0 - ctj*ctj ) );

					const Scalar hdotk_j = ctj*cos_i + stj*( cpj*wiu + spj*wiv );
					if( hdotk_j <= 0 ) {
						continue;
					}
					const Scalar cosO_j = -cos_i + 2.0*hdotk_j*ctj;
					if( cosO_j < 0 ) {
						continue;
					}
					const Scalar geomOk_j = -wiDotGeomN + 2.0*hdotk_j*( cpj*stj*gu + spj*stj*gv + ctj*gw );
					if( geomOk_j <= 0 ) {
						continue;
					}
					nAccepted++;
					const Scalar fresnel_j = lobes.Rs[j] + (1.0-lobes.Rs[j]) * pow(1.0-hdotk_j, 5.0);
					wOther += fresnel_j * cosO_j / r_max( cos_i, cosO_j );
				}
			}
		}

		const Scalar wS = w_i + wOther;

		const Scalar q = AshikminSpecularSelectCoefficient( w_i, wS, nAccepted, wDBase, cos_i, gu, gv, gw );

		sum += q * pdf_i;
	}

	return sum;
}

AshikminShirleyAnisotropicPhongSPF::AshikminShirleyAnisotropicPhongSPF(
	const IScalarPainter& Nu_,
	const IScalarPainter& Nv_,
	const IPainter& Rd_,
	const IPainter& Rs_
	) :
  pNu( &Nu_ ),
  pNv( &Nv_ ),
  pRd( &Rd_ ),
  pRs( &Rs_ )
{
	pNu->addref();
	pNv->addref();
	pRd->addref();
	pRs->addref();
}

AshikminShirleyAnisotropicPhongSPF::~AshikminShirleyAnisotropicPhongSPF( )
{
	safe_release( pNu );
	safe_release( pNv );
	safe_release( pRd );
	safe_release( pRs );
}

void AshikminShirleyAnisotropicPhongSPF::SetNu( const IScalarPainter& v ) { v.addref(); safe_release( pNu ); pNu = &v; }
void AshikminShirleyAnisotropicPhongSPF::SetNv( const IScalarPainter& v ) { v.addref(); safe_release( pNv ); pNv = &v; }
void AshikminShirleyAnisotropicPhongSPF::SetRd( const IPainter& v )       { v.addref(); safe_release( pRd ); pRd = &v; }
void AshikminShirleyAnisotropicPhongSPF::SetRs( const IPainter& v )       { v.addref(); safe_release( pRs ); pRs = &v; }

static bool GenerateSpecularRay(
	ScatteredRay& specular,
	Scalar& diffuseFactor,
	Scalar& specFactor,
	const OrthonormalBasis3D& onb,
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand,										///< [in] Random numbers
	const Scalar NU,
	const Scalar NV,
	const Scalar Rs
	)
{
	// Do this according to the paper
	Scalar	phi = 0;

	// Generate the half-way vector h
	const Scalar phi_root_ns = sqrt((NU+1.0)/(NV+1.0));

	if( ptrand.x < 0.25 )
	{
//		Scalar val = 1.0 - 4*(0.25 - p.x);		reduces to -->
		Scalar val = 4.0 * ptrand.x;
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
	}
	else if( ptrand.x < 0.5 )
	{
		Scalar val = 1.0 - 4*(0.5 - ptrand.x);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	}
	else if( ptrand.x < 0.75 )
	{
		Scalar val = 4*(ptrand.x - 0.5);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi += PI;
	}
	else
	{
		Scalar val = 1.0 - 4*(1.0 - ptrand.x);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}

	const Scalar cos_phi = cos( phi );
	const Scalar sin_phi = sin( phi );
	const Scalar exponent = 1.0 / (cos_phi*cos_phi*NU + sin_phi*sin_phi*NV + 1.0);
	const Scalar cos_theta = pow( ptrand.y, exponent );
	const Scalar sin_theta = sqrt( 1.0 - cos_theta*cos_theta );

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z,
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z,
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );

	{
		// Set the attenuation to ps from the paper, computed based on the monte carlo section of the paper
		const Vector3 k1 = -ri.ray.Dir();
		const Scalar hdotk = Vector3Ops::Dot(h, k1);

		if( hdotk < 0 ) {
			return false;
		}

		// Now compute the ray
		// Rather than using -k1, we just the original ri.ray.Dir()
		Vector3 k2 = Vector3Ops::Normalize( ri.ray.Dir()/*-k1*/ + 2.0 * hdotk * h );

		// If the ray goes into the material, then lets not use it.
		// Geometric-horizon gate: GlintModifier can tilt the shading normal up
		// to 60 deg off the true surface, so a k2 that validates against the
		// (tilted) shading normal can still point below the geometric surface --
		// the continuation ray then tunnels into the solid.  Oriented to the
		// passed-in onb's w() (the normal this lobe was sampled around).
		// Degenerate vGeomNormal (SquaredModulus guard, matches
		// GlintModifier.cpp) falls back to the shading normal, making the gate
		// a no-op.
		// (ray-anchor sweep: geomN's orientation is anchored to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot flip the gate to the wrong side.)
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : onb.w();
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		// Reject against the frame this lobe was actually sampled around
		// (the `onb` parameter, which is the caller's possibly-FlipW'd
		// myonb) -- not the raw ri.onb.w(), which differs by sign on a
		// back-face hit and silently dropped every legitimately-sampled
		// back-face specular lobe.
		if( Vector3Ops::Dot(k2, onb.w()) < 0 || Vector3Ops::Dot(k2, geomN) <= 0 ) {
			return false;
		}

		// Compute the density of the perturbed ray (against `onb`, the frame
		// h was actually built in above -- NOT ri.onb.w(), which differs by
		// sign on a back-face hit and would feed pow() a negative base).
		const Scalar hdotn = Vector3Ops::Dot( onb.w(), h );
		const Scalar factor1 = sqrt((NU+1.0)*(NV+1.0)) / TWO_PI;
		const Scalar factor2 = pow( hdotn, (NU*cos_phi*cos_phi + NV*sin_phi*sin_phi));

		const Scalar inv_actual_density = (4.0 * hdotk) / (factor1*factor2);

		// Compute the density of what we actually want (from the BRDF).
		// Pass `onb` (the caller's possibly-FlipW'd myonb) explicitly -- NOT
		// the raw ri.onb -- so ndotk2 = Dot(n,k2) agrees in sign with the k2
		// this function validated above; the raw ri.onb.w() went negative on
		// back-face hits and made the helper's ndotk2 early-out silently
		// drop the density computation for every legitimately-sampled
		// back-face specular ray.
		Scalar brdf;
		AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors( diffuseFactor, brdf, k2, ri, onb.w(), onb.u(), onb.v(), NU, NV, Rs );

		// The weighing factor is then the inverse of the actual density multiplied by the density
		// we truly want.  This should be as close to 1 as possible, but it won't always be so
		specFactor = inv_actual_density*brdf;

		specular.ray.Set( ri.ptIntersection, k2 );

		// Set the PDF: specular half-vector density converted to solid angle
		// pdf = factor1 * factor2 / (4 * hdotk)
		specular.pdf = (factor1 * factor2) / (4.0 * hdotk);
		specular.isDelta = false;
	}

	return true;
}

void AshikminShirleyAnisotropicPhongSPF::Scatter(
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

	// Geometric-horizon gate for the diffuse lobe below (the specular lobe
	// is gated inside GenerateSpecularRay): a wo that validates against a
	// GlintModifier-tilted shading normal can still point below the
	// geometric surface.  Degenerate vGeomNormal falls back to the shading
	// normal (gate is a no-op).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const ScalarTriple NUt = pNu->GetValuesAt(ri);
	const ScalarTriple NVt = pNv->GetValuesAt(ri);
	const Scalar NU[3] = { NUt.v[0], NUt.v[1], NUt.v[2] };
	const Scalar NV[3] = { NVt.v[0], NVt.v[1], NVt.v[2] };

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;

	const RISEPel rho = pRs->GetColor(ri);

	if( !pNu->HasPerChannelVariation() && !pNv->HasPerChannelVariation() )
	{
		Scalar diffuseFactor_unused=0, specFactor=0;
		if( GenerateSpecularRay( specular, diffuseFactor_unused, specFactor, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), NU[0], NV[0], ColorMath::MaxValue(rho) ) ) {
			// specFactor = brdf_spec/pdf.  For correct IS: kray = BRDF*cos/pdf.
			// specularFactor already includes Fresnel (which contains Rs),
			// so no extra Rs multiplication.  Add cos_o for the missing cosine.
			//
			// cos_o is taken against `myonb.w()`, the frame this lobe was
			// SAMPLED around and the frame GenerateSpecularRay validated
			// `k2` against -- NOT the raw `ri.onb.w()`, which differs by
			// sign on a back-face hit.  Reading the raw one there made
			// cos_o negative for every accepted specular ray, so kray went
			// NEGATIVE: not a probability mass at all, it reverses the CDF
			// ordering inside RandomlySelect and is handed straight back by
			// RandomlySelectNonDiffuse (SMSPhotonMap / the caustic photon
			// tracers), which applies no weight test.  See
			// docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 7 (DL-100 sibling
			// audit) and AshikminShirleySPFPdfConsistencyTest's `backface`
			// rows.
			const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), myonb.w() );
			specular.kray = RISEPel(1,1,1) * specFactor * cos_o;
			scattered.AddScatteredRay( specular );
		}
	}
	else
	{
		const Point2 ptrand(sampler.Get1D(),sampler.Get1D());
		for( int i=0; i<3; i++ ) {
			Scalar specFactor=0;
			Scalar df_unused=0;
			if( GenerateSpecularRay( specular, df_unused, specFactor, myonb, ri, ptrand, NU[i], NV[i], rho[i] ) ) {
				const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), myonb.w() );
				specular.kray = 0.0;
				specular.kray[i] = specFactor * cos_o;
				scattered.AddScatteredRay( specular );
			}
		}
	}

	// Generate diffuse ray and compute the diffuse factor at the actual
	// diffuse direction (not the specular direction).
	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;
	diffuse.isDelta = false;
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(sampler.Get1D(),sampler.Get1D()) ) );

	// Both cosines are against `myonb.w()`, the frame the diffuse lobe was
	// sampled around (see the specular site above): against the raw
	// `ri.onb.w()` a back-face hit gave cos_o_diff < 0 and cos_i < 0, so
	// the stored pdf AND both Schlick-transmission factors collapsed to
	// exactly 0 and the whole diffuse lobe silently carried no weight.
	const Scalar cos_o_diff = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
	diffuse.pdf = r_max( 0.0, cos_o_diff ) * INV_PI;

	// Compute diffuse IS weight: kray = BRDF_diff * cos / pdf
	// BRDF_diff = Rd * (1-Rs) * (28/(23π)) * fromK1(wo) * fromK2(wi)
	// pdf = cos/π, so kray = Rd * (1-Rs) * (28/23) * fromK1 * fromK2
	// (the π from the BRDF normalisation cancels with the π in the pdf)
	const Scalar cos_i = Vector3Ops::Dot( Vector3Ops::Normalize(-ri.ray.Dir()), myonb.w() );
	const Scalar fromK1 = 1.0 - pow( 1.0 - r_max(0.0, cos_o_diff) * 0.5, 5.0 );
	const Scalar fromK2 = 1.0 - pow( 1.0 - r_max(0.0, cos_i) * 0.5, 5.0 );
	static const Scalar diffuseNorm = 28.0 / 23.0;

	const RISEPel oneMinusRs = RISEPel(1,1,1) - rho;
	diffuse.kray = pRd->GetColor(ri) * oneMinusRs * (diffuseNorm * fromK1 * fromK2);

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
		scattered.AddScatteredRay( diffuse );
	}
}

void AshikminShirleyAnisotropicPhongSPF::ScatterNM(
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

	// Geometric-horizon gate for the diffuse lobe below (the specular lobe
	// is gated inside GenerateSpecularRay): a wo that validates against a
	// GlintModifier-tilted shading normal can still point below the
	// geometric surface.  Degenerate vGeomNormal falls back to the shading
	// normal (gate is a no-op).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : myonb.w();
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Scalar NU = pNu->GetValueAtNM(ri,nm);
	const Scalar NV = pNv->GetValueAtNM(ri,nm);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	Scalar specFactor=0;
	Scalar diffuseFactor=0;

	const Scalar rho = GuardedGetColorNM( *pRs, ri, nm );

	if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), NU, NV, rho ) ) {
		// specFactor already includes Fresnel (which contains Rs) — no extra rho.
		// Add cos_o for correct IS weight.  `myonb.w()`, not `ri.onb.w()`
		// -- same back-face sign bug as Scatter()'s RGB twin; see the
		// comment there.
		const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), myonb.w() );
		specular.krayNM = specFactor * cos_o;
		scattered.AddScatteredRay( specular );
	}

	// Generate diffuse ray and compute factor at actual diffuse direction
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.isDelta = false;
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(sampler.Get1D(),sampler.Get1D()) ) );
		// `myonb.w()` for both cosines -- see Scatter()'s RGB twin.
		const Scalar cos_o_diff = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
		diffuse.pdf = r_max( 0.0, cos_o_diff ) * INV_PI;

		const Scalar cos_i = Vector3Ops::Dot( Vector3Ops::Normalize(-ri.ray.Dir()), myonb.w() );
		const Scalar fromK1 = 1.0 - pow( 1.0 - r_max(0.0, cos_o_diff) * 0.5, 5.0 );
		const Scalar fromK2 = 1.0 - pow( 1.0 - r_max(0.0, cos_i) * 0.5, 5.0 );
		static const Scalar diffuseNorm = 28.0 / 23.0;

		diffuse.krayNM = GuardedGetColorNM( *pRd, ri, nm ) * (1.0 - rho) * (diffuseNorm * fromK1 * fromK2);
		if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) > 0 ) {
			scattered.AddScatteredRay( diffuse );
		}
	}
}

Scalar AshikminShirleyAnisotropicPhongSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	// Mirror Scatter()'s FlipW: the lobes are sampled around this frame,
	// not the raw ri.onb.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 n = myonb.w();
	const Vector3 u = myonb.u();
	const Vector3 v = myonb.v();
	const Scalar cos_i = Vector3Ops::Dot( wi, n );
	if( cos_i <= 0 ) {
		return 0;
	}

	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}
	const Scalar cosO = Vector3Ops::Dot( woNorm, n );
	if( cosO <= 0 ) {
		// Matches GenerateSpecularRay's own accept-check (dot(k2,n)<0
		// rejects) and CreateDiffuseVector's natural support -- neither
		// lobe can land here.
		return 0;
	}

	const Scalar diffusePdf = cosO * INV_PI;

	// wD(wo): the diffuse lobe's realized selection weight IS
	// direction-dependent here (unlike Schlick/Phong), because
	// Scatter's diffuse.kray = Rd*(1-Rs)*(28/23)*fromK1(cos_o)*fromK2(cos_i)
	// -- exact at wo, no averaging needed (fromK2(cos_i) is fixed for the
	// whole call).
	//
	// `wDBase` is EXACT, not an approximation: `RandomlySelect` reads
	// `MaxValue(diffuse.kray)`, and `Scatter` forms the whole RISEPel
	// product `Rd*(1-Rs)` FIRST and only then hands it to that reduction,
	// so the scalar is `MaxValue(Rd*(1-Rs))` -- NOT
	// `MaxValue(Rd)*(1-MaxValue(Rs))`, which is a different number
	// whenever the two maxima sit on different channels (Rd=(.9,.1,.1)
	// with Rs=(.1,.9,.1): 0.09 vs 0.81, a factor of 9; at
	// (.95,.05,.05)/(.05,.95,.05) a factor of 19).  The two agree exactly
	// on any grey or co-maximal input, which is why every pre-P1-1 row of
	// AshikminShirleySPFPdfConsistencyTest was blind to it.  The remaining
	// per-direction scalars (fromK1/fromK2/diffuseNorm) are channel-
	// independent and factor cleanly out of the reduction.
	const RISEPel rdCol = pRd->GetColor(ri);
	const RISEPel rhoCol = pRs->GetColor(ri);
	const Scalar wDBase = ColorMath::MaxValue( rdCol * ( RISEPel(1,1,1) - rhoCol ) );
	const Scalar rho = ColorMath::MaxValue( rhoCol );
	static const Scalar diffuseNorm = 28.0 / 23.0;
	const Scalar fromK1 = 1.0 - pow( 1.0 - cosO*0.5, 5.0 );
	const Scalar fromK2 = 1.0 - pow( 1.0 - cos_i*0.5, 5.0 );
	const Scalar wD = wDBase * diffuseNorm * fromK1 * fromK2;

	const ScalarTriple NUt = pNu->GetValuesAt(ri);
	const ScalarTriple NVt = pNv->GetValuesAt(ri);
	AshikminLobeSet lobes;
	if( !pNu->HasPerChannelVariation() && !pNv->HasPerChannelVariation() ) {
		lobes.count = 1;
		lobes.NU[0] = NUt.v[0];
		lobes.NV[0] = NVt.v[0];
		lobes.Rs[0] = rho;
	} else {
		lobes.count = 3;
		for( int i = 0; i < 3; i++ ) {
			lobes.NU[i] = NUt.v[i];
			lobes.NV[i] = NVt.v[i];
			lobes.Rs[i] = rhoCol[i];
		}
	}

	const Scalar wiu = Vector3Ops::Dot( wi, u );
	const Scalar wiv = Vector3Ops::Dot( wi, v );
	const Scalar gu  = Vector3Ops::Dot( geomN, u );
	const Scalar gv  = Vector3Ops::Dot( geomN, v );
	const Scalar gw  = Vector3Ops::Dot( geomN, n );
	const Scalar wiDotGeomN = Vector3Ops::Dot( wi, geomN );

	const Scalar cD = AshikminDiffuseSelectCoefficient( wD, lobes, cos_i, wiu, wiv, wiDotGeomN, gu, gv, gw );

	Scalar specDensity = 0;
	const Vector3 h = Vector3Ops::Normalize( Vector3( wi.x+woNorm.x, wi.y+woNorm.y, wi.z+woNorm.z ) );
	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	if( hdotk > 0 ) {
		const Scalar hn = Vector3Ops::Dot( h, n );
		const Scalar hu = Vector3Ops::Dot( h, u );
		const Scalar hv = Vector3Ops::Dot( h, v );
		specDensity = AshikminSpecularDensity( hdotk, hn, hu, hv, cosO, cos_i, wiu, wiv, wiDotGeomN, gu, gv, gw, wDBase, lobes );
	}

	return cD * diffusePdf + specDensity;
}

Scalar AshikminShirleyAnisotropicPhongSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// Same construction as Pdf() above; ScatterNM has no per-channel
	// branch, so there is always exactly one specular lane.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 n = myonb.w();
	const Vector3 u = myonb.u();
	const Vector3 v = myonb.v();
	const Scalar cos_i = Vector3Ops::Dot( wi, n );
	if( cos_i <= 0 ) {
		return 0;
	}

	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) {
		return 0;
	}
	const Scalar cosO = Vector3Ops::Dot( woNorm, n );
	if( cosO <= 0 ) {
		return 0;
	}

	const Scalar diffusePdf = cosO * INV_PI;

	// Spectral: one wavelength lane, so there is no MaxValue reduction to
	// get the order of and `wDBase` is trivially `rd*(1-rho)`.  Note the
	// values are read RAW, exactly as `ScatterNM` reads them -- the earlier
	// `fabs()` here was a unilateral guard this density's own sampler does
	// not apply, so on a (non-physical) negative reflectance it described a
	// distribution `ScatterNM` does not draw from.  Matching the sampler is
	// the whole contract of this function.
	const Scalar rd  = GuardedGetColorNM( *pRd, ri, nm );
	const Scalar rho = GuardedGetColorNM( *pRs, ri, nm );
	static const Scalar diffuseNorm = 28.0 / 23.0;
	const Scalar fromK1 = 1.0 - pow( 1.0 - cosO*0.5, 5.0 );
	const Scalar fromK2 = 1.0 - pow( 1.0 - cos_i*0.5, 5.0 );
	const Scalar wDBase = rd * (1.0-rho);
	const Scalar wD = wDBase * diffuseNorm * fromK1 * fromK2;

	AshikminLobeSet lobes;
	lobes.count = 1;
	lobes.NU[0] = pNu->GetValueAtNM(ri,nm);
	lobes.NV[0] = pNv->GetValueAtNM(ri,nm);
	lobes.Rs[0] = rho;

	const Scalar wiu = Vector3Ops::Dot( wi, u );
	const Scalar wiv = Vector3Ops::Dot( wi, v );
	const Scalar gu  = Vector3Ops::Dot( geomN, u );
	const Scalar gv  = Vector3Ops::Dot( geomN, v );
	const Scalar gw  = Vector3Ops::Dot( geomN, n );
	const Scalar wiDotGeomN = Vector3Ops::Dot( wi, geomN );

	const Scalar cD = AshikminDiffuseSelectCoefficient( wD, lobes, cos_i, wiu, wiv, wiDotGeomN, gu, gv, gw );

	Scalar specDensity = 0;
	const Vector3 h = Vector3Ops::Normalize( Vector3( wi.x+woNorm.x, wi.y+woNorm.y, wi.z+woNorm.z ) );
	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	if( hdotk > 0 ) {
		const Scalar hn = Vector3Ops::Dot( h, n );
		const Scalar hu = Vector3Ops::Dot( h, u );
		const Scalar hv = Vector3Ops::Dot( h, v );
		specDensity = AshikminSpecularDensity( hdotk, hn, hu, hv, cosO, cos_i, wiu, wiv, wiDotGeomN, gu, gv, gw, wDBase, lobes );
	}

	return cD * diffusePdf + specDensity;
}

//////////////////////////////////////////////////////////////////////
// EvaluateKrayNM -- DL-125.
//
// Returns the `krayNM` `ScatterNM` itself would have stamped on this
// lobe had `nm` been the hero wavelength, for the SAME outgoing
// direction.  Both lobes are recoverable from `(ri, outDir, nm)`:
//
//   diffuse:   Rd(nm) * (1 - Rs(nm)) * (28/23) * K1(cos_o) * K2(cos_i)
//   specular:  (brdf_S / p_S) * cos_o
//
// The diffuse lobe's own sampled direction IS `outDir`, so DL-99's
// "an Ashikmin diffuse weight depends on its own draw" hazard does not
// bite here -- the draw is the argument.
//
// The specular lobe replays `GenerateSpecularRay`'s own density from
// the recovered half-vector: `h = normalize(wi + wo)` (exact -- `wo`
// is the mirror of `-wi` about `h`), the sampled azimuth `phi` read
// back off `h`'s tangential components in the SAMPLING frame (the
// sampler builds `h` as `(cos_phi sin_theta, sin_phi sin_theta,
// cos_theta)` in that frame, so `cos_phi = (h.u)/sin_theta`), and
// `p_S = factor1 * factor2 / (4 (h.wi))` exactly as the sampler stores
// it.  At the pole (`sin_theta -> 0`) the azimuth is undefined and
// irrelevant: `factor2 = pow(h.n, ...)` -> 1 for any exponent.
//
// `NU`, `NV` and both reflectances are read at `nm`; no sampler draw
// is consumed.
//////////////////////////////////////////////////////////////////////
Scalar AshikminShirleyAnisotropicPhongSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	// Rebuild ScatterNM's sampling frame exactly (post-FlipW on a
	// back-face hit -- DL-100).
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Scalar rho = GuardedGetColorNM( *pRs, ri, nm );

	if( rayType == ScatteredRay::eRayDiffuse ) {
		const Scalar cos_o_diff = Vector3Ops::Dot( outDir, myonb.w() );
		const Scalar cos_i = Vector3Ops::Dot(
			Vector3Ops::Normalize( -ri.ray.Dir() ), myonb.w() );
		const Scalar fromK1 = 1.0 - pow( 1.0 - r_max(0.0, cos_o_diff) * 0.5, 5.0 );
		const Scalar fromK2 = 1.0 - pow( 1.0 - r_max(0.0, cos_i) * 0.5, 5.0 );
		static const Scalar diffuseNorm = 28.0 / 23.0;
		return GuardedGetColorNM( *pRd, ri, nm ) * (1.0 - rho) * (diffuseNorm * fromK1 * fromK2);
	}

	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;						// not a lobe this SPF emits
	}

	const Scalar NU = pNu->GetValueAtNM( ri, nm );
	const Scalar NV = pNv->GetValueAtNM( ri, nm );

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 k2 = Vector3Ops::Normalize( outDir );
	const Vector3 h = Vector3Ops::Normalize( wi + k2 );

	const Scalar hdotk = Vector3Ops::Dot( h, wi );
	const Scalar hdotn = Vector3Ops::Dot( myonb.w(), h );
	if( hdotk <= 0 || hdotn <= 0 ) {
		return 0;						// a genuine zero, not "unimplemented"
	}

	// Recover the sampler's own azimuth from h's tangential part.
	const Scalar sin_theta_sq = r_max( Scalar(0), Scalar(1) - hdotn*hdotn );
	Scalar cos_phi = 1.0, sin_phi = 0.0;
	if( sin_theta_sq > 1e-24 ) {
		const Scalar inv_sin_theta = 1.0 / sqrt( sin_theta_sq );
		cos_phi = Vector3Ops::Dot( h, myonb.u() ) * inv_sin_theta;
		sin_phi = Vector3Ops::Dot( h, myonb.v() ) * inv_sin_theta;
	}

	const Scalar factor1 = sqrt((NU+1.0)*(NV+1.0)) / TWO_PI;
	const Scalar factor2 = pow( hdotn, (NU*cos_phi*cos_phi + NV*sin_phi*sin_phi) );
	const Scalar density = (factor1 * factor2) / (4.0 * hdotk);
	if( density <= 0 ) {
		return 0;
	}

	Scalar diffuseFactor = 0, brdf = 0;
	AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors(
		diffuseFactor, brdf, k2, ri, myonb.w(), myonb.u(), myonb.v(), NU, NV, rho );

	const Scalar cos_o = Vector3Ops::Dot( k2, myonb.w() );
	return ( brdf / density ) * cos_o;
}
