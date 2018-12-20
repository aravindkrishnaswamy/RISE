//////////////////////////////////////////////////////////////////////
//
//  SchlickSPF.cpp - Implementation of the Oren-Nayar SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SchlickSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

SchlickSPF::SchlickSPF(
	const IPainter& diffuse, 
	const IPainter& specular,
	const IPainter& roughness,
	const IPainter& isotropy
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pRoughness( roughness ),
  pIsotropy( isotropy )
{
	pDiffuse.addref();
	pSpecular.addref();
	pRoughness.addref();
	pIsotropy.addref();
}

SchlickSPF::~SchlickSPF( )
{
	pDiffuse.release();
	pSpecular.release();
	pRoughness.release();
	pIsotropy.release();
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
		Scalar& fresnel,
		const OrthonormalBasis3D& onb,								///< [in] Orthonormal basis in 3D
		const RayIntersectionGeometric& ri,							///< [in] Ray intersection information
		const Point2& random,										///< [in] Two random numbers
		const Scalar r,
		const Scalar p
		)
{
	specular.type = ScatteredRay::eRayReflection;

	// Use the warping function to perturb the reflected ray
	const Scalar xi = random.x;
	const Scalar b = random.y;

	const Scalar sqr_p = p*p;

	Scalar phi = 0;
	if( b < 0.25 )
	{
		const Scalar val = 4.0 * b;
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
	}
	else if( b < 0.5 )
	{
		const Scalar val = 1.0 - 4*(0.5 - b);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi = PI - phi;
	}
	else if( b < 0.75 )
	{
		const Scalar val = 4*(b - 0.5);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi += PI;
	}
	else
	{
		const Scalar val = 1.0 - 4*(1.0 - b);
		const Scalar sqr_b = val*val;
		phi = sqrt((sqr_p*sqr_b)/(1-sqr_b+sqr_b*sqr_p)) * PI_OV_TWO;
		phi = TWO_PI - phi;
	}

	const Scalar theta = acos(sqrt(xi/(r-xi*r+xi)));

	const Scalar cos_phi = cos(phi);
	const Scalar sin_phi = sin(phi);

	const Scalar cos_theta = cos(theta);
	const Scalar sin_theta = sin(theta);

	const Vector3	a( cos_phi*sin_theta, sin_phi*sin_theta, cos_theta );

	// Generate the actual vector from the half-way vector
	const Vector3	h(
		  ri.onb.u().x*a.x + ri.onb.v().x*a.y + ri.onb.w().x*a.z, 
	   	  ri.onb.u().y*a.x + ri.onb.v().y*a.y + ri.onb.w().y*a.z, 
		  ri.onb.u().z*a.x + ri.onb.v().z*a.y + ri.onb.w().z*a.z );

	const Scalar hdotk = Vector3Ops::Dot(h, -ri.ray.dir);

	fresnel = ::pow(1-hdotk,5);

	if( hdotk > 0 ) {
		Vector3 ret = Vector3Ops::Normalize( ri.ray.dir + 2.0 * hdotk * h );
		specular.ray.Set( ri.ptIntersection, ret );
	}	
}

void SchlickSPF::Scatter(
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
		d.kray = pDiffuse.GetColor(ri);
		scattered.AddScatteredRay( d );
	}

	const RISEPel roughness = pRoughness.GetColor(ri);
	const RISEPel isotropy = pIsotropy.GetColor(ri);

	if( roughness[0] == roughness[1] && roughness[1] == roughness[2] &&
		isotropy[0] == isotropy[1] && isotropy[1] == isotropy[2] )
	{
		Scalar fresnel = 0;
		GenerateSpecularRay( s, fresnel, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), roughness[0], isotropy[0] );

		if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
			const RISEPel rho = pSpecular.GetColor(ri);
			s.kray = rho + (RISEPel(1.0,1.0,1.0)-rho) * fresnel;
			scattered.AddScatteredRay( s );
		}
	} 
	else 
	{
		const Point2 ptrand( random.CanonicalRandom(),random.CanonicalRandom() );
		const RISEPel rho = pSpecular.GetColor(ri);

		for( int i=0; i<3; i++ ) {
			Scalar fresnel = 0;
			GenerateSpecularRay( s, fresnel, myonb, ri, ptrand, roughness[i], isotropy[i] );

			if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
				s.kray = 0;
				s.kray[i] = rho[i] + (1.0-rho[i]) * fresnel;
				scattered.AddScatteredRay( s );
			}
		}
	}	
}

void SchlickSPF::ScatterNM( 
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
	Scalar fresnel = 0;
	GenerateDiffuseRay( d, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()) );
	GenerateSpecularRay( s, fresnel, myonb, ri, Point2(random.CanonicalRandom(),random.CanonicalRandom()), pRoughness.GetColorNM(ri,nm), pIsotropy.GetColorNM(ri,nm) );
	
	if( Vector3Ops::Dot( d.ray.dir, ri.onb.w() ) > 0.0 ) {
		d.krayNM = pDiffuse.GetColorNM(ri,nm);
		scattered.AddScatteredRay( d );
	}

	if( Vector3Ops::Dot( s.ray.dir, ri.onb.w() ) > 0.0 ) {
		const Scalar rho = pSpecular.GetColorNM(ri,nm);
		s.krayNM = rho + (1.0-rho) * fresnel;
		scattered.AddScatteredRay( s );
	}
}

