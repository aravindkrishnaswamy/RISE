#pragma once

#include <string>
#include <vector>

namespace RISE::HyLIoS {

enum class Availability { Verified, Digitized, Substituted, Missing };
enum class Interpolation { Linear, LogLinear };
enum class Quantity { AbsorptionPerMetre, RefractiveIndex };

struct Provenance {
	Availability availability = Availability::Missing;
	std::string source;
	std::string sourceQuantity;
	std::string sourceUnits;
	std::string transformation;
	std::string checksum;
	std::string uncertainty;
	std::string redistribution;
	std::string label;
	// Optional source context. Empty means unavailable; no value is inferred.
	std::string referenceConcentration;
	std::string referencePathLength;
	std::string temperature;
	std::string measuredWavelengthRange;
	std::string interpolationRationale;
};

struct Sample { double wavelengthNm; double value; };

// Immutable validated curve. Missing curves carry provenance only and cannot be evaluated.
class SpectralCurve {
public:
	static SpectralCurve Create(Quantity quantity, Interpolation interpolation,
		std::vector<Sample> samples, Provenance provenance);
	static SpectralCurve Missing(Quantity quantity, Provenance provenance);

	double Evaluate(double wavelengthNm) const;
	void RequireCoverage(double minimumNm, double maximumNm) const;
	Availability availability() const noexcept { return provenance_.availability; }
	Quantity quantity() const noexcept { return quantity_; }
	Interpolation interpolation() const noexcept { return interpolation_; }
	const Provenance& provenance() const noexcept { return provenance_; }
	double minimumWavelengthNm() const;
	double maximumWavelengthNm() const;

private:
	SpectralCurve(Quantity q, Interpolation i, std::vector<Sample> s, Provenance p);
	Quantity quantity_;
	Interpolation interpolation_;
	std::vector<Sample> samples_;
	Provenance provenance_;
};

// epsilon is decadic molar extinction in L mol^-1 cm^-1; concentration is mol/L.
// Results use binary64 rounding. Reject nonfinite results and positive
// conversions that round to zero. Nonzero subnormal rounding is permitted;
// these helpers do not promise relative accuracy in the subnormal range.
double DecadicExtinctionToInverseMetres(double epsilonLPerMolCm, double concentrationMolPerL);
double NapierianInverseCentimetresToPerMetre(double inverseCentimetres);
double MeasuredAbsorptanceToInverseMetres(double absorptanceFraction, double pathLengthMetres);
} // namespace RISE::HyLIoS
