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
	// Same predicate as GuardedGetColorNM (IPainter.h IsUntintedWhitePainter:
	// min channel >= 1 - 1e-6, and never for a physical spectrum, DL-396).
	out.tinted = !IsUntintedWhitePainter( *pCoatTint, tintRGB );

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
//     (E = 1 - Q there, and g = rho - escape = 1 - e_1) -- PROVIDED the
//     tabulated g agrees with the exact single bounce's escape e_1 at the
//     same direction; near the critical cosine that needs the critical
//     patch below, and a substrate brighter than physical (E + Q > 1)
//     is capped by LobeReservoirEscape;
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
// The furnace of a lossless white metal reads 0.99-1.01 at every
// incidence, coat index (1.005-3) and roughness (0.002-0.6) measured
// (the substrate's own GGX directional albedo is 1.003 at normal
// incidence; the rest is the tables' residual).
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

	//! View-cosine nodes: (j + 1) / N, so the last node is normal incidence
	//! itself.  Every view a refracted direction can take lies in
	//! [sqrt(1 - 1/eta^2), 1], and a node grid that stopped half a bin
	//! short of 1 read the normal-incidence lobe from a view tilted 10 deg
	//! (its spill toward the critical angle 1.7 % too high).
	inline Scalar LobeViewNode( const int j ) { return Scalar( j + 1 ) / Scalar(kLobeN); }

	//! The CRITICAL PATCH.  For a narrow lobe the return g(mu) follows the
	//! coat's own weight Phi(mu) -- which steps from total internal
	//! reflection to the external Fresnel at the critical cosine mu_c with
	//! a square-root edge -- and a linear interpolation between view nodes
	//! 1/32 apart smears that step over a whole node interval: the
	//! single-bounce escape (the exact lobe) plus the interpolated return
	//! then exceeded the lobe's albedo, and a lossless white metal of
	//! alpha <= 0.005 under a 1.5 coat read 1.09 at 75 deg (DL-423).  So
	//! each basis also carries g and e on kPatchN nodes over [mu_c,
	//! mu_c + W], clustered quadratically at mu_c, built by DISPLACEMENT
	//! interpolation of the spill table (each node's histogram shifted
	//! with the view -- a reflected lobe moves one-for-one with its view
	//! cosine near the mirror -- so the step lands where it is, not where
	//! a linear blend of two nodes' steps puts it).  Anchored to mu_c, so
	//! the clear table's eta blend (uniform in mu_c) blends aligned steps,
	//! and every hemispherical integral reads g / e through the same
	//! lookup (G must normalise exactly what value() out-couples).
	constexpr int    kPatchN = 17;
	constexpr Scalar kPatchW = Scalar(2) / Scalar(kLobeN);
	//! The patch's displacement reads the spill histogram on sub-bins
	//! 1/256 wide (a bin's centroid and spread cannot describe a GGX lobe
	//! -- a narrow core with long tails -- against a square-root edge).
	//! Only substrates narrower than kPatchAlphaMax get a patch; broader
	//! lobes are smooth on the node grid and their patch IS the node
	//! interpolation.
	constexpr int    kLobeSubBins    = kLobeN * 8;
	constexpr Scalar kPatchAlphaMax  = Scalar(0.25);

	struct LobeSpillTable
	{
		std::vector<Scalar> t0;		//!< F = 1 (the sum of the bin's sub-bins)
		std::vector<Scalar> t5;		//!< F = (1 - w.m)^5
		std::vector<Scalar> tm;		//!< F = 1, times mu_u (the bin's centroid is tm / t0)
		std::vector<Scalar> t2;		//!< F = 1, times mu_u^2 (the bin's spread is t2 / t0 - centroid^2)
		std::vector<Scalar> s0;		//!< F = 1 on kLobeSubBins sub-bins per bin
		std::vector<Scalar> s5;		//!< F = (1 - w.m)^5 on the sub-bins
		std::vector<Scalar> sm;		//!< F = 1, times mu_u, on the sub-bins (centroid sm / s0)
		std::vector<Scalar> s2;		//!< F = 1, times mu_u^2, on the sub-bins (spread s2/s0 - centroid^2)
	};

	//! Deposits a mass spread UNIFORMLY over [lo, hi] (in the outgoing
	//! cosine) into the sub-bins, with its first and second moments.
	inline void DepositSpill( Scalar* sub0, Scalar* sub5, Scalar* subm, Scalar* sub2,
		const Scalar lo, const Scalar hi, const Scalar centre, const Scalar wt, const Scalar wt5 )
	{
		const Scalar len = hi - lo;
		if( !( len > Scalar(1e-12) ) ) {
			const int ks = r_min( kLobeSubBins - 1, r_max( 0, (int)( centre * Scalar(kLobeSubBins) ) ) );
			sub0[ks] += wt;  sub5[ks] += wt5;
			subm[ks] += wt * centre;  sub2[ks] += wt * centre * centre;
			return;
		}
		const int k0 = r_min( kLobeSubBins - 1, r_max( 0, (int)( lo * Scalar(kLobeSubBins) ) ) );
		const int k1 = r_min( kLobeSubBins - 1, r_max( 0, (int)( hi * Scalar(kLobeSubBins) ) ) );
		for( int ks = k0; ks <= k1; ++ks ) {
			const Scalar a = r_max( lo, Scalar(ks) / Scalar(kLobeSubBins) );
			const Scalar b = ( ks == kLobeSubBins - 1 ) ? hi : r_min( hi, Scalar(ks + 1) / Scalar(kLobeSubBins) );
			if( !( b > a ) ) continue;
			const Scalar share = ( b - a ) / len;
			sub0[ks] += wt * share;  sub5[ks] += wt5 * share;
			subm[ks] += wt * share * Scalar(0.5) * ( a + b );
			sub2[ks] += wt * share * ( a * a + a * b + b * b ) / Scalar(3);
		}
	}

	//! DL-426: each VNDF stratum's mass is spread over its own IMAGE in
	//! the outgoing cosine (a uniform centred on the stratum's sample with
	//! the linearised spread of its corners), not deposited as a point.  The 32 x 32 strata are a deterministic quadrature: at a
	//! normal view every azimuth stratum of one radial ring reflects to
	//! the SAME mu_u, so the table held point masses of up to 1/32 of the
	//! lobe in its tail -- and a point mass crossing the critical cosine's
	//! square-root edge as eta moves makes the priced return jump (22 %
	//! of the recycled term over delta-eta 1e-5 at alpha 0.1, eta 1.01415).
	//! The true spill is a density; this is its piecewise-uniform model.
	LobeSpillTable BuildLobeSpillTable()
	{
		using namespace RISE;
		LobeSpillTable T;
		T.t0.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.t5.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.tm.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.t2.assign( (std::size_t)kLobeA * kLobeN * kLobeN, Scalar(0) );
		T.s0.assign( (std::size_t)kLobeA * kLobeN * kLobeSubBins, Scalar(0) );
		T.s5.assign( (std::size_t)kLobeA * kLobeN * kLobeSubBins, Scalar(0) );
		T.sm.assign( (std::size_t)kLobeA * kLobeN * kLobeSubBins, Scalar(0) );
		T.s2.assign( (std::size_t)kLobeA * kLobeN * kLobeSubBins, Scalar(0) );
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );
		const Scalar invS2 = Scalar(1) / Scalar( kLobeS * kLobeS );
		std::vector<Scalar> cz( (std::size_t)kLobeS * kLobeS ), cw( cz.size() ), cw5( cz.size() );
		for( int a = 0; a < kLobeA; ++a ) {
			const Scalar alpha = LobeAlphaNode( a );
			for( int j = 0; j < kLobeN; ++j ) {
				const Scalar mu = LobeViewNode( j );
				const Scalar s  = sqrt( r_max( Scalar(0), Scalar(1) - mu * mu ) );
				const Vector3 w = Vector3Ops::Normalize( onb.u() * s + onb.w() * mu );
				const Vector3 wl( Vector3Ops::Dot( w, onb.u() ), Vector3Ops::Dot( w, onb.v() ), Vector3Ops::Dot( w, onb.w() ) );
				const Scalar G1 = MicrofacetUtils::GGX_G1_Aniso( alpha, alpha, wl );
				Scalar* row0 = &T.t0[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* row5 = &T.t5[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* rowm = &T.tm[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* row2 = &T.t2[ ( (std::size_t)a * kLobeN + j ) * kLobeN ];
				Scalar* sub0 = &T.s0[ ( (std::size_t)a * kLobeN + j ) * kLobeSubBins ];
				Scalar* sub5 = &T.s5[ ( (std::size_t)a * kLobeN + j ) * kLobeSubBins ];
				Scalar* subm = &T.sm[ ( (std::size_t)a * kLobeN + j ) * kLobeSubBins ];
				Scalar* sub2 = &T.s2[ ( (std::size_t)a * kLobeN + j ) * kLobeSubBins ];
				// Pass 1: every stratum's sample (the midpoint rule).
				for( int s1 = 0; s1 < kLobeS; ++s1 ) {
					for( int s2 = 0; s2 < kLobeS; ++s2 ) {
						const int id = s1 * kLobeS + s2;
						cz[id] = -1;  cw[id] = cw5[id] = 0;
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
						const Scalar om = Scalar(1) - wm;
						const Scalar om2 = om * om;
						cz[id]  = r_min( uz, Scalar(1) );
						cw[id]  = ( G1 > Scalar(0) ) ? G2 / G1 * invS2 : Scalar(0);
						cw5[id] = cw[id] * om2 * om2 * om;
					}
				}
				// Pass 2: deposit each as a uniform CENTRED on its sample with
				// the stratum's linearised spread -- a uniform (u1, u2) square
				// maps to mu_u with variance (d1^2 + d2^2) / 12, d1 / d2 the
				// change of mu_u across one stratum in each direction, taken
				// from the neighbouring strata's samples.  In
				// MicrofacetUtils::VNDF_Sample_Aniso u1 is the AZIMUTH
				// (phi = 2 pi u1, periodic: central differences wrap) and u2
				// the POLAR coordinate of the cap (pole at u2 = 0, rim at
				// u2 = 1: one-sided at the two ends, never wrapped).
				// Centring keeps the quadrature's first moment (the midpoint
				// rule); differencing samples, not corners, keeps clear of the
				// cap's rim at u2 -> 1, where a corner maps to a grazing
				// microfacet.
				auto czAt = [&]( const int s1, const int s2 ) { return cz[ ( ( s1 + kLobeS ) % kLobeS ) * kLobeS + s2 ]; };
				for( int s1 = 0; s1 < kLobeS; ++s1 ) {
					for( int s2 = 0; s2 < kLobeS; ++s2 ) {
						const int id = s1 * kLobeS + s2;
						if( !( cz[id] > Scalar(0) ) ) continue;
						const Scalar uc = cz[id];
						Scalar d1 = 0;
						{
							const Scalar zm = czAt( s1 - 1, s2 );
							const Scalar zp = czAt( s1 + 1, s2 );
							if( zm > Scalar(0) && zp > Scalar(0) ) d1 = Scalar(0.5) * ( zp - zm );
							else if( zp > Scalar(0) ) d1 = zp - uc;
							else if( zm > Scalar(0) ) d1 = uc - zm;
						}
						Scalar d2 = 0;
						{
							const Scalar zm = ( s2 > 0 ) ? czAt( s1, s2 - 1 ) : Scalar(-1);
							const Scalar zp = ( s2 + 1 < kLobeS ) ? czAt( s1, s2 + 1 ) : Scalar(-1);
							if( zm > Scalar(0) && zp > Scalar(0) ) d2 = Scalar(0.5) * ( zp - zm );
							else if( zp > Scalar(0) ) d2 = zp - uc;
							else if( zm > Scalar(0) ) d2 = uc - zm;
						}
						const Scalar hw = Scalar(0.5) * sqrt( d1 * d1 + d2 * d2 );
						DepositSpill( sub0, sub5, subm, sub2,
							r_max( uc - hw, Scalar(0) ), r_min( uc + hw, Scalar(1) ), uc, cw[id], cw5[id] );
					}
				}
				for( int k = 0; k < kLobeN; ++k ) {
					for( int q = 0; q < kLobeSubBins / kLobeN; ++q ) {
						row0[k] += sub0[ k * ( kLobeSubBins / kLobeN ) + q ];
						row5[k] += sub5[ k * ( kLobeSubBins / kLobeN ) + q ];
						rowm[k] += subm[ k * ( kLobeSubBins / kLobeN ) + q ];
						row2[k] += sub2[ k * ( kLobeSubBins / kLobeN ) + q ];
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
					for( int k = 0; k < kLobeN; ++k ) { row0[k] *= scale; row5[k] *= scale; rowm[k] *= scale; row2[k] *= scale; }
					for( int k = 0; k < kLobeSubBins; ++k ) { sub0[k] *= scale; sub5[k] *= scale; subm[k] *= scale; sub2[k] *= scale; }
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
		Scalar g0[kLobeN], g5[kLobeN];					//!< spec return at view node j, per Fresnel basis
		Scalar e0[kLobeN], e5[kLobeN];					//!< spec escape at view node j
		Scalar Q0, Q5, E0, E5, G0, G5, H0, H5;			//!< spec hemispherical integrals
		Scalar JAphi, JApsi;							//!< 2 INT (1-(1-u)^5) u {Phi,Psi} du  (diffuse shape)
		Scalar JMphi, JMpsi;							//!< 2 INT (1-Ess(u)) u {Phi,Psi} du   (multiscatter shape)
		Scalar Eavg;
		Scalar muC;										//!< the critical cosine the patch is anchored to
		Scalar pg0[kPatchN], pg5[kPatchN];				//!< g on the critical patch (LobePatchNode)
		Scalar pe0[kPatchN], pe5[kPatchN];				//!< escape on the critical patch
	};

	//! Node p of the critical patch: mu_c + W s^2, s = p / (N - 1).
	inline Scalar LobePatchNode( const Scalar muC, const int p )
	{
		const Scalar s = Scalar(p) / Scalar(kPatchN - 1);
		return muC + kPatchW * s * s;
	}

	//! g / e at view cosine mu: the critical patch on [mu_c, mu_c + W),
	//! the view nodes elsewhere.
	//! The view-node arrays at mu, linearly EXTRAPOLATED past normal
	//! incidence (only the patch's far end, mu_c + W, can lie there).
	inline Scalar LobeNodeLerp( const Scalar* arr, const Scalar mu )
	{
		Scalar xi = mu * Scalar(kLobeN) - Scalar(1);
		xi = r_max( xi, Scalar(0) );
		const int    i = r_min( (int)xi, kLobeN - 2 );
		const Scalar f = xi - Scalar(i);
		return arr[i] + ( arr[i + 1] - arr[i] ) * f;
	}

	inline Scalar LobeArrLookup( const Scalar* node, const Scalar* patch, const Scalar muC, const Scalar mu )
	{
		if( mu >= muC && mu < muC + kPatchW ) {
			const Scalar xs = sqrt( ( mu - muC ) / kPatchW ) * Scalar(kPatchN - 1);
			const int    k  = r_min( (int)xs, kPatchN - 2 );
			const Scalar m0 = LobePatchNode( muC, k ), m1 = LobePatchNode( muC, k + 1 );
			const Scalar g  = ( m1 > m0 ) ? ( mu - m0 ) / ( m1 - m0 ) : Scalar(0);
			// DL-426: the last patch cell ends on the NODE interpolant's own
			// value at mu_c + W, so the hand-off to the node arrays is
			// continuous for every basis -- a direct build's patch is joined
			// there already (the same number), the clear table's index-wise
			// eta blend of patches anchored to different mu_c is not.
			const Scalar p1 = ( k + 1 == kPatchN - 1 ) ? LobeNodeLerp( node, m1 ) : patch[k + 1];
			return patch[k] + ( p1 - patch[k] ) * g;
		}
		Scalar x = mu * Scalar(kLobeN) - Scalar(1);
		x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeN - 1) );
		const int    i = r_min( (int)x, kLobeN - 2 );
		const Scalar f = x - Scalar(i);
		return node[i] + ( node[i + 1] - node[i] ) * f;
	}

	//! DL-426: the coat's weights Phi(mu) = F_in(mu) a(mu)^2 and
	//! Psi(mu) = (1 - F_in(mu)) a(mu) as ONE continuous tabulated function
	//! of the internal cosine, accurate at the critical cosine for every
	//! eta.  Below mu_c the coat totally reflects (F_in = 1, Psi = 0, Phi =
	//! a^2: smooth in mu); above it F_in is the external Fresnel at the
	//! OUTER cosine t = sqrt(1 - eta^2 (1 - mu^2)), smooth in t -- the
	//! square-root edge at mu_c is the change of variables mu -> t, not a
	//! property of the weights.  So the table is two grids that meet at
	//! mu_c (where both read Phi = a^2, Psi = 0): uniform in mu on [0,
	//! mu_c] and uniform in t on [mu_c, 1].  It replaces a single grid
	//! uniform in mu that smeared the edge over one cell, patched by exact
	//! evaluation inside fixed distances of mu_c -- rules that switched as
	//! eta moved mu_c past them, which made the basis (and value())
	//! DISCONTINUOUS in eta (22 % in the recycled term over delta-eta
	//! 1e-5 at alpha 0.1, eta 1.01415).
	struct CoatWeightTable
	{
		static constexpr int kA = 128;		//!< nodes uniform in mu on [0, mu_c]
		static constexpr int kB = 192;		//!< nodes uniform in the outer cosine t on [mu_c, 1]
		static constexpr int kL = 256;		//!< cell-finding accelerator cells over [mu_c, 1]
		Scalar eta, muC, invStepA, invStepL;
		// Node positions (in mu), weights, slopes per unit mu, and the
		// cumulative integrals INT_0^mu {Phi, Psi} dmu of the
		// piecewise-linear-in-mu interpolant through the nodes, at the nodes.
		Scalar muA[kA], phA[kA], sphA[kA], cphA[kA];
		Scalar muB[kB], phB[kB], psB[kB], sphB[kB], spsB[kB], cphB[kB], cpsB[kB];
		int    lutB[kL + 1];

		static Scalar Atten( const Scalar mu, const Scalar tau, const bool opaque )
		{
			if( opaque ) return Scalar(0);
			if( !( tau > Scalar(0) ) ) return Scalar(1);
			return ( mu > Scalar(0) ) ? exp( -tau / mu ) : Scalar(0);
		}

		void Build( const Scalar eta_, const Scalar tau, const bool opaque )
		{
			using namespace RISE::Implementation;
			eta = eta_;
			muC = ( eta > Scalar(1) ) ? sqrt( Scalar(1) - Scalar(1) / ( eta * eta ) ) : Scalar(0);
			invStepA = ( muC > Scalar(0) ) ? Scalar(kA - 1) / muC : Scalar(0);
			for( int i = 0; i < kA; ++i ) {
				muA[i] = muC * Scalar(i) / Scalar(kA - 1);
				const Scalar a = Atten( muA[i], tau, opaque );
				phA[i] = a * a;
			}
			cphA[0] = 0;
			for( int i = 0; i + 1 < kA; ++i ) {
				const Scalar h = muA[i + 1] - muA[i];
				sphA[i] = ( h > Scalar(0) ) ? ( phA[i + 1] - phA[i] ) / h : Scalar(0);
				cphA[i + 1] = cphA[i] + Scalar(0.5) * ( phA[i] + phA[i + 1] ) * h;
			}
			sphA[kA - 1] = 0;
			const Scalar e2 = eta * eta;
			for( int i = 0; i < kB; ++i ) {
				const Scalar t = Scalar(i) / Scalar(kB - 1);
				muB[i] = ( i == 0 ) ? muC : ( ( i == kB - 1 ) ? Scalar(1) : sqrt( r_max( Scalar(0), t * t + e2 - Scalar(1) ) ) / eta );
				const Scalar F = ( eta > Scalar(1) ) ? CoatedLayer::Fresnel( t, eta ) : Scalar(0);
				const Scalar a = Atten( muB[i], tau, opaque );
				phB[i] = F * a * a;
				psB[i] = ( Scalar(1) - F ) * a;
			}
			cphB[0] = cphA[kA - 1];
			cpsB[0] = 0;
			for( int i = 0; i + 1 < kB; ++i ) {
				const Scalar h = muB[i + 1] - muB[i];
				sphB[i] = ( h > Scalar(0) ) ? ( phB[i + 1] - phB[i] ) / h : Scalar(0);
				spsB[i] = ( h > Scalar(0) ) ? ( psB[i + 1] - psB[i] ) / h : Scalar(0);
				cphB[i + 1] = cphB[i] + Scalar(0.5) * ( phB[i] + phB[i + 1] ) * h;
				cpsB[i + 1] = cpsB[i] + Scalar(0.5) * ( psB[i] + psB[i + 1] ) * h;
			}
			sphB[kB - 1] = spsB[kB - 1] = 0;
			// lutB[c]: the B cell holding the left edge of accelerator cell c.
			invStepL = ( Scalar(1) - muC > Scalar(0) ) ? Scalar(kL) / ( Scalar(1) - muC ) : Scalar(0);
			int i = 0;
			for( int c = 0; c <= kL; ++c ) {
				const Scalar m = muC + ( Scalar(1) - muC ) * Scalar(c) / Scalar(kL);
				while( i < kB - 2 && muB[i + 1] <= m ) ++i;
				lutB[c] = i;
			}
		}

		//! The cell holding mu in [0, 1]: segment A (i into muA) or B.
		inline void Cell( const Scalar mu, bool& inA, int& i ) const
		{
			if( mu < muC ) {
				inA = true;
				i = r_min( r_max( (int)( mu * invStepA ), 0 ), kA - 2 );
				return;
			}
			inA = false;
			const int c = r_min( r_max( (int)( ( mu - muC ) * invStepL ), 0 ), kL );
			i = lutB[c];
			while( i < kB - 2 && mu >= muB[i + 1] ) ++i;
		}

		inline void LerpInCell( const bool inA, const int i, const Scalar mu, Scalar& ph, Scalar& pv ) const
		{
			if( inA ) {
				ph = phA[i] + sphA[i] * ( mu - muA[i] );
				pv = 0;
			} else {
				const Scalar x = mu - muB[i];
				ph = phB[i] + sphB[i] * x;
				pv = psB[i] + spsB[i] * x;
			}
		}

		inline void Lookup( Scalar mu, Scalar& ph, Scalar& pv ) const
		{
			mu = r_min( r_max( mu, Scalar(0) ), Scalar(1) );
			bool inA; int i;
			Cell( mu, inA, i );
			LerpInCell( inA, i, mu, ph, pv );
		}

		//! INT_0^mu {Phi, Psi} dmu of the same interpolant, mu in cell (inA, i).
		inline void CumulativeInCell( const bool inA, const int i, const Scalar mu, Scalar& cph, Scalar& cps ) const
		{
			if( inA ) {
				const Scalar x = mu - muA[i];
				cph = cphA[i] + x * ( phA[i] + Scalar(0.5) * sphA[i] * x );
				cps = 0;
			} else {
				const Scalar x = mu - muB[i];
				cph = cphB[i] + x * ( phB[i] + Scalar(0.5) * sphB[i] * x );
				cps = cpsB[i] + x * ( psB[i] + Scalar(0.5) * spsB[i] * x );
			}
		}

		//! The weights averaged over a mass spread uniformly on [lo, hi]
		//! (clipped to [0, 1]): the exact average of the piecewise-linear
		//! interpolant, through its cumulative integral.  Continuous in
		//! lo, hi and eta; a vanishing range reads the point.  (A range
		//! inside one cell averages a LINEAR function, so its midpoint
		//! value is the same number, read directly.)
		inline void Window( Scalar lo, Scalar hi, Scalar& ph, Scalar& pv ) const
		{
			lo = r_max( lo, Scalar(0) );
			hi = r_min( hi, Scalar(1) );
			if( !( hi - lo > Scalar(1e-9) ) ) {
				Lookup( Scalar(0.5) * ( lo + hi ), ph, pv );
				return;
			}
			bool inA0; int i0;
			Cell( lo, inA0, i0 );
			const Scalar top = inA0 ? muA[i0 + 1] : muB[i0 + 1];
			if( hi <= top && ( !inA0 || hi < muC ) ) {
				LerpInCell( inA0, i0, Scalar(0.5) * ( lo + hi ), ph, pv );
				return;
			}
			bool inA1; int i1;
			Cell( hi, inA1, i1 );
			Scalar a0, b0, a1, b1;
			CumulativeInCell( inA0, i0, lo, a0, b0 );
			CumulativeInCell( inA1, i1, hi, a1, b1 );
			const Scalar inv = Scalar(1) / ( hi - lo );
			ph = ( a1 - a0 ) * inv;
			pv = ( b1 - b0 ) * inv;
		}

		//! A mass with centroid c and variance var, as a uniform spread of
		//! half-width sqrt(3 var) (plus 1e-7, so no window is a point).
		//! An ACCEPTED APPROXIMATION where the window straddles mu_c: the
		//! true mass inside a bin or sub-bin is not uniform, so the share
		//! priced on either side of the critical cosine's edge is the
		//! uniform model's, matched to the mass's first two moments only.
		//! What it buys is continuity: the price is a continuous function
		//! of c, var and eta, which no point or clamped-window rule is.
		static Scalar HalfWidth( const Scalar var )
		{
			return sqrt( r_max( var, Scalar(0) ) * Scalar(3) ) + Scalar(1e-7);
		}
	};

	void BuildLobeBasis( LobeBasis& b, const Scalar alpha, const Scalar eta, const Scalar tau )
	{
		using namespace RISE;
		using namespace RISE::Implementation;
		b.alpha = alpha; b.eta = eta; b.tau = tau; b.valid = true;
		b.JAphi = b.JApsi = b.JMphi = b.JMpsi = 0;
		const bool opaque = !( tau < std::numeric_limits<Scalar>::infinity() );

		// The coat's weights (DL-426: one continuous function of mu, exact
		// at the critical cosine; see CoatWeightTable).
		CoatWeightTable W;
		W.Build( eta, tau, opaque );
		const Scalar muCrit = W.muC;
		auto pointWeights = [&]( const Scalar mu, Scalar& ph, Scalar& pv ) { W.Lookup( mu, ph, pv ); };

		// roughness interpolation, linear in sqrt(alpha)
		const LobeSpillTable& T = SpillTable();
		Scalar x = ( sqrt( r_max( alpha, Scalar(0) ) ) - kLobeTA0 ) / ( Scalar(1) - kLobeTA0 ) * Scalar(kLobeA - 1);
		x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeA - 1) );
		const int    ia = r_min( (int)x, kLobeA - 2 );
		const Scalar fa = x - Scalar(ia);

		// Per view node: the spec lobe's return, escape and albedo for each
		// Fresnel basis function.  DL-426: EVERY bin is priced by one rule
		// -- its mass spread uniformly with the bin's own measured centroid
		// and spread, averaged against the continuous weights (CoatWeight
		// Table::Window) -- for every eta.  The pre-DL-426 build priced most
		// bins at their centroid and the three around mu_c by a uniform
		// window whose half-width was clamped to the centroid's distance
		// from the bin edge (a lopsided bin was priced at almost one
		// point); the set of windowed bins moved with eta, a
		// discontinuity.  Narrow lobes near mu_c are resolved by the
		// critical patch below, on the sub-bins.
		Scalar p0[kLobeN], p5[kLobeN];
		for( int j = 0; j < kLobeN; ++j ) {
			const std::size_t oa = ( (std::size_t)ia * kLobeN + j ) * kLobeN;
			const std::size_t ob = ( (std::size_t)( ia + 1 ) * kLobeN + j ) * kLobeN;
			Scalar g0 = 0, g5 = 0, e0 = 0, e5 = 0, q0 = 0, q5 = 0;
			for( int k = 0; k < kLobeN; ++k ) {
				const Scalar t0 = T.t0[oa + k] + ( T.t0[ob + k] - T.t0[oa + k] ) * fa;
				const Scalar t5 = T.t5[oa + k] + ( T.t5[ob + k] - T.t5[oa + k] ) * fa;
				q0 += t0;  q5 += t5;
				if( !( t0 > Scalar(0) ) ) continue;
				const Scalar tm = T.tm[oa + k] + ( T.tm[ob + k] - T.tm[oa + k] ) * fa;
				const Scalar t2 = T.t2[oa + k] + ( T.t2[ob + k] - T.t2[oa + k] ) * fa;
				const Scalar mc = tm / t0;
				const Scalar hw = CoatWeightTable::HalfWidth( t2 / t0 - mc * mc );
				Scalar ph, pv;
				W.Window( mc - hw, mc + hw, ph, pv );
				g0 += t0 * ph;  g5 += t5 * ph;
				e0 += t0 * pv;  e5 += t5 * pv;
			}
			b.g0[j] = g0; b.g5[j] = g5; b.e0[j] = e0; b.e5[j] = e5;
			p0[j] = q0; p5[j] = q5;
		}

		// The critical patch (see kPatchN).
		b.muC = muCrit;
		// The spill histogram at an arbitrary view mu, by displacement:
		// the two bracketing view nodes' sub-bin histograms, each sub-bin's
		// mass at its own centroid shifted by mu - mu_node.  A view past 1
		// (a patch node beyond normal incidence, DL-426) reads the last
		// node shifted: an extrapolation, so the patch is ONE function on
		// [mu_c, mu_c + W] for every eta, and only its part below 1 is
		// ever looked up.
		// Each view node's sub-bins, alpha-interpolated once per build
		// (only the nodes the patch reads): mass, F5 mass, centroid and
		// the uniform half-width of the sub-bin's own measured spread.
		// The patch spans 2 view-node intervals (kPatchW = 2 / kLobeN) and
		// reads both nodes of each, so at most kSubSlots distinct nodes; the
		// slots are per-thread storage reused by every build (no heap
		// allocation per build).
		struct SubBins { int n; Scalar u0[kLobeSubBins], u5[kLobeSubBins], mc[kLobeSubBins], hw[kLobeSubBins]; };
		constexpr int kSubSlots = 6;
		static thread_local SubBins subCache[kSubSlots];
		int nSub = 0;
		int subSlot[kLobeN];
		for( int j = 0; j < kLobeN; ++j ) subSlot[j] = -1;
		auto subBinsFor = [&]( const int j ) -> const SubBins& {
			if( subSlot[j] < 0 ) {
				// Never reached past kSubSlots (see above); reuse the last slot
				// rather than overrun if a future change widens the patch.
				const int slot = r_min( nSub, kSubSlots - 1 );
				if( nSub < kSubSlots ) ++nSub;
				for( int jj = 0; jj < kLobeN; ++jj ) if( subSlot[jj] == slot ) subSlot[jj] = -1;
				subSlot[j] = slot;
				SubBins& sbn = subCache[slot];
				sbn.n = 0;
				for( int ks = 0; ks < kLobeSubBins; ++ks ) {
					const std::size_t sa = ( (std::size_t)ia * kLobeN + j ) * kLobeSubBins + ks;
					const std::size_t sb = ( (std::size_t)( ia + 1 ) * kLobeN + j ) * kLobeSubBins + ks;
					const Scalar u0 = T.s0[sa] + ( T.s0[sb] - T.s0[sa] ) * fa;
					if( !( u0 > Scalar(0) ) ) continue;
					const Scalar um = T.sm[sa] + ( T.sm[sb] - T.sm[sa] ) * fa;
					const Scalar u2 = T.s2[sa] + ( T.s2[sb] - T.s2[sa] ) * fa;
					const Scalar mc = um / u0;
					sbn.u0[sbn.n] = u0;
					sbn.u5[sbn.n] = T.s5[sa] + ( T.s5[sb] - T.s5[sa] ) * fa;
					sbn.mc[sbn.n] = mc;
					sbn.hw[sbn.n] = CoatWeightTable::HalfWidth( u2 / u0 - mc * mc );
					++sbn.n;
				}
			}
			return subCache[ subSlot[j] ];
		};
		auto shiftedSum = [&]( const Scalar mu, Scalar& og0, Scalar& og5, Scalar& oe0, Scalar& oe5 ) {
			og0 = og5 = oe0 = oe5 = 0;
			Scalar xv = mu * Scalar(kLobeN) - Scalar(1);
			xv = r_min( r_max( xv, Scalar(0) ), Scalar(kLobeN - 1) );
			const int    j0 = r_min( (int)xv, kLobeN - 2 );
			const Scalar fv = xv - Scalar(j0);
			for( int side = 0; side < 2; ++side ) {
				const int    j  = j0 + side;
				const Scalar wv = side ? fv : ( Scalar(1) - fv );
				if( !( wv > Scalar(0) ) ) continue;
				const Scalar dmu = mu - LobeViewNode( j );
				// The node's sub-bins, each its mass spread over its own
				// measured width (a sub-bin is 1/256 wide; near normal
				// incidence a narrow lobe spans a fraction of one, mu_u =
				// cos compressing the angular spread by sin theta, so a
				// sub-bin-WIDE window would overstate its spread on the
				// edge -- its own variance does not).
				const SubBins& sbn = subBinsFor( j );
				Scalar a0 = 0, a5 = 0, c0 = 0, c5 = 0;
				for( int q = 0; q < sbn.n; ++q ) {
					const Scalar c = sbn.mc[q] + dmu;
					Scalar ph, pv;
					W.Window( c - sbn.hw[q], c + sbn.hw[q], ph, pv );
					a0 += sbn.u0[q] * ph;  a5 += sbn.u5[q] * ph;
					c0 += sbn.u0[q] * pv;  c5 += sbn.u5[q] * pv;
				}
				og0 += wv * a0;  og5 += wv * a5;
				oe0 += wv * c0;  oe5 += wv * c5;
			}
		};
		const bool patched = alpha < kPatchAlphaMax;
		for( int p = 0; p < kPatchN; ++p ) {
			const Scalar mp = LobePatchNode( muCrit, p );
			if( patched ) {
				shiftedSum( mp, b.pg0[p], b.pg5[p], b.pe0[p], b.pe5[p] );
			} else {
				b.pg0[p] = LobeNodeLerp( b.g0, mp );  b.pg5[p] = LobeNodeLerp( b.g5, mp );
				b.pe0[p] = LobeNodeLerp( b.e0, mp );  b.pe5[p] = LobeNodeLerp( b.e5, mp );
			}
		}
		if( patched ) {
			// Join the view-node arrays continuously at the patch's far end
			// (a correction growing as s^2 from 0 at mu_c) -- DL-426: for
			// every eta, through the node arrays' extrapolation when mu_c + W
			// lies past normal incidence (it was skipped there, a step in eta
			// at ~2.874).
			const Scalar muE = muCrit + kPatchW;
			const Scalar d0 = LobeNodeLerp( b.g0, muE ) - b.pg0[kPatchN - 1];
			const Scalar d5 = LobeNodeLerp( b.g5, muE ) - b.pg5[kPatchN - 1];
			const Scalar f0 = LobeNodeLerp( b.e0, muE ) - b.pe0[kPatchN - 1];
			const Scalar f5 = LobeNodeLerp( b.e5, muE ) - b.pe5[kPatchN - 1];
			for( int p = 0; p < kPatchN; ++p ) {
				const Scalar sp = Scalar(p) / Scalar(kPatchN - 1);
				const Scalar s2 = sp * sp;
				b.pg0[p] += d0 * s2;  b.pg5[p] += d5 * s2;
				b.pe0[p] += f0 * s2;  b.pe5[p] += f5 * s2;
			}
		}

		// Hemispherical integrals 2 INT (.) mu dmu against the arrays
		// looked up exactly as LobeDirection reads them.  DL-426: split at
		// mu_c -- midpoints uniform in mu below it, uniform in the outer
		// cosine t above it (dmu = t dt / (eta^2 mu)), so the square-root
		// edge of Psi at mu_c is resolved by construction and the nodes
		// move continuously with eta.  (Pre-DL-426 the 1/256 cells within
		// 2.5 cells of mu_c were sub-sampled with exact weights: a rule
		// that switched cells as mu_c moved.)
		auto interp = []( const Scalar* arr, const Scalar mu ) {
			Scalar xi = mu * Scalar(kLobeN) - Scalar(1);
			xi = r_min( r_max( xi, Scalar(0) ), Scalar(kLobeN - 1) );
			const int    i = r_min( (int)xi, kLobeN - 2 );
			const Scalar f = xi - Scalar(i);
			return arr[i] + ( arr[i + 1] - arr[i] ) * f;
		};
		b.Q0 = b.Q5 = b.E0 = b.E5 = b.G0 = b.G5 = b.H0 = b.H5 = 0;
		const int kQA = 128, kQB = 192;
		for( int seg = 0; seg < 2; ++seg ) {
			const int nq = seg ? kQB : kQA;
			if( seg == 0 && !( muCrit > Scalar(0) ) ) continue;
			for( int q = 0; q < nq; ++q ) {
				const Scalar u = ( Scalar(q) + Scalar(0.5) ) / Scalar(nq);
				Scalar mu, wq;
				if( seg == 0 ) {
					mu = muCrit * u;
					wq = Scalar(2) * mu * muCrit / Scalar(nq);
				} else {
					// t = u; mu = sqrt(t^2 + eta^2 - 1) / eta; 2 mu dmu = 2 t dt / eta^2
					mu = sqrt( r_max( Scalar(0), u * u + eta * eta - Scalar(1) ) ) / eta;
					wq = Scalar(2) * u / ( eta * eta * Scalar(nq) );
				}
				Scalar ph, pv;
				pointWeights( mu, ph, pv );
				const Scalar sM = Scalar(1) - MicrofacetEnergyLUT::LookupEssG2( mu, alpha );
				const Scalar r0 = interp( p0, mu ), r5 = interp( p5, mu );
				b.Q0 += wq * r0 * ph;  b.Q5 += wq * r5 * ph;
				b.E0 += wq * r0 * pv;  b.E5 += wq * r5 * pv;
				b.G0 += wq * LobeArrLookup( b.g0, b.pg0, muCrit, mu ) * pv;
				b.G5 += wq * LobeArrLookup( b.g5, b.pg5, muCrit, mu ) * pv;
				b.H0 += wq * LobeArrLookup( b.e0, b.pe0, muCrit, mu ) * pv;
				b.H5 += wq * LobeArrLookup( b.e5, b.pe5, muCrit, mu ) * pv;
				const Scalar om = Scalar(1) - mu;
				const Scalar sA = Scalar(1) - om * om * om * om * om;
				b.JAphi += wq * sA * ph;  b.JApsi += wq * sA * pv;
				b.JMphi += wq * sM * ph;  b.JMpsi += wq * sM * pv;
			}
		}
		b.Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
	}

	//! CLEAR coats (tau = 0, the common case: add_wetness, glTF / Blender
	//! clearcoat) read their basis from a second once-per-process table
	//! over (roughness node, coat eta node), blended bilinearly, so a
	//! TEXTURED substrate roughness costs a blend (~0.3 us) rather than a
	//! rebuild (~9-22 us).  The roughness blend is exactly what
	//! BuildLobeBasis does (the node arrays are linear in the table rows);
	//! the eta blend interpolates the coat's own weights between nodes
	//! spaced UNIFORMLY IN THE CRITICAL COSINE mu_c = sqrt(1 - 1/eta^2),
	//! not in eta: the inside Fresnel steps to total reflection at mu_c,
	//! and mu_c has infinite slope at eta = 1, so nodes uniform in eta
	//! (0.025 apart) put the whole [0, 0.22] range of mu_c inside ONE cell
	//! and the blended return term overshot the exact single bounce (a
	//! white metal under an index-1.01 coat read 1.16 at 80 deg).  Uniform
	//! in mu_c, a cell moves the step by 0.0118 (a third of a lobe bin)
	//! anywhere on [1, 3].  An absorbing / tinted coat (tau > 0) builds its
	//! basis directly (memoised below).
	constexpr int    kLobeE    = 81;							//!< eta nodes over [1, 3]
	constexpr Scalar kLobeMuC3 = Scalar(0.94280904158206336587);	//!< sqrt(8/9) = mu_c at eta 3
	inline Scalar LobeEtaNode( const int e )
	{
		const Scalar m = kLobeMuC3 * Scalar(e) / Scalar(kLobeE - 1);
		return Scalar(1) / sqrt( Scalar(1) - m * m );
	}

	struct LobeEvalEntry
	{
		Scalar g0[kLobeN], g5[kLobeN], e0[kLobeN], e5[kLobeN];
		Scalar pg0[kPatchN], pg5[kPatchN], pe0[kPatchN], pe5[kPatchN];
		Scalar Q0, Q5, E0, E5, G0, G5, H0, H5;
		Scalar JAphi, JApsi, JMphi, JMpsi;
	};

	inline const std::vector<LobeEvalEntry>& LobeClearTable()
	{
		static const std::vector<LobeEvalEntry> table = []() {
			std::vector<LobeEvalEntry> t( (std::size_t)kLobeA * kLobeE );
			LobeBasis b;
			for( int a = 0; a < kLobeA; ++a ) {
				for( int e = 0; e < kLobeE; ++e ) {
					BuildLobeBasis( b, LobeAlphaNode( a ), LobeEtaNode( e ), Scalar(0) );
					LobeEvalEntry& o = t[ (std::size_t)a * kLobeE + e ];
					for( int j = 0; j < kLobeN; ++j ) { o.g0[j] = b.g0[j]; o.g5[j] = b.g5[j]; o.e0[j] = b.e0[j]; o.e5[j] = b.e5[j]; }
					for( int p = 0; p < kPatchN; ++p ) { o.pg0[p] = b.pg0[p]; o.pg5[p] = b.pg5[p]; o.pe0[p] = b.pe0[p]; o.pe5[p] = b.pe5[p]; }
					o.Q0 = b.Q0; o.Q5 = b.Q5; o.E0 = b.E0; o.E5 = b.E5;
					o.G0 = b.G0; o.G5 = b.G5; o.H0 = b.H0; o.H5 = b.H5;
					o.JAphi = b.JAphi; o.JApsi = b.JApsi; o.JMphi = b.JMphi; o.JMpsi = b.JMpsi;
				}
			}
			return t;
		}();
		return table;
	}

	void BlendClearBasis( LobeBasis& out, const Scalar alpha, const Scalar eta )
	{
		using namespace RISE;
		const std::vector<LobeEvalEntry>& T = LobeClearTable();
		Scalar x = ( sqrt( r_max( alpha, Scalar(0) ) ) - kLobeTA0 ) / ( Scalar(1) - kLobeTA0 ) * Scalar(kLobeA - 1);
		x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeA - 1) );
		const int    ia = r_min( (int)x, kLobeA - 2 );
		const Scalar fa = x - Scalar(ia);
		const Scalar muC = ( eta > Scalar(1) ) ? sqrt( Scalar(1) - Scalar(1) / ( eta * eta ) ) : Scalar(0);
		Scalar y = muC / kLobeMuC3 * Scalar(kLobeE - 1);
		y = r_min( r_max( y, Scalar(0) ), Scalar(kLobeE - 1) );
		const int    ie = r_min( (int)y, kLobeE - 2 );
		const Scalar fe = y - Scalar(ie);
		const LobeEvalEntry& c00 = T[ (std::size_t)ia * kLobeE + ie ];
		const LobeEvalEntry& c01 = T[ (std::size_t)ia * kLobeE + ie + 1 ];
		const LobeEvalEntry& c10 = T[ (std::size_t)( ia + 1 ) * kLobeE + ie ];
		const LobeEvalEntry& c11 = T[ (std::size_t)( ia + 1 ) * kLobeE + ie + 1 ];
		const Scalar w00 = ( Scalar(1) - fa ) * ( Scalar(1) - fe ), w01 = ( Scalar(1) - fa ) * fe;
		const Scalar w10 = fa * ( Scalar(1) - fe ),                 w11 = fa * fe;
		auto mix = [&]( const Scalar a00, const Scalar a01, const Scalar a10, const Scalar a11 ) {
			return w00 * a00 + w01 * a01 + w10 * a10 + w11 * a11;
		};
		for( int j = 0; j < kLobeN; ++j ) {
			out.g0[j] = mix( c00.g0[j], c01.g0[j], c10.g0[j], c11.g0[j] );
			out.g5[j] = mix( c00.g5[j], c01.g5[j], c10.g5[j], c11.g5[j] );
			out.e0[j] = mix( c00.e0[j], c01.e0[j], c10.e0[j], c11.e0[j] );
			out.e5[j] = mix( c00.e5[j], c01.e5[j], c10.e5[j], c11.e5[j] );
		}
		// The patch arrays are anchored to mu_c, which is linear in this
		// blend's eta coordinate: blending them index-wise blends aligned
		// steps, and the blend's own mu_c is the query's.
		for( int p = 0; p < kPatchN; ++p ) {
			out.pg0[p] = mix( c00.pg0[p], c01.pg0[p], c10.pg0[p], c11.pg0[p] );
			out.pg5[p] = mix( c00.pg5[p], c01.pg5[p], c10.pg5[p], c11.pg5[p] );
			out.pe0[p] = mix( c00.pe0[p], c01.pe0[p], c10.pe0[p], c11.pe0[p] );
			out.pe5[p] = mix( c00.pe5[p], c01.pe5[p], c10.pe5[p], c11.pe5[p] );
		}
		out.muC = r_min( muC, kLobeMuC3 );
		out.Q0 = mix( c00.Q0, c01.Q0, c10.Q0, c11.Q0 );  out.Q5 = mix( c00.Q5, c01.Q5, c10.Q5, c11.Q5 );
		out.E0 = mix( c00.E0, c01.E0, c10.E0, c11.E0 );  out.E5 = mix( c00.E5, c01.E5, c10.E5, c11.E5 );
		out.G0 = mix( c00.G0, c01.G0, c10.G0, c11.G0 );  out.G5 = mix( c00.G5, c01.G5, c10.G5, c11.G5 );
		out.H0 = mix( c00.H0, c01.H0, c10.H0, c11.H0 );  out.H5 = mix( c00.H5, c01.H5, c10.H5, c11.H5 );
		out.JAphi = mix( c00.JAphi, c01.JAphi, c10.JAphi, c11.JAphi );
		out.JApsi = mix( c00.JApsi, c01.JApsi, c10.JApsi, c11.JApsi );
		out.JMphi = mix( c00.JMphi, c01.JMphi, c10.JMphi, c11.JMphi );
		out.JMpsi = mix( c00.JMpsi, c01.JMpsi, c10.JMpsi, c11.JMpsi );
		out.alpha = alpha; out.eta = eta; out.tau = Scalar(0); out.valid = true;
		out.Eavg = RISE::MicrofacetEnergyLUT::LookupEavgG2( alpha );
	}

	//! out = (1 - f) b0 + f b1, array by array (the roughness blend the
	//! clear table also performs: every array is linear in the spill rows).
	inline void BlendAlphaBases( LobeBasis& out, const LobeBasis& b0, const LobeBasis& b1, const Scalar f, const Scalar alpha )
	{
		const Scalar w0 = Scalar(1) - f;
		auto mix = [&]( const Scalar a, const Scalar b ) { return w0 * a + f * b; };
		for( int j = 0; j < kLobeN; ++j ) {
			out.g0[j] = mix( b0.g0[j], b1.g0[j] );  out.g5[j] = mix( b0.g5[j], b1.g5[j] );
			out.e0[j] = mix( b0.e0[j], b1.e0[j] );  out.e5[j] = mix( b0.e5[j], b1.e5[j] );
		}
		for( int p = 0; p < kPatchN; ++p ) {
			out.pg0[p] = mix( b0.pg0[p], b1.pg0[p] );  out.pg5[p] = mix( b0.pg5[p], b1.pg5[p] );
			out.pe0[p] = mix( b0.pe0[p], b1.pe0[p] );  out.pe5[p] = mix( b0.pe5[p], b1.pe5[p] );
		}
		out.Q0 = mix( b0.Q0, b1.Q0 );  out.Q5 = mix( b0.Q5, b1.Q5 );
		out.E0 = mix( b0.E0, b1.E0 );  out.E5 = mix( b0.E5, b1.E5 );
		out.G0 = mix( b0.G0, b1.G0 );  out.G5 = mix( b0.G5, b1.G5 );
		out.H0 = mix( b0.H0, b1.H0 );  out.H5 = mix( b0.H5, b1.H5 );
		out.JAphi = mix( b0.JAphi, b1.JAphi );  out.JApsi = mix( b0.JApsi, b1.JApsi );
		out.JMphi = mix( b0.JMphi, b1.JMphi );  out.JMpsi = mix( b0.JMpsi, b1.JMpsi );
		out.muC = b0.muC;
		out.alpha = alpha; out.eta = b0.eta; out.tau = b0.tau; out.valid = true;
		out.Eavg = RISE::MicrofacetEnergyLUT::LookupEavgG2( alpha );
	}

	//! An absorbing / tinted coat's basis at a ROUGHNESS NODE, from a
	//! per-thread 8-entry ring keyed on (node, eta, tau) -- a textured
	//! roughness keeps hitting the two nodes around it.  Copies out.
	inline void LobeNodeBasisCached( LobeBasis& out, const int ia, const Scalar eta, const Scalar tau )
	{
		static thread_local LobeBasis ring[8];
		static thread_local int       ringNode[8];
		static thread_local bool init = false;
		static thread_local unsigned int next = 0;
		if( !init ) {
			for( int i = 0; i < 8; ++i ) { ring[i].valid = false; ringNode[i] = -1; }
			init = true;
		}
		for( int i = 0; i < 8; ++i ) {
			if( ring[i].valid && ringNode[i] == ia && ring[i].eta == eta && ring[i].tau == tau ) {
				out = ring[i];
				return;
			}
		}
		LobeBasis& e = ring[next];
		ringNode[next] = ia;
		next = ( next + 1 ) & 7u;
		BuildLobeBasis( e, LobeAlphaNode( ia ), eta, tau );
		out = e;
	}

	//! Per-thread 4-entry memo keyed on (alpha, eta, tau): a coat and a
	//! substrate with uniform painters ask for the same one to three
	//! bases (one per channel when tinted) on every evaluation.  A miss
	//! blends the clear-coat table (tau = 0), or (tau > 0) blends the two
	//! roughness-node bases around alpha (LobeNodeBasisCached: a textured
	//! roughness under an absorbing coat pays a blend, ~1 us, not a build,
	//! ~10-20 us, until it leaves its roughness cell).  Returns a COPY so
	//! no caller holds a reference into a slot a later miss may overwrite.
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
		if( !( tau > Scalar(0) ) ) {
			BlendClearBasis( e, alpha, eta );
		} else {
			Scalar x = ( sqrt( r_max( alpha, Scalar(0) ) ) - kLobeTA0 ) / ( Scalar(1) - kLobeTA0 ) * Scalar(kLobeA - 1);
			x = r_min( r_max( x, Scalar(0) ), Scalar(kLobeA - 1) );
			const int    ia = r_min( (int)x, kLobeA - 2 );
			const Scalar fa = x - Scalar(ia);
			LobeBasis b0, b1;
			LobeNodeBasisCached( b0, ia, eta, tau );
			LobeNodeBasisCached( b1, ia + 1, eta, tau );
			BlendAlphaBases( e, b0, b1, fa, alpha );
		}
		out = e;
	}

	//! One channel's colours: the diffuse albedo, the Fresnel at normal
	//! incidence (Schlick's F0; the projection for conductor / film) and
	//! the Kulla-Conty multiscatter tint -- exactly what GGXBRDF::value
	//! multiplies its three lobes by.
	struct LobeChannel { Scalar c; Scalar F0; Scalar Fms; };

	//! Per-direction (channel-independent within one basis) pieces.
	struct LobeDir { Scalar g0, g5, e0, e5, sA, sM; };

	inline LobeDir LobeDirection( const LobeBasis& b, const Scalar mu )
	{
		LobeDir d;
		d.g0 = LobeArrLookup( b.g0, b.pg0, b.muC, mu );  d.g5 = LobeArrLookup( b.g5, b.pg5, b.muC, mu );
		d.e0 = LobeArrLookup( b.e0, b.pe0, b.muC, mu );  d.e5 = LobeArrLookup( b.e5, b.pe5, b.muC, mu );
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
	//! At most 1: whenever a round trip conserves energy (E + Q <= 1) the
	//! max below is 1 - Q and this is the exact series; a substrate
	//! authored brighter than physical (a diffuse colour above 1 is kept
	//! as authored, IPainter.h ReflectanceColor) can make E + Q exceed 1,
	//! where the series diverges -- the recycling then lets every returned
	//! unit out instead of amplifying it, the role the old
	//! CoatedLayer::Recycling's clamp of R to [0, 1] played.
	inline Scalar LobeReservoirEscape( const LobeTotals& t )
	{
		const Scalar denom = r_max( Scalar(1) - t.Q, t.E );
		return ( denom > Scalar(1e-12) ) ? t.E / denom : Scalar(0);
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

Scalar CoatedBRDF::RecycledSampleFraction() const
{
	// 0.15 of the substrate branch: bounds the recycled term's sample
	// weight by about pi * M / (0.15 (1 - pCoat)) at any substrate
	// roughness, for 15 % of the draws a smooth lobe would otherwise
	// spend on its own peak.  See CoatedSPF.cpp.
	return ( substrateModel == eSubstrateRefractedLobe ) ? Scalar(0.15) : Scalar(0);
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
