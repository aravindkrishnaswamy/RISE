#include "pch.h"
#include "HyLIoSParticleOverlap.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error HyLIoSParticleOverlap requires strict floating-point evaluation
#endif

// Clean-room implementation of Perram and Wertheim's ellipsoid contact
// function, J. Comput. Phys. 58 (1985), 409-416,
// https://doi.org/10.1016/0021-9991(85)90171-8 . No source code was ported.
namespace RISE::HyLIoS {
namespace {
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53,
    "overlap bounds require IEEE binary64");
using Status = ParticleOverlapStatus;
using Vec = std::array<double,3>;
struct Interval { double lo,hi; };
constexpr double Infinity = std::numeric_limits<double>::infinity();
double Down(double x) { return std::nextafter(x,-Infinity); }
double Up(double x) { return std::nextafter(x,Infinity); }
Interval Point(double x) { return {x,x}; }
bool Bounded(Interval x) { return std::isfinite(x.lo) && std::isfinite(x.hi); }
Interval Whole() { return {-Infinity,Infinity}; }
Interval operator+(Interval a, Interval b) {
    if (!Bounded(a) || !Bounded(b)) return Whole();
    const double lo=a.lo+b.lo,hi=a.hi+b.hi;
    if (!std::isfinite(lo) || !std::isfinite(hi)) return Whole();
    return {Down(lo),Up(hi)};
}
Interval operator-(Interval a, Interval b) {
    if (!Bounded(a) || !Bounded(b)) return Whole();
    const double lo=a.lo-b.hi,hi=a.hi-b.lo;
    if (!std::isfinite(lo) || !std::isfinite(hi)) return Whole();
    return {Down(lo),Up(hi)};
}
Interval operator*(Interval a, Interval b) {
    if (!Bounded(a) || !Bounded(b)) return Whole();
    const double p[4] = {a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};
    for (double x:p) if (!std::isfinite(x)) return Whole();
    return {Down(*std::min_element(p,p+4)),Up(*std::max_element(p,p+4))};
}
Interval DividePositive(Interval a, Interval b) {
    if (!Bounded(b) || b.lo<=0) return Whole();
    return a*Interval{Down(1.0/b.hi),Up(1.0/b.lo)};
}
using Matrix = std::array<std::array<Interval,3>,3>;
Vec Components(ParticleVector v) { return {v.x,v.y,v.z}; }
bool Finite(Vec v) { for (double x:v) if (!std::isfinite(x)) return false; return true; }
bool Valid(const ProlateParticle& p) {
    if (!std::isfinite(p.semiMinorMetres) || !std::isfinite(p.semiMajorMetres) ||
        p.semiMinorMetres<=0 || p.semiMajorMetres<p.semiMinorMetres || !Finite(Components(p.axis))) return false;
    return p.axis.x!=0 || p.axis.y!=0 || p.axis.z!=0;
}
bool Scale(Vec& v, int exponent) {
    for (double& x:v) {
        const double original=x;
        x=std::ldexp(x,-exponent);
        if (original!=0 && (std::abs(x)<0x1p-100 || !std::isfinite(x))) return false;
    }
    return true;
}
bool ScaleAxis(Vec& v) {
    int exponent=0;
    std::frexp(std::max({std::abs(v[0]),std::abs(v[1]),std::abs(v[2])}),&exponent);
    if (!Scale(v,exponent)) return false;
    for (double x:v) if (x!=0) { if (x<0) for (double& y:v) y=-y; break; }
    return true;
}
struct Shape { double a,b; Vec axis; };
Matrix ShapeMatrix(const Shape& p) {
    Interval norm=Point(0);
    for (double x:p.axis) norm=norm+Point(x)*Point(x);
    const Interval a2=Point(p.a)*Point(p.a), b2=Point(p.b)*Point(p.b);
    Matrix q;
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) {
        q[i][j]=DividePositive((b2-a2)*Point(p.axis[i])*Point(p.axis[j]),norm);
        if (i==j) q[i][j]=q[i][j]+a2;
    }
    return q;
}
// Error-free expansion arithmetic below is only used for degree <=4 controls.
// Common scaling puts all nonzero inputs in [2^-100,1): even a degree-four
// product's lowest possible binary bit is >=2^-612, far above underflow.
// TwoSum and FMA TwoProduct therefore retain every bit; no error band is used.
using Expansion=std::vector<double>;
void Add(Expansion& e, double b) {
    Expansion result;
    result.reserve(e.size()+1);
    double q=b;
    for (double x:e) {
        const double sum=q+x, bv=sum-q;
        const double error=(q-(sum-bv))+(x-bv);
        if (error!=0) result.push_back(error);
        q=sum;
    }
    if (q!=0 || result.empty()) result.push_back(q);
    e.swap(result);
}
Expansion Multiply(const Expansion& a, const Expansion& b) {
    Expansion result;
    for (double x:a) for (double y:b) {
        const double product=x*y;
        Add(result,std::fma(x,y,-product));
        Add(result,product);
    }
    return result;
}
void PolynomialTerm(Expansion& sum, double sign, std::initializer_list<double> factors) {
    Expansion product{sign};
    for (double factor:factors) product=Multiply(product,Expansion{factor});
    for (double x:product) Add(sum,x);
}
Status Sign(const Expansion& e) {
    for (auto i=e.rbegin();i!=e.rend();++i) if (*i!=0) return *i<0 ? Status::Overlap : Status::Disjoint;
    return Status::Touching;
}
Status Sphere(const Shape& a,const Shape& b,Vec r) {
    Expansion difference;
    for (double x:r) PolynomialTerm(difference,1,{x,x});
    PolynomialTerm(difference,-1,{a.a,a.a});
    PolynomialTerm(difference,-2,{a.a,b.a});
    PolynomialTerm(difference,-1,{b.a,b.a});
    return Sign(difference);
}
int CoordinateAxis(Vec v) {
    int index=-1;
    for (int i=0;i<3;++i) if (v[i]!=0) { if (index!=-1) return -1; index=i; }
    return index;
}
Status EqualAligned(const Shape& p,Vec r,int major) {
    Expansion difference;
    for (int i=0;i<3;++i) {
        const double multiplier=i==major ? p.a:p.b;
        PolynomialTerm(difference,1,{r[i],r[i],multiplier,multiplier});
    }
    PolynomialTerm(difference,-4,{p.a,p.a,p.b,p.b});
    return Sign(difference);
}
struct Evaluation { Interval value,derivative; bool valid; };
Evaluation Evaluate(const Matrix& a,const Matrix& b,Vec r,double t) {
    Matrix c,d;
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) {
        d[i][j]=b[i][j]-a[i][j];
        c[i][j]=(Point(1)-Point(t))*a[i][j]+Point(t)*b[i][j];
    }
    Matrix adj;
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) {
        // Cyclic ordering gives the correctly signed 2x2 cofactor of (j,i).
        const unsigned r0=(j+1)%3,r1=(j+2)%3,c0=(i+1)%3,c1=(i+2)%3;
        adj[i][j]=c[r0][c0]*c[r1][c1]-c[r0][c1]*c[r1][c0];
    }
    Interval determinant=Point(0);
    for (unsigned i=0;i<3;++i) determinant=determinant+c[0][i]*adj[i][0];
    if (!(determinant.lo>0) || !std::isfinite(determinant.hi)) return {{0,0},{0,0},false};
    std::array<Interval,3> x;
    for (unsigned i=0;i<3;++i) {
        Interval numerator=Point(0);
        for (unsigned j=0;j<3;++j) numerator=numerator+adj[i][j]*Point(r[j]);
        x[i]=DividePositive(numerator,determinant);
    }
    Interval h=Point(0),hNegativeDerivative=Point(0);
    for (unsigned i=0;i<3;++i) {
        h=h+Point(r[i])*x[i];
        for (unsigned j=0;j<3;++j) hNegativeDerivative=hNegativeDerivative+x[i]*d[i][j]*x[j];
    }
    const Interval weight=Point(t)*(Point(1)-Point(t));
    const Interval value=weight*h;
    const Interval derivative=(Point(1)-Point(2)*Point(t))*h-weight*hNegativeDerivative;
    return {value,derivative,std::isfinite(value.lo) && std::isfinite(value.hi) &&
        std::isfinite(derivative.lo) && std::isfinite(derivative.hi)};
}
}
ParticleOverlapResult ClassifyProlateOverlap(const ProlateParticle& first,
    const ProlateParticle& second,ParticleVector relativeCentre) {
    Vec r=Components(relativeCentre);
    if (!Valid(first) || !Valid(second) || !Finite(r)) return {Status::InvalidInput,0};
    // Also reject FTZ/DAZ modes that invalidate outward nextafter intervals.
    volatile double minimumNormal=std::numeric_limits<double>::min();
    volatile double half=0.5;
    volatile double minimumSubnormal=std::numeric_limits<double>::denorm_min();
    volatile double lift=0x1p+1023;
    if (std::fegetround()!=FE_TONEAREST || minimumNormal*half==0 || minimumSubnormal*lift==0)
        return {Status::UnsupportedRange,0};
    Shape a{first.semiMinorMetres,first.semiMajorMetres,Components(first.axis)};
    Shape b{second.semiMinorMetres,second.semiMajorMetres,Components(second.axis)};
    if (!ScaleAxis(a.axis) || !ScaleAxis(b.axis)) return {Status::UnsupportedRange,0};
    int exponent=0;
    std::frexp(std::max({a.b,b.b,std::abs(r[0]),std::abs(r[1]),std::abs(r[2])}),&exponent);
    Vec lengths{a.a,a.b,b.a};
    Vec last{b.b,0,0};
    if (!Scale(lengths,exponent) || !Scale(last,exponent) || !Scale(r,exponent)) return {Status::UnsupportedRange,0};
    a.a=lengths[0]; a.b=lengths[1]; b.a=lengths[2]; b.b=last[0];
    // Canonical argument/sign ordering makes swap symmetry include unresolved
    // results and work counts, not just mathematical exact-arithmetic verdicts.
    if (std::tie(b.a,b.b,b.axis)<std::tie(a.a,a.b,a.axis)) std::swap(a,b);
    for (double x:r) if (x!=0) { if (x<0) for (double& y:r) y=-y; break; }
    if (a.a==a.b && b.a==b.b) return {Sphere(a,b,r),0};
    const int major=CoordinateAxis(a.axis);
    if (a.a==b.a && a.b==b.b && major>=0 && CoordinateAxis(b.axis)==major)
        return {EqualAligned(a,r,major),0};
    if (r[0]==0 && r[1]==0 && r[2]==0) return {Status::Overlap,0};
    const Matrix qa=ShapeMatrix(a),qb=ShapeMatrix(b);
    double lower=0,upper=1;
    for (unsigned iteration=1;iteration<=64;++iteration) {
        const double t=lower+(upper-lower)*0.5;
        if (t==lower || t==upper) return {Status::Indeterminate,iteration-1};
        const Evaluation f=Evaluate(qa,qb,r,t);
        if (!f.valid) return {Status::Indeterminate,iteration};
        if (f.value.lo>1) return {Status::Disjoint,iteration};
        // f is concave: it is the minimum over position of an affine mixture
        // of the two ellipsoidal quadratic forms. Its tangent bounds its max.
        const Interval left=(Point(lower)-Point(t))*f.derivative;
        const Interval right=(Point(upper)-Point(t))*f.derivative;
        const double maximumUpper=Up(f.value.hi+std::max(left.hi,right.hi));
        if (maximumUpper<1) return {Status::Overlap,iteration};
        if (f.derivative.lo>0) lower=t;
        else if (f.derivative.hi<0) upper=t;
        else return {Status::Indeterminate,iteration};
    }
    return {Status::Indeterminate,64};
}
}
