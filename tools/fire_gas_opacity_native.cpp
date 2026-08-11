// Native streaming accumulator for production-scale HITEMP archives.
// The Python front end owns manifests, hashes, bzip2 streaming, and record
// emission; this translation unit owns the line-count-proportional hot loop.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr double kC2 = 1.4387768775039338;
constexpr double kBoltzmann = 1.380649e-23;
constexpr double kLight = 299792458.0;
constexpr double kAvogadro = 6.02214076e23;
constexpr double kReferenceTemperature = 296.0;
constexpr double kReferencePressure = 101325.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

constexpr double kNodes[16] = {
    0.048307665687738316, 0.14447196158279649, 0.23928736225213707,
    0.33186860228212765, 0.42135127613063535, 0.5068999089322294,
    0.5877157572407623, 0.6630442669302152, 0.7321821187402897,
    0.7944837959679424, 0.8482065834104272, 0.8963211557660521,
    0.9349060759377397, 0.9647622555875064, 0.9856115115452684,
    0.9972638618494816};
constexpr double kWeights[16] = {
    0.0965400885147278, 0.09563872007927486, 0.09384439908080457,
    0.09117387869576388, 0.08765209300440381, 0.08331192422694676,
    0.07819389578707031, 0.07234579410884851, 0.06582222277636185,
    0.05868409347853555, 0.050998059262376176, 0.04283589802222668,
    0.03427386291302143, 0.02539206530926206, 0.01627439473090567,
    0.007018610009470097};

void SetError(char* error, std::size_t capacity, const char* message)
{
    if (error && capacity) {
        std::snprintf(error, capacity, "%s", message);
    }
}

bool ParseDouble(const char* text, std::size_t length, double& value)
{
    if (length >= 32) {
        return false;
    }
    char buffer[32];
    std::memcpy(buffer, text, length);
    buffer[length] = '\0';
    for (std::size_t i = 0; i < length; ++i) {
        if (buffer[i] == 'D' || buffer[i] == 'd') {
            buffer[i] = 'E';
        }
    }
    char* end = nullptr;
    value = std::strtod(buffer, &end);
    while (end && *end == ' ') {
        ++end;
    }
    return end && *end == '\0' && std::isfinite(value);
}

double PlanckWeight(double wavenumber, double temperature)
{
    const double exponent = kC2 * wavenumber / temperature;
    return exponent > 700.0 ? 0.0 : wavenumber * wavenumber * wavenumber /
        std::expm1(exponent);
}

double LorentzInterval(double lower, double upper, double gamma, double shift)
{
    return (std::atan((upper - shift) / gamma) -
            std::atan((lower - shift) / gamma)) / kPi;
}

template <typename Function>
double LegendreSegment(Function&& function, double lower, double upper)
{
    const double midpoint = 0.5 * (lower + upper);
    const double halfWidth = 0.5 * (upper - lower);
    double total = 0.0;
    for (int i = 0; i < 16; ++i) {
        total += kWeights[i] *
            (function(midpoint + halfWidth * kNodes[i]) +
             function(midpoint - halfWidth * kNodes[i]));
    }
    return halfWidth * total;
}

double VoigtInterval(double lower, double upper, double sigma, double gamma)
{
    if (gamma == 0.0) {
        const double scale = sigma * std::sqrt(2.0);
        return 0.5 * (std::erf(upper / scale) - std::erf(lower / scale));
    }
    if (sigma == 0.0) {
        return LorentzInterval(lower, upper, gamma, 0.0);
    }
    const bool boundaryNearCore =
        std::min(std::abs(lower), std::abs(upper)) <= 6.0 * sigma;
    if (gamma <= sigma && boundaryNearCore) {
        const double scale = sigma * std::sqrt(2.0);
        const auto integrand = [&](double theta) {
            const double shift = gamma * std::tan(theta);
            return 0.5 * (std::erf((upper - shift) / scale) -
                          std::erf((lower - shift) / scale)) / kPi;
        };
        const double points[4] = {
            -0.5 * kPi, std::atan(lower / gamma),
            std::atan(upper / gamma), 0.5 * kPi};
        return LegendreSegment(integrand, points[0], points[1]) +
            LegendreSegment(integrand, points[1], points[2]) +
            LegendreSegment(integrand, points[2], points[3]);
    }
    const double scale = 8.0 * sigma;
    const double normalization = scale / (sigma * std::sqrt(2.0 * kPi));
    double total = 0.0;
    for (int i = 0; i < 16; ++i) {
        const double shift = scale * kNodes[i];
        total += kWeights[i] * normalization *
            std::exp(-0.5 * (shift / sigma) * (shift / sigma)) *
            (LorentzInterval(lower, upper, gamma, shift) +
             LorentzInterval(lower, upper, gamma, -shift));
    }
    return total;
}

double VoigtIntervalUpper(double distance, double sigma, double gamma)
{
    if (sigma == 0.0) {
        return gamma / (kPi * (distance * distance + gamma * gamma));
    }
    const double gaussianPeak = 1.0 / (sigma * std::sqrt(2.0 * kPi));
    if (gamma == 0.0) {
        return gaussianPeak * std::exp(-0.5 * (distance / sigma) *
                                      (distance / sigma));
    }
    const double lorentzPeak = 1.0 / (kPi * gamma);
    if (distance == 0.0) {
        return std::min(gaussianPeak, lorentzPeak);
    }
    const double radius = std::min(0.5 * distance, 8.0 * sigma);
    const double tail = std::erfc(radius / (std::sqrt(2.0) * sigma));
    const double nearDistance = std::max(0.0, distance - radius);
    const double nearLorentz = gamma /
        (kPi * (nearDistance * nearDistance + gamma * gamma));
    return std::min({gaussianPeak, lorentzPeak,
                     tail * lorentzPeak + nearLorentz});
}

double GaussianDensityIntervalUpper(double distance, double sigmaMinimum,
                                    double sigmaMaximum)
{
    const double candidate = std::min(sigmaMaximum,
        std::max(sigmaMinimum, distance));
    return std::exp(-0.5 * (distance / candidate) * (distance / candidate)) /
        (candidate * std::sqrt(2.0 * kPi));
}

double VoigtStateCellUpper(double distance, double shiftedCenter,
    double molecularMass, double temperatureMinimum, double temperatureMaximum,
    double selfMinimum, double selfMaximum, double gammaAir, double gammaSelf,
    double exponent, double pressure)
{
    const double sigmaMinimum = shiftedCenter * std::sqrt(kBoltzmann *
        temperatureMinimum / (molecularMass * kLight * kLight));
    const double sigmaMaximum = shiftedCenter * std::sqrt(kBoltzmann *
        temperatureMaximum / (molecularMass * kLight * kLight));
    const auto referenceWidth = [&](double selfFraction) {
        return (1.0 - selfFraction) * gammaAir + selfFraction * gammaSelf;
    };
    const double widthMinimum = std::min(referenceWidth(selfMinimum),
                                          referenceWidth(selfMaximum));
    const double widthMaximum = std::max(referenceWidth(selfMinimum),
                                          referenceWidth(selfMaximum));
    const double factorMinimum = std::min(
        std::pow(kReferenceTemperature / temperatureMinimum, exponent),
        std::pow(kReferenceTemperature / temperatureMaximum, exponent));
    const double temperatureFactorMaximum = std::max(
        std::pow(kReferenceTemperature / temperatureMinimum, exponent),
        std::pow(kReferenceTemperature / temperatureMaximum, exponent));
    const double gammaMinimum = widthMinimum * pressure / kReferencePressure *
        factorMinimum;
    const double gammaMaximum = widthMaximum * pressure / kReferencePressure *
        temperatureFactorMaximum;
    const double gaussianPeak = 1.0 / (sigmaMinimum * std::sqrt(2.0 * kPi));
    if (distance == 0.0) return gaussianPeak;
    const double radius = 0.5 * distance;
    const double cauchyTail = gammaMaximum == 0.0 ? 0.0 :
        1.0 - 2.0 / kPi * std::atan(radius / gammaMaximum);
    double result = std::min(gaussianPeak, cauchyTail * gaussianPeak +
        GaussianDensityIntervalUpper(distance - radius,
                                     sigmaMinimum, sigmaMaximum));
    if (gammaMinimum > 0.0) {
        result = std::min(result, 1.0 / (kPi * gammaMinimum));
        const double gaussianRadius = std::min(radius, 8.0 * sigmaMaximum);
        const double gaussianTail = std::erfc(gaussianRadius /
            (std::sqrt(2.0) * sigmaMaximum));
        const double nearDistance = distance - gaussianRadius;
        const double candidateGamma = std::min(gammaMaximum,
            std::max(gammaMinimum, nearDistance));
        const double nearLorentz = candidateGamma /
            (kPi * (nearDistance * nearDistance + candidateGamma * candidateGamma));
        result = std::min(result,
            gaussianTail / (kPi * gammaMinimum) + nearLorentz);
    }
    return result;
}

} // namespace

extern "C" int RiseFireGasOpacityAccumulate(
    const char* bytes, std::size_t byteCount, int molecule,
    const double* masses, const double* qReference, const double* qAtTemperature,
    const double* qMinimumInTemperatureCell,
    int isotopeCapacity, const double* temperatures, int temperatureCount,
    double pressure, const double* selfFractions, int selfCount,
    const double* cutoffs, int cutoffCount, double gridMinimum, double gridStep,
    int gridCount, const double* radiationTemperatures, int radiationCount,
    double visibleMinimum, double visibleMaximum, double* spectra,
    std::uint64_t* counts, double* tailBounds, double* centerNumerators,
    double* visibleBounds, double* visibleCellBounds,
    char* error, std::size_t errorCapacity)
{
    if (!bytes || !masses || !qReference || !qAtTemperature ||
        !qMinimumInTemperatureCell || !temperatures ||
        !selfFractions || !cutoffs || !radiationTemperatures || !spectra ||
        !counts || !tailBounds || !centerNumerators || !visibleBounds ||
        !visibleCellBounds ||
        molecule <= 0 || isotopeCapacity <= 1 || temperatureCount <= 0 ||
        selfCount <= 0 || cutoffCount <= 0 || gridCount <= 1 ||
        radiationCount <= 0 || pressure <= 0.0 || gridMinimum <= 0.0 ||
        gridStep <= 0.0) {
        SetError(error, errorCapacity, "native opacity configuration is invalid");
        return 1;
    }
    const double pressureAtmospheres = pressure / kReferencePressure;
    std::size_t cursor = 0;
    while (cursor < byteCount) {
        std::size_t end = cursor;
        while (end < byteCount && bytes[end] != '\n') {
            ++end;
        }
        std::size_t length = end - cursor;
        if (length && bytes[cursor + length - 1] == '\r') {
            --length;
        }
        if (length == 0) {
            cursor = end + (end < byteCount ? 1 : 0);
            continue;
        }
        if (length != 160) {
            SetError(error, errorCapacity, "native HITRAN record is not 160 bytes");
            return 2;
        }
        const char* line = bytes + cursor;
        double moleculeField = 0.0;
        if (!ParseDouble(line, 2, moleculeField)) {
            SetError(error, errorCapacity, "native HITRAN molecule is invalid");
            return 3;
        }
        const int lineMolecule = static_cast<int>(moleculeField);
        int isotope = -1;
        if (line[2] >= '1' && line[2] <= '9') isotope = line[2] - '0';
        else if (line[2] == '0') isotope = 10;
        else if (line[2] == 'A') isotope = 11;
        else if (line[2] == 'B') isotope = 12;
        double center = 0.0, intensity = 0.0, einsteinA = 0.0;
        double gammaAir = 0.0, gammaSelf = 0.0;
        double lowerEnergy = 0.0, exponent = 0.0, shift = 0.0;
        if (moleculeField != lineMolecule || lineMolecule != molecule ||
            isotope <= 0 || isotope >= isotopeCapacity ||
            !ParseDouble(line + 3, 12, center) ||
            !ParseDouble(line + 15, 10, intensity) ||
            !ParseDouble(line + 25, 10, einsteinA) ||
            !ParseDouble(line + 35, 5, gammaAir) ||
            !ParseDouble(line + 40, 5, gammaSelf) ||
            !ParseDouble(line + 45, 10, lowerEnergy) ||
            !ParseDouble(line + 55, 4, exponent) ||
            !ParseDouble(line + 59, 8, shift) || center <= 0.0 || intensity < 0.0 ||
            gammaAir < 0.0 || gammaSelf < 0.0 || lowerEnergy < 0.0 ||
            masses[isotope] <= 0.0 || qReference[isotope] <= 0.0) {
            SetError(error, errorCapacity, "native HITRAN record is inadmissible");
            return 3;
        }
        cursor = end + (end < byteCount ? 1 : 0);
        if (intensity == 0.0) continue;
        const double shiftedCenter = center + shift * pressureAtmospheres;
        if (shiftedCenter <= 0.0) {
            SetError(error, errorCapacity,
                     "native pressure-shifted line center is non-positive");
            return 3;
        }
        const double largestCutoff = cutoffs[cutoffCount - 1];
        const bool contributesToSpectralTable = !(
            shiftedCenter + largestCutoff < gridMinimum ||
            shiftedCenter - largestCutoff > gridMinimum + gridStep * (gridCount - 1));
        const double molecularMass = masses[isotope] / kAvogadro;
        const double visibleDistance = shiftedCenter < visibleMinimum ?
            visibleMinimum - shiftedCenter :
            (shiftedCenter > visibleMaximum ? shiftedCenter - visibleMaximum : 0.0);
        const int temperatureCellCount = std::max(1, temperatureCount - 1);
        const int selfCellCount = std::max(1, selfCount - 1);
        for (int sCell = 0; sCell < selfCellCount; ++sCell) {
            const double selfMinimum = selfFractions[sCell];
            const double selfMaximum = selfCount > 1 ?
                selfFractions[sCell + 1] : selfMinimum;
            for (int tCell = 0; tCell < temperatureCellCount; ++tCell) {
                const double temperatureMinimum = temperatures[tCell];
                const double temperatureMaximum = temperatureCount > 1 ?
                    temperatures[tCell + 1] : temperatureMinimum;
                const double qMinimum = qMinimumInTemperatureCell[
                    tCell * isotopeCapacity + isotope];
                if (qMinimum <= 0.0) {
                    SetError(error, errorCapacity,
                             "native partition-sum cell bound is unavailable");
                    return 4;
                }
                const double boltzmannUpper = std::exp(-kC2 * lowerEnergy *
                    (1.0 / temperatureMaximum - 1.0 / kReferenceTemperature));
                const double stimulatedUpper =
                    -std::expm1(-kC2 * center / temperatureMinimum);
                const double stimulatedReference =
                    -std::expm1(-kC2 * center / kReferenceTemperature);
                const double strengthUpper = intensity * qReference[isotope] /
                    qMinimum * boltzmannUpper * stimulatedUpper / stimulatedReference;
                const double numberDensityUpper = pressure /
                    (kBoltzmann * temperatureMinimum) / 1.0e6;
                const double lineAreaUpper = strengthUpper * numberDensityUpper * 100.0;
                visibleCellBounds[sCell * temperatureCellCount + tCell] +=
                    lineAreaUpper * VoigtStateCellUpper(
                        visibleDistance, shiftedCenter, molecularMass,
                        temperatureMinimum, temperatureMaximum,
                        selfMinimum, selfMaximum, gammaAir, gammaSelf,
                        exponent, pressure);
            }
        }
        for (int t = 0; t < temperatureCount; ++t) {
            const double temperature = temperatures[t];
            const double qT = qAtTemperature[t * isotopeCapacity + isotope];
            if (qT <= 0.0) {
                SetError(error, errorCapacity, "native partition sum is unavailable");
                return 4;
            }
            const double boltzmann = std::exp(-kC2 * lowerEnergy *
                (1.0 / temperature - 1.0 / kReferenceTemperature));
            const double stimulated = -std::expm1(-kC2 * center / temperature);
            const double stimulatedReference =
                -std::expm1(-kC2 * center / kReferenceTemperature);
            const double strength = intensity * (qReference[isotope] / qT) *
                boltzmann * stimulated / stimulatedReference;
            const double numberDensity = pressure / (kBoltzmann * temperature) / 1.0e6;
            const double lineArea = strength * numberDensity * 100.0;
            const double sigma = shiftedCenter * std::sqrt(kBoltzmann * temperature /
                (molecularMass * kLight * kLight));
            if (contributesToSpectralTable) {
                for (int r = 0; r < radiationCount; ++r) {
                    centerNumerators[t * radiationCount + r] +=
                        lineArea * PlanckWeight(shiftedCenter, radiationTemperatures[r]);
                }
            }
            for (int s = 0; s < selfCount; ++s) {
                const double referenceWidth =
                    (1.0 - selfFractions[s]) * gammaAir + selfFractions[s] * gammaSelf;
                const double gamma = referenceWidth * pressureAtmospheres *
                    std::pow(kReferenceTemperature / temperature, exponent);
                if (gamma == 0.0 && sigma == 0.0) {
                    SetError(error, errorCapacity, "native line width is zero");
                    return 5;
                }
                visibleBounds[s * temperatureCount + t] +=
                    lineArea * VoigtIntervalUpper(visibleDistance, sigma, gamma);
                if (!contributesToSpectralTable) continue;
                for (int c = 0; c < cutoffCount; ++c) {
                    const double cutoff = cutoffs[c];
                    const int first = std::max(0, static_cast<int>(std::ceil(
                        (shiftedCenter - cutoff - 0.5 * gridStep - gridMinimum) / gridStep)));
                    const int last = std::min(gridCount - 1, static_cast<int>(std::floor(
                        (shiftedCenter + cutoff + 0.5 * gridStep - gridMinimum) / gridStep)));
                    if (first > last) continue;
                    bool used = false;
                    for (int bin = first; bin <= last; ++bin) {
                        const double lower = std::max(gridMinimum + bin * gridStep -
                            0.5 * gridStep, shiftedCenter - cutoff) - shiftedCenter;
                        const double upper = std::min(gridMinimum + bin * gridStep +
                            0.5 * gridStep, shiftedCenter + cutoff) - shiftedCenter;
                        if (lower >= upper) continue;
                        const double weight = VoigtInterval(lower, upper, sigma, gamma);
                        if (weight <= 0.0) continue;
                        const std::size_t index = (((static_cast<std::size_t>(c) * selfCount + s) *
                            temperatureCount + t) * gridCount + bin);
                        spectra[index] += lineArea * weight / gridStep;
                        used = true;
                    }
                    if (!used) continue;
                    const std::size_t state = (static_cast<std::size_t>(c) * selfCount + s) *
                        temperatureCount + t;
                    ++counts[state];
                    const double available = std::min({cutoff,
                        shiftedCenter - (gridMinimum - 0.5 * gridStep),
                        (gridMinimum + (gridCount - 1) * gridStep + 0.5 * gridStep) -
                            shiftedCenter});
                    const double half = 0.5 * available;
                    const double tail = available <= 0.0 ? 1.0 : std::min(1.0,
                        (sigma > 0.0 ? std::erfc(half / (std::sqrt(2.0) * sigma)) : 0.0) +
                        (gamma > 0.0 ? 1.0 - 2.0 / kPi * std::atan(half / gamma) : 0.0));
                    tailBounds[state] = std::max(tailBounds[state], tail);
                }
            }
        }
    }
    return 0;
}
