//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianSPF.cpp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WardIsotropicGaussianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

WardIsotropicGaussianSPF::WardIsotropicGaussianSPF( 
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IPainter& alpha_
	) :
  diffuse( diffuse_ ),
  specular( specular_ ),
  alpha( alpha_ )
{
	diffuse.addref();
	specular.addref();
	alpha.addref();
}

WardIsotropicGaussianSPF::~WardIsotropicGaussianSPF( )
{
	diffuse.release();
	specular.release();
	alpha.release();
}

static inline void GenerateDiffuseRay( 
		ScatteredRay& diffuse, 
		const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& ptrand										///< [in] Random numbers
		)
{
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( onb, ptrand ) );
}

static void GenerateSpecularRay( 
	ScatteredRay& specular,
	const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
	const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
	const Point2& random,										///< [in] Two random numbers
	const Scalar alpha
	)
{
	specular.type = ScatteredRay::eRayReflection;

	const Scalar phi = TWO_PI * random.x;
	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);
	
	const Scalar theta = atan(alpha*(sqrt(-log(random.y))));

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  ri.onb.u().x*a.x + ri.onb.v().x*a.y + ri.onb.w().x*a.z, 
	   	  ri.onb.u().y*a.x + ri.onb.v().y*a.y + ri.onb.w().y*a.z, 
		  ri.onb.u().z*a.x + ri.onb.v().z*a.y + ri.onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.dir);

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.dir + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );
	}
}

void WardIsotropicGaussianSPF::Scatter( 
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

	ScatteredRay d, s;	
	GenerateDiffuseRay( d, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()) );

	if( Vector3Ops::Dot( d.ray.dir, ri.onb.w() ) > 0.0 ) {
		d.kray = diffuse.GetColor(ri);
		scattered.AddScatteredRay( d );
	}

	const RISEPel a = alpha.GetColor(ri);

	if( a[0] == a[1] && a[1] == a[2] )
	{
		GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), a[0] );

		if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
			s.kray = specular.GetColor(ri);
			scattered.AddScatteredRay( s );
		}
	} 
	else
	{
		const Point2 ptrand( random.CanonicalRandom(),random.CanonicalRandom() );
		const RISEPel spec = specular.GetColor(ri);
		for( int i=0; i<3; i++ ) {
			GenerateSpecularRay( s, myonb, ri, ptrand, a[i] );

			if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i];
				scattered.AddScatteredRay( s );
			}
		}
	}
}


void WardIsotropicGaussianSPF::ScatterNM( 
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

	ScatteredRay d, s;	
	GenerateDiffuseRay( d, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()) );
	GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), alpha.GetColorNM(ri,nm) );
	
	if( Vector3Ops::Dot( d.ray.dir, ri.onb.w() ) > 0.0 ) {
		d.krayNM = diffuse.GetColorNM(ri,nm);
		scattered.AddScatteredRay( d );
	}
	if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
		s.krayNM = specular.GetColorNM(ri,nm);
		scattered.AddScatteredRay( s );
	}
}

