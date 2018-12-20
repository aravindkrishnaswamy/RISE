//////////////////////////////////////////////////////////////////////
//
//  DataDrivenBSDF.cpp - Implements the data driven BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DataDrivenBSDF.h"
#include "../Interfaces/ILog.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

DataDrivenBSDF::DataDrivenBSDF(
	const char* filename 
	) : 
  pInterpolator( 0 )
{
	IReadBuffer* pBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pBuffer, filename );

	// First check for the signature
	if( pBuffer->getInt() != 0xBDF ) {
		GlobalLog()->PrintEasyError( "DataDrivenBSDF:: Signature not found in file!" );
		return;
	}

	// Next check the version
	static const unsigned int our_version = 1;

	if( pBuffer->getUInt() != our_version ) {
		GlobalLog()->PrintEasyError( "DataDrivenBSDF:: File contains bad version info!" );
		return;
	}

	const unsigned int numEmitterPositions = pBuffer->getInt();
	const unsigned int numPatches = pBuffer->getInt();

	for( unsigned int i=0; i<numEmitterPositions; i++ ) {
		const Scalar dEmitterTheta = pBuffer->getDouble();

		EMITTER_SAMPLE brdf;
		brdf.theta = dEmitterTheta;

		EMITTER_SAMPLE btdf;
		btdf.theta = dEmitterTheta;

		for( unsigned int j=0; j<numPatches; j++ )
		{
			{
				PATCH thispatch;
				thispatch.dThetaEnd = PI_OV_TWO - pBuffer->getDouble();
				thispatch.dThetaBegin = PI_OV_TWO - pBuffer->getDouble();
				thispatch.dTheta = (thispatch.dThetaEnd + thispatch.dThetaBegin) * 0.5;
				RISEPel value;
				value.r = pBuffer->getDouble();
				value.g = pBuffer->getDouble();
				value.b = pBuffer->getDouble();
				thispatch.value = RISEPel(value);

				brdf.values.push_back( thispatch );
			}

			{
				PATCH thispatch;
				thispatch.dThetaBegin = pBuffer->getDouble();
				thispatch.dThetaEnd = pBuffer->getDouble();
				thispatch.dTheta = (thispatch.dThetaEnd + thispatch.dThetaBegin) * 0.5;
				RISEPel value;
				value.r = pBuffer->getDouble();
				value.g = pBuffer->getDouble();
				value.b = pBuffer->getDouble();
				thispatch.value = RISEPel(value);

				btdf.values.push_back( thispatch );
			}
		}

		std::reverse( brdf.values.begin(), brdf.values.end() );
		std::reverse( btdf.values.begin(), btdf.values.end() );

		this->brdf.push_back( brdf );
		this->btdf.push_back( brdf );
	}

	pInterpolator = new CatmullRomCubicInterpolator<RISEPel>();
	GlobalLog()->PrintNew( pInterpolator, __FILE__, __LINE__, "uniform b-spline interpolator" );
}

DataDrivenBSDF::~DataDrivenBSDF( )
{
	safe_release( pInterpolator );
}

RISEPel DataDrivenBSDF::ComputeValueForPatchSet( BSDFValuesType::const_iterator it, int idx, int total, Scalar x ) const
{
	const RISEPel y1 = it->values[idx].value;	
	const RISEPel y0 = idx==0 ? y1 : it->values[idx-1].value;
	const RISEPel y2 = it->values[idx+1].value;;
	RISEPel y3 = y2;
	// We don't need this check for the y2, because if you look below our loop iterator only goes until 
	// the second last element.
	if( idx < total-2 ) {
		y3 = it->values[idx+2].value;
	}
	
    // Compute the value for this emitter value
	return pInterpolator->InterpolateValues( y0, y1, y2, y3, x );
}

RISEPel DataDrivenBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	const Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(ri.vNormal,r);
	const Scalar nv = Vector3Ops::Dot(ri.vNormal,v);

	if( nr < 0 || nv < 0 ) {
		return RISEPel(0,0,0);
	}

	const Scalar nv_theta = acos( nv );
	const Scalar nr_theta = acos( nr );

	// First find which emitter dataset to use
	for( BSDFValuesType::const_iterator it=brdf.begin(); it!=brdf.end(); it++ ) {
		if( nv_theta <= it->theta ) {
			// This is the one
			// Find the right patch

			const Scalar xemm = it==brdf.begin() ? (0) : (nv_theta-it->theta)/((it-1)->theta-it->theta);

			PatchValuesListType::const_iterator m;
			unsigned int cnt;
			for( m=it->values.begin(), cnt=0; m!=it->values.end(); m++, cnt++ ) {
				if( (nr_theta < m->dTheta && cnt==0) || m+1 == it->values.end() ) {
					// The very begining, just take the value
					const RISEPel tp = m->value;
					if( it==brdf.begin() ) {
						return tp;
					}

					// Otherwise interpolate in emitter direction
					const RISEPel pp = (cnt+1<(it-1)->values.size())?(it-1)->values[cnt+1].value:(it-1)->values[cnt].value;
                    return ((1.0-xemm)*tp + (xemm*pp));
				} else if( nr_theta >= m->dTheta && nr_theta <= (m+1)->dTheta ) {
					// Otherwise interpolate
					const Scalar xtheta = (nr_theta-m->dTheta)/((m+1)->dTheta-m->dTheta);

					const RISEPel tp = ComputeValueForPatchSet( it, cnt, it->values.size(), xtheta );
					if( it==brdf.begin() ) {
						return tp;
					}

					const RISEPel pp = ComputeValueForPatchSet( (it-1), cnt, (it-1)->values.size(), xtheta );
					return ((1.0-xemm)*tp + (xemm*pp));
				}
			}
		}
	}

	return RISEPel(0,0,0);
}

Scalar DataDrivenBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return 0;
}
