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
	// both roughness eigenvalues on their reachable grid and fall back to the
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
// ANISOTROPIC (alphaX != alphaY), DL-139: R_ss resolves both roughness
// eigenvalues on a 24x24 reachable grid plus DL-161-style low-alpha
// sub-grids; R_ms/Eavg uses the same constrained diagonal formulation
// over its pre-existing anisotropic data.  The bihemispherical quantity integrates a full
// white sky, so material-frame rotation is a change of variables and no
// phi axis is present.  The lookup decomposes every weight into the
// preserved isotropic curve at sqrt(alphaX*alphaY) plus an anisotropic
// correction.  That correction has an exact zero boundary on alphaX==alphaY
// and diagonal cells are triangulated along that boundary, so the unequal-
// axis limit equals the complete legacy isotropic interpolant, including at
// its own knots.  Exact equality still forwards to the original path before
// any new arithmetic (slot-by-slot identity retained).
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

	// DL-160: low-alpha sub-grid -- see LookupGGXSpecularQuadWeight's
	// own comment below for how these are consumed.  The exact alpha->0
	// boundary (idx<=0) is a CLOSED FORM (moments 2/((k+1)(k+2)) of the
	// mirror-limit kernel, discretized through the SAME numMuI-bin
	// midpoint rule every other row uses -- see
	// tools/GGXSpecularBihemisphericalGen.cpp's own derivation comment;
	// NOT a delta at node 0, which a naive first guess would suggest).
	// Row order: ALPHA_SUB_FINE=7 geometric octaves below 0.01
	// (alpha=0.01/2^(8-j)), then ALPHA_MID_SIZE-1=3 geometric nodes
	// bridging (0.01,0.04193548] -- IDENTICAL scheme to
	// MicrofacetEnergyLUT.h's own ALPHA_SUB_FINE/ALPHA_MID_SIZE
	// (DL-105); last row is an unused zero-padding slot
	// (MicrofacetEnergyLUT::AlphaLowSlot's own range only ever
	// addresses 9 of these 10 rows).  DO NOT HAND-EDIT; regenerate via
	// tools/GGXSpecularBihemisphericalGen.cpp and paste both arrays
	// back in verbatim.
	static const Scalar kGGXSpecularQuadWeightAlphaZero[ kGGXSpecularQuadNumNodes ] =
	{
		0.0196366999f, 0.1812715419f, 0.2864598270f, 0.2665231478f, 0.1707519982f, 0.0654906004f, 0.0102491160f, -0.0003829311f
	};

	static const Scalar kGGXSpecularQuadWeightAlphaLow[ 10 ][ kGGXSpecularQuadNumNodes ] =
	{
		{ 0.0196366957f, 0.1812717610f, 0.2864598542f, 0.2665229694f, 0.1707520275f, 0.0654905098f, 0.0102490686f, -0.0003829698f },	// slot 0, idx=1, alpha=0.00007813
		{ 0.0196368844f, 0.1812712062f, 0.2864596426f, 0.2665237506f, 0.1707518823f, 0.0654906372f, 0.0102488727f, -0.0003830577f },	// slot 1, idx=2, alpha=0.00015625
		{ 0.0196372950f, 0.1812709980f, 0.2864601309f, 0.2665243237f, 0.1707505219f, 0.0654908638f, 0.0102484391f, -0.0003833771f },	// slot 2, idx=3, alpha=0.00031250
		{ 0.0196370084f, 0.1812727192f, 0.2864589360f, 0.2665246245f, 0.1707517230f, 0.0654904877f, 0.0102471274f, -0.0003851709f },	// slot 3, idx=4, alpha=0.00062500
		{ 0.0196354520f, 0.1812754930f, 0.2864606924f, 0.2665252693f, 0.1707554320f, 0.0654855949f, 0.0102435009f, -0.0003920428f },	// slot 4, idx=5, alpha=0.00125000
		{ 0.0196369355f, 0.1812742989f, 0.2864802330f, 0.2665171320f, 0.1707626089f, 0.0654783444f, 0.0102219730f, -0.0004127991f },	// slot 5, idx=6, alpha=0.00250000
		{ 0.0196485605f, 0.1812898903f, 0.2864969597f, 0.2665383723f, 0.1707476189f, 0.0654594238f, 0.0101318461f, -0.0004621646f },	// slot 6, idx=7, alpha=0.00500000
		{ 0.0196716623f, 0.1813923454f, 0.2867367430f, 0.2666085826f, 0.1706584649f, 0.0651428744f, 0.0094035006f, -0.0005879527f },	// slot 7, idx=9, alpha=0.01431019
		{ 0.0197293578f, 0.1815247718f, 0.2868315902f, 0.2667236500f, 0.1705646109f, 0.0646660954f, 0.0087290608f, -0.0006161293f },	// slot 8, idx=10, alpha=0.02047816
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
	};

// DL-139: azimuth-averaged anisotropic single-scatter correction.
// Both ordinary alpha axes use MicrofacetEnergyLUT::ANISO_ALPHA_SIZE=24;
// low axes use its AnisoAlphaLowNode/AlphaLowSlot layout.  The virtual
// alpha->0 node folds to low slot 0 because production alpha is floored
// at 1e-4 and the off-diagonal limit has no isotropic closed form.
// Each value is W_aniso(alphaX,alphaY)-W_iso(sqrt(alphaX*alphaY));
// diagonal anchors are therefore exactly zero at their declared coordinates.
// Off-diagonal cells use the independent anisotropic stream.
// Independently seeded stream, numMuI=48, numSamples=100000. DO NOT HAND-EDIT.

static const Scalar kGGXSpecularQuadCorrectionAniso[ 24 ][ 24 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0002037146f, 0.0002828277f, 0.0006261674f, 0.0000746289f, -0.0006813407f, -0.0019763471f, -0.0016014557f, 0.0001574460f },
		{ 0.0003185647f, 0.0013265772f, 0.0015307830f, -0.0003208642f, -0.0031774439f, -0.0070285399f, -0.0031187503f, 0.0003782512f },
		{ 0.0007020877f, 0.0022860240f, 0.0026758883f, -0.0016363282f, -0.0085422647f, -0.0128796970f, -0.0036744443f, 0.0004432120f },
		{ 0.0008350851f, 0.0037622336f, 0.0033647040f, -0.0044021905f, -0.0154157812f, -0.0183665577f, -0.0036602486f, 0.0003819290f },
		{ 0.0013825909f, 0.0051816937f, 0.0031519880f, -0.0085383901f, -0.0231543515f, -0.0226375384f, -0.0035029954f, 0.0002677645f },
		{ 0.0014378527f, 0.0062397717f, 0.0022949746f, -0.0134935461f, -0.0310083588f, -0.0261374471f, -0.0032120969f, 0.0001384093f },
		{ 0.0017845265f, 0.0069656897f, 0.0008046119f, -0.0200350318f, -0.0388614192f, -0.0288121216f, -0.0029407713f, 0.0000496949f },
		{ 0.0018304860f, 0.0072381723f, -0.0015914016f, -0.0270120284f, -0.0464796721f, -0.0310331366f, -0.0026499474f, -0.0000350043f },
		{ 0.0022733086f, 0.0070736157f, -0.0050691935f, -0.0342107469f, -0.0536010464f, -0.0327522125f, -0.0023932619f, -0.0000927086f },
		{ 0.0024437550f, 0.0065261434f, -0.0085618643f, -0.0424814938f, -0.0602524126f, -0.0339976678f, -0.0021885765f, -0.0001247071f },
		{ 0.0023873233f, 0.0061943241f, -0.0134692178f, -0.0503087556f, -0.0660404033f, -0.0351961785f, -0.0018487842f, -0.0002133657f },
		{ 0.0025463896f, 0.0048266681f, -0.0184980891f, -0.0582412150f, -0.0717779577f, -0.0360857408f, -0.0015722280f, -0.0002719773f },
		{ 0.0023271270f, 0.0036031184f, -0.0237233083f, -0.0662586837f, -0.0767945983f, -0.0366893074f, -0.0014207599f, -0.0002952001f },
		{ 0.0025083272f, 0.0016622664f, -0.0296956467f, -0.0738794832f, -0.0811482788f, -0.0371542571f, -0.0013320680f, -0.0003080453f },
		{ 0.0023487289f, -0.0007138837f, -0.0358591535f, -0.0811046267f, -0.0849444089f, -0.0375564701f, -0.0011947264f, -0.0003461836f },
		{ 0.0022271697f, -0.0030945881f, -0.0417564043f, -0.0885237739f, -0.0887703695f, -0.0377272616f, -0.0011104448f, -0.0003523488f },
		{ 0.0020353457f, -0.0059177110f, -0.0483449073f, -0.0953803362f, -0.0920927873f, -0.0379614906f, -0.0009987074f, -0.0003765816f },
		{ 0.0018688146f, -0.0087004819f, -0.0546681157f, -0.1020049674f, -0.0951664878f, -0.0380609580f, -0.0009215210f, -0.0003802327f },
		{ 0.0017734278f, -0.0116276008f, -0.0613422481f, -0.1084158757f, -0.0981585052f, -0.0380510441f, -0.0008280833f, -0.0003893418f },
		{ 0.0011898017f, -0.0144774485f, -0.0676264092f, -0.1146499166f, -0.1005824271f, -0.0381954980f, -0.0006836257f, -0.0004282457f },
		{ 0.0010991171f, -0.0175465382f, -0.0742864581f, -0.1202213877f, -0.1031201910f, -0.0381723831f, -0.0005665603f, -0.0004521686f },
		{ 0.0008112423f, -0.0210330914f, -0.0805916817f, -0.1256739232f, -0.1052584223f, -0.0380745262f, -0.0004827899f, -0.0004598796f },
		{ 0.0006895790f, -0.0245675418f, -0.0866879651f, -0.1309124200f, -0.1073623181f, -0.0379507804f, -0.0004241319f, -0.0004561000f },
	},
	{
		{ 0.0002037146f, 0.0002828277f, 0.0006261674f, 0.0000746289f, -0.0006813407f, -0.0019763471f, -0.0016014557f, 0.0001574460f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000322563f, 0.0002448078f, 0.0004213379f, -0.0004324570f, -0.0008156723f, -0.0012117496f, -0.0002714748f, 0.0000418325f },
		{ 0.0002658231f, 0.0007600919f, 0.0004314132f, -0.0010356518f, -0.0039026174f, -0.0038911073f, -0.0003449344f, 0.0000025598f },
		{ 0.0003914972f, 0.0017331099f, 0.0002011310f, -0.0031079590f, -0.0085922595f, -0.0066032945f, -0.0000841922f, -0.0001198799f },
		{ 0.0006771303f, 0.0024025421f, 0.0000619749f, -0.0067138127f, -0.0144033109f, -0.0088686154f, 0.0001655023f, -0.0002097214f },
		{ 0.0008190035f, 0.0029804939f, -0.0008195875f, -0.0114668974f, -0.0199406489f, -0.0106304922f, 0.0002950372f, -0.0002679317f },
		{ 0.0011201319f, 0.0034986727f, -0.0030600958f, -0.0172683502f, -0.0254997636f, -0.0118599169f, 0.0005137632f, -0.0003471993f },
		{ 0.0011006637f, 0.0032675406f, -0.0051967234f, -0.0234166032f, -0.0312371186f, -0.0124964801f, 0.0006020596f, -0.0003499195f },
		{ 0.0011217467f, 0.0029558879f, -0.0083550438f, -0.0299568040f, -0.0361265267f, -0.0130398978f, 0.0005722321f, -0.0003342050f },
		{ 0.0011883082f, 0.0022511399f, -0.0119741655f, -0.0372191093f, -0.0405748398f, -0.0134748448f, 0.0006120625f, -0.0003368361f },
		{ 0.0012647439f, 0.0005014252f, -0.0168835460f, -0.0437842725f, -0.0444183490f, -0.0136923228f, 0.0006345007f, -0.0003370773f },
		{ 0.0012924316f, -0.0005498344f, -0.0221728734f, -0.0507357020f, -0.0479981863f, -0.0137243595f, 0.0006692616f, -0.0003385055f },
		{ 0.0012956457f, -0.0022274904f, -0.0275656032f, -0.0574678794f, -0.0512861651f, -0.0137089657f, 0.0006556195f, -0.0003204017f },
		{ 0.0009774727f, -0.0044559785f, -0.0331734549f, -0.0640632766f, -0.0541029207f, -0.0136920264f, 0.0005841752f, -0.0002856084f },
		{ 0.0010248911f, -0.0070546465f, -0.0389593451f, -0.0704689151f, -0.0564588798f, -0.0135308664f, 0.0005302316f, -0.0002542101f },
		{ 0.0008130451f, -0.0091048125f, -0.0448735570f, -0.0772078935f, -0.0587343530f, -0.0133786898f, 0.0005300630f, -0.0002481184f },
		{ 0.0005576664f, -0.0125184639f, -0.0512483573f, -0.0827877185f, -0.0607593257f, -0.0131306213f, 0.0004732146f, -0.0002147153f },
		{ 0.0005086968f, -0.0152521235f, -0.0577964361f, -0.0883433273f, -0.0623757232f, -0.0128709953f, 0.0004114787f, -0.0001821650f },
		{ 0.0000950055f, -0.0183074403f, -0.0638587993f, -0.0938273095f, -0.0635582653f, -0.0127702205f, 0.0003718157f, -0.0001646260f },
		{ -0.0000673004f, -0.0217228061f, -0.0702120301f, -0.0985750297f, -0.0648387044f, -0.0124892619f, 0.0002733885f, -0.0001178345f },
		{ -0.0005257023f, -0.0251313452f, -0.0756913477f, -0.1035662414f, -0.0660347138f, -0.0123029994f, 0.0002630118f, -0.0001094887f },
		{ -0.0008660915f, -0.0284699237f, -0.0822408334f, -0.1078761872f, -0.0668128903f, -0.0121226050f, 0.0002486008f, -0.0001029261f },
		{ -0.0009761984f, -0.0317511699f, -0.0884633079f, -0.1124705008f, -0.0676376150f, -0.0118533426f, 0.0001913354f, -0.0000744266f },
	},
	{
		{ 0.0003185647f, 0.0013265772f, 0.0015307830f, -0.0003208642f, -0.0031774439f, -0.0070285399f, -0.0031187503f, 0.0003782512f },
		{ -0.0000322563f, 0.0002448078f, 0.0004213379f, -0.0004324570f, -0.0008156723f, -0.0012117496f, -0.0002714748f, 0.0000418325f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0001304434f, 0.0000633145f, 0.0000239252f, -0.0002279153f, -0.0008324670f, -0.0007104318f, -0.0000852371f, 0.0000091731f },
		{ 0.0002671200f, 0.0005427710f, 0.0001346084f, -0.0019538796f, -0.0034855448f, -0.0018851315f, 0.0000209168f, -0.0000334204f },
		{ 0.0002374999f, 0.0011093951f, -0.0005585794f, -0.0044567326f, -0.0069657919f, -0.0031990936f, 0.0001960228f, -0.0001102941f },
		{ 0.0005085348f, 0.0011270412f, -0.0015382326f, -0.0079155046f, -0.0109644533f, -0.0041424699f, 0.0002458832f, -0.0001254965f },
		{ 0.0005545115f, 0.0009310939f, -0.0035508127f, -0.0122149932f, -0.0148514357f, -0.0047609927f, 0.0003452028f, -0.0001585305f },
		{ 0.0006974436f, 0.0011382615f, -0.0058148842f, -0.0173985016f, -0.0187707614f, -0.0054363871f, 0.0003813511f, -0.0001722345f },
		{ 0.0007431114f, 0.0003990038f, -0.0088187150f, -0.0230795863f, -0.0224790219f, -0.0055143590f, 0.0003136692f, -0.0001309579f },
		{ 0.0008011667f, -0.0005811203f, -0.0121892722f, -0.0286926355f, -0.0258566365f, -0.0056333725f, 0.0002841109f, -0.0001103773f },
		{ 0.0006195912f, -0.0011478331f, -0.0170019965f, -0.0348781590f, -0.0284258318f, -0.0058140315f, 0.0002763260f, -0.0001056846f },
		{ 0.0006542017f, -0.0030664584f, -0.0210631720f, -0.0407935169f, -0.0312898830f, -0.0057553576f, 0.0001555996f, -0.0000479006f },
		{ 0.0006362227f, -0.0049676881f, -0.0267230089f, -0.0464900037f, -0.0330637202f, -0.0057538506f, 0.0001839596f, -0.0000589975f },
		{ 0.0004355567f, -0.0073085214f, -0.0317041450f, -0.0517912403f, -0.0350926525f, -0.0056858987f, 0.0000753451f, -0.0000091613f },
		{ 0.0003921251f, -0.0098509496f, -0.0373911760f, -0.0573096689f, -0.0367904369f, -0.0055929014f, 0.0000368600f, 0.0000077731f },
		{ -0.0000086869f, -0.0119730460f, -0.0427289848f, -0.0626393966f, -0.0382911664f, -0.0055124471f, -0.0000022020f, 0.0000265703f },
		{ -0.0002109559f, -0.0152011015f, -0.0486317781f, -0.0673540428f, -0.0395026230f, -0.0053573554f, 0.0000041310f, 0.0000250802f },
		{ -0.0003642039f, -0.0177069818f, -0.0546350194f, -0.0718455950f, -0.0407418734f, -0.0053030484f, -0.0000623320f, 0.0000553278f },
		{ -0.0006721666f, -0.0209653579f, -0.0603159770f, -0.0763849833f, -0.0416624720f, -0.0052276491f, -0.0001078210f, 0.0000743507f },
		{ -0.0009334308f, -0.0245725237f, -0.0660476472f, -0.0803042734f, -0.0423993605f, -0.0051388246f, -0.0001108479f, 0.0000773861f },
		{ -0.0013358080f, -0.0279127652f, -0.0716090063f, -0.0840743237f, -0.0431522987f, -0.0049300103f, -0.0001919121f, 0.0001140287f },
		{ -0.0013719324f, -0.0309163006f, -0.0772911276f, -0.0875814908f, -0.0435716353f, -0.0049611899f, -0.0001684638f, 0.0001023129f },
		{ -0.0019850718f, -0.0344558787f, -0.0823600222f, -0.0907768788f, -0.0441309151f, -0.0048415666f, -0.0001984367f, 0.0001161618f },
	},
	{
		{ 0.0007020877f, 0.0022860240f, 0.0026758883f, -0.0016363282f, -0.0085422647f, -0.0128796970f, -0.0036744443f, 0.0004432120f },
		{ 0.0002658231f, 0.0007600919f, 0.0004314132f, -0.0010356518f, -0.0039026174f, -0.0038911073f, -0.0003449344f, 0.0000025598f },
		{ 0.0001304434f, 0.0000633145f, 0.0000239252f, -0.0002279153f, -0.0008324670f, -0.0007104318f, -0.0000852371f, 0.0000091731f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000686343f, -0.0001890172f, 0.0000033859f, -0.0000467162f, -0.0006891630f, -0.0003651893f, -0.0000337856f, 0.0000028678f },
		{ 0.0001018417f, 0.0004018496f, -0.0002525547f, -0.0020801528f, -0.0025180712f, -0.0010667213f, 0.0001127262f, -0.0000573144f },
		{ 0.0003208561f, 0.0004115226f, -0.0012983828f, -0.0044788596f, -0.0051337377f, -0.0015720771f, 0.0001500069f, -0.0000686726f },
		{ 0.0002058858f, 0.0006735177f, -0.0027070017f, -0.0082850832f, -0.0079393503f, -0.0018437707f, 0.0001308902f, -0.0000521998f },
		{ 0.0002355666f, 0.0002485760f, -0.0047887481f, -0.0119575875f, -0.0106414104f, -0.0022133071f, 0.0001350181f, -0.0000527119f },
		{ 0.0003651329f, -0.0006864453f, -0.0076440791f, -0.0166805968f, -0.0130963625f, -0.0022497091f, 0.0000746505f, -0.0000183595f },
		{ 0.0004986742f, -0.0016184536f, -0.0110197822f, -0.0208025229f, -0.0157479340f, -0.0023618367f, -0.0000616748f, 0.0000427252f },
		{ 0.0004374140f, -0.0029064665f, -0.0144016848f, -0.0261122929f, -0.0177217237f, -0.0024376058f, -0.0000232570f, 0.0000283853f },
		{ 0.0002116087f, -0.0038094207f, -0.0190544606f, -0.0310668731f, -0.0196807944f, -0.0024971559f, -0.0000302908f, 0.0000308064f },
		{ 0.0002055311f, -0.0061737247f, -0.0232441849f, -0.0358790565f, -0.0213580698f, -0.0025655422f, -0.0000663211f, 0.0000461776f },
		{ 0.0002255933f, -0.0085477413f, -0.0286284971f, -0.0401790835f, -0.0226820114f, -0.0024374244f, -0.0001523223f, 0.0000874215f },
		{ -0.0000000614f, -0.0104425005f, -0.0339712213f, -0.0448030006f, -0.0239356581f, -0.0024775312f, -0.0001424209f, 0.0000828225f },
		{ -0.0004740104f, -0.0130639120f, -0.0388002876f, -0.0490662874f, -0.0251084438f, -0.0024281161f, -0.0001795882f, 0.0000992682f },
		{ -0.0004991718f, -0.0160835112f, -0.0440507340f, -0.0527906300f, -0.0260929854f, -0.0023688769f, -0.0002081179f, 0.0001128240f },
		{ -0.0010844383f, -0.0192056147f, -0.0494491391f, -0.0560929177f, -0.0268697257f, -0.0023388488f, -0.0002159104f, 0.0001172406f },
		{ -0.0011609999f, -0.0217532148f, -0.0545834226f, -0.0603834447f, -0.0276350061f, -0.0023619397f, -0.0002061810f, 0.0001116134f },
		{ -0.0015209542f, -0.0251418793f, -0.0595790821f, -0.0635514733f, -0.0281120279f, -0.0023094441f, -0.0002148478f, 0.0001130427f },
		{ -0.0017677964f, -0.0284758917f, -0.0646480566f, -0.0668238242f, -0.0286942898f, -0.0022650143f, -0.0002437632f, 0.0001255721f },
		{ -0.0022497236f, -0.0317056748f, -0.0693586238f, -0.0698365833f, -0.0290125045f, -0.0022595585f, -0.0002313045f, 0.0001206464f },
		{ -0.0025266956f, -0.0347197143f, -0.0744807515f, -0.0726838023f, -0.0293514318f, -0.0022848534f, -0.0002057526f, 0.0001080862f },
	},
	{
		{ 0.0008350851f, 0.0037622336f, 0.0033647040f, -0.0044021905f, -0.0154157812f, -0.0183665577f, -0.0036602486f, 0.0003819290f },
		{ 0.0003914972f, 0.0017331099f, 0.0002011310f, -0.0031079590f, -0.0085922595f, -0.0066032945f, -0.0000841922f, -0.0001198799f },
		{ 0.0002671200f, 0.0005427710f, 0.0001346084f, -0.0019538796f, -0.0034855448f, -0.0018851315f, 0.0000209168f, -0.0000334204f },
		{ 0.0000686343f, -0.0001890172f, 0.0000033859f, -0.0000467162f, -0.0006891630f, -0.0003651893f, -0.0000337856f, 0.0000028678f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000073839f, 0.0000142643f, 0.0000412995f, -0.0005546292f, -0.0006050422f, -0.0001025621f, 0.0000296780f, -0.0000115060f },
		{ 0.0002518881f, -0.0001074403f, -0.0005482530f, -0.0017918198f, -0.0020733689f, -0.0004371092f, -0.0000383079f, 0.0000165479f },
		{ -0.0000362224f, 0.0001972923f, -0.0017374809f, -0.0045816731f, -0.0035152064f, -0.0006651320f, 0.0000397548f, -0.0000170855f },
		{ 0.0002212297f, -0.0003253841f, -0.0032484991f, -0.0072740446f, -0.0057600506f, -0.0008558501f, -0.0000174169f, 0.0000116738f },
		{ 0.0001249204f, -0.0008759268f, -0.0058962172f, -0.0108528689f, -0.0074419378f, -0.0009310533f, -0.0000134516f, 0.0000129552f },
		{ 0.0000798923f, -0.0019627840f, -0.0085472976f, -0.0147141987f, -0.0091886718f, -0.0010034182f, -0.0000755203f, 0.0000404052f },
		{ 0.0002379279f, -0.0029604127f, -0.0123671130f, -0.0186207433f, -0.0106342585f, -0.0009571599f, -0.0001280313f, 0.0000671655f },
		{ 0.0001617494f, -0.0046728452f, -0.0160824493f, -0.0222083773f, -0.0122291376f, -0.0011023921f, -0.0001459580f, 0.0000754428f },
		{ -0.0000425587f, -0.0067296573f, -0.0201952640f, -0.0256889582f, -0.0136320314f, -0.0011005417f, -0.0001833783f, 0.0000942957f },
		{ -0.0002854170f, -0.0082724750f, -0.0247566291f, -0.0300597945f, -0.0143889826f, -0.0011737117f, -0.0001124007f, 0.0000612744f },
		{ -0.0004458491f, -0.0106310303f, -0.0287971564f, -0.0338478906f, -0.0155888513f, -0.0012167250f, -0.0001515939f, 0.0000798484f },
		{ -0.0005862061f, -0.0130954669f, -0.0335057613f, -0.0374066479f, -0.0163089654f, -0.0011995550f, -0.0001366881f, 0.0000712638f },
		{ -0.0008929014f, -0.0156702201f, -0.0380593914f, -0.0409752365f, -0.0171044889f, -0.0011405075f, -0.0001843378f, 0.0000915703f },
		{ -0.0013138157f, -0.0181211437f, -0.0429982675f, -0.0441823012f, -0.0176092430f, -0.0012390260f, -0.0000957184f, 0.0000525980f },
		{ -0.0016459813f, -0.0209565902f, -0.0476831600f, -0.0473871585f, -0.0183365201f, -0.0011067636f, -0.0001601486f, 0.0000799965f },
		{ -0.0018954471f, -0.0238422697f, -0.0522412940f, -0.0502659170f, -0.0187780206f, -0.0011346394f, -0.0001480355f, 0.0000747781f },
		{ -0.0022816796f, -0.0271082196f, -0.0566440213f, -0.0525913559f, -0.0192275298f, -0.0011120452f, -0.0001671713f, 0.0000822266f },
		{ -0.0025526969f, -0.0304057602f, -0.0610224418f, -0.0552767354f, -0.0196871460f, -0.0010811617f, -0.0001878648f, 0.0000919484f },
		{ -0.0027611102f, -0.0332881038f, -0.0657977862f, -0.0574789393f, -0.0198617141f, -0.0010656662f, -0.0001957084f, 0.0000952998f },
	},
	{
		{ 0.0013825909f, 0.0051816937f, 0.0031519880f, -0.0085383901f, -0.0231543515f, -0.0226375384f, -0.0035029954f, 0.0002677645f },
		{ 0.0006771303f, 0.0024025421f, 0.0000619749f, -0.0067138127f, -0.0144033109f, -0.0088686154f, 0.0001655023f, -0.0002097214f },
		{ 0.0002374999f, 0.0011093951f, -0.0005585794f, -0.0044567326f, -0.0069657919f, -0.0031990936f, 0.0001960228f, -0.0001102941f },
		{ 0.0001018417f, 0.0004018496f, -0.0002525547f, -0.0020801528f, -0.0025180712f, -0.0010667213f, 0.0001127262f, -0.0000573144f },
		{ 0.0000073839f, 0.0000142643f, 0.0000412995f, -0.0005546292f, -0.0006050422f, -0.0001025621f, 0.0000296780f, -0.0000115060f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0001643737f, -0.0002725885f, -0.0001381829f, -0.0004866940f, -0.0002436657f, -0.0001503259f, 0.0000221355f, -0.0000134983f },
		{ 0.0000076584f, 0.0000742278f, -0.0004085969f, -0.0020184878f, -0.0013499536f, -0.0001956801f, 0.0000433909f, -0.0000188948f },
		{ 0.0001935282f, -0.0002544551f, -0.0020336173f, -0.0039384077f, -0.0026687685f, -0.0003451311f, -0.0000031608f, 0.0000025352f },
		{ 0.0000882000f, -0.0009671896f, -0.0041687571f, -0.0063552002f, -0.0038637650f, -0.0003464278f, -0.0000590289f, 0.0000312086f },
		{ 0.0000645534f, -0.0015054118f, -0.0063073397f, -0.0093044484f, -0.0052761467f, -0.0004566119f, -0.0000760203f, 0.0000384313f },
		{ 0.0000833270f, -0.0029590860f, -0.0097279251f, -0.0120882332f, -0.0063720508f, -0.0004579280f, -0.0000822728f, 0.0000438026f },
		{ -0.0000435744f, -0.0041031188f, -0.0123875125f, -0.0158275402f, -0.0075163370f, -0.0005628363f, -0.0001033188f, 0.0000517827f },
		{ -0.0001716400f, -0.0059139883f, -0.0160828445f, -0.0186131351f, -0.0082641953f, -0.0005676490f, -0.0000777327f, 0.0000402639f },
		{ -0.0004683724f, -0.0073025182f, -0.0195978413f, -0.0220156432f, -0.0093275667f, -0.0005996039f, -0.0000777617f, 0.0000401833f },
		{ -0.0005757519f, -0.0097581705f, -0.0237061250f, -0.0250734698f, -0.0100125679f, -0.0006009477f, -0.0000934591f, 0.0000479818f },
		{ -0.0008791218f, -0.0117029059f, -0.0279180117f, -0.0284504257f, -0.0104365615f, -0.0006260781f, -0.0000489096f, 0.0000257479f },
		{ -0.0011722908f, -0.0143052143f, -0.0319585437f, -0.0312013700f, -0.0112603171f, -0.0005756092f, -0.0001182298f, 0.0000564562f },
		{ -0.0014893119f, -0.0172605425f, -0.0364038800f, -0.0337954524f, -0.0116929653f, -0.0006479142f, -0.0000754126f, 0.0000379771f },
		{ -0.0017475253f, -0.0198794606f, -0.0408107996f, -0.0363760842f, -0.0122399426f, -0.0005958884f, -0.0001043239f, 0.0000496586f },
		{ -0.0018182250f, -0.0231513878f, -0.0449166972f, -0.0384556615f, -0.0128761826f, -0.0005821226f, -0.0001382139f, 0.0000658371f },
		{ -0.0021507079f, -0.0260855548f, -0.0493601935f, -0.0410097547f, -0.0131461428f, -0.0005478882f, -0.0001510920f, 0.0000729908f },
		{ -0.0023223258f, -0.0282548369f, -0.0534378663f, -0.0433197519f, -0.0134875791f, -0.0004974475f, -0.0001713228f, 0.0000808794f },
		{ -0.0026690394f, -0.0314294818f, -0.0573047049f, -0.0453867383f, -0.0138981374f, -0.0005375114f, -0.0001802256f, 0.0000835338f },
	},
	{
		{ 0.0014378527f, 0.0062397717f, 0.0022949746f, -0.0134935461f, -0.0310083588f, -0.0261374471f, -0.0032120969f, 0.0001384093f },
		{ 0.0008190035f, 0.0029804939f, -0.0008195875f, -0.0114668974f, -0.0199406489f, -0.0106304922f, 0.0002950372f, -0.0002679317f },
		{ 0.0005085348f, 0.0011270412f, -0.0015382326f, -0.0079155046f, -0.0109644533f, -0.0041424699f, 0.0002458832f, -0.0001254965f },
		{ 0.0003208561f, 0.0004115226f, -0.0012983828f, -0.0044788596f, -0.0051337377f, -0.0015720771f, 0.0001500069f, -0.0000686726f },
		{ 0.0002518881f, -0.0001074403f, -0.0005482530f, -0.0017918198f, -0.0020733689f, -0.0004371092f, -0.0000383079f, 0.0000165479f },
		{ 0.0001643737f, -0.0002725885f, -0.0001381829f, -0.0004866940f, -0.0002436657f, -0.0001503259f, 0.0000221355f, -0.0000134983f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0001263319f, -0.0004491240f, -0.0002564817f, -0.0000589736f, -0.0003611632f, -0.0000582245f, -0.0000262459f, 0.0000108643f },
		{ 0.0002112175f, -0.0002028541f, -0.0009220339f, -0.0016325791f, -0.0010008059f, -0.0001176196f, -0.0000309815f, 0.0000140596f },
		{ 0.0000580943f, -0.0006808754f, -0.0028484150f, -0.0029297509f, -0.0017764439f, -0.0001557085f, -0.0000301681f, 0.0000167299f },
		{ 0.0001464682f, -0.0014780181f, -0.0040018422f, -0.0052770345f, -0.0028798380f, -0.0001140549f, -0.0001017278f, 0.0000480671f },
		{ 0.0000366491f, -0.0026329996f, -0.0061950093f, -0.0076180100f, -0.0034057545f, -0.0002553283f, -0.0000389912f, 0.0000194384f },
		{ -0.0003309509f, -0.0035619181f, -0.0087791956f, -0.0103977115f, -0.0041829778f, -0.0003274420f, -0.0000005201f, 0.0000015812f },
		{ -0.0002542934f, -0.0050181311f, -0.0121557129f, -0.0129087084f, -0.0051591757f, -0.0002643652f, -0.0000471457f, 0.0000239303f },
		{ -0.0004112485f, -0.0063053819f, -0.0156525001f, -0.0157693787f, -0.0057814258f, -0.0003212420f, -0.0000485897f, 0.0000231046f },
		{ -0.0007409176f, -0.0086944517f, -0.0190352006f, -0.0182216970f, -0.0062510063f, -0.0002849287f, -0.0000614142f, 0.0000287299f },
		{ -0.0011950171f, -0.0104906163f, -0.0224708831f, -0.0208773012f, -0.0069236024f, -0.0003358438f, -0.0000377985f, 0.0000177711f },
		{ -0.0010054251f, -0.0132458597f, -0.0264694399f, -0.0231715779f, -0.0074337861f, -0.0003207798f, -0.0000854451f, 0.0000402556f },
		{ -0.0013025968f, -0.0158175742f, -0.0306000778f, -0.0254276828f, -0.0077969888f, -0.0002686216f, -0.0001043855f, 0.0000487925f },
		{ -0.0014353696f, -0.0181425936f, -0.0341190799f, -0.0276534673f, -0.0084869667f, -0.0002307499f, -0.0001387704f, 0.0000635961f },
		{ -0.0017445891f, -0.0205671513f, -0.0377631147f, -0.0300749697f, -0.0088439887f, -0.0002787862f, -0.0001272253f, 0.0000582449f },
		{ -0.0021930151f, -0.0229273657f, -0.0417093130f, -0.0321235363f, -0.0088973670f, -0.0003640298f, -0.0000613020f, 0.0000273588f },
		{ -0.0023710378f, -0.0259292191f, -0.0454552170f, -0.0335894712f, -0.0093051985f, -0.0003111105f, -0.0001077974f, 0.0000484422f },
		{ -0.0028360161f, -0.0288536925f, -0.0488933838f, -0.0353138997f, -0.0093997668f, -0.0003320990f, -0.0000891373f, 0.0000398416f },
	},
	{
		{ 0.0017845265f, 0.0069656897f, 0.0008046119f, -0.0200350318f, -0.0388614192f, -0.0288121216f, -0.0029407713f, 0.0000496949f },
		{ 0.0011201319f, 0.0034986727f, -0.0030600958f, -0.0172683502f, -0.0254997636f, -0.0118599169f, 0.0005137632f, -0.0003471993f },
		{ 0.0005545115f, 0.0009310939f, -0.0035508127f, -0.0122149932f, -0.0148514357f, -0.0047609927f, 0.0003452028f, -0.0001585305f },
		{ 0.0002058858f, 0.0006735177f, -0.0027070017f, -0.0082850832f, -0.0079393503f, -0.0018437707f, 0.0001308902f, -0.0000521998f },
		{ -0.0000362224f, 0.0001972923f, -0.0017374809f, -0.0045816731f, -0.0035152064f, -0.0006651320f, 0.0000397548f, -0.0000170855f },
		{ 0.0000076584f, 0.0000742278f, -0.0004085969f, -0.0020184878f, -0.0013499536f, -0.0001956801f, 0.0000433909f, -0.0000188948f },
		{ 0.0001263319f, -0.0004491240f, -0.0002564817f, -0.0000589736f, -0.0003611632f, -0.0000582245f, -0.0000262459f, 0.0000108643f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000356983f, -0.0002046155f, -0.0003813095f, -0.0000715276f, -0.0002036256f, -0.0000485381f, 0.0000065746f, -0.0000020558f },
		{ 0.0000428041f, -0.0001859086f, -0.0007724557f, -0.0013985510f, -0.0007270090f, -0.0001076472f, 0.0000090493f, -0.0000034177f },
		{ 0.0001298802f, -0.0008438379f, -0.0023222410f, -0.0031755772f, -0.0011361631f, -0.0001096762f, -0.0000026559f, 0.0000023082f },
		{ -0.0001596175f, -0.0014531031f, -0.0040421677f, -0.0044257965f, -0.0018613306f, -0.0001619149f, 0.0000165361f, -0.0000062677f },
		{ -0.0001095801f, -0.0025743864f, -0.0059699655f, -0.0066708368f, -0.0024567459f, -0.0001016219f, -0.0000413438f, 0.0000189763f },
		{ -0.0003618342f, -0.0039675763f, -0.0085269072f, -0.0088567672f, -0.0028023732f, -0.0001919958f, 0.0000159133f, -0.0000069802f },
		{ -0.0004794924f, -0.0057720025f, -0.0117887923f, -0.0105215799f, -0.0036085619f, -0.0001670854f, -0.0000316054f, 0.0000159023f },
		{ -0.0005827587f, -0.0079058190f, -0.0145130257f, -0.0127709615f, -0.0041071409f, -0.0000721608f, -0.0000895604f, 0.0000408520f },
		{ -0.0008913215f, -0.0094764264f, -0.0179283345f, -0.0149396069f, -0.0046873119f, -0.0000870392f, -0.0000972393f, 0.0000440969f },
		{ -0.0009902041f, -0.0115445750f, -0.0211342824f, -0.0170562468f, -0.0049094785f, -0.0001617029f, -0.0000665283f, 0.0000295571f },
		{ -0.0014210681f, -0.0131984653f, -0.0245049593f, -0.0191410025f, -0.0052362212f, -0.0001944265f, -0.0000278934f, 0.0000110990f },
		{ -0.0013864325f, -0.0154945706f, -0.0279337040f, -0.0211859729f, -0.0055590700f, -0.0002072028f, -0.0000300201f, 0.0000133053f },
		{ -0.0020222959f, -0.0176612957f, -0.0315519719f, -0.0229199742f, -0.0057057359f, -0.0002774717f, 0.0000110743f, -0.0000057611f },
		{ -0.0022253795f, -0.0205207187f, -0.0345005355f, -0.0246034181f, -0.0062616936f, -0.0002256396f, -0.0000301194f, 0.0000127321f },
		{ -0.0025572237f, -0.0229124605f, -0.0380365450f, -0.0261675873f, -0.0064864413f, -0.0002355635f, -0.0000226164f, 0.0000096901f },
		{ -0.0027150476f, -0.0252969048f, -0.0414077781f, -0.0277205800f, -0.0065935286f, -0.0002293462f, -0.0000322155f, 0.0000135025f },
	},
	{
		{ 0.0018304860f, 0.0072381723f, -0.0015914016f, -0.0270120284f, -0.0464796721f, -0.0310331366f, -0.0026499474f, -0.0000350043f },
		{ 0.0011006637f, 0.0032675406f, -0.0051967234f, -0.0234166032f, -0.0312371186f, -0.0124964801f, 0.0006020596f, -0.0003499195f },
		{ 0.0006974436f, 0.0011382615f, -0.0058148842f, -0.0173985016f, -0.0187707614f, -0.0054363871f, 0.0003813511f, -0.0001722345f },
		{ 0.0002355666f, 0.0002485760f, -0.0047887481f, -0.0119575875f, -0.0106414104f, -0.0022133071f, 0.0001350181f, -0.0000527119f },
		{ 0.0002212297f, -0.0003253841f, -0.0032484991f, -0.0072740446f, -0.0057600506f, -0.0008558501f, -0.0000174169f, 0.0000116738f },
		{ 0.0001935282f, -0.0002544551f, -0.0020336173f, -0.0039384077f, -0.0026687685f, -0.0003451311f, -0.0000031608f, 0.0000025352f },
		{ 0.0002112175f, -0.0002028541f, -0.0009220339f, -0.0016325791f, -0.0010008059f, -0.0001176196f, -0.0000309815f, 0.0000140596f },
		{ -0.0000356983f, -0.0002046155f, -0.0003813095f, -0.0000715276f, -0.0002036256f, -0.0000485381f, 0.0000065746f, -0.0000020558f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000711929f, -0.0000119120f, -0.0001247201f, -0.0002325863f, -0.0001760734f, -0.0000714192f, 0.0000270762f, -0.0000114298f },
		{ -0.0000050920f, -0.0007072441f, -0.0004674440f, -0.0010201666f, -0.0005950698f, 0.0000027882f, 0.0000045986f, -0.0000004471f },
		{ -0.0002923876f, -0.0009206263f, -0.0018885743f, -0.0026109431f, -0.0008845996f, -0.0000891910f, 0.0000234074f, -0.0000104765f },
		{ -0.0000662109f, -0.0023333024f, -0.0037743799f, -0.0036732195f, -0.0012232079f, -0.0000534903f, -0.0000115905f, 0.0000053066f },
		{ -0.0003712246f, -0.0033097608f, -0.0057973990f, -0.0054007913f, -0.0016497006f, -0.0000685682f, -0.0000304125f, 0.0000130529f },
		{ -0.0003937110f, -0.0045539227f, -0.0083377189f, -0.0071222419f, -0.0021816329f, -0.0000638606f, -0.0000488979f, 0.0000233475f },
		{ -0.0001487151f, -0.0057908819f, -0.0110840883f, -0.0088261383f, -0.0025513878f, -0.0000826598f, -0.0000458692f, 0.0000199062f },
		{ -0.0007025988f, -0.0073079063f, -0.0136196051f, -0.0107693776f, -0.0028476523f, -0.0001145903f, -0.0000125693f, 0.0000041083f },
		{ -0.0008225545f, -0.0094291037f, -0.0165312206f, -0.0120651249f, -0.0032053662f, -0.0000919494f, -0.0000416832f, 0.0000184910f },
		{ -0.0011398693f, -0.0115316406f, -0.0194422935f, -0.0137093106f, -0.0034546649f, -0.0001112045f, -0.0000244029f, 0.0000107748f },
		{ -0.0015974701f, -0.0134290381f, -0.0225056220f, -0.0158733822f, -0.0037547817f, -0.0001599456f, -0.0000023337f, -0.0000011154f },
		{ -0.0018701281f, -0.0155355175f, -0.0252875778f, -0.0172049142f, -0.0041409795f, -0.0001293880f, -0.0000090986f, 0.0000029973f },
		{ -0.0016985446f, -0.0182652146f, -0.0288023296f, -0.0185649524f, -0.0042770398f, -0.0001717900f, -0.0000168679f, 0.0000071367f },
		{ -0.0019997434f, -0.0201831538f, -0.0321940737f, -0.0199241686f, -0.0044034957f, -0.0000888149f, -0.0000562889f, 0.0000244376f },
		{ -0.0024203151f, -0.0226936980f, -0.0348480798f, -0.0213088378f, -0.0046627884f, -0.0001119658f, -0.0000452458f, 0.0000203761f },
	},
	{
		{ 0.0022733086f, 0.0070736157f, -0.0050691935f, -0.0342107469f, -0.0536010464f, -0.0327522125f, -0.0023932619f, -0.0000927086f },
		{ 0.0011217467f, 0.0029558879f, -0.0083550438f, -0.0299568040f, -0.0361265267f, -0.0130398978f, 0.0005722321f, -0.0003342050f },
		{ 0.0007431114f, 0.0003990038f, -0.0088187150f, -0.0230795863f, -0.0224790219f, -0.0055143590f, 0.0003136692f, -0.0001309579f },
		{ 0.0003651329f, -0.0006864453f, -0.0076440791f, -0.0166805968f, -0.0130963625f, -0.0022497091f, 0.0000746505f, -0.0000183595f },
		{ 0.0001249204f, -0.0008759268f, -0.0058962172f, -0.0108528689f, -0.0074419378f, -0.0009310533f, -0.0000134516f, 0.0000129552f },
		{ 0.0000882000f, -0.0009671896f, -0.0041687571f, -0.0063552002f, -0.0038637650f, -0.0003464278f, -0.0000590289f, 0.0000312086f },
		{ 0.0000580943f, -0.0006808754f, -0.0028484150f, -0.0029297509f, -0.0017764439f, -0.0001557085f, -0.0000301681f, 0.0000167299f },
		{ 0.0000428041f, -0.0001859086f, -0.0007724557f, -0.0013985510f, -0.0007270090f, -0.0001076472f, 0.0000090493f, -0.0000034177f },
		{ 0.0000711929f, -0.0000119120f, -0.0001247201f, -0.0002325863f, -0.0001760734f, -0.0000714192f, 0.0000270762f, -0.0000114298f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0002170758f, -0.0000045589f, -0.0003639860f, -0.0003396657f, -0.0001235996f, -0.0000305375f, 0.0000248349f, -0.0000112161f },
		{ 0.0000002531f, -0.0006259976f, -0.0007850150f, -0.0008825350f, -0.0004499618f, -0.0000018721f, -0.0000433819f, 0.0000201941f },
		{ 0.0000415300f, -0.0015555140f, -0.0024153624f, -0.0018707536f, -0.0006935137f, 0.0000401785f, -0.0000428113f, 0.0000209402f },
		{ 0.0000463885f, -0.0022019070f, -0.0036943172f, -0.0029637526f, -0.0010946484f, 0.0000256099f, -0.0000541444f, 0.0000246446f },
		{ -0.0004362240f, -0.0028691186f, -0.0054316607f, -0.0043862843f, -0.0011944413f, -0.0000272541f, -0.0000139949f, 0.0000045573f },
		{ -0.0005051484f, -0.0044350465f, -0.0076589009f, -0.0053807719f, -0.0014763909f, -0.0000552395f, -0.0000342144f, 0.0000150339f },
		{ -0.0005787552f, -0.0059458378f, -0.0101970011f, -0.0069969759f, -0.0017644589f, -0.0000622066f, -0.0000170862f, 0.0000080368f },
		{ -0.0008037741f, -0.0071142225f, -0.0126150864f, -0.0088230629f, -0.0020714483f, -0.0001071901f, 0.0000088672f, -0.0000050791f },
		{ -0.0010697522f, -0.0093203957f, -0.0152399537f, -0.0101993077f, -0.0023047423f, -0.0001037607f, -0.0000067840f, 0.0000020017f },
		{ -0.0010778172f, -0.0114198537f, -0.0178751649f, -0.0111264490f, -0.0027582045f, -0.0000318465f, -0.0000590099f, 0.0000263890f },
		{ -0.0013643363f, -0.0133255220f, -0.0205073482f, -0.0127180640f, -0.0028079858f, -0.0000430388f, -0.0000485189f, 0.0000213260f },
		{ -0.0015816170f, -0.0153798975f, -0.0233502364f, -0.0139283636f, -0.0029695835f, -0.0000582238f, -0.0000349001f, 0.0000150575f },
		{ -0.0017764021f, -0.0175093493f, -0.0264951536f, -0.0151599215f, -0.0030258841f, -0.0001026968f, -0.0000165057f, 0.0000073163f },
		{ -0.0021124043f, -0.0197146188f, -0.0287206812f, -0.0164826169f, -0.0032445313f, -0.0000789067f, -0.0000243437f, 0.0000097516f },
	},
	{
		{ 0.0024437550f, 0.0065261434f, -0.0085618643f, -0.0424814938f, -0.0602524126f, -0.0339976678f, -0.0021885765f, -0.0001247071f },
		{ 0.0011883082f, 0.0022511399f, -0.0119741655f, -0.0372191093f, -0.0405748398f, -0.0134748448f, 0.0006120625f, -0.0003368361f },
		{ 0.0008011667f, -0.0005811203f, -0.0121892722f, -0.0286926355f, -0.0258566365f, -0.0056333725f, 0.0002841109f, -0.0001103773f },
		{ 0.0004986742f, -0.0016184536f, -0.0110197822f, -0.0208025229f, -0.0157479340f, -0.0023618367f, -0.0000616748f, 0.0000427252f },
		{ 0.0000798923f, -0.0019627840f, -0.0085472976f, -0.0147141987f, -0.0091886718f, -0.0010034182f, -0.0000755203f, 0.0000404052f },
		{ 0.0000645534f, -0.0015054118f, -0.0063073397f, -0.0093044484f, -0.0052761467f, -0.0004566119f, -0.0000760203f, 0.0000384313f },
		{ 0.0001464682f, -0.0014780181f, -0.0040018422f, -0.0052770345f, -0.0028798380f, -0.0001140549f, -0.0001017278f, 0.0000480671f },
		{ 0.0001298802f, -0.0008438379f, -0.0023222410f, -0.0031755772f, -0.0011361631f, -0.0001096762f, -0.0000026559f, 0.0000023082f },
		{ -0.0000050920f, -0.0007072441f, -0.0004674440f, -0.0010201666f, -0.0005950698f, 0.0000027882f, 0.0000045986f, -0.0000004471f },
		{ -0.0002170758f, -0.0000045589f, -0.0003639860f, -0.0003396657f, -0.0001235996f, -0.0000305375f, 0.0000248349f, -0.0000112161f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000017678f, -0.0003358487f, -0.0002871516f, -0.0002728715f, -0.0001499684f, 0.0000189479f, -0.0000230735f, 0.0000113135f },
		{ -0.0000879063f, -0.0005395972f, -0.0010370252f, -0.0008031147f, -0.0001522720f, -0.0000330608f, 0.0000028081f, -0.0000031805f },
		{ -0.0001484678f, -0.0010155809f, -0.0021381981f, -0.0015084865f, -0.0005415679f, -0.0000796084f, 0.0000303637f, -0.0000138088f },
		{ -0.0002748582f, -0.0018674871f, -0.0033626015f, -0.0024618440f, -0.0006094637f, 0.0000081231f, -0.0000184747f, 0.0000082071f },
		{ -0.0005838388f, -0.0035109117f, -0.0048901364f, -0.0034966930f, -0.0008538904f, 0.0000033181f, -0.0000196208f, 0.0000091401f },
		{ -0.0006953601f, -0.0042236501f, -0.0069578074f, -0.0047919847f, -0.0011481029f, -0.0001060285f, 0.0000376893f, -0.0000168376f },
		{ -0.0005475042f, -0.0060046488f, -0.0092497778f, -0.0058304598f, -0.0014090229f, -0.0000336641f, -0.0000273834f, 0.0000118612f },
		{ -0.0006875011f, -0.0075642636f, -0.0116168408f, -0.0068471046f, -0.0016361362f, 0.0000058613f, -0.0000427592f, 0.0000195480f },
		{ -0.0009377027f, -0.0088260239f, -0.0136834126f, -0.0083338986f, -0.0016919699f, -0.0000642233f, -0.0000042830f, 0.0000025843f },
		{ -0.0011626057f, -0.0109752883f, -0.0166292785f, -0.0093239127f, -0.0018637151f, -0.0000597338f, -0.0000061847f, 0.0000025415f },
		{ -0.0015369033f, -0.0129430907f, -0.0184353469f, -0.0106337245f, -0.0020004812f, -0.0000457862f, -0.0000169965f, 0.0000067868f },
		{ -0.0017563685f, -0.0148065558f, -0.0208302379f, -0.0115460989f, -0.0022173243f, -0.0000040698f, -0.0000368804f, 0.0000165881f },
		{ -0.0017530407f, -0.0169034790f, -0.0230306149f, -0.0126888584f, -0.0023706663f, -0.0000370808f, -0.0000343382f, 0.0000157609f },
	},
	{
		{ 0.0023873233f, 0.0061943241f, -0.0134692178f, -0.0503087556f, -0.0660404033f, -0.0351961785f, -0.0018487842f, -0.0002133657f },
		{ 0.0012647439f, 0.0005014252f, -0.0168835460f, -0.0437842725f, -0.0444183490f, -0.0136923228f, 0.0006345007f, -0.0003370773f },
		{ 0.0006195912f, -0.0011478331f, -0.0170019965f, -0.0348781590f, -0.0284258318f, -0.0058140315f, 0.0002763260f, -0.0001056846f },
		{ 0.0004374140f, -0.0029064665f, -0.0144016848f, -0.0261122929f, -0.0177217237f, -0.0024376058f, -0.0000232570f, 0.0000283853f },
		{ 0.0002379279f, -0.0029604127f, -0.0123671130f, -0.0186207433f, -0.0106342585f, -0.0009571599f, -0.0001280313f, 0.0000671655f },
		{ 0.0000833270f, -0.0029590860f, -0.0097279251f, -0.0120882332f, -0.0063720508f, -0.0004579280f, -0.0000822728f, 0.0000438026f },
		{ 0.0000366491f, -0.0026329996f, -0.0061950093f, -0.0076180100f, -0.0034057545f, -0.0002553283f, -0.0000389912f, 0.0000194384f },
		{ -0.0001596175f, -0.0014531031f, -0.0040421677f, -0.0044257965f, -0.0018613306f, -0.0001619149f, 0.0000165361f, -0.0000062677f },
		{ -0.0002923876f, -0.0009206263f, -0.0018885743f, -0.0026109431f, -0.0008845996f, -0.0000891910f, 0.0000234074f, -0.0000104765f },
		{ 0.0000002531f, -0.0006259976f, -0.0007850150f, -0.0008825350f, -0.0004499618f, -0.0000018721f, -0.0000433819f, 0.0000201941f },
		{ -0.0000017678f, -0.0003358487f, -0.0002871516f, -0.0002728715f, -0.0001499684f, 0.0000189479f, -0.0000230735f, 0.0000113135f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0001779064f, -0.0001704979f, -0.0005272783f, -0.0000951018f, -0.0001904708f, -0.0000052974f, -0.0000198422f, 0.0000094297f },
		{ -0.0000607685f, -0.0005049132f, -0.0010059044f, -0.0005012621f, -0.0002072625f, 0.0000342216f, -0.0000406828f, 0.0000184683f },
		{ -0.0000429148f, -0.0009360355f, -0.0017270813f, -0.0014400135f, -0.0004921281f, -0.0000264903f, -0.0000142429f, 0.0000070267f },
		{ -0.0002254489f, -0.0020245856f, -0.0031108089f, -0.0019885461f, -0.0006021597f, -0.0000468778f, -0.0000025606f, 0.0000005660f },
		{ -0.0000810828f, -0.0033329168f, -0.0045024599f, -0.0029952086f, -0.0007232163f, 0.0000622125f, -0.0000641684f, 0.0000285747f },
		{ -0.0001848309f, -0.0043243536f, -0.0066586803f, -0.0035857204f, -0.0007761920f, 0.0000034095f, -0.0000294249f, 0.0000130022f },
		{ -0.0005685983f, -0.0054630074f, -0.0086703637f, -0.0047092854f, -0.0010371316f, 0.0000104657f, -0.0000305495f, 0.0000138298f },
		{ -0.0008419497f, -0.0071696512f, -0.0103304428f, -0.0059289707f, -0.0009245607f, -0.0000989930f, 0.0000370960f, -0.0000174453f },
		{ -0.0009677267f, -0.0090993638f, -0.0122209830f, -0.0068376146f, -0.0012026245f, -0.0000277582f, -0.0000058429f, 0.0000019971f },
		{ -0.0011709312f, -0.0107006188f, -0.0143421838f, -0.0077024505f, -0.0015434371f, -0.0000093275f, -0.0000374469f, 0.0000168019f },
		{ -0.0011162162f, -0.0124820799f, -0.0166434481f, -0.0086649735f, -0.0016068459f, 0.0000068586f, -0.0000507258f, 0.0000224763f },
		{ -0.0015771110f, -0.0143606070f, -0.0187819565f, -0.0094577610f, -0.0016724205f, -0.0000318586f, -0.0000275721f, 0.0000133397f },
	},
	{
		{ 0.0025463896f, 0.0048266681f, -0.0184980891f, -0.0582412150f, -0.0717779577f, -0.0360857408f, -0.0015722280f, -0.0002719773f },
		{ 0.0012924316f, -0.0005498344f, -0.0221728734f, -0.0507357020f, -0.0479981863f, -0.0137243595f, 0.0006692616f, -0.0003385055f },
		{ 0.0006542017f, -0.0030664584f, -0.0210631720f, -0.0407935169f, -0.0312898830f, -0.0057553576f, 0.0001555996f, -0.0000479006f },
		{ 0.0002116087f, -0.0038094207f, -0.0190544606f, -0.0310668731f, -0.0196807944f, -0.0024971559f, -0.0000302908f, 0.0000308064f },
		{ 0.0001617494f, -0.0046728452f, -0.0160824493f, -0.0222083773f, -0.0122291376f, -0.0011023921f, -0.0001459580f, 0.0000754428f },
		{ -0.0000435744f, -0.0041031188f, -0.0123875125f, -0.0158275402f, -0.0075163370f, -0.0005628363f, -0.0001033188f, 0.0000517827f },
		{ -0.0003309509f, -0.0035619181f, -0.0087791956f, -0.0103977115f, -0.0041829778f, -0.0003274420f, -0.0000005201f, 0.0000015812f },
		{ -0.0001095801f, -0.0025743864f, -0.0059699655f, -0.0066708368f, -0.0024567459f, -0.0001016219f, -0.0000413438f, 0.0000189763f },
		{ -0.0000662109f, -0.0023333024f, -0.0037743799f, -0.0036732195f, -0.0012232079f, -0.0000534903f, -0.0000115905f, 0.0000053066f },
		{ 0.0000415300f, -0.0015555140f, -0.0024153624f, -0.0018707536f, -0.0006935137f, 0.0000401785f, -0.0000428113f, 0.0000209402f },
		{ -0.0000879063f, -0.0005395972f, -0.0010370252f, -0.0008031147f, -0.0001522720f, -0.0000330608f, 0.0000028081f, -0.0000031805f },
		{ 0.0001779064f, -0.0001704979f, -0.0005272783f, -0.0000951018f, -0.0001904708f, -0.0000052974f, -0.0000198422f, 0.0000094297f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0001542769f, 0.0001072031f, -0.0002295087f, -0.0002477811f, -0.0000283801f, -0.0000856824f, 0.0000392268f, -0.0000175150f },
		{ -0.0000749595f, -0.0008645559f, -0.0010900554f, -0.0004510374f, -0.0001397312f, 0.0000167365f, -0.0000199964f, 0.0000090151f },
		{ -0.0001405775f, -0.0015247364f, -0.0015838514f, -0.0008492705f, -0.0004115830f, 0.0000205332f, -0.0000395932f, 0.0000193438f },
		{ 0.0001226594f, -0.0021593839f, -0.0034091227f, -0.0016719092f, -0.0004566004f, 0.0000191330f, -0.0000442699f, 0.0000200740f },
		{ -0.0003724996f, -0.0025591326f, -0.0044519681f, -0.0027190243f, -0.0004113753f, -0.0000496236f, 0.0000347522f, -0.0000163026f },
		{ -0.0006367664f, -0.0042247501f, -0.0057157511f, -0.0032184148f, -0.0005261459f, -0.0000126446f, 0.0000044445f, -0.0000034062f },
		{ -0.0006035017f, -0.0054503295f, -0.0076217147f, -0.0040148459f, -0.0005448100f, -0.0000507481f, 0.0000260997f, -0.0000118847f },
		{ -0.0006829786f, -0.0067374802f, -0.0088023085f, -0.0048766832f, -0.0009499181f, -0.0000288997f, -0.0000025756f, 0.0000009633f },
		{ -0.0010537006f, -0.0083659129f, -0.0108162718f, -0.0054227815f, -0.0011424858f, 0.0000082157f, -0.0000413638f, 0.0000192794f },
		{ -0.0011926565f, -0.0099886742f, -0.0130534429f, -0.0062684901f, -0.0010480828f, -0.0000276126f, -0.0000122636f, 0.0000056842f },
		{ -0.0013120522f, -0.0114452336f, -0.0150187447f, -0.0071847401f, -0.0012358197f, -0.0000244901f, -0.0000247694f, 0.0000120729f },
	},
	{
		{ 0.0023271270f, 0.0036031184f, -0.0237233083f, -0.0662586837f, -0.0767945983f, -0.0366893074f, -0.0014207599f, -0.0002952001f },
		{ 0.0012956457f, -0.0022274904f, -0.0275656032f, -0.0574678794f, -0.0512861651f, -0.0137089657f, 0.0006556195f, -0.0003204017f },
		{ 0.0006362227f, -0.0049676881f, -0.0267230089f, -0.0464900037f, -0.0330637202f, -0.0057538506f, 0.0001839596f, -0.0000589975f },
		{ 0.0002055311f, -0.0061737247f, -0.0232441849f, -0.0358790565f, -0.0213580698f, -0.0025655422f, -0.0000663211f, 0.0000461776f },
		{ -0.0000425587f, -0.0067296573f, -0.0201952640f, -0.0256889582f, -0.0136320314f, -0.0011005417f, -0.0001833783f, 0.0000942957f },
		{ -0.0001716400f, -0.0059139883f, -0.0160828445f, -0.0186131351f, -0.0082641953f, -0.0005676490f, -0.0000777327f, 0.0000402639f },
		{ -0.0002542934f, -0.0050181311f, -0.0121557129f, -0.0129087084f, -0.0051591757f, -0.0002643652f, -0.0000471457f, 0.0000239303f },
		{ -0.0003618342f, -0.0039675763f, -0.0085269072f, -0.0088567672f, -0.0028023732f, -0.0001919958f, 0.0000159133f, -0.0000069802f },
		{ -0.0003712246f, -0.0033097608f, -0.0057973990f, -0.0054007913f, -0.0016497006f, -0.0000685682f, -0.0000304125f, 0.0000130529f },
		{ 0.0000463885f, -0.0022019070f, -0.0036943172f, -0.0029637526f, -0.0010946484f, 0.0000256099f, -0.0000541444f, 0.0000246446f },
		{ -0.0001484678f, -0.0010155809f, -0.0021381981f, -0.0015084865f, -0.0005415679f, -0.0000796084f, 0.0000303637f, -0.0000138088f },
		{ -0.0000607685f, -0.0005049132f, -0.0010059044f, -0.0005012621f, -0.0002072625f, 0.0000342216f, -0.0000406828f, 0.0000184683f },
		{ -0.0001542769f, 0.0001072031f, -0.0002295087f, -0.0002477811f, -0.0000283801f, -0.0000856824f, 0.0000392268f, -0.0000175150f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000219925f, -0.0001699188f, -0.0003045915f, -0.0002308751f, -0.0001074467f, 0.0000599415f, -0.0000424279f, 0.0000190600f },
		{ 0.0001066530f, -0.0004657167f, -0.0009957485f, -0.0004493474f, -0.0001779240f, 0.0000269828f, -0.0000285775f, 0.0000145431f },
		{ 0.0000107899f, -0.0011472529f, -0.0018705572f, -0.0009535142f, -0.0001354004f, -0.0000128989f, -0.0000031217f, 0.0000012586f },
		{ -0.0002142069f, -0.0021421101f, -0.0028470021f, -0.0013235284f, -0.0001936128f, -0.0000502340f, 0.0000217933f, -0.0000101983f },
		{ -0.0002733089f, -0.0025378826f, -0.0038993123f, -0.0021868503f, -0.0003382398f, -0.0000221634f, 0.0000057388f, -0.0000035646f },
		{ -0.0004643685f, -0.0039200895f, -0.0052867187f, -0.0025684991f, -0.0004893410f, -0.0000313169f, -0.0000014823f, -0.0000000657f },
		{ -0.0007737681f, -0.0051166123f, -0.0066807176f, -0.0033648896f, -0.0004565530f, -0.0000815997f, 0.0000263549f, -0.0000122675f },
		{ -0.0007520079f, -0.0062875410f, -0.0083176189f, -0.0039514278f, -0.0005873055f, -0.0000401852f, 0.0000036200f, -0.0000011414f },
		{ -0.0008597412f, -0.0079311435f, -0.0097832972f, -0.0046075107f, -0.0008558894f, 0.0000241430f, -0.0000372251f, 0.0000171418f },
		{ -0.0011489207f, -0.0093946027f, -0.0115687552f, -0.0050777208f, -0.0009541737f, -0.0000078085f, -0.0000267406f, 0.0000123636f },
	},
	{
		{ 0.0025083272f, 0.0016622664f, -0.0296956467f, -0.0738794832f, -0.0811482788f, -0.0371542571f, -0.0013320680f, -0.0003080453f },
		{ 0.0009774727f, -0.0044559785f, -0.0331734549f, -0.0640632766f, -0.0541029207f, -0.0136920264f, 0.0005841752f, -0.0002856084f },
		{ 0.0004355567f, -0.0073085214f, -0.0317041450f, -0.0517912403f, -0.0350926525f, -0.0056858987f, 0.0000753451f, -0.0000091613f },
		{ 0.0002255933f, -0.0085477413f, -0.0286284971f, -0.0401790835f, -0.0226820114f, -0.0024374244f, -0.0001523223f, 0.0000874215f },
		{ -0.0002854170f, -0.0082724750f, -0.0247566291f, -0.0300597945f, -0.0143889826f, -0.0011737117f, -0.0001124007f, 0.0000612744f },
		{ -0.0004683724f, -0.0073025182f, -0.0195978413f, -0.0220156432f, -0.0093275667f, -0.0005996039f, -0.0000777617f, 0.0000401833f },
		{ -0.0004112485f, -0.0063053819f, -0.0156525001f, -0.0157693787f, -0.0057814258f, -0.0003212420f, -0.0000485897f, 0.0000231046f },
		{ -0.0004794924f, -0.0057720025f, -0.0117887923f, -0.0105215799f, -0.0036085619f, -0.0001670854f, -0.0000316054f, 0.0000159023f },
		{ -0.0003937110f, -0.0045539227f, -0.0083377189f, -0.0071222419f, -0.0021816329f, -0.0000638606f, -0.0000488979f, 0.0000233475f },
		{ -0.0004362240f, -0.0028691186f, -0.0054316607f, -0.0043862843f, -0.0011944413f, -0.0000272541f, -0.0000139949f, 0.0000045573f },
		{ -0.0002748582f, -0.0018674871f, -0.0033626015f, -0.0024618440f, -0.0006094637f, 0.0000081231f, -0.0000184747f, 0.0000082071f },
		{ -0.0000429148f, -0.0009360355f, -0.0017270813f, -0.0014400135f, -0.0004921281f, -0.0000264903f, -0.0000142429f, 0.0000070267f },
		{ -0.0000749595f, -0.0008645559f, -0.0010900554f, -0.0004510374f, -0.0001397312f, 0.0000167365f, -0.0000199964f, 0.0000090151f },
		{ -0.0000219925f, -0.0001699188f, -0.0003045915f, -0.0002308751f, -0.0001074467f, 0.0000599415f, -0.0000424279f, 0.0000190600f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000486643f, -0.0001548541f, -0.0002235489f, -0.0001666466f, -0.0000335162f, -0.0000251760f, 0.0000129472f, -0.0000062636f },
		{ 0.0000117703f, -0.0004061731f, -0.0008164875f, -0.0005020916f, -0.0000774887f, -0.0000054746f, -0.0000099273f, 0.0000045074f },
		{ -0.0002152702f, -0.0005089777f, -0.0010283540f, -0.0009321174f, -0.0001044646f, -0.0000673382f, 0.0000350051f, -0.0000161872f },
		{ -0.0000823338f, -0.0014344119f, -0.0023493138f, -0.0012318884f, -0.0002633220f, -0.0000286337f, 0.0000000743f, -0.0000007874f },
		{ -0.0003989637f, -0.0025850338f, -0.0034704316f, -0.0017986608f, -0.0002235864f, -0.0000158038f, 0.0000116350f, -0.0000060367f },
		{ -0.0003924385f, -0.0034073054f, -0.0045677537f, -0.0023690786f, -0.0004015325f, -0.0000033139f, -0.0000046600f, 0.0000020457f },
		{ -0.0004959867f, -0.0051914755f, -0.0062542577f, -0.0026316905f, -0.0004850312f, 0.0000048136f, -0.0000237822f, 0.0000114792f },
		{ -0.0006025310f, -0.0065028473f, -0.0075228239f, -0.0030825728f, -0.0006387887f, 0.0000269709f, -0.0000419434f, 0.0000198705f },
		{ -0.0008168572f, -0.0072445922f, -0.0089160967f, -0.0040048880f, -0.0006061578f, -0.0000320544f, -0.0000017919f, 0.0000004846f },
	},
	{
		{ 0.0023487289f, -0.0007138837f, -0.0358591535f, -0.0811046267f, -0.0849444089f, -0.0375564701f, -0.0011947264f, -0.0003461836f },
		{ 0.0010248911f, -0.0070546465f, -0.0389593451f, -0.0704689151f, -0.0564588798f, -0.0135308664f, 0.0005302316f, -0.0002542101f },
		{ 0.0003921251f, -0.0098509496f, -0.0373911760f, -0.0573096689f, -0.0367904369f, -0.0055929014f, 0.0000368600f, 0.0000077731f },
		{ -0.0000000614f, -0.0104425005f, -0.0339712213f, -0.0448030006f, -0.0239356581f, -0.0024775312f, -0.0001424209f, 0.0000828225f },
		{ -0.0004458491f, -0.0106310303f, -0.0287971564f, -0.0338478906f, -0.0155888513f, -0.0012167250f, -0.0001515939f, 0.0000798484f },
		{ -0.0005757519f, -0.0097581705f, -0.0237061250f, -0.0250734698f, -0.0100125679f, -0.0006009477f, -0.0000934591f, 0.0000479818f },
		{ -0.0007409176f, -0.0086944517f, -0.0190352006f, -0.0182216970f, -0.0062510063f, -0.0002849287f, -0.0000614142f, 0.0000287299f },
		{ -0.0005827587f, -0.0079058190f, -0.0145130257f, -0.0127709615f, -0.0041071409f, -0.0000721608f, -0.0000895604f, 0.0000408520f },
		{ -0.0001487151f, -0.0057908819f, -0.0110840883f, -0.0088261383f, -0.0025513878f, -0.0000826598f, -0.0000458692f, 0.0000199062f },
		{ -0.0005051484f, -0.0044350465f, -0.0076589009f, -0.0053807719f, -0.0014763909f, -0.0000552395f, -0.0000342144f, 0.0000150339f },
		{ -0.0005838388f, -0.0035109117f, -0.0048901364f, -0.0034966930f, -0.0008538904f, 0.0000033181f, -0.0000196208f, 0.0000091401f },
		{ -0.0002254489f, -0.0020245856f, -0.0031108089f, -0.0019885461f, -0.0006021597f, -0.0000468778f, -0.0000025606f, 0.0000005660f },
		{ -0.0001405775f, -0.0015247364f, -0.0015838514f, -0.0008492705f, -0.0004115830f, 0.0000205332f, -0.0000395932f, 0.0000193438f },
		{ 0.0001066530f, -0.0004657167f, -0.0009957485f, -0.0004493474f, -0.0001779240f, 0.0000269828f, -0.0000285775f, 0.0000145431f },
		{ -0.0000486643f, -0.0001548541f, -0.0002235489f, -0.0001666466f, -0.0000335162f, -0.0000251760f, 0.0000129472f, -0.0000062636f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000635853f, -0.0001434486f, 0.0000060365f, -0.0003328074f, 0.0000632517f, -0.0000413539f, 0.0000144394f, -0.0000063925f },
		{ 0.0002176480f, -0.0005791530f, -0.0008393411f, -0.0003857878f, -0.0001199844f, 0.0000215084f, -0.0000176529f, 0.0000089743f },
		{ -0.0000267980f, -0.0010191970f, -0.0011662641f, -0.0006407215f, -0.0001077165f, -0.0000373456f, 0.0000217066f, -0.0000095845f },
		{ -0.0002334051f, -0.0018892674f, -0.0020245538f, -0.0010612070f, -0.0001979621f, -0.0000060764f, -0.0000118121f, 0.0000053491f },
		{ -0.0004220302f, -0.0025453889f, -0.0034729072f, -0.0015262055f, -0.0002760333f, 0.0000041146f, -0.0000115418f, 0.0000057544f },
		{ -0.0003593351f, -0.0034637287f, -0.0042294136f, -0.0020435817f, -0.0002705950f, 0.0000029348f, -0.0000030426f, 0.0000015380f },
		{ -0.0002737278f, -0.0047658120f, -0.0053208086f, -0.0022584998f, -0.0004265035f, 0.0000514289f, -0.0000355878f, 0.0000163648f },
		{ -0.0007240718f, -0.0060192412f, -0.0062872606f, -0.0026913865f, -0.0004671233f, 0.0000361125f, -0.0000185919f, 0.0000085964f },
	},
	{
		{ 0.0022271697f, -0.0030945881f, -0.0417564043f, -0.0885237739f, -0.0887703695f, -0.0377272616f, -0.0011104448f, -0.0003523488f },
		{ 0.0008130451f, -0.0091048125f, -0.0448735570f, -0.0772078935f, -0.0587343530f, -0.0133786898f, 0.0005300630f, -0.0002481184f },
		{ -0.0000086869f, -0.0119730460f, -0.0427289848f, -0.0626393966f, -0.0382911664f, -0.0055124471f, -0.0000022020f, 0.0000265703f },
		{ -0.0004740104f, -0.0130639120f, -0.0388002876f, -0.0490662874f, -0.0251084438f, -0.0024281161f, -0.0001795882f, 0.0000992682f },
		{ -0.0005862061f, -0.0130954669f, -0.0335057613f, -0.0374066479f, -0.0163089654f, -0.0011995550f, -0.0001366881f, 0.0000712638f },
		{ -0.0008791218f, -0.0117029059f, -0.0279180117f, -0.0284504257f, -0.0104365615f, -0.0006260781f, -0.0000489096f, 0.0000257479f },
		{ -0.0011950171f, -0.0104906163f, -0.0224708831f, -0.0208773012f, -0.0069236024f, -0.0003358438f, -0.0000377985f, 0.0000177711f },
		{ -0.0008913215f, -0.0094764264f, -0.0179283345f, -0.0149396069f, -0.0046873119f, -0.0000870392f, -0.0000972393f, 0.0000440969f },
		{ -0.0007025988f, -0.0073079063f, -0.0136196051f, -0.0107693776f, -0.0028476523f, -0.0001145903f, -0.0000125693f, 0.0000041083f },
		{ -0.0005787552f, -0.0059458378f, -0.0101970011f, -0.0069969759f, -0.0017644589f, -0.0000622066f, -0.0000170862f, 0.0000080368f },
		{ -0.0006953601f, -0.0042236501f, -0.0069578074f, -0.0047919847f, -0.0011481029f, -0.0001060285f, 0.0000376893f, -0.0000168376f },
		{ -0.0000810828f, -0.0033329168f, -0.0045024599f, -0.0029952086f, -0.0007232163f, 0.0000622125f, -0.0000641684f, 0.0000285747f },
		{ 0.0001226594f, -0.0021593839f, -0.0034091227f, -0.0016719092f, -0.0004566004f, 0.0000191330f, -0.0000442699f, 0.0000200740f },
		{ 0.0000107899f, -0.0011472529f, -0.0018705572f, -0.0009535142f, -0.0001354004f, -0.0000128989f, -0.0000031217f, 0.0000012586f },
		{ 0.0000117703f, -0.0004061731f, -0.0008164875f, -0.0005020916f, -0.0000774887f, -0.0000054746f, -0.0000099273f, 0.0000045074f },
		{ 0.0000635853f, -0.0001434486f, 0.0000060365f, -0.0003328074f, 0.0000632517f, -0.0000413539f, 0.0000144394f, -0.0000063925f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0001154478f, -0.0001371459f, 0.0000075583f, -0.0000662710f, -0.0001267440f, -0.0000026123f, -0.0000036421f, 0.0000017630f },
		{ -0.0000567550f, -0.0006074444f, -0.0005244814f, -0.0001593119f, -0.0000568093f, 0.0000485655f, -0.0000316976f, 0.0000147280f },
		{ -0.0000048254f, -0.0011059869f, -0.0012462355f, -0.0005797539f, -0.0000728041f, -0.0000035658f, -0.0000039963f, 0.0000014536f },
		{ -0.0003071411f, -0.0015269605f, -0.0014497651f, -0.0011209527f, -0.0001311674f, -0.0000022006f, 0.0000048318f, -0.0000032809f },
		{ -0.0003880425f, -0.0021074900f, -0.0027303079f, -0.0010833018f, -0.0002166971f, -0.0000057914f, 0.0000033336f, -0.0000011323f },
		{ -0.0003926670f, -0.0033421926f, -0.0037615148f, -0.0014921835f, -0.0001837626f, -0.0000204266f, 0.0000055146f, -0.0000024746f },
		{ -0.0005879159f, -0.0042970265f, -0.0045265783f, -0.0020957502f, -0.0002735003f, 0.0000267140f, -0.0000076065f, 0.0000031526f },
	},
	{
		{ 0.0020353457f, -0.0059177110f, -0.0483449073f, -0.0953803362f, -0.0920927873f, -0.0379614906f, -0.0009987074f, -0.0003765816f },
		{ 0.0005576664f, -0.0125184639f, -0.0512483573f, -0.0827877185f, -0.0607593257f, -0.0131306213f, 0.0004732146f, -0.0002147153f },
		{ -0.0002109559f, -0.0152011015f, -0.0486317781f, -0.0673540428f, -0.0395026230f, -0.0053573554f, 0.0000041310f, 0.0000250802f },
		{ -0.0004991718f, -0.0160835112f, -0.0440507340f, -0.0527906300f, -0.0260929854f, -0.0023688769f, -0.0002081179f, 0.0001128240f },
		{ -0.0008929014f, -0.0156702201f, -0.0380593914f, -0.0409752365f, -0.0171044889f, -0.0011405075f, -0.0001843378f, 0.0000915703f },
		{ -0.0011722908f, -0.0143052143f, -0.0319585437f, -0.0312013700f, -0.0112603171f, -0.0005756092f, -0.0001182298f, 0.0000564562f },
		{ -0.0010054251f, -0.0132458597f, -0.0264694399f, -0.0231715779f, -0.0074337861f, -0.0003207798f, -0.0000854451f, 0.0000402556f },
		{ -0.0009902041f, -0.0115445750f, -0.0211342824f, -0.0170562468f, -0.0049094785f, -0.0001617029f, -0.0000665283f, 0.0000295571f },
		{ -0.0008225545f, -0.0094291037f, -0.0165312206f, -0.0120651249f, -0.0032053662f, -0.0000919494f, -0.0000416832f, 0.0000184910f },
		{ -0.0008037741f, -0.0071142225f, -0.0126150864f, -0.0088230629f, -0.0020714483f, -0.0001071901f, 0.0000088672f, -0.0000050791f },
		{ -0.0005475042f, -0.0060046488f, -0.0092497778f, -0.0058304598f, -0.0014090229f, -0.0000336641f, -0.0000273834f, 0.0000118612f },
		{ -0.0001848309f, -0.0043243536f, -0.0066586803f, -0.0035857204f, -0.0007761920f, 0.0000034095f, -0.0000294249f, 0.0000130022f },
		{ -0.0003724996f, -0.0025591326f, -0.0044519681f, -0.0027190243f, -0.0004113753f, -0.0000496236f, 0.0000347522f, -0.0000163026f },
		{ -0.0002142069f, -0.0021421101f, -0.0028470021f, -0.0013235284f, -0.0001936128f, -0.0000502340f, 0.0000217933f, -0.0000101983f },
		{ -0.0002152702f, -0.0005089777f, -0.0010283540f, -0.0009321174f, -0.0001044646f, -0.0000673382f, 0.0000350051f, -0.0000161872f },
		{ 0.0002176480f, -0.0005791530f, -0.0008393411f, -0.0003857878f, -0.0001199844f, 0.0000215084f, -0.0000176529f, 0.0000089743f },
		{ -0.0001154478f, -0.0001371459f, 0.0000075583f, -0.0000662710f, -0.0001267440f, -0.0000026123f, -0.0000036421f, 0.0000017630f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000343631f, -0.0002501390f, -0.0002968182f, -0.0001812142f, 0.0000206950f, -0.0000103321f, 0.0000055469f, -0.0000026756f },
		{ 0.0000175421f, -0.0004772877f, -0.0001325733f, -0.0002453682f, -0.0000656244f, 0.0000229312f, -0.0000046795f, 0.0000021554f },
		{ -0.0000929133f, -0.0009998646f, -0.0010866045f, -0.0004600490f, -0.0001143984f, 0.0000264766f, -0.0000044036f, 0.0000024070f },
		{ -0.0003150033f, -0.0015275844f, -0.0016438771f, -0.0008181521f, -0.0000868582f, 0.0000114581f, 0.0000012614f, -0.0000009335f },
		{ -0.0004031518f, -0.0021954377f, -0.0026877491f, -0.0008877325f, -0.0000835128f, -0.0000314038f, 0.0000245388f, -0.0000113259f },
		{ -0.0004502310f, -0.0028956304f, -0.0034528408f, -0.0011564860f, -0.0002306584f, 0.0000167617f, -0.0000176397f, 0.0000081423f },
	},
	{
		{ 0.0018688146f, -0.0087004819f, -0.0546681157f, -0.1020049674f, -0.0951664878f, -0.0380609580f, -0.0009215210f, -0.0003802327f },
		{ 0.0005086968f, -0.0152521235f, -0.0577964361f, -0.0883433273f, -0.0623757232f, -0.0128709953f, 0.0004114787f, -0.0001821650f },
		{ -0.0003642039f, -0.0177069818f, -0.0546350194f, -0.0718455950f, -0.0407418734f, -0.0053030484f, -0.0000623320f, 0.0000553278f },
		{ -0.0010844383f, -0.0192056147f, -0.0494491391f, -0.0560929177f, -0.0268697257f, -0.0023388488f, -0.0002159104f, 0.0001172406f },
		{ -0.0013138157f, -0.0181211437f, -0.0429982675f, -0.0441823012f, -0.0176092430f, -0.0012390260f, -0.0000957184f, 0.0000525980f },
		{ -0.0014893119f, -0.0172605425f, -0.0364038800f, -0.0337954524f, -0.0116929653f, -0.0006479142f, -0.0000754126f, 0.0000379771f },
		{ -0.0013025968f, -0.0158175742f, -0.0306000778f, -0.0254276828f, -0.0077969888f, -0.0002686216f, -0.0001043855f, 0.0000487925f },
		{ -0.0014210681f, -0.0131984653f, -0.0245049593f, -0.0191410025f, -0.0052362212f, -0.0001944265f, -0.0000278934f, 0.0000110990f },
		{ -0.0011398693f, -0.0115316406f, -0.0194422935f, -0.0137093106f, -0.0034546649f, -0.0001112045f, -0.0000244029f, 0.0000107748f },
		{ -0.0010697522f, -0.0093203957f, -0.0152399537f, -0.0101993077f, -0.0023047423f, -0.0001037607f, -0.0000067840f, 0.0000020017f },
		{ -0.0006875011f, -0.0075642636f, -0.0116168408f, -0.0068471046f, -0.0016361362f, 0.0000058613f, -0.0000427592f, 0.0000195480f },
		{ -0.0005685983f, -0.0054630074f, -0.0086703637f, -0.0047092854f, -0.0010371316f, 0.0000104657f, -0.0000305495f, 0.0000138298f },
		{ -0.0006367664f, -0.0042247501f, -0.0057157511f, -0.0032184148f, -0.0005261459f, -0.0000126446f, 0.0000044445f, -0.0000034062f },
		{ -0.0002733089f, -0.0025378826f, -0.0038993123f, -0.0021868503f, -0.0003382398f, -0.0000221634f, 0.0000057388f, -0.0000035646f },
		{ -0.0000823338f, -0.0014344119f, -0.0023493138f, -0.0012318884f, -0.0002633220f, -0.0000286337f, 0.0000000743f, -0.0000007874f },
		{ -0.0000267980f, -0.0010191970f, -0.0011662641f, -0.0006407215f, -0.0001077165f, -0.0000373456f, 0.0000217066f, -0.0000095845f },
		{ -0.0000567550f, -0.0006074444f, -0.0005244814f, -0.0001593119f, -0.0000568093f, 0.0000485655f, -0.0000316976f, 0.0000147280f },
		{ -0.0000343631f, -0.0002501390f, -0.0002968182f, -0.0001812142f, 0.0000206950f, -0.0000103321f, 0.0000055469f, -0.0000026756f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0001356927f, -0.0003640839f, -0.0001405662f, 0.0002073048f, -0.0000186955f, -0.0000016685f, -0.0000063151f, 0.0000032019f },
		{ -0.0001687863f, -0.0003677548f, -0.0001183163f, -0.0004217003f, -0.0000462053f, -0.0000109588f, 0.0000147618f, -0.0000072012f },
		{ -0.0001186395f, -0.0007276968f, -0.0011264640f, -0.0004783593f, -0.0000432911f, 0.0000238283f, -0.0000012828f, 0.0000004223f },
		{ -0.0003719482f, -0.0014683741f, -0.0012933238f, -0.0006755246f, -0.0000547846f, -0.0000071540f, 0.0000054246f, -0.0000026958f },
		{ -0.0002648938f, -0.0017322979f, -0.0022177893f, -0.0009903573f, -0.0001299446f, -0.0000264256f, 0.0000090213f, -0.0000043472f },
	},
	{
		{ 0.0017734278f, -0.0116276008f, -0.0613422481f, -0.1084158757f, -0.0981585052f, -0.0380510441f, -0.0008280833f, -0.0003893418f },
		{ 0.0000950055f, -0.0183074403f, -0.0638587993f, -0.0938273095f, -0.0635582653f, -0.0127702205f, 0.0003718157f, -0.0001646260f },
		{ -0.0006721666f, -0.0209653579f, -0.0603159770f, -0.0763849833f, -0.0416624720f, -0.0052276491f, -0.0001078210f, 0.0000743507f },
		{ -0.0011609999f, -0.0217532148f, -0.0545834226f, -0.0603834447f, -0.0276350061f, -0.0023619397f, -0.0002061810f, 0.0001116134f },
		{ -0.0016459813f, -0.0209565902f, -0.0476831600f, -0.0473871585f, -0.0183365201f, -0.0011067636f, -0.0001601486f, 0.0000799965f },
		{ -0.0017475253f, -0.0198794606f, -0.0408107996f, -0.0363760842f, -0.0122399426f, -0.0005958884f, -0.0001043239f, 0.0000496586f },
		{ -0.0014353696f, -0.0181425936f, -0.0341190799f, -0.0276534673f, -0.0084869667f, -0.0002307499f, -0.0001387704f, 0.0000635961f },
		{ -0.0013864325f, -0.0154945706f, -0.0279337040f, -0.0211859729f, -0.0055590700f, -0.0002072028f, -0.0000300201f, 0.0000133053f },
		{ -0.0015974701f, -0.0134290381f, -0.0225056220f, -0.0158733822f, -0.0037547817f, -0.0001599456f, -0.0000023337f, -0.0000011154f },
		{ -0.0010778172f, -0.0114198537f, -0.0178751649f, -0.0111264490f, -0.0027582045f, -0.0000318465f, -0.0000590099f, 0.0000263890f },
		{ -0.0009377027f, -0.0088260239f, -0.0136834126f, -0.0083338986f, -0.0016919699f, -0.0000642233f, -0.0000042830f, 0.0000025843f },
		{ -0.0008419497f, -0.0071696512f, -0.0103304428f, -0.0059289707f, -0.0009245607f, -0.0000989930f, 0.0000370960f, -0.0000174453f },
		{ -0.0006035017f, -0.0054503295f, -0.0076217147f, -0.0040148459f, -0.0005448100f, -0.0000507481f, 0.0000260997f, -0.0000118847f },
		{ -0.0004643685f, -0.0039200895f, -0.0052867187f, -0.0025684991f, -0.0004893410f, -0.0000313169f, -0.0000014823f, -0.0000000657f },
		{ -0.0003989637f, -0.0025850338f, -0.0034704316f, -0.0017986608f, -0.0002235864f, -0.0000158038f, 0.0000116350f, -0.0000060367f },
		{ -0.0002334051f, -0.0018892674f, -0.0020245538f, -0.0010612070f, -0.0001979621f, -0.0000060764f, -0.0000118121f, 0.0000053491f },
		{ -0.0000048254f, -0.0011059869f, -0.0012462355f, -0.0005797539f, -0.0000728041f, -0.0000035658f, -0.0000039963f, 0.0000014536f },
		{ 0.0000175421f, -0.0004772877f, -0.0001325733f, -0.0002453682f, -0.0000656244f, 0.0000229312f, -0.0000046795f, 0.0000021554f },
		{ 0.0001356927f, -0.0003640839f, -0.0001405662f, 0.0002073048f, -0.0000186955f, -0.0000016685f, -0.0000063151f, 0.0000032019f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000734338f, 0.0000183439f, -0.0000286008f, -0.0001234078f, -0.0000098232f, 0.0000038271f, -0.0000057545f, 0.0000024326f },
		{ -0.0001379985f, -0.0003593373f, -0.0002214936f, -0.0002592464f, -0.0000225647f, -0.0000130628f, 0.0000074997f, -0.0000036963f },
		{ -0.0001088208f, -0.0005078404f, -0.0008580553f, -0.0003993276f, -0.0000464861f, -0.0000363168f, 0.0000240878f, -0.0000114800f },
		{ -0.0000442326f, -0.0013202301f, -0.0012947346f, -0.0006540317f, -0.0000984317f, -0.0000027832f, -0.0000069771f, 0.0000033201f },
	},
	{
		{ 0.0011898017f, -0.0144774485f, -0.0676264092f, -0.1146499166f, -0.1005824271f, -0.0381954980f, -0.0006836257f, -0.0004282457f },
		{ -0.0000673004f, -0.0217228061f, -0.0702120301f, -0.0985750297f, -0.0648387044f, -0.0124892619f, 0.0002733885f, -0.0001178345f },
		{ -0.0009334308f, -0.0245725237f, -0.0660476472f, -0.0803042734f, -0.0423993605f, -0.0051388246f, -0.0001108479f, 0.0000773861f },
		{ -0.0015209542f, -0.0251418793f, -0.0595790821f, -0.0635514733f, -0.0281120279f, -0.0023094441f, -0.0002148478f, 0.0001130427f },
		{ -0.0018954471f, -0.0238422697f, -0.0522412940f, -0.0502659170f, -0.0187780206f, -0.0011346394f, -0.0001480355f, 0.0000747781f },
		{ -0.0018182250f, -0.0231513878f, -0.0449166972f, -0.0384556615f, -0.0128761826f, -0.0005821226f, -0.0001382139f, 0.0000658371f },
		{ -0.0017445891f, -0.0205671513f, -0.0377631147f, -0.0300749697f, -0.0088439887f, -0.0002787862f, -0.0001272253f, 0.0000582449f },
		{ -0.0020222959f, -0.0176612957f, -0.0315519719f, -0.0229199742f, -0.0057057359f, -0.0002774717f, 0.0000110743f, -0.0000057611f },
		{ -0.0018701281f, -0.0155355175f, -0.0252875778f, -0.0172049142f, -0.0041409795f, -0.0001293880f, -0.0000090986f, 0.0000029973f },
		{ -0.0013643363f, -0.0133255220f, -0.0205073482f, -0.0127180640f, -0.0028079858f, -0.0000430388f, -0.0000485189f, 0.0000213260f },
		{ -0.0011626057f, -0.0109752883f, -0.0166292785f, -0.0093239127f, -0.0018637151f, -0.0000597338f, -0.0000061847f, 0.0000025415f },
		{ -0.0009677267f, -0.0090993638f, -0.0122209830f, -0.0068376146f, -0.0012026245f, -0.0000277582f, -0.0000058429f, 0.0000019971f },
		{ -0.0006829786f, -0.0067374802f, -0.0088023085f, -0.0048766832f, -0.0009499181f, -0.0000288997f, -0.0000025756f, 0.0000009633f },
		{ -0.0007737681f, -0.0051166123f, -0.0066807176f, -0.0033648896f, -0.0004565530f, -0.0000815997f, 0.0000263549f, -0.0000122675f },
		{ -0.0003924385f, -0.0034073054f, -0.0045677537f, -0.0023690786f, -0.0004015325f, -0.0000033139f, -0.0000046600f, 0.0000020457f },
		{ -0.0004220302f, -0.0025453889f, -0.0034729072f, -0.0015262055f, -0.0002760333f, 0.0000041146f, -0.0000115418f, 0.0000057544f },
		{ -0.0003071411f, -0.0015269605f, -0.0014497651f, -0.0011209527f, -0.0001311674f, -0.0000022006f, 0.0000048318f, -0.0000032809f },
		{ -0.0000929133f, -0.0009998646f, -0.0010866045f, -0.0004600490f, -0.0001143984f, 0.0000264766f, -0.0000044036f, 0.0000024070f },
		{ -0.0001687863f, -0.0003677548f, -0.0001183163f, -0.0004217003f, -0.0000462053f, -0.0000109588f, 0.0000147618f, -0.0000072012f },
		{ 0.0000734338f, 0.0000183439f, -0.0000286008f, -0.0001234078f, -0.0000098232f, 0.0000038271f, -0.0000057545f, 0.0000024326f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000293813f, 0.0000523191f, -0.0000369043f, -0.0002430099f, 0.0000325319f, -0.0000105663f, 0.0000092678f, -0.0000045219f },
		{ 0.0000295216f, -0.0003478152f, -0.0004574265f, -0.0001879530f, -0.0000496361f, 0.0000050157f, 0.0000016336f, -0.0000011762f },
		{ -0.0001264282f, -0.0005041117f, -0.0003100260f, -0.0004880085f, -0.0000071201f, 0.0000074308f, -0.0000031771f, 0.0000008211f },
	},
	{
		{ 0.0010991171f, -0.0175465382f, -0.0742864581f, -0.1202213877f, -0.1031201910f, -0.0381723831f, -0.0005665603f, -0.0004521686f },
		{ -0.0005257023f, -0.0251313452f, -0.0756913477f, -0.1035662414f, -0.0660347138f, -0.0123029994f, 0.0002630118f, -0.0001094887f },
		{ -0.0013358080f, -0.0279127652f, -0.0716090063f, -0.0840743237f, -0.0431522987f, -0.0049300103f, -0.0001919121f, 0.0001140287f },
		{ -0.0017677964f, -0.0284758917f, -0.0646480566f, -0.0668238242f, -0.0286942898f, -0.0022650143f, -0.0002437632f, 0.0001255721f },
		{ -0.0022816796f, -0.0271082196f, -0.0566440213f, -0.0525913559f, -0.0192275298f, -0.0011120452f, -0.0001671713f, 0.0000822266f },
		{ -0.0021507079f, -0.0260855548f, -0.0493601935f, -0.0410097547f, -0.0131461428f, -0.0005478882f, -0.0001510920f, 0.0000729908f },
		{ -0.0021930151f, -0.0229273657f, -0.0417093130f, -0.0321235363f, -0.0088973670f, -0.0003640298f, -0.0000613020f, 0.0000273588f },
		{ -0.0022253795f, -0.0205207187f, -0.0345005355f, -0.0246034181f, -0.0062616936f, -0.0002256396f, -0.0000301194f, 0.0000127321f },
		{ -0.0016985446f, -0.0182652146f, -0.0288023296f, -0.0185649524f, -0.0042770398f, -0.0001717900f, -0.0000168679f, 0.0000071367f },
		{ -0.0015816170f, -0.0153798975f, -0.0233502364f, -0.0139283636f, -0.0029695835f, -0.0000582238f, -0.0000349001f, 0.0000150575f },
		{ -0.0015369033f, -0.0129430907f, -0.0184353469f, -0.0106337245f, -0.0020004812f, -0.0000457862f, -0.0000169965f, 0.0000067868f },
		{ -0.0011709312f, -0.0107006188f, -0.0143421838f, -0.0077024505f, -0.0015434371f, -0.0000093275f, -0.0000374469f, 0.0000168019f },
		{ -0.0010537006f, -0.0083659129f, -0.0108162718f, -0.0054227815f, -0.0011424858f, 0.0000082157f, -0.0000413638f, 0.0000192794f },
		{ -0.0007520079f, -0.0062875410f, -0.0083176189f, -0.0039514278f, -0.0005873055f, -0.0000401852f, 0.0000036200f, -0.0000011414f },
		{ -0.0004959867f, -0.0051914755f, -0.0062542577f, -0.0026316905f, -0.0004850312f, 0.0000048136f, -0.0000237822f, 0.0000114792f },
		{ -0.0003593351f, -0.0034637287f, -0.0042294136f, -0.0020435817f, -0.0002705950f, 0.0000029348f, -0.0000030426f, 0.0000015380f },
		{ -0.0003880425f, -0.0021074900f, -0.0027303079f, -0.0010833018f, -0.0002166971f, -0.0000057914f, 0.0000033336f, -0.0000011323f },
		{ -0.0003150033f, -0.0015275844f, -0.0016438771f, -0.0008181521f, -0.0000868582f, 0.0000114581f, 0.0000012614f, -0.0000009335f },
		{ -0.0001186395f, -0.0007276968f, -0.0011264640f, -0.0004783593f, -0.0000432911f, 0.0000238283f, -0.0000012828f, 0.0000004223f },
		{ -0.0001379985f, -0.0003593373f, -0.0002214936f, -0.0002592464f, -0.0000225647f, -0.0000130628f, 0.0000074997f, -0.0000036963f },
		{ 0.0000293813f, 0.0000523191f, -0.0000369043f, -0.0002430099f, 0.0000325319f, -0.0000105663f, 0.0000092678f, -0.0000045219f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000605731f, -0.0003094288f, 0.0000739814f, -0.0001181738f, 0.0000875849f, -0.0000249570f, 0.0000119480f, -0.0000058495f },
		{ -0.0000316374f, -0.0004033390f, -0.0002800512f, -0.0001833027f, -0.0000221368f, 0.0000054099f, -0.0000129238f, 0.0000056971f },
	},
	{
		{ 0.0008112423f, -0.0210330914f, -0.0805916817f, -0.1256739232f, -0.1052584223f, -0.0380745262f, -0.0004827899f, -0.0004598796f },
		{ -0.0008660915f, -0.0284699237f, -0.0822408334f, -0.1078761872f, -0.0668128903f, -0.0121226050f, 0.0002486008f, -0.0001029261f },
		{ -0.0013719324f, -0.0309163006f, -0.0772911276f, -0.0875814908f, -0.0435716353f, -0.0049611899f, -0.0001684638f, 0.0001023129f },
		{ -0.0022497236f, -0.0317056748f, -0.0693586238f, -0.0698365833f, -0.0290125045f, -0.0022595585f, -0.0002313045f, 0.0001206464f },
		{ -0.0025526969f, -0.0304057602f, -0.0610224418f, -0.0552767354f, -0.0196871460f, -0.0010811617f, -0.0001878648f, 0.0000919484f },
		{ -0.0023223258f, -0.0282548369f, -0.0534378663f, -0.0433197519f, -0.0134875791f, -0.0004974475f, -0.0001713228f, 0.0000808794f },
		{ -0.0023710378f, -0.0259292191f, -0.0454552170f, -0.0335894712f, -0.0093051985f, -0.0003111105f, -0.0001077974f, 0.0000484422f },
		{ -0.0025572237f, -0.0229124605f, -0.0380365450f, -0.0261675873f, -0.0064864413f, -0.0002355635f, -0.0000226164f, 0.0000096901f },
		{ -0.0019997434f, -0.0201831538f, -0.0321940737f, -0.0199241686f, -0.0044034957f, -0.0000888149f, -0.0000562889f, 0.0000244376f },
		{ -0.0017764021f, -0.0175093493f, -0.0264951536f, -0.0151599215f, -0.0030258841f, -0.0001026968f, -0.0000165057f, 0.0000073163f },
		{ -0.0017563685f, -0.0148065558f, -0.0208302379f, -0.0115460989f, -0.0022173243f, -0.0000040698f, -0.0000368804f, 0.0000165881f },
		{ -0.0011162162f, -0.0124820799f, -0.0166434481f, -0.0086649735f, -0.0016068459f, 0.0000068586f, -0.0000507258f, 0.0000224763f },
		{ -0.0011926565f, -0.0099886742f, -0.0130534429f, -0.0062684901f, -0.0010480828f, -0.0000276126f, -0.0000122636f, 0.0000056842f },
		{ -0.0008597412f, -0.0079311435f, -0.0097832972f, -0.0046075107f, -0.0008558894f, 0.0000241430f, -0.0000372251f, 0.0000171418f },
		{ -0.0006025310f, -0.0065028473f, -0.0075228239f, -0.0030825728f, -0.0006387887f, 0.0000269709f, -0.0000419434f, 0.0000198705f },
		{ -0.0002737278f, -0.0047658120f, -0.0053208086f, -0.0022584998f, -0.0004265035f, 0.0000514289f, -0.0000355878f, 0.0000163648f },
		{ -0.0003926670f, -0.0033421926f, -0.0037615148f, -0.0014921835f, -0.0001837626f, -0.0000204266f, 0.0000055146f, -0.0000024746f },
		{ -0.0004031518f, -0.0021954377f, -0.0026877491f, -0.0008877325f, -0.0000835128f, -0.0000314038f, 0.0000245388f, -0.0000113259f },
		{ -0.0003719482f, -0.0014683741f, -0.0012933238f, -0.0006755246f, -0.0000547846f, -0.0000071540f, 0.0000054246f, -0.0000026958f },
		{ -0.0001088208f, -0.0005078404f, -0.0008580553f, -0.0003993276f, -0.0000464861f, -0.0000363168f, 0.0000240878f, -0.0000114800f },
		{ 0.0000295216f, -0.0003478152f, -0.0004574265f, -0.0001879530f, -0.0000496361f, 0.0000050157f, 0.0000016336f, -0.0000011762f },
		{ -0.0000605731f, -0.0003094288f, 0.0000739814f, -0.0001181738f, 0.0000875849f, -0.0000249570f, 0.0000119480f, -0.0000058495f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000519809f, -0.0001235695f, -0.0002519526f, -0.0001007603f, -0.0000710147f, -0.0000127637f, 0.0000004627f, 0.0000005354f },
	},
	{
		{ 0.0006895790f, -0.0245675418f, -0.0866879651f, -0.1309124200f, -0.1073623181f, -0.0379507804f, -0.0004241319f, -0.0004561000f },
		{ -0.0009761984f, -0.0317511699f, -0.0884633079f, -0.1124705008f, -0.0676376150f, -0.0118533426f, 0.0001913354f, -0.0000744266f },
		{ -0.0019850718f, -0.0344558787f, -0.0823600222f, -0.0907768788f, -0.0441309151f, -0.0048415666f, -0.0001984367f, 0.0001161618f },
		{ -0.0025266956f, -0.0347197143f, -0.0744807515f, -0.0726838023f, -0.0293514318f, -0.0022848534f, -0.0002057526f, 0.0001080862f },
		{ -0.0027611102f, -0.0332881038f, -0.0657977862f, -0.0574789393f, -0.0198617141f, -0.0010656662f, -0.0001957084f, 0.0000952998f },
		{ -0.0026690394f, -0.0314294818f, -0.0573047049f, -0.0453867383f, -0.0138981374f, -0.0005375114f, -0.0001802256f, 0.0000835338f },
		{ -0.0028360161f, -0.0288536925f, -0.0488933838f, -0.0353138997f, -0.0093997668f, -0.0003320990f, -0.0000891373f, 0.0000398416f },
		{ -0.0027150476f, -0.0252969048f, -0.0414077781f, -0.0277205800f, -0.0065935286f, -0.0002293462f, -0.0000322155f, 0.0000135025f },
		{ -0.0024203151f, -0.0226936980f, -0.0348480798f, -0.0213088378f, -0.0046627884f, -0.0001119658f, -0.0000452458f, 0.0000203761f },
		{ -0.0021124043f, -0.0197146188f, -0.0287206812f, -0.0164826169f, -0.0032445313f, -0.0000789067f, -0.0000243437f, 0.0000097516f },
		{ -0.0017530407f, -0.0169034790f, -0.0230306149f, -0.0126888584f, -0.0023706663f, -0.0000370808f, -0.0000343382f, 0.0000157609f },
		{ -0.0015771110f, -0.0143606070f, -0.0187819565f, -0.0094577610f, -0.0016724205f, -0.0000318586f, -0.0000275721f, 0.0000133397f },
		{ -0.0013120522f, -0.0114452336f, -0.0150187447f, -0.0071847401f, -0.0012358197f, -0.0000244901f, -0.0000247694f, 0.0000120729f },
		{ -0.0011489207f, -0.0093946027f, -0.0115687552f, -0.0050777208f, -0.0009541737f, -0.0000078085f, -0.0000267406f, 0.0000123636f },
		{ -0.0008168572f, -0.0072445922f, -0.0089160967f, -0.0040048880f, -0.0006061578f, -0.0000320544f, -0.0000017919f, 0.0000004846f },
		{ -0.0007240718f, -0.0060192412f, -0.0062872606f, -0.0026913865f, -0.0004671233f, 0.0000361125f, -0.0000185919f, 0.0000085964f },
		{ -0.0005879159f, -0.0042970265f, -0.0045265783f, -0.0020957502f, -0.0002735003f, 0.0000267140f, -0.0000076065f, 0.0000031526f },
		{ -0.0004502310f, -0.0028956304f, -0.0034528408f, -0.0011564860f, -0.0002306584f, 0.0000167617f, -0.0000176397f, 0.0000081423f },
		{ -0.0002648938f, -0.0017322979f, -0.0022177893f, -0.0009903573f, -0.0001299446f, -0.0000264256f, 0.0000090213f, -0.0000043472f },
		{ -0.0000442326f, -0.0013202301f, -0.0012947346f, -0.0006540317f, -0.0000984317f, -0.0000027832f, -0.0000069771f, 0.0000033201f },
		{ -0.0001264282f, -0.0005041117f, -0.0003100260f, -0.0004880085f, -0.0000071201f, 0.0000074308f, -0.0000031771f, 0.0000008211f },
		{ -0.0000316374f, -0.0004033390f, -0.0002800512f, -0.0001833027f, -0.0000221368f, 0.0000054099f, -0.0000129238f, 0.0000056971f },
		{ 0.0000519809f, -0.0001235695f, -0.0002519526f, -0.0001007603f, -0.0000710147f, -0.0000127637f, 0.0000004627f, 0.0000005354f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
	},
};

static const Scalar kGGXSpecularQuadCorrectionAnisoLowOrd[ 9 ][ 24 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0000047427f, 0.0000415586f, 0.0000789313f, 0.0000477836f, -0.0000492169f, -0.0000668879f, -0.0002140803f, -0.0000977827f },
		{ 0.0001927764f, 0.0005994270f, 0.0011732679f, 0.0001967867f, -0.0008608004f, -0.0030671829f, -0.0032901639f, -0.0000192923f },
		{ 0.0005368079f, 0.0018640209f, 0.0024810503f, -0.0001689356f, -0.0038762824f, -0.0092132294f, -0.0058751573f, 0.0002933719f },
		{ 0.0009502379f, 0.0030281616f, 0.0037766890f, -0.0013079240f, -0.0094734051f, -0.0159298339f, -0.0073147912f, 0.0004541063f },
		{ 0.0013263963f, 0.0044269613f, 0.0045296832f, -0.0039286828f, -0.0165258903f, -0.0222836358f, -0.0079529736f, 0.0004492402f },
		{ 0.0015602201f, 0.0059377207f, 0.0050481664f, -0.0081361055f, -0.0247576058f, -0.0275671506f, -0.0083612955f, 0.0004410072f },
		{ 0.0017818188f, 0.0074363847f, 0.0042491048f, -0.0135313434f, -0.0331129156f, -0.0321629217f, -0.0084392466f, 0.0003608509f },
		{ 0.0023023154f, 0.0080208310f, 0.0028411698f, -0.0195273553f, -0.0415282951f, -0.0358458600f, -0.0085580967f, 0.0003310004f },
		{ 0.0025276964f, 0.0085328770f, 0.0006782212f, -0.0266960424f, -0.0491590552f, -0.0389633826f, -0.0086247580f, 0.0003003836f },
		{ 0.0026565708f, 0.0087242181f, -0.0024838493f, -0.0343977850f, -0.0568126086f, -0.0414076491f, -0.0086900835f, 0.0002832335f },
		{ 0.0028645705f, 0.0083685629f, -0.0058347717f, -0.0421073933f, -0.0638290000f, -0.0436171778f, -0.0087183285f, 0.0002650954f },
		{ 0.0028237350f, 0.0078675514f, -0.0100591384f, -0.0504138147f, -0.0701897876f, -0.0454334734f, -0.0087905592f, 0.0002707449f },
		{ 0.0031099121f, 0.0066323207f, -0.0151699007f, -0.0582330824f, -0.0759352707f, -0.0470149513f, -0.0088138597f, 0.0002665550f },
		{ 0.0031086913f, 0.0052047512f, -0.0204974642f, -0.0660617740f, -0.0814299520f, -0.0483392202f, -0.0088809330f, 0.0002792676f },
		{ 0.0030796867f, 0.0038858980f, -0.0260597087f, -0.0741315373f, -0.0863318686f, -0.0495510750f, -0.0088858205f, 0.0002706824f },
		{ 0.0029955524f, 0.0018249736f, -0.0320312141f, -0.0813416601f, -0.0909830133f, -0.0506853358f, -0.0089011779f, 0.0002670529f },
		{ 0.0031720951f, -0.0004421694f, -0.0383920906f, -0.0891323250f, -0.0951386067f, -0.0515417789f, -0.0089499994f, 0.0002810518f },
		{ 0.0028074289f, -0.0029866676f, -0.0445025454f, -0.0958500538f, -0.0987552207f, -0.0525347894f, -0.0088724546f, 0.0002435103f },
		{ 0.0026707131f, -0.0055697480f, -0.0511231483f, -0.1027184651f, -0.1024889097f, -0.0532289666f, -0.0089262974f, 0.0002639992f },
		{ 0.0026353804f, -0.0084280887f, -0.0569733036f, -0.1092498019f, -0.1059724318f, -0.0538790402f, -0.0089769583f, 0.0002842492f },
		{ 0.0023640123f, -0.0117642857f, -0.0636555886f, -0.1152298790f, -0.1090429996f, -0.0544893194f, -0.0090023994f, 0.0002914612f },
		{ 0.0021154995f, -0.0146517368f, -0.0695709453f, -0.1212185574f, -0.1118125412f, -0.0551215167f, -0.0090000238f, 0.0002893302f },
		{ 0.0017111659f, -0.0179021482f, -0.0757429198f, -0.1269572397f, -0.1145179776f, -0.0556145889f, -0.0090259071f, 0.0002987719f },
		{ 0.0014016152f, -0.0210425956f, -0.0819370459f, -0.1322999715f, -0.1168669551f, -0.0561544258f, -0.0089926937f, 0.0002852414f },
	},
	{
		{ 0.0000108416f, 0.0000280735f, 0.0000877483f, 0.0000325187f, -0.0000523270f, -0.0000588726f, -0.0002154359f, -0.0000926404f },
		{ 0.0002756013f, 0.0005955361f, 0.0011135652f, 0.0001338309f, -0.0009212363f, -0.0029548419f, -0.0032990160f, 0.0000078689f },
		{ 0.0004629968f, 0.0018107705f, 0.0024598308f, -0.0000907196f, -0.0037444793f, -0.0091931878f, -0.0058405830f, 0.0003171930f },
		{ 0.0008494807f, 0.0031306782f, 0.0039639255f, -0.0014490463f, -0.0094388270f, -0.0160277989f, -0.0071842990f, 0.0004476373f },
		{ 0.0012893342f, 0.0044445519f, 0.0048968926f, -0.0042165129f, -0.0166783926f, -0.0221265617f, -0.0079454869f, 0.0005050886f },
		{ 0.0017105525f, 0.0059492927f, 0.0047355822f, -0.0080795493f, -0.0248862745f, -0.0275017672f, -0.0082474006f, 0.0004700108f },
		{ 0.0020135339f, 0.0072335949f, 0.0043817030f, -0.0135719007f, -0.0332457487f, -0.0321077580f, -0.0083522406f, 0.0004024251f },
		{ 0.0021684091f, 0.0079305955f, 0.0031496278f, -0.0196524720f, -0.0413969323f, -0.0358692373f, -0.0084020749f, 0.0003529102f },
		{ 0.0025018989f, 0.0082182066f, 0.0007127003f, -0.0267285175f, -0.0492907172f, -0.0389288563f, -0.0084752604f, 0.0003290804f },
		{ 0.0027504287f, 0.0087531350f, -0.0023651687f, -0.0344822590f, -0.0566604062f, -0.0414323800f, -0.0085230758f, 0.0003142595f },
		{ 0.0029142061f, 0.0088232692f, -0.0061764707f, -0.0422704732f, -0.0636892282f, -0.0436136458f, -0.0085491543f, 0.0002989712f },
		{ 0.0031409620f, 0.0077753045f, -0.0101237276f, -0.0500623451f, -0.0702349233f, -0.0454326314f, -0.0086001725f, 0.0003084900f },
		{ 0.0031402643f, 0.0068181297f, -0.0156026370f, -0.0583556241f, -0.0758817324f, -0.0470128409f, -0.0085972094f, 0.0002885369f },
		{ 0.0031560229f, 0.0056646710f, -0.0203977051f, -0.0665285412f, -0.0812371657f, -0.0483672453f, -0.0086514138f, 0.0003001863f },
		{ 0.0030673324f, 0.0036946619f, -0.0263593174f, -0.0741485019f, -0.0862666692f, -0.0496050393f, -0.0086355022f, 0.0002868375f },
		{ 0.0030984839f, 0.0016506707f, -0.0325094419f, -0.0814348970f, -0.0907253272f, -0.0506072749f, -0.0086906681f, 0.0003109679f },
		{ 0.0031389842f, -0.0005534382f, -0.0383521670f, -0.0889206879f, -0.0949553364f, -0.0515421837f, -0.0087135828f, 0.0003175423f },
		{ 0.0028284112f, -0.0029597038f, -0.0445925719f, -0.0960121782f, -0.0989078991f, -0.0523892889f, -0.0087079693f, 0.0003180731f },
		{ 0.0027817573f, -0.0059766110f, -0.0505027141f, -0.1027635910f, -0.1025799605f, -0.0530449809f, -0.0087632935f, 0.0003439294f },
		{ 0.0024116823f, -0.0087629303f, -0.0573968030f, -0.1092880870f, -0.1058361801f, -0.0537460289f, -0.0087547691f, 0.0003434999f },
		{ 0.0023212301f, -0.0115189075f, -0.0634076546f, -0.1155286491f, -0.1089155862f, -0.0544474439f, -0.0087050116f, 0.0003228309f },
		{ 0.0020019420f, -0.0149240261f, -0.0697662260f, -0.1212640454f, -0.1118014409f, -0.0549890153f, -0.0087118749f, 0.0003315806f },
		{ 0.0017511173f, -0.0175489821f, -0.0761018636f, -0.1268940284f, -0.1143798049f, -0.0555265680f, -0.0086817058f, 0.0003245847f },
		{ 0.0014993280f, -0.0209653343f, -0.0819301613f, -0.1325465017f, -0.1169137719f, -0.0559916163f, -0.0087012280f, 0.0003368275f },
	},
	{
		{ 0.0000245399f, 0.0000159973f, 0.0000765915f, 0.0000312088f, -0.0000246524f, -0.0000675780f, -0.0002088856f, -0.0000833389f },
		{ 0.0001894094f, 0.0006004961f, 0.0011111280f, 0.0002131321f, -0.0008404637f, -0.0030691478f, -0.0032129602f, 0.0000171384f },
		{ 0.0006720118f, 0.0014290656f, 0.0026385242f, -0.0000844303f, -0.0040038222f, -0.0090521296f, -0.0058115510f, 0.0003697448f },
		{ 0.0008641239f, 0.0029975098f, 0.0038485956f, -0.0014838739f, -0.0093640759f, -0.0158666341f, -0.0071526516f, 0.0005122335f },
		{ 0.0012697223f, 0.0048494411f, 0.0044670424f, -0.0043324679f, -0.0166153872f, -0.0222214048f, -0.0077622044f, 0.0005205979f },
		{ 0.0016657623f, 0.0057166524f, 0.0047033603f, -0.0079782647f, -0.0247031140f, -0.0275786589f, -0.0080526657f, 0.0004904774f },
		{ 0.0018803270f, 0.0069931937f, 0.0044213971f, -0.0134508008f, -0.0332422513f, -0.0320528882f, -0.0081833122f, 0.0004486814f },
		{ 0.0023056511f, 0.0079284086f, 0.0028532099f, -0.0198084971f, -0.0413745895f, -0.0357089809f, -0.0082752474f, 0.0004231975f },
		{ 0.0025510922f, 0.0087484434f, 0.0004247359f, -0.0268977122f, -0.0492137144f, -0.0388433417f, -0.0082474298f, 0.0003697172f },
		{ 0.0028427766f, 0.0083265857f, -0.0023805074f, -0.0344239321f, -0.0567518855f, -0.0412498593f, -0.0083485241f, 0.0003919872f },
		{ 0.0029782867f, 0.0083051214f, -0.0062875206f, -0.0421025563f, -0.0637724040f, -0.0434539011f, -0.0082884937f, 0.0003548723f },
		{ 0.0030437845f, 0.0076642219f, -0.0108251979f, -0.0501131105f, -0.0700602121f, -0.0452498526f, -0.0083098578f, 0.0003515942f },
		{ 0.0030081598f, 0.0068459663f, -0.0153839649f, -0.0582169168f, -0.0758077516f, -0.0467934831f, -0.0083198329f, 0.0003532435f },
		{ 0.0029509672f, 0.0054222385f, -0.0203443600f, -0.0665442560f, -0.0811977408f, -0.0481619525f, -0.0083152478f, 0.0003514213f },
		{ 0.0032275941f, 0.0037664504f, -0.0267714720f, -0.0742125344f, -0.0861592949f, -0.0493723271f, -0.0082981587f, 0.0003458790f },
		{ 0.0031730138f, 0.0018886518f, -0.0324630900f, -0.0815659906f, -0.0907884771f, -0.0504361024f, -0.0082973123f, 0.0003514417f },
		{ 0.0028698558f, -0.0005942668f, -0.0384204102f, -0.0888733312f, -0.0949621770f, -0.0512862170f, -0.0082951665f, 0.0003572594f },
		{ 0.0029550774f, -0.0033748717f, -0.0447730180f, -0.0959410498f, -0.0989812140f, -0.0521133009f, -0.0082848311f, 0.0003642015f },
		{ 0.0027152621f, -0.0059592754f, -0.0511038255f, -0.1028454067f, -0.1024023565f, -0.0527951809f, -0.0082928102f, 0.0003735323f },
		{ 0.0024726303f, -0.0083611294f, -0.0574074860f, -0.1092140166f, -0.1058310109f, -0.0535858052f, -0.0081910986f, 0.0003406720f },
		{ 0.0023334506f, -0.0118011118f, -0.0636623952f, -0.1154598155f, -0.1087793178f, -0.0541074303f, -0.0082162305f, 0.0003623541f },
		{ 0.0021053724f, -0.0148429748f, -0.0699252314f, -0.1213673970f, -0.1118182094f, -0.0546970740f, -0.0081999788f, 0.0003633823f },
		{ 0.0018474358f, -0.0179415042f, -0.0763186229f, -0.1268752602f, -0.1143258663f, -0.0552264005f, -0.0081733804f, 0.0003637952f },
		{ 0.0015592123f, -0.0213080751f, -0.0821276967f, -0.1322298897f, -0.1169470801f, -0.0556339591f, -0.0081872744f, 0.0003823449f },
	},
	{
		{ 0.0000156628f, 0.0000709937f, 0.0000150539f, 0.0000238501f, -0.0000103384f, -0.0000819251f, -0.0001792089f, -0.0000791555f },
		{ 0.0001586258f, 0.0006660080f, 0.0010361484f, 0.0001730355f, -0.0008234904f, -0.0030373013f, -0.0031326566f, 0.0000481122f },
		{ 0.0005304106f, 0.0017453595f, 0.0023426624f, -0.0001209120f, -0.0038447317f, -0.0090960322f, -0.0056295403f, 0.0003855056f },
		{ 0.0009114050f, 0.0027434725f, 0.0040171876f, -0.0013937869f, -0.0095249363f, -0.0157362638f, -0.0070081822f, 0.0005755578f },
		{ 0.0011868444f, 0.0046657078f, 0.0047761737f, -0.0043979677f, -0.0165414162f, -0.0221357965f, -0.0075342414f, 0.0005649304f },
		{ 0.0016584596f, 0.0056509698f, 0.0047665582f, -0.0080523186f, -0.0246586829f, -0.0273292578f, -0.0078155084f, 0.0005510633f },
		{ 0.0018247052f, 0.0070280783f, 0.0044004271f, -0.0137909125f, -0.0328595905f, -0.0319004662f, -0.0078389540f, 0.0004839010f },
		{ 0.0021535455f, 0.0078587993f, 0.0027302337f, -0.0196546514f, -0.0413929982f, -0.0355453292f, -0.0078946943f, 0.0004628379f },
		{ 0.0023382411f, 0.0083222049f, 0.0004675235f, -0.0269735665f, -0.0489750953f, -0.0385706050f, -0.0078418532f, 0.0004167390f },
		{ 0.0026203061f, 0.0089440441f, -0.0026876743f, -0.0344321450f, -0.0567044807f, -0.0411088798f, -0.0078195981f, 0.0003957009f },
		{ 0.0028732786f, 0.0082211373f, -0.0058612077f, -0.0423160511f, -0.0636686006f, -0.0431333958f, -0.0078228359f, 0.0003958912f },
		{ 0.0028847213f, 0.0080943023f, -0.0105086091f, -0.0507574167f, -0.0699252659f, -0.0450129345f, -0.0077145214f, 0.0003575508f },
		{ 0.0030839273f, 0.0068870438f, -0.0156686907f, -0.0583707718f, -0.0758495882f, -0.0464798610f, -0.0077357842f, 0.0003759942f },
		{ 0.0031787443f, 0.0055315395f, -0.0208606339f, -0.0661823654f, -0.0814245248f, -0.0477835318f, -0.0077329156f, 0.0003868527f },
		{ 0.0031827050f, 0.0039606777f, -0.0265678265f, -0.0745609671f, -0.0861018525f, -0.0489835305f, -0.0076593002f, 0.0003675110f },
		{ 0.0030510264f, 0.0019212545f, -0.0324630976f, -0.0819404119f, -0.0908259750f, -0.0499599550f, -0.0076312388f, 0.0003742209f },
		{ 0.0028763415f, -0.0006968552f, -0.0387891637f, -0.0890091809f, -0.0948135897f, -0.0508134875f, -0.0076057196f, 0.0003745547f },
		{ 0.0027991293f, -0.0036026750f, -0.0448949410f, -0.0959137842f, -0.0987570711f, -0.0515956309f, -0.0075575777f, 0.0003699613f },
		{ 0.0025220252f, -0.0062312821f, -0.0509909803f, -0.1028690139f, -0.1023185006f, -0.0522909407f, -0.0074957799f, 0.0003562693f },
		{ 0.0024489590f, -0.0089287331f, -0.0576271650f, -0.1092869642f, -0.1057896882f, -0.0528063901f, -0.0074947136f, 0.0003725658f },
		{ 0.0021574690f, -0.0116349677f, -0.0637298076f, -0.1154508891f, -0.1086674436f, -0.0534953393f, -0.0073720174f, 0.0003332659f },
		{ 0.0017358994f, -0.0149569444f, -0.0698218907f, -0.1214807301f, -0.1115172125f, -0.0539596909f, -0.0073228947f, 0.0003311387f },
		{ 0.0017177449f, -0.0179477271f, -0.0762158966f, -0.1271015930f, -0.1141949192f, -0.0544273881f, -0.0072796346f, 0.0003274201f },
		{ 0.0015029898f, -0.0214978783f, -0.0824712589f, -0.1326472848f, -0.1166856340f, -0.0547946945f, -0.0072670765f, 0.0003373010f },
	},
	{
		{ 0.0000143035f, 0.0000207169f, 0.0000299757f, 0.0000211467f, -0.0000008450f, -0.0000705816f, -0.0001487950f, -0.0000596926f },
		{ 0.0002029329f, 0.0006235294f, 0.0010902804f, 0.0001064727f, -0.0008029460f, -0.0029723789f, -0.0030082372f, 0.0000929148f },
		{ 0.0005226187f, 0.0017259706f, 0.0023569071f, -0.0001820970f, -0.0038852709f, -0.0089135956f, -0.0054467544f, 0.0004564308f },
		{ 0.0008883005f, 0.0030838307f, 0.0033842525f, -0.0014149830f, -0.0093484405f, -0.0156784533f, -0.0066176477f, 0.0005951829f },
		{ 0.0011204653f, 0.0047602003f, 0.0044357747f, -0.0043045863f, -0.0165714072f, -0.0218838922f, -0.0070680144f, 0.0005890646f },
		{ 0.0016879168f, 0.0058991178f, 0.0045837596f, -0.0086330237f, -0.0243861161f, -0.0271886241f, -0.0072187626f, 0.0005455972f },
		{ 0.0019362029f, 0.0067922914f, 0.0039717281f, -0.0134197085f, -0.0329168065f, -0.0315831909f, -0.0072728662f, 0.0005156922f },
		{ 0.0020315835f, 0.0079308755f, 0.0024863736f, -0.0198038813f, -0.0413427734f, -0.0351595839f, -0.0072208333f, 0.0004701452f },
		{ 0.0024607822f, 0.0081975903f, 0.0004367690f, -0.0269753283f, -0.0490924797f, -0.0380724621f, -0.0071703856f, 0.0004461237f },
		{ 0.0025827922f, 0.0083966650f, -0.0025266535f, -0.0347989744f, -0.0565020020f, -0.0404935246f, -0.0070485529f, 0.0003919666f },
		{ 0.0026931430f, 0.0080853682f, -0.0066533948f, -0.0421642695f, -0.0633805471f, -0.0423961846f, -0.0069890287f, 0.0003767755f },
		{ 0.0027835764f, 0.0079698983f, -0.0112114081f, -0.0507060575f, -0.0695838924f, -0.0441667574f, -0.0068330698f, 0.0003245450f },
		{ 0.0030395403f, 0.0064555182f, -0.0157168347f, -0.0585936987f, -0.0755271980f, -0.0455386187f, -0.0068053989f, 0.0003325750f },
		{ 0.0031157251f, 0.0050635306f, -0.0214275747f, -0.0661726101f, -0.0809262141f, -0.0468286171f, -0.0066888876f, 0.0003054828f },
		{ 0.0030692161f, 0.0036086564f, -0.0269528893f, -0.0742848444f, -0.0858805459f, -0.0477902329f, -0.0066496709f, 0.0003111293f },
		{ 0.0028147247f, 0.0012770360f, -0.0325803755f, -0.0820142959f, -0.0903546594f, -0.0487940884f, -0.0065027329f, 0.0002706196f },
		{ 0.0028321749f, -0.0011705232f, -0.0392337332f, -0.0890071235f, -0.0945162900f, -0.0496107627f, -0.0064353153f, 0.0002691587f },
		{ 0.0025635150f, -0.0033786074f, -0.0449691392f, -0.0963343199f, -0.0985367484f, -0.0502865840f, -0.0063915934f, 0.0002766796f },
		{ 0.0026326368f, -0.0062979003f, -0.0517367162f, -0.1030183984f, -0.1019221130f, -0.0509777862f, -0.0062983900f, 0.0002631986f },
		{ 0.0022694606f, -0.0087293717f, -0.0579536643f, -0.1095974173f, -0.1052744319f, -0.0515300353f, -0.0062083194f, 0.0002499818f },
		{ 0.0021826525f, -0.0121911851f, -0.0643561945f, -0.1156213698f, -0.1082384553f, -0.0520585879f, -0.0061275855f, 0.0002430475f },
		{ 0.0019885731f, -0.0152864869f, -0.0707729336f, -0.1213061730f, -0.1111702035f, -0.0525145919f, -0.0060707426f, 0.0002441858f },
		{ 0.0015662694f, -0.0183833812f, -0.0768741079f, -0.1270544318f, -0.1138404732f, -0.0528850228f, -0.0060033986f, 0.0002423967f },
		{ 0.0012976513f, -0.0217552999f, -0.0827789070f, -0.1325626774f, -0.1162054902f, -0.0533630813f, -0.0058817307f, 0.0002151243f },
	},
	{
		{ 0.0000100399f, -0.0000082718f, 0.0000403700f, 0.0000597115f, -0.0000599835f, -0.0000217952f, -0.0001406283f, -0.0000234995f },
		{ 0.0002178850f, 0.0005119916f, 0.0009020023f, 0.0001580750f, -0.0008279291f, -0.0028323210f, -0.0027579491f, 0.0001406418f },
		{ 0.0004758383f, 0.0016242918f, 0.0022961672f, -0.0002373278f, -0.0038615525f, -0.0087374714f, -0.0049516029f, 0.0004676218f },
		{ 0.0008735546f, 0.0029218271f, 0.0036860212f, -0.0016576054f, -0.0093459232f, -0.0153572896f, -0.0059995402f, 0.0006101892f },
		{ 0.0011121324f, 0.0045094295f, 0.0042789566f, -0.0042251552f, -0.0164526817f, -0.0213973508f, -0.0063667980f, 0.0006021729f },
		{ 0.0014489284f, 0.0058953068f, 0.0043151048f, -0.0084888252f, -0.0243436094f, -0.0263356415f, -0.0064486202f, 0.0005516387f },
		{ 0.0017181761f, 0.0066222259f, 0.0039724718f, -0.0137211961f, -0.0327253443f, -0.0305126242f, -0.0063698866f, 0.0004883149f },
		{ 0.0022708021f, 0.0075108229f, 0.0021186084f, -0.0199174087f, -0.0410145070f, -0.0339782002f, -0.0061811115f, 0.0004072191f },
		{ 0.0020800530f, 0.0081776500f, 0.0000857843f, -0.0271198087f, -0.0485974588f, -0.0369464589f, -0.0059721976f, 0.0003223501f },
		{ 0.0024555763f, 0.0085393730f, -0.0033310782f, -0.0347482619f, -0.0560699265f, -0.0391445944f, -0.0058249105f, 0.0002882620f },
		{ 0.0026737420f, 0.0081673355f, -0.0070373814f, -0.0426654359f, -0.0628940030f, -0.0411072451f, -0.0056520906f, 0.0002462398f },
		{ 0.0026419166f, 0.0075123156f, -0.0112183852f, -0.0505820612f, -0.0694685782f, -0.0426745845f, -0.0055620486f, 0.0002448731f },
		{ 0.0028195848f, 0.0061757736f, -0.0162575243f, -0.0585547876f, -0.0752662367f, -0.0440358805f, -0.0054362065f, 0.0002266735f },
		{ 0.0028154589f, 0.0049308032f, -0.0218647493f, -0.0664806197f, -0.0804339388f, -0.0452270928f, -0.0052282060f, 0.0001821524f },
		{ 0.0028558974f, 0.0033675435f, -0.0273871793f, -0.0745814023f, -0.0853897259f, -0.0461578119f, -0.0051466692f, 0.0001863179f },
		{ 0.0028480318f, 0.0009293228f, -0.0335837277f, -0.0817717751f, -0.0897966630f, -0.0470362455f, -0.0050116919f, 0.0001751857f },
		{ 0.0028666238f, -0.0014701861f, -0.0398895367f, -0.0889846371f, -0.0941426444f, -0.0478010918f, -0.0048882033f, 0.0001632649f },
		{ 0.0024884605f, -0.0039584532f, -0.0458433839f, -0.0960959949f, -0.0979911854f, -0.0483469123f, -0.0048163561f, 0.0001580106f },
		{ 0.0024310304f, -0.0064982045f, -0.0525031811f, -0.1028319859f, -0.1014491729f, -0.0488932628f, -0.0046741895f, 0.0001208088f },
		{ 0.0023018984f, -0.0097125531f, -0.0587163951f, -0.1093346584f, -0.1045592605f, -0.0492837341f, -0.0045846036f, 0.0001063582f },
		{ 0.0019247042f, -0.0123920939f, -0.0650396332f, -0.1156561905f, -0.1072968234f, -0.0496807849f, -0.0044742381f, 0.0000776419f },
		{ 0.0016867889f, -0.0156084784f, -0.0712334323f, -0.1215138069f, -0.1101560714f, -0.0500165912f, -0.0043581450f, 0.0000524915f },
		{ 0.0014620177f, -0.0185000895f, -0.0777408761f, -0.1270260815f, -0.1127436284f, -0.0502889374f, -0.0043035773f, 0.0000531397f },
		{ 0.0012433533f, -0.0223808952f, -0.0836131530f, -0.1320861302f, -0.1151900708f, -0.0504047391f, -0.0042643799f, 0.0000618683f },
	},
	{
		{ -0.0000114727f, -0.0000148393f, 0.0000281290f, 0.0000054648f, 0.0000105640f, -0.0000115877f, -0.0000210739f, -0.0000169907f },
		{ 0.0001710015f, 0.0004685117f, 0.0008759997f, -0.0000131075f, -0.0006159057f, -0.0026187102f, -0.0022585768f, 0.0001565583f },
		{ 0.0006298749f, 0.0013860480f, 0.0021003948f, -0.0004547120f, -0.0036879629f, -0.0081369162f, -0.0042712300f, 0.0004957338f },
		{ 0.0008091286f, 0.0027348709f, 0.0030871051f, -0.0016684831f, -0.0089196435f, -0.0144040331f, -0.0050339322f, 0.0005520290f },
		{ 0.0012202882f, 0.0040266040f, 0.0037743160f, -0.0040439516f, -0.0161687972f, -0.0201558600f, -0.0052437937f, 0.0005225845f },
		{ 0.0013914245f, 0.0053589817f, 0.0038085681f, -0.0080880559f, -0.0241243666f, -0.0250535186f, -0.0051442604f, 0.0004414305f },
		{ 0.0017605391f, 0.0065946398f, 0.0032379729f, -0.0138768711f, -0.0322365832f, -0.0290809802f, -0.0049322056f, 0.0003545297f },
		{ 0.0020228526f, 0.0074685124f, 0.0018654283f, -0.0202898271f, -0.0404917872f, -0.0323329046f, -0.0046979633f, 0.0002908470f },
		{ 0.0023785439f, 0.0080046207f, -0.0009818597f, -0.0271235046f, -0.0482684926f, -0.0350496940f, -0.0044203454f, 0.0002184948f },
		{ 0.0025638106f, 0.0078807348f, -0.0037800537f, -0.0346342403f, -0.0554818534f, -0.0370196320f, -0.0042588823f, 0.0001678766f },
		{ 0.0025297176f, 0.0075523505f, -0.0074380133f, -0.0426282287f, -0.0621548349f, -0.0386735114f, -0.0040430919f, 0.0000983062f },
		{ 0.0029268224f, 0.0070532143f, -0.0123435180f, -0.0502308944f, -0.0685070202f, -0.0400316593f, -0.0038972088f, 0.0000690753f },
		{ 0.0028019484f, 0.0057157558f, -0.0172305781f, -0.0582721156f, -0.0739872959f, -0.0410539414f, -0.0037369328f, 0.0000356343f },
		{ 0.0027461008f, 0.0042244547f, -0.0226635054f, -0.0662074409f, -0.0791893120f, -0.0419727018f, -0.0035391494f, -0.0000156842f },
		{ 0.0028179561f, 0.0024874094f, -0.0282315098f, -0.0743764279f, -0.0838308342f, -0.0426323626f, -0.0034413194f, -0.0000202566f },
		{ 0.0024161237f, 0.0008021282f, -0.0345216746f, -0.0818828269f, -0.0882467956f, -0.0432624715f, -0.0032390071f, -0.0000712335f },
		{ 0.0025761001f, -0.0022497680f, -0.0407901248f, -0.0889898680f, -0.0920755106f, -0.0437662241f, -0.0031414694f, -0.0000736233f },
		{ 0.0023939655f, -0.0044417240f, -0.0470973032f, -0.0960688182f, -0.0959259618f, -0.0441879915f, -0.0029567850f, -0.0001147229f },
		{ 0.0020378958f, -0.0074997261f, -0.0530944151f, -0.1026568513f, -0.0992955124f, -0.0446428733f, -0.0028003797f, -0.0001441800f },
		{ 0.0018333422f, -0.0101599314f, -0.0599636852f, -0.1092827620f, -0.1024114929f, -0.0449013741f, -0.0026502010f, -0.0001727084f },
		{ 0.0016497692f, -0.0131788074f, -0.0663283221f, -0.1154010218f, -0.1054224006f, -0.0451358480f, -0.0025294992f, -0.0001839408f },
		{ 0.0014463708f, -0.0164191046f, -0.0723441708f, -0.1212578855f, -0.1081531840f, -0.0453050799f, -0.0024035045f, -0.0001996865f },
		{ 0.0011064744f, -0.0196224663f, -0.0785822289f, -0.1268429704f, -0.1106042601f, -0.0454977682f, -0.0022725039f, -0.0002195237f },
		{ 0.0008324695f, -0.0231303729f, -0.0850019814f, -0.1323662112f, -0.1128378420f, -0.0456461614f, -0.0021213814f, -0.0002484156f },
	},
	{
		{ 0.0000101644f, 0.0000102737f, 0.0000120632f, -0.0000389520f, 0.0000388994f, -0.0000364488f, -0.0000166063f, -0.0000114777f },
		{ 0.0000821867f, 0.0001284964f, 0.0005630926f, -0.0000607846f, -0.0004058365f, -0.0013935527f, -0.0010638475f, 0.0000987819f },
		{ 0.0004777812f, 0.0010261476f, 0.0014857821f, -0.0003044694f, -0.0033297573f, -0.0060613069f, -0.0023293555f, 0.0003233241f },
		{ 0.0005998928f, 0.0022612053f, 0.0021742642f, -0.0016373789f, -0.0080921898f, -0.0113909396f, -0.0027407571f, 0.0003404900f },
		{ 0.0008774994f, 0.0032578302f, 0.0028586177f, -0.0041336794f, -0.0145961826f, -0.0160522178f, -0.0028082202f, 0.0002793415f },
		{ 0.0010866019f, 0.0044944869f, 0.0024703990f, -0.0082676458f, -0.0219572902f, -0.0201017580f, -0.0024486268f, 0.0001168522f },
		{ 0.0014129092f, 0.0057158169f, 0.0018237279f, -0.0138091914f, -0.0298490821f, -0.0232966404f, -0.0021360844f, 0.0000134618f },
		{ 0.0015418008f, 0.0063383827f, -0.0000907564f, -0.0197866398f, -0.0374601086f, -0.0259594224f, -0.0016364347f, -0.0001443196f },
		{ 0.0019240229f, 0.0063822150f, -0.0026533768f, -0.0266522399f, -0.0449680894f, -0.0278098222f, -0.0013227473f, -0.0002065722f },
		{ 0.0019514949f, 0.0064818309f, -0.0056006521f, -0.0343517965f, -0.0514971056f, -0.0293140043f, -0.0010658435f, -0.0002943234f },
		{ 0.0021548168f, 0.0062254566f, -0.0098677006f, -0.0418047823f, -0.0578318032f, -0.0303324171f, -0.0009575080f, -0.0003151099f },
		{ 0.0022322883f, 0.0051770690f, -0.0141747861f, -0.0497593661f, -0.0633403927f, -0.0310984279f, -0.0007910321f, -0.0003539073f },
		{ 0.0022047496f, 0.0036143428f, -0.0189150003f, -0.0575484042f, -0.0685923908f, -0.0316922120f, -0.0006729321f, -0.0003724810f },
		{ 0.0020925133f, 0.0026000355f, -0.0244130779f, -0.0657367403f, -0.0731551666f, -0.0320495995f, -0.0005499982f, -0.0003919981f },
		{ 0.0021197415f, 0.0007390843f, -0.0307669119f, -0.0732749266f, -0.0772889887f, -0.0324326829f, -0.0003525854f, -0.0004443373f },
		{ 0.0022159741f, -0.0014661957f, -0.0371718456f, -0.0804019132f, -0.0813241623f, -0.0326352331f, -0.0001990741f, -0.0004720272f },
		{ 0.0018527126f, -0.0040871219f, -0.0425230535f, -0.0875081625f, -0.0850022263f, -0.0326084483f, -0.0001049861f, -0.0004738833f },
		{ 0.0016382721f, -0.0066729710f, -0.0499026941f, -0.0943681834f, -0.0879769546f, -0.0327920505f, 0.0000976446f, -0.0005282144f },
		{ 0.0015974598f, -0.0095501351f, -0.0562208001f, -0.1011525319f, -0.0908470945f, -0.0326364959f, 0.0000596782f, -0.0004957265f },
		{ 0.0013160018f, -0.0129255002f, -0.0623828212f, -0.1074234604f, -0.0932639680f, -0.0326472428f, 0.0001192024f, -0.0005066366f },
		{ 0.0011477026f, -0.0158306627f, -0.0689674402f, -0.1134118991f, -0.0957247589f, -0.0324819220f, 0.0000991091f, -0.0004809691f },
		{ 0.0007484094f, -0.0189944261f, -0.0749233145f, -0.1191871106f, -0.0977631799f, -0.0324044135f, 0.0001718998f, -0.0004975965f },
		{ 0.0005765158f, -0.0222483637f, -0.0814811193f, -0.1244391312f, -0.0997259883f, -0.0322106537f, 0.0001619924f, -0.0004778694f },
		{ 0.0002818796f, -0.0259631191f, -0.0878772147f, -0.1292862879f, -0.1015525730f, -0.0319759643f, 0.0001729869f, -0.0004645741f },
	},
	{
		{ 0.0000514746f, 0.0000032636f, 0.0000221922f, 0.0001156358f, -0.0000946322f, -0.0001853507f, -0.0002212366f, -0.0000069027f },
		{ 0.0001277595f, -0.0000124754f, 0.0002955099f, -0.0000521980f, -0.0002015603f, -0.0007151325f, -0.0006108121f, 0.0000732271f },
		{ 0.0002619231f, 0.0008526649f, 0.0009161770f, -0.0003975421f, -0.0024454111f, -0.0046286715f, -0.0015707780f, 0.0002348914f },
		{ 0.0005296313f, 0.0017137990f, 0.0018032826f, -0.0016428769f, -0.0071367316f, -0.0090399086f, -0.0018955836f, 0.0002261997f },
		{ 0.0007737152f, 0.0029090009f, 0.0021536971f, -0.0043491888f, -0.0134003482f, -0.0134051919f, -0.0016759134f, 0.0001093762f },
		{ 0.0008727979f, 0.0042802389f, 0.0018649301f, -0.0082884992f, -0.0206871554f, -0.0171927464f, -0.0011976081f, -0.0000626702f },
		{ 0.0013182354f, 0.0049671253f, 0.0008781172f, -0.0135105521f, -0.0279084989f, -0.0197962243f, -0.0009679991f, -0.0001459527f },
		{ 0.0015838021f, 0.0053599260f, -0.0009324291f, -0.0192276523f, -0.0351426927f, -0.0219361054f, -0.0007116050f, -0.0002405001f },
		{ 0.0015240361f, 0.0059918575f, -0.0035227166f, -0.0262601229f, -0.0417827767f, -0.0235514887f, -0.0003851531f, -0.0003529075f },
		{ 0.0017388789f, 0.0057325869f, -0.0066443655f, -0.0336125630f, -0.0481219107f, -0.0246360903f, -0.0002012649f, -0.0003962349f },
		{ 0.0017370307f, 0.0050320002f, -0.0108927188f, -0.0410871861f, -0.0539342298f, -0.0254958054f, 0.0000683910f, -0.0004698148f },
		{ 0.0020954627f, 0.0044237872f, -0.0156545733f, -0.0492065630f, -0.0594890983f, -0.0259564999f, 0.0002143497f, -0.0004810740f },
		{ 0.0021226203f, 0.0028182580f, -0.0206868016f, -0.0566506119f, -0.0640179325f, -0.0264255180f, 0.0002752011f, -0.0004907200f },
		{ 0.0020752345f, 0.0012614841f, -0.0256720844f, -0.0642639397f, -0.0684412356f, -0.0265725117f, 0.0002998052f, -0.0004862969f },
		{ 0.0019991138f, -0.0005233153f, -0.0319490812f, -0.0716619297f, -0.0721075100f, -0.0267424181f, 0.0003694607f, -0.0004961828f },
		{ 0.0019441256f, -0.0029264998f, -0.0379401861f, -0.0789521902f, -0.0756708708f, -0.0266363034f, 0.0003442757f, -0.0004634504f },
		{ 0.0016697597f, -0.0052823165f, -0.0440999034f, -0.0864582780f, -0.0787456543f, -0.0265844324f, 0.0004251692f, -0.0004758500f },
		{ 0.0015873016f, -0.0083989641f, -0.0502921689f, -0.0929087122f, -0.0816232590f, -0.0264330712f, 0.0004264661f, -0.0004573335f },
		{ 0.0011193415f, -0.0115193742f, -0.0566594458f, -0.0990102927f, -0.0842881360f, -0.0262314646f, 0.0004604195f, -0.0004475371f },
		{ 0.0010640207f, -0.0138409662f, -0.0635256657f, -0.1054796666f, -0.0865033629f, -0.0259720065f, 0.0005133442f, -0.0004519525f },
		{ 0.0007450605f, -0.0173998354f, -0.0695336327f, -0.1109958994f, -0.0884456469f, -0.0258789944f, 0.0004962986f, -0.0004355789f },
		{ 0.0004727498f, -0.0210445984f, -0.0761939429f, -0.1163610621f, -0.0902597765f, -0.0256792781f, 0.0004934772f, -0.0004220050f },
		{ 0.0002605315f, -0.0242592303f, -0.0823566355f, -0.1216430472f, -0.0918505027f, -0.0254831641f, 0.0004845504f, -0.0004123563f },
		{ -0.0000706587f, -0.0276435227f, -0.0884216229f, -0.1264147512f, -0.0932312366f, -0.0253443292f, 0.0005044999f, -0.0004123479f },
	},
};

static const Scalar kGGXSpecularQuadCorrectionAnisoLowLow[ 9 ][ 9 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000000147f, -0.0000006974f, 0.0000004403f, 0.0000000167f, 0.0000000093f, 0.0000005265f, -0.0000003683f, 0.0000001112f },
		{ 0.0000007473f, 0.0000001010f, 0.0000006800f, -0.0000012625f, 0.0000003451f, -0.0000004801f, 0.0000000044f, -0.0000002660f },
		{ 0.0000006527f, -0.0000006458f, 0.0000009527f, -0.0000013365f, 0.0000023091f, -0.0000016006f, 0.0000003817f, -0.0000012322f },
		{ 0.0000017236f, 0.0000010521f, 0.0000017211f, -0.0000010462f, -0.0000007443f, -0.0000016734f, -0.0000011947f, -0.0000044277f },
		{ -0.0000018566f, 0.0000038015f, 0.0000098418f, 0.0000020857f, -0.0000033816f, -0.0000022739f, -0.0000143874f, -0.0000136154f },
		{ -0.0000104445f, 0.0000205345f, 0.0000120567f, 0.0000257899f, -0.0000066707f, -0.0000169090f, -0.0000538290f, -0.0000436794f },
		{ 0.0000101415f, 0.0000526001f, 0.0001356351f, 0.0000880764f, -0.0000408220f, -0.0001975176f, -0.0004651325f, -0.0001442720f },
		{ 0.0000387190f, 0.0001019353f, 0.0002962154f, 0.0002173503f, -0.0002128060f, -0.0004577889f, -0.0009943054f, -0.0001575785f },
	},
	{
		{ 0.0000000147f, -0.0000006974f, 0.0000004403f, 0.0000000167f, 0.0000000093f, 0.0000005265f, -0.0000003683f, 0.0000001112f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000003411f, -0.0000006995f, 0.0000008794f, -0.0000006892f, 0.0000000421f, 0.0000003052f, -0.0000000697f, 0.0000000470f },
		{ 0.0000009013f, 0.0000001709f, -0.0000022889f, 0.0000000903f, 0.0000011371f, 0.0000007237f, -0.0000011818f, -0.0000005877f },
		{ 0.0000006068f, 0.0000000343f, -0.0000019199f, 0.0000004676f, 0.0000016005f, 0.0000011427f, -0.0000026032f, -0.0000029654f },
		{ 0.0000007144f, 0.0000029680f, 0.0000095750f, -0.0000064658f, 0.0000110034f, -0.0000087367f, -0.0000108051f, -0.0000141752f },
		{ 0.0000026813f, -0.0000029488f, 0.0000474100f, -0.0000118241f, 0.0000062972f, -0.0000213903f, -0.0000535099f, -0.0000398265f },
		{ 0.0000315357f, 0.0000698645f, 0.0001150742f, 0.0000949795f, -0.0000721712f, -0.0001961400f, -0.0004605395f, -0.0001362150f },
		{ 0.0000777310f, 0.0001591462f, 0.0002988379f, 0.0000637254f, -0.0001243673f, -0.0004971842f, -0.0009732923f, -0.0001563941f },
	},
	{
		{ 0.0000007473f, 0.0000001010f, 0.0000006800f, -0.0000012625f, 0.0000003451f, -0.0000004801f, 0.0000000044f, -0.0000002660f },
		{ 0.0000003411f, -0.0000006995f, 0.0000008794f, -0.0000006892f, 0.0000000421f, 0.0000003052f, -0.0000000697f, 0.0000000470f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000017161f, -0.0000005182f, 0.0000026343f, -0.0000010303f, 0.0000012450f, -0.0000004665f, 0.0000002521f, -0.0000003153f },
		{ 0.0000001563f, -0.0000000530f, 0.0000034001f, 0.0000000059f, -0.0000023615f, -0.0000007287f, -0.0000010721f, -0.0000026329f },
		{ -0.0000032741f, 0.0000088864f, 0.0000044213f, -0.0000016235f, 0.0000006521f, -0.0000075776f, -0.0000056562f, -0.0000122500f },
		{ 0.0000101578f, -0.0000029554f, 0.0000179556f, -0.0000014105f, 0.0000199752f, -0.0000219123f, -0.0000487328f, -0.0000376679f },
		{ 0.0000382143f, 0.0000557039f, 0.0001141389f, 0.0000641683f, -0.0000603524f, -0.0001646873f, -0.0004677995f, -0.0001177953f },
		{ 0.0000534207f, 0.0001435421f, 0.0002682869f, 0.0000957538f, -0.0001058051f, -0.0005031019f, -0.0009351934f, -0.0001531325f },
	},
	{
		{ 0.0000006527f, -0.0000006458f, 0.0000009527f, -0.0000013365f, 0.0000023091f, -0.0000016006f, 0.0000003817f, -0.0000012322f },
		{ 0.0000009013f, 0.0000001709f, -0.0000022889f, 0.0000000903f, 0.0000011371f, 0.0000007237f, -0.0000011818f, -0.0000005877f },
		{ -0.0000017161f, -0.0000005182f, 0.0000026343f, -0.0000010303f, 0.0000012450f, -0.0000004665f, 0.0000002521f, -0.0000003153f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000010768f, 0.0000023565f, -0.0000058448f, 0.0000034252f, -0.0000013873f, 0.0000019706f, -0.0000012891f, 0.0000001308f },
		{ 0.0000059267f, -0.0000072266f, 0.0000092560f, 0.0000061873f, -0.0000115417f, 0.0000010407f, -0.0000088155f, -0.0000070374f },
		{ 0.0000018664f, -0.0000024491f, 0.0000368281f, -0.0000040418f, -0.0000118246f, -0.0000018230f, -0.0000464676f, -0.0000279238f },
		{ 0.0000226183f, 0.0000967522f, 0.0000800949f, 0.0000918720f, -0.0000541937f, -0.0001952285f, -0.0004243404f, -0.0001085562f },
		{ 0.0000401601f, 0.0001956132f, 0.0002557263f, 0.0000554730f, -0.0001149196f, -0.0004906901f, -0.0009086402f, -0.0001251049f },
	},
	{
		{ 0.0000017236f, 0.0000010521f, 0.0000017211f, -0.0000010462f, -0.0000007443f, -0.0000016734f, -0.0000011947f, -0.0000044277f },
		{ 0.0000006068f, 0.0000000343f, -0.0000019199f, 0.0000004676f, 0.0000016005f, 0.0000011427f, -0.0000026032f, -0.0000029654f },
		{ 0.0000001563f, -0.0000000530f, 0.0000034001f, 0.0000000059f, -0.0000023615f, -0.0000007287f, -0.0000010721f, -0.0000026329f },
		{ -0.0000010768f, 0.0000023565f, -0.0000058448f, 0.0000034252f, -0.0000013873f, 0.0000019706f, -0.0000012891f, 0.0000001308f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000002279f, 0.0000049048f, 0.0000008689f, -0.0000009725f, -0.0000107024f, 0.0000080264f, -0.0000062846f, -0.0000005735f },
		{ 0.0000082723f, 0.0000079914f, -0.0000198605f, 0.0000374885f, -0.0000233078f, -0.0000046196f, -0.0000307167f, -0.0000197594f },
		{ -0.0000046805f, 0.0000651044f, 0.0001307625f, 0.0000547062f, -0.0000573698f, -0.0001861038f, -0.0003767852f, -0.0000841606f },
		{ 0.0000738155f, 0.0001036926f, 0.0001706396f, 0.0001442388f, -0.0000756853f, -0.0005107819f, -0.0008439330f, -0.0000955510f },
	},
	{
		{ -0.0000018566f, 0.0000038015f, 0.0000098418f, 0.0000020857f, -0.0000033816f, -0.0000022739f, -0.0000143874f, -0.0000136154f },
		{ 0.0000007144f, 0.0000029680f, 0.0000095750f, -0.0000064658f, 0.0000110034f, -0.0000087367f, -0.0000108051f, -0.0000141752f },
		{ -0.0000032741f, 0.0000088864f, 0.0000044213f, -0.0000016235f, 0.0000006521f, -0.0000075776f, -0.0000056562f, -0.0000122500f },
		{ 0.0000059267f, -0.0000072266f, 0.0000092560f, 0.0000061873f, -0.0000115417f, 0.0000010407f, -0.0000088155f, -0.0000070374f },
		{ 0.0000002279f, 0.0000049048f, 0.0000008689f, -0.0000009725f, -0.0000107024f, 0.0000080264f, -0.0000062846f, -0.0000005735f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ -0.0000043928f, 0.0000034002f, 0.0000270970f, -0.0000141692f, -0.0000180843f, 0.0000076048f, -0.0000088033f, -0.0000051935f },
		{ 0.0000312208f, 0.0000043827f, 0.0001041620f, 0.0000711061f, -0.0000593758f, -0.0001249590f, -0.0002979761f, -0.0000545703f },
		{ 0.0000538007f, 0.0000868877f, 0.0001799591f, 0.0000930510f, -0.0001016721f, -0.0004033289f, -0.0007323207f, -0.0000531937f },
	},
	{
		{ -0.0000104445f, 0.0000205345f, 0.0000120567f, 0.0000257899f, -0.0000066707f, -0.0000169090f, -0.0000538290f, -0.0000436794f },
		{ 0.0000026813f, -0.0000029488f, 0.0000474100f, -0.0000118241f, 0.0000062972f, -0.0000213903f, -0.0000535099f, -0.0000398265f },
		{ 0.0000101578f, -0.0000029554f, 0.0000179556f, -0.0000014105f, 0.0000199752f, -0.0000219123f, -0.0000487328f, -0.0000376679f },
		{ 0.0000018664f, -0.0000024491f, 0.0000368281f, -0.0000040418f, -0.0000118246f, -0.0000018230f, -0.0000464676f, -0.0000279238f },
		{ 0.0000082723f, 0.0000079914f, -0.0000198605f, 0.0000374885f, -0.0000233078f, -0.0000046196f, -0.0000307167f, -0.0000197594f },
		{ -0.0000043928f, 0.0000034002f, 0.0000270970f, -0.0000141692f, -0.0000180843f, 0.0000076048f, -0.0000088033f, -0.0000051935f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000154734f, 0.0000133617f, 0.0000575144f, 0.0000392316f, -0.0000532836f, -0.0000676656f, -0.0002059262f, -0.0000122291f },
		{ 0.0000610649f, 0.0000463357f, 0.0001749739f, 0.0000366946f, -0.0000759560f, -0.0003550955f, -0.0005251615f, -0.0000208710f },
	},
	{
		{ 0.0000101415f, 0.0000526001f, 0.0001356351f, 0.0000880764f, -0.0000408220f, -0.0001975176f, -0.0004651325f, -0.0001442720f },
		{ 0.0000315357f, 0.0000698645f, 0.0001150742f, 0.0000949795f, -0.0000721712f, -0.0001961400f, -0.0004605395f, -0.0001362150f },
		{ 0.0000382143f, 0.0000557039f, 0.0001141389f, 0.0000641683f, -0.0000603524f, -0.0001646873f, -0.0004677995f, -0.0001177953f },
		{ 0.0000226183f, 0.0000967522f, 0.0000800949f, 0.0000918720f, -0.0000541937f, -0.0001952285f, -0.0004243404f, -0.0001085562f },
		{ -0.0000046805f, 0.0000651044f, 0.0001307625f, 0.0000547062f, -0.0000573698f, -0.0001861038f, -0.0003767852f, -0.0000841606f },
		{ 0.0000312208f, 0.0000043827f, 0.0001041620f, 0.0000711061f, -0.0000593758f, -0.0001249590f, -0.0002979761f, -0.0000545703f },
		{ 0.0000154734f, 0.0000133617f, 0.0000575144f, 0.0000392316f, -0.0000532836f, -0.0000676656f, -0.0002059262f, -0.0000122291f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
		{ 0.0000272184f, -0.0000703904f, 0.0000628428f, 0.0000284560f, 0.0000059113f, -0.0000814661f, -0.0000535945f, -0.0000053834f },
	},
	{
		{ 0.0000387190f, 0.0001019353f, 0.0002962154f, 0.0002173503f, -0.0002128060f, -0.0004577889f, -0.0009943054f, -0.0001575785f },
		{ 0.0000777310f, 0.0001591462f, 0.0002988379f, 0.0000637254f, -0.0001243673f, -0.0004971842f, -0.0009732923f, -0.0001563941f },
		{ 0.0000534207f, 0.0001435421f, 0.0002682869f, 0.0000957538f, -0.0001058051f, -0.0005031019f, -0.0009351934f, -0.0001531325f },
		{ 0.0000401601f, 0.0001956132f, 0.0002557263f, 0.0000554730f, -0.0001149196f, -0.0004906901f, -0.0009086402f, -0.0001251049f },
		{ 0.0000738155f, 0.0001036926f, 0.0001706396f, 0.0001442388f, -0.0000756853f, -0.0005107819f, -0.0008439330f, -0.0000955510f },
		{ 0.0000538007f, 0.0000868877f, 0.0001799591f, 0.0000930510f, -0.0001016721f, -0.0004033289f, -0.0007323207f, -0.0000531937f },
		{ 0.0000610649f, 0.0000463357f, 0.0001749739f, 0.0000366946f, -0.0000759560f, -0.0003550955f, -0.0005251615f, -0.0000208710f },
		{ 0.0000272184f, -0.0000703904f, 0.0000628428f, 0.0000284560f, 0.0000059113f, -0.0000814661f, -0.0000535945f, -0.0000053834f },
		{ 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f, 0.0000000000f },
	},
};
	//! DL-160: resolve a low-alpha VIRTUAL node index (as returned by
	//! MicrofacetEnergyLUT::AlphaLowIndex, which this table's alpha
	//! axis shares byte-for-byte with MicrofacetEnergyLUT.h's own
	//! E_avg_TABLE_G2 -- see this file's header comment) to a weight
	//! for quadrature node `nodeIdx`.  Reuses MicrofacetEnergyLUT's own
	//! ALPHA_SUB_FINE/ALPHA_LOW_TOTAL/AlphaLowSlot constants/helper so
	//! the bracketing arithmetic cannot drift from DL-105's.
	inline Scalar AlphaLowQuadWeight( const int idx, const int nodeIdx )
	{
		if( idx <= 0 ) return kGGXSpecularQuadWeightAlphaZero[nodeIdx];
		if( idx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ) return kGGXSpecularQuadWeight[0][nodeIdx];
		if( idx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL ) return kGGXSpecularQuadWeight[1][nodeIdx];
		return kGGXSpecularQuadWeightAlphaLow[ MicrofacetEnergyLUT::AlphaLowSlot(idx) ][nodeIdx];
	}

	//! Linear interpolation across the alpha axis (same [0.01,1.0]
	//! uniform-32-node mapping as MicrofacetEnergyLUT::LookupEavgG2),
	//! returning the weight for quadrature node `nodeIdx`.
	//!
	//! DL-160: below MicrofacetEnergyLUT::ALPHA_LOW_A1 (row 1's alpha)
	//! this used to clamp unconditionally to row 0 -- covering both the
	//! alpha<0.01 clamp and the coarse row0->row1 cell (a 4.2x ratio in
	//! one interpolation cell) -- the SAME defect DL-105 fixed on
	//! MicrofacetEnergyLUT's E_ss/E_avg tables.  Now sources the row
	//! from the low-alpha sub-grid via the shared
	//! MicrofacetEnergyLUT::AlphaLowIndex bracket instead.
	inline Scalar LookupGGXSpecularQuadWeight( const int nodeIdx, const Scalar alpha )
	{
		if( alpha < MicrofacetEnergyLUT::ALPHA_LOW_A1 )
		{
			int idx0, idx1; Scalar f;
			MicrofacetEnergyLUT::AlphaLowIndex( alpha, idx0, idx1, f );
			return ( Scalar(1) - f ) * AlphaLowQuadWeight( idx0, nodeIdx ) + f * AlphaLowQuadWeight( idx1, nodeIdx );
		}
		const int kNumAlphaBins = 32;
		Scalar a = r_max( Scalar(0), r_min( Scalar(1), (alpha - Scalar(0.01)) / Scalar(0.99) ) ) * Scalar(kNumAlphaBins - 1);
		int ai0 = (int)a;
		int ai1 = r_min( ai0 + 1, kNumAlphaBins - 1 );
		Scalar af = a - Scalar(ai0);
		return kGGXSpecularQuadWeight[ai0][nodeIdx] * (Scalar(1) - af) + kGGXSpecularQuadWeight[ai1][nodeIdx] * af;
	}

	inline Scalar AnisoQuadCorrectionCellLowOrd( const int lowIdx, const int ordinaryIdx, const int nodeIdx )
	{
		const int idx = ( lowIdx <= 0 ) ? 1 : lowIdx;
		if( idx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ) {
			return kGGXSpecularQuadCorrectionAniso[0][ordinaryIdx][nodeIdx];
		}
		if( idx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL ) {
			return kGGXSpecularQuadCorrectionAniso[1][ordinaryIdx][nodeIdx];
		}
		return kGGXSpecularQuadCorrectionAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(idx)][ordinaryIdx][nodeIdx];
	}

	inline Scalar AnisoQuadCorrectionCellLowLow( const int xIdxRaw, const int yIdxRaw, const int nodeIdx )
	{
		const int xIdx = ( xIdxRaw <= 0 ) ? 1 : xIdxRaw;
		const int yIdx = ( yIdxRaw <= 0 ) ? 1 : yIdxRaw;
		const bool xBoundary = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 || xIdx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL;
		const bool yBoundary = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 || yIdx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL;
		if( xBoundary && yBoundary ) {
			const int xo = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			const int yo = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadCorrectionAniso[xo][yo][nodeIdx];
		}
		if( yBoundary ) {
			const int yo = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadCorrectionAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(xIdx)][yo][nodeIdx];
		}
		if( xBoundary ) {
			const int xo = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadCorrectionAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(yIdx)][xo][nodeIdx];
		}
		return kGGXSpecularQuadCorrectionAnisoLowLow
			[MicrofacetEnergyLUT::AlphaLowSlot(xIdx)]
			[MicrofacetEnergyLUT::AlphaLowSlot(yIdx)][nodeIdx];
	}

	inline Scalar BilinearAnisoCorrection(
		const Scalar v00, const Scalar v01, const Scalar v10, const Scalar v11,
		const Scalar xf, const Scalar yf )
	{
		return ( Scalar(1) - xf ) * ( ( Scalar(1) - yf ) * v00 + yf * v01 )
			+ xf * ( ( Scalar(1) - yf ) * v10 + yf * v11 );
	}

	inline Scalar DiagonalTriangleAnisoCorrection(
		const Scalar v00, const Scalar v01, const Scalar v11,
		const Scalar xf, const Scalar yf )
	{
		// Sorted alphaX<=alphaY implies xf<=yf inside a shared cell.  These
		// are the barycentric weights of the upper triangle with vertices
		// (0,0), (0,1), (1,1).  Its diagonal edge interpolates only v00/v11;
		// both are exact zero-correction anchors, so the correction approaches
		// zero continuously everywhere on the diagonal.
		return ( Scalar(1) - yf ) * v00 + ( yf - xf ) * v01 + xf * v11;
	}

	inline Scalar LookupGGXSpecularQuadWeightAniso(
		const int nodeIdx, const Scalar alphaXInput, const Scalar alphaYInput )
	{
		// Load-bearing identity contract: isotropic materials use the
		// pre-DL-139 literals and interpolation path bit-for-bit.
		if( alphaXInput == alphaYInput ) {
			return LookupGGXSpecularQuadWeight( nodeIdx, alphaXInput );
		}

		const Scalar alphaX = r_min( alphaXInput, alphaYInput );
		const Scalar alphaY = r_max( alphaXInput, alphaYInput );
		const Scalar isotropicBase = LookupGGXSpecularQuadWeight(
			nodeIdx, sqrt( alphaX * alphaY ) );
		const bool xLow = alphaX < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;
		const bool yLow = alphaY < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;

		if( !xLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = kGGXSpecularQuadCorrectionAniso[xi0][yi0][nodeIdx];
			const Scalar v01 = kGGXSpecularQuadCorrectionAniso[xi0][yi1][nodeIdx];
			const Scalar v10 = kGGXSpecularQuadCorrectionAniso[xi1][yi0][nodeIdx];
			const Scalar v11 = kGGXSpecularQuadCorrectionAniso[xi1][yi1][nodeIdx];
			const Scalar correction = xi0 == yi0
				? DiagonalTriangleAnisoCorrection( v00, v01, v11, xf, yf )
				: BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
			return isotropicBase + correction;
		}

		if( !yLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = AnisoQuadCorrectionCellLowOrd( xi0, yi0, nodeIdx );
			const Scalar v01 = AnisoQuadCorrectionCellLowOrd( xi0, yi1, nodeIdx );
			const Scalar v10 = AnisoQuadCorrectionCellLowOrd( xi1, yi0, nodeIdx );
			const Scalar v11 = AnisoQuadCorrectionCellLowOrd( xi1, yi1, nodeIdx );
			return isotropicBase + BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
		}

		int xi0, xi1, yi0, yi1; Scalar xf, yf;
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaY, yi0, yi1, yf );
		const Scalar v00 = AnisoQuadCorrectionCellLowLow( xi0, yi0, nodeIdx );
		const Scalar v01 = AnisoQuadCorrectionCellLowLow( xi0, yi1, nodeIdx );
		const Scalar v10 = AnisoQuadCorrectionCellLowLow( xi1, yi0, nodeIdx );
		const Scalar v11 = AnisoQuadCorrectionCellLowLow( xi1, yi1, nodeIdx );
		const Scalar correction = xi0 == yi0
			? DiagonalTriangleAnisoCorrection( v00, v01, v11, xf, yf )
			: BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
		return isotropicBase + correction;
	}

	inline Scalar AnisoAlphaNodeValue( const int idx )
	{
		return Scalar(0.01) + Scalar(0.99) * Scalar(idx)
			/ Scalar(MicrofacetEnergyLUT::ANISO_ALPHA_SIZE - 1);
	}

	inline int FoldAnisoLowIndex( const int idx )
	{
		return idx <= 0 ? 1 : idx;
	}

	inline Scalar EavgAnisoCorrectionOrdinary( const int xi, const int yi )
	{
		if( xi == yi ) return Scalar(0);
		const Scalar alphaX = AnisoAlphaNodeValue( xi );
		const Scalar alphaY = AnisoAlphaNodeValue( yi );
		return MicrofacetEnergyLUT::E_avg_TABLE_G2_ANISO[xi][yi]
			- MicrofacetEnergyLUT::LookupEavgG2( sqrt( alphaX * alphaY ) );
	}

	inline Scalar EavgAnisoCorrectionLowOrd(
		const int lowIdxRaw, const int ordinaryIdx )
	{
		const int lowIdx = FoldAnisoLowIndex( lowIdxRaw );
		const bool isA0Diagonal = lowIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1
			&& ordinaryIdx == 0;
		const bool isA1Diagonal = lowIdx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL
			&& ordinaryIdx == 1;
		if( isA0Diagonal || isA1Diagonal ) return Scalar(0);
		const Scalar alphaX = MicrofacetEnergyLUT::AnisoAlphaLowNode( lowIdx );
		const Scalar alphaY = AnisoAlphaNodeValue( ordinaryIdx );
		return MicrofacetEnergyLUT::AnisoEavgCellMixedX( true, lowIdx, ordinaryIdx )
			- MicrofacetEnergyLUT::LookupEavgG2( sqrt( alphaX * alphaY ) );
	}

	inline Scalar EavgAnisoCorrectionLowLow(
		const int xIdxRaw, const int yIdxRaw )
	{
		const int xIdx = FoldAnisoLowIndex( xIdxRaw );
		const int yIdx = FoldAnisoLowIndex( yIdxRaw );
		if( xIdx == yIdx ) return Scalar(0);
		const Scalar alphaX = MicrofacetEnergyLUT::AnisoAlphaLowNode( xIdx );
		const Scalar alphaY = MicrofacetEnergyLUT::AnisoAlphaLowNode( yIdx );
		return MicrofacetEnergyLUT::AnisoEavgCellBothLow( xIdx, yIdx )
			- MicrofacetEnergyLUT::LookupEavgG2( sqrt( alphaX * alphaY ) );
	}

	//! Albedo-local continuous twin of LookupEavgG2Aniso.  The shared LUT
	//! predates DL-139 and retains a 1e-9 near-isotropic forwarding band;
	//! using it here would merely move the public albedo discontinuity to
	//! that band's edge.  Apply the same exact diagonal-boundary formulation
	//! as the new single-scatter table without changing the shared sampler/
	//! evaluator contract outside hemisphericalAlbedo{,NM}.
	inline Scalar LookupEavgG2AnisoForHemisphericalAlbedo(
		const Scalar alphaXInput, const Scalar alphaYInput )
	{
		if( alphaXInput == alphaYInput ) {
			return MicrofacetEnergyLUT::LookupEavgG2( alphaXInput );
		}
		const Scalar alphaX = r_min( alphaXInput, alphaYInput );
		const Scalar alphaY = r_max( alphaXInput, alphaYInput );
		const Scalar isotropicBase = MicrofacetEnergyLUT::LookupEavgG2(
			sqrt( alphaX * alphaY ) );
		const bool xLow = alphaX < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;
		const bool yLow = alphaY < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;

		if( !xLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = EavgAnisoCorrectionOrdinary( xi0, yi0 );
			const Scalar v01 = EavgAnisoCorrectionOrdinary( xi0, yi1 );
			const Scalar v10 = EavgAnisoCorrectionOrdinary( xi1, yi0 );
			const Scalar v11 = EavgAnisoCorrectionOrdinary( xi1, yi1 );
			const Scalar correction = xi0 == yi0
				? DiagonalTriangleAnisoCorrection( v00, v01, v11, xf, yf )
				: BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
			return isotropicBase + correction;
		}

		if( !yLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = EavgAnisoCorrectionLowOrd( xi0, yi0 );
			const Scalar v01 = EavgAnisoCorrectionLowOrd( xi0, yi1 );
			const Scalar v10 = EavgAnisoCorrectionLowOrd( xi1, yi0 );
			const Scalar v11 = EavgAnisoCorrectionLowOrd( xi1, yi1 );
			return isotropicBase + BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
		}

		int xi0, xi1, yi0, yi1; Scalar xf, yf;
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaY, yi0, yi1, yf );
		const Scalar v00 = EavgAnisoCorrectionLowLow( xi0, yi0 );
		const Scalar v01 = EavgAnisoCorrectionLowLow( xi0, yi1 );
		const Scalar v10 = EavgAnisoCorrectionLowLow( xi1, yi0 );
		const Scalar v11 = EavgAnisoCorrectionLowLow( xi1, yi1 );
		const Scalar correction = xi0 == yi0
			? DiagonalTriangleAnisoCorrection( v00, v01, v11, xf, yf )
			: BilinearAnisoCorrection( v00, v01, v10, v11, xf, yf );
		return isotropicBase + correction;
	}

	//! R_ss(alpha) = SUM_i F(mu_i) * w_i(alpha) -- see the long
	//! derivation comment above GGXBRDF::hemisphericalAlbedo.  `F` is
	//! whatever Fresnel function `interfaceFresnel.Directional` (RGB) /
	//! `DirectionalNM` (NM) dispatches to for the material's actual
	//! FresnelMode -- this helper is mode-agnostic.
	RISEPel GGXSpecularSingleScatterBihemispherical(
		const GGXInterfaceFresnel& interfaceFresnel, const Scalar alphaX, const Scalar alphaY )
	{
		RISEPel sum(0,0,0);
		for( int i = 0; i < kGGXSpecularQuadNumNodes; i++ ) {
			sum = sum + interfaceFresnel.Directional( kGGXSpecularQuadNodes[i] )
				* LookupGGXSpecularQuadWeightAniso( i, alphaX, alphaY );
		}
		return sum;
	}
	Scalar GGXSpecularSingleScatterBihemisphericalNM(
		const GGXInterfaceFresnel& interfaceFresnel, const Scalar alphaX, const Scalar alphaY, const Scalar nm )
	{
		Scalar sum = 0;
		for( int i = 0; i < kGGXSpecularQuadNumNodes; i++ ) {
			sum += interfaceFresnel.DirectionalNM( kGGXSpecularQuadNodes[i], nm )
				* LookupGGXSpecularQuadWeightAniso( i, alphaX, alphaY );
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

	//! Resolve alphaX/alphaY with the same DL-62 glossy-filter widening
	//! and 1e-4 floor value()/valueNM apply.  DL-139 removed the former
	//! alphaEff collapse: both R_ss and R_ms now consume the real pair.
	void ResolveGGXHemisphericalAlphas( const IScalarPainter& alphaXP, const IScalarPainter& alphaYP,
		const RayIntersectionGeometric& ri, Scalar& outAlphaX, Scalar& outAlphaY )
	{
		Scalar alphaX = alphaXP.GetValuesAt(ri).v[0];
		Scalar alphaY = alphaYP.GetValuesAt(ri).v[0];
		if( ri.glossyFilterWidth > 0 ) {
			alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
			alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
		}
		outAlphaX = r_max( alphaX, Scalar(1e-4) );
		outAlphaY = r_max( alphaY, Scalar(1e-4) );
	}
	void ResolveGGXHemisphericalAlphasNM( const IScalarPainter& alphaXP, const IScalarPainter& alphaYP,
		const RayIntersectionGeometric& ri, const Scalar nm, Scalar& outAlphaX, Scalar& outAlphaY )
	{
		Scalar alphaX = alphaXP.GetValueAtNM(ri,nm);
		Scalar alphaY = alphaYP.GetValueAtNM(ri,nm);
		if( ri.glossyFilterWidth > 0 ) {
			alphaX = r_min( alphaX + ri.glossyFilterWidth, Scalar(1.0) );
			alphaY = r_min( alphaY + ri.glossyFilterWidth, Scalar(1.0) );
		}
		outAlphaX = r_max( alphaX, Scalar(1e-4) );
		outAlphaY = r_max( alphaY, Scalar(1e-4) );
	}
}

bool GGXBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	const GGXInterfaceFresnel interfaceFresnel { ri, fresnelMode, *pSpecular, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness };
	const RISEPel mean = interfaceFresnel.Mean();

	Scalar alphaX, alphaY;
	ResolveGGXHemisphericalAlphas( *pAlphaX, *pAlphaY, ri, alphaX, alphaY );
	const Scalar EavgAniso = LookupEavgG2AnisoForHemisphericalAlbedo( alphaX, alphaY );
	const RISEPel R_ss = GGXSpecularSingleScatterBihemispherical( interfaceFresnel, alphaX, alphaY );
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

	Scalar alphaX, alphaY;
	ResolveGGXHemisphericalAlphasNM( *pAlphaX, *pAlphaY, ri, nm, alphaX, alphaY );
	const Scalar EavgAniso = LookupEavgG2AnisoForHemisphericalAlbedo( alphaX, alphaY );
	const Scalar R_ss = GGXSpecularSingleScatterBihemisphericalNM( interfaceFresnel, alphaX, alphaY, nm );
	const Scalar specColor = GuardedGetColorNM( *pSpecular, ri, nm );
	const Scalar F_ms = ComputeGGXFmsNM( ri, fresnelMode, specColor, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness, nm, EavgAniso );
	const Scalar R_ms = F_ms * (Scalar(1) - EavgAniso);

	out = R_ss + R_ms + GuardedGetColorNM( *pDiffuse, ri, nm ) * GGXInterfaceFresnel::Transmission( mean, mean );
	return true;
}
