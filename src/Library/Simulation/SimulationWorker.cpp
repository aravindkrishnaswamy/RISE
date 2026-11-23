#include "pch.h"
#include "SimulationWorker.h"
#include "SimulationHost.h"
#include "../Agent/Json.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace RISE { namespace Simulation {
namespace {
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
using Clock = std::chrono::steady_clock;

constexpr char jobName[] = "rise-sim.job.cbor";
constexpr char checkpointName[] = "rise-sim.checkpoint";
constexpr char statusName[] = "rise-sim.status.cbor";
constexpr char checkpointMagic[] = "RISESIMC";
constexpr std::size_t maximumParameterFile = 64u<<20;

// Read by the stepping and watcher threads, written by the handler: a
// lock-free atomic is both race-free and async-signal-safe.
static_assert(ATOMIC_INT_LOCK_FREE==2,"stop flag must be lock-free");
std::atomic<int> g_signal{0};
extern "C" void OnStopSignal(int signal) { g_signal.store(signal); }

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool Exists(const fs::path& path)
{
	std::error_code ec;
	return fs::symlink_status(path,ec).type()!=fs::file_type::not_found && !ec;
}

// ---------------------------------------------------------------- parameters

bool JsonToValue(const Agent::JsonValue& j,V& v,std::string& error)
{
	using T=Agent::JsonValue::Type;
	switch (j.type()) {
	case T::Null: return Fail(error,"JSON null has no parameter meaning");
	case T::Bool: v=V::Bool(j.asBool()); return true;
	case T::Number: {
		const double x=j.asNumber();
		if (!std::isfinite(x)) return Fail(error,"nonfinite JSON number");
		// Integral values in the exactly representable range become integers.
		if (std::floor(x)==x && std::fabs(x)<=9007199254740992.0)
			v=x>=0 ? V::Unsigned(static_cast<std::uint64_t>(x)) : V::Signed(static_cast<std::int64_t>(x));
		else v=V::Float(x);
		return true;
	}
	case T::String: v=V::String(j.asString()); return true;
	case T::Array: {
		V::Values items;
		for (std::size_t i=0;i<j.size();++i) {
			V item;
			if (!JsonToValue(j.at(i),item,error)) return false;
			items.push_back(std::move(item));
		}
		v=V::ArrayValue(items);
		return true;
	}
	case T::Object: {
		V::Members members;
		for (const auto& m : j.members()) {
			V item;
			if (!JsonToValue(m.second,item,error)) return false;
			members.emplace_back(m.first,std::move(item));
		}
		v=V::MapValue(members);
		return true;
	}
	}
	return Fail(error,"unknown JSON value");
}

bool LoadParameters(const fs::path& file,std::size_t budget,V& parameters,std::string& error)
{
	std::error_code ec;
	const auto size=fs::file_size(file,ec);
	if (ec || size>maximumParameterFile || size>budget) return Fail(error,"parameter file unreadable or too large");
	Bytes bytes(static_cast<std::size_t>(size));
	std::ifstream in(file,std::ios::binary);
	if (!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())) ||
		in.peek()!=std::char_traits<char>::eof()) return Fail(error,"cannot read parameter file");
	std::size_t first=0;
	while (first<bytes.size() && std::isspace(static_cast<unsigned char>(bytes[first]))) ++first;
	V candidate;
	if (first<bytes.size() && bytes[first]=='{') {
		Agent::JsonValue json;
		if (!Agent::JsonParse(std::string(bytes.begin(),bytes.end()),json,error)) return false;
		if (!json.isObject()) return Fail(error,"parameter JSON must be an object");
		if (!JsonToValue(json,candidate,error)) return false;
		Bytes canonical;
		if (!RISECBOR64::Encode(candidate,canonical,&error)) return false;  // duplicate or non-NFC keys
	} else {
		if (!CanDecodeCanonicalRecord(bytes,budget)) return Fail(error,"parameter record exceeds budget");
		if (!RISECBOR64::DecodeCanonical(bytes,candidate,&error)) return false;
		if (candidate.GetType()!=V::Map) return Fail(error,"parameter record must be a map");
	}
	parameters=std::move(candidate);
	return true;
}

// ---------------------------------------------------------------- sidecars

struct Job {
	std::string solver, version, key, epoch;
	V parameters;
};

bool WriteJob(const fs::path& root,const Job& job,std::string& error)
{
	Bytes bytes;
	return RISECBOR64::Encode(V::MapValue({{"format",V::String("rise.sim.job.v1")},{"solver",V::String(job.solver)},
		{"version",V::String(job.version)},{"parameters",job.parameters},{"key",V::String(job.key)},
		{"epoch",V::String(job.epoch)}}),bytes,&error) && ReplaceFileDurably(root,jobName,bytes,error);
}

bool Text(const V& map,const char* name,std::string& out)
{
	const V* v=map.Find(name);
	if (!v || v->GetType()!=V::Text) return false;
	out=v->GetText();
	return true;
}

bool ReadRecord(const fs::path& root,const char* name,std::size_t budget,V& value,std::string& error)
{
	Bytes bytes;
	if (!ReadSidecarFile(root,name,budget,bytes,error)) return false;
	if (!CanDecodeCanonicalRecord(bytes,budget)) return Fail(error,std::string(name)+" exceeds budget");
	return RISECBOR64::DecodeCanonical(bytes,value,&error);
}

bool ReadJob(const fs::path& root,std::size_t budget,Job& job,std::string& error)
{
	V v;
	if (!ReadRecord(root,jobName,budget,v,error)) return false;
	std::string format;
	if (v.GetType()!=V::Map || v.GetMap().size()!=6 || !Text(v,"format",format) || format!="rise.sim.job.v1" ||
		!Text(v,"solver",job.solver) || !Text(v,"version",job.version) || !Text(v,"key",job.key) ||
		!Text(v,"epoch",job.epoch) || !v.Find("parameters")) return Fail(error,"invalid worker job record");
	job.parameters=*v.Find("parameters");
	return true;
}

// Header (canonical CBOR) then raw state bytes: "RISESIMC", u32 version 1,
// u64 header length, header, state.
bool WriteCheckpoint(const fs::path& root,const Job& job,const SolverPosition& p,const Bytes& state,
	std::string& error)
{
	Bytes header;
	if (!RISECBOR64::Encode(V::MapValue({{"key",V::String(job.key)},{"solver",V::String(job.solver)},
		{"version",V::String(job.version)},{"time",V::Float(p.time)},{"next",V::Signed(p.nextIndex)},
		{"finished",V::Bool(p.finished)},{"size",V::Unsigned(state.size())},
		{"digest",V::String(RISECBOR64::SHA256Hex(state))}}),header,&error)) return false;
	Bytes file(checkpointMagic,checkpointMagic+8);
	auto word=[&](std::uint64_t x,unsigned width){for (unsigned i=0;i<width;++i) file.push_back(static_cast<unsigned char>(x>>(8*i)));};
	word(1,4); word(header.size(),8);
	file.reserve(file.size()+header.size()+state.size());
	file.insert(file.end(),header.begin(),header.end());
	file.insert(file.end(),state.begin(),state.end());
	return ReplaceFileDurably(root,checkpointName,file,error);
}

bool ReadCheckpoint(const fs::path& root,const Job& job,std::size_t budget,SolverPosition& p,Bytes& state,
	std::string& error)
{
	Bytes file;
	if (!ReadSidecarFile(root,checkpointName,budget,file,error)) return false;
	if (file.size()<20 || std::memcmp(file.data(),checkpointMagic,8)!=0) return Fail(error,"invalid checkpoint file");
	auto word=[&](std::size_t at,unsigned width){std::uint64_t x=0; for (unsigned i=0;i<width;++i) x|=std::uint64_t(file[at+i])<<(8*i); return x;};
	const auto length=word(12,8);
	if (word(8,4)!=1 || length>file.size()-20) return Fail(error,"unsupported or truncated checkpoint");
	const Bytes header(file.begin()+20,file.begin()+20+static_cast<std::ptrdiff_t>(length));
	V h;
	if (!CanDecodeCanonicalRecord(header,budget) || !RISECBOR64::DecodeCanonical(header,h,&error))
		return Fail(error,"invalid checkpoint header");
	std::string key,solver,version,digest;
	const V *time=h.Find("time"), *next=h.Find("next"), *finished=h.Find("finished"), *size=h.Find("size");
	if (h.GetType()!=V::Map || !Text(h,"key",key) || !Text(h,"solver",solver) || !Text(h,"version",version) ||
		!Text(h,"digest",digest) || !time || time->GetType()!=V::Float64 || !next ||
		(next->GetType()!=V::UnsignedInteger && next->GetType()!=V::NegativeInteger) ||
		next->GetIntegerArgument()>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
		!finished || finished->GetType()!=V::Boolean || !size || size->GetType()!=V::UnsignedInteger)
		return Fail(error,"invalid checkpoint header fields");
	if (key!=job.key || solver!=job.solver || version!=job.version) return Fail(error,"checkpoint belongs to another job");
	if (size->GetIntegerArgument()!=file.size()-20-length) return Fail(error,"checkpoint state length mismatch");
	Bytes candidate(file.begin()+20+static_cast<std::ptrdiff_t>(length),file.end());
	if (RISECBOR64::SHA256Hex(candidate)!=digest) return Fail(error,"checkpoint state digest mismatch");
	const auto n=static_cast<std::int64_t>(next->GetIntegerArgument());
	p.time=time->GetFloat();
	p.nextIndex=next->GetType()==V::UnsignedInteger ? n : -1-n;
	p.finished=finished->GetBoolean();
	state.swap(candidate);
	return true;
}

bool WriteStatus(const fs::path& root,const WorkerStatus& s,std::string& error)
{
	Bytes bytes;
	return RISECBOR64::Encode(V::MapValue({{"format",V::String("rise.sim.status.v1")},{"state",V::String(s.state)},
		{"solver",V::String(s.solver)},{"simulated",V::Float(s.simulatedTime)},{"end",V::Float(s.endTime)},
		{"frames",V::Unsigned(s.framesWritten)},{"total",V::Unsigned(s.framesTotal)},
		{"wall",V::Float(s.wallSeconds)},{"error",V::String(s.lastError)}}),bytes,&error) &&
		ReplaceFileDurably(root,statusName,bytes,error);
}

// ---------------------------------------------------------------- run loop

struct Options {
	double checkpointSeconds = 300;
	std::size_t budget = std::size_t(1024)<<20;
	std::uint64_t stopAfter = 0;
	bool force = false;
};

class WriterSink final : public IFrameSink {
public:
	WriterSink(CacheWriter& writer,bool verifyReplay) : writer_(writer), verify_(verifyReplay) {}
	bool Publish(std::int64_t index,const std::string& output,double time,std::shared_ptr<const Bytes> payload,
		const Bytes& receipt,std::string& error) override {
		if (!payload) return Fail(error,"null frame payload");
		if (!verify_) {
			// A nondeterministic solver keeps the committed frame on resume.
			for (const auto& e : writer_.Snapshot().Entries()) if (e.index==index && e.output==output) return true;
		}
		return writer_.Append(index,output,time,*payload,error,receipt);
	}
private:
	CacheWriter& writer_;
	bool verify_;
};

class StopProgress final : public IProgress {
public:
	explicit StopProgress(const std::atomic<bool>& stop) : stop_(stop) {}
	bool Report(double,double,const std::string&) override { return !stop_.load() && g_signal==0; }
private:
	const std::atomic<bool>& stop_;
};

std::uint64_t CommittedIndices(const CacheSnapshot& s)
{
	const auto outputs=s.Description().outputs.size();
	return outputs ? s.Entries().size()/outputs : 0;
}

int Run(ISimulationSolver& solver,const SolverDescription& d,CacheWriter& writer,const fs::path& root,
	const Job& job,const Options& options,double priorWall,std::ostream& out,std::ostream& err)
{
	const auto began=Clock::now();
	auto lastCheckpoint=began;
	WorkerStatus status;
	status.solver=job.solver;
	status.framesTotal=d.cache.count;
	status.endTime=d.cache.startTime+static_cast<double>(d.cache.count)*d.cache.interval;
	auto update=[&](const std::string& state,const std::string& lastError) {
		const auto p=solver.Position();
		status.state=state;
		status.simulatedTime=p.time;
		status.framesWritten=CommittedIndices(writer.Snapshot());
		status.wallSeconds=priorWall+std::chrono::duration<double>(Clock::now()-began).count();
		status.lastError=lastError;
		std::string why;
		if (!WriteStatus(root,status,why)) err<<"rise-sim: cannot write status: "<<why<<"\n";
	};
	auto checkpoint=[&](std::string& error) {
		Bytes state;
		return solver.Checkpoint(state,error) && WriteCheckpoint(root,job,solver.Position(),state,error);
	};
	auto fail=[&](const std::string& error) {
		update("failed",error);
		err<<"rise-sim: "<<error<<"\n";
		return int(WorkerFailed);
	};
	update("running","");

	std::atomic<bool> stop{false}, done{false};
	std::thread watcher([&]{
		while (!done.load()) {
			if (g_signal!=0 || stop.load()) solver.RequestStop();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	});
	struct Join { std::atomic<bool>& done; std::thread& t; ~Join(){done=true; if (t.joinable()) t.join();} } join{done,watcher};

	WriterSink sink(writer,d.capabilities.deterministic);
	StopProgress progress(stop);
	const auto firstIndex=solver.Position().nextIndex;
	bool stopped=false;
	for (;;) {
		if (solver.Position().finished) break;
		if (g_signal!=0 || stop.load()) { stopped=true; break; }
		AdvanceResult result;
		std::string error;
		if (!solver.Advance(AdvanceRequest{},sink,nullptr,&progress,result,error)) return fail(error);
		if (result.position.finished) break;
		if (result.status==AdvanceResult::Status::Stopped) { stopped=true; break; }
		if (options.stopAfter &&
			static_cast<std::uint64_t>(result.position.nextIndex-firstIndex)>=options.stopAfter) {
			stop=true; stopped=true; break;
		}
		if (std::chrono::duration<double>(Clock::now()-lastCheckpoint).count()>=options.checkpointSeconds) {
			if (!checkpoint(error)) return fail("checkpoint failed: "+error);
			lastCheckpoint=Clock::now();
		}
		update("running","");
	}
	std::string error;
	if (stopped) {
		if (!checkpoint(error)) return fail("checkpoint at stop failed: "+error);
		update("stopped","");
		out<<"rise-sim: stopped at t="<<solver.Position().time<<", "<<status.framesWritten<<"/"<<status.framesTotal
			<<" frames; resume with: rise-sim resume "<<root.string()<<"\n";
		return WorkerStopped;
	}
	if (!writer.Seal(error)) return fail("seal failed: "+error);
	std::error_code ec;
	fs::remove(root/checkpointName,ec);
	update("finished","");
	out<<"rise-sim: finished, "<<status.framesWritten<<" frames sealed in "<<root.string()<<"\n";
	return WorkerFinished;
}

bool Configure(ISimulationSolver& solver,const V& parameters,SolverDescription& d,const std::string& parent,
	std::string& error)
{
	NoSceneInputs inputs;
	if (!solver.Configure(parameters,inputs,d,error)) return false;
	if (!d.inputs.empty()) return Fail(error,"this solver reads scene inputs; rise-sim bakes scenes from M4");
	return PrepareCacheDescription(d,inputs,parent,error);
}

double PriorWall(const fs::path& root)
{
	WorkerStatus s;
	std::string ignored;
	return ReadWorkerStatus(root,s,ignored) ? s.wallSeconds : 0;
}

int Bake(const std::string& id,const fs::path& parameterFile,const fs::path& root,const Options& options,
	std::ostream& out,std::ostream& err)
{
	std::string error;
	V parameters;
	if (!LoadParameters(parameterFile,options.budget,parameters,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	auto solver=CreateSolver(id,error);
	if (!solver) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	SolverDescription d;
	if (!Configure(*solver,parameters,d,"",error)) { err<<"rise-sim: configure: "<<error<<"\n"; return WorkerFailed; }
	std::string parent;
	if (Exists(root/"manifest.cbor")) {
		CacheSnapshot old;
		if (!CacheSnapshot::Open(root,options.budget,old,error)) { err<<"rise-sim: existing cache: "<<error<<"\n"; return WorkerFailed; }
		if (!options.force) {
			if (old.Description().key==d.cache.key && old.Complete()) {
				out<<"rise-sim: cache already complete for this configuration\n";
				return WorkerFinished;
			}
			err<<"rise-sim: "<<(old.Description().key==d.cache.key ? "partial cache exists; use resume" :
				"cache holds a different configuration")<<" (or --force to rebake)\n";
			return WorkerFailed;
		}
		parent=old.Digest();
		NoSceneInputs inputs;
		if (!PrepareCacheDescription(d,inputs,parent,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	}
	CacheWriter writer;
	if (!writer.Begin(root,d.cache,parent,false,options.budget,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	const Job job{d.solverId,d.version,d.cache.key,d.cache.epoch,d.effectiveParameters};
	std::error_code ec;
	fs::remove(root/checkpointName,ec);
	if (!WriteJob(root,job,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	if (!solver->Start(error)) {
		WorkerStatus s; s.state="failed"; s.solver=job.solver; s.lastError=error;
		std::string ignored; WriteStatus(root,s,ignored);
		err<<"rise-sim: start: "<<error<<"\n"; return WorkerFailed;
	}
	return Run(*solver,d,writer,root,job,options,0,out,err);
}

int Resume(const fs::path& root,const Options& options,std::ostream& out,std::ostream& err)
{
	std::string error;
	Job job;
	if (!ReadJob(root,options.budget,job,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	auto solver=CreateSolver(job.solver,error);
	if (!solver) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	SolverDescription d;
	if (!Configure(*solver,job.parameters,d,"",error)) { err<<"rise-sim: configure: "<<error<<"\n"; return WorkerFailed; }
	Bytes stored,effective;
	if (!RISECBOR64::Encode(job.parameters,stored,&error) || !RISECBOR64::Encode(d.effectiveParameters,effective,&error) ||
		stored!=effective || d.version!=job.version || d.cache.key!=job.key) {
		err<<"rise-sim: solver version or effective parameters no longer reproduce the cache key\n";
		return WorkerFailed;
	}
	d.cache.epoch=job.epoch;
	CacheSnapshot current;
	if (!CacheSnapshot::Open(root,options.budget,current,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	if (current.Description().key!=job.key || current.Description().epoch!=job.epoch) {
		err<<"rise-sim: cache key or epoch differs from the job\n";
		return WorkerFailed;
	}
	if (current.Complete()) { out<<"rise-sim: cache already complete\n"; return WorkerFinished; }
	CacheWriter writer;
	if (!writer.Begin(root,d.cache,current.Digest(),true,options.budget,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	if (Exists(root/checkpointName)) {
		SolverPosition position;
		Bytes state;
		if (!ReadCheckpoint(root,job,options.budget,position,state,error) || !solver->Restore(state,error)) {
			err<<"rise-sim: checkpoint: "<<error<<"\n"; return WorkerFailed;
		}
		const auto p=solver->Position();
		if (p.nextIndex!=position.nextIndex || p.time!=position.time || p.finished!=position.finished) {
			err<<"rise-sim: restored state disagrees with checkpoint header\n"; return WorkerFailed;
		}
	} else if (!solver->Start(error)) { err<<"rise-sim: start: "<<error<<"\n"; return WorkerFailed; }
	return Run(*solver,d,writer,root,job,options,PriorWall(root),out,err);
}

const char* StateName(CacheState s)
{
	switch (s) {
	case CacheState::Missing: return "missing";
	case CacheState::Baking: return "baking";
	case CacheState::Partial: return "partial";
	case CacheState::Complete: return "complete";
	case CacheState::Corrupt: break;
	}
	return "corrupt";
}

int Status(const fs::path& root,const Options& options,std::ostream& out,std::ostream& err)
{
	std::string error;
	const auto state=InspectCache(root,options.budget,error);
	out<<"cache: "<<StateName(state)<<"\n";
	if (state==CacheState::Corrupt) err<<"rise-sim: "<<error<<"\n";
	WorkerStatus s;
	if (Exists(root/statusName)) {
		if (!ReadWorkerStatus(root,s,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
		std::string worker=s.state;
		if (worker=="running" && state!=CacheState::Baking) worker="interrupted";
		out<<"worker: "<<worker<<"\nsolver: "<<s.solver<<"\nsimulated: "<<s.simulatedTime<<" / "<<s.endTime
			<<"\nframes: "<<s.framesWritten<<" / "<<s.framesTotal<<"\nwall: "<<s.wallSeconds<<" s\n";
		if (!s.lastError.empty()) out<<"error: "<<s.lastError<<"\n";
	}
	out<<"checkpoint: "<<(Exists(root/checkpointName)?"yes":"no")<<"\n";
	return state==CacheState::Corrupt ? WorkerFailed : WorkerFinished;
}

void Usage(std::ostream& err)
{
	err<<"usage: rise-sim bake <solver id> <parameters.json|.cbor> <cache dir> [options]\n"
		"       rise-sim resume <cache dir> [options]\n"
		"       rise-sim status <cache dir>\n"
		"options: --checkpoint-seconds S  --budget-mb N  --stop-after-frames N  --force (bake)\n"
		"exit: 0 finished, 1 failed, 2 usage, 3 stopped (resumable)\n";
	const auto ids=RegisteredSolvers();
	err<<"solvers:";
	for (const auto& id : ids) err<<" "<<id;
	err<<(ids.empty()?" (none registered)\n":"\n");
}

struct SignalScope {
	using Handler=void(*)(int);
	Handler oldInt, oldTerm;
	SignalScope() {
		g_signal=0;
		oldInt=std::signal(SIGINT,OnStopSignal);
		oldTerm=std::signal(SIGTERM,OnStopSignal);
	}
	~SignalScope() {
		std::signal(SIGINT,oldInt==SIG_ERR?SIG_DFL:oldInt);
		std::signal(SIGTERM,oldTerm==SIG_ERR?SIG_DFL:oldTerm);
	}
};
} // namespace

bool ReadWorkerStatus(const fs::path& root,WorkerStatus& status,std::string& error)
{
	V v;
	if (!ReadRecord(root,statusName,1u<<20,v,error)) return false;
	WorkerStatus s;
	std::string format;
	const V *sim=v.Find("simulated"), *end=v.Find("end"), *frames=v.Find("frames"), *total=v.Find("total"),
		*wall=v.Find("wall");
	if (v.GetType()!=V::Map || !Text(v,"format",format) || format!="rise.sim.status.v1" || !Text(v,"state",s.state) ||
		!Text(v,"solver",s.solver) || !Text(v,"error",s.lastError) || !sim || sim->GetType()!=V::Float64 ||
		!end || end->GetType()!=V::Float64 || !frames || frames->GetType()!=V::UnsignedInteger ||
		!total || total->GetType()!=V::UnsignedInteger || !wall || wall->GetType()!=V::Float64)
		return Fail(error,"invalid worker status record");
	s.simulatedTime=sim->GetFloat(); s.endTime=end->GetFloat(); s.wallSeconds=wall->GetFloat();
	s.framesWritten=frames->GetIntegerArgument(); s.framesTotal=total->GetIntegerArgument();
	status=std::move(s);
	return true;
}

int WorkerMain(const std::vector<std::string>& args,std::ostream& out,std::ostream& err)
{
	std::vector<std::string> positional;
	Options options;
	for (std::size_t i=0;i<args.size();++i) {
		const auto& a=args[i];
		auto number=[&](double& value) {
			if (i+1>=args.size()) return false;
			char* end=nullptr;
			value=std::strtod(args[++i].c_str(),&end);
			return end && *end=='\0' && std::isfinite(value) && value>=0;
		};
		double value=0;
		if (a=="--force") options.force=true;
		else if (a=="--checkpoint-seconds") { if (!number(value)) { Usage(err); return WorkerUsage; } options.checkpointSeconds=value; }
		else if (a=="--budget-mb") {
			if (!number(value) || value<1 || value>32768) { Usage(err); return WorkerUsage; }
			options.budget=static_cast<std::size_t>(value)<<20;
		}
		else if (a=="--stop-after-frames") {
			if (!number(value) || value<1 || std::floor(value)!=value) { Usage(err); return WorkerUsage; }
			options.stopAfter=static_cast<std::uint64_t>(value);
		}
		else if (a.size()>1 && a[0]=='-' && a[1]=='-') { Usage(err); return WorkerUsage; }
		else positional.push_back(a);
	}
	if (positional.empty()) { Usage(err); return WorkerUsage; }
	const auto& command=positional[0];
	SignalScope signals;
	if (command=="bake" && positional.size()==4) return Bake(positional[1],positional[2],positional[3],options,out,err);
	if (command=="resume" && positional.size()==2 && !options.force) return Resume(positional[1],options,out,err);
	if (command=="status" && positional.size()==2) return Status(positional[1],options,out,err);
	Usage(err);
	return WorkerUsage;
}

int WorkerMain(int argc,char** argv)
{
	std::vector<std::string> args;
	for (int i=1;i<argc;++i) args.emplace_back(argv[i]);
	return WorkerMain(args,std::cout,std::cerr);
}

} }
