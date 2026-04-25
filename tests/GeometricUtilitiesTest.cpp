#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/GeometricUtilities.h"

using namespace RISE;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-6) {
    return std::fabs(a - b) < epsilon;
}

bool Vec3Close(const Vector3& a, const Vector3& b, Scalar epsilon = 1e-6) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon) && IsClose(a.z, b.z, epsilon);
}

bool Pt3Close(const Point3& a, const Point3& b, Scalar epsilon = 1e-6) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon) && IsClose(a.z, b.z, epsilon);
}

bool Pt2Close(const Point2& a, const Point2& b, Scalar epsilon = 1e-6) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon);
}

// ==================== PointOnDisk Tests ====================

void TestPointOnDisk() {
    std::cout << "Testing PointOnDisk..." << std::endl;

    // Center of disk: uv=(0.5, 0.5) maps to center
    {
        Point2 p = GeometricUtilities::PointOnDisk(1.0, Point2(0.5, 0.5));
        assert(std::isfinite(p.x));
        assert(std::isfinite(p.y));
        assert(IsClose(p.x, 0.0, 1e-4));
        assert(IsClose(p.y, 0.0, 1e-4));
    }

    // Corner of canonical square: uv=(0, 0) should not hit a singularity
    {
        Point2 p = GeometricUtilities::PointOnDisk(1.0, Point2(0.0, 0.0));
        assert(std::isfinite(p.x));
        assert(std::isfinite(p.y));
        Scalar dist = std::sqrt(p.x * p.x + p.y * p.y);
        assert(dist <= 1.0 + 1e-6);
    }

    // All generated points should be within the disk radius
    {
        Scalar R = 2.5;
        for (int i = 0; i <= 10; i++) {
            for (int j = 0; j <= 10; j++) {
                Scalar u = i / 10.0;
                Scalar v = j / 10.0;
                Point2 p = GeometricUtilities::PointOnDisk(R, Point2(u, v));
                Scalar dist = std::sqrt(p.x * p.x + p.y * p.y);
                assert(dist <= R + 1e-6);
            }
        }
    }

    // Radius 0: all points map to origin
    {
        Point2 p = GeometricUtilities::PointOnDisk(0.0, Point2(0.3, 0.7));
        assert(IsClose(p.x, 0.0, 1e-10));
        assert(IsClose(p.y, 0.0, 1e-10));
    }

    std::cout << "PointOnDisk Passed!" << std::endl;
}

// ==================== PointOnSphere Tests ====================

void TestPointOnSphere() {
    std::cout << "Testing PointOnSphere..." << std::endl;

    Point3 center(0, 0, 0);
    Scalar radius = 1.0;

    // All generated points should be on the sphere surface
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= 10; j++) {
            Scalar u = i / 10.0;
            Scalar v = j / 10.0;
            Point3 p = GeometricUtilities::PointOnSphere(center, radius, Point2(u, v));
            Scalar dist = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
            assert(IsClose(dist, radius, 1e-6));
        }
    }

    // Non-zero center
    {
        Point3 c(10, 20, 30);
        Scalar r = 5.0;
        Point3 p = GeometricUtilities::PointOnSphere(c, r, Point2(0.3, 0.7));
        Scalar dist = Point3Ops::Distance(p, c);
        assert(IsClose(dist, r, 1e-6));
    }

    // Poles: coord.x=0 => costheta=1, north pole; coord.x=1 => costheta=-1, south pole
    {
        Point3 north = GeometricUtilities::PointOnSphere(center, radius, Point2(0, 0));
        assert(IsClose(north.z, 1.0, 1e-6));

        Point3 south = GeometricUtilities::PointOnSphere(center, radius, Point2(1, 0));
        assert(IsClose(south.z, -1.0, 1e-6));
    }

    std::cout << "PointOnSphere Passed!" << std::endl;
}

// ==================== PointOnEllipsoid Tests ====================

void TestPointOnEllipsoid() {
    std::cout << "Testing PointOnEllipsoid..." << std::endl;

    // Sphere case: equal radii should behave like sphere
    {
        Point3 center(0, 0, 0);
        Vector3 r(2, 2, 2);
        Point3 p = GeometricUtilities::PointOnEllipsoid(center, r, Point2(0.3, 0.7));
        Scalar dist = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
        assert(IsClose(dist, 2.0, 1e-6));
    }

    // Points should satisfy ellipsoid equation (x/rx)^2 + (y/ry)^2 + (z/rz)^2 = 1
    // Note: PointOnEllipsoid doesn't exactly satisfy this for non-spheres
    // since it's a parametric mapping, but all coordinates should be bounded
    {
        Point3 center(0, 0, 0);
        Vector3 r(3, 5, 2);
        for (int i = 0; i <= 10; i++) {
            for (int j = 0; j <= 10; j++) {
                Scalar u = i / 10.0;
                Scalar v = j / 10.0;
                Point3 p = GeometricUtilities::PointOnEllipsoid(center, r, Point2(u, v));
                assert(std::fabs(p.x) <= r.x + 1e-6);
                assert(std::fabs(p.y) <= r.y + 1e-6);
                assert(std::fabs(p.z) <= r.z + 1e-6);
            }
        }
    }

    std::cout << "PointOnEllipsoid Passed!" << std::endl;
}

// ==================== Spherical Coordinate Conversion Tests ====================

void TestSphericalCoordinates() {
    std::cout << "Testing Spherical Coordinates..." << std::endl;

    // CreatePoint3FromSpherical and GetSphericalFromPoint3 should round-trip
    {
        Scalar phi = 0.7;
        Scalar theta = 1.2;
        Point3 p = GeometricUtilities::CreatePoint3FromSpherical(phi, theta);

        Scalar phi_out, theta_out;
        bool ok = GeometricUtilities::GetSphericalFromPoint3(p, phi_out, theta_out);
        assert(ok);
        assert(IsClose(phi, phi_out, 1e-4));
        assert(IsClose(theta, theta_out, 1e-4));
    }

    // North pole: theta=0 => point at (0, 0, 1)
    {
        Point3 p = GeometricUtilities::CreatePoint3FromSpherical(0, 0);
        assert(Pt3Close(p, Point3(0, 0, 1), 1e-6));
    }

    // South pole: theta=PI => point at (0, 0, -1)
    {
        Point3 p = GeometricUtilities::CreatePoint3FromSpherical(0, PI);
        assert(Pt3Close(p, Point3(0, 0, -1), 1e-6));
    }

    // Equator phi=0: theta=PI/2 => (1, 0, 0)
    {
        Point3 p = GeometricUtilities::CreatePoint3FromSpherical(0, PI_OV_TWO);
        assert(Pt3Close(p, Point3(1, 0, 0), 1e-6));
    }

    // GetSphericalFromPoint3 fails for origin
    {
        Scalar phi, theta;
        bool ok = GeometricUtilities::GetSphericalFromPoint3(Point3(0, 0, 0), phi, theta);
        assert(ok == false);
    }

    // Radius of CreatePoint3FromSpherical should be 1
    for (int i = 0; i < 10; i++) {
        Scalar phi = i * TWO_PI / 10.0;
        Scalar theta = i * PI / 10.0;
        Point3 p = GeometricUtilities::CreatePoint3FromSpherical(phi, theta);
        Scalar r = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
        assert(IsClose(r, 1.0, 1e-6));
    }

    std::cout << "Spherical Coordinates Passed!" << std::endl;
}

// ==================== IsPointInsideSphere Tests ====================

void TestIsPointInsideSphere() {
    std::cout << "Testing IsPointInsideSphere..." << std::endl;

    Point3 center(0, 0, 0);
    Scalar radius = 5.0;

    // Origin is inside
    assert(GeometricUtilities::IsPointInsideSphere(Point3(0, 0, 0), radius, center));

    // Point well inside
    assert(GeometricUtilities::IsPointInsideSphere(Point3(1, 1, 1), radius, center));

    // Point well outside
    assert(!GeometricUtilities::IsPointInsideSphere(Point3(10, 0, 0), radius, center));

    // Point on surface (uses <, so on-surface is outside)
    assert(!GeometricUtilities::IsPointInsideSphere(Point3(5, 0, 0), radius, center));

    // Non-centered sphere
    {
        Point3 c(10, 10, 10);
        assert(GeometricUtilities::IsPointInsideSphere(Point3(10, 10, 10), 1.0, c));
        assert(!GeometricUtilities::IsPointInsideSphere(Point3(0, 0, 0), 1.0, c));
    }

    // Zero radius: only center is "inside" via boundary
    assert(!GeometricUtilities::IsPointInsideSphere(Point3(0, 0, 0), 0.0, center));

    std::cout << "IsPointInsideSphere Passed!" << std::endl;
}

// ==================== IsPointInsideBox Tests ====================

void TestIsPointInsideBox() {
    std::cout << "Testing IsPointInsideBox..." << std::endl;

    Point3 ll(-1, -1, -1);
    Point3 ur(1, 1, 1);

    // Center is inside
    assert(GeometricUtilities::IsPointInsideBox(Point3(0, 0, 0), ll, ur));

    // Point outside
    assert(!GeometricUtilities::IsPointInsideBox(Point3(2, 0, 0), ll, ur));
    assert(!GeometricUtilities::IsPointInsideBox(Point3(0, -2, 0), ll, ur));
    assert(!GeometricUtilities::IsPointInsideBox(Point3(0, 0, 5), ll, ur));

    // On face (uses strict <, so on-face is outside)
    assert(!GeometricUtilities::IsPointInsideBox(Point3(1, 0, 0), ll, ur));
    assert(!GeometricUtilities::IsPointInsideBox(Point3(-1, 0, 0), ll, ur));

    // Just inside
    assert(GeometricUtilities::IsPointInsideBox(Point3(0.999, 0.999, 0.999), ll, ur));

    // Large box
    {
        Point3 ll2(-1000, -1000, -1000);
        Point3 ur2(1000, 1000, 1000);
        assert(GeometricUtilities::IsPointInsideBox(Point3(999, -999, 0), ll2, ur2));
    }

    std::cout << "IsPointInsideBox Passed!" << std::endl;
}

// ==================== BilinearPatch Tests ====================

void TestBilinearPatch() {
    std::cout << "Testing BilinearPatch..." << std::endl;

    // Unit quad on XY plane: pts[0]=(0,0,0), pts[1]=(0,1,0), pts[2]=(1,0,0), pts[3]=(1,1,0)
    BilinearPatch patch;
    patch.pts[0] = Point3(0, 0, 0);
    patch.pts[1] = Point3(0, 1, 0);
    patch.pts[2] = Point3(1, 0, 0);
    patch.pts[3] = Point3(1, 1, 0);

    // Evaluate at corners
    assert(Pt3Close(GeometricUtilities::EvaluateBilinearPatchAt(patch, 0, 0), Point3(0, 0, 0)));
    assert(Pt3Close(GeometricUtilities::EvaluateBilinearPatchAt(patch, 0, 1), Point3(0, 1, 0)));
    assert(Pt3Close(GeometricUtilities::EvaluateBilinearPatchAt(patch, 1, 0), Point3(1, 0, 0)));
    assert(Pt3Close(GeometricUtilities::EvaluateBilinearPatchAt(patch, 1, 1), Point3(1, 1, 0)));

    // Evaluate at center
    Point3 center = GeometricUtilities::EvaluateBilinearPatchAt(patch, 0.5, 0.5);
    assert(Pt3Close(center, Point3(0.5, 0.5, 0)));

    // Normal of flat XY patch should point in Z direction
    Vector3 normal = GeometricUtilities::BilinearPatchNormalAt(patch, 0.5, 0.5);
    Vector3 nNorm = Vector3Ops::Normalize(normal);
    assert(IsClose(std::fabs(nNorm.z), 1.0, 1e-4));

    // Bounding box
    BoundingBox bb = GeometricUtilities::BilinearPatchBoundingBox(patch);
    assert(Pt3Close(bb.ll, Point3(0, 0, 0)));
    assert(Pt3Close(bb.ur, Point3(1, 1, 0)));

    std::cout << "BilinearPatch Passed!" << std::endl;
}

// ==================== SphericalPatchArea Tests ====================

void TestSphericalPatchArea() {
    std::cout << "Testing SphericalPatchArea..." << std::endl;

    // Full sphere area: 4*PI*r^2
    {
        Scalar radius = 1.0;
        Scalar area = GeometricUtilities::SphericalPatchArea(0, PI, 0, TWO_PI, radius);
        assert(IsClose(area, 4 * PI * radius * radius, 0.1)); // numerical integration tolerance
    }

    // Hemisphere: 2*PI*r^2
    {
        Scalar radius = 1.0;
        Scalar area = GeometricUtilities::SphericalPatchArea(0, PI_OV_TWO, 0, TWO_PI, radius);
        assert(IsClose(area, 2 * PI * radius * radius, 0.1));
    }

    // Zero-size patch: area should be ~0
    {
        Scalar area = GeometricUtilities::SphericalPatchArea(0.5, 0.5, 0.5, 0.5, 1.0);
        assert(IsClose(area, 0.0, 1e-6));
    }

    // Larger radius: area scales as r^2
    {
        Scalar area1 = GeometricUtilities::SphericalPatchArea(0, 0.5, 0, 0.5, 1.0);
        Scalar area2 = GeometricUtilities::SphericalPatchArea(0, 0.5, 0, 0.5, 2.0);
        assert(IsClose(area2 / area1, 4.0, 0.1));
    }

    std::cout << "SphericalPatchArea Passed!" << std::endl;
}

// ==================== PointOnCylinder Tests ====================

void TestPointOnCylinder() {
    std::cout << "Testing PointOnCylinder..." << std::endl;

    Scalar radius = 2.0;
    Scalar axisMin = -5.0;
    Scalar axisMax = 5.0;

    // Z-axis cylinder: x^2 + y^2 = r^2
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= 10; j++) {
            Scalar u = i / 10.0;
            Scalar v = j / 10.0;
            Point3 p;
            GeometricUtilities::PointOnCylinder(Point2(u, v), 'z', radius, axisMin, axisMax, p);

            Scalar radial = std::sqrt(p.x * p.x + p.y * p.y);
            assert(IsClose(radial, radius, 1e-6));
            assert(p.z >= axisMin - 1e-6 && p.z <= axisMax + 1e-6);
        }
    }

    // Y-axis cylinder
    {
        Point3 p;
        GeometricUtilities::PointOnCylinder(Point2(0.25, 0.5), 'y', radius, axisMin, axisMax, p);
        Scalar radial = std::sqrt(p.x * p.x + p.z * p.z);
        assert(IsClose(radial, radius, 1e-6));
    }

    // X-axis cylinder
    {
        Point3 p;
        GeometricUtilities::PointOnCylinder(Point2(0.25, 0.5), 'x', radius, axisMin, axisMax, p);
        Scalar radial = std::sqrt(p.y * p.y + p.z * p.z);
        assert(IsClose(radial, radius, 1e-6));
    }

    std::cout << "PointOnCylinder Passed!" << std::endl;
}

// ==================== CylinderNormal Tests ====================

void TestCylinderNormal() {
    std::cout << "Testing CylinderNormal..." << std::endl;

    // Z-axis: normal at (r, 0, z) should be unit-length (1, 0, 0)
    {
        Vector3 normal;
        GeometricUtilities::CylinderNormal(Point3(5, 0, 3), 'z', normal);
        assert(Vec3Close(normal, Vector3(1, 0, 0), 1e-6));
    }

    // Z-axis: normal at (0, r, z) should be unit-length (0, 1, 0)
    {
        Vector3 normal;
        GeometricUtilities::CylinderNormal(Point3(0, 5, 3), 'z', normal);
        assert(Vec3Close(normal, Vector3(0, 1, 0), 1e-6));
    }

    // Normal should be unit length for non-degenerate points
    {
        Vector3 normal;
        GeometricUtilities::CylinderNormal(Point3(3, 4, 7), 'z', normal);
        assert(IsClose(Vector3Ops::Magnitude(normal), 1.0, 1e-6));
    }

    // Normal should have no component along the cylinder axis
    {
        Vector3 normal;
        GeometricUtilities::CylinderNormal(Point3(3, 4, 7), 'z', normal);
        assert(IsClose(normal.z, 0.0, 1e-10));

        GeometricUtilities::CylinderNormal(Point3(3, 7, 4), 'y', normal);
        assert(IsClose(normal.y, 0.0, 1e-10));

        GeometricUtilities::CylinderNormal(Point3(7, 3, 4), 'x', normal);
        assert(IsClose(normal.x, 0.0, 1e-10));
    }

    std::cout << "CylinderNormal Passed!" << std::endl;
}

int main() {
    TestPointOnDisk();
    TestPointOnSphere();
    TestPointOnEllipsoid();
    TestSphericalCoordinates();
    TestIsPointInsideSphere();
    TestIsPointInsideBox();
    TestBilinearPatch();
    TestSphericalPatchArea();
    TestPointOnCylinder();
    TestCylinderNormal();
    std::cout << "All GeometricUtilities tests passed!" << std::endl;
    return 0;
}
