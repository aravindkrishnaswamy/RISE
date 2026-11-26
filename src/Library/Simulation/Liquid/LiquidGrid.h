//////////////////////////////////////////////////////////////////////
//
//  LiquidGrid.h - The staggered (MAC) grid of the liquid solver
//  (simulation M7, docs/SIMULATION_M7A_RESULTS.md), deterministic
//  parallel loops over it, interpolation, and the static geometry
//  (solid signed-distance fields, face-area fractions, initial liquid).
//
//  Layout.  nx x ny x nz cubic cells of side h; the domain is the box
//  [origin, origin + h n].  Cell-centred arrays (level set, pressure) are
//  indexed i + nx (j + ny k).  The velocity component along axis a lives on
//  the faces normal to a: an array of (n + e_a) entries, the face with index
//  i_a sitting at origin_a + i_a h (the other two coordinates at the cell
//  centres).  The solid signed distance lives on the (nx+1)(ny+1)(nz+1)
//  cell corners (nodes); negative inside a solid.
//
//  Threads.  Every loop is split into chunks of whole rows (a row = the
//  innermost index) whose count depends only on the array dimensions; each
//  chunk writes its own entries, and reductions combine per-chunk partial
//  results in chunk order, so results are bitwise the same for any number
//  of threads (the cloth solver's ParallelPool, ClothParallel.h).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_GRID_H
#define RISE_SIMULATION_LIQUID_GRID_H

#include "../Cloth/ClothParallel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Liquid {

using Pool = Cloth::ParallelPool;
using V3 = std::array<double,3>;

struct Grid {
	std::array<int,3> n{{0, 0, 0}};
	double h = 1;
	V3 origin{{0, 0, 0}};

	std::size_t Cells() const { return std::size_t(n[0])*std::size_t(n[1])*std::size_t(n[2]); }
	std::size_t C(int i, int j, int k) const { return std::size_t(i) + std::size_t(n[0])*(std::size_t(j) + std::size_t(n[1])*std::size_t(k)); }
	//! Dimensions of the face array of axis a (n + e_a).
	std::array<int,3> FaceDims(int a) const { std::array<int,3> d = n; ++d[a]; return d; }
	std::size_t Faces(int a) const { const auto d = FaceDims(a); return std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]); }
	std::size_t F(int a, int i, int j, int k) const {
		const auto d = FaceDims(a);
		return std::size_t(i) + std::size_t(d[0])*(std::size_t(j) + std::size_t(d[1])*std::size_t(k));
	}
	std::array<int,3> NodeDims() const { return {{n[0] + 1, n[1] + 1, n[2] + 1}}; }
	std::size_t Nodes() const { return std::size_t(n[0] + 1)*std::size_t(n[1] + 1)*std::size_t(n[2] + 1); }
	std::size_t N(int i, int j, int k) const { return std::size_t(i) + std::size_t(n[0] + 1)*(std::size_t(j) + std::size_t(n[1] + 1)*std::size_t(k)); }
	V3 CellCentre(int i, int j, int k) const {
		return {{origin[0] + (i + 0.5)*h, origin[1] + (j + 0.5)*h, origin[2] + (k + 0.5)*h}};
	}
	V3 FaceCentre(int a, int i, int j, int k) const {
		V3 x = CellCentre(i, j, k);
		x[a] -= 0.5*h;
		return x;
	}
	V3 Node(int i, int j, int k) const { return {{origin[0] + i*h, origin[1] + j*h, origin[2] + k*h}}; }
};

//! A velocity field on the faces: one array per axis.
struct MacField {
	std::array<std::vector<double>,3> u;
	void Resize(const Grid& g, double value = 0) { for (int a = 0; a < 3; ++a) u[a].assign(g.Faces(a), value); }
};

// ------------------------------------------------------------------ parallel loops

//! Rows of an array of dimensions d: d[1] d[2]; rows per chunk from d[0].
inline std::size_t RowGrain(int innermost) { return std::max<std::size_t>(1, 16384/std::size_t(std::max(1, innermost))); }

//! f(row begin, row end) over the rows [0, rows) in chunks of `grain`.
template <class F>
void ForRows(Pool* pool, std::size_t rows, std::size_t grain, F&& f)
{
	const std::size_t chunks = Cloth::Chunks(rows, grain);
	Cloth::ParallelFor(pool, chunks, [&](std::size_t c) { f(c*grain, std::min(rows, (c + 1)*grain)); });
}

//! Sum of f(row begin, row end) over chunks, combined in chunk order.
template <class F>
double SumRows(Pool* pool, std::size_t rows, std::size_t grain, F&& f)
{
	const std::size_t chunks = Cloth::Chunks(rows, grain);
	std::vector<double> part(chunks, 0.0);
	Cloth::ParallelFor(pool, chunks, [&](std::size_t c) { part[c] = f(c*grain, std::min(rows, (c + 1)*grain)); });
	double s = 0;
	for (double x : part) s += x;
	return s;
}

//! Max of f(row begin, row end) over chunks (order-independent).
template <class F>
double MaxRows(Pool* pool, std::size_t rows, std::size_t grain, F&& f)
{
	const std::size_t chunks = Cloth::Chunks(rows, grain);
	std::vector<double> part(chunks, 0.0);
	Cloth::ParallelFor(pool, chunks, [&](std::size_t c) { part[c] = f(c*grain, std::min(rows, (c + 1)*grain)); });
	double s = 0;
	for (double x : part) s = std::max(s, x);
	return s;
}

//! Fixed-order inner product of two arrays of length n (chunks of 4096).
double Dot(Pool* pool, const std::vector<double>& a, const std::vector<double>& b);

// ------------------------------------------------------------------ interpolation

//! Trilinear interpolation of a cell-centred array at world point x (clamped
//! to the cell centres: constant beyond the outermost centres).
double SampleCells(const Grid& g, const std::vector<double>& f, const V3& x);
//! Trilinear interpolation of the face component a at world point x.
double SampleFace(const Grid& g, int a, const std::vector<double>& f, const V3& x);
V3 SampleVelocity(const Grid& g, const MacField& u, const V3& x);
//! Cell-centred velocity: the average of the two faces along each axis.
void CellVelocity(const Grid& g, const MacField& u, std::array<std::vector<double>,3>& c, Pool* pool);

// ------------------------------------------------------------------ geometry

//! An analytic shape or a closed triangle mesh, as a signed distance
//! (negative inside).  Plane: the half space below `point` along `normal`
//! is inside.  Box: [lo, hi].  Cylinder: capped, axis through `point`
//! (its centre) with unit `axis`, `radius`, `height` (total length).
struct Shape {
	enum class Type { Plane, Sphere, Box, Cylinder, Mesh };
	Type type = Type::Sphere;
	V3 point{{0, 0, 0}}, axis{{0, 1, 0}}, lo{{0, 0, 0}}, hi{{0, 0, 0}};
	double radius = 0, height = 0;
	bool invert = false;   // the complement (m7b validation: the inside of a container); analytic shapes only
	// Mesh (world space): flat xyz array and triangles.
	std::vector<double> vertices;
	std::vector<std::uint32_t> triangles;
	//! Signed distance of an analytic shape (exact; Mesh is not handled here).
	double Distance(const V3& x) const;
};

//! Signed distance of the union of `shapes` (minimum) at every node, exact
//! within `band` of each surface and clamped to +-band beyond.  Meshes: the
//! exact distance to the nearest triangle within the band, the sign by ray
//! parity along +x (closed meshes); an empty list gives +band everywhere.
void SolidNodes(const Grid& g, const std::vector<Shape>& shapes, double band, std::vector<double>& nodes, Pool* pool);

//! Open (non-solid) fraction of every face from the node signed distance:
//! each face is split into four triangles about its centre (the mean of its
//! corners) and the positive area of the linear interpolant is exact on
//! each (Ng, Min and Gibou 2009, face fractions from a level set).  Node
//! distances within 1e-9 h of zero are zero and only strictly positive
//! values are open, so a solid face lying on a grid plane closes the faces
//! in that plane and opens none inside the solid (round-off had opened
//! slivers of 1e-16 there; docs/SIMULATION_M7A_RESULTS.md section 13).
//! Domain boundary faces are closed (fraction 0), except the top faces of
//! axis `openAxis` when `openTop` (fraction from the solid as inside).
void FaceFractions(const Grid& g, const std::vector<double>& nodes, bool openTop, int openAxis,
	std::array<std::vector<double>,3>& fraction, Pool* pool);
//! The open fraction of one face (i, j, k) of axis a, as FaceFractions computes it.
double FaceFraction(const Grid& g, const std::vector<double>& nodes, int a, int i, int j, int k, bool openTop, int openAxis);
//! Open fraction of each cell, clamp(1/2 + phi_s(centre)/h, 0, 1) with
//! phi_s(centre) the mean of its eight nodes.
void CellOpenFractions(const Grid& g, const std::vector<double>& nodes, std::vector<double>& open, Pool* pool);
//! The open fraction of one cell, as CellOpenFractions computes it.
double CellOpenFraction(const Grid& g, const std::vector<double>& nodes, int i, int j, int k);
//! Fraction of a triangle where the linear interpolant of a, b, c is > 0.
double TrianglePositiveFraction(double a, double b, double c);

//! Signed distance of the union of initial liquid regions at every cell
//! centre (minimum: exact outside the union, a lower bound of the depth
//! inside; reinitialised afterwards).
void LiquidCells(const Grid& g, const std::vector<Shape>& regions, std::vector<double>& phi, Pool* pool);

} } }

#endif
