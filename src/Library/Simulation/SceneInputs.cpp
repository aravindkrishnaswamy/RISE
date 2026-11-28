//////////////////////////////////////////////////////////////////////
//
//  SceneInputs.cpp - Captures scene objects as immutable, thread-safe
//  ISceneInputs snapshots (world rigid transforms and tessellated or
//  cached meshes in metres) with deterministic content-addressed hashes.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SceneInputs.h"
#include "SimulationMesh.h"
#include "Cloth/ClothParameterSchema.h"
#include "../Animation/Animator.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IGeometry.h"
#include "../Objects/Object.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <typeinfo>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;
constexpr std::size_t payloadBudget = std::size_t(512) << 20;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

std::uint32_t F32(double x)
{
	const float f = static_cast<float>(x);
	std::uint32_t bits;
	std::memcpy(&bits, &f, 4);
	return bits;
}

const char* EndPolicyName(MeshEndPolicy e)
{
	switch (e) {
	case MeshEndPolicy::Cache: return "cache";
	case MeshEndPolicy::Hold: return "hold";
	case MeshEndPolicy::Loop: return "loop";
	case MeshEndPolicy::Error: break;
	}
	return "error";
}

// Column-vector linear part (row-major) and translation of a RISE Matrix4
// (row-vector convention: x' = x M).
void Linear(const Matrix4& m, double a[9], double t[3])
{
	a[0] = m._00; a[1] = m._10; a[2] = m._20;
	a[3] = m._01; a[4] = m._11; a[5] = m._21;
	a[6] = m._02; a[7] = m._12; a[8] = m._22;
	t[0] = m._30; t[1] = m._31; t[2] = m._32;
}

double Det(const double a[9])
{
	return a[0]*(a[4]*a[8] - a[5]*a[7]) - a[1]*(a[3]*a[8] - a[5]*a[6]) + a[2]*(a[3]*a[7] - a[4]*a[6]);
}

// One captured object: pose holders of it and its ancestors (root first),
// the shared elements they stand for (animator keys only, never read), and
// its geometry.
struct Captured {
	std::string name;
	std::vector<Implementation::Object*> holders;
	std::vector<IKeyframable*> keys;
	std::vector<bool> animated;
	bool simulated = false;
	SimulatedGeometryBinding binding;
	std::string upstreamKey;                  // expected key of binding.simulation
	std::vector<double> local;                // object-space vertices (3 per vertex), static geometry
	std::vector<std::uint32_t> indices;
	double stretch0[9] = {1,0,0, 0,1,0, 0,0,1};
	std::string identity;
	// Lazily opened reader of a simulated geometry's cache, and the last sample.
	mutable std::mutex mutex;
	mutable std::unique_ptr<MeshCacheSource> source;
	mutable bool sampled = false;
	mutable double sampledAt = 0;
	mutable std::shared_ptr<const Bytes> sample;
	~Captured() { for (auto* h : holders) if (h) h->release(); }
};

// Immutable ISceneInputs snapshot that evaluates animated transforms on private
// pose-holder clones and lazily reads simulated upstream mesh caches.
class Inputs final : public ISceneInputs {
public:
	Inputs(const IAnimator* animator) : animator_(animator) { if (animator_) animator_->addref(); }
	~Inputs() override { if (animator_) animator_->release(); }
	Inputs(const Inputs&) = delete;
	Inputs& operator=(const Inputs&) = delete;

	std::vector<std::unique_ptr<Captured>> items;

	const Captured* Find(const std::string& name) const
	{
		for (const auto& c : items) if (c->name == name) return c.get();
		return nullptr;
	}

	// The world matrix of `c` at time t, posed on private clones.
	bool Pose(const Captured& c, double t, Matrix4& world, std::string& error) const
	{
		Matrix4 parent = Matrix4Ops::Identity();
		for (std::size_t k = 0; k < c.holders.size(); ++k) {
			Implementation::Object* pose = c.holders[k]->ClonePoseHolder();
			if (!pose) return Fail(error, "cannot pose `" + c.name + "`");
			if (c.animated[k] && animator_) animator_->EvaluateElementAtTimeInto(c.keys[k], t, *pose);
			pose->FinalizeTransformations(parent);
			parent = pose->GetFinalTransformMatrix();
			pose->release();
		}
		world = parent;
		return true;
	}

	// Extracts the rigid world transform at time t, refusing time-varying scale or shear.
	bool Rigid(const Captured& c, double t, RigidTransform& out, std::string& error) const
	{
		Matrix4 m;
		if (!Pose(c, t, m, error)) return false;
		double a[9], tr[3], r[9];
		Linear(m, a, tr);
		if (!PolarRotation(a, r)) return Fail(error, "the transform of `" + c.name + "` is singular or mirrored");
		// The stretch R^T A must be the one at t = 0: only rigid motion moves a collider.
		double s[9], scale = 0;
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
			s[3*i + j] = r[i]*a[j] + r[3 + i]*a[3 + j] + r[6 + i]*a[6 + j];
			scale = std::max(scale, std::fabs(c.stretch0[3*i + j]));
		}
		for (int k = 0; k < 9; ++k)
			if (std::fabs(s[k] - c.stretch0[k]) > 1e-9*std::max(1.0, scale))
				return Fail(error, "`" + c.name + "` changes scale or shear over time; a scene input moves rigidly only");
		std::copy(r, r + 9, out.rotation.begin());
		std::copy(tr, tr + 3, out.translation.begin());
		return true;
	}

	bool Identity(const std::string& name, std::string& hash, std::string& error) const override
	{
		const Captured* c = Find(name);
		if (!c) return Fail(error, "no scene input named " + name);
		hash = c->identity;
		return true;
	}

	bool SampleTransform(const std::string& name, double time, RigidTransform& transform,
		std::string& error) const override
	{
		const Captured* c = Find(name);
		if (!c) return Fail(error, "no scene input named " + name);
		if (!std::isfinite(time)) return Fail(error, "nonfinite sample time");
		return Rigid(*c, time, transform, error);
	}

	bool Sample(const std::string& name, double time, std::shared_ptr<const Bytes>& encoded, std::string& codec,
		std::string& error) const override
	{
		const Captured* c = Find(name);
		if (!c) return Fail(error, "no scene input named " + name);
		if (!std::isfinite(time)) return Fail(error, "nonfinite sample time");
		std::lock_guard<std::mutex> hold(c->mutex);
		if (c->sampled && c->sampledAt == time) { encoded = c->sample; codec = "mesh.v1"; return true; }
		std::vector<double> local;
		const std::vector<std::uint32_t>* indices = &c->indices;
		if (c->simulated) {
			if (!c->source) {
				std::unique_ptr<MeshCacheSource> source;
				if (!MeshCacheSource::Open(c->binding.cacheDir, c->binding.options, source, error))
					return Fail(error, "`" + name + "` reads simulation cache " + c->binding.cacheDir.string() + ": " + error);
				if (!c->binding.simulation.empty() && source->Description().key != c->upstreamKey)
					return Fail(error, "`" + name + "` reads simulation " + c->binding.simulation +
						", whose cache does not hold its current configuration (bake it first)");
				c->source = std::move(source);
			}
			MeshFrameSample frame;
			if (!c->source->Sample(time, time, time, false, frame, error)) return false;
			if (frame.state != MeshFrameSample::State::Ready)
				return Fail(error, "`" + name + "`: simulation frame " + std::to_string(frame.index) + " is not baked");
			local = std::move(frame.nominal);
			indices = &c->source->Indices();
		} else {
			local = c->local;
		}
		Matrix4 m;
		if (!Pose(*c, time, m, error)) return false;
		MeshFrame mesh;
		mesh.positionUnits = "m";
		mesh.positions.reserve(local.size());
		for (std::size_t i = 0; i + 2 < local.size(); i += 3) {
			const Point3 p = Point3Ops::Transform(m, Point3(local[i], local[i + 1], local[i + 2]));
			mesh.positions.push_back(F32(p.x));
			mesh.positions.push_back(F32(p.y));
			mesh.positions.push_back(F32(p.z));
		}
		mesh.indices = *indices;
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeMesh(mesh, payloadBudget, *bytes, error)) return false;
		c->sample = bytes;
		c->sampled = true;
		c->sampledAt = time;
		encoded = std::move(bytes);
		codec = "mesh.v1";
		return true;
	}

	const IAnimator* animator_;
};

V MatrixValue(const Matrix4& m)
{
	const double e[16] = {m._00, m._01, m._02, m._03, m._10, m._11, m._12, m._13,
		m._20, m._21, m._22, m._23, m._30, m._31, m._32, m._33};
	V::Values out;
	for (double x : e) out.push_back(V::Float(x));
	return V::ArrayValue(out);
}

} // namespace

// Newton-Schulz polar decomposition X <- (X + X^-T)/2 for proper rotations (det > 0).
bool PolarRotation(const double a[9], double r[9])
{
	if (!(Det(a) > 0)) return false;
	double x[9];
	std::copy(a, a + 9, x);
	for (int iteration = 0; iteration < 100; ++iteration) {
		// x <- (x + x^-T) / 2
		const double d = Det(x);
		if (!(std::fabs(d) > 0) || !std::isfinite(d)) return false;
		double invT[9] = {
			(x[4]*x[8] - x[5]*x[7])/d, -(x[3]*x[8] - x[5]*x[6])/d, (x[3]*x[7] - x[4]*x[6])/d,
			-(x[1]*x[8] - x[2]*x[7])/d, (x[0]*x[8] - x[2]*x[6])/d, -(x[0]*x[7] - x[1]*x[6])/d,
			(x[1]*x[5] - x[2]*x[4])/d, -(x[0]*x[5] - x[2]*x[3])/d, (x[0]*x[4] - x[1]*x[3])/d};
		double change = 0;
		for (int k = 0; k < 9; ++k) {
			const double next = 0.5*(x[k] + invT[k]);
			change = std::max(change, std::fabs(next - x[k]));
			x[k] = next;
		}
		if (change <= 1e-15) break;
	}
	std::copy(x, x + 9, r);
	return true;
}

SceneObjectInputProvider::SceneObjectInputProvider(const IScene& scene, SimulatedGeometryLookup lookup) :
	scene_(scene), lookup_(std::move(lookup))
{
}

bool SceneObjectInputProvider::InputDependencies(const std::string& input, std::vector<std::string>& simulations,
	std::string& error) const
{
	simulations.clear();
	const IObjectManager* objects = scene_.GetObjects();
	IObjectPriv* object = objects ? objects->GetItem(input.c_str()) : nullptr;
	if (!object) return Fail(error, "no scene object named `" + input + "`");
	SimulatedGeometryBinding binding;
	if (lookup_ && object->GetGeometry() && lookup_(object->GetGeometry(), binding) && !binding.simulation.empty())
		simulations.push_back(binding.simulation);
	return true;
}

// Captures named scene objects: validates non-CSG built-in Object chains, clones
// authored pose holders, encodes static/cached geometry, verifies rigid motion at
// keyframes and quarter intervals, and hashes the canonical CBOR identity.
std::shared_ptr<const ISceneInputs> SceneObjectInputProvider::Capture(const std::vector<std::string>& names,
	const std::map<std::string, std::string>& upstreamKeys, std::string& error) const
{
	auto inputs = std::make_shared<Inputs>(scene_.GetAnimator());
	const IObjectManager* objects = scene_.GetObjects();
	const IAnimator* animator = scene_.GetAnimator();
	const auto* timelines = dynamic_cast<const Implementation::Animator*>(animator);
	if (!objects) { error = "the scene has no objects"; return nullptr; }
	for (const auto& name : names) {
		if (inputs->Find(name)) continue;
		auto c = std::make_unique<Captured>();
		c->name = name;
		IObjectPriv* item = objects->GetItem(name.c_str());
		auto* object = dynamic_cast<Implementation::Object*>(item);
		// Pose holders are nonvirtual base-Object clones. Validate the entire
		// chain before cloning so custom setters/finalization and CSG behavior
		// cannot be silently discarded from a simulation input.
		if (!object || typeid(*object) != typeid(Implementation::Object)) {
			error = item ? "scene object `" + name + "` is not an exact built-in Object (CSG and custom subclasses cannot be scene inputs)" :
				"no scene object named `" + name + "`";
			return nullptr;
		}
		if (object->IsConsumed()) {
			error = "scene object `" + name + "` is a CSG operand (its composite-local pose has no unique world placement)";
			return nullptr;
		}
		// The chain from the root to the object.
		std::vector<Implementation::Object*> chain{object};
		std::vector<std::string> chainNames{name};
		std::set<std::string> seen{name};
		for (const char* parent = objects->GetObjectParent(name.c_str()); parent && parent[0];
			parent = objects->GetObjectParent(parent)) {
			const std::string p = parent;
			if (!seen.insert(p).second) { error = "object hierarchy cycle at `" + p + "`"; return nullptr; }
			auto* ancestor = dynamic_cast<Implementation::Object*>(objects->GetItem(p.c_str()));
			if (!ancestor || typeid(*ancestor) != typeid(Implementation::Object)) {
				error = "the parent `" + p + "` of `" + name + "` is not an exact built-in Object (CSG and custom subclasses cannot be scene inputs)";
				return nullptr;
			}
			if (ancestor->IsConsumed()) {
				error = "the parent `" + p + "` of `" + name + "` is a CSG operand (its composite-local pose has no unique world placement)";
				return nullptr;
			}
			chain.push_back(ancestor);
			chainNames.push_back(p);
		}
		std::reverse(chain.begin(), chain.end());
		std::reverse(chainNames.begin(), chainNames.end());
		std::vector<Scalar> keyTimes;
		for (std::size_t n = 0; n < chain.size(); ++n) {
			Implementation::Object* node = chain[n];
			Implementation::Object* holder = node->ClonePoseHolder();
			const auto drivenPose = driven_.find(chainNames[n]);
			if (drivenPose != driven_.end() && holder) {
				// The shared object carries the simulation's pose (a scrub, a live run); the holder gets the authored one.
				for (const auto& pv : {std::make_pair("position", &drivenPose->second.position),
					std::make_pair("orientation", &drivenPose->second.orientation)}) {
					if (pv.second->empty()) continue;
					IKeyframeParameter* value = holder->KeyframeFromParameters(pv.first, pv.second->c_str());
					if (!value) { holder->release(); error = "cannot pose `" + chainNames[n] + "` at its authored " + pv.first; return nullptr; }
					holder->SetIntermediateValue(*value);
					safe_release(value);
				}
				holder->RegenerateData();
			}
			c->holders.push_back(holder);
			c->keys.push_back(node);
			std::vector<String> params;
			// M6: a simulation-driven object is read at its authored pose.
			const bool animated = !driven_.count(chainNames[n]) && animator && animator->GetActiveAnimatedParameters(node, params);
			c->animated.push_back(animated);
			if (animated && timelines) timelines->CollectElementKeyframeTimes(node, keyTimes);
		}
		// Geometry.
		const IGeometry* geometry = object->GetGeometry();
		if (!geometry) { error = "scene object `" + name + "` has no geometry"; return nullptr; }
		V geometryValue;
		if (lookup_ && lookup_(geometry, c->binding)) {
			c->simulated = true;
			if (!c->binding.simulation.empty()) {
				const auto k = upstreamKeys.find(c->binding.simulation);
				if (k == upstreamKeys.end() || k->second.empty()) {
					error = "`" + name + "` shows simulation " + c->binding.simulation + ", which has no key";
					return nullptr;
				}
				c->upstreamKey = k->second;
				geometryValue = V::ArrayValue({V::String("simulation"), V::String(c->upstreamKey),
					V::String(c->binding.output), V::Float(c->binding.options.timeOffset),
					V::Float(c->binding.options.timeScale), V::String(EndPolicyName(c->binding.options.end))});
			} else {
				CacheSnapshot snapshot;
				std::string why;
				if (!CacheSnapshot::Open(c->binding.cacheDir, payloadBudget, snapshot, why)) {
					error = "`" + name + "` shows cache " + c->binding.cacheDir.string() + ": " + why;
					return nullptr;
				}
				geometryValue = V::ArrayValue({V::String("cache"), V::String(snapshot.Description().key),
					V::String(snapshot.Digest()), V::String(c->binding.output), V::Float(c->binding.options.timeOffset),
					V::Float(c->binding.options.timeScale), V::String(EndPolicyName(c->binding.options.end))});
			}
		} else {
			IndexTriangleListType tris;
			VerticesListType vertices;
			NormalsListType normals;
			TexCoordsListType coords;
			if (!geometry->TessellateToMesh(tris, vertices, normals, coords, ClothColliderTessellationDetail) ||
				vertices.empty() || tris.empty()) {
				error = "the geometry of scene object `" + name + "` cannot be tessellated";
				return nullptr;
			}
			for (const auto& p : vertices) { c->local.push_back(p.x); c->local.push_back(p.y); c->local.push_back(p.z); }
			for (const auto& t : tris) for (int k = 0; k < 3; ++k) {
				if (t.iVertices[k] >= vertices.size()) { error = "tessellation of `" + name + "` is invalid"; return nullptr; }
				c->indices.push_back(static_cast<std::uint32_t>(t.iVertices[k]));
			}
			MeshFrame mesh;
			for (double x : c->local) mesh.positions.push_back(F32(x));
			mesh.indices = c->indices;
			Bytes payload;
			if (!EncodeMesh(mesh, payloadBudget, payload, error)) return nullptr;
			geometryValue = V::ArrayValue({V::String("mesh"), V::Unsigned(ClothColliderTessellationDetail),
				V::String(RISECBOR64::SHA256Hex(payload))});
		}
		// Motion: M(t) at 0, every keyframe time, and between keyframes.
		std::sort(keyTimes.begin(), keyTimes.end());
		keyTimes.erase(std::unique(keyTimes.begin(), keyTimes.end()), keyTimes.end());
		std::vector<double> times{0.0};
		for (std::size_t k = 0; k < keyTimes.size(); ++k) {
			times.push_back(keyTimes[k]);
			if (k + 1 < keyTimes.size())
				for (double f : {0.25, 0.5, 0.75}) times.push_back(keyTimes[k] + f*(keyTimes[k + 1] - keyTimes[k]));
		}
		std::sort(times.begin(), times.end());
		times.erase(std::unique(times.begin(), times.end()), times.end());
		{
			Matrix4 m0;
			if (!inputs->Pose(*c, 0.0, m0, error)) return nullptr;
			double a[9], tr[3], r[9];
			Linear(m0, a, tr);
			if (!PolarRotation(a, r)) { error = "the transform of `" + name + "` is singular or mirrored"; return nullptr; }
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
				c->stretch0[3*i + j] = r[i]*a[j] + r[3 + i]*a[3 + j] + r[6 + i]*a[6 + j];
		}
		V::Values motion;
		for (double t : times) {
			Matrix4 m;
			RigidTransform ignored;
			if (!inputs->Pose(*c, t, m, error) || !inputs->Rigid(*c, t, ignored, error)) return nullptr;
			motion.push_back(V::ArrayValue({V::Float(t), MatrixValue(m)}));
		}
		Bytes identity;
		if (!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.scene_input.object.v1"), geometryValue,
			V::ArrayValue(motion)}), identity, &error)) return nullptr;
		c->identity = RISECBOR64::SHA256Hex(identity);
		inputs->items.push_back(std::move(c));
	}
	return inputs;
}

} }
