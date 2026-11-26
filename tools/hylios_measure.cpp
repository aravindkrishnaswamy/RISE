#include "Agent/Json.h"
#include "Materials/HyLIoS/HyLIoSForwardTransport.h"
#include "Materials/HyLIoS/HyLIoSOptics.h"
#include "Materials/HyLIoS/HyLIoSSurfaceBoundary.h"
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
#include <optional>
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
constexpr const char *MeasurementVersion = "hylios-forward-measure-v6";
constexpr double MeasurementPi=3.141592653589793238462643383279502884;
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
    double incidentPolar=0,incidentAzimuth=0;
    double wavelength = 0, roulette = 0, fiberRadius = 1e-7, fiberFraction = .22,
           fiberEta = 1.5 / 1.33, exteriorIOR = 1;
    std::uint64_t histories = 0, maxTransitions = 0, seed = 0;
    J explicitOptions = J::MakeObject();
};
void Usage() {
    std::cout << "hylios_measure --specimen FILE --wavelength-nm N --placement fixed|null "
                 "--post-scatter diagnostic|discard --boundary smooth|hylios2015 --histories N --max-transitions N --seed U64 --output FILE\n"
                 "  [--incident-polar-deg N] [--incident-azimuth-deg N] [--complex-layout explicit-pack-v1] [--estimator analog|weighted] [--roulette-target N] [--exterior-ior N]\n"
                 "  [--fiber-radius-m N] [--fiber-volume-fraction N] [--fiber-relative-ior N]\n"
                 "Forward particle/complex transport supports explicit smooth or hylios2015 boundary selection; adjoint sampling is not "
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
                                           "--incident-polar-deg",
                                           "--incident-azimuth-deg",
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
    Require(o.boundary == "smooth" || o.boundary == "hylios2015",
            "only explicit --boundary smooth or hylios2015 is implemented");
    o.wavelength = Real(flags.at("--wavelength-nm"), "wavelength", 0, true);
    if(flags.count("--incident-polar-deg")) o.incidentPolar=Real(flags.at("--incident-polar-deg"),"incident-polar-deg");
    Require(o.incidentPolar<90,"incident-polar-deg must be in [0,90)");
    if(flags.count("--incident-azimuth-deg")) {
        o.incidentAzimuth=std::fmod(Real(flags.at("--incident-azimuth-deg"),"incident-azimuth-deg",-std::numeric_limits<double>::max()),360.);
        if(o.incidentAzimuth<0) o.incidentAzimuth+=360;
        if(o.incidentAzimuth==360) o.incidentAzimuth=0;
    }
    if(o.incidentPolar==0) o.incidentAzimuth=0; // exact normal launch and canonical pole azimuth
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
    double surfaceFoldAspect=0;
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
    r.surfaceFoldAspect=Number(source.get("surface_fold_aspect_ratio"), "surface fold aspect ratio");
    if(options.boundary=="hylios2015") {
        // Invalid authored fold values are input errors, before any histories.
        HyLIoSSurfaceBoundary{r.surfaceFoldAspect};
    }
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
// Nonzero observations are scaled by their running maximum. This is Welford,
// not a subtraction of squared sums: squared tiny/large physical weights never
// have to fit binary64. Zeros are merged analytically only at publication.
struct SparseSummary {
    double sumWeight=0,mean=0;
    std::optional<double> standardError;
    std::uint64_t nonzeroCount=0;
};
// Volatile materialization pins these binary64 operations even when the caller
// TU is built with reassociation. No rounded absolute mean feeds variance.
double RoundedMomentSum(double a,double b) {volatile double value=a+b;return value;}
double RoundedMomentDifference(double a,double b) {volatile double value=a-b;return value;}
double RoundedMomentQuotient(double a,double b) {volatile double value=a/b;return value;}
double RoundedMomentProduct(double a,double b) {volatile double value=a*b;return value;}
// All primitive rounding nodes are materialized so reassociation in the caller
// cannot erase a residual. The pair is a compensated value, not wider hardware FP.
struct MomentPair {double high=0,low=0;};
MomentPair MomentTwoSum(double a,double b) {
    const double sum=RoundedMomentSum(a,b),bv=RoundedMomentDifference(sum,a);
    const double error=RoundedMomentSum(RoundedMomentDifference(a,RoundedMomentDifference(sum,bv)),RoundedMomentDifference(b,bv));
    return {sum,error};
}
MomentPair MomentAdd(MomentPair a,MomentPair b) {
    const auto first=MomentTwoSum(a.high,b.high);
    return MomentTwoSum(first.high,RoundedMomentSum(first.low,RoundedMomentSum(a.low,b.low)));
}
MomentPair MomentSubtract(MomentPair a,MomentPair b) {return MomentAdd(a,{-b.high,-b.low});}
MomentPair MomentMultiply(MomentPair a,MomentPair b) {
    const double product=RoundedMomentProduct(a.high,b.high);
    double error=std::fma(a.high,b.high,-product);
    error=RoundedMomentSum(error,RoundedMomentProduct(a.high,b.low));
    error=RoundedMomentSum(error,RoundedMomentProduct(a.low,b.high));
    error=RoundedMomentSum(error,RoundedMomentProduct(a.low,b.low));
    return MomentTwoSum(product,error);
}
MomentPair MomentDivide(MomentPair a,MomentPair b) {
    const double first=RoundedMomentQuotient(a.high,b.high);
    const auto residual=MomentSubtract(a,MomentMultiply({first,0},b));
    const double second=RoundedMomentQuotient(RoundedMomentSum(residual.high,residual.low),b.high);
    return MomentTwoSum(first,second);
}
MomentPair MomentCount(std::uint64_t value) {
    return MomentTwoSum(static_cast<double>(value>>32)*0x1p32,static_cast<double>(value&0xffffffffULL));
}
double MomentValue(MomentPair value) {return RoundedMomentSum(value.high,value.low);}
bool FiniteMoment(MomentPair value) {return std::isfinite(value.high)&&std::isfinite(value.low);}
class SparseMoments {
public:
    std::uint64_t Count() const { return count_; }
    void Observe(double x) {
        Require(std::isfinite(x)&&x>=0,"measurement moment weight must be finite and nonnegative");
        if(x==0) return;
        Require(count_!=std::numeric_limits<std::uint64_t>::max(),"measurement moment count overflow");
        if(count_==0) {maximum_=minimum_=origin_=x;positiveMean_={1,0};count_=1;return;}
        if(x==maximum_&&x==minimum_) {++count_;return;}
        const auto next=MomentCount(count_+1);
        if(x>maximum_) {
            positiveMean_=MomentMultiply(positiveMean_,MomentDivide({maximum_,0},{x,0}));maximum_=x;
        }
        minimum_=std::min(minimum_,x);
        const auto positiveDelta=MomentSubtract(MomentDivide({x,0},{maximum_,0}),positiveMean_);
        positiveMean_=MomentAdd(positiveMean_,MomentDivide(positiveDelta,next));
        const double nextRange=RoundedMomentDifference(maximum_,minimum_);
        if(nextRange>range_) {
            const auto ratio=MomentDivide({range_,0},{nextRange,0});
            meanOffset_=MomentMultiply(meanOffset_,ratio);
            m2_=MomentMultiply(MomentMultiply(m2_,ratio),ratio);range_=nextRange;
        }
        if(range_>0) {
            const auto centered=MomentSubtract({x,0},{origin_,0});
            const auto value=MomentDivide(centered,{range_,0});
            const auto delta=MomentSubtract(value,meanOffset_);
            meanOffset_=MomentAdd(meanOffset_,MomentDivide(delta,next));
            const auto increment=MomentMultiply(MomentMultiply(delta,delta),MomentDivide(MomentCount(count_),next));
            m2_=MomentAdd(m2_,increment);
        }
        ++count_;
        Require(FiniteMoment(positiveMean_)&&MomentValue(positiveMean_)>0&&FiniteMoment(meanOffset_)&&
            FiniteMoment(m2_)&&MomentValue(m2_)>=0,"measurement centered moment range failure");
    }
    // Each group's own nonnegative fraction is used for its positive mean.
    // Subtraction-form blending loses a rare large contribution when a tiny
    // positive group dominates. Centered offsets and M2 retain their residuals.
    void MergeNonzero(const SparseMoments& other) {
        if(other.count_==0) return;
        if(count_==0) {*this=other;return;}
        Require(other.count_<=std::numeric_limits<std::uint64_t>::max()-count_,"measurement merged count overflow");
        const auto combined=count_+other.count_;
        const double maximum=std::max(maximum_,other.maximum_),minimum=std::min(minimum_,other.minimum_);
        const auto fractionA=MomentDivide(MomentCount(count_),MomentCount(combined));
        const auto fractionB=MomentDivide(MomentCount(other.count_),MomentCount(combined));
        const auto positiveA=MomentMultiply(positiveMean_,MomentDivide({maximum_,0},{maximum,0}));
        const auto positiveB=MomentMultiply(other.positiveMean_,MomentDivide({other.maximum_,0},{maximum,0}));
        const auto positiveMean=MomentAdd(MomentMultiply(positiveA,fractionA),MomentMultiply(positiveB,fractionB));
        const double range=RoundedMomentDifference(maximum,minimum);
        MomentPair meanOffset,m2;
        if(range>0) {
            const auto ratioA=MomentDivide({range_,0},{range,0}),ratioB=MomentDivide({other.range_,0},{range,0});
            const auto meanA=MomentMultiply(meanOffset_,ratioA);
            const auto meanB=MomentAdd(MomentDivide(MomentSubtract({other.origin_,0},{origin_,0}),{range,0}),
                MomentMultiply(other.meanOffset_,ratioB));
            const auto delta=MomentSubtract(meanB,meanA);
            meanOffset=MomentAdd(meanA,MomentMultiply(delta,fractionB));
            const auto cross=MomentMultiply(MomentMultiply(delta,delta),MomentMultiply(MomentCount(count_),fractionB));
            m2=MomentAdd(MomentAdd(MomentMultiply(MomentMultiply(m2_,ratioA),ratioA),
                MomentMultiply(MomentMultiply(other.m2_,ratioB),ratioB)),cross);
        }
        Require(FiniteMoment(positiveMean)&&MomentValue(positiveMean)>0&&FiniteMoment(meanOffset)&&
            FiniteMoment(m2)&&MomentValue(m2)>=0,"measurement merged centered moment range failure");
        maximum_=maximum;minimum_=minimum;positiveMean_=positiveMean;range_=range;meanOffset_=meanOffset;m2_=m2;count_=combined;
    }
    SparseSummary Summary(std::uint64_t total) const {
        Require(total>0&&total>=count_,"measurement denominator must cover all nonzero histories");
        SparseSummary result;result.nonzeroCount=count_;
        if(total>1) result.standardError=0;
        if(count_==0) return result;
        const auto n=MomentCount(total),nz=MomentCount(count_);
        result.sumWeight=ScaleProduct(MomentMultiply(positiveMean_,nz),"measurement sum weight outside binary64 range");
        result.mean=ScaleProduct(MomentMultiply(positiveMean_,MomentDivide(nz,n)),"measurement mean outside binary64 range");
        if(total>1) {
            const auto zeroFraction=MomentDivide(MomentCount(total-count_),n);
            const auto rangeRatio=MomentDivide({range_,0},{maximum_,0});
            const auto nonzeroM2=MomentMultiply(MomentMultiply(m2_,rangeRatio),rangeRatio);
            const auto zeroM2=MomentMultiply(MomentMultiply(positiveMean_,positiveMean_),MomentMultiply(nz,zeroFraction));
            const auto variance=MomentDivide(MomentDivide(MomentAdd(nonzeroM2,zeroM2),n),MomentCount(total-1));
            result.standardError=ScaleProduct({std::sqrt(MomentValue(variance)),0},"measurement standard error outside binary64 range");
        }
        return result;
    }
private:
    std::uint64_t count_=0;
    double maximum_=0,minimum_=0,origin_=0,range_=0;
    MomentPair positiveMean_,meanOffset_,m2_;
    double ScaleProduct(MomentPair value,const char* message) const {
        volatile double product=maximum_*value.high;
        Require(std::isfinite(product),message);
        const double error=RoundedMomentSum(std::fma(maximum_,value.high,-product),RoundedMomentProduct(maximum_,value.low));
        const double result=RoundedMomentSum(product,error);
        Require(std::isfinite(result)&&(MomentValue(value)==0||result>0),message);return result;
    }
};

// All scalar channels and histogram cells use the same nonnegative moments;
// otherwise tiny physical weights could have a representable SE but an
// unrepresentable squared-weight variance in a second implementation.
struct Moments {
    std::uint64_t n=0;
    SparseMoments values;
    void Add(double x) {
        Require(n!=std::numeric_limits<std::uint64_t>::max(),"measurement history count overflow");
        values.Observe(x);++n;
    }
    SparseSummary Summary() const {return values.Summary(n);}
    J Json() const {
        const auto summary=Summary();J result=J::MakeObject();
        Put(result,"mean",summary.mean);
        result.set("standard_error",summary.standardError ? Num(*summary.standardError):J::MakeNull());
        Put(result,"history_count",static_cast<double>(n));return result;
    }
};
constexpr std::size_t ExitRadialBins=8,ExitAzimuthBins=8,ExitCosineBins=8;
constexpr std::size_t ExitCellCount=ExitRadialBins*ExitAzimuthBins*ExitCosineBins;
constexpr std::array<double,ExitRadialBins> ExitRadialEdges{{0,.0001,.0005,.001,.002,.005,.01,.02}};
std::array<double,ExitAzimuthBins+1> ExitPhiEdges() {
    std::array<double,ExitAzimuthBins+1> edges{};
    for(std::size_t i=0;i<edges.size();++i) edges[i]=2*MeasurementPi*static_cast<double>(i)/ExitAzimuthBins;
    return edges;
}
std::array<double,ExitCosineBins+1> ExitMuEdges() {
    std::array<double,ExitCosineBins+1> edges{};
    for(std::size_t i=0;i<edges.size();++i) edges[i]=static_cast<double>(i)/ExitCosineBins;
    return edges;
}
template<std::size_t N> J NumberArray(const std::array<double,N>& values) {
    J result=J::MakeArray();for(double x:values) result.push_back(Num(x));return result;
}
template<std::size_t N> std::size_t FiniteCell(double value,const std::array<double,N>& edges,bool includeUpper=false) {
    Require(std::isfinite(value)&&value>=edges.front()&&value<=edges.back(),"exit histogram angular coordinate outside domain");
    if(value==edges.back()) {
        Require(includeUpper,"exit histogram upper angular endpoint is excluded");return N-2;
    }
    return static_cast<std::size_t>(std::upper_bound(edges.begin(),edges.end(),value)-edges.begin()-1);
}
struct ExitCell {std::size_t radial,azimuth,cosine;bool radiusUnrepresentable;};
ExitCell LocateExit(SlabVector position,SlabVector direction) {
    Require(std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z),"exit histogram position is nonfinite");
    Require(std::isfinite(direction.x)&&std::isfinite(direction.y)&&std::isfinite(direction.z)&&direction.z<=0,"exit histogram direction is not exterior");
    const double length=std::hypot(direction.x,direction.y,direction.z);
    Require(std::isfinite(length)&&length>0,"exit histogram direction has unsupported norm");
    const double mu=-direction.z/length;
    const auto muEdges=ExitMuEdges();const auto phiEdges=ExitPhiEdges();
    double angle=direction.x==0&&direction.y==0 ? 0:std::atan2(direction.y,direction.x);
    const bool negativeAngle=angle<0;
    if(negativeAngle) angle+=2*MeasurementPi;
    // An original negative angle may round to 2pi after wrapping. It still
    // approaches the periodic seam from below, so belongs in the final bin.
    const auto azimuth=negativeAngle&&angle==2*MeasurementPi ? ExitAzimuthBins-1:FiniteCell(angle,phiEdges);
    const double radius=std::hypot(position.x,position.y);
    const bool unrepresentable=!std::isfinite(radius);
    std::size_t radial=ExitRadialBins-1;
    if(!unrepresentable&&radius<ExitRadialEdges.back())
        radial=static_cast<std::size_t>(std::upper_bound(ExitRadialEdges.begin(),ExitRadialEdges.end(),radius)-ExitRadialEdges.begin()-1);
    return {radial,azimuth,FiniteCell(mu,muEdges,true),unrepresentable};
}
std::size_t ExitIndex(std::size_t radius,std::size_t phi,std::size_t mu) {
    Require(radius<ExitRadialBins&&phi<ExitAzimuthBins&&mu<ExitCosineBins,"exit histogram cell index outside grid");
    return (radius*ExitAzimuthBins+phi)*ExitCosineBins+mu;
}
J SparseJson(const SparseMoments& moments,std::uint64_t total) {
    const auto summary=moments.Summary(total);J result=J::MakeObject();
    Put(result,"sum_weight",summary.sumWeight);Put(result,"mean",summary.mean);
    result.set("standard_error",summary.standardError ? Num(*summary.standardError):J::MakeNull());
    Put(result,"history_count",static_cast<double>(total));
    Put(result,"nonzero_history_count",static_cast<double>(summary.nonzeroCount));return result;
}
class ExitHistogram {
public:
    void Observe(SlabVector position,SlabVector direction,double weight,bool external) {
        Require(std::isfinite(weight)&&weight>=0,"exit histogram weight must be finite and nonnegative");
        if(weight==0) return;
        const auto cell=LocateExit(position,direction);const auto index=ExitIndex(cell.radial,cell.azimuth,cell.cosine);
        cells_[0][index].Observe(weight);cells_[external ? 1:2][index].Observe(weight);nonzeroCellUpdates_+=2;
        if(cell.radiusUnrepresentable) ++unrepresentableRadiusCount_;
    }
    J Json(std::uint64_t total) const {
        J result=J::MakeObject();
        result.set("version",Str("hylios_exit_radius_azimuth_cosine_cells_v1"));
        result.set("normalization",Str("all incident histories, including absorbed, unresolved, roulette-killed and unstarted-cancelled zeros"));
        Put(result,"history_count",static_cast<double>(total));
        Put(result,"nonzero_cell_updates",static_cast<double>(nonzeroCellUpdates_));
        result.set("statistics_accumulation",Str("scaled nonzero Welford; two joint-cell updates per positive terminal escape, missing zero histories merged at publication; marginals merge disjoint joint observations"));
        result.set("conditional_on_escape",Bool(false));result.set("is_density",Bool(false));
        result.set("angular_measure_classification",Str("unclassified mixture; angular atoms may be present; no lineage inferred from terminal name or rounded direction"));
        result.set("continuous_pdf_available",Bool(false));result.set("production_bake_available",Bool(false));
        result.set("axis_order",Str("radius, outgoing azimuth, outgoing cosine; cosine index varies fastest"));
        result.set("cell_mass_measure",Str("returned physical energy per incident history in a radial annulus and outgoing angular cell; exit-position azimuth integrated out"));
        result.set("interval_convention",Str("lower inclusive, upper exclusive; cosine 1 included in last cell; radial last bin is unbounded overflow"));
        result.set("azimuth_convention",Str("atan2(outgoing.y,outgoing.x) from global +x toward +y, wrapped [0,2pi); exact pole x=y=0 uses phi=0"));
        result.set("cosine_convention",Str("outward macro normal (0,0,-1); mu=-direction.z/hypot(direction), removing only stored unit-length rounding"));
        result.set("radial_edges_m",NumberArray(ExitRadialEdges));
        const auto phiEdges=ExitPhiEdges();const auto muEdges=ExitMuEdges();
        result.set("outgoing_azimuth_edges_rad",NumberArray(phiEdges));result.set("outgoing_cosine_edges",NumberArray(muEdges));
        Put(result,"radial_bin_count",ExitRadialBins);Put(result,"azimuth_bin_count",ExitAzimuthBins);Put(result,"cosine_bin_count",ExitCosineBins);
        J overflow=J::MakeObject();Put(overflow,"index",ExitRadialBins-1);Put(overflow,"lower_edge_m",ExitRadialEdges.back());
        overflow.set("upper_edge_m",J::MakeNull());overflow.set("includes_unrepresentable_finite_position_radius",Bool(true));
        result.set("radial_overflow",overflow);Put(result,"unrepresentable_radius_history_count",static_cast<double>(unrepresentableRadiusCount_));
        result.set("density_caution",Str("masses are not finite PDFs or BSDF values; finite cell measures do not classify atomic or singular components; overflow has no finite radial measure"));
        J joint=J::MakeArray();
        std::array<std::array<SparseMoments,ExitAzimuthBins*ExitCosineBins>,3> angular{};
        std::array<std::array<SparseMoments,ExitRadialBins>,3> radial{};
        for(std::size_t r=0;r<ExitRadialBins;++r) for(std::size_t p=0;p<ExitAzimuthBins;++p) for(std::size_t m=0;m<ExitCosineBins;++m) {
            const auto index=ExitIndex(r,p,m);J cell=J::MakeObject();
            Put(cell,"index",index);Put(cell,"radial_bin",r);Put(cell,"azimuth_bin",p);Put(cell,"cosine_bin",m);
            const double dphi=phiEdges[p+1]-phiEdges[p],dmu=muEdges[m+1]-muEdges[m];
            Put(cell,"solid_angle_sr",dmu*dphi);
            Put(cell,"projected_angle_measure_sr",.5*dmu*(muEdges[m+1]+muEdges[m])*dphi);
            if(r+1<ExitRadialEdges.size()) {
                const double width=ExitRadialEdges[r+1]-ExitRadialEdges[r];
                const double area=MeasurementPi*width*(ExitRadialEdges[r+1]+ExitRadialEdges[r]);
                Put(cell,"radial_width_m",width);Put(cell,"annular_area_m2",area);Put(cell,"area_solid_angle_m2_sr",area*dmu*dphi);
            } else {
                cell.set("radial_width_m",J::MakeNull());cell.set("annular_area_m2",J::MakeNull());cell.set("area_solid_angle_m2_sr",J::MakeNull());
            }
            AddChannels(cell,cells_,index,total);
            for(std::size_t channel=0;channel<3;++channel) {
                angular[channel][p*ExitCosineBins+m].MergeNonzero(cells_[channel][index]);
                radial[channel][r].MergeNonzero(cells_[channel][index]);
            }
            joint.push_back(cell);
        }
        result.set("joint_cells",joint);J angularJson=J::MakeArray(),radialJson=J::MakeArray();
        for(std::size_t p=0;p<ExitAzimuthBins;++p) for(std::size_t m=0;m<ExitCosineBins;++m) {
            J cell=J::MakeObject();Put(cell,"azimuth_bin",p);Put(cell,"cosine_bin",m);
            AddChannels(cell,angular,p*ExitCosineBins+m,total);angularJson.push_back(cell);
        }
        for(std::size_t r=0;r<ExitRadialBins;++r) {
            J cell=J::MakeObject();Put(cell,"radial_bin",r);AddChannels(cell,radial,r,total);radialJson.push_back(cell);
        }
        result.set("angular_marginal",angularJson);result.set("radial_marginal",radialJson);
        result.set("uncertainty_interpretation",Str("per-bin sample standard error of realised terminal escaped contribution over all incident histories; unresolved future response is not resolved by this SE"));
        return result;
    }
private:
    std::array<std::array<SparseMoments,ExitCellCount>,3> cells_{};
    std::uint64_t unrepresentableRadiusCount_=0,nonzeroCellUpdates_=0;
    template<std::size_t N> static void AddChannels(J& cell,const std::array<std::array<SparseMoments,N>,3>& moments,std::size_t index,std::uint64_t total) {
        cell.set("all_returns",SparseJson(moments[0][index],total));
        cell.set("external_reflection",SparseJson(moments[1][index],total));
        cell.set("subsurface_return",SparseJson(moments[2][index],total));
    }
};

struct SurfaceWork {
    std::uint64_t workUnits=0, branchAttempts=0, normalProposalAttempts=0, normalRejections=0;
    std::uint64_t exteriorReflectionBranches=0, exteriorTransmissionBranches=0;
    std::uint64_t interiorReflectionBranches=0, interiorTransmissionBranches=0;
    std::uint64_t readyDecisions=0, budgetStops=0, cancellations=0;
    std::uint64_t invalidSamples=0, numericalFailures=0;
};
struct InvalidBoundarySample : std::runtime_error {
    InvalidBoundarySample():std::runtime_error("invalid used surface-boundary random variate") {}
};
double BoundaryDraw(RISE::ISampler& rng) {
    const double u=rng.Get1D();
    if(!std::isfinite(u)||u<0||u>=1) throw InvalidBoundarySample();
    return u;
}
// The adapter owns the outer law, while the transport owns all live power and
// absorption. Boundary diagnostics therefore cancel the pending transport and
// explicitly override only the report's terminal classification below.
ForwardTermination RunFoldBoundaryHistory(const HyLIoSForwardTransport& kernel,
    const HyLIoSSurfaceBoundary& law,ForwardState& state,RISE::ISampler& rng,
    std::uint64_t limit,SurfaceWork& counters,bool& progressAnnounced) {
    std::optional<SurfaceBoundaryState> pending;
    ForwardTermination diagnostic=ForwardTermination::None;
    const auto StopBoundary=[&](bool cancel) {
        if(!pending||(pending->Phase()!=SurfaceBoundaryPhase::Branch&&pending->Phase()!=SurfaceBoundaryPhase::NormalProposal)) return;
        if(cancel) {law.Cancel(*pending);++counters.cancellations;}
        else {law.StopForBudget(*pending);++counters.budgetStops;}
    };
    while(state.Termination()==ForwardTermination::None&&
        state.Counters().workUnits+counters.workUnits<limit&&!interrupted) {
        if(!state.HasPendingBoundary()) {
            kernel.Advance(state,rng,static_cast<std::size_t>(std::min<std::uint64_t>(64,
                limit-state.Counters().workUnits-counters.workUnits)));
            continue;
        }
        try {
            if(!pending) pending.emplace(law.Begin(state.PendingBoundary()));
            const bool branch=pending->Phase()==SurfaceBoundaryPhase::Branch;
            const auto side=pending->Encounter().incidentSide;
            // Caller work counts every attempted transition, including failures.
            ++counters.workUnits;
            if(branch) ++counters.branchAttempts;else ++counters.normalProposalAttempts;
            const double u=BoundaryDraw(rng);
            double v=0,acceptance=0;
            if(!branch) {v=BoundaryDraw(rng);acceptance=BoundaryDraw(rng);}
            const auto phase=law.TrySample(*pending,u,v,acceptance);
            if(branch) {
                const bool reflected=phase==SurfaceBoundaryPhase::NormalProposal||pending->Decision().action==BoundaryAction::Reflect;
                if(side==SlabSide::Exterior) {
                    if(reflected) ++counters.exteriorReflectionBranches;else ++counters.exteriorTransmissionBranches;
                } else {
                    if(reflected) ++counters.interiorReflectionBranches;else ++counters.interiorTransmissionBranches;
                }
                if(phase==SurfaceBoundaryPhase::NormalProposal&&!progressAnnounced) {
                    std::cerr<<"hylios_measure: reflected surface branch pending\n"<<std::flush;
                    progressAnnounced=true;
                }
            } else if(phase==SurfaceBoundaryPhase::NormalProposal) ++counters.normalRejections;
            if(phase==SurfaceBoundaryPhase::Ready) {
                ++counters.readyDecisions;
                kernel.ResumeBoundary(state,pending->Encounter().token,pending->Decision());
                pending.reset();
            }
        } catch(const InvalidBoundarySample&) {
            ++counters.invalidSamples;diagnostic=ForwardTermination::InvalidSample;
            StopBoundary(true);kernel.Cancel(state);
        } catch(const std::exception&) {
            ++counters.numericalFailures;diagnostic=ForwardTermination::NumericalFailure;
            StopBoundary(true);kernel.Cancel(state);
        }
    }
    if(state.Termination()==ForwardTermination::None) {
        if(interrupted) {StopBoundary(true);kernel.Cancel(state);}
        else {StopBoundary(false);kernel.StopForBudget(state);}
    }
    return diagnostic;
}
struct Aggregate {
    std::map<std::string, std::uint64_t> counts, failureDetails;
    std::map<std::string, double> outcomeWeights, counters, surfaceCounters;
    std::array<double, 6> hostAbs{}, particleAbs{}, layerPath{}, hostPath{}, particlePath{};
    Moments reflectance, external, subsurface, absorption, unresolved, accounted;
    std::array<Moments, 6> hostLayerAbsorption, particleLayerAbsorption;
    ExitHistogram exits;
    std::array<Moments,3> returnedAngularMass;
    std::array<std::uint64_t,3> returnedAngularCounts{};
    std::uint64_t draws = 0,callerWork=0;
    void Add(const ForwardState &s, std::uint64_t randomDraws,const SurfaceWork& surface,
             ForwardTermination boundaryDiagnostic) {
        const auto term = boundaryDiagnostic==ForwardTermination::None ? s.Termination():boundaryDiagnostic;
        callerWork+=s.Counters().workUnits+surface.workUnits;
#define ADD_SURFACE(name) surfaceCounters[#name]+=static_cast<double>(surface.name)
        ADD_SURFACE(workUnits);ADD_SURFACE(branchAttempts);ADD_SURFACE(normalProposalAttempts);
        ADD_SURFACE(normalRejections);ADD_SURFACE(exteriorReflectionBranches);ADD_SURFACE(exteriorTransmissionBranches);
        ADD_SURFACE(interiorReflectionBranches);ADD_SURFACE(interiorTransmissionBranches);ADD_SURFACE(readyDecisions);
        ADD_SURFACE(budgetStops);ADD_SURFACE(cancellations);ADD_SURFACE(invalidSamples);ADD_SURFACE(numericalFailures);
#undef ADD_SURFACE
        const auto outcome = Outcome(term);
        if(Escaped(term)) exits.Observe(s.Position(),s.Direction(),s.Weight(),term==ForwardTermination::ExternalReflection);
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
            ++failureDetails[boundaryDiagnostic==ForwardTermination::NumericalFailure ? "surface_boundary":name];
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
        // Disjoint RETURNED law classes; unfinished and nonreturn trials are
        // zeros in the same all-history denominator. No direction comparison.
        std::size_t law=2;
        if(Escaped(term)) {
            if(s.ReturnedAtomMap()==ForwardReturnedAtomMap::PlanarSameExteriorMirror) law=0;
            else if(s.AngularLineage().classification==ForwardAngularClass::AbsolutelyContinuous) law=1;
            ++returnedAngularCounts[law];
        }
        for(std::size_t i=0;i<returnedAngularMass.size();++i)
            returnedAngularMass[i].Add(Escaped(term)&&i==law ? s.Weight():0);
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
    std::optional<HyLIoSSurfaceBoundary> surfaceLaw;
    if(o.boundary=="hylios2015") surfaceLaw.emplace(r.surfaceFoldAspect);
    bool boundaryProgressAnnounced=false;
    Aggregate aggregate;
    const double theta=o.incidentPolar*MeasurementPi/180,phi=o.incidentAzimuth*MeasurementPi/180;
    const SlabVector incoming=o.incidentPolar==0 ? SlabVector{0,0,1} :
        SlabVector{-std::sin(theta)*std::cos(phi),-std::sin(theta)*std::sin(phi),std::cos(theta)};
    SlabVector actualLaunch=incoming;
    for (std::uint64_t history = 0; history < o.histories; ++history) {
        Random rng(o.seed + history);
        auto s = kernel.Begin({0, 0, 0}, incoming);
        if(history==0) actualLaunch=s.Direction();
        SurfaceWork surface;
        ForwardTermination boundaryDiagnostic=ForwardTermination::None;
        if(surfaceLaw) boundaryDiagnostic=RunFoldBoundaryHistory(kernel,*surfaceLaw,s,rng,o.maxTransitions,surface,boundaryProgressAnnounced);
        else {
        // Keep smooth's legacy RNG, cap and boundary-dispatch ordering exact.
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
        }
        aggregate.Add(s, rng.draws,surface,boundaryDiagnostic);
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
    output.set("boundary_law", Str(surfaceLaw ? SurfaceBoundaryVersion:"smooth_planar_dielectric_override_v1"));
    output.set("authored_surface_folds_simulated", Bool(surfaceLaw.has_value()));
    output.set("boundary_work_convention",Str(surfaceLaw ?
        "transport transitions plus every surface branch/proposal attempt; pending branch retained":
        "legacy smooth diagnostic: transport transitions only; smooth boundary draws excluded"));
    output.set("boundary_failure_convention",Str(surfaceLaw ?
        "pending transport cancelled to preserve live power; adapter classifies boundary invalid/numerical failure separately":
        "legacy smooth diagnostic: helper errors abort before output replacement"));
    if(surfaceLaw) Put(output,"surface_polar_normalization",surfaceLaw->Normalization());
    else output.set("surface_polar_normalization",J::MakeNull());
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
    output.set("incident_measurement_rule",Str("one fixed outward-source direction per measurement; no incident-cell jitter or averaging"));
    output.set("incident_direction",Str("incoming travel is opposite outward source wi; launch at local origin; source polar from outward (0,0,-1), azimuth +x toward +y"));
    Put(output,"incident_polar_degrees",o.incidentPolar);Put(output,"incident_source_azimuth_degrees",o.incidentAzimuth);
    output.set("incident_azimuth_defined",Bool(o.incidentPolar!=0));
    output.set("incident_source_direction",NumberArray(std::array<double,3>{{-actualLaunch.x,-actualLaunch.y,-actualLaunch.z}}));
    output.set("incoming_travel_direction",NumberArray(std::array<double,3>{{actualLaunch.x,actualLaunch.y,actualLaunch.z}}));
    output.set("direction_frame",Str("global +x/+y azimuth with +x cross +y=+z inward; outward macro normal is -z; do not treat incoming travel as outward source wi"));
    output.set("exit_distribution",aggregate.exits.Json(o.histories));
    J angular=J::MakeObject();
    angular.set("version",Str(ForwardAngularLineageVersion));
    angular.set("interpretation",Str("ensemble laws conditional on incident direction; returned mass only; not an angular PDF or production bake certificate"));
    angular.set("unknown_policy",Str("positive unclassified returned mass must not be histogrammed as finite f or counted as absorption in a production bake"));
    const std::array<const char*,3> lawNames{{"planar_same_exterior_mirror","absolutely_continuous","unclassified_singular"}};
    for(std::size_t i=0;i<lawNames.size();++i) {
        J channel=aggregate.returnedAngularMass[i].Json();
        Put(channel,"returned_history_count",static_cast<double>(aggregate.returnedAngularCounts[i]));
        angular.set(lawNames[i],channel);
    }
    output.set("returned_angular_law",angular);
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
    J surfaceCounters=J::MakeObject();
    for(const auto& pair:aggregate.surfaceCounters) Put(surfaceCounters,pair.first,pair.second);
    output.set("surface_boundary_counters",surfaceCounters);
    Put(output,"caller_work_units",static_cast<double>(aggregate.callerWork));
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
        aggregate.reflectance.Summary().mean + aggregate.unresolved.Summary().mean);
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
