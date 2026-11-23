//////////////////////////////////////////////////////////////////////
//
//  FireRun.cpp - Fire run launch: the production temporal capstone
//
//  Moved verbatim from tests/FireSequenceTest.cpp (simulation M2, track B).
//  Compiled without fast-math or FP contraction (see the per-file rules in
//  build/make/rise/Makefile).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FireRun.h"
#if defined(RISE_ENABLE_OPENVDB)
#include "../../Utilities/FireSequence.h"
#include "../../Utilities/FireFastMode.h"
#include "../../Utilities/FireProductionForce.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace FireRun
	{

	bool ProductionTemporalCapstoneTierSupported(const double resolutionTier)
	{
		return resolutionTier==6.0||resolutionTier==8.0||resolutionTier==10.0;
	}

	bool BuildProductionTemporalWindowPlan(const SolverFrameValues& geometry,
		const double frameCadenceS,const bool r246TopDouble,ProductionTemporalWindowPlan& plan)
	{
		plan=ProductionTemporalWindowPlan();
		if(!geometry.succeeded||frameCadenceS!=0.0625||
			!(geometry.flowThroughTimeS>0.0)||!std::isfinite(geometry.flowThroughTimeS))return false;
		const double clock=r246TopDouble?geometry.matchedScheduleFlowThroughTimeS:geometry.flowThroughTimeS;
		if(r246TopDouble&&(geometry.dimensions!=std::array<std::size_t,3>{{104u,104u,160u}}||
			clock!=0x1.0d9da03060ef6p+1||geometry.flowThroughTimeS!=2.0*clock||
			geometry.statisticsStartS!=5.0*clock))return false;
		plan.scheduleFlowThroughTimeS=clock;
		plan.statisticsStartS=5.0*clock;
		plan.statisticsDurationS=std::ceil((40.0/(1.5/std::sqrt(CapstonePoolDiameterM)))/
			frameCadenceS)*frameCadenceS;
		plan.targetTimeS=plan.statisticsStartS+plan.statisticsDurationS;
		plan.frameCadenceS=frameCadenceS;
		return !r246TopDouble||(plan.statisticsStartS==10.531864278900095&&
			plan.statisticsDurationS==14.625&&plan.targetTimeS==25.156864278900095);
	}

	void WriteR246WindowIdentity(std::ostream& output,const SolverFrameValues& geometry,
		const ProductionTemporalWindowPlan& plan)
	{
		output<<std::setprecision(17)<<"domain_study r246-lateral-double-top-double-v1\n"
			<<"physical_flow_through_time_s "<<geometry.flowThroughTimeS<<'\n'
			<<"matched_schedule_flow_through_time_s "<<plan.scheduleFlowThroughTimeS<<'\n'
			<<"matched_schedule_flow_through_time_hex "<<std::hexfloat<<plan.scheduleFlowThroughTimeS
			<<std::defaultfloat<<'\n'
			<<"native_centerline_support layers_0_159\ncommon_height_support layers_0_79_separate_observation\n"
			<<"canonical_taller_qualification false\n";
	}

	bool ProductionTemporalEnvironmentAccepted()
	{
		return !std::getenv("RISE_FIRE_TEMPORAL_RESUME")&&
			!std::getenv("RISE_FIRE_TEMPORAL_TARGET_S")&&
			!std::getenv("RISE_FIRE_REFUSAL_CLAUSES")&&OwnerCostPrefixEnvironmentAccepted();
	}
	bool ProductionTemporalHistoriesComplete(const SolverFrameValues& result)
	{
		const std::size_t count=result.acceptedTimeStepHistoryS.size();
		return count>0u&&result.monitoredManifoldMaximumHistory.size()==count&&
			result.monitoredManifoldP95History.size()==count&&
			result.monitoredManifoldP50History.size()==count&&
			result.monitoredTailCellHistory.size()==count&&
			result.monitoredTailDrainedVolumeHistoryM3.size()==count&&
			result.productionDeviceHistoryMS.size()==count&&result.productionWallHistoryMS.size()==count;
	}
	bool ProductionTemporalFramesComplete(const std::vector<double>& times,
		const std::uint64_t streamed,const double start,const double duration,const double cadence)
	{
		if(times.size()<2u||times.size()!=streamed||
			times.size()!=static_cast<std::size_t>(duration/cadence)+1u||
			times.front()<start||times.back()<start+duration)return false;
		for(std::size_t frame=0u;frame<times.size();++frame){
			const double offset=static_cast<double>(frame)*cadence;
			// Same pre-existing snapshot cadence enclosure, now anchored to the
			// statistics boundary rather than an arbitrary surviving first frame.
			const double tolerance=8.0*std::numeric_limits<float>::epsilon()*std::max(cadence,std::fabs(offset));
			if(!std::isfinite(times[frame])||std::fabs(times[frame]-(start+offset))>tolerance)return false;
		}
		return true;
	}

	int SealProductionTemporalStop(const std::filesystem::path& outputDirectory,
		const SolverFrameValues& result,const double targetTimeS)
	{
		std::string publicationError;
		std::ofstream stop(outputDirectory/"window_stop.v1");
		const bool resourceStop=result.structuredError=="solver_failure:host_memory_ceiling_checkpoint_sealed";
		const int exitCode=resourceStop?FireRunHostMemory::GracefulStopExitCode:94;
		stop<<std::setprecision(17)<<"schema rise.fire.full-window-stop.v1\n"
			<<"reason "<<result.structuredError<<"\nexit_code "<<exitCode<<'\n'
			<<"accepted_time_s "<<result.simulatedTimeS<<"\ntarget_time_s "<<targetTimeS<<'\n'
			<<"verdict incomplete\n";
		if(!ClosePublishedStream(stop)||!SealPublishedRunDirectory(outputDirectory,
			result.caseRecordId,publicationError))return 98;
		return exitCode;
	}

	int RunProductionTemporalCapstone(const FireCapstoneOptions& options)
	{
		const double resolutionTier=options.resolutionTier,frameCadenceS=options.frameCadenceS;
		const std::filesystem::path& outputDirectory=options.outputDirectory;
		const bool r244LateralDouble=options.r244LateralDouble,r245AcceptedBudget=options.r245AcceptedBudget,
			r246TopDouble=options.r246TopDouble;
		if(!ProductionTemporalCapstoneTierSupported(resolutionTier)||frameCadenceS!=0.0625||
			(r245AcceptedBudget&&!r244LateralDouble&&resolutionTier!=8.0)||
			(r246TopDouble&&(!r244LateralDouble||!r245AcceptedBudget))||
			(r244LateralDouble&&resolutionTier!=6.0)||
			!std::isfinite(frameCadenceS)||
			static_cast<double>(static_cast<float>(frameCadenceS))!=frameCadenceS)return 91;
		if(!ProductionTemporalEnvironmentAccepted()||std::filesystem::exists(outputDirectory)||
			std::filesystem::is_symlink(outputDirectory))return 91;
		std::error_code directoryError;
		std::filesystem::create_directories(outputDirectory/"checkpoints",directoryError);
		std::filesystem::create_directories(outputDirectory/"frames",directoryError);
		std::filesystem::create_directories(outputDirectory/"budgets",directoryError);
		if(directoryError)return 92;
		FireRunPersistenceOptions geometryOptions;geometryOptions.r244LateralDouble=r244LateralDouble;
		geometryOptions.r246TopDouble=r246TopDouble;
		geometryOptions.productionMetal=r246TopDouble;
		geometryOptions.portedProductionTransport=r246TopDouble;
		const SolverFrameValues beginning=RunFireMethaneDriver(1u,0u,0.0,
			r246TopDouble?25.156864278900095:1.0,r246TopDouble?16.0:1.0,
			resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,geometryOptions);
		ProductionTemporalWindowPlan window;
		if(!BuildProductionTemporalWindowPlan(beginning,frameCadenceS,r246TopDouble,window))return 93;
		const double statisticsStartS=window.statisticsStartS;
		const double statisticsDurationS=window.statisticsDurationS;
		const double targetTimeS=window.targetTimeS;
		// The geometry probe uses the original window identity for r246, and a
		// one-second case for historical paths. The launch case binds publication.
		FireRunPersistenceOptions persistence;
		persistence.productionMetal=true;
		persistence.portedProductionTransport=true;
		persistence.r244LateralDouble=r244LateralDouble;
		persistence.r246TopDouble=r246TopDouble;
		const SolverFrameValues launchCase=RunFireMethaneDriver(1u,0u,0.0,targetTimeS,1.0/frameCadenceS,
			resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,persistence);
		if(!launchCase.succeeded||launchCase.flowThroughTimeS!=beginning.flowThroughTimeS||
			(r246TopDouble&&(launchCase.caseRecordId!=beginning.caseRecordId||
			launchCase.matchedScheduleFlowThroughTimeS!=window.scheduleFlowThroughTimeS)))return 93;
		persistence.r246FullWindow=r246TopDouble;
		persistence.r245AcceptedBudget=r245AcceptedBudget;
		persistence.checkpointPath=outputDirectory/"production_run.checkpoint";
		persistence.finalCheckpointPath=outputDirectory/"production_final.checkpoint";
		persistence.retainedCheckpointDirectory=outputDirectory/"checkpoint_history";
		persistence.checkpointCadenceWallS=300.0;
		persistence.productionOnsetDiagnosticDirectory=outputDirectory/"budgets";
		persistence.temporalSnapshotDirectory=outputDirectory/"checkpoints";
		persistence.temporalSnapshotCadenceS=frameCadenceS;
		persistence.temporalFrameObserver=MakePartialTemporalManifestObserver(outputDirectory,
			persistence.temporalSnapshotDirectory,resolutionTier,frameCadenceS,targetTimeS,
			r244LateralDouble,r246TopDouble);
		// M2: bounded runs, observers and requests for identity drivers and the generic host.
		persistence.windowS=options.windowS;
		persistence.hasTemporalFrameOrigin=options.hasTemporalFrameOrigin;
		persistence.temporalFrameOriginS=options.temporalFrameOriginS;
		persistence.retainedSnapshotEvery=options.retainedSnapshotEvery;
		persistence.retainedSnapshotDirectory=options.retainedSnapshotDirectory;
		persistence.acceptedStepObserver=options.acceptedStepObserver;
		persistence.stopRequested=options.stopRequested;
		persistence.checkpointRequested=options.checkpointRequested;
		if(options.frameObserver){
			const auto partial=persistence.temporalFrameObserver;const auto user=options.frameObserver;
			persistence.temporalFrameObserver=[partial,user](const SolverFrameValues& values,const std::uint64_t frame){
				const bool partialOK=!partial||partial(values,frame);return user(values,frame)&&partialOK;};
		}
		// r175 certifies the nonzero Binary32 source producer and burning-tail
		// continuation at this represented step.  A source-bearing campaign may
		// select a smaller CFL step, but it must not extrapolate that certificate
		// to a larger source integration interval.
		persistence.maximumProductionSourceStepS=
			static_cast<double>(static_cast<float>(0.0016462659696117043));
		// The capstone's CPU-side source/target reductions retain their sealed
		// eight-worker owner; Metal still owns the resident full-grid work.
		const unsigned int workers=FireRunWorkerCount(8u);
		RISECBOR64::Bytes buildRecord;
		std::string buildId,executableDigest,publicationError;
		if(!CurrentRendererBuildIdentity(buildRecord,buildId)||
			!CurrentExecutableDigest(buildRecord,executableDigest,publicationError))return 92;
		std::ofstream receipt(outputDirectory/"window_launch.v1");
		if(r246TopDouble)WriteR246WindowIdentity(receipt,launchCase,window);
		else if(r244LateralDouble)receipt<<"domain_study r244-lateral-double-v1\n";
		if(r245AcceptedBudget)receipt<<"accepted_stage_budget r245-v1\n";
		// RISE_FIRE_FAST_MODE=1: uncertified arithmetic and no per-step seals; the trajectory is not reproducible.
		if(FireSim::FireFastMode())receipt<<"fast_mode r275-uncertified-v1\n";
		// r271 A/B: the opt-in compact viscous stencil does not change the case, and both
		// settings share one kernel source text, so the receipt records the selection.
		// v2 adds the compact-diagonal nu_t for heat, species and the combustion mixing
		// time (v1 was momentum only).
		if(FireProductionForceMetalCompactViscousStencil())receipt<<"viscous_stencil r271-compact-mac-v2\n";
		// r271 A/B: the opt-in FDS mixing time likewise leaves the case unchanged.
		if(FireFDSMixingTimeSelected())receipt<<"mixing_time r271-fds-v1\n";
		if(FireCase::InertPlumeSelected())receipt<<"inert_plume r271-v1\n";
		// r275 opt-ins: worker count leaves the state unchanged; a source-step cap other than
		// r175's is uncertified and changes the trajectory, so both are recorded.
		if(workers!=8u)receipt<<"cpu_workers "<<workers<<"\n";
		if(FireProductionSourceStepCapS(persistence.maximumProductionSourceStepS)!=
			persistence.maximumProductionSourceStepS)receipt<<std::setprecision(17)<<
			"uncertified_source_step_cap_s "<<
			FireProductionSourceStepCapS(persistence.maximumProductionSourceStepS)<<"\n";
		if(FireCase::InertPlumeSeedAmplitude()!=0.0)receipt<<"inert_seed "<<std::setprecision(17)<<FireCase::InertPlumeSeedAmplitude()<<"\n";
		receipt<<std::setprecision(17)<<"schema rise.fire.full-window-launch.v1\n"
			<<"producer_build_id "<<buildId<<"\nproducer_executable_sha256 "<<executableDigest<<'\n'
			<<"case_record_id "<<launchCase.caseRecordId<<"\nresolution_tier "<<resolutionTier<<'\n'
			<<"from_zero true\nstatistics_start_s "<<statisticsStartS<<'\n'
			<<"statistics_duration_s "<<statisticsDurationS<<"\ntarget_time_s "<<targetTimeS<<'\n'
			<<"frame_cadence_s "<<frameCadenceS<<"\ncheckpoint_cadence_wall_s "<<persistence.checkpointCadenceWallS<<'\n'
			<<"authored_case_frames_per_s "<<1.0/frameCadenceS<<'\n'
			<<"crossing_stop_enabled false\nempirical_claim_tier 10\n"
			<<"exclusive_host_rule no_builds_no_qualification_no_oracle_no_render\n";
		if(!ClosePublishedStream(receipt)||!PublishPayloadMerkleSidecar(
			outputDirectory/"window_launch.v1",launchCase.caseRecordId,publicationError))return 92;
		const auto wallStart=std::chrono::steady_clock::now();
		const SolverFrameValues result=RunFireMethaneDriver(workers,1u,targetTimeS,
			targetTimeS,1.0/frameCadenceS,resolutionTier,CapstonePoolDiameterM,
			CapstoneHeatReleaseRateKW,persistence);
		const double wallS=std::chrono::duration<double>(
			std::chrono::steady_clock::now()-wallStart).count();
		if(!result.succeeded){std::fprintf(stderr,"production temporal capstone failed: %s\n",
			result.structuredError.c_str());
			return SealProductionTemporalStop(outputDirectory,result,targetTimeS);}
		// The standing binary32 event selector rounds outward. Record that actual
		// endpoint; never relabel it as an exact-time composition or shorten it.
		if(!std::isfinite(result.simulatedTimeS)||(!options.sealPartialWindow&&result.simulatedTimeS<targetTimeS)||
			result.caseRecordId!=launchCase.caseRecordId||!ProductionTemporalHistoriesComplete(result))return 98;
		std::vector<std::filesystem::path> temporalFrames;
		for(const std::filesystem::directory_entry& entry:
			std::filesystem::directory_iterator(outputDirectory/"checkpoints"))
			if(entry.is_regular_file()&&entry.path().extension()==".vdb")
				temporalFrames.push_back(entry.path());
		std::sort(temporalFrames.begin(),temporalFrames.end());
		std::vector<std::string> frameDigests;
		std::vector<double> frameTimesS;
		std::string error;
		for(std::size_t frame=0u;frame<temporalFrames.size();++frame){
			std::ostringstream name;name<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame<<".vdb";
			const std::filesystem::path framePath=outputDirectory/"frames"/name.str();
			if(temporalFrames[frame].filename()!=name.str())return 95;
			std::filesystem::copy_file(temporalFrames[frame],framePath,
				std::filesystem::copy_options::overwrite_existing,directoryError);
			if(directoryError)return 96;
			frameDigests.push_back(DigestFile(temporalFrames[frame]));
			if(frameDigests.back().empty())return 96;
			std::filesystem::path timePath=temporalFrames[frame];timePath.replace_extension(".time");
			std::ifstream timeRecord(timePath);double timeS=0.0;std::string recordedDigest;
			if(!(timeRecord>>timeS>>recordedDigest)||recordedDigest!=frameDigests.back()||
				!std::isfinite(timeS))return 95;
			frameTimesS.push_back(timeS);
		}
		if(!options.sealPartialWindow&&!ProductionTemporalFramesComplete(frameTimesS,result.streamedFrameCount,
			statisticsStartS,statisticsDurationS,frameCadenceS))return 95;
		std::ofstream frameTimes(outputDirectory/"frame_times.csv");
		frameTimes<<"frame,simulation_time_s,vdb_sha256\n";
		std::vector<std::pair<std::string,std::string> > manifestFrames;
		for(std::size_t frame=0u;frame<frameDigests.size();++frame){
			std::ostringstream name;name<<"frames/frame_"<<std::setw(5)<<
				std::setfill('0')<<frame<<".vdb";
			frameTimes<<frame<<','<<std::setprecision(17)<<frameTimesS[frame]<<','<<
				frameDigests[frame]<<'\n';
			manifestFrames.push_back({name.str(),frameDigests[frame]});
		}
		if(!ClosePublishedStream(frameTimes))return 98;
		std::string manifestDigest;
		if(manifestFrames.size()>=2u){
			const std::array<std::uint64_t,3> dimensions={{result.dimensions[0],
				result.dimensions[1],result.dimensions[2]}};
			const RISECBOR64::Bytes manifest=ManifestBytes("","","hold",true,true,
				dimensions,result.cellWidthM,frameTimesS.front(),frameCadenceS,1.0,0.0,
				true,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,0.0,
				resolutionTier,true,manifestFrames,targetTimeS,1.0/frameCadenceS,true,r244LateralDouble,r246TopDouble);
			const std::filesystem::path manifestPath=
				outputDirectory/"sequence_manifest.rise-fire.cbor";
			std::ofstream manifestOutput(manifestPath,std::ios::binary|std::ios::trunc);
			manifestOutput.write(reinterpret_cast<const char*>(manifest.data()),
				static_cast<std::streamsize>(manifest.size()));
			manifestOutput.close();
			if(!manifestOutput||DigestFile(manifestPath).empty())return 98;
			FireSequenceManifest verified;
			if(!verified.LoadCanonicalEnvelope(manifest,outputDirectory.string(),error)||
				verified.CaseRecordId()!=result.caseRecordId||
				!verified.PreflightAllFrames(error)||
				(r246TopDouble&&(!options.r246ManifestDomainCheck||!options.r246ManifestDomainCheck(manifest,error)))){
				std::fprintf(stderr,"production temporal capstone manifest refused: case=%s run=%s %s\n",
					verified.CaseRecordId().c_str(),result.caseRecordId.c_str(),error.c_str());
				return 98;
			}
			manifestDigest=DigestFile(manifestPath);
		}
		std::ofstream diagnostics(outputDirectory/"monitored_diagnostics.csv");
		diagnostics<<"step,dt_s,deviation_max,deviation_p95,deviation_p50,tail_cells,"
			"tail_drained_m3,device_ms,wall_ms\n";
		const std::size_t samples=result.acceptedTimeStepHistoryS.size();
		for(std::size_t sample=0u;sample<samples;++sample)diagnostics<<sample+1u<<','<<
			std::setprecision(17)<<result.acceptedTimeStepHistoryS[sample]<<','<<
			result.monitoredManifoldMaximumHistory[sample]<<','<<
			result.monitoredManifoldP95History[sample]<<','<<
			result.monitoredManifoldP50History[sample]<<','<<
			result.monitoredTailCellHistory[sample]<<','<<
			result.monitoredTailDrainedVolumeHistoryM3[sample]<<','<<
			result.productionDeviceHistoryMS[sample]<<','<<result.productionWallHistoryMS[sample]<<'\n';
		if(!ClosePublishedStream(diagnostics))return 98;
		std::ofstream summary(outputDirectory/"production_capstone_summary.txt");
		summary<<std::setprecision(17)<<"artifact_fidelity=simulation_evidence\n"
			<<"operator_mode=section_3_7_projected_heun_resident_owner\n"
			<<"formal_contract_verdict=pending_equal_time_composition\n"
			<<"resolution_tier="<<resolutionTier<<"\n"
			<<"maximum_source_step_s="<<persistence.maximumProductionSourceStepS<<"\n"
			<<"target_time_s="<<targetTimeS<<"\n"
			<<"accepted_time_s="<<result.simulatedTimeS<<"\n"
			<<"statistics_start_s="<<result.statisticsStartS<<"\n"
			<<"accepted_steps="<<result.acceptedTimeStepHistoryS.size()<<"\n"
			<<"snapshot_cadence_s="<<frameCadenceS<<"\n"
			<<"snapshot_count="<<frameDigests.size()<<"\n"
			<<"frame_times_sha256="<<DigestFile(outputDirectory/"frame_times.csv")<<"\n"
			<<"sequence_manifest_sha256="<<manifestDigest<<"\n"
			<<"wall_s="<<wallS<<"\n"
			<<"puffing_hz="<<result.puffingFrequencyHz<<"\n"
			<<"puffing_relative_error="<<result.puffingRelativeError<<"\n"
			<<"integrated_hrr_J="<<result.integratedHeatReleaseJ<<"\n"
			<<"integrated_fuel_kg="<<result.integratedFuelConsumptionKG<<"\n"
			<<"external_fuel_kg="<<result.externalFuelMassKG<<"\n"
			<<"integrated_radiative_fraction="<<result.integratedRadiativeFraction<<"\n"
			<<"hard_bound_retries="<<result.productionHardBoundRetryCount<<"\n"
			<<"physical_projection_retries="<<result.productionPhysicalProjectionRetryCount<<"\n"
			<<"diagnostics_sha256="<<DigestFile(outputDirectory/"monitored_diagnostics.csv")<<"\n";
		for(std::size_t frame=0u;frame<frameDigests.size();++frame)
			summary<<"frame_"<<std::setw(5)<<std::setfill('0')<<frame<<"_sha256="<<
				frameDigests[frame]<<"\n";
		summary.close();
		// The incremental manifest is a working file; the sealed final manifest above replaces it.
		std::filesystem::remove(outputDirectory/PartialTemporalManifestName,directoryError);
		std::filesystem::remove(outputDirectory/(std::string(PartialTemporalManifestName)+".tmp"),directoryError);
		if(!summary||!SealPublishedRunDirectory(outputDirectory,result.caseRecordId,error))return 98;
		std::fprintf(stderr,"PRODUCTION_TEMPORAL_CAPSTONE tier=%.0f time=%.17g steps=%zu "
			"frames=%zu wall_s=%.17g summary=%s\n",resolutionTier,result.simulatedTimeS,
			result.acceptedTimeStepHistoryS.size(),frameDigests.size(),wallS,
			(outputDirectory/"production_capstone_summary.txt").string().c_str());
		return frameDigests.empty()?97:0;
	}

	// r270: rebuilds a finished capstone's sequence manifest from its retained
	// frames, exactly as the capstone publishes it, and verifies the case
	// against both the run's sealed launch receipt and the launch case.  It
	// carries this binary's producer identity, not the run's.  Only the tier-8
	// standard domain is supported; the output file is separate so a run
	// directory is never overwritten in place.
	int RebuildProductionTemporalManifest(const double resolutionTier,const double frameCadenceS,
		const std::filesystem::path& runDirectory,const std::filesystem::path& outputPath)
	{
		if(resolutionTier!=8.0||frameCadenceS!=0.0625||std::filesystem::exists(outputPath)||
			std::filesystem::is_symlink(runDirectory))return 91;
		std::string receiptCaseId;
		{
			std::ifstream receipt(runDirectory/"window_launch.v1");std::string key,value;
			bool receiptCompact=false,receiptFDSMixing=false,receiptInert=false;double receiptSeed=0.0;
			while(receipt>>key>>value){
				if(key=="domain_study")return 91;
				if(key=="case_record_id")receiptCaseId=value;
				// A superseded stencil (v1, momentum only) cannot be rebuilt by this build.
				if(key=="viscous_stencil"){if(value!="r271-compact-mac-v2")return 91;receiptCompact=true;}
				if(key=="mixing_time"){if(value!="r271-fds-v1")return 91;receiptFDSMixing=true;}
				if(key=="inert_plume"){if(value!="r271-v1")return 91;receiptInert=true;}
				if(key=="inert_seed")receiptSeed=std::stod(value);
			}
			// A rebuild never mixes viscous stencils with the run it describes.
			if(receiptCompact!=FireProductionForceMetalCompactViscousStencil()||
				receiptFDSMixing!=FireFDSMixingTimeSelected()||receiptInert!=FireCase::InertPlumeSelected()||
				receiptSeed!=FireCase::InertPlumeSeedAmplitude())return 91;
		}
		if(receiptCaseId.size()!=64u)return 95;
		const SolverFrameValues beginning=RunFireMethaneDriver(1u,0u,0.0,1.0,1.0,
			resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,FireRunPersistenceOptions());
		ProductionTemporalWindowPlan window;
		if(!BuildProductionTemporalWindowPlan(beginning,frameCadenceS,false,window))return 93;
		FireRunPersistenceOptions persistence;
		persistence.productionMetal=true;persistence.portedProductionTransport=true;
		const SolverFrameValues launchCase=RunFireMethaneDriver(1u,0u,0.0,window.targetTimeS,
			1.0/frameCadenceS,resolutionTier,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,persistence);
		if(!launchCase.succeeded||launchCase.flowThroughTimeS!=beginning.flowThroughTimeS)return 93;
		std::ifstream csv(runDirectory/"frame_times.csv");std::string line;
		if(!std::getline(csv,line)||line!="frame,simulation_time_s,vdb_sha256")return 95;
		std::vector<double> frameTimesS;
		std::vector<std::pair<std::string,std::string> > manifestFrames;
		while(std::getline(csv,line)){
			std::istringstream row(line);std::string index,time,digest;
			if(!std::getline(row,index,',')||!std::getline(row,time,',')||!std::getline(row,digest))return 95;
			std::ostringstream name;name<<"frames/frame_"<<std::setw(5)<<std::setfill('0')<<
				manifestFrames.size()<<".vdb";
			char* indexEnd=nullptr;char* timeEnd=nullptr;
			const unsigned long long frame=std::strtoull(index.c_str(),&indexEnd,10);
			const double timeS=std::strtod(time.c_str(),&timeEnd);
			if(index.empty()||*indexEnd!='\0'||time.empty()||*timeEnd!='\0'||!std::isfinite(timeS)||
				frame!=manifestFrames.size()||DigestFile(runDirectory/name.str())!=digest)return 95;
			frameTimesS.push_back(timeS);manifestFrames.push_back({name.str(),digest});
		}
		if(manifestFrames.size()<2u)return 95;
		const std::array<std::uint64_t,3> dimensions={{launchCase.dimensions[0],
			launchCase.dimensions[1],launchCase.dimensions[2]}};
		const RISECBOR64::Bytes manifest=ManifestBytes("","","hold",true,true,
			dimensions,launchCase.cellWidthM,frameTimesS.front(),frameCadenceS,1.0,0.0,
			true,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,0.0,
			resolutionTier,true,manifestFrames,window.targetTimeS,1.0/frameCadenceS,true,false,false);
		FireSequenceManifest verified;std::string error;
		if(!verified.LoadCanonicalEnvelope(manifest,runDirectory.string(),error)||
			!verified.PreflightAllFrames(error)){
			std::fprintf(stderr,"manifest rebuild refused: %s\n",error.c_str());return 98;
		}
		std::fprintf(stderr,"MANIFEST_REBUILD case=%s launch_case=%s receipt_case=%s frames=%zu\n",
			verified.CaseRecordId().c_str(),launchCase.caseRecordId.c_str(),receiptCaseId.c_str(),
			manifestFrames.size());
		if(verified.CaseRecordId()!=launchCase.caseRecordId||verified.CaseRecordId()!=receiptCaseId)return 98;
		std::ofstream output(outputPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(manifest.data()),
			static_cast<std::streamsize>(manifest.size()));
		output.close();
		return output?0:98;
	}

	bool OwnerCostPrefixEnvironmentAccepted()
	{
		// Same mutation surface refused by the sealed replay; only the timing
		// observer is permitted here, and only with its exact enabling value.
		for(const char* name:{"RISE_FIRE_ONSET_RESUME_CHECKPOINT","RISE_FIRE_SINGLE_STAGE_FCT_ONSET",
			"RISE_FIRE_FORCE_TEST_FAILURE","RISE_FIRE_PRODUCTION_RESTORATION_CYCLE_PROBE",
			"RISE_FIRE_PRODUCTION_RESTORATION_TEST","RISE_FIRE_PRODUCTION_STEP_FAILURE",
			"RISE_FIRE_PROFILE","RISE_FIRE_PROJECTION_TEST_FAILURE",
			"RISE_FIRE_RESTORATION_PLATEAU_PROBE","RISE_FIRE_TIMESTEP_VELOCITY_AUDIT",
			"RISE_FIRE_TIMESTEP_VELOCITY_PACK_MODE","RISE_FIRE_MOMENTUM_AUDIT_PATH",
			"RISE_FIRE_TARGET_TEMPERATURE_IDENTITY_RED","RISE_FIRE_TARGET_MOLECULAR_IDENTITY_RED",
			"RISE_FIRE_REQUEST_LAYOUT_IDENTITY_RED","RISE_FIRE_MANIFOLD_TIMESTEP_PROBE",
			"RISE_FIRE_MANIFOLD_STAGE_BUDGET_PROBE","RISE_FIRE_GOLDEN_LONG_SHADOW",
			"RISE_FIRE_ADVECTIVE_ANOMALY_CLOSURE_TEST","RISE_FIRE_MANIFOLD_TAIL_THRESHOLD_RED",
			"RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PROBE","RISE_FIRE_ADVECTIVE_ANOMALY_CONVERGENCE_PASSES",
			"RISE_FIRE_HOST_RESIDUAL_PROBE","RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE",
			"RISE_FIRE_ONSET_TARGET_S","RISE_FIRE_EOS_REFUSAL_INPUTS"})if(std::getenv(name))return false;
		for(const char* name:{"RISE_FIRE_OWNER_PROFILE","RISE_FIRE_PRODUCER_KERNEL_PROFILE"}){
			const char* profile=std::getenv(name);if(profile&&std::strcmp(profile,"1")!=0)return false;
		}
		return true;
	}
	}
}
#endif
