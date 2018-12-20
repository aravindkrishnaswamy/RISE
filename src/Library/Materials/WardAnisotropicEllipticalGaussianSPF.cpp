//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianSPF.cpp
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
#include "WardAnisotropicEllipticalGaussianSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

WardAnisotropicEllipticalGaussianSPF::WardAnisotropicEllipticalGaussianSPF( 
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IPainter& alphax_,
	const IPainter& alphay_
	) :
  diffuse( diffuse_ ),
  specular( specular_ ),
  alphax( alphax_ ),
  alphay( alphay_ )
{
	diffuse.addref();
	specular.addref();
	alphax.addref();
	alphay.addref();
}

WardAnisotropicEllipticalGaussianSPF::~WardAnisotropicEllipticalGaussianSPF( )
{
	diffuse.release();
	specular.release();
	alphax.release();
	alphay.release();
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
		const Scalar alphax,
		const Scalar alphay
		)
{
	specular.type = ScatteredRay::eRayReflection;

	// Use the warping function to perturb the reflected ray
	const Scalar xi = random.x;
	const Scalar alpha_ratio = (alphay/alphax);
	Scalar phi = 0;
	if( xi < 0.25 )
	{
//		Scalar val = 1.0 - 4*(0.25 - p.x);		reduces to -->
		Scalar val = 4.0 * xi;
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );		
	}
	else if( xi < 0.5 )
	{
		Scalar val = 1.0 - 4*(0.5 - xi);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	}
	else if( xi < 0.75 )
	{
		Scalar val = 4*(xi - 0.5);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi += PI;
	}
	else
	{
		Scalar val = 1.0 - 4*(1.0 - xi);
		phi = atan( alpha_ratio * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}


	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);
	
	const Scalar denom = (cos_phi*cos_phi)/(alphax*alphax) + (sin_phi*sin_phi)/(alphay*alphay);
	const Scalar theta = atan( sqrt( -log(random.y) / denom ));

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.dir);

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.dir + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );
	}	
}

void WardAnisotropicEllipticalGaussianSPF::Scatter( 
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

	const RISEPel ax = alphax.GetColor(ri);
	const RISEPel ay = alphay.GetColor(ri);

	if( ax[0] == ax[1] && ax[1] == ax[2] && 
		ay[0] == ay[1] && ay[1] == ay[2] )
	{
		GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), ax[0], ay[0] );

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
			GenerateSpecularRay( s, myonb, ri, ptrand, alphax.GetColor(ri)[i], alphay.GetColor(ri)[i] );

			if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = spec[i];
				scattered.AddScatteredRay( s );
			}
		}
	}
}


void WardAnisotropicEllipticalGaussianSPF::ScatterNM( 
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
	GenerateSpecularRay( s, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), alphax.GetColorNM(ri,nm), alphay.GetColorNM(ri,nm) );
	
	if( Vector3Ops::Dot( d.ray.dir, ri.onb.w() ) > 0.0 ) {
		d.krayNM = diffuse.GetColorNM(ri,nm);
		scattered.AddScatteredRay( d );
	}
	if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
		s.krayNM = specular.GetColorNM(ri,nm);
		scattered.AddScatteredRay( s );
	}
}

