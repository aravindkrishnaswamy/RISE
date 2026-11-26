#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace RISE::HyLIoS {

inline constexpr const char* LocalResponseVersion = "hylios_local_projected_cells_v1";
inline constexpr double LocalResponsePi = 3.141592653589793238462643383279502884;
inline constexpr double LocalResponseTwoPi = 2.0 * LocalResponsePi;

// Right-handed frame: +Z is the outward normal, phi=atan2(y,x), +X at phi=0.
// Both wi and wo point AWAY from the interface. The oracle launches along -wi.
// t=cos(theta)^2. Queries require 0<t<=1; phi is periodic, and at t=1 phi=0.
struct LocalResponsePoint { double t, phi; };
struct LocalResponseDirection { double x, y, z; };

struct LocalResponseGrid {
    std::vector<double> tEdges;
    std::vector<double> phiEdges;
};

struct LocalResponseDefinition {
    LocalResponseGrid incident, outgoing;
    std::vector<double> wavelengthNm;
    // Cell index = tIndex * numberOfPhiCells + phiIndex.
    // Full F storage order: [wavelength][incident cell][outgoing cell].
    // f is relative to projected outgoing angle, NOT outgoing solid angle.
    std::vector<double> continuous;
    // [wavelength][incident cell]. Exact planar mirror MAP, approximate scalar
    // coefficient, using the SAME incident-cell-constant interpretation as F.
    std::vector<double> mirror;
};

struct LocalResponseLimits {
    std::size_t maximumWavelengths = 4096;
    std::size_t maximumCellsPerGrid = 65536;
    std::size_t maximumContinuousEntries = 32 * 1024 * 1024;
    // Owned object + logical vector payload, including the complete column CDF.
    // Caller input, shared_ptr control block and allocator overhead are excluded.
    std::size_t maximumStorageBytes = std::size_t{1024} * 1024 * 1024;
};

struct LocalResponseStorage {
    std::size_t incidentCells, outgoingCells, continuousEntries, storageBytes;
};

// Discrete selector variates are independent k/2^precisionBits, k=0..2^bits-1.
// The adapter must supply its actual grid (e.g. 32 for a 32-bit Sobol draw).
// The core accepts 1..53 bits; it does not floor unresolved proposal masses.
struct LocalResponseSamplingGrid { unsigned precisionBits; };

struct LocalResponseSample {
    enum class Event { None, Continuous, PlanarMirror };
    Event event = Event::None;
    LocalResponsePoint incident{1, 0};
    // Complete-mixture solid-angle sub-density; zero for a delta/empty event.
    double pdf = 0;
    // Z rounds to the public double only here. PDFs and event probabilities use
    // retained scale; subnormal final-output ULP error does not enter proposals.
    double weight = 0;
    double eventProbability = 0;
};

// Synthetic/pure numeric response core only. No loader, oracle bake, material,
// visible guide, semantic asset key or render preparation is supplied here.
// Construction validates every nodal forward row; columns may exceed one.
// All published methods are const; the object owns private copies of its input.
class HyLIoSLocalResponse final {
public:
    static LocalResponseStorage EstimateStorage(std::size_t wavelengthCount,
        std::size_t incidentTCells, std::size_t incidentPhiCells,
        std::size_t outgoingTCells, std::size_t outgoingPhiCells,
        const LocalResponseLimits& limits = {});
    static std::shared_ptr<const HyLIoSLocalResponse> Create(
        const LocalResponseDefinition&, const LocalResponseLimits& limits = {});

    // Spectral coverage is exactly the closed interval of supplied nodes.
    // A single node covers only itself. No extrapolation or endpoint clamping.
    bool Covers(double firstNm, double lastNm) const noexcept;
    void RequireCoverage(double firstNm, double lastNm) const;
    double EvaluateContinuousNM(LocalResponsePoint wi, LocalResponsePoint wo, double nm) const;
    // Exact nodal represented-product totals with the shared exact convex law,
    // rounded once on output; does not sum rounded pointwise query values.
    double ForwardRowMassNM(LocalResponsePoint wi, double nm) const;
    double CameraContinuousMassNM(LocalResponsePoint wo, double nm) const;
    double CameraMirrorMassNM(LocalResponsePoint wo, double nm) const;
    double CameraPdfNM(LocalResponsePoint wi, LocalResponsePoint wo, double nm) const;
    // Optical Covers() does not imply finite-grid sampling support. This checks
    // all active event/node/cell intervals at one query; preparation must cover
    // its entire eventual query domain. Failure is a domain_error.
    void RequireSamplingSupport(LocalResponsePoint wo, double nm, LocalResponseSamplingGrid) const;
    // event, spectral-node and cell draws are independent and in [0,1);
    // projected-cell draws are independent and in (0,1).
    // Samples only the represented mixture; never normalizes physical F.
    LocalResponseSample SampleCameraNM(LocalResponsePoint wo, double nm,
        double event, double node, double cell, double projectedT, double azimuth,
        LocalResponseSamplingGrid) const;

    static LocalResponsePoint CanonicalPoint(LocalResponsePoint);
    // Representative of the exact canonical mirror shift, rounded downward
    // when its angle is unrepresentable so every binary64 cell side is retained.
    // The geometric map is exact; scalar representatives need not be an involution.
    static LocalResponsePoint PlanarMirror(LocalResponsePoint);
    static LocalResponseDirection ToDirection(LocalResponsePoint);
    static LocalResponsePoint FromDirection(LocalResponseDirection);

    const LocalResponseStorage& Storage() const noexcept { return storage_; }
    std::size_t IncidentCell(LocalResponsePoint p) const;
    std::size_t OutgoingCell(LocalResponsePoint p) const;
    double IncidentCellArea(std::size_t i) const;
    double OutgoingCellArea(std::size_t j) const;

    HyLIoSLocalResponse(const HyLIoSLocalResponse&) = delete;
    HyLIoSLocalResponse& operator=(const HyLIoSLocalResponse&) = delete;
private:
    struct Scaled { double value; int exponent; };
    struct SpectralBracket { std::size_t lo, hi; double minority; bool primaryLo; };
    struct CameraMixture { std::size_t j; Scaled c0,c1,c,d,z; bool sameColumn; };
    explicit HyLIoSLocalResponse(const LocalResponseDefinition&, LocalResponseStorage);
    SpectralBracket Bracket(double nm) const;
    double F(std::size_t l, std::size_t i, std::size_t j) const;
    double ColumnCdfTotal(std::size_t l, std::size_t j) const;
    Scaled ScaledColumn(std::size_t l, std::size_t j) const;
    double Interpolate(double lo, double hi, const SpectralBracket&) const;
    static Scaled MakeScaled(double value, int exponent = 0);
    static Scaled MultiplyScaled(Scaled, Scaled);
    static Scaled TimesScaled(Scaled, double);
    static Scaled ComplementTimesScaled(Scaled, double minority);
    static Scaled AddScaled(Scaled, Scaled);
    static bool LessScaled(Scaled, Scaled);
    static double Physical(Scaled);
    static double RatioScaled(Scaled numerator, Scaled denominator);
    static bool ChooseFirstScaled(Scaled, Scaled, double);
    static void CheckScaledInterval(Scaled part, Scaled whole, double minimum);
    static Scaled InterpolateScaled(Scaled, Scaled, const SpectralBracket&);
    static Scaled ProjectedArea(const LocalResponseGrid&, std::size_t);
    Scaled Mirror(LocalResponsePoint wo, const SpectralBracket&) const;
    Scaled ContinuousMass(std::size_t j, const SpectralBracket&) const;
    CameraMixture SamplingMixture(LocalResponsePoint wo, const SpectralBracket&, LocalResponseSamplingGrid) const;
    std::size_t SelectCell(std::size_t l, std::size_t j, double draw) const;
    LocalResponsePoint SampleInCell(std::size_t i, double u, double v) const;

    LocalResponseStorage storage_;
    LocalResponseGrid incident_, outgoing_;
    std::vector<double> wavelengths_, continuous_, mirror_;
    std::vector<Scaled> incidentAreas_, outgoingAreas_;
    // [wavelength][outgoing cell][incident prefix ending at this cell].
    std::vector<double> columnCdf_;
    // CDF values are dimensionless power-of-two-scaled physical column masses.
    std::vector<int> columnExponent_;
    std::vector<double> minimumCellProbability_;
    // Exact binary proportionality of adjacent physical F columns, not a fit.
    std::vector<unsigned char> sameAdjacentColumn_;
};

} // namespace RISE::HyLIoS
