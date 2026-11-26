#include "Materials/HyLIoS/HyLIoSResolvedExperiment.h"
#include "Materials/HyLIoS/HyLIoSForwardExperiment.h"
#include "Utilities/RISECBOR64.h"
#include <cmath>
#include <clocale>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace RISE::HyLIoS;
using J=RISE::Agent::JsonValue;
namespace {
void Check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
J Fixture() {
    J result;std::string error;
    const std::string head=R"({"schema_version":1,"approximate":true,"composition_revision":"synthetic-v1","coverage_profile_id":"explicit-synthetic-approximation","spectral_bundle_sha256":"a","resolved_specimen_sha256":"b","preset_id":"synthetic","interpretation":"synthetic controls","units":{"length":"m","coefficient":"m^-1","wavelength":"nm","mass_concentration":"kg/m^3"},"wavelength_nm":[550,1400],"surface_fold_aspect_ratio":0.75,"provenance":{"retained":"yes"})";
    Check(RISE::Agent::JsonParse(head+"}",result,error),"fixture parse");
    const char* names[]={"stratum_corneum","stratum_granulosum","stratum_spinosum","stratum_basale","papillary_dermis","reticular_dermis"};
    J layers=J::MakeArray();
    for(auto name:names) {
        J layer;const std::string text=std::string("{\"layer_id\":\"")+name+R"(","thickness_m":0.001,"host_absorption_m_inv":[0,2],"real_refractive_index":[1,1.5],"constituent_absorption_m_inv":{"synthetic":[0,2]},"particles":null})";
        Check(RISE::Agent::JsonParse(text,layer,error),"layer fixture parse");layers.push_back(layer);
    }
    result.set("layers",layers);return result;
}
void Replace(J& object,const std::string& key,const J& value) {
    J next=J::MakeObject();for(const auto& member:object.members())
        if(member.first!=key) next.set(member.first,member.second);
    next.set(key,value);object=next;
}
void ReplaceLayer(J& specimen,std::size_t index,const std::string& key,const J& value) {
    J layers=J::MakeArray();for(std::size_t i=0;i<specimen.get("layers").size();++i) {
        J layer=specimen.get("layers").at(i);if(i==index) Replace(layer,key,value);layers.push_back(layer);
    }
    Replace(specimen,"layers",layers);
}
void Reject(const std::string& bytes,const char* expected,ResolveExperimentConfig config={550}) {
    bool rejected=false;
    try {ResolveExperiment(bytes,config);} catch(const std::exception& e) {
        rejected=std::string(e.what()).find(expected)!=std::string::npos;
        if(!rejected) std::cerr<<"unexpected rejection: "<<e.what()<<'\n';
    }
    Check(rejected,expected);
}
void RejectLayerValidation(const std::array<ForwardLayer,6>& layers,const char* expected) {
    bool rejected=false;
    try {ValidateForwardLayers(layers);}
    catch(const std::exception& error) {
        rejected=std::string(error.what()).find(expected)!=std::string::npos;
    }
    Check(rejected,"documented std::exception geometry rejection was not catchable");
}
class ConstantSampler : public RISE::ISampler {
public:
    explicit ConstantSampler(double value):value_(value) {}
    double Get1D() override {++draws;return value_;}
    RISE::Point2 Get2D() override {return {Get1D(),Get1D()};}
    unsigned draws=0;
private:
    double value_;
};
class FixedSampler final : public ConstantSampler {
public:
    FixedSampler():ConstantSampler(0) {}
    bool HasFixedDimensionBudget() const override {return true;}
};
struct CancelContext {bool cancel=false;unsigned notifications=0;};
}
int main() {
    try {
        const auto fixture=Fixture();const auto bytes=RISE::Agent::JsonSerialize(fixture)+"\n";
        const auto selected=ResolveExperiment(bytes,{1400});
        Check(RISE::Agent::JsonSerialize(selected.Original())==RISE::Agent::JsonSerialize(fixture),"original metadata differs");
        Check(selected.BytesHash()==RISE::RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size()),"input hash differs");
        Check(selected.SurfaceFoldAspect()==.75&&selected.Layers().size()==6,"resolved geometry differs");
        for(const auto& layer:selected.Layers()) Check(layer.hostAbsorptionPerMetre==2&&layer.realIOR==1.5&&layer.thicknessMetres==.001&&!layer.organelle,"selected node differs");
        Check(selected.Selected().at(3).get("host_absorption_m_inv").asNumber()==2,"selected record differs");
        Reject(bytes,"exact resolved coverage",{1000});
        Reject(bytes,"wavelength outside",{std::numeric_limits<double>::quiet_NaN()});
        Reject(bytes,"unsupported experiment boundary",{550,static_cast<ExperimentBoundary>(999)});
        Reject("{\"x\":9007199254740993}","2^53");
        Reject("{\"x\":1e-9999}","underflows");
        Reject("{\"x\":1,\"x\":2}","duplicate JSON key");
        Reject(std::string("{\"x\":\"")+char(0xc0)+char(0xaf)+"\"}","Unicode");
        Reject("{\"x\":\"\\ud800\"}","Unicode");
        Reject(bytes+" trailing","invalid specimen JSON");
        J invalid=fixture;Replace(invalid,"coverage_profile_id",J::MakeString("strict_source_coverage_v1"));
        Reject(RISE::Agent::JsonSerialize(invalid),"contradicts approximate");
        invalid=fixture;J nested=J::MakeObject(),bundle=J::MakeObject();bundle.set("approximate",J::MakeBool(false));nested.set("spectral_bundle",bundle);Replace(invalid,"provenance",nested);
        Reject(RISE::Agent::JsonSerialize(invalid),"nested approximation");
        invalid=fixture;Replace(invalid,"surface_fold_aspect_ratio",J::MakeNumber(1e-20));
        ResolveExperiment(RISE::Agent::JsonSerialize(invalid),{550}); // Smooth preserves authored metadata.
        Reject(RISE::Agent::JsonSerialize(invalid),"fold",{550,ExperimentBoundary::HyLIoS2015});
        invalid=fixture;ReplaceLayer(invalid,0,"thickness_m",J::MakeNumber(9007199254740992.));
        Reject(RISE::Agent::JsonSerialize(invalid),"layer planes not representably separated");
        J particle;std::string particleError;
        Check(RISE::Agent::JsonParse(R"({"enabled":true,"organelle_fraction":0.01,"organization":"individually_dispersed_particles","interior_absorption_evaluated":true,"interior_absorption_m_inv":[0,0],"semi_minor_m":0.001,"semi_major_m":0.001,"real_refractive_index":1})",particle,particleError),"particle fixture");
        invalid=fixture;ReplaceLayer(invalid,0,"particles",particle);
        Reject(RISE::Agent::JsonSerialize(invalid),"positive-measure spheroid placements");
        Replace(particle,"organization",J::MakeString("complexes"));particle.set("complex_diameter_m",J::MakeNumber(.002));particle.set("maximum_internal_count",J::MakeNumber(1));
        invalid=fixture;ReplaceLayer(invalid,0,"particles",particle);
        Reject(RISE::Agent::JsonSerialize(invalid),"positive-measure envelopes",{550,ExperimentBoundary::Smooth,true});
        const std::string savedLocale=std::setlocale(LC_NUMERIC,nullptr);
        if(std::setlocale(LC_NUMERIC,"de_DE.UTF-8")) {
            const std::string active=std::setlocale(LC_NUMERIC,nullptr);
            const auto localized=ResolveExperiment(bytes,{550});
            Check(localized.Layers()[0].thicknessMetres==.001,"resolver depends on numeric locale");
            Check(active==std::setlocale(LC_NUMERIC,nullptr),"resolver mutated numeric locale");
        }
        std::setlocale(LC_NUMERIC,savedLocale.c_str());
        ForwardExperimentRandom rng(0);
        Check(rng.Get1D()==static_cast<double>(0xe220a8397b1dcdafULL>>11)*0x1p-53,"SplitMix64 reference output differs");
        Check(rng.draws==1,"RNG draw accounting differs");
        const auto resolved=ResolveExperiment(bytes,{550});
        auto invalidLayers=resolved.Layers();invalidLayers[0].thicknessMetres=9007199254740992.;
        RejectLayerValidation(invalidLayers,"layer planes not representably separated");
        invalidLayers=resolved.Layers();
        invalidLayers[0].organelle=ForwardOrganelle{ForwardOrganization::IndividualSpheroids,.01,.001,.001,0,1};
        RejectLayerValidation(invalidLayers,"positive-measure spheroid placements");
        invalidLayers=resolved.Layers();
        ForwardOrganelle tinyComplex{ForwardOrganization::ExplicitComplexPackV1,.01,1e-40,1e-40,0,1};
        tinyComplex.envelopeRadiusMetres=1e-4;tinyComplex.childCount=1;
        invalidLayers[0].organelle=tinyComplex;
        RejectLayerValidation(invalidLayers,"unsupported complex dimension spread");
        // This delegated complex range_error also belongs to the documented
        // std::exception family accepted by the resolver caller's boundary.
        Replace(particle,"semi_minor_m",J::MakeNumber(1e-40));
        Replace(particle,"semi_major_m",J::MakeNumber(1e-40));
        invalid=fixture;ReplaceLayer(invalid,0,"particles",particle);
        Reject(RISE::Agent::JsonSerialize(invalid),"unsupported complex dimension spread",{550,ExperimentBoundary::Smooth,true});
        HyLIoSForwardTransport kernel(resolved.Layers(),550,1,0,AbsorptionEstimator::Analog,0,
            ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
        // Matched indices, vacuum layers and diffuse bottom: return energy exactly one.
        for(std::uint64_t seed=0;seed<20;++seed) {
            ForwardExperimentRandom random(seed);auto state=kernel.Begin({0,0,0},{0,0,1});ForwardSurfaceWork work;
            Check(DriveForwardExperimentHistory(kernel,nullptr,state,random,1000,work)==ForwardTermination::None,"smooth diagnostic differs");
            Check(state.Termination()==ForwardTermination::SubsurfaceReturn&&state.Weight()==1,"vacuum return identity");
            Check(work.workUnits==0,"legacy smooth charged surface work");
        }
        HyLIoSSurfaceBoundary law(.75);
        ForwardExperimentRandom budgetRng(7);auto budget=kernel.Begin({0,0,0},{0,0,1});ForwardSurfaceWork budgetWork;
        DriveForwardExperimentHistory(kernel,&law,budget,budgetRng,1,budgetWork);
        Check(budget.Termination()==ForwardTermination::BudgetExceeded&&budgetWork.workUnits==1&&budgetWork.branchAttempts==1&&budgetRng.draws==1,"fold first transition budget differs");
        ForwardExperimentRandom cancelRng(7);auto cancelled=kernel.Begin({0,0,0},{0,0,1});ForwardSurfaceWork cancelWork;
        const ForwardExperimentHooks hooks{nullptr,[](void*) {return true;},nullptr};
        DriveForwardExperimentHistory(kernel,&law,cancelled,cancelRng,1000,cancelWork,hooks);
        Check(cancelled.Termination()==ForwardTermination::Cancelled&&cancelRng.draws==0&&cancelWork.workUnits==0,"cancel drew samples");
        unsigned pulseCalls=0;const ForwardExperimentHooks pulseHooks{&pulseCalls,
            [](void* pointer) {return ++*static_cast<unsigned*>(pointer)==1;},nullptr};
        auto pulse=kernel.Begin({0,0,0},{0,0,1});ForwardSurfaceWork pulseWork;
        DriveForwardExperimentHistory(kernel,nullptr,pulse,cancelRng,1000,pulseWork,pulseHooks);
        Check(pulse.Termination()==ForwardTermination::Cancelled&&pulseCalls==1,"observed cancellation was not latched");
        auto glassLayers=resolved.Layers();for(auto& layer:glassLayers) layer.realIOR=1.5;
        HyLIoSForwardTransport glass(glassLayers,550,1,0,AbsorptionEstimator::Analog,0,
            ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
        ConstantSampler reflective(0);auto reflected=glass.Begin({0,0,0},{0,0,1});ForwardSurfaceWork reflectedWork;
        CancelContext context;
        const ForwardExperimentHooks stopOnProgress{&context,
            [](void* pointer) {return static_cast<CancelContext*>(pointer)->cancel;},
            [](void* pointer) {auto& status=*static_cast<CancelContext*>(pointer);status.cancel=true;++status.notifications;}};
        DriveForwardExperimentHistory(glass,&law,reflected,reflective,1000,reflectedWork,stopOnProgress);
        Check(reflected.Termination()==ForwardTermination::Cancelled&&context.notifications==1&&reflective.draws==1&&
            reflectedWork.workUnits==1&&reflectedWork.cancellations==1,"fold progress cancellation ordering differs");
        ConstantSampler invalidSample(std::numeric_limits<double>::quiet_NaN());
        auto invalidState=glass.Begin({0,0,0},{0,0,1});ForwardSurfaceWork invalidWork;
        Check(DriveForwardExperimentHistory(glass,&law,invalidState,invalidSample,1000,invalidWork)==ForwardTermination::InvalidSample,
            "invalid boundary draw diagnostic differs");
        Check(invalidState.Termination()==ForwardTermination::Cancelled&&invalidWork.workUnits==1&&invalidWork.invalidSamples==1&&
            invalidWork.cancellations==1&&invalidSample.draws==1,"invalid boundary draw accounting differs");
        ConstantSampler smoothInvalid(std::numeric_limits<double>::quiet_NaN());
        auto smoothPending=glass.Begin({0,0,0},{0,0,1});ForwardSurfaceWork smoothWork;
        const auto smoothToken=smoothPending.PendingBoundary().token;
        bool smoothThrew=false;
        try {DriveForwardExperimentHistory(glass,nullptr,smoothPending,smoothInvalid,64,smoothWork);}
        catch(const std::exception& error) {
            smoothThrew=std::string(error.what()).find("dielectric variate must be finite")!=std::string::npos;
        }
        Check(smoothThrew&&smoothInvalid.draws==1&&smoothPending.Termination()==ForwardTermination::None&&
            smoothPending.HasPendingBoundary()&&smoothPending.PendingBoundary().token==smoothToken&&
            smoothPending.Counters().workUnits==0&&smoothPending.Weight()==1&&smoothWork.workUnits==0&&
            smoothWork.invalidSamples==0&&smoothWork.cancellations==0,"smooth exception/state/draw contract differs");
        for(bool fold:{false,true}) {
            FixedSampler fixed;auto fixedState=glass.Begin({0,0,0},{0,0,1});ForwardSurfaceWork fixedWork;bool refused=false;
            try {DriveForwardExperimentHistory(glass,fold ? &law:nullptr,fixedState,fixed,64,fixedWork);}
            catch(const std::invalid_argument& error) {refused=std::string(error.what()).find("variable sampler budget")!=std::string::npos;}
            Check(refused&&fixed.draws==0&&fixedWork.workUnits==0&&fixedState.Counters().workUnits==0,"fixed-budget sampler consumed work/draws");
            ConstantSampler wrongOwner(0);auto foreign=kernel.Begin({0,0,0},{0,0,1});ForwardSurfaceWork ownerWork;refused=false;
            try {DriveForwardExperimentHistory(glass,fold ? &law:nullptr,foreign,wrongOwner,64,ownerWork);}
            catch(const std::invalid_argument& error) {refused=std::string(error.what()).find("another kernel")!=std::string::npos;}
            Check(refused&&wrongOwner.draws==0&&ownerWork.workUnits==0&&foreign.Counters().workUnits==0,"foreign state consumed work/draws");
        }
        std::cout<<"HyLIoSResolvedExperimentTest: PASS\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
