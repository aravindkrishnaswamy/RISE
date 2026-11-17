// DL-178 / DL-212: published BRDF formulas and directional-energy bounds.
// The reference uses the unnormalised half-vector, independently of the
// production factor helpers; quadrature integrates outgoing solid angle.
#include <cmath>
#include <cstdio>
#include "../src/Library/Materials/SchlickBRDF.h"
#include "../src/Library/Materials/SchlickMasking.h"
#include "../src/Library/Materials/IsotropicPhongBRDF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongBRDF.h"
#include "../src/Library/Materials/WardIsotropicGaussianBRDF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
using namespace RISE;
using namespace RISE::Implementation;
static int checks=0, failures=0;
static void Check(bool ok,const char* label,double actual,double expected) {
 ++checks; if(!ok) { ++failures; printf("FAIL %s: %.12g expected %.12g\n",label,actual,expected); }
}
static RayIntersectionGeometric Hit(double degrees) {
 double th=degrees*PI/180; RasterizerState rs={0,0};
 RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(sin(th),0,-cos(th))),rs);
 ri.bHit=true; ri.vNormal=ri.vGeomNormal=Vector3(0,0,1); ri.onb.CreateFromW(ri.vNormal);
 return ri;
}
// Schlick 1994 Eq.31 supplies G; Geisler-Moroder/Duer 2010 uses
// |l+v|^2 / (n.(l+v))^4 times the Gaussian / (pi ax ay).
static double PublishedReference(int model,const Vector3& l,const RayIntersectionGeometric& ri,double r,double p) {
 Vector3 v=-ri.ray.Dir(), H=l+v; double H2=Vector3Ops::SquaredModulus(H), hz=H.z;
 double nv=v.z,nl=l.z;
 if(model==0) {
  Vector3 h=H*(1.0/sqrt(H2)); double t=h.z, hv=Vector3Ops::Dot(h,v);
  Vector3 tangent=h-ri.onb.w()*t; double len=sqrt(Vector3Ops::SquaredModulus(tangent));
  double w=len>0?Vector3Ops::Dot(ri.onb.v(),tangent)/len:0;
  double A=sqrt(p/(p*p+(1-p*p)*w*w));
  double Z=r/std::pow(1-(1-r)*t*t,2);
  // DL-225: Eq.31's G(c)/c = 1/(r+(1-r)c) clipped to the Smith
  // projected-area bound of Z*A (SchlickMaskingBoundTest validates the
  // helper against exact Smith independently of this file).
  SchlickMasking::Lane lane; SchlickMasking::Prepare(lane,r,p);
  double mv=SchlickMasking::MaskOverCos(lane,nv,Vector3Ops::Dot(v,ri.onb.u()),Vector3Ops::Dot(v,ri.onb.v()));
  double ml=SchlickMasking::MaskOverCos(lane,nl,Vector3Ops::Dot(l,ri.onb.u()),Vector3Ops::Dot(l,ri.onb.v()));
  return (.9+.1*std::pow(1-hv,5))*Z*A*mv*ml/(4*PI);
 }
 double ax=r,ay=model==1?r:p;
 double x=Vector3Ops::Dot(H,ri.onb.u()),y=Vector3Ops::Dot(H,ri.onb.v());
 return .5*exp(-(x*x/(ax*ax)+y*y/(ay*ay))/(hz*hz))*H2/(PI*ax*ay*std::pow(hz,4));
}
int main() {
 auto* black=new UniformColorPainter(RISEPel(0,0,0)); black->addref();
 auto* spec=new UniformColorPainter(RISEPel(.9,.9,.9)); spec->addref();
 auto* ws=new UniformColorPainter(RISEPel(.5,.5,.5)); ws->addref();
 const double roughs[]={.1,.5,.8},degs[]={0,30,60,80,89.9};
 for(int model=0;model<3;++model) for(double r:roughs) {
  auto* rough=new UniformScalarPainter(r); rough->addref();
  auto* iso=new UniformScalarPainter(model==2?.12:1); iso->addref();
  IBSDF* b=model==0?static_cast<IBSDF*>(new SchlickBRDF(*black,*spec,*rough,*iso)):
   model==1?static_cast<IBSDF*>(new WardIsotropicGaussianBRDF(*black,*ws,*rough)):
   static_cast<IBSDF*>(new WardAnisotropicEllipticalGaussianBRDF(*black,*ws,*rough,*iso)); b->addref();
  for(double d:degs) {
   auto ri=Hit(d); double integral=0;
   for(int t=0;t<400;++t) {
    double th=(t+.5)*PI/800,ct=cos(th),st=sin(th);
    for(int q=0;q<800;++q) {
     double phi=(q+.5)*TWO_PI/800; Vector3 l(st*cos(phi),st*sin(phi),ct);
     integral+=b->value(l,ri)[0]*ct*st*(PI/800)*(TWO_PI/800);
    }
   }
   printf("ENERGY model=%d r=%.2f theta=%.1f Q=%.9f\n",model,r,d,integral);
   // DL-225 closed: the grazing Schlick rows were printed as an OPEN
   // witness (1.244529893 at r .1, 89.9 deg); they are now gated.
   Check(std::isfinite(integral)&&integral <= (model==0?1:.5)+1e-4,"prescribed specular energy family",integral,model==0?1:.5);
   for(double out:{15.,45.,85.}) for(double phi:{.2,1.0,2.2}) {
    double th=out*PI/180; Vector3 l(sin(th)*cos(phi),sin(th)*sin(phi),cos(th));
    double expected=PublishedReference(model,l,ri,r,model==2?.12:1), actual=b->value(l,ri)[0];
    Check(fabs(actual-expected)<=1e-10*(1+expected),"published RGB formula",actual,expected);
    // UniformColorPainter's scalar spectrum is not its RGB value; compare
    // the directional factor after dividing by its own public spectral input.
    double rho=model==0?GuardedGetColorNM(*spec,ri,550):GuardedGetColorNM(*ws,ri,550);
    Vector3 h=Vector3Ops::Normalize(l-ri.ray.Dir()); double F=std::pow(1-Vector3Ops::Dot(h,-ri.ray.Dir()),5);
    double nmExpected=GuardedGetColorNM(*black,ri,550)*INV_PI+expected*(model==0?(rho+(1-rho)*F)/(.9+.1*F):rho/.5);
    double nmActual=b->valueNM(l,ri,550);
    Check(fabs(nmActual-nmExpected)<=1e-10*(1+nmExpected),"published NM formula",nmActual,nmExpected);
   }
  }
  b->release();rough->release();iso->release();
 }
 // Independent half-angle solid-angle reference (with reflection Jacobian)
 // for the auxiliary estimate. This resolves the outgoing grazing peak,
 // including chromatic reflectance, anisotropy and view azimuth.
 auto* ar=new RGBScalarPainter(.02,.1,.8);ar->addref();
 auto* ap=new RGBScalarPainter(.3,.1,1);ap->addref();
 auto* ac=new UniformColorPainter(RISEPel(.2,.9,.5));ac->addref();
 auto* ab=new SchlickBRDF(*black,*ac,*ar,*ap);ab->addref();
 for(double angle:{0.,60.,80.,89.}) for(double azimuth:{0.,PI/4,PI/2}) {
  auto ri=Hit(angle);double th=angle*PI/180;
  ri.ray.SetDir(Vector3(sin(th)*cos(azimuth),sin(th)*sin(azimuth),-cos(th)));
  RISEPel q(0,0,0);
  for(int t=0;t<400;++t) {
   double ot=(t+.5)*PI/800,ct=cos(ot),st=sin(ot);
   for(int k=0;k<800;++k) {
    double ph=(k+.5)*TWO_PI/800;
    Vector3 h(st*cos(ph),st*sin(ph),ct);
    double hv=Vector3Ops::Dot(h,-ri.ray.Dir());Vector3 wo=ri.ray.Dir()+2*hv*h;
    if(hv>0&&wo.z>0) q=q+ab->value(wo,ri)*(wo.z*4*hv*st*(PI/800)*(TWO_PI/800));
   }
  }
  RISEPel a=ab->albedo(ri);
  for(int ch=0;ch<3;++ch) {
   double expected=r_min(1.0,q[ch]);
   printf("AOV theta=%.1f azimuth=%.4f ch=%d estimate=%.9f reference=%.9f\n",angle,azimuth,ch,a[ch],expected);
   Check(fabs(a[ch]-expected)<.02,"directional albedo estimate absolute error < .02",a[ch],expected);
  }
 }
 ab->release();ac->release();ar->release();ap->release();
 // Sibling audit: integrate the two Phong families directly. Their own
 // normalizations contain the energy bound; neither needs Schlick's G.
 for(int model=0;model<2;++model) for(double exponent:{10.,40.,100.}) {
  auto* nu=new UniformScalarPainter(exponent);nu->addref();
  auto* nv=new UniformScalarPainter(3*exponent);nv->addref();
  IBSDF* sibling=model==0?static_cast<IBSDF*>(new IsotropicPhongBRDF(*black,*spec,*nu)):
   static_cast<IBSDF*>(new AshikminShirleyAnisotropicPhongBRDF(*nu,*nv,*black,*spec));sibling->addref();
  for(double angle:{0.,60.,80.,89.9}) {
   auto ri=Hit(angle);double q=0;
   for(int t=0;t<400;++t) {
    double theta=(t+.5)*PI/800,ct=cos(theta),st=sin(theta);
    for(int j=0;j<800;++j) {
     double phi=(j+.5)*TWO_PI/800;
     Vector3 wo(st*cos(phi),st*sin(phi),ct);
     q+=sibling->value(wo,ri)[0]*ct*st*(PI/800)*(TWO_PI/800);
    }
   }
   printf("SIBLING model=%d exponent=%.0f theta=%.1f Q=%.9f\n",model,exponent,angle,q);
   Check(std::isfinite(q)&&q<=1.0001,"Phong sibling energy bound",q,1);
  }
  sibling->release();nu->release();nv->release();
 }
 auto* rd=new UniformColorPainter(RISEPel(.4,.4,.4));rd->addref();
 auto* rough=new UniformScalarPainter(.8);rough->addref();
 auto* iso=new UniformScalarPainter(1);iso->addref();
 auto* b=new SchlickBRDF(*rd,*spec,*rough,*iso);b->addref();
 for(double d:degs) { auto ri=Hit(d);double a=b->albedo(ri)[0]; Check(a>=0&&a<=1,"Schlick albedo AOV contract",a,1); }
 b->release();rd->release();rough->release();iso->release();black->release();spec->release();ws->release();
 printf("Checks: %d Failures: %d\n",checks,failures);return failures?1:0;
}
