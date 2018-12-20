//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDF.cpp - Implements a polished BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  WARNING!!!  This is experimental and unverified code
//             from ggLibrary, DO NOT USE!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PolishedBRDF.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

PolishedBRDF::PolishedBRDF( const IPainter& reflectance, const IPainter& phongN_, const IPainter& Nt_ ) : 
  pReflectance( reflectance ), phongN( phongN_ ), Nt( Nt_ )
{
	pReflectance.addref();
	phongN.addref();
	Nt.addref();
}

PolishedBRDF::~PolishedBRDF()
{
	pReflectance.release();
	phongN.release();
	Nt.release();
}

Scalar ComputeRs( const Vector3& v, const Vector3& n, const Scalar ior )
{
	Vector3	t = v;

	if( Optics::CalculateRefractedRay( n, 1.0, ior, t ) ) {					// t becomes refracted vector
		return Optics::CalculateDielectricReflectance( v, t, n, 1.0, ior );
	}

	return 1.0;
}

RISEPel PolishedBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const Vector3& n = Vector3Ops::Dot(ri.vNormal,-vLightIn)<NEARZERO? -ri.vNormal : ri.vNormal;
	const RISEPel ior = Nt.GetColor(ri);
	if( ior[0] == ior[1] && ior[1] == ior[2] ) {
		Scalar		Rs = ComputeRs( -vLightIn, n, ior[0] );
		return pReflectance.GetColor(ri) * INV_PI * (1.0 - Rs);
	}

	// Otherwise we have to do it for each pel
	RISEPel Rs;
	for( int i=0; i<3; i++ ) {
		Rs[i] = ComputeRs( -vLightIn, n, ior[i] );
	}

	return pReflectance.GetColor(ri) * INV_PI * (1.0 - Rs);
}

Scalar PolishedBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Vector3& n = Vector3Ops::Dot(ri.vNormal,-vLightIn)<NEARZERO? -ri.vNormal : ri.vNormal;
	Scalar		Rs = ComputeRs( -vLightIn, n, Nt.GetColorNM(ri,nm) );
	return pReflectance.GetColorNM(ri,nm) * INV_PI * (1.0 - Rs);
}