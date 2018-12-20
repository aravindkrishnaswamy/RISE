//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianBRDF.cpp
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
#include "WardIsotropicGaussianBRDF.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

WardIsotropicGaussianBRDF::WardIsotropicGaussianBRDF( 
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

WardIsotropicGaussianBRDF::~WardIsotropicGaussianBRDF( )
{
	diffuse.release();
	specular.release();
	alpha.release();
}

template< class T >
static void ComputeFactors( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const Vector3& n,
	const T& alpha
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) && (nv >= NEARZERO) ) {
		diffuse = INV_PI;

		const Vector3 h = Vector3Ops::Normalize(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

		const Scalar first = 1.0 / (sqrt(nr*nv));
		const Scalar tanh = tan(acos(hn));
		const T sqralpha = alpha*alpha;
		const T second = exp( -(tanh*tanh)/sqralpha ) / (FOUR_PI*sqralpha);

		specular = first*second;
	}
}

RISEPel WardIsotropicGaussianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel d, s;
	ComputeFactors<RISEPel>( d, s, vLightIn, ri, ri.onb.w(), alpha.GetColor(ri) );
	
	return d*diffuse.GetColor(ri) + s*specular.GetColor(ri);
}

Scalar WardIsotropicGaussianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar d=0, s=0;
	ComputeFactors<Scalar>( d, s, vLightIn, ri, ri.onb.w(), alpha.GetColorNM(ri,nm) );
	
	return d*diffuse.GetColorNM(ri,nm) + s*specular.GetColorNM(ri,nm);
}
