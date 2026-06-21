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

using namespace RISE;
using namespace RISE::Implementation;

DirectionalLight::DirectionalLight( Scalar radiantEnergy_, const RISEPel& c, const Vector3& vDir ) : 
  radiantEnergy( radiantEnergy_ ),
  cColor( c ),
  vDirection( Vector3Ops::Normalize(vDir) )
{

}

DirectionalLight::~DirectionalLight( )
{
}

void DirectionalLight::ComputeDirectLighting( 
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster, 
	const IBSDF& brdf, 
	const bool bReceivesShadows,
	RISEPel& amount 
	) const
{
	amount = RISEPel(0.0);

	// This dot product tells us the angle of incidence between the light ray
	// and the surface normal.  This angle tells us what illumination this surface
	// should recieve.  If this value is negative, then the light is 
	// behind the object and we can stop.

	Scalar		fDot = Vector3Ops::Dot( vDirection, ri.vNormal );

	if( fDot <= 0.0 ) {
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
	const Scalar nm
	) const
{
	// Same geometry as the RGB ComputeDirectLighting: cosine of angle
	// between light direction and surface normal, shadow ray test.
	// Only the BSDF eval differs (per-NM scalar instead of per-RGB).
	const Scalar fDot = Vector3Ops::Dot( vDirection, ri.vNormal );
	if( fDot <= 0.0 ) {
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

	const Scalar lightLum =
		Scalar(0.2126) * cColor.r +
		Scalar(0.7152) * cColor.g +
		Scalar(0.0722) * cColor.b;
	return lightLum * brdf.valueNM( vDirection, ri, nm ) * fDot * radiantEnergy * shadowT;
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


