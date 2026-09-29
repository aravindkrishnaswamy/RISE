//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianSPF.cpp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WardSelectionQuadrature.h"
#include "WardIsotropicGaussianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

WardIsotropicGaussianSPF::WardIsotropicGaussianSPF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IScalarPainter& alpha_
	) :
  pDiffuse( &diffuse_ ),
  pSpecular( &specular_ ),
  pAlpha( &alpha_ )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlpha->addref();
}

WardIsotropicGaussianSPF::~WardIsotropicGaussianSPF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlpha );
}

void WardIsotropicGaussianSPF::SetDiffuse( const IPainter& v )      { v.addref(); safe_release( pDiffuse );  pDiffuse  = &v; }
void WardIsotropicGaussianSPF::SetSpecular( const IPainter& v )     { v.addref(); safe_release( pSpecular ); pSpecular = &v; }
void WardIsotropicGaussianSPF::SetAlpha( const IScalarPainter& v )  { v.addref(); safe_release( pAlpha );    pAlpha    = &v; }

//! DL-212 integrates realized selection weights over their exact horizon
//! domains; see WardSelectionQuadrature.h. No midpoint rejection grid.

//! THE TRUE solid-angle density of the half-vector `GenerateSpecularRay`
//! draws.
//!
//! DL-177 (docs/DL177_WARD_DENSITY_AND_KRAY.md).  The sampler draws
//! `phi` uniformly and `theta = atan(alpha*sqrt(-ln xi))`.  Writing
//! `t = tan(theta)`, `xi = exp(-t^2/alpha^2)`, so
//! `P(T <= t) = 1 - exp(-t^2/alpha^2)` and
//! `p_T(t) = (2t/alpha^2) exp(-t^2/alpha^2)`.  With
//! `dt/dtheta = 1/cos^2(theta)` that is
//! `p_Theta = (2 tan/alpha^2) exp(...) / cos^2`, and a solid-angle
//! density is `p_Theta * p_Phi / sin(theta)` with `p_Phi = 1/(2 PI)`:
//!
//!     p_h = exp(-tan^2/alpha^2) / (PI alpha^2 cos^3(theta_h))
//!
//! Until 2026-09-18 both this file and the anisotropic twin stored
//! `cos(theta_h) * exp(...) / (PI alpha^2)` instead -- the true density
//! times `cos^4(theta_h)`, measured exactly that way per draw in
//! `tests/WardDensityKrayTest.cpp` section C.
static inline Scalar WardIsoHalfDensity( const Scalar cosThetaH, const Scalar alphaSq )
{
	if( cosThetaH <= 0 || alphaSq <= 0 ) {
		return 0;
	}
	const Scalar c2 = cosThetaH * cosThetaH;
	const Scalar tan2 = ( 1.0 - c2 ) / c2;
	return exp( -tan2 / alphaSq ) / ( PI * alphaSq * c2 * cosThetaH );
}

//! DL-212, Geisler-Moroder & Duer (2010):
//! f_S = Rs exp(-slope^2) / (4 pi ax ay (h.wi)^2 (n.h)^4).
//! The unchanged p_S = exp(-slope^2)/(4 pi ax ay (n.h)^3 (h.wi))
//! gives kray/Rs = cos_o/((h.wi)(n.h)) = 2 cos_o/(cos_i+cos_o).
//! Roughness cancels; the weight is globally bounded by 2 Rs.
static inline Scalar WardKrayRatio(
	const Scalar hdotwo,
	const Scalar cosThetaH,
	const Scalar cosO,
	const Scalar cosI
	)
{
	if( hdotwo <= 0 || cosThetaH <= 0 || cosO <= 0 || cosI <= 0 ) {
		return 0;
	}
	return 2.0 * cosO / (cosI + cosO);
}

//! Probability `Scatter`'s diffuse ray survives its geometric-horizon
//! gate: the exact fraction of a cosine-weighted hemisphere about `n`
//! that lies above the plane of `geomN`.  Malley's disk projection turns
//! the clipped region into a half-disk plus a half-ellipse of semi-axes
//! (cos phi, 1), giving `(1 + cos phi)/2` -- the closed form DL-45 uses
//! for TranslucentSPF's tilted exit and `SchlickSPF` uses for the same
//! purpose.
static inline Scalar WardDiffuseAcceptFraction(
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN
	)
{
	const Scalar c = Vector3Ops::Dot( myonb.w(), geomN );
	return r_max( Scalar(0), r_min( Scalar(1), 0.5 * (1.0 + c) ) );
}

//! The specular lobes `Scatter` emits in one call: up to three, one per
//! channel, when the alpha painter varies per channel.
struct WardIsoLobeSet
{
	int    count;
	Scalar alpha[3];
	//! The scalar `RandomlySelect` will reduce this lobe's `kray` to,
	//! EXCLUDING the direction-dependent ratio.  `RandomlySelect` reads
	//! `MaxValue(kray)` and the ratio is a nonnegative SCALAR, so
	//! `MaxValue(Rs * R) == MaxValue(Rs) * R` exactly -- Ward escapes
	//! DL-99's reduction-order trap, where the two colours varied
	//! independently.
	Scalar w[3];
};

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
	diffuse.isDelta = false;
}

static void GenerateSpecularRay(
	ScatteredRay& specular,
	const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
	const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
	const Point2& random,										///< [in] Two random numbers
	const Scalar alpha,
	Scalar& outKrayRatio										///< [out] DL-177: `f_S cos / (p_S Rs)` for the sampled direction, 0 if no ray was produced
	)
{
	outKrayRatio = 0;
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = false;

	const Scalar phi = TWO_PI * random.x;
	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar theta = atan(alpha*(sqrt(-log(random.y))));

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector.  FRAME (DL-100,
	// fixed 2026-09-17): `onb`, the caller's SAMPLING frame -- Scatter's
	// `myonb`, post-FlipW on a back-face hit -- not `ri.onb`.  Before the
	// fix this `onb` parameter was DEAD: it always read `ri.onb` directly,
	// so on a back-face hit the half-vector was built around the
	// UNFLIPPED normal while Scatter's own accept-check tested the
	// FLIPPED `myonb.w()` -- every specular draw was then on the wrong
	// side and got rejected, silently losing the whole specular lobe.
	// See docs/DL100_SCHLICK_BACKFACE_SPECULAR.md (same pattern as
	// SchlickSPF, fixed in the same slice).
	const Vector3	h(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z,
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z,
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.Dir());

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.Dir() + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );

		// DL-177 defect (1): the stored density is the TRUE solid-angle
		// density of the half-vector this sampler draws, converted by
		// the reflection Jacobian `1/(4 (h.wo))`.  It used to be
		// `cos(theta_h)*exp(...)/(PI alpha^2)` -- that same density
		// times `cos^4(theta_h)` -- so `Pdf`, MIS and the per-lobe
		// pairing were all quoting a function that is not a density.
		const Scalar alpha_sq = alpha * alpha;
		const Scalar pdf_h = WardIsoHalfDensity( cos_theta, alpha_sq );
		const Scalar hdotwo = Vector3Ops::Dot( h, ret );
		specular.pdf = ( hdotwo > 0 ) ? ( pdf_h / (4.0 * hdotwo) ) : Scalar(0);

		// DL-177 defect (3): the transport weight this lobe must carry,
		// minus the reflectance the caller multiplies in.
		const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
		outKrayRatio = WardKrayRatio( hdotwo, cos_theta,
		                              Vector3Ops::Dot( ret, onb.w() ),
		                              Vector3Ops::Dot( wi, onb.w() ) );
	}
}

void WardIsotropicGaussianSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in] Index of refraction stack
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

	ScatteredRay d;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );

	// Accept-check tests myonb.w() (the frame lobes are actually sampled
	// around, post-FlipW) rather than the raw ri.onb.w() -- on a back-face
	// hit the two differ by sign, and testing the unflipped normal here
	// silently dropped every legitimately-sampled back-face lobe.
	if( Vector3Ops::Dot( d.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( d.ray.Dir(), geomN ) > 0.0 ) {
		d.kray = pDiffuse->GetColor(ri);
		// Cosine-weighted hemisphere: pdf = cos(theta) / PI
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}

	const ScalarTriple at = pAlpha->GetValuesAt(ri);
	const Scalar a[3] = { at.v[0], at.v[1], at.v[2] };

	if( !pAlpha->HasPerChannelVariation() )
	{
		ScatteredRay s;
		Scalar ratio = 0;
		GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), a[0], ratio );

		// Accept-check uses myonb.w() -- see the diffuse-lobe comment above.
		if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
			// DL-177 defect (3): `kray_I` is this lobe's own
			// `f_I cos / p_I`, not the reflectance painter's colour.
			s.kray = pSpecular->GetColor(ri) * ratio;
			scattered.AddScatteredRay( s );
		}
	}
	else
	{
		const Point2 ptrand( sampler.Get1D(),sampler.Get1D() );
		const RISEPel spec = pSpecular->GetColor(ri);
		for( int i=0; i<3; i++ ) {
			// DL-101: a FRESH ScatteredRay every iteration -- see
			// SchlickSPF.cpp's identical fix and
			// docs/DL101_PERCHANNEL_SCATTEREDRAY_REUSE.md.
			ScatteredRay s;
			Scalar ratio = 0;
			GenerateSpecularRay( s, myonb, ri, ptrand, a[i], ratio );

			// Accept-check uses myonb.w() -- see the diffuse-lobe comment above.
			if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i] * ratio;
				scattered.AddScatteredRay( s );
			}
		}
	}
}


void WardIsotropicGaussianSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in] Index of refraction stack
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
	Scalar sRatio = 0;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );
	GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), pAlpha->GetValueAtNM(ri,nm), sRatio );

	// Accept-checks use myonb.w() -- see Scatter()'s comment: it is the
	// frame lobes are actually sampled around (post-FlipW), not the raw
	// ri.onb.w() which differs by sign on a back-face hit.
	if( Vector3Ops::Dot( d.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( d.ray.Dir(), geomN ) > 0.0 ) {
		d.krayNM = GuardedGetColorNM( *pDiffuse, ri, nm );
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}
	if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
		// DL-177 defect (3), NM twin of Scatter's line.
		s.krayNM = GuardedGetColorNM( *pSpecular, ri, nm ) * sRatio;
		scattered.AddScatteredRay( s );
	}
}

//! C_D -- the coefficient the DIFFUSE sampling density carries in the
//! aggregate: the probability `RandomlySelect` returns the diffuse ray,
//! averaged over the specular draw it is competing against.
//!
//! DL-177 defect (2) (docs/DL177_WARD_DENSITY_AND_KRAY.md).  Before the
//! fix `Pdf` was a RAW-ALBEDO mixture `(wD p_D + wS p_S)/(wD + wS)`,
//! which was exact only while `kray_S` was the reflectance painter's
//! direction-INDEPENDENT colour.  Once defect (3) makes `kray_S` carry
//! `WardKrayRatio`, `RandomlySelect`'s realized weight moves with the
//! specular ray's own direction, so `C_D` is an EXPECTATION over that
//! draw -- and its dominant term is usually the specular sampler's
//! REJECTION rate (`hdotk <= 0`, or a reflected direction below a
//! horizon), which leaves `RandomlySelect` holding one ray that it
//! returns with probability 1 whatever its weight.  No closed form over
//! reflectances can see that. DL-212 integrates the same joint sampler
//! measure after splitting its actual horizon domains, avoiding the
//! former midpoint replay's geometric-boundary mass bias.
static Scalar WardIsoDiffuseSelectCoefficient(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Scalar wD,
	const WardIsoLobeSet& lobes
	)
{
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	WardSelection::Input input = {};
	input.vx = Vector3Ops::Dot( v, myonb.u() );
	input.vy = Vector3Ops::Dot( v, myonb.v() );
	input.nv = Vector3Ops::Dot( v, myonb.w() );
	input.gx = Vector3Ops::Dot( geomN, myonb.u() );
	input.gy = Vector3Ops::Dot( geomN, myonb.v() );
	input.ng = Vector3Ops::Dot( geomN, myonb.w() );
	input.wD = wD;
	input.count = lobes.count;
	for( int j = 0; j < lobes.count; ++j ) {
		input.ax[j] = lobes.alpha[j];
		input.ay[j] = lobes.alpha[j];
		input.weight[j] = lobes.w[j];
	}
	return WardSelection::Evaluate( input );
}

//! `sum_i q_i(wo) p_i(wo)` -- the specular half of the aggregate.  `aD`
//! is the probability Scatter's diffuse ray survived its own
//! geometric-horizon gate (1 whenever the shading and geometric normals
//! agree).
static Scalar WardIsoSpecularDensity(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Vector3& woNorm,
	const Scalar wD,
	const Scalar aD,
	const WardIsoLobeSet& lobes
	)
{
	const Vector3& ew = myonb.w();
	const Vector3  wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3  h  = Vector3Ops::Normalize( wi + woNorm );

	const Scalar cosThetaH = Vector3Ops::Dot( h, ew );
	const Scalar hdotwo    = Vector3Ops::Dot( h, woNorm );
	if( cosThetaH <= 0 || hdotwo <= 0 ) {
		return 0;
	}

	const Scalar nv   = Vector3Ops::Dot( ew, wi );
	const Scalar cosO = Vector3Ops::Dot( woNorm, ew );

	// The ratio depends on `h`, `wo` and the frame only -- NOT on alpha
	// -- so every lane at this query direction shares it.
	const Scalar ratioAtWo = WardKrayRatio( hdotwo, cosThetaH, cosO, nv );

	// The azimuth of `h` in the sampling frame; every lane that shares
	// this call's `(xi1, xi2)` draws the SAME azimuth.
	const Scalar hu = Vector3Ops::Dot( h, myonb.u() );
	const Scalar hv = Vector3Ops::Dot( h, myonb.v() );
	const Scalar hr = sqrt( hu*hu + hv*hv );
	const Scalar cosP = ( hr > NEARZERO ) ? ( hu / hr ) : Scalar(1);
	const Scalar sinP = ( hr > NEARZERO ) ? ( hv / hr ) : Scalar(0);

	const Scalar tanThetaH = sqrt( r_max( Scalar(0), 1.0 - cosThetaH*cosThetaH ) ) / cosThetaH;

	Scalar sum = 0;

	for( int i = 0; i < lobes.count; i++ ) {
		const Scalar pdf_i = WardIsoHalfDensity( cosThetaH, lobes.alpha[i]*lobes.alpha[i] )
		                   / ( 4.0 * hdotwo );
		if( pdf_i <= 0 ) {
			continue;
		}

		const Scalar w_i = lobes.w[i] * ratioAtWo;

		// What the OTHER lanes drew.  They share lane i's random pair,
		// and this sampler inverts in closed form: `xi1` fixes the
		// azimuth (shared) and `tan(theta) = alpha sqrt(-ln xi2)`, so
		// lane j's polar angle is simply `(alpha_j/alpha_i)` times lane
		// i's tangent -- no exp/log round trip needed.
		Scalar wOther = 0;
		int nAcceptedSpec = 1;
		if( lobes.count > 1 && lobes.alpha[i] > NEARZERO ) {
			const Vector3& d = ri.ray.Dir();
			for( int j = 0; j < lobes.count; j++ ) {
				if( j == i ) {
					continue;
				}
				const Scalar tj = ( lobes.alpha[j] / lobes.alpha[i] ) * tanThetaH;
				const Scalar cj = 1.0 / sqrt( 1.0 + tj*tj );
				const Scalar sj = tj * cj;
				const Scalar lx = cosP * sj;
				const Scalar ly = sinP * sj;
				const Vector3 hj( myonb.u().x*lx + myonb.v().x*ly + ew.x*cj,
				                  myonb.u().y*lx + myonb.v().y*ly + ew.y*cj,
				                  myonb.u().z*lx + myonb.v().z*ly + ew.z*cj );
				const Scalar hdotk = -Vector3Ops::Dot( hj, d );
				if( hdotk <= 0 ) {
					continue;
				}
				const Vector3 rj = Vector3Ops::Normalize( d + 2.0 * hdotk * hj );
				if( Vector3Ops::Dot( rj, ew ) <= 0 || Vector3Ops::Dot( rj, geomN ) <= 0 ) {
					continue;
				}
				wOther += lobes.w[j] * WardKrayRatio( Vector3Ops::Dot( hj, rj ), cj,
				                                      Vector3Ops::Dot( rj, ew ), nv );
				nAcceptedSpec++;
			}
		}

		const Scalar wS = w_i + wOther;

		Scalar q = 0;
		// ...with the diffuse ray present (probability aD).
		const Scalar totalWith = wD + wS;
		if( totalWith > NEARZERO ) {
			q += aD * ( w_i / totalWith );
		}
		// ...and without it (probability 1-aD), where a lone specular
		// ray wins outright through RandomlySelect's freeidx==1
		// short-circuit.
		if( aD < 1.0 ) {
			if( nAcceptedSpec == 1 ) {
				q += ( 1.0 - aD );
			} else if( wS > NEARZERO ) {
				q += ( 1.0 - aD ) * ( w_i / wS );
			}
		}

		sum += q * pdf_i;
	}

	return sum;
}

//! The density of the direction `Scatter` + `RandomlySelect` actually
//! produce.  DL-177 defect (2): this used to be a raw-albedo-weighted
//! mixture of the two lobe densities.
static Scalar WardIsotropicPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const WardIsoLobeSet& lobes,
	const Scalar wDiff
	)
{
	// Mirror Scatter()'s FlipW: orient the frame to face the incoming ray so
	// this Pdf agrees with Scatter's actual sampling frame on backface hits
	// (Scatter samples both lobes relative to the flipped myonb, so a raw
	// ri.onb.w() here returned 0 for directions Scatter legitimately emits).
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 n = myonb.w();

	const Scalar cos_theta_i = Vector3Ops::Dot( wi, n );
	const Scalar cos_theta_o = Vector3Ops::Dot( wo, n );

	if( cos_theta_i <= 0.0 || cos_theta_o <= 0.0 ) {
		return 0.0;
	}

	// Geometric-horizon gate (MIS consistency with the sampler-side gates):
	// a wo the sampler can no longer emit contributes zero density.  BOTH
	// lobes carry this gate in Scatter, so a direction that fails it has
	// zero density outright.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( wo, geomN ) <= 0 ) {
		return 0.0;
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	const Scalar aD = WardDiffuseAcceptFraction( myonb, geomN );
	const Scalar cD = WardIsoDiffuseSelectCoefficient( ri, myonb, geomN, wDiff, lobes );

	// Diffuse lobe: cosine-weighted hemisphere about the sampling frame.
	const Scalar pdf_diffuse = Vector3Ops::Dot( woNorm, n ) * INV_PI;

	return cD * pdf_diffuse
	     + WardIsoSpecularDensity( ri, myonb, geomN, woNorm, wDiff, aD, lobes );
}

Scalar WardIsotropicGaussianSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	const ScalarTriple at = pAlpha->GetValuesAt(ri);
	const RISEPel spec = pSpecular->GetColor(ri);

	// Mirror Scatter's own branch EXACTLY: one lobe at `at.v[0]` unless
	// the alpha painter varies per channel, in which case three, each
	// with a single live channel in its `kray`.
	WardIsoLobeSet lobes;
	if( !pAlpha->HasPerChannelVariation() ) {
		lobes.count = 1;
		lobes.alpha[0] = at.v[0];
		lobes.w[0] = ColorMath::MaxValue( spec );
	} else {
		lobes.count = 3;
		for( int i = 0; i < 3; i++ ) {
			lobes.alpha[i] = at.v[i];
			lobes.w[i] = spec[i];
		}
	}

	const Scalar wDiff = ColorMath::MaxValue( pDiffuse->GetColor(ri) );

	return WardIsotropicPdf( ri, wo, lobes, wDiff );
}

Scalar WardIsotropicGaussianSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// ScatterNM emits exactly one specular lobe, and RandomlySelect
	// weights the NM lanes by `krayNM` (not `MaxValue(kray)`).
	WardIsoLobeSet lobes;
	lobes.count = 1;
	lobes.alpha[0] = pAlpha->GetValueAtNM(ri,nm);
	lobes.w[0] = fabs( GuardedGetColorNM( *pSpecular, ri, nm ) );

	const Scalar wDiff = fabs( GuardedGetColorNM( *pDiffuse, ri, nm ) );

	return WardIsotropicPdf( ri, wo, lobes, wDiff );
}

//////////////////////////////////////////////////////////////////////
// EvaluateKrayNM -- DL-125.
//
// Returns the `krayNM` `ScatterNM` itself would have stamped on this
// lobe had `nm` been the hero wavelength, for the SAME outgoing
// direction:
//
//   diffuse:   Rd(nm)                                 -- direction-free
//   specular:  Rs(nm) * WardKrayRatio(h.wo, cos_h, cos_o, cos_i)
//
// ALPHA DOES NOT APPEAR, and that is not an omission: DL-177's
// derivation shows the `exp(-tan^2/alpha^2)` and the whole `alpha`
// dependence cancel between `f_S cos_o` and `p_S`, leaving
// `Rs * 2 cos_o/(cos_i+cos_o)` after DL-212. So only the
// reflectance painters are read at `nm`.  The half-vector is recovered
// exactly (`wo` is the mirror of `-wi` about `h`, so `wi + wo` is
// parallel to `h`); no sampler draw is consumed.
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// EvaluateLobeFNM -- DL-216.
//
// Evaluates the SELECTED lobe's own spectral BSDF value f_I(wo; nm)
// in [1/sr], without multiplying by cosine and without dividing by
// any sampling density.
//////////////////////////////////////////////////////////////////////
Scalar WardIsotropicGaussianSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return GuardedGetColorNM( *pDiffuse, ri, nm ) * INV_PI;
	}

	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;						// not a lobe this SPF emits
	}

	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 woNorm = Vector3Ops::Normalize( outDir );
	const Vector3 h = Vector3Ops::Normalize( wi + woNorm );

	const Scalar hdotwo = Vector3Ops::Dot( h, woNorm );
	const Scalar cos_h = Vector3Ops::Dot( h, myonb.w() );
	const Scalar cos_o = Vector3Ops::Dot( woNorm, myonb.w() );
	const Scalar cos_i = Vector3Ops::Dot( wi, myonb.w() );

	if( hdotwo <= 0 || cos_h <= 0 || cos_o <= 0 || cos_i <= 0 ) {
		return 0;
	}

	const Scalar ratio = WardKrayRatio( hdotwo, cos_h, cos_o, cos_i );
	if( ratio <= 0 ) {
		return 0;
	}

	const Scalar kray = GuardedGetColorNM( *pSpecular, ri, nm ) * ratio;
	const Scalar alpha = pAlpha->GetValueAtNM( ri, nm );
	const Scalar pdf_h = WardIsoHalfDensity( cos_h, alpha * alpha );
	const Scalar pdf = pdf_h / (4.0 * hdotwo);

	return ( kray * pdf ) / cos_o;
}

Scalar WardIsotropicGaussianSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return GuardedGetColorNM( *pDiffuse, ri, nm );
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

	const Scalar ratio = WardKrayRatio(
		Vector3Ops::Dot( h, woNorm ),
		Vector3Ops::Dot( h, myonb.w() ),
		Vector3Ops::Dot( woNorm, myonb.w() ),
		Vector3Ops::Dot( wi, myonb.w() ) );
	if( ratio <= 0 ) {
		return 0;						// a genuine zero, not "unimplemented"
	}

	return GuardedGetColorNM( *pSpecular, ri, nm ) * ratio;
}

Scalar WardIsotropicGaussianSPF::EvaluateKrayNM(
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
		return GuardedGetColorNM( *pDiffuse, ri, nm );
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
