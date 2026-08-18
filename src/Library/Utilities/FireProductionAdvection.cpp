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
			if( im2==center&&im1==center&&ip1==center&&ip2==center ) {
				left=center;right=center;return;
			}
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

		float CellIntervalIntegral( float center, float left, float right,
			float beginning, float end )
		{
			if( left==center&&right==center ) return (end-beginning)*center;
			const float q6=6.0f*center-3.0f*(left+right);
			const float delta=end-beginning;
			return delta*(left+0.5f*(right-left+q6)*(beginning+end)-
				(q6/3.0f)*(beginning*beginning+beginning*end+end*end));
		}

		float CellTrailingIntegral( float center, float left, float right, float length )
		{
			if( left==center&&right==center ) return length*center;
			const float q6=6.0f*center-3.0f*(left+right);
			return length*(right-0.5f*(right-left-q6)*length-
				(q6/3.0f)*length*length);
		}

		std::size_t WrappedCell( long cell, std::size_t count )
		{
			const long signedCount=static_cast<long>(count);
			long wrapped=cell%signedCount;
			if( wrapped<0 ) wrapped+=signedCount;
			return static_cast<std::size_t>(wrapped);
		}

		float PeriodicLocalForwardIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, long beginningCell,
			float beginningFraction, float length, const std::vector<float>& left,
			const std::vector<float>& right )
		{
			float remaining=length,result=0.0f;
			long cell=beginningCell;
			float fraction=beginningFraction;
			while( remaining>0.0f ) {
				const float span=std::min(remaining,1.0f-fraction);
				const std::size_t wrapped=WrappedCell(cell,request.lineLength);
				const std::size_t value=ValueIndex(request,component,line,wrapped);
				result+=CellIntervalIntegral(request.values[value],left[value],right[value],
					fraction,fraction+span);
				remaining-=span;
				fraction=0.0f;
				++cell;
			}
			return result;
		}

		float PeriodicSweptIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t face, float courant,
			const std::vector<float>& left, const std::vector<float>& right,
			const std::vector<float>& prefix )
		{
			const float count=static_cast<float>(request.lineLength);
			const float magnitude=std::fabs(courant);
			const float localLength=std::fmod(magnitude,count);
			const float cycles=std::floor((magnitude-localLength)/count);
			const std::size_t base=(component*request.lineCount+line)*(request.lineLength+1u);
			float result=cycles*prefix[base+request.lineLength];
			if( courant>=0.0f ) {
				const float whole=std::floor(localLength);
				const float fractional=localLength-whole;
				const long wholeBeginning=static_cast<long>(face)-static_cast<long>(whole);
				if( fractional>0.0f ) {
					const std::size_t wrapped=WrappedCell(wholeBeginning-1l,request.lineLength);
					const std::size_t value=ValueIndex(request,component,line,wrapped);
					result+=CellTrailingIntegral(request.values[value],left[value],right[value],
						fractional);
				}
				result+=PeriodicLocalForwardIntegral(request,component,line,wholeBeginning,
					0.0f,whole,left,right);
				return result;
			}
			result+=PeriodicLocalForwardIntegral(request,component,line,
				static_cast<long>(face),0.0f,localLength,left,right);
			return -result;
		}

		float OpenLocalForwardIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t beginningCell,
			float beginningFraction, float length, const std::vector<float>& left,
			const std::vector<float>& right )
		{
			float remaining=length,result=0.0f;
			std::size_t cell=beginningCell;
			float fraction=beginningFraction;
			while( remaining>0.0f&&cell<request.lineLength ) {
				const float span=std::min(remaining,1.0f-fraction);
				const std::size_t value=ValueIndex(request,component,line,cell);
				result+=CellIntervalIntegral(request.values[value],left[value],right[value],
					fraction,fraction+span);
				remaining-=span;
				fraction=0.0f;
				++cell;
			}
			return result;
		}

		float OpenSweptIntegral( const FireProductionRemapRequest& request,
			std::size_t component, std::size_t line, std::size_t face, float courant,
			float faceVelocity, const std::vector<float>& left,
			const std::vector<float>& right )
		{
			const float magnitude=std::fabs(courant);
			const float leftExtension=request.boundary==FireProductionRemapPressureOpen&&
				faceVelocity>0.0f ? request.ambientValues[component] :
				request.values[ValueIndex(request,component,line,0u)];
			const float rightExtension=request.boundary==FireProductionRemapPressureOpen&&
				faceVelocity<0.0f ? request.ambientValues[component] :
				request.values[ValueIndex(request,component,line,request.lineLength-1u)];
			if( courant>=0.0f ) {
				const float interiorLength=std::min(magnitude,static_cast<float>(face));
				const float whole=std::floor(interiorLength);
				const float fractional=interiorLength-whole;
				const std::size_t wholeBeginning=face-static_cast<std::size_t>(whole);
				float result=(magnitude-interiorLength)*leftExtension;
				if( fractional>0.0f ) {
					const std::size_t value=ValueIndex(request,component,line,wholeBeginning-1u);
					result+=CellTrailingIntegral(request.values[value],left[value],right[value],
						fractional);
				}
				return result+OpenLocalForwardIntegral(request,component,line,wholeBeginning,
					0.0f,whole,left,right);
			}
			const float interiorLength=std::min(magnitude,
				static_cast<float>(request.lineLength-face));
			return -(OpenLocalForwardIntegral(request,component,line,face,0.0f,
				interiorLength,left,right)+(magnitude-interiorLength)*rightExtension);
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
		const std::uint64_t maximumWorkingBytes=std::uint64_t(2u)<<30u;
		const std::uint64_t alphaCount=request.lineCount*request.lineLength;
		const std::uint64_t workingFloatCount=4u*static_cast<std::uint64_t>(valueCount)+
			2u*static_cast<std::uint64_t>(fluxCount)+
			static_cast<std::uint64_t>(velocityCount)+alphaCount+
			static_cast<std::uint64_t>(request.componentCount);
		const std::uint64_t parameterBytes=4u*sizeof(std::uint32_t)+2u*sizeof(float);
		if( workingFloatCount>(maximumWorkingBytes-parameterBytes)/sizeof(float) )
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
		for( std::size_t line=0;line<request.lineCount;++line ) {
			const std::size_t base=line*(request.lineLength+1u);
			if( request.boundary==FireProductionRemapPeriodic&&
				request.faceVelocityMPerS[base]!=request.faceVelocityMPerS[base+request.lineLength] )
				return Fail(error,"production periodic remap seam velocity is not single-valued");
			double previous=0.0;
			for( std::size_t face=0;face<=request.lineLength;++face ) {
				const float courant=request.timeStepS*request.faceVelocityMPerS[base+face]/
					request.cellWidthM;
				if( !std::isfinite(courant) )
					return Fail(error,"production remap binary32 Courant number is nonfinite");
				const double departure=static_cast<double>(face)-
					static_cast<double>(courant);
				if( !std::isfinite(departure)||(face>0u&&departure<previous) )
					return Fail(error,"production remap backtraced face map is folded");
				previous=departure;
			}
		}
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
				for( std::size_t face=0;face<request.lineLength;++face ) {
					const std::size_t flux=FluxIndex(request,component,line,face);
					if( request.boundary==FireProductionRemapWall&&
						(face==0u||face==request.lineLength) ) {
						result.faceFluxes[flux]=0.0f;
						continue;
					}
					const float velocity=request.faceVelocityMPerS[line*(request.lineLength+1u)+face];
					const float courant=request.timeStepS*velocity/request.cellWidthM;
					const float swept=request.boundary==FireProductionRemapPeriodic ?
						PeriodicSweptIntegral(request,component,line,face,courant,left,right,prefix):
						OpenSweptIntegral(request,component,line,face,courant,velocity,left,right);
					result.faceFluxes[flux]=request.cellWidthM*swept;
				}
		for( std::size_t component=0;component<request.componentCount;++component )
			for( std::size_t line=0;line<request.lineCount;++line ) {
				const std::size_t end=FluxIndex(request,component,line,request.lineLength);
				if( request.boundary==FireProductionRemapPeriodic )
					result.faceFluxes[end]=result.faceFluxes[
						FluxIndex(request,component,line,0u)];
				else if( request.boundary==FireProductionRemapWall )
					result.faceFluxes[end]=0.0f;
				else {
					const float velocity=request.faceVelocityMPerS[
						line*(request.lineLength+1u)+request.lineLength];
					const float courant=request.timeStepS*velocity/request.cellWidthM;
					result.faceFluxes[end]=request.cellWidthM*OpenSweptIntegral(request,
						component,line,request.lineLength,courant,velocity,left,right);
				}
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
		for( const float value : result.updatedValues ) if( !std::isfinite(value) ) {
			result=FireProductionRemapResult();
			return Fail(error,"production remap produced nonfinite updated state");
		}
		for( const float value : result.faceFluxes ) if( !std::isfinite(value) ) {
			result=FireProductionRemapResult();
			return Fail(error,"production remap produced nonfinite face flux");
		}
		for( const float value : result.sharedLimiterAlpha ) if( !std::isfinite(value) ) {
			result=FireProductionRemapResult();
			return Fail(error,"production remap produced nonfinite limiter state");
		}
		if( error ) error->clear();
		return true;
	}
}
