//////////////////////////////////////////////////////////////////////
//
//  PerfectRefractorSPF.cpp - Implementation of the perfect
//  refractor SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PerfectRefractorSPF.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

PerfectRefractorSPF::PerfectRefractorSPF(
	const IPainter& ref,
	const IScalarPainter& Nt_
	) :
  pRefractivity( &ref ),
  pNt( &Nt_ )
{
	pRefractivity->addref();
	pNt->addref();
}

PerfectRefractorSPF::~PerfectRefractorSPF( )
{
	safe_release( pRefractivity );
	safe_release( pNt );
}

void PerfectRefractorSPF::SetRefractivity( const IPainter& ref )
{
	ref.addref();
	safe_release( pRefractivity );
	pRefractivity = &ref;
}

void PerfectRefractorSPF::SetIOR( const IScalarPainter& Nt_ )
{
	Nt_.addref();
	safe_release( pNt );
	pNt = &Nt_;
}

void PerfectRefractorSPF::DoSingleRGBComponent( 
	 const RayIntersectionGeometric& ri,						///< [in] Geometric intersection details for point of intersection
	 ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	 const IORStack& ior_stack,							///< [in/out] Index of refraction stack
	 const int oneofthree,
	 const Scalar newIOR,
	 const Scalar cosine
	 ) const
{
	ScatteredRay specular;
	ScatteredRay fresnel;

	fresnel.type = ScatteredRay::eRayReflection;
	fresnel.isDelta = true;
	fresnel.pdf = 1.0;
	specular.type = ScatteredRay::eRayRefraction;
	specular.isDelta = true;
	specular.pdf = 1.0;

	Vector3	vRefracted = ri.ray.Dir();

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available. If the object is NOT in the stack,
	// we are entering (push). If it IS in the stack, we are exiting (pop).
	// Note: cosine convention here is opposite to DielectricSPF:
	//   cosine = dot(normal, ray_dir), so cosine < NEARZERO means entering.
	const bool bEntering = !ior_stack.containsCurrent();

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a Fresnel reflection direction that
	// validates against the (tilted) shading normal can still point below the
	// geometric surface -- the continuation ray then tunnels into the solid.
	// Orient the geometric normal to the side of the normal this reflection is
	// actually built around (bEntering -> +onb.w(), else -> -onb.w(), mirroring
	// the two branches below).  Degenerate vGeomNormal (SquaredModulus guard,
	// matches GlintModifier.cpp) falls back to the shading normal, making the
	// gate a no-op.  Drop (not redistribute) the Fresnel lobe on failure.
	//
	// NOTE (ray-anchor sweep): nEff is stack-anchored via bEntering (ground
	// truth, independent of any glint tilt), not re-derived from a per-hit
	// Dot(rayDir, tiltable shading normal) test -- provably equivalent to a
	// direct ray-anchor: entering, the ray opposes the true outward normal
	// (Dot(rayDir,Ng)<0) so nEff=+onb.w() agrees; leaving, the ray travels
	// outward (Dot(rayDir,Ng)>0) so nEff=-onb.w() again agrees.  Left as-is
	// to avoid perturbing well-tested crossing logic for a no-op change.
	const Vector3 nEff = bEntering ? ri.onb.w() : -ri.onb.w();
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : nEff;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, nEff ) >= 0 ) ? geomNRaw : -geomNRaw;
	bool bDropFresnel = false;

	Scalar ref = 0;
	if( bEntering )
	{
		// Going in
		if( Optics::CalculateRefractedRay( ri.onb.w(), ior_stack.top(), newIOR, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, ri.onb.w(), ior_stack.top(), newIOR );
			specular.ior_stack = new IORStack( ior_stack );
			specular.ior_stack->push( newIOR );
			GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
		} else {
			// TIR, so reflect
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
			if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
				if( ref >= 1.0 ) {
					// Mandatory reflection: ref forced to 1.0 above means TIR -- no
					// transmission lobe will be emitted at all (see the `ref < 1.0`
					// gate below), so dropping the Fresnel lobe here would be total,
					// deterministic energy loss.  TIR has no companion channel:
					// re-derive the reflection direction about the TRUE geometric
					// normal instead of the shading normal.  This is guaranteed to
					// satisfy the gate (no re-check needed): for a ray arriving
					// against geomN, dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 --
					// holds unconditionally: geomN's orientation (nEff) is provably
					// ray-anchored (see the NOTE above), so dot(d,geomN) < 0 always.
					fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
				} else {
					bDropFresnel = true;
				}
			}
		}
	}
	else
	{
		specular.ior_stack = new IORStack( ior_stack );
		specular.ior_stack->pop();
		GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );

		// Coming out, IOR becomes air
		if( Optics::CalculateRefractedRay( -ri.onb.w(), newIOR, specular.ior_stack?specular.ior_stack->top():1.0, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, -ri.onb.w(), newIOR, specular.ior_stack?specular.ior_stack->top():1.0 );
		} else {
			// TIR, so reflect
			// We're still in the material
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
				if( ref >= 1.0 ) {
					// Mandatory reflection: ref forced to 1.0 above means TIR -- no
					// transmission lobe will be emitted at all (see the `ref < 1.0`
					// gate below), so dropping the Fresnel lobe here would be total,
					// deterministic energy loss.  TIR has no companion channel:
					// re-derive the reflection direction about the TRUE geometric
					// normal instead of the shading normal.  This is guaranteed to
					// satisfy the gate (no re-check needed): for a ray arriving
					// against geomN, dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 --
					// holds unconditionally: geomN's orientation (nEff) is provably
					// ray-anchored (see the NOTE above), so dot(d,geomN) < 0 always.
					fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
				} else {
					bDropFresnel = true;
				}
			}
		}
	}

	if( ref < 1.0 ) {
		specular.ray.Set( ri.ptIntersection, vRefracted );
		if( oneofthree ) {
			specular.kray[oneofthree-1] = pRefractivity->GetColor(ri)[oneofthree-1] * ((1.0-ref));
		} else {
			specular.kray = pRefractivity->GetColor(ri) * (1.0-ref);
		}

		scattered.AddScatteredRay( specular );
	}

	if( ref > 0.0 && !bDropFresnel ) {
		if( oneofthree ) {
			fresnel.kray[oneofthree-1] = ref;
		} else {
			fresnel.kray = RISEPel(ref,ref,ref);
		}

		scattered.AddScatteredRay( fresnel );
	}
}


void PerfectRefractorSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Scalar		cosine = Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() );

	const ScalarTriple ior = pNt->GetValuesAt(ri);

	// Dispersion is now an explicit static-property report from the
	// IScalarPainter — no FP-fuzzy compare needed.
	if( !pNt->HasPerChannelVariation() ) {
		DoSingleRGBComponent( ri, scattered, ior_stack, false, ior.v[0], cosine );
	} else {
		for( int i=0; i<3; i++ ) {
			DoSingleRGBComponent( ri, scattered, ior_stack, i+1, ior.v[i], cosine );
		}
	}
}

void PerfectRefractorSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay specular;
	ScatteredRay fresnel;

	fresnel.type = ScatteredRay::eRayReflection;
	fresnel.isDelta = true;
	fresnel.pdf = 1.0;
	specular.type = ScatteredRay::eRayRefraction;
	specular.isDelta = true;
	specular.pdf = 1.0;

	Vector3	vRefracted = ri.ray.Dir();

	Scalar newIOR = pNt->GetValueAtNM(ri,nm);

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available (see DoSingleRGBComponent for details)
	const bool bEntering = !ior_stack.containsCurrent();

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a Fresnel reflection direction that
	// validates against the (tilted) shading normal can still point below the
	// geometric surface -- the continuation ray then tunnels into the solid.
	// Orient the geometric normal to the side of the normal this reflection is
	// actually built around (bEntering -> +onb.w(), else -> -onb.w(), mirroring
	// the two branches below).  Degenerate vGeomNormal (SquaredModulus guard,
	// matches GlintModifier.cpp) falls back to the shading normal, making the
	// gate a no-op.  Drop (not redistribute) the Fresnel lobe on failure.
	//
	// NOTE (ray-anchor sweep): nEff is stack-anchored via bEntering (ground
	// truth, independent of any glint tilt), not re-derived from a per-hit
	// Dot(rayDir, tiltable shading normal) test -- provably equivalent to a
	// direct ray-anchor: entering, the ray opposes the true outward normal
	// (Dot(rayDir,Ng)<0) so nEff=+onb.w() agrees; leaving, the ray travels
	// outward (Dot(rayDir,Ng)>0) so nEff=-onb.w() again agrees.  Left as-is
	// to avoid perturbing well-tested crossing logic for a no-op change.
	const Vector3 nEff = bEntering ? ri.onb.w() : -ri.onb.w();
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : nEff;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, nEff ) >= 0 ) ? geomNRaw : -geomNRaw;
	bool bDropFresnel = false;

	Scalar ref = 0;
	if( bEntering )
	{
		// Going in
		if( Optics::CalculateRefractedRay( ri.onb.w(), ior_stack.top(), newIOR, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, ri.onb.w(), ior_stack.top(), newIOR );
			specular.ior_stack = new IORStack( ior_stack );
			specular.ior_stack->push( newIOR );
			GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );
		} else {
			// TIR, so reflect
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
			if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
				if( ref >= 1.0 ) {
					// Mandatory reflection: ref forced to 1.0 above means TIR -- no
					// transmission lobe will be emitted at all (see the `ref < 1.0`
					// gate below), so dropping the Fresnel lobe here would be total,
					// deterministic energy loss.  TIR has no companion channel:
					// re-derive the reflection direction about the TRUE geometric
					// normal instead of the shading normal.  This is guaranteed to
					// satisfy the gate (no re-check needed): for a ray arriving
					// against geomN, dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 --
					// holds unconditionally: geomN's orientation (nEff) is provably
					// ray-anchored (see the NOTE above), so dot(d,geomN) < 0 always.
					fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
				} else {
					bDropFresnel = true;
				}
			}
		}
	}
	else
	{
		specular.ior_stack = new IORStack( ior_stack );
		specular.ior_stack->pop();
		GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );

		// Coming out, IOR becomes whatever was there before
		const Scalar exitIOR = specular.ior_stack ? specular.ior_stack->top() : 1.0;
		if( Optics::CalculateRefractedRay( -ri.onb.w(), newIOR, exitIOR, vRefracted ) ) {
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, -ri.onb.w(), newIOR, exitIOR );
		} else {
			// TIR, so reflect
			// We're still in the material
			ref = 1.0;
		}

		if( ref > 0.0 ) {
			fresnel.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
				if( ref >= 1.0 ) {
					// Mandatory reflection: ref forced to 1.0 above means TIR -- no
					// transmission lobe will be emitted at all (see the `ref < 1.0`
					// gate below), so dropping the Fresnel lobe here would be total,
					// deterministic energy loss.  TIR has no companion channel:
					// re-derive the reflection direction about the TRUE geometric
					// normal instead of the shading normal.  This is guaranteed to
					// satisfy the gate (no re-check needed): for a ray arriving
					// against geomN, dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 --
					// holds unconditionally: geomN's orientation (nEff) is provably
					// ray-anchored (see the NOTE above), so dot(d,geomN) < 0 always.
					fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
				} else {
					bDropFresnel = true;
				}
			}
		}
	}

	if( ref < 1.0 ) {
		specular.ray.Set( ri.ptIntersection, vRefracted );
		specular.krayNM = pRefractivity->GetColorNM(ri,nm) * (1.0-ref);

		scattered.AddScatteredRay( specular );
	}

	if( ref > 0.0 && !bDropFresnel ) {
		fresnel.krayNM = ref;

		scattered.AddScatteredRay( fresnel );
	}
}

Scalar PerfectRefractorSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return 0;
}

Scalar PerfectRefractorSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return 0;
}

