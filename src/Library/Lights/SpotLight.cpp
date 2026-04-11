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
		if( bReceivesShadows ) {
			// Check to see if there is a shadow
			Ray		rayToLight( ri.ptIntersection, vToLight );

			if( pCaster.CastShadowRay( rayToLight, fDistFromLight ) ) {
				return;
			}
		}

		// Physically-based inverse-square falloff
		const Scalar invDistSq = 1.0 / (fDistFromLight * fDistFromLight);

		if( fAngleOfIncidence <= dInnerAngle/2.0 ) {
			amount = (cColor * brdf.value( vToLight, ri )) * (invDistSq * fDot * radiantEnergy);
		} else {
			// Quadratic falloff between inner and outer half-angles
			const Scalar t = (dOuterAngle/2.0 - fAngleOfIncidence) / (dOuterAngle/2.0 - dInnerAngle/2.0);
			amount = (cColor * brdf.value( vToLight, ri )) * (invDistSq * t * t * fDot * radiantEnergy);
		}
	}
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
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, TARGET_ID );
		}
	} else if( name == "color" ) {
		double d[3];
		if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
			p = new Parameter<RISEPel>( RISEPel(d), COLOR_ID );
		}
	} else if( name == "energy" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), ENERGY_ID );
	} else if( name == "inner_angle" ) {
		p = new Parameter<Scalar>( atof(value.c_str())*DEG_TO_RAD, INNER_ANGLE_ID );
	} else if( name == "outer_angle" ) {
		p = new Parameter<Scalar>( atof(value.c_str())*DEG_TO_RAD, OUTER_ANGLE_ID );
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
