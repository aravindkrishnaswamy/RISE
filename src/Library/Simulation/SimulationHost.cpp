#include "pch.h"
#include "SimulationHost.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool Earlier(const CacheEntry& a, const CacheEntry& b)
{
	return std::tie(a.index,a.output) < std::tie(b.index,b.output);
}

bool DigestValid(const std::string& digest)
{
	return digest.size()==64 && std::all_of(digest.begin(),digest.end(),[](char c){
		return (c>='0' && c<='9') || (c>='a' && c<='f');
	});
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
		if (p.name.empty() || p.codec.empty() || p.quantity.empty() || p.units.empty() || !ports.insert(p.name).second)
			return Fail(error,"invalid or duplicate port declaration");
		if (!p.output.empty()) {
			const auto out=std::find_if(d.cache.outputs.begin(),d.cache.outputs.end(),
				[&](const CacheOutput& o){return o.name==p.output;});
			if (out==d.cache.outputs.end() || out->type!=p.codec)
				return Fail(error,"port "+p.name+" names an undeclared output or a different codec");
		}
	}
	std::vector<std::pair<std::string,std::string>> identities;
	for (const auto& name : d.inputs) {
		std::string hash;
		if (!inputs.Identity(name,hash,error)) return false;
		identities.emplace_back(name,hash);
	}
	std::string key;
	if (!MakeCacheKey(d.solverId,d.version,d.effectiveParameters,identities,key,error)) return false;
	auto& c=d.cache;
	c.key=key;
	c.epoch=DeriveEpoch(key,parent);
	c.producer=d.solverId;
	c.producerVersion=d.version;
	c.fidelity=EffectiveFidelity(d.evidence);
	c.evidence.clear();
	for (const auto& e : d.evidence.cases) if (DigestValid(e.digest)) c.evidence.push_back(e.digest);
	return ValidateCacheDescription(c,error);
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
	CacheEntry needle;
	needle.index=index; needle.output=output;
	const auto it=std::lower_bound(entries_.begin(),entries_.end(),needle,Earlier);
	if (it==entries_.end() || it->index!=index || it->output!=output) {
		error="frame not yet published"; return FrameStatus::Unavailable;
	}
	const auto& payload=payloads_[static_cast<std::size_t>(it-entries_.begin())];
	if (payload->size()>budget) { error="frame exceeds read budget"; return FrameStatus::BudgetExceeded; }
	bytes=payload;
	return FrameStatus::Ready;
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

bool MemoryPublisher::Publish(std::int64_t index,const std::string& output,double time,
	std::shared_ptr<const Bytes> payload,const Bytes& receipt,std::string& error)
{
	std::lock_guard<std::mutex> hold(mutex_);
	if (!current_) return Fail(error,"publisher has no description");
	const auto& d=*current_->description_;
	const bool inRange=index>=d.firstIndex &&
		static_cast<std::uint64_t>(index)-static_cast<std::uint64_t>(d.firstIndex)<d.count;
	const bool known=std::any_of(d.outputs.begin(),d.outputs.end(),[&](const CacheOutput& o){return o.name==output;});
	if (!payload || !inRange || !known || !std::isfinite(time)) return Fail(error,"invalid frame publication");
	auto next=std::make_shared<MemoryCacheSnapshot>(*current_);
	CacheEntry entry{index,output,time,payload->size(),RISECBOR64::SHA256Hex(*payload),receipt};
	auto it=std::lower_bound(next->entries_.begin(),next->entries_.end(),entry,Earlier);
	const auto at=static_cast<std::size_t>(it-next->entries_.begin());
	V row=V::ArrayValue({V::String(current_->digest_),V::Signed(index),V::String(output),V::Float(time),
		V::String(entry.digest),V::BytesValue(receipt)});
	Bytes encoded;
	if (!RISECBOR64::Encode(row,encoded,&error)) return false;
	if (it!=next->entries_.end() && it->index==index && it->output==output) {
		*it=std::move(entry);
		next->payloads_[at]=std::move(payload);
	} else {
		next->entries_.insert(it,std::move(entry));
		next->payloads_.insert(next->payloads_.begin()+static_cast<std::ptrdiff_t>(at),std::move(payload));
	}
	next->digest_=RISECBOR64::SHA256Hex(encoded);
	next->complete_=next->entries_.size()==d.outputs.size()*d.count;
	current_=std::move(next);
	return true;
}

std::shared_ptr<const MemoryCacheSnapshot> MemoryPublisher::Current() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return current_;
}

class InProcessHost::Sink final : public IFrameSink {
public:
	explicit Sink(InProcessHost& host) : host_(host) {}
	bool Publish(std::int64_t index,const std::string& output,double time,std::shared_ptr<const Bytes> payload,
		const Bytes& receipt,std::string& error) override {
		if (!payload) return Fail(error,"null frame payload");
		if (host_.recorder_ && !host_.recorder_->Append(index,output,time,*payload,error,receipt)) return false;
		return host_.publisher_.Publish(index,output,time,std::move(payload),receipt,error);
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
	return true;
}

bool InProcessHost::Start(std::string& error)
{
	if (!configured_ || started_) return Fail(error,"start once, after Configure");
	if (!solver_->Start(error)) return false;
	std::lock_guard<std::mutex> hold(mutex_);
	position_=solver_->Position();
	started_=true;
	return true;
}

bool InProcessHost::StepLocked(const AdvanceRequest& request,AdvanceResult& result,std::string& error)
{
	std::lock_guard<std::mutex> step(stepMutex_);
	if (!started_) return Fail(error,"host not started");
	Sink sink(*this);
	const bool ok=solver_->Advance(request,sink,ports_,nullptr,result,error);
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
	return StepLocked(request,result,error);
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
	bool interrupt;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		playing_=false;
		interrupt=stepping_;
	}
	if (interrupt) solver_->RequestStop();
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
		const bool ok=StepLocked(AdvanceRequest{},result,error);
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
	if (!solver_->Restore(checkpoint.state,error)) return false;
	const auto position=solver_->Position();
	if (position.nextIndex!=checkpoint.position.nextIndex || position.time!=checkpoint.position.time ||
		position.finished!=checkpoint.position.finished) return Fail(error,"restored solver position disagrees with checkpoint");
	std::lock_guard<std::mutex> hold(mutex_);
	position_=position;
	started_=true;
	return true;
}

} }
