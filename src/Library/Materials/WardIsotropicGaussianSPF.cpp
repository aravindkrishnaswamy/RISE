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
	const IPainter& alpha_
	) :
  diffuse( diffuse_ ),
  specular( specular_ ),
  alpha( alpha_ )
{
	diffuse.addref();
	specular.addref();
	alpha.addref();
}

WardIsotropicGaussianSPF::~WardIsotropicGaussianSPF( )
{
	diffuse.release();
	specular.release();
	alpha.release();
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
	const Scalar alpha
	)
{
	specular.type = ScatteredRay::eRayReflection;
	specular.isDelta = false;

	const Scalar phi = TWO_PI * random.x;
	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar theta = atan(alpha*(sqrt(-log(random.y))));

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  ri.onb.u().x*a.x + ri.onb.v().x*a.y + ri.onb.w().x*a.z,
	   	  ri.onb.u().y*a.x + ri.onb.v().y*a.y + ri.onb.w().y*a.z,
		  ri.onb.u().z*a.x + ri.onb.v().z*a.y + ri.onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.Dir());

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.Dir() + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );

		// Compute the PDF for this sampled direction
		// Half-vector PDF: p(h) = cos(theta_h) / (PI * alpha^2) * exp(-tan^2(theta_h) / alpha^2)
		// Convert to solid angle: p(wo) = p(h) / (4 * dot(h, wo))
		const Scalar tan_theta = sin_theta / cos_theta;
		const Scalar alpha_sq = alpha * alpha;
		const Scalar pdf_h = (cos_theta / (PI * alpha_sq)) * exp(-(tan_theta * tan_theta) / alpha_sq);
		specular.pdf = pdf_h / (4.0 * hdotk);
	}
}

void WardIsotropicGaussianSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	ScatteredRay d, s;
	GenerateDiffuseRay( d, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()) );

	if( Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		d.kray = diffuse.GetColor(ri);
		// Cosine-weighted hemisphere: pdf = cos(theta) / PI
		const Scalar cos_theta = Vector3Ops::Dot( d.ray.Dir(), ri.onb.w() );
		d.pdf = cos_theta * INV_PI;
		scattered.AddScatteredRay( d );
	}

	const RISEPel a = alpha.GetColor(ri);

	if( a[0] == a[1] && a[1] == a[2] )
	{
		GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), a[0] );

		if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			s.kray = specular.GetColor(ri);
			scattered.AddScatteredRay( s );
		}
	}
	else
	{
		const Point2 ptrand( random.CanonicalRandom(),random.CanonicalRandom() );
		const RISEPel spec = specular.GetColor(ri);
		for( int i=0; i<3; i++ ) {
			GenerateSpecularRay( s, myonb, ri, ptrand, a[i] );

			if( Vector3Ops::Dot( s.ray.Dir(), ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i];
				scattered.AddScatteredRay( s );
			}
		}
	}
}


void WardIsotropicGaussianSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
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
	GenerateDiffuseRay( d, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()) );
	GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), alpha.GetColorNM(ri,nm) );

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

// Computes the Ward isotropic Gaussian half-vector PDF converted to solid angle
static Scalar WardIsotropicPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar alpha_val,
	const Scalar wDiff,
	const Scalar wSpec
	)
{
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 n = ri.onb.w();

	const Scalar cos_theta_i = Vector3Ops::Dot( wi, n );
	const Scalar cos_theta_o = Vector3Ops::Dot( wo, n );

	if( cos_theta_i <= 0.0 || cos_theta_o <= 0.0 ) {
		return 0.0;
	}

	// Diffuse PDF: cosine-weighted hemisphere
	const Scalar pdf_diffuse = cos_theta_o * INV_PI;

	// Specular PDF: Ward isotropic Gaussian via half-vector
	Vector3 h = Vector3Ops::Normalize( wi + wo );
	const Scalar cos_theta_h = Vector3Ops::Dot( h, n );

	if( cos_theta_h <= 0.0 ) {
		return pdf_diffuse;
	}

	const Scalar sin_theta_h = sqrt( r_max( 0.0, 1.0 - cos_theta_h * cos_theta_h ) );
	const Scalar tan_theta_h = sin_theta_h / cos_theta_h;
	const Scalar alpha_sq = alpha_val * alpha_val;

	const Scalar pdf_h = (cos_theta_h / (PI * alpha_sq)) * exp( -(tan_theta_h * tan_theta_h) / alpha_sq );

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

Scalar WardIsotropicGaussianSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const RISEPel a = alpha.GetColor(ri);
	// Use the average alpha across channels
	const Scalar alpha_val = (a[0] + a[1] + a[2]) / 3.0;

	// Weight by MaxValue(kray) to match RandomlySelect
	const Scalar wDiff = ColorMath::MaxValue( diffuse.GetColor(ri) );
	const Scalar wSpec = ColorMath::MaxValue( specular.GetColor(ri) );

	return WardIsotropicPdf( ri, wo, alpha_val, wDiff, wSpec );
}

Scalar WardIsotropicGaussianSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	const Scalar alpha_val = alpha.GetColorNM(ri,nm);

	// Weight by krayNM magnitude to match RandomlySelect
	const Scalar wDiff = fabs( diffuse.GetColorNM(ri,nm) );
	const Scalar wSpec = fabs( specular.GetColorNM(ri,nm) );

	return WardIsotropicPdf( ri, wo, alpha_val, wDiff, wSpec );
}
