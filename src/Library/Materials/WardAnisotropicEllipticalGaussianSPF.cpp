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
#include "WardAnisotropicEllipticalGaussianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

WardAnisotropicEllipticalGaussianSPF::WardAnisotropicEllipticalGaussianSPF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IPainter& alphax_,
	const IPainter& alphay_
	) :
  diffuse( diffuse_ ),
  specular( specular_ ),
  alphax( alphax_ ),
  alphay( alphay_ )
{
	diffuse.addref();
	specular.addref();
	alphax.addref();
	alphay.addref();
}

WardAnisotropicEllipticalGaussianSPF::~WardAnisotropicEllipticalGaussianSPF( )
{
	diffuse.release();
	specular.release();
	alphax.release();
	alphay.release();
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
	diffuse.isDelta = false;
}

static void GenerateSpecularRay(
		ScatteredRay& specular,
		const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& random,										///< [in] Two random numbers
		const Scalar alphax,
		const Scalar alphay
		)
{
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = false;

	// Use the warping function to perturb the reflected ray
	const Scalar xi = random.x;
	const Scalar alpha_ratio = (alphay/alphax);
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

		// Compute the PDF for this sampled direction
		// Half-vector PDF for Ward anisotropic:
		// p(h) = cos(theta_h) / (PI * alphax * alphay) * exp(-(h.u/alphax)^2 + (h.v/alphay)^2) / (1 - (h.n)^2))
		// But equivalently using the sampled angles:
		// p(h) = cos(theta_h) / (PI * alphax * alphay) * exp(-tan^2(theta_h) * ((cos_phi/alphax)^2 + (sin_phi/alphay)^2))
		const Scalar tan_theta = sin_theta / cos_theta;
		const Scalar exponent = -(tan_theta * tan_theta) * denom;
		const Scalar pdf_h = cos_theta / (PI * alphax * alphay) * exp( exponent );
		specular.pdf = pdf_h / (4.0 * hdotk);
	}
}

void WardAnisotropicEllipticalGaussianSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	ScatteredRay d, s;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );

	if( Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		d.kray = diffuse.GetColor(ri);
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}

	const RISEPel ax = alphax.GetColor(ri);
	const RISEPel ay = alphay.GetColor(ri);

	if( ax[0] == ax[1] && ax[1] == ax[2] &&
		ay[0] == ay[1] && ay[1] == ay[2] )
	{
		GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), ax[0], ay[0] );

		if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			s.kray = specular.GetColor(ri);
			scattered.AddScatteredRay( s );
		}
	}
	else
	{
		const Point2 ptrand( sampler.Get1D(),sampler.Get1D() );
		const RISEPel spec = specular.GetColor(ri);
		for( int i=0; i<3; i++ ) {
			GenerateSpecularRay( s, myonb, ri, ptrand, alphax.GetColor(ri)[i], alphay.GetColor(ri)[i] );

			if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i];
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
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	ScatteredRay d, s;
	GenerateDiffuseRay( d, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()) );
	GenerateSpecularRay( s, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), alphax.GetColorNM(ri,nm), alphay.GetColorNM(ri,nm) );

	if( Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		d.krayNM = diffuse.GetColorNM(ri,nm);
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}
	if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		s.krayNM = specular.GetColorNM(ri,nm);
		scattered.AddScatteredRay( s );
	}
}

// Computes the Ward anisotropic elliptical Gaussian half-vector PDF converted to solid angle
static Scalar WardAnisotropicPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar ax,
	const Scalar ay,
	const Scalar wDiff,
	const Scalar wSpec
	)
{
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3& n = ri.onb.w();

	const Scalar cos_theta_i = Vector3Ops::Dot( wi, n );
	const Scalar cos_theta_o = Vector3Ops::Dot( wo, n );

	if( cos_theta_i <= 0.0 || cos_theta_o <= 0.0 ) {
		return 0.0;
	}

	// Diffuse PDF: cosine-weighted hemisphere
	const Scalar pdf_diffuse = cos_theta_o * INV_PI;

	// Specular PDF: Ward anisotropic Gaussian via half-vector
	Vector3 h = Vector3Ops::Normalize( wi + wo );
	const Scalar cos_theta_h = Vector3Ops::Dot( h, n );

	if( cos_theta_h <= 0.0 ) {
		return pdf_diffuse;
	}

	// Project h onto the tangent plane to get the anisotropic components
	const Scalar h_dot_u = Vector3Ops::Dot( h, ri.onb.u() );
	const Scalar h_dot_v = Vector3Ops::Dot( h, ri.onb.v() );

	const Scalar exponent = -((h_dot_u * h_dot_u) / (ax * ax) + (h_dot_v * h_dot_v) / (ay * ay)) / (cos_theta_h * cos_theta_h);

	const Scalar pdf_h = cos_theta_h / (PI * ax * ay) * exp( exponent );

	const Scalar hdotwo = Vector3Ops::Dot( h, wo );
	if( hdotwo <= 0.0 ) {
		return pdf_diffuse;
	}

	const Scalar pdf_specular = pdf_h / (4.0 * hdotwo);

	// Weighted mixture of diffuse and specular PDFs
	const Scalar totalWeight = wDiff + wSpec;
	if( totalWeight < 1e-20 ) {
		return pdf_diffuse;
	}
	return (wDiff * pdf_diffuse + wSpec * pdf_specular) / totalWeight;
}

Scalar WardAnisotropicEllipticalGaussianSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const RISEPel ax = alphax.GetColor(ri);
	const RISEPel ay = alphay.GetColor(ri);
	// Use average values across channels
	const Scalar ax_val = (ax[0] + ax[1] + ax[2]) / 3.0;
	const Scalar ay_val = (ay[0] + ay[1] + ay[2]) / 3.0;

	// Weight by MaxValue(kray) to match RandomlySelect
	const Scalar wDiff = ColorMath::MaxValue( diffuse.GetColor(ri) );
	const Scalar wSpec = ColorMath::MaxValue( specular.GetColor(ri) );

	return WardAnisotropicPdf( ri, wo, ax_val, ay_val, wDiff, wSpec );
}

Scalar WardAnisotropicEllipticalGaussianSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	const Scalar ax_val = alphax.GetColorNM(ri,nm);
	const Scalar ay_val = alphay.GetColorNM(ri,nm);

	// Weight by krayNM magnitude to match RandomlySelect
	const Scalar wDiff = fabs( diffuse.GetColorNM(ri,nm) );
	const Scalar wSpec = fabs( specular.GetColorNM(ri,nm) );

	return WardAnisotropicPdf( ri, wo, ax_val, ay_val, wDiff, wSpec );
}
