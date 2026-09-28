//////////////////////////////////////////////////////////////////////
//
//  ScatteredRayContainer.cpp - Implements the scattered ray container
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <atomic>
#include "../Interfaces/ISPF.h"
#include "../Interfaces/ILog.h"

using namespace RISE;

ScatteredRayContainer::ScatteredRayContainer() :
  freeidx( 0 )
{
}

ScatteredRayContainer::~ScatteredRayContainer()
{
}

bool ScatteredRayContainer::AddScatteredRay( ScatteredRay& ray )
{
	if( freeidx >= kCapacity ) {
		// Container full -- the ray is DROPPED.  Returning false rather than
		// storing is the whole contract: `delete_stack` below is left true on
		// this path, so the caller's ScatteredRay destructor still frees its
		// IOR stack and nothing leaks.  Callers that can overflow must check.
		return false;
	}

	memcpy( (void*)&rays[freeidx], (void*)&ray, sizeof( ScatteredRay ) );
	freeidx++;

	ray.delete_stack = false;
	return true;
}

//! From the rays stored, randomly returns one given a value
ScatteredRay* ScatteredRayContainer::RandomlySelect(double random, bool bNM) const
{
	return RandomlySelect(random,bNM,nullptr);
}

ScatteredRay* ScatteredRayContainer::RandomlySelect(double random, bool bNM, Scalar* selectedProbability) const
{
	if(selectedProbability) *selectedProbability=0;

	if( freeidx == 0 ) {
		return 0;
	}

	if( freeidx == 1 ) {
		if(selectedProbability) *selectedProbability=1;
		return &rays[0];
	}

	if( freeidx == 2 ) {
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				if(selectedProbability) *selectedProbability=eventA/total;
				return &rays[0];
			} else {
				if(selectedProbability) *selectedProbability=eventB/total;
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
		cdf[i] = total + prob;
		total += prob;
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( random < (cdf[i]/total) ) {
				if(selectedProbability) *selectedProbability=(bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray))/total;
				return &rays[i];
			}
		}
	}

	return 0;
}

//! From the rays stored, randomly returns a non diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectNonDiffuse(double random, bool bNM) const
{
	return RandomlySelectNonDiffuse(random,bNM,nullptr);
}

ScatteredRay* ScatteredRayContainer::RandomlySelectNonDiffuse(double random, bool bNM, Scalar* selectedProbability) const
{
	if(selectedProbability) *selectedProbability=0;

	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type!=ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse) )
	{
		if(selectedProbability) *selectedProbability=1;
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		if(selectedProbability) *selectedProbability=1;
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				if(selectedProbability) *selectedProbability=eventA/total;
				return &rays[0];
			} else {
				if(selectedProbability) *selectedProbability=eventB/total;
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	bool valid[kCapacity];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type!=ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					if(selectedProbability) *selectedProbability=(bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray))/total;
					return &rays[i];
				}
			}
		}
	}


	return 0;
}


//! From the rays stored, randomly returns a diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectDiffuse(double random, bool bNM) const
{
	return RandomlySelectDiffuse(random,bNM,nullptr);
}

ScatteredRay* ScatteredRayContainer::RandomlySelectDiffuse(double random, bool bNM, Scalar* selectedProbability) const
{
	if(selectedProbability) *selectedProbability=0;

	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type==ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse) )
	{
		if(selectedProbability) *selectedProbability=1;
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		if(selectedProbability) *selectedProbability=1;
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				if(selectedProbability) *selectedProbability=eventA/total;
				return &rays[0];
			} else {
				if(selectedProbability) *selectedProbability=eventB/total;
				return &rays[1];
			}
		}

		return 0;
	}

	// Otherwise we have from a whole bunch of events to choose from
	Scalar cdf[kCapacity] = {0};
	bool valid[kCapacity];
	Scalar total = 0;
	for( unsigned int i=0; i<freeidx; i++ ) {
		valid[i] = rays[i].type==ScatteredRay::eRayDiffuse;
		if( valid[i] ) {
			const Scalar prob = bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray);
			cdf[i] = total + prob;
			total += prob;
		}
	}

	if( total > NEARZERO ) {
		for( unsigned int i=0; i<freeidx; i++ ) {
			if( valid[i] ) {
				if( random < (cdf[i]/total) ) {
					if(selectedProbability) *selectedProbability=(bNM ? rays[i].krayNM : ColorMath::MaxValue(rays[i].kray))/total;
					return &rays[i];
				}
			}
		}
	}

	return 0;
}

//////////////////////////////////////////////////////////////////////
// DL-125.  One-shot diagnostic for the HWSS companion ladder's
// `ISPF::EvaluateKrayNM` fallback.
//
// The three HWSS companion loops (PathTracingIntegrator.cpp's HWSS
// body and BDPTIntegrator.cpp's eye and light subpath generators) price
// a companion wavelength with the SELECTED lobe's own kray when
// EvaluateKrayNM accepts that lobe, and otherwise use aggregate BSDF
// evaluation times cos / the stored density. This matches a sampled
// aggregate-density ray when its response matches the aggregate BSDF;
// a per-lobe conditional density can instead mispair the summed response.
// CompositeSPF declines only for the rays its stochastic WALKER emits
// (DL-221, narrowed by DL-24). TranslucentSPF now evaluates
// its normal entry/exit lobes (DL-222 closed), retaining its diagnostic
// identity only in case an unsupported lobe reaches the fallback.
//
// Warn ONCE per process per class (the CompositeSPF.cpp /
// SplatFilm.cpp log-once idiom): this sits inside the per-sample
// companion loop, so an unthrottled warning would emit millions of
// lines and cost more than the render.  The class-name pointers come
// from string literals in the overriding classes, so comparing them by
// VALUE is enough to keep one slot per class without allocating.
//////////////////////////////////////////////////////////////////////
namespace RISE
{
	void NotePerLobeDensityCompanionFallback( const ISPF* pSPF )
	{
		if( !pSPF ) {
			return;
		}
		const char* name = pSPF->PerLobeDensityFallbackName();
		if( !name ) {
			return;
		}

		static const unsigned int kMaxNamed = 8;
		static std::atomic<const char*> warned[ kMaxNamed ];

		for( unsigned int i = 0; i < kMaxNamed; i++ ) {
			// Re-examine THIS slot until it settles: a concurrent thread
			// can claim an empty slot between our load and our CAS, and
			// the claimant may be this very class.
			for( ;; ) {
				const char* cur = warned[i].load( std::memory_order_acquire );
				if( cur == name ) {
					return;					// already reported
				}
				if( cur != 0 ) {
					break;					// taken by another class; try the next slot
				}
				const char* expected = 0;
				if( !warned[i].compare_exchange_strong( expected, name ) ) {
					continue;				// lost the race; look again
				}
				GlobalLog()->PrintEx( eLog_Warning,
					"%s:: ISPF::EvaluateKrayNM declined this lobe for an HWSS companion. "
					"The AGGREGATE-BSDF fallback is paired with a PER-LOBE conditional "
					"density, so the summed response may not match the selected lobe. "
					"CompositeSPF's walker-emitted rays are DL-221; TranslucentSPF supports its "
					"normal entry/exit lobes and retains this diagnostic for unsupported "
					"types. This fallback concerns companion lanes in `hwss TRUE` spectral "
					"transport; the hero wavelength and RGB rendering are unaffected. "
					"Using `hwss FALSE` skips companion evaluation.",
					name );
				return;
			}
		}
	}
}
