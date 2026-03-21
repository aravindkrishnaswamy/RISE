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

AshikminShirleyAnisotropicPhongSPF::AshikminShirleyAnisotropicPhongSPF(
	const IPainter& Nu_,
	const IPainter& Nv_,
	const IPainter& Rd_,
	const IPainter& Rs_
	) :
  Nu( Nu_ ),
  Nv( Nv_ ),
  Rd( Rd_ ),
  Rs( Rs_ )
{
	Nu.addref();
	Nv.addref();
	Rd.addref();
	Rs.addref();
}

AshikminShirleyAnisotropicPhongSPF::~AshikminShirleyAnisotropicPhongSPF( )
{
	Nu.release();
	Nv.release();
	Rd.release();
	Rs.release();
}

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

		// If the ray goes into the material, then lets not use it
		if( Vector3Ops::Dot(k2, ri.onb.w()) < 0 ) {
			return false;
		}

		// Compute the density of the perturbed ray
		const Scalar hdotn = Vector3Ops::Dot( ri.onb.w(), h );
		const Scalar factor1 = sqrt((NU+1.0)*(NV+1.0)) / TWO_PI;
		const Scalar factor2 = pow( hdotn, (NU*cos_phi*cos_phi + NV*sin_phi*sin_phi));

		const Scalar inv_actual_density = (4.0 * hdotk) / (factor1*factor2);

		// Compute the density of what we actually want (from the BRDF)
		Scalar brdf;
		AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors( diffuseFactor, brdf, k2, ri, NU, NV, Rs );

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
		const RandomNumberGenerator& random,				///< [in] Random number generator
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const RISEPel NU = Nu.GetColor(ri);
	const RISEPel NV = Nv.GetColor(ri);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	RISEPel df=0.0;

	const RISEPel rho = Rs.GetColor(ri);

	if( NU[0] == NU[1] && NU[1] == NU[2] &&
		NV[0] == NV[1] && NV[1] == NV[2]
		)
	{
		Scalar diffuseFactor=0, specFactor=0;
		if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), NU[0], NV[0], ColorMath::MaxValue(rho) ) ) {
			if( specFactor > 1.0 ) {
				specFactor = 1.0;
			}

			specular.kray = rho * specFactor;
			scattered.AddScatteredRay( specular );
		}
		df = diffuseFactor;
	}
	else
	{
		const Point2 ptrand(random.CanonicalRandom(),random.CanonicalRandom());
		for( int i=0; i<3; i++ ) {
			Scalar specFactor=0;
			if( GenerateSpecularRay( specular, df[i], specFactor, myonb, ri, ptrand, NU[i], NV[i], rho[i] ) ) {
				if( specFactor > 1.0 ) {
					specFactor = 1.0;
				}
				specular.kray = 0.0;
				specular.kray[i] = rho[i] * specFactor;
				scattered.AddScatteredRay( specular );
			}
		}
	}

	// The rest is diffuse
	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;
	diffuse.isDelta = false;
	diffuse.kray = Rd.GetColor(ri)*df;
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) ) );
	// Cosine-weighted hemisphere: pdf = cos(theta) / PI
	const Scalar cos_theta_d = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
	diffuse.pdf = r_max( 0.0, cos_theta_d ) * INV_PI;

	scattered.AddScatteredRay( diffuse );
}

void AshikminShirleyAnisotropicPhongSPF::ScatterNM(
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

	const Scalar NU = Nu.GetColorNM(ri,nm);
	const Scalar NV = Nv.GetColorNM(ri,nm);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	Scalar specFactor=0;
	Scalar diffuseFactor=0;

	const Scalar rho = Rs.GetColorNM(ri,nm);

	if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), NU, NV, rho ) ) {
		specular.krayNM = rho * specFactor;
		scattered.AddScatteredRay( specular );
	}

	// The rest is diffuse
	if( specFactor < 1.0 ) {
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.isDelta = false;
		diffuse.krayNM = Rd.GetColorNM(ri,nm) * diffuseFactor;
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) ) );
		const Scalar cos_theta_d = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
		diffuse.pdf = r_max( 0.0, cos_theta_d ) * INV_PI;

		scattered.AddScatteredRay( diffuse );
	}
}

// Computes the Ashikhmin-Shirley anisotropic Phong specular PDF for a given direction
static Scalar AshikminShirleySpecularPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nu,
	const Scalar nv
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

	// Specular PDF: Ashikhmin-Shirley anisotropic Phong half-vector distribution
	Vector3 h = Vector3Ops::Normalize( wi + wo );
	const Scalar hdotn = Vector3Ops::Dot( h, n );

	if( hdotn <= 0.0 ) {
		return pdf_diffuse;
	}

	const Scalar hdotk = Vector3Ops::Dot( h, wo );
	if( hdotk <= 0.0 ) {
		return pdf_diffuse;
	}

	const Scalar hu = Vector3Ops::Dot( h, ri.onb.u() );
	const Scalar hv = Vector3Ops::Dot( h, ri.onb.v() );

	// Exponent: (nu * (h.u)^2 + nv * (h.v)^2) / (1 - (h.n)^2)
	const Scalar sin_theta_h_sq = 1.0 - hdotn * hdotn;
	Scalar exponent_val = 0.0;
	if( sin_theta_h_sq > NEARZERO ) {
		exponent_val = (nu * hu * hu + nv * hv * hv) / sin_theta_h_sq;
	} else {
		// h is aligned with n, the exponent term vanishes (pow(1, ...) = 1)
		exponent_val = 0.0;
	}

	const Scalar factor1 = sqrt((nu + 1.0) * (nv + 1.0)) / TWO_PI;
	const Scalar factor2 = pow( hdotn, exponent_val );

	const Scalar pdf_specular = (factor1 * factor2) / (4.0 * hdotk);

	// Average of diffuse and specular PDFs
	return 0.5 * (pdf_diffuse + pdf_specular);
}

Scalar AshikminShirleyAnisotropicPhongSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const RISEPel nu = Nu.GetColor(ri);
	const RISEPel nv = Nv.GetColor(ri);
	// Use average values across channels
	const Scalar nu_val = (nu[0] + nu[1] + nu[2]) / 3.0;
	const Scalar nv_val = (nv[0] + nv[1] + nv[2]) / 3.0;
	return AshikminShirleySpecularPdf( ri, wo, nu_val, nv_val );
}

Scalar AshikminShirleyAnisotropicPhongSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	const Scalar nu_val = Nu.GetColorNM(ri,nm);
	const Scalar nv_val = Nv.GetColorNM(ri,nm);
	return AshikminShirleySpecularPdf( ri, wo, nu_val, nv_val );
}
