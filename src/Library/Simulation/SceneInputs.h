// Simulation M4: scene inputs (ISceneInputs) for a loaded scene.  A
// simulation names scene objects it reads (cloth: `collider_object`,
// `follow_object`); this captures each one detached from the live scene, so
// a solver samples it from its own threads while the viewport renders,
// scrubs or edits the scene.
//
// Definitions (docs/SIMULATION_SOLVER_CONTRACT.md section 3):
//   * The object's world matrix M(t) at any time t comes from pose holders
//     (Object::ClonePoseHolder) of the object and each of its ancestors,
//     captured at Capture, posed per call through the animator's read-only
//     IAnimator::EvaluateElementAtTimeInto, and composed parent first.  The
//     shared scene objects are never written. The object and every ancestor
//     must be exact built-in Objects; CSG and custom subclasses are refused
//     before cloning because nonvirtual pose holders would slice them. CSG
//     operands are refused too: their pose is composite-local and may have
//     multiple world placements.
//   * SampleTransform(name, t) is the rigid part of M(t): R from the polar
//     decomposition of its linear part A = R S, translation as is.  A
//     stretch S (scale) that changes over time is refused.
//   * Sample(name, t) is mesh.v1, WORLD space: the object's geometry,
//     tessellated in object space at Capture (ClothColliderTessellationDetail;
//     mesh geometries as they are), transformed by M(t).  For a
//     simulated_mesh_geometry it is the cache frame its binding shows at t.
//   * Identity(name) = SHA-256 of canonical CBOR
//       ["rise.simulation.scene_input.object.v1", geometry, motion]
//     geometry: ["mesh", detail, SHA-256 of the object-space mesh.v1 payload]
//            or ["simulation", the producer's expected cache key, output,
//                time_offset, time_scale, end policy]   (bound with `source`)
//            or ["cache", the cache's key, its generation digest]  (`cache` path)
//     motion:   [[t, M(t) as 16 float64]...] at t = 0, at every keyframe time
//               of the object and its ancestors in the active animation, and
//               at a quarter, a half and three quarters between consecutive
//               keyframe times (so a changed interpolator changes it).
//     Material, shader, name and everything else about the object are not
//     part of it: an edit to them leaves the simulation current.
#ifndef RISE_SIMULATION_SCENE_INPUTS_H
#define RISE_SIMULATION_SCENE_INPUTS_H

#include "SceneSimulation.h"
#include "SimulatedMeshSource.h"
#include <functional>
#include <map>
#include <set>

namespace RISE {
class IScene;
class IGeometry;
namespace Simulation {

//! How a simulated_mesh_geometry is bound: to a producer by `source`, or to
//! a cache path.
struct SimulatedGeometryBinding {
	std::string simulation;               // empty: bound by a cache path
	std::string output;
	std::filesystem::path cacheDir;
	MeshSourceOptions options;
};
//! The binding of a geometry, or false when it is not a simulated mesh.
using SimulatedGeometryLookup = std::function<bool(const IGeometry*, SimulatedGeometryBinding&)>;

class SceneObjectInputProvider final : public ISceneInputProvider {
public:
	//! `scene` must stay valid and stable for each call (the caller holds
	//! the scene: the editor's mutex, or a private worker scene).
	SceneObjectInputProvider(const IScene& scene, SimulatedGeometryLookup lookup);
	bool InputDependencies(const std::string& input, std::vector<std::string>& simulations,
		std::string& error) const override;
	std::shared_ptr<const ISceneInputs> Capture(const std::vector<std::string>& inputs,
		const std::map<std::string, std::string>& upstreamKeys, std::string& error) const override;
	//! M6: objects whose transform a simulation drives (`transform_source`):
	//! a scene input reads them at their authored pose (their simulation
	//! keyframes neither move them nor enter their identity -- a body must not
	//! depend on its own output).
	//! Each is given with its authored `position` and `orientation` (keyframe
	//! value text): the capture poses its holder there, so neither a scrub, a
	//! live run nor derived simulation keys (which move the shared object)
	//! change what a simulation reads, while an edit of the authored pose does.
	struct AuthoredPose { std::string position, orientation; };
	void SetSimulationDriven(std::map<std::string, AuthoredPose> objects) { driven_ = std::move(objects); }
private:
	const IScene& scene_;
	SimulatedGeometryLookup lookup_;
	std::map<std::string, AuthoredPose> driven_;
};

//! Polar decomposition of a 3x3 linear part (column-vector convention,
//! row-major `a`): a = r s with r proper orthonormal.  False for a singular
//! or reflecting matrix.
bool PolarRotation(const double a[9], double r[9]);

} }
#endif
