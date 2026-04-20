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
#include "../Interfaces/ILog.h"

using namespace RISE;

Vector3 Optics::CalculateReflectedRay( const Vector3& vIn, const Vector3& vNormal )
{
	Scalar normalMag = Vector3Ops::Magnitude( vNormal );
	if( normalMag < NEARZERO ) {
		GlobalLog()->PrintEx( eLog_Error, "Optics::CalculateReflectedRay: Invalid normal magnitude (%e)", normalMag );
		return vIn;
	}

	Vector3 useNormal = vNormal;
	if( fabs(normalMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateReflectedRay: Non-unit normal passed in (|n|=%f), normalizing", normalMag );
		useNormal = Vector3Ops::Normalize( useNormal );
	}

	// By Snell's law
	Scalar	d = 2.0 * Vector3Ops::Dot(useNormal, vIn);
	return Vector3( vIn.x - d * useNormal.x, vIn.y - d * useNormal.y, vIn.z - d * useNormal.z );
}

bool Optics::CalculateRefractedRay( const Vector3& vNormal, const Scalar Ni, const Scalar Nt, Vector3& vIn )
{
	if( Ni <= NEARZERO || Nt <= NEARZERO ) {
		GlobalLog()->PrintEx( eLog_Error, "Optics::CalculateRefractedRay: Invalid IOR (Ni=%f, Nt=%f). IOR must be > 0", Ni, Nt );
		return false;
	}

	Scalar normalMag = Vector3Ops::Magnitude( vNormal );
	Scalar inputMag = Vector3Ops::Magnitude( vIn );
	if( normalMag < NEARZERO || inputMag < NEARZERO ) {
		GlobalLog()->PrintEx( eLog_Error, "Optics::CalculateRefractedRay: Degenerate vectors (|n|=%e, |vIn|=%e)", normalMag, inputMag );
		return false;
	}

	Vector3	useNormal = vNormal;
	Vector3 useIn = vIn;

	if( fabs(normalMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateRefractedRay: Non-unit normal passed in (|n|=%f), normalizing", normalMag );
		useNormal = Vector3Ops::Normalize( useNormal );
	}
	if( fabs(inputMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateRefractedRay: Non-unit incident vector passed in (|vIn|=%f), normalizing", inputMag );
		useIn = Vector3Ops::Normalize( useIn );
	}

	// Snell's law formula below assumes the standard convention that the
	// surface normal points AGAINST the incoming ray (dot(n, vIn) <= 0):
	//
	//     vIn = s - sqrt(k) * useNormal
	//
	// The final `-sqrt(k) * useNormal` term drives the transmitted ray
	// toward -n, which is where the far-side medium lies when the
	// convention holds.  If the caller passes a normal in the same
	// direction as the incoming ray (a plane whose geometric normal
	// happens to face away from the photon's approach, or a multi-
	// object glass volume where the wrong interface is tagged), that
	// sign assumption fails and the formula produces a ray going
	// *back toward the source*.
	//
	// Flip the normal internally to restore the standard convention.
	// This does not change the physical result: Snell's law is
	// symmetric under n -> -n (both sides of the interface see the
	// same refracted ray).  Callers that already obey the convention
	// are unaffected because the check does nothing.
	if( Vector3Ops::Dot( useNormal, useIn ) > 0 ) {
		useNormal = -useNormal;
	}

	// Use Snell's law
	Scalar		k = Vector3Ops::Dot( useNormal, useIn );
	Vector3	s = (Ni/Nt) * (useIn-k*useNormal);
	k = 1.0 - Vector3Ops::SquaredModulus(s);

	if( k < NEARZERO ) {
		return false;
	} else {
		vIn = s - sqrt(k) * useNormal;
		return true;
	}
}

Scalar Optics::CalculateDielectricReflectance( const Vector3& v, const Vector3& tv, const Vector3& n, const Scalar Ni, const Scalar Nt )
{
	if( Ni <= NEARZERO || Nt <= NEARZERO ) {
		GlobalLog()->PrintEx( eLog_Error, "Optics::CalculateDielectricReflectance: Invalid IOR (Ni=%f, Nt=%f). IOR must be > 0", Ni, Nt );
		return 1.0;
	}

	Scalar normalMag = Vector3Ops::Magnitude( n );
	Scalar inMag = Vector3Ops::Magnitude( v );
	Scalar transMag = Vector3Ops::Magnitude( tv );
	if( normalMag < NEARZERO || inMag < NEARZERO || transMag < NEARZERO ) {
		GlobalLog()->PrintEx( eLog_Error, "Optics::CalculateDielectricReflectance: Degenerate vectors (|n|=%e, |v|=%e, |tv|=%e)", normalMag, inMag, transMag );
		return 1.0;
	}

	Vector3 useN = n;
	Vector3 useV = v;
	Vector3 useTv = tv;
	if( fabs(normalMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateDielectricReflectance: Non-unit normal passed in (|n|=%f), normalizing", normalMag );
		useN = Vector3Ops::Normalize( useN );
	}
	if( fabs(inMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateDielectricReflectance: Non-unit incident vector passed in (|v|=%f), normalizing", inMag );
		useV = Vector3Ops::Normalize( useV );
	}
	if( fabs(transMag - 1.0) > 1e-6 ) {
		GlobalLog()->PrintEx( eLog_Warning, "Optics::CalculateDielectricReflectance: Non-unit transmitted vector passed in (|tv|=%f), normalizing", transMag );
		useTv = Vector3Ops::Normalize( useTv );
	}

	const Scalar cosAi = fabs(Vector3Ops::Dot(useV, useN));
	const Scalar cosAt = fabs(Vector3Ops::Dot(useTv, useN));
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

