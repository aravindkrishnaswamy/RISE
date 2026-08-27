//////////////////////////////////////////////////////////////////////
//
//  HairFileLoader.cpp - Implementation of the Cem Yuksel `.hair`
//    reader.  See the header for the format table, for what of the
//    format is honoured versus read-and-dropped, and for the two
//    decisions the specification does not make for us (thickness as a
//    full width; no axis or unit juggling).
//
//  The whole file is validated BEFORE any array allocation: the header
//  is decoded, the declared counts are checked against the caps and
//  against each other, and the arrays' total byte length is checked
//  against the file's actual length.  Only then is anything reserved.
//  That ordering is the point -- a corrupt or hostile 128-byte header
//  must not be able to talk this loader into a multi-gigabyte reserve
//  or a read past the end of the mapping.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HairFileLoader.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/MediaPathLocator.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// ---- the format's array-presence bit flags (header bytes 12-15) ---
	const unsigned int kFlagSegments     = 1u << 0;
	const unsigned int kFlagPoints       = 1u << 1;
	const unsigned int kFlagThickness    = 1u << 2;
	const unsigned int kFlagTransparency = 1u << 3;
	const unsigned int kFlagColors       = 1u << 4;
	const unsigned int kFlagKnownMask    = kFlagSegments | kFlagPoints | kFlagThickness |
	                                       kFlagTransparency | kFlagColors;

	const unsigned int kHeaderBytes = 128;

	//! Diagnostic prefix.  Every message this file emits names the
	//! authoring chunk and the file, so an author with twenty grooms
	//! knows which one is broken.
	std::string Who( const char* who, const char* filename )
	{
		std::string s( "HairFileLoader:: `" );
		s += ( who && who[0] ) ? who : "(unnamed)";
		s += "`: `";
		s += filename ? filename : "(null)";
		s += "`";
		return s;
	}

	//! Run-time host-endianness probe.  The `.hair` format is defined
	//! little-endian; every RISE target is little-endian, so this is
	//! true everywhere the renderer currently builds and the swap
	//! helpers below never fire.  It is a probe rather than a
	//! `static_assert` precisely so a big-endian host reads the format
	//! CORRECTLY instead of failing to build or, worse, reading
	//! byte-reversed floats.
	bool HostIsLittleEndian()
	{
		const unsigned int one = 1u;
		unsigned char b[4];
		memcpy( b, &one, sizeof(b) );
		return b[0] == 1;
	}

	void SwapEach16( void* p, const size_t count )
	{
		unsigned char* b = static_cast<unsigned char*>( p );
		for( size_t i = 0; i < count; ++i, b += 2 ) {
			const unsigned char t = b[0]; b[0] = b[1]; b[1] = t;
		}
	}

	void SwapEach32( void* p, const size_t count )
	{
		unsigned char* b = static_cast<unsigned char*>( p );
		for( size_t i = 0; i < count; ++i, b += 4 ) {
			unsigned char t = b[0]; b[0] = b[3]; b[3] = t;
			t = b[1]; b[1] = b[2]; b[2] = t;
		}
	}

	//! Decodes a little-endian uint32 out of a byte buffer, independent
	//! of the host's own byte order.  Used for the header only, where
	//! there are eight scalars rather than a bulk array.
	unsigned int DecodeLEU32( const unsigned char* p )
	{
		return  (unsigned int)p[0]         |
		       ((unsigned int)p[1] <<  8 ) |
		       ((unsigned int)p[2] << 16 ) |
		       ((unsigned int)p[3] << 24 );
	}

	//! Same, reinterpreted as an IEEE-754 binary32.  RISE already
	//! assumes IEEE-754 floats everywhere (every binary asset format it
	//! reads does); this only fixes the BYTE ORDER.
	float DecodeLEF32( const unsigned char* p )
	{
		const unsigned int bits = DecodeLEU32( p );
		float f = 0;
		memcpy( &f, &bits, sizeof(f) );
		return f;
	}

	//! 64-bit file length.  `ftell` returns a 32-bit `long` on Windows,
	//! which silently overflows on a groom over 2 GB -- and a 64M-point
	//! file carrying points + thickness + transparency + colours is
	//! 2.05 GB, i.e. exactly at the cap this loader permits.  Returns a
	//! negative value on failure, and leaves the stream positioned at
	//! the start of the file on success.
	long long FileLength64( FILE* f )
	{
	#if defined(_WIN32)
		if( _fseeki64( f, 0, SEEK_END ) != 0 ) return -1;
		const long long len = _ftelli64( f );
		if( _fseeki64( f, 0, SEEK_SET ) != 0 ) return -1;
	#else
		if( fseeko( f, 0, SEEK_END ) != 0 ) return -1;
		const long long len = (long long)ftello( f );
		if( fseeko( f, 0, SEEK_SET ) != 0 ) return -1;
	#endif
		return len;
	}
}

namespace RISE
{
	namespace Implementation
	{

bool LoadHairFile( const char* filename, HairFileData& out, const char* who )
{
	out = HairFileData();

	if( !filename || !filename[0] ) {
		GlobalLog()->PrintEx( eLog_Error,
			"HairFileLoader:: `%s`: no file name given",
			( who && who[0] ) ? who : "(unnamed)" );
		return false;
	}

	const std::string label = Who( who, filename );

	// Media-path resolution, the same one every other file-backed chunk
	// gets (TriangleMeshLoaderPLY, DiskFileReadBuffer, the painters):
	// an unqualified name is looked up under $RISE_MEDIA_PATH, and
	// Find() hands back the original string when it cannot resolve it,
	// so the fopen below produces the ordinary "cannot open" diagnostic.
	const String resolved = GlobalMediaPathLocator().Find( filename );

	FILE* f = fopen( resolved.c_str(), "rb" );
	if( !f ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: cannot open the file (looked at `%s`; unqualified names resolve against $RISE_MEDIA_PATH)",
			label.c_str(), resolved.c_str() );
		return false;
	}

	const long long fileLen = FileLength64( f );
	if( fileLen < (long long)kHeaderBytes ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: not a .hair file -- %lld bytes is shorter than the format's 128-byte header",
			label.c_str(), fileLen );
		fclose( f );
		return false;
	}

	unsigned char hdr[kHeaderBytes];
	if( fread( hdr, 1, kHeaderBytes, f ) != kHeaderBytes ) {
		GlobalLog()->PrintEx( eLog_Error, "%s: failed to read the 128-byte header", label.c_str() );
		fclose( f );
		return false;
	}

	if( hdr[0] != 'H' || hdr[1] != 'A' || hdr[2] != 'I' || hdr[3] != 'R' ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: bad magic -- the first four bytes must be `HAIR` (got 0x%02X 0x%02X 0x%02X 0x%02X)",
			label.c_str(), (unsigned)hdr[0], (unsigned)hdr[1], (unsigned)hdr[2], (unsigned)hdr[3] );
		fclose( f );
		return false;
	}

	out.numStrands          = DecodeLEU32( hdr +  4 );
	out.numPoints           = DecodeLEU32( hdr +  8 );
	out.arrayFlags          = DecodeLEU32( hdr + 12 );
	out.defaultSegments     = DecodeLEU32( hdr + 16 );
	out.defaultThickness    = DecodeLEF32( hdr + 20 );
	out.defaultTransparency = DecodeLEF32( hdr + 24 );
	out.defaultColor[0]     = DecodeLEF32( hdr + 28 );
	out.defaultColor[1]     = DecodeLEF32( hdr + 32 );
	out.defaultColor[2]     = DecodeLEF32( hdr + 36 );
	{
		// The info block is a fixed 88 bytes of ASCII, NOT necessarily
		// NUL-terminated -- bound the scan by the block, never by a
		// terminator that may not be there.
		const char* p = reinterpret_cast<const char*>( hdr + 40 );
		size_t n = 0;
		while( n < 88 && p[n] != '\0' ) { ++n; }
		out.info.assign( p, n );
	}

	out.hasSegmentsArray     = ( out.arrayFlags & kFlagSegments     ) != 0;
	out.hasThicknessArray    = ( out.arrayFlags & kFlagThickness    ) != 0;
	out.hasTransparencyArray = ( out.arrayFlags & kFlagTransparency ) != 0;
	out.hasColorArray        = ( out.arrayFlags & kFlagColors       ) != 0;

	// ---- header sanity, all of it before a single allocation --------

	if( ( out.arrayFlags & kFlagPoints ) == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: the header declares no POINTS array (flag bit 1 clear) -- a .hair file without points carries no geometry",
			label.c_str() );
		fclose( f );
		return false;
	}
	if( ( out.arrayFlags & ~kFlagKnownMask ) != 0 ) {
		// Refused rather than ignored: an unknown array would sit
		// somewhere in the stream at an unknown size, so every offset
		// after it -- including the ones this loader computes -- would
		// be wrong.  Better a named refusal than silently-shifted data.
		GlobalLog()->PrintEx( eLog_Error,
			"%s: the header sets reserved flag bits (flags 0x%08X, known mask 0x%08X) -- this file uses an "
			"array this loader does not know, so the layout of everything after it is unknowable",
			label.c_str(), out.arrayFlags, kFlagKnownMask );
		fclose( f );
		return false;
	}
	if( out.numStrands == 0 || out.numPoints == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: the header declares %u strands and %u points -- both must be non-zero",
			label.c_str(), out.numStrands, out.numPoints );
		fclose( f );
		return false;
	}
	if( out.numStrands > kMaxHairFileStrands ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: %u strands exceeds the %u cap (a groom that large is gigabytes of control points; "
			"if the file is genuinely this big, split it)",
			label.c_str(), out.numStrands, kMaxHairFileStrands );
		fclose( f );
		return false;
	}
	if( out.numPoints > kMaxHairFilePoints ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: %u points exceeds the %u cap",
			label.c_str(), out.numPoints, kMaxHairFilePoints );
		fclose( f );
		return false;
	}
	if( !RISE::IsFiniteDouble( (double)out.defaultThickness ) ) {
		// Refused whole-file (unlike a bad value in the thickness ARRAY,
		// which costs one strand): with no thickness array this single
		// number is every strand's width.
		GlobalLog()->PrintEx( eLog_Error,
			"%s: the header's default thickness is not finite (%g)",
			label.c_str(), (double)out.defaultThickness );
		fclose( f );
		return false;
	}

	// ---- declared byte length vs. the file's actual length ----------
	//
	// In 64-bit arithmetic throughout: the products below overflow a
	// 32-bit unsigned at the caps above (64M points x 12 bytes is
	// already 768 MB, and the four arrays together are 2.05 GB).
	const unsigned long long nS = (unsigned long long)out.numStrands;
	const unsigned long long nP = (unsigned long long)out.numPoints;
	unsigned long long need = (unsigned long long)kHeaderBytes;
	const unsigned long long segBytes   = out.hasSegmentsArray     ? 2ull  * nS : 0ull;
	const unsigned long long ptBytes    =                            12ull * nP;
	const unsigned long long thickBytes = out.hasThicknessArray    ? 4ull  * nP : 0ull;
	const unsigned long long transBytes = out.hasTransparencyArray ? 4ull  * nP : 0ull;
	const unsigned long long colBytes   = out.hasColorArray        ? 12ull * nP : 0ull;
	need += segBytes + ptBytes + thickBytes + transBytes + colBytes;

	if( (unsigned long long)fileLen < need ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: TRUNCATED -- the header describes %llu bytes of header + arrays but the file is only %lld bytes",
			label.c_str(), need, fileLen );
		fclose( f );
		return false;
	}
	if( (unsigned long long)fileLen > need ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"%s: %llu trailing bytes after the arrays the header describes (%llu of %lld) -- loading the "
			"described arrays and ignoring the surplus",
			label.c_str(), (unsigned long long)fileLen - need, need, fileLen );
	}

	// ---- per-strand point counts ------------------------------------
	//
	// The format stores SEGMENTS; a strand with s segments has s+1
	// points.  Whether the counts come from the array or from the
	// header default, they must sum to exactly the declared point count
	// -- a file that disagrees with itself is corrupt, and every array
	// index below is derived from these numbers.
	out.pointsPerStrand.resize( out.numStrands );

	if( out.hasSegmentsArray ) {
		std::vector<unsigned short> segs( out.numStrands );
		if( fread( &segs[0], 2, out.numStrands, f ) != out.numStrands ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s: failed to read the %u-entry segments array", label.c_str(), out.numStrands );
			fclose( f );
			return false;
		}
		if( !HostIsLittleEndian() ) {
			SwapEach16( &segs[0], out.numStrands );
		}
		unsigned long long total = 0;
		for( unsigned int i = 0; i < out.numStrands; ++i ) {
			if( segs[i] == 0 ) {
				GlobalLog()->PrintEx( eLog_Error,
					"%s: strand %u declares 0 segments -- a strand needs at least one segment (2 points)",
					label.c_str(), i );
				fclose( f );
				return false;
			}
			out.pointsPerStrand[i] = (unsigned int)segs[i] + 1u;
			total += out.pointsPerStrand[i];
		}
		if( total != nP ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s: the segments array accounts for %llu points but the header declares %u -- corrupt file",
				label.c_str(), total, out.numPoints );
			fclose( f );
			return false;
		}
	} else {
		if( out.defaultSegments == 0 ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s: no segments array and the header's default segment count is 0 -- there is no way to know "
				"where one strand ends and the next begins",
				label.c_str() );
			fclose( f );
			return false;
		}
		const unsigned long long perStrand = (unsigned long long)out.defaultSegments + 1ull;
		if( perStrand * nS != nP ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s: no segments array, so every strand carries the header's default %u segments (%llu points), "
				"but %u strands x %llu != the declared %u points -- corrupt file",
				label.c_str(), out.defaultSegments, perStrand, out.numStrands, perStrand, out.numPoints );
			fclose( f );
			return false;
		}
		for( unsigned int i = 0; i < out.numStrands; ++i ) {
			out.pointsPerStrand[i] = (unsigned int)perStrand;
		}
	}

	// ---- points ------------------------------------------------------
	out.points.resize( (size_t)nP * 3 );
	if( fread( &out.points[0], 4, (size_t)nP * 3, f ) != (size_t)nP * 3 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s: failed to read the %u-point positions array", label.c_str(), out.numPoints );
		fclose( f );
		return false;
	}
	if( !HostIsLittleEndian() ) {
		SwapEach32( &out.points[0], (size_t)nP * 3 );
	}

	// ---- thickness (optional) ---------------------------------------
	if( out.hasThicknessArray ) {
		out.thickness.resize( (size_t)nP );
		if( fread( &out.thickness[0], 4, (size_t)nP, f ) != (size_t)nP ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s: failed to read the %u-entry thickness array", label.c_str(), out.numPoints );
			fclose( f );
			return false;
		}
		if( !HostIsLittleEndian() ) {
			SwapEach32( &out.thickness[0], (size_t)nP );
		}
	}

	// Transparency and colours are deliberately NOT read: their only
	// role in this loader is the byte-length arithmetic above, and RISE
	// has nowhere to put them (see the header).  Say so once, so an
	// author whose file carries authored fibre colours knows why the
	// render is uniformly the material's colour instead.
	if( out.hasTransparencyArray || out.hasColorArray ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"%s: the file carries per-point %s%s%s, which RISE has no home for -- fibre colour comes from the "
			"bound `hair_material` (melanin / sigma_a / artist colour) and per-strand opacity is not modelled.  "
			"Loading geometry and widths only",
			label.c_str(),
			out.hasTransparencyArray ? "transparency" : "",
			( out.hasTransparencyArray && out.hasColorArray ) ? " and " : "",
			out.hasColorArray ? "colours" : "" );
	}

	fclose( f );

	GlobalLog()->PrintEx( eLog_Info,
		"%s: loaded %u strands / %u points (segments array %s, thickness array %s)%s%s",
		label.c_str(), out.numStrands, out.numPoints,
		out.hasSegmentsArray  ? "present" : "absent (header default)",
		out.hasThicknessArray ? "present" : "absent (header default)",
		out.info.empty() ? "" : "; file info: ",
		out.info.empty() ? "" : out.info.c_str() );

	return true;
}

bool BuildStrandsFromHairFile(
		const HairFileData&						data,
		const double							widthRootScale,
		const double							widthTipScale,
		std::vector<HairGeometry::StrandDesc>&	out,
		const char*								who )
{
	out.clear();

	const char* label = ( who && who[0] ) ? who : "(unnamed)";

	if( !RISE::IsFiniteDouble( widthRootScale ) || !RISE::IsFiniteDouble( widthTipScale ) ||
	    !( widthRootScale > 0 ) || !( widthTipScale > 0 ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"HairFileLoader:: `%s`: width_root / width_tip are MULTIPLIERS on the file's own thickness in file "
			"mode and must be finite and > 0 (got %g / %g)",
			label, widthRootScale, widthTipScale );
		return false;
	}
	if( data.numStrands == 0 || data.pointsPerStrand.size() != data.numStrands ||
	    data.points.size() != (size_t)data.numPoints * 3 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"HairFileLoader:: `%s`: nothing to convert -- the parsed file is empty or internally inconsistent",
			label );
		return false;
	}

	// The width the file supplies when it has no per-point thickness
	// array.  A .hair file is not obliged to carry a usable thickness at
	// all (the format has no positivity requirement and plenty of real
	// files leave the default at 0), and a zero width would reject every
	// single strand downstream.  Fall back to RISE's own human-hair
	// defaults so the scale factors still have something to multiply --
	// loudly, because a groom silently rendered at the wrong gauge is
	// the kind of thing an author notices only much later.
	const double kFallbackRootWidth = 0.0001;	// same numbers as hair_geometry's width_root / width_tip
	const double kFallbackTipWidth  = 0.00003;
	double defaultRoot = (double)data.defaultThickness;
	double defaultTip  = (double)data.defaultThickness;
	if( !data.hasThicknessArray && !( defaultRoot > 0 ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"HairFileLoader:: `%s`: the file carries no thickness array and its header default thickness is %g "
			"(not > 0), so there is no width in the file at all -- falling back to RISE's human-hair defaults "
			"(%g root / %g tip scene units), which `width_root` / `width_tip` then scale",
			label, (double)data.defaultThickness, kFallbackRootWidth, kFallbackTipWidth );
		defaultRoot = kFallbackRootWidth;
		defaultTip  = kFallbackTipWidth;
	}

	out.reserve( data.numStrands );

	unsigned int rejectedNonFinitePoint = 0;
	unsigned int rejectedWidth          = 0;
	size_t       cursor                 = 0;	// index of the strand's first POINT

	for( unsigned int s = 0; s < data.numStrands; ++s )
	{
		const unsigned int nCP = data.pointsPerStrand[s];

		// LoadHairFile guarantees the counts sum to numPoints and that
		// each is >= 2, but this function is public and takes a plain
		// struct -- so it re-checks rather than trusting a caller-built
		// one to be well formed.
		if( nCP < 2 || cursor + nCP > (size_t)data.numPoints ) {
			GlobalLog()->PrintEx( eLog_Error,
				"HairFileLoader:: `%s`: strand %u has an out-of-range point range (%u points at offset %u of %u) "
				"-- the parsed file is internally inconsistent",
				label, s, nCP, (unsigned)cursor, data.numPoints );
			out.clear();
			return false;
		}

		// Both rejection tests run BEFORE anything is appended, so a
		// dropped strand costs no allocation and leaves `out` untouched.
		bool allFinite = true;
		for( unsigned int k = 0; k < nCP && allFinite; ++k ) {
			const float* p = &data.points[ ( cursor + k ) * 3 ];
			allFinite = RISE::IsFiniteDouble( (double)p[0] ) &&
			            RISE::IsFiniteDouble( (double)p[1] ) &&
			            RISE::IsFiniteDouble( (double)p[2] );
		}
		if( !allFinite ) {
			++rejectedNonFinitePoint;
			cursor += nCP;
			continue;
		}

		// Root and tip thickness.  HairGeometry carries ONE root width
		// and ONE tip width per strand and interpolates linearly in
		// arc-length fraction, so the file's interior thickness values
		// have nowhere to go (see the header).
		double rootW = defaultRoot;
		double tipW  = defaultTip;
		if( data.hasThicknessArray && data.thickness.size() == (size_t)data.numPoints ) {
			rootW = (double)data.thickness[ cursor ];
			tipW  = (double)data.thickness[ cursor + nCP - 1 ];
		}
		rootW *= widthRootScale;
		tipW  *= widthTipScale;

		if( !RISE::IsFiniteDouble( rootW ) || !RISE::IsFiniteDouble( tipW ) ||
		    !( rootW > 0 ) || !( tipW > 0 ) ) {
			++rejectedWidth;
			cursor += nCP;
			continue;
		}

		out.push_back( HairGeometry::StrandDesc() );
		HairGeometry::StrandDesc& sd = out.back();
		sd.rootWidth = rootW;
		sd.tipWidth  = tipW;
		// The format has no per-strand surface parameterization; see the
		// header for what a (0,0) root UV costs a UV-driven material.
		sd.rootUV    = Point2( 0, 0 );
		sd.controlPoints.resize( nCP );
		for( unsigned int k = 0; k < nCP; ++k ) {
			const float* p = &data.points[ ( cursor + k ) * 3 ];
			sd.controlPoints[k] = Point3( (double)p[0], (double)p[1], (double)p[2] );
		}

		cursor += nCP;
	}

	if( rejectedNonFinitePoint || rejectedWidth ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"HairFileLoader:: `%s`: dropped %u of %u strands (%u with a non-finite control point, %u whose "
			"root/tip width did not resolve finite and > 0)",
			label, rejectedNonFinitePoint + rejectedWidth, data.numStrands,
			rejectedNonFinitePoint, rejectedWidth );
	}

	if( out.empty() ) {
		GlobalLog()->PrintEx( eLog_Error,
			"HairFileLoader:: `%s`: every one of the %u strands was rejected -- the groom would be empty",
			label, data.numStrands );
		return false;
	}

	return true;
}

	}	// namespace Implementation
}	// namespace RISE
