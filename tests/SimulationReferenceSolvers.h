// Two small deterministic CPU reference solvers for the M2 solver contract
// (docs/SIMULATION_M2_DESIGN.md "Reference solvers"). Test code only: they
// prove the hosts are not fire-shaped and exercise grid and mesh outputs.
//
//   test.advection_diffusion  scalar u_t + v.grad u = D lap u + S on a periodic
//                             square; publishes grid.v1; evidence: a single
//                             Fourier mode against its exact solution.
//   test.membrane             fixed-boundary square membrane, rho u_tt = T lap u + p,
//                             on a fixed-topology triangle mesh; publishes mesh.v1
//                             with velocities and normals; evidence: measured
//                             fundamental frequency against (c/2) sqrt(2)/a.
//
// Both honour an externally imposed macro step, offer one port and accept one.
#ifndef RISE_TESTS_SIMULATION_REFERENCE_SOLVERS_H
#define RISE_TESTS_SIMULATION_REFERENCE_SOLVERS_H

#include "../src/Library/Simulation/SimulationSolver.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/SimulationMesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace RISE { namespace Simulation { namespace Reference {

using V = RISECBOR64::Value;
constexpr double Pi = 3.14159265358979323846;
constexpr std::size_t CodecBudget = std::size_t(256) << 20;

inline bool Fail(std::string& error, const std::string& message) { error = message; return false; }

inline std::uint32_t F32(double x)
{
	const float f = static_cast<float>(x);
	std::uint32_t bits;
	std::memcpy(&bits, &f, 4);
	return bits;
}
inline double FromF32(std::uint32_t bits)
{
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

inline Bytes PackDoubles(const std::vector<double>& v)
{
	Bytes out(v.size()*8);
	for (std::size_t i = 0; i < v.size(); ++i) {
		std::uint64_t w;
		std::memcpy(&w, &v[i], 8);
		for (unsigned b = 0; b < 8; ++b) out[8*i+b] = static_cast<unsigned char>(w >> (8*b));
	}
	return out;
}
inline bool UnpackDoubles(const V* value, std::size_t count, std::vector<double>& v)
{
	if (!value || value->GetType() != V::ByteString || value->GetBytes().size() != count*8) return false;
	const auto& b = value->GetBytes();
	v.assign(count, 0);
	for (std::size_t i = 0; i < count; ++i) {
		std::uint64_t w = 0;
		for (unsigned k = 0; k < 8; ++k) w |= std::uint64_t(b[8*i+k]) << (8*k);
		std::memcpy(&v[i], &w, 8);
	}
	return true;
}

// Strict parameter reader: unknown keys are refused; every accepted key is
// written back with a canonical type into the effective parameters.
class Params {
public:
	Params(const V& map, std::string& error) : map_(map), error_(error) {
		ok_ = map.GetType() == V::Map || Fail(error, "parameters must be a map");
	}
	double Real(const char* key, double fallback, bool effective = true) {
		double x = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Float64) x = v->GetFloat();
			else if (v->GetType() == V::UnsignedInteger) x = static_cast<double>(v->GetIntegerArgument());
			else if (v->GetType() == V::NegativeInteger) x = -1.0 - static_cast<double>(v->GetIntegerArgument());
			else ok_ = Fail(error_, std::string("parameter ") + key + " must be a number");
		}
		if (!std::isfinite(x)) ok_ = Fail(error_, std::string("parameter ") + key + " must be finite");
		if (effective) out_.emplace_back(key, V::Float(x));
		return x;
	}
	std::uint64_t Count(const char* key, std::uint64_t fallback, std::uint64_t low, std::uint64_t high, bool effective = true) {
		std::uint64_t n = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::UnsignedInteger) n = v->GetIntegerArgument();
			else if (v->GetType() == V::Float64 && v->GetFloat() >= 0 && std::floor(v->GetFloat()) == v->GetFloat() &&
				v->GetFloat() < 9e15) n = static_cast<std::uint64_t>(v->GetFloat());
			else ok_ = Fail(error_, std::string("parameter ") + key + " must be a count");
		}
		if (n < low || n > high) ok_ = Fail(error_, std::string("parameter ") + key + " out of range");
		if (effective) out_.emplace_back(key, V::Unsigned(n));
		return n;
	}
	std::int64_t Integer(const char* key, std::int64_t fallback) {
		std::int64_t n = fallback;
		if (const V* v = Use(key)) {
			if ((v->GetType() == V::UnsignedInteger || v->GetType() == V::NegativeInteger) && v->GetIntegerArgument() < 1000000)
				n = v->GetType() == V::UnsignedInteger ? static_cast<std::int64_t>(v->GetIntegerArgument()) :
					-1 - static_cast<std::int64_t>(v->GetIntegerArgument());
			else ok_ = Fail(error_, std::string("parameter ") + key + " must be a small integer");
		}
		out_.emplace_back(key, V::Signed(n));
		return n;
	}
	std::string Text(const char* key, const std::string& fallback) {
		std::string s = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Text && !v->GetText().empty()) s = v->GetText();
			else ok_ = Fail(error_, std::string("parameter ") + key + " must be text");
		}
		out_.emplace_back(key, V::String(s));
		return s;
	}
	bool Finish(V& effective) {
		if (!ok_) return false;
		for (const auto& m : map_.GetMap())
			if (std::find(used_.begin(), used_.end(), m.first) == used_.end())
				return Fail(error_, "unknown parameter " + m.first);
		effective = V::MapValue(out_);
		return true;
	}
private:
	const V* Use(const char* key) { used_.push_back(key); return ok_ ? map_.Find(key) : nullptr; }
	const V& map_;
	std::string& error_;
	bool ok_ = true;
	std::vector<std::string> used_;
	V::Members out_;
};

// Shared time stepping: output index i lives at time start + i*interval. A
// segment runs from the current time to the next output time (or to an
// imposed macro end) in N equal substeps; inputs are accepted once at the
// start of each segment. Stops land between substeps, and the segment state is
// part of the checkpoint, so a resumed run repeats the uninterrupted substeps.
class SteppedSolver : public ISimulationSolver {
public:
	bool Start(std::string& error) override {
		if (!configured_) return Fail(error, "configure first");
		t_ = start_; next_ = 0; finished_ = count_ == 0; segN_ = segJ_ = 0; segStart_ = segTarget_ = start_;
		Initialize();
		started_ = true;
		return true;
	}
	SolverPosition Position() const override { return {t_, next_, finished_}; }
	void RequestStop() override { stop_ = true; }

	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
		AdvanceResult& result, std::string& error) override {
		stop_ = false;
		result = AdvanceResult{};
		if (!started_) return Fail(error, "start or restore first");
		const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
		if (macro && (!std::isfinite(request.macroEnd) || request.macroEnd < t_)) return Fail(error, "macro end lies in the past");
		const double end = start_ + static_cast<double>(count_)*interval_;
		for (;;) {
			while (!finished_ && t_ == OutputTime(next_)) {
				std::uint64_t published = 0;
				if (!PublishOutputs(next_, t_, sink, ports, published, error)) return false;
				result.framesPublished += published;
				if (static_cast<std::uint64_t>(++next_) == count_) finished_ = true;
				if (pace_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(pace_));
				if (!macro) return Done(result, AdvanceResult::Status::Advanced);
			}
			if (finished_) return Done(result, AdvanceResult::Status::Finished);
			if (macro && t_ == request.macroEnd && segN_ == 0) return Done(result, AdvanceResult::Status::Advanced);
			if (segN_ == 0) {
				segStart_ = t_;
				segTarget_ = macro ? std::min(OutputTime(next_), request.macroEnd) : OutputTime(next_);
				const double nominal = interval_/static_cast<double>(substeps_);
				segN_ = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((segTarget_-segStart_)/nominal*(1-1e-12))));
				segJ_ = 0;
				if (!AcceptInputs(segStart_, ports, error)) return false;
			} else if (macro && segTarget_ > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
			while (segJ_ < segN_) {
				if (stop_.load() || (progress && !progress->Report(t_, end, std::string()))) {
					return Done(result, AdvanceResult::Status::Stopped);
				}
				const double tn = segJ_+1 == segN_ ? segTarget_ :
					segStart_ + (segTarget_-segStart_)*static_cast<double>(segJ_+1)/static_cast<double>(segN_);
				Substep(tn - t_);
				t_ = tn;
				++segJ_;
			}
			segN_ = segJ_ = 0;
		}
	}

	bool Checkpoint(Bytes& state, std::string& error) override {
		if (!started_) return Fail(error, "nothing to checkpoint");
		V::Members m{{"format", V::String(StateFormat())}, {"t", V::Float(t_)}, {"next", V::Signed(next_)},
			{"finished", V::Bool(finished_)}, {"segStart", V::Float(segStart_)}, {"segTarget", V::Float(segTarget_)},
			{"segN", V::Unsigned(segN_)}, {"segJ", V::Unsigned(segJ_)}};
		SaveFields(m);
		return RISECBOR64::Encode(V::MapValue(m), state, &error);
	}

	bool Restore(const Bytes& state, std::string& error) override {
		if (!configured_) return Fail(error, "configure before restore");
		V v;
		if (!CanDecodeCanonicalRecord(state, CodecBudget) || !RISECBOR64::DecodeCanonical(state, v, &error) ||
			v.GetType() != V::Map) return Fail(error, "invalid solver state");
		const V *format = v.Find("format"), *t = v.Find("t"), *next = v.Find("next"), *fin = v.Find("finished"),
			*s0 = v.Find("segStart"), *s1 = v.Find("segTarget"), *n = v.Find("segN"), *j = v.Find("segJ");
		if (!format || format->GetType() != V::Text || format->GetText() != StateFormat() || !t || t->GetType() != V::Float64 ||
			!next || next->GetType() != V::UnsignedInteger || !fin || fin->GetType() != V::Boolean ||
			!s0 || s0->GetType() != V::Float64 || !s1 || s1->GetType() != V::Float64 ||
			!n || n->GetType() != V::UnsignedInteger || !j || j->GetType() != V::UnsignedInteger ||
			next->GetIntegerArgument() > count_ || j->GetIntegerArgument() > n->GetIntegerArgument())
			return Fail(error, "invalid solver state fields");
		if (!LoadFields(v, error)) return false;
		t_ = t->GetFloat(); next_ = static_cast<std::int64_t>(next->GetIntegerArgument()); finished_ = fin->GetBoolean();
		segStart_ = s0->GetFloat(); segTarget_ = s1->GetFloat(); segN_ = n->GetIntegerArgument(); segJ_ = j->GetIntegerArgument();
		started_ = true;
		return true;
	}

protected:
	double OutputTime(std::int64_t i) const { return start_ + static_cast<double>(i)*interval_; }
	// Timing shared by both solvers; pace (wall delay per output) never enters the key.
	void ReadTiming(Params& p) {
		count_ = p.Count("frames", count_, 1, 100000);
		interval_ = p.Real("interval", interval_);
		substeps_ = p.Count("substeps", substeps_, 1, 100000);
		pace_ = static_cast<unsigned>(p.Count("pace_ms", 0, 0, 10000, false));
	}
	void Describe(SolverDescription& d, const std::string& id) const {
		d.solverId = id;
		d.version = "1";
		d.cache.firstIndex = 0;
		d.cache.count = count_;
		d.cache.startTime = start_;
		d.cache.interval = interval_;
		d.capabilities.backends = {{"cpu", BackendStatus::Supported, true, "reference implementation"}};
		d.capabilities.interactive = d.capabilities.deterministic = d.capabilities.resumable = true;
		d.capabilities.externalMacroStep = true;
		d.capabilities.fidelityModes = {"preview", "predictive"};
	}
	virtual const char* StateFormat() const = 0;
	virtual void Initialize() = 0;
	virtual void Substep(double dt) = 0;
	virtual bool PublishOutputs(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports,
		std::uint64_t& published, std::string& error) = 0;
	virtual bool AcceptInputs(double time, IPortExchange* ports, std::string& error) = 0;
	virtual void SaveFields(V::Members& m) const = 0;
	virtual bool LoadFields(const V& state, std::string& error) = 0;

	bool configured_ = false;
	std::uint64_t count_ = 16, substeps_ = 10;
	double start_ = 0, interval_ = 0.05;
	unsigned pace_ = 0;

private:
	bool Done(AdvanceResult& r, AdvanceResult::Status s) {
		r.status = finished_ ? AdvanceResult::Status::Finished : s;
		r.position = Position();
		return true;
	}
	bool started_ = false, finished_ = false;
	double t_ = 0, segStart_ = 0, segTarget_ = 0;
	std::int64_t next_ = 0;
	std::uint64_t segN_ = 0, segJ_ = 0;
	std::atomic<bool> stop_{false};
};

// ------------------------------------------------------------------ advection-diffusion

class AdvectionDiffusionSolver final : public SteppedSolver {
public:
	static constexpr const char* Id = "test.advection_diffusion";

	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override {
		count_ = 16; interval_ = 0.05; substeps_ = 10;
		Params p(parameters, error);
		n_ = static_cast<std::size_t>(p.Count("cells", 32, 4, 1024));
		length_ = p.Real("length", 1.0);
		vx_ = p.Real("velocity_x", 0.5); vy_ = p.Real("velocity_y", 0.25);
		diffusivity_ = p.Real("diffusivity", 0.01);
		kx_ = p.Integer("mode_x", 1); ky_ = p.Integer("mode_y", 1);
		amplitude_ = p.Real("amplitude", 1.0);
		ReadTiming(p);
		quantity_ = p.Text("quantity", "concentration");
		units_ = p.Text("units", "1");
		const std::string fidelity = p.Text("fidelity", "predictive");
		tolerance_ = p.Real("tolerance", 0.05);
		SolverDescription candidate;
		if (!p.Finish(candidate.effectiveParameters)) return false;
		if (length_ <= 0 || diffusivity_ < 0 || interval_ <= 0 || tolerance_ < 0 || (kx_ == 0 && ky_ == 0))
			return Fail(error, "invalid advection-diffusion parameters");
		const double h = length_/static_cast<double>(n_), dt = interval_/static_cast<double>(substeps_);
		if (diffusivity_*dt/(h*h) > 0.2 || (std::fabs(vx_)+std::fabs(vy_))*dt/h > 0.5)
			return Fail(error, "time step violates the explicit stability limit");
		configured_ = true;
		Describe(candidate, Id);
		candidate.cache.outputs = {{"field", "grid.v1", {{quantity_, units_, "f32"}}}};
		candidate.ports = {
			{"field", quantity_, units_, "grid.v1", true, "field"},
			{"source", quantity_ + " rate", units_ + "/s", "grid.v1", false, ""}};
		candidate.evidence.fidelity = fidelity;
		candidate.evidence.cases.push_back(FourierCase(candidate.effectiveParameters));
		d = std::move(candidate);
		return true;
	}

	// Exact solution of the continuous problem for the configured mode.
	double Exact(double x, double y, double t) const {
		const double kx = 2*Pi*static_cast<double>(kx_)/length_, ky = 2*Pi*static_cast<double>(ky_)/length_;
		return amplitude_*std::exp(-diffusivity_*(kx*kx+ky*ky)*t)*std::sin(kx*(x-vx_*t)+ky*(y-vy_*t));
	}
	double Value(std::size_t i, std::size_t j) const { return u_[i+n_*j]; }
	double Cell() const { return length_/static_cast<double>(n_); }

private:
	EvidenceCase FourierCase(const V& configuration) {
		// Run the configured discretization (no source) over the whole span and
		// compare with the exact solution at the last output time.
		Initialize();
		const std::uint64_t steps = (count_-1)*substeps_;
		const double dt = interval_/static_cast<double>(substeps_);
		for (std::uint64_t s = 0; s < steps; ++s) Substep(dt);
		const double t = static_cast<double>(steps)*dt, h = Cell();
		double worst = 0;
		for (std::size_t j = 0; j < n_; ++j) for (std::size_t i = 0; i < n_; ++i)
			worst = std::max(worst, std::fabs(Value(i, j) - Exact((static_cast<double>(i)+.5)*h, (static_cast<double>(j)+.5)*h, t)));
		EvidenceCase c;
		c.id = "fourier-mode";
		c.description = "single periodic Fourier mode advected and diffused; max error / amplitude at the last output";
		c.error = worst/std::fabs(amplitude_);
		c.tolerance = tolerance_;
		c.passed = c.error <= c.tolerance;
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		return c;
	}

	const char* StateFormat() const override { return "test.advection_diffusion.state.v1"; }
	void Initialize() override {
		const double h = Cell();
		u_.assign(n_*n_, 0);
		source_.clear();
		for (std::size_t j = 0; j < n_; ++j) for (std::size_t i = 0; i < n_; ++i)
			u_[i+n_*j] = Exact((static_cast<double>(i)+.5)*h, (static_cast<double>(j)+.5)*h, 0);
	}
	void Rate(const std::vector<double>& u, std::vector<double>& r) const {
		const double h = Cell(), cx = vx_/(2*h), cy = vy_/(2*h), d = diffusivity_/(h*h);
		r.resize(u.size());
		for (std::size_t j = 0; j < n_; ++j) for (std::size_t i = 0; i < n_; ++i) {
			const std::size_t ip = (i+1)%n_, im = (i+n_-1)%n_, jp = (j+1)%n_, jm = (j+n_-1)%n_;
			const double c = u[i+n_*j], e = u[ip+n_*j], w = u[im+n_*j], nn = u[i+n_*jp], s = u[i+n_*jm];
			r[i+n_*j] = -cx*(e-w) - cy*(nn-s) + d*(e+w+nn+s-4*c) + (source_.empty() ? 0 : source_[i+n_*j]);
		}
	}
	void Substep(double dt) override { // classical RK4
		std::vector<double> k1, k2, k3, k4, tmp(u_.size());
		Rate(u_, k1);
		for (std::size_t q = 0; q < u_.size(); ++q) tmp[q] = u_[q] + .5*dt*k1[q];
		Rate(tmp, k2);
		for (std::size_t q = 0; q < u_.size(); ++q) tmp[q] = u_[q] + .5*dt*k2[q];
		Rate(tmp, k3);
		for (std::size_t q = 0; q < u_.size(); ++q) tmp[q] = u_[q] + dt*k3[q];
		Rate(tmp, k4);
		for (std::size_t q = 0; q < u_.size(); ++q) u_[q] += dt/6*(k1[q]+2*k2[q]+2*k3[q]+k4[q]);
	}
	bool PublishOutputs(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports,
		std::uint64_t& published, std::string& error) override {
		const double h = Cell();
		GridFrame frame;
		GridChannel c;
		c.name = quantity_; c.units = units_;
		c.dimensions = {static_cast<std::uint32_t>(n_), static_cast<std::uint32_t>(n_), 1};
		c.indexToWorld = {h,0,0,h/2, 0,h,0,h/2, 0,0,h,0, 0,0,0,1};
		GridBlock b;
		b.dimensions = c.dimensions;
		b.active.assign((n_*n_+7)/8, 0xff);
		if ((n_*n_)%8) b.active.back() = static_cast<std::uint8_t>((1u << ((n_*n_)%8)) - 1);
		for (double x : u_) b.values.push_back(F32(x));
		c.blocks.push_back(std::move(b));
		frame.channels.push_back(std::move(c));
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeGrid(frame, CodecBudget, *bytes, error)) return false;
		std::shared_ptr<const Bytes> payload = std::move(bytes);
		if (!sink.Publish(index, "field", time, payload, {}, error)) return false;
		if (ports && !ports->Offer("field", time, payload, error)) return false;
		published = 1;
		return true;
	}
	bool AcceptInputs(double time, IPortExchange* ports, std::string& error) override {
		if (!ports) return true;
		std::shared_ptr<const Bytes> bytes;
		const auto status = ports->Accept("source", time, bytes, error);
		if (status == PortStatus::Unbound || status == PortStatus::Unavailable) { source_.clear(); return true; }
		if (status != PortStatus::Ready) return false;
		GridFrame g;
		if (!DecodeGrid(*bytes, CodecBudget, g, error)) return false;
		if (g.channels.size() != 1 || g.channels[0].components != 1 || g.channels[0].blocks.size() != 1 ||
			g.channels[0].dimensions[0] != n_ || g.channels[0].dimensions[1] != n_ || g.channels[0].blocks[0].constant)
			return Fail(error, "source must be one dense scalar block on the solver grid");
		source_.resize(n_*n_);
		for (std::size_t q = 0; q < source_.size(); ++q) source_[q] = FromF32(g.channels[0].blocks[0].values[q]);
		return true;
	}
	void SaveFields(V::Members& m) const override {
		m.emplace_back("u", V::BytesValue(PackDoubles(u_)));
		m.emplace_back("source", V::BytesValue(PackDoubles(source_)));
	}
	bool LoadFields(const V& s, std::string& error) override {
		std::vector<double> u, src;
		const V* sv = s.Find("source");
		if (!UnpackDoubles(s.Find("u"), n_*n_, u) || !sv || sv->GetType() != V::ByteString ||
			(!sv->GetBytes().empty() && !UnpackDoubles(sv, n_*n_, src))) return Fail(error, "state does not match the configured grid");
		u_.swap(u); source_.swap(src);
		return true;
	}

	std::size_t n_ = 32;
	double length_ = 1, vx_ = 0, vy_ = 0, diffusivity_ = 0, amplitude_ = 1, tolerance_ = 0;
	std::int64_t kx_ = 1, ky_ = 1;
	std::string quantity_, units_;
	std::vector<double> u_, source_;
};

// ------------------------------------------------------------------ membrane

class MembraneSolver final : public SteppedSolver {
public:
	static constexpr const char* Id = "test.membrane";

	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override {
		count_ = 24; interval_ = 0.01; substeps_ = 20;
		Params p(parameters, error);
		n_ = static_cast<std::size_t>(p.Count("vertices", 17, 3, 1025));
		size_ = p.Real("size", 1.0);
		tension_ = p.Real("tension", 100.0);
		density_ = p.Real("density", 1.0);
		amplitude_ = p.Real("amplitude", 0.01);
		ReadTiming(p);
		const std::string fidelity = p.Text("fidelity", "predictive");
		tolerance_ = p.Real("tolerance", 0.01);
		SolverDescription candidate;
		if (!p.Finish(candidate.effectiveParameters)) return false;
		if (size_ <= 0 || tension_ <= 0 || density_ <= 0 || interval_ <= 0 || tolerance_ < 0)
			return Fail(error, "invalid membrane parameters");
		const double h = size_/static_cast<double>(n_-1), dt = interval_/static_cast<double>(substeps_);
		if (WaveSpeed()*dt/h > 0.5) return Fail(error, "time step violates the CFL limit");
		configured_ = true;
		Describe(candidate, Id);
		candidate.cache.outputs = {{"surface", "mesh.v1", {{"normal","1","f32x3"},{"position","m","f32x3"},{"velocity","m/s","f32x3"}}}};
		candidate.ports = {
			{"surface", "surface", "m", "mesh.v1", true, "surface"},
			{"load", "pressure", "Pa", "grid.v1", false, ""}};
		candidate.evidence.fidelity = fidelity;
		candidate.evidence.cases.push_back(FrequencyCase(candidate.effectiveParameters));
		d = std::move(candidate);
		return true;
	}
	double WaveSpeed() const { return std::sqrt(tension_/density_); }
	double Fundamental() const { return WaveSpeed()/2*std::sqrt(2.0)/size_; }

private:
	EvidenceCase FrequencyCase(const V& configuration) {
		// Free vibration from the (1,1) mode shape: the first zero crossing of
		// the centre displacement is a quarter period.
		Initialize();
		const double dt = interval_/static_cast<double>(substeps_), f = Fundamental();
		const std::size_t centre = (n_/2) + n_*(n_/2);
		const std::uint64_t limit = static_cast<std::uint64_t>(std::ceil(0.5/f/dt)) + 2;
		double crossing = -1, previous = u_[centre];
		for (std::uint64_t s = 1; s <= limit && crossing < 0; ++s) {
			Substep(dt);
			const double now = u_[centre];
			if (previous > 0 && now <= 0) crossing = (static_cast<double>(s-1) + previous/(previous-now))*dt;
			previous = now;
		}
		EvidenceCase c;
		c.id = "fundamental-frequency";
		c.description = "free (1,1) mode: quarter period from the first centre zero crossing vs (c/2) sqrt(2)/a";
		c.error = crossing > 0 ? std::fabs(1/(4*crossing) - f)/f : 1.0;
		c.tolerance = tolerance_;
		c.passed = c.error <= c.tolerance;
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		return c;
	}

	const char* StateFormat() const override { return "test.membrane.state.v1"; }
	double Coordinate(std::size_t i) const { return size_*static_cast<double>(i)/static_cast<double>(n_-1); }
	void Initialize() override {
		u_.assign(n_*n_, 0); v_.assign(n_*n_, 0); load_.clear();
		for (std::size_t j = 1; j+1 < n_; ++j) for (std::size_t i = 1; i+1 < n_; ++i)
			u_[i+n_*j] = amplitude_*std::sin(Pi*Coordinate(i)/size_)*std::sin(Pi*Coordinate(j)/size_);
	}
	void Acceleration(std::vector<double>& a) const {
		const double h = size_/static_cast<double>(n_-1), c2 = tension_/density_/(h*h);
		a.assign(u_.size(), 0);
		for (std::size_t j = 1; j+1 < n_; ++j) for (std::size_t i = 1; i+1 < n_; ++i) {
			const std::size_t q = i+n_*j;
			a[q] = c2*(u_[q+1]+u_[q-1]+u_[q+n_]+u_[q-n_]-4*u_[q]) + (load_.empty() ? 0 : load_[q]/density_);
		}
	}
	void Substep(double dt) override { // velocity Verlet; boundary rows stay fixed
		std::vector<double> a;
		Acceleration(a);
		for (std::size_t q = 0; q < u_.size(); ++q) { v_[q] += .5*dt*a[q]; u_[q] += dt*v_[q]; }
		Acceleration(a);
		for (std::size_t q = 0; q < u_.size(); ++q) v_[q] += .5*dt*a[q];
	}
	bool PublishOutputs(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports,
		std::uint64_t& published, std::string& error) override {
		const double h = size_/static_cast<double>(n_-1);
		MeshFrame m;
		m.velocityUnits = "m/s";
		for (std::size_t j = 0; j < n_; ++j) for (std::size_t i = 0; i < n_; ++i) {
			const std::size_t q = i+n_*j;
			m.positions.insert(m.positions.end(), {F32(Coordinate(i)), F32(Coordinate(j)), F32(u_[q])});
			m.velocities.insert(m.velocities.end(), {F32(0), F32(0), F32(v_[q])});
			const double gx = (u_[std::min(i+1,n_-1)+n_*j] - u_[(i?i-1:0)+n_*j])/(h*static_cast<double>(std::min(i+1,n_-1)-(i?i-1:0)));
			const double gy = (u_[i+n_*std::min(j+1,n_-1)] - u_[i+n_*(j?j-1:0)])/(h*static_cast<double>(std::min(j+1,n_-1)-(j?j-1:0)));
			const double len = std::sqrt(gx*gx+gy*gy+1);
			m.normals.insert(m.normals.end(), {F32(-gx/len), F32(-gy/len), F32(1/len)});
		}
		for (std::size_t j = 0; j+1 < n_; ++j) for (std::size_t i = 0; i+1 < n_; ++i) {
			const auto a = static_cast<std::uint32_t>(i+n_*j), b = a+1, c = static_cast<std::uint32_t>(a+n_), e = c+1;
			m.indices.insert(m.indices.end(), {a, b, e, a, e, c});
		}
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeMesh(m, CodecBudget, *bytes, error)) return false;
		std::shared_ptr<const Bytes> payload = std::move(bytes);
		if (!sink.Publish(index, "surface", time, payload, {}, error)) return false;
		if (ports && !ports->Offer("surface", time, payload, error)) return false;
		published = 1;
		return true;
	}
	// Pressure on a grid (any axis-aligned transform), sampled at the nearest
	// cell centre of each vertex with periodic wrap.
	bool AcceptInputs(double time, IPortExchange* ports, std::string& error) override {
		if (!ports) return true;
		std::shared_ptr<const Bytes> bytes;
		const auto status = ports->Accept("load", time, bytes, error);
		if (status == PortStatus::Unbound || status == PortStatus::Unavailable) { load_.clear(); return true; }
		if (status != PortStatus::Ready) return false;
		GridFrame g;
		if (!DecodeGrid(*bytes, CodecBudget, g, error)) return false;
		if (g.channels.size() != 1 || g.channels[0].components != 1) return Fail(error, "load must be one scalar channel");
		const auto& c = g.channels[0];
		const auto& m = c.indexToWorld;
		if (m[1] != 0 || m[2] != 0 || m[4] != 0 || m[6] != 0 || m[8] != 0 || m[9] != 0 || m[0] <= 0 || m[5] <= 0)
			return Fail(error, "load grid must be axis aligned");
		load_.assign(n_*n_, 0);
		for (std::size_t j = 0; j < n_; ++j) for (std::size_t i = 0; i < n_; ++i) {
			auto wrap = [](double x, std::uint32_t n) {
				const auto k = static_cast<std::int64_t>(std::floor(x + .5)) % static_cast<std::int64_t>(n);
				return static_cast<std::int64_t>(k < 0 ? k + n : k);
			};
			const std::int64_t gi = c.origin[0] + wrap((Coordinate(i)-m[3])/m[0] - c.origin[0], c.dimensions[0]);
			const std::int64_t gj = c.origin[1] + wrap((Coordinate(j)-m[7])/m[5] - c.origin[1], c.dimensions[1]);
			std::uint32_t bits = c.background[0];
			for (const auto& b : c.blocks) {
				const std::int64_t li = gi - b.origin[0], lj = gj - b.origin[1], lk = c.origin[2] - b.origin[2];
				if (li < 0 || lj < 0 || lk < 0 || li >= b.dimensions[0] || lj >= b.dimensions[1] || lk >= b.dimensions[2]) continue;
				bits = b.constant ? b.values[0] :
					b.values[static_cast<std::size_t>(li + b.dimensions[0]*(lj + std::int64_t(b.dimensions[1])*lk))];
			}
			load_[i+n_*j] = FromF32(bits);
		}
		return true;
	}
	void SaveFields(V::Members& m) const override {
		m.emplace_back("u", V::BytesValue(PackDoubles(u_)));
		m.emplace_back("v", V::BytesValue(PackDoubles(v_)));
		m.emplace_back("load", V::BytesValue(PackDoubles(load_)));
	}
	bool LoadFields(const V& s, std::string& error) override {
		std::vector<double> u, v, load;
		const V* lv = s.Find("load");
		if (!UnpackDoubles(s.Find("u"), n_*n_, u) || !UnpackDoubles(s.Find("v"), n_*n_, v) || !lv ||
			lv->GetType() != V::ByteString || (!lv->GetBytes().empty() && !UnpackDoubles(lv, n_*n_, load)))
			return Fail(error, "state does not match the configured membrane");
		u_.swap(u); v_.swap(v); load_.swap(load);
		return true;
	}

	std::size_t n_ = 17;
	double size_ = 1, tension_ = 100, density_ = 1, amplitude_ = 0.01, tolerance_ = 0;
	std::vector<double> u_, v_, load_;
};

inline void RegisterReferenceSolvers()
{
	std::string ignored;
	RegisterSolver(AdvectionDiffusionSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new AdvectionDiffusionSolver); }, ignored);
	RegisterSolver(MembraneSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new MembraneSolver); }, ignored);
}

} } }
#endif
