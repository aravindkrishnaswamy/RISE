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
}
