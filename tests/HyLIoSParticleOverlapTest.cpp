#include "../src/Library/Materials/HyLIoS/HyLIoSParticleOverlap.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cfenv>
using namespace RISE::HyLIoS;
using S=ParticleOverlapStatus;
unsigned checks=0;
void Check(bool condition,const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
void Is(ProlateParticle a,ProlateParticle b,ParticleVector r,S expected,const char* message) {
    const auto result=ClassifyProlateOverlap(a,b,r);
    if (result.status!=expected) std::fprintf(stderr,"actual=%d expected=%d evaluations=%u\n",int(result.status),int(expected),result.evaluations);
    Check(result.status==expected,message);
    Check(result.evaluations<=64,"bounded contact search");
    const auto reverse=ClassifyProlateOverlap(b,a,{-r.x,-r.y,-r.z});
    Check(reverse.status==result.status && reverse.evaluations==result.evaluations,"argument swap symmetry");
}
ProlateParticle Scaled(ProlateParticle p,int k) {
    p.semiMinorMetres=std::ldexp(p.semiMinorMetres,k);
    p.semiMajorMetres=std::ldexp(p.semiMajorMetres,k);
    return p;
}
int main() {
    const ProlateParticle unit{1,1,{1,0,0}}, sphere2{2,2,{0,7,0}};
    Is(unit,unit,{0,0,0},S::Overlap,"coincident spheres");
    Is(unit,unit,{2,0,0},S::Touching,"exact sphere tangency");
    Is(unit,unit,{std::nextafter(2.0,0.0),0,0},S::Overlap,"sphere nextafter inside");
    Is(unit,unit,{std::nextafter(2.0,3.0),0,0},S::Disjoint,"sphere nextafter outside");
    Is(unit,sphere2,{3,0,0},S::Touching,"unequal sphere contact");
    Is(sphere2,ProlateParticle{3,3,{0,0,1}},{3,4,0},S::Touching,"Pythagorean sphere contact");
    Is(sphere2,ProlateParticle{3,3,{0,0,1}},{3,std::nextafter(4.0,5.0),0},S::Disjoint,"Pythagorean adjacent exterior");
    Is(sphere2,ProlateParticle{3,3,{0,0,1}},{3,std::nextafter(4.0,3.0),0},S::Overlap,"Pythagorean adjacent interior");
    const ProlateParticle axial{1,4,{0,0,2}};
    Is(axial,axial,{2,0,0},S::Touching,"equal spheroid equator contact");
    Is(axial,axial,{std::nextafter(2.0,0.0),0,0},S::Overlap,"equal spheroid nextafter equator inside");
    Is(axial,axial,{std::nextafter(2.0,3.0),0,0},S::Disjoint,"equal spheroid nextafter equator outside");
    Is(axial,axial,{0,0,8},S::Touching,"equal spheroid pole contact");
    Is(axial,axial,{0,0,std::nextafter(8.0,9.0)},S::Disjoint,"equal spheroid nextafter pole outside");
    // Same oriented spheroids are translates of the same affine unit ball:
    // contact iff r^T Q^-1 r=4. Axis=(1,1,0), major=4, minor=1.
    const ProlateParticle tilted{1,4,{1,1,0}};
    Is(tilted,tilted,{1,1,1},S::Overlap,"rotated equal spheroid affine distance interior");
    Is(tilted,tilted,{2,-2,0},S::Disjoint,"rotated equal spheroid affine distance exterior");
    // Both bounding balls have radius4 and overlap at centre distance3. The
    // long axes lie in xz, so y support is1 each: the y=1.5 plane separates.
    const ProlateParticle skewA{1,4,{1,0,1}},skewB{1,4,{-1,0,1}};
    Is(skewA,skewB,{0,3,0},S::Disjoint,"tilted disjoint ellipsoids despite overlapping bounding balls");
    Is(skewA,skewB,{0,1,0},S::Overlap,"crossed tilted spheroid interior");
    // At lambda=1/2 symmetry makes f=1 here, but interval uncertainty must
    // not be reclassified with an epsilon or retried as a rejected placement.
    Is(skewA,skewB,{0,2,0},S::Indeterminate,"generic unresolved exact contact is explicit");
    Is(skewA,skewB,{0,std::nextafter(2.0,3.0),0},S::Indeterminate,"generic nextafter uncertainty is explicit");
    // Exact anisotropic radial control for unequal coordinate-aligned shapes:
    // both y support radii1, so separation threshold2 regardless major length.
    Is(ProlateParticle{1,3,{1,0,0}},axial,{0,3,0},S::Disjoint,"unequal shape analytic separating plane");
    Is(ProlateParticle{1,3,{1,0,0}},axial,{0,1,0},S::Overlap,"unequal shape analytic overlap");
    const ProlateParticle homothetic1{1,2,{1,2,2}},homothetic2{2,4,{1,2,2}};
    // For homothetic ellipsoids the contact body has summed axes (3,6).
    Is(homothetic1,homothetic2,{1,2,2},S::Overlap,"homothetic rotated axial analytic interior");
    Is(homothetic1,homothetic2,{3,6,6},S::Disjoint,"homothetic rotated axial analytic exterior");
    Is(homothetic1,homothetic2,{2,-1,0},S::Overlap,"homothetic rotated transverse analytic interior");
    Is(homothetic1,homothetic2,{4,-2,0},S::Disjoint,"homothetic rotated transverse analytic exterior");
    Is(ProlateParticle{1,2,{1,0,0}},ProlateParticle{2,4,{1,0,0}},
        {5.99,0,0},S::Overlap,"unequal homothetic contact search maximum away from midpoint interior");
    Is(ProlateParticle{1,2,{1,0,0}},ProlateParticle{2,4,{1,0,0}},
        {6.01,0,0},S::Disjoint,"unequal homothetic contact search maximum away from midpoint exterior");
    for (int k:{-1000,-700,-23,0,500,1000}) {
        Is(Scaled(unit,k),Scaled(sphere2,k),{std::ldexp(3.,k),0,0},S::Touching,"sphere power-of-two uniform scale invariance");
        Is(Scaled(skewA,k),Scaled(skewB,k),{0,std::ldexp(3.,k),0},S::Disjoint,"spheroid power-of-two uniform scale invariance");
        Is(Scaled(skewA,k),Scaled(skewB,k),{0,std::ldexp(1.,k),0},S::Overlap,"spheroid scaled overlap");
    }
    const double tiny=std::numeric_limits<double>::denorm_min();
    Is(ProlateParticle{tiny,tiny,{1,0,0}},ProlateParticle{tiny,tiny,{1,0,0}},
        {2*tiny,0,0},S::Touching,"subnormal common-scale sphere contact");
    // Axis scaling and sign do not alter the exact real direction.
    Is(ProlateParticle{1,4,{-8,0,-8}},ProlateParticle{1,4,{0x1p+500,0,-0x1p+500}},
        {0,3,0},S::Disjoint,"axis representation invariance");
    const ProlateParticle microA{0.085e-6,0.205e-6,{1,2,3}},microB{0.14e-6,0.345e-6,{-2,1,4}};
    Is(microA,microB,{0,0,0.1e-6},S::Overlap,"physical micron sizes overlap");
    Is(microA,microB,{0,0,2e-6},S::Disjoint,"physical micron sizes disjoint");
    Is(ProlateParticle{0,1,{1,0,0}},unit,{0,0,0},S::InvalidInput,"invalid zero semiaxis");
    Is(ProlateParticle{1,2,{0,0,0}},unit,{0,0,0},S::InvalidInput,"invalid zero orientation");
    Is(ProlateParticle{1,2,{1,std::numeric_limits<double>::infinity(),0}},unit,{0,0,0},S::InvalidInput,"nonfinite orientation");
    Is(ProlateParticle{0x1p-200,1,{1,0,0}},unit,{0,0,0},S::UnsupportedRange,"extreme aspect ratio explicit unsupported range");
    Is(unit,unit,{0x1p+200,0,0},S::UnsupportedRange,"unsupported position/size spread");
    const int saved=std::fegetround();
    Check(std::fesetround(FE_DOWNWARD)==0,"set directed test environment");
    Is(unit,unit,{0,0,0},S::UnsupportedRange,"rounding mode contract");
    Check(std::fesetround(saved)==0,"restore rounding environment");
    std::printf("PASS: %u overlap contract checks\n",checks);
}
