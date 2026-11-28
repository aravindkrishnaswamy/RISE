//////////////////////////////////////////////////////////////////////
//
//  FireSequenceCache.cpp - Sealed fire sequence to native cache bridge
//
//  Implements process-wide fire cache resolver lookup, POSIX helper
//  spawning, sealed-sequence import into Simulation::CacheSnapshot
//  (grid.v1) with CBOR receipts, native solver-cache attachment, and
//  dense renderer frame projection with physical channel checks.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireSequenceCache.h"
#include "FireSequence.h"
#include "../Simulation/SimulationCache.h"
#include "../Simulation/SimulationGrid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if (defined(__APPLE__) && TARGET_OS_OSX) || (defined(__linux__) && !defined(__ANDROID__))
#define RISE_DESKTOP_POSIX_IMPORTER 1
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace RISE { namespace Implementation {
namespace {
namespace fs = std::filesystem;
using namespace Simulation;
using Value = RISECBOR64::Value;
// ------------------------------------------------------------------
// Budgets, converter constants, and default platform resolver lookup
// ------------------------------------------------------------------
constexpr std::size_t manifestBudget = 64u * 1024 * 1024;
constexpr std::size_t frameBudget = 1024u * 1024 * 1024;
constexpr char converter[] = "rise.fire.vdb-import";
constexpr char version[] = "1";
constexpr char volume[] = "volume";
std::mutex resolverMutex;
FireCacheResolver registeredResolver;

bool Fail(std::string& error, const std::string& message)
{
	error=message;
	return false;
}

#if defined(RISE_ENABLE_OPENVDB) && defined(RISE_DESKTOP_POSIX_IMPORTER)
fs::path LocalImportHelper(std::string& error)
{
	std::error_code ec;
	fs::path exe;
#if defined(__APPLE__)
	std::uint32_t size=0;
	_NSGetExecutablePath(nullptr,&size);
	std::vector<char> path(size);
	if(!size || _NSGetExecutablePath(path.data(),&size)!=0){error="cannot locate RISE executable";return {};}
	exe=fs::canonical(path.data(),ec);
#else
	exe=fs::canonical("/proc/self/exe",ec);
#endif
	if(ec){error="cannot resolve RISE executable: "+ec.message();return {};}
	auto bin=exe.parent_path();
	if(bin.filename()=="tests" || bin.filename()=="tools") bin=bin.parent_path();
	return bin/"tools"/"simulation_cache_import";
}

#endif

FireCacheResolver GetResolver()
{
	std::lock_guard<std::mutex> lock(resolverMutex);
	if(registeredResolver) return registeredResolver;
#if defined(RISE_ENABLE_OPENVDB)
	return [](const std::string& manifest,const std::string& digest,std::optional<std::int64_t> index,
		std::string& directory,std::string& verified,std::string& error) {
		fs::path root;
		if(const char* overrideRoot=std::getenv("RISE_SIMULATION_CACHE_DIR"); overrideRoot && *overrideRoot) root=overrideRoot;
#if defined(_WIN32)
		else if(const char* appData=std::getenv("LOCALAPPDATA")) root=fs::path(appData)/"RISE"/"Simulation";
#elif defined(__APPLE__)
		else if(const char* home=std::getenv("HOME")) root=fs::path(home)/"Library"/"Caches"/"RISE"/"Simulation";
#else
		else if(const char* xdg=std::getenv("XDG_CACHE_HOME")) root=fs::path(xdg)/"rise"/"simulation";
		else if(const char* home=std::getenv("HOME")) root=fs::path(home)/".cache"/"rise"/"simulation";
#endif
		if(root.empty()) return Fail(error,"cannot resolve writable simulation cache root");
		directory=(root/digest).string();

#if !defined(RISE_DESKTOP_POSIX_IMPORTER)
		(void)manifest; (void)index; (void)verified;
		return Fail(error,"fire sequence import worker unavailable on this platform");
#else
		const auto helper=LocalImportHelper(error);
		return !helper.empty() && ResolveFireCacheWithHelper(helper.string(),directory,manifest,digest,index,verified,error);
#endif
	};
#else
	return {};
#endif
}

// ------------------------------------------------------------------
// Sealed envelope I/O, cache description mapping, and import receipts
// ------------------------------------------------------------------
bool ReadEnvelope(const fs::path& path, Bytes& data, std::string& error)
{
	std::ifstream file(path,std::ios::binary | std::ios::ate);
	const std::streamoff size=file.tellg();
	if(size<1 || size>static_cast<std::streamoff>(manifestBudget))
		return Fail(error,"fire sequence envelope unreadable or over budget");
	data.resize(static_cast<std::size_t>(size));
	file.seekg(0);
	return (file.read(reinterpret_cast<char*>(data.data()),size) &&
		file.peek()==std::char_traits<char>::eof() && !file.bad()) ||
		Fail(error,"truncated fire sequence envelope");
}

double TimeAt(const FireSequenceManifest& source, std::int64_t index)
{
	const auto& map=source.TimeMap();
	return map.simulationTimeOrigin+
		static_cast<double>(index-map.firstFrameIndex)*map.frameStepSeconds;
}

bool DescribeImport(const FireSequenceManifest& source, const Bytes& envelope,
	CacheDescription& d, std::string& error)
{
	d.producer=converter;
	d.producerVersion=version;
	if(!MakeCacheKey(converter,version,Value::MapValue({}),
		{{"source_manifest",RISECBOR64::SHA256Hex(envelope)}},d.key,error)) return false;
	d.epoch=d.key;
	d.physicalRecords=envelope;
	d.fidelity=source.SourceQualification()=="predictive_qualified" ? "predictive" : "preview";
	d.evidence=source.GateEvidenceIds();
	const auto& map=source.TimeMap();
	d.startTime=map.simulationTimeOrigin;
	d.sceneOrigin=map.sceneTimeOrigin;
	d.timeScale=map.sceneToSimulationScale;
	d.interval=map.frameStepSeconds;
	d.firstIndex=map.firstFrameIndex;
	d.count=source.Frames().size();
	d.hold=source.EndPolicy()=="hold";
	d.outputs={{volume,"grid.v1",{}}};
	for(const auto& c:source.Channels()) d.outputs[0].channels.push_back({c.name,c.units,c.valueType});
	return true;
}

bool MatchesDescription(const CacheDescription& d, const CacheDescription& expected)
{
	if(d.key!=expected.key || d.epoch!=expected.epoch || d.producer!=converter ||
		d.producerVersion!=version || d.fidelity!=expected.fidelity || d.evidence!=expected.evidence ||
		d.startTime!=expected.startTime || d.sceneOrigin!=expected.sceneOrigin ||
		d.timeScale!=expected.timeScale || d.interval!=expected.interval ||
		d.firstIndex!=expected.firstIndex || d.count!=expected.count || d.hold!=expected.hold ||
		d.outputs.size()!=1 || d.outputs.front().name!=volume || d.outputs.front().type!="grid.v1")
		return false;
	const auto& channels=d.outputs.front().channels;
	const auto& want=expected.outputs.front().channels;
	if(channels.size()!=want.size()) return false;
	for(std::size_t i=0;i<want.size();++i)
		if(channels[i].name!=want[i].name || channels[i].units!=want[i].units ||
			channels[i].representation!=want[i].representation) return false;
	return true;
}

bool Receipt(const std::string& envelopeDigest, const std::string& sequenceId,
	const FireSequenceFrameDescriptor& source, const std::string& nativeDigest,
	Bytes& receipt, std::string& error)
{
	return RISECBOR64::Encode(Value::MapValue({
		{"converter",Value::String(converter)},{"version",Value::String(version)},
		{"format",Value::String("grid.v1")},{"source_envelope",Value::String(envelopeDigest)},
		{"sequence",Value::String(sequenceId)},{"ordinal",Value::Signed(source.index)},
		{"source_frame",Value::String(source.sha256)},{"native_frame",Value::String(nativeDigest)}
	}),receipt,&error);
}

// Verifies that every entry in the native snapshot matches the sealed manifest's
// frame cadence and reproduces the deterministic CBOR import receipt.
bool MatchesEntries(const CacheSnapshot& snap, const FireSequenceManifest& source,
	const std::string& envelopeDigest, std::string& error)
{
	for(const auto& e:snap.Entries()){
		// CacheSnapshot validates the ordinal against the declared signed interval.
		const auto offset=static_cast<std::uint64_t>(e.index)-static_cast<std::uint64_t>(source.TimeMap().firstFrameIndex);
		Bytes receipt;
		if(offset>=source.Frames().size() || e.output!=volume || e.time!=TimeAt(source,e.index) ||
			!Receipt(envelopeDigest,source.SequenceId(),source.Frames()[offset],e.digest,receipt,error) ||
			e.receipt!=receipt) return Fail(error,"native fire import receipt differs from the sealed source");
	}
	return true;
}

// ------------------------------------------------------------------
// Native GridFrame lattice validation and dense renderer projection
// ------------------------------------------------------------------
float FromBits(std::uint32_t bits)
{
	float value;
	std::memcpy(&value,&bits,sizeof(value));
	return value;
}

bool EqualBits(double lhs,double rhs) { return std::memcmp(&lhs,&rhs,sizeof(double))==0; }

bool CheckLattice(const GridChannel& grid, const FireSequenceChannelDescriptor& d, std::string& error)
{
	if(grid.components!=d.background.size() || grid.units!=d.units)
		return Fail(error,"native fire channel representation or units differ");
	for(unsigned c=0;c<grid.components;++c)
		if(!EqualBits(FromBits(grid.background[c]),d.background[c]))
			return Fail(error,"native fire channel background differs");
	for(unsigned r=0;r<3;++r){
		if(grid.origin[r]!=0 || grid.dimensions[r]!=d.dimensions[r] ||
			!EqualBits(grid.indexToWorld[4*r+3],d.originMeters[r])) return Fail(error,"native fire lattice differs");
		for(unsigned c=0;c<3;++c){
			const double origin=grid.indexToWorld[4*r+3];
			const double step=(origin+grid.indexToWorld[4*r+c])-origin;
			if(!EqualBits(step,r==c ? d.voxelSizeMeters[c] : 0.0)) return Fail(error,"native fire transform differs");
		}
	}
	return true;
}

// Expands a native sparse GridFrame (or background-only frame when grid == nullptr)
// into dense double channels under frameBudget, enforcing bit-exact inactive
// background values, certified temperature bounds, and non-negativity.
bool Project(const FireSequenceManifest& source,const GridFrame* grid,
	FireSequencePreparedFrame& result,std::string& error)
{
	// Budget the complete dense rendering projection, including the previous
	// installed frame and the medium's new accessor/majorant preparation.
	std::size_t remaining=frameBudget/sizeof(double);
	if(grid && grid->channels.size()!=source.Channels().size()) return Fail(error,"native fire channel set differs");
	for(const auto& d:source.Channels()){
		const auto components=d.background.size();
		std::size_t count=1;
		for(const auto n:d.dimensions){
			if(!n || n>remaining/count) return Fail(error,"native fire projection exceeds byte budget");
			count*=static_cast<std::size_t>(n);
		}
		if(!components || count>remaining/components) return Fail(error,"native fire projection exceeds byte budget");
		remaining-=count*components;
		FireSequenceDenseChannel ch;
		ch.name=d.name;
		ch.dimensions=d.dimensions;
		ch.vectorValues=components==3;
		ch.minimum=*std::min_element(d.background.begin(),d.background.end());
		ch.maximum=*std::max_element(d.background.begin(),d.background.end());
		ch.values.resize(count*components);
		for(std::size_t i=0;i<ch.values.size();++i) ch.values[i]=d.background[i%components];
		if(grid){
			const auto it=std::find_if(grid->channels.begin(),grid->channels.end(),
				[&](const GridChannel& g){return g.name==d.name;});
			if(it==grid->channels.end()) return Fail(error,"native fire channel missing");
			if(!CheckLattice(*it,d,error)) return false;
			for(const auto& b:it->blocks){
				std::size_t k=0;
				for(unsigned z=0;z<b.dimensions[2];++z) for(unsigned y=0;y<b.dimensions[1];++y)
					for(unsigned x=0;x<b.dimensions[0];++x,++k){
						const auto index=(std::size_t(b.origin[0])+x)+d.dimensions[0]*
							((std::size_t(b.origin[1])+y)+d.dimensions[1]*(std::size_t(b.origin[2])+z));
						const std::size_t j=b.constant ? 0 : k;
						const bool active=(b.active[j/8] & (1u<<(j%8)))!=0;
						for(std::size_t c=0;c<components;++c){
							const double value=FromBits(b.values[components*j+c]);
							if(!std::isfinite(value) || (!active && !EqualBits(value,d.background[c])) ||
								(d.name=="temperature" && (value<=0 || value<source.TemperatureDomainMinimumK() ||
									value>source.TemperatureDomainMaximumK())) ||
								(components==1 && d.name!="temperature" && value<0))
								return Fail(error,"native fire stored value violates physical channel constraints");
							ch.values[index*components+c]=value;
							ch.minimum=std::min(ch.minimum,value);
							ch.maximum=std::max(ch.maximum,value);
						}
					}
			}
		}
		result.channels.emplace(ch.name,std::move(ch));
	}
	return true;
}
} // namespace

// ==================================================================
// Out-of-process import helper and resolver registration
// ==================================================================
// Spawns the external import helper on desktop POSIX with a sanitized PATH and
// parses its trailing 64-hex verified cache generation digest from stdout/stderr.
bool ResolveFireCacheWithHelper(const std::string& helperPath,const std::string& directory,
	const std::string& manifest,const std::string& digest,std::optional<std::int64_t> index,
	std::string& verified,std::string& error)
{
#if defined(RISE_DESKTOP_POSIX_IMPORTER)
	std::string program=helperPath, operation="legacy", source=manifest, cache=directory,
		expected=digest, ordinal=index?std::to_string(*index):"all";
	char* args[]={program.data(),operation.data(),source.data(),cache.data(),expected.data(),ordinal.data(),nullptr};
	char pathEnv[]="PATH=/usr/bin:/bin";
	char* environment[]={pathEnv,nullptr};
	int fds[2];
	if(::pipe(fds)!=0) return Fail(error,"cannot open fire importer output pipe");
	posix_spawn_file_actions_t actions;
	const int init=posix_spawn_file_actions_init(&actions);
	int rc=init;
	if(!rc) rc=posix_spawn_file_actions_addclose(&actions,fds[0]);
	if(!rc) rc=posix_spawn_file_actions_adddup2(&actions,fds[1],STDOUT_FILENO);
	if(!rc) rc=posix_spawn_file_actions_adddup2(&actions,fds[1],STDERR_FILENO);
	if(!rc && fds[1]!=STDERR_FILENO && fds[1]!=STDOUT_FILENO) rc=posix_spawn_file_actions_addclose(&actions,fds[1]);
	pid_t child=0;
	if(!rc) rc=posix_spawn(&child,program.c_str(),&actions,nullptr,args,environment);
	if(!init) posix_spawn_file_actions_destroy(&actions);
	::close(fds[1]);
	if(rc){::close(fds[0]);return Fail(error,"cannot launch "+program+": "+std::strerror(rc));}
	char buf[4096];
	std::string reply;
	ssize_t n;
	while((n=::read(fds[0],buf,sizeof(buf)))!=0){
		if(n<0){if(errno==EINTR) continue;break;}
		reply.append(buf,static_cast<std::size_t>(n));
		if(reply.size()>4096) reply.erase(0,reply.size()-4096);
	}
	::close(fds[0]);
	int status=0;
	pid_t waited;
	do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
	if(n<0 || waited!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=0)
		return Fail(error,"fire importer failed: "+reply);
	if(reply.size()<65 || reply.back()!='\n' || (reply.size()>65 && reply[reply.size()-66]!='\n'))
		return Fail(error,"fire importer returned no generation digest");
	verified=reply.substr(reply.size()-65,64);
	return verified.find_first_not_of("0123456789abcdef")==std::string::npos ||
		Fail(error,"fire importer returned an invalid generation digest");
#else
	(void)helperPath; (void)directory; (void)manifest; (void)digest; (void)index; (void)verified;
	return Fail(error,"fire sequence import worker unavailable on this platform");
#endif
}

void SetFireCacheResolver(FireCacheResolver resolver)
{
	std::lock_guard<std::mutex> lock(resolverMutex);
	registeredResolver=std::move(resolver);
}

// ==================================================================
// FireSequenceManifest native-cache attachment and playback methods
// ==================================================================
// Loads either a sealed sequence manifest (auto-importing its first frame when a
// resolver exists) or falls back to attaching a native cache manifest.cbor.
bool FireSequenceManifest::LoadPlaybackFile(const std::string& path,std::string& error)
{
	*this=FireSequenceManifest();
	const fs::path file(path);
	Bytes envelope;
	if(!ReadEnvelope(file,envelope,error)) return false;
	if(!LoadCanonicalEnvelope(envelope,file.parent_path().string(),error)){
		if(file.filename()!="manifest.cbor") return false;
		error.clear();
		return AttachCache(file.parent_path().empty()?".":file.parent_path().string(),error);
	}
	originalManifestPath_=file.string();
	envelopeDigest_=RISECBOR64::SHA256Hex(envelope);
	if(!GetResolver()) return true; // Existing explicit capability error comes from LoadFrame.
	needsImport_=true;
	if(!ResolveImportedFrame(TimeMap().firstFrameIndex,error)) { valid_=false; return false; }
	return true;
}

// Attaches either a solver-baked cache (FireMethaneSolverId) or an imported
// vdb-import cache, verifying description/receipt integrity and source digest.
bool FireSequenceManifest::AttachCache(const std::string& directory,std::string& error)
{
	auto snap=std::make_shared<Simulation::CacheSnapshot>();
	if(!Simulation::CacheSnapshot::Open(directory,manifestBudget,*snap,error)) return false;
	const auto& d=snap->Description();
	if(d.producer==FireMethaneSolverId) return AttachSolverCache(snap,directory,error);
	if(d.producer!=converter || d.producerVersion!=version)
		return Fail(error,"native cache does not carry a supported fire source envelope");
	const auto digest=RISECBOR64::SHA256Hex(d.physicalRecords);
	if(!envelopeDigest_.empty() && digest!=envelopeDigest_)
		return Fail(error,"native cache source changed; reopen the simulation");
	FireSequenceManifest candidate;
	Simulation::CacheDescription expected;
	if(!candidate.LoadCanonicalEnvelope(d.physicalRecords,"",error) ||
		!DescribeImport(candidate,d.physicalRecords,expected,error) ||
		!MatchesDescription(d,expected) || !MatchesEntries(*snap,candidate,digest,error))
		return Fail(error,"native fire cache description or import receipt is invalid: "+error);
	candidate.originalManifestPath_=originalManifestPath_;
	candidate.needsImport_=needsImport_;
	candidate.qualifiedGeneration_=qualifiedGeneration_;
	candidate.nativeDirectory_=directory;
	candidate.envelopeDigest_=digest;
	candidate.native_=std::move(snap);
	// A standalone imported cache has no independently verified original preimage.
	if(candidate.originalManifestPath_.empty()) candidate.sourceQualification_="preview_only";
	*this=std::move(candidate);
	return true;
}

// A cache baked by the fire solver (rise-sim, M2 track C): its physical records
// are the sequence envelope without a frame list, its frames are the solver's
// grid.v1 payloads, and each receipt names the legacy VDB frame the payload was
// converted from. Such a cache has no original sealed manifest, so it is
// preview-only, like a standalone import.
bool FireSequenceManifest::AttachSolverCache(const std::shared_ptr<Simulation::CacheSnapshot>& snap,
	const std::string& directory,std::string& error)
{
	const auto& d=snap->Description();
	// Versions 1 and 2 use the same grid and receipt formats and remain playable.
	if(!FireMethaneSolverCacheVersionPlayable(d.producerVersion)) return Fail(error,"unsupported fire solver cache version");
	const auto digest=RISECBOR64::SHA256Hex(d.physicalRecords);
	if(!envelopeDigest_.empty() && digest!=envelopeDigest_)
		return Fail(error,"native cache source changed; reopen the simulation");
	FireSequenceManifest candidate;
	if(!candidate.LoadFrameTemplate(d.physicalRecords,error))
		return Fail(error,"fire solver cache physical record is invalid: "+error);
	const auto& map=candidate.timeMap_;
	bool matches=d.fidelity=="preview" && candidate.sourceQualification_=="preview_only" && d.count>0 &&
		d.startTime==map.simulationTimeOrigin && d.interval==map.frameStepSeconds &&
		d.firstIndex==map.firstFrameIndex && d.sceneOrigin==map.sceneTimeOrigin &&
		d.timeScale==map.sceneToSimulationScale && d.hold==(candidate.endPolicy_=="hold") &&
		d.outputs.size()==1 && d.outputs.front().name==volume && d.outputs.front().type=="grid.v1" &&
		d.outputs.front().channels.size()==candidate.channels_.size() &&
		d.count<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-std::max<std::int64_t>(0,d.firstIndex));
	for(std::size_t i=0;matches && i<candidate.channels_.size();++i){
		const auto& c=d.outputs.front().channels[i];
		const auto& want=candidate.channels_[i];
		matches=c.name==want.name && c.units==want.units && c.representation==want.valueType;
	}
	if(!matches) return Fail(error,"fire solver cache description differs from its physical record");
	candidate.frames_.clear();
	candidate.frames_.reserve(static_cast<std::size_t>(d.count));
	for(std::uint64_t k=0;k<d.count;++k){
		FireSequenceFrameDescriptor frame;
		frame.index=d.firstIndex+static_cast<std::int64_t>(k);
		candidate.frames_.push_back(frame);
	}
	for(const auto& e:snap->Entries()){
		Value receipt;
		const Value *format=nullptr,*legacy=nullptr;
		const auto offset=static_cast<std::uint64_t>(e.index)-static_cast<std::uint64_t>(d.firstIndex);
		if(e.output!=volume || offset>=d.count || !RISECBOR64::DecodeCanonical(e.receipt,receipt,&error) ||
			receipt.GetType()!=Value::Map || !(format=receipt.Find("format")) || format->GetType()!=Value::Text ||
			format->GetText()!=FireMethaneFrameReceiptFormat || !(legacy=receipt.Find("legacy_sha256")) ||
			legacy->GetType()!=Value::Text || legacy->GetText().size()!=64 ||
			legacy->GetText().find_first_not_of("0123456789abcdef")!=std::string::npos)
			return Fail(error,"fire solver cache frame receipt is invalid");
		candidate.frames_[static_cast<std::size_t>(offset)].sha256=legacy->GetText();
	}
	candidate.frameTemplate_=false;
	candidate.valid_=true;
	candidate.originalManifestPath_=originalManifestPath_;
	candidate.needsImport_=false;
	candidate.qualifiedGeneration_=qualifiedGeneration_;
	candidate.nativeDirectory_=directory;
	candidate.envelopeDigest_=digest;
	candidate.native_=snap;
	*this=std::move(candidate);
	return true;
}

// Concurrent fills may advance the pointer between helper exit and reopening it.
// Retry only while coverage makes forward progress; never accept an unverified digest.
bool FireSequenceManifest::ResolveImportedFrame(std::int64_t index,std::string& error)
{
	const auto resolver=GetResolver();
	if(!resolver) return Fail(error,"fire cache importer unavailable");
	std::optional<std::size_t> lastCoverage;
	for(;;){
		std::string directory,verified;
		if(!resolver(originalManifestPath_,envelopeDigest_,index,directory,verified,error) ||
			!AttachCache(directory,error)) return false;
		if(CoverageDigest()==verified) return true;
		const std::size_t count=native_->Entries().size();
		if(lastCoverage && count<=*lastCoverage) return Fail(error,"fire cache replaced during verified import");
		lastCoverage=count;
	}
}

bool FireSequenceManifest::AcquirePlaybackSnapshot(std::int64_t index, FireSequenceManifest& snapshot,
	std::string& error) const
{
	snapshot=*this;
	const auto directory=nativeDirectory_;
	if(!native_) return true;
	if(!snapshot.AttachCache(directory,error)) return false;
	if(!qualifiedGeneration_.empty() && snapshot.CoverageDigest()!=qualifiedGeneration_)
		return Fail(error,"fire cache generation differs from predictive preflight");
	if(!needsImport_) return true;
	const auto& entries=snapshot.native_->Entries();
	if(std::any_of(entries.begin(),entries.end(),[index](const auto& e){return e.index==index;}))
		return true; // Read/Decode will verify the native bytes against this pinned generation.
	return snapshot.ResolveImportedFrame(index,error);
}

bool FireSequenceManifest::IsComplete() const { return valid_ && (!native_ || native_->Complete()); }
const std::string& FireSequenceManifest::CoverageDigest() const
{
	return native_ ? native_->Digest() : sequenceId_;
}

bool FireSequenceManifest::LoadSourceGrid(std::int64_t index,Simulation::GridFrame& grid,
	std::string& error) const
{
	if(native_) return Fail(error,"fire import requires original VDB bytes");
	return ReadVDBFrame(index,nullptr,&grid,error);
}

bool FireSequenceManifest::LoadFrame(std::int64_t index,FireSequencePreparedFrame& frame,
	std::string& error) const
{
	if(native_) return LoadNativeFrame(index,frame,error);
	return ReadVDBFrame(index,&frame,nullptr,error);
}

// Decodes a native grid.v1 frame into dense renderer channels; for incomplete
// caches an Unavailable frame yields a background-filled frame with available=false.
bool FireSequenceManifest::LoadNativeFrame(std::int64_t index,FireSequencePreparedFrame& frame,
	std::string& error) const
{
	const auto offset=static_cast<std::uint64_t>(index)-static_cast<std::uint64_t>(timeMap_.firstFrameIndex);
	if(offset>=frames_.size()) return Fail(error,"fire frame is outside authored support");
	std::shared_ptr<const Bytes> bytes;
	const auto status=native_->Read(index,volume,frameBudget,bytes,error);
	Simulation::GridFrame grid;
	FireSequencePreparedFrame candidate;
	candidate.frameIndex=index;
	candidate.coverageDigest=native_->Digest();
	if(status==Simulation::FrameStatus::Ready){
		if(!Simulation::DecodeGrid(*bytes,frameBudget,grid,error) || !Project(*this,&grid,candidate,error)) return false;
		candidate.wholeFileSha256=frames_[offset].sha256;
	} else if(status==Simulation::FrameStatus::Unavailable && !native_->Complete()){
		candidate.available=false;
		error.clear();
		if(!Project(*this,nullptr,candidate,error)) return false;
	} else return Fail(error,"native fire frame refused: "+error);
	frame=std::move(candidate);
	return true;
}

bool FireSequenceManifest::PreflightAllFrames(std::string& error) const
{
	if(!valid_) return Fail(error,"fire sequence manifest is invalid");
	if(needsImport_ && !IsComplete()){
		std::string directory,verified;
		const auto resolver=GetResolver();
		auto snap=*this;
		if(!resolver || !resolver(originalManifestPath_,envelopeDigest_,std::nullopt,directory,verified,error) ||
			!snap.AttachCache(directory,error) || snap.CoverageDigest()!=verified)
			return Fail(error,"full fire import verification failed: "+error);
		snap.needsImport_=false;
		return snap.PreflightAllFrames(error);
	}
	if(!IsComplete()) return Fail(error,"fire sequence cache is incomplete");
	for(const auto& descriptor:frames_){
		FireSequencePreparedFrame frame;
		if(!LoadFrame(descriptor.index,frame,error)) return false;
	}
	return true;
}

bool FireSequenceManifest::PreflightPredictive(std::string& error)
{
	if(SourceQualification()!="predictive_qualified") return Fail(error,"fire producer is unqualified");
	if(!native_) return PreflightAllFrames(error);
	const auto resolver=GetResolver();
	std::string directory,verified;
	if(originalManifestPath_.empty() || !resolver ||
		!resolver(originalManifestPath_,envelopeDigest_,std::nullopt,directory,verified,error))
		return Fail(error,"predictive fire playback requires verified original VDB input: "+error);
	auto snapshot=*this;
	if(!snapshot.AttachCache(directory,error) || snapshot.CoverageDigest()!=verified ||
		!snapshot.IsComplete() || !snapshot.PreflightAllFrames(error))
		return Fail(error,"predictive cache generation failed verification: "+error);
	snapshot.qualifiedGeneration_=verified;
	*this=std::move(snapshot);
	return true;
}

// ==================================================================
// Sealed OpenVDB-to-native-cache importer
// ==================================================================
// Materializes one ordinal or all frames from a sealed manifest into grid.v1
// cache entries with receipts; re-verifies existing bytes on already-sealed caches
// and seals once all authored frames are present.
bool ImportFireSequenceCache(const std::string& sourceManifest,const std::string& cacheDirectory,
	std::string& error,std::optional<std::int64_t> requestedOrdinal,const std::string& expectedEnvelopeDigest,
	std::string* verifiedGenerationDigest)
{
	using namespace Simulation;
	Bytes envelope;
	if(!ReadEnvelope(sourceManifest,envelope,error)) return false;
	const auto envelopeDigest=RISECBOR64::SHA256Hex(envelope);
	if(!expectedEnvelopeDigest.empty() && expectedEnvelopeDigest!=envelopeDigest)
		return Fail(error,"fire source manifest changed during import");
	FireSequenceManifest source;
	CacheDescription d;
	if(!source.LoadCanonicalEnvelope(envelope,fs::path(sourceManifest).parent_path().string(),error) ||
		!DescribeImport(source,envelope,d,error)) return false;
	if(requestedOrdinal && (*requestedOrdinal<source.Frames().front().index ||
		*requestedOrdinal>source.Frames().back().index)) return Fail(error,"requested fire import ordinal is outside the source span");
	const fs::path root(cacheDirectory);
	CacheWriter writer;
	if(!writer.PrepareExtend(root,d,frameBudget,error)) return false;
	const auto& current=writer.Snapshot();
	if(!MatchesEntries(current,source,envelopeDigest,error)) return false;
	if(requestedOrdinal) {
		const auto& entries=current.Entries();
		const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.index==*requestedOrdinal;});
		if(found!=entries.end()) {
			std::shared_ptr<const Bytes> bytes;
			if(current.Read(*requestedOrdinal,volume,frameBudget,bytes,error)!=FrameStatus::Ready) return false;
			if(!current.Complete() && entries.size()==d.count && !writer.Seal(error)) return false;
			if(verifiedGenerationDigest) *verifiedGenerationDigest=writer.Snapshot().Digest();
			return true;
		}
	}
	for(const auto& f:source.Frames()){
		if(requestedOrdinal && *requestedOrdinal!=f.index) continue;
		GridFrame grid;
		Bytes bytes,receipt;
		if(!source.LoadSourceGrid(f.index,grid,error) || !EncodeGrid(grid,frameBudget,bytes,error) ||
			!Receipt(envelopeDigest,source.SequenceId(),f,RISECBOR64::SHA256Hex(bytes),receipt,error)) return false;
		if(writer.Snapshot().Complete()){
			std::shared_ptr<const Bytes> stored;
			if(writer.Snapshot().Read(f.index,volume,frameBudget,stored,error)!=FrameStatus::Ready ||
				*stored!=bytes) return Fail(error,"stored native fire frame differs from verified original");
		} else if(!writer.Append(f.index,volume,TimeAt(source,f.index),bytes,error,receipt)) return false;
	}
	if(!writer.Snapshot().Complete() && writer.Snapshot().Entries().size()==d.count && !writer.Seal(error)) return false;
	if(verifiedGenerationDigest)
		*verifiedGenerationDigest=writer.Snapshot().Digest();
	return true;
}

} }
