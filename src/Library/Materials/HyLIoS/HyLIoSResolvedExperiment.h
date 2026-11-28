#pragma once
#include "HyLIoSForwardTransport.h"
#include "Agent/Json.h"
#include <string>
namespace RISE::HyLIoS {
enum class ExperimentBoundary { Smooth, HyLIoS2015 };
struct ResolveExperimentConfig {
    double wavelengthNm;
    ExperimentBoundary boundary = ExperimentBoundary::Smooth;
    bool explicitComplexPackV1 = false;
};
// Shared scalar-value UTF-8 validation for artifact publication.
void ValidateExperimentUnicode(const std::string& text);
class ResolvedExperiment;
// Parses these exact source bytes once, preserving raw provenance and selecting
// an exact wavelength node. No interpolation or biological defaults are added.
// Input/numeric failures throw std::exception-derived errors, including
// std::invalid_argument and std::runtime_error. Reject the experiment on any
// exception. The source byte budget is 64 MiB.
ResolvedExperiment ResolveExperiment(const std::string& sourceBytes, const ResolveExperimentConfig&);
// Value object exposes only const views; each experiment owns its resolved data.
class ResolvedExperiment {
public:
    const Agent::JsonValue& Original() const {return original;}
    const std::array<ForwardLayer, 6>& Layers() const {return layers;}
    const Agent::JsonValue& Selected() const {return selected;}
    const std::string& BytesHash() const {return bytesHash;}
    double SurfaceFoldAspect() const {return surfaceFoldAspect;}
    double WavelengthNm() const {return wavelengthNm;}
private:
    ResolvedExperiment() = default;
    friend ResolvedExperiment ResolveExperiment(const std::string&, const ResolveExperimentConfig&);
    Agent::JsonValue original;
    std::array<ForwardLayer, 6> layers{};
    Agent::JsonValue selected = Agent::JsonValue::MakeArray();
    std::string bytesHash;
    double surfaceFoldAspect = 0;
    double wavelengthNm = 0;
};
}
