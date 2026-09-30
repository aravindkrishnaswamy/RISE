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
#include "FiniteMath.h"
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
	if( !IsFiniteDouble(Ni) || !IsFiniteDouble(Nt) || Ni <= NEARZERO || Nt <= NEARZERO ) {
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

	// Equal media have no boundary, even when the incidence cosine is zero.
	if( Ni == Nt ) {
		vIn = useIn;
		return true;
	}
	const Scalar k = Vector3Ops::Dot( useNormal, useIn );
	Scalar cosT;
	if( !CalculateRefractedCosine( -k, Ni, Nt, cosT ) ) return false;
	const Vector3 tangent = (Ni/Nt) * (useIn-k*useNormal);
	vIn = tangent - cosT * useNormal;
	return true;
}

// Error-free residuals below are meaningful only without reassociation or
// implicit contraction. Keep that local guarantee even in shipping fast-math.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-fast-math")))
#endif
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
bool Optics::CalculateRefractedCosine( Scalar cosI, Scalar Ni, Scalar Nt, Scalar& cosT )
{
#if defined(__clang__)
#pragma clang fp reassociate(off)
#pragma clang fp contract(off)
#endif
	if( !IsFiniteDouble(cosI) || !IsFiniteDouble(Ni) || !IsFiniteDouble(Nt) || Ni <= 0 || Nt <= 0 ) return false;
	cosI = fmin( 1.0, fabs(cosI) );
	if( Ni == Nt ) { cosT = cosI; return true; }
	if( cosI == 1.0 ) { cosT = 1.0; return true; }
	if( Ni < Nt ) {
		// Keep the small grazing cosine instead of subtracting it from one.
		const Scalar r = Ni/Nt;
		const Scalar rc = r*cosI;
		cosT = sqrt( (1-r)*(1+r) + rc*rc );
	} else {
		// cosT^2 = (cosI^2 + (Nt/Ni)^2 - 1) / (Nt/Ni)^2.
		// Near critical, a rounded ratio or rounded sinT can erase the sign
		// and the small positive cosine. Retain the quotient residual and
		// both square residuals, then add with error-free TwoSum steps.
		// All quantities are scaled to <= 1, avoiding index-square overflow.
		const Scalar q = Nt/Ni;
		const Scalar qLow = std::fma(-q, Ni, Nt)/Ni;
		const Scalar qSquare = q*q;
		const Scalar qError = std::fma(q, q, -qSquare) + 2*q*qLow + qLow*qLow;
		const Scalar muSquare = cosI*cosI;
		const Scalar muError = std::fma(cosI, cosI, -muSquare);
		const Scalar a = qSquare - 1;
		const Scalar virtualMinusOne = a - qSquare;
		const Scalar aError = (qSquare - (a - virtualMinusOne)) + (-1 - virtualMinusOne);
		const Scalar high = a + muSquare;
		const Scalar virtualMuSquare = high - a;
		const Scalar sumError = (a - (high - virtualMuSquare)) + (muSquare - virtualMuSquare);
		const Scalar discriminant = high + (aError + sumError + qError + muError);
		if( discriminant < 0 ) return false;
		cosT = sqrt(discriminant)/q;
	}
	return true;
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

Scalar Optics::CalculateDielectricReflectanceCosine( Scalar cosI, Scalar Ni, Scalar Nt )
{
	Scalar cosT;
	if( !CalculateRefractedCosine(cosI,Ni,Nt,cosT) ) return 1.0;
	if( Ni == Nt ) return 0.0;
	cosI = fmin( 1.0, fabs(cosI) );
	const Scalar cosScale = fmax(cosI,cosT);
	if( cosScale == 0.0 ) return 1.0;
	const Scalar indexScale = fmax(Ni,Nt);
	const Scalar ni = Ni/indexScale, nt = Nt/indexScale;
	const Scalar ci = cosI/cosScale, ct = cosT/cosScale;
	const Scalar rs = (ni*ci-nt*ct)/(ni*ci+nt*ct);
	const Scalar rp = (nt*ci-ni*ct)/(nt*ci+ni*ct);
	return fmin( 1.0, 0.5*(rs*rs+rp*rp) );
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
	}

	// Identical media have no interface, including at exact grazing.
	// Keep this identity explicit rather than evaluating its 0/0 limit.
	if( Ni == Nt ) {
		return 0.0;
	}

	// Fresnel is determined by the incident cosine and ordered absolute
	// indices. Reconstruct cosT with the same conditioned Snell kernel as
	// refraction rather than recovering it from a tangent vector subtraction.
	// `tv` remains validated above for the existing API contract.
	return CalculateDielectricReflectanceCosine(
		fabs(Vector3Ops::Dot(useV, useN)), Ni, Nt );
}

