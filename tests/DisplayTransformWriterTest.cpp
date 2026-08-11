// DisplayTransformWriterTest.cpp - Unit test for the writer wrapper.
// Verifies that the complete output ViewTransform is applied between
// source IRasterImage iteration and the inner writer receiving the pixel.
//
// Uses a recording mock writer that captures every WriteColor call.
// We then check the captured Rec.709-linear colours against the complete
// exposure, white-balance, target-primaries, and curve pipeline.

#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Rendering/DisplayTransformWriter.h"
#include "../src/Library/Rendering/DisplayTransform.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int gFailCount = 0;

static bool Check( const bool condition, const char* label )
{
    if( !condition ) {
        ++gFailCount;
        std::cerr << "FAIL: " << label << std::endl;
    }
    return condition;
}

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

    Check( mock->beginCalls == 1, "BeginWrite forwards exactly once" );
    Check( mock->beginWidth == 17, "BeginWrite forwards width" );
    Check( mock->beginHeight == 23, "BeginWrite forwards height" );
    Check( mock->endCalls == 1, "EndWrite forwards exactly once" );

    safe_release( dt );    // releases mock to refcount 1
    safe_release( mock );  // refcount 0 -> dtor
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

    if( Check( mock->writes.size() == 1, "identity emits one pixel" ) ) {
        const auto& w = mock->writes[0];
        Check( w.x == 4 && w.y == 5, "identity preserves coordinates" );
        Check( IsClose( w.c.base.r, 0.25 ), "identity preserves red" );
        Check( IsClose( w.c.base.g, 0.5 ), "identity preserves green" );
        Check( IsClose( w.c.base.b, 1.5 ), "identity preserves blue" );
        // Alpha must pass through unchanged (it's coverage, not radiance).
        Check( IsClose( w.c.a, 0.7 ), "identity preserves alpha" );
    }

    safe_release( dt );
    safe_release( mock );
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
    if( Check( mock->writes.size() == 1, "positive exposure emits one pixel" ) ) {
        Check( IsClose( mock->writes[0].c.base.r, 0.2 ), "EV +1 scales red" );
        Check( IsClose( mock->writes[0].c.base.g, 0.4 ), "EV +1 scales green" );
        Check( IsClose( mock->writes[0].c.base.b, 0.6 ), "EV +1 scales blue" );
    }

    safe_release( dt );
    safe_release( mock );

    // EV -2 = 0.25x scaling.
    mock = new RecordingWriter;
    dt = new DisplayTransformWriter( *mock, -2.0, eDisplayTransform_None );
    dt->WriteColor( RISEColor( RISEPel( 1.0, 4.0, 8.0 ), 1.0 ), 0, 0 );
    if( Check( mock->writes.size() == 1, "negative exposure emits one pixel" ) ) {
        Check( IsClose( mock->writes[0].c.base.r, 0.25 ), "EV -2 scales red" );
        Check( IsClose( mock->writes[0].c.base.g, 1.0 ), "EV -2 scales green" );
        Check( IsClose( mock->writes[0].c.base.b, 2.0 ), "EV -2 scales blue" );
    }

    safe_release( dt );
    safe_release( mock );
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
    if( Check( mock->writes.size() == 1, "exposure-plus-curve emits one pixel" ) ) {
        Check( IsClose( mock->writes[0].c.base.r, 0.5 ),
            "Reinhard receives exposed red" );
        Check( IsClose( mock->writes[0].c.base.g, 2.0/3.0 ),
            "Reinhard receives exposed green" );
        Check( IsClose( mock->writes[0].c.base.b, 0.0 ),
            "Reinhard preserves black" );
    }

    safe_release( dt );
    safe_release( mock );
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
        if( Check( mock->writes.size() == i+1u,
            "ACES emits one pixel per input" ) ) {
            Check( IsClose( mock->writes[i].c.base.r, expected ),
                "ACES wrapper matches direct red" );
            Check( IsClose( mock->writes[i].c.base.g, expected ),
                "ACES wrapper matches direct green" );
            Check( IsClose( mock->writes[i].c.base.b, expected ),
                "ACES wrapper matches direct blue" );
        }
    }

    safe_release( dt );
    safe_release( mock );
}

// ---- Reference counting holds inner writer alive ----

static void TestRefcountHoldsInner()
{
    std::cout << "TestRefcountHoldsInner..." << std::endl;
    RecordingWriter* mock = new RecordingWriter;
    // Initial refcount 1 (from new).
    Check( mock->refcount() == 1, "mock starts with one reference" );

    DisplayTransformWriter* dt = new DisplayTransformWriter(
        *mock, 0.0, eDisplayTransform_None );
    // Wrapper ctor must have addref'd mock.
    Check( mock->refcount() == 2, "wrapper retains inner writer" );

    // Releasing the wrapper must drop mock back to 1.
    safe_release( dt );
    Check( mock->refcount() == 1, "wrapper releases inner writer" );

    safe_release( mock );
}

static void TestCompleteViewTransformMatchesSharedPipeline()
{
    std::cout << "TestCompleteViewTransformMatchesSharedPipeline..." << std::endl;
    const float strengths[] = { 0.0f, 0.25f, 1.0f };
    for( const float strength : strengths ) {
        FrameStoreOutput::ViewTransform transform;
        transform.exposureEV = 0.5f;
        transform.whiteBalance._00 = 1.10;
        transform.whiteBalance._01 = 0.05;
        transform.whiteBalance._02 = 0.00;
        transform.whiteBalance._10 = 0.00;
        transform.whiteBalance._11 = 0.90;
        transform.whiteBalance._12 = 0.05;
        transform.whiteBalance._20 = 0.02;
        transform.whiteBalance._21 = 0.00;
        transform.whiteBalance._22 = 1.08;
        transform.toneCurve = eDisplayTransform_Reinhard;
        transform.toneCurveStrength = strength;

        RecordingWriter* mock = new RecordingWriter;
        DisplayTransformWriter* writer = new DisplayTransformWriter(
            *mock,transform,eColorSpace_sRGB );
        const RISEPel input(0.20,0.45,0.70);
        writer->WriteColor(RISEColor(input,0.6),0,0);

        double expectedR = 0.0;
        double expectedG = 0.0;
        double expectedB = 0.0;
        FrameStoreOutput::ApplyViewTransformLinear(
            transform,FrameStoreOutput::FSColorSpace::sRGB_Linear,true,
            input.r,input.g,input.b,expectedR,expectedG,expectedB);
        if( Check(mock->writes.size() == 1u,
            "complete transform emits one pixel") ) {
            Check(IsClose(mock->writes[0].c.base.r,expectedR),
                "complete transform matches shared red");
            Check(IsClose(mock->writes[0].c.base.g,expectedG),
                "complete transform matches shared green");
            Check(IsClose(mock->writes[0].c.base.b,expectedB),
                "complete transform matches shared blue");
            Check(IsClose(mock->writes[0].c.a,0.6),
                "complete transform preserves alpha");
        }

        safe_release(writer);
        safe_release(mock);
    }
}

int main()
{
    TestBeginEndPassThrough();
    TestIdentity();
    TestExposureScaling();
    TestExposureThenCurve();
    TestACESThroughWrapper();
    TestRefcountHoldsInner();
    TestCompleteViewTransformMatchesSharedPipeline();
    if( gFailCount == 0 ) {
        std::cout << "All DisplayTransformWriter tests passed!" << std::endl;
    } else {
        std::cerr << gFailCount << " DisplayTransformWriter checks failed" << std::endl;
    }
    return gFailCount == 0 ? 0 : 1;
}
