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
	const IScalarPainter& masking,
	const IScalarPainter& ior,
	const IScalarPainter& ext
	) :
  pDiffuse( &diffuse ),
  pSpecular( &specular ),
  pMasking( &masking ),
  pIOR( &ior ),
  pExtinction( &ext )
{
	pDiffuse->addref();
	pSpecular->addref();
	pMasking->addref();
	pIOR->addref();
	pExtinction->addref();
}
CookTorranceSPF::~CookTorranceSPF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pMasking );
	safe_release( pIOR );
	safe_release( pExtinction );
}

void CookTorranceSPF::SetDiffuse( const IPainter& v )        { v.addref(); safe_release( pDiffuse );    pDiffuse    = &v; }
void CookTorranceSPF::SetSpecular( const IPainter& v )       { v.addref(); safe_release( pSpecular );   pSpecular   = &v; }
void CookTorranceSPF::SetMasking( const IScalarPainter& v )  { v.addref(); safe_release( pMasking );    pMasking    = &v; }
void CookTorranceSPF::SetIOR( const IScalarPainter& v )      { v.addref(); safe_release( pIOR );        pIOR        = &v; }
void CookTorranceSPF::SetExtinction( const IScalarPainter& v ){ v.addref(); safe_release( pExtinction ); pExtinction = &v; }

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

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a wo that validates against the
	// (tilted) shading normal can still point below the geometric surface --
	// the continuation ray then tunnels into the solid.  Orient the
	// geometric normal to the shading-normal side once here and gate every
	// lobe's sample against it below.  Degenerate vGeomNormal (SquaredModulus
	// guard, matches GlintModifier.cpp) falls back to the shading normal,
	// making the gate a no-op.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;
	Scalar alpha = pMasking->GetValuesAt(ri).v[0];

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture weights: diffuse + specular + multiscatter
	const Scalar wd = ColorMath::MaxValue( pDiffuse->GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular->GetColor(ri) );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	// H6: direction-aware MS selection weight -- the true MS albedo for
	// THIS incident direction is F_ms*(1-Ess(cosWi)), not the
	// hemisphere-averaged F_ms*(1-Eavg); using the averaged form let a
	// grazing cosWi on a smooth surface produce a huge, rare kray (see
	// docs/... H6).  Eavg itself is still used unchanged below in the
	// MS lobe's BRDF-value term.
	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
	const Scalar wms = ws * (1.0 - Ess_i);
	const Scalar total = wd + ws + wms;
	// Exact MS-lobe outgoing-direction normalization, shared by every
	// mixPdf site below and by the MS lobe's own sampler/kray.
	const Scalar msZ = MicrofacetEnergyLUT::MSLobeZ( alpha );

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

		if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
		{
			const Scalar diffPdf = cosTheta * INV_PI;
			const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
			const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
			const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.kray = pDiffuse->GetColor(ri) * (1.0 / pDiffuseSelect);
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

				if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
				{
					const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha );

					if( vndfPdf > 1e-10 )
					{
						const Scalar diffPdf = cosTheta * INV_PI;
						const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
						const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * vndfPdf) / total : vndfPdf;

						const ScalarTriple iorT = pIOR->GetValuesAt(ri);
						const ScalarTriple extT = pExtinction->GetValuesAt(ri);
						const RISEPel iorPel( iorT.v[0], iorT.v[1], iorT.v[2] );
						const RISEPel extPel( extT.v[0], extT.v[1], extT.v[2] );
						const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
							ri.ray.Dir(), n, RISEPel(1,1,1),
							iorPel, extPel );

						// With VNDF sampling, kray = BRDF_spec * cos / pdf_spec
						// simplifies to: pSpecular * fresnel * G1(wo)
						// Divide by selection probability for unbiased single-lobe estimate
						const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, cosTheta );
						const RISEPel specColor = pSpecular->GetColor(ri);
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
		// --- Multiscatter lobe: importance-sample wo ~ (1-Ess(cosWo))*cosWo ---
		// (H6: replaces cosine-hemisphere sampling; see MicrofacetEnergyLUT.h
		// header comment above MSLobeZ/SampleMSCosTheta/MSPdf.)
		const Scalar pMSSelect = 1.0 - pDiffuseSelect - pSpecSelect;
		if( pMSSelect > 1e-10 && (1.0 - Eavg) > 1e-10 )
		{
			const Scalar u1ms = sampler.Get1D();
			const Scalar u2ms = sampler.Get1D();
			const Scalar cosTheta = MicrofacetEnergyLUT::SampleMSCosTheta( alpha, u1ms );
			const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1.0) - cosTheta * cosTheta ) );
			const Scalar phiMs = TWO_PI * u2ms;
			const Vector3 wo = myonb.Transform( Vector3( sinTheta * cos(phiMs), sinTheta * sin(phiMs), cosTheta ) );

			if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
			{
				const Scalar diffPdf = cosTheta * INV_PI;
				const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
				const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
				const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total : diffPdf;

				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alpha );
				// Ess_i, cosWi computed once at the top of Scatter() (shared
				// with the direction-aware wms selection weight).

				const ScalarTriple iorT = pIOR->GetValuesAt(ri);
				const ScalarTriple extT = pExtinction->GetValuesAt(ri);
				const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
				const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
				const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
				// specColor INSIDE the average: the tinted per-bounce reflectance specColor*F_avg
				// compounds across bounces (matches the single-scatter lobe specColor*fresnel).
				// Pulling it outside the nonlinear Fms over-brightens tinted rough metals.
				const RISEPel specColor = pSpecular->GetColor(ri);
				const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * F_avg, Eavg );

				// H6: honest f*cos/pdf estimator (was the hard-coded
				// (1-Ess_o)(1-Ess_i)/((1-Eavg)*pMSSelect) constant that
				// assumed (1-Ess)==(1-Eavg) and blew up at grazing cosWi).
				// f_ms (the BRDF value) is UNCHANGED: F_ms*(1-Ess_o)*(1-Ess_i)/(PI*(1-Eavg)).
				const RISEPel f_ms = F_ms * ((1.0 - Ess_o) * (1.0 - Ess_i) * INV_PI / (1.0 - Eavg));
				const RISEPel kray = (msPdfHere > 1e-14) ?
					f_ms * (cosTheta / (msPdfHere * pMSSelect)) : RISEPel(0,0,0);

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

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a wo that validates against the
	// (tilted) shading normal can still point below the geometric surface --
	// the continuation ray then tunnels into the solid.  Orient the
	// geometric normal to the shading-normal side once here and gate every
	// lobe's sample against it below.  Degenerate vGeomNormal (SquaredModulus
	// guard, matches GlintModifier.cpp) falls back to the shading normal,
	// making the gate a no-op.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;
	Scalar alpha = pMasking->GetValueAtNM(ri,nm);

	// Glossy filtering: increase effective roughness
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture weights
	const Scalar wd = pDiffuse->GetColorNM(ri,nm);
	const Scalar ws = pSpecular->GetColorNM(ri,nm);
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	// H6: direction-aware MS selection weight (see Scatter()'s twin comment).
	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
	const Scalar wms = ws * (1.0 - Ess_i);
	const Scalar total = wd + ws + wms;
	const Scalar msZ = MicrofacetEnergyLUT::MSLobeZ( alpha );

	const Scalar pDiffuseSelect = (total > 1e-10) ? wd / total : 1.0;
	const Scalar pSpecSelect    = (total > 1e-10) ? ws / total : 0.0;

	const Scalar uLobe = sampler.Get1D();

	if( uLobe < pDiffuseSelect )
	{
		// --- Diffuse lobe ---
		const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );
		const Scalar cosTheta = Vector3Ops::Dot( wo, n );

		if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
		{
			const Scalar diffPdf = cosTheta * INV_PI;
			const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
			const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
			const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total : diffPdf;

			ScatteredRay diffuse;
			diffuse.type = ScatteredRay::eRayDiffuse;
			diffuse.ray.Set( ri.ptIntersection, wo );
			diffuse.krayNM = pDiffuse->GetColorNM(ri,nm) / pDiffuseSelect;
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

				if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
				{
					const Scalar vndfPdf = MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha );

					if( vndfPdf > 1e-10 )
					{
						const Scalar diffPdf = cosTheta * INV_PI;
						const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
						const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * vndfPdf) / total : vndfPdf;

						const Scalar fresnel = Optics::CalculateConductorReflectance(
							ri.ray.Dir(), n, 1.0,
							pIOR->GetValueAtNM(ri,nm), pExtinction->GetValueAtNM(ri,nm) );

						const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, cosTheta );
						const Scalar krayNM = pSpecular->GetColorNM(ri,nm) * fresnel * G1wo / pSpecSelect;

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
		// --- Multiscatter lobe: importance-sample wo ~ (1-Ess(cosWo))*cosWo ---
		const Scalar pMSSelect = 1.0 - pDiffuseSelect - pSpecSelect;
		if( pMSSelect > 1e-10 && (1.0 - Eavg) > 1e-10 )
		{
			const Scalar u1ms = sampler.Get1D();
			const Scalar u2ms = sampler.Get1D();
			const Scalar cosTheta = MicrofacetEnergyLUT::SampleMSCosTheta( alpha, u1ms );
			const Scalar sinTheta = sqrt( r_max( Scalar(0), Scalar(1.0) - cosTheta * cosTheta ) );
			const Scalar phiMs = TWO_PI * u2ms;
			const Vector3 wo = myonb.Transform( Vector3( sinTheta * cos(phiMs), sinTheta * sin(phiMs), cosTheta ) );

			if( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 )
			{
				const Scalar diffPdf = cosTheta * INV_PI;
				const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, wo, n, alpha ) : 0;
				const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, msZ );
				const Scalar mixPdf = (total > 1e-10) ? (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total : diffPdf;

				const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosTheta, alpha );
				// Ess_i, cosWi computed once at the top of ScatterNM().

				const Scalar iorVal = pIOR->GetValueAtNM(ri,nm);
				const Scalar extVal = pExtinction->GetValueAtNM(ri,nm);
				const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
				// specColor INSIDE the average: the tinted per-bounce reflectance specColor*F_avg
				// compounds across bounces (matches the single-scatter lobe specColor*fresnel).
				const Scalar specColor = pSpecular->GetColorNM(ri,nm);
				const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * F_avg, Eavg );

				// H6: honest f*cos/pdf estimator (see Scatter()'s twin comment).
				const Scalar f_ms = F_ms * (1.0 - Ess_o) * (1.0 - Ess_i) * INV_PI / (1.0 - Eavg);
				const Scalar krayNM = (msPdfHere > 1e-14) ?
					f_ms * cosTheta / (msPdfHere * pMSSelect) : 0;

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

	// Geometric-horizon gate (MIS consistency with Scatter's sampler-side
	// gate): a wo the sampler can no longer emit contributes zero density.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, n ) >= 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( woNorm, geomN ) <= 0 ) return 0;

	const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
	Scalar alpha = pMasking->GetValuesAt(ri).v[0];
	if( ri.glossyFilterWidth > 0 ) {
		alpha = r_min( alpha + ri.glossyFilterWidth, Scalar(1.0) );
	}

	// 3-lobe mixture PDF weighted by painter albedos
	const Scalar wd = ColorMath::MaxValue( pDiffuse->GetColor(ri) );
	const Scalar ws = ColorMath::MaxValue( pSpecular->GetColor(ri) );
	// H6: direction-aware MS selection weight (see Scatter()'s twin comment).
	const Scalar cosWi = Vector3Ops::Dot( wi, n );
	const Scalar wms = ws * (1.0 - MicrofacetEnergyLUT::LookupEss( cosWi, alpha ));
	const Scalar total = wd + ws + wms;
	if( total < 1e-10 ) return cosTheta * INV_PI;

	const Scalar diffPdf = cosTheta * INV_PI;
	const Scalar specPdf = (alpha >= 1e-6) ? MicrofacetUtils::VNDF_Pdf( wi, woNorm, n, alpha ) : 0;
	const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alpha, MicrofacetEnergyLUT::MSLobeZ( alpha ) );

	// Diffuse, specular, and multiscatter lobes each importance-sample their
	// own outgoing-direction density (H6); this is the 3-term mixture pdf.
	return (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total;
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
