//////////////////////////////////////////////////////////////////////
//
//  FireRunFrames.h - Fire run case records, legacy frames and manifest
//
//  The production capstone's case identity records (burner policy, floor
//  emissivity option, aerosol and chemistry records), the legacy OpenVDB
//  frame writer (content-derived file UUID, durable sync before
//  publication) and the legacy sequence manifest bytes, including the
//  incremental (partial) manifest observer.  Moved verbatim from
//  tests/FireSequenceTest.cpp (simulation M2, track B); output bytes are
//  unchanged.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_RUN_FRAMES_H
#define RISE_SIMULATION_FIRE_RUN_FRAMES_H

#include "FireRunCheckpoint.h"
#include "../../Utilities/FireCase.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/openvdb.h>
#endif

namespace RISE
{
	namespace FireRun
	{
	constexpr double CapstonePoolDiameterM=0.30;
	constexpr double CapstoneHeatReleaseRateKW=33.0;
	// McCaffrey, NBSIR 79-1910, Table 1, adopted 33 kW plume row.
	constexpr double McCaffrey33KWVelocityCoefficient=1.13;
	constexpr double McCaffrey33KWTemperatureCoefficientK=23.3;

	// r275 follow mode: an incremental manifest of the frames written so far, so a partial sequence is
	// loadable (and renderable) while the run is still going. It is a separate file
	// (sequence_manifest.partial.rise-fire.cbor, written through a temp name and renamed, so a reader
	// never sees a half-written file) and the sealed final manifest is produced exactly as before.
	// Frame paths are relative to the run directory and point at the checkpoints/ copies, which are
	// the only place a frame exists before the run ends. The capstone removes the partial file just
	// before the final seal, so a completed run directory is unchanged. RISE_FIRE_PARTIAL_MANIFEST=0
	// disables it. Needs at least two frames (the manifest format's minimum).
	constexpr const char* PartialTemporalManifestName="sequence_manifest.partial.rise-fire.cbor";

#if defined(RISE_ENABLE_OPENVDB)
	//! The grids of one legacy frame, after the solver fields are filled and
	//! before the file is written.
	struct FireLegacyFrameGrids
	{
		openvdb::FloatGrid::Ptr carbon,temperature,reaction;
		openvdb::Vec3fGrid::Ptr velocity;
		openvdb::FloatGrid::Ptr chemCH,chemC2,chemCO2;
	};
	//! Test-only frame mutation (invalid grids, synthetic unit chemistry).
	//! Production passes none.
	class FireLegacyFrameTestMutation
	{
	public:
		virtual ~FireLegacyFrameTestMutation()=default;
		//! Synthetic unit preview chemistry: refused without chemistry or on
		//! the capstone output path; preview weight one everywhere.
		virtual bool SyntheticColdChemistry() const=0;
		virtual void Mutate(const FireLegacyFrameGrids& grids) const=0;
	};
	//! Legacy OpenVDB frame: carbon (g/m3), temperature, reaction, staggered
	//! velocity and, with includeChem, the preview chemistry channels.
	bool WriteLegacyFireFrame(const std::filesystem::path& path,const float carbonValue,
		const bool includeChem,const SolverFrameValues solver,const float chemScale,
		const FireLegacyFrameTestMutation* mutation=nullptr);
	//! Production temporal frame: the legacy writer with preview chemistry at
	//! 0.8, published with its payload-v2 sidecar (immutable).
	bool WriteProductionTemporalFrame(const std::filesystem::path& path,
		const SolverFrameValues& values);
#endif

	const RISECBOR64::Bytes& ProductionBurnerPolicyBytes();
	const std::string& ProductionBurnerPolicyId();
	bool ApplyFloorEmissivityOption(FireCase::AuthoredV1& authored);
	RISECBOR64::Bytes AerosolRecord();
	RISECBOR64::Bytes SyntheticChemRecord();
	RISECBOR64::Bytes ManifestBytes( const std::string& firstDigest,
		const std::string& secondDigest, const char* endPolicy="hold",
		const bool useProductionOptics=false, const bool syntheticChem=false,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5,
		const double simulationTimeOrigin=1.0, const double frameStepSeconds=0.25,
		const double sceneToSimulationScale=2.0, const double sceneTimeOrigin=10.0,
		const bool caseGridBound=false, const double casePoolDiameterM=0.02,
		const double caseHeatReleaseRateKW=0.10,
		const bool caseHasRadiativeFractionOverride=false,
		const double caseRadiativeFractionOverride=0.0,
		const double caseResolutionTier=6.0,const bool casePlumeLaw=false,
		const std::vector<std::pair<std::string,std::string> >& sequenceFrames={},
		const double authoredCaseDurationS=0.0,const double authoredCaseFramesPerS=0.0,
		const bool prescribedProductionBurner=false,const bool r244LateralDouble=false,
		const bool r246TopDouble=false );
	bool PartialTemporalManifestEnabled();
	std::function<bool(const SolverFrameValues&,std::uint64_t)> MakePartialTemporalManifestObserver(
		const std::filesystem::path& runDirectory,const std::filesystem::path& frameDirectory,
		const double resolutionTier,const double frameCadenceS,const double targetTimeS,
		const bool r244LateralDouble,const bool r246TopDouble);
	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error);
	}
}

#endif
