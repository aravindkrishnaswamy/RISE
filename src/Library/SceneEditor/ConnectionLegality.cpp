//////////////////////////////////////////////////////////////////////
//
//  ConnectionLegality.cpp - See ConnectionLegality.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ConnectionLegality.h"
#include "ChunkDescriptorRegistry.h"
#include "ReferenceGraph.h"

#include <cstdarg>
#include <cstdio>
#include <map>
#include <queue>
#include <set>
#include <vector>

namespace RISE
{
	namespace
	{
		//! sprintf into a std::string. Small helper so the verbatim-message
		//! constants below read as close to their Job.cpp / ChunkParserRegistry.cpp
		//! originals as possible.
		std::string Fmt( const char* fmt, ... )
		{
			char buf[1024];
			va_list ap;
			va_start( ap, fmt );
			vsnprintf( buf, sizeof( buf ), fmt, ap );
			va_end( ap );
			return std::string( buf );
		}

		// ---- verbatim-reused parser diagnostics -----------------------------
		//
		// kUndeclaredParameterFmt / kScalarBoundToPerChannelFmt /
		// kScalarBoundToIPainterFmt / kScalarUnknownFmt are declared in
		// ChunkDescriptor.h ("shared parser diagnostic strings") and are
		// the SAME `inline constexpr` symbols ChunkParserRegistry.cpp
		// (DispatchChunkParameters) and Job.cpp (ResolveOrDiagnoseScalar
		// branches a/b/c) format from -- not copies.  Consuming the symbol
		// rather than the text makes the two ends structurally unable to
		// drift apart.

		const ParameterDescriptor* FindParam( const ChunkDescriptor& d, const std::string& name )
		{
			for( size_t i = 0; i < d.parameters.size(); ++i ) {
				if( d.parameters[i].name == name ) return &d.parameters[i];
			}
			return nullptr;
		}

		bool IsReferenceKind( const ParameterDescriptor& p )
		{
			if( p.kind == ValueKind::Reference ) return true;
			for( size_t i = 0; i < p.tupleKinds.size(); ++i ) {
				if( p.tupleKinds[i] == ValueKind::Reference ) return true;
			}
			return false;
		}

		bool CategoryAllowed( const ParameterDescriptor& p, ChunkCategory candidateCategory )
		{
			if( p.referenceCategories.empty() ) return true;   // undeclared -- don't refuse on category alone
			for( size_t i = 0; i < p.referenceCategories.size(); ++i ) {
				if( p.referenceCategories[i] == candidateCategory ) return true;
			}
			return false;
		}

		//! The candidate chunk's OWN primary pipe, inferred from its keyword +
		//! category -- see ChunkDescriptor.h's ParameterPipe doc comment for
		//! why Function2D is handled SEPARATELY (`IsFunction2DCapable`, below)
		//! rather than folded into this single-pipe classification: a Color
		//! chunk can ALSO be Function2D-capable at the same time (the dual-
		//! registration), which a one-pipe-per-chunk answer cannot express.
		ParameterPipe OwnPipe( const std::string& keyword, ChunkCategory category )
		{
			switch( category ) {
				case ChunkCategory::Material: return ParameterPipe::Material;
				case ChunkCategory::Function:
					if( keyword == "piecewise_linear_function" )   return ParameterPipe::Function1D;
					if( keyword == "piecewise_linear_function2d" ) return ParameterPipe::Function2D;
					return ParameterPipe::Other;
				case ChunkCategory::Painter:
					return ( keyword == "scalar_painter" ) ? ParameterPipe::Scalar : ParameterPipe::Color;
				case ChunkCategory::Geometry:
				case ChunkCategory::Object:
					return ParameterPipe::Geometry;
				// ChunkCategory::HairGuides is DELIBERATELY ABSENT from the
				// Geometry arm and falls through to `Other`.  A `hair_guides`
				// set is not an IGeometry and never enters the geometry
				// manager, so it must not answer "yes" to a Geometry-pipe
				// question -- that equivalence is precisely the reverse-
				// direction hole the category split closed.  Its own port
				// (`hair_geometry.guides`) declares `ParameterPipe::Other`
				// plus `referenceCategories = {HairGuides}`, so it is checked
				// by the fallback `CategoryAllowed` arm, which now separates
				// the two categories exactly.
				default:
					return ParameterPipe::Other;
			}
		}

		const char* PipeName( ParameterPipe p )
		{
			switch( p ) {
				case ParameterPipe::Unspecified: return "unspecified";
				case ParameterPipe::Color:       return "colour (IPainter)";
				case ParameterPipe::Scalar:      return "scalar (IScalarPainter)";
				case ParameterPipe::Material:    return "material";
				case ParameterPipe::Function1D:  return "function1d";
				case ParameterPipe::Function2D:  return "function2d";
				case ParameterPipe::Geometry:    return "geometry";
				case ParameterPipe::Other:       return "other";
			}
			return "other";
		}

		bool InAllowlist( const std::vector<std::string>& allow, const std::string& keyword )
		{
			for( size_t i = 0; i < allow.size(); ++i ) if( allow[i] == keyword ) return true;
			return false;
		}

		std::string JoinAllowlist( const std::vector<std::string>& allow )
		{
			std::string s;
			for( size_t i = 0; i < allow.size(); ++i ) { if( i ) s += " / "; s += allow[i]; }
			return s;
		}

		//! `targetKeyword` is used only for the diagnostic text (mirrors the
		//! `chunkKind` argument every Job.cpp resolver passes to its own
		//! diagnostics), so a caller without a live chunk NAME can still get a
		//! readable message.
		ConnectionVerdict CheckScalarPipe(
			const ParameterDescriptor& pd,
			const std::string& targetKeyword,
			const std::string& paramName,
			const std::string& candidateKeyword,
			ChunkCategory candidateCategory,
			bool candidateIsPerChannelValues )
		{
			if( candidateKeyword == "scalar_painter" ) {
				if( pd.semantics.requireSingle && candidateIsPerChannelValues ) {
					return { false, Fmt( kScalarBoundToPerChannelFmt, targetKeyword.c_str(), "<name>",
						paramName.c_str(), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
			}
			if( OwnPipe( candidateKeyword, candidateCategory ) == ParameterPipe::Color ) {
				return { false, Fmt( kScalarBoundToIPainterFmt, targetKeyword.c_str(), "<name>",
					paramName.c_str(), candidateKeyword.c_str() ) };
			}
			return { false, Fmt( kScalarUnknownFmt, targetKeyword.c_str(), "<name>",
				paramName.c_str(), candidateKeyword.c_str() ) };
		}

		ConnectionVerdict CheckColorPipe(
			const ParameterDescriptor& pd,
			const std::string& targetKeyword,
			const std::string& paramName,
			const std::string& candidateKeyword,
			ChunkCategory candidateCategory )
		{
			// `pd.semantics.keywordAllowlist` is now enforced up front in
			// CheckConnectionByKeyword, for EVERY pipe (not just Color) -- see
			// the comment there.  By the time control reaches here the
			// candidate has already cleared the allowlist gate, so there is
			// nothing left to check on that front.
			if( ConnectionLegality::IsColorCapable( candidateKeyword, candidateCategory ) ) {
				return { true, std::string() };
			}
			// NOTE: many real Color-pipe resolvers (e.g. Job::AddLambertianMaterial)
			// call `pPntManager->GetItem(value)` and simply `return false` on a miss --
			// NO GlobalLog message at all. This diagnostic is therefore NOT always a
			// verbatim parser quote; it exists so the canvas gives the author feedback
			// where the real pipeline would otherwise fail silently. See the header.
			if( candidateKeyword == "scalar_painter" ) {
				return { false, Fmt(
					"%s: parameter `%s` is bound to `scalar_painter` `%s`; this slot requires a colour "
					"painter (IPainter) -- the physical-scalar pipe is not interchangeable with the "
					"colour pipe.  See docs/ISCALARPAINTER_REFACTOR.md.",
					targetKeyword.c_str(), paramName.c_str(), candidateKeyword.c_str() ) };
			}
			return { false, Fmt( "%s: parameter `%s` value `%s` is not a registered colour painter",
				targetKeyword.c_str(), paramName.c_str(), candidateKeyword.c_str() ) };
		}
	}   // anonymous namespace

	bool ConnectionLegality::IsFunction2DCapable( const std::string& candidateKeyword, ChunkCategory candidateCategory )
	{
		// See Job.cpp's `RegisterPainterDual`: every colour painter dual-
		// registers into IFunction2DManager except `expression_painter`
		// (deliberately single-registered -- see its own Job.cpp comment) and
		// `scalar_painter` (a different manager altogether, IScalarPainterManager,
		// never touches IPainterManager or IFunction2DManager). Within
		// ChunkCategory::Function, only `piecewise_linear_function2d` is a
		// genuine 2D function -- `piecewise_linear_function` is 1D.
		if( candidateCategory == ChunkCategory::Function ) {
			return candidateKeyword == "piecewise_linear_function2d";
		}
		if( candidateCategory == ChunkCategory::Painter ) {
			return candidateKeyword != "expression_painter" && candidateKeyword != "scalar_painter";
		}
		return false;
	}

	bool ConnectionLegality::IsColorCapable( const std::string& candidateKeyword, ChunkCategory candidateCategory )
	{
		// `piecewise_linear_function` is a SECOND, easy-to-miss dual-
		// registration this slice's corpus sweep caught (ConnectionLegalityTest.cpp
		// PART 2): `Job::AddPiecewiseLinearFunction` registers it into BOTH
		// `pFunc1DManager` (its primary identity, `ParameterPipe::Function1D`)
		// AND `pPntManager` as a `Function1DSpectralPainter`
		// (`RegisterOrDiag( pPntManager, pPainter, name, "function" )`,
		// unconditional -- not gated on anything the scene author writes).
		// So it is a legal binding for ANY Color-pipe slot too, exactly like
		// a `uniformcolor_painter` name would be.
		if( candidateCategory == ChunkCategory::Painter )  return candidateKeyword != "scalar_painter";
		if( candidateCategory == ChunkCategory::Function ) return candidateKeyword == "piecewise_linear_function";
		return false;
	}

	ConnectionVerdict ConnectionLegality::CheckConnectionByKeyword(
		const std::string& targetKeyword,
		const std::string& paramName,
		const std::string& candidateKeyword,
		ChunkCategory candidateCategory,
		bool candidateIsPerChannelValues )
	{
		const ChunkDescriptor* targetDesc = DescriptorForKeyword( String( targetKeyword.c_str() ) );
		if( !targetDesc ) {
			return { false, Fmt( "unknown chunk type `%s`", targetKeyword.c_str() ) };
		}

		const ParameterDescriptor* pd = FindParam( *targetDesc, paramName );
		if( !pd ) {
			return { false, Fmt( kUndeclaredParameterFmt, paramName.c_str(), targetKeyword.c_str() ) };
		}
		if( !IsReferenceKind( *pd ) ) {
			return { false, Fmt(
				"%s: parameter `%s` is not a Reference-kind parameter -- nothing can be wired into it",
				targetKeyword.c_str(), paramName.c_str() ) };
		}

		// A `keywordAllowlist` is a per-PARAMETER special case (e.g.
		// `hair_geometry`'s `guides` field, which resolves against the Job's
		// `hair_guides` table rather than the geometry manager and so must
		// accept ONLY `hair_guides` chunks).  It is enforced HERE, ahead of
		// the per-pipe switch below, so it applies uniformly to every pipe --
		// not just Color, whose own resolver used to be the only one that
		// consulted it (a `ParameterPipe::Other` parameter like `guides`
		// fell through to the generic `CategoryAllowed` check and never saw
		// the allowlist at all).
		//
		// It constrains ONE direction only: what may wire INTO this
		// parameter.  The REVERSE direction -- this parameter's candidate
		// keyword wired into some OTHER chunk's slot -- is governed by
		// category, and used to be a hole while `hair_guides` shared
		// `ChunkCategory::Geometry` with real geometry chunks (a guide name
		// was then a legal candidate for `standard_object.geometry`).  That
		// hole is closed by `ChunkCategory::HairGuides`: a Geometry-typed
		// port declares `referenceCategories = {Geometry}`, and the
		// `CategoryAllowed` fallback below refuses a HairGuides candidate
		// outright.  See IJob::AddHairGuides for the whole arrangement.
		if( !pd->semantics.keywordAllowlist.empty() && !InAllowlist( pd->semantics.keywordAllowlist, candidateKeyword ) ) {
			return { false, Fmt(
				"%s: parameter `%s` only accepts one of {%s} (got `%s`) -- see the parameter's "
				"documented special case.",
				targetKeyword.c_str(), paramName.c_str(),
				JoinAllowlist( pd->semantics.keywordAllowlist ).c_str(), candidateKeyword.c_str() ) };
		}

		// Every TYPED pipe below does its OWN precise legality check
		// (IsColorCapable / IsFunction2DCapable / an exact keyword match),
		// each already accounting for the dual-registration facts that make
		// the legal candidate set BIGGER than `referenceCategories` alone
		// declares (ChunkDescriptor.h's ParameterPipe doc comment) -- so the
		// generic `CategoryAllowed` check is used ONLY as the fallback for
		// an Unspecified/Other (un-audited) pipe, never ahead of a typed one.
		switch( pd->semantics.pipe ) {
			case ParameterPipe::Scalar:
				return CheckScalarPipe( *pd, targetKeyword, paramName, candidateKeyword, candidateCategory,
					candidateIsPerChannelValues );
			case ParameterPipe::Color:
				return CheckColorPipe( *pd, targetKeyword, paramName, candidateKeyword, candidateCategory );
			case ParameterPipe::Function2D:
				if( !IsFunction2DCapable( candidateKeyword, candidateCategory ) ) {
					return { false, Fmt(
						"%s: parameter `%s` requires a chunk usable as an IFunction2D (a `function`-category "
						"chunk, or any colour painter except `expression_painter` / `scalar_painter`) -- "
						"`%s` is not (Job.cpp's RegisterPainterDual dual-registration rule).",
						targetKeyword.c_str(), paramName.c_str(), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
			case ParameterPipe::Material:
				if( OwnPipe( candidateKeyword, candidateCategory ) != ParameterPipe::Material ) {
					return { false, Fmt( "%s: parameter `%s` requires a %s chunk (got `%s`)",
						targetKeyword.c_str(), paramName.c_str(), PipeName( ParameterPipe::Material ), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
			case ParameterPipe::Function1D:
				if( candidateKeyword != "piecewise_linear_function" ) {
					return { false, Fmt(
						"%s: parameter `%s` requires a %s chunk (`piecewise_linear_function`; got `%s`)",
						targetKeyword.c_str(), paramName.c_str(), PipeName( ParameterPipe::Function1D ), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
			case ParameterPipe::Geometry:
				if( OwnPipe( candidateKeyword, candidateCategory ) != ParameterPipe::Geometry ) {
					return { false, Fmt( "%s: parameter `%s` requires a %s chunk (got `%s`)",
						targetKeyword.c_str(), paramName.c_str(), PipeName( ParameterPipe::Geometry ), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
			case ParameterPipe::Unspecified:
			case ParameterPipe::Other:
			default:
				// No pipe audit for this parameter (outside this slice's
				// Painter/Material scope, or a genuinely un-audited family) --
				// fall back to the plain declared-category check.
				if( !CategoryAllowed( *pd, candidateCategory ) ) {
					return { false, Fmt(
						"%s: parameter `%s` does not accept a chunk of this category (`%s`)",
						targetKeyword.c_str(), paramName.c_str(), candidateKeyword.c_str() ) };
				}
				return { true, std::string() };
		}
	}

	namespace
	{
		//! Resolve `id`'s chunk keyword + declared category via the document,
		//! or (false) when `id` doesn't resolve to a chunk with a registered
		//! descriptor.
		bool KeywordAndCategoryForId(
			const Cst::Document& doc, Cst::NodeId id, std::string& outKeyword, ChunkCategory& outCategory )
		{
			if( id == 0 ) return false;
			const Cst::NodeRef node = Cst::DocResolveNodeId( doc, id );
			if( !node || node->kind != Cst::NodeKind::Chunk ) return false;
			const ChunkDescriptor* d = DescriptorForKeyword( String( node->role.c_str() ) );
			if( !d ) return false;
			outKeyword  = node->role;
			outCategory = d->category;
			return true;
		}

		//! True iff `chunkNode` (a Chunk node) has an authored `values` param
		//! line -- the one statically-visible per-channel `scalar_painter`
		//! form (RGBScalarPainter). See ConnectionLegality.h's "what this does
		//! not do" note for the forms this deliberately does not chase.
		bool ChunkHasValuesParam( const Cst::NodeRef& chunkNode )
		{
			if( !chunkNode ) return false;
			for( size_t i = 0; i < chunkNode->kids.size(); ++i ) {
				const Cst::NodeRef& k = chunkNode->kids[i];
				if( k && k->kind == Cst::NodeKind::Param && k->role == "values" ) return true;
			}
			return false;
		}
	}

	ConnectionVerdict ConnectionLegality::CheckConnection(
		const Cst::Document& doc, Cst::NodeId targetChunk, const String& paramName, Cst::NodeId candidateChunk )
	{
		std::string targetKeyword, candidateKeyword;
		ChunkCategory targetCategory = ChunkCategory::Painter, candidateCategory = ChunkCategory::Painter;

		if( !KeywordAndCategoryForId( doc, targetChunk, targetKeyword, targetCategory ) ) {
			return { false, "target chunk does not resolve to a known descriptor" };
		}
		if( !KeywordAndCategoryForId( doc, candidateChunk, candidateKeyword, candidateCategory ) ) {
			return { false, "candidate chunk does not resolve to a known descriptor" };
		}

		bool candidateIsValues = false;
		if( candidateKeyword == "scalar_painter" ) {
			candidateIsValues = ChunkHasValuesParam( Cst::DocResolveNodeId( doc, candidateChunk ) );
		}

		return CheckConnectionByKeyword(
			targetKeyword, std::string( paramName.c_str() ), candidateKeyword, candidateCategory, candidateIsValues );
	}

	ConnectionVerdict ConnectionLegality::CheckConnectionByName(
		const Cst::Document& doc,
		ChunkCategory targetCategory, const String& targetName,
		const String& paramName,
		ChunkCategory candidateCategory, const String& candidateName )
	{
		const Cst::NodeId targetId = SceneReferenceGraph::ResolveChunk( doc, targetCategory, targetName );
		if( targetId == 0 ) {
			return { false, Fmt( "target chunk `%s` not found (or ambiguous) in category", std::string( targetName.c_str() ).c_str() ) };
		}
		const Cst::NodeId candidateId = SceneReferenceGraph::ResolveChunk( doc, candidateCategory, candidateName );
		if( candidateId == 0 ) {
			return { false, Fmt( "candidate chunk `%s` not found (or ambiguous) in category", std::string( candidateName.c_str() ).c_str() ) };
		}
		return CheckConnection( doc, targetId, paramName, candidateId );
	}

	bool ConnectionLegality::WouldCycle( const Cst::Document& doc, Cst::NodeId from, Cst::NodeId to )
	{
		if( from == 0 || to == 0 ) return false;
		if( from == to ) return true;   // direct self-reference

		// Adjacency from the document's CURRENT edges: referrer -> target.
		const std::vector<ReferenceEdge> edges = SceneReferenceGraph::Edges( doc );
		std::multimap<Cst::NodeId, Cst::NodeId> adj;
		for( size_t i = 0; i < edges.size(); ++i ) {
			adj.insert( std::make_pair( edges[i].referrerId, edges[i].targetId ) );
		}

		// Adding from->to creates a cycle iff `to` can already reach `from`.
		// Iterative BFS -- no recursion (a pathological chain must not blow
		// the stack; this runs on every attempted wire commit).
		std::set<Cst::NodeId> visited;
		std::queue<Cst::NodeId> q;
		q.push( to );
		visited.insert( to );
		while( !q.empty() ) {
			const Cst::NodeId cur = q.front(); q.pop();
			if( cur == from ) return true;
			std::pair<std::multimap<Cst::NodeId, Cst::NodeId>::const_iterator,
				std::multimap<Cst::NodeId, Cst::NodeId>::const_iterator> range = adj.equal_range( cur );
			for( std::multimap<Cst::NodeId, Cst::NodeId>::const_iterator it = range.first; it != range.second; ++it ) {
				if( visited.insert( it->second ).second ) q.push( it->second );
			}
		}
		return false;
	}
}
