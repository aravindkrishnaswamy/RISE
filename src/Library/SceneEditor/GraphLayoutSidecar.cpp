//////////////////////////////////////////////////////////////////////
//
//  GraphLayoutSidecar.cpp - see GraphLayoutSidecar.h for format,
//    reuse-verdict, and atomic-write rationale.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GraphLayoutSidecar.h"

#include "../Agent/Json.h"
#include "../Interfaces/ILog.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#include <fcntl.h>     // open/fsync hardening, POSIX only -- same tmp+rename
                       // shape as SaveEngine.cpp, with looser durability
                       // guarantees appropriate for disposable layout state
                       // (see AtomicWriteSidecar below)
#endif

namespace RISE
{

namespace {

//! Whole-file read into a std::string. Returns false (leaves `out`
//! untouched) when the file cannot be opened -- caller decides what
//! "absent" vs "unreadable" means at its own call site.
bool ReadFileBytes( const std::string& path, std::string& out )
{
	std::ifstream ifs( path, std::ios::in | std::ios::binary );
	if( !ifs.is_open() ) return false;
	std::ostringstream ss;
	ss << ifs.rdbuf();
	out = ss.str();
	return true;
}

//! tmp+rename atomic write, following the same shape as SaveEngine.cpp's
//! own AtomicWrite (see GraphLayoutSidecar.h's header for why this is a
//! parallel copy rather than a shared call) but with two deliberate
//! durability relaxations documented at each site below -- appropriate
//! for this file's disposable layout state, not a copy/paste gap. The
//! temp file is always removed before this function returns, on every
//! path -- success (via the rename that consumes it) or failure
//! (explicit remove) -- so it never lingers.
bool AtomicWriteSidecar( const std::string& path, const std::string& text, std::string& outError )
{
	static std::atomic<unsigned long long> nextTmpId{ 1 };
#if defined(_WIN32)
	const long long pidValue = static_cast<long long>( ::_getpid() );
#else
	const long long pidValue = static_cast<long long>( ::getpid() );
#endif
	const unsigned long long tmpId = nextTmpId.fetch_add( 1, std::memory_order_relaxed );
	const std::string tmpPath = path + ".tmp." + std::to_string( pidValue ) + "." + std::to_string( tmpId );

	{
		std::ofstream ofs( tmpPath, std::ios::out | std::ios::binary | std::ios::trunc );
		if( !ofs.is_open() ) {
			outError = "GraphLayoutSidecar: could not open temp file for write: " + tmpPath;
			return false;
		}
		ofs.write( text.data(), static_cast<std::streamsize>( text.size() ) );
		if( !ofs.good() ) {
			outError = "GraphLayoutSidecar: write to temp file failed: " + tmpPath;
			ofs.close();
			std::error_code rmEc;
			std::filesystem::remove( tmpPath, rmEc );
			return false;
		}
		ofs.flush();
		ofs.close();
#if !defined(_WIN32)
		// RELAXATION 1 of 2: fsync hardening, POSIX-only, same intent as
		// SaveEngine.cpp's own AtomicWrite -- makes the bytes durable
		// before the rename that publishes them -- but strictly
		// best-effort here: the return value is not checked, and failure
		// is not fatal, whereas SaveEngine.cpp's AtomicWrite fails the
		// whole save on a failed fsync. That stricter behaviour is right
		// for a scene save (data loss); it would be needless friction for
		// a sidecar whose worst-case failure mode is "this node re-lays-
		// out on next open" (see GraphLayoutSidecar.h's ATOMIC WRITE note).
		const int fd = ::open( tmpPath.c_str(), O_RDONLY );
		if( fd >= 0 ) {
			::fsync( fd );
			::close( fd );
		}
#endif
	}

	std::error_code renameEc;
	std::filesystem::rename( tmpPath, path, renameEc );
	if( renameEc ) {
		outError = "GraphLayoutSidecar: rename failed: " + renameEc.message();
		std::error_code rmEc;
		std::filesystem::remove( tmpPath, rmEc );
		return false;
	}
	// RELAXATION 2 of 2: unlike SaveEngine.cpp's own AtomicWrite, there is
	// deliberately no post-rename directory fsync here. SaveEngine.cpp
	// does one (best-effort) to harden the RENAME itself against power
	// loss; for disposable layout state, a lost rename in that narrow
	// crash window degrades the same way as any other missing sidecar --
	// full auto-layout on next open, never a wrong or corrupt position --
	// so the extra syscall was judged not worth paying on every write.
	return true;
}

//! Clamp one coordinate for ParsePositions' sanitization rule -- see
//! GraphLayoutSidecar.h's "Per-node coordinate sanitization" note.
double SanitizeCoordinate( double v, bool& wasSanitized )
{
	if( !std::isfinite( v ) ) { wasSanitized = true; return 0.0; }
	if( v > GraphLayoutSidecar::kAbsurdCoordinateBound )  { wasSanitized = true; return  GraphLayoutSidecar::kAbsurdCoordinateBound; }
	if( v < -GraphLayoutSidecar::kAbsurdCoordinateBound ) { wasSanitized = true; return -GraphLayoutSidecar::kAbsurdCoordinateBound; }
	return v;
}

} // anonymous namespace

std::string GraphLayoutSidecar::SidecarPathForScene( const std::string& scenePath )
{
	if( scenePath.empty() ) return std::string();
	return scenePath + ".risegraph.json";
}

bool GraphLayoutSidecar::ParsePositions( const std::string& text, GraphLayout::Positions& out,
                                          unsigned int* outSanitizedCount )
{
	out.clear();
	if( outSanitizedCount ) *outSanitizedCount = 0;

	Agent::JsonValue root;
	std::string parseError;
	if( !Agent::JsonParse( text, root, parseError ) ) return false;
	if( !root.isObject() ) return false;   // top-level must be an object, per the format's own header

	const Agent::JsonValue* nodesVal = root.find( "nodes" );
	if( !nodesVal ) return true;           // {"version":1} with no nodes yet: well-formed, empty -- not malformed
	if( !nodesVal->isObject() ) return false;

	unsigned int sanitized = 0;
	for( const std::pair<std::string, Agent::JsonValue>& member : nodesVal->members() ) {
		const std::string& name = member.first;
		const Agent::JsonValue& entry = member.second;
		if( name.empty() ) continue;       // an empty name can never be a graph node's identity (see GraphLayout.h)
		if( !entry.isObject() ) continue;  // one malformed entry costs only itself, not the whole file
		const bool hasX = entry.has( "x" );
		const bool hasY = entry.has( "y" );
		if( !hasX && !hasY ) continue;     // neither coordinate present: nothing to recover for this entry

		GraphLayoutPoint p;
		bool sanitizedThisEntry = false;

		// A present-but-wrong-typed value (e.g. "x":"garbage") is NOT the
		// same as an absent one: JsonValue::asNumber(0.0) would silently
		// return the default for either, which would let a corrupted/
		// hand-edited file's type error masquerade as a legitimate 0.0 --
		// no count, no log. Treat it exactly like a non-finite value: the
		// entry still gets a position (0.0, never dropped), but it counts
		// toward *outSanitizedCount and therefore the same ReadSidecar
		// warning that non-finite/absurd coordinates trigger.
		if( hasX ) {
			const Agent::JsonValue& xv = entry.get( "x" );
			if( xv.isNumber() ) p.x = xv.asNumber( 0.0 );
			else { p.x = 0.0; sanitizedThisEntry = true; }
		} else {
			p.x = 0.0;
		}
		if( hasY ) {
			const Agent::JsonValue& yv = entry.get( "y" );
			if( yv.isNumber() ) p.y = yv.asNumber( 0.0 );
			else { p.y = 0.0; sanitizedThisEntry = true; }
		} else {
			p.y = 0.0;
		}

		p.x = SanitizeCoordinate( p.x, sanitizedThisEntry );
		p.y = SanitizeCoordinate( p.y, sanitizedThisEntry );
		if( sanitizedThisEntry ) ++sanitized;

		out[ name ] = p;
	}

	if( outSanitizedCount ) *outSanitizedCount = sanitized;
	return true;
}

std::string GraphLayoutSidecar::SerializePositions( const GraphLayout::Positions& positions )
{
	Agent::JsonValue root = Agent::JsonValue::MakeObject();
	root.set( "version", Agent::JsonValue::MakeNumber( 1.0 ) );

	Agent::JsonValue nodes = Agent::JsonValue::MakeObject();
	// `positions` is a std::map<std::string,...>: iteration is already
	// lexical-by-name order, so this loop alone is what makes
	// SerializePositions deterministic/byte-stable for equal input --
	// no separate sort needed.
	for( const std::pair<const std::string, GraphLayoutPoint>& kv : positions ) {
		Agent::JsonValue node = Agent::JsonValue::MakeObject();
		node.set( "x", Agent::JsonValue::MakeNumber( kv.second.x ) );
		node.set( "y", Agent::JsonValue::MakeNumber( kv.second.y ) );
		nodes.set( kv.first, node );
	}
	root.set( "nodes", nodes );

	return Agent::JsonSerialize( root );
}

GraphLayout::Positions GraphLayoutSidecar::ReadSidecar( const std::string& scenePath )
{
	GraphLayout::Positions out;
	const std::string path = SidecarPathForScene( scenePath );
	if( path.empty() ) return out;   // unsaved scene: no sidecar path at all

	std::error_code existsEc;
	if( !std::filesystem::exists( path, existsEc ) || existsEc ) return out;   // absent: ordinary, no log

	std::string text;
	if( !ReadFileBytes( path, text ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"GraphLayoutSidecar::ReadSidecar: could not open '%s' (exists but unreadable) -- ignoring, full auto-layout.",
			path.c_str() );
		return out;
	}

	unsigned int sanitizedCount = 0;
	if( !ParsePositions( text, out, &sanitizedCount ) ) {
		out.clear();
		GlobalLog()->PrintEx( eLog_Warning,
			"GraphLayoutSidecar::ReadSidecar: malformed sidecar '%s' -- ignoring, full auto-layout.",
			path.c_str() );
		return out;
	}
	if( sanitizedCount > 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"GraphLayoutSidecar::ReadSidecar: '%s' had %u non-finite/absurd coordinate(s), clamped.",
			path.c_str(), sanitizedCount );
	}
	return out;
}

bool GraphLayoutSidecar::WriteSidecar( const std::string& scenePath, const GraphLayout::Positions& positions,
                                        const std::set<std::string>& liveNodeNames, std::string& outError )
{
	outError.clear();
	if( scenePath.empty() ) return true;   // in-memory-only scene: nothing to write, not an error

	const std::string path = SidecarPathForScene( scenePath );
	if( path.empty() ) return true;        // defensive mirror of the above

	// ORPHAN-DROP: keep only entries whose node still exists in the
	// CURRENT graph -- see WriteSidecar's own header note.
	GraphLayout::Positions pruned;
	for( const std::pair<const std::string, GraphLayoutPoint>& kv : positions )
		if( liveNodeNames.find( kv.first ) != liveNodeNames.end() ) pruned.insert( kv );

	const std::string newText = SerializePositions( pruned );

	std::error_code existsEc;
	const bool exists = std::filesystem::exists( path, existsEc ) && !existsEc;
	if( !exists && pruned.empty() ) return true;   // nothing to create -- avoid useless-file churn

	if( exists ) {
		std::string existingText;
		if( ReadFileBytes( path, existingText ) && existingText == newText ) return true;   // unchanged: skip
	}

	return AtomicWriteSidecar( path, newText, outError );
}

bool GraphLayoutSidecar::MigrateSidecarOnSaveAs( const std::string& oldScenePath, const std::string& newScenePath,
                                                  std::string& outError )
{
	outError.clear();
	if( oldScenePath.empty() || newScenePath.empty() || oldScenePath == newScenePath )
		return true;   // nothing to migrate: unsaved-before, or an ordinary (non-Save-As) save

	const std::string oldPath = SidecarPathForScene( oldScenePath );
	const std::string newPath = SidecarPathForScene( newScenePath );
	if( oldPath.empty() || newPath.empty() ) return true;   // defensive mirror of the above

	std::error_code existsEc;
	if( !std::filesystem::exists( oldPath, existsEc ) || existsEc )
		return true;   // no old sidecar to migrate: ordinary "this scene never had one", not a failure

	if( std::filesystem::exists( newPath, existsEc ) && !existsEc )
		return true;   // NEVER overwrite an existing new-path sidecar -- see header

	std::error_code copyEc;
	std::filesystem::copy_file( oldPath, newPath, std::filesystem::copy_options::none, copyEc );
	if( copyEc ) {
		outError = "GraphLayoutSidecar::MigrateSidecarOnSaveAs: copy '" + oldPath + "' -> '" + newPath
			+ "' failed: " + copyEc.message();
		return false;
	}
	return true;
}

bool GraphLayoutSidecar::MigrateName( GraphLayout::Positions& positions,
                                       const std::string& oldName, const std::string& newName )
{
	if( oldName == newName ) return false;   // no-op rename (or degenerate call): nothing to migrate
	const GraphLayout::Positions::iterator oldIt = positions.find( oldName );
	if( oldIt == positions.end() ) return false;             // nothing saved under the old name
	if( positions.find( newName ) != positions.end() ) return false;   // refuse to clobber an existing entry -- see header

	positions[ newName ] = oldIt->second;
	positions.erase( oldIt );
	return true;
}

} // namespace RISE
