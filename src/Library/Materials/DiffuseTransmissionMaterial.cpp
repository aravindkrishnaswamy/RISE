//////////////////////////////////////////////////////////////////////
//
//  DiffuseTransmissionMaterial.cpp - Implementation of the thin
//    two-sided diffuse reflection + diffuse transmission material.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DiffuseTransmissionMaterial.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/FiniteMath.h"
#include <algorithm>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Evaluate RGB reflectance and transmittance, clamping negative/non-finite
	//! channels to 0 and scaling any channel where R + T > 1 back onto the unit
	//! simplex so hemispherical reflection + transmission never exceeds 1.
	inline void EvalConservedPel(
		const IPainter& pRefl,
		const IPainter& pTrans,
		const RayIntersectionGeometric& ri,
		RISEPel& outR,
		RISEPel& outT )
	{
		const RISEPel rawR = ReflectanceColor( pRefl, ri );
		const RISEPel rawT = ReflectanceColor( pTrans, ri );
		for( int c = 0; c < 3; ++c ) {
			Scalar r = ( IsFiniteDouble( rawR[c] ) && rawR[c] > Scalar(0) ) ? rawR[c] : Scalar(0);
			Scalar t = ( IsFiniteDouble( rawT[c] ) && rawT[c] > Scalar(0) ) ? rawT[c] : Scalar(0);
			const Scalar sum = r + t;
			if( sum > Scalar(1) ) {
				const Scalar inv = Scalar(1) / sum;
				r *= inv;
				t *= inv;
			}
			outR[c] = r;
			outT[c] = t;
		}
	}

	//! Spectral twin of EvalConservedPel.  Crucial for Round 3 P1-13:
	//! RGB R + T <= 1 does NOT guarantee uplifted spectra R(nm) + T(nm) <= 1
	//! at every wavelength, so conservation is enforced per wavelength here.
	inline void EvalConservedNM(
		const IPainter& pRefl,
		const IPainter& pTrans,
		const RayIntersectionGeometric& ri,
		const Scalar nm,
		Scalar& outR,
		Scalar& outT )
	{
		Scalar r = ReflectanceColorNM( pRefl, ri, nm );
		Scalar t = ReflectanceColorNM( pTrans, ri, nm );
		if( !IsFiniteDouble( r ) || !( r > Scalar(0) ) ) r = Scalar(0);
		if( !IsFiniteDouble( t ) || !( t > Scalar(0) ) ) t = Scalar(0);
		const Scalar sum = r + t;
		if( sum > Scalar(1) ) {
			const Scalar inv = Scalar(1) / sum;
			r *= inv;
			t *= inv;
		}
		outR = r;
		outT = t;
	}

	// Derive wavelength-independent broadband lobe probabilities (pRefl, pTrans)
	// for spectral (NM / HWSS) sampling so every hero wavelength keeps positive
	// sampling density on both hemispheres whenever either painter carries energy.
	// This prevents HWSS energy-loss bias on spectrally disjoint R and T, prevents
	// hero-zero aborts from killing live companion lanes, and eliminates sigmoid-
	// tail fireflies on saturated complementary RGB colors, while preserving
	// pRefl == 0 for pure Translucent BSDFs (R == 0).
	bool EvalSpectralLobeProbs(
		const IPainter& pRefl,
		const IPainter& pTrans,
		const RayIntersectionGeometric& ri,
		const Scalar rNM,
		const Scalar tNM,
		Scalar& pReflOut,
		Scalar& pTransOut )
	{
		RISEPel R, T;
		EvalConservedPel( pRefl, pTrans, ri, R, T );
		Scalar wR = ColorMath::MaxValue( R );
		Scalar wT = ColorMath::MaxValue( T );
		if( wR <= NEARZERO && rNM > NEARZERO ) wR = rNM;
		if( wT <= NEARZERO && tNM > NEARZERO ) wT = tNM;
		const Scalar wSum = wR + wT;
		if( wSum <= NEARZERO ) {
			pReflOut = Scalar(0);
			pTransOut = Scalar(0);
			return false;
		}
		pReflOut = wR / wSum;
		pTransOut = wT / wSum;
		return true;
	}

	enum SideClass { eSideNone = 0, eSideReflect = 1, eSideTransmit = 2 };

	//! Symmetric both-directions hemisphere classifier with geometric-horizon
	//! guard.  Works identically whether `ri.ray` arrives from front or back
	//! without flipping `ri.onb.w()` (Round 3 P1-13 BDPT/VCM re-aim).
	inline SideClass ClassifySides(
		const Vector3& vLightIn,
		const RayIntersectionGeometric& ri )
	{
		const Vector3 n = ri.onb.w();
		const Vector3 wi = Vector3Ops::Normalize( vLightIn );
		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );

		const Scalar cosO = Vector3Ops::Dot( n, wo );
		const Scalar cosI = Vector3Ops::Dot( n, wi );
		if( fabs( cosO ) <= NEARZERO || fabs( cosI ) <= NEARZERO ) {
			return eSideNone;
		}

		// Geometric-horizon anchor oriented to wo (same convention as LambertianBRDF).
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, wo ) >= 0 ) ? geomNRaw : -geomNRaw;

		if( Vector3Ops::Dot( wo, geomN ) <= 0 ) {
			return eSideNone;
		}

		const bool sameSide = ( cosO * cosI ) > Scalar(0);
		if( sameSide ) {
			return ( Vector3Ops::Dot( wi, geomN ) > 0 ) ? eSideReflect : eSideNone;
		}
		return ( Vector3Ops::Dot( wi, geomN ) < 0 ) ? eSideTransmit : eSideNone;
	}

	inline Vector3 GeometricNormalForRay( const RayIntersectionGeometric& ri, const Vector3& nEff )
	{
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : nEff;
		return ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	}
}

//////////////////////////////////////////////////////////////////////
// DiffuseTransmissionBSDF
//////////////////////////////////////////////////////////////////////

DiffuseTransmissionBSDF::DiffuseTransmissionBSDF(
	const IPainter& reflectance,
	const IPainter& transmittance ) :
  pReflectance( &reflectance ),
  pTransmittance( &transmittance )
{
	pReflectance->addref();
	pTransmittance->addref();
}

DiffuseTransmissionBSDF::~DiffuseTransmissionBSDF()
{
	safe_release( pReflectance );
	safe_release( pTransmittance );
}

RISEPel DiffuseTransmissionBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const SideClass side = ClassifySides( vLightIn, ri );
	if( side == eSideNone ) {
		return RISEPel( 0, 0, 0 );
	}
	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	return ( side == eSideReflect ) ? ( R * INV_PI ) : ( T * INV_PI );
}

Scalar DiffuseTransmissionBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const SideClass side = ClassifySides( vLightIn, ri );
	if( side == eSideNone ) {
		return Scalar(0);
	}
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	return ( side == eSideReflect ) ? ( r * INV_PI ) : ( t * INV_PI );
}

bool DiffuseTransmissionBSDF::valueByScatterType(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const IORStack*,
	RISEPel out[5] ) const
{
	for( int i = 0; i < 5; ++i ) {
		out[i] = RISEPel( 0, 0, 0 );
	}
	const SideClass side = ClassifySides( vLightIn, ri );
	if( side == eSideNone ) {
		return true;
	}
	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	if( side == eSideReflect ) {
		out[ ScatteredRay::eRayDiffuse ] = R * INV_PI;
	} else {
		out[ ScatteredRay::eRayTranslucent ] = T * INV_PI;
	}
	return true;
}

bool DiffuseTransmissionBSDF::valueByScatterTypeNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const IORStack*,
	Scalar out[5] ) const
{
	for( int i = 0; i < 5; ++i ) {
		out[i] = Scalar(0);
	}
	const SideClass side = ClassifySides( vLightIn, ri );
	if( side == eSideNone ) {
		return true;
	}
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	if( side == eSideReflect ) {
		out[ ScatteredRay::eRayDiffuse ] = r * INV_PI;
	} else {
		out[ ScatteredRay::eRayTranslucent ] = t * INV_PI;
	}
	return true;
}

RISEPel DiffuseTransmissionBSDF::albedo( const RayIntersectionGeometric& ri ) const
{
	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	return R + T;
}

bool DiffuseTransmissionBSDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	out = R + T;
	return true;
}

bool DiffuseTransmissionBSDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	out = r + t;
	return true;
}

void DiffuseTransmissionBSDF::SetReflectance( const IPainter& reflectance )
{
	reflectance.addref();
	safe_release( pReflectance );
	pReflectance = &reflectance;
}

void DiffuseTransmissionBSDF::SetTransmittance( const IPainter& transmittance )
{
	transmittance.addref();
	safe_release( pTransmittance );
	pTransmittance = &transmittance;
}

//////////////////////////////////////////////////////////////////////
// DiffuseTransmissionSPF
//////////////////////////////////////////////////////////////////////

DiffuseTransmissionSPF::DiffuseTransmissionSPF(
	const IPainter& reflectance,
	const IPainter& transmittance ) :
  pReflectance( &reflectance ),
  pTransmittance( &transmittance )
{
	pReflectance->addref();
	pTransmittance->addref();
}

DiffuseTransmissionSPF::~DiffuseTransmissionSPF()
{
	safe_release( pReflectance );
	safe_release( pTransmittance );
}

void DiffuseTransmissionSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ) const
{
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar cosO = Vector3Ops::Dot( ri.onb.w(), wo );
	if( fabs( cosO ) <= NEARZERO ) {
		return;
	}

	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	const Scalar wR = ColorMath::MaxValue( R );
	const Scalar wT = ColorMath::MaxValue( T );
	const Scalar wSum = wR + wT;
	if( wSum <= NEARZERO ) {
		return;
	}

	const bool rayEntersBack = cosO < Scalar(0);
	OrthonormalBasis3D reflOnb = ri.onb;
	OrthonormalBasis3D transOnb = ri.onb;
	if( rayEntersBack ) {
		reflOnb.FlipW();
	} else {
		transOnb.FlipW();
	}

	const Vector3 geomN = GeometricNormalForRay( ri, reflOnb.w() );
	if( Vector3Ops::Dot( wo, geomN ) <= Scalar(0) ) {
		return;
	}

	// Single-lobe up-front selection so the emitted ray's marginal PDF matches
	// Pdf() exactly even when a tilted normal map rejects rays against geomN.
	const Scalar pRefl = wR / wSum;
	const Scalar pTrans = wT / wSum;
	const bool chooseRefl = ( sampler.Get1D() < pRefl );
	const Point2 xi( sampler.Get1D(), sampler.Get1D() );

	if( chooseRefl && pRefl > Scalar(0) ) {
		ScatteredRay refl;
		refl.type = ScatteredRay::eRayDiffuse;
		const Vector3 dir = GeometricUtilities::CreateDiffuseVector( reflOnb, xi );
		const Scalar absCos = fabs( Vector3Ops::Dot( dir, ri.onb.w() ) );
		if( absCos > NEARZERO && Vector3Ops::Dot( dir, geomN ) > Scalar(0) ) {
			refl.ray.Set( ri.ptIntersection, dir );
			refl.kray = R * ( Scalar(1) / pRefl );
			refl.pdf = pRefl * absCos * INV_PI;
			refl.isDelta = false;
			scattered.AddScatteredRay( refl );
		}
	} else if( pTrans > Scalar(0) ) {
		ScatteredRay trans;
		trans.type = ScatteredRay::eRayTranslucent;
		const Vector3 dir = GeometricUtilities::CreateDiffuseVector( transOnb, xi );
		const Scalar absCos = fabs( Vector3Ops::Dot( dir, ri.onb.w() ) );
		if( absCos > NEARZERO && Vector3Ops::Dot( dir, geomN ) < Scalar(0) ) {
			trans.ray.Set( ri.ptIntersection, dir );
			trans.kray = T * ( Scalar(1) / pTrans );
			trans.pdf = pTrans * absCos * INV_PI;
			trans.isDelta = false;
			scattered.AddScatteredRay( trans );
		}
	}
}

void DiffuseTransmissionSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ) const
{
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar cosO = Vector3Ops::Dot( ri.onb.w(), wo );
	if( fabs( cosO ) <= NEARZERO ) {
		return;
	}

	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	Scalar pRefl = Scalar(0), pTrans = Scalar(0);
	if( !EvalSpectralLobeProbs( *pReflectance, *pTransmittance, ri, r, t, pRefl, pTrans ) ) {
		return;
	}

	const bool rayEntersBack = cosO < Scalar(0);
	OrthonormalBasis3D reflOnb = ri.onb;
	OrthonormalBasis3D transOnb = ri.onb;
	if( rayEntersBack ) {
		reflOnb.FlipW();
	} else {
		transOnb.FlipW();
	}

	const Vector3 geomN = GeometricNormalForRay( ri, reflOnb.w() );
	if( Vector3Ops::Dot( wo, geomN ) <= Scalar(0) ) {
		return;
	}

	const bool chooseRefl = ( sampler.Get1D() < pRefl );
	const Point2 xi( sampler.Get1D(), sampler.Get1D() );

	if( chooseRefl && pRefl > Scalar(0) ) {
		ScatteredRay refl;
		refl.type = ScatteredRay::eRayDiffuse;
		const Vector3 dir = GeometricUtilities::CreateDiffuseVector( reflOnb, xi );
		const Scalar absCos = fabs( Vector3Ops::Dot( dir, ri.onb.w() ) );
		if( absCos > NEARZERO && Vector3Ops::Dot( dir, geomN ) > Scalar(0) ) {
			refl.ray.Set( ri.ptIntersection, dir );
			refl.krayNM = r / pRefl;
			refl.pdf = pRefl * absCos * INV_PI;
			refl.isDelta = false;
			scattered.AddScatteredRay( refl );
		}
	} else if( pTrans > Scalar(0) ) {
		ScatteredRay trans;
		trans.type = ScatteredRay::eRayTranslucent;
		const Vector3 dir = GeometricUtilities::CreateDiffuseVector( transOnb, xi );
		const Scalar absCos = fabs( Vector3Ops::Dot( dir, ri.onb.w() ) );
		if( absCos > NEARZERO && Vector3Ops::Dot( dir, geomN ) < Scalar(0) ) {
			trans.ray.Set( ri.ptIntersection, dir );
			trans.krayNM = t / pTrans;
			trans.pdf = pTrans * absCos * INV_PI;
			trans.isDelta = false;
			scattered.AddScatteredRay( trans );
		}
	}
}

Scalar DiffuseTransmissionSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ) const
{
	const SideClass side = ClassifySides( wo, ri );
	if( side == eSideNone ) {
		return Scalar(0);
	}
	RISEPel R, T;
	EvalConservedPel( *pReflectance, *pTransmittance, ri, R, T );
	const Scalar wR = ColorMath::MaxValue( R );
	const Scalar wT = ColorMath::MaxValue( T );
	const Scalar wSum = wR + wT;
	if( wSum <= NEARZERO ) {
		return Scalar(0);
	}
	const Scalar lobeProb = ( side == eSideReflect ) ? ( wR / wSum ) : ( wT / wSum );
	if( lobeProb <= Scalar(0) ) {
		return Scalar(0);
	}
	const Scalar cosO = fabs( Vector3Ops::Dot( Vector3Ops::Normalize( wo ), ri.onb.w() ) );
	return lobeProb * cosO * INV_PI;
}

Scalar DiffuseTransmissionSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ) const
{
	const SideClass side = ClassifySides( wo, ri );
	if( side == eSideNone ) {
		return Scalar(0);
	}
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	Scalar pRefl = Scalar(0), pTrans = Scalar(0);
	if( !EvalSpectralLobeProbs( *pReflectance, *pTransmittance, ri, r, t, pRefl, pTrans ) ) {
		return Scalar(0);
	}
	const Scalar lobeProb = ( side == eSideReflect ) ? pRefl : pTrans;
	if( lobeProb <= Scalar(0) ) {
		return Scalar(0);
	}
	const Scalar cosO = fabs( Vector3Ops::Dot( Vector3Ops::Normalize( wo ), ri.onb.w() ) );
	return lobeProb * cosO * INV_PI;
}

Scalar DiffuseTransmissionSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	const ScatteredRay::ScatRayType rayType,
	const Scalar nm,
	const IORStack& ) const
{
	const SideClass side = ClassifySides( outDir, ri );
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return ( side == eSideReflect ) ? ( r * INV_PI ) : Scalar(0);
	}
	if( rayType == ScatteredRay::eRayTranslucent ) {
		return ( side == eSideTransmit ) ? ( t * INV_PI ) : Scalar(0);
	}
	return Scalar(-1);
}

Scalar DiffuseTransmissionSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	const ScatteredRay::ScatRayType rayType,
	const Scalar nm,
	const IORStack& ) const
{
	// Matches ScatterNM / PdfNM broadband lobe probabilities:
	//   pdfNM = lobeProb * |cos_o| * INV_PI, so f_lobe * |cos_o| / pdfNM = (r or t) / lobeProb.
	const SideClass side = ClassifySides( outDir, ri );
	Scalar r, t;
	EvalConservedNM( *pReflectance, *pTransmittance, ri, nm, r, t );
	Scalar pRefl = Scalar(0), pTrans = Scalar(0);
	EvalSpectralLobeProbs( *pReflectance, *pTransmittance, ri, r, t, pRefl, pTrans );
	if( rayType == ScatteredRay::eRayDiffuse ) {
		return ( side == eSideReflect && pRefl > Scalar(0) ) ? ( r / pRefl ) : Scalar(0);
	}
	if( rayType == ScatteredRay::eRayTranslucent ) {
		return ( side == eSideTransmit && pTrans > Scalar(0) ) ? ( t / pTrans ) : Scalar(0);
	}
	return Scalar(-1);
}

void DiffuseTransmissionSPF::SetReflectance( const IPainter& reflectance )
{
	reflectance.addref();
	safe_release( pReflectance );
	pReflectance = &reflectance;
}

void DiffuseTransmissionSPF::SetTransmittance( const IPainter& transmittance )
{
	transmittance.addref();
	safe_release( pTransmittance );
	pTransmittance = &transmittance;
}

//////////////////////////////////////////////////////////////////////
// DiffuseTransmissionMaterial
//////////////////////////////////////////////////////////////////////

DiffuseTransmissionMaterial::DiffuseTransmissionMaterial(
	const IPainter& reflectance,
	const IPainter& transmittance )
{
	pBSDF = new DiffuseTransmissionBSDF( reflectance, transmittance );
	GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "DiffuseTransmissionBSDF" );
	pSPF = new DiffuseTransmissionSPF( reflectance, transmittance );
	GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "DiffuseTransmissionSPF" );
}

DiffuseTransmissionMaterial::~DiffuseTransmissionMaterial()
{
	safe_release( pBSDF );
	safe_release( pSPF );
}
