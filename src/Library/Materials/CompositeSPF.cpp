//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.cpp - Implementation of the Composite SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompositeSPF.h"

using namespace RISE;
using namespace RISE::Implementation;

CompositeSPF::CompositeSPF(
	const ISPF& top_,
	const ISPF& bottom_,
	const unsigned int max_recur_ ,
	const unsigned int max_reflection_recursion_,		// maximum level of reflection recursion
	const unsigned int max_refraction_recursion_,		// maximum level of refraction recursion
	const unsigned int max_diffuse_recursion_,			// maximum level of diffuse recursion
	const unsigned int max_translucent_recursion_,		// maximum level of translucent recursion
	const Scalar thickness_,							// thickness between the materials
	const IPainter& extinction_							// extinction coefficient for absorption between layers
	) :
  top( top_ ),
  bottom( bottom_ ),
  max_recur( max_recur_ ),
  max_reflection_recursion( max_reflection_recursion_ ),
  max_refraction_recursion( max_refraction_recursion_ ),
  max_diffuse_recursion( max_diffuse_recursion_ ),
  max_translucent_recursion( max_translucent_recursion_ ),
  thickness( thickness_ ),
  extinction( extinction_ )
{
	top.addref();
	bottom.addref();
	extinction.addref();
}

CompositeSPF::~CompositeSPF( )
{
	top.release();
	bottom.release();
	extinction.release();
}

bool CompositeSPF::ShouldScatteredRayBePropagated(
	const ScatteredRay::ScatRayType type,
	const unsigned int steps
	) const
{
	switch( type )
	{
	case ScatteredRay::eRayReflection:
		return (steps<max_reflection_recursion);
		break;
	case ScatteredRay::eRayRefraction:
		return (steps<max_refraction_recursion);
		break;
	case ScatteredRay::eRayDiffuse:
		return (steps<max_diffuse_recursion);
		break;
	case ScatteredRay::eRayTranslucent:
		return (steps<max_translucent_recursion);
		break;
	default:
		return false;
		break;
	};

	return false;
}

void CompositeSPF::ProcessTopLayer(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RISEPel& importance,									///< [in] Importance from prevous pass
				ISampler& sampler,				///< Sampler for the MC process
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const
{
	if( steps >= max_recur || ColorMath::MaxValue(importance) < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.Scatter( ri, sampler, scat_top, ior_stack );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].kray = scat_top[i].kray * importance;
			scattered.AddScatteredRay( scat_top[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_top[i].ray.Dir()));
				my_ri.ray.Advance( thickness );

				// Apply Beer's law absorption through the layer
				const Scalar cosTheta = fabs( Vector3Ops::Dot( my_ri.ray.Dir(), ri.onb.w() ) );
				const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
				const RISEPel attenuation = ColorMath::exponential( extinction.GetColor(ri) * (-pathLength) );

				// Propagate the IOR stack from the scattered ray into the
				// recursive walk.  Without this, a refraction that crossed
				// into the dielectric body (DielectricSPF pushes its IOR
				// onto a fresh stack on `scat_top[i].ior_stack`) hands the
				// bottom layer an empty stack, and the next top-side
				// crossing is then evaluated as if from outside —
				// computing Fresnel / refraction direction with the wrong
				// inside/outside flag.  Same convention as the integrators
				// (see PathTracingIntegrator.cpp's `scat.ior_stack ?
				// *scat.ior_stack : iorStack`).
				const IORStack& nextStack = scat_top[i].ior_stack
					? *scat_top[i].ior_stack : ior_stack;
				ProcessBottomLayer( my_ri, scat_top[i].kray*importance*attenuation, sampler, scattered, steps+1, nextStack );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayer(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RISEPel& importance,									///< [in] Importance from prevous pass
		ISampler& sampler,				///< Sampler for the MC process
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack& ior_stack								///< [in/out] Index of refraction stack
		) const
{
	if( steps >= max_recur || ColorMath::MaxValue(importance) < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.Scatter( ri, sampler, scat_bottom, ior_stack );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.Dir(), ri.onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].kray = scat_bottom[i].kray * importance;
			scattered.AddScatteredRay( scat_bottom[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_bottom[i].ray.Dir()));
				my_ri.ray.Advance( thickness );

				// Apply Beer's law absorption through the layer
				const Scalar cosTheta = fabs( Vector3Ops::Dot( my_ri.ray.Dir(), ri.onb.w() ) );
				const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
				const RISEPel attenuation = ColorMath::exponential( extinction.GetColor(ri) * (-pathLength) );

				// IOR stack propagation — see ProcessTopLayer's matching
				// site for the rationale.  A bottom layer that doesn't
				// touch the stack (Lambertian) leaves scat_bottom[i].ior_stack
				// null, so we fall back to whatever stack we received,
				// which still reflects "we're inside the dielectric body"
				// from the prior top-layer push.
				const IORStack& nextStack = scat_bottom[i].ior_stack
					? *scat_bottom[i].ior_stack : ior_stack;
				ProcessTopLayer( my_ri, scat_bottom[i].kray*importance*attenuation, sampler, scattered, steps+1, nextStack );
			}
		}
	}
}

void CompositeSPF::ProcessTopLayerNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Scalar importance,									///< [in] Importance from prevous pass
				ISampler& sampler,				///< Sampler for the MC process
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const
{
	if( steps >= max_recur || importance < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.ScatterNM( ri, sampler, nm, scat_top, ior_stack );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].krayNM *= importance;
			scattered.AddScatteredRay( scat_top[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_top[i].ray.Dir()));
				my_ri.ray.Advance( thickness );

				// Apply Beer's law absorption through the layer
				const Scalar cosTheta = fabs( Vector3Ops::Dot( my_ri.ray.Dir(), ri.onb.w() ) );
				const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
				const Scalar extinctionNM = extinction.GetColorNM(ri, nm);
				const Scalar attenuation = exp( -extinctionNM * pathLength );

				// IOR stack propagation — see ProcessTopLayer (RGB) for rationale.
				const IORStack& nextStack = scat_top[i].ior_stack
					? *scat_top[i].ior_stack : ior_stack;
				ProcessBottomLayerNM( my_ri, scat_top[i].krayNM*importance*attenuation, sampler, nm, scattered, steps+1, nextStack );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayerNM(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const Scalar importance,									///< [in] Importance from prevous pass
		ISampler& sampler,				///< Sampler for the MC process
		const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack& ior_stack								///< [in/out] Index of refraction stack
		) const
{
	if( steps >= max_recur || importance < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.ScatterNM( ri, sampler, nm, scat_bottom, ior_stack );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.Dir(), ri.onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].krayNM *= importance;
			scattered.AddScatteredRay( scat_bottom[i] );
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_bottom[i].ray.Dir()));
				my_ri.ray.Advance( thickness );

				// Apply Beer's law absorption through the layer
				const Scalar cosTheta = fabs( Vector3Ops::Dot( my_ri.ray.Dir(), ri.onb.w() ) );
				const Scalar pathLength = (cosTheta > NEARZERO) ? thickness / cosTheta : thickness;
				const Scalar extinctionNM = extinction.GetColorNM(ri, nm);
				const Scalar attenuation = exp( -extinctionNM * pathLength );

				// IOR stack propagation — see ProcessBottomLayer (RGB) for rationale.
				const IORStack& nextStack = scat_bottom[i].ior_stack
					? *scat_bottom[i].ior_stack : ior_stack;
				ProcessTopLayerNM( my_ri, scat_bottom[i].krayNM*importance*attenuation, sampler, nm, scattered, steps+1, nextStack );
			}
		}
	}
}

void CompositeSPF::Scatter(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in/out] Index of refraction stack
			) const
{
	// Khronos additive composition for upward-emitting top layers.
	// When the top SPF opts in via UsesAdditiveLayering(), the random
	// walk is bypassed in favour of:
	//
	//   f_combined(wi, wo) = f_top(wi, wo) + f_base(wi, wo) · (1 − topAlbedo)
	//
	// where topAlbedo = top.GetLayerAlbedo(ri) is the per-direction
	// directional albedo of the top lobe.  This is the only correct
	// composition for sheen-like top layers whose cosine-hemisphere
	// outgoing rays never reach ProcessBottomLayer in the random
	// walk: without this branch the base layer is silently dropped
	// (Landing 6 §"Finding B").  The opt-in is a static property of
	// the SPF type, NOT a runtime check on `topAlbedo > 0` — a black
	// sheen texel still needs the additive path with topAlbedo = 0
	// so the base BRDF/SPF shines through; gating on `topAlbedo > 0`
	// would silently re-engage the broken random walk for those
	// texels.  The default ISPF::UsesAdditiveLayering() returns
	// false, so non-sheen top layers (dielectric, GGX, translucent)
	// fall through to the historical random walk and behave exactly
	// as before.
	if( top.UsesAdditiveLayering() )
	{
		const RISEPel topAlbedo = top.GetLayerAlbedo( ri, ior_stack );
		// Sample the top layer.  Keep only its upward-going rays —
		// the additive form treats the top lobe as a pure reflective
		// addition; any below-surface samples it produces are
		// outside the model and the random walk's "route to base"
		// path doesn't apply.
		ScatteredRayContainer scat_top;
		top.Scatter( ri, sampler, scat_top, ior_stack );
		for( unsigned int i = 0; i < scat_top.Count(); ++i ) {
			if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
				scattered.AddScatteredRay( scat_top[i] );
			}
		}

		// Sample the base layer with attenuation `(1 − topAlbedo)`
		// so the total integrates to topAlbedo + (1 − topAlbedo) ·
		// E_base ≤ 1 (energy-conserving by construction when both
		// layers are individually energy-conserving).
		ScatteredRayContainer scat_base;
		bottom.Scatter( ri, sampler, scat_base, ior_stack );
		const RISEPel baseAttenuation = RISEPel( 1, 1, 1 ) - topAlbedo;
		for( unsigned int i = 0; i < scat_base.Count(); ++i ) {
			if( Vector3Ops::Dot( scat_base[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
				scat_base[i].kray = scat_base[i].kray * baseAttenuation;
				scattered.AddScatteredRay( scat_base[i] );
			}
		}

		for( unsigned int i = 0; i < scattered.Count(); ++i ) {
			scattered[i].ray.origin = ri.ptIntersection;
		}
		return;
	}

	// Default path: random walk between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0 ) {
		ProcessTopLayer( ri, RISEPel(1,1,1), sampler, scattered, 0, ior_stack );
	} else {
		ProcessBottomLayer( ri, RISEPel(1,1,1), sampler, scattered, 0, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

void CompositeSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	// Spectral mirror of Scatter() — see that function for the rationale
	// of the additive Khronos composition vs. the random walk fallback.
	if( top.UsesAdditiveLayering() )
	{
		const Scalar topAlbedo = top.GetLayerAlbedoNM( ri, ior_stack, nm );
		ScatteredRayContainer scat_top;
		top.ScatterNM( ri, sampler, nm, scat_top, ior_stack );
		for( unsigned int i = 0; i < scat_top.Count(); ++i ) {
			if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
				scattered.AddScatteredRay( scat_top[i] );
			}
		}

		ScatteredRayContainer scat_base;
		bottom.ScatterNM( ri, sampler, nm, scat_base, ior_stack );
		const Scalar baseAttenuation = Scalar(1) - topAlbedo;
		for( unsigned int i = 0; i < scat_base.Count(); ++i ) {
			if( Vector3Ops::Dot( scat_base[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
				scat_base[i].krayNM *= baseAttenuation;
				scattered.AddScatteredRay( scat_base[i] );
			}
		}

		for( unsigned int i = 0; i < scattered.Count(); ++i ) {
			scattered[i].ray.origin = ri.ptIntersection;
		}
		return;
	}

	// Default path: random walk between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0 ) {
		ProcessTopLayerNM( ri, 1, sampler, nm, scattered, 0, ior_stack );
	} else {
		ProcessBottomLayerNM( ri, 1, sampler, nm, scattered, 0, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

Scalar CompositeSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	const Scalar pdf_top = top.Pdf( ri, wo, ior_stack );
	const Scalar pdf_bottom = bottom.Pdf( ri, wo, ior_stack );

	if( top.UsesAdditiveLayering() ) {
		// Additive composition emits BOTH a top-sampled ray and a
		// base-sampled ray per Scatter call.  The probability that
		// the composite produces direction `wo` is therefore the SUM
		// of the per-layer densities — each layer is an independent
		// sampling strategy and both fire on every call.  Returning
		// the average (`0.5 · (top + bottom)`) here would
		// underestimate the composite density by 2× and bias MIS
		// weights against this strategy in BDPT / guided sampling.
		return pdf_top + pdf_bottom;
	}

	// Random walk — multi-bounce path between top and bottom whose
	// exact density depends on the chain.  Equal weighting is the
	// pragmatic approximation that matches the prior behaviour for
	// non-additive composites.
	return 0.5 * (pdf_top + pdf_bottom);
}

Scalar CompositeSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	const Scalar pdf_top = top.PdfNM( ri, wo, nm, ior_stack );
	const Scalar pdf_bottom = bottom.PdfNM( ri, wo, nm, ior_stack );

	if( top.UsesAdditiveLayering() ) {
		return pdf_top + pdf_bottom;
	}
	return 0.5 * (pdf_top + pdf_bottom);
}

