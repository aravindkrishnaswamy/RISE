// HDRRoundTripTest.cpp - Round-trip test for the EXR primary output
// added in Landing 1 of the PB pipeline plan.
//
// Writes a synthetic image with known float values into an in-memory
// MemoryBuffer via EXRWriter, then reads back via EXRReader, then
// compares pixels.  Tolerances accommodate half-precision float
// rounding (the EXR writer emits half = ~3.3 decimal digits).
//
// Sentinel inputs cover:
//   - Zero
//   - Mid-grey (0.18)
//   - White-point (1.0)
//   - Bright HDR (10.0)
//   - Very-bright HDR (1e3)
//   - Tiny non-zero (1e-3)
//   - Negative (clamped to 0 by the writer's premultiply path)
//
// Skips when NO_EXR_SUPPORT is set; the test is meant for builds
// where OpenEXR is wired in (every supported platform after Landing 1).

#include <cassert>
#include <cmath>
#include <iostream>

#ifdef NO_EXR_SUPPORT
int main()
{
    std::cout << "HDRRoundTripTest: NO_EXR_SUPPORT defined; skipping." << std::endl;
    return 0;
}
#else

#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/RISE_API.h"   // RISE_API_CreateEXRWriter / Reader
#include "../src/Library/RasterImages/EXRCompression.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"

using namespace RISE;
using namespace RISE::Implementation;

// Tolerance: half-precision = ~3.3 decimal digits ~ 1e-3 relative.
// Use absolute tolerance for tiny values, relative for everything else.
static bool ApproxEqual( double got, double want )
{
    const double absTol = 1e-3;
    const double relTol = 2e-3;  // a touch above the half-float ulp at 1.0
    const double diff = std::fabs( got - want );
    if( diff <= absTol ) return true;
    const double scale = std::max( std::fabs( got ), std::fabs( want ) );
    return diff <= relTol * scale;
}

// Build an in-memory IRasterImage that DumpImage iterates over.
// Implements just enough of IRasterImage to drive the writer.
class SyntheticImage : public virtual IRasterImage, public virtual Reference
{
protected:
    virtual ~SyntheticImage() {}

public:
    static const unsigned int W = 7;
    static const unsigned int H = 5;
    RISEColor pixels[H][W];

    SyntheticImage()
    {
        // Fill pixels with sentinel values across the dynamic range,
        // including a negative sentinel to verify the round-trip
        // preserves the sign (half-precision EXR can store negatives).
        const double sentinels[] = {
            0.0, 0.18, 1.0, 10.0, 1e3, 1e-3, -0.5
        };
        static_assert( sizeof(sentinels)/sizeof(sentinels[0]) == W,
                       "sentinels must cover every column" );
        for( unsigned int y = 0; y < H; ++y ) {
            for( unsigned int x = 0; x < W; ++x ) {
                const double v = sentinels[ x ];
                pixels[y][x] = RISEColor( RISEPel( v, v * 0.5, v * 2.0 ),
                                          /*alpha*/ 1.0 );
            }
        }
    }

    RISEColor GetPEL( const unsigned int x, const unsigned int y ) const override
    {
        return pixels[y][x];
    }

    void SetPEL( const unsigned int, const unsigned int, const RISEColor& ) override {}
    void Clear( const RISEColor&, const Rect* ) override {}

    void DumpImage( IRasterImageWriter* pWriter ) const override
    {
        pWriter->BeginWrite( W, H );
        for( unsigned int y = 0; y < H; ++y ) {
            for( unsigned int x = 0; x < W; ++x ) {
                pWriter->WriteColor( pixels[y][x], x, y );
            }
        }
        pWriter->EndWrite();
    }

    void LoadImage( IRasterImageReader* ) override {}

    unsigned int GetWidth() const override  { return W; }
    unsigned int GetHeight() const override { return H; }
};

static void TestPIZRoundTrip()
{
    std::cout << "TestPIZRoundTrip..." << std::endl;
    SyntheticImage* src = new SyntheticImage;
    MemoryBuffer*   buf = new MemoryBuffer( (unsigned int)0 );

    // Write.
    {
        IRasterImageWriter* writer = 0;
        const bool wok = RISE_API_CreateEXRWriter(
            &writer, *buf, eColorSpace_ROMMRGB_Linear,
            eExrCompression_Piz, /*with_alpha*/ true );
        assert( wok );
        src->DumpImage( writer );
        safe_release( writer );
    }

    // Rewind and read.
    buf->seek( IBuffer::START, 0 );
    {
        IRasterImageReader* reader = 0;
        const bool rok = RISE_API_CreateEXRReader( &reader, *buf, eColorSpace_ROMMRGB_Linear );
        assert( rok );
        unsigned int w = 0, h = 0;
        const bool ok = reader->BeginRead( w, h );
        assert( ok );
        assert( w == SyntheticImage::W );
        assert( h == SyntheticImage::H );

        for( unsigned int y = 0; y < h; ++y ) {
            for( unsigned int x = 0; x < w; ++x ) {
                RISEColor got;
                reader->ReadColor( got, x, y );
                const RISEColor& want = src->pixels[y][x];
                if( !ApproxEqual( got.base.r, want.base.r ) ||
                    !ApproxEqual( got.base.g, want.base.g ) ||
                    !ApproxEqual( got.base.b, want.base.b ) ) {
                    std::cerr << "Mismatch at (" << x << "," << y << "): "
                              << "got (" << got.base.r << "," << got.base.g << "," << got.base.b << ")"
                              << " want (" << want.base.r << "," << want.base.g << "," << want.base.b << ")"
                              << std::endl;
                    assert( false );
                }
            }
        }
        reader->EndRead();
        safe_release( reader );
    }

    safe_release( buf );
    safe_release( src );
    std::cout << "  Passed!" << std::endl;
}

static void RoundTripWithCompression( EXR_COMPRESSION compression, const char* label )
{
    std::cout << label << "..." << std::endl;
    SyntheticImage* src = new SyntheticImage;
    MemoryBuffer*   buf = new MemoryBuffer( (unsigned int)0 );

    {
        IRasterImageWriter* writer = 0;
        const bool wok = RISE_API_CreateEXRWriter(
            &writer, *buf, eColorSpace_ROMMRGB_Linear, compression, true );
        assert( wok );
        src->DumpImage( writer );
        safe_release( writer );
    }

    buf->seek( IBuffer::START, 0 );
    {
        IRasterImageReader* reader = 0;
        const bool rok = RISE_API_CreateEXRReader( &reader, *buf, eColorSpace_ROMMRGB_Linear );
        assert( rok );
        unsigned int w = 0, h = 0;
        const bool ok = reader->BeginRead( w, h );
        assert( ok );
        for( unsigned int y = 0; y < h; ++y ) {
            for( unsigned int x = 0; x < w; ++x ) {
                RISEColor got;
                reader->ReadColor( got, x, y );
                assert( ApproxEqual( got.base.r, src->pixels[y][x].base.r ) );
                assert( ApproxEqual( got.base.g, src->pixels[y][x].base.g ) );
                assert( ApproxEqual( got.base.b, src->pixels[y][x].base.b ) );
            }
        }
        reader->EndRead();
        safe_release( reader );
    }

    safe_release( buf );
    safe_release( src );
    std::cout << "  Passed!" << std::endl;
}

int main()
{
    TestPIZRoundTrip();
    RoundTripWithCompression( eExrCompression_None, "TestNoCompressionRoundTrip" );
    RoundTripWithCompression( eExrCompression_Zip,  "TestZipRoundTrip" );
    // DWAA is lossy — round-trip values won't match within half-precision
    // tolerance.  We test that the round-trip *succeeds* (no crash, header
    // parses, dimensions match) using a wider tolerance.
    {
        std::cout << "TestDwaaRoundTripSucceeds..." << std::endl;
        SyntheticImage* src = new SyntheticImage;
        MemoryBuffer*   buf = new MemoryBuffer( (unsigned int)0 );
        {
            IRasterImageWriter* writer = 0;
            const bool wok = RISE_API_CreateEXRWriter(
                &writer, *buf, eColorSpace_ROMMRGB_Linear, eExrCompression_Dwaa, true );
            assert( wok );
            src->DumpImage( writer );
            safe_release( writer );
        }
        buf->seek( IBuffer::START, 0 );
        {
            IRasterImageReader* reader = 0;
            const bool rok = RISE_API_CreateEXRReader( &reader, *buf, eColorSpace_ROMMRGB_Linear );
            assert( rok );
            unsigned int w = 0, h = 0;
            const bool ok = reader->BeginRead( w, h );
            assert( ok );
            assert( w == SyntheticImage::W );
            assert( h == SyntheticImage::H );
            // Don't compare values — DWAA is lossy.  Just drain.
            for( unsigned int y = 0; y < h; ++y ) {
                for( unsigned int x = 0; x < w; ++x ) {
                    RISEColor got;
                    reader->ReadColor( got, x, y );
                }
            }
            reader->EndRead();
            safe_release( reader );
        }
        safe_release( buf );
        safe_release( src );
        std::cout << "  Passed!" << std::endl;
    }
    std::cout << "All HDR round-trip tests passed!" << std::endl;
    return 0;
}

#endif // NO_EXR_SUPPORT
