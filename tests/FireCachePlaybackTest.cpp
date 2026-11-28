// Differential check for captured legacy frames and their native conversion.
#include "../src/Library/Utilities/FireSequence.h"
#include "../src/Library/Utilities/FireSequenceCache.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace RISE;
using namespace RISE::Implementation;
namespace fs=std::filesystem;

bool CheckFrame(const FireSequenceManifest& original,const FireSequenceManifest& native,
	std::int64_t index,std::size_t& compared,std::string& error)
{
	FireSequencePreparedFrame a,b;
	if(!original.LoadFrame(index,a,error) || !native.LoadFrame(index,b,error)) return false;
	if(!b.available || a.channels.size()!=b.channels.size() || a.frameIndex!=b.frameIndex ||
		a.wholeFileSha256!=b.wholeFileSha256){error="frame identity mismatch";return false;}
	for(const auto& item:a.channels){
		const auto found=b.channels.find(item.first);
		if(found==b.channels.end()){error="missing channel "+item.first;return false;}
		const auto& x=item.second; const auto& y=found->second;
		if(x.name!=y.name || x.vectorValues!=y.vectorValues || x.dimensions!=y.dimensions ||
			x.minimum!=y.minimum || x.maximum!=y.maximum || x.values.size()!=y.values.size() ||
			std::memcmp(x.values.data(),y.values.data(),x.values.size()*sizeof(double))!=0){
			error="channel bit mismatch at "+std::to_string(index)+"/"+item.first;
			return false;
		}
		compared+=x.values.size();
	}
	return true;
}

bool CheckPrepareRollback(const FireSequenceManifest& manifest,std::int64_t first,std::int64_t next,
	std::string& error)
{
	FireSequencePreparationController controller(manifest);
	std::int64_t installed=-1;
	if(!controller.SetReversibleFrameInstaller([&](const FireSequencePreparedFrame& candidate,
		std::function<void()>* rollback) {
		if(rollback) *rollback=[&,old=installed] { installed=old; };
		installed=candidate.frameIndex;
		return true;
	},std::string(64,'b'),error) || !controller.SetActiveBindingIdentity(std::string(64,'c'),error)) return false;
	const auto& map=manifest.TimeMap();
	auto support=[&](std::int64_t ordinal) {
		const double t=map.sceneTimeOrigin+(double(ordinal-map.firstFrameIndex)+0.25)*map.frameStepSeconds/map.sceneToSimulationScale;
		return FireSequenceRenderTimeSupport{t,t,t};
	};
	if(!controller.PrepareMediaForRender(support(first),false,error) || installed!=first) return false;
	const auto previous=controller.PreparedFrame();
	const auto identity=controller.PreparedInputId();
	const auto generation=controller.Generation();
	std::function<void()> rollback;
	if(!controller.PrepareMediaForRender(support(next),false,error,&rollback) || !rollback || installed!=next) return false;
	rollback();
	if(controller.PreparedFrame()!=previous || controller.PreparedInputId()!=identity || installed!=first ||
		controller.Generation()!=generation || controller.MajorantGeneration()!=generation ||
		controller.EmissionCDFGeneration()!=generation) {
		error="rollback changed prepared identity, installed frame or generation";
		return false;
	}
	return true;
}

bool CheckHistorical(const char* source,const fs::path& output,int count,char** selected,std::string& error)
{
	if(fs::exists(output)){error="comparison needs a fresh cache directory";return false;}
	std::ifstream file(source,std::ios::binary);
	if(!file){error="cannot read source envelope";return false;}
	RISECBOR64::Bytes envelope((std::istreambuf_iterator<char>(file)),{});
	FireSequenceManifest original;
	if(!original.LoadCanonicalEnvelope(envelope,fs::path(source).parent_path().string(),error)) return false;
	std::set<std::int64_t> indices;
	for(int i=0;i<count;++i) indices.insert(std::stoll(selected[i]));
	if(indices.empty()) for(const auto& f:original.Frames()) indices.insert(f.index);
	std::size_t compared=0;
	for(auto index:indices){
		if(!ImportFireSequenceCache(source,output.string(),error,index)) return false;
		FireSequenceManifest native;
		if(!native.LoadPlaybackFile((output/"manifest.cbor").string(),error) ||
			!CheckFrame(original,native,index,compared,error)) return false;
		if(native.SequenceId()!=original.SequenceId() || native.CaseRecordId()!=original.CaseRecordId() ||
			native.OpticalRecord()!=original.OpticalRecord()){error="physical record identity mismatch";return false;}
	}
	if(indices.size()>1){
		FireSequenceManifest native;
		if(!native.LoadPlaybackFile((output/"manifest.cbor").string(),error) ||
			!CheckPrepareRollback(native,*indices.begin(),*indices.rbegin(),error)) return false;
	}
	std::cout<<"Verified "<<indices.size()<<" frames, "<<compared<<" scalar components bit-for-bit\n";
	return true;
}

bool CheckMissingRecords(std::string& error)
{
	const auto directory=fs::temp_directory_path()/("rise_fire_cache_admission_"+
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	if(!fs::create_directory(directory)){error="could not allocate test directory";return false;}
	struct Cleanup {
		fs::path directory;
		~Cleanup(){std::error_code ignored;fs::remove_all(directory,ignored);}
	} cleanup{directory};
	Simulation::CacheDescription description;
	description.key=std::string(64,'1'); description.epoch=std::string(64,'2');
	description.producer="rise.fire.vdb-import"; description.producerVersion="1";
	description.outputs={{"volume","grid.v1",{{"carbon","g/m3","float32"}}}};
	description.fidelity="predictive"; description.evidence={std::string(64,'a')};
	Simulation::CacheWriter writer;
	if(!writer.Begin(directory,description,"",false,4*1024*1024,error) || !writer.Seal(error))
		return false;
	FireSequenceManifest fire;
	if(fire.LoadPlaybackFile((directory/"manifest.cbor").string(),error) || fire.IsValid()){
		error="fire playback accepted a predictive label without physical records";return false;
	}
	std::cout<<"Rejected a sealed predictive-labelled cache with missing source records\n";
	return true;
}

// Solver version 4 (docs/FIRE_SMOKE_STATUS.md, "Fire solver version 4"; version 3: the contract's
// Appendix C, docs/SIMULATION_SOLVER_CONTRACT.md): a cache written by fire.methane versions 1, 2
// and 3 passes playback's version gate (it is stale for baking only, through the key); any other
// version is refused there.  The fixture has no frame template, so an admitted version stops at
// the physical-record check, after the gate.
bool CheckSolverCacheVersions(std::string& error)
{
	const std::string versions[]={"1","2","3","4",Implementation::FireMethaneSolverVersion,"0","5"};
	for(const std::string& v:versions){
		const bool playable=v=="1"||v=="2"||v=="3"||v=="4";
		if(Implementation::FireMethaneSolverCacheVersionPlayable(v)!=playable){
			error="fire solver cache version "+v+" has the wrong playability";return false;
		}
		const auto directory=fs::temp_directory_path()/("rise_fire_solver_cache_version_"+v+"_"+
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		if(!fs::create_directory(directory)){error="could not allocate test directory";return false;}
		struct Cleanup {
			fs::path directory;
			~Cleanup(){std::error_code ignored;fs::remove_all(directory,ignored);}
		} cleanup{directory};
		Simulation::CacheDescription description;
		description.key=std::string(64,'1'); description.epoch=std::string(64,'2');
		description.producer=Implementation::FireMethaneSolverId; description.producerVersion=v;
		description.outputs={{"volume","grid.v1",{{"carbon","g/m3","float32"}}}};
		description.fidelity="preview";
		Simulation::CacheWriter writer;
		if(!writer.Begin(directory,description,"",false,4*1024*1024,error) || !writer.Seal(error))
			return false;
		FireSequenceManifest fire;
		std::string why;
		if(fire.LoadPlaybackFile((directory/"manifest.cbor").string(),why) || fire.IsValid()){
			error="fire playback accepted a solver cache without a frame template";return false;
		}
		const bool gated=why.find("unsupported fire solver cache version")!=std::string::npos;
		if(gated==playable){
			error="fire solver cache version "+v+(playable?" was refused by the version gate: ":
				" passed the version gate: ")+why;
			return false;
		}
	}
	std::cout<<"Fire solver caches of versions 1, 2, 3 and 4 pass playback's version gate; 0 and 5 are refused\n";
	return true;
}
}

int main(int argc,char** argv)
{
	std::string error;
	try{
		if(argc==1 && CheckMissingRecords(error) && CheckSolverCacheVersions(error)) return 0;
		if(argc>=3 && CheckHistorical(argv[1],argv[2],argc-3,argv+3,error)) return 0;
	} catch(const std::exception& e){error=e.what();}
	std::cerr<<"FAIL: "<<(error.empty()?"expected [source-manifest new-cache-directory [ordinal ...]]":error)<<'\n';
	return 1;
}
