#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <algorithm>
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

int main() {
    TestSolveQuadric();
    TestSolveQuadricWithinRange();
    TestSolveCubic();
    TestSolveQuartic();
    TestBessi0();
    std::cout << "All Polynomial tests passed!" << std::endl;
    return 0;
}
