//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianBRDF.cpp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WardAnisotropicEllipticalGaussianBRDF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

WardAnisotropicEllipticalGaussianBRDF::WardAnisotropicEllipticalGaussianBRDF( 
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

WardAnisotropicEllipticalGaussianBRDF::~WardAnisotropicEllipticalGaussianBRDF( )
{
	diffuse.release();
	specular.release();
	alphax.release();
	alphay.release();
}

template< class T >
static void ComputeFactors( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const T& alphax,
	const T& alphay
	)
{
	Vector3 l = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Vector3& n = ri.onb.w();
	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nl = Vector3Ops::Dot(n,l);	

	if( (nr >= 0) && (nl >= 0) ) {
		diffuse = INV_PI;

		const Vector3 h = Vector3Ops::Normalize(l+r);
		const Scalar nh = Vector3Ops::Dot(n,h);

		const Scalar phi = acos(Vector3Ops::Dot(ri.onb.u(),Vector3Ops::Normalize(h-(nh*n))));

		const Scalar first = 1.0 / (sqrt(nr*nl));
		const Scalar tanh = tan(acos(nh));

		const T inside = (cos(phi)*cos(phi))/(alphax*alphax) + (sin(phi)*sin(phi))/(alphay*alphay);
		const T second = exp( -(tanh*tanh)*inside );
		const T third = 1.0 / (FOUR_PI*alphax*alphay);

		specular = first*second*third;
	}
}

RISEPel WardAnisotropicEllipticalGaussianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel d, s;
	ComputeFactors<RISEPel>( d, s, vLightIn, ri, alphax.GetColor(ri), alphay.GetColor(ri) );

	return d*diffuse.GetColor(ri) + s*specular.GetColor(ri);
}

Scalar WardAnisotropicEllipticalGaussianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar d=0, s=0;
	ComputeFactors<Scalar>( d, s, vLightIn, ri, alphax.GetColorNM(ri,nm), alphay.GetColorNM(ri,nm) );
	
	return d*diffuse.GetColorNM(ri,nm) + s*specular.GetColorNM(ri,nm);
}
