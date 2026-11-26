#include "../src/Library/Materials/HyLIoS/HyLIoSOptics.h"

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
void Near(double actual, double expected, double relativeTolerance, const char* contract) {
	const bool valid = std::isfinite(actual) && std::isfinite(expected);
	const double scale = std::max(std::abs(actual), std::abs(expected));
	if (!valid || std::abs(actual-expected) > relativeTolerance*scale) {
		++failures; std::cerr << "FAIL: " << contract << ": actual=" << actual << ", expected=" << expected << '\n';
	}
}
template<class Exception, class F> void Throws(F operation, const char* contract) {
	try { operation(); Check(false, contract); }
	catch (const Exception&) {}
	catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << contract << ": wrong exception " << error.what() << '\n'; }
}
template<class F> double Integrate(F function, double low, double high) {
	// Independent composite Simpson reference, including the endpoints.
	constexpr int count = 20000;
	const double step = (high-low)/count;
	double sum = function(low)+function(high);
	for (int i=1; i<count; ++i) sum += (i%2 ? 4.0 : 2.0)*function(low+i*step);
	return sum*step/3.0;
}
double OrientationCdf(double x, double ratio) {
	// Independently integrated linear cosine density, with symmetric half masses.
	if (x <= 0.0) return (x+1.0 + (1.0-ratio)*(x*x-1.0)/2.0)/(1.0+ratio);
	return 0.5+(x-(1.0-ratio)*x*x/2.0)/(1.0+ratio);
}
}
int main() {
	const double largest = std::numeric_limits<double>::max();
	const double smallest = std::numeric_limits<double>::denorm_min();
	const double nan = std::strtod("nan", nullptr);
	const double infinity = std::strtod("inf", nullptr);
	// Exact contracts, analytic limits and dimensional oracles.
	Check(ProlateSurfaceToVolume(2.0,2.0)==1.5, "sphere S/V is exactly 3/r");
	Near(ProlateSurfaceToVolume(0.25,1.0), 10.0*ProlateSurfaceToVolume(2.5,10.0), 2e-15, "axis scaling gives inverse length");
	Near(ProlateSurfaceToVolume(1.0,std::nextafter(1.0,2.0)), 3.0, 5e-16, "near-sphere expression has continuous finite limit");
	Near(ProlateSurfaceToVolume(1.0,largest), 3.0*Pi/4.0, 2e-15, "slender spheroid approaches 3*pi/(4*a)");
	Near(GeometricEncounterRate(ProlateSurfaceToVolume(2.0,2.0),0.2),0.075,2e-15,"sphere encounter rate is 3*v/(4*r)");
	Check(GeometricEncounterRate(largest,0.0)==0.0,"zero volume eliminates geometric events exactly");
	Near(GeometricEncounterRate(largest,smallest),largest*smallest/4.0,2e-15,"tiny fraction does not vanish before large S/V compensates");
	Check(!SampleExponentialFreePath(0.0,0.25),"zero rate is explicit no-event");
	Check(SampleExponentialFreePath(2.0,0.0).value()==0.0,"zero uniform gives zero distance at positive rate");
	Near(SampleExponentialFreePath(2.0,0.5).value(),std::log(2.0)/2.0,2e-15,"free path median equals log(2)/rate");
	Near(SampleExponentialFreePath(1e-100,1e-200).value(),1e-100,2e-15,"log1p resolves tiny variates and reciprocal rate units");
	for (double x : {-1.0,-0.4,0.0,0.4,1.0}) Check(OrientationCosinePdf(x,2.0,2.0)==0.5,"sphere orientation is isotropic in cosine");
	for (double u : {0.0,0.125,0.5,0.875}) Check(SampleOrientationCosine(u,1.0,1.0)==2.0*u-1.0,"sphere inverse CDF is uniform cosine");
	Check(SampleOrientationCosine(0.0,1.0,2.0)==-1.0,"orientation inverse reaches lower endpoint exactly");
	Check(SampleOrientationCosine(0.5,1.0,2.0)==0.0,"orientation inverse median is zero");
	for (double ratio : {1.0,0.5,1e-6,1e-100}) {
		Near(Integrate([&](double x){ return OrientationCosinePdf(x,ratio,1.0); },-1.0,1.0),1.0,2e-13,"orientation cosine PDF integrates to one");
		const double expectedSecond = (1.0+3.0*ratio)/(6.0*(1.0+ratio));
		Near(Integrate([&](double x){ return x*x*OrientationCosinePdf(x,ratio,1.0); },-1.0,1.0),expectedSecond,2e-13,"orientation analytic second moment matches independent quadrature");
		for (double u : {0.001,0.125,0.25,0.5,0.75,0.99,std::nextafter(1.0,0.0)})
			Near(OrientationCdf(SampleOrientationCosine(u,ratio,1.0),ratio),u,2e-12,"orientation inverse CDF inverts independent antiderivative");
	}
	Check(IsExponentialForwardDelta(0.0),"zero scale identifies a delta explicitly");
	Check(!IsExponentialForwardDelta(1e-100),"positive tiny scale remains a continuous distribution");
	const auto delta = TrySampleExponentialForward(0.0,0.9,0.9,0.9);
	Check(delta && delta->isForwardDelta && delta->thetaRadians==0.0,"zero-scale sample is exactly forward delta");
	const auto exactForward = TrySampleExponentialForward(1.0,0.0,0.0,0.99);
	Check(exactForward && !exactForward->isForwardDelta && exactForward->thetaRadians==0.0,"continuous proposal endpoint uses sinc(0)=1");
	Check(!TrySampleExponentialForward(2.0,0.9,0.9,0.0),"beyond-pi gamma proposal is rejected after one trial");
	Check(!TrySampleExponentialForward(1.0,0.8,0.1,0.99),"sinc rejection is distinct from transport termination");
	for (double scale : {0.01,0.1,1.0,10.0,100.0}) {
		const double independentZ = Integrate([&](double theta){ return std::exp(-theta/scale)*std::sin(theta); },0.0,Pi);
		Near(ExponentialForwardNormalization(scale),independentZ,5e-9,"forward Z matches independent integral");
		Near(Integrate([&](double theta){ return ExponentialForwardPolarPdf(theta,scale); },0.0,Pi),1.0,5e-9,"forward polar PDF is normalized with sin(theta) measure");
	}
	Near(ExponentialForwardNormalization(largest),2.0,2e-15,"large scale normalization tends to two without squaring overflow");
	Near(ExponentialForwardPolarPdf(Pi/2.0,largest),0.5,2e-15,"large scale polar density tends to sin(theta)/2");
	Near(ExponentialForwardPolarPdf(1e-200,1e-200),std::exp(-1.0)*1e200,2e-12,"log normalization preserves finite PDF when Z underflows");
	Check(ExponentialForwardPolarPdf(0.0,1.0)==0.0 && ExponentialForwardPolarPdf(Pi,1.0)==0.0,"continuous polar PDF has exact zero endpoints");
	Check(RayleighRate(1.0,1.0,1.0,1.0)==0.0,"matched indices eliminate fiber event rate exactly");
	Check(RayleighRate(largest,smallest,0.0,2.0)==0.0,"inactive fiber rate avoids intermediate overflows");
	const double rayleigh = RayleighRate(0.1,0.5,0.2,2.0);
	// Independent unreduced printed factors, valid here without extreme powers.
	const double printed = (128.0*std::pow(Pi,5)*std::pow(0.1,6)*0.2/(3.0*std::pow(0.5,4)))
		*(3.0/(4.0*Pi*std::pow(0.1,3)))*std::pow(3.0/5.0,2);
	Near(rayleigh,printed,3e-15,"Rayleigh rate follows printed +1 contrast denominator");
	Near(RayleighRate(0.2,0.5,0.2,2.0),8.0*rayleigh,3e-15,"Rayleigh scales cubically with radius");
	Near(RayleighRate(0.1,1.0,0.2,2.0),rayleigh/16.0,3e-15,"Rayleigh scales as inverse wavelength fourth power");
	Near(RayleighRate(0.1,0.5,0.4,2.0),2.0*rayleigh,3e-15,"Rayleigh scales linearly with volume fraction");
	Near(RayleighRate(1e100,1e75,0.2,2.0),32.0*std::pow(Pi,4)*0.2*0.36,5e-15,"Rayleigh preserves finite result across overflowing intermediate powers");
	Near(RayleighRate(1.0,1.0,1.0,largest),32.0*std::pow(Pi,4),3e-15,"huge finite index ratio has finite unit-contrast limit");
	Near(Integrate([](double x){ return RayleighCosinePdf(x); },-1.0,1.0),1.0,2e-13,"Rayleigh cosine PDF integrates to one");
	Near(Integrate([](double x){ return x*x*RayleighCosinePdf(x); },-1.0,1.0),0.4,2e-13,"Rayleigh analytic cosine second moment equals 2/5");
	Check(TrySampleRayleighCosine(0.5,0.5).value()==0.0,"Rayleigh acceptance equality is accepted");
	Check(!TrySampleRayleighCosine(0.5,0.75),"Rayleigh single-trial rejection is explicit");
	// Invalid inputs and explicit numerical failure, including inactive limits.
	for (double bad : {-1.0,0.0,nan,infinity}) {
		Throws<std::invalid_argument>([&]{ ProlateSurfaceToVolume(bad,1.0); },"invalid spheroid minor axis is rejected");
		Throws<std::invalid_argument>([&]{ RayleighRate(1.0,bad,0.0,1.0); },"inactive fiber still validates wavelength");
		Throws<std::invalid_argument>([&]{ ExponentialForwardNormalization(bad); },"continuous normalization requires finite positive scale");
	}
	for (double bad : {-0.1,1.0,nan,infinity}) {
		Throws<std::invalid_argument>([&]{ SampleExponentialFreePath(0.0,bad); },"inactive free-path clock still validates uniform");
		Throws<std::invalid_argument>([&]{ SampleOrientationCosine(bad,1.0,2.0); },"orientation rejects invalid uniform");
		Throws<std::invalid_argument>([&]{ TrySampleExponentialForward(0.0,0.0,bad,0.0); },"delta sampler still validates variates");
		Throws<std::invalid_argument>([&]{ TrySampleRayleighCosine(0.0,bad); },"Rayleigh rejects invalid acceptance variate");
	}
	Throws<std::invalid_argument>([]{ ProlateSurfaceToVolume(2.0,1.0); },"oblate axes are outside prolate domain");
	Throws<std::invalid_argument>([]{ OrientationCosinePdf(1.01,1.0,2.0); },"orientation rejects cosine outside support");
	Throws<std::invalid_argument>([]{ RayleighCosinePdf(-1.01); },"Rayleigh rejects cosine outside support");
	Throws<std::invalid_argument>([]{ ExponentialForwardPolarPdf(1.0,0.0); },"delta has no finite continuous PDF");
	Throws<std::invalid_argument>([]{ ExponentialForwardPolarPdf(-1.0,1.0); },"negative polar angle is rejected");
	Throws<std::invalid_argument>([]{ GeometricEncounterRate(1.0,1.01); },"volume fraction is not a percentage");
	Throws<std::invalid_argument>([]{ RayleighRate(1.0,1.0,-0.1,2.0); },"negative fiber fraction is rejected");
	Throws<std::invalid_argument>([]{ RayleighRate(1.0,1.0,0.0,0.0); },"inactive fiber still rejects zero index ratio");
	Throws<std::invalid_argument>([&]{ RayleighCosinePdf(nan); },"Rayleigh PDF rejects nonfinite cosine");
	Throws<std::invalid_argument>([&]{ IsExponentialForwardDelta(infinity); },"delta query rejects infinite scale");
	Throws<std::invalid_argument>([&]{ OrientationCosinePdf(0.0,1.0,infinity); },"orientation PDF rejects infinite major axis");
	Throws<std::invalid_argument>([]{ SampleExponentialFreePath(-1.0,0.0); },"negative rate is rejected");
	Throws<std::overflow_error>([&]{ ProlateSurfaceToVolume(smallest,smallest); },"unrepresentable S/V reports overflow");
	Throws<std::overflow_error>([&]{ SampleExponentialFreePath(smallest,0.5); },"unrepresentable distance reports overflow");
	Throws<std::underflow_error>([&]{ SampleExponentialFreePath(largest,smallest); },"positive vanishing distance reports underflow");
	Throws<std::overflow_error>([&]{ RayleighRate(largest,smallest,1.0,2.0); },"unrepresentable Rayleigh rate reports overflow");
	Throws<std::underflow_error>([&]{ RayleighRate(smallest,largest,1.0,2.0); },"positive vanishing Rayleigh rate reports underflow");
	Throws<std::underflow_error>([&]{ ExponentialForwardNormalization(smallest); },"unrepresentable positive normalization is explicit");
	// Secondary statistical controls, fixed RNG seeds and bounded trial counts.
	// Every sampler call consumes one proposal. Six standard errors constrain
	// accepted count and moments against independent integrals/analytic moments.
	std::mt19937_64 engine(0x48594c494f53ULL);
	auto uniform = [&]{ return static_cast<double>(engine() >> 11)*0x1.0p-53; };
	constexpr int trials = 200000;
	for (double scale : {0.05,0.5,2.0}) {
		int accepted = 0; double sum = 0.0; double sumSquares = 0.0;
		for (int i=0; i<trials; ++i) {
			const double u1 = uniform(), u2 = uniform(), u3 = uniform();
			const auto sample = TrySampleExponentialForward(scale,u1,u2,u3);
			if (sample) { ++accepted; sum += sample->thetaRadians; sumSquares += sample->thetaRadians*sample->thetaRadians; }
		}
		const double z = Integrate([&](double t){ return std::exp(-t/scale)*std::sin(t); },0.0,Pi);
		const double probability = z/(scale*scale);
		Check(std::abs(accepted-trials*probability) <= 6.0*std::sqrt(trials*probability*(1.0-probability))+1.0,"gamma proposal acceptance matches Z/scale^2 within six sigma");
		const double expected = Integrate([&](double t){ return t*std::exp(-t/scale)*std::sin(t)/z; },0.0,Pi);
		const double second = Integrate([&](double t){ return t*t*std::exp(-t/scale)*std::sin(t)/z; },0.0,Pi);
		const double fourth = Integrate([&](double t){ return t*t*t*t*std::exp(-t/scale)*std::sin(t)/z; },0.0,Pi);
		Check(accepted>0,"bounded forward experiment accepted samples");
		if (accepted>0) {
			Check(std::abs(sum/accepted-expected) <= 6.0*std::sqrt((second-expected*expected)/accepted),"forward accepted mean matches independent quadrature within six sigma");
			Check(std::abs(sumSquares/accepted-second) <= 6.0*std::sqrt((fourth-second*second)/accepted),"forward second moment matches independent quadrature within six sigma");
		}
	}
	int accepted = 0; double sum = 0.0, sumSquares = 0.0;
	for (int i=0; i<trials; ++i) {
		const double proposal = uniform(), accept = uniform();
		const auto sample = TrySampleRayleighCosine(proposal,accept);
		if (sample) { ++accepted; sum += *sample; sumSquares += (*sample)*(*sample); }
	}
	Check(std::abs(accepted-trials*(2.0/3.0)) <= 6.0*std::sqrt(trials*2.0/9.0),"Rayleigh rejection acceptance is 2/3 within six sigma");
	Check(accepted>0,"bounded Rayleigh experiment accepted samples");
	if (accepted>0) {
		Check(std::abs(sum/accepted) <= 6.0*std::sqrt(0.4/accepted),"Rayleigh accepted cosine mean is zero within six sigma");
		Check(std::abs(sumSquares/accepted-0.4) <= 6.0*std::sqrt((9.0/35.0-0.16)/accepted),"Rayleigh accepted second moment is 2/5 within six sigma");
	}
	if (failures) return 1;
	std::cout << "HyLIoSOpticsTest: all checks passed\n";
	return 0;
}
