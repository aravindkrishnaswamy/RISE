// Simulation M6: see SimulationGroup.h.
#include "pch.h"
#include "SimulationGroup.h"
#include "SimulationField.h"
#include "SimulationGrid.h"
#include "SimulationMesh.h"
#include "SimulationPortAdapters.h"
#include "SimulationTransform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>

namespace RISE { namespace Simulation {

const char* const kCouplingInput = "rise.coupling";
const char* const kGroupManifestName = "rise-sim.group.cbor";

namespace {
using V = RISECBOR64::Value;
namespace fs = std::filesystem;
constexpr std::size_t CodecBudget = std::size_t(1) << 30;
constexpr const char* StateFormat = "rise.simulation.group.state.v1";

bool Fail(std::string& error, const std::string& message) { error = message; return false; }

double FromF32(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
std::uint32_t F32(double x) { const float f = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

std::string Hash(const V& value)
{
	Bytes encoded;
	if (!RISECBOR64::Encode(value, encoded, nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

// ------------------------------------------------------------------ coupling evidence registry

struct EvidenceEntry {
	std::string kind;
	std::function<bool(Evidence&, std::string&)> compute;
	bool done = false, ok = false;
	Evidence evidence;
	std::string error;
};
struct EvidenceRegistry {
	std::mutex mutex;
	std::vector<std::shared_ptr<EvidenceEntry>> entries;
};
EvidenceRegistry& Registry()
{
	static EvidenceRegistry r;
	return r;
}

// ------------------------------------------------------------------ payload vectors (residuals, relaxation)

// The numbers a payload carries, in a fixed order (decode with the resolver).
// The numbers a payload carries, in a fixed order (decode with the resolver),
// and per number its group: one per field or grid channel, per transform
// quantity (position, orientation, velocity, angular velocity), per mesh
// array (positions, velocities).  Relaxation factors are per group.
bool PayloadVector(const std::string& codec, const Bytes& bytes, const StaticPayloadResolver& resolve,
	std::vector<double>& v, std::vector<int>& group, std::string& error)
{
	v.clear();
	group.clear();
	auto put = [&](double x, int g) { v.push_back(x); group.push_back(g); };
	if (codec == "field.v1") {
		FieldFrame f;
		if (!DecodeField(bytes, CodecBudget, f, error)) return false;
		for (std::size_t c = 0; c < f.channels.size(); ++c) for (auto w : f.channels[c].values) put(FromF32(w), static_cast<int>(c));
	} else if (codec == TransformCodec) {
		TransformFrame f;
		if (!DecodeTransforms(bytes, CodecBudget, f, error)) return false;
		for (const auto& b : f.bodies) {
			for (double x : b.position) put(x, 0);
			for (double x : b.orientation) put(x, 1);
			for (double x : b.velocity) put(x, 2);
			for (double x : b.angularVelocity) put(x, 3);
		}
	} else if (codec == "mesh.v1" || codec == "mesh.v2") {
		MeshFrame m;
		if (!DecodeMesh(bytes, CodecBudget, resolve, m, error)) return false;
		for (auto w : m.positions) put(FromF32(w), 0);
		for (auto w : m.velocities) put(FromF32(w), 1);
	} else if (codec == "grid.v1") {
		GridFrame g;
		if (!DecodeGrid(bytes, CodecBudget, g, error)) return false;
		for (std::size_t c = 0; c < g.channels.size(); ++c) {
			for (auto w : g.channels[c].background) put(FromF32(w), static_cast<int>(c));
			for (const auto& b : g.channels[c].blocks) for (auto w : b.values) put(FromF32(w), static_cast<int>(c));
		}
	} else return Fail(error, "no numeric view of codec " + codec);
	return true;
}

// The payload with its numbers replaced (same structure); false when the
// codec or the payload (a mesh referencing a static topology) cannot be rebuilt.
bool ReplaceVector(const std::string& codec, const Bytes& bytes, const std::vector<double>& v,
	std::shared_ptr<const Bytes>& out, std::string& error)
{
	auto result = std::make_shared<Bytes>();
	std::size_t k = 0;
	auto next = [&]() { return k < v.size() ? v[k++] : 0.0; };
	if (codec == "field.v1") {
		FieldFrame f;
		if (!DecodeField(bytes, CodecBudget, f, error)) return false;
		for (auto& c : f.channels) for (auto& w : c.values) w = F32(next());
		if (!EncodeField(f, CodecBudget, *result, error)) return false;
	} else if (codec == TransformCodec) {
		TransformFrame f;
		if (!DecodeTransforms(bytes, CodecBudget, f, error)) return false;
		for (auto& b : f.bodies) {
			for (double& x : b.position) x = next();
			for (double& x : b.orientation) x = next();
			for (double& x : b.velocity) x = next();
			for (double& x : b.angularVelocity) x = next();
			if (!CanonicalQuaternion(b.orientation)) return Fail(error, "relaxed orientation degenerate");
		}
		if (!EncodeTransforms(f, CodecBudget, *result, error)) return false;
	} else if (codec == "mesh.v1" || codec == "mesh.v2") {
		MeshTopology top;
		if (!PeekMeshTopology(bytes, top, error) || !top.reference.empty()) return Fail(error, "only self-contained meshes are relaxed");
		MeshFrame m;
		if (!DecodeMesh(bytes, CodecBudget, m, error)) return false;
		for (auto& w : m.positions) w = F32(next());
		for (auto& w : m.velocities) w = F32(next());
		if (codec == "mesh.v1") { if (!EncodeMesh(m, CodecBudget, *result, error)) return false; }
		else {
			Bytes fixed;
			std::string digest;
			if (!EncodeMeshV2(m, MeshStaticLayout(), CodecBudget, *result, fixed, digest, error)) return false;
		}
	} else if (codec == "grid.v1") {
		GridFrame g;
		if (!DecodeGrid(bytes, CodecBudget, g, error)) return false;
		for (auto& c : g.channels) {
			for (auto& w : c.background) w = F32(next());
			for (auto& b : c.blocks) for (auto& w : b.values) w = F32(next());
		}
		if (!EncodeGrid(g, CodecBudget, *result, error)) return false;
	} else return Fail(error, "no numeric view of codec " + codec);
	out = std::move(result);
	return true;
}

double Norm(const std::vector<double>& v)
{
	double s = 0;
	for (double x : v) s += x*x;
	return std::sqrt(s);
}

// Scene inputs with the coupling identity added (the key input of a coupled member).
class CouplingInputs final : public ISceneInputs {
public:
	CouplingInputs(const ISceneInputs& base, std::string identity) : base_(base), identity_(std::move(identity)) {}
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name == kCouplingInput) { hash = identity_; return true; }
		return base_.Identity(name, hash, error);
	}
	bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded, std::string& codec,
		std::string& error) const override { return base_.Sample(name, time, encoded, codec, error); }
	bool SampleTransform(const std::string& name, double time, RigidTransform& t, std::string& error) const override {
		return base_.SampleTransform(name, time, t, error);
	}
	bool ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) const override { return base_.ResolveStatic(digest, budget, payload, error); }
private:
	const ISceneInputs& base_;
	std::string identity_;
};

const char* OrderName(ExchangeOrder o) { return o == ExchangeOrder::Jacobi ? "jacobi" : "gauss-seidel"; }

V PolicyValue(const GroupPolicy& p, double macro, const std::vector<std::string>& order)
{
	V::Values names;
	for (const auto& n : order) names.push_back(V::String(n));
	V::Members m{{"absolute_tolerance", V::Float(p.absoluteTolerance)}, {"aitken", V::Bool(p.aitken)},
		{"macro_step", V::Float(macro)}, {"member_order", V::ArrayValue(names)},
		{"minimum_macro_step", V::Float(p.minimumMacroStep)},
		{"non_convergence", V::String(p.onNonConvergence == GroupPolicy::NonConvergence::Halve ? "halve" : "fail")},
		{"order", V::String(OrderName(p.order))}, {"relaxation", V::Float(p.relaxation)},
		{"sub_iterations", V::Unsigned(p.subIterations)}, {"tolerance", V::Float(p.convergenceTolerance)}};
	// M7 prerequisites: present only when used, so every earlier group keeps its identity.
	if (!p.ownedExchanges.empty()) {
		V::Values owned;
		for (const auto& e : p.ownedExchanges) {
			std::vector<std::string> partners = e.partners;
			std::sort(partners.begin(), partners.end());
			V::Values ps;
			for (const auto& q : partners) ps.push_back(V::String(q));
			owned.push_back(V::ArrayValue({V::String(e.owner), V::ArrayValue(ps)}));
		}
		m.push_back({"owned_exchanges", V::ArrayValue(owned)});
	}
	return V::MapValue(m);
}
} // namespace

bool RegisterCouplingEvidence(const std::string& kind, std::function<bool(Evidence&, std::string&)> compute,
	std::string& error)
{
	if (kind.empty() || !compute) return Fail(error, "invalid coupling evidence registration");
	auto& r = Registry();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& e : r.entries) if (e->kind == kind) return Fail(error, "coupling evidence already registered: " + kind);
	auto e = std::make_shared<EvidenceEntry>();
	e->kind = kind;
	e->compute = std::move(compute);
	r.entries.push_back(e);
	return true;
}

bool CouplingEvidence(const std::string& kind, Evidence& evidence, std::string& error)
{
	std::shared_ptr<EvidenceEntry> entry;
	{
		auto& r = Registry();
		std::lock_guard<std::mutex> hold(r.mutex);
		for (const auto& e : r.entries) if (e->kind == kind) entry = e;
	}
	if (!entry) return Fail(error, "no coupling evidence is registered for " + kind);
	// The computation may configure a group of the same kind itself: a nested
	// request while it runs finds no evidence (that inner group is preview).
	static std::recursive_mutex computing;
	static bool busy = false;
	std::lock_guard<std::recursive_mutex> hold(computing);
	if (busy && !entry->done) return Fail(error, "the coupling evidence for " + kind + " is being computed");
	if (!entry->done) {
		busy = true;
		entry->ok = entry->compute(entry->evidence, entry->error);
		busy = false;
		entry->done = true;
	}
	if (!entry->ok) return Fail(error, "coupling evidence for " + kind + " failed: " + entry->error);
	evidence = entry->evidence;
	return true;
}

// ------------------------------------------------------------------ members and links

struct SimulationGroup::Member {
	std::string name;
	bool cached = false, foreign = false;
	std::unique_ptr<ISimulationSolver> solver;
	V parameters;
	const ISceneInputs* inputs = nullptr;
	std::unique_ptr<CouplingInputs> keyInputs;
	SolverDescription description;
	std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot;
	std::unique_ptr<CachedPortFeed> feed;
	double timeOffset = 0;   // a cached member is read at cache time = group time + timeOffset
	MemoryPublisher publisher;
	std::unique_ptr<CacheWriter> recorder;
	fs::path recordRoot;
	std::map<std::string, std::vector<std::pair<double, std::shared_ptr<const Bytes>>>> offers;
	std::map<std::string, std::shared_ptr<const Bytes>> statics;   // by digest
	std::unique_ptr<Exchange> exchange;
	std::shared_ptr<const SolverStateToken> saved;
	struct HeldFrame { std::int64_t index; std::string output; double time; std::shared_ptr<const Bytes> payload; Bytes receipt; };
	std::vector<HeldFrame> heldFrames;
	std::vector<std::pair<std::string, std::shared_ptr<const Bytes>>> heldStatics;
	std::string couplingLimit, baseIdentity;
	bool coupled = false;
	bool Finished() const { return cached || !solver || solver->Position().finished; }
};

struct SimulationGroup::Link {
	PortConnection c;
	std::size_t from = 0, to = 0;
	PortDeclaration offer, accept;
	std::shared_ptr<const IPortAdapter> adapter;
	std::vector<std::pair<std::size_t, std::string>> aux;
	bool fresh = false;                      // the source has always advanced first (or is a cache)
	std::shared_ptr<const Bytes> served, relaxed, accepted;
	bool owned = false;                      // M7 prerequisites: an owned implicit exchange
	std::size_t owner = 0;                   // its owner (a member index), when owned
	// Conversion memo: the last raw input pointers and their result.
	std::shared_ptr<const Bytes> memoRaw, memoOut;
	std::vector<std::shared_ptr<const Bytes>> memoAux;
	std::string identity;
};

class SimulationGroup::Exchange final : public IPortExchange {
public:
	Exchange(SimulationGroup& g, std::size_t member) : g_(g), m_(member) {}
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes> encoded, std::string& error) override {
		if (!encoded || !std::isfinite(time)) return Fail(error, "invalid port offer");
		auto& list = g_.members_[m_]->offers[port];
		auto it = std::lower_bound(list.begin(), list.end(), time, [](const auto& e, double t) { return e.first < t; });
		if (it != list.end() && it->first == time) it->second = std::move(encoded);
		else list.insert(it, {time, std::move(encoded)});
		return true;
	}
	bool OfferStatic(const std::string&, std::shared_ptr<const Bytes> payload, std::string& error) override {
		if (!payload) return Fail(error, "invalid static payload offer");
		auto& statics = g_.members_[m_]->statics;
		for (const auto& s : statics) if (s.second == payload) return true;
		statics[RISECBOR64::SHA256Hex(*payload)] = std::move(payload);
		return true;
	}
	PortStatus Accept(const std::string& port, double, std::shared_ptr<const Bytes>& encoded, std::string& error) override {
		for (auto& l : g_.links_) {
			if (l->to != m_ || l->c.toPort != port) continue;
			std::shared_ptr<const Bytes> value;
			if (!g_.Serve(*l, value, error)) return PortStatus::Failed;
			if (!value) { error = "nothing offered yet on " + l->c.fromMember + "." + l->c.fromPort; return PortStatus::Unavailable; }
			l->served = value;
			encoded = std::move(value);
			return PortStatus::Ready;
		}
		error = "port not connected: " + port;
		return PortStatus::Unbound;
	}
	PortStatus ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
		std::string& error) override { return g_.ResolveStatic(digest, budget, payload, error); }
private:
	SimulationGroup& g_;
	std::size_t m_;
};

// Frames to the member's memory publisher and recording, or held back while sub-iterating.
class SimulationGroup::Sink final : public IFrameSink {
public:
	Sink(Member& m, bool held) : m_(m), held_(held) {}
	bool Publish(std::int64_t index, const std::string& output, double time, std::shared_ptr<const Bytes> payload,
		const Bytes& receipt, std::string& error) override {
		if (!payload) return Fail(error, "null frame payload");
		if (held_) { m_.heldFrames.push_back({index, output, time, std::move(payload), receipt}); return true; }
		return Commit(m_, index, output, time, std::move(payload), receipt, error);
	}
	bool PublishStatic(const std::string& type, std::shared_ptr<const Bytes> payload, std::string& digest,
		std::string& error) override {
		if (!payload) return Fail(error, "null static payload");
		if (held_) { digest = RISECBOR64::SHA256Hex(*payload); m_.heldStatics.emplace_back(type, std::move(payload)); return true; }
		return CommitStatic(m_, type, std::move(payload), digest, error);
	}
	static bool Commit(Member& m, std::int64_t index, const std::string& output, double time, std::shared_ptr<const Bytes> payload,
		const Bytes& receipt, std::string& error) {
		if (m.recorder && !m.recorder->Append(index, output, time, *payload, error, receipt)) return false;
		return m.publisher.Publish(index, output, time, std::move(payload), receipt, error);
	}
	static bool CommitStatic(Member& m, const std::string& type, std::shared_ptr<const Bytes> payload, std::string& digest,
		std::string& error) {
		if (m.recorder && !m.recorder->AppendStatic(type, *payload, digest, error)) return false;
		return m.publisher.PublishStatic(type, std::move(payload), digest, error);
	}
	static bool Flush(Member& m, std::string& error) {
		for (auto& s : m.heldStatics) {
			std::string digest;
			if (!CommitStatic(m, s.first, std::move(s.second), digest, error)) return false;
		}
		for (auto& f : m.heldFrames)
			if (!Commit(m, f.index, f.output, f.time, std::move(f.payload), f.receipt, error)) return false;
		m.heldStatics.clear();
		m.heldFrames.clear();
		return true;
	}
private:
	Member& m_;
	bool held_;
};

SimulationGroup::SimulationGroup() = default;
SimulationGroup::~SimulationGroup() = default;

bool SimulationGroup::AddMember(const std::string& name, std::unique_ptr<ISimulationSolver> solver, const V& parameters,
	std::string& error)
{
	if (configured_) return Fail(error, "add members before Configure");
	if (name.empty() || name.find('.') != std::string::npos || !solver) return Fail(error, "a member needs a name without '.' and a solver");
	for (const auto& m : members_) if (m->name == name) return Fail(error, "member named twice: " + name);
	auto m = std::make_unique<Member>();
	m->name = name;
	m->solver = std::move(solver);
	m->parameters = parameters;
	members_.push_back(std::move(m));
	return true;
}

bool SimulationGroup::AddCachedMember(const std::string& name, std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot,
	std::string& error)
{
	return AddCachedMember(name, std::move(snapshot), false, error);
}

bool SimulationGroup::AddCachedMember(const std::string& name, std::shared_ptr<const IReadOnlyCacheSnapshot> snapshot,
	bool foreign, std::string& error)
{
	if (configured_) return Fail(error, "add members before Configure");
	if (name.empty() || name.find('.') != std::string::npos || !snapshot) return Fail(error, "a cached member needs a name and a snapshot");
	for (const auto& m : members_) if (m->name == name) return Fail(error, "member named twice: " + name);
	auto m = std::make_unique<Member>();
	m->name = name;
	m->cached = true;
	m->foreign = foreign;
	m->snapshot = snapshot;
	m->feed = std::make_unique<CachedPortFeed>(snapshot, CodecBudget);
	for (const auto& o : snapshot->Description().outputs) m->feed->Bind(o.name, o.name);
	members_.push_back(std::move(m));
	return true;
}

bool SimulationGroup::SetCachedTimeOffset(const std::string& name, double offset, std::string& error)
{
	if (configured_) return Fail(error, "set a cached member's time offset before Configure");
	if (!std::isfinite(offset)) return Fail(error, "invalid time offset");
	for (auto& m : members_) if (m->name == name && m->cached) { m->timeOffset = offset; return true; }
	return Fail(error, "no cached member named " + name);
}

bool SimulationGroup::SetMemberInputs(const std::string& name, const ISceneInputs& inputs, std::string& error)
{
	if (configured_) return Fail(error, "set member inputs before Configure");
	for (auto& m : members_) if (m->name == name && !m->cached) { m->inputs = &inputs; return true; }
	return Fail(error, "no live member named " + name);
}

bool SimulationGroup::Connect(const PortConnection& connection, std::string& error)
{
	if (configured_) return Fail(error, "connect before Configure");
	auto link = std::make_unique<Link>();
	link->c = connection;
	links_.push_back(std::move(link));
	return true;
}

bool SimulationGroup::Configure(const GroupPolicy& policy, const ISceneInputs& inputs, std::string& error)
{
	if (configured_) return Fail(error, "the group is already configured");
	if (members_.empty()) return Fail(error, "a group needs members");
	policy_ = policy;
	inputs_ = &inputs;
	const bool implicit = policy.subIterations > 1;
	if (policy.subIterations == 0) return Fail(error, "subIterations must be at least 1");
	if (!(policy.relaxation > 0 && policy.relaxation <= 1)) return Fail(error, "relaxation must be in (0, 1]");
	if (!(policy.convergenceTolerance >= 0) || !(policy.absoluteTolerance >= 0)) return Fail(error, "tolerances must not be negative");
	auto index = [&](const std::string& name) -> std::size_t {
		for (std::size_t i = 0; i < members_.size(); ++i) if (members_[i]->name == name) return i;
		return members_.size();
	};
	// Members.
	double start = 0, smallest = 0;
	bool first = true;
	for (auto& m : members_) {
		if (m->cached) continue;
		if (!m->inputs) m->inputs = &inputs;
		if (!m->solver->Configure(m->parameters, *m->inputs, m->description, error))
			return Fail(error, "member " + m->name + ": " + error);
		const auto& caps = m->description.capabilities;
		if (!caps.externalMacroStep) return Fail(error, "member " + m->name + " cannot honour an imposed macro step");
		if (implicit && !caps.rollback) return Fail(error, "member " + m->name + " does not support rollback (implicit coupling needs it)");
		const double s = m->description.cache.startTime, h = m->description.cache.interval;
		if (first) { start = s; smallest = h; first = false; }
		else if (s != start) return Fail(error, "live members must start at the same time");
		smallest = std::min(smallest, h);
		end_ = std::max(end_, s + static_cast<double>(m->description.cache.count ? m->description.cache.count - 1 : 0)*h);
	}
	if (first) return Fail(error, "a group needs at least one live member");
	macroStep_ = policy.macroStep > 0 ? policy.macroStep : smallest;
	if (!(macroStep_ > 0) || !std::isfinite(macroStep_)) return Fail(error, "invalid macro step");
	time_ = start;
	// Order.
	std::vector<std::string> orderNames;
	if (!policy.memberOrder.empty()) {
		for (const auto& n : policy.memberOrder) {
			const std::size_t i = index(n);
			if (i == members_.size() || members_[i]->cached) return Fail(error, "member order names no live member " + n);
			if (std::find(order_.begin(), order_.end(), i) != order_.end()) return Fail(error, "member order lists " + n + " twice");
			order_.push_back(i);
		}
		for (std::size_t i = 0; i < members_.size(); ++i)
			if (!members_[i]->cached && std::find(order_.begin(), order_.end(), i) == order_.end())
				return Fail(error, "member order leaves out " + members_[i]->name);
	} else for (std::size_t i = 0; i < members_.size(); ++i) if (!members_[i]->cached) order_.push_back(i);
	for (std::size_t i : order_) orderNames.push_back(members_[i]->name);
	auto position = [&](std::size_t member) {
		return static_cast<std::size_t>(std::find(order_.begin(), order_.end(), member) - order_.begin());
	};
	// Links.
	std::set<std::pair<std::size_t,std::string>> targets;
	for (auto& l : links_) {
		const auto& c = l->c;
		l->from = index(c.fromMember);
		l->to = index(c.toMember);
		if (l->from == members_.size() || l->to == members_.size()) return Fail(error, "connection names an unknown member: " + c.fromMember + " -> " + c.toMember);
		if (l->from == l->to) return Fail(error, "a member cannot feed itself: " + c.fromMember);
		Member& from = *members_[l->from];
		Member& to = *members_[l->to];
		if (to.cached) return Fail(error, "a cached member accepts nothing: " + to.name);
		if (!targets.insert({l->to, c.toPort}).second) return Fail(error, "two connections feed " + to.name + "." + c.toPort);
		const PortDeclaration* accept = nullptr;
		for (const auto& p : to.description.ports) if (p.name == c.toPort && !p.offers) accept = &p;
		if (!accept) return Fail(error, to.name + " accepts no port " + c.toPort);
		l->accept = *accept;
		const CacheDescription* offering = nullptr;
		if (from.cached) {
			const auto& outs = from.snapshot->Description().outputs;
			const auto o = std::find_if(outs.begin(), outs.end(), [&](const CacheOutput& x) { return x.name == c.fromPort; });
			if (o == outs.end()) return Fail(error, "cached member " + from.name + " has no output " + c.fromPort);
			l->offer.name = o->name; l->offer.codec = o->type; l->offer.offers = true; l->offer.output = o->name;
			l->offer.quantity = o->name; l->offer.units = "-";
			for (const auto& ch : o->channels) l->offer.channels.push_back({ch.name, ch.name, ch.units, 1});
			offering = &from.snapshot->Description();
		} else {
			const PortDeclaration* offer = nullptr;
			for (const auto& p : from.description.ports) if (p.name == c.fromPort && p.offers) offer = &p;
			if (!offer) return Fail(error, from.name + " offers no port " + c.fromPort);
			l->offer = *offer;
			offering = &from.description.cache;
		}
		PortAdapterContext context;
		context.offer = &l->offer; context.accept = &l->accept; context.offering = offering; context.accepting = &to.description.cache;
		std::string why;
		if (!c.adapter.empty()) {
			l->adapter = CreatePortAdapter(c.adapter, c.adapterParameters, context, why);
			if (!l->adapter) return Fail(error, c.fromMember + "." + c.fromPort + " -> " + c.toMember + "." + c.toPort + ": " + why);
		} else if (from.cached ? l->offer.codec != l->accept.codec : !PortsCompatible(l->offer, l->accept, why)) {
			l->adapter = FindPortAdapter(l->offer, l->accept);
			if (!l->adapter)
				return Fail(error, "ports " + c.fromMember + "." + c.fromPort + " -> " + c.toMember + "." + c.toPort +
					" are incompatible and no adapter converts them" + (why.empty() ? std::string() : ": " + why));
		}
		for (const auto& a : c.auxiliary) {
			const std::size_t m = index(a.first);
			if (m == members_.size()) return Fail(error, "auxiliary input names an unknown member " + a.first);
			l->aux.emplace_back(m, a.second);
		}
		l->fresh = from.cached || (policy.order == ExchangeOrder::GaussSeidel && position(l->from) < position(l->to));
		std::string adapterId = l->adapter ? l->adapter->Identity() : std::string("direct");
		if (adapterId.empty()) adapterId = "adapter:" + c.adapter;
		V::Values aux;
		for (const auto& a : c.auxiliary) aux.push_back(V::ArrayValue({V::String(a.first), V::String(a.second)}));
		l->identity = Hash(V::ArrayValue({V::String(c.fromMember), V::String(c.fromPort), V::String(c.toMember),
			V::String(c.toPort), V::String(adapterId), V::ArrayValue(aux)}));
	}
	// M7 prerequisites: owned implicit exchanges.
	if (!policy.ownedExchanges.empty()) {
		if (policy.order != ExchangeOrder::GaussSeidel) return Fail(error, "an owned exchange needs Gauss-Seidel order");
		std::set<std::size_t> covered;
		for (const auto& e : policy.ownedExchanges) {
			const std::size_t o = index(e.owner);
			if (o == members_.size() || members_[o]->cached) return Fail(error, "owned exchange: no live member " + e.owner);
			if (e.partners.empty()) return Fail(error, "owned exchange of " + e.owner + " names no partner");
			covered.insert(o);
			for (const auto& name : e.partners) {
				const std::size_t q = index(name);
				if (q == members_.size() || members_[q]->cached || q == o)
					return Fail(error, "owned exchange of " + e.owner + ": invalid partner " + name);
				if (position(o) > position(q))
					return Fail(error, "owned exchange: the owner " + e.owner + " must step before its partner " + name);
				covered.insert(q);
				for (auto& l : links_) {
					if (!((l->from == o && l->to == q) || (l->from == q && l->to == o))) continue;
					if (l->owned && l->owner != o) return Fail(error, "a connection is owned by two members");
					l->owned = true;
					l->owner = o;
				}
			}
		}
		if (implicit)
			for (std::size_t i : order_)
				if (!covered.count(i))
					return Fail(error, "member " + members_[i]->name + " would sub-iterate beside an owned exchange "
						"(not supported: every live member must belong to an owned exchange when subIterations > 1)");
	}
	// Base identities.
	for (auto& m : members_) {
		if (m->cached) {
			const auto& d = m->snapshot->Description();
			V::Values id{V::String(m->name), V::String("cache"), V::String(d.key),
				V::String(m->foreign ? m->snapshot->Digest() : std::string())};
			if (m->timeOffset != 0) id.push_back(V::Float(m->timeOffset));   // absent at 0: the identity of earlier groups
			m->baseIdentity = Hash(V::ArrayValue(id));
			continue;
		}
		V::Values ids;
		for (const auto& name : m->description.inputs) {
			if (name == kCouplingInput) return Fail(error, "member " + m->name + " already reads an input named " + kCouplingInput);
			std::string hash;
			if (!m->inputs->Identity(name, hash, error)) return Fail(error, "member " + m->name + ": " + error);
			ids.push_back(V::ArrayValue({V::String(name), V::String(hash)}));
		}
		m->baseIdentity = Hash(V::ArrayValue({V::String(m->name), V::String("live"), V::String(m->description.solverId),
			V::String(m->description.version), m->description.effectiveParameters, V::ArrayValue(ids)}));
	}
	const V policyValue = PolicyValue(policy, macroStep_, orderNames);
	{
		V::Values ms, ls;
		for (const auto& m : members_) ms.push_back(V::String(m->baseIdentity));
		for (const auto& l : links_) ls.push_back(V::String(l->identity));
		identity_ = Hash(V::ArrayValue({V::String("rise.simulation.group.v1"), policyValue, V::ArrayValue(ms), V::ArrayValue(ls)}));
	}
	// Per live member: the upstream closure, its coupling identity and label.
	for (std::size_t mi = 0; mi < members_.size(); ++mi) {
		Member& m = *members_[mi];
		if (m.cached) continue;
		std::set<std::size_t> closure{mi};
		for (bool grew = true; grew;) {
			grew = false;
			for (const auto& l : links_) {
				if (!closure.count(l->to)) continue;
				if (closure.insert(l->from).second) grew = true;
				for (const auto& a : l->aux) if (closure.insert(a.first).second) grew = true;
			}
		}
		std::vector<const Link*> into;
		for (const auto& l : links_) if (closure.count(l->to)) into.push_back(l.get());
		if (into.empty()) {
			if (!PrepareCacheDescription(m.description, *m.inputs, "", error)) return Fail(error, "member " + m.name + ": " + error);
			m.publisher.Reset(m.description.cache);
			continue;
		}
		m.coupled = true;
		std::vector<std::string> bases;
		for (std::size_t k : closure) bases.push_back(members_[k]->name + ":" + members_[k]->baseIdentity);
		std::sort(bases.begin(), bases.end());
		V::Values bv, lv;
		for (const auto& b : bases) bv.push_back(V::String(b));
		for (const Link* l : into) lv.push_back(V::String(l->identity));
		const std::string coupling = Hash(V::ArrayValue({V::String("rise.simulation.coupling.v1"), policyValue, V::ArrayValue(bv), V::ArrayValue(lv)}));
		// Label.
		std::set<std::string> kinds;
		bool exact = true;
		for (const Link* l : into) {
			kinds.insert(l->adapter ? (l->c.adapter.empty() ? std::string("auto") : l->c.adapter) : std::string("direct"));
			if (l->adapter && l->adapter->Fidelity() != "predictive") exact = false;
		}
		std::string kind;
		for (const auto& k : kinds) kind += (kind.empty() ? "" : "+") + k;
		bool membersPredictive = true;
		for (std::size_t k : closure) {
			const Member& c = *members_[k];
			membersPredictive = membersPredictive && (c.cached ? c.snapshot->Description().fidelity == "predictive" :
				EffectiveFidelity(c.description.evidence) == "predictive");
		}
		// M7 prerequisites: owned implicit exchanges.  The owner's joint solve is
		// the implicit coupling when it declares every owned port on its side;
		// its own evidence (in the closure, so it must be predictive) stands in
		// for the coupling's.
		bool anyPlain = false;
		std::string undeclared;
		for (const Link* l : into) {
			if (!l->owned) { anyPlain = true; continue; }
			const Member& owner = *members_[l->owner];
			const std::string& port = l->to == l->owner ? l->c.toPort : l->c.fromPort;
			const auto& declared = owner.description.capabilities.ownedImplicitPorts;
			if (std::find(declared.begin(), declared.end(), port) == declared.end() && undeclared.empty())
				undeclared = owner.name + "." + port;
		}
		const bool ownedOnly = !anyPlain;
		if (ownedOnly) kind = "owned-implicit";
		Evidence coupled;
		std::string why;
		const bool haveEvidence = ownedOnly ? true : CouplingEvidence(kind, coupled, why) && EffectiveFidelity(coupled) == "predictive";
		if (!undeclared.empty())
			m.couplingLimit = "the owned exchange on " + undeclared + " is not declared by its owner as an implicit joint solve";
		else if (ownedOnly) { /* implicit through the owner's solve */ }
		else if (!implicit) m.couplingLimit = "explicit staggered coupling is first order in the macro step and lags one partner by a step";
		else if (!(policy.convergenceTolerance > 0 && policy.convergenceTolerance <= kPredictiveCouplingTolerance))
			m.couplingLimit = "implicit coupling needs a convergence tolerance in (0, 1e-6] to be predictive";
		else if (!exact) m.couplingLimit = "a connection goes through a model adapter (preview)";
		else if (!membersPredictive) m.couplingLimit = "a member of the coupling is not predictive";
		else if (!haveEvidence) m.couplingLimit = "the coupling " + kind + " has no passing evidence" + (why.empty() ? "" : ": " + why);
		auto& ev = m.description.evidence;
		EvidenceConditions conditions = ev.conditions;
		conditions.push_back({"coupling", {ownedOnly ? std::string("owned-implicit") : implicit ? std::string("implicit-") + OrderName(policy.order) :
			std::string("staggered-") + OrderName(policy.order)}});
		if (haveEvidence && !ownedOnly) {
			for (const auto& c : coupled.conditions) conditions.push_back({"coupling_" + c.first, c.second});
			for (const auto& c : coupled.cases) ev.cases.push_back(c);
		}
		std::sort(conditions.begin(), conditions.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		ev.conditions = conditions;
		if (ownedOnly)
			ev.scope += std::string(ev.scope.empty() ? "" : " ") + "Coupled in a co-stepping group through exchanges owned by "
				"a member that solves the partner's motion inside its own step (one exchange per macro step); the coupling is "
				"covered by that member's own evidence.";
		else
			ev.scope += std::string(ev.scope.empty() ? "" : " ") + "Coupled in a co-stepping group (" + kind + ", " +
				(implicit ? "implicit" : "staggered") + " " + OrderName(policy.order) + ")" +
				(haveEvidence ? ": " + coupled.scope : std::string(": the coupling itself has no evidence")) + ".";
		if (!m.couplingLimit.empty()) {
			ev.fidelity = "preview";
			m.description.capabilities.fidelityLimit = m.couplingLimit;
			m.description.notes.push_back("coupling: " + m.couplingLimit);
		}
		m.description.inputs.push_back(kCouplingInput);
		m.keyInputs = std::make_unique<CouplingInputs>(*m.inputs, coupling);
		if (!PrepareCacheDescription(m.description, *m.keyInputs, "", error)) return Fail(error, "member " + m.name + ": " + error);
		m.publisher.Reset(m.description.cache);
	}
	for (std::size_t i = 0; i < members_.size(); ++i)
		if (!members_[i]->cached) members_[i]->exchange = std::make_unique<Exchange>(*this, i);
	configured_ = true;
	return true;
}

bool SimulationGroup::Record(const std::string& member, const fs::path& root, std::size_t budget, bool resume, std::string& error)
{
	if (!configured_ || started_) return Fail(error, "record after Configure and before Start");
	for (auto& m : members_) {
		if (m->name != member || m->cached) continue;
		if (m->recorder) return Fail(error, "member " + member + " is already recorded");
		std::string parent;
		std::error_code ec;
		auto cache = m->description.cache;
		if (fs::exists(root/"manifest.cbor", ec)) {
			CacheSnapshot old;
			if (!CacheSnapshot::Open(root, budget, old, error)) return false;
			if (resume) {
				if (old.Description().key != cache.key) return Fail(error, "resume: the cache at " + root.string() + " holds another key");
				cache.epoch = old.Description().epoch;
			} else cache.epoch = DeriveEpoch(cache.key, old.Digest());
			parent = old.Digest();
		} else if (resume) return Fail(error, "resume: no cache at " + root.string());
		auto writer = std::make_unique<CacheWriter>();
		if (!writer->Begin(root, cache, parent, resume, budget, error)) return false;
		m->description.cache = cache;
		m->publisher.Reset(cache);
		m->recorder = std::move(writer);
		m->recordRoot = root;
		return true;
	}
	return Fail(error, "no live member named " + member);
}

bool SimulationGroup::WriteGroupManifests(std::string& error)
{
	V::Values ms, cs;
	for (const auto& m : members_) {
		const CacheDescription& d = m->cached ? m->snapshot->Description() : m->description.cache;
		ms.push_back(V::MapValue({{"name", V::String(m->name)}, {"kind", V::String(m->cached ? "cache" : "live")},
			{"producer", V::String(d.producer)}, {"key", V::String(d.key)}, {"epoch", V::String(d.epoch)},
			{"fidelity", V::String(d.fidelity)}}));
	}
	for (const auto& l : links_)
		cs.push_back(V::MapValue({{"from", V::String(l->c.fromMember + "." + l->c.fromPort)},
			{"to", V::String(l->c.toMember + "." + l->c.toPort)}, {"adapter", V::String(l->c.adapter)},
			{"identity", V::String(l->identity)}}));
	std::vector<std::string> orderNames;
	for (std::size_t i : order_) orderNames.push_back(members_[i]->name);
	Bytes bytes;
	if (!RISECBOR64::Encode(V::MapValue({{"format", V::String("rise.simulation.group.v1")}, {"identity", V::String(identity_)},
		{"policy", PolicyValue(policy_, macroStep_, orderNames)}, {"members", V::ArrayValue(ms)}, {"connections", V::ArrayValue(cs)}}),
		bytes, &error)) return false;
	for (const auto& m : members_)
		if (m->recorder && !ReplaceFileDurably(m->recordRoot, kGroupManifestName, bytes, error)) return false;
	return true;
}

bool SimulationGroup::Start(std::string& error)
{
	if (!configured_ || started_) return Fail(error, "start once, after Configure");
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		if (!m.solver->Start(error)) return Fail(error, "member " + m.name + ": " + error);
	}
	if (!WriteGroupManifests(error)) return false;
	// Every member publishes its first frame and offers its initial state.
	stepStart_ = stepEnd_ = time_;
	iteration_ = 0;
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		AdvanceRequest r;
		r.mode = AdvanceRequest::Mode::MacroStep;
		r.macroEnd = time_;
		AdvanceResult result;
		Sink sink(m, false);
		if (!m.solver->Advance(r, sink, m.exchange.get(), nullptr, result, error)) return Fail(error, "member " + m.name + ": " + error);
	}
	started_ = true;
	return true;
}

bool SimulationGroup::Latest(const Member& m, const std::string& port, double limit, std::shared_ptr<const Bytes>& value) const
{
	value.reset();
	if (m.cached) {
		std::string ignored;
		return m.feed->Accept(port, limit + m.timeOffset, value, ignored) == PortStatus::Ready;
	}
	const auto it = m.offers.find(port);
	if (it == m.offers.end()) return false;
	for (auto e = it->second.rbegin(); e != it->second.rend(); ++e)
		if (e->first <= limit) { value = e->second; return true; }
	return false;
}

PortStatus SimulationGroup::ResolveStatic(const std::string& digest, std::size_t budget, std::shared_ptr<const Bytes>& payload,
	std::string& error) const
{
	for (const auto& m : members_) {
		if (m->cached) {
			std::string why;
			if (m->feed->ResolveStatic(digest, budget, payload, why) == PortStatus::Ready) return PortStatus::Ready;
			continue;
		}
		const auto it = m->statics.find(digest);
		if (it == m->statics.end()) continue;
		if (it->second->size() > budget) { error = "static payload exceeds the budget"; return PortStatus::Failed; }
		payload = it->second;
		return PortStatus::Ready;
	}
	error = "static payload not offered: " + digest;
	return PortStatus::Unavailable;
}

bool SimulationGroup::Serve(Link& l, std::shared_ptr<const Bytes>& value, std::string& error)
{
	value.reset();
	if (l.relaxed) { value = l.relaxed; return true; }
	// Jacobi reads the step-start values on the first pass; Gauss-Seidel and the
	// later passes of an implicit step read the end-of-step offers.  A cache
	// stands in for a member that has always advanced (Gauss-Seidel) or not
	// yet (Jacobi), on every pass alike.
	const bool jacobi = policy_.order == ExchangeOrder::Jacobi;
	const double limit = members_[l.from]->cached ? (jacobi ? stepStart_ : stepEnd_) :
		(jacobi && iteration_ == 0 ? stepStart_ : stepEnd_);
	std::shared_ptr<const Bytes> raw;
	if (!Latest(*members_[l.from], l.c.fromPort, limit, raw)) return true;   // nothing yet: Unavailable
	if (!l.adapter) { value = raw; return true; }
	std::vector<std::shared_ptr<const Bytes>> aux;
	for (const auto& a : l.aux) {
		std::shared_ptr<const Bytes> x;
		Latest(*members_[a.first], a.second, limit, x);
		aux.push_back(x);
	}
	if (l.memoOut && l.memoRaw == raw && l.memoAux == aux) { value = l.memoOut; return true; }
	const StaticPayloadResolver resolve = [this](const std::string& d, std::size_t b, std::shared_ptr<const Bytes>& p, std::string& e) {
		return ResolveStatic(d, b, p, e) == PortStatus::Ready;
	};
	std::shared_ptr<const Bytes> out;
	if (!l.adapter->ConvertWithAuxiliary(l.offer, l.accept, *raw, aux, resolve, out, error))
		return Fail(error, l.c.fromMember + "." + l.c.fromPort + " -> " + l.c.toMember + "." + l.c.toPort + ": " + error);
	l.memoRaw = raw; l.memoAux = aux; l.memoOut = out;
	value = out;
	return true;
}

bool SimulationGroup::AdvanceAll(double b, bool buffered, std::string& error)
{
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		if (m.Finished()) continue;
		AdvanceRequest r;
		r.mode = AdvanceRequest::Mode::MacroStep;
		r.macroEnd = b;
		AdvanceResult result;
		Sink sink(m, buffered);
		if (!m.solver->Advance(r, sink, m.exchange.get(), nullptr, result, error))
			return Fail(error, "member " + m.name + " at t = " + std::to_string(b) + ": " + error);
		if (result.status == AdvanceResult::Status::Stopped) return Fail(error, "member " + m.name + " stopped inside a macro step");
	}
	return true;
}

void SimulationGroup::DropAfter(double t)
{
	for (auto& m : members_)
		for (auto& p : m->offers) {
			auto& list = p.second;
			list.erase(std::remove_if(list.begin(), list.end(), [t](const auto& e) { return e.first > t; }), list.end());
		}
}

void SimulationGroup::Prune(double t)
{
	for (auto& m : members_)
		for (auto& p : m->offers) {
			auto& list = p.second;
			std::size_t keep = 0;
			for (std::size_t k = 0; k < list.size(); ++k) if (list[k].first <= t) keep = k;
			if (keep > 0) list.erase(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(keep));
		}
}

bool SimulationGroup::StepInterval(double a, double b, int depth, std::string& error)
{
	stepStart_ = a;
	stepEnd_ = b;
	for (auto& l : links_) { l->served.reset(); l->relaxed.reset(); }
	// M7 prerequisites: with owned exchanges (every member belongs to one when
	// sub-iterating, Configure) the step is one exchange.
	if (policy_.subIterations <= 1 || !policy_.ownedExchanges.empty()) {
		iteration_ = 0;
		if (!AdvanceAll(b, false, error)) return false;
		for (auto& l : links_) l->accepted = l->served;
		report_.iterations = 1;
		++report_.totalIterations;
		report_.residual = 0;
		if (observer_) observer_(b, *this);
		return true;
	}
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		m.saved.reset();
		if (!m.Finished() && !m.solver->SaveState(m.saved, error)) return Fail(error, "member " + m.name + ": " + error);
	}
	auto restore = [&]() {
		for (std::size_t i : order_) {
			Member& m = *members_[i];
			// Statics stay: they are immutable, and a solver that published one in a
			// repeated pass need not publish it again after RestoreState.
			m.heldFrames.clear();
			if (m.saved && !m.solver->RestoreState(*m.saved, error)) return Fail(error, "member " + m.name + ": " + error);
		}
		return true;
	};
	const StaticPayloadResolver resolve = [this](const std::string& d, std::size_t bud, std::shared_ptr<const Bytes>& p, std::string& e) {
		return ResolveStatic(d, bud, p, e) == PortStatus::Ready;
	};
	std::ostringstream diagnostic;
	struct Pending { Link* link; std::vector<double> served, r; std::vector<int> groups; std::shared_ptr<const Bytes> fresh; };
	std::vector<Pending> pending;
	std::map<const Link*, std::map<int,double>> omega;
	std::map<const Link*, std::vector<double>> lastR;
	for (unsigned k = 0; k < policy_.subIterations; ++k) {
		iteration_ = k;
		if (k > 0 && !restore()) return false;
		for (auto& l : links_) l->served.reset();
		if (!AdvanceAll(b, true, error)) return false;
		++report_.totalIterations;
		// Residuals: what each connection would serve now (the end-of-step offers) against what it served.
		bool converged = true;
		double worst = 0;
		std::vector<std::pair<std::string,double>> residuals;
		diagnostic.str("");
		for (auto& l : links_) {
			if (!l->served) continue;
			const auto relaxed = l->relaxed;
			l->relaxed.reset();
			const unsigned saved = iteration_;
			iteration_ = 1;                 // the end-of-step value
			std::shared_ptr<const Bytes> fresh;
			const bool ok = Serve(*l, fresh, error);
			iteration_ = saved;
			l->relaxed = relaxed;
			if (!ok) return false;
			if (!fresh) continue;
			std::vector<double> vn, vs;
			std::vector<int> groups, ignoredGroups;
			if (!PayloadVector(l->accept.codec, *fresh, resolve, vn, groups, error) ||
				!PayloadVector(l->accept.codec, *l->served, resolve, vs, ignoredGroups, error))
				return Fail(error, "coupling residual: " + error);
			std::vector<double> r(vn.size());
			bool shape = vn.size() == vs.size();
			if (shape) for (std::size_t q = 0; q < vn.size(); ++q) r[q] = vn[q] - vs[q];
			const double rn = shape ? Norm(r) : INFINITY, nn = Norm(vn);
			const double rel = rn/std::max(nn, 1e-300);
			const std::string name = l->c.fromMember + "." + l->c.fromPort + "->" + l->c.toMember + "." + l->c.toPort;
			residuals.emplace_back(name, rel);
			worst = std::max(worst, std::isfinite(rel) ? rel : 1e300);
			diagnostic << " " << name << " " << rn << " (relative " << rel << ")";
			const bool linkConverged = policy_.convergenceTolerance > 0 ?
				rn <= policy_.convergenceTolerance*nn + policy_.absoluteTolerance : false;
			converged = converged && linkConverged;
			// The next iterate: under-relaxed, unless the source always advances first.
			if (l->fresh || !shape) { l->relaxed.reset(); continue; }
			pending.push_back({l.get(), std::move(vs), std::move(r), std::move(groups), fresh});
		}
		// Relaxation factors per (connection, group) -- a position and a velocity
		// beside it relax separately -- updated by Aitken's rule from the second
		// iterate on (either sign: a monotonically divergent map needs omega < 0;
		// |omega| in [1e-4, 2]).
		for (auto& p : pending) {
			std::map<int, std::pair<double,double>> sums;   // group -> (num, den)
			auto& last = lastR[p.link];
			const bool have = policy_.aitken && last.size() == p.r.size();
			if (have)
				for (std::size_t q = 0; q < p.r.size(); ++q) {
					const double d = p.r[q] - last[q];
					sums[p.groups[q]].first += last[q]*d;
					sums[p.groups[q]].second += d*d;
				}
			auto& w = omega[p.link];
			for (int g : p.groups) if (!w.count(g)) w[g] = policy_.relaxation;
			if (have)
				for (const auto& sgrp : sums)
					if (sgrp.second.second > 0 && std::isfinite(sgrp.second.first/sgrp.second.second))
					{
						const double f = -w[sgrp.first]*sgrp.second.first/sgrp.second.second;
						w[sgrp.first] = std::copysign(std::min(2.0, std::max(1e-4, std::fabs(f))), f);
					}
			last = p.r;
			std::vector<double> next(p.served.size());
			bool all1 = true;
			for (std::size_t q = 0; q < next.size(); ++q) {
				const double f = w[p.groups[q]];
				all1 = all1 && f == 1;
				next[q] = p.served[q] + f*p.r[q];
			}
			std::shared_ptr<const Bytes> relaxedNext;
			std::string why;
			if (!all1 && ReplaceVector(p.link->accept.codec, *p.link->served, next, relaxedNext, why)) p.link->relaxed = relaxedNext;
			else p.link->relaxed = p.fresh;
		}
		pending.clear();
		if (policy_.convergenceTolerance == 0 && k + 1 == policy_.subIterations) converged = true;
		if (converged) {
			for (std::size_t i : order_) if (!Sink::Flush(*members_[i], error)) return false;
			for (auto& l : links_) { l->accepted = l->served; l->relaxed.reset(); }
			report_.iterations = k + 1;
			report_.residual = worst;
			report_.connectionResiduals = residuals;
			if (observer_) observer_(b, *this);
			return true;
		}
	}
	if (!restore()) return false;
	for (auto& l : links_) l->relaxed.reset();
	const double floor = policy_.minimumMacroStep > 0 ? policy_.minimumMacroStep : macroStep_/64;
	if (policy_.onNonConvergence == GroupPolicy::NonConvergence::Halve && (b - a)/2 >= floor) {
		DropAfter(a);
		++report_.halvings;
		const double m = a + (b - a)/2;
		return StepInterval(a, m, depth + 1, error) && StepInterval(m, b, depth + 1, error);
	}
	std::ostringstream out;
	out << "coupling did not converge in " << policy_.subIterations << " iterations on [" << a << ", " << b << "] s"
		<< (depth ? " (after halving)" : "") << ": residuals" << diagnostic.str();
	return Fail(error, out.str());
}

bool SimulationGroup::StepMacro(std::string& error)
{
	if (!started_) return Fail(error, "start or restore the group first");
	if (Finished()) return true;
	const double a = time_;
	const double b = members_[order_.front()]->description.cache.startTime + static_cast<double>(steps_ + 1)*macroStep_;
	report_ = StepReport();
	if (!StepInterval(a, b, 0, error)) return false;
	time_ = b;
	++steps_;
	Prune(b);
	return true;
}

bool SimulationGroup::Finished() const
{
	for (std::size_t i : order_) if (!members_[i]->Finished()) return false;
	return true;
}

bool SimulationGroup::Run(std::string& error)
{
	while (!Finished()) {
		if (stop_) return true;
		if (!StepMacro(error)) return false;
	}
	return true;
}

bool SimulationGroup::SealRecordings(std::string& error)
{
	for (auto& m : members_) if (m->recorder && !m->recorder->Seal(error)) return Fail(error, "member " + m->name + ": " + error);
	return true;
}

bool SimulationGroup::Checkpoint(Bytes& state, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	V::Values ms, offers, statics;
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		Bytes s;
		if (!m.solver->Checkpoint(s, error)) return Fail(error, "member " + m.name + ": " + error);
		ms.push_back(V::ArrayValue({V::String(m.name), V::BytesValue(s)}));
	}
	for (const auto& m : members_) {
		for (const auto& p : m->offers)
			for (const auto& e : p.second)
				offers.push_back(V::ArrayValue({V::String(m->name), V::String(p.first), V::Float(e.first), V::BytesValue(*e.second)}));
		for (const auto& s : m->statics) statics.push_back(V::ArrayValue({V::String(m->name), V::BytesValue(*s.second)}));
	}
	return RISECBOR64::Encode(V::MapValue({{"format", V::String(StateFormat)}, {"identity", V::String(identity_)},
		{"time", V::Float(time_)}, {"steps", V::Unsigned(steps_)}, {"members", V::ArrayValue(ms)},
		{"offers", V::ArrayValue(offers)}, {"statics", V::ArrayValue(statics)}}), state, &error);
}

bool SimulationGroup::Restore(const Bytes& state, std::string& error)
{
	if (!configured_ || started_) return Fail(error, "restore once, after Configure, instead of Start");
	V v;
	if (!CanDecodeCanonicalRecord(state, std::size_t(4) << 30) || !RISECBOR64::DecodeCanonical(state, v, &error) || v.GetType() != V::Map)
		return Fail(error, "invalid group state");
	const V *format = v.Find("format"), *identity = v.Find("identity"), *time = v.Find("time"), *steps = v.Find("steps"),
		*ms = v.Find("members"), *offers = v.Find("offers"), *statics = v.Find("statics");
	if (!format || format->GetType() != V::Text || format->GetText() != StateFormat || !identity || identity->GetType() != V::Text ||
		!time || time->GetType() != V::Float64 || !steps || steps->GetType() != V::UnsignedInteger || !ms || ms->GetType() != V::Array ||
		!offers || offers->GetType() != V::Array || !statics || statics->GetType() != V::Array)
		return Fail(error, "invalid group state fields");
	if (identity->GetText() != identity_) return Fail(error, "the group state belongs to another group configuration");
	auto member = [&](const std::string& name) -> Member* {
		for (auto& m : members_) if (m->name == name) return m.get();
		return nullptr;
	};
	for (const V& e : ms->GetArray()) {
		if (e.GetType() != V::Array || e.GetArray().size() != 2 || e.GetArray()[0].GetType() != V::Text ||
			e.GetArray()[1].GetType() != V::ByteString) return Fail(error, "invalid group member state");
		Member* m = member(e.GetArray()[0].GetText());
		if (!m || m->cached) return Fail(error, "group state names an unknown member");
		if (!m->solver->Restore(e.GetArray()[1].GetBytes(), error)) return Fail(error, "member " + m->name + ": " + error);
	}
	for (const V& e : offers->GetArray()) {
		const auto& a = e.GetArray();
		if (e.GetType() != V::Array || a.size() != 4 || a[0].GetType() != V::Text || a[1].GetType() != V::Text ||
			a[2].GetType() != V::Float64 || a[3].GetType() != V::ByteString) return Fail(error, "invalid group offer");
		Member* m = member(a[0].GetText());
		if (!m) return Fail(error, "group state names an unknown member");
		m->offers[a[1].GetText()].emplace_back(a[2].GetFloat(), std::make_shared<const Bytes>(a[3].GetBytes()));
	}
	for (const V& e : statics->GetArray()) {
		const auto& a = e.GetArray();
		if (e.GetType() != V::Array || a.size() != 2 || a[0].GetType() != V::Text || a[1].GetType() != V::ByteString)
			return Fail(error, "invalid group static");
		Member* m = member(a[0].GetText());
		if (!m) return Fail(error, "group state names an unknown member");
		auto bytes = std::make_shared<const Bytes>(a[1].GetBytes());
		m->statics[RISECBOR64::SHA256Hex(*bytes)] = bytes;
	}
	time_ = time->GetFloat();
	steps_ = steps->GetIntegerArgument();
	if (!WriteGroupManifests(error)) return false;
	started_ = true;
	return true;
}

namespace {
constexpr const char* StreamFormat = "rise.simulation.group.stream.v1";

class CountingSink final : public ICheckpointSink {
public:
	bool Write(const std::uint8_t*, std::size_t size, std::string&) override { length += size; return true; }
	std::uint64_t length = 0;
};

class CountedSink final : public ICheckpointSink {
public:
	CountedSink(ICheckpointSink& inner) : inner_(inner) {}
	bool Write(const std::uint8_t* data, std::size_t size, std::string& error) override
	{
		length += size;
		return inner_.Write(data, size, error);
	}
	std::uint64_t length = 0;
private:
	ICheckpointSink& inner_;
};

class LimitedSource final : public ICheckpointSource {
public:
	LimitedSource(ICheckpointSource& inner, std::uint64_t length) : inner_(inner), left_(length) {}
	bool Read(std::uint8_t* data, std::size_t size, std::string& error) override
	{
		if (size > left_) { error = "member read past its checkpoint stream"; return false; }
		if (!inner_.Read(data, size, error)) return false;
		left_ -= size;
		return true;
	}
	std::uint64_t Remaining() const override { return left_; }
private:
	ICheckpointSource& inner_;
	std::uint64_t left_;
};
}

bool SimulationGroup::CheckpointTo(ICheckpointSink& sink, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	V::Values ms, offers, statics;
	std::vector<std::uint64_t> lengths;
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		CountingSink count;
		if (!m.solver->CheckpointTo(count, error)) return Fail(error, "member " + m.name + ": " + error);
		lengths.push_back(count.length);
		ms.push_back(V::ArrayValue({V::String(m.name), V::Unsigned(count.length)}));
	}
	for (const auto& m : members_) {
		for (const auto& p : m->offers)
			for (const auto& e : p.second)
				offers.push_back(V::ArrayValue({V::String(m->name), V::String(p.first), V::Float(e.first), V::BytesValue(*e.second)}));
		for (const auto& s : m->statics) statics.push_back(V::ArrayValue({V::String(m->name), V::BytesValue(*s.second)}));
	}
	Bytes index;
	if (!RISECBOR64::Encode(V::MapValue({{"format", V::String(StreamFormat)}, {"identity", V::String(identity_)},
		{"time", V::Float(time_)}, {"steps", V::Unsigned(steps_)}, {"members", V::ArrayValue(ms)},
		{"offers", V::ArrayValue(offers)}, {"statics", V::ArrayValue(statics)}}), index, &error)) return false;
	std::uint8_t size[8];
	for (unsigned k = 0; k < 8; ++k) size[k] = static_cast<std::uint8_t>(std::uint64_t(index.size()) >> (8*k));
	if (!sink.Write(size, 8, error) || !sink.Write(index.data(), index.size(), error)) return false;
	std::size_t at = 0;
	for (std::size_t i : order_) {
		Member& m = *members_[i];
		CountedSink counted(sink);
		if (!m.solver->CheckpointTo(counted, error)) return Fail(error, "member " + m.name + ": " + error);
		if (counted.length != lengths[at++]) return Fail(error, "member " + m.name + ": its checkpoint stream changed length between passes");
	}
	return true;
}

bool SimulationGroup::RestoreFrom(ICheckpointSource& source, std::string& error)
{
	if (!configured_ || started_) return Fail(error, "restore once, after Configure, instead of Start");
	std::uint8_t size[8];
	if (!source.Read(size, 8, error)) return Fail(error, "invalid group stream");
	std::uint64_t n = 0;
	for (unsigned k = 0; k < 8; ++k) n |= std::uint64_t(size[k]) << (8*k);
	if (n > source.Remaining() || n > (std::uint64_t(1) << 32)) return Fail(error, "invalid group stream index");
	Bytes index(static_cast<std::size_t>(n));
	if (!source.Read(index.data(), index.size(), error)) return false;
	V v;
	if (!CanDecodeCanonicalRecord(index, std::size_t(4) << 30) || !RISECBOR64::DecodeCanonical(index, v, &error) || v.GetType() != V::Map)
		return Fail(error, "invalid group stream index");
	const V *format = v.Find("format"), *identity = v.Find("identity"), *time = v.Find("time"), *steps = v.Find("steps"),
		*ms = v.Find("members"), *offers = v.Find("offers"), *statics = v.Find("statics");
	if (!format || format->GetType() != V::Text || format->GetText() != StreamFormat || !identity || identity->GetType() != V::Text ||
		!time || time->GetType() != V::Float64 || !steps || steps->GetType() != V::UnsignedInteger || !ms || ms->GetType() != V::Array ||
		!offers || offers->GetType() != V::Array || !statics || statics->GetType() != V::Array)
		return Fail(error, "invalid group stream fields");
	if (identity->GetText() != identity_) return Fail(error, "the group state belongs to another group configuration");
	auto member = [&](const std::string& name) -> Member* {
		for (auto& m : members_) if (m->name == name) return m.get();
		return nullptr;
	};
	for (const V& e : ms->GetArray()) {
		if (e.GetType() != V::Array || e.GetArray().size() != 2 || e.GetArray()[0].GetType() != V::Text ||
			e.GetArray()[1].GetType() != V::UnsignedInteger) return Fail(error, "invalid group member entry");
		Member* m = member(e.GetArray()[0].GetText());
		if (!m || m->cached) return Fail(error, "group state names an unknown member");
		LimitedSource limited(source, e.GetArray()[1].GetIntegerArgument());
		if (limited.Remaining() > source.Remaining()) return Fail(error, "group stream is truncated");
		if (!m->solver->RestoreFrom(limited, error)) return Fail(error, "member " + m->name + ": " + error);
		if (limited.Remaining() != 0) return Fail(error, "member " + m->name + ": did not consume its checkpoint stream");
	}
	for (const V& e : offers->GetArray()) {
		const auto& a = e.GetArray();
		if (e.GetType() != V::Array || a.size() != 4 || a[0].GetType() != V::Text || a[1].GetType() != V::Text ||
			a[2].GetType() != V::Float64 || a[3].GetType() != V::ByteString) return Fail(error, "invalid group offer");
		Member* m = member(a[0].GetText());
		if (!m) return Fail(error, "group state names an unknown member");
		m->offers[a[1].GetText()].emplace_back(a[2].GetFloat(), std::make_shared<const Bytes>(a[3].GetBytes()));
	}
	for (const V& e : statics->GetArray()) {
		const auto& a = e.GetArray();
		if (e.GetType() != V::Array || a.size() != 2 || a[0].GetType() != V::Text || a[1].GetType() != V::ByteString)
			return Fail(error, "invalid group static");
		Member* m = member(a[0].GetText());
		if (!m) return Fail(error, "group state names an unknown member");
		auto bytes = std::make_shared<const Bytes>(a[1].GetBytes());
		m->statics[RISECBOR64::SHA256Hex(*bytes)] = bytes;
	}
	if (source.Remaining() != 0) return Fail(error, "trailing bytes after the group stream");
	time_ = time->GetFloat();
	steps_ = steps->GetIntegerArgument();
	if (!WriteGroupManifests(error)) return false;
	started_ = true;
	return true;
}

std::shared_ptr<const IReadOnlyCacheSnapshot> SimulationGroup::Snapshot(const std::string& member) const
{
	for (const auto& m : members_) if (m->name == member) return m->cached ? m->snapshot : m->publisher.Current();
	return nullptr;
}

const SolverDescription* SimulationGroup::Description(const std::string& member) const
{
	for (const auto& m : members_) if (m->name == member && !m->cached) return &m->description;
	return nullptr;
}

std::vector<std::string> SimulationGroup::Members() const
{
	std::vector<std::string> names;
	for (const auto& m : members_) names.push_back(m->name);
	return names;
}

bool SimulationGroup::IsCached(const std::string& member) const
{
	for (const auto& m : members_) if (m->name == member) return m->cached;
	return false;
}

std::string SimulationGroup::MemberFidelity(const std::string& member) const
{
	for (const auto& m : members_) if (m->name == member) return m->cached ? m->snapshot->Description().fidelity : m->description.cache.fidelity;
	return std::string();
}

std::string SimulationGroup::CouplingLimit(const std::string& member) const
{
	for (const auto& m : members_) if (m->name == member) return m->couplingLimit;
	return std::string();
}

std::shared_ptr<const Bytes> SimulationGroup::ServedValue(std::size_t connection) const
{
	return connection < links_.size() ? links_[connection]->accepted : nullptr;
}

std::size_t SimulationGroup::ConnectionCount() const { return links_.size(); }

} }
