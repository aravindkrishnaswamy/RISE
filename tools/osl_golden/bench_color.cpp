//////////////////////////////////////////////////////////////////////
//
//  bench_color.cpp - Time one call of the colour-conversion and Mix
//    Color kernels (src/Library/Utilities/OslColor.*).  Compare with
//    bench_voronoi.cpp for the 3D Noise Texture scale reference.  The
//    figures OslColor.h quotes come from this program.  It is NOT part
//    of RISE's build; from the repository root:
//
//    c++ -std=c++17 -O3 -fno-fast-math -ffp-contract=off -Isrc/Library \
//        -include src/Library/pch.h tools/osl_golden/bench_color.cpp \
//        src/Library/Utilities/OslColor.cpp -o /tmp/bench_color && /tmp/bench_color
//
//  Run it more than once: the first run's first rows include warm-up.
//
//  Tabs: 2
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdio>
#include "Utilities/OslColor.h"

using namespace RISE;

namespace {

template<class Fn>
void Bench(const char *label, int n, Fn fn)
{
  double sink = 0.0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    sink += fn(i);
  }
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
  std::printf("%-22s %6.2f ns/call (checksum %g)\n", label, ns, sink);
}

}  // namespace

int main()
{
  const int n = 2000000;
  // Precompute small varying inputs in a ring to avoid measuring input generation
  // while defeating constant folding.
  const int kRing = 256;
  Scalar a[kRing][3], b[kRing][3], t[kRing];
  for (int i = 0; i < kRing; ++i) {
    const double f = double(i) / double(kRing);
    a[i][0] = 0.1 + 0.8 * f;
    a[i][1] = 0.9 - 0.7 * f;
    a[i][2] = 0.2 + 0.5 * ((i * 37) % kRing) / double(kRing);
    b[i][0] = 0.8 - 0.6 * f;
    b[i][1] = 0.15 + 0.75 * ((i * 53) % kRing) / double(kRing);
    b[i][2] = 0.3 + 0.6 * f;
    t[i] = 0.05 + 0.9 * ((i * 19) % kRing) / double(kRing);
  }

  Bench("RgbToHsv", n, [&](int i) {
    Scalar out[3];
    OslColor::RgbToHsv(a[i & 255], out);
    return out[0] + out[1] + out[2];
  });
  Bench("HsvToRgb", n, [&](int i) {
    Scalar out[3];
    OslColor::HsvToRgb(a[i & 255], out);
    return out[0] + out[1] + out[2];
  });
  Bench("RgbToHsl", n, [&](int i) {
    Scalar out[3];
    OslColor::RgbToHsl(a[i & 255], out);
    return out[0] + out[1] + out[2];
  });
  Bench("HslToRgb", n, [&](int i) {
    Scalar out[3];
    OslColor::HslToRgb(a[i & 255], out);
    return out[0] + out[1] + out[2];
  });

  const struct {
    int mode;
    const char *name;
  } kModes[] = {
      {OslColor::eBlend, "MixColor Blend"},
      {OslColor::eOverlay, "MixColor Overlay"},
      {OslColor::eSoft, "MixColor Soft"},
      {OslColor::eDodge, "MixColor Dodge"},
      {OslColor::eBurn, "MixColor Burn"},
      {OslColor::eHue, "MixColor Hue"},
      {OslColor::eSat, "MixColor Sat"},
      {OslColor::eVal, "MixColor Val"},
      {OslColor::eCol, "MixColor Color"},
  };
  for (const auto &m : kModes) {
    Bench(m.name, n, [&](int i) {
      const int k = i & 255;
      Scalar out[3];
      OslColor::MixColor(m.mode, t[k], a[k], b[k], true, false, out);
      return out[0] + out[1] + out[2];
    });
  }
  return 0;
}
