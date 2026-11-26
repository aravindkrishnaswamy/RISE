#include "pch.h"
#include "SimulationWorker.h"
#include "SimulationHost.h"
#include "SceneSimulationWorker.h"
#include "SimulationProcess.h"
#include "../Agent/Json.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

namespace RISE { namespace Simulation {
const char* const kWorkerStopFileName = "rise-sim.stop";
const char* const kWorkerSceneMismatch = "scene mismatch";
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
	return ParseSolverParameters(bytes,budget,parameters,error);
}

} // namespace

bool ParseSolverParameters(const Bytes& bytes,std::size_t budget,V& parameters,std::string& error)
{
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

namespace {

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

// Version 1 checkpoint (written by workers before the M7 prerequisites; still
// read): "RISESIMC", u32 version 1, u64 header length, header (canonical CBOR
// with the state's size and digest), state.
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
	// Drop the header in place rather than copying a possibly large state.
	file.erase(file.begin(),file.begin()+20+static_cast<std::ptrdiff_t>(length));
	Bytes& candidate=file;
	if (RISECBOR64::SHA256Hex(candidate)!=digest) return Fail(error,"checkpoint state digest mismatch");
	const auto n=static_cast<std::int64_t>(next->GetIntegerArgument());
	p.time=time->GetFloat();
	p.nextIndex=next->GetType()==V::UnsignedInteger ? n : -1-n;
	p.finished=finished->GetBoolean();
	state.swap(candidate);
	return true;
}

// M7 prerequisites: the worker checkpoint, streamed (version 2).  The header
// carries the job and the position; the state's length and digest are in the
// trailer (WriteStreamedCheckpoint).
bool WriteCheckpointStream(const fs::path& root,const Job& job,ISimulationSolver& solver,std::string& error)
{
	const SolverPosition p=solver.Position();
	Bytes header;
	if (!RISECBOR64::Encode(V::MapValue({{"key",V::String(job.key)},{"solver",V::String(job.solver)},
		{"version",V::String(job.version)},{"time",V::Float(p.time)},{"next",V::Signed(p.nextIndex)},
		{"finished",V::Bool(p.finished)}}),header,&error)) return false;
	return WriteStreamedCheckpoint(root,checkpointName,header,
		[&](ICheckpointSink& sink,std::string& e) { return solver.CheckpointTo(sink,e); },error);
}

// Restores the solver from the worker checkpoint (version 1 or 2) and returns
// the position the header recorded.
bool RestoreCheckpoint(const fs::path& root,const Job& job,std::size_t budget,ISimulationSolver& solver,
	SolverPosition& p,std::string& error)
{
	if (!IsStreamedCheckpoint(root,checkpointName)) {
		Bytes state;
		return ReadCheckpoint(root,job,budget,p,state,error) && solver.Restore(state,error);
	}
	Bytes header;
	V h;
	bool decoded=false;
	const auto read=[&](ICheckpointSource& source,std::string& e) {
		if (!CanDecodeCanonicalRecord(header,budget) || !RISECBOR64::DecodeCanonical(header,h,&e))
			return Fail(e,"invalid checkpoint header");
		std::string key,solverId,version;
		const V *time=h.Find("time"), *next=h.Find("next"), *finished=h.Find("finished");
		if (h.GetType()!=V::Map || !Text(h,"key",key) || !Text(h,"solver",solverId) || !Text(h,"version",version) ||
			!time || time->GetType()!=V::Float64 || !next ||
			(next->GetType()!=V::UnsignedInteger && next->GetType()!=V::NegativeInteger) ||
			next->GetIntegerArgument()>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
			!finished || finished->GetType()!=V::Boolean)
			return Fail(e,"invalid checkpoint header fields");
		if (key!=job.key || solverId!=job.solver || version!=job.version) return Fail(e,"checkpoint belongs to another job");
		const auto n=static_cast<std::int64_t>(next->GetIntegerArgument());
		p.time=time->GetFloat();
		p.nextIndex=next->GetType()==V::UnsignedInteger ? n : -1-n;
		p.finished=finished->GetBoolean();
		decoded=true;
		return solver.RestoreFrom(source,e);
	};
	return ReadStreamedCheckpoint(root,checkpointName,budget,header,read,error) && (decoded || Fail(error,"empty checkpoint"));
}

double PriorWall(const fs::path& root);

// M5: this process, as the status record names it (pid, start stamp, host).
void StampProcess(WorkerStatus& s)
{
	static const std::uint64_t pid=CurrentProcessId();
	static const std::uint64_t start=ProcessStartStamp(pid);
	static const std::string host=CurrentHostName();
	s.pid=pid; s.pidStart=start; s.host=host;
}

bool WriteStatus(const fs::path& root,const WorkerStatus& given,std::string& error)
{
	WorkerStatus s=given;
	StampProcess(s);
	Bytes bytes;
	return RISECBOR64::Encode(V::MapValue({{"format",V::String("rise.sim.status.v1")},{"state",V::String(s.state)},
		{"solver",V::String(s.solver)},{"simulated",V::Float(s.simulatedTime)},{"end",V::Float(s.endTime)},
		{"frames",V::Unsigned(s.framesWritten)},{"total",V::Unsigned(s.framesTotal)},
		{"wall",V::Float(s.wallSeconds)},{"error",V::String(s.lastError)},{"detail",V::String(s.detail)},
		{"pid",V::Unsigned(s.pid)},{"pid_start",V::Unsigned(s.pidStart)},{"host",V::String(s.host)}}),bytes,&error) &&
		ReplaceFileDurably(root,statusName,bytes,error);
}

bool StopFilePresent(const fs::path& root)
{
	return Exists(root/kWorkerStopFileName);
}

// M5: one GPU solver at a time.  A configuration whose backend needs the GPU
// takes the advisory GPU lock before Start/Restore and holds it until the
// process ends; while another holder has it the worker reports
// "waiting-gpu" and keeps honouring stop requests.  Returns WorkerFinished
// (0) when the run may go on, WorkerStopped when a stop came first.
int AcquireGpu(const SolverDescription& d,const fs::path& root,const std::string& solver,const WorkerOptions& options,
	std::uint64_t framesWritten,AdvisoryFileLock& lock,std::ostream& out,std::ostream& err)
{
	if (!IsGpuBackend(d.capabilities.backend)) return WorkerFinished;
	const fs::path path=options.gpuLock.empty() ? DefaultGpuLockPath() : options.gpuLock;
	bool announced=false;
	std::string lastHolder;
	for (;;) {
		bool busy=false;
		std::string error;
		if (lock.TryLock(path,busy,error)) {
			std::string ignored;
			lock.WriteContent(std::to_string(CurrentProcessId())+" "+root.string()+"\n",ignored);
			if (announced) out<<"rise-sim: GPU lock acquired\n";
			return WorkerFinished;
		}
		if (!busy) { err<<"rise-sim: GPU lock: "<<error<<"\n"; return WorkerFailed; }
		if (g_signal!=0 || StopFilePresent(root)) {
			WorkerStatus s; s.state="stopped"; s.solver=solver; s.wallSeconds=PriorWall(root);
			s.endTime=d.cache.startTime+static_cast<double>(d.cache.count ? d.cache.count-1 : 0)*d.cache.interval;
			s.framesTotal=d.cache.count;
			s.framesWritten=framesWritten;
			std::string ignored; WriteStatus(root,s,ignored);
			out<<"rise-sim: stopped while waiting for the GPU\n";
			return WorkerStopped;
		}
		const std::string holder=ReadLockFileContent(path);
		if (!announced || holder!=lastHolder) {
			WorkerStatus s; s.state="waiting-gpu"; s.solver=solver; s.wallSeconds=PriorWall(root);
			s.endTime=d.cache.startTime+static_cast<double>(d.cache.count ? d.cache.count-1 : 0)*d.cache.interval;
			s.framesTotal=d.cache.count;
			s.framesWritten=framesWritten;
			const auto space=holder.find(' ');
			s.detail="waiting for the GPU (backend "+d.capabilities.backend+"; lock "+path.string()+
				(holder.empty() ? std::string(")") : ", held by pid "+holder.substr(0,space)+")");
			std::string ignored; WriteStatus(root,s,ignored);
			if (!announced) out<<"rise-sim: "<<s.detail<<"\n";
			announced=true;
			lastHolder=holder;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

// ---------------------------------------------------------------- run loop

using Options = WorkerOptions;

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
	bool PublishStatic(const std::string& type,std::shared_ptr<const Bytes> payload,std::string& digest,
		std::string& error) override {
		if (!payload) return Fail(error,"null static payload");
		return writer_.AppendStatic(type,*payload,digest,error);
	}
private:
	CacheWriter& writer_;
	bool verify_;
};

// Stop requests, and progress inside a long Advance: the status record is
// refreshed at most once a second with the reported time and detail.
class StopProgress final : public IProgress {
public:
	StopProgress(const std::atomic<bool>& stop,std::function<void(double,const std::string&)> status) :
		stop_(stop), status_(std::move(status)), last_(Clock::now()) {}
	bool Report(double simulatedTime,double,const std::string& detail) override {
		if (status_ && std::chrono::duration<double>(Clock::now()-last_).count()>=1.0) {
			status_(simulatedTime,detail);
			last_=Clock::now();
		}
		return !stop_.load() && g_signal==0;
	}
private:
	const std::atomic<bool>& stop_;
	std::function<void(double,const std::string&)> status_;
	Clock::time_point last_;
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
	// The time of the last output, which is where a finished run's clock stops.
	status.endTime=d.cache.startTime+static_cast<double>(d.cache.count ? d.cache.count-1 : 0)*d.cache.interval;
	auto update=[&](const std::string& state,const std::string& lastError) {
		const auto p=solver.Position();
		status.state=state;
		status.simulatedTime=p.time;
		if (state!="running") status.detail.clear();
		status.framesWritten=CommittedIndices(writer.Snapshot());
		status.wallSeconds=priorWall+std::chrono::duration<double>(Clock::now()-began).count();
		status.lastError=lastError;
		std::string why;
		if (!WriteStatus(root,status,why)) err<<"rise-sim: cannot write status: "<<why<<"\n";
	};
	// M7 prerequisites: streamed (CheckpointTo; a solver without its own
	// stream writes its in-memory Checkpoint through the default).
	auto checkpoint=[&](std::string& error) { return WriteCheckpointStream(root,job,solver,error); };
	auto fail=[&](const std::string& error) {
		update("failed",error);
		err<<"rise-sim: "<<error<<"\n";
		return int(WorkerFailed);
	};
	update("running","");

	std::atomic<bool> stop{false}, done{false};
	// RequestStop is sticky (honoured by the next Advance if none is running),
	// so one request per stop is enough.
	std::thread watcher([&]{
		bool requested=false;
		unsigned tick=0;
		while (!done.load()) {
			// M5: the stop file, checked about four times a second.
			if (!requested && ++tick%12==0 && StopFilePresent(root)) stop=true;
			if (!requested && (g_signal!=0 || stop.load())) { solver.RequestStop(); requested=true; }
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	});
	struct Join { std::atomic<bool>& done; std::thread& t; ~Join(){done=true; if (t.joinable()) t.join();} } join{done,watcher};

	WriterSink sink(writer,d.capabilities.deterministic);
	StopProgress progress(stop,[&](double,const std::string& detail) {
		status.detail=detail;
		update("running","");
	});
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

// `inputs` outlive the solver (a solver may keep the object it is configured
// with): EmptySceneInputs() is process-lifetime, a scene's are the caller's.
bool Configure(ISimulationSolver& solver,const V& parameters,const ISceneInputs& inputs,SolverDescription& d,
	const std::string& parent,std::string& error)
{
	if (!solver.Configure(parameters,inputs,d,error)) return false;
	return PrepareCacheDescription(d,inputs,parent,error);
}

void PrintNotes(const SolverDescription& d,std::ostream& out)
{
	for (const auto& note : d.notes) out<<"rise-sim: note: "<<note<<"\n";
}

// The solver-owned side-output directory beside the cache files. A new epoch
// starts with an empty one; a resume keeps what the interrupted run wrote.
bool PrepareSideOutput(ISimulationSolver& solver,const SolverDescription& d,const fs::path& root,bool fresh,
	std::string& error)
{
	if (d.sideOutput.empty()) return true;
	if (!ValidSideOutputName(d.sideOutput)) return Fail(error,"invalid side output name");
	const auto dir=root/d.sideOutput;
	std::error_code ec;
	const auto status=fs::symlink_status(dir,ec);
	if (ec && ec!=std::errc::no_such_file_or_directory) return Fail(error,"cannot inspect side output directory");
	if (status.type()!=fs::file_type::not_found && status.type()!=fs::file_type::directory)
		return Fail(error,"side output path is not a directory: "+dir.string());
	if (fresh && status.type()==fs::file_type::directory) {
		// Rename first, so an interrupted removal never leaves a half-deleted
		// directory under the live name.
		const auto old=root/(".rise-sim.stale."+d.sideOutput);
		fs::remove_all(old,ec);
		fs::rename(dir,old,ec);
		if (ec) return Fail(error,"cannot retire the previous side output: "+ec.message());
		fs::remove_all(old,ec);
	}
	if (!fs::is_directory(fs::symlink_status(dir,ec))) {
		// Owner-only, like everything else the worker creates in the cache.
		fs::create_directory(dir,ec);
		if (!ec) fs::permissions(dir,fs::perms::owner_all,fs::perm_options::replace,ec);
	}
	if (ec || !fs::is_directory(fs::symlink_status(dir,ec))) return Fail(error,"cannot create side output directory "+dir.string());
	return solver.SetSideOutputDirectory(dir,error);
}

double PriorWall(const fs::path& root)
{
	WorkerStatus s;
	std::string ignored;
	return ReadWorkerStatus(root,s,ignored) ? s.wallSeconds : 0;
}

int Resume(const fs::path& root,const Options& options,const ISceneInputs& inputs,std::ostream& out,std::ostream& err);

int BakeWith(const std::string& id,const V& parameters,const ISceneInputs& inputs,const fs::path& root,
	const Options& options,bool replace,std::ostream& out,std::ostream& err)
{
	std::string error;
	auto solver=CreateSolver(id,error);
	if (!solver) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	SolverDescription d;
	if (!Configure(*solver,parameters,inputs,d,"",error)) { err<<"rise-sim: configure: "<<error<<"\n"; return WorkerFailed; }
	PrintNotes(d,out);
	std::string parent;
	if (Exists(root/"manifest.cbor")) {
		CacheSnapshot old;
		if (!CacheSnapshot::Open(root,options.budget,old,error)) { err<<"rise-sim: existing cache: "<<error<<"\n"; return WorkerFailed; }
		const bool same=old.Description().key==d.cache.key;
		if (!options.force) {
			if (same && old.Complete()) {
				out<<"rise-sim: cache already complete for this configuration\n";
				return WorkerFinished;
			}
			// A scene bake resumes its own partial cache and replaces a stale one.
			if (replace && same) {
				solver.reset();
				return Resume(root,options,inputs,out,err);
			}
			if (!replace) {
				err<<"rise-sim: "<<(same ? "partial cache exists; use resume" :
					"cache holds a different configuration")<<" (or --force to rebake)\n";
				return WorkerFailed;
			}
		}
		parent=old.Digest();
		if (!PrepareCacheDescription(d,inputs,parent,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	}
	CacheWriter writer;
	if (!writer.Begin(root,d.cache,parent,false,options.budget,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	const Job job{d.solverId,d.version,d.cache.key,d.cache.epoch,d.effectiveParameters};
	std::error_code ec;
	fs::remove(root/checkpointName,ec);
	// M5: a stop file left by an earlier run is not a request to this one.
	fs::remove(root/kWorkerStopFileName,ec);
	if (!WriteJob(root,job,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	AdvisoryFileLock gpu;
	if (const int code=AcquireGpu(d,root,job.solver,options,0,gpu,out,err)) return code;
	if (!PrepareSideOutput(*solver,d,root,true,error) || !solver->Start(error)) {
		WorkerStatus s; s.state="failed"; s.solver=job.solver; s.lastError=error;
		std::string ignored; WriteStatus(root,s,ignored);
		err<<"rise-sim: start: "<<error<<"\n"; return WorkerFailed;
	}
	return Run(*solver,d,writer,root,job,options,0,out,err);
}

int Bake(const std::string& id,const fs::path& parameterFile,const fs::path& root,const Options& options,
	std::ostream& out,std::ostream& err)
{
	std::string error;
	V parameters;
	if (!LoadParameters(parameterFile,options.budget,parameters,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	return BakeWith(id,parameters,EmptySceneInputs(),root,options,false,out,err);
}

int Resume(const fs::path& root,const Options& options,const ISceneInputs& inputs,std::ostream& out,std::ostream& err)
{
	std::string error;
	Job job;
	if (!ReadJob(root,options.budget,job,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	auto solver=CreateSolver(job.solver,error);
	if (!solver) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	SolverDescription d;
	if (!Configure(*solver,job.parameters,inputs,d,"",error)) {
		err<<"rise-sim: configure: "<<error;
		if (&inputs==&EmptySceneInputs()) err<<" (a configuration that reads scene inputs resumes from its scene: rise-sim resume <scene> <simulation>)";
		err<<"\n";
		return WorkerFailed;
	}
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
	PrintNotes(d,out);
	std::error_code ec;
	fs::remove(root/kWorkerStopFileName,ec);   // M5: stale stop file (see BakeWith)
	AdvisoryFileLock gpu;
	if (const int code=AcquireGpu(d,root,job.solver,options,CommittedIndices(writer.Snapshot()),gpu,out,err)) return code;
	if (!PrepareSideOutput(*solver,d,root,false,error)) { err<<"rise-sim: "<<error<<"\n"; return WorkerFailed; }
	if (Exists(root/checkpointName)) {
		SolverPosition position;
		if (!RestoreCheckpoint(root,job,options.budget,*solver,position,error)) {
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
		if ((worker=="running" || worker=="waiting-gpu") && state!=CacheState::Baking) worker="interrupted";
		out<<"worker: "<<worker<<"\nsolver: "<<s.solver<<"\nsimulated: "<<s.simulatedTime<<" / "<<s.endTime
			<<"\nframes: "<<s.framesWritten<<" / "<<s.framesTotal<<"\nwall: "<<s.wallSeconds<<" s\n";
		if (!s.detail.empty()) out<<"detail: "<<s.detail<<"\n";
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
		"       rise-sim bake <scene.RISEscene> (<simulation> ... | --all) [options]\n"
		"       rise-sim resume <scene.RISEscene> [<simulation> ...] [options]\n"
		"       rise-sim status <scene.RISEscene>\n"
		"options: --checkpoint-seconds S  --budget-mb N  --stop-after-frames N  --force (bake)\n"
		"         --gpu-lock PATH  --no-upstream (scene bake)  --expect-key NAME=KEY (scene bake/resume)\n"
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

bool ReadWorkerJobRecord(const fs::path& root,std::string& solver,std::string& version,V& effectiveParameters,
	std::string& key,std::string& error)
{
	Job job;
	if (!ReadJob(root,std::size_t(64)<<20,job,error)) return false;
	solver=job.solver;
	version=job.version;
	effectiveParameters=job.parameters;
	key=job.key;
	return true;
}

// M6: the status sidecar for a host other than this file's single-solver
// worker (the scene group bake), with this process stamped in it.
bool WriteWorkerStatus(const fs::path& root,const WorkerStatus& status,std::string& error)
{
	return WriteStatus(root,status,error);
}

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
	if (const V* detail=v.Find("detail")) {
		if (detail->GetType()!=V::Text) return Fail(error,"invalid worker status record");
		s.detail=detail->GetText();
	}
	// M5: the worker process (absent in records of older workers).
	const V *pid=v.Find("pid"), *start=v.Find("pid_start"), *host=v.Find("host");
	if ((pid && pid->GetType()!=V::UnsignedInteger) || (start && start->GetType()!=V::UnsignedInteger) ||
		(host && host->GetType()!=V::Text)) return Fail(error,"invalid worker status record");
	if (pid) s.pid=pid->GetIntegerArgument();
	if (start) s.pidStart=start->GetIntegerArgument();
	if (host) s.host=host->GetText();
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
		else if (a=="--all") options.all=true;
		else if (a=="--no-upstream") options.noUpstream=true;
		else if (a=="--gpu-lock") {
			if (i+1>=args.size() || args[i+1].empty()) { Usage(err); return WorkerUsage; }
			options.gpuLock=args[++i];
		}
		else if (a=="--expect-key") {
			if (i+1>=args.size()) { Usage(err); return WorkerUsage; }
			const std::string& pair=args[++i];
			const auto eq=pair.find('=');
			if (eq==std::string::npos || eq==0 || eq+1>=pair.size()) { Usage(err); return WorkerUsage; }
			options.expectKeys.emplace_back(pair.substr(0,eq),pair.substr(eq+1));
		}
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
	// M4: a scene file in place of a solver id or a cache directory.
	if (positional.size()>=2 && IsSceneFilePath(positional[1]) && (command=="bake" || command=="resume" || command=="status"))
		return SceneWorkerMain(command,positional[1],
			std::vector<std::string>(positional.begin()+2,positional.end()),options,out,err);
	if (options.all || options.noUpstream || !options.expectKeys.empty()) { Usage(err); return WorkerUsage; }
	if (command=="bake" && positional.size()==4) return Bake(positional[1],positional[2],positional[3],options,out,err);
	if (command=="resume" && positional.size()==2 && !options.force) return Resume(positional[1],options,EmptySceneInputs(),out,err);
	if (command=="status" && positional.size()==2) return Status(positional[1],options,out,err);
	Usage(err);
	return WorkerUsage;
}

int WorkerBakeConfiguration(const std::string& solverId,const V& parameters,const ISceneInputs& inputs,
	const fs::path& cache,const WorkerOptions& options,bool replace,std::ostream& out,std::ostream& err)
{
	return BakeWith(solverId,parameters,inputs,cache,options,replace,out,err);
}

int WorkerResumeConfiguration(const fs::path& cache,const ISceneInputs& inputs,const WorkerOptions& options,
	std::ostream& out,std::ostream& err)
{
	return Resume(cache,options,inputs,out,err);
}

int WorkerMain(int argc,char** argv)
{
	std::vector<std::string> args;
	for (int i=1;i<argc;++i) args.emplace_back(argv[i]);
	return WorkerMain(args,std::cout,std::cerr);
}

// ---------------------------------------------------------------- streamed checkpoints (M7 prerequisites)

namespace {
constexpr char streamTrailerMagic[] = "RISESIME";
constexpr std::size_t streamTrailerBytes = 8 + 64 + 8;

class HashingFileSink final : public ICheckpointSink {
public:
	explicit HashingFileSink(DurableSidecarWriter& writer) : writer_(writer) {}
	bool Write(const std::uint8_t* data, std::size_t size, std::string& error) override
	{
		if (!writer_.Write(data, size, error)) return false;
		sha_.Update(data, size);
		length_ += size;
		return true;
	}
	std::uint64_t Length() const { return length_; }
	std::string Digest() { return sha_.FinalHex(); }
private:
	DurableSidecarWriter& writer_;
	RISECBOR64::SHA256Accumulator sha_;
	std::uint64_t length_ = 0;
};

class HashingFileSource final : public ICheckpointSource {
public:
	HashingFileSource(SidecarReader& reader, std::uint64_t length) : reader_(reader), left_(length) {}
	bool Read(std::uint8_t* data, std::size_t size, std::string& error) override
	{
		if (size > left_) return Fail(error, "checkpoint stream is shorter than the solver reads");
		if (!reader_.Read(data, size, error)) return false;
		sha_.Update(data, size);
		left_ -= size;
		return true;
	}
	std::uint64_t Remaining() const override { return left_; }
	std::string Digest() { return sha_.FinalHex(); }
private:
	SidecarReader& reader_;
	std::uint64_t left_;
	RISECBOR64::SHA256Accumulator sha_;
};

void PutWord(Bytes& b, std::uint64_t x, unsigned width)
{
	for (unsigned i = 0; i < width; ++i) b.push_back(static_cast<std::uint8_t>(x >> (8*i)));
}

std::uint64_t GetWord(const std::uint8_t* p, unsigned width)
{
	std::uint64_t x = 0;
	for (unsigned i = 0; i < width; ++i) x |= std::uint64_t(p[i]) << (8*i);
	return x;
}
}

bool WriteStreamedCheckpoint(const fs::path& directory, const std::string& name, const Bytes& header,
	const std::function<bool(ICheckpointSink&, std::string&)>& write, std::string& error)
{
	DurableSidecarWriter writer;
	if (!writer.Begin(directory, name, error)) return false;
	Bytes prefix(checkpointMagic, checkpointMagic + 8);
	PutWord(prefix, 2, 4);
	PutWord(prefix, header.size(), 8);
	prefix.insert(prefix.end(), header.begin(), header.end());
	if (!writer.Write(prefix.data(), prefix.size(), error)) return false;
	HashingFileSink sink(writer);
	if (!write(sink, error)) return false;
	Bytes trailer;
	PutWord(trailer, sink.Length(), 8);
	const std::string digest = sink.Digest();
	trailer.insert(trailer.end(), digest.begin(), digest.end());
	trailer.insert(trailer.end(), streamTrailerMagic, streamTrailerMagic + 8);
	return writer.Write(trailer.data(), trailer.size(), error) && writer.Commit(error);
}

bool IsStreamedCheckpoint(const fs::path& directory, const std::string& name)
{
	SidecarReader reader;
	std::string ignored;
	std::uint8_t prefix[12];
	return reader.Open(directory, name, ignored) && reader.Read(prefix, sizeof(prefix), ignored) &&
		std::memcmp(prefix, checkpointMagic, 8) == 0 && GetWord(prefix + 8, 4) == 2;
}

bool ReadStreamedCheckpoint(const fs::path& directory, const std::string& name, std::size_t budget,
	Bytes& header, const std::function<bool(ICheckpointSource&, std::string&)>& read, std::string& error)
{
	SidecarReader reader;
	if (!reader.Open(directory, name, error)) return false;
	std::uint8_t prefix[20];
	if (!reader.Read(prefix, sizeof(prefix), error)) return Fail(error, "truncated checkpoint");
	if (std::memcmp(prefix, checkpointMagic, 8) != 0 || GetWord(prefix + 8, 4) != 2) return Fail(error, "not a streamed checkpoint");
	const std::uint64_t headerLength = GetWord(prefix + 12, 8);
	if (headerLength > budget || headerLength > reader.Size() - 20 || reader.Size() - 20 - headerLength < streamTrailerBytes)
		return Fail(error, "invalid or truncated checkpoint");
	const std::uint64_t streamOffset = 20 + headerLength;
	const std::uint64_t length = reader.Size() - streamOffset - streamTrailerBytes;
	// The trailer: length, digest, end magic.
	std::uint8_t trailer[streamTrailerBytes];
	if (!reader.Seek(streamOffset + length, error) || !reader.Read(trailer, sizeof(trailer), error)) return false;
	if (GetWord(trailer, 8) != length || std::memcmp(trailer + 72, streamTrailerMagic, 8) != 0)
		return Fail(error, "checkpoint trailer is invalid (an interrupted or foreign file)");
	const std::string digest(reinterpret_cast<const char*>(trailer + 8), 64);
	// Pass 1: verify the digest before the solver sees a byte.
	{
		if (!reader.Seek(streamOffset, error)) return false;
		RISECBOR64::SHA256Accumulator sha;
		std::vector<std::uint8_t> buffer(std::size_t(1) << 20);
		for (std::uint64_t left = length; left; ) {
			const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
			if (!reader.Read(buffer.data(), n, error)) return false;
			sha.Update(buffer.data(), n);
			left -= n;
		}
		if (sha.FinalHex() != digest) return Fail(error, "checkpoint state digest mismatch");
	}
	Bytes candidate(static_cast<std::size_t>(headerLength));
	if (!reader.Seek(20, error) || !reader.Read(candidate.data(), candidate.size(), error)) return false;
	header.swap(candidate);
	// Pass 2: the solver reads; the bytes it saw are hashed again.
	HashingFileSource source(reader, length);
	if (!read(source, error)) return false;
	if (source.Remaining() != 0) return Fail(error, "the solver did not consume the whole checkpoint stream");
	if (source.Digest() != digest) return Fail(error, "checkpoint changed while it was read");
	return true;
}

} }
