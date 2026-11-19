#include "pch.h"
#include "RoughDielectricEnergy.h"
#include "RoughDielectricEnergyData.h"
#include <algorithm>
#include <iterator>

using namespace RISE;
using namespace RISE::RoughDielectricEnergy;
namespace RD=MicrofacetUtils::RoughDielectric;
static_assert(std::size(Data::Missing)==RowCount*RowSize,"Regenerate rough dielectric energy data after grid edits");
static_assert(std::size(Data::Mean)==RowCount && std::size(Data::Transmission)==IndexCount,"Energy table shape mismatch");
namespace {
struct Interval { int index; Scalar t; };
template<int N> Interval Locate(Scalar value,const Scalar (&nodes)[N]) {
    int i=0;
    while(i<N-2 && value>nodes[i+1]) ++i;
    return {i,std::max(Scalar(0),std::min(Scalar(1),(value-nodes[i])/(nodes[i+1]-nodes[i])))};
}
Scalar Integral(Scalar a,Scalar h,Scalar d0,Scalar d1) {
    // Integral of mu * piecewise-linear deficit, over [a,a+h].
    return h*(a*(d0+d1)/2+h*(d0+2*d1)/6);
}
RD::PositiveProduct Sum(const RD::PositiveProduct& a,const RD::PositiveProduct& b) {
    if(a.mantissa==0) return b;
    if(b.mantissa==0) return a;
    const int exponent=std::max(a.exponent,b.exponent);
    RD::PositiveProduct result;
    result.mantissa=std::scalbn(a.mantissa,a.exponent-exponent)+std::scalbn(b.mantissa,b.exponent-exponent);
    result.exponent=exponent;
    return result;
}
}

Model::Model(const Vector3& incident,Scalar alphaX,Scalar alphaY,Scalar relativeIOR)
    : wi(incident),ax(alphaX),ay(alphaY),eta(relativeIOR),swapAxes(ax<ay),side(eta<1 ? 1 : 0),
      rows{},weights{},means{0,0},missing(0),tailTransmission(0),proposal(0)
{
    if(!RD::ValidParameters(ax,ay,eta) || ax==0 || eta==1 || wi.z<=0) return;
    const Scalar maximum=std::max(ax,ay),ratio=std::min(ax,ay)/maximum;
    const Scalar contrast=eta>=1 ? (eta-1)/(eta+1) : (1-eta)/(1+eta);
    const Interval r=Locate(maximum,RoughNodes),a=Locate(ratio,RatioNodes);
    Interval n=Locate(contrast,IndexNodes);
    if(n.index>0 && IndexNodes[n.index+1]<=0.001)
        n.t=std::log2(contrast/IndexNodes[n.index])/std::log2(IndexNodes[n.index+1]/IndexNodes[n.index]);
    n.t=std::max(Scalar(0),std::min(Scalar(1),n.t));
    for(int c=0;c<8;++c) {
        const int ir=c&1,ia=(c>>1)&1,in=(c>>2)&1;
        rows[c]=(((r.index+ir)*RatioCount+a.index+ia)*IndexCount+n.index+in)*2;
        weights[c]=(ir?r.t:1-r.t)*(ia?a.t:1-a.t)*(in?n.t:1-n.t);
        for(int f=0;f<2;++f) means[f]+=weights[c]*Data::Mean[rows[c]+f];
    }
    missing=Missing(wi,side);
    const Scalar sum=means[0]+means[1];
    if(sum==0 || means[side]==0 || missing==0) { missing=0; return; }
    Scalar transmission=Data::Transmission[n.index]*(1-n.t)+Data::Transmission[n.index+1]*n.t;
    // Diffuse Fresnel reciprocity: T(high->low) = T(low->high)/eta_high^2.
    if(side) { transmission*=eta; transmission*=eta; }
    // Kulla-Conty 2017, glass reciprocity slides 31-32: solve the two
    // reflection/transmission ratios jointly. Opposite-side deficit appears
    // in both the transmission lobe and its angular normalization.
    tailTransmission=transmission*(means[side^1]/sum);
    proposal=missing/(1+missing); // Keep SS support even if a baked deficit is one.
}

Scalar Model::Node(int face,int phi,int cosine) const {
    Scalar result=0;
    for(int c=0;c<8;++c) result+=weights[c]*(Scalar(Data::Missing[(rows[c]+face)*RowSize+phi*CosCount+cosine])/65535);
    return result;
}
Scalar Model::Missing(const Vector3& direction,int face) const {
    Scalar x=std::abs(direction.x),y=std::abs(direction.y);
    if(swapAxes) std::swap(x,y);
    const Scalar p=(std::atan2(y,x)/(PI/2))*(PhiCount-1);
    const int ip=std::min(PhiCount-2,static_cast<int>(p));
    const Scalar tp=p-ip,mu=std::abs(direction.z);
    int im=0;
    while(im<CosCount-2 && mu>CosNode(im+1)) ++im;
    const Scalar tm=(mu-CosNode(im))/(CosNode(im+1)-CosNode(im));
    const Scalar a=Node(face,ip,im)*(1-tm)+Node(face,ip,im+1)*tm;
    const Scalar b=Node(face,ip+1,im)*(1-tm)+Node(face,ip+1,im+1)*tm;
    return a*(1-tp)+b*tp;
}

Vector3 Model::DrawTail(int face,Scalar uPhi,Scalar uCos) const {
    Scalar values[PhiCount][CosCount],marginal[PhiCount]={};
    int scale=0;
    std::frexp(means[face],&scale);
    for(int p=0;p<PhiCount;++p) {
        // A common binary scale cancels from both CDFs. Keep tiny deficits
        // away from underflow while integrating their angular cell masses.
        for(int c=0;c<CosCount;++c) values[p][c]=std::scalbn(Node(face,p,c),-scale);
        for(int c=0;c<CosCount-1;++c)
            marginal[p]+=Integral(CosNode(c),CosNode(c+1)-CosNode(c),values[p][c],values[p][c+1]);
    }
    const Scalar quadrantSample=uPhi*4;
    const int quadrant=static_cast<int>(quadrantSample);
    Scalar total=0;
    for(int p=0;p<PhiCount-1;++p) total+=(marginal[p]+marginal[p+1])/2;
    Scalar target=(quadrantSample-quadrant)*total;
    int p=0;
    for(;p<PhiCount-2;++p) {
        const Scalar mass=(marginal[p]+marginal[p+1])/2;
        if(target<mass) break;
        target-=mass;
    }
    // Monotone inversion with a positive bracket avoids grazing endpoint
    // zeros and cancellation in a quadratic formula for rising/falling cells.
    Scalar lo=0,hi=1;
    for(int k=0;k<52;++k) {
        const Scalar t=(lo+hi)/2;
        const Scalar integral=t*(marginal[p]*(1-t/2)+marginal[p+1]*t/2);
        if(integral<target) lo=t; else hi=t;
    }
    const Scalar t=(lo+hi)/2,phi=(PI/2)*(p+t)/(PhiCount-1);
    Scalar curve[CosCount],mass[CosCount-1],sum=0;
    for(int c=0;c<CosCount;++c) curve[c]=values[p][c]*(1-t)+values[p+1][c]*t;
    for(int c=0;c<CosCount-1;++c) { mass[c]=Integral(CosNode(c),CosNode(c+1)-CosNode(c),curve[c],curve[c+1]); sum+=mass[c]; }
    target=uCos*sum;
    int c=0;
    for(;c<CosCount-2 && target>=mass[c];++c) target-=mass[c];
    lo=0; hi=1;
    const Scalar a=CosNode(c),h=CosNode(c+1)-a;
    for(int k=0;k<52;++k) {
        const Scalar v=(lo+hi)/2;
        const Scalar end=curve[c]*(1-v)+curve[c+1]*v;
        if(Integral(a,h*v,curve[c],end)<target) lo=v; else hi=v;
    }
    const Scalar mu=a+h*((lo+hi)/2),r=std::sqrt((1-mu)*(1+mu));
    Scalar x=r*std::cos(phi),y=r*std::sin(phi);
    if(quadrant&1) x=-x;
    if(quadrant&2) y=-y;
    if(swapAxes) std::swap(x,y);
    return Vector3(x,y,face==side ? mu : -mu);
}

RD::PositiveProduct Model::TailPdf(const Vector3& wo) const {
    const bool transmitted=wo.z<0;
    const int face=side^(transmitted?1:0);
    RD::PositiveProduct density;
    density.mantissa=0;
    if(means[face]==0 || wo.z==0) return density;
    const Scalar probability=transmitted ? tailTransmission : 1-tailTransmission;
    density.mantissa=1;
    density.Multiply(probability); density.Multiply(Missing(wo,face));
    density.Divide(means[face]); density.Multiply(std::abs(wo.z)); density.Divide(PI);
    return density;
}

RD::Evaluation Model::Combine(const Vector3& wo,RD::Evaluation ss,bool radiance) const {
    if(!ss.positiveDensity && proposal==0) return ss;
    RD::PositiveProduct ssPdf,ssF;
    ssPdf.mantissa=ss.pdfMantissa; ssPdf.exponent=ss.pdfExponent;
    ssF.mantissa=ss.fMantissa; ssF.exponent=ss.fExponent;
    RD::PositiveProduct q=TailPdf(wo),weightedSS=ssPdf,weightedMS=q;
    weightedSS.Multiply(1-proposal); weightedMS.Multiply(proposal);
    const RD::PositiveProduct pdf=Sum(weightedSS,weightedMS);
    RD::PositiveProduct tail=q;
    tail.Multiply(missing); tail.Divide(std::abs(wo.z));
    RD::PositiveProduct f=Sum(ssF,tail);
    RD::PositiveProduct numerator=ssPdf,tailNumerator=q;
    numerator.Multiply(ss.weight); tailNumerator.Multiply(missing);
    RD::PositiveProduct weight=Sum(numerator,tailNumerator);
    if(pdf.mantissa>0) weight.Divide(pdf);
    if(radiance && wo.z<0) { f.Divide(eta); f.Divide(eta); weight.Divide(eta); weight.Divide(eta); }
    RD::Evaluation result;
    result.f=f.Value(); result.pdf=pdf.Value(); result.weight=weight.Value();
    result.fMantissa=f.mantissa; result.fExponent=f.exponent;
    result.pdfMantissa=pdf.mantissa; result.pdfExponent=pdf.exponent;
    result.positiveDensity=pdf.mantissa>0;
    return result;
}
RD::Evaluation Model::Evaluate(const Vector3& wo,bool radiance) const {
    const auto ss=RD::Evaluate(wi,wo,ax,ay,eta,false);
    return wo.z==0 ? ss : Combine(wo,ss,radiance);
}
RD::Sample Model::Draw(Scalar u,Scalar v,Scalar lobe,Scalar technique,bool radiance) const {
    if(!(u>=0 && u<1 && v>=0 && v<1 && lobe>=0 && lobe<1 && technique>=0 && technique<1)) return {};
    RD::Sample result;
    RD::Evaluation ss;
    if(proposal>0 && technique<proposal) {
        result.transmission=lobe<tailTransmission;
        result.wo=DrawTail(side^(result.transmission?1:0),u,v);
        ss=RD::Evaluate(wi,result.wo,ax,ay,eta,false);
    } else {
        result=RD::Draw(wi,ax,ay,eta,u,v,lobe,false);
        if(!result.valid) return result;
        if(result.delta) return RD::Draw(wi,ax,ay,eta,u,v,lobe,radiance);
        ss.f=result.f; ss.pdf=result.pdf; ss.weight=result.weight; ss.positiveDensity=true;
        ss.fMantissa=result.fMantissa; ss.fExponent=result.fExponent;
        ss.pdfMantissa=result.pdfMantissa; ss.pdfExponent=result.pdfExponent;
    }
    const auto evaluated=Combine(result.wo,ss,radiance);
    result.f=evaluated.f; result.pdf=evaluated.pdf; result.weight=evaluated.weight;
    result.fMantissa=evaluated.fMantissa; result.fExponent=evaluated.fExponent;
    result.pdfMantissa=evaluated.pdfMantissa; result.pdfExponent=evaluated.pdfExponent;
    result.valid=evaluated.positiveDensity;
    return result;
}
