#include "Agent/Json.h"
#include "Materials/HyLIoS/HyLIoSForwardTransport.h"
#include "Materials/HyLIoS/HyLIoSOptics.h"
#include "Utilities/RISECBOR64.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace RISE::HyLIoS;
using J = RISE::Agent::JsonValue;
namespace {
constexpr const char *MeasurementVersion = "hylios-forward-measure-v3";
volatile std::sig_atomic_t interrupted = 0;
void Signal(int) {
    interrupted = 1;
}
void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
J Num(double x) {
    Require(std::isfinite(x), "nonfinite result cannot be published");
    return J::MakeNumber(x);
}
J Str(const std::string &s) {
    return J::MakeString(s);
}
J Bool(bool b) {
    return J::MakeBool(b);
}
void Put(J &object, const std::string &key, double v) {
    object.set(key, Num(v));
}
std::string Text(const J &v, const std::string &label) {
    Require(v.isString(), label + " must be a string");
    return v.asString();
}
double Number(const J &v, const std::string &label, double minimum = 0, bool strict = false) {
    Require(v.isNumber(), label + " must be a number");
    const double n = v.asNumber();
    Require(std::isfinite(n) && (strict ? n > minimum : n >= minimum),
            label + " outside finite supported range");
    return n;
}
bool Boolean(const J &v, const std::string &label) {
    Require(v.isBool(), label + " must be boolean");
    return v.asBool();
}
void ValidateUnicode(const std::string &text) {
    // Agent JSON is intentionally permissive; this artifact boundary accepts
    // only UTF-8 encodings of Unicode scalar values, including object keys.
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80)
            continue;
        unsigned continuation = 0, minimum = 0, codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
            minimum = 0x80;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            minimum = 0x800;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            minimum = 0x10000;
            codepoint = first & 0x07;
        } else {
            throw std::runtime_error("invalid Unicode: malformed UTF-8");
        }
        Require(continuation <= text.size() - i, "invalid Unicode: truncated UTF-8");
        for (unsigned j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            Require((next & 0xc0) == 0x80, "invalid Unicode: malformed UTF-8 continuation");
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        Require(codepoint >= minimum && codepoint <= 0x10ffff &&
                    !(codepoint >= 0xd800 && codepoint <= 0xdfff),
                "invalid Unicode: non-scalar or overlong UTF-8");
    }
}
bool Digit(char c) {
    return c >= '0' && c <= '9';
}
void ValidateNumericToken(const std::string &token) {
    // Compare the original decimal magnitude, before strtod can round an
    // integer identifier such as 9007199254740993 down to 9007199254740992.
    std::size_t position = token[0] == '-' ? 1 : 0;
    std::string digits;
    while (position < token.size() && Digit(token[position]))
        digits += token[position++];
    Require(!digits.empty(), "invalid JSON number");
    Require(digits.size() == 1 || digits[0] != '0', "invalid JSON number: leading zero");
    std::size_t fractionDigits = 0;
    if (position < token.size() && token[position] == '.') {
        ++position;
        while (position < token.size() && Digit(token[position])) {
            digits += token[position++];
            ++fractionDigits;
        }
        Require(fractionDigits > 0, "invalid JSON number: missing fractional digits");
    }
    std::int64_t exponent = 0;
    if (position < token.size() && (token[position] == 'e' || token[position] == 'E')) {
        ++position;
        bool negative = false;
        if (position < token.size() && (token[position] == '-' || token[position] == '+'))
            negative = token[position++] == '-';
        const std::size_t start = position;
        // Any larger exponent exceeds the number of significand digits that
        // could offset it. Saturation bounds work without integer overflow.
        const auto limit = static_cast<std::int64_t>(token.size()) + 32;
        while (position < token.size() && Digit(token[position]))
            exponent = std::min(limit, exponent * 10 + (token[position++] - '0'));
        Require(position > start, "invalid JSON number: missing exponent digits");
        if (negative)
            exponent = -exponent;
    }
    Require(position == token.size(), "invalid JSON number");
    const auto significant = digits.find_first_not_of('0');
    if (significant == std::string::npos)
        return;
    const auto integerPlaces = static_cast<std::int64_t>(digits.size() - significant) + exponent -
                               static_cast<std::int64_t>(fractionDigits);
    constexpr const char *maximum = "9007199254740992"; // 2^53, exactly representable.
    bool supported = integerPlaces < 16;
    if (integerPlaces == 16) {
        std::string leading;
        for (std::size_t i = 0; i < 16; ++i)
            leading += significant + i < digits.size() ? digits[significant + i] : '0';
        supported = leading < maximum;
        if (leading == maximum)
            supported = digits.find_first_not_of('0', std::min(digits.size(), significant + 16)) ==
                        std::string::npos;
    }
    Require(supported,
            "supported JSON numeric magnitude is <= 2^53; encode larger identifiers as strings");
    const double value = std::strtod(token.c_str(), nullptr);
    Require(std::isfinite(value) && value != 0.0, "JSON number underflows finite binary64 support");
}
void ValidateSourceTokens(const std::string &bytes) {
    ValidateUnicode(bytes);
    for (std::size_t position = 0; position < bytes.size();) {
        if (bytes[position] == '"') {
            // Numeric-looking text inside quoted keys/values remains text.
            ++position;
            while (position < bytes.size() && bytes[position] != '"') {
                if (bytes[position++] == '\\' && position < bytes.size())
                    ++position;
            }
            if (position < bytes.size())
                ++position;
        } else if (Digit(bytes[position]) || bytes[position] == '-') {
            const std::size_t start = position++;
            while (position < bytes.size() &&
                   (Digit(bytes[position]) || bytes[position] == '.' || bytes[position] == 'e' ||
                    bytes[position] == 'E' || bytes[position] == '+' || bytes[position] == '-'))
                ++position;
            ValidateNumericToken(bytes.substr(start, position - start));
        } else {
            ++position;
        }
    }
}
void ValidateTree(const J &v) {
    if (v.isString())
        ValidateUnicode(v.asString());
    if (v.isNumber())
        Require(std::isfinite(v.asNumber()), "input contains nonfinite number");
    if (v.isArray())
        for (std::size_t i = 0; i < v.size(); ++i)
            ValidateTree(v.at(i));
    if (v.isObject()) {
        std::set<std::string> keys;
        for (const auto &p : v.members()) {
            ValidateUnicode(p.first);
            Require(keys.insert(p.first).second, "duplicate JSON key: " + p.first);
            ValidateTree(p.second);
        }
    }
}
std::uint64_t Integer(const std::string &s, const std::string &label, std::uint64_t maximum,
                      bool allowZero = false) {
    Require(!s.empty(), label + " is empty");
    std::uint64_t result = 0;
    for (char c : s) {
        Require(c >= '0' && c <= '9', label + " must be an unsigned decimal integer");
        const auto digit = static_cast<unsigned>(c - '0');
        Require(result <= (maximum - digit) / 10, label + " exceeds supported bound");
        result = result * 10 + digit;
    }
    Require(allowZero || result > 0, label + " must be positive");
    return result;
}
double Real(const std::string &s, const std::string &label, double minimum = 0,
            bool strict = false) {
    std::size_t end = 0;
    double v = 0;
    try {
        v = std::stod(s, &end);
    } catch (const std::exception &) {
        throw std::runtime_error(label + " invalid number");
    }
    Require(end == s.size() && std::isfinite(v) && (strict ? v > minimum : v >= minimum),
            label + " outside finite supported range");
    return v;
}
struct Options {
    std::string specimen, output, placement, postScatter, complexLayout, boundary, seedText, estimator = "analog";
    double wavelength = 0, roulette = 0, fiberRadius = 1e-7, fiberFraction = .22,
           fiberEta = 1.5 / 1.33, exteriorIOR = 1;
    std::uint64_t histories = 0, maxTransitions = 0, seed = 0;
    J explicitOptions = J::MakeObject();
};
void Usage() {
    std::cout << "hylios_measure --specimen FILE --wavelength-nm N --placement fixed|null "
                 "--post-scatter diagnostic|discard --boundary smooth --histories N --max-transitions N --seed U64 --output FILE\n"
                 "  [--complex-layout explicit-pack-v1] [--estimator analog|weighted] [--roulette-target N] [--exterior-ior N]\n"
                 "  [--fiber-radius-m N] [--fiber-volume-fraction N] [--fiber-relative-ior N]\n"
                 "Only forward particle/complex transport and explicit smooth boundary override are "
                 "supported.\n";
}
Options Parse(int argc, char **argv) {
    Options o;
    std::map<std::string, std::string> flags;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        Require(i + 1 < argc, "missing value for " + key);
        Require(flags.emplace(key, argv[++i]).second, "duplicate CLI option: " + key);
    }
    const std::set<std::string> allowed = {"--specimen",
                                           "--wavelength-nm",
                                           "--placement",
                                           "--post-scatter",
                                           "--complex-layout",
                                           "--boundary",
                                           "--histories",
                                           "--max-transitions",
                                           "--seed",
                                           "--output",
                                           "--estimator",
                                           "--roulette-target",
                                           "--exterior-ior",
                                           "--fiber-radius-m",
                                           "--fiber-volume-fraction",
                                           "--fiber-relative-ior"};
    for (const auto &pair : flags) {
        Require(allowed.count(pair.first) > 0, "unknown option: " + pair.first);
        o.explicitOptions.set(pair.first, Str(pair.second));
    }
    for (const auto &key : {"--specimen", "--wavelength-nm", "--placement", "--post-scatter", "--boundary",
                            "--histories", "--max-transitions", "--seed", "--output"})
        Require(flags.count(key) > 0, std::string("required option: ") + key);
    o.specimen = flags.at("--specimen");
    o.output = flags.at("--output");
    Require(!o.specimen.empty() && !o.output.empty(), "empty input/output path");
    o.placement = flags.at("--placement");
    Require(o.placement == "fixed" || o.placement == "null", "placement must be fixed or null");
    o.postScatter = flags.at("--post-scatter");
    Require(o.postScatter == "diagnostic" || o.postScatter == "discard",
            "post-scatter must be diagnostic or discard");
    if (flags.count("--complex-layout")) {
        o.complexLayout = flags.at("--complex-layout");
        Require(o.complexLayout == "explicit-pack-v1", "complex-layout must be explicit-pack-v1");
    }
    o.boundary = flags.at("--boundary");
    Require(o.boundary == "smooth",
            "only explicit --boundary smooth is implemented; authored folds are not simulated");
    o.wavelength = Real(flags.at("--wavelength-nm"), "wavelength", 0, true);
    o.histories = Integer(flags.at("--histories"), "histories", 10000000);
    o.maxTransitions = Integer(flags.at("--max-transitions"), "max-transitions", 10000000);
    Require(o.histories <= 10000000000ULL / o.maxTransitions,
            "history*transition work budget exceeds 10^10");
    o.seedText = flags.at("--seed");
    o.seed = Integer(o.seedText, "seed", std::numeric_limits<std::uint64_t>::max(), true);
    if (flags.count("--estimator"))
        o.estimator = flags.at("--estimator");
    Require(o.estimator == "analog" || o.estimator == "weighted",
            "estimator must be analog or weighted");
    if (flags.count("--roulette-target"))
        o.roulette = Real(flags.at("--roulette-target"), "roulette-target");
    Require(o.estimator == "weighted" || o.roulette == 0, "roulette requires weighted estimator");
    if (flags.count("--exterior-ior"))
        o.exteriorIOR = Real(flags.at("--exterior-ior"), "exterior-ior", 0, true);
    if (flags.count("--fiber-radius-m"))
        o.fiberRadius = Real(flags.at("--fiber-radius-m"), "fiber-radius-m", 0, true);
    if (flags.count("--fiber-volume-fraction"))
        o.fiberFraction = Real(flags.at("--fiber-volume-fraction"), "fiber-volume-fraction");
    Require(o.fiberFraction <= 1, "fiber fraction must be <=1");
    if (flags.count("--fiber-relative-ior"))
        o.fiberEta = Real(flags.at("--fiber-relative-ior"), "fiber-relative-ior", 0, true);
    Require(std::filesystem::absolute(std::filesystem::u8path(o.specimen)).lexically_normal() !=
                std::filesystem::absolute(std::filesystem::u8path(o.output)).lexically_normal(),
            "input and output paths must differ");
    if (std::filesystem::exists(std::filesystem::u8path(o.output)))
        Require(!std::filesystem::equivalent(std::filesystem::u8path(o.specimen),
                                             std::filesystem::u8path(o.output)),
                "input and output name the same file");
    return o;
}
std::string Read(const std::string &path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    Require(input.good(), "cannot open specimen");
    std::string bytes;
    std::array<char, 8192> chunk{};
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        bytes.append(chunk.data(), static_cast<std::size_t>(input.gcount()));
        Require(bytes.size() <= 64 * 1024 * 1024, "specimen exceeds 64MiB input budget");
    }
    Require(input.eof(), "specimen read failed");
    return bytes;
}
std::vector<double> Spectrum(const J &values, std::size_t size, const std::string &label,
                             bool positive = false) {
    Require(values.isArray() && values.size() == size, label + " wavelength shape mismatch");
    std::vector<double> v;
    for (std::size_t i = 0; i < size; ++i)
        v.push_back(Number(values.at(i), label, 0, positive));
    return v;
}
struct Resolved {
    J original;
    std::array<ForwardLayer, 6> layers{};
    J selected = J::MakeArray();
    std::string bytesHash;
};
Resolved Resolve(const Options &options) {
    const auto bytes = Read(options.specimen);
    Resolved r;
    std::string error;
    ValidateSourceTokens(bytes);
    Require(RISE::Agent::JsonParse(bytes, r.original, error), "invalid specimen JSON: " + error);
    ValidateTree(r.original);
    const auto &source = r.original;
    Require(source.isObject(), "specimen must be an object");
    Require(Number(source.get("schema_version"), "schema version") == 1,
            "unsupported resolved specimen schema");
    const auto &units = source.get("units");
    Require(units.isObject(), "missing units");
    for (const auto &unit : std::map<std::string, std::string>{{"length", "m"},
                                                               {"coefficient", "m^-1"},
                                                               {"wavelength", "nm"},
                                                               {"mass_concentration", "kg/m^3"}})
        Require(Text(units.get(unit.first), "units." + unit.first) == unit.second,
                "unsupported units." + unit.first);
    Boolean(source.get("approximate"), "approximate");
    for (const auto &key : {"composition_revision", "coverage_profile_id", "spectral_bundle_sha256",
                            "resolved_specimen_sha256", "preset_id", "interpretation"})
        Require(!Text(source.get(key), key).empty(), std::string("empty metadata: ") + key);
    Require(source.get("provenance").isObject(), "missing provenance object");
    const bool approximate = source.get("approximate").asBool();
    const auto profile = source.get("coverage_profile_id").asString();
    if (profile == "strict_source_coverage_v1")
        Require(!approximate, "strict coverage profile contradicts approximate flag");
    if (profile == "hylios_cleanroom_approximation_v1")
        Require(approximate, "approximation profile contradicts approximate flag");
    const auto &bundle = source.get("provenance").get("spectral_bundle");
    if (!bundle.isNull()) {
        Require(bundle.isObject(), "spectral bundle provenance must be object");
        Require(Boolean(bundle.get("approximate"), "bundle approximate") == approximate,
                "nested approximation flag disagrees with top-level metadata");
        Require(Text(bundle.get("profile_id"), "bundle profile") == profile,
                "nested coverage profile disagrees with top-level metadata");
        Require(Text(bundle.get("bundle_sha256"), "bundle identity") ==
                    source.get("spectral_bundle_sha256").asString(),
                "nested spectral bundle identity disagrees with top-level metadata");
    }
    Number(source.get("surface_fold_aspect_ratio"), "surface fold aspect ratio");
    const auto &grid = source.get("wavelength_nm");
    Require(grid.isArray() && grid.size() > 0 && grid.size() <= 10000, "invalid wavelength grid");
    std::size_t selected = grid.size();
    std::set<double> wavelengths;
    for (std::size_t i = 0; i < grid.size(); ++i) {
        const double wave = Number(grid.at(i), "wavelength grid", 0, true);
        Require(wavelengths.insert(wave).second, "duplicate wavelength grid value");
        if (wave == options.wavelength)
            selected = i;
    }
    Require(selected < grid.size(), "requested wavelength has no exact resolved coverage");
    const auto &layers = source.get("layers");
    Require(layers.isArray() && layers.size() == 6,
            "resolved specimen must contain exactly six layers");
    const std::array<std::string, 6> names = {"stratum_corneum",  "stratum_granulosum",
                                              "stratum_spinosum", "stratum_basale",
                                              "papillary_dermis", "reticular_dermis"};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto &input = layers.at(i);
        Require(input.isObject(), "layer must be an object");
        Require(Text(input.get("layer_id"), "layer_id") == names[i], "layer order mismatch");
        const auto absorption =
                       Spectrum(input.get("host_absorption_m_inv"), grid.size(), "host absorption"),
                   ior =
                       Spectrum(input.get("real_refractive_index"), grid.size(), "layer IOR", true);
        r.layers[i] = {Number(input.get("thickness_m"), "layer thickness", 0, true),
                       absorption[selected], ior[selected], std::nullopt};
        const auto &constituents = input.get("constituent_absorption_m_inv");
        Require(constituents.isObject(), "missing constituent absorption record");
        for (const auto &c : constituents.members())
            Spectrum(c.second, grid.size(), "constituent " + c.first);
        const auto &particle = input.get("particles");
        if (!particle.isNull()) {
            Require(particle.isObject(), "particles must be null or object");
            const bool enabled = Boolean(particle.get("enabled"), "particle enabled");
            if (!enabled) {
                if (particle.has("organelle_fraction"))
                    Require(Number(particle.get("organelle_fraction"),
                                   "inactive organelle fraction") == 0,
                            "disabled particles have positive fraction");
            } else {
                const double fraction =
                    Number(particle.get("organelle_fraction"), "organelle fraction", 0, true);
                Require(fraction <= 1, "organelle fraction exceeds1");
                const auto organization = Text(particle.get("organization"), "organelle organization");
                const bool complex = organization == "complexes";
                Require(complex || organization == "individually_dispersed_particles",
                        "unknown organelle organization");
                Require(!complex || options.complexLayout == "explicit-pack-v1",
                        "active complexes require explicit --complex-layout explicit-pack-v1");
                Require(Boolean(particle.get("interior_absorption_evaluated"),
                                "particle absorption coverage"),
                        "active organelle absorption has not been evaluated");
                const auto melanin = Spectrum(particle.get("interior_absorption_m_inv"),
                                              grid.size(), "particle absorption");
                ForwardOrganelle o{
                    complex ? ForwardOrganization::ExplicitComplexPackV1 : ForwardOrganization::IndividualSpheroids,
                    fraction,
                    Number(particle.get("semi_minor_m"), "semi-minor", 0, true),
                    Number(particle.get("semi_major_m"), "semi-major", 0, true),
                    melanin[selected],
                    Number(particle.get("real_refractive_index"), "particle IOR", 0, true)};
                Require(o.semiMajorMetres >= o.semiMinorMetres, "prolate major axis below minor");
                if (complex) {
                    o.envelopeRadiusMetres = Number(particle.get("complex_diameter_m"), "complex diameter", 0, true) / 2;
                    Require(o.envelopeRadiusMetres > 0, "complex radius underflows");
                    const double count = Number(particle.get("maximum_internal_count"), "complex child count");
                    Require(std::floor(count) == count && count <= ComplexLayout::MaximumChildren,
                            "complex child count must be an integer from 0 to 64");
                    o.childCount = static_cast<std::size_t>(count);
                    // Constructor checks dimensions, volume and implementation range
                    // before any history or output mutation. It draws no samples.
                    const ComplexLayout validated(o.envelopeRadiusMetres, o.semiMinorMetres,
                                                  o.semiMajorMetres, o.childCount);
                }
                if (particle.has("encounter_rate_m_inv")) {
                    const double rate = Number(particle.get("encounter_rate_m_inv"),
                                               "declared geometric rate"),
                                 computed = GeometricEncounterRate(
                                     complex ? ProlateSurfaceToVolume(o.envelopeRadiusMetres, o.envelopeRadiusMetres)
                                             : ProlateSurfaceToVolume(o.semiMinorMetres, o.semiMajorMetres),
                                     fraction);
                    Require(std::abs(rate - computed) <=
                                64 * std::numeric_limits<double>::epsilon() *
                                    std::max(rate, computed),
                            "declared particle rate disagrees with explicit axes and fraction");
                }
                r.layers[i].organelle = o;
            }
        }
        J chosen = J::MakeObject();
        chosen.set("layer_id", Str(names[i]));
        Put(chosen, "thickness_m", r.layers[i].thicknessMetres);
        Put(chosen, "host_absorption_m_inv", absorption[selected]);
        Put(chosen, "real_refractive_index", ior[selected]);
        if (r.layers[i].organelle) {
            const auto &o = *r.layers[i].organelle;
            J object = J::MakeObject();
            Put(object, "fraction", o.volumeFraction);
            Put(object, "semi_minor_m", o.semiMinorMetres);
            Put(object, "semi_major_m", o.semiMajorMetres);
            Put(object, "absorption_m_inv", o.absorptionPerMetre);
            Put(object, "real_refractive_index", o.realIOR);
            Put(object, "nominal_geometric_rate_m_inv",
                GeometricEncounterRate(o.organization == ForwardOrganization::ExplicitComplexPackV1
                    ? ProlateSurfaceToVolume(o.envelopeRadiusMetres, o.envelopeRadiusMetres)
                    : ProlateSurfaceToVolume(o.semiMinorMetres, o.semiMajorMetres), o.volumeFraction));
            if (o.organization == ForwardOrganization::ExplicitComplexPackV1) {
                Put(object, "envelope_radius_m", o.envelopeRadiusMetres);
                Put(object, "actual_child_count", static_cast<double>(o.childCount));
                object.set("count_interpretation", Str("resolved_maximum_internal_count_as_actual_children_v1"));
                chosen.set("active_individual_spheroids", J::MakeNull());
                chosen.set("active_complex_pack", object);
            } else {
                chosen.set("active_individual_spheroids", object);
                chosen.set("active_complex_pack", J::MakeNull());
            }
        } else {
            chosen.set("active_individual_spheroids", J::MakeNull());
            chosen.set("active_complex_pack", J::MakeNull());
        }
        r.selected.push_back(chosen);
    }
    r.bytesHash = RISE::RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char *>(bytes.data()),
                                              bytes.size());
    return r;
}
struct Random : RISE::ISampler {
    std::uint64_t state, draws = 0;
    explicit Random(std::uint64_t seed) : state(seed) {
    }
    double Get1D() override {
        ++draws;
        state += 0x9e3779b97f4a7c15ULL;
        auto z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * 0x1p-53;
    }
    RISE::Point2 Get2D() override {
        return {Get1D(), Get1D()};
    }
};
struct Moments {
    std::uint64_t n = 0;
    double mean = 0, m2 = 0;
    void Add(double v) {
        ++n;
        const double delta = v - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (v - mean);
        Require(std::isfinite(mean) && std::isfinite(m2), "measurement moments overflow");
    }
    J Json() const {
        J j = J::MakeObject();
        Put(j, "mean", mean);
        j.set("standard_error",
              n > 1 ? Num(std::sqrt(std::max(0., m2) /
                                    (static_cast<double>(n) * static_cast<double>(n - 1))))
                    : J::MakeNull());
        Put(j, "history_count", static_cast<double>(n));
        return j;
    }
};
std::string Outcome(ForwardTermination t) {
    switch (t) {
    case ForwardTermination::ExternalReflection:
        return "external_reflection";
    case ForwardTermination::SubsurfaceReturn:
        return "subsurface_return";
    case ForwardTermination::Absorbed:
        return "physical_absorption";
    case ForwardTermination::RouletteKilled:
        return "roulette_killed";
    case ForwardTermination::ZeroWeight:
        return "zero_weight";
    case ForwardTermination::Cancelled:
        return "cancelled";
    case ForwardTermination::InvalidSample:
        return "invalid_sample";
    case ForwardTermination::NumericalFailure:
        return "numerical_failure";
    case ForwardTermination::UnplaceableBoundaryCandidate:
        return "unplaceable_candidate";
    case ForwardTermination::ForwardPerturbationReentry:
        return "post_exit_reentry";
    case ForwardTermination::BudgetExceeded:
        return "budget_exceeded";
    default:
        throw std::runtime_error("unterminated history publication refused");
    }
}
bool Escaped(ForwardTermination t) {
    return t == ForwardTermination::ExternalReflection || t == ForwardTermination::SubsurfaceReturn;
}
bool Unresolved(ForwardTermination t) {
    return t == ForwardTermination::Cancelled || t == ForwardTermination::InvalidSample ||
           t == ForwardTermination::NumericalFailure ||
           t == ForwardTermination::UnplaceableBoundaryCandidate ||
           t == ForwardTermination::ForwardPerturbationReentry ||
           t == ForwardTermination::BudgetExceeded || t == ForwardTermination::ZeroWeight;
}
J Array(const std::array<double, 6> &a) {
    J j = J::MakeArray();
    for (double v : a)
        j.push_back(Num(v));
    return j;
}
struct Aggregate {
    std::map<std::string, std::uint64_t> counts, failureDetails;
    std::map<std::string, double> outcomeWeights, counters;
    std::array<double, 6> hostAbs{}, particleAbs{}, layerPath{}, hostPath{}, particlePath{};
    Moments reflectance, external, subsurface, absorption, unresolved, accounted;
    std::array<Moments, 6> hostLayerAbsorption, particleLayerAbsorption;
    std::uint64_t draws = 0;
    void Add(const ForwardState &s, std::uint64_t randomDraws) {
        const auto term = s.Termination();
        const auto outcome = Outcome(term);
        ++counts[outcome];
        if (term == ForwardTermination::NumericalFailure) {
            const auto detail = s.FailureDetail();
            const auto name =
                detail == ForwardFailureDetail::OverlapIndeterminate  ? "overlap_indeterminate"
                : detail == ForwardFailureDetail::OverlapInvalidInput ? "overlap_invalid_input"
                : detail == ForwardFailureDetail::OverlapUnsupportedRange
                    ? "overlap_unsupported_range"
                : detail == ForwardFailureDetail::ArithmeticOrGeometry ? "arithmetic_or_geometry"
                                                                       : "unspecified_arithmetic";
            ++failureDetails[name];
        }
        outcomeWeights[outcome] += s.Weight();
        draws += randomDraws;
        const auto &c = s.Counters();
#define ADD_COUNTER(name) counters[#name] += static_cast<double>(c.name)
        ADD_COUNTER(workUnits);
        ADD_COUNTER(particleCandidates);
        ADD_COUNTER(placementAttempts);
        ADD_COUNTER(nullPlacements);
        ADD_COUNTER(flights);
        ADD_COUNTER(internalInterfaces);
        ADD_COUNTER(bottomReflections);
        ADD_COUNTER(boundaryEncounters);
        ADD_COUNTER(rouletteTrials);
        ADD_COUNTER(rouletteKills);
        ADD_COUNTER(containmentRejections);
        ADD_COUNTER(overlapRejections);
        ADD_COUNTER(acceptedParticles);
        ADD_COUNTER(particleReflections);
        ADD_COUNTER(particleExits);
        ADD_COUNTER(inwardPostExitEvents);
        ADD_COUNTER(acceptedComplexes);
        ADD_COUNTER(complexExits);
        ADD_COUNTER(complexChildEntries);
        ADD_COUNTER(complexChildExits);
        ADD_COUNTER(complexChildReflections);
        ADD_COUNTER(complexHostSegments);
        ADD_COUNTER(complexLayoutProposals);
        ADD_COUNTER(complexLayoutContainmentRejections);
        ADD_COUNTER(complexLayoutOverlapRejections);
        ADD_COUNTER(complexLayoutRestarts);
        ADD_COUNTER(complexLayoutPairChecks);
        ADD_COUNTER(fiberEvents);
        ADD_COUNTER(fiberPasses);
        ADD_COUNTER(perturbationProposals);
        ADD_COUNTER(pathLengthMetres);
#undef ADD_COUNTER
        double absorbed = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            hostLayerAbsorption[i].Add(s.HostAbsorbedByLayer()[i]);
            particleLayerAbsorption[i].Add(s.ParticleAbsorbedByLayer()[i]);
            hostAbs[i] += s.HostAbsorbedByLayer()[i];
            particleAbs[i] += s.ParticleAbsorbedByLayer()[i];
            absorbed += s.HostAbsorbedByLayer()[i] + s.ParticleAbsorbedByLayer()[i];
            layerPath[i] += c.layerPathLengthMetres[i];
            hostPath[i] += c.hostPathLengthMetres[i];
            particlePath[i] += c.particlePathLengthMetres[i];
        }
        const double escaped = Escaped(term) ? s.Weight() : 0,
                     live = Unresolved(term) ? s.Weight() : 0;
        reflectance.Add(escaped);
        external.Add(term == ForwardTermination::ExternalReflection ? s.Weight() : 0);
        subsurface.Add(term == ForwardTermination::SubsurfaceReturn ? s.Weight() : 0);
        absorption.Add(absorbed);
        unresolved.Add(live);
        accounted.Add(absorbed + escaped + live);
    }
};
J Measure(const Options &o, const Resolved &r) {
    const double fiberRate =
        RayleighRate(o.fiberRadius, o.wavelength * 1e-9, o.fiberFraction, o.fiberEta);
    HyLIoSForwardTransport kernel(
        r.layers, o.wavelength, o.exteriorIOR, fiberRate,
        o.estimator == "analog" ? AbsorptionEstimator::Analog : AbsorptionEstimator::Weighted,
        o.roulette,
        o.placement == "fixed" ? ForwardPlacementPolicy::FixedEventDiagnosticV1
                               : ForwardPlacementPolicy::NullCollisionPlacementV1,
        o.postScatter == "diagnostic" ? ForwardPostScatterPolicy::DiagnosticRetainParticleV1
                                      : ForwardPostScatterPolicy::DiscardAfterScatterV1);
    Aggregate aggregate;
    for (std::uint64_t history = 0; history < o.histories; ++history) {
        Random rng(o.seed + history);
        auto s = kernel.Begin({0, 0, 0}, {0, 0, 1});
        while (s.Termination() == ForwardTermination::None &&
               s.Counters().workUnits < o.maxTransitions && !interrupted) {
            const auto result = kernel.Advance(s, rng,
                                               static_cast<std::size_t>(std::min<std::uint64_t>(
                                                   64, o.maxTransitions - s.Counters().workUnits)));
            if (result.kind == ForwardAdvanceKind::OuterBoundaryEncounter) {
                const auto event = s.PendingBoundary();
                kernel.ResumeBoundary(s, event.token,
                                      SampleSlabSmoothDielectric(event, rng.Get1D()));
            }
        }
        if (s.Termination() == ForwardTermination::None) {
            if (interrupted)
                kernel.Cancel(s);
            else
                kernel.StopForBudget(s);
        }
        aggregate.Add(s, rng.draws);
    }
    J output = J::MakeObject();
    output.set("measurement_version", Str(MeasurementVersion));
    output.set("kernel_version", Str(ForwardTransportVersion));
    output.set("model_scope", Str("six-layer monochromatic individual/explicit-complex forward transport; "
                                  "renderer adjoint unsupported"));
    output.set("placement_policy",
               Str(o.placement == "fixed" ? "FixedEventDiagnosticV1" : "NullCollisionPlacementV1"));
    output.set("complex_layout_policy", o.complexLayout.empty() ? J::MakeNull() : Str(ComplexLayoutVersion));
    J construction = J::MakeObject();
    construction.set("centre_support", Str("uniform ball of largest binary64 radius <= R-b, via cube rejection"));
    construction.set("child_axis_prior", Str("isotropic, independent of incoming ray"));
    Put(construction, "maximum_children", ComplexLayout::MaximumChildren);
    Put(construction, "proposals_per_child_before_restart", ComplexLayout::ProposalsPerChildBeforeRestart);
    construction.set("interaction_count_cap", J::MakeNull());
    output.set("complex_layout_convention", o.complexLayout.empty() ? J::MakeNull() : construction);
    output.set("boundary_law", Str("smooth_planar_dielectric_override_v1"));
    output.set("authored_surface_folds_simulated", Bool(false));
    output.set("authored_surface_fold_aspect_ratio", r.original.get("surface_fold_aspect_ratio"));
    output.set("post_exit_reentry_policy",
               Str(o.postScatter == "diagnostic" ? "hylios_diagnostic_retain_particle_v1"
                                                  : "hylios_discard_after_scatter_v1"));
    output.set("transport_interpretation", Str(o.postScatter == "discard"
        ? "versioned clean-room virtual particles; discarded after scattering"
        : "diagnostic; inward post-exit directions retain unresolved live weight"));
    output.set("approximate", r.original.get("approximate"));
    for (const auto &key :
         {"composition_revision", "coverage_profile_id", "spectral_bundle_sha256",
          "resolved_specimen_sha256", "preset_id", "interpretation", "provenance"})
        output.set(key, r.original.get(key));
    output.set("claimed_resolved_specimen_sha256_verified", Bool(false));
    output.set("input_identity_basis", Str("sha256 of exact opened input bytes; source canonical "
                                           "JSON hash retained as unverified claim"));
    output.set("input_bytes_sha256", Str(r.bytesHash));
    output.set("specimen_path", Str(o.specimen));
    output.set("normalization", Str("all incident histories, including physical absorption, "
                                    "roulette and unresolved outcomes"));
    std::uint64_t unresolvedCount = 0;
    for (auto term :
         {ForwardTermination::Cancelled, ForwardTermination::InvalidSample,
          ForwardTermination::NumericalFailure, ForwardTermination::UnplaceableBoundaryCandidate,
          ForwardTermination::ForwardPerturbationReentry, ForwardTermination::BudgetExceeded,
          ForwardTermination::ZeroWeight})
        unresolvedCount += aggregate.counts[Outcome(term)];
    output.set("complete_physical_estimate", Bool(unresolvedCount == 0));
    Put(output, "unresolved_history_count", static_cast<double>(unresolvedCount));
    Put(output, "wavelength_nm", o.wavelength);
    Put(output, "initial_history_count", static_cast<double>(o.histories));
    Put(output, "max_transitions_per_history", static_cast<double>(o.maxTransitions));
    output.set("estimator", Str(o.estimator));
    Put(output, "roulette_target", o.roulette);
    Put(output, "exterior_ior", o.exteriorIOR);
    output.set("incident_direction", Str("normal incidence (0,0,1) at local origin"));
    output.set("explicit_cli_options", o.explicitOptions);
    output.set("seed_u64", Str(std::to_string(o.seed)));
    output.set(
        "rng_algorithm",
        Str("splitmix64 top53; state=(seed+zero_based_history_id) mod 2^64; no shared stream"));
    Put(output, "random_draw_count", static_cast<double>(aggregate.draws));
    J fiber = J::MakeObject();
    fiber.set("parameter_convention", Str("paper Eq7 r100nm v.22 eta1.5/1.33 named defaults; each "
                                          "explicit override recorded below"));
    Put(fiber, "radius_m", o.fiberRadius);
    Put(fiber, "volume_fraction", o.fiberFraction);
    Put(fiber, "relative_refractive_index", o.fiberEta);
    Put(fiber, "rate_m_inv", fiberRate);
    output.set("papillary_fiber", fiber);
    output.set("selected_resolved_layers", r.selected);
    J outcomes = J::MakeObject();
    for (auto term :
         {ForwardTermination::ExternalReflection, ForwardTermination::SubsurfaceReturn,
          ForwardTermination::Absorbed, ForwardTermination::RouletteKilled,
          ForwardTermination::ZeroWeight, ForwardTermination::Cancelled,
          ForwardTermination::InvalidSample, ForwardTermination::NumericalFailure,
          ForwardTermination::UnplaceableBoundaryCandidate,
          ForwardTermination::ForwardPerturbationReentry, ForwardTermination::BudgetExceeded}) {
        const auto name = Outcome(term);
        J value = J::MakeObject();
        Put(value, "count", static_cast<double>(aggregate.counts[name]));
        Put(value, "terminal_live_weight", aggregate.outcomeWeights[name]);
        value.set("unresolved", Bool(Unresolved(term)));
        outcomes.set(name, value);
    }
    output.set("outcomes", outcomes);
    J failures = J::MakeObject();
    for (const auto &pair : aggregate.failureDetails)
        Put(failures, pair.first, static_cast<double>(pair.second));
    output.set("numerical_failure_details", failures);
    J counters = J::MakeObject();
    for (const auto &pair : aggregate.counters)
        Put(counters, pair.first, pair.second);
    output.set("aggregate_counters", counters);
    output.set("host_absorbed_weight_by_layer", Array(aggregate.hostAbs));
    output.set("particle_absorbed_weight_by_layer", Array(aggregate.particleAbs));
    output.set("path_length_m_by_layer", Array(aggregate.layerPath));
    output.set("host_path_length_m_by_layer", Array(aggregate.hostPath));
    output.set("particle_path_length_m_by_layer", Array(aggregate.particlePath));
    J estimates = J::MakeObject();
    estimates.set("escaped_reflectance", aggregate.reflectance.Json());
    estimates.set("external_reflection", aggregate.external.Json());
    estimates.set("subsurface_return", aggregate.subsurface.Json());
    estimates.set("physical_absorption", aggregate.absorption.Json());
    estimates.set("unresolved_live_weight", aggregate.unresolved.Json());
    estimates.set("accounted_weight", aggregate.accounted.Json());
    Put(estimates, "escaped_plus_unresolved_mean",
        aggregate.reflectance.mean + aggregate.unresolved.mean);
    estimates.set(
        "bound_interpretation",
        Str("remaining expected physical escaped power conditional on history lies in [0,live "
            "weight]; future roulette realizations and MC uncertainty remain statistical"));
    J hostEst = J::MakeArray(), particleEst = J::MakeArray();
    for (std::size_t i = 0; i < 6; ++i) {
        hostEst.push_back(aggregate.hostLayerAbsorption[i].Json());
        particleEst.push_back(aggregate.particleLayerAbsorption[i].Json());
    }
    estimates.set("host_absorption_by_layer", hostEst);
    estimates.set("particle_absorption_by_layer", particleEst);
    output.set("estimates", estimates);
    return output;
}
void AtomicWrite(const std::string &path, const std::string &bytes) {
#ifdef _WIN32
    const auto destination = std::filesystem::u8path(path).wstring();
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        temporary = destination + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        Require(GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS,
                "unique Windows output creation failed");
    }
    Require(file != INVALID_HANDLE_VALUE, "unique Windows output sibling budget exhausted");
    bool installed = false;
    try {
        std::size_t position = 0;
        while (position < bytes.size()) {
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 1024 * 1024));
            DWORD written = 0;
            Require(WriteFile(file, bytes.data() + position, requested, &written, nullptr) &&
                        written > 0,
                    "Windows output write failed");
            position += written;
        }
        Require(FlushFileBuffers(file) != 0, "Windows output sync failed");
        const BOOL closed = CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        Require(closed != 0, "Windows output close failed");
        Require(MoveFileExW(temporary.c_str(), destination.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "atomic Windows output replacement failed");
        installed = true;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (!installed)
            DeleteFileW(temporary.c_str());
        throw;
    }
#else
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    Require(fd >= 0, "cannot create unique output sibling: " + std::string(std::strerror(errno)));
    bool installed = false;
    try {
        std::size_t position = 0;
        while (position < bytes.size()) {
            const auto count = write(fd, bytes.data() + position, bytes.size() - position);
            if (count < 0 && errno == EINTR)
                continue;
            Require(count > 0, "output write failed");
            position += static_cast<std::size_t>(count);
        }
        Require(fsync(fd) == 0, "output sync failed");
        const int closed = close(fd);
        fd = -1;
        Require(closed == 0, "output close failed");
        Require(rename(name.data(), path.c_str()) == 0, "atomic output replacement failed");
        installed = true;
    } catch (...) {
        if (fd >= 0)
            close(fd);
        if (!installed)
            unlink(name.data());
        throw;
    }
#endif
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            Usage();
            return 0;
        }
        const auto options = Parse(argc, argv);
        const auto resolved = Resolve(options);
        std::signal(SIGINT, Signal);
        std::signal(SIGTERM, Signal);
        std::cerr << "hylios_measure: signal handlers ready\n" << std::flush;
        const J output = Measure(options, resolved);
        // Complete serialization precedes any output mutation; temporary is unique.
        const auto bytes = RISE::Agent::JsonSerialize(output) + "\n";
        ValidateUnicode(bytes);
        AtomicWrite(options.output, bytes);
        std::cout << "Wrote diagnostic measurement for " << options.histories
                  << " incident histories to " << options.output << '\n';
        return interrupted ? 130 : 0;
    } catch (const std::exception &e) {
        std::cerr << "hylios_measure: " << e.what() << '\n';
        return 1;
    }
}
