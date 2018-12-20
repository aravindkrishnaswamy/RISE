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

	if( bReceivesShadows ) {
		// Check to see if there is a shadow
		Ray		rayToLight;
		rayToLight.origin = ri.ptIntersection;
		rayToLight.dir = vDirection;

		if( pCaster.CastShadowRay( rayToLight, INFINITY ) ) {
			return;
		}
	}

	amount = (cColor * brdf.value( vDirection, ri )) * (fDot * radiantEnergy);
}

static const unsigned int DIRECTION_ID = 100;
static const unsigned int COLOR_ID = 101;
static const unsigned int ENERGY_ID = 102;

IKeyframeParameter* DirectionalLight::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "direction" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, DIRECTION_ID );
		}
	} else if( name == "color" ) {
		double d[3];
		if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
			p = new Parameter<RISEPel>( RISEPel(d), COLOR_ID );
		}
	} else if( name == "energy" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), ENERGY_ID );
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


