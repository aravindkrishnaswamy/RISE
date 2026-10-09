// DL-466 independent unfolded-image quadrature, no SMS or Optics calls.
// Each side reflection mirrors the light in x/y. Parallel entry/exit faces
// conserve n sin(theta); invert R=3.5 tan(theta)+h tan(theta_glass).
// Irradiance is I sin(theta)/(R dR/dtheta), times native dielectric Fresnel.
// k=4 includes two orthogonal side TIRs and a top/bottom reflection pair.
#pragma once
#include <array>

#include <cmath>
#include <algorithm>
#include <limits>
namespace SMSPlanarSlabReference {
constexpr double pi=3.14159265358979323846;
struct Path { double theta, lateral, jacobian, fresnel; };
inline Path snell(double r, double h) {
 double a=0,b=pi/2;
 for(int k=0;k<45;++k) {double m=(a+b)/2, s=std::sin(m);
  double d=3.5*std::tan(m)+h*s/std::sqrt(2.25-s*s);
  if(d<r)a=m;else b=m;}
 double t=(a+b)/2,s=std::sin(t),c=std::cos(t),g=std::sqrt(1-s*s/2.25);
 double rs=(c-1.5*g)/(c+1.5*g),rp=(1.5*c-g)/(1.5*c+g);
 double derivative=3.5/(c*c)+h*2.25*c/std::pow(2.25-s*s,1.5);
 return {t,h*s/std::sqrt(2.25-s*s),r>1e-12?s/(r*derivative):1/std::pow(3.5+h/1.5,2),.5*(rs*rs+rp*rp)};
}
inline std::array<double,3> Closed(unsigned N,double lx,double ly,double cx,double cy,double half) {
 double totals[3]={};
 for(unsigned j=0;j<N;++j) for(unsigned i=0;i<N;++i) {
  double x=cx-half+(i+.5)*2*half/N,y=cy-half+(j+.5)*2*half/N;
  for(int sx=-1;sx<=1;++sx) for(int sy=-1;sy<=1;++sy) {
   double imageX=sx?6*sx-lx:lx,imageY=sy?6*sy-ly:ly;
   double dx=imageX-x,dy=imageY-y,r=std::hypot(dx,dy);
   auto p=snell(r,.5);double u=r>0?dx/r:0,v=r>0?dy/r:0;
   double entryDist=1.75*std::tan(p.theta);
   double ex=x+u*entryDist,ey=y+v*entryDist;
   if(std::fabs(ex)>=3 || std::fabs(ey)>=3) continue;
   double tx=ex+u*p.lateral,ty=ey+v*p.lateral;
   if(sx ? !(sx*tx>3 && sx*tx<9):std::fabs(tx)>=3) continue;
   if(sy ? !(sy*ty>3 && sy*ty<9):std::fabs(ty)>=3) continue;
   int reflections=(sx!=0)+(sy!=0);
   totals[reflections]+=40/pi*p.jacobian*std::pow(1-p.fresnel,2);
  }
  double r=std::hypot(lx-x,ly-y);auto p=snell(r,1.5);
  double u=r>0?(lx-x)/r:0,v=r>0?(ly-y)/r:0,entry=1.75*std::tan(p.theta);
  double ex=x+u*entry,ey=y+v*entry,tx=ex+u*p.lateral,ty=ey+v*p.lateral;
  if(std::fabs(ex)<3 && std::fabs(ey)<3 && std::fabs(tx)<3 && std::fabs(ty)<3)
   totals[2]+=40/pi*p.jacobian*std::pow(1-p.fresnel,2)*p.fresnel*p.fresnel;
 }
 return {totals[0]/(N*N),totals[1]/(N*N),totals[2]/(N*N)};
}
// Lateral-distance margins: spacing of the two side reflections, and
// distance of each from entry/exit. Zero is a chart/order/existence boundary.
inline std::array<double,2> CornerMargins(unsigned N,double lx,double ly,double cx,double cy,double half) {
 double separation=std::numeric_limits<double>::infinity(),boundary=separation;
 for(unsigned j=0;j<N;++j) for(unsigned i=0;i<N;++i) {
  double x=cx-half+(i+.5)*2*half/N,y=cy-half+(j+.5)*2*half/N;
  for(int sx:{-1,1}) for(int sy:{-1,1}) {
   double dx=6*sx-lx-x,dy=6*sy-ly-y,r=std::hypot(dx,dy);
   auto p=snell(r,.5);double u=dx/r,v=dy/r;
   double ex=x+u*1.75*std::tan(p.theta),ey=y+v*1.75*std::tan(p.theta);
   double tx=ex+u*p.lateral,ty=ey+v*p.lateral;
   if(std::fabs(ex)>=3 || std::fabs(ey)>=3 || sx*tx<=3 || sy*ty<=3) continue;
   double ax=(3*sx-ex)/u,ay=(3*sy-ey)/v;
   separation=std::min(separation,std::fabs(ax-ay));
   boundary=std::min(boundary,std::min({ax,ay,p.lateral-ax,p.lateral-ay}));
  }
 }
 return {separation,boundary};
}

}
