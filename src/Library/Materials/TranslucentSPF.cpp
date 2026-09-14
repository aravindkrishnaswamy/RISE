//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.cpp - Implementation of the translucent
//  SPF
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
#include "TranslucentSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// DL-45: the diffuse exit re-emission must actually leave the object
	// GEOMETRICALLY, not merely according to the (possibly bump/glint-
	// tilted) shading normal `n` used to build the cosine sample.  Sampling
	// unconditionally around `n` and unconditionally popping the IOR stack
	// lets a fraction of "exit" rays point back INTO the solid while the
	// stack now says outside (DL-03's tilted fixture measured 1021/4096
	// such inward exits per 4096 trials at a 60-degree shading-normal
	// tilt).  A later hit on the same object then gets misclassified.
	//
	// Fix: resample (rejection sampling against the SAME cosine
	// distribution) until the direction also satisfies the true geometric
	// horizon `dot(wo,geomN)>0`, and report the density of THAT actual,
	// restricted sampling procedure -- not the unclipped cosine density,
	// and not an unrenormalized "reject and lose the energy" density
	// either.  Malley's-method disk projection gives this restricted
	// density a closed form:
	//
	//   cosine-around-n sampling <=> uniform sampling on the unit disk via
	//   (x,y) -> (x,y,sqrt(1-x^2-y^2)) in the (u,v,n) frame.  Writing
	//   geomN = cos(phi)*n + sin(phi)*u for the angle phi between n and
	//   geomN, the constraint dot(wo,geomN)>0 reduces on that disk to
	//   x > -cos(phi)*sqrt(1-y^2) -- the region to the right of one branch
	//   of an ellipse with semi-axes cos(phi) (x) and 1 (y) inscribed in
	//   the disk.  The excluded crescent between that arc and the disk
	//   boundary has area (1-cos(phi))*pi/2, so the valid fraction of the
	//   full cosine-weighted hemisphere is exactly
	//
	//     P(valid) = (1 + cos(phi)) / 2
	//
	// Rejection sampling against the ORIGINAL (unclipped) cosine
	// distribution, keeping only accepted directions, therefore draws
	// EXACTLY the conditional density cos(theta)/pi / P(valid) on the
	// valid region -- a properly normalized (integrates to 1 over its own
	// support) PDF, unlike silently dropping the sample and reporting the
	// unclipped density (which would integrate to only P(valid) < 1, and
	// which is the rejected policy this fix replaces).
	const int kMaxExitResamples = 32;
	// Floor on P(valid): only engaged when n and geomN are nearly opposed
	// (phi -> 180 deg), a configuration well outside GlintModifier's
	// documented <=60 deg tilt.  Bounds the reported pdf's magnitude
	// instead of dividing by (near) zero; the resample loop below almost
	// certainly exhausts its budget and emits nothing in that regime
	// anyway, so this floor is reached only by an externally-supplied wo
	// in Pdf()/PdfNM(), never by Scatter()/ScatterNM()'s own samples.
	const Scalar kMinExitValidFraction = Scalar(1e-4);

	inline Scalar ExitValidFraction( const Vector3& n, const Vector3& geomN )
	{
		const Scalar cosPhi = r_max( Scalar(-1), r_min( Scalar(1), Vector3Ops::Dot(n,geomN) ) );
		return r_max( kMinExitValidFraction, (Scalar(1)+cosPhi) * Scalar(0.5) );
	}

	// Draws a cosine-around-`n` direction that is ALSO geometrically valid
	// (dot(wo,geomN)>0), returning it and its normalized conditional
	// density.  Returns false (only when n and geomN are nearly opposed --
	// see kMinExitValidFraction above) if no valid direction was found
	// within the resample budget; the caller must then not emit this lobe,
	// matching Pdf()/PdfNM()'s own return of 0 for the same configuration
	// so sampler and evaluator stay honest about their shared support.
	bool SampleValidDiffuseExit( const Vector3& n, const Vector3& geomN,
		ISampler& sampler, Vector3& outDir, Scalar& outPdf )
	{
		const Scalar pValid = ExitValidFraction( n, geomN );
		for( int attempt = 0; attempt < kMaxExitResamples; attempt++ ) {
			const Vector3 rv = GeometricUtilities::Perturb( n,
				acos( sqrt(sampler.Get1D()) ), TWO_PI * sampler.Get1D() );
			if( Vector3Ops::Dot( rv, geomN ) > 0 ) {
				outDir = rv;
				outPdf = ( fabs( Vector3Ops::Dot( rv, n ) ) * INV_PI ) / pValid;
				return true;
			}
		}
		return false;
	}
}

TranslucentSPF::TranslucentSPF(
	const IPainter& rF,
	const IPainter& T,
	const IScalarPainter& ext,
	const IScalarPainter& N_,
	const IScalarPainter& scat
	) :
  pRefFront( &rF ),
  pTrans( &T ),
  pExtinction( &ext ),
  pN( &N_ ),
  pScat( &scat )
{
	pRefFront->addref();
	pTrans->addref();
	pExtinction->addref();
	pN->addref();
	pScat->addref();
}

TranslucentSPF::~TranslucentSPF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExtinction );
	safe_release( pN );
	safe_release( pScat );
}

void TranslucentSPF::SetRefFront( const IPainter& v )         { v.addref(); safe_release( pRefFront );   pRefFront   = &v; }
void TranslucentSPF::SetTrans( const IPainter& v )            { v.addref(); safe_release( pTrans );      pTrans      = &v; }
void TranslucentSPF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }
void TranslucentSPF::SetN( const IScalarPainter& v )          { v.addref(); safe_release( pN );          pN          = &v; }
void TranslucentSPF::SetScat( const IScalarPainter& v )       { v.addref(); safe_release( pScat );       pScat       = &v; }

void TranslucentSPF::Scatter( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in/out] Index of refraction stack
			) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a front-lobe direction that
	// validates against the (tilted) shading normal can still point below
	// the geometric surface -- the continuation ray then tunnels into the
	// solid.  The entering-branch front (reflection) lobe is gated against
	// `geomN` below (ray-anchor sweep: `geomN`'s orientation is anchored
	// to ri.ray.Dir(), not to the shading normal, so a glint tilt cannot
	// flip the gate to the wrong side -- this is the correct reference for
	// a REFLECTION, which must stay on the same side the incoming ray
	// arrived from).  `trans` entry/backscatter lobes remain exempt (DL-46
	// sibling audit: distinct site, own recipe, not this row's pattern).
	//
	// The exit-face re-emission (DL-45) is gated separately, against
	// `geomNRaw` WITHOUT the ray-anchor flip: unlike the entry reflection,
	// an exit ray must travel to the OPPOSITE side from where the incoming
	// (interior) ray arrived, i.e. the object's actual outward direction,
	// which for real geometry (SphereGeometry etc.) `ri.vGeomNormal`
	// already reports unconditionally regardless of which side a ray hit
	// it from (IORStackSeeding.h's probe relies on the exact same
	// unflipped convention).  Reusing the ray-anchored `geomN` for the
	// exit gate would validate the WRONG hemisphere -- for a ray already
	// travelling outward (dot(geomNRaw, ri.ray.Dir()) > 0, the
	// characteristic exit signature), the flip below produces `geomN`
	// pointing back INTO the solid.  Degenerate vGeomNormal (SquaredModulus
	// guard, matches GlintModifier.cpp) falls back to the shading normal
	// for both gates, making each a no-op when no independent geometric
	// truth is available.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available, matching DielectricSPF.  The normal-
	// based dot-product test is unreliable for nested translucent objects
	// because a back-scattered ray hitting an enclosing surface from
	// inside the cavity is misclassified as "exiting."
	const bool bEntering = !ior_stack.containsCurrent();

	if( bEntering )
	{
		// Going in
		// Front face
		front.kray = pRefFront->GetColor(ri);
		front.type = ScatteredRay::eRayDiffuse;

		// MaxValue, not channel 0 alone (C2, review round 3): a reflectance
		// painter like (0, 0.5, 0.5) has zero red but non-zero green/blue,
		// and gating on channel 0 alone dropped the WHOLE lobe -- 100% of
		// the green/blue energy along with it.  The exit branch's
		// equivalent gate (below) already uses
		// `ColorMath::MaxValue(front.kray) > 0`; this makes the entry gate
		// consistent with it.
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt( sampler.Get1D() ) ),
				TWO_PI * sampler.Get1D() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			if( Vector3Ops::Dot( front.ray.Dir(), geomN ) > 0 ) {
				scattered.AddScatteredRay( front );
			}
		}

		trans.kray = pTrans->GetColor(ri);
		trans.type = ScatteredRay::eRayTranslucent;

		// MaxValue, not channel 0 alone -- see the front-lobe gate above
		// (C2, review round 3) for the rationale; same trap, same fix.
		if( ColorMath::MaxValue(trans.kray) > 0 ) {
			myonb.FlipW();

			const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
			if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(sampler.Get1D(), 1.0 / (Nfactor[0] + 1.0)) ),
					TWO_PI * sampler.Get1D() );

				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
				trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
				trans.isDelta = false;
				trans.ior_stack = new IORStack( ior_stack );
				// translucent_material has no ior parameter -- there is no
				// second medium to enter, so re-push the enclosing medium's
				// own IOR (RadianceEtaScale then sees before==after and
				// returns exactly 1) rather than fabricating a jump to
				// air's 1.0, which would misprice a translucent object
				// nested inside water or glass.
				trans.ior_stack->push( ior_stack.top() );
				GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
				scattered.AddScatteredRay( trans );
			} else {
				// Add a new ray for each color component
				RISEPel p = trans.kray;
				trans.kray = 0;
				Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
				for( int i=0; i<3; i++ ) {
					rv = GeometricUtilities::Perturb( myonb.w(),
						acos( pow(ptrand.x, 1.0 / (Nfactor[i] + 1.0)) ),
						TWO_PI * ptrand.y );

					trans.kray = 0;
					trans.kray[i] = p[i];
					trans.ray.Set( ri.ptIntersection, rv );
					// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
					const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
					trans.pdf = (Nfactor[i] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[i] );
					trans.isDelta = false;
					// `trans` is ONE local reused across all three loop
					// iterations.  `AddScatteredRay` only clears
					// `delete_stack` to false on the CALLER's local after a
					// successful memcpy (ScatteredRayContainer.cpp) -- it does
					// NOT touch the value that gets memcpy'd INTO the stored
					// copy, which is whatever `delete_stack` reads at the
					// moment of the call.  Iteration 0 starts from the
					// freshly-constructed `trans` (delete_stack==true from
					// ScatteredRay's ctor), so its stored copy correctly owns
					// the stack.  Without this re-arm, iterations 1 and 2
					// would memcpy a stored copy with `delete_stack==false`
					// (left over from iteration 0's post-add reset on the
					// local) alongside a BRAND NEW `IORStack` pointer that
					// copy is the only reference to -- an unconditional,
					// two-per-call leak on the success path.  Re-arming right
					// before every new allocation makes each iteration's
					// stored copy independently own its own stack, matching
					// the exit loop below (which never assigns `ior_stack` at
					// all, so it never needed this).
					//
					// This loop never checks `AddScatteredRay`'s return value
					// (debt 31(b), RENDERING_INTEGRATORS.md), so a container
					// near `kCapacity` can still make an iteration's add FAIL
					// (overflow).  On failure `AddScatteredRay` leaves
					// `delete_stack` exactly as it found it -- true, since we
					// just armed it -- so `trans` correctly RETAINS ownership
					// of that iteration's stack.  If nothing intervened, the
					// pointer would then be silently overwritten by the NEXT
					// iteration's `new IORStack` below, orphaning the still-
					// owned allocation (a real leak distinct from the one
					// above).  Free any such carried-over stack before
					// overwriting the pointer; a no-op on the ordinary path
					// (iteration 0's `ior_stack` starts null, and a
					// successful add already cleared `delete_stack` to false,
					// so there is nothing to free).
					if( trans.delete_stack ) {
						safe_delete( trans.ior_stack );
					}
					trans.delete_stack = true;
					trans.ior_stack = new IORStack( ior_stack );
					// See the comment on the single-color-component branch
					// above: translucent_material has no ior, so re-push
					// the enclosing medium's own IOR rather than 1.0.
					trans.ior_stack->push( ior_stack.top() );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			}
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		const ScalarTriple abt = pExtinction->GetValuesAt(ri);
		const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
		front.kray = ColorMath::exponential( -distance*ab );

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			// Check the scattering parameter
			const ScalarTriple scat_t = pScat->GetValuesAt(ri);
			const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );

			if( ColorMath::MaxValue(scat) > 0 ) {
				// Multiple scatter back
				myonb.FlipW();

				trans.type = ScatteredRay::eRayTranslucent;
				trans.kray = front.kray * scat;

				const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
				if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
					rv = GeometricUtilities::Perturb( myonb.w(),
						acos( pow(sampler.Get1D(), 1.0 / (Nfactor[0] + 1.0)) ),
						TWO_PI * sampler.Get1D() );

					trans.ray.Set( ri.ptIntersection, rv );
					// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
					{
						const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
						trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
						trans.isDelta = false;
					}
					front.kray = front.kray * (RISEPel(1.0,1.0,1.0)-scat);
					// Back-scattered ray stays inside this object, no stack change
					scattered.AddScatteredRay( trans );
				} else {
					// Add a new ray for each color component
					RISEPel p = trans.kray;
					RISEPel f = front.kray;
					trans.kray = 0;
					// `front` is added to `scattered` ONCE, after this whole
					// if/else block (the exit-ray site below) -- unlike
					// `trans`, which is (correctly) added once per channel
					// inside this loop.  So `front.kray` must ACCUMULATE all
					// three channels here, not get reset to 0 each iteration:
					// zero it once, before the loop, and only ever assign
					// (never re-zero) the i'th component inside it.  The
					// previous per-iteration `front.kray = 0;` left every
					// channel but the last (i=2) at zero on the exit ray --
					// the sibling of the entry-loop bug fixed in 74cd56f4.
					front.kray = 0;
					Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
					for( int i=0; i<3; i++ ) {
						rv = GeometricUtilities::Perturb( myonb.w(),
							acos( pow(ptrand.x, 1.0 / (Nfactor[i] + 1.0)) ),
							TWO_PI * ptrand.y );

						trans.kray = 0;
						trans.kray[i] = p[i];
						trans.ray.Set( ri.ptIntersection, rv );
						// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
						{
							const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
							trans.pdf = (Nfactor[i] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[i] );
							trans.isDelta = false;
						}
						front.kray[i] = f[i] * (1.0-scat[i]);
						// Back-scattered ray stays inside this object, no stack change
						scattered.AddScatteredRay( trans );
					}
				}
			}
		}

		// Exit diffuse ray leaves the object — pop from IOR stack.
		// DL-45: the direction must actually be geometrically outward
		// (dot(rv,geomN)>0), not just above the (possibly tilted) shading
		// horizon -- otherwise we'd pop the stack while the ray still
		// travels into the solid.  See SampleValidDiffuseExit above; if no
		// valid direction turns up within its resample budget (only when
		// n and geomN are nearly opposed), this lobe is simply not
		// emitted this trial, exactly like the entry front-lobe's existing
		// geometric gate above.
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( n, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;
			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

void TranslucentSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();

	// Geometric-horizon gates: `geomN` (ray-anchored, mirrors Scatter()'s
	// entry front-lobe gate) and `geomNRaw` (unflipped, DL-45's exit gate
	// -- see the long comment in Scatter() above for why the exit case
	// needs the UNFLIPPED reference).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	const bool bEnteringNM = !ior_stack.containsCurrent();

	if( bEnteringNM )
	{
		// Extinction check
		front.krayNM = GuardedGetColorNM( *pRefFront, ri, nm );
		front.type = ScatteredRay::eRayDiffuse;

		if( front.krayNM > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt(sampler.Get1D()) ),
				TWO_PI * sampler.Get1D() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			if( Vector3Ops::Dot( front.ray.Dir(), geomN ) > 0 ) {
				scattered.AddScatteredRay( front );
			}
		}

		trans.krayNM = GuardedGetColorNM( *pTrans, ri, nm );
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			myonb.FlipW();

			const Scalar Nval = pN->GetValueAtNM(ri,nm);
			rv = GeometricUtilities::Perturb( myonb.w(),
				acos( pow(sampler.Get1D(), 1.0 / (Nval + 1.0)) ),
				TWO_PI * sampler.Get1D() );

			trans.ray.Set( ri.ptIntersection, rv );
			// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
			const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
			trans.pdf = (Nval + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval );
			trans.isDelta = false;
			trans.ior_stack = new IORStack( ior_stack );
			// NM twin of the RGB entry lobe above: translucent_material has
			// no ior parameter, so re-push the enclosing medium's own IOR
			// rather than fabricating a jump to air's 1.0.
			trans.ior_stack->push( ior_stack.top() );
			GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( trans );
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		// The primary-layer transmittance was paid on entry, just as in
		// Scatter(). Each interior segment pays only Beer extinction before
		// splitting into exit and backscatter; multiplying pTrans here again
		// attenuates both children a second time (DL-01).
		front.krayNM = exp(-(pExtinction->GetValueAtNM(ri,nm)*distance));

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( front.krayNM > 0 ) {
			// Check the scattering parameter
			const Scalar scat = pScat->GetValueAtNM(ri,nm);

			if( scat > 0 ) {
				// Multiple scatter back
				myonb.FlipW();
				const Scalar Nval_scat = pN->GetValueAtNM(ri,nm);
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(sampler.Get1D(), 1.0 / (Nval_scat + 1.0)) ),
					TWO_PI * sampler.Get1D() );

				trans.type = ScatteredRay::eRayTranslucent;
				// RGB twin (Scatter(), ~line 182) assigns
				// `trans.kray = front.kray * scat;` -- trans.krayNM was never
				// assigned before this point (ScatteredRay's ctor defaults
				// krayNM to 0), so `*= scat` left this lobe at 0 (dead) in
				// every spectral render.  Mirror the RGB twin: derive it from
				// front.krayNM (the extinction-attenuated pass-through, set
				// just above).
				trans.krayNM = front.krayNM * scat;
				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				{
					const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
					trans.pdf = (Nval_scat + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval_scat );
					trans.isDelta = false;
				}
				// Back-scattered ray stays inside this object, no stack change
				scattered.AddScatteredRay( trans );

				front.krayNM *= (1.0-scat);
			}
		}

		// Exit re-emission is diffuse in both RGB and NM. N controls
		// entry transmission and internal backscatter, not this exit lobe.
		// DL-45: same geometric-horizon requirement as the RGB twin above.
		Scalar exitPdf = 0;
		if( SampleValidDiffuseExit( n, geomNRaw, sampler, rv, exitPdf ) ) {
			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = exitPdf;
			front.isDelta = false;

			front.ior_stack = new IORStack( ior_stack );
			front.ior_stack->pop();
			GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( front );
		}
	}
}

Scalar TranslucentSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	// For the front hemisphere diffuse component, return cosine-weighted PDF
	// For the translucent (back hemisphere) component, return 0
	// (translucent paths have a complex mixed PDF that we approximate as 0)
	//
	// Both the entry reflection and inside-state exit re-emission
	// sample around +onb.w(). Membership only controls which one is being
	// evaluated; it must not reverse which hemisphere has support.
	const bool bFrontFace = !ior_stack.containsCurrent();
	const Vector3& n = ri.onb.w();
	const Scalar cosTheta = Vector3Ops::Dot( wo, n );
	if( cosTheta <= 0 ) return 0;

	// Geometric-horizon gate: a wo the sampler could not have geometrically
	// emitted contributes zero density, in EITHER membership state.  Entry
	// reflection and DL-45's exit re-emission both need this, but against
	// DIFFERENT references -- see the long comment in Scatter() for why:
	// the entry reflection needs `geomN` (ray-anchored, the incident
	// side); the exit re-emission needs the UNFLIPPED `geomNRaw` (the
	// object's actual outward direction, independent of which side the
	// evaluated ray happens to approach from).
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : n;

	if( bFrontFace )
	{
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
		if( Vector3Ops::Dot( wo, geomN ) <= 0 ) return 0;
		// Entry reflection lobe: existing accepted (DL-01/DL-02 unchanged)
		// gate -- Scatter() drops a below-horizon sample rather than
		// resampling, so this is intentionally the UNNORMALIZED cosine
		// density restricted to the valid sub-hemisphere (integrates to
		// P(valid) < 1 there, not 1); that asymmetric energy loss at
		// grazing shading-normal tilt is the pre-existing, reviewed design
		// for this lobe and is out of DL-45's scope.
		return cosTheta * INV_PI;
	}

	// Inside-state diffuse exit re-emission (DL-45): Scatter()/ScatterNM()
	// now RESAMPLE until valid rather than dropping the trial, so this
	// must report the matching NORMALIZED conditional density -- see
	// ExitValidFraction's derivation above.  Gate against the UNFLIPPED
	// `geomNRaw`, matching SampleValidDiffuseExit's own call sites.
	if( Vector3Ops::Dot( wo, geomNRaw ) <= 0 ) return 0;
	return ( cosTheta * INV_PI ) / ExitValidFraction( n, geomNRaw );
}

Scalar TranslucentSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}

