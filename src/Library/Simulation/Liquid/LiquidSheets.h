//////////////////////////////////////////////////////////////////////
//
//  LiquidSheets.h - Thin open surfaces (a cloth, a paddle) as moving
//  boundaries of the liquid solver (simulation M7d, preview;
//  docs/SIMULATION_M7D_RESULTS.md).
//
//  A zero-thickness sheet cannot be a signed-distance solid.  It is a
//  face-crossing barrier instead (Guendelman, Bridson and Fedkiw 2005, in
//  form): a face of the staggered grid is closed when the segment between
//  the centres of the two cells it separates crosses a sheet triangle.  No
//  volume is removed (the cell fractions are unchanged); flow through the
//  sheet is blocked because every path of neighbouring cells from one side to
//  the other crosses such a segment.  A closed face carries the sheet's
//  normal velocity there, (v . n) n_a (v the barycentric velocity at the
//  crossing, n the triangle's unit normal), as a solid face velocity, so a
//  moving sheet pushes the liquid without pumping it along itself.  The
//  sheet is located to the cell-centre lattice: a staircase of half a cell.
//
//  Crossings.  For each triangle and axis a, the lattice lines along a
//  through the cell centres of the other two axes are intersected with the
//  triangle (barycentric coordinates in the projection, inclusive by 1e-10 so
//  that a line through a shared edge or vertex is never missed); the crossing
//  at coordinate s closes the face between the two cell centres that bracket
//  s.  A face crossed more than once (shared edges, folds, two sheets) keeps
//  the crossing nearest its centre, ties by sheet and triangle: the result is
//  independent of the thread count.
//
//  Load.  A closed face f of axis a between cells L and R (L on the minus
//  side) receives the impulse rho h^3 (q_L - q_R) e_a in a step (q the step's
//  scaled pressure, 0 in air): the counterpart of LiquidBodies.h's rho h^3 J q
//  for a fully closed face, so the liquid's impulse on the sheet is minus the
//  sheet's on the liquid.  It is spread to the crossed triangle's vertices by
//  the barycentric weights of the crossing.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_SHEETS_H
#define RISE_SIMULATION_LIQUID_SHEETS_H

#include "LiquidBodies.h"

#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Liquid {

struct LiquidSheet {
	std::string name;
	enum class Motion {
		Prescribed,   // a rigid motion of `local` (LiquidBody::PrescribedAt of `path`)
		Port          // positions and velocities from a mesh.v2 surface (cloth's surface_state) at each macro start, drifted with that velocity
	};
	Motion motion = Motion::Prescribed;
	std::vector<std::uint32_t> triangles;   // fixed topology
	std::vector<double> x, v;               // world positions (m) and velocities (m/s), 3 per vertex
	// Prescribed: vertices in the sheet's frame and the frame's motion.
	std::vector<double> local;
	LiquidBody path;
	// Port: the accepted surface and the offered load.
	std::string port, loadPort;
	std::string topology;                   // the accepted surface's topology digest (the load's domain)
	std::size_t Vertices() const { return x.size()/3; }
	//! Prescribed: x and v at time t.
	void PrescribedAt(double t);
	//! A bound of the sheet's surface speed over the coming steps.
	double SpeedBound() const;
};

//! A face closed by a sheet.
struct SheetCrossing {
	int axis = 0;
	std::size_t face = 0;
	std::uint32_t sheet = 0, triangle = 0;
	double w[3] = {0, 0, 0};    // barycentric weights of the crossing on the triangle's vertices
	double distance = 0;        // |s - face coordinate| (the choice among several crossings)
	double velocity = 0;        // (v . n) n_a at the crossing
};


//! Every face the sheets close (sorted by axis and face index; one entry per face).
void FindSheetCrossings(const Grid& g, const std::vector<LiquidSheet>& sheets, std::vector<SheetCrossing>& out, Pool* pool);

//! The level set beside the sheets after a step (`old`: before it): an air
//! cell (old >= 0) beside a closed face whose level set fell keeps its old
//! value unless a neighbour across an open face (fraction > 0) was liquid
//! before the step.  Liquid reaches a cell through its faces; a cell beside a sheet that
//! fills without a wet open neighbour was filled across the sheet by the level
//! set's own stencils (the false interface where a sheet holds liquid on one
//! side only: a tilted sheet leaked 3% of the liquid in 4 s at 32 cells).
//! Without the creep check, those cells' level set crept towards zero and the
//! smoothed volume measure counted them as half full.  Returns the number of
//! cells held.
//! Anywhere in the grid (with sheets only), an air cell that turned liquid
//! without such a neighbour keeps its old value too: on the dry side of a
//! sheet the extrapolated velocity carries the false interface's level set
//! (near zero) away from the sheet (a 3-D sloshing tank wetted two dry cells
//! six cells from the sheet in 1 s without this).
std::size_t BlockSheetLeaks(const Grid& g, const std::vector<SheetCrossing>& faces, const std::array<std::vector<double>,3>& fraction,
	const std::vector<double>& old, std::vector<double>& phi, Pool* pool);

//! Whether a point that moved from `from` to `to` (less than a cell along each
//! axis) crossed the barrier: its cell changed and every axis-by-axis path of
//! neighbouring cells between the two cells crosses a closed face (`faces`
//! sorted by axis and face, as FindSheetCrossings gives them).  Points outside
//! the domain count as in the nearest cell.
bool CrossesSheets(const Grid& g, const std::vector<SheetCrossing>& faces, const V3& from, const V3& to);

//! The per-vertex pressure (Pa) that, applied as cloth applies `surface_load`
//! pressure (force -p times the vertex's vector area: a third of each
//! adjacent triangle's area times its unit normal, from `x`), has the
//! component along that vector area of the force `impulse / span` (3 per
//! vertex, N s).  The tangential part is dropped (a pressure has none; on
//! the staircase it is discretisation noise).
void SheetPressure(const LiquidSheet& sheet, const std::vector<double>& impulse, double span, std::vector<double>& pressure);

} } }

#endif
