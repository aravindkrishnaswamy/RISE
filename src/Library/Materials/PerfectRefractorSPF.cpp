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

	// Geometric-horizon reference (DL-111, 2026-09-17).  Identical in
	// construction and rationale to `DielectricSPF::GenerateScatteredRay`'s
	// -- see the long comment there.  In brief: a bump / normal map or
	// `GlintModifier` (up to 60 deg) moves the SHADING normal off the
	// surface, so both lobes need a reference no shading tilt can move.
	// `geomN` is RAY-ANCHORED (flipped to oppose `ri.ray.Dir()`), which
	// makes the reflection's half-space `Dot(dir, geomN) > 0` and the
	// transmission's its exact complement `Dot(dir, throughSurface) > 0`,
	// with ONE expression correct on both crossings.  This replaces an
	// `nEff`-anchored rule whose own comment claimed equivalence; it is
	// not equivalent on a DOUBLE-SIDED mesh, where the geometry flips both
	// normals toward the ray and `nEff = -onb.w()` at an exit hit lands on
	// the outward side instead of the ray-opposing one.
	// `HasTrueGeomSide()` (DL-70) excludes `HairGeometry`, whose normal is
	// fabricated from the ray; there, as for a degenerate normal, fall
	// back to the shading normal and both gates become no-ops.
	const Vector3 nShading = ri.onb.w();
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : nShading;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : nShading;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	const Vector3 throughSurface = -geomN;

	// The ordered indices of THIS crossing, hoisted so the DL-111
	// re-derivation below can rebuild both the direction and its Fresnel
	// at the same interface.
	Scalar Ni = 1.0, Nt = 1.0;

	Scalar ref = 0;
	if( bEntering )
	{
		// Going in
		Ni = ior_stack.top();
		Nt = newIOR;
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
				// DL-111, reflection half of the disposal ruling (see
				// DielectricSPF for the full statement).  This re-derivation
				// used to be reserved for a MANDATORY (TIR) reflection, and
				// every other wrong-side reflection was DROPPED -- the `ref`
				// was then paid to nothing, deterministic energy loss growing
				// with the shading tilt.  A delta reflection has no
				// distribution to renormalize, so the same answer applies:
				// re-derive about the TRUE geometric normal (the coarse form
				// of Cycles' `ensure_valid_reflection`).  Guaranteed to
				// satisfy the gate, so no re-check is needed: geomN is
				// ray-anchored, so dot(reflect(d,geomN), geomN) =
				// -dot(d,geomN) > 0 (up to the measure-zero exact-tangent
				// boundary).
				fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
			}
		}
	}
	else
	{
		specular.ior_stack = new IORStack( ior_stack );
		specular.ior_stack->pop();
		GlobalLog()->PrintNew( specular.ior_stack, __FILE__, __LINE__, "ior stack" );

		// Coming out, IOR becomes air
		Ni = newIOR;
		Nt = specular.ior_stack ? specular.ior_stack->top() : 1.0;
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
				// DL-111, reflection half of the disposal ruling (see
				// DielectricSPF for the full statement).  This re-derivation
				// used to be reserved for a MANDATORY (TIR) reflection, and
				// every other wrong-side reflection was DROPPED -- the `ref`
				// was then paid to nothing, deterministic energy loss growing
				// with the shading tilt.  A delta reflection has no
				// distribution to renormalize, so the same answer applies:
				// re-derive about the TRUE geometric normal (the coarse form
				// of Cycles' `ensure_valid_reflection`).  Guaranteed to
				// satisfy the gate, so no re-check is needed: geomN is
				// ray-anchored, so dot(reflect(d,geomN), geomN) =
				// -dot(d,geomN) > 0 (up to the measure-zero exact-tangent
				// boundary).
				fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
			}
		}
	}

	// DL-111: the refracted direction above was built from the SHADING
	// normal, and nothing yet says it actually crossed the surface -- while
	// `specular.ior_stack` has ALREADY been pushed (entry) or popped (exit)
	// to say it did.  Unlike `DielectricSPF`, this SPF had NO geometric
	// gate on the transmission at all; only the Fresnel lobe was gated.
	//
	// Disposal ruling, identical to DielectricSPF's (full derivation in
	// docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md): RE-DERIVE the
	// refraction about the true geometric normal and recompute its Fresnel
	// there, rather than dropping.  It keeps the event a genuine refraction
	// at the same eta (so a dispersion fan stays an ordered fan),
	// preserves the energy, and cannot fail the gate.  If the TRUE
	// interface total-internally-reflects at this incidence where the
	// tilted shading normal did not, `ref` becomes 1 and the reflection
	// above carries everything.
	//
	// Reachability -- CORRECTED by the review's P2-1.  An earlier version
	// of this comment said "refraction into a denser medium always
	// crosses"; that holds ONLY while the tilted shading normal still
	// OPPOSES the incoming ray (`Dot(d, n_s) < 0`).  Two reachable
	// families:
	//   (1) refraction into a RARER medium (a glass->air exit, or entry
	//       into a bubble) at a grazing, normal-perturbed silhouette --
	//       `MakeObliqueHit` with a POSITIVE tilt builds it
	//       (tests/TransmissionPushGateTest.cpp sub-test 2b);
	//   (2) ANY refraction, denser included, once the tilt carries the
	//       shading normal PAST the grazing incoming ray
	//       (`Dot(d, n_s) > 0`, a bump / normal map or `GlintModifier` at
	//       a silhouette): `Optics::CalculateRefractedRay` flips the
	//       normal internally, so the refraction is built about `-n_s`,
	//       whose far side is the side the ray CAME FROM.  Measured
	//       pre-fix: 64/64 wrong-side transmissions on an ordinary
	//       air->glass 1.5 ENTRY at every (delta, tilt) in
	//       {80,85,89} x {-30,-45,-60} -- `MakeObliqueHit` with a
	//       NEGATIVE tilt, sub-test 2c.
	if( ref < 1.0 && Vector3Ops::Dot( vRefracted, throughSurface ) <= 0 ) {
		Vector3 geomRefracted = ri.ray.Dir();
		if( Optics::CalculateRefractedRay( geomN, Ni, Nt, geomRefracted ) ) {
			vRefracted = geomRefracted;
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, geomN, Ni, Nt );
		} else {
			ref = 1.0;
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

	if( ref > 0.0 ) {
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

	// Geometric-horizon reference (DL-111, 2026-09-17).  Identical in
	// construction and rationale to `DielectricSPF::GenerateScatteredRay`'s
	// -- see the long comment there.  In brief: a bump / normal map or
	// `GlintModifier` (up to 60 deg) moves the SHADING normal off the
	// surface, so both lobes need a reference no shading tilt can move.
	// `geomN` is RAY-ANCHORED (flipped to oppose `ri.ray.Dir()`), which
	// makes the reflection's half-space `Dot(dir, geomN) > 0` and the
	// transmission's its exact complement `Dot(dir, throughSurface) > 0`,
	// with ONE expression correct on both crossings.  This replaces an
	// `nEff`-anchored rule whose own comment claimed equivalence; it is
	// not equivalent on a DOUBLE-SIDED mesh, where the geometry flips both
	// normals toward the ray and `nEff = -onb.w()` at an exit hit lands on
	// the outward side instead of the ray-opposing one.
	// `HasTrueGeomSide()` (DL-70) excludes `HairGeometry`, whose normal is
	// fabricated from the ray; there, as for a degenerate normal, fall
	// back to the shading normal and both gates become no-ops.
	const Vector3 nShading = ri.onb.w();
	const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : nShading;
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
		? trueGeomNormal : nShading;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	const Vector3 throughSurface = -geomN;

	// The ordered indices of THIS crossing, hoisted so the DL-111
	// re-derivation below can rebuild both the direction and its Fresnel
	// at the same interface.
	Scalar Ni = 1.0, Nt = 1.0;

	Scalar ref = 0;
	if( bEntering )
	{
		// Going in
		Ni = ior_stack.top();
		Nt = newIOR;
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
				// DL-111, reflection half of the disposal ruling (see
				// DielectricSPF for the full statement).  This re-derivation
				// used to be reserved for a MANDATORY (TIR) reflection, and
				// every other wrong-side reflection was DROPPED -- the `ref`
				// was then paid to nothing, deterministic energy loss growing
				// with the shading tilt.  A delta reflection has no
				// distribution to renormalize, so the same answer applies:
				// re-derive about the TRUE geometric normal (the coarse form
				// of Cycles' `ensure_valid_reflection`).  Guaranteed to
				// satisfy the gate, so no re-check is needed: geomN is
				// ray-anchored, so dot(reflect(d,geomN), geomN) =
				// -dot(d,geomN) > 0 (up to the measure-zero exact-tangent
				// boundary).
				fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
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
		Ni = newIOR;
		Nt = exitIOR;
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
				// DL-111, reflection half of the disposal ruling (see
				// DielectricSPF for the full statement).  This re-derivation
				// used to be reserved for a MANDATORY (TIR) reflection, and
				// every other wrong-side reflection was DROPPED -- the `ref`
				// was then paid to nothing, deterministic energy loss growing
				// with the shading tilt.  A delta reflection has no
				// distribution to renormalize, so the same answer applies:
				// re-derive about the TRUE geometric normal (the coarse form
				// of Cycles' `ensure_valid_reflection`).  Guaranteed to
				// satisfy the gate, so no re-check is needed: geomN is
				// ray-anchored, so dot(reflect(d,geomN), geomN) =
				// -dot(d,geomN) > 0 (up to the measure-zero exact-tangent
				// boundary).
				fresnel.ray.Set( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
			}
		}
	}

	// DL-111: the refracted direction above was built from the SHADING
	// normal, and nothing yet says it actually crossed the surface -- while
	// `specular.ior_stack` has ALREADY been pushed (entry) or popped (exit)
	// to say it did.  Unlike `DielectricSPF`, this SPF had NO geometric
	// gate on the transmission at all; only the Fresnel lobe was gated.
	//
	// Disposal ruling, identical to DielectricSPF's (full derivation in
	// docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md): RE-DERIVE the
	// refraction about the true geometric normal and recompute its Fresnel
	// there, rather than dropping.  It keeps the event a genuine refraction
	// at the same eta (so a dispersion fan stays an ordered fan),
	// preserves the energy, and cannot fail the gate.  If the TRUE
	// interface total-internally-reflects at this incidence where the
	// tilted shading normal did not, `ref` becomes 1 and the reflection
	// above carries everything.
	//
	// Reachability -- CORRECTED by the review's P2-1.  An earlier version
	// of this comment said "refraction into a denser medium always
	// crosses"; that holds ONLY while the tilted shading normal still
	// OPPOSES the incoming ray (`Dot(d, n_s) < 0`).  Two reachable
	// families:
	//   (1) refraction into a RARER medium (a glass->air exit, or entry
	//       into a bubble) at a grazing, normal-perturbed silhouette --
	//       `MakeObliqueHit` with a POSITIVE tilt builds it
	//       (tests/TransmissionPushGateTest.cpp sub-test 2b);
	//   (2) ANY refraction, denser included, once the tilt carries the
	//       shading normal PAST the grazing incoming ray
	//       (`Dot(d, n_s) > 0`, a bump / normal map or `GlintModifier` at
	//       a silhouette): `Optics::CalculateRefractedRay` flips the
	//       normal internally, so the refraction is built about `-n_s`,
	//       whose far side is the side the ray CAME FROM.  Measured
	//       pre-fix: 64/64 wrong-side transmissions on an ordinary
	//       air->glass 1.5 ENTRY at every (delta, tilt) in
	//       {80,85,89} x {-30,-45,-60} -- `MakeObliqueHit` with a
	//       NEGATIVE tilt, sub-test 2c.
	if( ref < 1.0 && Vector3Ops::Dot( vRefracted, throughSurface ) <= 0 ) {
		Vector3 geomRefracted = ri.ray.Dir();
		if( Optics::CalculateRefractedRay( geomN, Ni, Nt, geomRefracted ) ) {
			vRefracted = geomRefracted;
			ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), vRefracted, geomN, Ni, Nt );
		} else {
			ref = 1.0;
		}
	}

	if( ref < 1.0 ) {
		specular.ray.Set( ri.ptIntersection, vRefracted );
		specular.krayNM = GuardedGetColorNM( *pRefractivity, ri, nm ) * (1.0-ref);

		scattered.AddScatteredRay( specular );
	}

	if( ref > 0.0 ) {
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

