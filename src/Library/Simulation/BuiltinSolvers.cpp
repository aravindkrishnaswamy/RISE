// Simulation M3c: see BuiltinSolvers.h.
#include "pch.h"
#include "BuiltinSolvers.h"
#include "Cloth/ClothSolver.h"
#include "Cloth/ClothParameterSchema.h"
#include "SimulationParameterSchema.h"
#include "Fire/FireSolver.h"
#include "Fire/FireParameterSchema.h"
#include "Rigid/RigidSolver.h"
#include "Rigid/RigidParameterSchema.h"
#include "SimulationPortAdapters.h"
#include "Coupling/ClothRigidCoupling.h"
#include "../Utilities/FireSequence.h"

namespace RISE { namespace Simulation {

bool RegisterBuiltinSolvers( std::string& error )
{
	// Both register functions are idempotent.  The fire solver's translation
	// unit is in every build list (its Metal backend reports itself
	// unavailable where there is no Metal), so it registers everywhere; its
	// Capabilities say it is not interactive, and a live attach refuses it.
	std::string failures, e;
	if( !RegisterClothSolver( e ) ) failures += "cloth.shell: " + e;
	// M4: each solver's parameter schema beside its factory (not part of the contract).
	e.clear();
	if( !RegisterSolverSchema( ClothSolverId, &ClothParameterSchema, e ) ) failures += ( failures.empty() ? "" : "; " ) + e;
	e.clear();
	if( !RegisterFireSolver( e ) ) failures += ( failures.empty() ? "" : "; " ) + std::string( "fire.methane: " ) + e;
	e.clear();
	if( !RegisterSolverSchema( Implementation::FireMethaneSolverId, &FireParameterSchema, e ) )
		failures += ( failures.empty() ? "" : "; " ) + e;
	// M6: the rigid-body solver and the port adapters.
	e.clear();
	if( !RegisterRigidSolver( e ) ) failures += ( failures.empty() ? "" : "; " ) + std::string( "rigid.bodies: " ) + e;
	e.clear();
	if( !RegisterSolverSchema( RigidSolverId, &RigidParameterSchema, e ) ) failures += ( failures.empty() ? "" : "; " ) + e;
	e.clear();
	if( !RegisterBuiltinPortAdapters( e ) ) failures += ( failures.empty() ? "" : "; " ) + std::string( "port adapters: " ) + e;
	e.clear();
	if( !RegisterClothRigidCouplingEvidence( e ) ) failures += ( failures.empty() ? "" : "; " ) + std::string( "coupling evidence: " ) + e;
	if( failures.empty() ) return true;
	error = failures;
	return false;
}

} }
