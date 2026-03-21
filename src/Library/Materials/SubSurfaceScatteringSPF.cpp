//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringSPF.cpp - Implementation of the volumetric
//  random walk subsurface scattering SPF.
//
//  The core idea: at surface boundaries we split the ray energy
//  between Fresnel reflection, volumetric scattering (HG phase
//  function), and exit refraction.  Beer-Lambert attenuation is
//  applied for the distance traveled through the medium.
//
//  When roughness > 0, the smooth Fresnel boundary is replaced with
//  a GGX microfacet model (Walter et al. 2007), producing non-delta
//  surface rays that enable BDPT connections at the boundary.
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
// Henyey-Greenstein phase function helpers
//=============================================================

/// Computes the Henyey-Greenstein phase function value
/// \return Phase function value [1/sr]
static Scalar HGPhase( const Scalar g, const Scalar cosTheta )
{
	const Scalar denom = 1.0 + g*g - 2.0*g*cosTheta;
	return (1.0 - g*g) / (4.0 * PI * denom * sqrt(denom));
}

/// Samples a direction from the HG phase function
/// \return The sampled direction
static Vector3 SampleHGDirection(
	const Vector3& incoming,					///< [in] Incoming ray direction (the direction to scatter around)
	const Scalar g,								///< [in] HG asymmetry parameter
	const RandomNumberGenerator& random			///< [in] RNG
	)
{
	Scalar cosTheta;

	if( fabs(g) > 1e-4 ) {
		const Scalar inner = (1.0 - g*g) / (1.0 - g + 2.0*g*random.CanonicalRandom());
		cosTheta = (1.0/(2.0*g)) * (1.0 + g*g - inner*inner);
		if( cosTheta < -1.0 ) cosTheta = -1.0;
		if( cosTheta > 1.0 ) cosTheta = 1.0;
	} else {
		cosTheta = 1.0 - 2.0*random.CanonicalRandom();
	}

	const Scalar a = acos( cosTheta );
	const Scalar phi = TWO_PI * random.CanonicalRandom();

	return GeometricUtilities::Perturb( incoming, a, phi );
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
		// Case A: Ray entering from outside
		//

		const Scalar Ni = ior_stack ? ior_stack->top() : 1.0;

		// Refraction is always smooth (delta) to preserve SSS energy.
		// Roughness only affects the surface reflection (non-delta GGX).
		Vector3 refracted = ri.ray.Dir();

		if( Optics::CalculateRefractedRay( ri.onb.w(), Ni, n, refracted ) )
		{
			const Scalar R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, ri.onb.w(), Ni, n );

			// Refracted ray (enters the medium) — always delta
			{
				ScatteredRay refractedRay;
				refractedRay.type = ScatteredRay::eRayRefraction;
				refractedRay.isDelta = true;
				refractedRay.pdf = 1.0;
				refractedRay.ray = Ray( ri.ptIntersection, refracted );
				refractedRay.kray = RISEPel( 1.0-R, 1.0-R, 1.0-R );

				if( ior_stack ) {
					refractedRay.ior_stack = new IORStack( *ior_stack );
					refractedRay.ior_stack->push( n );
					GlobalLog()->PrintNew( refractedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
				}

				scattered.AddScatteredRay( refractedRay );
			}

			// Reflected ray — rough GGX or smooth delta
			if( alpha > 1e-6 )
			{
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
			// TIR — always smooth
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			reflectedRay.kray = RISEPel( 1.0, 1.0, 1.0 );

			scattered.AddScatteredRay( reflectedRay );
		}
	}
	else
	{
		//
		// Case B: Ray hitting surface from inside the medium
		//

		// Distance traveled through the medium
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.ray.origin, ri.ptIntersection ) );

		// Compute extinction and survival probability
		const RISEPel sa = absorption.GetColor( ri );
		const RISEPel ss = scattering.GetColor( ri );
		const RISEPel sigma_t = sa + ss;

		const RISEPel neg_sigma_t_d = RISEPel( -sigma_t[0]*distance, -sigma_t[1]*distance, -sigma_t[2]*distance );
		const RISEPel survival = ColorMath::exponential( neg_sigma_t_d );
		const RISEPel interaction = RISEPel(1,1,1) - survival;

		RISEPel albedo;
		for( int c=0; c<3; c++ ) {
			albedo[c] = sigma_t[c] > 1e-10 ? ss[c] / sigma_t[c] : 0.0;
		}

		const Scalar Nt = ior_stack ? ior_stack->top() : 1.0;

		// 1 & 3. Surface boundary: smooth refraction (delta), rough or smooth reflection.
		// Refraction is always delta to preserve SSS energy.
		Vector3 refracted = ri.ray.Dir();
		const bool bCanRefract = Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted );

		Scalar R;
		if( bCanRefract ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		// Exit refraction — always delta
		if( bCanRefract )
		{
			ScatteredRay exitRay;
			exitRay.type = ScatteredRay::eRayRefraction;
			exitRay.isDelta = true;
			exitRay.pdf = 1.0;
			exitRay.ray = Ray( ri.ptIntersection, refracted );
			exitRay.kray = survival * RISEPel( 1.0-R, 1.0-R, 1.0-R );

			if( ior_stack ) {
				exitRay.ior_stack = new IORStack( *ior_stack );
				exitRay.ior_stack->pop();
				GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( exitRay );
		}

		// Fresnel reflection — rough GGX or smooth delta
		if( alpha > 1e-6 )
		{
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			const Vector3 n_inward = -(ri.onb.w());
			Vector3 m = GGX_SampleNormal( n_inward, alpha, random );
			if( Vector3Ops::Dot( m, wi ) < 0 ) m = -m;

			const Vector3 wo = Optics::CalculateReflectedRay( ri.ray.Dir(), (-m) );
			const Scalar nWi = fabs( Vector3Ops::Dot( n_inward, wi ) );
			const Scalar nWo = fabs( Vector3Ops::Dot( n_inward, wo ) );
			const Scalar woH = fabs( Vector3Ops::Dot( wo, m ) );
			const Scalar cosThetaM = fabs( Vector3Ops::Dot( m, n_inward ) );

			if( nWo > 1e-10 && woH > 1e-10 && cosThetaM > 1e-10 ) {
				const Scalar G = GGX_G( alpha, wi, wo, n_inward );
				const Scalar pdf = GGX_ReflectionPdf( alpha, m, wo, n_inward );
				const Scalar weight = R * G * woH / (nWi * cosThetaM);

				if( weight > 1e-10 && pdf > 1e-10 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = false;
					reflectedRay.pdf = pdf;
					reflectedRay.ray = Ray( ri.ptIntersection, wo );
					reflectedRay.kray = survival * RISEPel( weight, weight, weight );

					if( ior_stack ) {
						reflectedRay.ior_stack = new IORStack( *ior_stack );
						GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
					}

					scattered.AddScatteredRay( reflectedRay );
				}
			}
		}
		else
		{
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
			reflectedRay.kray = survival * RISEPel( R, R, R );

			if( ior_stack ) {
				reflectedRay.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( reflectedRay );
		}

		// 2. HG volumetric scatter (unchanged by roughness)
		const Scalar albedo_avg = (albedo[0] + albedo[1] + albedo[2]) / 3.0;
		if( albedo_avg > 1e-10 )
		{
			Vector3 scatterDir = SampleHGDirection( ri.ray.Dir(), g, random );

			const Scalar cosTheta = Vector3Ops::Dot(
				Vector3Ops::Normalize( ri.ray.Dir() ),
				Vector3Ops::Normalize( scatterDir )
			);
			const Scalar hgPdf = HGPhase( g, cosTheta );

			ScatteredRay scatterRay;
			scatterRay.type = ScatteredRay::eRayRefraction;
			scatterRay.isDelta = false;
			scatterRay.pdf = hgPdf;

			// Sample the interaction point along the ray path
			const Scalar sigma_t_avg = (sigma_t[0] + sigma_t[1] + sigma_t[2]) / 3.0;
			const Scalar xi = random.CanonicalRandom();
			Scalar t_interact;
			if( sigma_t_avg * distance > 1e-6 ) {
				t_interact = -::log( 1.0 - xi * (1.0 - ::exp(-sigma_t_avg * distance)) ) / sigma_t_avg;
			} else {
				t_interact = xi * distance;
			}
			if( t_interact >= distance ) t_interact = distance * 0.999;
			if( t_interact < 0 ) t_interact = 0;

			const Point3 scatterOrigin = Point3Ops::mkPoint3( ri.ray.origin, ri.ray.Dir() * t_interact );
			scatterRay.ray = Ray( scatterOrigin, scatterDir );
			scatterRay.kray = interaction * albedo;

			if( ior_stack ) {
				scatterRay.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( scatterRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( scatterRay );
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
	// Spectral path: same structure as RGB Scatter() but uses scalar krayNM.
	// GGX geometry is wavelength-independent; IOR variation across wavelengths
	// is handled by the scalar IOR lookup.

	const Scalar cosine = -Vector3Ops::Dot( ri.onb.w(), ri.ray.Dir() );
	const Scalar n = ior.GetColorNM( ri, nm );

	const bool bFromInside = ior_stack ? ior_stack->containsCurrent() : (cosine < NEARZERO);

	if( !bFromInside )
	{
		const Scalar Ni = ior_stack ? ior_stack->top() : 1.0;

		// Refraction is always smooth delta; roughness only affects reflection
		Vector3 refracted = ri.ray.Dir();

		if( Optics::CalculateRefractedRay( ri.onb.w(), Ni, n, refracted ) )
		{
			const Scalar R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, ri.onb.w(), Ni, n );

			// Delta refraction
			{
				ScatteredRay refractedRay;
				refractedRay.type = ScatteredRay::eRayRefraction;
				refractedRay.isDelta = true;
				refractedRay.pdf = 1.0;
				refractedRay.ray = Ray( ri.ptIntersection, refracted );
				refractedRay.krayNM = 1.0 - R;

				if( ior_stack ) {
					refractedRay.ior_stack = new IORStack( *ior_stack );
					refractedRay.ior_stack->push( n );
					GlobalLog()->PrintNew( refractedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
				}

				scattered.AddScatteredRay( refractedRay );
			}

			// Reflection — rough GGX or smooth delta
			if( alpha > 1e-6 )
			{
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
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), -ri.onb.w() ) );
			reflectedRay.krayNM = 1.0;

			scattered.AddScatteredRay( reflectedRay );
		}
	}
	else
	{
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.ray.origin, ri.ptIntersection ) );

		const Scalar sa = absorption.GetColorNM( ri, nm );
		const Scalar ss = scattering.GetColorNM( ri, nm );
		const Scalar st = sa + ss;

		const Scalar survivalVal = ::exp( -st * distance );
		const Scalar interactionVal = 1.0 - survivalVal;
		const Scalar albedoVal = st > 1e-10 ? ss / st : 0.0;

		const Scalar Nt = ior_stack ? ior_stack->top() : 1.0;

		// Smooth refraction, rough or smooth reflection
		Vector3 refracted = ri.ray.Dir();
		const bool bCanRefract = Optics::CalculateRefractedRay( -ri.onb.w(), n, Nt, refracted );

		Scalar R;
		if( bCanRefract ) {
			R = Optics::CalculateDielectricReflectance( ri.ray.Dir(), refracted, -ri.onb.w(), n, Nt );
		} else {
			R = 1.0;
		}

		// Exit refraction — always delta
		if( bCanRefract )
		{
			ScatteredRay exitRay;
			exitRay.type = ScatteredRay::eRayRefraction;
			exitRay.isDelta = true;
			exitRay.pdf = 1.0;
			exitRay.ray = Ray( ri.ptIntersection, refracted );
			exitRay.krayNM = survivalVal * (1.0-R);

			if( ior_stack ) {
				exitRay.ior_stack = new IORStack( *ior_stack );
				exitRay.ior_stack->pop();
				GlobalLog()->PrintNew( exitRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( exitRay );
		}

		// Reflection — rough GGX or smooth delta
		if( alpha > 1e-6 )
		{
			const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
			const Vector3 n_inward = -(ri.onb.w());
			Vector3 m = GGX_SampleNormal( n_inward, alpha, random );
			if( Vector3Ops::Dot( m, wi ) < 0 ) m = -m;

			const Vector3 wo = Optics::CalculateReflectedRay( ri.ray.Dir(), (-m) );
			const Scalar nWi = fabs( Vector3Ops::Dot( n_inward, wi ) );
			const Scalar woH = fabs( Vector3Ops::Dot( wo, m ) );
			const Scalar cosThetaM = fabs( Vector3Ops::Dot( m, n_inward ) );
			const Scalar nWo = fabs( Vector3Ops::Dot( n_inward, wo ) );

			if( nWo > 1e-10 && woH > 1e-10 && cosThetaM > 1e-10 ) {
				const Scalar G = GGX_G( alpha, wi, wo, n_inward );
				const Scalar pdf = GGX_ReflectionPdf( alpha, m, wo, n_inward );
				const Scalar weight = R * G * woH / (nWi * cosThetaM);

				if( weight > 1e-10 && pdf > 1e-10 ) {
					ScatteredRay reflectedRay;
					reflectedRay.type = ScatteredRay::eRayReflection;
					reflectedRay.isDelta = false;
					reflectedRay.pdf = pdf;
					reflectedRay.ray = Ray( ri.ptIntersection, wo );
					reflectedRay.krayNM = survivalVal * weight;

					if( ior_stack ) {
						reflectedRay.ior_stack = new IORStack( *ior_stack );
						GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
					}

					scattered.AddScatteredRay( reflectedRay );
				}
			}
		}
		else
		{
			ScatteredRay reflectedRay;
			reflectedRay.type = ScatteredRay::eRayReflection;
			reflectedRay.isDelta = true;
			reflectedRay.pdf = 1.0;
			reflectedRay.ray = Ray( ri.ptIntersection, Optics::CalculateReflectedRay( ri.ray.Dir(), ri.onb.w() ) );
			reflectedRay.krayNM = survivalVal * R;

			if( ior_stack ) {
				reflectedRay.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( reflectedRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( reflectedRay );
		}

		// HG scatter (unchanged by roughness)
		if( albedoVal > 1e-10 )
		{
			Vector3 scatterDir = SampleHGDirection( ri.ray.Dir(), g, random );
			const Scalar cosTheta = Vector3Ops::Dot(
				Vector3Ops::Normalize( ri.ray.Dir() ),
				Vector3Ops::Normalize( scatterDir )
			);
			const Scalar hgPdf = HGPhase( g, cosTheta );

			const Scalar xi = random.CanonicalRandom();
			Scalar t_interact;
			if( st * distance > 1e-6 ) {
				t_interact = -::log( 1.0 - xi * (1.0 - ::exp(-st * distance)) ) / st;
			} else {
				t_interact = xi * distance;
			}
			if( t_interact >= distance ) t_interact = distance * 0.999;
			if( t_interact < 0 ) t_interact = 0;

			ScatteredRay scatterRay;
			scatterRay.type = ScatteredRay::eRayRefraction;
			scatterRay.isDelta = false;
			scatterRay.pdf = hgPdf;
			const Point3 scatterOrigin = Point3Ops::mkPoint3( ri.ray.origin, ri.ray.Dir() * t_interact );
			scatterRay.ray = Ray( scatterOrigin, scatterDir );
			scatterRay.krayNM = interactionVal * albedoVal;

			if( ior_stack ) {
				scatterRay.ior_stack = new IORStack( *ior_stack );
				GlobalLog()->PrintNew( scatterRay.ior_stack, __FILE__, __LINE__, "ior stack" );
			}

			scattered.AddScatteredRay( scatterRay );
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

	if( alpha > 1e-6 )
	{
		// Rough surface: only reflection uses GGX (refraction is always delta)
		const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
		const Vector3 woNorm = Vector3Ops::Normalize( wo );
		const Vector3 n = bFromInside ? -(ri.onb.w()) : ri.onb.w();

		// Only return PDF for reflection (same hemisphere)
		const bool woSameSide = (Vector3Ops::Dot( woNorm, n ) > 0) == (Vector3Ops::Dot( wi, n ) > 0);

		if( woSameSide ) {
			const Vector3 h = Vector3Ops::Normalize( wi + woNorm );
			return GGX_ReflectionPdf( alpha, h, woNorm, n );
		}

		// Refraction side: delta, so PDF = 0 for non-delta queries
		return 0;
	}

	// Smooth surface: only HG scatter (from inside) has non-zero PDF
	if( !bFromInside ) {
		return 0;
	}

	const Scalar cosTheta = Vector3Ops::Dot(
		Vector3Ops::Normalize( ri.ray.Dir() ),
		Vector3Ops::Normalize( wo )
	);

	return HGPhase( g, cosTheta );
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
