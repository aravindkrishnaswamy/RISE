#include "../src/Library/Materials/HyLIoS/HyLIoSLocalResponseAsset.h"
#include "Utilities/RISECBOR64.h"
#include "Utilities/DiskFileWriteBuffer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <chrono>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace RISE::HyLIoS;
using V=RISE::RISECBOR64::Value;
using Bytes=RISE::RISECBOR64::Bytes;
namespace {
int failed=0, passed=0;
void Check(bool good,const std::string& marker)
{ if(good) { ++passed; std::cout<<"ok: "<<marker<<'\n'; } else { ++failed; std::cout<<"FAIL "<<marker<<'\n'; } }
template<class F> void Refuse(F action,const std::string& marker,const std::string& reason="")
{
    try { action(); Check(false,marker+" (accepted)"); }
    catch(const std::exception& e) { Check(reason.empty() || std::string(e.what()).find(reason)!=std::string::npos,marker+" ["+e.what()+"]"); }
}
Bytes CBOR(const V& value)
{ Bytes out; std::string error; if(!RISE::RISECBOR64::Encode(value,out,&error)) throw std::runtime_error(error); return out; }
V UnCBOR(const Bytes& bytes)
{ V out; std::string error; if(!RISE::RISECBOR64::DecodeCanonical(bytes,out,&error)) throw std::runtime_error(error); return out; }
void Put32(Bytes& b,std::uint32_t n) { for(unsigned i=0;i<4;++i)b.push_back(static_cast<unsigned char>(n>>(8*i))); }
void Put64(Bytes& b,std::uint64_t n) { for(unsigned i=0;i<8;++i)b.push_back(static_cast<unsigned char>(n>>(8*i))); }
std::uint64_t Get64(const unsigned char* p) { std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t{p[i]}<<(8*i);return n; }
void Set64(Bytes& b,std::size_t at,std::uint64_t n) { for(unsigned i=0;i<8;++i)b[at+i]=static_cast<unsigned char>(n>>(8*i)); }
void Hash(Bytes& b)
{
    const auto hash=RISE::RISECBOR64::SHA256Hex(b);
    for(unsigned i=0;i<64;i+=2)b.push_back(static_cast<unsigned char>(std::stoul(hash.substr(i,2),nullptr,16)));
}
Bytes Read(const std::string& path)
{ std::ifstream f(path,std::ios::binary);return Bytes(std::istreambuf_iterator<char>(f),{}); }
void Write(const std::string& path,const Bytes& b)
{ std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size()); }
void Rehash(Bytes& b) { b.resize(b.size()-32);Hash(b); }
V Replace(const V& map,const std::string& key,const V& value)
{
    V::Members members=map.GetMap();
    for(auto& m:members)if(m.first==key){m.second=value;return V::MapValue(members);}
    throw std::runtime_error("replace missing field");
}
Bytes AlterMetadata(const Bytes& file,const std::function<V(V)>& alter)
{
    const auto n=Get64(file.data()+16);
    auto metadata=CBOR(alter(UnCBOR(Bytes(file.begin()+80,file.begin()+80+n))));
    Bytes result(file.begin(),file.begin()+80);Set64(result,16,metadata.size());
    result.insert(result.end(),metadata.begin(),metadata.end());
    result.insert(result.end(),file.begin()+80+n,file.end()-32);Hash(result);return result;
}
LocalAssetDefinition Fixture()
{
    LocalAssetDefinition d;
    const Bytes record=CBOR(V::MapValue({{"actual",V::Float(1.125)},{"revision",V::String("synthetic_v1")}}));
    d.semantic={record,record,record,record,record,record,record,record,record,400,700};
    d.production={"synthetic_oracle_v1","hylios_angular_lineage_v1","analog_v1","index_mapping_v1","synthetic_stats_v1",
        0xffffffffffffffffULL,16,2,32,512,record,record};
    d.response.incident={{0,.5,1},{0,LocalResponseTwoPi}};
    d.response.outgoing={{0,.25,1},{0,LocalResponseTwoPi}};
    d.response.wavelengthNm={400,700};
    d.response.continuous={.1,.2,.3,.1,.15,.05,.25,.125};
    d.response.mirror={.125,.25,.0625,.125};
    d.normalIncidenceGuide={.35,.45};return d;
}
// Independent writer: spell metadata/framing directly without the codec's save
// path or its input structs. Coefficients are exact IEEE float32 bit patterns.
Bytes Independent(const LocalAssetDefinition& d)
{
    auto rec=V::BytesValue(d.semantic.model);
    V semantic=V::MapValue({{"schema",V::String("caller_prepared_hylios_semantics_v1")},
        {"model",rec},{"placement",rec},{"postscatter",rec},{"boundary",rec},{"lineage",rec},
        {"data",rec},{"specimen",rec},{"exterior",rec},{"approximation",rec},
        {"covered_first_nm",V::Float(400)},{"covered_last_nm",V::Float(700)}});
    V production=V::MapValue({{"oracle",V::String("fixture_oracle")},{"lineage",V::String("fixture_lineage")},
        {"estimator",V::String("fixture_analog")},{"rng",V::String("splitmix64_index_top53_v1")},
        {"seed",V::Unsigned(9007199254740993ULL)},{"history_mapping",V::String("fixture_index_v1")},
        {"histories",V::Unsigned(100)},{"replicates",V::Unsigned(2)},{"guide_histories",V::Unsigned(200)},
        {"maximum_work",V::Unsigned(1000)},{"statistics_revision",V::String("fixture_stats")},{"build",rec},{"statistics",rec},
        {"encoding",V::String("positive_float32_downward_le_v1")},
        {"grid_rule",V::String("full_independent_projected_cells_average_v1")},
        {"guide_experiment",V::String("normal_incidence_separate_experiment_v1")},
        {"spectral_interpolation",V::String("nonnegative_linear_physical_nm_v1")},
        {"atomic_map",V::String("same_exterior_planar_mirror_v1")},
        {"azimuth",V::String("right_handed_outward_z_phi_atan2_y_x_no_reduction_v1")},
        {"physical_accounting",V::String("producer_declared_complete_no_unresolved_or_unclassified_mass_v1")}});
    auto arr=[](std::initializer_list<double> values){V::Values a;for(double x:values)a.push_back(V::Float(x));return V::ArrayValue(a);};
    Bytes meta=CBOR(V::MapValue({{"format",V::String("hylios_local_asset_v1")},{"semantic",semantic},{"production",production},
        {"incident_t",arr({0,1})},{"incident_phi",arr({0,LocalResponseTwoPi})},
        {"outgoing_t",arr({0,1})},{"outgoing_phi",arr({0,LocalResponseTwoPi})},
        {"wavelength_nm",arr({400,700})},
        {"encoding_error",V::MapValue({{"maximum_coefficient_absolute",V::Float(0)},
        {"maximum_forward_row_absolute",V::Float(0)},{"status",V::String("producer_computed_downward_bound_v1")}})}}));
    Bytes file={'H','Y','L','O','C','A','L',0};Put32(file,1);Put32(file,0x01020304);Put64(file,meta.size());Put64(file,24);
    for(auto n:{2,2,2,2,2,0})Put64(file,n);
    file.insert(file.end(),meta.begin(),meta.end());
    for(auto bits:{0x3e000000u,0x3d800000u,0x3e800000u,0x3f000000u,0x3f000000u,0x3e800000u})Put32(file,bits);
    Hash(file);return file;
}
}
int main()
{
#ifdef _WIN32
    const auto dir=std::filesystem::temp_directory_path()/("hylios-asset-test-"+
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    if(!std::filesystem::create_directory(dir))return 2;
#else
    char dirTemplate[]="/tmp/hylios-asset-test-XXXXXX";char* root=mkdtemp(dirTemplate);
    if(!root)return 2;chmod(root,0700);
    const auto dir=std::filesystem::path(root);
#endif
    const auto path=(dir/"asset.hyl").string(), bad=(dir/"bad.hyl").string();
    try {
        auto d=Fixture(); SaveLocalResponseAsset(path,d);
        const auto original=Read(path); auto asset=LoadLocalResponseAsset(path,d.semantic);
        Check(asset->ProductionSHA256()==RISE::RISECBOR64::SHA256Hex(original.data()+80,static_cast<std::size_t>(Get64(original.data()+16))),
            "complete production identity hashes exact canonical semantic plus production manifest");
        const double encoded=static_cast<double>(std::nextafter(.1f,0.0f));
        Check(asset->Response().EvaluateContinuousNM({.1,0},{.1,0},400)==encoded,"downward actual float bits");
        Check(asset->Response().EvaluateContinuousNM({.1,0},{.1,0},400)<=.1,"encoding never increases physical F");
        Check(asset->Production().seed==std::numeric_limits<std::uint64_t>::max(),"exact uint64 production seed");
        Check(asset->NormalIncidenceGuide().size()==2 && asset->NormalIncidenceGuide()[0]<=.35 &&
            asset->NormalIncidenceGuide()[0]>.349,"separate guide nodes and downward coefficients");
        Check(asset->EncodingError().maximumCoefficientAbsoluteError>=.1-encoded &&
            asset->EncodingError().maximumForwardRowAbsoluteError>0,"declared encoding error bounds");
        for(auto member:{&LocalAssetSemanticIdentity::model,&LocalAssetSemanticIdentity::placement,
            &LocalAssetSemanticIdentity::postscatter,&LocalAssetSemanticIdentity::boundary,&LocalAssetSemanticIdentity::lineage,
            &LocalAssetSemanticIdentity::data,&LocalAssetSemanticIdentity::specimen,&LocalAssetSemanticIdentity::exterior,
            &LocalAssetSemanticIdentity::approximation}) {
            auto changedSemantic=d;changedSemantic.semantic.*member=CBOR(V::MapValue({{"actual",V::Float(7)}}));
            SaveLocalResponseAsset(bad,changedSemantic);auto changedIdentity=LoadLocalResponseAsset(bad,changedSemantic.semantic);
            Check(changedIdentity->ProductionSHA256()!=asset->ProductionSHA256() &&
                changedIdentity->SemanticSHA256()!=asset->SemanticSHA256(),"complete production identity includes each semantic record");
        }
        for(bool first:{true,false}) {
            auto changedCoverage=d;
            if(first) { changedCoverage.semantic.coveredFirstNm=401;changedCoverage.response.wavelengthNm.front()=401; }
            else { changedCoverage.semantic.coveredLastNm=701;changedCoverage.response.wavelengthNm.back()=701; }
            SaveLocalResponseAsset(bad,changedCoverage);auto changedIdentity=LoadLocalResponseAsset(bad,changedCoverage.semantic);
            Check(changedIdentity->ProductionSHA256()!=asset->ProductionSHA256() &&
                changedIdentity->SemanticSHA256()!=asset->SemanticSHA256(),"complete production identity includes coverage endpoint");
        }
        const auto rename=(dir/"renamed.hyl").string();Write(rename,original);
        Check(LoadLocalResponseAsset(rename,d.semantic)->ContentSHA256()==asset->ContentSHA256(),"content identity independent of path");
        const auto independent=Independent(d);Write(bad,independent);
        auto small=LoadLocalResponseAsset(bad,d.semantic);
        Check(small->Response().EvaluateContinuousNM({.5,1},{.5,1},400)==.125 &&
            small->Response().EvaluateContinuousNM({.5,1},{.5,1},700)==.0625,"independent framed little endian fixture");
        Check(small->Production().seed==9007199254740993ULL,"independent integer beyond double exact range");
        Check(small->NormalIncidenceGuide()[0]==.5 && small->NormalIncidenceGuide()[1]==.25,"independent normal guide physical values");
        const double c=.125*LocalResponsePi;
        Check(std::abs(small->Response().CameraContinuousMassNM({.5,1},400)-c)<1e-15,"CDF reconstructed from physical F");
        const auto sample=small->Response().SampleCameraNM({.5,1},400,.5,.3,.3,.5,.5,{53});
        const double z=c+.25;
        Check(sample.event==LocalResponseSample::Event::Continuous && std::abs(sample.weight-z)<1e-15 &&
            std::abs(sample.pdf-.125*std::sqrt(.5)/z)<1e-15,"loaded core sample matches independent physical complete mixture");
        for(auto member:{&LocalAssetSemanticIdentity::model,&LocalAssetSemanticIdentity::placement,
            &LocalAssetSemanticIdentity::postscatter,&LocalAssetSemanticIdentity::boundary,&LocalAssetSemanticIdentity::lineage,
            &LocalAssetSemanticIdentity::data,&LocalAssetSemanticIdentity::specimen,&LocalAssetSemanticIdentity::exterior,
            &LocalAssetSemanticIdentity::approximation}) {
            auto expected=d.semantic;expected.*member=CBOR(V::MapValue({{"actual",V::Float(7)}}));
            Refuse([&]{LoadLocalResponseAsset(path,expected);},"full semantic field mismatch","semantic mismatch:");
        }
        auto wrongDomain=d.semantic;wrongDomain.coveredFirstNm=401;
        Refuse([&]{LoadLocalResponseAsset(path,wrongDomain);},"domain identity mismatch","semantic mismatch:");
        auto invalid=d;invalid.semantic.coveredLastNm=699;
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"coverage node endpoint mismatch","covered interval");
        invalid=d;invalid.normalIncidenceGuide.pop_back();
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"guide cardinality refusal");
        invalid=d;invalid.normalIncidenceGuide[0]=1.01;
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"guide physical bound refusal");
        invalid=d;invalid.normalIncidenceGuide[0]=1e-50;
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"positive support quantization refusal","positive-to-zero");
        invalid=d;invalid.response.mirror[0]=1;
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"reject nonpassive original before quantization");
        invalid=d;invalid.production.historiesPerCellPerReplicate=std::numeric_limits<std::uint64_t>::max();
        Refuse([&]{SaveLocalResponseAsset(path,invalid);},"history work arithmetic overflow","overflow");
        LocalAssetLimits limits;limits.maximumFileBytes=80;
        Refuse([&]{LoadLocalResponseAsset(path,d.semantic,limits);},"file budget before read","budget");
        limits={};limits.maximumResidentBytes=1000;
        Refuse([&]{LoadLocalResponseAsset(path,d.semantic,limits);},"resident preflight refusal","resident budget");
        limits={};limits.maximumMetadataBytes=10;
        Refuse([&]{LoadLocalResponseAsset(path,d.semantic,limits);},"metadata preflight refusal","metadata");
        limits={};limits.response.maximumContinuousEntries=1;
        Refuse([&]{LoadLocalResponseAsset(path,d.semantic,limits);},"numeric core entry budget refusal");
        auto corrupt=original;corrupt[80+Get64(corrupt.data()+16)]^=1;Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"corrupt coefficient digest refusal","SHA256");
        corrupt=original;corrupt.push_back(0);Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"trailing byte refusal","extent");
        for(std::size_t n:{std::size_t{0},std::size_t{79},std::size_t{100},original.size()-1}) {
            Write(bad,Bytes(original.begin(),original.begin()+n));
            Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"truncation refusal");
        }
        corrupt=original;Set64(corrupt,32,std::numeric_limits<std::uint64_t>::max());Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"header enormous count before allocation","cardinality");
        corrupt=original;Set64(corrupt,24,std::numeric_limits<std::uint64_t>::max());Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"payload integer overflow refusal","extent");
        auto mutate=[&](const std::function<V(V)>& action,const std::string& marker) {
            Write(bad,AlterMetadata(original,action));Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},marker);
        };
        mutate([](V v){auto p=*v.Find("production");return Replace(v,"production",Replace(p,"seed",V::Float(1)));},"floating seed rejected");
        for(const auto* field:{"encoding","grid_rule","guide_experiment","spectral_interpolation","atomic_map","azimuth","physical_accounting","rng"})
            mutate([&](V v){auto p=*v.Find("production");return Replace(v,"production",Replace(p,field,V::String("unsupported")));},std::string("unsupported production ")+field);
        mutate([](V v){auto m=v.GetMap();m.push_back({"unknown",V::Unsigned(1)});return V::MapValue(m);},"unknown top level metadata key");
        mutate([](V v){auto p=v.Find("production")->GetMap();p.erase(p.begin());return Replace(v,"production",V::MapValue(p));},"missing production provenance field");
        mutate([](V v){return Replace(v,"wavelength_nm",V::ArrayValue({V::Float(401),V::Float(700)}));},"loaded coverage must match nodes");
        mutate([](V v){auto s=*v.Find("semantic");return Replace(v,"semantic",Replace(s,"data",V::BytesValue({0xa0})));},"empty opaque record refusal");
        // Independently put a negative IEEE physical value into a valid digest.
        corrupt=independent;const auto payloadAt=80+Get64(corrupt.data()+16);corrupt[payloadAt+3]|=0x80;Rehash(corrupt);Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"negative physical payload refusal","float32");
        corrupt=independent;corrupt[payloadAt]=0;corrupt[payloadAt+1]=0;corrupt[payloadAt+2]=0;corrupt[payloadAt+3]=0x80;
        Rehash(corrupt);Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"importer rejects negative wire zero","float32");
        corrupt=independent;corrupt[payloadAt]=0;corrupt[payloadAt+1]=0;corrupt[payloadAt+2]=0x80;corrupt[payloadAt+3]=0x7f;Rehash(corrupt);Write(bad,corrupt);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"nonfinite physical payload refusal","float32");
        // Corrupt schema bytes independently; do not ask the canonical encoder
        // to create the malformed data being tested.
        const auto metadataLength=Get64(original.data()+16);
        auto rawMeta=Bytes(original.begin()+80,original.begin()+80+metadataLength);
        auto malformed=[&](Bytes raw,const std::string& marker) {
            Bytes bytes(original.begin(),original.begin()+80);Set64(bytes,16,raw.size());
            bytes.insert(bytes.end(),raw.begin(),raw.end());
            bytes.insert(bytes.end(),original.begin()+80+metadataLength,original.end()-32);Hash(bytes);
            Write(bad,bytes);Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},marker);
        };
        auto noncanonical=rawMeta;noncanonical.insert(noncanonical.begin()+1,0x09);noncanonical[0]=0xb8;
        malformed(noncanonical,"noncanonical map length refusal");
        auto duplicate=rawMeta;
        // Top map has nine pairs. Add a duplicate UTF8 key/value directly.
        duplicate[0]=0xaa;
        const Bytes duplicatePair={0x66,'f','o','r','m','a','t',0x61,'x'};
        duplicate.insert(duplicate.end(),duplicatePair.begin(),duplicatePair.end());
        malformed(duplicate,"duplicate top level key refusal");
        auto depth=V::Unsigned(1);for(unsigned k=0;k<10;++k)depth=V::ArrayValue({depth});
        mutate([&](V v){return Replace(v,"format",depth);},"bounded metadata depth refusal");
        // A source whose nearest-float encoding is super-unit is still encoded
        // downward. The long-double reference uses the exact stored double pi.
        auto unit=d;unit.response.incident={{0,1},{0,LocalResponseTwoPi}};
        unit.response.outgoing=unit.response.incident;unit.response.continuous={.2,.2};
        const double mirror=std::nextafter(1-.2*LocalResponsePi,0.0);
        unit.response.mirror={mirror,mirror};
        const long double nearestMass=static_cast<long double>(.2f)*LocalResponsePi+static_cast<float>(mirror);
        Check(nearestMass>1,"nearest float counterexample actually violates passivity");
        SaveLocalResponseAsset(bad,unit);auto encodedUnit=LoadLocalResponseAsset(bad,d.semantic);
        Check(encodedUnit->Response().ForwardRowMassNM({.5,0},400)<=1,"downward quantization preserves near unit row");
        // Match actual encoding loss against its independent full-row integral.
        const long double sourceMass=static_cast<long double>(.2)*LocalResponsePi+mirror;
        const long double storedMass=static_cast<long double>(encodedUnit->Response().EvaluateContinuousNM({.5,0},{.5,0},400))*LocalResponsePi+
            encodedUnit->Response().CameraMirrorMassNM({.5,0},400);
        Check(sourceMass-storedMass<=encodedUnit->EncodingError().maximumForwardRowAbsoluteError,
            "encoding bound covers independently integrated row loss");
        auto zero=d;std::fill(zero.response.continuous.begin(),zero.response.continuous.end(),0);
        std::fill(zero.response.mirror.begin(),zero.response.mirror.end(),0);
        zero.response.continuous[0]=-0.0;
        SaveLocalResponseAsset(bad,zero);auto loadedZero=LoadLocalResponseAsset(bad,d.semantic);
        const auto zeroBytes=Read(bad);const auto zeroAt=80+Get64(zeroBytes.data()+16);
        Check(zeroBytes[zeroAt]==0 && zeroBytes[zeroAt+1]==0 && zeroBytes[zeroAt+2]==0 && zeroBytes[zeroAt+3]==0,
            "producer canonicalizes signed mathematical zero to positive wire zero");
        Check(loadedZero->Response().CameraContinuousMassNM({.3,.7},550)==0 &&
            loadedZero->Response().CameraPdfNM({.2,.1},{.3,.7},550)==0,"exact zero columns rebuilt without normalization");
        auto mono=zero;mono.response.wavelengthNm={500};mono.semantic.coveredFirstNm=500;mono.semantic.coveredLastNm=500;
        mono.response.continuous.resize(4);mono.response.mirror.resize(2);mono.normalIncidenceGuide={.25};
        SaveLocalResponseAsset(bad,mono);auto loadedMono=LoadLocalResponseAsset(bad,mono.semantic);
        Check(loadedMono->Response().Covers(500,500) && !loadedMono->Response().Covers(499,500),"one node exact monochromatic coverage");
        SaveLocalResponseAsset(bad,d);const auto productionHash=LoadLocalResponseAsset(bad,d.semantic)->ProductionSHA256();
        auto gridChanged=d;gridChanged.response.incident.tEdges[1]=.6;
        SaveLocalResponseAsset(bad,gridChanged);
        Check(LoadLocalResponseAsset(bad,d.semantic)->ProductionSHA256()!=productionHash,"full production key includes actual grid boundaries");
        // Publication failure on a non-file destination leaves its contents.
        const auto occupied=dir/"occupied";std::filesystem::create_directory(occupied);Write((occupied/"old").string(),{1,2,3});
        Refuse([&]{SaveLocalResponseAsset(occupied.string(),d);},"atomic non-file destination refusal");
        Check(Read((occupied/"old").string())==Bytes({1,2,3}),"atomic refusal preserves previous directory contents");
#ifndef _WIN32
        const auto fifo=(dir/"fifo").string();
        if(mkfifo(fifo.c_str(),0600)!=0)throw std::runtime_error("cannot make FIFO fixture");
        Refuse([&]{LoadLocalResponseAsset(fifo,d.semantic);},"FIFO refused without blocking open","regular file");
#endif
        // Smallest positive IEEE binary32 is preserved even under fast-math.
        auto subnormal=independent;const auto subAt=80+Get64(subnormal.data()+16);
        subnormal[subAt]=1;subnormal[subAt+1]=0;subnormal[subAt+2]=0;subnormal[subAt+3]=0;
        Rehash(subnormal);Write(bad,subnormal);auto loadedSubnormal=LoadLocalResponseAsset(bad,d.semantic);
        Check(loadedSubnormal->Response().EvaluateContinuousNM({.5,0},{.5,0},400)==std::ldexp(1.0,-149),
            "float32 subnormal payload retains exact positive support");
        auto nonpassive=independent;
        nonpassive[subAt]=0;nonpassive[subAt+1]=0;nonpassive[subAt+2]=0x80;nonpassive[subAt+3]=0x3f;
        Rehash(nonpassive);Write(bad,nonpassive);
        Refuse([&]{LoadLocalResponseAsset(bad,d.semantic);},"rehashed unsafe physical row independently rejected","passivity");
        // UTF8 callers and independently spelled native-wide expectations must
        // identify the same files regardless of the Windows active code page.
        using Disk=RISE::Implementation::DiskFileWriteBuffer;
#ifdef _WIN32
        const auto unicodeParent=dir/std::filesystem::path(L"parent-\u00e9-\u6f22");
        const auto unicodeNative=unicodeParent/std::filesystem::path(L"caf\u00e9-\u6f22.hyl");
#else
        const auto unicodeParent=dir/std::filesystem::u8path(u8"parent-\u00e9-\u6f22");
        const auto unicodeNative=unicodeParent/std::filesystem::u8path(u8"caf\u00e9-\u6f22.hyl");
#endif
        std::filesystem::create_directory(unicodeParent);
        std::filesystem::permissions(unicodeParent,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
        const std::string unicodeUTF8=dir.u8string()+"/"+u8"parent-\u00e9-\u6f22/caf\u00e9-\u6f22.hyl";
        SaveLocalResponseAsset(unicodeUTF8,d);auto unicodeLoaded=LoadLocalResponseAsset(unicodeUTF8,d.semantic);
        Check(std::filesystem::is_regular_file(unicodeNative) && unicodeLoaded->ContentSHA256()==asset->ContentSHA256(),
            "UTF8 parent and leaf save/load reaches independently expected native file");
        auto unicodeChanged=d;unicodeChanged.response.continuous[0]=.05;SaveLocalResponseAsset(unicodeUTF8,unicodeChanged);
        Check(std::filesystem::is_regular_file(unicodeNative) &&
            LoadLocalResponseAsset(unicodeUTF8,d.semantic)->ContentSHA256()!=unicodeLoaded->ContentSHA256(),
            "UTF8 replacement publishes to same native pathname");
        const auto unicodeCopy=unicodeParent/"copy.bin", unicodeAlias=unicodeParent/"alias.bin";
        Disk::WriteAtomicallyUTF8(unicodeCopy.u8string(),[](Disk& output){return output.setBytes("first",5);});
        std::filesystem::create_hard_link(unicodeCopy,unicodeAlias);unsigned copyCalls=0;
        const bool unicodeCopied=Disk::WriteAtomicallyUTF8(unicodeCopy.u8string(),[&](Disk& output){
            ++copyCalls;return output.setBytes("replacement",11);
        },Disk::StagingPolicy::AllowInPlace);
        std::ifstream aliasInput(unicodeAlias,std::ios::binary);
        const std::string aliasBytes(std::istreambuf_iterator<char>(aliasInput),{});aliasInput.close();
        Check(unicodeCopied && copyCalls==1 && std::filesystem::equivalent(unicodeCopy,unicodeAlias) && aliasBytes=="replacement",
            "UTF8 in-place copy retains hardlink and encodes once");
        const auto unicodeFailure=unicodeParent/"publication-conflict.hyl";
        Disk::WriteDiagnostics unicodeDiagnostics;
        const bool unicodeSaved=Disk::WriteAtomicallyUTF8(unicodeFailure.u8string(),[&](Disk& output){
            if(!output.setBytes(original.data(),static_cast<unsigned int>(original.size())))return false;
            std::filesystem::create_directory(unicodeFailure);
            std::ofstream(unicodeFailure/"sentinel")<<"old";
            return true;
        },Disk::StagingPolicy::Required,&unicodeDiagnostics);
        Check(!unicodeSaved && unicodeDiagnostics.operation=="rename staging output" &&
            !unicodeDiagnostics.recoveryPath.empty() && std::filesystem::is_regular_file(unicodeFailure/"sentinel"),
            "UTF8 failed atomic publication retains completed recovery and prior destination");
        const auto unicodeRecovery=std::filesystem::u8path(unicodeDiagnostics.recoveryPath);
        Check(unicodeDiagnostics.recoveryPath.find(u8"parent-\u00e9-\u6f22")!=std::string::npos &&
            std::filesystem::equivalent(unicodeRecovery.parent_path().parent_path(),unicodeParent) &&
            LoadLocalResponseAsset(unicodeDiagnostics.recoveryPath,d.semantic)->ContentSHA256()==asset->ContentSHA256(),
            "UTF8 recovery diagnostic reopens same completed asset");
#ifndef _WIN32
        const auto writeOnlyTarget=unicodeParent/"write-only.hyl";
        SaveLocalResponseAsset(writeOnlyTarget.u8string(),d);
        std::filesystem::permissions(writeOnlyTarget,std::filesystem::perms::owner_write,std::filesystem::perm_options::replace);
        SaveLocalResponseAsset(writeOnlyTarget.u8string(),d);
        Check((std::filesystem::status(writeOnlyTarget).permissions() & std::filesystem::perms::all)==std::filesystem::perms::owner_write,
            "successful write-only replacement preserves exact destination mode");
        Disk::WriteDiagnostics writeOnlyDiagnostics;
        const bool writeOnlySaved=Disk::WriteAtomicallyUTF8(writeOnlyTarget.u8string(),[&](Disk& output){
            if(!output.setBytes(original.data(),static_cast<unsigned int>(original.size())))return false;
            std::filesystem::remove(writeOnlyTarget);std::filesystem::create_directory(writeOnlyTarget);
            return true;
        },Disk::StagingPolicy::Required,&writeOnlyDiagnostics);
        bool writeOnlyReadable=false, recoveryReadBit=false;
        if(!writeOnlyDiagnostics.recoveryPath.empty()) {
            try {
                writeOnlyReadable=LoadLocalResponseAsset(writeOnlyDiagnostics.recoveryPath,d.semantic)->ContentSHA256()==asset->ContentSHA256();
                recoveryReadBit=(std::filesystem::status(std::filesystem::u8path(writeOnlyDiagnostics.recoveryPath)).permissions() &
                    std::filesystem::perms::owner_read)!=std::filesystem::perms::none;
            } catch(const std::exception&) { writeOnlyReadable=false; }
        }
        Check(!writeOnlySaved && writeOnlyDiagnostics.operation=="rename staging output" && writeOnlyDiagnostics.error && writeOnlyReadable && recoveryReadBit,
            "failed write-only replacement retains readable complete recovery asset and original failure");
#endif
        // Save-side independent IEEE boundary controls use a microscopic
        // dyadic-area output cell so even FLT_MAX density is physically passive.
        auto boundary=d;boundary.response.incident={{0,1},{0,LocalResponseTwoPi}};
        boundary.response.outgoing={{0,std::ldexp(1.0,-130),1},{0,2,LocalResponseTwoPi}};
        boundary.response.wavelengthNm={400};boundary.semantic.coveredLastNm=400;
        boundary.response.continuous={0,0,0,0};boundary.response.mirror={0};boundary.normalIncidenceGuide={0};
        const double minimumNormal=std::ldexp(1.0,-126);
        const double maximumFloat=std::ldexp(16777215.0,104);
        const std::pair<double,std::uint32_t> bitCases[]={
            {std::ldexp(1.0,-149),0x00000001u},
            {std::nextafter(std::ldexp(1.0,-149),1.0),0x00000001u},
            {std::nextafter(minimumNormal,0.0),0x007fffffu},
            {minimumNormal,0x00800000u},
            {std::nextafter(minimumNormal,1.0),0x00800000u},
            {std::nextafter(maximumFloat,0.0),0x7f7ffffeu},
            {maximumFloat,0x7f7fffffu}};
        for(const auto& control:bitCases) {
            boundary.response.continuous[0]=control.first;SaveLocalResponseAsset(bad,boundary);
            const auto bytes=Read(bad);const auto offset=80+Get64(bytes.data()+16);
            const std::uint32_t actual=std::uint32_t{bytes[offset]}|(std::uint32_t{bytes[offset+1]}<<8)|
                (std::uint32_t{bytes[offset+2]}<<16)|(std::uint32_t{bytes[offset+3]}<<24);
            Check(actual==control.second,"save-side independent IEEE boundary bits "+std::to_string(control.second));
        }
        boundary.response.outgoing.tEdges[1]=std::ldexp(1.0,-1070);
        boundary.response.continuous[0]=std::nextafter(maximumFloat,0.0);
        SaveLocalResponseAsset(bad,boundary);auto microscopic=LoadLocalResponseAsset(bad,boundary.semantic);
        const double coefficientLoss=boundary.response.continuous[0]-
            microscopic->Response().EvaluateContinuousNM({.5,0},{std::ldexp(1.0,-1071),1},400);
        const double exactDyadicLoss=std::ldexp(coefficientLoss,-1070);
        Check(exactDyadicLoss>0 && exactDyadicLoss<=microscopic->EncodingError().maximumForwardRowAbsoluteError,
            "microscopic dyadic area positive row loss conservatively bounded");
        Refuse([&]{LocalAssetDetail::SaveObserved(path,d,{},[]{throw std::runtime_error("injected publication failure");});},"atomic failure seam refuses publication");
        Check(Read(path)==original,"atomic failure preserves old destination bytes");
        // Immutable loaded bytes survive atomic replacement of the pathname.
        auto changed=d;changed.response.continuous[0]=.05;SaveLocalResponseAsset(path,changed);
        Check(asset->Response().EvaluateContinuousNM({.1,0},{.1,0},400)==encoded,"loaded immutable lifetime after replacement");
        Check(LoadLocalResponseAsset(path,d.semantic)->ContentSHA256()!=asset->ContentSHA256(),"replacement has new immutable identity");
    } catch(const std::exception& e) { Check(false,std::string("unexpected exception: ")+e.what()); }
    std::filesystem::remove_all(dir);
    std::cout<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
