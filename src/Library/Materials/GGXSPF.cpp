//////////////////////////////////////////////////////////////////////
//
//  GGXSPF.cpp - GGX microfacet SPF implementation.
//
//  Uses a three-lobe mixture model identical in structure to
//  CookTorranceSPF, but with anisotropic VNDF sampling and
//  height-correlated Smith G2.
//
//  Specular lobe weight derivation (with height-correlated G2):
//    BRDF_spec = D * G2 / (4 * cosWi * cosWo)
//    VNDF PDF  = D_v(m) / (4 * woH)
//    D_v(m)    = G1(wi) * wiH * D / cosWi
//    kray      = BRDF_spec * cosWo / pdf
//            = [D * G2 / (4*cosWi*cosWo)] * cosWo * [4*woH / (G1wi*wiH*D/cosWi)]
//            = G2 * woH * cosWi / (cosWi * G1wi * wiH)
//            = G2 / G1wi              (since wiH = woH for reflection)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GGXSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

GGXSPF::GGXSPF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IPainter& alphaX,
	const IPainter& alphaY,
	const IPainter& ior,
	const IPainter& ext,
	const FresnelMode fresnel_mode,
	const IPainter* tangent_rotation
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pAlphaX( alphaX ),
  pAlphaY( alphaY ),
  pIOR( ior ),
  pExtinction( ext ),
  fresnelMode( fresnel_mode ),
  pTangentRotation( tangent_rotation )
{
	pDiffuse.addref();
	pSpecular.addref();
	pAlphaX.addref();
	pAlphaY.addref();
	pIOR.addref();
	pExtinction.addref();
	if( pTangentRotation ) pTangentRotation->addref();
}

namespace
{
	// Closed-form Schlick hemispherical Fresnel average: F0 + (1-F0)/21.
	template< class T >
	inline T SchlickFresnelAvg( const T& F0 )
	{
		return F0 + (T(1.0) - F0) * (1.0 / 21.0);
	}

	// Landing 8: rotate the tangent ONB per anisotropy_rotation
	// (mirrors GGXBRDF.cpp's helper).  Returns source unchanged when
	// the rotation painter is null — every pre-L8 GGXSPF site falls
	// back here.
	inline RISE::OrthonormalBasis3D ApplyTangentRotation(
		const RISE::OrthonormalBasis3D& source,
		const RISE::IPainter* pRotation,
		const RISE::RayIntersectionGeometric& ri )
	{
		if( !pRotation ) return source;
		const RISE::Scalar angle = RISE::ColorMath::MaxValue( pRotation->GetColor( ri ) );
		return RISE::MicrofacetUtils::RotateTangent( source, angle );
	}
}

GGXSPF::~GGXSPF()
{
	pDiffuse.release();
	pSpecular.release();
	pAlphaX.release();
	pAlphaY.release();
	pIOR.release();
	pExtinction.release();
	if( pTangentRotation ) pTangentRotation->release();
}

void GGXSPF::Scatter(
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

	// Landing 8: apply the anisotropy_rotation AFTER the FlipW so the
	// rotation is in the (possibly flipped) surface tangent plane.
	myonb = ApplyTangentRotation( myonb, pTangentRotation, ri );

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

	Scalar alphaX = r_max( ColorMath::MaxValue( pAlphaX.GetColor(ri) ), Scalar(1e-4) );
	Scalar alphaY = r_max( ColorMath::MaxValue( pAlphaY.GetColor(ri) ), Scalar(1e-4) );

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// Effective alpha for isotropic LUT lookups
	const Scalar alphaEff = sqrt( alphaX * alphaY );

	// 3-lobe mixture weights: diffuse + specular + multiscatter
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;

	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;
	const Scalar pSpecSelect    = (total > 1e-10) ? ws / total : 0.0;

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
			const Scalar specPdf = (alphaEff >= 1e-6) ?
				MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY ) : 0;
			const Scalar mixPdf = (total > 1e-10) ?
				((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

			RISEPel kray = pDiffuse.GetColor(ri) * (1.0 / pDiffuseSelect);
			if( fresnelMode == eFresnelSchlickF0 )
			{
				// (1 - max(F0)) energy split per glTF spec
				const RISEPel specColor = pSpecular.GetColor(ri);
				const Scalar maxF0 = ColorMath::MaxValue( specColor );
				kray = kray * r_max( Scalar(0), Scalar(1.0) - maxF0 );
			}

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.kray = kray;
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( uLobe < pDiffuseSelect + pSpecSelect )
	{
		// --- Specular lobe: anisotropic VNDF importance sampling ---
		if( alphaEff >= 1e-6 )
		{
			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, myonb, alphaX, alphaY, u1, u2 );

			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM > 0 )
			{
				const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
				const Scalar cosTheta = Vector3Ops::Dot( wo, n );

				if( cosTheta > 0 )
				{
					const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY );

					if( vndfPdf > 1e-10 )
					{
						const Scalar diffPdf = cosTheta * INV_PI;
						const Scalar mixPdf = (total > 1e-10) ?
							((wd + wms) * diffPdf + ws * vndfPdf) / total : vndfPdf;

						// Fresnel evaluated at microfacet normal m, not macrosurface normal
						const RISEPel specColor = pSpecular.GetColor(ri);
						RISEPel F;
						if( fresnelMode == eFresnelSchlickF0 )
						{
							const Scalar cosWoH = r_max( Scalar(0), wiDotM );	// reflection: wi·m == wo·m
							F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( specColor, cosWoH );
						}
						else
						{
							F = specColor * Optics::CalculateConductorReflectance<RISEPel>(
								ri.ray.Dir(), m, RISEPel(1,1,1),
								pIOR.GetColor(ri), pExtinction.GetColor(ri) );
						}

						// Height-correlated weight: kray = F * G2 / G1(wi) / pSpecSelect
						const Vector3 wi_local(
							Vector3Ops::Dot( wi, myonb.u() ),
							Vector3Ops::Dot( wi, myonb.v() ),
							Vector3Ops::Dot( wi, myonb.w() )
						);
						const Vector3 wo_local(
							Vector3Ops::Dot( wo, myonb.u() ),
							Vector3Ops::Dot( wo, myonb.v() ),
							Vector3Ops::Dot( wo, myonb.w() )
						);

						const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );
						const Scalar G1wi = MicrofacetUtils::GGX_G1_Aniso( alphaX, alphaY, wi_local );

						const RISEPel kray = (G1wi > 1e-10) ?
							F * (G2 / (G1wi * pSpecSelect)) :
							RISEPel(0,0,0);

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
				const Scalar specPdf = (alphaEff >= 1e-6) ?
					MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY ) : 0;
				const Scalar mixPdf = (total > 1e-10) ?
					((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

				const Scalar cosWi = Vector3Ops::Dot( wi, n );
				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alphaEff );
				const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alphaEff );

				const RISEPel specColor = pSpecular.GetColor(ri);
				RISEPel kray;
				if( fresnelMode == eFresnelSchlickF0 )
				{
					const RISEPel F_avg = SchlickFresnelAvg<RISEPel>( specColor );
					const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
					kray = F_ms * ((1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect));
				}
				else
				{
					const RISEPel ior = pIOR.GetColor(ri);
					const RISEPel ext = pExtinction.GetColor(ri);
					const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
					const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
					kray = specColor * F_ms *
						((1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect));
				}

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

void GGXSPF::ScatterNM(
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
	myonb = ApplyTangentRotation( myonb, pTangentRotation, ri );	// Landing 8

	const Vector3 n = myonb.w();
	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

	Scalar alphaX = r_max( pAlphaX.GetColorNM(ri,nm), Scalar(1e-4) );
	Scalar alphaY = r_max( pAlphaY.GetColorNM(ri,nm), Scalar(1e-4) );

	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}

	const Scalar alphaEff = sqrt( alphaX * alphaY );

	// 3-lobe mixture weights
	const Scalar wd = pDiffuse.GetColorNM(ri,nm);
	const Scalar ws = pSpecular.GetColorNM(ri,nm);
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );
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
			const Scalar specPdf = (alphaEff >= 1e-6) ?
				MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY ) : 0;
			const Scalar mixPdf = (total > 1e-10) ?
				((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

			Scalar krayNM = pDiffuse.GetColorNM(ri,nm) / pDiffuseSelect;
			if( fresnelMode == eFresnelSchlickF0 )
			{
				const Scalar F0 = pSpecular.GetColorNM(ri,nm);
				krayNM = krayNM * r_max( Scalar(0), Scalar(1.0) - F0 );
			}

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.krayNM = krayNM;
			diffuse.pdf = mixPdf;
			diffuse.isDelta = false;
			scattered.AddScatteredRay( diffuse );
		}
	}
	else if( uLobe < pDiffuseSelect + pSpecSelect )
	{
		// --- Specular lobe ---
		if( alphaEff >= 1e-6 )
		{
			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, myonb, alphaX, alphaY, u1, u2 );

			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM > 0 )
			{
				const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );
				const Scalar cosTheta = Vector3Ops::Dot( wo, n );

				if( cosTheta > 0 )
				{
					const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY );

					if( vndfPdf > 1e-10 )
					{
						const Scalar diffPdf = cosTheta * INV_PI;
						const Scalar mixPdf = (total > 1e-10) ?
							((wd + wms) * diffPdf + ws * vndfPdf) / total : vndfPdf;

						// Fresnel evaluated at microfacet normal m
						const Scalar specColor = pSpecular.GetColorNM(ri,nm);
						Scalar F;
						if( fresnelMode == eFresnelSchlickF0 )
						{
							const Scalar cosWoH = r_max( Scalar(0), wiDotM );
							F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( specColor, cosWoH );
						}
						else
						{
							const Scalar fresnel = Optics::CalculateConductorReflectance(
								ri.ray.Dir(), m, 1.0,
								pIOR.GetColorNM(ri,nm), pExtinction.GetColorNM(ri,nm) );
							F = specColor * fresnel;
						}

						const Vector3 wi_local(
							Vector3Ops::Dot( wi, myonb.u() ),
							Vector3Ops::Dot( wi, myonb.v() ),
							Vector3Ops::Dot( wi, myonb.w() )
						);
						const Vector3 wo_local(
							Vector3Ops::Dot( wo, myonb.u() ),
							Vector3Ops::Dot( wo, myonb.v() ),
							Vector3Ops::Dot( wo, myonb.w() )
						);

						const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );
						const Scalar G1wi = MicrofacetUtils::GGX_G1_Aniso( alphaX, alphaY, wi_local );

						const Scalar krayNM = (G1wi > 1e-10) ?
							F * G2 / (G1wi * pSpecSelect) : 0;

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
				const Scalar specPdf = (alphaEff >= 1e-6) ?
					MicrofacetUtils::VNDF_Pdf_Aniso( wi, wo, myonb, alphaX, alphaY ) : 0;
				const Scalar mixPdf = (total > 1e-10) ?
					((wd + wms) * diffPdf + ws * specPdf) / total : diffPdf;

				const Scalar cosWi = Vector3Ops::Dot( wi, n );
				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alphaEff );
				const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alphaEff );

				Scalar krayNM;
				if( fresnelMode == eFresnelSchlickF0 )
				{
					const Scalar F0 = pSpecular.GetColorNM(ri,nm);
					const Scalar F_avg = SchlickFresnelAvg<Scalar>( F0 );
					const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
					krayNM = F_ms * (1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect);
				}
				else
				{
					const Scalar iorVal = pIOR.GetColorNM(ri,nm);
					const Scalar extVal = pExtinction.GetColorNM(ri,nm);
					const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
					const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
					krayNM = pSpecular.GetColorNM(ri,nm) * F_ms *
						(1.0 - Ess_o) * (1.0 - Ess_i) / ((1.0 - Eavg) * pMSSelect);
				}

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

Scalar GGXSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}
	myonb = ApplyTangentRotation( myonb, pTangentRotation, ri );	// Landing 8

	const Vector3 n = myonb.w();
	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	if( cosTheta <= 0 ) return 0;

	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

	Scalar alphaX = r_max( ColorMath::MaxValue( pAlphaX.GetColor(ri) ), Scalar(1e-4) );
	Scalar alphaY = r_max( ColorMath::MaxValue( pAlphaY.GetColor(ri) ), Scalar(1e-4) );
	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}
	const Scalar alphaEff = sqrt( alphaX * alphaY );

	// 3-lobe mixture PDF weighted by painter albedos
	const Scalar wd = ColorMath::MaxValue( pDiffuse.GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular.GetColor(ri) );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;
	if( total < 1e-10 ) return cosTheta * INV_PI;

	const Scalar diffPdf = cosTheta * INV_PI;
	const Scalar specPdf = (alphaEff >= 1e-6) ?
		MicrofacetUtils::VNDF_Pdf_Aniso( wi, woNorm, myonb, alphaX, alphaY ) : 0;

	return ((wd + wms) * diffPdf + ws * specPdf) / total;
}

Scalar GGXSPF::PdfNM(
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
	myonb = ApplyTangentRotation( myonb, pTangentRotation, ri );	// Landing 8

	const Vector3 n = myonb.w();
	const Vector3 woNorm = Vector3Ops::Normalize( wo );
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	if( cosTheta <= 0 ) return 0;

	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

	Scalar alphaX = r_max( pAlphaX.GetColorNM(ri,nm), Scalar(1e-4) );
	Scalar alphaY = r_max( pAlphaY.GetColorNM(ri,nm), Scalar(1e-4) );
	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}
	const Scalar alphaEff = sqrt( alphaX * alphaY );

	// 3-lobe mixture PDF weighted by per-wavelength albedos
	const Scalar wd = pDiffuse.GetColorNM(ri,nm);
	const Scalar ws = pSpecular.GetColorNM(ri,nm);
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );
	const Scalar wms = ws * (1.0 - Eavg);
	const Scalar total = wd + ws + wms;
	if( total < 1e-10 ) return cosTheta * INV_PI;

	const Scalar diffPdf = cosTheta * INV_PI;
	const Scalar specPdf = (alphaEff >= 1e-6) ?
		MicrofacetUtils::VNDF_Pdf_Aniso( wi, woNorm, myonb, alphaX, alphaY ) : 0;

	return ((wd + wms) * diffPdf + ws * specPdf) / total;
}
