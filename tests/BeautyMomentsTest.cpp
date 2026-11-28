// Deterministic beauty moment contracts; no scene/image renders.
#include "../src/Library/Rendering/BeautyMoments.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Rendering/ProgressiveFilm.h"
#include "BeautyMomentOracles.h"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace RISE::Implementation;
namespace {
unsigned int passed = 0, failed = 0;
void Check(bool ok, const char* message)
{
    std::printf("%s: beauty-moments %s\n",ok ? "PASS" : "FAIL",message);
    if(ok) ++passed; else ++failed;
}
bool Close(double actual, double expected, double relative = 2e-14)
{
    return std::isfinite(actual) && std::fabs(actual-expected) <= std::fabs(expected)*relative;
}
bool Same(const BeautyMoments& a, const BeautyMoments& b)
{
    return a.mean == b.mean && a.meanCorrection == b.meanCorrection &&
        a.m2 == b.m2 && a.m2Correction == b.m2Correction && a.count == b.count && a.valid == b.valid;
}
void Adjacent()
{
    const double u = 0x1p-52;
    BeautyMoments m;
    m.Add(1+u); m.Add(1+2*u);
    double error = 0;
    Check(m.valid && m.count == 2 && m.m2 == 0x1p-105 &&
        m.mean == 1+2*u && m.meanCorrection == -0x1p-53,
        "adjacent finite samples retain exact positive M2 and half-ULP mean");
    Check(m.StandardError(error) && error == 0x1p-53,
        "adjacent finite samples retain nonzero standard error");
}
void ExactOracles()
{
    for(const auto& expected : beautyOracles) {
        BeautyMoments m;
        // n identical ones have exactly this state without spending billions
        // of test iterations. Inputs and oracle use exact sum/squared sums.
        m.mean = 1; m.count = expected.n;
        for(unsigned int i=0;i<expected.k;++i) m.Add(expected.x);
        double error = 0;
        Check(m.valid && m.count == expected.n+expected.k && m.mean == expected.mean &&
            Close(m.meanCorrection,expected.meanLow,1e-11) && Close(m.m2,expected.m2) &&
            m.StandardError(error) && Close(error,expected.se),
            "100k sub-ULP updates and near-uint32 count match exact independent moments");
    }
}
void SignedAndZeros()
{
    BeautyMoments exactSigned;
    exactSigned.Add(2); exactSigned.Add(-2);
    Check(exactSigned.valid && exactSigned.mean == 0 && exactSigned.meanCorrection == 0 && exactSigned.m2 == 8,
        "exact signed cancellation permits zero mean with positive M2");
    BeautyMoments signedMean;
    for(unsigned int i=0;i<64;++i) signedMean.Add(i%2 ? -2.0 : 2.0);
    double error = 0;
    Check(signedMean.valid && std::fabs(signedMean.mean+signedMean.meanCorrection) < 1e-12 && signedMean.m2 == 256 &&
        signedMean.StandardError(error) && Close(error,2.0/std::sqrt(63.0)) &&
        !signedMean.IsConverged(0.01,1e-12),
        "signed mean zero retains positive variance and cannot use black shortcut");
    BeautyMoments zeros;
    for(unsigned int i=0;i<63;++i) zeros.Add(0);
    Check(zeros.valid && !zeros.IsConverged(0.01,1e-12),"zero trials count and preserve 64-trial black minimum");
    zeros.Add(-0.0);
    Check(zeros.count == 64 && zeros.IsConverged(0.01,1e-12),"zero trials allow original black convergence policy");
    BeautyMoments misses;
    misses.Add(2); misses.Add(0);
    Check(misses.count == 2 && misses.mean == 1 && misses.m2 == 2,
        "misses and camera rejection zeros remain real trials");
    BeautyMoments negative;
    for(unsigned int i=0;i<32;++i) negative.Add(-3);
    Check(negative.valid && negative.IsConverged(0.01,1e-12),"finite negative constant luminance remains supported");
}
void RangeAndPolicy()
{
    BeautyMoments huge;
    huge.Add(std::numeric_limits<double>::max()); huge.Add(std::numeric_limits<double>::max());
    Check(huge.valid && huge.mean == std::numeric_limits<double>::max() && huge.m2 == 0,
        "first and repeated DBL_MAX observation do not square absolute luminance");
    huge.Add(-std::numeric_limits<double>::max());
    Check(!huge.valid && !huge.IsConverged(1,1e-12),"unrepresentable signed range disables convergence");
    BeautyMoments tiny;
    for(unsigned int i=0;i<64;++i) tiny.Add(0);
    tiny.Add(std::numeric_limits<double>::denorm_min());
    Check(!tiny.valid && !tiny.IsConverged(1,1e-12),"positive M2 range underflow blocks black heuristic");
    const BeautyMoments invalid = tiny;
    tiny.Add(0);
    Check(Same(tiny,invalid),"range failure remains sticky across later trials");
    BeautyMoments overflow;
    overflow.count = std::numeric_limits<unsigned int>::max();
    overflow.Add(0);
    Check(!overflow.valid && overflow.count == std::numeric_limits<unsigned int>::max(),"count guard disables convergence before uint32 wrap");
    BeautyMoments nonfinite;
    for(unsigned int i=0;i<64;++i) nonfinite.Add(0);
    nonfinite.Add(std::numeric_limits<double>::infinity());
    Check(!nonfinite.valid && !nonfinite.IsConverged(1,1e-12),"nonfinite luminance cannot drop a trial and converge black");
    BeautyMoments corrupt;
    corrupt.count = 64; corrupt.m2 = -1;
    Check(!corrupt.IsConverged(1,1e-12),"invalid negative M2 is validated before black shortcut");
    corrupt.m2 = std::numeric_limits<double>::infinity();
    Check(!corrupt.IsConverged(1,1e-12),"invalid infinite M2 is validated before black shortcut");
    BeautyMoments subnormal;
    subnormal.count = std::numeric_limits<unsigned int>::max();
    subnormal.m2 = std::numeric_limits<double>::denorm_min();
    double error = 0;
    Check(subnormal.StandardError(error) && error > 0 && std::isfinite(error),
        "square-root-before-division retains SE when sequential variance division underflows");
    BeautyMoments boundary;
    boundary.mean = 1; boundary.count = 32; boundary.m2 = 0.01;
    boundary.StandardError(error);
    const double threshold = error/(1-4.0/32);
    Check(!boundary.IsConverged(threshold*0.99,1e-12) && boundary.IsConverged(threshold*1.01,1e-12),
        "relative threshold and confidence factor retain original policy");
}
void Progressive()
{
    ProgressiveFilm film(1,1);
    BeautyMoments uninterrupted;
    for(unsigned int i=0;i<1001;++i) {
        const double x = i%3 ? 1+0x1p-52 : -2;
        uninterrupted.Add(x);
        BeautyMoments pass = film.Get(0,0).beautyMoments;
        pass.Add(x);
        film.Get(0,0).beautyMoments = pass;
    }
    Check(Same(uninterrupted,film.Get(0,0).beautyMoments),
        "progressive load/store preserves exact compensated state across 1001 passes");
    ProgressiveFilm copied = film;
    Check(Same(copied.Get(0,0).beautyMoments,uninterrupted),"progressive film copy carries both residuals and validity");
    film.Get(0,0).beautyMoments.valid = false;
    ProgressiveFilm invalid = film;
    Check(!invalid.Get(0,0).beautyMoments.valid,"progressive copy retains sticky invalid state");
    film.Get(0,0).colorSum = RISE::XYZPel(2,3,4);
    film.Get(0,0).sampleIndex = 99;
    film.Get(0,0).converged = true;
    film.Clear();
    Check(Same(film.Get(0,0).beautyMoments,BeautyMoments()) &&
        film.Get(0,0).sampleIndex == 0 && !film.Get(0,0).converged && film.Get(0,0).colorSum.Y == 0,
        "progressive clear resets corrections validity count and color state");
    std::printf("sizeof BeautyMoments=%zu ProgressivePixel=%zu\n",sizeof(BeautyMoments),sizeof(ProgressivePixel));
}
}
int main()
{
    Adjacent(); ExactOracles(); SignedAndZeros(); RangeAndPolicy(); Progressive();
    std::printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
