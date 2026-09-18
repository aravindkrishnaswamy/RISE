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
ScatteredRay* ScatteredRayContainer::RandomlySelect(
		const double random,										///< [in] Random number to use in ray selection
		const bool bNM												///< [in] Should the spectral values be used when selecting?
		) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( freeidx == 1 ) {
		return &rays[0];
	}

	if( freeidx == 2 ) {
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
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
				return &rays[i];
			}
		}
	}

	return 0;
}

//! From the rays stored, randomly returns a non diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectNonDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type!=ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
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
					return &rays[i];
				}
			}
		}
	}


	return 0;
}


//! From the rays stored, randomly returns a diffuse ray
ScatteredRay* ScatteredRayContainer::RandomlySelectDiffuse(
	const double random,										///< [in] Random number to use in ray selection
	const bool bNM												///< [in] Should the spectral values be used when selecting?
	) const
{
	if( freeidx == 0 ) {
		return 0;
	}

	if( (freeidx==1&&rays[0].type==ScatteredRay::eRayDiffuse) || (freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type!=ScatteredRay::eRayDiffuse) )
	{
		return &rays[0];
	}
	else if( freeidx==2 && rays[0].type!=ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		return &rays[1];
	}
	else if( freeidx==2 && rays[0].type==ScatteredRay::eRayDiffuse && rays[1].type==ScatteredRay::eRayDiffuse )
	{
		const Scalar eventA = bNM ? rays[0].krayNM : ColorMath::MaxValue(rays[0].kray);
		const Scalar eventB = bNM ? rays[1].krayNM : ColorMath::MaxValue(rays[1].kray);

		const Scalar total = eventA + eventB;

		if( total > NEARZERO ) {
			if( random < (eventA/total) ) {
				return &rays[0];
			} else {
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
// a companion wavelength with the SELECTED lobe's own kray when the SPF
// implements `EvaluateKrayNM`, and otherwise fall back to
// `IBSDF::valueNM(outDir) * cos / pS->pdf`.  That fallback is EXACT for
// an SPF whose emitted ray carries the AGGREGATE mixture density and
// wrong for one that carries a PER-LOBE conditional density; the latter
// all implement the method now except `CompositeSPF` (DL-221), which
// names itself here instead of failing silently.
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
			const char* cur = warned[i].load( std::memory_order_acquire );
			if( cur == name ) {
				return;						// already reported
			}
			if( cur == 0 ) {
				const char* expected = 0;
				if( !warned[i].compare_exchange_strong( expected, name ) ) {
					// Another thread claimed this slot; re-examine it.
					i--;
					continue;
				}
				GlobalLog()->PrintEx( eLog_Warning,
					"%s:: an HWSS companion wavelength was priced through the AGGREGATE-BSDF "
					"fallback because this SPF does not implement ISPF::EvaluateKrayNM, and it "
					"stores a PER-LOBE conditional density on each emitted ray -- so the "
					"companion's throughput pairs the material's summed BSDF with one lobe's "
					"density (DL-125/DL-221).  Affects `hwss TRUE` spectral renders under PT, "
					"BDPT, VCM and MLT only; the hero wavelength and every RGB render are "
					"unaffected.  Render with `hwss FALSE` to avoid it.",
					name );
				return;
			}
		}
	}
}
