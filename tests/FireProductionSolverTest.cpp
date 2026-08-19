//////////////////////////////////////////////////////////////////////
//
//  FireProductionSolverTest.cpp - production-fire P0 capability/tables
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Utilities/FireProductionAdvection.h"
#include "../src/Library/Utilities/FireProductionTables.h"
#include "../src/Library/Utilities/FireProductionTransport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	bool denyTestAllocations=false;

	int failures=0;
}

void* operator new( std::size_t bytes )
{
	if( denyTestAllocations ) throw std::bad_alloc();
	if( void* memory=std::malloc(bytes) ) return memory;
	throw std::bad_alloc();
}

void operator delete( void* memory ) noexcept {std::free(memory);}
void operator delete( void* memory, std::size_t ) noexcept {std::free(memory);}

namespace
{
	void Check( bool condition, const char* message )
	{
		if( !condition ) {
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	const RISE::FireThermochemistrySegment* SegmentAt(
		const RISE::FireThermochemistrySpecies& species, double temperatureK )
	{
		for( const RISE::FireThermochemistrySegment& segment : species.segments )
			if( temperatureK>=segment.temperatureMinK&&temperatureK<=segment.temperatureMaxK )
				return &segment;
		return 0;
	}

	std::string ReadText( const char* path )
	{
		std::ifstream input(path);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
	}

	std::size_t CountSubstring( const std::string& text, const std::string& needle )
	{
		std::size_t count=0u,position=0u;
		while( (position=text.find(needle,position))!=std::string::npos ) {
			++count;position+=needle.size();
		}
		return count;
	}

	std::size_t RemapValueIndex( const RISE::FireProductionRemapRequest& request,
		std::size_t component, std::size_t line, std::size_t cell )
	{
		return (component*request.lineCount+line)*request.lineLength+cell;
	}

	std::size_t RemapFluxIndex( const RISE::FireProductionRemapRequest& request,
		std::size_t component, std::size_t line, std::size_t face )
	{
		return (component*request.lineCount+line)*(request.lineLength+1u)+face;
	}

	std::size_t TransportCellIndex( const RISE::FireProductionProjectionShape& shape,
		std::size_t x, std::size_t y, std::size_t z )
	{
		return (z*shape.ny+y)*shape.nx+x;
	}

	std::size_t TransportFaceIndex( const RISE::FireProductionProjectionShape& shape,
		unsigned int axis, std::size_t x, std::size_t y, std::size_t z )
	{
		if( axis==0u ) return (z*shape.ny+y)*(shape.nx+1u)+x;
		if( axis==1u ) return (z*(shape.ny+1u)+y)*shape.nx+x;
		return (z*shape.ny+y)*shape.nx+x;
	}

	std::string FloatBytesSHA256( const std::vector<float>& values )
	{
		RISE::RISECBOR64::Bytes bytes(values.size()*sizeof(float));
		if( !bytes.empty() ) std::memcpy(bytes.data(),values.data(),bytes.size());
		return RISE::RISECBOR64::SHA256Hex(bytes);
	}

	RISE::FireProductionRemapRequest PeriodicRequest( std::size_t cells,
		std::size_t components, float courant )
	{
		RISE::FireProductionRemapRequest request;
		request.lineLength=cells;
		request.lineCount=1u;
		request.componentCount=components;
		request.cellWidthM=1.0f/static_cast<float>(cells);
		request.timeStepS=courant*request.cellWidthM;
		request.boundary=RISE::FireProductionRemapPeriodic;
		request.values.resize(cells*components);
		request.faceVelocityMPerS.assign(cells+1u,1.0f);
		request.ambientValues.assign(components,0.0f);
		return request;
	}

	RISE::FireProductionRemapBoundary TestRemapBoundary(
		RISE::FireProductionProjectionBoundary boundary )
	{
		if( boundary==RISE::FireProductionProjectionPeriodic )
			return RISE::FireProductionRemapPeriodic;
		if( boundary==RISE::FireProductionProjectionPressureOpen )
			return RISE::FireProductionRemapPressureOpen;
		return RISE::FireProductionRemapWall;
	}

	bool IndependentCellAxisPass(
		const RISE::FireProductionCellPalindromeRequest& request,
		unsigned int axis, float timeStepS, std::vector<float>& values,
		std::string& error )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t length=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		const std::size_t lines=axis==0u?shape.ny*shape.nz:
			(axis==1u?shape.nx*shape.nz:shape.nx*shape.ny);
		RISE::FireProductionRemapRequest oneDimensional;
		oneDimensional.lineLength=length;oneDimensional.lineCount=lines;
		oneDimensional.componentCount=request.componentCount;
		oneDimensional.cellWidthM=shape.cellWidthM;oneDimensional.timeStepS=timeStepS;
		oneDimensional.asymmetricBoundaries=true;
		oneDimensional.lowerBoundary=TestRemapBoundary(request.boundary[2u*axis]);
		oneDimensional.upperBoundary=TestRemapBoundary(request.boundary[2u*axis+1u]);
		oneDimensional.ambientValues=request.ambientValues;
		oneDimensional.values.resize(values.size());
		oneDimensional.faceVelocityMPerS.resize(lines*(length+1u));
		for( std::size_t line=0;line<lines;++line ) {
			const std::size_t transverse0=axis==0u?line%shape.ny:line%shape.nx;
			const std::size_t transverse1=axis==0u?line/shape.ny:
				(axis==1u?line/shape.nx:line/shape.nx);
			for( std::size_t coordinate=0;coordinate<=length;++coordinate ) {
				const std::size_t x=axis==0u?coordinate:transverse0;
				const std::size_t y=axis==1u?coordinate:(axis==0u?transverse0:transverse1);
				const std::size_t z=axis==2u?coordinate:transverse1;
				std::size_t face=0u;
				if( axis==0u ) face=(z*shape.ny+y)*(shape.nx+1u)+x;
				else if( axis==1u ) face=(z*(shape.ny+1u)+y)*shape.nx+x;
				else face=(z*shape.ny+y)*shape.nx+x;
				oneDimensional.faceVelocityMPerS[line*(length+1u)+coordinate]=
					request.frozenVelocityMPerS[axis][face];
				if( coordinate==length ) continue;
				const std::size_t cell=TransportCellIndex(shape,x,y,z);
				for( std::size_t component=0;component<request.componentCount;++component )
					oneDimensional.values[(component*lines+line)*length+coordinate]=
						values[component*shape.CellCount()+cell];
			}
		}
		RISE::FireProductionRemapResult remapped;
		if( !RISE::RemapFireProductionCPU(oneDimensional,remapped,&error) ) return false;
		for( std::size_t line=0;line<lines;++line ) {
			const std::size_t transverse0=axis==0u?line%shape.ny:line%shape.nx;
			const std::size_t transverse1=axis==0u?line/shape.ny:
				(axis==1u?line/shape.nx:line/shape.nx);
			for( std::size_t coordinate=0;coordinate<length;++coordinate ) {
				const std::size_t x=axis==0u?coordinate:transverse0;
				const std::size_t y=axis==1u?coordinate:(axis==0u?transverse0:transverse1);
				const std::size_t z=axis==2u?coordinate:transverse1;
				const std::size_t cell=TransportCellIndex(shape,x,y,z);
				for( std::size_t component=0;component<request.componentCount;++component )
					values[component*shape.CellCount()+cell]=
						remapped.updatedValues[(component*lines+line)*length+coordinate];
			}
		}
		return true;
	}

	bool IndependentCellComposition(
		const RISE::FireProductionCellPalindromeRequest& request,
		const unsigned int* axes, const float* stepFactors, std::size_t passCount,
		std::vector<float>& values, std::string& error )
	{
		values=request.conservativeValues;
		for( std::size_t pass=0;pass<passCount;++pass )
			if( !IndependentCellAxisPass(request,axes[pass],
				stepFactors[pass]*request.timeStepS,values,error) ) return false;
		return true;
	}

	std::size_t TestAxisExtent( const RISE::FireProductionProjectionShape& shape,
		unsigned int axis )
	{
		return axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
	}

	std::size_t TestAxisCoordinate( unsigned int axis, std::size_t x,
		std::size_t y, std::size_t z )
	{
		return axis==0u?x:(axis==1u?y:z);
	}

	void SetTestAxisCoordinate( unsigned int axis, std::size_t coordinate,
		std::size_t& x, std::size_t& y, std::size_t& z )
	{
		if( axis==0u ) x=coordinate;
		else if( axis==1u ) y=coordinate;
		else z=coordinate;
	}

	float TestCanonicalFaceValue( const RISE::FireProductionProjectionShape& shape,
		const std::vector<float>& values, unsigned int axis,
		std::size_t x, std::size_t y, std::size_t z )
	{
		if( axis==0u&&x==shape.nx ) x=0u;
		if( axis==1u&&y==shape.ny ) y=0u;
		if( axis==2u&&z==shape.nz ) z=0u;
		return values[TransportFaceIndex(shape,axis,x,y,z)];
	}

	bool IndependentPeriodicDualComponent(
		const RISE::FireProductionPeriodicDualMomentumRequest& request,
		unsigned int transportedComponent, int oneSidedSweepAxis,
		bool useLowerCarrier, bool splitTupleLimiter,
		std::vector<float>& density, std::vector<float>& momentum,
		std::string& error )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t cells=shape.CellCount();
		RISE::FireProductionCellPalindromeRequest dual;
		dual.shape=shape;dual.componentCount=2u;dual.timeStepS=request.timeStepS;
		dual.boundary.fill(RISE::FireProductionProjectionPeriodic);
		dual.conservativeValues.resize(2u*cells);dual.ambientValues.assign(2u,0.0f);
		for( std::size_t z=0u;z<shape.nz;++z ) for( std::size_t y=0u;y<shape.ny;++y )
			for( std::size_t x=0u;x<shape.nx;++x ) {
				const std::size_t cell=TransportCellIndex(shape,x,y,z);
				const std::size_t face=TransportFaceIndex(shape,transportedComponent,x,y,z);
				dual.conservativeValues[cell]=request.beginningFaceDensity[transportedComponent][face];
				dual.conservativeValues[cells+cell]=request.beginningMomentum[transportedComponent][face];
			}
		for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis ) {
			std::vector<float>& carrier=dual.frozenVelocityMPerS[sweepAxis];
			carrier.resize(RISE::FireProductionProjectionFaceCount(shape,sweepAxis));
			const std::size_t xEnd=shape.nx+(sweepAxis==0u?1u:0u);
			const std::size_t yEnd=shape.ny+(sweepAxis==1u?1u:0u);
			const std::size_t zEnd=shape.nz+(sweepAxis==2u?1u:0u);
			for( std::size_t z=0u;z<zEnd;++z ) for( std::size_t y=0u;y<yEnd;++y )
				for( std::size_t x=0u;x<xEnd;++x ) {
					const unsigned int averageAxis=transportedComponent==sweepAxis?
						sweepAxis:transportedComponent;
					const std::size_t extent=TestAxisExtent(shape,averageAxis);
					const std::size_t raw=TestAxisCoordinate(averageAxis,x,y,z);
					const std::size_t current=raw==extent?0u:raw;
					const std::size_t previous=current?current-1u:extent-1u;
					std::size_t lx=x,ly=y,lz=z,ux=x,uy=y,uz=z;
					SetTestAxisCoordinate(averageAxis,previous,lx,ly,lz);
					SetTestAxisCoordinate(averageAxis,current,ux,uy,uz);
					if( averageAxis!=sweepAxis ) {
						const std::size_t sweepExtent=TestAxisExtent(shape,sweepAxis);
						const std::size_t sweepRaw=TestAxisCoordinate(sweepAxis,x,y,z);
						const std::size_t sweepCurrent=sweepRaw==sweepExtent?0u:sweepRaw;
						SetTestAxisCoordinate(sweepAxis,sweepCurrent,lx,ly,lz);
						SetTestAxisCoordinate(sweepAxis,sweepCurrent,ux,uy,uz);
					}
					const float lower=TestCanonicalFaceValue(shape,
						request.frozenVelocityMPerS[sweepAxis],sweepAxis,lx,ly,lz);
					const float upper=TestCanonicalFaceValue(shape,
						request.frozenVelocityMPerS[sweepAxis],sweepAxis,ux,uy,uz);
					carrier[TransportFaceIndex(shape,sweepAxis,x,y,z)]=
						static_cast<int>(sweepAxis)==oneSidedSweepAxis?
							(useLowerCarrier?lower:upper):0.5f*(lower+upper);
				}
		}
		const unsigned int axes[]={0u,1u,2u,1u,0u};
		const float factors[]={0.5f,0.5f,1.0f,0.5f,0.5f};
		std::vector<float> values;
		if( !splitTupleLimiter ) {
			if( !IndependentCellComposition(dual,axes,factors,5u,values,error) ) return false;
		} else {
			values.resize(2u*cells);
			for( std::size_t channel=0u;channel<2u;++channel ) {
				RISE::FireProductionCellPalindromeRequest single=dual;
				single.componentCount=1u;single.ambientValues.assign(1u,0.0f);
				single.conservativeValues.assign(dual.conservativeValues.begin()+channel*cells,
					dual.conservativeValues.begin()+(channel+1u)*cells);
				std::vector<float> channelValues;
				if( !IndependentCellComposition(single,axes,factors,5u,channelValues,error) )
					return false;
				std::copy(channelValues.begin(),channelValues.end(),values.begin()+channel*cells);
			}
		}
		density.assign(RISE::FireProductionProjectionFaceCount(shape,transportedComponent),0.0f);
		momentum.assign(density.size(),0.0f);
		for( std::size_t z=0u;z<shape.nz;++z ) for( std::size_t y=0u;y<shape.ny;++y )
			for( std::size_t x=0u;x<shape.nx;++x ) {
				const std::size_t cell=TransportCellIndex(shape,x,y,z);
				const std::size_t face=TransportFaceIndex(shape,transportedComponent,x,y,z);
				density[face]=values[cell];momentum[face]=values[cells+cell];
			}
		if( transportedComponent==0u ) for( std::size_t z=0u;z<shape.nz;++z )
			for( std::size_t y=0u;y<shape.ny;++y ) {
				density[TransportFaceIndex(shape,0u,shape.nx,y,z)]=
					density[TransportFaceIndex(shape,0u,0u,y,z)];
				momentum[TransportFaceIndex(shape,0u,shape.nx,y,z)]=
					momentum[TransportFaceIndex(shape,0u,0u,y,z)];
			}
		if( transportedComponent==1u ) for( std::size_t z=0u;z<shape.nz;++z )
			for( std::size_t x=0u;x<shape.nx;++x ) {
				density[TransportFaceIndex(shape,1u,x,shape.ny,z)]=
					density[TransportFaceIndex(shape,1u,x,0u,z)];
				momentum[TransportFaceIndex(shape,1u,x,shape.ny,z)]=
					momentum[TransportFaceIndex(shape,1u,x,0u,z)];
			}
		if( transportedComponent==2u ) for( std::size_t y=0u;y<shape.ny;++y )
			for( std::size_t x=0u;x<shape.nx;++x ) {
				density[TransportFaceIndex(shape,2u,x,y,shape.nz)]=
					density[TransportFaceIndex(shape,2u,x,y,0u)];
				momentum[TransportFaceIndex(shape,2u,x,y,shape.nz)]=
					momentum[TransportFaceIndex(shape,2u,x,y,0u)];
			}
		return true;
	}

	bool TestDualAxisPeriodic( const RISE::FireProductionDualMomentumRequest& request,
		unsigned int axis )
	{
		return request.boundary[2u*axis]==RISE::FireProductionProjectionPeriodic&&
			request.boundary[2u*axis+1u]==RISE::FireProductionProjectionPeriodic;
	}

	float IndependentDualCrossCarrier(
		const RISE::FireProductionDualMomentumRequest& request,
		unsigned int component, unsigned int sweepAxis, std::size_t sweepFace,
		std::size_t componentFace, std::size_t remainingCoordinate )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		if( (sweepFace==0u&&request.boundary[2u*sweepAxis]==
			RISE::FireProductionProjectionWall)||
			(sweepFace==TestAxisExtent(shape,sweepAxis)&&
			 request.boundary[2u*sweepAxis+1u]==RISE::FireProductionProjectionWall) )
			return 0.0f;
		const unsigned int remaining=3u-component-sweepAxis;
		const std::size_t componentExtent=TestAxisExtent(shape,component);
		auto sample=[&](std::size_t componentCell) {
			std::size_t coordinates[]={0u,0u,0u};
			coordinates[sweepAxis]=sweepFace;
			coordinates[component]=componentCell;
			coordinates[remaining]=remainingCoordinate;
			return request.frozenVelocityMPerS[sweepAxis][TransportFaceIndex(shape,
				sweepAxis,coordinates[0],coordinates[1],coordinates[2])];
		};
		float lower=0.0f,upper=0.0f;
		if( componentFace>0u&&componentFace<componentExtent ) {
			lower=sample(componentFace-1u);upper=sample(componentFace);
		} else if( componentFace==0u ) {
			upper=sample(0u);
			const RISE::FireProductionProjectionBoundary boundary=request.boundary[2u*component];
			lower=boundary==RISE::FireProductionProjectionPeriodic?
				sample(componentExtent-1u):
				(boundary==RISE::FireProductionProjectionWall?-upper:upper);
		} else {
			lower=sample(componentExtent-1u);
			const RISE::FireProductionProjectionBoundary boundary=
				request.boundary[2u*component+1u];
			upper=boundary==RISE::FireProductionProjectionPeriodic?sample(0u):
				(boundary==RISE::FireProductionProjectionWall?-lower:lower);
		}
		return 0.5f*(lower+upper);
	}

	bool IndependentDualAxisPass(
		const RISE::FireProductionDualMomentumRequest& request,
		unsigned int component, unsigned int sweepAxis, float timeStepS,
		std::vector<float>& density, std::vector<float>& momentum,
		std::string& error )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t componentExtent=TestAxisExtent(shape,component);
		const bool componentPeriodic=TestDualAxisPeriodic(request,component);
		const std::size_t componentBeginning=componentPeriodic?0u:
			(request.boundary[2u*component]==RISE::FireProductionProjectionWall?1u:0u);
		const std::size_t componentEnd=componentPeriodic?componentExtent:
			componentExtent+1u-
			(request.boundary[2u*component+1u]==RISE::FireProductionProjectionWall?1u:0u);
		const unsigned int remaining=component==sweepAxis?(component+2u)%3u:
			3u-component-sweepAxis;
		const unsigned int first=(component+1u)%3u;
		const unsigned int second=(component+2u)%3u;
		const std::size_t length=component==sweepAxis?
			(componentPeriodic?componentExtent:componentExtent-1u):
			TestAxisExtent(shape,sweepAxis);
		const std::size_t lines=component==sweepAxis?
			TestAxisExtent(shape,first)*TestAxisExtent(shape,second):
			(componentEnd-componentBeginning)*TestAxisExtent(shape,remaining);
		RISE::FireProductionRemapRequest lineRequest;
		lineRequest.lineLength=length;lineRequest.lineCount=lines;
		lineRequest.componentCount=2u;lineRequest.cellWidthM=shape.cellWidthM;
		lineRequest.timeStepS=timeStepS;lineRequest.asymmetricBoundaries=true;
		lineRequest.lowerBoundary=TestRemapBoundary(request.boundary[2u*sweepAxis]);
		lineRequest.upperBoundary=TestRemapBoundary(request.boundary[2u*sweepAxis+1u]);
		lineRequest.lineSpecificAmbientValues=!TestDualAxisPeriodic(request,sweepAxis);
		lineRequest.ambientValues.assign(2u,0.0f);
		if( lineRequest.lineSpecificAmbientValues ) {
			lineRequest.lowerAmbientValues.resize(2u*lines);
			lineRequest.upperAmbientValues.resize(2u*lines);
		}
		lineRequest.values.resize(2u*lines*length);
		lineRequest.faceVelocityMPerS.resize(lines*(length+1u));
		if( component==sweepAxis ) {
			const std::size_t firstExtent=TestAxisExtent(shape,first);
			for( std::size_t line=0u;line<lines;++line ) {
				const std::size_t firstCoordinate=line%firstExtent;
				const std::size_t secondCoordinate=line/firstExtent;
				for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
					std::size_t xyz[]={0u,0u,0u};
					xyz[component]=componentPeriodic?coordinate:coordinate+1u;
					xyz[first]=firstCoordinate;xyz[second]=secondCoordinate;
					const std::size_t face=TransportFaceIndex(shape,component,
						xyz[0],xyz[1],xyz[2]);
					lineRequest.values[line*length+coordinate]=density[face];
					lineRequest.values[(lines+line)*length+coordinate]=momentum[face];
				}
				for( std::size_t face=0u;face<=length;++face ) {
					std::size_t lowerCoordinate=face,upperCoordinate=face+1u;
					if( componentPeriodic ) {
						upperCoordinate=face==length?0u:face;
						lowerCoordinate=upperCoordinate?upperCoordinate-1u:componentExtent-1u;
					}
					std::size_t lowerXYZ[]={0u,0u,0u},upperXYZ[]={0u,0u,0u};
					lowerXYZ[component]=lowerCoordinate;upperXYZ[component]=upperCoordinate;
					lowerXYZ[first]=firstCoordinate;upperXYZ[first]=firstCoordinate;
					lowerXYZ[second]=secondCoordinate;upperXYZ[second]=secondCoordinate;
					const float lower=request.frozenVelocityMPerS[component][
						TransportFaceIndex(shape,component,lowerXYZ[0],lowerXYZ[1],lowerXYZ[2])];
					const float upper=request.frozenVelocityMPerS[component][
						TransportFaceIndex(shape,component,upperXYZ[0],upperXYZ[1],upperXYZ[2])];
					lineRequest.faceVelocityMPerS[line*(length+1u)+face]=0.5f*(lower+upper);
				}
			}
		} else {
			const std::size_t remainingExtent=TestAxisExtent(shape,remaining);
			for( std::size_t line=0u;line<lines;++line ) {
				const std::size_t componentCoordinate=componentBeginning+line/remainingExtent;
				const std::size_t remainingCoordinate=line%remainingExtent;
				for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
					std::size_t xyz[]={0u,0u,0u};
					xyz[component]=componentCoordinate;xyz[sweepAxis]=coordinate;
					xyz[remaining]=remainingCoordinate;
					const std::size_t face=TransportFaceIndex(shape,component,
						xyz[0],xyz[1],xyz[2]);
					lineRequest.values[line*length+coordinate]=density[face];
					lineRequest.values[(lines+line)*length+coordinate]=momentum[face];
				}
				for( std::size_t face=0u;face<=length;++face )
					lineRequest.faceVelocityMPerS[line*(length+1u)+face]=
						IndependentDualCrossCarrier(request,component,sweepAxis,face,
							componentCoordinate,remainingCoordinate);
			}
		}
		if( lineRequest.lineSpecificAmbientValues )
			for( std::size_t line=0u;line<lines;++line ) {
				lineRequest.lowerAmbientValues[line]=request.ambientDensityKGPerM3;
				lineRequest.upperAmbientValues[line]=request.ambientDensityKGPerM3;
				lineRequest.lowerAmbientValues[lines+line]=component==sweepAxis?
					request.ambientDensityKGPerM3*lineRequest.faceVelocityMPerS[
						line*(length+1u)]:0.0f;
				lineRequest.upperAmbientValues[lines+line]=component==sweepAxis?
					request.ambientDensityKGPerM3*lineRequest.faceVelocityMPerS[
						line*(length+1u)+length]:0.0f;
			}
		RISE::FireProductionRemapResult remapped;
		if( !RISE::RemapFireProductionCPU(lineRequest,remapped,&error) ) return false;
		if( component==sweepAxis ) {
			const std::size_t firstExtent=TestAxisExtent(shape,first);
			for( std::size_t line=0u;line<lines;++line )
				for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
					std::size_t xyz[]={0u,0u,0u};
					xyz[component]=componentPeriodic?coordinate:coordinate+1u;
					xyz[first]=line%firstExtent;xyz[second]=line/firstExtent;
					const std::size_t face=TransportFaceIndex(shape,component,
						xyz[0],xyz[1],xyz[2]);
					density[face]=remapped.updatedValues[line*length+coordinate];
					momentum[face]=remapped.updatedValues[(lines+line)*length+coordinate];
				}
		} else {
			const std::size_t remainingExtent=TestAxisExtent(shape,remaining);
			for( std::size_t line=0u;line<lines;++line )
				for( std::size_t coordinate=0u;coordinate<length;++coordinate ) {
					std::size_t xyz[]={0u,0u,0u};
					xyz[component]=componentBeginning+line/remainingExtent;
					xyz[sweepAxis]=coordinate;xyz[remaining]=line%remainingExtent;
					const std::size_t face=TransportFaceIndex(shape,component,
						xyz[0],xyz[1],xyz[2]);
					density[face]=remapped.updatedValues[line*length+coordinate];
					momentum[face]=remapped.updatedValues[(lines+line)*length+coordinate];
				}
		}
		return true;
	}

	bool IndependentDualComponent(
		const RISE::FireProductionDualMomentumRequest& request,
		unsigned int component, std::vector<float>& density,
		std::vector<float>& momentum, std::string& error )
	{
		density=request.beginningFaceDensity[component];
		momentum=request.beginningMomentum[component];
		const RISE::FireProductionProjectionShape& shape=request.shape;
		const std::size_t componentExtent=TestAxisExtent(shape,component);
		const unsigned int first=(component+1u)%3u,second=(component+2u)%3u;
		for( std::size_t secondCoordinate=0u;
			secondCoordinate<TestAxisExtent(shape,second);++secondCoordinate )
			for( std::size_t firstCoordinate=0u;
				firstCoordinate<TestAxisExtent(shape,first);++firstCoordinate ) {
				std::size_t xyz[]={0u,0u,0u};
				xyz[first]=firstCoordinate;xyz[second]=secondCoordinate;
				if( request.boundary[2u*component]==RISE::FireProductionProjectionWall ) {
					xyz[component]=0u;
					momentum[TransportFaceIndex(shape,component,xyz[0],xyz[1],xyz[2])]=0.0f;
				}
				if( request.boundary[2u*component+1u]==RISE::FireProductionProjectionWall ) {
					xyz[component]=componentExtent;
					momentum[TransportFaceIndex(shape,component,xyz[0],xyz[1],xyz[2])]=0.0f;
				}
			}
		const unsigned int axes[]={0u,1u,2u,1u,0u};
		const float steps[]={0.5f,0.5f,1.0f,0.5f,0.5f};
		for( unsigned int pass=0u;pass<5u;++pass )
			if( !IndependentDualAxisPass(request,component,axes[pass],
				steps[pass]*request.timeStepS,density,momentum,error) ) return false;
		if( TestDualAxisPeriodic(request,component) ) {
			const std::size_t firstExtent=TestAxisExtent(shape,first);
			const std::size_t secondExtent=TestAxisExtent(shape,second);
			for( std::size_t secondCoordinate=0u;secondCoordinate<secondExtent;
				++secondCoordinate ) for( std::size_t firstCoordinate=0u;
				firstCoordinate<firstExtent;++firstCoordinate ) {
				std::size_t lowXYZ[]={0u,0u,0u},highXYZ[]={0u,0u,0u};
				lowXYZ[first]=firstCoordinate;lowXYZ[second]=secondCoordinate;
				highXYZ[first]=firstCoordinate;highXYZ[second]=secondCoordinate;
				highXYZ[component]=componentExtent;
				const std::size_t low=TransportFaceIndex(shape,component,
					lowXYZ[0],lowXYZ[1],lowXYZ[2]);
				const std::size_t high=TransportFaceIndex(shape,component,
					highXYZ[0],highXYZ[1],highXYZ[2]);
				density[high]=density[low];momentum[high]=momentum[low];
			}
		}
		return true;
	}

	bool NearFloat( float a, float b, float relative=2.0e-5f )
	{
		return std::isfinite(a)&&std::isfinite(b)&&
			std::fabs(a-b)<=relative*std::max(1.0f,std::max(std::fabs(a),std::fabs(b)));
	}

	bool SameFloatVectorsWithin( const std::vector<float>& first,
		const std::vector<float>& second, float relative )
	{
		return first.size()==second.size()&&std::equal(first.begin(),first.end(),second.begin(),
			[relative](float a,float b){return NearFloat(a,b,relative);});
	}

	bool SameRemapWithin( const RISE::FireProductionRemapResult& cpu,
		const RISE::FireProductionRemapResult& gpu, float relative )
	{
		if( cpu.updatedValues.size()!=gpu.updatedValues.size()||
			cpu.faceFluxes.size()!=gpu.faceFluxes.size()||
			cpu.sharedLimiterAlpha.size()!=gpu.sharedLimiterAlpha.size() ) return false;
		for( std::size_t i=0;i<cpu.updatedValues.size();++i )
			if( !NearFloat(cpu.updatedValues[i],gpu.updatedValues[i],relative) ) return false;
		for( std::size_t i=0;i<cpu.faceFluxes.size();++i )
			if( !NearFloat(cpu.faceFluxes[i],gpu.faceFluxes[i],relative) ) return false;
		for( std::size_t i=0;i<cpu.sharedLimiterAlpha.size();++i )
			if( !NearFloat(cpu.sharedLimiterAlpha[i],gpu.sharedLimiterAlpha[i],relative) ) return false;
		return true;
	}

	double SmoothPeriodicError( std::size_t cells, RISE::FireProductionRemapResult* output )
	{
		const double pi=std::acos(-1.0);
		const double shift=0.2;
		RISE::FireProductionRemapRequest request=PeriodicRequest(cells,1u,
			static_cast<float>(shift*static_cast<double>(cells)));
		for( std::size_t cell=0;cell<cells;++cell ) {
			const double left=static_cast<double>(cell)/static_cast<double>(cells);
			const double right=static_cast<double>(cell+1u)/static_cast<double>(cells);
			request.values[cell]=static_cast<float>(2.0+(std::cos(2.0*pi*left)-
				std::cos(2.0*pi*right))/(2.0*pi*(right-left)));
		}
		RISE::FireProductionRemapResult result;
		std::string error;
		if( !RISE::RemapFireProductionCPU(request,result,&error) )
			return std::numeric_limits<double>::max();
		double l1=0.0;
		for( std::size_t cell=0;cell<cells;++cell ) {
			const double left=static_cast<double>(cell)/static_cast<double>(cells)-shift;
			const double right=static_cast<double>(cell+1u)/static_cast<double>(cells)-shift;
			const double exact=2.0+(std::cos(2.0*pi*left)-std::cos(2.0*pi*right))/
				(2.0*pi/static_cast<double>(cells));
			l1+=std::fabs(static_cast<double>(result.updatedValues[cell])-exact);
		}
		if( output ) *output=result;
		return l1/static_cast<double>(cells);
	}
}

int main()
{
	using namespace RISE;
	const FireSimulationMethaneRecord& methane=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationGasOpacityRecord& opacity=
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	std::string error;
	FireProductionTablePackage package,repeated;
	Check(BuildFireProductionTablePackage(methane,opacity,package,&error),
		"record-derived production table package compiles");
	Check(BuildFireProductionTablePackage(methane,opacity,repeated,&error)&&
		package.CanonicalEnvelope()==repeated.CanonicalEnvelope()&&
		package.TablePackageId()==repeated.TablePackageId(),
		"production table compiler is byte deterministic");
	const std::string expectedTablePackageId=
		"3e8f46503657cf137bb5e812a9090ae1bf09792c7f2e7b1222eb9e3b2b70ce55";
	if( package.TablePackageId()!=expectedTablePackageId )
		std::cerr << "Observed table package ID: " << package.TablePackageId() << '\n';
	Check(package.TablePackageId()==expectedTablePackageId,
		"production r83 table package digest matches the independently pinned fixture");

	RISECBOR64::Value envelope;RISECBOR64::Bytes payloadBytes;
	const RISECBOR64::Value* payload=0;const RISECBOR64::Value* id=0;
	Check(RISECBOR64::DecodeCanonical(package.CanonicalEnvelope(),envelope,&error)&&
		envelope.GetType()==RISECBOR64::Value::Map&&envelope.GetMap().size()==2u&&
		(payload=envelope.Find("payload"))!=0&&(id=envelope.Find("table_package_id"))!=0&&
		id->GetType()==RISECBOR64::Value::Text&&RISECBOR64::Encode(*payload,payloadBytes,&error)&&
		id->GetText()==RISECBOR64::SHA256Hex(payloadBytes)&&id->GetText()==package.TablePackageId(),
		"production table package has one canonical payload preimage");
	Check(payload&&payload->Find("thermochemistry_record_id")&&
		payload->Find("thermochemistry_record_id")->GetText()==methane.RecordId()&&
		payload->Find("gas_opacity_record_id")&&
		payload->Find("gas_opacity_record_id")->GetText()==opacity.RecordId(),
		"production table manifest binds both source record IDs");

	Check(package.Thermochemistry().size()==methane.SpeciesOrder().size(),
		"production thermochemistry table preserves record species order");
	for( std::size_t speciesIndex=0;speciesIndex<package.Thermochemistry().size();++speciesIndex ) {
		const FireProductionThermochemistryTable& table=package.Thermochemistry()[speciesIndex];
		const FireThermochemistrySpecies* source=methane.FindSpecies(table.speciesId.c_str());
		Check(source&&table.speciesId==methane.SpeciesOrder()[speciesIndex]&&
			table.temperatureK.size()==table.sensibleEnthalpyJPerKG.size()&&
			table.temperatureK.size()==table.cpJPerKGK.size()&&table.temperatureK.size()>=2u,
			"production thermochemistry table shape and order are exact");
		if( !source ) continue;
		for( std::size_t i=0;i+1u<table.temperatureK.size();++i ) {
			const double fractions[]={0.25,0.5,0.75};
			for( const double fraction : fractions ) {
				const double sample=(1.0-fraction)*table.temperatureK[i]+
					fraction*table.temperatureK[i+1u];
				double exactH=0.0,exactCp=0.0,compiledH=0.0,compiledCp=0.0;
				const FireThermochemistrySegment* segment=SegmentAt(*source,sample);
				Check(segment&&methane.SensibleEnthalpyJPerKG(table.speciesId.c_str(),sample,exactH,&error)&&
					methane.CpJPerKGK(table.speciesId.c_str(),sample,exactCp,&error)&&
					package.ThermochemistryValues(table.speciesId.c_str(),sample,compiledH,compiledCp,&error)&&
					std::fabs(compiledH-exactH)<=table.maximumEnthalpyErrorJPerKG&&
					std::fabs(compiledCp-exactCp)*0.5*(table.temperatureK[i+1u]-table.temperatureK[i])<=
						table.maximumCpIntegratedErrorJPerKG&&
					std::fabs(compiledH-exactH)<=segment->certifiedCpLowerJPerKGK*0.25,
					"production thermochemistry independent samples meet the certified gate");
			}
		}
	}

	Check(package.Opacity().size()==2u&&package.Opacity()[0].speciesId=="CO2"&&
		package.Opacity()[1].speciesId=="H2O","production opacity table has the adopted species");
	for( const FireProductionOpacityTable& table : package.Opacity() ) {
		Check(table.maximumRelativeError<=0.005&&table.gasTemperatureK.size()>=2u&&
			table.radiationTemperatureK.size()>=2u,
			"production opacity compiler meets the derived half-percent gate");
		for( std::size_t g=0;g+1u<table.gasTemperatureK.size();++g )
			for( std::size_t r=0;r+1u<table.radiationTemperatureK.size();++r ) {
				const double fractions[]={0.125,0.375,0.625,0.875};
				for( const double gasFraction : fractions ) for( const double radiationFraction : fractions ) {
				const double gas=(1.0-gasFraction)*table.gasTemperatureK[g]+
					gasFraction*table.gasTemperatureK[g+1u];
				const double radiation=(1.0-radiationFraction)*table.radiationTemperatureK[r]+
					radiationFraction*table.radiationTemperatureK[r+1u];
				double exact=0.0,dGas=0.0,dRadiation=0.0,compiled=0.0;
				Check(opacity.PlanckMeanCrossSectionM2PerMolecule(table.speciesId.c_str(),gas,
					radiation,exact,dGas,dRadiation,&error)&&
					package.PlanckMeanM2PerMolecule(table.speciesId.c_str(),gas,radiation,compiled,&error)&&
					std::fabs(compiled-exact)<=table.maximumRelativeError*
						std::max(std::fabs(compiled),std::fabs(exact))&&
					std::fabs(compiled-exact)<=0.005*std::max(std::fabs(compiled),std::fabs(exact)),
					"production opacity independent stencil matches the certified record");
				}
			}
	}

	double h=11.0,cp=12.0,kappa=13.0;
	const double nan=std::numeric_limits<double>::quiet_NaN();
	const double infinity=std::numeric_limits<double>::infinity();
	Check(!package.ThermochemistryValues("CH4",methane.TemperatureMinK()-1.0,h,cp,&error)&&
		h==11.0&&cp==12.0&&!package.ThermochemistryValues("CH4",methane.TemperatureMaxK()+1.0,h,cp,&error)&&
		!package.ThermochemistryValues("CH4",nan,h,cp,&error)&&
		!package.ThermochemistryValues("CH4",infinity,h,cp,&error)&&
		!package.ThermochemistryValues("unknown",methane.TemperatureMinK(),h,cp,&error)&&
		h==11.0&&cp==12.0,
		"production thermochemistry rejects both bounds, nonfinite input, and unknown species");
	Check(!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK()-1.0,
		opacity.TemperatureMinK(),kappa,&error)&&kappa==13.0&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMaxK()+1.0,
			opacity.TemperatureMinK(),kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),
			opacity.TemperatureMinK()-1.0,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),
			opacity.TemperatureMaxK()+1.0,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",nan,opacity.TemperatureMinK(),kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),infinity,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("unknown",opacity.TemperatureMinK(),
			opacity.TemperatureMinK(),kappa,&error)&&kappa==13.0,
		"production opacity rejects every axis bound, nonfinite input, and unknown species");
	const FireProductionThermochemistryTable& ch4=package.Thermochemistry().front();
	const double aboveThermoKnot=std::nextafter(static_cast<double>(
		ch4.temperatureK[ch4.temperatureK.size()/2u]),infinity);
	const FireProductionOpacityTable& co2=package.Opacity().front();
	const double aboveGasKnot=std::nextafter(static_cast<double>(
		co2.gasTemperatureK[co2.gasTemperatureK.size()/2u]),infinity);
	const double aboveRadiationKnot=std::nextafter(static_cast<double>(
		co2.radiationTemperatureK[co2.radiationTemperatureK.size()/2u]),infinity);
	Check(package.ThermochemistryValues("CH4",aboveThermoKnot,h,cp,&error)&&
		package.PlanckMeanM2PerMolecule("CO2",aboveGasKnot,aboveRadiationKnot,kappa,&error),
		"in-domain values immediately above fp32 knots select the upper interval");

	FireProductionRemapRequest constant=PeriodicRequest(32u,3u,0.3f);
	constant.values.assign(constant.values.size(),0.1f);
	FireProductionRemapResult constantCPU;
	const bool constantRemapped=RemapFireProductionCPU(constant,constantCPU,&error);
	const bool constantExact=constantRemapped&&
		std::all_of(constantCPU.updatedValues.begin(),constantCPU.updatedValues.end(),
			[](float value){return value==0.1f;});
	if( !constantExact&&constantRemapped ) {
		auto range=std::minmax_element(constantCPU.updatedValues.begin(),constantCPU.updatedValues.end());
		std::cerr << std::hexfloat << "Observed constant remap range: " << *range.first << ", "
			<< *range.second << std::defaultfloat << '\n';
		for( std::size_t i=0;i<constantCPU.updatedValues.size();++i )
			if( constantCPU.updatedValues[i]!=0.1f ) {
				std::cerr << "First constant mismatch index " << i << " value " << std::hexfloat
					<< constantCPU.updatedValues[i] << std::defaultfloat << '\n';break;
			}
		auto alphaRange=std::minmax_element(constantCPU.sharedLimiterAlpha.begin(),
			constantCPU.sharedLimiterAlpha.end());
		std::cerr << "Constant alpha range: " << *alphaRange.first << ", " << *alphaRange.second << '\n';
	}
	Check(constantExact,
		"production remap preserves an ordinary binary32 periodic constant exactly");

	FireProductionRemapRequest latePrefix=PeriodicRequest(1024u,1u,1.0e-6f);
	latePrefix.cellWidthM=1.0f;latePrefix.timeStepS=1.0e-6f;
	latePrefix.values.assign(latePrefix.values.size(),1.0e8f);
	FireProductionRemapResult latePrefixCPU;
	Check(RemapFireProductionCPU(latePrefix,latePrefixCPU,&error)&&
		latePrefixCPU.faceFluxes[RemapFluxIndex(latePrefix,0u,0u,1023u)]>0.0f&&
		std::all_of(latePrefixCPU.updatedValues.begin(),latePrefixCPU.updatedValues.end(),
			[](float value){return value==1.0e8f;}),
		"local fractional integral survives a tiny late-cell sweep without prefix cancellation");
	FireProductionRemapRequest subUlpSweep=PeriodicRequest(9u,1u,0x1p-25f);
	subUlpSweep.cellWidthM=1.0f;subUlpSweep.timeStepS=0x1p-25f;
	subUlpSweep.values.assign(subUlpSweep.values.size(),0.1f);
	FireProductionRemapResult subUlpSweepCPU;
	Check(RemapFireProductionCPU(subUlpSweep,subUlpSweepCPU,&error)&&
		subUlpSweepCPU.faceFluxes.front()>0.0f&&
		std::all_of(subUlpSweepCPU.updatedValues.begin(),subUlpSweepCPU.updatedValues.end(),
			[](float value){return value==0.1f;}),
		"sub-ulp trailing sweep advances without constructing the rounded value one-minus-C");
	for( const std::pair<std::size_t,float>& largeCourant : {
		std::make_pair(std::size_t(5u),0x1.fff832p+24f),
		std::make_pair(std::size_t(7u),0x1.110bb6p+62f)} ) {
		FireProductionRemapRequest large=PeriodicRequest(largeCourant.first,1u,
			largeCourant.second);
		large.cellWidthM=1.0f;large.timeStepS=largeCourant.second;
		large.values.assign(large.values.size(),0.1f);
		FireProductionRemapResult largeCPU;
		Check(RemapFireProductionCPU(large,largeCPU,&error)&&
			std::all_of(largeCPU.updatedValues.begin(),largeCPU.updatedValues.end(),
				[](float value){return value==0.1f;}),
			"large finite periodic Courant has a bounded canonical quotient and remainder");
	}

	FireProductionRemapRequest seam=PeriodicRequest(7u,1u,0.1f);
	for( std::size_t cell=0;cell<seam.lineLength;++cell )
		seam.values[cell]=0.1f+0.137f*static_cast<float>(cell);
	FireProductionRemapResult seamPositive,seamNegative;
	const bool seamPositiveOK=RemapFireProductionCPU(seam,seamPositive,&error);
	std::fill(seam.faceVelocityMPerS.begin(),seam.faceVelocityMPerS.end(),-1.0f);
	const bool seamNegativeOK=RemapFireProductionCPU(seam,seamNegative,&error);
	Check(seamPositiveOK&&seamNegativeOK&&
		seamPositive.faceFluxes.front()==seamPositive.faceFluxes.back()&&
		seamNegative.faceFluxes.front()==seamNegative.faceFluxes.back(),
		"non-power-of-two periodic lines publish one exact seam for both velocity signs");
	FireProductionRemapRequest unequalSeam=seam;
	unequalSeam.faceVelocityMPerS.back()=-2.0f;
	Check(!ValidateFireProductionRemapRequest(unequalSeam,&error),
		"periodic remap rejects a multi-valued seam velocity");

	FireProductionRemapRequest blelloch=PeriodicRequest(8u,1u,8.0f);
	blelloch.values={1.0e8f,1.0f,-1.0e8f,1.0f,1.0e8f,1.0f,-1.0e8f,1.0f};
	FireProductionRemapResult blellochCPU;
	Check(RemapFireProductionCPU(blelloch,blellochCPU,&error)&&
		blellochCPU.faceFluxes.front()==0.0f&&
		blellochCPU.faceFluxes.front()==blellochCPU.faceFluxes.back(),
		"whole-domain transport uses the pinned cancellation-sensitive Blelloch root");

	for( const float signedCourant : {0.75f,2.25f,70.25f,-2.25f} ) {
		FireProductionRemapRequest pulse=PeriodicRequest(64u,2u,std::fabs(signedCourant));
		if( signedCourant<0.0f )
			std::fill(pulse.faceVelocityMPerS.begin(),pulse.faceVelocityMPerS.end(),-1.0f);
		for( std::size_t cell=16u;cell<32u;++cell ) {
			pulse.values[RemapValueIndex(pulse,0u,0u,cell)]=1.0f;
			pulse.values[RemapValueIndex(pulse,1u,0u,cell)]=0.5f;
		}
		FireProductionRemapResult pulseCPU;
		Check(RemapFireProductionCPU(pulse,pulseCPU,&error),
			"periodic pulse remaps below, above, negative, and beyond-domain Courant numbers");
		for( std::size_t component=0;component<2u;++component ) {
			float before=0.0f,after=0.0f;
			for( std::size_t cell=0;cell<pulse.lineLength;++cell ) {
				before+=pulse.values[RemapValueIndex(pulse,component,0u,cell)];
				after+=pulseCPU.updatedValues[RemapValueIndex(pulse,component,0u,cell)];
			}
			Check(before==after,"periodic arbitrary-Courant pulse conserves each component");
		}
	}

	FireProductionRemapResult smooth64Result;
	const double smooth16=SmoothPeriodicError(16u,0);
	const double smooth32=SmoothPeriodicError(32u,0);
	const double smooth64=SmoothPeriodicError(64u,&smooth64Result);
	const double order16To32=std::log(smooth16/smooth32)/std::log(2.0);
	const double order32To64=std::log(smooth32/smooth64)/std::log(2.0);
	if( !(order16To32>=1.8&&order32To64>=1.8) )
		std::cerr << "Observed production remap orders: " << order16To32 << ", "
			<< order32To64 << " errors: " << smooth16 << ", " << smooth32 << ", "
			<< smooth64 << '\n';
	Check(order16To32>=1.8&&order32To64>=1.8,
		"smooth periodic production remap has at least 1.8 observed order");

	FireProductionRemapRequest affine=PeriodicRequest(48u,3u,1.35f);
	for( std::size_t cell=0;cell<affine.lineLength;++cell ) {
		const float first=1.0f+0.4f*std::sin(static_cast<float>(2.0*std::acos(-1.0)*
			static_cast<double>(cell)/static_cast<double>(affine.lineLength)));
		const float second=0.6f+(cell>=11u&&cell<19u?0.7f:0.0f)+
			0.1f*std::cos(static_cast<float>(6.0*std::acos(-1.0)*
			static_cast<double>(cell)/static_cast<double>(affine.lineLength)));
		affine.values[RemapValueIndex(affine,0u,0u,cell)]=first;
		affine.values[RemapValueIndex(affine,1u,0u,cell)]=second;
		affine.values[RemapValueIndex(affine,2u,0u,cell)]=first+second;
	}
	FireProductionRemapResult affineCPU;
	Check(RemapFireProductionCPU(affine,affineCPU,&error),
		"common-limiter affine tuple remaps");
	float maximumAffineResidual=0.0f;
	for( std::size_t cell=0;cell<affine.lineLength;++cell )
		maximumAffineResidual=std::max(maximumAffineResidual,std::fabs(
			affineCPU.updatedValues[RemapValueIndex(affine,2u,0u,cell)]-
			(affineCPU.updatedValues[RemapValueIndex(affine,0u,0u,cell)]+
			 affineCPU.updatedValues[RemapValueIndex(affine,1u,0u,cell)])));
	if( maximumAffineResidual>2.0e-5f )
		std::cerr << "Observed common-weight affine residual: " << maximumAffineResidual << '\n';
	Check(maximumAffineResidual<=2.0e-5f,
		"one tuple limiter preserves a cancellation-sensitive affine constituent row");

	FireProductionRemapRequest open;
	open.lineLength=8u;open.lineCount=1u;open.componentCount=2u;
	open.cellWidthM=1.0f;open.timeStepS=0.5f;
	open.boundary=FireProductionRemapPressureOpen;
	open.values.assign(16u,2.0f);open.faceVelocityMPerS.assign(9u,0.0f);
	open.faceVelocityMPerS.front()=1.0f;open.faceVelocityMPerS.back()=1.0f;
	open.ambientValues={5.0f,7.0f};
	FireProductionRemapResult openCPU;
	Check(RemapFireProductionCPU(open,openCPU,&error)&&
		openCPU.faceFluxes[RemapFluxIndex(open,0u,0u,0u)]==2.5f&&
		openCPU.faceFluxes[RemapFluxIndex(open,0u,0u,8u)]==1.0f&&
		openCPU.faceFluxes[RemapFluxIndex(open,1u,0u,0u)]==3.5f&&
		openCPU.faceFluxes[RemapFluxIndex(open,1u,0u,8u)]==1.0f,
		"pressure-open remap uses ambient inflow and nearest-interior outflow");
	FireProductionRemapRequest openNegative=open;
	openNegative.faceVelocityMPerS.front()=-1.0f;
	openNegative.faceVelocityMPerS.back()=-1.0f;
	FireProductionRemapResult openNegativeCPU;
	Check(RemapFireProductionCPU(openNegative,openNegativeCPU,&error)&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,0u,0u,0u)]==-1.0f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,0u,0u,8u)]==-2.5f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,1u,0u,0u)]==-1.0f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,1u,0u,8u)]==-3.5f,
		"pressure-open remap reverses ambient and interior roles under negative velocity");
	FireProductionRemapRequest wall=open;wall.boundary=FireProductionRemapWall;
	FireProductionRemapResult wallCPU;
	const bool wallRemapped=RemapFireProductionCPU(wall,wallCPU,&error);
	bool everyWallFluxZero=wallRemapped;
	for( std::size_t component=0;component<wall.componentCount;++component )
		everyWallFluxZero=everyWallFluxZero&&
			wallCPU.faceFluxes[RemapFluxIndex(wall,component,0u,0u)]==0.0f&&
			wallCPU.faceFluxes[RemapFluxIndex(wall,component,0u,wall.lineLength)]==0.0f;
	Check(everyWallFluxZero,
		"wall production remap has exact zero boundary flux");
	FireProductionRemapRequest wallNegative=openNegative;
	wallNegative.boundary=FireProductionRemapWall;
	FireProductionRemapResult wallNegativeCPU;
	const bool wallNegativeRemapped=RemapFireProductionCPU(wallNegative,wallNegativeCPU,&error);
	bool everyNegativeWallFluxZero=wallNegativeRemapped;
	for( std::size_t component=0;component<wallNegative.componentCount;++component )
		everyNegativeWallFluxZero=everyNegativeWallFluxZero&&
			wallNegativeCPU.faceFluxes[RemapFluxIndex(wallNegative,component,0u,0u)]==0.0f&&
			wallNegativeCPU.faceFluxes[RemapFluxIndex(wallNegative,component,0u,
				wallNegative.lineLength)]==0.0f;
	Check(everyNegativeWallFluxZero,
		"wall production remap has exact zero boundary flux under negative velocity");
	FireProductionRemapRequest wallToOpen=open;
	wallToOpen.asymmetricBoundaries=true;
	wallToOpen.lowerBoundary=FireProductionRemapWall;
	wallToOpen.upperBoundary=FireProductionRemapPressureOpen;
	FireProductionRemapResult wallToOpenCPU;
	Check(RemapFireProductionCPU(wallToOpen,wallToOpenCPU,&error)&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,0u,0u,0u)]==0.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,0u,0u,8u)]==1.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,1u,0u,0u)]==0.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,1u,0u,8u)]==1.0f,
		"asymmetric remap applies a lower wall and upper pressure-open outflow independently");
	FireProductionRemapRequest openToWall=open;
	openToWall.asymmetricBoundaries=true;
	openToWall.lowerBoundary=FireProductionRemapPressureOpen;
	openToWall.upperBoundary=FireProductionRemapWall;
	FireProductionRemapResult openToWallCPU;
	Check(RemapFireProductionCPU(openToWall,openToWallCPU,&error)&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,0u,0u,0u)]==2.5f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,0u,0u,8u)]==0.0f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,1u,0u,0u)]==3.5f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,1u,0u,8u)]==0.0f,
		"asymmetric remap applies lower pressure-open inflow and an upper wall independently");
	FireProductionRemapRequest lineAmbient;
	lineAmbient.lineLength=4u;lineAmbient.lineCount=3u;lineAmbient.componentCount=2u;
	lineAmbient.cellWidthM=1.0f;lineAmbient.timeStepS=0.25f;
	lineAmbient.boundary=FireProductionRemapPressureOpen;
	lineAmbient.lineSpecificAmbientValues=true;
	lineAmbient.values.resize(lineAmbient.componentCount*lineAmbient.lineCount*
		lineAmbient.lineLength);
	for( std::size_t component=0u;component<lineAmbient.componentCount;++component )
		for( std::size_t line=0u;line<lineAmbient.lineCount;++line )
			for( std::size_t cell=0u;cell<lineAmbient.lineLength;++cell )
				lineAmbient.values[RemapValueIndex(lineAmbient,component,line,cell)]=
					4.0f+static_cast<float>(component*3u+line);
	lineAmbient.faceVelocityMPerS.assign(lineAmbient.lineCount*
		(lineAmbient.lineLength+1u),0.0f);
	lineAmbient.lowerAmbientValues.resize(lineAmbient.componentCount*lineAmbient.lineCount);
	lineAmbient.upperAmbientValues.resize(lineAmbient.componentCount*lineAmbient.lineCount);
	for( std::size_t component=0u;component<lineAmbient.componentCount;++component )
		for( std::size_t line=0u;line<lineAmbient.lineCount;++line ) {
			const std::size_t ambientIndex=component*lineAmbient.lineCount+line;
			lineAmbient.lowerAmbientValues[ambientIndex]=
				1.0f+static_cast<float>(component*8u+line);
			lineAmbient.upperAmbientValues[ambientIndex]=
				17.0f+static_cast<float>(component*8u+line);
		}
	for( std::size_t line=0u;line<lineAmbient.lineCount;++line ) {
		lineAmbient.faceVelocityMPerS[line*(lineAmbient.lineLength+1u)]=0.5f;
		lineAmbient.faceVelocityMPerS[line*(lineAmbient.lineLength+1u)+
			lineAmbient.lineLength]=-0.75f;
	}
	FireProductionRemapResult lineAmbientCPU;
	bool everyLineAmbientFlux=RemapFireProductionCPU(lineAmbient,lineAmbientCPU,&error);
	for( std::size_t component=0u;component<lineAmbient.componentCount;++component )
		for( std::size_t line=0u;line<lineAmbient.lineCount;++line ) {
			const std::size_t ambientIndex=component*lineAmbient.lineCount+line;
			everyLineAmbientFlux=everyLineAmbientFlux&&
				lineAmbientCPU.faceFluxes[RemapFluxIndex(lineAmbient,component,line,0u)]==
					0.125f*lineAmbient.lowerAmbientValues[ambientIndex]&&
				lineAmbientCPU.faceFluxes[RemapFluxIndex(lineAmbient,component,line,
					lineAmbient.lineLength)]==
					-0.1875f*lineAmbient.upperAmbientValues[ambientIndex];
		}
	Check(everyLineAmbientFlux,
		"pressure-open remap binds distinct lower and upper ambient tuples for every line");
	auto seedRemapResult=[](FireProductionRemapResult& seeded) {
		seeded.updatedValues.assign(1u,1.0f);seeded.faceFluxes.assign(1u,2.0f);
		seeded.sharedLimiterAlpha.assign(1u,3.0f);seeded.deviceElapsedMS=4.0;
	};
	auto remapResultIsDefault=[](const FireProductionRemapResult& rejected) {
		return rejected.updatedValues.empty()&&rejected.faceFluxes.empty()&&
			rejected.sharedLimiterAlpha.empty()&&rejected.deviceElapsedMS==0.0;
	};
	FireProductionRemapRequest malformedLineAmbient=lineAmbient;
	malformedLineAmbient.lowerAmbientValues.pop_back();
	FireProductionRemapResult malformedLineAmbientResult;
	seedRemapResult(malformedLineAmbientResult);
	Check(!RemapFireProductionCPU(malformedLineAmbient,malformedLineAmbientResult,&error)&&
		remapResultIsDefault(malformedLineAmbientResult)&&
		error.find("shape")!=std::string::npos,
		"line-specific pressure-open ambient tuples require complete lower and upper shapes");
	malformedLineAmbient=lineAmbient;
	malformedLineAmbient.upperAmbientValues.pop_back();
	seedRemapResult(malformedLineAmbientResult);
	Check(!RemapFireProductionCPU(malformedLineAmbient,malformedLineAmbientResult,&error)&&
		remapResultIsDefault(malformedLineAmbientResult)&&
		error.find("shape")!=std::string::npos,
		"line-specific pressure-open ambient tuples reject an incomplete upper side");
	malformedLineAmbient=lineAmbient;
	malformedLineAmbient.lowerAmbientValues[1u]=std::numeric_limits<float>::infinity();
	seedRemapResult(malformedLineAmbientResult);
	Check(!RemapFireProductionCPU(malformedLineAmbient,malformedLineAmbientResult,&error)&&
		remapResultIsDefault(malformedLineAmbientResult)&&
		error.find("nonfinite")!=std::string::npos,
		"line-specific pressure-open ambient tuples reject nonfinite lower-side values");
	malformedLineAmbient=lineAmbient;
	malformedLineAmbient.upperAmbientValues[1u]=std::numeric_limits<float>::quiet_NaN();
	seedRemapResult(malformedLineAmbientResult);
	Check(!RemapFireProductionCPU(malformedLineAmbient,malformedLineAmbientResult,&error)&&
		remapResultIsDefault(malformedLineAmbientResult)&&
		error.find("nonfinite")!=std::string::npos,
		"line-specific pressure-open ambient tuples reject nonfinite upper-side values");
	FireProductionRemapRequest invalidPeriodicPair=open;
	invalidPeriodicPair.asymmetricBoundaries=true;
	invalidPeriodicPair.lowerBoundary=FireProductionRemapPeriodic;
	invalidPeriodicPair.upperBoundary=FireProductionRemapWall;
	Check(!ValidateFireProductionRemapRequest(invalidPeriodicPair,&error)&&
		error.find("boundary pairing")!=std::string::npos,
		"asymmetric remap rejects an unpaired periodic boundary");
	FireProductionRemapRequest folded=open;
	folded.timeStepS=0.75f;
	folded.faceVelocityMPerS.assign(9u,0.0f);
	folded.faceVelocityMPerS[3u]=-1.0f;
	folded.faceVelocityMPerS[4u]=1.0f;
	FireProductionRemapResult foldedResult;foldedResult.updatedValues.push_back(9.0f);
	Check(!RemapFireProductionCPU(folded,foldedResult,&error)&&
		foldedResult.updatedValues.empty()&&error.find("folded")!=std::string::npos,
		"production remap rejects a crossed departure map instead of draining a donor twice");
	FireProductionRemapRequest roundedFold;
	roundedFold.lineLength=4u;roundedFold.lineCount=1u;roundedFold.componentCount=1u;
	roundedFold.cellWidthM=0x1.a0e166p+72f;roundedFold.timeStepS=0x1.34a348p+38f;
	roundedFold.boundary=FireProductionRemapPressureOpen;
	roundedFold.values.assign(4u,1.0e-30f);roundedFold.ambientValues.assign(1u,1.0e-30f);
	roundedFold.faceVelocityMPerS={0x1.cdfcecp+57f,0x1.cdfceep+57f,
		0x1.cdfceep+57f,0x1.cdfceep+57f,0x1.cdfceep+57f};
	Check(!ValidateFireProductionRemapRequest(roundedFold,&error)&&
		error.find("folded")!=std::string::npos,
		"fold admission evaluates the same rounded binary32 Courants as the kernels");
	FireProductionRemapRequest cancellationFold;
	cancellationFold.lineLength=4u;cancellationFold.lineCount=1u;
	cancellationFold.componentCount=1u;cancellationFold.cellWidthM=1.0f;
	cancellationFold.timeStepS=1.0f;cancellationFold.boundary=FireProductionRemapPressureOpen;
	cancellationFold.values.assign(4u,0x1p-100f);
	cancellationFold.ambientValues.assign(1u,0x1p-100f);
	cancellationFold.faceVelocityMPerS={0x1.000002p+24f,0x1.000002p+24f,
		0x1.000002p+24f,0x1.000004p+24f,0x1.000004p+24f};
	Check(!ValidateFireProductionRemapRequest(cancellationFold,&error)&&
		error.find("folded")!=std::string::npos,
		"fold admission preserves unit face separation after rounded large Courants");
	FireProductionRemapRequest invalid=constant;
	invalid.values[0]=std::numeric_limits<float>::quiet_NaN();
	FireProductionRemapResult invalidResult;invalidResult.updatedValues.push_back(9.0f);
	Check(!RemapFireProductionCPU(invalid,invalidResult,&error)&&
		invalidResult.updatedValues.empty()&&!error.empty(),
		"production remap rejects nonfinite state without returning partial output");
	FireProductionRemapRequest finiteOverflow=constant;
	finiteOverflow.values.assign(finiteOverflow.values.size(),
		std::numeric_limits<float>::max());
	FireProductionRemapResult finiteOverflowResult;
	Check(!RemapFireProductionCPU(finiteOverflow,finiteOverflowResult,&error)&&
		finiteOverflowResult.updatedValues.empty()&&!error.empty(),
		"finite input that overflows derived arithmetic fails without published output");
	FireProductionRemapRequest overflow=constant;
	overflow.lineCount=std::numeric_limits<std::size_t>::max();
	std::uint64_t overflowWorkingBytes=9u;
	Check(!FireProductionRemapWorkingSetBytes(overflow,overflowWorkingBytes)&&
		overflowWorkingBytes==0u&&
		!ValidateFireProductionRemapRequest(overflow,&error)&&!error.empty(),
		"production remap rejects overflowing dimensions before indexing or allocation");
	FireProductionRemapRequest excessiveWorkingSet;
	excessiveWorkingSet.lineLength=1024u;excessiveWorkingSet.lineCount=70000u;
	excessiveWorkingSet.componentCount=1u;excessiveWorkingSet.cellWidthM=1.0f;
	excessiveWorkingSet.timeStepS=0.0f;excessiveWorkingSet.boundary=FireProductionRemapPeriodic;
	Check(!ValidateFireProductionRemapRequest(excessiveWorkingSet,&error)&&
		error.find("two GiB")!=std::string::npos,
		"production remap accounts for every Metal buffer before the two-GiB admission gate");
	FireProductionRemapRequest admittedWorkingSet=excessiveWorkingSet;
	admittedWorkingSet.lineCount=65000u;
	Check(!ValidateFireProductionRemapRequest(admittedWorkingSet,&error)&&
		error.find("two GiB")==std::string::npos,
		"working-set RED straddles the complete two-GiB allocation boundary");
	FireProductionRemapRequest finalBytesBoundary;
	finalBytesBoundary.lineLength=15u;finalBytesBoundary.lineCount=4364545u;
	finalBytesBoundary.componentCount=1u;finalBytesBoundary.cellWidthM=1.0f;
	finalBytesBoundary.timeStepS=0.0f;
	finalBytesBoundary.boundary=FireProductionRemapPeriodic;
	std::uint64_t finalBytesBoundaryCount=0u,finalBytesBelowCount=0u;
	Check(FireProductionRemapWorkingSetBytes(finalBytesBoundary,
		finalBytesBoundaryCount)&&finalBytesBoundaryCount==UINT64_C(2147500032)&&
		!ValidateFireProductionRemapRequest(finalBytesBoundary,&error)&&
		error.find("two GiB")!=std::string::npos,
		"two-GiB admission counts the ambient tuple and parameter at allocation granularity");
	FireProductionRemapRequest finalBytesBelow=finalBytesBoundary;
	finalBytesBelow.lineCount-=1u;
	Check(FireProductionRemapWorkingSetBytes(finalBytesBelow,finalBytesBelowCount)&&
		finalBytesBelowCount==UINT64_C(2147450880)&&
		!ValidateFireProductionRemapRequest(finalBytesBelow,&error)&&
		error.find("two GiB")==std::string::npos,
		"rounded-allocation resource fixture has a discriminating below-bound companion");
	FireProductionRemapRequest lineAmbientResourceBoundary;
	lineAmbientResourceBoundary.lineLength=9u;
	lineAmbientResourceBoundary.lineCount=6971848u;
	lineAmbientResourceBoundary.componentCount=1u;
	lineAmbientResourceBoundary.cellWidthM=1.0f;
	lineAmbientResourceBoundary.timeStepS=0.0f;
	lineAmbientResourceBoundary.boundary=FireProductionRemapPeriodic;
	lineAmbientResourceBoundary.lineSpecificAmbientValues=true;
	std::uint64_t lineAmbientResourceBytes=0u,lineAmbientResourceBelowBytes=0u;
	Check(FireProductionRemapWorkingSetBytes(lineAmbientResourceBoundary,
		lineAmbientResourceBytes)&&lineAmbientResourceBytes==UINT64_C(2147500032)&&
		!ValidateFireProductionRemapRequest(lineAmbientResourceBoundary,&error)&&
		error.find("two GiB")!=std::string::npos,
		"line-specific resource admission counts both per-line ambient side buffers");
	lineAmbientResourceBoundary.lineCount-=1u;
	Check(FireProductionRemapWorkingSetBytes(lineAmbientResourceBoundary,
		lineAmbientResourceBelowBytes)&&lineAmbientResourceBelowBytes==UINT64_C(2147418112)&&
		!ValidateFireProductionRemapRequest(lineAmbientResourceBoundary,&error)&&
		error.find("two GiB")==std::string::npos,
		"line-specific resource fixture has a discriminating below-bound companion");

	FireProductionCellPalindromeRequest translated;
	translated.shape.nx=8u;translated.shape.ny=8u;translated.shape.nz=8u;
	translated.shape.cellWidthM=1.0f;translated.componentCount=2u;
	translated.timeStepS=1.0f;
	translated.boundary.fill(FireProductionProjectionPeriodic);
	translated.conservativeValues.assign(2u*translated.shape.CellCount(),0.0f);
	translated.ambientValues.assign(2u,0.0f);
	for( unsigned int axis=0u;axis<3u;++axis )
		translated.frozenVelocityMPerS[axis].assign(
			FireProductionProjectionFaceCount(translated.shape,axis),axis<2u?2.0f:1.0f);
	const std::size_t translatedBeginning=TransportCellIndex(translated.shape,1u,1u,1u);
	translated.conservativeValues[translatedBeginning]=1.0f;
	translated.conservativeValues[translated.shape.CellCount()+translatedBeginning]=2.0f;
	const unsigned int canonicalAxes[]={0u,1u,2u,1u,0u};
	const float canonicalFactors[]={0.5f,0.5f,1.0f,0.5f,0.5f};
	std::vector<float> translatedIndependent;
	const bool translatedReference=IndependentCellComposition(translated,canonicalAxes,
		canonicalFactors,5u,translatedIndependent,error);
	FireProductionCellPalindromeResult translatedResult;
	const std::size_t translatedEnd=TransportCellIndex(translated.shape,3u,3u,2u);
	bool exactTranslatedTuple=false;
	if( RemapFireProductionCellPalindromeCPU(translated,translatedResult,&error) ) {
		exactTranslatedTuple=translatedResult.executedSubmapCount==5u;
		for( std::size_t cell=0;cell<translated.shape.CellCount();++cell )
			exactTranslatedTuple=exactTranslatedTuple&&
				translatedResult.conservativeValues[cell]==(cell==translatedEnd?1.0f:0.0f)&&
				translatedResult.conservativeValues[translated.shape.CellCount()+cell]==
					(cell==translatedEnd?2.0f:0.0f);
	}
	Check(translatedReference&&exactTranslatedTuple&&
		translatedResult.conservativeValues==translatedIndependent,
		"production palindrome exactly translates an integer-Courant tuple by (2,2,1)");

	FireProductionPeriodicDualMomentumRequest dualTranslated;
	dualTranslated.shape=translated.shape;dualTranslated.timeStepS=translated.timeStepS;
	for( unsigned int axis=0u;axis<3u;++axis ) {
		const std::size_t faces=FireProductionProjectionFaceCount(dualTranslated.shape,axis);
		dualTranslated.beginningFaceDensity[axis].assign(faces,1.0f);
		dualTranslated.beginningMomentum[axis].assign(faces,0.0f);
		dualTranslated.frozenVelocityMPerS[axis].assign(faces,axis<2u?2.0f:1.0f);
		const std::size_t beginning=TransportFaceIndex(dualTranslated.shape,axis,1u,1u,1u);
		dualTranslated.beginningMomentum[axis][beginning]=static_cast<float>(axis+1u);
		if( axis==0u ) {
			dualTranslated.beginningMomentum[axis][TransportFaceIndex(
				dualTranslated.shape,axis,dualTranslated.shape.nx,1u,1u)]=
				dualTranslated.beginningMomentum[axis][TransportFaceIndex(
					dualTranslated.shape,axis,0u,1u,1u)];
		} else if( axis==1u ) {
			dualTranslated.beginningMomentum[axis][TransportFaceIndex(
				dualTranslated.shape,axis,1u,dualTranslated.shape.ny,1u)]=
				dualTranslated.beginningMomentum[axis][TransportFaceIndex(
					dualTranslated.shape,axis,1u,0u,1u)];
		} else {
			dualTranslated.beginningMomentum[axis][TransportFaceIndex(
				dualTranslated.shape,axis,1u,1u,dualTranslated.shape.nz)]=
				dualTranslated.beginningMomentum[axis][TransportFaceIndex(
					dualTranslated.shape,axis,1u,1u,0u)];
		}
	}
	FireProductionPeriodicDualMomentumResult dualTranslatedResult;
	bool exactDualTranslation=RemapFireProductionPeriodicDualMomentumCPU(
		dualTranslated,dualTranslatedResult,&error)&&
		dualTranslatedResult.executedSubmapCount==15u&&
		dualTranslatedResult.canonicalSeamCopyCount==2u*(
			dualTranslated.shape.ny*dualTranslated.shape.nz+
			dualTranslated.shape.nx*dualTranslated.shape.nz+
			dualTranslated.shape.nx*dualTranslated.shape.ny);
	for( unsigned int axis=0u;axis<3u&&exactDualTranslation;++axis ) {
		const std::size_t expected=TransportFaceIndex(dualTranslated.shape,axis,3u,3u,2u);
		for( std::size_t z=0u;z<dualTranslated.shape.nz;++z )
			for( std::size_t y=0u;y<dualTranslated.shape.ny;++y )
				for( std::size_t x=0u;x<dualTranslated.shape.nx;++x ) {
					const std::size_t face=TransportFaceIndex(dualTranslated.shape,axis,x,y,z);
					exactDualTranslation=exactDualTranslation&&
						dualTranslatedResult.auxiliaryFaceDensity[axis][face]==1.0f&&
						dualTranslatedResult.momentum[axis][face]==
							(face==expected?static_cast<float>(axis+1u):0.0f);
				}
	}
	Check(exactDualTranslation,
		"each periodic MAC dual tuple exactly translates by the (2,2,1) palindrome");
	FireProductionPeriodicDualMomentumRequest brokenDualSeam=dualTranslated;
	brokenDualSeam.beginningMomentum[1][TransportFaceIndex(brokenDualSeam.shape,1u,
		2u,brokenDualSeam.shape.ny,2u)]=1.0f;
	Check(!RemapFireProductionPeriodicDualMomentumCPU(brokenDualSeam,
		dualTranslatedResult,&error)&&dualTranslatedResult.executedSubmapCount==0u&&
		dualTranslatedResult.canonicalSeamCopyCount==0u&&
		dualTranslatedResult.momentum[0].empty()&&error.find("seam")!=std::string::npos,
		"dual periodic publication seams are validated before any component remap");
	FireProductionPeriodicDualMomentumRequest laterComponentFold=dualTranslated;
	for( unsigned int axis=0u;axis<3u;++axis )
		std::fill(laterComponentFold.frozenVelocityMPerS[axis].begin(),
			laterComponentFold.frozenVelocityMPerS[axis].end(),0.0f);
	for( std::size_t z=0u;z<laterComponentFold.shape.nz;++z )
		for( std::size_t y=0u;y<laterComponentFold.shape.ny;++y )
			laterComponentFold.frozenVelocityMPerS[0][TransportFaceIndex(
				laterComponentFold.shape,0u,2u,y,z)]=4.0f;
	for( unsigned int axis=0u;axis<3u;++axis ) {
		dualTranslatedResult.auxiliaryFaceDensity[axis].assign(1u,3.0f);
		dualTranslatedResult.momentum[axis].assign(1u,4.0f);
	}
	dualTranslatedResult.executedSubmapCount=9u;
	dualTranslatedResult.canonicalSeamCopyCount=9u;
	const bool laterFoldRejected=!RemapFireProductionPeriodicDualMomentumCPU(laterComponentFold,
		dualTranslatedResult,&error);
	if( !laterFoldRejected||error.find("folded")==std::string::npos )
		std::cerr << "Later dual fold detail: rejected=" << laterFoldRejected << " error=" <<
			error << " count=" << dualTranslatedResult.executedSubmapCount << '\n';
	Check(laterFoldRejected&&error.find("folded")!=std::string::npos&&
		dualTranslatedResult.executedSubmapCount==0u&&
		dualTranslatedResult.canonicalSeamCopyCount==0u&&
		dualTranslatedResult.auxiliaryFaceDensity[0].empty()&&
		dualTranslatedResult.auxiliaryFaceDensity[1].empty()&&
		dualTranslatedResult.auxiliaryFaceDensity[2].empty()&&
		dualTranslatedResult.momentum[0].empty()&&dualTranslatedResult.momentum[1].empty()&&
		dualTranslatedResult.momentum[2].empty(),
		"a later-component carrier fold cannot publish an earlier dual remap");
	FireProductionPeriodicDualMomentumRequest dualCombinedOver,dualCombinedUnder;
	dualCombinedOver.shape.nx=22u;dualCombinedOver.shape.ny=294u;
	dualCombinedOver.shape.nz=1024u;dualCombinedOver.shape.cellWidthM=1.0f;
	dualCombinedUnder=dualCombinedOver;dualCombinedUnder.shape.nz=1023u;
	Check(!RemapFireProductionPeriodicDualMomentumCPU(dualCombinedOver,
		dualTranslatedResult,&error)&&error.find("combined working set")!=std::string::npos&&
		!RemapFireProductionPeriodicDualMomentumCPU(dualCombinedUnder,
			dualTranslatedResult,&error)&&error.find("combined working set")==std::string::npos,
		"dual momentum admission counts caller and atomic result faces around nested remap peak");
	FireProductionPeriodicDualMomentumRequest dualVariable;
	dualVariable.shape.nx=5u;dualVariable.shape.ny=6u;dualVariable.shape.nz=7u;
	dualVariable.shape.cellWidthM=1.0f;dualVariable.timeStepS=0.35f;
	for( unsigned int axis=0u;axis<3u;++axis ) {
		const std::size_t faces=FireProductionProjectionFaceCount(dualVariable.shape,axis);
		dualVariable.beginningFaceDensity[axis].resize(faces);
		dualVariable.beginningMomentum[axis].resize(faces);
		dualVariable.frozenVelocityMPerS[axis].resize(faces);
		const std::size_t xEnd=dualVariable.shape.nx+(axis==0u?1u:0u);
		const std::size_t yEnd=dualVariable.shape.ny+(axis==1u?1u:0u);
		const std::size_t zEnd=dualVariable.shape.nz+(axis==2u?1u:0u);
		for( std::size_t z=0u;z<zEnd;++z ) for( std::size_t y=0u;y<yEnd;++y )
			for( std::size_t x=0u;x<xEnd;++x ) {
				const std::size_t cx=axis==0u&&x==dualVariable.shape.nx?0u:x;
				const std::size_t cy=axis==1u&&y==dualVariable.shape.ny?0u:y;
				const std::size_t cz=axis==2u&&z==dualVariable.shape.nz?0u:z;
				const std::size_t face=TransportFaceIndex(dualVariable.shape,axis,x,y,z);
				dualVariable.beginningFaceDensity[axis][face]=1.0f+
					0.03f*static_cast<float>((cx+3u*cy+5u*cz+axis)%9u);
				dualVariable.beginningMomentum[axis][face]=0.2f+
					0.04f*static_cast<float>((5u*cx+2u*cy+cz+2u*axis)%11u);
				dualVariable.frozenVelocityMPerS[axis][face]=0.08f+
					0.01f*static_cast<float>((3u*cx+5u*cy+4u*cz+axis)%7u);
			}
	}
	FireProductionPeriodicDualMomentumResult dualVariableResult;
	bool dualVariableMatches=RemapFireProductionPeriodicDualMomentumCPU(
		dualVariable,dualVariableResult,&error)&&dualVariableResult.executedSubmapCount==15u;
	bool everyOneSidedCarrierDiffers=true,everySplitLimiterDiffers=true;
	for( unsigned int component=0u;component<3u;++component ) {
		std::vector<float> expectedDensity,expectedMomentum,wrongDensity,wrongMomentum;
		dualVariableMatches=dualVariableMatches&&IndependentPeriodicDualComponent(
			dualVariable,component,-1,false,false,expectedDensity,expectedMomentum,error)&&
			dualVariableResult.auxiliaryFaceDensity[component]==expectedDensity&&
			dualVariableResult.momentum[component]==expectedMomentum;
		for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
			for( unsigned int side=0u;side<2u;++side ) {
				const bool wrongOK=IndependentPeriodicDualComponent(dualVariable,component,
					static_cast<int>(sweepAxis),side==0u,false,
					wrongDensity,wrongMomentum,error);
				const bool differs=wrongOK&&
					(wrongDensity!=expectedDensity||wrongMomentum!=expectedMomentum);
				if( !differs ) std::cerr << "Insensitive dual carrier mutant c=" << component <<
					" a=" << sweepAxis << " side=" << side << '\n';
				everyOneSidedCarrierDiffers=everyOneSidedCarrierDiffers&&differs;
			}
		const bool splitOK=IndependentPeriodicDualComponent(dualVariable,component,
			-1,false,true,wrongDensity,wrongMomentum,error);
		const bool splitDiffers=splitOK&&
			(wrongDensity!=expectedDensity||wrongMomentum!=expectedMomentum);
		if( !splitDiffers ) std::cerr << "Insensitive split dual limiter c=" << component << '\n';
		everySplitLimiterDiffers=everySplitLimiterDiffers&&splitDiffers;
	}
	Check(dualVariableMatches&&everyOneSidedCarrierDiffers&&everySplitLimiterDiffers,
		"periodic dual momentum binds all nine cross-carrier averages and common tuple bytes");
	FireProductionDualMomentumResult dualVariableGeneralResult;
	Check(RemapFireProductionDualMomentumCPU(dualVariable,dualVariableGeneralResult,&error)&&
		dualVariableGeneralResult.executedSubmapCount==15u&&
		dualVariableGeneralResult.canonicalSeamCopyCount==
			dualVariableResult.canonicalSeamCopyCount&&
		dualVariableGeneralResult.auxiliaryFaceDensity==
			dualVariableResult.auxiliaryFaceDensity&&
		dualVariableGeneralResult.momentum==dualVariableResult.momentum,
		"six-side dual oracle preserves the settled periodic bytes exactly");
	FireProductionDualMomentumRequest mixedDual=dualVariable;
	mixedDual.ambientDensityKGPerM3=0.8f;
	mixedDual.boundary={FireProductionProjectionWall,FireProductionProjectionPressureOpen,
		FireProductionProjectionPressureOpen,FireProductionProjectionWall,
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen};
	FireProductionDualMomentumResult mixedDualResult;
	const bool mixedDualRemapped=RemapFireProductionDualMomentumCPU(
		mixedDual,mixedDualResult,&error);
	bool mixedDualMatchesIndependent=mixedDualRemapped;
	for( unsigned int component=0u;component<3u;++component ) {
		std::vector<float> expectedDensity,expectedMomentum;
		mixedDualMatchesIndependent=mixedDualMatchesIndependent&&
			IndependentDualComponent(mixedDual,component,expectedDensity,
				expectedMomentum,error)&&
			mixedDualResult.auxiliaryFaceDensity[component]==expectedDensity&&
			mixedDualResult.momentum[component]==expectedMomentum;
	}
	bool mixedDualWallPlanesZero=mixedDualRemapped&&
		mixedDualResult.executedSubmapCount==15u&&
		mixedDualResult.canonicalSeamCopyCount==0u;
	for( std::size_t z=0u;z<mixedDual.shape.nz;++z )
		for( std::size_t y=0u;y<mixedDual.shape.ny;++y )
			mixedDualWallPlanesZero=mixedDualWallPlanesZero&&
				mixedDualResult.momentum[0][TransportFaceIndex(mixedDual.shape,0u,0u,y,z)]==0.0f;
	for( std::size_t z=0u;z<mixedDual.shape.nz;++z )
		for( std::size_t x=0u;x<mixedDual.shape.nx;++x )
			mixedDualWallPlanesZero=mixedDualWallPlanesZero&&
				mixedDualResult.momentum[1][TransportFaceIndex(mixedDual.shape,1u,x,
					mixedDual.shape.ny,z)]==0.0f;
	Check(mixedDualMatchesIndependent&&mixedDualWallPlanesZero&&
		mixedDualResult.momentum!=mixedDual.beginningMomentum,
		"mixed dual momentum matches an independent corner oracle, prescribes normal walls, "
		"and advances ordinary open endpoints");
	bool dualBoundaryMatrixMatches=true,dualBoundaryMatrixChanges=true;
	bool dualBoundaryRoleVisited[3][3][2][3]={};
	std::vector<float> positiveSignMomentum[3][2][3];
	for( unsigned int component=0u;component<3u;++component )
		for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
			for( unsigned int side=0u;side<2u;++side )
				dualBoundaryRoleVisited[component][sweepAxis][side][0]=true;
	for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
		for( unsigned int openSide=0u;openSide<2u;++openSide )
			for( unsigned int sign=0u;sign<2u;++sign ) {
				FireProductionDualMomentumRequest orientation=dualVariable;
				orientation.ambientDensityKGPerM3=0.73f+
					0.04f*static_cast<float>(sweepAxis);
				orientation.boundary[2u*sweepAxis]=openSide==0u?
					FireProductionProjectionPressureOpen:FireProductionProjectionWall;
				orientation.boundary[2u*sweepAxis+1u]=openSide==1u?
					FireProductionProjectionPressureOpen:FireProductionProjectionWall;
				if( sign==1u ) for( float& velocity :
					orientation.frozenVelocityMPerS[sweepAxis] ) velocity=-velocity;
				FireProductionDualMomentumResult observed;
				const bool observedOK=RemapFireProductionDualMomentumCPU(
					orientation,observed,&error);
				bool thisMatches=observedOK&&observed.executedSubmapCount==15u;
				bool thisChanges=true;
				for( unsigned int component=0u;component<3u;++component ) {
					std::vector<float> expectedDensity,expectedMomentum;
					thisMatches=thisMatches&&IndependentDualComponent(orientation,component,
						expectedDensity,expectedMomentum,error)&&
						observed.auxiliaryFaceDensity[component]==expectedDensity&&
						observed.momentum[component]==expectedMomentum;
					thisChanges=thisChanges&&
						observed.momentum[component]!=orientation.beginningMomentum[component];
					if( sign==0u ) positiveSignMomentum[sweepAxis][openSide][component]=
						observed.momentum[component];
					else thisChanges=thisChanges&&observed.momentum[component]!=
						positiveSignMomentum[sweepAxis][openSide][component];
					for( unsigned int axis=0u;axis<3u;++axis )
						for( unsigned int side=0u;side<2u;++side ) {
							const FireProductionProjectionBoundary role=
								orientation.boundary[2u*axis+side];
							const unsigned int roleIndex=role==FireProductionProjectionPeriodic?
								0u:(role==FireProductionProjectionWall?1u:2u);
							dualBoundaryRoleVisited[component][axis][side][roleIndex]=true;
						}
				}
				if( !thisMatches ) std::cerr << "Dual boundary mismatch a=" << sweepAxis <<
					" side=" << openSide << " sign=" << sign << " error=" << error << '\n';
				dualBoundaryMatrixMatches=dualBoundaryMatrixMatches&&thisMatches;
				dualBoundaryMatrixChanges=dualBoundaryMatrixChanges&&thisChanges;
			}
	bool everyDualBoundaryRoleVisited=true;
	for( unsigned int component=0u;component<3u;++component )
		for( unsigned int sweepAxis=0u;sweepAxis<3u;++sweepAxis )
			for( unsigned int side=0u;side<2u;++side )
				for( unsigned int role=0u;role<3u;++role )
					everyDualBoundaryRoleVisited=everyDualBoundaryRoleVisited&&
						dualBoundaryRoleVisited[component][sweepAxis][side][role];
	Check(dualBoundaryMatrixMatches&&dualBoundaryMatrixChanges&&
		everyDualBoundaryRoleVisited,
		"independent dual oracle covers every component, sweep axis, wall/open side, "
		"periodic role, and both pressure-open flow signs with nonzero deltas");
	auto seedDualResult=[](FireProductionDualMomentumResult& seeded) {
		for( unsigned int component=0u;component<3u;++component ) {
			seeded.auxiliaryFaceDensity[component].assign(1u,3.0f);
			seeded.momentum[component].assign(1u,4.0f);
		}
		seeded.executedSubmapCount=9u;seeded.canonicalSeamCopyCount=11u;
	};
	auto dualResultIsDefault=[](const FireProductionDualMomentumResult& rejected) {
		return rejected.auxiliaryFaceDensity[0].empty()&&
			rejected.auxiliaryFaceDensity[1].empty()&&
			rejected.auxiliaryFaceDensity[2].empty()&&
			rejected.momentum[0].empty()&&rejected.momentum[1].empty()&&
			rejected.momentum[2].empty()&&rejected.executedSubmapCount==0u&&
			rejected.canonicalSeamCopyCount==0u;
	};
	FireProductionDualMomentumRequest malformedDual=mixedDual;
	malformedDual.boundary[0]=FireProductionProjectionPeriodic;
	malformedDual.boundary[1]=FireProductionProjectionWall;
	FireProductionDualMomentumResult rejectedDual;
	seedDualResult(rejectedDual);error.clear();
	Check(!RemapFireProductionDualMomentumCPU(malformedDual,rejectedDual,&error)&&
		dualResultIsDefault(rejectedDual)&&error.find("boundary pairing")!=std::string::npos,
		"general dual remap rejects an unpaired periodic side with no partial result");
	malformedDual=mixedDual;malformedDual.ambientDensityKGPerM3=0.0f;
	seedDualResult(rejectedDual);error.clear();
	Check(!RemapFireProductionDualMomentumCPU(malformedDual,rejectedDual,&error)&&
		dualResultIsDefault(rejectedDual)&&error.find("ambient density")!=std::string::npos,
		"general dual remap rejects invalid ambient density with no partial result");
	malformedDual=mixedDual;
	malformedDual.frozenVelocityMPerS[2][3u]=
		std::numeric_limits<float>::quiet_NaN();
	seedDualResult(rejectedDual);error.clear();
	Check(!RemapFireProductionDualMomentumCPU(malformedDual,rejectedDual,&error)&&
		dualResultIsDefault(rejectedDual)&&error.find("nonfinite")!=std::string::npos,
		"general dual remap rejects nonfinite carrier bytes with no partial result");
	FireProductionDualMomentumRequest lateDualArithmetic=dualVariable;
	std::fill(lateDualArithmetic.beginningFaceDensity[1].begin(),
		lateDualArithmetic.beginningFaceDensity[1].end(),
		std::numeric_limits<float>::max());
	std::fill(lateDualArithmetic.beginningMomentum[1].begin(),
		lateDualArithmetic.beginningMomentum[1].end(),
		std::numeric_limits<float>::max());
	seedDualResult(rejectedDual);error.clear();
	Check(!RemapFireProductionDualMomentumCPU(lateDualArithmetic,rejectedDual,&error)&&
		dualResultIsDefault(rejectedDual)&&!error.empty(),
		"a later-component derived-arithmetic failure cannot publish an earlier dual remap");

	FireProductionCellPalindromeRequest donor=translated;
	donor.componentCount=1u;
	donor.conservativeValues.assign(donor.shape.CellCount(),0.0f);
	donor.ambientValues.assign(1u,0.0f);
	donor.conservativeValues[translatedBeginning]=1.0f;
	std::fill(donor.frozenVelocityMPerS[0].begin(),donor.frozenVelocityMPerS[0].end(),0.75f);
	std::fill(donor.frozenVelocityMPerS[1].begin(),donor.frozenVelocityMPerS[1].end(),0.75f);
	std::fill(donor.frozenVelocityMPerS[2].begin(),donor.frozenVelocityMPerS[2].end(),0.0f);
	FireProductionCellPalindromeResult donorResult;
	std::vector<float> donorIndependent;
	const bool donorReference=IndependentCellComposition(donor,canonicalAxes,
		canonicalFactors,5u,donorIndependent,error);
	Check(RemapFireProductionCellPalindromeCPU(donor,donorResult,&error)&&
		donorReference&&donorResult.conservativeValues==donorIndependent&&
		donorResult.executedSubmapCount==5u&&
		*std::min_element(donorResult.conservativeValues.begin(),
			donorResult.conservativeValues.end())>=0.0f&&
		std::fabs(std::accumulate(donorResult.conservativeValues.begin(),
			donorResult.conservativeValues.end(),0.0)-1.0)<=2.0e-6,
		"palindromic composition survives combined 0.75+0.75 donor outflow without a clamp");
	FireProductionCellPalindromeRequest orderSensitive=donor;
	orderSensitive.timeStepS=0.4f;
	for( std::size_t z=0;z<orderSensitive.shape.nz;++z )
		for( std::size_t y=0;y<orderSensitive.shape.ny;++y )
			for( std::size_t x=0;x<orderSensitive.shape.nx;++x )
				orderSensitive.conservativeValues[TransportCellIndex(orderSensitive.shape,x,y,z)]=
					0.2f+0.07f*static_cast<float>((x+3u*y+5u*z)%11u);
	for( std::size_t z=0;z<orderSensitive.shape.nz;++z )
		for( std::size_t y=0;y<orderSensitive.shape.ny;++y )
			for( std::size_t x=0;x<=orderSensitive.shape.nx;++x )
				orderSensitive.frozenVelocityMPerS[0][(z*orderSensitive.shape.ny+y)*
					(orderSensitive.shape.nx+1u)+x]=0.3f+0.02f*static_cast<float>(y);
	for( std::size_t z=0;z<orderSensitive.shape.nz;++z )
		for( std::size_t y=0;y<=orderSensitive.shape.ny;++y )
			for( std::size_t x=0;x<orderSensitive.shape.nx;++x )
				orderSensitive.frozenVelocityMPerS[1][(z*(orderSensitive.shape.ny+1u)+y)*
					orderSensitive.shape.nx+x]=-0.2f+0.015f*static_cast<float>(x);
	for( std::size_t z=0;z<=orderSensitive.shape.nz;++z )
		for( std::size_t y=0;y<orderSensitive.shape.ny;++y )
			for( std::size_t x=0;x<orderSensitive.shape.nx;++x )
				orderSensitive.frozenVelocityMPerS[2][(z*orderSensitive.shape.ny+y)*
					orderSensitive.shape.nx+x]=0.1f+
					0.01f*static_cast<float>((x+y)%3u);
	FireProductionCellPalindromeResult orderSensitiveResult;
	std::vector<float> orderIndependent,axisReversed,lieSplit;
	const unsigned int axisReversedAxes[]={2u,1u,0u,1u,2u};
	const unsigned int lieAxes[]={0u,1u,2u};
	const float lieFactors[]={1.0f,1.0f,1.0f};
	const bool orderReference=IndependentCellComposition(orderSensitive,canonicalAxes,
		canonicalFactors,5u,orderIndependent,error);
	const bool axisReversedReference=IndependentCellComposition(orderSensitive,
		axisReversedAxes,canonicalFactors,5u,axisReversed,error);
	const bool lieReference=IndependentCellComposition(orderSensitive,lieAxes,
		lieFactors,3u,lieSplit,error);
	Check(RemapFireProductionCellPalindromeCPU(orderSensitive,orderSensitiveResult,&error)&&
		orderReference&&axisReversedReference&&lieReference&&
		orderSensitiveResult.conservativeValues==orderIndependent&&
		orderIndependent!=axisReversed&&orderIndependent!=lieSplit&&
		FloatBytesSHA256(orderSensitiveResult.conservativeValues)==
			"e2d29d9429e1a0aee15a6969f90077846a0d98c09ac140f89ca572b0c7b4ac38",
		"asymmetric variable-carrier fixture binds the canonical x-y-z-y-x palindrome bytes");

	FireProductionCellPalindromeRequest palindromeConstant=donor;
	palindromeConstant.conservativeValues.assign(palindromeConstant.shape.CellCount(),0.1f);
	FireProductionCellPalindromeResult palindromeConstantResult;
	Check(RemapFireProductionCellPalindromeCPU(palindromeConstant,palindromeConstantResult,&error)&&
		palindromeConstantResult.conservativeValues.size()==
			palindromeConstant.conservativeValues.size()&&
		std::all_of(palindromeConstantResult.conservativeValues.begin(),
			palindromeConstantResult.conservativeValues.end(),
			[](float value){return value==0.1f;}),
		"production palindrome preserves an ordinary fp32 constant exactly through five passes");

	std::vector<FireProductionCellPalindromeRequest> mixedPalindromeRequests;
	std::vector<std::vector<float> > mixedPalindromeExpected;
	for( unsigned int axis=0u;axis<3u;++axis ) for( unsigned int role=0u;role<4u;++role ) {
		FireProductionCellPalindromeRequest mixed;
		mixed.shape.nx=5u;mixed.shape.ny=6u;mixed.shape.nz=7u;
		mixed.shape.cellWidthM=1.0f;mixed.componentCount=2u;mixed.timeStepS=0.4f;
		mixed.boundary.fill(FireProductionProjectionWall);
		const bool openAtLower=role>=2u;
		mixed.boundary[2u*axis]=openAtLower?FireProductionProjectionPressureOpen:
			FireProductionProjectionWall;
		mixed.boundary[2u*axis+1u]=openAtLower?FireProductionProjectionWall:
			FireProductionProjectionPressureOpen;
		mixed.conservativeValues.resize(2u*mixed.shape.CellCount());
		for( std::size_t cell=0;cell<mixed.shape.CellCount();++cell ) {
			mixed.conservativeValues[cell]=1.0f+0.01f*static_cast<float>(cell%13u);
			mixed.conservativeValues[mixed.shape.CellCount()+cell]=
				2.0f+0.02f*static_cast<float>(cell%7u);
		}
		mixed.ambientValues={5.0f,9.0f};
		for( unsigned int velocityAxis=0u;velocityAxis<3u;++velocityAxis )
			mixed.frozenVelocityMPerS[velocityAxis].assign(
				FireProductionProjectionFaceCount(mixed.shape,velocityAxis),0.0f);
		const bool inflow=(role%2u)==1u;
		const float boundaryVelocity=openAtLower?(inflow?0.6f:-0.6f):
			(inflow?-0.6f:0.6f);
		const std::size_t extent=axis==0u?mixed.shape.nx:
			(axis==1u?mixed.shape.ny:mixed.shape.nz);
		const std::size_t lines=axis==0u?mixed.shape.ny*mixed.shape.nz:
			(axis==1u?mixed.shape.nx*mixed.shape.nz:mixed.shape.nx*mixed.shape.ny);
		for( std::size_t line=0;line<lines;++line ) {
			const std::size_t faceCoordinate=openAtLower?0u:extent;
			const std::size_t transverse0=axis==0u?line%mixed.shape.ny:line%mixed.shape.nx;
			const std::size_t transverse1=axis==0u?line/mixed.shape.ny:line/mixed.shape.nx;
			const std::size_t x=axis==0u?faceCoordinate:transverse0;
			const std::size_t y=axis==1u?faceCoordinate:(axis==0u?transverse0:transverse1);
			const std::size_t z=axis==2u?faceCoordinate:transverse1;
			std::size_t face=0u;
			if( axis==0u ) face=(z*mixed.shape.ny+y)*(mixed.shape.nx+1u)+x;
			else if( axis==1u ) face=(z*(mixed.shape.ny+1u)+y)*mixed.shape.nx+x;
			else face=(z*mixed.shape.ny+y)*mixed.shape.nx+x;
			mixed.frozenVelocityMPerS[axis][face]=boundaryVelocity;
			const std::size_t wallCoordinate=openAtLower?extent:0u;
			const std::size_t wallX=axis==0u?wallCoordinate:transverse0;
			const std::size_t wallY=axis==1u?wallCoordinate:
				(axis==0u?transverse0:transverse1);
			const std::size_t wallZ=axis==2u?wallCoordinate:transverse1;
			if( axis==0u ) face=(wallZ*mixed.shape.ny+wallY)*(mixed.shape.nx+1u)+wallX;
			else if( axis==1u ) face=(wallZ*(mixed.shape.ny+1u)+wallY)*mixed.shape.nx+wallX;
			else face=(wallZ*mixed.shape.ny+wallY)*mixed.shape.nx+wallX;
			mixed.frozenVelocityMPerS[axis][face]=openAtLower?-0.35f:0.35f;
		}
		std::vector<float> mixedIndependent;
		FireProductionCellPalindromeResult mixedResult;
		Check(IndependentCellComposition(mixed,canonicalAxes,canonicalFactors,5u,
			mixedIndependent,error)&&
			RemapFireProductionCellPalindromeCPU(mixed,mixedResult,&error)&&
			mixedResult.conservativeValues==mixedIndependent&&
			mixedResult.conservativeValues!=mixed.conservativeValues,
			"non-cubic P3 packing binds every axis, side, and pressure-open flow sign");
		mixedPalindromeRequests.push_back(mixed);
		mixedPalindromeExpected.push_back(mixedIndependent);
	}
	FireProductionCellPalindromeRequest invalidPalindrome=translated;
	invalidPalindrome.boundary[4]=FireProductionProjectionPeriodic;
	invalidPalindrome.boundary[5]=FireProductionProjectionWall;
	Check(!ValidateFireProductionCellPalindromeRequest(invalidPalindrome,&error)&&
		error.find("boundary pairing")!=std::string::npos,
		"P3 palindrome rejects an unpaired periodic side before dispatch");
	FireProductionCellPalindromeRequest lateFold=translated;
	lateFold.frozenVelocityMPerS[2].assign(
		FireProductionProjectionFaceCount(lateFold.shape,2u),0.0f);
	lateFold.frozenVelocityMPerS[2][TransportCellIndex(lateFold.shape,2u,2u,3u)]=-1.0f;
	lateFold.frozenVelocityMPerS[2][TransportCellIndex(lateFold.shape,2u,2u,4u)]=1.0f;
	FireProductionCellPalindromeResult lateFoldResult;
	lateFoldResult.conservativeValues.assign(1u,8.0f);lateFoldResult.executedSubmapCount=9u;
	Check(!RemapFireProductionCellPalindromeCPU(lateFold,lateFoldResult,&error)&&
		lateFoldResult.conservativeValues.empty()&&lateFoldResult.executedSubmapCount==0u&&
		error.find("folded")!=std::string::npos,
		"a folded z map fails preflight without publishing partial x/y work");
	FireProductionProjectionShape tier10TransportShape;
	tier10TransportShape.nx=86u;tier10TransportShape.ny=86u;tier10TransportShape.nz=132u;
	tier10TransportShape.cellWidthM=0.3f/128.0f;
	std::uint64_t tier10TransportBytes=0u;
	FireProductionProjectionShape transportUnder,transportOver;
	transportUnder.nx=16u;transportUnder.ny=209u;transportUnder.nz=1024u;
	transportOver.nx=28u;transportOver.ny=120u;transportOver.nz=1024u;
	transportUnder.cellWidthM=1.0f;transportOver.cellWidthM=1.0f;
	std::uint64_t transportUnderBytes=0u,transportOverBytes=0u;
	Check(FireProductionCellPalindromeWorkingSetBytes(tier10TransportShape,9u,
		tier10TransportBytes)&&tier10TransportBytes==UINT64_C(466364852)&&
		FireProductionCellPalindromeWorkingSetBytes(transportUnder,12u,
			transportUnderBytes)&&transportUnderBytes==UINT64_C(2147468400)&&
		FireProductionCellPalindromeWorkingSetBytes(transportOver,12u,
			transportOverBytes)&&transportOverBytes==UINT64_C(2147529904)&&
		transportUnderBytes<(UINT64_C(1)<<31u)&&transportOverBytes>(UINT64_C(1)<<31u),
		"palindrome working-set certificate includes outward-rounded Metal allocations at two GiB");
	FireProductionCellPalindromeRequest overAdmission,underAdmission;
	overAdmission.shape=transportOver;overAdmission.componentCount=12u;
	underAdmission.shape=transportUnder;underAdmission.componentCount=12u;
	Check(!ValidateFireProductionCellPalindromeRequest(overAdmission,&error)&&
		error.find("two GiB")!=std::string::npos&&
		!ValidateFireProductionCellPalindromeRequest(underAdmission,&error)&&
		error.find("two GiB")==std::string::npos,
		"the actual palindrome admission path rejects the over-cap shape before tuple allocation");

	FireProductionComputeCapability capability;
	Check(QueryFireProductionComputeCapability(capability),
		"production compute capability query completes structurally");
#if defined(__APPLE__)
	if( !capability.available )
		std::cerr << "Metal capability detail: " << capability.structuredError << '\n';
	Check(capability.available&&capability.identityKernelPassed&&capability.backend=="metal"&&
		!capability.deviceName.empty()&&!capability.deviceFamily.empty()&&
		capability.deviceFamily!="metal-family-unreported"&&
		capability.registryId!=0u&&capability.maximumThreadsPerThreadgroup>=8u&&
		capability.structuredError.empty(),
		"Metal production capability compiles, dispatches, and verifies fp32 bytes");
#if defined(__arm64__)
	Check(capability.unifiedMemory,"Apple-silicon Metal capability reports unified memory");
#endif
	std::vector<std::uint32_t> challenge(16u),returned;
	for( std::size_t i=0;i<challenge.size();++i )
		challenge[i]=static_cast<std::uint32_t>(package.TablePackageId()[i])*0x01010101u+
			static_cast<std::uint32_t>(i);
	Check(RunFireProductionComputeChallenge(challenge.data(),challenge.size(),returned,&error)&&
		returned.size()==challenge.size(),"Metal production challenge dispatch returns every word");
	for( std::size_t i=0;i<returned.size();++i )
		Check(returned[i]==(challenge[i]^(0x9e3779b9u+static_cast<std::uint32_t>(i)*0x85ebca6bu)),
			"Metal production challenge proves nonidentity device execution");
	FireProductionRemapResult constantGPU,constantGPURepeated,openGPU,openNegativeGPU,wallGPU,
		wallNegativeGPU,wallToOpenGPU,openToWallGPU,
		lineAmbientGPU,smoothGPU,affineGPU,latePrefixGPU,subUlpSweepGPU,blellochGPU;
	FireProductionCellPalindromeResult translatedGPU,donorGPU,orderGPU,orderGPURepeated,
		palindromeConstantGPU;
	const bool translatedMetal=RemapFireProductionCellPalindromeMetal(
		translated,translatedGPU,&error);
	if( !translatedMetal ) std::cerr << "Metal palindrome detail: " << error << '\n';
	const bool donorMetal=RemapFireProductionCellPalindromeMetal(donor,donorGPU,&error);
	const bool orderMetal=RemapFireProductionCellPalindromeMetal(orderSensitive,orderGPU,&error);
	const bool orderRepeated=orderMetal&&RemapFireProductionCellPalindromeMetal(
		orderSensitive,orderGPURepeated,&error);
	const bool palindromeConstantMetal=RemapFireProductionCellPalindromeMetal(
		palindromeConstant,palindromeConstantGPU,&error);
	Check(translatedMetal&&translatedGPU.executedSubmapCount==5u&&
		translatedGPU.conservativeValues==translatedResult.conservativeValues&&
		donorMetal&&SameFloatVectorsWithin(donorGPU.conservativeValues,
			donorResult.conservativeValues,3.0e-5f)&&orderMetal&&orderRepeated&&
		SameFloatVectorsWithin(orderGPU.conservativeValues,
			orderSensitiveResult.conservativeValues,3.0e-5f)&&
		orderGPU.conservativeValues==orderGPURepeated.conservativeValues&&
		orderGPU.executedSubmapCount==5u&&orderGPU.deviceElapsedMS>0.0&&
		orderGPU.privateResidentBufferCount==13u&&orderGPU.sharedBufferCount==16u&&
		orderGPU.commandCommitCount==1u&&orderGPU.interstageFullGridReadbackCount==0u&&
		orderGPU.actualTrackedWorkingSetBytes<=orderGPU.certifiedWorkingSetBytes&&
		palindromeConstantMetal&&palindromeConstantGPU.conservativeValues==
			palindromeConstantResult.conservativeValues,
		"one-command Metal palindrome is byte-identical to the independent five-pass oracle");
	bool everyMixedPalindromeMetal=true;
	for( std::size_t fixture=0;fixture<mixedPalindromeRequests.size();++fixture ) {
		FireProductionCellPalindromeResult mixedGPU;
		everyMixedPalindromeMetal=everyMixedPalindromeMetal&&
			RemapFireProductionCellPalindromeMetal(mixedPalindromeRequests[fixture],mixedGPU,&error)&&
			SameFloatVectorsWithin(mixedGPU.conservativeValues,
				mixedPalindromeExpected[fixture],3.0e-5f);
	}
	Check(everyMixedPalindromeMetal,
		"Metal palindrome matches all non-cubic axis/side/open-role oracle fixtures");
	FireProductionCellPalindromeRequest tier10Palindrome;
	tier10Palindrome.shape=tier10TransportShape;tier10Palindrome.componentCount=9u;
	tier10Palindrome.timeStepS=1.0f/480.0f;
	tier10Palindrome.boundary.fill(FireProductionProjectionPeriodic);
	tier10Palindrome.conservativeValues.resize(
		tier10Palindrome.componentCount*tier10Palindrome.shape.CellCount());
	for( std::size_t index=0;index<tier10Palindrome.conservativeValues.size();++index )
		tier10Palindrome.conservativeValues[index]=1.0f+
			static_cast<float>(index%97u)*(1.0f/256.0f);
	tier10Palindrome.ambientValues.assign(tier10Palindrome.componentCount,0.0f);
	for( unsigned int axis=0u;axis<3u;++axis )
		tier10Palindrome.frozenVelocityMPerS[axis].assign(
			FireProductionProjectionFaceCount(tier10Palindrome.shape,axis),0.2f);
	std::vector<double> palindromeDeviceMS,palindromeWallMS;
	FireProductionCellPalindromeResult tier10PalindromeResult;
	for( unsigned int trial=0u;trial<6u;++trial ) {
		const std::chrono::steady_clock::time_point beginning=std::chrono::steady_clock::now();
		const bool remapped=RemapFireProductionCellPalindromeMetal(
			tier10Palindrome,tier10PalindromeResult,&error);
		const double wallMS=std::chrono::duration<double,std::milli>(
			std::chrono::steady_clock::now()-beginning).count();
		Check(remapped&&tier10PalindromeResult.executedSubmapCount==5u&&
			tier10PalindromeResult.certifiedWorkingSetBytes==tier10TransportBytes&&
			tier10PalindromeResult.actualTrackedWorkingSetBytes<=tier10TransportBytes,
			"tier-10-shaped Metal palindrome timing trial remains structurally valid");
		if( trial>0u&&remapped ) {
			palindromeDeviceMS.push_back(tier10PalindromeResult.deviceElapsedMS);
			palindromeWallMS.push_back(wallMS);
		}
	}
	std::sort(palindromeDeviceMS.begin(),palindromeDeviceMS.end());
	std::sort(palindromeWallMS.begin(),palindromeWallMS.end());
	const double palindromeDeviceP95=palindromeDeviceMS.empty()?infinity:
		palindromeDeviceMS.back();
	const double palindromeWallP95=palindromeWallMS.empty()?infinity:palindromeWallMS.back();
	std::cout << "Production P3 cell palindrome device_p95_ms=" << palindromeDeviceP95 <<
		" wall_p95_ms=" << palindromeWallP95 << " certified_bytes=" <<
		tier10PalindromeResult.certifiedWorkingSetBytes << " actual_tracked_bytes=" <<
		tier10PalindromeResult.actualTrackedWorkingSetBytes << '\n';
	Check(palindromeDeviceMS.size()==5u&&palindromeWallMS.size()==5u&&
		palindromeDeviceP95>0.0&&palindromeDeviceP95<=45.0&&
		palindromeWallP95>0.0&&palindromeWallP95<=45.0,
		"tier-10-shaped one-command Metal palindrome meets the 45 ms remap allocation");
	const bool constantMetal=RemapFireProductionMetal(constant,constantGPU,&error);
	if( !constantMetal ) std::cerr << "Metal remap detail: " << error << '\n';
	const bool repeatedMetal=constantMetal&&RemapFireProductionMetal(constant,constantGPURepeated,&error);
	if( constantMetal&&repeatedMetal&& !SameRemapWithin(constantCPU,constantGPU,2.0e-5f) ) {
		for( std::size_t i=0;i<constantCPU.updatedValues.size();++i )
			if( constantCPU.updatedValues[i]!=constantGPU.updatedValues[i] ) {
				std::cerr << "Constant CPU/GPU state mismatch " << i << ": " << std::hexfloat <<
					constantCPU.updatedValues[i] << " vs " << constantGPU.updatedValues[i] <<
					std::defaultfloat << '\n';break;
			}
		for( std::size_t i=0;i<constantCPU.faceFluxes.size();++i )
			if( constantCPU.faceFluxes[i]!=constantGPU.faceFluxes[i] ) {
				std::cerr << "Constant CPU/GPU flux mismatch " << i << ": " << std::hexfloat <<
					constantCPU.faceFluxes[i] << " vs " << constantGPU.faceFluxes[i] <<
					std::defaultfloat << '\n';break;
			}
		for( std::size_t i=0;i<constantCPU.sharedLimiterAlpha.size();++i )
			if( !NearFloat(constantCPU.sharedLimiterAlpha[i],
				constantGPU.sharedLimiterAlpha[i],2.0e-5f) ) {
				std::cerr << "Constant CPU/GPU alpha mismatch " << i << ": " << std::hexfloat <<
					constantCPU.sharedLimiterAlpha[i] << " vs " <<
					constantGPU.sharedLimiterAlpha[i] << std::defaultfloat << '\n';break;
			}
	}
	Check(constantMetal&&repeatedMetal&&
		constantGPU.updatedValues==constantGPURepeated.updatedValues&&
		constantGPU.faceFluxes==constantGPURepeated.faceFluxes&&
		constantGPU.sharedLimiterAlpha==constantGPURepeated.sharedLimiterAlpha&&
		SameRemapWithin(constantCPU,constantGPU,2.0e-5f),
		"Metal remap is same-device byte deterministic and matches the fp32 oracle");
	const bool openMetal=RemapFireProductionMetal(open,openGPU,&error);
	if( !openMetal ) std::cerr << "Metal open-remap detail: " << error << '\n';
	const bool wallMetal=RemapFireProductionMetal(wall,wallGPU,&error);
	Check(openMetal&&SameRemapWithin(openCPU,openGPU,2.0e-5f)&&
		RemapFireProductionMetal(openNegative,openNegativeGPU,&error)&&
		SameRemapWithin(openNegativeCPU,openNegativeGPU,2.0e-5f)&&
		wallMetal&&SameRemapWithin(wallCPU,wallGPU,2.0e-5f)&&
		RemapFireProductionMetal(wallNegative,wallNegativeGPU,&error)&&
		SameRemapWithin(wallNegativeCPU,wallNegativeGPU,2.0e-5f)&&
		RemapFireProductionMetal(wallToOpen,wallToOpenGPU,&error)&&
		SameRemapWithin(wallToOpenCPU,wallToOpenGPU,2.0e-5f)&&
		RemapFireProductionMetal(openToWall,openToWallGPU,&error)&&
		SameRemapWithin(openToWallCPU,openToWallGPU,2.0e-5f)&&
		RemapFireProductionMetal(lineAmbient,lineAmbientGPU,&error)&&
		SameRemapWithin(lineAmbientCPU,lineAmbientGPU,2.0e-5f),
		"Metal symmetric and asymmetric pressure-open/wall fluxes match the fp32 oracle");
	const bool affineMetal=RemapFireProductionMetal(affine,affineGPU,&error);
	float maximumGPUAffineResidual=0.0f;
	if( affineMetal ) for( std::size_t cell=0;cell<affine.lineLength;++cell )
		maximumGPUAffineResidual=std::max(maximumGPUAffineResidual,std::fabs(
			affineGPU.updatedValues[RemapValueIndex(affine,2u,0u,cell)]-
			(affineGPU.updatedValues[RemapValueIndex(affine,0u,0u,cell)]+
			 affineGPU.updatedValues[RemapValueIndex(affine,1u,0u,cell)])));
	Check(affineMetal&&SameRemapWithin(affineCPU,affineGPU,3.0e-5f)&&
		maximumGPUAffineResidual<=3.0e-5f,
		"Metal applies one shared tuple limiter to a cancellation-sensitive affine row");
	Check(RemapFireProductionMetal(latePrefix,latePrefixGPU,&error)&&
		SameRemapWithin(latePrefixCPU,latePrefixGPU,3.0e-5f)&&
		RemapFireProductionMetal(subUlpSweep,subUlpSweepGPU,&error)&&
		subUlpSweepGPU.faceFluxes.front()>0.0f&&
		subUlpSweepGPU.faceFluxes.front()==subUlpSweepCPU.faceFluxes.front()&&
		SameRemapWithin(subUlpSweepCPU,subUlpSweepGPU,3.0e-5f)&&
		RemapFireProductionMetal(blelloch,blellochGPU,&error)&&
		blellochGPU.faceFluxes.front()==0.0f&&
		blellochGPU.faceFluxes.front()==blellochGPU.faceFluxes.back(),
		"Metal local integration and Blelloch cancellation topology match their oracle fixtures");
	FireProductionRemapRequest largeMetalRequest=PeriodicRequest(7u,1u,0x1.110bb6p+62f);
	largeMetalRequest.cellWidthM=1.0f;largeMetalRequest.timeStepS=0x1.110bb6p+62f;
	largeMetalRequest.values.assign(7u,0.1f);
	FireProductionRemapResult largeMetalCPU,largeMetalGPU;
	Check(RemapFireProductionCPU(largeMetalRequest,largeMetalCPU,&error)&&
		RemapFireProductionMetal(largeMetalRequest,largeMetalGPU,&error)&&
		SameRemapWithin(largeMetalCPU,largeMetalGPU,3.0e-5f),
		"Metal bounded quotient and remainder handle a large finite periodic Courant");
	FireProductionRemapResult finiteOverflowGPU;finiteOverflowGPU.updatedValues.push_back(4.0f);
	Check(!RemapFireProductionMetal(finiteOverflow,finiteOverflowGPU,&error)&&
		finiteOverflowGPU.updatedValues.empty()&&!error.empty(),
		"Metal rejects nonfinite derived output without publishing partial state");
	FireProductionRemapRequest smoothRequest=PeriodicRequest(64u,1u,12.8f);
	const double pi=std::acos(-1.0);
	for( std::size_t cell=0;cell<smoothRequest.lineLength;++cell ) {
		const double left=static_cast<double>(cell)/64.0;
		const double right=static_cast<double>(cell+1u)/64.0;
		smoothRequest.values[cell]=static_cast<float>(2.0+(std::cos(2.0*pi*left)-
			std::cos(2.0*pi*right))/(2.0*pi*(right-left)));
	}
	FireProductionRemapResult smoothGPURepeated;
	const bool smoothMetal=RemapFireProductionMetal(smoothRequest,smoothGPU,&error);
	const bool smoothMetalRepeated=smoothMetal&&
		RemapFireProductionMetal(smoothRequest,smoothGPURepeated,&error);
	Check(smoothMetal&&smoothMetalRepeated&&smoothGPU.updatedValues==smoothGPURepeated.updatedValues&&
		smoothGPU.faceFluxes==smoothGPURepeated.faceFluxes&&
		smoothGPU.sharedLimiterAlpha==smoothGPURepeated.sharedLimiterAlpha&&
		SameRemapWithin(smooth64Result,smoothGPU,3.0e-5f),
		"nontrivial Metal remap is byte deterministic and matches the fp32 oracle");

	FireProductionRemapRequest tier10;
	tier10.lineLength=129u;tier10.lineCount=7568u;tier10.componentCount=8u;
	tier10.cellWidthM=0.3f/128.0f;tier10.timeStepS=1.0f/480.0f;
	tier10.boundary=FireProductionRemapPeriodic;
	tier10.values.resize(tier10.componentCount*tier10.lineCount*tier10.lineLength);
	tier10.faceVelocityMPerS.resize(tier10.lineCount*(tier10.lineLength+1u));
	tier10.ambientValues.assign(tier10.componentCount,0.0f);
	for( std::size_t index=0;index<tier10.values.size();++index )
		tier10.values[index]=1.0f+static_cast<float>(index%97u)*(1.0f/256.0f);
	for( std::size_t index=0;index<tier10.faceVelocityMPerS.size();++index )
		tier10.faceVelocityMPerS[index]=0.2f+static_cast<float>(index%11u)*(1.0f/64.0f);
	for( std::size_t line=0;line<tier10.lineCount;++line )
		tier10.faceVelocityMPerS[line*(tier10.lineLength+1u)+tier10.lineLength]=
			tier10.faceVelocityMPerS[line*(tier10.lineLength+1u)];
	std::vector<double> elapsed,wallElapsed;
	FireProductionRemapResult tier10GPU;
	for( unsigned run=0;run<6u;++run ) {
		const std::chrono::steady_clock::time_point beginning=std::chrono::steady_clock::now();
		const bool remapped=RemapFireProductionMetal(tier10,tier10GPU,&error);
		const double wallMS=std::chrono::duration<double,std::milli>(
			std::chrono::steady_clock::now()-beginning).count();
		if( !remapped ) { std::cerr << "Metal tier-10 remap detail: " << error << '\n';break; }
		if( run>0u ) {elapsed.push_back(tier10GPU.deviceElapsedMS);wallElapsed.push_back(wallMS);}
	}
	std::sort(elapsed.begin(),elapsed.end());
	std::sort(wallElapsed.begin(),wallElapsed.end());
	const double p95=elapsed.empty()?std::numeric_limits<double>::infinity():elapsed.back();
	const double wallP95=wallElapsed.empty()?std::numeric_limits<double>::infinity():wallElapsed.back();
	std::cout << "Production P1 tier-10-shaped remap device_p95_ms=" << p95 <<
		" wall_p95_ms=" << wallP95 << '\n';
	Check(elapsed.size()==5u&&wallElapsed.size()==5u&&std::isfinite(p95)&&p95>0.0&&
		std::isfinite(wallP95)&&wallP95>0.0&&p95<=wallP95+1.0&&p95<=45.0&&
		wallP95<=45.0,
		"tier-10-shaped Metal remap meets the 45 ms p95 allocation");
	const std::string metalSource=ReadText("src/Library/Utilities/FireProductionComputeMac.mm");
	Check(metalSource.find("newLibraryWithSource")!=std::string::npos&&
		metalSource.find("dispatchThreads")!=std::string::npos&&
		metalSource.find("[command commit]")!=std::string::npos&&
		metalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		metalSource.find("[output contents]")!=std::string::npos,
		"Metal capability source gate binds compilation, dispatch, completion, and returned bytes");
	const std::string advectionMetalSource=ReadText(
		"src/Library/Utilities/FireProductionAdvectionMac.mm");
	const std::string transportSource=ReadText(
		"src/Library/Utilities/FireProductionTransport.cpp");
	const std::size_t crossCarrierBeginning=transportSource.find("float CrossCarrierAt(");
	const std::size_t dualAxisBeginning=transportSource.find("bool BuildDualAxisRequest(");
	const std::string crossCarrierBody=crossCarrierBeginning==std::string::npos||
		dualAxisBeginning==std::string::npos?std::string():
		transportSource.substr(crossCarrierBeginning,
			dualAxisBeginning-crossCarrierBeginning);
	const std::size_t standaloneMetalBeginning=advectionMetalSource.find(
		"bool RemapFireProductionMetal(");
	const std::size_t palindromeMetalBeginning=advectionMetalSource.find(
		"bool RemapFireProductionCellPalindromeMetal(");
	const std::string standaloneMetalBody=standaloneMetalBeginning==std::string::npos||
		palindromeMetalBeginning==std::string::npos?std::string():
		advectionMetalSource.substr(standaloneMetalBeginning,
			palindromeMetalBeginning-standaloneMetalBeginning);
	const std::string palindromeMetalBody=palindromeMetalBeginning==std::string::npos?
		std::string():advectionMetalSource.substr(palindromeMetalBeginning);
	const std::string makeRules=ReadText("build/make/rise/Makefile");
	const std::string xcodeProject=ReadText("build/XCode/rise/rise.xcodeproj/project.pbxproj");
	const std::string androidRules=ReadText("build/cmake/rise-android/CMakeLists.txt");
	const std::string visualStudioProject=ReadText("build/VS2022/Library/Library.vcxproj");
	Check(!standaloneMetalBody.empty()&&
		CountSubstring(standaloneMetalBody,"recordBuffer(")==11u&&
		standaloneMetalBody.find("recordBuffer(values)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(velocity)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(lowerAmbient)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(upperAmbient)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(left)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(right)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(alpha)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(prefix)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(flux)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(updated)")!=std::string::npos&&
		standaloneMetalBody.find("recordBuffer(parameterBuffer)")!=std::string::npos&&
		advectionMetalSource.find("MTLMathModeSafe")!=std::string::npos&&
		advectionMetalSource.find("MTLMathModeFast")==std::string::npos&&
		advectionMetalSource.find("fast::")==std::string::npos&&
		advectionMetalSource.find("atomic_")==std::string::npos&&
		advectionMetalSource.find("simd_")==std::string::npos&&
		advectionMetalSource.find("RemapFireProductionCPU")==std::string::npos&&
		advectionMetalSource.find("makePipeline(\"reconstruct\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"scan_lines\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"face_flux\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"update_cells\")")!=std::string::npos&&
		advectionMetalSource.find("[command commit]")!=std::string::npos&&
		advectionMetalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		advectionMetalSource.find("ReadTrackedMetalBuffer(updated)")!=std::string::npos&&
		advectionMetalSource.find("GPUStartTime")!=std::string::npos&&
		advectionMetalSource.find("GPUEndTime")!=std::string::npos&&
		advectionMetalSource.find("actualWorkingSetBytes>certifiedWorkingSetBytes")!=
			std::string::npos&&
		advectionMetalSource.find("[buffer allocatedSize]")!=std::string::npos&&
		advectionMetalSource.find("activeFaces=periodic?p.n:faces")!=std::string::npos&&
		advectionMetalSource.find("p.lowerBoundary==2u")!=std::string::npos&&
		advectionMetalSource.find("p.upperBoundary==2u")!=std::string::npos&&
		advectionMetalSource.find("flux[base+p.n]=flux[base]")!=std::string::npos&&
		makeRules.find("-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		CountSubstring(xcodeProject,
			"FireProductionAdvection.cpp in Sources */ = {isa = PBXBuildFile; fileRef = "
			"FA84000131FF000100000007 /* FireProductionAdvection.cpp */; settings = "
			"{COMPILER_FLAGS = \"-fno-fast-math -ffp-contract=off\"; }; }")==2u&&
		makeRules.find("Utilities/FireProductionTransport.o : "
			"$(PATHLIBRARY)Utilities/FireProductionTransport.cpp\n\t@echo \"Compiling "
			"(safe fp32): $<\"\n\t@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) "
			"-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		CountSubstring(xcodeProject,
			"FireProductionTransport.cpp in Sources */ = {isa = PBXBuildFile; fileRef = "
			"FC92000131FF000100000007 /* FireProductionTransport.cpp */; settings = "
			"{COMPILER_FLAGS = \"-fno-fast-math -ffp-contract=off\"; }; }")==2u&&
		androidRules.find("-fno-fast-math;-ffp-contract=off")!=std::string::npos&&
		androidRules.find("\"${RISE_LIB}/Utilities/FireProductionTransport.cpp\"\n"
			"    PROPERTIES COMPILE_OPTIONS \"-fno-fast-math;-ffp-contract=off\"")!=
			std::string::npos&&visualStudioProject.find(
			"<ClCompile Include=\"..\\..\\..\\src\\Library\\Utilities\\"
			"FireProductionTransport.cpp\">\n      <FloatingPointModel>Strict"
			"</FloatingPointModel>")!=std::string::npos,
		"production remap source binds safe math, four real kernels, device output, and strict CPU builds");
	Check(!palindromeMetalBody.empty()&&
		advectionMetalSource.find("makePipeline(\"gather_grid_values\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"scatter_grid_values\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"gather_grid_velocity\")")!=std::string::npos&&
		CountSubstring(palindromeMetalBody,"options:MTLResourceStorageModePrivate")==13u&&
		CountSubstring(palindromeMetalBody,"options:MTLResourceStorageModeShared")==8u&&
		palindromeMetalBody.find("recordBuffer(gridA,MTLStorageModePrivate")!=std::string::npos&&
		palindromeMetalBody.find("recordBuffer(outputStage,MTLStorageModeShared")!=std::string::npos&&
		palindromeMetalBody.find("context.gatherValues")!=std::string::npos&&
		palindromeMetalBody.find("context.gatherVelocity")!=std::string::npos&&
		palindromeMetalBody.find("context.scatterValues")!=std::string::npos&&
		CountSubstring(advectionMetalSource,"[queue commandBuffer]")==1u&&
		CountSubstring(advectionMetalSource,"[command commit]")==1u&&
		CountSubstring(advectionMetalSource,"[buffer contents]")==1u&&
		CountSubstring(advectionMetalSource,"TrackedMetalCommandBuffer(")==3u&&
		CountSubstring(advectionMetalSource,"CommitTrackedMetalCommand(")==3u&&
		CountSubstring(advectionMetalSource,"ReadTrackedMetalBuffer(")==5u&&
		CountSubstring(palindromeMetalBody,"TrackedMetalCommandBuffer(")==1u&&
		CountSubstring(palindromeMetalBody,"CommitTrackedMetalCommand(")==1u&&
		CountSubstring(palindromeMetalBody,"ReadTrackedMetalBuffer(")==1u&&
		palindromeMetalBody.find("RemapFireProductionMetal(request")==std::string::npos,
		"P3 palindrome measures one command and one scheduled host publication at the global seams");
	Check(CountSubstring(transportSource,"void PublishPeriodicDualSeam(")==1u&&
		CountSubstring(transportSource,"PublishPeriodicDualSeam(shape,component,")==2u&&
		CountSubstring(transportSource,
			"density[high]=density[low];momentum[high]=momentum[low];copyCount+=2u;")==1u,
		"both dual APIs share one canonical positive-seam byte-copy publication path");
	Check(!crossCarrierBody.empty()&&
		CountSubstring(crossCarrierBody,"sweepFace==0u")==1u&&
		CountSubstring(crossCarrierBody,"request.boundary[2u*sweepAxis]==")==1u&&
		CountSubstring(crossCarrierBody,
			"sweepFace==AxisCoordinateExtent(shape,sweepAxis)")==1u&&
		CountSubstring(crossCarrierBody,"request.boundary[2u*sweepAxis+1u]")==1u&&
		crossCarrierBody.find("return 0.0f;")<
			crossCarrierBody.find("float lower=0.0f,upper=0.0f;")&&
		crossCarrierBody.find("boundary==FireProductionProjectionWall?-upper:upper")!=
			std::string::npos&&
		crossCarrierBody.find("boundary==FireProductionProjectionWall?-lower:lower")!=
			std::string::npos&&
		transportSource.find("lineRequest.lowerBoundary=RemapBoundary("
			"request.boundary[2u*sweepAxis]);")!=std::string::npos&&
		transportSource.find("lineRequest.upperBoundary=RemapBoundary("
			"request.boundary[2u*sweepAxis+1u]);")!=std::string::npos&&
		transportSource.find("request.boundary[2u*component]=="
			"FireProductionProjectionWall?1u:0u")!=std::string::npos,
		"dual line builder binds sweep-side roles, wall-zero precedence, open nearest ghosts, "
		"and prescribed component-wall ownership");
#else
	Check(!capability.available&&!capability.identityKernelPassed&&capability.backend=="unavailable"&&
		!capability.structuredError.empty(),
		"non-Metal production capability reports honest unavailability");
	const std::uint32_t unsupportedInput[]={0x12345678u,0x9abcdef0u};
	std::vector<std::uint32_t> unsupportedOutput(2u,0xfeedfaceu);
	error.clear();
	Check(!RunFireProductionComputeChallenge(unsupportedInput,2u,unsupportedOutput,&error)&&
		unsupportedOutput.empty()&&!error.empty(),
		"non-Metal challenge fails explicitly without a silent CPU fallback");
	FireProductionRemapResult unsupportedRemap;unsupportedRemap.updatedValues.push_back(3.0f);
	error.clear();
	Check(!RemapFireProductionMetal(constant,unsupportedRemap,&error)&&
		unsupportedRemap.updatedValues.empty()&&!error.empty(),
		"non-Metal production remap fails explicitly without a silent CPU fallback");
	FireProductionCellPalindromeResult unsupportedPalindrome;
	unsupportedPalindrome.conservativeValues.push_back(3.0f);
	unsupportedPalindrome.executedSubmapCount=7u;
	unsupportedPalindrome.privateResidentBufferCount=9u;
	unsupportedPalindrome.sharedBufferCount=9u;
	unsupportedPalindrome.commandCommitCount=9u;
	unsupportedPalindrome.interstageFullGridReadbackCount=9u;
	unsupportedPalindrome.certifiedWorkingSetBytes=9u;
	unsupportedPalindrome.actualTrackedWorkingSetBytes=9u;
	unsupportedPalindrome.deviceElapsedMS=9.0;
	error.clear();
	Check(!RemapFireProductionCellPalindromeMetal(translated,unsupportedPalindrome,&error)&&
		unsupportedPalindrome.conservativeValues.empty()&&
		unsupportedPalindrome.executedSubmapCount==0u&&
		unsupportedPalindrome.privateResidentBufferCount==0u&&
		unsupportedPalindrome.sharedBufferCount==0u&&
		unsupportedPalindrome.commandCommitCount==0u&&
		unsupportedPalindrome.interstageFullGridReadbackCount==0u&&
		unsupportedPalindrome.certifiedWorkingSetBytes==0u&&
		unsupportedPalindrome.actualTrackedWorkingSetBytes==0u&&
		unsupportedPalindrome.deviceElapsedMS==0.0&&!error.empty(),
		"non-Metal production palindrome fails explicitly without a silent CPU fallback");
#endif
	FireProductionRemapResult allocationFailureRemap;
	allocationFailureRemap.updatedValues.push_back(1.0f);
	allocationFailureRemap.faceFluxes.push_back(2.0f);
	allocationFailureRemap.sharedLimiterAlpha.push_back(3.0f);
	allocationFailureRemap.deviceElapsedMS=4.0;
	error.clear();error.shrink_to_fit();denyTestAllocations=true;
	const bool allocationFailureRemapReturned=RemapFireProductionMetal(
		constant,allocationFailureRemap,&error);
	denyTestAllocations=false;
	Check(!allocationFailureRemapReturned&&allocationFailureRemap.updatedValues.empty()&&
		allocationFailureRemap.faceFluxes.empty()&&
		allocationFailureRemap.sharedLimiterAlpha.empty()&&
		allocationFailureRemap.deviceElapsedMS==0.0,
		"standalone Metal remap contains persistent allocation failure without partial output");

	if( failures==0 ) {
		std::cout << "FireProductionSolverTest passed: table_package_id="
			<< package.TablePackageId() << "\n";
		return 0;
	}
	std::cerr << failures << " FireProductionSolverTest failure(s)\n";
	return 1;
}
