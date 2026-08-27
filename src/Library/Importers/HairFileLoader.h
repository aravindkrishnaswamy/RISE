//////////////////////////////////////////////////////////////////////
//
//  HairFileLoader.h - Reader for the Cem Yuksel `.hair` binary strand
//    format (cemyuksel.com/research/hairmodels), the de-facto
//    interchange for bulk grooms: Blender / Houdini / XGen exports and
//    the published research hair models all speak it.  Slice P2-B of
//    the hair/fur arc, docs/HAIR_FUR_DESIGN.md section 7 Phase 2.
//
//  WHY A FILE SOURCE AT ALL.  `hair_geometry`'s grow mode authors a
//  groom as a RECIPE (a base surface plus a few painter fields) and
//  `hair_guides` authors a handful of shapes as scene text.  Neither
//  can carry a 100,000-strand production groom: that is megabytes of
//  control points, which belongs in a binary file next to the scene,
//  not inline in it.  This loader is that third source.
//
//  THE FORMAT, AND EXACTLY WHAT OF IT IS HONOURED.  128-byte header,
//  then up to five arrays back to back.  Everything below is
//  implemented from the published specification; no third-party reader
//  code was consulted or reproduced.
//
//    offset  size  field
//    ------  ----  ----------------------------------------------------
//      0      4    magic, must be the four ASCII bytes 'H','A','I','R'
//      4      4    uint32  strand count
//      8      4    uint32  total point count (ALL strands)
//     12      4    uint32  array-presence bit flags (see below)
//     16      4    uint32  default segments per strand
//     20      4    float   default thickness
//     24      4    float   default transparency
//     28     12    float3  default colour
//     40     88    char    free-form ASCII file info
//    ------  ----  ----------------------------------------------------
//    then, in this order, each present only if its flag bit is set:
//      bit 0  segments      uint16 per STRAND   -- honoured
//      bit 1  points        float3 per POINT    -- honoured (MANDATORY)
//      bit 2  thickness     float  per POINT    -- honoured, see below
//      bit 3  transparency  float  per POINT    -- READ AND DISCARDED
//      bit 4  colours       float3 per POINT    -- READ AND DISCARDED
//
//  * SEGMENTS.  A strand with s segments has s+1 POINTS.  With the
//    array present the per-strand counts come from it; without it every
//    strand carries the header's default.  Either way the counts must
//    sum to exactly the header's point count -- a mismatch is a
//    corrupt file and is refused, not patched.
//  * TRANSPARENCY and COLOURS are parsed for their LENGTH ONLY (they
//    have to be, or the byte-length check below would be wrong) and
//    then dropped.  RISE has no per-vertex strand opacity or albedo:
//    fibre colour comes from the `hair_material` (melanin / sigma_a /
//    artist colour), and opacity is not a hair concept here at all.  A
//    file carrying them loads fine and is shaded by its material; the
//    loader says so once, in a warning, rather than silently.
//  * THE FILE INFO string is exposed for diagnostics only.
//
//  THICKNESS IS AMBIGUOUS IN THE SPECIFICATION, AND THIS IS THE
//  DECISION TAKEN.  The published format description says "thickness"
//  and never says whether that is a radius or a full diameter.  Since
//  neither reading can be called correct, this loader treats it as the
//  FULL WIDTH -- the same quantity `HairGeometry::StrandDesc::rootWidth`
//  / `tipWidth` and the `hair_geometry` chunk's `width_root` /
//  `width_tip` already mean -- and passes it through with no conversion
//  factor invented on the author's behalf.  The `hair_geometry` chunk's
//  `width_root` / `width_tip` become MULTIPLIERS in file mode (default
//  1.0 = verbatim), so an author whose file means radius writes
//  `width_root 2` / `width_tip 2` and an author whose file is in
//  millimetres against a metre-scale scene writes 0.001.  See
//  `BuildStrandsFromHairFile`.
//
//  WIDTH IS A ROOT/TIP PAIR, NOT A PER-POINT CURVE.  `.hair` carries a
//  thickness per POINT; `HairGeometry` interpolates ONE root width to
//  ONE tip width linearly in arc-length fraction.  The strand's FIRST
//  and LAST thickness values are taken and the interior ones are
//  dropped.  For the overwhelmingly common file -- a constant or
//  monotonically tapering thickness -- that is lossless or nearly so;
//  a file that bulges mid-strand loses the bulge.
//
//  NO ROOT UVs.  The format has no per-strand surface parameterization,
//  so every imported strand reports ptCoord1 = (0, 0).  Consequence,
//  and it is a real one: a `hair_material` driven by a painter over the
//  root UV (a scalp-space tint or roughness map) evaluates at one
//  single point across the whole imported groom and therefore does not
//  vary.  Uniform materials, and materials driven by the fibre's own
//  along-strand coordinate, are unaffected.
//
//  NO AXIS OR UNIT JUGGLING.  `.hair` files carry no unit or up-axis
//  declaration -- the published models are in assorted scales and
//  orientations.  This loader therefore transforms NOTHING: the points
//  arrive in the file's own coordinates and the `standard_object`'s
//  `scale` / `orientation` / `matrix` places the groom, exactly as it
//  does for every mesh RISE imports.
//
//  ENDIANNESS.  The format is defined little-endian.  Multi-byte
//  values are read raw and byte-swapped when (and only when) the host
//  is big-endian, detected at run time -- so the reader is correct on a
//  BE host without a build flag, and pays nothing on the LE hosts RISE
//  actually targets.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_FILE_LOADER_
#define HAIR_FILE_LOADER_

#include "../Geometry/HairGeometry.h"
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! Sanity caps on a `.hair` file's declared counts, checked
		//! BEFORE a single byte of array data is read or a single
		//! allocation is made -- so a corrupt or hostile header can
		//! never talk this loader into a multi-gigabyte reserve.
		//!
		//! Both are "you did not mean this" guards rather than working
		//! points, the same shape as `kMaxHairStrandCount`
		//! (HairGenerator.h) and the path_instances budget caps: 4M
		//! strands is ~27x a full human scalp, and 64M control points is
		//! already ~1.5 GB once expanded into `StrandDesc`s.
		const unsigned int kMaxHairFileStrands = 4000000;
		const unsigned int kMaxHairFilePoints  = 64000000;

		//////////////////////////////////////////////////////////////
		//
		//  HairFileData -- one `.hair` file, parsed, in the format's own
		//  terms.  Deliberately format-faithful and free of any RISE
		//  geometry concept: the translation into strands (and every
		//  policy decision that goes with it) lives in the separate
		//  `BuildStrandsFromHairFile` below, so the two can be tested
		//  independently.
		//
		//////////////////////////////////////////////////////////////
		struct HairFileData
		{
			unsigned int	numStrands;				//!< header field, and the length of `pointsPerStrand`
			unsigned int	numPoints;				//!< header field, and the point count of `points` / `thickness`
			unsigned int	arrayFlags;				//!< header bit flags, verbatim
			unsigned int	defaultSegments;		//!< header field; used when the segments array is absent
			float			defaultThickness;		//!< header field; used when the thickness array is absent
			float			defaultTransparency;	//!< header field; retained for diagnostics only
			float			defaultColor[3];		//!< header field; retained for diagnostics only
			std::string		info;					//!< the header's 88-byte ASCII info block, NUL-trimmed

			//! POINTS (not segments) per strand -- ALWAYS populated,
			//! whether the file carried a segments array or only the
			//! header default.  `numStrands` entries, each >= 2,
			//! summing to exactly `numPoints`.
			std::vector<unsigned int>	pointsPerStrand;

			//! x y z per point, every strand concatenated in file order.
			//! `3 * numPoints` entries.
			std::vector<float>			points;

			//! Per-POINT thickness, `numPoints` entries -- or EMPTY when
			//! the file carried no thickness array (use
			//! `defaultThickness` for every point in that case).
			std::vector<float>			thickness;

			bool	hasSegmentsArray;
			bool	hasThicknessArray;
			bool	hasTransparencyArray;	//!< true iff the file HAD one; its values are discarded
			bool	hasColorArray;			//!< true iff the file HAD one; its values are discarded

			HairFileData() :
				numStrands( 0 ), numPoints( 0 ), arrayFlags( 0 ), defaultSegments( 0 ),
				defaultThickness( 0 ), defaultTransparency( 0 ),
				hasSegmentsArray( false ), hasThicknessArray( false ),
				hasTransparencyArray( false ), hasColorArray( false )
			{
				defaultColor[0] = defaultColor[1] = defaultColor[2] = 0;
			}
		};

		//! Reads a `.hair` file into `out`.  The path is resolved
		//! through `GlobalMediaPathLocator()`, exactly as every other
		//! file-backed chunk in RISE resolves its `file` parameter.
		//!
		//! REFUSES THE WHOLE FILE (returns false, logs ONE diagnostic
		//! naming what was wrong) on: an unopenable file, a file shorter
		//! than the 128-byte header, a bad magic, a missing points-array
		//! flag, an unknown (reserved) flag bit -- which would make the
		//! array layout unknowable -- a zero strand or point count,
		//! counts over the caps above, a segment/point-count arithmetic
		//! mismatch, a strand with fewer than 2 points, or a file whose
		//! actual byte length is shorter than the header says the arrays
		//! need.  A file LONGER than its arrays require loads, with a
		//! warning naming the surplus.
		//!
		//! NON-FINITE DATA IS NOT REFUSED HERE.  A NaN / infinity in the
		//! points or thickness array is a per-STRAND defect, so it is
		//! caught by `BuildStrandsFromHairFile` (which drops that strand
		//! and counts it) rather than costing the whole file.  A
		//! non-finite HEADER DEFAULT thickness is refused here, because
		//! it would poison every strand in a file with no thickness
		//! array.
		//!
		//! `who` names the authoring chunk in every diagnostic (may be
		//! null).
		/// \return TRUE if successful, FALSE otherwise
		bool LoadHairFile(
			const char*		filename,	///< [in] Path to the .hair file, resolved via the media-path locator
			HairFileData&	out,		///< [out] Receives the parsed file
			const char*		who			///< [in] Name of the authoring chunk, for diagnostics (may be null)
			);

		//! Translates a parsed `.hair` file into the explicit strand
		//! list `HairGeometry`'s explicit-strand constructor takes.
		//!
		//! WIDTHS.  `widthRootScale` / `widthTipScale` MULTIPLY the
		//! file's own thickness at the strand's first / last point
		//! (1.0 = verbatim); see the header comment for why the file's
		//! thickness is read as a full width rather than a radius.  When
		//! the file carries no thickness array the header's default
		//! thickness is used for both ends; when THAT is not strictly
		//! positive either -- which a surprising number of real files
		//! do, the format not requiring it -- the groom falls back to
		//! RISE's own human-hair defaults (0.0001 / 0.00003 scene units)
		//! with a warning, so the scale factors still have something
		//! meaningful to act on.
		//!
		//! PER-STRAND REJECTION, NOT WHOLE-FILE FAILURE, for a strand
		//! whose points are not all finite, or whose resolved root/tip
		//! width is not finite and strictly positive: that strand is
		//! dropped and counted, and one summary warning names the total.
		//! Returns false only when the result would be an EMPTY groom
		//! (every strand rejected, or nothing to convert), which is a
		//! failure the author has to see.
		//!
		//! `who` names the authoring chunk in every diagnostic (may be
		//! null).
		/// \return TRUE if successful, FALSE otherwise
		bool BuildStrandsFromHairFile(
			const HairFileData&						data,			///< [in] A file parsed by LoadHairFile
			const double							widthRootScale,	///< [in] Multiplier on the file's root thickness
			const double							widthTipScale,	///< [in] Multiplier on the file's tip thickness
			std::vector<HairGeometry::StrandDesc>&	out,			///< [out] Receives one StrandDesc per surviving strand
			const char*								who				///< [in] Name of the authoring chunk, for diagnostics (may be null)
			);
	}
}

#endif
