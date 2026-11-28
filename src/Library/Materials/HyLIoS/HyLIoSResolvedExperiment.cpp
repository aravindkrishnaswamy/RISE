#include "pch.h"
#include "HyLIoSResolvedExperiment.h"
#include "HyLIoSOptics.h"
#include "HyLIoSSurfaceBoundary.h"
#include "Utilities/RISECBOR64.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace RISE::HyLIoS {
namespace {
using J = RISE::Agent::JsonValue;
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
J Num(double value) {
    Require(std::isfinite(value), "nonfinite result cannot be published");
    return J::MakeNumber(value);
}
J Str(const std::string& value) { return J::MakeString(value); }
void Put(J& object, const std::string& key, double value) {object.set(key, Num(value));}
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
    // Reuse the codec's private C-locale conversion without changing locale.
    J parsed;
    std::string error;
    Require(RISE::Agent::JsonParse(token, parsed, error) && parsed.isNumber(), "invalid JSON number");
    const double value = parsed.asNumber();
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
std::vector<double> Spectrum(const J &values, std::size_t size, const std::string &label,
                             bool positive = false) {
    Require(values.isArray() && values.size() == size, label + " wavelength shape mismatch");
    std::vector<double> v;
    for (std::size_t i = 0; i < size; ++i)
        v.push_back(Number(values.at(i), label, 0, positive));
    return v;
}
}
void ValidateExperimentUnicode(const std::string& text) {ValidateUnicode(text);}
ResolvedExperiment ResolveExperiment(const std::string& bytes, const ResolveExperimentConfig& options) {
    Require(bytes.size() <= 64 * 1024 * 1024, "specimen exceeds 64MiB input budget");
    Require(std::isfinite(options.wavelengthNm) && options.wavelengthNm > 0,
            "wavelength outside finite supported range");
    Require(options.boundary == ExperimentBoundary::Smooth || options.boundary == ExperimentBoundary::HyLIoS2015,
            "unsupported experiment boundary");
    ResolvedExperiment r;
    r.wavelengthNm = options.wavelengthNm;
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
    if(options.boundary==ExperimentBoundary::HyLIoS2015) {
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
        if (wave == options.wavelengthNm)
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
                Require(!complex || options.explicitComplexPackV1,
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
    ValidateForwardLayers(r.layers);
    r.bytesHash = RISE::RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char *>(bytes.data()),
                                              bytes.size());
    return r;
}
}
