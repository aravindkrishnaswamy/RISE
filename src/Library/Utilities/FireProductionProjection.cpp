//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjection.cpp - strict-binary32 P2 projection oracle
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionProjection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>

namespace RISE
{
	namespace
	{
		struct Level
		{
			std::size_t nx,ny,nz;
			float spacing[3];
			std::vector<float> density,rhs,pressure,temporary,residual,diagonal;
			std::array<std::vector<float>,3> inverseFaceDensity;
		};

		bool Fail( std::string* error, const std::string& message )
		{
			if( error ) *error=message;
			return false;
		}

		std::size_t CellIndex( std::size_t nx, std::size_t ny,
			std::size_t x, std::size_t y, std::size_t z )
		{
			return (z*ny+y)*nx+x;
		}

		std::size_t FaceIndex( std::size_t nx, std::size_t ny,
			unsigned int axis, std::size_t x, std::size_t y, std::size_t z )
		{
			if( axis==0u ) return (z*ny+y)*(nx+1u)+x;
			if( axis==1u ) return (z*(ny+1u)+y)*nx+x;
			return (z*ny+y)*nx+x;
		}

		std::size_t SideFaceCount( const FireProductionProjectionShape& shape,
			unsigned int side )
		{
			if( side<2u ) return shape.ny*shape.nz;
			if( side<4u ) return shape.nx*shape.nz;
			return shape.nx*shape.ny;
		}

		std::size_t SideFaceIndex( const FireProductionProjectionShape& shape,
			unsigned int side, std::size_t x, std::size_t y, std::size_t z )
		{
			if( side<2u ) return z*shape.ny+y;
			if( side<4u ) return z*shape.nx+x;
			return y*shape.nx+x;
		}

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;
			while( result<value ) result<<=1u;
			return result;
		}

		bool AddBytes( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue ) return false;
			const std::uint64_t bytes=count*bytesPerValue;
			if( total>std::numeric_limits<std::uint64_t>::max()-bytes ) return false;
			total+=bytes;return true;
		}

		bool AddMetalBufferBytes( std::uint64_t count, std::uint64_t bytesPerValue,
			std::uint64_t& total )
		{
			if( count>std::numeric_limits<std::uint64_t>::max()/bytesPerValue ) return false;
			const std::uint64_t bytes=count*bytesPerValue;
			const std::uint64_t alignment=UINT64_C(16384);
			if( bytes>std::numeric_limits<std::uint64_t>::max()-(alignment-1u) ) return false;
			const std::uint64_t allocated=(bytes+alignment-1u)&~(alignment-1u);
			if( total>std::numeric_limits<std::uint64_t>::max()-allocated ) return false;
			total+=allocated;return true;
		}

		std::uint64_t FaceValueCount( std::size_t nx, std::size_t ny, std::size_t nz )
		{
			return static_cast<std::uint64_t>(nx+1u)*ny*nz+
				static_cast<std::uint64_t>(nx)*(ny+1u)*nz+
				static_cast<std::uint64_t>(nx)*ny*(nz+1u);
		}

		bool ProjectionWorkingSetBytes( const FireProductionProjectionShape& shape,
			std::uint64_t& bytes )
		{
			std::uint64_t total=0u;
			std::size_t nx=shape.nx,ny=shape.ny,nz=shape.nz;
			const std::uint64_t fineCells=static_cast<std::uint64_t>(nx)*ny*nz;
			const std::uint64_t fineFaces=FaceValueCount(nx,ny,nz);
			// Caller-owned request bytes and returned fine-grid bytes are part of the
			// production batch peak even though their vectors predate this call.
			if( !AddBytes(2u*fineCells+fineFaces,sizeof(float),total)||
				!AddBytes(fineCells+3u*fineFaces,sizeof(float),total) ) return false;
			// Every Metal allocation is rounded outward independently to the measured
			// M4 allocation quantum.  The terminal Private-to-Shared staging payload
			// coexists with the resident solve and caller/result vectors; upload staging
			// is smaller and has already been released.
			if( !AddMetalBufferBytes(fineCells,sizeof(float),total) ) return false; // target
			for( unsigned int copy=0u;copy<4u;++copy )
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::uint64_t count=axis==0u?static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz:
						(axis==1u?static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz:
						static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u));
					if( !AddMetalBufferBytes(count,sizeof(float),total) ) return false;
				}
			for( ;; ) {
				const std::uint64_t cells=static_cast<std::uint64_t>(nx)*ny*nz;
				// Six cell buffers, three independently allocated beta face buffers, and
				// one retained parameter buffer per level.
				for( unsigned int field=0u;field<6u;++field )
					if( !AddMetalBufferBytes(cells,sizeof(float),total) ) return false;
				const std::uint64_t xFaces=static_cast<std::uint64_t>(nx+1u)*ny*nz;
				const std::uint64_t yFaces=static_cast<std::uint64_t>(nx)*(ny+1u)*nz;
				const std::uint64_t zFaces=static_cast<std::uint64_t>(nx)*ny*(nz+1u);
				if( !AddMetalBufferBytes(xFaces,sizeof(float),total)||
					!AddMetalBufferBytes(yFaces,sizeof(float),total)||
					!AddMetalBufferBytes(zFaces,sizeof(float),total)||
					!AddMetalBufferBytes(84u,1u,total) ) return false;
				if( nx<=4u&&ny<=4u&&nz<=4u ) break;
				nx=nx>4u?(nx+1u)/2u:nx;ny=ny>4u?(ny+1u)/2u:ny;
				nz=nz>4u?(nz+1u)/2u:nz;
			}
			const std::uint64_t boundaryFaces=2u*(static_cast<std::uint64_t>(shape.ny)*shape.nz+
				static_cast<std::uint64_t>(shape.nx)*shape.nz+
				static_cast<std::uint64_t>(shape.nx)*shape.ny);
			if( !AddMetalBufferBytes(boundaryFaces,sizeof(float),total)||
				!AddMetalBufferBytes(boundaryFaces,sizeof(unsigned char),total)||
				// Six caller-owned classification vectors are materialized while the
				// device classification remains live.
				!AddBytes(boundaryFaces,sizeof(unsigned char),total)||
				!AddMetalBufferBytes(NextPowerOfTwo(static_cast<std::size_t>(fineCells)),sizeof(float),total)||
				!AddMetalBufferBytes(12u,sizeof(float),total) ) return false;
			// Terminal staging: pressure plus stored density, momentum, and velocity.
			if( !AddMetalBufferBytes(fineCells,sizeof(float),total) ) return false;
			for( unsigned int copy=0u;copy<3u;++copy )
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::uint64_t count=axis==0u?static_cast<std::uint64_t>(shape.nx+1u)*shape.ny*shape.nz:
						(axis==1u?static_cast<std::uint64_t>(shape.nx)*(shape.ny+1u)*shape.nz:
						static_cast<std::uint64_t>(shape.nx)*shape.ny*(shape.nz+1u));
					if( !AddMetalBufferBytes(count,sizeof(float),total) ) return false;
				}
			bytes=total;return true;
		}

		float BlellochSum( const std::vector<float>& values )
		{
			const std::size_t width=NextPowerOfTwo(values.size());
			std::vector<float> scratch(width,0.0f);
			std::copy(values.begin(),values.end(),scratch.begin());
			for( std::size_t offset=1u;offset<width;offset<<=1u )
				for( std::size_t index=2u*offset-1u;index<width;index+=2u*offset )
					scratch[index]+=scratch[index-offset];
			return scratch.back();
		}

		float RemoveMean( std::vector<float>& values )
		{
			const float mean=BlellochSum(values)/static_cast<float>(values.size());
			for( float& value : values ) value-=mean;
			return mean;
		}

		bool HasOpenBoundary( const std::array<FireProductionProjectionBoundary,6>& boundary )
		{
			return std::find(boundary.begin(),boundary.end(),
				FireProductionProjectionPressureOpen)!=boundary.end();
		}

		float ArithmeticMean( float first, float second )
		{
			return 0.5f*first+0.5f*second;
		}

		float StoredFaceDensity( const Level& level,
			const std::array<FireProductionProjectionBoundary,6>& boundary,
			float ambientDensity, unsigned int axis,
			std::size_t x, std::size_t y, std::size_t z )
		{
			const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
			const std::size_t extent=axis==0u?level.nx:(axis==1u?level.ny:level.nz);
			std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
			if( coordinate>0u&&coordinate<extent ) {
				if( axis==0u ) --lowX;if( axis==1u ) --lowY;if( axis==2u ) --lowZ;
				return ArithmeticMean(level.density[CellIndex(level.nx,level.ny,
					lowX,lowY,lowZ)],level.density[CellIndex(level.nx,level.ny,
					highX,highY,highZ)]);
			}
			const unsigned int side=2u*axis+(coordinate?1u:0u);
			if( boundary[side]==FireProductionProjectionPeriodic ) {
				if( axis==0u ) {lowX=level.nx-1u;highX=0u;}
				if( axis==1u ) {lowY=level.ny-1u;highY=0u;}
				if( axis==2u ) {lowZ=level.nz-1u;highZ=0u;}
				return ArithmeticMean(level.density[CellIndex(level.nx,level.ny,
					lowX,lowY,lowZ)],level.density[CellIndex(level.nx,level.ny,
					highX,highY,highZ)]);
			}
			if( axis==0u ) highX=coordinate?level.nx-1u:0u;
			if( axis==1u ) highY=coordinate?level.ny-1u:0u;
			if( axis==2u ) highZ=coordinate?level.nz-1u:0u;
			const float interior=level.density[CellIndex(level.nx,level.ny,
				highX,highY,highZ)];
			return boundary[side]==FireProductionProjectionPressureOpen?
				ArithmeticMean(interior,ambientDensity):interior;
		}

		void BuildLevelCoefficients( Level& level,
			const std::array<FireProductionProjectionBoundary,6>& boundary,
			float ambientDensity )
		{
			const std::size_t nx=level.nx,ny=level.ny,nz=level.nz;
			FireProductionProjectionShape shape;shape.nx=nx;shape.ny=ny;shape.nz=nz;
			for( unsigned int axis=0;axis<3u;++axis )
				level.inverseFaceDensity[axis].assign(
					FireProductionProjectionFaceCount(shape,axis),0.0f);
			for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<ny;++y )
				for( std::size_t x=0;x<=nx;++x ) {
					const std::size_t face=FaceIndex(nx,ny,0u,x,y,z);
					if( x>0u&&x<nx ) level.inverseFaceDensity[0][face]=1.0f/
						StoredFaceDensity(level,boundary,ambientDensity,0u,x,y,z);
					else {
						const unsigned int side=x?1u:0u;
						if( boundary[side]==FireProductionProjectionPeriodic ) {
							const float value=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,0u,x,y,z);
							level.inverseFaceDensity[0][face]=value;
						} else if( boundary[side]==FireProductionProjectionPressureOpen ) {
							level.inverseFaceDensity[0][face]=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,0u,x,y,z);
						}
					}
				}
			for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<=ny;++y )
				for( std::size_t x=0;x<nx;++x ) {
					const std::size_t face=FaceIndex(nx,ny,1u,x,y,z);
					if( y>0u&&y<ny ) level.inverseFaceDensity[1][face]=1.0f/
						StoredFaceDensity(level,boundary,ambientDensity,1u,x,y,z);
					else {
						const unsigned int side=y?3u:2u;
						if( boundary[side]==FireProductionProjectionPeriodic ) {
							level.inverseFaceDensity[1][face]=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,1u,x,y,z);
						} else if( boundary[side]==FireProductionProjectionPressureOpen ) {
							level.inverseFaceDensity[1][face]=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,1u,x,y,z);
						}
					}
				}
			for( std::size_t z=0;z<=nz;++z ) for( std::size_t y=0;y<ny;++y )
				for( std::size_t x=0;x<nx;++x ) {
					const std::size_t face=FaceIndex(nx,ny,2u,x,y,z);
					if( z>0u&&z<nz ) level.inverseFaceDensity[2][face]=1.0f/
						StoredFaceDensity(level,boundary,ambientDensity,2u,x,y,z);
					else {
						const unsigned int side=z?5u:4u;
						if( boundary[side]==FireProductionProjectionPeriodic ) {
							level.inverseFaceDensity[2][face]=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,2u,x,y,z);
						} else if( boundary[side]==FireProductionProjectionPressureOpen ) {
							level.inverseFaceDensity[2][face]=1.0f/
								StoredFaceDensity(level,boundary,ambientDensity,2u,x,y,z);
						}
					}
				}
			level.diagonal.assign(nx*ny*nz,0.0f);
			for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<ny;++y )
				for( std::size_t x=0;x<nx;++x ) {
					float diagonal=0.0f;
					const std::size_t coordinate[3]={x,y,z};
					const std::size_t extent[3]={nx,ny,nz};
					for( unsigned int axis=0;axis<3u;++axis ) {
						std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
						if( axis==0u ) ++highX;
						if( axis==1u ) ++highY;
						if( axis==2u ) ++highZ;
						const float inverseSpacing2=1.0f/(level.spacing[axis]*level.spacing[axis]);
						const float low=level.inverseFaceDensity[axis][FaceIndex(nx,ny,
							axis,lowX,lowY,lowZ)]*inverseSpacing2;
						const float high=level.inverseFaceDensity[axis][FaceIndex(nx,ny,
							axis,highX,highY,highZ)]*inverseSpacing2;
						diagonal+=(coordinate[axis]>0u||
							boundary[2u*axis]==FireProductionProjectionPeriodic)?low:
							(boundary[2u*axis]==FireProductionProjectionPressureOpen?2.0f*low:0.0f);
						diagonal+=(coordinate[axis]+1u<extent[axis]||
							boundary[2u*axis+1u]==FireProductionProjectionPeriodic)?high:
							(boundary[2u*axis+1u]==FireProductionProjectionPressureOpen?2.0f*high:0.0f);
					}
					level.diagonal[CellIndex(nx,ny,x,y,z)]=diagonal;
				}
		}

		void ApplyOperator( const Level& level,
			const std::array<FireProductionProjectionBoundary,6>& boundary,
			const std::vector<float>& pressure, std::vector<float>& output )
		{
			const std::size_t nx=level.nx,ny=level.ny,nz=level.nz;
			output.assign(nx*ny*nz,0.0f);
			for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<ny;++y )
				for( std::size_t x=0;x<nx;++x ) {
					const std::size_t cell=CellIndex(nx,ny,x,y,z);
					float value=0.0f;
					const std::size_t coordinate[3]={x,y,z};
					const std::size_t extent[3]={nx,ny,nz};
					for( unsigned int axis=0;axis<3u;++axis ) {
						std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
						if( axis==0u ) ++hx;
						if( axis==1u ) ++hy;
						if( axis==2u ) ++hz;
						const float scale=1.0f/(level.spacing[axis]*level.spacing[axis]);
						const float low=level.inverseFaceDensity[axis][FaceIndex(nx,ny,
							axis,lx,ly,lz)]*scale;
						const float high=level.inverseFaceDensity[axis][FaceIndex(nx,ny,
							axis,hx,hy,hz)]*scale;
						if( coordinate[axis]>0u ) {
							std::size_t px=x,py=y,pz=z;
							if( axis==0u ) --px;if( axis==1u ) --py;if( axis==2u ) --pz;
							value+=low*(pressure[cell]-pressure[CellIndex(nx,ny,px,py,pz)]);
						} else if( boundary[2u*axis]==FireProductionProjectionPeriodic ) {
							std::size_t px=x,py=y,pz=z;
							if( axis==0u ) px=nx-1u;if( axis==1u ) py=ny-1u;if( axis==2u ) pz=nz-1u;
							value+=low*(pressure[cell]-pressure[CellIndex(nx,ny,px,py,pz)]);
						} else if( boundary[2u*axis]==FireProductionProjectionPressureOpen )
							value+=2.0f*low*pressure[cell];
						if( coordinate[axis]+1u<extent[axis] ) {
							std::size_t px=x,py=y,pz=z;
							if( axis==0u ) ++px;if( axis==1u ) ++py;if( axis==2u ) ++pz;
							value+=high*(pressure[cell]-pressure[CellIndex(nx,ny,px,py,pz)]);
						} else if( boundary[2u*axis+1u]==FireProductionProjectionPeriodic ) {
							std::size_t px=x,py=y,pz=z;
							if( axis==0u ) px=0u;if( axis==1u ) py=0u;if( axis==2u ) pz=0u;
							value+=high*(pressure[cell]-pressure[CellIndex(nx,ny,px,py,pz)]);
						} else if( boundary[2u*axis+1u]==FireProductionProjectionPressureOpen )
							value+=2.0f*high*pressure[cell];
					}
					output[cell]=value;
				}
		}

		void Smooth( Level& level,
			const std::array<FireProductionProjectionBoundary,6>& boundary,
			unsigned int sweeps, bool nullspace, std::uint64_t& sweepCounter )
		{
			const float omega=2.0f/3.0f;
			for( unsigned int sweep=0;sweep<sweeps;++sweep ) {
				++sweepCounter;
				ApplyOperator(level,boundary,level.pressure,level.temporary);
				for( std::size_t cell=0;cell<level.pressure.size();++cell )
					level.residual[cell]=level.pressure[cell]+omega*
						(level.rhs[cell]-level.temporary[cell])/level.diagonal[cell];
				level.pressure.swap(level.residual);
				if( nullspace ) RemoveMean(level.pressure);
			}
		}

		std::size_t CoarseCoordinate( std::size_t fine, std::size_t fineExtent,
			std::size_t coarseExtent )
		{
			return coarseExtent==fineExtent?fine:std::min(coarseExtent-1u,fine/2u);
		}

		void RestrictAverage( const Level& fine, Level& coarse,
			const std::vector<float>& values, std::vector<float>& restricted )
		{
			restricted.assign(coarse.nx*coarse.ny*coarse.nz,0.0f);
			std::vector<unsigned int> count(restricted.size(),0u);
			for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
				for( std::size_t x=0;x<fine.nx;++x ) {
					const std::size_t cx=CoarseCoordinate(x,fine.nx,coarse.nx);
					const std::size_t cy=CoarseCoordinate(y,fine.ny,coarse.ny);
					const std::size_t cz=CoarseCoordinate(z,fine.nz,coarse.nz);
					const std::size_t cell=CellIndex(coarse.nx,coarse.ny,cx,cy,cz);
					restricted[cell]+=values[CellIndex(fine.nx,fine.ny,x,y,z)];
					++count[cell];
				}
			for( std::size_t cell=0;cell<restricted.size();++cell )
				restricted[cell]/=static_cast<float>(count[cell]);
		}

		void InterpolationCoordinate( std::size_t fine, std::size_t fineExtent,
			std::size_t coarseExtent, std::size_t& first, std::size_t& second, float& weight )
		{
			if( fineExtent==coarseExtent ) {first=fine;second=fine;weight=0.0f;return;}
			const float position=(static_cast<float>(fine)+0.5f)*
				static_cast<float>(coarseExtent)/static_cast<float>(fineExtent)-0.5f;
			if( position<=0.0f ) {first=0u;second=0u;weight=0.0f;return;}
			if( position>=static_cast<float>(coarseExtent-1u) ) {
				first=coarseExtent-1u;second=first;weight=0.0f;return;
			}
			first=static_cast<std::size_t>(std::floor(position));second=first+1u;
			weight=position-static_cast<float>(first);
		}

		void ProlongateAndAdd( const Level& coarse, Level& fine )
		{
			for( std::size_t z=0;z<fine.nz;++z ) for( std::size_t y=0;y<fine.ny;++y )
				for( std::size_t x=0;x<fine.nx;++x ) {
					std::size_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;
					float wx=0.0f,wy=0.0f,wz=0.0f;
					InterpolationCoordinate(x,fine.nx,coarse.nx,x0,x1,wx);
					InterpolationCoordinate(y,fine.ny,coarse.ny,y0,y1,wy);
					InterpolationCoordinate(z,fine.nz,coarse.nz,z0,z1,wz);
					float value=0.0f;
					for( unsigned int iz=0;iz<2u;++iz ) for( unsigned int iy=0;iy<2u;++iy )
						for( unsigned int ix=0;ix<2u;++ix ) {
							const float weight=(ix?wx:1.0f-wx)*(iy?wy:1.0f-wy)*
								(iz?wz:1.0f-wz);
							value+=weight*coarse.pressure[CellIndex(coarse.nx,coarse.ny,
								ix?x1:x0,iy?y1:y0,iz?z1:z0)];
						}
					fine.pressure[CellIndex(fine.nx,fine.ny,x,y,z)]+=value;
				}
		}

		void VCycle( std::vector<Level>& hierarchy, std::size_t levelIndex,
			const std::array<FireProductionProjectionBoundary,6>& boundary, bool nullspace,
			std::uint64_t& sweepCounter )
		{
			Level& level=hierarchy[levelIndex];
			if( levelIndex+1u==hierarchy.size() ) {
				Smooth(level,boundary,32u,nullspace,sweepCounter);return;
			}
			Smooth(level,boundary,3u,nullspace,sweepCounter);
			ApplyOperator(level,boundary,level.pressure,level.temporary);
			for( std::size_t cell=0;cell<level.rhs.size();++cell )
				level.residual[cell]=level.rhs[cell]-level.temporary[cell];
			Level& coarse=hierarchy[levelIndex+1u];
			RestrictAverage(level,coarse,level.residual,coarse.rhs);
			if( nullspace ) RemoveMean(coarse.rhs);
			std::fill(coarse.pressure.begin(),coarse.pressure.end(),0.0f);
			VCycle(hierarchy,levelIndex+1u,boundary,nullspace,sweepCounter);
			ProlongateAndAdd(coarse,level);
			Smooth(level,boundary,3u,nullspace,sweepCounter);
		}

		float CellCenteredVelocity( const FireProductionProjectionRequest& request,
			const std::array<std::vector<float>,3>& velocity, unsigned int axis,
			std::size_t x, std::size_t y, std::size_t z )
		{
			std::size_t hx=x,hy=y,hz=z;
			if( axis==0u ) ++hx;if( axis==1u ) ++hy;if( axis==2u ) ++hz;
			return 0.5f*(velocity[axis][FaceIndex(request.shape.nx,request.shape.ny,
				axis,x,y,z)]+velocity[axis][FaceIndex(request.shape.nx,
				request.shape.ny,axis,hx,hy,hz)]);
		}
	}

	std::size_t FireProductionProjectionFaceCount(
		const FireProductionProjectionShape& shape, unsigned int axis )
	{
		if( axis==0u ) return (shape.nx+1u)*shape.ny*shape.nz;
		if( axis==1u ) return shape.nx*(shape.ny+1u)*shape.nz;
		return shape.nx*shape.ny*(shape.nz+1u);
	}

	bool FireProductionProjectionWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes )
	{
		bytes=0u;
		if( shape.nx<4u||shape.ny<4u||shape.nz<4u||shape.nx>1024u||
			shape.ny>1024u||shape.nz>1024u||
			shape.nx>std::numeric_limits<std::size_t>::max()/shape.ny||
			shape.nx*shape.ny>std::numeric_limits<std::size_t>::max()/shape.nz ) return false;
		return ProjectionWorkingSetBytes(shape,bytes);
	}

	bool FireProductionProjectionResidualWithinBand( float maximumResidualPerS,
		float maximumVelocityMPerS,float domainLengthM,bool& withinBand )
	{
		withinBand=false;
		if( !std::isfinite(maximumResidualPerS)||maximumResidualPerS<0.0f||
			!std::isfinite(maximumVelocityMPerS)||maximumVelocityMPerS<0.0f||
			!std::isfinite(domainLengthM)||!(domainLengthM>0.0f) ) return false;
		const float tolerance=0.005f*maximumVelocityMPerS/domainLengthM;
		if( !std::isfinite(tolerance) ) return false;
		withinBand=maximumResidualPerS<=tolerance;return true;
	}

	bool ValidateFireProductionProjectionRequest( const FireProductionProjectionRequest& request,
		std::string* error )
	{
		const FireProductionProjectionShape& shape=request.shape;
		if( shape.nx<4u||shape.ny<4u||shape.nz<4u||shape.nx>1024u||shape.ny>1024u||
			shape.nz>1024u||!std::isfinite(shape.cellWidthM)||!(shape.cellWidthM>0.0f)||
			!std::isfinite(request.timeStepS)||!(request.timeStepS>0.0f)||
			!std::isfinite(request.ambientDensityKGPerM3)||!(request.ambientDensityKGPerM3>0.0f) )
			return Fail(error,"production projection shape or schedule is invalid");
		if( shape.nx>std::numeric_limits<std::size_t>::max()/shape.ny||
			shape.nx*shape.ny>std::numeric_limits<std::size_t>::max()/shape.nz )
			return Fail(error,"production projection dimensions overflow");
		std::uint64_t workingSetBytes=0u;
		if( !ProjectionWorkingSetBytes(shape,workingSetBytes)||
			workingSetBytes>(UINT64_C(1)<<31u) )
			return Fail(error,"production projection working set exceeds 2 GiB");
		const std::size_t cells=shape.CellCount();
		for( const FireProductionProjectionBoundary value : request.boundary )
			if( value!=FireProductionProjectionPeriodic&&value!=FireProductionProjectionWall&&
				value!=FireProductionProjectionPressureOpen )
				return Fail(error,"production projection boundary kind is invalid");
		if( request.gasDensityKGPerM3.size()!=cells||
			request.divergenceTargetPerS.size()!=cells )
			return Fail(error,"production projection cell arrays are malformed");
		for( unsigned int axis=0;axis<3u;++axis ) {
			if( (request.boundary[2u*axis]==FireProductionProjectionPeriodic)!=
				(request.boundary[2u*axis+1u]==FireProductionProjectionPeriodic) )
				return Fail(error,"production projection periodic boundary is unpaired");
			if( request.provisionalMomentumKGPerM2S[axis].size()!=
				FireProductionProjectionFaceCount(shape,axis) )
				return Fail(error,"production projection face arrays are malformed");
			for( const float value : request.provisionalMomentumKGPerM2S[axis] )
				if( !std::isfinite(value) )
					return Fail(error,"production projection momentum is nonfinite");
			if( request.boundary[2u*axis]==FireProductionProjectionPeriodic ) {
				const std::size_t firstCount=axis==0u?shape.ny:shape.nx;
				const std::size_t secondCount=axis==2u?shape.ny:shape.nz;
				for( std::size_t second=0;second<secondCount;++second )
					for( std::size_t first=0;first<firstCount;++first ) {
						std::size_t x0=0u,y0=0u,z0=0u,x1=0u,y1=0u,z1=0u;
						if( axis==0u ) {x1=shape.nx;y0=y1=first;z0=z1=second;}
						if( axis==1u ) {y1=shape.ny;x0=x1=first;z0=z1=second;}
						if( axis==2u ) {z1=shape.nz;x0=x1=first;y0=y1=second;}
						if( request.provisionalMomentumKGPerM2S[axis][FaceIndex(shape.nx,
							shape.ny,axis,x0,y0,z0)]!=request.provisionalMomentumKGPerM2S[axis][
							FaceIndex(shape.nx,shape.ny,axis,x1,y1,z1)] )
							return Fail(error,"production projection periodic momentum seam differs");
					}
			}
		}
		for( const float value : request.gasDensityKGPerM3 )
			if( !std::isfinite(value)||!(value>0.0f) )
				return Fail(error,"production projection density is invalid");
		for( const float value : request.divergenceTargetPerS )
			if( !std::isfinite(value) )
				return Fail(error,"production projection divergence target is nonfinite");
		return true;
	}

	bool ProjectFireProductionCPUImplementation( const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result, std::string* error )
	{
		result=FireProductionProjectionResult();
		if( !ValidateFireProductionProjectionRequest(request,error) ) return false;
		const FireProductionProjectionShape& shape=request.shape;
		const std::size_t nx=shape.nx,ny=shape.ny,nz=shape.nz,cells=shape.CellCount();
		Level fine;fine.nx=nx;fine.ny=ny;fine.nz=nz;
		fine.spacing[0]=shape.cellWidthM;fine.spacing[1]=shape.cellWidthM;
		fine.spacing[2]=shape.cellWidthM;fine.density=request.gasDensityKGPerM3;
		BuildLevelCoefficients(fine,request.boundary,request.ambientDensityKGPerM3);
		float maximumVelocity=0.0f;
		for( unsigned int axis=0;axis<3u;++axis ) {
			const std::size_t faces=FireProductionProjectionFaceCount(shape,axis);
			result.faceDensityKGPerM3[axis].resize(faces);
			result.velocityMPerS[axis].resize(faces);
			result.momentumKGPerM2S[axis]=request.provisionalMomentumKGPerM2S[axis];
			const std::size_t ex=axis==0u?nx+1u:nx;
			const std::size_t ey=axis==1u?ny+1u:ny;
			const std::size_t ez=axis==2u?nz+1u:nz;
			for( std::size_t z=0;z<ez;++z ) for( std::size_t y=0;y<ey;++y )
				for( std::size_t x=0;x<ex;++x ) {
					const std::size_t face=FaceIndex(nx,ny,axis,x,y,z);
					const float density=StoredFaceDensity(fine,request.boundary,
						request.ambientDensityKGPerM3,axis,x,y,z);
					result.faceDensityKGPerM3[axis][face]=density;
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					const unsigned int side=2u*axis+(coordinate?1u:0u);
					const bool wall=(coordinate==0u||coordinate==extent)&&
						request.boundary[side]==FireProductionProjectionWall;
					result.velocityMPerS[axis][face]=wall?0.0f:
						request.provisionalMomentumKGPerM2S[axis][face]/density;
				}
			for( const float value : result.velocityMPerS[axis] )
				maximumVelocity=std::max(maximumVelocity,std::fabs(value));
		}
		for( unsigned int side=0;side<6u;++side )
			result.pressureOpenInflow[side].assign(SideFaceCount(shape,side),0u);
		std::array<std::vector<float>,6> boundaryPressure;
		for( unsigned int side=0;side<6u;++side )
			boundaryPressure[side].assign(SideFaceCount(shape,side),0.0f);
		for( unsigned int side=0;side<6u;++side ) {
			if( request.boundary[side]!=FireProductionProjectionPressureOpen ) continue;
			const unsigned int axis=side/2u;const bool positive=(side&1u)!=0u;
			const std::size_t firstCount=axis==0u?ny:nx;
			const std::size_t secondCount=axis==2u?ny:nz;
			for( std::size_t second=0;second<secondCount;++second )
				for( std::size_t first=0;first<firstCount;++first ) {
					std::size_t x=0u,y=0u,z=0u;
					if( axis==0u ) {x=positive?nx:0u;y=first;z=second;}
					if( axis==1u ) {x=first;y=positive?ny:0u;z=second;}
					if( axis==2u ) {x=first;y=second;z=positive?nz:0u;}
					const std::size_t face=FaceIndex(nx,ny,axis,x,y,z);
					const float outward=(positive?1.0f:-1.0f)*result.velocityMPerS[axis][face];
					const std::size_t cx=axis==0u?(positive?nx-1u:0u):x;
					const std::size_t cy=axis==1u?(positive?ny-1u:0u):y;
					const std::size_t cz=axis==2u?(positive?nz-1u:0u):z;
					const std::size_t index=SideFaceIndex(shape,side,cx,cy,cz);
					if( outward<0.0f ) {
						result.pressureOpenInflow[side][index]=1u;
						float speed2=result.velocityMPerS[axis][face]*
							result.velocityMPerS[axis][face];
						for( unsigned int tangent=0;tangent<3u;++tangent ) if( tangent!=axis ) {
							const float value=CellCenteredVelocity(request,result.velocityMPerS,
								tangent,cx,cy,cz);speed2+=value*value;
						}
						boundaryPressure[side][index]=-
							0.5f*request.ambientDensityKGPerM3*speed2;
					}
				}
		}

		auto divergence=[&](const std::array<std::vector<float>,3>& velocity,
			std::size_t x,std::size_t y,std::size_t z){return
			(velocity[0][FaceIndex(nx,ny,0u,x+1u,y,z)]-
			 velocity[0][FaceIndex(nx,ny,0u,x,y,z)]+ 
			 velocity[1][FaceIndex(nx,ny,1u,x,y+1u,z)]-
			 velocity[1][FaceIndex(nx,ny,1u,x,y,z)]+ 
			 velocity[2][FaceIndex(nx,ny,2u,x,y,z+1u)]-
			 velocity[2][FaceIndex(nx,ny,2u,x,y,z)])/shape.cellWidthM;};

		fine.rhs.assign(cells,0.0f);
		for( unsigned int axis=0;axis<3u;++axis ) for( const float value:result.velocityMPerS[axis] )
			maximumVelocity=std::max(maximumVelocity,std::fabs(value));
		for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<ny;++y )
			for( std::size_t x=0;x<nx;++x ) {
				const std::size_t cell=CellIndex(nx,ny,x,y,z);
				const float residual=divergence(result.velocityMPerS,x,y,z)-
					request.divergenceTargetPerS[cell];
				result.maximumPreProjectionResidualPerS=std::max(
					result.maximumPreProjectionResidualPerS,std::fabs(residual));
				fine.rhs[cell]=-residual/request.timeStepS;
				const std::size_t coordinate[3]={x,y,z};
				const std::size_t extent[3]={nx,ny,nz};
				for( unsigned int axis=0;axis<3u;++axis ) for( unsigned int high=0;high<2u;++high ) {
					const unsigned int side=2u*axis+high;
					if( request.boundary[side]!=FireProductionProjectionPressureOpen||
						coordinate[axis]!=(high?extent[axis]-1u:0u) ) continue;
					std::size_t fx=x,fy=y,fz=z;if( axis==0u&&high ) ++fx;
					if( axis==1u&&high ) ++fy;if( axis==2u&&high ) ++fz;
					const float coefficient=2.0f*fine.inverseFaceDensity[axis][
						FaceIndex(nx,ny,axis,fx,fy,fz)]/
						(shape.cellWidthM*shape.cellWidthM);
					fine.rhs[cell]+=coefficient*boundaryPressure[side][
						SideFaceIndex(shape,side,x,y,z)];
				}
			}
		const bool nullspace=!HasOpenBoundary(request.boundary);
		if( nullspace ) result.removedFineRightHandSideMean=RemoveMean(fine.rhs);
		fine.pressure.assign(cells,0.0f);fine.temporary.assign(cells,0.0f);
		fine.residual.assign(cells,0.0f);
		std::vector<Level> hierarchy;hierarchy.push_back(std::move(fine));
		while( hierarchy.back().nx>4u||hierarchy.back().ny>4u||hierarchy.back().nz>4u ) {
			const Level& parent=hierarchy.back();Level coarse;
			coarse.nx=parent.nx>4u?(parent.nx+1u)/2u:parent.nx;
			coarse.ny=parent.ny>4u?(parent.ny+1u)/2u:parent.ny;
			coarse.nz=parent.nz>4u?(parent.nz+1u)/2u:parent.nz;
			coarse.spacing[0]=parent.spacing[0]*static_cast<float>(parent.nx)/
				static_cast<float>(coarse.nx);
			coarse.spacing[1]=parent.spacing[1]*static_cast<float>(parent.ny)/
				static_cast<float>(coarse.ny);
			coarse.spacing[2]=parent.spacing[2]*static_cast<float>(parent.nz)/
				static_cast<float>(coarse.nz);
			RestrictAverage(parent,coarse,parent.density,coarse.density);
			BuildLevelCoefficients(coarse,request.boundary,request.ambientDensityKGPerM3);
			const std::size_t count=coarse.nx*coarse.ny*coarse.nz;
			coarse.rhs.assign(count,0.0f);coarse.pressure.assign(count,0.0f);
			coarse.temporary.assign(count,0.0f);coarse.residual.assign(count,0.0f);
			hierarchy.push_back(std::move(coarse));
		}
		for( unsigned int cycle=0;cycle<12u;++cycle ) {
			VCycle(hierarchy,0u,request.boundary,nullspace,
				result.executedJacobiSweepCount);
			++result.executedVCycleCount;
			if( nullspace ) RemoveMean(hierarchy[0].pressure);
		}
		result.pressurePa=hierarchy[0].pressure;

		for( unsigned int axis=0;axis<3u;++axis ) {
			const std::size_t ex=axis==0u?nx+1u:nx;
			const std::size_t ey=axis==1u?ny+1u:ny;
			const std::size_t ez=axis==2u?nz+1u:nz;
			for( std::size_t z=0;z<ez;++z ) for( std::size_t y=0;y<ey;++y )
				for( std::size_t x=0;x<ex;++x ) {
					const std::size_t face=FaceIndex(nx,ny,axis,x,y,z);
					const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
					const std::size_t extent=axis==0u?nx:(axis==1u?ny:nz);
					float gradient=0.0f;
					if( coordinate>0u&&coordinate<extent ) {
						std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
						if( axis==0u ) --lx;if( axis==1u ) --ly;if( axis==2u ) --lz;
						gradient=(result.pressurePa[CellIndex(nx,ny,hx,hy,hz)]-
							result.pressurePa[CellIndex(nx,ny,lx,ly,lz)])/shape.cellWidthM;
					} else {
						const unsigned int side=2u*axis+(coordinate?1u:0u);
						if( request.boundary[side]==FireProductionProjectionWall ) {
							result.momentumKGPerM2S[axis][face]=0.0f;
							result.velocityMPerS[axis][face]=0.0f;continue;
						}
						std::size_t cx=x,cy=y,cz=z;
						if( axis==0u ) cx=coordinate?nx-1u:0u;
						if( axis==1u ) cy=coordinate?ny-1u:0u;
						if( axis==2u ) cz=coordinate?nz-1u:0u;
						if( request.boundary[side]==FireProductionProjectionPeriodic ) {
							std::size_t ox=cx,oy=cy,oz=cz;
							if( axis==0u ) {cx=nx-1u;ox=0u;}
							if( axis==1u ) {cy=ny-1u;oy=0u;}
							if( axis==2u ) {cz=nz-1u;oz=0u;}
							gradient=(result.pressurePa[CellIndex(nx,ny,ox,oy,oz)]-
								result.pressurePa[CellIndex(nx,ny,cx,cy,cz)])/shape.cellWidthM;
						} else {
							const float pb=boundaryPressure[side][SideFaceIndex(shape,side,cx,cy,cz)];
							gradient=coordinate?2.0f*(pb-result.pressurePa[
								CellIndex(nx,ny,cx,cy,cz)])/shape.cellWidthM:
								2.0f*(result.pressurePa[CellIndex(nx,ny,cx,cy,cz)]-pb)/
								shape.cellWidthM;
						}
					}
					result.momentumKGPerM2S[axis][face]=
						request.provisionalMomentumKGPerM2S[axis][face]-request.timeStepS*gradient;
					result.velocityMPerS[axis][face]=result.momentumKGPerM2S[axis][face]/
						result.faceDensityKGPerM3[axis][face];
				}
			if( request.boundary[2u*axis]==FireProductionProjectionPeriodic ) {
				// The positive API seam is a publication duplicate, never a second solve.
				const std::size_t firstCount=axis==0u?ny:nx;
				const std::size_t secondCount=axis==2u?ny:nz;
				for( std::size_t second=0;second<secondCount;++second )
					for( std::size_t first=0;first<firstCount;++first ) {
						std::size_t x0=0u,y0=0u,z0=0u,x1=0u,y1=0u,z1=0u;
						if( axis==0u ) {x1=nx;y0=y1=first;z0=z1=second;}
						if( axis==1u ) {y1=ny;x0=x1=first;z0=z1=second;}
						if( axis==2u ) {z1=nz;x0=x1=first;y0=y1=second;}
						const std::size_t low=FaceIndex(nx,ny,axis,x0,y0,z0);
						const std::size_t high=FaceIndex(nx,ny,axis,x1,y1,z1);
						result.faceDensityKGPerM3[axis][high]=result.faceDensityKGPerM3[axis][low];
						result.momentumKGPerM2S[axis][high]=result.momentumKGPerM2S[axis][low];
						result.velocityMPerS[axis][high]=result.velocityMPerS[axis][low];
					}
			}
			for( const float value : result.velocityMPerS[axis] )
				maximumVelocity=std::max(maximumVelocity,std::fabs(value));
		}
		for( std::size_t z=0;z<nz;++z ) for( std::size_t y=0;y<ny;++y )
			for( std::size_t x=0;x<nx;++x ) {
				const std::size_t cell=CellIndex(nx,ny,x,y,z);
				result.maximumPostProjectionResidualPerS=std::max(
					result.maximumPostProjectionResidualPerS,std::fabs(
						divergence(result.velocityMPerS,x,y,z)-request.divergenceTargetPerS[cell]));
			}
		for( unsigned int side=0;side<6u;++side ) {
			if( request.boundary[side]!=FireProductionProjectionPressureOpen ) continue;
			const unsigned int axis=side/2u;const bool positive=(side&1u)!=0u;
			const std::size_t firstCount=axis==0u?ny:nx;
			const std::size_t secondCount=axis==2u?ny:nz;
			for( std::size_t second=0;second<secondCount;++second )
				for( std::size_t first=0;first<firstCount;++first ) {
					std::size_t x=0u,y=0u,z=0u;
					if( axis==0u ) {x=positive?nx:0u;y=first;z=second;}
					if( axis==1u ) {x=first;y=positive?ny:0u;z=second;}
					if( axis==2u ) {x=first;y=second;z=positive?nz:0u;}
					const std::size_t face=FaceIndex(nx,ny,axis,x,y,z);
					const float outward=(positive?1.0f:-1.0f)*result.velocityMPerS[axis][face];
					const std::size_t cx=axis==0u?(positive?nx-1u:0u):x;
					const std::size_t cy=axis==1u?(positive?ny-1u:0u):y;
					const std::size_t cz=axis==2u?(positive?nz-1u:0u):z;
					const bool inflow=result.pressureOpenInflow[side][SideFaceIndex(shape,side,cx,cy,cz)]!=0u;
					const float discrepancy=inflow?std::max(0.0f,outward):std::max(0.0f,-outward);
					result.maximumOpenComplementarityDiscrepancyMPerS=std::max(
						result.maximumOpenComplementarityDiscrepancyMPerS,discrepancy);
				}
		}
		const float length=shape.cellWidthM*static_cast<float>(
			std::max(nx,std::max(ny,nz)));
		if( !FireProductionProjectionResidualWithinBand(
			result.maximumPostProjectionResidualPerS,maximumVelocity,length,
			result.validationPassed) ) {
			result=FireProductionProjectionResult();
			return Fail(error,"production projection validation band overflowed");
		}
		for( const float value : result.pressurePa ) if( !std::isfinite(value) ) {
			result=FireProductionProjectionResult();
			return Fail(error,"production projection produced nonfinite pressure");
		}
		for( unsigned int axis=0;axis<3u;++axis ) {
			for( const float value:result.faceDensityKGPerM3[axis] )
				if( !std::isfinite(value)||!(value>0.0f) ) {
					result=FireProductionProjectionResult();
					return Fail(error,"production projection produced invalid face density");
				}
			for( const float value:result.momentumKGPerM2S[axis] )
				if( !std::isfinite(value) ) {
					result=FireProductionProjectionResult();
					return Fail(error,"production projection produced nonfinite momentum");
				}
			for( const float value:result.velocityMPerS[axis] )
				if( !std::isfinite(value) ) {
					result=FireProductionProjectionResult();
					return Fail(error,"production projection produced nonfinite velocity");
				}
		}
		if( !std::isfinite(result.maximumPreProjectionResidualPerS)||
			!std::isfinite(result.maximumPostProjectionResidualPerS)||
			!std::isfinite(result.maximumOpenComplementarityDiscrepancyMPerS) ) {
			result=FireProductionProjectionResult();
			return Fail(error,"production projection produced nonfinite diagnostics");
		}
		if( error ) error->clear();
		return true;
	}

	bool ProjectFireProductionCPU( const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result, std::string* error )
	{
		try {
			return ProjectFireProductionCPUImplementation(request,result,error);
		} catch( const std::bad_alloc& ) {
			result=FireProductionProjectionResult();
			if( error ) {
				try { *error="production projection allocation failed"; }
				catch( const std::bad_alloc& ) { error->clear(); }
			}
			return false;
		}
	}
}
