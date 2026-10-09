// DL-353: wavelength-isolated caustics against independent Snell/flux integration.
#define main SupersededSuiteMain
#include "SMSSupersededRowsTest.cpp"
#undef main
// Independent planar mapping: for interface radius r around the source's
// projection, receiver radius R=r*(1+n/sqrt(1-(n*n-1)*r*r)). The radial
// area Jacobian is (R/r)*dR/dr. Integrate over camera footprints, including
// the unobstructed direct path outside the finite pane.
static double PlanarCentroid(double n,unsigned subdivisions)
{
    double total=0,weighted=0;
    for(unsigned y=0;y<16*subdivisions;++y) for(unsigned x=0;x<64*subdivisions;++x) {
        const double px=double(x+.5)/subdivisions;
        const double worldX=(.5-px/64)*3;
        const double worldZ=(double(y+.5)/(16*subdivisions)-.5)*1.5;
        const double dx=worldX-.5,R=std::hypot(dx,worldZ);
        double low=0,high=1/std::sqrt(n*n-1);
        for(unsigned it=0;it<48;++it) {
            const double r=(low+high)/2,D=std::sqrt(1-(n*n-1)*r*r);
            if(r*(1+n/D)<R) low=r; else high=r;
        }
        const double r=(low+high)/2,D=std::sqrt(1-(n*n-1)*r*r),f=1+n/D;
        const double u=.5+(R?r*dx/R:0),v=R?r*worldZ/R:0;
        const double ci=D/std::sqrt(1+r*r),ct=1/std::sqrt(1+r*r);
        const double rs=(ci-n*ct)/(ci+n*ct),rp=(n*ci-ct)/(n*ci+ct);
        const double F=.5*(rs*rs+rp*rp);
        const double indirect=std::fabs(u)<.5 && std::fabs(v)<.5
            ? (1-F)/std::pow(1+r*r,1.5)/(f*(f+n*(n*n-1)*r*r/std::pow(D,3))) : 0;
        const double direct=std::fabs((worldX+.5)/2)>.5 || std::fabs(worldZ/2)>.5
            ? 2/std::pow(4+R*R,1.5) : 0;
        const double value=indirect+direct;
        total+=value;weighted+=(std::floor(px)+.5)*value;
    }
    return weighted/total;
}
int main(int argc,char** argv)
{
    const std::string mode=argc>1?argv[1]:"uniform";
    for(double nm:{450.,650.}) {
        std::array<Series,3> sms;
        const double n=Sf11(nm+.005);
        const double expected=PlanarCentroid(n,32),coarse=PlanarCentroid(n,16);
        // Deterministic integration error is separate from the MC band.
        const double quadrature=std::fabs(expected-coarse)*2;
        for(unsigned salt=0;salt<4;++salt) {
            g_renderIndex=salt;
            std::string raster=SpecRaster(mode,false,nm,nm+.01,160,256);
            raster.insert(raster.find("\n}\n"),"\n max_diffuse_bounce 0\n");
            auto image=Render(PaneScene(true,raster),"353_sms");
            Check(image.ok,"dispersion reference renders valid");
            const auto a=Centroids(image);
            for(unsigned channel=0;channel<3;++channel) sms[channel].v.push_back(a[channel]);
        }
        for(unsigned channel=0;channel<3;++channel) {
            const auto& sample=sms[channel];
            std::cout<<mode<<" nm="<<nm<<" channel="<<channel<<" centroid="<<sample.mean()<<" +/- "<<sample.se()
                <<" closed="<<expected<<" quadrature="<<quadrature<<std::endl;
            Check(std::fabs(sample.mean()-expected)<=3*sample.se()+quadrature,
                "wavelength caustic centroid agrees with independent Snell/flux mapping");
        }
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
