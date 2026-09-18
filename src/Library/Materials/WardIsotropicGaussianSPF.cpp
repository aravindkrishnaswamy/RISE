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

//! Stratified grid `Pdf()`/`PdfNM()` replay the specular sampler's own
//! `(xi1, xi2)` square on to estimate `C_D` (see
//! WardIsoDiffuseSelectCoefficient).  kWardQuadN^2 replays per call.
//! Matches `SchlickSPF`'s `kSpecQuadN` and the DL-98/DL-99 grids, and
//! the same cost/accuracy note applies: the integrand is bounded in
//! [0,1] but not smooth (the accept test is a step), so the error is
//! O(1/kWardQuadN), not O(1/kWardQuadN^2).
static const int kWardQuadN = 16;

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

//! `f_S cos_o / p_S` divided by the specular reflectance -- the factor
//! the specular lobe's `kray` must carry (DL-177 defect 3; the same
//! shape as DL-127's `R` for Schlick).
//!
//! `WardIsotropicGaussianBRDF::ComputeFactors` returns
//! `Rs exp(-tan^2/alpha^2) / (4 PI alpha^2 sqrt(nr nv))` with
//! `nr = (n.wi)` and `nv = (n.wo)`, and `p_S = p_h / (4 (h.wo))` with
//! `p_h` above, so the exponential AND the whole `alpha` dependence
//! cancel:
//!
//!     f_S cos_o / p_S = Rs * (h.wo) * cos^3(theta_h) * sqrt(cos_o/cos_i)
//!
//! Every factor but the last is at most 1 and `cos_o <= 1`, so the
//! weight is bounded above by `Rs / sqrt(cos_i)` -- a per-shading-point
//! constant, not a per-draw divergence.  The anisotropic twin's ratio is
//! the SAME expression (its `ax*ay` prefactor cancels identically).
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
	return hdotwo * cosThetaH * cosThetaH * cosThetaH * sqrt( cosO / cosI );
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
//! reflectances can see that, so this is the DL-67/DL-98/DL-99
//! deterministic stratified replay of the sampler's own `(xi1, xi2)`
//! square.
static Scalar WardIsoDiffuseSelectCoefficient(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Scalar wD,
	const WardIsoLobeSet& lobes
	)
{
	// Rows: the azimuth is shared by every lane (this sampler draws
	// `phi = 2 PI xi1` with no alpha dependence at all), and the polar
	// angle is `atan(alpha_j sqrt(-ln xi2))`, so each lane's row is the
	// same `sqrt(-ln xi2)` scaled by its own alpha.
	Scalar cosP[kWardQuadN], sinP[kWardQuadN];
	Scalar cosT[3][kWardQuadN], sinT[3][kWardQuadN];

	const Scalar inv = 1.0 / Scalar(kWardQuadN);
	for( int b = 0; b < kWardQuadN; b++ ) {
		const Scalar phi = TWO_PI * ( Scalar(b) + 0.5 ) * inv;
		cosP[b] = cos( phi );
		sinP[b] = sin( phi );
	}
	for( int a = 0; a < kWardQuadN; a++ ) {
		const Scalar xi = ( Scalar(a) + 0.5 ) * inv;
		const Scalar root = sqrt( -log( xi ) );
		for( int j = 0; j < lobes.count; j++ ) {
			const Scalar t = lobes.alpha[j] * root;
			const Scalar ct = 1.0 / sqrt( 1.0 + t*t );
			cosT[j][a] = ct;
			sinT[j][a] = t * ct;
		}
	}

	const Vector3& eu = myonb.u();
	const Vector3& ev = myonb.v();
	const Vector3& ew = myonb.w();
	// The sampler reflects about `h` and NORMALIZES; normalising the
	// INCOMING direction once instead makes every reflected direction
	// unit by construction, which lets the inner loop below drop the
	// per-node `Normalize` (a sqrt and three divides) and read the two
	// quantities it needs off closed forms:
	//     (h . wo)  ==  (h . wi)            (reflection about a unit h)
	//     (n . wo)  ==  (n . d) + 2 (h.wi) (n . h)
	// Both are EXACT for a unit `d`, not approximations, and the two
	// accept gates only need signs.  `(n . d)` is exactly `-nvView`
	// below, since `nvView` is `(n . wi)` and `wi == -d`.
	const Vector3  dHat = Vector3Ops::Normalize( ri.ray.Dir() );
	const Scalar   nvView = -Vector3Ops::Dot( ew, dHat );
	const Scalar   dDotG  = Vector3Ops::Dot( dHat, geomN );

	Scalar accum = 0;

	for( int a = 0; a < kWardQuadN; a++ ) {
		for( int b = 0; b < kWardQuadN; b++ ) {
			Scalar wS = 0;
			int nAcceptedSpec = 0;

			for( int j = 0; j < lobes.count; j++ ) {
				const Scalar st = sinT[j][a];
				const Scalar lx = cosP[b] * st;
				const Scalar ly = sinP[b] * st;
				const Scalar lz = cosT[j][a];

				const Scalar hx = eu.x*lx + ev.x*ly + ew.x*lz;
				const Scalar hy = eu.y*lx + ev.y*ly + ew.y*lz;
				const Scalar hz = eu.z*lx + ev.z*ly + ew.z*lz;

				const Scalar hdotk = -( hx*dHat.x + hy*dHat.y + hz*dHat.z );
				if( hdotk <= 0 ) {
					// GenerateSpecularRay leaves the ray untouched and
					// Scatter's accept-check drops it.
					continue;
				}

				// (n . wo) = (n . d) + 2 (h.wi) (n . h), and (n . d) is
				// exactly -nvView.
				const Scalar cosO = -nvView + 2.0 * hdotk * lz;
				if( cosO <= 0 ) {
					continue;
				}
				const Scalar hDotG = hx*geomN.x + hy*geomN.y + hz*geomN.z;
				if( dDotG + 2.0 * hdotk * hDotG <= 0 ) {
					continue;
				}

				wS += lobes.w[j] * WardKrayRatio( hdotk, lz, cosO, nvView );
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

	return accum / Scalar(kWardQuadN*kWardQuadN);
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
