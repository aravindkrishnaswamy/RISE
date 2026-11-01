// r271: Fourier check of the production Metal viscous operator, legacy vs the opt-in
// compact MAC stencil (RISE_FIRE_COMPACT_VISCOUS_STENCIL=1). Periodic 8^3 box, rho=1,
// molecular nu only (C_v=0), single modes; prints rate/(nu*u) against the stencil symbols.
// Then a uniform u_x with open sides, a no-slip floor and an open top: only the floor layer
// should feel stress, at -2/dx^2 for the compact stencil (velocity zero at the wall).
#include "Utilities/FireProductionForce.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace RISE;
int main(){
 const std::size_t n=8;const float dx=0.1f,nu=0.01f;const double pi=3.14159265358979323846,k=2.0*pi/(n*dx);
 FireProductionProjectionShape shape;shape.nx=shape.ny=shape.nz=n;shape.cellWidthM=dx;
 auto faceIndex=[&](int axis,std::size_t x,std::size_t y,std::size_t z){
  return axis==0?(z*n+y)*(n+1)+x:(axis==1?(z*(n+1)+y)*n+x:(z*n+y)*n+x);};
 struct Mode{const char* name;int axis;int along;bool checker;double scale;};
 const Mode modes[]={{"shear u_x(y), wave 8dx",0,1,false,1.0},{"shear u_x(y), checkerboard 2dx",0,1,true,1.0},
  {"normal u_z(z), wave 8dx",2,2,false,4.0/3.0},{"normal u_z(z), checkerboard 2dx",2,2,true,4.0/3.0}};
 for(const Mode& mode:modes){
  FireProductionNonpressureMomentumRHSRequest request;FireProductionFrozenForceRequest& f=request.force;
  f.shape=shape;f.timeStepS=1e-3f;f.vremanCoefficient=0.0f;
  f.cellGasDensityKGPerM3.assign(n*n*n,1.0f);f.molecularKinematicViscosityM2PerS.assign(n*n*n,nu);
  request.cellGasPhaseSourceRateKGPerM3S.assign(n*n*n,0.0f);
  for(int a=0;a<3;++a){const std::size_t faces=FireProductionProjectionFaceCount(shape,a);
   f.faceDensityKGPerM3[a].assign(faces,1.0f);f.beginningMomentumKGPerM2S[a].assign(faces,0.0f);}
  for(std::size_t z=0;z<=n;++z)for(std::size_t y=0;y<=n;++y)for(std::size_t x=0;x<=n;++x){
   std::size_t xyz[3]={x,y,z};bool inside=true;
   for(int a=0;a<3;++a)if(xyz[a]>=(a==mode.axis?n+1:n))inside=false;
   if(!inside)continue;
   const std::size_t j=xyz[mode.along]%n;const double position=mode.axis==mode.along?j*dx:(j+0.5)*dx;
   const double value=mode.checker?((j&1u)?-1.0:1.0):std::sin(k*position);
   f.beginningMomentumKGPerM2S[mode.axis][faceIndex(mode.axis,x,y,z)]=static_cast<float>(value);
  }
  FireProductionNonpressureMomentumRHSResult result;FireProductionNonpressureMomentumRHSMetalDiagnostics diagnostics;
  std::string error;
  if(!EvaluateFireProductionNonpressureMomentumRHSMetal(request,result,diagnostics,&error)){
   std::printf("%s: failed: %s\n",mode.name,error.c_str());return 1;}
  double numerator=0.0,denominator=0.0;const auto& rate=result.stressMomentumRateKGPerM2S2[mode.axis];
  const auto& u=f.beginningMomentumKGPerM2S[mode.axis];
  for(std::size_t i=0;i<u.size();++i){numerator+=rate[i]*u[i];denominator+=u[i]*u[i];}
  const double kk=mode.checker?pi/dx:k,compact=std::pow(2.0*std::sin(kk*dx/2.0)/dx,2.0),
   legacy=std::pow(std::sin(kk*dx)/dx,2.0);
  std::printf("%-34s measured %10.3f   compact symbol %10.3f   legacy symbol %10.3f   continuum %10.3f\n",
   mode.name,-numerator/denominator/nu,mode.scale*compact,mode.scale*legacy,mode.scale*kk*kk);
 }
 {
  FireProductionNonpressureMomentumRHSRequest request;FireProductionFrozenForceRequest& f=request.force;
  f.shape=shape;f.timeStepS=1e-3f;f.vremanCoefficient=0.0f;
  f.boundary={{FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
   FireProductionProjectionPressureOpen,FireProductionProjectionPressureOpen,
   FireProductionProjectionWall,FireProductionProjectionPressureOpen}};
  f.cellGasDensityKGPerM3.assign(n*n*n,1.0f);f.molecularKinematicViscosityM2PerS.assign(n*n*n,nu);
  request.cellGasPhaseSourceRateKGPerM3S.assign(n*n*n,0.0f);
  for(int a=0;a<3;++a){const std::size_t faces=FireProductionProjectionFaceCount(shape,a);
   f.faceDensityKGPerM3[a].assign(faces,1.0f);f.beginningMomentumKGPerM2S[a].assign(faces,a==0?1.0f:0.0f);}
  FireProductionNonpressureMomentumRHSResult result;FireProductionNonpressureMomentumRHSMetalDiagnostics diagnostics;
  std::string error;
  if(!EvaluateFireProductionNonpressureMomentumRHSMetal(request,result,diagnostics,&error)){
   std::printf("bounded: failed: %s\n",error.c_str());return 1;}
  double floorMin=1e30,floorMax=-1e30,restMax=0.0,otherMax=0.0;
  for(std::size_t z=0;z<n;++z)for(std::size_t y=0;y<n;++y)for(std::size_t x=1;x<n;++x){
   const double rate=result.stressMomentumRateKGPerM2S2[0][faceIndex(0,x,y,z)]/nu;
   if(!std::isfinite(rate)){std::printf("bounded: nonfinite rate\n");return 1;}
   if(z==0){floorMin=std::fmin(floorMin,rate);floorMax=std::fmax(floorMax,rate);}
   else restMax=std::fmax(restMax,std::fabs(rate));}
  for(int a=1;a<3;++a)for(float r:result.stressMomentumRateKGPerM2S2[a])otherMax=std::fmax(otherMax,std::fabs(r/nu));
  std::printf("bounded uniform u_x: floor-layer rate/nu %.3f..%.3f, other layers max |.| %.3g, v/w max |.| %.3g\n",
   floorMin,floorMax,restMax,otherMax);
 }
 return 0;}
