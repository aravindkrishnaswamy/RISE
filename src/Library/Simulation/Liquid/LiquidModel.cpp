//////////////////////////////////////////////////////////////////////
//
//  LiquidModel.cpp - see LiquidModel.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidModel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::uint32_t Bits(double x)
{
	const float f = static_cast<float>(x);
	std::uint32_t b;
	std::memcpy(&b, &f, 4);
	return b;
}

std::size_t Index(const std::array<int,3>& d, int i, int j, int k)
{
	return std::size_t(i) + std::size_t(d[0])*(std::size_t(j) + std::size_t(d[1])*std::size_t(k));
}

constexpr std::uint32_t None = std::numeric_limits<std::uint32_t>::max();

// Cube faces, corners counter-clockwise seen from outside (corner = x + 2y + 4z).
const int FaceCorners[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
} // namespace

// ------------------------------------------------------------------ marching cubes

void MarchingCubes(const std::array<int,3>& d, const std::vector<double>& values, const V3& origin, double h,
	IsoSurface& out, Pool* pool)
{
	out = IsoSurface();
	const std::size_t samples = std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]);
	const std::size_t rows = std::size_t(d[1])*std::size_t(d[2]);
	const std::size_t grain = RowGrain(d[0]);
	const std::size_t strides[3] = {1, std::size_t(d[0]), std::size_t(d[0])*std::size_t(d[1])};
	auto neg = [&](std::size_t s) { return values[s] < 0; };
	// Pass 1: one vertex per crossing sample edge, numbered in (sample, axis) order.
	std::vector<std::uint32_t> vid(3*samples, None);
	const std::size_t chunks = Cloth::Chunks(rows, grain);
	std::vector<std::size_t> counts(chunks, 0);
	auto crossing = [&](int i, int j, int k, int a, std::size_t s) {
		const int idx[3] = {i, j, k};
		if (idx[a] + 1 >= d[a]) return false;
		return neg(s) != neg(s + strides[a]);
	};
	Cloth::ParallelFor(pool, chunks, [&](std::size_t c) {
		std::size_t n = 0;
		for (std::size_t r = c*grain; r < std::min(rows, (c + 1)*grain); ++r) {
			const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
			for (int i = 0; i < d[0]; ++i) {
				const std::size_t s = Index(d, i, j, k);
				for (int a = 0; a < 3; ++a) if (crossing(i, j, k, a, s)) ++n;
			}
		}
		counts[c] = n;
	});
	std::vector<std::size_t> first(chunks + 1, 0);
	for (std::size_t c = 0; c < chunks; ++c) first[c + 1] = first[c] + counts[c];
	const std::size_t vertices = first[chunks];
	out.positions.resize(3*vertices);
	out.edges.resize(vertices);
	out.t.resize(vertices);
	Cloth::ParallelFor(pool, chunks, [&](std::size_t c) {
		std::size_t v = first[c];
		for (std::size_t r = c*grain; r < std::min(rows, (c + 1)*grain); ++r) {
			const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
			for (int i = 0; i < d[0]; ++i) {
				const std::size_t s = Index(d, i, j, k);
				for (int a = 0; a < 3; ++a) {
					if (!crossing(i, j, k, a, s)) continue;
					const double va = values[s], vb = values[s + strides[a]];
					const double t = va/(va - vb);
					const int idx[3] = {i, j, k};
					for (int b = 0; b < 3; ++b) out.positions[3*v + std::size_t(b)] = origin[b] + h*(idx[b] + (b == a ? t : 0.0));
					out.edges[v] = 3*std::uint64_t(s) + std::uint64_t(a);
					out.t[v] = t;
					vid[3*s + std::size_t(a)] = std::uint32_t(v);
					++v;
				}
			}
		}
	});
	// Pass 2: triangles, cube by cube, per chunk of cube rows, joined in order.
	const std::array<int,3> cd{{std::max(0, d[0] - 1), std::max(0, d[1] - 1), std::max(0, d[2] - 1)}};
	const std::size_t cubeRows = std::size_t(cd[1])*std::size_t(cd[2]);
	if (cd[0] == 0 || cubeRows == 0) return;
	const std::size_t cgrain = RowGrain(cd[0]);
	const std::size_t cchunks = Cloth::Chunks(cubeRows, cgrain);
	std::vector<std::vector<std::uint32_t>> tris(cchunks), loops(cchunks), loopStarts(cchunks);
	constexpr std::uint32_t Centroid = 0x80000000u;   // chunk-local centroid ids, remapped below
	Cloth::ParallelFor(pool, cchunks, [&](std::size_t c) {
		std::vector<std::uint32_t>& out3 = tris[c];
		std::vector<std::uint32_t>& members = loops[c];
		std::vector<std::uint32_t>& starts = loopStarts[c];
		for (std::size_t r = c*cgrain; r < std::min(cubeRows, (c + 1)*cgrain); ++r) {
			const int j = int(r % std::size_t(cd[1])), k = int(r/std::size_t(cd[1]));
			for (int i = 0; i < cd[0]; ++i) {
				double v[8];
				std::size_t s[8];
				int mask = 0;
				for (int m = 0; m < 8; ++m) {
					s[m] = Index(d, i + (m & 1), j + ((m >> 1) & 1), k + ((m >> 2) & 1));
					v[m] = values[s[m]];
					if (v[m] < 0) mask |= 1 << m;
				}
				if (mask == 0 || mask == 255) continue;
				auto edgeVertex = [&](int a, int b) {
					const int lo = std::min(a, b), axis = (a ^ b) == 1 ? 0 : (a ^ b) == 2 ? 1 : 2;
					return vid[3*s[lo] + std::size_t(axis)];
				};
				// Segments start at a - to + crossing and end at a + to - crossing
				// (counter-clockwise around each face seen from outside).
				std::uint32_t segStart[12], segEnd[12];
				int segments = 0;
				for (int f = 0; f < 6; ++f) {
					const int* fc = FaceCorners[f];
					bool n4[4];
					for (int m = 0; m < 4; ++m) n4[m] = v[fc[m]] < 0;
					int starts[2], ends[2], ns = 0, ne = 0;
					for (int m = 0; m < 4; ++m) {
						const bool a = n4[m], b = n4[(m + 1) & 3];
						if (a && !b) starts[ns++] = m;
						else if (!a && b) ends[ne++] = m;
					}
					if (ns == 0) continue;
					if (ns == 1) {
						segStart[segments] = edgeVertex(fc[starts[0]], fc[(starts[0] + 1) & 3]);
						segEnd[segments] = edgeVertex(fc[ends[0]], fc[(ends[0] + 1) & 3]);
						++segments;
						continue;
					}
					// Ambiguous face: the asymptotic decider.  Corners 0 and 2 share a sign;
					// they are joined through the face when the bilinear saddle has it too.
					const double v0 = v[fc[0]], v1 = v[fc[1]], v2 = v[fc[2]], v3 = v[fc[3]];
					const double saddle = (v0*v2 - v1*v3)/(v0 + v2 - v1 - v3);
					const bool joined02 = (saddle < 0) == n4[0];
					// joined02: cut off corners 1 and 3 (edge pairs 0-1, 2-3); else corners 0 and 2 (3-0, 1-2).
					const int pairs[2][2] = {{joined02 ? 0 : 3, joined02 ? 1 : 0}, {joined02 ? 2 : 1, joined02 ? 3 : 2}};
					for (const auto& pr : pairs) {
						const int e0 = pr[0], e1 = pr[1];
						const bool e0start = n4[e0] && !n4[(e0 + 1) & 3];
						const int st = e0start ? e0 : e1, en = e0start ? e1 : e0;
						segStart[segments] = edgeVertex(fc[st], fc[(st + 1) & 3]);
						segEnd[segments] = edgeVertex(fc[en], fc[(en + 1) & 3]);
						++segments;
					}
				}
				// Chain into loops and fan-triangulate (reversed: the chained loops wind
				// around the negative side).
				bool used[12] = {false};
				for (int s0 = 0; s0 < segments; ++s0) {
					if (used[s0]) continue;
					std::uint32_t loop[12];
					int len = 0, cur = s0;
					while (cur >= 0 && !used[cur] && len < 12) {
						used[cur] = true;
						loop[len++] = segStart[cur];
						const std::uint32_t next = segEnd[cur];
						cur = -1;
						for (int q = 0; q < segments; ++q) if (!used[q] && segStart[q] == next) { cur = q; break; }
					}
					if (len < 3) continue;
					if (len == 3) { out3.push_back(loop[0]); out3.push_back(loop[2]); out3.push_back(loop[1]); continue; }
					const std::uint32_t centre = Centroid | std::uint32_t(starts.size());
					starts.push_back(std::uint32_t(members.size()));
					for (int m = 0; m < len; ++m) members.push_back(loop[m]);
					for (int m = 0; m < len; ++m) {
						out3.push_back(centre);
						out3.push_back(loop[(m + 1) % len]);
						out3.push_back(loop[m]);
					}
				}
			}
		}
	});
	std::size_t total = 0, centroids = 0;
	for (std::size_t c = 0; c < cchunks; ++c) { total += tris[c].size(); centroids += loopStarts[c].size(); }
	out.edgeVertices = vertices;
	out.triangles.reserve(total);
	out.positions.reserve(3*(vertices + centroids));
	std::size_t base = vertices;
	for (std::size_t c = 0; c < cchunks; ++c) {
		for (std::uint32_t v : tris[c]) out.triangles.push_back(v & Centroid ? std::uint32_t(base + (v & ~Centroid)) : v);
		for (std::size_t q = 0; q < loopStarts[c].size(); ++q) {
			const std::size_t b = loopStarts[c][q], e = q + 1 < loopStarts[c].size() ? loopStarts[c][q + 1] : loops[c].size();
			double x[3] = {0, 0, 0};
			out.loopStart.push_back(std::uint32_t(out.loopMembers.size()));
			for (std::size_t m = b; m < e; ++m) {
				const std::uint32_t v = loops[c][m];
				out.loopMembers.push_back(v);
				for (int a = 0; a < 3; ++a) x[a] += out.positions[3*std::size_t(v) + std::size_t(a)];
			}
			for (int a = 0; a < 3; ++a) out.positions.push_back(x[a]/double(e - b));
		}
		base += loopStarts[c].size();
	}
	out.loopStart.push_back(std::uint32_t(out.loopMembers.size()));
}

// ------------------------------------------------------------------ model

void LiquidModel::SetThreads(unsigned threads)
{
	if (!pool_ || pool_->Threads() != std::max(1u, threads)) pool_.reset(new Pool(std::max(1u, threads)));
}

bool LiquidModel::Initialize(const LiquidSetup& setup, const SolverOptions& options, unsigned threads, std::string& error)
{
	const Grid& g = setup.grid;
	if (g.n[0] < 1 || g.n[1] < 1 || g.n[2] < 1 || !(g.h > 0)) { error = "the grid needs at least one cell per axis and a positive cell size"; return false; }
	setup_ = setup;
	options_ = options;
	SetThreads(threads);
	Pool* pool = pool_.get();
	const double band = 4*g.h;
	if (setup.solids.empty()) solidNodes.assign(g.Nodes(), band);
	else SolidNodes(g, setup.solids, band, solidNodes, pool);
	FaceFractions(g, solidNodes, setup.openTop, 1, fraction, pool);
	// Cut faces less than `minFaceFraction` open are closed (default 0: none).  The
	// jets that closing faces below 1/2 had suppressed came from transporting the
	// velocities of narrow cut faces (LiquidFlow.h Project, minValidFraction); closing
	// locates the solid only to half a cell (docs/SIMULATION_M7A_RESULTS.md section 13).
	if (options.minFaceFraction > 0)
		for (int a = 0; a < 3; ++a) for (double& f : fraction[a]) if (f < options.minFaceFraction) f = 0;
	CellOpenFractions(g, solidNodes, openCells, pool);
	// Rigid bodies (m7b): the static data above is kept, and the bodies' cut
	// cells are rebuilt from their poses at every step (LiquidBodies.h).
	bodies = setup.bodies;
	bodyImpulse.assign(bodies.size(), Vec6{{0, 0, 0, 0, 0, 0}});
	bodyAccumulated = bodyImpulse;
	wallImpulse = {{0, 0, 0}};
	if (!bodies.empty()) {
		for (LiquidBody& b : bodies) if (b.motion == BodyMotion::Prescribed) {
			V3 x, v, w;
			Quat q;
			b.PrescribedAt(0, x, q, v, w);
			b.state.x = x; b.state.q = q; b.state.v = v;
			const V3 wb = Rigid::ApplyT(Rigid::QMatrix(q), w);
			for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
		}
		geometry_.Initialize(g, solidNodes, fraction, openCells, setup.openTop, band);
		geometry_.Update(bodies, solidNodes, fraction, openCells, pool);
		solidVelocity_.Resize(g);
		enclosedMark_.assign(g.Cells(), 0);
	}
	// Sheets (m7d, LiquidSheets.h): the faces they close at t = 0 (a Port sheet has
	// no geometry until its port delivers one).  Without sheets nothing runs.
	sheets = setup.sheets;
	sheetFaces_.clear();
	sheetSaved_.clear();
	for (auto& t : sheetTouched_) t.clear();
	sheetStepImpulse.assign(sheets.size(), V3{{0, 0, 0}});
	ResetSheetImpulses();
	if (!sheets.empty()) {
		if (setup.viscosity > 0) { error = "sheets with viscosity are not supported (m7d: the viscous solve does not see them)"; return false; }
		for (LiquidSheet& sh : sheets) if (sh.motion == LiquidSheet::Motion::Prescribed) sh.PrescribedAt(0);
		sheetSolid_.Resize(g);
		CloseSheetFaces();
		// m7e: implicitly coupled sheets project through the solid velocity field the bodies use.
		for (const LiquidSheet& sh : sheets) if (sh.implicit && solidVelocity_.u[0].size() != g.Faces(0)) solidVelocity_.Resize(g);
	}
	LiquidCells(g, setup.liquid, phi, pool);
	// phi is not extrapolated into solids: the velocity is (closed faces are
	// extrapolated from the liquid's), so the level set inside a solid's
	// boundary cells follows the liquid; an explicit extrapolation of phi
	// (mean or linear) perturbed a hydrostatic surface near a solid
	// (docs/SIMULATION_M7A_RESULTS.md section 2).
	u.Resize(g);
	for (int a = 0; a < 3; ++a) if (g.n[a] > 1) std::fill(u.u[a].begin(), u.u[a].end(), setup.initialVelocity[a]);
	q_.assign(g.Cells(), 0.0);
	lastDt_ = 0;
	steps = 0;
	time_ = 0;
	clampedFaces = clampedSteps = 0;
	sheetCrossedCells = sheetUnfedCells = 0;
	sheetCrossedVolume = 0;
	if (!bodies.empty()) ExtrapolateEnclosed();
	if (!ProjectState(error)) return false;
	targetVolume_ = Volume();
	if (options_.particles) particles.Seed(g, phi, options_.particle, 0, pool);
	else particles = MarkerParticles();
	const double gn = std::sqrt(setup.gravity[0]*setup.gravity[0] + setup.gravity[1]*setup.gravity[1] + setup.gravity[2]*setup.gravity[2]);
	depth_ = 0;
	if (gn > 0) for (int a = 0; a < 3; ++a) depth_ += std::fabs(setup.gravity[a])/gn*g.n[a]*g.h;
	return true;
}

double LiquidModel::MaxSpeed() const
{
	const Grid& g = setup_.grid;
	double m = 0;
	for (int a = 0; a < 3; ++a) {
		const auto& f = u.u[a];
		const auto& ok = valid[a];
		m = std::max(m, MaxRows(pool_.get(), f.size(), 4096, [&](std::size_t i0, std::size_t i1) {
			double x = 0;
			for (std::size_t i = i0; i < i1; ++i) if (ok[i]) x = std::max(x, std::fabs(f[i]));
			return x;
		}));
	}
	(void)g;
	return m;
}

double LiquidModel::Volume() const
{
	return LiquidVolume(setup_.grid, phi, HasSolids() ? &openCells : nullptr, pool_.get());
}

double LiquidModel::MaxStep(double segment) const
{
	const Grid& g = setup_.grid;
	const double gn = std::sqrt(setup_.gravity[0]*setup_.gravity[0] + setup_.gravity[1]*setup_.gravity[1] +
		setup_.gravity[2]*setup_.gravity[2]);
	double dt = std::numeric_limits<double>::infinity();
	double base = bodies.empty() ? MaxSpeed() : std::max(MaxSpeed(), BodySpeed());
	if (!sheets.empty()) for (const LiquidSheet& sh : sheets) base = std::max(base, sh.SpeedBound());   // m7d
	const double speed = base + gn*segment;
	if (speed > 0) dt = std::min(dt, options_.cfl*g.h/speed);
	if (gn > 0 && depth_ > 0) dt = std::min(dt, options_.waveCfl*g.h/std::sqrt(gn*depth_));
	// m7c: the capillary limit sqrt(rho h^3 / (2 pi sigma)) with the liquid's density (the 2 pi
	// form is Denner and van Wachem 2015; Brackbill, Kothe and Zemach 1992 have 4 pi;
	// docs/SIMULATION_M7C_RESULTS.md for the measured stability).
	if (setup_.viscosity > 0 && options_.viscousCfl > 0) dt = std::min(dt, options_.viscousCfl*g.h*g.h*setup_.density/setup_.viscosity);
	if (setup_.surfaceTension > 0)
		dt = std::min(dt, options_.capillaryCfl*std::sqrt(setup_.density*g.h*g.h*g.h/(2*3.14159265358979323846*setup_.surfaceTension)));
	return dt;
}

PressureOptions LiquidModel::FinalPressure() const
{
	PressureOptions po = options_.pressure;
	po.minValidFraction = options_.minTransportFraction;
	return po;
}

PressureOptions LiquidModel::WithJump(PressureOptions po, double dt, double weight) const
{
	if (!(setup_.surfaceTension > 0)) return po;
	po.curvature = &kappa_;
	po.jumpScale = setup_.surfaceTension*dt*weight/(setup_.density*setup_.grid.h);
	po.interpolateCurvature = options_.interpolateCurvature;
	return po;
}

bool LiquidModel::Viscous(MacField& s, double dt, std::vector<Vec6>& w, std::vector<Vec6>& impulse, StepStats& stats, std::string& error)
{
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	const double t0 = Now();
	ViscosityOptions vo;
	vo.kinematic = setup_.viscosity/setup_.density;
	vo.noSlip = setup_.noSlip;
	vo.openTop = setup_.openTop;
	vo.tolerance = options_.viscosityTolerance;
	vo.thetaMin = options_.viscousThetaMin;
	vo.cvSamples = options_.viscousSamples;
	vo.massThetaMin = options_.pressure.thetaMin;
	ViscosityInput in;
	std::vector<ViscousTwist> twists;
	if (HasSolids()) in.solidNodes = &solidNodes;
	if (!bodies.empty()) {
		geometry_.SolidVelocity(w, solidVelocity_);
		in.solid = &solidVelocity_;
		for (int a = 0; a < 3; ++a) owner_[a].assign(g.Faces(a), std::int16_t(-1));
		const auto& lists = geometry_.Faces();
		for (std::size_t i = 0; i < lists.size(); ++i) for (const BodyFace& f : lists[i]) owner_[f.axis][f.face] = std::int16_t(i);
		in.owner = &owner_;
		for (std::size_t i = 0; i < bodies.size(); ++i) {
			ViscousTwist t;
			t.x = bodies[i].state.x;
			t.v = {{w[i][0], w[i][1], w[i][2]}};
			t.w = {{w[i][3], w[i][4], w[i][5]}};
			// Bodies the liquid integrates are unknowns of the viscous solve (implicit coupling).
			t.coupled = bodies[i].Coupled();
			t.mass = bodies[i].state.mass;
			std::array<double,9> inverse;
			WorldInertia(bodies[i].state, t.inertia, inverse);
			t.free = bodies[i].free;
			twists.push_back(t);
		}
		in.bodies = &twists;
	}
	std::vector<Impulse6> onBodies;
	if (!ViscousStep(g, phi, fraction, in, vo, dt, setup_.density, s, onBodies, viscousWallImpulse, stats.viscous, pool, error)) return false;
	impulse.assign(bodies.size(), Vec6{{0, 0, 0, 0, 0, 0}});
	bodyViscousImpulse = impulse;
	// Coupled bodies were unknowns of the solve (their impulse M (w - w*)); the caller
	// moves them by M^-1 of it before their pressure solve.
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		for (int m = 0; m < 6; ++m) impulse[i][std::size_t(m)] = onBodies[i][std::size_t(m)];
		bodyViscousImpulse[i] = impulse[i];
	}
	stats.viscosity += Now() - t0;
	return true;
}

bool LiquidModel::ProjectState(std::string& error)
{
	PressureStats stats;
	if (bodies.empty()) {
		if (sheets.empty()) return Project(setup_.grid, phi, fraction, nullptr, FinalPressure(), {}, u, q_, valid, stats, pool_.get(), error);
		ClearSheetVelocity(sheetSolid_);
		SetSheetVelocity(sheetSolid_);
		return Project(setup_.grid, phi, fraction, &sheetSolid_, FinalPressure(), {}, u, q_, valid, stats, pool_.get(), error);
	}
	// The bodies' current velocities as boundary conditions (kinematic: an impulsive start of the liquid);
	// bodyImpulse receives the impulse rho h^3 J q the liquid exerts on each.
	if (!sheets.empty()) ClearSheetVelocity(solidVelocity_);
	geometry_.SolidVelocity(BodyVelocities(), solidVelocity_);
	if (!sheets.empty()) SetSheetVelocity(solidVelocity_);
	if (!Project(setup_.grid, phi, fraction, &solidVelocity_, FinalPressure(), {}, u, q_, valid, stats, pool_.get(), error)) return false;
	std::vector<std::uint8_t> active;
	ActiveCells(setup_.grid, phi, fraction, active, pool_.get());
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		BodyCoupling c(setup_.grid, geometry_.Faces()[i], bodies[i], setup_.density, false);
		c.Begin(active);
		bodyImpulse[i] = c.Measure(q_);
	}
	return true;
}

bool LiquidModel::SetLevelSet(std::vector<double> levelSet, std::string& error)
{
	if (levelSet.size() != setup_.grid.Cells()) { error = "level set of the wrong size"; return false; }
	phi = std::move(levelSet);
	targetVolume_ = Volume();
	if (options_.particles) particles.Seed(setup_.grid, phi, options_.particle, 0, pool_.get());
	return ProjectState(error);
}

bool LiquidModel::Run(double duration, double segment, const std::function<void(double, const StepStats&)>& after, std::string& error)
{
	const double end = time_ + duration;
	while (time_ < end*(1 - 1e-14)) {
		const double len = std::min(segment, end - time_);
		const double start = time_;
		const double dtmax = MaxStep(len);
		const std::uint64_t n = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(len/dtmax*(1 - 1e-12))));
		for (std::uint64_t k = 0; k < n; ++k) {
			const double t1 = k + 1 == n ? start + len : start + len*double(k + 1)/double(n);
			StepStats stats;
			if (!Step(t1 - time_, stats, error)) return false;
			time_ = t1;
			if (after) after(time_, stats);
		}
	}
	return true;
}

bool LiquidModel::Step(double dt, StepStats& stats, std::string& error)
{
	stats = StepStats();
	stats.dt = dt;
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	const bool hasBodies = !bodies.empty();
	const bool solids = HasSolids();
	const std::vector<double>* open = solids ? &openCells : nullptr;
	double t0 = Now();
	// m7d: faces a sheet closes carry its normal velocity as known values (the
	// projection left them invalid), so both extrapolations and the transport
	// see the sheet's motion there.
	for (const SheetCrossing& c : sheetFaces_) {
		u.u[c.axis][c.face] = c.velocity;
		valid[c.axis][c.face] = 1;
	}
	// Two extrapolations of the step-start velocity: for the interface
	// (level set, particles) linear in the first `linearExtrapolationLayers`
	// layers, which makes the surface velocity second-order accurate; for
	// momentum advection constant, which keeps under-resolved films stable
	// (a linear extrapolation ahead of a dam-break front made its film
	// accelerate without bound; docs/SIMULATION_M7A_RESULTS.md section 2).
	const std::vector<double> phiStart = sheets.empty() ? std::vector<double>() : phi;   // m7d
	sweptCells_.clear();
	MacField ui = u;
	{
		FaceMask known = valid;
		ExtrapolateVelocity(g, ui, known, options_.interfaceExtrapolationLayers, InterfaceLinearLayers(), pool);
	}
	// m7c, no slip: in the momentum field, closed faces and cut faces too narrow
	// to be transported (minTransportFraction) take the solids' velocity (at rest,
	// or the bodies' rigid velocity) instead of the liquid's extrapolated one, so
	// the advection beside a no-slip wall carries the wall's momentum
	// (docs/SIMULATION_M7C_RESULTS.md, circular Couette).  The interface field
	// keeps the m7b extension.
	if (setup_.viscosity > 0 && setup_.noSlip && solids) {
		if (hasBodies) geometry_.SolidVelocity(BodyVelocities(), solidVelocity_);
		for (int a = 0; a < 3; ++a) {
			if (g.n[a] == 1) continue;
			const auto d = g.FaceDims(a);
			for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
				const int idx[3] = {i, j, k};
				if (idx[a] == 0 || idx[a] == g.n[a]) continue;
				const std::size_t f = g.F(a, i, j, k);
				if (valid[a][f]) continue;
				const double A = fraction[a][f];
				if (A >= options_.minTransportFraction) continue;
				u.u[a][f] = hasBodies ? solidVelocity_.u[a][f] : 0.0;
				valid[a][f] = 1;
			}
		}
	}
	ExtrapolateVelocity(g, u, valid, options_.extrapolationLayers, 0, pool);
	double t1 = Now();
	stats.extrapolate = t1 - t0;
	if (options_.particles) {
		std::vector<V3> before;
		if (!sheetFaces_.empty()) for (const auto& p : particles.Particles()) before.push_back(p.x);
		particles.Advect(g, [&](double) -> const MacField& { return ui; }, 0.0, dt, pool);
		if (!sheetFaces_.empty()) {
			// m7d: marker particles carried through a sheet are removed (on the far side they
			// corrected the level set there and seeded liquid on a dry side).
			auto& ps = particles.Particles();
			std::size_t out = 0;
			for (std::size_t i = 0; i < ps.size(); ++i) if (i >= before.size() || !CrossesSheets(g, sheetFaces_, before[i], ps[i].x)) ps[out++] = ps[i];
			stats.sheetParticlesRemoved = ps.size() - out;
			ps.resize(out);
		}
		t0 = Now();
		stats.particles += t0 - t1;
		t1 = t0;
	}
	{
		std::array<std::vector<double>,3> c;
		CellVelocity(g, ui, c, pool);
		AdvectLevelSet(g, phi, [&](double, std::array<std::vector<double>,3>& v) { v = c; }, 0.0, dt, pool, &barrier_);
	}
	if (stageHook) stageHook("advected");
	t0 = Now();
	stats.levelSet += t0 - t1;
	t1 = t0;
	// m7d: the faces the sheets closed open again before the bodies' cut cells
	// are rebuilt (which write the fractions in their regions).
	const std::vector<SheetCrossing> facesBefore = CoupledSheets() ? sheetFaces_ : std::vector<SheetCrossing>();   // m7e
	if (!sheets.empty()) RestoreSheetFaces();
	// Bodies (m7b): new poses (drifted with the step-start velocity, as the
	// interface), the cut cells rebuilt there, the level set in the cells
	// they enclose extrapolated from the liquid around them, and marker
	// particles inside them removed.
	if (hasBodies) {
		MoveBodies(dt);
		geometry_.Update(bodies, solidNodes, fraction, openCells, pool);
		ExtrapolateEnclosed();
		if (options_.particles) {
			auto& ps = particles.Particles();
			std::vector<Rigid::M3> R;
			for (const LiquidBody& b : bodies) R.push_back(Rigid::QMatrix(b.state.q));
			ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const MarkerParticles::Particle& p) {
				for (std::size_t i = 0; i < bodies.size(); ++i) {
					const V3 d{{p.x[0] - bodies[i].state.x[0], p.x[1] - bodies[i].state.x[1], p.x[2] - bodies[i].state.x[2]}};
					if (bodies[i].shape.Distance(Rigid::ApplyT(R[i], d)) < 0) return true;
				}
				return false;
			}), ps.end());
		}
		t0 = Now();
		stats.bodies += t0 - t1;
		t1 = t0;
	}
	// m7d: the sheets at the step's end (prescribed: their motion; port: drifted
	// with the accepted velocity), the faces they cross closed with their velocity.
	MacField* sheetSolid = nullptr;
	const bool coupledSheets = CoupledSheets();   // m7e
	if (!sheets.empty()) {
		MoveSheets(dt);
		CloseSheetFaces();
		if (coupledSheets) {
			if (stageHook) stageHook("closed");
			const std::pair<std::size_t, std::size_t> n = SweepSheetCells(facesBefore);
			if (stageHook) stageHook("swept");
			stats.sheetCellsKept += n.first;
			stats.sheetParticlesRemoved += n.second;
		}

		if (!hasBodies && !coupledSheets) {
			ClearSheetVelocity(sheetSolid_);
			SetSheetVelocity(sheetSolid_);
			sheetSolid = &sheetSolid_;
		}
		t0 = Now();
		stats.bodies += t0 - t1;
		t1 = t0;
	}
	const MacField un = u;   // the extrapolated step-start velocity (advected below)
	if (options_.particles) stats.escaped = particles.Correct(g, phi, pool);
	t0 = Now();
	stats.particles += t0 - t1;
	stats.reinitialised = options_.reinit != ReinitScheme::None &&
		(options_.reinitThreshold <= 0 || GradientDeviation(g, phi, 3, pool, &barrier_) > options_.reinitThreshold);
	if (stageHook) stageHook("corrected");
	// m7g: with coupled sheets the reinitialisation's ghosts at the domain walls are constant
	// (LiquidLevelSet.h Reinitialize): with the linear ones a false interface spread along the
	// walls below a sagging sheet.  Other runs keep m7a's linear ghosts (their bytes).
	if (stats.reinitialised) Reinitialize(g, phi, options_.reinit, options_.reinitIterations, options_.reinitCfl, pool, &barrier_,
		coupledSheets ? Extension::Constant : Extension::Linear);
	if (stageHook) stageHook("reinitialised");
	t1 = Now();
	stats.levelSet += t1 - t0;
	if (options_.particles) {
		particles.Correct(g, phi, pool);
		particles.AdjustRadii(g, phi, pool);
		if ((steps + 1) % std::uint64_t(std::max(1, options_.particle.reseedInterval)) == 0) particles.Reseed(g, phi, steps + 1, pool);
	}
	t0 = Now();
	stats.particles += t0 - t1;
	if (!coupledSheets) { if (options_.volumeCorrection) stats.volumeShift = CorrectVolume(g, phi, targetVolume_, open, pool); }
	else {
		// m7f: the level set beside a one-sided coupled sheet is its distance to the sheet
		// (PinSheetInterface) before the volume is measured, and the correction moves the free
		// surface only: the pinned cells are left out of its area and keep their values.  Pinned
		// after a correction that had shifted them too (m7e's order), the cells beside the sheet
		// returned what the shift had taken from them each step while the free surface kept its
		// share: a pinned bag lost 12% of its water in 2 s at 24 cells.
		std::vector<std::uint8_t> pinned;
		if (stageHook) stageHook("particles");
		stats.sheetCellsKept += PinSheetInterface(&pinned);
		if (stageHook) stageHook("pinned");
		if (options_.volumeCorrection) {
			// The band of the sheet's zero: the cells set and their neighbours on the same side with
			// no interface of their own (their values follow the boundary values: shifted, they
			// were pulled back by the reinitialisation and the pin each step, and the measured
			// volume crept up by a few per cent per second in a bag at rest).
			std::vector<std::uint8_t> band = pinned;
			for (std::size_t c = 0; c < pinned.size(); ++c) {
				if (!pinned[c]) continue;
				const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
					int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
				for (int a = 0; a < 3; ++a) for (int o = -1; o <= 1; o += 2) {
					int q[3] = {idx[0], idx[1], idx[2]};
					q[a] += o;
					if (q[a] < 0 || q[a] >= g.n[a]) continue;
					const std::size_t nb = g.C(q[0], q[1], q[2]);
					if (!band[nb] && (phi[nb] < 0) == (phi[c] < 0) && !OwnInterface(nb)) band[nb] = 2;
				}
			}
			pinned.swap(band);
			const double v = Volume(), area = InterfaceArea(g, phi, open, pool, &pinned);
			if (area > 0) {
				const double shift = (v - targetVolume_)/area;
				ForRows(pool, phi.size(), 4096, [&](std::size_t a, std::size_t b) { for (std::size_t c = a; c < b; ++c) if (!pinned[c]) phi[c] += shift; });
				stats.volumeShift = shift;
			}
		}
	}
	// m7d: no cell beside a sheet fills across it (LiquidSheets.h BlockSheetLeaks).
	if (!sheets.empty() && !coupledSheets) stats.sheetCellsKept = BlockSheetLeaks(g, sheetFaces_, fraction, phiStart, phi, pool);
	if (coupledSheets) CountSheetCrossings(phiStart, facesBefore);   // m7g, before the guard holds them
	if (coupledSheets) {
		// m7e: beside a coupled sheet the creep rule would hold a dry cell's level set while the
		// sheet approaches it (the false interface could not follow a membrane sagging inside a
		// cell gap: its oscillation came out stiffer with every step taken); PinSheetInterface
		// puts that zero at the sheet instead.  The rule elsewhere (cells that fill without a wet
		// neighbour) stays.
		std::vector<SheetCrossing> others;
		for (const SheetCrossing& c : sheetFaces_) if (!sheets[c.sheet].Coupled()) others.push_back(c);
		stats.sheetCellsKept += BlockSheetLeaks(g, others, fraction, phiStart, phi, pool);
	}
	// m7c: the curvature of the new interface for the surface-tension jump.
	if (setup_.surfaceTension > 0) Curvature(g, phi, kappa_, pool);
	if (coupledSheets) OpenUnwetted();   // m7g
	t1 = Now();
	stats.levelSet += t1 - t0;
	const bool viscous = setup_.viscosity > 0;
	std::vector<Vec6> viscousImpulse;
	// m7c: the viscous step with an incremental pressure.  Splitting the implicit
	// viscous step from the projection, u = P(V(u*)), has the steady state of
	// minimising the strain energy without the incompressibility constraint
	// and then projecting, which is not Stokes flow once nu dt / h^2 is large:
	// the 2-D Stokes drag on a cylinder in a concentric container came out at 26%
	// of the exact value at nu dt / h^2 = 30 (docs/SIMULATION_M7C_RESULTS.md).  The
	// viscous solve therefore sees the step's gravity and the previous step's
	// pressure gradient (scaled to this dt; `stagePart` the part of it this
	// projection carries), s - G q_prev + w dt g, and G q_prev is added back
	// before the projection, which then solves for the whole pressure (the
	// incremental pressure-correction scheme): a steady Stokes flow is its fixed
	// point.  Without viscosity the arithmetic is m7b's.
	const std::vector<double> qPrev = viscous && lastDt_ > 0 ? q_ : std::vector<double>();
	const double prevDt = lastDt_;
	auto viscousStep = [&](MacField& s, std::vector<Vec6>& w, double gravityWeight, const std::vector<double>* stagePart,
		const std::vector<Vec6>* stageImpulse) -> bool {
		const double tv = Now();
		std::vector<double> qp;
		// The predictor's gradient carries the surface-tension jump of this stage's share
		// (a drop's Laplace pressure is no gradient at its surface).
		const PressureOptions pg = WithJump(options_.pressure, dt, stagePart ? 0.5 : 1.0);
		if (!qPrev.empty()) {
			qp = qPrev;
			const double r = dt/prevDt;
			for (std::size_t c = 0; c < qp.size(); ++c) qp[c] = qp[c]*r - (stagePart ? (*stagePart)[c] : 0.0);
			SubtractPressureGradient(g, phi, fraction, pg, qp, 1.0, s, pool, true);
		}
		for (int a = 0; a < 3; ++a) {
			if (g.n[a] == 1) continue;
			const double dv = gravityWeight*dt*setup_.gravity[a];
			for (double& x : s.u[a]) x += dv;
		}
		// Coupled bodies are unknowns of the viscous solve (implicit), starting from their
		// predicted velocity with the previous step's pressure impulse (less the part
		// this stage already holds): the body's counterpart of G q_prev, so that at a
		// terminal velocity the drag is evaluated at the body's velocity (without it a
		// settling sphere's drag was evaluated about g dt off, which at low Reynolds
		// number is the terminal velocity itself).  Only the viscous impulse moves w.
		std::vector<Vec6> wall = w;
		if (!qPrev.empty() && bodyImpulse.size() == bodies.size() && bodyViscousImpulse.size() == bodies.size())
			for (std::size_t i = 0; i < bodies.size(); ++i) {
				if (!bodies[i].Coupled()) continue;
				Vec6 I = bodyImpulse[i];
				for (int m = 0; m < 6; ++m)
					I[std::size_t(m)] = (I[std::size_t(m)] - bodyViscousImpulse[i][std::size_t(m)])*dt/prevDt - (stageImpulse ? 0.5*(*stageImpulse)[i][std::size_t(m)] : 0.0);
				BodyCoupling c(g, geometry_.Faces()[i], bodies[i], setup_.density, true);
				const Vec6 dw = c.InverseMass(I);
				for (int m = 0; m < 6; ++m) wall[i][std::size_t(m)] += dw[std::size_t(m)];
			}
		if (!Viscous(s, dt, wall, viscousImpulse, stats, error)) return false;
		for (std::size_t i = 0; i < bodies.size(); ++i) {
			if (!bodies[i].Coupled()) continue;
			BodyCoupling c(g, geometry_.Faces()[i], bodies[i], setup_.density, true);
			const Vec6 dw = c.InverseMass(viscousImpulse[i]);
			for (int m = 0; m < 6; ++m) w[i][std::size_t(m)] += dw[std::size_t(m)];
		}
		if (!qp.empty()) SubtractPressureGradient(g, phi, fraction, pg, qp, -1.0, s, pool, true);
		stats.momentum -= Now() - tv;   // counted in stats.viscosity
		return true;
	};
	// Momentum (step 3 of LiquidModel.h) with the new interface.
	auto addGravity = [&](MacField& f, double scale) {
		for (int a = 0; a < 3; ++a) {
			if (g.n[a] == 1) continue;
			const double dv = scale*dt*setup_.gravity[a];
			for (double& x : f.u[a]) x += dv;
		}
	};
	const bool weno = options_.momentum == MomentumScheme::Weno5;
	MacField s1;
	if (options_.momentumStages <= 1) AdvectVelocity(g, un, dt, options_.momentum, s1, pool);
	else if (weno) LimitedAdvectionStage(g, un, dt, s1, pool);
	else AdvectFieldAlong(g, un, un, dt, options_.momentum, s1, pool);
	// Bodies: predicted velocities w* (free degrees of freedom gain dt F_ext / m).
	std::vector<Vec6> w0, w1star, impulse1, impulse2;
	if (hasBodies) {
		w0 = BodyVelocities();
		w1star = w0;
		for (std::size_t i = 0; i < bodies.size(); ++i) {
			const LiquidBody& b = bodies[i];
			if (!b.Coupled()) continue;
			for (int a = 0; a < 3; ++a) if (b.free[std::size_t(a)]) w1star[i][std::size_t(a)] += dt*b.externalForce[std::size_t(a)]/b.state.mass;
		}
	}
	std::vector<std::vector<double>> v1star(sheets.size()), sheetImp1, sheetImp2;
	// The sheet velocities after the step's projections (empty: not coupled).
	std::vector<std::vector<double>> sheetNew(sheets.size());
	auto sheetUpdate = [&](const std::vector<std::vector<double>>& star, const std::vector<std::vector<double>>& imp, double w,
		std::vector<std::vector<double>>& out) {
		out.assign(sheets.size(), {});
		for (std::size_t i = 0; i < sheets.size(); ++i) {
			if (!sheets[i].Coupled()) continue;
			std::vector<double> r = imp[i], dv;
			for (double& x : r) x *= w;
			sheets[i].dyn.Solve(r, dv);
			out[i] = star[i];
			for (std::size_t k = 0; k < dv.size(); ++k) out[i][k] += dv[k];
		}
	};
	// m7c: viscosity once per step, implicit over dt, before the step's last
	// projection and before gravity's increment (a uniform field, which the
	// hydrostatic pressure balances; diffused at a no-slip wall it would leave
	// a splitting error there).
	if (viscous && options_.momentumStages <= 1) { if (!viscousStep(s1, w1star, 1.0, nullptr, nullptr)) return false; }
	else addGravity(s1, 1);
	t0 = Now();
	stats.momentum += t0 - t1;
	// The step's last projection leaves cut faces narrower than minTransportFraction invalid
	// (replaced by extrapolation before the next step transports them); the
	// first stage's keeps every updated face for the second stage.
	PressureOptions po = WithJump(options_.momentumStages <= 1 ? FinalPressure() : options_.pressure, dt, 1.0);
	if (coupledSheets) {
		// m7e: the implicitly coupled sheets' predicted velocities: their linearised
		// backward-Euler step with the liquid on the faces they close moving with
		// them (LiquidSheets.h), from the stage's predicted face velocities.
		std::vector<std::vector<SheetBlock>> faceMass;
		std::vector<std::vector<double>> faceRhs;
		SheetFaceMass(s1, faceMass, faceRhs);
		for (std::size_t i = 0; i < sheets.size(); ++i) {
			if (!sheets[i].Coupled()) continue;
			sheets[i].dyn.Rotate(dt, sheets[i].v);   // m7h: the corotational model's rotations for this step
			if (!sheets[i].dyn.Prepare(dt, sheets[i].v, faceMass[i], sheets[i].triangles, error)) return false;
			sheets[i].dyn.Predict(dt, sheets[i].v, faceRhs[i], v1star[i]);
		}
		if (!ProjectCoupled(s1, po, w1star, v1star, impulse1, sheetImp1, stats.pressure, error)) return false;
	} else if (hasBodies) {
		if (!ProjectBodies(s1, po, w1star, impulse1, stats.pressure, error)) return false;
	} else if (!Project(g, phi, fraction, sheetSolid, po, {}, s1, q_, valid, stats.pressure, pool, error)) return false;
	t1 = Now();
	stats.project += t1 - t0;
	if (options_.momentumStages <= 1) {
		u = std::move(s1);
		if (coupledSheets) sheetUpdate(v1star, sheetImp1, 1.0, sheetNew);
		if (hasBodies)
			for (std::size_t i = 0; i < bodies.size(); ++i) {
				bodyImpulse[i] = impulse1[i];
				if (viscous) for (int m = 0; m < 6; ++m) bodyImpulse[i][std::size_t(m)] += viscousImpulse[i][std::size_t(m)];
				if (!bodies[i].Coupled()) continue;
				BodyCoupling c(g, geometry_.Faces()[i], bodies[i], setup_.density, true);
				const Vec6 dw = c.InverseMass(impulse1[i]);
				Vec6 w = w1star[i];
				for (int m = 0; m < 6; ++m) w[std::size_t(m)] += dw[std::size_t(m)];
				SetBodyVelocity(i, w);
			}
	} else {
		ExtrapolateVelocity(g, s1, valid, options_.extrapolationLayers, 0, pool);
		MacField s2;
		if (weno) {
			LimitedAdvectionStage(g, s1, dt, s2, pool);
			for (int a = 0; a < 3; ++a) if (g.n[a] > 1)
				for (std::size_t c = 0; c < s2.u[a].size(); ++c) s2.u[a][c] = 0.5*un.u[a][c] + 0.5*s2.u[a][c];
		} else {
			MacField mid;
			mid.Resize(g);
			for (int a = 0; a < 3; ++a) for (std::size_t c = 0; c < mid.u[a].size(); ++c) mid.u[a][c] = 0.5*(un.u[a][c] + s1.u[a][c]);
			AdvectFieldAlong(g, un, mid, dt, options_.momentum, s2, pool);
		}
		// Bodies: w*2 = w*1 + M^-1 I1 / 2 for coupled bodies under Heun (the midpoint scheme: w*1).
		std::vector<Vec6> w2star = w1star;
		if (hasBodies)
			for (std::size_t i = 0; i < bodies.size(); ++i) {
				if (!bodies[i].Coupled() || !weno) continue;
				BodyCoupling c(g, geometry_.Faces()[i], bodies[i], setup_.density, true);
				Vec6 half = impulse1[i];
				for (double& x : half) x *= 0.5;
				const Vec6 dw = c.InverseMass(half);
				for (int m = 0; m < 6; ++m) w2star[i][std::size_t(m)] += dw[std::size_t(m)];
			}
		// m7e: coupled sheets likewise, V*2 = V*1 + S^-1 I1 / 2 (Heun) or V*1 (midpoint).
		std::vector<std::vector<double>> v2star = v1star;
		if (coupledSheets && weno) sheetUpdate(v1star, sheetImp1, 0.5, v2star);
		if (viscous) {
			// Heun: s2 holds q1 / 2 already (through s1); the midpoint scheme none.
			std::vector<double> half;
			if (weno) { half = q_; for (double& x : half) x *= 0.5; }
			if (!viscousStep(s2, w2star, weno ? 0.5 : 1.0, weno ? &half : nullptr, weno && hasBodies ? &impulse1 : nullptr)) return false;
		} else addGravity(s2, weno ? 0.5 : 1.0);
		t0 = Now();
		stats.momentum += t0 - t1;
		// The step's pressure impulse is q1/2 + q2 (Heun) or q2 (midpoint);
		// q2 starts from q1/2 (Heun) or q1.
		const std::vector<double> q1 = q_;
		for (double& x : q_) x *= weno ? 0.5 : 1.0;
		po = WithJump(FinalPressure(), dt, weno ? 0.5 : 1.0);
		po.warmStart = true;
		PressureStats second;
		if (hasBodies || coupledSheets) {
			if (coupledSheets) { if (!ProjectCoupled(s2, po, w2star, v2star, impulse2, sheetImp2, second, error)) return false; }
			else if (!ProjectBodies(s2, po, w2star, impulse2, second, error)) return false;
			// m7e: the coupled sheets' new velocities V*2 + S^-1 I2 (their impulse: SheetDynamics::ApplyS below).
			if (coupledSheets) sheetUpdate(v2star, sheetImp2, 1.0, sheetNew);
			// The step's impulse: I1 / 2 + I2 (Heun) or I2 (midpoint), and the viscous
			// impulse; new velocities w*2 + M^-1 I2 (w*2 already holds the viscous part).
			for (std::size_t i = 0; i < bodies.size(); ++i) {
				Vec6 I = impulse2[i];
				if (weno) for (int m = 0; m < 6; ++m) I[std::size_t(m)] += 0.5*impulse1[i][std::size_t(m)];
				if (viscous) for (int m = 0; m < 6; ++m) I[std::size_t(m)] += viscousImpulse[i][std::size_t(m)];
				bodyImpulse[i] = I;
				if (!bodies[i].Coupled()) continue;
				BodyCoupling c(g, geometry_.Faces()[i], bodies[i], setup_.density, true);
				const Vec6 dw = c.InverseMass(impulse2[i]);
				Vec6 w = w2star[i];
				for (int m = 0; m < 6; ++m) w[std::size_t(m)] += dw[std::size_t(m)];
				SetBodyVelocity(i, w);
			}
		} else if (!Project(g, phi, fraction, sheetSolid, po, {}, s2, q_, valid, second, pool, error)) return false;
		if (weno) for (std::size_t c = 0; c < q_.size(); ++c) q_[c] += 0.5*q1[c];
		stats.pressure.iterations += second.iterations;
		stats.pressure.residual = std::max(stats.pressure.residual, second.residual);
		stats.pressure.seconds += second.seconds;
		u = std::move(s2);
		t1 = t0;
	}
	if (coupledSheets) CloseUnwetted();   // m7g
	// Safeguard (not part of the validated model): valid face speeds above
	// speedLimit sqrt(2 |g| D) are clamped and counted (none in the robustness
	// sweep since m7a.2: docs/SIMULATION_M7A_RESULTS.md section 13; invalid faces
	// are replaced by the extrapolation before any use); a non-finite state fails.
	{
		const double gn = std::sqrt(setup_.gravity[0]*setup_.gravity[0] + setup_.gravity[1]*setup_.gravity[1] +
			setup_.gravity[2]*setup_.gravity[2]);
		const double u0 = std::sqrt(setup_.initialVelocity[0]*setup_.initialVelocity[0] + setup_.initialVelocity[1]*setup_.initialVelocity[1] +
			setup_.initialVelocity[2]*setup_.initialVelocity[2]);
		const double limit = options_.speedLimit*std::max(std::sqrt(2*gn*depth_), u0);
		bool finite = true;
		for (int a = 0; a < 3; ++a) {
			std::vector<double>& f = u.u[a];
			const std::vector<std::uint8_t>& ok = valid[a];
			for (std::size_t c = 0; c < f.size(); ++c) {
				double& x = f[c];
				if (!std::isfinite(x)) { finite = false; continue; }
				if (limit > 0 && ok[c] && std::fabs(x) > limit) { x = x > 0 ? limit : -limit; ++stats.clamped; }
			}
		}
		if (finite) for (double x : phi) if (!std::isfinite(x)) { finite = false; break; }
		if (!finite) { error = "the liquid simulation diverged (non-finite velocity or level set) at step " + std::to_string(steps + 1); return false; }
		clampedFaces += stats.clamped;
		clampedSteps += stats.clamped > 0;
	}
	lastDt_ = dt;
	++steps;
	if (!sheets.empty()) AccumulateSheetImpulse();
	if (coupledSheets) {
		// m7e: the coupled sheets' new velocities, elastic positions and the step's impulse;
		// their closed faces carry the new velocity into the next step's transport.
		for (std::size_t i = 0; i < sheets.size(); ++i) {
			if (!sheets[i].Coupled()) continue;
			LiquidSheet& sh = sheets[i];
			// The impulse on the sheet itself: S V_new - (M v + dt (f0 - K (xe - x0))), its
			// momentum change in the linearised step beyond its own forces (the pressure's
			// impulse and the exchange with the liquid on its faces).
			std::vector<double> sv, rhs;
			sh.dyn.ApplyS(dt, sheetNew[i], sv);
			sh.dyn.Rhs(dt, sh.v, rhs);
			V3 total{{0, 0, 0}};
			for (std::size_t q = 0; q < sh.dyn.Vertices(); ++q) {
				if (sh.dyn.fixed[q]) continue;
				for (int a = 0; a < 3; ++a) {
					const std::size_t k = 3*q + std::size_t(a);
					const double I = sv[k] - rhs[k];
					sheetImpulse[i][k] += I;
					total[std::size_t(a)] += I;
				}
			}
			const std::size_t at = sheetHistory[i].size();
			sheetHistory[i].resize(at + 3*sh.dyn.Vertices(), 0.0);
			for (std::size_t q = 0; q < sh.dyn.Vertices(); ++q)
				if (!sh.dyn.fixed[q]) for (int a = 0; a < 3; ++a) sheetHistory[i][at + 3*q + std::size_t(a)] = sv[3*q + std::size_t(a)] - rhs[3*q + std::size_t(a)];
			sheetStepImpulse[i] = total;
			sh.v = sheetNew[i];
			for (std::size_t k = 0; k < sh.v.size(); ++k) sh.dyn.xe[k] += dt*sh.v[k];
			// m7h: how far the elements turned beyond the step's predicted rotations (the
			// corotational model's remaining rotation error; a diagnostic, never changes results).
			if (sh.dyn.Corotated()) sh.dyn.rotationLag = std::max(sh.dyn.rotationLag, sh.dyn.elements.Deviation(sh.dyn.x0, sh.dyn.xe, pool));
			// The geometry follows the linearised step's positions (it drifted with the step-start
			// velocity); the next step's closing sweeps the difference (SweepSheetCells).
			sh.x = sh.dyn.xe;
		}
		SheetCrossingVelocities(sheets, sheetNew, sheetFaces_);
	}
	if (hasBodies) {
		for (std::size_t i = 0; i < bodies.size(); ++i)
			for (int m = 0; m < 6; ++m) bodyAccumulated[i][std::size_t(m)] += bodyImpulse[i][std::size_t(m)];
		if (audit) {
			AuditWalls();
			if (viscous) for (int a = 0; a < 3; ++a) wallImpulse[std::size_t(a)] += viscousWallImpulse[std::size_t(a)];
		}
	}
	time_ += dt;
	stats.project += Now() - t1;
	return true;
}

// ------------------------------------------------------------------ bodies (m7b)

void LiquidModel::ResetBodyImpulses()
{
	bodyAccumulated.assign(bodies.size(), Vec6{{0, 0, 0, 0, 0, 0}});
}

double LiquidModel::BodySpeed() const
{
	double s = 0;
	for (const LiquidBody& b : bodies) {
		if (b.motion == BodyMotion::Prescribed) {
			double v = 0, a = 0, w = 0;
			for (int k = 0; k < 3; ++k) {
				v += b.v0[std::size_t(k)]*b.v0[std::size_t(k)];
				a += b.amplitude[std::size_t(k)]*b.amplitude[std::size_t(k)];
				w += b.spin[std::size_t(k)]*b.spin[std::size_t(k)];
			}
			s = std::max(s, std::sqrt(v) + std::sqrt(a)*2*3.14159265358979323846*b.frequency + std::sqrt(w)*b.shape.BoundingRadius());
		} else s = std::max(s, b.SurfaceSpeed());
	}
	return s;
}

std::vector<Vec6> LiquidModel::BodyVelocities() const
{
	std::vector<Vec6> w(bodies.size());
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		const V3 om = bodies[i].Omega();
		w[i] = {{bodies[i].state.v[0], bodies[i].state.v[1], bodies[i].state.v[2], om[0], om[1], om[2]}};
	}
	return w;
}

void LiquidModel::SetBodyVelocity(std::size_t i, const Vec6& w)
{
	LiquidBody& b = bodies[i];
	for (int a = 0; a < 3; ++a) b.state.v[std::size_t(a)] = w[std::size_t(a)];
	if (!b.free[3]) { b.state.pi = {{0, 0, 0}}; return; }
	const V3 wb = Rigid::ApplyT(Rigid::QMatrix(b.state.q), V3{{w[3], w[4], w[5]}});
	for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
}

void LiquidModel::MoveBodies(double dt)
{
	for (LiquidBody& b : bodies) {
		if (b.motion == BodyMotion::Prescribed) {
			V3 x, v, w;
			Quat q;
			b.PrescribedAt(time_ + dt, x, q, v, w);
			b.state.x = x; b.state.q = q; b.state.v = v;
			const V3 wb = Rigid::ApplyT(Rigid::QMatrix(q), w);
			for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
			continue;
		}
		for (int a = 0; a < 3; ++a) b.state.x[std::size_t(a)] += dt*b.state.v[std::size_t(a)];
		if (b.Coupled()) {
			if (b.free[3]) Rigid::FreeRotation(b.state, dt);
			continue;
		}
		// A kinematic port body: a constant angular velocity over the step.
		const V3 w = b.Omega();
		const double rate = std::sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
		if (rate > 0) {
			const double half = 0.5*rate*dt, k = std::sin(half)/rate;
			b.state.q = Rigid::QMul(Quat{{std::cos(half), w[0]*k, w[1]*k, w[2]*k}}, b.state.q);
			Rigid::QNormalize(b.state.q);
			const V3 wb = Rigid::ApplyT(Rigid::QMatrix(b.state.q), w);
			for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
		}
	}
}

void LiquidModel::UpdateBodyGeometry(bool extrapolate)
{
	if (bodies.empty()) return;
	if (!sheets.empty()) RestoreSheetFaces();
	geometry_.Update(bodies, solidNodes, fraction, openCells, pool_.get());
	if (!sheets.empty()) CloseSheetFaces();
	if (extrapolate) ExtrapolateEnclosed();
}

// ------------------------------------------------------------------ sheets (m7d)

void LiquidModel::ResetSheetImpulses()
{
	sheetImpulse.resize(sheets.size());
	for (std::size_t i = 0; i < sheets.size(); ++i) sheetImpulse[i].assign(3*sheets[i].Vertices(), 0.0);
	sheetHistory.assign(sheets.size(), {});
}

void LiquidModel::UpdateSheetGeometry(bool sweep)
{
	if (sheets.empty()) return;
	const std::vector<SheetCrossing> before = sweep && CoupledSheets() ? sheetFaces_ : std::vector<SheetCrossing>();
	RestoreSheetFaces();
	CloseSheetFaces();
	if (sweep && CoupledSheets()) SweepSheetCells(before);
}

void LiquidModel::MoveSheets(double dt)
{
	for (LiquidSheet& sh : sheets) {
		if (sh.motion == LiquidSheet::Motion::Prescribed) { sh.PrescribedAt(time_ + dt); continue; }
		for (std::size_t k = 0; k < sh.x.size(); ++k) sh.x[k] += dt*sh.v[k];
	}
}

void LiquidModel::RestoreSheetFaces()
{
	for (std::size_t i = 0; i < sheetFaces_.size(); ++i) fraction[sheetFaces_[i].axis][sheetFaces_[i].face] = sheetSaved_[i];
	sheetFaces_.clear();
	sheetSaved_.clear();
	barrier_.Clear();
}

void LiquidModel::CloseSheetFaces()
{
	FindSheetCrossings(setup_.grid, sheets, sheetFaces_, pool_.get());
	sheetSaved_.resize(sheetFaces_.size());
	for (std::size_t i = 0; i < sheetFaces_.size(); ++i) {
		double& f = fraction[sheetFaces_[i].axis][sheetFaces_[i].face];
		sheetSaved_[i] = f;
		f = 0;
	}
	BuildBarrier();
}

void LiquidModel::BuildBarrier()
{
	// m7f: the faces of the implicitly coupled sheets are not read across by the level set's
	// stencils (LiquidLevelSet.h LevelSetBarrier): boundary faces where the sheet holds liquid
	// on one side (the two cells differ in sign and have no interface of their own, the cells
	// PinSheetInterface sets), walls elsewhere.  Other sheets keep m7d's stencils and guards.
	barrier_.Clear();
	if (!CoupledSheets()) return;
	const Grid& g = setup_.grid;
	// m7h: not before the level set exists (Initialize closes the sheets' faces first; a model given a
	// coupled sheet's dynamics before Initialize read an empty phi here; the next geometry update builds it).
	if (phi.size() != g.Cells()) return;
	std::array<std::vector<std::pair<std::size_t, std::uint8_t>>,3> faces;
	std::vector<std::uint8_t> fixed;   // the boundary faces' cells: kept by the reinitialisation
	for (const SheetCrossing& c : sheetFaces_) {
		if (!sheets[c.sheet].Coupled()) continue;
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		const std::size_t R = g.C(idx[0], idx[1], idx[2]);
		--idx[c.axis];
		const std::size_t L = g.C(idx[0], idx[1], idx[2]);
		const bool boundary = (phi[L] < 0) != (phi[R] < 0) && !OwnInterface(L) && !OwnInterface(R);
		faces[std::size_t(c.axis)].emplace_back(c.face, std::uint8_t(boundary ? 2 : 1));
		if (boundary) {
			if (fixed.empty()) fixed.assign(g.Cells(), 0);
			fixed[L] = fixed[R] = 1;
		}
	}
	barrier_.Build(g, faces);
	barrier_.fixed.swap(fixed);
}

bool LiquidModel::OwnInterface(std::size_t c0) const
{
	// m7f: whether a cell of the other sign lies within two steps across open faces (an interface
	// on the cell's own side, near enough to be what its value measures).
	const Grid& g = setup_.grid;
	const bool neg = phi[c0] < 0;
	std::size_t frontier[6] = {c0};
	std::size_t n0 = 1;
	for (int depth = 0; depth < 2; ++depth) {
		std::size_t next[6];
		std::size_t n1 = 0;
		for (std::size_t e = 0; e < n0; ++e) {
			const std::size_t c = frontier[e];
			const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
				int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
			for (int a = 0; a < 3; ++a) for (int side = 0; side < 2; ++side) {
				int f[3] = {idx[0], idx[1], idx[2]}, nb[3] = {idx[0], idx[1], idx[2]};
				f[a] += side;
				nb[a] += side ? 1 : -1;
				if (nb[a] < 0 || nb[a] >= g.n[a]) continue;
				if (!(fraction[std::size_t(a)][g.F(a, f[0], f[1], f[2])] > 0)) continue;
				const std::size_t q = g.C(nb[0], nb[1], nb[2]);
				if ((phi[q] < 0) != neg) return true;
				if (depth == 0 && n1 < 6) next[n1++] = q;
			}
		}
		n0 = n1;
		for (std::size_t e = 0; e < n0; ++e) frontier[e] = next[e];
	}
	return false;
}

void LiquidModel::ClearSheetVelocity(MacField& solid)
{
	for (int a = 0; a < 3; ++a) {
		if (solid.u[a].size() != setup_.grid.Faces(a)) solid.u[a].assign(setup_.grid.Faces(a), 0.0);
		for (std::size_t f : sheetTouched_[a]) solid.u[a][f] = 0;
		sheetTouched_[a].clear();
	}
}

void LiquidModel::SetSheetVelocity(MacField& solid)
{
	for (const SheetCrossing& c : sheetFaces_) {
		solid.u[c.axis][c.face] = c.velocity;
		sheetTouched_[c.axis].push_back(c.face);
	}
}

void LiquidModel::AccumulateSheetImpulse()
{
	// A closed face of axis a between cells L (minus side) and R: rho h^3 (q_L - q_R) e_a
	// on the sheet (q = 0 in air), spread to the crossed triangle's vertices.
	const Grid& g = setup_.grid;
	const double scale = setup_.density*g.h*g.h*g.h;
	for (V3& x : sheetStepImpulse) x = {{0, 0, 0}};
	for (const SheetCrossing& c : sheetFaces_) {
		if (sheets[c.sheet].Coupled()) continue;   // m7e: their impulse comes from the coupled solve
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		const std::size_t R = g.C(idx[0], idx[1], idx[2]);
		--idx[c.axis];
		const std::size_t L = g.C(idx[0], idx[1], idx[2]);
		const double imp = scale*(q_[L] - q_[R]);
		const LiquidSheet& sh = sheets[c.sheet];
		std::vector<double>& acc = sheetImpulse[c.sheet];
		for (int m = 0; m < 3; ++m) acc[3*std::size_t(sh.triangles[3*std::size_t(c.triangle) + std::size_t(m)]) + std::size_t(c.axis)] += c.w[m]*imp;
		sheetStepImpulse[c.sheet][std::size_t(c.axis)] += imp;
	}
}

void LiquidModel::CountSheetCrossings(const std::vector<double>& phiStart, const std::vector<SheetCrossing>& facesBefore)
{
	// m7g: a through-sheet measure for any scene (an open sheet has no inside, so liquid on its
	// two sides is not defined).  Liquid reaches a cell through its faces: a cell that turns wet
	// in a step had a wet neighbour across an open face at the step's start, or a coupled sheet
	// swept over it (it changed sides with the sheet).  A cell that turned wet otherwise, with a
	// neighbour wet at the start across a face a coupled sheet closes (now or at the start), is
	// liquid that would have passed the sheet; one with no wet neighbour at all is an artefact of
	// the level set's corrections.  Both are counted before the guard holds them.
	const Grid& g = setup_.grid;
	std::vector<std::uint8_t> swept(g.Cells(), 0);
	for (std::size_t c : sweptCells_) swept[c] = 1;
	std::array<std::vector<std::uint8_t>,3> sheetFace;
	for (const std::vector<SheetCrossing>* list : {&facesBefore, static_cast<const std::vector<SheetCrossing>*>(&sheetFaces_)})
		for (const SheetCrossing& c : *list) {
			if (!sheets[c.sheet].Coupled()) continue;
			if (sheetFace[std::size_t(c.axis)].empty()) sheetFace[std::size_t(c.axis)].assign(g.Faces(c.axis), 0);
			sheetFace[std::size_t(c.axis)][c.face] = 1;
		}
	const double cell = g.h*g.h*g.h;
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const std::size_t c = g.C(i, j, k);
		if (!(phiStart[c] >= 0) || !(phi[c] < 0) || swept[c]) continue;
		bool fed = false, across = false;
		for (int a = 0; a < 3 && !fed; ++a) for (int side = 0; side < 2; ++side) {
			int f[3] = {i, j, k}, nb[3] = {i, j, k};
			f[a] += side;
			nb[a] += side ? 1 : -1;
			if (nb[a] < 0 || nb[a] >= g.n[a] || !(phiStart[g.C(nb[0], nb[1], nb[2])] < 0)) continue;
			const std::size_t face = g.F(a, f[0], f[1], f[2]);
			const bool closedBySheet = !sheetFace[std::size_t(a)].empty() && sheetFace[std::size_t(a)][face];
			if (closedBySheet) across = true;
			else if (fraction[std::size_t(a)][face] > 0) { fed = true; break; }
		}
		if (fed) continue;
		if (across) { ++sheetCrossedCells; sheetCrossedVolume += std::min(1.0, std::max(0.0, 0.5 - phi[c]/g.h))*cell; }
		else ++sheetUnfedCells;
	}
}

long long LiquidModel::SheetBarrierHoles() const
{
	const Grid& g = setup_.grid;
	std::array<std::vector<std::uint8_t>,3> closed;
	for (const SheetCrossing& c : sheetFaces_) {
		if (closed[std::size_t(c.axis)].empty()) closed[std::size_t(c.axis)].assign(g.Faces(c.axis), 0);
		closed[std::size_t(c.axis)][c.face] = 1;
	}
	long long holes = -1;
	for (std::size_t s = 0; s < sheets.size(); ++s) {
		const LiquidSheet& sh = sheets[s];
		if (!sh.Coupled()) continue;
		// The boundary: edges of one triangle.  Every boundary vertex must be pinned.
		std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
		for (std::size_t t = 0; t + 2 < sh.triangles.size(); t += 3)
			for (int a = 0; a < 3; ++a) {
				const std::uint32_t p = sh.triangles[t + std::size_t(a)], q = sh.triangles[t + std::size_t((a + 1) % 3)];
				++edges[{std::min(p, q), std::max(p, q)}];
			}
		std::vector<std::uint8_t> rim(sh.Vertices(), 0);
		for (const auto& e : edges) if (e.second == 1) rim[e.first.first] = rim[e.first.second] = 1;
		bool pinned = true;
		for (std::size_t q = 0; q < rim.size(); ++q) if (rim[q] && !(q < sh.dyn.fixed.size() && sh.dyn.fixed[q])) pinned = false;
		if (!pinned) continue;
		if (holes < 0) holes = 0;
		// Sides: the two cells of each closed face of an interior triangle (no rim vertex), front
		// (+, along the triangle's normal) or back (-); a cell given both (a fold) is left out.  The
		// fill may also pass through those cells' face neighbours (unlabelled): a gap in the closed
		// faces lies between such cells.  Reaching a back cell without crossing a closed face is a
		// path of open faces through the sheet.
		std::map<std::size_t, int> side;   // 1 front, -1 back, 0 both, 2 a neighbour (no side)
		for (const SheetCrossing& c : sheetFaces_) {
			if (c.sheet != s) continue;
			const std::uint32_t* v = &sh.triangles[3*std::size_t(c.triangle)];
			if (rim[v[0]] || rim[v[1]] || rim[v[2]]) continue;
			const auto d = g.FaceDims(c.axis);
			int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
				int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
			const std::size_t R = g.C(idx[0], idx[1], idx[2]);
			--idx[c.axis];
			const std::size_t L = g.C(idx[0], idx[1], idx[2]);
			const int sR = c.normal[c.axis] > 0 ? 1 : -1;
			for (const auto& cs : {std::make_pair(R, sR), std::make_pair(L, -sR)}) {
				auto it = side.find(cs.first);
				if (it == side.end() || it->second == 2) side[cs.first] = cs.second;
				else if (it->second != cs.second) it->second = 0;
			}
			for (int q = 0; q < 12; ++q) {
				int nb[3] = {idx[0], idx[1], idx[2]};
				if (q >= 6) ++nb[c.axis];   // around the plus cell
				nb[(q % 6)/2] += q % 2 ? 1 : -1;
				bool inside = true;
				for (int m = 0; m < 3; ++m) inside = inside && nb[m] >= 0 && nb[m] < g.n[m];
				if (inside) side.emplace(g.C(nb[0], nb[1], nb[2]), 2);
			}
		}
		// Flood fill from the front cells through open faces, among those cells only.
		std::vector<std::size_t> queue;
		std::map<std::size_t, std::uint8_t> seen;
		for (const auto& e : side) if (e.second == 1) { queue.push_back(e.first); seen[e.first] = 1; }
		for (std::size_t head = 0; head < queue.size(); ++head) {
			const std::size_t c = queue[head];
			const int i = int(c % std::size_t(g.n[0])), j = int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])), k = int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])));
			for (int a = 0; a < 3; ++a) for (int dir = 0; dir < 2; ++dir) {
				int f[3] = {i, j, k}, nb[3] = {i, j, k};
				f[a] += dir;
				nb[a] += dir ? 1 : -1;
				if (nb[a] < 0 || nb[a] >= g.n[a]) continue;
				const std::size_t face = g.F(a, f[0], f[1], f[2]);
				if ((!closed[std::size_t(a)].empty() && closed[std::size_t(a)][face]) || !(fraction[std::size_t(a)][face] > 0)) continue;
				const std::size_t n = g.C(nb[0], nb[1], nb[2]);
				const auto it = side.find(n);
				if (it == side.end() || it->second == 0 || seen.count(n)) continue;
				seen[n] = 1;
				if (it->second == -1) ++holes;
				queue.push_back(n);
			}
		}
	}
	return holes;
}

void LiquidModel::OpenUnwetted()
{
	// m7g: a face a coupled sheet closes between a wet cell W and a dry cell D is a contact of the
	// liquid with the sheet only if the liquid reaches the sheet: the free surface, at theta h from
	// W's centre (theta = phi_W / (phi_W - phi_D), the projection's own ghost-fluid position), lies
	// at or beyond the crossing.  Where it lies short of it, air separates the liquid from the
	// sheet there, and the face is the liquid's free surface (open, p = 0 at theta), not a wall
	// carrying the sheet: closed, the wet cell's pressure acted on the sheet across the air gap
	// (a flap's light edge 3 cm above still water was kicked to 1.15 m/s in one step by the
	// pressure of the cell below a face it closed 1.2 cm above the water;
	// docs/SIMULATION_M7G_RESULTS.md).  The face stays a barrier of the level set and of the
	// sweeps; it is closed again after the step's projections (CloseUnwetted).  So the rule acts
	// where a free surface meets the sheet (cells with an interface of their own): beside a sheet
	// holding liquid on one side the level set is the distance to the sheet, contact by definition.
	sheetUnwetted_.clear();
	const Grid& g = setup_.grid;
	const double h = g.h, tol = 1e-2*g.h;
	for (std::size_t k = 0; k < sheetFaces_.size(); ++k) {
		const SheetCrossing& c = sheetFaces_[k];
		if (!sheets[c.sheet].Coupled()) continue;
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		const std::size_t R = g.C(idx[0], idx[1], idx[2]);
		const double xR = g.origin[std::size_t(c.axis)] + (idx[c.axis] + 0.5)*h;
		--idx[c.axis];
		const std::size_t L = g.C(idx[0], idx[1], idx[2]);
		const double pl = phi[L], pr = phi[R];
		if ((pl < 0) == (pr < 0)) continue;
		// The boundary values beside a sheet holding liquid on one side (PinSheetInterface) are
		// contact: a cell beside several closed faces holds its least distance, which is not
		// theta along the others (opened there, the bag of the results drained through them
		// each step and the volume correction put back 1.4 times its volume per second).
		if (!barrier_.fixed.empty() && (barrier_.fixed[L] || barrier_.fixed[R])) continue;
		// Only a free surface below the sheet (the dry cell above the wet one against gravity): liquid
		// on a sheet, with the level set's zero short of the crossing by a fraction of a cell (thin
		// films, where the cells beside the sheet hold the free surface's values), rests on it under
		// gravity and its weight must reach the sheet (opened there, a 3 cm layer drained into the
		// faces and the correction put back a third of its volume per second).
		const double up = setup_.gravity[std::size_t(c.axis)]*(pl < 0 ? 1.0 : -1.0);   // g . (dry - wet)
		if (!(up < 0)) continue;
		const double theta = pl < 0 ? pl/(pl - pr) : pr/(pr - pl);   // from the wet cell
		const double reach = pl < 0 ? c.s - (xR - h) : xR - c.s;   // the wet cell's centre to the crossing
		if (!(theta*h < reach - tol)) continue;
		if (sheetUnwetted_.empty()) sheetUnwetted_.assign(sheetFaces_.size(), 0);
		sheetUnwetted_[k] = 1;
		fraction[std::size_t(c.axis)][c.face] = sheetSaved_[k];
	}
}

void LiquidModel::CloseUnwetted()
{
	for (std::size_t k = 0; k < sheetUnwetted_.size(); ++k)
		if (sheetUnwetted_[k]) fraction[std::size_t(sheetFaces_[k].axis)][sheetFaces_[k].face] = 0;
	sheetUnwetted_.clear();
}

void LiquidModel::ExtrapolateEnclosed()
{
	// Cells a body encloses (every face closed) take no part in the pressure
	// solve, but the level-set stencils read them, and the body's motion
	// carries their values into the cut cells beside it.  Each takes the
	// liquid's level set extended from outside the body along its normal: at
	// x_o = x + (1.5 h - d) n (d < 0 the body's distance, n its outward
	// normal), phi_o + (x - x_o) . grad phi_o, the correction limited to
	// |x - x_o| (exact for a plane; the trilinear stencil at x_o lies outside
	// the enclosed cells).  Left as advected, the inside of a moving body
	// turned the liquid under it into air within a few steps
	// (docs/SIMULATION_M7B_RESULTS.md section 3).
	const Grid& g = setup_.grid;
	const auto& E = geometry_.EnclosedCells();
	if (E.empty()) return;
	const std::vector<double> old = phi;
	std::vector<Rigid::M3> R;
	for (const LiquidBody& b : bodies) R.push_back(Rigid::QMatrix(b.state.q));
	auto distance = [&](std::size_t i, const V3& p) {
		const V3 d{{p[0] - bodies[i].state.x[0], p[1] - bodies[i].state.x[1], p[2] - bodies[i].state.x[2]}};
		return bodies[i].shape.Distance(Rigid::ApplyT(R[i], d));
	};
	ForRows(pool_.get(), E.size(), 64, [&](std::size_t e0, std::size_t e1) {
		for (std::size_t e = e0; e < e1; ++e) {
			const std::size_t c = E[e];
			const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
				int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
			const V3 x = g.CellCentre(idx[0], idx[1], idx[2]);
			std::size_t best = 0;
			double d = 1e300;
			for (std::size_t i = 0; i < bodies.size(); ++i) { const double di = distance(i, x); if (di < d) { d = di; best = i; } }
			if (!(d < 0)) continue;
			V3 n;
			const double eps = 1e-3*g.h;
			for (int a = 0; a < 3; ++a) {
				V3 p = x, q = x;
				p[std::size_t(a)] += eps; q[std::size_t(a)] -= eps;
				n[std::size_t(a)] = (distance(best, p) - distance(best, q))/(2*eps);
			}
			const double len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
			if (!(len > 0)) continue;
			for (double& v : n) v /= len;
			const double reach = 1.5*g.h - d;
			const V3 xo{{x[0] + reach*n[0], x[1] + reach*n[1], x[2] + reach*n[2]}};
			const double po = SampleCells(g, old, xo);
			double slope = 0;
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] == 1) continue;
				V3 p = xo, q = xo;
				p[std::size_t(a)] += 0.5*g.h; q[std::size_t(a)] -= 0.5*g.h;
				slope += (x[std::size_t(a)] - xo[std::size_t(a)])*(SampleCells(g, old, p) - SampleCells(g, old, q))/g.h;
			}
			phi[c] = po + std::min(reach, std::max(-reach, slope));
		}
	});
}

bool LiquidModel::ProjectBodies(MacField& s, const PressureOptions& po, const std::vector<Vec6>& wStar, std::vector<Vec6>& impulse,
	PressureStats& stats, std::string& error)
{
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	if (!sheets.empty()) ClearSheetVelocity(solidVelocity_);
	geometry_.SolidVelocity(wStar, solidVelocity_);
	if (!sheets.empty()) SetSheetVelocity(solidVelocity_);
	std::vector<std::unique_ptr<BodyCoupling>> couplings;
	std::vector<IPressureCoupling*> coupled;
	bool kinematic = false;
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		couplings.emplace_back(new BodyCoupling(g, geometry_.Faces()[i], bodies[i], setup_.density, bodies[i].Coupled()));
		if (bodies[i].Coupled()) coupled.push_back(couplings.back().get());
		else kinematic = true;
	}
	if (!Project(g, phi, fraction, &solidVelocity_, po, coupled, s, q_, valid, stats, pool, error)) return false;
	std::vector<std::uint8_t> active;
	if (kinematic) ActiveCells(g, phi, fraction, active, pool);
	impulse.assign(bodies.size(), Vec6{{0, 0, 0, 0, 0, 0}});
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		if (bodies[i].Coupled()) impulse[i] = couplings[i]->Impulse();
		else { couplings[i]->Begin(active); impulse[i] = couplings[i]->Measure(q_); }
	}
	return true;
}

bool LiquidModel::ProjectCoupled(MacField& s, const PressureOptions& po, const std::vector<Vec6>& wStar,
	const std::vector<std::vector<double>>& vStar, std::vector<Vec6>& impulse, std::vector<std::vector<double>>& sheetImp,
	PressureStats& stats, std::string& error)
{
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	ClearSheetVelocity(solidVelocity_);
	if (!bodies.empty()) geometry_.SolidVelocity(wStar, solidVelocity_);
	SheetCrossingVelocities(sheets, vStar, sheetFaces_);
	SetSheetVelocity(solidVelocity_);
	std::vector<std::unique_ptr<BodyCoupling>> couplings;
	std::vector<IPressureCoupling*> coupled;
	bool kinematic = false;
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		couplings.emplace_back(new BodyCoupling(g, geometry_.Faces()[i], bodies[i], setup_.density, bodies[i].Coupled()));
		if (bodies[i].Coupled()) coupled.push_back(couplings.back().get());
		else kinematic = true;
	}
	SheetCoupling sheet(g, sheetFaces_, sheets, setup_.density, &sheetUnwetted_);
	coupled.push_back(&sheet);
	if (!Project(g, phi, fraction, &solidVelocity_, po, coupled, s, q_, valid, stats, pool, error)) return false;
	std::vector<std::uint8_t> active;
	if (kinematic) ActiveCells(g, phi, fraction, active, pool);
	impulse.assign(bodies.size(), Vec6{{0, 0, 0, 0, 0, 0}});
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		if (bodies[i].Coupled()) impulse[i] = couplings[i]->Impulse();
		else { couplings[i]->Begin(active); impulse[i] = couplings[i]->Measure(q_); }
	}
	sheetImp = sheet.Impulse();
	return true;
}

void LiquidModel::SheetFaceMass(const MacField& s, std::vector<std::vector<SheetBlock>>& blocks, std::vector<std::vector<double>>& rhs) const
{
	// The liquid on a face a coupled sheet closes moves with the sheet's normal velocity
	// G_f V: its kinetic energy rho h^3 w_f (G_f V - u*_f)^2 / 2, w_f = A_f theta_f the
	// projection's weight (A_f the face's fraction before the sheet closed it), joins the
	// sheet's step: F = rho h^3 sum w_f G_f^T G_f, right-hand side rho h^3 sum w_f G_f^T u*_f.
	// Without it the weight of that liquid would not be carried: across a closed face the
	// pressure jumps by its hydrostatic difference over the cell, which acted on the sheet as
	// a buoyancy of rho g h n_a^2 per area (as if it were a slab a cell thick).
	// m7g: the face's liquid moves with the sheet along the sheet's normal only, its normal
	// velocity the normal part of its velocity along the axis: energy rho h^3 w_f (n_w . V - n_a u*_f)^2 / 2,
	// n_w . V = sum_m w_m V_m . n.  F = rho h^3 sum w_f (w n)(w n)^T; the right-hand side (the
	// momentum it brings, rho h^3 w_f u*_f n_a n per vertex weight) is m7e's.  m7e's energy
	// rho h^3 w_f ((n_w . V) n_a - u*_f)^2 / 2 made the sheet's normal velocity u*_f / n_a: on a
	// face whose axis lies nearly in the sheet (n_a small, the staircase's faces beside tangential
	// flow) the liquid sliding along the sheet drove a light cloth's vertex across it at 1 / n_a
	// times its speed (a 0.05 kg/m^2 flap's edge at 10 m/s in a 2 m/s surge, n_a = 0.16;
	// docs/SIMULATION_M7G_RESULTS.md).  The two agree for an axis-aligned sheet.
	const Grid& g = setup_.grid;
	const double scale = setup_.density*g.h*g.h*g.h, thetaMin = options_.pressure.thetaMin;
	blocks.assign(sheets.size(), {});
	rhs.assign(sheets.size(), {});
	for (std::size_t i = 0; i < sheets.size(); ++i) if (sheets[i].Coupled()) rhs[i].assign(3*sheets[i].dyn.Vertices(), 0.0);
	for (std::size_t k = 0; k < sheetFaces_.size(); ++k) {
		const SheetCrossing& c = sheetFaces_[k];
		const LiquidSheet& sh = sheets[c.sheet];
		if (!sh.Coupled() || (!sheetUnwetted_.empty() && sheetUnwetted_[k])) continue;
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		const double pr = phi[g.C(idx[0], idx[1], idx[2])];
		--idx[c.axis];
		const double pl = phi[g.C(idx[0], idx[1], idx[2])];
		double theta = 0;
		if (pl < 0 && pr < 0) theta = 1;
		else if (pl < 0) theta = std::min(1.0, std::max(thetaMin, pl/(pl - pr)));
		else if (pr < 0) theta = std::min(1.0, std::max(thetaMin, pr/(pr - pl)));
		const double w = scale*theta*sheetSaved_[k];
		if (!(w > 0)) continue;
		double cv[3][3], cn[3][3];
		std::uint32_t v[3];
		for (int m = 0; m < 3; ++m) {
			v[m] = sh.triangles[3*std::size_t(c.triangle) + std::size_t(m)];
			for (int a = 0; a < 3; ++a) {
				cv[m][a] = c.w[m]*c.normal[c.axis]*c.normal[a];
				cn[m][a] = c.w[m]*c.normal[a];
			}
		}
		const double us = s.u[c.axis][c.face];
		for (int m = 0; m < 3; ++m) {
			for (int a = 0; a < 3; ++a) rhs[c.sheet][3*std::size_t(v[m]) + std::size_t(a)] += w*cv[m][a]*us;
			for (int n = m; n < 3; ++n) {
				SheetBlock e;
				const int p = v[m] <= v[n] ? m : n, q = v[m] <= v[n] ? n : m;
				e.i = v[p]; e.j = v[q];
				for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) {
					e.b[3*r + cc] = w*cn[p][r]*cn[q][cc];
					if (m != n && e.i == e.j) e.b[3*r + cc] += w*cn[q][r]*cn[p][cc];   // a repeated vertex
				}
				blocks[c.sheet].push_back(e);
			}
		}
	}
}

std::size_t LiquidModel::PinSheetInterface(std::vector<std::uint8_t>* mask)
{
	// m7f: where a coupled sheet holds liquid on one side only, the liquid's boundary is the
	// sheet: the level set is the signed distance to it there, its zero at the sheet.  Across a
	// closed face whose two cells differ in sign, each takes its distance to the crossed
	// triangle's plane, |s - x_c| |n_a| (n the unit normal), negative on the wet side; a cell
	// beside several such faces takes the least.  The faces between the cells set become
	// boundary faces of the level set's stencils (LevelSetBarrier): the reinitialisation, the
	// advection and the volume measure see the liquid end at the sheet.  The cells set are
	// boundary values only and are left out of the volume correction (`mask`).
	// A cell with a cell of the other sign within two steps across open faces has an
	// interface of its own (where a free surface meets the sheet: the waterline of a sheet with
	// liquid on both sides, where the free surface and the sheet can cross the same segment)
	// and keeps its value, from which the free surface is interpolated (pinned there, a flap's
	// waterline moved to the sheet every step and the flap's speed in still water doubled).
	// m7e pinned the dry side only, after the volume correction, with stencils reading across
	// the sheet; a pinned bag (3-D, 24 cells, 3 cm of water) lost 22% of its water to the
	// correction in 2 s that way, and 40% with sided stencils and no pin (the small values the
	// sheet left on the dry side spread into it and were counted as liquid).
	const Grid& g = setup_.grid;
	const double h = g.h;
	std::vector<std::pair<std::size_t, double>> set;   // (cell, distance), negative distance: wet
	for (const SheetCrossing& c : sheetFaces_) {
		if (!sheets[c.sheet].Coupled()) continue;
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		const std::size_t R = g.C(idx[0], idx[1], idx[2]);
		const double xR = g.origin[std::size_t(c.axis)] + (idx[c.axis] + 0.5)*h;
		--idx[c.axis];
		const std::size_t L = g.C(idx[0], idx[1], idx[2]);
		const bool wL = phi[L] < 0, wR = phi[R] < 0;
		if (wL == wR) continue;
		const double na = std::fabs(c.normal[c.axis]);
		const double dL = std::max(1e-3*h, (c.s - (xR - h))*na), dR = std::max(1e-3*h, (xR - c.s)*na);
		set.emplace_back(L, wL ? -dL : dL);
		set.emplace_back(R, wR ? -dR : dR);
	}
	std::sort(set.begin(), set.end(), [](const std::pair<std::size_t, double>& p, const std::pair<std::size_t, double>& q) {
		return p.first != q.first ? p.first < q.first : std::fabs(p.second) < std::fabs(q.second); });
	std::size_t changed = 0;
	if (mask) mask->assign(g.Cells(), 0);
	std::vector<std::pair<std::size_t, double>> apply;
	for (std::size_t k = 0; k < set.size(); ++k) {
		if (k > 0 && set[k].first == set[k - 1].first) continue;   // the least distance (first after sorting)
		const std::size_t c = set[k].first;
		if (OwnInterface(c)) continue;
		if (mask) (*mask)[c] = 1;
		apply.emplace_back(c, set[k].second);
	}
	for (const auto& e : apply) if (phi[e.first] != e.second) { phi[e.first] = e.second; ++changed; }
	BuildBarrier();   // the faces between the cells set are boundary faces now
	return changed;
}

void LiquidModel::Redistance(std::size_t c)
{
	// m7f: the fast-marching update of one cell from its face neighbours on its own side (not
	// across a coupled sheet's closed face): |phi| solves sum_k max(0, |phi| - a_k)^2 = h^2, a_k
	// the least |phi| of the neighbours along axis k.  A cell with a neighbour of the other sign
	// across an open face is beside the free surface and keeps its value.
	const Grid& g = setup_.grid;
	const double h = g.h;
	const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
		int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
	const bool neg = phi[c] < 0;
	double a[3];
	int n = 0;
	for (int k = 0; k < 3; ++k) {
		if (g.n[k] == 1) continue;
		double m = std::numeric_limits<double>::infinity();
		for (int side = 0; side < 2; ++side) {
			int f[3] = {idx[0], idx[1], idx[2]}, nb[3] = {idx[0], idx[1], idx[2]};
			f[k] += side;
			nb[k] += side ? 1 : -1;
			if (nb[k] < 0 || nb[k] >= g.n[k]) continue;
			if (barrier_.Closed(k, g.F(k, f[0], f[1], f[2]))) continue;
			const double p = phi[g.C(nb[0], nb[1], nb[2])];
			if ((p < 0) != neg) return;
			m = std::min(m, std::fabs(p));
		}
		if (std::isfinite(m)) a[n++] = m;
	}
	if (n == 0) return;
	std::sort(a, a + n);
	double v = a[0] + h;
	if (n > 1 && v > a[1]) {
		const double d = a[0] - a[1];
		v = 0.5*(a[0] + a[1] + std::sqrt(std::max(0.0, 2*h*h - d*d)));
		if (n > 2 && v > a[2]) {
			const double s = a[0] + a[1] + a[2], q = a[0]*a[0] + a[1]*a[1] + a[2]*a[2];
			v = (s + std::sqrt(std::max(0.0, s*s - 3*(q - h*h))))/3;
		}
	}
	phi[c] = neg ? -v : v;
}

std::pair<std::size_t, std::size_t> LiquidModel::SweepSheetCells(const std::vector<SheetCrossing>& facesBefore)
{
	// m7e: a cell whose centre a coupled sheet swept over changed sides.  Its level set is
	// that of its new side (the cell beyond the sheet's old crossing) extended to it (m7f: as a
	// constant; m7e extended a zero contour at the sheet with unit slope, which the sided level
	// set no longer has); and the marker particles of every cell beside a coupled sheet's closed face,
	// before or now, go (a particle the sheet moved over without its cell's centre changing
	// sides is on the wrong side of it).  Left as they were, a membrane holding a layer
	// leaked half of it within 0.4 s as it oscillated across cell centres.
	const Grid& g = setup_.grid;
	std::vector<SweptCell> swept;
	SweptSheetCells(facesBefore, swept);
	const std::vector<double> old = phi;
	for (const SweptCell& c : swept) sweptCells_.push_back(c.cell);   // m7g (CountSheetCrossings)
	for (const SweptCell& c : swept) {
		// m7f: the new side's value beyond the old crossing (constant extension, as the level
		// set's stencils see across a closed face; a linear one carried the distance to the
		// sheet that the other side's values hold onto the swept cells).
		phi[c.cell] = old[c.ref];
	}
	// m7f: the boundary values at the sheet's new position (PinSheetInterface), then the cells
	// the sheet left (beside its crossings before and not now, and swept cells not beside it
	// now) re-distanced from their own side: their values were distances to where the sheet
	// had been (left stale, the cells a sagging sheet uncovers keep values near zero inside the
	// liquid; the effect on the bags of the results was below what they resolve).
	PinSheetInterface();
	{
		auto beside = [&](const std::vector<SheetCrossing>& list, std::vector<std::size_t>& out) {
			for (const SheetCrossing& c : list) {
				if (!sheets[c.sheet].Coupled()) continue;
				const auto d = g.FaceDims(c.axis);
				int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
					int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
				out.push_back(g.C(idx[0], idx[1], idx[2]));
				--idx[c.axis];
				out.push_back(g.C(idx[0], idx[1], idx[2]));
			}
			std::sort(out.begin(), out.end());
			out.erase(std::unique(out.begin(), out.end()), out.end());
		};
		std::vector<std::size_t> before, now, stale;
		beside(facesBefore, before);
		beside(sheetFaces_, now);
		for (std::size_t c : before) if (!std::binary_search(now.begin(), now.end(), c)) stale.push_back(c);
		for (const SweptCell& c : swept) if (!std::binary_search(now.begin(), now.end(), c.cell)) stale.push_back(c.cell);
		std::sort(stale.begin(), stale.end());
		stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
		for (int pass = 0; pass < 2; ++pass) for (std::size_t c : stale) Redistance(c);
	}
	std::size_t removed = 0;
	if (options_.particles) {
		std::vector<std::size_t> cells;
		for (const SweptCell& c : swept) cells.push_back(c.cell);
		for (const std::vector<SheetCrossing>* list : {&facesBefore, static_cast<const std::vector<SheetCrossing>*>(&sheetFaces_)})
			for (const SheetCrossing& c : *list) {
				if (!sheets[c.sheet].Coupled()) continue;
				const auto d = g.FaceDims(c.axis);
				int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
					int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
				// m7f: the cells within two steps of the two (a particle's trilinear stencil reaches a
				// diagonal neighbour, which may lie across the sheet, and its corrections near the
				// sheet's boundary values fought them: a pinned bag at rest gained 12% of its volume
				// per second that the correction then took from the free surface).
				for (int side = 0; side < 2; ++side) {
					if (side) --idx[c.axis];
					for (int a = -2; a <= 2; ++a) for (int b = -2; b <= 2; ++b) for (int e = -2; e <= 2; ++e) {
						if (std::abs(a) + std::abs(b) + std::abs(e) > 2) continue;
						const int q[3] = {idx[0] + a, idx[1] + b, idx[2] + e};
						if (q[0] >= 0 && q[0] < g.n[0] && q[1] >= 0 && q[1] < g.n[1] && q[2] >= 0 && q[2] < g.n[2]) cells.push_back(g.C(q[0], q[1], q[2]));
					}
				}
			}
		std::sort(cells.begin(), cells.end());
		cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
		auto& ps = particles.Particles();
		std::size_t out = 0;
		for (std::size_t i = 0; i < ps.size(); ++i) {
			int idx[3];
			for (int a = 0; a < 3; ++a)
				idx[a] = std::min(g.n[a] - 1, std::max(0, int(std::floor((ps[i].x[std::size_t(a)] - g.origin[std::size_t(a)])/g.h))));
			if (!std::binary_search(cells.begin(), cells.end(), g.C(idx[0], idx[1], idx[2]))) ps[out++] = ps[i];
		}
		removed = ps.size() - out;
		ps.resize(out);
	}
	return {swept.size(), removed};
}

void LiquidModel::SweptSheetCells(const std::vector<SheetCrossing>& before, std::vector<SweptCell>& swept) const
{
	const Grid& g = setup_.grid;
	swept.clear();
	// Per (axis, line): the coupled sheets' crossing coordinates, before and now.
	using Key = std::tuple<int, std::size_t>;   // axis, the face index with the axis coordinate zeroed
	auto key = [&](const SheetCrossing& c, int& along) {
		const auto d = g.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		along = idx[c.axis];
		idx[c.axis] = 0;
		return Key(c.axis, g.F(c.axis, idx[0], idx[1], idx[2]));
	};
	std::map<Key, std::pair<std::vector<double>, std::vector<double>>> lines;
	int along = 0;
	for (const SheetCrossing& c : before) if (sheets[c.sheet].Coupled()) lines[key(c, along)].first.push_back(c.s);
	for (const SheetCrossing& c : sheetFaces_) if (sheets[c.sheet].Coupled()) lines[key(c, along)].second.push_back(c.s);
	for (auto& e : lines) {
		auto& a = e.second.first;
		auto& b = e.second.second;
		if (a.size() != b.size() || a.empty()) continue;   // a fold appeared or vanished: not paired
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		const int axis = std::get<0>(e.first);
		const auto d = g.FaceDims(axis);
		const std::size_t f0 = std::get<1>(e.first);
		int idx[3] = {int(f0 % std::size_t(d[0])), int((f0/std::size_t(d[0])) % std::size_t(d[1])), int(f0/(std::size_t(d[0])*std::size_t(d[1])))};
		for (std::size_t m = 0; m < a.size(); ++m) {
			const double s0 = a[m], s1 = b[m];
			if (s0 == s1) continue;
			const double o = g.origin[std::size_t(axis)];
			// Cells with centres strictly between s0 and s1; the new side lies beyond s0, away from s1.
			const int dir = s0 > s1 ? 1 : -1;
			int lo = int(std::ceil((std::min(s0, s1) - o)/g.h - 0.5)), hi = int(std::floor((std::max(s0, s1) - o)/g.h - 0.5));
			lo = std::max(0, lo); hi = std::min(g.n[axis] - 1, hi);
			int ref = dir > 0 ? hi + 1 : lo - 1;
			if (ref < 0 || ref >= g.n[axis]) continue;
			for (int i = lo; i <= hi; ++i) {
				const double centre = o + (i + 0.5)*g.h;
				if (!((centre - s0)*(centre - s1) < 0)) continue;
				int c[3] = {idx[0], idx[1], idx[2]}, r[3] = {idx[0], idx[1], idx[2]};
				c[axis] = i; r[axis] = ref;
				SweptCell w;
				w.cell = g.C(c[0], c[1], c[2]);
				w.ref = g.C(r[0], r[1], r[2]);
				w.distance = std::fabs(centre - s1);
				w.span = std::abs(ref - i)*g.h;
				swept.push_back(w);
			}
		}
	}
	std::sort(swept.begin(), swept.end(), [](const SweptCell& p, const SweptCell& q) {
		return p.cell != q.cell ? p.cell < q.cell : p.distance < q.distance; });
	swept.erase(std::unique(swept.begin(), swept.end(), [](const SweptCell& p, const SweptCell& q) { return p.cell == q.cell; }), swept.end());
}

bool LiquidModel::CoupledSheets() const
{
	for (const LiquidSheet& sh : sheets) if (sh.Coupled()) return true;
	return false;
}

void LiquidModel::ResetSheetDynamics()
{
	std::vector<std::vector<double>> v(sheets.size());
	for (std::size_t i = 0; i < sheets.size(); ++i) {
		if (!sheets[i].Coupled()) continue;
		sheets[i].dyn.x0 = sheets[i].x;
		sheets[i].dyn.xe = sheets[i].x;
		sheets[i].dyn.ResetElements();   // m7h

		v[i] = sheets[i].v;
	}
	SheetCrossingVelocities(sheets, v, sheetFaces_);
}

void LiquidModel::AuditWalls()
{
	// The impulse on the liquid from the closed parts of the faces of its cells
	// that belong to the walls and static solids: -rho h^3 sum_c q_c sum_f s (1 - A) e_a.
	const Grid& g = setup_.grid;
	std::array<std::vector<std::size_t>,3> bodyFaces;
	for (const auto& list : geometry_.Faces()) for (const BodyFace& f : list) bodyFaces[f.axis].push_back(f.face);
	for (const SheetCrossing& c : sheetFaces_) bodyFaces[c.axis].push_back(c.face);   // m7d: the sheets' impulse is theirs
	for (auto& v : bodyFaces) std::sort(v.begin(), v.end());
	std::vector<std::uint8_t> active;
	ActiveCells(g, phi, fraction, active, pool_.get());
	V3 sum{{0, 0, 0}};
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const std::size_t c = g.C(i, j, k);
		if (!active[c]) continue;
		const int idx[3] = {i, j, k};
		for (int a = 0; a < 3; ++a) for (int side = 0; side < 2; ++side) {
			int f3[3] = {idx[0], idx[1], idx[2]};
			f3[a] += side;
			const std::size_t f = g.F(a, f3[0], f3[1], f3[2]);
			if (std::binary_search(bodyFaces[a].begin(), bodyFaces[a].end(), f)) continue;
			const double closed = 1 - fraction[a][f];
			if (closed == 0) continue;
			sum[std::size_t(a)] -= (side ? 1.0 : -1.0)*closed*q_[c];
		}
	}
	const double scale = setup_.density*g.h*g.h*g.h;
	for (double& x : sum) x *= scale;
	wallImpulse = sum;
}

V3 LiquidModel::FluidMomentum() const
{
	V3 p{{0, 0, 0}};
	const Grid& g = setup_.grid;
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		double s = 0;
		const auto d = g.FaceDims(a);
		for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
			const int idx[3] = {i, j, k};
			if (idx[a] == 0 || idx[a] == g.n[a]) continue;
			const std::size_t f = g.F(a, i, j, k);
			const double A = fraction[a][f];
			if (!(A > 0)) continue;
			int lo[3] = {i, j, k};
			--lo[a];
			const double pl = phi[g.C(lo[0], lo[1], lo[2])], pr = phi[g.C(i, j, k)];
			double w = 0;
			if (pl < 0 && pr < 0) w = A;
			else if (pl < 0) w = A*std::min(1.0, std::max(options_.pressure.thetaMin, pl/(pl - pr)));
			else if (pr < 0) w = A*std::min(1.0, std::max(options_.pressure.thetaMin, pr/(pr - pl)));
			s += w*u.u[a][f];
		}
		p[std::size_t(a)] = s*setup_.density*g.h*g.h*g.h;
	}
	return p;
}

double LiquidModel::FluidMass(int axis) const
{
	const Grid& g = setup_.grid;
	double s = 0;
	const auto d = g.FaceDims(axis);
	for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
		const int idx[3] = {i, j, k};
		if (idx[axis] == 0 || idx[axis] == g.n[axis]) continue;
		const std::size_t f = g.F(axis, i, j, k);
		const double A = fraction[axis][f];
		if (!(A > 0)) continue;
		int lo[3] = {i, j, k};
		--lo[axis];
		const double pl = phi[g.C(lo[0], lo[1], lo[2])], pr = phi[g.C(i, j, k)];
		if (pl < 0 && pr < 0) s += A;
		else if (pl < 0) s += A*std::min(1.0, std::max(options_.pressure.thetaMin, pl/(pl - pr)));
		else if (pr < 0) s += A*std::min(1.0, std::max(options_.pressure.thetaMin, pr/(pr - pl)));
	}
	return s*setup_.density*g.h*g.h*g.h;
}

// ------------------------------------------------------------------ outputs

MacField LiquidModel::OutputVelocity() const
{
	// The field the next step's interface moves with: invalid faces (air, and
	// narrow cut faces whose projected speed is not transported) re-extrapolated.
	MacField v = u;
	FaceMask known = valid;
	ExtrapolateVelocity(setup_.grid, v, known, options_.interfaceExtrapolationLayers, InterfaceLinearLayers(), pool_.get());
	return v;
}

bool LiquidModel::Surface(MeshFrame& frame, std::string& error) const
{
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	const std::array<int,3> d{{g.n[0] + 2, g.n[1] + 2, g.n[2] + 2}};
	std::vector<double> f(std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]));
	const bool solids = HasSolids();
	// Interior: phi with the solids carved out half a cell inside their surface;
	// the ghost layer |phi| of the nearest cell closes the surface on the walls.
	ForRows(pool, std::size_t(d[1])*std::size_t(d[2]), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int J = int(r % std::size_t(d[1])), K = int(r/std::size_t(d[1]));
			for (int I = 0; I < d[0]; ++I) {
				const int i = std::min(std::max(I - 1, 0), g.n[0] - 1), j = std::min(std::max(J - 1, 0), g.n[1] - 1),
					k = std::min(std::max(K - 1, 0), g.n[2] - 1);
				double v = phi[g.C(i, j, k)];
				if (solids) {
					double s = 0;
					for (int m = 0; m < 8; ++m) s += solidNodes[g.N(i + (m & 1), j + ((m >> 1) & 1), k + ((m >> 2) & 1))];
					v = std::max(v, -(0.125*s + 0.5*g.h));
				}
				const bool ghost = I == 0 || J == 0 || K == 0 || I == d[0] - 1 || J == d[1] - 1 || K == d[2] - 1;
				f[Index(d, I, J, K)] = ghost ? std::fabs(v) + 1e-12*g.h : v;
			}
		}
	});
	IsoSurface iso;
	const V3 origin{{g.origin[0] - 0.5*g.h, g.origin[1] - 0.5*g.h, g.origin[2] - 0.5*g.h}};
	MarchingCubes(d, f, origin, g.h, iso, pool);
	const std::size_t ne = iso.edgeVertices, nv = iso.positions.size()/3;
	frame = MeshFrame();
	frame.positionUnits = "m";
	frame.velocityUnits = "m/s";
	frame.positions.resize(3*nv);
	frame.velocities.resize(3*nv);
	frame.normals.resize(3*nv);
	const MacField vf = OutputVelocity();
	const std::size_t strides[3] = {1, std::size_t(d[0]), std::size_t(d[0])*std::size_t(d[1])};
	auto gradient = [&](std::size_t s, V3& gr) {
		const int idx[3] = {int(s % std::size_t(d[0])), int((s/std::size_t(d[0])) % std::size_t(d[1])),
			int(s/(std::size_t(d[0])*std::size_t(d[1])))};
		for (int a = 0; a < 3; ++a) {
			const std::size_t lo = idx[a] > 0 ? s - strides[a] : s, hi = idx[a] + 1 < d[a] ? s + strides[a] : s;
			const double span = double((idx[a] > 0) + (idx[a] + 1 < d[a]));
			gr[a] = span > 0 ? (f[hi] - f[lo])/(span*g.h) : 0.0;
		}
	};
	auto edgeNormal = [&](std::size_t v) {
		const std::size_t s = std::size_t(iso.edges[v]/3);
		const int a = int(iso.edges[v] % 3);
		V3 ga, gb;
		gradient(s, ga);
		gradient(s + strides[a], gb);
		const double t = iso.t[v];
		return V3{{ga[0] + t*(gb[0] - ga[0]), ga[1] + t*(gb[1] - ga[1]), ga[2] + t*(gb[2] - ga[2])}};
	};
	ForRows(pool, nv, 1024, [&](std::size_t v0, std::size_t v1) {
		for (std::size_t v = v0; v < v1; ++v) {
			V3 n{{0, 0, 0}};
			if (v < ne) n = edgeNormal(v);
			else {
				// A centroid: the mean gradient of its loop.
				const std::size_t q = v - ne;
				for (std::uint32_t m = iso.loopStart[q]; m < iso.loopStart[q + 1]; ++m) {
					const V3 e = edgeNormal(iso.loopMembers[m]);
					for (int b = 0; b < 3; ++b) n[b] += e[b];
				}
			}
			const double len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
			if (len > 0) for (double& x : n) x /= len;
			else n = {{0, 1, 0}};
			const V3 x{{iso.positions[3*v], iso.positions[3*v + 1], iso.positions[3*v + 2]}};
			const V3 vel = SampleVelocity(g, vf, x);
			for (int b = 0; b < 3; ++b) {
				frame.positions[3*v + std::size_t(b)] = Bits(x[b]);
				frame.velocities[3*v + std::size_t(b)] = Bits(vel[b]);
				frame.normals[3*v + std::size_t(b)] = Bits(n[b]);
			}
		}
	});
	frame.indices = std::move(iso.triangles);
	if (nv == 0) { frame.velocityUnits.clear(); frame.normals.clear(); }   // an empty surface (no liquid) is a valid empty mesh
	(void)error;
	return true;
}

bool LiquidModel::Fields(GridFrame& frame, std::string& error) const
{
	const Grid& g = setup_.grid;
	const std::size_t n = g.Cells();
	frame = GridFrame();
	auto channel = [&](const char* name, const char* units, std::uint8_t components, const std::function<double(std::size_t, int)>& value) {
		GridChannel c;
		c.name = name;
		c.units = units;
		c.components = components;
		c.origin = {{0, 0, 0}};
		c.dimensions = {{std::uint32_t(g.n[0]), std::uint32_t(g.n[1]), std::uint32_t(g.n[2])}};
		c.indexToWorld = {{g.h, 0, 0, g.origin[0] + 0.5*g.h, 0, g.h, 0, g.origin[1] + 0.5*g.h, 0, 0, g.h, g.origin[2] + 0.5*g.h, 0, 0, 0, 1}};
		c.background = {{0, 0, 0}};
		GridBlock b;
		b.origin = {{0, 0, 0}};
		b.dimensions = c.dimensions;
		b.constant = false;
		b.active.assign((n + 7)/8, 0xff);
		if (n % 8) b.active.back() = std::uint8_t((1u << (n % 8)) - 1);
		b.values.resize(n*components);
		for (std::size_t i = 0; i < n; ++i) for (int m = 0; m < components; ++m) b.values[components*i + std::size_t(m)] = Bits(value(i, m));
		c.blocks.push_back(std::move(b));
		frame.channels.push_back(std::move(c));
	};
	channel("phi", "m", 1, [&](std::size_t i, int) { return phi[i]; });
	channel("pressure", "Pa", 1, [&](std::size_t i, int) { return Pressure(i); });
	std::array<std::vector<double>,3> c;
	CellVelocity(g, OutputVelocity(), c, pool_.get());
	channel("velocity", "m/s", 3, [&](std::size_t i, int m) { return c[std::size_t(m)][i]; });
	(void)error;
	return true;
}

bool LiquidModel::Points(PointsFrame& frame, std::string& error) const
{
	const Grid& g = setup_.grid;
	frame = PointsFrame();
	frame.lengthUnits = "m";
	const auto& ps = particles.Particles();
	if (ps.empty()) { (void)error; return true; }
	frame.velocityUnits = "m/s";
	PointAttribute sign{"sign", "1", 1, {}};
	const MacField vf = OutputVelocity();
	for (const auto& p : ps) {
		const V3 v = SampleVelocity(g, vf, p.x);
		for (int a = 0; a < 3; ++a) { frame.positions.push_back(Bits(p.x[a])); frame.velocities.push_back(Bits(v[a])); }
		frame.radii.push_back(Bits(p.radius));
		frame.ids.push_back(p.id);
		sign.values.push_back(Bits(p.sign));
	}
	frame.attributes.push_back(std::move(sign));
	(void)error;
	return true;
}

} } }
