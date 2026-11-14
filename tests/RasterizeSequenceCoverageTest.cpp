//////////////////////////////////////////////////////////////////////
//
//  RasterizeSequenceCoverageTest.cpp
//
//  Every IRasterizeSequence must, for Begin( startx, endx, starty, endy )
//  with INCLUSIVE ends (the BoundsFromRect convention every rasterizer
//  uses), return regions that are pairwise disjoint and whose union is
//  exactly [startx, endx] x [starty, endy], and NumRegions() must equal
//  the regions returned.  Instances are reused across ranges, as the
//  rasterizers reuse them, and every reused result must equal a fresh
//  instance's.
//
//  Why it matters: tile-preview composition (GetIntermediateOutputImage
//  with a region) may be called concurrently only for pairwise-disjoint
//  regions, and two regions sharing a row render that row twice with
//  concurrent writes to the same primary pixels.  Defects this locks
//  down:
//    - the scanline sequence returned Rect( y, x0, y+1, x1 ) -- two rows,
//      Rect being inclusive -- and a one-row range produced no regions;
//    - block and Hilbert sequences started at (0,0), rendering outside a
//      crop;
//    - the Hilbert walk indexed a num x num grid that the block split did
//      not always produce (out-of-bounds reads, then a logged fallback),
//      its depth was decremented permanently by Begin, and depth 0
//      recursed forever.  The Hilbert fallback is a failure here.
//
//  Also: a block-sequence crop keeps its boundaries on absolute multiples
//  of the block size (no two blocks share a 32-pixel FrameStore tile when
//  the block size is a multiple of 32), and the centre-out order centres
//  on the crop.
//
//  The sequences are constructed from their headers (not through
//  RISE_API), so compiling this file with -fsanitize=address,undefined
//  instruments all of the sequence code.
//
//  Standalone executable (no framework), matching the repo's test style.
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Rendering/HilbertRasterizeSequence.h"
#include "../src/Library/Rendering/BlockRasterizeSequence.h"
#include "../src/Library/Rendering/MortonRasterizeSequence.h"
#include "../src/Library/Rendering/ScanlineRasterizeSequence.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace RISE;

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool cond, const std::string& what )
{
	if( cond ) {
		g_pass++;
	} else {
		g_fail++;
		std::printf( "  FAIL: %s\n", what.c_str() );
	}
}

static std::vector<Rect> Regions( IRasterizeSequence& seq,
	unsigned int sx, unsigned int ex, unsigned int sy, unsigned int ey )
{
	seq.Begin( sx, ex, sy, ey );
	const int n = seq.NumRegions();
	std::vector<Rect> v;
	for( int i = 0; i < n; i++ ) {
		v.push_back( seq.GetNextRegion() );
	}
	seq.End();
	return v;
}

static bool SameList( const std::vector<Rect>& a, const std::vector<Rect>& b )
{
	if( a.size() != b.size() ) return false;
	for( size_t i = 0; i < a.size(); i++ ) {
		if( a[i].top != b[i].top || a[i].left != b[i].left || a[i].bottom != b[i].bottom || a[i].right != b[i].right ) {
			return false;
		}
	}
	return true;
}

// Returns true when the regions tile the range exactly.  Writes a short
// diagnostic into `why` otherwise.
static bool CoversExactly( const std::vector<Rect>& v,
	unsigned int sx, unsigned int ex, unsigned int sy, unsigned int ey,
	std::string& why )
{
	if( v.empty() ) {
		why = "no regions";
		return false;
	}

	// Coverage count over a frame large enough to hold any region the
	// sequence might (wrongly) return outside the range.
	const unsigned int W = ex + 2;
	const unsigned int H = ey + 2;
	std::vector<int> count( static_cast<size_t>( W ) * H, 0 );
	bool ok = true;

	for( const Rect& r : v ) {
		if( r.left > r.right || r.top > r.bottom ) {
			why = "inverted region";
			ok = false;
			continue;
		}
		if( r.right >= W || r.bottom >= H ) {
			why = "region far outside range";
			ok = false;
			continue;
		}
		for( unsigned int y = r.top; y <= r.bottom; y++ ) {
			for( unsigned int x = r.left; x <= r.right; x++ ) {
				count[ static_cast<size_t>( y ) * W + x ]++;
			}
		}
	}

	unsigned int dup = 0, missing = 0, outside = 0;
	for( unsigned int y = 0; y < H; y++ ) {
		for( unsigned int x = 0; x < W; x++ ) {
			const int c = count[ static_cast<size_t>( y ) * W + x ];
			const bool inside = x >= sx && x <= ex && y >= sy && y <= ey;
			if( inside ) {
				if( c == 0 ) missing++;
				if( c > 1 ) dup++;
			} else if( c != 0 ) {
				outside++;
			}
		}
	}
	if( dup || missing || outside ) {
		char buf[ 160 ];
		std::snprintf( buf, sizeof( buf ), "pixels: duplicated %u, missing %u, outside range %u", dup, missing, outside );
		why = buf;
		ok = false;
	}
	return ok;
}

struct Range { unsigned int sx, ex, sy, ey; };

typedef std::function<IRasterizeSequence*()> Factory;
typedef std::function<bool( IRasterizeSequence& )> FellBack;

//! Every range through ONE reused instance and through a fresh instance
//! each: both must cover exactly, never take a fallback, and (when the
//! order is deterministic) agree.
static void RunSequence( const char* name, const Factory& make, const std::vector<Range>& ranges,
	bool deterministic, const FellBack& fellBack = FellBack() )
{
	std::printf( "%s\n", name );
	IRasterizeSequence* reused = make();
	for( const Range& rg : ranges ) {
		IRasterizeSequence* fresh = make();
		const std::vector<Rect> a = Regions( *reused, rg.sx, rg.ex, rg.sy, rg.ey );
		const bool fbA = fellBack && fellBack( *reused );
		const std::vector<Rect> b = Regions( *fresh, rg.sx, rg.ex, rg.sy, rg.ey );
		const bool fbB = fellBack && fellBack( *fresh );
		fresh->release();

		std::string why;
		const bool ok = CoversExactly( a, rg.sx, rg.ex, rg.sy, rg.ey, why );
		char buf[ 256 ];
		std::snprintf( buf, sizeof( buf ), "%s x[%u,%u] y[%u,%u]: %s",
			name, rg.sx, rg.ex, rg.sy, rg.ey, ok ? "ok" : why.c_str() );
		Check( ok, buf );
		if( fellBack ) {
			std::snprintf( buf, sizeof( buf ), "%s x[%u,%u] y[%u,%u]: no Hilbert fallback (reused %d, fresh %d)",
				name, rg.sx, rg.ex, rg.sy, rg.ey, int( fbA ), int( fbB ) );
			Check( !fbA && !fbB, buf );
		}
		if( deterministic ) {
			std::snprintf( buf, sizeof( buf ), "%s x[%u,%u] y[%u,%u]: reused instance returns the fresh instance's list",
				name, rg.sx, rg.ex, rg.sy, rg.ey );
			Check( SameList( a, b ), buf );
		}
	}
	reused->release();
}

//! Crops of the block sequence: boundaries on absolute multiples of the
//! block size (so with a block size that is a multiple of 32 no two blocks
//! share a 32-pixel FrameStore tile), and the centre-out order (type 0)
//! starts at the crop's centre.
static void TestBlockCropAlignment()
{
	std::printf( "block crop alignment and centre order\n" );
	const unsigned int blocks[] = { 32, 64 };
	const Range crops[] = {
		{ 600, 699, 300, 359 }, { 5, 90, 17, 140 }, { 33, 33, 1, 200 }, { 1, 1000, 7, 13 }, { 31, 97, 31, 97 },
	};
	for( unsigned int b : blocks ) {
		for( const Range& c : crops ) {
			IRasterizeSequence* p = new Implementation::BlockRasterizeSequence( b, b, 0 );
			const std::vector<Rect> v = Regions( *p, c.sx, c.ex, c.sy, c.ey );
			p->release();

			// No two regions share a 32x32 absolute cell.
			unsigned int shared = 0;
			for( size_t i = 0; i < v.size(); i++ ) {
				for( size_t j = i + 1; j < v.size(); j++ ) {
					const bool xo = v[i].left / 32 <= v[j].right / 32 && v[j].left / 32 <= v[i].right / 32;
					const bool yo = v[i].top / 32 <= v[j].bottom / 32 && v[j].top / 32 <= v[i].bottom / 32;
					if( xo && yo ) shared++;
				}
			}
			char buf[ 200 ];
			std::snprintf( buf, sizeof( buf ), "block %u crop x[%u,%u] y[%u,%u]: no two blocks share a 32-pixel tile (%u pairs do)",
				b, c.sx, c.ex, c.sy, c.ey, shared );
			Check( shared == 0, buf );

			// The first block is the one nearest the crop centre: within one
			// block size of it.
			const long cx = long( c.sx + ( c.ex - c.sx ) / 2 ), cy = long( c.sy + ( c.ey - c.sy ) / 2 );
			const long fx = long( ( v[0].left + v[0].right ) / 2 ), fy = long( ( v[0].top + v[0].bottom ) / 2 );
			const long dx = fx > cx ? fx - cx : cx - fx, dy = fy > cy ? fy - cy : cy - fy;
			std::snprintf( buf, sizeof( buf ), "block %u crop x[%u,%u] y[%u,%u]: centre-out order starts at the crop centre (first block %ld,%ld px away)",
				b, c.sx, c.ex, c.sy, c.ey, dx, dy );
			Check( dx <= long( b ) && dy <= long( b ), buf );
		}
	}
}

int main()
{
	std::vector<Range> ranges;
	// Full frames (start 0) of assorted sizes, odd and even, including
	// one-row, one-column and one-pixel frames.
	const unsigned int sizes[] = { 1, 2, 3, 5, 7, 8, 9, 17, 31, 32, 33, 64, 97, 100, 101 };
	for( unsigned int w : sizes ) {
		for( unsigned int h : sizes ) {
			ranges.push_back( { 0, w - 1, 0, h - 1 } );
		}
	}
	// Configurations that broke the Hilbert walk.
	ranges.push_back( { 0, 100, 0, 99 } );		// 101x100
	ranges.push_back( { 0, 1919, 0, 1079 } );
	ranges.push_back( { 0, 1918, 0, 1079 } );
	// Crops (non-zero start), as BoundsFromRect produces for a region render.
	ranges.push_back( { 5, 5, 9, 9 } );
	ranges.push_back( { 3, 40, 7, 7 } );
	ranges.push_back( { 10, 74, 4, 52 } );
	ranges.push_back( { 1, 2, 1, 2 } );
	ranges.push_back( { 17, 99, 33, 34 } );
	ranges.push_back( { 600, 699, 300, 359 } );

	RunSequence( "scanline", []() {
		return static_cast<IRasterizeSequence*>( new Implementation::ScanlineRasterizeSequence() ); }, ranges, true );

	const unsigned int tiles[] = { 1, 8, 32 };
	for( unsigned int t : tiles ) {
		char name[ 64 ];
		std::snprintf( name, sizeof( name ), "morton tile %u", t );
		RunSequence( name, [t]() {
			return static_cast<IRasterizeSequence*>( new Implementation::MortonRasterizeSequence( t ) ); }, ranges, true );
	}

	const unsigned int bs[][2] = { { 1, 1 }, { 8, 8 }, { 64, 64 }, { 16, 5 }, { 0, 0 } };
	for( auto& b : bs ) {
		for( int order = 0; order <= 8; order++ ) {
			char name[ 64 ];
			std::snprintf( name, sizeof( name ), "block %ux%u order %d", b[0], b[1], order );
			const unsigned int bw = b[0], bh = b[1];
			RunSequence( name, [bw, bh, order]() {
				return static_cast<IRasterizeSequence*>( new Implementation::BlockRasterizeSequence( bw, bh, static_cast<char>( order ) ) ); },
				ranges, order != 1 );
		}
	}

	const auto hilbertFellBack = []( IRasterizeSequence& s ) {
#ifdef RSC_OLD_HILBERT_HEADER
		// Red-proof shim: older headers have no fallback accessor; their
		// fallback is still visible as an ASan out-of-bounds read and as
		// coverage failures.
		(void)s;
		return false;
#else
		const Implementation::HilbertRasterizeSequence* h = dynamic_cast<const Implementation::HilbertRasterizeSequence*>( &s );
		return !h || h->LastBeginFellBack();
#endif
	};
#ifdef RSC_SKIP_DEPTH0
	// Older headers recurse forever at depth 0.
	const unsigned int depths[] = { 1, 2, 3, 4, 5 };
#else
	const unsigned int depths[] = { 0, 1, 2, 3, 4, 5 };
#endif
	for( unsigned int d : depths ) {
		char name[ 64 ];
		std::snprintf( name, sizeof( name ), "hilbert depth %u", d );
		RunSequence( name, [d]() {
			return static_cast<IRasterizeSequence*>( new Implementation::HilbertRasterizeSequence( d ) ); },
			ranges, true, hilbertFellBack );
	}

	TestBlockCropAlignment();

	std::printf( "\nRasterizeSequenceCoverageTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
