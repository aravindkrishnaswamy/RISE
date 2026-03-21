//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringSPF.cpp - Implementation of the surface
//  scattering probability function for BSSRDF-based SSS materials.
//
//  With BSSRDF, the SPF only handles the surface boundary:
//    - From outside: GGX reflection (rough) or perfect specular
//      reflection (smooth).  No refraction ray is emitted; the
//      integrator handles subsurface entry via BSSRDF sampling.
//    - From inside: delta Fresnel reflection (rare with BSSRDF).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubSurfaceScatteringSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

SubSurfaceScatteringSPF::SubSurfaceScatteringSPF(
	const IPainter& ior_,
	const IPainter& absorption_,
	const IPainter& scattering_,
	const Scalar g_,
	const Scalar roughness_
	) :
  ior( ior_ ),
  absorption( absorption_ ),
  scattering( scattering_ ),
  g( g_ ),
  roughness( roughness_ ),
  alpha( roughness_ * roughness_ )
{
	ior.addref();
	absorption.addref();
	scattering.addref();
}

SubSurfaceScatteringSPF::~SubSurfaceScatteringSPF()
{
	ior.release();
	absorption.release();
	scattering.release();
}

//=============================================================
// GGX microfacet helpers (for rough surface boundary)
//=============================================================

/// GGX (Trowbridge-Reitz) normal distribution function
static Scalar GGX_D( const Scalar alpha, const Scalar cosThetaM )
{
	if( cosThetaM <= 0 ) return 0;
	const Scalar a2 = alpha * alpha;
	const Scalar cos2 = cosThetaM * cosThetaM;
	const Scalar denom = cos2 * (a2 - 1.0) + 1.0;
	return a2 / (PI * denom * denom);
}

/// Smith G1 masking function for GGX
static Scalar GGX_G1( const Scalar alpha, const Vector3& v, const Vector3& n )
{
	const Scalar cosTheta = fabs( Vector3Ops::Dot( v, n ) );
	if( cosTheta < 1e-10 ) return 0;
	const Scalar cos2 = cosTheta * cosTheta;
	const Scalar tan2 = (1.0 - cos2) / cos2;
	return 2.0 / (1.0 + sqrt(1.0 + alpha*alpha*tan2));
}

/// Smith separable masking-shadowing function
static Scalar GGX_G( const Scalar alpha, const Vector3& wi, const Vector3& wo, const Vector3& n )
{
	return GGX_G1( alpha, wi, n ) * GGX_G1( alpha, wo, n );
}

/// Importance-sample a microfacet normal from the GGX distribution
/// Returns the sampled normal in world space
static Vector3 GGX_SampleNormal(
	const Vector3& n,							///< [in] Geometric normal
	const Scalar alpha,							///< [in] GGX alpha parameter
	const RandomNumberGenerator& random			///< [in] RNG
	)
{
	const Scalar xi1 = random.CanonicalRandom();
	const Scalar xi2 = random.CanonicalRandom();

	// Sample theta from GGX distribution: D(m) * cos(theta_m)
	const Scalar a2 = alpha * alpha;
	Scalar cosThetaM = sqrt( (1.0 - xi1) / (1.0 + (a2 - 1.0) * xi1) );
	if( cosThetaM > 1.0 ) cosThetaM = 1.0;
	const Scalar sinThetaM = sqrt( r_max( 0.0, 1.0 - cosThetaM * cosThetaM ) );
	const Scalar phiM = TWO_PI * xi2;

	// Build ONB from geometric normal and convert to world space
	OrthonormalBasis3D onb;
	onb.CreateFromW( n );
	return Vector3Ops::Normalize(
		onb.u() * (sinThetaM * cos(phiM)) +
		onb.v() * (sinThetaM * sin(phiM)) +
		onb.w() * cosThetaM
	);
}

/// Reflection PDF: D(h) * |n.h| / (4 * |wo.h|)
static Scalar GGX_ReflectionPdf( const Scalar alpha, const Vector3& h, const Vector3& wo, const Vector3& n )
{
	const Scalar cosThetaH = fabs( Vector3Ops::Dot( h, n ) );
	const Scalar woH = fabs( Vector3Ops::Dot( wo, h ) );
	if( woH < 1e-10 ) return 0;
	return GGX_D( alpha, cosThetaH ) * cosThetaH / (4.0 * woH);
}


//=============================================================
// Scatter (RGB path)
//=============================================================

void SubSurfaceScatteringSPF::Scatter(
	const RayIntersectionGeometric& ri,
	const RandomNumberGenerator& random,
	ScatteredRayContainer& scattered,
	const IORStack* const ior_stack
	) const
{
	const Scalar cosine = -Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() );
	const RISEPel iorVal = ior.GetColor( ri );
	const Scalar n = iorVal[0];

	const bool bFromInside = ior_stack ? ior_stack->containsCurrent() : (cosine < NEARZERO);

	if( !bFromInside )
	{
		//
		// Front face hit: surface reflection only.
		// The subsurface entry is handled by BSSRDF importance
		// sampling in the integrator, not by the SPF.
		//

		const Scalar Ni = ior_stack ? ior_stack->top() : 1.0;

		// Compute Fresnel reflectance at the boundary
		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( ri.onb.w(), Ni, n, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, ri.onb.w(), Ni, n );
		} else {
			R = 1.0;
		}

		if( alpha > 1e-6 )
		{
			// Rough surface: GGX microfacet reflection (non-delta)
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			Vector3 m = GGX_SampleNormal( ri.onb.w(), alpha, random );
			if( Vector3Ops::Dot( m, wi ) < 0 ) m = -m;

			const Vector3 wo = Optics::CalculateReflectedRay( ri.ray.Dir(), (-m) );
			const Scalar nWi = fabs( Vector3Ops::Dot( ri.onb.w(), wi ) );
			const Scalar nWo = fabs( Vector3Ops::Dot( ri.onb.w(), wo ) );
			const Scalar woH = fabs( Vector3Ops::Dot( wo, m ) );
			const Scalar cosThetaM = fabs( Vector3Ops::Dot( m, ri.onb.w() ) );

			if( nWo > 1e-10 && woH > 1e-10 && cosThetaM > 1e-10 ) {
				const Scalar G = GGX_G( alpha, wi, wo, ri.onb.w() );
				const Scalar pdf = GGX_ReflectionPdf( alpha, m, wo, ri.onb.w() );
				const Scalar weight = R * G * woH / (nWi * cosThetaM);

				if( weight > 1e-10 && pdf > 1e-10 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = false;
					reflectedRay.pdf = pdf;
					reflectedRay.ray = Ray( ri.ptIntersection, wo );
					reflectedRay.kray = RISEPel( weight, weight, weight );

					scattered.AddScatteredRay( reflectedRay );
				}
			}
		}
		else
		{
			// Smooth surface: perfect specular reflection (delta)
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			reflectedRay.kray = RISEPel( R, R, R );

			scattered.AddScatteredRay( reflectedRay );
		}
	}
	else
	{
		//
		// Back face hit (from inside): should not normally occur
		// with BSSRDF (no volumetric random walk), but handle
		// gracefully for BDPT light subpaths that may enter the
		// medium from behind.  Emit delta Fresnel reflection.
		//

		const Scalar Nt = ior_stack ? ior_stack->top() : 1.0;

		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		// Delta reflection back into the medium
		ScatteredRay reflectedRay;
		reflectedRay.type = ScatteredRay::eRayReflection;
		reflectedRay.isDelta = true;
		reflectedRay.pdf = 1.0;
		reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
		reflectedRay.kray = RISEPel( R, R, R );

		if( ior_stack ) {
			reflectedRay.ior_stack = new IORStack( *ior_stack );
			GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
		}

		scattered.AddScatteredRay( reflectedRay );

		// Also emit exit refraction if possible (for light subpaths
		// that need to escape the medium)
		if( R < 1.0 )
		{
			ScatteredRay exitRay;
			exitRay.type = ScatteredRay::eRayRefraction;
			exitRay.isDelta = true;
			exitRay.pdf = 1.0;
			exitRay.ray = Ray( ri.ptIntersection, refracted );
			exitRay.kray = RISEPel( 1.0-R, 1.0-R, 1.0-R );

			if( ior_stack ) {
				exitRay.ior_stack = new IORStack( *ior_stack );
				exitRay.ior_stack->pop();
				GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( exitRay );
		}
	}
}

//=============================================================
// ScatterNM (spectral path)
//=============================================================

void SubSurfaceScatteringSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	const RandomNumberGenerator& random,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack* const ior_stack
	) const
{
	const Scalar cosine = -Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() );
	const Scalar n = ior.GetColorNM( ri, nm );

	const bool bFromInside = ior_stack ? ior_stack->containsCurrent() : (cosine < NEARZERO);

	if( !bFromInside )
	{
		//
		// Front face hit: surface reflection only.
		//

		const Scalar Ni = ior_stack ? ior_stack->top() : 1.0;

		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( ri.onb.w(), Ni, n, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, ri.onb.w(), Ni, n );
		} else {
			R = 1.0;
		}

		if( alpha > 1e-6 )
		{
			// Rough surface: GGX microfacet reflection (non-delta)
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			Vector3 m = GGX_SampleNormal( ri.onb.w(), alpha, random );
			if( Vector3Ops::Dot( m, wi ) < 0 ) m = -m;

			const Vector3 wo = Optics::CalculateReflectedRay( ri.ray.Dir(), (-m) );
			const Scalar nWi = fabs( Vector3Ops::Dot( ri.onb.w(), wi ) );
			const Scalar woH = fabs( Vector3Ops::Dot( wo, m ) );
			const Scalar cosThetaM = fabs( Vector3Ops::Dot( m, ri.onb.w() ) );
			const Scalar nWo = fabs( Vector3Ops::Dot( ri.onb.w(), wo ) );

			if( nWo > 1e-10 && woH > 1e-10 && cosThetaM > 1e-10 ) {
				const Scalar G = GGX_G( alpha, wi, wo, ri.onb.w() );
				const Scalar pdf = GGX_ReflectionPdf( alpha, m, wo, ri.onb.w() );
				const Scalar weight = R * G * woH / (nWi * cosThetaM);

				if( weight > 1e-10 && pdf > 1e-10 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = false;
					reflectedRay.pdf = pdf;
					reflectedRay.ray = Ray( ri.ptIntersection, wo );
					reflectedRay.krayNM = weight;

					scattered.AddScatteredRay( reflectedRay );
				}
			}
		}
		else
		{
			// Smooth surface: perfect specular reflection (delta)
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			reflectedRay.krayNM = R;

			scattered.AddScatteredRay( reflectedRay );
		}
	}
	else
	{
		//
		// Back face hit (from inside): delta reflection + exit refraction
		//

		const Scalar Nt = ior_stack ? ior_stack->top() : 1.0;

		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		ScatteredRay reflectedRay;
		reflectedRay.type = ScatteredRay::eRayReflection;
		reflectedRay.isDelta = true;
		reflectedRay.pdf = 1.0;
		reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
		reflectedRay.krayNM = R;

		if( ior_stack ) {
			reflectedRay.ior_stack = new IORStack( *ior_stack );
			GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
		}

		scattered.AddScatteredRay( reflectedRay );

		if( R < 1.0 )
		{
			ScatteredRay exitRay;
			exitRay.type = ScatteredRay::eRayRefraction;
			exitRay.isDelta = true;
			exitRay.pdf = 1.0;
			exitRay.ray = Ray( ri.ptIntersection, refracted );
			exitRay.krayNM = 1.0 - R;

			if( ior_stack ) {
				exitRay.ior_stack = new IORStack( *ior_stack );
				exitRay.ior_stack->pop();
				GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( exitRay );
		}
	}
}

//=============================================================
// PDF evaluation
//=============================================================

Scalar SubSurfaceScatteringSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const Scalar cosine = -Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() );
	const bool bFromInside = ior_stack ? ior_stack->containsCurrent() : (cosine < NEARZERO);

	if( !bFromInside )
	{
		// From outside: only rough reflection has non-zero PDF
		if( alpha > 1e-6 )
		{
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			const Vector3 woNorm = Vector3Ops::Normalize( wo );
			const Vector3 n = ri.onb.w();

			// Only reflection (same hemisphere as wi relative to n)
			if( Vector3Ops::Dot( woNorm, n ) > 0 && Vector3Ops::Dot( wi, n ) > 0 ) {
				const Vector3 h = Vector3Ops::Normalize( wi + woNorm );
				return GGX_ReflectionPdf( alpha, h, woNorm, n );
			}
		}
		return 0;
	}

	// From inside: all rays are delta, PDF = 0
	return 0;
}

Scalar SubSurfaceScatteringSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}
