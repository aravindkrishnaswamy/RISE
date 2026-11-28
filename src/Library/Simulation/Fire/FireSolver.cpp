//////////////////////////////////////////////////////////////////////
//
//  FireSolver.cpp - The production methane pool fire behind the generic
//  solver contract (simulation M2, track C).  See FireSolver.h.
//
//  Compiled without fast-math or FP contraction like the fire driver: the
//  frame-clock arithmetic below must round exactly as the driver's.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireSolver.h"
#include "../SimulationGrid.h"
#include "../../Utilities/FireProductionCompute.h"
#include "../../Utilities/FireSequence.h"
#if defined(RISE_ENABLE_OPENVDB)
#include "FireRun.h"
#include "FireParameterSchema.h"   // M5c: FireEstimateCapabilities
#include "FireCPUBackend.h"        // M8b
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace RISE { namespace Simulation {
namespace {
namespace fs = std::filesystem;
using V = RISECBOR64::Value;


bool Fail(std::string& error,const std::string& message)
{
	error=message;
	return false;
}

// ---------------------------------------------------------------- options
//
// Every RISE_FIRE_* variable that changes results on the production path is a
// parameter (the list was taken from every getenv on that path in
// src/Library: FireRunDriver/FireRunFrames, FireCase, FireFastMode,
// FireProductionSourceKernel.h and the Metal force/advection sources).  The
// effective value of a text option is the exact environment text the library
// parses ("" = unset, the library default), so applying it reproduces the
// historical behaviour byte for byte.  A JSON number is converted to its
// shortest round-trip decimal text.

enum class Kind { Flag, Level, Text };
struct OptionSpec {
	const char* parameter;
	const char* environment;
	Kind kind;
	unsigned maxLevel;           // Level: 0..maxLevel, 0 = unset
	const char* defaultText;     // Text: effective value when unset ("" = none)
	bool (*valid)(const std::string&);
};

bool ParseDouble(const std::string& text,double& value)
{
	if (text.empty()) return false;
	char* end=nullptr;
	value=std::strtod(text.c_str(),&end);
	return end && *end=='\0' && end!=text.c_str();
}
bool ParseFloat(const std::string& text,float& value)
{
	if (text.empty()) return false;
	char* end=nullptr;
	value=std::strtof(text.c_str(),&end);
	return end && *end=='\0' && end!=text.c_str();
}
bool PositiveCap(const std::string& t) { double x; return ParseDouble(t,x) && !std::isnan(x) && x>0.0; }
bool UnitInterval(const std::string& t) { double x; return ParseDouble(t,x) && std::isfinite(x) && x>0.0 && x<=1.0; }
bool OpenUnit(const std::string& t) { double x; return ParseDouble(t,x) && std::isfinite(x) && x>0.0 && x<1.0; }
bool SootYield(const std::string& t) { double x; return ParseDouble(t,x) && std::isfinite(x) && x>=0.0 && x<=0.5; }
bool NonNegativeFloat(const std::string& t) { float x; return ParseFloat(t,x) && std::isfinite(x) && x>=0.0f; }
bool UnitFloat(const std::string& t) { float x; return ParseFloat(t,x) && std::isfinite(x) && x>0.0f && x<=1.0f; }
bool ZeroOrOne(const std::string& t) { return t=="0" || t=="1"; }
bool ScalarSlope(const std::string& t) { return t=="central" || t=="superbee"; }
bool WallSlope(const std::string& t) { return t=="linear"; }
bool VremanZone(const std::string& t)
{
	// The two forms FireProductionForceMac.mm accepts, consumed completely.
	char side[8]={};
	unsigned layer=0, k0=0, k1=0;
	float scale=-1.0f;
	int used=0;
	if (std::sscanf(t.c_str(),"%7[a-z]:%u:%f%n",side,&layer,&scale,&used)==3 && used==static_cast<int>(t.size()))
		return (std::string(side)=="below" || std::string(side)=="above") && layer>0u &&
			std::isfinite(scale) && scale>=0.0f && scale<=4.0f;
	used=0; scale=-1.0f;
	if (std::sscanf(t.c_str(),"ramp:%u:%u:%f%n",&k0,&k1,&scale,&used)==3 && used==static_cast<int>(t.size()))
		return k1>k0 && std::isfinite(scale) && scale>=0.0f && scale<=4.0f;
	return false;
}

const std::vector<OptionSpec>& Options()
{
	static const std::vector<OptionSpec> options={
		{"source_step_cap_s","RISE_FIRE_SOURCE_STEP_CAP_S",Kind::Text,0,"",PositiveCap},
		{"floor_emissivity","RISE_FIRE_FLOOR_EMISSIVITY",Kind::Text,0,"",UnitInterval},
		{"inert_plume","RISE_FIRE_INERT_PLUME",Kind::Flag,0,"",nullptr},
		{"inert_seed","RISE_FIRE_INERT_SEED",Kind::Text,0,"",OpenUnit},
		{"advective_cfl","RISE_FIRE_ADVECTIVE_CFL",Kind::Text,0,"0.5",UnitInterval},
		{"fast_fixed_vcycles","RISE_FIRE_FAST_FIXED_VCYCLES",Kind::Flag,0,"",nullptr},
		{"fds_mixing_time","RISE_FIRE_FDS_MIXING_TIME",Kind::Flag,0,"",nullptr},
		{"methane_soot_yield","RISE_FIRE_METHANE_SOOT_YIELD",Kind::Text,0,"",SootYield},
		{"soot_radiative_fraction_cap","RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP",Kind::Text,0,"",UnitInterval},
		{"soot_sheet_oxidation","RISE_FIRE_SOOT_SHEET_OXIDATION",Kind::Flag,0,"",nullptr},
		{"soot_self_absorption","RISE_FIRE_SOOT_SELF_ABSORPTION",Kind::Level,2,"",nullptr},
		{"scalar_vreman_cv","RISE_FIRE_SCALAR_VREMAN_CV",Kind::Text,0,"",NonNegativeFloat},
		{"fct_alpha","RISE_FIRE_FCT_ALPHA_ONE",Kind::Text,0,"",ZeroOrOne},
		{"scalar_slope","RISE_FIRE_SCALAR_SLOPE",Kind::Text,0,"",ScalarSlope},
		{"wall_slope","RISE_FIRE_WALL_SLOPE",Kind::Text,0,"",WallSlope},
		{"limited_momentum_carrier","RISE_FIRE_LIMITED_MOMENTUM_CARRIER",Kind::Flag,0,"",nullptr},
		{"vreman_zone","RISE_FIRE_VREMAN_ZONE",Kind::Text,0,"",VremanZone},
		{"compact_viscous_stencil","RISE_FIRE_COMPACT_VISCOUS_STENCIL",Kind::Flag,0,"",nullptr},
		{"near_wall_wale","RISE_FIRE_NEAR_WALL_WALE",Kind::Flag,0,"",nullptr},
		{"hyper_vreman","RISE_FIRE_HYPER_VREMAN",Kind::Text,0,"",UnitFloat},
	};
	return options;
}
constexpr const char* fastModeEnvironment = "RISE_FIRE_FAST_MODE";

#if defined(RISE_ENABLE_OPENVDB)
// M8b: how Configure hands the backend to the fire library (FireRun::FireCPUBackendRequested).
// Not a legacy default: Configure writes it from the parameter and never reads it.
// (Inside the OpenVDB block, its only user, so a build without OpenVDB has no unused constant.)
constexpr const char* backendEnvironment = "RISE_FIRE_BACKEND";
using namespace RISE::FireRun;

// M8b: the CPU backend's status evidence (docs/SIMULATION_SOLVER_CONTRACT.md section 10).
std::string CPUBackendDetail()
{
	return "experimental (M8): the r190 projected-Heun CPU owner with the r264 centred momentum carrier, "
		"selected by parameter backend \"cpu\"; deterministic at any RISE_FIRE_WORKERS count; agrees with Metal "
		"to binary32 rounding over the first steps of a coarse case (not bit-identical; trajectories decorrelate "
		"later); refuses the Metal-only opt-in operators; built and tested on macOS with and without Metal, "
		"Windows and Android not compiled";
}

constexpr char legacyName[] = "legacy";
constexpr char restoreName[] = ".rise-sim.restore.checkpoint";
constexpr char checkpointMagic[] = "RISEFSA1";
constexpr char checkpointFormat[] = "rise.fire.methane.solver-checkpoint.v1";
constexpr std::size_t frameBudget = std::size_t(1024)<<20; // the legacy importer's frame budget
constexpr double capstoneCadenceS = 0.0625;

// Environment text of an option value ("" = leave the variable unset).
std::string EnvironmentText(const OptionSpec& o,const V& value)
{
	switch (o.kind) {
	case Kind::Flag: return value.GetBoolean() ? "1" : "";
	case Kind::Level: return value.GetIntegerArgument()==0 ? "" : std::to_string(value.GetIntegerArgument());
	case Kind::Text: return value.GetText()==o.defaultText ? std::string() : value.GetText();
	}
	return {};
}

std::string ShortestText(double x)
{
	char buffer[40];
	for (int precision=1;precision<=17;++precision) {
		std::snprintf(buffer,sizeof(buffer),"%.*g",precision,x);
		if (std::strtod(buffer,nullptr)==x) break;
	}
	return buffer;
}

// Legacy environment value -> parameter value; false: malformed.
bool FromEnvironment(const OptionSpec& o,const std::string& text,V& value)
{
	switch (o.kind) {
	case Kind::Flag:
		if (text!="0" && text!="1") return false;
		value=V::Bool(text=="1");
		return true;
	case Kind::Level:
		if (text.size()!=1 || text[0]<'0' || static_cast<unsigned>(text[0]-'0')>o.maxLevel) return false;
		value=V::Unsigned(static_cast<std::uint64_t>(text[0]-'0'));
		return true;
	case Kind::Text:
		if (!o.valid(text)) return false;
		value=V::String(text);
		return true;
	}
	return false;
}

// Parameter value (CBOR from JSON or canonical CBOR) -> canonical value.
bool FromParameter(const OptionSpec& o,const V& input,V& value,std::string& error)
{
	const std::string name=std::string("parameter ")+o.parameter;
	switch (o.kind) {
	case Kind::Flag:
		if (input.GetType()==V::Boolean) { value=input; return true; }
		if (input.GetType()==V::UnsignedInteger && input.GetIntegerArgument()<=1) {
			value=V::Bool(input.GetIntegerArgument()==1); return true; }
		return Fail(error,name+" must be true or false");
	case Kind::Level:
		if (input.GetType()==V::UnsignedInteger && input.GetIntegerArgument()<=o.maxLevel) { value=input; return true; }
		return Fail(error,name+" must be an integer from 0 to "+std::to_string(o.maxLevel));
	case Kind::Text: {
		std::string text;
		if (input.GetType()==V::Text) text=input.GetText();
		else if (input.GetType()==V::UnsignedInteger) text=std::to_string(input.GetIntegerArgument());
		else if (input.GetType()==V::Float64 && std::isfinite(input.GetFloat())) text=ShortestText(input.GetFloat());
		else return Fail(error,name+" must be text or a number");
		if (!text.empty() && !o.valid(text)) return Fail(error,name+" has an invalid value '"+text+"'");
		value=V::String(text.empty() ? o.defaultText : text);
		return true;
	}
	}
	return Fail(error,name+" is not understood");
}

// ---------------------------------------------------------------- process-wide state

// The fire library caches most option values on first use, so the options
// are applied to the process environment once (transitional, see Configure).
struct ProcessState {
	std::mutex mutex;
	bool applied=false;
	std::vector<std::pair<std::string,std::string>> environment; // name -> text, "" = unset
	int live=0;
};
ProcessState& Process()
{
	static ProcessState state;
	return state;
}

bool SetVariable(const std::string& name,const std::string& text)
{
#if defined(_WIN32)
	return _putenv_s(name.c_str(),text.c_str())==0;
#else
	return text.empty() ? ::unsetenv(name.c_str())==0 : ::setenv(name.c_str(),text.c_str(),1)==0;
#endif
}

bool EnvironmentMatches(const std::vector<std::pair<std::string,std::string>>& wanted,std::string& error)
{
	for (const auto& w : wanted) {
		const char* value=std::getenv(w.first.c_str());
		const bool ok=w.second.empty() ? (!value || !*value) : (value && w.second==value);
		if (!ok) return Fail(error,"environment "+w.first+"="+(value?value:"(unset)")+
			" differs from the configured fire option ("+(w.second.empty()?"unset":w.second)+
			"); the process environment changed after Configure");
	}
	return true;
}

// ---------------------------------------------------------------- little codecs

Bytes PackDoubles(const std::vector<double>& v)
{
	Bytes out(v.size()*8);
	for (std::size_t i=0;i<v.size();++i) {
		std::uint64_t w;
		std::memcpy(&w,&v[i],8);
		for (unsigned b=0;b<8;++b) out[8*i+b]=static_cast<unsigned char>(w>>(8*b));
	}
	return out;
}
bool UnpackDoubles(const V* value,std::vector<double>& v)
{
	if (!value || value->GetType()!=V::ByteString || value->GetBytes().size()%8) return false;
	const auto& b=value->GetBytes();
	v.assign(b.size()/8,0.0);
	for (std::size_t i=0;i<v.size();++i) {
		std::uint64_t w=0;
		for (unsigned k=0;k<8;++k) w|=std::uint64_t(b[8*i+k])<<(8*k);
		std::memcpy(&v[i],&w,8);
	}
	return true;
}
Bytes PackWords(const std::vector<std::uint32_t>& v)
{
	Bytes out(v.size()*4);
	for (std::size_t i=0;i<v.size();++i) for (unsigned b=0;b<4;++b) out[4*i+b]=static_cast<unsigned char>(v[i]>>(8*b));
	return out;
}
bool UnpackWords(const V* value,std::vector<std::uint32_t>& v)
{
	if (!value || value->GetType()!=V::ByteString || value->GetBytes().size()%4) return false;
	const auto& b=value->GetBytes();
	v.assign(b.size()/4,0u);
	for (std::size_t i=0;i<v.size();++i) for (unsigned k=0;k<4;++k) v[i]|=std::uint32_t(b[4*i+k])<<(8*k);
	return true;
}
bool Real(const V& map,const char* key,double& out)
{
	const V* v=map.Find(key);
	if (!v || v->GetType()!=V::Float64) return false;
	out=v->GetFloat();
	return true;
}
bool Count(const V& map,const char* key,std::uint64_t& out)
{
	const V* v=map.Find(key);
	if (!v || v->GetType()!=V::UnsignedInteger) return false;
	out=v->GetIntegerArgument();
	return true;
}
bool Text(const V& map,const char* key,std::string& out)
{
	const V* v=map.Find(key);
	if (!v || v->GetType()!=V::Text) return false;
	out=v->GetText();
	return true;
}
bool Number(const V* v,double& x)
{
	if (!v) return false;
	if (v->GetType()==V::Float64) x=v->GetFloat();
	else if (v->GetType()==V::UnsignedInteger) x=static_cast<double>(v->GetIntegerArgument());
	else return false;
	return std::isfinite(x);
}


// Run values the fire checkpoint format does not carry, written by the loop
// and read by the legacy finish (diagnostics, summary).  Carried in this
// adapter's checkpoint envelope; the fire checkpoint format is unchanged.
V SaveSupplement(const SolverFrameValues& v)
{
	return V::MapValue({
		{"device_ms",V::BytesValue(PackDoubles(v.productionDeviceHistoryMS))},
		{"external_fuel_kg",V::Float(v.externalFuelMassKG)},
		{"hard_bound_retries",V::Unsigned(v.productionHardBoundRetryCount)},
		{"matched_schedule_flow_through_s",V::Float(v.matchedScheduleFlowThroughTimeS)},
		{"monitored_max",V::BytesValue(PackDoubles(v.monitoredManifoldMaximumHistory))},
		{"monitored_p50",V::BytesValue(PackDoubles(v.monitoredManifoldP50History))},
		{"monitored_p95",V::BytesValue(PackDoubles(v.monitoredManifoldP95History))},
		{"physical_projection_retries",V::Unsigned(v.productionPhysicalProjectionRetryCount)},
		{"pressure_tolerance_pa",V::Float(v.pressureTolerancePa)},
		{"projection_reference_length_m",V::Float(v.projectionReferenceLengthM)},
		{"projection_tolerance_per_s",V::Float(v.projectionTolerancePerS)},
		{"r246_initial_fields",V::Bool(v.r246InitialFieldsAllocated)},
		{"r246_reserve_bytes",V::Unsigned(v.r246PreallocationReserveBytes)},
		{"tail_cells",V::BytesValue(PackWords(v.monitoredTailCellHistory))},
		{"tail_drained_m3",V::BytesValue(PackDoubles(v.monitoredTailDrainedVolumeHistoryM3))},
		{"velocity_tolerance_m_per_s",V::Float(v.velocityToleranceMPerS)},
		{"wall_ms",V::BytesValue(PackDoubles(v.productionWallHistoryMS))}});
}

bool RestoreSupplement(const V& s,SolverFrameValues& v,std::string& error)
{
	const V* initial=s.Find("r246_initial_fields");
	if (s.GetType()!=V::Map || s.GetMap().size()!=17 ||
		!UnpackDoubles(s.Find("device_ms"),v.productionDeviceHistoryMS) ||
		!Real(s,"external_fuel_kg",v.externalFuelMassKG) ||
		!Count(s,"hard_bound_retries",v.productionHardBoundRetryCount) ||
		!Real(s,"matched_schedule_flow_through_s",v.matchedScheduleFlowThroughTimeS) ||
		!UnpackDoubles(s.Find("monitored_max"),v.monitoredManifoldMaximumHistory) ||
		!UnpackDoubles(s.Find("monitored_p50"),v.monitoredManifoldP50History) ||
		!UnpackDoubles(s.Find("monitored_p95"),v.monitoredManifoldP95History) ||
		!Count(s,"physical_projection_retries",v.productionPhysicalProjectionRetryCount) ||
		!Real(s,"pressure_tolerance_pa",v.pressureTolerancePa) ||
		!Real(s,"projection_reference_length_m",v.projectionReferenceLengthM) ||
		!Real(s,"projection_tolerance_per_s",v.projectionTolerancePerS) ||
		!initial || initial->GetType()!=V::Boolean ||
		!Count(s,"r246_reserve_bytes",v.r246PreallocationReserveBytes) ||
		!UnpackWords(s.Find("tail_cells"),v.monitoredTailCellHistory) ||
		!UnpackDoubles(s.Find("tail_drained_m3"),v.monitoredTailDrainedVolumeHistoryM3) ||
		!Real(s,"velocity_tolerance_m_per_s",v.velocityToleranceMPerS) ||
		!UnpackDoubles(s.Find("wall_ms"),v.productionWallHistoryMS))
		return Fail(error,"fire solver checkpoint run values are invalid");
	v.r246InitialFieldsAllocated=initial->GetBoolean();
	const auto steps=v.acceptedTimeStepHistoryS.size();
	if (v.monitoredManifoldMaximumHistory.size()!=steps || v.monitoredManifoldP95History.size()!=steps ||
		v.monitoredManifoldP50History.size()!=steps || v.monitoredTailCellHistory.size()!=steps ||
		v.monitoredTailDrainedVolumeHistoryM3.size()!=steps || v.productionDeviceHistoryMS.size()!=steps ||
		v.productionWallHistoryMS.size()!=steps)
		return Fail(error,"fire solver checkpoint run values do not cover the accepted steps");
	return true;
}

// Sequence envelope without a frame list: the capstone's sealed manifest for
// this case and frame clock, minus "frames", "frame_count", "first_frame_index"
// and "last_frame_index" (FireSequenceManifest::LoadFrameTemplate).
bool BuildFrameTemplate(const ProductionTemporalLaunch& launch,double tier,double originS,double cadenceS,
	Bytes& envelope,std::string& error)
{
	const auto& c=launch.launchCase;
	const std::array<std::uint64_t,3> dimensions={{c.dimensions[0],c.dimensions[1],c.dimensions[2]}};
	const Bytes manifest=ManifestBytes("","","hold",true,true,dimensions,c.cellWidthM,originS,cadenceS,1.0,0.0,
		true,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,0.0,tier,true,
		{{"frames/frame_00000.vdb",std::string(64,'0')}},launch.window.targetTimeS,1.0/cadenceS,true,false,false);
	V root;
	if (manifest.empty() || !RISECBOR64::DecodeCanonical(manifest,root,&error) || root.GetType()!=V::Map ||
		!root.Find("payload") || root.Find("payload")->GetType()!=V::Map)
		return Fail(error,"cannot derive the fire sequence record: "+error);
	V::Members payload;
	for (const auto& m : root.Find("payload")->GetMap())
		if (m.first!="frames" && m.first!="frame_count" && m.first!="first_frame_index" && m.first!="last_frame_index")
			payload.push_back(m);
	Bytes payloadBytes;
	if (!RISECBOR64::Encode(V::MapValue(payload),payloadBytes,&error)) return false;
	return RISECBOR64::Encode(V::MapValue({{"payload",V::MapValue(payload)},
		{"sequence_id",V::String(RISECBOR64::SHA256Hex(payloadBytes))}}),envelope,&error);
}

void RemoveWithSidecar(const fs::path& path)
{
	std::error_code ec;
	fs::remove(path,ec);
	fs::remove(path.string()+".payload-v2.json",ec);
}

#if !defined(_WIN32)
extern "C" void* FireSolverThreadEntry(void* argument)
{
	auto* body=static_cast<std::function<void()>*>(argument);
	(*body)();
	return nullptr;
}
#endif


// ---------------------------------------------------------------- the solver

class FireSolver final : public ISimulationSolver {
public:
	FireSolver();
	~FireSolver() override;
	bool Configure(const RISECBOR64::Value& parameters,const ISceneInputs& inputs,SolverDescription& description,
		std::string& error) override;
	bool SetSideOutputDirectory(const fs::path& directory,std::string& error) override;
	bool Start(std::string& error) override;
	bool Restore(const Bytes& checkpoint,std::string& error) override;
	bool Advance(const AdvanceRequest& request,IFrameSink& sink,IPortExchange* ports,IProgress* progress,
		AdvanceResult& result,std::string& error) override;
	bool Checkpoint(Bytes& state,std::string& error) override;
	SolverPosition Position() const override;
	void RequestStop() override { stop_.store(true); }

private:
	enum class Phase { Unconfigured, Configured, Ready, Running, Parked, Returned, Finished, Failed };

	bool Startable(std::string& error) const;
	std::string ConfigurationDigest() const;
	void JoinThread();
	SolverPosition PositionLocked() const { return SolverPosition{time_,next_,finished_}; }
	bool AppendTrailer(Bytes& state,const char* kind,const V& values,std::uint64_t fireBytes,std::string& error) const;
	bool PrepareLaunch(bool fresh,std::string& error);
	bool OnStep(const FireRun::FireRunStepRendezvous& step);
	bool LaunchThread(std::string& error);
	bool PublishFrame(std::uint64_t index,std::uint64_t steps,IFrameSink& sink,std::string& error);
	FireRun::FireCapstoneOptions capstone_;
	FireRun::ProductionTemporalLaunch launch_;
	FireRun::SolverFrameValues returned_;
	Implementation::FireSequenceManifest template_;
	// configuration
	V defaults_ = V::MapValue({});                                   // parameter defaults from the legacy environment
	std::vector<std::pair<std::string,std::string>> fromEnvironment_; // parameter <- "VARIABLE=value"
	std::string environmentError_;
	V effective_;
	std::vector<std::pair<std::string,std::string>> environment_;     // variable -> applied text ("" unset)
	double tier_=8, originS_=0, endS_=0, targetS_=0, statisticsStartS_=0;
	bool fast_=false, partialWindow_=false, metal_=false, cpu_=false;
	std::string metalDetail_;
	std::uint64_t frames_=0;
	fs::path side_;

	// run state; guarded by mutex_ once the driver thread exists
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	Phase phase_=Phase::Unconfigured;
	bool release_=false, abandon_=false, restored_=false;
	std::atomic<bool> stop_{false};
	const void* parked_=nullptr;     // the FireRunStepRendezvous the driver waits in
	bool parkedFrame_=false;
	std::uint64_t parkedFrameIndex_=0, steps_=0;
	double time_=0;
	std::int64_t next_=0;
	bool finished_=false;
	Bytes supplement_;               // restored run values (canonical CBOR)
	std::chrono::steady_clock::time_point wallStart_;
	bool threadStarted_=false;
	std::function<void()> body_;
#if !defined(_WIN32)
	pthread_t thread_{};
#endif
};

FireSolver::FireSolver()
{
	{
		std::lock_guard<std::mutex> hold(Process().mutex);
		++Process().live;
	}
	// The legacy environment, read once here at the factory boundary, supplies
	// parameter defaults; explicit parameters win.
	V::Members members;
	for (const auto& o : Options()) {
		const char* text=std::getenv(o.environment);
		if (!text || !*text) continue;
		V value;
		if (!FromEnvironment(o,text,value)) {
			environmentError_="legacy environment "+std::string(o.environment)+"="+text+" is not a valid "+o.parameter;
			continue;
		}
		members.emplace_back(o.parameter,value);
		fromEnvironment_.emplace_back(o.parameter,std::string(o.environment)+"="+text);
	}
	if (const char* text=std::getenv(fastModeEnvironment); text && *text) {
		if (std::strcmp(text,"0")!=0 && std::strcmp(text,"1")!=0)
			environmentError_=std::string("legacy environment ")+fastModeEnvironment+"="+text+" is not 0 or 1";
		else {
			members.emplace_back("mode",V::String(std::strcmp(text,"1")==0 ? "fast" : "certified"));
			fromEnvironment_.emplace_back("mode",std::string(fastModeEnvironment)+"="+text);
		}
	}
	defaults_=V::MapValue(members);
}

FireSolver::~FireSolver()
{
	JoinThread();
	std::lock_guard<std::mutex> hold(Process().mutex);
	--Process().live;
}

void FireSolver::JoinThread()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!threadStarted_) return;
		abandon_=true;
	}
	wake_.notify_all();
#if !defined(_WIN32)
	pthread_join(thread_,nullptr);
#endif
	std::lock_guard<std::mutex> lock(mutex_);
	threadStarted_=false;
}

std::string FireSolver::ConfigurationDigest() const
{
	Bytes bytes;
	std::string ignored;
	RISECBOR64::Encode(V::MapValue({{"parameters",effective_},{"solver",V::String(Implementation::FireMethaneSolverId)},
		{"version",V::String(Implementation::FireMethaneSolverVersion)}}),bytes,&ignored);
	return RISECBOR64::SHA256Hex(bytes);
}

bool FireSolver::Configure(const V& parameters,const ISceneInputs&,SolverDescription& d,std::string& error)
{
	{
		std::lock_guard<std::mutex> hold(Process().mutex);
		if (Process().live>1) return Fail(error,"only one fire solver may exist in a process (the fire library keeps process-wide state)");
	}
	if (phase_!=Phase::Unconfigured && phase_!=Phase::Configured) return Fail(error,"fire solver is already running");
	if (!environmentError_.empty()) return Fail(error,environmentError_);
	if (parameters.GetType()!=V::Map) return Fail(error,"parameters must be a map");
	std::vector<std::string> known={"tier","frame_cadence_s","frame_origin_s","end_time_s","pool_diameter_m",
		"heat_release_rate_kw","mode","backend"};
	for (const auto& o : Options()) known.emplace_back(o.parameter);
	for (const auto& m : parameters.GetMap())
		if (std::find(known.begin(),known.end(),m.first)==known.end()) return Fail(error,"unknown parameter "+m.first);
	auto given=[&](const char* key) { return parameters.Find(key); };

	// The case.  Only the production capstone's case is supported for now.
	double x=8;
	// M8b: tier 4 is a coarse smoke tier (35 x 35 x 53 cells) for the CPU backend's tests and the
	// Metal/CPU agreement check; it is not a capstone tier and scenes do not offer it.
	if (const V* v=given("tier"); v && (!Number(v,x) || (x!=4 && x!=6 && x!=8 && x!=10)))
		return Fail(error,"parameter tier must be 6, 8 or 10 (or 4, the coarse smoke tier)");
	tier_=x;
	double cadence=capstoneCadenceS;
	if (const V* v=given("frame_cadence_s"); v && (!Number(v,cadence) || cadence!=capstoneCadenceS))
		return Fail(error,"parameter frame_cadence_s must be 0.0625 (the production frame cadence)");
	double diameter=0.30, hrr=33.0;
	if (const V* v=given("pool_diameter_m"); v && (!Number(v,diameter) || diameter!=0.30))
		return Fail(error,"parameter pool_diameter_m: only the certified 0.30 m pool is supported");
	if (const V* v=given("heat_release_rate_kw"); v && (!Number(v,hrr) || hrr!=33.0))
		return Fail(error,"parameter heat_release_rate_kw: only the certified 33 kW fire is supported");
	std::string mode="certified";
	if (const V* v=given("mode") ? given("mode") : defaults_.Find("mode")) {
		if (v->GetType()!=V::Text || (v->GetText()!="certified" && v->GetText()!="fast"))
			return Fail(error,"parameter mode must be \"certified\" or \"fast\"");
		mode=v->GetText();
	}
	fast_=mode=="fast";
	// M8b: the backend the step runs on. Absent = "metal" (unchanged keys); "cpu" selects the
	// experimental CPU backend and enters the effective parameters (the bytes differ).
	cpu_=false;
	if (const V* v=given("backend")) {
		if (v->GetType()!=V::Text || (v->GetText()!="metal" && v->GetText()!="cpu"))
			return Fail(error,"parameter backend must be \"metal\" or \"cpu\"");
		cpu_=v->GetText()=="cpu";
	}
	// Result-affecting options: explicit > legacy environment > library default.
	V::Members options;
	std::vector<std::pair<std::string,std::string>> environment;
	std::vector<std::string> notes;
	for (const auto& o : Options()) {
		V value;
		if (const V* v=given(o.parameter)) { if (!FromParameter(o,*v,value,error)) return false; }
		else if (const V* v=defaults_.Find(o.parameter)) {
			value=*v;
			for (const auto& f : fromEnvironment_) if (f.first==o.parameter) notes.push_back(std::string(o.parameter)+" from legacy "+f.second);
		}
		else value=o.kind==Kind::Flag ? V::Bool(false) : o.kind==Kind::Level ? V::Unsigned(0) : V::String(o.defaultText);
		options.emplace_back(o.parameter,value);
		environment.emplace_back(o.environment,EnvironmentText(o,value));
	}
	if (!given("mode")) for (const auto& f : fromEnvironment_) if (f.first=="mode") notes.push_back("mode from legacy "+f.second);
	environment.emplace_back(fastModeEnvironment,fast_ ? "1" : "");
	environment.emplace_back(backendEnvironment,cpu_ ? "cpu" : "");
	auto option=[&](const char* name) -> const V& {
		for (const auto& m : options) if (m.first==name) return m.second;
		return options.front().second;
	};
	const bool compact=option("compact_viscous_stencil").GetBoolean();
	if (option("fast_fixed_vcycles").GetBoolean() && !fast_) return Fail(error,"fast_fixed_vcycles needs mode \"fast\"");
	if (!compact && (!option("vreman_zone").GetText().empty() || option("near_wall_wale").GetBoolean() ||
		!option("hyper_vreman").GetText().empty()))
		return Fail(error,"vreman_zone, near_wall_wale and hyper_vreman need compact_viscous_stencil");

	// TRANSITIONAL (M2): the fire library still reads these options from the
	// process environment, much of it once per process, below this adapter.
	// The effective options are written to the environment here, once per
	// process and before anything in the fire library reads them; Start and
	// Restore verify that the environment still holds exactly these values.
	{
		std::lock_guard<std::mutex> hold(Process().mutex);
		auto& p=Process();
		if (p.applied && p.environment!=environment)
			return Fail(error,"this process already runs fire with other options; one fire configuration per process");
		if (!p.applied) {
			for (const auto& e : environment)
				if (!SetVariable(e.first,e.second)) return Fail(error,"cannot set "+e.first);
			p.applied=true;
			p.environment=environment;
		}
	}
	environment_=environment;
	if (!EnvironmentMatches(environment_,error)) return false;
	notes.push_back("transitional: the result-affecting options are applied to the process environment "
		"(RISE_FIRE_*) once per process; the fire library still reads them there");

	FireProductionComputeCapability capability;
	QueryFireProductionComputeCapability(capability);
	metal_=capability.available;
	metalDetail_=capability.available ? capability.deviceName : capability.structuredError;

	capstone_=FireRun::FireCapstoneOptions();
	capstone_.resolutionTier=tier_;
	capstone_.frameCadenceS=cadence;
	capstone_.outputDirectory=fs::path("unbound-side-output");
	const int planned=FireRun::PlanProductionTemporalLaunch(capstone_,launch_);
	if (planned==91) return Fail(error,"the production fire launch refuses this environment (diagnostic RISE_FIRE_* "
		"variables such as RISE_FIRE_TEMPORAL_RESUME or RISE_FIRE_PROFILE are set) or tier");
	if (planned!=0) return Fail(error,"the production fire case could not be derived: "+launch_.launchCase.structuredError);
	statisticsStartS_=launch_.window.statisticsStartS;
	targetS_=launch_.window.targetTimeS;
	originS_=statisticsStartS_;
	if (const V* v=given("frame_origin_s"); v && (!Number(v,originS_) || originS_<0 || !(originS_<targetS_)))
		return Fail(error,"parameter frame_origin_s must be a time in [0, target time)");
	endS_=targetS_;
	if (const V* v=given("end_time_s"); v && (!Number(v,endS_) || !(endS_>0) || endS_>targetS_ || endS_<originS_))
		return Fail(error,"parameter end_time_s must lie in [frame_origin_s, target time]");
	// The frame clock exactly as the driver advances it: origin + index * cadence.
	frames_=0;
	while (frames_<(std::uint64_t(1)<<24) && originS_+static_cast<double>(frames_)*cadence<=endS_) ++frames_;
	if (frames_==0) return Fail(error,"no frame lies in [frame_origin_s, end_time_s]");
	partialWindow_=endS_!=targetS_ || originS_!=statisticsStartS_;

	V::Members members={
		{"end_time_s",V::Float(endS_)},{"frame_cadence_s",V::Float(cadence)},{"frame_origin_s",V::Float(originS_)},
		{"heat_release_rate_kw",V::Float(hrr)},{"mode",V::String(mode)},{"pool_diameter_m",V::Float(diameter)},
		{"tier",V::Unsigned(static_cast<std::uint64_t>(tier_))}};
	if (cpu_) members.emplace_back("backend",V::String("cpu"));
	for (auto& m : options) members.push_back(m);
	effective_=V::MapValue(members);

	Bytes envelope;
	if (!BuildFrameTemplate(launch_,tier_,originS_,cadence,envelope,error)) return false;
	if (!template_.LoadFrameTemplate(envelope,error)) return Fail(error,"fire sequence record refused: "+error);

	SolverDescription out;
	out.solverId=Implementation::FireMethaneSolverId;
	out.version=Implementation::FireMethaneSolverVersion;
	out.effectiveParameters=effective_;
	auto& c=out.cache;
	c.firstIndex=0;
	c.count=frames_;
	c.startTime=originS_;
	c.interval=cadence;
	c.sceneOrigin=0;
	c.timeScale=1;
	c.hold=true;
	CacheOutput volume{"volume","grid.v1",{}};
	for (const auto& ch : template_.Channels()) volume.channels.push_back({ch.name,ch.units,ch.valueType});
	c.outputs={volume};
	c.physicalRecords=envelope;
	std::string cpuDetail;
	const bool cpuAvailable=FireRun::FireCPUBackendOptionsSupported(cpuDetail);
	out.capabilities.backends={
		{"metal",BackendStatus::Supported,metal_,metalDetail_},
		{"cpu",BackendStatus::Experimental,cpuAvailable,cpuAvailable ? CPUBackendDetail() :
			CPUBackendDetail()+"; not available with these options: "+cpuDetail}};
	out.capabilities.interactive=false;
	// Certified mode is bit-reproducible (r274/M0 identity checks); Metal fast mode is
	// not claimed to be (FireFastMode.h: "not bit-reproducible"). The CPU backend runs
	// the same strict binary32 C++ in both modes, identical at any worker count (M8b).
	out.capabilities.deterministic=cpu_ || !fast_;
	out.capabilities.resumable=true;
	out.capabilities.externalMacroStep=false;
	out.capabilities.modes={"certified","fast"};
	out.capabilities.backend=cpu_ ? "cpu" : "metal";   // M4: the backend this configuration runs on (not keyed)
	FireEstimateCapabilities(tier_,fast_,launch_.launchCase.dimensions,out.capabilities,cpu_);   // M5c: not keyed
	out.capabilities.earnableFidelity={"preview"};
	out.capabilities.fidelityLimit="no computed, digest-bound validation case exists for a fire configuration yet";
	// The sealed fire sequences are preview_only; no computed, digest-bound
	// validation case exists for a fire configuration yet.
	out.evidence.fidelity="preview";
	out.sideOutput=legacyName;
	out.notes=notes;
	d=std::move(out);
	phase_=Phase::Configured;
	return true;
}

bool FireSolver::SetSideOutputDirectory(const fs::path& directory,std::string& error)
{
	if (phase_!=Phase::Configured) return Fail(error,"side output directory set outside Configure..Start");
	side_=directory;
	return true;
}

bool FireSolver::Startable(std::string& error) const
{
	if (phase_!=Phase::Configured) return Fail(error,"fire solver must be configured once before Start or Restore");
	if (side_.empty()) return Fail(error,"the fire solver needs a side-output directory for its legacy frames "
		"(run it through rise-sim)");
	if (cpu_) {
		std::string why;
		if (!FireRun::FireCPUBackendOptionsSupported(why)) return Fail(error,why);
	} else if (!metal_) return Fail(error,"the fire solver needs its Metal backend, unavailable here: "+metalDetail_+
		" (parameter backend \"cpu\" selects the experimental CPU backend)");
	return EnvironmentMatches(environment_,error);
}

bool FireSolver::PrepareLaunch(bool fresh,std::string& error)
{
	capstone_.outputDirectory=side_;
	capstone_.sealPartialWindow=partialWindow_;
	const auto configured=launch_;
	FireRun::ProductionTemporalLaunch launch;
	if (FireRun::PlanProductionTemporalLaunch(capstone_,launch)!=0 ||
		launch.launchCase.caseRecordId!=configured.launchCase.caseRecordId ||
		launch.window.targetTimeS!=configured.window.targetTimeS)
		return Fail(error,"the fire case changed between Configure and Start");
	auto& p=launch.persistence;
	// No wall-clock checkpoints inside the driver: the host checkpoints through
	// Checkpoint, and a wall-time cadence would make the run's checkpoint step
	// list depend on timing.
	p.checkpointPath.clear();
	p.retainedCheckpointDirectory.clear();
	p.hasTemporalFrameOrigin=originS_!=statisticsStartS_;
	p.temporalFrameOriginS=p.hasTemporalFrameOrigin ? originS_ : 0.0;
	if (endS_!=targetS_) { p.windowEndTimeS=endS_; p.finalCheckpointAtWindowEnd=true; }
	p.acceptedStepRendezvous=[this](const FireRun::FireRunStepRendezvous& step) { return OnStep(step); };
	std::error_code ec;
	if (fresh) {
		if (fs::directory_iterator(side_,ec)!=fs::directory_iterator() || ec)
			return Fail(error,"the legacy side-output directory is not empty: "+side_.string());
		for (const char* sub : {"checkpoints","frames","budgets"}) fs::create_directories(side_/sub,ec);
		if (ec) return Fail(error,"cannot create the legacy run directory: "+ec.message());
		launch_=launch;
		if (FireRun::WriteProductionTemporalLaunchReceipt(capstone_,launch_)!=0)
			return Fail(error,"cannot publish the legacy launch receipt");
		return true;
	}
	if (!fs::is_regular_file(side_/"window_launch.v1") || !fs::is_directory(side_/"checkpoints"))
		return Fail(error,"the legacy run directory of this cache is missing or incomplete: "+side_.string());
	if (fs::exists(side_/"sequence_manifest.rise-fire.cbor") || fs::exists(side_/"production_capstone_summary.txt"))
		return Fail(error,"the legacy export was already sealed but the cache was not; rebake with --force");
	// Outputs past the restored state belong to the interrupted continuation
	// and are produced again (identically in certified mode).
	for (std::uint64_t index=static_cast<std::uint64_t>(next_);;++index) {
		std::ostringstream stem;
		stem<<"frame_"<<std::setw(5)<<std::setfill('0')<<index;
		const auto vdb=side_/"checkpoints"/(stem.str()+".vdb");
		if (!fs::exists(vdb) && !fs::exists(side_/"checkpoints"/(stem.str()+".time"))) break;
		RemoveWithSidecar(vdb);
		RemoveWithSidecar(side_/"checkpoints"/(stem.str()+".time"));
	}
	RemoveWithSidecar(side_/"production_final.checkpoint");
	p.generalResume=true;
	p.generalResumeCheckpointPath=side_/restoreName;
	p.restoreValues=[this](FireRun::SolverFrameValues& values,std::string& why) {
		std::error_code ignored;
		fs::remove(side_/restoreName,ignored);
		V supplement;
		if (!RISECBOR64::DecodeCanonical(supplement_,supplement,&why) || !RestoreSupplement(supplement,values,why))
			return false;
		if (values.streamedFrameCount!=static_cast<std::uint64_t>(next_) || values.simulatedTimeS!=time_)
			return Fail(why,"restored fire state disagrees with the solver checkpoint header");
		return true;
	};
	launch_=launch;
	return true;
}

bool FireSolver::Start(std::string& error)
{
	if (!Startable(error) || !PrepareLaunch(true,error)) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	time_=0; next_=0; steps_=0; finished_=false; restored_=false;
	phase_=Phase::Ready;
	return true;
}

bool FireSolver::Restore(const Bytes& checkpoint,std::string& error)
{
	if (!Startable(error)) return false;
	if (checkpoint.size()<16 || std::memcmp(checkpoint.data()+checkpoint.size()-8,checkpointMagic,8)!=0)
		return Fail(error,"not a fire solver checkpoint");
	std::uint64_t length=0;
	for (unsigned i=0;i<8;++i) length|=std::uint64_t(checkpoint[checkpoint.size()-16+i])<<(8*i);
	if (length>checkpoint.size()-16) return Fail(error,"truncated fire solver checkpoint");
	const std::size_t fireBytes=checkpoint.size()-16-static_cast<std::size_t>(length);
	const Bytes trailer(checkpoint.begin()+static_cast<std::ptrdiff_t>(fireBytes),checkpoint.end()-16);
	V t;
	std::string format,configuration,kind,digest;
	std::uint64_t next=0,steps=0,size=0;
	double time=0;
	if (!RISECBOR64::DecodeCanonical(trailer,t,&error) || t.GetType()!=V::Map || !Text(t,"format",format) ||
		format!=checkpointFormat || !Text(t,"configuration",configuration) || !Text(t,"state",kind) ||
		!Count(t,"next",next) || !Count(t,"steps",steps) || !Real(t,"time",time) || !Count(t,"fire_size",size) ||
		!Text(t,"fire_sha256",digest) || !t.Find("values"))
		return Fail(error,"invalid fire solver checkpoint header");
	if (configuration!=ConfigurationDigest()) return Fail(error,"the checkpoint belongs to another fire configuration");
	if (size!=fireBytes || RISECBOR64::SHA256Hex(checkpoint.data(),fireBytes)!=digest)
		return Fail(error,"fire solver checkpoint state digest mismatch");
	if (kind=="initial") {
		if (fireBytes!=0 || steps!=0 || time!=0.0 || (next!=0 && !(originS_==0.0 && next==1)))
			return Fail(error,"invalid initial fire solver checkpoint");
		// Nothing was accepted yet: begin the run again from an empty directory.
		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(side_,ec)) fs::remove_all(entry.path(),ec);
		if (!Start(error)) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		next_=static_cast<std::int64_t>(next);
		return true;
	}
	if (kind!="running" || fireBytes==0 || next>frames_) return Fail(error,"invalid fire solver checkpoint state");
	if (!RISECBOR64::Encode(*t.Find("values"),supplement_,&error)) return false;
	{
		std::ofstream out(side_/restoreName,std::ios::binary|std::ios::trunc);
		out.write(reinterpret_cast<const char*>(checkpoint.data()),static_cast<std::streamsize>(fireBytes));
		out.close();
		if (!out) return Fail(error,"cannot stage the fire checkpoint for the driver");
	}
	next_=static_cast<std::int64_t>(next); time_=time; steps_=steps; finished_=false;
	if (!PrepareLaunch(false,error)) {
		std::error_code ignored;
		fs::remove(side_/restoreName,ignored);
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	restored_=true;
	phase_=Phase::Ready;
	return true;
}

// Driver thread, after every accepted step.  Parks at every published frame
// and, on a stop request, at the next step that is not the last.
bool FireSolver::OnStep(const FireRun::FireRunStepRendezvous& step)
{
	std::unique_lock<std::mutex> lock(mutex_);
	time_=step.simulationTimeS;
	steps_=step.acceptedSteps;
	if (abandon_) return false;
	// Rebuilding from the seed recreates the legacy file. The cache already owns this frame.
	if (step.framePublished && step.acceptedSteps==0 && next_==1) return true;
	if (!step.framePublished && !(stop_.load() && !step.lastStep)) return true;
	parked_=&step;
	parkedFrame_=step.framePublished;
	parkedFrameIndex_=step.frameIndex;
	phase_=Phase::Parked;
	wake_.notify_all();
	wake_.wait(lock,[&]{ return release_ || abandon_; });
	release_=false;
	parked_=nullptr;
	parkedFrame_=false;
	if (phase_==Phase::Parked) phase_=Phase::Running;
	return !abandon_;
}

bool FireSolver::LaunchThread(std::string& error)
{
	body_=[this]{
		const double cadence=capstone_.frameCadenceS;
		auto result=FireRun::RunFireMethaneDriver(launch_.workers,1u,targetS_,targetS_,1.0/cadence,tier_,
			FireRun::CapstonePoolDiameterM,FireRun::CapstoneHeatReleaseRateKW,launch_.persistence);
		std::lock_guard<std::mutex> lock(mutex_);
		returned_=std::move(result);
		phase_=Phase::Returned;
		wake_.notify_all();
	};
	wallStart_=std::chrono::steady_clock::now();
#if defined(_WIN32)
	return Fail(error,"the fire solver's driver thread is not implemented on Windows yet");
#else
	// The driver keeps large frames on its stack; give it room like a main thread.
	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes)!=0) return Fail(error,"cannot create the fire driver thread");
	pthread_attr_setstacksize(&attributes,std::size_t(256)<<20);
	const int rc=pthread_create(&thread_,&attributes,FireSolverThreadEntry,&body_);
	pthread_attr_destroy(&attributes);
	if (rc!=0) return Fail(error,"cannot start the fire driver thread");
	threadStarted_=true;
	return true;
#endif
}

bool FireSolver::PublishFrame(std::uint64_t index,std::uint64_t steps,IFrameSink& sink,std::string& error)
{
	std::ostringstream stem;
	stem<<"frame_"<<std::setw(5)<<std::setfill('0')<<index;
	const auto vdb=side_/"checkpoints"/(stem.str()+".vdb");
	double time=0;
	std::string digest;
	{
		std::ifstream record(side_/"checkpoints"/(stem.str()+".time"));
		if (!(record>>time>>digest) || digest.size()!=64 || !std::isfinite(time))
			return Fail(error,"legacy frame record missing for "+stem.str());
	}
	// The legacy importer's own conversion (FireSequenceManifest::ReadVDBFrame),
	// so the native payload is byte-identical to importing the sealed sequence.
	GridFrame grid;
	Bytes payload,receipt;
	if (!template_.LoadVDBFileGrid(vdb.string(),digest,grid,error) || !EncodeGrid(grid,frameBudget,payload,error) ||
		!RISECBOR64::Encode(V::MapValue({{"accepted_step",V::Unsigned(steps)},
			{"format",V::String(Implementation::FireMethaneFrameReceiptFormat)},
			{"legacy_frame",V::String("frames/"+stem.str()+".vdb")},{"legacy_sha256",V::String(digest)}}),receipt,&error))
		return Fail(error,"frame "+std::to_string(index)+": "+error);
	return sink.Publish(static_cast<std::int64_t>(index),"volume",time,
		std::make_shared<const Bytes>(std::move(payload)),receipt,error);
}

bool FireSolver::Advance(const AdvanceRequest& request,IFrameSink& sink,IPortExchange*,IProgress* progress,
	AdvanceResult& result,std::string& error)
{
	if (request.mode!=AdvanceRequest::Mode::NextOutput) return Fail(error,"the fire solver does not take an external macro step");
	std::unique_lock<std::mutex> lock(mutex_);
	result=AdvanceResult();
	if (phase_==Phase::Finished) { result.status=AdvanceResult::Status::Finished; result.position=PositionLocked(); return true; }
	if (phase_!=Phase::Ready && phase_!=Phase::Parked) return Fail(error,"Advance needs Start or Restore (or follows a failure)");
	// RequestStop is sticky: a request made between calls stops this one; it
	// is cleared only where an Advance returns Stopped (below).
	auto failed=[&](const std::string& message) { phase_=Phase::Failed; return Fail(error,message); };
	if (phase_==Phase::Ready) {
		phase_=Phase::Running;
		lock.unlock();
		std::string why;
		const bool launched=LaunchThread(why);
		lock.lock();
		if (!launched) return failed(why);
	} else {
		release_=true;
		phase_=Phase::Running;
		wake_.notify_all();
	}
	for (;;) {
		wake_.wait_for(lock,std::chrono::milliseconds(250),[&]{ return phase_==Phase::Parked || phase_==Phase::Returned; });
		if (phase_==Phase::Running) {
			if (progress) {
				std::ostringstream detail;
				detail<<"accepted step "<<steps_<<", t="<<std::setprecision(6)<<time_<<" s, frame "<<next_<<"/"<<frames_;
				const double now=time_;
				lock.unlock();
				const bool go=progress->Report(now,endS_,detail.str());
				lock.lock();
				if (!go) stop_.store(true);
			}
			continue;
		}
		if (phase_==Phase::Parked) {
			if (!parkedFrame_) {
				stop_.store(false); // the stop request is consumed here
				result.status=AdvanceResult::Status::Stopped;
				result.position=PositionLocked();
				return true;
			}
			const std::uint64_t index=parkedFrameIndex_, steps=steps_;
			if (index!=static_cast<std::uint64_t>(next_) || index>=frames_)
				return failed("the fire run published frame "+std::to_string(index)+" where frame "+std::to_string(next_)+
					" of "+std::to_string(frames_)+" was due (an end time just before a frame time can produce one "
					"frame past the window)");
			lock.unlock();
			std::string why;
			const bool published=PublishFrame(index,steps,sink,why);
			lock.lock();
			if (!published) return failed(why);
			parkedFrame_=false;
			next_=static_cast<std::int64_t>(index+1);
			++result.framesPublished;
			if (static_cast<std::uint64_t>(next_)<frames_) {
				result.status=AdvanceResult::Status::Advanced;
				result.position=PositionLocked();
				return true;
			}
			// Every frame is out: finish the window (statistics, final state).
			release_=true;
			phase_=Phase::Running;
			wake_.notify_all();
			continue;
		}
		// The driver returned.
		lock.unlock();
#if !defined(_WIN32)
		pthread_join(thread_,nullptr);
#endif
		lock.lock();
		threadStarted_=false;
		if (!returned_.succeeded) return failed("fire run failed: "+returned_.structuredError);
		if (static_cast<std::uint64_t>(next_)!=frames_)
			return failed("fire run ended after "+std::to_string(next_)+" of "+std::to_string(frames_)+" frames");
		const double wallS=std::chrono::duration<double>(std::chrono::steady_clock::now()-wallStart_).count();
		const int finished=FireRun::FinishProductionTemporalRun(capstone_,launch_,returned_,wallS);
		if (finished!=0) return failed("legacy export finish failed with code "+std::to_string(finished));
		time_=returned_.simulatedTimeS;
		finished_=true;
		phase_=Phase::Finished;
		result.status=AdvanceResult::Status::Finished;
		result.position=PositionLocked();
		return true;
	}
}

bool FireSolver::AppendTrailer(Bytes& state,const char* kind,const V& values,std::uint64_t fireBytes,
	std::string& error) const
{
	Bytes trailer;
	if (!RISECBOR64::Encode(V::MapValue({{"configuration",V::String(ConfigurationDigest())},
		{"fire_sha256",V::String(RISECBOR64::SHA256Hex(state.data(),static_cast<std::size_t>(fireBytes)))},
		{"fire_size",V::Unsigned(fireBytes)},{"format",V::String(checkpointFormat)},
		{"next",V::Unsigned(static_cast<std::uint64_t>(next_))},{"state",V::String(kind)},{"steps",V::Unsigned(steps_)},
		{"time",V::Float(time_)},{"values",values}}),trailer,&error)) return false;
	state.insert(state.end(),trailer.begin(),trailer.end());
	for (unsigned i=0;i<8;++i) state.push_back(static_cast<unsigned char>(std::uint64_t(trailer.size())>>(8*i)));
	state.insert(state.end(),checkpointMagic,checkpointMagic+8);
	return true;
}

bool FireSolver::Checkpoint(Bytes& state,std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex_);
	state.clear();
	if ((phase_==Phase::Ready && !restored_) || (phase_==Phase::Parked && steps_==0))
		return AppendTrailer(state,"initial",V::MapValue({}),0,error);
	if (phase_==Phase::Ready) {
		// Restored but not advanced yet: the staged bytes are the state.
		std::ifstream in(side_/restoreName,std::ios::binary);
		state.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
		V supplement;
		if (state.empty() || !RISECBOR64::DecodeCanonical(supplement_,supplement,&error))
			return Fail(error,"restored fire state is no longer staged");
		const auto size=state.size();
		return AppendTrailer(state,"running",supplement,size,error);
	}
	if (phase_!=Phase::Parked || !parked_) return Fail(error,"fire checkpoint is valid only between Advance calls");
	// The driver thread is blocked in OnStep: its state may be read here.
	const auto* step=static_cast<const FireRun::FireRunStepRendezvous*>(parked_);
	const V supplement=SaveSupplement(*step->values);
	if (!(*step->checkpoint)(state,error)) return Fail(error,"fire checkpoint failed: "+error);
	const auto size=state.size();
	return AppendTrailer(state,"running",supplement,size,error);
}

SolverPosition FireSolver::Position() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return PositionLocked();
}

#else // !RISE_ENABLE_OPENVDB

// Builds without OpenVDB cannot write the fire's legacy frames, which the
// native frames are converted from: the solver registers and refuses.
class FireSolver final : public ISimulationSolver {
public:
	bool Configure(const RISECBOR64::Value&,const ISceneInputs&,SolverDescription&,std::string& error) override
		{ return Fail(error,Unavailable()); }
	bool Start(std::string& error) override { return Fail(error,Unavailable()); }
	bool Restore(const Bytes&,std::string& error) override { return Fail(error,Unavailable()); }
	bool Advance(const AdvanceRequest&,IFrameSink&,IPortExchange*,IProgress*,AdvanceResult&,std::string& error) override
		{ return Fail(error,Unavailable()); }
	bool Checkpoint(Bytes&,std::string& error) override { return Fail(error,Unavailable()); }
	SolverPosition Position() const override { return SolverPosition(); }
	void RequestStop() override {}
private:
	static std::string Unavailable()
	{
		return "the fire solver needs a build with OpenVDB (its legacy frame export); this build has no "
			"OpenVDB (neither the Metal nor the experimental CPU backend can run here)";
	}
};
#endif // RISE_ENABLE_OPENVDB

} // namespace

std::vector<FireSolverOptionInfo> FireSolverOptionInfos()
{
	std::vector<FireSolverOptionInfo> out;
	for (const auto& o : Options())
		out.push_back({o.parameter,o.environment,o.kind==Kind::Flag ? "flag" : o.kind==Kind::Level ? "level" : "text",
			o.defaultText,o.maxLevel,o.valid});
	return out;
}

std::vector<std::pair<std::string,std::string>> FireSolverEnvironmentOptions()
{
	std::vector<std::pair<std::string,std::string>> out;
	for (const auto& o : Options()) out.emplace_back(o.parameter,o.environment);
	out.emplace_back("mode",fastModeEnvironment);
	return out;
}

bool RegisterFireSolver(std::string& error)
{
	static std::mutex once;
	static bool registered=false;
	std::lock_guard<std::mutex> hold(once);
	if (registered) return true;
	if (!RegisterSolver(Implementation::FireMethaneSolverId,[]{ return std::unique_ptr<ISimulationSolver>(new FireSolver()); },error))
		return false;
	registered=true;
	return true;
}

} }
