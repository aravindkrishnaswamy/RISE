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
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IEnumCallback.h"

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
// per manager for stability; geometry carries its bounding-sphere radius so a
// changed sphere is visible. (Richer per-type detail can be added as the kernel
// slices need it; this is the seam.)
inline std::string DumpJob( Job& job )
{
	std::ostringstream o;
	o << "geometries:\n";
	for( const auto& n : SortedNames( job.GetGeometries() ) ) {
		o << "  " << n;
		IGeometry* g = job.GetGeometries() ? job.GetGeometries()->GetItem( n.c_str() ) : 0;
		if( g ) { Point3 c; Scalar r = 0; g->GenerateBoundingSphere( c, r ); char b[64]; std::snprintf( b, sizeof(b), " bsphere=%.6g", (double)r ); o << b; }
		o << "\n";
	}
	o << "materials:\n"; for( const auto& n : SortedNames( job.GetMaterials() ) ) o << "  " << n << "\n";
	o << "painters:\n";  for( const auto& n : SortedNames( job.GetPainters()  ) ) o << "  " << n << "\n";
	o << "objects:\n";   for( const auto& n : SortedNames( job.GetObjects()   ) ) o << "  " << n << "\n";
	return o.str();
}

} // namespace risequiv

#endif // RISE_TESTS_CST_RENDER_EQUIVALENCE_H
