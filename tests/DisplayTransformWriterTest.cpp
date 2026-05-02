// DisplayTransformWriterTest.cpp - Unit test for the writer wrapper
// added in Landing 1.  Verifies that exposure + tone curve are
// applied between source IRasterImage iteration and the inner writer
// receiving the pixel.
//
// Uses a recording mock writer that captures every WriteColor call.
// We then check the captured colours against expected (curve(input *
// 2^EV)) values.

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Rendering/DisplayTransformWriter.h"
#include "../src/Library/Rendering/DisplayTransform.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( double a, double b, double eps = 1e-6 )
{
    return std::fabs( a - b ) < eps;
}

// Mock writer that records every WriteColor call.  Provides
// reference-counting via Reference base.  Tracks BeginWrite /
// EndWrite to verify pass-through.
class RecordingWriter :
    public virtual IRasterImageWriter,
    public virtual Reference
{
public:
    struct Record { unsigned int x, y; RISEColor c; };

    unsigned int beginWidth = 0;
    unsigned int beginHeight = 0;
    int          beginCalls = 0;
    int          endCalls = 0;
    std::vector<Record> writes;

    void BeginWrite( const unsigned int width, const unsigned int height ) override
    {
        beginWidth = width;
        beginHeight = height;
        ++beginCalls;
    }

    void WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y ) override
    {
        Record r;
        r.x = x; r.y = y; r.c = c;
        writes.push_back( r );
    }

    void EndWrite() override
    {
        ++endCalls;
    }
};

// ---- BeginWrite / EndWrite pass through unchanged ----

static void TestBeginEndPassThrough()
{
    std::cout << "TestBeginEndPassThrough..." << std::endl;
    RecordingWriter* mock = new RecordingWriter;
    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, /*EV*/ 0.0, eDisplayTransform_None );

    dt->BeginWrite( 17, 23 );
    dt->EndWrite();

    assert( mock->beginCalls == 1 );
    assert( mock->beginWidth == 17 );
    assert( mock->beginHeight == 23 );
    assert( mock->endCalls == 1 );

    safe_release( dt );    // releases mock to refcount 1
    safe_release( mock );  // refcount 0 -> dtor
    std::cout << "  Passed!" << std::endl;
}

// ---- exposure = 0, transform = None: identity ----

static void TestIdentity()
{
    std::cout << "TestIdentity..." << std::endl;
    RecordingWriter* mock = new RecordingWriter;
    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, /*EV*/ 0.0, eDisplayTransform_None );

    RISEPel input( 0.25, 0.5, 1.5 );
    dt->WriteColor( RISEColor( input, /*alpha*/ 0.7 ), 4, 5 );

    assert( mock->writes.size() == 1 );
    const auto& w = mock->writes[0];
    assert( w.x == 4 && w.y == 5 );
    assert( IsClose( w.c.base.r, 0.25 ) );
    assert( IsClose( w.c.base.g, 0.5 ) );
    assert( IsClose( w.c.base.b, 1.5 ) );
    // Alpha must pass through unchanged (it's coverage, not radiance).
    assert( IsClose( w.c.a, 0.7 ) );

    safe_release( dt );
    safe_release( mock );
    std::cout << "  Passed!" << std::endl;
}

// ---- exposure scales linearly before tone curve ----

static void TestExposureScaling()
{
    std::cout << "TestExposureScaling..." << std::endl;
    // EV +1 = 2x scaling.  With None curve, result must be 2x input.
    RecordingWriter* mock = new RecordingWriter;
    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, /*EV*/ 1.0, eDisplayTransform_None );

    dt->WriteColor( RISEColor( RISEPel( 0.1, 0.2, 0.3 ), 1.0 ), 0, 0 );
    assert( IsClose( mock->writes[0].c.base.r, 0.2 ) );
    assert( IsClose( mock->writes[0].c.base.g, 0.4 ) );
    assert( IsClose( mock->writes[0].c.base.b, 0.6 ) );

    safe_release( dt );
    safe_release( mock );

    // EV -2 = 0.25x scaling.
    mock = new RecordingWriter;
    dt = new DisplayTransformWriter( *mock, -2.0, eDisplayTransform_None );
    dt->WriteColor( RISEColor( RISEPel( 1.0, 4.0, 8.0 ), 1.0 ), 0, 0 );
    assert( IsClose( mock->writes[0].c.base.r, 0.25 ) );
    assert( IsClose( mock->writes[0].c.base.g, 1.0 ) );
    assert( IsClose( mock->writes[0].c.base.b, 2.0 ) );

    safe_release( dt );
    safe_release( mock );
    std::cout << "  Passed!" << std::endl;
}

// ---- exposure THEN curve: ordering check ----

static void TestExposureThenCurve()
{
    std::cout << "TestExposureThenCurve..." << std::endl;
    // EV +1 doubles input; Reinhard then maps it.
    // input 0.5, EV +1 -> 1.0 -> Reinhard(1.0) = 0.5
    // input 1.0, EV +1 -> 2.0 -> Reinhard(2.0) = 2/3
    RecordingWriter* mock = new RecordingWriter;
    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, 1.0, eDisplayTransform_Reinhard );

    dt->WriteColor( RISEColor( RISEPel( 0.5, 1.0, 0.0 ), 1.0 ), 0, 0 );
    assert( IsClose( mock->writes[0].c.base.r, 0.5 ) );        // Reinhard(1.0)
    assert( IsClose( mock->writes[0].c.base.g, 2.0/3.0 ) );    // Reinhard(2.0)
    assert( IsClose( mock->writes[0].c.base.b, 0.0 ) );        // Reinhard(0)

    safe_release( dt );
    safe_release( mock );
    std::cout << "  Passed!" << std::endl;
}

// ---- ACES curve through wrapper matches direct call ----

static void TestACESThroughWrapper()
{
    std::cout << "TestACESThroughWrapper..." << std::endl;
    RecordingWriter* mock = new RecordingWriter;
    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, 0.0, eDisplayTransform_ACES );

    const double samples[] = { 0.18, 0.5, 1.0, 5.0 };
    for( size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i ) {
        const double x = samples[i];
        dt->WriteColor( RISEColor( RISEPel( x, x, x ), 1.0 ),
                        (unsigned)i, 0 );
        const double expected = DisplayTransforms::ACES( x );
        assert( IsClose( mock->writes[i].c.base.r, expected ) );
        assert( IsClose( mock->writes[i].c.base.g, expected ) );
        assert( IsClose( mock->writes[i].c.base.b, expected ) );
    }

    safe_release( dt );
    safe_release( mock );
    std::cout << "  Passed!" << std::endl;
}

// ---- Reference counting holds inner writer alive ----

static void TestRefcountHoldsInner()
{
    std::cout << "TestRefcountHoldsInner..." << std::endl;
    RecordingWriter* mock = new RecordingWriter;
    // Initial refcount 1 (from new).
    assert( mock->refcount() == 1 );

    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, 0.0, eDisplayTransform_None );
    // Wrapper ctor must have addref'd mock.
    assert( mock->refcount() == 2 );

    // Releasing the wrapper must drop mock back to 1.
    safe_release( dt );
    assert( mock->refcount() == 1 );

    safe_release( mock );
    std::cout << "  Passed!" << std::endl;
}

int main()
{
    TestBeginEndPassThrough();
    TestIdentity();
    TestExposureScaling();
    TestExposureThenCurve();
    TestACESThroughWrapper();
    TestRefcountHoldsInner();
    std::cout << "All DisplayTransformWriter tests passed!" << std::endl;
    return 0;
}
