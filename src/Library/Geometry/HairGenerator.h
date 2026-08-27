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
//  the .cpp), never from `GlobalRNG()`.  Two claims, and they are not
//  the same strength:
//
//    * THE RANDOM STREAM is bit-identical everywhere.  The generator is
//      fixed-width integer arithmetic only, so the same recipe and seed
//      draw the same numbers in the same order on every run, every
//      re-`Realize()`, every build and every platform.  That is the
//      property `GlobalRNG()` (a compile-time-configurable Mersenne
//      Twister) could not give.
//
//    * THE GROOM ITSELF is byte-identical for the same binary on the
//      same platform, which is what makes a re-`Realize()` and a
//      re-render reproduce frame to frame.  ACROSS platforms it is
//      identical only to floating-point tolerance: the growth arithmetic
//      is `Scalar` math through `sin`/`cos`/`sqrt`, so a different
//      libm, a different FMA contraction, or macOS's `-ffast-math`
//      against Linux's strict IEEE can move a control point in the last
//      couple of ULPs.  Do NOT hash a groom across platforms and expect
//      a match; compare it against a tolerance.
//
//  Per-strand jitter and per-candidate placement each draw from their
//  own stream keyed on (seed, candidate ordinal) rather than from one
//  sequence walked across the whole groom, so neither a strand rejected
//  by the density mask nor a candidate dropped on a degenerate triangle
//  reshuffles anything that comes after it.
//
//  WHAT IS NOT HERE.  Animation-following (a groom that tracks a
//  deforming base mesh) is out of scope for Phase 1: a re-Realize()
//  regenerates deterministically from the seed against whatever the base
//  geometry currently tessellates to, which is correct for a static base
//  and for a base whose shape is fixed at load, but there is no
//  correspondence machinery that would keep a given follicle attached to
//  a given surface point across a deforming base.
//
//  PLACEMENT IS ALWAYS GROW-ON-A-SURFACE.  A groom's roots always come
//  from the area-weighted sampler; there is no way to author a strand's
//  POSITION in the scene language.  What CAN be authored explicitly is a
//  strand's SHAPE -- a `hair_guides` set (Phase 2), whose polylines are
//  interpolated onto the generated roots.  Guides never place anything.
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

		//! Hard caps on an authored guide set.  Both are "you did not mean
		//! this" guards rather than working points: `hair_guides` is the
		//! HAND / AGENT authoring surface, where tens to a few hundred
		//! guides is the intended scale, and guide selection is a LINEAR
		//! scan over the set per strand (kNearestGuides below) -- so a
		//! five-figure guide count would cost more than the groom.  A bulk
		//! interchange importer (which is where a five-figure guide set
		//! would legitimately come from) is a later slice and will want a
		//! spatial index before it raises these.
		const unsigned int kMaxHairGuideStrands       = 4096;
		const unsigned int kMaxHairGuidePointsPerGuide = 4096;

		//! How many guides a strand's shape is interpolated from.  Clamped
		//! DOWN to the guide count when the set is smaller (one guide is a
		//! perfectly legal set -- every strand then reproduces it).
		const unsigned int kNearestGuides = 3;

		//! Cheap, PARSE-TIME validation of a groom recipe: exactly the
		//! checks that can be made without tessellating anything.
		//! Returns false and logs ONE diagnostic naming the offending
		//! parameter and its value.  `chunkName` is only used to make
		//! that diagnostic point at the author's own chunk.
		bool ValidateHairGroomRecipe( const HairGroomRecipe& recipe, const char* chunkName );

		//! Cheap, PARSE-TIME validation of an authored guide set: counts,
		//! caps, finiteness, and the one geometric requirement a guide has
		//! to meet to carry a SHAPE at all -- a strictly positive total arc
		//! length (a guide whose points all coincide is a point, and has
		//! neither a direction to align nor a length to normalise by).
		//! Returns false and logs ONE diagnostic naming the offending guide
		//! by INDEX.  `chunkName` names the author's own chunk.
		bool ValidateHairGuides( const HairGuidesDescriptor& guides, const char* chunkName );

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
