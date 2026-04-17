//////////////////////////////////////////////////////////////////////
//
//  CPUTopology.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CPUTopology.h"
#include "CPU.h"
#include "../Interfaces/IOptions.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#ifdef __linux__
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <string>
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#endif

#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		namespace
		{
			void DetectTopology( CPUTopology& topo )
			{
				// Default: everything homogeneous, all cores are P.
				int logical = 0, physical = 0;
				GetCPUCount( logical, physical );
				const unsigned int total = static_cast<unsigned int>(
					logical > 0 && physical > 0 ? logical * physical : 1 );
				topo.numTotal = total;
				topo.numPerformance = total;
				topo.numEfficiency = 0;
				topo.performanceCoreIds.clear();

#if defined( __APPLE__ )
				// Apple Silicon exposes perflevel counts via sysctl.
				// perflevel0 = performance, perflevel1 = efficiency.
				// On Intel Macs these sysctls don't exist; fall back
				// to homogeneous.
				int p = 0, e = 0;
				size_t sz = sizeof( p );
				if( sysctlbyname( "hw.perflevel0.logicalcpu", &p, &sz, 0, 0 ) == 0 && p > 0 ) {
					sz = sizeof( e );
					if( sysctlbyname( "hw.perflevel1.logicalcpu", &e, &sz, 0, 0 ) != 0 ) {
						e = 0;
					}
					topo.numPerformance = static_cast<unsigned int>( p );
					topo.numEfficiency  = static_cast<unsigned int>( e );
					topo.numTotal       = topo.numPerformance + topo.numEfficiency;
					// macOS doesn't support CPU affinity pinning on
					// Apple Silicon; QoS class is the only mechanism.
					// Leave performanceCoreIds empty.
				}
#elif defined( __linux__ )
				// Linux exposes per-CPU capacity in sysfs.  On big.LITTLE
				// / Intel Alder Lake+ the high-performance cores have
				// the highest cpu_capacity value; E-cores have a lower
				// value.  Homogeneous systems have identical values.
				std::vector<std::pair<unsigned int, int>> caps;  // (cpu_id, capacity)
				caps.reserve( 64 );
				for( unsigned int cpu = 0; cpu < 1024; cpu++ ) {
					char path[256];
					std::snprintf( path, sizeof( path ),
						"/sys/devices/system/cpu/cpu%u/cpu_capacity", cpu );
					std::ifstream fs( path );
					if( !fs.is_open() ) {
						break;
					}
					int cap = 0;
					fs >> cap;
					if( !fs.fail() ) {
						caps.emplace_back( cpu, cap );
					}
				}
				if( !caps.empty() ) {
					int maxCap = 0;
					for( const auto& c : caps ) {
						if( c.second > maxCap ) maxCap = c.second;
					}
					unsigned int pCount = 0;
					unsigned int eCount = 0;
					for( const auto& c : caps ) {
						if( c.second == maxCap ) {
							topo.performanceCoreIds.push_back( c.first );
							pCount++;
						} else {
							eCount++;
						}
					}
					topo.numPerformance = pCount;
					topo.numEfficiency = eCount;
					topo.numTotal = pCount + eCount;
					// Homogeneous system → every CPU has maxCap, so
					// all CPUs are P.  Clear performanceCoreIds so
					// callers don't pin unnecessarily.
					if( eCount == 0 ) {
						topo.performanceCoreIds.clear();
					}
				}
#elif defined( WIN32 )
				// GetLogicalProcessorInformationEx gives per-core info
				// including EfficiencyClass.  On Alder Lake+ P-cores
				// have EfficiencyClass 1+ (higher), E-cores 0.
				DWORD len = 0;
				GetLogicalProcessorInformationEx( RelationProcessorCore, 0, &len );
				if( len > 0 ) {
					std::vector<BYTE> buf( len );
					if( GetLogicalProcessorInformationEx(
							RelationProcessorCore,
							reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>( buf.data() ),
							&len ) )
					{
						BYTE* p = buf.data();
						BYTE* end = p + len;
						std::vector<std::pair<unsigned int, BYTE>> coreEff;
						while( p < end ) {
							SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* info =
								reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>( p );
							if( info->Relationship == RelationProcessorCore &&
								info->Processor.GroupCount > 0 )
							{
								// Iterate the logical CPUs in this core's mask.
								const KAFFINITY mask = info->Processor.GroupMask[0].Mask;
								const BYTE eff = info->Processor.EfficiencyClass;
								for( unsigned int i = 0; i < 64; i++ ) {
									if( mask & ( static_cast<KAFFINITY>( 1 ) << i ) ) {
										coreEff.emplace_back( i, eff );
									}
								}
							}
							p += info->Size;
						}
						if( !coreEff.empty() ) {
							BYTE maxEff = 0;
							for( const auto& c : coreEff ) {
								if( c.second > maxEff ) maxEff = c.second;
							}
							unsigned int pCount = 0;
							unsigned int eCount = 0;
							for( const auto& c : coreEff ) {
								if( c.second == maxEff ) {
									topo.performanceCoreIds.push_back( c.first );
									pCount++;
								} else {
									eCount++;
								}
							}
							topo.numPerformance = pCount;
							topo.numEfficiency  = eCount;
							topo.numTotal       = pCount + eCount;
							if( eCount == 0 ) {
								topo.performanceCoreIds.clear();
							}
						}
					}
				}
#endif

				if( topo.numPerformance == 0 ) {
					// Absolute fallback — something went wrong with
					// detection, at least give the pool 1 worker.
					topo.numPerformance = 1;
					topo.numTotal       = topo.numTotal > 0 ? topo.numTotal : 1;
				}
			}
		}

		const CPUTopology& GetCPUTopology()
		{
			// C++11 guarantees thread-safe initialisation of
			// function-local statics.  The lambda runs exactly once
			// even under concurrent first access — no separate
			// `initialised` flag or std::call_once needed.
			static const CPUTopology cached = []() {
				CPUTopology topo;
				DetectTopology( topo );
				return topo;
			}();
			return cached;
		}

		namespace
		{
			unsigned int ResolveReserveE( int reserveE )
			{
				if( reserveE >= 0 ) {
					return static_cast<unsigned int>( reserveE );
				}
				// Read from global options; default 1 so the system
				// always has at least one core free for UI / daemons.
				const int opt = GlobalOptions().ReadInt(
					"render_thread_reserve_count", 1 );
				return opt < 0 ? 0 : static_cast<unsigned int>( opt );
			}
		}

		unsigned int ComputeRenderPoolSize( int reserveE )
		{
			// Legacy explicit overrides take priority over topology.
			// These exist for users who want hard control (CI caps,
			// deterministic test runs, debugging).  Both options are
			// honoured here so the pool and HowManyThreadsToSpawn()
			// agree on the final count.
			const int force = GlobalOptions().ReadInt(
				"force_number_of_threads", 0 );
			if( force > 0 ) {
				return static_cast<unsigned int>( force );
			}
			const int maxThreadsOpt = GlobalOptions().ReadInt(
				"maximum_thread_count", 0x7FFFFFFF );

			const CPUTopology& t = GetCPUTopology();
			const unsigned int reserve = ResolveReserveE( reserveE );

			unsigned int workers;
			if( reserve == 0 ) {
				// Benchmark mode — every core gets a worker.
				workers = t.numTotal > 0 ? t.numTotal : 1;
			}
			else if( t.numEfficiency > 0 ) {
				// Heterogeneous: always use every P-core, reserve
				// from the E-pool.  If `reserve` > numEfficiency,
				// clamp so we don't shrink below the P-core count.
				const unsigned int reservedE =
					reserve >= t.numEfficiency ? t.numEfficiency : reserve;
				workers = t.numPerformance + ( t.numEfficiency - reservedE );
			}
			else if( t.numTotal > reserve ) {
				// Homogeneous: reserve from total.
				workers = t.numTotal - reserve;
			}
			else {
				workers = 1;
			}

			if( maxThreadsOpt > 0 &&
			    workers > static_cast<unsigned int>( maxThreadsOpt ) ) {
				workers = static_cast<unsigned int>( maxThreadsOpt );
			}
			if( workers < 1 ) workers = 1;
			return workers;
		}

		std::vector<unsigned int> GetRenderAffinityMask( int reserveE )
		{
			std::vector<unsigned int> mask;

#if defined( __APPLE__ )
			// Apple Silicon doesn't support thread-affinity pinning;
			// QoS class is the only lever.  Leave mask empty.
			(void)reserveE;
			return mask;
#else
			const CPUTopology& t = GetCPUTopology();
			if( t.numEfficiency == 0 || t.performanceCoreIds.empty() ) {
				// Homogeneous or detection failed — no pinning.
				return mask;
			}

			const unsigned int reserve = ResolveReserveE( reserveE );
			if( reserve == 0 ) {
				// Benchmark: allow every CPU.  Leave mask empty so
				// caller doesn't pin.
				return mask;
			}

			// Build the render CPU set: every P-core + every E-core
			// whose ID is not among the last `reserve` E-cores.
			// First, collect E-core IDs.
			std::vector<unsigned int> eIds;
			eIds.reserve( t.numEfficiency );
			for( unsigned int cpu = 0; cpu < t.numTotal; cpu++ ) {
				bool isP = false;
				for( unsigned int pid : t.performanceCoreIds ) {
					if( pid == cpu ) { isP = true; break; }
				}
				if( !isP ) {
					eIds.push_back( cpu );
				}
			}

			// Add all P-cores.
			mask = t.performanceCoreIds;

			// Add all E-cores except the last `reserve` ones.
			const unsigned int keepE =
				eIds.size() > reserve ? static_cast<unsigned int>( eIds.size() ) - reserve : 0;
			for( unsigned int i = 0; i < keepE; i++ ) {
				mask.push_back( eIds[i] );
			}
			return mask;
#endif
		}
	}
}
