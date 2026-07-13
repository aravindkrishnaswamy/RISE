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

	// Geometric-horizon gate: GlintModifier can tilt the shading normal up
	// to 60 deg off the true surface, so a Fresnel reflection direction that
	// validates against the (tilted) shading normal can still point below the
	// geometric surface -- the continuation ray then tunnels into the solid.
	// Orient the geometric normal to the crossing's outward side (entering:
	// +onb.w(); leaving: -onb.w() -- the side the reflection must stay on).
	// Degenerate
	// vGeomNormal (SquaredModulus guard, matches GlintModifier.cpp) falls back
	// to the shading normal, making the gate a no-op.
	//
	// NOTE (ray-anchor sweep): unlike the myonb.FlipW()-derived anchors
	// elsewhere (GGXSPF et al.), nEff here is NOT re-derived from a per-hit
	// Dot(rayDir, tiltable shading normal) test -- bFromInside is ground
	// truth from the IOR stack (which side of the interface the walk is
	// physically on), independent of any glint tilt.  This is provably
	// equivalent to the ray-anchor rule used elsewhere: for the entering
	// case the ray opposes the true outward normal (Dot(rayDir,Ng)<0) so
	// nEff=+onb.w() picks the same side as a direct ray-anchor would; for
	// the leaving case the ray travels outward (Dot(rayDir,Ng)>0) so
	// nEff=-onb.w() again matches.  Left as stack-anchored rather than
	// rewritten to Dot(geomNRaw, ri.ray.Dir()) to avoid perturbing
	// well-tested crossing logic for a change that is a no-op here.
	const Vector3 nEff = bFromInside ? -ri.onb.w() : ri.onb.w();
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : nEff;
	const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, nEff ) >= 0 ) ? geomNRaw : -geomNRaw;

	if( bFromInside )
	{
		// Determine the exit IOR: the medium the ray enters after leaving
		// this object.  Pop the current object from a temporary copy of
		// the stack so that top() reveals the underlying medium's IOR.
		IORStack exitStack( ior_stack );
		exitStack.pop();
		Scalar exitIOR = exitStack.top();

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

	// reflect ray
	{
		if( bFromInside ) {
			fresnel.ior_stack = new IORStack( ior_stack );
			GlobalLog()->PrintNew( fresnel.ior_stack, __FILE__, __LINE__, "ior stack" );
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
		} else {
			fresnel.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
		}
	}

	if( Vector3Ops::Dot( fresnel.ray.Dir(), geomN ) <= 0 ) {
		if( ref >= 1.0 ) {
			// Mandatory reflection: ref forced to 1.0 above means TIR (or an
			// exact grazing Fresnel of 1.0) -- no transmission lobe will be
			// emitted at all (see the `bDielectric && ref < 1.0` gate below),
			// so dropping the Fresnel lobe here would be total, deterministic
			// energy loss.  TIR has no companion channel: re-derive the
			// reflection direction about the TRUE geometric normal instead of
			// the shading normal.  This is guaranteed to satisfy the gate (no
			// re-check needed): for a ray arriving against geomN,
			// dot(reflect(d,geomN), geomN) = -dot(d,geomN) > 0 -- holds
			// unconditionally here since geomN's orientation (nEff, see the
			// NOTE above) is provably ray-anchored, so dot(d,geomN) < 0 always.
			fresnel.ray.SetDir( Optics::CalculateReflectedRay( ri.ray.Dir(), geomN ) );
		} else {
			bFresnel = false;
		}
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

		// Use the warping function for a Phong based PDF
		if( alpha > 0 && alpha < PI_OV_TWO ) {
			dielectric.ray.SetDir(GeometricUtilities::Perturb(
				dielectric.ray.Dir(),
				alpha,
				TWO_PI * random.y
				));
		}

		if( !bFromInside && Vector3Ops::Dot(dielectric.ray.Dir(), ri.onb.w()) > -NEARZERO ) {
			bDielectric = false;
		} else if( bFromInside && Vector3Ops::Dot(dielectric.ray.Dir(), ri.onb.w()) < NEARZERO ) {
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
		// We have dispersion, so we must process each component seperately
		// Representative per-channel wavelengths for the AR-coating reflectance
		// on the RGB preview path (sRGB primary dominant wavelengths, nm).
		static const Scalar kARChannelNM[3] = { 611.0, 549.0, 465.0 };
		Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
		for( int i=0; i<3; i++ ) {
			DoSingleRGBComponent( ri, ptrand, scattered, ior_stack, i+1, iorVals.v[i], scatVals.v[i], cosine, kARChannelNM[i] );
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
