// DL24 SCRATCH -- per-bounce energy ledger of the pre-fix CompositeSPF walk. Reverted.
#include <iostream>
#include <cmath>
#include <cstdio>
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"
#include "TestStubObject.h"
using namespace RISE; using namespace RISE::Implementation;
extern double g_dl24_exitTop[16], g_dl24_exitBot[16], g_dl24_dropType[16][5], g_dl24_dropRecur[16];
static void Reset(){ for(int i=0;i<16;i++){g_dl24_exitTop[i]=g_dl24_exitBot[i]=g_dl24_dropRecur[i]=0; for(int j=0;j<5;j++) g_dl24_dropType[i][j]=0;} }
static RayIntersectionGeometric MakeRI(double th){ const Vector3 d(std::sin(th),0,-std::cos(th)); RayIntersectionGeometric ri(Ray(Point3(std::sin(th),0,1),d), RasterizerState{0,0}); ri.bHit=true; ri.range=1.0/std::cos(th); ri.ptIntersection=Point3(0,0,0); ri.vNormal=Vector3(0,0,1); ri.onb.CreateFromW(Vector3(0,0,1)); ri.ptCoord=Point2(.5,.5); return ri; }
int main(){
  StubObject* stub=new StubObject(); stub->addref();
  UniformColorPainter* one=new UniformColorPainter(RISEPel(1,1,1)); one->addref();
  UniformScalarPainter* s1=new UniformScalarPainter(1.0); s1->addref();
  UniformScalarPainter* sior=new UniformScalarPainter(1.5); sior->addref();
  UniformScalarPainter* s0=new UniformScalarPainter(0.0); s0->addref();
  UniformScalarPainter* sBig=new UniformScalarPainter(1e6); sBig->addref();
  LambertianSPF* lamb=new LambertianSPF(*one); lamb->addref();
  DielectricSPF* dScat0=new DielectricSPF(*s1,*sior,*s0,false); dScat0->addref();
  DielectricSPF* dSmooth=new DielectricSPF(*s1,*sior,*sBig,false); dSmooth->addref();
  struct B{const char*n;unsigned r,a,b,c,d;} budgets[]={{"test 4/2/2/2/2",4,2,2,2,2},{"parser default 3/3",3,3,3,3,3},{"shipped scene 5/3",5,3,3,3,3},{"deep 20/10",20,10,10,10,10}};
  struct T{const char*n;DielectricSPF*d;} tops[]={{"dielectric scattering 0 (config 3)",dScat0},{"dielectric scattering 1e6 (smooth coat)",dSmooth}};
  const int N=200000;
  for(auto&t:tops) for(auto&b:budgets){
    CompositeSPF* c=new CompositeSPF(*t.d,*lamb,b.r,b.a,b.b,b.c,b.d,0.0,*s0); c->addref();
    for(double thd: {0.0,60.0}){
      Reset(); RandomNumberGenerator rng(1234); IndependentSampler smp(rng);
      RayIntersectionGeometric ri=MakeRI(thd*M_PI/180); IORStack st=MakeTestIORStack(stub);
      double rho=0; unsigned maxCount=0;
      for(int i=0;i<N;i++){ ScatteredRayContainer sc; c->Scatter(ri,smp,sc,st); if(sc.Count()>maxCount)maxCount=sc.Count(); for(unsigned j=0;j<sc.Count();j++){ if(Vector3Ops::Dot(sc[j].ray.Dir(),Vector3(0,0,1))>0) rho+=ColorMath::MaxValue(sc[j].kray);} }
      printf("\n== %s | budgets %s | theta %g : rho=%.4f (max rays/Scatter %u)\n",t.n,b.n,thd,rho/N,maxCount);
      double totExit=0,totDrop=0;
      for(int s=0;s<16;s++){ double dt=0; for(int j=0;j<5;j++) dt+=g_dl24_dropType[s][j]; if(g_dl24_exitTop[s]+g_dl24_exitBot[s]+dt+g_dl24_dropRecur[s]==0) continue;
        printf("  step %2d: exitTop %.4f exitBot %.4f | budget-drop refl %.4f refr %.4f diff %.4f transl %.4f other %.4f | max_recur-drop %.4f\n",s,g_dl24_exitTop[s]/N,g_dl24_exitBot[s]/N,g_dl24_dropType[s][0]/N,g_dl24_dropType[s][1]/N,g_dl24_dropType[s][2]/N,g_dl24_dropType[s][3]/N,g_dl24_dropType[s][4]/N,g_dl24_dropRecur[s]/N);
        totExit+=g_dl24_exitTop[s]+g_dl24_exitBot[s]; totDrop+=dt+g_dl24_dropRecur[s]; }
      printf("  TOTAL exit %.4f + dropped %.4f = %.4f\n", totExit/N,totDrop/N,(totExit+totDrop)/N);
    }
    c->release();
  }
  return 0;
}
