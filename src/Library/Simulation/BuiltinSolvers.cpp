// Simulation M3c: see BuiltinSolvers.h.
#include "pch.h"
#include "BuiltinSolvers.h"
#include "Cloth/ClothSolver.h"
#include "Fire/FireSolver.h"

namespace RISE { namespace Simulation {

bool RegisterBuiltinSolvers( std::string& error )
{
	// Both register functions are idempotent.  The fire solver's translation
	// unit is in every build list (its Metal backend reports itself
	// unavailable where there is no Metal), so it registers everywhere; its
	// Capabilities say it is not interactive, and a live attach refuses it.
	std::string failures, e;
	if( !RegisterClothSolver( e ) ) failures += "cloth.shell: " + e;
	e.clear();
	if( !RegisterFireSolver( e ) ) failures += ( failures.empty() ? "" : "; " ) + std::string( "fire.methane: " ) + e;
	if( failures.empty() ) return true;
	error = failures;
	return false;
}

} }
