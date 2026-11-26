#include "pch.h"
#include "HyLIoSLocalResponseAsset.h"
#include "Utilities/RISECBOR64.h"
#include "Utilities/DiskFileWriteBuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <limits>
#include <stdexcept>
#include <utility>

namespace RISE::HyLIoS {
namespace {
using V = RISECBOR64::Value;
using Bytes = RISECBOR64::Bytes;
constexpr std::size_t HeaderBytes = 80, DigestBytes = 32;
constexpr unsigned char Magic[8] = {'H','Y','L','O','C','A','L',0};
constexpr const char* Format = "hylios_local_asset_v1";
constexpr const char* Encoding = "positive_float32_downward_le_v1";
constexpr const char* GridRule = "full_independent_projected_cells_average_v1";
constexpr const char* GuideRule = "normal_incidence_separate_experiment_v1";
constexpr const char* RNG = "splitmix64_index_top53_v1";
constexpr const char* Interpolation = "nonnegative_linear_physical_nm_v1";
constexpr std::size_t MaxRecordBytes = 4096;

[[noreturn]] void Fail(const std::string& message)
{ throw std::invalid_argument("HyLIoS local asset: " + message); }
std::uint64_t Add(std::uint64_t a, std::uint64_t b)
{
    if (b > std::numeric_limits<std::uint64_t>::max() - a) Fail("size addition overflow");
    return a + b;
}
std::uint64_t Mul(std::uint64_t a, std::uint64_t b)
{
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a) Fail("size multiplication overflow");
    return a * b;
}
std::size_t Size(std::uint64_t n)
{
    if (n > std::numeric_limits<std::size_t>::max()) Fail("size exceeds platform size_t");
    return static_cast<std::size_t>(n);
}
void Put32(Bytes& b, std::uint32_t n)
{ for (unsigned i=0;i<4;++i) b.push_back(static_cast<unsigned char>(n >> (8*i))); }
void Put64(Bytes& b, std::uint64_t n)
{ for (unsigned i=0;i<8;++i) b.push_back(static_cast<unsigned char>(n >> (8*i))); }
std::uint64_t Get64(const unsigned char* p)
{
    std::uint64_t n=0; for (unsigned i=0;i<8;++i) n |= std::uint64_t{p[i]} << (8*i); return n;
}
std::uint32_t Get32(const unsigned char* p)
{
    std::uint32_t n=0; for (unsigned i=0;i<4;++i) n |= std::uint32_t{p[i]} << (8*i); return n;
}

// This allocation-free syntax walk precedes the generic decoder. The explicit
// depth/token caps also bound the latter's recursive Value copies. Canonical
// token widths, UTF8/NFC, ordering, duplicate keys, floats and trailing bytes are
// subsequently checked by RISE-CBOR64; this pass does not normalize anything.
#ifdef _WIN32
#ifdef _MSC_VER
using NativeFileStatus = struct __stat64;
#else
using NativeFileStatus = struct _stat64;
#endif
#endif

// POSIX opens nonblocking to avoid an open-time FIFO hang during path races.
// Both platforms fstat the SAME opened descriptor and refuse nonregular files
// before reading. Windows converts the UTF8 pathname to the wide CRT API.
class RegularInput final {
public:
    explicit RegularInput(const std::string& path)
    {
#ifdef _WIN32
        const int fd=_wopen(std::filesystem::u8path(path).wstring().c_str(),_O_RDONLY|_O_BINARY|_O_NOINHERIT);
        if(fd<0) Fail("cannot open file: "+path);
        NativeFileStatus status{};
        if(_fstat64(fd,&status)!=0 || (status.st_mode&_S_IFMT)!=_S_IFREG || status.st_size<0) {
            _close(fd); Fail("input must be a regular file");
        }
        file_=_fdopen(fd,"rb");
        if(!file_) { _close(fd); Fail("cannot adopt input file"); }
#else
        const int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if(fd<0) Fail("cannot open file: "+path);
        struct stat status{};
        if(fstat(fd,&status)!=0 || !S_ISREG(status.st_mode) || status.st_size<0) {
            close(fd); Fail("input must be a regular file");
        }
        file_=fdopen(fd,"rb");
        if(!file_) { close(fd); Fail("cannot adopt input file"); }
#endif
        size_=static_cast<std::uint64_t>(status.st_size);
    }
    ~RegularInput() { if(file_) std::fclose(file_); }
    RegularInput(const RegularInput&)=delete;
    RegularInput& operator=(const RegularInput&)=delete;
    std::uint64_t SizeBytes() const noexcept { return size_; }
    void Read(unsigned char* bytes,std::size_t n)
    { if(n && std::fread(bytes,1,n,file_)!=n) Fail("truncated/unreadable file bytes"); }
    void RequireEnd()
    { if(std::fgetc(file_)!=EOF || std::ferror(file_)) Fail("changed/extra/unreadable file bytes"); }
private:
    std::FILE* file_=nullptr;
    std::uint64_t size_=0;
};

struct Scan {
    const unsigned char* p; std::size_t n, at=0, tokens=0;
    void Value(unsigned depth)
    {
        if (depth>8 || ++tokens>8192 || at==n) Fail("metadata nesting/token bound");
        unsigned first=p[at++], major=first>>5, ai=first&31;
        std::uint64_t argument=ai;
        if (ai>=24) {
            unsigned width=ai==24?1:ai==25?2:ai==26?4:ai==27?8:0;
            if (!width || width>n-at) Fail("metadata token truncation");
            argument=0; for(unsigned i=0;i<width;++i) argument=(argument<<8)|p[at++];
        }
        if (major==2 || major==3) {
            if(argument>n-at) Fail("metadata string extent"); at+=Size(argument);
        } else if (major==4 || major==5) {
            const auto count=major==5?Mul(argument,2):argument;
            if(count>n-at || count>8192-tokens) Fail("metadata container bound");
            for(std::uint64_t i=0;i<count;++i) Value(depth+1);
        } else if(major!=0 && major!=1 && major!=7) Fail("metadata unsupported token");
    }
};
V Decode(const Bytes& bytes)
{
    Scan scan{bytes.data(),bytes.size()}; scan.Value(0);
    if(scan.at!=bytes.size()) Fail("metadata trailing bytes");
    V v; std::string error;
    if(!RISECBOR64::DecodeCanonical(bytes,v,&error)) Fail(error);
    return v;
}
Bytes Encode(const V& value)
{
    Bytes bytes; std::string error;
    if(!RISECBOR64::Encode(value,bytes,&error)) Fail(error);
    return bytes;
}
void Keys(const V& value, std::initializer_list<const char*> keys)
{
    if(value.GetType()!=V::Map || value.GetMap().size()!=keys.size()) Fail("closed metadata key set mismatch");
    for(const auto* key:keys) if(!value.Find(key)) Fail(std::string("missing metadata field ")+key);
}
const V& Field(const V& value,const char* key,V::Type type)
{
    const V* field=value.Find(key);
    if(!field || field->GetType()!=type) Fail(std::string("wrong metadata field type: ")+key);
    return *field;
}
std::string Text(const V& value,const char* key)
{
    auto text=Field(value,key,V::Text).GetText();
    if(text.empty() || text.size()>256) Fail(std::string("empty/long revision: ")+key);
    return text;
}
void Literal(const V& value,const char* key,const char* expected)
{ if(Text(value,key)!=expected) Fail(std::string("unsupported ")+key); }
double Number(const V& value,const char* key)
{
    double x=Field(value,key,V::Float64).GetFloat();
    if(!std::isfinite(x)) Fail(std::string("nonfinite ")+key); return x;
}
void Record(const Bytes& bytes,const char* field)
{
    if(bytes.empty() || bytes.size()>MaxRecordBytes) Fail(std::string("record byte bound: ")+field);
    V v=Decode(bytes);
    if(v.GetType()!=V::Map || v.GetMap().empty()) Fail(std::string("record must be nonempty map: ")+field);
}
V RecordValue(const Bytes& bytes,const char* field)
{ Record(bytes,field); return V::BytesValue(bytes); }
Bytes RecordField(const V& v,const char* key)
{ auto bytes=Field(v,key,V::ByteString).GetBytes(); Record(bytes,key); return bytes; }
V Array(const std::vector<double>& input)
{
    V::Values values; values.reserve(input.size());
    for(double x:input) { if(!std::isfinite(x)) Fail("nonfinite grid/node"); values.push_back(V::Float(x)); }
    return V::ArrayValue(values);
}
std::vector<double> DoubleArray(const V& v,const char* key,std::size_t count)
{
    const auto& array=Field(v,key,V::Array).GetArray();
    if(array.size()!=count) Fail(std::string("header/metadata cardinality mismatch: ")+key);
    std::vector<double> result; result.reserve(count);
    for(const auto& x:array) {
        if(x.GetType()!=V::Float64 || !std::isfinite(x.GetFloat())) Fail("invalid grid/node scalar");
        result.push_back(x.GetFloat());
    }
    return result;
}
V Semantic(const LocalAssetSemanticIdentity& s)
{
    if(!std::isfinite(s.coveredFirstNm) || !std::isfinite(s.coveredLastNm) ||
       !(s.coveredFirstNm>0) || s.coveredLastNm<s.coveredFirstNm) Fail("invalid covered wavelength interval");
    return V::MapValue({{"schema",V::String("caller_prepared_hylios_semantics_v1")},
        {"model",RecordValue(s.model,"model")}, {"placement",RecordValue(s.placement,"placement")},
        {"postscatter",RecordValue(s.postscatter,"postscatter")}, {"boundary",RecordValue(s.boundary,"boundary")},
        {"lineage",RecordValue(s.lineage,"lineage")}, {"data",RecordValue(s.data,"data")},
        {"specimen",RecordValue(s.specimen,"specimen")}, {"exterior",RecordValue(s.exterior,"exterior")},
        {"approximation",RecordValue(s.approximation,"approximation")},
        {"covered_first_nm",V::Float(s.coveredFirstNm)}, {"covered_last_nm",V::Float(s.coveredLastNm)}});
}
LocalAssetSemanticIdentity ReadSemantic(const V& v)
{
    Keys(v,{"schema","model","placement","postscatter","boundary","lineage","data","specimen","exterior",
        "approximation","covered_first_nm","covered_last_nm"});
    Literal(v,"schema","caller_prepared_hylios_semantics_v1");
    LocalAssetSemanticIdentity s;
    s.model=RecordField(v,"model"); s.placement=RecordField(v,"placement");
    s.postscatter=RecordField(v,"postscatter"); s.boundary=RecordField(v,"boundary");
    s.lineage=RecordField(v,"lineage"); s.data=RecordField(v,"data");
    s.specimen=RecordField(v,"specimen"); s.exterior=RecordField(v,"exterior");
    s.approximation=RecordField(v,"approximation");
    s.coveredFirstNm=Number(v,"covered_first_nm"); s.coveredLastNm=Number(v,"covered_last_nm");
    Semantic(s); return s;
}
void NonemptyRevision(const std::string& s,const char* field)
{ if(s.empty() || s.size()>256) Fail(std::string("empty/long revision: ")+field); }
V Production(const LocalAssetProductionIdentity& p)
{
    NonemptyRevision(p.oracleRevision,"oracle"); NonemptyRevision(p.lineageRevision,"lineage");
    NonemptyRevision(p.estimatorRevision,"estimator"); NonemptyRevision(p.historyMappingRevision,"history_mapping");
    NonemptyRevision(p.statisticsRevision,"statistics_revision");
    if(!p.historiesPerCellPerReplicate || !p.replicates || !p.normalGuideHistoriesPerReplicate ||
       !p.maximumWorkPerHistory) Fail("production work/count declaration must be positive");
    return V::MapValue({{"oracle",V::String(p.oracleRevision)}, {"lineage",V::String(p.lineageRevision)},
        {"estimator",V::String(p.estimatorRevision)}, {"rng",V::String(RNG)}, {"seed",V::Unsigned(p.seed)},
        {"history_mapping",V::String(p.historyMappingRevision)}, {"histories",V::Unsigned(p.historiesPerCellPerReplicate)},
        {"replicates",V::Unsigned(p.replicates)}, {"guide_histories",V::Unsigned(p.normalGuideHistoriesPerReplicate)},
        {"maximum_work",V::Unsigned(p.maximumWorkPerHistory)}, {"statistics_revision",V::String(p.statisticsRevision)},
        {"build",RecordValue(p.build,"build")}, {"statistics",RecordValue(p.statistics,"statistics")},
        {"encoding",V::String(Encoding)}, {"grid_rule",V::String(GridRule)},
        {"guide_experiment",V::String(GuideRule)}, {"spectral_interpolation",V::String(Interpolation)},
        {"atomic_map",V::String("same_exterior_planar_mirror_v1")},
        {"azimuth",V::String("right_handed_outward_z_phi_atan2_y_x_no_reduction_v1")},
        {"physical_accounting",V::String("producer_declared_complete_no_unresolved_or_unclassified_mass_v1")}});
}
LocalAssetProductionIdentity ReadProduction(const V& v)
{
    Keys(v,{"oracle","lineage","estimator","rng","seed","history_mapping","histories","replicates",
        "guide_histories","maximum_work","statistics_revision","build","statistics","encoding","grid_rule",
        "guide_experiment","spectral_interpolation","atomic_map","azimuth","physical_accounting"});
    Literal(v,"rng",RNG); Literal(v,"encoding",Encoding); Literal(v,"grid_rule",GridRule);
    Literal(v,"guide_experiment",GuideRule); Literal(v,"spectral_interpolation",Interpolation);
    Literal(v,"atomic_map","same_exterior_planar_mirror_v1");
    Literal(v,"azimuth","right_handed_outward_z_phi_atan2_y_x_no_reduction_v1");
    Literal(v,"physical_accounting","producer_declared_complete_no_unresolved_or_unclassified_mass_v1");
    LocalAssetProductionIdentity p;
    p.oracleRevision=Text(v,"oracle"); p.lineageRevision=Text(v,"lineage"); p.estimatorRevision=Text(v,"estimator");
    p.historyMappingRevision=Text(v,"history_mapping"); p.statisticsRevision=Text(v,"statistics_revision");
    p.seed=Field(v,"seed",V::UnsignedInteger).GetIntegerArgument();
    p.historiesPerCellPerReplicate=Field(v,"histories",V::UnsignedInteger).GetIntegerArgument();
    p.replicates=Field(v,"replicates",V::UnsignedInteger).GetIntegerArgument();
    p.normalGuideHistoriesPerReplicate=Field(v,"guide_histories",V::UnsignedInteger).GetIntegerArgument();
    p.maximumWorkPerHistory=Field(v,"maximum_work",V::UnsignedInteger).GetIntegerArgument();
    p.build=RecordField(v,"build"); p.statistics=RecordField(v,"statistics");
    Production(p); return p;
}
struct Dimensions {
    std::array<std::uint64_t,5> counts;
    LocalResponseStorage core;
    std::uint64_t mirror, coefficients, payload;
};
Dimensions Dims(std::array<std::uint64_t,5> n,const LocalAssetLimits& limits)
{
    for(unsigned k=0;k<5;++k) if(n[k]<(k==4?1u:2u) || n[k]>8192) Fail("axis/node cardinality bound");
    // A one-node optical interval is also a valid exact monochromatic asset.
    Dimensions d{n,{},0,0,0};
    d.core=HyLIoSLocalResponse::EstimateStorage(Size(n[4]),Size(n[0]-1),Size(n[1]-1),
        Size(n[2]-1),Size(n[3]-1),limits.response);
    d.mirror=Mul(n[4],d.core.incidentCells);
    d.coefficients=Add(Add(d.core.continuousEntries,d.mirror),n[4]);
    d.payload=Mul(d.coefficients,4);
    return d;
}
void WorkCardinality(const LocalAssetProductionIdentity& p,const Dimensions& d)
{
    const auto rows=Mul(d.counts[4],d.core.incidentCells);
    const auto main=Mul(Mul(rows,p.replicates),p.historiesPerCellPerReplicate);
    const auto guide=Mul(Mul(d.counts[4],p.replicates),p.normalGuideHistoriesPerReplicate);
    // Uniqueness/work cardinality itself is validated; the opaque versioned
    // producer mapping is a claim, not a certification of its implementation.
    Mul(Add(main,guide),p.maximumWorkPerHistory);
}
std::uint64_t Preflight(const Dimensions& d,std::uint64_t metadata,const LocalAssetLimits& limits)
{
    if(!limits.maximumMetadataBytes || limits.maximumMetadataBytes>64*1024 ||
       metadata>limits.maximumMetadataBytes) Fail("metadata exceeds explicit 64KiB profile cap");
    const auto file=Add(Add(HeaderBytes,metadata),Add(d.payload,DigestBytes));
    if(file>limits.maximumFileBytes) Fail("file budget exceeded before payload allocation");
    // Conservative logical peak bound for this codec: 4096 bytes per encoded
    // metadata byte covers capped-depth generic Value trees/copies/records,
    // 2 complete numeric cores plus decoded definition/guide, two wire buffers,
    // grid/node doubles, control blocks and fixed I/O/hash scratch allowance.
    // The numeric core's own logical-array accounting includes its CDF tables.
    auto peak=Add(Mul(metadata,4096),Mul(file,2));
    peak=Add(peak,Mul(d.core.storageBytes,2));
    peak=Add(peak,Mul(d.coefficients,8));
    std::uint64_t axes=0; for(auto n:d.counts) axes=Add(axes,n);
    peak=Add(peak,Mul(axes,32)); peak=Add(peak,1024*1024);
    if(peak>limits.maximumResidentBytes) Fail("resident budget exceeded before allocation");
    Size(file); Size(peak); return peak;
}
void Coverage(const LocalAssetSemanticIdentity& s,const LocalResponseDefinition& d)
{
    if(d.wavelengthNm.empty() || d.wavelengthNm.front()!=s.coveredFirstNm ||
       d.wavelengthNm.back()!=s.coveredLastNm) Fail("covered interval must equal exact wavelength node endpoints");
}
std::string Hex(const unsigned char* data)
{
    constexpr char hex[]="0123456789abcdef"; std::string s; s.reserve(64);
    for(unsigned i=0;i<32;++i) { s.push_back(hex[data[i]>>4]); s.push_back(hex[data[i]&15]); } return s;
}
void Digest(Bytes& b)
{
    const auto hex=RISECBOR64::SHA256Hex(b);
    for(std::size_t i=0;i<64;i+=2) {
        auto value=[](char c) { return static_cast<unsigned>(c>='a'?c-'a'+10:c-'0'); };
        b.push_back(static_cast<unsigned char>((value(hex[i])<<4)|value(hex[i+1])));
    }
}
// IEEE binary32 is a wire encoding, not an arithmetic mode. Integer
// truncation makes the policy independent of active rounding modes and float
// subnormal flushing under shipping fast-math flags.
static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559 &&
    std::numeric_limits<double>::digits==53,"asset format requires IEEE binary64");
double PhysicalBits(std::uint32_t bits)
{
    if((bits&0x80000000u) || (bits&0x7f800000u)==0x7f800000u) Fail("invalid positive float32 coefficient");
    const unsigned exponent=bits>>23;
    const auto mantissa=bits&0x7fffffu;
    return exponent ? std::ldexp(static_cast<double>(0x800000u|mantissa),static_cast<int>(exponent)-150) :
        std::ldexp(static_cast<double>(mantissa),-149);
}
std::uint32_t DownBits(double x)
{
    if(!std::isfinite(x) || x<0 || x>PhysicalBits(0x7f7fffffu)) Fail("coefficient outside float32 encoding domain");
    if(x==0) return 0;
    std::uint64_t bits; std::memcpy(&bits,&x,8);
    const int exponent=static_cast<int>((bits>>52)&0x7ff)-1023;
    if(exponent < -149) Fail("positive-to-zero float32 quantization would lose support");
    const std::uint64_t mantissa=(std::uint64_t{1}<<52)|(bits&0xfffffffffffffULL);
    if(exponent < -126) return static_cast<std::uint32_t>(mantissa >> (-exponent-97));
    return (static_cast<std::uint32_t>(exponent+127)<<23)|
        (static_cast<std::uint32_t>(mantissa>>29)&0x7fffffu);
}
double DecodeFloat(const unsigned char* p) { return PhysicalBits(Get32(p)); }
V Metadata(const LocalAssetDefinition& d,const LocalAssetEncodingError& e)
{
    return V::MapValue({{"format",V::String(Format)}, {"semantic",Semantic(d.semantic)},
        {"production",Production(d.production)},
        {"incident_t",Array(d.response.incident.tEdges)}, {"incident_phi",Array(d.response.incident.phiEdges)},
        {"outgoing_t",Array(d.response.outgoing.tEdges)}, {"outgoing_phi",Array(d.response.outgoing.phiEdges)},
        {"wavelength_nm",Array(d.response.wavelengthNm)},
        {"encoding_error",V::MapValue({{"maximum_coefficient_absolute",V::Float(e.maximumCoefficientAbsoluteError)},
            {"maximum_forward_row_absolute",V::Float(e.maximumForwardRowAbsoluteError)},
            {"status",V::String("producer_computed_downward_bound_v1")}})}});
}
V ProductionKey(const V& manifest)
{
    V::Members members;
    for(const auto* key:{"format","semantic","production","incident_t","incident_phi","outgoing_t","outgoing_phi","wavelength_nm","encoding_error"})
        members.push_back({key,*manifest.Find(key)});
    return V::MapValue(members);
}
Bytes MakeFile(const LocalAssetDefinition& input,const LocalAssetLimits& limits)
{
    const auto& r=input.response;
    const auto d=Dims({r.incident.tEdges.size(),r.incident.phiEdges.size(),r.outgoing.tEdges.size(),
        r.outgoing.phiEdges.size(),r.wavelengthNm.size()},limits);
    // Bound metadata construction before Encode/Value array creation. Each
    // scalar/record has a known bounded representation; final exact bound below.
    Preflight(d,limits.maximumMetadataBytes,limits);
    if(r.continuous.size()!=d.core.continuousEntries || r.mirror.size()!=d.mirror ||
       input.normalIncidenceGuide.size()!=d.counts[4]) Fail("physical payload cardinality mismatch");
    Coverage(input.semantic,r); WorkCardinality(input.production,d);
    Semantic(input.semantic); Production(input.production);
    // Source core must already satisfy physics; encoding cannot make invalid
    // source coefficients acceptable by quantizing them downward.
    const auto sourceCore=HyLIoSLocalResponse::Create(r,limits.response);
    LocalAssetDefinition quantized=input;
    LocalAssetEncodingError error;
    auto quantize=[&](std::vector<double>& values,bool guide) {
        for(double& x:values) {
            if(guide && (!std::isfinite(x) || x<0 || x>1)) Fail("normal guide outside [0,1]");
            const double f=PhysicalBits(DownBits(x));
            error.maximumCoefficientAbsoluteError=std::max(error.maximumCoefficientAbsoluteError,x-static_cast<double>(f));
            x=f;
        }
    };
    quantize(quantized.response.continuous,false); quantize(quantized.response.mirror,false);
    quantize(quantized.normalIncidenceGuide,true);
    // Bound the core's ACTUAL represented scaled areas, not an assumed pi
    // telescope. Physical() may round a scaled subnormal area; its successor
    // bounds that exact represented area. Upward successors after every add
    // retain the bound even when a tiny positive term vanishes in the sum.
    // Finally add the mirror coefficient and bound multiplication upward.
    if(error.maximumCoefficientAbsoluteError>0) {
        const double infinity=std::numeric_limits<double>::infinity();
        double areaSumUpper=0;
        for(std::size_t j=0;j<d.core.outgoingCells;++j) {
            const double areaUpper=std::nextafter(sourceCore->OutgoingCellArea(j),infinity);
            areaSumUpper=std::nextafter(areaSumUpper+areaUpper,infinity);
        }
        const double factor=std::nextafter(areaSumUpper+1,infinity);
        const double product=error.maximumCoefficientAbsoluteError*factor;
        error.maximumForwardRowAbsoluteError=std::nextafter(product,infinity);
    }
    HyLIoSLocalResponse::Create(quantized.response,limits.response);
    Bytes metadata=Encode(Metadata(quantized,error));
    Preflight(d,metadata.size(),limits);
    Bytes file; file.reserve(Size(Add(Add(HeaderBytes,metadata.size()),Add(d.payload,DigestBytes))));
    file.insert(file.end(),std::begin(Magic),std::end(Magic)); Put32(file,1); Put32(file,0x01020304);
    Put64(file,metadata.size()); Put64(file,d.payload);
    for(auto n:d.counts) Put64(file,n); Put64(file,0);
    file.insert(file.end(),metadata.begin(),metadata.end());
    auto physical=[&](const std::vector<double>& values) {
        for(double x:values) Put32(file,DownBits(x));
    };
    physical(quantized.response.continuous); physical(quantized.response.mirror); physical(quantized.normalIncidenceGuide);
    Digest(file); return file;
}
} // namespace

std::shared_ptr<const HyLIoSLocalResponseAsset> LoadLocalResponseAsset(const std::string& path,
    const LocalAssetSemanticIdentity& expected,const LocalAssetLimits& limits)
{
    // Stat this same opened descriptor, then bounded reads. No path reopen
    // between size validation, digest, and decoding.
    RegularInput stream(path);
    const auto fileSize=stream.SizeBytes();
    if(fileSize>limits.maximumFileBytes || fileSize<HeaderBytes+DigestBytes) Fail("file extent/budget refusal");
    std::array<unsigned char,HeaderBytes> header{};
    stream.Read(header.data(),header.size());
    if(std::memcmp(header.data(),Magic,8)!=0 || Get32(header.data()+8)!=1 ||
       Get32(header.data()+12)!=0x01020304 || Get64(header.data()+72)!=0) Fail("magic/version/endian/reserved header refusal");
    const auto metadataBytes=Get64(header.data()+16), payloadBytes=Get64(header.data()+24);
    std::array<std::uint64_t,5> counts{};
    for(unsigned i=0;i<5;++i) counts[i]=Get64(header.data()+32+8*i);
    const auto d=Dims(counts,limits);
    const auto resident=Preflight(d,metadataBytes,limits);
    if(payloadBytes!=d.payload || fileSize!=Add(Add(HeaderBytes,metadataBytes),Add(payloadBytes,DigestBytes)))
        Fail("nonexact payload/file extent");
    Bytes metadata(Size(metadataBytes));
    stream.Read(metadata.data(),metadata.size());
    V manifest=Decode(metadata);
    Keys(manifest,{"format","semantic","production","incident_t","incident_phi","outgoing_t","outgoing_phi",
        "wavelength_nm","encoding_error"});
    Literal(manifest,"format",Format);
    const auto semantic=ReadSemantic(Field(manifest,"semantic",V::Map));
    const auto wanted=Semantic(expected);
    const auto& actual=Field(manifest,"semantic",V::Map);
    // Compare full required fields themselves; producer-provided hash equality
    // is never accepted as a substitute. Diagnose the exact differing field.
    for(const auto& member:wanted.GetMap()) {
        const auto* found=actual.Find(member.first);
        if(!found || Encode(*found)!=Encode(member.second)) Fail("semantic mismatch: "+member.first+"; rebuild bake for prepared configuration");
    }
    const auto production=ReadProduction(Field(manifest,"production",V::Map));
    WorkCardinality(production,d);
    LocalResponseDefinition response;
    response.incident.tEdges=DoubleArray(manifest,"incident_t",Size(counts[0]));
    response.incident.phiEdges=DoubleArray(manifest,"incident_phi",Size(counts[1]));
    response.outgoing.tEdges=DoubleArray(manifest,"outgoing_t",Size(counts[2]));
    response.outgoing.phiEdges=DoubleArray(manifest,"outgoing_phi",Size(counts[3]));
    response.wavelengthNm=DoubleArray(manifest,"wavelength_nm",Size(counts[4])); Coverage(semantic,response);
    const auto& errors=Field(manifest,"encoding_error",V::Map);
    Keys(errors,{"maximum_coefficient_absolute","maximum_forward_row_absolute","status"});
    Literal(errors,"status","producer_computed_downward_bound_v1");
    LocalAssetEncodingError encodingError{Number(errors,"maximum_coefficient_absolute"),Number(errors,"maximum_forward_row_absolute")};
    if(encodingError.maximumCoefficientAbsoluteError<0 || encodingError.maximumForwardRowAbsoluteError<0)
        Fail("negative declared encoding error");
    // Keep one bounded exact framed buffer for SHA. No unbounded whole-file read.
    Bytes file; file.reserve(Size(fileSize)); file.insert(file.end(),header.begin(),header.end());
    file.insert(file.end(),metadata.begin(),metadata.end()); file.resize(Size(fileSize));
    stream.Read(file.data()+HeaderBytes+metadata.size(),Size(Add(payloadBytes,DigestBytes)));
    stream.RequireEnd();
    const std::string digest=RISECBOR64::SHA256Hex(file.data(),file.size()-DigestBytes);
    if(digest!=Hex(file.data()+file.size()-DigestBytes)) Fail("content SHA256 mismatch");
    const unsigned char* cursor=file.data()+HeaderBytes+metadata.size();
    auto values=[&](std::size_t n) {
        std::vector<double> v; v.reserve(n);
        for(std::size_t i=0;i<n;++i,cursor+=4) v.push_back(DecodeFloat(cursor)); return v;
    };
    response.continuous=values(d.core.continuousEntries); response.mirror=values(Size(d.mirror));
    auto guide=values(Size(counts[4]));
    for(double x:guide) if(x>1) Fail("normal guide outside [0,1]");
    auto core=HyLIoSLocalResponse::Create(response,limits.response);
    auto asset=std::shared_ptr<HyLIoSLocalResponseAsset>(new HyLIoSLocalResponseAsset);
    asset->response_=std::move(core); asset->semantic_=semantic; asset->production_=production;
    asset->guide_=std::move(guide); asset->wavelengths_=std::move(response.wavelengthNm);
    asset->encodingError_=encodingError; asset->contentHash_=digest;
    asset->semanticHash_=RISECBOR64::SHA256Hex(Encode(actual));
    asset->productionHash_=RISECBOR64::SHA256Hex(Encode(ProductionKey(manifest)));
    asset->residentBound_=resident; return asset;
}

void LocalAssetDetail::SaveObserved(const std::string& path,const LocalAssetDefinition& input,
    const LocalAssetLimits& limits,const std::function<void()>& beforePublication)
{
    const Bytes bytes=MakeFile(input,limits);
    Implementation::DiskFileWriteBuffer::WriteDiagnostics diagnostics;
    const bool ok=Implementation::DiskFileWriteBuffer::WriteAtomicallyUTF8(path,
        [&](Implementation::DiskFileWriteBuffer& file) {
            std::size_t at=0;
            while(at<bytes.size()) {
                const auto count=static_cast<unsigned int>(std::min<std::size_t>(bytes.size()-at,std::numeric_limits<unsigned int>::max()));
                if(!file.setBytes(bytes.data()+at,count)) return false; at+=count;
            }
            if(beforePublication) beforePublication(); return true;
        },Implementation::DiskFileWriteBuffer::StagingPolicy::Required,&diagnostics);
    if(!ok) {
        std::string message="atomic publication failed: "+diagnostics.operation+": "+diagnostics.error.message();
        if(!diagnostics.recoveryPath.empty()) message+="; completed output retained for recovery at '"+diagnostics.recoveryPath+"'";
        Fail(message);
    }
}
void SaveLocalResponseAsset(const std::string& path,const LocalAssetDefinition& input,const LocalAssetLimits& limits)
{ LocalAssetDetail::SaveObserved(path,input,limits,{}); }
} // namespace RISE::HyLIoS
