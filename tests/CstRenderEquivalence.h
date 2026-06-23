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
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
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
// chunk set/order always. (A value that reaches no dumped field, e.g. a material
// IOR or a light power, is not surfaced here; it is covered by the
// by-construction argument below.) Painter colour and
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
	o << "materials:\n"; for( const auto& n : SortedNames( job.GetMaterials() ) ) o << "  " << n << "\n";
	o << "painters:\n";  for( const auto& n : SortedNames( job.GetPainters()  ) ) o << "  " << n << "\n";
	o << "objects:\n";
	for( const auto& n : SortedNames( job.GetObjects() ) ) {
		o << "  " << n;
		IObject* ob = job.GetObjects() ? job.GetObjects()->GetItem( n.c_str() ) : 0;
		if( ob ) {
			o << " geometry=" << ReverseName( job.GetGeometries(), ob->GetGeometry() );
			o << " material=" << ReverseName( job.GetMaterials(),  ob->GetMaterial() );
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
	return o.str();
}

} // namespace risequiv

#endif // RISE_TESTS_CST_RENDER_EQUIVALENCE_H
