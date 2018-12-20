//////////////////////////////////////////////////////////////////////
//
//  Optics.cpp - Implementation of a bunch of useful optics functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 11, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Optics.h"

using namespace RISE;

Vector3 Optics::CalculateReflectedRay( const Vector3& vIn, const Vector3& vNormal )
{
	// By Snell's law
	Scalar	d = 2.0 * Vector3Ops::Dot(vNormal, vIn);
	return Vector3( vIn.x - d * vNormal.x, vIn.y - d * vNormal.y, vIn.z - d * vNormal.z );
}

bool Optics::CalculateRefractedRay( const Vector3& vNormal, const Scalar Ni, const Scalar Nt, Vector3& vIn )
{
	// Use Snell's law
	Scalar		k = Vector3Ops::Dot( vNormal, vIn );
	Vector3	s = (Ni/Nt) * (vIn-k*vNormal);
	k = 1.0 - Vector3Ops::SquaredModulus(s);

	if( k < NEARZERO ) {
		return false;
	} else {
		vIn = s - sqrt(k) * vNormal;
		return true;
	}
}

Scalar Optics::CalculateDielectricReflectance( const Vector3& v, const Vector3& tv, const Vector3& n, const Scalar Ni, const Scalar Nt )
{
	const Scalar cosAi = fabs(Vector3Ops::Dot(v, n));
	const Scalar cosAt = fabs(Vector3Ops::Dot(tv, n));
	const Scalar nn = Ni * Nt;
	const Scalar cc = cosAt * cosAi;
	const Scalar Ni2 = Ni * Ni;
	const Scalar Nt2 = Nt * Nt;
	const Scalar ci2 = cosAi * cosAi;
	const Scalar ct2 = cosAt * cosAt;
	const Scalar Nit = Ni2 - Nt2;
	const Scalar cit = ci2 - ct2;
	const Scalar num = Nit*Nit*cc*cc +  cit*cit*nn*nn;
	Scalar denom = cc*(Ni2 + Nt2) + nn*(ci2 + ct2);
	denom = denom*denom;

	Scalar answer;
	if ((denom < 0.000001) && (num < 0.000001)) {
		answer = 1.0;
	} else {
		answer = num/denom;
	}

	if (answer < 1.0) {
		return answer;
	} else {
		// place a debug assertion here
		return 1.00;
	}
}


