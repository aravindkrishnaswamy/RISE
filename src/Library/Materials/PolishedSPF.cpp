//////////////////////////////////////////////////////////////////////
//
//  PolishedSPF.cpp - Implementation of the polished SPF
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
#include "PolishedSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

PolishedSPF::PolishedSPF(
	const IPainter& Rd_,
	const IPainter& tau_,
	const IPainter& Nt_,
	const IPainter& s,
	const bool hg
	) :
  Rd( Rd_ ),
  tau( tau_ ),
  Nt( Nt_ ),
  scat( s ),
  bHG( hg )
{
	Rd.addref();
	tau.addref();
	Nt.addref();
	scat.addref();
}

PolishedSPF::~PolishedSPF( )
{
	Rd.release();
	tau.release();
	Nt.release();
	scat.release();
}

Scalar PolishedSPF::GenerateScatteredRayFromPolish(
	ScatteredRay& dielectric,
	const Vector3 normal,										///< [in] Normal
	const Vector3 reflected,									///< [in] Reflected ray
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& random,										///< [in] Random numbers
	const Scalar scatfunc,
	const Scalar ior,
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Vector3	vRefracted = ri.ray.Dir();
	const Vector3	vIn = vRefracted;

	Scalar		Rs = 0.0;
	if( Optics::CalculateRefractedRay( normal, ior_stack?ior_stack->top():1.0, ior, vRefracted ) ) {
		Rs = Optics::CalculateDielectricReflectance( vIn, vRefracted, normal, ior_stack?ior_stack->top():1.0, ior );
	}

	Vector3	rv = reflected;

	// Generate one reflected ray from the polish
	Scalar alpha = 0;

	if( bHG ) {
		if( scatfunc<1 ) {
			const Scalar& g = scatfunc;
			do {
				const Scalar inner = (1.0 - g*g) / (1 - g + 2*g*random.x);
				alpha = acos( (1/(2.0*g)) * (1 + g*g - inner*inner) );
			} while( alpha < 0 || alpha > PI_OV_TWO );

			dielectric.isDelta = false;
			// HG phase function PDF: p(cos_alpha) = (1-g^2) / (4*PI*(1+g^2-2*g*cos(alpha))^(3/2))
			// Convert to solid angle (already in solid angle for phase functions)
			const Scalar cos_alpha = cos(alpha);
			const Scalar denom = 1.0 + g*g - 2.0*g*cos_alpha;
			dielectric.pdf = (1.0 - g*g) / (FOUR_PI * denom * sqrt(denom));
		} else {
			// Perfect mirror reflection (g >= 1)
			dielectric.isDelta = true;
			dielectric.pdf = 1.0;
		}
	} else {
		if( scatfunc < 1000000.0 ) {
			alpha = acos( pow(random.x, 1.0 / (scatfunc+1.0)) );

			dielectric.isDelta = false;
			// Phong lobe PDF: p(alpha) = (n+1)/(2*PI) * cos^n(alpha)
			const Scalar cos_alpha = cos(alpha);
			dielectric.pdf = (scatfunc + 1.0) / TWO_PI * pow(cos_alpha, scatfunc);
		} else {
			// Perfect mirror reflection (very high Phong exponent)
			dielectric.isDelta = true;
			dielectric.pdf = 1.0;
		}
	}

	// Use the warping function for a Phong based PDF
	if( alpha > 0 ) {
		rv = GeometricUtilities::Perturb(
			rv,
			alpha,
			TWO_PI * random.y
			);
	}

	dielectric.ray.Set( ri.ptIntersection, rv );

	return Rs;
}

void PolishedSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	dielectric;
	dielectric.type = ScatteredRay::eRayReflection;
	RISEPel Rs;

	RISEPel scattering = scat.GetColor(ri);
	RISEPel ior = Nt.GetColor(ri);

	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.Dir())>0 ? -ri.vNormal : ri.vNormal;
	const Vector3 rv = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	if( scattering[0] == scattering[1] && scattering[1] == scattering[2] &&
		ior[0] == ior[1] && ior[1] == ior[2] )
	{
		Rs = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, Point2(sampler.Get1D(),sampler.Get1D()), scattering[0], ior[0], ior_stack );
		dielectric.kray = tau.GetColor(ri) * Rs;

		if( Vector3Ops::Dot( dielectric.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( dielectric );
		}
	}
	else
	{
		Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		for( int i=0; i<3; i++ ) {
			Rs[i] = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, ptrand, scattering[i], ior[i], ior_stack );
			dielectric.kray = 0;
			dielectric.kray[i] = tau.GetColor(ri)[i] * Rs[i];

			if( Vector3Ops::Dot( dielectric.ray.Dir(), ri.onb.w() ) > 0.0 ) {
				scattered.AddScatteredRay( dielectric );
			}
		}
	}

	if( ColorMath::MinValue(Rs) < 1.0 )
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.isDelta = false;

		// Generate a reflected ray with a cosine distribution
		diffuse.kray = Rd.GetColor(ri) * (1.0-Rs);
 		diffuse.ray.Set(
			ri.ptIntersection,
			GeometricUtilities::CreateDiffuseVector( ri.onb, Point2(sampler.Get1D(),sampler.Get1D()) )
			);
		// Cosine-weighted hemisphere: pdf = cos(theta) / PI
		const Scalar cos_theta = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
		diffuse.pdf = r_max( 0.0, cos_theta ) * INV_PI;

		if( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( diffuse );
		}
	}
}

void PolishedSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	dielectric;
	dielectric.type = ScatteredRay::eRayReflection;

	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.Dir())>0 ? -ri.vNormal : ri.vNormal;
	const Vector3 rv = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	Scalar Rs = GenerateScatteredRayFromPolish( dielectric, n, rv, ri, Point2(sampler.Get1D(),sampler.Get1D()), scat.GetColorNM(ri,nm), Nt.GetColorNM(ri,nm), ior_stack );
	dielectric.krayNM = tau.GetColorNM(ri,nm) * Rs;


	if( Vector3Ops::Dot( dielectric.ray.Dir(), ri.onb.w() ) > 0.0 ) {
		scattered.AddScatteredRay( dielectric );
	}

	if( Rs < 1.0 )
	{
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.isDelta = false;

		// Generate a reflected ray with a cosine distribution
		diffuse.krayNM = Rd.GetColorNM(ri,nm) * (1.0-Rs);
		diffuse.ray.Set(
			ri.ptIntersection,
			GeometricUtilities::CreateDiffuseVector( ri.onb, Point2(sampler.Get1D(),sampler.Get1D()) )
			);
		const Scalar cos_theta = Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() );
		diffuse.pdf = r_max( 0.0, cos_theta ) * INV_PI;

		if( Vector3Ops::Dot( diffuse.ray.Dir(), ri.onb.w() ) > 0.0 ) {
			scattered.AddScatteredRay( diffuse );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateKrayNM — HWSS companion throughput evaluation.
//
// Returns the krayNM that ScatterNM would have produced for the
// given lobe at wavelength nm.  Both lobes are direction-independent:
//   coat:    tau(nm) * Rs(nm, theta_i)
//   diffuse: Rd(nm) * (1 - Rs(nm, theta_i))
// where Rs is the Fresnel reflectance at the incident angle.
//////////////////////////////////////////////////////////////////////
Scalar PolishedSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack* ior_stack
	) const
{
	// Compute Fresnel reflectance at the incident angle (same logic
	// as GenerateScatteredRayFromPolish lines 62-68).
	const Vector3 n = Vector3Ops::Dot( ri.vNormal, ri.ray.Dir() ) > 0
		? -ri.vNormal : ri.vNormal;
	Vector3 vRefracted = ri.ray.Dir();
	Scalar Rs = 0.0;
	const Scalar iorTop = ior_stack ? ior_stack->top() : 1.0;
	const Scalar iorCoat = Nt.GetColorNM( ri, nm );
	if( Optics::CalculateRefractedRay( n, iorTop, iorCoat, vRefracted ) ) {
		Rs = Optics::CalculateDielectricReflectance(
			ri.ray.Dir(), vRefracted, n, iorTop, iorCoat );
	}

	if( rayType == ScatteredRay::eRayReflection ) {
		return tau.GetColorNM( ri, nm ) * Rs;
	}
	else if( rayType == ScatteredRay::eRayDiffuse ) {
		return Rd.GetColorNM( ri, nm ) * ( 1.0 - Rs );
	}

	return -1;
}

// Computes the Polished SPF PDF for a given direction
static Scalar PolishedPdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar scatfunc,
	const Scalar ior,
	const bool bHG,
	const IORStack* const ior_stack,
	const Scalar wSpec,
	const Scalar wDiff
	)
{
	const Vector3& n = ri.onb.w();
	const Scalar cos_theta_o = Vector3Ops::Dot( wo, n );

	if( cos_theta_o <= 0.0 ) {
		return 0.0;
	}

	// Diffuse PDF: cosine-weighted hemisphere
	const Scalar pdf_diffuse = cos_theta_o * INV_PI;

	// Specular PDF: depends on whether it's a delta or not
	bool is_delta = false;

	if( bHG ) {
		is_delta = (scatfunc >= 1.0);
	} else {
		is_delta = (scatfunc >= 1000000.0);
	}

	if( is_delta ) {
		// Delta distribution for specular: pdf is 0 for any non-delta query direction
		// Only the diffuse component contributes
		return pdf_diffuse;
	}

	// For non-delta specular, compute the lobe PDF
	// The specular ray is perturbed from the perfect reflection direction
	const Vector3 vn = Vector3Ops::Dot(ri.vNormal, ri.ray.Dir())>0 ? -ri.vNormal : ri.vNormal;
	const Vector3 rv = Optics::CalculateReflectedRay( ri.ray.Dir(), vn );

	// Angle between wo and the reflected direction
	const Scalar cos_alpha = Vector3Ops::Dot( wo, rv );

	Scalar pdf_specular = 0.0;

	if( cos_alpha > 0.0 ) {
		if( bHG ) {
			// Henyey-Greenstein phase function
			const Scalar& g = scatfunc;
			const Scalar denom = 1.0 + g*g - 2.0*g*cos_alpha;
			pdf_specular = (1.0 - g*g) / (FOUR_PI * denom * sqrt(denom));
		} else {
			// Phong lobe: (n+1)/(2*PI) * cos^n(alpha)
			pdf_specular = (scatfunc + 1.0) / TWO_PI * pow(cos_alpha, scatfunc);
		}
	}

	// Weighted mixture of diffuse and specular PDFs
	const Scalar totalWeight = wSpec + wDiff;
	if( totalWeight < 1e-20 ) {
		return pdf_diffuse;
	}
	return (wSpec * pdf_specular + wDiff * pdf_diffuse) / totalWeight;
}

Scalar PolishedSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const RISEPel s = scat.GetColor(ri);
	const RISEPel ior_val = Nt.GetColor(ri);
	// Use average values across channels
	const Scalar s_val = (s[0] + s[1] + s[2]) / 3.0;
	const Scalar ior_avg = (ior_val[0] + ior_val[1] + ior_val[2]) / 3.0;

	// Compute Fresnel reflectance for lobe weighting
	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.Dir())>0 ? -ri.vNormal : ri.vNormal;
	Vector3 vRefracted = ri.ray.Dir();
	Scalar Rs = 0.0;
	if( Optics::CalculateRefractedRay( n, ior_stack?ior_stack->top():1.0, ior_avg, vRefracted ) ) {
		Rs = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, n, ior_stack?ior_stack->top():1.0, ior_avg );
	}

	// Weight by MaxValue(kray) to match RandomlySelect:
	// specular kray = tau * Rs, diffuse kray = Rd * (1 - Rs)
	const Scalar wSpec = ColorMath::MaxValue( tau.GetColor(ri) * Rs );
	const Scalar wDiff = ColorMath::MaxValue( Rd.GetColor(ri) * (1.0 - Rs) );

	return PolishedPdf( ri, wo, s_val, ior_avg, bHG, ior_stack, wSpec, wDiff );
}

Scalar PolishedSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	const Scalar s_val = scat.GetColorNM(ri,nm);
	const Scalar ior_val = Nt.GetColorNM(ri,nm);

	// Compute Fresnel reflectance for lobe weighting
	const Vector3 n = Vector3Ops::Dot(ri.vNormal, ri.ray.Dir())>0 ? -ri.vNormal : ri.vNormal;
	Vector3 vRefracted = ri.ray.Dir();
	Scalar Rs = 0.0;
	if( Optics::CalculateRefractedRay( n, ior_stack?ior_stack->top():1.0, ior_val, vRefracted ) ) {
		Rs = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, n, ior_stack?ior_stack->top():1.0, ior_val );
	}

	// Weight by krayNM magnitude: specular = tau*Rs, diffuse = Rd*(1-Rs)
	const Scalar wSpec = fabs( tau.GetColorNM(ri,nm) * Rs );
	const Scalar wDiff = fabs( Rd.GetColorNM(ri,nm) * (1.0 - Rs) );

	return PolishedPdf( ri, wo, s_val, ior_val, bHG, ior_stack, wSpec, wDiff );
}
