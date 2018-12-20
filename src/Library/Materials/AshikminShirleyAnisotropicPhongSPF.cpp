//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongSPF.cpp - Implementation of the SPF
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
#include "AshikminShirleyAnisotropicPhongSPF.h"
#include "AshikminShirleyAnisotropicPhongBRDF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/RandomNumbers.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

AshikminShirleyAnisotropicPhongSPF::AshikminShirleyAnisotropicPhongSPF( 
	const IPainter& Nu_,
	const IPainter& Nv_,
	const IPainter& Rd_,
	const IPainter& Rs_ 
	) : 
  Nu( Nu_ ),
  Nv( Nv_ ),
  Rd( Rd_ ),
  Rs( Rs_ )
{
	Nu.addref();
	Nv.addref();
	Rd.addref();
	Rs.addref();
}

AshikminShirleyAnisotropicPhongSPF::~AshikminShirleyAnisotropicPhongSPF( )
{
	Nu.release();
	Nv.release();
	Rd.release();
	Rs.release();
}

static bool GenerateSpecularRay( 
	ScatteredRay& specular, 
	Scalar& diffuseFactor,
	Scalar& specFactor,
	const OrthonormalBasis3D& onb,
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand,										///< [in] Random numbers
	const Scalar NU, 
	const Scalar NV,
	const Scalar Rs
	)
{
	// Do this according to the paper
	Scalar	phi = 0;

	// Generate the half-way vector h
	const Scalar phi_root_ns = sqrt((NU+1.0)/(NV+1.0));

	if( ptrand.x < 0.25 )
	{
//		Scalar val = 1.0 - 4*(0.25 - p.x);		reduces to -->
		Scalar val = 4.0 * ptrand.x;
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );		
	}
	else if( ptrand.x < 0.5 )
	{
		Scalar val = 1.0 - 4*(0.5 - ptrand.x);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	}
	else if( ptrand.x < 0.75 )
	{
		Scalar val = 4*(ptrand.x - 0.5);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi += PI;
	}
	else
	{
		Scalar val = 1.0 - 4*(1.0 - ptrand.x);
		phi = atan( phi_root_ns * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}

	const Scalar cos_phi = cos( phi );
	const Scalar sin_phi = sin( phi );
	const Scalar exponent = 1.0 / (cos_phi*cos_phi*NU + sin_phi*sin_phi*NV + 1.0);
	const Scalar cos_theta = pow( ptrand.y, exponent );
	const Scalar sin_theta = sqrt( 1.0 - cos_theta*cos_theta );

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );

	{
		// Set the attenuation to ps from the paper, computed based on the monte carlo section of the paper
		const Vector3 k1 = -ri.ray.dir;
		const Scalar hdotk = Vector3Ops::Dot(h, k1);

		if( hdotk < 0 ) {
			return false;
		}

		// Now compute the ray
		// Rather than using -k1, we just the original ri.ray.dir
		Vector3 k2 = Vector3Ops::Normalize( ri.ray.dir/*-k1*/ + 2.0 * hdotk * h );

		// If the ray goes into the material, then lets not use it
		if( Vector3Ops::Dot(k2, ri.onb.w()) < 0 ) {
			return false;
		}

		// Compute the density of the perturbed ray
		const Scalar hdotn = Vector3Ops::Dot( ri.onb.w(), h );
		const Scalar factor1 = sqrt((NU+1.0)*(NV+1.0)) / TWO_PI;
		const Scalar factor2 = pow( hdotn, (NU*cos_phi*cos_phi + NV*sin_phi*sin_phi));

		const Scalar inv_actual_density = (4.0 * hdotk) / (factor1*factor2);
	
		// Compute the density of what we actually want (from the BRDF)
		Scalar brdf;
		AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors( diffuseFactor, brdf, k2, ri, NU, NV, Rs );

		// The weighing factor is then the inverse of the actual density multiplied by the density
		// we truly want.  This should be as close to 1 as possible, but it won't always be so
		specFactor = inv_actual_density*brdf;

		specular.ray.Set( ri.ptIntersection, k2 );
	}

	return true;
}

void AshikminShirleyAnisotropicPhongSPF::Scatter( 
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RandomNumberGenerator& random,				///< [in] Random number generator
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.dir, ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const RISEPel NU = Nu.GetColor(ri);
	const RISEPel NV = Nv.GetColor(ri);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	RISEPel df=0.0;

	const RISEPel rho = Rs.GetColor(ri);

	if( NU[0] == NU[1] && NU[1] == NU[2] &&
		NV[0] == NV[1] && NV[1] == NV[2]
		)
	{
		Scalar diffuseFactor=0, specFactor=0;
		if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), NU[0], NV[0], ColorMath::MaxValue(rho) ) ) {
			if( specFactor > 1.0 ) {
				specFactor = 1.0;
			}
			
			specular.kray = rho * specFactor; 
			scattered.AddScatteredRay( specular );
		}
		df = diffuseFactor;
	}
	else 
	{
		const Point2 ptrand(random.CanonicalRandom(),random.CanonicalRandom());
		for( int i=0; i<3; i++ ) {
			Scalar specFactor=0;
			if( GenerateSpecularRay( specular, df[i], specFactor, myonb, ri, ptrand, NU[i], NV[i], rho[i] ) ) {
				if( specFactor > 1.0 ) {
					specFactor = 1.0;
				}
				specular.kray = 0.0;
				specular.kray[i] = rho[i] * specFactor; 
				scattered.AddScatteredRay( specular );
			}
		}
	}

	// The rest is diffuse
	ScatteredRay	diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;
	diffuse.kray = Rd.GetColor(ri)*df;
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) ) );
	
	scattered.AddScatteredRay( diffuse );
}

void AshikminShirleyAnisotropicPhongSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	OrthonormalBasis3D	myonb = ri.onb;
	if( Vector3Ops::Dot(ri.ray.dir, ri.onb.w()) > NEARZERO ) {
		myonb.FlipW();
	}

	const Scalar NU = Nu.GetColorNM(ri,nm);
	const Scalar NV = Nv.GetColorNM(ri,nm);

	ScatteredRay	specular;
	specular.type = ScatteredRay::eRayReflection;
	Scalar specFactor=0;
	Scalar diffuseFactor=0;

	const Scalar rho = Rs.GetColorNM(ri,nm);

	if( GenerateSpecularRay( specular, diffuseFactor, specFactor, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), NU, NV, rho ) ) {
		specular.krayNM = rho * specFactor; 
		scattered.AddScatteredRay( specular );
	}

	// The rest is diffuse
	if( specFactor < 1.0 ) {
		ScatteredRay	diffuse;
		diffuse.type = ScatteredRay::eRayDiffuse;
		diffuse.krayNM = Rd.GetColorNM(ri,nm) * diffuseFactor;
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, Point2(random.CanonicalRandom(),random.CanonicalRandom()) ) );
		
		scattered.AddScatteredRay( diffuse );
	}
}

