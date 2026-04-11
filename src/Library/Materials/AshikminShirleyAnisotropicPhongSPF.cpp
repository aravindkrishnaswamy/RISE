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
		ISampler& sampler,				///< [in] Sampler
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack& ior_stack								///< [in/out] Index of refraction stack
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

	const RISEPel rho = Rs.GetColor(ri);

	if( NU[0] == NU[1] && NU[1] == NU[2] &&
		NV[0] == NV[1] && NV[1] == NV[2]
		)
	{
		Scalar diffuseFactor_unused=0, specFactor=0;
		if( GenerateSpecularRay( specular, diffuseFactor_unused, specFactor, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), NU[0], NV[0], ColorMath::MaxValue(rho) ) ) {
			// specFactor = brdf_spec/pdf.  For correct IS: kray = BRDF*cos/pdf.
			// specularFactor already includes Fresnel (which contains Rs),
			// so no extra Rs multiplication.  Add cos_o for the missing cosine.
			const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), ri.onb.w() );
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
				const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), ri.onb.w() );
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

	const Scalar cos_o_diff = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
	diffuse.pdf = r_max( 0.0, cos_o_diff ) * INV_PI;

	// Compute diffuse IS weight: kray = BRDF_diff * cos / pdf
	// BRDF_diff = Rd * (1-Rs) * (28/(23π)) * fromK1(wo) * fromK2(wi)
	// pdf = cos/π, so kray = Rd * (1-Rs) * (28/23) * fromK1 * fromK2
	// (the π from the BRDF normalisation cancels with the π in the pdf)
	const Scalar cos_i = Vector3Ops::Dot( Vector3Ops::Normalize(-ri.ray.Dir()), ri.onb.w() );
	const Scalar fromK1 = 1.0 - pow( 1.0 - r_max(0.0, cos_o_diff) * 0.5, 5.0 );
	const Scalar fromK2 = 1.0 - pow( 1.0 - r_max(0.0, cos_i) * 0.5, 5.0 );
	static const Scalar diffuseNorm = 28.0 / 23.0;

	const RISEPel oneMinusRs = RISEPel(1,1,1) - rho;
	diffuse.kray = Rd.GetColor(ri) * oneMinusRs * (diffuseNorm * fromK1 * fromK2);

	scattered.AddScatteredRay( diffuse );
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

	const Scalar NU = Nu.GetColorNM(ri,nm);
	const Scalar NV = Nv.GetColorNM(ri,nm);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	Scalar specFactor=0;
	Scalar diffuseFactor=0;

	const Scalar rho = Rs.GetColorNM(ri,nm);

	if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(sampler.Get1D(),sampler.Get1D()), NU, NV, rho ) ) {
		// specFactor already includes Fresnel (which contains Rs) — no extra rho.
		// Add cos_o for correct IS weight.
		const Scalar cos_o = Vector3Ops::Dot( specular.ray.Dir(), ri.onb.w() );
		specular.krayNM = specFactor * cos_o;
		scattered.AddScatteredRay( specular );
	}

	// Generate diffuse ray and compute factor at actual diffuse direction
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.isDelta = false;
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(sampler.Get1D(),sampler.Get1D()) ) );
		const Scalar cos_o_diff = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
		diffuse.pdf = r_max( 0.0, cos_o_diff ) * INV_PI;

		const Scalar cos_i = Vector3Ops::Dot( Vector3Ops::Normalize(-ri.ray.Dir()), ri.onb.w() );
		const Scalar fromK1 = 1.0 - pow( 1.0 - r_max(0.0, cos_o_diff) * 0.5, 5.0 );
		const Scalar fromK2 = 1.0 - pow( 1.0 - r_max(0.0, cos_i) * 0.5, 5.0 );
		static const Scalar diffuseNorm = 28.0 / 23.0;

		diffuse.krayNM = Rd.GetColorNM(ri,nm) * (1.0 - rho) * (diffuseNorm * fromK1 * fromK2);
		scattered.AddScatteredRay( diffuse );
	}
}

// Computes the Ashikhmin-Shirley anisotropic Phong specular PDF for a given direction
static Scalar AshikminShirleySpecularPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nu,
	const Scalar nv,
	const Scalar wSpec,
	const Scalar wDiff
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

	// Weighted mixture of diffuse and specular PDFs
	const Scalar totalWeight = wSpec + wDiff;
	if( totalWeight < 1e-20 ) {
		return pdf_diffuse;
	}
	return (wSpec * pdf_specular + wDiff * pdf_diffuse) / totalWeight;
}

Scalar AshikminShirleyAnisotropicPhongSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	const RISEPel nu = Nu.GetColor(ri);
	const RISEPel nv = Nv.GetColor(ri);
	// Use average values across channels
	const Scalar nu_val = (nu[0] + nu[1] + nu[2]) / 3.0;
	const Scalar nv_val = (nv[0] + nv[1] + nv[2]) / 3.0;

	// Compute representative weights at the mirror reflection direction.
	// In Scatter: kray_spec = Rs * specFactor, kray_diff = Rd * diffFactor,
	// where specFactor = fresnel/max(cos_i,cos_o) and diffFactor depends on
	// the direction.  Using the mirror direction gives constant weights that
	// are much more representative than raw Rs/Rd, especially at grazing.
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3& n = ri.onb.w();
	const Scalar cos_i = Vector3Ops::Dot( wi, n );

	if( cos_i <= 0 ) {
		return 0;
	}

	// At mirror reflection, h = n, hdotk = cos_i
	const Scalar rs_val = ColorMath::MaxValue( Rs.GetColor(ri) );
	const Scalar fresnel_m = rs_val + (1.0 - rs_val) * pow(1.0 - cos_i, 5.0);
	const Scalar specFactor_m = r_min( fresnel_m / cos_i, 1.0 );

	static const Scalar energyConservation = 28.0 / (23.0 * PI);
	const Scalar fromK = 1.0 - pow( 1.0 - cos_i * 0.5, 5.0 );
	const Scalar diffFactor_m = energyConservation * fromK * fromK;

	const Scalar wSpec = rs_val * specFactor_m;
	const Scalar wDiff = ColorMath::MaxValue( Rd.GetColor(ri) ) * diffFactor_m;

	return AshikminShirleySpecularPdf( ri, wo, nu_val, nv_val, wSpec, wDiff );
}

Scalar AshikminShirleyAnisotropicPhongSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	const Scalar nu_val = Nu.GetColorNM(ri,nm);
	const Scalar nv_val = Nv.GetColorNM(ri,nm);

	// Representative weights at mirror direction (same as Pdf)
	const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3& n = ri.onb.w();
	const Scalar cos_i = Vector3Ops::Dot( wi, n );

	if( cos_i <= 0 ) {
		return 0;
	}

	const Scalar rs_val = fabs( Rs.GetColorNM(ri,nm) );
	const Scalar fresnel_m = rs_val + (1.0 - rs_val) * pow(1.0 - cos_i, 5.0);
	const Scalar specFactor_m = r_min( fresnel_m / cos_i, 1.0 );

	static const Scalar energyConservation = 28.0 / (23.0 * PI);
	const Scalar fromK = 1.0 - pow( 1.0 - cos_i * 0.5, 5.0 );
	const Scalar diffFactor_m = energyConservation * fromK * fromK;

	const Scalar wSpec = rs_val * specFactor_m;
	const Scalar wDiff = fabs( Rd.GetColorNM(ri,nm) ) * diffFactor_m;

	return AshikminShirleySpecularPdf( ri, wo, nu_val, nv_val, wSpec, wDiff );
}
