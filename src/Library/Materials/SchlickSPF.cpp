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

using namespace RISE;
using namespace RISE::Implementation;

SchlickSPF::SchlickSPF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IPainter& roughness,
	const IPainter& isotropy
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pRoughness( roughness ),
  pIsotropy( isotropy )
{
	pDiffuse.addref();
	pSpecular.addref();
	pRoughness.addref();
	pIsotropy.addref();
}

SchlickSPF::~SchlickSPF( )
{
	pDiffuse.release();
	pSpecular.release();
	pRoughness.release();
	pIsotropy.release();
}

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

static void GenerateSpecularRay(
		ScatteredRay& specular,
		Scalar& fresnel,
		const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& random,										///< [in] Two random numbers
		const Scalar r,
		const Scalar p
		)
{
	specular.type = ScatteredRay::eRayReflection;

	// Use the warping function to perturb the reflected ray
	const Scalar xi = random.x;
	const Scalar b = random.y;

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

	const Scalar theta = acos(sqrt(xi/(r-xi*r+xi)));

	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  ri.onb.u().x*a.x + ri.onb.v().x*a.y + ri.onb.w().x*a.z,
	   	  ri.onb.u().y*a.x + ri.onb.v().y*a.y + ri.onb.w().y*a.z,
		  ri.onb.u().z*a.x + ri.onb.v().z*a.y + ri.onb.w().z*a.z );

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
	const Vector3& wo,
	const Scalar r,
	const Scalar p
	)
{
	if( r < NEARZERO ) {
		return 0;
	}

	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Compute half-vector
	Vector3 h = Vector3Ops::Normalize( wi + woNorm );
	const Scalar hdotn = Vector3Ops::Dot( h, ri.onb.w() );
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

	// For the azimuthal part with anisotropy parameter p:
	// The phi PDF (from the sampling inversion) is:
	// p(phi) = p / (2*pi*(cos^2(phi) + p^2*sin^2(phi)))
	// This integrates to 1 over [0, 2pi].

	// Project h onto tangent plane to get phi
	const Scalar hu = Vector3Ops::Dot( h, ri.onb.u() );
	const Scalar hv = Vector3Ops::Dot( h, ri.onb.v() );

	Scalar phi_pdf;
	if( sin2_theta_h < NEARZERO ) {
		// At the pole, phi is degenerate; use uniform 1/(2*pi)
		phi_pdf = INV_PI * 0.5;
	} else {
		// cos(phi_h) and sin(phi_h) in the tangent plane
		const Scalar inv_sin = 1.0 / sqrt(sin2_theta_h);
		const Scalar cos_phi = hu * inv_sin;
		const Scalar sin_phi = hv * inv_sin;
		const Scalar cos2_phi = cos_phi * cos_phi;
		const Scalar sin2_phi = sin_phi * sin_phi;
		const Scalar phi_denom = cos2_phi + p * p * sin2_phi;
		if( phi_denom < NEARZERO ) {
			return 0;
		}
		phi_pdf = p / (TWO_PI * phi_denom);
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

	ScatteredRay d, s;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );

	if( Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		d.kray = pDiffuse.GetColor(ri);
		// Cosine-weighted hemisphere PDF
		const Scalar cosTheta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		d.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		d.isDelta = false;
		scattered.AddScatteredRay( d );
	}

	RISEPel roughness = pRoughness.GetColor(ri);
	const RISEPel isotropy = pIsotropy.GetColor(ri);

	// Glossy filtering: increase effective roughness to blur
	// secondary glossy reflections, reducing caustic noise.
	if( ri.glossyFilterWidth > 0 ) {
		for( int ch = 0; ch < 3; ch++ ) {
			roughness[ch] = r_min( roughness[ch] + ri.glossyFilterWidth, Scalar(1.0) );
		}
	}

	if( roughness[0] == roughness[1] && roughness[1] == roughness[2] &&
		isotropy[0] == isotropy[1] && isotropy[1] == isotropy[2] )
	{
		Scalar fresnel = 0;
		GenerateSpecularRay( s, fresnel, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), roughness[0], isotropy[0] );

		if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			const RISEPel rho = pSpecular.GetColor(ri);
			s.kray = rho + (RISEPel(1.0,1.0,1.0)-rho) * fresnel;
			s.pdf = ComputeSchlickSpecularPdf( ri, s.ray.Dir(), roughness[0], isotropy[0] );
			s.isDelta = false;
			scattered.AddScatteredRay( s );
		}
	}
	else
	{
		const Point2 ptrand( sampler.Get1D(),sampler.Get1D() );
		const RISEPel rho = pSpecular.GetColor(ri);

		for( int i=0; i<3; i++ ) {
			Scalar fresnel = 0;
			GenerateSpecularRay( s, fresnel, myonb, ri, ptrand, roughness[i], isotropy[i] );

			if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = rho[i] + (1.0-rho[i]) * fresnel;
				s.pdf = ComputeSchlickSpecularPdf( ri, s.ray.Dir(), roughness[i], isotropy[i] );
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

	ScatteredRay d, s;
	Scalar fresnel = 0;
	Scalar roughnessNM = pRoughness.GetColorNM(ri,nm);
	const Scalar isotropyNM = pIsotropy.GetColorNM(ri,nm);

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		roughnessNM = r_min( roughnessNM + ri.glossyFilterWidth, Scalar(1.0) );
	}

	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );
	GenerateSpecularRay( s, fresnel, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), roughnessNM, isotropyNM );

	if( Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		d.krayNM = pDiffuse.GetColorNM(ri,nm);
		const Scalar cosTheta = Vector3Ops::Dot( d.ray.Dir(), myonb.w() );
		d.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		d.isDelta = false;
		scattered.AddScatteredRay( d );
	}

	if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		const Scalar rho = pSpecular.GetColorNM(ri,nm);
		s.krayNM = rho + (1.0-rho) * fresnel;
		s.pdf = ComputeSchlickSpecularPdf( ri, s.ray.Dir(), roughnessNM, isotropyNM );
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

	// Diffuse PDF: cosine-weighted hemisphere
	const Scalar diffusePdf = cosTheta * INV_PI;

	// Specular PDF: Schlick half-vector sampling (use average roughness/isotropy)
	RISEPel roughness = pRoughness.GetColor(ri);
	const RISEPel isotropy = pIsotropy.GetColor(ri);
	if( ri.glossyFilterWidth > 0 ) {
		for( int ch = 0; ch < 3; ch++ ) {
			roughness[ch] = r_min( roughness[ch] + ri.glossyFilterWidth, Scalar(1.0) );
		}
	}
	const Scalar rAvg = (roughness[0] + roughness[1] + roughness[2]) / 3.0;
	const Scalar pAvg = (isotropy[0] + isotropy[1] + isotropy[2]) / 3.0;
	const Scalar specPdf = ComputeSchlickSpecularPdf( ri, wo, rAvg, pAvg );

	// Weight by relative importance of diffuse vs specular
	const RISEPel rd = pDiffuse.GetColor(ri);
	const RISEPel rs = pSpecular.GetColor(ri);
	const Scalar dWeight = ColorMath::MaxValue(rd);
	const Scalar sWeight = ColorMath::MaxValue(rs);
	const Scalar totalWeight = dWeight + sWeight;

	if( totalWeight < NEARZERO ) {
		return 0;
	}

	return (dWeight * diffusePdf + sWeight * specPdf) / totalWeight;
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

	// Diffuse PDF
	const Scalar diffusePdf = cosTheta * INV_PI;

	// Specular PDF
	Scalar r = pRoughness.GetColorNM(ri,nm);
	const Scalar p = pIsotropy.GetColorNM(ri,nm);
	if( ri.glossyFilterWidth > 0 ) {
		r = r_min( r + ri.glossyFilterWidth, Scalar(1.0) );
	}
	const Scalar specPdf = ComputeSchlickSpecularPdf( ri, wo, r, p );

	// Weight
	const Scalar rd = pDiffuse.GetColorNM(ri,nm);
	const Scalar rs = pSpecular.GetColorNM(ri,nm);
	const Scalar totalWeight = rd + rs;

	if( totalWeight < NEARZERO ) {
		return 0;
	}

	return (rd * diffusePdf + rs * specPdf) / totalWeight;
}
