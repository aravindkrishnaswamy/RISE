//////////////////////////////////////////////////////////////////////
//
//  FireRun.h - Fire run launch (fire-private API)
//
//  The production temporal capstone: authored window and 0.0625 s frame
//  cadence, case and build identity, launch receipt, the driver run, and
//  the sealed legacy manifest, frame times, diagnostics and summary.  Moved
//  verbatim from tests/FireSequenceTest.cpp (RunProductionTemporalCapstoneChild,
//  simulation M2 track B); FireSequenceTest --fire-production-temporal-capstone
//  still calls it.
//
//  Pieces (all in RISE::FireRun):
//    FireRunCheckpoint.h  run state, checkpoint codec, durable publication
//    FireRunFrames.h      case records, legacy VDB frames, legacy manifest
//    FireRunDriver.h      the accepted-step driver (RunFireMethaneDriver) and
//                         its options, observers, cooperative stop
//    FireRun.h            the capstone launch (this file)
//
//  Environment options are still read where they always were (track B);
//  track C maps them to parameters at the fire factory boundary.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_RUN_H
#define RISE_SIMULATION_FIRE_RUN_H

#include "FireRunDriver.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#if defined(RISE_ENABLE_OPENVDB)
namespace RISE
{
	namespace FireRun
	{
	struct ProductionTemporalWindowPlan
	{
		double scheduleFlowThroughTimeS=0.0,statisticsStartS=0.0;
		double statisticsDurationS=0.0,targetTimeS=0.0,frameCadenceS=0.0;
	};

	//! One capstone launch.  The first block is the historical command line
	//! (FireSequenceTest --fire-production-temporal-capstone <tier> 0.0625
	//! <dir> and its r244/r245/r246 study variants); the rest is new in M2
	//! and neutral at its defaults.
	struct FireCapstoneOptions
	{
		double resolutionTier=8.0;
		double frameCadenceS=0.0625;
		std::filesystem::path outputDirectory;
		bool r244LateralDouble=false,r245AcceptedBudget=false,r246TopDouble=false;
		//! r246 only: the sealed manifest's domain check (the test supplies
		//! its TemporalManifestDomainMatches).  Without it an r246 launch
		//! refuses its manifest.
		std::function<bool(const RISECBOR64::Bytes& manifest,std::string& error)> r246ManifestDomainCheck;

		// M2 additions.
		//! Bounded window, frame clock origin and retained snapshots for the
		//! main run (see FireRunPersistenceOptions).
		double windowS=0.0;
		bool hasTemporalFrameOrigin=false;
		double temporalFrameOriginS=0.0;
		std::uint64_t retainedSnapshotEvery=0u;
		std::filesystem::path retainedSnapshotDirectory;
		//! Seal a run that ended before the authored target (window or
		//! cooperative stop): skip the full-window and frame-coverage checks
		//! and seal what was written.  Off for production launches.
		bool sealPartialWindow=false;
		//! Observers and requests forwarded to the driver (see
		//! FireRunPersistenceOptions).  frameObserver runs after the capstone's
		//! own partial-manifest observer.
		std::function<bool(const SolverFrameValues&,std::uint64_t)> frameObserver;
		std::function<void(const SolverFrameValues&,std::uint64_t)> acceptedStepObserver;
		const std::atomic<bool>* stopRequested=nullptr;
		std::atomic<bool>* checkpointRequested=nullptr;
	};

	//! Exit codes as the historical test child: 0 sealed, 90-98 refusals and
	//! publication failures, 97 graceful host-memory stop (window_stop.v1).
	int RunProductionTemporalCapstone(const FireCapstoneOptions& options);

	//! The capstone in its phases, shared with the generic solver adapter
	//! (FireSolver, M2 track C).  RunProductionTemporalCapstone is: options and
	//! environment checks, the run directory, Plan, Receipt, the driver run
	//! with launch.persistence, Finish.
	struct ProductionTemporalLaunch
	{
		ProductionTemporalWindowPlan window;
		SolverFrameValues launchCase;           // the launch case (geometry and identity, no steps)
		FireRunPersistenceOptions persistence;  // the capstone's driver options for options.outputDirectory
		unsigned int workers=8u;
	};
	bool ProductionTemporalOptionsAccepted(const FireCapstoneOptions& options);
	//! Options, environment, case and window plan; touches no file.  0, 91 or 93.
	int PlanProductionTemporalLaunch(const FireCapstoneOptions& options,ProductionTemporalLaunch& launch);
	//! window_launch.v1 and its seal in options.outputDirectory.  0 or 92.
	int WriteProductionTemporalLaunchReceipt(const FireCapstoneOptions& options,
		const ProductionTemporalLaunch& launch);
	//! After the driver returned: frame copies, frame_times.csv, the sealed
	//! manifest, diagnostics, summary and the run-directory seal (exit codes as
	//! RunProductionTemporalCapstone).
	int FinishProductionTemporalRun(const FireCapstoneOptions& options,
		const ProductionTemporalLaunch& launch,const SolverFrameValues& result,const double wallS);

	bool ProductionTemporalCapstoneTierSupported(const double resolutionTier);
	bool BuildProductionTemporalWindowPlan(const SolverFrameValues& geometry,
		const double frameCadenceS,const bool r246TopDouble,ProductionTemporalWindowPlan& plan);
	void WriteR246WindowIdentity(std::ostream& output,const SolverFrameValues& geometry,
		const ProductionTemporalWindowPlan& plan);
	bool ProductionTemporalEnvironmentAccepted();
	bool ProductionTemporalHistoriesComplete(const SolverFrameValues& result);
	bool ProductionTemporalFramesComplete(const std::vector<double>& times,
		const std::uint64_t streamed,const double start,const double duration,const double cadence);
	int SealProductionTemporalStop(const std::filesystem::path& outputDirectory,
		const SolverFrameValues& result,const double targetTimeS);
	int RebuildProductionTemporalManifest(const double resolutionTier,const double frameCadenceS,
		const std::filesystem::path& runDirectory,const std::filesystem::path& outputPath);
	bool OwnerCostPrefixEnvironmentAccepted();
	}
}
#endif

#endif
