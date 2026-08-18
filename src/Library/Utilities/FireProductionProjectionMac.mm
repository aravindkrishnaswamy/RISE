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
	namespace
	{
		struct MetalLevelParameters
		{
			std::uint32_t nx,ny,nz,nullspace;
			float sx,sy,sz,ambientDensity;
			float timeStepS;
			std::uint32_t boundary[6];
			std::uint32_t sideOffset[6];
		};
		static_assert(sizeof(MetalLevelParameters)==84u,
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
 uint nx,ny,nz,nullspace;float sx,sy,sz,ambient,dt;uint boundary[6];uint sideOffset[6];
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
 uint cz=axis==2u?(positive?p.nz-1u:0u):z;bool inside=outward<0.0f;inflow[p.sideOffset[side]+gid]=inside?1u:0u;
 float pressure=0.0f;if(inside){float speed2=normal*normal;for(uint t=0u;t<3u;++t)if(t!=axis){
  float q=centered_velocity(vel[t],p,t,cx,cy,cz);speed2+=q*q;}pressure=-0.5f*p.ambient*speed2;}
 pb[p.sideOffset[side]+gid]=pressure;
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
 float residual=divergence-target[gid],value=-residual/p.dt;absoluteResidual[gid]=abs(residual);
 device const float* beta[3]={bx,by,bz};uint coordinate[3]={x,y,z},extent[3]={p.nx,p.ny,p.nz};
 for(uint axis=0u;axis<3u;++axis)for(uint high=0u;high<2u;++high){uint side=2u*axis+high;
  if(p.boundary[side]!=1u||coordinate[axis]!=(high?extent[axis]-1u:0u))continue;
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
  value+=w*coarse[cell_index(c,ix?x1:x0,iy?y1:y0,iz?z1:z0)];}fine[gid]+=value;
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
kernel void cell_post_residual(device const float* vx [[buffer(0)]],device const float* vy [[buffer(1)]],
 device const float* vz [[buffer(2)]],device const float* target [[buffer(3)]],device float* output [[buffer(4)]],
 constant LevelParams& p [[buffer(5)]],uint gid [[thread_position_in_grid]]){
 uint count=p.nx*p.ny*p.nz;if(gid>=count)return;uint x=gid%p.nx,r=gid/p.nx,y=r%p.ny,z=r/p.ny;
 float divergence=(vx[face_index(p,0u,x+1u,y,z)]-vx[face_index(p,0u,x,y,z)]+
  vy[face_index(p,1u,x,y+1u,z)]-vy[face_index(p,1u,x,y,z)]+
  vz[face_index(p,2u,x,y,z+1u)]-vz[face_index(p,2u,x,y,z)])/p.sx;
 output[gid]=abs(divergence-target[gid]);
}
)METAL";
		}

		struct MetalProjectionContext
		{
			id<MTLDevice> device;
			id<MTLCommandQueue> queue;
			id<MTLComputePipelineState> buildFaces,buildDiagonal,restrictAverage,
				setupVelocity,classifyOpen,buildRHS,jacobi,computeResidual,
				prolongate,clearValues,copyReduction,sumPass,maxPass,subtractMean,
				storeRoot,correctFaces,copySeam,postResidual;
			std::string error;

			MetalProjectionContext() : device(nil),queue(nil),buildFaces(nil),buildDiagonal(nil),
				restrictAverage(nil),setupVelocity(nil),classifyOpen(nil),buildRHS(nil),
				jacobi(nil),computeResidual(nil),prolongate(nil),clearValues(nil),
				copyReduction(nil),sumPass(nil),maxPass(nil),subtractMean(nil),storeRoot(nil),
				correctFaces(nil),copySeam(nil),postResidual(nil)
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
					classifyOpen=make("classify_open");buildRHS=make("build_rhs");jacobi=make("jacobi");
					computeResidual=make("compute_residual");prolongate=make("prolongate_add");
					clearValues=make("clear_values");copyReduction=make("copy_reduction");
					sumPass=make("sum_pass");maxPass=make("max_pass");subtractMean=make("subtract_mean");
					storeRoot=make("store_root");correctFaces=make("correct_faces");
					copySeam=make("copy_periodic_seam");postResidual=make("cell_post_residual");
					if( !buildFaces||!buildDiagonal||!restrictAverage||!setupVelocity||!classifyOpen||
						!buildRHS||!jacobi||!computeResidual||!prolongate||!clearValues||!copyReduction||
						!sumPass||!maxPass||!subtractMean||!storeRoot||!correctFaces||!copySeam||
						!postResidual ) {
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
			return [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
		}

		id<MTLBuffer> NewBufferWithBytes( id<MTLDevice> device,const void* bytes,std::size_t size )
		{
			return [device newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
		}

		bool AllFinite( const std::vector<float>& values )
		{
			return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
		}

		MetalLevelParameters Parameters( const MetalLevel& level,
			const FireProductionProjectionRequest& request )
		{
			MetalLevelParameters p={};p.nx=static_cast<std::uint32_t>(level.nx);
			p.ny=static_cast<std::uint32_t>(level.ny);p.nz=static_cast<std::uint32_t>(level.nz);
			p.nullspace=std::find(request.boundary.begin(),request.boundary.end(),
				FireProductionProjectionPressureOpen)==request.boundary.end()?1u:0u;
			p.sx=level.spacing[0];p.sy=level.spacing[1];p.sz=level.spacing[2];
			p.ambientDensity=request.ambientDensityKGPerM3;p.timeStepS=request.timeStepS;
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
				const MetalLevelParameters* p=static_cast<const MetalLevelParameters*>([level.parameters contents]);
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
			const MetalLevelParameters* cp=static_cast<const MetalLevelParameters*>([coarse.parameters contents]);
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

	bool ProjectFireProductionMetal( const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result, std::string* error )
	{
		result=FireProductionProjectionResult();
		try {
			if( !ValidateFireProductionProjectionRequest(request,error) ) return false;
			MetalProjectionContext& context=Context();
			if( !context.Valid() ) {if( error ) *error=context.error;return false;}
			@autoreleasepool {
				const FireProductionProjectionShape& shape=request.shape;
				const std::size_t cells=shape.CellCount(),finePadded=NextPowerOfTwo(cells);
				std::vector<MetalLevel> hierarchy;MetalLevel fine={};fine.nx=shape.nx;fine.ny=shape.ny;fine.nz=shape.nz;
				fine.spacing[0]=fine.spacing[1]=fine.spacing[2]=shape.cellWidthM;
				fine.density=NewBufferWithBytes(context.device,request.gasDensityKGPerM3.data(),cells*sizeof(float));
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
					const MetalLevelParameters parameters=Parameters(level,request);
					level.parameters=NewBufferWithBytes(context.device,&parameters,sizeof(parameters));
				}
				std::array<id<MTLBuffer>,3> provisional,stored,momentum,velocity;
				for( unsigned int axis=0;axis<3u;++axis ) {
					const std::size_t count=FireProductionProjectionFaceCount(shape,axis),bytes=count*sizeof(float);
					provisional[axis]=NewBufferWithBytes(context.device,
						request.provisionalMomentumKGPerM2S[axis].data(),bytes);
					stored[axis]=NewBuffer(context.device,bytes);momentum[axis]=NewBuffer(context.device,bytes);
					velocity[axis]=NewBuffer(context.device,bytes);
				}
				id<MTLBuffer> target=NewBufferWithBytes(context.device,request.divergenceTargetPerS.data(),cells*sizeof(float));
				const std::size_t boundaryCount=2u*(shape.ny*shape.nz+shape.nx*shape.nz+shape.nx*shape.ny);
				id<MTLBuffer> boundaryPressure=NewBuffer(context.device,boundaryCount*sizeof(float));
				id<MTLBuffer> inflow=NewBuffer(context.device,boundaryCount*sizeof(unsigned char));
				id<MTLBuffer> scratch=NewBuffer(context.device,finePadded*sizeof(float));
				id<MTLBuffer> diagnostics=NewBuffer(context.device,12u*sizeof(float));
				bool allocated=target&&boundaryPressure&&inflow&&scratch&&diagnostics;
				for( const MetalLevel& level:hierarchy ) allocated=allocated&&level.density&&level.rhs&&
					level.pressure&&level.temporary&&level.residual&&level.diagonal&&level.parameters&&
					level.beta[0]&&level.beta[1]&&level.beta[2];
				for( unsigned int axis=0;axis<3u;++axis ) allocated=allocated&&provisional[axis]&&stored[axis]&&
					momentum[axis]&&velocity[axis];
				if( InjectedFailure("buffer") ) allocated=false;
				if( !allocated ) {if( error ) *error="production fire projection buffer allocation failed";return false;}
				std::fill_n(static_cast<float*>([boundaryPressure contents]),boundaryCount,0.0f);
				std::fill_n(static_cast<unsigned char*>([inflow contents]),boundaryCount,
					static_cast<unsigned char>(0u));
				std::fill_n(static_cast<float*>([diagnostics contents]),12u,0.0f);

				id<MTLCommandBuffer> command=[context.queue commandBuffer];
				if( InjectedFailure("command_buffer") ) command=nil;
				if( !command ) {if( error ) *error="production fire projection command allocation failed";return false;}
				id<MTLComputeCommandEncoder> encoder=nil;
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
					[encoder setBuffer:provisional[axis] offset:0 atIndex:0];[encoder setBuffer:stored[axis] offset:0 atIndex:1];
					[encoder setBuffer:momentum[axis] offset:0 atIndex:2];[encoder setBuffer:velocity[axis] offset:0 atIndex:3];
					[encoder setBuffer:fineLevel.parameters offset:0 atIndex:4];[encoder setBytes:&axisParameters length:sizeof(axisParameters) atIndex:5];
					Dispatch(encoder,context.setupVelocity,FireProductionProjectionFaceCount(shape,axis));[encoder endEncoding];
				}
				for( unsigned int side=0;side<6u;++side ) if( request.boundary[side]==FireProductionProjectionPressureOpen ) {
					const MetalAxisParameters sideParameters={0u,0u,side,0u};
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
				const MetalLevelParameters* fp=static_cast<const MetalLevelParameters*>([fineLevel.parameters contents]);
				if( fp->nullspace!=0u&&!EncodeRemoveMean(context,command,fineLevel.rhs,cells,scratch,diagnostics,11u,error) ) return false;
				for( MetalLevel& level:hierarchy ) {
					const std::size_t count=level.nx*level.ny*level.nz;
					MetalReductionParameters clear={static_cast<std::uint32_t>(count),0u,0u,0u};
					if( !Begin(command,encoder,error,"pressure clear") ) return false;
					[encoder setBuffer:level.pressure offset:0 atIndex:0];[encoder setBytes:&clear length:sizeof(clear) atIndex:1];
					Dispatch(encoder,context.clearValues,count);[encoder endEncoding];
				}
				std::uint32_t executedCycles=0u;std::uint64_t executedSweeps=0u;
				for( unsigned int cycle=0;cycle<12u;++cycle ) {
					if( !EncodeVCycle(context,command,hierarchy,0u,scratch,diagnostics,
						executedSweeps,error) ) return false;
					if( fp->nullspace!=0u&&!EncodeRemoveMean(context,command,fineLevel.pressure,cells,scratch,
						diagnostics,std::numeric_limits<std::uint32_t>::max(),error) ) return false;
					++executedCycles;
				}
				for( unsigned int axis=0;axis<3u;++axis ) {
					const MetalAxisParameters axisParameters={axis,0u,0u,0u};
					if( !Begin(command,encoder,error,"face correction") ) return false;
					[encoder setBuffer:provisional[axis] offset:0 atIndex:0];[encoder setBuffer:stored[axis] offset:0 atIndex:1];
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
				if( !Begin(command,encoder,error,"post residual") ) return false;
				for( unsigned int axis=0;axis<3u;++axis ) [encoder setBuffer:velocity[axis] offset:0 atIndex:axis];
				[encoder setBuffer:target offset:0 atIndex:3];[encoder setBuffer:fineLevel.residual offset:0 atIndex:4];
				[encoder setBuffer:fineLevel.parameters offset:0 atIndex:5];Dispatch(encoder,context.postResidual,cells);[encoder endEncoding];
				if( !EncodeReduction(context,command,fineLevel.residual,cells,scratch,diagnostics,1u,true,error) ) return false;
				[command commit];[command waitUntilCompleted];
				if( [command status]!=MTLCommandBufferStatusCompleted||InjectedFailure("command") ) {
					if( error ) *error=MetalError("production fire projection command failed",[command error]);return false;}

				result.pressurePa.assign(static_cast<const float*>([fineLevel.pressure contents]),
					static_cast<const float*>([fineLevel.pressure contents])+cells);
				for( unsigned int axis=0;axis<3u;++axis ) {
					const std::size_t count=FireProductionProjectionFaceCount(shape,axis);
					result.faceDensityKGPerM3[axis].assign(static_cast<const float*>([stored[axis] contents]),
						static_cast<const float*>([stored[axis] contents])+count);
					result.momentumKGPerM2S[axis].assign(static_cast<const float*>([momentum[axis] contents]),
						static_cast<const float*>([momentum[axis] contents])+count);
					result.velocityMPerS[axis].assign(static_cast<const float*>([velocity[axis] contents]),
						static_cast<const float*>([velocity[axis] contents])+count);
				}
				const MetalLevelParameters& parameter=*fp;
				for( unsigned int side=0;side<6u;++side ) {
					const std::size_t count=side<2u?shape.ny*shape.nz:(side<4u?shape.nx*shape.nz:shape.nx*shape.ny);
					const unsigned char* values=static_cast<const unsigned char*>([inflow contents])+parameter.sideOffset[side];
					result.pressureOpenInflow[side].assign(values,values+count);
				}
				const float* d=static_cast<const float*>([diagnostics contents]);
				result.maximumPreProjectionResidualPerS=d[0];result.maximumPostProjectionResidualPerS=d[1];
				result.removedFineRightHandSideMean=fp->nullspace!=0u?d[11]:0.0f;
				float maximumVelocity=0.0f;
				for( unsigned int axis=0;axis<3u;++axis )
					for( std::size_t face=0;face<result.velocityMPerS[axis].size();++face ) {
						const std::size_t coordinate=axis==0u?face%(shape.nx+1u):
							(axis==1u?(face/shape.nx)%(shape.ny+1u):face/(shape.nx*shape.ny));
						const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
						const bool wall=(coordinate==0u||coordinate==extent)&&
							request.boundary[2u*axis+(coordinate?1u:0u)]==
								FireProductionProjectionWall;
						if( !wall ) maximumVelocity=std::max(maximumVelocity,std::fabs(
							request.provisionalMomentumKGPerM2S[axis][face]/
							result.faceDensityKGPerM3[axis][face]));
						maximumVelocity=std::max(maximumVelocity,
							std::fabs(result.velocityMPerS[axis][face]));
					}
				result.maximumOpenComplementarityDiscrepancyMPerS=0.0f;
				for( unsigned int side=0;side<6u;++side ) if( request.boundary[side]==FireProductionProjectionPressureOpen ) {
					const unsigned int axis=side/2u;const bool positive=(side&1u)!=0u;
					const std::size_t firstCount=axis==0u?shape.ny:shape.nx;
					const std::size_t secondCount=axis==2u?shape.ny:shape.nz;
					for( std::size_t second=0;second<secondCount;++second ) for( std::size_t first=0;first<firstCount;++first ) {
						std::size_t x=0u,y=0u,z=0u;if( axis==0u ){x=positive?shape.nx:0u;y=first;z=second;}
						if( axis==1u ){x=first;y=positive?shape.ny:0u;z=second;}
						if( axis==2u ){x=first;y=second;z=positive?shape.nz:0u;}
						const std::size_t face=axis==0u?(z*shape.ny+y)*(shape.nx+1u)+x:
							(axis==1u?(z*(shape.ny+1u)+y)*shape.nx+x:(z*shape.ny+y)*shape.nx+x);
						const float outward=(positive?1.0f:-1.0f)*result.velocityMPerS[axis][face];
						const std::size_t index=second*firstCount+first;
						const bool inside=result.pressureOpenInflow[side][index]!=0u;
						result.maximumOpenComplementarityDiscrepancyMPerS=std::max(
							result.maximumOpenComplementarityDiscrepancyMPerS,
							inside?std::max(0.0f,outward):std::max(0.0f,-outward));
					}
				}
				result.executedVCycleCount=executedCycles;
				result.executedJacobiSweepCount=executedSweeps;
				const float length=shape.cellWidthM*static_cast<float>(std::max(shape.nx,std::max(shape.ny,shape.nz)));
				if( !FireProductionProjectionResidualWithinBand(result.maximumPostProjectionResidualPerS,
					maximumVelocity,length,result.validationPassed) ) {
					result=FireProductionProjectionResult();if( error ) *error="production fire projection validation band overflowed";return false;}
				result.deviceElapsedMS=([command GPUEndTime]-[command GPUStartTime])*1000.0;
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
}
