//////////////////////////////////////////////////////////////////////
//
//  FireCPUBackend.h - the CPU backend of the fire.methane production step
//  (simulation M8, part A).
//
//  The production step is the r190 resident projected-Heun owner.  Metal runs
//  it on the GPU (AttemptFireProductionProjectedHeunResidentStepMetal); this
//  backend runs the same discretisation on the CPU through the r190 CPU owner
//  (FireProductionProjectedHeunCPUOwner, with the r264 centred momentum
//  carrier the Metal owner uses by default) and publishes the same accepted-
//  step result, so the driver's outputs, frames and checkpoints are produced
//  by the same code for both backends.  It needs no Metal.
//
//  Selection: RISE_FIRE_BACKEND=cpu (the fire solver's `backend` parameter
//  writes it; unset = Metal).  The worker count (RISE_FIRE_WORKERS, default
//  eight in the production launch) threads the step and never changes a byte.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_CPU_BACKEND_H
#define RISE_SIMULATION_FIRE_CPU_BACKEND_H

#include "../../Utilities/FireProductionForce.h"

#include <cstdint>
#include <string>

namespace RISE
{
	namespace FireRun
	{
		//! True when RISE_FIRE_BACKEND is "cpu" (read on every call, not cached).
		bool FireCPUBackendRequested();
		//! Refuses (returns false, with the reason) the Metal-only opt-in operators the
		//! CPU owner does not implement: compact viscous stencil (and the WALE,
		//! hyper-Vreman and Vreman-zone variants that need it), the scalar Vreman
		//! coefficient override, the forced FCT alpha, and the scalar/wall slope variants.
		bool FireCPUBackendOptionsSupported(std::string& reason);
		//! Host-side peak of one CPU production step (owner certificate plus the
		//! backend's request copies and result staging), in bytes.
		bool FireCPUBackendWorkingSetBytes(const FireProductionProjectionShape& shape,
			std::uint64_t& bytes);
		//! The admission ceiling for one CPU fire process: half of physical memory,
		//! at most 32 GiB (the FireRunHostMemory production ceiling). Zero when the
		//! host memory size is unknown.
		std::uint64_t FireCPUBackendHostCeilingBytes();
		//! Clear refusal when the step cannot fit under the ceiling.
		bool FireCPUBackendAdmits(const FireProductionProjectionShape& shape,std::string& reason);
	}
}

#endif
