//////////////////////////////////////////////////////////////////////
//
//  CstRenderEquivalence.h - the render-equivalence harness (the pre-P0
//  regression oracle for the agentic-redesign migration).
//
//  The migration changes the parse -> Job mapping (legacy AsciiSceneParser ->
//  Job, vs the new CST -> derive -> Job). The same Job renders the same image,
//  so the deterministic, precise oracle for "did the CST path produce the same
//  scene?" is STRUCTURAL Job equivalence: parse via each path, dump the Job to a
//  canonical string, and compare. (Image equivalence adds RNG noise and catches
//  nothing the migration changes beyond what the Job already determines.)
//
//  This header is the reusable primitive: `ParseLegacy(text, job)` drives the
//  real legacy parser; `DumpJob(job)` is the canonical equivalence metric. The
//  oracle test (CstRenderEquivalenceTest) proves the metric is STABLE (a legacy
//  parse is deterministic) before any CST node exists; the in-tree CST slices
//  will compare `DumpJob(cstJob) == DumpJob(legacyJob)` against it.
//
//////////////////////////////////////////////////////////////////////
#ifndef RISE_TESTS_CST_RENDER_EQUIVALENCE_H
#define RISE_TESTS_CST_RENDER_EQUIVALENCE_H

#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IPhaseFunction.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Utilities/BoundingBox.h"

namespace risequiv {

using namespace RISE;

// Collects item names from a manager's EnumerateItemNames callback.
struct NameCollector : public IEnumCallback<const char*> {
	std::vector<std::string> names;
	bool operator()( const char* const& n ) override { names.push_back( n ? n : "" ); return true; }
};
template <typename Mgr>
inline std::vector<std::string> SortedNames( Mgr* m )
{
	NameCollector c;
	if( m ) m->EnumerateItemNames( c );
	std::sort( c.names.begin(), c.names.end() );
	return c.names;
}

// Reverse-lookup an item's registered name in its manager (managers map
// name->item; GetItem borrows -- no addref, see GenericManager::GetItem). Used
// to dump an object's REFERENCE WIRING (which geometry / material it points at)
// as discriminating state: a CST derive that binds the wrong reference shows up
// as a changed name here.
template <typename Mgr, typename Item>
inline std::string ReverseName( Mgr* m, const Item* ptr )
{
	if( !ptr ) return "(none)";
	if( !m )   return "(unknown)";
	NameCollector c; m->EnumerateItemNames( c );
	for( const auto& n : c.names )
		if( static_cast<const Item*>( m->GetItem( n.c_str() ) ) == ptr ) return n;
	return "(unknown)";
}

// Parse a scene string via the LEGACY AsciiSceneParser into `job`. Returns
// ParseAndLoadScene's result. Hermetic: writes a temp file, parses, removes it.
inline bool ParseLegacy( const std::string& sceneText, Job& job, const char* tmpPath = "cst_equiv_tmp.RISEscene" )
{
	{ std::ofstream f( tmpPath, std::ios::binary | std::ios::trunc ); f << sceneText; }
	ISceneParser* parser = 0;
	if( !RISE_API_CreateAsciiSceneParser( &parser, tmpPath ) || !parser ) { std::remove( tmpPath ); return false; }
	const bool ok = parser->ParseAndLoadScene( job );
	parser->release();
	std::remove( tmpPath );
	return ok;
}

// Dump a medium's discriminating, cheaply-readable state: coefficients sampled at the bbox CENTRE for a
// bounded/heterogeneous medium (so the sample is not vacuum -- homogeneous media ignore the point), the
// phase asymmetry g (GetMeanCosine), homogeneity, and the world bbox when bounded (placement). The spatial
// FIELD of a heterogeneous medium beyond that one centre sample is still by-construction (a render
// spot-check covers it); a single non-vacuum sample + bbox is far stronger than the prior origin-only one.
inline void DumpMedium( std::ostream& o, const IMedium* m )
{
	Point3 bbMin, bbMax; const bool bounded = m->GetBoundingBox( bbMin, bbMax );
	const Point3 sp = bounded ? Point3( (bbMin.x+bbMax.x)*0.5, (bbMin.y+bbMax.y)*0.5, (bbMin.z+bbMax.z)*0.5 ) : Point3(0,0,0);
	const MediumCoefficients c = m->GetCoefficients( sp ); const IPhaseFunction* pf = m->GetPhaseFunction();
	char b[480];
	std::snprintf( b, sizeof(b), " sigma_t=[%.17g %.17g %.17g] sigma_s=[%.17g %.17g %.17g] emission=[%.17g %.17g %.17g] g=%.17g homog=%d",
		(double)c.sigma_t.r,(double)c.sigma_t.g,(double)c.sigma_t.b, (double)c.sigma_s.r,(double)c.sigma_s.g,(double)c.sigma_s.b,
		(double)c.emission.r,(double)c.emission.g,(double)c.emission.b, (double)( pf ? pf->GetMeanCosine() : 0 ), m->IsHomogeneous()?1:0 );
	o << b;
	if( bounded ) {
		char bb[224]; std::snprintf( bb, sizeof(bb), " bbox=[%.17g %.17g %.17g .. %.17g %.17g %.17g]",
			(double)bbMin.x,(double)bbMin.y,(double)bbMin.z, (double)bbMax.x,(double)bbMax.y,(double)bbMax.z ); o << bb;
	}
}

// Reverse-lookup a medium pointer to its registered name (media use Job::GetMedium/EnumerateMediumNames,
// not a GenericManager, so this mirrors ReverseName for that surface). Used by global= and interior_medium.
inline std::string ReverseMediumName( Job& job, const IMedium* m )
{
	if( !m ) return "(none)";
	NameCollector c; job.EnumerateMediumNames( c ); std::sort( c.names.begin(), c.names.end() );
	for( const auto& n : c.names ) if( job.GetMedium( n.c_str() ) == m ) return n;
	return "(unnamed)";
}
// Dump a radiance map's cheaply-readable state: bound painter (reverse-named), scale, and the full 4x4
// transform (encodes radiance_orient). Shared by the scene global_rmap and per-object radiance_map.
inline void DumpRadianceMap( std::ostream& o, const IRadianceMap* rm, Job& job )
{
	o << "painter=" << ReverseName( job.GetPainters(), &rm->GetPainter() );
	const Matrix4& t = rm->GetTransform(); char b[640];
	std::snprintf( b, sizeof(b), " scale=%.17g xform=[%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g]",
		(double)rm->GetScale(), (double)t._00,(double)t._01,(double)t._02,(double)t._03, (double)t._10,(double)t._11,(double)t._12,(double)t._13,
		(double)t._20,(double)t._21,(double)t._22,(double)t._23, (double)t._30,(double)t._31,(double)t._32,(double)t._33 ); o << b;
}

// Canonical structural dump of a Job -- the equivalence metric. Two parse paths
// that yield the same dump produce the same scene (hence the same render). Sorted
// per manager for stability. Numeric fields use LOSSLESS %.17g (exactly
// round-trips an IEEE double), so two distinct radii can never collide into an
// equal dump.
//
// GATE-MAINTENANCE RULE (per the item-2 review): as each new DERIVED type lands,
// this dump MUST gain that type's discriminating state (reference bindings,
// parameter values, transforms, ordering) in the SAME change -- otherwise the
// oracle silently certifies different scenes as equal.
//
// Item 5 derives ALL registry chunk types through the SAME parser Finalize the
// legacy path uses, so a CST-vs-legacy Job can differ ONLY in (a) the param
// VALUES fed to Finalize or (b) the chunk set/order. This dump discriminates a
// value divergence WHEREVER the value reaches a dumped field -- geometry
// bounding-sphere radius; OBJECT reference wiring (geometry + material names) and
// world-space bounding box (encodes position / scale / geometry size) -- and the
// chunk set/order always. (A value that reaches no cheaply-readable interface field -- material IOR, camera
// intrinsics, the accelerator choice, the light-RR threshold -- is not surfaced here; those stay covered by
// the by-construction argument below + a Phase-B render spot-check (docs/agentic-redesign/61-...). The
// cheaply-readable, render-discriminating values ARE now surfaced DIRECTLY (caught here, not merely argued):
// DELTA-light (ILight) power/photons; AREA-light luminaire-material emitter exitance (IMaterial::GetEmitter
// ->averageRadiantExitance -- now DETERMINISTIC since the RefreshAverages stratified-grid fix; it formerly
// sampled the painter via GlobalRNG at construction and falsely flagged the gltf-import scenes); medium
// coefficients + phase-g + placement; per-object radiance-map (painter/scale/transform) + interior-medium
// name; the scene global radiance map + film dims. What STAYS by-construction (no cheaply + DETERMINISTICALLY
// readable Job field -> the render spot-check): material IOR, camera intrinsics, the accelerator, the RR
// threshold, rasterizer flags (radiance_background), a heterogeneous medium's spatial field beyond the
// bbox-centre sample. A Hosek/procedural SKY global-rmap is a partial case: its
// painter/scale/transform ARE dumped, but the painter is an internal unregistered adapter (reverse-names to
// (unknown)) and its dome params (solar elevation/azimuth, turbidity, ground albedo) need an eval context, so only those
// dome params stay by-construction. Painter colour and
// material scalar state are identical BY CONSTRUCTION (same Finalize) once the
// param values match -- and the multi-token value path that feeds them is
// covered END-TO-END here: an object's `position`/`scale` are multi-token
// DoubleVec3 values, so a multi-token mis-capture moves the world bbox and fails
// the CST-vs-legacy comparison (CstDescriptorBindTest [equiv]); its [multitoken]
// ParamValue assertions additionally pin the CST-side capture. Colour shares
// that one capture mechanism, so dumping a painter's colour (which would require
// constructing a RayIntersectionGeometric for GetColor) buys nothing over the
// position/bbox check. Materials/painters therefore stay names-only here.
inline std::string DumpJob( Job& job )
{
	std::ostringstream o;
	o << "geometries:\n";
	for( const auto& n : SortedNames( job.GetGeometries() ) ) {
		o << "  " << n;
		IGeometry* g = job.GetGeometries() ? job.GetGeometries()->GetItem( n.c_str() ) : 0;
		if( g ) { Point3 c; Scalar r = 0; g->GenerateBoundingSphere( c, r ); char b[64]; std::snprintf( b, sizeof(b), " bsphere=%.17g", (double)r ); o << b; }
		o << "\n";
	}
	o << "materials:\n";
	for( const auto& n : SortedNames( job.GetMaterials() ) ) {
		o << "  " << n; IMaterial* mat = job.GetMaterials() ? job.GetMaterials()->GetItem( n.c_str() ) : 0;
		if( mat && mat->GetEmitter() ) { const RISEPel e = mat->GetEmitter()->averageRadiantExitance(); char eb[96]; std::snprintf( eb, sizeof(eb), " emitter=[%.17g %.17g %.17g]", (double)e.r,(double)e.g,(double)e.b ); o << eb; }
		o << "\n";
	}
	o << "painters:\n";  for( const auto& n : SortedNames( job.GetPainters()  ) ) o << "  " << n << "\n";
	o << "objects:\n";
	for( const auto& n : SortedNames( job.GetObjects() ) ) {
		o << "  " << n;
		IObject* ob = job.GetObjects() ? job.GetObjects()->GetItem( n.c_str() ) : 0;
		if( ob ) {
			o << " geometry=" << ReverseName( job.GetGeometries(), ob->GetGeometry() );
			o << " material=" << ReverseName( job.GetMaterials(),  ob->GetMaterial() );
			o << " modifier=" << ReverseName( job.GetModifiers(), ob->GetModifier() );
			o << " shader=" << ReverseName( job.GetShaders(), ob->GetShader() );
			o << " radiance_map="; if( ob->GetRadianceMap() ) DumpRadianceMap( o, ob->GetRadianceMap(), job ); else o << "(none)";
			o << " interior_medium=" << ReverseMediumName( job, ob->GetInteriorMedium() );
			o << " visible=" << ( ob->IsWorldVisible() ? "1" : "0" );
			BoundingBox bb = ob->getBoundingBox();
			char b[160];
			std::snprintf( b, sizeof(b), " bbox=[%.17g %.17g %.17g .. %.17g %.17g %.17g]",
				(double)bb.ll.x, (double)bb.ll.y, (double)bb.ll.z,
				(double)bb.ur.x, (double)bb.ur.y, (double)bb.ur.z );
			o << b;
		}
		o << "\n";
	}
	// Cameras: name + world location. Names surface the camera-name dedup state
	// (an unnamed camera auto-names default / default_1 / ...; a cross-parse leak
	// of that state renames it), and location surfaces extrinsic/unit leaks --
	// the cross-derive parse-state vectors DumpJob would otherwise miss (it has
	// no camera section). Intrinsics (sensor/focal/fstop) are not on the ICamera
	// contract, so a pure-FOV leak is not surfaced here; the derive clears ALL
	// parser state regardless (ClearChunkParserState). So the cross-derive
	// leak vectors with a Job-observable manifestation -- painter-colour
	// (spurious energy-auto-scaled painters) AND camera-name dedup (an
	// unnamed camera renamed default -> default_1) -- are BOTH surfaced here
	// (what CstDeriveDifferentialTest's cross-derive cases assert).
	o << "cameras:\n";
	for( const auto& n : SortedNames( job.GetCameras() ) ) {
		o << "  " << n;
		ICamera* c = job.GetCameras() ? job.GetCameras()->GetItem( n.c_str() ) : 0;
		if( c ) { Point3 p = c->GetLocation(); char b[96]; std::snprintf( b, sizeof(b), " loc=[%.17g %.17g %.17g]", (double)p.x, (double)p.y, (double)p.z ); o << b; }
		o << "\n";
	}
	// --- scene singletons reachable via job.GetScene() (audit-by-bug-pattern: the global-medium sibling set).
	o << "scene:\n";
	{
		IScenePriv* scp = job.GetScene(); const IFilm* fl = scp ? scp->GetFilm() : 0; char fb[96];
		if( fl ) { std::snprintf( fb, sizeof(fb), "  film=[%u x %u par=%.17g]", fl->GetWidth(), fl->GetHeight(), (double)fl->GetPixelAR() ); o << fb << "\n"; } else o << "  film=(none)\n";
		const IRadianceMap* grm = scp ? scp->GetGlobalRadianceMap() : 0;
		o << "  global_rmap="; if( grm ) DumpRadianceMap( o, grm, job ); else o << "(none)"; o << "\n";
		o << "  active_camera=" << job.GetActiveCameraName() << "\n";   // last-add-wins; catches a camera-ORDER divergence the sorted cameras: section cannot
	}
	// --- lights + media (Phase B / 0b: close the F1 verification gap for the CHEAPLY-READABLE blind values).
	// Lights have no manager names, so dump a value-tuple per light and SORT for a stable canonical order; a
	// CST divergence in any light value (power/colour/position/cone) reorders or changes a row. Media dump the
	// global medium identity + per-medium state via DumpMedium (coefficients at the bbox centre, phase-g, placement).
	o << "lights:\n";
	{
		std::vector<std::string> rows;
		ILightManager* lm = job.GetLights();
		if( lm ) {
			const ILightManager::LightsList& ls = lm->getLights();
			for( const ILightPriv* lp : ls ) {
				const ILight* l = lp; if( !l ) continue;
				const RISEPel col = l->emissionColor(); const RISEPel rx = l->radiantExitance();
				const Point3 p = l->position(), tg = l->emissionTarget(); const Vector3 d = l->emissionDirection();
				char b[768];
				std::snprintf( b, sizeof(b),
					"  type=%d energy=%.17g col=[%.17g %.17g %.17g] exitance=[%.17g %.17g %.17g] pos=[%.17g %.17g %.17g] dir=[%.17g %.17g %.17g] cone=%.17g inner=%.17g outer=%.17g target=[%.17g %.17g %.17g] photons=%d",
					(int)l->lightType(), (double)l->emissionEnergy(),
					(double)col.r,(double)col.g,(double)col.b, (double)rx.r,(double)rx.g,(double)rx.b,
					(double)p.x,(double)p.y,(double)p.z, (double)d.x,(double)d.y,(double)d.z,
					(double)l->emissionConeHalfAngle(), (double)l->emissionInnerAngle(), (double)l->emissionOuterAngle(),
					(double)tg.x,(double)tg.y,(double)tg.z, (int)l->CanGeneratePhotons() );
				rows.push_back( b );
			}
		}
		std::sort( rows.begin(), rows.end() );
		for( const auto& r : rows ) o << r << "\n";
	}
	o << "media:\n";
	{
		IScenePriv* sc = job.GetScene(); const IMedium* gm = sc ? sc->GetGlobalMedium() : 0;
		o << "  global=" << ReverseMediumName( job, gm ); if( gm ) DumpMedium( o, gm ); o << "\n";
		NameCollector mns; job.EnumerateMediumNames( mns ); std::sort( mns.names.begin(), mns.names.end() );
		for( const auto& n : mns.names ) {
			o << "  " << n; const IMedium* m = job.GetMedium( n.c_str() ); if( m ) DumpMedium( o, m ); o << "\n";
		}
	}
	return o.str();
}

} // namespace risequiv

#endif // RISE_TESTS_CST_RENDER_EQUIVALENCE_H
