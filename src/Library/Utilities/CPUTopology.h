//////////////////////////////////////////////////////////////////////
//
//  CPUTopology.h - Heterogeneous-CPU topology detection.
//
//    On Apple Silicon, ARM big.LITTLE, and Intel Alder Lake+, cores
//    come in two flavours:
//      - Performance (P) cores: higher clock, wider OoO, more cache.
//      - Efficiency (E) cores: lower clock, simpler pipeline, less cache.
//    P-core throughput per unit CPU-time is typically 2–3× E-core.
//
//    Running a renderer across a mix of P and E cores is usually a
//    loss: `ParallelFor` waits for the slowest worker, so E-core
//    workers become the tail that drags the whole render.  Better
//    to pin render threads to P-cores and leave E-cores to the OS.
//
//    This utility exposes the minimum information needed:
//      - Number of P-cores (NumPerformanceCores()).
//      - Number of E-cores (NumEfficiencyCores()).
//      - The CPU-ID list of the P-cores for platforms that support
//        affinity pinning (Linux, Windows).
//
//    On platforms without heterogeneous cores (older Intel, older
//    AMD, older Macs), NumPerformanceCores() returns the full CPU
//    count and NumEfficiencyCores() returns 0.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 17, 2026
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CPU_TOPOLOGY_
#define RISE_CPU_TOPOLOGY_

#include <vector>

namespace RISE
{
	namespace Implementation
	{
		struct CPUTopology
		{
			//! Number of high-performance cores in the system.  On
			//! Apple Silicon this is `hw.perflevel0.logicalcpu`; on
			//! Linux big.LITTLE it's the count of CPUs with the
			//! maximum `cpu_capacity` value; on Windows Alder Lake
			//! it's the count of cores with the highest EfficiencyClass.
			//! Homogeneous systems return the full CPU count.
			unsigned int numPerformance;

			//! Number of efficiency / low-power cores.  Zero on
			//! homogeneous systems.
			unsigned int numEfficiency;

			//! Total logical CPU count (P + E + any uncategorised).
			unsigned int numTotal;

			//! CPU-ID list of the P-cores (for sched_setaffinity
			//! on Linux, SetThreadSelectedCpuSetMasks on Windows).
			//! Empty on macOS — macOS doesn't support thread-affinity
			//! pinning; QoS class is used instead.  Empty on
			//! homogeneous systems (no pinning needed).
			std::vector<unsigned int> performanceCoreIds;
		};

		//! Query the current machine's topology once and cache it.
		const CPUTopology& GetCPUTopology();

		//! Decide how many render-pool workers to launch.
		//!
		//! Policy:
		//!   - Heterogeneous (P + E cores visible): every P-core gets
		//!     a worker, plus (numEfficiency - reserveE) E-cores each
		//!     get a worker.  1 E-core is always left to the system
		//!     by default so UI / daemons / system calls remain
		//!     responsive during long renders.
		//!   - Homogeneous (no E-cores detected): reserve `reserveE`
		//!     cores from the total.
		//!
		//! `reserveE` defaults to the `render_thread_reserve_count`
		//! option, default 1.  Benchmark harnesses override to 0 for
		//! clean measurement.  Set to a negative number to ignore the
		//! option and force 0.
		unsigned int ComputeRenderPoolSize( int reserveE = -1 );

		//! Returns the affinity mask (list of CPU IDs) that render
		//! workers should be pinned to on Linux / Windows.  Empty on
		//! macOS (no thread-affinity API on Apple Silicon) and on
		//! homogeneous systems where pinning adds no value.
		std::vector<unsigned int> GetRenderAffinityMask( int reserveE = -1 );
	}
}

#endif
