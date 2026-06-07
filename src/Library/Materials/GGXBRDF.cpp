//////////////////////////////////////////////////////////////////////
//
//  GGXBRDF.cpp - GGX microfacet BRDF implementation.
//
//  Uses anisotropic GGX NDF (Walter et al. 2007) with Smith
//  height-correlated masking-shadowing G2 (Heitz 2014) and
//  Kulla-Conty multiscattering energy compensation (2017).
//
//  The height-correlated G2 = 1/(1 + Lambda(wi) + Lambda(wo)) is
//  more accurate than the separable G1(wi)*G1(wo) used by
//  CookTorrance, because it accounts for the correlation between
//  masking and shadowing at nearby microsurface heights.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GGXBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"
#include "../Utilities/ThinFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

GGXBRDF::GGXBRDF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IScalarPainter& alphaX,
	const IScalarPainter& alphaY,
	const IScalarPainter& ior,
	const IScalarPainter& ext,
	const FresnelMode fresnel_mode,
	const IPainter* tangent_rotation,
	const IScalarPainter* film_ior,
	const IScalarPainter* film_extinction,
	const IScalarPainter* film_thickness
	) :
  pDiffuse( &diffuse ),
  pSpecular( &specular ),
  pAlphaX( &alphaX ),
  pAlphaY( &alphaY ),
  pIOR( &ior ),
  pExtinction( &ext ),
  fresnelMode( fresnel_mode ),
  pTangentRotation( tangent_rotation ),
  pFilmIOR( film_ior ),
  pFilmExtinction( film_extinction ),
  pFilmThickness( film_thickness )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlphaX->addref();
	pAlphaY->addref();
	pIOR->addref();
	pExtinction->addref();
	if( pTangentRotation ) pTangentRotation->addref();
	if( pFilmIOR )        pFilmIOR->addref();
	if( pFilmExtinction ) pFilmExtinction->addref();
	if( pFilmThickness )  pFilmThickness->addref();
}

namespace
{
	// Hemispherical Fresnel average for Schlick's approximation:
	//   F_avg = 2 ∫₀¹ [F0 + (1-F0)(1-μ)^5] μ dμ = F0 + (1-F0)/21
	// Closed-form, no quadrature needed.  Per-channel for RISEPel.
	template< class T >
	inline T SchlickFresnelAvg( const T& F0 )
	{
		return F0 + (T(1.0) - F0) * (1.0 / 21.0);
	}
}

GGXBRDF::~GGXBRDF()
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlphaX );
	safe_release( pAlphaY );
	safe_release( pIOR );
	safe_release( pExtinction );
	if( pTangentRotation ) pTangentRotation->release();
	if( pFilmIOR )        pFilmIOR->release();
	if( pFilmExtinction ) pFilmExtinction->release();
	if( pFilmThickness )  pFilmThickness->release();
}

void GGXBRDF::SetDiffuse( const IPainter& v )       { v.addref(); safe_release( pDiffuse );    pDiffuse    = &v; }
void GGXBRDF::SetSpecular( const IPainter& v )      { v.addref(); safe_release( pSpecular );   pSpecular   = &v; }
void GGXBRDF::SetAlphaX( const IScalarPainter& v )  { v.addref(); safe_release( pAlphaX );     pAlphaX     = &v; }
void GGXBRDF::SetAlphaY( const IScalarPainter& v )  { v.addref(); safe_release( pAlphaY );     pAlphaY     = &v; }
void GGXBRDF::SetIOR( const IScalarPainter& v )     { v.addref(); safe_release( pIOR );        pIOR        = &v; }
void GGXBRDF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }

namespace
{
	// Landing 8: resolve the per-shading-point tangent ONB.  When the
	// material has a non-null rotation painter, sample it at this hit
	// and rotate the tangent frame around w by that angle.  When null
	// (every pre-L8 GGX site), returns ri.onb verbatim — bit-identical
	// to the pre-L8 path.
	inline RISE::OrthonormalBasis3D ResolveTangentONB(
		const RISE::OrthonormalBasis3D& source,
		const RISE::IPainter* pRotation,
		const RISE::RayIntersectionGeometric& ri )
	{
		if( !pRotation ) return source;
		const RISE::Scalar angle = RISE::ColorMath::MaxValue( pRotation->GetColor( ri ) );
		return RISE::MicrofacetUtils::RotateTangent( source, angle );
	}
}

RISEPel GGXBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	// Landing 8: rotate the tangent frame per anisotropy_rotation.
	// effOnb == ri.onb when no rotation painter is set.
	const OrthonormalBasis3D effOnb = ResolveTangentONB( ri.onb, pTangentRotation, ri );
	const Vector3 n = effOnb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );         // light direction (toward light)
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );    // view direction (toward viewer)

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return RISEPel(0,0,0);
	}

	// Read roughness parameters, clamped to avoid division-by-zero in NDF
	const Scalar alphaX = r_max( pAlphaX->GetValuesAt(ri).v[0], Scalar(1e-4) );
	const Scalar alphaY = r_max( pAlphaY->GetValuesAt(ri).v[0], Scalar(1e-4) );

	// Half-vector and tangent-space projections
	const Vector3 h = Vector3Ops::Normalize( v + r );
	const Vector3 h_local(
		Vector3Ops::Dot( h, effOnb.u() ),
		Vector3Ops::Dot( h, effOnb.v() ),
		Vector3Ops::Dot( h, effOnb.w() )
	);
	const Vector3 wi_local(
		Vector3Ops::Dot( v, effOnb.u() ),
		Vector3Ops::Dot( v, effOnb.v() ),
		Vector3Ops::Dot( v, effOnb.w() )
	);
	const Vector3 wo_local(
		Vector3Ops::Dot( r, effOnb.u() ),
		Vector3Ops::Dot( r, effOnb.v() ),
		Vector3Ops::Dot( r, effOnb.w() )
	);

	// Anisotropic GGX NDF
	const Scalar D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alphaX, alphaY, h_local );

	// Height-correlated Smith G2
	const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );

	// Single-scatter specular: D * G2 / (4 * cosWi * cosWo)
	const Scalar specFactor = D * G2 / (4.0 * nv * nr);

	const RISEPel specColor = pSpecular->GetColor(ri);

	RISEPel specular(0,0,0);

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector), not macrosurface normal
		if( fresnelMode == eFresnelSchlickF0 )
		{
			// Schlick: F = F0 + (1-F0)*(1-cosθ_h)^5, where specColor is F0.
			// cosθ_h = max(0, dot(wo, h)).
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const RISEPel F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( specColor, cosWoH );
			specular = F * specFactor;
		}
		else if( fresnelMode == eFresnelThinFilmConductor )
		{
			// Thin-film interference on the RGB (no-wavelength) path: the
			// spectral interference R(λ) is pre-integrated against the CIE
			// CMFs in the WHITE-NORMALIZED ALBEDO BASIS (illuminant-
			// independent — docs/THIN_FILM_INTERFERENCE.md §8), so a perfect
			// reflector → neutral white, NOT a D65-tinted colour.  This is
			// PREVIEW-grade; the spectral path (valueNM) is authoritative.
			// cosThetaI is the half-vector cosine dot(r,h), the same cosine
			// the conductor branch consumes via fabs().  The film slots are
			// sampled per-channel-agnostic (the substrate/film n,k use .v[0],
			// matching the spectral path's single-scalar reads).
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const ScalarTriple iorT  = pIOR->GetValuesAt(ri);
			const ScalarTriple extT  = pExtinction->GetValuesAt(ri);
			const ScalarTriple fIorT = pFilmIOR->GetValuesAt(ri);
			const ScalarTriple fExtT = pFilmExtinction->GetValuesAt(ri);
			const ScalarTriple fThkT = pFilmThickness->GetValuesAt(ri);
			const RISEPel Rfilm = ThinFilm::ReflectanceConductorRGB(
				cosWoH,
				1.0, 0.0,
				fIorT.v[0], fExtT.v[0],
				fThkT.v[0],
				iorT.v[0], extT.v[0] );
			specular = specColor * Rfilm * specFactor;
		}
		else
		{
			const ScalarTriple iorT = pIOR->GetValuesAt(ri);
			const ScalarTriple extT = pExtinction->GetValuesAt(ri);
			const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
			const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
			const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
				ri.ray.Dir(), h, RISEPel(1,1,1), ior, ext );
			specular = specColor * fresnel * specFactor;
		}
	}

	// Kulla-Conty multiscattering energy compensation
	// Use effective alpha = sqrt(alphaX * alphaY) for isotropic LUT lookup
	const Scalar alphaEff = sqrt( alphaX * alphaY );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );

	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alphaEff );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alphaEff );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		if( fresnelMode == eFresnelSchlickF0 )
		{
			// Closed-form Schlick hemispherical average.  specColor is F0.
			const RISEPel F_avg = SchlickFresnelAvg<RISEPel>( specColor );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			// Conductor AND thin-film: bare-SUBSTRATE hemispherical Fresnel
			// average for the Kulla-Conty multiscatter tail (the §7 P2-D
			// starting point; see GGXBRDF::valueNM for the rationale).
			const ScalarTriple iorT = pIOR->GetValuesAt(ri);
			const ScalarTriple extT = pExtinction->GetValuesAt(ri);
			const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
			const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
			const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	// Diffuse lobe.  In Schlick mode, modulate by (1 - max(F0)) per glTF spec
	// to enforce the (1-F) energy split between diffuse and specular.
	RISEPel diffuse = pDiffuse->GetColor(ri) * INV_PI;
	if( fresnelMode == eFresnelSchlickF0 )
	{
		const Scalar maxF0 = ColorMath::MaxValue( specColor );
		diffuse = diffuse * r_max( Scalar(0), Scalar(1.0) - maxF0 );
	}

	return diffuse + specular;
}

Scalar GGXBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Landing 8: rotate the tangent frame per anisotropy_rotation.
	const OrthonormalBasis3D effOnb = ResolveTangentONB( ri.onb, pTangentRotation, ri );
	const Vector3 n = effOnb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return 0;
	}

	const Scalar alphaX = r_max( pAlphaX->GetValueAtNM(ri,nm), Scalar(1e-4) );
	const Scalar alphaY = r_max( pAlphaY->GetValueAtNM(ri,nm), Scalar(1e-4) );

	const Vector3 h = Vector3Ops::Normalize( v + r );
	const Vector3 h_local(
		Vector3Ops::Dot( h, effOnb.u() ),
		Vector3Ops::Dot( h, effOnb.v() ),
		Vector3Ops::Dot( h, effOnb.w() )
	);
	const Vector3 wi_local(
		Vector3Ops::Dot( v, effOnb.u() ),
		Vector3Ops::Dot( v, effOnb.v() ),
		Vector3Ops::Dot( v, effOnb.w() )
	);
	const Vector3 wo_local(
		Vector3Ops::Dot( r, effOnb.u() ),
		Vector3Ops::Dot( r, effOnb.v() ),
		Vector3Ops::Dot( r, effOnb.w() )
	);

	const Scalar D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alphaX, alphaY, h_local );
	const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );
	const Scalar specFactor = D * G2 / (4.0 * nv * nr);

	const Scalar specColor = pSpecular->GetColorNM(ri,nm);

	Scalar specular = 0;

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector)
		if( fresnelMode == eFresnelSchlickF0 )
		{
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( specColor, cosWoH );
			if( F > 0 ) {
				specular = F * specFactor;
			}
		}
		else if( fresnelMode == eFresnelThinFilmConductor )
		{
			// Thin-film interference at the hero wavelength.  This is the
			// HWSS companion path for GGXSPF::ScatterNM (GGXSPF does NOT
			// override EvaluateKrayNM, so companion wavelengths route here)
			// — the thin-film term MUST be computed identically to
			// ScatterNM (docs/THIN_FILM_INTERFERENCE.md §7; the RGB/NM-twin
			// hazard, docs/skills/audit-by-bug-pattern.md).  cosThetaI is the
			// half-vector cosine dot(r,h) == |dot(ri.ray.Dir(),h)|, the SAME
			// cosine the conductor branch's CalculateConductorReflectance
			// consumes via fabs().
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const Scalar Rfilm = ThinFilm::ReflectanceConductor(
				cosWoH, nm,
				1.0, 0.0,
				pFilmIOR->GetValueAtNM(ri,nm), pFilmExtinction->GetValueAtNM(ri,nm),
				pFilmThickness->GetValueAtNM(ri,nm),
				pIOR->GetValueAtNM(ri,nm), pExtinction->GetValueAtNM(ri,nm) );
			if( Rfilm > 0 ) {
				specular = specColor * Rfilm * specFactor;
			}
		}
		else
		{
			const Scalar iorVal = pIOR->GetValueAtNM(ri,nm);
			const Scalar extVal = pExtinction->GetValueAtNM(ri,nm);
			const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), h, 1.0, iorVal, extVal );
			if( fresnel > 0 ) {
				specular = specColor * fresnel * specFactor;
			}
		}
	}

	// Kulla-Conty multiscattering
	const Scalar alphaEff = sqrt( alphaX * alphaY );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );

	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alphaEff );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alphaEff );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		if( fresnelMode == eFresnelSchlickF0 )
		{
			const Scalar F_avg = SchlickFresnelAvg<Scalar>( specColor );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			// Conductor AND thin-film share this multiscatter tail: the
			// Kulla-Conty energy-compensation term uses the bare-SUBSTRATE
			// hemispherical Fresnel average.  For thin-film this is the
			// documented P2-D starting point (§7 "measure the white-furnace
			// error of reusing the bare-substrate F_avg first; if material,
			// add a thin-film-aware F_avg") — the interference modifies the
			// single-scatter Fresnel above, while the multiscatter tail (a
			// small high-roughness correction) keeps the substrate average.
			const Scalar iorVal = pIOR->GetValueAtNM(ri,nm);
			const Scalar extVal = pExtinction->GetValueAtNM(ri,nm);
			const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	Scalar diffuse = pDiffuse->GetColorNM(ri,nm) * INV_PI;
	if( fresnelMode == eFresnelSchlickF0 )
	{
		// In NM, F0 is scalar; same (1-F0) split applies.
		diffuse = diffuse * r_max( Scalar(0), Scalar(1.0) - specColor );
	}

	return diffuse + specular;
}

RISEPel GGXBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Diffuse + Fresnel(cos θo) · spec, matching whichever Fresnel model
	// `value()` uses.  D / G microfacet terms shape the lobe but don't
	// change total integrated reflectance to first order.
	//
	// The Schlick branch evaluates Fresnel at the actual outgoing-cosine,
	// matching the conductor branch's intent.  Diffuse gets the
	// (1 - max(F0)) glTF-spec split.  Earlier revisions used the
	// hemispherical Schlick average here; review found it
	// overestimates near-normal and underestimates at grazing.
	const Vector3 n = ri.onb.w();
	const RISEPel specColor = pSpecular->GetColor( ri );
	const RISEPel diffColor = pDiffuse->GetColor( ri );

	if( fresnelMode == eFresnelSchlickF0 )
	{
		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar cosThetaO = r_max( Scalar(0), Vector3Ops::Dot( wo, n ) );
		const RISEPel F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( specColor, cosThetaO );
		const Scalar maxF0 = ColorMath::MaxValue( specColor );
		return diffColor * r_max( Scalar(0), Scalar(1.0) - maxF0 ) + F;
	}
	else if( fresnelMode == eFresnelThinFilmConductor )
	{
		// OIDN AOV: a representative thin-film RGB albedo (albedo-basis
		// reflectance at the outgoing-cosine), NOT the bare-conductor
		// Fresnel — so the denoiser's guide buffer carries the actual
		// iridescent tint.  Uses the geometric-normal cosine here (this is
		// a cheap per-hit guide, not a shading evaluation), matching the
		// conductor branch which feeds CalculateConductorReflectance the
		// dot(ray.Dir(), n) cosine.
		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar cosThetaO = r_max( Scalar(0), Vector3Ops::Dot( wo, n ) );
		const ScalarTriple iorT  = pIOR->GetValuesAt( ri );
		const ScalarTriple extT  = pExtinction->GetValuesAt( ri );
		const ScalarTriple fIorT = pFilmIOR->GetValuesAt( ri );
		const ScalarTriple fExtT = pFilmExtinction->GetValuesAt( ri );
		const ScalarTriple fThkT = pFilmThickness->GetValuesAt( ri );
		const RISEPel Rfilm = ThinFilm::ReflectanceConductorRGB(
			cosThetaO,
			1.0, 0.0,
			fIorT.v[0], fExtT.v[0],
			fThkT.v[0],
			iorT.v[0], extT.v[0] );
		return diffColor + specColor * Rfilm;
	}
	else
	{
		const ScalarTriple iorT = pIOR->GetValuesAt( ri );
		const ScalarTriple extT = pExtinction->GetValuesAt( ri );
		const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
		const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
		const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
			ri.ray.Dir(), n, RISEPel( 1, 1, 1 ), ior, ext );
		return diffColor + specColor * fresnel;
	}
}
