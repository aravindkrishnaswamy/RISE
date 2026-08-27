//////////////////////////////////////////////////////////////////////
//
//  HairGenerator.h - The painter-driven GROOM GENERATOR: turns a
//    `HairGroomRecipe` (a base geometry + a handful of scalar / colour
//    fields + a seed) into the explicit strand list `HairGeometry`
//    already knows how to build.  Slice D of the hair/fur arc,
//    docs/HAIR_FUR_DESIGN.md section 5.3.
//
//  WHERE THIS RUNS.  Never at parse time.  `HairGeometry`'s deferred-
//  groom constructor stores the recipe and `HairGeometry::Realize()`
//  calls `GenerateHairStrands` once, single-threaded, before the
//  parallel rasterize -- the same deferred-build contract
//  `DisplacedGeometry` uses for its tessellate+displace bake
//  (IGeometry.h's Realize() contract; DisplacedGeometry.cpp).  Three
//  reasons, all from the design: generation needs the RESOLVED base
//  geometry and painters, it can be expensive (a 1M-strand groom), and
//  it must not happen lazily on the const hot path.
//
//  DETERMINISM IS A CONTRACT, NOT AN ACCIDENT.  Every random draw comes
//  from a self-contained integer PRNG seeded from `recipe.p.seed` (see
//  the .cpp), never from `GlobalRNG()`.  The same recipe and seed
//  therefore produce byte-identical strand arrays on every run, every
//  platform, and every re-`Realize()` -- which is what makes an animated
//  groom stable frame to frame.  Per-strand jitter draws from a stream
//  keyed on (seed, strand index) rather than from one global sequence,
//  so a strand's frizz does not change when an EARLIER strand is
//  rejected by the density mask.
//
//  WHAT IS NOT HERE.  Animation-following (a groom that tracks a
//  deforming base mesh) is out of scope for Phase 1: a re-Realize()
//  regenerates deterministically from the seed against whatever the base
//  geometry currently tessellates to, which is correct for a static base
//  and for a base whose shape is fixed at load, but there is no
//  correspondence machinery that would keep a given follicle attached to
//  a given surface point across a deforming base.  Explicit strand
//  authoring in the scene language is likewise deferred -- this
//  generator is grow-on-a-surface only.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_GENERATOR_
#define HAIR_GENERATOR_

#include "HairGeometry.h"
#include "../Interfaces/ProceduralDescriptors.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! Hard cap on the strand budget a single groom may request.
		//! At the design's ~0.6-0.7 GB-per-1M-strand figure
		//! (HAIR_FUR_DESIGN.md section 5.2E) this is already a
		//! multi-gigabyte groom, so it is a "you did not mean this"
		//! guard rather than a supported working point -- the
		//! path_instances budget-cap precedent (RISE_API.cpp's 20M
		//! vertex / 100K instance caps).  Enforced with a NAMED
		//! diagnostic at construction, before any generation work.
		const unsigned int kMaxHairStrandCount = 2000000;

		//! Cheap, PARSE-TIME validation of a groom recipe: exactly the
		//! checks that can be made without tessellating anything.
		//! Returns false and logs ONE diagnostic naming the offending
		//! parameter and its value.  `chunkName` is only used to make
		//! that diagnostic point at the author's own chunk.
		bool ValidateHairGroomRecipe( const HairGroomRecipe& recipe, const char* chunkName );

		//! Runs the generation.  Appends to `out` (which is expected to
		//! be empty) one `StrandDesc` per SURVIVING root -- candidates
		//! removed by the density mask, or whose length painter drove
		//! their length to zero, simply do not appear.
		//!
		//! Returns false (with a diagnostic) only for a failure that
		//! produces NO groom at all: a null / non-tessellatable base, a
		//! base that tessellates to zero triangles or zero total area.
		//! An empty-but-successful result (every candidate masked out)
		//! returns true with an empty `out` and a warning -- that is an
		//! authoring outcome, not an engine failure.
		//!
		//! `chunkName` names the scene chunk in every diagnostic.
		bool GenerateHairStrands(
			const HairGroomRecipe&                    recipe,
			const char*                               chunkName,
			std::vector<HairGeometry::StrandDesc>&    out );
	}
}

#endif
