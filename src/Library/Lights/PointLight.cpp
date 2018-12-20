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

using namespace RISE;
using namespace RISE::Implementation;

PointLight::PointLight( 
	const Scalar radiantEnergy_,
	const RISEPel& c,
	const Scalar linearAtten,
	const Scalar quadraticAtten
	) : 
  radiantEnergy( radiantEnergy_ ),
  ptPosition( Point3( 0, 0, 0 ) ),
  cColor( c ),
  linearAttenuation( linearAtten ),
  quadraticAttenuation( quadraticAtten )
{
}

PointLight::~PointLight( )
{
}

void PointLight::ComputeDirectLighting( 
	const RayIntersectionGeometric& ri, 
	const IRayCaster& pCaster, 
	const IBSDF& brdf, 
	const bool bReceivesShadows, 
	RISEPel& amount 
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

	const Scalar fDot = Vector3Ops::Dot( vToLight, ri.vNormal );

	if( fDot <= 0.0 ) {
		return;
	}

	if( bReceivesShadows ) {
		// Check to see if there is a shadow
		Ray		rayToLight;
		rayToLight.origin = ri.ptIntersection;
		rayToLight.dir = vToLight;

		if( pCaster.CastShadowRay( rayToLight, fDistFromLight ) ) {
			return;
		}
	}

	Scalar attenuation = 1.0;

	if( linearAttenuation ) {
		attenuation += (linearAttenuation * fDistFromLight);
	}

	if( quadraticAttenuation) {
		attenuation += (quadraticAttenuation * fDistFromLight * fDistFromLight);
	}

	amount = (cColor * brdf.value( vToLight, ri )) * ((1.0/attenuation) * fDot * radiantEnergy);
}

void PointLight::FinalizeTransformations( )
{
	// Tells out transform helper to finalize transformations
	Transformable::FinalizeTransformations();

	// Then calculate the real-world position of this light...
	ptPosition = Point3Ops::Transform( m_mxFinalTrans, Point3( 0, 0, 0 ) );
}

static const unsigned int COLOR_ID = 100;
static const unsigned int ENERGY_ID = 101;
static const unsigned int LINEARATTEN_ID = 102;
static const unsigned int QUADRATICATTEN_ID = 103;

IKeyframeParameter* PointLight::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "color" ) {
		double d[3];
		if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
			p = new Parameter<RISEPel>( RISEPel(d), COLOR_ID );
		}
	} else if( name == "energy" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), ENERGY_ID );
	} else if( name == "linear_attenuation" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), LINEARATTEN_ID );
	} else if( name == "quadratic_attenuation" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), QUADRATICATTEN_ID );
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
		}
		break;
	case ENERGY_ID:
		{
			radiantEnergy = *(Scalar*)val.getValue();
		}
		break;
	case LINEARATTEN_ID:
		{
			linearAttenuation = *(Scalar*)val.getValue();
		}
		break;
	case QUADRATICATTEN_ID:
		{
			quadraticAttenuation = *(Scalar*)val.getValue();
		}
		break;
	}

	Transformable::SetIntermediateValue( val );
}

