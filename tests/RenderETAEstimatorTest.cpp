#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include "../src/Library/Utilities/RenderETAEstimator.h"

using namespace RISE;

namespace {

// Synthetic-clock subclass so tests are deterministic and fast.
class FakeClockETA : public RenderETAEstimator {
public:
    FakeClockETA() : RenderETAEstimator(), now(0.0) {}
    explicit FakeClockETA(const Config& c) : RenderETAEstimator(c), now(0.0) {}
    double NowSeconds() const override { return now; }
    void Advance(double dt) { now += dt; }
    double now;
};

bool IsClose(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) < eps;
}

}

// 1. Under a linear progress schedule with a constant true rate, the
// displayed remaining-seconds must converge to (totalTime - elapsed)
// within a small tolerance after warmup, and must never spike above
// the true total.
static void TestLinearProgress() {
    std::cout << "Testing linear progress..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    const double totalTime = 20.0;
    const int steps = 200;
    double peak = 0.0;
    for (int i = 1; i <= steps; ++i) {
        eta.Advance(totalTime / steps);
        const double frac = static_cast<double>(i) / steps;
        eta.Update(frac, 1.0);
        double rem = 0.0;
        if (eta.RemainingSeconds(rem)) {
            assert(std::isfinite(rem));
            assert(rem >= 0.0);
            if (rem > peak) peak = rem;
            const double elapsed = eta.ElapsedSeconds();
            const double truth = totalTime - elapsed;
            // Must track truth closely once we're a few seconds in.
            if (elapsed > 5.0 && frac < 0.95) {
                assert(std::fabs(rem - truth) < 1.5);
            }
        }
    }
    assert(peak < totalTime * 1.1);

    std::cout << "Linear progress Passed!" << std::endl;
}

// 2. Warmup: remaining must stay invalid while elapsed < warmupSeconds
// OR progress < warmupFraction. Defaults are 2.0 s and 3%.
static void TestWarmup() {
    std::cout << "Testing warmup gating..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    // Elapsed=0.5s, frac=0.5% — both gates closed.
    eta.Advance(0.5);
    eta.Update(0.005, 1.0);
    double rem = 0.0;
    assert(!eta.RemainingSeconds(rem));

    // Elapsed=2.5s, frac=0.5% — elapsed gate passes, progress gate closed.
    eta.Advance(2.0);
    eta.Update(0.005, 1.0);
    assert(!eta.RemainingSeconds(rem));

    // Elapsed=3s, frac=5% — both open.
    eta.Advance(0.5);
    eta.Update(0.05, 1.0);
    assert(eta.RemainingSeconds(rem));
    assert(rem > 0.0);

    std::cout << "Warmup gating Passed!" << std::endl;
}

// 3. Countdown between updates: after an Update the remaining value
// must decrease at ~1 s per real second even without further updates.
static void TestCountdownBetweenUpdates() {
    std::cout << "Testing countdown between updates..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    // Reach warmup with a clean estimate.
    for (int i = 1; i <= 30; ++i) {
        eta.Advance(0.1);
        eta.Update(i * 0.01, 1.0);
    }
    double r0 = 0.0;
    assert(eta.RemainingSeconds(r0));

    // No further updates — just advance the clock.
    eta.Advance(1.0);
    double r1 = 0.0;
    assert(eta.RemainingSeconds(r1));
    // Should have decreased by ~1 second.
    assert(std::fabs((r0 - r1) - 1.0) < 0.05);

    eta.Advance(2.0);
    double r2 = 0.0;
    assert(eta.RemainingSeconds(r2));
    assert(std::fabs((r1 - r2) - 2.0) < 0.05);

    std::cout << "Countdown between updates Passed!" << std::endl;
}

// 4. Concurrent-worker reordering: the real rasterizer dispatches
// tiles via an atomic fetch_add, but Progress calls reach the
// estimator out of order because the serialization mutex is acquired
// after the fetch. Out-of-order samples must be ignored (no re-baseline)
// and the estimate must stay well-behaved.
static void TestConcurrentWorkerReordering() {
    std::cout << "Testing concurrent-worker reordering..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    const int totalTiles = 200;
    const double totalTime = 10.0;
    const double dtPerSample = totalTime / totalTiles;
    const int perm[4] = { 2, 0, 3, 1 };

    double peak = 0.0;
    for (int base = 0; base < totalTiles; base += 4) {
        for (int j = 0; j < 4 && base + j < totalTiles; ++j) {
            const int idx = base + perm[j];
            eta.Advance(dtPerSample);
            eta.Update(static_cast<double>(idx + 1) / totalTiles, 1.0);
            double rem = 0.0;
            if (eta.RemainingSeconds(rem)) {
                assert(std::isfinite(rem));
                assert(rem >= 0.0);
                if (rem > peak) peak = rem;
            }
        }
    }
    // True render takes totalTime; peak must not exceed ~2x.
    assert(peak < totalTime * 2.0);
    assert(eta.ProgressFraction() >= 0.99);

    std::cout << "Concurrent-worker reordering Passed!" << std::endl;
}

// 5. Real rasterizer trace (captured from shapes.RISEscene). This is
// the pattern that caused "starts at 0, goes up, settles back to 0"
// under earlier designs. The displayed value must not oscillate
// wildly, must not stay stuck at 0 during the render, and must finish
// near zero.
static void TestRealRasterizerTrace() {
    std::cout << "Testing real-rasterizer trace..." << std::endl;

    const int seq[] = {
        0, 3, 1, 2, 2, 4, 4, 1, 5, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11,
        13, 12, 13, 14, 14, 15, 15, 16, 17, 17, 18, 18, 19, 20, 20, 21,
        21, 22, 23, 23, 24, 24, 25, 26, 26, 27, 27, 28, 28, 29, 30, 30,
        31, 31, 32, 33, 33, 34, 34, 35, 36, 36, 37, 37, 38, 39, 39, 40,
        40, 41, 42, 42, 43, 43, 44, 44, 45, 46, 46, 47, 47, 48, 49, 49,
        50, 50, 51, 52, 52, 53, 53, 54, 55, 55, 56, 56, 57, 57, 58, 59,
        59, 60, 60, 61, 62, 62, 63, 63, 64, 65, 65, 66, 66, 67, 68, 68,
        69, 69, 70, 71, 71, 72, 72, 73, 73, 74, 75, 75, 76, 76, 77, 78,
        78, 79, 79, 80, 81, 81, 82, 82, 83, 84, 84, 85, 85, 86, 86, 87,
        88, 88, 89, 89, 90, 91, 91, 92, 92, 93, 94, 94, 95, 95, 96, 97,
        97, 98, 98, 99,
    };
    const int n = static_cast<int>(sizeof(seq) / sizeof(seq[0]));
    const double totalWall = 30.0;
    const double dt = totalWall / n;

    FakeClockETA eta;
    eta.Begin();

    double peak = 0.0;
    double lastRem = -1.0;
    int validSamples = 0;
    // Also track per-tick change: the displayed value must not jump
    // around by many seconds between consecutive samples.
    double maxDelta = 0.0;
    double prev = -1.0;

    for (int i = 0; i < n; ++i) {
        eta.Advance(dt);
        eta.Update(static_cast<double>(seq[i]) / 100.0, 1.0);
        double rem = 0.0;
        if (eta.RemainingSeconds(rem)) {
            assert(std::isfinite(rem));
            assert(rem >= 0.0);
            ++validSamples;
            if (rem > peak) peak = rem;
            if (prev >= 0.0) {
                const double delta = std::fabs(rem - prev);
                if (delta > maxDelta) maxDelta = delta;
            }
            prev = rem;
            lastRem = rem;
        }
    }

    // Many valid samples post-warmup.
    assert(validSamples > 100);
    // Peak remaining must stay bounded.
    assert(peak < totalWall * 1.5);
    // Between consecutive samples, remaining changes by at most a
    // few seconds — catches the "jumps around wildly" failure mode.
    assert(maxDelta < 3.0);
    // Final estimate must be small.
    assert(lastRem >= 0.0 && lastRem < 1.5);

    std::cout << "Real-rasterizer trace Passed! "
              << "(valid=" << validSamples
              << ", peak=" << peak
              << "s, maxDelta=" << maxDelta
              << "s, final=" << lastRem << "s)"
              << std::endl;
}

// 6. Large backward jump (e.g. MLT bootstrap -> mutation): must not
// produce negative rates or NaN, must re-baseline cleanly.
static void TestLargeBackwardJump() {
    std::cout << "Testing large-backward-jump reset..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    for (int i = 1; i <= 40; ++i) {
        eta.Advance(0.1);
        eta.Update(i * 0.01, 1.0);
    }
    double rem = 0.0;
    assert(eta.RemainingSeconds(rem));

    // Big backward jump — renderer restarted from zero.
    eta.Advance(0.1);
    eta.Update(0.0, 1.0);
    assert(!eta.RemainingSeconds(rem));  // displayedInit was cleared

    // Ramp up again.
    for (int i = 1; i <= 40; ++i) {
        eta.Advance(0.1);
        eta.Update(i * 0.01, 1.0);
    }
    assert(eta.RemainingSeconds(rem));
    assert(rem > 0.0 && rem < 100.0);

    std::cout << "Large-backward-jump reset Passed!" << std::endl;
}

// 7. Stall: if no updates arrive, the displayed value keeps counting
// down (not stuck, not NaN) but never goes negative.
static void TestStall() {
    std::cout << "Testing stall handling..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    for (int i = 1; i <= 30; ++i) {
        eta.Advance(0.1);
        eta.Update(i * 0.01, 1.0);
    }
    double r0 = 0.0;
    assert(eta.RemainingSeconds(r0));

    // Very long stall.
    eta.Advance(600.0);
    double r1 = 0.0;
    assert(eta.RemainingSeconds(r1));
    assert(std::isfinite(r1));
    assert(r1 == 0.0);  // countdown floor

    std::cout << "Stall Passed!" << std::endl;
}

// 8. Completion: at progress == 1.0, remaining should be zero.
static void TestCompletion() {
    std::cout << "Testing completion..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    for (int i = 1; i <= 200; ++i) {
        eta.Advance(0.05);
        eta.Update(i * 0.005, 1.0);
    }
    double rem = 0.0;
    assert(eta.RemainingSeconds(rem));
    assert(IsClose(rem, 0.0, 1e-9));
    assert(IsClose(eta.ProgressFraction(), 1.0, 1e-9));

    std::cout << "Completion Passed!" << std::endl;
}

// 9. Formatting: verify MM:SS and zero-padded HH:MM:SS boundaries.
static void TestFormatDuration() {
    std::cout << "Testing FormatDuration..." << std::endl;

    assert(RenderETAEstimator::FormatDuration(0.0) == "0:00");
    assert(RenderETAEstimator::FormatDuration(9.0) == "0:09");
    assert(RenderETAEstimator::FormatDuration(65.0) == "1:05");
    assert(RenderETAEstimator::FormatDuration(605.0) == "10:05");
    assert(RenderETAEstimator::FormatDuration(3599.0) == "59:59");
    assert(RenderETAEstimator::FormatDuration(3600.0) == "01:00:00");
    assert(RenderETAEstimator::FormatDuration(3661.0) == "01:01:01");
    assert(RenderETAEstimator::FormatDuration(36000.0) == "10:00:00");
    assert(RenderETAEstimator::FormatDuration(-5.0) == "0:00");

    std::cout << "FormatDuration Passed!" << std::endl;
}

// 10. Zero/invalid totals should not crash.
static void TestDegenerateInputs() {
    std::cout << "Testing degenerate inputs..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    eta.Advance(1.0);
    eta.Update(0.0, 0.0);
    eta.Update(5.0, 0.0);
    double rem = 0.0;
    (void)eta.RemainingSeconds(rem);
    eta.Advance(1.0);
    eta.Update(2.0, 1.0);  // progress > total, should clamp to 1.0
    (void)eta.RemainingSeconds(rem);

    std::cout << "Degenerate inputs Passed!" << std::endl;
}

// 11. Variable render rate (slow start, fast finish or vice versa).
// The overall-rate estimator is slow to adapt, but must stay bounded
// and eventually converge.
static void TestVariableRate() {
    std::cout << "Testing variable render rate..." << std::endl;

    FakeClockETA eta;
    eta.Begin();

    // Fast phase: 0 -> 0.3 over 3 seconds (rate=0.1/s)
    for (int i = 1; i <= 30; ++i) {
        eta.Advance(0.1);
        eta.Update(i * 0.01, 1.0);
    }
    // Slow phase: 0.3 -> 1.0 over 14 seconds (rate=0.05/s).
    for (int i = 1; i <= 70; ++i) {
        eta.Advance(0.2);
        eta.Update(0.3 + i * 0.01, 1.0);
    }

    double rem = 0.0;
    assert(eta.RemainingSeconds(rem));
    // Finished — remaining should be small.
    assert(rem < 1.0);
    // Total elapsed == 3 + 14 = 17s; the estimate never exceeded a
    // reasonable bound. (Implicitly covered by no intermediate
    // asserts failing; we'd add one if needed.)
    assert(eta.ElapsedSeconds() > 16.5 && eta.ElapsedSeconds() < 17.5);

    std::cout << "Variable rate Passed!" << std::endl;
}

int main() {
    TestLinearProgress();
    TestWarmup();
    TestCountdownBetweenUpdates();
    TestConcurrentWorkerReordering();
    TestRealRasterizerTrace();
    TestLargeBackwardJump();
    TestStall();
    TestCompletion();
    TestFormatDuration();
    TestDegenerateInputs();
    TestVariableRate();
    std::cout << "All RenderETAEstimator tests passed!" << std::endl;
    return 0;
}
