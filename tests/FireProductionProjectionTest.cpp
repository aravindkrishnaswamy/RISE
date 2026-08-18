//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjectionTest.cpp - production P2 manufactured gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireProductionProjection.h"
#include "../tools/fire_simulator_core.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <string>

namespace
{
	bool denyTestAllocations=false;
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
	int failures=0;

	void Check( bool condition, const char* message )
	{
		if( !condition ) {std::cerr << "FAIL: " << message << '\n';++failures;}
	}

	std::size_t Cell( const RISE::FireProductionProjectionShape& shape,
		std::size_t x,std::size_t y,std::size_t z )
	{
		return (z*shape.ny+y)*shape.nx+x;
	}

	std::size_t Face( const RISE::FireProductionProjectionShape& shape,
		unsigned int axis,std::size_t x,std::size_t y,std::size_t z )
	{
		if( axis==0u ) return (z*shape.ny+y)*(shape.nx+1u)+x;
		if( axis==1u ) return (z*(shape.ny+1u)+y)*shape.nx+x;
		return (z*shape.ny+y)*shape.nx+x;
	}

	RISE::FireProductionProjectionRequest EmptyRequest( std::size_t nx,
		std::size_t ny,std::size_t nz )
	{
		RISE::FireProductionProjectionRequest request;
		request.shape.nx=nx;request.shape.ny=ny;request.shape.nz=nz;
		request.shape.cellWidthM=0.1f;request.timeStepS=0.01f;
		request.ambientDensityKGPerM3=1.0f;
		request.gasDensityKGPerM3.assign(request.shape.CellCount(),1.0f);
		request.divergenceTargetPerS.assign(request.shape.CellCount(),0.0f);
		for( unsigned int axis=0;axis<3u;++axis ) request.provisionalMomentumKGPerM2S[axis].
			assign(RISE::FireProductionProjectionFaceCount(request.shape,axis),0.0f);
		return request;
	}

	void SetBoundary( RISE::FireProductionProjectionRequest& request,
		RISE::FireProductionProjectionBoundary kind )
	{
		request.boundary.fill(kind);
	}

	float IndependentFaceDensity( const RISE::FireProductionProjectionRequest& request,
		unsigned int axis,std::size_t x,std::size_t y,std::size_t z )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
		const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
		const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		if( coordinate>0u&&coordinate<extent ) {
			if( axis==0u ) --lowX;if( axis==1u ) --lowY;if( axis==2u ) --lowZ;
			return 0.5f*request.gasDensityKGPerM3[Cell(shape,lowX,lowY,lowZ)]+
				0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
		}
		const unsigned int side=2u*axis+(coordinate?1u:0u);
		if( request.boundary[side]==RISE::FireProductionProjectionPeriodic ) {
			if( axis==0u ) {lowX=shape.nx-1u;highX=0u;}
			if( axis==1u ) {lowY=shape.ny-1u;highY=0u;}
			if( axis==2u ) {lowZ=shape.nz-1u;highZ=0u;}
			return 0.5f*request.gasDensityKGPerM3[Cell(shape,lowX,lowY,lowZ)]+
				0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
		}
		if( axis==0u ) highX=coordinate?shape.nx-1u:0u;
		if( axis==1u ) highY=coordinate?shape.ny-1u:0u;
		if( axis==2u ) highZ=coordinate?shape.nz-1u:0u;
		return request.boundary[side]==RISE::FireProductionProjectionPressureOpen?
			0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)]+
			0.5f*request.ambientDensityKGPerM3:
			request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
	}

	bool EveryZero( const RISE::FireProductionProjectionResult& result )
	{
		if( !std::all_of(result.pressurePa.begin(),result.pressurePa.end(),
			[](float value){return value==0.0f;}) ) return false;
		for( unsigned int axis=0;axis<3u;++axis )
			if( !std::all_of(result.velocityMPerS[axis].begin(),result.velocityMPerS[axis].end(),
				[](float value){return value==0.0f;}) ) return false;
		return true;
	}

	std::string ReadFile( const char* path )
	{
		std::ifstream input(path,std::ios::binary);
		std::ostringstream bytes;bytes << input.rdbuf();return bytes.str();
	}

	std::size_t Count( const std::string& text, const std::string& needle )
	{
		std::size_t count=0u,position=0u;
		while( (position=text.find(needle,position))!=std::string::npos ) {
			++count;position+=needle.size();
		}
		return count;
	}

	float IndependentResidual( const RISE::FireProductionProjectionRequest& request,
		const RISE::FireProductionProjectionResult& result )
	{
		float maximum=0.0f;
		for( std::size_t z=0;z<request.shape.nz;++z )
			for( std::size_t y=0;y<request.shape.ny;++y )
				for( std::size_t x=0;x<request.shape.nx;++x ) {
					const float residual=(result.velocityMPerS[0][Face(request.shape,0u,x+1u,y,z)]-
						result.velocityMPerS[0][Face(request.shape,0u,x,y,z)]+
						result.velocityMPerS[1][Face(request.shape,1u,x,y+1u,z)]-
						result.velocityMPerS[1][Face(request.shape,1u,x,y,z)]+
						result.velocityMPerS[2][Face(request.shape,2u,x,y,z+1u)]-
						result.velocityMPerS[2][Face(request.shape,2u,x,y,z)])/
						request.shape.cellWidthM-request.divergenceTargetPerS[Cell(request.shape,x,y,z)];
					maximum=std::max(maximum,std::fabs(residual));
				}
		return maximum;
	}

	bool EveryPeriodicSeamExact( const RISE::FireProductionProjectionShape& shape,
		const RISE::FireProductionProjectionResult& result )
	{
		for( unsigned int axis=0;axis<3u;++axis ) {
			const std::size_t firstCount=axis==0u?shape.ny:shape.nx;
			const std::size_t secondCount=axis==2u?shape.ny:shape.nz;
			for( std::size_t second=0;second<secondCount;++second )
				for( std::size_t first=0;first<firstCount;++first ) {
					std::size_t x0=0u,y0=0u,z0=0u,x1=0u,y1=0u,z1=0u;
					if( axis==0u ) {x1=shape.nx;y0=y1=first;z0=z1=second;}
					if( axis==1u ) {y1=shape.ny;x0=x1=first;z0=z1=second;}
					if( axis==2u ) {z1=shape.nz;x0=x1=first;y0=y1=second;}
					const std::size_t low=Face(shape,axis,x0,y0,z0);
					const std::size_t high=Face(shape,axis,x1,y1,z1);
					if( result.faceDensityKGPerM3[axis][low]!=result.faceDensityKGPerM3[axis][high]||
						result.momentumKGPerM2S[axis][low]!=result.momentumKGPerM2S[axis][high]||
						result.velocityMPerS[axis][low]!=result.velocityMPerS[axis][high] ) return false;
				}
		}
		return true;
	}

	struct DoubleLevel
	{
		std::size_t nx,ny,nz;
		double spacing[3];
		std::vector<double> density,rhs,pressure,temporary,residual,diagonal;
		std::array<std::vector<double>,3> beta;
	};

	std::size_t DoubleCell( const DoubleLevel& level,
		std::size_t x,std::size_t y,std::size_t z )
	{
		return (z*level.ny+y)*level.nx+x;
	}

	std::size_t DoubleFace( const DoubleLevel& level,unsigned int axis,
		std::size_t x,std::size_t y,std::size_t z )
	{
		if( axis==0u ) return (z*level.ny+y)*(level.nx+1u)+x;
		if( axis==1u ) return (z*(level.ny+1u)+y)*level.nx+x;
		return (z*level.ny+y)*level.nx+x;
	}

	void BuildDoubleWallCoefficients( DoubleLevel& level )
	{
		level.beta[0].assign((level.nx+1u)*level.ny*level.nz,0.0);
		level.beta[1].assign(level.nx*(level.ny+1u)*level.nz,0.0);
		level.beta[2].assign(level.nx*level.ny*(level.nz+1u),0.0);
		for( std::size_t z=0;z<level.nz;++z ) for( std::size_t y=0;y<level.ny;++y )
			for( std::size_t x=1u;x<level.nx;++x ) level.beta[0][DoubleFace(
				level,0u,x,y,z)]=1.0/(0.5*level.density[DoubleCell(level,x-1u,y,z)]+
				0.5*level.density[DoubleCell(level,x,y,z)]);
		for( std::size_t z=0;z<level.nz;++z ) for( std::size_t y=1u;y<level.ny;++y )
			for( std::size_t x=0;x<level.nx;++x ) level.beta[1][DoubleFace(
				level,1u,x,y,z)]=1.0/(0.5*level.density[DoubleCell(level,x,y-1u,z)]+
				0.5*level.density[DoubleCell(level,x,y,z)]);
		for( std::size_t z=1u;z<level.nz;++z ) for( std::size_t y=0;y<level.ny;++y )
			for( std::size_t x=0;x<level.nx;++x ) level.beta[2][DoubleFace(
				level,2u,x,y,z)]=1.0/(0.5*level.density[DoubleCell(level,x,y,z-1u)]+
				0.5*level.density[DoubleCell(level,x,y,z)]);
		level.diagonal.assign(level.nx*level.ny*level.nz,0.0);
		for( std::size_t z=0;z<level.nz;++z ) for( std::size_t y=0;y<level.ny;++y )
			for( std::size_t x=0;x<level.nx;++x ) {
				double value=0.0;
				for( unsigned int axis=0;axis<3u;++axis ) {
					std::size_t hx=x,hy=y,hz=z;
					if( axis==0u ) ++hx;if( axis==1u ) ++hy;if( axis==2u ) ++hz;
					const double scale=1.0/(level.spacing[axis]*level.spacing[axis]);
					value+=level.beta[axis][DoubleFace(level,axis,x,y,z)]*scale;
					value+=level.beta[axis][DoubleFace(level,axis,hx,hy,hz)]*scale;
				}
				level.diagonal[DoubleCell(level,x,y,z)]=value;
			}
	}

	void ApplyDoubleWallOperator( const DoubleLevel& level,
		const std::vector<double>& pressure,std::vector<double>& output )
	{
		output.assign(level.nx*level.ny*level.nz,0.0);
		for( std::size_t z=0;z<level.nz;++z ) for( std::size_t y=0;y<level.ny;++y )
			for( std::size_t x=0;x<level.nx;++x ) {
				const std::size_t cell=DoubleCell(level,x,y,z);double value=0.0;
				const std::size_t coordinate[3]={x,y,z};
				const std::size_t extent[3]={level.nx,level.ny,level.nz};
				for( unsigned int axis=0;axis<3u;++axis ) {
					const double scale=1.0/(level.spacing[axis]*level.spacing[axis]);
					if( coordinate[axis]>0u ) {
						std::size_t px=x,py=y,pz=z;
						if( axis==0u ) --px;if( axis==1u ) --py;if( axis==2u ) --pz;
						value+=level.beta[axis][DoubleFace(level,axis,x,y,z)]*scale*
							(pressure[cell]-pressure[DoubleCell(level,px,py,pz)]);
					}
					if( coordinate[axis]+1u<extent[axis] ) {
						std::size_t px=x,py=y,pz=z,hx=x,hy=y,hz=z;
						if( axis==0u ) {++px;++hx;}if( axis==1u ) {++py;++hy;}
						if( axis==2u ) {++pz;++hz;}
						value+=level.beta[axis][DoubleFace(level,axis,hx,hy,hz)]*scale*
							(pressure[cell]-pressure[DoubleCell(level,px,py,pz)]);
					}
				}
				output[cell]=value;
			}
	}

	double DoubleTreeSum( const std::vector<double>& values )
	{
		std::size_t width=1u;while( width<values.size() ) width<<=1u;
		std::vector<double> scratch(width,0.0);
		std::copy(values.begin(),values.end(),scratch.begin());
		for( std::size_t offset=1u;offset<width;offset<<=1u )
			for( std::size_t index=2u*offset-1u;index<width;index+=2u*offset )
				scratch[index]+=scratch[index-offset];
		return scratch.back();
	}

	void RemoveDoubleMean( std::vector<double>& values )
	{
		const double mean=DoubleTreeSum(values)/static_cast<double>(values.size());
		for( double& value : values ) value-=mean;
	}

	void SmoothDoubleWall( DoubleLevel& level,unsigned int sweeps )
	{
		const double omega=static_cast<double>(2.0f/3.0f);
		for( unsigned int sweep=0;sweep<sweeps;++sweep ) {
			ApplyDoubleWallOperator(level,level.pressure,level.temporary);
			for( std::size_t cell=0;cell<level.pressure.size();++cell )
				level.residual[cell]=level.pressure[cell]+omega*
					(level.rhs[cell]-level.temporary[cell])/level.diagonal[cell];
			level.pressure.swap(level.residual);RemoveDoubleMean(level.pressure);
		}
	}

	std::size_t DoubleCoarseCoordinate( std::size_t fine,std::size_t fineExtent,
		std::size_t coarseExtent )
	{
		return coarseExtent==fineExtent?fine:std::min(coarseExtent-1u,fine/2u);
	}

	void RestrictDouble( const DoubleLevel& fine,DoubleLevel& coarse,
		const std::vector<double>& values,std::vector<double>& result )
	{
		result.assign(coarse.nx*coarse.ny*coarse.nz,0.0);
		std::vector<unsigned int> count(result.size(),0u);
		for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
			for( std::size_t x=0;x<fine.nx;++x ) {
				const std::size_t cell=DoubleCell(coarse,DoubleCoarseCoordinate(x,fine.nx,
					coarse.nx),DoubleCoarseCoordinate(y,fine.ny,coarse.ny),
					DoubleCoarseCoordinate(z,fine.nz,coarse.nz));
				result[cell]+=values[DoubleCell(fine,x,y,z)];++count[cell];
			}
		for( std::size_t cell=0;cell<result.size();++cell )
			result[cell]/=static_cast<double>(count[cell]);
	}

	void DoubleInterpolation( std::size_t fine,std::size_t fineExtent,
		std::size_t coarseExtent,std::size_t& first,std::size_t& second,double& weight )
	{
		if( fineExtent==coarseExtent ) {first=second=fine;weight=0.0;return;}
		const double position=(static_cast<double>(fine)+0.5)*
			static_cast<double>(coarseExtent)/static_cast<double>(fineExtent)-0.5;
		if( position<=0.0 ) {first=second=0u;weight=0.0;return;}
		if( position>=static_cast<double>(coarseExtent-1u) ) {
			first=second=coarseExtent-1u;weight=0.0;return;
		}
		first=static_cast<std::size_t>(std::floor(position));second=first+1u;
		weight=position-static_cast<double>(first);
	}

	void ProlongateDouble( const DoubleLevel& coarse,DoubleLevel& fine )
	{
		for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
			for( std::size_t x=0;x<fine.nx;++x ) {
				std::size_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;
				double wx=0.0,wy=0.0,wz=0.0;
				DoubleInterpolation(x,fine.nx,coarse.nx,x0,x1,wx);
				DoubleInterpolation(y,fine.ny,coarse.ny,y0,y1,wy);
				DoubleInterpolation(z,fine.nz,coarse.nz,z0,z1,wz);
				double value=0.0;
				for( unsigned int iz=0;iz<2u;++iz ) for( unsigned int iy=0;iy<2u;++iy )
					for( unsigned int ix=0;ix<2u;++ix ) value+=(ix?wx:1.0-wx)*
						(iy?wy:1.0-wy)*(iz?wz:1.0-wz)*coarse.pressure[DoubleCell(
							coarse,ix?x1:x0,iy?y1:y0,iz?z1:z0)];
				fine.pressure[DoubleCell(fine,x,y,z)]+=value;
			}
	}

	void DoubleVCycle( std::vector<DoubleLevel>& levels,std::size_t levelIndex )
	{
		DoubleLevel& level=levels[levelIndex];
		if( levelIndex+1u==levels.size() ) {SmoothDoubleWall(level,32u);return;}
		SmoothDoubleWall(level,3u);ApplyDoubleWallOperator(level,level.pressure,level.temporary);
		for( std::size_t cell=0;cell<level.rhs.size();++cell )
			level.residual[cell]=level.rhs[cell]-level.temporary[cell];
		DoubleLevel& coarse=levels[levelIndex+1u];
		RestrictDouble(level,coarse,level.residual,coarse.rhs);RemoveDoubleMean(coarse.rhs);
		std::fill(coarse.pressure.begin(),coarse.pressure.end(),0.0);
		DoubleVCycle(levels,levelIndex+1u);ProlongateDouble(coarse,level);
		SmoothDoubleWall(level,3u);
	}

	double IndependentFixed12WallResidual(
		const RISE::FireProductionProjectionRequest& request )
	{
		DoubleLevel fine;fine.nx=request.shape.nx;fine.ny=request.shape.ny;
		fine.nz=request.shape.nz;fine.spacing[0]=request.shape.cellWidthM;
		fine.spacing[1]=request.shape.cellWidthM;fine.spacing[2]=request.shape.cellWidthM;
		fine.density.assign(request.gasDensityKGPerM3.begin(),request.gasDensityKGPerM3.end());
		BuildDoubleWallCoefficients(fine);const std::size_t cells=fine.nx*fine.ny*fine.nz;
		fine.rhs.assign(cells,0.0);fine.pressure.assign(cells,0.0);
		fine.temporary.assign(cells,0.0);fine.residual.assign(cells,0.0);
		for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
			for( std::size_t x=0;x<fine.nx;++x ) {
				double divergence=0.0;
				for( unsigned int axis=0;axis<3u;++axis ) {
					std::size_t hx=x,hy=y,hz=z;if( axis==0u ) ++hx;
					if( axis==1u ) ++hy;if( axis==2u ) ++hz;
					const std::size_t low=DoubleFace(fine,axis,x,y,z);
					const std::size_t high=DoubleFace(fine,axis,hx,hy,hz);
					const double lowVelocity=fine.beta[axis][low]==0.0?0.0:
						static_cast<double>(request.provisionalMomentumKGPerM2S[axis][low])*
						fine.beta[axis][low];
					const double highVelocity=fine.beta[axis][high]==0.0?0.0:
						static_cast<double>(request.provisionalMomentumKGPerM2S[axis][high])*
						fine.beta[axis][high];
					divergence+=(highVelocity-lowVelocity)/fine.spacing[axis];
				}
				const std::size_t cell=DoubleCell(fine,x,y,z);
				fine.rhs[cell]=-(divergence-request.divergenceTargetPerS[cell])/
					static_cast<double>(request.timeStepS);
			}
		RemoveDoubleMean(fine.rhs);std::vector<DoubleLevel> levels;levels.push_back(fine);
		while( levels.back().nx>4u||levels.back().ny>4u||levels.back().nz>4u ) {
			const DoubleLevel& parent=levels.back();DoubleLevel coarse;
			coarse.nx=parent.nx>4u?(parent.nx+1u)/2u:parent.nx;
			coarse.ny=parent.ny>4u?(parent.ny+1u)/2u:parent.ny;
			coarse.nz=parent.nz>4u?(parent.nz+1u)/2u:parent.nz;
			coarse.spacing[0]=parent.spacing[0]*parent.nx/coarse.nx;
			coarse.spacing[1]=parent.spacing[1]*parent.ny/coarse.ny;
			coarse.spacing[2]=parent.spacing[2]*parent.nz/coarse.nz;
			RestrictDouble(parent,coarse,parent.density,coarse.density);
			BuildDoubleWallCoefficients(coarse);const std::size_t count=coarse.nx*coarse.ny*coarse.nz;
			coarse.rhs.assign(count,0.0);coarse.pressure.assign(count,0.0);
			coarse.temporary.assign(count,0.0);coarse.residual.assign(count,0.0);
			levels.push_back(coarse);
		}
		for( unsigned int cycle=0;cycle<12u;++cycle ) {
			DoubleVCycle(levels,0u);RemoveDoubleMean(levels[0].pressure);
		}
		double maximum=0.0;
		for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
			for( std::size_t x=0;x<fine.nx;++x ) {
				double divergence=0.0;
				for( unsigned int axis=0;axis<3u;++axis ) {
					std::size_t hx=x,hy=y,hz=z;if( axis==0u ) ++hx;
					if( axis==1u ) ++hy;if( axis==2u ) ++hz;
					auto corrected=[&](std::size_t fx,std::size_t fy,std::size_t fz){
						const std::size_t face=DoubleFace(fine,axis,fx,fy,fz);
						if( fine.beta[axis][face]==0.0 ) return 0.0;
						std::size_t lx=fx,ly=fy,lz=fz;
						if( axis==0u ) --lx;if( axis==1u ) --ly;if( axis==2u ) --lz;
						const double gradient=(levels[0].pressure[DoubleCell(fine,fx,fy,fz)]-
							levels[0].pressure[DoubleCell(fine,lx,ly,lz)])/fine.spacing[axis];
						return (static_cast<double>(request.provisionalMomentumKGPerM2S[axis][face])-
							static_cast<double>(request.timeStepS)*gradient)*fine.beta[axis][face];};
					const double low=corrected(x,y,z);const double high=corrected(hx,hy,hz);
					divergence+=(high-low)/fine.spacing[axis];
				}
				maximum=std::max(maximum,std::fabs(divergence-
					request.divergenceTargetPerS[DoubleCell(fine,x,y,z)]));
			}
		return maximum;
	}
}

int main()
{
	using namespace RISE;
	std::string error;
	for( const FireProductionProjectionBoundary boundary : {
		FireProductionProjectionPeriodic,FireProductionProjectionWall,
		FireProductionProjectionPressureOpen} ) {
		FireProductionProjectionRequest rest=EmptyRequest(9u,7u,5u);
		SetBoundary(rest,boundary);
		FireProductionProjectionResult result;
		Check(ProjectFireProductionCPU(rest,result,&error)&&EveryZero(result)&&
			result.validationPassed&&result.maximumPreProjectionResidualPerS==0.0f&&
			result.maximumPostProjectionResidualPerS==0.0f,
			"P2 periodic, wall, and pressure-open rest states are byte exact");
	}

	FireProductionProjectionRequest sinusoid=EmptyRequest(16u,8u,4u);
	SetBoundary(sinusoid,FireProductionProjectionPeriodic);
	const float pi=static_cast<float>(std::acos(-1.0));
	std::vector<float> sinusoidPressure(sinusoid.shape.CellCount(),0.0f);
	for( std::size_t z=0;z<sinusoid.shape.nz;++z )
		for( std::size_t y=0;y<sinusoid.shape.ny;++y )
			for( std::size_t x=0;x<sinusoid.shape.nx;++x )
				sinusoidPressure[Cell(sinusoid.shape,x,y,z)]=std::sin(2.0f*pi*
					(static_cast<float>(x)+0.5f)/static_cast<float>(sinusoid.shape.nx));
	for( std::size_t z=0;z<sinusoid.shape.nz;++z )
		for( std::size_t y=0;y<sinusoid.shape.ny;++y )
			for( std::size_t x=0;x<=sinusoid.shape.nx;++x ) {
				const std::size_t low=x==0u||x==sinusoid.shape.nx?sinusoid.shape.nx-1u:x-1u;
				const std::size_t high=x==0u||x==sinusoid.shape.nx?0u:x;
				sinusoid.provisionalMomentumKGPerM2S[0][Face(sinusoid.shape,0u,x,y,z)]=
					sinusoid.timeStepS*(sinusoidPressure[Cell(sinusoid.shape,high,y,z)]-
					sinusoidPressure[Cell(sinusoid.shape,low,y,z)])/sinusoid.shape.cellWidthM;
			}
	FireProductionProjectionResult sinusoidResult;
	Check(ProjectFireProductionCPU(sinusoid,sinusoidResult,&error)&&
		sinusoidResult.validationPassed&&sinusoidResult.maximumPostProjectionResidualPerS<=5.0e-5f,
		"P2 constant-density sinusoidal pressure is recovered within the fixed schedule");

	FireProductionProjectionRequest densityBytes=EmptyRequest(4u,4u,4u);
	densityBytes.ambientDensityKGPerM3=0.04f;
	densityBytes.boundary={{FireProductionProjectionPeriodic,FireProductionProjectionPeriodic,
		FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
		FireProductionProjectionWall,FireProductionProjectionWall}};
	for( std::size_t z=0;z<densityBytes.shape.nz;++z )
		for( std::size_t y=0;y<densityBytes.shape.ny;++y )
			for( std::size_t x=0;x<densityBytes.shape.nx;++x )
				densityBytes.gasDensityKGPerM3[Cell(densityBytes.shape,x,y,z)]=
					x==0u?0.01f:(x==1u?0.02f:(x==2u?0.08f:0.16f));
	for( std::size_t z=0;z<densityBytes.shape.nz;++z )
		for( std::size_t y=0;y<densityBytes.shape.ny;++y )
			for( std::size_t x=0;x<=densityBytes.shape.nx;++x )
				densityBytes.provisionalMomentumKGPerM2S[0][Face(
					densityBytes.shape,0u,x,y,z)]=IndependentFaceDensity(
						densityBytes,0u,x,y,z)*0.1f;
	FireProductionProjectionResult densityBytesResult;
	const bool densityBytesOK=ProjectFireProductionCPU(densityBytes,densityBytesResult,&error);
	Check(densityBytesOK&&
		densityBytesResult.faceDensityKGPerM3[0][Face(densityBytes.shape,0u,1u,0u,0u)]==
			0.5f*0.01f+0.5f*0.02f&&
		densityBytesResult.faceDensityKGPerM3[0][Face(densityBytes.shape,0u,0u,0u,0u)]==
			0.5f*0.16f+0.5f*0.01f&&
		densityBytesResult.faceDensityKGPerM3[1][Face(densityBytes.shape,1u,0u,0u,0u)]==
			0.5f*0.01f+0.5f*densityBytes.ambientDensityKGPerM3&&
		densityBytesResult.faceDensityKGPerM3[2][Face(densityBytes.shape,2u,2u,0u,0u)]==0.08f&&
		densityBytesResult.velocityMPerS[0][Face(densityBytes.shape,0u,1u,0u,0u)]==
			((0.5f*0.01f+0.5f*0.02f)*0.1f)/(0.5f*0.01f+0.5f*0.02f),
		"P2 stores exact arithmetic-mean interior, periodic, open, and wall face densities");
	Check(densityBytesOK&&
		densityBytesResult.faceDensityKGPerM3[0][Face(densityBytes.shape,0u,0u,0u,0u)]==
			densityBytesResult.faceDensityKGPerM3[0][Face(densityBytes.shape,0u,4u,0u,0u)]&&
		densityBytesResult.momentumKGPerM2S[0][Face(densityBytes.shape,0u,0u,0u,0u)]==
			densityBytesResult.momentumKGPerM2S[0][Face(densityBytes.shape,0u,4u,0u,0u)]&&
		densityBytesResult.velocityMPerS[0][Face(densityBytes.shape,0u,0u,0u,0u)]==
			densityBytesResult.velocityMPerS[0][Face(densityBytes.shape,0u,4u,0u,0u)],
		"P2 publishes the positive periodic seam as an exact canonical byte duplicate");

	FireProductionProjectionRequest wallOverwrite=EmptyRequest(4u,4u,4u);
	SetBoundary(wallOverwrite,FireProductionProjectionWall);
	std::fill(wallOverwrite.gasDensityKGPerM3.begin(),wallOverwrite.gasDensityKGPerM3.end(),2.0f);
	for( unsigned int axis=0;axis<3u;++axis ) {
		const std::size_t extent=axis==0u?wallOverwrite.shape.nx:
			(axis==1u?wallOverwrite.shape.ny:wallOverwrite.shape.nz);
		for( const std::size_t coordinate : {std::size_t(0u),extent} ) {
			std::size_t x=1u,y=1u,z=1u;
			if( axis==0u ) x=coordinate;if( axis==1u ) y=coordinate;
			if( axis==2u ) z=coordinate;
			wallOverwrite.provisionalMomentumKGPerM2S[axis][Face(
				wallOverwrite.shape,axis,x,y,z)]=7.0f+static_cast<float>(2u*axis)+
					static_cast<float>(coordinate!=0u);
		}
	}
	FireProductionProjectionResult wallOverwriteResult;
	bool everyWallOverwritten=ProjectFireProductionCPU(wallOverwrite,wallOverwriteResult,&error);
	for( unsigned int axis=0;everyWallOverwritten&&axis<3u;++axis ) {
		const std::size_t extent=axis==0u?wallOverwrite.shape.nx:
			(axis==1u?wallOverwrite.shape.ny:wallOverwrite.shape.nz);
		for( const std::size_t coordinate : {std::size_t(0u),extent} ) {
			std::size_t x=1u,y=1u,z=1u;
			if( axis==0u ) x=coordinate;if( axis==1u ) y=coordinate;
			if( axis==2u ) z=coordinate;
			const std::size_t face=Face(wallOverwrite.shape,axis,x,y,z);
			everyWallOverwritten=wallOverwriteResult.faceDensityKGPerM3[axis][face]==2.0f&&
				wallOverwriteResult.momentumKGPerM2S[axis][face]==0.0f&&
				wallOverwriteResult.velocityMPerS[axis][face]==0.0f;
		}
	}
	Check(everyWallOverwritten,
		"P2 wall output preserves adjacent density and overwrites nonzero normal momentum exactly");

	FireProductionProjectionRequest manufactured=EmptyRequest(17u,9u,7u);
	SetBoundary(manufactured,FireProductionProjectionPeriodic);
	std::vector<float> knownPressure(manufactured.shape.CellCount(),0.0f);
	for( std::size_t z=0;z<manufactured.shape.nz;++z )
		for( std::size_t y=0;y<manufactured.shape.ny;++y )
			for( std::size_t x=0;x<manufactured.shape.nx;++x ) {
				const float phaseX=2.0f*pi*(static_cast<float>(x)+0.5f)/
					static_cast<float>(manufactured.shape.nx);
				const float phaseY=2.0f*pi*(static_cast<float>(y)+0.5f)/
					static_cast<float>(manufactured.shape.ny);
				knownPressure[Cell(manufactured.shape,x,y,z)]=0.7f*std::sin(phaseX)+
					0.2f*std::cos(phaseY);
				manufactured.gasDensityKGPerM3[Cell(manufactured.shape,x,y,z)]=
					1.0f+0.2f*std::sin(phaseX)*std::cos(phaseY);
			}
	for( unsigned int axis=0;axis<3u;++axis ) {
		const std::size_t ex=axis==0u?manufactured.shape.nx+1u:manufactured.shape.nx;
		const std::size_t ey=axis==1u?manufactured.shape.ny+1u:manufactured.shape.ny;
		const std::size_t ez=axis==2u?manufactured.shape.nz+1u:manufactured.shape.nz;
		for( std::size_t z=0;z<ez;++z ) for( std::size_t y=0;y<ey;++y )
			for( std::size_t x=0;x<ex;++x ) {
				const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?manufactured.shape.nx:
					(axis==1u?manufactured.shape.ny:manufactured.shape.nz);
				std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
				if( coordinate==0u||coordinate==extent ) {
					if( axis==0u ) {lx=manufactured.shape.nx-1u;hx=0u;}
					if( axis==1u ) {ly=manufactured.shape.ny-1u;hy=0u;}
					if( axis==2u ) {lz=manufactured.shape.nz-1u;hz=0u;}
				} else {
					if( axis==0u ) --lx;if( axis==1u ) --ly;if( axis==2u ) --lz;
				}
				const float gradient=(knownPressure[Cell(manufactured.shape,hx,hy,hz)]-
					knownPressure[Cell(manufactured.shape,lx,ly,lz)])/manufactured.shape.cellWidthM;
				float baseVelocity=axis==0u?0.12f:(axis==1u?-0.07f:0.03f);
				const std::size_t periodicCoordinate=coordinate==extent?0u:coordinate;
				if( axis==0u ) baseVelocity+=0.04f*std::sin(2.0f*pi*
					static_cast<float>(periodicCoordinate)/static_cast<float>(extent));
				if( axis==1u ) baseVelocity+=0.02f*std::cos(2.0f*pi*
					static_cast<float>(periodicCoordinate)/static_cast<float>(extent));
				const float density=IndependentFaceDensity(manufactured,axis,x,y,z);
				manufactured.provisionalMomentumKGPerM2S[axis][Face(
					manufactured.shape,axis,x,y,z)]=density*baseVelocity+
					manufactured.timeStepS*gradient;
			}
	}
	for( std::size_t z=0;z<manufactured.shape.nz;++z )
		for( std::size_t y=0;y<manufactured.shape.ny;++y )
			for( std::size_t x=0;x<manufactured.shape.nx;++x ) {
				const float ux0=0.12f+0.04f*std::sin(2.0f*pi*static_cast<float>(x)/
					static_cast<float>(manufactured.shape.nx));
				const float ux1=0.12f+0.04f*std::sin(2.0f*pi*static_cast<float>((x+1u)%manufactured.shape.nx)/
					static_cast<float>(manufactured.shape.nx));
				const float uy0=-0.07f+0.02f*std::cos(2.0f*pi*static_cast<float>(y)/
					static_cast<float>(manufactured.shape.ny));
				const float uy1=-0.07f+0.02f*std::cos(2.0f*pi*static_cast<float>((y+1u)%manufactured.shape.ny)/
					static_cast<float>(manufactured.shape.ny));
				manufactured.divergenceTargetPerS[Cell(manufactured.shape,x,y,z)]=
					(ux1-ux0+uy1-uy0)/manufactured.shape.cellWidthM;
			}
	FireProductionProjectionResult manufacturedResult,repeatedResult;
	const bool manufacturedOK=ProjectFireProductionCPU(manufactured,manufacturedResult,&error);
	const bool repeatedOK=ProjectFireProductionCPU(manufactured,repeatedResult,&error);
	float maximumPressureError=0.0f;
	if( manufacturedOK ) {
		float pressureMean=0.0f;
		for( const float value : manufacturedResult.pressurePa ) pressureMean+=value;
		pressureMean/=static_cast<float>(manufacturedResult.pressurePa.size());
		for( std::size_t cell=0;cell<knownPressure.size();++cell )
			maximumPressureError=std::max(maximumPressureError,std::fabs(
				(manufacturedResult.pressurePa[cell]-pressureMean)-knownPressure[cell]));
	}
	if( !manufacturedOK||!manufacturedResult.validationPassed )
		std::cerr << "Manufactured projection detail: " << error << " post=" <<
			manufacturedResult.maximumPostProjectionResidualPerS << " pressure_error=" <<
			maximumPressureError << '\n';
	const float independentlyMeasuredResidual=manufacturedOK?
		IndependentResidual(manufactured,manufacturedResult):0.0f;
	Check(manufacturedOK&&repeatedOK&&manufacturedResult.validationPassed&&
		maximumPressureError<=0.025f&&
		manufacturedResult.maximumPostProjectionResidualPerS==independentlyMeasuredResidual&&
		manufacturedResult.executedVCycleCount==12u&&
		manufacturedResult.executedJacobiSweepCount==600u&&
		manufacturedResult.pressurePa==repeatedResult.pressurePa&&
		manufacturedResult.velocityMPerS==repeatedResult.velocityMPerS&&
		EveryPeriodicSeamExact(manufactured.shape,manufacturedResult),
		"P2 odd-grid variable-density manufactured projection is accurate and byte deterministic");
	FireSim::PeriodicMACShape oracleShape;oracleShape.nx=manufactured.shape.nx;
	oracleShape.ny=manufactured.shape.ny;oracleShape.nz=manufactured.shape.nz;
	oracleShape.cellWidthM=manufactured.shape.cellWidthM;
	std::vector<double> oracleDensity(manufactured.gasDensityKGPerM3.begin(),
		manufactured.gasDensityKGPerM3.end());
	std::vector<double> oracleTarget(manufactured.divergenceTargetPerS.begin(),
		manufactured.divergenceTargetPerS.end());
	FireSim::PeriodicMACField oracleMomentum;
	for( unsigned int axis=0;axis<3u;++axis ) {
		oracleMomentum.component[axis].assign(oracleShape.CellCount(),0.0);
		for( std::size_t z=0;z<manufactured.shape.nz;++z )
			for( std::size_t y=0;y<manufactured.shape.ny;++y )
				for( std::size_t x=0;x<manufactured.shape.nx;++x ) {
					const std::size_t fx=axis==0u?x+1u:x;
					const std::size_t fy=axis==1u?y+1u:y;
					const std::size_t fz=axis==2u?z+1u:z;
					oracleMomentum.component[axis][oracleShape.Index(x,y,z)]=
						manufactured.provisionalMomentumKGPerM2S[axis][Face(
							manufactured.shape,axis,fx,fy,fz)];
				}
	}
	FireSim::PeriodicMACProjection3DResult oracleResult;
	// U_ref=0.2 m/s is a predeclared upper bound on the fixture's independently
	// authored provisional velocity (the measured maximum is below 0.185 m/s).
	const double oracleReferenceVelocity=0.2;
	const double oracleTolerance=0.005*oracleReferenceVelocity/
		(static_cast<double>(manufactured.shape.cellWidthM)*manufactured.shape.nx);
	const bool oracleOK=FireSim::ProjectPeriodicMACVelocity3D(oracleShape,oracleDensity,oracleMomentum,
		oracleTarget,manufactured.timeStepS,oracleTolerance,oracleResult,&error);
	double oracleMaximumResidual=0.0;
	if( oracleOK ) for( std::size_t cell=0;cell<oracleShape.CellCount();++cell )
		oracleMaximumResidual=std::max(oracleMaximumResidual,std::fabs(
			FireSim::PeriodicMACDivergence3D(oracleShape,oracleResult.velocityMPerS,cell)-
			oracleTarget[cell]));
	Check(oracleOK&&oracleMaximumResidual>0.0&&oracleMaximumResidual<=oracleTolerance&&
		independentlyMeasuredResidual<=
		1.25f*static_cast<float>(oracleMaximumResidual),
		"P2 manufactured residual is no worse than 1.25 times the fp64 oracle error");

	FireProductionProjectionRequest fixedWork=EmptyRequest(17u,9u,7u);
	SetBoundary(fixedWork,FireProductionProjectionWall);
	std::vector<float> fixedPressure(fixedWork.shape.CellCount(),0.0f);
	for( std::size_t z=0;z<fixedWork.shape.nz;++z )
		for( std::size_t y=0;y<fixedWork.shape.ny;++y )
			for( std::size_t x=0;x<fixedWork.shape.nx;++x ) {
				const float px=pi*(static_cast<float>(x)+0.5f)/
					static_cast<float>(fixedWork.shape.nx);
				const float py=pi*(static_cast<float>(y)+0.5f)/
					static_cast<float>(fixedWork.shape.ny);
				const float pz=pi*(static_cast<float>(z)+0.5f)/
					static_cast<float>(fixedWork.shape.nz);
				fixedPressure[Cell(fixedWork.shape,x,y,z)]=0.7f*std::cos(px)+
					0.2f*std::cos(py)+0.1f*std::cos(pz);
				fixedWork.gasDensityKGPerM3[Cell(fixedWork.shape,x,y,z)]=
					1.0f+0.15f*std::sin(px)*std::cos(py);
			}
	for( unsigned int axis=0;axis<3u;++axis ) {
		const std::size_t ex=axis==0u?fixedWork.shape.nx+1u:fixedWork.shape.nx;
		const std::size_t ey=axis==1u?fixedWork.shape.ny+1u:fixedWork.shape.ny;
		const std::size_t ez=axis==2u?fixedWork.shape.nz+1u:fixedWork.shape.nz;
		for( std::size_t z=0;z<ez;++z ) for( std::size_t y=0;y<ey;++y )
			for( std::size_t x=0;x<ex;++x ) {
				const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?fixedWork.shape.nx:
					(axis==1u?fixedWork.shape.ny:fixedWork.shape.nz);
				if( coordinate==0u||coordinate==extent ) continue;
				std::size_t lx=x,ly=y,lz=z;
				if( axis==0u ) --lx;if( axis==1u ) --ly;if( axis==2u ) --lz;
				fixedWork.provisionalMomentumKGPerM2S[axis][Face(fixedWork.shape,
					axis,x,y,z)]=fixedWork.timeStepS*(fixedPressure[Cell(fixedWork.shape,x,y,z)]-
					fixedPressure[Cell(fixedWork.shape,lx,ly,lz)])/fixedWork.shape.cellWidthM;
			}
	}
	FireProductionProjectionResult fixedWorkResult;
	const bool fixedWorkOK=ProjectFireProductionCPU(fixedWork,fixedWorkResult,&error);
	const double fixedWorkOracleResidual=IndependentFixed12WallResidual(fixedWork);
	Check(fixedWorkOK&&fixedWorkResult.validationPassed&&fixedWorkOracleResidual>0.0&&
		fixedWorkResult.maximumPostProjectionResidualPerS<=
			1.25f*static_cast<float>(fixedWorkOracleResidual),
		"P2 fp32 fixed schedule stays within 1.25 times an independent fp64 fixed-work mirror");

	FireProductionProjectionRequest expansion=EmptyRequest(11u,7u,5u);
	SetBoundary(expansion,FireProductionProjectionPressureOpen);
	const float expansionRate=0.4f;
	const float length=expansion.shape.cellWidthM*static_cast<float>(expansion.shape.nx);
	for( std::size_t z=0;z<expansion.shape.nz;++z ) for( std::size_t y=0;y<expansion.shape.ny;++y )
		for( std::size_t x=0;x<=expansion.shape.nx;++x )
			expansion.provisionalMomentumKGPerM2S[0][Face(expansion.shape,0u,x,y,z)]=
				expansionRate*(static_cast<float>(x)*expansion.shape.cellWidthM-0.5f*length);
	std::fill(expansion.divergenceTargetPerS.begin(),expansion.divergenceTargetPerS.end(),
		expansionRate);
	FireProductionProjectionResult expansionResult;
	const bool expansionOK=ProjectFireProductionCPU(expansion,expansionResult,&error);
	float expansionMaximumPressure=0.0f;
	for( const float value : expansionResult.pressurePa )
		expansionMaximumPressure=std::max(expansionMaximumPressure,std::fabs(value));
	if( !expansionOK||!expansionResult.validationPassed||
		expansionResult.maximumPostProjectionResidualPerS>1.0e-5f||
		expansionMaximumPressure>1.0e-6f )
		std::cerr << "Expansion projection detail: " << error << " pre=" <<
			expansionResult.maximumPreProjectionResidualPerS << " post=" <<
			expansionResult.maximumPostProjectionResidualPerS << " complementarity=" <<
			expansionResult.maximumOpenComplementarityDiscrepancyMPerS << " pressure=" <<
			expansionMaximumPressure << '\n';
	Check(expansionOK&&
		expansionResult.validationPassed&&expansionResult.maximumPostProjectionResidualPerS<=1.0e-5f&&
		expansionMaximumPressure<=1.0e-6f&&
		std::all_of(expansionResult.pressureOpenInflow.begin(),
			expansionResult.pressureOpenInflow.end(),[](const std::vector<unsigned char>& side){
				return std::all_of(side.begin(),side.end(),
					[](unsigned char value){return value==0u;});}),
		"P2 pressure-open expansion preserves the independent x-linear target field");

	for( const float normalVelocity : {0.3f,-0.3f} ) {
		FireProductionProjectionRequest totalHead=EmptyRequest(11u,7u,5u);
		totalHead.boundary={{FireProductionProjectionPressureOpen,
			FireProductionProjectionPressureOpen,FireProductionProjectionPeriodic,
			FireProductionProjectionPeriodic,FireProductionProjectionPeriodic,
			FireProductionProjectionPeriodic}};
		for( float& value : totalHead.provisionalMomentumKGPerM2S[0] ) value=normalVelocity;
		for( float& value : totalHead.provisionalMomentumKGPerM2S[1] ) value=0.4f;
		for( float& value : totalHead.provisionalMomentumKGPerM2S[2] ) value=-0.2f;
		float speed2=normalVelocity*normalVelocity;
		speed2+=0.4f*0.4f;speed2+=(-0.2f)*(-0.2f);
		const float inflowPressure=-0.5f*totalHead.ambientDensityKGPerM3*speed2;
		const float lowPressure=normalVelocity>0.0f?inflowPressure:0.0f;
		const float highPressure=normalVelocity<0.0f?inflowPressure:0.0f;
		const float gradient=(highPressure-lowPressure)/
			(totalHead.shape.cellWidthM*static_cast<float>(totalHead.shape.nx));
		FireProductionProjectionResult totalHeadResult;
		const bool totalHeadOK=ProjectFireProductionCPU(totalHead,totalHeadResult,&error);
		float totalHeadPressureError=0.0f,totalHeadVelocityError=0.0f;
		if( totalHeadOK ) {
			for( std::size_t z=0;z<totalHead.shape.nz;++z )
				for( std::size_t y=0;y<totalHead.shape.ny;++y )
					for( std::size_t x=0;x<totalHead.shape.nx;++x ) {
						const float expected=lowPressure+(static_cast<float>(x)+0.5f)*
							totalHead.shape.cellWidthM*gradient;
						totalHeadPressureError=std::max(totalHeadPressureError,std::fabs(
							totalHeadResult.pressurePa[Cell(totalHead.shape,x,y,z)]-expected));
					}
			const float expectedVelocity=normalVelocity-totalHead.timeStepS*gradient;
			for( const float value : totalHeadResult.velocityMPerS[0] )
				totalHeadVelocityError=std::max(totalHeadVelocityError,
					std::fabs(value-expectedVelocity));
		}
		const unsigned char lowInflow=normalVelocity>0.0f?1u:0u;
		const unsigned char highInflow=normalVelocity<0.0f?1u:0u;
		if( !totalHeadOK||!totalHeadResult.validationPassed||
			totalHeadPressureError>2.0e-4f||totalHeadVelocityError>3.0e-5f )
			std::cerr << "Total-head detail: u=" << normalVelocity << " error=" << error <<
				" residual=" << totalHeadResult.maximumPostProjectionResidualPerS <<
				" pressure=" << totalHeadPressureError << " velocity=" <<
				totalHeadVelocityError << '\n';
		Check(totalHeadOK&&totalHeadResult.validationPassed&&
			totalHeadResult.maximumPostProjectionResidualPerS<=2.5e-4f&&
			totalHeadPressureError<=2.0e-4f&&totalHeadVelocityError<=3.0e-5f&&
			std::all_of(totalHeadResult.pressureOpenInflow[0].begin(),
				totalHeadResult.pressureOpenInflow[0].end(),[&](unsigned char value){
					return value==lowInflow;})&&
			std::all_of(totalHeadResult.pressureOpenInflow[1].begin(),
				totalHeadResult.pressureOpenInflow[1].end(),[&](unsigned char value){
					return value==highInflow;}),
			"P2 total-head manufactured field binds factor-two faces, tangents, and reversed roles");
	}

	FireProductionProjectionRequest roles=EmptyRequest(8u,6u,4u);
	SetBoundary(roles,FireProductionProjectionPressureOpen);
	for( std::size_t z=0;z<roles.shape.nz;++z ) for( std::size_t y=0;y<roles.shape.ny;++y ) {
		roles.provisionalMomentumKGPerM2S[0][Face(roles.shape,0u,0u,y,z)]=0.2f;
		roles.provisionalMomentumKGPerM2S[0][Face(roles.shape,0u,roles.shape.nx,y,z)]=0.2f;
	}
	FireProductionProjectionResult rolesResult;
	Check(ProjectFireProductionCPU(roles,rolesResult,&error)&&
		std::all_of(rolesResult.pressureOpenInflow[0].begin(),
			rolesResult.pressureOpenInflow[0].end(),[](unsigned char value){return value==1u;})&&
		std::all_of(rolesResult.pressureOpenInflow[1].begin(),
			rolesResult.pressureOpenInflow[1].end(),[](unsigned char value){return value==0u;}),
		"P2 frozen pressure-open total-head roles distinguish inflow and outflow signs");

	FireProductionProjectionRequest incompatible=EmptyRequest(6u,5u,4u);
	SetBoundary(incompatible,FireProductionProjectionWall);
	std::fill(incompatible.divergenceTargetPerS.begin(),
		incompatible.divergenceTargetPerS.end(),0.4f);
	FireProductionProjectionResult incompatibleResult;
	Check(ProjectFireProductionCPU(incompatible,incompatibleResult,&error)&&
		!incompatibleResult.validationPassed&&
		incompatibleResult.maximumPostProjectionResidualPerS==0.4f&&
		!incompatibleResult.pressurePa.empty(),
		"P2 finite validation miss publishes diagnostics without retry or structural abort");
	bool justBelow=false,justAbove=true;
	const float exactBand=0.005f*2.0f/4.0f;
	Check(FireProductionProjectionResidualWithinBand(std::nextafter(exactBand,0.0f),
		2.0f,4.0f,justBelow)&&justBelow&&
		FireProductionProjectionResidualWithinBand(std::nextafter(exactBand,
			std::numeric_limits<float>::infinity()),2.0f,4.0f,justAbove)&&!justAbove,
		"P2 validation band straddles the exact 0.005 U/L boundary");
	for( const float speed : {47.0f,49.0f} ) {
		FireProductionProjectionRequest wiredBand=EmptyRequest(6u,5u,4u);
		SetBoundary(wiredBand,FireProductionProjectionPeriodic);
		std::fill(wiredBand.provisionalMomentumKGPerM2S[0].begin(),
			wiredBand.provisionalMomentumKGPerM2S[0].end(),speed);
		std::fill(wiredBand.divergenceTargetPerS.begin(),
			wiredBand.divergenceTargetPerS.end(),0.4f);
		FireProductionProjectionResult wiredBandResult;
		const bool wiredBandOK=ProjectFireProductionCPU(wiredBand,wiredBandResult,&error);
		const float wiredResidual=wiredBandOK?IndependentResidual(wiredBand,wiredBandResult):0.0f;
		float wiredMaximumVelocity=0.0f;
		if( wiredBandOK ) for( unsigned int axis=0;axis<3u;++axis )
			for( const float value : wiredBandResult.velocityMPerS[axis] )
				wiredMaximumVelocity=std::max(wiredMaximumVelocity,std::fabs(value));
		const float wiredLength=wiredBand.shape.cellWidthM*static_cast<float>(
			std::max(wiredBand.shape.nx,std::max(wiredBand.shape.ny,wiredBand.shape.nz)));
		const bool independentlyAccepted=wiredResidual<=
			0.005f*wiredMaximumVelocity/wiredLength;
		Check(wiredBandOK&&wiredResidual==0.4f&&wiredMaximumVelocity==speed&&
			wiredBandResult.maximumPostProjectionResidualPerS==wiredResidual&&
			wiredBandResult.validationPassed==independentlyAccepted&&
			independentlyAccepted==(speed==49.0f),
			"P2 production wiring straddles the independently computed nonzero 0.005 U/L band");
	}
	FireProductionProjectionRequest cancellation=EmptyRequest(4u,4u,4u);
	SetBoundary(cancellation,FireProductionProjectionPeriodic);
	cancellation.divergenceTargetPerS[0]=1.0e6f;
	cancellation.divergenceTargetPerS[1]=0.01f;
	cancellation.divergenceTargetPerS[2]=-1.0e6f;
	cancellation.divergenceTargetPerS[3]=0.01f;
	FireProductionProjectionResult cancellationResult;
	Check(ProjectFireProductionCPU(cancellation,cancellationResult,&error)&&
		cancellationResult.removedFineRightHandSideMean==0.0f,
		"P2 nullspace mean uses the cancellation-sensitive pinned Blelloch tree");
	FireProductionProjectionRequest toleranceOverflow=EmptyRequest(4u,4u,4u);
	SetBoundary(toleranceOverflow,FireProductionProjectionPeriodic);
	toleranceOverflow.shape.cellWidthM=1.0e-18f;toleranceOverflow.timeStepS=1.0f;
	for( unsigned int axis=0;axis<3u;++axis )
		std::fill(toleranceOverflow.provisionalMomentumKGPerM2S[axis].begin(),
			toleranceOverflow.provisionalMomentumKGPerM2S[axis].end(),1.0e30f);
	FireProductionProjectionResult toleranceOverflowResult;
	Check(!ProjectFireProductionCPU(toleranceOverflow,toleranceOverflowResult,&error)&&
		toleranceOverflowResult.pressurePa.empty()&&
		error.find("validation band overflowed")!=std::string::npos,
		"P2 finite inputs fail structurally when the derived validation band overflows");

	FireProductionProjectionRequest invalid=manufactured;
	invalid.gasDensityKGPerM3[0]=std::numeric_limits<float>::quiet_NaN();
	FireProductionProjectionResult invalidResult;invalidResult.pressurePa.push_back(9.0f);
	Check(!ProjectFireProductionCPU(invalid,invalidResult,&error)&&
		invalidResult.pressurePa.empty()&&!error.empty(),
		"P2 structural input failure returns no partial projection");
	FireProductionProjectionRequest allocationFailure=EmptyRequest(4u,4u,4u);
	FireProductionProjectionResult allocationFailureResult;
	allocationFailureResult.pressurePa.push_back(7.0f);error.clear();error.shrink_to_fit();
	denyTestAllocations=true;
	const bool allocationFailureReturned=ProjectFireProductionCPU(
		allocationFailure,allocationFailureResult,&error);
	denyTestAllocations=false;
	Check(!allocationFailureReturned&&allocationFailureResult.pressurePa.empty(),
		"P2 persistent allocator denial returns false with no partial result or escaped exception");
	FireProductionProjectionRequest seam=EmptyRequest(4u,4u,4u);
	SetBoundary(seam,FireProductionProjectionPeriodic);
	seam.provisionalMomentumKGPerM2S[0][Face(seam.shape,0u,4u,0u,0u)]=1.0f;
	Check(!ValidateFireProductionProjectionRequest(seam,&error)&&
		error.find("seam differs")!=std::string::npos,
		"P2 rejects a noncanonical periodic momentum seam");
	FireProductionProjectionRequest unpaired=EmptyRequest(4u,4u,4u);
	unpaired.boundary[0]=FireProductionProjectionPeriodic;
	Check(!ValidateFireProductionProjectionRequest(unpaired,&error)&&
		error.find("unpaired")!=std::string::npos,
		"P2 rejects an unpaired periodic boundary");
	FireProductionProjectionRequest oversized;
	oversized.shape.nx=1024u;oversized.shape.ny=1024u;oversized.shape.nz=1024u;
	oversized.shape.cellWidthM=0.1f;oversized.timeStepS=0.01f;
	oversized.ambientDensityKGPerM3=1.0f;
	Check(!ValidateFireProductionProjectionRequest(oversized,&error)&&
		error.find("2 GiB")!=std::string::npos,
		"P2 rejects the complete peak working set before allocating arrays");
	FireProductionProjectionShape nearUnder,nearOver;
	nearUnder.nx=264u;nearUnder.ny=266u;nearUnder.nz=281u;nearUnder.cellWidthM=0.1f;
	nearOver.nx=260u;nearOver.ny=274u;nearOver.nz=277u;nearOver.cellWidthM=0.1f;
	std::uint64_t nearUnderBytes=0u,nearOverBytes=0u;
	Check(FireProductionProjectionWorkingSetBytes(nearUnder,nearUnderBytes)&&
		FireProductionProjectionWorkingSetBytes(nearOver,nearOverBytes)&&
		nearUnderBytes==UINT64_C(2147440808)&&nearOverBytes==UINT64_C(2147512068)&&
		nearUnderBytes<=(UINT64_C(1)<<31u)&&nearOverBytes>(UINT64_C(1)<<31u),
		"P2 complete working-set accounting binds the independent final-42-KiB boundary pair");
	FireProductionProjectionShape malformedWorkingSet=nearUnder;
	malformedWorkingSet.nx=std::numeric_limits<std::size_t>::max();
	std::uint64_t malformedBytes=9u;
	Check(!FireProductionProjectionWorkingSetBytes(malformedWorkingSet,malformedBytes)&&
		malformedBytes==0u,
		"P2 working-set query rejects SIZE_MAX dimensions without wrapped arithmetic");
	malformedWorkingSet.nx=1025u;malformedBytes=9u;
	Check(!FireProductionProjectionWorkingSetBytes(malformedWorkingSet,malformedBytes)&&
		malformedBytes==0u,
		"P2 working-set query is total only over the production 4..1024 shape domain");

	const std::string source=ReadFile("src/Library/Utilities/FireProductionProjection.cpp");
	const std::string makefile=ReadFile("build/make/rise/Makefile");
	const std::string android=ReadFile("build/cmake/rise-android/CMakeLists.txt");
	const std::string visualStudio=ReadFile("build/VS2022/Library/Library.vcxproj");
	const std::string xcode=ReadFile("build/XCode/rise/rise.xcodeproj/project.pbxproj");
	const std::size_t cycleLoop=source.find("for( unsigned int cycle=0;cycle<12u;++cycle )");
	const std::size_t cycleLoopEnd=cycleLoop==std::string::npos?std::string::npos:
		source.find("result.pressurePa=",cycleLoop);
	const std::string cycleBody=cycleLoop==std::string::npos?std::string():
		source.substr(cycleLoop,cycleLoopEnd-cycleLoop);
	Check(Count(source,"for( unsigned int cycle=0;cycle<12u;++cycle )")==1u&&
		Count(source,"Smooth(level,boundary,3u,nullspace,sweepCounter)")==2u&&
		Count(source,"Smooth(level,boundary,32u,nullspace,sweepCounter)")==1u&&
		Count(source,"const float omega=2.0f/3.0f;")==1u&&
		Count(source,"VCycle(hierarchy,0u,request.boundary,nullspace,")==1u&&
		cycleBody.find("break") == std::string::npos&&
		source.find("const float mean=BlellochSum(values)")!=std::string::npos,
		"P2 source guard binds 12 cycles, 3+3/32 Jacobi, fp32 omega, and Blelloch mean");
	const std::size_t makeRule=makefile.find("FireProductionProjection.o :");
	const std::size_t makeRuleEnd=makeRule==std::string::npos?std::string::npos:
		makefile.find("\n\n",makeRule);
	const std::string projectionMakeRule=makeRule==std::string::npos?std::string():
		makefile.substr(makeRule,makeRuleEnd-makeRule);
	Check(projectionMakeRule.find("$(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math "
		"-ffp-contract=off")!=std::string::npos&&android.find("FireProductionProjection.cpp\"\n    PROPERTIES "
			"COMPILE_OPTIONS \"-fno-fast-math;-ffp-contract=off\"")!=std::string::npos&&
		visualStudio.find("FireProductionProjection.cpp\">\n      <FloatingPointModel>Strict")!=
			std::string::npos&&Count(xcode,"FireProductionProjection.cpp in Sources */ = "
			"{isa = PBXBuildFile;")==2u&&Count(xcode,"FireProductionProjection.cpp */; settings = "
			"{COMPILER_FLAGS = \"-fno-fast-math -ffp-contract=off\"; };")==2u,
		"P2 strict-fp32 bindings are present on every authoritative build surface");

	if( failures==0 ) {
		std::cout << "FireProductionProjectionTest passed: post_residual=" <<
			manufacturedResult.maximumPostProjectionResidualPerS <<
			" oracle_residual=" << oracleMaximumResidual << " oracle_ratio=" <<
			manufacturedResult.maximumPostProjectionResidualPerS/oracleMaximumResidual <<
			" fixed12_fp64_residual=" << fixedWorkOracleResidual <<
			" pressure_error=" << maximumPressureError << '\n';
		return 0;
	}
	std::cerr << failures << " FireProductionProjectionTest failure(s)\n";
	return 1;
}
