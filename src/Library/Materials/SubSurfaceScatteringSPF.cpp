//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringSPF.cpp - Implementation of the surface
//  scattering probability function for BSSRDF-based SSS materials.
//
//  With BSSRDF, the SPF only handles the surface boundary:
//    - From outside: GGX VNDF reflection (rough) or perfect specular
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
#include "../Utilities/MicrofacetUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

/// Dielectric Fresnel reflectance from cosine of incidence angle and IOR.
static Scalar DielectricFresnelCos( const Scalar cosI, const Scalar eta_i, const Scalar eta_t )
{
	const Scalar sinI2 = 1.0 - cosI * cosI;
	const Scalar sinT2 = (eta_i * eta_i) / (eta_t * eta_t) * sinI2;
	if( sinT2 >= 1.0 ) return 1.0;
	const Scalar cosT = sqrt(1.0 - sinT2);

	const Scalar rs = (eta_i * cosI - eta_t * cosT) / (eta_i * cosI + eta_t * cosT);
	const Scalar rp = (eta_t * cosI - eta_i * cosT) / (eta_t * cosI + eta_i * cosT);
	return (rs * rs + rp * rp) * 0.5;
}

SubSurfaceScatteringSPF::SubSurfaceScatteringSPF(
	const IPainter& ior_,
	const IPainter& absorption_,
	const IPainter& scattering_,
	const Scalar g_,
	const Scalar roughness_,
	const bool bAbsorbBackFace_
	) :
  ior( ior_ ),
  absorption( absorption_ ),
  scattering( scattering_ ),
  g( g_ ),
  roughness( roughness_ ),
  alpha( roughness_ * roughness_ ),
  bAbsorbBackFace( bAbsorbBackFace_ )
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
// Scatter (RGB path)
//=============================================================

void SubSurfaceScatteringSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
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
			// Rough surface: VNDF microfacet reflection (non-delta)
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, ri.onb, alpha, u1, u2 );

			// Reflect wi around micronormal m
			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM <= 0 ) return;
			const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );

			const Scalar nWo = Vector3Ops::Dot( ri.onb.w(), wo );
			if( nWo <= 1e-10 ) return;

			// Fresnel at the microfacet normal (not geometric normal)
			// Must match BSDF evaluation which uses F(OdotH)
			const Scalar F = DielectricFresnelCos( wiDotM, Ni, n );

			// With VNDF sampling, the specular estimator simplifies to F * G1(wo)
			// Derivation: f*cos/pdf = (F*D*G)/(4*cosI*cosO) * cosO * (4*cosI)/(G1_i*D) = F*G/G1_i = F*G1_o
			const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, nWo );
			const Scalar weight = F * G1wo;

			if( weight > 1e-10 )
			{
				const Scalar pdf = MicrofacetUtils::VNDF_Pdf( wi, wo, ri.onb.w(), alpha );

				if( pdf > 1e-10 )
				{
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
		// Back-face hit: for BSSRDF materials with no actual medium,
		// absorb the ray (like BioSpecSPF) to prevent artifacts on
		// thin geometry (lips, eyelids).
		if( bAbsorbBackFace ) {
			return;
		}

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
	ISampler& sampler,
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
			// Rough surface: VNDF microfacet reflection (non-delta)
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );

			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, ri.onb, alpha, u1, u2 );

			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM <= 0 ) return;
			const Vector3 wo = Vector3Ops::Normalize( m * (2.0 * wiDotM) - wi );

			const Scalar nWo = Vector3Ops::Dot( ri.onb.w(), wo );
			if( nWo <= 1e-10 ) return;

			// Fresnel at the microfacet normal (matching BSDF)
			const Scalar F = DielectricFresnelCos( wiDotM, Ni, n );
			const Scalar G1wo = MicrofacetUtils::GGX_G1( alpha, nWo );
			const Scalar weight = F * G1wo;

			if( weight > 1e-10 )
			{
				const Scalar pdf = MicrofacetUtils::VNDF_Pdf( wi, wo, ri.onb.w(), alpha );

				if( pdf > 1e-10 )
				{
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
		if( bAbsorbBackFace ) {
			return;
		}

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

			if( Vector3Ops::Dot( woNorm, n ) > 0 && Vector3Ops::Dot( wi, n ) > 0 ) {
				return MicrofacetUtils::VNDF_Pdf( wi, woNorm, n, alpha );
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
