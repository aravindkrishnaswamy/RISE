//////////////////////////////////////////////////////////////////////
//
//  DielectricSPF.cpp - Implementation of dielectric SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DielectricSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/ThinFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

// The stack-allocated AR path caps at DielectricSPF::kMaxARLayers; the TMM it
// forwards to caps at ThinFilm::kMaxFilms.  They MUST agree or a scene could
// author more layers than the TMM evaluates (silent truncation of coating).
static_assert( DielectricSPF::kMaxARLayers == RISE::ThinFilm::kMaxFilms,
	"DielectricSPF::kMaxARLayers must match ThinFilm::kMaxFilms" );

DielectricSPF::ARStack DielectricSPF::BuildARStack(
	const Scalar* n, const Scalar* k, const Scalar* t, int nLayers )
{
	ARStack out;
	if( nLayers < 0 || !n || !t ) nLayers = 0;
	if( nLayers > kMaxARLayers ) nLayers = kMaxARLayers;
	out.nLayers = nLayers;
	for( int i = 0; i < nLayers; ++i ) {
		out.n[i] = n[i];
		out.k[i] = k ? k[i] : Scalar(0);	// k array optional (transparent AR)
		out.t[i] = t[i];
	}
	for( int i = nLayers; i < kMaxARLayers; ++i ) {
		out.n[i] = out.k[i] = out.t[i] = Scalar(0);
	}
	return out;
}

DielectricSPF::DielectricSPF(
	const IScalarPainter& tau_,
	const IScalarPainter& ri,
	const IScalarPainter& s,
	const bool hg,
	const Scalar* arN_, const Scalar* arK_, const Scalar* arThickness_,
	int arNLayers_
	) :
  pTau( &tau_ ),
  pRIndex( &ri ),
  pScat( &s ),
  bHG( hg ),
  arStack( BuildARStack( arN_, arK_, arThickness_, arNLayers_ ) )
{
	pTau->addref();
	pRIndex->addref();
	pScat->addref();
}

DielectricSPF::~DielectricSPF( )
{
	safe_release( pTau );
	safe_release( pRIndex );
	safe_release( pScat );
}

void DielectricSPF::SetTransmittance( const IScalarPainter& v )
{
	v.addref();
	safe_release( pTau );
	pTau = &v;
}

void DielectricSPF::SetIOR( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRIndex );
	pRIndex = &v;
}

void DielectricSPF::SetScattering( const IScalarPainter& v )
{
	v.addref();
	safe_release( pScat );
	pScat = &v;
}

//! Unpolarized reflectance of the AR stack for one interface crossing.
//! The stack is authored ambient(air)->substrate(medium); a ray crossing from
//! OUTSIDE traverses it in that order, while one crossing from INSIDE meets the
//! substrate-adjacent layer first, so the layer order (and endpoints) reverse.
//! `nIncident`/`nSubstrate` are the real indices on the incoming / outgoing
//! side of THIS crossing.  Falls back to the exact Airy single-film result at
//! nLayers==1 (ReflectanceConductorStack is algebraically identical there).
static Scalar ARStackReflectance(
	const DielectricSPF::ARStack& s, bool fromInside,
	Scalar cosI, Scalar lam, Scalar nIncident, Scalar nSubstrate )
{
	const int nF = s.nLayers;
	RISE::ThinFilm::Complex film[ DielectricSPF::kMaxARLayers ];
	Scalar                  thick[ DielectricSPF::kMaxARLayers ];
	for( int i = 0; i < nF; ++i ) {
		const int src = fromInside ? ( nF - 1 - i ) : i;
		film[i]  = RISE::ThinFilm::detail::MakeIndex( s.n[src], s.k[src] );
		thick[i] = s.t[src];
	}
	const RISE::ThinFilm::Complex N0 = RISE::ThinFilm::detail::MakeIndex( nIncident,  0.0 );
	const RISE::ThinFilm::Complex Ns = RISE::ThinFilm::detail::MakeIndex( nSubstrate, 0.0 );
	return RISE::ThinFilm::ReflectanceConductorStack( cosI, lam, N0, film, thick, nF, Ns );
}

//! Returns true if there was reflection
Scalar DielectricSPF::GenerateScatteredRay(
	ScatteredRay& dielectric,									///< [out] Scattered dielectric ray
	ScatteredRay& fresnel,										///< [out] Scattered fresnel or reflected ray
	bool& bDielectric,											///< [out] Dielectric ray exists?
	bool& bFresnel,												///< [out] Fresnel ray exists?
	const bool bFromInside,
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& random,										///< [in] Two canonical random numbers
	const Scalar scatfunc,
	const Scalar rIndex,
	const Scalar nm,
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	dielectric.type = ScatteredRay::eRayRefraction;
	dielectric.isDelta = true;
	dielectric.pdf = 1.0;
	fresnel.type = ScatteredRay::eRayReflection;
	fresnel.isDelta = true;
	fresnel.pdf = 1.0;

	Vector3	refracted = ri.ray.Dir();
	Scalar		ref=0;

	bDielectric = bFresnel = true;

	// Geometric-horizon reference (DL-111, 2026-09-17).  GlintModifier can
	// tilt the shading normal up to 60 deg off the true surface, and a bump
	// or normal map or a smooth-shaded mesh's interpolated normal does the
	// same more mildly, so a direction that validates against the (tilted)
	// SHADING normal can still be on the wrong side of the actual surface.
	// Both lobes below need a reference that no shading tilt can move:
	//
	//   * the Fresnel reflection must stay on the side the incoming ray
	//     arrived from,      Dot(dir, geomN) > 0;
	//   * the transmission must cross to the other side,
	//                        Dot(dir, geomN) < 0   ==   Dot(dir, throughSurface) > 0.
	//
	// `geomN` is therefore RAY-ANCHORED -- flipped to oppose
	// `ri.ray.Dir()` -- which makes those two half-spaces exact
	// complements and, crucially, makes ONE expression correct for BOTH
	// crossings: on entry it is the outward normal, on exit the inward
	// one.
	//
	// This REPLACES an `nEff`-anchored rule (`nEff = bFromInside ?
	// -onb.w() : onb.w()`, oriented by `Dot(geomNRaw, nEff) >= 0`) whose
	// own comment argued it was "provably equivalent to the ray-anchor
	// rule used elsewhere".  It is not, on a DOUBLE-SIDED triangle mesh:
	// there the geometry flips BOTH normals to face the incoming ray
	// (`bGeomNormalOrientedToRay`), so at an EXIT hit `onb.w()` points
	// INWARD and `nEff = -onb.w()` lands outward -- the opposite of the
	// ray-opposing side.  The gate then inverted: measured on a
	// double-sided dielectric slab's inside face, 100% of the internal
	// Fresnel reflection was DROPPED at zero tilt (no bump map needed),
	// and past 45 deg of tilt it was EMITTED pointing out of the solid
	// while still carrying the interior IOR stack
	// (tests/TransmissionPushGateTest.cpp sub-test 3).  Ray-anchoring is
	// also the convention `a4fb884d` established for GGXSPF after the
	// same class of inversion (tests/GlintModifierTest.cpp Test 8d).
	//
	// `ri.vGeomNormal` is recovered through `HasTrueGeomSide()` /
	// `UnflippedGeomNormal()` (DL-70) first.  The un-flip is immaterial to
	// the ray-anchored composite itself, but `HasTrueGeomSide()` also
	// excludes `HairGeometry`, whose normal is FABRICATED from the ray
	// (`bGeomNormalRayDerived`) and so carries no genuine second side:
	// there, as for a degenerate normal (SquaredModulus guard, matching
	// GlintModifier.cpp), fall back to the shading normal and the gates
	// become no-ops.
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

	if( bFromInside )
	{
		// Determine the exit IOR: the medium the ray enters after leaving
		// this object.  Pop the current object from a temporary copy of
		// the stack so that top() reveals the underlying medium's IOR.
		IORStack exitStack( ior_stack );
		exitStack.pop();
		Scalar exitIOR = exitStack.top();
		Ni = rIndex;
		Nt = exitIOR;

		if( Optics::CalculateRefractedRay( -ri.onb.w(), rIndex, exitIOR, refracted ) ) {
			dielectric.ior_stack = new IORStack( ior_stack );
			dielectric.ior_stack->pop();
			GlobalLog()->PrintNew( dielectric.ior_stack, __FILE__, __LINE__, "ior stack" );
			if( arStack.nLayers > 0 ) {
				const Scalar cosI = fabs( Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() ) );
				const Scalar lam = ( nm > 0.0 ) ? nm : 550.0;
				ref = ARStackReflectance( arStack, /*fromInside*/ true, cosI, lam, rIndex, exitIOR );
			} else {
				ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), rIndex, exitIOR );
			}
		} else {
			// Total internal reflection
			ref = 1.0;
		}
	}
	else
	{
		Ni = ior_stack.top();
		Nt = rIndex;

		if( Optics::CalculateRefractedRay( ri.onb.w(), ior_stack.top(), rIndex, refracted ) ) {
			if( arStack.nLayers > 0 ) {
				const Scalar cosI = fabs( Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() ) );
				const Scalar lam = ( nm > 0.0 ) ? nm : 550.0;
				ref = ARStackReflectance( arStack, /*fromInside*/ false, cosI, lam, ior_stack.top(), rIndex );
			} else {
				ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, ri.onb.w(), ior_stack.top(), rIndex );
			}
			dielectric.ior_stack = new IORStack( ior_stack );
			dielectric.ior_stack->push( rIndex );
			GlobalLog()->PrintNew( dielectric.ior_stack, __FILE__, __LINE__, "ior stack" );
		} else {
			ref = 1.0;
		}
	}

	// DL-111: the transmitted direction above was built from the SHADING
	// normal, and nothing yet says it actually crossed the surface -- while
	// `dielectric.ior_stack` has ALREADY been pushed (entry) or popped
	// (exit) to say it did.  A continuation that travels back out the side
	// it came from with the object pushed makes the NEXT hit on that object
	// read as an exit that was never legitimately entered: the DL-03 /
	// DL-45 / DL-68 misclassification, here at a delta lobe.
	//
	// THE DISPOSAL RULING (docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md has
	// the full derivation).  A delta Snell lobe has no distribution to
	// renormalize, so DL-68's clip does not apply; of the candidates --
	// mirroring the Snell result across the geometric plane, re-labelling
	// it as a reflection, dropping it with the energy accounted as loss,
	// or RE-DERIVING the refraction about the true geometric normal -- only
	// the last keeps the event a genuine refraction obeying Snell's law at
	// the true interface with the SAME eta (so a dispersion fan stays an
	// ordered fan), preserves the energy exactly, and CANNOT fail the gate:
	// a refraction about N always lands on the far side of N.  It is also
	// exactly the treatment this function already applies to its own
	// mandatory (TIR) reflection just below.  The Fresnel term is
	// recomputed at the same normal so direction and weight describe one
	// interface.
	//
	// Reachability, worth stating because it is narrow: refraction into a
	// DENSER medium bends TOWARD the normal, so the transmitted direction
	// is angularly BETWEEN the incoming ray and the shading normal's far
	// side -- both already inside the (convex) crossing half-space -- and
	// this branch is unreachable.  It needs refraction into a RARER medium
	// (an ordinary glass->air exit, or entry into a bubble: an `ior 1.0`
	// object inside a glass block) AND the tilt and the refracted deviation
	// to add, i.e. a grazing ray at a normal-perturbed silhouette.
	if( ref < 1.0 && Vector3Ops::Dot( refracted, throughSurface ) <= 0 ) {
		Vector3 geomRefracted = ri.ray.Dir();
		if( Optics::CalculateRefractedRay( geomN, Ni, Nt, geomRefracted ) ) {
			refracted = geomRefracted;
			if( arStack.nLayers > 0 ) {
				const Scalar cosI = fabs( Vector3Ops::Dot( geomN, ri.ray.Dir() ) );
				const Scalar lam = ( nm > 0.0 ) ? nm : 550.0;
				ref = ARStackReflectance( arStack, bFromInside, cosI, lam, Ni, Nt );
			} else {
				ref = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, geomN, Ni, Nt );
			}
		} else {
			// The TRUE interface total-internally-reflects at this
			// incidence even though the tilted shading normal did not.
			// Mandatory reflection; the transmission lobe is not emitted
			// (the `ref < 1.0` gate below), and the reflection block
			// immediately following carries all the energy.
			//
			// `refracted` MUST be restored to the incoming direction here
			// (review P1-1).  It is currently holding the WRONG-SIDE
			// shading-normal Snell result -- that is what brought us into
			// this block -- and the `scattering` warp further down runs
			// UNCONDITIONALLY, so leaving it would hand
			// `GeometricUtilities::PerturbClipped` an axis outside its clip
			// half-space and fire its fail-loud precondition (a
			// global-lock console + file write) on the per-sample scatter
			// path.  Measured on the shipped
			// `scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene` at
			// 400x400 / 4 spp: 89-98 such lines per render; 320/320 at
			// five (delta, tilt) exit cells in
			// tests/TransmissionPushGateTest.cpp sub-test 12.  Behaviour
			// was otherwise nil (`ref == 1` drops the lobe and the energy
			// still sums to 1), but the log write is not.
			//
			// `ri.ray.Dir()` is the value the two ORIGINAL TIR branches
			// above leave in place (`Optics::CalculateRefractedRay` does
			// not touch its in/out argument when it returns false), and it
			// satisfies the warp's precondition by construction:
			// `geomN` is ray-anchored, so
			// `Dot(ri.ray.Dir(), throughSurface) > 0` always.
			refracted = ri.ray.Dir();
			ref = 1.0;
		}
	}

	// reflect ray
	{
		// `Optics::CalculateReflectedRay` is sign-invariant in its normal
		// argument (r = d - 2 Dot(d,n) n), so the two branches differ only
		// in the IOR stack the reflection carries, not in the direction.
		if( bFromInside ) {
			fresnel.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
		} else {
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
		}
	}

	if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
		// DL-111, reflection half of the same disposal ruling.  This
		// re-derivation used to be reserved for a MANDATORY (TIR)
		// reflection, and every other wrong-side reflection was DROPPED --
		// deterministic energy loss that grows with the shading tilt and
		// that the transmission channel does not pick up, since `ref` is
		// simply never paid to anything.  A delta reflection has no
		// distribution to renormalize either, so the same answer applies:
		// re-derive about the TRUE geometric normal, which is the coarse
		// form of the "clamp the shading normal so the reflection stays
		// valid" rule production renderers apply (Cycles'
		// `ensure_valid_reflection`).
		//
		// Guaranteed to satisfy the gate, so no re-check is needed: for a
		// ray arriving against geomN,
		// dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0, and geomN is
		// ray-anchored by construction above so dot(d,geomN) < 0 always
		// (up to the measure-zero exact-tangent boundary).
		fresnel.ray.SetDir( Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
	}

	// refracted ray
	{
		dielectric.ray = Ray( ri.ptIntersection, refracted );

		Scalar alpha = 0;

		if( bHG ) {
			if( scatfunc<1 ) {
				const Scalar& g = scatfunc;
				const Scalar inner = (1.0 - g*g) / (1 - g + 2*g*random.x);
				alpha = acos( (1/(2.0*g)) * (1 + g*g - inner*inner) );
			}
		} else {
			if( scatfunc < 1000000.0 ) {
				alpha = acos( pow(random.x, 1.0 / (scatfunc+1.0)) );
			}
		}

		// Use the warping function for a Phong based PDF.
		//
		// DL-111: this warp is the one NON-delta part of the lobe, and
		// DL-68's construction applies to it verbatim.  Whether `alpha`
		// came from the Henyey-Greenstein inverse CDF or the Phong
		// `cos^N` one, the warp is UNIFORM IN AZIMUTH about the Snell
		// axis at fixed `alpha`, and the crossing constraint is a plane
		// through the origin -- so the clipped conditional is still
		// uniform in azimuth, and drawing it on the valid ARC instead of
		// the full circle is EXACT, costs the same single canonical
		// number `random.y`, and renormalizes the clipped-away energy
		// into the valid region rather than dropping it.  Without it a
		// wide `scattering` widens the wrong-side set on its own, with no
		// grazing silhouette needed: measured pre-fix on a closed
		// analytic entry at `scattering 1`, 9/4075 wrong-side pushes at
		// 15 deg of tilt rising to 663/3572 at 89 deg, and at
		// `scattering 0` (the documented "maximally diffuse
		// transmission") 119/3878 to 1065/3123.
		//
		// The precondition (`Dot(axis, throughSurface) >= 0`) holds on
		// every path into here, and the block runs UNCONDITIONALLY --
		// including when the lobe is about to be dropped -- so all THREE
		// paths matter:
		//
		//   * the re-derivation above SUCCEEDED (`ref < 1`): `refracted`
		//     is a refraction about the ray-anchored `geomN`, which lands
		//     on `throughSurface` by construction;
		//   * the shading-normal Snell SUCCEEDED and was already on the
		//     right side: the re-derivation block was skipped and the
		//     value satisfies the same test it was just checked against;
		//   * any TIR (either of the two original branches, or the
		//     re-derivation's own fallback -- review P1-1): every one of
		//     them leaves `refracted == ri.ray.Dir()`, and
		//     `Dot(ri.ray.Dir(), throughSurface) > 0` holds because
		//     `geomN` is ray-anchored.
		if( alpha > 0 && alpha < PI_OV_TWO ) {
			dielectric.ray.SetDir(GeometricUtilities::PerturbClipped(
				dielectric.ray.Dir(),
				alpha,
				throughSurface,
				random.y
				));
		}

		// DL-111: the crossing test, against the ray-anchored GEOMETRIC
		// normal.  This replaces a test against `ri.onb.w()` -- the
		// SHADING normal, the very quantity a bump map or glint tilts --
		// which both admitted directions that never crossed and rejected
		// directions that did.  Both branches above now guarantee this
		// holds, so it is a defensive net for the measure-zero
		// exact-tangent boundary of the clipped arc rather than a live
		// rejection path.
		if( Vector3Ops::Dot( dielectric.ray.Dir(), throughSurface ) <= 0 ) {
			bDielectric = false;
		}
	}

	return ref;
}


void DielectricSPF::DoSingleRGBComponent(
	 const RayIntersectionGeometric& ri,						///< [in] Geometric intersection details for point of intersection
	 const Point2& random,										///< [in] Two canonical random numbers
	 ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	 const IORStack& ior_stack,							///< [in/out] Index of refraction stack
	 const int oneofthree,
	 const Scalar newIOR,
	 const Scalar scattering,
	 const Scalar cosine,
	  const Scalar nm
	 ) const
{
	ScatteredRay dielectric;
	ScatteredRay fresnel;

	bool		bFromInside = false;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available. The stack tracks which objects a ray
	// is currently inside, so if this object is in the stack we must be
	// exiting. This is more robust than the normal-based cosine test at
	// grazing angles where numerical precision can give wrong results.
	if( ior_stack.containsCurrent() ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		bFromInside = true;

		const ScalarTriple tauVals = pTau->GetValuesAt( ri );
		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = pow( tauVals.v[oneofthree-1], distance );
		} else {
			dielectric.kray = RISEPel(
				pow( tauVals.v[0], distance ),
				pow( tauVals.v[1], distance ),
				pow( tauVals.v[2], distance ) );
		}
	} else {
		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = 1.0;
		} else {
			dielectric.kray = RISEPel(1.0,1.0,1.0);
		}
	}

	bool bDielectric, bFresnel;
	Scalar ref = GenerateScatteredRay( dielectric, fresnel, bDielectric, bFresnel, bFromInside, ri, random, scattering, newIOR, nm, ior_stack );

	// NO eta^2 HERE, DELIBERATELY (debt 30).  The transmission lobe below
	// carries only Fresnel (1-ref) and Beer's-law tau^distance.  Radiance
	// is not invariant across the interface -- L/n^2 is -- but whether the
	// (eta_before/eta_after)^2 factor applies depends on whether the walk
	// consuming this ray carries RADIANCE (camera-rooted: apply) or
	// IMPORTANCE / FLUX (light subpath, photon tracer: do not), and an SPF
	// cannot know which.  The factor therefore lives at the consumer,
	// which reads it off `dielectric.ior_stack` (pushed / popped just above
	// in GenerateScatteredRay) via RISE::RadianceEtaScale -- see the
	// contract on ScatteredRay::kray in Interfaces/ISPF.h and the site
	// table in docs/REFRACTIVE_RADIANCE_SCALING.md.
	if( bDielectric && ref < 1.0 ) {
		if( oneofthree ) {
			dielectric.kray[oneofthree-1] = dielectric.kray[oneofthree-1] * (1.0-ref);
		} else {
			dielectric.kray = dielectric.kray * (1.0-ref);
		}

		scattered.AddScatteredRay( dielectric );
	}

	if( bFresnel ) {
		if( oneofthree ) {
			fresnel.kray[oneofthree-1] = ref;
		} else {
			fresnel.kray = RISEPel(ref,ref,ref);
		}

		scattered.AddScatteredRay( fresnel );
	}
}


void DielectricSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	Scalar		cosine = -Vector3Ops::Dot(ri.onb.w(), ri.ray.Dir());

	// IOR + scattering coefficient are now physical scalars carried by
	// `IScalarPainter`.  Dispersion is detected via the painter's own
	// static-property report (HasPerChannelVariation) — no FP fuzzy
	// compare needed.  `tau`'s per-channel variation is irrelevant to
	// the disperse-vs-uniform path branch; only ior and scat trigger
	// the per-channel scatter loop.
	const ScalarTriple iorVals  = pRIndex->GetValuesAt( ri );
	const ScalarTriple scatVals = pScat->GetValuesAt( ri );
	const bool disperse =
		pRIndex->HasPerChannelVariation() ||
		pScat->HasPerChannelVariation() ||
		( arStack.nLayers > 0 );

	if( !disperse ) {
		// No dispersion
		DoSingleRGBComponent( ri, Point2(sampler.Get1D(),sampler.Get1D()), scattered, ior_stack, false, iorVals.v[0], scatVals.v[0], cosine, -1.0 );
	} else {
		// We have dispersion, so we must process each component seperately.
		// The per-channel representative wavelengths (used here for the
		// AR-coating reflectance on the RGB preview path) are the SHARED
		// `ScalarPainterRGB::kChannelNM` -- the same three an
		// `IScalarPainter` samples its curve at to build the `iorVals`
		// triple this loop is refracting against (DL-29).  They used to
		// be a private `kARChannelNM` copy here; keeping one array means
		// the painter's triple and this loop can never describe two
		// different sets of wavelengths.
		Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		for( int i=0; i<3; i++ ) {
			DoSingleRGBComponent( ri, ptrand, scattered, ior_stack, i+1, iorVals.v[i], scatVals.v[i], cosine, ScalarPainterRGB::kChannelNM[i] );
		}
	}
}

void DielectricSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay dielectric;
	ScatteredRay fresnel;

	bool		bFromInside = false;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available (see DoSingleRGBComponent for details)
	if( ior_stack.containsCurrent() ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		bFromInside = true;

		dielectric.krayNM = pow( pTau->GetValueAtNM( ri, nm ), distance );
	} else {
		dielectric.krayNM = 1.0;
	}

	bool bDielectric, bFresnel;
	const Scalar ref = GenerateScatteredRay( dielectric, fresnel, bDielectric, bFresnel, bFromInside, ri, Point2(sampler.Get1D(),sampler.Get1D()), pScat->GetValueAtNM( ri, nm ), pRIndex->GetValueAtNM( ri, nm ), nm, ior_stack );

	// No eta^2 on krayNM either -- same contract as the Pel twin above.
	// The per-wavelength IOR is already in the stack this ray carries
	// (`pRIndex->GetValueAtNM` was pushed by GenerateScatteredRay), so a
	// dispersive medium gets its own factor at the consumer for free.
	if( bDielectric && ref < 1.0 ) {
		dielectric.krayNM = dielectric.krayNM * (1.0-ref);
		scattered.AddScatteredRay( dielectric );
	}

	if( bFresnel && ref ) {
		fresnel.krayNM = ref;
		scattered.AddScatteredRay( fresnel );
	}
}

Scalar DielectricSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return 0;
}

Scalar DielectricSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return 0;
}
