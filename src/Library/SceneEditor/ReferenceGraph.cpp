//////////////////////////////////////////////////////////////////////
//
//  ReferenceGraph.cpp - See ReferenceGraph.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ReferenceGraph.h"
#include "ChunkDescriptorRegistry.h"

#include <cstring>
#include <map>
#include <utility>

namespace RISE
{
	namespace
	{
		typedef SceneReferenceGraph::DocumentChunk DocumentChunk;

		//! True iff `s` is empty under String's own convention (it always
		//! carries its NUL, so an empty string has size() <= 1) -- the same
		//! test BuildAuthoredTree's parent-name check uses (Cst.h has no
		//! String::empty()).
		bool StringEmpty( const String& s ) { return s.size() <= 1; }

		//! The shared structural walk both `Edges` and `EdgesAndDangling`
		//! need: given the document's chunk scan (`chunks`, from
		//! `AllChunks`) and an already-computed `Cst::ReferenceGraph`
		//! (`graph`, from ONE `Cst::BuildReferenceGraph` call), produce the
		//! presentation-level `ReferenceEdge` list. Pulled out so BOTH
		//! callers do exactly ONE `Cst::BuildReferenceGraph` pass and ONE
		//! `AllChunks` scan between them, never a second of either (see the
		//! header's P1-1 note).
		std::vector<ReferenceEdge> ComputeEdges( const Cst::Document& doc,
		                                          const std::vector<DocumentChunk>& chunks,
		                                          const Cst::ReferenceGraph& graph )
		{
			std::vector<ReferenceEdge> out;

			// sourceParam NodeId -> every resolved target(s).  A MULTIMAP, not a
			// map: a tuple param with more than one Reference-kind token (rare,
			// but the descriptor shape allows it) shares ONE param NodeId across
			// multiple targets (Cst.cpp's ComputeChunkRefs: "a tuple's ref tokens
			// share it") -- a plain map would silently drop all-but-one target.
			std::multimap<Cst::NodeId, Cst::NodeId> bySource;
			for( const Cst::ReferenceUse& e : graph.edges ) bySource.insert( std::make_pair( e.sourceValueNodeId, e.targetNodeId ) );
			if( bySource.empty() ) return out;   // nothing resolves anywhere: skip the chunk walk entirely

			std::map<Cst::NodeId, const DocumentChunk*> byId;
			for( const DocumentChunk& c : chunks ) byId[ c.id ] = &c;

			for( const DocumentChunk& c : chunks ) {
				const ChunkDescriptor* desc = c.hasCategory ? DescriptorForKeyword( c.keyword ) : nullptr;
				// Per-role occurrence counter, walked in document order -- the
				// EXACT convention Cst.cpp's ComputeChunkRefs (the producer of
				// `graph.edges`) and Cst.cpp's ChunkParams (the producer of the
				// paramIds every DocParamId call below resolves against) both use,
				// so occurrence N here names the SAME line either of them would.
				std::map<std::string, int> occCounter;
				for( const auto& kid : c.item->kids ) {
					if( kid->kind != Cst::NodeKind::Param ) continue;
					const std::string role = kid->role;
					const int occ = occCounter[role]++;
					// STRUCTURE ONLY: which (chunk, role, occurrence) wrote this
					// param -- not a re-resolution of what it points at.  Every
					// param (reference-typed or not) has an id (Cst.cpp's
					// AddChunkParams mints one for every Param kid at parse time),
					// so a non-reference param's id simply misses `bySource` below.
					const Cst::NodeId srcParam = Cst::DocParamId( doc, c.id, role, occ );
					if( srcParam == 0 ) continue;
					const std::pair<std::multimap<Cst::NodeId,Cst::NodeId>::const_iterator,
					                 std::multimap<Cst::NodeId,Cst::NodeId>::const_iterator>
						range = bySource.equal_range( srcParam );
					if( range.first == range.second ) continue;

					const ParameterDescriptor* pd = nullptr;
					if( desc ) {
						for( const ParameterDescriptor& p : desc->parameters ) {
							if( p.name == role ) { pd = &p; break; }
						}
					}

					for( std::multimap<Cst::NodeId,Cst::NodeId>::const_iterator it = range.first; it != range.second; ++it ) {
						const Cst::NodeId targetId = it->second;
						std::map<Cst::NodeId, const DocumentChunk*>::const_iterator ti = byId.find( targetId );

						ReferenceEdge e;
						e.referrerId       = c.id;
						e.referrerKeyword  = c.keyword;
						e.referrerCategory = c.category;
						e.referrerName     = c.name;
						e.paramName        = String( role.c_str() );
						e.occurrence       = occ;
						e.repeatable       = pd ? pd->repeatable : false;
						if( pd ) for( ChunkCategory rc : pd->referenceCategories ) e.portCategories.push_back( rc );

						e.targetId = targetId;
						if( ti != byId.end() ) {
							e.targetCategory = ti->second->category;
							e.targetName     = ti->second->name;
						} else {
							// BELT-AND-BRACES: unreachable in practice.  Every
							// targetId in `graph.edges` came from BuildReferenceGraph's
							// own producer namespace (`defs`), which only ever seeds
							// real chunk NodeIds -- so `byId` (built from the SAME
							// document) should always contain it.  Kept, and
							// labelled, rather than assumed: degrading to an empty
							// target beats a dereference of `ti->second` that a
							// future divergence between the two scans would turn
							// into a crash instead of a wrong-but-safe row.
							// SENTINEL: `e.targetName` is left at its default
							// ("", String's own empty convention) in this branch --
							// a caller that hits `targetName.size() <= 1` paired
							// with a `targetCategory` that just mirrors the
							// REFERRER's own category (not a real resolution) can
							// use that pairing to recognize this unreachable-in-
							// practice fallback rather than trust the category.
							e.targetCategory = c.category;
						}
						out.push_back( e );
					}
				}
			}
			return out;
		}
	}

	std::vector<DocumentChunk> SceneReferenceGraph::AllChunks( const Cst::Document& doc )
	{
		std::vector<DocumentChunk> out;
		const int n = Cst::DocItemCount( doc );
		out.reserve( n > 0 ? static_cast<std::size_t>( n ) : 0 );
		for( int i = 0; i < n; ++i ) {
			const Cst::NodeId id = Cst::DocNodeIdAt( doc, i );
			const Cst::NodeRef item = Cst::DocResolveNodeId( doc, id );
			if( !item || item->kind != Cst::NodeKind::Chunk ) continue;   // trivia/stray item: not a chunk
			DocumentChunk ci;
			ci.id   = id;
			ci.item = item;
			ci.keyword = String( item->role.c_str() );
			if( const ChunkDescriptor* desc = DescriptorForKeyword( ci.keyword ) ) {
				ci.hasCategory = true;
				ci.category    = desc->category;
			}
			bool present = false;
			const std::string nm = Cst::ParamValueAsParsed( item, "name", &present );
			if( present && !nm.empty() ) ci.name = String( nm.c_str() );
			out.push_back( ci );
		}
		return out;
	}

	Cst::NodeId SceneReferenceGraph::ResolveChunk( const Cst::Document& doc, ChunkCategory category,
	                                                const String& name, int* outOccurrences )
	{
		if( outOccurrences ) *outOccurrences = 0;
		if( StringEmpty( name ) ) return 0;
		const std::vector<DocumentChunk> chunks = AllChunks( doc );
		Cst::NodeId found = 0;
		int count = 0;
		for( const DocumentChunk& c : chunks ) {
			if( !c.hasCategory || c.category != category ) continue;
			if( StringEmpty( c.name ) ) continue;                        // unnamed: not addressable by name
			if( std::strcmp( c.name.c_str(), name.c_str() ) != 0 ) continue;
			++count;
			found = c.id;
		}
		if( outOccurrences ) *outOccurrences = count;
		return ( count == 1 ) ? found : 0;   // unique-or-refuse, like Cst::DocFindByName
	}

	std::vector<ReferenceEdge> SceneReferenceGraph::Edges( const Cst::Document& doc,
	                                                        const std::vector<DocumentChunk>* prescanned )
	{
		// RESOLUTION, delegated entirely to Cst::BuildReferenceGraph (see the
		// header's "why wrap" note) -- this is the ONLY place this file decides
		// which chunk a reference value names.
		const Cst::ReferenceGraph graph = Cst::BuildReferenceGraph( doc );
		if( prescanned ) return ComputeEdges( doc, *prescanned, graph );
		const std::vector<DocumentChunk> chunks = AllChunks( doc );
		return ComputeEdges( doc, chunks, graph );
	}

	std::vector<Cst::UnresolvedReference> SceneReferenceGraph::DanglingReferences( const Cst::Document& doc )
	{
		std::vector<Cst::UnresolvedReference> unresolved;
		Cst::BuildReferenceGraph( doc, nullptr, &unresolved );
		return unresolved;
	}

	SceneReferenceGraph::Snapshot SceneReferenceGraph::EdgesAndDangling( const Cst::Document& doc,
	                                                                      const std::vector<DocumentChunk>* prescanned )
	{
		Snapshot out;
		// ONE Cst::BuildReferenceGraph pass produces BOTH halves -- doc-88
		// S11 review round 1 P1-1: the prior seeding path called Edges()
		// (its own BuildReferenceGraph pass) and then DanglingReferences()
		// (a SECOND, separate pass) back to back. Both out-params are wired
		// to the SAME call here instead.
		const Cst::ReferenceGraph graph = Cst::BuildReferenceGraph( doc, nullptr, &out.unresolved );
		if( prescanned ) {
			out.edges = ComputeEdges( doc, *prescanned, graph );
		} else {
			const std::vector<DocumentChunk> chunks = AllChunks( doc );
			out.edges = ComputeEdges( doc, chunks, graph );
		}
		return out;
	}

	std::vector<ReferenceEdge> SceneReferenceGraph::FindReferencesTo( const Cst::Document& doc,
	                                                                   ChunkCategory category, const String& name,
	                                                                   int* outOccurrences )
	{
		std::vector<ReferenceEdge> out;
		const Cst::NodeId target = ResolveChunk( doc, category, name, outOccurrences );
		if( target == 0 ) return out;
		const std::vector<ReferenceEdge> all = Edges( doc );
		for( const ReferenceEdge& e : all ) if( e.targetId == target ) out.push_back( e );
		return out;
	}

	std::vector<ReferenceEdge> SceneReferenceGraph::FindReferencesFrom( const Cst::Document& doc,
	                                                                     ChunkCategory category, const String& name,
	                                                                     int* outOccurrences )
	{
		std::vector<ReferenceEdge> out;
		const Cst::NodeId src = ResolveChunk( doc, category, name, outOccurrences );
		if( src == 0 ) return out;
		const std::vector<ReferenceEdge> all = Edges( doc );
		for( const ReferenceEdge& e : all ) if( e.referrerId == src ) out.push_back( e );
		return out;
	}
}
