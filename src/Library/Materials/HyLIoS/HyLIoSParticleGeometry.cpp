#include "pch.h"
#include "HyLIoSParticleGeometry.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cfenv>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <stdexcept>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "HyLIoS particle geometry requires strict floating-point arithmetic"
#endif

namespace RISE::HyLIoS {
namespace {
constexpr double Pi = 3.141592653589793238462643383279502884;
void RequireFloatingPointEnvironment() {
	if (std::fegetround()!=FE_TONEAREST) throw std::runtime_error("particle geometry requires round-to-nearest");
	volatile double normal=std::numeric_limits<double>::min();
	volatile double subnormal=std::numeric_limits<double>::denorm_min();
	volatile double half=0.5,two=2.0;
	if (normal*half==0.0 || subnormal*two==0.0)
		throw std::runtime_error("particle geometry requires gradual underflow");
}
void Finite(double value) {
	if (!std::isfinite(value)) throw std::invalid_argument("particle geometry requires finite inputs");
}
void Finite(ParticleVector v) { Finite(v.x); Finite(v.y); Finite(v.z); }
void Uniform(double value) {
	Finite(value);
	if (value < 0.0 || value >= 1.0) throw std::invalid_argument("particle uniform must lie in [0,1)");
}
double ProductRatio(std::initializer_list<double> numerator, std::initializer_list<double> denominator, bool allowUnderflow = false) {
	double fraction = 1.0;
	int exponent = 0;
	for (double value : numerator) {
		if (value == 0.0) return 0.0;
		int power = 0;
		fraction *= std::frexp(value,&power); exponent += power;
		fraction = std::frexp(fraction,&power); exponent += power;
	}
	for (double value : denominator) {
		int power = 0;
		fraction /= std::frexp(value,&power); exponent -= power;
		fraction = std::frexp(fraction,&power); exponent += power;
	}
	const double result = std::ldexp(fraction,exponent);
	if (!std::isfinite(result)) throw std::overflow_error("particle geometry result overflowed");
	if (result == 0.0 && !allowUnderflow) throw std::underflow_error("particle geometry positive result underflowed");
	return result;
}
ParticleVector Scale(ParticleVector v, double s) { return {v.x*s,v.y*s,v.z*s}; }
ParticleVector Unit(ParticleVector v) {
	Finite(v);
	const double scale = std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)});
	if (scale == 0.0) throw std::invalid_argument("particle axis/direction must be nonzero");
	int exponent = 0;
	std::frexp(scale,&exponent);
	// Put the largest component in [1,2). Normalization then only reduces
	// components; a representable subnormal cannot vanish in an unnecessary
	// preliminary division by two and need restoring by the final norm.
	--exponent;
	v = {std::ldexp(v.x,-exponent),std::ldexp(v.y,-exponent),std::ldexp(v.z,-exponent)};
	return Scale(v,1.0/std::hypot(v.x,v.y,v.z));
}
double DifferenceOfProducts(double a, double b, double c, double d) {
	const double cd = c*d;
	return std::fma(a,b,-cd)+std::fma(-c,d,cd);
}
struct SumPair { double head,tail; };
SumPair TwoSum(double a, double b) {
	const double head=a+b;
	if (!std::isfinite(head)) throw std::overflow_error("particle compensated sum exceeds binary64 range");
	const double second=head-a;
	return {head,(a-(head-second))+(b-second)};
}
// Fixed-size expansion for three products, their FMA residuals, and an
// optional constant. Its tail stays separate until cancellation is complete.
template<std::size_t Count> double SumComponents(double (&terms)[Count]) {
	static_assert(Count==6 || Count==7 || Count==8,"bounded particle arithmetic expansion");
	std::sort(terms,terms+Count,[](double a,double b){ return std::abs(a)>std::abs(b); });
	for (std::size_t i=1; i<Count; ++i) if (std::signbit(terms[i]) != std::signbit(terms[0])) {
		std::swap(terms[1],terms[i]); break;
	}
	double components[Count]={};
	std::size_t count=0;
	for (double term : terms) {
		double next[Count]={};
		std::size_t nextCount=0;
		for (std::size_t i=0; i<count; ++i) {
			const SumPair sum=TwoSum(term,components[i]);
			if (sum.tail != 0.0) next[nextCount++]=sum.tail;
			term=sum.head;
		}
		if (term != 0.0 || nextCount==0) next[nextCount++]=term;
		std::copy(next,next+nextCount,components);
		count=nextCount;
	}
	double result=0.0;
	for (std::size_t i=0; i<count; ++i) result+=components[i];
	if (!std::isfinite(result)) throw std::overflow_error("particle compensated dot exceeds binary64 range");
	return result;
}
// An exponent is part of the value until the final representable result.
// In particular, a coefficient smaller than denorm_min may become a finite
// normal component after cancellation of a semiaxis in the gradient.
struct Extended { double fraction; int exponent; };
Extended Canonical(double fraction, int exponent) {
	int adjustment = 0;
	fraction = std::frexp(fraction,&adjustment);
	return {fraction,exponent+adjustment};
}
Extended ExtendedProduct(std::initializer_list<double> numerator, std::initializer_list<double> denominator) {
	double fraction = 1.0;
	int exponent = 0;
	for (double value : numerator) {
		if (value == 0.0) return {0.0,0};
		int power = 0;
		fraction *= std::frexp(value,&power); exponent += power;
		fraction = std::frexp(fraction,&power); exponent += power;
	}
	for (double value : denominator) {
		int power = 0;
		fraction /= std::frexp(value,&power); exponent -= power;
		fraction = std::frexp(fraction,&power); exponent += power;
	}
	return {fraction,exponent};
}
Extended ExtendedAdd(Extended a, Extended b) {
	if (a.fraction == 0.0) return b;
	if (b.fraction == 0.0) return a;
	if (a.exponent < b.exponent) std::swap(a,b);
	return Canonical(a.fraction+std::ldexp(b.fraction,b.exponent-a.exponent),a.exponent);
}
Extended ExtendedMultiply(Extended a, double value) {
	const Extended factor = ExtendedProduct({value},{});
	return Canonical(a.fraction*factor.fraction,a.exponent+factor.exponent);
}
Extended ExtendedMultiply(Extended a, Extended b) {
	return Canonical(a.fraction*b.fraction,a.exponent+b.exponent);
}
Extended ExtendedSqrt(Extended value) {
	if (value.exponent%2 != 0) { value.fraction*=2.0; --value.exponent; }
	return Canonical(std::sqrt(value.fraction),value.exponent/2);
}
Extended ExtendedDivide(Extended a, Extended b) {
	return Canonical(a.fraction/b.fraction,a.exponent-b.exponent);
}
double Rounded(Extended value) {
	const double result = std::ldexp(value.fraction,value.exponent);
	if (!std::isfinite(result)) throw std::overflow_error("particle projection exceeds binary64 range");
	if (value.fraction != 0.0 && result == 0.0)
		throw std::underflow_error("particle projected displacement cannot be resolved in binary64");
	return result;
}
Extended ExtendedDifferenceOfProducts(double a, double b, double c, double d) {
	if (a == 0.0 || b == 0.0) return ExtendedProduct({-c,d},{});
	if (c == 0.0 || d == 0.0) return ExtendedProduct({a,b},{});
	int ae=0,be=0,ce=0,de=0;
	const double af=std::frexp(a,&ae),bf=std::frexp(b,&be),cf=std::frexp(c,&ce),df=std::frexp(d,&de);
	const int exponent = std::max(ae+be,ce+de);
	// A common power-of-two scaling preserves exact raw-line collinearity.
	// Compensated products retain the residual when the two products round
	// alike; normalized direction/basis rounding never multiplies the origin.
	return Canonical(DifferenceOfProducts(std::ldexp(af,ae+be-exponent),bf,
		std::ldexp(cf,ce+de-exponent),df),exponent);
}
struct ExtendedSumPair { Extended head,tail; };
ExtendedSumPair ExtendedTwoSum(Extended a, Extended b) {
	if (a.fraction==0.0) return {b,{0.0,0}};
	if (b.fraction==0.0) return {a,{0.0,0}};
	if (a.exponent<b.exponent) std::swap(a,b);
	// A sufficiently small addend is an exact residual, not a value to
	// round away while aligning exponents before a later cancellation.
	if (a.exponent-b.exponent>std::numeric_limits<double>::digits+1) return {a,b};
	const SumPair sum=TwoSum(a.fraction,std::ldexp(b.fraction,b.exponent-a.exponent));
	return {Canonical(sum.head,a.exponent),Canonical(sum.tail,a.exponent)};
}
Extended ExtendedDot(Extended a, Extended b, Extended c, ParticleVector direction) {
	Extended terms[3]={ExtendedMultiply(a,direction.x),ExtendedMultiply(b,direction.y),ExtendedMultiply(c,direction.z)};
	std::sort(terms,terms+3,[](Extended x,Extended y){
		if (x.fraction==0.0) return false;
		if (y.fraction==0.0) return true;
		return x.exponent!=y.exponent ? x.exponent>y.exponent : std::abs(x.fraction)>std::abs(y.fraction);
	});
	if (std::signbit(terms[0].fraction)==std::signbit(terms[1].fraction) &&
		std::signbit(terms[0].fraction)!=std::signbit(terms[2].fraction)) std::swap(terms[1],terms[2]);
	const ExtendedSumPair first=ExtendedTwoSum(terms[0],terms[1]);
	const ExtendedSumPair second=ExtendedTwoSum(first.head,terms[2]);
	const ExtendedSumPair tails=ExtendedTwoSum(first.tail,second.tail);
	const ExtendedSumPair result=ExtendedTwoSum(second.head,tails.head);
	return ExtendedAdd(result.head,ExtendedAdd(result.tail,tails.tail));
}
Extended DirectionLength(ParticleVector direction) {
	const double largest=std::max({std::abs(direction.x),std::abs(direction.y),std::abs(direction.z)});
	int exponent=0;
	std::frexp(largest,&exponent); --exponent;
	const ParticleVector scaled{std::ldexp(direction.x,-exponent),std::ldexp(direction.y,-exponent),std::ldexp(direction.z,-exponent)};
	return Canonical(std::hypot(scaled.x,scaled.y,scaled.z),exponent);
}
ParticleVector Cross(ParticleVector a, ParticleVector b) {
	return {DifferenceOfProducts(a.y,b.z,a.z,b.y),DifferenceOfProducts(a.z,b.x,a.x,b.z),
		DifferenceOfProducts(a.x,b.y,a.y,b.x)};
}
double Dot(ParticleVector a, ParticleVector b) {
	const double x=a.x*b.x,y=a.y*b.y,z=a.z*b.z;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
		throw std::overflow_error("particle dot product exceeds binary64 range");
	double terms[6]={x,y,z,std::fma(a.x,b.x,-x),std::fma(a.y,b.y,-y),std::fma(a.z,b.z,-z)};
	return SumComponents(terms);
}
ParticleVector Combine(ParticleVector a, double x, ParticleVector b, double y, ParticleVector c, double z) {
	const ParticleVector result{Dot({x,y,z},{a.x,b.x,c.x}),Dot({x,y,z},{a.y,b.y,c.y}),Dot({x,y,z},{a.z,b.z,c.z})};
	return result;
}
// Outward-rounded, finite intervals are a support check, not a replacement
// transport representation. Unsupported exponent ranges fail explicitly.
struct Bound { double low,high; };
using BoundVector=std::array<Bound,3>;
Bound Point(double x) { return {x,x}; }
Bound Outward(double low,double high) {
	low=std::nextafter(low,-std::numeric_limits<double>::infinity());
	high=std::nextafter(high,std::numeric_limits<double>::infinity());
	if (!std::isfinite(low) || !std::isfinite(high)) throw std::range_error("particle root uncertainty exceeds supported exponent range");
	return {low,high};
}
Bound AddBound(Bound a,Bound b) {
	if (a.low==0.0 && a.high==0.0) return b;
	if (b.low==0.0 && b.high==0.0) return a;
	if (a.low==a.high && b.low==b.high) {
		const SumPair sum=TwoSum(a.low,b.low);
		if (sum.tail==0.0) return Point(sum.head);
	}
	return Outward(a.low+b.low,a.high+b.high);
}
Bound NegateBound(Bound a) { return {-a.high,-a.low}; }
// If the exact product quantum is no smaller than denorm_min, a zero
// FMA residual certifies exactness. Otherwise residual underflow can conceal it.
bool ProductResidualRepresentable(double a,double b) {
	if (a==0.0 || b==0.0) return true;
	int ae=0,be=0; std::frexp(a,&ae); std::frexp(b,&be);
	return ae+be>=std::numeric_limits<double>::min_exponent+std::numeric_limits<double>::digits;
}
Bound MultiplyBound(Bound a,Bound b) {
	if ((a.low==0.0 && a.high==0.0) || (b.low==0.0 && b.high==0.0)) return Point(0.0);
	const double values[4]={a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
	for (double value:values) if (!std::isfinite(value)) throw std::range_error("particle uncertainty product exceeds supported exponent range");
	if (a.low==a.high && b.low==b.high && ProductResidualRepresentable(a.low,b.low) && std::abs(values[0])>=std::numeric_limits<double>::min() &&
		std::fma(a.low,b.low,-values[0])==0.0) return Point(values[0]);
	return Outward(*std::min_element(values,values+4),*std::max_element(values,values+4));
}
Bound DivideBound(Bound a,Bound b) {
	if (b.low<=0.0 && b.high>=0.0) throw std::range_error("particle uncertainty divisor includes zero");
	if (a.low==0.0 && a.high==0.0) return Point(0.0);
	const double values[4]={a.low/b.low,a.low/b.high,a.high/b.low,a.high/b.high};
	for (double value:values) if (!std::isfinite(value)) throw std::range_error("particle uncertainty quotient exceeds supported exponent range");
	if (a.low==a.high && b.low==b.high && ProductResidualRepresentable(values[0],b.low) && std::abs(values[0])>=std::numeric_limits<double>::min() &&
		std::fma(values[0],b.low,-a.low)==0.0) return Point(values[0]);
	return Outward(*std::min_element(values,values+4),*std::max_element(values,values+4));
}
Bound SqrtBound(Bound a) {
	if (a.low<0.0) throw std::range_error("particle discriminant uncertainty includes negative values");
	const double low=std::sqrt(a.low),high=std::sqrt(a.high);
	if (a.low==a.high && (low==0.0 || (ProductResidualRepresentable(low,low) && std::abs(a.low)>=std::numeric_limits<double>::min() && std::fma(low,low,-a.low)==0.0))) return Point(low);
	return Outward(low,high);
}
BoundVector PointVector(ParticleVector v) { return {Point(v.x),Point(v.y),Point(v.z)}; }
Bound DotBound(const BoundVector& a,const BoundVector& b) {
	return AddBound(AddBound(MultiplyBound(a[0],b[0]),MultiplyBound(a[1],b[1])),MultiplyBound(a[2],b[2]));
}
BoundVector CrossBound(const BoundVector& a,const BoundVector& b) {
	return {AddBound(MultiplyBound(a[1],b[2]),NegateBound(MultiplyBound(a[2],b[1]))),
		AddBound(MultiplyBound(a[2],b[0]),NegateBound(MultiplyBound(a[0],b[2]))),
		AddBound(MultiplyBound(a[0],b[1]),NegateBound(MultiplyBound(a[1],b[0])))};
}
BoundVector UnitBound(BoundVector v) {
	const Bound norm=SqrtBound(DotBound(v,v));
	for (Bound& component:v) component=DivideBound(component,norm);
	return v;
}
BoundVector AxisBound(ParticleVector axis) {
	const double largest=std::max({std::abs(axis.x),std::abs(axis.y),std::abs(axis.z)});
	int exponent=0; std::frexp(largest,&exponent); --exponent;
	const double raw[3]={axis.x,axis.y,axis.z};
	BoundVector result;
	for (std::size_t i=0;i<3;++i) {
		const double scaled=std::ldexp(raw[i],-exponent);
		result[i]=raw[i]!=0.0 && std::abs(scaled)<std::numeric_limits<double>::min() ? Outward(scaled,scaled) : Point(scaled);
	}
	return UnitBound(result);
}
struct Projection {
	double a,b,c,s,radius,axialCoefficient;
	ParticleVector direction,e,f,radial,axis;
};
Projection Project(const ProlateParticle& particle, ParticleVector direction) {
	RequireFloatingPointEnvironment();
	Finite(particle.semiMinorMetres); Finite(particle.semiMajorMetres);
	if (particle.semiMinorMetres <= 0.0 || particle.semiMajorMetres < particle.semiMinorMetres)
		throw std::invalid_argument("particle requires positive prolate semi-axes");
	const ParticleVector axis = Unit(particle.axis);
	const ParticleVector d = Unit(direction);
	const ParticleVector perpendicular = Cross(d,axis);
	// libc++'s three-component hypot can erase an all-subnormal vector
	// before scaling it. The two-component library primitive retains its range.
	const double sine = std::hypot(std::hypot(perpendicular.x,perpendicular.y),perpendicular.z);
	const double cosine = Dot(axis,d);
	const double angularNorm = std::hypot(cosine,sine);
	const double c = cosine/angularNorm, s = sine/angularNorm;
	ParticleVector f;
	if (sine != 0.0) f = Unit(perpendicular);
	else {
		// Select the coordinate least aligned with the direction. The cross
		// product is well conditioned even at poles; no near-parallel epsilon.
		const double x = std::abs(d.x), y = std::abs(d.y), z = std::abs(d.z);
		const ParticleVector helper = x <= y && x <= z ? ParticleVector{1,0,0} :
			(y <= z ? ParticleVector{0,1,0} : ParticleVector{0,0,1});
		f = Unit(Cross(d,helper));
	}
	const ParticleVector e = Unit(Cross(f,d));
	const double a = particle.semiMinorMetres, b = particle.semiMajorMetres;
	const double radius = std::hypot(a*c,b*s);
	// B is bounded and never smaller than s; unlike a*c/radius it does
	// not lose a coefficient needed after division by a microscopic axis.
	const double B = s == 0.0 ? 0.0 : ProductRatio({b,s},{radius});
	// Retain the authored axis directly. Reconstructing it (or its radial
	// partner) from rounded c/s can inject a spurious component at a pole;
	// huge aspect ratios then make that basis error visible in the normal.
	return {a,b,c,s,radius,B,d,e,f,Unit(Cross(f,axis)),axis};
}
ParticleVector NormalizeGradient(const Projection& p, Extended radial, Extended side, Extended axial) {
	const Extended gradients[3]={radial,side,axial};
	int maximum=-10000;
	for (const Extended value : gradients) if (value.fraction != 0.0) maximum=std::max(maximum,value.exponent);
	ParticleVector local{std::ldexp(radial.fraction,radial.exponent-maximum+1),
		std::ldexp(side.fraction,side.exponent-maximum+1),std::ldexp(axial.fraction,axial.exponent-maximum+1)};
	local=Unit(local);
	return Unit(Combine(p.radial,local.x,p.f,local.y,p.axis,local.z));
}
ParticleVector Normal(const Projection& p, double X, double Y, double w, double sign) {
	// Differentiate the ellipsoid before rounding its unit-sphere coordinates:
	// (A*X +/- B*w)/a = c*X/L +/- b*s*w/(a*L), and likewise for the
	// axial term. Canceling a in A/a is essential when A itself underflows.
	const Extended radial=ExtendedAdd(ExtendedProduct({p.c,X},{p.radius}),
		ExtendedProduct({sign,p.b,p.s,w},{p.a,p.radius}));
	const Extended side=ExtendedProduct({Y},{p.a});
	const Extended axial=ExtendedAdd(ExtendedProduct({p.s,X},{p.radius}),
		ExtendedProduct({-sign,p.a,p.c,w},{p.b,p.radius}));
	return NormalizeGradient(p,radial,side,axial);
}
struct ImplicitFrame {
	ParticleVector radial,side,axis;
	BoundVector radialBound,sideBound,axisBound;
};
ImplicitFrame MakeImplicitFrame(const Projection& p,ParticleVector rawAxis) {
	if (p.a==p.b) return {{1,0,0},{0,1,0},{0,0,1},PointVector({1,0,0}),PointVector({0,1,0}),PointVector({0,0,1})};
	const double x=std::abs(p.axis.x),y=std::abs(p.axis.y),z=std::abs(p.axis.z);
	const ParticleVector helper=x<=y && x<=z ? ParticleVector{1,0,0} : (y<=z ? ParticleVector{0,1,0} : ParticleVector{0,0,1});
	const ParticleVector radial=Unit(Cross(helper,p.axis)),side=Unit(Cross(p.axis,radial));
	const BoundVector axisBound=AxisBound(rawAxis);
	const BoundVector radialBound=UnitBound(CrossBound(PointVector(helper),axisBound));
	return {radial,side,p.axis,radialBound,UnitBound(CrossBound(axisBound,radialBound)),axisBound};
}
ParticleVector InFrame(ParticleVector v,const ImplicitFrame& frame) {
	return {Dot(v,frame.radial),Dot(v,frame.side),Dot(v,frame.axis)};
}
BoundVector InFrameBound(const BoundVector& v,const ImplicitFrame& frame) {
	return {DotBound(v,frame.radialBound),DotBound(v,frame.sideBound),DotBound(v,frame.axisBound)};
}
ParticleVector ImplicitEndpointNormal(const Projection& p,const ImplicitFrame& frame,ParticleVector point) {
	const ParticleVector local=InFrame(point,frame);
	const Extended gradients[3]={ExtendedProduct({local.x},{p.a,p.a}),ExtendedProduct({local.y},{p.a,p.a}),ExtendedProduct({local.z},{p.b,p.b})};
	int maximum=-10000;
	for (Extended value:gradients) if (value.fraction!=0.0) maximum=std::max(maximum,value.exponent);
	const ParticleVector normalized=Unit({std::ldexp(gradients[0].fraction,gradients[0].exponent-maximum+1),
		std::ldexp(gradients[1].fraction,gradients[1].exponent-maximum+1),std::ldexp(gradients[2].fraction,gradients[2].exponent-maximum+1)});
	return Unit(Combine(frame.radial,normalized.x,frame.side,normalized.y,frame.axis,normalized.z));
}
void CheckEndpointUncertainty(const Projection& p,const ImplicitFrame& frame,ParticleVector origin,ParticleVector direction,
	ParticleVector point,Bound root) {
	const BoundVector o=PointVector(origin),d=PointVector(direction);
	BoundVector endpoint;
	for (std::size_t i=0;i<3;++i) endpoint[i]=AddBound(o[i],MultiplyBound(d[i],root));
	const BoundVector localBound=InFrameBound(endpoint,frame);
	const ParticleVector local=InFrame(point,frame);
	const double coordinates[3]={local.x,local.y,local.z},axes[3]={p.a,p.a,p.b};
	double gradients[3],errors[3];
	for (std::size_t i=0;i<3;++i) {
		const Bound denominator=MultiplyBound(Point(axes[i]),Point(axes[i]));
		const Bound gradient=DivideBound(localBound[i],denominator);
		gradients[i]=coordinates[i]/(axes[i]*axes[i]);
		errors[i]=std::max(std::abs(gradient.low-gradients[i]),std::abs(gradient.high-gradients[i]));
	}
	const double magnitude=std::hypot(std::hypot(gradients[0],gradients[1]),gradients[2]);
	const double uncertainty=std::hypot(std::hypot(errors[0],errors[1]),errors[2]);
	if (!std::isfinite(magnitude) || !std::isfinite(uncertainty) || uncertainty>=magnitude)
		throw std::range_error("particle endpoint uncertainty dominates its surface gradient");
}
// Only on/inside origins use this branch. C<=0 makes the discriminant
// B*B-A*C a sum of nonnegative terms; external distant origins instead use
// the perpendicular silhouette, where that subtraction would lose the radius.
bool TryInsideChord(const Projection& p, ParticleVector rawAxis,ParticleVector origin, ParticleVector direction,
	std::optional<ParticleChord>& result) {
	if (p.a==p.b && ((std::abs(origin.x)==p.a && (origin.y!=0.0 || origin.z!=0.0)) ||
		(std::abs(origin.y)==p.a && (origin.x!=0.0 || origin.z!=0.0)) ||
		(std::abs(origin.z)==p.a && (origin.x!=0.0 || origin.y!=0.0)))) return false;
	const ImplicitFrame frame=MakeImplicitFrame(p,rawAxis);
	const ParticleVector localOrigin=InFrame(origin,frame);
	if (std::abs(localOrigin.x)>p.a || std::abs(localOrigin.y)>p.a || std::abs(localOrigin.z)>p.b) return false;
	const ParticleVector q{ProductRatio({localOrigin.x},{p.a}),ProductRatio({localOrigin.y},{p.a}),ProductRatio({localOrigin.z},{p.b})};
	// A unit coordinate already exhausts the implicit surface budget; any
	// other nonzero coordinate is strictly outside, even if its square would
	// underflow before entering the compensated expansion.
	if (p.a!=p.b && ((std::abs(q.x)==1.0 && (q.y!=0.0 || q.z!=0.0)) ||
		(std::abs(q.y)==1.0 && (q.x!=0.0 || q.z!=0.0)) ||
		(std::abs(q.z)==1.0 && (q.x!=0.0 || q.y!=0.0)))) return false;
	const double rr=p.a*p.a,xx=localOrigin.x*localOrigin.x,yy=localOrigin.y*localOrigin.y;
	if (!std::isfinite(rr) || rr==0.0 || !std::isfinite(xx) || !std::isfinite(yy) ||
		!ProductResidualRepresentable(p.a,p.a) || !ProductResidualRepresentable(localOrigin.x,localOrigin.x) ||
		!ProductResidualRepresentable(localOrigin.y,localOrigin.y))
		throw std::range_error("particle raw implicit metric exceeds supported exact-residual range");
	double equatorialTerms[6]={xx,yy,-rr,std::fma(localOrigin.x,localOrigin.x,-xx),
		std::fma(localOrigin.y,localOrigin.y,-yy),-std::fma(p.a,p.a,-rr)};
	const double equatorial=SumComponents(equatorialTerms);
	double constant=Rounded(ExtendedAdd(ExtendedProduct({equatorial},{p.a,p.a}),
		ExtendedProduct({localOrigin.z,localOrigin.z},{p.b,p.b})));
	bool exactFrame=true;
	for (std::size_t i=0;i<3;++i) exactFrame=exactFrame && frame.radialBound[i].low==frame.radialBound[i].high &&
		frame.sideBound[i].low==frame.sideBound[i].high && frame.axisBound[i].low==frame.axisBound[i].high;
	bool certifiedZero=constant==0.0 && exactFrame && equatorial==0.0 && localOrigin.z==0.0;
	if (p.a==p.b) {
		const double ox=origin.x*origin.x,oy=origin.y*origin.y,oz=origin.z*origin.z;
		if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) ||
			!ProductResidualRepresentable(origin.x,origin.x) || !ProductResidualRepresentable(origin.y,origin.y) ||
			!ProductResidualRepresentable(origin.z,origin.z)) throw std::range_error("sphere raw metric exceeds supported exact-residual range");
		double sphereTerms[8]={ox,oy,oz,-rr,std::fma(origin.x,origin.x,-ox),std::fma(origin.y,origin.y,-oy),std::fma(origin.z,origin.z,-oz),-std::fma(p.a,p.a,-rr)};
		const double residual=SumComponents(sphereTerms);
		constant=residual/(p.a*p.a);
		certifiedZero=residual==0.0;
	}
	if (constant>0.0) return false;
	const ParticleVector localDirection=InFrame(direction,frame);
	Extended A=ExtendedDot(
		ExtendedProduct({localDirection.x,localDirection.x},{p.a,p.a}),
		ExtendedProduct({localDirection.y,localDirection.y},{p.a,p.a}),
		ExtendedProduct({localDirection.z,localDirection.z},{p.b,p.b}),{1,1,1});
	const double equatorialDot=Dot({localOrigin.x,localOrigin.y,0},{localDirection.x,localDirection.y,0});
	Extended B=ExtendedAdd(ExtendedProduct({equatorialDot},{p.a,p.a}),
		ExtendedProduct({localOrigin.z,localDirection.z},{p.b,p.b}));
	bool certifiedBZero=exactFrame && equatorialDot==0.0 && (localOrigin.z==0.0 || localDirection.z==0.0) &&
		ProductResidualRepresentable(localOrigin.x,localDirection.x) && ProductResidualRepresentable(localOrigin.y,localDirection.y);
	if (p.a==p.b) {
		A=ExtendedProduct({Dot(direction,direction)},{p.a,p.a});
		const double rawB=Dot(origin,direction);
		B=ExtendedProduct({rawB},{p.a,p.a});
		certifiedBZero=rawB==0.0 && ProductResidualRepresentable(origin.x,direction.x) && ProductResidualRepresentable(origin.y,direction.y) && ProductResidualRepresentable(origin.z,direction.z);
	}
	const BoundVector boundedOrigin=InFrameBound(PointVector(origin),frame),boundedDirection=InFrameBound(PointVector(direction),frame);
	BoundVector boundedQ,boundedV;
	const double axes[3]={p.a,p.a,p.b};
	for (std::size_t i=0;i<3;++i) {
		boundedQ[i]=DivideBound(boundedOrigin[i],Point(axes[i]));
		boundedV[i]=DivideBound(boundedDirection[i],Point(axes[i]));
	}
	const Bound boundedA=DotBound(boundedV,boundedV);
	const Bound boundedB=certifiedBZero ? Point(0.0) : DotBound(boundedQ,boundedV);
	const Bound boundedC=certifiedZero ? Point(0.0) : AddBound(DotBound(boundedQ,boundedQ),Point(-1.0));
	if (constant==0.0 && p.a!=p.b && (boundedC.low!=0.0 || boundedC.high!=0.0))
		throw std::range_error("particle surface root is not certified by its implicit enclosure");
	Extended first{0.0,0},second{0.0,0};
	if (constant==0.0) {
		// Deflate the known surface root. No range test or ray-origin epsilon
		// may move that exact zero to the wrong side of the boundary.
		first=ExtendedDivide(ExtendedMultiply(B,-2.0),A);
	} else {
		const Extended discriminant=ExtendedAdd(ExtendedMultiply(B,B),ExtendedMultiply(A,-constant));
		const Extended root=ExtendedSqrt(discriminant);
		const double sign=B.fraction<0.0 ? -1.0 : 1.0;
		const Extended numerator=ExtendedAdd(ExtendedMultiply(B,-1.0),ExtendedMultiply(root,-sign));
		first=ExtendedDivide(numerator,A);
		second=ExtendedDivide(ExtendedProduct({constant},{}),numerator);
	}
	double entryParameter=Rounded(first),exitParameter=Rounded(second);
	if (entryParameter>exitParameter) { std::swap(first,second); std::swap(entryParameter,exitParameter); }
	if (exitParameter<0.0) { result=std::nullopt; return true; }
	const Extended directionLength=DirectionLength(direction);
	const double entry=Rounded(ExtendedMultiply(first,directionLength));
	const double exit=Rounded(ExtendedMultiply(second,directionLength));
	const double length=Rounded(ExtendedMultiply(ExtendedAdd(second,ExtendedMultiply(first,-1.0)),directionLength));
	Bound firstBound=Point(0.0),secondBound=Point(0.0);
	if (constant==0.0) {
		if (certifiedBZero) firstBound=Point(0.0);
		else firstBound=DivideBound(MultiplyBound(Point(-2.0),boundedB),boundedA);
	} else {
		const Bound discriminant=AddBound(MultiplyBound(boundedB,boundedB),NegateBound(MultiplyBound(boundedA,boundedC)));
		const Bound root=SqrtBound(discriminant);
		const double sign=B.fraction<0.0 ? -1.0 : 1.0;
		const Bound numerator=AddBound(NegateBound(boundedB),MultiplyBound(Point(-sign),root));
		firstBound=DivideBound(numerator,boundedA);
		secondBound=DivideBound(boundedC,numerator);
	}
	// first/second were sorted above; use intervals ordered by their endpoint
	// signs (inside roots straddle zero, surface includes exact zero).
	Bound entryBound{std::min(firstBound.low,secondBound.low),std::min(firstBound.high,secondBound.high)};
	Bound exitBound{std::max(firstBound.low,secondBound.low),std::max(firstBound.high,secondBound.high)};
	const ParticleVector entryPoint{std::fma(entryParameter,direction.x,origin.x),std::fma(entryParameter,direction.y,origin.y),std::fma(entryParameter,direction.z,origin.z)};
	const ParticleVector exitPoint{std::fma(exitParameter,direction.x,origin.x),std::fma(exitParameter,direction.y,origin.y),std::fma(exitParameter,direction.z,origin.z)};
	CheckEndpointUncertainty(p,frame,origin,direction,entryPoint,entryBound);
	CheckEndpointUncertainty(p,frame,origin,direction,exitPoint,exitBound);
	result=ParticleChord{entry,exit,length,entryPoint,exitPoint,ImplicitEndpointNormal(p,frame,entryPoint),ImplicitEndpointNormal(p,frame,exitPoint)};
	return true;
}
// Sphere-only raw quartic discriminant. The degree-four expression has at
// most 120 exact product/residual terms; capacity is fixed, not arbitrary
// precision. Every product must have a representable exact FMA residual.
struct SphereExpansion {
	std::array<double,128> components{};
	std::size_t count=0;
	void Add(double value) {
		std::array<double,128> next{};
		std::size_t size=0;
		for (std::size_t i=0;i<count;++i) {
			const SumPair sum=TwoSum(value,components[i]);
			if (sum.tail!=0.0) {
				if (size==next.size()) throw std::range_error("sphere exact discriminant capacity exceeded");
				next[size++]=sum.tail;
			}
			value=sum.head;
		}
		if (value!=0.0 || size==0) {
			if (size==next.size()) throw std::range_error("sphere exact discriminant capacity exceeded");
			next[size++]=value;
		}
		components=next; count=size;
	}
	void Add(const SphereExpansion& other,double sign=1.0) {
		for (std::size_t i=0;i<other.count;++i) Add(sign*other.components[i]);
	}
	int Sign() const {
		for (std::size_t i=count;i>0;--i) if (components[i-1]!=0.0) return components[i-1]<0.0 ? -1 : 1;
		return 0;
	}
	double Value() const {
		double result=0.0;
		for (std::size_t i=0;i<count;++i) result+=components[i];
		if (!std::isfinite(result) || (result==0.0 && Sign()!=0))
			throw std::range_error("sphere exact discriminant result exceeds binary64 range");
		return result;
	}
};
SphereExpansion SphereProduct(double a,double b) {
	SphereExpansion result;
	if (a==0.0 || b==0.0) return result;
	const double product=a*b;
	if (!std::isfinite(product) || !ProductResidualRepresentable(a,b))
		throw std::range_error("sphere raw products exceed supported exact-residual exponent range");
	result.Add(std::fma(a,b,-product)); result.Add(product);
	return result;
}
SphereExpansion SphereMultiply(const SphereExpansion& a,const SphereExpansion& b) {
	SphereExpansion result;
	for (std::size_t i=0;i<a.count;++i) for (std::size_t j=0;j<b.count;++j)
		result.Add(SphereProduct(a.components[i],b.components[j]));
	return result;
}
SphereExpansion SphereScale(const SphereExpansion& a,double b) {
	SphereExpansion result;
	for (std::size_t i=0;i<a.count;++i) result.Add(SphereProduct(a.components[i],b));
	return result;
}
SphereExpansion SphereDot(ParticleVector a,ParticleVector b) {
	SphereExpansion result=SphereProduct(a.x,b.x);
	result.Add(SphereProduct(a.y,b.y)); result.Add(SphereProduct(a.z,b.z));
	return result;
}
SphereExpansion SphereDeterminant(double a,double b,double c,double d) {
	SphereExpansion result=SphereProduct(a,b); result.Add(SphereProduct(c,d),-1.0); return result;
}
std::optional<ParticleChord> IntersectExternalSphere(double radius,ParticleVector origin,ParticleVector direction) {
	const SphereExpansion B=SphereDot(origin,direction);
	// This branch is reached only after the raw metric classified the origin
	// outside. An outward or tangent-forward ray cannot hit the sphere.
	if (B.Sign()>=0) return std::nullopt;
	const SphereExpansion A=SphereDot(direction,direction);
	const SphereExpansion rr=SphereProduct(radius,radius);
	const SphereExpansion gx=SphereDeterminant(origin.y,direction.z,origin.z,direction.y);
	const SphereExpansion gy=SphereDeterminant(origin.z,direction.x,origin.x,direction.z);
	const SphereExpansion gz=SphereDeterminant(origin.x,direction.y,origin.y,direction.x);
	SphereExpansion discriminant=SphereMultiply(rr,A);
	discriminant.Add(SphereMultiply(gx,gx),-1.0);
	discriminant.Add(SphereMultiply(gy,gy),-1.0);
	discriminant.Add(SphereMultiply(gz,gz),-1.0);
	if (discriminant.Sign()<0) return std::nullopt;
	const double normSquared=A.Value(),norm=std::sqrt(normSquared);
	const double root=std::sqrt(discriminant.Value());
	SphereExpansion C=SphereDot(origin,origin); C.Add(rr,-1.0);
	if (C.Sign()<=0) throw std::range_error("sphere outside classification disagrees with exact raw metric");
	const double numerator=-B.Value()+root;
	if (!std::isfinite(numerator)) throw std::range_error("sphere raw root exceeds supported exponent range");
	// A certified zero discriminant has ONE root. Evaluate it once so two
	// algebraically equivalent divisions cannot reverse equal distances.
	const bool tangent=discriminant.Sign()==0;
	const double half=ProductRatio({root},{norm},true);
	double entry=tangent ? ProductRatio({-B.Value(),norm},{normSquared}) : ProductRatio({C.Value(),norm},{numerator});
	double exit=tangent ? entry : ProductRatio({numerator},{norm});
	if (entry>exit) {
		const double centre=ProductRatio({-B.Value()},{norm});
		// At a distant origin both physical roots can have the same rounded
		// distance. Preserve that shared distance and the independent chord.
		if (centre-half!=centre+half) throw std::range_error("sphere root distances cannot be ordered in binary64");
		entry=exit=centre;
	}
	const double length=ProductRatio({2.0,root},{norm});
	// Reconstruct endpoints from the raw perpendicular displacement, not from
	// subtracting distant roots from the origin. Full chord is independent of
	// subtracting its two rounded distances.
	SphereExpansion px=SphereScale(gz,direction.y); px.Add(SphereScale(gy,direction.z),-1.0);
	SphereExpansion py=SphereScale(gx,direction.z); py.Add(SphereScale(gz,direction.x),-1.0);
	SphereExpansion pz=SphereScale(gy,direction.x); pz.Add(SphereScale(gx,direction.y),-1.0);
	const ParticleVector closest{px.Value()/normSquared,py.Value()/normSquared,pz.Value()/normSquared};
	const ParticleVector unit=Unit(direction);
	const ParticleVector start{std::fma(-half,unit.x,closest.x),std::fma(-half,unit.y,closest.y),std::fma(-half,unit.z,closest.z)};
	const ParticleVector end{std::fma(half,unit.x,closest.x),std::fma(half,unit.y,closest.y),std::fma(half,unit.z,closest.z)};
	return ParticleChord{entry,exit,length,start,end,Unit(start),Unit(end)};
}
double HalfChord(const Projection& p, double w) {
	return w == 0.0 ? 0.0 : ProductRatio({p.a,p.b,w},{p.radius},true);
}
double ChordCentre(const Projection& p, double X) {
	// (b-a)*(1+a/b) preserves a tiny departure from the sphere. This is
	// (b*b-a*a)/b without squared-axis overflow or near-sphere cancellation.
	return ProductRatio({p.b-p.a,p.c,p.axialCoefficient,1.0+ProductRatio({p.a},{p.b},true),X},{},true);
}
ParticleChord Chord(const Projection& p, double X, double Y, double w, double originDistance) {
	const double length = w == 0.0 ? 0.0 : ProductRatio({2.0,p.a,p.b,w},{p.radius});
	const double half = HalfChord(p,w);
	const double centre = ChordCentre(p,X);
	const double start = centre-half, end = centre+half;
	const double entry = start-originDistance, exit = end-originDistance;
	if (!std::isfinite(entry) || !std::isfinite(exit)) throw std::overflow_error("particle root overflowed");
	const double x = p.radius*X, y = p.a*Y;
	return {entry,exit,length,Combine(p.e,x,p.f,y,p.direction,start),Combine(p.e,x,p.f,y,p.direction,end),
		Normal(p,X,Y,w,1.0),Normal(p,X,Y,w,-1.0)};
}
}
double ProlateProjectedArea(const ProlateParticle& particle, ParticleVector direction) {
	const Projection p = Project(particle,direction);
	return ProductRatio({Pi,p.a,p.radius},{});
}
std::optional<ParticleChord> IntersectProlateRay(const ProlateParticle& particle, ParticleVector origin, ParticleVector direction) {
	Finite(origin);
	const Projection p = Project(particle,direction);
	std::optional<ParticleChord> insideChord;
	if (TryInsideChord(p,particle.axis,origin,direction,insideChord)) return insideChord;
	if (p.a==p.b) return IntersectExternalSphere(p.a,origin,direction);
	// Project the raw line's perpendicular displacement, not the distant
	// origin onto a rounded normalized frame. The latter can turn an exact
	// central chord into a miss by amplifying one basis ulp by |origin|.
	const Extended gx=ExtendedDifferenceOfProducts(origin.y,direction.z,origin.z,direction.y);
	const Extended gy=ExtendedDifferenceOfProducts(origin.z,direction.x,origin.x,direction.z);
	const Extended gz=ExtendedDifferenceOfProducts(origin.x,direction.y,origin.y,direction.x);
	const Extended directionLength=DirectionLength(direction);
	const double x = -Rounded(ExtendedDivide(ExtendedDot(gx,gy,gz,p.f),directionLength));
	const double y = Rounded(ExtendedDivide(ExtendedDot(gx,gy,gz,p.e),directionLength));
	const double z = Dot(origin,p.direction);
	if (std::abs(x) > p.radius || std::abs(y) > p.a) return std::nullopt;
	// A nonzero impact that disappears here could reappear after the normal
	// divides by a semiaxis. Reject that unsupported range explicitly.
	const double X = ProductRatio({x},{p.radius}), Y = ProductRatio({y},{p.a});
	// Factor the larger squared coordinate's complement: points one ulp inside
	// the silhouette retain that distance rather than subtracting two squares.
	const bool largerX = std::abs(X) >= std::abs(Y);
	const double difference = largerX ? ProductRatio({p.radius-std::abs(x)},{p.radius},true) : ProductRatio({p.a-std::abs(y)},{p.a},true);
	const double larger = largerX ? std::abs(X) : std::abs(Y), other = largerX ? Y : X;
	// An exact rim coordinate leaves no room for ANY other nonzero offset,
	// even when its square falls below binary64. This is not a tangent.
	if (difference == 0.0 && other != 0.0) return std::nullopt;
	const double remainder = std::fma(-other,other,difference*(1.0+larger));
	if (remainder < 0.0) return std::nullopt;
	const double w = std::sqrt(remainder);
	if (ChordCentre(p,X)+HalfChord(p,w) < z) return std::nullopt;
	ParticleChord result = Chord(p,X,Y,w,z);
	if (result.exitDistanceMetres < 0.0) return std::nullopt;
	return result;
}
ProjectedParticleHit SampleProlateProjectedHit(const ProlateParticle& particle, ParticleVector direction, double u, double v) {
	Uniform(u); Uniform(v);
	const Projection p = Project(particle,direction);
	const double radius = std::sqrt(u), azimuth = 2.0*Pi*v;
	// Preserve product exponents until the result is checked. Silently
	// dropping a tiny disk component can become a finite normal error.
	const double X = Rounded(ExtendedProduct({radius,std::cos(azimuth)},{}));
	const double Y = Rounded(ExtendedProduct({radius,std::sin(azimuth)},{}));
	return {Combine(p.e,p.radius*X,p.f,p.a*Y,p.direction,0.0),Chord(p,X,Y,std::sqrt(1.0-u),0.0)};
}
} // namespace RISE::HyLIoS
