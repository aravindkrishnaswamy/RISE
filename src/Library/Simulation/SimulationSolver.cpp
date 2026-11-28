//////////////////////////////////////////////////////////////////////
//
//  SimulationSolver.cpp - Solver registry, verification evidence, and port relays
//
//  Manages global solver factories, build-keyed CBOR evidence persistence,
//  fidelity gating, port compatibility checks, in-memory/cached port feeds,
//  and streamed checkpoint fallback adapters.
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SimulationSolver.h"
#include "SimulationProcess.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <mutex>
#include <cstring>
#include <limits>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/loader.h>
#endif

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool DigestValid(const std::string& digest)
{
	return digest.size()==64 && std::all_of(digest.begin(),digest.end(),[](char c){
		return (c>='0' && c<='9') || (c>='a' && c<='f');
	});
}

struct Registry {
	std::mutex mutex;
	std::vector<std::pair<std::string,SolverFactory>> factories;
};
Registry& TheRegistry()
{
	static Registry registry;
	return registry;
}
} // namespace

const char* BackendStatusName(BackendStatus status)
{
	switch (status) {
	case BackendStatus::Supported: return "supported";
	case BackendStatus::Experimental: return "experimental";
	case BackendStatus::Unsupported: break;
	}
	return "unsupported";
}

// Canonical SHA-256 digest over CBOR ["rise.simulation.evidence.v1", id, config, error, tol, passed].
std::string EvidenceDigest(const std::string& id,const V& configuration,double error,double tolerance,bool passed)
{
	Bytes encoded;
	if (!std::isfinite(error) || !std::isfinite(tolerance) ||
		!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.evidence.v1"),V::String(id),configuration,
			V::Float(error),V::Float(tolerance),V::Bool(passed)}),encoded,nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

// ------------------------------------------------------------------ persistent evidence (A2)

namespace {
constexpr const char* EvidenceStoreFormat = "rise.simulation.evidence-store.v1";
constexpr std::size_t EvidenceStoreEntries = 256;              // pruned to 3/4 of this
constexpr std::uintmax_t EvidenceStoreBytes = 16u << 20;       // or of this many bytes
constexpr std::uintmax_t EvidenceEntryBytes = 1u << 20;        // larger files are not read

std::uint64_t DoubleBits(double d) { std::uint64_t b; std::memcpy(&b,&d,8); return b; }
double BitsDouble(std::uint64_t b) { double d; std::memcpy(&d,&b,8); return d; }

// Encodes Evidence to CBOR, storing error/tolerance as raw IEEE-754 uint64 bits for exact round-trip.
V EvidenceToValue(const Evidence& e)
{
	V::Values cases;
	for (const auto& c : e.cases)
		cases.push_back(V::ArrayValue({V::String(c.id),V::String(c.description),V::String(c.digest),
			V::Unsigned(DoubleBits(c.error)),V::Unsigned(DoubleBits(c.tolerance)),V::Bool(c.passed)}));
	V::Values conditions;
	for (const auto& c : e.conditions) {
		V::Values values;
		for (const auto& v : c.second) values.push_back(V::String(v));
		conditions.push_back(V::ArrayValue({V::String(c.first),V::ArrayValue(std::move(values))}));
	}
	return V::ArrayValue({V::String(e.fidelity),V::ArrayValue(std::move(cases)),V::ArrayValue(std::move(conditions)),
		V::String(e.scope)});
}

bool EvidenceFromValue(const V& v,Evidence& e)
{
	auto text = [](const V& x) { return x.GetType()==V::Text; };
	auto unsignedValue = [](const V& x) { return x.GetType()==V::UnsignedInteger; };
	if (v.GetType()!=V::Array || v.GetArray().size()!=4) return false;
	const auto& a = v.GetArray();
	if (!text(a[0]) || a[1].GetType()!=V::Array || a[2].GetType()!=V::Array || !text(a[3])) return false;
	Evidence out;
	out.fidelity = a[0].GetText();
	for (const V& c : a[1].GetArray()) {
		if (c.GetType()!=V::Array || c.GetArray().size()!=6) return false;
		const auto& f = c.GetArray();
		if (!text(f[0]) || !text(f[1]) || !text(f[2]) || !unsignedValue(f[3]) || !unsignedValue(f[4]) ||
			f[5].GetType()!=V::Boolean) return false;
		EvidenceCase k;
		k.id = f[0].GetText(); k.description = f[1].GetText(); k.digest = f[2].GetText();
		k.error = BitsDouble(f[3].GetIntegerArgument()); k.tolerance = BitsDouble(f[4].GetIntegerArgument());
		k.passed = f[5].GetBoolean();
		out.cases.push_back(std::move(k));
	}
	for (const V& c : a[2].GetArray()) {
		if (c.GetType()!=V::Array || c.GetArray().size()!=2 || !text(c.GetArray()[0]) || c.GetArray()[1].GetType()!=V::Array) return false;
		std::vector<std::string> values;
		for (const V& x : c.GetArray()[1].GetArray()) { if (!text(x)) return false; values.push_back(x.GetText()); }
		out.conditions.push_back({c.GetArray()[0].GetText(),std::move(values)});
	}
	out.scope = a[3].GetText();
	e = std::move(out);
	return true;
}

bool EvidenceStoreOff()
{
	const char* s = std::getenv("RISE_SIMULATION_EVIDENCE_STORE");
	return (s && std::string(s)=="off") || EvidenceBuildIdentity().empty();
}

// The entry's name: SHA-256 of (format, space, key, build).  False when the key cannot be encoded.
bool EvidenceEntryName(const std::string& space,const V& key,Bytes& keyBytes,std::string& name)
{
	if (!RISECBOR64::Encode(key,keyBytes,nullptr)) return false;
	Bytes head;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String(EvidenceStoreFormat),V::String(space),V::BytesValue(keyBytes),
		V::String(EvidenceBuildIdentity())}),head,nullptr)) return false;
	name = RISECBOR64::SHA256Hex(head);
	return true;
}

std::filesystem::path EvidenceStoreDirectory()
{
	return DefaultSimulationCacheDirectory().parent_path() / "evidence";
}
}

// Returns the current binary's Mach-O LC_UUID on macOS so persisted evidence invalidates
// on rebuild; returns empty on other platforms (disabling on-disk evidence reuse).
const std::string& EvidenceBuildIdentity()
{
	static const std::string identity = [] {
		std::string id;
#if defined(__APPLE__)
		Dl_info info = {};
		if (dladdr(reinterpret_cast<const void*>(&EvidenceBuildIdentity),&info)==0 || !info.dli_fbase) return id;
		const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
		if (header->magic!=MH_MAGIC_64) return id;
		const auto* command = reinterpret_cast<const load_command*>(header + 1);
		for (std::uint32_t k = 0; k < header->ncmds; ++k) {
			if (command->cmd==LC_UUID) {
				const auto* u = reinterpret_cast<const uuid_command*>(command);
				static const char* hex = "0123456789abcdef";
				for (unsigned char b : u->uuid) { id += hex[b >> 4]; id += hex[b & 15]; }
				break;
			}
			command = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(command) + command->cmdsize);
		}
#endif
		return id;
	}();
	return identity;
}

// Loads and verifies a cached CBOR evidence record [format, space, key, buildId, body, sha256(body)]
// and bumps its mtime for LRU retention.
bool LoadPersistedEvidence(const std::string& space,const V& key,Evidence& evidence)
{
	if (EvidenceStoreOff()) return false;
	Bytes keyBytes;
	std::string name;
	if (!EvidenceEntryName(space,key,keyBytes,name)) return false;
	const std::filesystem::path path = EvidenceStoreDirectory() / (name + ".cbor");
	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(path,ec);
	if (ec || size==0 || size > EvidenceEntryBytes) return false;
	std::ifstream in(path,std::ios::binary);
	Bytes bytes(static_cast<std::size_t>(size));
	if (!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) return false;
	V entry;
	if (!RISECBOR64::DecodeCanonical(bytes,entry,nullptr) || entry.GetType()!=V::Array || entry.GetArray().size()!=6) return false;
	const auto& f = entry.GetArray();
	if (f[0].GetType()!=V::Text || f[0].GetText()!=EvidenceStoreFormat || f[1].GetType()!=V::Text || f[1].GetText()!=space ||
		f[2].GetType()!=V::ByteString || f[2].GetBytes()!=keyBytes || f[3].GetType()!=V::Text ||
		f[3].GetText()!=EvidenceBuildIdentity() || f[5].GetType()!=V::Text) return false;
	Bytes body;
	if (!RISECBOR64::Encode(f[4],body,nullptr) || RISECBOR64::SHA256Hex(body)!=f[5].GetText()) return false;
	Evidence out;
	if (!EvidenceFromValue(f[4],out)) return false;
	evidence = std::move(out);
	std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now(),ec); // recently used
	return true;
}

// Atomically writes a verified evidence entry via temp-file rename and evicts oldest files
// down to 75% of the entry/byte limits when the store overflows.
void PersistEvidence(const std::string& space,const V& key,const Evidence& evidence)
{
	if (EvidenceStoreOff()) return;
	Bytes keyBytes, body, bytes;
	std::string name, error;
	if (!EvidenceEntryName(space,key,keyBytes,name)) return;
	const V value = EvidenceToValue(evidence);
	if (!RISECBOR64::Encode(value,body,nullptr)) return;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String(EvidenceStoreFormat),V::String(space),V::BytesValue(keyBytes),
		V::String(EvidenceBuildIdentity()),value,V::String(RISECBOR64::SHA256Hex(body))}),bytes,nullptr)) return;
	if (bytes.size() > EvidenceEntryBytes) return;
	const std::filesystem::path directory = EvidenceStoreDirectory();
	if (!CreatePrivateDirectories(directory,error)) return;
	static std::atomic<std::uint64_t> counter{0};
	const std::filesystem::path target = directory / (name + ".cbor");
	const std::filesystem::path temporary = directory / (name + ".tmp." + std::to_string(CurrentProcessId()) + "." +
		std::to_string(counter.fetch_add(1)));
	{
		std::ofstream out(temporary,std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
		if (!out.flush()) { out.close(); std::error_code ec; std::filesystem::remove(temporary,ec); return; }
	}
	std::error_code ec;
	std::filesystem::rename(temporary,target,ec);   // atomic: readers see the old entry, the new one, or none
	if (ec) { std::filesystem::remove(temporary,ec); return; }
	// Bound the store: drop the least recently used entries (and stale temporaries) beyond the limits.
	struct File { std::filesystem::file_time_type time; std::uintmax_t size; std::filesystem::path path; };
	std::vector<File> files;
	std::uintmax_t total = 0;
	for (std::filesystem::directory_iterator it(directory,ec), end; !ec && it!=end; it.increment(ec)) {
		std::error_code e2;
		const auto size = it->file_size(e2);
		const auto time = it->last_write_time(e2);
		if (e2) continue;
		files.push_back({time,size,it->path()});
		total += size;
	}
	if (files.size() <= EvidenceStoreEntries && total <= EvidenceStoreBytes) return;
	std::sort(files.begin(),files.end(),[](const File& a,const File& b) {
		return a.time!=b.time ? a.time < b.time : a.path < b.path; });
	std::size_t count = files.size();
	for (const auto& f : files) {
		if (count <= EvidenceStoreEntries*3/4 && total <= EvidenceStoreBytes*3/4) break;
		if (f.path==target) continue;
		std::error_code e3;
		std::filesystem::remove(f.path,e3);
		--count;
		total -= f.size;
	}
}

std::string ScopedEvidenceDigest(const std::string& caseDigest,const EvidenceConditions& conditions,
	const std::string& scope)
{
	if (conditions.empty() && scope.empty()) return caseDigest;
	V::Members map;
	for (const auto& c : conditions) {
		V::Values values;
		for (const auto& v : c.second) values.push_back(V::String(v));
		map.emplace_back(c.first,V::ArrayValue(values));
	}
	Bytes encoded;
	if (!DigestValid(caseDigest) || !ValidEvidenceConditions(conditions) ||
		!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.evidence.scope.v1"),V::String(caseDigest),
			V::MapValue(map),V::String(scope)}),encoded,nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

// Downgrades "predictive" to "preview" unless scope, conditions, and every case pass
// with finite error <= tolerance (tolerance >= 0) and a valid 64-hex digest.
std::string EffectiveFidelity(const Evidence& evidence)
{
	if (evidence.fidelity!="predictive" || evidence.cases.empty() || evidence.scope.empty() ||
		!RISECBOR64::IsUTF8NFC(evidence.scope) || !ValidEvidenceConditions(evidence.conditions)) return "preview";
	for (const auto& c : evidence.cases)
		if (!c.passed || !std::isfinite(c.error) || !std::isfinite(c.tolerance) || c.tolerance<0 ||
			!(c.error<=c.tolerance) || !DigestValid(c.digest) || c.id.empty()) return "preview";
	return "predictive";
}

// ------------------------------------------------------------------ ports & registry

// Rejects paths, hidden names, and cache/worker-reserved filenames or suffixes.
bool ValidSideOutputName(const std::string& name)
{
	if (name.empty() || name.size()>64 || name[0]=='.' || name.find_first_of("/\\:")!=std::string::npos)
		return false;
	// Cache-owned and worker-owned names (SimulationCache.cpp, SimulationWorker.cpp).
	if (name=="manifest.cbor" || name=="writer.lock" || name.rfind("rise-sim.",0)==0) return false;
	return (name.size()<6 || name.compare(name.size()-6,6,".frame")!=0) &&
		(name.size()<7 || name.compare(name.size()-7,7,".static")!=0);
}

// Checks direct (adapter-free) compatibility: direction, codec, quantity/units,
// ordered channel schemas, and domain digest when both ports specify one.
bool PortsCompatible(const PortDeclaration& offer,const PortDeclaration& accept,std::string& error)
{
	if (!offer.offers || accept.offers) return Fail(error,"port binding must connect an offered port to an accepted port");
	if (offer.codec!=accept.codec) return Fail(error,"port codecs differ: "+offer.codec+" vs "+accept.codec);
	if (offer.quantity!=accept.quantity || offer.units!=accept.units)
		return Fail(error,"port quantities or units differ: "+offer.quantity+" ["+offer.units+"] vs "+
			accept.quantity+" ["+accept.units+"]");
	if (offer.channels.size()!=accept.channels.size())
		return Fail(error,"port channel lists differ (a port adapter must convert them)");
	for (std::size_t i=0;i<offer.channels.size();++i) {
		const auto& a=offer.channels[i];
		const auto& b=accept.channels[i];
		if (a.name!=b.name || a.quantity!=b.quantity || a.units!=b.units || a.components!=b.components)
			return Fail(error,"port channel "+a.name+" ["+a.units+"] differs from "+b.name+" ["+b.units+"]");
	}
	if (!offer.domain.empty() && !accept.domain.empty() && offer.domain!=accept.domain)
		return Fail(error,"port domains differ (a port adapter must resample)");
	return true;
}

bool ISceneInputs::SampleTransform(const std::string& name,double,RigidTransform&,std::string& error) const
{
	return Fail(error,"no transform input named "+name);
}

bool ISceneInputs::ResolveStatic(const std::string& digest,std::size_t,std::shared_ptr<const Bytes>&,std::string& error) const
{
	return Fail(error,"no static payload "+digest+" among the scene inputs");
}

const ISceneInputs& EmptySceneInputs()
{
	static const NoSceneInputs empty;
	return empty;
}

bool IFrameSink::PublishStatic(const std::string&,std::shared_ptr<const Bytes>,std::string&,std::string& error)
{
	return Fail(error,"this frame sink does not take static payloads");
}

bool IPortExchange::OfferStatic(const std::string&,std::shared_ptr<const Bytes>,std::string&)
{
	return true;
}

PortStatus IPortExchange::ResolveStatic(const std::string& digest,std::size_t,std::shared_ptr<const Bytes>&,
	std::string& error)
{
	error="static payload not offered: "+digest;
	return PortStatus::Unavailable;
}

StaticPayloadResolver IPortExchange::Resolver()
{
	return [this](const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& bytes,std::string& error) {
		return ResolveStatic(digest,budget,bytes,error)==PortStatus::Ready;
	};
}

bool NoSceneInputs::Identity(const std::string& name,std::string&,std::string& error) const
{
	return Fail(error,"no scene input named "+name);
}

bool NoSceneInputs::Sample(const std::string& name,double,std::shared_ptr<const Bytes>&,std::string&,
	std::string& error) const
{
	return Fail(error,"no scene input named "+name);
}

// Registers a unique NFC solver factory ID in the process-wide thread-safe registry.
bool RegisterSolver(const std::string& id,SolverFactory factory,std::string& error)
{
	if (id.empty() || id.find('\0')!=std::string::npos || !RISECBOR64::IsUTF8NFC(id) || !factory)
		return Fail(error,"invalid solver id or factory");
	auto& r=TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& f : r.factories) if (f.first==id) return Fail(error,"solver already registered: "+id);
	r.factories.emplace_back(id,std::move(factory));
	return true;
}

std::unique_ptr<ISimulationSolver> CreateSolver(const std::string& id,std::string& error)
{
	SolverFactory factory;
	{
		auto& r=TheRegistry();
		std::lock_guard<std::mutex> hold(r.mutex);
		for (const auto& f : r.factories) if (f.first==id) factory=f.second;
	}
	if (!factory) { error="unknown solver: "+id; return nullptr; }
	auto solver=factory();
	if (!solver) error="solver factory failed: "+id;
	return solver;
}

std::vector<std::string> RegisteredSolvers()
{
	auto& r=TheRegistry();
	std::lock_guard<std::mutex> hold(r.mutex);
	std::vector<std::string> ids;
	for (const auto& f : r.factories) ids.push_back(f.first);
	std::sort(ids.begin(),ids.end());
	return ids;
}

// ------------------------------------------------------------------ port relays

// In-memory coupling relay: enforces monotonic offer times and deduplicates static payloads.
void PortRelay::Connect(const std::string& accepted,const std::string& offered)
{
	routes_.emplace_back(accepted,offered);
}

bool PortRelay::Offer(const std::string& port,double time,std::shared_ptr<const Bytes> encoded,std::string& error)
{
	if (!encoded || !std::isfinite(time)) return Fail(error,"invalid port offer");
	auto it=std::find_if(offers_.begin(),offers_.end(),[&](const auto& o){return o.first==port;});
	if (it==offers_.end()) { offers_.push_back({port,{}}); it=offers_.end()-1; }
	auto& samples=it->second;
	if (!samples.empty() && samples.back().time>time) return Fail(error,"port offers must not go back in time");
	if (!samples.empty() && samples.back().time==time) samples.back().bytes=std::move(encoded);
	else samples.push_back({time,std::move(encoded)});
	return true;
}

bool PortRelay::OfferStatic(const std::string&,std::shared_ptr<const Bytes> payload,std::string& error)
{
	if (!payload) return Fail(error,"invalid static payload offer");
	for (const auto& s : statics_) if (s.second==payload) return true; // the same immutable bytes again: no rehash
	const auto digest=RISECBOR64::SHA256Hex(*payload);
	for (const auto& s : statics_) if (s.first==digest) return true;
	statics_.emplace_back(digest,std::move(payload));
	return true;
}

PortStatus PortRelay::ResolveStatic(const std::string& digest,std::size_t budget,std::shared_ptr<const Bytes>& payload,
	std::string& error)
{
	for (const auto& s : statics_) if (s.first==digest) {
		if (s.second->size()>budget) { error="static payload exceeds the budget"; return PortStatus::Failed; }
		payload=s.second;
		return PortStatus::Ready;
	}
	error="static payload not offered: "+digest;
	return PortStatus::Unavailable;
}

// Resolves route (or direct port name when unrouted) and returns latest sample at or before `time`.
PortStatus PortRelay::Accept(const std::string& port,double time,std::shared_ptr<const Bytes>& encoded,std::string& error)
{
	std::string source=port;
	bool routed=routes_.empty();
	for (const auto& r : routes_) if (r.first==port) { source=r.second; routed=true; }
	if (!routed) { error="port not connected: "+port; return PortStatus::Unbound; }
	const auto it=std::find_if(offers_.begin(),offers_.end(),[&](const auto& o){return o.first==source;});
	if (it==offers_.end()) { error="nothing offered on "+source; return PortStatus::Unavailable; }
	const auto& s=it->second;
	auto after=std::upper_bound(s.begin(),s.end(),time,[](double t,const Sample& x){return t<x.time;});
	if (after==s.begin()) { error="no offer at or before requested time"; return PortStatus::Unavailable; }
	encoded=std::prev(after)->bytes;
	return PortStatus::Ready;
}

// Read-only port feed backed by a cache snapshot; serves latest entry <= requested time.
CachedPortFeed::CachedPortFeed(std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot,std::size_t byteBudget)
	: snapshot_(std::move(snapshot)), budget_(byteBudget)
{
}

void CachedPortFeed::Bind(const std::string& accepted,const std::string& output)
{
	bindings_.emplace_back(accepted,output);
}

bool CachedPortFeed::Offer(const std::string&,double,std::shared_ptr<const Bytes>,std::string&)
{
	return true;
}

PortStatus CachedPortFeed::ResolveStatic(const std::string& digest,std::size_t budget,
	std::shared_ptr<const Bytes>& payload,std::string& error)
{
	if (!snapshot_) { error="no snapshot"; return PortStatus::Unbound; }
	const auto status=snapshot_->ReadStatic(digest,std::min(budget,budget_),payload,error);
	if (status==FrameStatus::Ready) return PortStatus::Ready;
	return status==FrameStatus::Unavailable ? PortStatus::Unavailable : PortStatus::Failed;
}

PortStatus CachedPortFeed::Accept(const std::string& port,double time,std::shared_ptr<const Bytes>& encoded,
	std::string& error)
{
	const auto binding=std::find_if(bindings_.begin(),bindings_.end(),[&](const auto& b){return b.first==port;});
	if (binding==bindings_.end() || !snapshot_) { error="port not bound: "+port; return PortStatus::Unbound; }
	const CacheEntry* best=nullptr;
	for (const auto& e : snapshot_->Entries())
		if (e.output==binding->second && e.time<=time && (!best || e.time>best->time)) best=&e;
	if (!best) { error="no cached value at or before requested time"; return PortStatus::Unavailable; }
	std::shared_ptr<const Bytes> bytes;
	const auto status=snapshot_->Read(best->index,best->output,budget_,bytes,error);
	if (status==FrameStatus::Unavailable) return PortStatus::Unavailable;
	if (status!=FrameStatus::Ready) return PortStatus::Failed;
	encoded=std::move(bytes);
	return PortStatus::Ready;
}

// ---------------------------------------------------------------- streamed checkpoints (M7 prerequisites)

bool MemoryCheckpointSink::Write(const std::uint8_t* data, std::size_t size, std::string& error)
{
	(void)error;
	if (size) bytes.insert(bytes.end(), data, data + size);
	return true;
}

bool MemoryCheckpointSource::Read(std::uint8_t* data, std::size_t size, std::string& error)
{
	if (size > bytes_.size() - position_) { error = "checkpoint stream is truncated"; return false; }
	if (size) std::memcpy(data, bytes_.data() + position_, size);
	position_ += size;
	return true;
}

// Default whole-buffer fallback for solvers that do not override streaming checkpoints.
bool ISimulationSolver::CheckpointTo(ICheckpointSink& sink, std::string& error)
{
	Bytes state;
	return Checkpoint(state, error) && sink.Write(state.data(), state.size(), error);
}

bool ISimulationSolver::RestoreFrom(ICheckpointSource& source, std::string& error)
{
	const std::uint64_t size = source.Remaining();
	if (size > (std::numeric_limits<std::size_t>::max)()) { error = "checkpoint stream too large"; return false; }
	Bytes state(static_cast<std::size_t>(size));
	return source.Read(state.data(), state.size(), error) && Restore(state, error);
}

} }
