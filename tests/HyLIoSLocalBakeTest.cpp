#include "../src/Library/Materials/HyLIoS/HyLIoSLocalBake.h"
#include "../src/Library/Utilities/RISECBOR64.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cfenv>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <thread>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <cstdlib>
#include <new>
#ifdef HYLIOS_ALLOCATION_GATE
namespace AllocationGate {bool active=false;std::size_t maximum=0,total=0;}
void* operator new(std::size_t n) {
    if(AllocationGate::active){AllocationGate::maximum=std::max(AllocationGate::maximum,n);AllocationGate::total+=n;}
    if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
#endif
using namespace RISE::HyLIoS;
using J=RISE::Agent::JsonValue;using V=RISE::RISECBOR64::Value;
namespace {
unsigned passed=0;
void Check(bool b,const std::string& s){if(!b)throw std::runtime_error("FAIL "+s);++passed;std::cout<<"ok: "<<s<<'\n';}
template<class F>void Reject(F f,const std::string& reason,const std::string& marker){try{f();}catch(const std::exception& e){Check(std::string(e.what()).find(reason)!=std::string::npos,marker+" ["+e.what()+"]");return;}Check(false,marker+" accepted");}
#ifdef HYLIOS_ALLOCATION_GATE
template<class F>void AllocationRefuse(F action,const std::string& marker) {
    bool refused=false;AllocationGate::maximum=0;AllocationGate::total=0;AllocationGate::active=true;
    try{action();}catch(const std::exception&){refused=true;}
    AllocationGate::active=false;
    const auto maximum=AllocationGate::maximum,total=AllocationGate::total;
    Check(refused&&maximum<4096&&total<65536,"METADATA_ALLOCATION "+marker+" max="+std::to_string(maximum)+" total="+std::to_string(total));
}
#endif
J Parse(const std::string& bytes){J j;std::string e;Check(RISE::Agent::JsonParse(bytes,j,e),"fixture syntax");return j;}
void Set(J& j,const std::string& key,J value){J replacement=J::MakeObject();for(const auto& p:j.members())if(p.first!=key)replacement.set(p.first,p.second);replacement.set(key,value);j=replacement;}
J Fixture(double ior=1,double absorption=0){
    J result=Parse(R"({"schema_version":1,"approximate":true,"composition_revision":"synthetic-v1","coverage_profile_id":"explicit-synthetic-approximation","spectral_bundle_sha256":"untrusted_bundle_label","resolved_specimen_sha256":"untrusted_specimen_label","preset_id":"synthetic","interpretation":"synthetic controls","units":{"length":"m","coefficient":"m^-1","wavelength":"nm","mass_concentration":"kg/m^3"},"wavelength_nm":[550,1400],"surface_fold_aspect_ratio":0.75,"provenance":{"retained":"yes"}})");
    const char* names[]={"stratum_corneum","stratum_granulosum","stratum_spinosum","stratum_basale","papillary_dermis","reticular_dermis"};J layers=J::MakeArray();
    for(const char* name:names){J l=J::MakeObject();l.set("layer_id",J::MakeString(name));l.set("thickness_m",J::MakeNumber(.001));
        J a=J::MakeArray(),eta=J::MakeArray();for(int i=0;i<2;++i){a.push_back(J::MakeNumber(absorption));eta.push_back(J::MakeNumber(ior));}l.set("host_absorption_m_inv",a);l.set("real_refractive_index",eta);
        J constituents=J::MakeObject();constituents.set("synthetic",a);l.set("constituent_absorption_m_inv",constituents);l.set("particles",J::MakeNull());layers.push_back(l);}
    result.set("layers",layers);return result;
}
std::string Bytes(const J& j){return RISE::Agent::JsonSerialize(j)+"\n";}
V Decode(const LocalAssetRecord& b){V v;std::string e;Check(RISE::RISECBOR64::DecodeCanonical(b,v,&e),"canonical report");return v;}
const V& Field(const V& v,const char* key){const auto* p=v.Find(key);Check(p!=nullptr,std::string("report field ")+key);return *p;}
#ifndef _WIN32
template<class F>void BoundedRegularRefusal(F action,const std::string& marker) {
    std::cout.flush();std::cerr.flush();const pid_t child=fork();Check(child>=0,"create bounded FIFO probe child");
    if(child==0){try{action();_exit(2);}catch(const std::exception& e){_exit(std::string(e.what()).find("regular")!=std::string::npos?0:3);}}
    int status=0;bool finished=false;for(unsigned n=0;n<200;++n){const auto result=waitpid(child,&status,WNOHANG);if(result==child){finished=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(!finished){kill(child,SIGKILL);waitpid(child,&status,0);}Check(finished&&WIFEXITED(status)&&WEXITSTATUS(status)==0,marker);
}
#endif
LocalAssetRecord Encode(const V& value){LocalAssetRecord bytes;std::string error;Check(RISE::RISECBOR64::Encode(value,bytes,&error),"encode canonical mutation fixture");return bytes;}
V Replace(const V& map,const std::string& key,V value){V::Members members;bool found=false;for(const auto& member:map.GetMap()){if(member.first==key){members.emplace_back(key,value);found=true;}else members.push_back(member);}Check(found,"mutation fixture field exists");return V::MapValue(members);}
void Write(const std::string& path,const LocalAssetRecord& bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());Check(out.good(),"write isolated corruption fixture");}

std::string ReportPath(const std::string& asset,const LocalBakeResult& r){return asset+".statistics."+RISE::RISECBOR64::SHA256Hex(r.report)+".cbor";}
std::string SourcePath(const std::string& asset,const LocalBakeResult& r){return asset+".source."+RISE::RISECBOR64::SHA256Hex(r.sourceArtifact)+".json";}
J SpectralFixture(unsigned count,bool distinct=false) {
    auto source=Fixture();J wavelengths=J::MakeArray(),layers=J::MakeArray();
    for(unsigned n=0;n<count;++n)wavelengths.push_back(J::MakeNumber(380+400.0*n/(count-1)));
    Set(source,"wavelength_nm",wavelengths);
    for(std::size_t layer=0;layer<6;++layer) {
        auto l=source.get("layers").at(layer);J absorption=J::MakeArray(),ior=J::MakeArray();
        for(unsigned n=0;n<count;++n){absorption.push_back(J::MakeNumber(distinct?(layer+1)*.001+n*.00001:0));ior.push_back(J::MakeNumber(1));}
        Set(l,"host_absorption_m_inv",absorption);Set(l,"real_refractive_index",ior);
        J constituents=J::MakeObject();constituents.set("synthetic",absorption);Set(l,"constituent_absorption_m_inv",constituents);layers.push_back(l);
    }
    Set(source,"layers",layers);return source;
}
LocalBakeConfig SpectralConfig(LocalBakeConfig c,const J& source) {
    c.wavelengthsNm.clear();for(std::size_t n=0;n<source.get("wavelength_nm").size();++n)c.wavelengthsNm.push_back(source.get("wavelength_nm").at(n).asNumber());
    c.historiesPerCellPerReplicate=1;c.normalGuideHistoriesPerReplicate=1;return c;
}
double ColumnValue(const V& dictionary,unsigned layer,unsigned node) {
    const auto index=Field(dictionary,"layer_column").GetArray()[layer].GetIntegerArgument();
    const auto& series=Field(dictionary,"columns").GetArray()[index];if(series.GetType()==V::Float64)return series.GetFloat();
    Check(series.GetType()==V::ByteString&&series.GetBytes().size()>=8*(node+1),"packed semantic series extent");
    std::uint64_t bits=0;for(unsigned k=0;k<8;++k)bits|=std::uint64_t{series.GetBytes()[node*8+k]}<<(8*k);
    double value=0;std::memcpy(&value,&bits,8);return value;
}

}
int main(int argc,char** argv){try{
    LocalBakeConfig c;c.wavelengthsNm={550};c.historiesPerCellPerReplicate=64;c.normalGuideHistoriesPerReplicate=96;c.fiberFraction=0;c.seed=std::numeric_limits<std::uint64_t>::max();c.buildArtifactPath=argc>1?argv[1]:argv[0];
    const auto vacuum=Bytes(Fixture());const auto baked=BakeLocalResponse(vacuum,c);
    const auto source32=SpectralFixture(32);const auto c32=SpectralConfig(c,source32);const auto baked32=BakeLocalResponse(Bytes(source32),c32);
    Check(baked32.asset.response.wavelengthNm.size()==32&&baked32.completedHistories==320,"full 32-node synthetic bake has independent normal/angular histories");
    for(unsigned count:{8U,16U,32U,64U}) {
        const auto input=SpectralFixture(count,true);const auto semantic=PrepareLocalBakeSemantic(Bytes(input),SpectralConfig(c,input));
        Check(semantic.data.size()<=4096&&semantic.specimen.size()<=4096&&semantic.model.size()<=4096,"dense spectral columns fit actual canonical record caps through 64 nodes");
        const auto data=Decode(semantic.data);const double actual=ColumnValue(Field(data,"host_absorption_m_inv"),3,count/2);
        const double expected=input.get("layers").at(3).get("host_absorption_m_inv").at(count/2).asNumber();Check(actual==expected,"packed semantic columns retain exact selected binary64 coefficients");
    }
    const auto tooDense=SpectralFixture(128,true);
    Reject([&]{PrepareLocalBakeSemantic(Bytes(tooDense),SpectralConfig(c,tooDense));},"4096-byte record","actual canonical byte capacity refuses oversized dense grid");
    auto sourceLimit=c;sourceLimit.maximumSourceArtifactBytes=1;
    Reject([&]{BakeLocalResponse(vacuum,sourceLimit);},"source artifact exceeds","source artifact byte preflight before histories");
    auto assetLimit=c;assetLimit.limits.maximumFileBytes=1;
    Reject([&]{BakeLocalResponse(vacuum,assetLimit);},"file budget exceeded before generation","asset payload byte preflight before histories");
    assetLimit=c;assetLimit.limits.maximumResidentBytes=128*1024*1024;
    Reject([&]{BakeLocalResponse(vacuum,assetLimit);},"codec resident budget exceeded before generation","codec peak admission before histories");
    auto reportLimit=c;reportLimit.maximumReportBytes=1;
    Reject([&]{BakeLocalResponse(vacuum,reportLimit);},"report budget exceeded","report byte preflight before histories");

    auto independentRequest=c;independentRequest.seed=12;independentRequest.historiesPerCellPerReplicate=0;independentRequest.replicates=0;
    independentRequest.maximumWorkPerHistory=0;independentRequest.maximumTotalWork=0;independentRequest.maximumTotalHistories=0;
    independentRequest.incident={};independentRequest.outgoing={};
    const auto independentlyPrepared=PrepareLocalBakeSemantic(vacuum,independentRequest);
    Check(independentlyPrepared.model==baked.asset.semantic.model&&independentlyPrepared.data==baked.asset.semantic.data&&independentlyPrepared.specimen==baked.asset.semantic.specimen,
        "semantic preparation is independent of unknown producer grid seed history and work settings");
    Check(baked.completedHistories==2*(4*64+96),"all-history denominator and separate normal cardinality");
    auto core=HyLIoSLocalResponse::Create(baked.asset.response);for(std::size_t i=0;i<4;++i){const auto pi=i%2,ti=i/2;LocalResponsePoint wi{.25+.5*ti,LocalResponsePi*(.5+pi)};
        Check(core->ForwardRowMassNM(wi,550)<=1&&core->ForwardRowMassNM(wi,550)>.99999999999999,"vacuum diffuse exact finite passivity all N");Check(baked.asset.response.mirror[i]==0,"diffuse bottom never assigned mirror");}
    Check(baked.asset.normalIncidenceGuide[0]>.99999999999999,"separate exact-normal unit diffuse guide");
    const auto report=Decode(baked.report);const auto& rows=Field(report,"rows").GetArray();Check(rows.size()==5,"report has every angular row plus exact normal");
    Check(Field(rows.back(),"normal_exact").GetBoolean(),"last statistics row is declared exact normal");
    for(const auto& row:rows){for(const auto& rep:Field(row,"replicate_bin_counts").GetArray()){std::uint64_t total=0;for(const auto& x:rep.GetArray())total+=x.GetIntegerArgument();Check(total==Field(row,"histories_per_replicate").GetIntegerArgument(),"exact replicate all-N count conservation");}}
    LocalBakeExecution partition;partition.historyChunkSize=7;partition.reverseTraversal=true;const auto reversed=BakeLocalResponse(vacuum,c,partition);
    Check(baked.asset.response.continuous==reversed.asset.response.continuous&&baked.asset.response.mirror==reversed.asset.response.mirror&&baked.report==reversed.report,"indexed RNG partition order byte reproducibility and UINT64_MAX seed wrap");
    // Analytic synthetic measures: one mirror atom, a finite diffuse row, and
    // absorber zero. These exercise the same count/area construction as bake.
    for(std::uint64_t n:{1ULL,3ULL,7ULL,17ULL,9007199254740991ULL,9007199254740992ULL}){
        LocalResponseDefinition d;d.wavelengthNm={550};d.incident={{0,1},{0,LocalResponseTwoPi}};d.outgoing={{0,.1,.8,1},{0,LocalResponseTwoPi}};d.continuous.resize(3);d.mirror={LocalBakeDetail::DownwardCountRatio(n,n)};
        auto mirror=HyLIoSLocalResponse::Create(d);Check(mirror->ForwardRowMassNM({.5,.7},550)<=1&&mirror->CameraMirrorMassNM({.5,.7},550)>0,"synthetic mirror near-tie downward ratio passivity");
        d.mirror[0]=0;auto blank=HyLIoSLocalResponse::Create(d);
        const std::uint64_t a=n/3,b=n/3,cc=n-a-b;const std::uint64_t counts[]={a,b,cc};for(unsigned j=0;j<3;++j)d.continuous[j]=LocalBakeDetail::DownwardCoefficient(counts[j],n,blank->OutgoingCellArea(j));
        auto diffuse=HyLIoSLocalResponse::Create(d);Check(diffuse->ForwardRowMassNM({.5,.7},550)<=1&&diffuse->ForwardRowMassNM({.5,.7},550)>.99999999999999,"synthetic diffuse unequal-area near-tie exact represented-area passivity");
        std::fill(d.continuous.begin(),d.continuous.end(),0);Check(HyLIoSLocalResponse::Create(d)->ForwardRowMassNM({.5,.7},550)==0,"synthetic absorber exactly zero");
    }
    // At the actual oracle bridge: oblique, nonaxis azimuth exposes Y signs.
    const LocalResponsePoint wi{.37,.83};const auto w=HyLIoSLocalResponse::ToDirection(wi);const auto launch=LocalBakeDetail::Launch(wi);
    Check(launch.x==-w.x&&launch.y==w.y&&launch.z==w.z,"right-handed actual launch frame");
    const auto resolved=ResolveExperiment(vacuum,{550});HyLIoSForwardTransport kernel(resolved.Layers(),550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto state=kernel.Begin({0,0,0},launch);ForwardExperimentRandom rng(1);const auto encounter=kernel.Advance(state,rng,0);Check(encounter.kind==ForwardAdvanceKind::OuterBoundaryEncounter,"bridge starts at exterior boundary");
    const auto event=state.PendingBoundary();auto decision=SampleSlabSmoothDielectric(event,0);decision.action=BoundaryAction::Reflect;decision.targetSide=SlabSide::Exterior;decision.direction={launch.x,launch.y,-launch.z};decision.angularLaw=SlabBoundaryAngularLaw::PlanarDielectric;kernel.ResumeBoundary(state,event.token,decision);
    const auto escaped=LocalBakeDetail::Escaped(state.Direction());const auto mirror=HyLIoSLocalResponse::PlanarMirror(wi);Check(std::abs(escaped.t-mirror.t)<1e-15&&std::abs(escaped.phi-mirror.phi)<1e-15,"right-handed actual escaped oblique mirror map");
    Check(state.ReturnedAtomMap()==ForwardReturnedAtomMap::PlanarSameExteriorMirror,"planar returned lineage distinct from direction bin");
    // Matched exterior and huge absorption has zero returned measure for every
    // representable free-path draw; it is an exact black-control topology.
    const auto black=BakeLocalResponse(Bytes(Fixture(1,1e12)),c);Check(std::all_of(black.asset.response.continuous.begin(),black.asset.response.continuous.end(),[](double x){return x==0;})&&std::all_of(black.asset.response.mirror.begin(),black.asset.response.mirror.end(),[](double x){return x==0;})&&black.asset.normalIncidenceGuide[0]==0,"physical black absorber no escape normalization");
    LocalBakeConfig oblique=c;oblique.historiesPerCellPerReplicate=512;oblique.normalGuideHistoriesPerReplicate=512;
    const auto coatedBlack=BakeLocalResponse(Bytes(Fixture(1.5,1e12)),oblique);Check(coatedBlack.asset.response.mirror[0]>coatedBlack.asset.normalIncidenceGuide[0]+.04,"normal guide independently launched, distinct from incident-cell average");
    auto complex=Fixture();J particles=Parse(R"({"enabled":true,"organelle_fraction":0.01,"organization":"complexes","interior_absorption_evaluated":true,"interior_absorption_m_inv":[0,0],"semi_minor_m":1e-7,"semi_major_m":1e-7,"real_refractive_index":1.5,"complex_diameter_m":2e-6,"maximum_internal_count":2})");J complexLayers=J::MakeArray();for(std::size_t i=0;i<6;++i){J layer=complex.get("layers").at(i);if(i==3)Set(layer,"particles",particles);complexLayers.push_back(layer);}Set(complex,"layers",complexLayers);auto complexConfig=c;complexConfig.explicitComplexPackV1=true;complexConfig.wavelengthsNm={1400};
    Reject([&]{BakeLocalResponse(Bytes(complex),complexConfig);},"angular closure unproved","static unknown refusal before any observed outcomes");
    auto visibleSource=complex;J grid=J::MakeArray();const double below=std::nextafter(1400.0,0.0);grid.push_back(J::MakeNumber(below));grid.push_back(J::MakeNumber(1400));Set(visibleSource,"wavelength_nm",grid);
    for(auto policy:{ForwardPostScatterPolicy::DiscardAfterScatterV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1}){
        auto cc=complexConfig;cc.wavelengthsNm={below};cc.historiesPerCellPerReplicate=8;cc.normalGuideHistoriesPerReplicate=8;cc.postScatter=policy;
        Check(BakeLocalResponse(Bytes(visibleSource),cc).completedHistories==80,"Ngt1 just-below-1400 positive final-kick static closure both policies");
        cc.wavelengthsNm={1400};Reject([&]{BakeLocalResponse(Bytes(visibleSource),cc);},"angular closure unproved","Ngt1 exact-1400 zero-kick static refusal both policies");
    }
    // N0/N1 remain enabled by the static certificate in both postscatter modes.
    for(unsigned childCount=0;childCount<=1;++childCount){
        auto supported=complex;J enabled=particles;Set(enabled,"maximum_internal_count",J::MakeNumber(childCount));Set(enabled,"real_refractive_index",J::MakeNumber(1));Set(enabled,"organelle_fraction",J::MakeNumber(.00001));J layers=J::MakeArray();
        for(std::size_t i=0;i<6;++i){J layer=supported.get("layers").at(i);if(i==3)Set(layer,"particles",enabled);layers.push_back(layer);}Set(supported,"layers",layers);
        auto cc=complexConfig;cc.historiesPerCellPerReplicate=8;cc.normalGuideHistoriesPerReplicate=8;cc.postScatter=ForwardPostScatterPolicy::DiagnosticRetainParticleV1;
        Check(BakeLocalResponse(Bytes(supported),cc).completedHistories==80,"proved N0/N1 retained configurations remain available");
    }
    auto folded=c;folded.boundary=ExperimentBoundary::HyLIoS2015;const auto foldBake=BakeLocalResponse(vacuum,folded);
    Check(foldBake.surfaceWork>=2*foldBake.completedHistories&&foldBake.bulkWork>0,"actual shared fold proposals charged in production work statistics");
    auto cap=c;cap.maximumWorkPerHistory=1;Reject([&]{BakeLocalResponse(vacuum,cap);},"unresolved","work limit refuses production");
    LocalBakeExecution cancel;cancel.hooks.isCancelled=[](void*){return true;};Reject([&]{BakeLocalResponse(vacuum,c,cancel);},"cancelled","cancellation refuses production");
    cap=c;cap.historiesPerCellPerReplicate=std::numeric_limits<std::uint64_t>::max();Reject([&]{BakeLocalResponse(vacuum,cap);},"overflow","uint64 cardinality preflight overflow");
    cap=c;cap.maximumTotalHistories=1;Reject([&]{BakeLocalResponse(vacuum,cap);},"before generation","explicit total work cardinality refusal");
    auto directory=std::filesystem::temp_directory_path()/("hylios-local-baker-gate-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    while(!std::filesystem::create_directory(directory))directory+="x";
    const auto path=(directory/"response.asset").string();PublishLocalBake(path,baked);const auto asset=LoadLocalBake(path,PrepareLocalBakeSemantic(vacuum,c));
    const auto unicodePath=(directory/std::filesystem::u8path("response-\xcf\x89.asset")).u8string();PublishLocalBake(unicodePath,baked);
    Check(LoadLocalBake(unicodePath,baked.asset.semantic)->ContentSHA256()==asset->ContentSHA256(),"UTF8 asset source and report pathname parity");
    for(const auto& compiler:{std::string("\xc2\x85"),std::string("\xe2\x80\xa8"),std::string("\xe2\x80\x8b")}) {
        auto unicode=baked;unicode.asset.production.build=Encode(Replace(Decode(unicode.asset.production.build),"compiler",V::String(compiler)));
        const auto unicodePath=(directory/("compiler-unicode-"+std::to_string(compiler.back()&255)+".asset")).string();
        PublishLocalBake(unicodePath,unicode);const auto accepted=LoadLocalBake(unicodePath,baked.asset.semantic);
        Check(accepted->Production().build==unicode.asset.production.build,"BUILD_COMPILER_UNICODE NFC non-ASCII text is accepted without printability claim");
    }
#ifdef HYLIOS_ALLOCATION_GATE
    // Build adversarial encoded inputs before tracking. Each public entry must
    // refuse with no generic Value allocations, independently of field schema.
    std::vector<LocalAssetRecord> records;
    records.push_back(Encode(V::MapValue({{"oversize",V::String(std::string(2*1024*1024,'x'))}})));
    V nested=V::Unsigned(0);for(unsigned n=0;n<12;++n)nested=V::ArrayValue({nested});
    records.push_back(Encode(V::MapValue({{"deep",nested}})));
    records.push_back({0xa1,0x61,0x78,0x99,0x20,0x01}); // array cardinality 8193
    V::Values denseTokens(2000,V::Unsigned(0));records.push_back(Encode(V::MapValue({{"many",V::ArrayValue(denseTokens)}})));
    LocalAssetLimits tiny;tiny.maximumResidentBytes=65536;
    for(std::size_t kind=0;kind<records.size();++kind) {
        const LocalAssetLimits admissionLimits=kind<3?LocalAssetLimits{}:tiny;
        for(unsigned slot=0;slot<11;++slot) {
            auto malformedMetadata=baked;auto& semantic=malformedMetadata.asset.semantic;auto& production=malformedMetadata.asset.production;
            LocalAssetRecord* fields[]={&semantic.model,&semantic.placement,&semantic.postscatter,&semantic.boundary,&semantic.lineage,
                &semantic.data,&semantic.specimen,&semantic.exterior,&semantic.approximation,&production.build,&production.statistics};
            *fields[slot]=records[kind];
            AllocationRefuse([&]{PublishLocalBake(path,malformedMetadata,admissionLimits);},"publish kind="+std::to_string(kind)+" slot="+std::to_string(slot));
            if(slot<9)AllocationRefuse([&]{LoadLocalBake(path,semantic,admissionLimits);},"load expected kind="+std::to_string(kind)+" slot="+std::to_string(slot));
        }
        if(kind<3)AllocationRefuse([&]{LocalAssetDetail::ValidateRecordSyntax(records[kind]);},"shared record syntax kind="+std::to_string(kind));
    }
    AllocationRefuse([&]{PublishLocalBake(path,baked,tiny);},"valid metadata low resident publication");
    AllocationRefuse([&]{LoadLocalBake(path,baked.asset.semantic,tiny);},"valid metadata low resident load");
#endif
    Reject([&]{PublishLocalBake(path+std::string("\xc0\xaf"),baked);},"Unicode","invalid UTF8 output pathname refused before publication");
    Reject([&]{LoadLocalBake(path+std::string("\0x",2),baked.asset.semantic);},"pathname contract","embedded NUL input pathname refused");
    Reject([&]{PublishLocalBake(std::string(32769,'x'),baked);},"pathname contract","oversized output pathname refused before construction");
    Check(asset->Production().seed==std::numeric_limits<std::uint64_t>::max()&&asset->NormalIncidenceGuide()[0]<=1,"persist exact U64 seed and bounded normal guide");
    Reject([&]{PublishLocalBake(path,black,{},[]{throw std::runtime_error("test refusal at publication");});},"atomic publication failed","asset publication callback refuses");
    Check(LoadLocalBake(path,baked.asset.semantic)->ContentSHA256()==asset->ContentSHA256(),"failed replacement preserves prior valid asset/report pair");
    std::filesystem::remove(ReportPath(path,black));
    auto forged=baked;auto forgedData=Decode(forged.asset.semantic.data);
    forgedData=Replace(forgedData,"host_absorption_m_inv",V::MapValue({{"columns",V::ArrayValue({V::Float(.125)})},{"layer_column",V::ArrayValue({V::Unsigned(0),V::Unsigned(0),V::Unsigned(0),V::Unsigned(0),V::Unsigned(0),V::Unsigned(0)})}}));
    forged.asset.semantic.data=Encode(forgedData);const auto forgedPath=(directory/"forged-semantics.asset").string();
    Reject([&]{PublishLocalBake(path,forged);},"semantic rederivation mismatch","SOURCE_REDERIVATION publication checks actual bound source coefficients");
    SaveLocalResponseAsset(forgedPath,forged.asset);Write(ReportPath(forgedPath,forged),forged.report);Write(SourcePath(forgedPath,forged),forged.sourceArtifact);
    Reject([&]{LoadLocalBake(forgedPath,forged.asset.semantic);},"semantic rederivation mismatch","SOURCE_REDERIVATION producer fields cannot override actual bound source coefficients");
    const auto sourcePath=SourcePath(path,baked);std::filesystem::rename(sourcePath,sourcePath+".hold");
    Reject([&]{LoadLocalBake(path,baked.asset.semantic);},"bound source artifact","missing full provenance source sidecar refused");
    std::filesystem::rename(sourcePath+".hold",sourcePath);
    auto changedSource=baked;changedSource.sourceArtifact.back()^=1;
    Reject([&]{PublishLocalBake(path,changedSource);},"source content","source artifact mutation cannot publish asset");
    {std::fstream sourceFile(sourcePath,std::ios::in|std::ios::out|std::ios::binary);char byte='!';sourceFile.write(&byte,1);}
    Reject([&]{LoadLocalBake(path,baked.asset.semantic);},"bound source artifact","source sidecar corruption refused at pair load");
    {std::ofstream sourceFile(sourcePath,std::ios::binary|std::ios::trunc);sourceFile.write(reinterpret_cast<const char*>(baked.sourceArtifact.data()),baked.sourceArtifact.size());}
    auto provenanceOnly=Fixture();J provenance=J::MakeObject();provenance.set("retained",J::MakeString("changed full provenance"));Set(provenanceOnly,"provenance",provenance);
    const auto provenanceSemantic=PrepareLocalBakeSemantic(Bytes(provenanceOnly),c);
    Check(provenanceSemantic.data==baked.asset.semantic.data&&provenanceSemantic.specimen!=baked.asset.semantic.specimen,"full provenance bytes bind semantic identity even when kernel coefficients agree");
    Reject([&]{LoadLocalBake(path,provenanceSemantic);},"semantic","independent source provenance hash mutation refused");
    auto numerical=Fixture(1,7);const auto numericalSemantic=PrepareLocalBakeSemantic(Bytes(numerical),c);
    Check(numericalSemantic.data!=baked.asset.semantic.data,"actual selected optical values change semantic coefficient record");
    Reject([&]{LoadLocalBake(path,numericalSemantic);},"semantic","independent selected coefficient mutation refused");
    auto changed=c;changed.exteriorIOR=1.1;Reject([&]{LoadLocalBake(path,PrepareLocalBakeSemantic(vacuum,changed));},"semantic","independently prepared semantic key mismatch");
    auto many=c;many.incident={{0,1},{0,LocalResponseTwoPi}};many.outgoing=many.incident;many.replicates=1024;many.historiesPerCellPerReplicate=1;many.normalGuideHistoriesPerReplicate=1;
    const auto manyResult=BakeLocalResponse(vacuum,many);const auto manyPath=(directory/"many-replicates.asset").string();PublishLocalBake(manyPath,manyResult);
    const auto manyAsset=LoadLocalBake(manyPath,manyResult.asset.semantic);
    struct FiveStats {double mean,history,replicate,lower,upper;};
    const std::uint64_t expectedScratch=65536+(manyResult.asset.production.statistics.size()+manyResult.asset.production.build.size())*4096+
        sizeof(std::vector<FiveStats>)+4*sizeof(std::vector<std::uint64_t>)+sizeof(std::vector<std::vector<std::uint64_t>>)+
        many.replicates*sizeof(std::vector<std::uint64_t>)+many.replicates*3*8+3*sizeof(FiveStats)+many.replicates*32;
    LocalAssetLimits tight;tight.maximumResidentBytes=manyAsset->PeakResidentBytesBound()+manyResult.report.size()+expectedScratch-1;
    Reject([&]{LoadLocalBake(manyPath,manyResult.asset.semantic,tight);},"report resident budget","REPORT_CONTAINER_BUDGET nested count vector headers are charged before allocation");
    tight.maximumResidentBytes+=1;Check(LoadLocalBake(manyPath,manyResult.asset.semantic,tight)->Production().replicates==1024,"REPORT_CONTAINER_BUDGET exact logical scratch boundary accepted");
    const int originalRounding=std::fegetround();Check(std::fesetround(FE_UPWARD)==0,"set alternate rounding for profile refusal");
    bool refusedRounding=false;try{LoadLocalBake(path,baked.asset.semantic);}catch(const std::exception& error){refusedRounding=std::string(error.what()).find("round-to-nearest")!=std::string::npos;}
    std::fesetround(originalRounding);Check(refusedRounding,"strict statistics profile refuses changed runtime rounding");
    auto RehashReport=[](LocalBakeResult& result){auto stat=Replace(Decode(result.asset.production.statistics),"full_report_sha256",V::String(RISE::RISECBOR64::SHA256Hex(result.report)));stat=Replace(stat,"full_report_bytes",V::Unsigned(result.report.size()));result.asset.production.statistics=Encode(stat);};
    unsigned malformedIndex=0;
    auto RefuseBundle=[&](LocalBakeResult result,const std::string& reason,const std::string& marker){
        RehashReport(result);Reject([&]{PublishLocalBake(path,result);},reason,marker+" publication");
        const auto isolated=(directory/("malformed-"+std::to_string(malformedIndex++)+".asset")).string();
        SaveLocalResponseAsset(isolated,result.asset);Write(ReportPath(isolated,result),result.report);Write(SourcePath(isolated,result),result.sourceArtifact);
        Reject([&]{LoadLocalBake(isolated,result.asset.semantic);},reason,marker+" load");
    };
    auto badBuild=baked;badBuild.asset.production.build=Encode(V::MapValue({{"adapter_version",V::String(LocalBakeVersion)}}));
    RefuseBundle(badBuild,"BUILD_RECORD_SCHEMA","BUILD_RECORD_SCHEMA adapter-only producer record refused");
    const auto completeBuild=Decode(baked.asset.production.build);
    for(const char* removed:{"adapter_version","compiler","selected_artifact_sha256","adapter_strict_fp"}) {
        V::Members members;for(const auto& member:completeBuild.GetMap())if(member.first!=removed)members.push_back(member);
        badBuild=baked;badBuild.asset.production.build=Encode(V::MapValue(members));
        RefuseBundle(badBuild,"BUILD_RECORD_SCHEMA",std::string("BUILD_RECORD_SCHEMA missing ")+removed);
    }
    badBuild=baked;auto extraBuild=completeBuild.GetMap();extraBuild.emplace_back("unknown_build_field",V::Bool(true));badBuild.asset.production.build=Encode(V::MapValue(extraBuild));
    RefuseBundle(badBuild,"BUILD_RECORD_SCHEMA","BUILD_RECORD_SCHEMA unknown field refused");
    for(const auto& bad:std::vector<V>{V::Unsigned(17),V::String(""),V::String("obsolete-adapter")}) {
        badBuild=baked;badBuild.asset.production.build=Encode(Replace(completeBuild,"adapter_version",bad));
        RefuseBundle(badBuild,"unsupported bake adapter version","BUILD_RECORD_VERSION unsupported or mistyped adapter version refused");
    }
    for(const auto& bad:std::vector<V>{V::Unsigned(17),V::String(""),V::String(std::string(257,'c')),V::String("   "),V::String(std::string(1,'\n'))}) {
        badBuild=baked;badBuild.asset.production.build=Encode(Replace(completeBuild,"compiler",bad));
        RefuseBundle(badBuild,"BUILD_RECORD_COMPILER","BUILD_RECORD_COMPILER malformed compiler claim refused");
    }
    for(const auto& bad:std::vector<V>{V::Unsigned(17),V::String("not-a-digest"),V::String(std::string(63,'a')),V::String(std::string(64,'A'))}) {
        badBuild=baked;badBuild.asset.production.build=Encode(Replace(completeBuild,"selected_artifact_sha256",bad));
        RefuseBundle(badBuild,"BUILD_RECORD_DIGEST","BUILD_RECORD_DIGEST malformed selected-artifact digest refused");
    }
    for(const auto& bad:std::vector<V>{V::Bool(false),V::Unsigned(1),V::String("true")}) {
        badBuild=baked;badBuild.asset.production.build=Encode(Replace(completeBuild,"adapter_strict_fp",bad));
        RefuseBundle(badBuild,"BUILD_RECORD_STRICT","BUILD_RECORD_STRICT non-true strict-FP claim refused");
    }
    Check(LoadLocalBake(path,baked.asset.semantic)->ContentSHA256()==asset->ContentSHA256(),"malformed build claims preserve prior valid publication");
    auto Rows=[](const LocalBakeResult& result){return Field(Decode(result.report),"rows").GetArray();};
    auto SetRows=[](LocalBakeResult& result,const V::Values& rows){result.report=Encode(Replace(Decode(result.report),"rows",V::ArrayValue(rows)));};
    auto malformed=baked;auto malformedRows=Rows(malformed);for(auto& row:malformedRows)row=V::Unsigned(0);SetRows(malformed,malformedRows);
    RefuseBundle(malformed,"map shape","REPORT_ROW_SCHEMA integer placeholders cannot replace rows");
    malformed=baked;std::fill(malformed.asset.response.continuous.begin(),malformed.asset.response.continuous.end(),0);std::fill(malformed.asset.response.mirror.begin(),malformed.asset.response.mirror.end(),0);std::fill(malformed.asset.normalIncidenceGuide.begin(),malformed.asset.normalIncidenceGuide.end(),0);
    RefuseBundle(malformed,"response/guide","REPORT_RESPONSE_COUNTS genuine unit-return report cannot certify zero response");
    malformed=baked;malformedRows=Rows(malformed);std::swap(malformedRows[0],malformedRows[1]);SetRows(malformed,malformedRows);
    RefuseBundle(malformed,"ordering/index","REPORT_ROW_ORDER reordered rows refused");
    malformed=baked;malformedRows=Rows(malformed);malformedRows[0]=Replace(malformedRows[0],"normal_exact",V::Bool(true));SetRows(malformed,malformedRows);
    RefuseBundle(malformed,"normal-row flag","REPORT_NORMAL_FLAG angular row cannot be normal guide");
    malformed=baked;malformedRows=Rows(malformed);
    for(auto& row:malformedRows){
        auto reps=Field(row,"replicate_bin_counts").GetArray();for(auto& rep:reps){auto zero=rep.GetArray();for(auto& count:zero)count=V::Unsigned(0);rep=V::ArrayValue(zero);}
        const auto stats=Field(row,"bin_statistics").GetArray();const auto zeroStats=stats[stats.size()-2];V::Values emptyStats(stats.size(),zeroStats);
        row=Replace(row,"replicate_bin_counts",V::ArrayValue(reps));row=Replace(row,"bin_statistics",V::ArrayValue(emptyStats));row=Replace(row,"returned_statistics",zeroStats);
    }
    SetRows(malformed,malformedRows);malformed.report=Encode(Replace(Decode(malformed.report),"maximum_binary64_row_construction_loss_bound",V::Float(0)));
    for(const char* field:{"maximum_bin_or_row_bernoulli_history_se","maximum_bin_or_row_independent_replicate_se","maximum_binary64_row_construction_loss_bound"})malformed.asset.production.statistics=Encode(Replace(Decode(malformed.asset.production.statistics),field,V::Float(0)));
    std::fill(malformed.asset.response.continuous.begin(),malformed.asset.response.continuous.end(),0);std::fill(malformed.asset.response.mirror.begin(),malformed.asset.response.mirror.end(),0);std::fill(malformed.asset.normalIncidenceGuide.begin(),malformed.asset.normalIncidenceGuide.end(),0);
    RefuseBundle(malformed,"sum to all histories","REPORT_COUNT_CONSERVATION zero-response count-free bundle cannot omit all histories");
    malformed=baked;malformedRows=Rows(malformed);auto excessiveReplicates=Field(malformedRows[0],"replicate_bin_counts").GetArray();auto excessiveCounts=excessiveReplicates[0].GetArray();excessiveCounts[0]=V::Unsigned(65);excessiveReplicates[0]=V::ArrayValue(excessiveCounts);malformedRows[0]=Replace(malformedRows[0],"replicate_bin_counts",V::ArrayValue(excessiveReplicates));SetRows(malformed,malformedRows);
    RefuseBundle(malformed,"count exceeds","REPORT_COUNT_RANGE individual count cannot exceed all histories");
    malformed=baked;malformedRows=Rows(malformed);auto binStats=Field(malformedRows[0],"bin_statistics").GetArray();binStats[0]=Replace(binStats[0],"mean",V::Float(.999));malformedRows[0]=Replace(malformedRows[0],"bin_statistics",V::ArrayValue(binStats));SetRows(malformed,malformedRows);
    RefuseBundle(malformed,"derived statistics","REPORT_DERIVED_STATS altered bin statistics refused");
    malformed=baked;malformed.report=Encode(Replace(Decode(malformed.report),"bulk_work",V::Unsigned(malformed.bulkWork+1)));
    RefuseBundle(malformed,"derived work totals","REPORT_WORK_SUM inconsistent aggregate work refused");
    malformed=baked;malformed.asset.production.statistics=Encode(Replace(Decode(malformed.asset.production.statistics),"maximum_bin_or_row_bernoulli_history_se",V::Float(.999)));
    RefuseBundle(malformed,"uncertainty summary","REPORT_STAT_SUMMARY inconsistent compact statistics refused");
    Check(LoadLocalBake(path,baked.asset.semantic)->ContentSHA256()==asset->ContentSHA256(),"malformed bundle failures preserve old valid asset");
#ifndef _WIN32
    const auto fifo=(directory/"input.fifo").string();Check(mkfifo(fifo.c_str(),0600)==0,"create FIFO fixture");
    BoundedRegularRefusal([&]{LocalAssetDetail::ReadRegularFile(fifo,1024);},"REGULAR_FILE_GUARD FIFO snapshot refuses without blocking");
    BoundedRegularRefusal([&]{LocalAssetDetail::HashRegularFile(fifo,1024);},"REGULAR_FILE_GUARD FIFO hash refuses without blocking");
    auto fifoBuild=c;fifoBuild.buildArtifactPath=fifo;BoundedRegularRefusal([&]{BakeLocalResponse(vacuum,fifoBuild);},"REGULAR_FILE_GUARD FIFO build artifact refuses without blocking");
    Reject([&]{LocalAssetDetail::ReadRegularFile(directory.string(),1024);},"regular","REGULAR_FILE_GUARD directory snapshot refused");
    const auto race=(directory/"race.bin").string();const LocalAssetRecord raceBytes{1,2,3,4};Write(race,raceBytes);
    Reject([&]{LocalAssetDetail::ReadRegularFile(race,1024,[&]{Write(race,{1});});},"truncated","REGULAR_FILE_GUARD same-descriptor truncation refused");
    Reject([&]{LocalAssetDetail::ReadRegularFile(race+std::string("\0unused",7),1024);},"pathname","REGULAR_FILE_GUARD embedded NUL input refused");
    Write(race,raceBytes);Reject([&]{LocalAssetDetail::ReadRegularFile(race,1024,[&]{Write(race,{1,2,3,4,5});});},"extra","REGULAR_FILE_GUARD growth after fstat refused");
    Write(race,raceBytes);Reject([&]{LocalAssetDetail::HashRegularFile(race,1024,[&]{Write(race,{1});});},"hash read failed","REGULAR_FILE_GUARD hashing detects same-descriptor truncation");
    Write(race,raceBytes);auto captured=LocalAssetDetail::ReadRegularFile(race,1024,[&]{std::filesystem::rename(race,race+".hold");Check(mkfifo(race.c_str(),0600)==0,"replace path with FIFO after regular open");});
    Check(captured==raceBytes,"REGULAR_FILE_GUARD owned descriptor snapshot survives path replacement");std::filesystem::remove(race);std::filesystem::rename(race+".hold",race);
    const auto sourceFifo=SourcePath(path,baked);std::filesystem::rename(sourceFifo,sourceFifo+".hold");Check(mkfifo(sourceFifo.c_str(),0600)==0,"replace source sidecar by FIFO");
    BoundedRegularRefusal([&]{LoadLocalBake(path,baked.asset.semantic);},"REGULAR_FILE_GUARD FIFO source sidecar refuses without blocking");std::filesystem::remove(sourceFifo);std::filesystem::rename(sourceFifo+".hold",sourceFifo);
    const auto reportFifo=ReportPath(path,baked);std::filesystem::rename(reportFifo,reportFifo+".hold");Check(mkfifo(reportFifo.c_str(),0600)==0,"replace report sidecar by FIFO");
    BoundedRegularRefusal([&]{LoadLocalBake(path,baked.asset.semantic);},"REGULAR_FILE_GUARD FIFO report sidecar refuses without blocking");std::filesystem::remove(reportFifo);std::filesystem::rename(reportFifo+".hold",reportFifo);
#endif
    Check(asset->Response().StoredContinuous(0,0,0)>=0&&asset->Response().StoredMirror(0,0)>=0,"checked nodal accessors expose stored coefficients");
    Reject([&]{asset->Response().StoredContinuous(1,0,0);},"index","checked nodal accessor rejects out of range");
    auto wrongReport=baked;
    wrongReport.report=Encode(Replace(Decode(baked.report),"source_artifact_sha256",V::String(std::string(64,'0'))));
    auto statistics=Replace(Decode(wrongReport.asset.production.statistics),"full_report_sha256",V::String(RISE::RISECBOR64::SHA256Hex(wrongReport.report)));
    statistics=Replace(statistics,"full_report_bytes",V::Unsigned(wrongReport.report.size()));wrongReport.asset.production.statistics=Encode(statistics);
    Reject([&]{PublishLocalBake(path,wrongReport);},"report source hash mismatch","REPORT_SOURCE_BINDING valid digest cannot substitute a different source");
    const auto wrongPath=(directory/"wrong-report.asset").string();SaveLocalResponseAsset(wrongPath,wrongReport.asset);
    Write(ReportPath(wrongPath,wrongReport),wrongReport.report);Write(SourcePath(wrongPath,wrongReport),wrongReport.sourceArtifact);
    Reject([&]{LoadLocalBake(wrongPath,baked.asset.semantic);},"report source hash mismatch","REPORT_SOURCE_BINDING load inspects source binding inside hashed report");
    Check(LoadLocalBake(path,baked.asset.semantic)->ContentSHA256()==asset->ContentSHA256(),"report binding failure preserves prior valid publication");
    auto corrupt=baked;corrupt.report.back()^=1;Reject([&]{PublishLocalBake(path,corrupt);},"report content","report mutation cannot publish asset");
    const auto reportPath=ReportPath(path,baked);std::filesystem::rename(reportPath,reportPath+".hold");Reject([&]{LoadLocalBake(path,baked.asset.semantic);},"bound statistics report","asset missing sidecar refused");std::filesystem::rename(reportPath+".hold",reportPath);
    {std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary);file.seekg(-1,std::ios::end);char byte=0;file.read(&byte,1);byte^=1;file.seekp(-1,std::ios::end);file.write(&byte,1);}
    Reject([&]{LoadLocalBake(path,baked.asset.semantic);},"SHA256","asset byte corruption refused");
    std::filesystem::remove_all(directory);
    Check(true,"BAKE_GUARD all controls");
    std::cout<<passed<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL BAKE_GUARD "<<e.what()<<'\n';return 1;}}
