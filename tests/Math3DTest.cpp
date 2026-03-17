#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;

bool IsClose(Scalar a, Scalar b, Scalar epsilon = 1e-9) {
    return std::fabs(a - b) < epsilon;
}

bool Vec3Close(const Vector3& a, const Vector3& b, Scalar epsilon = 1e-9) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon) && IsClose(a.z, b.z, epsilon);
}

bool Vec2Close(const Vector2& a, const Vector2& b, Scalar epsilon = 1e-9) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon);
}

bool Pt3Close(const Point3& a, const Point3& b, Scalar epsilon = 1e-9) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon) && IsClose(a.z, b.z, epsilon);
}

bool Pt2Close(const Point2& a, const Point2& b, Scalar epsilon = 1e-9) {
    return IsClose(a.x, b.x, epsilon) && IsClose(a.y, b.y, epsilon);
}

// ==================== Vector3 Tests ====================

void TestVector3Basics() {
    std::cout << "Testing Vector3 basics..." << std::endl;

    // Default constructor
    Vector3 v;
    assert(v.x == 0 && v.y == 0 && v.z == 0);

    // Parameterized constructor
    Vector3 a(1, 2, 3);
    assert(a.x == 1 && a.y == 2 && a.z == 3);

    // Copy constructor
    Vector3 b(a);
    assert(b.x == 1 && b.y == 2 && b.z == 3);

    // Array access
    assert(a[0] == 1 && a[1] == 2 && a[2] == 3);

    // Addition
    Vector3 c = a + Vector3(4, 5, 6);
    assert(Vec3Close(c, Vector3(5, 7, 9)));

    // Subtraction
    Vector3 d = c - a;
    assert(Vec3Close(d, Vector3(4, 5, 6)));

    // Scalar multiplication
    Vector3 e = a * 2.0;
    assert(Vec3Close(e, Vector3(2, 4, 6)));
    Vector3 f = 3.0 * a;
    assert(Vec3Close(f, Vector3(3, 6, 9)));

    // Unary negation
    Vector3 g = -a;
    assert(Vec3Close(g, Vector3(-1, -2, -3)));

    std::cout << "Vector3 basics Passed!" << std::endl;
}

void TestVector3Ops() {
    std::cout << "Testing Vector3Ops..." << std::endl;

    Vector3 a(1, 0, 0);
    Vector3 b(0, 1, 0);
    Vector3 c(0, 0, 1);

    // Dot product: orthogonal
    assert(IsClose(Vector3Ops::Dot(a, b), 0.0));
    assert(IsClose(Vector3Ops::Dot(a, c), 0.0));
    assert(IsClose(Vector3Ops::Dot(b, c), 0.0));

    // Dot product: parallel
    assert(IsClose(Vector3Ops::Dot(a, a), 1.0));

    // Dot product: anti-parallel
    assert(IsClose(Vector3Ops::Dot(a, -a), -1.0));

    // Dot product: arbitrary
    Vector3 d(3, 4, 0);
    assert(IsClose(Vector3Ops::Dot(d, d), 25.0));

    // Cross product: basis vectors
    assert(Vec3Close(Vector3Ops::Cross(a, b), c));
    assert(Vec3Close(Vector3Ops::Cross(b, c), a));
    assert(Vec3Close(Vector3Ops::Cross(c, a), b));

    // Cross product: anti-commutativity
    assert(Vec3Close(Vector3Ops::Cross(a, b), -Vector3Ops::Cross(b, a)));

    // Cross product: self is zero
    assert(Vec3Close(Vector3Ops::Cross(a, a), Vector3(0, 0, 0)));

    // Magnitude
    assert(IsClose(Vector3Ops::Magnitude(Vector3(3, 4, 0)), 5.0));
    assert(IsClose(Vector3Ops::Magnitude(Vector3(0, 0, 0)), 0.0));
    assert(IsClose(Vector3Ops::Magnitude(Vector3(1, 1, 1)), std::sqrt(3.0)));

    // SquaredModulus
    assert(IsClose(Vector3Ops::SquaredModulus(Vector3(3, 4, 0)), 25.0));

    // Normalize
    Vector3 n = Vector3Ops::Normalize(Vector3(3, 4, 0));
    assert(IsClose(Vector3Ops::Magnitude(n), 1.0));
    assert(IsClose(n.x, 0.6));
    assert(IsClose(n.y, 0.8));

    // Normalize zero vector returns zero
    Vector3 z = Vector3Ops::Normalize(Vector3(0, 0, 0));
    assert(Vec3Close(z, Vector3(0, 0, 0)));

    // NormalizeMag returns magnitude
    Vector3 nm(3, 4, 0);
    Scalar mag = Vector3Ops::NormalizeMag(nm);
    assert(IsClose(mag, 5.0));
    assert(IsClose(Vector3Ops::Magnitude(nm), 1.0));

    // mkVector3
    Point3 p1(1, 2, 3);
    Point3 p2(4, 6, 8);
    Vector3 v = Vector3Ops::mkVector3(p2, p1);
    assert(Vec3Close(v, Vector3(3, 4, 5)));

    // AreEqual
    assert(Vector3Ops::AreEqual(Vector3(1, 2, 3), Vector3(1, 2, 3), 1e-10));
    assert(!Vector3Ops::AreEqual(Vector3(1, 2, 3), Vector3(1, 2, 4), 0.5));

    // Perpendicular
    Vector3 perp = Vector3Ops::Perpendicular(Vector3(1, 0, 0));
    assert(IsClose(Vector3Ops::Dot(perp, Vector3(1, 0, 0)), 0.0));

    perp = Vector3Ops::Perpendicular(Vector3(0, 1, 0));
    assert(IsClose(Vector3Ops::Dot(perp, Vector3(0, 1, 0)), 0.0));

    perp = Vector3Ops::Perpendicular(Vector3(0, 0, 1));
    assert(IsClose(Vector3Ops::Dot(perp, Vector3(0, 0, 1)), 0.0));

    // Perpendicular for arbitrary vector
    perp = Vector3Ops::Perpendicular(Vector3(1, 2, 3));
    assert(IsClose(Vector3Ops::Dot(perp, Vector3(1, 2, 3)), 0.0));

    // IndexOfMinAbsComponent
    assert(Vector3Ops::IndexOfMinAbsComponent(Vector3(1, 2, 3)) == 0);
    assert(Vector3Ops::IndexOfMinAbsComponent(Vector3(3, 1, 2)) == 1);
    assert(Vector3Ops::IndexOfMinAbsComponent(Vector3(3, 2, 1)) == 2);

    // WeightedAverage2
    Vector3 wa = Vector3Ops::WeightedAverage2(Vector3(1, 0, 0), Vector3(0, 1, 0), 0.5, 0.5);
    assert(Vec3Close(wa, Vector3(0.5, 0.5, 0)));

    // WeightedAverage3 (2-weight version)
    Vector3 wa3 = Vector3Ops::WeightedAverage3(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), 1.0/3, 1.0/3);
    assert(Vec3Close(wa3, Vector3(1.0/3, 1.0/3, 1.0/3), 1e-6));

    std::cout << "Vector3Ops Passed!" << std::endl;
}

// ==================== Vector2 Tests ====================

void TestVector2Ops() {
    std::cout << "Testing Vector2Ops..." << std::endl;

    // Dot product
    assert(IsClose(Vector2Ops::Dot(Vector2(1, 0), Vector2(0, 1)), 0.0));
    assert(IsClose(Vector2Ops::Dot(Vector2(3, 4), Vector2(3, 4)), 25.0));

    // Magnitude
    assert(IsClose(Vector2Ops::Magnitude(Vector2(3, 4)), 5.0));

    // Normalize
    Vector2 n = Vector2Ops::Normalize(Vector2(3, 4));
    assert(IsClose(Vector2Ops::Magnitude(n), 1.0));

    // Normalize zero
    Vector2 z = Vector2Ops::Normalize(Vector2(0, 0));
    assert(Vec2Close(z, Vector2(0, 0)));

    // Perpendicular
    Vector2 perp = Vector2Ops::Perpendicular(Vector2(1, 0));
    assert(IsClose(Vector2Ops::Dot(perp, Vector2(1, 0)), 0.0));

    perp = Vector2Ops::Perpendicular(Vector2(3, 4));
    assert(IsClose(Vector2Ops::Dot(perp, Vector2(3, 4)), 0.0));

    // mkVector2
    Vector2 v = Vector2Ops::mkVector2(Point2(5, 7), Point2(2, 3));
    assert(Vec2Close(v, Vector2(3, 4)));

    std::cout << "Vector2Ops Passed!" << std::endl;
}

// ==================== Point Tests ====================

void TestPointOps() {
    std::cout << "Testing PointOps..." << std::endl;

    // Point3 distance
    assert(IsClose(Point3Ops::Distance(Point3(0, 0, 0), Point3(3, 4, 0)), 5.0));
    assert(IsClose(Point3Ops::Distance(Point3(1, 1, 1), Point3(1, 1, 1)), 0.0));

    // Point3 mkPoint3
    Point3 p = Point3Ops::mkPoint3(Point3(1, 2, 3), Vector3(4, 5, 6));
    assert(Pt3Close(p, Point3(5, 7, 9)));

    // Point3 AreEqual
    assert(Point3Ops::AreEqual(Point3(1, 2, 3), Point3(1, 2, 3), 1e-10));
    assert(!Point3Ops::AreEqual(Point3(1, 2, 3), Point3(1, 2, 4), 0.5));

    // Point3 WeightedAverage2
    Point3 avg = Point3Ops::WeightedAverage2(Point3(0, 0, 0), Point3(10, 10, 10), 0.5);
    assert(Pt3Close(avg, Point3(5, 5, 5)));

    // Point3 WeightedAverage2 at extremes
    avg = Point3Ops::WeightedAverage2(Point3(1, 0, 0), Point3(0, 1, 0), 1.0);
    assert(Pt3Close(avg, Point3(1, 0, 0)));
    avg = Point3Ops::WeightedAverage2(Point3(1, 0, 0), Point3(0, 1, 0), 0.0);
    assert(Pt3Close(avg, Point3(0, 1, 0)));

    // Point2 distance
    assert(IsClose(Point2Ops::Distance(Point2(0, 0), Point2(3, 4)), 5.0));

    // Point2 mkPoint2
    Point2 p2 = Point2Ops::mkPoint2(Point2(1, 2), Vector2(3, 4));
    assert(Pt2Close(p2, Point2(4, 6)));

    std::cout << "PointOps Passed!" << std::endl;
}

// ==================== Matrix3 Tests ====================

void TestMatrix3Ops() {
    std::cout << "Testing Matrix3Ops..." << std::endl;

    // Identity
    Matrix3 id = Matrix3Ops::Identity();
    assert(id._00 == 1 && id._11 == 1 && id._22 == 1);
    assert(id._01 == 0 && id._02 == 0 && id._10 == 0);

    // Determinant of identity
    assert(IsClose(Matrix3Ops::Determinant(id), 1.0));

    // Determinant of known matrix
    Matrix3 m(2, 0, 0,
              0, 3, 0,
              0, 0, 4);
    assert(IsClose(Matrix3Ops::Determinant(m), 24.0));

    // Inverse of diagonal matrix
    Matrix3 inv = Matrix3Ops::Inverse(m);
    assert(IsClose(inv._00, 0.5));
    assert(IsClose(inv._11, 1.0/3));
    assert(IsClose(inv._22, 0.25));

    // M * M^-1 = I
    Matrix3 prod = m * inv;
    assert(IsClose(prod._00, 1.0, 1e-9));
    assert(IsClose(prod._11, 1.0, 1e-9));
    assert(IsClose(prod._22, 1.0, 1e-9));
    assert(IsClose(prod._01, 0.0, 1e-9));

    // Transpose
    Matrix3 t(1, 2, 3,
              4, 5, 6,
              7, 8, 9);
    Matrix3 tt = Matrix3Ops::Transpose(t);
    assert(tt._00 == 1 && tt._01 == 4 && tt._02 == 7);
    assert(tt._10 == 2 && tt._11 == 5 && tt._12 == 8);
    assert(tt._20 == 3 && tt._21 == 6 && tt._22 == 9);

    // Double transpose = original
    Matrix3 ttt = Matrix3Ops::Transpose(tt);
    assert(IsClose(ttt._01, t._01));
    assert(IsClose(ttt._12, t._12));

    // Rotation: 90-degree rotation
    Matrix3 rot = Matrix3Ops::Rotation(PI / 2.0);
    Vector2 v(1, 0);
    Vector2 rv = Vector2Ops::Transform(rot, v);
    assert(Vec2Close(rv, Vector2(0, 1), 1e-9));

    // Rotation: 360 degrees returns to start
    Matrix3 rot360 = Matrix3Ops::Rotation(2 * PI);
    rv = Vector2Ops::Transform(rot360, v);
    assert(Vec2Close(rv, v, 1e-9));

    // Scale
    Matrix3 sc = Matrix3Ops::Scale2D(3.0);
    Vector2 sv = Vector2Ops::Transform(sc, Vector2(1, 2));
    assert(Vec2Close(sv, Vector2(3, 6)));

    // Singular matrix: determinant 0, inverse returns original
    Matrix3 singular(1, 2, 3,
                     4, 5, 6,
                     7, 8, 9);
    assert(IsClose(Matrix3Ops::Determinant(singular), 0.0, 1e-6));

    std::cout << "Matrix3Ops Passed!" << std::endl;
}

// ==================== Matrix4 Tests ====================

void TestMatrix4Ops() {
    std::cout << "Testing Matrix4Ops..." << std::endl;

    // Identity
    Matrix4 id = Matrix4Ops::Identity();
    assert(IsClose(Matrix4Ops::Determinant(id), 1.0));

    // Diagonal
    Matrix4 diag = Matrix4Ops::Scale(2.0);
    assert(IsClose(diag._00, 2.0));
    assert(IsClose(diag._11, 2.0));
    assert(IsClose(diag._22, 2.0));

    // Translation: translate point
    Matrix4 tr = Matrix4Ops::Translation(Vector3(10, 20, 30));
    Point3 p = Point3Ops::Transform(tr, Point3(1, 2, 3));
    assert(Pt3Close(p, Point3(11, 22, 33)));

    // Translation: vectors are unaffected (homogeneous w=0)
    Vector3 v(1, 2, 3);
    Vector3 tv = Vector3Ops::Transform(tr, v);
    assert(Vec3Close(tv, v));

    // XRotation 90 degrees: (0,1,0) -> (0,0,1)
    Matrix4 rx = Matrix4Ops::XRotation(PI / 2.0);
    Vector3 ry = Vector3Ops::Transform(rx, Vector3(0, 1, 0));
    assert(Vec3Close(ry, Vector3(0, 0, 1), 1e-9));

    // YRotation 90 degrees: (0,0,1) -> (1,0,0)
    Matrix4 rym = Matrix4Ops::YRotation(PI / 2.0);
    Vector3 rz = Vector3Ops::Transform(rym, Vector3(0, 0, 1));
    assert(Vec3Close(rz, Vector3(1, 0, 0), 1e-9));

    // ZRotation 90 degrees: (1,0,0) -> (0,1,0)
    Matrix4 rzm = Matrix4Ops::ZRotation(PI / 2.0);
    Vector3 rx2 = Vector3Ops::Transform(rzm, Vector3(1, 0, 0));
    assert(Vec3Close(rx2, Vector3(0, 1, 0), 1e-9));

    // Inverse of identity is identity
    Matrix4 inv_id = Matrix4Ops::Inverse(id);
    assert(IsClose(inv_id._00, 1.0) && IsClose(inv_id._01, 0.0));

    // M * M^-1 = I for a rotation
    Matrix4 rot = Matrix4Ops::XRotation(0.7);
    Matrix4 rot_inv = Matrix4Ops::Inverse(rot);
    Matrix4 prod = rot * rot_inv;
    assert(IsClose(prod._00, 1.0, 1e-9));
    assert(IsClose(prod._11, 1.0, 1e-9));
    assert(IsClose(prod._22, 1.0, 1e-9));
    assert(IsClose(prod._01, 0.0, 1e-9));
    assert(IsClose(prod._10, 0.0, 1e-9));

    // Transpose of transpose = original
    Matrix4 m(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Matrix4 mtt = Matrix4Ops::Transpose(Matrix4Ops::Transpose(m));
    assert(IsClose(mtt._00, m._00) && IsClose(mtt._23, m._23));

    // Arbitrary rotation: applying Rotation(axis, angle) then Rotation(axis, -angle) = identity
    Vector3 axis = Vector3Ops::Normalize(Vector3(1, 1, 1));
    Matrix4 r1 = Matrix4Ops::Rotation(axis, 1.2);
    Matrix4 r2 = Matrix4Ops::Rotation(axis, -1.2);
    Matrix4 rp = r1 * r2;
    assert(IsClose(rp._00, 1.0, 1e-6));
    assert(IsClose(rp._11, 1.0, 1e-6));
    assert(IsClose(rp._22, 1.0, 1e-6));
    assert(IsClose(rp._01, 0.0, 1e-6));

    // Stretch
    Matrix4 stretch = Matrix4Ops::Stretch(Vector3(2, 3, 4));
    Vector3 sv = Vector3Ops::Transform(stretch, Vector3(1, 1, 1));
    assert(Vec3Close(sv, Vector3(2, 3, 4)));

    std::cout << "Matrix4Ops Passed!" << std::endl;
}

// ==================== Quaternion Tests ====================

void TestQuaternionOps() {
    std::cout << "Testing QuaternionOps..." << std::endl;

    // Default constructor
    Quaternion q;
    assert(q.s == 0 && q.v.x == 0 && q.v.y == 0 && q.v.z == 0);

    // Identity quaternion (1, 0, 0, 0)
    Quaternion identity(1, Vector3(0, 0, 0));

    // Dot product: self
    assert(IsClose(QuaternionOps::Dot(identity, identity), 1.0));

    // Magnitude of identity
    assert(IsClose(QuaternionOps::Magnitude(identity), 1.0));

    // Normal (squared magnitude) of identity
    assert(IsClose(QuaternionOps::Normal(identity), 1.0));

    // Unary negation
    Quaternion neg = QuaternionOps::UnaryNegation(identity);
    assert(IsClose(neg.s, -1.0));

    // Unary conjugate: flips vector part
    Quaternion conj = QuaternionOps::UnaryConjugate(Quaternion(1, Vector3(2, 3, 4)));
    assert(IsClose(conj.s, 1.0));
    assert(Vec3Close(conj.v, Vector3(-2, -3, -4)));

    // Quaternion multiplication: identity * q = q
    Quaternion a(0.5, Vector3(0.5, 0.5, 0.5));
    Quaternion result = identity * a;
    assert(IsClose(result.s, a.s));
    assert(Vec3Close(result.v, a.v));

    // q * identity = q
    result = a * identity;
    assert(IsClose(result.s, a.s));
    assert(Vec3Close(result.v, a.v));

    // Quaternion multiplication is not commutative (in general)
    Quaternion b(0.0, Vector3(1, 0, 0));
    Quaternion c(0.0, Vector3(0, 1, 0));
    Quaternion bc = b * c;
    Quaternion cb = c * b;
    // bc and cb should differ (cross product reverses)
    assert(!Vec3Close(bc.v, cb.v));

    // Slerp: t=0 => p, t=1 => q (approximately)
    Quaternion p1(1, Vector3(0, 0, 0));
    Quaternion q1(std::cos(0.5), Vector3(std::sin(0.5), 0, 0));
    Quaternion s0 = QuaternionOps::Slerp(p1, q1, 0.0);
    assert(IsClose(s0.s, p1.s, 1e-6));

    Quaternion s1 = QuaternionOps::Slerp(p1, q1, 1.0);
    assert(IsClose(s1.s, q1.s, 1e-6));
    assert(Vec3Close(s1.v, q1.v, 1e-6));

    // Slerp midpoint should have magnitude 1
    Quaternion smid = QuaternionOps::Slerp(p1, q1, 0.5);
    assert(IsClose(QuaternionOps::Magnitude(smid), 1.0, 1e-6));

    std::cout << "QuaternionOps Passed!" << std::endl;
}

// ==================== Constants Tests ====================

void TestConstants() {
    std::cout << "Testing Constants..." << std::endl;

    assert(IsClose(PI, std::acos(-1.0), 1e-12));
    assert(IsClose(INV_PI, 1.0 / PI, 1e-12));
    assert(IsClose(TWO_PI, 2.0 * PI, 1e-12));
    assert(IsClose(PI_OV_TWO, PI / 2.0, 1e-12));
    assert(IsClose(PI_OV_FOUR, PI / 4.0, 1e-12));
    assert(IsClose(PI_OV_THREE, PI / 3.0, 1e-12));
    assert(IsClose(DEG_TO_RAD * 180.0, PI, 1e-12));
    assert(IsClose(RAD_TO_DEG * PI, 180.0, 1e-12));
    assert(IsClose(THIRD, 1.0 / 3.0, 1e-12));
    assert(NEARZERO > 0 && NEARZERO < 1e-10);

    std::cout << "Constants Passed!" << std::endl;
}

// ==================== smoothstep Tests ====================

void TestSmoothstep() {
    std::cout << "Testing smoothstep..." << std::endl;

    // Below range
    assert(IsClose(rmath::smoothstep(0.0, 1.0, -0.5), 0.0));

    // Above range
    assert(IsClose(rmath::smoothstep(0.0, 1.0, 1.5), 1.0));

    // At edges
    assert(IsClose(rmath::smoothstep(0.0, 1.0, 0.0), 0.0));
    assert(IsClose(rmath::smoothstep(0.0, 1.0, 1.0), 1.0));

    // Midpoint
    assert(IsClose(rmath::smoothstep(0.0, 1.0, 0.5), 0.5));

    // Monotonic
    Scalar prev = 0;
    for (int i = 1; i <= 10; i++) {
        Scalar t = i / 10.0;
        Scalar val = rmath::smoothstep(0.0, 1.0, t);
        assert(val >= prev);
        prev = val;
    }

    std::cout << "smoothstep Passed!" << std::endl;
}

int main() {
    TestVector3Basics();
    TestVector3Ops();
    TestVector2Ops();
    TestPointOps();
    TestMatrix3Ops();
    TestMatrix4Ops();
    TestQuaternionOps();
    TestConstants();
    TestSmoothstep();
    std::cout << "All Math3D tests passed!" << std::endl;
    return 0;
}
