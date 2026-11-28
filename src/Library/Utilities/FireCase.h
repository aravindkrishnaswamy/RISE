//////////////////////////////////////////////////////////////////////
//
//  FireCase.h - Canonical fire-case-v1 record and r54 derivations
//
//  Defines authored and derived methane fire-case schemas (Heskestad
//  flame height, D* grid sizing, source/pilot geometry, timestep
//  policies), canonical CBOR64 envelope construction/validation,
//  r271 inert-plume diagnostics, and explicit CFL timestep selection.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_CASE_H
#define RISE_FIRE_CASE_H

#include "RISECBOR64.h"
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	class FireSimulationMethaneRecord;

	namespace FireCase
	{
		//! Process-cached override from RISE_FIRE_SCALAR_VREMAN_CV (>= 0).
		//! Negative means the transport record supplies the scalar coefficient.
		float ProductionScalarVremanOverride();

		//! Piecewise-linear HRR/flux envelope knot: timeS (s), normalized value in [0, 1].
		struct EnvelopeKnot { double timeS=0.0, value=0.0; };
		//! User-authored methane fire-case parameters (pool or patch source in m,
		//! hrr in kW or mass_flux in kg/(m^2*s), quality draft/standard/high/dstar).
		struct AuthoredV1
		{
			std::string fuelRecordId;
			std::string sourceKind="pool";
			double poolDiameterM=0.0,patchXM=0.0,patchZM=0.0;
			std::string intensityKind="hrr";
			double heatReleaseRateKW=0.0,fuelMassFluxKGPerM2S=0.0;
			std::vector<EnvelopeKnot> envelope;
			double durationS=0.0;
			std::string quality;
			double numericDStarTier=0.0;
			std::uint64_t seed=0;
			double outputFramesPerS=0.0;
			bool plumeLaw=false;
			bool hasRadiativeFractionOverride=false;
			double radiativeFractionOverride=0.0;
			// Empty retains canonical r54 bytes. The named r244 tier-6 study
			// doubles lateral extents with an exactly translated source/pilot.
			// r246 additionally doubles height under an explicit matched schedule.
			std::string domainStudy;
			// r270: absent retains canonical bytes and the gas-only adiabatic bed.
			// Present makes the bed outside the burner a gray zero-net-flux
			// (FDS ADIABATIC) surface that absorbs optically thin gas emission.
			bool hasFloorEmissivity=false;
			double floorEmissivity=0.0;
		};

		//! Deterministically derived r54 physical, grid (nx,ny,nz, cellWidthM in m),
		//! Heskestad/D*, flow-through, pilot, and floor-radiation closure parameters.
		struct DerivedV1
		{
			double sourceAreaM2=0.0, nominalFuelFluxKGPerM2S=0.0;
			double peakEnvelope=0.0, referenceHeatReleaseRateW=0.0;
			double ambientDensityKGPerM3=0.0, ambientCpJPerKGK=0.0;
			double effectiveRadiativeFraction=0.0;
			double flameHeightM=0.0, effectiveFlameHeightM=0.0;
			double characteristicDiameterM=0.0, cellWidthM=0.0,resolutionTier=0.0;
			double maximumAcceptedTemperatureK=0.0;
			std::uint64_t nx=0,ny=0,nz=0;
			double extentXM=0.0,extentYM=0.0,extentZM=0.0;
			// Physical geometry residence time; an experimental clock is separate.
			double flowThroughTimeS=0.0,preRollOrDiscardS=0.0;
			double matchedScheduleFlowThroughTimeS=0.0;
			std::string windowKind;
			std::string perturbationDigest;
			std::string pilotModelVersion;
			std::string pilotMaskRule;
			std::string limiterAcceptanceModelVersion;
			std::string reactionEnergyHeadroomModelVersion;
			double pilotSetpointTemperatureK=0.0,pilotExpansionVolumeRatioCap=0.0,
				pilotDurationMultiplier=0.0,pilotAmbientTemperatureK=0.0,
				pilotRampExponentPerFlowThrough=0.0;
			// Zero with an empty model version unless the case authors floor_emissivity.
			double floorEmissivity=0.0;
			std::string floorRadiationModelVersion;
		};

		//! Complete canonical fire-case-v1 record with serialized CBOR64 payload/envelope
		//! bytes, SHA-256 caseRecordId, and sorted unique referencedRecordIds.
		struct RecordV1
		{
			AuthoredV1 authored;
			DerivedV1 derived;
			RISECBOR64::Bytes payloadBytes;
			RISECBOR64::Bytes envelopeBytes;
			std::string caseRecordId;
			std::vector<std::string> referencedRecordIds;
		};

		//! Deterministic 64-bit mixing hash used to seed source-pattern perturbations.
		std::uint64_t SplitMix64(std::uint64_t value);
		//! Builds the 2D (nx*ny) burner multiplicative flux-factor mask normalized so
		//! sum(factor) * dx^2 == sourceAreaM2, with deterministic +-1% seeded variation.
		bool BuildSourcePattern(
			const AuthoredV1&,const DerivedV1&,std::vector<double>&,
			std::string& error );
		//! Builds the 3D (nx*ny*nz) pilot mask: active (1) on z=0 cells whose centers lie
		//! in the burner-rim annulus [D/2, D/2 + 2*dx] (translated for r244/r246 studies).
		bool BuildPilotMask(
			const AuthoredV1&,const DerivedV1&,std::vector<std::uint8_t>&,
			std::string& error );
		//! Evaluates the pilot target temperature (K) over [beginningTimeS, endTimeS] (s)
		//! via exponential ramp from ambient to pilotSetpointTemperatureK; outputs 0 when
		//! inactive or when InertPlumeSelected() is true.
		bool EvaluatePilotSetpointTemperatureK(
			const DerivedV1&,bool maskCell,double beginningTimeS,double endTimeS,
			double& setpointTemperatureK,
			std::string& error );
		//! r271 diagnostic (opt-in RISE_FIRE_INERT_PLUME=1): a non-reacting buoyant plume on the production case.
		//! Reaction, pilot and gas radiation are off; a frozen volumetric heat source of InertPlumeHeatW is applied
		//! uniformly over the burner-masked cells of layers 0..InertPlumeLayers-1, split by InertPlumeFraction (FDS
		//! r263's time-mean heat release per layer, docs/data/fire_r271/inert_source_profile.json).
		bool InertPlumeSelected();
		//! The pre-pilot burner-rim ignition witness (docs/SIMULATION_SOLVER_CONTRACT.md sections 12.2 and 13):
		//! before the pilot is established a rim cell is reaction-eligible only once its product mass
		//! fraction (CO2+H2O)/rho reaches this value (or exceeds the ambient fraction, whichever is
		//! larger), so binary32 rounding residue (about 2e-11) cannot light the burner.  Used by the
		//! source build and the driver; solver version 3.
		constexpr double RimIgnitionProductWitness=1.0e-6;
		const int InertPlumeLayers=27;
		const double InertPlumeHeatW=26400.0;
		double InertPlumeFraction(std::size_t layer);
		//! The same vertical profile on a grid of another cell width: the tier-8 table is piecewise constant in height
		//! (layer width tier8CellWidthM) and is integrated over cell layer `layer` of width cellWidthM. Equal widths
		//! return the table value itself.
		double InertPlumeLayerFraction(std::size_t layer,double cellWidthM,double tier8CellWidthM);
		//! Optional seed (RISE_FIRE_INERT_SEED=eps, 0 when unset): each burner cell's dose is scaled by 1+eps*xi, xi uniform
		//! in [-1,1] from a hash of the cell and a 10 ms time bucket, renormalised per layer so the total stays exact.
		double InertPlumeSeedAmplitude();
		const double InertPlumeSeedBucketS=0.01;
		//! Returns matchedScheduleFlowThroughTimeS when set (r246 top study), else flowThroughTimeS.
		double ScheduleFlowThroughTimeS(const DerivedV1&);
		//! Maximum step (s) keeping one pilot ramp step within pilotExpansionVolumeRatioCap (17/16).
		double PilotCommandMaximumStepS(const DerivedV1&);
		//! Validates authored inputs against `fuel`, computes all DerivedV1 quantities,
		//! and encodes canonical CBOR64 payload/envelope bytes and SHA-256 caseRecordId.
		bool BuildMethaneV1(
			const AuthoredV1&,const FireSimulationMethaneRecord&,
			const std::vector<std::string>& referencedRecordIds,
			RecordV1&,std::string& error );
		//! Structural check only: verifies canonical {case_record_id, payload} v1 envelope and SHA-256.
		bool ValidateEnvelopeV1(
			const RISECBOR64::Bytes&,RecordV1&,std::string& error );
		//! Full semantic check: parses authored fields, rebuilds via BuildMethaneV1,
		//! and requires bit-identical reproduction of the input envelopeBytes.
		bool ValidateMethaneEnvelopeV1(
			const RISECBOR64::Bytes&,const FireSimulationMethaneRecord&,
			RecordV1&,std::string& error );
		//! Selects the next explicit timestep (s) as the minimum of advective CFL
		//! (0.5 or RISE_FIRE_ADVECTIVE_CFL), buoyant 0.5*sqrt(2*dx/g'), diffusive
		//! dx^2/(8*D), and 1.1*previousStepS; returns 0 on invalid or unconstrained inputs.
		double SelectTimeStepS(
			double cellWidthM,double maximumAxisSpeedMPerS,
			double maximumPositiveReducedGravityMPerS2,
			double maximumActiveDiffusivityM2PerS,double previousStepS );
	}
}

#endif
