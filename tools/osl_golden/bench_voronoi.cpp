//////////////////////////////////////////////////////////////////////
//
//  bench_voronoi.cpp - Time one call of the Voronoi kernel
//    (src/Library/Utilities/OslVoronoi.*) per dimension, feature and
//    detail, beside the Noise Texture kernel for scale.  The figures
//    OslVoronoi.h quotes come from this program.  It is NOT part of
//    RISE's build; from the repository root:
//
//    c++ -std=c++17 -O3 -fno-fast-math -ffp-contract=off -Isrc/Library \
//        -include src/Library/pch.h tools/osl_golden/bench_voronoi.cpp \
//        src/Library/Utilities/OslVoronoi.cpp src/Library/Utilities/OslNoise.cpp \
//        -o bench_voronoi && ./bench_voronoi
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
#include "Utilities/OslNoise.h"
#include "Utilities/OslVoronoi.h"

using namespace RISE;

int main()
{
  const char *featureName[5] = {"F1", "F2", "SmoothF1", "DistToEdge", "NSphere"};
  for (int dims = 1; dims <= 4; ++dims) {
    for (int feature = 0; feature < 5; ++feature) {
      for (int detail : {0, 15}) {
        if (feature == OslVoronoi::eNSphereRadius && detail != 0) {
          continue;  // N-Sphere Radius has no fractal: detail is unread
        }
        const int n = (dims == 4 && feature == OslVoronoi::eSmoothF1) ? 2000 : 20000;
        double sink = 0;
        Scalar p[4];
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
          p[0] = 0.37 + i * 0.013;
          p[1] = 1.1 - i * 0.007;
          p[2] = 0.2 + i * 0.003;
          p[3] = -0.4 + i * 0.011;
          OslVoronoi::Result r;
          OslVoronoi::VoronoiTexture(dims, p, feature, OslVoronoi::eEuclidean, 5, detail, 0.5, 2, 1, 1, 1,
                                     false, r);
          sink += r.distance + r.color[0] + r.radius;
        }
        const double ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("dims %d %-10s detail %2d: %9.0f ns/call (checksum %g)\n", dims, featureName[feature],
                    detail, ns, sink);
      }
    }
  }
  for (int detail : {2, 15}) {
    double sink = 0;
    Scalar p[4] = {0.3, 0.4, 0.5, 0};
    const int n = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
      p[0] = 0.3 + i * 0.01;
      Scalar fac;
      OslNoise::NoiseTexture(3, p, OslNoise::eFbm, detail, 0.5, 2, 0, 1, 0, true, fac, 0);
      sink += fac;
    }
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("for scale: Noise Texture 3D fBM detail %2d: %6.0f ns/call (checksum %g)\n", detail, ns, sink);
  }
  return 0;
}
