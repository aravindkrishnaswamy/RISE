//////////////////////////////////////////////////////////////////////
//
//  SpotLight.cpp - Implementation of the SpotLight class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 15, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SpotLight.h"
#include "../Animation/KeyframableHelper.h"
#include "../Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for transparent (Fresnel-attenuated) shadow rays

using namespace RISE;
using namespace RISE::Implementation;

SpotLight::SpotLight(
	const Scalar radiantEnergy_,
	const Point3& target,
	const Scalar inner,
	const Scalar outer,
	const RISEPel& c,
	const bool shootPhotons
	) :
  radiantEnergy( radiantEnergy_ ),
  ptPosition( Point3( 0, 0, 0 ) ),
  ptTarget( target ),
  dInnerAngle( inner ),
  dOuterAngle( outer ),
  cColor( c ),
  bShootPhotons( shootPhotons )
{
	vDirection = Vector3Ops::Normalize(Vector3Ops::mkVector3(ptTarget,ptPosition));
}

SpotLight::~SpotLight( )
{
}

void SpotLight::ComputeDirectLighting(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	RISEPel& amount
	) const
{
	//
	// Computing direct lighting for spot lights
	//
	amount = RISEPel(0.0);

	// Vector to the light from the surface
	Vector3	vToLight = Vector3Ops::mkVector3( ptPosition, ri.ptIntersection );
	Scalar fDistFromLight = Vector3Ops::NormalizeMag(vToLight);

	// This dot product tells us the angle of incidence between the light ray
	// and the surface normal.  This angle tells us what illumination this surface
	// should recieve.  If this value is negative, then the light is
	// behind the object and we can stop.

	// Also if this angle is greater than the outer angle, then we stop because
	// we limit the spot light's effect.  If its between the outer and inner
	// angles, then we linearly scale it.  If its within the inner angle, then its
	// at full power.

	const Scalar fDot = Vector3Ops::Dot( vToLight, ri.vNormal );

	if( fDot <= 0.0 ) {
		return;
	}

	const Scalar fDirDot = Vector3Ops::Dot( vToLight, -vDirection );
	const Scalar fAngleOfIncidence = acos(fDirDot);

	if( fAngleOfIncidence <= dOuterAngle/2.0 )
	{
		// shadowT carries the per-interface Fresnel transmittance when
		// transparent_shadows is enabled, else (1,1,1).  See PointLight.
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

		// Physically-based inverse-square falloff
		const Scalar invDistSq = 1.0 / (fDistFromLight * fDistFromLight);

		if( fAngleOfIncidence <= dInnerAngle/2.0 ) {
			amount = (cColor * brdf.value( vToLight, ri )) * (invDistSq * fDot * radiantEnergy) * shadowT;
		} else {
			// Quadratic falloff between inner and outer half-angles
			const Scalar t = (dOuterAngle/2.0 - fAngleOfIncidence) / (dOuterAngle/2.0 - dInnerAngle/2.0);
			amount = (cColor * brdf.value( vToLight, ri )) * (invDistSq * t * t * fDot * radiantEnergy) * shadowT;
		}
	}
}

Scalar SpotLight::ComputeDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf,
	const bool bReceivesShadows,
	const Scalar nm
	) const
{
	// Same geometry / cone falloff as the RGB ComputeDirectLighting; only the
	// BSDF eval and the shadow Fresnel are per-wavelength (brdf.valueNM,
	// CastShadowRayAuto bNM=true).
	Vector3 vToLight = Vector3Ops::mkVector3( ptPosition, ri.ptIntersection );
	const Scalar fDistFromLight = Vector3Ops::NormalizeMag( vToLight );

	const Scalar fDot = Vector3Ops::Dot( vToLight, ri.vNormal );
	if( fDot <= 0.0 ) {
		return Scalar(0);
	}

	const Scalar fDirDot = Vector3Ops::Dot( vToLight, -vDirection );
	const Scalar fAngleOfIncidence = acos( fDirDot );
	if( fAngleOfIncidence > dOuterAngle/2.0 ) {
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
	const Scalar lightLum =
		Scalar(0.2126) * cColor.r +
		Scalar(0.7152) * cColor.g +
		Scalar(0.0722) * cColor.b;
	const Scalar base = lightLum * brdf.valueNM( vToLight, ri, nm ) * invDistSq * fDot * radiantEnergy * shadowT;

	if( fAngleOfIncidence <= dInnerAngle/2.0 ) {
		return base;
	}
	// Quadratic falloff between inner and outer half-angles (matches RGB).
	const Scalar tt = (dOuterAngle/2.0 - fAngleOfIncidence) / (dOuterAngle/2.0 - dInnerAngle/2.0);
	return base * tt * tt;
}

void SpotLight::FinalizeTransformations( )
{
	// Tells out transform helper to finalize transformations
	Transformable::FinalizeTransformations();

	// Then calculate the real-world position of this light...
	ptPosition = Point3Ops::Transform( m_mxFinalTrans, Point3( 0, 0, 0 ) );
	vDirection = Vector3Ops::Normalize(Vector3Ops::mkVector3(ptTarget,ptPosition));
}

static const unsigned int TARGET_ID = 100;
static const unsigned int COLOR_ID = 101;
static const unsigned int ENERGY_ID = 102;
static const unsigned int INNER_ANGLE_ID = 105;
static const unsigned int OUTER_ANGLE_ID = 106;

IKeyframeParameter* SpotLight::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "target" ) {
		double d[3];
		if( ParseStrictVec3( value, d ) ) {
			p = new Point3Keyframe( Point3( d[0], d[1], d[2] ), TARGET_ID );
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
	} else if( name == "inner_angle" ) {
		Scalar parsed;
		if( !ParseStrictScalar( value, parsed ) ) {
			return 0;
		}
		p = new Parameter<Scalar>( parsed * DEG_TO_RAD, INNER_ANGLE_ID );
	} else if( name == "outer_angle" ) {
		Scalar parsed;
		if( !ParseStrictScalar( value, parsed ) ) {
			return 0;
		}
		p = new Parameter<Scalar>( parsed * DEG_TO_RAD, OUTER_ANGLE_ID );
	} else {
		return Transformable::KeyframeFromParameters( name, value );
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void SpotLight::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case TARGET_ID:
		{
			ptTarget = *(Point3*)val.getValue();
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
	case INNER_ANGLE_ID:
		{
			dInnerAngle = *(Scalar*)val.getValue();
		}
		break;
	case OUTER_ANGLE_ID:
		{
			dOuterAngle = *(Scalar*)val.getValue();
		}
		break;
	}

	Transformable::SetIntermediateValue( val );
}
