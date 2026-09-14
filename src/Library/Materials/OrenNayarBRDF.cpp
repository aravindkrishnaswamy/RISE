//////////////////////////////////////////////////////////////////////
//
//  OrenNayarBRDF.cpp - Implements the lambertian BRDF
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
#include "OrenNayarBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//////////////////////////////////////////////////////////////////
	// Baked bihemispherical-albedo tables for hemisphericalAlbedo{,NM}
	// (debt ledger DL-07, docs/DEBT_LEDGER.md; CLOTH_FABRIC_DESIGN.md
	// section 15 item 17 / WETNESS_COAT_DESIGN.md section 12 item 13).
	//
	// hemisphericalAlbedo(sigma) = rho*A1(sigma) + rho^2*A2(sigma),
	// where A1/A2 are exactly the bihemispherical (white-sky, uniform-
	// incident-field) integrals of the L1/L2 terms ComputeFactor
	// computes above -- see tools/OrenNayarHemisphericalAlbedoGen.cpp's
	// header for the full derivation, quadrature, and axis-warp
	// rationale.  DO NOT HAND-EDIT the two arrays below; regenerate
	// with that tool (`c++ -O3 -std=c++17 -o /tmp/gen
	// tools/OrenNayarHemisphericalAlbedoGen.cpp && /tmp/gen`) and paste
	// both back in verbatim if OrenNayarBRDF::ComputeFactor's algebra
	// ever changes.
	//
	// kNumSigmaBins=64 nodes, kSigmaMax=3.0, sigma_i = kSigmaMax*(i/(N-1))^2,
	// baked at Nmu=256 Nphi=512 midpoint quadrature (measured converged
	// to 6 significant figures against Nmu=800/Nphi=1600).
	const unsigned int kNumSigmaBins = 64;
	const float kSigmaMax = 3.0f;

	static const float kOrenNayarA1Table[ kNumSigmaBins ] =
	{
		1.00000000f, 	0.99999928f, 	0.99998897f, 	0.99994415f,
		0.99982357f, 	0.99956930f, 	0.99910718f, 	0.99834704f,
		0.99718297f, 	0.99549448f, 	0.99314827f, 	0.99000061f,
		0.98590189f, 	0.98070282f, 	0.97426295f, 	0.96646124f,
		0.95720768f, 	0.94645494f, 	0.93420738f, 	0.92052662f,
		0.90553206f, 	0.88939536f, 	0.87233102f, 	0.85458273f,
		0.83640850f, 	0.81806558f, 	0.79979759f, 	0.78182387f,
		0.76433271f, 	0.74747741f, 	0.73137558f, 	0.71611065f,
		0.70173508f, 	0.68827450f, 	0.67573220f, 	0.66409373f,
		0.65333110f, 	0.64340669f, 	0.63427609f, 	0.62589121f,
		0.61820179f, 	0.61115748f, 	0.60470891f, 	0.59880823f,
		0.59341007f, 	0.58847171f, 	0.58395326f, 	0.57981777f,
		0.57603121f, 	0.57256216f, 	0.56938213f, 	0.56646502f,
		0.56378710f, 	0.56132680f, 	0.55906457f, 	0.55698264f,
		0.55506492f, 	0.55329686f, 	0.55166537f, 	0.55015844f,
		0.54876530f, 	0.54747617f, 	0.54628217f, 	0.54517525f,
	};

	static const float kOrenNayarA2Table[ kNumSigmaBins ] =
	{
		0.00000000f, 	0.00000075f, 	0.00001195f, 	0.00006049f,
		0.00019105f, 	0.00046567f, 	0.00096277f, 	0.00177509f,
		0.00300606f, 	0.00476443f, 	0.00715661f, 	0.01027720f,
		0.01419823f, 	0.01895860f, 	0.02455539f, 	0.03093906f,
		0.03801408f, 	0.04564521f, 	0.05366891f, 	0.06190783f,
		0.07018580f, 	0.07834101f, 	0.08623566f, 	0.09376132f,
		0.10084038f, 	0.10742432f, 	0.11348983f, 	0.11903413f,
		0.12406991f, 	0.12862079f, 	0.13271742f, 	0.13639432f,
		0.13968757f, 	0.14263299f, 	0.14526518f, 	0.14761660f,
		0.14971733f, 	0.15159480f, 	0.15327382f, 	0.15477666f,
		0.15612316f, 	0.15733100f, 	0.15841584f, 	0.15939149f,
		0.16027017f, 	0.16106267f, 	0.16177848f, 	0.16242597f,
		0.16301255f, 	0.16354471f, 	0.16402824f, 	0.16446823f,
		0.16486916f, 	0.16523501f, 	0.16556935f, 	0.16587529f,
		0.16615562f, 	0.16641285f, 	0.16664916f, 	0.16686654f,
		0.16706675f, 	0.16725139f, 	0.16742185f, 	0.16757941f,
	};

	//! Map `sigma` to a continuous bin position on the POWER-2-WARPED
	//! axis sigma_i = kSigmaMax*(i/(N-1))^2 (nodes uniform in
	//! sqrt(sigma) -- measured to need 4x fewer nodes than a uniform-
	//! sigma axis for the same worst-case interpolation error, because
	//! A1/A2's slope-in-sigma peaks around sigma in [0.05,0.5], not at
	//! sigma=0; see the generator's header for the alternatives
	//! measured and rejected).  Clamped at both ends: sigma<0 floors to
	//! node 0 (Lambertian), sigma>kSigmaMax clamps to the table's last
	//! node -- the table's STATED domain (the model plateaus there:
	//! A1+A2 measured 0.6978 at sigma=50 vs 0.7127 at sigma=3), not a
	//! clamp of convenience.
	Scalar SigmaPos( const Scalar sigma )
	{
		const Scalar sClamped = ( sigma < Scalar(0) ) ? Scalar(0) :
			( ( sigma > (Scalar)kSigmaMax ) ? (Scalar)kSigmaMax : sigma );
		const Scalar t = sqrt( sClamped / (Scalar)kSigmaMax );
		return t * (Scalar)( kNumSigmaBins - 1 );
	}

	//! Linear interpolation of a table indexed by SigmaPos.
	Scalar LookupSigmaTable( const float table[], const Scalar sigma )
	{
		const Scalar pos = SigmaPos( sigma );
		const unsigned int i0 = (unsigned int)pos;
		if( i0 >= kNumSigmaBins - 1 ) {
			return (Scalar)table[ kNumSigmaBins - 1 ];
		}
		const Scalar frac = pos - (Scalar)i0;
		return (Scalar)table[i0] * ( Scalar(1) - frac ) + (Scalar)table[i0+1] * frac;
	}

	//! hemisphericalAlbedo(sigma) = rho*OrenNayarA1(sigma) + rho^2*OrenNayarA2(sigma).
	Scalar OrenNayarA1( const Scalar sigma ) { return LookupSigmaTable( kOrenNayarA1Table, sigma ); }
	Scalar OrenNayarA2( const Scalar sigma ) { return LookupSigmaTable( kOrenNayarA2Table, sigma ); }
}

OrenNayarBRDF::OrenNayarBRDF(
	const IPainter& reflectance,
	const IScalarPainter& roughness
	) :
  pReflectance( &reflectance ),
  pRoughness( &roughness )
{
	pReflectance->addref();
	pRoughness->addref();
}

OrenNayarBRDF::~OrenNayarBRDF( )
{
	safe_release( pReflectance );
	safe_release( pRoughness );
}

void OrenNayarBRDF::SetReflectance( const IPainter& v )
{
	v.addref();
	safe_release( pReflectance );
	pReflectance = &v;
}

void OrenNayarBRDF::SetRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRoughness );
	pRoughness = &v;
}

template< class T >
void OrenNayarBRDF::ComputeFactor( 
	T& L1, 
	T& L2, 
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const Vector3& n, 
	const T& roughness 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) &&	(nv >= NEARZERO) ) {
		// Geometric-horizon gate: a GlintModifier-tilted shading normal can
		// validate light/view directions that are still below the true
		// geometric surface.  This is a DEFENSIVE check (a valid exterior hit
		// already satisfies it) rather than a literal sampler-consistency one
		// -- NEE's light direction isn't sampler-drawn -- but it guards
		// against the same tilt pathology; the early return leaves L1/L2 at
		// the caller's zero-init, identical to the guard falling through.
		// Degenerate vGeomNormal falls back to the shading normal (gate is a
		// no-op).
		// (r is tautologically inside the gate: r = -ri.ray.Dir() and geomN is
		// ray-anchored, so Dot(r,geomN) > 0 always holds -- see LambertianBRDF.cpp:57-60.)
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( v, geomN ) <= 0 || Vector3Ops::Dot( r, geomN ) <= 0 ) {
			return;
		}

		const T sqr_r = roughness*roughness;
		const Scalar cos_phi_diff = Vector3Ops::Dot(
			Vector3Ops::Normalize(r-(n*nr)),
			Vector3Ops::Normalize(v-(n*nv))
			);

		const Scalar theta_i = acos(nv);
		const Scalar theta_r = acos(nr);

		const Scalar alpha = r_max(theta_i,theta_r);
		const Scalar beta = r_min(theta_i,theta_r);

		const T C1 = 1.0 - 0.5*(sqr_r / (sqr_r + 0.33));
		const T C2 = 0.45 * (sqr_r / (sqr_r + 0.09)) *	(sin(alpha) - ((cos_phi_diff >= 0)? 0 : pow(2.0*beta/PI,3.0)));
		const Scalar t = (4.0*alpha*beta/(PI*PI));
		const T C3 = 0.125 * (sqr_r / (sqr_r + 0.09)) * (t*t);
		L1 = ( C1 + cos_phi_diff * C2 * tan(beta) + (1.0 - fabs(cos_phi_diff)) * C3 * tan((alpha+beta)/2.0) );
		const Scalar u = (2.0*beta)/PI;
		L2 = 0.17 * (sqr_r / (sqr_r + 0.13)) * (1.0 - cos_phi_diff * (u*u));
	}
}

RISEPel OrenNayarBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel L1, L2;
	const ScalarTriple r = pRoughness->GetValuesAt(ri);
	const RISEPel roughness( r.v[0], r.v[1], r.v[2] );

	// Flip to the ray-facing frame, mirroring OrenNayarSPF::Scatter's FlipW
	// (same condition), so value() agrees with Scatter()/Pdf() on back-face
	// hits -- ComputeFactor's geomN gate orients to whatever n it's given,
	// so the flip propagates through automatically.
	const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) ? -ri.onb.w() : ri.onb.w();
	ComputeFactor<RISEPel>( L1, L2, vLightIn, ri, n, roughness );
	const RISEPel rho = pReflectance->GetColor(ri);

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}

Scalar OrenNayarBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar L1=0, L2=0;

	// Same ray-facing flip as value() above.
	const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) ? -ri.onb.w() : ri.onb.w();
	ComputeFactor<Scalar>( L1, L2, vLightIn, ri, n, pRoughness->GetValueAtNM(ri,nm) );
	const Scalar rho = GuardedGetColorNM( *pReflectance, ri, nm );

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}

RISEPel OrenNayarBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Oren-Nayar is energy-conserving: total reflectance ≈ Rd
	// regardless of roughness.  The L1 / L2 terms only redistribute
	// directional scattering shape.
	return pReflectance->GetColor( ri );
}

//////////////////////////////////////////////////////////////////////
// hemisphericalAlbedo{,NM} -- FIXED 2026-09-14 (DL-07).  IBSDF.h's
// contract for this method is the BIHEMISPHERICAL (white-sky) albedo:
// "reflectance under a uniform incident field", with `ri` for painter
// sampling ONLY -- `ri.ray` must not be read, so the return cannot
// depend on any single incidence or view direction.  Before this fix,
// the method ignored roughness entirely and returned `rho` verbatim
// -- exact only at roughness 0 -- which is what DL-07 tracked as a
// ~12.6% / ~25.6% over-estimate at roughness 0.5 / 1.0 (that pre-fix
// comment's *directional*-hemispherical figures, quoted at fixed
// incidence angles, do NOT reproduce under an independent brute-force
// integration of THIS implementation's ComputeFactor -- see
// tests/OrenNayarHemisphericalAlbedoTest.cpp's
// TestDirectionalHemisphericalReproduction, which measures 0.8963 /
// 0.7745 at roughness 0.5 / 1.0, 0 deg incidence, where that comment
// stated 0.8740 / 0.7444; the DIRECTION and rough SIZE of the bias
// were right, the exact table was not, and this fix does not depend
// on it -- see below).
//
// THE FIX.  f(wi,wo) = (L1/pi)*rho + (L2/pi)*rho^2 (ComputeFactor
// above), and L1/L2 depend only on (theta_i, theta_o, deltaPhi,
// sigma) -- never on rho -- so the bihemispherical integral factors
// exactly into
//
//     R_bi = (1/pi) INT_H INT_H f(wi,wo) (n.wi)(n.wo) dwi dwo
//          = rho * A1(sigma) + rho^2 * A2(sigma)
//
// where A1(sigma) = (1/pi^2) INT INT L1 (n.wi)(n.wo) dwi dwo and A2
// likewise for L2 -- pure functions of roughness alone, baked as two
// small 1-D tables by tools/OrenNayarHemisphericalAlbedoGen.cpp (see
// that tool's header for the full derivation, the exact-not-
// approximate azimuthal reduction it uses, the quadrature, and the
// axis warp) and looked up above via OrenNayarA1/OrenNayarA2.  At
// sigma=0, A1=1 and A2=0, recovering the old exact Lambertian case
// exactly (C1=1, C2=C3=0 there).  Measured residual: A1+A2 exceeds 1
// by at most ~0.02% for sigma in [0,2] (the *model itself* is not
// perfectly energy-conserving -- see ComputeFactor's own qualitative
// fit -- not a quadrature artifact; converged from Nmu=50 to Nmu=800
// in a scratch probe) and the table interpolates its own converged
// curve to within 2.3e-4 absolute -- both negligible next to the
// 10-27% bias this replaces.  tests/OrenNayarHemisphericalAlbedoTest.cpp
// gates this with an INDEPENDENT double-hemisphere quadrature over
// the real BRDF (different resolution/loop structure than the
// generator's own bake).
//
// DIRECTION OF THE ERROR THIS FIXES, for the two consumers that read
// it.  `coated_material`'s recycling denominator 1/(1 - r_i*R) and
// `fabric_material`'s `R * SheenTransmitMean(...)` energy-subtraction
// both used the old over-estimated R; both now read the corrected,
// still-conservative bihemispherical value, so a coated or
// sheen-covered rough Oren-Nayar substrate gets dimmer (was too
// BRIGHT) at any roughness above ~0.1 -- see this fix's commit for
// the measured before/after on both materials' furnace configs.
//
// PER-CHANNEL ROUGHNESS.  `value()`/`valueNM()` already read a
// per-channel (RGB) or per-wavelength (NM) roughness via
// `pRoughness->GetValuesAt`/`GetValueAtNM` -- so hemisphericalAlbedo
// does the same here, evaluating A1/A2 once per channel/wavelength
// rather than collapsing to a single representative sigma (which
// would silently disagree with value()'s own per-channel shape for a
// spectrally-varying roughness painter -- the same class of bug this
// whole row is about, just on the OTHER input; see
// tests/OrenNayarHemisphericalAlbedoTest.cpp's TestPerChannelRoughness).
//////////////////////////////////////////////////////////////////////
bool OrenNayarBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	const ScalarTriple sigma = pRoughness->GetValuesAt( ri );
	const RISEPel rho = pReflectance->GetColor( ri );

	for( unsigned int c = 0; c < 3; c++ ) {
		const Scalar a1 = OrenNayarA1( sigma.v[c] );
		const Scalar a2 = OrenNayarA2( sigma.v[c] );
		out[c] = rho[c] * a1 + rho[c] * rho[c] * a2;
	}
	return true;
}

bool OrenNayarBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	const Scalar sigma = pRoughness->GetValueAtNM( ri, nm );
	const Scalar rho = GuardedGetColorNM( *pReflectance, ri, nm );
	const Scalar a1 = OrenNayarA1( sigma );
	const Scalar a2 = OrenNayarA2( sigma );
	out = rho * a1 + rho * rho * a2;
	return true;
}

// Explicit instantiation so other TUs (OrenNayarSPF.cpp) can link to the
// scalar overload without seeing the template body.  The RISEPel flavour
// is instantiated implicitly through OrenNayarBRDF::value above.
template void OrenNayarBRDF::ComputeFactor<Scalar>(
	Scalar&, Scalar&,
	const Vector3&, const RayIntersectionGeometric&,
	const Vector3&, const Scalar& );
