// Standalone deterministic bake, using the same VNDF as the live kernel.
// c++ -std=c++17 -O3 -Isrc/Library tools/GenerateRoughDielectricEnergy.cpp -o /tmp/rise-rd-bake
// /tmp/rise-rd-bake 2048 > src/Library/Utilities/RoughDielectricEnergyData.h
#include "../src/Library/Utilities/RoughDielectricEnergy.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using namespace RISE;
using namespace RISE::RoughDielectricEnergy;
namespace RD=MicrofacetUtils::RoughDielectric;

static Scalar RadicalInverse(uint32_t bits) {
    bits=(bits<<16)|(bits>>16);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    return (Scalar(bits)+.5)/4294967296.0;
}
static Scalar Weight(const Vector3& wi,const Vector3& wo,Scalar ax,Scalar ay) {
    if(wi.z==0) return 1;
    const Scalar ci=wi.z,co=std::abs(wo.z);
    const Scalar li=std::hypot(ci,std::hypot(ax*wi.x,ay*wi.y));
    const Scalar lo=std::hypot(co,std::hypot(ax*wo.x,ay*wo.y));
    return ((ci+li)*co)/(li*co+lo*ci);
}
static Scalar FresnelFromContrast(Scalar ci,Scalar si,Scalar contrast,bool inside,Scalar& ct,Scalar& T) {
    // Reconstructing eta erases the first positive contrast knot. Keep
    // Nt^2-Ni^2 = +/-4c explicit, especially at grazing/TIR incidence.
    const Scalar ni=inside ? 1+contrast : 1-contrast;
    const Scalar nt=inside ? 1-contrast : 1+contrast;
    if(ci>=si) {
        const Scalar a=std::fma(inside ? -contrast : contrast,1+si,1-si);
        if(a<=0) { ct=0; T=0; return 1; }
        ct=std::sqrt((a/nt)*((nt+ni*si)/nt));
    } else {
        const Scalar incident=ni*ci;
        const Scalar discriminant=std::fma(incident,incident,inside ? -4*contrast : 4*contrast);
        if(discriminant<=0) { ct=0; T=0; return 1; }
        ct=std::sqrt(discriminant)/nt;
    }
    Scalar rs=0,rp=0,ts=0,tp=0;
    RD::PolarizedFresnel(ni*ci,nt*ct,rs,ts);
    RD::PolarizedFresnel(nt*ci,ni*ct,rp,tp);
    Scalar F=(rs+rp)*.5;
    T=(ts+tp)*.5;
    if(F<=T) T=1-F; else F=1-T;
    return F;
}
static Scalar Energy(Scalar ax,Scalar ay,Scalar contrast,bool inside,Scalar phi,Scalar mu,int samples) {
    if(ax==0 && ay==0) return 1;
    const Scalar sinTheta=std::sqrt((1-mu)*(1+mu));
    Vector3 wi(phi==PI/2 ? 0 : sinTheta*std::cos(phi),sinTheta*std::sin(phi),mu);
    if(contrast==0 && mu>0) {
        const Scalar t=std::hypot(ax*wi.x,ay*wi.y),l=std::hypot(mu,t);
        return 1-Scalar(.5)*(t/l)*(t/(l+mu)); // eta -> 1, not the exact delta.
    }
    if(mu==0 && std::hypot(ax*wi.x,ay*wi.y)==0) {
        // Grazing along a zero-width axis: the reflection escape limit is
        // the 1D conductor's normal-incidence energy; Fresnel tends to one.
        wi=Vector3(0,0,1); contrast=1;
    }
    if(contrast==0) return inside ? 0 : 1;
    const Scalar eta=inside ? (1-contrast)/(1+contrast) : (1+contrast)/(1-contrast);
    Scalar sum=0;
    for(int k=0;k<samples;++k) {
        const Vector3 m=RD::SampleVisibleNormal(wi,ax,ay,(Scalar(k)+.5)/samples,RadicalInverse(static_cast<uint32_t>(k)));
        const Scalar ci=Vector3Ops::Dot(wi,m);
        const Vector3 reflected=m*(2*ci)-wi;
        Scalar ct=0,T=0;
        const Vector3 cross=RD::Cross(wi,m);
        const Scalar F=contrast==1 ? Scalar(1) : FresnelFromContrast(ci,std::hypot(cross.x,std::hypot(cross.y,cross.z)),contrast,inside,ct,T);
        if(reflected.z>0) sum+=F*Weight(wi,reflected,ax,ay);
        if(T>0) {
            const Scalar invEta=1/eta;
            const Scalar indexCorrection=inside ? (2*contrast)/(1-contrast) : (-2*contrast)/(1+contrast);
            const Vector3 transmitted=eta>=.5 && eta<=2
                ? -wi+(m*((invEta+1)/(ci*invEta+ct))-wi)*indexCorrection
                : wi*(-invEta)+m*(ci*invEta-ct);
            if(transmitted.z<0) sum+=T*Weight(wi,transmitted,ax,ay);
        }
    }
    return sum/samples;
}
int main(int argc,char** argv) {
    const int samples=argc>1 ? std::atoi(argv[1]) : 2048;
    if(samples<=0) return 1;
    std::vector<uint16_t> data(RowCount*RowSize);
    std::vector<Scalar> means(RowCount);
    std::atomic<int> next{0};
    std::vector<std::thread> workers;
    for(int worker=0;worker<8;++worker) workers.emplace_back([&] {
        for(int row;(row=next.fetch_add(1))<RowCount;) {
            int q=row; const int side=q%2; q/=2;
            const int index=q%IndexCount; q/=IndexCount;
            const int ratio=q%RatioCount,rough=q/RatioCount;
            const Scalar ax=RoughNodes[rough],ay=ax*RatioNodes[ratio];
            Scalar average=0;
            for(int p=0;p<PhiCount;++p) {
                const Scalar phi=(PI/2)*Scalar(p)/(PhiCount-1);
                for(int c=0;c<CosCount;++c) {
                    Scalar e=Energy(ax,ay,IndexNodes[index],side!=0,phi,CosNode(c),samples);
                    if(!(e>=0 && e<=1+1e-12)) std::abort();
                    const Scalar deficit=std::max(Scalar(0),1-e);
                    data[row*RowSize+p*CosCount+c]=static_cast<uint16_t>(std::lround(65535*deficit));
                }
                Scalar integral=0;
                for(int c=0;c<CosCount-1;++c) {
                    const Scalar a=CosNode(c),h=CosNode(c+1)-a;
                    const Scalar da=Scalar(data[row*RowSize+p*CosCount+c])/65535;
                    const Scalar db=Scalar(data[row*RowSize+p*CosCount+c+1])/65535;
                    integral+=h*(a*(da+db)/2+h*(da+2*db)/6);
                }
                average+=integral*((p==0 || p==PhiCount-1) ? .5 : 1);
            }
            means[row]=2*average/(PhiCount-1);
        }
    });
    for(auto& w:workers) w.join();
    std::printf("// Generated by tools/GenerateRoughDielectricEnergy.cpp (%d QMC samples/node).\n",samples);
    std::printf("// 16-bit deficit grid; means exactly integrate its bilinear angular model.\n#pragma once\n#include <cstdint>\nnamespace RISE { namespace RoughDielectricEnergy { namespace Data {\ninline constexpr uint16_t Missing[]={\n");
    for(size_t i=0;i<data.size();++i) std::printf("%u,%s",data[i],i%24==23 ? "\n" : "");
    std::printf("};\ninline constexpr double Mean[]={\n");
    for(size_t i=0;i<means.size();++i) std::printf("%.17g,%s",means[i],i%8==7 ? "\n" : "");
    std::printf("};\ninline constexpr double Transmission[]={\n");
    for(Scalar contrast:IndexNodes) {
        Scalar sum=0;
        if(contrast<1) for(int k=0;k<16384;++k) {
            const Scalar mu=std::sqrt((k+.5)/16384.0);
            Scalar ct=0,T=0; FresnelFromContrast(mu,std::sqrt((1-mu)*(1+mu)),contrast,false,ct,T); sum+=T;
        }
        std::printf("%.17g,\n",sum/16384);
    }
    std::printf("};\n} } }\n");
}
