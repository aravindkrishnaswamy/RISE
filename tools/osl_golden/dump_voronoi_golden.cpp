//////////////////////////////////////////////////////////////////////
//
//  dump_voronoi_golden.cpp - Dump golden values for RISE's `osl_voronoi_`
//    builtins (src/Library/Utilities/OslVoronoi.*) from Cycles' own
//    Voronoi Texture node.  Build and run with
//    tools/osl_golden/dump_voronoi_golden.sh, which compiles
//    dump_voronoi_path.cpp against a Blender source checkout three times
//    -- Cycles' SIMD path and its scalar path as Blender builds them, and
//    the scalar path as strict IEEE -- and links all three here.  Output:
//    tests/data/osl_voronoi_golden.inc.
//
//  It is NOT part of RISE's build and links nothing of RISE's.  Every
//  input is generated as a float and written as a hexadecimal float
//  literal, so RISE reads exactly the value Cycles evaluated; every
//  output is Cycles' float result, written exactly, once per build (the
//  PCG hash rows carry one value, checked equal across the builds).
//
//  Tabs: 2
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define OSL_DECL(path) \
  void HashInt2##path(const int *, float *); \
  void HashInt3##path(const int *, float *); \
  void HashInt4##path(const int *, float *); \
  void Voronoi##path(int, const float *, int, int, const float *, bool, float *);
OSL_DECL(simd)
OSL_DECL(scalar)
OSL_DECL(ieee)

namespace {

// The same fixed LCG and per-table seeding as dump_noise_golden.cpp.
uint32_t g_state = 0x9e3779b9u;
uint32_t Next()
{
  g_state = g_state * 1664525u + 1013904223u;
  return g_state;
}
void Reseed(unsigned table)
{
  g_state = 0x9e3779b9u ^ (table * 0x85ebca6bu);
}
float Rand01()
{
  return float(Next() >> 8) * (1.0f / 16777216.0f);
}
float RandRange(float lo, float hi)
{
  return lo + (hi - lo) * Rand01();
}
float RandSign()
{
  return Rand01() < 0.5f ? -1.0f : 1.0f;
}
// One of `n` choices, uniformly.
template<int N> float Pick(const float (&choices)[N])
{
  return choices[Next() % unsigned(N)];
}

// A float as a C++ literal: hexadecimal, so exact, and the infinities
// and NaN by name, since printf's "inf" and "nan" are not literals.
void PrintFloat(float v)
{
  if (std::isnan(v)) {
    printf("NAN");
  }
  else if (std::isinf(v)) {
    printf("%sINFINITY", v < 0 ? "-" : "");
  }
  else {
    printf("%a", double(v));
  }
}

void PrintArray(const float *v, int n)
{
  printf("{");
  for (int k = 0; k < n; ++k) {
    PrintFloat(v[k]);
    printf("%s", k + 1 < n ? "," : "");
  }
  printf("}");
}

// Coordinates (before the node's Scale): texture space, a wider range,
// exact integers and half-integers (cell corners and the midpoints where,
// at randomness 0, two feature points are equally near -- so the loop
// order decides), and a large range where a float's spacing approaches
// a cell.
const int kBands = 5;
void RandCoords(int dims, int band, float *c)
{
  for (int k = 0; k < 4; ++k) {
    c[k] = 0.0f;
  }
  for (int k = 0; k < dims; ++k) {
    switch (band) {
      case 0: c[k] = RandRange(-4.0f, 4.0f); break;
      case 1: c[k] = RandRange(-60.0f, 60.0f); break;
      case 2: c[k] = float(int(RandRange(-6.0f, 6.0f))); break;
      case 3: c[k] = float(int(RandRange(-6.0f, 6.0f))) + 0.5f; break;
      default: c[k] = RandRange(1.0e5f, 4.0e6f) * RandSign(); break;
    }
  }
}

struct Inputs {
  float scale, detail, roughness, lacunarity, smoothness, exponent, randomness;
  bool normalize;
};

// The node's parameters, drawn so the table covers: zero detail and zero
// roughness (Cycles' single-octave shortcut), whole and fractional
// details, the node's clamps on every clamped input (detail, roughness,
// randomness, smoothness), Smooth F1 at smoothness 0 (which runs F1),
// lacunarities below 1 and negative, Minkowski at the exponents that
// equal the other metrics and between them, and both normalize settings.
Inputs RandInputs()
{
  const float scales[] = {1.0f, 0.75f, 2.5f, 5.0f, -1.5f};
  const float details[] = {0.0f, 1.0f, 2.0f, 3.5f, 4.37f, 7.0f, 15.0f, -1.0f, 20.0f};
  const float roughs[] = {0.0f, 0.5f, 0.35f, 0.8f, 1.0f, 1.5f, -0.25f};
  const float lacs[] = {2.0f, 2.0f, 1.7f, 2.9f, 0.6f, 1.0f, -2.0f};
  const float smooths[] = {0.0f, 0.25f, 1.0f, 0.6f, 2.0f, 3.0f};
  const float exps[] = {0.5f, 1.0f, 2.0f, 3.5f, 0.3f, 7.0f};
  const float rands[] = {1.0f, 1.0f, 0.0f, 0.5f, 0.85f, 1.25f, -0.5f};
  Inputs q;
  q.scale = (Next() % 3u == 0u) ? Pick(scales) : RandRange(0.3f, 6.0f);
  q.detail = (Next() % 3u == 0u) ? RandRange(0.0f, 15.0f) : Pick(details);
  q.roughness = (Next() % 3u == 0u) ? RandRange(0.0f, 1.0f) : Pick(roughs);
  q.lacunarity = Pick(lacs);
  q.smoothness = (Next() % 3u == 0u) ? RandRange(0.0f, 1.0f) : Pick(smooths);
  q.exponent = (Next() % 3u == 0u) ? RandRange(0.4f, 4.0f) : Pick(exps);
  q.randomness = (Next() % 3u == 0u) ? RandRange(0.0f, 1.0f) : Pick(rands);
  q.normalize = (Next() & 1u) != 0u;
  return q;
}

void PrintRow(int dims, const float *c, int feature, int metric, const Inputs &q)
{
  const float in[7] = {q.scale, q.detail, q.roughness, q.lacunarity, q.smoothness, q.exponent,
                       q.randomness};
  float a[9], b[9], e[9];
  Voronoisimd(dims, c, feature, metric, in, q.normalize, a);
  Voronoiscalar(dims, c, feature, metric, in, q.normalize, b);
  Voronoiieee(dims, c, feature, metric, in, q.normalize, e);
  printf("  { %d, ", dims);
  float cc[4];
  for (int k = 0; k < 4; ++k) {
    cc[k] = k < dims ? c[k] : 0.0f;
  }
  PrintArray(cc, 4);
  printf(", %d, %d, ", feature, metric);
  for (int k = 0; k < 7; ++k) {
    PrintFloat(in[k]);
    printf(", ");
  }
  printf("%d, ", q.normalize ? 1 : 0);
  PrintArray(a, 9);
  printf(", ");
  PrintArray(b, 9);
  printf(", ");
  PrintArray(e, 9);
  printf(" },\n");
}

}  // namespace

int main()
{
  printf("// GENERATED by tools/osl_golden/dump_voronoi_golden.sh -- do not edit by hand.\n");
  printf("// Cycles' own svm_node_tex_voronoi evaluated in float; inputs and outputs\n");
  printf("// are exact (hexadecimal float literals).  Each node output is given three\n");
  printf("// times: Cycles' SIMD path and scalar path as Blender builds them, then the\n");
  printf("// scalar path as strict IEEE.  The PCG hash rows are integer arithmetic and\n");
  printf("// carry one value, on which the three builds were checked to agree.\n");
  printf("// Outputs: {distance, color.rgb, position.xyz, w, radius}.\n");
  printf("// See tests/OslVoronoiTest.cpp.\n\n");

  // --- the PCG cell hashes (exact) ---
  Reseed(0);
  printf("static const OslPcgRow kPcgRows[] = {\n");
  for (int dims = 2; dims <= 4; ++dims) {
    for (int i = 0; i < 24; ++i) {
      int k[4] = {0, 0, 0, 0};
      for (int c = 0; c < dims; ++c) {
        // Small cell ids, as ordinary coordinates give, then the extremes.
        if (i < 12) {
          k[c] = int(Next() % 41u) - 20;
        }
        else if (i < 20) {
          k[c] = int(Next());
        }
        else {
          const int edge[4] = {2147483647, -2147483647 - 1, -1, 0};
          k[c] = edge[(i + c) % 4];
        }
      }
      float h[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
      if (dims == 2) {
        HashInt2simd(k, h[0]);
        HashInt2scalar(k, h[1]);
        HashInt2ieee(k, h[2]);
      }
      else if (dims == 3) {
        HashInt3simd(k, h[0]);
        HashInt3scalar(k, h[1]);
        HashInt3ieee(k, h[2]);
      }
      else {
        HashInt4simd(k, h[0]);
        HashInt4scalar(k, h[1]);
        HashInt4ieee(k, h[2]);
      }
      if (std::memcmp(h[0], h[1], sizeof h[0]) != 0 || std::memcmp(h[0], h[2], sizeof h[0]) != 0) {
        fprintf(stderr, "PCG hashes: the three Cycles builds disagree (dims %d, row %d)\n", dims, i);
        return 1;
      }
      printf("  { %d, {%d,%d,%d,%d}, {%a,%a,%a,%a} },\n", dims, k[0], k[1], k[2], k[3],
             double(h[0][0]), double(h[0][1]), double(h[0][2]), double(h[0][3]));
    }
  }
  printf("};\n\n");

  // --- the Voronoi node: every dimension, feature and metric ---
  Reseed(1);
  printf("static const OslVoronoiRow kVoronoiRows[] = {\n");
  for (int dims = 1; dims <= 4; ++dims) {
    for (int feature = 0; feature < 5; ++feature) {
      for (int metric = 0; metric < 4; ++metric) {
        for (int i = 0; i < 12; ++i) {
          float c[4];
          RandCoords(dims, i % kBands, c);
          const Inputs q = RandInputs();
          PrintRow(dims, c, feature, metric, q);
        }
      }
    }
  }
  printf("};\n\n");

  // --- ties: randomness 0 puts every feature point on its cell corner,
  // so at a half-integer coordinate two or more points are equally near
  // and Cycles' loop order picks the cell.  Single octave, scale 1.
  Reseed(2);
  printf("static const OslVoronoiRow kTieRows[] = {\n");
  for (int dims = 1; dims <= 4; ++dims) {
    for (int feature = 0; feature < 5; ++feature) {
      for (int metric = 0; metric < 4; ++metric) {
        float c[4];
        RandCoords(dims, 3, c);
        Inputs q = {1.0f, 0.0f, 0.5f, 2.0f, 0.5f, 1.5f, 0.0f, (Next() & 1u) != 0u};
        PrintRow(dims, c, feature, metric, q);
      }
    }
  }
  printf("};\n\n");

  // --- edge cases: the non-finite results Cycles produces from finite
  // inputs -- a zero lacunarity (position * 1/0) and a zero Minkowski
  // exponent (a power of 1/0) -- and a zero Scale, whose coordinate is 0
  // and whose Position takes safe_divide's zero branch.
  Reseed(3);
  printf("static const OslVoronoiRow kEdgeRows[] = {\n");
  for (int dims = 1; dims <= 4; ++dims) {
    for (int i = 0; i < 3; ++i) {
      float c[4];
      RandCoords(dims, 0, c);
      Inputs q = {2.0f, 3.0f, 0.5f, 2.0f, 0.5f, 2.0f, 1.0f, true};
      int feature = 0, metric = 0;
      if (i == 0) {
        q.lacunarity = 0.0f;
      }
      else if (i == 1) {
        q.scale = 0.0f;
      }
      else {
        q.exponent = 0.0f;
        metric = 3;
        feature = 1;
      }
      PrintRow(dims, c, feature, metric, q);
    }
  }
  printf("};\n\n");

  // --- signed zeros: a coordinate that is -0 after the Scale multiply
  // (-0 itself at Scale 1, or +0 at a negative Scale).  In 1D the kernel
  // hashes the float bits of cellPosition + offset: strict IEEE turns
  // -0 + 0 into +0, while Blender's -fno-signed-zeros lets the compiler
  // drop the + 0 and hash -0, so the strict and as-built columns differ
  // here by design.  2D-4D convert to int and cannot see the sign.
  Reseed(4);
  printf("static const OslVoronoiRow kSignedZeroRows[] = {\n");
  for (int dims = 1; dims <= 4; ++dims) {
    for (int feature = 0; feature < 5; ++feature) {
      for (int k = 0; k < 2; ++k) {
        float c[4];
        RandCoords(dims, 0, c);
        Inputs q = {1.0f, 0.0f, 0.5f, 2.0f, 0.5f, 1.5f, 1.0f, false};
        if (k == 0) {
          c[0] = -0.0f;  // -0 at Scale 1
        }
        else {
          c[0] = 0.0f;  // +0 at a negative Scale
          q.scale = -1.5f;
          q.detail = 2.0f;
        }
        PrintRow(dims, c, feature, (feature + k) % 4, q);
      }
    }
  }
  printf("};\n");
  return 0;
}
