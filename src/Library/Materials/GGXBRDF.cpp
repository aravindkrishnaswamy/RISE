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
	const IScalarPainter* film_thickness,
	const IScalarPainter* tangent_rotation_scalar
	) :
  pDiffuse( &diffuse ),
  pSpecular( &specular ),
  pAlphaX( &alphaX ),
  pAlphaY( &alphaY ),
  pIOR( &ior ),
  pExtinction( &ext ),
  fresnelMode( fresnel_mode ),
  pTangentRotation( tangent_rotation ),
  pTangentRotationScalar( tangent_rotation_scalar ),
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
	if( pTangentRotationScalar ) pTangentRotationScalar->addref();
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

RISEPel GGXInterfaceFresnel::Directional( const Scalar cosine ) const
{
	const RISEPel tint = specular.GetColor( ri );
	if( mode == eFresnelSchlickF0 ) {
		return Optics::CalculateFresnelReflectanceSchlick<RISEPel>( tint, cosine );
	}
	if( mode == eFresnelThinFilmConductor ) {
		auto stackAt = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
			n0 = ri.ambientIOR; k0 = 0;
			n1 = filmIOR->GetValueAtNM( ri, nm );
			k1 = filmExtinction ? filmExtinction->GetValueAtNM( ri, nm ) : Scalar(0);
			n2 = ior.GetValueAtNM( ri, nm ); k2 = extinction.GetValueAtNM( ri, nm );
		};
		const RISEPel preview = tint * ThinFilm::ReflectanceConductorRGBSpectral(
			cosine, filmThickness->GetValueAtNM( ri, Scalar(550) ), stackAt );
		// A passive spectrum can have an RGB preview component above one.
		// Interface transmission needs a reflectance in [0,1], unlike an
		// RGB radiance proxy. Project at this boundary, before complements;
		// do not clamp the resulting transported energy. NM stays physical.
		return RISEPel(
			r_min(Scalar(1), r_max(Scalar(0), preview[0])),
			r_min(Scalar(1), r_max(Scalar(0), preview[1])),
			r_min(Scalar(1), r_max(Scalar(0), preview[2])) );
	}
	const ScalarTriple eta = ior.GetValuesAt( ri );
	const ScalarTriple k = extinction.GetValuesAt( ri );
	const Vector3 direction( sqrt( r_max( Scalar(0), Scalar(1)-cosine*cosine ) ), 0, -cosine );
	return tint * Optics::CalculateConductorReflectance<RISEPel>( direction, Vector3(0,0,1),
		RISEPel(ri.ambientIOR), RISEPel(eta.v[0],eta.v[1],eta.v[2]), RISEPel(k.v[0],k.v[1],k.v[2]) );
}

Scalar GGXInterfaceFresnel::DirectionalNM( const Scalar cosine, const Scalar nm ) const
{
	const Scalar tint = GuardedGetColorNM( specular, ri, nm );
	if( mode == eFresnelSchlickF0 ) {
		return Optics::CalculateFresnelReflectanceSchlick<Scalar>( tint, cosine );
	}
	if( mode == eFresnelThinFilmConductor ) {
		return tint * ThinFilm::ReflectanceConductor( cosine, nm, ri.ambientIOR, Scalar(0),
			filmIOR->GetValueAtNM( ri, nm ), filmExtinction ? filmExtinction->GetValueAtNM( ri, nm ) : Scalar(0),
			filmThickness->GetValueAtNM( ri, nm ), ior.GetValueAtNM( ri, nm ), extinction.GetValueAtNM( ri, nm ) );
	}
	const Vector3 direction( sqrt( r_max( Scalar(0), Scalar(1)-cosine*cosine ) ), 0, -cosine );
	return tint * Optics::CalculateConductorReflectance<Scalar>( direction, Vector3(0,0,1),
		ri.ambientIOR, ior.GetValueAtNM( ri, nm ), extinction.GetValueAtNM( ri, nm ) );
}

RISEPel GGXInterfaceFresnel::Mean() const
{
	const RISEPel tint = specular.GetColor( ri );
	if( mode == eFresnelSchlickF0 ) return SchlickFresnelAvg<RISEPel>( tint );
	if( mode == eFresnelThinFilmConductor ) {
		// Integrate the same projected directional function. Projecting an
		// already averaged preview would define a different diffuse model.
		RISEPel mean(Scalar(0));
		for( int i=0; i<MicrofacetEnergyLUT::GL_N; ++i ) {
			const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
			mean = mean + Directional(mu) * (Scalar(2)*mu*MicrofacetEnergyLUT::GL_weights[i]);
		}
		return mean;
	}
	const ScalarTriple eta = ior.GetValuesAt( ri );
	const ScalarTriple k = extinction.GetValuesAt( ri );
	return tint * MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( Vector3(0,0,1),
		RISEPel(ri.ambientIOR), RISEPel(eta.v[0],eta.v[1],eta.v[2]), RISEPel(k.v[0],k.v[1],k.v[2]) );
}

Scalar GGXInterfaceFresnel::MeanNM( const Scalar nm ) const
{
	const Scalar tint = GuardedGetColorNM( specular, ri, nm );
	if( mode == eFresnelSchlickF0 ) return SchlickFresnelAvg<Scalar>( tint );
	if( mode == eFresnelThinFilmConductor ) {
		return tint * ThinFilm::FresnelAvgConductor( nm, ri.ambientIOR, Scalar(0),
			filmIOR->GetValueAtNM( ri, nm ), filmExtinction ? filmExtinction->GetValueAtNM( ri, nm ) : Scalar(0),
			filmThickness->GetValueAtNM( ri, nm ), ior.GetValueAtNM( ri, nm ), extinction.GetValueAtNM( ri, nm ) );
	}
	return tint * MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( Vector3(0,0,1), ri.ambientIOR,
		ior.GetValueAtNM( ri, nm ), extinction.GetValueAtNM( ri, nm ) );
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
	if( pTangentRotationScalar ) pTangentRotationScalar->release();
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

// Thin-film FILM slots.  Same release-old / addref-new discipline as
// SetIOR; safe_release is null-safe so it correctly handles the
// pre-edit state where film_extinction may have been null (transparent
// k=0 default).  addref BEFORE the release so a self-rebind (v aliasing
// the current binding) never drops the last reference mid-swap.
void GGXBRDF::SetFilmIOR( const IScalarPainter& v )        { v.addref(); safe_release( pFilmIOR );        pFilmIOR        = &v; }
void GGXBRDF::SetFilmExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pFilmExtinction ); pFilmExtinction = &v; }
void GGXBRDF::SetFilmThickness( const IScalarPainter& v )  { v.addref(); safe_release( pFilmThickness );  pFilmThickness  = &v; }

namespace
{
	// Landing 8: resolve the per-shading-point tangent ONB.  When the
	// material has a non-null rotation painter, sample it at this hit
	// and rotate the tangent frame around w by that angle.  When null
	// (every pre-L8 GGX site), returns ri.onb verbatim — bit-identical
	// to the pre-L8 path.
	// DL-16: `pRotationScalar` (the Scalar-pipe alias) is preferred over
	// `pRotation` (the legacy Color-pipe binding) when both are bound --
	// see GGXBRDF.h's `pTangentRotationScalar` doc comment.  Falls back to
	// `source` unchanged when neither is bound (every pre-L8 / pre-DL-16
	// GGX site), bit-identical to prior behaviour.
	inline RISE::OrthonormalBasis3D ResolveTangentONB(
		const RISE::OrthonormalBasis3D& source,
		const RISE::IPainter* pRotation,
		const RISE::IScalarPainter* pRotationScalar,
		const RISE::RayIntersectionGeometric& ri )
	{
		if( pRotationScalar ) {
			const RISE::Scalar angle = pRotationScalar->GetValuesAt( ri ).v[0];
			return RISE::MicrofacetUtils::RotateTangent( source, angle );
		}
		if( !pRotation ) return source;
		const RISE::Scalar angle = RISE::ColorMath::MaxValue( pRotation->GetColor( ri ) );
		return RISE::MicrofacetUtils::RotateTangent( source, angle );
	}
}

RISEPel GGXBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	// Flip to the ray-facing frame first, mirroring GGXSPF::Scatter's FlipW
	// (same condition), so value() agrees with Scatter()/Pdf() on back-face
	// hits.  Landing 8: apply anisotropy_rotation AFTER the flip -- same
	// order as GGXSPF::ApplyTangentRotation -- so a rotated tangent frame on
	// a back-face hit still matches the sampler's frame.  effOnb == ri.onb
	// when no rotation painter is set and the hit is front-face.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const OrthonormalBasis3D effOnb = ResolveTangentONB( myonb, pTangentRotation, pTangentRotationScalar, ri );
	const Vector3 n = effOnb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );         // light direction (toward light)
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );    // view direction (toward viewer)

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return RISEPel(0,0,0);
	}

	// Geometric-horizon gate (mirrors GGXSPF::Scatter's sampler-side gate):
	// a GlintModifier-tilted shading normal can validate light/view
	// directions that are still below the true geometric surface.  This is
	// a DEFENSIVE check (a valid exterior hit already satisfies it), not a
	// literal sampler-consistency one -- NEE's light direction isn't
	// sampler-drawn -- but it guards against the same tilt pathology.
	// (r is tautologically inside this gate: r = -ri.ray.Dir() and geomN is
	// anchored to ri.ray.Dir(), so Dot(r,geomN) > 0 always holds -- only v,
	// the light half, can actually reject.  See LambertianBRDF.cpp:57-60.)
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
		return RISEPel(0,0,0);
	}

	// Read roughness parameters, clamped to avoid division-by-zero in NDF
	Scalar alphaX = r_max( pAlphaX->GetValuesAt(ri).v[0], Scalar(1e-4) );
	Scalar alphaY = r_max( pAlphaY->GetValuesAt(ri).v[0], Scalar(1e-4) );

	// DL-62: widen by the same glossy-filter amount GGXSPF::Scatter/
	// ScatterNM/Pdf/PdfNM already apply to their sampling/density
	// roughness -- NEE evaluation and BSDF-sampled continuation must
	// agree on which surface roughness is being rendered at this hit.
	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}

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
			// Dispersion-correct RGB preview: the air/film/substrate complex
			// indices are sampled per integration wavelength via GetValueAtNM (a
			// file-based Ti/TiO2 stack varies n,k across the band; reading only the
			// .v[0] 555 nm representative dropped that dispersion).  thickness is
			// wavelength-independent (one representative read).  ThinFilm.h stays
			// painter-free -- this functor is the template boundary.
			const Scalar thickness = pFilmThickness->GetValueAtNM( ri, Scalar(550) );
			auto stackAt = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
				n0 = ri.ambientIOR; k0 = Scalar(0); // G6 ambient medium IOR (default 1.0 = air)
				n1 = pFilmIOR->GetValueAtNM( ri, nm );
				k1 = pFilmExtinction ? pFilmExtinction->GetValueAtNM( ri, nm ) : Scalar(0);
				n2 = pIOR->GetValueAtNM( ri, nm );
				k2 = pExtinction->GetValueAtNM( ri, nm );
			};
			const RISEPel Rfilm = ThinFilm::ReflectanceConductorRGBSpectral(
				cosWoH, thickness, stackAt );
			specular = specColor * Rfilm * specFactor;
		}
		else
		{
			const ScalarTriple iorT = pIOR->GetValuesAt(ri);
			const ScalarTriple extT = pExtinction->GetValuesAt(ri);
			const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
			const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
			// G6: ambient (incident) medium IOR from the hit context (default
			// 1.0 = air; the surrounding dielectric's n when the conductor is
			// buried, e.g. silver under enamel glass).  Consistent with
			// GGXSPF::Scatter so sampling and NEE eval agree (MIS-correct).
			const RISEPel niPel( ri.ambientIOR, ri.ambientIOR, ri.ambientIOR );
			const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
				ri.ray.Dir(), h, niPel, ior, ext );
			specular = specColor * fresnel * specFactor;
		}
	}

	// Kulla-Conty multiscattering energy compensation.  DL-63: uses the
	// height-correlated-G2 LUT (LookupEavgG2/LookupEssG2), calibrated to
	// the SAME Smith height-correlated G2 masking-shadowing model this
	// function's own single-scatter specFactor renders with above --
	// NOT LookupEavg/LookupEss, which are calibrated to the separable
	// G1(wi)*G1(wo) model CookTorranceBRDF renders with instead.
	// DL-77 (fixed): LookupEavgG2/LookupEssG2 are calibrated to an
	// ISOTROPIC Smith Lambda at alphaEff=sqrt(alphaX*alphaY), while the
	// single-scatter term above uses direction-dependent per-axis Lambda
	// (MicrofacetUtils::GGX_G2_Aniso) -- isotropizing under-compensated
	// strongly anisotropic configurations by 11-44% at F0=1 (e.g.
	// alphaX=.02/alphaY=1.0).  LookupEavgG2Aniso/LookupEssG2Aniso resolve
	// an additional anisotropy-RATIO table dimension and fall back to the
	// exact isotropic LookupEavgG2/LookupEssG2 when alphaX==alphaY (so
	// isotropic materials render byte-identically to before this fix).
	// See docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-77" and the
	// DL-77 ledger row for the azimuthal-averaging design and residual.
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );

	if( (1.0 - Eavg) > 1e-10 )
	{
		// DL-77 P2 (per-azimuth, not azimuth-averaged): Ess_o/Ess_i are the
		// ENERGY term (unlike the H6 sampler, which stays azimuth-averaged
		// -- see the LookupEssG2AnisoDirectional doc comment), so they use
		// the actual per-direction azimuth (wo_local/wi_local's x,y in the
		// SAME tangent frame alphaX/alphaY are defined in) rather than
		// LookupEssG2Aniso's averaged table.
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nr, wo_local.x, wo_local.y, alphaX, alphaY );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nv, wi_local.x, wi_local.y, alphaX, alphaY );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		if( fresnelMode == eFresnelSchlickF0 )
		{
			// Closed-form Schlick hemispherical average.  specColor is F0.
			const RISEPel F_avg = SchlickFresnelAvg<RISEPel>( specColor );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else if( fresnelMode == eFresnelThinFilmConductor )
		{
			// Thin-film multiscatter tail: the Kulla-Conty F_avg is the THIN-FILM
			// hemispherical average, not the substrate's -- the film shifts the
			// average reflectance by up to ~0.5 (~13% rough-surface albedo error
			// otherwise; tests/ThinFilmFurnaceTest.cpp, design doc section 7 P2-D).
			// Dispersion-correct per-lambda stack (twin of the single-scatter
			// site): film/substrate n,k sampled at each integration wavelength.
			const Scalar thickness = pFilmThickness->GetValueAtNM( ri, Scalar(550) );
			auto stackAt = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
				n0 = ri.ambientIOR; k0 = Scalar(0); // G6 ambient medium IOR (default 1.0 = air)
				n1 = pFilmIOR->GetValueAtNM( ri, nm );
				k1 = pFilmExtinction ? pFilmExtinction->GetValueAtNM( ri, nm ) : Scalar(0);
				n2 = pIOR->GetValueAtNM( ri, nm );
				k2 = pExtinction->GetValueAtNM( ri, nm );
			};
			const RISEPel F_avg = ThinFilm::FresnelAvgConductorRGBSpectral( thickness, stackAt );
			// specColor is INSIDE the average: the per-bounce reflectance of the
			// tinted lobe is specColor*F_avg and the tint compounds across bounces,
			// so the nonlinear Fms must see the tinted average (matches the
			// single-scatter lobe specColor*Rfilm).  Pulling specColor outside
			// over-brightens tinted (specColor<1) rough metals.
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			// Conductor mode: bare-SUBSTRATE hemispherical Fresnel average
			// for the Kulla-Conty multiscatter tail (the thin-film mode is
			// handled by the branch above, which uses a thin-film F_avg).
			const ScalarTriple iorT = pIOR->GetValuesAt(ri);
			const ScalarTriple extT = pExtinction->GetValuesAt(ri);
			const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
			const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
			const RISEPel niPel( ri.ambientIOR, ri.ambientIOR, ri.ambientIOR ); // G6 ambient IOR (default 1.0 = air)
			const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, niPel, ior, ext );
			// specColor INSIDE the average (tinted per-bounce reflectance
			// specColor*F_avg compounds across bounces; matches the single-scatter
			// lobe specColor*fresnel).  See the thin-film branch above.
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
	}

	// Diffuse interface transmission on entry and exit, shared with the
	// selected cosine lobe in GGXSPF. No diffuse recycling is added.
	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const RISEPel diffuse = pDiffuse->GetColor(ri) * INV_PI *
		GGXInterfaceFresnel::Transmission( interfaceFresnel.Directional(nv), interfaceFresnel.Directional(nr) );

	return diffuse + specular;
}

Scalar GGXBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Same ray-facing flip (before the tangent rotation) as value() above.
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const OrthonormalBasis3D effOnb = ResolveTangentONB( myonb, pTangentRotation, pTangentRotationScalar, ri );
	const Vector3 n = effOnb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return 0;
	}

	// Geometric-horizon gate (mirrors GGXSPF::ScatterNM's sampler-side
	// gate); see GGXBRDF::value for rationale.
	// r is tautologically inside the gate here too (see value()'s note).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
		return 0;
	}

	Scalar alphaX = r_max( pAlphaX->GetValueAtNM(ri,nm), Scalar(1e-4) );
	Scalar alphaY = r_max( pAlphaY->GetValueAtNM(ri,nm), Scalar(1e-4) );

	// DL-62: see the RGB value() path above -- keep in lockstep with
	// GGXSPF::ScatterNM/PdfNM's widening.
	if( ri.glossyFilterWidth > 0 ) {
		alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
		alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
	}

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

	const Scalar specColor = GuardedGetColorNM( *pSpecular, ri, nm );

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
				ri.ambientIOR, 0.0, // G6 ambient medium n(λ), k=0 (default 1.0 = air)
				pFilmIOR->GetValueAtNM(ri,nm), ( pFilmExtinction ? pFilmExtinction->GetValueAtNM(ri,nm) : Scalar(0) ),
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
			// G6: ambient medium IOR from the hit context (per-wavelength n(λ) in NM; default 1.0 = air).
			const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), h, ri.ambientIOR, iorVal, extVal );
			if( fresnel > 0 ) {
				specular = specColor * fresnel * specFactor;
			}
		}
	}

	// Kulla-Conty multiscattering.  DL-77 (fixed): see value()'s twin
	// comment above -- LookupEavgG2Aniso/LookupEssG2Aniso resolve the
	// anisotropy-ratio dimension the isotropized alphaEff lookup was
	// missing, and fall back to the exact isotropic lookup at
	// alphaX==alphaY.
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );

	if( (1.0 - Eavg) > 1e-10 )
	{
		// DL-77 P2 (per-azimuth energy term; see value()'s twin comment).
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nr, wo_local.x, wo_local.y, alphaX, alphaY );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( nv, wi_local.x, wi_local.y, alphaX, alphaY );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		if( fresnelMode == eFresnelSchlickF0 )
		{
			const Scalar F_avg = SchlickFresnelAvg<Scalar>( specColor );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else if( fresnelMode == eFresnelThinFilmConductor )
		{
			// Thin-film multiscatter tail at the hero wavelength -- the thin-film
			// hemispherical F_avg (not the substrate's).  Identical term to
			// GGXSPF::ScatterNM (the RGB/NM twin); see the RGB path above.
			const Scalar F_avg = ThinFilm::FresnelAvgConductor(
				nm, ri.ambientIOR, 0.0, // G6 ambient medium n(λ), k=0 (default 1.0 = air)
				pFilmIOR->GetValueAtNM(ri,nm), ( pFilmExtinction ? pFilmExtinction->GetValueAtNM(ri,nm) : Scalar(0) ),
				pFilmThickness->GetValueAtNM(ri,nm),
				pIOR->GetValueAtNM(ri,nm), pExtinction->GetValueAtNM(ri,nm) );
			// specColor INSIDE the average (per-bounce reflectance specColor*F_avg
			// compounds across bounces; matches single-scatter specColor*Rfilm).
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			// Conductor mode: the Kulla-Conty energy-compensation term uses
			// the bare-SUBSTRATE hemispherical Fresnel average.  (The thin-film
			// mode is handled by the branch above, which feeds a thin-film
			// hemispherical F_avg into the same multiscatter tail.)
			const Scalar iorVal = pIOR->GetValueAtNM(ri,nm);
			const Scalar extVal = pExtinction->GetValueAtNM(ri,nm);
			// G6: ambient medium IOR from the hit context (per-wavelength n(λ) in NM; default 1.0 = air).
			const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, ri.ambientIOR, iorVal, extVal );
			// specColor INSIDE the average (tinted per-bounce reflectance
			// specColor*F_avg compounds; matches single-scatter specColor*fresnel).
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
	}

	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const Scalar diffuse = GuardedGetColorNM( *pDiffuse, ri, nm ) * INV_PI *
		GGXInterfaceFresnel::Transmission( interfaceFresnel.DirectionalNM(nv,nm), interfaceFresnel.DirectionalNM(nr,nm) );

	return diffuse + specular;
}

// Albedo guides retain a macro-interface approximation for specular energy.
// Unlike the diffuse integrals below, this is not the integrated rough-GGX
// lobe. The exact diffuse integral is c*(1-Ao)*(1-Amean), with no recycling.
RISEPel GGXBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const Scalar cosine = fabs( Vector3Ops::Dot( Vector3Ops::Normalize(-ri.ray.Dir()), ri.onb.w() ) );
	const RISEPel outgoing = interfaceFresnel.Directional( cosine );
	return outgoing + pDiffuse->GetColor(ri) * GGXInterfaceFresnel::Transmission( outgoing, interfaceFresnel.Mean() );
}

//////////////////////////////////////////////////////////////////////
// hemisphericalAlbedo{,NM} -- FIXED 2026-09-17 (DL-123).  Before this
// fix, the specular term was just `interfaceFresnel.Mean()` -- the
// FLAT macro-interface Fresnel hemispherical average, with NO
// dependence on alpha (roughness) at all.  That is only correct in the
// alpha->0 (mirror) limit; at rough/grazing configurations it ignores
// both (a) the single-scatter lobe's actual half-vector-weighted
// Fresnel shape and (b) the Kulla-Conty multiscatter energy the same
// `value()` function adds back in.  Measured (FabricMaterialChunkTest
// gate 5(b), GGX alpha=0.5, F0=0.04): +7.73% high.
//
// DERIVATION.  Write value()'s specular contribution as three additive
// bihemispherical pieces (each derived by integrating the ACTUAL code
// in value()/valueNM against IBSDF's uniform-incident-field measure,
// (1/pi) INT INT (.) (n.wi)(n.wo) dwi dwo):
//
//   1. SINGLE-SCATTER, R_ss.  specFactor's 1/(4*cosI*cosO) cancels the
//      measure's (n.wi)(n.wo) exactly (same cancellation
//      OrenNayarHemisphericalAlbedoGen.cpp's header derives for its own
//      BRDF), leaving R_ss = (1/pi) INT INT F(cosThetaH)*D(h)*G2/4
//      dwi dwo -- a 1-D integral in u=1-cosThetaH against a fixed
//      (Fresnel-independent) GGX kernel.  Evaluated via
//      GGXSpecularSingleScatterBihemispherical{,NM} below: a small
//      moment-matched fixed-node quadrature (tools/
//      GGXSpecularBihemisphericalGen.cpp), EXACT for Schlick (whose
//      F(u)=F0+(1-F0)*u^5 is degree 5, inside the baked degree-7 basis)
//      and a degree-7-polynomial-fit-quality approximation for
//      conductor/thin-film (measured in
//      tests/GGXHemisphericalAlbedoTest.cpp).
//
//   2. MULTISCATTER, R_ms.  f_ms = (1-Ess_o)(1-Ess_i)/(pi*(1-Eavg)) is
//      SEPARABLE in (wi,wo) and F_ms is a bihemispherical constant, so
//      INT(1-Ess_o)cosO dwo = INT(1-Ess_i)cosI dwi = pi*(1-Eavg)
//      (E_avg's own definition), giving, EXACTLY,
//        R_ms = (F_ms/pi) * (1/(pi(1-Eavg))) * [pi(1-Eavg)]^2
//             = F_ms * (1-Eavg)
//      with F_ms and Eavg computed by literally reusing the same
//      per-mode helper calls value()/valueNM use (SchlickFresnelAvg /
//      ThinFilm::FresnelAvgConductor{,RGBSpectral} /
//      MicrofacetEnergyLUT::ComputeFresnelAvg, then ComputeFms), so
//      this term cannot drift from value()'s own multiscatter tint --
//      no new baking needed.
//
//   3. DIFFUSE, R_diff.  Already exact pre-fix: diffuse = c*INV_PI*
//      (1-A(nv))(1-A(nr)) is separable, so R_diff = c*(1-Ā)^2 with
//      Ā = Mean() the cosine-weighted average of the SAME flat
//      Directional() function value()'s diffuse term itself uses --
//      unchanged.
//
// R_bi = R_ss + R_ms + R_diff.  At alpha->0, R_ss->Mean() and
// R_ms->F_ms*(1-Eavg)->0 (Eavg->~1), recovering the pre-fix mirror-limit
// answer exactly -- see the generator's own header for the smooth-limit
// check.
//
// ANISOTROPIC (alphaX != alphaY): R_ms/Eavg use the EXACT anisotropic
// LookupEavgG2Aniso (no approximation).  R_ss's baked quadrature table
// is ISOTROPIC-only; anisotropic configurations evaluate it at
// alphaEff=sqrt(alphaX*alphaY), an approximation for this term alone
// (residual tracked as DL-139).
//
// DL-62-style consistency: alphaX/alphaY are widened by
// ri.glossyFilterWidth before use, matching value()'s own widening (ri
// is passed for painter/filter-state sampling; only ri.ray is
// off-limits per IBSDF.h's contract).
//////////////////////////////////////////////////////////////////////

namespace
{
	// Baked by tools/GGXSpecularBihemisphericalGen.cpp -- see that
	// file's header for the full derivation (moment-matched
	// Chebyshev-node quadrature via VNDF importance sampling) and
	// GGXBRDF::hemisphericalAlbedo's own comment above for how it is
	// used.  DO NOT HAND-EDIT; regenerate and paste both arrays back
	// in.  Alpha axis: uniform 32 nodes on [0.01, 1.0], IDENTICAL to
	// MicrofacetEnergyLUT.h's E_avg_TABLE_G2 (cross-checked against it
	// in tests/GGXHemisphericalAlbedoTest.cpp: this table's own moment
	// 0, baked independently via a different outer quadrature and RNG
	// stream, agrees with E_avg_TABLE_G2 to within 3.4e-4 absolute
	// across the whole alpha range).
	static const int kGGXSpecularQuadNumNodes = 8;

	// mu_i = 1 - u_i (cosThetaH nodes, normal incidence first).
	static const Scalar kGGXSpecularQuadNodes[ kGGXSpecularQuadNumNodes ] =
	{
		1.0000000000f, 0.9504844340f, 0.8117449009f, 0.6112604670f, 0.3887395330f, 0.1882550991f, 0.0495155660f, 0.0000000000f
	};

	static const Scalar kGGXSpecularQuadWeight[ 32 ][ kGGXSpecularQuadNumNodes ] =
	{
		{ 0.0196541649f, 0.1813501614f, 0.2865772823f, 0.2665871764f, 0.1707216309f, 0.0653173630f, 0.0098298467f, -0.0005527483f },
		{ 0.0198812255f, 0.1821196328f, 0.2878830284f, 0.2668099854f, 0.1697370821f, 0.0615999069f, 0.0059251049f, -0.0003923701f },
		{ 0.0203420868f, 0.1834109159f, 0.2896509179f, 0.2665506395f, 0.1662765785f, 0.0541123674f, 0.0026858645f, 0.0000523506f },
		{ 0.0207780070f, 0.1849814439f, 0.2915703171f, 0.2651772095f, 0.1599165665f, 0.0456795501f, 0.0009564703f, 0.0002725022f },
		{ 0.0210642521f, 0.1871821400f, 0.2927103285f, 0.2626750863f, 0.1513955413f, 0.0374206411f, 0.0004370595f, 0.0002051954f },
		{ 0.0216166122f, 0.1892582074f, 0.2934961277f, 0.2582620788f, 0.1410839518f, 0.0305963259f, 0.0002617362f, 0.0001194596f },
		{ 0.0220497026f, 0.1908699318f, 0.2934435979f, 0.2525914482f, 0.1306392682f, 0.0248009793f, 0.0003793991f, -0.0000295460f },
		{ 0.0224127630f, 0.1925976830f, 0.2926659555f, 0.2454808466f, 0.1198302541f, 0.0201833458f, 0.0005607280f, -0.0001628171f },
		{ 0.0229108259f, 0.1937911225f, 0.2909857175f, 0.2371833394f, 0.1095734037f, 0.0165882310f, 0.0006521583f, -0.0002341716f },
		{ 0.0231268118f, 0.1949557328f, 0.2885365644f, 0.2280847291f, 0.0998819078f, 0.0137183263f, 0.0007813893f, -0.0003119953f },
		{ 0.0235479159f, 0.1956260678f, 0.2853307520f, 0.2181993330f, 0.0909507324f, 0.0114857777f, 0.0008110046f, -0.0003380688f },
		{ 0.0238546334f, 0.1958132327f, 0.2807709886f, 0.2085662438f, 0.0826899082f, 0.0097636815f, 0.0007931144f, -0.0003367094f },
		{ 0.0241565262f, 0.1954676230f, 0.2758756780f, 0.1986794714f, 0.0753298006f, 0.0082858043f, 0.0007791765f, -0.0003357670f },
		{ 0.0244737470f, 0.1950198039f, 0.2703928779f, 0.1887433996f, 0.0685937860f, 0.0071582370f, 0.0007514508f, -0.0003259591f },
		{ 0.0244020877f, 0.1943562578f, 0.2646694334f, 0.1788774758f, 0.0628176346f, 0.0061353853f, 0.0007891450f, -0.0003469105f },
		{ 0.0245753324f, 0.1927326899f, 0.2581117388f, 0.1696008046f, 0.0573862958f, 0.0053877213f, 0.0007455236f, -0.0003281032f },
		{ 0.0246630472f, 0.1911149386f, 0.2514846954f, 0.1603106649f, 0.0526103606f, 0.0047070494f, 0.0007307320f, -0.0003240894f },
		{ 0.0248395799f, 0.1889324685f, 0.2444189167f, 0.1519660389f, 0.0483947829f, 0.0041985699f, 0.0006825629f, -0.0003036041f },
		{ 0.0244885548f, 0.1868890657f, 0.2374057468f, 0.1434092999f, 0.0446433274f, 0.0036346882f, 0.0007242749f, -0.0003247489f },
		{ 0.0245196274f, 0.1841329441f, 0.2304369107f, 0.1357190128f, 0.0410867612f, 0.0032857907f, 0.0006652733f, -0.0002994006f },
		{ 0.0245429772f, 0.1815375049f, 0.2230444445f, 0.1283645104f, 0.0379915365f, 0.0029235517f, 0.0006448835f, -0.0002909576f },
		{ 0.0242403979f, 0.1786017986f, 0.2156303534f, 0.1213946246f, 0.0353641168f, 0.0025986528f, 0.0006692498f, -0.0003032994f },
		{ 0.0241995832f, 0.1754544909f, 0.2086202976f, 0.1147708672f, 0.0327812349f, 0.0023500076f, 0.0006332554f, -0.0002881913f },
		{ 0.0239808702f, 0.1724420007f, 0.2017848756f, 0.1085479206f, 0.0305927121f, 0.0020956109f, 0.0006367138f, -0.0002908806f },
		{ 0.0236858255f, 0.1688770687f, 0.1948642871f, 0.1029545162f, 0.0285387326f, 0.0018835180f, 0.0006267110f, -0.0002868409f },
		{ 0.0235063483f, 0.1657838497f, 0.1879353015f, 0.0973225645f, 0.0267071793f, 0.0016845645f, 0.0006263645f, -0.0002876463f },
		{ 0.0233191435f, 0.1621149752f, 0.1816051672f, 0.0923893144f, 0.0249589050f, 0.0015242993f, 0.0006099001f, -0.0002808021f },
		{ 0.0230416412f, 0.1586400294f, 0.1750429675f, 0.0876247732f, 0.0234479438f, 0.0013884758f, 0.0005988544f, -0.0002760458f },
		{ 0.0226143444f, 0.1550961624f, 0.1690736902f, 0.0831277074f, 0.0220714843f, 0.0012527631f, 0.0005989019f, -0.0002772788f },
		{ 0.0223766810f, 0.1517260270f, 0.1628850559f, 0.0789570452f, 0.0207734923f, 0.0011194194f, 0.0006061724f, -0.0002806027f },
		{ 0.0220877116f, 0.1480205729f, 0.1573698270f, 0.0750250719f, 0.0196675106f, 0.0010063039f, 0.0006026792f, -0.0002800912f },
		{ 0.0217673239f, 0.1445616620f, 0.1514399619f, 0.0713737717f, 0.0185561643f, 0.0009202578f, 0.0005866106f, -0.0002724636f },
	};

	//! Linear interpolation across the alpha axis (same [0.01,1.0]
	//! uniform-32-node mapping as MicrofacetEnergyLUT::LookupEavgG2),
	//! returning the weight for quadrature node `nodeIdx`.
	inline Scalar LookupGGXSpecularQuadWeight( const int nodeIdx, const Scalar alpha )
	{
		const int kNumAlphaBins = 32;
		Scalar a = r_max( Scalar(0), r_min( Scalar(1), (alpha - Scalar(0.01)) / Scalar(0.99) ) ) * Scalar(kNumAlphaBins - 1);
		int ai0 = (int)a;
		int ai1 = r_min( ai0 + 1, kNumAlphaBins - 1 );
		Scalar af = a - Scalar(ai0);
		return kGGXSpecularQuadWeight[ai0][nodeIdx] * (Scalar(1) - af) + kGGXSpecularQuadWeight[ai1][nodeIdx] * af;
	}

	//! R_ss(alpha) = SUM_i F(mu_i) * w_i(alpha) -- see the long
	//! derivation comment above GGXBRDF::hemisphericalAlbedo.  `F` is
	//! whatever Fresnel function `interfaceFresnel.Directional` (RGB) /
	//! `DirectionalNM` (NM) dispatches to for the material's actual
	//! FresnelMode -- this helper is mode-agnostic.
	RISEPel GGXSpecularSingleScatterBihemispherical( const GGXInterfaceFresnel& interfaceFresnel, const Scalar alpha )
	{
		RISEPel sum(0,0,0);
		for( int i = 0; i < kGGXSpecularQuadNumNodes; i++ ) {
			sum = sum + interfaceFresnel.Directional( kGGXSpecularQuadNodes[i] ) * LookupGGXSpecularQuadWeight( i, alpha );
		}
		return sum;
	}
	Scalar GGXSpecularSingleScatterBihemisphericalNM( const GGXInterfaceFresnel& interfaceFresnel, const Scalar alpha, const Scalar nm )
	{
		Scalar sum = 0;
		for( int i = 0; i < kGGXSpecularQuadNumNodes; i++ ) {
			sum += interfaceFresnel.DirectionalNM( kGGXSpecularQuadNodes[i], nm ) * LookupGGXSpecularQuadWeight( i, alpha );
		}
		return sum;
	}

	//! F_ms(Eavg) computed by literally replaying value()'s own
	//! per-mode F_avg construction (see value()'s "Kulla-Conty
	//! multiscattering" comment block) so this can never drift from
	//! what value() itself renders with.  `n` is the ComputeFresnelAvg
	//! reference normal used only to build an arbitrary tangent frame
	//! for its internal quadrature -- the RESULT is rotationally
	//! invariant (Fresnel depends only on the angle to `n`), so passing
	//! a fixed axis here (as GGXInterfaceFresnel::Mean() already does)
	//! is bit-for-bit equivalent to passing the real shading normal.
	RISEPel ComputeGGXFms(
		const RayIntersectionGeometric& ri, const FresnelMode mode, const RISEPel& specColor,
		const IScalarPainter& iorP, const IScalarPainter& extP,
		const IScalarPainter* filmIOR, const IScalarPainter* filmExt, const IScalarPainter* filmThick,
		const Scalar Eavg )
	{
		if( mode == eFresnelSchlickF0 ) {
			const RISEPel F_avg = SchlickFresnelAvg<RISEPel>( specColor );
			return MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
		}
		if( mode == eFresnelThinFilmConductor ) {
			const Scalar thickness = filmThick->GetValueAtNM( ri, Scalar(550) );
			auto stackAt = [&]( Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2 ) {
				n0 = ri.ambientIOR; k0 = Scalar(0);
				n1 = filmIOR->GetValueAtNM( ri, nm );
				k1 = filmExt ? filmExt->GetValueAtNM( ri, nm ) : Scalar(0);
				n2 = iorP.GetValueAtNM( ri, nm ); k2 = extP.GetValueAtNM( ri, nm );
			};
			const RISEPel F_avg = ThinFilm::FresnelAvgConductorRGBSpectral( thickness, stackAt );
			return MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * F_avg, Eavg );
		}
		const ScalarTriple iorT = iorP.GetValuesAt( ri );
		const ScalarTriple extT = extP.GetValuesAt( ri );
		const RISEPel ior( iorT.v[0], iorT.v[1], iorT.v[2] );
		const RISEPel ext( extT.v[0], extT.v[1], extT.v[2] );
		const RISEPel niPel( ri.ambientIOR, ri.ambientIOR, ri.ambientIOR );
		const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( Vector3(0,0,1), niPel, ior, ext );
		return MicrofacetEnergyLUT::ComputeFms<RISEPel>( specColor * F_avg, Eavg );
	}
	Scalar ComputeGGXFmsNM(
		const RayIntersectionGeometric& ri, const FresnelMode mode, const Scalar specColor,
		const IScalarPainter& iorP, const IScalarPainter& extP,
		const IScalarPainter* filmIOR, const IScalarPainter* filmExt, const IScalarPainter* filmThick,
		const Scalar nm, const Scalar Eavg )
	{
		if( mode == eFresnelSchlickF0 ) {
			const Scalar F_avg = SchlickFresnelAvg<Scalar>( specColor );
			return MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
		}
		if( mode == eFresnelThinFilmConductor ) {
			const Scalar F_avg = ThinFilm::FresnelAvgConductor(
				nm, ri.ambientIOR, Scalar(0),
				filmIOR->GetValueAtNM( ri, nm ), filmExt ? filmExt->GetValueAtNM( ri, nm ) : Scalar(0),
				filmThick->GetValueAtNM( ri, nm ),
				iorP.GetValueAtNM( ri, nm ), extP.GetValueAtNM( ri, nm ) );
			return MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * F_avg, Eavg );
		}
		const Scalar iorVal = iorP.GetValueAtNM( ri, nm );
		const Scalar extVal = extP.GetValueAtNM( ri, nm );
		const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( Vector3(0,0,1), ri.ambientIOR, iorVal, extVal );
		return MicrofacetEnergyLUT::ComputeFms<Scalar>( specColor * F_avg, Eavg );
	}

	//! Resolve alphaX/alphaY (DL-62-style glossy-filter widening + the
	//! 1e-4 floor GGX_D_Aniso/GGX_Lambda_Aniso apply internally), plus
	//! alphaEff=sqrt(alphaX*alphaY) for the (isotropic-only) R_ss table
	//! -- see the DL-123 comment's "ANISOTROPIC" paragraph.  The REAL
	//! alphaX/alphaY (not alphaEff) must be used for the anisotropic
	//! Eavg lookup (R_ms), which needs no approximation.
	void ResolveGGXHemisphericalAlphas( const IScalarPainter& alphaXP, const IScalarPainter& alphaYP,
		const RayIntersectionGeometric& ri, Scalar& outAlphaX, Scalar& outAlphaY, Scalar& outAlphaEff )
	{
		Scalar alphaX = alphaXP.GetValuesAt(ri).v[0];
		Scalar alphaY = alphaYP.GetValuesAt(ri).v[0];
		if( ri.glossyFilterWidth > 0 ) {
			alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
			alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
		}
		outAlphaX = r_max( alphaX, Scalar(1e-4) );
		outAlphaY = r_max( alphaY, Scalar(1e-4) );
		outAlphaEff = sqrt( outAlphaX * outAlphaY );
	}
	void ResolveGGXHemisphericalAlphasNM( const IScalarPainter& alphaXP, const IScalarPainter& alphaYP,
		const RayIntersectionGeometric& ri, const Scalar nm, Scalar& outAlphaX, Scalar& outAlphaY, Scalar& outAlphaEff )
	{
		Scalar alphaX = alphaXP.GetValueAtNM(ri,nm);
		Scalar alphaY = alphaYP.GetValueAtNM(ri,nm);
		if( ri.glossyFilterWidth > 0 ) {
			alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
			alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
		}
		outAlphaX = r_max( alphaX, Scalar(1e-4) );
		outAlphaY = r_max( alphaY, Scalar(1e-4) );
		outAlphaEff = sqrt( outAlphaX * outAlphaY );
	}
}

bool GGXBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const RISEPel mean = interfaceFresnel.Mean();

	Scalar alphaX, alphaY, alphaEff;
	ResolveGGXHemisphericalAlphas( *pAlphaX, *pAlphaY, ri, alphaX, alphaY, alphaEff );
	const Scalar EavgAniso = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );
	const RISEPel R_ss = GGXSpecularSingleScatterBihemispherical( interfaceFresnel, alphaEff );
	const RISEPel specColor = pSpecular->GetColor(ri);
	const RISEPel F_ms = ComputeGGXFms( ri, fresnelMode, specColor, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness, EavgAniso );
	const RISEPel R_ms = F_ms * (Scalar(1) - EavgAniso);

	out = R_ss + R_ms + pDiffuse->GetColor(ri) * GGXInterfaceFresnel::Transmission( mean, mean );
	return true;
}

bool GGXBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const Scalar mean = interfaceFresnel.MeanNM( nm );

	Scalar alphaX, alphaY, alphaEff;
	ResolveGGXHemisphericalAlphasNM( *pAlphaX, *pAlphaY, ri, nm, alphaX, alphaY, alphaEff );
	const Scalar EavgAniso = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );
	const Scalar R_ss = GGXSpecularSingleScatterBihemisphericalNM( interfaceFresnel, alphaEff, nm );
	const Scalar specColor = GuardedGetColorNM( *pSpecular, ri, nm );
	const Scalar F_ms = ComputeGGXFmsNM( ri, fresnelMode, specColor, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness, nm, EavgAniso );
	const Scalar R_ms = F_ms * (Scalar(1) - EavgAniso);

	out = R_ss + R_ms + GuardedGetColorNM( *pDiffuse, ri, nm ) * GGXInterfaceFresnel::Transmission( mean, mean );
	return true;
}
