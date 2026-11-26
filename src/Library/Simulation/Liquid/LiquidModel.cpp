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
	PressureStats stats;
	if (!Project(g, phi, fraction, nullptr, FinalPressure(), {}, u, q_, valid, stats, pool, error)) return false;
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
	return LiquidVolume(setup_.grid, phi, setup_.solids.empty() ? nullptr : &openCells, pool_.get());
}

double LiquidModel::MaxStep(double segment) const
{
	const Grid& g = setup_.grid;
	const double gn = std::sqrt(setup_.gravity[0]*setup_.gravity[0] + setup_.gravity[1]*setup_.gravity[1] +
		setup_.gravity[2]*setup_.gravity[2]);
	double dt = std::numeric_limits<double>::infinity();
	const double speed = MaxSpeed() + gn*segment;
	if (speed > 0) dt = std::min(dt, options_.cfl*g.h/speed);
	if (gn > 0 && depth_ > 0) dt = std::min(dt, options_.waveCfl*g.h/std::sqrt(gn*depth_));
	return dt;
}

PressureOptions LiquidModel::FinalPressure() const
{
	PressureOptions po = options_.pressure;
	po.minValidFraction = options_.minTransportFraction;
	return po;
}

bool LiquidModel::ProjectState(std::string& error)
{
	PressureStats stats;
	return Project(setup_.grid, phi, fraction, nullptr, FinalPressure(), {}, u, q_, valid, stats, pool_.get(), error);
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
	const bool solids = !setup_.solids.empty();
	const std::vector<double>* open = solids ? &openCells : nullptr;
	double t0 = Now();
	// Two extrapolations of the step-start velocity: for the interface
	// (level set, particles) linear in the first `linearExtrapolationLayers`
	// layers, which makes the surface velocity second-order accurate; for
	// momentum advection constant, which keeps under-resolved films stable
	// (a linear extrapolation ahead of a dam-break front made its film
	// accelerate without bound; docs/SIMULATION_M7A_RESULTS.md section 2).
	MacField ui = u;
	{
		FaceMask known = valid;
		ExtrapolateVelocity(g, ui, known, options_.interfaceExtrapolationLayers, options_.linearExtrapolationLayers, pool);
	}
	ExtrapolateVelocity(g, u, valid, options_.extrapolationLayers, 0, pool);
	double t1 = Now();
	stats.extrapolate = t1 - t0;
	if (options_.particles) {
		particles.Advect(g, [&](double) -> const MacField& { return ui; }, 0.0, dt, pool);
		t0 = Now();
		stats.particles += t0 - t1;
		t1 = t0;
	}
	{
		std::array<std::vector<double>,3> c;
		CellVelocity(g, ui, c, pool);
		AdvectLevelSet(g, phi, [&](double, std::array<std::vector<double>,3>& v) { v = c; }, 0.0, dt, pool);
	}
	t0 = Now();
	stats.levelSet += t0 - t1;
	t1 = t0;
	const MacField un = u;   // the extrapolated step-start velocity (advected below)
	if (options_.particles) stats.escaped = particles.Correct(g, phi, pool);
	t0 = Now();
	stats.particles += t0 - t1;
	stats.reinitialised = options_.reinit != ReinitScheme::None &&
		(options_.reinitThreshold <= 0 || GradientDeviation(g, phi, 3, pool) > options_.reinitThreshold);
	if (stats.reinitialised) Reinitialize(g, phi, options_.reinit, options_.reinitIterations, options_.reinitCfl, pool);
	t1 = Now();
	stats.levelSet += t1 - t0;
	if (options_.particles) {
		particles.Correct(g, phi, pool);
		particles.AdjustRadii(g, phi, pool);
		if ((steps + 1) % std::uint64_t(std::max(1, options_.particle.reseedInterval)) == 0) particles.Reseed(g, phi, steps + 1, pool);
	}
	t0 = Now();
	stats.particles += t0 - t1;
	if (options_.volumeCorrection) stats.volumeShift = CorrectVolume(g, phi, targetVolume_, open, pool);
	t1 = Now();
	stats.levelSet += t1 - t0;
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
	addGravity(s1, 1);
	t0 = Now();
	stats.momentum += t0 - t1;
	// The step's last projection leaves cut faces narrower than minTransportFraction invalid
	// (replaced by extrapolation before the next step transports them); the
	// first stage's keeps every updated face for the second stage.
	PressureOptions po = options_.momentumStages <= 1 ? FinalPressure() : options_.pressure;
	if (!Project(g, phi, fraction, nullptr, po, {}, s1, q_, valid, stats.pressure, pool, error)) return false;
	t1 = Now();
	stats.project += t1 - t0;
	if (options_.momentumStages <= 1) u = std::move(s1);
	else {
		ExtrapolateVelocity(g, s1, valid, options_.extrapolationLayers, 0, pool);
		MacField s2;
		if (weno) {
			LimitedAdvectionStage(g, s1, dt, s2, pool);
			for (int a = 0; a < 3; ++a) if (g.n[a] > 1)
				for (std::size_t c = 0; c < s2.u[a].size(); ++c) s2.u[a][c] = 0.5*un.u[a][c] + 0.5*s2.u[a][c];
			addGravity(s2, 0.5);
		} else {
			MacField mid;
			mid.Resize(g);
			for (int a = 0; a < 3; ++a) for (std::size_t c = 0; c < mid.u[a].size(); ++c) mid.u[a][c] = 0.5*(un.u[a][c] + s1.u[a][c]);
			AdvectFieldAlong(g, un, mid, dt, options_.momentum, s2, pool);
			addGravity(s2, 1);
		}
		t0 = Now();
		stats.momentum += t0 - t1;
		// The step's pressure impulse is q1/2 + q2 (Heun) or q2 (midpoint);
		// q2 starts from q1/2 (Heun) or q1.
		const std::vector<double> q1 = q_;
		for (double& x : q_) x *= weno ? 0.5 : 1.0;
		po = FinalPressure();
		po.warmStart = true;
		PressureStats second;
		if (!Project(g, phi, fraction, nullptr, po, {}, s2, q_, valid, second, pool, error)) return false;
		if (weno) for (std::size_t c = 0; c < q_.size(); ++c) q_[c] += 0.5*q1[c];
		stats.pressure.iterations += second.iterations;
		stats.pressure.residual = std::max(stats.pressure.residual, second.residual);
		stats.pressure.seconds += second.seconds;
		u = std::move(s2);
		t1 = t0;
	}
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
	stats.project += Now() - t1;
	return true;
}

// ------------------------------------------------------------------ outputs

MacField LiquidModel::OutputVelocity() const
{
	// The field the next step's interface moves with: invalid faces (air, and
	// narrow cut faces whose projected speed is not transported) re-extrapolated.
	MacField v = u;
	FaceMask known = valid;
	ExtrapolateVelocity(setup_.grid, v, known, options_.interfaceExtrapolationLayers, options_.linearExtrapolationLayers, pool_.get());
	return v;
}

bool LiquidModel::Surface(MeshFrame& frame, std::string& error) const
{
	const Grid& g = setup_.grid;
	Pool* pool = pool_.get();
	const std::array<int,3> d{{g.n[0] + 2, g.n[1] + 2, g.n[2] + 2}};
	std::vector<double> f(std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]));
	const bool solids = !setup_.solids.empty();
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
