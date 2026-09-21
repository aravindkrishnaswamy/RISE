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
// ANISOTROPIC (alphaX != alphaY), DL-139: R_ms/Eavg already used the
// anisotropic LookupEavgG2Aniso.  R_ss now likewise resolves both
// roughness eigenvalues on a 24x24 reachable grid plus DL-161-style
// low-alpha sub-grids.  The bihemispherical quantity integrates a full
// white sky, so material-frame rotation is a change of variables and no
// phi axis is present.  alphaX==alphaY forwards exactly to the original
// isotropic literals/interpolation path (slot-by-slot identity retained).
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

// DL-139: azimuth-averaged anisotropic single-scatter quadrature.
// Both ordinary alpha axes use MicrofacetEnergyLUT::ANISO_ALPHA_SIZE=24;
// low axes use its AnisoAlphaLowNode/AlphaLowSlot layout.  The virtual
// alpha->0 node folds to low slot 0 because production alpha is floored
// at 1e-4 and the off-diagonal limit has no isotropic closed form.
// Ordinary diagonal cells interpolate the preserved isotropic table; only
// off-diagonal cells use the independent anisotropic stream.
// Independently seeded stream, numMuI=48, numSamples=100000. DO NOT HAND-EDIT.

static const Scalar kGGXSpecularQuadWeightAniso[ 24 ][ 24 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0196541649f, 0.1813501614f, 0.2865772823f, 0.2665871764f, 0.1707216309f, 0.0653173630f, 0.0098298467f, -0.0005527483f },
		{ 0.0199511418f, 0.1818783766f, 0.2875828588f, 0.2668085512f, 0.1697848101f, 0.0623249301f, 0.0067939876f, -0.0004320602f },
		{ 0.0201223777f, 0.1831429875f, 0.2888778548f, 0.2664451129f, 0.1669814601f, 0.0561343158f, 0.0042356338f, -0.0001281772f },
		{ 0.0205505062f, 0.1842771527f, 0.2903317811f, 0.2651550068f, 0.1613735832f, 0.0493825793f, 0.0028563815f, 0.0000025047f },
		{ 0.0207270812f, 0.1859120445f, 0.2912890491f, 0.2624017339f, 0.1542404268f, 0.0430583607f, 0.0021891533f, -0.0000000477f },
		{ 0.0213434985f, 0.1875245871f, 0.2913406815f, 0.2582267549f, 0.1459844154f, 0.0376677857f, 0.0018620504f, -0.0000477142f },
		{ 0.0214613452f, 0.1887580211f, 0.2907237471f, 0.2532363797f, 0.1376604725f, 0.0331510708f, 0.0017130607f, -0.0001166764f },
		{ 0.0218657539f, 0.1896457058f, 0.2894548585f, 0.2466624042f, 0.1293738942f, 0.0295383876f, 0.0015785874f, -0.0001496780f },
		{ 0.0219655766f, 0.1900691074f, 0.2872654675f, 0.2396550965f, 0.1213511941f, 0.0264422645f, 0.0014908243f, -0.0001824004f },
		{ 0.0224590788f, 0.1900465497f, 0.2839820856f, 0.2324278584f, 0.1138492777f, 0.0238998043f, 0.0013912993f, -0.0001912000f },
		{ 0.0226775268f, 0.1896335727f, 0.2806735515f, 0.2241300990f, 0.1068374784f, 0.0218744750f, 0.0012585976f, -0.0001768779f },
		{ 0.0226668030f, 0.1894298219f, 0.2759415361f, 0.2162771154f, 0.1007062772f, 0.0199333550f, 0.0012771241f, -0.0002214295f },
		{ 0.0228695840f, 0.1881846496f, 0.2710803567f, 0.2083200560f, 0.0946404793f, 0.0183335677f, 0.0012464246f, -0.0002378574f },
		{ 0.0226910344f, 0.1870926493f, 0.2660236879f, 0.2002232068f, 0.0891636206f, 0.0170009431f, 0.0011785374f, -0.0002318295f },
		{ 0.0229104517f, 0.1852894852f, 0.2602196230f, 0.1924819989f, 0.0842523589f, 0.0157966897f, 0.0011156135f, -0.0002253741f },
		{ 0.0227877497f, 0.1830462651f, 0.2542185746f, 0.1851406079f, 0.0799179156f, 0.0146807207f, 0.0011065787f, -0.0002448787f },
		{ 0.0227018945f, 0.1807941942f, 0.2484785316f, 0.1776089704f, 0.0755710404f, 0.0138192422f, 0.0010492149f, -0.0002330124f },
		{ 0.0225446905f, 0.1780958000f, 0.2420424640f, 0.1706433326f, 0.0717435212f, 0.0129152932f, 0.0010236067f, -0.0002397612f },
		{ 0.0224117887f, 0.1754341882f, 0.2358673286f, 0.1639127475f, 0.0681791745f, 0.0121652720f, 0.0009673781f, -0.0002264285f },
		{ 0.0223491209f, 0.1726249489f, 0.2293372611f, 0.1573987532f, 0.0647097917f, 0.0115422412f, 0.0009310121f, -0.0002190136f },
		{ 0.0217973736f, 0.1698899541f, 0.2231934658f, 0.1510642733f, 0.0618207616f, 0.0107810948f, 0.0009489990f, -0.0002418178f },
		{ 0.0217377894f, 0.1669329122f, 0.2166703547f, 0.1453948162f, 0.0588292487f, 0.0102025784f, 0.0009426824f, -0.0002500342f },
		{ 0.0214802908f, 0.1635557979f, 0.2104988803f, 0.1398465761f, 0.0562478337f, 0.0097128126f, 0.0009059437f, -0.0002424044f },
		{ 0.0213883278f, 0.1601283515f, 0.2045333703f, 0.1345145040f, 0.0537106141f, 0.0092620092f, 0.0008467737f, -0.0002236253f },
	},
	{
		{ 0.0199511418f, 0.1818783766f, 0.2875828588f, 0.2668085512f, 0.1697848101f, 0.0623249301f, 0.0067939876f, -0.0004320602f },
		{ 0.0200415251f, 0.1825687747f, 0.2884979465f, 0.2667197782f, 0.1685334287f, 0.0589955453f, 0.0047984126f, -0.0002376846f },
		{ 0.0202740533f, 0.1835554799f, 0.2899350127f, 0.2661383158f, 0.1657295489f, 0.0534818837f, 0.0026658556f, 0.0000596589f },
		{ 0.0207721981f, 0.1847629031f, 0.2908057079f, 0.2649973740f, 0.1599770210f, 0.0470431280f, 0.0016891618f, 0.0001378804f },
		{ 0.0210670581f, 0.1863454624f, 0.2913203682f, 0.2623920217f, 0.1528189798f, 0.0410580605f, 0.0012787048f, 0.0001008843f },
		{ 0.0214864436f, 0.1876246733f, 0.2917569736f, 0.2581897428f, 0.1445813221f, 0.0359076686f, 0.0010651653f, 0.0000554196f },
		{ 0.0217178436f, 0.1888909209f, 0.2912319643f, 0.2526540872f, 0.1363789246f, 0.0315627125f, 0.0010322479f, -0.0000238418f },
		{ 0.0221015609f, 0.1900440560f, 0.2893203780f, 0.2461307087f, 0.1283612794f, 0.0279503844f, 0.0011011108f, -0.0001225291f },
		{ 0.0221591434f, 0.1904053015f, 0.2874906156f, 0.2393089408f, 0.1203302574f, 0.0250907099f, 0.0010495935f, -0.0001433667f },
		{ 0.0223147548f, 0.1906219624f, 0.2845384557f, 0.2316896034f, 0.1128653672f, 0.0227899848f, 0.0009684234f, -0.0001489947f },
		{ 0.0225138187f, 0.1904152298f, 0.2811078346f, 0.2233686881f, 0.1059434684f, 0.0207179936f, 0.0009661965f, -0.0001721925f },
		{ 0.0227164253f, 0.1891397339f, 0.2763779477f, 0.2157954985f, 0.0997445690f, 0.0189416945f, 0.0009485871f, -0.0001920176f },
		{ 0.0228647819f, 0.1885420129f, 0.2712602864f, 0.2078800013f, 0.0939120572f, 0.0174188147f, 0.0009450469f, -0.0002121757f },
		{ 0.0229683719f, 0.1872395426f, 0.2659237184f, 0.2000594754f, 0.0884445050f, 0.0161364772f, 0.0009326009f, -0.0002202483f },
		{ 0.0227376459f, 0.1853364840f, 0.2603052602f, 0.1923190991f, 0.0835188228f, 0.0149832555f, 0.0008849143f, -0.0002155412f },
		{ 0.0228694894f, 0.1830519999f, 0.2545091300f, 0.1848080479f, 0.0791268139f, 0.0140146910f, 0.0008539075f, -0.0002131896f },
		{ 0.0227393399f, 0.1813058632f, 0.2485850091f, 0.1769993844f, 0.0748810973f, 0.0130736566f, 0.0008759344f, -0.0002352057f },
		{ 0.0225631776f, 0.1781870117f, 0.2422006006f, 0.1703823465f, 0.0709456914f, 0.0122617004f, 0.0008406076f, -0.0002290572f },
		{ 0.0225858604f, 0.1757484914f, 0.2355883427f, 0.1637102919f, 0.0674459768f, 0.0115807171f, 0.0008045930f, -0.0002217913f },
		{ 0.0222349298f, 0.1729918436f, 0.2293915518f, 0.1569971300f, 0.0643949260f, 0.0108832611f, 0.0007962756f, -0.0002272904f },
		{ 0.0221337732f, 0.1698674779f, 0.2229073450f, 0.1510517919f, 0.0612939561f, 0.0103864852f, 0.0007283891f, -0.0002029453f },
		{ 0.0217350271f, 0.1667428320f, 0.2173002501f, 0.1448922109f, 0.0583218776f, 0.0098140072f, 0.0007478073f, -0.0002164977f },
		{ 0.0214529046f, 0.1636815364f, 0.2106259623f, 0.1394411007f, 0.0558089870f, 0.0092533281f, 0.0007624973f, -0.0002313235f },
		{ 0.0213997681f, 0.1606714041f, 0.2042814624f, 0.1337310115f, 0.0532881421f, 0.0087980044f, 0.0007336855f, -0.0002237366f },
	},
	{
		{ 0.0201223777f, 0.1831429875f, 0.2888778548f, 0.2664451129f, 0.1669814601f, 0.0561343158f, 0.0042356338f, -0.0001281772f },
		{ 0.0202740533f, 0.1835554799f, 0.2899350127f, 0.2661383158f, 0.1657295489f, 0.0534818837f, 0.0026658556f, 0.0000596589f },
		{ 0.0206453356f, 0.1845034571f, 0.2909861522f, 0.2655952099f, 0.1618522223f, 0.0482460597f, 0.0014828077f, 0.0002054996f },
		{ 0.0209964361f, 0.1857212062f, 0.2919446579f, 0.2641801941f, 0.1564649152f, 0.0424305031f, 0.0007115772f, 0.0002609866f },
		{ 0.0212826371f, 0.1873502282f, 0.2926508425f, 0.2611472094f, 0.1493607563f, 0.0369416425f, 0.0005464093f, 0.0001832345f },
		{ 0.0214637304f, 0.1889003373f, 0.2923821830f, 0.2569242489f, 0.1414058983f, 0.0322203319f, 0.0005816691f, 0.0000697595f },
		{ 0.0219672817f, 0.1897919056f, 0.2917333125f, 0.2516078181f, 0.1330665657f, 0.0284042552f, 0.0005577270f, 0.0000184666f },
		{ 0.0222155274f, 0.1903545477f, 0.2899399293f, 0.2454656890f, 0.1251616468f, 0.0252411497f, 0.0006190027f, -0.0000543481f },
		{ 0.0225153633f, 0.1911456248f, 0.2876568267f, 0.2382277753f, 0.1174583306f, 0.0224661669f, 0.0006977789f, -0.0001220351f },
		{ 0.0227086610f, 0.1909557644f, 0.2846350898f, 0.2306137120f, 0.1101897340f, 0.0204127036f, 0.0006702054f, -0.0001315509f },
		{ 0.0228975423f, 0.1905109213f, 0.2811543564f, 0.2229847146f, 0.1033930868f, 0.0185739904f, 0.0006868205f, -0.0001570559f },
		{ 0.0228275843f, 0.1904753799f, 0.2761025576f, 0.2146131425f, 0.0975008212f, 0.0169737087f, 0.0007347826f, -0.0001933354f },
		{ 0.0229689448f, 0.1890647622f, 0.2718127333f, 0.2066070669f, 0.0914586145f, 0.0156746679f, 0.0006673719f, -0.0001747368f },
		{ 0.0230550887f, 0.1876446188f, 0.2659223577f, 0.1988891689f, 0.0866408508f, 0.0143854423f, 0.0007458079f, -0.0002226890f },
		{ 0.0229897753f, 0.1856281122f, 0.2604846035f, 0.1913330180f, 0.0818245370f, 0.0134763928f, 0.0006620404f, -0.0001922439f },
		{ 0.0230770190f, 0.1833988034f, 0.2543567332f, 0.1836375966f, 0.0774356927f, 0.0126261503f, 0.0006475436f, -0.0001940306f },
		{ 0.0228026589f, 0.1815797060f, 0.2485923335f, 0.1762012380f, 0.0733308801f, 0.0117938509f, 0.0006316946f, -0.0001933494f },
		{ 0.0227099017f, 0.1786441124f, 0.2422401862f, 0.1694067035f, 0.0696206504f, 0.0110975802f, 0.0006622916f, -0.0002127060f },
		{ 0.0226083034f, 0.1764167309f, 0.2356512673f, 0.1627393570f, 0.0660638263f, 0.0104655933f, 0.0006267322f, -0.0002010687f },
		{ 0.0223505924f, 0.1734293153f, 0.2294004850f, 0.1560830688f, 0.0628883858f, 0.0098732751f, 0.0006113103f, -0.0002001525f },
		{ 0.0221382896f, 0.1700861524f, 0.2231136217f, 0.1501012347f, 0.0599545534f, 0.0093115272f, 0.0006375785f, -0.0002147588f },
		{ 0.0217836779f, 0.1670034663f, 0.2170106286f, 0.1443190126f, 0.0570583258f, 0.0088856576f, 0.0005850939f, -0.0001953270f },
		{ 0.0218315558f, 0.1641614896f, 0.2106617094f, 0.1387032649f, 0.0546840476f, 0.0083506249f, 0.0006183180f, -0.0002144300f },
		{ 0.0213073521f, 0.1607634838f, 0.2049157583f, 0.1334201157f, 0.0522385346f, 0.0079987416f, 0.0005945997f, -0.0002060877f },
	},
	{
		{ 0.0205505062f, 0.1842771527f, 0.2903317811f, 0.2651550068f, 0.1613735832f, 0.0493825793f, 0.0028563815f, 0.0000025047f },
		{ 0.0207721981f, 0.1847629031f, 0.2908057079f, 0.2649973740f, 0.1599770210f, 0.0470431280f, 0.0016891618f, 0.0001378804f },
		{ 0.0209964361f, 0.1857212062f, 0.2919446579f, 0.2641801941f, 0.1564649152f, 0.0424305031f, 0.0007115772f, 0.0002609866f },
		{ 0.0210882678f, 0.1872724038f, 0.2927444937f, 0.2624832164f, 0.1509472113f, 0.0371239317f, 0.0004294367f, 0.0002014678f },
		{ 0.0215040981f, 0.1883883369f, 0.2932418081f, 0.2596626233f, 0.1437765094f, 0.0324691938f, 0.0002854484f, 0.0001504447f },
		{ 0.0218179728f, 0.1900304114f, 0.2932315022f, 0.2548788849f, 0.1361658208f, 0.0281979053f, 0.0004014998f, 0.0000279055f },
		{ 0.0222563629f, 0.1908564805f, 0.2921590660f, 0.2496078008f, 0.1282595485f, 0.0247569991f, 0.0004983810f, -0.0000589293f },
		{ 0.0223295094f, 0.1918952287f, 0.2905782641f, 0.2428586094f, 0.1204991461f, 0.0220170343f, 0.0005472088f, -0.0001088805f },
		{ 0.0225174653f, 0.1922234947f, 0.2881575069f, 0.2360862603f, 0.1130849297f, 0.0196344548f, 0.0006303864f, -0.0001674917f },
		{ 0.0228026656f, 0.1919705901f, 0.2849383145f, 0.2283875965f, 0.1062237958f, 0.0177548437f, 0.0006399255f, -0.0001847252f },
		{ 0.0231297068f, 0.1915022388f, 0.2809098308f, 0.2210420488f, 0.0995873885f, 0.0162459967f, 0.0005391213f, -0.0001513621f },
		{ 0.0232527004f, 0.1906557279f, 0.2769063395f, 0.2126626918f, 0.0938191706f, 0.0148402477f, 0.0006113629f, -0.0001920989f },
		{ 0.0231574213f, 0.1901703525f, 0.2715345281f, 0.2046426179f, 0.0883227216f, 0.0136261904f, 0.0006428011f, -0.0002159715f },
		{ 0.0232246965f, 0.1882015715f, 0.2665130264f, 0.1967403792f, 0.0833540361f, 0.0125831317f, 0.0006506600f, -0.0002270307f },
		{ 0.0233153883f, 0.1862083944f, 0.2603278163f, 0.1894650141f, 0.0788608768f, 0.0117727616f, 0.0006069186f, -0.0002112360f },
		{ 0.0231875261f, 0.1846099781f, 0.2541026656f, 0.1818550231f, 0.0746572618f, 0.0109185835f, 0.0006432425f, -0.0002329359f },
		{ 0.0228422269f, 0.1821933579f, 0.2482942046f, 0.1745716889f, 0.0707559489f, 0.0102859418f, 0.0006151229f, -0.0002244558f },
		{ 0.0229418099f, 0.1793723332f, 0.2420940947f, 0.1679189780f, 0.0671257094f, 0.0096838291f, 0.0005953662f, -0.0002186238f },
		{ 0.0224738497f, 0.1764267824f, 0.2357274180f, 0.1617806587f, 0.0638016552f, 0.0090886937f, 0.0005944892f, -0.0002207822f },
		{ 0.0224831583f, 0.1739315819f, 0.2293165592f, 0.1547932014f, 0.0607236311f, 0.0085834758f, 0.0005992100f, -0.0002260289f },
		{ 0.0222068694f, 0.1705939716f, 0.2230771033f, 0.1489974922f, 0.0579932503f, 0.0081662240f, 0.0005856631f, -0.0002242287f },
		{ 0.0220416492f, 0.1673097666f, 0.2167947087f, 0.1431616347f, 0.0552126617f, 0.0077523787f, 0.0005519869f, -0.0002113376f },
		{ 0.0216389002f, 0.1640686454f, 0.2108611957f, 0.1376164966f, 0.0528487202f, 0.0073377271f, 0.0005602407f, -0.0002159568f },
		{ 0.0214386500f, 0.1609667741f, 0.2044949951f, 0.1322566962f, 0.0506393271f, 0.0069368508f, 0.0005822504f, -0.0002282776f },
	},
	{
		{ 0.0207270812f, 0.1859120445f, 0.2912890491f, 0.2624017339f, 0.1542404268f, 0.0430583607f, 0.0021891533f, -0.0000000477f },
		{ 0.0210670581f, 0.1863454624f, 0.2913203682f, 0.2623920217f, 0.1528189798f, 0.0410580605f, 0.0012787048f, 0.0001008843f },
		{ 0.0212826371f, 0.1873502282f, 0.2926508425f, 0.2611472094f, 0.1493607563f, 0.0369416425f, 0.0005464093f, 0.0001832345f },
		{ 0.0215040981f, 0.1883883369f, 0.2932418081f, 0.2596626233f, 0.1437765094f, 0.0324691938f, 0.0002854484f, 0.0001504447f },
		{ 0.0217860823f, 0.1898888822f, 0.2934755726f, 0.2560431364f, 0.1369969017f, 0.0283285816f, 0.0003077782f, 0.0000611531f },
		{ 0.0220678026f, 0.1909351922f, 0.2934619446f, 0.2518269434f, 0.1297151884f, 0.0245621237f, 0.0004144291f, -0.0000449856f },
		{ 0.0225227432f, 0.1918149240f, 0.2924216563f, 0.2464683174f, 0.1219817586f, 0.0215511114f, 0.0004515449f, -0.0000941781f },
		{ 0.0224481795f, 0.1929666338f, 0.2906867975f, 0.2397057009f, 0.1148397528f, 0.0190011102f, 0.0006136337f, -0.0001901659f },
		{ 0.0229540878f, 0.1930392987f, 0.2883376009f, 0.2328741594f, 0.1074783301f, 0.0170169870f, 0.0006020716f, -0.0001970014f },
		{ 0.0230599455f, 0.1930456794f, 0.2848150957f, 0.2253110566f, 0.1010456247f, 0.0153356318f, 0.0006531858f, -0.0002299358f },
		{ 0.0231109357f, 0.1924765591f, 0.2810752246f, 0.2174048666f, 0.0949904585f, 0.0139874277f, 0.0006485678f, -0.0002370830f },
		{ 0.0233604015f, 0.1919719281f, 0.2762186444f, 0.2096467379f, 0.0894423099f, 0.0128188105f, 0.0006507623f, -0.0002432667f },
		{ 0.0234505893f, 0.1905408122f, 0.2712206156f, 0.2020727511f, 0.0842163247f, 0.0117569172f, 0.0006468263f, -0.0002465848f },
		{ 0.0234099300f, 0.1887445045f, 0.2658619637f, 0.1947505246f, 0.0793426129f, 0.0108911585f, 0.0006209150f, -0.0002378645f },
		{ 0.0233077640f, 0.1873812144f, 0.2599011980f, 0.1867178977f, 0.0753426265f, 0.0100579209f, 0.0006959637f, -0.0002765938f },
		{ 0.0232581367f, 0.1850902744f, 0.2542134093f, 0.1794497492f, 0.0711584501f, 0.0093927827f, 0.0006503074f, -0.0002575287f },
		{ 0.0232250033f, 0.1826912676f, 0.2479107836f, 0.1725234168f, 0.0675504830f, 0.0088079352f, 0.0006589592f, -0.0002656381f },
		{ 0.0230213242f, 0.1800747910f, 0.2417452866f, 0.1656394062f, 0.0641325697f, 0.0083314482f, 0.0006060254f, -0.0002449531f },
		{ 0.0226998145f, 0.1775100680f, 0.2351945260f, 0.1591769130f, 0.0612043448f, 0.0077463075f, 0.0006900554f, -0.0002836151f },
		{ 0.0224643628f, 0.1745639025f, 0.2289413795f, 0.1528047449f, 0.0581191952f, 0.0074051195f, 0.0006211601f, -0.0002559147f },
		{ 0.0223115672f, 0.1715540796f, 0.2227617568f, 0.1468321568f, 0.0554796946f, 0.0069717045f, 0.0006267282f, -0.0002594280f },
		{ 0.0220219313f, 0.1681517645f, 0.2166894646f, 0.1414810894f, 0.0529790049f, 0.0066509436f, 0.0005991498f, -0.0002489929f },
		{ 0.0218452616f, 0.1647210340f, 0.2106803543f, 0.1358405318f, 0.0505159689f, 0.0063464670f, 0.0005702101f, -0.0002363539f },
		{ 0.0217089186f, 0.1616972700f, 0.2042981132f, 0.1307525366f, 0.0484323586f, 0.0060394970f, 0.0005576983f, -0.0002317464f },
	},
	{
		{ 0.0213434985f, 0.1875245871f, 0.2913406815f, 0.2582267549f, 0.1459844154f, 0.0376677857f, 0.0018620504f, -0.0000477142f },
		{ 0.0214864436f, 0.1876246733f, 0.2917569736f, 0.2581897428f, 0.1445813221f, 0.0359076686f, 0.0010651653f, 0.0000554196f },
		{ 0.0214637304f, 0.1889003373f, 0.2923821830f, 0.2569242489f, 0.1414058983f, 0.0322203319f, 0.0005816691f, 0.0000697595f },
		{ 0.0218179728f, 0.1900304114f, 0.2932315022f, 0.2548788849f, 0.1361658208f, 0.0281979053f, 0.0004014998f, 0.0000279055f },
		{ 0.0220678026f, 0.1909351922f, 0.2934619446f, 0.2518269434f, 0.1297151884f, 0.0245621237f, 0.0004144291f, -0.0000449856f },
		{ 0.0223180516f, 0.1921469653f, 0.2928688187f, 0.2473357862f, 0.1226499969f, 0.0213879459f, 0.0005134248f, -0.0001280508f },
		{ 0.0227681919f, 0.1927828936f, 0.2918832393f, 0.2418112584f, 0.1156520971f, 0.0186539466f, 0.0006179358f, -0.0002036867f },
		{ 0.0229075864f, 0.1938392373f, 0.2906138850f, 0.2353464051f, 0.1084478748f, 0.0164712137f, 0.0006935487f, -0.0002515051f },
		{ 0.0232194260f, 0.1941571430f, 0.2876472520f, 0.2283974174f, 0.1017412461f, 0.0147140857f, 0.0007178486f, -0.0002730990f },
		{ 0.0232380242f, 0.1940251755f, 0.2841926167f, 0.2211893139f, 0.0955300739f, 0.0132498947f, 0.0007239787f, -0.0002822116f },
		{ 0.0234225273f, 0.1938182966f, 0.2804694168f, 0.2133537628f, 0.0897030559f, 0.0120361727f, 0.0007216261f, -0.0002878769f },
		{ 0.0236372604f, 0.1926706538f, 0.2755133687f, 0.2059221078f, 0.0844166121f, 0.0109940639f, 0.0007283807f, -0.0002942396f },
		{ 0.0236484281f, 0.1916108733f, 0.2708012013f, 0.1978464602f, 0.0795537113f, 0.0101139529f, 0.0006992815f, -0.0002856475f },
		{ 0.0236528941f, 0.1898808773f, 0.2651356099f, 0.1908984380f, 0.0752363770f, 0.0093650279f, 0.0007171373f, -0.0002965789f },
		{ 0.0234822393f, 0.1884008376f, 0.2596168213f, 0.1834073799f, 0.0710224027f, 0.0086942283f, 0.0007109215f, -0.0002962265f },
		{ 0.0234961235f, 0.1858063615f, 0.2535421987f, 0.1763782540f, 0.0673810079f, 0.0080992536f, 0.0006896256f, -0.0002880494f },
		{ 0.0233117580f, 0.1837162203f, 0.2473639042f, 0.1691530148f, 0.0641637597f, 0.0075376159f, 0.0007272644f, -0.0003089570f },
		{ 0.0231381483f, 0.1809451307f, 0.2412569268f, 0.1626572048f, 0.0608012272f, 0.0071631092f, 0.0006474944f, -0.0002745521f },
		{ 0.0229372648f, 0.1778258515f, 0.2348042831f, 0.1564254279f, 0.0579024572f, 0.0066779906f, 0.0006801610f, -0.0002894405f },
		{ 0.0227113523f, 0.1750026566f, 0.2283944556f, 0.1503201224f, 0.0551552840f, 0.0063501055f, 0.0006549485f, -0.0002806480f },
		{ 0.0226157829f, 0.1715004431f, 0.2223022144f, 0.1448165381f, 0.0525144082f, 0.0060088867f, 0.0006341404f, -0.0002717408f },
		{ 0.0222590378f, 0.1683416140f, 0.2159208856f, 0.1389220601f, 0.0502887702f, 0.0056968060f, 0.0006340247f, -0.0002716807f },
		{ 0.0221185389f, 0.1657380216f, 0.2097637738f, 0.1334813484f, 0.0481143717f, 0.0054705899f, 0.0006080585f, -0.0002618215f },
		{ 0.0218278417f, 0.1620384170f, 0.2037765893f, 0.1284148704f, 0.0459476602f, 0.0051887786f, 0.0005850513f, -0.0002530860f },
	},
	{
		{ 0.0214613452f, 0.1887580211f, 0.2907237471f, 0.2532363797f, 0.1376604725f, 0.0331510708f, 0.0017130607f, -0.0001166764f },
		{ 0.0217178436f, 0.1888909209f, 0.2912319643f, 0.2526540872f, 0.1363789246f, 0.0315627125f, 0.0010322479f, -0.0000238418f },
		{ 0.0219672817f, 0.1897919056f, 0.2917333125f, 0.2516078181f, 0.1330665657f, 0.0284042552f, 0.0005577270f, 0.0000184666f },
		{ 0.0222563629f, 0.1908564805f, 0.2921590660f, 0.2496078008f, 0.1282595485f, 0.0247569991f, 0.0004983810f, -0.0000589293f },
		{ 0.0225227432f, 0.1918149240f, 0.2924216563f, 0.2464683174f, 0.1219817586f, 0.0215511114f, 0.0004515449f, -0.0000941781f },
		{ 0.0227681919f, 0.1927828936f, 0.2918832393f, 0.2418112584f, 0.1156520971f, 0.0186539466f, 0.0006179358f, -0.0002036867f },
		{ 0.0229296073f, 0.1938923930f, 0.2907727477f, 0.2363921559f, 0.1087306649f, 0.0163386741f, 0.0006633958f, -0.0002409389f },
		{ 0.0231960823f, 0.1941989300f, 0.2889271252f, 0.2304295202f, 0.1020811440f, 0.0144183029f, 0.0007210018f, -0.0002805708f },
		{ 0.0234816899f, 0.1949815650f, 0.2865208605f, 0.2230797251f, 0.0958342115f, 0.0128390678f, 0.0007605111f, -0.0003068308f },
		{ 0.0235684123f, 0.1948853420f, 0.2827685656f, 0.2161521925f, 0.0899717020f, 0.0115294007f, 0.0007781923f, -0.0003190110f },
		{ 0.0238324627f, 0.1942323079f, 0.2792761881f, 0.2085856586f, 0.0843520230f, 0.0105964668f, 0.0007012230f, -0.0002893897f },
		{ 0.0238802008f, 0.1931734709f, 0.2747407226f, 0.2012962754f, 0.0795826155f, 0.0095705722f, 0.0007547696f, -0.0003173200f },
		{ 0.0236610913f, 0.1920940077f, 0.2697636558f, 0.1936684940f, 0.0751569225f, 0.0087635727f, 0.0007862503f, -0.0003346992f },
		{ 0.0238801165f, 0.1904748109f, 0.2640785898f, 0.1864950564f, 0.0707098178f, 0.0081297067f, 0.0007330519f, -0.0003119058f },
		{ 0.0238660810f, 0.1889917038f, 0.2581352318f, 0.1791262630f, 0.0669831828f, 0.0075351650f, 0.0007200284f, -0.0003089274f },
		{ 0.0236754767f, 0.1864063167f, 0.2523489534f, 0.1723181188f, 0.0635606338f, 0.0070771696f, 0.0006950493f, -0.0002990024f },
		{ 0.0232612867f, 0.1843676680f, 0.2465288008f, 0.1654645471f, 0.0602641598f, 0.0065734119f, 0.0007228277f, -0.0003132880f },
		{ 0.0234214025f, 0.1813394826f, 0.2401759717f, 0.1591120375f, 0.0573780234f, 0.0061677378f, 0.0006906863f, -0.0002994216f },
		{ 0.0231089022f, 0.1784504853f, 0.2337131168f, 0.1529458492f, 0.0547255949f, 0.0058261477f, 0.0006823898f, -0.0002970963f },
		{ 0.0230434785f, 0.1754943019f, 0.2276448032f, 0.1471137479f, 0.0519241777f, 0.0055733642f, 0.0006310471f, -0.0002749814f },
		{ 0.0227998787f, 0.1724547868f, 0.2215169160f, 0.1411785286f, 0.0495099321f, 0.0052421358f, 0.0006260697f, -0.0002732090f },
		{ 0.0223991026f, 0.1694957467f, 0.2151342561f, 0.1356994809f, 0.0475749923f, 0.0048934362f, 0.0006813910f, -0.0002999763f },
		{ 0.0222527375f, 0.1659100232f, 0.2089965550f, 0.1308805986f, 0.0454434559f, 0.0047006910f, 0.0006295571f, -0.0002774443f },
		{ 0.0218187124f, 0.1624146696f, 0.2032198040f, 0.1258778190f, 0.0436635316f, 0.0044395036f, 0.0006429975f, -0.0002846285f },
	},
	{
		{ 0.0218657539f, 0.1896457058f, 0.2894548585f, 0.2466624042f, 0.1293738942f, 0.0295383876f, 0.0015785874f, -0.0001496780f },
		{ 0.0221015609f, 0.1900440560f, 0.2893203780f, 0.2461307087f, 0.1283612794f, 0.0279503844f, 0.0011011108f, -0.0001225291f },
		{ 0.0222155274f, 0.1903545477f, 0.2899399293f, 0.2454656890f, 0.1251616468f, 0.0252411497f, 0.0006190027f, -0.0000543481f },
		{ 0.0223295094f, 0.1918952287f, 0.2905782641f, 0.2428586094f, 0.1204991461f, 0.0220170343f, 0.0005472088f, -0.0001088805f },
		{ 0.0224481795f, 0.1929666338f, 0.2906867975f, 0.2397057009f, 0.1148397528f, 0.0190011102f, 0.0006136337f, -0.0001901659f },
		{ 0.0229075864f, 0.1938392373f, 0.2906138850f, 0.2353464051f, 0.1084478748f, 0.0164712137f, 0.0006935487f, -0.0002515051f },
		{ 0.0231960823f, 0.1941989300f, 0.2889271252f, 0.2304295202f, 0.1020811440f, 0.0144183029f, 0.0007210018f, -0.0002805708f },
		{ 0.0233099006f, 0.1952471828f, 0.2871427329f, 0.2237867308f, 0.0959987881f, 0.0127476530f, 0.0007942655f, -0.0003233316f },
		{ 0.0235388665f, 0.1954377139f, 0.2845532721f, 0.2172908440f, 0.0900293734f, 0.0112876170f, 0.0008160248f, -0.0003400065f },
		{ 0.0238055580f, 0.1955712575f, 0.2813644436f, 0.2100533556f, 0.0844374899f, 0.0101719010f, 0.0008075229f, -0.0003405343f },
		{ 0.0240695202f, 0.1948720784f, 0.2770703316f, 0.2026067605f, 0.0794812944f, 0.0092378663f, 0.0007865339f, -0.0003341358f },
		{ 0.0239470802f, 0.1940715639f, 0.2726414980f, 0.1958855204f, 0.0746832809f, 0.0083678181f, 0.0007980131f, -0.0003421902f },
		{ 0.0241624724f, 0.1927301486f, 0.2679089718f, 0.1883900897f, 0.0704199151f, 0.0077735420f, 0.0007277356f, -0.0003132189f },
		{ 0.0240713699f, 0.1911094620f, 0.2625667100f, 0.1811565302f, 0.0666523214f, 0.0071103521f, 0.0007709076f, -0.0003341929f },
		{ 0.0239683610f, 0.1890080330f, 0.2565359534f, 0.1746568275f, 0.0628980465f, 0.0066215503f, 0.0007334659f, -0.0003176275f },
		{ 0.0238312538f, 0.1865608585f, 0.2511088373f, 0.1677482875f, 0.0596716942f, 0.0062334357f, 0.0006933119f, -0.0003025720f },
		{ 0.0235611069f, 0.1844080619f, 0.2448355919f, 0.1612422933f, 0.0565521073f, 0.0058310931f, 0.0006792303f, -0.0002973487f },
		{ 0.0235389911f, 0.1816204915f, 0.2387238532f, 0.1550150525f, 0.0539232514f, 0.0054251307f, 0.0006906122f, -0.0003035548f },
		{ 0.0231686598f, 0.1792687226f, 0.2325191626f, 0.1489351233f, 0.0513662578f, 0.0050815843f, 0.0007152026f, -0.0003163454f },
		{ 0.0232400287f, 0.1762951358f, 0.2263151465f, 0.1429996308f, 0.0490433442f, 0.0047837564f, 0.0007068815f, -0.0003124583f },
		{ 0.0226399552f, 0.1734683251f, 0.2199928683f, 0.1374750049f, 0.0469479694f, 0.0044357552f, 0.0007419406f, -0.0003298870f },
		{ 0.0225063364f, 0.1697452696f, 0.2142356708f, 0.1324613040f, 0.0447088662f, 0.0042836184f, 0.0006818755f, -0.0003033888f },
		{ 0.0222431269f, 0.1665049987f, 0.2079525360f, 0.1276528026f, 0.0428451314f, 0.0040760012f, 0.0006706507f, -0.0002984663f },
		{ 0.0220690977f, 0.1633128651f, 0.2019036037f, 0.1228941604f, 0.0412088164f, 0.0038801743f, 0.0006569347f, -0.0002934408f },
	},
	{
		{ 0.0219655766f, 0.1900691074f, 0.2872654675f, 0.2396550965f, 0.1213511941f, 0.0264422645f, 0.0014908243f, -0.0001824004f },
		{ 0.0221591434f, 0.1904053015f, 0.2874906156f, 0.2393089408f, 0.1203302574f, 0.0250907099f, 0.0010495935f, -0.0001433667f },
		{ 0.0225153633f, 0.1911456248f, 0.2876568267f, 0.2382277753f, 0.1174583306f, 0.0224661669f, 0.0006977789f, -0.0001220351f },
		{ 0.0225174653f, 0.1922234947f, 0.2881575069f, 0.2360862603f, 0.1130849297f, 0.0196344548f, 0.0006303864f, -0.0001674917f },
		{ 0.0229540878f, 0.1930392987f, 0.2883376009f, 0.2328741594f, 0.1074783301f, 0.0170169870f, 0.0006020716f, -0.0001970014f },
		{ 0.0232194260f, 0.1941571430f, 0.2876472520f, 0.2283974174f, 0.1017412461f, 0.0147140857f, 0.0007178486f, -0.0002730990f },
		{ 0.0234816899f, 0.1949815650f, 0.2865208605f, 0.2230797251f, 0.0958342115f, 0.0128390678f, 0.0007605111f, -0.0003068308f },
		{ 0.0235388665f, 0.1954377139f, 0.2845532721f, 0.2172908440f, 0.0900293734f, 0.0112876170f, 0.0008160248f, -0.0003400065f },
		{ 0.0237879557f, 0.1957725447f, 0.2817622415f, 0.2106603936f, 0.0844857395f, 0.0101380503f, 0.0007970036f, -0.0003370049f },
		{ 0.0240578175f, 0.1956502159f, 0.2785059786f, 0.2040110393f, 0.0792959055f, 0.0090461163f, 0.0008140968f, -0.0003477272f },
		{ 0.0241695870f, 0.1947347526f, 0.2750944836f, 0.1970907184f, 0.0743492660f, 0.0082240681f, 0.0007821886f, -0.0003356528f },
		{ 0.0240695742f, 0.1942569844f, 0.2704363844f, 0.1896338232f, 0.0700828868f, 0.0074663888f, 0.0007846285f, -0.0003398918f },
		{ 0.0243923264f, 0.1925456634f, 0.2654036931f, 0.1829761312f, 0.0661445860f, 0.0068876458f, 0.0007478609f, -0.0003250995f },
		{ 0.0240484737f, 0.1912095666f, 0.2602785976f, 0.1759012814f, 0.0625874502f, 0.0063181877f, 0.0007494689f, -0.0003287087f },
		{ 0.0240562128f, 0.1893540378f, 0.2545210130f, 0.1691937724f, 0.0591363080f, 0.0058650807f, 0.0007282024f, -0.0003183700f },
		{ 0.0243884962f, 0.1872990613f, 0.2484706195f, 0.1628159248f, 0.0560300313f, 0.0054695790f, 0.0007092530f, -0.0003123354f },
		{ 0.0238961984f, 0.1849920145f, 0.2427193151f, 0.1563461963f, 0.0532610199f, 0.0050910426f, 0.0007289974f, -0.0003229211f },
		{ 0.0238177102f, 0.1821060183f, 0.2366747368f, 0.1506584961f, 0.0506454625f, 0.0047918929f, 0.0006928907f, -0.0003066410f },
		{ 0.0235583947f, 0.1791479129f, 0.2306328365f, 0.1449366692f, 0.0483147221f, 0.0044944074f, 0.0006967198f, -0.0003092280f },
		{ 0.0231796678f, 0.1762753995f, 0.2244125560f, 0.1390442636f, 0.0461311080f, 0.0042184803f, 0.0006972673f, -0.0003119655f },
		{ 0.0229408051f, 0.1732301916f, 0.2185590031f, 0.1340628203f, 0.0439476520f, 0.0040231643f, 0.0006768684f, -0.0003023323f },
		{ 0.0229633116f, 0.1696326805f, 0.2120658238f, 0.1290688153f, 0.0422183837f, 0.0037412870f, 0.0006868138f, -0.0003071730f },
		{ 0.0225165069f, 0.1668671339f, 0.2057650036f, 0.1241602476f, 0.0405358170f, 0.0035903629f, 0.0006646950f, -0.0002986430f },
		{ 0.0220783903f, 0.1632950128f, 0.2002811253f, 0.1195882408f, 0.0388186992f, 0.0034087465f, 0.0006597548f, -0.0002960921f },
	},
	{
		{ 0.0224590788f, 0.1900465497f, 0.2839820856f, 0.2324278584f, 0.1138492777f, 0.0238998043f, 0.0013912993f, -0.0001912000f },
		{ 0.0223147548f, 0.1906219624f, 0.2845384557f, 0.2316896034f, 0.1128653672f, 0.0227899848f, 0.0009684234f, -0.0001489947f },
		{ 0.0227086610f, 0.1909557644f, 0.2846350898f, 0.2306137120f, 0.1101897340f, 0.0204127036f, 0.0006702054f, -0.0001315509f },
		{ 0.0228026656f, 0.1919705901f, 0.2849383145f, 0.2283875965f, 0.1062237958f, 0.0177548437f, 0.0006399255f, -0.0001847252f },
		{ 0.0230599455f, 0.1930456794f, 0.2848150957f, 0.2253110566f, 0.1010456247f, 0.0153356318f, 0.0006531858f, -0.0002299358f },
		{ 0.0232380242f, 0.1940251755f, 0.2841926167f, 0.2211893139f, 0.0955300739f, 0.0132498947f, 0.0007239787f, -0.0002822116f },
		{ 0.0235684123f, 0.1948853420f, 0.2827685656f, 0.2161521925f, 0.0899717020f, 0.0115294007f, 0.0007781923f, -0.0003190110f },
		{ 0.0238055580f, 0.1955712575f, 0.2813644436f, 0.2100533556f, 0.0844374899f, 0.0101719010f, 0.0008075229f, -0.0003405343f },
		{ 0.0240578175f, 0.1956502159f, 0.2785059786f, 0.2040110393f, 0.0792959055f, 0.0090461163f, 0.0008140968f, -0.0003477272f },
		{ 0.0241979029f, 0.1954092118f, 0.2751605301f, 0.1973834620f, 0.0744511900f, 0.0081387303f, 0.0007755601f, -0.0003344877f },
		{ 0.0241891112f, 0.1951106192f, 0.2711965902f, 0.1905198670f, 0.0699047879f, 0.0073678429f, 0.0007821906f, -0.0003392640f },
		{ 0.0244444592f, 0.1941202650f, 0.2672484206f, 0.1837937198f, 0.0657626540f, 0.0067347029f, 0.0007236081f, -0.0003144021f },
		{ 0.0244453837f, 0.1927841943f, 0.2621872268f, 0.1769121629f, 0.0620687579f, 0.0061679427f, 0.0007458890f, -0.0003257785f },
		{ 0.0245496794f, 0.1912059219f, 0.2571443511f, 0.1704946316f, 0.0585501969f, 0.0057242372f, 0.0007095185f, -0.0003112794f },
		{ 0.0241511099f, 0.1896422224f, 0.2517733324f, 0.1639433963f, 0.0555383868f, 0.0052673342f, 0.0007295048f, -0.0003229967f },
		{ 0.0241289870f, 0.1872131211f, 0.2460101423f, 0.1579920271f, 0.0527081714f, 0.0048761669f, 0.0007013931f, -0.0003103786f },
		{ 0.0241172519f, 0.1847616174f, 0.2399684611f, 0.1517556854f, 0.0500588220f, 0.0045499059f, 0.0007046523f, -0.0003122279f },
		{ 0.0239806133f, 0.1825005904f, 0.2340129301f, 0.1457519046f, 0.0476413255f, 0.0042503547f, 0.0007064901f, -0.0003150878f },
		{ 0.0237088653f, 0.1792571960f, 0.2279609890f, 0.1402806863f, 0.0454385270f, 0.0039968801f, 0.0006830230f, -0.0003052745f },
		{ 0.0235347115f, 0.1761908944f, 0.2220074706f, 0.1353048923f, 0.0432100499f, 0.0038019916f, 0.0006505333f, -0.0002908920f },
		{ 0.0231275690f, 0.1732663574f, 0.2161469652f, 0.1298620101f, 0.0414518453f, 0.0035540286f, 0.0006693940f, -0.0003006897f },
		{ 0.0229242631f, 0.1699724283f, 0.2101698734f, 0.1251930374f, 0.0396906975f, 0.0033819285f, 0.0006564770f, -0.0002955578f },
		{ 0.0227431273f, 0.1666322853f, 0.2039637310f, 0.1205833400f, 0.0380720915f, 0.0031841940f, 0.0006489536f, -0.0002921642f },
		{ 0.0224171782f, 0.1633117666f, 0.1985644710f, 0.1161008231f, 0.0365225889f, 0.0030524443f, 0.0006322365f, -0.0002860493f },
	},
	{
		{ 0.0226775268f, 0.1896335727f, 0.2806735515f, 0.2241300990f, 0.1068374784f, 0.0218744750f, 0.0012585976f, -0.0001768779f },
		{ 0.0225138187f, 0.1904152298f, 0.2811078346f, 0.2233686881f, 0.1059434684f, 0.0207179936f, 0.0009661965f, -0.0001721925f },
		{ 0.0228975423f, 0.1905109213f, 0.2811543564f, 0.2229847146f, 0.1033930868f, 0.0185739904f, 0.0006868205f, -0.0001570559f },
		{ 0.0231297068f, 0.1915022388f, 0.2809098308f, 0.2210420488f, 0.0995873885f, 0.0162459967f, 0.0005391213f, -0.0001513621f },
		{ 0.0231109357f, 0.1924765591f, 0.2810752246f, 0.2174048666f, 0.0949904585f, 0.0139874277f, 0.0006485678f, -0.0002370830f },
		{ 0.0234225273f, 0.1938182966f, 0.2804694168f, 0.2133537628f, 0.0897030559f, 0.0120361727f, 0.0007216261f, -0.0002878769f },
		{ 0.0238324627f, 0.1942323079f, 0.2792761881f, 0.2085856586f, 0.0843520230f, 0.0105964668f, 0.0007012230f, -0.0002893897f },
		{ 0.0240695202f, 0.1948720784f, 0.2770703316f, 0.2026067605f, 0.0794812944f, 0.0092378663f, 0.0007865339f, -0.0003341358f },
		{ 0.0241695870f, 0.1947347526f, 0.2750944836f, 0.1970907184f, 0.0743492660f, 0.0082240681f, 0.0007821886f, -0.0003356528f },
		{ 0.0241891112f, 0.1951106192f, 0.2711965902f, 0.1905198670f, 0.0699047879f, 0.0073678429f, 0.0007821906f, -0.0003392640f },
		{ 0.0244394752f, 0.1947024558f, 0.2676555783f, 0.1840249143f, 0.0658312788f, 0.0066690471f, 0.0007694785f, -0.0003359793f },
		{ 0.0244239621f, 0.1937988454f, 0.2634873717f, 0.1773386432f, 0.0619264667f, 0.0060523016f, 0.0007601186f, -0.0003330304f },
		{ 0.0244468816f, 0.1925730565f, 0.2586094113f, 0.1709687099f, 0.0585051203f, 0.0055296363f, 0.0007585404f, -0.0003356852f },
		{ 0.0244593389f, 0.1911181756f, 0.2535200377f, 0.1646528680f, 0.0550765531f, 0.0050561105f, 0.0007704111f, -0.0003404261f },
		{ 0.0243839830f, 0.1893250242f, 0.2484398672f, 0.1582942923f, 0.0522299077f, 0.0047478114f, 0.0007129666f, -0.0003160748f },
		{ 0.0241699050f, 0.1864827469f, 0.2429643988f, 0.1525267828f, 0.0495906477f, 0.0044491278f, 0.0006863635f, -0.0003044247f },
		{ 0.0241240959f, 0.1845916724f, 0.2370590521f, 0.1466835060f, 0.0470316133f, 0.0040602147f, 0.0007226435f, -0.0003216538f },
		{ 0.0240869400f, 0.1817336746f, 0.2310707093f, 0.1411351026f, 0.0447934452f, 0.0038353787f, 0.0006795556f, -0.0003040997f },
		{ 0.0238040481f, 0.1790591987f, 0.2251173296f, 0.1358210938f, 0.0426644502f, 0.0036069268f, 0.0006758298f, -0.0003027581f },
		{ 0.0235693244f, 0.1764245596f, 0.2195794422f, 0.1305036151f, 0.0408370203f, 0.0033630494f, 0.0006849161f, -0.0003070954f },
		{ 0.0233588854f, 0.1729504932f, 0.2132175810f, 0.1258080790f, 0.0389759917f, 0.0031971438f, 0.0006574611f, -0.0002961852f },
		{ 0.0229956434f, 0.1697538109f, 0.2079113523f, 0.1210160820f, 0.0373737089f, 0.0030395796f, 0.0006369952f, -0.0002879424f },
		{ 0.0227818441f, 0.1666847218f, 0.2020974601f, 0.1167086597f, 0.0357328394f, 0.0029143658f, 0.0006083867f, -0.0002745639f },
		{ 0.0226483567f, 0.1632603798f, 0.1965447029f, 0.1124143733f, 0.0343914745f, 0.0027344475f, 0.0006219465f, -0.0002809715f },
	},
	{
		{ 0.0226668030f, 0.1894298219f, 0.2759415361f, 0.2162771154f, 0.1007062772f, 0.0199333550f, 0.0012771241f, -0.0002214295f },
		{ 0.0227164253f, 0.1891397339f, 0.2763779477f, 0.2157954985f, 0.0997445690f, 0.0189416945f, 0.0009485871f, -0.0001920176f },
		{ 0.0228275843f, 0.1904753799f, 0.2761025576f, 0.2146131425f, 0.0975008212f, 0.0169737087f, 0.0007347826f, -0.0001933354f },
		{ 0.0232527004f, 0.1906557279f, 0.2769063395f, 0.2126626918f, 0.0938191706f, 0.0148402477f, 0.0006113629f, -0.0001920989f },
		{ 0.0233604015f, 0.1919719281f, 0.2762186444f, 0.2096467379f, 0.0894423099f, 0.0128188105f, 0.0006507623f, -0.0002432667f },
		{ 0.0236372604f, 0.1926706538f, 0.2755133687f, 0.2059221078f, 0.0844166121f, 0.0109940639f, 0.0007283807f, -0.0002942396f },
		{ 0.0238802008f, 0.1931734709f, 0.2747407226f, 0.2012962754f, 0.0795826155f, 0.0095705722f, 0.0007547696f, -0.0003173200f },
		{ 0.0239470802f, 0.1940715639f, 0.2726414980f, 0.1958855204f, 0.0746832809f, 0.0083678181f, 0.0007980131f, -0.0003421902f },
		{ 0.0240695742f, 0.1942569844f, 0.2704363844f, 0.1896338232f, 0.0700828868f, 0.0074663888f, 0.0007846285f, -0.0003398918f },
		{ 0.0244444592f, 0.1941202650f, 0.2672484206f, 0.1837937198f, 0.0657626540f, 0.0067347029f, 0.0007236081f, -0.0003144021f },
		{ 0.0244239621f, 0.1937988454f, 0.2634873717f, 0.1773386432f, 0.0619264667f, 0.0060523016f, 0.0007601186f, -0.0003330304f },
		{ 0.0245452029f, 0.1930150496f, 0.2592522074f, 0.1712141387f, 0.0583308764f, 0.0055177498f, 0.0007531099f, -0.0003313740f },
		{ 0.0247958360f, 0.1917765581f, 0.2543661476f, 0.1649941016f, 0.0548764718f, 0.0050518670f, 0.0007184981f, -0.0003167243f },
		{ 0.0246232381f, 0.1903509054f, 0.2496398861f, 0.1588186631f, 0.0519025915f, 0.0046809003f, 0.0006843302f, -0.0003031890f },
		{ 0.0247487046f, 0.1885893686f, 0.2446114730f, 0.1527931015f, 0.0490479470f, 0.0043102238f, 0.0006814066f, -0.0003021428f },
		{ 0.0245029105f, 0.1862604410f, 0.2390860203f, 0.1472663332f, 0.0466039935f, 0.0039730290f, 0.0006932185f, -0.0003097377f },
		{ 0.0244473662f, 0.1837883829f, 0.2337003382f, 0.1413865703f, 0.0443464665f, 0.0037609862f, 0.0006553659f, -0.0002937710f },
		{ 0.0243173512f, 0.1813559725f, 0.2276907761f, 0.1364508856f, 0.0423073485f, 0.0034850834f, 0.0006689739f, -0.0003006299f },
		{ 0.0239502516f, 0.1787389002f, 0.2219409208f, 0.1312021533f, 0.0401386217f, 0.0033049865f, 0.0006362001f, -0.0002862051f },
		{ 0.0236892795f, 0.1756736941f, 0.2164333650f, 0.1261358023f, 0.0386242728f, 0.0030068116f, 0.0006922382f, -0.0003126508f },
		{ 0.0235736277f, 0.1724223955f, 0.2107836964f, 0.1214895132f, 0.0367748199f, 0.0028940510f, 0.0006391713f, -0.0002890267f },
		{ 0.0232203215f, 0.1693648137f, 0.2049845595f, 0.1171670998f, 0.0351306134f, 0.0027513078f, 0.0006196548f, -0.0002803443f },
		{ 0.0231284299f, 0.1661609356f, 0.1990909981f, 0.1128275074f, 0.0337941596f, 0.0026100729f, 0.0006181819f, -0.0002806498f },
		{ 0.0226445243f, 0.1627943636f, 0.1936258507f, 0.1088919000f, 0.0325043369f, 0.0024524911f, 0.0006251310f, -0.0002830144f },
	},
	{
		{ 0.0228695840f, 0.1881846496f, 0.2710803567f, 0.2083200560f, 0.0946404793f, 0.0183335677f, 0.0012464246f, -0.0002378574f },
		{ 0.0228647819f, 0.1885420129f, 0.2712602864f, 0.2078800013f, 0.0939120572f, 0.0174188147f, 0.0009450469f, -0.0002121757f },
		{ 0.0229689448f, 0.1890647622f, 0.2718127333f, 0.2066070669f, 0.0914586145f, 0.0156746679f, 0.0006673719f, -0.0001747368f },
		{ 0.0231574213f, 0.1901703525f, 0.2715345281f, 0.2046426179f, 0.0883227216f, 0.0136261904f, 0.0006428011f, -0.0002159715f },
		{ 0.0234505893f, 0.1905408122f, 0.2712206156f, 0.2020727511f, 0.0842163247f, 0.0117569172f, 0.0006468263f, -0.0002465848f },
		{ 0.0236484281f, 0.1916108733f, 0.2708012013f, 0.1978464602f, 0.0795537113f, 0.0101139529f, 0.0006992815f, -0.0002856475f },
		{ 0.0236610913f, 0.1920940077f, 0.2697636558f, 0.1936684940f, 0.0751569225f, 0.0087635727f, 0.0007862503f, -0.0003346992f },
		{ 0.0241624724f, 0.1927301486f, 0.2679089718f, 0.1883900897f, 0.0704199151f, 0.0077735420f, 0.0007277356f, -0.0003132189f },
		{ 0.0243923264f, 0.1925456634f, 0.2654036931f, 0.1829761312f, 0.0661445860f, 0.0068876458f, 0.0007478609f, -0.0003250995f },
		{ 0.0244453837f, 0.1927841943f, 0.2621872268f, 0.1769121629f, 0.0620687579f, 0.0061679427f, 0.0007458890f, -0.0003257785f },
		{ 0.0244468816f, 0.1925730565f, 0.2586094113f, 0.1709687099f, 0.0585051203f, 0.0055296363f, 0.0007585404f, -0.0003356852f },
		{ 0.0247958360f, 0.1917765581f, 0.2543661476f, 0.1649941016f, 0.0548764718f, 0.0050518670f, 0.0007184981f, -0.0003167243f },
		{ 0.0246937485f, 0.1907353786f, 0.2502558643f, 0.1588594256f, 0.0518772166f, 0.0046186182f, 0.0007223548f, -0.0003205268f },
		{ 0.0246561025f, 0.1894006770f, 0.2453581680f, 0.1530985536f, 0.0490637072f, 0.0041969957f, 0.0007297574f, -0.0003245076f },
		{ 0.0245993800f, 0.1871060075f, 0.2400275034f, 0.1474870287f, 0.0464891032f, 0.0039498665f, 0.0006822019f, -0.0003045426f },
		{ 0.0243506154f, 0.1851303285f, 0.2352302259f, 0.1419071071f, 0.0439297841f, 0.0036255992f, 0.0006796724f, -0.0003032529f },
		{ 0.0246323193f, 0.1828576764f, 0.2292632702f, 0.1365140147f, 0.0417710458f, 0.0034168439f, 0.0006399300f, -0.0002874579f },
		{ 0.0241530935f, 0.1809107004f, 0.2240962350f, 0.1311209803f, 0.0398845842f, 0.0031436183f, 0.0006948161f, -0.0003135461f },
		{ 0.0239018975f, 0.1777922036f, 0.2186942848f, 0.1265046740f, 0.0380371635f, 0.0029778226f, 0.0006530945f, -0.0002959234f },
		{ 0.0238305766f, 0.1750306085f, 0.2127543828f, 0.1218411879f, 0.0365011126f, 0.0027558718f, 0.0006797526f, -0.0003072841f },
		{ 0.0235905375f, 0.1721856399f, 0.2076395384f, 0.1172808154f, 0.0347017773f, 0.0026053142f, 0.0006640073f, -0.0003009852f },
		{ 0.0231700354f, 0.1689510522f, 0.2019523487f, 0.1132678097f, 0.0331672146f, 0.0025053634f, 0.0006131919f, -0.0002778523f },
		{ 0.0230104423f, 0.1657369134f, 0.1961706736f, 0.1090729217f, 0.0319556315f, 0.0023438124f, 0.0006240922f, -0.0002838085f },
		{ 0.0227982422f, 0.1627794204f, 0.1908110195f, 0.1050456343f, 0.0306519596f, 0.0022216611f, 0.0006098979f, -0.0002772164f },
	},
	{
		{ 0.0226910344f, 0.1870926493f, 0.2660236879f, 0.2002232068f, 0.0891636206f, 0.0170009431f, 0.0011785374f, -0.0002318295f },
		{ 0.0229683719f, 0.1872395426f, 0.2659237184f, 0.2000594754f, 0.0884445050f, 0.0161364772f, 0.0009326009f, -0.0002202483f },
		{ 0.0230550887f, 0.1876446188f, 0.2659223577f, 0.1988891689f, 0.0866408508f, 0.0143854423f, 0.0007458079f, -0.0002226890f },
		{ 0.0232246965f, 0.1882015715f, 0.2665130264f, 0.1967403792f, 0.0833540361f, 0.0125831317f, 0.0006506600f, -0.0002270307f },
		{ 0.0234099300f, 0.1887445045f, 0.2658619637f, 0.1947505246f, 0.0793426129f, 0.0108911585f, 0.0006209150f, -0.0002378645f },
		{ 0.0236528941f, 0.1898808773f, 0.2651356099f, 0.1908984380f, 0.0752363770f, 0.0093650279f, 0.0007171373f, -0.0002965789f },
		{ 0.0238801165f, 0.1904748109f, 0.2640785898f, 0.1864950564f, 0.0707098178f, 0.0081297067f, 0.0007330519f, -0.0003119058f },
		{ 0.0240713699f, 0.1911094620f, 0.2625667100f, 0.1811565302f, 0.0666523214f, 0.0071103521f, 0.0007709076f, -0.0003341929f },
		{ 0.0240484737f, 0.1912095666f, 0.2602785976f, 0.1759012814f, 0.0625874502f, 0.0063181877f, 0.0007494689f, -0.0003287087f },
		{ 0.0245496794f, 0.1912059219f, 0.2571443511f, 0.1704946316f, 0.0585501969f, 0.0057242372f, 0.0007095185f, -0.0003112794f },
		{ 0.0244593389f, 0.1911181756f, 0.2535200377f, 0.1646528680f, 0.0550765531f, 0.0050561105f, 0.0007704111f, -0.0003404261f },
		{ 0.0246232381f, 0.1903509054f, 0.2496398861f, 0.1588186631f, 0.0519025915f, 0.0046809003f, 0.0006843302f, -0.0003031890f },
		{ 0.0246561025f, 0.1894006770f, 0.2453581680f, 0.1530985536f, 0.0490637072f, 0.0041969957f, 0.0007297574f, -0.0003245076f },
		{ 0.0246564364f, 0.1878663453f, 0.2407598715f, 0.1475016533f, 0.0464375018f, 0.0039043708f, 0.0007043257f, -0.0003146362f },
		{ 0.0244722604f, 0.1862137310f, 0.2358232145f, 0.1417681848f, 0.0438836799f, 0.0036306491f, 0.0006710274f, -0.0003010405f },
		{ 0.0246207543f, 0.1841573877f, 0.2306805296f, 0.1366373387f, 0.0415413520f, 0.0033748229f, 0.0006471888f, -0.0002893656f },
		{ 0.0245406979f, 0.1818429544f, 0.2253115505f, 0.1315274106f, 0.0395885752f, 0.0031134028f, 0.0006531743f, -0.0002944246f },
		{ 0.0243167722f, 0.1792789860f, 0.2199034534f, 0.1267646070f, 0.0376937393f, 0.0028604346f, 0.0006676430f, -0.0003016453f },
		{ 0.0240815045f, 0.1771740057f, 0.2145345593f, 0.1218433236f, 0.0360193931f, 0.0026993446f, 0.0006657749f, -0.0003021971f },
		{ 0.0237683431f, 0.1740890068f, 0.2090234982f, 0.1175787369f, 0.0343883665f, 0.0025205109f, 0.0006609890f, -0.0003005199f },
		{ 0.0234364175f, 0.1711554558f, 0.2037605847f, 0.1131266335f, 0.0329956381f, 0.0023329986f, 0.0006689606f, -0.0003043834f },
		{ 0.0233866287f, 0.1683274895f, 0.1983979203f, 0.1090853533f, 0.0315840757f, 0.0022389322f, 0.0006378391f, -0.0002900821f },
		{ 0.0231638757f, 0.1650996388f, 0.1933375399f, 0.1051566649f, 0.0301645625f, 0.0021694749f, 0.0005988128f, -0.0002732133f },
		{ 0.0227379051f, 0.1619110890f, 0.1880102084f, 0.1016873228f, 0.0289838400f, 0.0020201986f, 0.0006067849f, -0.0002772294f },
	},
	{
		{ 0.0229104517f, 0.1852894852f, 0.2602196230f, 0.1924819989f, 0.0842523589f, 0.0157966897f, 0.0011156135f, -0.0002253741f },
		{ 0.0227376459f, 0.1853364840f, 0.2603052602f, 0.1923190991f, 0.0835188228f, 0.0149832555f, 0.0008849143f, -0.0002155412f },
		{ 0.0229897753f, 0.1856281122f, 0.2604846035f, 0.1913330180f, 0.0818245370f, 0.0134763928f, 0.0006620404f, -0.0001922439f },
		{ 0.0233153883f, 0.1862083944f, 0.2603278163f, 0.1894650141f, 0.0788608768f, 0.0117727616f, 0.0006069186f, -0.0002112360f },
		{ 0.0233077640f, 0.1873812144f, 0.2599011980f, 0.1867178977f, 0.0753426265f, 0.0100579209f, 0.0006959637f, -0.0002765938f },
		{ 0.0234822393f, 0.1884008376f, 0.2596168213f, 0.1834073799f, 0.0710224027f, 0.0086942283f, 0.0007109215f, -0.0002962265f },
		{ 0.0238660810f, 0.1889917038f, 0.2581352318f, 0.1791262630f, 0.0669831828f, 0.0075351650f, 0.0007200284f, -0.0003089274f },
		{ 0.0239683610f, 0.1890080330f, 0.2565359534f, 0.1746568275f, 0.0628980465f, 0.0066215503f, 0.0007334659f, -0.0003176275f },
		{ 0.0240562128f, 0.1893540378f, 0.2545210130f, 0.1691937724f, 0.0591363080f, 0.0058650807f, 0.0007282024f, -0.0003183700f },
		{ 0.0241511099f, 0.1896422224f, 0.2517733324f, 0.1639433963f, 0.0555383868f, 0.0052673342f, 0.0007295048f, -0.0003229967f },
		{ 0.0243839830f, 0.1893250242f, 0.2484398672f, 0.1582942923f, 0.0522299077f, 0.0047478114f, 0.0007129666f, -0.0003160748f },
		{ 0.0247487046f, 0.1885893686f, 0.2446114730f, 0.1527931015f, 0.0490479470f, 0.0043102238f, 0.0006814066f, -0.0003021428f },
		{ 0.0245993800f, 0.1871060075f, 0.2400275034f, 0.1474870287f, 0.0464891032f, 0.0039498665f, 0.0006822019f, -0.0003045426f },
		{ 0.0244722604f, 0.1862137310f, 0.2358232145f, 0.1417681848f, 0.0438836799f, 0.0036306491f, 0.0006710274f, -0.0003010405f },
		{ 0.0245155744f, 0.1844924382f, 0.2313458893f, 0.1367220937f, 0.0415506611f, 0.0033312991f, 0.0006729691f, -0.0003027069f },
		{ 0.0244833861f, 0.1825972151f, 0.2262802814f, 0.1316394840f, 0.0394064648f, 0.0030678895f, 0.0006673723f, -0.0003011722f },
		{ 0.0245023706f, 0.1806231574f, 0.2209445688f, 0.1266559231f, 0.0374592382f, 0.0028618367f, 0.0006391741f, -0.0002885866f },
		{ 0.0240872464f, 0.1786955131f, 0.2161240913f, 0.1218934050f, 0.0357990540f, 0.0025980154f, 0.0006992525f, -0.0003169529f },
		{ 0.0241417988f, 0.1759131362f, 0.2104874247f, 0.1175230671f, 0.0340714767f, 0.0024709301f, 0.0006549798f, -0.0002980660f },
		{ 0.0238011916f, 0.1729135700f, 0.2052481195f, 0.1130650455f, 0.0325938504f, 0.0023376888f, 0.0006453949f, -0.0002944397f },
		{ 0.0236850224f, 0.1703651074f, 0.2002358633f, 0.1089270945f, 0.0311577014f, 0.0022046468f, 0.0006305265f, -0.0002876473f },
		{ 0.0234504303f, 0.1668342373f, 0.1947224798f, 0.1052630713f, 0.0298678314f, 0.0020756578f, 0.0006117636f, -0.0002789297f },
		{ 0.0231829677f, 0.1635785413f, 0.1896794047f, 0.1017615301f, 0.0285938277f, 0.0019821390f, 0.0005881468f, -0.0002683351f },
		{ 0.0228338905f, 0.1610279234f, 0.1845939573f, 0.0978488934f, 0.0275746075f, 0.0018125792f, 0.0006248514f, -0.0002865137f },
	},
	{
		{ 0.0227877497f, 0.1830462651f, 0.2542185746f, 0.1851406079f, 0.0799179156f, 0.0146807207f, 0.0011065787f, -0.0002448787f },
		{ 0.0228694894f, 0.1830519999f, 0.2545091300f, 0.1848080479f, 0.0791268139f, 0.0140146910f, 0.0008539075f, -0.0002131896f },
		{ 0.0230770190f, 0.1833988034f, 0.2543567332f, 0.1836375966f, 0.0774356927f, 0.0126261503f, 0.0006475436f, -0.0001940306f },
		{ 0.0231875261f, 0.1846099781f, 0.2541026656f, 0.1818550231f, 0.0746572618f, 0.0109185835f, 0.0006432425f, -0.0002329359f },
		{ 0.0232581367f, 0.1850902744f, 0.2542134093f, 0.1794497492f, 0.0711584501f, 0.0093927827f, 0.0006503074f, -0.0002575287f },
		{ 0.0234961235f, 0.1858063615f, 0.2535421987f, 0.1763782540f, 0.0673810079f, 0.0080992536f, 0.0006896256f, -0.0002880494f },
		{ 0.0236754767f, 0.1864063167f, 0.2523489534f, 0.1723181188f, 0.0635606338f, 0.0070771696f, 0.0006950493f, -0.0002990024f },
		{ 0.0238312538f, 0.1865608585f, 0.2511088373f, 0.1677482875f, 0.0596716942f, 0.0062334357f, 0.0006933119f, -0.0003025720f },
		{ 0.0243884962f, 0.1872990613f, 0.2484706195f, 0.1628159248f, 0.0560300313f, 0.0054695790f, 0.0007092530f, -0.0003123354f },
		{ 0.0241289870f, 0.1872131211f, 0.2460101423f, 0.1579920271f, 0.0527081714f, 0.0048761669f, 0.0007013931f, -0.0003103786f },
		{ 0.0241699050f, 0.1864827469f, 0.2429643988f, 0.1525267828f, 0.0495906477f, 0.0044491278f, 0.0006863635f, -0.0003044247f },
		{ 0.0245029105f, 0.1862604410f, 0.2390860203f, 0.1472663332f, 0.0466039935f, 0.0039730290f, 0.0006932185f, -0.0003097377f },
		{ 0.0243506154f, 0.1851303285f, 0.2352302259f, 0.1419071071f, 0.0439297841f, 0.0036255992f, 0.0006796724f, -0.0003032529f },
		{ 0.0246207543f, 0.1841573877f, 0.2306805296f, 0.1366373387f, 0.0415413520f, 0.0033748229f, 0.0006471888f, -0.0002893656f },
		{ 0.0244833861f, 0.1825972151f, 0.2262802814f, 0.1316394840f, 0.0394064648f, 0.0030678895f, 0.0006673723f, -0.0003011722f },
		{ 0.0244771991f, 0.1808993079f, 0.2214326856f, 0.1268493179f, 0.0374203583f, 0.0028529215f, 0.0006501805f, -0.0002936406f },
		{ 0.0243401134f, 0.1788088950f, 0.2165216874f, 0.1218940732f, 0.0357411019f, 0.0025960942f, 0.0006807797f, -0.0003082182f },
		{ 0.0244366728f, 0.1763745226f, 0.2111201181f, 0.1175402321f, 0.0338915774f, 0.0024899553f, 0.0006327480f, -0.0002864136f },
		{ 0.0241403442f, 0.1739884621f, 0.2064401603f, 0.1132071187f, 0.0323489028f, 0.0022749282f, 0.0006554750f, -0.0002981747f },
		{ 0.0238008129f, 0.1712875304f, 0.2014275966f, 0.1090045953f, 0.0309285677f, 0.0021515861f, 0.0006240582f, -0.0002848755f },
		{ 0.0234560953f, 0.1686551798f, 0.1959019812f, 0.1050738991f, 0.0296014124f, 0.0020258674f, 0.0006216887f, -0.0002837195f },
		{ 0.0233483462f, 0.1656774159f, 0.1911475229f, 0.1013252715f, 0.0284202882f, 0.0019021638f, 0.0006244093f, -0.0002856022f },
		{ 0.0233241247f, 0.1625950771f, 0.1861471506f, 0.0979354457f, 0.0272144707f, 0.0018374274f, 0.0005909534f, -0.0002708709f },
		{ 0.0227744425f, 0.1596110752f, 0.1813831406f, 0.0944247338f, 0.0261668951f, 0.0017139703f, 0.0006070837f, -0.0002787635f },
	},
	{
		{ 0.0227018945f, 0.1807941942f, 0.2484785316f, 0.1776089704f, 0.0755710404f, 0.0138192422f, 0.0010492149f, -0.0002330124f },
		{ 0.0227393399f, 0.1813058632f, 0.2485850091f, 0.1769993844f, 0.0748810973f, 0.0130736566f, 0.0008759344f, -0.0002352057f },
		{ 0.0228026589f, 0.1815797060f, 0.2485923335f, 0.1762012380f, 0.0733308801f, 0.0117938509f, 0.0006316946f, -0.0001933494f },
		{ 0.0228422269f, 0.1821933579f, 0.2482942046f, 0.1745716889f, 0.0707559489f, 0.0102859418f, 0.0006151229f, -0.0002244558f },
		{ 0.0232250033f, 0.1826912676f, 0.2479107836f, 0.1725234168f, 0.0675504830f, 0.0088079352f, 0.0006589592f, -0.0002656381f },
		{ 0.0233117580f, 0.1837162203f, 0.2473639042f, 0.1691530148f, 0.0641637597f, 0.0075376159f, 0.0007272644f, -0.0003089570f },
		{ 0.0232612867f, 0.1843676680f, 0.2465288008f, 0.1654645471f, 0.0602641598f, 0.0065734119f, 0.0007228277f, -0.0003132880f },
		{ 0.0235611069f, 0.1844080619f, 0.2448355919f, 0.1612422933f, 0.0565521073f, 0.0058310931f, 0.0006792303f, -0.0002973487f },
		{ 0.0238961984f, 0.1849920145f, 0.2427193151f, 0.1563461963f, 0.0532610199f, 0.0050910426f, 0.0007289974f, -0.0003229211f },
		{ 0.0241172519f, 0.1847616174f, 0.2399684611f, 0.1517556854f, 0.0500588220f, 0.0045499059f, 0.0007046523f, -0.0003122279f },
		{ 0.0241240959f, 0.1845916724f, 0.2370590521f, 0.1466835060f, 0.0470316133f, 0.0040602147f, 0.0007226435f, -0.0003216538f },
		{ 0.0244473662f, 0.1837883829f, 0.2337003382f, 0.1413865703f, 0.0443464665f, 0.0037609862f, 0.0006553659f, -0.0002937710f },
		{ 0.0246323193f, 0.1828576764f, 0.2292632702f, 0.1365140147f, 0.0417710458f, 0.0034168439f, 0.0006399300f, -0.0002874579f },
		{ 0.0245406979f, 0.1818429544f, 0.2253115505f, 0.1315274106f, 0.0395885752f, 0.0031134028f, 0.0006531743f, -0.0002944246f },
		{ 0.0245023706f, 0.1806231574f, 0.2209445688f, 0.1266559231f, 0.0374592382f, 0.0028618367f, 0.0006391741f, -0.0002885866f },
		{ 0.0243401134f, 0.1788088950f, 0.2165216874f, 0.1218940732f, 0.0357411019f, 0.0025960942f, 0.0006807797f, -0.0003082182f },
		{ 0.0242173287f, 0.1768228855f, 0.2116681480f, 0.1176507617f, 0.0339042271f, 0.0024581142f, 0.0006489051f, -0.0002947600f },
		{ 0.0240340372f, 0.1746273065f, 0.2070621400f, 0.1132791732f, 0.0321531897f, 0.0022891234f, 0.0006304055f, -0.0002870443f },
		{ 0.0239516943f, 0.1722144223f, 0.2021223204f, 0.1091733036f, 0.0308118689f, 0.0021762550f, 0.0006045801f, -0.0002758135f },
		{ 0.0238281411f, 0.1695489410f, 0.1970694028f, 0.1051642364f, 0.0294902640f, 0.0019857246f, 0.0006277032f, -0.0002874019f },
		{ 0.0233584803f, 0.1670018986f, 0.1926345127f, 0.1011995638f, 0.0282013837f, 0.0018589208f, 0.0006315038f, -0.0002902125f },
		{ 0.0231705475f, 0.1645767255f, 0.1872218640f, 0.0978785959f, 0.0270236058f, 0.0017366839f, 0.0006297990f, -0.0002885441f },
		{ 0.0230591321f, 0.1613725920f, 0.1823292631f, 0.0943928923f, 0.0260139910f, 0.0016174386f, 0.0006270816f, -0.0002881265f },
		{ 0.0227572689f, 0.1583283107f, 0.1779591480f, 0.0909798084f, 0.0249285999f, 0.0015733072f, 0.0006045839f, -0.0002786016f },
	},
	{
		{ 0.0225446905f, 0.1780958000f, 0.2420424640f, 0.1706433326f, 0.0717435212f, 0.0129152932f, 0.0010236067f, -0.0002397612f },
		{ 0.0225631776f, 0.1781870117f, 0.2422006006f, 0.1703823465f, 0.0709456914f, 0.0122617004f, 0.0008406076f, -0.0002290572f },
		{ 0.0227099017f, 0.1786441124f, 0.2422401862f, 0.1694067035f, 0.0696206504f, 0.0110975802f, 0.0006622916f, -0.0002127060f },
		{ 0.0229418099f, 0.1793723332f, 0.2420940947f, 0.1679189780f, 0.0671257094f, 0.0096838291f, 0.0005953662f, -0.0002186238f },
		{ 0.0230213242f, 0.1800747910f, 0.2417452866f, 0.1656394062f, 0.0641325697f, 0.0083314482f, 0.0006060254f, -0.0002449531f },
		{ 0.0231381483f, 0.1809451307f, 0.2412569268f, 0.1626572048f, 0.0608012272f, 0.0071631092f, 0.0006474944f, -0.0002745521f },
		{ 0.0234214025f, 0.1813394826f, 0.2401759717f, 0.1591120375f, 0.0573780234f, 0.0061677378f, 0.0006906863f, -0.0002994216f },
		{ 0.0235389911f, 0.1816204915f, 0.2387238532f, 0.1550150525f, 0.0539232514f, 0.0054251307f, 0.0006906122f, -0.0003035548f },
		{ 0.0238177102f, 0.1821060183f, 0.2366747368f, 0.1506584961f, 0.0506454625f, 0.0047918929f, 0.0006928907f, -0.0003066410f },
		{ 0.0239806133f, 0.1825005904f, 0.2340129301f, 0.1457519046f, 0.0476413255f, 0.0042503547f, 0.0007064901f, -0.0003150878f },
		{ 0.0240869400f, 0.1817336746f, 0.2310707093f, 0.1411351026f, 0.0447934452f, 0.0038353787f, 0.0006795556f, -0.0003040997f },
		{ 0.0243173512f, 0.1813559725f, 0.2276907761f, 0.1364508856f, 0.0423073485f, 0.0034850834f, 0.0006689739f, -0.0003006299f },
		{ 0.0241530935f, 0.1809107004f, 0.2240962350f, 0.1311209803f, 0.0398845842f, 0.0031436183f, 0.0006948161f, -0.0003135461f },
		{ 0.0243167722f, 0.1792789860f, 0.2199034534f, 0.1267646070f, 0.0376937393f, 0.0028604346f, 0.0006676430f, -0.0003016453f },
		{ 0.0240872464f, 0.1786955131f, 0.2161240913f, 0.1218934050f, 0.0357990540f, 0.0025980154f, 0.0006992525f, -0.0003169529f },
		{ 0.0244366728f, 0.1763745226f, 0.2111201181f, 0.1175402321f, 0.0338915774f, 0.0024899553f, 0.0006327480f, -0.0002864136f },
		{ 0.0240340372f, 0.1746273065f, 0.2070621400f, 0.1132791732f, 0.0321531897f, 0.0022891234f, 0.0006304055f, -0.0002870443f },
		{ 0.0239998887f, 0.1727039564f, 0.2023792602f, 0.1090890464f, 0.0307830184f, 0.0021177324f, 0.0006364131f, -0.0002906468f },
		{ 0.0237761325f, 0.1701332794f, 0.1974917398f, 0.1051367758f, 0.0294273296f, 0.0019628050f, 0.0006364846f, -0.0002912235f },
		{ 0.0236631864f, 0.1677072736f, 0.1931804581f, 0.1014482713f, 0.0280630615f, 0.0018619077f, 0.0006219539f, -0.0002848658f },
		{ 0.0234396913f, 0.1652365028f, 0.1878623622f, 0.0976864331f, 0.0268607251f, 0.0017401467f, 0.0006220117f, -0.0002851214f },
		{ 0.0231037047f, 0.1625386741f, 0.1833279590f, 0.0941949011f, 0.0258018628f, 0.0016209942f, 0.0006199181f, -0.0002853757f },
		{ 0.0228970410f, 0.1596822317f, 0.1784692823f, 0.0911762089f, 0.0247722078f, 0.0014836200f, 0.0006336846f, -0.0002918032f },
		{ 0.0226871272f, 0.1569429876f, 0.1738535802f, 0.0881116869f, 0.0237384506f, 0.0014520861f, 0.0005850246f, -0.0002695441f },
	},
	{
		{ 0.0224117887f, 0.1754341882f, 0.2358673286f, 0.1639127475f, 0.0681791745f, 0.0121652720f, 0.0009673781f, -0.0002264285f },
		{ 0.0225858604f, 0.1757484914f, 0.2355883427f, 0.1637102919f, 0.0674459768f, 0.0115807171f, 0.0008045930f, -0.0002217913f },
		{ 0.0226083034f, 0.1764167309f, 0.2356512673f, 0.1627393570f, 0.0660638263f, 0.0104655933f, 0.0006267322f, -0.0002010687f },
		{ 0.0224738497f, 0.1764267824f, 0.2357274180f, 0.1617806587f, 0.0638016552f, 0.0090886937f, 0.0005944892f, -0.0002207822f },
		{ 0.0226998145f, 0.1775100680f, 0.2351945260f, 0.1591769130f, 0.0612043448f, 0.0077463075f, 0.0006900554f, -0.0002836151f },
		{ 0.0229372648f, 0.1778258515f, 0.2348042831f, 0.1564254279f, 0.0579024572f, 0.0066779906f, 0.0006801610f, -0.0002894405f },
		{ 0.0231089022f, 0.1784504853f, 0.2337131168f, 0.1529458492f, 0.0547255949f, 0.0058261477f, 0.0006823898f, -0.0002970963f },
		{ 0.0231686598f, 0.1792687226f, 0.2325191626f, 0.1489351233f, 0.0513662578f, 0.0050815843f, 0.0007152026f, -0.0003163454f },
		{ 0.0235583947f, 0.1791479129f, 0.2306328365f, 0.1449366692f, 0.0483147221f, 0.0044944074f, 0.0006967198f, -0.0003092280f },
		{ 0.0237088653f, 0.1792571960f, 0.2279609890f, 0.1402806863f, 0.0454385270f, 0.0039968801f, 0.0006830230f, -0.0003052745f },
		{ 0.0238040481f, 0.1790591987f, 0.2251173296f, 0.1358210938f, 0.0426644502f, 0.0036069268f, 0.0006758298f, -0.0003027581f },
		{ 0.0239502516f, 0.1787389002f, 0.2219409208f, 0.1312021533f, 0.0401386217f, 0.0033049865f, 0.0006362001f, -0.0002862051f },
		{ 0.0239018975f, 0.1777922036f, 0.2186942848f, 0.1265046740f, 0.0380371635f, 0.0029778226f, 0.0006530945f, -0.0002959234f },
		{ 0.0240815045f, 0.1771740057f, 0.2145345593f, 0.1218433236f, 0.0360193931f, 0.0026993446f, 0.0006657749f, -0.0003021971f },
		{ 0.0241417988f, 0.1759131362f, 0.2104874247f, 0.1175230671f, 0.0340714767f, 0.0024709301f, 0.0006549798f, -0.0002980660f },
		{ 0.0241403442f, 0.1739884621f, 0.2064401603f, 0.1132071187f, 0.0323489028f, 0.0022749282f, 0.0006554750f, -0.0002981747f },
		{ 0.0239516943f, 0.1722144223f, 0.2021223204f, 0.1091733036f, 0.0308118689f, 0.0021762550f, 0.0006045801f, -0.0002758135f },
		{ 0.0237761325f, 0.1701332794f, 0.1974917398f, 0.1051367758f, 0.0294273296f, 0.0019628050f, 0.0006364846f, -0.0002912235f },
		{ 0.0236390054f, 0.1680701420f, 0.1930567256f, 0.1014853114f, 0.0280609361f, 0.0018316171f, 0.0006266206f, -0.0002870510f },
		{ 0.0236553605f, 0.1656493228f, 0.1883089560f, 0.0979478332f, 0.0268244088f, 0.0016976610f, 0.0006200751f, -0.0002843847f },
		{ 0.0232301755f, 0.1633115137f, 0.1841858221f, 0.0940709976f, 0.0256581091f, 0.0015816726f, 0.0006316818f, -0.0002909214f },
		{ 0.0231434346f, 0.1606726430f, 0.1791291627f, 0.0909311097f, 0.0246048792f, 0.0015201950f, 0.0006063457f, -0.0002794017f },
		{ 0.0227188221f, 0.1577868607f, 0.1749114174f, 0.0877927642f, 0.0236606602f, 0.0014053680f, 0.0006062345f, -0.0002795837f },
		{ 0.0225944917f, 0.1553961591f, 0.1702790909f, 0.0847162738f, 0.0227308953f, 0.0013041644f, 0.0006078959f, -0.0002809189f },
	},
	{
		{ 0.0223491209f, 0.1726249489f, 0.2293372611f, 0.1573987532f, 0.0647097917f, 0.0115422412f, 0.0009310121f, -0.0002190136f },
		{ 0.0222349298f, 0.1729918436f, 0.2293915518f, 0.1569971300f, 0.0643949260f, 0.0108832611f, 0.0007962756f, -0.0002272904f },
		{ 0.0223505924f, 0.1734293153f, 0.2294004850f, 0.1560830688f, 0.0628883858f, 0.0098732751f, 0.0006113103f, -0.0002001525f },
		{ 0.0224831583f, 0.1739315819f, 0.2293165592f, 0.1547932014f, 0.0607236311f, 0.0085834758f, 0.0005992100f, -0.0002260289f },
		{ 0.0224643628f, 0.1745639025f, 0.2289413795f, 0.1528047449f, 0.0581191952f, 0.0074051195f, 0.0006211601f, -0.0002559147f },
		{ 0.0227113523f, 0.1750026566f, 0.2283944556f, 0.1503201224f, 0.0551552840f, 0.0063501055f, 0.0006549485f, -0.0002806480f },
		{ 0.0230434785f, 0.1754943019f, 0.2276448032f, 0.1471137479f, 0.0519241777f, 0.0055733642f, 0.0006310471f, -0.0002749814f },
		{ 0.0232400287f, 0.1762951358f, 0.2263151465f, 0.1429996308f, 0.0490433442f, 0.0047837564f, 0.0007068815f, -0.0003124583f },
		{ 0.0231796678f, 0.1762753995f, 0.2244125560f, 0.1390442636f, 0.0461311080f, 0.0042184803f, 0.0006972673f, -0.0003119655f },
		{ 0.0235347115f, 0.1761908944f, 0.2220074706f, 0.1353048923f, 0.0432100499f, 0.0038019916f, 0.0006505333f, -0.0002908920f },
		{ 0.0235693244f, 0.1764245596f, 0.2195794422f, 0.1305036151f, 0.0408370203f, 0.0033630494f, 0.0006849161f, -0.0003070954f },
		{ 0.0236892795f, 0.1756736941f, 0.2164333650f, 0.1261358023f, 0.0386242728f, 0.0030068116f, 0.0006922382f, -0.0003126508f },
		{ 0.0238305766f, 0.1750306085f, 0.2127543828f, 0.1218411879f, 0.0365011126f, 0.0027558718f, 0.0006797526f, -0.0003072841f },
		{ 0.0237683431f, 0.1740890068f, 0.2090234982f, 0.1175787369f, 0.0343883665f, 0.0025205109f, 0.0006609890f, -0.0003005199f },
		{ 0.0238011916f, 0.1729135700f, 0.2052481195f, 0.1130650455f, 0.0325938504f, 0.0023376888f, 0.0006453949f, -0.0002944397f },
		{ 0.0238008129f, 0.1712875304f, 0.2014275966f, 0.1090045953f, 0.0309285677f, 0.0021515861f, 0.0006240582f, -0.0002848755f },
		{ 0.0238281411f, 0.1695489410f, 0.1970694028f, 0.1051642364f, 0.0294902640f, 0.0019857246f, 0.0006277032f, -0.0002874019f },
		{ 0.0236631864f, 0.1677072736f, 0.1931804581f, 0.1014482713f, 0.0280630615f, 0.0018619077f, 0.0006219539f, -0.0002848658f },
		{ 0.0236553605f, 0.1656493228f, 0.1883089560f, 0.0979478332f, 0.0268244088f, 0.0016976610f, 0.0006200751f, -0.0002843847f },
		{ 0.0233923976f, 0.1635506217f, 0.1840821763f, 0.0943197166f, 0.0256430123f, 0.0015870118f, 0.0006163427f, -0.0002834802f },
		{ 0.0233165225f, 0.1611809437f, 0.1797780700f, 0.0909600929f, 0.0245349739f, 0.0014909014f, 0.0006011183f, -0.0002770660f },
		{ 0.0229249569f, 0.1585475935f, 0.1753254992f, 0.0877314787f, 0.0235414321f, 0.0013858452f, 0.0006072024f, -0.0002801075f },
		{ 0.0226947299f, 0.1561575406f, 0.1708588213f, 0.0847196733f, 0.0226344923f, 0.0012765397f, 0.0006229687f, -0.0002882128f },
		{ 0.0225280204f, 0.1531790642f, 0.1666829168f, 0.0817350301f, 0.0217431715f, 0.0012263641f, 0.0005932124f, -0.0002745474f },
	},
	{
		{ 0.0217973736f, 0.1698899541f, 0.2231934658f, 0.1510642733f, 0.0618207616f, 0.0107810948f, 0.0009489990f, -0.0002418178f },
		{ 0.0221337732f, 0.1698674779f, 0.2229073450f, 0.1510517919f, 0.0612939561f, 0.0103864852f, 0.0007283891f, -0.0002029453f },
		{ 0.0221382896f, 0.1700861524f, 0.2231136217f, 0.1501012347f, 0.0599545534f, 0.0093115272f, 0.0006375785f, -0.0002147588f },
		{ 0.0222068694f, 0.1705939716f, 0.2230771033f, 0.1489974922f, 0.0579932503f, 0.0081662240f, 0.0005856631f, -0.0002242287f },
		{ 0.0223115672f, 0.1715540796f, 0.2227617568f, 0.1468321568f, 0.0554796946f, 0.0069717045f, 0.0006267282f, -0.0002594280f },
		{ 0.0226157829f, 0.1715004431f, 0.2223022144f, 0.1448165381f, 0.0525144082f, 0.0060088867f, 0.0006341404f, -0.0002717408f },
		{ 0.0227998787f, 0.1724547868f, 0.2215169160f, 0.1411785286f, 0.0495099321f, 0.0052421358f, 0.0006260697f, -0.0002732090f },
		{ 0.0226399552f, 0.1734683251f, 0.2199928683f, 0.1374750049f, 0.0469479694f, 0.0044357552f, 0.0007419406f, -0.0003298870f },
		{ 0.0229408051f, 0.1732301916f, 0.2185590031f, 0.1340628203f, 0.0439476520f, 0.0040231643f, 0.0006768684f, -0.0003023323f },
		{ 0.0231275690f, 0.1732663574f, 0.2161469652f, 0.1298620101f, 0.0414518453f, 0.0035540286f, 0.0006693940f, -0.0003006897f },
		{ 0.0233588854f, 0.1729504932f, 0.2132175810f, 0.1258080790f, 0.0389759917f, 0.0031971438f, 0.0006574611f, -0.0002961852f },
		{ 0.0235736277f, 0.1724223955f, 0.2107836964f, 0.1214895132f, 0.0367748199f, 0.0028940510f, 0.0006391713f, -0.0002890267f },
		{ 0.0235905375f, 0.1721856399f, 0.2076395384f, 0.1172808154f, 0.0347017773f, 0.0026053142f, 0.0006640073f, -0.0003009852f },
		{ 0.0234364175f, 0.1711554558f, 0.2037605847f, 0.1131266335f, 0.0329956381f, 0.0023329986f, 0.0006689606f, -0.0003043834f },
		{ 0.0236850224f, 0.1703651074f, 0.2002358633f, 0.1089270945f, 0.0311577014f, 0.0022046468f, 0.0006305265f, -0.0002876473f },
		{ 0.0234560953f, 0.1686551798f, 0.1959019812f, 0.1050738991f, 0.0296014124f, 0.0020258674f, 0.0006216887f, -0.0002837195f },
		{ 0.0233584803f, 0.1670018986f, 0.1926345127f, 0.1011995638f, 0.0282013837f, 0.0018589208f, 0.0006315038f, -0.0002902125f },
		{ 0.0234396913f, 0.1652365028f, 0.1878623622f, 0.0976864331f, 0.0268607251f, 0.0017401467f, 0.0006220117f, -0.0002851214f },
		{ 0.0232301755f, 0.1633115137f, 0.1841858221f, 0.0940709976f, 0.0256581091f, 0.0015816726f, 0.0006316818f, -0.0002909214f },
		{ 0.0233165225f, 0.1611809437f, 0.1797780700f, 0.0909600929f, 0.0245349739f, 0.0014909014f, 0.0006011183f, -0.0002770660f },
		{ 0.0230537065f, 0.1587911140f, 0.1753282806f, 0.0878319272f, 0.0235136378f, 0.0013943812f, 0.0005993346f, -0.0002762526f },
		{ 0.0228051126f, 0.1564869744f, 0.1712913387f, 0.0845832067f, 0.0226238950f, 0.0012934545f, 0.0006081518f, -0.0002813350f },
		{ 0.0225790813f, 0.1538296812f, 0.1669293023f, 0.0818028726f, 0.0216680283f, 0.0012214306f, 0.0006025174f, -0.0002793611f },
		{ 0.0222720214f, 0.1515306005f, 0.1631418733f, 0.0788510455f, 0.0208852608f, 0.0011390638f, 0.0006023293f, -0.0002794771f },
	},
	{
		{ 0.0217377894f, 0.1669329122f, 0.2166703547f, 0.1453948162f, 0.0588292487f, 0.0102025784f, 0.0009426824f, -0.0002500342f },
		{ 0.0217350271f, 0.1667428320f, 0.2173002501f, 0.1448922109f, 0.0583218776f, 0.0098140072f, 0.0007478073f, -0.0002164977f },
		{ 0.0217836779f, 0.1670034663f, 0.2170106286f, 0.1443190126f, 0.0570583258f, 0.0088856576f, 0.0005850939f, -0.0001953270f },
		{ 0.0220416492f, 0.1673097666f, 0.2167947087f, 0.1431616347f, 0.0552126617f, 0.0077523787f, 0.0005519869f, -0.0002113376f },
		{ 0.0220219313f, 0.1681517645f, 0.2166894646f, 0.1414810894f, 0.0529790049f, 0.0066509436f, 0.0005991498f, -0.0002489929f },
		{ 0.0222590378f, 0.1683416140f, 0.2159208856f, 0.1389220601f, 0.0502887702f, 0.0056968060f, 0.0006340247f, -0.0002716807f },
		{ 0.0223991026f, 0.1694957467f, 0.2151342561f, 0.1356994809f, 0.0475749923f, 0.0048934362f, 0.0006813910f, -0.0002999763f },
		{ 0.0225063364f, 0.1697452696f, 0.2142356708f, 0.1324613040f, 0.0447088662f, 0.0042836184f, 0.0006818755f, -0.0003033888f },
		{ 0.0229633116f, 0.1696326805f, 0.2120658238f, 0.1290688153f, 0.0422183837f, 0.0037412870f, 0.0006868138f, -0.0003071730f },
		{ 0.0229242631f, 0.1699724283f, 0.2101698734f, 0.1251930374f, 0.0396906975f, 0.0033819285f, 0.0006564770f, -0.0002955578f },
		{ 0.0229956434f, 0.1697538109f, 0.2079113523f, 0.1210160820f, 0.0373737089f, 0.0030395796f, 0.0006369952f, -0.0002879424f },
		{ 0.0232203215f, 0.1693648137f, 0.2049845595f, 0.1171670998f, 0.0351306134f, 0.0027513078f, 0.0006196548f, -0.0002803443f },
		{ 0.0231700354f, 0.1689510522f, 0.2019523487f, 0.1132678097f, 0.0331672146f, 0.0025053634f, 0.0006131919f, -0.0002778523f },
		{ 0.0233866287f, 0.1683274895f, 0.1983979203f, 0.1090853533f, 0.0315840757f, 0.0022389322f, 0.0006378391f, -0.0002900821f },
		{ 0.0234504303f, 0.1668342373f, 0.1947224798f, 0.1052630713f, 0.0298678314f, 0.0020756578f, 0.0006117636f, -0.0002789297f },
		{ 0.0233483462f, 0.1656774159f, 0.1911475229f, 0.1013252715f, 0.0284202882f, 0.0019021638f, 0.0006244093f, -0.0002856022f },
		{ 0.0231705475f, 0.1645767255f, 0.1872218640f, 0.0978785959f, 0.0270236058f, 0.0017366839f, 0.0006297990f, -0.0002885441f },
		{ 0.0231037047f, 0.1625386741f, 0.1833279590f, 0.0941949011f, 0.0258018628f, 0.0016209942f, 0.0006199181f, -0.0002853757f },
		{ 0.0231434346f, 0.1606726430f, 0.1791291627f, 0.0909311097f, 0.0246048792f, 0.0015201950f, 0.0006063457f, -0.0002794017f },
		{ 0.0229249569f, 0.1585475935f, 0.1753254992f, 0.0877314787f, 0.0235414321f, 0.0013858452f, 0.0006072024f, -0.0002801075f },
		{ 0.0228051126f, 0.1564869744f, 0.1712913387f, 0.0845832067f, 0.0226238950f, 0.0012934545f, 0.0006081518f, -0.0002813350f },
		{ 0.0225420121f, 0.1540704691f, 0.1671901928f, 0.0818583754f, 0.0216764432f, 0.0012121802f, 0.0006011147f, -0.0002782904f },
		{ 0.0223231172f, 0.1515159923f, 0.1631415561f, 0.0789618750f, 0.0208993583f, 0.0010983950f, 0.0006179060f, -0.0002863541f },
		{ 0.0221653491f, 0.1490184652f, 0.1591753802f, 0.0763286596f, 0.0200636049f, 0.0010544888f, 0.0005910764f, -0.0002745876f },
	},
	{
		{ 0.0214802908f, 0.1635557979f, 0.2104988803f, 0.1398465761f, 0.0562478337f, 0.0097128126f, 0.0009059437f, -0.0002424044f },
		{ 0.0214529046f, 0.1636815364f, 0.2106259623f, 0.1394411007f, 0.0558089870f, 0.0092533281f, 0.0007624973f, -0.0002313235f },
		{ 0.0218315558f, 0.1641614896f, 0.2106617094f, 0.1387032649f, 0.0546840476f, 0.0083506249f, 0.0006183180f, -0.0002144300f },
		{ 0.0216389002f, 0.1640686454f, 0.2108611957f, 0.1376164966f, 0.0528487202f, 0.0073377271f, 0.0005602407f, -0.0002159568f },
		{ 0.0218452616f, 0.1647210340f, 0.2106803543f, 0.1358405318f, 0.0505159689f, 0.0063464670f, 0.0005702101f, -0.0002363539f },
		{ 0.0221185389f, 0.1657380216f, 0.2097637738f, 0.1334813484f, 0.0481143717f, 0.0054705899f, 0.0006080585f, -0.0002618215f },
		{ 0.0222527375f, 0.1659100232f, 0.2089965550f, 0.1308805986f, 0.0454434559f, 0.0047006910f, 0.0006295571f, -0.0002774443f },
		{ 0.0222431269f, 0.1665049987f, 0.2079525360f, 0.1276528026f, 0.0428451314f, 0.0040760012f, 0.0006706507f, -0.0002984663f },
		{ 0.0225165069f, 0.1668671339f, 0.2057650036f, 0.1241602476f, 0.0405358170f, 0.0035903629f, 0.0006646950f, -0.0002986430f },
		{ 0.0227431273f, 0.1666322853f, 0.2039637310f, 0.1205833400f, 0.0380720915f, 0.0031841940f, 0.0006489536f, -0.0002921642f },
		{ 0.0227818441f, 0.1666847218f, 0.2020974601f, 0.1167086597f, 0.0357328394f, 0.0029143658f, 0.0006083867f, -0.0002745639f },
		{ 0.0231284299f, 0.1661609356f, 0.1990909981f, 0.1128275074f, 0.0337941596f, 0.0026100729f, 0.0006181819f, -0.0002806498f },
		{ 0.0230104423f, 0.1657369134f, 0.1961706736f, 0.1090729217f, 0.0319556315f, 0.0023438124f, 0.0006240922f, -0.0002838085f },
		{ 0.0231638757f, 0.1650996388f, 0.1933375399f, 0.1051566649f, 0.0301645625f, 0.0021694749f, 0.0005988128f, -0.0002732133f },
		{ 0.0231829677f, 0.1635785413f, 0.1896794047f, 0.1017615301f, 0.0285938277f, 0.0019821390f, 0.0005881468f, -0.0002683351f },
		{ 0.0233241247f, 0.1625950771f, 0.1861471506f, 0.0979354457f, 0.0272144707f, 0.0018374274f, 0.0005909534f, -0.0002708709f },
		{ 0.0230591321f, 0.1613725920f, 0.1823292631f, 0.0943928923f, 0.0260139910f, 0.0016174386f, 0.0006270816f, -0.0002881265f },
		{ 0.0228970410f, 0.1596822317f, 0.1784692823f, 0.0911762089f, 0.0247722078f, 0.0014836200f, 0.0006336846f, -0.0002918032f },
		{ 0.0227188221f, 0.1577868607f, 0.1749114174f, 0.0877927642f, 0.0236606602f, 0.0014053680f, 0.0006062345f, -0.0002795837f },
		{ 0.0226947299f, 0.1561575406f, 0.1708588213f, 0.0847196733f, 0.0226344923f, 0.0012765397f, 0.0006229687f, -0.0002882128f },
		{ 0.0225790813f, 0.1538296812f, 0.1669293023f, 0.0818028726f, 0.0216680283f, 0.0012214306f, 0.0006025174f, -0.0002793611f },
		{ 0.0223231172f, 0.1515159923f, 0.1631415561f, 0.0789618750f, 0.0208993583f, 0.0010983950f, 0.0006179060f, -0.0002863541f },
		{ 0.0221882227f, 0.1493094265f, 0.1592881675f, 0.0763927148f, 0.0200521999f, 0.0010456484f, 0.0006038942f, -0.0002802691f },
		{ 0.0220375930f, 0.1467947361f, 0.1552281766f, 0.0737607347f, 0.0192423380f, 0.0009661195f, 0.0005980212f, -0.0002771251f },
	},
	{
		{ 0.0213883278f, 0.1601283515f, 0.2045333703f, 0.1345145040f, 0.0537106141f, 0.0092620092f, 0.0008467737f, -0.0002236253f },
		{ 0.0213997681f, 0.1606714041f, 0.2042814624f, 0.1337310115f, 0.0532881421f, 0.0087980044f, 0.0007336855f, -0.0002237366f },
		{ 0.0213073521f, 0.1607634838f, 0.2049157583f, 0.1334201157f, 0.0522385346f, 0.0079987416f, 0.0005945997f, -0.0002060877f },
		{ 0.0214386500f, 0.1609667741f, 0.2044949951f, 0.1322566962f, 0.0506393271f, 0.0069368508f, 0.0005822504f, -0.0002282776f },
		{ 0.0217089186f, 0.1616972700f, 0.2042981132f, 0.1307525366f, 0.0484323586f, 0.0060394970f, 0.0005576983f, -0.0002317464f },
		{ 0.0218278417f, 0.1620384170f, 0.2037765893f, 0.1284148704f, 0.0459476602f, 0.0051887786f, 0.0005850513f, -0.0002530860f },
		{ 0.0218187124f, 0.1624146696f, 0.2032198040f, 0.1258778190f, 0.0436635316f, 0.0044395036f, 0.0006429975f, -0.0002846285f },
		{ 0.0220690977f, 0.1633128651f, 0.2019036037f, 0.1228941604f, 0.0412088164f, 0.0038801743f, 0.0006569347f, -0.0002934408f },
		{ 0.0220783903f, 0.1632950128f, 0.2002811253f, 0.1195882408f, 0.0388186992f, 0.0034087465f, 0.0006597548f, -0.0002960921f },
		{ 0.0224171782f, 0.1633117666f, 0.1985644710f, 0.1161008231f, 0.0365225889f, 0.0030524443f, 0.0006322365f, -0.0002860493f },
		{ 0.0226483567f, 0.1632603798f, 0.1965447029f, 0.1124143733f, 0.0343914745f, 0.0027344475f, 0.0006219465f, -0.0002809715f },
		{ 0.0226445243f, 0.1627943636f, 0.1936258507f, 0.1088919000f, 0.0325043369f, 0.0024524911f, 0.0006251310f, -0.0002830144f },
		{ 0.0227982422f, 0.1627794204f, 0.1908110195f, 0.1050456343f, 0.0306519596f, 0.0022216611f, 0.0006098979f, -0.0002772164f },
		{ 0.0227379051f, 0.1619110890f, 0.1880102084f, 0.1016873228f, 0.0289838400f, 0.0020201986f, 0.0006067849f, -0.0002772294f },
		{ 0.0228338905f, 0.1610279234f, 0.1845939573f, 0.0978488934f, 0.0275746075f, 0.0018125792f, 0.0006248514f, -0.0002865137f },
		{ 0.0227744425f, 0.1596110752f, 0.1813831406f, 0.0944247338f, 0.0261668951f, 0.0017139703f, 0.0006070837f, -0.0002787635f },
		{ 0.0227572689f, 0.1583283107f, 0.1779591480f, 0.0909798084f, 0.0249285999f, 0.0015733072f, 0.0006045839f, -0.0002786016f },
		{ 0.0226871272f, 0.1569429876f, 0.1738535802f, 0.0881116869f, 0.0237384506f, 0.0014520861f, 0.0005850246f, -0.0002695441f },
		{ 0.0225944917f, 0.1553961591f, 0.1702790909f, 0.0847162738f, 0.0227308953f, 0.0013041644f, 0.0006078959f, -0.0002809189f },
		{ 0.0225280204f, 0.1531790642f, 0.1666829168f, 0.0817350301f, 0.0217431715f, 0.0012263641f, 0.0005932124f, -0.0002745474f },
		{ 0.0222720214f, 0.1515306005f, 0.1631418733f, 0.0788510455f, 0.0208852608f, 0.0011390638f, 0.0006023293f, -0.0002794771f },
		{ 0.0221653491f, 0.1490184652f, 0.1591753802f, 0.0763286596f, 0.0200636049f, 0.0010544888f, 0.0005910764f, -0.0002745876f },
		{ 0.0220375930f, 0.1467947361f, 0.1552281766f, 0.0737607347f, 0.0192423380f, 0.0009661195f, 0.0005980212f, -0.0002771251f },
		{ 0.0217673239f, 0.1445616620f, 0.1514399619f, 0.0713737717f, 0.0185561643f, 0.0009202578f, 0.0005866106f, -0.0002724636f },
	},
};

static const Scalar kGGXSpecularQuadWeightAnisoLowOrd[ 9 ][ 24 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0196411064f, 0.1813154268f, 0.2865385948f, 0.2665726752f, 0.1707040424f, 0.0654215731f, 0.0100315450f, -0.0004858001f },
		{ 0.0198291608f, 0.1818741694f, 0.2876462425f, 0.2667169413f, 0.1698991426f, 0.0624138547f, 0.0069398056f, -0.0004243815f },
		{ 0.0201748586f, 0.1831398156f, 0.2889628881f, 0.2663502342f, 0.1668848883f, 0.0562632997f, 0.0043381688f, -0.0001241634f },
		{ 0.0205908790f, 0.1843074304f, 0.2902622539f, 0.2652159785f, 0.1612844256f, 0.0495424793f, 0.0028784528f, 0.0000255714f },
		{ 0.0209692493f, 0.1857091966f, 0.2910184306f, 0.2625992611f, 0.1542290883f, 0.0431850774f, 0.0022231220f, 0.0000113126f },
		{ 0.0212050358f, 0.1872225884f, 0.2915397378f, 0.2583954244f, 0.1459948420f, 0.0378983682f, 0.0017995840f, -0.0000052548f },
		{ 0.0214284169f, 0.1887236430f, 0.2907432410f, 0.2530034433f, 0.1376372338f, 0.0332996961f, 0.0017078137f, -0.0000929803f },
		{ 0.0219505579f, 0.1893102947f, 0.2893376719f, 0.2470104358f, 0.1292197340f, 0.0296140814f, 0.0015762153f, -0.0001298135f },
		{ 0.0221765500f, 0.1898259196f, 0.2871793819f, 0.2398448824f, 0.1215872045f, 0.0264886113f, 0.0014912933f, -0.0001665186f },
		{ 0.0223057724f, 0.1900210023f, 0.2840222979f, 0.2321461696f, 0.1139320378f, 0.0240355255f, 0.0014072192f, -0.0001892923f },
		{ 0.0225141016f, 0.1896688911f, 0.2806760986f, 0.2244394311f, 0.1069141182f, 0.0218176435f, 0.0013612164f, -0.0002127567f },
		{ 0.0224735799f, 0.1891712543f, 0.2764562292f, 0.2161357422f, 0.1005518755f, 0.0199933937f, 0.0012720764f, -0.0002121792f },
		{ 0.0227600571f, 0.1879392511f, 0.2713497682f, 0.2083190880f, 0.0948050008f, 0.0184043085f, 0.0012326039f, -0.0002212197f },
		{ 0.0227591244f, 0.1865147796f, 0.2660263334f, 0.2004929051f, 0.0893089837f, 0.0170727374f, 0.0011500074f, -0.0002131633f },
		{ 0.0227303971f, 0.1851989095f, 0.2604680644f, 0.1924255572f, 0.0844057808f, 0.0158538516f, 0.0011301728f, -0.0002262318f },
		{ 0.0226465307f, 0.1831408650f, 0.2545003970f, 0.1852177665f, 0.0797533943f, 0.0147128026f, 0.0011003849f, -0.0002341897f },
		{ 0.0228233325f, 0.1808765089f, 0.2481432345f, 0.1774293582f, 0.0755965993f, 0.0138497908f, 0.0010375993f, -0.0002243793f },
		{ 0.0224589176f, 0.1783347129f, 0.2420363810f, 0.1707138175f, 0.0719788202f, 0.0128504110f, 0.0011016039f, -0.0002659822f },
		{ 0.0223224459f, 0.1757542575f, 0.2354192763f, 0.1638475318f, 0.0682439993f, 0.0121500467f, 0.0010346084f, -0.0002494383f },
		{ 0.0222873506f, 0.1728984707f, 0.2295725246f, 0.1573182630f, 0.0647593760f, 0.0114939536f, 0.0009711508f, -0.0002330266f },
		{ 0.0220162139f, 0.1695647620f, 0.2228935557f, 0.1513402007f, 0.0616877353f, 0.0108778093f, 0.0009332416f, -0.0002295545f },
		{ 0.0217679268f, 0.1666797384f, 0.2169814341f, 0.1453534880f, 0.0589171469f, 0.0102398902f, 0.0009234536f, -0.0002353339f },
		{ 0.0213638137f, 0.1634316980f, 0.2108126194f, 0.1396167256f, 0.0562106882f, 0.0097412295f, 0.0008856899f, -0.0002294556f },
		{ 0.0210544786f, 0.1602935689f, 0.2046215828f, 0.1342758710f, 0.0538607111f, 0.0091959284f, 0.0009072873f, -0.0002464703f },
	},
	{
		{ 0.0196462936f, 0.1813035665f, 0.2865484406f, 0.2665577880f, 0.1707031049f, 0.0654267222f, 0.0100280650f, -0.0004846832f },
		{ 0.0199142986f, 0.1818721980f, 0.2875963333f, 0.2666541821f, 0.1698391007f, 0.0625206349f, 0.0069092976f, -0.0004124120f },
		{ 0.0201063248f, 0.1830936430f, 0.2889492618f, 0.2664380923f, 0.1670098866f, 0.0562747523f, 0.0043318297f, -0.0001227519f },
		{ 0.0204964719f, 0.1844184638f, 0.2904586273f, 0.2650864588f, 0.1613108154f, 0.0494341790f, 0.0029597134f, -0.0000078634f },
		{ 0.0209382705f, 0.1857384832f, 0.2913992377f, 0.2623251315f, 0.1540674840f, 0.0433233375f, 0.0021661113f, 0.0000368507f },
		{ 0.0213601578f, 0.1872504192f, 0.2912475162f, 0.2584679214f, 0.1458564996f, 0.0379311727f, 0.0018281447f, -0.0000090410f },
		{ 0.0216637468f, 0.1885412560f, 0.2909023457f, 0.2529808614f, 0.1374942078f, 0.0333097795f, 0.0016905617f, -0.0000864479f },
		{ 0.0218191826f, 0.1892442848f, 0.2896783042f, 0.2469051714f, 0.1293404249f, 0.0295340914f, 0.0016105218f, -0.0001450228f },
		{ 0.0221531954f, 0.1895375199f, 0.2872488718f, 0.2398336799f, 0.1214442150f, 0.0264612165f, 0.0015091562f, -0.0001773051f },
		{ 0.0224022172f, 0.1900777399f, 0.2841780548f, 0.2320842232f, 0.1140722443f, 0.0239452204f, 0.0014348263f, -0.0002000789f },
		{ 0.0225664606f, 0.1901528861f, 0.2803734322f, 0.2243000674f, 0.1070412613f, 0.0217521412f, 0.0013836345f, -0.0002229000f },
		{ 0.0227936603f, 0.1891096939f, 0.2764325355f, 0.2165120600f, 0.1004935083f, 0.0199219067f, 0.0013087029f, -0.0002205539f },
		{ 0.0227933870f, 0.1881570834f, 0.2709597090f, 0.2082224770f, 0.0948447311f, 0.0183309389f, 0.0012887954f, -0.0002473669f },
		{ 0.0228095530f, 0.1870080060f, 0.2661704797f, 0.2000531076f, 0.0894874087f, 0.0169662077f, 0.0012126378f, -0.0002423024f },
		{ 0.0227212548f, 0.1850422156f, 0.2602144896f, 0.1924365629f, 0.0844560862f, 0.0157184702f, 0.0012074111f, -0.0002619915f },
		{ 0.0227531422f, 0.1830020216f, 0.2540723371f, 0.1851528830f, 0.0799945225f, 0.0147051677f, 0.0011271560f, -0.0002427732f },
		{ 0.0227949698f, 0.1808011127f, 0.2482417082f, 0.1776687159f, 0.0757597217f, 0.0137570227f, 0.0010718999f, -0.0002388692f },
		{ 0.0224856839f, 0.1783979500f, 0.2420130323f, 0.1705788002f, 0.0718025129f, 0.0128970831f, 0.0010461539f, -0.0002409278f },
		{ 0.0224402801f, 0.1753840567f, 0.2361142834f, 0.1638289169f, 0.0681259383f, 0.0122289240f, 0.0009603676f, -0.0002175868f },
		{ 0.0220714214f, 0.1726006698f, 0.2292312795f, 0.1573059088f, 0.0648653277f, 0.0115157464f, 0.0009392544f, -0.0002204636f },
		{ 0.0219821543f, 0.1698475498f, 0.2232312283f, 0.1510667966f, 0.0617816434f, 0.0108025132f, 0.0009601354f, -0.0002435170f },
		{ 0.0216640225f, 0.1664452185f, 0.2168831933f, 0.1453328148f, 0.0588916149f, 0.0102494123f, 0.0009251007f, -0.0002370935f },
		{ 0.0214143269f, 0.1638229851f, 0.2105578469f, 0.1397042133f, 0.0563091743f, 0.0097005985f, 0.0009277545f, -0.0002463614f },
		{ 0.0211636418f, 0.1604092947f, 0.2047396115f, 0.1340530907f, 0.0537712214f, 0.0092245397f, 0.0008813291f, -0.0002363400f },
	},
	{
		{ 0.0196606064f, 0.1812909957f, 0.2865453779f, 0.2665531075f, 0.1707337524f, 0.0654150136f, 0.0100256982f, -0.0004839793f },
		{ 0.0198336518f, 0.1818845949f, 0.2876018746f, 0.2667436147f, 0.1699127232f, 0.0623973040f, 0.0069523636f, -0.0004266894f },
		{ 0.0203211100f, 0.1827247384f, 0.2891431902f, 0.2664586243f, 0.1667413034f, 0.0563936647f, 0.0042913209f, -0.0001011104f },
		{ 0.0205144709f, 0.1843066122f, 0.2903711590f, 0.2650700552f, 0.1613752591f, 0.0495475063f, 0.0028829289f, 0.0000211944f },
		{ 0.0209211356f, 0.1861700113f, 0.2910048888f, 0.2622307472f, 0.1541190031f, 0.0431657057f, 0.0022159147f, 0.0000123234f },
		{ 0.0213181218f, 0.1870473981f, 0.2912547676f, 0.2585931900f, 0.1460268887f, 0.0377844674f, 0.0018744668f, -0.0000330903f },
		{ 0.0215335459f, 0.1883331808f, 0.2909851203f, 0.2531281370f, 0.1374837666f, 0.0332884559f, 0.0016975147f, -0.0000887755f },
		{ 0.0219596626f, 0.1892769210f, 0.2894282945f, 0.2467773439f, 0.1293477525f, 0.0296122690f, 0.0015628624f, -0.0001270723f },
		{ 0.0222073803f, 0.1901037235f, 0.2870213675f, 0.2396920617f, 0.1215002518f, 0.0264528484f, 0.0015306831f, -0.0001873029f },
		{ 0.0225015849f, 0.1896879417f, 0.2842390920f, 0.2321689250f, 0.1139529826f, 0.0240211985f, 0.0013681804f, -0.0001701035f },
		{ 0.0226394821f, 0.1896722323f, 0.2803538330f, 0.2244932211f, 0.1069238469f, 0.0217933525f, 0.0013700474f, -0.0002120211f },
		{ 0.0227072528f, 0.1890368127f, 0.2758368702f, 0.2164854477f, 0.1006278333f, 0.0199747342f, 0.0012932991f, -0.0002198724f },
		{ 0.0226738020f, 0.1882237980f, 0.2712979145f, 0.2083843008f, 0.0948724460f, 0.0184094253f, 0.0012303552f, -0.0002225969f },
		{ 0.0226186961f, 0.1868051009f, 0.2663565361f, 0.2000595145f, 0.0894749238f, 0.0170201471f, 0.0011840957f, -0.0002286174f },
		{ 0.0228973322f, 0.1851541568f, 0.2599477349f, 0.1923936942f, 0.0845061165f, 0.0157897361f, 0.0011522278f, -0.0002382023f },
		{ 0.0228447119f, 0.1832810795f, 0.2542737120f, 0.1850426635f, 0.0798699294f, 0.0147064756f, 0.0011057690f, -0.0002365285f },
		{ 0.0225458792f, 0.1808080885f, 0.2483235021f, 0.1777439491f, 0.0756891937f, 0.0138206187f, 0.0010573546f, -0.0002328231f },
		{ 0.0226352947f, 0.1780371096f, 0.2419777888f, 0.1706845948f, 0.0716633344f, 0.0129588776f, 0.0010186647f, -0.0002279292f },
		{ 0.0223995533f, 0.1754620566f, 0.2356536784f, 0.1637883628f, 0.0682355648f, 0.0122433322f, 0.0009630633f, -0.0002205880f },
		{ 0.0221608851f, 0.1730693001f, 0.2293565338f, 0.1574276580f, 0.0648004628f, 0.0114199536f, 0.0010184417f, -0.0002553840f },
		{ 0.0220255672f, 0.1696381817f, 0.2231079732f, 0.1511895610f, 0.0618458738f, 0.0108664154f, 0.0009481662f, -0.0002355878f },
		{ 0.0218012566f, 0.1666049661f, 0.2168513306f, 0.1452894934f, 0.0588008535f, 0.0102456380f, 0.0009203769f, -0.0002363995f },
		{ 0.0215469998f, 0.1635148829f, 0.2104639884f, 0.1397889692f, 0.0562872106f, 0.0096859027f, 0.0009039597f, -0.0002377838f },
		{ 0.0212623742f, 0.1601565702f, 0.2046608294f, 0.1344415154f, 0.0536601440f, 0.0092486117f, 0.0008480072f, -0.0002209912f },
	},
	{
		{ 0.0196525983f, 0.1813452926f, 0.2864952869f, 0.2665409821f, 0.1707522705f, 0.0653964193f, 0.0100427641f, -0.0004919546f },
		{ 0.0198080357f, 0.1819650329f, 0.2875452817f, 0.2667188045f, 0.1699201898f, 0.0624005919f, 0.0069534189f, -0.0004277811f },
		{ 0.0201820530f, 0.1830683925f, 0.2888837912f, 0.2664442975f, 0.1668885966f, 0.0562852731f, 0.0043362379f, -0.0001264705f },
		{ 0.0205648133f, 0.1840854979f, 0.2905836272f, 0.2651868015f, 0.1612002027f, 0.0496002760f, 0.0028624315f, 0.0000350375f },
		{ 0.0208437310f, 0.1860224311f, 0.2913782605f, 0.2621925385f, 0.1541703891f, 0.0431544244f, 0.0022292863f, 0.0000067060f },
		{ 0.0213201926f, 0.1870193769f, 0.2914128114f, 0.2585441165f, 0.1460356270f, 0.0379126344f, 0.0018299326f, -0.0000169118f },
		{ 0.0214908395f, 0.1884070966f, 0.2910867919f, 0.2528109073f, 0.1378188304f, 0.0332975342f, 0.0016992416f, -0.0000929296f },
		{ 0.0218237402f, 0.1892476064f, 0.2894536016f, 0.2469521357f, 0.1292707648f, 0.0296121809f, 0.0015445670f, -0.0001221620f },
		{ 0.0220152501f, 0.1897268224f, 0.2872130561f, 0.2396456795f, 0.1216746720f, 0.0265280859f, 0.0014991465f, -0.0001738248f },
		{ 0.0223055277f, 0.1903675115f, 0.2840713590f, 0.2322034800f, 0.1139319273f, 0.0239219451f, 0.0014254000f, -0.0001988737f },
		{ 0.0225662787f, 0.1896624586f, 0.2809106130f, 0.2243350874f, 0.1069551539f, 0.0218331494f, 0.0013312337f, -0.0002024822f },
		{ 0.0225851283f, 0.1895526243f, 0.2762753880f, 0.2159084940f, 0.1006864398f, 0.0198924024f, 0.0013529645f, -0.0002444399f },
		{ 0.0227914181f, 0.1883616251f, 0.2711269518f, 0.2083092668f, 0.0947505940f, 0.0183669369f, 0.0012488939f, -0.0002294560f },
		{ 0.0228930349f, 0.1870217280f, 0.2659461869f, 0.2005112347f, 0.0891645962f, 0.0170070749f, 0.0011722756f, -0.0002219183f },
		{ 0.0229035429f, 0.1854658940f, 0.2602497576f, 0.1921456909f, 0.0844766178f, 0.0157529709f, 0.0011693552f, -0.0002444575f },
		{ 0.0227781854f, 0.1834409794f, 0.2543648778f, 0.1847788528f, 0.0797422128f, 0.0147243110f, 0.0011235256f, -0.0002408345f },
		{ 0.0226086638f, 0.1808395283f, 0.2480629506f, 0.1777161544f, 0.0757348678f, 0.0137927557f, 0.0010686077f, -0.0002372069f },
		{ 0.0225359393f, 0.1779512867f, 0.2419882434f, 0.1708141023f, 0.0717669329f, 0.0129200062f, 0.0010338925f, -0.0002351881f },
		{ 0.0222631944f, 0.1753397547f, 0.2359223850f, 0.1638613508f, 0.0681817497f, 0.0121366833f, 0.0010152045f, -0.0002424572f },
		{ 0.0221943696f, 0.1726589166f, 0.2293155642f, 0.1574458116f, 0.0646874513f, 0.0115356031f, 0.0009379635f, -0.0002199118f },
		{ 0.0219070119f, 0.1699688684f, 0.2232415317f, 0.1512842360f, 0.0617871786f, 0.0107632220f, 0.0009843632f, -0.0002531230f },
		{ 0.0214894738f, 0.1666626828f, 0.2171773598f, 0.1452566868f, 0.0589154423f, 0.0102174759f, 0.0009590526f, -0.0002493104f },
		{ 0.0214752568f, 0.1636873234f, 0.2108106152f, 0.1396380624f, 0.0562162797f, 0.0096702796f, 0.0009296127f, -0.0002472275f },
		{ 0.0212643517f, 0.1601522525f, 0.2045819079f, 0.1340945592f, 0.0537045863f, 0.0092252427f, 0.0008710881f, -0.0002316741f },
	},
	{
		{ 0.0196560542f, 0.1813014739f, 0.2865171371f, 0.2665470767f, 0.1707555548f, 0.0653999256f, 0.0100358462f, -0.0004929396f },
		{ 0.0198550161f, 0.1819513031f, 0.2876377269f, 0.2666755208f, 0.1699283382f, 0.0623977526f, 0.0069337873f, -0.0004261861f },
		{ 0.0201806784f, 0.1830855218f, 0.2889696840f, 0.2664098443f, 0.1668222998f, 0.0563649277f, 0.0042881913f, -0.0001041537f },
		{ 0.0205554056f, 0.1844651894f, 0.2900794642f, 0.2651880244f, 0.1613264759f, 0.0495098663f, 0.0028968940f, 0.0000163992f },
		{ 0.0207994249f, 0.1861692949f, 0.2911845139f, 0.2623185499f, 0.1540751871f, 0.0431986797f, 0.0022501838f, -0.0000024519f },
		{ 0.0213826689f, 0.1873444603f, 0.2913584605f, 0.2580216088f, 0.1462347885f, 0.0377634434f, 0.0019148272f, -0.0000536318f },
		{ 0.0216452976f, 0.1882705539f, 0.2907700071f, 0.2532635289f, 0.1376807667f, 0.0332503536f, 0.0016930638f, -0.0000905412f },
		{ 0.0217539092f, 0.1894395069f, 0.2893064036f, 0.2469057440f, 0.1292332767f, 0.0295646225f, 0.0015904297f, -0.0001425499f },
		{ 0.0221941591f, 0.1897381049f, 0.2872961852f, 0.2397506066f, 0.1214502310f, 0.0265124878f, 0.0014844694f, -0.0001640839f },
		{ 0.0223249570f, 0.1899716013f, 0.2843936042f, 0.2319319563f, 0.1139928237f, 0.0239140001f, 0.0014440511f, -0.0002052932f },
		{ 0.0224436312f, 0.1896929071f, 0.2803244893f, 0.2245713929f, 0.1070689240f, 0.0218432906f, 0.0013498981f, -0.0002082206f },
		{ 0.0225419904f, 0.1896084821f, 0.2758213489f, 0.2160341107f, 0.1008223912f, 0.0199126983f, 0.0013595231f, -0.0002487735f },
		{ 0.0228055343f, 0.1881237931f, 0.2713684023f, 0.2081507787f, 0.0948377816f, 0.0183877957f, 0.0012472414f, -0.0002295751f },
		{ 0.0228889953f, 0.1867603056f, 0.2657080375f, 0.2005760037f, 0.0893991180f, 0.0169508940f, 0.0012294130f, -0.0002459468f },
		{ 0.0228494922f, 0.1853328736f, 0.2602312279f, 0.1924677522f, 0.0844066105f, 0.0158478285f, 0.0011392774f, -0.0002299779f },
		{ 0.0226017647f, 0.1830277471f, 0.2546505706f, 0.1847421460f, 0.0798956406f, 0.0147074113f, 0.0011613331f, -0.0002605218f },
		{ 0.0226257602f, 0.1806058255f, 0.2480425282f, 0.1777530393f, 0.0756983448f, 0.0137585891f, 0.0011079045f, -0.0002523390f },
		{ 0.0223634468f, 0.1784226006f, 0.2423510619f, 0.1704294508f, 0.0716433039f, 0.0129546315f, 0.0010344488f, -0.0002354671f },
		{ 0.0224387336f, 0.1755274556f, 0.2356261672f, 0.1637488770f, 0.0682243465f, 0.0121389600f, 0.0010138281f, -0.0002398649f },
		{ 0.0220815555f, 0.1731194784f, 0.2294507460f, 0.1571732679f, 0.0648393441f, 0.0114656107f, 0.0009931555f, -0.0002442443f },
		{ 0.0220005914f, 0.1696805560f, 0.2230886765f, 0.1511526378f, 0.0618434763f, 0.0108190675f, 0.0009659897f, -0.0002425680f },
		{ 0.0218122134f, 0.1666075861f, 0.2167114098f, 0.1454710758f, 0.0588806616f, 0.0102479544f, 0.0009175681f, -0.0002330295f },
		{ 0.0213954782f, 0.1635325037f, 0.2106487889f, 0.1397259826f, 0.0561800486f, 0.0097650947f, 0.0008820987f, -0.0002266140f },
		{ 0.0211323048f, 0.1601819117f, 0.2047816854f, 0.1342208322f, 0.0537853634f, 0.0091771087f, 0.0009032406f, -0.0002458642f },
	},
	{
		{ 0.0196586004f, 0.1812816185f, 0.2865373298f, 0.2665980838f, 0.1706876355f, 0.0654376286f, 0.0099912178f, -0.0004856640f },
		{ 0.0198782025f, 0.1818769861f, 0.2875353555f, 0.2667527784f, 0.1698714908f, 0.0624236868f, 0.0069219822f, -0.0004244854f },
		{ 0.0201586203f, 0.1830421598f, 0.2890511902f, 0.2663934319f, 0.1667788238f, 0.0563135130f, 0.0043219122f, -0.0001257615f },
		{ 0.0205858127f, 0.1844073504f, 0.2904895006f, 0.2650319410f, 0.1612465041f, 0.0494501134f, 0.0029294107f, 0.0000024108f },
		{ 0.0208475966f, 0.1860581197f, 0.2911528234f, 0.2625019662f, 0.1540786557f, 0.0431454584f, 0.0022495205f, -0.0000049594f },
		{ 0.0212012913f, 0.1875101889f, 0.2913059681f, 0.2582479030f, 0.1460956465f, 0.0378659840f, 0.0018556939f, -0.0000305953f },
		{ 0.0214858863f, 0.1882972229f, 0.2910695904f, 0.2530242569f, 0.1376302839f, 0.0333791411f, 0.0016510679f, -0.0000713066f },
		{ 0.0220526703f, 0.1892412762f, 0.2893137481f, 0.2468360930f, 0.1292639742f, 0.0296277176f, 0.0015784422f, -0.0001315423f },
		{ 0.0218751298f, 0.1899598408f, 0.2873723720f, 0.2396412020f, 0.1216090488f, 0.0263927796f, 0.0015434843f, -0.0001969500f },
		{ 0.0222630809f, 0.1903702434f, 0.2840415524f, 0.2320198138f, 0.1140688615f, 0.0239437271f, 0.0014613138f, -0.0002127272f },
		{ 0.0224930178f, 0.1900443132f, 0.2804167453f, 0.2241093316f, 0.1071806439f, 0.0217434185f, 0.0014168014f, -0.0002374059f },
		{ 0.0224724011f, 0.1894331973f, 0.2763133435f, 0.2161990784f, 0.1005449925f, 0.0199497774f, 0.0012998959f, -0.0002222580f },
		{ 0.0226607892f, 0.1881386448f, 0.2713484223f, 0.2082324461f, 0.0946889212f, 0.0183720484f, 0.0012278151f, -0.0002246631f },
		{ 0.0226669532f, 0.1869339798f, 0.2658124385f, 0.2003124638f, 0.0894651491f, 0.0169730833f, 0.0012458307f, -0.0002540231f },
		{ 0.0227172996f, 0.1854095291f, 0.2603586050f, 0.1922173138f, 0.0844553735f, 0.0158423242f, 0.0011444357f, -0.0002352595f },
		{ 0.0227189996f, 0.1830087765f, 0.2542282827f, 0.1850323789f, 0.0799963135f, 0.0147707631f, 0.0011028026f, -0.0002322980f },
		{ 0.0227468480f, 0.1806455245f, 0.2479865593f, 0.1778247791f, 0.0755998939f, 0.0138190315f, 0.0010553891f, -0.0002305805f },
		{ 0.0223859448f, 0.1782067349f, 0.2421020141f, 0.1707048410f, 0.0716238133f, 0.0129888409f, 0.0009944714f, -0.0002186701f },
		{ 0.0223462914f, 0.1757167919f, 0.2355104093f, 0.1639588464f, 0.0680323446f, 0.0121536754f, 0.0010116915f, -0.0002387178f },
		{ 0.0222344549f, 0.1725509034f, 0.2293635418f, 0.1574464409f, 0.0647923889f, 0.0114822068f, 0.0009797131f, -0.0002364787f },
		{ 0.0218741120f, 0.1699185785f, 0.2231049467f, 0.1511154259f, 0.0619282927f, 0.0108113739f, 0.0009716357f, -0.0002489337f },
		{ 0.0216526366f, 0.1667482566f, 0.2169742117f, 0.1452485580f, 0.0589456015f, 0.0102084719f, 0.0009721785f, -0.0002582200f },
		{ 0.0214439224f, 0.1639016357f, 0.2105283637f, 0.1397272475f, 0.0562374756f, 0.0096752492f, 0.0009138865f, -0.0002420772f },
		{ 0.0212409579f, 0.1600648191f, 0.2047163121f, 0.1346583639f, 0.0536731468f, 0.0093043750f, 0.0008427350f, -0.0002181986f },
	},
	{
		{ 0.0196394092f, 0.1813000161f, 0.2865583595f, 0.2665640525f, 0.1707474183f, 0.0653889926f, 0.0099856799f, -0.0005166762f },
		{ 0.0198611410f, 0.1819032670f, 0.2876431177f, 0.2666323258f, 0.1700125022f, 0.0623714746f, 0.0069289328f, -0.0004404180f },
		{ 0.0203694293f, 0.1829507594f, 0.2890025796f, 0.2662747347f, 0.1668210868f, 0.0563233121f, 0.0042695702f, -0.0001053721f },
		{ 0.0205802238f, 0.1844231268f, 0.2902076594f, 0.2650788943f, 0.1614175397f, 0.0494193894f, 0.0029245244f, -0.0000026051f },
		{ 0.0210183168f, 0.1858203569f, 0.2910813400f, 0.2627187371f, 0.1540216261f, 0.0431237827f, 0.0022173893f, 0.0000076335f },
		{ 0.0212133515f, 0.1872463441f, 0.2912810502f, 0.2586882188f, 0.1459358338f, 0.0377436174f, 0.0018756818f, -0.0000383090f },
		{ 0.0216041705f, 0.1885670175f, 0.2908607227f, 0.2529117424f, 0.1377053498f, 0.0332779474f, 0.0016870056f, -0.0000932309f },
		{ 0.0218865064f, 0.1895193172f, 0.2896268008f, 0.2465101690f, 0.1293410436f, 0.0296217735f, 0.0015515713f, -0.0001274130f },
		{ 0.0222620285f, 0.1901305834f, 0.2869098349f, 0.2396852095f, 0.1214516262f, 0.0265135089f, 0.0014888808f, -0.0001716952f },
		{ 0.0224831311f, 0.1901071059f, 0.2842491096f, 0.2321543075f, 0.1139691814f, 0.0239613503f, 0.0013984650f, -0.0001877326f },
		{ 0.0224829804f, 0.1898738241f, 0.2807213543f, 0.2241412183f, 0.1070413352f, 0.0217560166f, 0.0013756867f, -0.0002245495f },
		{ 0.0229124056f, 0.1894652460f, 0.2759398324f, 0.2165203646f, 0.1004464635f, 0.0198727647f, 0.0012944006f, -0.0002225920f },
		{ 0.0228184425f, 0.1882143965f, 0.2711713483f, 0.2084617485f, 0.0947340845f, 0.0183482777f, 0.0012374140f, -0.0002262046f },
		{ 0.0227922661f, 0.1868062310f, 0.2658522415f, 0.2005097260f, 0.0893092739f, 0.0169474538f, 0.0012266483f, -0.0002488911f },
		{ 0.0228926911f, 0.1851492348f, 0.2603938319f, 0.1923246617f, 0.0844532285f, 0.0158236259f, 0.0011236716f, -0.0002258944f },
		{ 0.0225184412f, 0.1835412365f, 0.2542094749f, 0.1848027409f, 0.0798301569f, 0.0147453899f, 0.0011321162f, -0.0002502549f },
		{ 0.0227051085f, 0.1805641254f, 0.2480434126f, 0.1776806796f, 0.0758010256f, 0.0138079939f, 0.0010420522f, -0.0002268885f },
		{ 0.0225488546f, 0.1784446843f, 0.2418355139f, 0.1705871652f, 0.0717562422f, 0.0129657472f, 0.0010448299f, -0.0002430139f },
		{ 0.0222179249f, 0.1754571218f, 0.2359348406f, 0.1639849849f, 0.0681979207f, 0.0121024193f, 0.0010245343f, -0.0002482114f },
		{ 0.0220378308f, 0.1728654495f, 0.2291593986f, 0.1573453097f, 0.0648982791f, 0.0114465283f, 0.0010027951f, -0.0002531369f },
		{ 0.0218780893f, 0.1699133467f, 0.2228861806f, 0.1512136389f, 0.0617084261f, 0.0108248681f, 0.0009559932f, -0.0002413725f },
		{ 0.0216979404f, 0.1667381920f, 0.2169595182f, 0.1453436918f, 0.0588030677f, 0.0102779058f, 0.0009185752f, -0.0002346829f },
		{ 0.0213807521f, 0.1635984559f, 0.2108085697f, 0.1397458281f, 0.0561814815f, 0.0097162823f, 0.0008899680f, -0.0002326074f },
		{ 0.0211289501f, 0.1601527594f, 0.2044739887f, 0.1342100928f, 0.0537811830f, 0.0092071621f, 0.0008850337f, -0.0002400740f },
	},
	{
		{ 0.0196737438f, 0.1813831323f, 0.2866751434f, 0.2665597420f, 0.1707265438f, 0.0651870305f, 0.0095838447f, -0.0005831678f },
		{ 0.0198674169f, 0.1818721182f, 0.2877815081f, 0.2666946283f, 0.1698543257f, 0.0621444886f, 0.0066336349f, -0.0004350262f },
		{ 0.0203324735f, 0.1830418507f, 0.2891851111f, 0.2664904322f, 0.1665519045f, 0.0560743015f, 0.0040856357f, -0.0001081395f },
		{ 0.0205390588f, 0.1845431810f, 0.2902795555f, 0.2651400010f, 0.1612098303f, 0.0492676176f, 0.0027771035f, 0.0000040311f },
		{ 0.0209123386f, 0.1858078720f, 0.2913309171f, 0.2625898611f, 0.1539874484f, 0.0430519509f, 0.0020371848f, 0.0000352052f },
		{ 0.0212063339f, 0.1872823891f, 0.2912683518f, 0.2584081221f, 0.1459889000f, 0.0376231712f, 0.0018000951f, -0.0000453645f },
		{ 0.0216097401f, 0.1887197418f, 0.2909174364f, 0.2528231897f, 0.1375181897f, 0.0331756751f, 0.0015707350f, -0.0000743562f },
		{ 0.0218097559f, 0.1895415896f, 0.2892757882f, 0.2468057167f, 0.1293731081f, 0.0293573507f, 0.0015704768f, -0.0001635045f },
		{ 0.0222583326f, 0.1897713406f, 0.2869677082f, 0.2399027760f, 0.1213668847f, 0.0264288966f, 0.0014177791f, -0.0001617263f },
		{ 0.0223452795f, 0.1900790034f, 0.2842778968f, 0.2320359612f, 0.1140252079f, 0.0237982739f, 0.0014149237f, -0.0002158639f },
		{ 0.0226045349f, 0.1900241453f, 0.2802571286f, 0.2244067490f, 0.1068744499f, 0.0216978367f, 0.0013013587f, -0.0002084025f },
		{ 0.0227352672f, 0.1891676449f, 0.2761845553f, 0.2162843596f, 0.1005887939f, 0.0199015041f, 0.0012565371f, -0.0002203018f },
		{ 0.0227586666f, 0.1877884379f, 0.2716686267f, 0.2083348335f, 0.0945936162f, 0.0183223290f, 0.0011725540f, -0.0002131503f },
		{ 0.0226953255f, 0.1869502896f, 0.2663858394f, 0.1999924460f, 0.0893174668f, 0.0170190708f, 0.0011015097f, -0.0002079741f },
		{ 0.0227696337f, 0.1852589575f, 0.2602393030f, 0.1923059274f, 0.0844967548f, 0.0157252317f, 0.0011121456f, -0.0002365366f },
		{ 0.0229113193f, 0.1832174353f, 0.2540345036f, 0.1850357343f, 0.0797984277f, 0.0146433983f, 0.0010853343f, -0.0002412714f },
		{ 0.0225920419f, 0.1807549740f, 0.2488769614f, 0.1777909071f, 0.0754786438f, 0.0138193188f, 0.0010049278f, -0.0002209144f },
		{ 0.0224188867f, 0.1783285199f, 0.2416780079f, 0.1707862333f, 0.0718619905f, 0.0128122660f, 0.0010493834f, -0.0002563253f },
		{ 0.0224052780f, 0.1756605018f, 0.2354682443f, 0.1637640923f, 0.0681820439f, 0.0121829244f, 0.0009620541f, -0.0002302340f },
		{ 0.0221502874f, 0.1724886216f, 0.2294116331f, 0.1572618078f, 0.0649772835f, 0.0114085269f, 0.0009735516f, -0.0002473675f },
		{ 0.0220077759f, 0.1697817190f, 0.2229297172f, 0.1510479537f, 0.0617488364f, 0.0108298054f, 0.0009066647f, -0.0002277637f },
		{ 0.0216336406f, 0.1668113736f, 0.2170740380f, 0.1450528319f, 0.0589615071f, 0.0101814428f, 0.0009338046f, -0.0002503066f },
		{ 0.0214863191f, 0.1637463503f, 0.2106140952f, 0.1395860215f, 0.0562672284f, 0.0096662332f, 0.0008793095f, -0.0002363574f },
		{ 0.0212157084f, 0.1602163063f, 0.2043136845f, 0.1345288537f, 0.0537254472f, 0.0092077263f, 0.0008467082f, -0.0002287113f },
	},
	{
		{ 0.0197312360f, 0.1814141985f, 0.2867722494f, 0.2667403711f, 0.1705506578f, 0.0648905955f, 0.0090875889f, -0.0005988107f },
		{ 0.0199595594f, 0.1819135588f, 0.2878363459f, 0.2667296894f, 0.1698048425f, 0.0618826709f, 0.0062268452f, -0.0003919658f },
		{ 0.0202168477f, 0.1831787946f, 0.2890819194f, 0.2663709697f, 0.1667382809f, 0.0557738575f, 0.0038363204f, -0.0000863607f },
		{ 0.0206225796f, 0.1844266559f, 0.2904984914f, 0.2650479633f, 0.1610105718f, 0.0491201721f, 0.0025413925f, 0.0000381372f },
		{ 0.0209845244f, 0.1859520912f, 0.2913010268f, 0.2622753261f, 0.1538619645f, 0.0428400221f, 0.0019326578f, 0.0000350469f },
		{ 0.0211881873f, 0.1876163517f, 0.2914134356f, 0.2582771640f, 0.1457898875f, 0.0373533685f, 0.0016759032f, -0.0000360820f },
		{ 0.0217249085f, 0.1886107319f, 0.2908134149f, 0.2528365989f, 0.1374257750f, 0.0330667301f, 0.0014616370f, -0.0000609843f },
		{ 0.0220733520f, 0.1893021199f, 0.2893677830f, 0.2468583835f, 0.1289824215f, 0.0293236093f, 0.0013892402f, -0.0001136767f },
		{ 0.0220909052f, 0.1902126163f, 0.2871179397f, 0.2395823074f, 0.1212142611f, 0.0262124960f, 0.0014089492f, -0.0001870358f },
		{ 0.0223784972f, 0.1902154457f, 0.2843166128f, 0.2320006602f, 0.1138137264f, 0.0237205701f, 0.0013042246f, -0.0001936227f },
		{ 0.0224455539f, 0.1897631089f, 0.2803716546f, 0.2243089421f, 0.1069960943f, 0.0215278983f, 0.0013005190f, -0.0002324038f },
		{ 0.0228695985f, 0.1893912836f, 0.2758986981f, 0.2159828436f, 0.1004839498f, 0.0197979399f, 0.0011861783f, -0.0002105269f },
		{ 0.0229392905f, 0.1880969506f, 0.2710374972f, 0.2081886351f, 0.0947476965f, 0.0181384987f, 0.0011615144f, -0.0002273089f },
		{ 0.0229314574f, 0.1868442639f, 0.2662097386f, 0.2002295695f, 0.0891469773f, 0.0168503075f, 0.0011143475f, -0.0002321861f },
		{ 0.0228934211f, 0.1853522626f, 0.2600844180f, 0.1924986771f, 0.0843469980f, 0.0155815703f, 0.0011148965f, -0.0002510271f },
		{ 0.0228752012f, 0.1832317582f, 0.2542397479f, 0.1848870182f, 0.0796891090f, 0.0146268256f, 0.0010229930f, -0.0002269403f },
		{ 0.0226364151f, 0.1811494852f, 0.2482217325f, 0.1770699197f, 0.0755551731f, 0.0136521249f, 0.0010393244f, -0.0002477060f },
		{ 0.0225884569f, 0.1782980774f, 0.2421668673f, 0.1703179162f, 0.0716505684f, 0.0128080779f, 0.0009780191f, -0.0002373017f },
		{ 0.0221540092f, 0.1754353163f, 0.2359330586f, 0.1639233966f, 0.0679880824f, 0.0120427630f, 0.0009511619f, -0.0002353853f },
		{ 0.0221341023f, 0.1733630841f, 0.2291929559f, 0.1571488459f, 0.0647833525f, 0.0113766126f, 0.0009485533f, -0.0002476619f },
		{ 0.0218764443f, 0.1700346218f, 0.2232721988f, 0.1511428472f, 0.0616966641f, 0.0107122460f, 0.0009120500f, -0.0002408035f },
		{ 0.0216639387f, 0.1666146385f, 0.2166969686f, 0.1452998797f, 0.0587660789f, 0.0101730807f, 0.0008902460f, -0.0002365123f },
		{ 0.0215101329f, 0.1636195525f, 0.2106173748f, 0.1395512154f, 0.0560848935f, 0.0096475177f, 0.0008627785f, -0.0002359303f },
		{ 0.0212360557f, 0.1604499215f, 0.2046336376f, 0.1343232148f, 0.0536379608f, 0.0090807314f, 0.0008646000f, -0.0002447868f },
	},
};

static const Scalar kGGXSpecularQuadWeightAnisoLowLow[ 9 ][ 9 ][ kGGXSpecularQuadNumNodes ] =
{
	{
		{ 0.0196366957f, 0.1812717610f, 0.2864598542f, 0.2665229694f, 0.1707520275f, 0.0654905098f, 0.0102490686f, -0.0003829698f },
		{ 0.0196367886f, 0.1812708338f, 0.2864602069f, 0.2665233097f, 0.1707519767f, 0.0654910890f, 0.0102486191f, -0.0003828950f },
		{ 0.0196376317f, 0.1812713071f, 0.2864603226f, 0.2665224881f, 0.1707522274f, 0.0654901571f, 0.0102488771f, -0.0003833237f },
		{ 0.0196377072f, 0.1812704742f, 0.2864607976f, 0.2665226515f, 0.1707536279f, 0.0654891305f, 0.0102490749f, -0.0003844222f },
		{ 0.0196390187f, 0.1812720500f, 0.2864618519f, 0.2665232776f, 0.1707497776f, 0.0654891905f, 0.0102472444f, -0.0003878048f },
		{ 0.0196353197f, 0.1812755125f, 0.2864694777f, 0.2665265341f, 0.1707476378f, 0.0654884341f, 0.0102335084f, -0.0003977355f },
		{ 0.0196265639f, 0.1812932537f, 0.2864709927f, 0.2665504144f, 0.1707450522f, 0.0654735787f, 0.0101932984f, -0.0004288503f },
		{ 0.0196459948f, 0.1813273780f, 0.2865958747f, 0.2666131794f, 0.1707136537f, 0.0652893387f, 0.0097793034f, -0.0005345431f },
		{ 0.0196742795f, 0.1813773410f, 0.2867583363f, 0.2667420247f, 0.1705431506f, 0.0650272759f, 0.0092476216f, -0.0005511387f },
	},
	{
		{ 0.0196367886f, 0.1812708338f, 0.2864602069f, 0.2665233097f, 0.1707519767f, 0.0654910890f, 0.0102486191f, -0.0003828950f },
		{ 0.0196368844f, 0.1812712062f, 0.2864596426f, 0.2665237506f, 0.1707518823f, 0.0654906372f, 0.0102488727f, -0.0003830577f },
		{ 0.0196373956f, 0.1812704204f, 0.2864607242f, 0.2665232988f, 0.1707513609f, 0.0654910363f, 0.0102486235f, -0.0003831430f },
		{ 0.0196381964f, 0.1812711689f, 0.2864578419f, 0.2665244141f, 0.1707516590f, 0.0654915876f, 0.0102472573f, -0.0003839648f },
		{ 0.0196377831f, 0.1812717452f, 0.2864577160f, 0.2665249159f, 0.1707526199f, 0.0654918508f, 0.0102452926f, -0.0003870855f },
		{ 0.0196377227f, 0.1812756872f, 0.2864685110f, 0.2665181587f, 0.1707627264f, 0.0654817510f, 0.0102363224f, -0.0003993461f },
		{ 0.0196390450f, 0.1812709194f, 0.2865070735f, 0.2665130675f, 0.1707595564f, 0.0654670708f, 0.0101921153f, -0.0004278438f },
		{ 0.0196673317f, 0.1813450806f, 0.2865802982f, 0.2666183617f, 0.1706849251f, 0.0652877734f, 0.0097779689f, -0.0005330714f },
		{ 0.0197139509f, 0.1814340212f, 0.2867696446f, 0.2665847828f, 0.1706347795f, 0.0649846578f, 0.0092590657f, -0.0005591804f },
	},
	{
		{ 0.0196376317f, 0.1812713071f, 0.2864603226f, 0.2665224881f, 0.1707522274f, 0.0654901571f, 0.0102488771f, -0.0003833237f },
		{ 0.0196373956f, 0.1812704204f, 0.2864607242f, 0.2665232988f, 0.1707513609f, 0.0654910363f, 0.0102486235f, -0.0003831430f },
		{ 0.0196372950f, 0.1812709980f, 0.2864601309f, 0.2665243237f, 0.1707505219f, 0.0654908638f, 0.0102484391f, -0.0003833771f },
		{ 0.0196354602f, 0.1812711927f, 0.2864622702f, 0.2665234180f, 0.1707522644f, 0.0654902416f, 0.0102481479f, -0.0003844354f },
		{ 0.0196371647f, 0.1812726662f, 0.2864623361f, 0.2665246304f, 0.1707493615f, 0.0654897590f, 0.0102460553f, -0.0003878038f },
		{ 0.0196330896f, 0.1812827545f, 0.2864640848f, 0.2665232681f, 0.1707539113f, 0.0654808834f, 0.0102399690f, -0.0004002674f },
		{ 0.0196456098f, 0.1812725376f, 0.2864786480f, 0.2665238588f, 0.1707754072f, 0.0654636826f, 0.0101947680f, -0.0004297107f },
		{ 0.0196747673f, 0.1813303107f, 0.2865893340f, 0.2665833982f, 0.1707004062f, 0.0653155263f, 0.0097597237f, -0.0005252431f },
		{ 0.0196912061f, 0.1814189808f, 0.2867497428f, 0.2666144386f, 0.1706557078f, 0.0649738592f, 0.0092801907f, -0.0005695406f },
	},
	{
		{ 0.0196377072f, 0.1812704742f, 0.2864607976f, 0.2665226515f, 0.1707536279f, 0.0654891305f, 0.0102490749f, -0.0003844222f },
		{ 0.0196381964f, 0.1812711689f, 0.2864578419f, 0.2665244141f, 0.1707516590f, 0.0654915876f, 0.0102472573f, -0.0003839648f },
		{ 0.0196354602f, 0.1812711927f, 0.2864622702f, 0.2665234180f, 0.1707522644f, 0.0654902416f, 0.0102481479f, -0.0003844354f },
		{ 0.0196370084f, 0.1812727192f, 0.2864589360f, 0.2665246245f, 0.1707517230f, 0.0654904877f, 0.0102471274f, -0.0003851709f },
		{ 0.0196352869f, 0.1812762246f, 0.2864538187f, 0.2665283168f, 0.1707518720f, 0.0654904317f, 0.0102443361f, -0.0003878865f },
		{ 0.0196413787f, 0.1812682664f, 0.2864699483f, 0.2665314566f, 0.1707438902f, 0.0654866356f, 0.0102346853f, -0.0003990802f },
		{ 0.0196379329f, 0.1812725492f, 0.2865056144f, 0.2665178569f, 0.1707465801f, 0.0654807686f, 0.0101881162f, -0.0004285641f },
		{ 0.0196622497f, 0.1813746669f, 0.2865642071f, 0.2666139299f, 0.1707049389f, 0.0652787280f, 0.0097767313f, -0.0005328037f },
		{ 0.0196831127f, 0.1814779822f, 0.2867446171f, 0.2665835990f, 0.1706399304f, 0.0649778609f, 0.0092666829f, -0.0005634557f },
	},
	{
		{ 0.0196390187f, 0.1812720500f, 0.2864618519f, 0.2665232776f, 0.1707497776f, 0.0654891905f, 0.0102472444f, -0.0003878048f },
		{ 0.0196377831f, 0.1812717452f, 0.2864577160f, 0.2665249159f, 0.1707526199f, 0.0654918508f, 0.0102452926f, -0.0003870855f },
		{ 0.0196371647f, 0.1812726662f, 0.2864623361f, 0.2665246304f, 0.1707493615f, 0.0654897590f, 0.0102460553f, -0.0003878038f },
		{ 0.0196352869f, 0.1812762246f, 0.2864538187f, 0.2665283168f, 0.1707518720f, 0.0654904317f, 0.0102443361f, -0.0003878865f },
		{ 0.0196354520f, 0.1812754930f, 0.2864606924f, 0.2665252693f, 0.1707554320f, 0.0654855949f, 0.0102435009f, -0.0003920428f },
		{ 0.0196362944f, 0.1812799031f, 0.2864696553f, 0.2665209262f, 0.1707477024f, 0.0654906180f, 0.0102282992f, -0.0004012139f },
		{ 0.0196452077f, 0.1812822903f, 0.2864603725f, 0.2665546205f, 0.1707393010f, 0.0654737248f, 0.0101912563f, -0.0004325585f },
		{ 0.0196408829f, 0.1813509750f, 0.2866234098f, 0.2665876024f, 0.1706941138f, 0.0652781980f, 0.0097782971f, -0.0005335979f },
		{ 0.0197227857f, 0.1813979892f, 0.2866734715f, 0.2666861791f, 0.1706700338f, 0.0649382562f, 0.0092658348f, -0.0005643378f },
	},
	{
		{ 0.0196353197f, 0.1812755125f, 0.2864694777f, 0.2665265341f, 0.1707476378f, 0.0654884341f, 0.0102335084f, -0.0003977355f },
		{ 0.0196377227f, 0.1812756872f, 0.2864685110f, 0.2665181587f, 0.1707627264f, 0.0654817510f, 0.0102363224f, -0.0003993461f },
		{ 0.0196330896f, 0.1812827545f, 0.2864640848f, 0.2665232681f, 0.1707539113f, 0.0654808834f, 0.0102399690f, -0.0004002674f },
		{ 0.0196413787f, 0.1812682664f, 0.2864699483f, 0.2665314566f, 0.1707438902f, 0.0654866356f, 0.0102346853f, -0.0003990802f },
		{ 0.0196362944f, 0.1812799031f, 0.2864696553f, 0.2665209262f, 0.1707477024f, 0.0654906180f, 0.0102282992f, -0.0004012139f },
		{ 0.0196369355f, 0.1812742989f, 0.2864802330f, 0.2665171320f, 0.1707626089f, 0.0654783444f, 0.0102219730f, -0.0004127991f },
		{ 0.0196373579f, 0.1812841572f, 0.2865142584f, 0.2665117609f, 0.1707383155f, 0.0654781120f, 0.0101758379f, -0.0004384405f },
		{ 0.0196810810f, 0.1813082504f, 0.2866197493f, 0.2666207966f, 0.1706822163f, 0.0653015195f, 0.0097638333f, -0.0005377422f },
		{ 0.0197052621f, 0.1814079745f, 0.2867184940f, 0.2666566845f, 0.1706324954f, 0.0649825638f, 0.0092432098f, -0.0005622446f },
	},
	{
		{ 0.0196265639f, 0.1812932537f, 0.2864709927f, 0.2665504144f, 0.1707450522f, 0.0654735787f, 0.0101932984f, -0.0004288503f },
		{ 0.0196390450f, 0.1812709194f, 0.2865070735f, 0.2665130675f, 0.1707595564f, 0.0654670708f, 0.0101921153f, -0.0004278438f },
		{ 0.0196456098f, 0.1812725376f, 0.2864786480f, 0.2665238588f, 0.1707754072f, 0.0654636826f, 0.0101947680f, -0.0004297107f },
		{ 0.0196379329f, 0.1812725492f, 0.2865056144f, 0.2665178569f, 0.1707465801f, 0.0654807686f, 0.0101881162f, -0.0004285641f },
		{ 0.0196452077f, 0.1812822903f, 0.2864603725f, 0.2665546205f, 0.1707393010f, 0.0654737248f, 0.0101912563f, -0.0004325585f },
		{ 0.0196373579f, 0.1812841572f, 0.2865142584f, 0.2665117609f, 0.1707383155f, 0.0654781120f, 0.0101758379f, -0.0004384405f },
		{ 0.0196485605f, 0.1812898903f, 0.2864969597f, 0.2665383723f, 0.1707476189f, 0.0654594238f, 0.0101318461f, -0.0004621646f },
		{ 0.0196681934f, 0.1813479842f, 0.2866140883f, 0.2666138255f, 0.1706750475f, 0.0652863230f, 0.0097017808f, -0.0005416234f },
		{ 0.0197181976f, 0.1814036521f, 0.2867793030f, 0.2666275018f, 0.1706349611f, 0.0649326717f, 0.0092323709f, -0.0005795904f },
	},
	{
		{ 0.0196459948f, 0.1813273780f, 0.2865958747f, 0.2666131794f, 0.1707136537f, 0.0652893387f, 0.0097793034f, -0.0005345431f },
		{ 0.0196673317f, 0.1813450806f, 0.2865802982f, 0.2666183617f, 0.1706849251f, 0.0652877734f, 0.0097779689f, -0.0005330714f },
		{ 0.0196747673f, 0.1813303107f, 0.2865893340f, 0.2665833982f, 0.1707004062f, 0.0653155263f, 0.0097597237f, -0.0005252431f },
		{ 0.0196622497f, 0.1813746669f, 0.2865642071f, 0.2666139299f, 0.1707049389f, 0.0652787280f, 0.0097767313f, -0.0005328037f },
		{ 0.0196408829f, 0.1813509750f, 0.2866234098f, 0.2665876024f, 0.1706941138f, 0.0652781980f, 0.0097782971f, -0.0005335979f },
		{ 0.0196810810f, 0.1813082504f, 0.2866197493f, 0.2666207966f, 0.1706822163f, 0.0653015195f, 0.0097638333f, -0.0005377422f },
		{ 0.0196681934f, 0.1813479842f, 0.2866140883f, 0.2666138255f, 0.1706750475f, 0.0652863230f, 0.0097017808f, -0.0005416234f },
		{ 0.0196716623f, 0.1813923454f, 0.2867367430f, 0.2666085826f, 0.1706584649f, 0.0651428744f, 0.0094035006f, -0.0005879527f },
		{ 0.0197399011f, 0.1814161077f, 0.2868670204f, 0.2667188493f, 0.1705976478f, 0.0647224275f, 0.0088703921f, -0.0006133692f },
	},
	{
		{ 0.0196742795f, 0.1813773410f, 0.2867583363f, 0.2667420247f, 0.1705431506f, 0.0650272759f, 0.0092476216f, -0.0005511387f },
		{ 0.0197139509f, 0.1814340212f, 0.2867696446f, 0.2665847828f, 0.1706347795f, 0.0649846578f, 0.0092590657f, -0.0005591804f },
		{ 0.0196912061f, 0.1814189808f, 0.2867497428f, 0.2666144386f, 0.1706557078f, 0.0649738592f, 0.0092801907f, -0.0005695406f },
		{ 0.0196831127f, 0.1814779822f, 0.2867446171f, 0.2665835990f, 0.1706399304f, 0.0649778609f, 0.0092666829f, -0.0005634557f },
		{ 0.0197227857f, 0.1813979892f, 0.2866734715f, 0.2666861791f, 0.1706700338f, 0.0649382562f, 0.0092658348f, -0.0005643378f },
		{ 0.0197052621f, 0.1814079745f, 0.2867184940f, 0.2666566845f, 0.1706324954f, 0.0649825638f, 0.0092432098f, -0.0005622446f },
		{ 0.0197181976f, 0.1814036521f, 0.2867793030f, 0.2666275018f, 0.1706349611f, 0.0649326717f, 0.0092323709f, -0.0005795904f },
		{ 0.0197399011f, 0.1814161077f, 0.2868670204f, 0.2667188493f, 0.1705976478f, 0.0647224275f, 0.0088703921f, -0.0006133692f },
		{ 0.0197293578f, 0.1815247718f, 0.2868315902f, 0.2667236500f, 0.1705646109f, 0.0646660954f, 0.0087290608f, -0.0006161293f },
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

	inline Scalar AnisoQuadCellLowOrd( const int lowIdx, const int ordinaryIdx, const int nodeIdx )
	{
		const int idx = ( lowIdx <= 0 ) ? 1 : lowIdx;
		if( idx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ) {
			return kGGXSpecularQuadWeightAniso[0][ordinaryIdx][nodeIdx];
		}
		if( idx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL ) {
			return kGGXSpecularQuadWeightAniso[1][ordinaryIdx][nodeIdx];
		}
		return kGGXSpecularQuadWeightAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(idx)][ordinaryIdx][nodeIdx];
	}

	inline Scalar AnisoQuadCellLowLow( const int xIdxRaw, const int yIdxRaw, const int nodeIdx )
	{
		const int xIdx = ( xIdxRaw <= 0 ) ? 1 : xIdxRaw;
		const int yIdx = ( yIdxRaw <= 0 ) ? 1 : yIdxRaw;
		const bool xBoundary = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 || xIdx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL;
		const bool yBoundary = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 || yIdx >= MicrofacetEnergyLUT::ALPHA_LOW_TOTAL;
		if( xBoundary && yBoundary ) {
			const int xo = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			const int yo = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadWeightAniso[xo][yo][nodeIdx];
		}
		if( yBoundary ) {
			const int yo = yIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadWeightAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(xIdx)][yo][nodeIdx];
		}
		if( xBoundary ) {
			const int xo = xIdx == MicrofacetEnergyLUT::ALPHA_SUB_FINE + 1 ? 0 : 1;
			return kGGXSpecularQuadWeightAnisoLowOrd[MicrofacetEnergyLUT::AlphaLowSlot(yIdx)][xo][nodeIdx];
		}
		return kGGXSpecularQuadWeightAnisoLowLow
			[MicrofacetEnergyLUT::AlphaLowSlot(xIdx)]
			[MicrofacetEnergyLUT::AlphaLowSlot(yIdx)][nodeIdx];
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
		const bool xLow = alphaX < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;
		const bool yLow = alphaY < MicrofacetEnergyLUT::ANISO_ALPHA_LOW_A1;

		if( !xLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = kGGXSpecularQuadWeightAniso[xi0][yi0][nodeIdx];
			const Scalar v01 = kGGXSpecularQuadWeightAniso[xi0][yi1][nodeIdx];
			const Scalar v10 = kGGXSpecularQuadWeightAniso[xi1][yi0][nodeIdx];
			const Scalar v11 = kGGXSpecularQuadWeightAniso[xi1][yi1][nodeIdx];
			return ( Scalar(1) - xf ) * ( ( Scalar(1) - yf ) * v00 + yf * v01 )
				+ xf * ( ( Scalar(1) - yf ) * v10 + yf * v11 );
		}

		if( !yLow )
		{
			int xi0, xi1, yi0, yi1; Scalar xf, yf;
			MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
			MicrofacetEnergyLUT::AnisoAlphaIndex( alphaY, yi0, yi1, yf );
			const Scalar v00 = AnisoQuadCellLowOrd( xi0, yi0, nodeIdx );
			const Scalar v01 = AnisoQuadCellLowOrd( xi0, yi1, nodeIdx );
			const Scalar v10 = AnisoQuadCellLowOrd( xi1, yi0, nodeIdx );
			const Scalar v11 = AnisoQuadCellLowOrd( xi1, yi1, nodeIdx );
			return ( Scalar(1) - xf ) * ( ( Scalar(1) - yf ) * v00 + yf * v01 )
				+ xf * ( ( Scalar(1) - yf ) * v10 + yf * v11 );
		}

		int xi0, xi1, yi0, yi1; Scalar xf, yf;
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaX, xi0, xi1, xf );
		MicrofacetEnergyLUT::AnisoAlphaLowIndex( alphaY, yi0, yi1, yf );
		const Scalar v00 = AnisoQuadCellLowLow( xi0, yi0, nodeIdx );
		const Scalar v01 = AnisoQuadCellLowLow( xi0, yi1, nodeIdx );
		const Scalar v10 = AnisoQuadCellLowLow( xi1, yi0, nodeIdx );
		const Scalar v11 = AnisoQuadCellLowLow( xi1, yi1, nodeIdx );
		return ( Scalar(1) - xf ) * ( ( Scalar(1) - yf ) * v00 + yf * v01 )
			+ xf * ( ( Scalar(1) - yf ) * v10 + yf * v11 );
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
	const Scalar EavgAniso = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );
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
	const Scalar EavgAniso = MicrofacetEnergyLUT::LookupEavgG2Aniso( alphaX, alphaY );
	const Scalar R_ss = GGXSpecularSingleScatterBihemisphericalNM( interfaceFresnel, alphaX, alphaY, nm );
	const Scalar specColor = GuardedGetColorNM( *pSpecular, ri, nm );
	const Scalar F_ms = ComputeGGXFmsNM( ri, fresnelMode, specColor, *pIOR, *pExtinction, pFilmIOR, pFilmExtinction, pFilmThickness, nm, EavgAniso );
	const Scalar R_ms = F_ms * (Scalar(1) - EavgAniso);

	out = R_ss + R_ms + GuardedGetColorNM( *pDiffuse, ri, nm ) * GGXInterfaceFresnel::Transmission( mean, mean );
	return true;
}
