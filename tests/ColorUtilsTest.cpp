#include <iostream>
#include <cassert>
#include <cmath>
#include <limits>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/PixelBasedSpectralIntegratingRasterizer.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

using namespace RISE;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-6) {
    return std::fabs(a - b) < epsilon;
}

// ==================== sRGB Transfer Function Tests ====================

void TestSRGBTransferFunction() {
    std::cout << "Testing SRGBTransferFunction..." << std::endl;

    // f(0) = 0
    assert(IsClose(ColorUtils::SRGBTransferFunction(0.0), 0.0));

    // f(1) should be close to 1
    assert(IsClose(ColorUtils::SRGBTransferFunction(1.0), 1.0, 1e-3));

    // Monotonicity: f is non-decreasing
    Scalar prev = 0;
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar y = ColorUtils::SRGBTransferFunction(x);
        assert(y >= prev - 1e-10);
        prev = y;
    }

    // Output stays in [0, 1] for inputs in [0, 1]
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar y = ColorUtils::SRGBTransferFunction(x);
        assert(y >= -1e-10 && y <= 1.0 + 1e-10);
    }

    // Linear segment: for very small values, f(x) ~ 12.92*x
    {
        Scalar x = 0.001;
        Scalar y = ColorUtils::SRGBTransferFunction(x);
        assert(IsClose(y, 12.92 * x, 1e-6));
    }

    // Transition point smoothness: values near the threshold should be close
    {
        Scalar threshold = 0.003040247678;
        Scalar below = ColorUtils::SRGBTransferFunction(threshold - 1e-7);
        Scalar above = ColorUtils::SRGBTransferFunction(threshold + 1e-7);
        assert(IsClose(below, above, 1e-4));
    }

    std::cout << "SRGBTransferFunction Passed!" << std::endl;
}

void TestSRGBTransferFunctionInverse() {
    std::cout << "Testing SRGBTransferFunctionInverse..." << std::endl;

    // Inverse(0) = 0
    assert(IsClose(ColorUtils::SRGBTransferFunctionInverse(0.0), 0.0));

    // Round-trip: Inverse(Forward(x)) ~ x
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar roundtrip = ColorUtils::SRGBTransferFunctionInverse(
            ColorUtils::SRGBTransferFunction(x));
        assert(IsClose(roundtrip, x, 1e-4));
    }

    // Round-trip other direction: Forward(Inverse(x)) ~ x
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar roundtrip = ColorUtils::SRGBTransferFunction(
            ColorUtils::SRGBTransferFunctionInverse(x));
        assert(IsClose(roundtrip, x, 1e-4));
    }

    // Monotonicity
    Scalar prev = 0;
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar y = ColorUtils::SRGBTransferFunctionInverse(x);
        assert(y >= prev - 1e-10);
        prev = y;
    }

    std::cout << "SRGBTransferFunctionInverse Passed!" << std::endl;
}

// ==================== ROMM RGB Transfer Function Tests ====================

void TestROMMRGBTransferFunction() {
    std::cout << "Testing ROMMRGBTransferFunction..." << std::endl;

    // f(0) = 0
    assert(IsClose(ColorUtils::ROMMRGBTransferFunction(0.0), 0.0));

    // f(1) = 1 (since 1^(1/1.8) = 1)
    assert(IsClose(ColorUtils::ROMMRGBTransferFunction(1.0), 1.0));

    // Monotonicity
    Scalar prev = 0;
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar y = ColorUtils::ROMMRGBTransferFunction(x);
        assert(y >= prev - 1e-10);
        prev = y;
    }

    // Linear segment: small x maps to 16*x
    {
        Scalar x = 0.001;
        Scalar y = ColorUtils::ROMMRGBTransferFunction(x);
        assert(IsClose(y, 16.0 * x, 1e-6));
    }

    std::cout << "ROMMRGBTransferFunction Passed!" << std::endl;
}

void TestROMMRGBTransferFunctionInverse() {
    std::cout << "Testing ROMMRGBTransferFunctionInverse..." << std::endl;

    // Inverse(0) = 0
    assert(IsClose(ColorUtils::ROMMRGBTransferFunctionInverse(0.0), 0.0));

    // Round-trip: Inverse(Forward(x)) ~ x
    for (int i = 0; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar roundtrip = ColorUtils::ROMMRGBTransferFunctionInverse(
            ColorUtils::ROMMRGBTransferFunction(x));
        assert(IsClose(roundtrip, x, 1e-4));
    }

    // Round-trip other direction
    for (int i = 1; i <= 100; i++) {
        Scalar x = i / 100.0;
        Scalar roundtrip = ColorUtils::ROMMRGBTransferFunction(
            ColorUtils::ROMMRGBTransferFunctionInverse(x));
        assert(IsClose(roundtrip, x, 1e-4));
    }

    // Linear segment: small x maps to x/16
    {
        Scalar x = 0.01;
        Scalar y = ColorUtils::ROMMRGBTransferFunctionInverse(x);
        assert(IsClose(y, x / 16.0, 1e-6));
    }

    std::cout << "ROMMRGBTransferFunctionInverse Passed!" << std::endl;
}

// ==================== CIE SPD / XYZ Tests ====================

void TestXYZFromNM() {
    std::cout << "Testing XYZFromNM..." << std::endl;

    // Out of range: below 380nm
    {
        XYZPel p;
        bool result = ColorUtils::XYZFromNM(p, 300);
        assert(result == false);
    }

    // Out of range: above 780nm
    {
        XYZPel p;
        bool result = ColorUtils::XYZFromNM(p, 800);
        assert(result == false);
    }

    // At boundary: 380nm
    {
        XYZPel p;
        bool result = ColorUtils::XYZFromNM(p, 380);
        assert(result == true);
        // Values should be small but non-negative
        assert(p.X >= 0 && p.Y >= 0 && p.Z >= 0);
    }

    // At boundary: 780nm
    {
        XYZPel p;
        bool result = ColorUtils::XYZFromNM(p, 780);
        assert(result == true);
    }

    // Peak of Y (luminosity function) is near 555nm
    {
        XYZPel peak;
        ColorUtils::XYZFromNM(peak, 555);

        XYZPel lower;
        ColorUtils::XYZFromNM(lower, 450);

        XYZPel higher;
        ColorUtils::XYZFromNM(higher, 650);

        assert(peak.Y > lower.Y);
        assert(peak.Y > higher.Y);
    }

    // Monotonicity check near peak: Y at 550 < Y at 555
    {
        XYZPel a, b;
        ColorUtils::XYZFromNM(a, 500);
        ColorUtils::XYZFromNM(b, 555);
        assert(b.Y > a.Y);
    }

    // All XYZ components should be non-negative in visible range
    for (int nm = 380; nm <= 780; nm += 10) {
        XYZPel p;
        bool result = ColorUtils::XYZFromNM(p, nm);
        assert(result == true);
        assert(p.X >= -1e-10);
        assert(p.Y >= -1e-10);
        assert(p.Z >= -1e-10);
    }

    std::cout << "XYZFromNM Passed!" << std::endl;
}

void TestInterpCIE_SPDIndices() {
    std::cout << "Testing InterpCIE_SPDIndices..." << std::endl;

    unsigned int min_idx, max_idx;
    Scalar weight;

    // Exact wavelength: 380nm -> index 0, weight 0
    {
        bool result = ColorUtils::InterpCIE_SPDIndices(380, min_idx, max_idx, weight);
        assert(result == true);
        assert(min_idx == 0);
        assert(max_idx == 1);
        assert(IsClose(weight, 0.0));
    }

    // Exact wavelength: 385nm -> index 1, weight 0
    {
        bool result = ColorUtils::InterpCIE_SPDIndices(385, min_idx, max_idx, weight);
        assert(result == true);
        assert(min_idx == 1);
    }

    // Midpoint: 382.5nm -> weight 0.5
    {
        bool result = ColorUtils::InterpCIE_SPDIndices(382.5, min_idx, max_idx, weight);
        assert(result == true);
        assert(min_idx == 0);
        assert(max_idx == 1);
        assert(IsClose(weight, 0.5));
    }

    // Out of range
    {
        assert(ColorUtils::InterpCIE_SPDIndices(379, min_idx, max_idx, weight) == false);
        assert(ColorUtils::InterpCIE_SPDIndices(781, min_idx, max_idx, weight) == false);
    }

    std::cout << "InterpCIE_SPDIndices Passed!" << std::endl;
}

void TestApplySPDFunction() {
    std::cout << "Testing ApplySPDFunction..." << std::endl;

    // Simple table for testing
    Scalar table[3] = {1.0, 3.0, 7.0};

    // Weight 0: return min value
    assert(IsClose(ColorUtils::ApplySPDFunction(0, 1, 0.0, table), 1.0));

    // Weight 1: return max value
    assert(IsClose(ColorUtils::ApplySPDFunction(0, 1, 1.0, table), 3.0));

    // Weight 0.5: midpoint
    assert(IsClose(ColorUtils::ApplySPDFunction(0, 1, 0.5, table), 2.0));

    // Second interval
    assert(IsClose(ColorUtils::ApplySPDFunction(1, 2, 0.0, table), 3.0));
    assert(IsClose(ColorUtils::ApplySPDFunction(1, 2, 1.0, table), 7.0));
    assert(IsClose(ColorUtils::ApplySPDFunction(1, 2, 0.25, table), 4.0));

    std::cout << "ApplySPDFunction Passed!" << std::endl;
}

// Integral of the public piecewise-linear CMF, split independently at its
// five-nanometre knots. Partial boundary cells must contribute their area.
class SpectralScaleProbe : public RISE::Implementation::PixelBasedSpectralIntegratingRasterizer {
public:
    SpectralScaleProbe(IRayCaster* caster,Scalar a,Scalar b,bool hwss):
      Rasterizer(nullptr),PixelBasedRasterizerHelper(caster,nullptr),
      PixelBasedSpectralIntegratingRasterizer(caster,a,b,160,4,StabilityConfig(),false,hwss) {}
    Scalar Scale()const{return mYNormalization;}
};

int TestCIEPartialCellIntegral() {
    int failed=0,checks=0;
    const Scalar intervals[][2]={{550,551},{552,553},{552,558},{399,701},{400,700},{380,780},{370,382},{778,790},{780,790},{370,380},{550,550},{560,550}};
    for(const auto& interval:intervals){
        const Scalar lo=std::max(interval[0],Scalar(380)),hi=std::min(interval[1],Scalar(780));
        Scalar expected=0;
        for(int knot=380;knot<780;knot+=5){
            const Scalar a=std::max(lo,Scalar(knot)),b=std::min(hi,Scalar(knot+5));
            if(b>a){XYZPel left,right;ColorUtils::XYZFromNM(left,a);ColorUtils::XYZFromNM(right,b);expected+=(left.Y+right.Y)*((b-a)*.5);}
        }
        const Scalar got=ColorUtils::CIE_Y_Integral(interval[0],interval[1]);
        const bool ok=std::isfinite(got)&&std::fabs(got-expected)<=1e-12*std::fabs(expected);
        ++checks;if(!ok)++failed;
        std::cout<<(ok?"PASS ":"FAIL ")<<"CIE partial interval ["<<interval[0]<<","<<interval[1]<<"] got="<<got<<" expected="<<expected<<std::endl;
    }
    // The actual inherited rasterizer constructor must cache this integral.
    // An empty shader is sufficient: these controls construct but never render.
    auto* shader=new RISE::Implementation::StandardShader(std::vector<IShaderOp*>());
    IRayCaster* caster=nullptr;
    const bool created=RISE_API_CreateRayCaster(&caster,false,10,*shader,true);
    ++checks;if(!created||!caster)++failed;
    if(caster){
        const Scalar ranges[][3]={{550,551,.9955},{552,558,5.991},{400,700,0},{380,780,0}};
        for(const auto& r:ranges)for(bool hwss:{false,true}){
            SpectralScaleProbe probe(caster,r[0],r[1],hwss);
            Scalar area=r[2];
            if(area==0)for(int nm=int(r[0]);nm<int(r[1]);nm+=5){XYZPel a,b;ColorUtils::XYZFromNM(a,nm);ColorUtils::XYZFromNM(b,nm+5);area+=(a.Y+b.Y)*2.5;}
            const Scalar expected=(r[1]-r[0])/area;
            const bool ok=std::isfinite(probe.Scale())&&std::fabs(probe.Scale()-expected)<=1e-12*expected;
            ++checks;if(!ok)++failed;
            std::cout<<(ok?"PASS ":"FAIL ")<<"spectral constructor ["<<r[0]<<","<<r[1]<<"] hwss="<<hwss<<" got="<<probe.Scale()<<" expected="<<expected<<std::endl;
        }
        caster->release();
    }
    shader->release();
    const Scalar split=ColorUtils::CIE_Y_Integral(552,554)+ColorUtils::CIE_Y_Integral(554,558);
    ++checks;if(std::fabs(split-5.991)>1e-12*5.991)++failed;
    for(Scalar invalid:{std::numeric_limits<Scalar>::infinity(),-std::numeric_limits<Scalar>::infinity(),std::numeric_limits<Scalar>::quiet_NaN()}){
        ++checks;if(ColorUtils::CIE_Y_Integral(invalid,700)!=0)++failed;
        ++checks;if(ColorUtils::CIE_Y_Integral(400,invalid)!=0)++failed;
    }
    std::cout<<"CIE partial-cell checks="<<checks<<" failures="<<failed<<std::endl;
    return failed;
}

int main() {
    TestSRGBTransferFunction();
    TestSRGBTransferFunctionInverse();
    TestROMMRGBTransferFunction();
    TestROMMRGBTransferFunctionInverse();
    TestXYZFromNM();
    TestInterpCIE_SPDIndices();
    TestApplySPDFunction();
    const int failures=TestCIEPartialCellIntegral();
    if(!failures)std::cout << "All ColorUtils tests passed!" << std::endl;
    return failures?1:0;
}
