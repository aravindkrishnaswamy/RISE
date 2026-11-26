#include "pch.h"
#include "HyLIoSParticleGeometry.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace RISE::HyLIoS {
namespace {
constexpr double Pi = 3.141592653589793238462643383279502884;
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
ParticleVector Cross(ParticleVector a, ParticleVector b) {
	return {DifferenceOfProducts(a.y,b.z,a.z,b.y),DifferenceOfProducts(a.z,b.x,a.x,b.z),
		DifferenceOfProducts(a.x,b.y,a.y,b.x)};
}
// Sum opposite signs first, avoiding an overflowing partial sum when the final
// projection is finite. FMA keeps product residuals at cancellation-heavy dots.
double Dot(ParticleVector a, ParticleVector b) {
	double p[3] = {a.x*b.x,a.y*b.y,a.z*b.z};
	const double residual = std::fma(a.x,b.x,-p[0])+std::fma(a.y,b.y,-p[1])+std::fma(a.z,b.z,-p[2]);
	if (std::signbit(p[0]) == std::signbit(p[1])) {
		if (std::signbit(p[0]) != std::signbit(p[2])) std::swap(p[1],p[2]);
	}
	const double sum = p[0]+p[1];
	const double second = sum-p[0];
	const double error = (p[0]-(sum-second))+(p[1]-second);
	const double result = sum+(p[2]+(error+residual));
	if (!std::isfinite(result)) throw std::overflow_error("particle projection overflowed");
	return result;
}
ParticleVector Combine(ParticleVector a, double x, ParticleVector b, double y, ParticleVector c, double z) {
	const ParticleVector result{Dot({x,y,z},{a.x,b.x,c.x}),Dot({x,y,z},{a.y,b.y,c.y}),Dot({x,y,z},{a.z,b.z,c.z})};
	return result;
}
struct Projection {
	double a,b,c,s,radius,radialCoefficient,axialCoefficient;
	ParticleVector direction,e,f,radial,axis;
};
Projection Project(const ProlateParticle& particle, ParticleVector direction) {
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
	// A and B are a plane rotation in the ellipsoid's unit-sphere space.
	const double A = ProductRatio({a,c},{radius},true);
	const double B = s == 0.0 ? 0.0 : ProductRatio({b,s},{radius});
	return {a,b,c,s,radius,A,B,d,e,f,Combine(e,c,d,-s,f,0.0),Combine(e,s,d,c,f,0.0)};
}
ParticleVector Normal(const Projection& p, double radial, double side, double axial) {
	// Normalize (radial/a, side/a, axial/b) using separate exponents. Neither
	// squaring a submicron axis nor forming the potentially overflowing b/a
	// is necessary, and a pole remains valid even when a/b rounds to zero.
	double values[3] = {radial,side,axial};
	const double divisors[3] = {p.a,p.a,p.b};
	int exponents[3] = {0,0,0};
	int maximum = -10000;
	for (int i=0; i<3; ++i) {
		if (values[i] == 0.0) continue;
		int numeratorExponent = 0, divisorExponent = 0;
		values[i] = std::frexp(values[i],&numeratorExponent)/std::frexp(divisors[i],&divisorExponent);
		exponents[i] = numeratorExponent-divisorExponent;
		maximum = std::max(maximum,exponents[i]);
	}
	// Mantissa quotients lie in (0.5,2). Keep a largest-exponent component
	// in (1,4), so subsequent normalization cannot restore an avoidably lost
	// subnormal. The bounded vector norm cannot overflow.
	--maximum;
	for (int i=0; i<3; ++i) if (values[i] != 0.0) values[i] = std::ldexp(values[i],exponents[i]-maximum);
	const ParticleVector local = Unit({values[0],values[1],values[2]});
	return Unit(Combine(p.radial,local.x,p.f,local.y,p.axis,local.z));
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
		Normal(p,std::fma(p.axialCoefficient,w,p.radialCoefficient*X),Y,
			std::fma(-p.radialCoefficient,w,p.axialCoefficient*X)),
		Normal(p,std::fma(-p.axialCoefficient,w,p.radialCoefficient*X),Y,
			std::fma(p.radialCoefficient,w,p.axialCoefficient*X))};
}
}
double ProlateProjectedArea(const ProlateParticle& particle, ParticleVector direction) {
	const Projection p = Project(particle,direction);
	return ProductRatio({Pi,p.a,p.radius},{});
}
std::optional<ParticleChord> IntersectProlateRay(const ProlateParticle& particle, ParticleVector origin, ParticleVector direction) {
	Finite(origin);
	const Projection p = Project(particle,direction);
	const double x = Dot(origin,p.e), y = Dot(origin,p.f), z = Dot(origin,p.direction);
	if (std::abs(x) > p.radius || std::abs(y) > p.a) return std::nullopt;
	const double X = ProductRatio({x},{p.radius},true), Y = ProductRatio({y},{p.a},true);
	// Factor the larger squared coordinate's complement: points one ulp inside
	// the silhouette retain that distance rather than subtracting two squares.
	const bool largerX = std::abs(X) >= std::abs(Y);
	const double difference = largerX ? ProductRatio({p.radius-std::abs(x)},{p.radius},true) : ProductRatio({p.a-std::abs(y)},{p.a},true);
	const double larger = largerX ? std::abs(X) : std::abs(Y), other = largerX ? Y : X;
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
	const double X = radius*std::cos(azimuth), Y = radius*std::sin(azimuth);
	return {Combine(p.e,p.radius*X,p.f,p.a*Y,p.direction,0.0),Chord(p,X,Y,std::sqrt(1.0-u),0.0)};
}
} // namespace RISE::HyLIoS
