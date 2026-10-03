//////////////////////////////////////////////////////////////////////
//
//  CoatedBRDF.cpp - Closed-form combined coat + substrate response.
//    See CoatedBRDF.h for the model and CoatedLayer.h for the layer
//    algebra and its energy-conservation proof.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CoatedBRDF.h"
#include "CoatedLayer.h"
#include "GGXBRDF.h"
#include "LambertianBRDF.h"
#include "OrenNayarBRDF.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"
#include "../Modifiers/ModifierFrame.h"
#include "../Interfaces/ILog.h"

#include <atomic>
#include <cmath>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

CoatedBRDF::CoatedBRDF(
	const IBSDF& base,
	const IScalarPainter& coatWeight,
	const IScalarPainter& coatIOR,
	const IScalarPainter& coatRoughness,
	const IScalarPainter& coatThickness,
	const IScalarPainter& coatAbsorption,
	const IPainter& coatTint,
	const bool recyclingCompensation,
	const bool baseScattersFullSphere,
	const IPainter* coatNormal,
	const Scalar coatNormalScale_
	) :
  pBase( &base ),
  pCoatWeight( &coatWeight ),
  pCoatIOR( &coatIOR ),
  pCoatRoughness( &coatRoughness ),
  pCoatThickness( &coatThickness ),
  pCoatAbsorption( &coatAbsorption ),
  pCoatTint( &coatTint ),
  bRecycling( recyclingCompensation ),
  bBaseFullSphere( baseScattersFullSphere ),
  pCoatNormal( coatNormal ),
  coatNormalScale( coatNormalScale_ ),
  substrateModel( eSubstrateOuterFrame ),
  pGGXBase( 0 )
{
	// DL-388: the substrate model is a property of the substrate's CLASS
	// (CoatedMaterial's allowlist is closed).  See "SUBSTRATE IN THE
	// COAT'S FRAME" below for what each model evaluates.
	if( dynamic_cast<const LambertianBRDF*>( &base ) ) {
		substrateModel = eSubstrateLambertian;
	} else if( dynamic_cast<const OrenNayarBRDF*>( &base ) ) {
		substrateModel = eSubstrateRefractedCosine;
	} else if( ( pGGXBase = dynamic_cast<const GGXBRDF*>( &base ) ) != 0 ) {
		substrateModel = eSubstrateRefractedLobe;
	}

	pBase->addref();
	pCoatWeight->addref();
	pCoatIOR->addref();
	pCoatRoughness->addref();
	pCoatThickness->addref();
	pCoatAbsorption->addref();
	pCoatTint->addref();
	if( pCoatNormal ) {
		pCoatNormal->addref();
	}
}

CoatedBRDF::~CoatedBRDF()
{
	safe_release( pBase );
	safe_release( pCoatWeight );
	safe_release( pCoatIOR );
	safe_release( pCoatRoughness );
	safe_release( pCoatThickness );
	safe_release( pCoatAbsorption );
	safe_release( pCoatTint );
	safe_release( pCoatNormal );	// null-checks internally; may already be 0
}

// Editor rebind.  addref BEFORE release so a self-rebind is safe; see
// the contract note in CoatedBRDF.h.  No SPF forwarder is needed --
// CoatedSPF reads these back through this object.
void CoatedBRDF::SetCoatWeight( const IScalarPainter& v )     { v.addref(); safe_release( pCoatWeight );     pCoatWeight     = &v; }
void CoatedBRDF::SetCoatIOR( const IScalarPainter& v )        { v.addref(); safe_release( pCoatIOR );        pCoatIOR        = &v; }
void CoatedBRDF::SetCoatRoughness( const IScalarPainter& v )  { v.addref(); safe_release( pCoatRoughness );  pCoatRoughness  = &v; }
void CoatedBRDF::SetCoatThickness( const IScalarPainter& v )  { v.addref(); safe_release( pCoatThickness );  pCoatThickness  = &v; }
void CoatedBRDF::SetCoatAbsorption( const IScalarPainter& v ) { v.addref(); safe_release( pCoatAbsorption ); pCoatAbsorption = &v; }
void CoatedBRDF::SetCoatTint( const IPainter& v )             { v.addref(); safe_release( pCoatTint );       pCoatTint       = &v; }

void CoatedBRDF::ResolveCoat(
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	CoatParams& out
	) const
{
	const bool spectral = ( nm >= Scalar(0) );

	const Scalar w  = spectral ? pCoatWeight->GetValueAtNM( ri, nm )     : pCoatWeight->GetValuesAt( ri ).v[0];
	const Scalar n1 = spectral ? pCoatIOR->GetValueAtNM( ri, nm )        : pCoatIOR->GetValuesAt( ri ).v[0];
	const Scalar a  = spectral ? pCoatRoughness->GetValueAtNM( ri, nm )  : pCoatRoughness->GetValuesAt( ri ).v[0];
	const Scalar th = spectral ? pCoatThickness->GetValueAtNM( ri, nm )  : pCoatThickness->GetValuesAt( ri ).v[0];
	const Scalar ab = spectral ? pCoatAbsorption->GetValueAtNM( ri, nm ) : pCoatAbsorption->GetValuesAt( ri ).v[0];

	out.weight = r_min( r_max( w, Scalar(0) ), Scalar(1) );

	// Relative IOR against the ambient medium recorded on the hit
	// (`ri.ambientIOR`, default 1.0 = air).  Deliberately NOT
	// `ior_stack.top()`: `value()` has no IOR stack, and using two
	// different outside-IOR sources in the evaluator and the sampler
	// would break the value <-> Scatter <-> Pdf agreement that
	// SPFBSDFConsistencyTest / SPFPdfConsistencyTest enforce.
	//
	// eta is clamped to >= 1: the layer model assumes the coat is
	// optically DENSER than what surrounds it (light entering never
	// totally-internally-reflects, and the recycling series is driven
	// by TIR on the way OUT).  A coat less dense than the ambient
	// medium is out of the scope 7.2 describes; the descriptor says so.
	const Scalar ambient = ( ri.ambientIOR > Scalar(0) ) ? ri.ambientIOR : Scalar(1);
	out.ambient = ambient;
	out.eta = r_min( r_max( n1 / ambient, Scalar(1) ), CoatedLayer::MaxRelativeIOR() );

	out.alpha      = r_min( r_max( a, CoatedLayer::kMinCoatAlpha ), Scalar(1) );
	out.thickness  = r_max( th, Scalar(0) );
	out.absorption = r_max( ab, Scalar(0) );

	// "Is the coat tinted at all?" is decided from the AUTHORED RGB
	// triple, which carries no Jakob-Hanika uplift, and the SAME answer
	// drives both pipes.  Deciding it per-wavelength from GetColorNM
	// would make an untinted (white) coat's spectral sample disagree
	// with the RGB path -- pre-Stage-C (2026-09-02,
	// docs/SPECTRAL_ILLUMINANT_CONVENTION.md) that disagreement was an
	// opaque coat above ~640 nm; post-Stage-C it is merely "1 - epsilon
	// instead of exactly 1", epsilon wavelength-dependent -- see
	// CoatedLayer::PassTransmittance / IPainter.h's IsUntintedWhite for
	// the measured curve either way.  Epsilon guards the RGB side against
	// a painter that returns 1 - 1e-16 rather than exactly 1.
	const RISEPel tintRGB = ReflectanceColor( *pCoatTint, ri );
	const Scalar  minTint = r_min( r_min( tintRGB[0], tintRGB[1] ), tintRGB[2] );
	out.tinted = ( minTint < Scalar(1) - Scalar(1e-6) );

	if( !out.tinted ) {
		// Untinted: never sample the spectral pipe at all, so no
		// per-wavelength deviation from exact white -- pre-Stage-C the
		// JH red-end collapse, post-Stage-C the residual 1-epsilon --
		// can leak in.  `tint` is left at white so any diagnostic
		// reading it sees the authored value.
		out.tint = RISEPel( 1, 1, 1 );
	} else if( spectral ) {
		const Scalar t = r_max( Scalar(0), pCoatTint->GetColorNM( ri, nm ) );	// DL-386: negative -> 0, as ReflectanceColorNM
		out.tint = RISEPel( t, t, t );
	} else {
		out.tint = tintRGB;
	}

	out.re = CoatedLayer::ExternalDiffuseFresnel( out.eta );
	// `ri` is ALWAYS the true internal diffuse reflectance, even when
	// the compensation is switched off.  The red-proof lever deletes
	// only the GEOMETRIC SERIES 1/(1 - r_i R) -- the `(1 - r_i)` EXIT
	// factor is part of Weidlich-Wilkie's single bounce and stays.
	//
	// Zeroing `ri` outright would delete both, which is not the model
	// 7.4 describes and would also desynchronise `albedo` from `value`:
	// `value` reaches the exit factor through the DIRECTIONAL
	// T(theta_o), whose hemispherical average is (1 - r_e)/eta^2 ==
	// (1 - r_i) and which no toggle touches, while `albedo` carries
	// the interior-transport `escape` term (== 1 - r_i for a clear coat).  Consumers of the toggle therefore gate the
	// recycling factor itself -- see RecyclingFactor* below.
	out.ri = CoatedLayer::InternalDiffuseFresnel( out.eta );
}

namespace
{
	//! Shading frame oriented to face the incoming ray, matching
	//! GGXBRDF::value / GGXSPF::Scatter's FlipW so the coat lobe, the
	//! sampler and the substrate all agree on which side is "up".
	inline RISE::OrthonormalBasis3D RayFacingONB( const RISE::RayIntersectionGeometric& ri )
	{
		RISE::OrthonormalBasis3D onb = ri.onb;
		if( RISE::Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
			onb.FlipW();
		}
		return onb;
	}

	//! DL-192: once-per-process warning when a coat-normal painter is
	//! bound at a hit with no coherent tangent frame (mirrors
	//! `NormalMap.cpp`'s identical, SEPARATELY-scoped flag -- a scene
	//! using both an object-level normal map AND a coat normal on the
	//! same tangent-less primitive gets one warning from each, which is
	//! correct: they are two independent perturbations with the same
	//! underlying limitation).
	std::atomic<bool> g_warnedCoatNoTangentFrame{ false };

	//! DL-192: decode `coatNormal`'s tangent-space normal at `ri` and
	//! perturb `baseOnb` (the substrate's own ray-facing frame) by it,
	//! same glTF convention `NormalMap.cpp` uses (RGB [0,1] -> [-1,1],
	//! z reconstructed). Independent shader directions retain normalMapOnb
	//! for original UV decode, unaffected by base normal modifiers; legacy
	//! hits use baseOnb unchanged (including their inherited layering). Anisotropy must
	//! not redirect the normal texture's UV tilt. One shared function
	//! serves all coat evaluation/sampling paths (DL-100).
	inline RISE::Vector3 DecodeCoatPerturbedNormal(
		const RISE::RayIntersectionGeometric& ri,
		const RISE::OrthonormalBasis3D& baseOnb,
		const RISE::IPainter& coatNormalPainter,
		const RISE::Scalar scale )
	{
		using namespace RISE;

		if( !ri.bShadingTangentFromGeometry && !ri.bHasShadingTangent &&
		    !g_warnedCoatNoTangentFrame.exchange( true ) ) {
			GlobalLog()->PrintEasyWarning(
				"coated_material: coat_normal is bound at a hit with no coherent "
				"tangent frame (no imported TANGENT, no valid UV derivatives, no "
				"geometry-supplied shading tangent).  Falling back to an arbitrary "
				"ONB-aligned frame, which is correct only when the normal map's UV "
				"axes happen to align with it -- i.e. essentially never.  This "
				"warning fires once per process; subsequent hits are silent." );
		}

		const RISEPel encoded = coatNormalPainter.GetColor( ri );
		const Scalar nx = ( Scalar(2) * encoded.r - Scalar(1) ) * scale;
		const Scalar ny = ( Scalar(2) * encoded.g - Scalar(1) ) * scale;
		const Scalar nzSqr = Scalar(1) - nx*nx - ny*ny;
		const Scalar nz = ( nzSqr > 0 ) ? std::sqrt( nzSqr ) : Scalar(0);

		OrthonormalBasis3D decodeFrame=ri.bHasNormalMapFrame ? ri.normalMapOnb : baseOnb;
        if (Vector3Ops::Dot(decodeFrame.w(),baseOnb.w())<0) decodeFrame.FlipW();
        return Vector3Ops::Normalize(decodeFrame.u()*nx+decodeFrame.v()*ny+decodeFrame.w()*nz);
	}

	//! Geometric-horizon gate, identical in construction to
	//! GGXBRDF::value's (ray-anchored so a tilted shading normal can't
	//! flip it to the wrong side).
	inline bool PassesHorizonGate(
		const RISE::Vector3& v,
		const RISE::Vector3& r,
		const RISE::Vector3& n,
		const RISE::RayIntersectionGeometric& ri )
	{
		const RISE::Vector3& geomNRaw = ( RISE::Vector3Ops::SquaredModulus( ri.vGeomNormal ) > RISE::Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const RISE::Vector3 geomN = ( RISE::Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		return RISE::Vector3Ops::Dot( v, geomN ) > 0 && RISE::Vector3Ops::Dot( r, geomN ) > 0;
	}

	//! The coat's own GGX reflection lobe: dielectric Fresnel at the
	//! microfacet normal, height-correlated Smith G2, plus the
	//! Kulla-Conty multiple-scattering tail (7.4's stated role for
	//! MicrofacetEnergyLUT in this material).  `re` is the coat's
	//! hemispherical Fresnel average, which for a dielectric IS the
	//! F_avg the Kulla-Conty compensation wants -- closed-form, no
	//! conductor quadrature needed.
	RISE::Scalar CoatLobeValue(
		const RISE::Vector3& v,						// toward the light
		const RISE::Vector3& r,						// toward the viewer
		const RISE::OrthonormalBasis3D& onb,
		const RISE::Scalar nv,
		const RISE::Scalar nr,
		const RISE::Scalar alpha,
		const RISE::Scalar eta,
		const RISE::Scalar re )
	{
		using namespace RISE;

		// DL-192: `onb`/`nv`/`nr` may now be the COAT's OWN perturbed
		// frame (ResolveCoatFrame), not the substrate's -- a tilted
		// coat normal can put either direction below ITS OWN horizon
		// even though both are above the substrate's.  Guard here
		// (rather than at every caller) so a negative product can never
		// reach `single`'s division below; pre-DL-192 callers always
		// passed the substrate's already-gated nv/nr (both provably
		// positive at every call site), so this is a no-op for them.
		if( nv <= 0 || nr <= 0 ) {
			return 0;
		}

		Scalar spec = 0;

		const Vector3 h = Vector3Ops::Normalize( v + r );
		const Vector3 h_local(  Vector3Ops::Dot( h, onb.u() ), Vector3Ops::Dot( h, onb.v() ), Vector3Ops::Dot( h, onb.w() ) );
		const Vector3 wi_local( Vector3Ops::Dot( v, onb.u() ), Vector3Ops::Dot( v, onb.v() ), Vector3Ops::Dot( v, onb.w() ) );
		const Vector3 wo_local( Vector3Ops::Dot( r, onb.u() ), Vector3Ops::Dot( r, onb.v() ), Vector3Ops::Dot( r, onb.w() ) );

		const Scalar D  = MicrofacetUtils::GGX_D_Aniso<Scalar>( alpha, alpha, h_local );
		const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alpha, alpha, wi_local, wo_local );
		const Scalar F  = CoatedLayer::Fresnel( r_max( Scalar(0), Vector3Ops::Dot( r, h ) ), eta );

		const Scalar single = D * G2 / ( Scalar(4) * nv * nr );
		if( single > 0 ) {
			spec = F * single;
		}

		// DL-63: the coat lobe's single-scatter term above uses
		// height-correlated G2 (GGX_G2_Aniso), so its Kulla-Conty
		// compensation must be calibrated to the SAME model --
		// LookupEavgG2/LookupEssG2, not LookupEavg/LookupEss (which are
		// calibrated to the separable G1(wi)*G1(wo) model CookTorrance
		// renders with instead).  Sibling of GGXBRDF's own DL-63 fix.
		// DL-77 does NOT apply here: this function takes a single scalar
		// `alpha` (line above), not an alphaX/alphaY pair -- the coat
		// lobe's GGX_G2_Aniso call two lines up is always invoked with
		// alpha==alpha, i.e. always isotropic, so the isotropic-LUT-vs-
		// anisotropic-render mismatch GGXBRDF/GGXSPF are exposed to
		// (see their own DL-77 comments) has no anisotropic case to
		// mismatch against here.
		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
		if( ( Scalar(1) - Eavg ) > Scalar(1e-10) )
		{
			const Scalar Ess_o = MicrofacetEnergyLUT::LookupEssG2( nr, alpha );
			const Scalar Ess_i = MicrofacetEnergyLUT::LookupEssG2( nv, alpha );
			const Scalar f_ms  = ( Scalar(1) - Ess_o ) * ( Scalar(1) - Ess_i ) * INV_PI / ( Scalar(1) - Eavg );
			const Scalar F_ms  = MicrofacetEnergyLUT::ComputeFms<Scalar>( re, Eavg );
			spec += F_ms * f_ms;
		}

		return r_max( Scalar(0), spec );
	}

	//! DL-342: the coat's interior diffuse transport (CoatedLayer.h's
	//! InteriorDiffuseTransport) per RGB channel.  A grey (untinted)
	//! coat has one optical depth for all three channels, so the
	//! quadrature runs once; a tinted coat runs it per channel.
	struct InteriorRGB
	{
		RISE::RISEPel returned;
		RISE::RISEPel escape;
		RISE::RISEPel entry;
	};

	inline RISE::Implementation::CoatedLayer::InteriorDiffuse InteriorAt(
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const RISE::Scalar tint )
	{
		using namespace RISE::Implementation;
		const RISE::Scalar tau = CoatedLayer::OpticalDepth( cp.thickness, cp.absorption, tint, cp.tinted );
		return CoatedLayer::InteriorDiffuseTransportCached( cp.eta, tau, cp.re, cp.ri );
	}

	inline InteriorRGB InteriorDiffuseRGB( const RISE::Implementation::CoatedBRDF::CoatParams& cp )
	{
		using namespace RISE;
		InteriorRGB out;
		if( !cp.tinted ) {
			const Implementation::CoatedLayer::InteriorDiffuse d = InteriorAt( cp, Scalar(1) );
			out.returned = RISEPel( d.returned, d.returned, d.returned );
			out.escape   = RISEPel( d.escape,   d.escape,   d.escape );
			out.entry    = RISEPel( d.entry,    d.entry,    d.entry );
			return out;
		}
		for( int c = 0; c < 3; ++c ) {
			const Implementation::CoatedLayer::InteriorDiffuse d = InteriorAt( cp, cp.tint[c] );
			out.returned[c] = d.returned;
			out.escape[c]   = d.escape;
			out.entry[c]    = d.entry;
		}
		return out;
	}

	//! 7.4's geometric series, or unity when the red-proof lever has
	//! switched it off.  The ONE place the toggle acts, so `value`,
	//! `valueNM`, `albedo` and `hemisphericalAlbedo{,NM}` cannot drift
	//! apart on it.  `returned` is the round trip's returned fraction
	//! E_ret (r_i for a clear coat; DL-342 for an absorbing one).
	inline RISE::RISEPel RecyclingFactorRGB(
		const bool enabled,
		const RISE::RISEPel& returned,
		const RISE::RISEPel& R )
	{
		using namespace RISE;
		if( !enabled ) {
			return RISEPel( 1, 1, 1 );
		}
		return Implementation::CoatedLayer::RecyclingRGB( returned, R );
	}

	inline RISE::Scalar RecyclingFactorNM(
		const bool enabled,
		const RISE::Scalar returned,
		const RISE::Scalar R )
	{
		using namespace RISE;
		if( !enabled ) {
			return Scalar(1);
		}
		return Implementation::CoatedLayer::Recycling( returned, R, Scalar(1) );
	}
}

//////////////////////////////////////////////////////////////////////
// SUBSTRATE IN THE COAT'S FRAME (DL-388).
//
// Single bounce.  Radiance L arriving from outer direction wi (cosine
// c_i) enters the smooth coat with transmittance T(c_i); inside, the
// n^2 law gives radiance eta^2 T L in the solid angle
// dw' = c_i dw / (eta^2 mu_i) about the refracted direction wi' (mu_i its
// internal cosine), so the substrate's irradiance is T(c_i) L c_i dw --
// flux is conserved.  The substrate returns f_b(wi', wo') of it toward
// wo', one traversal each way attenuates by a(mu_i) a(mu_o), and leaving
// divides radiance by eta^2:
//
//     f_1(wi, wo) = T(c_i) T(c_o) a(mu_i) a(mu_o) f_b(wi', wo') / eta^2
//
// The 1/eta^2 is the solid-angle compression and nothing else, and
// f_b is read at the REFRACTED directions.  Reciprocal because T, a and
// f_b are.  For a Lambertian f_b is a constant, so the outer-frame
// expression the pre-DL-388 code used was exact there -- and only there:
// a GGX diffuse lobe's (1 - A(mu)) read at the OUTER grazing angle where
// the light actually leaves at the critical one (~42 deg inside at
// eta 1.5), and a glossy lobe's whole energy (outer frame: T^2/eta^2 of
// the lobe instead of T^2) were both lost.
//
// Recycling.  What the substrate sends up from wi' either escapes the
// coat (weight Psi(mu) = (1 - F_in(mu)) a(mu) per direction) or is
// returned to it after the coat's underside reflects it (Phi(mu) =
// F_in(mu) a(mu)^2; F_in = 1 below the critical cosine).  Write
//     g(mu) = INT f_b(w, u) mu_u Phi(mu_u) du      (first-bounce return)
//     rho(mu) = INT f_b(w, u) mu_u du               (directional albedo)
// and treat the RETURNED field as a reservoir that re-enters the
// substrate with a cosine-weighted distribution.  The reservoir's loop
// gain and escape are Q = 2 INT rho mu Phi and E = 2 INT rho mu Psi; the
// recycled kernel, made reciprocal by coupling OUT through the same g:
//
//     M(wi', wo') = g(mu_i) g(mu_o) E / ( pi G (1 - Q) ),
//     G = 2 INT g(mu) mu Psi(mu) dmu
//
// so the full substrate term is f_1 + T T a a M / eta^2.  Three
// properties fix this form:
//   * the energy M carries out of direction wi' is g(mu_i) E / (1 - Q),
//     i.e. exactly what the reservoir model says the returned g(mu_i)
//     eventually delivers -- and since E + Q <= 2 INT rho mu <= 1, that is
//     at most g(mu_i): the layer never returns more than the substrate
//     reflects, for ANY substrate, given g;
//   * a lossless, albedo-1 substrate of ANY lobe shape is then exactly 1
//     (E = 1 - Q there, and g = rho - escape = 1 - e_1);
//   * for a substrate that is ONE separable lobe f_b = s(w) s(u) (a
//     Lambertian) M is exactly the rest of the interreflection series,
//     f_b q / (1 - q) -- for a Lambertian the pre-DL-388 1/(1 - E_ret R).
//     GGX's diffuse and multiscatter lobes are each separable, so the
//     kernel is exact for either alone and a rank-one approximation for
//     their sum.
// Where the reservoir assumption fails: light a SMOOTH glossy lobe
// returns near the critical angle stays near it (the mirror keeps its
// polar angle), bouncing between total internal reflection and the
// substrate and losing (1 - rho) and a^2 every round trip, while the
// reservoir lets it escape after one re-randomisation -- a smooth metal
// (alpha 0.05) at 70-85 deg reads 1.06-1.24 of the composite (DL-423).
// The furnace of a white metal reads up to 1.007 (the substrate's own
// GGX directional albedo is 1.003 at normal incidence, the rest is the
// tables' escape/return split).
// The alternatives the derivation rules out, both measured against the
// DL-24 composite: amplifying f_b itself by 1/(1 - q) (the pre-DL-388
// shape) recycles the part of a glossy lobe that escapes on the first
// bounce, 1.2-2.0x the composite on a glossy metal; and a separable proxy
// built from the directional albedo alone, rho(i) rho(o) q / (pi R (1-q)),
// returns up to 60 % of a mirror-like lobe that in fact escapes, 1.4x on
// a smooth metal at normal incidence.  The in-coupling must be the
// substrate's own g.
//
// Which substrates get which model (SubstrateModel): Lambertian keeps
// the pre-DL-388 expression verbatim (it IS the formula above); Oren-Nayar
// is diffuse enough that the cosine reservoir (the old recycling factor
// on the refracted f_b) is within 1 % of the composite; GGX uses the lobe
// reservoir, its g/rho/Q/E/G built below; fabric and weave keep the outer
// frame (DL-417).
//////////////////////////////////////////////////////////////////////
namespace
{
	using RISE::Scalar;

	//! The GGX lobe tables: for each roughness node and internal view
	//! cosine node, a histogram over the outgoing cosine mu_u of
	//! INT f_ss(w, u) mu_u du with the microfacet Fresnel replaced by the
	//! two Schlick basis functions 1 and (1 - w.m)^5 (Schlick's F0 is the
	//! exact combination; conductor / thin-film are projected onto it).
	//! Built ONCE per process (magic static) by deterministic VNDF
	//! quadrature: E_vndf[ F G2/G1 ] is exactly INT F D G2/(4 mu_w) dw_m.
	constexpr int    kLobeN   = 32;							//!< mu grid: view nodes AND outgoing bins
	constexpr int    kLobeA   = 24;							//!< roughness nodes, uniform in sqrt(alpha)
	constexpr int    kLobeS   = 32;							//!< VNDF strata per axis
	constexpr Scalar kLobeTA0 = 0.031622776601683794;		//!< sqrt(1e-3) = sqrt(kMinCoatAlpha-scale lowest alpha)

	inline Scalar LobeAlphaNode( const int a )
	{
		const Scalar t = kLobeTA0 + ( Scalar(1) - kLobeTA0 ) * Scalar(a) / Scalar(kLobeA - 1);
		return t * t;
	}

	struct LobeSpillTable
	{
		std::vector<Scalar> t0;		//!< F = 1
		std::vector<Scalar> t5;		//!< F = (1 - w.m)^5
		std::vector<Scalar> tm;		//!< F = 1, times mu_u: the bin's mass centroid is tm / t0
	};

	LobeSpillTable BuildLobeSpillTable()
	{
		using namespace RISE;
		LobeSpillTable T;
		T.t0.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.t5.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.tm.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );
		const Scalar invS2 = Scalar(1) / Scalar( kLobeS * kLobeS );
		for( int a = 0; a < kLobeA; ++a ) {
			const Scalar alpha = LobeAlphaNode( a );
			for( int j = 0; j < kLobeN; ++j ) {
				const Scalar mu = ( Scalar(j) + Scalar(0.5) ) / Scalar(kLobeN);
				const Scalar s  = sqrt( r_max( Scalar(0), Scalar(1) - mu * mu ) );
				const Vector3 w = Vector3Ops::Normalize( onb.u() * s + onb.w() * mu );
				const Vector3 wl( Vector3Ops::Dot( w, onb.u() ), Vector3Ops::Dot( w, onb.v() ), Vector3Ops::Dot( w, onb.w() ) );
				const Scalar G1 = MicrofacetUtils::GGX_G1_Aniso( alpha, alpha, wl );
				Scalar* row0 = &T.t0[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* row5 = &T.t5[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* rowm = &T.tm[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				for( int s1 = 0; s1 < kLobeS; ++s1 ) {
					for( int s2 = 0; s2 < kLobeS; ++s2 ) {
						const Scalar u1 = ( Scalar(s1) + Scalar(0.5) ) / Scalar(kLobeS);
						const Scalar u2 = ( Scalar(s2) + Scalar(0.5) ) / Scalar(kLobeS);
						const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( w, onb, alpha, alpha, u1, u2 );
						const Scalar wm = Vector3Ops::Dot( w, m );
						if( !( wm > Scalar(0) ) ) continue;
						const Vector3 u = m * ( Scalar(2) * wm ) - w;
						const Scalar uz = Vector3Ops::Dot( u, onb.w() );
						if( !( uz > Scalar(0) ) ) continue;
						const Vector3 ul( Vector3Ops::Dot( u, onb.u() ), Vector3Ops::Dot( u, onb.v() ), uz );
						const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alpha, alpha, wl, ul );
						const Scalar wt = ( G1 > Scalar(0) ) ? G2 / G1 * invS2 : Scalar(0);
						const int k = r_min( kLobeN - 1, (int)( uz * Scalar(kLobeN) ) );
						const Scalar om = Scalar(1) - wm;
						const Scalar om2 = om * om;
						row0[k] += wt;
						row5[k] += wt * om2 * om2 * om;
						rowm[k] += wt * uz;
					}
				}
				// Calibrate the row's total to the single-scatter albedo
				// GGXBRDF's own multiscatter term is the complement of
				// (MicrofacetEnergyLUT::LookupEssG2): the quadrature reads up
				// to 0.6 % high, and an uncalibrated row makes a white
				// metal's spec + multiscatter albedo exceed 1.
				Scalar sum = 0;
				for( int k = 0; k < kLobeN; ++k ) sum += row0[k];
				if( sum > Scalar(0) ) {
					const Scalar scale = MicrofacetEnergyLUT::LookupEssG2( mu, alpha ) / sum;
					for( int k = 0; k < kLobeN; ++k ) { row0[k] *= scale; row5[k] *= scale; rowm[k] *= scale; }
				}
			}
		}
		return T;
	}

	inline const LobeSpillTable& SpillTable()
	{
		static const LobeSpillTable t = BuildLobeSpillTable();
		return t;
	}

	//! Everything about the lobe reservoir that depends only on
	//! (alpha, eta, tau): the coat's bin weights, the spec part's
	//! per-view-node return / escape / albedo for each Fresnel basis
	//! function, and the hemispherical integrals.  Per-channel colours
	//! (diffuse c, F0, F_ms) enter linearly afterwards.
	struct LobeBasis
	{
		Scalar alpha, eta, tau;
		bool   valid;
		Scalar phi[kLobeN], psi[kLobeN];				//!< bin-averaged Phi = F_in a^2, Psi = (1 - F_in) a
		Scalar g0[kLobeN], g5[kLobeN];					//!< spec return at view node j, per Fresnel basis
		Scalar e0[kLobeN], e5[kLobeN];					//!< spec escape at view node j
		Scalar Q0, Q5, E0, E5, G0, G5, H0, H5;			//!< spec hemispherical integrals
		Scalar JAphi, JApsi;							//!< 2 INT (1-(1-u)^5) u {Phi,Psi} du  (diffuse shape)
		Scalar JMphi, JMpsi;							//!< 2 INT (1-Ess(u)) u {Phi,Psi} du   (multiscatter shape)
		Scalar Eavg;
	};

	void BuildLobeBasis( LobeBasis& b, const Scalar alpha, const Scalar eta, const Scalar tau )
	{
		using namespace RISE;
		using namespace RISE::Implementation;
		b.alpha = alpha; b.eta = eta; b.tau = tau; b.valid = true;
		b.JAphi = b.JApsi = b.JMphi = b.JMpsi = 0;
		const bool opaque = !( tau < std::numeric_limits<Scalar>::infinity() );

		// The coat's weights on a fine sub-grid.  Phi and Psi carry the
		// square-root kink of F_in at the critical cosine, so every
		// hemispherical integral below is taken on this sub-grid with the
		// EXACT weights, and the spec node arrays are interpolated onto it:
		// the integrals are then the ones value()'s own out-coupling and
		// escape actually perform (G especially -- the kernel's
		// normalisation must match the integral of what it multiplies, or
		// a lossless substrate stops summing to 1).
		const int kSub = 8;
		const int nS = kLobeN * kSub;
		Scalar phiS[kLobeN * 8], psiS[kLobeN * 8];
		for( int k = 0; k < kLobeN; ++k ) {
			Scalar ps = 0, ss = 0;
			for( int s = 0; s < kSub; ++s ) {
				const int    i  = k * kSub + s;
				const Scalar mu = ( Scalar(i) + Scalar(0.5) ) / Scalar(nS);
				const Scalar F  = CoatedLayer::FresnelInside( mu, eta );
				const Scalar a  = opaque ? Scalar(0) : ( ( tau > Scalar(0) ) ? exp( -tau / mu ) : Scalar(1) );
				phiS[i] = F * a * a;
				psiS[i] = ( Scalar(1) - F ) * a;
				ps += phiS[i]; ss += psiS[i];
				const Scalar wq = Scalar(2) * mu / Scalar(nS);
				const Scalar om = Scalar(1) - mu;
				const Scalar sA = Scalar(1) - om * om * om * om * om;
				const Scalar sM = Scalar(1) - MicrofacetEnergyLUT::LookupEssG2( mu, alpha );
				b.JAphi += wq * sA * phiS[i];  b.JApsi += wq * sA * psiS[i];
				b.JMphi += wq * sM * phiS[i];  b.JMpsi += wq * sM * psiS[i];
			}
			b.phi[k] = ps / Scalar(kSub);
			b.psi[k] = ss / Scalar(kSub);
		}

		// roughness interpolation, linear in sqrt(alpha)
		const LobeSpillTable& T = SpillTable();
		Scalar x = ( sqrt( r_max( alpha, Scalar(0) ) ) - kLobeTA0 ) / ( Scalar(1) - kLobeTA0 ) * Scalar(kLobeA - 1);
		x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeA - 1) );
		const int    ia = r_min( (int)x, kLobeA - 2 );
		const Scalar fa = x - Scalar(ia);

		// Per view node: the spec lobe's return, escape and albedo for each
		// Fresnel basis function.  Each histogram bin is weighted at its
		// mass CENTROID (interpolated on the fine weight grid), not by the
		// bin average: within one bin next to the critical cosine F_in
		// falls from ~0.6 to ~0.2, and a lobe whose mirror direction sits
		// in that bin would otherwise be charged the bin's average return
		// (measured: a white mirror-like metal at 70 deg read 1.03).
		auto fine = [&]( const Scalar* arr, const Scalar mu ) {
			Scalar xi = mu * Scalar(nS) - Scalar(0.5);
			xi = r_min( r_max( xi, Scalar(0) ), Scalar(nS - 1) );
			const int    i = r_min( (int)xi, nS - 2 );
			const Scalar f = xi - Scalar(i);
			return arr[i] + ( arr[i + 1] - arr[i] ) * f;
		};
		Scalar p0[kLobeN], p5[kLobeN];
		for( int j = 0; j < kLobeN; ++j ) {
			const std::size_t oa = ( (std::size_t)ia * kLobeN + j ) * kLobeN;
			const std::size_t ob = ( (std::size_t)( ia + 1 ) * kLobeN + j ) * kLobeN;
			Scalar g0 = 0, g5 = 0, e0 = 0, e5 = 0, q0 = 0, q5 = 0;
			for( int k = 0; k < kLobeN; ++k ) {
				const Scalar t0 = T.t0[oa + k] + ( T.t0[ob + k] - T.t0[oa + k] ) * fa;
				const Scalar t5 = T.t5[oa + k] + ( T.t5[ob + k] - T.t5[oa + k] ) * fa;
				const Scalar tm = T.tm[oa + k] + ( T.tm[ob + k] - T.tm[oa + k] ) * fa;
				Scalar ph = b.phi[k], pv = b.psi[k];
				if( t0 > Scalar(0) ) {
					const Scalar mc = tm / t0;
					ph = fine( phiS, mc );
					pv = fine( psiS, mc );
				}
				g0 += t0 * ph;  g5 += t5 * ph;
				e0 += t0 * pv;  e5 += t5 * pv;
				q0 += t0;       q5 += t5;
			}
			b.g0[j] = g0; b.g5[j] = g5; b.e0[j] = e0; b.e5[j] = e5;
			p0[j] = q0; p5[j] = q5;
		}

		// Hemispherical integrals on the sub-grid, against the node arrays
		// interpolated exactly as LobeDirection reads them.
		auto interp = []( const Scalar* arr, const Scalar mu ) {
			Scalar xi = mu * Scalar(kLobeN) - Scalar(0.5);
			xi = r_min( r_max( xi, Scalar(0) ), Scalar(kLobeN - 1) );
			const int    i = r_min( (int)xi, kLobeN - 2 );
			const Scalar f = xi - Scalar(i);
			return arr[i] + ( arr[i + 1] - arr[i] ) * f;
		};
		b.Q0 = b.Q5 = b.E0 = b.E5 = b.G0 = b.G5 = b.H0 = b.H5 = 0;
		for( int i = 0; i < nS; ++i ) {
			const Scalar mu = ( Scalar(i) + Scalar(0.5) ) / Scalar(nS);
			const Scalar wq = Scalar(2) * mu / Scalar(nS);
			const Scalar r0 = interp( p0, mu ), r5 = interp( p5, mu );
			b.Q0 += wq * r0 * phiS[i];  b.Q5 += wq * r5 * phiS[i];
			b.E0 += wq * r0 * psiS[i];  b.E5 += wq * r5 * psiS[i];
			b.G0 += wq * interp( b.g0, mu ) * psiS[i];  b.G5 += wq * interp( b.g5, mu ) * psiS[i];
			b.H0 += wq * interp( b.e0, mu ) * psiS[i];  b.H5 += wq * interp( b.e5, mu ) * psiS[i];
		}
		b.Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
	}

	//! Per-thread 4-entry memo keyed on (alpha, eta, tau): a coat and a
	//! substrate with uniform painters ask for the same one to three
	//! bases (one per channel when tinted) on every evaluation.  A
	//! textured roughness / coat thickness misses and pays the build
	//! (~2 us).  Returns a COPY so no caller holds a reference into a slot
	//! a later miss may overwrite.
	inline void LobeBasisCached( LobeBasis& out, const Scalar alpha, const Scalar eta, const Scalar tau )
	{
		static thread_local LobeBasis cache[4];
		static thread_local bool init = false;
		static thread_local unsigned int next = 0;
		if( !init ) {
			for( int i = 0; i < 4; ++i ) cache[i].valid = false;
			init = true;
		}
		for( int i = 0; i < 4; ++i ) {
			if( cache[i].valid && cache[i].alpha == alpha && cache[i].eta == eta && cache[i].tau == tau ) {
				out = cache[i];
				return;
			}
		}
		LobeBasis& e = cache[next];
		next = ( next + 1 ) & 3u;
		BuildLobeBasis( e, alpha, eta, tau );
		out = e;
	}

	//! One channel's colours: the diffuse albedo, the Fresnel at normal
	//! incidence (Schlick's F0; the projection for conductor / film) and
	//! the Kulla-Conty multiscatter tint -- exactly what GGXBRDF::value
	//! multiplies its three lobes by.
	struct LobeChannel { Scalar c; Scalar F0; Scalar Fms; };

	//! Per-direction (channel-independent within one basis) pieces.
	struct LobeDir { Scalar g0, g5, e0, e5, sA, sM; };

	inline Scalar LobeNode( const Scalar* arr, const Scalar mu )
	{
		Scalar x = mu * Scalar(kLobeN) - Scalar(0.5);
		x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeN - 1) );
		const int    i = r_min( (int)x, kLobeN - 2 );
		const Scalar f = x - Scalar(i);
		return arr[i] + ( arr[i + 1] - arr[i] ) * f;
	}

	inline LobeDir LobeDirection( const LobeBasis& b, const Scalar mu )
	{
		LobeDir d;
		d.g0 = LobeNode( b.g0, mu );  d.g5 = LobeNode( b.g5, mu );
		d.e0 = LobeNode( b.e0, mu );  d.e5 = LobeNode( b.e5, mu );
		const Scalar om = Scalar(1) - mu;
		d.sA = Scalar(1) - om * om * om * om * om;
		d.sM = Scalar(1) - RISE::MicrofacetEnergyLUT::LookupEssG2( mu, b.alpha );
		return d;
	}

	//! Diffuse-lobe coefficient c (1 - F0)^2 and multiscatter coefficient
	//! F_ms / (1 - Eavg): the scale of each separable lobe's s(w) s(u).
	inline Scalar LobeDiffuseK( const LobeChannel& ch ) { return ch.c * ( Scalar(1) - ch.F0 ) * ( Scalar(1) - ch.F0 ); }
	inline Scalar LobeMsK( const LobeBasis& b, const LobeChannel& ch )
	{
		return ( ( Scalar(1) - b.Eavg ) > Scalar(1e-10) ) ? ch.Fms / ( Scalar(1) - b.Eavg ) : Scalar(0);
	}

	inline Scalar LobeReturn( const LobeBasis& b, const LobeChannel& ch, const LobeDir& d )
	{
		return ch.F0 * d.g0 + ( Scalar(1) - ch.F0 ) * d.g5
		     + LobeDiffuseK( ch ) * d.sA * b.JAphi
		     + LobeMsK( b, ch ) * d.sM * b.JMphi;
	}

	inline Scalar LobeEscape( const LobeBasis& b, const LobeChannel& ch, const LobeDir& d )
	{
		return ch.F0 * d.e0 + ( Scalar(1) - ch.F0 ) * d.e5
		     + LobeDiffuseK( ch ) * d.sA * b.JApsi
		     + LobeMsK( b, ch ) * d.sM * b.JMpsi;
	}

	struct LobeTotals { Scalar Q, E, G, H; };

	inline LobeTotals LobeHemispherical( const LobeBasis& b, const LobeChannel& ch )
	{
		// The separable lobes' directional albedo is k (1 - Ā) s(mu) with
		// 2 INT s(u) u du = 20/21 (diffuse) and 1 - Eavg (multiscatter),
		// so their Q / E are k * that * J; G and H are k * J * J.
		const Scalar kd = LobeDiffuseK( ch );
		const Scalar km = LobeMsK( b, ch );
		const Scalar aD = Scalar(20) / Scalar(21);
		const Scalar aM = Scalar(1) - b.Eavg;
		LobeTotals t;
		t.Q = ch.F0 * b.Q0 + ( Scalar(1) - ch.F0 ) * b.Q5 + kd * aD * b.JAphi + km * aM * b.JMphi;
		t.E = ch.F0 * b.E0 + ( Scalar(1) - ch.F0 ) * b.E5 + kd * aD * b.JApsi + km * aM * b.JMpsi;
		t.G = ch.F0 * b.G0 + ( Scalar(1) - ch.F0 ) * b.G5 + kd * b.JAphi * b.JApsi + km * b.JMphi * b.JMpsi;
		t.H = ch.F0 * b.H0 + ( Scalar(1) - ch.F0 ) * b.H5 + kd * b.JApsi * b.JApsi + km * b.JMpsi * b.JMpsi;
		return t;
	}

	//! E / (1 - Q): the reservoir's eventual escape per unit returned.
	inline Scalar LobeReservoirEscape( const LobeTotals& t )
	{
		const Scalar denom = Scalar(1) - t.Q;
		return ( denom > Scalar(1e-6) ) ? t.E / denom : t.E * Scalar(1e6);
	}

	//! g(mu_i) g(mu_o) E / (pi G (1 - Q)).
	inline Scalar LobeKernel( const LobeTotals& t, const Scalar gI, const Scalar gO )
	{
		if( !( t.G > Scalar(1e-12) ) ) {
			return Scalar(0);
		}
		return gI * gO * LobeReservoirEscape( t ) / ( RISE::PI * t.G );
	}

	//! Reads one GGX substrate's lobe colours at `riIn` (RGB).
	void ReadGGXLobeRGB(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		Scalar& alpha,
		LobeChannel ch[3] )
	{
		using namespace RISE;
		using namespace RISE::Implementation;
		Scalar ax = r_max( g.GetAlphaX().GetValuesAt( riIn ).v[0], Scalar(1e-4) );
		Scalar ay = r_max( g.GetAlphaY().GetValuesAt( riIn ).v[0], Scalar(1e-4) );
		if( riIn.glossyFilterWidth > 0 ) {
			ax = r_min( ax + riIn.glossyFilterWidth, Scalar(1) );
			ay = r_min( ay + riIn.glossyFilterWidth, Scalar(1) );
		}
		alpha = sqrt( ax * ay );
		const GGXInterfaceFresnel fr { riIn, g.GetFresnelMode(), g.GetSpecular(), g.GetIOR(), g.GetExtinction(),
			g.GetFilmIOR(), g.GetFilmExtinction(), g.GetFilmThickness() };
		const RISEPel F0   = fr.Directional( Scalar(1) );
		const RISEPel mean = fr.Mean();
		const RISEPel c    = ReflectanceColor( g.GetDiffuse(), riIn );
		const Scalar  Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
		const RISEPel Fms  = ( ( Scalar(1) - Eavg ) > Scalar(1e-10) ) ? MicrofacetEnergyLUT::ComputeFms<RISEPel>( mean, Eavg ) : RISEPel( 0, 0, 0 );
		for( int i = 0; i < 3; ++i ) {
			ch[i].c   = c[i];
			ch[i].F0  = r_min( r_max( F0[i], Scalar(0) ), Scalar(1) );
			ch[i].Fms = Fms[i];
		}
	}

	void ReadGGXLobeNM(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		const Scalar nm,
		Scalar& alpha,
		LobeChannel& ch )
	{
		using namespace RISE;
		using namespace RISE::Implementation;
		Scalar ax = r_max( g.GetAlphaX().GetValueAtNM( riIn, nm ), Scalar(1e-4) );
		Scalar ay = r_max( g.GetAlphaY().GetValueAtNM( riIn, nm ), Scalar(1e-4) );
		if( riIn.glossyFilterWidth > 0 ) {
			ax = r_min( ax + riIn.glossyFilterWidth, Scalar(1) );
			ay = r_min( ay + riIn.glossyFilterWidth, Scalar(1) );
		}
		alpha = sqrt( ax * ay );
		const GGXInterfaceFresnel fr { riIn, g.GetFresnelMode(), g.GetSpecular(), g.GetIOR(), g.GetExtinction(),
			g.GetFilmIOR(), g.GetFilmExtinction(), g.GetFilmThickness() };
		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
		ch.c   = ReflectanceColorNM( g.GetDiffuse(), riIn, nm );
		ch.F0  = r_min( r_max( fr.DirectionalNM( Scalar(1), nm ), Scalar(0) ), Scalar(1) );
		ch.Fms = ( ( Scalar(1) - Eavg ) > Scalar(1e-10) ) ? MicrofacetEnergyLUT::ComputeFms<Scalar>( fr.MeanNM( nm ), Eavg ) : Scalar(0);
	}

	//! The coat's per-channel optical depth (the exponent of
	//! CoatedLayer::PassTransmittance).
	inline Scalar ChannelTau( const RISE::Implementation::CoatedBRDF::CoatParams& cp, const Scalar tint )
	{
		return RISE::Implementation::CoatedLayer::OpticalDepth( cp.thickness, cp.absorption, tint, cp.tinted );
	}
}

RayIntersectionGeometric CoatedBRDF::MakeSubstrateRecord(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& onb,
	const CoatParams& cp
	) const
{
	RayIntersectionGeometric out( ri );
	const Vector3 r   = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 rIn = CoatedLayer::RefractIntoCoat( r, onb.w(), cp.eta );
	out.ray.Set( Point3Ops::mkPoint3( ri.ptIntersection, rIn ), -rIn );
	out.onb = onb;
	out.ambientIOR = cp.eta * cp.ambient;
	return out;
}

Scalar CoatedBRDF::RefractedSampleFraction() const
{
	// Half the substrate branch samples the GGX lobe in the coat's frame
	// (the right lobe shape once refracted out) and half at the outer
	// directions (which covers the broad recycled field and the
	// uncovered `1 - coat_weight` fraction without losing the internal
	// draws total internal reflection traps).  See CoatedSPF.cpp.
	return ( substrateModel == eSubstrateRefractedLobe ) ? Scalar(0.5) : Scalar(0);
}

OrthonormalBasis3D CoatedBRDF::ResolveCoatFrame(
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& baseOnb
	) const
{
	if( !pCoatNormal ) {
		return baseOnb;
	}
	OrthonormalBasis3D coatOnb;
	coatOnb.CreateFromW(
		DecodeCoatPerturbedNormal( ri, baseOnb, *pCoatNormal, coatNormalScale ) );
	return coatOnb;
}

RISEPel CoatedBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n = onb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );			// toward the light
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );		// toward the viewer

	const Scalar nv = Vector3Ops::Dot( n, v );
	const Scalar nr = Vector3Ops::Dot( n, r );

	// The VIEW must be on the shading-normal side; `n` is already the
	// ray-facing normal, so this is a degeneracy guard, not a real gate
	// (mirrors FabricBRDF::ComputeTerms's identical `nDotV` check).
	if( nr < NEARZERO ) {
		return RISEPel( 0, 0, 0 );
	}

	if( nv < -NEARZERO && bBaseFullSphere )
	{
		// DL-23 -- TRANSMISSION.  The light is on the FAR side of the
		// surface from the viewer; reached only when the substrate
		// itself reports `ScattersFullSphere()` (a `transmission thin`
		// weave_material, or fabric_material wrapping one).  See
		// CoatedBRDF.h's "TRANSMISSION THROUGH THE COAT" section for the
		// derivation.  Bit-identical to the pre-DL-23 code for every
		// substrate that cannot transmit, since `bBaseFullSphere` is
		// false for all of them.
		const RISEPel fBaseT = pBase->value( vLightIn, ri );

		CoatParams cp;
		ResolveCoat( ri, Scalar(-1), cp );
		if( cp.weight <= 0 ) {
			return fBaseT;
		}

		const Scalar absNv = -nv;
		const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr,    cp.eta );
		const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( absNv, cp.eta );
		const RISEPel Ain  = CoatedLayer::PassTransmittanceRGB( nr,    cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
		const RISEPel Aout = CoatedLayer::PassTransmittanceRGB( absNv, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );

		const RISEPel R   = SubstrateAlbedo( ri );
		const RISEPel rec = RecyclingFactorRGB( bRecycling, InteriorDiffuseRGB( cp ).returned, R );
		const RISEPel K   = Ain * Aout * rec * ( Tin * Tout / ( cp.eta * cp.eta ) );

		// No separate coat term: the coat's own GGX lobe is
		// reflection-only and has no transmission to add (FabricBRDF.h's
		// "sheen stays 0" precedent) -- only the `(1-c)` uncoated
		// fraction shows the bare substrate's transmission and the `c`
		// fraction shows it attenuated by `K`.
		return fBaseT * ( K * cp.weight + RISEPel( 1, 1, 1 ) * ( Scalar(1) - cp.weight ) );
	}

	if( nv < NEARZERO ) {
		return RISEPel( 0, 0, 0 );
	}
	if( !PassesHorizonGate( v, r, n, ri ) ) {
		return RISEPel( 0, 0, 0 );
	}

	// DL-388: Oren-Nayar and GGX are evaluated in the coat's frame.
	// Lambertian (exact either way) and fabric / weave (DL-417) keep the
	// expression below, unchanged.
	if( substrateModel == eSubstrateRefractedCosine || substrateModel == eSubstrateRefractedLobe ) {
		return ValueRefracted( vLightIn, ri, onb, v, r );
	}

	const RISEPel fBase = pBase->value( vLightIn, ri );

	CoatParams cp;
	ResolveCoat( ri, Scalar(-1), cp );
	if( cp.weight <= 0 ) {
		return fBase;						// bare substrate, exactly
	}

	// --- coat lobe -------------------------------------------------
	// DL-192: the coat lobe's OWN frame -- ResolveCoatFrame returns
	// `onb` unchanged when no coat-normal painter is bound, so this is
	// a no-op for every pre-DL-192 material.  `coatNv`/`coatNr` are
	// deliberately SEPARATE from the substrate's `nv`/`nr` above (which
	// keep gating the horizon / Fresnel / recycling terms below,
	// unperturbed) -- CoatLobeValue's own guard handles either going
	// non-positive under a tilted coat normal.
	const OrthonormalBasis3D coatOnb = ResolveCoatFrame( ri, onb );
	const Scalar coatNv = Vector3Ops::Dot( coatOnb.w(), v );
	const Scalar coatNr = Vector3Ops::Dot( coatOnb.w(), r );
	const Scalar fCoat = CoatLobeValue( v, r, coatOnb, coatNv, coatNr, cp.alpha, cp.eta, cp.re );

	// --- substrate reached THROUGH the coat ------------------------
	// Interface transmittances at the two macro angles.  Reached only by
	// the Lambertian model (direction-independent, so the unrefracted
	// directions are as good as the refracted ones and this is exact) and
	// by fabric / weave, which keep the pre-DL-388 outer-frame
	// approximation (DL-417).  Oren-Nayar and GGX went to ValueRefracted
	// above -- see "SUBSTRATE IN THE COAT'S FRAME" for why the outer frame
	// is wrong for them.
	const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( nv, cp.eta );

	const RISEPel Ain  = CoatedLayer::PassTransmittanceRGB( nr, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
	const RISEPel Aout = CoatedLayer::PassTransmittanceRGB( nv, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );

	const RISEPel R   = SubstrateAlbedo( ri );		// VIEW-INDEPENDENT -- see SubstrateAlbedo
	// DL-342: the round trip's returned fraction is the EXACT
	// path-length average over the trapped diffuse field, not r_i times
	// one mean-cosine Beer factor -- see CoatedLayer::InteriorDiffuseTransport.
	const RISEPel rec = RecyclingFactorRGB( bRecycling, InteriorDiffuseRGB( cp ).returned, R );

	const RISEPel K = Ain * Aout * rec * ( Tin * Tout / ( cp.eta * cp.eta ) );

	// f = c * f_coat + ( c * K + (1 - c) ) * f_base
	return fBase * ( K * cp.weight + RISEPel( 1, 1, 1 ) * ( Scalar(1) - cp.weight ) )
	     + RISEPel( fCoat, fCoat, fCoat ) * cp.weight;
}

Scalar CoatedBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n = onb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nv = Vector3Ops::Dot( n, v );
	const Scalar nr = Vector3Ops::Dot( n, r );

	if( nr < NEARZERO ) {
		return 0;
	}

	if( nv < -NEARZERO && bBaseFullSphere )
	{
		// DL-23 -- TRANSMISSION.  See `value()` above; identical
		// structure, spectral path.
		const Scalar fBaseT = pBase->valueNM( vLightIn, ri, nm );

		CoatParams cp;
		ResolveCoat( ri, nm, cp );
		if( cp.weight <= 0 ) {
			return fBaseT;
		}

		const Scalar absNv = -nv;
		const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr,    cp.eta );
		const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( absNv, cp.eta );
		const Scalar Ain  = CoatedLayer::PassTransmittance( nr,    cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );
		const Scalar Aout = CoatedLayer::PassTransmittance( absNv, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );

		const Scalar R  = SubstrateAlbedoNM( ri, nm );
		const Scalar K  = Ain * Aout * RecyclingFactorNM( bRecycling, InteriorAt( cp, cp.tint[0] ).returned, R ) * ( Tin * Tout / ( cp.eta * cp.eta ) );

		return fBaseT * ( cp.weight * K + ( Scalar(1) - cp.weight ) );
	}

	if( nv < NEARZERO ) {
		return 0;
	}
	if( !PassesHorizonGate( v, r, n, ri ) ) {
		return 0;
	}

	// DL-388: see value().
	if( substrateModel == eSubstrateRefractedCosine || substrateModel == eSubstrateRefractedLobe ) {
		return ValueRefractedNM( vLightIn, ri, onb, v, r, nm );
	}

	const Scalar fBase = pBase->valueNM( vLightIn, ri, nm );

	CoatParams cp;
	ResolveCoat( ri, nm, cp );
	if( cp.weight <= 0 ) {
		return fBase;
	}

	// DL-192: see the identical block in value() above.
	const OrthonormalBasis3D coatOnb = ResolveCoatFrame( ri, onb );
	const Scalar coatNv = Vector3Ops::Dot( coatOnb.w(), v );
	const Scalar coatNr = Vector3Ops::Dot( coatOnb.w(), r );
	const Scalar fCoat = CoatLobeValue( v, r, coatOnb, coatNv, coatNr, cp.alpha, cp.eta, cp.re );

	const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( nv, cp.eta );
	const Scalar Ain  = CoatedLayer::PassTransmittance( nr, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );
	const Scalar Aout = CoatedLayer::PassTransmittance( nv, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );

	// THE per-wavelength wet darkening (7.1 second requirement): the
	// substrate's reflectance AT THIS WAVELENGTH drives the recycling
	// denominator, so channels where R(lambda) is high are amplified
	// more -- darkening and chroma boost from transport, not from a
	// fitted exponent.
	const Scalar R  = SubstrateAlbedoNM( ri, nm );
	const Scalar K  = Ain * Aout * RecyclingFactorNM( bRecycling, InteriorAt( cp, cp.tint[0] ).returned, R ) * ( Tin * Tout / ( cp.eta * cp.eta ) );

	return cp.weight * fCoat + ( cp.weight * K + ( Scalar(1) - cp.weight ) ) * fBase;
}

//////////////////////////////////////////////////////////////////////
// SubstrateAlbedo{,NM} -- the `R` in the recycling denominator
// 1/(1 - r_i * R).
//
// MUST be VIEW-INDEPENDENT, and that is worth stating loudly because
// getting it wrong is silent.  `R` is shared by both directions of the
// layer's response: it sits in a factor that multiplies f_base(wi,wo)
// symmetrically.  If `R` were a function of the view -- which
// `IBSDF::albedo` legitimately is, being the OIDN AOV -- then f(a->b)
// would carry R(b) while f(b->a) carried R(a), and the coated BRDF
// would be NON-RECIPROCAL.  That was a real defect in this file's
// first cut: ~28 % asymmetry at 80 deg on a clearcoat-over-PBR
// substrate, on exactly the NEE / BDPT-connection path Phase 2 exists
// to fix, and invisible to every consistency check that existed
// (SPFBSDFConsistencyTest Part D holds fine under a non-reciprocal f,
// because it compares the SPF against that same f).
// SPFBSDFConsistencyTest's reciprocity sweep (Part E) is the guard
// that CAN see it.
//
// So we read `IBSDF::hemisphericalAlbedo`, whose contract forbids
// touching `ri.ray`.  It is also the physically right quantity: the
// recycled field has been totally-internally-reflected at the coat's
// underside and arrives back at the substrate DIFFUSE, so what the
// substrate returns to it is its reflectance under a uniform field --
// including a glossy substrate's specular lobe at its hemispherical
// Fresnel average, not merely its diffuse lobe.
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// DL-388: the refracted-frame value (see "SUBSTRATE IN THE COAT'S
// FRAME").  f = c ( f_coat + T T a a / eta^2 [ f_b(wi', wo') + M ] )
//              + (1 - c) f_b(wi, wo)
// with M the lobe-reservoir kernel (GGX) or, for the cosine reservoir
// (Oren-Nayar), f_b(wi', wo') E_ret R / (1 - E_ret R) -- the pre-DL-388
// recycling factor applied to the refracted f_b.
//////////////////////////////////////////////////////////////////////
namespace
{
	//! M per RGB channel at the two internal cosines.
	RISE::RISEPel LobeRecycledRGB(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const Scalar muA,
		const Scalar muB )
	{
		using namespace RISE;
		Scalar alpha = 0;
		LobeChannel ch[3];
		ReadGGXLobeRGB( g, riIn, alpha, ch );
		RISEPel out( 0, 0, 0 );
		LobeBasis b;
		LobeDir dA = {}, dB = {};
		Scalar lastTau = -1;
		for( int c = 0; c < 3; ++c ) {
			const Scalar tau = ChannelTau( cp, cp.tint[c] );
			if( c == 0 || !( tau == lastTau ) ) {
				LobeBasisCached( b, alpha, cp.eta, tau );
				dA = LobeDirection( b, muA );
				dB = LobeDirection( b, muB );
				lastTau = tau;
			}
			const LobeTotals t = LobeHemispherical( b, ch[c] );
			out[c] = LobeKernel( t, LobeReturn( b, ch[c], dA ), LobeReturn( b, ch[c], dB ) );
		}
		return out;
	}

	Scalar LobeRecycledNM(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const Scalar muA,
		const Scalar muB,
		const Scalar nm )
	{
		Scalar alpha = 0;
		LobeChannel ch;
		ReadGGXLobeNM( g, riIn, nm, alpha, ch );
		LobeBasis b;
		LobeBasisCached( b, alpha, cp.eta, ChannelTau( cp, cp.tint[0] ) );
		const LobeTotals t = LobeHemispherical( b, ch );
		return LobeKernel( t, LobeReturn( b, ch, LobeDirection( b, muA ) ), LobeReturn( b, ch, LobeDirection( b, muB ) ) );
	}
}

namespace
{
	//! Directional (AOV) substrate albedo through the coat, per channel,
	//! for an internal view cosine: e_1 + g E / (1 - Q).
	RISE::RISEPel LobeAlbedoRGB(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const Scalar mu,
		const bool recycling,
		const bool whiteSky )
	{
		using namespace RISE;
		Scalar alpha = 0;
		LobeChannel ch[3];
		ReadGGXLobeRGB( g, riIn, alpha, ch );
		RISEPel out( 0, 0, 0 );
		LobeBasis b;
		LobeDir d = {};
		Scalar lastTau = -1;
		for( int c = 0; c < 3; ++c ) {
			const Scalar tau = ChannelTau( cp, cp.tint[c] );
			if( c == 0 || !( tau == lastTau ) ) {
				LobeBasisCached( b, alpha, cp.eta, tau );
				d = LobeDirection( b, mu );
				lastTau = tau;
			}
			const LobeTotals t = LobeHemispherical( b, ch[c] );
			if( whiteSky ) {
				// (1/pi) INT INT over both outer hemispheres, mapped inside:
				// eta^2 ( 2 INT mu Psi e_1 + G E / (1 - Q) ).
				out[c] = cp.eta * cp.eta * ( t.H + ( recycling ? t.G * LobeReservoirEscape( t ) : Scalar(0) ) );
			} else {
				out[c] = LobeEscape( b, ch[c], d ) + ( recycling ? LobeReturn( b, ch[c], d ) * LobeReservoirEscape( t ) : Scalar(0) );
			}
		}
		return out;
	}

	Scalar LobeAlbedoNM(
		const RISE::Implementation::GGXBRDF& g,
		const RISE::RayIntersectionGeometric& riIn,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const Scalar mu,
		const bool recycling,
		const bool whiteSky,
		const Scalar nm )
	{
		Scalar alpha = 0;
		LobeChannel ch;
		ReadGGXLobeNM( g, riIn, nm, alpha, ch );
		LobeBasis b;
		LobeBasisCached( b, alpha, cp.eta, ChannelTau( cp, cp.tint[0] ) );
		const LobeTotals t = LobeHemispherical( b, ch );
		if( whiteSky ) {
			return cp.eta * cp.eta * ( t.H + ( recycling ? t.G * LobeReservoirEscape( t ) : Scalar(0) ) );
		}
		const LobeDir d = LobeDirection( b, mu );
		return LobeEscape( b, ch, d ) + ( recycling ? LobeReturn( b, ch, d ) * LobeReservoirEscape( t ) : Scalar(0) );
	}

	//! The substrate record for a VIEW-INDEPENDENT query: the white-sky
	//! terms do not depend on the view and IBSDF::hemisphericalAlbedo
	//! may not read ri.ray, so the record looks along the normal.
	RISE::RayIntersectionGeometric WhiteSkyRecord(
		const RISE::RayIntersectionGeometric& ri,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp )
	{
		using namespace RISE;
		RayIntersectionGeometric out( ri );
		const Vector3 n = ri.onb.w();
		out.ray.Set( Point3Ops::mkPoint3( ri.ptIntersection, n ), -n );
		out.ambientIOR = cp.eta * cp.ambient;
		return out;
	}
}

RISEPel CoatedBRDF::ValueRefracted(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& onb,
	const Vector3& v,
	const Vector3& r
	) const
{
	CoatParams cp;
	ResolveCoat( ri, Scalar(-1), cp );
	if( cp.weight <= 0 ) {
		return pBase->value( vLightIn, ri );			// bare substrate, exactly
	}

	const Vector3 n  = onb.w();
	const Scalar  nv = Vector3Ops::Dot( n, v );
	const Scalar  nr = Vector3Ops::Dot( n, r );

	// --- coat lobe (unchanged; see value()) ------------------------
	const OrthonormalBasis3D coatOnb = ResolveCoatFrame( ri, onb );
	const Scalar fCoat = CoatLobeValue( v, r, coatOnb, Vector3Ops::Dot( coatOnb.w(), v ), Vector3Ops::Dot( coatOnb.w(), r ), cp.alpha, cp.eta, cp.re );

	// --- substrate in the coat's frame ------------------------------
	const Scalar  Tin  = Scalar(1) - CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar  Tout = Scalar(1) - CoatedLayer::Fresnel( nv, cp.eta );
	const RISEPel Ain  = CoatedLayer::PassTransmittanceRGB( nr, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
	const RISEPel Aout = CoatedLayer::PassTransmittanceRGB( nv, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );

	const RayIntersectionGeometric riIn = MakeSubstrateRecord( ri, onb, cp );
	const Vector3 vIn = CoatedLayer::RefractIntoCoat( v, n, cp.eta );
	const RISEPel fIn = pBase->value( vIn, riIn );

	RISEPel sub;
	if( substrateModel == eSubstrateRefractedLobe ) {
		sub = fIn;
		if( bRecycling ) {
			const Scalar muL = Vector3Ops::Dot( vIn, n );
			const Scalar muV = -Vector3Ops::Dot( riIn.ray.Dir(), n );
			sub = sub + LobeRecycledRGB( *pGGXBase, riIn, cp, muL, muV );
		}
	} else {
		const RISEPel R = SubstrateAlbedo( ri );
		sub = fIn * RecyclingFactorRGB( bRecycling, InteriorDiffuseRGB( cp ).returned, R );
	}

	const RISEPel K0 = Ain * Aout * ( Tin * Tout / ( cp.eta * cp.eta ) );
	RISEPel result = ( RISEPel( fCoat, fCoat, fCoat ) + K0 * sub ) * cp.weight;
	if( cp.weight < Scalar(1) ) {
		result = result + pBase->value( vLightIn, ri ) * ( Scalar(1) - cp.weight );
	}
	return result;
}

Scalar CoatedBRDF::ValueRefractedNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const OrthonormalBasis3D& onb,
	const Vector3& v,
	const Vector3& r,
	const Scalar nm
	) const
{
	CoatParams cp;
	ResolveCoat( ri, nm, cp );
	if( cp.weight <= 0 ) {
		return pBase->valueNM( vLightIn, ri, nm );
	}

	const Vector3 n  = onb.w();
	const Scalar  nv = Vector3Ops::Dot( n, v );
	const Scalar  nr = Vector3Ops::Dot( n, r );

	const OrthonormalBasis3D coatOnb = ResolveCoatFrame( ri, onb );
	const Scalar fCoat = CoatLobeValue( v, r, coatOnb, Vector3Ops::Dot( coatOnb.w(), v ), Vector3Ops::Dot( coatOnb.w(), r ), cp.alpha, cp.eta, cp.re );

	const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( nv, cp.eta );
	const Scalar Ain  = CoatedLayer::PassTransmittance( nr, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );
	const Scalar Aout = CoatedLayer::PassTransmittance( nv, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );

	const RayIntersectionGeometric riIn = MakeSubstrateRecord( ri, onb, cp );
	const Vector3 vIn = CoatedLayer::RefractIntoCoat( v, n, cp.eta );
	const Scalar  fIn = pBase->valueNM( vIn, riIn, nm );

	Scalar sub;
	if( substrateModel == eSubstrateRefractedLobe ) {
		sub = fIn;
		if( bRecycling ) {
			const Scalar muL = Vector3Ops::Dot( vIn, n );
			const Scalar muV = -Vector3Ops::Dot( riIn.ray.Dir(), n );
			sub += LobeRecycledNM( *pGGXBase, riIn, cp, muL, muV, nm );
		}
	} else {
		const Scalar R = SubstrateAlbedoNM( ri, nm );
		sub = fIn * RecyclingFactorNM( bRecycling, InteriorAt( cp, cp.tint[0] ).returned, R );
	}

	const Scalar K0 = Ain * Aout * ( Tin * Tout / ( cp.eta * cp.eta ) );
	Scalar result = cp.weight * ( fCoat + K0 * sub );
	if( cp.weight < Scalar(1) ) {
		result += ( Scalar(1) - cp.weight ) * pBase->valueNM( vLightIn, ri, nm );
	}
	return result;
}

RISEPel CoatedBRDF::SubstrateAlbedo( const RayIntersectionGeometric& ri ) const
{
	RISEPel R;
	if( pBase->hemisphericalAlbedo( ri, R ) ) {
		return R;
	}
	// Unreachable via the shipping paths: CoatedMaterial's allowlist is
	// enforced at BOTH parse time (Job::AddCoatedMaterial) and
	// construction time (RISE_API_CreateCoatedMaterial), and every
	// allowlisted BSDF implements this.  Falling back to ZERO rather
	// than to the view-dependent `albedo()` is deliberate: zero simply
	// switches the recycling off (the layer degrades to
	// Weidlich-Wilkie single bounce, which is merely dimmer), whereas
	// `albedo()` would reintroduce the non-reciprocity this whole
	// method exists to avoid.  Dimmer is a better failure than wrong.
	return RISEPel( 0, 0, 0 );
}

Scalar CoatedBRDF::SubstrateAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar R = 0;
	if( pBase->hemisphericalAlbedoNM( ri, nm, R ) ) {
		return R;
	}
	return 0;		// see SubstrateAlbedo above
}

RISEPel CoatedBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n = onb.w();
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar  nr = r_min( r_max( Vector3Ops::Dot( n, r ), Scalar(0) ), Scalar(1) );

	// The SUBSTRATE term is the view-independent one even here, so this
	// AOV stays a faithful hemispherical summary of `value` (they must
	// agree on the layer model, or the denoiser's guide buffer would
	// describe a surface the renderer is not shading).  The view
	// dependence an AOV legitimately wants -- a coated surface really
	// does read brighter at grazing -- enters through the COAT's own
	// F(cos_o) and A(cos_o) below, which is where it belongs.
	const RISEPel R = SubstrateAlbedo( ri );

	CoatParams cp;
	ResolveCoat( ri, Scalar(-1), cp );
	if( cp.weight <= 0 ) {
		return R;
	}

	// Hemispherical form of the same layer model: the coat returns
	// F(cos_o); the rest enters through ONE traversal at the view's
	// refracted angle, A(cos_o), and what comes back out is
	// R * escape / (1 - E_ret R) -- `escape` the trapped diffuse field's
	// exit fraction, (1 - r_i) for a clear coat and its path-length
	// average through an absorbing one (DL-342; the pre-DL-342 form
	// charged the exit a second traversal at the VIEW's angle, which
	// the diffuse field does not take).  At R = 1, A = 1 this is
	// F + (1 - F) = 1 exactly, so the AOV stays in [0,1] as
	// IBSDF::albedo requires.
	const Scalar  F = CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar  T = Scalar(1) - F;
	const RISEPel A = CoatedLayer::PassTransmittanceRGB( nr, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );

	RISEPel sub;
	if( substrateModel == eSubstrateRefractedLobe ) {
		// DL-388: the lobe reservoir's own directional albedo at the
		// view's internal cosine, e_1 + g E / (1 - Q) -- the energy
		// value()'s substrate term returns from this view.
		const RayIntersectionGeometric riIn = MakeSubstrateRecord( ri, onb, cp );
		sub = A * LobeAlbedoRGB( *pGGXBase, riIn, cp, -Vector3Ops::Dot( riIn.ray.Dir(), n ), bRecycling, false ) * T;
	} else {
		const InteriorRGB in = InteriorDiffuseRGB( cp );
		const RISEPel rec = RecyclingFactorRGB( bRecycling, in.returned, R );
		sub = A * R * rec * ( in.escape * T );
	}
	const RISEPel coated = RISEPel( F, F, F ) + sub;

	const RISEPel result = coated * cp.weight + R * ( Scalar(1) - cp.weight );
	return RISEPel(
		r_min( r_max( result[0], Scalar(0) ), Scalar(1) ),
		r_min( r_max( result[1], Scalar(0) ), Scalar(1) ),
		r_min( r_max( result[2], Scalar(0) ), Scalar(1) ) );
}

//////////////////////////////////////////////////////////////////////
// hemisphericalAlbedo{,NM} -- IBSDF's VIEW-INDEPENDENT reflectance for
// the coated stack itself.
//
// Same layer algebra as `albedo` above, with the coat's own Fresnel
// replaced by its HEMISPHERICAL AVERAGE r_e and the entry traversal
// replaced by its white-sky average (CoatedLayer::InteriorDiffuse::entry,
// DL-342 -- it used to be one Beer factor at an outer cosine of 0.5),
// so nothing here reads `ri.ray` -- the contract IBSDF.h states.
//
// `coated_material` refuses a coated substrate (the allowlist admits
// only lambertian / orennayar / ggx), so nothing in tree consumes this
// today.  It is implemented anyway because CoatedBRDF is an IBSDF and
// leaving a view-independent-reflectance query unanswered on a
// material whose whole subject IS layered reflectance would be an odd
// gap -- and because the day the allowlist grows, the alternative is
// the FALSE path, which silently switches the recycling off.
//////////////////////////////////////////////////////////////////////
bool CoatedBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	const RISEPel R = SubstrateAlbedo( ri );

	CoatParams cp;
	ResolveCoat( ri, Scalar(-1), cp );
	if( cp.weight <= 0 ) {
		out = R;
		return true;
	}

	// White-sky form (DL-342): the coat returns r_e; `entry` of the
	// outside field reaches the substrate, `escape` of what the
	// substrate returns leaves, and the round trip returns E_ret --
	// all three exact path-length averages through an absorbing coat,
	// and (1 - r_e), (1 - r_i), r_i for a clear one.
	const Scalar  F = cp.re;						// hemispherical, not F(cos_o)
	RISEPel sub;
	if( substrateModel == eSubstrateRefractedLobe ) {
		// DL-388: eta^2 ( 2 INT mu Psi e_1 + G E / (1 - Q) ) -- the same
		// lobe reservoir value() evaluates, integrated over both outer
		// hemispheres.  For a Lambertian it is R entry escape / (1 - E_ret R).
		sub = LobeAlbedoRGB( *pGGXBase, WhiteSkyRecord( ri, cp ), cp, Scalar(1), bRecycling, true );
	} else {
		const InteriorRGB in = InteriorDiffuseRGB( cp );
		const RISEPel rec = RecyclingFactorRGB( bRecycling, in.returned, R );
		sub = R * rec * ( in.entry * in.escape );
	}
	const RISEPel result = ( RISEPel( F, F, F ) + sub ) * cp.weight + R * ( Scalar(1) - cp.weight );

	out = RISEPel(
		r_min( r_max( result[0], Scalar(0) ), Scalar(1) ),
		r_min( r_max( result[1], Scalar(0) ), Scalar(1) ),
		r_min( r_max( result[2], Scalar(0) ), Scalar(1) ) );
	return true;
}

bool CoatedBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	const Scalar R = SubstrateAlbedoNM( ri, nm );

	CoatParams cp;
	ResolveCoat( ri, nm, cp );
	if( cp.weight <= 0 ) {
		out = R;
		return true;
	}

	const Scalar F = cp.re;
	Scalar sub;
	if( substrateModel == eSubstrateRefractedLobe ) {
		sub = LobeAlbedoNM( *pGGXBase, WhiteSkyRecord( ri, cp ), cp, Scalar(1), bRecycling, true, nm );		// DL-388: see hemisphericalAlbedo
	} else {
		const CoatedLayer::InteriorDiffuse in = InteriorAt( cp, cp.tint[0] );
		sub = R * RecyclingFactorNM( bRecycling, in.returned, R ) * ( in.entry * in.escape );
	}
	const Scalar result = cp.weight * ( F + sub ) + ( Scalar(1) - cp.weight ) * R;
	out = r_min( r_max( result, Scalar(0) ), Scalar(1) );
	return true;
}
