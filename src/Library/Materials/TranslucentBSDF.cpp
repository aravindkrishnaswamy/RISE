//////////////////////////////////////////////////////////////////////
//
//  TranslucentBSDF.cpp - Implements the translucent BSDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentBSDF.h"

using namespace RISE;
using namespace RISE::Implementation;

TranslucentBSDF::TranslucentBSDF( const IPainter& rF, const IPainter& T, const IPainter& exp ) :
  pRefFront( rF ), pTrans( T ), exponent( exp )
{
	pRefFront.addref();
	pTrans.addref();
	exponent.addref();
}

TranslucentBSDF::~TranslucentBSDF( )
{
	pRefFront.release();
	pTrans.release();
	exponent.release();
}

template< class T >
static char GetReflectedSide( T& intensity, const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& exponent )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn);
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir);

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	const Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((fabs(nr)*-2.0)*n));
	const Scalar sd = fabs( Vector3Ops::Dot( Vector3( incident.x, incident.y, incident.z ), v ) );
	intensity = pow( sd, exponent );

	if( nr <= /*-NEARZERO*/ 0 )						// viewer front
	{
		if( nv <= -NEARZERO ) {						// light front
			return 1;
		} else {									// light back
			return 0;
		}
	}
	else if( nr >= NEARZERO )						// viewer back
	{
		if( nv >= NEARZERO) {						// light back
			return 2;
		} else {									// light front
			return 0;
		}
	}

	return 3;
}


RISEPel TranslucentBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel intensity = RISEPel(1,1,1);
	switch( GetReflectedSide<RISEPel>(intensity, vLightIn, ri, ri.onb.w(), exponent.GetColor(ri) ) )
	{
	case 0:
		return pTrans.GetColor(ri) * intensity * INV_PI;
		break;
	case 1:
		return pRefFront.GetColor(ri) * INV_PI;
		break;
	case 2:
		return pRefFront.GetColor(ri) * INV_PI;
		break;
	default:
	case 3:
		return RISEPel(0,0,0);
		break;
	}
}

Scalar TranslucentBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar intensity = 1.0;
	switch( GetReflectedSide<Scalar>(intensity, vLightIn, ri, ri.onb.w(), exponent.GetColorNM(ri,nm) ) )
	{
	case 0:
		return pTrans.GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	case 1:
		return pRefFront.GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	case 2:
		return pRefFront.GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	default:
	case 3:
		return 0;
		break;
	}
}
