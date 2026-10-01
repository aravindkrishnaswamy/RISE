// Snapshot material reconstruction must preserve root alpha and painter ownership.
#include <cmath>
#include <iostream>
#include "../src/Library/Objects/SnapshotLeafClone.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
using namespace RISE;using namespace RISE::Implementation;
int main(){int pass=0,fail=0;auto check=[&](bool ok,const char* label){(ok?pass:fail)++;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;};
 const RayIntersectionGeometric ri(Ray(Point3(0,0,0),Vector3(0,0,1)),nullRasterizerState);
 auto* color=new UniformColorPainter(RISEPel(.5));
 for(unsigned mode=0;mode<3;++mode){
  auto* mat=new LambertianMaterial(*color);auto* alpha=new UniformScalarPainter(.6);
  mat->SetAlpha(alpha,static_cast<AlphaMode>(mode),.75);alpha->release();
  const IMaterial* clone=CloneMaterialForSnapshot(mat);
  check(clone!=mat,"standard material is independently reconstructed");
  const double expected=mode==0?1:mode==1?0:.6;
  check(std::fabs(clone->AlphaCoverage(ri)-expected)<1e-12,"mode and nondefault cutoff preserved");
  mat->SetAlpha(nullptr,eAlphaOpaque,.5);mat->release();
  check(std::fabs(clone->AlphaCoverage(ri)-expected)<1e-12,"clone owns alpha painter across source rebind and destruction");
  clone->release();
 }
 auto* base=new LambertianMaterial(*color);auto* a=new UniformScalarPainter(.2);base->SetAlpha(a,eAlphaBlend,.5);a->release();
 auto* inner=new LambertianLuminaireMaterial(*color,1,*base);a=new UniformScalarPainter(.3);inner->SetAlpha(a,eAlphaBlend,.5);a->release();
 auto* outer=new LambertianLuminaireMaterial(*color,1,*inner);a=new UniformScalarPainter(.4);outer->SetAlpha(a,eAlphaBlend,.5);a->release();
 const IMaterial* clone=CloneMaterialForSnapshot(outer);
 check(std::fabs(clone->AlphaCoverage(ri)-.4)<1e-12,"nested wrapper preserves root alpha rather than child alpha");
 outer->release();inner->release();base->release();color->release();
 check(std::fabs(clone->AlphaCoverage(ri)-.4)<1e-12,"existing shared-wrapper policy retains root and painter lifetime");clone->release();
 std::cout<<pass<<" passed / "<<fail<<" failed"<<std::endl;return fail?1:0;
}
