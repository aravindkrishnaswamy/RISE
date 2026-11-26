#include "../src/Library/Materials/HyLIoS/HyLIoSParticleGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace RISE::HyLIoS;
namespace {
constexpr double Pi = 3.141592653589793238462643383279502884;
int failures = 0;
void Check(bool condition, const char* contract) {
	if (!condition) { ++failures; std::cerr << "FAIL: " << contract << '\n'; }
}
void Near(double actual, double expected, double tolerance, const char* contract) {
	const double scale = std::max(std::abs(actual),std::abs(expected));
	if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual-expected) > tolerance*scale) {
		++failures; std::cerr.precision(17);
		std::cerr << "FAIL: " << contract << ": actual=" << actual << ", expected=" << expected << '\n';
	}
}
void Absolute(double actual, double expected, double tolerance, const char* contract) {
	Check(std::isfinite(actual) && std::abs(actual-expected) <= tolerance,contract);
}
ParticleVector Add(ParticleVector a, ParticleVector b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
ParticleVector Scale(ParticleVector a, double s) { return {a.x*s,a.y*s,a.z*s}; }
double Dot(ParticleVector a, ParticleVector b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
double Length(ParticleVector a) { return std::hypot(a.x,a.y,a.z); }
ParticleVector Unit(ParticleVector a) { return Scale(a,1.0/Length(a)); }
void VectorNear(ParticleVector a, ParticleVector b, double tolerance, const char* contract) {
	Check(Length(Add(a,Scale(b,-1))) <= tolerance*std::max(Length(a),Length(b)),contract);
}
template<class Exception, class F> void Throws(F operation, const char* contract) {
	try { operation(); Check(false,contract); }
	catch (const Exception&) {}
	catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << contract << ": wrong exception " << error.what() << '\n'; }
}
// Independent implicit-quadric oracle. It uses the original particle axis and
// inverse-squared metric, with no projected frame or silhouette construction.
// Restrict this reference to ordinary, well-conditioned magnitudes; analytic
// identities below cover the cancellation/range boundaries this naive solver
// cannot certify (long double has binary64 precision on some targets).
struct ReferenceChord { long double entry,exit; };
ReferenceChord Reference(const ProlateParticle& p, ParticleVector origin, ParticleVector direction) {
	const ParticleVector n=Unit(p.axis), d=Unit(direction);
	const long double a=p.semiMinorMetres,b=p.semiMajorMetres;
	const auto metric=[&](ParticleVector x, ParticleVector y) {
		const long double xn=static_cast<long double>(x.x)*n.x+static_cast<long double>(x.y)*n.y+static_cast<long double>(x.z)*n.z;
		const long double yn=static_cast<long double>(y.x)*n.x+static_cast<long double>(y.y)*n.y+static_cast<long double>(y.z)*n.z;
		return (static_cast<long double>(x.x)*y.x+static_cast<long double>(x.y)*y.y+static_cast<long double>(x.z)*y.z)/(a*a)+(1/(b*b)-1/(a*a))*xn*yn;
	};
	const long double A=metric(d,d), B=metric(origin,d), C=metric(origin,origin)-1;
	const long double discriminant=B*B-A*C;
	Check(discriminant>0,"reference cases have two well-separated real roots");
	return {(-B-std::sqrt(discriminant))/A,(-B+std::sqrt(discriminant))/A};
}
ParticleVector Rotate(ParticleVector p) {
	// Nontrivial proper rotation from the unit quaternion (1,2,2,0)/3.
	// A cyclic coordinate permutation would miss some orientation mistakes.
	return {(p.x+8*p.y+4*p.z)/9,(8*p.x+p.y-4*p.z)/9,(-4*p.x+4*p.y-7*p.z)/9};
}
void SurfaceAndNormals(const ProlateParticle& p, const ParticleChord& chord) {
	const ParticleVector n=Unit(p.axis);
	for (int i=0;i<2;++i) {
		const ParticleVector point=i ? chord.exitPointMetres : chord.entryPointMetres;
		const ParticleVector normal=i ? chord.exitOutwardNormal : chord.entryOutwardNormal;
		const double axial=Dot(point,n);
		const ParticleVector transverse=Add(point,Scale(n,-axial));
		Near(Dot(transverse,transverse)/(p.semiMinorMetres*p.semiMinorMetres)+axial*axial/(p.semiMajorMetres*p.semiMajorMetres),1.0,2e-12,"endpoint lies on independent implicit spheroid");
		const ParticleVector gradient=Add(Scale(transverse,1.0/(p.semiMinorMetres*p.semiMinorMetres)),Scale(n,axial/(p.semiMajorMetres*p.semiMajorMetres)));
		VectorNear(normal,Unit(gradient),2e-12,"outward normal agrees with implicit gradient");
		Near(Length(normal),1.0,1e-14,"outward normal is unit length");
	}
}
}
int main() {
	const ParticleVector z{0,0,1},x{1,0,0};
	const ProlateParticle sphere{2,2,z},prolate{2,5,z};
	const double largest=std::numeric_limits<double>::max(),smallest=std::numeric_limits<double>::denorm_min();
	const double nan=std::strtod("nan",nullptr),infinity=std::strtod("inf",nullptr);
	// Exact contracts and analytic limits first.
	Near(ProlateProjectedArea(sphere,{1,2,3}),4*Pi,5e-16,"sphere projected area is pi*r^2 for every direction");
	Near(ProlateProjectedArea(prolate,z),4*Pi,5e-16,"axial spheroid projection is the minor disk");
	Near(ProlateProjectedArea(prolate,x),10*Pi,5e-16,"transverse spheroid projection is pi*a*b");
	Near(ProlateProjectedArea(prolate,{3,0,4}),2*Pi*std::sqrt(11.56),1e-15,"oblique projection matches analytic orthographic ellipse");
	const auto sphereChord=IntersectProlateRay(sphere,{1,0,-3},{0,0,7});
	Check(sphereChord.has_value(),"sphere line through disk hits");
	if(sphereChord) {
		Near(sphereChord->entryDistanceMetres,3-std::sqrt(3.0),1e-15,"sphere entry distance is analytic");
		Near(sphereChord->exitDistanceMetres,3+std::sqrt(3.0),1e-15,"sphere exit distance is analytic");
		Near(sphereChord->chordLengthMetres,2*std::sqrt(3.0),1e-15,"sphere full chord is analytic");
		SurfaceAndNormals(sphere,*sphereChord);
	}
	for (ParticleVector d : {z,x}) {
		const auto chord=IntersectProlateRay(prolate,{0,0,0},d);
		Check(chord.has_value(),"inside origin has a forward boundary");
		if(chord) {
			const double radius=d.z==1 ? 5.0 : 2.0;
			Check(chord->entryDistanceMetres==-radius && chord->exitDistanceMetres==radius,"axial/transverse central roots have exact signed distances");
			Check(chord->chordLengthMetres==2*radius,"inside ray retains full chord rather than remaining distance");
			VectorNear(chord->entryOutwardNormal,Scale(d,-1),0,"central entry normal opposes direction");
			VectorNear(chord->exitOutwardNormal,d,0,"central exit normal follows direction");
		}
	}
	const auto axial=IntersectProlateRay(prolate,{1,0,-10},z);
	if(axial) Near(axial->chordLengthMetres,10*std::sqrt(0.75),1e-15,"off-axis axial chord is 2*b*sqrt(1-rho^2/a^2)");
	else Check(false,"off-axis axial chord exists");
	const auto transverse=IntersectProlateRay(prolate,{-8,0,3},x);
	if(transverse) Near(transverse->chordLengthMetres,3.2,1e-15,"off-axis transverse chord is 2*a*sqrt(1-z^2/b^2)");
	else Check(false,"off-axis transverse chord exists");
	Check(!IntersectProlateRay(prolate,{3,0,-10},z),"line beyond silhouette misses");
	Check(!IntersectProlateRay(prolate,{0,0,6},z),"complete chord behind ray origin misses");
	Check(!IntersectProlateRay({1e308,1e308,z},{0,0,1.7e308},z),"behind-ray rejection precedes unrepresentable full-chord construction");
	const auto tangent=IntersectProlateRay(prolate,{2,0,-3},z);
	Check(tangent && tangent->entryDistanceMetres==3 && tangent->exitDistanceMetres==3 && tangent->chordLengthMetres==0,"exact tangent is retained without epsilon");
	Check(!IntersectProlateRay({1,1,z},{1,smallest,0},z),"exact silhouette rim plus any nonzero second coordinate is outside despite squared underflow");
	const auto onSurface=IntersectProlateRay(prolate,{0,0,5},z);
	Check(onSurface && onSurface->exitDistanceMetres==0,"outward ray on exact surface retains zero-distance exit");
	Check(!IntersectProlateRay(prolate,{std::nextafter(2.0,3.0),0,-3},z),"one-ulp outside tangent is a miss without widened gate");
	const auto nearTangent=IntersectProlateRay(prolate,{std::nextafter(2.0,0.0),0,0},z);
	Check(nearTangent && nearTangent->chordLengthMetres>0,"one-ulp inside tangent retains positive true chord");
	if(nearTangent) Near(nearTangent->chordLengthMetres,10*std::sqrt((2-std::nextafter(2.0,0.0))/2*(1+std::nextafter(2.0,0.0)/2)),1e-15,"near-rim chord preserves factored complement");
	const auto far=IntersectProlateRay(sphere,{0,0,-1e100},z);
	Check(far && far->chordLengthMetres==4,"distant-origin root cancellation does not erase physical chord");
	for(double distance : {1e16,1e100}) {
		const auto oblique=IntersectProlateRay({1,1,z},{-distance,-2*distance,-4*distance},{1,2,4});
		Check(oblique && oblique->chordLengthMetres==2,"raw oblique central line retains diameter at distant origin");
		const double microscopic=1e-100;
		const auto rescaled=IntersectProlateRay({microscopic,microscopic,z},
			{-distance*microscopic,-2*distance*microscopic,-4*distance*microscopic},{1,2,4});
		Check(rescaled.has_value(),"raw oblique microscopic central line retains intersection under length rescaling");
		if(rescaled) Near(rescaled->chordLengthMetres,2*microscopic,1e-14,"raw oblique microscopic central line retains scaled diameter");
	}
	const double powerOfTwoDistance=0x1p54;
	const auto farOffset=IntersectProlateRay({5,5,z},
		{-powerOfTwoDistance,-2*powerOfTwoDistance,-4*powerOfTwoDistance+8},{1.1,2.2,4.4});
	Check(farOffset.has_value(),"compensated raw products retain noncentral far-line intersection");
	if(farOffset) Near(farOffset->chordLengthMetres,2*std::sqrt(25.0-320.0/21.0),2e-15,
		"compensated raw products retain exact eight-unit projected offset");
	const ParticleVector translatedCentre{32,-64,128},translatedOrigin{31,-66,124};
	const auto translatedMicroscopic=IntersectProlateRay({0x1p-40,0x1p-40,z},
		Add(translatedOrigin,Scale(translatedCentre,-1)),{1,2,4});
	Check(translatedMicroscopic && translatedMicroscopic->chordLengthMetres==0x1p-39,
		"particle-relative raw oblique projection preserves translated microscopic diameter");
	for(double major : {1.0,2.0}) {
		const auto extremeTangent=IntersectProlateRay({smallest,major,z},{-0.48*major,0,0.64*major},{0.8,0,0.6});
		if(extremeTangent && extremeTangent->chordLengthMetres==0)
			Absolute(Dot(extremeTangent->exitOutwardNormal,{0.8,0,0.6}),0,2e-15,"underflowing spheroid coefficients cannot produce a non-tangent tangent normal");
	}
	for(double major : {0x1p20,0x1p52,0x1p100}) {
		const ProlateParticle grazing{1,major,z};
		const ParticleVector rawDirection{1/major,0,1};
		const auto surfaceChord=IntersectProlateRay(grazing,{1,0,0},rawDirection);
		Check(surfaceChord && surfaceChord->exitDistanceMetres==0,"implicit surface deflation retains exact zero exit for long grazing chord");
		if(surfaceChord) {
			VectorNear(surfaceChord->entryPointMetres,{0,0,-major},0,"implicit surface deflation retains exact opposite pole");
			VectorNear(surfaceChord->entryOutwardNormal,{0,0,-1},0,"implicit surface deflation retains exact polar normal");
			Near(surfaceChord->chordLengthMetres,std::hypot(major,1.0),2e-15,"implicit surface chord length uses physical normalized-direction distance");
		}
		const auto reversedSurface=IntersectProlateRay(grazing,{-1,0,0},rawDirection);
		Check(reversedSurface && reversedSurface->entryDistanceMetres==0,"implicit surface deflation retains exact zero entry with opposite incidence");
		if(reversedSurface) VectorNear(reversedSurface->exitOutwardNormal,{0,0,1},0,"implicit surface deflation retains opposite-incidence polar normal");
		const double insideX=std::nextafter(1.0,0.0);
		const auto nearSurface=IntersectProlateRay(grazing,{insideX,0,0},rawDirection);
		Check(nearSurface.has_value(),"implicit near-surface inside origin retains grazing intersection");
		if(nearSurface) {
			const double mass=(1-insideX)*(1+insideX);
			const double expected=major*mass/(insideX+std::sqrt(1+mass))*std::hypot(1.0,1/major);
			Near(nearSurface->exitDistanceMetres,expected,3e-15,"implicit near-surface small root matches factored analytic Vieta oracle");
		}
		const auto midpoint=IntersectProlateRay(grazing,{0.5,0,-major/2},rawDirection);
		Check(midpoint.has_value(),"implicit inside-origin grazing chord retains intersection");
		if(midpoint) {
			VectorNear(midpoint->entryPointMetres,{0,0,-major},0,"implicit midpoint chord retains exact opposite pole");
			VectorNear(midpoint->entryOutwardNormal,{0,0,-1},0,"implicit midpoint chord retains exact polar normal");
			VectorNear(midpoint->exitPointMetres,{1,0,0},0,"implicit midpoint chord retains exact equator exit");
		}
	}
	// Independent reference and rotation/scale/axis-sign covariance.
	std::mt19937_64 generator(0x82b43a6);
	std::uniform_real_distribution<double> uniform(0,1);
	for(int i=0;i<250;++i) {
		const ProlateParticle p{0.5,1.0+2*uniform(generator),{uniform(generator)-0.5,uniform(generator)-0.5,uniform(generator)-0.5}};
		const ParticleVector d=Unit({uniform(generator)-0.5,uniform(generator)-0.5,uniform(generator)-0.5});
		// Origins stay inside the inscribed ball; both roots always exist.
		const ParticleVector origin{0.1*uniform(generator),0.1*uniform(generator),0.1*uniform(generator)};
		const auto chord=IntersectProlateRay(p,origin,d);
		Check(chord.has_value(),"inscribed-ball origin always intersects");
		if(!chord) continue;
		const auto reference=Reference(p,origin,d);
		Near(chord->entryDistanceMetres,static_cast<double>(reference.entry),2e-13,"projected chord entry agrees with independent quadratic");
		Near(chord->exitDistanceMetres,static_cast<double>(reference.exit),2e-13,"projected chord exit agrees with independent quadratic");
		SurfaceAndNormals(p,*chord);
		VectorNear(chord->entryPointMetres,Add(origin,Scale(d,chord->entryDistanceMetres)),2e-13,"entry point stays on original ray");
		const ProlateParticle rotated{p.semiMinorMetres,p.semiMajorMetres,Rotate(p.axis)};
		const auto rotatedChord=IntersectProlateRay(rotated,Rotate(origin),Rotate(d));
		Check(rotatedChord.has_value(),"rotation preserves intersection");
		if(rotatedChord) {
			Near(rotatedChord->chordLengthMetres,chord->chordLengthMetres,2e-14,"rotation preserves chord length");
			VectorNear(rotatedChord->entryPointMetres,Rotate(chord->entryPointMetres),2e-13,"entry point rotates covariantly");
			VectorNear(rotatedChord->exitOutwardNormal,Rotate(chord->exitOutwardNormal),2e-13,"normal rotates covariantly");
		}
		const auto sample=SampleProlateProjectedHit(p,d,0.37,0.19);
		const auto rotatedSample=SampleProlateProjectedHit(rotated,Rotate(d),0.37,0.19);
		VectorNear(rotatedSample.projectedPointMetres,Rotate(sample.projectedPointMetres),2e-13,"oblique projected sample rotates with explicit particle frame");
		VectorNear(rotatedSample.chord.exitOutwardNormal,Rotate(sample.chord.exitOutwardNormal),2e-13,"sampled exit normal rotates covariantly");
		const ProlateParticle scaled{p.semiMinorMetres*1e-8,p.semiMajorMetres*1e-8,Scale(p.axis,-3)};
		const auto scaledChord=IntersectProlateRay(scaled,Scale(origin,1e-8),Scale(d,1e100));
		Check(scaledChord.has_value(),"microscopic geometry/axis reversal preserves hit");
		if(scaledChord) {
			Near(scaledChord->chordLengthMetres,1e-8*chord->chordLengthMetres,2e-13,"physical chord scales with particle size");
			VectorNear(scaledChord->exitOutwardNormal,chord->exitOutwardNormal,2e-13,"axis sign, direction magnitude and length scale preserve normal");
		}
	}
	// The projected-area sampler has exact radial CDF u, not the surface-area
	// distribution. Deterministic midpoint quadrature supplies independent
	// projected second moments and the volume/area mean-chord identity.
	for(ParticleVector d : {z,x,Unit({3,1,4})}) {
		const double area=ProlateProjectedArea(prolate,d);
		const auto centre=SampleProlateProjectedHit(prolate,d,0,0);
		Check(Length(centre.projectedPointMetres)==0,"zero radial uniform samples silhouette centre exactly");
		const double meanChord=4*Pi*2*2*5/(3*area);
		double sumChord=0,sumNormSquared=0,sumRadialFourth=0,sumAxisProjectionSquared=0;
		constexpr int radialCount=2000,azimuthCount=8;
		for(int i=0;i<radialCount;++i) for(int j=0;j<azimuthCount;++j) {
			volatile double storedUniform=(i+0.5)/radialCount;
			const double u=storedUniform;
			const auto sample=SampleProlateProjectedHit(prolate,d,u,(j+0.5)/azimuthCount);
			Near(sample.chord.chordLengthMetres,centre.chord.chordLengthMetres*std::sqrt(1-u),3e-15,"area-uniform disk radial CDF gives analytic chord at every sample");
			Absolute(Dot(sample.projectedPointMetres,d),0,2e-14,"projected point is perpendicular to incoming direction");
			sumChord+=sample.chord.chordLengthMetres;
			sumNormSquared+=Dot(sample.projectedPointMetres,sample.projectedPointMetres);
			sumRadialFourth+=u*u;
			sumAxisProjectionSquared+=sample.projectedPointMetres.z*sample.projectedPointMetres.z;
			if(i==0 && j==0) SurfaceAndNormals(prolate,sample.chord);
		}
		const double count=radialCount*azimuthCount;
		Near(sumChord/count,meanChord,2e-6,"mean true chord equals volume/projected area");
		const double projectedMajor=area/(2*Pi);
		Near(sumNormSquared/count,(projectedMajor*projectedMajor+4)/4,2e-13,"uniform ellipse projected second moment is (L^2+a^2)/4");
		const double axisSineSquared=d.x*d.x+d.y*d.y;
		Absolute(sumAxisProjectionSquared/count,axisSineSquared*(4+21*axisSineSquared)/4,2e-12,"projected second moment along particle axis matches independent covariance");
		Near(sumRadialFourth/count,1.0/3,1e-7,"uniform disk fourth radial moment is 1/3");
		const auto rim=SampleProlateProjectedHit(prolate,d,std::nextafter(1.0,0.0),0.125);
		Check(rim.chord.chordLengthMetres>0,"last representable radial uniform retains nonzero chord without rejection");
	}
	const auto extremeProjected=SampleProlateProjectedHit({smallest,2,z},{0.8,0,0.6},0.75,0);
	Check(extremeProjected.chord.exitOutwardNormal.x==1 && extremeProjected.chord.exitOutwardNormal.z==smallest,
		"known projected disk parameters retain analytic extreme-aspect gradient normal");
	// Exponent boundaries: no squared-axis overflow/underflow; arbitrary-length
	// orientation vectors normalize without squaring their original components.
	for(double scale : {1e-150,1e150}) {
		const ProlateParticle p{scale,2*scale,{0,0,largest}};
		const auto sample=SampleProlateProjectedHit(p,{0,0,smallest},0.75,0);
		Near(sample.chord.chordLengthMetres,2*scale,2e-15,"microscopic/huge axial sample chord scales analytically");
		Near(ProlateProjectedArea(p,z),Pi*scale*scale,2e-15,"scaled projected area avoids spurious intermediate range loss");
	}
	// Mixed magnitudes must retain a representable transverse component. Scaling
	// max=1 into [.5,1) would erase denorm_min before normalization restores it.
	const auto mixedDirection=SampleProlateProjectedHit({1,1,z},{1,0,smallest},0,0);
	Check(mixedDirection.chord.exitPointMetres.z==smallest,"mixed-component normalization preserves representable ray component");
	Check(mixedDirection.chord.exitOutwardNormal.z==smallest,"mixed-component normalization preserves representable sphere normal");
	const auto mixedScaledSphere=SampleProlateProjectedHit({3,3,z},{1,0,smallest},0,0);
	Check(mixedScaledSphere.chord.exitOutwardNormal.z==smallest,"mixed-component gradient exponent packing preserves representable normal");
	const auto mixedAxis=SampleProlateProjectedHit({1,2,{1,0,smallest}},x,0,0);
	Check(mixedAxis.chord.exitOutwardNormal.z==-3*smallest,"mixed-component normalization preserves tilted spheroid axis and normal");
	const auto subnormalSphere=SampleProlateProjectedHit({smallest,smallest,z},z,0.75,0);
	Check(subnormalSphere.chord.chordLengthMetres==smallest,"full chord retains subnormal length when half-chord rounds to zero");
	const ProlateParticle slender{smallest,largest,z};
	const auto tiny=IntersectProlateRay(slender,{0,0,0},x);
	Check(tiny && tiny->chordLengthMetres==2*smallest,"axis ratio below representable range keeps transverse subnormal chord");
	if(tiny) VectorNear(tiny->exitOutwardNormal,x,0,"extreme-ratio transverse normal remains exact");
	const auto pole=SampleProlateProjectedHit({smallest,1.0,z},z,0,0);
	Check(pole.chord.chordLengthMetres==2,"extreme-ratio axial chord remains macroscopic");
	VectorNear(pole.chord.exitOutwardNormal,z,0,"extreme-ratio pole normal survives vanishing a/b");
	volatile double compensatedAxes=smallest*largest;
	Near(ProlateProjectedArea(slender,x),Pi*compensatedAxes,2e-15,"tiny minor/huge major projected area preserves compensating exponents");
	Throws<std::underflow_error>([&]{ IntersectProlateRay({2,1e100,z},{0,smallest,0},z); },
		"unrepresentable normalized impact is an explicit range failure before normal amplification");
	Throws<std::underflow_error>([&]{ SampleProlateProjectedHit({1,1e300,z},z,smallest,smallest); },
		"unrepresentable sampled disk component is an explicit range failure before normal amplification");
	Throws<std::overflow_error>([&]{ ProlateProjectedArea({largest,largest,z},z); },"unrepresentable projected area throws overflow");
	Throws<std::underflow_error>([&]{ ProlateProjectedArea({smallest,smallest,z},z); },"unrepresentable positive projected area throws underflow");
	Throws<std::overflow_error>([&]{ SampleProlateProjectedHit(slender,z,0,0); },"unrepresentable true full chord throws overflow");
	for(double bad : {-1.0,0.0,nan,infinity}) {
		Throws<std::invalid_argument>([&]{ ProlateProjectedArea({bad,2,z},z); },"invalid minor axis is rejected");
		Throws<std::invalid_argument>([&]{ ProlateProjectedArea({1,bad,z},z); },"invalid major axis is rejected");
	}
	for(ParticleVector bad : {ParticleVector{0,0,0},ParticleVector{nan,0,1},ParticleVector{0,infinity,1}}) {
		Throws<std::invalid_argument>([&]{ ProlateProjectedArea({1,2,bad},z); },"invalid particle axis is rejected");
		Throws<std::invalid_argument>([&]{ SampleProlateProjectedHit(prolate,bad,0,0); },"invalid ray direction is rejected");
	}
	Throws<std::invalid_argument>([&]{ IntersectProlateRay(prolate,{0,nan,0},z); },"nonfinite relative origin is rejected");
	for(double bad : {-0.01,1.0,nan,infinity}) {
		Throws<std::invalid_argument>([&]{ SampleProlateProjectedHit(prolate,z,bad,0); },"invalid radial uniform is rejected");
		Throws<std::invalid_argument>([&]{ SampleProlateProjectedHit(prolate,z,0,bad); },"invalid azimuth uniform is rejected");
	}
	if(failures) { std::cerr << failures << " HyLIoS particle geometry checks failed\n"; return 1; }
	std::cout << "PASS: HyLIoS particle geometry analytic, independent-reference, sampling and boundary contracts\n";
	return 0;
}
