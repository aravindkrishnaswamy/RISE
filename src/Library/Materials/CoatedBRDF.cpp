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
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"

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
	const bool recyclingCompensation
	) :
  pBase( &base ),
  pCoatWeight( &coatWeight ),
  pCoatIOR( &coatIOR ),
  pCoatRoughness( &coatRoughness ),
  pCoatThickness( &coatThickness ),
  pCoatAbsorption( &coatAbsorption ),
  pCoatTint( &coatTint ),
  bRecycling( recyclingCompensation )
{
	pBase->addref();
	pCoatWeight->addref();
	pCoatIOR->addref();
	pCoatRoughness->addref();
	pCoatThickness->addref();
	pCoatAbsorption->addref();
	pCoatTint->addref();
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
	const RISEPel tintRGB = pCoatTint->GetColor( ri );
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
		const Scalar t = pCoatTint->GetColorNM( ri, nm );
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
	// (1 - r_i) explicitly.  Consumers of the toggle therefore gate the
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

		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
		if( ( Scalar(1) - Eavg ) > Scalar(1e-10) )
		{
			const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alpha );
			const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alpha );
			const Scalar f_ms  = ( Scalar(1) - Ess_o ) * ( Scalar(1) - Ess_i ) * INV_PI / ( Scalar(1) - Eavg );
			const Scalar F_ms  = MicrofacetEnergyLUT::ComputeFms<Scalar>( re, Eavg );
			spec += F_ms * f_ms;
		}

		return r_max( Scalar(0), spec );
	}

	//! Per-channel round-trip attenuation for the recycling series.
	inline RISE::RISEPel RecycleRoundTripRGB( const RISE::Implementation::CoatedBRDF::CoatParams& cp )
	{
		using namespace RISE;
		return RISEPel(
			CoatedLayer::RecycleRoundTrip( cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted ),
			CoatedLayer::RecycleRoundTrip( cp.eta, cp.thickness, cp.absorption, cp.tint[1], cp.tinted ),
			CoatedLayer::RecycleRoundTrip( cp.eta, cp.thickness, cp.absorption, cp.tint[2], cp.tinted ) );
	}

	//! 7.4's geometric series, or unity when the red-proof lever has
	//! switched it off.  The ONE place the toggle acts, so `value`,
	//! `valueNM`, `albedo` and `hemisphericalAlbedo{,NM}` cannot drift
	//! apart on it.
	inline RISE::RISEPel RecyclingFactorRGB(
		const bool enabled,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const RISE::RISEPel& R )
	{
		using namespace RISE;
		if( !enabled ) {
			return RISEPel( 1, 1, 1 );
		}
		return CoatedLayer::RecyclingRGB( cp.ri, R, RecycleRoundTripRGB( cp ) );
	}

	inline RISE::Scalar RecyclingFactorNM(
		const bool enabled,
		const RISE::Implementation::CoatedBRDF::CoatParams& cp,
		const RISE::Scalar R )
	{
		using namespace RISE;
		if( !enabled ) {
			return Scalar(1);
		}
		const Scalar rt = CoatedLayer::RecycleRoundTrip( cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );
		return CoatedLayer::Recycling( cp.ri, R, rt );
	}
}

RISEPel CoatedBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const OrthonormalBasis3D onb = RayFacingONB( ri );
	const Vector3 n = onb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );			// toward the light
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );		// toward the viewer

	const Scalar nv = Vector3Ops::Dot( n, v );
	const Scalar nr = Vector3Ops::Dot( n, r );
	if( nv < NEARZERO || nr < NEARZERO ) {
		return RISEPel( 0, 0, 0 );
	}
	if( !PassesHorizonGate( v, r, n, ri ) ) {
		return RISEPel( 0, 0, 0 );
	}

	const RISEPel fBase = pBase->value( vLightIn, ri );

	CoatParams cp;
	ResolveCoat( ri, Scalar(-1), cp );
	if( cp.weight <= 0 ) {
		return fBase;						// bare substrate, exactly
	}

	// --- coat lobe -------------------------------------------------
	const Scalar fCoat = CoatLobeValue( v, r, onb, nv, nr, cp.alpha, cp.eta, cp.re );

	// --- substrate reached THROUGH the coat ------------------------
	// Interface transmittances at the two macro angles.  The substrate
	// BRDF itself is evaluated at the UNREFRACTED directions: a
	// Lambertian / Oren-Nayar base is direction-independent anyway, and
	// for a GGX base this matches Mitsuba's `roughplastic`, keeps
	// reciprocity manifest, and leaves the exit-side radiance
	// compression to the 1/eta^2 factor rather than double-counting it
	// in a refracted-frame evaluation.
	const Scalar Tin  = Scalar(1) - CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar Tout = Scalar(1) - CoatedLayer::Fresnel( nv, cp.eta );

	const RISEPel Ain  = CoatedLayer::PassTransmittanceRGB( nr, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
	const RISEPel Aout = CoatedLayer::PassTransmittanceRGB( nv, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );

	const RISEPel R   = SubstrateAlbedo( ri );		// VIEW-INDEPENDENT -- see SubstrateAlbedo
	const RISEPel rec = RecyclingFactorRGB( bRecycling, cp, R );

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
	if( nv < NEARZERO || nr < NEARZERO ) {
		return 0;
	}
	if( !PassesHorizonGate( v, r, n, ri ) ) {
		return 0;
	}

	const Scalar fBase = pBase->valueNM( vLightIn, ri, nm );

	CoatParams cp;
	ResolveCoat( ri, nm, cp );
	if( cp.weight <= 0 ) {
		return fBase;
	}

	const Scalar fCoat = CoatLobeValue( v, r, onb, nv, nr, cp.alpha, cp.eta, cp.re );

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
	const Scalar K  = Ain * Aout * RecyclingFactorNM( bRecycling, cp, R ) * ( Tin * Tout / ( cp.eta * cp.eta ) );

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
	// F(cos_o); the rest enters, and what comes back out is
	// R * (1 - r_i) / (1 - r_i R) attenuated by two coat traversals.
	// At R = 1, A = 1 this is F + (1 - F) = 1 exactly, so the AOV
	// stays in [0,1] as IBSDF::albedo requires.
	const Scalar  F = CoatedLayer::Fresnel( nr, cp.eta );
	const Scalar  T = Scalar(1) - F;
	const RISEPel A = CoatedLayer::PassTransmittanceRGB( nr, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
	const RISEPel rec = RecyclingFactorRGB( bRecycling, cp, R );

	const RISEPel sub = A * A * R * rec * ( T * ( Scalar(1) - cp.ri ) );
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
// replaced by its HEMISPHERICAL AVERAGE r_e and the coat traversal
// evaluated at the diffuse-mean cosine, so nothing here reads
// `ri.ray` -- the contract IBSDF.h states.
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

	const Scalar  F = cp.re;						// hemispherical, not F(cos_o)
	const Scalar  T = Scalar(1) - F;
	const RISEPel A = CoatedLayer::PassTransmittanceRGB(
		CoatedLayer::kRecycleMeanCos, cp.eta, cp.thickness, cp.absorption, cp.tint, cp.tinted );
	const RISEPel rec = RecyclingFactorRGB( bRecycling, cp, R );

	const RISEPel sub = A * A * R * rec * ( T * ( Scalar(1) - cp.ri ) );
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
	const Scalar T = Scalar(1) - F;
	const Scalar A = CoatedLayer::PassTransmittance(
		CoatedLayer::kRecycleMeanCos, cp.eta, cp.thickness, cp.absorption, cp.tint[0], cp.tinted );

	const Scalar sub = A * A * R * RecyclingFactorNM( bRecycling, cp, R ) * T * ( Scalar(1) - cp.ri );
	const Scalar result = cp.weight * ( F + sub ) + ( Scalar(1) - cp.weight ) * R;
	out = r_min( r_max( result, Scalar(0) ), Scalar(1) );
	return true;
}
