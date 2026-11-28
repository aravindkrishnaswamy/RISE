//////////////////////////////////////////////////////////////////////
//
//  HairFileLoader.h - Reader for the Cem Yuksel `.hair` binary strand
//    format (cemyuksel.com/research/hairmodels).
//
//  Format (little-endian, runtime byte-swapped on BE hosts): 128-byte
//  header ("HAIR" magic, counts, flags, defaults, 88-byte ASCII info)
//  followed by bit-flagged arrays:
//    bit 0: segments (uint16/strand; s segments = s+1 points)
//    bit 1: points   (float3/point; mandatory)
//    bit 2: thickness (float/point; first/last used as full root/tip width)
//    bit 3/4: transparency/colours (skipped with one-time warning)
//
//  Points are loaded verbatim (no axis/unit transforms); root UVs are
//  controlled by `HairFileRootUVMode` (Zero or deterministic Scatter).
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
#include "../Interfaces/ProceduralDescriptors.h"
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! Pre-allocation sanity caps on declared header counts (guards downstream
		//! HairSegmentRef + BVH memory expansion).
		const unsigned int kMaxHairFileStrands = 4000000;
		const unsigned int kMaxHairFilePoints  = 64000000;

		//! Format-faithful parsed `.hair` file (policy translation lives in BuildStrandsFromHairFile).
		struct HairFileData
		{
			unsigned int	numStrands;				//!< header strand count (== pointsPerStrand.size())
			unsigned int	numPoints;				//!< header total point count
			unsigned int	arrayFlags;				//!< header bit flags
			unsigned int	defaultSegments;		//!< fallback segments/strand when segments array absent
			float			defaultThickness;		//!< fallback thickness when thickness array absent
			float			defaultTransparency;	//!< diagnostics only
			float			defaultColor[3];		//!< diagnostics only
			std::string		info;					//!< NUL-trimmed printable ASCII header info block
			std::string		sourceFile;				//!< filename passed to LoadHairFile, for diagnostics

			//! Points (segments + 1) per strand; sums to numPoints. An entry of 1
			//! denotes a 0-segment strand dropped later by BuildStrandsFromHairFile.
			std::vector<unsigned int>	pointsPerStrand;

			//! Concatenated xyz coordinates (`3 * numPoints` floats).
			std::vector<float>			points;

			//! Per-point thickness (`numPoints` floats), or empty if absent.
			std::vector<float>			thickness;

			bool	hasSegmentsArray;
			bool	hasThicknessArray;
			bool	hasTransparencyArray;	//!< true if present in file (values discarded)
			bool	hasColorArray;			//!< true if present in file (values discarded)

			HairFileData() :
				numStrands( 0 ), numPoints( 0 ), arrayFlags( 0 ), defaultSegments( 0 ),
				defaultThickness( 0 ), defaultTransparency( 0 ),
				hasSegmentsArray( false ), hasThicknessArray( false ),
				hasTransparencyArray( false ), hasColorArray( false )
			{
				defaultColor[0] = defaultColor[1] = defaultColor[2] = 0;
			}
		};

		//! Reads a `.hair` file (resolved via GlobalMediaPathLocator) into `out`.
		//! Refuses the whole file on bad magic/header/flags, zero/over-cap counts,
		//! segment-sum mismatch, truncated length, or non-finite default thickness.
		//! Surplus trailing bytes warn; non-finite points/thicknesses and 0-segment
		//! strands are deferred to per-strand filtering in BuildStrandsFromHairFile.
		/// \return TRUE if successful, FALSE otherwise
		bool LoadHairFile(
			const char*		filename,	///< [in] Path to the .hair file, resolved via the media-path locator
			HairFileData&	out,		///< [out] Receives the parsed file
			const char*		who			///< [in] Name of the authoring chunk, for diagnostics (may be null)
			);

		//! Translates parsed `.hair` data into `HairGeometry::StrandDesc` entries.
		//! `widthRootScale`/`widthTipScale` multiply first/last point thickness (falling
		//! back to human-hair defaults 0.0001/0.00003 if header thickness <= 0).
		//! Rejects <2-point, non-finite, or non-positive-width strands individually; fails
		//! whole-file on array-size mismatch, range overrun, or zero surviving strands.
		//! `rootUVMode`: Zero -> (0,0); Scatter -> deterministic UV keyed on file-order index.
		/// \return TRUE if successful, FALSE otherwise
		bool BuildStrandsFromHairFile(
			const HairFileData&						data,			///< [in] A file parsed by LoadHairFile
			const double							widthRootScale,	///< [in] Multiplier on the file's root thickness
			const double							widthTipScale,	///< [in] Multiplier on the file's tip thickness
			std::vector<HairGeometry::StrandDesc>&	out,			///< [out] Receives one StrandDesc per surviving strand
			const char*								who,			///< [in] Name of the authoring chunk, for diagnostics (may be null)
			const HairFileRootUVMode				rootUVMode = HairFileRootUVMode::Zero	///< [in] How to assign each surviving strand's rootUV; see above
			);
	}
}

#endif
