//////////////////////////////////////////////////////////////////////
//
//  SimulationDescriptor.h - Simulation M4: what a producer chunk
//    (`cloth_simulation`, `fire_simulation`, `simulation_cache`) hands
//    to IJob::AddSimulation.  Data only: declaring a simulation never runs
//    a solver; `rise-sim bake <scene>` (or a live session) does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMULATION_DESCRIPTOR_
#define SIMULATION_DESCRIPTOR_

#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	struct SimulationDescriptor
	{
		std::string keyword;		//!< the producer chunk keyword
		std::string name;			//!< unique among the scene's simulations
		std::string solverId;		//!< registry id; empty for `simulation_cache` (a foreign producer)
		//! The solver parameter map as canonical CBOR (empty for simulation_cache).
		std::vector<unsigned char> parameters;
		//! The cache directory as authored: absolute, `~/...`, or relative to
		//! the scene file's directory.  Empty: `caches/<name>` beside the scene.
		std::string cache;
		//! Scene elements (object names) the parameters read, in the order the
		//! solver names them.
		std::vector<std::string> sceneInputs;
		//! Outputs the producer declares: (name, codec).  Empty for a
		//! simulation_cache, whose outputs come from its manifest.
		std::vector< std::pair<std::string, std::string> > outputs;
	};
}

#endif
