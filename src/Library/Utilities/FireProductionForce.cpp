//////////////////////////////////////////////////////////////////////
//
//  FireProductionForce.cpp - strict-binary32 production force primitives
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionForce.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace RISE
{
	namespace
	{
		bool Fail( std::string* error, const char* message ) noexcept
		{
			if( error ) try { *error=message; } catch( const std::bad_alloc& ) {}
			return false;
		}
	}

	bool EvaluateFireProductionVremanEddyViscosity(
		const FireProductionVremanInput& input,
		float& eddyViscosityM2PerS,
		std::string* error )
	{
		eddyViscosityM2PerS=0.0f;
		if( !(input.coefficient>=0.0f)||!std::isfinite(input.coefficient) )
			return Fail(error,"production Vreman coefficient is invalid");
		float alphaSquared=0.0f;
		float beta[3][3]={};
		for( unsigned int i=0u;i<3u;++i ) for( unsigned int j=0u;j<3u;++j ) {
			const float alpha=input.velocityGradientPerS[3u*i+j];
			if( !std::isfinite(alpha) )
				return Fail(error,"production Vreman gradient is nonfinite");
			alphaSquared+=alpha*alpha;
			if( !std::isfinite(alphaSquared) )
				return Fail(error,"production Vreman gradient norm overflowed");
		}
		for( unsigned int m=0u;m<3u;++m ) {
			const float width=input.directionalWidthsM[m];
			if( !(width>0.0f)||!std::isfinite(width) )
				return Fail(error,"production Vreman filter width is invalid");
			const float widthSquared=width*width;
			if( !std::isfinite(widthSquared) )
				return Fail(error,"production Vreman filter width overflowed");
			for( unsigned int i=0u;i<3u;++i ) for( unsigned int j=0u;j<3u;++j ) {
				beta[i][j]+=widthSquared*input.velocityGradientPerS[3u*m+i]*
					input.velocityGradientPerS[3u*m+j];
				if( !std::isfinite(beta[i][j]) )
					return Fail(error,"production Vreman beta tensor overflowed");
			}
		}
		if( alphaSquared==0.0f ) {
			if( error ) error->clear();
			return true;
		}
		const float rawBBeta=
			beta[0][0]*beta[1][1]-beta[0][1]*beta[0][1]+
			beta[0][0]*beta[2][2]-beta[0][2]*beta[0][2]+
			beta[1][1]*beta[2][2]-beta[1][2]*beta[1][2];
		if( !std::isfinite(rawBBeta) )
			return Fail(error,"production Vreman B_beta overflowed");
		const float bBeta=std::max(0.0f,rawBBeta);
		eddyViscosityM2PerS=input.coefficient*std::sqrt(bBeta/alphaSquared);
		if( !(eddyViscosityM2PerS>=0.0f)||!std::isfinite(eddyViscosityM2PerS) ) {
			eddyViscosityM2PerS=0.0f;
			return Fail(error,"production Vreman viscosity is invalid");
		}
		if( error ) error->clear();
		return true;
	}

	bool SelectFireProductionViscousSchedule(
		float timeStepS,
		float outwardLambdaPerS,
		FireProductionViscousSchedule& schedule,
		std::string* error )
	{
		schedule=FireProductionViscousSchedule();
		if( !(timeStepS>0.0f)||!std::isfinite(timeStepS)||
			!(outwardLambdaPerS>=0.0f)||!std::isfinite(outwardLambdaPerS) )
			return Fail(error,"production viscous schedule input is invalid");
		const double infinity=std::numeric_limits<double>::infinity();
		const double work=std::nextafter((static_cast<double>(timeStepS)*
			static_cast<double>(outwardLambdaPerS))*0.5,infinity);
		if( !std::isfinite(work)||work>
			static_cast<double>(std::numeric_limits<std::uint32_t>::max()) )
			return Fail(error,"production viscous schedule overflowed");
		std::uint32_t count=std::max<std::uint32_t>(1u,
			static_cast<std::uint32_t>(std::ceil(work)));
		for( ;; ) {
			if( count>8u ) return Fail(error,
				"production viscous schedule exceeds eight substeps");
			const float substep=timeStepS/static_cast<float>(count);
			const double product=std::nextafter(static_cast<double>(substep)*
				static_cast<double>(outwardLambdaPerS),infinity);
			if( !std::isfinite(substep)||!(substep>0.0f)||!std::isfinite(product) )
				return Fail(error,"production viscous represented substep is invalid");
			if( product<=2.0 ) {
				schedule.substepCount=count;schedule.substepTimeS=substep;
				schedule.outwardWork=work;schedule.representedProductUpper=product;
				if( error ) error->clear();
				return true;
			}
			++count;
		}
	}
}
