//////////////////////////////////////////////////////////////////////
//
//  DirectionalLight.cpp - Implementation of the DirectionalLight class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DirectionalLight.h"
#include "../Animation/KeyframableHelper.h"
#include "../Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for transparent (Fresnel-attenuated) shadow rays
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

DirectionalLight::DirectionalLight( Scalar radiantEnergy_, const RISEPel& c, const Vector3& vDir ) : 
  radiantEnergy( radiantEnergy_ ),
  cColor( c ),
  vDirection( Vector3Ops::Normalize(vDir) )
{
	RefreshSpectrum();
}

void DirectionalLight::RefreshSpectrum()
{
	// FromRGB derives its scale from the max channel OUTSIDE the
	// maxc>1e-9 branch, so a negative cColor (a keyframe spline can
	// overshoot past its endpoint values even when every keyframe is
	// non-negative, and SetIntermediateValue feeds the interpolated
	// value straight into this function) yields sigmoid 0.5 times a
	// NEGATIVE scale.  Clamp a local copy; cColor itself is left
	// untouched since emissionColor() and the RGB path return it
	// verbatim.
	RISEPel c = cColor;
	ColorMath::EnsurePositve( c );
	cSpectrum = RGBIlluminantSpectrum::FromRGB( c );
}

DirectionalLight::~DirectionalLight( )
{
}

void DirectionalLight::ComputeDirectLighting(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	RISEPel& amount,
	const bool bFullSphereReceiver,
	const bool bVolumeReceiver
	) const
{
	amount = RISEPel(0.0);

	// This dot product tells us the angle of incidence between the light ray
	// and the surface normal.  This angle tells us what illumination this surface
	// should recieve.  If this value is negative, then the light is
	// behind the object and we can stop.

	// FULL-SPHERE NEE (residual wave 2 item D; see LightSampler.cpp's
	// FULL-SPHERE NEE block comment for the full derivation, and
	// IMaterial::ScattersFullSphere() for the capability).  A
	// directional light has no MIS partner to keep in partition with
	// (same as LightSampler's delta-position light row) -- so when the
	// receiver scatters over the full sphere (e.g. hair), `fabs` simply
	// restores the below-horizon term at full weight instead of
	// rejecting it.  When `bFullSphereReceiver` is false this reduces
	// TEXTUALLY to the pre-existing expression -- `fDotSigned` used
	// verbatim -- so every non-full-sphere receiver is byte-identical.
	//
	// VOLUME RECEIVER (residual-ledger item 10 of
	// docs/PT_ENV_MIS_DOUBLECOUNT.md).  `bVolumeReceiver` says the
	// receiver is a MEDIUM SCATTER vertex, where `ri.vNormal` is not a
	// normal at all: MediumTransport::EvaluateInScattering{,NM} sets it
	// to the OUTGOING direction `wo` so that the surface-shaped rows it
	// borrows produce *something*.  `Dot(vDirection, wo)` is a
	// meaningless quantity there, and worse, the `<= 0` gate below
	// rejects HALF THE SPHERE at a vertex whose phase function scatters
	// over all of it -- a directional light lit only fog whose outgoing
	// direction happened to lie within 90 degrees of the light.
	//
	// Derivation of the correct behaviour, radiance-only:
	//     Ls(x, wo) = sigma_s(x) INT_{S^2} p(wi, wo) Li(x, wi) dwi
	// The in-scattering integral runs over the FULL sphere and its only
	// angular factor is the phase function.  There is no `cos theta`
	// because the surface form's cosine is the projected-area Jacobian
	// dA_perp/dA of an oriented surface patch, and a volume element has
	// neither patch nor orientation.  `p` is already supplied to us as
	// `brdf` (MediumScatterBSDF::value returns p(vLightIn, wo)) and
	// `sigma_s` is applied by the caller, so all this function owes the
	// estimator is `Li` = emitted radiance times visibility.  Setting
	// `fDot` to 1 drops the factor; skipping the gate drops the
	// rejection.  That is exactly what LightSampler's Step-2 / Step-3
	// rows already do inline for the same vertex (`cosSurface = 1.0`
	// under `isVolumeScatter`, plus `if( !isVolumeScatter && ... )` on
	// the gate); Step 1, which reaches us, did not.
	//
	// PRECEDENCE: `bVolumeReceiver` beats `bFullSphereReceiver`.  The
	// latter replaces the signed cosine with |cos| because a full-sphere
	// SURFACE still has a real normal and a real projected-area factor;
	// the former removes the cosine outright.  No cosine at all beats an
	// unsigned cosine, so the volume branch is tested first.
	//
	// With both flags false the expression below reduces TEXTUALLY to the
	// pre-existing one -- `fDotSigned` used verbatim, gate unchanged --
	// so every surface receiver is byte-identical, not merely close.
	const Scalar fDotSigned = Vector3Ops::Dot( vDirection, ri.vNormal );
	const Scalar fDot = bVolumeReceiver ? Scalar(1.0) :
		( bFullSphereReceiver ? std::fabs( fDotSigned ) : fDotSigned );

	if( !bVolumeReceiver && fDot <= 0.0 ) {
		return;
	}

	// shadowT carries the per-interface Fresnel transmittance when
	// transparent_shadows is enabled (a clear dielectric between the surface
	// and the light attenuates the directional contribution rather than fully
	// blocking it), else (1,1,1).  Routed through CastShadowRayAuto so the flag
	// is honored for directional lights exactly as for the omni/spot/area NEE
	// path — and uniformly across analytic-primitive and SDF dielectrics.
	RISEPel shadowT( 1.0, 1.0, 1.0 );
	if( bReceivesShadows ) {
		Ray		rayToLight( ri.ptIntersection, vDirection );

		const RayCaster* pRC = dynamic_cast<const RayCaster*>( &pCaster );
		if( pRC ) {
			if( pRC->CastShadowRayAuto( rayToLight, RISE_INFINITY, false, 0.0, shadowT ) ) {
				return;
			}
		} else if( pCaster.CastShadowRay( rayToLight, RISE_INFINITY ) ) {
			return;
		}
	}

	amount = (cColor * brdf.value( vDirection, ri )) * (fDot * radiantEnergy) * shadowT;
}

Scalar DirectionalLight::ComputeDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	const Scalar nm,
	const bool bFullSphereReceiver,
	const bool bVolumeReceiver
	) const
{
	// Same geometry as the RGB ComputeDirectLighting: cosine of angle
	// between light direction and surface normal, shadow ray test.
	// Only the BSDF eval differs (per-NM scalar instead of per-RGB).
	// FULL-SPHERE NEE and VOLUME RECEIVER: see the RGB overload above for
	// both derivations; byte-identical to before when both flags are
	// false.
	const Scalar fDotSigned = Vector3Ops::Dot( vDirection, ri.vNormal );
	const Scalar fDot = bVolumeReceiver ? Scalar(1.0) :
		( bFullSphereReceiver ? std::fabs( fDotSigned ) : fDotSigned );
	if( !bVolumeReceiver && fDot <= 0.0 ) {
		return Scalar(0);
	}

	// shadowT carries the Fresnel transmittance when transparent_shadows is
	// enabled (else 1.0).  See the RGB ComputeDirectLighting above.
	Scalar shadowT = 1.0;
	if( bReceivesShadows ) {
		Ray rayToLight( ri.ptIntersection, vDirection );
		const RayCaster* pRC = dynamic_cast<const RayCaster*>( &pCaster );
		if( pRC ) {
			RISEPel t( 1.0, 1.0, 1.0 );
			if( pRC->CastShadowRayAuto( rayToLight, RISE_INFINITY, true, nm, t ) ) {
				return Scalar(0);
			}
			shadowT = t.r;	// NM path fills all 3 channels equally
		} else if( pCaster.CastShadowRay( rayToLight, RISE_INFINITY ) ) {
			return Scalar(0);
		}
	}

	// Stage C slice 2: the light's own spectrum at `nm`, not a flat Rec.709
	// luma projection.  See PointLight::ComputeDirectLightingNM.
	const Scalar lightSpec = cSpectrum.Eval( nm );
	return lightSpec * brdf.valueNM( vDirection, ri, nm ) * fDot * radiantEnergy * shadowT;
}

static const unsigned int DIRECTION_ID = 100;
static const unsigned int COLOR_ID = 101;
static const unsigned int ENERGY_ID = 102;

IKeyframeParameter* DirectionalLight::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "direction" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Vector3Keyframe( Vector3( d[0], d[1], d[2] ), DIRECTION_ID );
		}
	} else if( name == "color" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Parameter<RISEPel>( RISEPel(d), COLOR_ID );
		}
	} else if( name == "energy" ) {
		Scalar parsed;
		if( !ParseStrictScalar( value, parsed ) ) {
			return 0;
		}
		p = new Parameter<Scalar>( parsed, ENERGY_ID );
	} else {
		return Transformable::KeyframeFromParameters( name, value );
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void DirectionalLight::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case DIRECTION_ID:
		{
			vDirection = Vector3Ops::Normalize(*(Vector3*)val.getValue());
		}
		break;
	case COLOR_ID:
		{
			cColor = *(RISEPel*)val.getValue();
			RefreshSpectrum();		// keep the cached illuminant spectrum in step
		}
		break;
	case ENERGY_ID:
		{
			radiantEnergy = *(Scalar*)val.getValue();
		}
		break;
	}

	Transformable::SetIntermediateValue( val );
}


