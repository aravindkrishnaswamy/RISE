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
#include "../Utilities/MicrofacetEnergyLUT.h"
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
	const IORStack& ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;

	// Ensure normal faces the incoming ray
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	Scalar alpha = ColorMath::MaxValue( pMasking.GetColor(ri) );

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture weights: diffuse + specular + multiscatter
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;

	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;
	const Scalar pSpecSelect    = (total > 1e-10) ? ws / total : 0.0;
	// pMSSelect = wms / total = 1 - pDiffuseSelect - pSpecSelect

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
			const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.kray = pDiffuse.GetColor(ri) * (1.0 / pDiffuseSelect);
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( uLobe < pDiffuseSelect + pSpecSelect )
	{
		// --- Specular lobe: VNDF importance sampling ---
		if( alpha >= 1e-6 )
		{
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
						const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * vndfPdf) / total : vndfPdf;

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
	else
	{
		// --- Multiscatter lobe: cosine hemisphere sampling ---
		const Scalar pMSSelect = 1.0 - pDiffuseSelect - pSpecSelect;
		if( pMSSelect > 1e-10 && (1.0 - Eavg) > 1e-10 )
		{
			const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
			const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );
			const Scalar cosTheta = Vector3Ops::Dot( wo, n );

			if( cosTheta > 0 )
			{
				const Scalar diffPdf = cosTheta * INV_PI;
				const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
				const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

				const Scalar cosWi = Vector3Ops::Dot( wi, n );
				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alpha );
				const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );

				const RISEPel ior = pIOR.GetColor(ri);
				const RISEPel ext = pExtinction.GetColor(ri);
				const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
				const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );

				// kray = BRDF_ms * cos / pdf_cosine / pMSSelect
				// BRDF_ms = F_ms * (1-Ess_o) * (1-Ess_i) / (PI * (1-Eavg))
				// pdf_cosine = cos / PI
				// kray = F_ms * (1-Ess_o) * (1-Ess_i) / (1-Eavg) / pMSSelect
				const RISEPel specColor = pSpecular.GetColor(ri);
				const RISEPel kray = specColor * F_ms *
					((1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect));

				if( ColorMath::MaxValue( kray ) > 0 )
				{
					ScatteredRay ms;
					ms.type = ScatteredRay::eRayDiffuse;
					ms.ray.Set( ri.ptIntersection, wo );
					ms.kray = kray;
					ms.pdf = mixPdf;
					ms.isDelta = false;
					scattered.AddScatteredRay( ms );
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
	const IORStack& ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;

	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	Scalar alpha = pMasking.GetColorNM(ri,nm);

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture weights
	const Scalar wd = pDiffuse.GetColorNM(ri,nm);
	const Scalar ws = pSpecular.GetColorNM(ri,nm);
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;

	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;
	const Scalar pSpecSelect    = (total > 1e-10) ? ws / total : 0.0;

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
			const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.krayNM = pDiffuse.GetColorNM(ri,nm) / pDiffuseSelect;
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( uLobe < pDiffuseSelect + pSpecSelect )
	{
		// --- Specular lobe ---
		if( alpha >= 1e-6 )
		{
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
						const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * vndfPdf) / total : vndfPdf;

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
	else
	{
		// --- Multiscatter lobe ---
		const Scalar pMSSelect = 1.0 - pDiffuseSelect - pSpecSelect;
		if( pMSSelect > 1e-10 && (1.0 - Eavg) > 1e-10 )
		{
			const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
			const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );
			const Scalar cosTheta = Vector3Ops::Dot( wo, n );

			if( cosTheta > 0 )
			{
				const Scalar diffPdf = cosTheta * INV_PI;
				const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
				const Scalar mixPdf = (total > 1e-10) ? ((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

				const Scalar cosWi = Vector3Ops::Dot( wi, n );
				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alpha );
				const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );

				const Scalar iorVal = pIOR.GetColorNM(ri,nm);
				const Scalar extVal = pExtinction.GetColorNM(ri,nm);
				const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
				const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );

				const Scalar krayNM = pSpecular.GetColorNM(ri,nm) * F_ms *
					(1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect);

				if( krayNM > 0 )
				{
					ScatteredRay ms;
					ms.type = ScatteredRay::eRayDiffuse;
					ms.ray.Set( ri.ptIntersection, wo );
					ms.krayNM = krayNM;
					ms.pdf = mixPdf;
					ms.isDelta = false;
					scattered.AddScatteredRay( ms );
				}
			}
		}
	}
}

Scalar CookTorranceSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
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
	Scalar alpha = ColorMath::MaxValue( pMasking.GetColor(ri) );
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture PDF weighted by painter albedos
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;
	if( total < 1e-10 ) return cosTheta * INV_PI;

	const Scalar diffPdf = cosTheta * INV_PI;
	const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, woNorm, n, alpha ) : 0;

	// Diffuse and multiscatter lobes both use cosine hemisphere sampling
	return ((wd + wms) * diffPdf + ws * specPdf) / total;
}

Scalar CookTorranceSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}
