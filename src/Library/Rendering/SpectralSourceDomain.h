// Immutable proposal configuration only. This quadrature is NOT a transport
// wavelength density or a certificate of calibrated source/material coverage.
#ifndef RISE_SPECTRAL_SOURCE_DOMAIN_H
#define RISE_SPECTRAL_SOURCE_DOMAIN_H
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <utility>
#include <stdexcept>
#include <vector>
namespace RISE { namespace Implementation {
class SpectralSourceDomain final {
public:
    struct Node { double nm; double weight; };
    // A tagged logarithmic mass avoids overflowing power products or sums.
    // Zero is represented explicitly, never by a floating-point sentinel.
    struct Mass {
        bool positive = false;
        double logarithm = 0;
        void AddLog(double value) {
            if (!positive) { positive = true; logarithm = value; return; }
            const double hi = std::max(logarithm,value);
            logarithm = hi + std::log1p(std::exp(std::min(logarithm,value)-hi));
        }
        void Add(double value, double weight = 1) {
            if (!std::isfinite(value) || value < 0 || !std::isfinite(weight) || weight <= 0)
                throw std::invalid_argument("spectral proposal requires finite nonnegative emission and positive weights");
            if (value > 0) AddLog(std::log(value)+std::log(weight));
        }
        void Multiply(double value) {
            if (!std::isfinite(value) || value <= 0)
                throw std::invalid_argument("spectral proposal geometry factor must be finite and positive");
            if (positive) logarithm += std::log(value);
        }
    };
private:
    const std::vector<Node> nodes;
    const double sourceMixture;
    const unsigned long long revision;
    SpectralSourceDomain(std::vector<Node> n, double mixture, unsigned long long r)
        : nodes(std::move(n)), sourceMixture(mixture), revision(r) {}
public:
    // The renderer's Sobol draws lie on a 2^-32 grid. Require every
    // uniform fallback interval to cover two grid cells, leaving an
    // interior draw after endpoint rounding. This is proposal policy,
    // not a physical tolerance. Alias construction has a 32-bit count.
    static constexpr double MinimumSourceProbability() { return 1.0/2147483648.0; }
    static std::shared_ptr<const SpectralSourceDomain> Create(
        const std::vector<Node>& nodes, double uniformSourceMass = .05,
        unsigned long long revision = 1) {
        if (nodes.empty() || !std::isfinite(uniformSourceMass) ||
            uniformSourceMass < 2*MinimumSourceProbability() || uniformSourceMass > 1)
            throw std::invalid_argument("spectral proposal domain/mixture is invalid");
        double previous = 0;
        for (const Node& n : nodes) {
            if (!std::isfinite(n.nm) || n.nm <= previous ||
                !std::isfinite(n.weight) || n.weight <= 0)
                throw std::invalid_argument("spectral quadrature must be ordered, finite and positive");
            previous = n.nm;
        }
        return std::shared_ptr<const SpectralSourceDomain>(new SpectralSourceDomain(nodes,uniformSourceMass,revision));
    }
    const std::vector<Node>& Nodes() const { return nodes; }
    double UniformSourceMass() const { return sourceMixture; }
    unsigned long long Revision() const { return revision; }
    template<class Evaluate> Mass Integrate(Evaluate evaluate) const {
        Mass m;
        for (const Node& n : nodes) m.Add(evaluate(n.nm), n.weight);
        return m;
    }
    // Actual support-preserving PMFs. Class selection uses this same policy
    // on [finite total, environment total], independently of finite NEE PMFs.
    std::vector<double> Probabilities(const std::vector<Mass>& masses) const {
        std::vector<double> q(masses.size(),0);
        if (masses.empty()) return q;
        if (masses.size()>std::numeric_limits<unsigned int>::max() ||
            sourceMixture/static_cast<double>(masses.size())<MinimumSourceProbability())
            throw std::invalid_argument("spectral uniform mixture/count leaves fewer than two 32-bit Sobol cells per source");
        bool any = false;
        double maximum = 0;
        for (const Mass& m : masses) if (m.positive) {
            maximum = any ? std::max(maximum,m.logarithm) : m.logarithm;
            any = true;
        }
        double sum = 0;
        if (any) for (size_t i=0;i<masses.size();++i) {
            if (masses[i].positive) q[i] = std::exp(masses[i].logarithm-maximum);
            sum += q[i];
        }
        const double uniform = 1.0/static_cast<double>(q.size());
        const double floor = sourceMixture*uniform;
        if (!(floor > 0)) throw std::invalid_argument("spectral proposal uniform mass is unrepresentable for this source count");
        for (double& p : q) p = any ? (1-sourceMixture)*(p/sum)+floor : uniform;
        return q;
    }
};
} }
#endif
