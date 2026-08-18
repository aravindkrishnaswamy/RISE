//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvection.cpp - strict-binary32 CPU oracle for production PPM
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionAdvection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace RISE
{
	namespace
	{
		bool Fail( std::string* error, const std::string& message )
		{
			if( error ) *error=message;
			return false;
		}

		std::size_t ValueIndex( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t cell )
		{
			return (component*request.lineCount+line)*request.lineLength+cell;
		}

		std::size_t FluxIndex( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t face )
		{
			return (component*request.lineCount+line)*(request.lineLength+1u)+face;
		}

		float Sample( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, long cell )
		{
			const long count=static_cast<long>(request.lineLength);
			if( request.boundary==FireProductionRemapPeriodic ) {
				long wrapped=cell%count;
				if( wrapped<0 ) wrapped+=count;
				return request.values[ValueIndex(request,component,line,
					static_cast<std::size_t>(wrapped))];
			}
			if( cell<0 ) {
				const bool inflow=request.boundary==FireProductionRemapPressureOpen&&
					request.faceVelocityMPerS[line*(request.lineLength+1u)]>0.0f;
				return inflow ? request.ambientValues[component] :
					request.values[ValueIndex(request,component,line,0u)];
			}
			if( cell>=count ) {
				const bool inflow=request.boundary==FireProductionRemapPressureOpen&&
					request.faceVelocityMPerS[line*(request.lineLength+1u)+request.lineLength]<0.0f;
				return inflow ? request.ambientValues[component] :
					request.values[ValueIndex(request,component,line,request.lineLength-1u)];
			}
			return request.values[ValueIndex(request,component,line,static_cast<std::size_t>(cell))];
		}

		void UnlimitedEdges( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t cell,
			float& left, float& right )
		{
			const long i=static_cast<long>(cell);
			const float im2=Sample(request,component,line,i-2);
			const float im1=Sample(request,component,line,i-1);
			const float center=Sample(request,component,line,i);
			const float ip1=Sample(request,component,line,i+1);
			const float ip2=Sample(request,component,line,i+2);
			left=(7.0f*(im1+center)-(im2+ip1))/12.0f;
			right=(7.0f*(center+ip1)-(im1+ip2))/12.0f;
		}

		void ParabolaDeviationRange( float leftDeviation, float rightDeviation,
			float& minimum, float& maximum )
		{
			minimum=std::min(leftDeviation,rightDeviation);
			maximum=std::max(leftDeviation,rightDeviation);
			const float quadratic=3.0f*(leftDeviation+rightDeviation);
			const float linear=-4.0f*leftDeviation-2.0f*rightDeviation;
			if( quadratic!=0.0f ) {
				const float stationary=-linear/(2.0f*quadratic);
				if( stationary>0.0f&&stationary<1.0f ) {
					const float value=(quadratic*stationary+linear)*stationary+leftDeviation;
					minimum=std::min(minimum,value);
					maximum=std::max(maximum,value);
				}
			}
		}

		float PartialCellIntegral( float center, float left, float right, float fraction )
		{
			const float q6=6.0f*center-3.0f*(left+right);
			const float fraction2=fraction*fraction;
			return left*fraction+0.5f*(right-left+q6)*fraction2-
				(q6/3.0f)*fraction2*fraction;
		}

		float LocalAntiderivative( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, float positionCells,
			const std::vector<float>& left, const std::vector<float>& right,
			const std::vector<float>& prefix )
		{
			if( positionCells<=0.0f ) return 0.0f;
			if( positionCells>=static_cast<float>(request.lineLength) )
				return prefix[(component*request.lineCount+line)*(request.lineLength+1u)+
					request.lineLength];
			const std::size_t cell=std::min(request.lineLength-1u,
				static_cast<std::size_t>(std::floor(positionCells)));
			const float fraction=positionCells-static_cast<float>(cell);
			const std::size_t value=ValueIndex(request,component,line,cell);
			const std::size_t base=(component*request.lineCount+line)*(request.lineLength+1u);
			return prefix[base+cell]+
				PartialCellIntegral(request.values[value],left[value],right[value],fraction);
		}

		float PeriodicForwardIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, float beginningCells, float endCells,
			const std::vector<float>& left,
			const std::vector<float>& right, const std::vector<float>& prefix )
		{
			const float count=static_cast<float>(request.lineLength);
			const std::size_t base=(component*request.lineCount+line)*(request.lineLength+1u);
			const float total=prefix[base+request.lineLength];
			float start=beginningCells-std::floor(beginningCells/count)*count;
			if( start>=count ) start=0.0f;
			float remaining=endCells-beginningCells;
			const float firstLength=std::min(remaining,count-start);
			float result=LocalAntiderivative(request,component,line,start+firstLength,
				left,right,prefix)-LocalAntiderivative(request,component,line,start,left,right,prefix);
			remaining-=firstLength;
			if( remaining>0.0f ) {
				const float cycles=std::floor(remaining/count);
				result+=cycles*total;
				remaining-=cycles*count;
				result+=LocalAntiderivative(request,component,line,remaining,left,right,prefix);
			}
			return result;
		}

		float PeriodicIntervalIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, float beginningCells, float endCells,
			const std::vector<float>& left, const std::vector<float>& right,
			const std::vector<float>& prefix )
		{
			return endCells>=beginningCells ? PeriodicForwardIntegral(request,component,line,
				beginningCells,endCells,left,right,prefix) : -PeriodicForwardIntegral(request,
				component,line,endCells,beginningCells,left,right,prefix);
		}

		float OpenForwardIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, float beginningCells, float endCells,
			float faceVelocity, const std::vector<float>& left,
			const std::vector<float>& right, const std::vector<float>& prefix )
		{
			const float count=static_cast<float>(request.lineLength);
			const float leftExtension=request.boundary==FireProductionRemapPressureOpen&&
				faceVelocity>0.0f ? request.ambientValues[component] :
				request.values[ValueIndex(request,component,line,0u)];
			const float rightExtension=request.boundary==FireProductionRemapPressureOpen&&
				faceVelocity<0.0f ? request.ambientValues[component] :
				request.values[ValueIndex(request,component,line,request.lineLength-1u)];
			float result=0.0f;
			if( beginningCells<0.0f )
				result+=(std::min(endCells,0.0f)-beginningCells)*leftExtension;
			const float interiorBeginning=std::max(beginningCells,0.0f);
			const float interiorEnd=std::min(endCells,count);
			if( interiorEnd>interiorBeginning ) result+=
				LocalAntiderivative(request,component,line,interiorEnd,left,right,prefix)-
				LocalAntiderivative(request,component,line,interiorBeginning,left,right,prefix);
			if( endCells>count ) result+=(endCells-std::max(beginningCells,count))*rightExtension;
			return result;
		}

		float OpenIntervalIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, float beginningCells, float endCells,
			float faceVelocity, const std::vector<float>& left,
			const std::vector<float>& right, const std::vector<float>& prefix )
		{
			return endCells>=beginningCells ? OpenForwardIntegral(request,component,line,
				beginningCells,endCells,faceVelocity,left,right,prefix) : -OpenForwardIntegral(
				request,component,line,endCells,beginningCells,faceVelocity,left,right,prefix);
		}

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;
			while( result<value ) result<<=1u;
			return result;
		}

		void BlellochPrefixLine( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::vector<float>& prefix )
		{
			const std::size_t width=NextPowerOfTwo(request.lineLength);
			std::vector<float> scratch(width,0.0f);
			for( std::size_t cell=0;cell<request.lineLength;++cell )
				scratch[cell]=request.values[ValueIndex(request,component,line,cell)];
			for( std::size_t offset=1u;offset<width;offset<<=1u )
				for( std::size_t index=offset*2u-1u;index<width;index+=offset*2u )
					scratch[index]+=scratch[index-offset];
			const float total=scratch[width-1u];
			scratch[width-1u]=0.0f;
			for( std::size_t offset=width>>1u;offset>0u;offset>>=1u )
				for( std::size_t index=offset*2u-1u;index<width;index+=offset*2u ) {
					const float below=scratch[index-offset];
					scratch[index-offset]=scratch[index];
					scratch[index]+=below;
				}
			const std::size_t base=(component*request.lineCount+line)*
				(request.lineLength+1u);
			for( std::size_t cell=0;cell<request.lineLength;++cell )
				prefix[base+cell]=scratch[cell];
			prefix[base+request.lineLength]=total;
		}
	}

	bool ValidateFireProductionRemapRequest( const FireProductionRemapRequest& request,
		std::string* error )
	{
		if( request.lineLength<4u||request.lineLength>1024u||request.lineCount==0u||
			request.componentCount==0u )
			return Fail(error,"production remap request shape or schedule is invalid");
		const std::size_t maximum=std::numeric_limits<std::size_t>::max();
		if( request.lineCount>maximum/request.lineLength||
			request.componentCount>maximum/(request.lineCount*request.lineLength)||
			request.lineCount>maximum/(request.lineLength+1u)||
			request.componentCount>maximum/(request.lineCount*(request.lineLength+1u)) )
			return Fail(error,"production remap request dimensions overflow");
		const std::size_t valueCount=request.componentCount*request.lineCount*request.lineLength;
		const std::size_t fluxCount=request.componentCount*request.lineCount*(request.lineLength+1u);
		const std::size_t velocityCount=request.lineCount*(request.lineLength+1u);
		if( valueCount>std::numeric_limits<std::uint32_t>::max()||
			fluxCount>std::numeric_limits<std::uint32_t>::max()||
			request.lineCount>std::numeric_limits<std::uint32_t>::max()||
			request.componentCount>std::numeric_limits<std::uint32_t>::max() )
			return Fail(error,"production remap request exceeds binary32 kernel indexing");
		const std::size_t maximumWorkingBytes=std::size_t(2u)<<30u;
		if( valueCount>maximumWorkingBytes/(4u*sizeof(float))||
			fluxCount>maximumWorkingBytes/(2u*sizeof(float))||
			4u*valueCount*sizeof(float)+2u*fluxCount*sizeof(float)>maximumWorkingBytes )
			return Fail(error,"production remap working set exceeds two GiB");
		if( !(request.cellWidthM>0.0f)||request.timeStepS<0.0f||
			!std::isfinite(request.cellWidthM)||!std::isfinite(request.timeStepS)||
			request.values.size()!=valueCount||request.faceVelocityMPerS.size()!=velocityCount||
			request.ambientValues.size()!=request.componentCount||
			request.boundary<FireProductionRemapPeriodic||request.boundary>FireProductionRemapWall )
			return Fail(error,"production remap request shape or schedule is invalid");
		for( const float value : request.values ) if( !std::isfinite(value) )
			return Fail(error,"production remap state is nonfinite");
		for( const float value : request.faceVelocityMPerS ) if( !std::isfinite(value) )
			return Fail(error,"production remap velocity is nonfinite");
		for( const float value : request.ambientValues ) if( !std::isfinite(value) )
			return Fail(error,"production remap ambient state is nonfinite");
		return true;
	}

	bool RemapFireProductionCPU( const FireProductionRemapRequest& request,
		FireProductionRemapResult& result, std::string* error )
	{
		result=FireProductionRemapResult();
		if( !ValidateFireProductionRemapRequest(request,error) ) return false;
		std::vector<float> left(request.values.size()),right(request.values.size());
		result.sharedLimiterAlpha.resize(request.lineCount*request.lineLength,1.0f);
		for( std::size_t line=0;line<request.lineCount;++line ) for( std::size_t cell=0;
			cell<request.lineLength;++cell ) {
			float alpha=1.0f;
			for( std::size_t component=0;component<request.componentCount;++component ) {
				const std::size_t index=ValueIndex(request,component,line,cell);
				UnlimitedEdges(request,component,line,cell,left[index],right[index]);
				const float center=request.values[index];
				float minimumDeviation=0.0f,maximumDeviation=0.0f;
				ParabolaDeviationRange(left[index]-center,right[index]-center,
					minimumDeviation,maximumDeviation);
				const float envelopeMinimum=std::min(center,std::min(
					Sample(request,component,line,static_cast<long>(cell)-1),
					Sample(request,component,line,static_cast<long>(cell)+1)));
				const float envelopeMaximum=std::max(center,std::max(
					Sample(request,component,line,static_cast<long>(cell)-1),
					Sample(request,component,line,static_cast<long>(cell)+1)));
				if( maximumDeviation>0.0f ) alpha=std::min(alpha,
					(envelopeMaximum-center)/maximumDeviation);
				if( minimumDeviation<0.0f ) alpha=std::min(alpha,
					(center-envelopeMinimum)/(-minimumDeviation));
			}
			alpha=std::max(0.0f,std::min(1.0f,alpha));
			result.sharedLimiterAlpha[line*request.lineLength+cell]=alpha;
			for( std::size_t component=0;component<request.componentCount;++component ) {
				const std::size_t index=ValueIndex(request,component,line,cell);
				const float center=request.values[index];
				left[index]=center+alpha*(left[index]-center);
				right[index]=center+alpha*(right[index]-center);
			}
		}

		std::vector<float> prefix(request.componentCount*request.lineCount*
			(request.lineLength+1u),0.0f);
		for( std::size_t component=0;component<request.componentCount;++component )
			for( std::size_t line=0;line<request.lineCount;++line )
				BlellochPrefixLine(request,component,line,prefix);

		result.faceFluxes.resize(request.componentCount*request.lineCount*
			(request.lineLength+1u),0.0f);
		for( std::size_t component=0;component<request.componentCount;++component )
			for( std::size_t line=0;line<request.lineCount;++line )
				for( std::size_t face=0;face<=request.lineLength;++face ) {
					const std::size_t flux=FluxIndex(request,component,line,face);
					if( request.boundary==FireProductionRemapWall&&
						(face==0u||face==request.lineLength) ) {
						result.faceFluxes[flux]=0.0f;
						continue;
					}
					const float velocity=request.faceVelocityMPerS[line*(request.lineLength+1u)+face];
					const float arrival=static_cast<float>(face);
					const float departure=arrival-request.timeStepS*velocity/request.cellWidthM;
					const float swept=request.boundary==FireProductionRemapPeriodic ?
						PeriodicIntervalIntegral(request,component,line,departure,arrival,left,right,prefix):
						OpenIntervalIntegral(request,component,line,departure,arrival,velocity,left,right,prefix);
					result.faceFluxes[flux]=request.cellWidthM*swept;
				}

		result.updatedValues.resize(request.values.size());
		for( std::size_t component=0;component<request.componentCount;++component )
			for( std::size_t line=0;line<request.lineCount;++line )
				for( std::size_t cell=0;cell<request.lineLength;++cell ) {
					const std::size_t value=ValueIndex(request,component,line,cell);
					result.updatedValues[value]=request.values[value]-
						(result.faceFluxes[FluxIndex(request,component,line,cell+1u)]-
						 result.faceFluxes[FluxIndex(request,component,line,cell)])/request.cellWidthM;
				}
		return true;
	}
}
