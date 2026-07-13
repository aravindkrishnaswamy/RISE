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
	const IScalarPainter& ior_,
	const Scalar g_,
	const Scalar roughness_,
	const bool bAbsorbBackFace_
	) :
  pIOR( &ior_ ),
  g( g_ ),
  roughness( roughness_ ),
  alpha( roughness_ * roughness_ ),
  bAbsorbBackFace( bAbsorbBackFace_ )
{
	pIOR->addref();
}

SubSurfaceScatteringSPF::~SubSurfaceScatteringSPF()
{
	safe_release( pIOR );
}

void SubSurfaceScatteringSPF::SetIOR( const IScalarPainter& v )
{
	v.addref();
	safe_release( pIOR );
	pIOR = &v;
}

//=============================================================
// Scatter (RGB path)
//=============================================================

void SubSurfaceScatteringSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	const ScalarTriple iorVal = pIOR->GetValuesAt( ri );
	const Scalar n = iorVal.v[0];

	const bool bFromInside = ior_stack.containsCurrent();

	if( !bFromInside )
	{
		//
		// Front face hit: surface reflection only.
		// The subsurface entry is handled by BSSRDF importance
		// sampling in the integrator, not by the SPF.
		//

		const Scalar Ni = ior_stack.top();

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

			// Geometric-horizon gate: GlintModifier can tilt the shading normal
			// up to 60 deg off the true surface, so a wo that validates against
			// the (tilted) shading normal can still point below the geometric
			// surface -- the continuation ray then tunnels into the solid.
			// Orient the geometric normal to the front-face side (ri.onb.w(),
			// the normal this reflection was sampled around).  Degenerate
			// vGeomNormal (SquaredModulus guard, matches GlintModifier.cpp)
			// falls back to the shading normal, making the gate a no-op (the
			// orientation itself is anchored to ri.ray.Dir(), not to the
			// shading normal, so a glint tilt cannot flip the gate to the
			// wrong side).  Drop (no sibling lobe; BSSRDF entry is handled
			// by the integrator) --
			// UNLESS the reflection is MANDATORY (R >= 1.0, i.e. TIR at the
			// entry boundary, reachable when the SSS medium's ambient IOR
			// exceeds its own): there is no refraction channel here at all
			// (BSSRDF entry is the integrator's business, not emitted by this
			// SPF), so dropping would be total, deterministic energy loss.
			// Fall back to a single delta reflection about the TRUE geometric
			// normal instead of the VNDF-sampled wo -- guaranteed to satisfy
			// the gate (no re-check needed): for a ray arriving against geomN,
			// dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 -- holds
			// unconditionally: geomN is ray-anchored, so dot(d,geomN) < 0 always.
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : ri.onb.w();
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( wo, geomN ) <= 0 ) {
				if( R >= 1.0 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = true;
					reflectedRay.pdf = 1.0;
					reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
					reflectedRay.kray = RISEPel( R, R, R );

					scattered.AddScatteredRay( reflectedRay );
				}
				return;
			}

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
			Vector3 rvDir = Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() );

			// Geometric-horizon gate: derived-direction delta lobe -- compute
			// the reflection direction first, gate it, and drop the lobe
			// entirely on failure (no redistribution) UNLESS the reflection is
			// MANDATORY (R >= 1.0, i.e. TIR at the entry boundary, reachable
			// when the SSS medium's ambient IOR exceeds its own): there is no
			// refraction channel here at all (BSSRDF entry is the
			// integrator's business, not emitted by this SPF), so dropping
			// here would be total, deterministic energy loss.  Re-derive the
			// reflection direction about the TRUE geometric normal instead of
			// the shading normal -- guaranteed to satisfy the gate (no
			// re-check needed): for a ray arriving against geomN,
			// dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 -- holds
			// unconditionally: geomN is ray-anchored, so dot(d,geomN) < 0 always.
			// The reflect formula is sign-invariant in its normal argument, so
			// the reflection physically lands on the incoming ray's side.
			const Vector3 nRef = ri.onb.w();
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : nRef;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

			bool bEmit = Vector3Ops::Dot( rvDir, geomN ) > 0;
			if( !bEmit && R >= 1.0 ) {
				rvDir = Optics::CalculateReflectedRay( ri.ray.Dir(), geomN );
				bEmit = true;
			}

			if( bEmit )
			{
				ScatteredRay reflectedRay;
				reflectedRay.type = ScatteredRay::eRayReflection;
				reflectedRay.isDelta = true;
				reflectedRay.pdf = 1.0;
				reflectedRay.ray = Ray( ri.ptIntersection, rvDir );
				reflectedRay.kray = RISEPel( R, R, R );

				scattered.AddScatteredRay( reflectedRay );
			}
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

		const Scalar Nt = ior_stack.top();

		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		// Delta reflection back into the medium
		Vector3 rvDirBack = Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() );

		// Geometric-horizon gate (sign flip vs the front smooth-reflection
		// branch: the ray arrives from inside here, so the reflection back
		// into the medium lands on the -ri.onb.w() side).  Drop on fail
		// (no redistribution) UNLESS the reflection is MANDATORY (R >= 1.0,
		// i.e. TIR at the inside boundary): the exit-refraction lobe below is
		// gated on `R < 1.0`, so when R >= 1.0 there is no companion channel
		// to carry the energy and dropping here would be total, deterministic
		// energy loss.  Re-derive the reflection direction about the TRUE
		// geometric normal instead of the shading normal -- guaranteed to
		// satisfy the gate (no re-check needed): for a ray arriving against
		// geomNBack, dot(reflect(d,geomNBack), geomNBack) = -dot(d,geomNBack) > 0 --
		// holds unconditionally: geomNBack is ray-anchored, so dot(d,geomNBack) < 0
		// always.
		const Vector3 nRefBack = -ri.onb.w();
		const Vector3& geomNRawBack = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : nRefBack;
		const Vector3 geomNBack = ( Vector3Ops::Dot( geomNRawBack, ri.ray.Dir() ) < 0 ) ? geomNRawBack : -geomNRawBack;

		bool bEmitBack = Vector3Ops::Dot( rvDirBack, geomNBack ) > 0;
		if( !bEmitBack && R >= 1.0 ) {
			rvDirBack = Optics::CalculateReflectedRay( ri.ray.Dir(), geomNBack );
			bEmitBack = true;
		}

		if( bEmitBack )
		{
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, rvDirBack );
			reflectedRay.kray = RISEPel( R, R, R );

			reflectedRay.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );

			scattered.AddScatteredRay( reflectedRay );
		}

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

			exitRay.ior_stack = new IORStack( ior_stack );
			exitRay.ior_stack->pop();
			GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );

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
	const IORStack& ior_stack
	) const
{
	const Scalar n = pIOR->GetValueAtNM( ri, nm );

	const bool bFromInside = ior_stack.containsCurrent();

	if( !bFromInside )
	{
		//
		// Front face hit: surface reflection only.
		//

		const Scalar Ni = ior_stack.top();

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

			// Geometric-horizon gate (mirrors Scatter()'s rough-reflection gate)
			// -- including the R >= 1.0 mandatory-lobe fallback: no drop when
			// the reflection is the only channel (entry-side TIR).
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : ri.onb.w();
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
			if( Vector3Ops::Dot( wo, geomN ) <= 0 ) {
				if( R >= 1.0 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = true;
					reflectedRay.pdf = 1.0;
					reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
					reflectedRay.krayNM = R;

					scattered.AddScatteredRay( reflectedRay );
				}
				return;
			}

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
			Vector3 rvDir = Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() );

			// Geometric-horizon gate (mirrors Scatter()'s smooth-reflection
			// gate) -- including the R >= 1.0 mandatory-lobe fallback: no drop
			// when the reflection is the only channel (entry-side TIR); the
			// direction is re-derived about the TRUE geometric normal, which
			// is guaranteed to satisfy the gate (no re-check needed): geomN is
			// ray-anchored, so the reflected direction always lands on the
			// incoming ray's side.
			const Vector3 nRef = ri.onb.w();
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : nRef;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

			bool bEmit = Vector3Ops::Dot( rvDir, geomN ) > 0;
			if( !bEmit && R >= 1.0 ) {
				rvDir = Optics::CalculateReflectedRay( ri.ray.Dir(), geomN );
				bEmit = true;
			}

			if( bEmit )
			{
				ScatteredRay reflectedRay;
				reflectedRay.type = ScatteredRay::eRayReflection;
				reflectedRay.isDelta = true;
				reflectedRay.pdf = 1.0;
				reflectedRay.ray = Ray( ri.ptIntersection, rvDir );
				reflectedRay.krayNM = R;

				scattered.AddScatteredRay( reflectedRay );
			}
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

		const Scalar Nt = ior_stack.top();

		Vector3 refracted = ri.ray.Dir();
		Scalar R;
		if( Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted ) ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		Vector3 rvDirBack = Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() );

		// Geometric-horizon gate (mirrors Scatter()'s back-face gate).  Drop on
		// fail UNLESS mandatory (R >= 1.0, TIR at the inside boundary -- the
		// exit-refraction lobe below is gated on `R < 1.0`, so there is no
		// companion channel and dropping would be total energy loss); in that
		// case re-derive the reflection about the TRUE geometric normal, which
		// is guaranteed to satisfy the gate (no re-check needed): geomNBack is
		// ray-anchored, so the reflected direction always lands on the
		// incoming ray's side.
		const Vector3 nRefBack = -ri.onb.w();
		const Vector3& geomNRawBack = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
			? ri.vGeomNormal : nRefBack;
		const Vector3 geomNBack = ( Vector3Ops::Dot( geomNRawBack, ri.ray.Dir() ) < 0 ) ? geomNRawBack : -geomNRawBack;

		bool bEmitBack = Vector3Ops::Dot( rvDirBack, geomNBack ) > 0;
		if( !bEmitBack && R >= 1.0 ) {
			rvDirBack = Optics::CalculateReflectedRay( ri.ray.Dir(), geomNBack );
			bEmitBack = true;
		}

		if( bEmitBack )
		{
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, rvDirBack );
			reflectedRay.krayNM = R;

			reflectedRay.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );

			scattered.AddScatteredRay( reflectedRay );
		}

		if( R < 1.0 )
		{
			ScatteredRay exitRay;
			exitRay.type = ScatteredRay::eRayRefraction;
			exitRay.isDelta = true;
			exitRay.pdf = 1.0;
			exitRay.ray = Ray( ri.ptIntersection, refracted );
			exitRay.krayNM = 1.0 - R;

			exitRay.ior_stack = new IORStack( ior_stack );
			exitRay.ior_stack->pop();
			GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );

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
	const IORStack& ior_stack
	) const
{
	const bool bFromInside = ior_stack.containsCurrent();

	if( !bFromInside )
	{
		// From outside: only rough reflection has non-zero PDF
		if( alpha > 1e-6 )
		{
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			const Vector3 woNorm = Vector3Ops::Normalize( wo );
			const Vector3 n = ri.onb.w();

			// Geometric-horizon gate (MIS consistency with Scatter()'s
			// sampler-side gate): a wo the sampler can no longer emit
			// contributes zero density.
			const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
				? ri.vGeomNormal : n;
			const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

			if( Vector3Ops::Dot( woNorm, n ) > 0 && Vector3Ops::Dot( wi, n ) > 0 && Vector3Ops::Dot( woNorm, geomN ) > 0 ) {
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
	const IORStack& ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}
