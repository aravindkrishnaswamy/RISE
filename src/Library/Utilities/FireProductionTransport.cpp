//////////////////////////////////////////////////////////////////////
//
//  FireProductionTransport.cpp - strict-binary32 production transport oracle
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionTransport.h"

#include "FireProductionAdvection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace RISE
{
	namespace
	{
		const std::uint64_t MetalAllocationQuantumBytes=UINT64_C(16384);

		bool Fail( std::string* error, const char* message ) noexcept
		{
			if( error ) try { *error=message; } catch( const std::bad_alloc& ) {}
			return false;
		}

		bool AddBytes( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( bytesPerValue&&count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue )
				return false;
			const std::uint64_t bytes=count*bytesPerValue;
			if( total>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
			total+=bytes;return true;
		}

		bool AddMetalBufferBytes( std::uint64_t requestedBytes, std::uint64_t& total )
		{
			if( requestedBytes==0u ) return false;
			const std::uint64_t remainder=requestedBytes%MetalAllocationQuantumBytes;
			if( remainder ) {
				const std::uint64_t increment=MetalAllocationQuantumBytes-remainder;
				if( requestedBytes>std::numeric_limits<std::uint64_t>::max()-increment )
					return false;
				requestedBytes+=increment;
			}
			if( total>std::numeric_limits<std::uint64_t>::max()-requestedBytes ) return false;
			total+=requestedBytes;return true;
		}

		bool AddMetalValueBuffer( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( bytesPerValue&&count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue )
				return false;
			return AddMetalBufferBytes(count*bytesPerValue,total);
		}

		void FailWithoutThrow( std::string* error, const char* message ) noexcept
		{
			if( !error ) return;
			try { *error=message; } catch( const std::bad_alloc& ) {}
		}

		std::size_t CellIndex( const FireProductionProjectionShape& shape,
			std::size_t x, std::size_t y, std::size_t z )
		{
			return (z*shape.ny+y)*shape.nx+x;
		}

		std::size_t FaceIndex( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t x, std::size_t y, std::size_t z )
		{
			if( axis==0u ) return (z*shape.ny+y)*(shape.nx+1u)+x;
			if( axis==1u ) return (z*(shape.ny+1u)+y)*shape.nx+x;
			return (z*shape.ny+y)*shape.nx+x;
		}

		FireProductionRemapBoundary RemapBoundary(
			FireProductionProjectionBoundary boundary )
		{
			if( boundary==FireProductionProjectionPeriodic )
				return FireProductionRemapPeriodic;
			if( boundary==FireProductionProjectionPressureOpen )
				return FireProductionRemapPressureOpen;
			return FireProductionRemapWall;
		}

		std::size_t AxisExtent( const FireProductionProjectionShape& shape,
			unsigned int axis )
		{
			return axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		}

		std::size_t AxisLineCount( const FireProductionProjectionShape& shape,
			unsigned int axis )
		{
			return axis==0u?shape.ny*shape.nz:
				(axis==1u?shape.nx*shape.nz:shape.nx*shape.ny);
		}

		void AxisCoordinates( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t line, std::size_t coordinate,
			std::size_t& x, std::size_t& y, std::size_t& z );

		bool ValidateAxisSchedule( const FireProductionCellPalindromeRequest& request,
			unsigned int axis, float timeStepS, std::string* error )
		{
			const std::size_t length=AxisExtent(request.shape,axis);
			const std::size_t lines=AxisLineCount(request.shape,axis);
			const std::size_t valueCount=request.componentCount*request.shape.CellCount();
			const std::size_t maximum=std::numeric_limits<std::size_t>::max();
			if( lines>maximum/(length+1u)||
				request.componentCount>maximum/(lines*(length+1u)) )
				return Fail(error,"production palindrome axis dimensions overflow");
			const std::size_t fluxCount=request.componentCount*lines*(length+1u);
			const std::size_t velocityCount=lines*(length+1u);
			if( valueCount>std::numeric_limits<std::uint32_t>::max()||
				fluxCount>std::numeric_limits<std::uint32_t>::max()||
				lines>std::numeric_limits<std::uint32_t>::max()||
				request.componentCount>std::numeric_limits<std::uint32_t>::max() )
				return Fail(error,"production palindrome axis exceeds kernel indexing");
			const std::uint64_t floatCount=4u*static_cast<std::uint64_t>(valueCount)+
				2u*static_cast<std::uint64_t>(fluxCount)+
				static_cast<std::uint64_t>(velocityCount)+
				static_cast<std::uint64_t>(lines)*length+request.componentCount;
			const std::uint64_t maximumBytes=std::uint64_t(2u)<<30u;
			const std::uint64_t parameterBytes=5u*sizeof(std::uint32_t)+2u*sizeof(float);
			if( floatCount>(maximumBytes-parameterBytes)/sizeof(float) )
				return Fail(error,"production palindrome axis exceeds two GiB");
			const bool periodic=request.boundary[2u*axis]==FireProductionProjectionPeriodic;
			for( std::size_t line=0;line<lines;++line ) {
				double previous=0.0;
				float seamVelocity=0.0f;
				for( std::size_t face=0;face<=length;++face ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,face,x,y,z);
					const float velocity=request.frozenVelocityMPerS[axis][
						FaceIndex(request.shape,axis,x,y,z)];
					if( face==0u ) seamVelocity=velocity;
					if( periodic&&face==length&&velocity!=seamVelocity )
						return Fail(error,"production palindrome periodic seam is not single-valued");
					const float courant=timeStepS*velocity/request.shape.cellWidthM;
					if( !std::isfinite(courant) )
						return Fail(error,"production palindrome Courant number is nonfinite");
					const double departure=static_cast<double>(face)-
						static_cast<double>(courant);
					if( !std::isfinite(departure)||(face>0u&&departure<previous) )
						return Fail(error,"production palindrome backtraced map is folded");
					previous=departure;
				}
			}
			return true;
		}

		void AxisCoordinates( const FireProductionProjectionShape& shape,
			unsigned int axis, std::size_t line, std::size_t coordinate,
			std::size_t& x, std::size_t& y, std::size_t& z )
		{
			if( axis==0u ) {x=coordinate;y=line%shape.ny;z=line/shape.ny;return;}
			if( axis==1u ) {x=line%shape.nx;y=coordinate;z=line/shape.nx;return;}
			x=line%shape.nx;y=line/shape.nx;z=coordinate;
		}

		bool ApplyAxis( const FireProductionCellPalindromeRequest& request,
			unsigned int axis, float timeStepS, std::vector<float>& values,
			std::string* error )
		{
			FireProductionRemapRequest lineRequest;
			lineRequest.lineLength=AxisExtent(request.shape,axis);
			lineRequest.lineCount=AxisLineCount(request.shape,axis);
			lineRequest.componentCount=request.componentCount;
			lineRequest.cellWidthM=request.shape.cellWidthM;
			lineRequest.timeStepS=timeStepS;
			lineRequest.asymmetricBoundaries=true;
			lineRequest.lowerBoundary=RemapBoundary(request.boundary[2u*axis]);
			lineRequest.upperBoundary=RemapBoundary(request.boundary[2u*axis+1u]);
			lineRequest.ambientValues=request.ambientValues;
			lineRequest.values.resize(values.size());
			lineRequest.faceVelocityMPerS.resize(lineRequest.lineCount*
				(lineRequest.lineLength+1u));
			for( std::size_t line=0;line<lineRequest.lineCount;++line ) {
				for( std::size_t face=0;face<=lineRequest.lineLength;++face ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,face,x,y,z);
					lineRequest.faceVelocityMPerS[line*(lineRequest.lineLength+1u)+face]=
						request.frozenVelocityMPerS[axis][FaceIndex(request.shape,axis,x,y,z)];
				}
				for( std::size_t coordinate=0;coordinate<lineRequest.lineLength;++coordinate ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,coordinate,x,y,z);
					const std::size_t cell=CellIndex(request.shape,x,y,z);
					for( std::size_t component=0;component<request.componentCount;++component )
						lineRequest.values[(component*lineRequest.lineCount+line)*
							lineRequest.lineLength+coordinate]=
							values[component*request.shape.CellCount()+cell];
				}
			}
			FireProductionRemapResult lineResult;
			if( !RemapFireProductionCPU(lineRequest,lineResult,error) ) return false;
			for( std::size_t line=0;line<lineRequest.lineCount;++line )
				for( std::size_t coordinate=0;coordinate<lineRequest.lineLength;++coordinate ) {
					std::size_t x=0u,y=0u,z=0u;
					AxisCoordinates(request.shape,axis,line,coordinate,x,y,z);
					const std::size_t cell=CellIndex(request.shape,x,y,z);
					for( std::size_t component=0;component<request.componentCount;++component )
						values[component*request.shape.CellCount()+cell]=
							lineResult.updatedValues[(component*lineRequest.lineCount+line)*
								lineRequest.lineLength+coordinate];
				}
			return true;
		}
	}

	bool FireProductionCellPalindromeWorkingSetBytes(
		const FireProductionProjectionShape& shape, std::size_t componentCount,
		std::uint64_t& bytes )
	{
		bytes=0u;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||componentCount==0u ) return false;
		const std::uint64_t cells=static_cast<std::uint64_t>(shape.nx)*shape.ny*shape.nz;
		if( componentCount>std::numeric_limits<std::uint64_t>::max()/cells ) return false;
		const std::uint64_t values=cells*componentCount;
		const std::uint64_t xFaces=static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz;
		const std::uint64_t yFaces=static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz;
		const std::uint64_t zFaces=static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u);
		const std::uint64_t allFaces=xFaces+yFaces+zFaces;
		const std::uint64_t maximumLineFaces=std::max(xFaces,std::max(yFaces,zFaces));
		if( componentCount>std::numeric_limits<std::uint64_t>::max()/maximumLineFaces )
			return false;
		const std::uint64_t componentLineFaces=componentCount*maximumLineFaces;
		std::uint64_t total=0u;
		// Caller-owned request/result payloads remain live through publication.
		if( !AddBytes(values,2u*sizeof(float),total)||
			!AddBytes(allFaces,sizeof(float),total)||
			!AddBytes(componentCount,sizeof(float),total) ) return false;
		// Eight value-sized Metal buffers: input/output staging, two resident grids,
		// and four resident line/reconstruction fields.
		for( unsigned int buffer=0u;buffer<8u;++buffer )
			if( !AddMetalValueBuffer(values,sizeof(float),total) ) return false;
		// Each carrier is present once in Shared staging and once in Private storage.
		const std::uint64_t faceCounts[]={xFaces,yFaces,zFaces};
		for( const std::uint64_t faceCount : faceCounts )
			for( unsigned int buffer=0u;buffer<2u;++buffer )
				if( !AddMetalValueBuffer(faceCount,sizeof(float),total) ) return false;
		if( !AddMetalValueBuffer(maximumLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(cells,sizeof(float),total)||
			!AddMetalValueBuffer(componentLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(componentLineFaces,sizeof(float),total)||
			!AddMetalValueBuffer(componentCount,sizeof(float),total) ) return false;
		// Five grid-parameter and five line-parameter resources remain retained by
		// the one command until its terminal publication completes.
		for( unsigned int pass=0u;pass<5u;++pass )
			if( !AddMetalBufferBytes(sizeof(std::uint32_t)*5u,total)||
				!AddMetalBufferBytes(sizeof(std::uint32_t)*5u+sizeof(float)*2u,total) )
				return false;
		bytes=total;return true;
	}

	bool ValidateFireProductionCellPalindromeRequest(
		const FireProductionCellPalindromeRequest& request, std::string* error )
	{
		const FireProductionProjectionShape& shape=request.shape;
		if( shape.nx<4u||shape.nx>1024u||shape.ny<4u||shape.ny>1024u||
			shape.nz<4u||shape.nz>1024u||request.componentCount==0u||
			!(shape.cellWidthM>0.0f)||request.timeStepS<0.0f||
			!std::isfinite(shape.cellWidthM)||!std::isfinite(request.timeStepS) )
			return Fail(error,"production palindrome shape or schedule is invalid");
		const std::size_t maximum=std::numeric_limits<std::size_t>::max();
		if( shape.nx>maximum/shape.ny||shape.nx*shape.ny>maximum/shape.nz )
			return Fail(error,"production palindrome shape overflows");
		const std::size_t cells=shape.CellCount();
		if( request.componentCount>maximum/cells )
			return Fail(error,"production palindrome tuple shape is invalid");
		std::uint64_t workingBytes=0u;
		if( !FireProductionCellPalindromeWorkingSetBytes(shape,request.componentCount,
			workingBytes)||workingBytes>(std::uint64_t(2u)<<30u) )
			return Fail(error,"production palindrome working set exceeds two GiB");
		if( request.conservativeValues.size()!=request.componentCount*cells||
			request.ambientValues.size()!=request.componentCount )
			return Fail(error,"production palindrome tuple shape is invalid");
		for( unsigned int axis=0u;axis<3u;++axis ) {
			if( request.frozenVelocityMPerS[axis].size()!=
				FireProductionProjectionFaceCount(shape,axis) )
				return Fail(error,"production palindrome velocity shape is invalid");
			const FireProductionProjectionBoundary lower=request.boundary[2u*axis];
			const FireProductionProjectionBoundary upper=request.boundary[2u*axis+1u];
			if( lower<FireProductionProjectionPeriodic||lower>FireProductionProjectionWall||
				upper<FireProductionProjectionPeriodic||upper>FireProductionProjectionWall||
				((lower==FireProductionProjectionPeriodic)!=(upper==FireProductionProjectionPeriodic)) )
				return Fail(error,"production palindrome boundary pairing is invalid");
		}
		for( const float value : request.conservativeValues ) if( !std::isfinite(value) )
			return Fail(error,"production palindrome state is nonfinite");
		for( const std::vector<float>& velocity : request.frozenVelocityMPerS )
			for( const float value : velocity ) if( !std::isfinite(value) )
				return Fail(error,"production palindrome velocity is nonfinite");
		for( const float value : request.ambientValues ) if( !std::isfinite(value) )
			return Fail(error,"production palindrome ambient tuple is nonfinite");
		const float halfStep=0.5f*request.timeStepS;
		if( !ValidateAxisSchedule(request,0u,halfStep,error)||
			!ValidateAxisSchedule(request,1u,halfStep,error)||
			!ValidateAxisSchedule(request,2u,request.timeStepS,error) ) return false;
		return true;
	}

	bool RemapFireProductionCellPalindromeCPU(
		const FireProductionCellPalindromeRequest& request,
		FireProductionCellPalindromeResult& result, std::string* error )
	{
		result=FireProductionCellPalindromeResult();
		try {
			if( !ValidateFireProductionCellPalindromeRequest(request,error) ) return false;
			std::vector<float> values=request.conservativeValues;
			const float halfStep=0.5f*request.timeStepS;
			const unsigned int axes[]={0u,1u,2u,1u,0u};
			const float steps[]={halfStep,halfStep,request.timeStepS,halfStep,halfStep};
			for( unsigned int pass=0u;pass<5u;++pass )
				if( !ApplyAxis(request,axes[pass],steps[pass],values,error) ) return false;
			result.conservativeValues=std::move(values);
			result.executedSubmapCount=5u;
			if( error ) error->clear();
			return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionCellPalindromeResult();
			FailWithoutThrow(error,"production palindrome allocation failed");
			return false;
		}
	}
}
