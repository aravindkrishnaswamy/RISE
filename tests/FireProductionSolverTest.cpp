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
#include "../src/Library/Utilities/FireProductionForce.h"
#include "../src/Library/Utilities/FireSimulationRecords.h"

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

	bool FrozenForceResultEmpty( const RISE::FireProductionFrozenForceResult& result )
	{
		if( !result.eddyKinematicViscosityM2PerS.empty()||
			!result.effectiveDynamicViscosityPaS.empty() ) return false;
		for( unsigned int axis=0u;axis<3u;++axis )
			if( !result.beginningViscousMomentumRateKGPerM2S2[axis].empty()||
				!result.gravityMomentumIncrementKGPerM2S[axis].empty() ) return false;
		return true;
	}

	bool FrozenForceAdvanceResultEmpty(
		const RISE::FireProductionFrozenForceAdvanceResult& result )
	{
		if( !FrozenForceResultEmpty(result.frozenFields)||
			result.schedule.substepCount!=0u||result.schedule.substepTimeS!=0.0f||
			result.schedule.outwardWork!=0.0||result.schedule.representedProductUpper!=0.0||
			result.intermediateMomentumDigestCount!=0u||
			result.executedViscousSubstepCount!=0u ) return false;
		for( const std::uint64_t digest : result.intermediateMomentumByteDigests )
			if( digest!=0u ) return false;
		for( unsigned int axis=0u;axis<3u;++axis )
			if( !result.momentumKGPerM2S[axis].empty() ) return false;
		return true;
	}

	void PublishTestForceBoundaries(
		const RISE::FireProductionFrozenForceRequest& request,
		std::array<std::vector<float>,3>& momentum )
	{
		for( unsigned int axis=0u;axis<3u;++axis ) {
			const std::size_t extent=axis==0u?request.shape.nx:
				(axis==1u?request.shape.ny:request.shape.nz);
			const unsigned int firstAxis=(axis+1u)%3u,secondAxis=(axis+2u)%3u;
			const std::size_t firstExtent=firstAxis==0u?request.shape.nx:
				(firstAxis==1u?request.shape.ny:request.shape.nz);
			const std::size_t secondExtent=secondAxis==0u?request.shape.nx:
				(secondAxis==1u?request.shape.ny:request.shape.nz);
			for( std::size_t second=0u;second<secondExtent;++second )
				for( std::size_t first=0u;first<firstExtent;++first ) {
					std::size_t lowXYZ[]={0u,0u,0u};
					lowXYZ[firstAxis]=first;lowXYZ[secondAxis]=second;
					std::size_t highXYZ[]={lowXYZ[0],lowXYZ[1],lowXYZ[2]};
					highXYZ[axis]=extent;
					const std::size_t low=TransportFaceIndex(request.shape,axis,
						lowXYZ[0],lowXYZ[1],lowXYZ[2]);
					const std::size_t high=TransportFaceIndex(request.shape,axis,
						highXYZ[0],highXYZ[1],highXYZ[2]);
					if( request.boundary[2u*axis]==RISE::FireProductionProjectionPeriodic )
						momentum[axis][high]=momentum[axis][low];
					if( request.boundary[2u*axis]==RISE::FireProductionProjectionWall )
						momentum[axis][low]=0.0f;
					if( request.boundary[2u*axis+1u]==RISE::FireProductionProjectionWall )
						momentum[axis][high]=0.0f;
				}
		}
	}

	RISE::FireProductionFrozenForceRequest ComposedForceCharacterizationCase(
		int boundaryClass,int phase,float vremanCoefficient )
	{
		RISE::FireProductionFrozenForceRequest request;
		request.shape.nx=5u;request.shape.ny=6u;request.shape.nz=7u;
		request.shape.cellWidthM=0.1f;request.timeStepS=1.0e-6f;
		request.ambientDensityKGPerM3=1.1f;request.vremanCoefficient=vremanCoefficient;
		request.gravityMPerS2={1.3f,-9.81f,0.7f};
		if( boundaryClass==0 ) request.boundary.fill(RISE::FireProductionProjectionPeriodic);
		else if( boundaryClass==1 )
			request.boundary.fill(RISE::FireProductionProjectionPressureOpen);
		else {
			const unsigned int mask=static_cast<unsigned int>(boundaryClass-2);
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const bool lowerWall=(mask&(1u<<axis))!=0u;
				request.boundary[2u*axis]=lowerWall?RISE::FireProductionProjectionWall:
					RISE::FireProductionProjectionPressureOpen;
				request.boundary[2u*axis+1u]=lowerWall?
					RISE::FireProductionProjectionPressureOpen:RISE::FireProductionProjectionWall;
			}
		}
		const std::size_t cells=request.shape.CellCount();
		request.cellGasDensityKGPerM3.resize(cells);
		request.molecularKinematicViscosityM2PerS.resize(cells);
		for( std::size_t cell=0u;cell<cells;++cell ) {
			request.cellGasDensityKGPerM3[cell]=0.7f+0.13f*static_cast<float>(
				(cell+static_cast<std::size_t>(phase))%9u);
			request.molecularKinematicViscosityM2PerS[cell]=0.002f+0.001f*
				static_cast<float>((cell+2u*static_cast<std::size_t>(phase))%11u);
		}
		for( unsigned int axis=0u;axis<3u;++axis ) {
			const std::size_t faces=RISE::FireProductionProjectionFaceCount(request.shape,axis);
			request.faceDensityKGPerM3[axis].resize(faces);
			request.beginningMomentumKGPerM2S[axis].resize(faces);
			for( std::size_t face=0u;face<faces;++face ) {
				request.faceDensityKGPerM3[axis][face]=0.6f+0.11f*static_cast<float>(
					(face+3u*axis+5u*static_cast<unsigned int>(phase))%13u);
				request.beginningMomentumKGPerM2S[axis][face]=static_cast<float>(
					static_cast<int>((17u*face+5u*axis+7u*static_cast<unsigned int>(phase))%37u)-18)*0.071f;
			}
		}
		if( boundaryClass==0 ) PublishTestForceBoundaries(request,
			request.beginningMomentumKGPerM2S);
		if( boundaryClass==0 ) {
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t extent=axis==0u?request.shape.nx:
					(axis==1u?request.shape.ny:request.shape.nz);
				const std::size_t plane=request.faceDensityKGPerM3[axis].size()/(extent+1u);
				for( std::size_t line=0u;line<plane;++line ) {
					const std::size_t low=axis==0u?line*(extent+1u):
						(axis==1u?(line/request.shape.nx)*(extent+1u)*request.shape.nx+
						line%request.shape.nx:line);
					const std::size_t high=axis==0u?low+extent:
						(axis==1u?low+extent*request.shape.nx:
						low+extent*request.shape.nx*request.shape.ny);
					request.faceDensityKGPerM3[axis][high]=request.faceDensityKGPerM3[axis][low];
				}
			}
		}
		return request;
	}

	std::uint64_t IndependentMomentumByteDigest(
		const std::array<std::vector<float>,3>& momentum )
	{
		std::uint64_t digest=UINT64_C(14695981039346656037);
		for( unsigned int axis=0u;axis<3u;++axis ) for( const float value : momentum[axis] ) {
			std::uint32_t bits=0u;
			std::memcpy(&bits,&value,sizeof(bits));
			for( unsigned int byte=0u;byte<4u;++byte ) {
				digest^=static_cast<unsigned char>(bits>>(8u*byte));
				digest*=UINT64_C(1099511628211);
			}
		}
		return digest;
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

	std::uint64_t FloatULPDistance( float first, float second )
	{
		if( !std::isfinite(first)||!std::isfinite(second) )
			return std::numeric_limits<std::uint64_t>::max();
		std::uint32_t firstBits=0u,secondBits=0u;
		std::memcpy(&firstBits,&first,sizeof(first));
		std::memcpy(&secondBits,&second,sizeof(second));
		const std::uint32_t firstOrdered=(firstBits&UINT32_C(0x80000000))?
			~firstBits:(firstBits|UINT32_C(0x80000000));
		const std::uint32_t secondOrdered=(secondBits&UINT32_C(0x80000000))?
			~secondBits:(secondBits|UINT32_C(0x80000000));
		return firstOrdered>secondOrdered?
			static_cast<std::uint64_t>(firstOrdered)-secondOrdered:
			static_cast<std::uint64_t>(secondOrdered)-firstOrdered;
	}

	bool SameFloatBytes( float first, float second )
	{
		return std::memcmp(&first,&second,sizeof(float))==0;
	}

	bool SameFloatVectorBytes( const std::vector<float>& first,
		const std::vector<float>& second )
	{
		return first.size()==second.size()&&(first.empty()||std::memcmp(
			first.data(),second.data(),first.size()*sizeof(float))==0);
	}

	bool SameFloatVectorsWithinULP( const std::vector<float>& first,
		const std::vector<float>& second, std::uint64_t maximumULPs )
	{
		return first.size()==second.size()&&std::equal(first.begin(),first.end(),second.begin(),
			[maximumULPs](float a,float b){return FloatULPDistance(a,b)<=maximumULPs;});
	}

	bool SameFrozenForceWithinULP(
		const RISE::FireProductionFrozenForceResult& cpu,
		const RISE::FireProductionFrozenForceResult& gpu,
		std::uint64_t maximumULPs )
	{
		if( !SameFloatVectorsWithinULP(cpu.eddyKinematicViscosityM2PerS,
			gpu.eddyKinematicViscosityM2PerS,maximumULPs)||
			!SameFloatVectorsWithinULP(cpu.effectiveDynamicViscosityPaS,
				gpu.effectiveDynamicViscosityPaS,maximumULPs) ) return false;
		for( unsigned int axis=0u;axis<3u;++axis ) if( !SameFloatVectorsWithinULP(
			cpu.beginningViscousMomentumRateKGPerM2S2[axis],
			gpu.beginningViscousMomentumRateKGPerM2S2[axis],maximumULPs)||
			!SameFloatVectorsWithinULP(cpu.gravityMomentumIncrementKGPerM2S[axis],
				gpu.gravityMomentumIncrementKGPerM2S[axis],maximumULPs) ) return false;
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

	FireProductionVremanInput vremanInput;
	vremanInput.coefficient=static_cast<float>(FireSimulationTransportRecord::OpenV1().VremanCv());
	vremanInput.directionalWidthsM={0.01f,0.0125f,0.008f};
	vremanInput.velocityGradientPerS={12.0f,3.0f,0.0f,-2.0f,-4.0f,1.0f,
		0.5f,2.0f,-8.0f};
	double certifiedGradient[3][3]={};double certifiedWidths[3]={};
	for( unsigned int i=0u;i<3u;++i ) {
		certifiedWidths[i]=vremanInput.directionalWidthsM[i];
		for( unsigned int j=0u;j<3u;++j )
			certifiedGradient[i][j]=vremanInput.velocityGradientPerS[3u*i+j];
	}
	double certifiedVreman=0.0;float productionVreman=0.0f;
	Check(FireSimulationTransportRecord::OpenV1().VremanEddyViscosityM2PerS(
		certifiedGradient,certifiedWidths,certifiedVreman,&error)&&
		EvaluateFireProductionVremanEddyViscosity(vremanInput,productionVreman,&error)&&
		productionVreman==0x1.86cf3p-15f&&
		std::fabs(productionVreman-static_cast<float>(certifiedVreman))<=
			4.0f*std::numeric_limits<float>::epsilon()*
			std::max(1.0f,std::fabs(static_cast<float>(certifiedVreman))),
		"strict-fp32 Vreman bytes are pinned and remain within four ulps of the "
		"certified-record oracle");
	FireProductionVremanInput zeroVremanInput;
	float zeroProductionVreman=1.0f;
	Check(EvaluateFireProductionVremanEddyViscosity(zeroVremanInput,
		zeroProductionVreman,&error)&&zeroProductionVreman==0.0f,
		"production Vreman zero-gradient branch is exact");
	FireProductionVremanInput laminarVremanInput;
	laminarVremanInput.velocityGradientPerS[0]=7.0f;
	float laminarProductionVreman=1.0f;
	Check(EvaluateFireProductionVremanEddyViscosity(laminarVremanInput,
		laminarProductionVreman,&error)&&laminarProductionVreman==0.0f,
		"production Vreman nonnegative B_beta floor preserves a rank-one laminar gradient");
	FireProductionVremanInput roundedNegativeBBeta;
	roundedNegativeBBeta.velocityGradientPerS={-0x1.01e168p+7f,-0x1.1a8adcp+7f,
		-0x1.f9930ap+8f,-0x1.cf77f2p+7f,-0x1.fbcaa0p+7f,-0x1.c65094p+9f,
		0x1.f29338p+10f,0x1.1120b0p+11f,0x1.e8ba58p+12f};
	roundedNegativeBBeta.directionalWidthsM={0x1.4f33a4p-5f,0x1.7b5decp-4f,
		0x1.6c25e8p-4f};
	float flooredProductionVreman=1.0f;
	Check(EvaluateFireProductionVremanEddyViscosity(roundedNegativeBBeta,
		flooredProductionVreman,&error)&&flooredProductionVreman==0.0f,
		"production Vreman floors a measured negative strict-fp32 B_beta roundoff witness");
	FireProductionVremanInput associationVreman;
	associationVreman.velocityGradientPerS={-0x1.08ceacp+13f,-0x1.54d38p+12f,
		0x1.5de604p+12f,-0x1.c491c4p+11f,-0x1.33f43p+10f,0x1.2ae3acp+13f,
		0x1.1754dcp+12f,-0x1.bc26ap+9f,0x1.2abe5p+13f};
	associationVreman.directionalWidthsM={0x1.bf4aacp-10f,0x1.d3201ep-7f,
		0x1.29de1ap-10f};
	float associationVremanValue=0.0f;
	Check(EvaluateFireProductionVremanEddyViscosity(associationVreman,
		associationVremanValue,&error)&&associationVremanValue==0x1.1454fep-7f,
		"production Vreman pins (widthSquared*gradient_i)*gradient_j association");
	FireProductionVremanInput accumulationVreman;
	accumulationVreman.velocityGradientPerS={0x1.de60cp+9f,0x1.09475p+10f,
		0x1.fa119p+11f,0x1.15111cp+13f,-0x1.1bce9cp+11f,0x1.43e0bp+11f,
		0x1.e618cp+11f,0x1.44593p+10f,0x1.96169p+12f};
	accumulationVreman.directionalWidthsM={0x1.9779eap-6f,0x1.dba5a4p-12f,
		0x1.c99916p-2f};
	float accumulationVremanValue=0.0f;
	Check(EvaluateFireProductionVremanEddyViscosity(accumulationVreman,
		accumulationVremanValue,&error)&&accumulationVremanValue==0x1.3795a6p-1f,
		"production Vreman pins the m=0,1,2 beta accumulation order");
	FireProductionVremanInput malformedVreman=vremanInput;
	malformedVreman.velocityGradientPerS[4]=std::numeric_limits<float>::quiet_NaN();
	productionVreman=9.0f;error.clear();
	Check(!EvaluateFireProductionVremanEddyViscosity(malformedVreman,
		productionVreman,&error)&&productionVreman==0.0f&&
		error.find("nonfinite")!=std::string::npos,
		"production Vreman rejects nonfinite operands with no partial value");
	malformedVreman=vremanInput;malformedVreman.directionalWidthsM[1]=-0.01f;
	productionVreman=9.0f;error.clear();
	Check(!EvaluateFireProductionVremanEddyViscosity(malformedVreman,
		productionVreman,&error)&&productionVreman==0.0f&&
		error.find("filter width")!=std::string::npos,
		"production Vreman rejects a signed-invalid filter width with no partial value");
	bool everyViscousScheduleThreshold=true;
	for( std::uint32_t expected=1u;expected<=8u;++expected ) {
		FireProductionViscousSchedule selected;
		const float interiorLambda=static_cast<float>(2u*expected-1u);
		everyViscousScheduleThreshold=everyViscousScheduleThreshold&&
			SelectFireProductionViscousSchedule(1.0f,interiorLambda,selected,&error)&&
			selected.substepCount==expected&&selected.substepTimeS==
				1.0f/static_cast<float>(expected)&&
			selected.outwardWork==std::nextafter(
				static_cast<double>(interiorLambda)*0.5,
				std::numeric_limits<double>::infinity())&&
			selected.representedProductUpper==std::nextafter(
				static_cast<double>(selected.substepTimeS)*
					static_cast<double>(interiorLambda),
				std::numeric_limits<double>::infinity())&&
			selected.representedProductUpper<=2.0;
		if( expected<8u ) {
			const float exactBoundary=static_cast<float>(2u*expected);
			FireProductionViscousSchedule below,at;
			everyViscousScheduleThreshold=everyViscousScheduleThreshold&&
				SelectFireProductionViscousSchedule(1.0f,
					std::nextafter(exactBoundary,0.0f),below,&error)&&
				below.substepCount==expected&&
				SelectFireProductionViscousSchedule(1.0f,exactBoundary,at,&error)&&
				at.substepCount==expected+1u;
		}
	}
	Check(everyViscousScheduleThreshold,
		"outward viscous selection straddles every represented N=1..8 threshold");
	FireProductionViscousSchedule roundedDivisionSchedule;
	Check(SelectFireProductionViscousSchedule(0x1.ce70f4p-1f,0x1.a92704p+2f,
		roundedDivisionSchedule,&error)&&roundedDivisionSchedule.substepCount==4u&&
		roundedDivisionSchedule.outwardWork==std::nextafter(
			(static_cast<double>(0x1.ce70f4p-1f)*static_cast<double>(0x1.a92704p+2f))*0.5,
			std::numeric_limits<double>::infinity())&&
		roundedDivisionSchedule.representedProductUpper==std::nextafter(
			static_cast<double>(roundedDivisionSchedule.substepTimeS)*
				static_cast<double>(0x1.a92704p+2f),
			std::numeric_limits<double>::infinity())&&
		roundedDivisionSchedule.representedProductUpper<=2.0,
		"represented upward-rounded dt/N increments the pre-dispatch substep count");
	FireProductionViscousSchedule rejectedSchedule;
	rejectedSchedule.substepCount=3u;rejectedSchedule.substepTimeS=4.0f;
	rejectedSchedule.outwardWork=5.0;rejectedSchedule.representedProductUpper=6.0;
	error.clear();
	Check(!SelectFireProductionViscousSchedule(1.0f,16.0f,rejectedSchedule,&error)&&
		rejectedSchedule.substepCount==0u&&rejectedSchedule.substepTimeS==0.0f&&
		rejectedSchedule.outwardWork==0.0&&rejectedSchedule.representedProductUpper==0.0&&
		error.find("eight")!=std::string::npos,
		"outward exact N=8 boundary rejects a required ninth substep transactionally");
	rejectedSchedule.substepCount=3u;rejectedSchedule.substepTimeS=4.0f;
	rejectedSchedule.outwardWork=5.0;rejectedSchedule.representedProductUpper=6.0;
	error.clear();
	Check(!SelectFireProductionViscousSchedule(1.0f,-1.0f,rejectedSchedule,&error)&&
		rejectedSchedule.substepCount==0u&&rejectedSchedule.substepTimeS==0.0f&&
		rejectedSchedule.outwardWork==0.0&&rejectedSchedule.representedProductUpper==0.0&&
		error.find("input")!=std::string::npos,
		"viscous scheduling rejects negative Lambda with a fully default payload");
	FireProductionFrozenForceRequest forceRest;
	forceRest.shape.nx=5u;forceRest.shape.ny=6u;forceRest.shape.nz=7u;
	forceRest.shape.cellWidthM=0.1f;forceRest.timeStepS=0.02f;
	forceRest.ambientDensityKGPerM3=1.2f;
	forceRest.vremanCoefficient=static_cast<float>(
		FireSimulationTransportRecord::OpenV1().VremanCv());
	forceRest.boundary.fill(FireProductionProjectionPressureOpen);
	forceRest.cellGasDensityKGPerM3.assign(forceRest.shape.CellCount(),1.2f);
	forceRest.molecularKinematicViscosityM2PerS.assign(forceRest.shape.CellCount(),0.01f);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		const std::size_t faces=FireProductionProjectionFaceCount(forceRest.shape,axis);
		forceRest.faceDensityKGPerM3[axis].assign(faces,1.2f);
		forceRest.beginningMomentumKGPerM2S[axis].assign(faces,0.0f);
	}
	FireProductionFrozenForceResult forceRestResult;
	bool forceRestExact=BuildFireProductionFrozenForceFieldsCPU(
		forceRest,forceRestResult,&error)&&
		forceRestResult.eddyKinematicViscosityM2PerS.size()==forceRest.shape.CellCount()&&
		forceRestResult.effectiveDynamicViscosityPaS.size()==forceRest.shape.CellCount()&&
		std::all_of(forceRestResult.eddyKinematicViscosityM2PerS.begin(),
			forceRestResult.eddyKinematicViscosityM2PerS.end(),
			[](float value){return value==0.0f;});
	for( std::size_t cell=0u;cell<forceRest.shape.CellCount();++cell )
		forceRestExact=forceRestExact&&forceRestResult.effectiveDynamicViscosityPaS[cell]==
			forceRest.cellGasDensityKGPerM3[cell]*
			forceRest.molecularKinematicViscosityM2PerS[cell];
	for( unsigned int axis=0u;axis<3u;++axis )
		forceRestExact=forceRestExact&&
		forceRestResult.beginningViscousMomentumRateKGPerM2S2[axis].size()==
			FireProductionProjectionFaceCount(forceRest.shape,axis)&&
		forceRestResult.gravityMomentumIncrementKGPerM2S[axis].size()==
			FireProductionProjectionFaceCount(forceRest.shape,axis)&&std::all_of(
			forceRestResult.beginningViscousMomentumRateKGPerM2S2[axis].begin(),
			forceRestResult.beginningViscousMomentumRateKGPerM2S2[axis].end(),
			[](float value){return value==0.0f;})&&std::all_of(
			forceRestResult.gravityMomentumIncrementKGPerM2S[axis].begin(),
			forceRestResult.gravityMomentumIncrementKGPerM2S[axis].end(),
			[](float value){return value==0.0f;});
	Check(forceRestExact,
		"ambient rest gives zero Vreman, stress divergence, and relative gravity exactly");
	FireProductionFrozenForceRequest gravityForce=forceRest;
	gravityForce.gravityMPerS2={0.0f,-9.81f,0.0f};
	gravityForce.boundary[2u]=FireProductionProjectionWall;
	for( float& density : gravityForce.faceDensityKGPerM3[1] ) density=1.5f;
	FireProductionFrozenForceResult gravityForceResult;
	bool gravityForceExact=BuildFireProductionFrozenForceFieldsCPU(
		gravityForce,gravityForceResult,&error);
	for( std::size_t z=0u;z<gravityForce.shape.nz;++z )
		for( std::size_t y=0u;y<=gravityForce.shape.ny;++y )
			for( std::size_t x=0u;x<gravityForce.shape.nx;++x ) {
				// Derived by the strict-fp32 association dt*(rho_f-rho_amb)*g.
				// Writing the expression here would let the Opto test translation
				// unit contract it differently from the strict production object.
				const float expected=y==0u?0.0f:-0x1.e22e5ap-5f;
				const float observed=gravityForceResult.gravityMomentumIncrementKGPerM2S[1][
					TransportFaceIndex(gravityForce.shape,1u,x,y,z)];
				gravityForceExact=gravityForceExact&&observed==expected;
			}
	Check(gravityForceExact,
		"relative gravity uses authoritative face density, keeps a wall prescribed, "
		"and updates the opposite open endpoint");
	FireProductionFrozenForceRequest affineForce=forceRest;
	affineForce.ambientDensityKGPerM3=1.0f;
	std::fill(affineForce.cellGasDensityKGPerM3.begin(),
		affineForce.cellGasDensityKGPerM3.end(),1.0f);
	for( unsigned int axis=0u;axis<3u;++axis )
		std::fill(affineForce.faceDensityKGPerM3[axis].begin(),
			affineForce.faceDensityKGPerM3[axis].end(),1.0f);
	for( std::size_t z=0u;z<affineForce.shape.nz;++z )
		for( std::size_t y=0u;y<affineForce.shape.ny;++y )
			for( std::size_t x=0u;x<=affineForce.shape.nx;++x ) {
				const float px=static_cast<float>(x)*affineForce.shape.cellWidthM;
				const float py=(static_cast<float>(y)+0.5f)*affineForce.shape.cellWidthM;
				affineForce.beginningMomentumKGPerM2S[0][TransportFaceIndex(
					affineForce.shape,0u,x,y,z)]=2.0f*px+3.0f*py;
			}
	for( std::size_t z=0u;z<affineForce.shape.nz;++z )
		for( std::size_t y=0u;y<=affineForce.shape.ny;++y )
			for( std::size_t x=0u;x<affineForce.shape.nx;++x ) {
				const float px=(static_cast<float>(x)+0.5f)*affineForce.shape.cellWidthM;
				const float pz=(static_cast<float>(z)+0.5f)*affineForce.shape.cellWidthM;
				affineForce.beginningMomentumKGPerM2S[1][TransportFaceIndex(
					affineForce.shape,1u,x,y,z)]=-px+4.0f*pz;
			}
	for( std::size_t z=0u;z<=affineForce.shape.nz;++z )
		for( std::size_t y=0u;y<affineForce.shape.ny;++y )
			for( std::size_t x=0u;x<affineForce.shape.nx;++x ) {
				const float py=(static_cast<float>(y)+0.5f)*affineForce.shape.cellWidthM;
				const float pz=static_cast<float>(z)*affineForce.shape.cellWidthM;
				affineForce.beginningMomentumKGPerM2S[2][TransportFaceIndex(
					affineForce.shape,2u,x,y,z)]=0.5f*py-2.0f*pz;
			}
	FireProductionFrozenForceResult affineForceResult;
	FireProductionVremanInput affineCenterVreman;
	affineCenterVreman.coefficient=affineForce.vremanCoefficient;
	affineCenterVreman.directionalWidthsM.fill(affineForce.shape.cellWidthM);
	affineCenterVreman.velocityGradientPerS={2.0f,-1.0f,0.0f,3.0f,0.0f,0.5f,
		0.0f,4.0f,-2.0f};
	float expectedAffineCenterVreman=0.0f;
	const std::size_t affineCenter=TransportCellIndex(affineForce.shape,2u,2u,2u);
	Check(EvaluateFireProductionVremanEddyViscosity(affineCenterVreman,
		expectedAffineCenterVreman,&error)&&BuildFireProductionFrozenForceFieldsCPU(
		affineForce,affineForceResult,&error)&&
		affineForceResult.eddyKinematicViscosityM2PerS[affineCenter]==
			expectedAffineCenterVreman&&
		affineForceResult.effectiveDynamicViscosityPaS[affineCenter]==
			affineForce.cellGasDensityKGPerM3[affineCenter]*(
				affineForce.molecularKinematicViscosityM2PerS[affineCenter]+
				expectedAffineCenterVreman),
		"frozen affine-gradient Vreman and effective mu match an independent center oracle");
	FireProductionFrozenForceRequest transposeShear=forceRest;
	transposeShear.shape.cellWidthM=0.125f;
	transposeShear.ambientDensityKGPerM3=1.0f;
	transposeShear.vremanCoefficient=0.0f;
	std::fill(transposeShear.cellGasDensityKGPerM3.begin(),
		transposeShear.cellGasDensityKGPerM3.end(),1.0f);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		std::fill(transposeShear.faceDensityKGPerM3[axis].begin(),
			transposeShear.faceDensityKGPerM3[axis].end(),1.0f);
		std::fill(transposeShear.beginningMomentumKGPerM2S[axis].begin(),
			transposeShear.beginningMomentumKGPerM2S[axis].end(),0.0f);
	}
	for( std::size_t z=0u;z<transposeShear.shape.nz;++z )
		for( std::size_t y=0u;y<transposeShear.shape.ny;++y )
			for( std::size_t x=0u;x<transposeShear.shape.nx;++x )
				transposeShear.molecularKinematicViscosityM2PerS[TransportCellIndex(
					transposeShear.shape,x,y,z)]=0.03125f+static_cast<float>(y)*0.0078125f;
	for( std::size_t z=0u;z<transposeShear.shape.nz;++z )
		for( std::size_t y=0u;y<=transposeShear.shape.ny;++y )
			for( std::size_t x=0u;x<transposeShear.shape.nx;++x )
				transposeShear.beginningMomentumKGPerM2S[1][TransportFaceIndex(
					transposeShear.shape,1u,x,y,z)]=
					2.0f*(static_cast<float>(x)+0.5f)*transposeShear.shape.cellWidthM;
	FireProductionFrozenForceResult transposeShearResult;
	const std::size_t transposeFace=TransportFaceIndex(
		transposeShear.shape,0u,2u,2u,2u);
	Check(BuildFireProductionFrozenForceFieldsCPU(
		transposeShear,transposeShearResult,&error)&&
		transposeShearResult.beginningViscousMomentumRateKGPerM2S2[0][transposeFace]==
			0.125f&&std::all_of(
				transposeShearResult.eddyKinematicViscosityM2PerS.begin(),
				transposeShearResult.eddyKinematicViscosityM2PerS.end(),
				[](float value){return value==0.0f;}),
		"variable-mu transverse shear binds the transposed gradient and four-cell stencil");
	FireProductionFrozenForceRequest wallShear=transposeShear;
	wallShear.boundary[2u]=FireProductionProjectionWall;
	std::fill(wallShear.molecularKinematicViscosityM2PerS.begin(),
		wallShear.molecularKinematicViscosityM2PerS.end(),0.015625f);
	for( std::size_t face=0u;face<wallShear.beginningMomentumKGPerM2S[0].size();++face )
		wallShear.beginningMomentumKGPerM2S[0][face]=1.0f;
	std::fill(wallShear.beginningMomentumKGPerM2S[1].begin(),
		wallShear.beginningMomentumKGPerM2S[1].end(),0.0f);
	FireProductionFrozenForceResult wallShearResult;
	const std::size_t lowWallShearFace=TransportFaceIndex(
		wallShear.shape,0u,2u,0u,2u);
	const std::size_t openInteriorShearFace=TransportFaceIndex(
		wallShear.shape,0u,2u,wallShear.shape.ny-1u,2u);
	const std::size_t pressureOpenNormalLow=TransportFaceIndex(
		wallShear.shape,0u,0u,0u,2u);
	const std::size_t pressureOpenNormalHigh=TransportFaceIndex(
		wallShear.shape,0u,wallShear.shape.nx,0u,2u);
	Check(BuildFireProductionFrozenForceFieldsCPU(wallShear,wallShearResult,&error)&&
		wallShearResult.beginningViscousMomentumRateKGPerM2S2[0][lowWallShearFace]==
			-1.0f&&
		wallShearResult.beginningViscousMomentumRateKGPerM2S2[0][openInteriorShearFace]==
			0.0f&&
		wallShearResult.beginningViscousMomentumRateKGPerM2S2[0][pressureOpenNormalLow]==
			0.0f&&
		wallShearResult.beginningViscousMomentumRateKGPerM2S2[0][pressureOpenNormalHigh]==
			0.0f,
		"odd no-slip wall ghosts create the exact tangential shear while open extension does not");
	FireProductionFrozenForceRequest upperWallShear=transposeShear;
	upperWallShear.boundary[3u]=FireProductionProjectionWall;
	upperWallShear.timeStepS=0.125f;upperWallShear.gravityMPerS2[1]=1.0f;
	std::fill(upperWallShear.faceDensityKGPerM3[1].begin(),
		upperWallShear.faceDensityKGPerM3[1].end(),2.0f);
	std::fill(upperWallShear.molecularKinematicViscosityM2PerS.begin(),
		upperWallShear.molecularKinematicViscosityM2PerS.end(),0.015625f);
	for( std::size_t face=0u;face<upperWallShear.beginningMomentumKGPerM2S[0].size();++face )
		upperWallShear.beginningMomentumKGPerM2S[0][face]=1.0f;
	std::fill(upperWallShear.beginningMomentumKGPerM2S[1].begin(),
		upperWallShear.beginningMomentumKGPerM2S[1].end(),0.0f);
	FireProductionFrozenForceResult upperWallShearResult;
	const std::size_t lowerOpenShearFace=TransportFaceIndex(
		upperWallShear.shape,0u,2u,0u,2u);
	const std::size_t highWallShearFace=TransportFaceIndex(
		upperWallShear.shape,0u,2u,upperWallShear.shape.ny-1u,2u);
	const std::size_t lowerOpenGravityFace=TransportFaceIndex(
		upperWallShear.shape,1u,2u,0u,2u);
	const std::size_t upperWallGravityFace=TransportFaceIndex(
		upperWallShear.shape,1u,2u,upperWallShear.shape.ny,2u);
	Check(BuildFireProductionFrozenForceFieldsCPU(
		upperWallShear,upperWallShearResult,&error)&&
		upperWallShearResult.beginningViscousMomentumRateKGPerM2S2[0][
			lowerOpenShearFace]==0.0f&&
		upperWallShearResult.beginningViscousMomentumRateKGPerM2S2[0][
			highWallShearFace]==-1.0f&&
		upperWallShearResult.gravityMomentumIncrementKGPerM2S[1][lowerOpenGravityFace]==
			0.125f&&
		upperWallShearResult.gravityMomentumIncrementKGPerM2S[1][upperWallGravityFace]==
			0.0f,
		"upper odd wall and lower pressure-open extensions retain distinct exact shear roles");
	FireProductionFrozenForceRequest compressionForce=transposeShear;
	for( std::size_t z=0u;z<compressionForce.shape.nz;++z )
		for( std::size_t y=0u;y<compressionForce.shape.ny;++y )
			for( std::size_t x=0u;x<compressionForce.shape.nx;++x )
				compressionForce.molecularKinematicViscosityM2PerS[TransportCellIndex(
					compressionForce.shape,x,y,z)]=
					0.03125f+static_cast<float>(x)*0.0078125f;
	for( unsigned int axis=0u;axis<3u;++axis )
		std::fill(compressionForce.beginningMomentumKGPerM2S[axis].begin(),
			compressionForce.beginningMomentumKGPerM2S[axis].end(),0.0f);
	for( std::size_t z=0u;z<compressionForce.shape.nz;++z )
		for( std::size_t y=0u;y<compressionForce.shape.ny;++y )
			for( std::size_t x=0u;x<=compressionForce.shape.nx;++x )
				compressionForce.beginningMomentumKGPerM2S[0][TransportFaceIndex(
					compressionForce.shape,0u,x,y,z)]=
					2.0f*static_cast<float>(x)*compressionForce.shape.cellWidthM;
	FireProductionFrozenForceResult compressionForceResult;
	const std::size_t compressionFace=TransportFaceIndex(
		compressionForce.shape,0u,2u,2u,2u);
	Check(BuildFireProductionFrozenForceFieldsCPU(
		compressionForce,compressionForceResult,&error)&&
		compressionForceResult.beginningViscousMomentumRateKGPerM2S2[0][compressionFace]==
			0x1.555558p-3f,
		"variable-mu compression binds the two-thirds deviatoric trace subtraction");
	FireProductionFrozenForceRequest wallNormalCompression=compressionForce;
	wallNormalCompression.boundary[1u]=FireProductionProjectionWall;
	FireProductionFrozenForceResult wallNormalCompressionResult;
	const std::size_t openCompressionEndpoint=TransportFaceIndex(
		wallNormalCompression.shape,0u,0u,2u,2u);
	const std::size_t wallCompressionEndpoint=TransportFaceIndex(
		wallNormalCompression.shape,0u,wallNormalCompression.shape.nx,2u,2u);
	const std::size_t adjacentCompressionFace=TransportFaceIndex(
		wallNormalCompression.shape,0u,wallNormalCompression.shape.nx-1u,2u,2u);
	Check(BuildFireProductionFrozenForceFieldsCPU(
		wallNormalCompression,wallNormalCompressionResult,&error)&&
		wallNormalCompressionResult.beginningViscousMomentumRateKGPerM2S2[0][
			openCompressionEndpoint]==0.0f&&
		wallNormalCompressionResult.beginningViscousMomentumRateKGPerM2S2[0][
			wallCompressionEndpoint]==0.0f&&
		wallNormalCompressionResult.beginningViscousMomentumRateKGPerM2S2[0][
			adjacentCompressionFace]==-0x1.affffep+1f,
		"pressure-open and wall normal endpoints own no viscous increment beside active interior work");
	FireProductionFrozenForceRequest authorityForce=forceRest;
	authorityForce.shape.cellWidthM=1.0f;authorityForce.ambientDensityKGPerM3=1.0f;
	std::fill(authorityForce.cellGasDensityKGPerM3.begin(),
		authorityForce.cellGasDensityKGPerM3.end(),3.0f);
	std::fill(authorityForce.molecularKinematicViscosityM2PerS.begin(),
		authorityForce.molecularKinematicViscosityM2PerS.end(),0.125f);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		std::fill(authorityForce.faceDensityKGPerM3[axis].begin(),
			authorityForce.faceDensityKGPerM3[axis].end(),2.0f);
		std::fill(authorityForce.beginningMomentumKGPerM2S[axis].begin(),
			authorityForce.beginningMomentumKGPerM2S[axis].end(),0.0f);
	}
	for( std::size_t z=0u;z<authorityForce.shape.nz;++z )
		for( std::size_t y=0u;y<authorityForce.shape.ny;++y )
			for( std::size_t x=0u;x<=authorityForce.shape.nx;++x )
				authorityForce.beginningMomentumKGPerM2S[0][TransportFaceIndex(
					authorityForce.shape,0u,x,y,z)]=
					2.0f*static_cast<float>(x*x);
	for( std::size_t z=0u;z<authorityForce.shape.nz;++z )
		for( std::size_t y=0u;y<=authorityForce.shape.ny;++y )
			for( std::size_t x=0u;x<authorityForce.shape.nx;++x )
				authorityForce.beginningMomentumKGPerM2S[1][TransportFaceIndex(
					authorityForce.shape,1u,x,y,z)]=
					2.0f*static_cast<float>(y*y);
	FireProductionVremanInput authorityVreman;
	authorityVreman.coefficient=authorityForce.vremanCoefficient;
	authorityVreman.directionalWidthsM.fill(1.0f);
	authorityVreman.velocityGradientPerS={5.0f,0.0f,0.0f,0.0f,5.0f,0.0f,
		0.0f,0.0f,0.0f};
	float expectedAuthorityVreman=0.0f;
	FireProductionFrozenForceResult authorityForceResult;
	const std::size_t authorityCell=TransportCellIndex(authorityForce.shape,2u,2u,2u);
	Check(EvaluateFireProductionVremanEddyViscosity(authorityVreman,
		expectedAuthorityVreman,&error)&&BuildFireProductionFrozenForceFieldsCPU(
			authorityForce,authorityForceResult,&error)&&
		authorityForceResult.eddyKinematicViscosityM2PerS[authorityCell]==
			expectedAuthorityVreman&&
		authorityForceResult.effectiveDynamicViscosityPaS[authorityCell]==
			3.0f*(0.125f+expectedAuthorityVreman),
		"two-face velocity averaging, authoritative face division, and cell-density mu are bound");
	FireProductionFrozenForceRequest periodicForce=forceRest;
	periodicForce.shape.cellWidthM=1.0f;periodicForce.timeStepS=0.125f;
	periodicForce.ambientDensityKGPerM3=1.0f;periodicForce.gravityMPerS2[0]=1.0f;
	periodicForce.boundary.fill(FireProductionProjectionPeriodic);
	std::fill(periodicForce.cellGasDensityKGPerM3.begin(),
		periodicForce.cellGasDensityKGPerM3.end(),1.0f);
	std::fill(periodicForce.molecularKinematicViscosityM2PerS.begin(),
		periodicForce.molecularKinematicViscosityM2PerS.end(),1.0f);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		std::fill(periodicForce.faceDensityKGPerM3[axis].begin(),
			periodicForce.faceDensityKGPerM3[axis].end(),1.0f);
		std::fill(periodicForce.beginningMomentumKGPerM2S[axis].begin(),
			periodicForce.beginningMomentumKGPerM2S[axis].end(),0.0f);
	}
	std::fill(periodicForce.faceDensityKGPerM3[0].begin(),
		periodicForce.faceDensityKGPerM3[0].end(),2.0f);
	const std::size_t periodicPulseX[]={0u,2u,4u,periodicForce.shape.nx};
	for( std::size_t z=0u;z<periodicForce.shape.nz;++z )
		for( std::size_t y=0u;y<periodicForce.shape.ny;++y ) {
			for( const std::size_t x : periodicPulseX )
				periodicForce.beginningMomentumKGPerM2S[0][TransportFaceIndex(
					periodicForce.shape,0u,x,y,z)]=2.0f;
		}
	for( std::size_t z=0u;z<periodicForce.shape.nz;++z )
		for( std::size_t x=0u;x<periodicForce.shape.nx;++x )
			periodicForce.beginningMomentumKGPerM2S[1][TransportFaceIndex(
				periodicForce.shape,1u,x,2u,z)]=1.0f;
	FireProductionFrozenForceResult periodicForceResult;
	const std::size_t periodicLow=TransportFaceIndex(periodicForce.shape,0u,0u,2u,2u);
	const std::size_t periodicHigh=TransportFaceIndex(
		periodicForce.shape,0u,periodicForce.shape.nx,2u,2u);
	const std::size_t periodicCell=TransportCellIndex(periodicForce.shape,0u,2u,2u);
	const bool periodicBuilt=BuildFireProductionFrozenForceFieldsCPU(
		periodicForce,periodicForceResult,&error);
	Check(periodicBuilt&&
		periodicForceResult.beginningViscousMomentumRateKGPerM2S2[0][periodicLow]==
			-0x1.5771fap-2f&&
		periodicForceResult.eddyKinematicViscosityM2PerS[periodicCell]==0x1.957bbap-7f&&
		periodicForceResult.beginningViscousMomentumRateKGPerM2S2[0][periodicHigh]==
			periodicForceResult.beginningViscousMomentumRateKGPerM2S2[0][periodicLow]&&
		periodicForceResult.gravityMomentumIncrementKGPerM2S[0][periodicLow]==0.125f&&
		periodicForceResult.gravityMomentumIncrementKGPerM2S[0][periodicHigh]==
			periodicForceResult.gravityMomentumIncrementKGPerM2S[0][periodicLow],
		"periodic face zero owns the wrapped viscous stencil and publishes one exact seam copy");
	FireProductionFrozenForceRequest advanceForce=periodicForce;
	advanceForce.vremanCoefficient=0.0f;
	for( std::size_t z=0u;z<advanceForce.shape.nz;++z )
		for( std::size_t y=0u;y<advanceForce.shape.ny;++y )
			for( std::size_t x=0u;x<advanceForce.shape.nx;++x ) {
				const std::size_t cell=TransportCellIndex(advanceForce.shape,x,y,z);
				advanceForce.cellGasDensityKGPerM3[cell]=((x+y+z)&1u)?2.0f:1.0f;
				advanceForce.molecularKinematicViscosityM2PerS[cell]=0.25f;
			}
	for( std::size_t z=0u;z<advanceForce.shape.nz;++z )
		for( std::size_t y=0u;y<advanceForce.shape.ny;++y )
			for( std::size_t x=0u;x<=advanceForce.shape.nx;++x ) {
				const std::size_t canonicalX=x==advanceForce.shape.nx?0u:x;
				advanceForce.faceDensityKGPerM3[0][TransportFaceIndex(
					advanceForce.shape,0u,x,y,z)]=1.25f+0.25f*static_cast<float>(canonicalX);
			}
	FireProductionFrozenForceResult advanceForceFields;
	const bool advanceForceBuilt=BuildFireProductionFrozenForceFieldsCPU(
		advanceForce,advanceForceFields,&error);
	FireProductionFrozenForceAdvanceResult periodicAdvance;
	bool periodicAdvanceExact=advanceForceBuilt&&AdvanceFireProductionFrozenForceCPU(
		advanceForce,16.0f,periodicAdvance,&error)&&
		periodicAdvance.schedule.substepCount==2u&&
		periodicAdvance.executedViscousSubstepCount==2u&&
		periodicAdvance.intermediateMomentumDigestCount==2u&&
		periodicAdvance.frozenFields.effectiveDynamicViscosityPaS==
			advanceForceFields.effectiveDynamicViscosityPaS;
	std::array<std::vector<float>,3> expectedPeriodicMomentum=
		advanceForce.beginningMomentumKGPerM2S;
	PublishTestForceBoundaries(advanceForce,expectedPeriodicMomentum);
	FireProductionFrozenForceRequest periodicSubstep=advanceForce;
	periodicSubstep.timeStepS=periodicAdvance.schedule.substepTimeS;
	periodicSubstep.gravityMPerS2.fill(0.0f);periodicSubstep.vremanCoefficient=0.0f;
	for( std::size_t cell=0u;cell<advanceForce.shape.CellCount();++cell )
		periodicSubstep.molecularKinematicViscosityM2PerS[cell]=
			advanceForceFields.effectiveDynamicViscosityPaS[cell]/
			advanceForce.cellGasDensityKGPerM3[cell];
	std::array<std::uint64_t,8> expectedIntermediateDigests={};
	for( std::uint32_t substep=0u;substep<periodicAdvance.schedule.substepCount;++substep ) {
		periodicSubstep.beginningMomentumKGPerM2S=expectedPeriodicMomentum;
		FireProductionFrozenForceResult expectedSubstepFields;
		periodicAdvanceExact=periodicAdvanceExact&&BuildFireProductionFrozenForceFieldsCPU(
			periodicSubstep,expectedSubstepFields,&error);
		if( periodicAdvanceExact ) for( unsigned int axis=0u;axis<3u;++axis )
			for( std::size_t face=0u;face<expectedPeriodicMomentum[axis].size();++face )
				expectedPeriodicMomentum[axis][face]+=
					periodicAdvance.schedule.substepTimeS*
					expectedSubstepFields.beginningViscousMomentumRateKGPerM2S2[axis][face];
		PublishTestForceBoundaries(advanceForce,expectedPeriodicMomentum);
		expectedIntermediateDigests[substep]=
			IndependentMomentumByteDigest(expectedPeriodicMomentum);
	}
	for( unsigned int axis=0u;axis<3u;++axis )
		for( std::size_t face=0u;face<expectedPeriodicMomentum[axis].size();++face )
			expectedPeriodicMomentum[axis][face]+=
				advanceForceFields.gravityMomentumIncrementKGPerM2S[axis][face];
	PublishTestForceBoundaries(advanceForce,expectedPeriodicMomentum);
	std::array<std::vector<float>,3> gravityFirstMomentum=
		advanceForce.beginningMomentumKGPerM2S;
	for( unsigned int axis=0u;axis<3u;++axis )
		for( std::size_t face=0u;face<gravityFirstMomentum[axis].size();++face )
			gravityFirstMomentum[axis][face]+=
				advanceForceFields.gravityMomentumIncrementKGPerM2S[axis][face];
	PublishTestForceBoundaries(advanceForce,gravityFirstMomentum);
	for( std::uint32_t substep=0u;substep<periodicAdvance.schedule.substepCount;++substep ) {
		periodicSubstep.beginningMomentumKGPerM2S=gravityFirstMomentum;
		FireProductionFrozenForceResult gravityFirstFields;
		periodicAdvanceExact=periodicAdvanceExact&&BuildFireProductionFrozenForceFieldsCPU(
			periodicSubstep,gravityFirstFields,&error);
		if( periodicAdvanceExact ) for( unsigned int axis=0u;axis<3u;++axis )
			for( std::size_t face=0u;face<gravityFirstMomentum[axis].size();++face )
				gravityFirstMomentum[axis][face]+=
					periodicAdvance.schedule.substepTimeS*
					gravityFirstFields.beginningViscousMomentumRateKGPerM2S2[axis][face];
		PublishTestForceBoundaries(advanceForce,gravityFirstMomentum);
	}
	Check(periodicAdvanceExact&&
		periodicAdvance.intermediateMomentumByteDigests==expectedIntermediateDigests&&
		periodicAdvance.momentumKGPerM2S==expectedPeriodicMomentum&&
		gravityFirstMomentum!=expectedPeriodicMomentum,
		"every outward-selected viscous intermediate reuses frozen dynamic mu before "
		"one noncommuting gravity update");
	FireProductionFrozenForceAdvanceResult mixedBoundaryAdvance;
	const std::size_t mixedOpenFace=TransportFaceIndex(
		upperWallShear.shape,1u,2u,0u,2u);
	const std::size_t mixedWallFace=TransportFaceIndex(
		upperWallShear.shape,1u,2u,upperWallShear.shape.ny,2u);
	upperWallShear.beginningMomentumKGPerM2S[1][mixedOpenFace]=0.5f;
	upperWallShear.beginningMomentumKGPerM2S[1][mixedWallFace]=7.0f;
	FireProductionFrozenForceResult mixedInitialFields;
	bool mixedAdvanceExact=BuildFireProductionFrozenForceFieldsCPU(
		upperWallShear,mixedInitialFields,&error)&&AdvanceFireProductionFrozenForceCPU(
		upperWallShear,16.0f,mixedBoundaryAdvance,&error)&&
		mixedBoundaryAdvance.intermediateMomentumDigestCount==2u;
	std::array<std::vector<float>,3> expectedMixedMomentum=
		upperWallShear.beginningMomentumKGPerM2S;
	PublishTestForceBoundaries(upperWallShear,expectedMixedMomentum);
	FireProductionFrozenForceRequest mixedSubstep=upperWallShear;
	mixedSubstep.timeStepS=mixedBoundaryAdvance.schedule.substepTimeS;
	mixedSubstep.gravityMPerS2.fill(0.0f);mixedSubstep.vremanCoefficient=0.0f;
	mixedSubstep.molecularKinematicViscosityM2PerS=
		mixedInitialFields.effectiveDynamicViscosityPaS;
	for( std::uint32_t substep=0u;substep<2u;++substep ) {
		mixedSubstep.beginningMomentumKGPerM2S=expectedMixedMomentum;
		FireProductionFrozenForceResult mixedSubstepFields;
		mixedAdvanceExact=mixedAdvanceExact&&BuildFireProductionFrozenForceFieldsCPU(
			mixedSubstep,mixedSubstepFields,&error);
		if( mixedAdvanceExact ) for( unsigned int axis=0u;axis<3u;++axis )
			for( std::size_t face=0u;face<expectedMixedMomentum[axis].size();++face )
				expectedMixedMomentum[axis][face]+=
					mixedBoundaryAdvance.schedule.substepTimeS*
					mixedSubstepFields.beginningViscousMomentumRateKGPerM2S2[axis][face];
		PublishTestForceBoundaries(upperWallShear,expectedMixedMomentum);
		mixedAdvanceExact=mixedAdvanceExact&&
			mixedBoundaryAdvance.intermediateMomentumByteDigests[substep]==
				IndependentMomentumByteDigest(expectedMixedMomentum);
	}
	for( unsigned int axis=0u;axis<3u;++axis )
		for( std::size_t face=0u;face<expectedMixedMomentum[axis].size();++face )
			expectedMixedMomentum[axis][face]+=
				mixedInitialFields.gravityMomentumIncrementKGPerM2S[axis][face];
	PublishTestForceBoundaries(upperWallShear,expectedMixedMomentum);
	Check(mixedAdvanceExact&&mixedBoundaryAdvance.momentumKGPerM2S==
		expectedMixedMomentum&&mixedBoundaryAdvance.momentumKGPerM2S[1][mixedWallFace]==0.0f&&
		mixedBoundaryAdvance.momentumKGPerM2S[1][mixedOpenFace]==0.625f,
		"both mixed-boundary intermediates prescribe a raw wall endpoint while "
		"pressure-open gravity remains owned");
	FireProductionFrozenForceAdvanceResult rejectedAdvance;
	auto SeedRejectedAdvance=[](FireProductionFrozenForceAdvanceResult& seeded) {
		seeded.frozenFields.eddyKinematicViscosityM2PerS.assign(1u,1.0f);
		seeded.frozenFields.effectiveDynamicViscosityPaS.assign(1u,2.0f);
		for( unsigned int axis=0u;axis<3u;++axis ) {
			seeded.frozenFields.beginningViscousMomentumRateKGPerM2S2[axis].assign(1u,3.0f);
			seeded.frozenFields.gravityMomentumIncrementKGPerM2S[axis].assign(1u,4.0f);
			seeded.momentumKGPerM2S[axis].assign(1u,5.0f);
		}
		seeded.schedule.substepCount=6u;seeded.schedule.substepTimeS=7.0f;
		seeded.schedule.outwardWork=8.0;
		seeded.schedule.representedProductUpper=9.0;
		seeded.intermediateMomentumByteDigests.fill(11u);
		seeded.intermediateMomentumDigestCount=12u;
		seeded.executedViscousSubstepCount=10u;
	};
	SeedRejectedAdvance(rejectedAdvance);error.clear();
	const bool rejectedNinthAdvance=!AdvanceFireProductionFrozenForceCPU(
		forceRest,900.0f,rejectedAdvance,&error);
	Check(rejectedNinthAdvance&&FrozenForceAdvanceResultEmpty(rejectedAdvance)&&
		error.find("eight")!=std::string::npos,
		"a required ninth viscous substep rejects before any force-state publication");
	SeedRejectedAdvance(rejectedAdvance);error.clear();denyTestAllocations=true;
	const bool advanceAllocationRejected=!AdvanceFireProductionFrozenForceCPU(
		forceRest,1.0f,rejectedAdvance,&error);
	denyTestAllocations=false;
	Check(advanceAllocationRejected&&FrozenForceAdvanceResultEmpty(rejectedAdvance),
		"persistent allocation denial is contained by the advance API boundary");
	FireProductionFrozenForceRequest lateOverflowAdvance=forceRest;
	lateOverflowAdvance.vremanCoefficient=0.0f;
	std::fill(lateOverflowAdvance.molecularKinematicViscosityM2PerS.begin(),
		lateOverflowAdvance.molecularKinematicViscosityM2PerS.end(),0.0f);
	lateOverflowAdvance.timeStepS=1.0f;lateOverflowAdvance.ambientDensityKGPerM3=1.0f;
	lateOverflowAdvance.gravityMPerS2[0]=2.0f;
	std::fill(lateOverflowAdvance.faceDensityKGPerM3[0].begin(),
		lateOverflowAdvance.faceDensityKGPerM3[0].end(),0x1.2ced32p+126f);
	std::fill(lateOverflowAdvance.beginningMomentumKGPerM2S[0].begin(),
		lateOverflowAdvance.beginningMomentumKGPerM2S[0].end(),0x1.2ced32p+127f);
	SeedRejectedAdvance(rejectedAdvance);error.clear();
	const bool lateOverflowRejected=!AdvanceFireProductionFrozenForceCPU(
		lateOverflowAdvance,0.0f,rejectedAdvance,&error);
	Check(lateOverflowRejected&&FrozenForceAdvanceResultEmpty(rejectedAdvance)&&
		error.find("gravity update overflowed")!=std::string::npos,
		"a finite late gravity overflow discards every completed viscous intermediate");
	FireProductionProjectionShape advanceBelowShape,advanceAboveShape;
	advanceBelowShape.nx=103u;advanceBelowShape.ny=313u;advanceBelowShape.nz=332u;
	advanceAboveShape.nx=86u;advanceAboveShape.ny=200u;advanceAboveShape.nz=622u;
	std::uint64_t advanceBelowBytes=0u,advanceAboveBytes=0u;
	FireProductionFrozenForceRequest advanceAdmission;
	advanceAdmission.shape=advanceAboveShape;advanceAdmission.shape.cellWidthM=1.0f;
	advanceAdmission.timeStepS=1.0f;
	SeedRejectedAdvance(rejectedAdvance);error.clear();
	const bool advanceOverRejected=!AdvanceFireProductionFrozenForceCPU(
		advanceAdmission,0.0f,rejectedAdvance,&error)&&
		FrozenForceAdvanceResultEmpty(rejectedAdvance)&&
		error.find("two GiB")!=std::string::npos;
	advanceAdmission.shape=advanceBelowShape;advanceAdmission.shape.cellWidthM=1.0f;
	SeedRejectedAdvance(rejectedAdvance);error.clear();
	const bool advanceBelowContinues=!AdvanceFireProductionFrozenForceCPU(
		advanceAdmission,0.0f,rejectedAdvance,&error)&&
		FrozenForceAdvanceResultEmpty(rejectedAdvance)&&
		error.find("cell shape")!=std::string::npos;
	Check(FireProductionFrozenForceAdvanceWorkingSetBytes(
		advanceBelowShape,advanceBelowBytes)&&
		FireProductionFrozenForceAdvanceWorkingSetBytes(
			advanceAboveShape,advanceAboveBytes)&&
		advanceBelowBytes==UINT64_C(2147483640)&&
		advanceAboveBytes==UINT64_C(2147483680)&&
		advanceOverRejected&&advanceBelowContinues,
		"multi-substep force peak and actual admission straddle two GiB exactly");
	FireProductionFrozenForceRequest invalidFrozenForce=forceRest;
	auto SeedRejectedFrozenForce=[](FireProductionFrozenForceResult& seeded) {
		seeded.eddyKinematicViscosityM2PerS.assign(1u,1.0f);
		seeded.effectiveDynamicViscosityPaS.assign(1u,2.0f);
		for( unsigned int axis=0u;axis<3u;++axis ) {
			seeded.beginningViscousMomentumRateKGPerM2S2[axis].assign(
				1u,3.0f+static_cast<float>(axis));
			seeded.gravityMomentumIncrementKGPerM2S[axis].assign(
				1u,6.0f+static_cast<float>(axis));
		}
	};
	invalidFrozenForce.molecularKinematicViscosityM2PerS[3]=-1.0f;
	FireProductionFrozenForceResult rejectedFrozenForce;
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("molecular")!=std::string::npos,
		"negative molecular viscosity rejects with a fully default frozen-force payload");
	invalidFrozenForce=forceRest;
	invalidFrozenForce.faceDensityKGPerM3[0][5]=std::numeric_limits<float>::min();
	invalidFrozenForce.beginningMomentumKGPerM2S[0][5]=
		std::numeric_limits<float>::max();
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("overflowed")!=std::string::npos,
		"late derived-velocity overflow cannot publish a partial frozen-force result");
	invalidFrozenForce=forceRest;
	invalidFrozenForce.boundary.fill(FireProductionProjectionPeriodic);
	invalidFrozenForce.beginningMomentumKGPerM2S[2].back()=1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("seam")!=std::string::npos,
		"frozen-force input rejects a noncanonical periodic MAC publication seam");
	invalidFrozenForce=forceRest;
	invalidFrozenForce.boundary.fill(FireProductionProjectionPeriodic);
	invalidFrozenForce.beginningMomentumKGPerM2S[0][0]=0.0f;
	invalidFrozenForce.beginningMomentumKGPerM2S[0][forceRest.shape.nx]=-0.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("seam")!=std::string::npos,
		"periodic seam validation rejects numerically equal but byte-distinct signed zero");
	invalidFrozenForce=forceRest;
	invalidFrozenForce.boundary[0]=FireProductionProjectionPeriodic;
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("pairing")!=std::string::npos,
		"unpaired periodic frozen-force boundaries reject with no result");
	invalidFrozenForce=forceRest;
	invalidFrozenForce.faceDensityKGPerM3[2][7]=-1.0f;
	invalidFrozenForce.beginningMomentumKGPerM2S[2][7]=1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("face state")!=std::string::npos,
		"finite negative authoritative face density is structurally rejected");
	invalidFrozenForce=forceRest;invalidFrozenForce.cellGasDensityKGPerM3[9]=-1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("cell density")!=std::string::npos,
		"finite negative cell density cannot create a negative effective dynamic viscosity");
	invalidFrozenForce=forceRest;invalidFrozenForce.timeStepS=-1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("scalar")!=std::string::npos,
		"finite negative frozen-force timestep is rejected transactionally");
	invalidFrozenForce=forceRest;invalidFrozenForce.ambientDensityKGPerM3=-1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("scalar")!=std::string::npos,
		"finite negative ambient density is rejected transactionally");
	SeedRejectedFrozenForce(rejectedFrozenForce);
	denyTestAllocations=true;
	const bool frozenAllocationRejected=!BuildFireProductionFrozenForceFieldsCPU(
		forceRest,rejectedFrozenForce,0);
	denyTestAllocations=false;
	Check(frozenAllocationRejected&&FrozenForceResultEmpty(rejectedFrozenForce),
		"persistent allocation denial cannot escape or publish a partial frozen-force result");
	FireProductionProjectionShape frozenBytesBelow,frozenBytesAbove;
	frozenBytesBelow.nx=26u;frozenBytesBelow.ny=765u;frozenBytesBelow.nz=865u;
	frozenBytesAbove.nx=49u;frozenBytesAbove.ny=439u;frozenBytesAbove.nz=802u;
	std::uint64_t frozenBelowBytes=0u,frozenAboveBytes=0u;
	Check(FireProductionFrozenForceWorkingSetBytes(frozenBytesBelow,frozenBelowBytes)&&
		FireProductionFrozenForceWorkingSetBytes(frozenBytesAbove,frozenAboveBytes)&&
		frozenBelowBytes==UINT64_C(2147483500)&&
		frozenAboveBytes==UINT64_C(2147483668)&&
		frozenBelowBytes<(UINT64_C(1)<<31u)&&
		frozenAboveBytes>(UINT64_C(1)<<31u),
		"independent near-cap shapes bind all sixteen cell and five face payloads");
	FireProductionFrozenForceRequest frozenAdmission;
	frozenAdmission.shape=frozenBytesAbove;frozenAdmission.shape.cellWidthM=1.0f;
	frozenAdmission.timeStepS=1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	const bool overFrozenAdmission=!BuildFireProductionFrozenForceFieldsCPU(
		frozenAdmission,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("two GiB")!=std::string::npos;
	frozenAdmission.shape=frozenBytesBelow;frozenAdmission.shape.cellWidthM=1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);error.clear();
	const bool underFrozenAdmission=!BuildFireProductionFrozenForceFieldsCPU(
		frozenAdmission,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("cell shape")!=std::string::npos&&
		error.find("two GiB")==std::string::npos;
	Check(overFrozenAdmission&&underFrozenAdmission,
		"the actual frozen-force admission path straddles the exact two-GiB boundary");
	FireProductionProjectionShape forceMetalBelow,forceMetalAbove;
	forceMetalBelow.nx=67u;forceMetalBelow.ny=306u;forceMetalBelow.nz=492u;
	forceMetalAbove.nx=43u;forceMetalAbove.ny=393u;forceMetalAbove.nz=596u;
	FireProductionProjectionShape tier10ForceMetalShape;
	tier10ForceMetalShape.nx=86u;tier10ForceMetalShape.ny=86u;tier10ForceMetalShape.nz=132u;
	std::uint64_t forceMetalBelowBytes=0u,forceMetalAboveBytes=0u,tier10ForceMetalBytes=0u;
	Check(FireProductionFrozenForceMetalWorkingSetBytes(
		forceMetalBelow,forceMetalBelowBytes)&&
		FireProductionFrozenForceMetalWorkingSetBytes(
			forceMetalAbove,forceMetalAboveBytes)&&
		FireProductionFrozenForceMetalWorkingSetBytes(
			tier10ForceMetalShape,tier10ForceMetalBytes)&&
		forceMetalBelowBytes==UINT64_C(2147483504)&&
		forceMetalAboveBytes==UINT64_C(2147483752)&&
		tier10ForceMetalBytes==UINT64_C(208432992),
		"standalone Metal force certificate counts live host arrays and every rounded buffer");
	invalidFrozenForce=FireProductionFrozenForceRequest();
	invalidFrozenForce.shape.nx=1024u;invalidFrozenForce.shape.ny=1024u;
	invalidFrozenForce.shape.nz=1024u;invalidFrozenForce.shape.cellWidthM=1.0f;
	invalidFrozenForce.timeStepS=1.0f;
	SeedRejectedFrozenForce(rejectedFrozenForce);
	error.clear();
	Check(!BuildFireProductionFrozenForceFieldsCPU(
		invalidFrozenForce,rejectedFrozenForce,&error)&&
		FrozenForceResultEmpty(rejectedFrozenForce)&&error.find("two GiB")!=std::string::npos,
		"frozen-force peak is rejected before any oversized allocation or payload access");

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
	FireProductionFrozenForceResult periodicForceGPU,periodicForceGPURepeat,
		mixedForceGPU,authorityForceGPU,restForceGPU,compressionForceGPU;
	double periodicForceMetalMS=0.0,periodicForceRepeatMS=0.0,
		mixedForceMetalMS=0.0,authorityForceMetalMS=0.0,restForceMetalMS=0.0,
		compressionForceMetalMS=0.0;
	const std::uint64_t forceMetalMaximumULPs=8u;
	const bool periodicForceMetal=BuildFireProductionFrozenForceFieldsMetal(
		periodicForce,periodicForceGPU,periodicForceMetalMS,&error);
	const bool periodicForceMetalRepeat=BuildFireProductionFrozenForceFieldsMetal(
		periodicForce,periodicForceGPURepeat,periodicForceRepeatMS,&error);
	const bool mixedForceMetal=BuildFireProductionFrozenForceFieldsMetal(
		upperWallShear,mixedForceGPU,mixedForceMetalMS,&error);
	const bool authorityForceMetal=BuildFireProductionFrozenForceFieldsMetal(
		authorityForce,authorityForceGPU,authorityForceMetalMS,&error);
	const bool restForceMetal=BuildFireProductionFrozenForceFieldsMetal(
		forceRest,restForceGPU,restForceMetalMS,&error);
	const bool compressionForceMetal=BuildFireProductionFrozenForceFieldsMetal(
		compressionForce,compressionForceGPU,compressionForceMetalMS,&error);
	FireProductionFrozenForceRequest residentForce=periodicForce;
	residentForce.timeStepS=62.5f;residentForce.vremanCoefficient=0.0f;
	residentForce.cellGasDensityKGPerM3.assign(residentForce.shape.CellCount(),1.2f);
	residentForce.molecularKinematicViscosityM2PerS.assign(
		residentForce.shape.CellCount(),0.01f);
	for( unsigned int axis=0u;axis<3u;++axis )
		residentForce.faceDensityKGPerM3[axis].assign(
			FireProductionProjectionFaceCount(residentForce.shape,axis),1.2f);
	FireProductionFrozenForceAdvanceResult residentForceGPU,residentForceCPU;
	FireProductionFrozenForceAdvanceResult residentForceGPURepeat;
	FireProductionResidentForceDiagnostics residentForceDiagnostics;
	FireProductionResidentForceDiagnostics residentForceRepeatDiagnostics;
	const bool residentForceMetal=AdvanceFireProductionFrozenForceMetal(
		residentForce,true,residentForceGPU,residentForceDiagnostics,&error);
	const bool residentForceMetalRepeat=AdvanceFireProductionFrozenForceMetal(
		residentForce,true,residentForceGPURepeat,residentForceRepeatDiagnostics,&error);
	const bool residentForceOracle=residentForceMetal&&AdvanceFireProductionFrozenForceCPU(
		residentForce,residentForceDiagnostics.outwardLambdaPerS,residentForceCPU,&error);
	std::uint64_t residentForceMaximumULPs=0u;
	float residentForceMaximumAbsolute=0.0f,residentForceMaximumMagnitude=0.0f;
	bool residentForceComposedWithinBound=true;
	float residentForceExpected=0.0f,residentForceObserved=0.0f;
	unsigned int residentForceAxis=0u;std::size_t residentForceFace=0u;
	if( residentForceOracle ) for( unsigned int axis=0u;axis<3u;++axis )
		for( std::size_t face=0u;face<residentForceCPU.momentumKGPerM2S[axis].size();++face ) {
			const float expected=residentForceCPU.momentumKGPerM2S[axis][face];
			const float observed=residentForceGPU.momentumKGPerM2S[axis][face];
			residentForceMaximumAbsolute=std::max(residentForceMaximumAbsolute,std::fabs(expected-observed));
			residentForceMaximumMagnitude=std::max(residentForceMaximumMagnitude,
				std::max(std::fabs(expected),std::fabs(observed)));
			const std::uint64_t difference=FloatULPDistance(
				expected,observed);
			residentForceComposedWithinBound=residentForceComposedWithinBound&&
				(difference<=64u||std::fabs(expected-observed)<=0x1p-25f);
			if( difference>residentForceMaximumULPs ) {residentForceMaximumULPs=difference;
				residentForceExpected=expected;residentForceObserved=observed;
				residentForceAxis=axis;residentForceFace=face;}
		}
	if( residentForceMetal ) std::cerr << "Resident force lambda=" <<
		residentForceDiagnostics.outwardLambdaPerS << " substeps=" <<
		residentForceGPU.executedViscousSubstepCount << " max_ulp=" <<
		residentForceMaximumULPs << " preflight_ms=" <<
		residentForceDiagnostics.preflightDeviceElapsedMS << " advance_ms=" <<
		residentForceDiagnostics.advanceDeviceElapsedMS << " axis=" << residentForceAxis <<
		" face=" << residentForceFace << " expected=" << residentForceExpected <<
		" observed=" << residentForceObserved << " max_abs=" << residentForceMaximumAbsolute <<
		" max_mag=" << residentForceMaximumMagnitude << '\n';
	bool residentForceSeamsExact=true;
	for( unsigned int axis=0u;axis<3u;++axis ) {
		const std::size_t extent=axis==0u?residentForce.shape.nx:
			(axis==1u?residentForce.shape.ny:residentForce.shape.nz);
		const std::size_t plane=residentForceGPU.momentumKGPerM2S[axis].size()/(extent+1u);
		for( std::size_t line=0u;line<plane;++line ) {
			const std::size_t low=axis==0u?line*(extent+1u):
				(axis==1u?(line/residentForce.shape.nx)*(extent+1u)*residentForce.shape.nx+
					line%residentForce.shape.nx:line);
			const std::size_t high=axis==0u?low+extent:
				(axis==1u?low+extent*residentForce.shape.nx:
					low+extent*residentForce.shape.nx*residentForce.shape.ny);
			residentForceSeamsExact=residentForceSeamsExact&&SameFloatBytes(
				residentForceGPU.momentumKGPerM2S[axis][low],
				residentForceGPU.momentumKGPerM2S[axis][high]);
		}
	}
	Check(residentForceOracle&&residentForceMetalRepeat&&
		residentForceGPU.frozenFields.eddyKinematicViscosityM2PerS==
			residentForceGPURepeat.frozenFields.eddyKinematicViscosityM2PerS&&
		residentForceGPU.frozenFields.effectiveDynamicViscosityPaS==
			residentForceGPURepeat.frozenFields.effectiveDynamicViscosityPaS&&
		residentForceGPU.frozenFields.beginningViscousMomentumRateKGPerM2S2==
			residentForceGPURepeat.frozenFields.beginningViscousMomentumRateKGPerM2S2&&
		residentForceGPU.frozenFields.gravityMomentumIncrementKGPerM2S==
			residentForceGPURepeat.frozenFields.gravityMomentumIncrementKGPerM2S&&
		residentForceGPU.momentumKGPerM2S==residentForceGPURepeat.momentumKGPerM2S&&
		residentForceGPU.intermediateMomentumByteDigests==
			residentForceGPURepeat.intermediateMomentumByteDigests&&
		residentForceDiagnostics.outwardLambdaPerS==residentForceRepeatDiagnostics.outwardLambdaPerS&&
		residentForceGPU.executedViscousSubstepCount==8u&&
		residentForceGPU.intermediateMomentumDigestCount==8u&&
		residentForceDiagnostics.scalarDiagnosticTransferCount==1u&&
		residentForceDiagnostics.substepLoopDeviceToHostTransferCount==0u&&
		residentForceDiagnostics.terminalStagingCount==1u&&
		residentForceDiagnostics.commandCommitCount==3u&&
		residentForceDiagnostics.actualMetalAllocationBytes<=
			residentForceDiagnostics.certifiedWorkingSetBytes&&
		residentForceComposedWithinBound&&residentForceMaximumAbsolute==0x1p-25f&&
		residentForceMaximumULPs>64u&&residentForceSeamsExact,
		"resident eight-substep force keeps the loop private and remains within the measured composed bound");
	std::uint64_t composedCharacterizationCases=0u,composedCharacterizationMaximumULPs=0u;
	float composedCharacterizationMaximumAbsolute=0.0f;
	float composedHighULPMaximumAbsolute=0.0f;
	bool composedCharacterizationWithinBound=true,composedAnalyticBytesExact=true;
	for( const float cv : {0.0f,0.07f} ) for( int phase=0;phase<8;++phase )
		for( int boundaryClass=0;boundaryClass<10;++boundaryClass ) {
			FireProductionFrozenForceRequest characterized=
				ComposedForceCharacterizationCase(boundaryClass,phase,cv);
			FireProductionFrozenForceAdvanceResult preflightCharacterized,metalCharacterized,
				cpuCharacterized;
			FireProductionResidentForceDiagnostics characterizedDiagnostics;
			if( !AdvanceFireProductionFrozenForceMetal(characterized,false,
				preflightCharacterized,characterizedDiagnostics,&error) ) {
				composedCharacterizationWithinBound=false;continue;
			}
			characterized.timeStepS=static_cast<float>(15.0/
				static_cast<double>(characterizedDiagnostics.outwardLambdaPerS));
			if( !AdvanceFireProductionFrozenForceMetal(characterized,false,metalCharacterized,
				characterizedDiagnostics,&error)||metalCharacterized.executedViscousSubstepCount!=8u||
				!AdvanceFireProductionFrozenForceCPU(characterized,
					characterizedDiagnostics.outwardLambdaPerS,cpuCharacterized,&error) ) {
				composedCharacterizationWithinBound=false;continue;
			}
			++composedCharacterizationCases;
			for( unsigned int axis=0u;axis<3u;++axis )
				for( std::size_t face=0u;face<cpuCharacterized.momentumKGPerM2S[axis].size();++face ) {
					const float expected=cpuCharacterized.momentumKGPerM2S[axis][face];
					const float observed=metalCharacterized.momentumKGPerM2S[axis][face];
					const std::uint64_t ulps=FloatULPDistance(expected,observed);
					const float absolute=std::fabs(expected-observed);
					composedCharacterizationMaximumULPs=std::max(
						composedCharacterizationMaximumULPs,ulps);
					composedCharacterizationMaximumAbsolute=std::max(
						composedCharacterizationMaximumAbsolute,absolute);
					if( ulps>64u ) composedHighULPMaximumAbsolute=std::max(
						composedHighULPMaximumAbsolute,absolute);
					composedCharacterizationWithinBound=composedCharacterizationWithinBound&&
						(ulps<=64u||absolute<=0x1p-23f);
					if( expected==0.0f ) composedAnalyticBytesExact=
						composedAnalyticBytesExact&&SameFloatBytes(expected,observed);
				}
		}
	std::cout << "Production resident composed_force_cases=" << composedCharacterizationCases <<
		" max_ulp=" << composedCharacterizationMaximumULPs << " max_abs=" <<
		composedCharacterizationMaximumAbsolute << " high_ulp_max_abs=" <<
		composedHighULPMaximumAbsolute << '\n';
	Check(composedCharacterizationCases==160u&&composedCharacterizationWithinBound&&
		composedAnalyticBytesExact&&composedCharacterizationMaximumULPs==8192u&&
		composedCharacterizationMaximumAbsolute==0x1p-21f&&
		composedHighULPMaximumAbsolute==0x1p-23f,
		"the 160-case N8 boundary/density/Vreman matrix binds the r96 composed force bound");
	FireProductionProjectionRequest stagedProjection;
	stagedProjection.shape=residentForce.shape;stagedProjection.timeStepS=residentForce.timeStepS;
	stagedProjection.ambientDensityKGPerM3=residentForce.ambientDensityKGPerM3;
	stagedProjection.boundary=residentForce.boundary;
	stagedProjection.gasDensityKGPerM3=residentForce.cellGasDensityKGPerM3;
	stagedProjection.provisionalMomentumKGPerM2S=residentForceGPU.momentumKGPerM2S;
	stagedProjection.divergenceTargetPerS.assign(residentForce.shape.CellCount(),0.0f);
	FireProductionProjectionResult stagedProjectionResult;
	const bool stagedProjectionOK=ProjectFireProductionMetal(stagedProjection,
		stagedProjectionResult,&error);
	FireProductionResidentForceProjectionResult residentStep;
	const bool residentStepOK=AdvanceFireProductionForceProjectionMetal(residentForce,
		stagedProjection.divergenceTargetPerS,residentStep,&error);
	const bool residentProjectionMatches=residentStepOK&&stagedProjectionOK&&
		residentStep.projection.faceDensityKGPerM3==stagedProjectionResult.faceDensityKGPerM3&&
		residentStep.projection.velocityMPerS==stagedProjectionResult.velocityMPerS&&
		residentStep.projection.momentumKGPerM2S==stagedProjectionResult.momentumKGPerM2S&&
		residentStep.projection.pressurePa==stagedProjectionResult.pressurePa&&
		residentStep.projection.pressureOpenInflow==stagedProjectionResult.pressureOpenInflow&&
		residentStep.projection.maximumPreProjectionResidualPerS==
			stagedProjectionResult.maximumPreProjectionResidualPerS&&
		residentStep.projection.maximumPostProjectionResidualPerS==
			stagedProjectionResult.maximumPostProjectionResidualPerS&&
		residentStep.projection.validationPassed==stagedProjectionResult.validationPassed;
	Check(residentProjectionMatches&&residentStep.forceSchedule.substepCount==8u&&
		residentStep.forceDiagnostics.scalarDiagnosticTransferCount==1u&&
		residentStep.forceDiagnostics.terminalStagingCount==0u&&
		residentStep.forceDiagnostics.commandCommitCount==2u&&
		residentStep.projection.residentUploadStagingCount==0u&&
		residentStep.projection.residentInterstageDeviceToHostTransferCount==0u&&
		residentStep.projection.residentTerminalStagingCount==1u&&
		residentStep.projection.residentCommandCommitCount==2u&&
		residentStep.forceToProjectionDeviceToHostTransferCount==0u&&
		residentStep.residentProjectionInvocationCount==1u&&
		residentStep.combinedActualMetalAllocationBytes<=
			residentStep.combinedCertifiedWorkingSetBytes&&
		residentStep.combinedActualMetalAllocationBytes<(UINT64_C(1)<<31u),
		"resident force, gravity, and one projection preserve staged-reference bytes without an interstage transfer");
	FireProductionResidentForceProjectionResult rejectedResidentStep;
	rejectedResidentStep.projection.pressurePa.push_back(1.0f);
	rejectedResidentStep.forceSchedule.substepCount=4u;
	rejectedResidentStep.forceToProjectionDeviceToHostTransferCount=4u;
	rejectedResidentStep.residentProjectionInvocationCount=4u;
	rejectedResidentStep.combinedCertifiedWorkingSetBytes=4u;
	rejectedResidentStep.combinedActualMetalAllocationBytes=4u;
	setenv("RISE_FIRE_FORCE_TEST_FAILURE","resident-interstage-transfer",1);
	const bool rejectedResidentTransfer=!AdvanceFireProductionForceProjectionMetal(residentForce,
		stagedProjection.divergenceTargetPerS,rejectedResidentStep,&error);
	unsetenv("RISE_FIRE_FORCE_TEST_FAILURE");
	Check(rejectedResidentTransfer&&rejectedResidentStep.projection.pressurePa.empty()&&
		rejectedResidentStep.forceSchedule.substepCount==0u&&
		rejectedResidentStep.forceToProjectionDeviceToHostTransferCount==0u&&
		rejectedResidentStep.residentProjectionInvocationCount==0u&&
		rejectedResidentStep.combinedCertifiedWorkingSetBytes==0u&&
		rejectedResidentStep.combinedActualMetalAllocationBytes==0u&&
		error.find("interstage transfer observed")!=std::string::npos,
		"the observed transfer seam fails closed when an interstage host access is injected");
	FireProductionFrozenForceRequest matrixForce=forceRest;
	matrixForce.shape.nx=4u;matrixForce.shape.ny=4u;matrixForce.shape.nz=4u;
	matrixForce.shape.cellWidthM=1.0f;matrixForce.timeStepS=1.0f;
	matrixForce.vremanCoefficient=0.0f;matrixForce.gravityMPerS2.fill(0.0f);
	matrixForce.boundary.fill(FireProductionProjectionPeriodic);
	matrixForce.cellGasDensityKGPerM3.assign(matrixForce.shape.CellCount(),1.2f);
	matrixForce.molecularKinematicViscosityM2PerS.assign(matrixForce.shape.CellCount(),0.01f);
	std::size_t matrixFaces=0u;std::array<std::size_t,3> matrixOffsets={};
	for( unsigned int axis=0u;axis<3u;++axis ) {
		matrixOffsets[axis]=matrixFaces;
		const std::size_t count=FireProductionProjectionFaceCount(matrixForce.shape,axis);
		matrixFaces+=count;matrixForce.faceDensityKGPerM3[axis].assign(count,1.2f);
		matrixForce.beginningMomentumKGPerM2S[axis].assign(count,0.0f);
	}
	std::vector<double> exactRowSums(matrixFaces,0.0);
	bool exactMatrixBuilt=true;
	for( unsigned int inputAxis=0u;inputAxis<3u;++inputAxis ) {
		const std::size_t count=matrixForce.beginningMomentumKGPerM2S[inputAxis].size();
		for( std::size_t inputFace=0u;inputFace<count;++inputFace ) {
			std::size_t x=0u,y=0u,z=0u;
			if( inputAxis==0u ) {x=inputFace%(matrixForce.shape.nx+1u);
				const std::size_t rest=inputFace/(matrixForce.shape.nx+1u);
				y=rest%matrixForce.shape.ny;z=rest/matrixForce.shape.ny;}
			if( inputAxis==1u ) {x=inputFace%matrixForce.shape.nx;
				const std::size_t rest=inputFace/matrixForce.shape.nx;
				y=rest%(matrixForce.shape.ny+1u);z=rest/(matrixForce.shape.ny+1u);}
			if( inputAxis==2u ) {x=inputFace%matrixForce.shape.nx;
				const std::size_t rest=inputFace/matrixForce.shape.nx;
				y=rest%matrixForce.shape.ny;z=rest/matrixForce.shape.ny;}
			const std::size_t coordinate=inputAxis==0u?x:(inputAxis==1u?y:z);
			const std::size_t extent=inputAxis==0u?matrixForce.shape.nx:
				(inputAxis==1u?matrixForce.shape.ny:matrixForce.shape.nz);
			if( coordinate==extent ) continue;
			matrixForce.beginningMomentumKGPerM2S[inputAxis][inputFace]=1.0f;
			std::size_t duplicate=inputFace;
			if( coordinate==0u ) duplicate=inputAxis==0u?inputFace+extent:
				(inputAxis==1u?inputFace+extent*matrixForce.shape.nx:
					inputFace+extent*matrixForce.shape.nx*matrixForce.shape.ny);
			matrixForce.beginningMomentumKGPerM2S[inputAxis][duplicate]=1.0f;
			FireProductionFrozenForceResult column;
			exactMatrixBuilt=exactMatrixBuilt&&BuildFireProductionFrozenForceFieldsCPU(
				matrixForce,column,&error);
			if( exactMatrixBuilt ) for( unsigned int outputAxis=0u;outputAxis<3u;++outputAxis )
				for( std::size_t outputFace=0u;outputFace<column.beginningViscousMomentumRateKGPerM2S2[outputAxis].size();++outputFace )
					exactRowSums[matrixOffsets[outputAxis]+outputFace]+=std::fabs(
						static_cast<double>(column.beginningViscousMomentumRateKGPerM2S2[outputAxis][outputFace]));
			matrixForce.beginningMomentumKGPerM2S[inputAxis][inputFace]=0.0f;
			matrixForce.beginningMomentumKGPerM2S[inputAxis][duplicate]=0.0f;
		}
	}
	const double exactMaximumRowSum=exactMatrixBuilt?
		*std::max_element(exactRowSums.begin(),exactRowSums.end()):
		std::numeric_limits<double>::infinity();
	FireProductionFrozenForceAdvanceResult matrixForceGPU;
	FireProductionResidentForceDiagnostics matrixForceDiagnostics;
	const bool matrixEnvelopeBuilt=AdvanceFireProductionFrozenForceMetal(
		matrixForce,false,matrixForceGPU,matrixForceDiagnostics,&error);
	std::cout << "Production resident force exact_row_max=" << exactMaximumRowSum <<
		" outward_lambda=" << matrixForceDiagnostics.outwardLambdaPerS << '\n';
	Check(matrixEnvelopeBuilt&&exactMatrixBuilt&&exactMaximumRowSum>0.0&&
		exactMaximumRowSum<=static_cast<double>(matrixForceDiagnostics.outwardLambdaPerS),
		"matrix-free outward envelope dominates every independently assembled signed row");
	FireProductionFrozenForceRequest rejectedResidentForce=matrixForce;
	rejectedResidentForce.cellGasDensityKGPerM3[0]=-1.0f;
	FireProductionFrozenForceAdvanceResult rejectedResidentResult;
	rejectedResidentResult.momentumKGPerM2S[0].push_back(1.0f);
	rejectedResidentResult.intermediateMomentumByteDigests.fill(5u);
	rejectedResidentResult.intermediateMomentumDigestCount=5u;
	rejectedResidentResult.executedViscousSubstepCount=5u;
	FireProductionResidentForceDiagnostics rejectedResidentDiagnostics;
	rejectedResidentDiagnostics.outwardLambdaPerS=1.0f;
	rejectedResidentDiagnostics.commandCommitCount=5u;
	Check(!AdvanceFireProductionFrozenForceMetal(rejectedResidentForce,true,
		rejectedResidentResult,rejectedResidentDiagnostics,&error)&&
		FrozenForceAdvanceResultEmpty(rejectedResidentResult)&&
		rejectedResidentDiagnostics.outwardLambdaPerS==0.0f&&
		rejectedResidentDiagnostics.commandCommitCount==0u,
		"resident force rejects malformed input with a fully default result and transfer ledger");
	const char* residentFailureStages[]={"resident-snapshot","resident-command-allocation",
		"resident-command","resident-staging","resident-staging-command"};
	bool residentLateFailuresAtomic=true;
	for( const char* stage:residentFailureStages ) {
		FireProductionFrozenForceAdvanceResult failed;failed.schedule.substepCount=7u;
		failed.schedule.substepTimeS=1.0f;failed.schedule.outwardWork=2.0;
		failed.schedule.representedProductUpper=2.0;
		failed.frozenFields.eddyKinematicViscosityM2PerS.push_back(1.0f);
		for( unsigned int axis=0u;axis<3u;++axis ) {
			failed.frozenFields.beginningViscousMomentumRateKGPerM2S2[axis].push_back(1.0f);
			failed.frozenFields.gravityMomentumIncrementKGPerM2S[axis].push_back(1.0f);
			failed.momentumKGPerM2S[axis].push_back(1.0f);
		}
		failed.intermediateMomentumByteDigests.fill(3u);
		failed.intermediateMomentumDigestCount=3u;failed.executedViscousSubstepCount=3u;
		FireProductionResidentForceDiagnostics failedDiagnostics;
		failedDiagnostics.outwardLambdaPerS=1.0f;failedDiagnostics.preflightDeviceElapsedMS=1.0;
		failedDiagnostics.advanceDeviceElapsedMS=1.0;failedDiagnostics.certifiedWorkingSetBytes=1u;
		failedDiagnostics.actualMetalAllocationBytes=1u;failedDiagnostics.commandCommitCount=1u;
		failedDiagnostics.scalarDiagnosticTransferCount=1u;
		failedDiagnostics.substepLoopDeviceToHostTransferCount=1u;
		failedDiagnostics.terminalStagingCount=1u;
		setenv("RISE_FIRE_FORCE_TEST_FAILURE",stage,1);
		const bool rejected=!AdvanceFireProductionFrozenForceMetal(matrixForce,true,
			failed,failedDiagnostics,&error);
		unsetenv("RISE_FIRE_FORCE_TEST_FAILURE");
		residentLateFailuresAtomic=residentLateFailuresAtomic&&rejected&&
			FrozenForceAdvanceResultEmpty(failed)&&failedDiagnostics.outwardLambdaPerS==0.0f&&
			failedDiagnostics.preflightDeviceElapsedMS==0.0&&
			failedDiagnostics.advanceDeviceElapsedMS==0.0&&
			failedDiagnostics.certifiedWorkingSetBytes==0u&&
			failedDiagnostics.actualMetalAllocationBytes==0u&&
			failedDiagnostics.commandCommitCount==0u&&
			failedDiagnostics.scalarDiagnosticTransferCount==0u&&
			failedDiagnostics.substepLoopDeviceToHostTransferCount==0u&&
			failedDiagnostics.terminalStagingCount==0u;
	}
	Check(residentLateFailuresAtomic,
		"every post-preflight resident-force failure publishes a fully default result and ledger");
	FireProductionFrozenForceRequest tier10ResidentForce;
	tier10ResidentForce.shape.nx=86u;tier10ResidentForce.shape.ny=86u;
	tier10ResidentForce.shape.nz=132u;tier10ResidentForce.shape.cellWidthM=0.30f/86.0f;
	tier10ResidentForce.timeStepS=1.0f/480.0f;
	tier10ResidentForce.ambientDensityKGPerM3=1.2f;tier10ResidentForce.vremanCoefficient=0.0f;
	tier10ResidentForce.gravityMPerS2.fill(0.0f);
	tier10ResidentForce.boundary.fill(FireProductionProjectionPeriodic);
	const float tier10N8Nu=(15.0f/tier10ResidentForce.timeStepS)*
		tier10ResidentForce.shape.cellWidthM*tier10ResidentForce.shape.cellWidthM/24.0f;
	tier10ResidentForce.cellGasDensityKGPerM3.assign(
		tier10ResidentForce.shape.CellCount(),1.2f);
	tier10ResidentForce.molecularKinematicViscosityM2PerS.assign(
		tier10ResidentForce.shape.CellCount(),tier10N8Nu);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		const std::size_t count=FireProductionProjectionFaceCount(tier10ResidentForce.shape,axis);
		tier10ResidentForce.faceDensityKGPerM3[axis].assign(count,1.2f);
		tier10ResidentForce.beginningMomentumKGPerM2S[axis].assign(count,0.0f);
	}
	std::vector<double> residentForceDeviceMS;
	for( unsigned int trial=0u;trial<5u;++trial ) {
		FireProductionFrozenForceAdvanceResult timedResult;
		FireProductionResidentForceDiagnostics timedDiagnostics;
		if( AdvanceFireProductionFrozenForceMetal(tier10ResidentForce,false,timedResult,
			timedDiagnostics,&error)&&timedResult.executedViscousSubstepCount==8u&&
			timedDiagnostics.substepLoopDeviceToHostTransferCount==0u )
			residentForceDeviceMS.push_back(timedDiagnostics.preflightDeviceElapsedMS+
				timedDiagnostics.advanceDeviceElapsedMS);
	}
	std::sort(residentForceDeviceMS.begin(),residentForceDeviceMS.end());
	const double residentForceDeviceP95=residentForceDeviceMS.size()==5u?
		residentForceDeviceMS.back():std::numeric_limits<double>::infinity();
	FireProductionFrozenForceRequest tier10ResidentForceN7=tier10ResidentForce;
	tier10ResidentForceN7.timeStepS*=6.5f/7.5f;
	FireProductionFrozenForceAdvanceResult tier10N7Result,tier10N9Result;
	FireProductionResidentForceDiagnostics tier10N7Diagnostics,tier10N9Diagnostics;
	const bool tier10N7=AdvanceFireProductionFrozenForceMetal(tier10ResidentForceN7,false,
		tier10N7Result,tier10N7Diagnostics,&error)&&
		tier10N7Result.executedViscousSubstepCount==7u;
	FireProductionFrozenForceRequest tier10ResidentForceN9=tier10ResidentForce;
	tier10ResidentForceN9.timeStepS*=8.5f/7.5f;
	const bool tier10N9=!AdvanceFireProductionFrozenForceMetal(tier10ResidentForceN9,false,
		tier10N9Result,tier10N9Diagnostics,&error)&&FrozenForceAdvanceResultEmpty(tier10N9Result)&&
		tier10N9Diagnostics.commandCommitCount==0u&&
		tier10N9Diagnostics.outwardLambdaPerS==0.0f&&
		tier10N9Diagnostics.terminalStagingCount==0u;
	std::cout << "Production resident force N8 device_p95_ms=" << residentForceDeviceP95 <<
		" lambda=" << (residentForceDeviceMS.empty()?0.0f:tier10N7Diagnostics.outwardLambdaPerS) << '\n';
	Check(std::isfinite(residentForceDeviceP95)&&residentForceDeviceP95>0.0&&
		residentForceDeviceP95<=20.0&&tier10N7&&tier10N9,
		"tier-10-shaped force preflight and exact eight-update edge fit twenty milliseconds");
	std::vector<float> tier10ProjectionTarget(tier10ResidentForce.shape.CellCount(),0.0f);
	std::vector<double> residentCombinedDeviceMS,residentCombinedWallMS;
	bool residentCombinedExactZero=true,residentCombinedTopology=true;
	FireProductionResidentForceProjectionResult tier10ResidentStep;
	for( unsigned int trial=0u;trial<5u;++trial ) {
		const auto beginning=std::chrono::steady_clock::now();
		const bool advanced=AdvanceFireProductionForceProjectionMetal(tier10ResidentForce,
			tier10ProjectionTarget,tier10ResidentStep,&error);
		const double wallMS=std::chrono::duration<double,std::milli>(
			std::chrono::steady_clock::now()-beginning).count();
		if( !advanced ) {std::cerr << "Resident combined detail: " << error << '\n';
			residentCombinedTopology=false;continue;}
		residentCombinedDeviceMS.push_back(tier10ResidentStep.forceDiagnostics.preflightDeviceElapsedMS+
			tier10ResidentStep.forceDiagnostics.advanceDeviceElapsedMS+
			tier10ResidentStep.projection.deviceElapsedMS);
		residentCombinedWallMS.push_back(wallMS);
		residentCombinedTopology=residentCombinedTopology&&
			tier10ResidentStep.forceSchedule.substepCount==8u&&
			tier10ResidentStep.forceDiagnostics.scalarDiagnosticTransferCount==1u&&
			tier10ResidentStep.forceToProjectionDeviceToHostTransferCount==0u&&
			tier10ResidentStep.projection.residentUploadStagingCount==0u&&
			tier10ResidentStep.projection.residentInterstageDeviceToHostTransferCount==0u&&
			tier10ResidentStep.projection.residentTerminalStagingCount==1u&&
			tier10ResidentStep.residentProjectionInvocationCount==1u;
		for( const float pressure:tier10ResidentStep.projection.pressurePa )
			residentCombinedExactZero=residentCombinedExactZero&&SameFloatBytes(pressure,0.0f);
		for( unsigned int axis=0u;axis<3u;++axis ) {
			for( const float momentum:tier10ResidentStep.projection.momentumKGPerM2S[axis] )
				residentCombinedExactZero=residentCombinedExactZero&&SameFloatBytes(momentum,0.0f);
			for( const float velocity:tier10ResidentStep.projection.velocityMPerS[axis] )
				residentCombinedExactZero=residentCombinedExactZero&&SameFloatBytes(velocity,0.0f);
		}
	}
	std::sort(residentCombinedDeviceMS.begin(),residentCombinedDeviceMS.end());
	std::sort(residentCombinedWallMS.begin(),residentCombinedWallMS.end());
	const double residentCombinedDeviceP95=residentCombinedDeviceMS.size()==5u?
		residentCombinedDeviceMS.back():std::numeric_limits<double>::infinity();
	const double residentCombinedWallP95=residentCombinedWallMS.size()==5u?
		residentCombinedWallMS.back():std::numeric_limits<double>::infinity();
	std::cout << "Production resident N8 force-projection device_p95_ms=" <<
		residentCombinedDeviceP95 << " wall_p95_ms=" << residentCombinedWallP95 <<
		" certified_bytes=" << tier10ResidentStep.combinedCertifiedWorkingSetBytes <<
		" actual_bytes=" << tier10ResidentStep.combinedActualMetalAllocationBytes << '\n';
	Check(residentCombinedTopology&&residentCombinedExactZero&&
		residentCombinedDeviceMS.size()==5u&&residentCombinedWallMS.size()==5u&&
		std::isfinite(residentCombinedDeviceP95)&&residentCombinedDeviceP95>0.0&&
		std::isfinite(residentCombinedWallP95)&&residentCombinedWallP95>0.0&&
		residentCombinedDeviceP95<=140.0&&residentCombinedWallP95<=160.0&&
		tier10ResidentStep.combinedActualMetalAllocationBytes<=
			tier10ResidentStep.combinedCertifiedWorkingSetBytes&&
		tier10ResidentStep.combinedActualMetalAllocationBytes<(UINT64_C(1)<<31u),
		"tier-10 N8 force and one projection remain resident, exact at rest, and fit their combined allocation");
	if( !periodicForceMetal ) std::cerr << "Metal frozen-force detail: " << error << '\n';
	auto reportForceDifference=[](const char* label,
		const FireProductionFrozenForceResult& cpu,
		const FireProductionFrozenForceResult& gpu) {
		std::uint64_t maximum=0u;float expected=0.0f,observed=0.0f;
		const char* field="none";std::size_t index=0u;
		auto scan=[&](const char* name,const std::vector<float>& a,const std::vector<float>& b) {
			for( std::size_t i=0u;i<std::min(a.size(),b.size());++i ) {
				const std::uint64_t difference=FloatULPDistance(a[i],b[i]);
				if( difference>maximum ) {maximum=difference;field=name;index=i;expected=a[i];observed=b[i];}
			}
		};
		scan("eddy",cpu.eddyKinematicViscosityM2PerS,gpu.eddyKinematicViscosityM2PerS);
		scan("mu",cpu.effectiveDynamicViscosityPaS,gpu.effectiveDynamicViscosityPaS);
		const char* viscousNames[]={"viscous-x","viscous-y","viscous-z"};
		const char* gravityNames[]={"gravity-x","gravity-y","gravity-z"};
		for( unsigned int axis=0u;axis<3u;++axis ) {scan(viscousNames[axis],
			cpu.beginningViscousMomentumRateKGPerM2S2[axis],
			gpu.beginningViscousMomentumRateKGPerM2S2[axis]);scan(gravityNames[axis],
			cpu.gravityMomentumIncrementKGPerM2S[axis],gpu.gravityMomentumIncrementKGPerM2S[axis]);}
		std::cerr << label << " force max_ulp=" << maximum << " field=" << field <<
			" index=" << index << " expected=" << expected << " observed=" << observed << '\n';
	};
	if( periodicForceMetal&&!SameFrozenForceWithinULP(
		periodicForceResult,periodicForceGPU,forceMetalMaximumULPs) )
		{reportForceDifference("periodic",periodicForceResult,periodicForceGPU);
		std::cerr << "periodic low cpu=" << periodicForceResult.beginningViscousMomentumRateKGPerM2S2[0][periodicLow]
			<< " gpu=" << periodicForceGPU.beginningViscousMomentumRateKGPerM2S2[0][periodicLow] << '\n';}
	if( mixedForceMetal&&!SameFrozenForceWithinULP(
		mixedInitialFields,mixedForceGPU,forceMetalMaximumULPs) )
		reportForceDifference("mixed",mixedInitialFields,mixedForceGPU);
	if( authorityForceMetal&&!SameFrozenForceWithinULP(
		authorityForceResult,authorityForceGPU,forceMetalMaximumULPs) )
		reportForceDifference("authority",authorityForceResult,authorityForceGPU);
	Check(periodicForceMetal&&periodicForceMetalRepeat&&
		SameFrozenForceWithinULP(periodicForceResult,periodicForceGPU,
			forceMetalMaximumULPs)&&
		SameFrozenForceWithinULP(periodicForceGPU,periodicForceGPURepeat,0u)&&
		SameFloatBytes(periodicForceGPU.beginningViscousMomentumRateKGPerM2S2[0][periodicHigh],
			periodicForceGPU.beginningViscousMomentumRateKGPerM2S2[0][periodicLow])&&
		SameFloatBytes(periodicForceGPU.gravityMomentumIncrementKGPerM2S[0][periodicHigh],
			periodicForceGPU.gravityMomentumIncrementKGPerM2S[0][periodicLow])&&
		std::isfinite(periodicForceMetalMS)&&periodicForceMetalMS>0.0,
		"Metal frozen-force periodic field matches the CPU oracle and exact seam publication");
	Check(mixedForceMetal&&SameFrozenForceWithinULP(
		mixedInitialFields,mixedForceGPU,forceMetalMaximumULPs)&&
		mixedForceGPU.beginningViscousMomentumRateKGPerM2S2[0][highWallShearFace]==
			mixedInitialFields.beginningViscousMomentumRateKGPerM2S2[0][highWallShearFace]&&
		mixedForceGPU.gravityMomentumIncrementKGPerM2S[1][upperWallGravityFace]==0.0f&&
		mixedForceGPU.gravityMomentumIncrementKGPerM2S[1][lowerOpenGravityFace]==0.125f,
		"Metal frozen-force distinguishes upper wall prescription from lower open ownership");
	Check(authorityForceMetal&&SameFrozenForceWithinULP(
		authorityForceResult,authorityForceGPU,forceMetalMaximumULPs)&&
		authorityForceGPU.effectiveDynamicViscosityPaS[authorityCell]==
			authorityForceResult.effectiveDynamicViscosityPaS[authorityCell],
		"Metal frozen-force uses the authoritative cell density and strict Vreman mu bytes");
	bool restForceExact=restForceMetal&&SameFrozenForceWithinULP(
		forceRestResult,restForceGPU,forceMetalMaximumULPs)&&SameFloatVectorBytes(
			forceRestResult.eddyKinematicViscosityM2PerS,
			restForceGPU.eddyKinematicViscosityM2PerS);
	for( unsigned int axis=0u;axis<3u;++axis ) {
		restForceExact=restForceExact&&SameFloatVectorBytes(
			forceRestResult.beginningViscousMomentumRateKGPerM2S2[axis],
			restForceGPU.beginningViscousMomentumRateKGPerM2S2[axis])&&
			SameFloatVectorBytes(forceRestResult.gravityMomentumIncrementKGPerM2S[axis],
				restForceGPU.gravityMomentumIncrementKGPerM2S[axis]);
	}
	Check(restForceExact,
		"Metal zero-flow witness publishes exact-zero Vreman, stress divergence, and gravity");
	Check(compressionForceMetal&&SameFrozenForceWithinULP(
		compressionForceResult,compressionForceGPU,forceMetalMaximumULPs)&&
		compressionForceGPU.beginningViscousMomentumRateKGPerM2S2[0][compressionFace]==
			compressionForceResult.beginningViscousMomentumRateKGPerM2S2[0][compressionFace],
		"Metal compression binds the strict two-thirds deviatoric coefficient bytes");
	bool everyMetalBoundaryOrientation=true;
	for( unsigned int axis=0u;axis<3u;++axis ) for( unsigned int wallSide=0u;
		wallSide<2u;++wallSide ) {
		FireProductionFrozenForceRequest oriented=forceRest;
		oriented.boundary.fill(FireProductionProjectionPressureOpen);
		oriented.boundary[2u*axis+wallSide]=FireProductionProjectionWall;
		oriented.ambientDensityKGPerM3=1.0f;oriented.timeStepS=0.125f;
		oriented.gravityMPerS2.fill(0.0f);oriented.gravityMPerS2[axis]=1.0f;
		std::fill(oriented.cellGasDensityKGPerM3.begin(),
			oriented.cellGasDensityKGPerM3.end(),1.0f);
		std::fill(oriented.molecularKinematicViscosityM2PerS.begin(),
			oriented.molecularKinematicViscosityM2PerS.end(),0.015625f);
		for( unsigned int component=0u;component<3u;++component ) {
			std::fill(oriented.faceDensityKGPerM3[component].begin(),
				oriented.faceDensityKGPerM3[component].end(),2.0f);
			std::fill(oriented.beginningMomentumKGPerM2S[component].begin(),
				oriented.beginningMomentumKGPerM2S[component].end(),0.0f);
		}
		const unsigned int tangential=(axis+1u)%3u;
		std::fill(oriented.beginningMomentumKGPerM2S[tangential].begin(),
			oriented.beginningMomentumKGPerM2S[tangential].end(),2.0f);
		const std::size_t xFaces=oriented.shape.nx+(axis==0u?1u:0u);
		const std::size_t yFaces=oriented.shape.ny+(axis==1u?1u:0u);
		const std::size_t zFaces=oriented.shape.nz+(axis==2u?1u:0u);
		for( std::size_t z=0u;z<zFaces;++z ) for( std::size_t y=0u;y<yFaces;++y )
			for( std::size_t x=0u;x<xFaces;++x ) {
				const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?oriented.shape.nx:
					(axis==1u?oriented.shape.ny:oriented.shape.nz);
				if( coordinate!=0u&&coordinate!=extent ) continue;
				const bool isWall=(coordinate==0u?0u:1u)==wallSide;
				oriented.beginningMomentumKGPerM2S[axis][TransportFaceIndex(
					oriented.shape,axis,x,y,z)]=isWall?14.0f:1.0f;
			}
		std::size_t wallXYZ[]={2u,2u,2u},openXYZ[]={2u,2u,2u},shearXYZ[]={2u,2u,2u};
		const std::size_t axisExtent=axis==0u?oriented.shape.nx:
			(axis==1u?oriented.shape.ny:oriented.shape.nz);
		wallXYZ[axis]=wallSide==0u?0u:axisExtent;
		openXYZ[axis]=wallSide==0u?axisExtent:0u;
		shearXYZ[axis]=wallSide==0u?0u:axisExtent-1u;
		const std::size_t wallFace=TransportFaceIndex(oriented.shape,axis,
			wallXYZ[0],wallXYZ[1],wallXYZ[2]);
		const std::size_t openFace=TransportFaceIndex(oriented.shape,axis,
			openXYZ[0],openXYZ[1],openXYZ[2]);
		const std::size_t shearFace=TransportFaceIndex(oriented.shape,tangential,
			shearXYZ[0],shearXYZ[1],shearXYZ[2]);
		FireProductionFrozenForceResult orientedCPU,orientedGPU;double orientedMS=0.0;
		error.clear();
		everyMetalBoundaryOrientation=everyMetalBoundaryOrientation&&
			BuildFireProductionFrozenForceFieldsCPU(oriented,orientedCPU,&error)&&
			BuildFireProductionFrozenForceFieldsMetal(oriented,orientedGPU,orientedMS,&error)&&
			SameFrozenForceWithinULP(orientedCPU,orientedGPU,forceMetalMaximumULPs)&&
			SameFloatBytes(orientedCPU.gravityMomentumIncrementKGPerM2S[axis][wallFace],
				0.0f)&&SameFloatBytes(
				orientedGPU.gravityMomentumIncrementKGPerM2S[axis][wallFace],0.0f)&&
			orientedCPU.gravityMomentumIncrementKGPerM2S[axis][openFace]==0.125f&&
			orientedGPU.gravityMomentumIncrementKGPerM2S[axis][openFace]==0.125f&&
			orientedCPU.beginningViscousMomentumRateKGPerM2S2[tangential][shearFace]!=0.0f;
	}
	Check(everyMetalBoundaryOrientation,
		"Metal frozen force covers lower and upper wall/open ownership on all three axes");
	bool everyMetalPeriodicAxis=true;
	for( unsigned int axis=0u;axis<3u;++axis ) {
		FireProductionFrozenForceRequest oriented=forceRest;
		oriented.boundary.fill(FireProductionProjectionPeriodic);
		oriented.shape.cellWidthM=1.0f;oriented.timeStepS=0.125f;
		oriented.ambientDensityKGPerM3=1.0f;oriented.gravityMPerS2.fill(0.0f);
		oriented.gravityMPerS2[axis]=1.0f;
		std::fill(oriented.cellGasDensityKGPerM3.begin(),
			oriented.cellGasDensityKGPerM3.end(),1.0f);
		std::fill(oriented.molecularKinematicViscosityM2PerS.begin(),
			oriented.molecularKinematicViscosityM2PerS.end(),1.0f);
		for( unsigned int component=0u;component<3u;++component ) {
			std::fill(oriented.faceDensityKGPerM3[component].begin(),
				oriented.faceDensityKGPerM3[component].end(),1.0f);
			std::fill(oriented.beginningMomentumKGPerM2S[component].begin(),
				oriented.beginningMomentumKGPerM2S[component].end(),0.0f);
		}
		std::fill(oriented.faceDensityKGPerM3[axis].begin(),
			oriented.faceDensityKGPerM3[axis].end(),2.0f);
		const std::size_t axisExtent=axis==0u?oriented.shape.nx:
			(axis==1u?oriented.shape.ny:oriented.shape.nz);
		for( std::size_t z=0u;z<oriented.shape.nz;++z )
			for( std::size_t y=0u;y<oriented.shape.ny;++y )
				for( std::size_t x=0u;x<oriented.shape.nx;++x ) {
					std::size_t xyz[]={x,y,z};
					const std::size_t first=TransportFaceIndex(oriented.shape,axis,
						x,y,z);oriented.beginningMomentumKGPerM2S[axis][first]=
						xyz[axis]==0u||xyz[axis]==2u?2.0f:0.0f;
				}
		for( std::size_t z=0u;z<oriented.shape.nz;++z )
			for( std::size_t y=0u;y<oriented.shape.ny;++y )
				for( std::size_t x=0u;x<oriented.shape.nx;++x ) {
					std::size_t lowCopyXYZ[]={x,y,z},highCopyXYZ[]={x,y,z};
					if( lowCopyXYZ[axis]!=0u ) continue;
					highCopyXYZ[axis]=axisExtent;
					const std::size_t lowCopy=TransportFaceIndex(oriented.shape,axis,
						lowCopyXYZ[0],lowCopyXYZ[1],lowCopyXYZ[2]);
					const std::size_t highCopy=TransportFaceIndex(oriented.shape,axis,
						highCopyXYZ[0],highCopyXYZ[1],highCopyXYZ[2]);
					oriented.beginningMomentumKGPerM2S[axis][highCopy]=
						oriented.beginningMomentumKGPerM2S[axis][lowCopy];
				}
		std::size_t lowXYZ[]={2u,2u,2u},highXYZ[]={2u,2u,2u};
		lowXYZ[axis]=0u;highXYZ[axis]=axisExtent;
		const std::size_t low=TransportFaceIndex(oriented.shape,axis,
			lowXYZ[0],lowXYZ[1],lowXYZ[2]);
		const std::size_t high=TransportFaceIndex(oriented.shape,axis,
			highXYZ[0],highXYZ[1],highXYZ[2]);
		FireProductionFrozenForceResult orientedCPU,orientedGPU;double orientedMS=0.0;
		error.clear();
		const bool orientedBuilt=BuildFireProductionFrozenForceFieldsCPU(
			oriented,orientedCPU,&error)&&BuildFireProductionFrozenForceFieldsMetal(
				oriented,orientedGPU,orientedMS,&error);
		bool seamBytesExact=orientedBuilt;
		if( orientedBuilt ) for( std::size_t z=0u;z<oriented.shape.nz;++z )
			for( std::size_t y=0u;y<oriented.shape.ny;++y )
				for( std::size_t x=0u;x<oriented.shape.nx;++x ) {
					std::size_t lowSeamXYZ[]={x,y,z},highSeamXYZ[]={x,y,z};
					if( lowSeamXYZ[axis]!=0u ) continue;
					highSeamXYZ[axis]=axisExtent;
					const std::size_t lowSeam=TransportFaceIndex(oriented.shape,axis,
						lowSeamXYZ[0],lowSeamXYZ[1],lowSeamXYZ[2]);
					const std::size_t highSeam=TransportFaceIndex(oriented.shape,axis,
						highSeamXYZ[0],highSeamXYZ[1],highSeamXYZ[2]);
					seamBytesExact=seamBytesExact&&SameFloatBytes(
						orientedGPU.beginningViscousMomentumRateKGPerM2S2[axis][lowSeam],
						orientedGPU.beginningViscousMomentumRateKGPerM2S2[axis][highSeam])&&
						SameFloatBytes(orientedGPU.gravityMomentumIncrementKGPerM2S[axis][lowSeam],
							orientedGPU.gravityMomentumIncrementKGPerM2S[axis][highSeam]);
				}
		everyMetalPeriodicAxis=everyMetalPeriodicAxis&&orientedBuilt&&seamBytesExact&&
			SameFrozenForceWithinULP(orientedCPU,orientedGPU,forceMetalMaximumULPs)&&
			orientedCPU.beginningViscousMomentumRateKGPerM2S2[axis][low]!=0.0f&&
			SameFloatBytes(orientedGPU.beginningViscousMomentumRateKGPerM2S2[axis][high],
				orientedGPU.beginningViscousMomentumRateKGPerM2S2[axis][low])&&
			orientedGPU.gravityMomentumIncrementKGPerM2S[axis][low]==0.125f&&
			SameFloatBytes(orientedGPU.gravityMomentumIncrementKGPerM2S[axis][high],
				orientedGPU.gravityMomentumIncrementKGPerM2S[axis][low]);
	}
	Check(everyMetalPeriodicAxis,
		"Metal periodic force publishes nonzero viscous and gravity seams exactly on every axis");
	FireProductionFrozenForceRequest malformedForceMetal=forceRest;
	malformedForceMetal.boundary[0]=FireProductionProjectionPeriodic;
	FireProductionFrozenForceResult rejectedForceMetal;
	SeedRejectedFrozenForce(rejectedForceMetal);double rejectedForceMetalMS=7.0;error.clear();
	Check(!BuildFireProductionFrozenForceFieldsMetal(malformedForceMetal,rejectedForceMetal,
		rejectedForceMetalMS,&error)&&FrozenForceResultEmpty(rejectedForceMetal)&&
		rejectedForceMetalMS==0.0&&error.find("pairing")!=std::string::npos,
		"Metal frozen-force structural rejection publishes no field or timing payload");
	malformedForceMetal=forceRest;
	malformedForceMetal.boundary.fill(FireProductionProjectionPeriodic);
	malformedForceMetal.beginningMomentumKGPerM2S[0][0]=0.0f;
	malformedForceMetal.beginningMomentumKGPerM2S[0][forceRest.shape.nx]=-0.0f;
	SeedRejectedFrozenForce(rejectedForceMetal);rejectedForceMetalMS=7.0;error.clear();
	Check(!BuildFireProductionFrozenForceFieldsMetal(malformedForceMetal,rejectedForceMetal,
		rejectedForceMetalMS,&error)&&FrozenForceResultEmpty(rejectedForceMetal)&&
		rejectedForceMetalMS==0.0&&error.find("seam")!=std::string::npos,
		"Metal periodic validation rejects byte-distinct signed-zero input seams");
	FireProductionFrozenForceRequest forceMetalAdmission;
	forceMetalAdmission.shape=forceMetalAbove;forceMetalAdmission.shape.cellWidthM=1.0f;
	forceMetalAdmission.timeStepS=1.0f;
	SeedRejectedFrozenForce(rejectedForceMetal);rejectedForceMetalMS=7.0;error.clear();
	const bool forceMetalOverRejected=!BuildFireProductionFrozenForceFieldsMetal(
		forceMetalAdmission,rejectedForceMetal,rejectedForceMetalMS,&error)&&
		FrozenForceResultEmpty(rejectedForceMetal)&&rejectedForceMetalMS==0.0&&
		error.find("two GiB")!=std::string::npos;
	forceMetalAdmission.shape=forceMetalBelow;forceMetalAdmission.shape.cellWidthM=1.0f;
	SeedRejectedFrozenForce(rejectedForceMetal);rejectedForceMetalMS=7.0;error.clear();
	const bool forceMetalUnderContinues=!BuildFireProductionFrozenForceFieldsMetal(
		forceMetalAdmission,rejectedForceMetal,rejectedForceMetalMS,&error)&&
		FrozenForceResultEmpty(rejectedForceMetal)&&rejectedForceMetalMS==0.0&&
		error.find("cell shape")!=std::string::npos&&error.find("two GiB")==std::string::npos;
	Check(forceMetalOverRejected&&forceMetalUnderContinues,
		"actual Metal force admission straddles the rounded two-GiB certificate");
	bool everyForceMetalFailureAtomic=true;
	const char* forceMetalFailureStages[]={"buffer","command-allocation","encoder","command","output"};
	for( const char* stage : forceMetalFailureStages ) {
		SeedRejectedFrozenForce(rejectedForceMetal);rejectedForceMetalMS=7.0;error.clear();
		setenv("RISE_FIRE_FORCE_TEST_FAILURE",stage,1);
		const bool rejected=!BuildFireProductionFrozenForceFieldsMetal(
			periodicForce,rejectedForceMetal,rejectedForceMetalMS,&error);
		unsetenv("RISE_FIRE_FORCE_TEST_FAILURE");
		everyForceMetalFailureAtomic=everyForceMetalFailureAtomic&&rejected&&
			FrozenForceResultEmpty(rejectedForceMetal)&&rejectedForceMetalMS==0.0&&
			!error.empty();
	}
	Check(everyForceMetalFailureAtomic,
		"buffer, command-allocation, encoder, completed-command, and output failures publish no Metal force payload");
	SeedRejectedFrozenForce(rejectedForceMetal);rejectedForceMetalMS=7.0;
	denyTestAllocations=true;
	const bool forceMetalAllocationRejected=!BuildFireProductionFrozenForceFieldsMetal(
		periodicForce,rejectedForceMetal,rejectedForceMetalMS,0);
	denyTestAllocations=false;
	Check(forceMetalAllocationRejected&&FrozenForceResultEmpty(rejectedForceMetal)&&
		rejectedForceMetalMS==0.0,
		"persistent allocation denial cannot escape the Metal force API boundary");
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
	const std::string forceSource=ReadText(
		"src/Library/Utilities/FireProductionForce.cpp");
	const std::string forceMetalSource=ReadText(
		"src/Library/Utilities/FireProductionForceMac.mm");
	const std::size_t forceAdvanceBeginning=forceSource.find(
		"bool AdvanceFireProductionFrozenForceCPU(");
	const std::string forceAdvanceBody=forceAdvanceBeginning==std::string::npos?
		std::string():forceSource.substr(forceAdvanceBeginning);
	const std::size_t forceMetalBeginning=forceMetalSource.find(
		"bool BuildFireProductionFrozenForceFieldsMetal(");
	const std::string forceMetalBody=forceMetalBeginning==std::string::npos?
		std::string():forceMetalSource.substr(forceMetalBeginning);
	const std::size_t forceResourceBeginning=forceMetalBody.find(
		"const id<MTLBuffer> buffers[]=");
	const std::size_t forceResourceEnd=forceMetalBody.find(
		"if( actual>certifiedBytes",forceResourceBeginning);
	const std::string forceResourceBody=forceResourceBeginning==std::string::npos||
		forceResourceEnd==std::string::npos?std::string():forceMetalBody.substr(
			forceResourceBeginning,forceResourceEnd-forceResourceBeginning);
	const std::size_t residentForceBeginning=forceMetalSource.find(
		"bool AdvanceFireProductionFrozenForceMetalImpl(");
	const std::size_t residentForceEnd=forceMetalSource.find(
		"bool AdvanceFireProductionFrozenForceMetal(",residentForceBeginning);
	const std::string residentForceBody=residentForceBeginning==std::string::npos||
		residentForceEnd==std::string::npos?std::string():forceMetalSource.substr(
			residentForceBeginning,residentForceEnd-residentForceBeginning);
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
	const std::string makeFilelist=ReadText("build/make/rise/Filelist");
	const std::string xcodeProject=ReadText("build/XCode/rise/rise.xcodeproj/project.pbxproj");
	const std::string androidRules=ReadText("build/cmake/rise-android/CMakeLists.txt");
	const std::string androidSources=ReadText("build/cmake/rise-android/rise_sources.cmake");
	const std::string visualStudioProject=ReadText("build/VS2022/Library/Library.vcxproj");
	const std::string visualStudioFilters=ReadText(
		"build/VS2022/Library/Library.vcxproj.filters");
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
	Check(!forceAdvanceBody.empty()&&!forceMetalBody.empty()&&!forceResourceBody.empty()&&
		CountSubstring(forceAdvanceBody,"publishBoundaries();")==3u&&
		CountSubstring(forceAdvanceBody,"MomentumByteDigest(")==1u&&
		forceSource.find("alphaSquared+=alpha*alpha;")!=std::string::npos&&
		forceSource.find("std::max(0.0f,rawBBeta)")!=std::string::npos&&
		forceSource.find("std::nextafter((static_cast<double>(timeStepS)*")!=
			std::string::npos&&
		forceSource.find("std::nextafter(static_cast<double>(substep)*\n"
			"\t\t\t\tstatic_cast<double>(outwardLambdaPerS),infinity);")!=
			std::string::npos&&
		forceSource.find("if( count>8u )")!=std::string::npos&&
		forceSource.find("bytes=(16u*cells+5u*allFaces)*sizeof(float);")!=
			std::string::npos&&
		forceSource.find("leftXYZ[component]=normal==0u?normalExtent-1u:normal-1u;")!=
			std::string::npos&&
		forceSource.find("computed.beginningViscousMomentumRateKGPerM2S2[component][high]=\n"
			"\t\t\t\t\t\t\t\tcomputed.beginningViscousMomentumRateKGPerM2S2[component][low];")!=
			std::string::npos&&
		makeFilelist.find("$(PATHLIBRARY)Utilities/FireProductionForce.cpp")!=
			std::string::npos&&
		androidSources.find("${RISE_LIB}/Utilities/FireProductionForce.cpp")!=
			std::string::npos&&
		visualStudioProject.find("<ClInclude Include=\"..\\..\\..\\src\\Library\\Utilities\\"
			"FireProductionForce.h\" />")!=std::string::npos&&
		visualStudioFilters.find("FireProductionForce.cpp")!=std::string::npos&&
		visualStudioFilters.find("FireProductionForce.h")!=std::string::npos&&
		makeRules.find("$(PATHLIBRARY)Utilities/FireProductionForce.o : "
			"$(PATHLIBRARY)Utilities/FireProductionForce.cpp\n"
			"\t@echo \"Compiling (safe fp32): $<\"\n"
			"\t@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) "
			"-fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) "
			"$(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@")!=std::string::npos&&
		androidRules.find("\"${RISE_LIB}/Utilities/FireProductionForce.cpp\"\n"
			"    PROPERTIES COMPILE_OPTIONS \"-fno-fast-math;-ffp-contract=off\"")!=
			std::string::npos&&
		visualStudioProject.find("<ClCompile Include=\"..\\..\\..\\src\\Library\\Utilities\\"
			"FireProductionForce.cpp\">\n      <FloatingPointModel>Strict"
			"</FloatingPointModel>")!=std::string::npos&&
		CountSubstring(xcodeProject,
			"FireProductionForce.cpp in Sources */ = {isa = PBXBuildFile;")==2u&&
		CountSubstring(xcodeProject,
			"FireProductionForce.cpp */; settings = {COMPILER_FLAGS = \"-fno-fast-math "
			"-ffp-contract=off\"; };")==2u&&
		CountSubstring(xcodeProject,"FireProductionForce.h in Headers")==2u&&
		forceMetalSource.find("MTLMathModeSafe")!=std::string::npos&&
		forceMetalSource.find("MTLMathModeFast")==std::string::npos&&
		forceMetalSource.find("fast::")==std::string::npos&&
		forceMetalSource.find("BuildFireProductionFrozenForceFieldsCPU")==std::string::npos&&
		forceMetalSource.find("pipeline(\"setup_face_velocity\")")!=std::string::npos&&
		forceMetalSource.find("pipeline(\"build_cell_velocity\")")!=std::string::npos&&
		forceMetalSource.find("pipeline(\"build_stress\")")!=std::string::npos&&
		forceMetalSource.find("pipeline(\"build_face_force\")")!=std::string::npos&&
		forceMetalSource.find(
			"beta[i][j]+=widthSquared*gradient[m][i]*gradient[m][j];")!=
			std::string::npos&&
		forceMetalSource.find("for(uint m=0u;m<3u;++m)for(uint i=0u;i<3u;++i)")!=
			std::string::npos&&
		forceMetalSource.find("(c==d?(2.0f/3.0f)*divergence:0.0f)")!=
			std::string::npos&&
		forceMetalBody.find("const id<MTLBuffer> buffers[]={rho,nu,faceRho,momentum,"
			"faceVelocity,cellVelocity,\n\t\t\t\t\tstress,eddy,mu,viscous,gravity,parameters};")!=
			std::string::npos&&
		forceMetalBody.find("sizeof(buffers)/sizeof(buffers[0])==12u")!=
			std::string::npos&&
		CountSubstring(forceResourceBody,"for( id<MTLBuffer> buffer : buffers )")==1u&&
		CountSubstring(forceResourceBody,"[buffer allocatedSize]")==1u&&
		forceResourceBody.find("for( id<MTLBuffer> buffer : buffers ) {\n"
			"\t\t\t\t\tconst std::uint64_t allocated=[buffer allocatedSize];\n"
			"\t\t\t\t\tif( actual>std::numeric_limits<std::uint64_t>::max()-allocated )\n"
			"\t\t\t\t\t\treturn Fail(error,\"production frozen-force Metal allocation overflowed\");\n"
			"\t\t\t\t\tactual+=allocated;\n\t\t\t\t}")!=std::string::npos&&
		forceMetalSource.find("actual>certifiedBytes")!=std::string::npos&&
		forceMetalBody.find("InjectedFailure(\"command-allocation\")")!=std::string::npos&&
		forceMetalBody.find("InjectedFailure(\"encoder\")")!=std::string::npos&&
		forceMetalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		makeFilelist.find("$(PATHLIBRARY)Utilities/FireProductionForceMac.mm")!=
			std::string::npos&&makeFilelist.find(
			"$(PATHLIBRARY)Utilities/FireProductionForceUnsupported.cpp")!=std::string::npos&&
		makeRules.find("Utilities/FireProductionForceMac.o : "
			"$(PATHLIBRARY)Utilities/FireProductionForceMac.mm\n"
			"\t@echo \"Compiling (safe fp32 ObjC++): $<\"\n"
			"\t@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) "
			"-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		androidSources.find("${RISE_LIB}/Utilities/FireProductionForceUnsupported.cpp")!=
			std::string::npos&&visualStudioProject.find(
			"FireProductionForceUnsupported.cpp\" />")!=std::string::npos&&
		visualStudioFilters.find("FireProductionForceUnsupported.cpp")!=std::string::npos&&
		CountSubstring(xcodeProject,
			"FireProductionForceMac.mm in Sources */ = {isa = PBXBuildFile;")==2u&&
		CountSubstring(xcodeProject,
			"FireProductionForceMac.mm */; settings = {COMPILER_FLAGS = \"-fno-fast-math "
			"-ffp-contract=off\"; };")==2u,
		"production force primitives bind their arithmetic topology and all five strict build "
		"surfaces");
	Check(!residentForceBody.empty()&&
		CountSubstring(residentForceBody,"CommitResidentForceCommand(")==3u&&
		CountSubstring(residentForceBody,"ResidentForceBufferContents(")==8u&&
		CountSubstring(residentForceBody,"[preflight commit]")==0u&&
		CountSubstring(residentForceBody,"[advance commit]")==0u&&
		CountSubstring(residentForceBody,"[staging commit]")==0u&&
		CountSubstring(residentForceBody," contents]")==0u&&
		residentForceBody.find("residentForceInterstageFullGridReadCount-"
			"beginningInterstageReads")!=std::string::npos&&
		residentForceBody.find("ProjectFireProductionMetalResident(")!=std::string::npos,
		"resident force observes every command/read seam and invokes one private-buffer projection");
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
	FireProductionFrozenForceResult unsupportedForce;
	SeedRejectedFrozenForce(unsupportedForce);double unsupportedForceMS=9.0;error.clear();
	Check(!BuildFireProductionFrozenForceFieldsMetal(forceRest,unsupportedForce,
		unsupportedForceMS,&error)&&FrozenForceResultEmpty(unsupportedForce)&&
		unsupportedForceMS==0.0&&!error.empty(),
		"non-Metal frozen-force seam fails explicitly without a silent CPU fallback");
	FireProductionFrozenForceAdvanceResult unsupportedResidentForce;
	unsupportedResidentForce.momentumKGPerM2S[0].push_back(1.0f);
	unsupportedResidentForce.intermediateMomentumByteDigests.fill(7u);
	unsupportedResidentForce.intermediateMomentumDigestCount=7u;
	unsupportedResidentForce.executedViscousSubstepCount=7u;
	FireProductionResidentForceDiagnostics unsupportedResidentDiagnostics;
	unsupportedResidentDiagnostics.outwardLambdaPerS=1.0f;
	unsupportedResidentDiagnostics.commandCommitCount=7u;
	error.clear();
	Check(!AdvanceFireProductionFrozenForceMetal(forceRest,true,unsupportedResidentForce,
		unsupportedResidentDiagnostics,&error)&&
		FrozenForceAdvanceResultEmpty(unsupportedResidentForce)&&
		unsupportedResidentDiagnostics.outwardLambdaPerS==0.0f&&
		unsupportedResidentDiagnostics.commandCommitCount==0u&&!error.empty(),
		"non-Metal resident force seam fails explicitly without a silent CPU fallback");
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
