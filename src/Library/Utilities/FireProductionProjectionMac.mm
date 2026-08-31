//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjectionMac.mm - Metal-resident fixed P2 projection
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "FireProductionProjection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <new>

namespace RISE
{
	bool ValidateFireProductionRestorationCycleProbe(
		unsigned int& cycleCount,bool& enabled,std::string* error )
	{
		cycleCount=0u;enabled=false;
		const char* value=std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_CYCLE_PROBE");
		if( !value ) return true;
		const char* activation=std::getenv("RISE_FIRE_RESTORATION_PLATEAU_PROBE");
		if( !activation||std::strcmp(activation,"1")!=0||!*value ) {
			if( error ) *error="production fire restoration cycle probe is not authorized";
			return false;
		}
		unsigned int parsed=0u;
		for( const char* digit=value;*digit;++digit ) {
			if( *digit<'0'||*digit>'9'||parsed>16u ) {
				if( error ) *error="production fire restoration cycle probe is malformed";
				return false;
			}
			parsed=10u*parsed+static_cast<unsigned int>(*digit-'0');
		}
		if( parsed<1u||parsed>16u ) {
			if( error ) *error="production fire restoration cycle probe is outside 1..16";
			return false;
		}
		cycleCount=parsed;enabled=true;return true;
	}

	namespace
	{
		enum ProjectionHostAccessKind
		{
			ProjectionMetadataAccess,
			ProjectionDiagnosticAccess,
			ProjectionInterstageFullGridAccess,
			ProjectionTerminalAccess
		};
		enum ProjectionTransferPhase
		{
			ProjectionUploadTransfer,
			ProjectionInterstageTransfer,
			ProjectionTerminalTransfer
		};

		thread_local std::uint64_t projectionCommandCommitCount=0u;
		thread_local std::uint64_t projectionInterstageFullGridReadCount=0u;
		thread_local std::uint64_t projectionInvocationCount=0u;
		thread_local ProjectionTransferPhase projectionTransferPhase=ProjectionInterstageTransfer;
		thread_local std::uint64_t projectionBufferAllocationCount=0u;
		thread_local std::uint64_t projectionBufferAllocationBytes=0u;

		struct ProjectionTransferScope
		{
			ProjectionTransferPhase previous;
			explicit ProjectionTransferScope( ProjectionTransferPhase phase ) :
				previous(projectionTransferPhase) {projectionTransferPhase=phase;}
			~ProjectionTransferScope() {projectionTransferPhase=previous;}
		};

		void CommitProjectionCommand( id<MTLCommandBuffer> command )
		{
			[command commit];++projectionCommandCommitCount;
		}

		void* ProjectionBufferContents( id<MTLBuffer> buffer,
			ProjectionHostAccessKind kind )
		{
			if( kind==ProjectionInterstageFullGridAccess )
				++projectionInterstageFullGridReadCount;
			return [buffer contents];
		}

		void CopyProjectionBuffer( id<MTLBlitCommandEncoder> encoder,
			id<MTLBuffer> source,std::size_t sourceOffset,id<MTLBuffer> destination,
			std::size_t destinationOffset,std::size_t size )
		{
			[encoder copyFromBuffer:source sourceOffset:sourceOffset toBuffer:destination
				destinationOffset:destinationOffset size:size];
			const bool sourceVisible=[source storageMode]!=MTLStorageModePrivate;
			const bool destinationVisible=[destination storageMode]!=MTLStorageModePrivate;
			if( projectionTransferPhase==ProjectionInterstageTransfer&&
				sourceVisible!=destinationVisible )
				++projectionInterstageFullGridReadCount;
		}

		void ObserveProjectionInvocation()
		{
			++projectionInvocationCount;
		}

		id<MTLBuffer> ObserveProjectionAllocation( id<MTLBuffer> buffer )
		{
			if( !buffer ) return nil;
			const std::uint64_t bytes=[buffer allocatedSize];
			if( projectionBufferAllocationCount==std::numeric_limits<std::uint64_t>::max()||
				projectionBufferAllocationBytes>std::numeric_limits<std::uint64_t>::max()-bytes )
				return nil;
			++projectionBufferAllocationCount;projectionBufferAllocationBytes+=bytes;
			return buffer;
		}
		struct MetalLevelParameters
		{
			std::uint32_t nx,ny,nz,nullspace;
			float sx,sy,sz,ambientDensity;
			float timeStepS;
			std::uint32_t restoration;
			std::uint32_t boundary[6];
			std::uint32_t sideOffset[6];
		};
		static_assert(sizeof(MetalLevelParameters)==88u,
			"P2 working-set certificate binds the serialized Metal level payload");

		struct MetalAxisParameters
		{
			std::uint32_t axis,storeDensity,side,padding;
		};

		struct MetalReductionParameters
		{
			std::uint32_t count,width,offset,diagnosticIndex;
		};

		struct MetalLevel
		{
			std::size_t nx,ny,nz;
			float spacing[3];
			id<MTLBuffer> density,rhs,pressure,temporary,residual,diagonal;
			id<MTLBuffer> beta[3];
			id<MTLBuffer> parameters;
		};

		std::string MetalError( const char* prefix, NSError* error )
		{
			std::string result(prefix);
			if( error ) result += " ["+std::string([[error domain] UTF8String])+" "+
				std::to_string(static_cast<long>([error code]))+"] "+
				std::string([[error localizedDescription] UTF8String]);
			return result;
		}

		const char* ProjectionSource()
		{
			return R"METAL(
#include <metal_stdlib>
using namespace metal;
struct LevelParams {
 uint nx,ny,nz,nullspace;float sx,sy,sz,ambient,dt;uint restoration;uint boundary[6];uint sideOffset[6];
};
struct AxisParams {uint axis,storeDensity,side,padding;};
struct ReductionParams {uint count,width,offset,diagnosticIndex;};
inline uint cell_index(constant LevelParams& p,uint x,uint y,uint z){return (z*p.ny+y)*p.nx+x;}
inline uint face_count(constant LevelParams& p,uint axis){
 return axis==0u?(p.nx+1u)*p.ny*p.nz:(axis==1u?p.nx*(p.ny+1u)*p.nz:p.nx*p.ny*(p.nz+1u));
}
inline uint face_index(constant LevelParams& p,uint axis,uint x,uint y,uint z){
 if(axis==0u)return (z*p.ny+y)*(p.nx+1u)+x;
 if(axis==1u)return (z*(p.ny+1u)+y)*p.nx+x;
 return (z*p.ny+y)*p.nx+x;
}
inline void face_coordinate(constant LevelParams& p,uint axis,uint face,
 thread uint& x,thread uint& y,thread uint& z){
 if(axis==0u){x=face%(p.nx+1u);uint r=face/(p.nx+1u);y=r%p.ny;z=r/p.ny;return;}
 if(axis==1u){x=face%p.nx;uint r=face/p.nx;y=r%(p.ny+1u);z=r/(p.ny+1u);return;}
 x=face%p.nx;uint r=face/p.nx;y=r%p.ny;z=r/p.ny;
}
inline float stored_face_density(device const float* rho,constant LevelParams& p,
 uint axis,uint x,uint y,uint z){
 uint coordinate=axis==0u?x:(axis==1u?y:z),extent=axis==0u?p.nx:(axis==1u?p.ny:p.nz);
 uint lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
 if(coordinate>0u&&coordinate<extent){if(axis==0u)--lx;if(axis==1u)--ly;if(axis==2u)--lz;
  return 0.5f*rho[cell_index(p,lx,ly,lz)]+0.5f*rho[cell_index(p,hx,hy,hz)];}
 uint side=2u*axis+(coordinate?1u:0u);
 if(p.boundary[side]==0u){
  if(axis==0u){lx=p.nx-1u;hx=0u;}if(axis==1u){ly=p.ny-1u;hy=0u;}
  if(axis==2u){lz=p.nz-1u;hz=0u;}
  return 0.5f*rho[cell_index(p,lx,ly,lz)]+0.5f*rho[cell_index(p,hx,hy,hz)];
 }
 if(axis==0u)hx=coordinate?p.nx-1u:0u;if(axis==1u)hy=coordinate?p.ny-1u:0u;
 if(axis==2u)hz=coordinate?p.nz-1u:0u;float interior=rho[cell_index(p,hx,hy,hz)];
 return p.boundary[side]==1u?0.5f*interior+0.5f*p.ambient:interior;
}
kernel void build_faces(device const float* rho [[buffer(0)]],device float* beta [[buffer(1)]],
 device float* stored [[buffer(2)]],constant LevelParams& p [[buffer(3)]],
 constant AxisParams& a [[buffer(4)]],uint gid [[thread_position_in_grid]]){
 if(gid>=face_count(p,a.axis))return;uint x,y,z;face_coordinate(p,a.axis,gid,x,y,z);
 uint coordinate=a.axis==0u?x:(a.axis==1u?y:z),extent=a.axis==0u?p.nx:(a.axis==1u?p.ny:p.nz);
 uint side=2u*a.axis+(coordinate?1u:0u);float d=stored_face_density(rho,p,a.axis,x,y,z);
 bool wall=(coordinate==0u||coordinate==extent)&&p.boundary[side]==2u;
 beta[gid]=wall?0.0f:1.0f/d;if(a.storeDensity!=0u)stored[gid]=d;
}
kernel void build_diagonal(device const float* bx [[buffer(0)]],device const float* by [[buffer(1)]],
 device const float* bz [[buffer(2)]],device float* diagonal [[buffer(3)]],
 constant LevelParams& p [[buffer(4)]],uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float value=0.0f;device const float* beta[3]={bx,by,bz};
 for(uint axis=0u;axis<3u;++axis){uint c=axis==0u?x:(axis==1u?y:z),n=axis==0u?p.nx:(axis==1u?p.ny:p.nz);
  uint hx=x,hy=y,hz=z;if(axis==0u)++hx;if(axis==1u)++hy;if(axis==2u)++hz;
  float spacing=axis==0u?p.sx:(axis==1u?p.sy:p.sz),scale=1.0f/(spacing*spacing);
  float lo=beta[axis][face_index(p,axis,x,y,z)]*scale,hi=beta[axis][face_index(p,axis,hx,hy,hz)]*scale;
  value+=(c>0u||p.boundary[2u*axis]==0u)?lo:(p.boundary[2u*axis]==1u?2.0f*lo:0.0f);
  value+=(c+1u<n||p.boundary[2u*axis+1u]==0u)?hi:(p.boundary[2u*axis+1u]==1u?2.0f*hi:0.0f);
 }
 diagonal[gid]=value;
}
kernel void restrict_average(device const float* fine [[buffer(0)]],device float* coarse [[buffer(1)]],
 constant LevelParams& f [[buffer(2)]],constant LevelParams& c [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){
 uint count=c.nx*c.ny*c.nz;if(gid>=count)return;uint cx=gid%c.nx,r=gid/c.nx,cy=r%c.ny,cz=r/c.ny;
 uint x0=f.nx==c.nx?cx:2u*cx,x1=f.nx==c.nx?x0:min(f.nx-1u,x0+1u);
 uint y0=f.ny==c.ny?cy:2u*cy,y1=f.ny==c.ny?y0:min(f.ny-1u,y0+1u);
 uint z0=f.nz==c.nz?cz:2u*cz,z1=f.nz==c.nz?z0:min(f.nz-1u,z0+1u);
 float sum=0.0f;uint n=0u;for(uint z=z0;z<=z1;++z)for(uint y=y0;y<=y1;++y)for(uint x=x0;x<=x1;++x){
  sum+=fine[cell_index(f,x,y,z)];++n;}coarse[gid]=sum/float(n);
}
kernel void setup_velocity(device const float* momentumIn [[buffer(0)]],device const float* stored [[buffer(1)]],
 device float* momentumOut [[buffer(2)]],device float* velocity [[buffer(3)]],
 constant LevelParams& p [[buffer(4)]],constant AxisParams& a [[buffer(5)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=face_count(p,a.axis))return;uint x,y,z;face_coordinate(p,a.axis,gid,x,y,z);
 uint coordinate=a.axis==0u?x:(a.axis==1u?y:z),extent=a.axis==0u?p.nx:(a.axis==1u?p.ny:p.nz);
 uint side=2u*a.axis+(coordinate?1u:0u);bool wall=(coordinate==0u||coordinate==extent)&&p.boundary[side]==2u;
 momentumOut[gid]=wall?0.0f:momentumIn[gid];velocity[gid]=wall?0.0f:momentumIn[gid]/stored[gid];
}
inline float centered_velocity(device const float* velocity,constant LevelParams& p,
 uint axis,uint x,uint y,uint z){uint hx=x,hy=y,hz=z;if(axis==0u)++hx;if(axis==1u)++hy;if(axis==2u)++hz;
 return 0.5f*(velocity[face_index(p,axis,x,y,z)]+velocity[face_index(p,axis,hx,hy,hz)]);
}
inline uint side_face_count(constant LevelParams& p,uint side){return side<2u?p.ny*p.nz:(side<4u?p.nx*p.nz:p.nx*p.ny);}
kernel void classify_open(device const float* vx [[buffer(0)]],device const float* vy [[buffer(1)]],
 device const float* vz [[buffer(2)]],device uchar* inflow [[buffer(3)]],device float* pb [[buffer(4)]],
 constant LevelParams& p [[buffer(5)]],constant AxisParams& a [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 uint side=a.side;if(gid>=side_face_count(p,side))return;uint axis=side/2u,positive=side&1u;
 uint first=axis==0u?p.ny:p.nx;uint u=gid%first,v=gid/first,x=0u,y=0u,z=0u;
 if(axis==0u){x=positive?p.nx:0u;y=u;z=v;}if(axis==1u){x=u;y=positive?p.ny:0u;z=v;}
 if(axis==2u){x=u;y=v;z=positive?p.nz:0u;}device const float* vel[3]={vx,vy,vz};
 float normal=vel[axis][face_index(p,axis,x,y,z)],outward=positive?normal:-normal;
 uint cx=axis==0u?(positive?p.nx-1u:0u):x,cy=axis==1u?(positive?p.ny-1u:0u):y;
 uint cz=axis==2u?(positive?p.nz-1u:0u):z;uint inflowIndex=p.sideOffset[side]+gid;
 bool sealedClass=(a.padding&1u)!=0u,sealedHead=(a.padding&2u)!=0u;
 bool inside=sealedClass?inflow[inflowIndex]!=0u:outward<0.0f;
 if(!sealedClass)inflow[inflowIndex]=inside?1u:0u;
 if(!sealedHead){float pressure=0.0f;if(inside&&p.restoration==0u){float speed2=normal*normal;
  for(uint t=0u;t<3u;++t)if(t!=axis){float q=centered_velocity(vel[t],p,t,cx,cy,cz);
   speed2+=q*q;}pressure=-0.5f*p.ambient*speed2;}pb[p.sideOffset[side]+gid]=pressure;}
}
kernel void classify_endpoint(device const float* vx [[buffer(0)]],device const float* vy [[buffer(1)]],
 device const float* vz [[buffer(2)]],device uchar* inflow [[buffer(3)]],
 constant LevelParams& p [[buffer(4)]],constant AxisParams& a [[buffer(5)]],
 constant float& tolerance [[buffer(6)]],uint gid [[thread_position_in_grid]]){
 uint side=a.side;if(gid>=side_face_count(p,side))return;uint axis=side/2u,positive=side&1u;
 uint first=axis==0u?p.ny:p.nx;uint u=gid%first,v=gid/first,x=0u,y=0u,z=0u;
 if(axis==0u){x=positive?p.nx:0u;y=u;z=v;}if(axis==1u){x=u;y=positive?p.ny:0u;z=v;}
 if(axis==2u){x=u;y=v;z=positive?p.nz:0u;}device const float* vel[3]={vx,vy,vz};
 float outward=(positive?1.0f:-1.0f)*vel[axis][face_index(p,axis,x,y,z)];
 uint index=p.sideOffset[side]+gid;if(outward < -tolerance)inflow[index]=1u;
 else if(outward > tolerance)inflow[index]=0u;
}
kernel void build_rhs(device const float* vx [[buffer(0)]],device const float* vy [[buffer(1)]],
 device const float* vz [[buffer(2)]],device const float* target [[buffer(3)]],
 device const float* bx [[buffer(4)]],device const float* by [[buffer(5)]],
 device const float* bz [[buffer(6)]],device const float* pb [[buffer(7)]],
 device float* rhs [[buffer(8)]],device float* absoluteResidual [[buffer(9)]],
 constant LevelParams& p [[buffer(10)]],uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float divergence=(vx[face_index(p,0u,x+1u,y,z)]-vx[face_index(p,0u,x,y,z)]+
  vy[face_index(p,1u,x,y+1u,z)]-vy[face_index(p,1u,x,y,z)]+
  vz[face_index(p,2u,x,y,z+1u)]-vz[face_index(p,2u,x,y,z)])/p.sx;
 float residual=p.restoration!=0u?-target[gid]:divergence-target[gid];
 float value=-residual/p.dt;absoluteResidual[gid]=abs(residual);
 device const float* beta[3]={bx,by,bz};uint coordinate[3]={x,y,z},extent[3]={p.nx,p.ny,p.nz};
 for(uint axis=0u;axis<3u;++axis)for(uint high=0u;high<2u;++high){uint side=2u*axis+high;
  if(p.restoration!=0u||p.boundary[side]!=1u||coordinate[axis]!=(high?extent[axis]-1u:0u))continue;
  uint fx=x,fy=y,fz=z;if(axis==0u&&high!=0u)++fx;if(axis==1u&&high!=0u)++fy;if(axis==2u&&high!=0u)++fz;
  uint sideIndex=side<2u?z*p.ny+y:(side<4u?z*p.nx+x:y*p.nx+x);
  float spacing=axis==0u?p.sx:(axis==1u?p.sy:p.sz);
  value+=2.0f*beta[axis][face_index(p,axis,fx,fy,fz)]/(spacing*spacing)*pb[p.sideOffset[side]+sideIndex];
 }
 rhs[gid]=value;
}
inline float apply_operator(device const float* pressure,device const float* bx,device const float* by,
 device const float* bz,constant LevelParams& p,uint x,uint y,uint z){
 uint cell=cell_index(p,x,y,z),coordinate[3]={x,y,z},extent[3]={p.nx,p.ny,p.nz};
 device const float* beta[3]={bx,by,bz};float value=0.0f;
 for(uint axis=0u;axis<3u;++axis){uint hx=x,hy=y,hz=z;if(axis==0u)++hx;if(axis==1u)++hy;if(axis==2u)++hz;
  float spacing=axis==0u?p.sx:(axis==1u?p.sy:p.sz),scale=1.0f/(spacing*spacing);
  float lo=beta[axis][face_index(p,axis,x,y,z)]*scale,hi=beta[axis][face_index(p,axis,hx,hy,hz)]*scale;
  if(coordinate[axis]>0u){uint px=x,py=y,pz=z;if(axis==0u)--px;if(axis==1u)--py;if(axis==2u)--pz;
   value+=lo*(pressure[cell]-pressure[cell_index(p,px,py,pz)]);
  }else if(p.boundary[2u*axis]==0u){uint px=x,py=y,pz=z;if(axis==0u)px=p.nx-1u;if(axis==1u)py=p.ny-1u;if(axis==2u)pz=p.nz-1u;
   value+=lo*(pressure[cell]-pressure[cell_index(p,px,py,pz)]);
  }else if(p.boundary[2u*axis]==1u)value+=2.0f*lo*pressure[cell];
  if(coordinate[axis]+1u<extent[axis]){uint px=x,py=y,pz=z;if(axis==0u)++px;if(axis==1u)++py;if(axis==2u)++pz;
   value+=hi*(pressure[cell]-pressure[cell_index(p,px,py,pz)]);
  }else if(p.boundary[2u*axis+1u]==0u){uint px=x,py=y,pz=z;if(axis==0u)px=0u;if(axis==1u)py=0u;if(axis==2u)pz=0u;
   value+=hi*(pressure[cell]-pressure[cell_index(p,px,py,pz)]);
  }else if(p.boundary[2u*axis+1u]==1u)value+=2.0f*hi*pressure[cell];
 }return value;
}
kernel void jacobi(device const float* pressure [[buffer(0)]],device const float* rhs [[buffer(1)]],
 device const float* diagonal [[buffer(2)]],device const float* bx [[buffer(3)]],
 device const float* by [[buffer(4)]],device const float* bz [[buffer(5)]],
 device float* output [[buffer(6)]],constant LevelParams& p [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 output[gid]=pressure[gid]+(2.0f/3.0f)*(rhs[gid]-apply_operator(pressure,bx,by,bz,p,x,y,z))/diagonal[gid];
}
kernel void compute_residual(device const float* pressure [[buffer(0)]],device const float* rhs [[buffer(1)]],
 device const float* bx [[buffer(2)]],device const float* by [[buffer(3)]],device const float* bz [[buffer(4)]],
 device float* residual [[buffer(5)]],constant LevelParams& p [[buffer(6)]],
 uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 residual[gid]=rhs[gid]-apply_operator(pressure,bx,by,bz,p,x,y,z);
}
inline void interp_coord(uint fine,uint fn,uint cn,thread uint& first,thread uint& second,thread float& weight){
 if(fn==cn){first=fine;second=fine;weight=0.0f;return;}float position=(float(fine)+0.5f)*float(cn)/float(fn)-0.5f;
 if(position<=0.0f){first=0u;second=0u;weight=0.0f;return;}if(position>=float(cn-1u)){first=cn-1u;second=first;weight=0.0f;return;}
 first=uint(floor(position));second=first+1u;weight=position-float(first);
}
kernel void prolongate_add(device const float* coarse [[buffer(0)]],device float* fine [[buffer(1)]],
 constant LevelParams& c [[buffer(2)]],constant LevelParams& f [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){
 uint count=f.nx*f.ny*f.nz;if(gid>=count)return;uint x=gid%f.nx,r=gid/f.nx,y=r%f.ny,z=r/f.ny;
 uint x0,x1,y0,y1,z0,z1;float wx,wy,wz;interp_coord(x,f.nx,c.nx,x0,x1,wx);
 interp_coord(y,f.ny,c.ny,y0,y1,wy);interp_coord(z,f.nz,c.nz,z0,z1,wz);float value=0.0f;
 for(uint iz=0u;iz<2u;++iz)for(uint iy=0u;iy<2u;++iy)for(uint ix=0u;ix<2u;++ix){
  float w=(ix?wx:1.0f-wx)*(iy?wy:1.0f-wy)*(iz?wz:1.0f-wz);
  value+=w*coarse[cell_index(c,ix?x1:x0,iy?y1:y0,iz?z1:z0)];}bool pressureOpen=false;
 for(uint side=0u;side<6u;++side)pressureOpen=pressureOpen||f.boundary[side]==1u;
 fine[gid]+=(pressureOpen?0.75f:1.0f)*value;
}
kernel void clear_values(device float* values [[buffer(0)]],constant ReductionParams& r [[buffer(1)]],
 uint gid [[thread_position_in_grid]]){if(gid<r.count)values[gid]=0.0f;}
kernel void copy_reduction(device const float* values [[buffer(0)]],device float* scratch [[buffer(1)]],
 constant ReductionParams& r [[buffer(2)]],uint gid [[thread_position_in_grid]]){
 if(gid<r.width)scratch[gid]=gid<r.count?values[gid]:0.0f;
}
kernel void sum_pass(device float* scratch [[buffer(0)]],constant ReductionParams& r [[buffer(1)]],
 uint gid [[thread_position_in_grid]]){uint index=2u*r.offset-1u+gid*2u*r.offset;if(index<r.width)scratch[index]+=scratch[index-r.offset];}
kernel void max_pass(device float* scratch [[buffer(0)]],constant ReductionParams& r [[buffer(1)]],
 uint gid [[thread_position_in_grid]]){uint index=2u*r.offset-1u+gid*2u*r.offset;if(index<r.width)scratch[index]=max(scratch[index],scratch[index-r.offset]);}
kernel void subtract_mean(device float* values [[buffer(0)]],device const float* scratch [[buffer(1)]],
 constant ReductionParams& r [[buffer(2)]],device float* diagnostics [[buffer(3)]],
 uint gid [[thread_position_in_grid]]){float mean=scratch[r.width-1u]/float(r.count);
 if(gid<r.count)values[gid]-=mean;if(gid==0u&&r.diagnosticIndex!=0xffffffffu)diagnostics[r.diagnosticIndex]=mean;}
kernel void store_root(device const float* scratch [[buffer(0)]],device float* diagnostics [[buffer(1)]],
 constant ReductionParams& r [[buffer(2)]],uint gid [[thread_position_in_grid]]){
 if(gid==0u)diagnostics[r.diagnosticIndex]=scratch[r.width-1u];
}
kernel void correct_faces(device const float* provisional [[buffer(0)]],device const float* stored [[buffer(1)]],
 device const float* pressure [[buffer(2)]],device const float* pb [[buffer(3)]],
 device float* momentum [[buffer(4)]],device float* velocity [[buffer(5)]],
 constant LevelParams& p [[buffer(6)]],constant AxisParams& a [[buffer(7)]],
 uint gid [[thread_position_in_grid]]){
 if(gid>=face_count(p,a.axis))return;uint x,y,z;face_coordinate(p,a.axis,gid,x,y,z);
 uint coordinate=a.axis==0u?x:(a.axis==1u?y:z),extent=a.axis==0u?p.nx:(a.axis==1u?p.ny:p.nz);
 uint side=2u*a.axis+(coordinate?1u:0u);float gradient=0.0f;
 if(coordinate>0u&&coordinate<extent){uint lx=x,ly=y,lz=z;if(a.axis==0u)--lx;if(a.axis==1u)--ly;if(a.axis==2u)--lz;
  gradient=(pressure[cell_index(p,x,y,z)]-pressure[cell_index(p,lx,ly,lz)])/(a.axis==0u?p.sx:(a.axis==1u?p.sy:p.sz));
 }else if(p.boundary[side]==2u){momentum[gid]=0.0f;velocity[gid]=0.0f;return;
 }else{uint cx=x,cy=y,cz=z;if(a.axis==0u)cx=coordinate?p.nx-1u:0u;if(a.axis==1u)cy=coordinate?p.ny-1u:0u;
  if(a.axis==2u)cz=coordinate?p.nz-1u:0u;float spacing=a.axis==0u?p.sx:(a.axis==1u?p.sy:p.sz);
  if(p.boundary[side]==0u){uint ox=cx,oy=cy,oz=cz;if(a.axis==0u){cx=p.nx-1u;ox=0u;}
   if(a.axis==1u){cy=p.ny-1u;oy=0u;}if(a.axis==2u){cz=p.nz-1u;oz=0u;}
   gradient=(pressure[cell_index(p,ox,oy,oz)]-pressure[cell_index(p,cx,cy,cz)])/spacing;
  }else{uint sideIndex=side<2u?cz*p.ny+cy:(side<4u?cz*p.nx+cx:cy*p.nx+cx);
   float boundaryPressure=pb[p.sideOffset[side]+sideIndex];gradient=coordinate?
    2.0f*(boundaryPressure-pressure[cell_index(p,cx,cy,cz)])/spacing:
    2.0f*(pressure[cell_index(p,cx,cy,cz)]-boundaryPressure)/spacing;}
 }
 momentum[gid]=provisional[gid]-p.dt*gradient;velocity[gid]=momentum[gid]/stored[gid];
}
kernel void copy_periodic_seam(device float* stored [[buffer(0)]],device float* momentum [[buffer(1)]],
 device float* velocity [[buffer(2)]],constant LevelParams& p [[buffer(3)]],
 constant AxisParams& a [[buffer(4)]],uint gid [[thread_position_in_grid]]){
 uint first=a.axis==0u?p.ny:p.nx,second=a.axis==2u?p.ny:p.nz,count=first*second;if(gid>=count)return;
 uint u=gid%first,v=gid/first,x0=0u,y0=0u,z0=0u,x1=0u,y1=0u,z1=0u;
 if(a.axis==0u){x1=p.nx;y0=y1=u;z0=z1=v;}if(a.axis==1u){y1=p.ny;x0=x1=u;z0=z1=v;}
 if(a.axis==2u){z1=p.nz;x0=x1=u;y0=y1=v;}uint lo=face_index(p,a.axis,x0,y0,z0),hi=face_index(p,a.axis,x1,y1,z1);
 stored[hi]=stored[lo];momentum[hi]=momentum[lo];velocity[hi]=velocity[lo];
}
inline float prescribed_provisional_velocity(device const float* momentum,device const float* stored,
 constant LevelParams& p,uint axis,uint x,uint y,uint z){
 uint coordinate=axis==0u?x:(axis==1u?y:z),extent=axis==0u?p.nx:(axis==1u?p.ny:p.nz);
 uint side=2u*axis+(coordinate==extent?1u:0u);
 return (coordinate==0u||coordinate==extent)&&p.boundary[side]==2u?0.0f:
  momentum[face_index(p,axis,x,y,z)]/stored[face_index(p,axis,x,y,z)];
}
kernel void cell_post_residual(device const float* vx [[buffer(0)]],device const float* vy [[buffer(1)]],
 device const float* vz [[buffer(2)]],device const float* target [[buffer(3)]],device float* output [[buffer(4)]],
 constant LevelParams& p [[buffer(5)]],device const float* px [[buffer(6)]],
 device const float* py [[buffer(7)]],device const float* pz [[buffer(8)]],
 device const float* dx [[buffer(9)]],device const float* dy [[buffer(10)]],
 device const float* dz [[buffer(11)]],uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float divergence=(vx[face_index(p,0u,x+1u,y,z)]-vx[face_index(p,0u,x,y,z)]+
  vy[face_index(p,1u,x,y+1u,z)]-vy[face_index(p,1u,x,y,z)]+
  vz[face_index(p,2u,x,y,z+1u)]-vz[face_index(p,2u,x,y,z)])/p.sx;
 if(p.restoration==0u){output[gid]=abs(divergence-target[gid]);return;}
 float beginning=(prescribed_provisional_velocity(px,dx,p,0u,x+1u,y,z)-
  prescribed_provisional_velocity(px,dx,p,0u,x,y,z)+
  prescribed_provisional_velocity(py,dy,p,1u,x,y+1u,z)-
  prescribed_provisional_velocity(py,dy,p,1u,x,y,z)+
  prescribed_provisional_velocity(pz,dz,p,2u,x,y,z+1u)-
  prescribed_provisional_velocity(pz,dz,p,2u,x,y,z))/p.sx;
 output[gid]=abs((divergence-beginning)-target[gid]);
}
kernel void cell_validation_metrics(device const float* px [[buffer(0)]],
 device const float* py [[buffer(1)]],device const float* pz [[buffer(2)]],
 device const float* dx [[buffer(3)]],device const float* dy [[buffer(4)]],
 device const float* dz [[buffer(5)]],device const float* vx [[buffer(6)]],
 device const float* vy [[buffer(7)]],device const float* vz [[buffer(8)]],
 device const uchar* inflow [[buffer(9)]],device float* maximumVelocity [[buffer(10)]],
 device float* complementarity [[buffer(11)]],constant LevelParams& p [[buffer(12)]],
 uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 device const float* provisional[3]={px,py,pz};device const float* stored[3]={dx,dy,dz};
 device const float* velocity[3]={vx,vy,vz};uint coordinate[3]={x,y,z},extent[3]={p.nx,p.ny,p.nz};
 float mv=0.0f,mc=0.0f;for(uint axis=0u;axis<3u;++axis)for(uint high=0u;high<2u;++high){
  uint fx=x,fy=y,fz=z;if(axis==0u&&high!=0u)++fx;if(axis==1u&&high!=0u)++fy;if(axis==2u&&high!=0u)++fz;
  uint face=face_index(p,axis,fx,fy,fz),side=2u*axis+high;
  bool endpoint=coordinate[axis]==(high?extent[axis]-1u:0u),wall=endpoint&&p.boundary[side]==2u;
  if(!wall)mv=max(mv,abs(provisional[axis][face]/stored[axis][face]));
  float finalVelocity=velocity[axis][face];mv=max(mv,abs(finalVelocity));
  if(endpoint&&p.boundary[side]==1u){uint sideIndex=side<2u?z*p.ny+y:(side<4u?z*p.nx+x:y*p.nx+x);
   bool inside=inflow[p.sideOffset[side]+sideIndex]!=0u;float outward=(high?1.0f:-1.0f)*finalVelocity;
   mc=max(mc,inside?max(0.0f,outward):max(0.0f,-outward));}
 }
 maximumVelocity[gid]=mv;complementarity[gid]=mc;
}
)METAL";
		}

		struct MetalProjectionContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> buildFaces,buildDiagonal,restrictAverage,
				setupVelocity,classifyOpen,classifyEndpoint,buildRHS,jacobi,computeResidual,
				prolongate,clearValues,copyReduction,sumPass,maxPass,subtractMean,
				storeRoot,correctFaces,copySeam,postResidual,validationMetrics;
			std::string error;

			MetalProjectionContext() : device(nil),queue(nil),buildFaces(nil),buildDiagonal(nil),
				restrictAverage(nil),setupVelocity(nil),classifyOpen(nil),classifyEndpoint(nil),buildRHS(nil),
				jacobi(nil),computeResidual(nil),prolongate(nil),clearValues(nil),
				copyReduction(nil),sumPass(nil),maxPass(nil),subtractMean(nil),storeRoot(nil),
				correctFaces(nil),copySeam(nil),postResidual(nil),validationMetrics(nil)
			{
				@autoreleasepool {
					device=MTLCreateSystemDefaultDevice();
					if( !device ) {error="production fire projection has no Metal device";return;}
					MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
					if( @available(macOS 15.0,*) ) options.mathMode=MTLMathModeSafe;
					else {error="production fire projection requires Metal safe math mode";return;}
					NSError* metalError=nil;
					id<MTLLibrary> library=[device newLibraryWithSource:
						[NSString stringWithUTF8String:ProjectionSource()] options:options error:&metalError];
					if( !library ) {error=MetalError("production fire projection library compilation failed",metalError);return;}
					auto make=[&](const char* name)->id<MTLComputePipelineState>{
						id<MTLFunction> fn=[library newFunctionWithName:[NSString stringWithUTF8String:name]];
						return fn?[device newComputePipelineStateWithFunction:fn error:&metalError]:nil;};
					buildFaces=make("build_faces");buildDiagonal=make("build_diagonal");
				restrictAverage=make("restrict_average");setupVelocity=make("setup_velocity");
				classifyOpen=make("classify_open");classifyEndpoint=make("classify_endpoint");
				buildRHS=make("build_rhs");jacobi=make("jacobi");
					computeResidual=make("compute_residual");prolongate=make("prolongate_add");
					clearValues=make("clear_values");copyReduction=make("copy_reduction");
					sumPass=make("sum_pass");maxPass=make("max_pass");subtractMean=make("subtract_mean");
					storeRoot=make("store_root");correctFaces=make("correct_faces");
					copySeam=make("copy_periodic_seam");postResidual=make("cell_post_residual");
					validationMetrics=make("cell_validation_metrics");
				if( !buildFaces||!buildDiagonal||!restrictAverage||!setupVelocity||!classifyOpen||
					!classifyEndpoint||
						!buildRHS||!jacobi||!computeResidual||!prolongate||!clearValues||!copyReduction||
						!sumPass||!maxPass||!subtractMean||!storeRoot||!correctFaces||!copySeam||
						!postResidual||!validationMetrics ) {
						error=MetalError("production fire projection pipeline creation failed",metalError);return;}
					queue=[device newCommandQueue];
					if( !queue ) error="production fire projection command queue allocation failed";
				}
			}

			bool Valid() const {return device&&queue&&error.empty();}
		};

		MetalProjectionContext& Context()
		{
			static MetalProjectionContext context;
			return context;
		}

		bool InjectedFailure( const char* stage )
		{
			const char* value=std::getenv("RISE_FIRE_PROJECTION_TEST_FAILURE");
			return value&&std::strcmp(value,stage)==0;
		}

		enum ProjectionExecutionKind
		{
			ProjectionStandaloneTerminal,
			ProjectionResidentTerminal,
			ProjectionResidentStateOnly,
			ProjectionResidentRestorationTerminal,
			ProjectionResidentRestorationStateOnly
		};

		bool ProjectionCycleCount( const FireProductionProjectionRequest& request,
			ProjectionExecutionKind execution,bool hasOpenBoundary,unsigned int& cycleCount,
			std::string* error )
		{
			cycleCount=hasOpenBoundary?
				((execution==ProjectionResidentStateOnly||
					execution==ProjectionResidentTerminal)?
					request.residentPhysicalOpenVCycleCount:16u):12u;
			const char* activation=std::getenv("RISE_FIRE_RESTORATION_PLATEAU_PROBE");
			const char* restorationTest=std::getenv("RISE_FIRE_PRODUCTION_RESTORATION_TEST");
			if( execution==ProjectionResidentTerminal&&hasOpenBoundary&&activation&&
				std::strcmp(activation,"1")==0&&restorationTest&&
				std::strcmp(restorationTest,"removed")==0 ) cycleCount=17u;
			unsigned int probeCycles=0u;bool probeEnabled=false;
			if( !ValidateFireProductionRestorationCycleProbe(
				probeCycles,probeEnabled,error) ) return false;
			if( probeEnabled&&(execution==ProjectionResidentRestorationTerminal||
				execution==ProjectionResidentRestorationStateOnly) )
				cycleCount=probeCycles;
			return true;
		}

		std::size_t NextPowerOfTwo( std::size_t value )
		{
			std::size_t result=1u;while( result<value ) result<<=1u;return result;
		}

		void Dispatch( id<MTLComputeCommandEncoder> encoder,id<MTLComputePipelineState> pipeline,
			std::size_t count )
		{
			const std::size_t width=std::min<std::size_t>(256u,
				static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]));
			[encoder setComputePipelineState:pipeline];
			[encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(width,1,1)];
		}

		id<MTLBuffer> NewBuffer( id<MTLDevice> device, std::size_t bytes )
		{
			id<MTLBuffer> buffer=[device newBufferWithLength:bytes
				options:MTLResourceStorageModePrivate];
			return buffer&&[buffer storageMode]==MTLStorageModePrivate?
				ObserveProjectionAllocation(buffer):nil;
		}

		id<MTLBuffer> NewBufferWithBytes( id<MTLDevice> device,const void* bytes,std::size_t size )
		{
			id<MTLBuffer> buffer=[device newBufferWithBytes:bytes length:size
				options:MTLResourceStorageModeShared];
			return buffer&&[buffer storageMode]==MTLStorageModeShared?
				ObserveProjectionAllocation(buffer):nil;
		}

		id<MTLBuffer> NewSharedBuffer( id<MTLDevice> device, std::size_t bytes )
		{
			id<MTLBuffer> buffer=[device newBufferWithLength:bytes
				options:MTLResourceStorageModeShared];
			return buffer&&[buffer storageMode]==MTLStorageModeShared?
				ObserveProjectionAllocation(buffer):nil;
		}

		bool AllFinite( const std::vector<float>& values )
		{
			return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
		}

		MetalLevelParameters Parameters( const MetalLevel& level,
			const FireProductionProjectionRequest& request,bool restoration )
		{
			MetalLevelParameters p={};p.nx=static_cast<std::uint32_t>(level.nx);
			p.ny=static_cast<std::uint32_t>(level.ny);p.nz=static_cast<std::uint32_t>(level.nz);
			p.nullspace=std::find(request.boundary.begin(),request.boundary.end(),
				FireProductionProjectionPressureOpen)==request.boundary.end()?1u:0u;
			p.sx=level.spacing[0];p.sy=level.spacing[1];p.sz=level.spacing[2];
			p.ambientDensity=request.ambientDensityKGPerM3;p.timeStepS=request.timeStepS;
			p.restoration=restoration?1u:0u;
			std::uint32_t offset=0u;
			for( unsigned int side=0;side<6u;++side ) {
				p.boundary[side]=static_cast<std::uint32_t>(request.boundary[side]);p.sideOffset[side]=offset;
				const std::size_t count=side<2u?level.ny*level.nz:
					(side<4u?level.nx*level.nz:level.nx*level.ny);
				offset+=static_cast<std::uint32_t>(count);
			}
			return p;
		}

		bool Begin( id<MTLCommandBuffer> command,id<MTLComputeCommandEncoder> __strong& encoder,
			std::string* error,const char* label )
		{
			encoder=[command computeCommandEncoder];
			if( encoder ) return true;
			if( error ) *error=std::string("production fire projection ")+label+" encoder allocation failed";
			return false;
		}

		bool EncodeReduction( MetalProjectionContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> values,std::size_t count,id<MTLBuffer> scratch,id<MTLBuffer> diagnostics,
			std::uint32_t diagnosticIndex,bool maximum,std::string* error )
		{
			const std::size_t width=NextPowerOfTwo(count);MetalReductionParameters parameters={
				static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(width),1u,diagnosticIndex};
			id<MTLComputeCommandEncoder> encoder=nil;
			if( !Begin(command,encoder,error,"reduction-copy") ) return false;
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:scratch offset:0 atIndex:1];
			[encoder setBytes:&parameters length:sizeof(parameters) atIndex:2];
			Dispatch(encoder,context.copyReduction,width);[encoder endEncoding];
			for( std::size_t offset=1u;offset<width;offset<<=1u ) {
				parameters.offset=static_cast<std::uint32_t>(offset);
				if( !Begin(command,encoder,error,"reduction") ) return false;
				[encoder setBuffer:scratch offset:0 atIndex:0];
				[encoder setBytes:&parameters length:sizeof(parameters) atIndex:1];
				Dispatch(encoder,maximum?context.maxPass:context.sumPass,width/(2u*offset));
				[encoder endEncoding];
			}
			if( diagnosticIndex!=std::numeric_limits<std::uint32_t>::max() ) {
				if( !Begin(command,encoder,error,"reduction-root") ) return false;
				[encoder setBuffer:scratch offset:0 atIndex:0];[encoder setBuffer:diagnostics offset:0 atIndex:1];
				[encoder setBytes:&parameters length:sizeof(parameters) atIndex:2];
				Dispatch(encoder,context.storeRoot,1u);[encoder endEncoding];
			}
			return true;
		}

		bool EncodeRemoveMean( MetalProjectionContext& context,id<MTLCommandBuffer> command,
			id<MTLBuffer> values,std::size_t count,id<MTLBuffer> scratch,id<MTLBuffer> diagnostics,
			std::uint32_t diagnosticIndex,std::string* error )
		{
			if( !EncodeReduction(context,command,values,count,scratch,diagnostics,
				std::numeric_limits<std::uint32_t>::max(),false,error) ) return false;
			const std::size_t width=NextPowerOfTwo(count);MetalReductionParameters p={
				static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(width),1u,diagnosticIndex};
			id<MTLComputeCommandEncoder> encoder=nil;
			if( !Begin(command,encoder,error,"mean-removal") ) return false;
			[encoder setBuffer:values offset:0 atIndex:0];[encoder setBuffer:scratch offset:0 atIndex:1];
			[encoder setBytes:&p length:sizeof(p) atIndex:2];[encoder setBuffer:diagnostics offset:0 atIndex:3];
			Dispatch(encoder,context.subtractMean,count);[encoder endEncoding];return true;
		}

		bool EncodeSmooth( MetalProjectionContext& context,id<MTLCommandBuffer> command,
			MetalLevel& level,unsigned int sweeps,id<MTLBuffer> scratch,id<MTLBuffer> diagnostics,
			std::uint64_t& executedSweeps,std::string* error )
		{
			const std::size_t count=level.nx*level.ny*level.nz;
			for( unsigned int sweep=0;sweep<sweeps;++sweep ) {
				id<MTLComputeCommandEncoder> encoder=nil;if( !Begin(command,encoder,error,"Jacobi") ) return false;
				[encoder setBuffer:level.pressure offset:0 atIndex:0];[encoder setBuffer:level.rhs offset:0 atIndex:1];
				[encoder setBuffer:level.diagonal offset:0 atIndex:2];
				for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:level.beta[axis] offset:0 atIndex:3u+axis];
				[encoder setBuffer:level.temporary offset:0 atIndex:6];[encoder setBuffer:level.parameters offset:0 atIndex:7];
				Dispatch(encoder,context.jacobi,count);[encoder endEncoding];
				std::swap(level.pressure,level.temporary);
				const MetalLevelParameters* p=static_cast<const MetalLevelParameters*>(
					ProjectionBufferContents(level.parameters,ProjectionMetadataAccess));
				if( p->nullspace!=0u&&!EncodeRemoveMean(context,command,level.pressure,count,scratch,
					diagnostics,std::numeric_limits<std::uint32_t>::max(),error) ) return false;
				++executedSweeps;
			}
			return true;
		}

		bool EncodeVCycle( MetalProjectionContext& context,id<MTLCommandBuffer> command,
			std::vector<MetalLevel>& hierarchy,std::size_t index,id<MTLBuffer> scratch,
			id<MTLBuffer> diagnostics,std::uint64_t& executedSweeps,std::string* error )
		{
			MetalLevel& level=hierarchy[index];const std::size_t count=level.nx*level.ny*level.nz;
			if( index+1u==hierarchy.size() ) return EncodeSmooth(context,command,level,32u,
				scratch,diagnostics,executedSweeps,error);
			if( !EncodeSmooth(context,command,level,3u,scratch,diagnostics,executedSweeps,error) ) return false;
			id<MTLComputeCommandEncoder> encoder=nil;if( !Begin(command,encoder,error,"residual") ) return false;
			[encoder setBuffer:level.pressure offset:0 atIndex:0];[encoder setBuffer:level.rhs offset:0 atIndex:1];
			for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:level.beta[axis] offset:0 atIndex:2u+axis];
			[encoder setBuffer:level.residual offset:0 atIndex:5];[encoder setBuffer:level.parameters offset:0 atIndex:6];
			Dispatch(encoder,context.computeResidual,count);[encoder endEncoding];
			MetalLevel& coarse=hierarchy[index+1u];const std::size_t coarseCount=coarse.nx*coarse.ny*coarse.nz;
			if( !Begin(command,encoder,error,"residual restriction") ) return false;
			[encoder setBuffer:level.residual offset:0 atIndex:0];[encoder setBuffer:coarse.rhs offset:0 atIndex:1];
			[encoder setBuffer:level.parameters offset:0 atIndex:2];[encoder setBuffer:coarse.parameters offset:0 atIndex:3];
			Dispatch(encoder,context.restrictAverage,coarseCount);[encoder endEncoding];
			const MetalLevelParameters* cp=static_cast<const MetalLevelParameters*>(
				ProjectionBufferContents(coarse.parameters,ProjectionMetadataAccess));
			if( cp->nullspace!=0u&&!EncodeRemoveMean(context,command,coarse.rhs,coarseCount,scratch,
				diagnostics,std::numeric_limits<std::uint32_t>::max(),error) ) return false;
			MetalReductionParameters clear={static_cast<std::uint32_t>(coarseCount),0u,0u,0u};
			if( !Begin(command,encoder,error,"coarse clear") ) return false;
			[encoder setBuffer:coarse.pressure offset:0 atIndex:0];[encoder setBytes:&clear length:sizeof(clear) atIndex:1];
			Dispatch(encoder,context.clearValues,coarseCount);[encoder endEncoding];
			if( !EncodeVCycle(context,command,hierarchy,index+1u,scratch,diagnostics,
				executedSweeps,error) ) return false;
			if( !Begin(command,encoder,error,"prolongation") ) return false;
			[encoder setBuffer:coarse.pressure offset:0 atIndex:0];[encoder setBuffer:level.pressure offset:0 atIndex:1];
			[encoder setBuffer:coarse.parameters offset:0 atIndex:2];[encoder setBuffer:level.parameters offset:0 atIndex:3];
			Dispatch(encoder,context.prolongate,count);[encoder endEncoding];
			return EncodeSmooth(context,command,level,3u,scratch,diagnostics,executedSweeps,error);
		}
	}

	bool ProjectFireProductionMetalImpl( const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput* residentInput,
		ProjectionExecutionKind execution,
		FireProductionMetalProjectionResidentState* residentState,
		FireProductionProjectionResult& result, std::string* error )
	{
		result=FireProductionProjectionResult();
		if( residentState ) *residentState=FireProductionMetalProjectionResidentState();
		try {
			std::uint32_t observedUploadStaging=0u,observedTerminalStaging=0u;
			const std::uint64_t beginningCommandCommits=projectionCommandCommitCount;
			const std::uint64_t beginningInterstageReads=projectionInterstageFullGridReadCount;
			const std::uint64_t beginningProjectionInvocations=projectionInvocationCount;
			const std::uint64_t beginningAllocationCount=projectionBufferAllocationCount;
			const std::uint64_t beginningAllocationBytes=projectionBufferAllocationBytes;
			std::uint64_t observedActualMetalBytes=0u,certifiedWorkingSetBytes=0u;
			if( !ValidateFireProductionProjectionRequest(request,error) ) return false;
			const bool hasOpenBoundary=std::any_of(request.boundary.begin(),
				request.boundary.end(),[](const FireProductionProjectionBoundary boundary){
					return boundary==FireProductionProjectionPressureOpen;});
			unsigned int cycleCount=0u;
			if( !ProjectionCycleCount(request,execution,hasOpenBoundary,cycleCount,error) ) return false;
			if( !FireProductionProjectionWorkingSetBytes(request.shape,certifiedWorkingSetBytes) )
				return false;
			if( residentInput ) for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::uint64_t bytes=FireProductionProjectionFaceCount(request.shape,axis)*sizeof(float);
				const std::uint64_t allocation=(bytes+UINT64_C(16383))&~UINT64_C(16383);
				if( certifiedWorkingSetBytes>std::numeric_limits<std::uint64_t>::max()-allocation )
					return false;
				certifiedWorkingSetBytes+=allocation;
			}
			if( residentInput ) {
				const std::size_t cellBytes=request.shape.CellCount()*sizeof(float);
				id<MTLBuffer> packed=residentInput->provisionalMomentumKGPerM2S[0];
				if( !residentInput->gasDensityKGPerM3||!residentInput->divergenceTargetPerS||!packed||
					[residentInput->gasDensityKGPerM3 storageMode]!=MTLStorageModePrivate||
					[residentInput->divergenceTargetPerS storageMode]!=MTLStorageModePrivate||
					[packed storageMode]!=MTLStorageModePrivate||
					[residentInput->gasDensityKGPerM3 length]<cellBytes||
					[residentInput->divergenceTargetPerS length]<cellBytes ) {
					if( error ) *error="production fire projection resident input is not private full-grid storage";
					return false;
				}
				std::size_t expectedOffset=0u;
				const bool packedMomentum=
					residentInput->provisionalMomentumKGPerM2S[1]==packed&&
					residentInput->provisionalMomentumKGPerM2S[2]==packed;
				const bool separateMomentum=
					residentInput->provisionalMomentumKGPerM2S[1]!=packed&&
					residentInput->provisionalMomentumKGPerM2S[2]!=packed&&
					residentInput->provisionalMomentumKGPerM2S[1]!=
						residentInput->provisionalMomentumKGPerM2S[2];
				if( !packedMomentum&&!separateMomentum ) {
					if( error ) *error="production fire projection resident momentum ownership is invalid";
					return false;
				}
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t bytes=FireProductionProjectionFaceCount(request.shape,axis)*sizeof(float);
					id<MTLBuffer> axisBuffer=residentInput->provisionalMomentumKGPerM2S[axis];
					const std::size_t requiredOffset=packedMomentum?expectedOffset:0u;
					if( !axisBuffer||[axisBuffer storageMode]!=MTLStorageModePrivate||
						residentInput->provisionalMomentumByteOffset[axis]!=requiredOffset||
						requiredOffset>[axisBuffer length]||bytes>[axisBuffer length]-requiredOffset ) {
						if( error ) *error="production fire projection resident momentum packing is invalid";
						return false;
					}
					expectedOffset+=bytes;
				}
				bool aliasesCellBuffer=false;
				for( unsigned int axis=0u;axis<3u;++axis ) aliasesCellBuffer=
					aliasesCellBuffer||residentInput->provisionalMomentumKGPerM2S[axis]==
						residentInput->gasDensityKGPerM3||
					residentInput->provisionalMomentumKGPerM2S[axis]==
						residentInput->divergenceTargetPerS;
				if( (packedMomentum&&[packed length]<expectedOffset)||aliasesCellBuffer||
					residentInput->gasDensityKGPerM3==residentInput->divergenceTargetPerS ) {
					if( error ) *error="production fire projection resident buffer ownership is invalid";
					return false;
				}
			}
			MetalProjectionContext& context=Context();
			if( !context.Valid() ) {if( error ) *error=context.error;return false;}
			@autoreleasepool {
				const bool resident=residentInput!=0;
				const bool stateOnly=execution==ProjectionResidentStateOnly||
					execution==ProjectionResidentRestorationStateOnly;
				const bool terminal=!stateOnly;
				const bool restoration=execution==ProjectionResidentRestorationTerminal||
					execution==ProjectionResidentRestorationStateOnly;
				if( resident!=(execution!=ProjectionStandaloneTerminal)||
					(stateOnly&&!residentState)||(!stateOnly&&residentState) ) return false;
				const FireProductionProjectionShape& shape=request.shape;
				const std::size_t cells=shape.CellCount(),finePadded=NextPowerOfTwo(cells);
				std::vector<MetalLevel> hierarchy;MetalLevel fine={};fine.nx=shape.nx;fine.ny=shape.ny;fine.nz=shape.nz;
				fine.spacing[0]=fine.spacing[1]=fine.spacing[2]=shape.cellWidthM;
				fine.density=resident?residentInput->gasDensityKGPerM3:
					NewBuffer(context.device,cells*sizeof(float));
				id<MTLBuffer> densityUpload=resident?nil:NewBufferWithBytes(context.device,
					request.gasDensityKGPerM3.data(),cells*sizeof(float));
				hierarchy.push_back(fine);
				while( hierarchy.back().nx>4u||hierarchy.back().ny>4u||hierarchy.back().nz>4u ) {
					const MetalLevel& parent=hierarchy.back();MetalLevel coarse={};
					coarse.nx=parent.nx>4u?(parent.nx+1u)/2u:parent.nx;
					coarse.ny=parent.ny>4u?(parent.ny+1u)/2u:parent.ny;
					coarse.nz=parent.nz>4u?(parent.nz+1u)/2u:parent.nz;
					coarse.spacing[0]=parent.spacing[0]*static_cast<float>(parent.nx)/static_cast<float>(coarse.nx);
					coarse.spacing[1]=parent.spacing[1]*static_cast<float>(parent.ny)/static_cast<float>(coarse.ny);
					coarse.spacing[2]=parent.spacing[2]*static_cast<float>(parent.nz)/static_cast<float>(coarse.nz);
					hierarchy.push_back(coarse);
				}
				for( MetalLevel& level : hierarchy ) {
					const std::size_t count=level.nx*level.ny*level.nz,bytes=count*sizeof(float);
					if( !level.density ) level.density=NewBuffer(context.device,bytes);
					level.rhs=NewBuffer(context.device,bytes);level.pressure=NewBuffer(context.device,bytes);
					level.temporary=NewBuffer(context.device,bytes);level.residual=NewBuffer(context.device,bytes);
					level.diagonal=NewBuffer(context.device,bytes);
					FireProductionProjectionShape local;local.nx=level.nx;local.ny=level.ny;local.nz=level.nz;
					for( unsigned int axis=0;axis<3u;++axis ) level.beta[axis]=NewBuffer(context.device,
						FireProductionProjectionFaceCount(local,axis)*sizeof(float));
					const MetalLevelParameters parameters=Parameters(level,request,restoration);
					level.parameters=NewBufferWithBytes(context.device,&parameters,sizeof(parameters));
				}
				std::array<id<MTLBuffer>,3> provisional,provisionalUpload,stored,momentum,velocity;
				std::array<std::size_t,3> provisionalOffset={};
				for( unsigned int axis=0;axis<3u;++axis ) {
					const std::size_t count=FireProductionProjectionFaceCount(shape,axis),bytes=count*sizeof(float);
					provisional[axis]=resident?residentInput->provisionalMomentumKGPerM2S[axis]:
						NewBuffer(context.device,bytes);
					provisionalOffset[axis]=resident?residentInput->provisionalMomentumByteOffset[axis]:0u;
					provisionalUpload[axis]=resident?nil:NewBufferWithBytes(context.device,
						request.provisionalMomentumKGPerM2S[axis].data(),bytes);
					stored[axis]=InjectedFailure("local_shared")&&axis==1u?
						NewSharedBuffer(context.device,bytes):NewBuffer(context.device,bytes);
					momentum[axis]=NewBuffer(context.device,bytes);
					velocity[axis]=NewBuffer(context.device,bytes);
				}
				id<MTLBuffer> target=resident?residentInput->divergenceTargetPerS:
					NewBuffer(context.device,cells*sizeof(float));
				id<MTLBuffer> targetUpload=resident?nil:NewBufferWithBytes(context.device,
					request.divergenceTargetPerS.data(),cells*sizeof(float));
				const std::size_t boundaryCount=2u*(shape.ny*shape.nz+shape.nx*shape.nz+shape.nx*shape.ny);
				id<MTLBuffer> boundaryPressure=NewSharedBuffer(context.device,boundaryCount*sizeof(float));
				id<MTLBuffer> inflow=NewSharedBuffer(context.device,boundaryCount*sizeof(unsigned char));
				id<MTLBuffer> scratch=NewBuffer(context.device,finePadded*sizeof(float));
				id<MTLBuffer> diagnostics=NewSharedBuffer(context.device,12u*sizeof(float));
				bool allocated=fine.density&&target&&boundaryPressure&&inflow&&scratch&&diagnostics&&
					(resident||(densityUpload&&targetUpload));
				for( const MetalLevel& level:hierarchy ) allocated=allocated&&level.density&&level.rhs&&
					level.pressure&&level.temporary&&level.residual&&level.diagonal&&level.parameters&&
					level.beta[0]&&level.beta[1]&&level.beta[2];
				for( unsigned int axis=0;axis<3u;++axis ) allocated=allocated&&provisional[axis]&&
					(resident||provisionalUpload[axis])&&stored[axis]&&momentum[axis]&&velocity[axis];
				if( InjectedFailure("buffer") ) allocated=false;
				if( !allocated ) {if( error ) *error="production fire projection buffer allocation failed";return false;}
				bool storageModes=[target storageMode]==MTLStorageModePrivate&&
					[boundaryPressure storageMode]==MTLStorageModeShared&&
					[inflow storageMode]==MTLStorageModeShared&&
					[scratch storageMode]==MTLStorageModePrivate&&
					[diagnostics storageMode]==MTLStorageModeShared;
				for( const MetalLevel& level:hierarchy ) storageModes=storageModes&&
					[level.density storageMode]==MTLStorageModePrivate&&
					[level.rhs storageMode]==MTLStorageModePrivate&&
					[level.pressure storageMode]==MTLStorageModePrivate&&
					[level.temporary storageMode]==MTLStorageModePrivate&&
					[level.residual storageMode]==MTLStorageModePrivate&&
					[level.diagonal storageMode]==MTLStorageModePrivate&&
					[level.beta[0] storageMode]==MTLStorageModePrivate&&
					[level.beta[1] storageMode]==MTLStorageModePrivate&&
					[level.beta[2] storageMode]==MTLStorageModePrivate&&
					[level.parameters storageMode]==MTLStorageModeShared;
				for( unsigned int axis=0u;axis<3u;++axis ) storageModes=storageModes&&
					[provisional[axis] storageMode]==MTLStorageModePrivate&&
					[stored[axis] storageMode]==MTLStorageModePrivate&&
					[momentum[axis] storageMode]==MTLStorageModePrivate&&
					[velocity[axis] storageMode]==MTLStorageModePrivate&&
					(resident||[provisionalUpload[axis] storageMode]==MTLStorageModeShared);
				if( !resident ) storageModes=storageModes&&
					[densityUpload storageMode]==MTLStorageModeShared&&
					[targetUpload storageMode]==MTLStorageModeShared;
				if( !storageModes ) {
					if( error )
						*error="production fire projection resident storage topology changed";
					return false;
				}
				auto addAllocation=[&](id<MTLBuffer> buffer,std::uint64_t& total)->bool {
					const std::uint64_t value=[buffer allocatedSize];
					if( total>std::numeric_limits<std::uint64_t>::max()-value ) return false;
					total+=value;return true;};
				std::uint64_t residentBytes=0u,uploadBytes=0u;
				for( const MetalLevel& level:hierarchy ) {
					const id<MTLBuffer> levelBuffers[]={level.density,level.rhs,level.pressure,
						level.temporary,level.residual,level.diagonal,level.beta[0],level.beta[1],
						level.beta[2],level.parameters};
					for( id<MTLBuffer> buffer:levelBuffers ) if( !addAllocation(buffer,residentBytes) )
						return false;
				}
				const id<MTLBuffer> fixedResident[]={target,boundaryPressure,inflow,scratch,diagnostics};
				for( id<MTLBuffer> buffer:fixedResident ) if( !addAllocation(buffer,residentBytes) ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const id<MTLBuffer> faceResident[]={stored[axis],momentum[axis],velocity[axis]};
					for( id<MTLBuffer> buffer:faceResident ) if( !addAllocation(buffer,residentBytes) ) return false;
				}
				if( resident ) {
					if( !addAllocation(provisional[0],residentBytes) ) return false;
					for( unsigned int axis=1u;axis<3u;++axis ) if(
						provisional[axis]!=provisional[0]&&
						!addAllocation(provisional[axis],residentBytes) ) return false;
				} else for( id<MTLBuffer> buffer:provisional )
					if( !addAllocation(buffer,residentBytes) ) return false;
				if( !resident ) {
					if( !addAllocation(densityUpload,uploadBytes)||!addAllocation(targetUpload,uploadBytes) )
						return false;
					for( id<MTLBuffer> buffer:provisionalUpload )
						if( !addAllocation(buffer,uploadBytes) ) return false;
				}
				std::uint64_t faceValues=0u;for( unsigned int axis=0u;axis<3u;++axis )
					faceValues+=FireProductionProjectionFaceCount(shape,axis);
				const std::uint64_t hostBytes=terminal?
					(3u*static_cast<std::uint64_t>(cells)+4u*faceValues)*sizeof(float)+
						6u*static_cast<std::uint64_t>(boundaryCount):0u;
				if( hostBytes>std::numeric_limits<std::uint64_t>::max()-residentBytes||
					hostBytes+residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes )
					return false;
				observedActualMetalBytes=hostBytes+residentBytes+uploadBytes;
				if( observedActualMetalBytes>certifiedWorkingSetBytes||
					observedActualMetalBytes>(UINT64_C(1)<<31u) ) {
					if( error ) *error="production fire projection actual allocation exceeds certificate";
					return false;
				}
				float* representedBoundaryPressure=static_cast<float*>(
					ProjectionBufferContents(boundaryPressure,ProjectionMetadataAccess));
				if(request.openHeadMode==FireProductionProjectionUseSealedOpenHead){
					std::size_t offset=0u;
					for(unsigned int side=0u;side<6u;++side){
						const std::vector<float>& sealed=
							request.sealedPressureOpenDynamicPressurePa[side];
						std::copy(sealed.begin(),sealed.end(),representedBoundaryPressure+offset);
						offset+=sealed.size();
					}
				}else std::fill_n(representedBoundaryPressure,boundaryCount,0.0f);
				unsigned char* representedInflow=static_cast<unsigned char*>(
					ProjectionBufferContents(inflow,ProjectionMetadataAccess));
				if(request.openClassificationMode==
					FireProductionProjectionUseSealedOpenClassification){
					std::size_t offset=0u;
					for(unsigned int side=0u;side<6u;++side){
						const std::vector<unsigned char>& sealed=
							request.sealedPressureOpenInflow[side];
						std::copy(sealed.begin(),sealed.end(),representedInflow+offset);
						offset+=sealed.size();
					}
				}else std::fill_n(representedInflow,boundaryCount,
					static_cast<unsigned char>(0u));
				std::fill_n(static_cast<float*>(ProjectionBufferContents(diagnostics,
					ProjectionMetadataAccess)),12u,0.0f);

				if( !resident ) {
					id<MTLCommandBuffer> uploadCommand=InjectedFailure("upload_command")?
						nil:[context.queue commandBuffer];
					if( !uploadCommand ) {if( error ) *error="production fire projection upload command allocation failed";return false;}
					id<MTLBlitCommandEncoder> upload=InjectedFailure("upload_encoder")?
						nil:[uploadCommand blitCommandEncoder];
					if( !upload ) {if( error ) *error="production fire projection upload encoder allocation failed";return false;}
					{
						ProjectionTransferScope transferScope(ProjectionUploadTransfer);
						CopyProjectionBuffer(upload,densityUpload,0u,fine.density,0u,cells*sizeof(float));
						CopyProjectionBuffer(upload,targetUpload,0u,target,0u,cells*sizeof(float));
						for( unsigned int axis=0;axis<3u;++axis ) CopyProjectionBuffer(upload,
							provisionalUpload[axis],0u,provisional[axis],0u,
							FireProductionProjectionFaceCount(shape,axis)*sizeof(float));
					}
					[upload endEncoding];CommitProjectionCommand(uploadCommand);
					++observedUploadStaging;[uploadCommand waitUntilCompleted];
					if( [uploadCommand status]!=MTLCommandBufferStatusCompleted ) {
						if( error ) *error=MetalError("production fire projection upload command failed",[uploadCommand error]);return false;}
					densityUpload=nil;targetUpload=nil;
					for( unsigned int axis=0;axis<3u;++axis ) provisionalUpload[axis]=nil;
				}
				id<MTLCommandBuffer> command=[context.queue commandBuffer];
				if( InjectedFailure("command_buffer") ) command=nil;
				if( !command ) {if( error ) *error="production fire projection command allocation failed";return false;}
				id<MTLComputeCommandEncoder> encoder=nil;ObserveProjectionInvocation();
				for( std::size_t index=0;index<hierarchy.size();++index ) {
					MetalLevel& level=hierarchy[index];const std::size_t count=level.nx*level.ny*level.nz;
					if( index>0u ) {
						if( !Begin(command,encoder,error,"density restriction") ) return false;
						[encoder setBuffer:hierarchy[index-1u].density offset:0 atIndex:0];[encoder setBuffer:level.density offset:0 atIndex:1];
						[encoder setBuffer:hierarchy[index-1u].parameters offset:0 atIndex:2];[encoder setBuffer:level.parameters offset:0 atIndex:3];
						Dispatch(encoder,context.restrictAverage,count);[encoder endEncoding];
					}
					FireProductionProjectionShape local;local.nx=level.nx;local.ny=level.ny;local.nz=level.nz;
					for( unsigned int axis=0;axis<3u;++axis ) {
						const MetalAxisParameters axisParameters={axis,index==0u?1u:0u,0u,0u};
						if( !Begin(command,encoder,error,"face coefficient") ) return false;
						[encoder setBuffer:level.density offset:0 atIndex:0];[encoder setBuffer:level.beta[axis] offset:0 atIndex:1];
						[encoder setBuffer:stored[axis] offset:0 atIndex:2];[encoder setBuffer:level.parameters offset:0 atIndex:3];
						[encoder setBytes:&axisParameters length:sizeof(axisParameters) atIndex:4];
						Dispatch(encoder,context.buildFaces,FireProductionProjectionFaceCount(local,axis));[encoder endEncoding];
					}
					if( !Begin(command,encoder,error,"diagonal") ) return false;
					for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:level.beta[axis] offset:0 atIndex:axis];
					[encoder setBuffer:level.diagonal offset:0 atIndex:3];[encoder setBuffer:level.parameters offset:0 atIndex:4];
					Dispatch(encoder,context.buildDiagonal,count);[encoder endEncoding];
				}
				MetalLevel& fineLevel=hierarchy.front();
				for( unsigned int axis=0;axis<3u;++axis ) {
					const MetalAxisParameters axisParameters={axis,0u,0u,0u};
					if( !Begin(command,encoder,error,"provisional velocity") ) return false;
					[encoder setBuffer:provisional[axis] offset:provisionalOffset[axis] atIndex:0];[encoder setBuffer:stored[axis] offset:0 atIndex:1];
					[encoder setBuffer:momentum[axis] offset:0 atIndex:2];[encoder setBuffer:velocity[axis] offset:0 atIndex:3];
					[encoder setBuffer:fineLevel.parameters offset:0 atIndex:4];[encoder setBytes:&axisParameters length:sizeof(axisParameters) atIndex:5];
					Dispatch(encoder,context.setupVelocity,FireProductionProjectionFaceCount(shape,axis));[encoder endEncoding];
				}
				for( unsigned int side=0;side<6u;++side ) if(
					request.boundary[side]==FireProductionProjectionPressureOpen&&(
					request.openHeadMode==FireProductionProjectionDeriveCurrentOpenHead||
					request.openClassificationMode==
						FireProductionProjectionDeriveOpenClassification) ) {
					const MetalAxisParameters sideParameters={0u,0u,side,
						(request.openClassificationMode==
							FireProductionProjectionUseSealedOpenClassification?1u:0u)|
						(request.openHeadMode==FireProductionProjectionUseSealedOpenHead?2u:0u)};
					if( !Begin(command,encoder,error,"open-boundary classification") ) return false;
					for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:velocity[axis] offset:0 atIndex:axis];
					[encoder setBuffer:inflow offset:0 atIndex:3];[encoder setBuffer:boundaryPressure offset:0 atIndex:4];
					[encoder setBuffer:fineLevel.parameters offset:0 atIndex:5];[encoder setBytes:&sideParameters length:sizeof(sideParameters) atIndex:6];
					const std::size_t count=side<2u?shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
					Dispatch(encoder,context.classifyOpen,count);[encoder endEncoding];
				}
				if( !Begin(command,encoder,error,"right-hand side") ) return false;
				for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:velocity[axis] offset:0 atIndex:axis];
				[encoder setBuffer:target offset:0 atIndex:3];for( unsigned int axis=0;axis<3u;++axis )
					[encoder setBuffer:fineLevel.beta[axis] offset:0 atIndex:4u+axis];
				[encoder setBuffer:boundaryPressure offset:0 atIndex:7];[encoder setBuffer:fineLevel.rhs offset:0 atIndex:8];
				[encoder setBuffer:fineLevel.residual offset:0 atIndex:9];[encoder setBuffer:fineLevel.parameters offset:0 atIndex:10];
				Dispatch(encoder,context.buildRHS,cells);[encoder endEncoding];
				if( !EncodeReduction(context,command,fineLevel.residual,cells,scratch,diagnostics,0u,true,error) ) return false;
				const MetalLevelParameters* fp=static_cast<const MetalLevelParameters*>(
					ProjectionBufferContents(fineLevel.parameters,ProjectionMetadataAccess));
				if( fp->nullspace!=0u&&!EncodeRemoveMean(context,command,fineLevel.rhs,cells,scratch,diagnostics,11u,error) ) return false;
				for( MetalLevel& level:hierarchy ) {
					const std::size_t count=level.nx*level.ny*level.nz;
					MetalReductionParameters clear={static_cast<std::uint32_t>(count),0u,0u,0u};
					if( !Begin(command,encoder,error,"pressure clear") ) return false;
					[encoder setBuffer:level.pressure offset:0 atIndex:0];[encoder setBytes:&clear length:sizeof(clear) atIndex:1];
					Dispatch(encoder,context.clearValues,count);[encoder endEncoding];
				}
				std::uint32_t executedCycles=0u;std::uint64_t executedSweeps=0u;
				for( unsigned int cycle=0;cycle<cycleCount;++cycle ) {
					if( !EncodeVCycle(context,command,hierarchy,0u,scratch,diagnostics,
						executedSweeps,error) ) return false;
					if( fp->nullspace!=0u&&!EncodeRemoveMean(context,command,fineLevel.pressure,cells,scratch,
						diagnostics,std::numeric_limits<std::uint32_t>::max(),error) ) return false;
					++executedCycles;
				}
				for( unsigned int axis=0;axis<3u;++axis ) {
					const MetalAxisParameters axisParameters={axis,0u,0u,0u};
					if( !Begin(command,encoder,error,"face correction") ) return false;
					[encoder setBuffer:provisional[axis] offset:provisionalOffset[axis] atIndex:0];[encoder setBuffer:stored[axis] offset:0 atIndex:1];
					[encoder setBuffer:fineLevel.pressure offset:0 atIndex:2];[encoder setBuffer:boundaryPressure offset:0 atIndex:3];
					[encoder setBuffer:momentum[axis] offset:0 atIndex:4];[encoder setBuffer:velocity[axis] offset:0 atIndex:5];
					[encoder setBuffer:fineLevel.parameters offset:0 atIndex:6];[encoder setBytes:&axisParameters length:sizeof(axisParameters) atIndex:7];
					Dispatch(encoder,context.correctFaces,FireProductionProjectionFaceCount(shape,axis));[encoder endEncoding];
					if( request.boundary[2u*axis]==FireProductionProjectionPeriodic ) {
						if( !Begin(command,encoder,error,"periodic publication") ) return false;
						[encoder setBuffer:stored[axis] offset:0 atIndex:0];[encoder setBuffer:momentum[axis] offset:0 atIndex:1];
						[encoder setBuffer:velocity[axis] offset:0 atIndex:2];[encoder setBuffer:fineLevel.parameters offset:0 atIndex:3];
						[encoder setBytes:&axisParameters length:sizeof(axisParameters) atIndex:4];
						const std::size_t seam=axis==0u?shape.ny*shape.nz:(axis==1u?shape.nx*shape.nz:shape.nx*shape.ny);
						Dispatch(encoder,context.copySeam,seam);[encoder endEncoding];
					}
				}
				if(request.outputClassificationMode==
					FireProductionProjectionDeriveEndpointOpenClassification){
					for(unsigned int side=0u;side<6u;++side)if(
						request.boundary[side]==FireProductionProjectionPressureOpen){
						const MetalAxisParameters sideParameters={0u,0u,side,0u};
						if(!Begin(command,encoder,error,"endpoint open-boundary classification"))return false;
						for(unsigned int axis=0u;axis<3u;++axis)
							[encoder setBuffer:velocity[axis] offset:0 atIndex:axis];
						[encoder setBuffer:inflow offset:0 atIndex:3];
						[encoder setBuffer:fineLevel.parameters offset:0 atIndex:4];
						[encoder setBytes:&sideParameters length:sizeof(sideParameters) atIndex:5];
						[encoder setBytes:&request.endpointVelocityToleranceMPerS
							length:sizeof(request.endpointVelocityToleranceMPerS) atIndex:6];
						const std::size_t count=side<2u?shape.ny*shape.nz:
							(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
						Dispatch(encoder,context.classifyEndpoint,count);[encoder endEncoding];
					}
				}
				if( !Begin(command,encoder,error,"post residual") ) return false;
				for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:velocity[axis] offset:0 atIndex:axis];
				[encoder setBuffer:target offset:0 atIndex:3];[encoder setBuffer:fineLevel.residual offset:0 atIndex:4];
				[encoder setBuffer:fineLevel.parameters offset:0 atIndex:5];
				for( unsigned int axis=0u;axis<3u;++axis ) [encoder setBuffer:provisional[axis]
					offset:provisionalOffset[axis] atIndex:6u+axis];
				for( unsigned int axis=0u;axis<3u;++axis ) [encoder setBuffer:stored[axis]
					offset:0 atIndex:9u+axis];
				Dispatch(encoder,context.postResidual,cells);[encoder endEncoding];
				if( !EncodeReduction(context,command,fineLevel.residual,cells,scratch,diagnostics,1u,true,error) ) return false;
				if( !Begin(command,encoder,error,"validation metrics") ) return false;
				for( unsigned int axis=0u;axis<3u;++axis ) [encoder setBuffer:provisional[axis]
					offset:provisionalOffset[axis] atIndex:axis];
				for( unsigned int axis=0u;axis<3u;++axis ) [encoder setBuffer:stored[axis]
					offset:0 atIndex:3u+axis];
				for( unsigned int axis=0u;axis<3u;++axis ) [encoder setBuffer:velocity[axis]
					offset:0 atIndex:6u+axis];
				[encoder setBuffer:inflow offset:0 atIndex:9];
				[encoder setBuffer:fineLevel.residual offset:0 atIndex:10];
				[encoder setBuffer:fineLevel.temporary offset:0 atIndex:11];
				[encoder setBuffer:fineLevel.parameters offset:0 atIndex:12];
				Dispatch(encoder,context.validationMetrics,cells);[encoder endEncoding];
				if( !EncodeReduction(context,command,fineLevel.residual,cells,scratch,diagnostics,
					2u,true,error)||!EncodeReduction(context,command,fineLevel.temporary,cells,scratch,
					diagnostics,3u,true,error) ) return false;
				id<MTLBuffer> injectedInterstageStage=nil;
				if( InjectedFailure("interstage_transfer") ) {
					injectedInterstageStage=NewSharedBuffer(context.device,cells*sizeof(float));
					id<MTLBlitCommandEncoder> injectedBlit=injectedInterstageStage?
						[command blitCommandEncoder]:nil;
					if( !injectedBlit ) {
						if( error ) *error=
							"production fire projection injected staging allocation failed";
						return false;
					}
					CopyProjectionBuffer(injectedBlit,fineLevel.pressure,0u,injectedInterstageStage,
						0u,cells*sizeof(float));
					[injectedBlit endEncoding];
				}
				id<MTLBuffer> injectedInterstageUpload=nil;
				if( InjectedFailure("interstage_upload") ) {
					injectedInterstageUpload=NewSharedBuffer(context.device,cells*sizeof(float));
					id<MTLBlitCommandEncoder> injectedBlit=injectedInterstageUpload?
						[command blitCommandEncoder]:nil;
					if( !injectedBlit ) {
						if( error ) *error=
							"production fire projection injected upload allocation failed";
						return false;
					}
					CopyProjectionBuffer(injectedBlit,injectedInterstageUpload,0u,
						fineLevel.pressure,0u,cells*sizeof(float));
					[injectedBlit endEncoding];
				}
				CommitProjectionCommand(command);[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted||InjectedFailure("command") ) {
					if( error ) *error=MetalError("production fire projection command failed",[command error]);return false;}
				if( InjectedFailure("second_projection") ) ObserveProjectionInvocation();
				std::uint64_t stagingBytes=0u;
				if( terminal ) {
				id<MTLBuffer> pressureStage=InjectedFailure("staging_buffer")?
					nil:NewSharedBuffer(context.device,cells*sizeof(float));
				std::array<id<MTLBuffer>,3> storedStage,momentumStage,velocityStage,provisionalStage;
				bool staged=pressureStage!=nil;
				for( unsigned int axis=0;axis<3u;++axis ) {
					const std::size_t bytes=FireProductionProjectionFaceCount(shape,axis)*sizeof(float);
					storedStage[axis]=NewSharedBuffer(context.device,bytes);
					momentumStage[axis]=NewSharedBuffer(context.device,bytes);
					velocityStage[axis]=NewSharedBuffer(context.device,bytes);
					provisionalStage[axis]=resident?NewSharedBuffer(context.device,bytes):nil;
					staged=staged&&storedStage[axis]&&momentumStage[axis]&&velocityStage[axis];
					if( resident ) staged=staged&&provisionalStage[axis];
				}
				if( !staged ) {if( error ) *error="production fire projection staging allocation failed";return false;}
				if( [pressureStage storageMode]!=MTLStorageModeShared ) {
					if( error ) *error="production fire projection staging storage topology changed";
					return false;
				}
				for( unsigned int axis=0u;axis<3u;++axis ) if(
					[storedStage[axis] storageMode]!=MTLStorageModeShared||
					[momentumStage[axis] storageMode]!=MTLStorageModeShared||
					[velocityStage[axis] storageMode]!=MTLStorageModeShared||
					(resident&&[provisionalStage[axis] storageMode]!=MTLStorageModeShared) ) {
					if( error ) *error="production fire projection staging storage topology changed";
					return false;
				}
				if( !addAllocation(pressureStage,stagingBytes) ) return false;
				for( unsigned int axis=0u;axis<3u;++axis )
					if( !addAllocation(storedStage[axis],stagingBytes)||
						!addAllocation(momentumStage[axis],stagingBytes)||
						!addAllocation(velocityStage[axis],stagingBytes)||
						(resident&&!addAllocation(provisionalStage[axis],stagingBytes)) ) return false;
				if( hostBytes>std::numeric_limits<std::uint64_t>::max()-residentBytes||
					hostBytes+residentBytes>std::numeric_limits<std::uint64_t>::max()-stagingBytes )
					return false;
				observedActualMetalBytes=std::max(observedActualMetalBytes,
					hostBytes+residentBytes+stagingBytes);
				if( observedActualMetalBytes>certifiedWorkingSetBytes||
					observedActualMetalBytes>(UINT64_C(1)<<31u) ) {
					if( error ) *error="production fire projection staging allocation exceeds certificate";
					return false;
				}
				id<MTLCommandBuffer> stagingCommand=InjectedFailure("staging_command")?
					nil:[context.queue commandBuffer];
				id<MTLBlitCommandEncoder> staging=InjectedFailure("staging_encoder")?
					nil:(stagingCommand?[stagingCommand blitCommandEncoder]:nil);
				if( !staging ) {if( error ) *error="production fire projection staging encoder allocation failed";return false;}
				{
					ProjectionTransferScope transferScope(ProjectionTerminalTransfer);
					CopyProjectionBuffer(staging,fineLevel.pressure,0u,pressureStage,0u,
						cells*sizeof(float));
					for( unsigned int axis=0;axis<3u;++axis ) {
						const std::size_t bytes=FireProductionProjectionFaceCount(shape,axis)*sizeof(float);
						CopyProjectionBuffer(staging,stored[axis],0u,storedStage[axis],0u,bytes);
						CopyProjectionBuffer(staging,momentum[axis],0u,momentumStage[axis],0u,bytes);
						CopyProjectionBuffer(staging,velocity[axis],0u,velocityStage[axis],0u,bytes);
						if( resident ) CopyProjectionBuffer(staging,provisional[axis],
							provisionalOffset[axis],provisionalStage[axis],0u,bytes);
					}
				}
				[staging endEncoding];CommitProjectionCommand(stagingCommand);
				++observedTerminalStaging;[stagingCommand waitUntilCompleted];
				if( [stagingCommand status]!=MTLCommandBufferStatusCompleted ) {
					if( error ) *error=MetalError("production fire projection staging command failed",[stagingCommand error]);return false;}

				const float* stagedPressure=static_cast<const float*>(ProjectionBufferContents(
					pressureStage,ProjectionTerminalAccess));
				result.pressurePa.assign(stagedPressure,stagedPressure+cells);
				for( unsigned int axis=0;axis<3u;++axis ) {
					const std::size_t count=FireProductionProjectionFaceCount(shape,axis);
					const float* stagedDensity=static_cast<const float*>(ProjectionBufferContents(
						storedStage[axis],ProjectionTerminalAccess));
					const float* stagedMomentum=static_cast<const float*>(ProjectionBufferContents(
						momentumStage[axis],ProjectionTerminalAccess));
					const float* stagedVelocity=static_cast<const float*>(ProjectionBufferContents(
						velocityStage[axis],ProjectionTerminalAccess));
					result.faceDensityKGPerM3[axis].assign(stagedDensity,stagedDensity+count);
					result.momentumKGPerM2S[axis].assign(stagedMomentum,stagedMomentum+count);
					result.velocityMPerS[axis].assign(stagedVelocity,stagedVelocity+count);
				}
				const MetalLevelParameters& parameter=*fp;
				for( unsigned int side=0;side<6u;++side ) {
					const std::size_t count=side<2u?shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
					const unsigned char* values=static_cast<const unsigned char*>(ProjectionBufferContents(
						inflow,ProjectionTerminalAccess))+parameter.sideOffset[side];
					result.pressureOpenInflow[side].assign(values,values+count);
				}
				} else {
					residentState->pressurePa=fineLevel.pressure;
					residentState->pressureOpenInflow=inflow;
					residentState->restoration=restoration;
					for( unsigned int axis=0u;axis<3u;++axis ) {
						residentState->faceDensityKGPerM3[axis]=stored[axis];
						residentState->momentumKGPerM2S[axis]=momentum[axis];
						residentState->velocityMPerS[axis]=velocity[axis];
						residentState->provisionalMomentumKGPerM2S[axis]=provisional[axis];
						residentState->provisionalMomentumByteOffset[axis]=provisionalOffset[axis];
						residentState->momentumByteOffset[axis]=0u;
					}
				}
				const float* d=static_cast<const float*>(ProjectionBufferContents(
					diagnostics,ProjectionDiagnosticAccess));
				result.maximumPreProjectionResidualPerS=d[0];result.maximumPostProjectionResidualPerS=d[1];
				result.removedFineRightHandSideMean=fp->nullspace!=0u?d[11]:0.0f;
				const float maximumVelocity=d[2];
				result.maximumOpenComplementarityDiscrepancyMPerS=d[3];
				result.executedVCycleCount=executedCycles;
				result.executedJacobiSweepCount=executedSweeps;
				result.residentUploadStagingCount=observedUploadStaging;
				if( projectionCommandCommitCount<beginningCommandCommits||
					projectionInterstageFullGridReadCount<beginningInterstageReads||
					projectionInvocationCount<beginningProjectionInvocations ) return false;
				result.residentInterstageDeviceToHostTransferCount=static_cast<std::uint32_t>(
					projectionInterstageFullGridReadCount-beginningInterstageReads);
				result.residentTerminalStagingCount=observedTerminalStaging;
				result.residentCommandCommitCount=static_cast<std::uint32_t>(
					projectionCommandCommitCount-beginningCommandCommits);
				result.residentProjectionInvocationCount=static_cast<std::uint32_t>(
					projectionInvocationCount-beginningProjectionInvocations);
				if( result.residentInterstageDeviceToHostTransferCount!=0u||
					result.residentProjectionInvocationCount!=1u ) {
					result=FireProductionProjectionResult();
					if( error ) *error="production fire projection resident topology changed";
					return false;
				}
				if( projectionBufferAllocationCount<beginningAllocationCount||
					projectionBufferAllocationBytes<beginningAllocationBytes ) {
					result=FireProductionProjectionResult();
					return false;
				}
				std::uint64_t borrowedBytes=0u;
				if( resident ) {
					const id<MTLBuffer> borrowedBase[]={fine.density,target,provisional[0]};
					for( id<MTLBuffer> buffer:borrowedBase ) {
						const std::uint64_t bytes=[buffer allocatedSize];
						if( borrowedBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) {
							result=FireProductionProjectionResult();return false;
						}
						borrowedBytes+=bytes;
					}
					for( unsigned int axis=1u;axis<3u;++axis ) if(
						provisional[axis]!=provisional[0] ) {
						const std::uint64_t bytes=[provisional[axis] allocatedSize];
						if( borrowedBytes>std::numeric_limits<std::uint64_t>::max()-bytes ) {
							result=FireProductionProjectionResult();return false;
						}
						borrowedBytes+=bytes;
					}
				}
				const std::uint64_t trackedCount=projectionBufferAllocationCount-
					beginningAllocationCount;
				const std::uint64_t trackedBytes=projectionBufferAllocationBytes-
					beginningAllocationBytes;
				const std::uint64_t expectedCount=10u*hierarchy.size()+
					(resident?(terminal?25u:12u):32u);
				if( residentBytes>std::numeric_limits<std::uint64_t>::max()-uploadBytes||
					residentBytes+uploadBytes>std::numeric_limits<std::uint64_t>::max()-stagingBytes||
					trackedBytes>std::numeric_limits<std::uint64_t>::max()-borrowedBytes||
					trackedCount!=expectedCount||
					trackedBytes+borrowedBytes!=residentBytes+uploadBytes+stagingBytes ) {
					result=FireProductionProjectionResult();
					if( error ) *error="production fire projection allocation topology changed: count="+
						std::to_string(trackedCount)+" expected="+std::to_string(expectedCount)+
						" tracked="+std::to_string(trackedBytes)+" borrowed="+
						std::to_string(borrowedBytes)+" resident="+std::to_string(residentBytes)+
						" upload="+std::to_string(uploadBytes)+" staging="+
						std::to_string(stagingBytes);
					return false;
				}
				observedActualMetalBytes=std::max(observedActualMetalBytes,
					residentBytes+uploadBytes);
				result.residentCertifiedWorkingSetBytes=certifiedWorkingSetBytes;
				result.residentActualMetalAllocationBytes=observedActualMetalBytes;
				const float length=shape.cellWidthM*static_cast<float>(std::max(shape.nx,std::max(shape.ny,shape.nz)));
				float maximumRestorationTarget=0.0f;
				if( restoration ) for( const float value:request.divergenceTargetPerS )
					maximumRestorationTarget=std::max(maximumRestorationTarget,std::fabs(value));
				result.validationBandPerS=restoration?0.005f*maximumRestorationTarget:
					0.005f*maximumVelocity/length;
				const bool validBand=restoration?
					FireProductionRestorationProjectionResidualWithinBand(
						result.maximumPostProjectionResidualPerS,maximumRestorationTarget,
						result.validationPassed):FireProductionProjectionResidualWithinBand(
						result.maximumPostProjectionResidualPerS,maximumVelocity,length,
						result.validationPassed);
				if( !validBand ) {
					result=FireProductionProjectionResult();if( error ) *error="production fire projection validation band overflowed";return false;}
				result.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
				result.deviceStartTimeS=[command GPUStartTime];
				result.deviceEndTimeS=[command GPUEndTime];
				if( InjectedFailure("output")&&!result.pressurePa.empty() )
					result.pressurePa[0]=std::numeric_limits<float>::quiet_NaN();
			}
			if( !AllFinite(result.pressurePa)||!std::isfinite(result.deviceElapsedMS)||
			!std::isfinite(result.maximumPreProjectionResidualPerS)||
			!std::isfinite(result.maximumPostProjectionResidualPerS)||
			!std::isfinite(result.maximumOpenComplementarityDiscrepancyMPerS) ) {
			result=FireProductionProjectionResult();if( error ) *error="production fire projection produced nonfinite output";return false;}
			for( unsigned int axis=0;axis<3u;++axis ) if( !AllFinite(result.faceDensityKGPerM3[axis])||
			!AllFinite(result.velocityMPerS[axis])||!AllFinite(result.momentumKGPerM2S[axis])||
			std::any_of(result.faceDensityKGPerM3[axis].begin(),result.faceDensityKGPerM3[axis].end(),
				[](float value){return !(value>0.0f);}) ) {
			result=FireProductionProjectionResult();if( error ) *error="production fire projection produced invalid face output";return false;}
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			result=FireProductionProjectionResult();if( error ) {try {*error="production fire projection allocation failed";}
			catch( const std::bad_alloc& ) {error->clear();}}return false;
		}
	}

	bool ProjectFireProductionMetal( const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result, std::string* error )
	{
		return ProjectFireProductionMetalImpl(request,0,ProjectionStandaloneTerminal,0,
			result,error);
	}

	bool ProjectFireProductionMetalResident( const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		FireProductionProjectionResult& result,std::string* error )
	{
		return ProjectFireProductionMetalImpl(request,&input,ProjectionResidentTerminal,0,
			result,error);
	}

	bool ProjectFireProductionMetalResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,std::string* error )
	{
		return ProjectFireProductionMetalImpl(request,&input,ProjectionResidentStateOnly,
			&state,result,error);
	}

	bool ProjectFireProductionMetalRestorationResident(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		id<MTLBuffer> expectedRestorationTargetPerS,
		FireProductionProjectionResult& result,std::string* error )
	{
		if( !expectedRestorationTargetPerS||
			input.divergenceTargetPerS!=expectedRestorationTargetPerS ) {
			result=FireProductionProjectionResult();
			if( error ) *error="production restoration projection target ownership is invalid";
			return false;
		}
		return ProjectFireProductionMetalImpl(request,&input,
			ProjectionResidentRestorationTerminal,0,result,error);
	}

	bool ProjectFireProductionMetalRestorationResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		id<MTLBuffer> expectedRestorationTargetPerS,
		FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,std::string* error )
	{
		if( !expectedRestorationTargetPerS||
			input.divergenceTargetPerS!=expectedRestorationTargetPerS ) {
			state=FireProductionMetalProjectionResidentState();
			result=FireProductionProjectionResult();
			if( error ) *error="production restoration projection target ownership is invalid";
			return false;
		}
		return ProjectFireProductionMetalImpl(request,&input,
			ProjectionResidentRestorationStateOnly,&state,result,error);
	}

	bool PublishFireProductionMetalRestorationResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,std::string* error )
	{
		try {
			if( !state.restoration||!state.pressurePa||!state.pressureOpenInflow||
				[state.pressurePa storageMode]!=MTLStorageModePrivate||
				[state.pressureOpenInflow storageMode]!=MTLStorageModeShared ) {
				if( error ) *error="production restoration publication state is invalid";
				return false;
			}
			MetalProjectionContext& context=Context();
			if( !context.Valid() ) {if( error ) *error=context.error;return false;}
			const FireProductionProjectionShape& shape=request.shape;
			const std::size_t cells=shape.CellCount();
			if( [state.pressurePa length]<cells*sizeof(float) ) return false;
			id<MTLBuffer> pressureStage=NewSharedBuffer(context.device,cells*sizeof(float));
			std::array<id<MTLBuffer>,3> densityStage,momentumStage,velocityStage,
				provisionalStage;
			std::uint64_t stagingBytes=pressureStage?[pressureStage allocatedSize]:0u;
			bool staged=pressureStage!=nil;
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t bytes=FireProductionProjectionFaceCount(shape,axis)*sizeof(float);
				const id<MTLBuffer> required[]={state.faceDensityKGPerM3[axis],
					state.momentumKGPerM2S[axis],state.velocityMPerS[axis],
					state.provisionalMomentumKGPerM2S[axis]};
				for( id<MTLBuffer> buffer:required ) staged=staged&&buffer&&
					[buffer storageMode]==MTLStorageModePrivate;
				staged=staged&&state.provisionalMomentumByteOffset[axis]<=
					[state.provisionalMomentumKGPerM2S[axis] length]&&bytes<=
					[state.provisionalMomentumKGPerM2S[axis] length]-
					state.provisionalMomentumByteOffset[axis];
				densityStage[axis]=NewSharedBuffer(context.device,bytes);
				momentumStage[axis]=NewSharedBuffer(context.device,bytes);
				velocityStage[axis]=NewSharedBuffer(context.device,bytes);
				provisionalStage[axis]=NewSharedBuffer(context.device,bytes);
				const id<MTLBuffer> outputs[]={densityStage[axis],momentumStage[axis],
					velocityStage[axis],provisionalStage[axis]};
				for( id<MTLBuffer> buffer:outputs ) {
					staged=staged&&buffer!=nil;
					if( buffer ) {
						const std::uint64_t allocation=[buffer allocatedSize];
						if( stagingBytes>std::numeric_limits<std::uint64_t>::max()-allocation )
							return false;
						stagingBytes+=allocation;
					}
				}
			}
			if( !staged||result.residentActualMetalAllocationBytes>
				std::numeric_limits<std::uint64_t>::max()-stagingBytes||
				result.residentActualMetalAllocationBytes+stagingBytes>
				result.residentCertifiedWorkingSetBytes ) {
				if( error ) *error="production restoration publication allocation exceeds certificate";
				return false;
			}
			const std::uint64_t beginningInterstageReads=projectionInterstageFullGridReadCount;
			id<MTLCommandBuffer> command=[context.queue commandBuffer];
			id<MTLBlitCommandEncoder> blit=command?[command blitCommandEncoder]:nil;
			if( !blit ) {if( error ) *error="production restoration publication encoder failed";
				return false;}
			{
				ProjectionTransferScope transferScope(ProjectionTerminalTransfer);
				CopyProjectionBuffer(blit,state.pressurePa,0u,pressureStage,0u,cells*sizeof(float));
				for( unsigned int axis=0u;axis<3u;++axis ) {
					const std::size_t bytes=FireProductionProjectionFaceCount(shape,axis)*sizeof(float);
					CopyProjectionBuffer(blit,state.faceDensityKGPerM3[axis],0u,
						densityStage[axis],0u,bytes);
					CopyProjectionBuffer(blit,state.momentumKGPerM2S[axis],0u,
						momentumStage[axis],0u,bytes);
					CopyProjectionBuffer(blit,state.velocityMPerS[axis],0u,
						velocityStage[axis],0u,bytes);
					CopyProjectionBuffer(blit,state.provisionalMomentumKGPerM2S[axis],
						state.provisionalMomentumByteOffset[axis],provisionalStage[axis],0u,bytes);
				}
			}
			[blit endEncoding];CommitProjectionCommand(command);[command waitUntilCompleted];
			if( [command status]!=MTLCommandBufferStatusCompleted||
				projectionInterstageFullGridReadCount!=beginningInterstageReads ) {
				if( error ) *error="production restoration publication transfer topology changed";
				return false;
			}
			const float* pressure=static_cast<const float*>(ProjectionBufferContents(
				pressureStage,ProjectionTerminalAccess));
			result.pressurePa.assign(pressure,pressure+cells);
			for( unsigned int axis=0u;axis<3u;++axis ) {
				const std::size_t count=FireProductionProjectionFaceCount(shape,axis);
				const float* density=static_cast<const float*>(ProjectionBufferContents(
					densityStage[axis],ProjectionTerminalAccess));
				const float* momentum=static_cast<const float*>(ProjectionBufferContents(
					momentumStage[axis],ProjectionTerminalAccess));
				const float* velocity=static_cast<const float*>(ProjectionBufferContents(
					velocityStage[axis],ProjectionTerminalAccess));
				result.faceDensityKGPerM3[axis].assign(density,density+count);
				result.momentumKGPerM2S[axis].assign(momentum,momentum+count);
				result.velocityMPerS[axis].assign(velocity,velocity+count);
			}
			std::size_t offset=0u;
			const unsigned char* inflow=static_cast<const unsigned char*>(
				ProjectionBufferContents(state.pressureOpenInflow,ProjectionTerminalAccess));
			for( unsigned int side=0u;side<6u;++side ) {
				const std::size_t count=side<2u?shape.ny*shape.nz:
					(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
				result.pressureOpenInflow[side].assign(inflow+offset,inflow+offset+count);
				offset+=count;
			}
			result.residentTerminalStagingCount+=1u;
			result.residentCommandCommitCount+=1u;
			result.residentActualMetalAllocationBytes+=stagingBytes;
			if( !AllFinite(result.pressurePa) ) return false;
			for( unsigned int axis=0u;axis<3u;++axis ) if(
				!AllFinite(result.faceDensityKGPerM3[axis])||
				!AllFinite(result.momentumKGPerM2S[axis])||
				!AllFinite(result.velocityMPerS[axis]) ) return false;
			if( error ) error->clear();return true;
		} catch( const std::bad_alloc& ) {
			if( error ) {
				try {*error="production restoration publication allocation failed";}
				catch( const std::bad_alloc& ) {error->clear();}
			}
			return false;
		}
	}
}
