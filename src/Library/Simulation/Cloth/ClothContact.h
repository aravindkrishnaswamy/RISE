//////////////////////////////////////////////////////////////////////
//
//  ClothContact.h - Barrier contact, continuous collision detection and
//  smoothed friction for the cloth solver (simulation M3a, bake quality)
//
//  Incremental-potential contact (Li et al. 2020, with the thickness offset
//  of Li et al. 2021, "Codimensional IPC"):
//   * Every primitive pair within reach contributes kappa w b(z) with
//     z = d - xi (d the unsigned distance, xi the cloth's thickness offset)
//     and the C2 log barrier b(z) = -(z - dhat)^2 ln(z / dhat) for
//     0 < z < dhat, 0 beyond.  w is the cloth area the pair stands for (the
//     vertex's dual area; a third of the adjacent triangles' area for an edge;
//     a third of the triangle's area for a collider vertex against a cloth
//     triangle), so kappa (N/m^3) is a property of the contact, not of the
//     mesh.  z <= 0 is infeasible (infinite energy).
//   * Pairs: cloth vertex - analytic shape (plane, sphere, box, cylinder;
//     signed distance), cloth vertex - collider triangle, cloth edge -
//     collider edge, collider vertex - cloth triangle; with self-collision,
//     cloth vertex - cloth triangle and cloth edge - cloth edge (pairs sharing
//     a vertex excluded).  Distances are classified into point-point,
//     point-edge, point-plane and edge-edge pieces; each piece's squared
//     distance is the minimum over its closest-point parameters of a function
//     bilinear in positions and parameters, and its Hessian is the Schur
//     complement of the joint Hessian (exact inside the piece).
//   * Hessians: exact, or projected to positive semidefinite per pair
//     (Jacobi eigenvalues).  Self-collision pairs contribute only their
//     diagonal 3x3 blocks to the Hessian (the matrix pattern is fixed); their
//     gradient is exact, so converged solutions are unaffected.
//   * Broad phase: a uniform hash grid over the collider primitives (cell
//     about the mean collider edge or 2(xi + dhat)), queried with each cloth
//     primitive's box, swept over the Newton step and inflated by xi + dhat;
//     self pairs use a grid over the cloth's swept boxes.  Candidate lists are
//     sorted, so everything is deterministic.
//   * Continuous collision detection: additive CCD (conservative advancement
//     with the thickness offset; Li et al. 2021, Algorithm 1) on every
//     candidate pair bounds the line search so the gap never falls below
//     s = 0.1 of its value at the start of the step; analytic shapes use the
//     1-Lipschitz signed distance the same way.
//   * Friction: smoothed Coulomb, lagged (Li et al. 2020 sec. 5): per active
//     pair at the start of the step, the normal force lambda = kappa w |b'(z)|,
//     the closest-point weights and a tangent basis T are frozen; the
//     dissipative potential (mu lambda / c) F0(|T^T v_rel|) with
//     v = c (x - xtilde) the integrator's velocity, F0' = f1,
//     f1(y) = 2y/eps - y^2/eps^2 below eps (the slip speed), 1 above.
//   * Mesh colliders move rigidly (fixed, translate, rotate, oscillate);
//     within a step they sit at the step's end time.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_CONTACT_H
#define RISE_SIMULATION_CLOTH_CONTACT_H

#include "ClothModel.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

//! C2 log barrier and its first two derivatives (z in (0, dhat); 0 beyond).
double Barrier(double z, double dhat, double* first = nullptr, double* second = nullptr);

//! Squared distance between primitives with gradient (3n) and Hessian (3n x 3n,
//! row-major) over the primitive's n points; any output may be null.
double PointTriangleDistance2(const V3& p, const V3& t0, const V3& t1, const V3& t2, double* g, double* h);
double EdgeEdgeDistance2(const V3& a0, const V3& a1, const V3& b0, const V3& b1, double* g, double* h);

//! Additive CCD: the fraction t in (0, 1] of the displacements (dp per point)
//! the pair can travel while its gap d - xi stays above s times its initial
//! value; 1 when it never gets there.  kind: 0 point-triangle, 1 edge-edge.
double AdditiveCCD(int kind, const V3* x, const V3* dx, double xi, double s = 0.1);

//! Exact intersection test of a point-triangle or edge-edge pair moving
//! linearly from x0 to x1 (coplanarity cubic, then the inside test at each
//! root): true when they touch.
bool CrossesDuringStep(int kind, const V3* x0, const V3* x1);

struct ContactPair {
	enum Kind : std::uint8_t { Shape, VertexTriangle, EdgeEdge, TriangleVertex, SelfVertexTriangle, SelfEdgeEdge };
	Kind kind = Shape;
	std::uint32_t a = 0, b = 0; // Shape: cloth vertex, analytic collider; VertexTriangle: cloth vertex, collider
	                            // triangle; EdgeEdge: cloth edge, collider edge; TriangleVertex: cloth triangle,
	                            // collider vertex; SelfVertexTriangle: vertex, triangle; SelfEdgeEdge: edge, edge
	bool operator<(const ContactPair& o) const {
		return kind != o.kind ? kind < o.kind : a != o.a ? a < o.a : b < o.b;
	}
	bool operator==(const ContactPair& o) const { return kind == o.kind && a == o.a && b == o.b; }
};

//! A pair's frozen friction data (lagged at the start of a step).
struct FrictionRecord {
	int count = 0;
	std::uint32_t point[4] = {0, 0, 0, 0};
	bool cloth[4] = {false, false, false, false}; // cloth vertex, or collider vertex
	double weight[4] = {0, 0, 0, 0};              // relative velocity = sum weight_k v_k
	double tangent[6] = {0, 0, 0, 0, 0, 0};       // T: two orthonormal columns, row-major 3x2
	double normalForce = 0;                       // lambda, N
};

//! Uniform hash grid of primitive ids; a primitive covering more than 4096
//! cells goes to `oversize` (visited by every query).
struct HashGrid {
	std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells;
	std::vector<std::uint32_t> oversize;
	void Clear() { cells.clear(); oversize.clear(); }
};

//! Per-step and per-iteration contact state, owned by the Newton solve.
struct ContactFrame {
	double time = 0;                       // the colliders' time
	std::vector<double> colliderX, colliderV; // collider vertices at `time`
	std::vector<double> triangleBox;       // 6 per collider triangle at `time`
	std::vector<double> edgeBox;           // 6 per collider edge
	HashGrid gridTriangles, gridEdges, gridVertices;
	double cell = 1;
	std::vector<ContactPair> candidates;   // for the current configuration or Newton segment
	std::vector<FrictionRecord> friction;  // lagged; empty: no friction
	double frictionScale = 0;              // c: v = c (x - xTilde)
	std::vector<double> xTilde;
};

struct ContactStats {
	long long ccdCalls = 0, ccdLimited = 0;  // line searches bounded by CCD (alpha < 1)
	double smallestAlpha = 1;
};

class BarrierContact {
public:
	bool Initialize(const Sheet& sheet, const ModelConfig& config, std::string& error);
	bool Active() const { return active_; }
	bool HasMeshes() const { return !colliderV0_.empty(); }
	bool MovingColliders() const { return moving_; }
	const BarrierSettings& Settings() const { return settings_; }
	double Thickness() const { return xi_; }

	//! Colliders at time t (positions, boxes, grid).  Velocities are the chord
	//! (X(t) - X(t0))/(t - t0) when t0 < t (a step from t0: consistent with the
	//! colliders' discrete motion), else the instantaneous ones.
	void Place(double t, ContactFrame& f, double t0 = -1) const;
	//! Candidate pairs for x (dx null) or for the segment x -> x + dx, within xi + dhat.
	void Candidates(ContactFrame& f, const std::vector<double>& x, const std::vector<double>* dx) const;
	//! Freezes friction at x (with f.candidates for x).
	void LagFriction(ContactFrame& f, const std::vector<double>& x, double scale, const std::vector<double>& xTilde) const;
	//! Barrier (and friction) energy on f.candidates; +infinity when a pair is at or inside the offset.
	double Evaluate(const ContactFrame& f, const std::vector<double>& x, std::vector<double>* gradient,
		BlockMatrix* hessian, bool project, const std::vector<char>& fixed) const;
	//! Largest fraction of dx (<= 1) the CCD allows, over f.candidates built for the segment.
	double MaxStep(const ContactFrame& f, const std::vector<double>& x, const std::vector<double>& dx,
		ContactStats* stats = nullptr) const;
	//! Smallest gap d - xi over f.candidates (infinity when none).
	double SmallestGap(const ContactFrame& f, const std::vector<double>& x) const;
	//! Sum of the barrier forces on the cloth (N) at x over f.candidates.
	V3 TotalForce(const ContactFrame& f, const std::vector<double>& x) const;
	//! Exact check that no cloth-collider (and, with self-collision, cloth-cloth)
	//! pair touches on the straight path x0 -> x1 with the colliders of f
	//! (`skipMoving`: pairs with moving mesh colliders are left out).
	bool IntersectionFree(ContactFrame& f, const std::vector<double>& x0, const std::vector<double>& x1,
		std::string& detail, bool skipMoving = false) const;
	//! Moves x rigidly with the mesh colliders from t0 to t1 where a vertex is
	//! near a moving collider at t0 (each vertex with its nearest one; the start
	//! of a step); false when no vertex needed it.
	bool Carry(const std::vector<double>& x, double t0, double t1, std::vector<double>& out) const;

private:
	struct PairGeometry;
	bool Points(const ContactFrame& f, const std::vector<double>& x, const ContactPair& p, V3* xs,
		std::uint32_t* ids, bool* cloth, int& kind) const;
	double Weight(const ContactPair& p) const;

	bool active_ = false, moving_ = false;
	BarrierSettings settings_;
	double xi_ = 0.002;
	std::vector<Collider> shapes_;
	// Cloth topology.
	std::uint32_t vertices_ = 0;
	std::vector<std::uint32_t> triangles_;   // 3 per cloth triangle
	std::vector<std::uint32_t> edges_;       // 2 per cloth edge
	std::vector<double> vertexWeight_, edgeWeight_, triangleWeight_;
	// Colliders (all meshes concatenated).
	std::vector<double> colliderV0_;         // rest positions
	std::vector<std::uint32_t> colliderMesh_; // per vertex: mesh index
	std::vector<std::uint32_t> colliderTriangles_, colliderEdges_;
	std::vector<Motion> motions_;
	double meanEdge_ = 0;
};

} } }

#endif
