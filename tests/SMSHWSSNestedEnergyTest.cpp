// DL-391: matched 160-point quadrature; each RGB channel must agree within
// the combined three-sigma uncertainty of four independent ValueSalt renders.
#define main SupersededRowsMain
#include "SMSSupersededRowsTest.cpp"
#undef main
int main(int argc, char**) {
    XYZPel discrete(0,0,0), continuous(0,0,0);
    for(unsigned i=0;i<160;++i) {
        XYZPel v; ColorUtils::XYZFromNM(v,450+i*1.25); discrete=discrete+v/160.;
    }
    for(unsigned i=0;i<20000;++i) {
        XYZPel v; ColorUtils::XYZFromNM(v,450+(i+.5)*.01); continuous=continuous+v/20000.;
    }
    std::cout << "flat spectrum continuous/discrete XYZ " << continuous.X/discrete.X
        << " " << continuous.Y/discrete.Y << " " << continuous.Z/discrete.Z << std::endl;
    if(argc>1) return 0;
    Nested(true,false,true,4);
    std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount?1:0;
}
