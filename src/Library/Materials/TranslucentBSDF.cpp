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

TranslucentBSDF::TranslucentBSDF( const IPainter& rF, const IPainter& T, const IScalarPainter& exp ) :
  pRefFront( &rF ), pTrans( &T ), pExponent( &exp )
{
	pRefFront->addref();
	pTrans->addref();
	pExponent->addref();
}

TranslucentBSDF::~TranslucentBSDF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExponent );
}

void TranslucentBSDF::SetRefFront( const IPainter& v )      { v.addref(); safe_release( pRefFront ); pRefFront = &v; }
void TranslucentBSDF::SetTrans( const IPainter& v )         { v.addref(); safe_release( pTrans );    pTrans    = &v; }
void TranslucentBSDF::SetN( const IScalarPainter& v )       { v.addref(); safe_release( pExponent ); pExponent = &v; }

template< class T >
static char GetReflectedSide( T& intensity, const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& exponent )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn);
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir());

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	// Build the specular lobe direction from |n.r| (nrSigned is a magnitude,
	// not a signed, per-branch mirror-reflect of r about n the way
	// IsotropicPhongBRDF::ComputeDiffuseSpecularFactors does it) -- this is
	// a pre-existing, intentionally-simple approximation: GetReflectedSide's
	// case 0 (the transmission lobe, `intensity` consumed by value()'s
	// `case 0`) fires for TWO geometrically distinct configurations (viewer
	// front/light back, and viewer back/light front) and reuses this one
	// `sd` formula for both, where IsotropicPhongBRDF has no equivalent
	// mismatched-sign case to be equivalent to (its two branches only cover
	// matched-sign viewer/light pairs).  What actually changed here: the old
	// construction dotted an UN-normalized `incident` (|incident|^2 = 1+8nr^2
	// for nr<0) against v, so sd could exceed 1 and pow(sd, exponent) could
	// blow up to Inf for a tilted shading frame.  Normalizing before the dot
	// bounds sd to [0,1] by construction, which is the actual fix -- it does
	// not make this construction equivalent to a Householder mirror
	// reflection for the nr<0 sub-case.
	Scalar nrSigned = nr;
	if( nrSigned <= 0 ) {
		nrSigned = -nrSigned;
	}

	const Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((nrSigned*-2.0)*n));
	const Scalar sd = fabs( Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), v ) );
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
	const ScalarTriple exp_t = pExponent->GetValuesAt(ri);
	const RISEPel exp_rgb( exp_t.v[0], exp_t.v[1], exp_t.v[2] );
	switch( GetReflectedSide<RISEPel>(intensity, vLightIn, ri, ri.onb.w(), exp_rgb ) )
	{
	case 0:
		return pTrans->GetColor(ri) * intensity * INV_PI;
		break;
	case 1:
		return pRefFront->GetColor(ri) * INV_PI;
		break;
	case 2:
		return pRefFront->GetColor(ri) * INV_PI;
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
	switch( GetReflectedSide<Scalar>(intensity, vLightIn, ri, ri.onb.w(), pExponent->GetValueAtNM(ri,nm) ) )
	{
	case 0:
		return pTrans->GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	case 1:
		return pRefFront->GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	case 2:
		return pRefFront->GetColorNM(ri,nm) * intensity * INV_PI;
		break;
	default:
	case 3:
		return 0;
		break;
	}
}

RISEPel TranslucentBSDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Only the reflective lobe returns energy back toward the camera —
	// transmitted energy reaches the OIDN beauty pass via what's behind
	// the surface, not via this BSDF's albedo AOV.
	return pRefFront->GetColor( ri );
}
