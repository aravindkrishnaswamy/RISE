//////////////////////////////////////////////////////////////////////
//
//  CookTorranceSPF.cpp - Implementation of the Cook-Torrance SPF
//    using GGX VNDF importance sampling for the specular lobe and
//    cosine hemisphere sampling for the diffuse lobe.
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
#include "CookTorranceSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

CookTorranceSPF::CookTorranceSPF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IPainter& masking,
	const IPainter& ior,
	const IPainter& ext
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pMasking( masking ),
  pIOR( ior ),
  pExtinction( ext )
{
	pDiffuse.addref();
	pSpecular.addref();
	pMasking.addref();
	pIOR.addref();
	pExtinction.addref();
}
CookTorranceSPF::~CookTorranceSPF( )
{
	pDiffuse.release();
	pSpecular.release();
	pMasking.release();
	pIOR.release();
	pExtinction.release();
}

void CookTorranceSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack* const ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;

	// Ensure normal faces the incoming ray
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	const Scalar alpha = ColorMath::MaxValue( pMasking.GetColor(ri) );

	// Mixture PDF weights (albedo-based) for lobe selection
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar total = wd + ws;
	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;

	// Randomly select one lobe
	const Scalar uLobe = sampler.Get1D();

	if( uLobe < pDiffuseSelect )
	{
		// --- Diffuse lobe: cosine hemisphere sampling ---
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );
		const Scalar cosTheta = Vector3Ops::Dot( wo, n );

		if( cosTheta > 0 )
		{
			const Scalar diffPdf = cosTheta * INV_PI;
			const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
			const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.kray = pDiffuse.GetColor(ri) * (1.0 / pDiffuseSelect);
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( alpha >= 1e-6 )
	{
		// --- Specular lobe: VNDF importance sampling ---
		const Scalar pSpecSelect = 1.0 - pDiffuseSelect;
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();
		const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, myonb, alpha, u1, u2 );

		const Scalar wiDotM = Vector3Ops::Dot( wi, m );
		if( wiDotM > 0 )
		{
			const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
			const Scalar cosTheta = Vector3Ops::Dot( wo, n );

			if( cosTheta > 0 )
			{
				const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha );

				if( vndfPdf > 1e-10 )
				{
					const Scalar diffPdf = cosTheta * INV_PI;
					const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + ws * vndfPdf) / total : vndfPdf;

					const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
						ri.ray.Dir(), n, RISEPel(1,1,1),
						pIOR.GetColor(ri), pExtinction.GetColor(ri) );

					// With VNDF sampling, kray = BRDF_spec * cos / pdf_spec
					// simplifies to: pSpecular * fresnel * G1(wo)
					// Divide by selection probability for unbiased single-lobe estimate
					const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, cosTheta );
					const RISEPel specColor = pSpecular.GetColor(ri);
					const RISEPel kray = specColor * fresnel * (G1wo / pSpecSelect);

					if( ColorMath::MaxValue( kray ) > 0 )
					{
						ScatteredRay specular;
						specular.type = ScatteredRay::eRayReflection;
						specular.ray.Set( ri.ptIntersection, wo );
						specular.kray = kray;
						specular.pdf = mixPdf;
						specular.isDelta = false;
						scattered.AddScatteredRay( specular );
					}
				}
			}
		}
	}
}

void CookTorranceSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack* const ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;

	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	const Scalar alpha = pMasking.GetColorNM(ri,nm);

	// Mixture PDF weights
	const Scalar wd = pDiffuse.GetColorNM(ri,nm);
	const Scalar ws = pSpecular.GetColorNM(ri,nm);
	const Scalar total = wd + ws;
	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;

	const Scalar uLobe = sampler.Get1D();

	if( uLobe < pDiffuseSelect )
	{
		// --- Diffuse lobe ---
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );
		const Scalar cosTheta = Vector3Ops::Dot( wo, n );

		if( cosTheta > 0 )
		{
			const Scalar diffPdf = cosTheta * INV_PI;
			const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
			const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.krayNM = pDiffuse.GetColorNM(ri,nm) / pDiffuseSelect;
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( alpha >= 1e-6 )
	{
		// --- Specular lobe ---
		const Scalar pSpecSelect = 1.0 - pDiffuseSelect;
		const Scalar u1 = sampler.Get1D();
		const Scalar u2 = sampler.Get1D();
		const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, myonb, alpha, u1, u2 );

		const Scalar wiDotM = Vector3Ops::Dot( wi, m );
		if( wiDotM > 0 )
		{
			const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
			const Scalar cosTheta = Vector3Ops::Dot( wo, n );

			if( cosTheta > 0 )
			{
				const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha );

				if( vndfPdf > 1e-10 )
				{
					const Scalar diffPdf = cosTheta * INV_PI;
					const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + ws * vndfPdf) / total : vndfPdf;

					const Scalar fresnel = Optics::CalculateConductorReflectance(
						ri.ray.Dir(), n, 1.0,
						pIOR.GetColorNM(ri,nm), pExtinction.GetColorNM(ri,nm) );

					const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, cosTheta );
					const Scalar krayNM = pSpecular.GetColorNM(ri,nm) * fresnel * G1wo / pSpecSelect;

					if( krayNM > 0 )
					{
						ScatteredRay specular;
						specular.type = ScatteredRay::eRayReflection;
						specular.ray.Set( ri.ptIntersection, wo );
						specular.krayNM = krayNM;
						specular.pdf = mixPdf;
						specular.isDelta = false;
						scattered.AddScatteredRay( specular );
					}
				}
			}
		}
	}
}

Scalar CookTorranceSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 n = myonb.w();
	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	if( cosTheta <= 0 ) return 0;

	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	const Scalar alpha = ColorMath::MaxValue( pMasking.GetColor(ri) );

	// Mixture PDF weighted by painter albedos
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar total = wd + ws;
	if( total < 1e-10 ) return cosTheta * INV_PI;

	const Scalar diffPdf = cosTheta * INV_PI;
	const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, woNorm, n, alpha ) : 0;

	return (wd * diffPdf + ws * specPdf) / total;
}

Scalar CookTorranceSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}
