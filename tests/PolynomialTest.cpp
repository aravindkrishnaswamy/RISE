#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <array>
#include <iomanip>
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Intersection/RayPrimitiveIntersections.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Functions/Polynomial.h"

using namespace RISE;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-6) {
    return std::fabs(a - b) < epsilon;
}

// Helper: verify that each claimed solution actually satisfies the polynomial
void VerifyQuadricRoots(const Scalar (&coeff)[3], const Scalar (&sol)[2], int n) {
    for (int i = 0; i < n; i++) {
        Scalar x = sol[i];
        Scalar val = coeff[0]*x*x + coeff[1]*x + coeff[2];
        assert(IsClose(val, 0.0, 1e-4));
    }
}

void VerifyCubicRoots(const Scalar (&coeff)[4], const Scalar (&sol)[3], int n) {
    for (int i = 0; i < n; i++) {
        Scalar x = sol[i];
        Scalar val = coeff[0]*x*x*x + coeff[1]*x*x + coeff[2]*x + coeff[3];
        assert(IsClose(val, 0.0, 1e-4));
    }
}

void VerifyQuarticRoots(const Scalar (&coeff)[5], const Scalar (&sol)[4], int n) {
    for (int i = 0; i < n; i++) {
        Scalar x = sol[i];
        Scalar val = coeff[0]*x*x*x*x + coeff[1]*x*x*x + coeff[2]*x*x + coeff[3]*x + coeff[4];
        assert(IsClose(val, 0.0, 1e-3));
    }
}

void TestSolveQuadric() {
    std::cout << "Testing SolveQuadric..." << std::endl;

    // Case 1: Two distinct real roots: x^2 - 5x + 6 = 0 => x=2, x=3
    {
        Scalar coeff[3] = {1, -5, 6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Case 2: Double root: x^2 - 4x + 4 = 0 => x=2 (double)
    {
        Scalar coeff[3] = {1, -4, 4};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n >= 1);
        assert(IsClose(sol[0], 2.0, 1e-4));
    }

    // Case 3: No real roots: x^2 + 1 = 0
    {
        Scalar coeff[3] = {1, 0, 1};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 0);
    }

    // Case 4: Roots at zero: x^2 - x = 0 => x=0, x=1
    {
        Scalar coeff[3] = {1, -1, 0};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Case 5: Large coefficients
    {
        Scalar coeff[3] = {1, -2000, 999999};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Case 6: Negative leading coefficient: -x^2 + 3x - 2 = 0 => x=1, x=2
    {
        Scalar coeff[3] = {-1, 3, -2};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Case 7: leading coefficient NOT +/-1 -- the two-root branch divides
    // by 2a, it does not multiply by a/2.  2x^2 - 6x + 4 = 0 => x=1, x=2.
    // RED-PROOF: restoring `const Scalar p = 0.5 * a;` makes this return
    // 8 and 4 (each root scaled by a^2 = 4) and VerifyQuadricRoots fires.
    // Cases 1-6 above all have |a| == 1, where 0.5*a and 0.5/a coincide,
    // which is why the bug survived them.
    {
        Scalar coeff[3] = {2, -6, 4};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
        const Scalar lo = std::fmin(sol[0], sol[1]);
        const Scalar hi = std::fmax(sol[0], sol[1]);
        assert(IsClose(lo, 1.0, 1e-9));
        assert(IsClose(hi, 2.0, 1e-9));
    }

    // Case 8: large leading coefficient, distinct roots.
    // 100x^2 - 300x + 200 = 0 => x=1, x=2 (a^2 = 1e4 scaling under the bug).
    {
        Scalar coeff[3] = {100, -300, 200};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 2);
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Case 9: degenerate leading coefficient -- LINEAR, matching
    // SolveQuadricWithinRange's long-standing a == 0 branch.  3x - 6 = 0.
    {
        Scalar coeff[3] = {0, 3, -6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 1);
        assert(IsClose(sol[0], 2.0, 1e-9));
    }

    // Case 10: a and b both zero -- no root (and no division by zero).
    {
        Scalar coeff[3] = {0, 0, 5};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadric(coeff, sol);
        assert(n == 0);
    }

    std::cout << "SolveQuadric Passed!" << std::endl;
}

void TestSolveQuadricWithinRange() {
    std::cout << "Testing SolveQuadricWithinRange..." << std::endl;

    // x^2 - 5x + 6 = 0 => x=2, x=3
    // Range [0, 2.5] should include only x=2
    {
        Scalar coeff[3] = {1, -5, 6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 2.5);
        assert(n == 1);
        assert(IsClose(sol[0], 2.0, 1e-4));
    }

    // Range [2.5, 3.5] should include only x=3
    {
        Scalar coeff[3] = {1, -5, 6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 2.5, 3.5);
        assert(n == 1);
        assert(IsClose(sol[0], 3.0, 1e-4));
    }

    // Range [1.5, 3.5] should include both
    {
        Scalar coeff[3] = {1, -5, 6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 1.5, 3.5);
        assert(n == 2);
    }

    // Range [4, 5] should include none
    {
        Scalar coeff[3] = {1, -5, 6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 4, 5);
        assert(n == 0);
    }

    // Linear equation (a=0): 3x - 6 = 0 => x=2
    {
        Scalar coeff[3] = {0, 3, -6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 5);
        assert(n == 1);
        assert(IsClose(sol[0], 2.0, 1e-4));
    }

    // Linear equation, out of range
    {
        Scalar coeff[3] = {0, 3, -6};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 3, 5);
        assert(n == 0);
    }

    // Both a and b zero: degenerate
    {
        Scalar coeff[3] = {0, 0, 5};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, -10, 10);
        assert(n == 0);
    }

    // ---- Exact double roots (discriminant identically 0) --------------
    //
    // The d == 0.0 branch returns the parabola's VERTEX, -b/(2a).  It used
    // to return -b/a -- twice the root -- which satisfies the polynomial
    // only when the root itself is 0.  Every coefficient triple below has
    // b*b - 4*a*c evaluating to EXACTLY 0.0 in binary FP (the constants are
    // dyadic), so the branch is genuinely taken rather than falling into
    // the two-root path.
    //
    // RED-PROOF: restoring `sol[0] = -b/a;` makes all three report the
    // doubled root (1.0, 0.5, 2.0), so VerifyQuadricRoots fires on the
    // first and the closed-form asserts on the rest.

    // x^2 - x + 0.25 = 0 => x = 0.5 (double)
    {
        Scalar coeff[3] = {1, -1, 0.25};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 1);
        assert(n == 1);
        assert(IsClose(sol[0], 0.5, 1e-12));
        VerifyQuadricRoots(coeff, sol, n);
    }

    // x^2 - 0.5x + 0.0625 = 0 => x = 0.25 (double)
    {
        Scalar coeff[3] = {1, -0.5, 0.0625};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 1);
        assert(n == 1);
        assert(IsClose(sol[0], 0.25, 1e-12));
        VerifyQuadricRoots(coeff, sol, n);
    }

    // x^2 - 2x + 1 = 0 => x = 1 (double), inside [0, 2]
    {
        Scalar coeff[3] = {1, -2, 1};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 2);
        assert(n == 1);
        assert(IsClose(sol[0], 1.0, 1e-12));
        VerifyQuadricRoots(coeff, sol, n);
    }

    // Same double root, range chosen so the CORRECT root (1.0) is outside
    // it but the pre-fix doubled root (2.0) would have been inside.  This
    // is the direction the bilinear-patch caller cared about: its range is
    // [-NEARZERO, 1+NEARZERO], so a doubled v silently escaped the clamp.
    {
        Scalar coeff[3] = {1, -2, 1};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 1.5, 2.5);
        assert(n == 0);
    }

    // Double root at a non-unit leading coefficient: 4x^2 - 4x + 1 = 0
    // => x = 0.5.  Pins the 2a divisor (not just the 2).
    {
        Scalar coeff[3] = {4, -4, 1};
        Scalar sol[2] = {0, 0};
        int n = Polynomial::SolveQuadricWithinRange(coeff, sol, 0, 1);
        assert(n == 1);
        assert(IsClose(sol[0], 0.5, 1e-12));
        VerifyQuadricRoots(coeff, sol, n);
    }

    std::cout << "SolveQuadricWithinRange Passed!" << std::endl;
}

void TestSolveCubic() {
    std::cout << "Testing SolveCubic..." << std::endl;

    // Case 1: Three real roots: x^3 - 6x^2 + 11x - 6 = 0 => x=1,2,3
    {
        Scalar coeff[4] = {1, -6, 11, -6};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n == 3);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 2: One real root: x^3 + x = 0 only at x=0 (x^2+1 has no real roots)
    // Actually x^3 + x = x(x^2 + 1) = 0, root at 0
    {
        Scalar coeff[4] = {1, 0, 1, 0};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n >= 1);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 3: Triple root: (x-1)^3 = x^3 - 3x^2 + 3x - 1 = 0
    {
        Scalar coeff[4] = {1, -3, 3, -1};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n >= 1);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 4: Degenerates to quadratic (leading coeff 0): 0x^3 + x^2 - 1 = 0 => x=+-1
    {
        Scalar coeff[4] = {0, 1, 0, -1};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n == 2);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 5: x^3 - 1 = 0 => x=1 (only real root)
    {
        Scalar coeff[4] = {1, 0, 0, -1};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n >= 1);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 6: Negative leading coeff: -x^3 + 6x^2 - 11x + 6 = 0 => x=1,2,3
    {
        Scalar coeff[4] = {-1, 6, -11, 6};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n == 3);
        VerifyCubicRoots(coeff, sol, n);
    }

    // Case 7: Two roots (double + single): (x-1)^2(x-3) = x^3 - 5x^2 + 7x - 3
    {
        Scalar coeff[4] = {1, -5, 7, -3};
        Scalar sol[3] = {0, 0, 0};
        int n = Polynomial::SolveCubic(coeff, sol);
        assert(n >= 1);
        VerifyCubicRoots(coeff, sol, n);
    }

    std::cout << "SolveCubic Passed!" << std::endl;
}

void TestSolveQuartic() {
    std::cout << "Testing SolveQuartic..." << std::endl;

    // Case 1: (x-1)(x-2)(x-3)(x-4) = x^4 - 10x^3 + 35x^2 - 50x + 24
    {
        Scalar coeff[5] = {1, -10, 35, -50, 24};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n == 4);
        VerifyQuarticRoots(coeff, sol, n);
    }

    // Case 2: x^4 - 1 = 0 => x=1, x=-1 (real roots only)
    {
        Scalar coeff[5] = {1, 0, 0, 0, -1};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n >= 2);
        VerifyQuarticRoots(coeff, sol, n);
    }

    // Case 3: x^4 + 1 = 0 => no real roots
    {
        Scalar coeff[5] = {1, 0, 0, 0, 1};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n == 0);
    }

    // Case 4: Degenerates to cubic (leading coeff 0)
    {
        Scalar coeff[5] = {0, 1, -6, 11, -6};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n == 3);
        VerifyQuarticRoots(coeff, sol, n);
    }

    // Case 5: Two double roots: (x-1)^2(x+1)^2 = x^4 - 2x^2 + 1
    {
        Scalar coeff[5] = {1, 0, -2, 0, 1};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n >= 2);
        VerifyQuarticRoots(coeff, sol, n);
    }

    // Case 6: Root at zero: x^4 - x^3 = x^3(x-1) = 0
    {
        Scalar coeff[5] = {1, -1, 0, 0, 0};
        Scalar sol[4] = {0, 0, 0, 0};
        int n = Polynomial::SolveQuartic(coeff, sol);
        assert(n >= 2);
        VerifyQuarticRoots(coeff, sol, n);
    }

    std::cout << "SolveQuartic Passed!" << std::endl;
}

void TestBessi0() {
    std::cout << "Testing bessi0..." << std::endl;

    // I0(0) = 1
    assert(IsClose(Polynomial::bessi0(0.0), 1.0, 1e-6));

    // I0 is even: I0(-x) = I0(x)
    assert(IsClose(Polynomial::bessi0(2.0), Polynomial::bessi0(-2.0), 1e-10));
    assert(IsClose(Polynomial::bessi0(5.0), Polynomial::bessi0(-5.0), 1e-10));

    // I0(x) >= 1 for all x
    assert(Polynomial::bessi0(0.5) >= 1.0);
    assert(Polynomial::bessi0(1.0) >= 1.0);
    assert(Polynomial::bessi0(3.0) >= 1.0);
    assert(Polynomial::bessi0(10.0) >= 1.0);

    // I0 is monotonically increasing for x > 0
    assert(Polynomial::bessi0(1.0) < Polynomial::bessi0(2.0));
    assert(Polynomial::bessi0(2.0) < Polynomial::bessi0(3.0));
    assert(Polynomial::bessi0(3.0) < Polynomial::bessi0(4.0));

    // Cross the branch boundary at 3.75
    Scalar below = Polynomial::bessi0(3.74);
    Scalar above = Polynomial::bessi0(3.76);
    assert(below < above);
    // Continuity check: they should be close
    assert(IsClose(below, above, 0.5));

    // Known value: I0(1) ~ 1.2660658
    assert(IsClose(Polynomial::bessi0(1.0), 1.2660658, 1e-4));

    std::cout << "bessi0 Passed!" << std::endl;
}

// DL226: explicit checks remain live even when legacy assert() is disabled.
// Oracles are factored polynomials and the torus cross-section, independent
// of the quartic solver and its internal factor/error selection.
static int quarticChecks = 0, quarticFailures = 0;
static void QuarticCheck( bool ok, const char* contract )
{
    ++quarticChecks;
    if( !ok ) { ++quarticFailures; std::cout << "FAIL DL226: " << contract << '\n'; }
}

static std::array<double,5> MultiplyQuadratics( double a, double b, double c,
                                              double d, double e, double f )
{
    return {{a*d, a*e+b*d, a*f+b*e+c*d, b*f+c*e, c*f}};
}

static void CheckQuarticOracle( const char* name, const std::array<double,5>& c,
                               const std::vector<double>& expected, double rootTolerance=1e-7, bool relative=false )
{
    const bool finiteInput=std::all_of(c.begin(),c.end(),[](double x){return std::isfinite(x);});
    QuarticCheck(finiteInput,"independent oracle fixture has finite coefficients");
    if(!finiteInput) return;
    const Scalar coeff[5]={c[0],c[1],c[2],c[3],c[4]};
    Scalar roots[4]={0,0,0,0};
    const int n=Polynomial::SolveQuartic(coeff,roots);
    std::cout << std::setprecision(17) << "QUARTIC case=" << name << " coefficients=";
    for( double x:c ) std::cout << x << ',';
    std::cout << " n=" << n << " roots=";
    for( int i=0;i<n && i<4;++i ) std::cout << roots[i] << ',';
    std::cout << '\n';
    QuarticCheck(n>=0 && n<=4,"root count fits public output");
    if(n<0 || n>4) return;
    QuarticCheck(expected.empty() ? n==0 : n>=int(expected.size()),"all expected real roots, no invented positive-polynomial roots");
    for( int i=0;i<n;++i ) {
        bool known=false;
        for(double x:expected) known |= std::fabs(roots[i]-x)<=(relative ? rootTolerance*std::fabs(x) : rootTolerance);
        QuarticCheck(std::isfinite(roots[i]) && known,"every returned root matches the independent factor oracle");
        // Diagnostic residual only; it never decides which roots to retain.
        long double v=c[0], scale=std::fabs(c[0]);
        for(int j=1;j<5;++j) { v=v*roots[i]+c[j]; scale=scale*std::fabs(roots[i])+std::fabs(c[j]); }
        std::cout << "ROOT residual=" << v << " normalized=" << (scale ? std::fabs(v)/scale : 0) << '\n';
    }
    for(double x:expected) {
        bool found=false;
        for(int i=0;i<n;++i) found |= std::fabs(roots[i]-x)<=(relative ? rootTolerance*std::fabs(x) : rootTolerance);
        QuarticCheck(found,"each independent root is represented");
    }
}

static void TestQuarticClassificationAndCallers()
{
    CheckQuarticOracle("positive_square",{{1,0,2,0,1}},{});
    CheckQuarticOracle("positive_even_external_shape",{{.664615,0,1,0,.165264}},{});
    for(double b:{0.0,0.25,1.0,2.0,4.0,16.0})
        for(double d:{0.0625,1.0,16.0})
            CheckQuarticOracle("strictly_positive_even",{{1,0,b,0,d}},{});
    for(double shift:{-3.0,0.0,2.0}) {
        // ((x-shift)^2+1)^2 > 0, including translated/non-even cases.
        const auto c=MultiplyQuadratics(1,-2*shift,shift*shift+1,1,-2*shift,shift*shift+1);
        CheckQuarticOracle("translated_positive_square",c,{});
    }
    for(int exponent:{-600,-200,0,200,600}) {
        const double scale=std::ldexp(1.0,exponent);
        for(double sign:{-1.0,1.0}) {
            CheckQuarticOracle("common_scale_no_real",{{sign*scale,0,2*sign*scale,0,sign*scale}},{});
            CheckQuarticOracle("common_scale_four_real",{{sign*scale,0,-5*sign*scale,0,4*sign*scale}},{-2,-1,1,2});
        }
    }
    for(int exponent:{-100,-20,0,20,100}) {
        const double r=std::ldexp(1.0,exponent), r2=r*r;
        CheckQuarticOracle("variable_scale_four_real",{{1,0,-5*r2,0,4*r2*r2}},{-2*r,-r,r,2*r},r*1e-8);
    }
    // DL273–275: independently derived exact tangencies and exponent boundaries.
    for(double scale:{0.125,1.0,8.0}) {
        CheckQuarticOracle("translated_two_double",MultiplyQuadratics(1,-6*scale,8*scale*scale,1,-6*scale,8*scale*scale),{2*scale,4*scale},1e-10,true);
        for(double offset:{-0x1p-20,0.0,0x1p-20}) {
            const double R=scale,r=.25*R,y=r+offset*R;
            HIT hit; RayTorusIntersection(Ray(Point3(-3*R,y,0),Vector3(1,0,0)),hit,R,r,R*R);
            std::cout << "TANGENT R=" << R << " offset=" << offset << " hit=" << hit.bHit << " range=" << hit.dRange << '\n';
            QuarticCheck(hit.bHit==(offset<=0),"exact torus tangent/inside hits, outside misses");
            if(offset<=0) {
                const double chord=std::sqrt(r*r-y*y);
                QuarticCheck(hit.bHit && std::fabs(hit.dRange-(2*R-chord))<R*1e-8,"torus tangent range matches circle oracle");
            }
        }
    }
    for(int exponent:{-250,-200,-150,150,200,250}) {
        const double r=std::ldexp(1.0,exponent), r2=r*r;
        CheckQuarticOracle("extreme_variable_even",{{1,0,-5*r2,0,4*r2*r2}},{-2*r,-r,r,2*r},1e-8,true);
        CheckQuarticOracle("extreme_variable_odd",{{1,-10*r,35*r2,-50*r2*r,24*r2*r2}},{r,2*r,3*r,4*r},1e-8,true);
    }
    for(int exponent:{-1022,-1000,-900}) {
        const double a=std::ldexp(1.0,exponent),big=2/std::sqrt(a);
        CheckQuarticOracle("normalization_boundary_even",{{a,0,-4,0,4}},{-big,-1,1,big},1e-12,true);
        // (a*x²-4)(x²-x-1), stored rounded coefficient of x² is -4.
        const double g=(1+std::sqrt(5.0))/2;
        CheckQuarticOracle("normalization_boundary_odd",{{a,-a,-4,4,4}},{-big,1-g,g,big},1e-12,true);
    }
    for(int exponent:{400,500,600,800,1000}) {
        const double h=std::ldexp(1.0,exponent);
        // Rounded expansion of (x-h)(x-1)(x-2)(x-3). Dropped input
        // corrections are O(1/h), far below the relative oracle band.
        CheckQuarticOracle("wide_root_spread",{{1,-h,6*h,-11*h,6*h}},{1,2,3,h},1e-10,true);
        CheckQuarticOracle("reciprocal_wide_spread",{{6*h,-11*h,6*h,-h,1}},{1/h,1.0/3,.5,1},1e-10,true);
    }
    for(int exponent:{-260,-255,254}) {
        const double r=std::ldexp(1.0,exponent),r2=r*r;
        CheckQuarticOracle("representable_scale_edge",{{1,-10*r,35*r2,-50*r2*r,24*r2*r2}},{r,2*r,3*r,4*r},1e-8,true);
    }
    for(int exponent:{-20,-40,-45}) {
        const double delta=std::ldexp(1.0,exponent),q=std::sqrt(delta);
        CheckQuarticOracle("positive_near_two_double",{{1,-12,52,-96,64+delta}},{});
        const double outer=std::sqrt(1+q),inner=std::sqrt(1-q);
        CheckQuarticOracle("four_real_near_two_double",{{1,-12,52,-96,64-delta}},
            {3-outer,3-inner,3+inner,3+outer},1e-7);
    }
    CheckQuarticOracle("fourfold_zero",{{1,0,0,0,0}},{0});
    CheckQuarticOracle("fourfold_one",{{1,-4,6,-4,1}},{1},1e-6);
    // Equal-error factor candidates must not split an exactly squared
    // quadratic. Dyadic coefficients keep these identities exact as inputs.
    for(double r:{-4.0,-0.5,0.25,1.0,2.0}) {
        CheckQuarticOracle("squared_repeated_real",MultiplyQuadratics(1,-2*r,r*r,1,-2*r,r*r),{r},1e-6);
        CheckQuarticOracle("squared_positive",MultiplyQuadratics(1,-2*r,r*r+.25,1,-2*r,r*r+.25),{});
    }
    // These stored coefficients are exactly (x-1)^4 + delta > 0;
    // unlike expanding a tiny complex-pair square, delta is not rounded away.
    for(int exponent:{-4,-20,-40,-48})
        CheckQuarticOracle("positive_near_fourfold",{{1,-4,6,-4,1+std::ldexp(1.0,exponent)}},{});
    CheckQuarticOracle("two_double_real",{{1,0,-2,0,1}},{-1,1});
    CheckQuarticOracle("zero_and_triple",{{1,-1,0,0,0}},{0,1});
    for(double r:{-4.0,-1.0,0.0,0.25,2.0})
        CheckQuarticOracle("double_real_complex_pair",MultiplyQuadratics(1,-2*r,r*r,1,0,1),{r},1e-6);
    for(int exponent:{-4,-12,-24}) {
        const double delta=std::ldexp(1.0,exponent);
        CheckQuarticOracle("near_real_complex_pair",MultiplyQuadratics(1,0,delta*delta,1,-3,2),{1,2});
    }
    CheckQuarticOracle("cubic_dispatch",{{0,1,-6,11,-6}},{1,2,3});
    CheckQuarticOracle("quadratic_dispatch",{{0,0,2,-6,4}},{1,2});
    CheckQuarticOracle("linear_dispatch",{{0,0,0,3,-6}},{2});
    CheckQuarticOracle("constant_dispatch",{{0,0,0,0,2}},{});


    for(double R:{0.125,1.0,8.0}) for(double ratio:{0.125,0.25,0.5}) {
        const double r=R*ratio;
        for(double radial:{0.0,0.25,0.5}) {
            // Radial distance<R-r: the entire y-parallel line misses.
            const double x=radial*(R-r);
            HIT hit; RayTorusIntersection(Ray(Point3(x,-2*R,0),Vector3(0,1,0)),hit,R,r,R*R);
            std::cout << "TORUS hole R=" << R << " r=" << r << " x=" << x
                      << " hit=" << hit.bHit << " near=" << hit.dRange << '\n';
            QuarticCheck(!hit.bHit,"axial line wholly inside torus hole must miss");
        }
        HIT axis; RayTorusIntersection(Ray(Point3(0,0,0),Vector3(0,1,0)),axis,R,r,R*R);
        QuarticCheck(!axis.bHit,"origin-centered axial torus ray must miss");
        for(double height:{0.0,0.5,0.99}) {
            const double y=height*r, chord=std::sqrt(r*r-y*y), L=3*R;
            HIT hit; RayTorusIntersection(Ray(Point3(-L,y,0),Vector3(1,0,0)),hit,R,r,R*R);
            QuarticCheck(hit.bHit,"four-crossing torus ray, including grazing, must hit");
            if(hit.bHit) {
                QuarticCheck(std::fabs(hit.dRange-(L-R-chord))<1e-8*R,"torus first entry matches circle cross-section");
                QuarticCheck(std::fabs(hit.dRange2-(L-R+chord))<1e-8*R,"torus first exit matches circle cross-section");
            }
        }
    }
    // Exercise actual Object inverse transforms and both primary/shadow paths.
    for(double angle:{0.0,0.37,1.11}) {
        auto* geometry=new Implementation::TorusGeometry(1.0,.25);
        auto* object=new Implementation::Object(geometry); geometry->release();
        object->SetOrientation(Vector3(angle,angle*.7,-angle*.3));
        object->TranslateObject(Vector3(3,-5,7)); object->FinalizeTransformations();
        const Matrix4 transform=object->GetFinalTransformMatrix();
        auto worldRay=[&](const Point3& o,const Vector3& d) {
            return Ray(Point3Ops::Transform(transform,o),Vector3Ops::Transform(transform,d));
        };
        const Ray hole=worldRay(Point3(0,0,0),Vector3(0,1,0));
        RayIntersection miss(hole,nullRasterizerState);
        object->IntersectRay(miss,RISE_INFINITY,true,true,true);
        QuarticCheck(!miss.geometric.bHit,"translated rotated torus primary axial ray misses");
        QuarticCheck(!object->IntersectRay_IntersectionOnly(hole,100,true,true),"translated rotated torus shadow axial ray misses");
        for(double height:{0.0,.125,.249}) {
            const double chord=std::sqrt(.0625-height*height);
            const Ray ray=worldRay(Point3(-3,height,0),Vector3(1,0,0));
            RayIntersection hit(ray,nullRasterizerState);
            object->IntersectRay(hit,RISE_INFINITY,true,true,true);
            QuarticCheck(hit.geometric.bHit,"rotated translated grazing primary retains real hit");
            QuarticCheck(object->IntersectRay_IntersectionOnly(ray,100,true,true),"rotated translated grazing shadow retains real hit");
            if(hit.geometric.bHit) {
                QuarticCheck(std::fabs(hit.geometric.range-(2-chord))<1e-8,"rigid torus first range matches circle oracle");
                QuarticCheck(std::fabs(hit.geometric.range2-(2+chord))<1e-8,"rigid torus exit range matches circle oracle");
            }
        }
        object->release();
    }
    std::cout << "DL226 checks=" << quarticChecks << " failures=" << quarticFailures << '\n';
}

int main() {
    TestQuarticClassificationAndCallers();
    TestSolveQuadric();
    TestSolveQuadricWithinRange();
    TestSolveCubic();
    TestSolveQuartic();
    TestBessi0();
    std::cout << "All Polynomial tests passed!" << std::endl;
    return quarticFailures ? 1 : 0;
}
