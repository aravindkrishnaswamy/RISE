#include "pch.h"
#include "HyLIoSOptics.h"

#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace RISE::HyLIoS {
namespace {
constexpr double Pi = 3.141592653589793238462643383279502884;
void Finite(double x, const char* name) {
	if (!std::isfinite(x)) throw std::invalid_argument(std::string(name) + " must be finite");
}
void Positive(double x, const char* name) {
	Finite(x, name);
	if (x <= 0.0) throw std::invalid_argument(std::string(name) + " must be positive");
}
void Nonnegative(double x, const char* name) {
	Finite(x, name);
	if (x < 0.0) throw std::invalid_argument(std::string(name) + " must be nonnegative");
}
void Fraction(double x, const char* name) {
	Finite(x, name);
	if (x < 0.0 || x > 1.0) throw std::invalid_argument(std::string(name) + " must lie in [0,1]");
}
void Uniform(double x) {
	Finite(x, "uniform variate");
	if (x < 0.0 || x >= 1.0) throw std::invalid_argument("uniform variate must lie in [0,1)");
}
void Cosine(double x) {
	Finite(x, "cosine");
	if (x < -1.0 || x > 1.0) throw std::invalid_argument("cosine must lie in [-1,1]");
}
double AxisRatio(double a, double b) {
	Positive(a, "semi-minor axis"); Positive(b, "semi-major axis");
	if (a > b) throw std::invalid_argument("prolate semi-major axis must be at least semi-minor axis");
	return a/b;
}
// Maintain a separate binary exponent so intermediate powers/ratios cannot
// overflow or vanish before reciprocal dimensions compensate them.
double ProductRatio(std::initializer_list<double> numerator, std::initializer_list<double> denominator) {
	double fraction = 1.0;
	int exponent = 0;
	for (double x : numerator) {
		if (x == 0.0) return 0.0;
		int power = 0;
		fraction *= std::frexp(x, &power); exponent += power;
		fraction = std::frexp(fraction, &power); exponent += power;
	}
	for (double x : denominator) {
		int power = 0;
		fraction /= std::frexp(x, &power); exponent -= power;
		fraction = std::frexp(fraction, &power); exponent += power;
	}
	const double result = std::ldexp(fraction, exponent);
	if (!std::isfinite(result)) throw std::overflow_error("HyLIoS scalar result overflowed");
	if (result == 0.0) throw std::underflow_error("HyLIoS positive scalar result underflowed");
	return result;
}
double LogForwardNormalization(double scale) {
	const double tail = std::exp(-Pi/scale);
	if (scale <= 1.0) return 2.0*std::log(scale) - std::log1p(scale*scale) + std::log1p(tail);
	const double inverse = 1.0/scale;
	return std::log1p(tail) - std::log1p(inverse*inverse);
}
}

double ProlateSurfaceToVolume(double a, double b) {
	const double ratio = AxisRatio(a,b);
	// (1-r)(1+r) avoids cancellation in 1-r*r near the sphere.
	const double eccentricity = std::sqrt((1.0-ratio)*(1.0+ratio));
	// asin(e) loses the complementary angle when a slender spheroid's e
	// rounds toward one. atan2 retains that angle from the independent ratio.
	const double sincInverse = eccentricity == 0.0 ? 1.0 : std::atan2(eccentricity,ratio)/eccentricity;
	return ProductRatio({1.5, ratio+sincInverse}, {a});
}
double GeometricEncounterRate(double surfaceToVolume, double volume) {
	Nonnegative(surfaceToVolume, "surface-to-volume ratio"); Fraction(volume, "volume fraction");
	return ProductRatio({surfaceToVolume, volume}, {4.0});
}
double OrientationCosinePdf(double cosine, double a, double b) {
	Cosine(cosine);
	const double ratio = AxisRatio(a,b);
	// Factored numerator also preserves the endpoint value at small a/b.
	const double magnitude = std::abs(cosine);
	return ((1.0-magnitude)+ratio*magnitude)/(1.0+ratio);
}
OrientationSample SampleOrientation(double uniform, double a, double b) {
	Uniform(uniform);
	const double ratio = AxisRatio(a,b);
	if (uniform == 0.0) return {-1.0, 0.0};
	const double signedMass = 2.0*uniform-1.0;
	const double mass = std::abs(signedMass);
	// C=2*min(u,1-u) retains tiny lower-tail probabilities. hypot also
	// retains a tiny ratio whose square would underflow before adding C.
	const double complement = 2.0*(uniform <= 0.5 ? uniform : 1.0-uniform);
	const double root = std::hypot(std::sqrt(complement),ratio*std::sqrt(mass));
	const double magnitude = mass*(1.0+ratio)/(1.0+root);
	// Rationalizing 1-|cos(alpha)| gives delta=C*(1+r)/(root+r).
	// Its square root must precede multiplication: delta itself can round
	// at subnormal precision while the transverse radius remains normal.
	const double gain = (1.0+ratio)/(root+ratio);
	const double delta = complement*gain;
	// Materialize these square roots so full-LTO fast-math cannot fold them
	// into sqrt(C*gain), reintroducing that subnormal intermediate.
	volatile double rootComplement = std::sqrt(complement);
	volatile double rootGain = std::sqrt(gain);
	const double radius = rootComplement*rootGain*std::sqrt(2.0-delta);
	return {std::copysign(magnitude, signedMass), radius};
}
double SampleOrientationCosine(double uniform, double a, double b) {
	return SampleOrientation(uniform,a,b).cosine;
}
std::optional<double> SampleExponentialFreePath(double rate, double uniform) {
	Nonnegative(rate, "event rate"); Uniform(uniform);
	if (rate == 0.0) return std::nullopt;
	if (uniform == 0.0) return 0.0;
	return ProductRatio({-std::log1p(-uniform)}, {rate});
}
bool IsExponentialForwardDelta(double scale) {
	Nonnegative(scale, "angular scale");
	return scale == 0.0;
}
double ExponentialForwardNormalization(double scale) {
	Positive(scale, "continuous angular scale");
	const double tail = 1.0+std::exp(-Pi/scale);
	if (scale <= 1.0) return ProductRatio({scale, scale, tail}, {1.0+scale*scale});
	const double inverse = 1.0/scale;
	return tail/(1.0+inverse*inverse);
}
double ExponentialForwardPolarPdf(double theta, double scale) {
	Positive(scale, "continuous angular scale"); Finite(theta, "polar angle");
	if (theta < 0.0 || theta > Pi) throw std::invalid_argument("polar angle must lie in [0,pi]");
	if (theta == 0.0 || theta == Pi) return 0.0;
	const double result = std::exp(-theta/scale + std::log(std::sin(theta)) - LogForwardNormalization(scale));
	if (!std::isfinite(result)) throw std::overflow_error("forward polar density overflowed");
	return result;
}
std::optional<PolarSample> TrySampleExponentialForward(double scale, double u1, double u2, double accept) {
	Nonnegative(scale, "angular scale"); Uniform(u1); Uniform(u2); Uniform(accept);
	if (scale == 0.0) return PolarSample{0.0, true};
	const double gamma = -std::log1p(-u1)-std::log1p(-u2);
	// Compare before multiplying, so a huge scale rejects a beyond-pi
	// proposal rather than producing infinity. No internal retry loop.
	if (gamma >= Pi/scale) return std::nullopt;
	const double theta = gamma == 0.0 ? 0.0 : ProductRatio({scale, gamma}, {});
	if (theta >= Pi) return std::nullopt;
	const double acceptance = theta == 0.0 ? 1.0 : std::sin(theta)/theta;
	if (accept > acceptance) return std::nullopt;
	return PolarSample{theta, false};
}
double RayleighRate(double radius, double wavelength, double volume, double eta) {
	Positive(radius, "fiber radius"); Positive(wavelength, "wavelength");
	Fraction(volume, "fiber volume fraction"); Positive(eta, "relative refractive index");
	if (volume == 0.0 || eta == 1.0) return 0.0;
	const double q = eta > 1.0 ? 1.0/eta : eta;
	const double contrastMagnitude = ((1.0-q)*(1.0+q))/(1.0+q*q);
	return ProductRatio({32.0, Pi, Pi, Pi, Pi, radius, radius, radius, volume,
		contrastMagnitude, contrastMagnitude}, {wavelength, wavelength, wavelength, wavelength});
}
double RayleighCosinePdf(double cosine) {
	Cosine(cosine);
	return (3.0/8.0)*(1.0+cosine*cosine);
}
std::optional<double> TrySampleRayleighCosine(double uniform, double accept) {
	Uniform(uniform); Uniform(accept);
	const double cosine = 2.0*uniform-1.0;
	if (accept > (1.0+cosine*cosine)/2.0) return std::nullopt;
	return cosine;
}
} // namespace RISE::HyLIoS
