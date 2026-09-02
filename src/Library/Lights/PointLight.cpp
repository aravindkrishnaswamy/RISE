//////////////////////////////////////////////////////////////////////
//
//  PointLight.cpp - Implementation of the PointLight class
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
#include "PointLight.h"
#include "../Animation/KeyframableHelper.h"
#include "../Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for transparent (Fresnel-attenuated) shadow rays
#include "../Utilities/Color/RGBSpectra.h"

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// ILight's two out-of-line defaults (Stage C slice 2).
//
// They live in THIS translation unit — rather than inline in ILight.h —
// so that RGBSpectra.h (and through it the Jakob-Hanika LUT table
// header) is not dragged into every TU that includes ILight.h.
// PointLight.cpp is always linked into the library, and every build
// target links the whole library, so the definitions are always
// available.  See docs/SPECTRAL_ILLUMINANT_CONVENTION.md.
//////////////////////////////////////////////////////////////////////

Scalar ILight::emittedRadianceNM( const Vector3& vLightOut, const Scalar nm ) const
{
	return RGBIlluminantSpectrum::FromRGB( emittedRadiance( vLightOut ) ).Eval( nm );
}

Scalar ILight::ComputeDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	const Scalar nm,
	const bool bFullSphereReceiver,
	const bool bVolumeReceiver
	) const
{
	RISEPel amount( 0, 0, 0 );
	ComputeDirectLighting( ri, pCaster, brdf, bReceivesShadows, amount, bFullSphereReceiver, bVolumeReceiver );
	// `amount` is a computed RADIANCE, so it uplifts as an illuminant and
	// round-trips through the film back to itself.  (Pre-Stage-C this was a
	// Rec.709 luma projection that discarded `nm` entirely.)
	return RGBIlluminantSpectrum::FromRGB( amount ).Eval( nm );
}

PointLight::PointLight(
	const Scalar radiantEnergy_,
	const RISEPel& c,
	const bool shootPhotons
	) :
  radiantEnergy( radiantEnergy_ ),
  ptPosition( Point3( 0, 0, 0 ) ),
  cColor( c ),
  bShootPhotons( shootPhotons )
{
	RefreshSpectrum();
}

PointLight::~PointLight( )
{
}

void PointLight::ComputeDirectLighting(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	RISEPel& amount,
	const bool /*bFullSphereReceiver*/,	// no-op here; see the .h doc
	const bool bVolumeReceiver			// implemented; see the .h doc
	) const
{
	//
	// Computing direct lighting for point lights
	//
	amount = RISEPel(0.0);

	// Vector to the light from the surface
	Vector3	vToLight = Vector3Ops::mkVector3( ptPosition, ri.ptIntersection );
	const Scalar fDistFromLight = Vector3Ops::NormalizeMag(vToLight);

	// This dot product tells us the angle of incidence between the light ray
	// and the surface normal.  This angle tells us what illumination this surface
	// should recieve.  If this value is negative, then the light is
	// behind the object and we can stop.
	//
	// VOLUME RECEIVER: at a medium scatter vertex `ri.vNormal` is the
	// outgoing direction `wo`, not a normal, so the cosine is meaningless
	// and the `<= 0` gate would reject half the sphere at a vertex that
	// scatters over all of it.  Drop both; the phase function (handed in
	// as `brdf`) carries the whole angular term.  `invDistSq` below is
	// deliberately KEPT -- it is emitter geometry, not receiver
	// orientation.  With the flag false this reduces TEXTUALLY to the
	// pre-existing expression and gate.  See ILight.h for the derivation.
	const Scalar fDotSigned = Vector3Ops::Dot( vToLight, ri.vNormal );
	const Scalar fDot = bVolumeReceiver ? Scalar(1.0) : fDotSigned;

	if( !bVolumeReceiver && fDot <= 0.0 ) {
		return;
	}

	// shadowT carries the per-interface Fresnel transmittance when
	// transparent_shadows is enabled (a clear dielectric between the surface
	// and the light attenuates rather than fully blocks), else (1,1,1).  Routed
	// through CastShadowRayAuto so a direct ComputeDirectLighting caller honors
	// the flag uniformly with the LightSampler NEE path and the other lights.
	RISEPel shadowT( 1.0, 1.0, 1.0 );
	if( bReceivesShadows ) {
		Ray		rayToLight( ri.ptIntersection, vToLight );

		const RayCaster* pRC = dynamic_cast<const RayCaster*>( &pCaster );
		if( pRC ) {
			if( pRC->CastShadowRayAuto( rayToLight, fDistFromLight, false, 0.0, shadowT ) ) {
				return;
			}
		} else if( pCaster.CastShadowRay( rayToLight, fDistFromLight ) ) {
			return;
		}
	}

	// Physically-based inverse-square falloff.
	// emittedRadiance = cColor * radiantEnergy [watts/sr]
	// Irradiance at surface = emittedRadiance * cos / d^2
	const Scalar invDistSq = 1.0 / (fDistFromLight * fDistFromLight);

	amount = (cColor * brdf.value( vToLight, ri )) * (invDistSq * fDot * radiantEnergy) * shadowT;
}

Scalar PointLight::ComputeDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	const Scalar nm,
	const bool /*bFullSphereReceiver*/,	// no-op here; see the .h doc
	const bool bVolumeReceiver			// implemented; see the .h doc
	) const
{
	// Same geometry as the RGB ComputeDirectLighting; only the BSDF eval and
	// the shadow Fresnel are per-wavelength (brdf.valueNM, CastShadowRayAuto
	// bNM=true) so a clear dielectric attenuates with the wavelength-specific
	// IOR rather than the representative RGB IOR.  VOLUME RECEIVER: see the
	// RGB overload above.
	Vector3 vToLight = Vector3Ops::mkVector3( ptPosition, ri.ptIntersection );
	const Scalar fDistFromLight = Vector3Ops::NormalizeMag( vToLight );

	const Scalar fDotSigned = Vector3Ops::Dot( vToLight, ri.vNormal );
	const Scalar fDot = bVolumeReceiver ? Scalar(1.0) : fDotSigned;
	if( !bVolumeReceiver && fDot <= 0.0 ) {
		return Scalar(0);
	}

	Scalar shadowT = 1.0;
	if( bReceivesShadows ) {
		Ray rayToLight( ri.ptIntersection, vToLight );
		const RayCaster* pRC = dynamic_cast<const RayCaster*>( &pCaster );
		if( pRC ) {
			RISEPel t( 1.0, 1.0, 1.0 );
			if( pRC->CastShadowRayAuto( rayToLight, fDistFromLight, true, nm, t ) ) {
				return Scalar(0);
			}
			shadowT = t.r;	// NM path fills all 3 channels equally
		} else if( pCaster.CastShadowRay( rayToLight, fDistFromLight ) ) {
			return Scalar(0);
		}
	}

	const Scalar invDistSq = 1.0 / (fDistFromLight * fDistFromLight);
	// Stage C slice 2: the light's own spectrum at `nm`, NOT a Rec.709 luma
	// projection of its RGB colour used flat at every wavelength.  The luma
	// collapse made every coloured point light spectrally grey and tinted
	// even a white one by the flat-spectrum chromaticity.
	const Scalar lightSpec = cSpectrum.Eval( nm );
	return lightSpec * brdf.valueNM( vToLight, ri, nm ) * invDistSq * fDot * radiantEnergy * shadowT;
}

void PointLight::RefreshSpectrum()
{
	cSpectrum = RGBIlluminantSpectrum::FromRGB( cColor );
}

void PointLight::FinalizeTransformations( const Matrix4& parentWorld )
{
	// Tells out transform helper to finalize transformations
	Transformable::FinalizeTransformations( parentWorld );

	// Then calculate the real-world position of this light...
	ptPosition = Point3Ops::Transform( m_mxFinalTrans, Point3( 0, 0, 0 ) );
}

static const unsigned int COLOR_ID = 100;
static const unsigned int ENERGY_ID = 101;

IKeyframeParameter* PointLight::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "color" ) {
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

void PointLight::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
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
