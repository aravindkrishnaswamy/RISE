//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvectionUnsupported.cpp - honest non-Metal remap seam
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "FireProductionAdvection.h"
#include "FireProductionTransport.h"

#include <new>

namespace RISE
{
	bool RemapFireProductionMetal( const FireProductionRemapRequest&,
		FireProductionRemapResult& result, std::string* error )
	{
		result=FireProductionRemapResult();
		if( error ) try {
			*error="production fire Metal remap unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) {}
		return false;
	}

	bool RemapFireProductionCellPalindromeMetal(
		const FireProductionCellPalindromeRequest&,
		FireProductionCellPalindromeResult& result, std::string* error )
	{
		result=FireProductionCellPalindromeResult();
		if( error ) try {
			*error="production fire Metal palindrome unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) {}
		return false;
	}

	bool RemapFireProductionCellPalindromeMetalResidentComparator(
		const FireProductionCellPalindromeRequest&,
		FireProductionCellPalindromeResult& result, std::string* error )
	{
		result=FireProductionCellPalindromeResult();
		if( error ) try {
			*error="production resident Metal palindrome unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) {}
		return false;
	}

	bool RemapFireProductionDualMomentumMetalResidentComparator(
		const FireProductionDualMomentumRequest&,
		FireProductionDualMomentumResult& result,
		std::string* structuredError )
	{
		result=FireProductionDualMomentumResult();
		if( structuredError ) try {
			*structuredError="production resident dual Metal unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { structuredError->clear(); }
		return false;
	}

	bool RemapFireProductionPeriodicDualMomentumMetal(
		const FireProductionPeriodicDualMomentumRequest&,
		FireProductionPeriodicDualMomentumResult& result, std::string* error )
	{
		result=FireProductionPeriodicDualMomentumResult();
		if( error ) try {
			*error="production resident periodic dual remap unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) {}
		return false;
	}

	bool EvaluateFireProductionScalarFCTMetalStageDiagnostic(
		const FireProductionScalarFCTRequest&,
		const FireProductionScalarFCTRequest&,
		FireProductionScalarFCTMetalStageDiagnosticResult& result,
		std::string* error )
	{
		result=FireProductionScalarFCTMetalStageDiagnosticResult();
		if( error ) try {
			*error="production scalar FCT Metal-stage diagnostic unavailable: Metal is not built on this platform";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

#if defined(__APPLE__)
	bool EvaluateFireProductionResidentTransportMetalComparator(
		const FireProductionResidentTransportComparatorRequest&,
		FireProductionResidentTransportComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentTransportComparatorResult();
		if( error ) try {
			*error="production resident transport Metal unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool EvaluateFireProductionResidentPhysicalFluxMetalComparator(
		const FireProductionResidentPhysicalFluxComparatorRequest&,
		FireProductionResidentPhysicalFluxComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentPhysicalFluxComparatorResult();
		if( error ) try {
			*error="production resident physical-flux Metal unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool FireProductionResidentEOSCandidateMetalWorkingSetBytes(
		const FireProductionProjectionShape&,std::uint64_t& bytes,bool )
	{
		bytes=0u;return false;
	}

	bool EvaluateFireProductionResidentEOSCandidateMetalComparator(
		const FireProductionResidentEOSCandidateComparatorRequest&,
		FireProductionResidentEOSCandidateComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentEOSCandidateComparatorResult();
		if( error ) try {
			*error="production resident EOS Metal unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool FireProductionResidentTargetLineageMetalWorkingSetBytes(
		const FireProductionProjectionShape&,std::uint64_t& bytes,bool )
	{
		bytes=0u;return false;
	}

	bool FireProductionResidentTargetLineageLiveIncrementWorkingSetBytes(
		const FireProductionProjectionShape&,std::uint64_t& bytes )
	{
		bytes=0u;return false;
	}

	bool EvaluateFireProductionResidentTargetLineageMetalComparator(
		const FireProductionResidentTargetLineageComparatorRequest&,
		FireProductionResidentTargetLineageComparatorResult& result,std::string* error )
	{
		result=FireProductionResidentTargetLineageComparatorResult();
		if( error ) try {
			*error="production resident target-lineage Metal unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool EvaluateFireProductionEOSEnergyEnclosureMetalDiagnostic(
		const std::array<float,9>&,const std::array<float,2>&,bool,
		std::array<std::array<float,4>,8>& output,std::string& shaderSHA256,std::string* error )
	{
		output=std::array<std::array<float,4>,8>();
		shaderSHA256.clear();
		if( error ) try {
			*error="production EOS energy-enclosure Metal diagnostic unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	bool EvaluateFireProductionEOSTemperatureMetalDiagnostic(
		const std::array<float,9>&,unsigned int,float& temperature,bool& accepted,
		std::string& shaderSHA256,std::string* error,float* alternateTemperature,
		std::array<std::array<float,4>,2>* tangentPaths )
	{
		temperature=0.0f;
		accepted=false;
		shaderSHA256.clear();
		if( alternateTemperature ) *alternateTemperature=0.0f;
		if( tangentPaths ) *tangentPaths=std::array<std::array<float,4>,2>();
		if( error ) try {
			*error="production EOS temperature Metal diagnostic unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}

	// FireProductionAdvection.cpp provides this fallback for non-Apple builds;
	// Apple builds without Metal need the same honest refusal here.
	bool FireProductionPayloadDigestMetal(const std::vector<unsigned char>&,
		unsigned int,FireProductionPayloadDigestV2& result,double& deviceMS,
		std::string* error,unsigned int)
	{
		result=FireProductionPayloadDigestV2();
		deviceMS=0.0;
		if( error ) try {
			*error="digest-v2 Metal device unavailable: Metal is not linked";
		} catch( const std::bad_alloc& ) { error->clear(); }
		return false;
	}
#endif

	// The Metal build defines this autorelease-pool RED in
	// FireProductionAdvectionMac.mm; without Metal there is no pool to exercise.
	bool FireProductionProjectedHeunPoolAllocationFailureRED()
	{
		return false;
	}
}
