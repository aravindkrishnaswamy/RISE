//////////////////////////////////////////////////////////////////////
//
//  FireProductionForceUnsupported.cpp - honest non-Metal force seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionForce.h"

#include <new>

namespace RISE
{
	bool BuildFireProductionFrozenForceFieldsMetal(
		const FireProductionFrozenForceRequest&,
		FireProductionFrozenForceResult& result,
		double& deviceElapsedMS,
		std::string* error )
	{
		result=FireProductionFrozenForceResult();deviceElapsedMS=0.0;
		if( error ) try {
			*error="production frozen-force Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool EvaluateFireProductionNonpressureMomentumRHSMetal(
		const FireProductionNonpressureMomentumRHSRequest&,
		FireProductionNonpressureMomentumRHSResult& result,
		FireProductionNonpressureMomentumRHSMetalDiagnostics& diagnostics,
		std::string* error )
	{
		result=FireProductionNonpressureMomentumRHSResult();
		diagnostics=FireProductionNonpressureMomentumRHSMetalDiagnostics();
		if( error ) try {
			*error="production nonpressure momentum Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AdvanceFireProductionFrozenForceMetal(
		const FireProductionFrozenForceRequest&,
		bool,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error )
	{
		result=FireProductionFrozenForceAdvanceResult();
		diagnostics=FireProductionResidentForceDiagnostics();
		if( error ) try {
			*error="production resident frozen-force Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AdvanceFireProductionFrozenForceMetalResidentStateComparator(
		const FireProductionFrozenForceRequest&,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error )
	{
		result=FireProductionFrozenForceAdvanceResult();
		diagnostics=FireProductionResidentForceDiagnostics();
		if( error ) try {
			*error="production resident force-state Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AdvanceFireProductionForceProjectionMetal(
		const FireProductionFrozenForceRequest&,
		const std::vector<float>&,
		FireProductionResidentForceProjectionResult& result,
		std::string* error )
	{
		result=FireProductionResidentForceProjectionResult();
		if( error ) try {
			*error="production resident force-projection Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AdvanceFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest&,
		FireProductionResidentStepResult& result,
		std::string* error )
	{
		result=FireProductionResidentStepResult();
		if( error ) try {
			*error="production resident step Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AttemptFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest&,
		FireProductionResidentStepResult& result,
		std::string* error )
	{
		result=FireProductionResidentStepResult();
		if( error ) try {
			*error="production resident step Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AttemptFireProductionCompatibleMomentumDiagnosticMetal(
		const FireProductionResidentStepRequest&,
		FireProductionResidentStepResult& result,
		std::string* error )
	{
		result=FireProductionResidentStepResult();
		if( error ) try {
			*error="production compatible-momentum diagnostic Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool SealFireProductionSingleStageFCTBoundaryState(
		const FireProductionProjectionShape&,
		const std::array<FireProductionProjectionBoundary,6>&,
		FireProductionSingleStageFCTBoundaryState& state,
		std::string* error )
	{
		state.identity=0u;
		if( error ) try {
			*error="production single-stage FCT boundary seal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool AttemptFireProductionSingleStageFCTDiagnosticMetal(
		const FireProductionResidentStepRequest&,
		const FireProductionSingleStageFCTBoundaryState&,
		FireProductionSingleStageFCTDiagnosticResult& result,
		std::string* error )
	{
		result=FireProductionSingleStageFCTDiagnosticResult();
		if( error ) try {
			*error="production single-stage FCT diagnostic Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	std::uint64_t FireProductionResidentStepMetalCommandCommitCount()
	{
		return 0u;
	}
}
