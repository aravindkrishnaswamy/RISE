#include "pch.h"
#include "SimulationHost.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;
namespace fs = std::filesystem;

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

bool PortValid(const PortDeclaration& p)
{
	if (p.name.empty() || p.codec.empty() || p.quantity.empty() || p.units.empty()) return false;
	if (!p.domain.empty() && !DigestValid(p.domain)) return false;
	std::set<std::string> names;
	for (const auto& c : p.channels)
		if (c.name.empty() || c.quantity.empty() || c.units.empty() || c.components<1 || c.components>16 ||
			!names.insert(c.name).second) return false;
	return true;
}

// Creates `directory` owner-only when missing; refuses a non-directory.
bool MakePrivateDirectory(const fs::path& directory, bool fresh, std::string& error)
{
	std::error_code ec;
	const auto status=fs::symlink_status(directory,ec);
	if (ec && ec!=std::errc::no_such_file_or_directory) return Fail(error,"cannot inspect side output directory");
	if (status.type()!=fs::file_type::not_found && status.type()!=fs::file_type::directory)
		return Fail(error,"side output path is not a directory: "+directory.string());
	if (fresh && status.type()==fs::file_type::directory) {
		const auto old=directory.parent_path()/(".rise-sim.stale."+directory.filename().string());
		fs::remove_all(old,ec);
		fs::rename(directory,old,ec);
		if (ec) return Fail(error,"cannot retire the previous side output: "+ec.message());
		fs::remove_all(old,ec);
	}
	if (!fs::is_directory(fs::symlink_status(directory,ec))) {
		fs::create_directories(directory,ec);
		if (ec || !fs::is_directory(fs::symlink_status(directory,ec)))
			return Fail(error,"cannot create side output directory "+directory.string());
		fs::permissions(directory,fs::perms::owner_all,fs::perm_options::replace,ec);
		if (ec) return Fail(error,"cannot make the side output directory owner-only");
	}
	return true;
}

struct AdapterRegistry {
	std::mutex mutex;
	std::vector<std::pair<std::string,std::shared_ptr<const IPortAdapter>>> adapters;
};
AdapterRegistry& Adapters()
{
	static AdapterRegistry registry;
	return registry;
}
} // namespace

std::string DeriveEpoch(const std::string& key,const std::string& parentDigest)
{
	const std::string text=std::string("rise.simulation.epoch.v1")+'\0'+key+'\0'+parentDigest;
	return RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(text.data()),text.size());
}

bool PrepareCacheDescription(SolverDescription& d,const ISceneInputs& inputs,const std::string& parent,
	std::string& error)
{
	if (!d.sideOutput.empty() && !ValidSideOutputName(d.sideOutput)) return Fail(error,"invalid side output name");
	std::set<std::string> ports;
	for (const auto& p : d.ports) {
		if (!PortValid(p) || !ports.insert(p.name).second) return Fail(error,"invalid or duplicate port declaration");
		if (!p.output.empty()) {
			const auto out=std::find_if(d.cache.outputs.begin(),d.cache.outputs.end(),
				[&](const CacheOutput& o){return o.name==p.output;});
			if (out==d.cache.outputs.end() || out->type!=p.codec)
				return Fail(error,"port "+p.name+" names an undeclared output or a different codec");
		}
	}
	std::set<std::string> names;
	std::vector<std::pair<std::string,std::string>> identities;
	for (const auto& name : d.inputs) {
		if (!names.insert(name).second) return Fail(error,"scene input listed twice: "+name);
		std::string hash;
		if (!inputs.Identity(name,hash,error)) return false;
		identities.emplace_back(name,hash);
	}
	if (!ValidEvidenceConditions(d.evidence.conditions)) return Fail(error,"invalid evidence conditions");
	std::string key;
	if (!MakeCacheKey(d.solverId,d.version,d.effectiveParameters,identities,key,error)) return false;
	auto& c=d.cache;
	c.key=key;
	c.epoch=DeriveEpoch(key,parent);
	c.producer=d.solverId;
	c.producerVersion=d.version;
	c.fidelity=EffectiveFidelity(d.evidence);
	c.evidenceConditions=d.evidence.conditions;
	c.evidenceScope=d.evidence.scope;
	c.evidence.clear();
	for (const auto& e : d.evidence.cases)
		if (DigestValid(e.digest)) c.evidence.push_back(ScopedEvidenceDigest(e.digest,c.evidenceConditions,c.evidenceScope));
	return ValidateCacheDescription(c,error);
}

// ---------------------------------------------------------------- memory snapshots

std::size_t MemoryCacheSnapshot::LowerBound(std::int64_t index,const std::string& output) const
{
	std::size_t low=0, high=items_.size();
	while (low<high) {
		const auto mid=low+(high-low)/2;
		const auto& e=items_[mid].entry;
		if (std::tie(e.index,e.output)<std::tie(index,output)) low=mid+1;
		else high=mid;
	}
	return low;
}

const std::vector<CacheEntry>& MemoryCacheSnapshot::Entries() const
{
	std::call_once(materialised_,[this]{
		entries_.reserve(items_.size());
		for (std::size_t i=0;i<items_.size();++i) entries_.push_back(items_[i].entry);
	});
	return entries_;
}

OutputSpan MemoryCacheSnapshot::Span(const std::string& output) const
{
	const auto it=std::find_if(spans_.begin(),spans_.end(),[&](const auto& s){return s.first==output;});
	return it==spans_.end() ? OutputSpan{} : it->second;
}

bool MemoryCacheSnapshot::FindEntry(std::int64_t index,const std::string& output,CacheEntry& entry) const
{
	const auto at=LowerBound(index,output);
	if (at==items_.size() || items_[at].entry.index!=index || items_[at].entry.output!=output) return false;
	entry=items_[at].entry;
	return true;
}

FrameStatus MemoryCacheSnapshot::Read(std::int64_t index,const std::string& output,std::size_t budget,
	std::shared_ptr<const Bytes>& bytes,std::string& error) const
{
	bytes.reset();
	const auto& outs=description_->outputs;
	const auto type=std::find_if(outs.begin(),outs.end(),[&](const CacheOutput& o){return o.name==output;});
	if (type==outs.end() || !IsNativePayloadType(type->type)) {
		error="undeclared output or unsupported native payload type"; return FrameStatus::Unsupported;
	}
	const auto at=LowerBound(index,output);
	if (at==items_.size() || items_[at].entry.index!=index || items_[at].entry.output!=output) {
		error="frame not yet published"; return FrameStatus::Unavailable;
	}
	const auto& payload=items_[at].payload;
	if (payload->size()>budget) { error="frame exceeds read budget"; return FrameStatus::BudgetExceeded; }
	bytes=payload;
	return FrameStatus::Ready;
}

const std::vector<CacheStatic>& MemoryCacheSnapshot::Statics() const
{
	static const std::vector<CacheStatic> none;
	return statics_ ? statics_->list : none;
}

FrameStatus MemoryCacheSnapshot::ReadStatic(const std::string& digest,std::size_t budget,
	std::shared_ptr<const Bytes>& bytes,std::string& error) const
{
	bytes.reset();
	if (statics_) {
		const auto& list=statics_->list;
		const auto it=std::lower_bound(list.begin(),list.end(),digest,
			[](const CacheStatic& s,const std::string& d){return s.digest<d;});
		if (it!=list.end() && it->digest==digest) {
			const auto& payload=statics_->payloads[static_cast<std::size_t>(it-list.begin())];
			if (payload->size()>budget) { error="static payload exceeds read budget"; return FrameStatus::BudgetExceeded; }
			bytes=payload;
			return FrameStatus::Ready;
		}
	}
	error="static payload not yet published: "+digest;
	return FrameStatus::Unavailable;
}

void MemoryPublisher::Reset(const CacheDescription& description)
{
	auto first=std::make_shared<MemoryCacheSnapshot>();
	first->description_=std::make_shared<const CacheDescription>(description);
	const std::string seed="rise.simulation.memory.v1:"+description.key+":"+description.epoch;
	first->digest_=RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(seed.data()),seed.size());
	first->complete_=description.count==0;
	std::lock_guard<std::mutex> hold(mutex_);
	current_=std::move(first);
}

// The next generation sharing everything with the current one, its chain
// digest extended by `row`. Called with mutex_ held.
std::shared_ptr<MemoryCacheSnapshot> MemoryPublisher::Next(const char* event,const V& row,std::string& error) const
{
	Bytes encoded;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String(event),V::String(current_->digest_),row}),encoded,&error))
		return nullptr;
	auto next=std::make_shared<MemoryCacheSnapshot>();
	next->description_=current_->description_;
	next->items_=current_->items_;
	next->spans_=current_->spans_;
	next->statics_=current_->statics_;
	next->digest_=RISECBOR64::SHA256Hex(encoded);
	return next;
}

bool MemoryPublisher::Publish(std::int64_t index,const std::string& output,double time,
	std::shared_ptr<const Bytes> payload,const Bytes& receipt,std::string& error)
{
	std::lock_guard<std::mutex> hold(mutex_);
	if (!current_) return Fail(error,"publisher has no description");
	const auto& d=*current_->description_;
	const bool inRange=index>=d.firstIndex &&
		static_cast<std::uint64_t>(index)-static_cast<std::uint64_t>(d.firstIndex)<d.count;
	const auto type=std::find_if(d.outputs.begin(),d.outputs.end(),[&](const CacheOutput& o){return o.name==output;});
	if (!payload || !inRange || type==d.outputs.end() || !std::isfinite(time)) return Fail(error,"invalid frame publication");
	std::vector<std::string> references;
	if (!NativePayloadReferences(type->type,*payload,references,error)) return Fail(error,"invalid native frame payload: "+error);
	for (const auto& r : references) {
		std::shared_ptr<const Bytes> ignored;
		std::string why;
		if (current_->ReadStatic(r,(std::numeric_limits<std::size_t>::max)(),ignored,why)!=FrameStatus::Ready)
			return Fail(error,"frame references an unpublished static payload "+r);
	}
	MemoryCacheSnapshot::Item item{CacheEntry{index,output,time,payload->size(),RISECBOR64::SHA256Hex(*payload),receipt},payload};
	auto next=Next("frame",V::ArrayValue({V::Signed(index),V::String(output),V::Float(time),
		V::String(item.entry.digest),V::BytesValue(receipt)}),error);
	if (!next) return false;
	const auto at=current_->LowerBound(index,output);
	auto& items=next->items_;
	if (at<items.size() && items[at].entry.index==index && items[at].entry.output==output) {
		items=items.set(at,std::move(item));
	} else {
		auto sp=std::find_if(next->spans_.begin(),next->spans_.end(),[&](const auto& s){return s.first==output;});
		if (sp==next->spans_.end()) { next->spans_.push_back({output,OutputSpan{}}); sp=next->spans_.end()-1; }
		auto& span=sp->second;
		if (span.count==0 || index<span.first) span.first=index;
		if (span.count==0 || index>span.last) span.last=index;
		++span.count;
		if (at==items.size()) {
			items=items.push_back(std::move(item));
		} else {
			// Out of order (rare: several outputs per index, or a replay after a
			// restore): rebuild only the tail behind the insertion point.
			std::vector<MemoryCacheSnapshot::Item> tail;
			for (std::size_t i=at;i<items.size();++i) tail.push_back(items[i]);
			items=items.truncate(at).push_back(std::move(item));
			for (auto& t : tail) items=items.push_back(std::move(t));
		}
	}
	next->complete_=items.size()==d.outputs.size()*d.count;
	current_=std::move(next);
	return true;
}

bool MemoryPublisher::PublishStatic(const std::string& type,std::shared_ptr<const Bytes> payload,std::string& digest,
	std::string& error)
{
	std::lock_guard<std::mutex> hold(mutex_);
	if (!current_) return Fail(error,"publisher has no description");
	if (!payload || type.empty() || !RISECBOR64::IsUTF8NFC(type)) return Fail(error,"invalid static payload publication");
	const auto hash=RISECBOR64::SHA256Hex(*payload);
	const auto& old=current_->Statics();
	const auto it=std::lower_bound(old.begin(),old.end(),hash,[](const CacheStatic& s,const std::string& d){return s.digest<d;});
	if (it!=old.end() && it->digest==hash) {
		if (it->type!=type) return Fail(error,"conflicting static payload declaration");
		digest=hash;
		return true;
	}
	auto next=Next("static",V::ArrayValue({V::String(hash),V::String(type)}),error);
	if (!next) return false;
	auto set=std::make_shared<MemoryCacheSnapshot::StaticSet>();
	if (current_->statics_) *set=*current_->statics_;
	const auto at=static_cast<std::size_t>(it-old.begin());
	set->list.insert(set->list.begin()+static_cast<std::ptrdiff_t>(at),CacheStatic{hash,type,payload->size()});
	set->payloads.insert(set->payloads.begin()+static_cast<std::ptrdiff_t>(at),std::move(payload));
	next->statics_=std::move(set);
	next->complete_=current_->complete_;
	current_=std::move(next);
	digest=hash;
	return true;
}

void MemoryPublisher::DiscardFrom(std::int64_t index)
{
	std::lock_guard<std::mutex> hold(mutex_);
	if (!current_) return;
	const auto at=current_->LowerBound(index,std::string());
	if (at>=current_->items_.size()) return;
	std::string ignored;
	auto next=Next("discard",V::Signed(index),ignored);
	if (!next) return;
	next->items_=next->items_.truncate(at);
	next->spans_.clear(); // rare: recompute the per-output spans of what is left
	for (std::size_t i=0;i<next->items_.size();++i) {
		const auto& e=next->items_[i].entry;
		auto sp=std::find_if(next->spans_.begin(),next->spans_.end(),[&](const auto& s){return s.first==e.output;});
		if (sp==next->spans_.end()) { next->spans_.push_back({e.output,OutputSpan{}}); sp=next->spans_.end()-1; }
		auto& span=sp->second;
		if (span.count==0 || e.index<span.first) span.first=e.index;
		if (span.count==0 || e.index>span.last) span.last=e.index;
		++span.count;
	}
	const auto& d=*current_->description_;
	next->complete_=next->items_.size()==d.outputs.size()*d.count;
	current_=std::move(next);
}

std::shared_ptr<const MemoryCacheSnapshot> MemoryPublisher::Current() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return current_;
}

// ---------------------------------------------------------------- in-process host

class InProcessHost::Sink final : public IFrameSink {
public:
	explicit Sink(InProcessHost& host) : host_(host) {}
	bool Publish(std::int64_t index,const std::string& output,double time,std::shared_ptr<const Bytes> payload,
		const Bytes& receipt,std::string& error) override {
		if (!payload) return Fail(error,"null frame payload");
		if (host_.recorder_ && !host_.recorder_->Append(index,output,time,*payload,error,receipt)) return false;
		return host_.publisher_.Publish(index,output,time,std::move(payload),receipt,error);
	}
	bool PublishStatic(const std::string& type,std::shared_ptr<const Bytes> payload,std::string& digest,
		std::string& error) override {
		if (!payload) return Fail(error,"null static payload");
		if (host_.recorder_ && !host_.recorder_->AppendStatic(type,*payload,digest,error)) return false;
		return host_.publisher_.PublishStatic(type,std::move(payload),digest,error);
	}
private:
	InProcessHost& host_;
};

InProcessHost::InProcessHost(std::unique_ptr<ISimulationSolver> solver) : solver_(std::move(solver))
{
}

InProcessHost::~InProcessHost()
{
	{
		std::lock_guard<std::mutex> hold(mutex_);
		shutdown_=true;
		playing_=false;
	}
	if (solver_) solver_->RequestStop();
	wake_.notify_all();
	if (worker_.joinable()) worker_.join();
}

bool InProcessHost::Configure(const V& parameters,const ISceneInputs& inputs,std::string& error)
{
	if (!solver_) return Fail(error,"no solver");
	if (started_ || Busy()) return Fail(error,"host already started");
	SolverDescription d;
	if (!solver_->Configure(parameters,inputs,d,error) || !PrepareCacheDescription(d,inputs,"",error)) return false;
	description_=std::move(d);
	publisher_.Reset(description_.cache);
	sideOutput_.clear();
	sideOutputReady_=false;
	configured_=true;
	return true;
}

bool InProcessHost::Record(const std::filesystem::path& root,std::size_t budget,std::string& error)
{
	if (!configured_ || started_ || recorder_) return Fail(error,"record after Configure and before Start");
	std::string parent;
	std::error_code ec;
	if (std::filesystem::exists(root/"manifest.cbor",ec)) {
		CacheSnapshot old;
		if (!CacheSnapshot::Open(root,budget,old,error)) return false;
		parent=old.Digest();
	}
	auto cache=description_.cache;
	cache.epoch=DeriveEpoch(cache.key,parent);
	auto writer=std::make_unique<CacheWriter>();
	if (!writer->Begin(root,cache,parent,false,budget,error)) return false;
	description_.cache=cache;
	publisher_.Reset(cache);
	recorder_=std::move(writer);
	recordRoot_=root;
	return true;
}

bool InProcessHost::SetSideOutputDirectory(const std::filesystem::path& directory,std::string& error)
{
	if (!configured_ || started_) return Fail(error,"set the side output after Configure and before Start");
	if (description_.sideOutput.empty()) return Fail(error,"this solver declares no side output");
	if (directory.empty() || !MakePrivateDirectory(directory,false,error)) return directory.empty() ?
		Fail(error,"empty side output directory") : false;
	if (!solver_->SetSideOutputDirectory(directory,error)) return false;
	sideOutput_=directory;
	sideOutputReady_=true;
	return true;
}

// A recording gives a side-output solver <cache>/<sideOutput>, emptied for the new epoch.
bool InProcessHost::PrepareSideOutput(std::string& error)
{
	if (description_.sideOutput.empty() || sideOutputReady_ || !recorder_) return true;
	const auto directory=recordRoot_/description_.sideOutput;
	if (!MakePrivateDirectory(directory,true,error) || !solver_->SetSideOutputDirectory(directory,error)) return false;
	sideOutput_=directory;
	sideOutputReady_=true;
	return true;
}

bool InProcessHost::Start(std::string& error)
{
	if (!configured_ || started_) return Fail(error,"start once, after Configure");
	if (!PrepareSideOutput(error) || !solver_->Start(error)) return false;
	std::lock_guard<std::mutex> hold(mutex_);
	position_=solver_->Position();
	started_=true;
	return true;
}

bool InProcessHost::StepLocked(const AdvanceRequest& request,AdvanceResult& result,bool worker,std::string& error)
{
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!started_) return Fail(error,"host not started");
	Sink sink(*this);
	bool stale;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		stale=stopPending_;
	}
	bool ok=solver_->Advance(request,sink,ports_,nullptr,result,error);
	if (ok && result.status==AdvanceResult::Status::Stopped) {
		// RequestStop is sticky: a Pause that reached the solver after its last
		// Advance had already returned is consumed here. Repeat the step once
		// unless a pause is in force now.
		bool retry;
		{
			std::lock_guard<std::mutex> hold(mutex_);
			stopPending_=false;
			retry=stale && (!worker || playing_);
		}
		if (retry) {
			const auto published=result.framesPublished;
			ok=solver_->Advance(request,sink,ports_,nullptr,result,error);
			result.framesPublished+=published;
			if (ok && result.status==AdvanceResult::Status::Stopped) {
				std::lock_guard<std::mutex> hold(mutex_);
				stopPending_=false;
			}
		}
	}
	const auto position=solver_->Position();
	{
		std::lock_guard<std::mutex> hold(mutex_);
		position_=position;
	}
	if (!ok) return false;
	if (position.finished && recorder_) {
		if (!recorder_->Seal(error)) return false;
		recorder_.reset(); // release the writer lock once the bake is sealed
	}
	return true;
}

bool InProcessHost::Step(const AdvanceRequest& request,AdvanceResult& result,std::string& error)
{
	if (Busy()) return Fail(error,"host is playing on its worker thread");
	return StepLocked(request,result,false,error);
}

bool InProcessHost::StepToIndex(std::int64_t index,std::string& error)
{
	for (;;) {
		const auto position=Position();
		if (position.finished || position.nextIndex>index) return true;
		AdvanceResult result;
		if (!Step(AdvanceRequest{},result,error)) return false;
		if (result.status==AdvanceResult::Status::Stopped) return Fail(error,"stopped before reaching index");
	}
}

bool InProcessHost::Play(std::string& error,std::int64_t untilIndex)
{
	if (!started_) return Fail(error,"host not started");
	std::lock_guard<std::mutex> hold(mutex_);
	if (shutdown_) return Fail(error,"host shutting down");
	playing_=true;
	failed_=false;
	workerError_.clear();
	until_=untilIndex;
	if (!worker_.joinable()) worker_=std::thread(&InProcessHost::WorkerLoop,this);
	wake_.notify_all();
	return true;
}

void InProcessHost::Pause()
{
	{
		std::lock_guard<std::mutex> hold(mutex_);
		playing_=false;
		if (stepping_) {
			// Under mutex_, so the worker cannot clear the request between our
			// check and the solver call; the request may still miss the Advance
			// in progress, and is then consumed by the next one (StepLocked).
			stopPending_=true;
			solver_->RequestStop();
		}
	}
	wake_.notify_all();
}

bool InProcessHost::WaitIdle(std::string& error)
{
	std::unique_lock<std::mutex> lock(mutex_);
	wake_.wait(lock,[&]{return !playing_ && !stepping_;});
	if (failed_) return Fail(error,workerError_);
	return true;
}

bool InProcessHost::Busy() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return playing_ || stepping_;
}

SolverPosition InProcessHost::Position() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return position_;
}

void InProcessHost::WorkerLoop()
{
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		wake_.wait(lock,[&]{return shutdown_ || playing_;});
		if (shutdown_) break;
		if (position_.finished || position_.nextIndex>until_) {
			playing_=false;
			wake_.notify_all();
			continue;
		}
		stepping_=true;
		lock.unlock();
		AdvanceResult result;
		std::string error;
		const bool ok=StepLocked(AdvanceRequest{},result,true,error);
		lock.lock();
		stepping_=false;
		if (!ok) {
			failed_=true;
			workerError_=error;
			playing_=false;
		} else if (position_.finished || position_.nextIndex>until_) playing_=false;
		wake_.notify_all();
	}
}

bool InProcessHost::SaveCheckpoint(Checkpoint& checkpoint,std::string& error)
{
	if (Busy()) return Fail(error,"checkpoint while idle");
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!started_) return Fail(error,"host not started");
	Checkpoint candidate;
	if (!solver_->Checkpoint(candidate.state,error)) return false;
	candidate.key=description_.cache.key;
	candidate.position=solver_->Position();
	checkpoint=std::move(candidate);
	return true;
}

bool InProcessHost::RestoreCheckpoint(const Checkpoint& checkpoint,std::string& error)
{
	if (Busy()) return Fail(error,"restore while idle");
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!configured_) return Fail(error,"restore after Configure");
	if (checkpoint.key!=description_.cache.key) return Fail(error,"checkpoint belongs to a different configuration");
	if (!started_ && !PrepareSideOutput(error)) return false;
	if (!solver_->Restore(checkpoint.state,error)) return false;
	const auto position=solver_->Position();
	if (position.nextIndex!=checkpoint.position.nextIndex || position.time!=checkpoint.position.time ||
		position.finished!=checkpoint.position.finished) return Fail(error,"restored solver position disagrees with checkpoint");
	// Frames after the checkpoint belong to the abandoned continuation.
	publisher_.DiscardFrom(position.nextIndex);
	std::lock_guard<std::mutex> hold(mutex_);
	position_=position;
	started_=true;
	return true;
}

bool InProcessHost::SaveCheckpointTo(ICheckpointSink& sink,std::string& key,SolverPosition& position,std::string& error)
{
	if (Busy()) return Fail(error,"checkpoint while idle");
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!started_) return Fail(error,"host not started");
	if (!solver_->CheckpointTo(sink,error)) return false;
	key=description_.cache.key;
	position=solver_->Position();
	return true;
}

bool InProcessHost::RestoreCheckpointFrom(ICheckpointSource& source,const std::string& key,const SolverPosition& expected,
	std::string& error)
{
	if (Busy()) return Fail(error,"restore while idle");
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!configured_) return Fail(error,"restore after Configure");
	if (key!=description_.cache.key) return Fail(error,"checkpoint belongs to a different configuration");
	if (!started_ && !PrepareSideOutput(error)) return false;
	if (!solver_->RestoreFrom(source,error)) return false;
	if (source.Remaining()!=0) return Fail(error,"the solver did not consume the whole checkpoint stream");
	const auto position=solver_->Position();
	if (position.nextIndex!=expected.nextIndex || position.time!=expected.time ||
		position.finished!=expected.finished) return Fail(error,"restored solver position disagrees with checkpoint");
	publisher_.DiscardFrom(position.nextIndex);
	std::lock_guard<std::mutex> hold(mutex_);
	position_=position;
	started_=true;
	return true;
}

// ---------------------------------------------------------------- port adapters

bool RegisterPortAdapter(const std::string& id,std::shared_ptr<const IPortAdapter> adapter,std::string& error)
{
	if (id.empty() || !RISECBOR64::IsUTF8NFC(id) || !adapter) return Fail(error,"invalid port adapter id or adapter");
	auto& r=Adapters();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& a : r.adapters) if (a.first==id) return Fail(error,"port adapter already registered: "+id);
	r.adapters.emplace_back(id,std::move(adapter));
	return true;
}

std::shared_ptr<const IPortAdapter> FindPortAdapter(const PortDeclaration& offer,const PortDeclaration& accept)
{
	auto& r=Adapters();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& a : r.adapters) if (a.second->Handles(offer,accept)) return a.second;
	return nullptr;
}

} }
