#include "pch.h"
#include "HyLIoSOpticalData.h"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace RISE::HyLIoS {
namespace {
void RequireFinite(double x, const char* what) {
	if (!std::isfinite(x)) throw std::invalid_argument(std::string(what) + " must be finite");
}
void RequireNonnegative(double x, const char* what) {
	RequireFinite(x, what);
	if (x < 0.0) throw std::invalid_argument(std::string(what) + " must be nonnegative");
}

// A wavelength ratio may underflow even when its product with a coefficient
// is representable. Keep exponents separate until the complete expression.
double ScaledProductQuotient(double value, double numerator, double denominator) {
	if (value == 0.0 || numerator == 0.0) return 0.0;
	int ve = 0, ne = 0, de = 0;
	const double vm = std::frexp(value, &ve);
	const double nm = std::frexp(numerator, &ne);
	const double dm = std::frexp(denominator, &de);
	return std::ldexp((vm * nm) / dm, ve + ne - de);
}
}

SpectralCurve::SpectralCurve(Quantity q, Interpolation i, std::vector<Sample> s, Provenance p)
	: quantity_(q), interpolation_(i), samples_(std::move(s)), provenance_(std::move(p)) {}

SpectralCurve SpectralCurve::Create(Quantity q, Interpolation i, std::vector<Sample> s, Provenance p) {
	if (q != Quantity::AbsorptionPerMetre && q != Quantity::RefractiveIndex) throw std::invalid_argument("unknown spectral quantity");
	if (i != Interpolation::Linear && i != Interpolation::LogLinear) throw std::invalid_argument("unknown interpolation policy");
	if (p.availability != Availability::Verified && p.availability != Availability::Digitized && p.availability != Availability::Substituted && p.availability != Availability::Missing) throw std::invalid_argument("unknown availability status");
	if (p.availability == Availability::Missing) throw std::invalid_argument("missing data must use SpectralCurve::Missing");
	if (p.source.empty() || p.sourceQuantity.empty() || p.sourceUnits.empty() || p.transformation.empty() || p.checksum.empty() || p.uncertainty.empty() || p.redistribution.empty() || p.label.empty())
		throw std::invalid_argument("active curve provenance requires source, quantity, units, transformation, checksum, uncertainty, redistribution status and label");
	if (s.size() < 2) throw std::invalid_argument("active spectral curve requires at least two samples");
	for (std::size_t n = 0; n < s.size(); ++n) {
		RequireFinite(s[n].wavelengthNm, "wavelength");
		if (s[n].wavelengthNm <= 0.0) throw std::invalid_argument("wavelength must be positive");
		if (n && !(s[n].wavelengthNm > s[n-1].wavelengthNm)) throw std::invalid_argument("wavelengths must be strictly increasing");
		if (q == Quantity::AbsorptionPerMetre) RequireNonnegative(s[n].value, "absorption coefficient");
		else { RequireFinite(s[n].value, "refractive index"); if (s[n].value <= 0.0) throw std::invalid_argument("refractive index must be positive"); }
		if (i == Interpolation::LogLinear && s[n].value <= 0.0) throw std::invalid_argument("log-linear interpolation requires positive values");
	}
	return SpectralCurve(q, i, std::move(s), std::move(p));
}

SpectralCurve SpectralCurve::Missing(Quantity q, Provenance p) {
	if (q != Quantity::AbsorptionPerMetre && q != Quantity::RefractiveIndex) throw std::invalid_argument("unknown spectral quantity");
	p.availability = Availability::Missing;
	return SpectralCurve(q, Interpolation::Linear, {}, std::move(p));
}

double SpectralCurve::Evaluate(double wavelengthNm) const {
	RequireFinite(wavelengthNm, "requested wavelength");
	if (provenance_.availability == Availability::Missing) throw std::runtime_error("scientific lookup failed: active optical data is missing (" + provenance_.label + ")");
	if (samples_.size() < 2) throw std::runtime_error("spectral curve has no usable samples");
	if (wavelengthNm < samples_.front().wavelengthNm || wavelengthNm > samples_.back().wavelengthNm) throw std::out_of_range("wavelength outside measured spectral coverage");
	const auto upper = std::lower_bound(samples_.begin(), samples_.end(), wavelengthNm,
		[](const Sample& sample, double wavelength) { return sample.wavelengthNm < wavelength; });
	if (upper != samples_.end() && wavelengthNm == upper->wavelengthNm) return upper->value;
	if (upper == samples_.begin() || upper == samples_.end()) throw std::logic_error("validated spectral interpolation interval was not found");
	const auto& a = *(upper - 1); const auto& b = *upper;
	const double left = wavelengthNm - a.wavelengthNm;
	const double right = b.wavelengthNm - wavelengthNm;
	const double span = b.wavelengthNm - a.wavelengthNm;
	const double av = interpolation_ == Interpolation::Linear ? a.value : std::log(a.value);
	const double bv = interpolation_ == Interpolation::Linear ? b.value : std::log(b.value);
	// Use the nearer endpoint to avoid cancellation of a nonnegative value.
	// Equal endpoints remain exact, including constant subnormal curves.
	const double interpolated = left <= right
		? av + ScaledProductQuotient(bv - av, left, span)
		: bv + ScaledProductQuotient(av - bv, right, span);
	const double result = interpolation_ == Interpolation::Linear ? interpolated : std::exp(interpolated);
	if (!std::isfinite(result)) throw std::overflow_error("spectral interpolation overflowed");
	return result;
}

void SpectralCurve::RequireCoverage(double lo, double hi) const {
	RequireFinite(lo, "coverage minimum"); RequireFinite(hi, "coverage maximum");
	if (lo > hi) throw std::invalid_argument("coverage minimum exceeds maximum");
	if (provenance_.availability == Availability::Missing) throw std::runtime_error("active constituent has missing optical data: " + provenance_.label);
	if (samples_.size() < 2) throw std::runtime_error("active spectral curve has no usable samples");
	if (lo < minimumWavelengthNm() || hi > maximumWavelengthNm()) {
		std::ostringstream msg; msg << "active optical curve covers " << minimumWavelengthNm() << ".." << maximumWavelengthNm() << " nm, requested " << lo << ".." << hi << " nm";
		throw std::out_of_range(msg.str());
	}
}

double SpectralCurve::minimumWavelengthNm() const { if (samples_.empty()) throw std::runtime_error("curve has no wavelength coverage"); return samples_.front().wavelengthNm; }
double SpectralCurve::maximumWavelengthNm() const { if (samples_.empty()) throw std::runtime_error("curve has no wavelength coverage"); return samples_.back().wavelengthNm; }

double DecadicExtinctionToInverseMetres(double epsilon, double concentration) {
	RequireNonnegative(epsilon, "decadic extinction"); RequireNonnegative(concentration, "concentration");
	if (epsilon == 0.0 || concentration == 0.0) return 0.0;
	int epsilonExponent = 0, concentrationExponent = 0, factorExponent = 0;
	const double epsilonFraction = std::frexp(epsilon, &epsilonExponent);
	const double concentrationFraction = std::frexp(concentration, &concentrationExponent);
	const double factorFraction = std::frexp(100.0 * std::log(10.0), &factorExponent);
	const int exponent = epsilonExponent + concentrationExponent + factorExponent;
	const double fraction = epsilonFraction * concentrationFraction * factorFraction;
	const double result = std::ldexp(fraction, exponent);
	if (!std::isfinite(result)) throw std::overflow_error("decadic extinction conversion overflowed inverse metres");
	if (result == 0.0) throw std::underflow_error("decadic extinction conversion underflowed inverse metres");
	return result;
}
double NapierianInverseCentimetresToPerMetre(double x) {
	RequireNonnegative(x, "inverse-centimetre coefficient");
	if (x == 0.0) return 0.0;
	const double result = x * 100.0;
	if (!std::isfinite(result)) throw std::overflow_error("Napierian coefficient conversion overflowed inverse metres");
	if (result == 0.0) throw std::underflow_error("Napierian coefficient conversion underflowed inverse metres");
	return result;
}
double MeasuredAbsorptanceToInverseMetres(double a, double path) {
	RequireFinite(a, "absorptance"); RequireFinite(path, "path length");
	if (a < 0.0 || a >= 1.0) throw std::invalid_argument("absorptance must be in [0,1); 100% is censored, not finite data");
	if (path <= 0.0) throw std::invalid_argument("path length must be positive");
	const double result = -std::log1p(-a)/path;
	if (!std::isfinite(result)) throw std::overflow_error("absorptance conversion overflowed inverse metres");
	if (a > 0.0 && result == 0.0) throw std::underflow_error("absorptance conversion underflowed inverse metres");
	return result;
}
} // namespace RISE::HyLIoS
