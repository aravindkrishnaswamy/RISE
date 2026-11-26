//////////////////////////////////////////////////////////////////////
//
//  bench_procedural_baseline.cpp - Performance baseline for RISE's
//    expression VM and Cycles-ported procedural builtins (design doc §8).
//
//  Measures:
//    1. ns/instruction on a pure-arithmetic VM program (41 instructions).
//    2. ns/call for Group 5 (OslMath), Group 6 (OslVector), Group 7 (live
//       space transforms) through the VM.
//    3. ns/eval for the exact verbatim 8-node median and 36-node p75 Phase 1
//       Blender shader graphs compiled by procedural_expr.py (via
//       tools/measure_procedural_lowering.py --write-bench-inc ->
//       bench_procedural_graphs.inc), plus L2 memo OFF vs ON for 3x same-hit
//       same-progId re-evaluation.
//
//  Build & run from repo root:
//    make -C build/make/rise -j8 all
//    c++ -std=c++17 -O3 -ffast-math -Isrc/Library -include src/Library/pch.h \
//        tools/osl_golden/bench_procedural_baseline.cpp \
//        bin/librise.a -L/opt/homebrew/lib -lpng -ljpeg -ltiff -lz -lOpenImageDenoise \
//        -framework Security -framework CoreFoundation -lpthread -lm -o /tmp/bench_proc
//
//  Tabs: 2
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "Painters/ExpressionEval.h"
#include "Painters/ExpressionPainter.h"
#include "Utilities/ExpressionMemo.h"
#include "Cameras/PinholeCamera.h"

#include "bench_procedural_graphs.inc"

using namespace RISE;
using namespace RISE::Implementation;

namespace {

template<class Fn>
double Bench(const char *label, int n, Fn fn)
{
  // Warmup
  double sink = 0.0;
  for (int i = 0; i < 4096; ++i) {
    sink += fn(i);
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    sink += fn(i);
  }
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
  std::printf("%-42s %8.2f ns/eval  (checksum %.4g)\n", label, ns, sink);
  return ns;
}

ExpressionProgram CompileOrDie(const std::vector<std::pair<std::string, std::string>> &defs,
                               const std::string &expr)
{
  ExpressionProgram::Builder b;
  b.EnableContextVars(true);
  for (const auto &d : defs) {
    if (!b.AddDef(d.first, d.second)) {
      std::fprintf(stderr, "Def %s compile error: %s\n", d.first.c_str(), b.Error().c_str());
      std::exit(1);
    }
  }
  ExpressionProgram prog = ExpressionProgram::Invalid();
  if (!b.Finalize(expr, prog)) {
    std::fprintf(stderr, "Expr compile error: %s\n", b.Error().c_str());
    std::exit(1);
  }
  return prog;
}

// Parse a multi-line procedural_expr.py expression ("def name body\n...\nfinal_expr").
ExpressionProgram CompileProgramTextOrDie(const char *src)
{
  std::vector<std::pair<std::string, std::string>> defs;
  std::string finalExpr;
  std::string text(src);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) {
      nl = text.size();
    }
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) {
      continue;
    }
    if (line.compare(0, 4, "def ") == 0) {
      size_t sp = line.find(' ', 4);
      if (sp == std::string::npos) {
        std::fprintf(stderr, "Malformed def line: %s\n", line.c_str());
        std::exit(1);
      }
      defs.emplace_back(line.substr(4, sp - 4), line.substr(sp + 1));
    } else {
      finalExpr = line;
    }
  }
  return CompileOrDie(defs, finalExpr);
}

}  // namespace

int main()
{
  const int kRing = 256;
  PinholeCamera *cam = new PinholeCamera(
      Point3(0, 0, 5), Point3(0, 0, 0), Vector3(0, 1, 0),
      45.0 * DEG_TO_RAD, 320, 240, 1.0, 0, 0, 0, Vector3(0, 0, 0), Vector2(0, 0));
  const Matrix4 mxO2W = Matrix4Ops::Stretch(Vector3(1.5, 0.8, 2.0)) *
                        Matrix4Ops::YRotation(0.7) *
                        Matrix4Ops::Translation(Vector3(1.0, -2.0, 3.0));
  const Matrix4 mxW2O = Matrix4Ops::Inverse(mxO2W);

  std::vector<RayIntersectionGeometric> hits;
  hits.reserve(kRing);
  ExprEvalContext ctxs[kRing];
  for (int i = 0; i < kRing; ++i) {
    const double f = double(i) / double(kRing);
    hits.emplace_back(Ray(Point3(0, 0, 5), Vector3(0, 0, -1)), nullRasterizerState);
    hits[i].ptIntersection = Point3(0.3 + 2.1 * f, -0.7 + 1.4 * f, 0.5 - 0.9 * f);
    hits[i].ptObjIntersec = Point3Ops::Transform(mxW2O, hits[i].ptIntersection);
    hits[i].vNormal = Vector3Ops::Normalize(Vector3(0.2 + f, 0.8 - 0.3 * f, 0.4));
    hits[i].ptCoord = Point2(f, 1.0 - f);
    hits[i].pmxObjectToWorld = &mxO2W;
    hits[i].pmxWorldToObject = &mxW2O;
    hits[i].pCamera = cam;

    ctxs[i].u = hits[i].ptCoord.x;
    ctxs[i].v = hits[i].ptCoord.y;
    ctxs[i].P = Vector3(hits[i].ptIntersection.x, hits[i].ptIntersection.y, hits[i].ptIntersection.z);
    ctxs[i].Po = Vector3(hits[i].ptObjIntersec.x, hits[i].ptObjIntersec.y, hits[i].ptObjIntersec.z);
    ctxs[i].N = hits[i].vNormal;
    ctxs[i].pHit = &hits[i];
  }

  // 1. Pure arithmetic 41-instruction program -> ns per VM instruction
  std::string arith = "u";
  for (int i = 0; i < 20; ++i) arith += "+u";
  ExpressionProgram pArith = CompileOrDie({}, arith);
  const double nsArith = Bench("VM 41-instr arithmetic (20 adds)", 2000000, [&](int i) {
    return pArith.Eval(ctxs[i & 255]);
  });
  std::printf("  => %.2f ns/instruction (41 bytecode ops)\n", nsArith / 41.0);

  // 2. Group 5-7 individual builtin calls through VM
  ExpressionProgram pMathAdd = CompileOrDie({}, "osl_math(0, u, v, 0)");
  ExpressionProgram pMathPow = CompileOrDie({}, "osl_math(10, u + 0.1, v + 1.2, 0)");
  ExpressionProgram pVMathCross = CompileOrDie({}, "osl_vmath(4, Po, N, vec3(0,0,0), 1)");
  ExpressionProgram pMapRange = CompileOrDie({}, "osl_map_range(3, 1, u, 0.1, 0.9, 0, 1, 4)");
  ExpressionProgram pVecRotAxis = CompileOrDie({}, "osl_vector_rotate(0, 0, Po, vec3(0,0,0), vec3(0,1,0), u, vec3(0,0,0))");
  ExpressionProgram pVecRotEuler = CompileOrDie({}, "osl_vector_rotate(4, 0, Po, vec3(0,0,0), vec3(0,0,1), 0, vec3(u, v, 0.5))");
  ExpressionProgram pSpaceObjPt = CompileOrDie({}, "osl_to_object_point(P)");
  ExpressionProgram pSpaceObjN = CompileOrDie({}, "osl_to_object_normal(N)");
  ExpressionProgram pSpaceCamPt = CompileOrDie({}, "osl_to_camera_point(P)");

  Bench("VM osl_math(ADD)", 2000000, [&](int i) { return pMathAdd.Eval(ctxs[i & 255]); });
  Bench("VM osl_math(POWER)", 2000000, [&](int i) { return pMathPow.Eval(ctxs[i & 255]); });
  Bench("VM osl_vmath(CROSS)", 2000000, [&](int i) { return pVMathCross.EvalVec3(ctxs[i & 255]).x; });
  Bench("VM osl_map_range(SMOOTHERSTEP)", 2000000, [&](int i) { return pMapRange.Eval(ctxs[i & 255]); });
  Bench("VM osl_vector_rotate(AXIS)", 1000000, [&](int i) { return pVecRotAxis.EvalVec3(ctxs[i & 255]).x; });
  Bench("VM osl_vector_rotate(EULER_XYZ)", 1000000, [&](int i) { return pVecRotEuler.EvalVec3(ctxs[i & 255]).x; });
  Bench("VM osl_to_object_point(P)", 2000000, [&](int i) { return pSpaceObjPt.EvalVec3(ctxs[i & 255]).x; });
  Bench("VM osl_to_object_normal(N)", 2000000, [&](int i) { return pSpaceObjN.EvalVec3(ctxs[i & 255]).x; });
  Bench("VM osl_to_camera_point(P)", 2000000, [&](int i) { return pSpaceCamPt.EvalVec3(ctxs[i & 255]).x; });

  // 3. Exact verbatim procedural_expr.py-compiled 8-node median and 36-node p75 Phase 1 graphs
  ExpressionProgram p8 = CompileProgramTextOrDie(kBenchGraph8Expr);
  ExpressionProgram p36 = CompileProgramTextOrDie(kBenchGraph36Expr);

  Bench("8-node median (verbatim procedural_expr)", 200000, [&](int i) {
    return p8.EvalVec3(ctxs[i & 255]).x;
  });
  Bench("36-node p75 (verbatim procedural_expr)", 100000, [&](int i) {
    return p36.EvalVec3(ctxs[i & 255]).x;
  });

  // Measure L2 memo benefit for 3x same-hit same-progId evaluation (e.g. a shared
  // painter queried multiple times at one intersection).
  ExpressionPainter *ep36 = new ExpressionPainter(p36, std::vector<ParamSpec>(), Scalar(0));
  ExpressionMemo::SetEnabledOverride(0);
  Bench("36-node 3x same-hit same-progId (memo OFF)", 50000, [&](int i) {
    const auto &ri = hits[i & 255];
    return ep36->GetColor(ri)[0] + ep36->GetColor(ri)[1] + ep36->GetColor(ri)[2];
  });
  ExpressionMemo::SetEnabledOverride(1);
  Bench("36-node 3x same-hit same-progId (memo ON)", 50000, [&](int i) {
    const auto &ri = hits[i & 255];
    return ep36->GetColor(ri)[0] + ep36->GetColor(ri)[1] + ep36->GetColor(ri)[2];
  });
  ExpressionMemo::SetEnabledOverride(-1);

  ep36->release();
  cam->release();
  return 0;
}
