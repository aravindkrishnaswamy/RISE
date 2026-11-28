// Analytic raw radiometric primitive controls; no renderer/source calibration claim.
#include "../src/Library/Rendering/RadiometricFilm.h"
#include "../src/Library/Rendering/RadiometricCheckpoint.h"
#include "../src/Library/Rendering/RadiometricPublication.h"
#include "../src/Library/RasterImages/RadiometricEXRWriter.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifndef NO_EXR_SUPPORT
#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfDoubleAttribute.h>
#include <ImfStringAttribute.h>
#endif
using namespace RISE;
using namespace RISE::Implementation;
using RISECBOR64::Bytes;
using RISECBOR64::Value;
namespace fs=std::filesystem;
static int failures=0, checks=0;
void Check(bool pass,const char* message) { ++checks; if(!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
    else if(std::strstr(message,"zero-mean/nonzero-M2") || std::strstr(message,"recovery path") || std::strstr(message,"adjacent-variance") || std::strstr(message,"mean-trend") || std::strstr(message,"compensated-FLOAT") || std::strstr(message,"mixed-scale") || std::strstr(message,"subnormal-resolve")) std::printf("PASS: %s\n",message);
}
RadiometricIdentity Identity(std::uint32_t width=1,std::uint32_t height=1,std::uint64_t spp=3) {
    RadiometricIdentity id; id.width=width; id.height=height; id.wavelengthNM=1200;
    id.targetSamples=spp; id.seed=4711; id.seedAlgorithm="external_fixture_indexed_v1";
    std::string error;
    Check(RISECBOR64::Encode(Value::MapValue({{"fixture",Value::String("synthetic_unverified")},
        {"config",Value::String("analytic_constant")}, {"build",Value::String("primitive_test")}}),id.provenance,&error),"encode fixture provenance");
    return id;
}
bool SameDoubleBits(double a,double b) { std::uint64_t x=0,y=0; std::memcpy(&x,&a,8); std::memcpy(&y,&b,8); return x==y; }
Bytes Read(const fs::path& path) { std::ifstream f(path,std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f),{}); }
void Rehash(Bytes& bytes) {
    const auto digest=RISECBOR64::SHA256Hex(bytes.data(),bytes.size()-64);
    std::copy(digest.begin(),digest.end(),bytes.end()-64);
}
std::uint64_t Get64(const unsigned char* p) { std::uint64_t value=0; for(unsigned int i=0;i<8;++i) value|=static_cast<std::uint64_t>(p[i])<<(8*i); return value; }
void Set64(Bytes& bytes,std::size_t offset,std::uint64_t value) { for(unsigned int i=0;i<8;++i) bytes[offset+i]=static_cast<unsigned char>(value>>(8*i)); }
void SetDouble(Bytes& bytes,std::size_t offset,double value) { std::uint64_t bits=0; std::memcpy(&bits,&value,8); Set64(bytes,offset,bits); }
Bytes EditHeader(const Bytes& original,const std::string& key,const Value& replacement) {
    const auto length=static_cast<std::size_t>(Get64(original.data()+16));
    Value header; std::string error; Check(RISECBOR64::DecodeCanonical(original.data()+24,length,header,&error),"decode test header");
    auto members=header.GetMap(); bool found=false;
    for(auto& pair:members) if(pair.first==key) { pair.second=replacement; found=true; }
    if(!found) members.push_back({key,replacement});
    Bytes changed; Check(RISECBOR64::Encode(Value::MapValue(members),changed,&error),"encode changed test header");
    Bytes result(original.begin(),original.begin()+24); Set64(result,16,changed.size());
    result.insert(result.end(),changed.begin(),changed.end());
    result.insert(result.end(),original.begin()+24+length,original.end()-64); result.resize(result.size()+64); Rehash(result); return result;
}
#ifndef _WIN32
// Exercise the production private publication helper at its per-call observer
// boundary. Only the observer removes directory write permission; encoding,
// Finish and rename all use the exact default production path, without a race.
template<class Publish, class Inspect>
void LatePublicationFailure(const fs::path& root, Publish publish, Inspect inspect) {
    if(::geteuid()==0) { std::printf("SKIP: permission-based late publication failure under root\n"); return; }
    fs::create_directory(root); fs::permissions(root,fs::perms::owner_all);
    const auto destination=root/"existing";
    { std::ofstream original(destination); original << "ORIGINAL DESTINATION"; }
    const auto prior=Read(destination);
    bool changed=false; unsigned int observed=0;
    std::string diagnostic;
    const bool published=publish(destination.string(),diagnostic,[&] {
        ++observed;
        std::error_code error;
        fs::permissions(root,fs::perms::owner_read|fs::perms::owner_exec,error);
        changed=!error;
    });
    fs::permissions(root,fs::perms::owner_all);
    Check(changed && observed==1 && !published && Read(destination)==prior,"late atomic publication failure preserves prior destination");
    fs::path recovery;
    std::size_t retained=0;
    for(const auto& entry:fs::directory_iterator(root)) if(entry.is_directory()) {
        ++retained;
        if(fs::is_regular_file(entry.path()/"output")) recovery=fs::canonical(entry.path()/"output");
    }
    Check(retained==1 && !recovery.empty(),"late publication deliberately retains one completed recovery output");
    Check(!recovery.empty() && diagnostic.find(recovery.string())!=std::string::npos,"recovery path returned in publication failure diagnostic");
    if(!recovery.empty()) inspect(recovery);
    fs::remove_all(root);
}
#endif
int main() {
    std::string error;
    auto id=Identity(); auto film=RadiometricFilm::Create(id,error); Check(bool(film),"create scalar film"); if(!film) return 1;
    Check(film->Commit(0,0,0,0,error) && film->Commit(0,0,1,2,error) && film->Commit(0,0,2,4,error),"all zero/nonzero trials accepted");
    auto complete=film->Snapshot(); RadiometricStatistics stats;
    Check(complete.status==RadiometricStatus::Complete && complete.pixels[0].count==3 && complete.pixels[0].nextSampleIndex==3,"fixed sample completion/count/index");
    Check(RadiometricPixelStatistics(complete.pixels[0],stats,error) && stats.mean==2 && stats.sampleVariance==4 && stats.meanVariance==4.0/3.0 && stats.varianceAvailable,"analytic {0,2,4} mean sample variance and mean variance");
    auto zero=RadiometricFilm::Create(id,error); for(std::uint64_t i=0;i<3;++i) Check(zero->Commit(0,0,i,0,error),"zero trials count");
    Check(RadiometricPixelStatistics(zero->Snapshot().pixels[0],stats,error) && stats.mean==0 && stats.meanVariance==0 && stats.varianceAvailable,"black complete image has actual zero variance");
    RadiometricPixel empty; Check(RadiometricPixelStatistics(empty,stats,error) && !stats.varianceAvailable,"n=0 variance unavailable");
    empty.count=empty.nextSampleIndex=1; Check(RadiometricPixelStatistics(empty,stats,error) && !stats.varianceAvailable,"n=1 variance unavailable");
    RadiometricPixel varianceUnderflow; varianceUnderflow.count=varianceUnderflow.nextSampleIndex=2;
    varianceUnderflow.mean=1; varianceUnderflow.m2=std::numeric_limits<double>::denorm_min();
    Check(!RadiometricPixelStatistics(varianceUnderflow,stats,error),"required positive mean variance underflow is explicit failure");
    // Independent offline oracle, including a nonzero baseline and uneven values.
    auto varying=RadiometricFilm::Create(Identity(1,1,100),error); long double sum=0,squares=0;
    for(std::uint64_t i=0;i<100;++i) { const double v=1000.0+static_cast<double>((i*17)%31); sum+=v; Check(varying->Commit(0,0,i,v,error),"offline-reference sample accepted"); }
    const long double expectedMean=sum/100;
    for(std::uint64_t i=0;i<100;++i) { const long double d=1000.0+static_cast<double>((i*17)%31)-expectedMean; squares+=d*d; }
    Check(RadiometricPixelStatistics(varying->Snapshot().pixels[0],stats,error) && std::abs(stats.mean-static_cast<double>(expectedMean))<1e-10 && std::abs(stats.sampleVariance-static_cast<double>(squares/99))<1e-10,"Welford versus independently centered offline variance");
    // Independent hex/power-of-two oracle. For adjacent a,b separated by
    // d=2^(scale-52), two observations have M2=d^2/2; estimated mean
    // variance is M2/[n(n-1)]=d^2/4. This is the unbiased sample-variance
    // contract, not population two-point variance divided by sample count.
    for(int scale:{-484,-480,-200,0,200,500,564}) for(bool reverse:{false,true}) {
        const double low=std::ldexp(0x1.0000000000001p+0,scale);
        const double high=std::ldexp(0x1.0000000000002p+0,scale);
        auto adjacent=RadiometricFilm::Create(Identity(1,1,2),error);
        const bool accepted=adjacent->Commit(0,0,0,reverse ? high : low,error) && adjacent->Commit(0,0,1,reverse ? low : high,error);
        auto snapshot=adjacent->Snapshot(); RadiometricStatistics actual;
        Check(accepted && snapshot.status==RadiometricStatus::Complete &&
            SameDoubleBits(snapshot.pixels[0].m2,std::ldexp(1.0,2*scale-105)) &&
            RadiometricPixelStatistics(snapshot.pixels[0],actual,error) &&
            SameDoubleBits(actual.meanVariance,std::ldexp(1.0,2*scale-106)),
            "adjacent-variance two-point order/scaled independent M2 and mean-variance oracle");
    }
    for(bool reverse:{false,true}) {
        const double a=0x1.0000000000001p+0,b=0x1.0000000000002p+0;
        auto adjacent=RadiometricFilm::Create(Identity(1,1,3),error);
        const double values[]={reverse ? b : a,reverse ? a : b,reverse ? b : a};
        bool accepted=true; for(std::uint64_t i=0;i<3;++i) accepted=accepted && adjacent->Commit(0,0,i,values[i],error);
        // For {a,b,a}, M2=2*d^2/3. The third update must retain the
        // sub-ULP residual of the first pair's mathematical mean.
        Check(accepted && SameDoubleBits(adjacent->Snapshot().pixels[0].m2,std::ldexp(2.0/3.0,-104)),
            "adjacent-variance three-point order independent rational M2 oracle");
    }
    for(bool reverse:{false,true}) {
        const double a=0x1.0000000000001p+0,b=0x1.0000000000002p+0;
        auto adjacent=RadiometricFilm::Create(Identity(1,1,4),error);
        const double values[]={reverse ? b : a,reverse ? a : b,reverse ? b : a,reverse ? a : b};
        bool accepted=true; for(std::uint64_t i=0;i<4;++i) accepted=accepted && adjacent->Commit(0,0,i,values[i],error);
        Check(accepted && SameDoubleBits(adjacent->Snapshot().pixels[0].m2,std::ldexp(1.0,-104)),
            "adjacent-variance four-point balanced order independent M2 oracle");
    }
    for(int scale:{-600,600}) for(bool reverse:{false,true}) {
        const double a=std::ldexp(0x1.0000000000001p+0,scale),b=std::ldexp(0x1.0000000000002p+0,scale);
        auto unrepresentable=RadiometricFilm::Create(Identity(1,1,2),error);
        const bool first=unrepresentable->Commit(0,0,0,reverse ? b : a,error);
        const bool second=unrepresentable->Commit(0,0,1,reverse ? a : b,error);
        const auto snapshot=unrepresentable->Snapshot();
        Check(first && !second && snapshot.status==RadiometricStatus::Invalid && snapshot.pixels[0].count==1 && snapshot.pixels[0].nextSampleIndex==1,
            "adjacent-variance unrepresentable scaled M2 invalidates before second count/index advances");
    }
    // Exact rational reference values generated with Python Fraction from
    // the initial binary64 mean and sample value, not a repeated FP update.
    // This reproduces the root probe's accepted large-count restored state.
    constexpr std::uint64_t trendStart=1000000000,trendLength=1024;
    const std::uint64_t trendSteps[]={1,2,4,16,64,1024};
    const double trendOracle[2][6]={
        {0x1.12e0bdfb63689p-30,0x1.12e0bdf6c6d12p-30,0x1.12e0bded8da24p-30,0x1.12e0bdb63688fp-30,0x1.12e0bcd8da24bp-30,0x1.12e0ab8da373ep-30},
        {0x1.12e0be049c977p-30,0x1.12e0be09392efp-30,0x1.12e0be12725dep-30,0x1.12e0be49c9775p-30,0x1.12e0bf2725dc7p-30,0x1.12e0d0725c9dbp-30}
    };
    for(unsigned int direction=0;direction<2;++direction) {
        auto trendIdentity=Identity(1,1,trendStart+trendLength);
        auto initial=RadiometricFilm::Create(trendIdentity,error)->Snapshot(); auto& pixel=initial.pixels[0];
        pixel.count=pixel.nextSampleIndex=trendStart;
        pixel.mean=0x1.12e0be0000000p-30; pixel.m2=1-1e-9;
        auto trend=RadiometricFilm::Restore(initial,error); Check(bool(trend),"mean-trend restored large-count fixture accepted");
        std::size_t oracleIndex=0; double previous=pixel.mean; bool commits=bool(trend),monotonic=true;
        for(std::uint64_t k=1;k<=trendLength && trend;++k) {
            commits=commits && trend->Commit(0,0,trendStart+k-1,direction ? 2e-9 : 0,error);
            const double current=trend->Snapshot().pixels[0].mean;
            monotonic=monotonic && (direction ? current>previous : current<previous); previous=current;
            if(oracleIndex<6 && k==trendSteps[oracleIndex]) {
                Check(commits && SameDoubleBits(current,trendOracle[direction][oracleIndex]),"mean-trend repeated zero/positive updates match independent rational hex oracle");
                ++oracleIndex;
            }
        }
        Check(commits && monotonic && trend && trend->Snapshot().status==RadiometricStatus::Complete,
            "mean-trend every ordinary large-count update moves representably without failure");
    }
    // Exact analytically constructed nonnegative history: one 1 and N-1
    // zeros. This is not the root probe's approximate mean. All hex constants
    // come from tests/radiometric_moment_oracles.py using Fraction sums/squares.
    struct ConstructedOracle {
        std::uint64_t count; double mean,meanCorrection,m2,m2Correction;
        double values[2][6][3];
    };
    const ConstructedOracle constructed[]={
        {1000000ULL,0x1.0c6f7a0b5ed8dp-20,0x1.b5a63f9a49c2cp-75,0x1.ffffde7210be9p-1,0x1.093964a59c066p-55,{{{0x1.0c6f6873c67edp-20,0x1.ffffde7212f18p-1,0x1.1979732e269dep-40},{0x1.0c6f56dc30731p-20,0x1.ffffde7215248p-1,0x1.19794e4975db9p-40},{0x1.0c6f33ad0b467p-20,0x1.ffffde72198a6p-1,0x1.197904802a19bp-40},{0x1.0c6e6092edeaap-20,0x1.ffffde7233edap-1,0x1.197749cac4d63p-40},{0x1.0c6b1437709ddp-20,0x1.ffffde729d791p-1,0x1.19705f1dfbbb4p-40},{0x1.0c292e12e3468p-20,0x1.ffffde7ada3dap-1,0x1.18e63f1756334p-40}},{{0x1.0c6f687cc8560p-20,0x1.ffffde7212ef4p-1,0x1.1979732e269cap-40},{0x1.0c6f56ee34205p-20,0x1.ffffde7215200p-1,0x1.19794e4975d92p-40},{0x1.0c6f33d1129c4p-20,0x1.ffffde7219816p-1,0x1.197904802a14cp-40},{0x1.0c6e61230ad07p-20,0x1.ffffde7233c9ap-1,0x1.197749cac4c27p-40},{0x1.0c6b1677dd1ffp-20,0x1.ffffde729ce92p-1,0x1.19705f1dfb6c2p-40},{0x1.0c295210d3238p-20,0x1.ffffde7ad1407p-1,0x1.18e63f175144cp-40}}}},
        {1000000000ULL,0x1.12e0be826d695p-30,-0x1.34674bfabb83bp-84,0x1.fffffff768fa1p-1,-0x1.04dad2965cc5ap-55,{{{0x1.12e0be7dd0d1dp-30,0x1.fffffff768fa1p-1,0x1.2725dd133cedap-60},{0x1.12e0be79343a6p-30,0x1.fffffff768fa1p-1,0x1.2725dd0955a08p-60},{0x1.12e0be6ffb0b8p-30,0x1.fffffff768fa1p-1,0x1.2725dcf587065p-60},{0x1.12e0be38a3f21p-30,0x1.fffffff768fa1p-1,0x1.2725dc7eaf694p-60},{0x1.12e0bd5b478d7p-30,0x1.fffffff768fa1p-1,0x1.2725daa350f7ep-60},{0x1.12e0ac1010d46p-30,0x1.fffffff768faap-1,0x1.2725b57ff3c9bp-60}},{{0x1.12e0be870a00cp-30,0x1.fffffff768fa1p-1,0x1.2725dd133cedap-60},{0x1.12e0be8ba6983p-30,0x1.fffffff768fa1p-1,0x1.2725dd0955a08p-60},{0x1.12e0be94dfc72p-30,0x1.fffffff768fa1p-1,0x1.2725dcf587065p-60},{0x1.12e0becc36e08p-30,0x1.fffffff768fa1p-1,0x1.2725dc7eaf694p-60},{0x1.12e0bfa993453p-30,0x1.fffffff768fa1p-1,0x1.2725daa350f7ep-60},{0x1.12e0d0f4c9fe3p-30,0x1.fffffff768faap-1,0x1.2725b57ff3c9bp-60}}}},
        {9007199254739968ULL,0x1.0000000000200p-53,0x1.0000000000200p-139,0x1.fffffffffffffp-1,-0x1.0000000000200p-96,{{{0x1.0000000000200p-53,0x1.fffffffffffffp-1,0x1.00000000003ffp-106},{0x1.00000000001ffp-53,0x1.fffffffffffffp-1,0x1.00000000003fep-106},{0x1.00000000001fep-53,0x1.fffffffffffffp-1,0x1.00000000003fcp-106},{0x1.00000000001f8p-53,0x1.fffffffffffffp-1,0x1.00000000003f0p-106},{0x1.00000000001e0p-53,0x1.fffffffffffffp-1,0x1.00000000003c0p-106},{0x1.0000000000000p-53,0x1.fffffffffffffp-1,0x1.0000000000000p-106}},{{0x1.000000089725fp-53,0x1.fffffffffffffp-1,0x1.00000000003ffp-106},{0x1.000000112e2bep-53,0x1.fffffffffffffp-1,0x1.00000000003fep-106},{0x1.000000225c37bp-53,0x1.fffffffffffffp-1,0x1.00000000003fcp-106},{0x1.00000089707ecp-53,0x1.0000000000000p+0,0x1.00000000003f0p-106},{0x1.00000225c19b0p-53,0x1.0000000000001p+0,0x1.00000000003c1p-106},{0x1.0000225c17d05p-53,0x1.0000000000012p+0,0x1.0000000000012p-106}}}}
    };
    for(const auto& oracle:constructed) for(unsigned int direction=0;direction<2;++direction) {
        auto identity=Identity(1,1,oracle.count+1024);
        auto initial=RadiometricFilm::Create(identity,error)->Snapshot();
        auto& pixel=initial.pixels[0]; pixel.count=pixel.nextSampleIndex=oracle.count;
        pixel.mean=oracle.mean; pixel.meanCorrection=oracle.meanCorrection;
        pixel.m2=oracle.m2; pixel.m2Correction=oracle.m2Correction;
        auto run=RadiometricFilm::Restore(initial,error),uninterrupted=RadiometricFilm::Restore(initial,error);
        Check(bool(run) && bool(uninterrupted),"mean-trend exact valid one-high/rest-zero constructed state accepted");
        bool accepted=bool(run) && bool(uninterrupted); std::size_t selected=0;
        for(std::uint64_t k=1;k<=1024 && run && uninterrupted;++k) {
            const double value=direction ? 2e-9 : 0;
            accepted=accepted && run->Commit(0,0,oracle.count+k-1,value,error) && uninterrupted->Commit(0,0,oracle.count+k-1,value,error);
            if(selected<6 && k==trendSteps[selected]) {
                const auto current=run->Snapshot(); RadiometricStatistics statistics;
                Check(accepted && RadiometricPixelStatistics(current.pixels[0],statistics,error) &&
                    SameDoubleBits(current.pixels[0].mean,oracle.values[direction][selected][0]) &&
                    SameDoubleBits(current.pixels[0].m2,oracle.values[direction][selected][1]) &&
                    SameDoubleBits(statistics.meanVariance,oracle.values[direction][selected][2]),
                    "mean-trend large-count mean/M2/variance match independent exact-rational oracle");
                ++selected;
            }
            if(k==16) {
                run->Cancel(); const auto before=run->Snapshot(); Bytes bytes; RadiometricSnapshot restored;
                const bool decoded=EncodeRadiometricCheckpoint(before,bytes,error) && DecodeRadiometricCheckpoint(bytes,identity,restored,error);
                Check(decoded && SameDoubleBits(before.pixels[0].meanCorrection,restored.pixels[0].meanCorrection) &&
                    SameDoubleBits(before.pixels[0].m2Correction,restored.pixels[0].m2Correction),
                    "mean-trend checkpoint preserves both nonzero compensated residuals exactly");
                run=decoded ? RadiometricFilm::Restore(restored,error) : nullptr;
                accepted=accepted && bool(run) && run->Resume(identity,error);
            }
        }
        Bytes a,b;
        Check(accepted && run && uninterrupted && EncodeRadiometricCheckpoint(run->Snapshot(),a,error) &&
            EncodeRadiometricCheckpoint(uninterrupted->Snapshot(),b,error) && a==b,
            "mean-trend large-count resumed identity/state equals uninterrupted completion");
    }
    // Real sequential ordinary data, with no constructed initial state.
    const double ordinaryOracle[2][3]={
        {0x1.4f8a7ca7c7dd7p-17,0x1.fffeb07583584p-1,0x1.b7cbbd29beabfp-34},
        {0x1.4f8a7ca7c7ddcp-17,0x1.fffeb07583584p-1,0x1.b7cbbd29beabfp-34}
    };
    for(unsigned int direction=0;direction<2;++direction) {
        auto ordinary=RadiometricFilm::Create(Identity(1,1,100001),error);
        bool accepted=ordinary->Commit(0,0,0,1,error);
        for(std::uint64_t i=1;i<100001;++i) accepted=accepted && ordinary->Commit(0,0,i,direction ? 1e-20 : 0,error);
        const auto snapshot=ordinary->Snapshot(); RadiometricStatistics actual;
        Check(accepted && snapshot.status==RadiometricStatus::Complete && RadiometricPixelStatistics(snapshot.pixels[0],actual,error) &&
            SameDoubleBits(actual.mean,ordinaryOracle[direction][0]) && SameDoubleBits(snapshot.pixels[0].m2,ordinaryOracle[direction][1]) &&
            SameDoubleBits(actual.meanVariance,ordinaryOracle[direction][2]),
            "mean-trend 100000 ordinary zero/low-positive appends retain exact-rational mean/M2/variance");
    }
    // A tiny auxiliary increment may be unrepresentable by itself while the
    // required total M2 and both variances are normal. These finite inputs
    // are all accepted, and cancellation/resume retains every trial.
    for(double tiny:{1e-200,0x1p-600,std::numeric_limits<double>::denorm_min()}) for(bool reverse:{false,true}) {
        const auto identity=Identity(1,1,32);
        auto direct=RadiometricFilm::Create(identity,error);
        bool accepted=direct->Commit(0,0,0,reverse ? tiny : 1,error) && direct->Commit(0,0,1,reverse ? 1 : tiny,error);
        Check(accepted,"mixed-scale first pair of wide-range ordinary values accepted");
        const auto firstPair=direct->Snapshot(); auto checkpointFilm=RadiometricFilm::Restore(firstPair,error);
        if(checkpointFilm) checkpointFilm->Cancel();
        Bytes bytes; RadiometricSnapshot restored;
        bool resumedOkay=bool(checkpointFilm) && EncodeRadiometricCheckpoint(checkpointFilm->Snapshot(),bytes,error) && DecodeRadiometricCheckpoint(bytes,identity,restored,error);
        auto resumed=resumedOkay ? RadiometricFilm::Restore(restored,error) : nullptr;
        resumedOkay=resumedOkay && bool(resumed) && resumed->Resume(identity,error);
        for(std::uint64_t index=2;index<32;++index) {
            accepted=accepted && direct->Commit(0,0,index,0.5,error);
            resumedOkay=resumedOkay && resumed->Commit(0,0,index,0.5,error);
            if(index==2) {
                const auto snapshot=direct->Snapshot(); RadiometricStatistics actual;
                Check(accepted && RadiometricPixelStatistics(snapshot.pixels[0],actual,error) &&
                    SameDoubleBits(actual.mean,0.5) && SameDoubleBits(snapshot.pixels[0].m2,0.5) &&
                    SameDoubleBits(actual.sampleVariance,0.25) && SameDoubleBits(actual.meanVariance,1.0/12.0),
                    "mixed-scale tiny auxiliary square must not invalidate normal mean/M2/variance");
            }
        }
        const auto final=direct->Snapshot(); RadiometricStatistics actual;
        Check(accepted && final.status==RadiometricStatus::Complete && final.pixels[0].count==32 && final.pixels[0].nextSampleIndex==32 &&
            RadiometricPixelStatistics(final.pixels[0],actual,error) && SameDoubleBits(actual.mean,0.5) && SameDoubleBits(final.pixels[0].m2,0.5) &&
            SameDoubleBits(actual.sampleVariance,1.0/62.0) && SameDoubleBits(actual.meanVariance,1.0/1984.0),
            "mixed-scale repeated middle-value trials all count with independent normal-moment oracle");
        Bytes a,b;
        Check(resumedOkay && resumed && EncodeRadiometricCheckpoint(final,a,error) && EncodeRadiometricCheckpoint(resumed->Snapshot(),b,error) && a==b,
            "mixed-scale resumed identity and compensated moments equal uninterrupted result");
    }
    auto requiredUnderflow=RadiometricFilm::Create(Identity(1,1,2),error);
    Check(requiredUnderflow->Commit(0,0,0,0,error) && !requiredUnderflow->Commit(0,0,1,1e-200,error) &&
        requiredUnderflow->Snapshot().status==RadiometricStatus::Invalid && requiredUnderflow->Snapshot().pixels[0].count==1 &&
        requiredUnderflow->Snapshot().pixels[0].nextSampleIndex==1,
        "mixed-scale zero-prior positive total M2 underflow still invalidates without advancing sample");
    // Independent exact rational ties: M2 = n*(n-1)*(odd/2)*DBL_TRUE_MIN.
    // Both statistics must be rounded independently from that represented M2.
    struct SubnormalOracle {std::uint64_t n; double m2,correction,sampleVariance,meanVariance; bool available;};
    const SubnormalOracle subnormalOracles[]={
        {3ULL,0x0.0000000000003p-1022,0x0.0p+0,0x0.0000000000002p-1022,0x0.0p+0,false},
        {3ULL,0x0.0000000000009p-1022,0x0.0p+0,0x0.0000000000004p-1022,0x0.0000000000002p-1022,true},
        {3ULL,0x0.000000000000fp-1022,0x0.0p+0,0x0.0000000000008p-1022,0x0.0000000000002p-1022,true},
        {3ULL,0x0.0000000000015p-1022,0x0.0p+0,0x0.000000000000ap-1022,0x0.0000000000004p-1022,true},
        {4ULL,0x0.0000000000006p-1022,0x0.0p+0,0x0.0000000000002p-1022,0x0.0p+0,false},
        {4ULL,0x0.0000000000012p-1022,0x0.0p+0,0x0.0000000000006p-1022,0x0.0000000000002p-1022,true},
        {4ULL,0x0.000000000001ep-1022,0x0.0p+0,0x0.000000000000ap-1022,0x0.0000000000002p-1022,true},
        {4ULL,0x0.000000000002ap-1022,0x0.0p+0,0x0.000000000000ep-1022,0x0.0000000000004p-1022,true},
        {5ULL,0x0.000000000000ap-1022,0x0.0p+0,0x0.0000000000002p-1022,0x0.0p+0,false},
        {5ULL,0x0.000000000001ep-1022,0x0.0p+0,0x0.0000000000008p-1022,0x0.0000000000002p-1022,true},
        {5ULL,0x0.0000000000032p-1022,0x0.0p+0,0x0.000000000000cp-1022,0x0.0000000000002p-1022,true},
        {5ULL,0x0.0000000000046p-1022,0x0.0p+0,0x0.0000000000012p-1022,0x0.0000000000004p-1022,true},
        {6ULL,0x0.000000000000fp-1022,0x0.0p+0,0x0.0000000000003p-1022,0x0.0p+0,false},
        {6ULL,0x0.000000000002dp-1022,0x0.0p+0,0x0.0000000000009p-1022,0x0.0000000000002p-1022,true},
        {6ULL,0x0.000000000004bp-1022,0x0.0p+0,0x0.000000000000fp-1022,0x0.0000000000002p-1022,true},
        {6ULL,0x0.0000000000069p-1022,0x0.0p+0,0x0.0000000000015p-1022,0x0.0000000000004p-1022,true},
        {7ULL,0x0.0000000000015p-1022,0x0.0p+0,0x0.0000000000004p-1022,0x0.0p+0,false},
        {7ULL,0x0.000000000003fp-1022,0x0.0p+0,0x0.000000000000ap-1022,0x0.0000000000002p-1022,true},
        {7ULL,0x0.0000000000069p-1022,0x0.0p+0,0x0.0000000000012p-1022,0x0.0000000000002p-1022,true},
        {7ULL,0x0.0000000000093p-1022,0x0.0p+0,0x0.0000000000018p-1022,0x0.0000000000004p-1022,true},
        {8ULL,0x0.000000000001cp-1022,0x0.0p+0,0x0.0000000000004p-1022,0x0.0p+0,false},
        {8ULL,0x0.0000000000054p-1022,0x0.0p+0,0x0.000000000000cp-1022,0x0.0000000000002p-1022,true},
        {8ULL,0x0.000000000008cp-1022,0x0.0p+0,0x0.0000000000014p-1022,0x0.0000000000002p-1022,true},
        {8ULL,0x0.00000000000c4p-1022,0x0.0p+0,0x0.000000000001cp-1022,0x0.0000000000004p-1022,true},
        {9ULL,0x0.0000000000024p-1022,0x0.0p+0,0x0.0000000000004p-1022,0x0.0p+0,false},
        {9ULL,0x0.000000000006cp-1022,0x0.0p+0,0x0.000000000000ep-1022,0x0.0000000000002p-1022,true},
        {9ULL,0x0.00000000000b4p-1022,0x0.0p+0,0x0.0000000000016p-1022,0x0.0000000000002p-1022,true},
        {9ULL,0x0.00000000000fcp-1022,0x0.0p+0,0x0.0000000000020p-1022,0x0.0000000000004p-1022,true},
        {10ULL,0x0.000000000002dp-1022,0x0.0p+0,0x0.0000000000005p-1022,0x0.0p+0,false},
        {10ULL,0x0.0000000000087p-1022,0x0.0p+0,0x0.000000000000fp-1022,0x0.0000000000002p-1022,true},
        {10ULL,0x0.00000000000e1p-1022,0x0.0p+0,0x0.0000000000019p-1022,0x0.0000000000002p-1022,true},
        {10ULL,0x0.000000000013bp-1022,0x0.0p+0,0x0.0000000000023p-1022,0x0.0000000000004p-1022,true},
        {11ULL,0x0.0000000000037p-1022,0x0.0p+0,0x0.0000000000006p-1022,0x0.0p+0,false},
        {11ULL,0x0.00000000000a5p-1022,0x0.0p+0,0x0.0000000000010p-1022,0x0.0000000000002p-1022,true},
        {11ULL,0x0.0000000000113p-1022,0x0.0p+0,0x0.000000000001cp-1022,0x0.0000000000002p-1022,true},
        {11ULL,0x0.0000000000181p-1022,0x0.0p+0,0x0.0000000000026p-1022,0x0.0000000000004p-1022,true},
        {12ULL,0x0.0000000000042p-1022,0x0.0p+0,0x0.0000000000006p-1022,0x0.0p+0,false},
        {12ULL,0x0.00000000000c6p-1022,0x0.0p+0,0x0.0000000000012p-1022,0x0.0000000000002p-1022,true},
        {12ULL,0x0.000000000014ap-1022,0x0.0p+0,0x0.000000000001ep-1022,0x0.0000000000002p-1022,true},
        {12ULL,0x0.00000000001cep-1022,0x0.0p+0,0x0.000000000002ap-1022,0x0.0000000000004p-1022,true},
        {13ULL,0x0.000000000004ep-1022,0x0.0p+0,0x0.0000000000006p-1022,0x0.0p+0,false},
        {13ULL,0x0.00000000000eap-1022,0x0.0p+0,0x0.0000000000014p-1022,0x0.0000000000002p-1022,true},
        {13ULL,0x0.0000000000186p-1022,0x0.0p+0,0x0.0000000000020p-1022,0x0.0000000000002p-1022,true},
        {13ULL,0x0.0000000000222p-1022,0x0.0p+0,0x0.000000000002ep-1022,0x0.0000000000004p-1022,true},
        {14ULL,0x0.000000000005bp-1022,0x0.0p+0,0x0.0000000000007p-1022,0x0.0p+0,false},
        {14ULL,0x0.0000000000111p-1022,0x0.0p+0,0x0.0000000000015p-1022,0x0.0000000000002p-1022,true},
        {14ULL,0x0.00000000001c7p-1022,0x0.0p+0,0x0.0000000000023p-1022,0x0.0000000000002p-1022,true},
        {14ULL,0x0.000000000027dp-1022,0x0.0p+0,0x0.0000000000031p-1022,0x0.0000000000004p-1022,true},
        {15ULL,0x0.0000000000069p-1022,0x0.0p+0,0x0.0000000000008p-1022,0x0.0p+0,false},
        {15ULL,0x0.000000000013bp-1022,0x0.0p+0,0x0.0000000000016p-1022,0x0.0000000000002p-1022,true},
        {15ULL,0x0.000000000020dp-1022,0x0.0p+0,0x0.0000000000026p-1022,0x0.0000000000002p-1022,true},
        {15ULL,0x0.00000000002dfp-1022,0x0.0p+0,0x0.0000000000034p-1022,0x0.0000000000004p-1022,true},
        {16ULL,0x0.0000000000078p-1022,0x0.0p+0,0x0.0000000000008p-1022,0x0.0p+0,false},
        {16ULL,0x0.0000000000168p-1022,0x0.0p+0,0x0.0000000000018p-1022,0x0.0000000000002p-1022,true},
        {16ULL,0x0.0000000000258p-1022,0x0.0p+0,0x0.0000000000028p-1022,0x0.0000000000002p-1022,true},
        {16ULL,0x0.0000000000348p-1022,0x0.0p+0,0x0.0000000000038p-1022,0x0.0000000000004p-1022,true},
        {17ULL,0x0.0000000000088p-1022,0x0.0p+0,0x0.0000000000008p-1022,0x0.0p+0,false},
        {17ULL,0x0.0000000000198p-1022,0x0.0p+0,0x0.000000000001ap-1022,0x0.0000000000002p-1022,true},
        {17ULL,0x0.00000000002a8p-1022,0x0.0p+0,0x0.000000000002ap-1022,0x0.0000000000002p-1022,true},
        {17ULL,0x0.00000000003b8p-1022,0x0.0p+0,0x0.000000000003cp-1022,0x0.0000000000004p-1022,true},
        {257ULL,0x0.0000000008080p-1022,0x0.0p+0,0x0.0000000000080p-1022,0x0.0p+0,false},
        {257ULL,0x0.0000000018180p-1022,0x0.0p+0,0x0.0000000000182p-1022,0x0.0000000000002p-1022,true},
        {257ULL,0x0.0000000028280p-1022,0x0.0p+0,0x0.0000000000282p-1022,0x0.0000000000002p-1022,true},
        {257ULL,0x0.0000000038380p-1022,0x0.0p+0,0x0.0000000000384p-1022,0x0.0000000000004p-1022,true},
        {1025ULL,0x0.0000000080200p-1022,0x0.0p+0,0x0.0000000000200p-1022,0x0.0p+0,false},
        {1025ULL,0x0.0000000180600p-1022,0x0.0p+0,0x0.0000000000602p-1022,0x0.0000000000002p-1022,true},
        {1025ULL,0x0.0000000280a00p-1022,0x0.0p+0,0x0.0000000000a02p-1022,0x0.0000000000002p-1022,true},
        {1025ULL,0x0.0000000380e00p-1022,0x0.0p+0,0x0.0000000000e04p-1022,0x0.0000000000004p-1022,true},
        {9007199254740992ULL,0x1.fffffffffffffp-970,0x0.0p+0,0x1.0000000000000p-1022,0x0.0p+0,false},
        {9007199254740992ULL,0x1.7ffffffffffffp-968,0x1.0000000000000p-1022,0x1.8000000000000p-1021,0x0.0000000000002p-1022,true},
        {9007199254740992ULL,0x1.3ffffffffffffp-967,0x1.8000000000000p-1021,0x1.4000000000000p-1020,0x0.0000000000002p-1022,true},
        {9007199254740992ULL,0x1.bffffffffffffp-967,0x1.0000000000000p-1022,0x1.c000000000000p-1020,0x0.0000000000004p-1022,true}
    };
    for(const auto& oracle:subnormalOracles) {
        RadiometricPixel pixel; pixel.count=pixel.nextSampleIndex=oracle.n; pixel.mean=1;
        pixel.m2=oracle.m2; pixel.m2Correction=oracle.correction;
        RadiometricStatistics actual; actual.meanVariance=-1;
        const bool resolved=RadiometricPixelStatistics(pixel,actual,error);
        Check(resolved==oracle.available && (!resolved ? actual.meanVariance==-1 :
            actual.varianceAvailable && SameDoubleBits(actual.sampleVariance,oracle.sampleVariance) && SameDoubleBits(actual.meanVariance,oracle.meanVariance)),
            "subnormal-resolve exact rational half ties resolve sample and mean variance independently");
        if(oracle.n<9007199254740992ULL) {
            auto identity=Identity(1,1,oracle.n+1); auto snapshot=RadiometricFilm::Create(identity,error)->Snapshot(); snapshot.pixels[0]=pixel;
            Check(bool(RadiometricFilm::Restore(snapshot,error))==oracle.available,
                "subnormal-resolve restored state refuses only required rounded-zero variance");
        }
    }
    // Fraction-generated full-range represented moments, including maximal
    // component exponent separation and exact 106-bit count products.
    struct ResolveOracle {std::uint64_t n; double high,low,sample,mean; bool available;};
#include "RadiometricResolveOracles.h"
    for(const auto& oracle:resolveOracles) {
        RadiometricPixel pixel; pixel.count=pixel.nextSampleIndex=oracle.n; pixel.mean=1;
        pixel.m2=oracle.high; pixel.m2Correction=oracle.low;
        RadiometricStatistics actual; actual.meanVariance=-1;
        const bool available=RadiometricPixelStatistics(pixel,actual,error);
        Check(available==oracle.available && (!available ? actual.meanVariance==-1 :
            SameDoubleBits(actual.sampleVariance,oracle.sample) && SameDoubleBits(actual.meanVariance,oracle.mean)),
            "subnormal-resolve full-range exact Fraction oracle for represented compensated M2");
    }
    // The reviewer's live path reaches a stored M2 of nine minimum units.
    const double subnormalLive=std::ldexp(std::nextafter(std::sqrt(18.0),20.0),-537);
    const auto subnormalIdentity=Identity(1,1,3);
    auto liveSubnormal=RadiometricFilm::Create(subnormalIdentity,error);
    Check(liveSubnormal->Commit(0,0,0,0,error) && liveSubnormal->Commit(0,0,1,subnormalLive,error),"subnormal-resolve live first pair accepted");
    liveSubnormal->Cancel(); Bytes subnormalBytes; RadiometricSnapshot subnormalState;
    bool subnormalResume=EncodeRadiometricCheckpoint(liveSubnormal->Snapshot(),subnormalBytes,error) && DecodeRadiometricCheckpoint(subnormalBytes,subnormalIdentity,subnormalState,error);
    auto subnormalResumed=subnormalResume ? RadiometricFilm::Restore(subnormalState,error) : nullptr;
    subnormalResume=subnormalResume && subnormalResumed && subnormalResumed->Resume(subnormalIdentity,error) && subnormalResumed->Commit(0,0,2,subnormalLive/2,error);
    RadiometricStatistics subnormalStatistics;
    Check(subnormalResume && RadiometricPixelStatistics(subnormalResumed->Snapshot().pixels[0],subnormalStatistics,error) &&
        SameDoubleBits(subnormalResumed->Snapshot().pixels[0].m2,0x0.0000000000009p-1022) &&
        SameDoubleBits(subnormalStatistics.sampleVariance,0x0.0000000000004p-1022) &&
        SameDoubleBits(subnormalStatistics.meanVariance,0x0.0000000000002p-1022),
        "subnormal-resolve live/resumed nine-unit M2 has independently rounded four/two-unit variances");
    // Exact Fraction squares of these binary64 inputs lie on opposite sides
    // of physical half-subnormal moment boundaries; low parts decide rounding.
    const double scaleInputs[]={0x1.a887293fd6f34p+1,0x1.07e0f66afed07p+2};
    const double scaleExpected[]={0x0.0000000000005p-1022,0x0.0000000000009p-1022};
    for(unsigned int i=0;i<2;++i) {
        auto film=RadiometricFilm::Create(Identity(1,1,2),error);
        Check(film->Commit(0,0,0,0,error) && film->Commit(0,0,1,std::ldexp(scaleInputs[i],-537),error) &&
            SameDoubleBits(film->Snapshot().pixels[0].m2,scaleExpected[i]),
            "subnormal-resolve final physical M2 scale rounds combined high/low once");
    }
    // Failed samples never change count/index and cannot be resumed/serialized.
    for(double bad:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        auto invalid=RadiometricFilm::Create(id,error); Check(!invalid->Commit(0,0,0,bad,error),"invalid trial refused"); auto s=invalid->Snapshot(); Bytes bytes;
        Check(s.status==RadiometricStatus::Invalid && !s.failure.empty() && s.pixels[0].count==0 && s.pixels[0].nextSampleIndex==0 && !EncodeRadiometricCheckpoint(s,bytes,error) && !invalid->Resume(id,error),"invalid measurement cannot skip sample or serialize/resume");
    }
    auto overflow=RadiometricFilm::Create(id,error); Check(overflow->Commit(0,0,0,0,error) && !overflow->Commit(0,0,1,std::numeric_limits<double>::max(),error) && overflow->Snapshot().pixels[0].count==1,"M2 overflow invalidates without partial count/index commit");
    auto tiny=RadiometricFilm::Create(id,error); Check(tiny->Commit(0,0,0,0,error) && !tiny->Commit(0,0,1,1e-200,error),"unrepresentable positive M2 is refused rather than zeroed");
    auto wrongIndex=RadiometricFilm::Create(id,error); Check(!wrongIndex->Commit(0,0,1,4,error) && wrongIndex->Snapshot().status==RadiometricStatus::Invalid,"sample-index mismatch invalidates");
    auto outside=RadiometricFilm::Create(id,error); Check(!outside->Commit(1,0,0,4,error),"pixel bounds refusal");
    RadiometricLimits small; small.maxPixelBytes=31; Check(!RadiometricFilm::Create(id,error,small),"film allocation budget enforced before allocation");
    auto absurd=id; absurd.width=std::numeric_limits<std::uint32_t>::max(); absurd.height=absurd.width; Check(!RadiometricFilm::Create(absurd,error),"overflowing dimensions refused");
    auto absent=id; absent.provenance.clear(); Check(!RadiometricFilm::Create(absent,error),"provenance is required");
    // Cancellation has no partially accepted trial; codec preserves double state.
    auto partial=RadiometricFilm::Create(id,error); Check(partial->Commit(0,0,0,0,error),"partial initial zero trial"); partial->Cancel();
    Check(!partial->Commit(0,0,1,9,error) && partial->Snapshot().status==RadiometricStatus::Cancelled,"cancel prevents commits without invalidating checkpoint");
    Bytes checkpoint; Check(EncodeRadiometricCheckpoint(partial->Snapshot(),checkpoint,error),"encode genuine cancelled checkpoint");
    RadiometricSnapshot decoded; Check(DecodeRadiometricCheckpoint(checkpoint,id,decoded,error),"decode cancelled checkpoint");
    Bytes again; Check(EncodeRadiometricCheckpoint(decoded,again,error) && again==checkpoint,"checkpoint byte-for-byte roundtrip");
    auto resumed=RadiometricFilm::Restore(decoded,error); Check(bool(resumed) && resumed->Resume(id,error),"restore and explicitly resume exact identity");
    Check(resumed->Commit(0,0,1,2,error) && resumed->Commit(0,0,2,4,error),"resumed trial indices continue");
    Bytes a,b; Check(EncodeRadiometricCheckpoint(complete,a,error) && EncodeRadiometricCheckpoint(resumed->Snapshot(),b,error) && a==b,"cancel/resume matches uninterrupted double-state bytes");
    // Nonintegral nonzero moments must survive the actual codec bit-for-bit.
    const auto fractionalIdentity=Identity(1,1,5); const double fractions[]={0.1,0.7,1.3,0.2,0.9};
    auto fractional=RadiometricFilm::Create(fractionalIdentity,error);
    for(std::uint64_t i=0;i<3;++i) Check(fractional->Commit(0,0,i,fractions[i],error),"fractional checkpoint trial accepted");
    fractional->Cancel(); const auto fractionalBefore=fractional->Snapshot(); Bytes fractionalBytes;
    RadiometricSnapshot fractionalAfter;
    Check(EncodeRadiometricCheckpoint(fractionalBefore,fractionalBytes,error) && DecodeRadiometricCheckpoint(fractionalBytes,fractionalIdentity,fractionalAfter,error) &&
        fractionalBefore.pixels[0].mean>0 && fractionalBefore.pixels[0].m2>0 && fractionalBefore.pixels[0].mean!=std::floor(fractionalBefore.pixels[0].mean) &&
        fractionalBefore.pixels[0].m2!=std::floor(fractionalBefore.pixels[0].m2) &&
        SameDoubleBits(fractionalBefore.pixels[0].mean,fractionalAfter.pixels[0].mean) && SameDoubleBits(fractionalBefore.pixels[0].m2,fractionalAfter.pixels[0].m2) &&
        SameDoubleBits(fractionalBefore.pixels[0].meanCorrection,fractionalAfter.pixels[0].meanCorrection) && SameDoubleBits(fractionalBefore.pixels[0].m2Correction,fractionalAfter.pixels[0].m2Correction),
        "nonintegral nonzero mean/M2 checkpoint roundtrip preserves exact float64 bits");
    auto fractionalResumed=RadiometricFilm::Restore(fractionalAfter,error);
    auto fractionalContinuous=RadiometricFilm::Create(fractionalIdentity,error);
    bool fractionResumeOkay=bool(fractionalResumed) && fractionalResumed->Resume(fractionalIdentity,error);
    for(std::uint64_t i=0;i<5;++i) {
        fractionResumeOkay=fractionResumeOkay && fractionalContinuous->Commit(0,0,i,fractions[i],error);
        if(i>=3 && fractionalResumed) fractionResumeOkay=fractionResumeOkay && fractionalResumed->Commit(0,0,i,fractions[i],error);
    }
    Bytes fractionContinuousBytes,fractionResumedBytes;
    Check(fractionResumeOkay && EncodeRadiometricCheckpoint(fractionalContinuous->Snapshot(),fractionContinuousBytes,error) &&
        EncodeRadiometricCheckpoint(fractionalResumed->Snapshot(),fractionResumedBytes,error) && fractionContinuousBytes==fractionResumedBytes,
        "nonintegral compensated mean checkpoint resume matches uninterrupted exact bytes");
    const auto adjacentIdentity=Identity(1,1,3);
    auto adjacentPartial=RadiometricFilm::Create(adjacentIdentity,error);
    Check(adjacentPartial->Commit(0,0,0,0x1.0000000000001p+0,error) && adjacentPartial->Commit(0,0,1,0x1.0000000000002p+0,error),"adjacent-variance checkpoint first pair accepted");
    adjacentPartial->Cancel(); Bytes adjacentBytes; RadiometricSnapshot adjacentRestored;
    bool adjacentResumeOkay=EncodeRadiometricCheckpoint(adjacentPartial->Snapshot(),adjacentBytes,error) && DecodeRadiometricCheckpoint(adjacentBytes,adjacentIdentity,adjacentRestored,error);
    auto adjacentResumed=adjacentResumeOkay ? RadiometricFilm::Restore(adjacentRestored,error) : nullptr;
    adjacentResumeOkay=adjacentResumeOkay && bool(adjacentResumed) && adjacentResumed->Resume(adjacentIdentity,error) && adjacentResumed->Commit(0,0,2,0x1.0000000000001p+0,error);
    Check(adjacentResumeOkay && SameDoubleBits(adjacentResumed->Snapshot().pixels[0].m2,std::ldexp(2.0/3.0,-104)),
        "adjacent-variance checkpoint preserves sub-ULP mean state for third sample");
    // Expected run identity, not self-authored checkpoint, is authoritative.
    for(unsigned int field=0;field<7;++field) { auto changed=id; if(field==0) ++changed.seed; if(field==1) changed.wavelengthNM=365; if(field==2) ++changed.targetSamples; if(field==3) changed.seedAlgorithm="changed"; if(field==4) changed.width=2; if(field==6) changed.pixelAspectRatio=2; if(field==5) Check(RISECBOR64::Encode(Value::MapValue({{"changed",Value::Bool(true)}}),changed.provenance,&error),"changed provenance encoded");
        RadiometricSnapshot untouched=complete; Check(!DecodeRadiometricCheckpoint(checkpoint,changed,untouched,error) && untouched.status==RadiometricStatus::Complete && untouched.pixels[0].mean==2,"resume mismatch refuses before mutating output"); }
    auto corrupt=checkpoint; corrupt[corrupt.size()/2]^=1; Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"checkpoint digest detects corruption");
    auto truncated=checkpoint; truncated.pop_back(); Check(!DecodeRadiometricCheckpoint(truncated,id,decoded,error),"truncated checkpoint refused");
    auto extra=checkpoint; extra.push_back(0); Rehash(extra); Check(!DecodeRadiometricCheckpoint(extra,id,decoded,error),"extra checkpoint bytes refused even with updated digest");
    Check(!DecodeRadiometricCheckpoint(EditHeader(checkpoint,"schema_version",Value::Unsigned(1)),id,decoded,error),"resigned unknown schema refused");
    Check(!DecodeRadiometricCheckpoint(EditHeader(checkpoint,"extra",Value::Bool(true)),id,decoded,error),"resigned extra schema key refused");
    Check(!DecodeRadiometricCheckpoint(EditHeader(checkpoint,"pixel_count",Value::Unsigned(std::numeric_limits<std::uint64_t>::max())),id,decoded,error),"resigned huge pixel count refused before allocation");
    Check(!DecodeRadiometricCheckpoint(EditHeader(checkpoint,"status",Value::String("complete")),id,decoded,error),"resigned false completion refused");
    const auto pixelOffset=24+static_cast<std::size_t>(Get64(checkpoint.data()+16));
    corrupt=checkpoint; Set64(corrupt,pixelOffset+8,99); Rehash(corrupt); Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"resigned count/index mismatch refused");
    corrupt=checkpoint; SetDouble(corrupt,pixelOffset+16,std::numeric_limits<double>::quiet_NaN()); Rehash(corrupt); Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"resigned NaN pixel refused");
    corrupt=checkpoint; SetDouble(corrupt,pixelOffset+24,-1); Rehash(corrupt); Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"resigned negative M2 refused");
    corrupt=checkpoint; SetDouble(corrupt,pixelOffset+32,std::numeric_limits<double>::quiet_NaN()); Rehash(corrupt);
    Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"re-signed invalid mean correction refused");
    corrupt=checkpoint; SetDouble(corrupt,pixelOffset+40,1); Rehash(corrupt);
    Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"re-signed nonnormalized M2 correction refused");
    for(unsigned char version:{'1','2'}) {
        corrupt=checkpoint; corrupt[13]=version; Rehash(corrupt);
        Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"legacy v1/v2 checkpoint framing refused by v3 codec");
    }
    corrupt=checkpoint; Set64(corrupt,pixelOffset,4); Set64(corrupt,pixelOffset+8,4); Rehash(corrupt);
    Check(!DecodeRadiometricCheckpoint(corrupt,id,decoded,error),"resigned count above fixed sample budget refused");
    // Re-signed bytes still must satisfy the exact-zero invariant of the
    // nonnegative/underflow-refusing accumulator, for either zero sign.
    for(double signedZero:{0.0,-0.0}) {
        corrupt=checkpoint; Set64(corrupt,pixelOffset,2); Set64(corrupt,pixelOffset+8,2);
        SetDouble(corrupt,pixelOffset+16,signedZero); SetDouble(corrupt,pixelOffset+24,2); Rehash(corrupt);
        RadiometricSnapshot preserved=complete;
        Check(!DecodeRadiometricCheckpoint(corrupt,id,preserved,error) && preserved.status==complete.status &&
            preserved.pixels[0].count==3 && preserved.pixels[0].mean==2 && preserved.pixels[0].m2==8,
            "zero-mean/nonzero-M2 re-signed checkpoint refused without output mutation");
        auto forged=partial->Snapshot(); forged.pixels[0].count=forged.pixels[0].nextSampleIndex=2;
        forged.pixels[0].mean=signedZero; forged.pixels[0].m2=2;
        Check(!RadiometricFilm::Restore(forged,error),"zero-mean/nonzero-M2 direct Restore refused");
    }
    auto maxSeed=id; maxSeed.seed=std::numeric_limits<std::uint64_t>::max();
    auto fullWidthSeed=RadiometricFilm::Create(maxSeed,error); Bytes seedBytes;
    Check(bool(fullWidthSeed) && EncodeRadiometricCheckpoint(fullWidthSeed->Snapshot(),seedBytes,error) && DecodeRadiometricCheckpoint(seedBytes,maxSeed,decoded,error) && decoded.identity.seed==maxSeed.seed,"uint64 seed survives without double precision loss");
    small=RadiometricLimits(); small.maxCheckpointBytes=checkpoint.size()-1; Check(!DecodeRadiometricCheckpoint(checkpoint,id,decoded,error,small) && !EncodeRadiometricCheckpoint(partial->Snapshot(),again,error,small),"codec byte budget enforced");
    // Concurrent independent pixels and mutex-protected snapshot semantics.
    auto concurrent=RadiometricFilm::Create(Identity(2,1,32),error);
    std::atomic<bool> workerOkay{true};
    std::thread worker([&] { std::string err; for(std::uint64_t i=0;i<32;++i) if(!concurrent->Commit(0,0,i,2,err)) { workerOkay.store(false); break; } });
    for(std::uint64_t i=0;i<32;++i) {
        Check(concurrent->Commit(1,0,i,4,error),"concurrent second-pixel samples accepted");
        Check(ValidateRadiometricSnapshot(concurrent->Snapshot(),error),"snapshot remains consistent during independent worker commits");
    }
    worker.join();
    Check(workerOkay.load() && concurrent->Snapshot().status==RadiometricStatus::Complete,"parallel distinct-pixel commits complete fixed budget");
    const auto root=fs::temp_directory_path()/fs::path("rise-radiometric-primitives-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root); fs::permissions(root,fs::perms::owner_all);
    const auto checkpointPath=root/"film.checkpoint";
    Check(SaveRadiometricCheckpoint(checkpointPath.string(),partial->Snapshot(),error),"atomic checkpoint publication");
    Check(LoadRadiometricCheckpoint(checkpointPath.string(),id,decoded,error) && decoded.status==RadiometricStatus::Cancelled,"real checkpoint file load");
    const auto oldBytes=Read(checkpointPath); auto invalid=complete; invalid.pixels[0].m2=-1;
    Check(!SaveRadiometricCheckpoint(checkpointPath.string(),invalid,error) && Read(checkpointPath)==oldBytes,"invalid checkpoint cannot replace existing file");
    Check(SaveRadiometricCheckpoint(checkpointPath.string(),complete,error) && LoadRadiometricCheckpoint(checkpointPath.string(),id,decoded,error) && decoded.status==RadiometricStatus::Complete,"atomic valid replacement loads complete checkpoint");
    const auto exrPath=root/"plane.exr";
#ifndef NO_EXR_SUPPORT
    auto planeIdentity=Identity(3,2,1); planeIdentity.pixelAspectRatio=1.5;
    auto plane=RadiometricFilm::Create(planeIdentity,error);
    const double values[]={0,2.5,70000,1e10,static_cast<double>(std::numeric_limits<float>::max()),static_cast<double>(std::numeric_limits<float>::denorm_min())};
    for(std::uint32_t y=0;y<2;++y) for(std::uint32_t x=0;x<3;++x) Check(plane->Commit(x,y,0,values[y*3+x],error),"absolute FLOAT fixture sample accepted");
    Check(WriteRadiometricEXR(exrPath.string(),plane->Snapshot(),error),"atomic complete raw scalar EXR publication");
    try {
        Imf::InputFile input(exrPath.string().c_str()); const auto& h=input.header();
        const auto* channel=h.channels().findChannel("L_lambda"); std::size_t channels=0; for(auto it=h.channels().begin();it!=h.channels().end();++it) ++channels;
        Check(channel && channel->type==Imf::FLOAT && channels==1 && h.find("chromaticities")==h.end() && h.channels().findChannel("A")==nullptr && h.compression()==Imf::ZIP_COMPRESSION,"one lossless FLOAT scalar channel with no alpha/chromaticities");
        std::vector<float> pixels(6,-1); Imf::FrameBuffer fb; fb.insert("L_lambda",Imf::Slice(Imf::FLOAT,reinterpret_cast<char*>(pixels.data()),4,12)); input.setFrameBuffer(fb); input.readPixels(0,1);
        bool exact=true; for(std::size_t i=0;i<6;++i) {
            const float expected=static_cast<float>(values[i]); std::uint32_t expectedBits=0,actualBits=0;
            std::memcpy(&expectedBits,&expected,4); std::memcpy(&actualBits,&pixels[i],4);
            exact=exact && actualBits==expectedBits;
            if(i==5) exact=exact && actualBits==1u;
        }
        Check(exact,"independent OpenEXR decoder preserves absolute nonnegative FLOAT ordinates including >half and subnormal");
        Check(h.pixelAspectRatio()==1.5f && h.typedAttribute<Imf::DoubleAttribute>("rise.pixel_aspect_ratio").value()==1.5 && h.typedAttribute<Imf::DoubleAttribute>("rise.wavelength_nm").value()==1200 && h.typedAttribute<Imf::StringAttribute>("rise.units").value()=="W m^-2 sr^-1 nm^-1","raw EXR declares fixed wavelength and absolute per-nm units");
    } catch(const std::exception& ex) { std::fprintf(stderr,"EXR exception: %s\n",ex.what()); Check(false,"raw EXR independently decodes"); }
    const auto exrOriginal=Read(exrPath);
    auto tooLarge=RadiometricFilm::Create(Identity(1,1,1),error); Check(tooLarge->Commit(0,0,0,1e100,error),"double mean above FLOAT range remains valid film state");
    Check(!WriteRadiometricEXR(exrPath.string(),tooLarge->Snapshot(),error) && Read(exrPath)==exrOriginal,"FLOAT overflow explicitly refused without clipping or replacing destination");
    auto tooSmall=RadiometricFilm::Create(Identity(1,1,1),error); Check(tooSmall->Commit(0,0,0,1e-100,error),"small double ordinate accepted");
    Check(!WriteRadiometricEXR(exrPath.string(),tooSmall->Snapshot(),error) && Read(exrPath)==exrOriginal,"positive-to-zero FLOAT underflow refused without replacement");
    Check(!WriteRadiometricEXR(exrPath.string(),partial->Snapshot(),error) && Read(exrPath)==exrOriginal,"incomplete measurement cannot publish successful EXR");
    // Float32 midpoint rounding must use the persisted low mean component,
    // including at the half-minimum-FLOAT threshold and exact range maximum.
    auto checkCompensatedFloat=[&](double a,double b,std::uint32_t expected,bool publish) {
        auto sample=RadiometricFilm::Create(Identity(1,1,2),error);
        Check(sample->Commit(0,0,0,a,error) && sample->Commit(0,0,1,b,error) && sample->Snapshot().pixels[0].meanCorrection!=0,
            "compensated-FLOAT adjacent trial mean retains a nonzero rounding residual");
        const auto prior=Read(exrPath);
        const bool written=WriteRadiometricEXR(exrPath.string(),sample->Snapshot(),error);
        if(!publish) { Check(!written && !error.empty() && Read(exrPath)==prior,"compensated-FLOAT true overflow/underflow refuses without replacing destination"); return; }
        Check(written,"compensated-FLOAT representable rounded output publishes");
        if(written) {
            try {
                Imf::InputFile input(exrPath.string().c_str()); float value=0; Imf::FrameBuffer buffer;
                buffer.insert("L_lambda",Imf::Slice(Imf::FLOAT,reinterpret_cast<char*>(&value),sizeof(float),0));
                input.setFrameBuffer(buffer); input.readPixels(0,0); std::uint32_t bits=0; std::memcpy(&bits,&value,4);
                Check(bits==expected,"compensated-FLOAT independent decode selects correct adjacent float bits");
            } catch(const std::exception&) { Check(false,"compensated-FLOAT independent decode succeeds"); }
        }
    };
    const double infinity=std::numeric_limits<double>::infinity();
    checkCompensatedFloat(0x1.000001p0,std::nextafter(0x1.000001p0,infinity),0x3f800001u,true);
    checkCompensatedFloat(0x1.000003p0,std::nextafter(0x1.000003p0,-infinity),0x3f800001u,true);
    checkCompensatedFloat(0x1p-150,std::nextafter(0x1p-150,infinity),1u,true);
    checkCompensatedFloat(0x1p-150,std::nextafter(0x1p-150,-infinity),0u,false);
    const double floatMaximum=static_cast<double>(std::numeric_limits<float>::max());
    checkCompensatedFloat(floatMaximum,std::nextafter(floatMaximum,infinity),0u,false);
    checkCompensatedFloat(floatMaximum,std::nextafter(floatMaximum,-infinity),0x7f7fffffu,true);

#else
    Check(!WriteRadiometricEXR(exrPath.string(),complete,error) && error.find("NO_EXR_SUPPORT")!=std::string::npos,"NO_EXR_SUPPORT reports honest refusal");
#endif
#ifndef _WIN32
    RadiometricSnapshot large; large.identity=Identity(1024,512,1); large.status=RadiometricStatus::Complete;
    large.pixels.resize(1024u*512u);
    std::uint32_t randomBits=1234567;
    for(auto& pixel:large.pixels) {
        randomBits=randomBits*1664525u+1013904223u;
        pixel.count=pixel.nextSampleIndex=1; pixel.mean=static_cast<double>((randomBits>>8)+1)/1024.0;
    }
    LatePublicationFailure(root/"late-checkpoint",[&](const std::string& path,std::string& diagnostic,const std::function<void()>& observer) {
        return RadiometricDetail::SaveCheckpointObserved(path,large,diagnostic,{},observer);
    },[&](const fs::path& recovery) {
        RadiometricSnapshot restored;
        Check(LoadRadiometricCheckpoint(recovery.string(),large.identity,restored,error) &&
            restored.status==RadiometricStatus::Complete && restored.pixels.size()==large.pixels.size() &&
            restored.pixels.front().mean==large.pixels.front().mean && restored.pixels.back().mean==large.pixels.back().mean,
            "recovery path contains useful complete checkpoint bytes");
    });
#ifndef NO_EXR_SUPPORT
    LatePublicationFailure(root/"late-exr",[&](const std::string& path,std::string& diagnostic,const std::function<void()>& observer) {
        return RadiometricDetail::WriteEXRObserved(path,large,diagnostic,{},observer);
    },[&](const fs::path& recovery) {
        try {
            Imf::InputFile input(recovery.string().c_str()); std::vector<float> row(large.identity.width);
            Imf::FrameBuffer framebuffer; framebuffer.insert("L_lambda",Imf::Slice(Imf::FLOAT,reinterpret_cast<char*>(row.data()),sizeof(float),0));
            input.setFrameBuffer(framebuffer); input.readPixels(511,511);
            bool exact=true;
            for(std::size_t i=0;i<row.size();++i) {
                const float expected=static_cast<float>(large.pixels[511u*1024u+i].mean);
                std::uint32_t wanted=0,actual=0; std::memcpy(&wanted,&expected,4); std::memcpy(&actual,&row[i],4);
                exact=exact && wanted==actual;
            }
            Check(exact,"recovery path contains useful complete EXR bytes with exact final row");
        } catch(const std::exception&) { Check(false,"recovery path contains useful complete EXR bytes"); }
    });
#endif
#endif
    Check(!SaveRadiometricCheckpoint((root/"missing"/"fail").string(),complete,error),"missing-parent IO failure is explicit");
    std::size_t files=0; for(const auto& item:fs::directory_iterator(root)) { ++files; Check(!item.is_directory(),"publication leaves no staging directory"); }
#ifndef NO_EXR_SUPPORT
    Check(files==2,"only checkpoint and raw plane artifacts remain");
#else
    Check(files==1,"only checkpoint artifact remains without EXR");
#endif
    fs::remove_all(root);
    std::printf("%d checks; %d failures\n",checks,failures); return failures ? 1 : 0;
}
