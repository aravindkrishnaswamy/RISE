#include "pch.h"
#include "HyLIoSLocalBake.h"
#include "HyLIoSOptics.h"
#include "Utilities/RISECBOR64.h"
#include "Utilities/DiskFileWriteBuffer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cfenv>
#include <set>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#if defined(__FAST_MATH__)
#error HyLIoSLocalBake must be compiled with strict floating point
#endif
namespace RISE::HyLIoS {
namespace {
using V=RISECBOR64::Value;using U=std::uint64_t;using B=LocalAssetRecord;
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559&&std::numeric_limits<double>::digits==53,
    "HyLIoS compact metadata requires IEEE binary64");
void Require(bool c,const std::string& m){if(!c)throw std::runtime_error("HyLIoS local bake: "+m);}
U Add(U a,U b){Require(b<=std::numeric_limits<U>::max()-a,"uint64 addition overflow");return a+b;}
U Mul(U a,U b){Require(a==0||b<=std::numeric_limits<U>::max()/a,"uint64 multiplication overflow");return a*b;}
B Encode(const V& v){B b;std::string e;Require(RISECBOR64::Encode(v,b,&e),e);return b;}
B Record(V::Members m){auto b=Encode(V::MapValue(m));Require(b.size()<=4096,"canonical identity exceeds 4096-byte record bound; reduce wavelength nodes");return b;}
V String(const char* s){return V::String(s);}V Number(double x){Require(std::isfinite(x),"nonfinite record value");return V::Float(x);}
V Counts(const std::vector<U>& xs){V::Values a;for(U x:xs)a.push_back(V::Unsigned(x));return V::ArrayValue(a);}
V JSON(const Agent::JsonValue& j){
    if(j.isNull())return V();if(j.isBool())return V::Bool(j.asBool());if(j.isString())return V::String(j.asString());
    if(j.isNumber())return Number(j.asNumber());if(j.isArray()){V::Values a;for(std::size_t i=0;i<j.size();++i)a.push_back(JSON(j.at(i)));return V::ArrayValue(a);}
    V::Members m;for(const auto& p:j.members())m.emplace_back(p.first,JSON(p.second));return V::MapValue(m);
}
// Exact binary64 payload; little-endian bytes avoid per-scalar CBOR overhead.
B Packed(const std::vector<double>& values) {
    B bytes;bytes.reserve(values.size()*8);
    for(double value:values) {
        Require(std::isfinite(value)&&value>=0,"invalid packed optical value");
        std::uint64_t bits=0;std::memcpy(&bits,&value,8);
        for(unsigned k=0;k<8;++k)bytes.push_back(static_cast<unsigned char>(bits>>(8*k)));
    }
    return bytes;
}
V Series(const std::vector<double>& values) {
    Require(!values.empty(),"empty optical series");
    const auto packed=Packed(values);
    bool constant=true;for(std::size_t i=1;i<values.size();++i)
        constant=constant&&std::equal(packed.begin(),packed.begin()+8,packed.begin()+8*i);
    if(constant)return Number(values.front());
    return V::BytesValue(packed);
}
V ColumnDictionary(const std::array<std::vector<double>,6>& values,const std::array<bool,6>* active=nullptr) {
    V::Values columns,indices;std::vector<B> identities;
    for(std::size_t layer=0;layer<6;++layer) {
        if(active&&!(*active)[layer]) {indices.push_back(V());continue;}
        const auto identity=Packed(values[layer]);
        const auto it=std::find(identities.begin(),identities.end(),identity);
        const U index=static_cast<U>(it-identities.begin());
        if(it==identities.end()) {identities.push_back(identity);columns.push_back(Series(values[layer]));}
        indices.push_back(V::Unsigned(index));
    }
    return V::MapValue({{"columns",V::ArrayValue(columns)},{"layer_column",V::ArrayValue(indices)}});
}
// BEGIN compiler identity compatibility boundary
std::string CompilerIdentity() {
#if defined(__clang_version__)
    return std::string("Clang ")+__clang_version__;
#elif defined(_MSC_FULL_VER)
    return std::string("MSVC ")+std::to_string(_MSC_FULL_VER);
#elif defined(_MSC_VER)
    return std::string("MSVC ")+std::to_string(_MSC_VER);
#elif defined(__VERSION__)
    return __VERSION__;
#else
    return "unknown compiler";
#endif
}
// END compiler identity compatibility boundary
std::string Hash(const B& b){return RISECBOR64::SHA256Hex(b);}
void ValidatePath(const std::string& path,std::size_t suffixReserve=81) {
    Require(!path.empty()&&path.size()<=32768-suffixReserve&&path.find('\0')==std::string::npos,
        "artifact path exceeds bounded UTF8 pathname contract");
    ValidateExperimentUnicode(path);
}
struct Prepared {
    std::vector<std::unique_ptr<HyLIoSForwardTransport>> kernels;
    std::unique_ptr<HyLIoSSurfaceBoundary> law;
    LocalResponseDefinition blank;std::shared_ptr<const HyLIoSLocalResponse> geometry;
    LocalAssetSemanticIdentity semantic;U cells=0,outs=0,total=0,perNode=0,rows=0,reportBound=0;
};
Prepared Prepare(const std::string& bytes,const LocalBakeConfig& c,bool modelOnly=false){
    Require(c.maximumReportBytes<=64*1024*1024,"statistics report hard cap is 64MiB");
    Require(c.limits.maximumMetadataBytes>0&&c.limits.maximumMetadataBytes<=64*1024,"metadata profile hard cap is 64KiB");
    Require(c.maximumSourceArtifactBytes>0&&c.maximumSourceArtifactBytes<=64*1024*1024,"source artifact hard cap is 64MiB");
    Require(!bytes.empty()&&bytes.size()<=c.maximumSourceArtifactBytes,"exact source artifact exceeds explicit byte budget");
    Require(!c.wavelengthsNm.empty()&&c.wavelengthsNm.size()<=c.limits.response.maximumWavelengths,"wavelength node count exceeds response limit");
    Require(Mul(c.wavelengthsNm.size(),8)<=4096,"canonical wavelength payload exceeds 4096-byte record capacity");
    // Necessary capacity check from the actual canonical node-list extent;
    // complete compact records are checked after independent resolution.
    Record({{"schema",String("hylios_resolved_columns_v2")},{"wavelength_nm_f64le",V::BytesValue(Packed(c.wavelengthsNm))}});
    Require(c.replicates>=2&&c.replicates<=1024,"requires 2..1024 independent replicates");
    Require(c.historiesPerCellPerReplicate>0&&c.normalGuideHistoriesPerReplicate>0&&c.maximumWorkPerHistory>0,"positive history/work counts required");
    Require(c.boundary==ExperimentBoundary::Smooth||c.boundary==ExperimentBoundary::HyLIoS2015,"unsupported boundary");
    Require(c.placement==ForwardPlacementPolicy::NullCollisionPlacementV1||c.placement==ForwardPlacementPolicy::FixedEventDiagnosticV1,"unsupported placement");
    Require(c.postScatter==ForwardPostScatterPolicy::DiscardAfterScatterV1||c.postScatter==ForwardPostScatterPolicy::DiagnosticRetainParticleV1,"unsupported post-scatter policy");
    Require(std::isfinite(c.exteriorIOR)&&c.exteriorIOR>0,"invalid exterior IOR");
    Require(c.incident.tEdges.size()>=2&&c.incident.phiEdges.size()>=2&&c.outgoing.tEdges.size()>=2&&c.outgoing.phiEdges.size()>=2,"empty angular axis");
    for(const auto* axis:{&c.incident.tEdges,&c.incident.phiEdges,&c.outgoing.tEdges,&c.outgoing.phiEdges})
        Require(axis->size()<=8192,"asset axis cardinality cap exceeded before generation");
    auto storage=HyLIoSLocalResponse::EstimateStorage(c.wavelengthsNm.size(),c.incident.tEdges.size()-1,c.incident.phiEdges.size()-1,c.outgoing.tEdges.size()-1,c.outgoing.phiEdges.size()-1,c.limits.response);
    Prepared p;p.cells=storage.incidentCells;p.outs=storage.outgoingCells;
    // <=2^53 guarantees exact integer-to-binary64 conversions for the disclosed
    // downward count ratios; practical explicit work limits are much smaller.
    Require(Mul(c.replicates,std::max(c.historiesPerCellPerReplicate,c.normalGuideHistoriesPerReplicate))<=U{1}<<53,"estimator denominator exceeds exact binary64 integer domain");
    p.perNode=Add(Mul(Mul(p.cells,c.replicates),c.historiesPerCellPerReplicate),Mul(c.replicates,c.normalGuideHistoriesPerReplicate));
    p.total=Mul(c.wavelengthsNm.size(),p.perNode);Require(p.total<=c.maximumTotalHistories,"total history budget exceeded before generation");
    Require(Mul(p.total,c.maximumWorkPerHistory)<=c.maximumTotalWork,"total worst-case work budget exceeded before generation");
    p.rows=Mul(c.wavelengthsNm.size(),Add(p.cells,1));
    // CBOR integer worst case 9 bytes; floats 9 bytes; arrays/maps/text overhead
    // bounded by 4096 bytes per row plus 128 bytes per bin per replicate/summary.
    p.reportBound=Add(16384,Mul(p.rows,Add(4096,Mul(Add(c.replicates,1),Mul(Add(p.outs,3),128)))));
    Require(p.reportBound<=c.maximumReportBytes,"statistics report budget exceeded before generation");
    const U peakBound=Add(Mul(storage.storageBytes,3),Add(modelOnly?1024*1024:Mul(p.reportBound,64),Mul(bytes.size(),256)));
    Require(peakBound<=c.limits.maximumResidentBytes,"bake peak logical resident budget exceeded before generation");
    if(!modelOnly) {
    // Mirror the codec's advertised conservative Save admission before any
    // history. No quantized values or producer uncertainty claims are needed
    // to bound fixed payload/cardinality and maximal metadata allocations.
    const U coefficients=Add(Add(storage.continuousEntries,Mul(c.wavelengthsNm.size(),p.cells)),c.wavelengthsNm.size());
    const U wireBound=Add(Add(80,c.limits.maximumMetadataBytes),Add(Mul(coefficients,4),32));
    Require(wireBound<=c.limits.maximumFileBytes,"asset file budget exceeded before generation");
    U codecPeak=Add(Mul(c.limits.maximumMetadataBytes,4096),Mul(wireBound,2));
    codecPeak=Add(codecPeak,Mul(storage.storageBytes,2));codecPeak=Add(codecPeak,Mul(coefficients,8));
    U axes=c.wavelengthsNm.size();for(const auto* axis:{&c.incident.tEdges,&c.incident.phiEdges,&c.outgoing.tEdges,&c.outgoing.phiEdges})axes=Add(axes,axis->size());
    codecPeak=Add(Add(codecPeak,Mul(axes,32)),1024*1024);
    Require(codecPeak<=c.limits.maximumResidentBytes,"asset codec resident budget exceeded before generation");
    }
    p.blank.incident=c.incident;p.blank.outgoing=c.outgoing;p.blank.wavelengthNm=c.wavelengthsNm;
    p.blank.continuous.resize(storage.continuousEntries);p.blank.mirror.resize(Mul(c.wavelengthsNm.size(),p.cells));
    p.geometry=HyLIoSLocalResponse::Create(p.blank,c.limits.response);
    for(const auto* g:{&c.incident})for(const auto* axis:{&g->tEdges,&g->phiEdges})for(std::size_t k=0;k+1<axis->size();++k){
        const double a=(*axis)[k],b=(*axis)[k+1],width=b-a;
        Require(a+0x1p-33*width>a && a+(1-0x1p-33)*width<b,"incident jitter has unresolved finite-grid support; use coarser grid");
    }
    for(U j=0;j<p.outs;++j)Require(p.geometry->OutgoingCellArea(j)>=std::numeric_limits<double>::min(),"outgoing area subnormal; estimator error bound unsupported");
    // Keep only one resolver result alive. The exact source artifact preserves
    // full provenance/constituents; the bounded semantic records retain all
    // numerical inputs actually consumed by the forward kernel.
    std::array<std::vector<double>,6> hostAbsorption,layerIor,particleAbsorption;
    std::array<bool,6> active{};
    V::Values geometryRows;
    B geometryIdentity;
    V compositionRevision,interpretation,preset,approximate,coverageProfile,bundleClaim;
    double foldAspect=0;
    std::string sourceHash;
    std::vector<double> fiberRates;
    bool first=true;
    for(double nm:c.wavelengthsNm) {
        const auto n=ResolveExperiment(bytes,{nm,c.boundary,c.explicitComplexPackV1});
        V::Values geometry;
        for(std::size_t i=0;i<6;++i) {
            const auto& layer=n.Layers()[i];
            hostAbsorption[i].push_back(layer.hostAbsorptionPerMetre);
            layerIor[i].push_back(layer.realIOR);
            V particle;
            if(layer.organelle) {
                const auto& o=*layer.organelle;
                // N0/N1 and individuals use lineage-v2; nonempty N>1 below
                // 1400nm must apply the accepted positive final kick before
                // body-to-bulk completion. Zero observed unknown is no proof.
                Require(o.organization!=ForwardOrganization::ExplicitComplexPackV1||o.childCount<=1||nm<1400,
                    "angular closure unproved for active complex childCount > 1 at wavelength >= 1400nm; retain full-model measurement route");
                if(first)active[i]=true;
                Require(active[i],"particle organization changes between wavelength nodes");
                particleAbsorption[i].push_back(o.absorptionPerMetre);
                particle=V::ArrayValue({V::Unsigned(static_cast<U>(o.organization)),Number(o.volumeFraction),
                    Number(o.semiMinorMetres),Number(o.semiMajorMetres),Number(o.realIOR),
                    Number(o.envelopeRadiusMetres),V::Unsigned(o.childCount)});
            } else Require(!active[i],"particle organization changes between wavelength nodes");
            geometry.push_back(V::ArrayValue({Number(layer.thicknessMetres),particle}));
        }
        const auto encodedGeometry=Encode(V::ArrayValue(geometry));
        if(first) {
            geometryRows=geometry;geometryIdentity=encodedGeometry;foldAspect=n.SurfaceFoldAspect();sourceHash=n.BytesHash();
            const auto& original=n.Original();
            compositionRevision=JSON(original.get("composition_revision"));interpretation=JSON(original.get("interpretation"));
            preset=JSON(original.get("preset_id"));approximate=JSON(original.get("approximate"));
            coverageProfile=JSON(original.get("coverage_profile_id"));bundleClaim=JSON(original.get("spectral_bundle_sha256"));
            first=false;
        } else Require(encodedGeometry==geometryIdentity&&n.SurfaceFoldAspect()==foldAspect&&n.BytesHash()==sourceHash,
            "resolved geometry/source changes between wavelength nodes");
        const double rate=RayleighRate(c.fiberRadiusMetres,nm*1e-9,c.fiberFraction,c.fiberRelativeIOR);
        fiberRates.push_back(rate);
        if(!modelOnly)p.kernels.emplace_back(new HyLIoSForwardTransport(n.Layers(),nm,c.exteriorIOR,rate,
            AbsorptionEstimator::Analog,0,c.placement,c.postScatter));
    }
    if(c.boundary==ExperimentBoundary::HyLIoS2015)p.law.reset(new HyLIoSSurfaceBoundary(foldAspect));
    auto& s=p.semantic;s.coveredFirstNm=c.wavelengthsNm.front();s.coveredLastNm=c.wavelengthsNm.back();
    s.model=Record({{"transport",String(ForwardTransportVersion)},{"elastic",V::Bool(true)},{"emission",V::Bool(false)},
        {"length_unit",String("metre")},{"composition_revision",compositionRevision},{"interpretation",interpretation},
        {"numeric_layout",String("f64le_columns_constant_scalars_v2")},
        {"particle_absorption_m_inv",ColumnDictionary(particleAbsorption,&active)},
        {"fiber_parameters",V::ArrayValue({Number(c.fiberRadiusMetres),Number(c.fiberFraction),Number(c.fiberRelativeIOR)})},
        {"fiber_rate_m_inv",Series(fiberRates)}});
    s.placement=Record({{"policy",String(c.placement==ForwardPlacementPolicy::NullCollisionPlacementV1?
        "null_collision_placement_v1":"fixed_event_diagnostic_v1")},{"complex_layout",String(ComplexLayoutVersion)},
        {"integerization",String("resolved_maximum_internal_count_as_actual_children_v1")}});
    s.postscatter=Record({{"policy",String(c.postScatter==ForwardPostScatterPolicy::DiscardAfterScatterV1?
        ForwardDiscardPostScatterVersion:ForwardDiagnosticPostScatterVersion)}});
    s.boundary=Record({{"law",String(c.boundary==ExperimentBoundary::Smooth?"smooth_planar_dielectric_v1":SurfaceBoundaryVersion)},
        {"authored_fold_aspect",Number(foldAspect)},{"bottom",String("unit_cosine_diffuse_v1")},
        {"fold_rejection",String("resumable_each_proposal_charged_v1")}});
    s.lineage=Record({{"version",String(ForwardAngularLineageVersion)},
        {"static_closure",String("individual_complex_N0_N1_v2;Ngt1_positive_finalkick_dominates_return_v1")}});
    s.data=Record({{"schema",String("hylios_resolved_columns_v2")},{"numeric_layout",String("f64le_columns_constant_scalars_v2")},
        {"wavelength_nm_f64le",V::BytesValue(Packed(c.wavelengthsNm))},
        {"host_absorption_m_inv",ColumnDictionary(hostAbsorption)}});
    s.specimen=Record({{"geometry_fields",String("thickness_m,organelle=[organization,fraction,minor_m,major_m,ior,envelope_m,child_count]")},
        {"layer_geometry",V::ArrayValue(geometryRows)},{"layer_ior",ColumnDictionary(layerIor)},
        {"source_artifact_sha256",V::String(sourceHash)},{"source_artifact_bytes",V::Unsigned(bytes.size())},
        {"source_artifact_format",String("exact_resolved_specimen_json_v1")},{"preset_label",preset}});
    s.exterior=Record({{"constant_spatial_and_spectral",V::Bool(true)},{"ior",Number(c.exteriorIOR)},
        {"wavelength_nodes",String("data.wavelength_nm_f64le")}});
    s.approximation=Record({{"local",String("integrate_all_exit_displacement_infinite_homogeneous_slab_v1")},
        {"source_approximate",approximate},{"coverage_profile",coverageProfile},{"source_bundle_claim_uncertified",bundleClaim},
        {"full_provenance",String("independently_hashed_exact_source_artifact_v1")},
        {"incident",String("uniform_projected_cell_average_top32_midpoint_v1")},
        {"spectral",String("nonnegative_linear_nodes_v1")},{"frame",String("right_handed_kernel_X_minusY_minusZ_v1")}});
    U recordBytes=0;
    for(const auto* record:{&s.model,&s.placement,&s.postscatter,&s.boundary,&s.lineage,&s.data,&s.specimen,&s.exterior,&s.approximation})
        recordBytes=Add(recordBytes,record->size());
    // Two production records <=4096 each, bounded framing/key/revision text,
    // exact public-grid/node array extents (binary64 scalars are nine bytes).
    U scalarCount=c.wavelengthsNm.size();
    for(const auto* axis:{&c.incident.tEdges,&c.incident.phiEdges,&c.outgoing.tEdges,&c.outgoing.phiEdges})
        scalarCount=Add(scalarCount,axis->size());
    const U metadataBound=Add(Add(recordBytes,Mul(scalarCount,9)),2*4096+4096);
    Require(metadataBound<=c.limits.maximumMetadataBytes,"canonical metadata budget exceeded before generation");
    return p;
}
struct Row {std::vector<std::vector<U>> counts;std::vector<U> bulk,surface;};
struct Stats {double mean=0,historySE=0,replicateSE=0,lower95=0,upper95=1;};
Stats Statistics(const std::vector<U>& counts,U n){
    const U sum=std::accumulate(counts.begin(),counts.end(),U{0});const double mean=static_cast<double>(sum)/static_cast<double>(Mul(counts.size(),n));
    double variance=0;for(U x:counts){const double d=static_cast<double>(x)/static_cast<double>(n)-mean;variance+=d*d;}
    const double total=static_cast<double>(Mul(counts.size(),n)), z2=3.841458820694124;
    const double center=(mean+z2/(2*total))/(1+z2/total);
    const double radius=std::sqrt(z2*(mean*(1-mean)/total+z2/(4*total*total)))/(1+z2/total);
    return {mean,std::sqrt(mean*(1-mean)/total),std::sqrt(variance/(static_cast<double>(counts.size())*static_cast<double>(counts.size()-1))),std::max(0.0,center-radius),std::min(1.0,center+radius)};
}
V StatValue(const Stats& s){return V::MapValue({{"mean",Number(s.mean)},{"bernoulli_history_se",Number(s.historySE)},{"independent_replicate_se",Number(s.replicateSE)},{"wilson95_lower_diagnostic",Number(s.lower95)},{"wilson95_upper_diagnostic",Number(s.upper95)}});}
std::string ReportPath(const std::string& path,const std::string& hash){Require(hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid statistics report digest");return path+".statistics."+hash+".cbor";}
// Caller-owned records are scanned before any generic Value tree is built.
// All live record decoders (Reconstruction retains several) share the codec's
// conservative factor; owned scalar/record buffers and fixed control scratch
// are admitted separately. Source/report reconstruction has later phase gates.
U AdmitMetadata(const LocalAssetSemanticIdentity& s,const LocalAssetProductionIdentity* p,
    const LocalAssetLimits& limits,U retained) {
    U bytes=0;
    for(const B* record:{&s.model,&s.placement,&s.postscatter,&s.boundary,&s.lineage,
        &s.data,&s.specimen,&s.exterior,&s.approximation}) {
        LocalAssetDetail::ValidateRecordSyntax(*record);bytes=Add(bytes,record->size());
    }
    if(p)for(const B* record:{&p->build,&p->statistics}) {
        LocalAssetDetail::ValidateRecordSyntax(*record);bytes=Add(bytes,record->size());
    }
    Require(limits.maximumMetadataBytes>0&&limits.maximumMetadataBytes<=64*1024&&bytes<=limits.maximumMetadataBytes,
        "metadata record aggregate exceeds explicit profile budget");
    Require(Add(Add(Add(retained,bytes),LocalAssetDetail::MetadataDecoderBytesBound(bytes)),1024*1024)<=limits.maximumResidentBytes,
        "METADATA_ADMISSION resident budget exceeded before metadata decode");
    return bytes;
}
U ScalarBytes(const LocalResponseDefinition& r) {
    U count=Add(Add(r.continuous.size(),r.mirror.size()),r.wavelengthNm.size());
    for(const auto* axis:{&r.incident.tEdges,&r.incident.phiEdges,&r.outgoing.tEdges,&r.outgoing.phiEdges})count=Add(count,axis->size());
    return Mul(count,8);
}
V Decode(const B& b){LocalAssetDetail::ValidateRecordSyntax(b);V v;std::string e;Require(RISECBOR64::DecodeCanonical(b,v,&e),e);return v;}
std::string ReportHash(const LocalAssetProductionIdentity& p){const auto s=Decode(p.statistics);const auto* f=s.Find("full_report_sha256");Require(f&&f->GetType()==V::Text&&f->GetText().size()==64,"missing bound statistics report hash");return f->GetText();}
U ReportBytes(const LocalAssetProductionIdentity& p){const auto s=Decode(p.statistics);const auto* f=s.Find("full_report_bytes");Require(f&&f->GetType()==V::UnsignedInteger&&f->GetIntegerArgument()>0&&f->GetIntegerArgument()<=64*1024*1024,"invalid bound statistics report extent");return f->GetIntegerArgument();}
struct SourceInfo {std::string hash;U bytes=0;};
bool HexDigest(const std::string& text) {
    return text.size()==64&&std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
SourceInfo Source(const LocalAssetSemanticIdentity& semantic) {
    const auto record=Decode(semantic.specimen);
    const auto* hash=record.Find("source_artifact_sha256");const auto* size=record.Find("source_artifact_bytes");
    Require(hash&&hash->GetType()==V::Text&&HexDigest(hash->GetText())&&size&&size->GetType()==V::UnsignedInteger&&
        size->GetIntegerArgument()>0&&size->GetIntegerArgument()<=64*1024*1024,"invalid exact source artifact declaration");
    return {hash->GetText(),size->GetIntegerArgument()};
}
std::string SourcePath(const std::string& path,const SourceInfo& source) {return path+".source."+source.hash+".json";}
void VerifyArtifact(const std::string& path,const std::string& hash,U bytes,const char* label) {
    const auto actual=LocalAssetDetail::HashRegularFile(path,bytes);
    Require(actual.bytes==bytes&&actual.sha256==hash,std::string("missing or mismatched bound ")+label);
}
// One bounded owned byte snapshot is hashed and parsed without a generic
// value tree or any path reopen between integrity and semantic validation.
class ReportReader {
public:
    struct Head {unsigned major;U value;};
    explicit ReportReader(const B& bytes):input(bytes),extent(bytes.size()){}
    Head Token() {
        const unsigned first=Byte(),major=first>>5,ai=first&31;
        if(major==7) {
            if(ai==20||ai==21)return {8,ai};
            Require(ai==27,"statistics report requires binary64 scalar encoding");return {major,Word(8)};
        }
        Require(major<=5&&ai<=27,"unsupported statistics report token");
        if(ai<24)return {major,ai};
        const unsigned width=1U<<(ai-24);const U value=Word(width);
        const U minimum=width==1?24:width==2?256:width==4?65536:U{1}<<32;
        Require(value>=minimum,"noncanonical statistics report integer/length");return {major,value};
    }
    std::string Text(Head head,U maximum=1024) {
        Require(head.major==3&&head.value<=maximum,"statistics report text/key bound");
        std::string text;for(U n=0;n<head.value;++n){const unsigned c=Byte();Require(c>=32&&c<127,"statistics report text must be printable ASCII");text.push_back(static_cast<char>(c));}return text;
    }
    U UInt(){const auto head=Token();Require(head.major==0,"report unsigned integer type");return head.value;}
    double Float(){const auto head=Token();Require(head.major==7,"report float64 type");double value=0;std::memcpy(&value,&head.value,8);Require(std::isfinite(value)&&value>=0,"invalid report float64");return value;}
    bool Boolean(){const auto head=Token();Require(head.major==8,"report boolean type");return head.value==21;}
    void Array(U count){const auto head=Token();Require(head.major==4&&head.value==count,"closed report array shape mismatch");}
    void Map(U count){const auto head=Token();Require(head.major==5&&head.value==count,"closed report row/stat map shape mismatch");}
    void Key(const char* key){Require(Text(Token(),128)==key,std::string("closed report field/order mismatch: ")+key);}
    bool Finished()const{return used==extent;}
private:
    unsigned Byte(){Require(used<extent,"statistics report truncated");return input[static_cast<std::size_t>(used++)];}
    U Word(unsigned width){U value=0;for(unsigned n=0;n<width;++n)value=(value<<8)|Byte();return value;}
    const B& input;U extent,used=0;
};
U ExpectedHistories(const LocalAssetProductionIdentity& production,U nodes,U cells) {
    Require(production.replicates>=2&&production.replicates<=1024&&
        Mul(production.replicates,std::max(production.historiesPerCellPerReplicate,production.normalGuideHistoriesPerReplicate))<=(U{1}<<53),
        "invalid analog estimator replicate/denominator declaration");
    Require(production.oracleRevision==ForwardTransportVersion&&production.lineageRevision==ForwardAngularLineageVersion&&
        production.estimatorRevision=="analog_noRR_allN_downward_binary64_division_v1"&&
        production.historyMappingRevision=="splitmix64_top53_disjoint_fullgrid_and_exactnormal_index_v1"&&
        production.statisticsRevision=="exact_replicate_bin_counts_bernoulli_and_replicate_se_source_bound_v2",
        "unsupported bake production algorithm revision");
    return Mul(nodes,Add(Mul(Mul(cells,production.replicates),production.historiesPerCellPerReplicate),
        Mul(production.replicates,production.normalGuideHistoriesPerReplicate)));
}
struct ReportValues {
    const HyLIoSLocalResponse& geometry;
    const std::vector<double>& guide;
    bool encoded;
    std::function<double(U,U,U)> continuous;
    std::function<double(U,U)> mirror;
    const LocalAssetEncodingError* encodingError=nullptr;
};
struct ReportSummary {U bulk=0,surface=0;double historySE=0,replicateSE=0,construction=0,encoding=0;};
U ReportScratch(const LocalAssetProductionIdentity& p,U nodes,U cells,U outgoing) {
    Require(p.replicates>=2&&p.replicates<=1024,"report replicate count exceeds bounded validation profile");
    const U bound=Add(16384,Mul(Mul(nodes,Add(cells,1)),Add(4096,Mul(Add(p.replicates,1),Mul(Add(outgoing,3),128)))));
    Require(bound<=64*1024*1024,"report dimension envelope exceeds 64MiB profile");
    U bytes=Add(65536,Mul(Add(p.statistics.size(),p.build.size()),4096));
    bytes=Add(bytes,sizeof(std::vector<Stats>)+4*sizeof(std::vector<U>)+sizeof(std::vector<std::vector<U>>));
    bytes=Add(bytes,Mul(p.replicates,sizeof(std::vector<U>)));
    return Add(bytes,Add(Mul(Mul(p.replicates,Add(outgoing,2)),8),Add(Mul(Add(outgoing,2),sizeof(Stats)),Mul(p.replicates,32))));
}
void AddStats(ReportSummary& summary,const Stats& stats) {
    summary.historySE=std::max(summary.historySE,stats.historySE);summary.replicateSE=std::max(summary.replicateSE,stats.replicateSE);
}
Stats ReadStats(ReportReader& reader) {
    reader.Map(5);Stats stats;
    reader.Key("bernoulli_history_se");stats.historySE=reader.Float();
    reader.Key("independent_replicate_se");stats.replicateSE=reader.Float();
    reader.Key("mean");stats.mean=reader.Float();
    reader.Key("wilson95_lower_diagnostic");stats.lower95=reader.Float();
    reader.Key("wilson95_upper_diagnostic");stats.upper95=reader.Float();return stats;
}
void SameStats(const Stats& actual,const Stats& expected) {
    Require(actual.mean==expected.mean&&actual.historySE==expected.historySE&&actual.replicateSE==expected.replicateSE&&
        actual.lower95==expected.lower95&&actual.upper95==expected.upper95,"report derived statistics mismatch");
}
void CheckCoefficient(double actual,double original,bool encoded,ReportSummary& summary) {
    const double quantized=LocalAssetDetail::QuantizeCoefficient(original);
    summary.encoding=std::max(summary.encoding,original-quantized);
    Require(actual==(encoded?quantized:original),"REPORT_RESPONSE_COUNTS response/guide does not match reported counts");
}
void ReadRows(ReportReader& reader,const LocalAssetProductionIdentity& p,U nodes,const ReportValues& values,ReportSummary& summary) {
    const U cells=values.geometry.Storage().incidentCells,outgoing=values.geometry.Storage().outgoingCells,bins=Add(outgoing,2);
    Require(values.guide.size()==nodes,"report guide cardinality mismatch");
    reader.Array(Mul(nodes,Add(cells,1)));
    for(U l=0;l<nodes;++l)for(U i=0;i<=cells;++i) {
        const bool normal=i==cells;const U histories=normal?p.normalGuideHistoriesPerReplicate:p.historiesPerCellPerReplicate;
        std::vector<Stats> reported(static_cast<std::size_t>(bins));std::vector<U> bulk(static_cast<std::size_t>(p.replicates)),surface(bulk.size());
        std::vector<std::vector<U>> counts(bulk.size(),std::vector<U>(static_cast<std::size_t>(bins)));
        reader.Map(9);
        reader.Key("bin_statistics");reader.Array(bins);for(auto& stat:reported)stat=ReadStats(reader);
        reader.Key("bulk_work_per_replicate");reader.Array(p.replicates);for(auto& work:bulk)work=reader.UInt();
        reader.Key("histories_per_replicate");Require(reader.UInt()==histories,"report row history denominator mismatch");
        reader.Key("incident_cell_or_normal");Require(reader.UInt()==i,"report incident row ordering/index mismatch");
        reader.Key("normal_exact");Require(reader.Boolean()==normal,"report normal-row flag mismatch");
        reader.Key("replicate_bin_counts");reader.Array(p.replicates);
        for(auto& replicate:counts) {
            reader.Array(bins);U total=0;for(auto& count:replicate){count=reader.UInt();Require(count<=histories,"report count exceeds history denominator");total=Add(total,count);}
            Require(total==histories,"REPORT_COUNT_CONSERVATION replicate counts do not sum to all histories");
        }
        reader.Key("returned_statistics");const auto returnedStat=ReadStats(reader);
        reader.Key("surface_work_per_replicate");reader.Array(p.replicates);for(auto& work:surface)work=reader.UInt();
        reader.Key("wavelength_index");Require(reader.UInt()==l,"report wavelength row ordering/index mismatch");
        std::vector<U> returned(bulk.size()),column(bulk.size());
        const U denominator=Mul(histories,p.replicates);
        for(U j=0;j<bins;++j) {
            U total=0;for(U r=0;r<p.replicates;++r){column[r]=counts[r][j];total=Add(total,column[r]);if(j<=outgoing)returned[r]=Add(returned[r],column[r]);}
            const auto stats=Statistics(column,histories);SameStats(reported[j],stats);AddStats(summary,stats);
            if(!normal&&j<outgoing)CheckCoefficient(values.continuous(l,i,j),LocalBakeDetail::DownwardCoefficient(total,denominator,values.geometry.OutgoingCellArea(j)),values.encoded,summary);
            else if(!normal&&j==outgoing)CheckCoefficient(values.mirror(l,i),LocalBakeDetail::DownwardCountRatio(total,denominator),values.encoded,summary);
        }
        const auto stats=Statistics(returned,histories);SameStats(returnedStat,stats);AddStats(summary,stats);
        const U returnedTotal=std::accumulate(returned.begin(),returned.end(),U{0});
        if(returnedTotal)summary.construction=10*std::numeric_limits<double>::epsilon();
        if(normal)CheckCoefficient(values.guide[l],LocalBakeDetail::DownwardCountRatio(returnedTotal,denominator),values.encoded,summary);
        for(U r=0;r<p.replicates;++r) {
            Require(Add(bulk[r],surface[r])<=Mul(histories,p.maximumWorkPerHistory),"report replicate work cap mismatch");
            summary.bulk=Add(summary.bulk,bulk[r]);summary.surface=Add(summary.surface,surface[r]);
        }
    }
}
void ClosedKeys(const V& record,std::initializer_list<const char*> keys) {
    Require(record.GetType()==V::Map&&record.GetMap().size()==keys.size(),"closed production statistics schema mismatch");
    for(const char* key:keys)Require(record.Find(key)!=nullptr,std::string("missing production statistics field: ")+key);
}
U RecordUInt(const V& record,const char* key) {const auto* value=record.Find(key);Require(value&&value->GetType()==V::UnsignedInteger,std::string("invalid integer record field: ")+key);return value->GetIntegerArgument();}
double RecordFloat(const V& record,const char* key) {const auto* value=record.Find(key);Require(value&&value->GetType()==V::Float64&&std::isfinite(value->GetFloat())&&value->GetFloat()>=0,std::string("invalid float record field: ")+key);return value->GetFloat();}
void RecordLiteral(const V& record,const char* key,const char* literal) {const auto* value=record.Find(key);Require(value&&value->GetType()==V::Text&&value->GetText()==literal,std::string("record literal mismatch: ")+key);}
ReportSummary CheckReportBinding(const B& input,const SourceInfo& source,const LocalAssetProductionIdentity& production,U nodes,const ReportValues& values) {
    const U cells=values.geometry.Storage().incidentCells,outgoing=values.geometry.Storage().outgoingCells;
    const U reportEnvelope=Add(16384,Mul(Mul(nodes,Add(cells,1)),Add(4096,Mul(Add(production.replicates,1),Mul(Add(outgoing,3),128)))));
    const U histories=ExpectedHistories(production,nodes,cells),worstWork=Mul(histories,production.maximumWorkPerHistory);
    ReportScratch(production,nodes,cells,outgoing);
    const std::set<std::string> fields{"schema","bin_order","rows","normal_experiment","completed_histories","bulk_work","surface_work",
        "maximum_total_histories","maximum_total_work","seed","rng","index","angular_discretization","spectral_discretization",
        "maximum_binary64_row_construction_loss_bound","source_artifact_sha256","source_artifact_bytes","maximum_source_artifact_bytes","maximum_report_bytes"};
    ReportReader reader(input);reader.Map(fields.size());std::string previous;U bulk=0,surface=0;double construction=0;ReportSummary summary;
    for(U n=0;n<fields.size();++n) {
        const auto key=reader.Text(reader.Token(),128);Require(fields.count(key)&&(n==0||previous<key),"statistics report top-level key order/field");previous=key;
        if(key=="schema")Require(reader.Text(reader.Token())=="hylios_local_full_statistics_v2","statistics report schema mismatch");
        else if(key=="bin_order")Require(reader.Text(reader.Token())=="continuous_cells,proved_planar_mirror,physical_absorption","report bin order mismatch");
        else if(key=="normal_experiment")Require(reader.Text(reader.Token())=="wi=(0,0,1),separate_indices_v1","report normal experiment mismatch");
        else if(key=="rng")Require(reader.Text(reader.Token())=="splitmix64_top53_v1","report RNG mismatch");
        else if(key=="index")Require(reader.Text(reader.Token())=="l*(I*R*N+R*G)+i*R*N+r*N+h;normal=l*block+I*R*N+r*G+h;seed_plus_index_mod64","report history mapping mismatch");
        else if(key=="angular_discretization")Require(reader.Text(reader.Token())=="cell_constant_projected_top32_midpoint_average;refinement_not_certified","report angular estimator mismatch");
        else if(key=="spectral_discretization")Require(reader.Text(reader.Token())=="linear_nodes;heldout_not_certified","report spectral estimator mismatch");
        else if(key=="source_artifact_sha256")Require(reader.Text(reader.Token())==source.hash,"statistics report source hash mismatch");
        else if(key=="source_artifact_bytes")Require(reader.UInt()==source.bytes,"statistics report source extent mismatch");
        else if(key=="completed_histories")Require(reader.UInt()==histories,"statistics report history cardinality mismatch");
        else if(key=="seed")Require(reader.UInt()==production.seed,"statistics report exact seed mismatch");
        else if(key=="maximum_total_histories")Require(reader.UInt()>=histories,"report declared history budget mismatch");
        else if(key=="maximum_total_work")Require(reader.UInt()>=worstWork,"report declared work budget mismatch");
        else if(key=="maximum_source_artifact_bytes"){const U limit=reader.UInt();Require(limit>=source.bytes&&limit<=64*1024*1024,"report source budget mismatch");}
        else if(key=="maximum_report_bytes"){const U limit=reader.UInt();Require(limit>=input.size()&&limit>=reportEnvelope&&limit<=64*1024*1024,"report byte budget mismatch");}
        else if(key=="rows")ReadRows(reader,production,nodes,values,summary);
        else if(key=="bulk_work")bulk=reader.UInt();
        else if(key=="surface_work")surface=reader.UInt();
        else if(key=="maximum_binary64_row_construction_loss_bound")construction=reader.Float();
    }
    Require(reader.Finished(),"statistics report trailing bytes");
    Require(bulk==summary.bulk&&surface==summary.surface&&Add(bulk,surface)<=worstWork,"report derived work totals mismatch");
    Require(construction==summary.construction,"report construction bound mismatch");
    const auto statistics=Decode(production.statistics);
    ClosedKeys(statistics,{"source_artifact_sha256","source_artifact_bytes","full_report_sha256","full_report_bytes","full_report_schema","completed_histories","bulk_work","surface_work","maximum_bin_or_row_bernoulli_history_se","maximum_bin_or_row_independent_replicate_se","maximum_binary64_row_construction_loss_bound","confidence","refinement"});
    Require(RecordUInt(statistics,"completed_histories")==histories&&RecordUInt(statistics,"bulk_work")==bulk&&RecordUInt(statistics,"surface_work")==surface,"production/report count/work summary mismatch");
    Require(RecordFloat(statistics,"maximum_bin_or_row_bernoulli_history_se")==summary.historySE&&RecordFloat(statistics,"maximum_bin_or_row_independent_replicate_se")==summary.replicateSE&&
        RecordFloat(statistics,"maximum_binary64_row_construction_loss_bound")==summary.construction,"production/report uncertainty summary mismatch");
    RecordLiteral(statistics,"full_report_schema","hylios_local_full_statistics_v2");
    RecordLiteral(statistics,"confidence","SE_is_diagnostic_not_a_coverage_guarantee;zero_observed_bins_not_proved_zero");
    RecordLiteral(statistics,"refinement","not_certified;synthetic_controls_not_skin_validation");
    if(values.encodingError)Require(values.encodingError->maximumCoefficientAbsoluteError==summary.encoding&&
        values.encodingError->maximumForwardRowAbsoluteError==LocalAssetDetail::ForwardEncodingErrorBound(summary.encoding,values.geometry),"report-derived codec encoding error mismatch");
    return summary;
}
void CheckProductionSource(const LocalAssetProductionIdentity& production,const SourceInfo& source) {
    const auto statistics=Decode(production.statistics);
    const auto* hash=statistics.Find("source_artifact_sha256");const auto* size=statistics.Find("source_artifact_bytes");
    Require(hash&&hash->GetType()==V::Text&&hash->GetText()==source.hash&&size&&size->GetType()==V::UnsignedInteger&&size->GetIntegerArgument()==source.bytes,
        "production/source semantic binding mismatch");
    const auto build=Decode(production.build);
    Require(build.GetType()==V::Map&&build.GetMap().size()==4&&build.Find("adapter_version")&&build.Find("compiler")&&
        build.Find("selected_artifact_sha256")&&build.Find("adapter_strict_fp"),"BUILD_RECORD_SCHEMA closed four-field build record required");
    const auto* version=build.Find("adapter_version");
    Require(version->GetType()==V::Text&&version->GetText()==LocalBakeVersion,"unsupported bake adapter version");
    const auto* compiler=build.Find("compiler");
    Require(compiler->GetType()==V::Text&&!compiler->GetText().empty()&&compiler->GetText().size()<=256&&
        std::all_of(compiler->GetText().begin(),compiler->GetText().end(),[](unsigned char c){return c>=32&&c!=127;})&&
        std::any_of(compiler->GetText().begin(),compiler->GetText().end(),[](unsigned char c){return c>32;}),
        "BUILD_RECORD_COMPILER compiler must be NFC text of 1..256 bytes excluding ASCII controls and all-ASCII-space strings");
    const auto* digest=build.Find("selected_artifact_sha256");
    Require(digest->GetType()==V::Text&&HexDigest(digest->GetText()),"BUILD_RECORD_DIGEST selected artifact SHA256 must be lowercase hexadecimal");
    const auto* strict=build.Find("adapter_strict_fp");
    Require(strict->GetType()==V::Boolean&&strict->GetBoolean(),"BUILD_RECORD_STRICT adapter strict-FP declaration must be boolean true");
}
B ReadVerified(const std::string& path,const std::string& hash,U bytes,const char* label) {
    B snapshot;
    try {snapshot=LocalAssetDetail::ReadRegularFile(path,bytes);}
    catch(const std::exception& error){throw std::runtime_error(std::string("missing or mismatched bound ")+label+": "+error.what());}
    Require(snapshot.size()==bytes&&Hash(snapshot)==hash,std::string("missing or mismatched bound ")+label);return snapshot;
}
void VerifyReport(const std::string& path,const LocalAssetProductionIdentity& production,const SourceInfo& source,U nodes,const ReportValues& values,U residentAvailable) {
    const U bytes=ReportBytes(production);
    Require(Add(bytes,ReportScratch(production,nodes,values.geometry.Storage().incidentCells,values.geometry.Storage().outgoingCells))<=residentAvailable,"statistics report resident budget exceeded before allocation");
    const auto report=ReadVerified(path,ReportHash(production),bytes,"statistics report");
    CheckReportBinding(report,source,production,nodes,values);
}
const V& Typed(const V& map,const char* key,V::Type type) {
    const auto* value=map.Find(key);Require(value&&value->GetType()==type,std::string("invalid semantic reconstruction field: ")+key);return *value;
}
LocalBakeConfig Reconstruction(const LocalAssetSemanticIdentity& semantic,const std::vector<double>& nodes,U residentAvailable) {
    LocalBakeConfig c;c.limits.maximumResidentBytes=residentAvailable;c.explicitComplexPackV1=true;c.wavelengthsNm=nodes;
    const auto model=Decode(semantic.model),placement=Decode(semantic.placement),postscatter=Decode(semantic.postscatter),boundary=Decode(semantic.boundary),exterior=Decode(semantic.exterior);
    const auto& fiber=Typed(model,"fiber_parameters",V::Array).GetArray();Require(fiber.size()==3,"invalid fiber reconstruction dimensions");
    for(const auto& value:fiber)Require(value.GetType()==V::Float64,"invalid fiber reconstruction scalar");
    c.fiberRadiusMetres=fiber[0].GetFloat();c.fiberFraction=fiber[1].GetFloat();c.fiberRelativeIOR=fiber[2].GetFloat();
    const auto policy=Typed(placement,"policy",V::Text).GetText();Require(policy=="null_collision_placement_v1"||policy=="fixed_event_diagnostic_v1","unsupported placement reconstruction policy");
    c.placement=policy=="null_collision_placement_v1"?ForwardPlacementPolicy::NullCollisionPlacementV1:ForwardPlacementPolicy::FixedEventDiagnosticV1;
    const auto post=Typed(postscatter,"policy",V::Text).GetText();Require(post==ForwardDiscardPostScatterVersion||post==ForwardDiagnosticPostScatterVersion,"unsupported postscatter reconstruction policy");
    c.postScatter=post==ForwardDiscardPostScatterVersion?ForwardPostScatterPolicy::DiscardAfterScatterV1:ForwardPostScatterPolicy::DiagnosticRetainParticleV1;
    const auto law=Typed(boundary,"law",V::Text).GetText();Require(law=="smooth_planar_dielectric_v1"||law==SurfaceBoundaryVersion,"unsupported boundary reconstruction law");
    c.boundary=law=="smooth_planar_dielectric_v1"?ExperimentBoundary::Smooth:ExperimentBoundary::HyLIoS2015;
    c.exteriorIOR=Typed(exterior,"ior",V::Float64).GetFloat();return c;
}
void SameSemantic(const LocalAssetSemanticIdentity& actual,const LocalAssetSemanticIdentity& expected) {
    const std::pair<const B*,const B*> records[]={{&actual.model,&expected.model},{&actual.placement,&expected.placement},{&actual.postscatter,&expected.postscatter},
        {&actual.boundary,&expected.boundary},{&actual.lineage,&expected.lineage},{&actual.data,&expected.data},{&actual.specimen,&expected.specimen},
        {&actual.exterior,&expected.exterior},{&actual.approximation,&expected.approximation}};
    for(const auto& record:records)Require(*record.first==*record.second,"bound source semantic rederivation mismatch");
    Require(actual.coveredFirstNm==expected.coveredFirstNm&&actual.coveredLastNm==expected.coveredLastNm,"bound source coverage rederivation mismatch");
}
void PublishArtifact(const std::string& path,const B& bytes,const char* label) {
    const auto hash=Hash(bytes);
    if(!std::filesystem::exists(std::filesystem::u8path(path)))
        Require(Implementation::DiskFileWriteBuffer::WriteAtomicallyUTF8(path,[&](Implementation::DiskFileWriteBuffer& out){
            return out.setBytes(bytes.data(),static_cast<unsigned int>(bytes.size()));}),std::string("cannot publish immutable ")+label);
    VerifyArtifact(path,hash,bytes.size(),label);
}

}
LocalBakeFailure::LocalBakeFailure(const std::string& why,U index,U completed,U bulk,U surface,ForwardTermination t):std::runtime_error("HyLIoS local bake refused: "+why),historyIndex(index),completedHistories(completed),bulkWork(bulk),surfaceWork(surface),termination(t){}
namespace LocalBakeDetail {
SlabVector Launch(LocalResponsePoint p){const auto w=HyLIoSLocalResponse::ToDirection(p);return {-w.x,w.y,w.z};}
LocalResponsePoint Escaped(SlabVector d){return HyLIoSLocalResponse::FromDirection({d.x,-d.y,-d.z});}
double DownwardCountRatio(U count,U n){Require(n>0&&n<=(U{1}<<53)&&count<=n,"invalid count ratio");if(!count)return 0;return std::nextafter(static_cast<double>(count)/static_cast<double>(n),0.0);}
double DownwardCoefficient(U count,U n,double area){Require(std::isfinite(area)&&area>=std::numeric_limits<double>::min(),"projected area must be normal binary64 for construction error bound");if(!count)return 0;
    const double upper=std::nextafter(area,std::numeric_limits<double>::infinity());
    const double f=std::nextafter(DownwardCountRatio(count,n)/upper,0.0);
    Require(std::isfinite(f)&&f>0,"positive coefficient unsupported by binary64; cannot discard support");return f;
}
}
LocalAssetSemanticIdentity PrepareLocalBakeSemantic(const std::string& bytes,const LocalBakeConfig& c) {
    Require(std::fegetround()==FE_TONEAREST,"strict statistics profile requires round-to-nearest arithmetic");
    // Physical identity does not require the caller to reproduce an author's
    // grid, seed, sample count, or work budget. Use a tiny fixed preparation
    // envelope; the requested model/exterior/nodes remain unchanged.
    auto semantic=c;semantic.incident={{0,1},{0,LocalResponseTwoPi}};semantic.outgoing=semantic.incident;
    semantic.seed=0;semantic.historiesPerCellPerReplicate=1;semantic.replicates=2;
    semantic.normalGuideHistoriesPerReplicate=1;semantic.maximumWorkPerHistory=1;
    semantic.maximumTotalHistories=std::numeric_limits<U>::max();semantic.maximumTotalWork=std::numeric_limits<U>::max();
    semantic.maximumReportBytes=64*1024*1024;semantic.limits.maximumMetadataBytes=64*1024;
    return Prepare(bytes,semantic,true).semantic;
}
LocalBakeResult BakeLocalResponse(const std::string& bytes,const LocalBakeConfig& c,const LocalBakeExecution& e){
    Require(std::fegetround()==FE_TONEAREST,"strict statistics profile requires round-to-nearest arithmetic");
    Require(e.historyChunkSize>0,"zero execution chunk size");auto p=Prepare(bytes,c);LocalBakeResult result;result.asset.response=std::move(p.blank);result.asset.semantic=p.semantic;result.sourceArtifact.assign(bytes.begin(),bytes.end());
    ValidatePath(c.buildArtifactPath,0);
    const auto artifact=LocalAssetDetail::HashRegularFile(c.buildArtifactPath,U{256}*1024*1024);
    Require(artifact.bytes>0,"caller-selected build artifact must be nonempty");const auto& artifactHash=artifact.sha256;
    std::vector<Row> rows(static_cast<std::size_t>(p.rows));
    for(auto& row:rows){row.counts.resize(static_cast<std::size_t>(c.replicates),std::vector<U>(static_cast<std::size_t>(p.outs+2)));row.bulk.resize(c.replicates);row.surface.resize(c.replicates);}
    for(U chunk=0;chunk<p.total;){const U end=Add(chunk,std::min(e.historyChunkSize,p.total-chunk));
        for(U step=chunk;step<end;++step){const U id=e.reverseTraversal?p.total-1-step:step;const U l=id/p.perNode,within=id%p.perNode;
            const U angular=Mul(Mul(p.cells,c.replicates),c.historiesPerCellPerReplicate);const bool guide=within>=angular;
            const U k=guide?within-angular:within;const U n=guide?c.normalGuideHistoriesPerReplicate:c.historiesPerCellPerReplicate;
            const U i=guide?p.cells:k/Mul(c.replicates,n),rep=(k/n)%c.replicates;
            if(e.hooks.isCancelled&&e.hooks.isCancelled(e.hooks.context))throw LocalBakeFailure("cancelled before incident jitter",id,result.completedHistories,result.bulkWork,result.surfaceWork,ForwardTermination::Cancelled);
            ForwardExperimentRandom rng(c.seed+id);LocalResponsePoint wi{1,0};
            if(!guide){const auto np=c.incident.phiEdges.size()-1;const auto ti=i/np,pi=i%np;
                // Two independent top32 midpoint draws, exactly representable.
                // Fixed two-draw jitter; no unbounded endpoint rejection loop.
                const double u=(std::floor(rng.Get1D()*0x1p32)+.5)*0x1p-32;
                const double v=(std::floor(rng.Get1D()*0x1p32)+.5)*0x1p-32;
                wi={c.incident.tEdges[ti]+u*(c.incident.tEdges[ti+1]-c.incident.tEdges[ti]),c.incident.phiEdges[pi]+v*(c.incident.phiEdges[pi+1]-c.incident.phiEdges[pi])};
                Require(wi.t>c.incident.tEdges[ti]&&wi.t<c.incident.tEdges[ti+1]&&wi.phi>c.incident.phiEdges[pi]&&wi.phi<c.incident.phiEdges[pi+1],"incident grid jitter rounds to boundary; use coarser grid");
            }
            auto state=p.kernels[l]->Begin({0,0,0},LocalBakeDetail::Launch(wi));ForwardSurfaceWork surface;
            const auto diagnostic=DriveForwardExperimentHistory(*p.kernels[l],p.law.get(),state,rng,c.maximumWorkPerHistory,surface,e.hooks);
            result.bulkWork=Add(result.bulkWork,state.Counters().workUnits);result.surfaceWork=Add(result.surfaceWork,surface.workUnits);
            auto fail=[&](const char* why){throw LocalBakeFailure(why,id,result.completedHistories,result.bulkWork,result.surfaceWork,state.Termination());};
            if(diagnostic!=ForwardTermination::None)fail("boundary driver diagnostic");
            if(Add(state.Counters().workUnits,surface.workUnits)>c.maximumWorkPerHistory)fail("shared driver exceeded complete work contract");
            const auto termination=state.Termination();U bin=p.outs+1;
            if(termination==ForwardTermination::ExternalReflection||termination==ForwardTermination::SubsurfaceReturn){
                if(state.Weight()!=1)fail("analog/no-roulette returned weight must equal one");
                if(state.ReturnedAtomMap()==ForwardReturnedAtomMap::PlanarSameExteriorMirror)bin=p.outs;
                else if(state.AngularLineage().classification==ForwardAngularClass::AbsolutelyContinuous)bin=p.geometry->OutgoingCell(LocalBakeDetail::Escaped(state.Direction()));
                else fail("positive unknown angular return; no production asset");
            }else if(termination!=ForwardTermination::Absorbed)fail("unresolved, cancelled, budget, or numeric history");
            auto& row=rows[l*(p.cells+1)+i];++row.counts[rep][bin];row.bulk[rep]=Add(row.bulk[rep],state.Counters().workUnits);row.surface[rep]=Add(row.surface[rep],surface.workUnits);++result.completedHistories;
        }chunk=end;
    }
    V::Values reportRows;double maxHistorySE=0,maxReplicateSE=0,maxEstimatorLoss=0;
    for(U l=0;l<c.wavelengthsNm.size();++l)for(U i=0;i<=p.cells;++i){const bool guide=i==p.cells;const U n=guide?c.normalGuideHistoriesPerReplicate:c.historiesPerCellPerReplicate;const U denominator=Mul(n,c.replicates);
        auto& row=rows[l*(p.cells+1)+i];V::Values replicas;for(const auto& rep:row.counts)replicas.push_back(Counts(rep));V::Values bins;U returns=0;std::vector<U> returned(c.replicates);
        double rowLoss=0;
        for(U j=0;j<p.outs+2;++j){std::vector<U> values;U count=0;for(U r=0;r<c.replicates;++r){values.push_back(row.counts[r][j]);count=Add(count,row.counts[r][j]);if(j<=p.outs)returned[r]=Add(returned[r],row.counts[r][j]);}
            const auto s=Statistics(values,n);bins.push_back(StatValue(s));maxHistorySE=std::max(maxHistorySE,s.historySE);maxReplicateSE=std::max(maxReplicateSE,s.replicateSE);
            if(j<=p.outs)returns=Add(returns,count);
            if(!guide&&j<p.outs){const double f=LocalBakeDetail::DownwardCoefficient(count,denominator,p.geometry->OutgoingCellArea(j));result.asset.response.continuous[(l*p.cells+i)*p.outs+j]=f;
                // Conservative binary64 construction-loss bound: 8 eps per
                // unit returned mass (two divisions/round-down + upper area).
                if(count)rowLoss+=8*std::numeric_limits<double>::epsilon()*static_cast<double>(count)/static_cast<double>(denominator);
            }else if(!guide&&j==p.outs){result.asset.response.mirror[l*p.cells+i]=LocalBakeDetail::DownwardCountRatio(count,denominator);if(count)rowLoss+=2*std::numeric_limits<double>::epsilon();}
        }
        if(returns>0)rowLoss=10*std::numeric_limits<double>::epsilon();
        const auto rs=Statistics(returned,n);maxHistorySE=std::max(maxHistorySE,rs.historySE);maxReplicateSE=std::max(maxReplicateSE,rs.replicateSE);maxEstimatorLoss=std::max(maxEstimatorLoss,rowLoss);
        if(guide)result.asset.normalIncidenceGuide.push_back(LocalBakeDetail::DownwardCountRatio(returns,denominator));
        reportRows.push_back(V::MapValue({{"wavelength_index",V::Unsigned(l)},{"incident_cell_or_normal",V::Unsigned(i)},{"normal_exact",V::Bool(guide)},{"histories_per_replicate",V::Unsigned(n)},{"replicate_bin_counts",V::ArrayValue(replicas)},{"bin_statistics",V::ArrayValue(bins)},{"returned_statistics",StatValue(rs)},{"bulk_work_per_replicate",Counts(row.bulk)},{"surface_work_per_replicate",Counts(row.surface)}}));
    }
    result.report=Encode(V::MapValue({{"schema",String("hylios_local_full_statistics_v2")},{"source_artifact_sha256",V::String(Source(result.asset.semantic).hash)},{"source_artifact_bytes",V::Unsigned(bytes.size())},{"maximum_source_artifact_bytes",V::Unsigned(c.maximumSourceArtifactBytes)},{"maximum_report_bytes",V::Unsigned(c.maximumReportBytes)},{"bin_order",String("continuous_cells,proved_planar_mirror,physical_absorption")},{"rows",V::ArrayValue(reportRows)},{"normal_experiment",String("wi=(0,0,1),separate_indices_v1")},{"completed_histories",V::Unsigned(result.completedHistories)},{"bulk_work",V::Unsigned(result.bulkWork)},{"surface_work",V::Unsigned(result.surfaceWork)},{"maximum_total_histories",V::Unsigned(c.maximumTotalHistories)},{"maximum_total_work",V::Unsigned(c.maximumTotalWork)},{"seed",V::Unsigned(c.seed)},{"rng",String("splitmix64_top53_v1")},{"index",String("l*(I*R*N+R*G)+i*R*N+r*N+h;normal=l*block+I*R*N+r*G+h;seed_plus_index_mod64")},{"angular_discretization",String("cell_constant_projected_top32_midpoint_average;refinement_not_certified")},{"spectral_discretization",String("linear_nodes;heldout_not_certified")},{"maximum_binary64_row_construction_loss_bound",Number(maxEstimatorLoss)}}));
    Require(result.report.size()<=p.reportBound&&result.report.size()<=c.maximumReportBytes,"actual report exceeds proved budget");
    auto& prod=result.asset.production;prod.oracleRevision=ForwardTransportVersion;prod.lineageRevision=ForwardAngularLineageVersion;prod.estimatorRevision="analog_noRR_allN_downward_binary64_division_v1";prod.historyMappingRevision="splitmix64_top53_disjoint_fullgrid_and_exactnormal_index_v1";prod.statisticsRevision="exact_replicate_bin_counts_bernoulli_and_replicate_se_source_bound_v2";
    prod.seed=c.seed;prod.historiesPerCellPerReplicate=c.historiesPerCellPerReplicate;prod.replicates=c.replicates;prod.normalGuideHistoriesPerReplicate=c.normalGuideHistoriesPerReplicate;prod.maximumWorkPerHistory=c.maximumWorkPerHistory;
    prod.build=Record({{"adapter_version",String(LocalBakeVersion)},{"compiler",V::String(CompilerIdentity())},{"selected_artifact_sha256",V::String(artifactHash)},{"adapter_strict_fp",V::Bool(true)}});
    prod.statistics=Record({{"source_artifact_sha256",V::String(Source(result.asset.semantic).hash)},{"source_artifact_bytes",V::Unsigned(bytes.size())},{"full_report_sha256",V::String(Hash(result.report))},{"full_report_bytes",V::Unsigned(result.report.size())},{"full_report_schema",String("hylios_local_full_statistics_v2")},{"completed_histories",V::Unsigned(result.completedHistories)},{"bulk_work",V::Unsigned(result.bulkWork)},{"surface_work",V::Unsigned(result.surfaceWork)},{"maximum_bin_or_row_bernoulli_history_se",Number(maxHistorySE)},{"maximum_bin_or_row_independent_replicate_se",Number(maxReplicateSE)},{"maximum_binary64_row_construction_loss_bound",Number(maxEstimatorLoss)},{"confidence",String("SE_is_diagnostic_not_a_coverage_guarantee;zero_observed_bins_not_proved_zero")},{"refinement",String("not_certified;synthetic_controls_not_skin_validation")}});
    // Exact represented-area passivity is checked before float32 quantization.
    HyLIoSLocalResponse::Create(result.asset.response,c.limits.response);return result;
}
void PublishLocalBake(const std::string& path,const LocalBakeResult& r,const LocalAssetLimits& limits,const std::function<void()>& beforePublication){
    Require(std::fegetround()==FE_TONEAREST,"strict statistics profile requires round-to-nearest arithmetic");
    ValidatePath(path);
    AdmitMetadata(r.asset.semantic,&r.asset.production,limits,
        Add(Add(Add(r.report.size(),r.sourceArtifact.size()),ScalarBytes(r.asset.response)),Mul(r.asset.normalIncidenceGuide.size(),8)));
    Require(r.report.size()==ReportBytes(r.asset.production)&&Hash(r.report)==ReportHash(r.asset.production),"report content does not match production record");
    const auto source=Source(r.asset.semantic);CheckProductionSource(r.asset.production,source);
    Require(r.sourceArtifact.size()==source.bytes&&Hash(r.sourceArtifact)==source.hash,"source content does not match independently prepared semantic record");
    {
        const auto& response=r.asset.response;
        const auto storage=HyLIoSLocalResponse::EstimateStorage(response.wavelengthNm.size(),response.incident.tEdges.size()-1,
            response.incident.phiEdges.size()-1,response.outgoing.tEdges.size()-1,response.outgoing.phiEdges.size()-1,limits.response);
        const U retained=Add(Add(storage.storageBytes,r.report.size()),r.sourceArtifact.size());
        Require(retained<limits.maximumResidentBytes,"source publication reconstruction resident budget exceeded");
        const std::string exactSource(r.sourceArtifact.begin(),r.sourceArtifact.end());
        SameSemantic(PrepareLocalBakeSemantic(exactSource,Reconstruction(r.asset.semantic,response.wavelengthNm,limits.maximumResidentBytes-retained)),r.asset.semantic);
    }
    const auto geometry=HyLIoSLocalResponse::Create(r.asset.response,limits.response);
    const U cells=geometry->Storage().incidentCells,outgoing=geometry->Storage().outgoingCells;
    U held=Add(Add(geometry->Storage().storageBytes,r.report.size()),r.sourceArtifact.size());
    held=Add(held,Mul(Add(Add(r.asset.response.continuous.size(),r.asset.response.mirror.size()),r.asset.normalIncidenceGuide.size()),8));
    Require(Add(held,ReportScratch(r.asset.production,r.asset.response.wavelengthNm.size(),cells,outgoing))<=limits.maximumResidentBytes,"report validation scratch budget exceeded");
    ReportValues values{*geometry,r.asset.normalIncidenceGuide,false,
        [&](U l,U i,U j){return r.asset.response.continuous.at((l*cells+i)*outgoing+j);},
        [&](U l,U i){return r.asset.response.mirror.at(l*cells+i);}};
    const auto summary=CheckReportBinding(r.report,source,r.asset.production,r.asset.response.wavelengthNm.size(),values);
    Require(r.completedHistories==ExpectedHistories(r.asset.production,r.asset.response.wavelengthNm.size(),cells)&&r.bulkWork==summary.bulk&&r.surfaceWork==summary.surface,
        "result/report history and work counters mismatch");
    PublishArtifact(SourcePath(path,source),r.sourceArtifact,"source artifact");
    const auto reportPath=ReportPath(path,Hash(r.report));
    PublishArtifact(reportPath,r.report,"statistics report");
    LocalAssetDetail::SaveObserved(path,r.asset,limits,beforePublication);
}
std::shared_ptr<const HyLIoSLocalResponseAsset> LoadLocalBake(const std::string& path,const LocalAssetSemanticIdentity& expected,const LocalAssetLimits& limits) {
    Require(std::fegetround()==FE_TONEAREST,"strict statistics profile requires round-to-nearest arithmetic");
    ValidatePath(path);
    AdmitMetadata(expected,nullptr,limits,0);
    auto asset=LoadLocalResponseAsset(path,expected,limits);
    U retained=Add(asset->Response().Storage().storageBytes,Mul(asset->NormalIncidenceGuide().size(),8));
    retained=Add(retained,Mul(asset->WavelengthsNm().size(),8));
    AdmitMetadata(asset->Semantic(),&asset->Production(),limits,retained);
    const auto source=Source(asset->Semantic());
    CheckProductionSource(asset->Production(),source);
    Require(asset->PeakResidentBytesBound()<=limits.maximumResidentBytes,"asset resident bound exceeds load budget");
    const U residentAvailable=limits.maximumResidentBytes-asset->PeakResidentBytesBound();
    Require(Add(Mul(source.bytes,256),1024*1024)<=residentAvailable,"source semantic reconstruction resident budget exceeded before allocation");
    {
        const auto snapshot=ReadVerified(SourcePath(path,source),source.hash,source.bytes,"source artifact");
        const std::string exactSource(snapshot.begin(),snapshot.end());
        const auto independentlyResolved=PrepareLocalBakeSemantic(exactSource,Reconstruction(asset->Semantic(),asset->WavelengthsNm(),residentAvailable));
        SameSemantic(independentlyResolved,asset->Semantic());
    }
    ReportValues values{asset->Response(),asset->NormalIncidenceGuide(),true,
        [&](U l,U i,U j){return asset->Response().StoredContinuous(l,i,j);},
        [&](U l,U i){return asset->Response().StoredMirror(l,i);},&asset->EncodingError()};
    VerifyReport(ReportPath(path,ReportHash(asset->Production())),asset->Production(),source,asset->WavelengthsNm().size(),values,residentAvailable);
    return asset;
}
}
