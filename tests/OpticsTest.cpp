#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-5) {
    return std::fabs(a - b) < epsilon;
}

bool IsVectorClose(const Vector3& a, const Vector3& b, Scalar epsilon = 1e-5) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon) && IsClose(a.z, b.z, epsilon);
}

Scalar Pi() {
    return std::acos(-1.0);
}

Scalar Clamp(Scalar v, Scalar lo, Scalar hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void TestCalculateReflectedRay() {
    std::cout << "Testing CalculateReflectedRay..." << std::endl;

    // Case 1: Normal incidence
    // Incoming ray straight down (0, -1, 0)
    // Normal (0, 1, 0)
    // Reflected should be (0, 1, 0)
    {
        Vector3 vIn(0, -1, 0);
        Vector3 vNormal(0, 1, 0);
        Vector3 vReflected = Optics::CalculateReflectedRay(vIn, vNormal);
        // Formula: R = I - 2(N.I)N
        // I=(0,-1,0), N=(0,1,0). N.I = -1.
        // R = (0,-1,0) - 2(-1)(0,1,0) = (0,-1,0) + (0,2,0) = (0,1,0).
        assert(IsVectorClose(vReflected, Vector3(0, 1, 0)));
    }

    // Case 2: 45 degrees
    // Incoming (1, -1, 0) normalized? No, function doesn't require normalization but usually rays are.
    // Let's normalize inputs.
    {
        Vector3 vIn(1, -1, 0);
        Vector3Ops::NormalizeMag(vIn);
        Vector3 vNormal(0, 1, 0);
        Vector3 vReflected = Optics::CalculateReflectedRay(vIn, vNormal);
        
        Vector3 expected(1, 1, 0);
        Vector3Ops::NormalizeMag(expected);
        
        assert(IsVectorClose(vReflected, expected));
    }

    // Case 3: Glancing angle
    // Incoming (1, -0.1, 0)
    {
        Vector3 vIn(1, -0.1, 0);
        Vector3Ops::NormalizeMag(vIn);
        Vector3 vNormal(0, 1, 0);
        Vector3 vReflected = Optics::CalculateReflectedRay(vIn, vNormal);
        
        Vector3 expected(1, 0.1, 0);
        Vector3Ops::NormalizeMag(expected);
        
        assert(IsVectorClose(vReflected, expected));
    }

    std::cout << "CalculateReflectedRay Passed!" << std::endl;
}

void TestCalculateRefractedRay() {
    std::cout << "Testing CalculateRefractedRay..." << std::endl;

    // Case 1: Normal incidence, no index change (Ni=1, Nt=1)
    // Should go straight through.
    {
        Vector3 vIn(0, -1, 0); // Towards surface
        Vector3 vNormal(0, 1, 0);
        Vector3 vRefracted = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, 1.0, 1.0, vRefracted);
        
        assert(result == true);
        assert(IsVectorClose(vRefracted, vIn));
    }

    // Case 2: Normal incidence, entering denser medium (Ni=1, Nt=1.5)
    // Should still go straight through.
    {
        Vector3 vIn(0, -1, 0);
        Vector3 vNormal(0, 1, 0);
        Vector3 vRefracted = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, 1.0, 1.5, vRefracted);
        
        assert(result == true);
        assert(IsVectorClose(vRefracted, vIn));
    }

    // Case 3: Total Internal Reflection
    // Going from dense to rare (Ni=1.5, Nt=1.0)
    // Critical angle: asin(1/1.5) = 41.8 degrees.
    // Incident at 45 degrees (greater than critical).
    {
        Vector3 vIn(1, -1, 0);
        Vector3Ops::NormalizeMag(vIn); // 45 degrees from normal
        Vector3 vNormal(0, 1, 0);
        Vector3 vRefracted = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, 1.5, 1.0, vRefracted);
        
        assert(result == false); // Should be TIR
    }

    // Case 4: Refraction
    // Ni=1, Nt=sqrt(3) (~1.732)
    // Incident at 60 degrees from normal. sin(60) = sqrt(3)/2.
    // Snell: 1 * sin(60) = sqrt(3) * sin(t)
    // sqrt(3)/2 = sqrt(3) * sin(t) => sin(t) = 0.5 => t = 30 degrees.
    // vIn = (sin60, -cos60, 0) = (sqrt(3)/2, -0.5, 0)
    // vRefracted should be (sin30, -cos30, 0) = (0.5, -sqrt(3)/2, 0)
    {
        Scalar sqrt3 = std::sqrt(3.0);
        Vector3 vIn(sqrt3/2.0, -0.5, 0);
        Vector3 vNormal(0, 1, 0);
        Vector3 vRefracted = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, 1.0, sqrt3, vRefracted);
        
        assert(result == true);
        Vector3 expected(0.5, -sqrt3/2.0, 0);
        assert(IsVectorClose(vRefracted, expected));
    }

    std::cout << "CalculateRefractedRay Passed!" << std::endl;
}

void TestCalculateRefractedRayBoundariesAndInvariants() {
    std::cout << "Testing CalculateRefractedRay boundaries/invariants..." << std::endl;

    // Case 1: Equal IOR should preserve direction for arbitrary incidence.
    {
        Vector3 vIn(0.3, -0.9539392014, 0.0); // normalized
        Vector3 vNormal(0, 1, 0);
        Vector3 vRefracted = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, 1.0, 1.0, vRefracted);
        assert(result == true);
        assert(IsVectorClose(vRefracted, vIn));
    }

    // Case 2: At critical angle (dense -> rare), behavior should be TIR due to NEARZERO tolerance.
    {
        Scalar n1 = 1.5;
        Scalar n2 = 1.0;
        Scalar thetaC = std::asin(n2 / n1);
        Vector3 vIn(std::sin(thetaC), -std::cos(thetaC), 0);
        Vector3 vNormal(0, 1, 0);
        Vector3 original = vIn;
        bool result = Optics::CalculateRefractedRay(vNormal, n1, n2, vIn);
        assert(result == false);
        // On failure, API should not mutate input direction.
        assert(IsVectorClose(vIn, original));
    }

    // Case 3: Slightly below critical angle should refract and remain unit length.
    {
        Scalar n1 = 1.5;
        Scalar n2 = 1.0;
        Scalar theta = std::asin(n2 / n1) - 1e-4;
        Vector3 vIn(std::sin(theta), -std::cos(theta), 0);
        Vector3 vNormal(0, 1, 0);
        bool result = Optics::CalculateRefractedRay(vNormal, n1, n2, vIn);
        assert(result == true);
        assert(IsClose(Vector3Ops::Magnitude(vIn), 1.0, 1e-5));
        // Refracted ray should continue away from the interface normal direction (into second medium).
        assert(Vector3Ops::Dot(vIn, vNormal) <= 0.0);
    }

    std::cout << "CalculateRefractedRay boundaries/invariants Passed!" << std::endl;
}

void TestCalculateDielectricReflectance() {
    std::cout << "Testing CalculateDielectricReflectance..." << std::endl;

    // Case 1: Normal incidence
    // R = ((n1-n2)/(n1+n2))^2
    {
        Scalar n1 = 1.0;
        Scalar n2 = 1.5;
        Vector3 v(0, -1, 0);
        Vector3 tv(0, -1, 0); // Transmitted is same direction
        Vector3 n(0, 1, 0);
        
        Scalar R = Optics::CalculateDielectricReflectance(v, tv, n, n1, n2);
        Scalar expected = std::pow((n1 - n2) / (n1 + n2), 2); // (-0.5/2.5)^2 = (-0.2)^2 = 0.04
        
        assert(IsClose(R, expected));
    }

    // Case 2: Brewster's Angle
    // n1=1, n2=1.5
    // tan(theta_B) = 1.5. theta_B = 56.31 degrees.
    // At Brewster angle, Rp = 0. Rs is non-zero.
    // R = 0.5 * (Rs + Rp) = 0.5 * Rs.
    {
        Scalar n1 = 1.0;
        Scalar n2 = 1.5;
        Scalar thetaB = std::atan(n2/n1);
        Scalar sinB = std::sin(thetaB);
        Scalar cosB = std::cos(thetaB);
        
        // Incident vector
        Vector3 v(sinB, -cosB, 0);
        Vector3 n(0, 1, 0);
        
        // Calculate transmitted vector
        // n1 sinB = n2 sinT
        // sinT = (n1/n2) sinB = (n1/n2) * (n2/sqrt(n1^2+n2^2)) = n1/sqrt... = cosB
        // So T = 90 - B.
        Scalar thetaT = std::asin((n1/n2) * sinB);
        Vector3 tv(std::sin(thetaT), -std::cos(thetaT), 0);
        
        Scalar R = Optics::CalculateDielectricReflectance(v, tv, n, n1, n2);
        
        // Calculate expected Rs
        // Rs = ((n1 cosB - n2 cosT) / (n1 cosB + n2 cosT))^2
        Scalar cosT = std::cos(thetaT);
        Scalar Rs_num = n1 * cosB - n2 * cosT;
        Scalar Rs_den = n1 * cosB + n2 * cosT;
        Scalar Rs = (Rs_num * Rs_num) / (Rs_den * Rs_den);
        
        Scalar Rp = 0.0; // By definition
        Scalar expected = 0.5 * (Rs + Rp);
        
        assert(IsClose(R, expected));
    }

    // Case 3: Total Internal Reflection boundary
    // As we approach critical angle, R should approach 1.0.
    // n1=1.5, n2=1.0. Critical angle ~41.8 deg.
    // Test at critical angle.
    {
        Scalar n1 = 1.5;
        Scalar n2 = 1.0;
        Scalar thetaC = std::asin(n2/n1);
        
        Vector3 v(std::sin(thetaC), -std::cos(thetaC), 0);
        Vector3 n(0, 1, 0);
        // Transmitted ray is parallel to surface (90 degrees)
        Vector3 tv(1, 0, 0); 
        
        Scalar R = Optics::CalculateDielectricReflectance(v, tv, n, n1, n2);
        assert(IsClose(R, 1.0));
    }

    std::cout << "CalculateDielectricReflectance Passed!" << std::endl;
}

void TestCalculateDielectricReflectanceBoundariesAndRange() {
    std::cout << "Testing CalculateDielectricReflectance boundaries/range..." << std::endl;

    // Case 1: Matched IOR should yield zero reflectance at any incidence.
    {
        Scalar n = 1.33;
        Vector3 normal(0, 1, 0);
        for (int i = 0; i <= 80; i += 10) {
            Scalar theta = (Scalar(i) * Pi()) / 180.0;
            Vector3 v(std::sin(theta), -std::cos(theta), 0);
            Vector3 tv = v;
            Scalar R = Optics::CalculateDielectricReflectance(v, tv, normal, n, n);
            assert(IsClose(R, 0.0, 1e-6));
        }
    }

    // Case 2: Reflectance should stay in [0, 1] for typical air->glass cases.
    {
        Scalar n1 = 1.0;
        Scalar n2 = 1.5;
        Vector3 normal(0, 1, 0);
        for (int i = 0; i <= 85; i += 5) {
            Scalar thetaI = (Scalar(i) * Pi()) / 180.0;
            Scalar sinT = (n1 / n2) * std::sin(thetaI);
            sinT = Clamp(sinT, -1.0, 1.0);
            Scalar thetaT = std::asin(sinT);
            Vector3 v(std::sin(thetaI), -std::cos(thetaI), 0);
            Vector3 tv(std::sin(thetaT), -std::cos(thetaT), 0);
            Scalar R = Optics::CalculateDielectricReflectance(v, tv, normal, n1, n2);
            assert(R >= 0.0);
            assert(R <= 1.0);
        }
    }

    // Case 3: Grazing incidence should have higher reflectance than normal incidence.
    {
        Scalar n1 = 1.0;
        Scalar n2 = 1.5;
        Vector3 normal(0, 1, 0);
        Vector3 vNormalInc(0, -1, 0);
        Vector3 tvNormalInc(0, -1, 0);
        Scalar Rnormal = Optics::CalculateDielectricReflectance(vNormalInc, tvNormalInc, normal, n1, n2);

        Scalar thetaI = Scalar(85.0 * Pi() / 180.0);
        Scalar thetaT = std::asin((n1 / n2) * std::sin(thetaI));
        Vector3 vGrazing(std::sin(thetaI), -std::cos(thetaI), 0);
        Vector3 tvGrazing(std::sin(thetaT), -std::cos(thetaT), 0);
        Scalar Rgrazing = Optics::CalculateDielectricReflectance(vGrazing, tvGrazing, normal, n1, n2);
        assert(Rgrazing > Rnormal);
    }

    std::cout << "CalculateDielectricReflectance boundaries/range Passed!" << std::endl;
}

void TestCalculateConductorReflectanceAndSchlick() {
    std::cout << "Testing CalculateConductorReflectance and Schlick..." << std::endl;

    // Case 1: Conductor reflectance normal incidence closed form.
    {
        Scalar n1 = 1.0;
        Scalar n2 = 0.2;
        Scalar k = 3.0;
        Vector3 v(0, -1, 0);
        Vector3 n(0, 1, 0);
        Scalar R = Optics::CalculateConductorReflectance(v, n, n1, n2, k);
        Scalar expected = ((n2 - n1) * (n2 - n1) + k * k) / ((n2 + n1) * (n2 + n1) + k * k);
        assert(IsClose(R, expected, 1e-6));
        assert(R >= 0.0 && R <= 1.0);
    }

    // Case 2: Near grazing incidence should remain in [0,1] and high.
    {
        Scalar n1 = 1.0;
        Scalar n2 = 0.2;
        Scalar k = 3.0;
        Scalar theta = Scalar(89.9 * Pi() / 180.0);
        Vector3 v(std::sin(theta), -std::cos(theta), 0);
        Vector3 n(0, 1, 0);
        Scalar R = Optics::CalculateConductorReflectance(v, n, n1, n2, k);
        assert(R >= 0.0 && R <= 1.0);
        assert(R > 0.9);
    }

    // Case 3: Schlick helper sanity checks.
    {
        Scalar r = 0.04;
        Scalar atNormal = Optics::CalculateFresnelReflectanceSchlick(r, 1.0);
        Scalar atGrazing = Optics::CalculateFresnelReflectanceSchlick(r, 0.0);
        Scalar mid = Optics::CalculateFresnelReflectanceSchlick(r, 0.5);
        assert(IsClose(atNormal, r));
        assert(IsClose(atGrazing, 1.0));
        assert(mid > atNormal && mid < atGrazing);
    }

    std::cout << "CalculateConductorReflectance and Schlick Passed!" << std::endl;
}

void TestInvalidPhysicalInputs() {
    std::cout << "Testing invalid physical inputs..." << std::endl;

    // Refraction should fail with invalid IORs.
    {
        Vector3 vIn(0, -1, 0);
        Vector3 n(0, 1, 0);
        assert(Optics::CalculateRefractedRay(n, 0.0, 1.5, vIn) == false);
        assert(Optics::CalculateRefractedRay(n, 1.0, 0.0, vIn) == false);
    }

    // Refraction should fail with degenerate vectors.
    {
        Vector3 zero(0, 0, 0);
        Vector3 vIn(0, -1, 0);
        assert(Optics::CalculateRefractedRay(zero, 1.0, 1.5, vIn) == false);
        assert(Optics::CalculateRefractedRay(Vector3(0, 1, 0), 1.0, 1.5, zero) == false);
    }

    // Dielectric reflectance should clamp to 1.0 for invalid physical inputs.
    {
        Vector3 v(0, -1, 0);
        Vector3 tv(0, -1, 0);
        Vector3 n(0, 1, 0);
        assert(IsClose(Optics::CalculateDielectricReflectance(v, tv, n, 0.0, 1.5), 1.0));
        assert(IsClose(Optics::CalculateDielectricReflectance(v, tv, n, 1.0, 0.0), 1.0));
        assert(IsClose(Optics::CalculateDielectricReflectance(v, tv, Vector3(0, 0, 0), 1.0, 1.5), 1.0));
    }

    std::cout << "Invalid physical inputs Passed!" << std::endl;
}

int main() {
    TestCalculateReflectedRay();
    TestCalculateRefractedRay();
    TestCalculateRefractedRayBoundariesAndInvariants();
    TestCalculateDielectricReflectance();
    TestCalculateDielectricReflectanceBoundariesAndRange();
    TestCalculateConductorReflectanceAndSchlick();
    TestInvalidPhysicalInputs();
    return 0;
}
