#include "../src/Library/Materials/HyLIoS/HyLIoSOpticalData.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace RISE::HyLIoS;

namespace {
int failures = 0;
void Check(bool ok, const char* label) { if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
bool Near(double a, double b, double tol = 1e-12) { return std::abs(a-b) <= tol * std::max({1.0, std::abs(a), std::abs(b)}); }
template<class F> void Throws(F f, const char* label) { try { f(); Check(false, label); } catch (const std::exception&) {} }
Provenance Synthetic() { Provenance p; p.availability=Availability::Verified; p.source="TEST ONLY: synthetic analytic control"; p.sourceQuantity="synthetic coefficient"; p.sourceUnits="m^-1"; p.transformation="TEST ONLY: direct literal values"; p.checksum="TEST ONLY: in-test values"; p.uncertainty="TEST ONLY: exact analytic controls"; p.redistribution="not applicable; synthetic test data"; p.label="TEST ONLY: synthetic two-point curve"; p.measuredWavelengthRange="400..600 nm (synthetic)"; p.interpolationRationale="test exercises declared policy, not a fitted dataset"; return p; }
}

int main() {
	// Exact endpoints and linear midpoint oracle; synthetic data are not physiological defaults.
	auto linear = SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear,
		{{400, 2}, {600, 6}}, Synthetic());
	Check(Near(linear.Evaluate(400), 2), "linear curve returns exact first endpoint");
	Check(Near(linear.Evaluate(600), 6), "linear curve returns exact last endpoint");
	Check(Near(linear.Evaluate(500), 4), "linear curve midpoint equals analytic interpolation");
	linear.RequireCoverage(400, 600);
	Throws([&]{ linear.RequireCoverage(399, 600); }, "coverage validation rejects uncovered lower wavelength");
	Throws([&]{ linear.Evaluate(601); }, "lookup rejects extrapolation");
	Throws([&]{ linear.Evaluate(std::strtod("nan", nullptr)); }, "lookup rejects runtime NaN wavelength");

	auto logcurve = SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::LogLinear,
		{{400, 1}, {600, 100}}, Synthetic());
	Check(Near(logcurve.Evaluate(500), 10), "log-linear midpoint is geometric mean");
	Throws([&]{ SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::LogLinear, {{400,0},{600,1}}, Synthetic()); }, "log-linear curve rejects zero coefficient");
	Throws([&]{ SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{400,2}}, Synthetic()); }, "curve rejects duplicate wavelengths");
	Throws([&]{ SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{600,-1}}, Synthetic()); }, "absorption curve rejects negative coefficient");
	Throws([&]{ SpectralCurve::Create(Quantity::RefractiveIndex, Interpolation::Linear, {{400,1},{600,0}}, Synthetic()); }, "IOR curve rejects nonpositive index");
	Throws([&]{ SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{600,std::numeric_limits<double>::infinity()}}, Synthetic()); }, "curve rejects infinity data");

	Provenance absent; absent.label="TEST ONLY: absent synthetic constituent";
	auto missing = SpectralCurve::Missing(Quantity::AbsorptionPerMetre, absent);
	Throws([&]{ missing.RequireCoverage(400, 600); }, "active missing curve fails coverage validation");
	Throws([&]{ missing.Evaluate(500); }, "missing scientific lookup raises explicit diagnostic");

	Check(Near(DecadicExtinctionToInverseMetres(2.0, 0.5), std::log(10.0)*100.0), "decadic extinction applies ln(10), mol/L and cm-to-m once");
	Check(Near(DecadicExtinctionToInverseMetres(1e308, 1e-308), std::log(10.0)*100.0), "decadic conversion preserves finite reciprocal extreme inputs");
	Check(Near(NapierianInverseCentimetresToPerMetre(0.25), 25), "Napierian inverse cm converts to inverse m without ln(10)");
	Check(Near(MeasuredAbsorptanceToInverseMetres(0.5, 2.0), std::log(2.0)/2.0), "measured absorptance uses -log1p(-A)/metres");
	Throws([]{ MeasuredAbsorptanceToInverseMetres(1.0, 1.0); }, "100 percent absorptance is rejected as finite measurement");
	Throws([]{ MeasuredAbsorptanceToInverseMetres(std::nextafter(1.0, 0.0), std::numeric_limits<double>::denorm_min()); }, "absorptance conversion rejects overflow instead of returning infinity");
	Throws([]{ MeasuredAbsorptanceToInverseMetres(0.2, 0.0); }, "absorptance conversion rejects zero path");
	Throws([]{ DecadicExtinctionToInverseMetres(-1, 1); }, "decadic conversion rejects negative extinction");
	Throws([]{ DecadicExtinctionToInverseMetres(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()); }, "decadic conversion detects overflow");
	Throws([]{ NapierianInverseCentimetresToPerMetre(std::numeric_limits<double>::max()); }, "Napierian conversion detects overflow");
	Throws([]{ DecadicExtinctionToInverseMetres(std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::denorm_min()); }, "decadic conversion detects positive underflow");
	Throws([]{ MeasuredAbsorptanceToInverseMetres(std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()); }, "absorptance conversion detects positive underflow");
	Check(DecadicExtinctionToInverseMetres(std::numeric_limits<double>::max(), 0.0) == 0.0, "zero concentration returns exact zero without intermediate overflow");
	Check(MeasuredAbsorptanceToInverseMetres(1e-300, 1.0) > 0.0, "tiny positive absorptance remains numerically resolved by log1p");
	Throws([]{ SpectralCurve::Create(static_cast<Quantity>(99), Interpolation::Linear, {{400,1},{600,2}}, Synthetic()); }, "curve rejects invalid quantity enum");
	Throws([]{ SpectralCurve::Create(Quantity::AbsorptionPerMetre, static_cast<Interpolation>(99), {{400,1},{600,2}}, Synthetic()); }, "curve rejects invalid interpolation enum");
	Throws([]{ auto p=Synthetic(); p.availability=static_cast<Availability>(99); SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{600,2}}, p); }, "curve rejects invalid availability enum");
	Throws([]{ auto p=Synthetic(); p.checksum.clear(); SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{600,2}}, p); }, "active curve rejects incomplete provenance");
	Throws([]{ SpectralCurve::Missing(static_cast<Quantity>(99), {}); }, "missing curve rejects invalid quantity enum");
	auto movedFrom = SpectralCurve::Create(Quantity::AbsorptionPerMetre, Interpolation::Linear, {{400,1},{600,2}}, Synthetic());
	auto movedTo = std::move(movedFrom);
	Check(Near(movedTo.Evaluate(500), 1.5), "moved-to curve retains valid data");
	Throws([&]{ movedFrom.Evaluate(500); }, "moved-from curve fails explicitly");
	Throws([&]{ movedFrom.RequireCoverage(400, 600); }, "moved-from curve coverage fails explicitly");
	if (failures) return 1;
	std::cout << "HyLIoSOpticalDataTest: all checks passed\n";
	return 0;
}
