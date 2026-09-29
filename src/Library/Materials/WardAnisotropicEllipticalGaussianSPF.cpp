//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianSPF.cpp
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
#include "WardAnisotropicEllipticalGaussianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

WardAnisotropicEllipticalGaussianSPF::WardAnisotropicEllipticalGaussianSPF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IScalarPainter& alphax_,
	const IScalarPainter& alphay_
	) :
  pDiffuse( &diffuse_ ),
  pSpecular( &specular_ ),
  pAlphaX( &alphax_ ),
  pAlphaY( &alphay_ )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlphaX->addref();
	pAlphaY->addref();
}

WardAnisotropicEllipticalGaussianSPF::~WardAnisotropicEllipticalGaussianSPF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlphaX );
	safe_release( pAlphaY );
}

void WardAnisotropicEllipticalGaussianSPF::SetDiffuse( const IPainter& v )      { v.addref(); safe_release( pDiffuse );  pDiffuse  = &v; }
void WardAnisotropicEllipticalGaussianSPF::SetSpecular( const IPainter& v )     { v.addref(); safe_release( pSpecular ); pSpecular = &v; }
void WardAnisotropicEllipticalGaussianSPF::SetAlphaX( const IScalarPainter& v ) { v.addref(); safe_release( pAlphaX );   pAlphaX   = &v; }
void WardAnisotropicEllipticalGaussianSPF::SetAlphaY( const IScalarPainter& v ) { v.addref(); safe_release( pAlphaY );   pAlphaY   = &v; }

//! DL-212 integrates realized selection weights over their exact horizon
//! domains; see WardSelectionQuadrature.h. No midpoint rejection grid.

//! Ward's azimuthal warp, extracted VERBATIM from `GenerateSpecularRay`
//! so the sampler and everything that replays it cannot drift apart.
static inline Scalar WardAnisoPhiFromXi( const Scalar xi, const Scalar alphax, const Scalar alphay )
{
	const Scalar alpha_ratio = ( alphay / alphax );
	Scalar phi = 0;
	if( xi < 0.25 )
	{
//		Scalar val = 1.0 - 4*(0.25 - p.x);		reduces to -->
		Scalar val = 4.0 * xi;
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
	}
	else if( xi < 0.5 )
	{
		Scalar val = 1.0 - 4*(0.5 - xi);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	}
	else if( xi < 0.75 )
	{
		Scalar val = 4*(xi - 0.5);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi += PI;
	}
	else
	{
		Scalar val = 1.0 - 4*(1.0 - xi);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}
	return phi;
}

//! The exact inverse of `WardAnisoPhiFromXi` (DL-177 defect 2 needs it to
//! recover the random pair a per-channel lane shared with its siblings).
//! `phi` is taken in `[0, 2 PI)`.
static inline Scalar WardAnisoXiFromPhi( const Scalar phi, const Scalar alphax, const Scalar alphay )
{
	const Scalar inv_ratio = ( alphax / alphay );
	// Fold to the first quadrant, remembering which one we came from.
	if( phi < PI_OV_TWO ) {
		const Scalar val = ( 2.0 / PI ) * atan( inv_ratio * tan( phi ) );
		return 0.25 * val;
	} else if( phi < PI ) {
		const Scalar val = ( 2.0 / PI ) * atan( inv_ratio * tan( PI - phi ) );
		return 0.25 + 0.25 * val;
	} else if( phi < PI + PI_OV_TWO ) {
		const Scalar val = ( 2.0 / PI ) * atan( inv_ratio * tan( phi - PI ) );
		return 0.5 + 0.25 * val;
	}
	const Scalar val = ( 2.0 / PI ) * atan( inv_ratio * tan( TWO_PI - phi ) );
	return 0.75 + 0.25 * val;
}

//! THE TRUE solid-angle density of the half-vector `GenerateSpecularRay`
//! draws (DL-177 defect 1, docs/DL177_WARD_DENSITY_AND_KRAY.md).
//!
//! `theta = atan(sqrt(-ln xi2 / D))` with
//! `D(phi) = cos^2 phi/ax^2 + sin^2 phi/ay^2`, so
//! `p_Theta(theta|phi) = 2 tan(theta) D exp(-tan^2 D) / cos^2(theta)`;
//! and differentiating `WardAnisoXiFromPhi` gives
//! `p_Phi(phi) = 1/(2 PI ax ay D(phi))`.  The `D` cancels between them:
//!
//!     p_h = exp(-tan^2(theta_h) D) / (PI ax ay cos^3(theta_h))
//!
//! Until 2026-09-18 this file stored
//! `cos(theta_h) exp(...) / (PI ax ay)` -- the same density times
//! `cos^4(theta_h)`.
static inline Scalar WardAnisoHalfDensity(
	const Scalar cosThetaH,
	const Scalar tan2D,			///< tan^2(theta_h) * D(phi)
	const Scalar ax,
	const Scalar ay
	)
{
	if( cosThetaH <= 0 || ax <= 0 || ay <= 0 ) {
		return 0;
	}
	const Scalar c2 = cosThetaH * cosThetaH;
	return exp( -tan2D ) / ( PI * ax * ay * c2 * cosThetaH );
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

//! See the isotropic twin: Malley's exact clipped-cosine fraction.
static inline Scalar WardDiffuseAcceptFraction(
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN
	)
{
	const Scalar c = Vector3Ops::Dot( myonb.w(), geomN );
	return r_max( Scalar(0), r_min( Scalar(1), 0.5 * (1.0 + c) ) );
}

//! The specular lobes `Scatter` emits in one call.
struct WardAnisoLobeSet
{
	int    count;
	Scalar ax[3];
	Scalar ay[3];
	//! `MaxValue` of the lobe's reflectance EXCLUDING the ratio -- see
	//! the isotropic twin's note on why that factorisation is exact.
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
		const Scalar alphax,
		const Scalar alphay,
		Scalar& outKrayRatio										///< [out] DL-177: `f_S cos / (p_S Rs)` for the sampled direction, 0 if no ray was produced
		)
{
	outKrayRatio = 0;
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = false;

	// Use the warping function to perturb the reflected ray
	const Scalar phi = WardAnisoPhiFromXi( random.x, alphax, alphay );


	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar denom = (cos_phi*cos_phi)/(alphax*alphax) + (sin_phi*sin_phi)/(alphay*alphay);
	const Scalar theta = atan( sqrt( -log(random.y) / denom ));

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z,
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z,
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.Dir());

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.Dir() + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );

		// DL-177 defect (1): the TRUE solid-angle density of this
		// sampler's half-vector, converted by the reflection Jacobian.
		// It used to be that density times `cos^4(theta_h)`.
		const Scalar tan_theta = sin_theta / cos_theta;
		const Scalar pdf_h = WardAnisoHalfDensity( cos_theta,
		                                           (tan_theta * tan_theta) * denom,
		                                           alphax, alphay );
		const Scalar hdotwo = Vector3Ops::Dot( h, ret );
		specular.pdf = ( hdotwo > 0 ) ? ( pdf_h / (4.0 * hdotwo) ) : Scalar(0);

		// DL-177 defect (3).
		const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
		outKrayRatio = WardKrayRatio( hdotwo, cos_theta,
		                              Vector3Ops::Dot( ret, onb.w() ),
		                              Vector3Ops::Dot( wi, onb.w() ) );
	}
}

void WardAnisotropicEllipticalGaussianSPF::Scatter(
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
		// DL-310: coupled diffuse (WardSelection::CoupledDiffuse) -- the
		// same constant min(Rd, 1 - Rs) value() uses.
		{
			const RISEPel rd = pDiffuse->GetColor(ri), rs = pSpecular->GetColor(ri);
			d.kray = RISEPel( WardSelection::CoupledDiffuse( rd[0], rs[0] ),
				WardSelection::CoupledDiffuse( rd[1], rs[1] ), WardSelection::CoupledDiffuse( rd[2], rs[2] ) );
		}
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}

	const ScalarTriple axt = pAlphaX->GetValuesAt(ri);
	const ScalarTriple ayt = pAlphaY->GetValuesAt(ri);

	if( !pAlphaX->HasPerChannelVariation() && !pAlphaY->HasPerChannelVariation() )
	{
		ScatteredRay s;
		Scalar ratio = 0;
		GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), axt.v[0], ayt.v[0], ratio );

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
			GenerateSpecularRay( s, myonb, ri, ptrand, axt.v[i], ayt.v[i], ratio );

			// Accept-check uses myonb.w() -- see the diffuse-lobe comment above.
			if( Vector3Ops::Dot( s.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( s.ray.Dir(), geomN ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i] * ratio;
				scattered.AddScatteredRay( s );
			}
		}
	}
}


void WardAnisotropicEllipticalGaussianSPF::ScatterNM(
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
	GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), pAlphaX->GetValueAtNM(ri,nm), pAlphaY->GetValueAtNM(ri,nm), sRatio );

	// Accept-checks use myonb.w() -- see Scatter()'s comment: it is the
	// frame lobes are actually sampled around (post-FlipW), not the raw
	// ri.onb.w() which differs by sign on a back-face hit.
	if( Vector3Ops::Dot( d.ray.Dir(), myonb.w() ) > 0.0 && Vector3Ops::Dot( d.ray.Dir(), geomN ) > 0.0 ) {
		d.krayNM = WardSelection::CoupledDiffuse( GuardedGetColorNM( *pDiffuse, ri, nm ), GuardedGetColorNM( *pSpecular, ri, nm ) );
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

//! Build the half-vector lane `j` would have drawn from the random pair
//! `(xi1, xi2)`, in the sampling frame, and report the realized
//! selection weight it contributes.  Returns false if `Scatter` would
//! have dropped that lane's ray.
static inline bool WardAnisoReplayLane(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Scalar nvView,
	const Scalar xi1,
	const Scalar xi2Log,		///< `-ln(xi2)`, shared by every lane
	const Scalar ax,
	const Scalar ay,
	Scalar& outRatio
	)
{
	outRatio = 0;
	if( ax <= 0 || ay <= 0 ) {
		return false;
	}

	const Scalar phi = WardAnisoPhiFromXi( xi1, ax, ay );
	const Scalar cp = cos( phi ), sp = sin( phi );
	const Scalar denom = (cp*cp)/(ax*ax) + (sp*sp)/(ay*ay);
	if( denom <= 0 ) {
		return false;
	}
	const Scalar t = sqrt( xi2Log / denom );
	const Scalar ct = 1.0 / sqrt( 1.0 + t*t );
	const Scalar st = t * ct;

	const Vector3& eu = myonb.u();
	const Vector3& ev = myonb.v();
	const Vector3& ew = myonb.w();
	const Scalar lx = cp * st, ly = sp * st;
	const Vector3 h( eu.x*lx + ev.x*ly + ew.x*ct,
	                 eu.y*lx + ev.y*ly + ew.y*ct,
	                 eu.z*lx + ev.z*ly + ew.z*ct );

	const Vector3& d = ri.ray.Dir();
	const Scalar hdotk = -Vector3Ops::Dot( h, d );
	if( hdotk <= 0 ) {
		return false;
	}
	const Vector3 ret = Vector3Ops::Normalize( d + 2.0 * hdotk * h );
	if( Vector3Ops::Dot( ret, ew ) <= 0 || Vector3Ops::Dot( ret, geomN ) <= 0 ) {
		return false;
	}

	outRatio = WardKrayRatio( Vector3Ops::Dot( h, ret ), ct,
	                          Vector3Ops::Dot( ret, ew ), nvView );
	return true;
}

//! C_D -- see the isotropic twin's block comment; identical construction,
//! with the azimuth now lane-dependent because Ward's warp carries the
//! `alphay/alphax` ratio.
static Scalar WardAnisoDiffuseSelectCoefficient(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Scalar wD,
	const WardAnisoLobeSet& lobes
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
		input.ax[j] = lobes.ax[j];
		input.ay[j] = lobes.ay[j];
		input.weight[j] = lobes.w[j];
	}
	return WardSelection::Evaluate( input );
}

//! `sum_i q_i(wo) p_i(wo)` -- see the isotropic twin.
static Scalar WardAnisoSpecularDensity(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& myonb,
	const Vector3& geomN,
	const Vector3& woNorm,
	const Scalar wD,
	const Scalar aD,
	const WardAnisoLobeSet& lobes
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
	const Scalar ratioAtWo = WardKrayRatio( hdotwo, cosThetaH, cosO, nv );

	const Scalar hu = Vector3Ops::Dot( h, myonb.u() );
	const Scalar hv = Vector3Ops::Dot( h, myonb.v() );
	Scalar phi = atan2( hv, hu );
	if( phi < 0 ) {
		phi += TWO_PI;
	}
	const Scalar tan2 = ( 1.0 - cosThetaH*cosThetaH ) / ( cosThetaH*cosThetaH );

	Scalar sum = 0;

	for( int i = 0; i < lobes.count; i++ ) {
		const Scalar axi = lobes.ax[i], ayi = lobes.ay[i];
		if( axi <= 0 || ayi <= 0 ) {
			continue;
		}
		const Scalar cp = cos( phi ), sp = sin( phi );
		const Scalar denom_i = (cp*cp)/(axi*axi) + (sp*sp)/(ayi*ayi);
		const Scalar pdf_i = WardAnisoHalfDensity( cosThetaH, tan2 * denom_i, axi, ayi )
		                   / ( 4.0 * hdotwo );
		if( pdf_i <= 0 ) {
			continue;
		}

		const Scalar w_i = lobes.w[i] * ratioAtWo;

		Scalar wOther = 0;
		int nAcceptedSpec = 1;
		if( lobes.count > 1 ) {
			// Invert lane i to the random pair it must have drawn, then
			// replay every sibling through the SAME pair.
			const Scalar xi1 = WardAnisoXiFromPhi( phi, axi, ayi );
			const Scalar xi2Log = tan2 * denom_i;		// = -ln(xi2)
			for( int j = 0; j < lobes.count; j++ ) {
				if( j == i ) {
					continue;
				}
				Scalar ratio = 0;
				if( WardAnisoReplayLane( ri, myonb, geomN, nv, xi1, xi2Log,
				                         lobes.ax[j], lobes.ay[j], ratio ) ) {
					wOther += lobes.w[j] * ratio;
					nAcceptedSpec++;
				}
			}
		}

		const Scalar wS = w_i + wOther;

		Scalar q = 0;
		const Scalar totalWith = wD + wS;
		if( totalWith > NEARZERO ) {
			q += aD * ( w_i / totalWith );
		}
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
//! produce.  DL-177 defect (2).
static Scalar WardAnisotropicPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const WardAnisoLobeSet& lobes,
	const Scalar wDiff
	)
{
	// Mirror Scatter()'s FlipW: orient the frame to face the incoming ray so
	// this Pdf agrees with Scatter's actual sampling frame on backface hits
	// (Scatter samples both lobes relative to the flipped myonb, so a raw
	// ri.onb.w()/onb.u()/onb.v() here returned 0 for directions Scatter
	// legitimately emits).
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3& n = myonb.w();

	const Scalar cos_theta_i = Vector3Ops::Dot( wi, n );
	const Scalar cos_theta_o = Vector3Ops::Dot( wo, n );

	if( cos_theta_i <= 0.0 || cos_theta_o <= 0.0 ) {
		return 0.0;
	}

	// Geometric-horizon gate (MIS consistency with the sampler-side gates):
	// a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( wo, geomN ) <= 0 ) {
		return 0.0;
	}

	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	const Scalar aD = WardDiffuseAcceptFraction( myonb, geomN );
	const Scalar cD = WardAnisoDiffuseSelectCoefficient( ri, myonb, geomN, wDiff, lobes );

	const Scalar pdf_diffuse = Vector3Ops::Dot( woNorm, n ) * INV_PI;

	return cD * pdf_diffuse
	     + WardAnisoSpecularDensity( ri, myonb, geomN, woNorm, wDiff, aD, lobes );
}

Scalar WardAnisotropicEllipticalGaussianSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	const ScalarTriple axt = pAlphaX->GetValuesAt(ri);
	const ScalarTriple ayt = pAlphaY->GetValuesAt(ri);
	const RISEPel spec = pSpecular->GetColor(ri);

	// Mirror Scatter's own branch EXACTLY.
	WardAnisoLobeSet lobes;
	if( !pAlphaX->HasPerChannelVariation() && !pAlphaY->HasPerChannelVariation() ) {
		lobes.count = 1;
		lobes.ax[0] = axt.v[0];
		lobes.ay[0] = ayt.v[0];
		lobes.w[0]  = ColorMath::MaxValue( spec );
	} else {
		lobes.count = 3;
		for( int i = 0; i < 3; i++ ) {
			lobes.ax[i] = axt.v[i];
			lobes.ay[i] = ayt.v[i];
			lobes.w[i]  = spec[i];
		}
	}

	// DL-310: the diffuse ray's realized weight is its coupled kray.
	const RISEPel rdP = pDiffuse->GetColor(ri), rsP = pSpecular->GetColor(ri);
	const Scalar wDiff = ColorMath::MaxValue( RISEPel( WardSelection::CoupledDiffuse( rdP[0], rsP[0] ),
		WardSelection::CoupledDiffuse( rdP[1], rsP[1] ), WardSelection::CoupledDiffuse( rdP[2], rsP[2] ) ) );

	return WardAnisotropicPdf( ri, wo, lobes, wDiff );
}

Scalar WardAnisotropicEllipticalGaussianSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// ScatterNM emits exactly one specular lobe, and RandomlySelect
	// weights the NM lanes by `krayNM`.
	WardAnisoLobeSet lobes;
	lobes.count = 1;
	lobes.ax[0] = pAlphaX->GetValueAtNM(ri,nm);
	lobes.ay[0] = pAlphaY->GetValueAtNM(ri,nm);
	lobes.w[0]  = fabs( GuardedGetColorNM( *pSpecular, ri, nm ) );

	const Scalar wDiff = fabs( WardSelection::CoupledDiffuse( GuardedGetColorNM( *pDiffuse, ri, nm ), GuardedGetColorNM( *pSpecular, ri, nm ) ) );

	return WardAnisotropicPdf( ri, wo, lobes, wDiff );
}

//////////////////////////////////////////////////////////////////////
// EvaluateKrayNM -- DL-125.  Isotropic twin's derivation, verbatim:
// DL-212 changes the BRDF normalization; ax ay and the Gaussian
// still cancel against p_S, leaving 2 cos_o/(cos_i+cos_o).
// Thus `WardKrayRatio` is the SAME expression
// here and NEITHER `alphaX` nor `alphaY` appears in the transport
// weight.  Only the reflectance painters are read at `nm`.
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// EvaluateLobeFNM -- DL-216.
//
// Evaluates the SELECTED lobe's own spectral BSDF value f_I(wo; nm)
// in [1/sr], without multiplying by cosine and without dividing by
// any sampling density.
//////////////////////////////////////////////////////////////////////
Scalar WardAnisotropicEllipticalGaussianSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return WardSelection::CoupledDiffuse( GuardedGetColorNM( *pDiffuse, ri, nm ), GuardedGetColorNM( *pSpecular, ri, nm ) ) * INV_PI;
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

	const Scalar ax = pAlphaX->GetValueAtNM( ri, nm );
	const Scalar ay = pAlphaY->GetValueAtNM( ri, nm );
	if( ax <= 0 || ay <= 0 ) {
		return 0;
	}

	const Scalar hu = Vector3Ops::Dot( h, myonb.u() );
	const Scalar hv = Vector3Ops::Dot( h, myonb.v() );
	Scalar phi = atan2( hv, hu );
	if( phi < 0 ) {
		phi += TWO_PI;
	}
	const Scalar tan2 = ( 1.0 - cos_h * cos_h ) / ( cos_h * cos_h );
	const Scalar cp = cos( phi ), sp = sin( phi );
	const Scalar denom = (cp*cp)/(ax*ax) + (sp*sp)/(ay*ay);
	const Scalar pdf_h = WardAnisoHalfDensity( cos_h, tan2 * denom, ax, ay );
	const Scalar pdf = pdf_h / ( 4.0 * hdotwo );

	const Scalar kray = GuardedGetColorNM( *pSpecular, ri, nm ) * ratio;
	return ( kray * pdf ) / cos_o;
}

Scalar WardAnisotropicEllipticalGaussianSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /* ior_stack */
	) const
{
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return WardSelection::CoupledDiffuse( GuardedGetColorNM( *pDiffuse, ri, nm ), GuardedGetColorNM( *pSpecular, ri, nm ) );
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

Scalar WardAnisotropicEllipticalGaussianSPF::EvaluateKrayNM(
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
		return WardSelection::CoupledDiffuse( GuardedGetColorNM( *pDiffuse, ri, nm ), GuardedGetColorNM( *pSpecular, ri, nm ) );
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
