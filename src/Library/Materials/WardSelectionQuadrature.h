// Boundary-aware expectation of Ward's realized diffuse selection weight.
// Pure value inputs permit exact bounded thread-local memoization. See
// docs/DL178_DL212_BOUNDED_SCHLICK_WARD.md for the domain transformation.
#ifndef RISE_WARD_SELECTION_QUADRATURE_H
#define RISE_WARD_SELECTION_QUADRATURE_H
#include "../Utilities/Math3D/Math3D.h"
#include <algorithm>
#include <cmath>
namespace RISE { namespace Implementation { namespace WardSelection {
struct Input {
    double vx,vy,nv,gx,gy,ng,wD;
    int count;
    double ax[3],ay[3],weight[3];
};
inline bool Equal(const Input& a,const Input& b) {
    if(a.vx!=b.vx || a.vy!=b.vy || a.nv!=b.nv || a.gx!=b.gx ||
       a.gy!=b.gy || a.ng!=b.ng || a.wD!=b.wD || a.count!=b.count) return false;
    for(int j=0;j<a.count;++j)
        if(a.ax[j]!=b.ax[j] || a.ay[j]!=b.ay[j] || a.weight[j]!=b.weight[j]) return false;
    return true;
}
constexpr double pi=3.14159265358979323846264338327950288;
static const double nodes8[8]={-0.96028985649753618,-0.79666647741362673,-0.52553240991632899,-0.18343464249564984,0.18343464249564984,0.52553240991632899,0.79666647741362673,0.96028985649753618};
static const double weights8[8]={0.10122853629037652,0.22238103445337445,0.31370664587788716,0.36268378337836182,0.36268378337836182,0.31370664587788716,0.22238103445337445,0.10122853629037652};
static const double nodes32[32]={-0.99726386184948157,-0.98561151154526827,-0.96476225558750639,-0.93490607593773967,-0.89632115576605209,-0.84936761373256997,-0.79448379596794239,-0.73218211874028971,-0.66304426693021523,-0.5877157572407623,-0.50689990893222936,-0.42135127613063533,-0.33186860228212767,-0.23928736225213706,-0.14447196158279649,-0.048307665687738317,0.048307665687738317,0.14447196158279649,0.23928736225213706,0.33186860228212767,0.42135127613063533,0.50689990893222936,0.5877157572407623,0.66304426693021523,0.73218211874028971,0.79448379596794239,0.84936761373256997,0.89632115576605209,0.93490607593773967,0.96476225558750639,0.98561151154526827,0.99726386184948157};
static const double weights32[32]={0.0070186100094703618,0.016274394730906486,0.025392065309262728,0.034273862913021633,0.042835898022226926,0.05099805926237596,0.058684093478535461,0.0658222227763616,0.072345794108848199,0.078193895787070117,0.083311924226946624,0.087652093004403686,0.09117387869576371,0.093844399080804386,0.095638720079274597,0.096540088514727576,0.096540088514727576,0.095638720079274597,0.093844399080804386,0.09117387869576371,0.087652093004403686,0.083311924226946624,0.078193895787070117,0.072345794108848199,0.0658222227763616,0.058684093478535461,0.05099805926237596,0.042835898022226926,0.034273862913021633,0.025392065309262728,0.016274394730906486,0.0070186100094703618};

// At most five initial angular cuts plus ten per lane; radial cuts have
// three fixed endpoints plus at most seven roots per lane. Arrays and work
// are bounded independently of numeric convergence or scene content.
struct Cuts {
    double v[40]; int n;
    explicit Cuts(double end):n(2) {v[0]=0;v[1]=end;}
    void Add(double x) {if(x>v[0] && x<v[1]) v[n++]=x;}
    void Sort() {
        std::sort(v,v+n);int out=1;
        for(int i=1;i<n;++i) if(v[i]-v[out-1]>1e-12) v[out++]=v[i];
        n=out;
    }
};
inline void AddRoots(Cuts& cuts,double a,double b,double c) {
    // Scale before discriminant evaluation; q-form avoids cancellation
    // of the small root. Exact linear/zero cases need no arbitrary epsilon.
    const double scale=std::max(std::abs(a),std::max(std::abs(b),std::abs(c)));
    if(scale==0) return;
    a/=scale;b/=scale;c/=scale;
    if(a==0) {if(b!=0) cuts.Add(-c/b);return;}
    const double disc=b*b-4*a*c;
    if(disc<0) return;
    const double q=-.5*(b+std::copysign(std::sqrt(disc),b));
    if(q==0) {cuts.Add(-b/(2*a));return;}
    cuts.Add(q/a);cuts.Add(c/q);
}
inline double SampleAzimuth(double physical,double ax,double ay) {
    double psi=std::atan2(ax*std::sin(physical),ay*std::cos(physical));
    if(psi<0) psi+=2*pi;
    return psi;
}
struct Lane {double a2,sv,sg,weight;};
inline double Radial(const Input& in,double psi,unsigned* nodes) {
    const double vg=in.vx*in.gx+in.vy*in.gy+in.nv*in.ng;
    // Omitted Gaussian tail mass exp(-4.5^2) < 1.61e-9. Adding it
    // with coefficient one gives a conservative error of that size.
    Cuts cuts(4.5);cuts.Add(2.0);Lane lanes[3];
    for(int j=0;j<in.count;++j) {
        // This elliptical slope map is the same joint distribution as
        // Ward's quadrant-folded azimuth CDF (a shared measure-preserving
        // permutation of xi1, including when RGB lanes differ).
        const double x=in.ax[j]*std::cos(psi),y=in.ay[j]*std::sin(psi);
        const double a2=x*x+y*y;
        Lane& lane=lanes[j];lane.a2=a2;lane.sv=x*in.vx+y*in.vy;
        lane.sg=x*in.gx+y*in.gy;lane.weight=in.weight[j];
        AddRoots(cuts,-in.nv*a2,2*lane.sv,in.nv);
        // Resolve the grazing weight transition as cos_o crosses 4 nv:
        // 2 cos_o/(nv+cos_o) has reached 8/5 (its limiting value is 2).
        // This is a quadrature domain split, not a clamp or model change.
        AddRoots(cuts,-5*in.nv*a2,2*lane.sv,-3*in.nv);
        AddRoots(cuts,2*lane.sv*lane.sg-vg*a2,
            2*(in.nv*lane.sg+in.ng*lane.sv),2*in.nv*in.ng-vg);
        if(lane.sv<0) cuts.Add(-in.nv/lane.sv);
    }
    cuts.Sort();double value=std::exp(-4.5*4.5);
    for(int k=1;k<cuts.n;++k) {
        const double lo=cuts.v[k-1],hi=cuts.v[k],mid=(lo+hi)*.5,half=(hi-lo)*.5;
        bool active[3];int accepted=0;
        for(int j=0;j<in.count;++j) {
            const Lane& l=lanes[j];const double denom=1+l.a2*mid*mid;
            const double hv=in.nv+l.sv*mid;
            const double co=(in.nv*(1-l.a2*mid*mid)+2*l.sv*mid)/denom;
            active[j]=hv>0 && co>0 && 2*hv*(in.ng+l.sg*mid)/denom-vg>0;
            if(active[j]) ++accepted;
        }
        if(!accepted) {value+=std::exp(-lo*lo)-std::exp(-hi*hi);continue;}
        for(int kNode=0;kNode<8;++kNode) {
            const double s=mid+half*nodes8[kNode],ss=s*s;double ws=0;
            for(int j=0;j<in.count;++j) if(active[j]) {
                const Lane& l=lanes[j];
                const double co=(in.nv*(1-l.a2*ss)+2*l.sv*s)/(1+l.a2*ss);
                ws+=l.weight*2*co/(in.nv+co);
            }
            const double total=in.wD+ws;
            const double coefficient=total>NEARZERO ? in.wD/total : 0;
            value+=half*weights8[kNode]*coefficient*2*s*std::exp(-ss);
            if(nodes) ++*nodes;
        }
    }
    return value;
}
inline double Discriminant(const Input& in,double phi) {
    const double V=std::cos(phi)*in.vx+std::sin(phi)*in.vy;
    const double G=std::cos(phi)*in.gx+std::sin(phi)*in.gy;
    const double vg=in.vx*in.gx+in.vy*in.gy+in.nv*in.ng;
    const double b=in.nv*G+in.ng*V;
    return b*b-(2*V*G-vg)*(2*in.nv*in.ng-vg);
}
inline double Integrate(const Input& in,unsigned* nodes=0) {
    Cuts cuts(2*pi);cuts.Add(pi/2);cuts.Add(pi);cuts.Add(3*pi/2);
    const double az=std::atan2(in.vy,in.vx);
    const double d0=Discriminant(in,0),d90=Discriminant(in,pi/2);
    const double C=(d0+d90)/2,A=(d0-d90)/2,B=Discriminant(in,pi/4)-C;
    const double R=std::hypot(A,B),shift=std::atan2(B,A);
    const double crossLength=std::hypot(in.gx,in.gy);
    for(int j=0;j<in.count;++j) {
        const double ax=in.ax[j],ay=in.ay[j];
        for(int k=0;k<4;++k) cuts.Add(SampleAzimuth(az+k*pi/2,ax,ay));
        if(R>0 && std::abs(C)<=R) {
            const double delta=std::acos(-C/R);
            for(int sign=-1;sign<=1;sign+=2) for(int k=0;k<2;++k)
                cuts.Add(SampleAzimuth((shift+sign*delta)/2+k*pi,ax,ay));
        }
        if(crossLength>0) for(int sign=-1;sign<=1;sign+=2) {
            const double hx=in.vx-sign*in.gy/crossLength;
            const double hy=in.vy+sign*in.gx/crossLength;
            cuts.Add(SampleAzimuth(std::atan2(hy,hx),ax,ay));
        }
    }
    cuts.Sort();double result=0;
    for(int k=1;k<cuts.n;++k) {
        const double lo=cuts.v[k-1],width=cuts.v[k]-lo;
        for(int i=0;i<32;++i) {
            const double u=(nodes32[i]+1)*.5,s=std::sin(pi*u/2);
            result+=weights32[i]*Radial(in,lo+width*s*s,nodes)*width*pi/4*std::sin(pi*u);
        }
    }
    // A probability, not an energy clamp: quadrature can overshoot one
    // by roundoff for the identically-one integrand.
    return std::max(0.0,std::min(1.0,result/(2*pi)));
}
struct CacheEntry {Input key;double value;bool valid=false;};
struct Cache {CacheEntry entries[4];unsigned next=0;};
inline double Evaluate(const Input& in) {
    // Memoization of a pure function; no pointers or shared material state.
    // Every numeric dependency is in Input. Storage lasts for this thread,
    // is bounded to four entries, and only affects evaluation cost.
    static thread_local Cache cache;
    for(const auto& entry:cache.entries) if(entry.valid && Equal(entry.key,in)) return entry.value;
    const double result=Integrate(in);
    CacheEntry& entry=cache.entries[cache.next];entry.key=in;entry.value=result;entry.valid=true;
    cache.next=(cache.next+1)%4;return result;
}
}}}
#endif
