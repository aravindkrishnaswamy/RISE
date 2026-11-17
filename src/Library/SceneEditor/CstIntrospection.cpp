//////////////////////////////////////////////////////////////////////
//
//  CstIntrospection.cpp - See CstIntrospection.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CstIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "ConnectionLegality.h"
#include "ReferenceGraph.h"
#include "../Interfaces/IGeometryManager.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IFunction1DManager.h"
#include "../Interfaces/IFunction2DManager.h"
#include "../Interfaces/IScenePriv.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>

namespace RISE
{

namespace
{
	// Name-collecting IEnumCallback shared by every manager enumeration
	// below (same shape as SceneEditController.cpp's CollectNamesCallback).
	struct CollectNames : public IEnumCallback<const char*>
	{
		std::vector<String> names;
		bool operator()( const char* const& name ) override
		{
			if( name && name[0] ) names.push_back( String( name ) );
			return true;
		}
	};

	void SortStable( std::vector<String>& v )
	{
		std::stable_sort( v.begin(), v.end(),
			[]( const String& a, const String& b ) { return std::string( a.c_str() ) < std::string( b.c_str() ); } );
	}

	void AppendUnique( std::vector<String>& dst, const std::vector<String>& src )
	{
		for( const String& s : src )
		{
			bool dup = false;
			for( const String& e : dst ) { if( e == s ) { dup = true; break; } }
			if( !dup ) dst.push_back( s );
		}
	}

	bool IsSingleFiniteNumber( const char* s, bool disallowNegative = false )
	{
		if( !s || !s[0] ) return false;
		const char* p = s;
		while( *p == ' ' || *p == '\t' ) ++p;
		if( *p == '\0' ) return false;
		if( disallowNegative && *p == '-' ) return false;
		const char* q = ( *p == '+' || *p == '-' ) ? ( p + 1 ) : p;
		if( q[0] == '0' && ( q[1] == 'x' || q[1] == 'X' ) ) return false;
		char* end = nullptr;
		errno = 0;
		const double v = std::strtod( p, &end );
		if( end == p || errno == ERANGE || !std::isfinite( v ) ) return false;
		if( disallowNegative && v < 0.0 ) return false;
		while( *end == ' ' || *end == '\t' ) ++end;
		return *end == '\0';
	}

	bool IsThreeFiniteNumbers( const char* s )
	{
		if( !s || !s[0] ) return false;
		const char* p = s;
		for( int i = 0; i < 3; ++i )
		{
			while( *p == ' ' || *p == '\t' ) ++p;
			if( *p == '\0' ) return false;
			const char* q = ( *p == '+' || *p == '-' ) ? ( p + 1 ) : p;
			if( q[0] == '0' && ( q[1] == 'x' || q[1] == 'X' ) ) return false;
			char* end = nullptr;
			errno = 0;
			const double v = std::strtod( p, &end );
			if( end == p || errno == ERANGE || !std::isfinite( v ) ) return false;
			if( i < 2 && end && *end != ' ' && *end != '\t' ) return false;
			p = end;
		}
		while( *p == ' ' || *p == '\t' ) ++p;
		return *p == '\0';
	}

	bool TargetsPainterCategory( const std::vector<ChunkCategory>& cats )
	{
		for( ChunkCategory rc : cats )
		{
			if( rc == ChunkCategory::Painter || rc == ChunkCategory::Function )
				return true;
		}
		return false;
	}

	bool IsConstantUniformColorPainterChunk( const RISE::Cst::NodeRef& chunk, std::string& outColor )
	{
		outColor.clear();
		if( !chunk || chunk->role != "uniformcolor_painter" ) return false;
		if( RISE::Cst::ParamOccurrenceCount( chunk, "name" ) != 1 ) return false;
		if( RISE::Cst::ParamOccurrenceCount( chunk, "color" ) != 1 ) return false;
		for( const RISE::Cst::NodeRef& kid : chunk->kids )
		{
			if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
			if( kid->role != "name" && kid->role != "color" && kid->role != "colorspace" )
				return false;
		}
		if( RISE::Cst::ParamOccurrenceCount( chunk, "colorspace" ) > 0 )
		{
			if( RISE::Cst::ParamOccurrenceCount( chunk, "colorspace" ) != 1 ) return false;
			bool csPresent = false;
			const std::string cs = RISE::Cst::ParamValueAsParsed( chunk, "colorspace", &csPresent );
			if( !csPresent || cs != "Rec709RGB_Linear" ) return false;
		}
		bool present = false;
		const std::string val = RISE::Cst::ParamValueAsParsed( chunk, "color", &present );
		if( !present || !IsThreeFiniteNumbers( val.c_str() ) ) return false;
		outColor = val;
		return true;
	}

	bool IsConstantUniformScalarPainterChunk( const RISE::Cst::NodeRef& chunk, std::string& outValue )
	{
		outValue.clear();
		if( !chunk || chunk->role != "scalar_painter" ) return false;
		if( RISE::Cst::ParamOccurrenceCount( chunk, "name" ) != 1 ) return false;
		if( RISE::Cst::ParamOccurrenceCount( chunk, "value" ) != 1 ) return false;
		for( const RISE::Cst::NodeRef& kid : chunk->kids )
		{
			if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
			if( kid->role != "name" && kid->role != "value" )
				return false;
		}
		bool present = false;
		const std::string val = RISE::Cst::ParamValueAsParsed( chunk, "value", &present );
		if( !present || !IsSingleFiniteNumber( val.c_str(), false ) ) return false;
		outValue = val;
		return true;
	}

	std::vector<String> GatherCandidateNamesForParam(
		IJobPriv& job, const ParameterDescriptor& p )
	{
		CollectNames cb;
		switch( p.semantics.pipe )
		{
		case ParameterPipe::Scalar:
			if( IScalarPainterManager* m = job.GetScalarPainters() ) m->EnumerateItemNames( cb );
			SortStable( cb.names );
			return cb.names;

		case ParameterPipe::Color:
			if( IPainterManager* m = job.GetPainters() ) m->EnumerateItemNames( cb );
			if( p.semantics.colorAlsoAcceptsScalar )
			{
				if( IScalarPainterManager* m = job.GetScalarPainters() ) m->EnumerateItemNames( cb );
			}
			break;

		case ParameterPipe::Function1D:
			if( IFunction1DManager* m = job.GetFunction1Ds() ) m->EnumerateItemNames( cb );
			SortStable( cb.names );
			return cb.names;

		case ParameterPipe::Function2D:
			if( IFunction2DManager* m = job.GetFunction2Ds() ) m->EnumerateItemNames( cb );
			SortStable( cb.names );
			return cb.names;

		case ParameterPipe::Material:
			if( IMaterialManager* m = job.GetMaterials() ) m->EnumerateItemNames( cb );
			SortStable( cb.names );
			return cb.names;

		case ParameterPipe::Geometry:
			if( IGeometryManager* m = job.GetGeometries() ) m->EnumerateItemNames( cb );
			SortStable( cb.names );
			return cb.names;

		case ParameterPipe::Unspecified:
		case ParameterPipe::Other:
		default:
		{
			std::vector<String> cands;
			for( ChunkCategory rc : p.referenceCategories )
			{
				AppendUnique( cands, CstIntrospection::CandidateNamesForChunkCategory( job, rc ) );
			}
			return cands;
		}
		}

		SortStable( cb.names );
		std::vector<String> deduped;
		AppendUnique( deduped, cb.names );
		return deduped;
	}

	bool DeclaredChunkMatchesParamDomain(
		const SceneReferenceGraph::DocumentChunk& dc,
		const ParameterDescriptor& p )
	{
		if( !dc.hasCategory ) return false;
		switch( p.semantics.pipe )
		{
		case ParameterPipe::Scalar:
			return dc.keyword == String( "scalar_painter" );
		case ParameterPipe::Color:
			if( ConnectionLegality::IsColorCapable( dc.keyword.c_str(), dc.category ) )
				return true;
			if( p.semantics.colorAlsoAcceptsScalar && dc.keyword == String( "scalar_painter" ) )
				return true;
			return false;
		case ParameterPipe::Function1D:
			return dc.keyword == String( "piecewise_linear_function" );
		case ParameterPipe::Function2D:
			return ConnectionLegality::IsFunction2DCapable( dc.keyword.c_str(), dc.category );
		case ParameterPipe::Material:
			return dc.category == ChunkCategory::Material;
		case ParameterPipe::Geometry:
			return dc.category == ChunkCategory::Geometry;
		default:
			if( p.referenceCategories.empty() ) return true;
			for( ChunkCategory rc : p.referenceCategories )
			{
				if( dc.category == rc ) return true;
				if( rc == ChunkCategory::Function && dc.category == ChunkCategory::Painter
					&& dc.keyword != String( "scalar_painter" ) )
					return true;
				if( rc == ChunkCategory::Painter && dc.keyword == String( "piecewise_linear_function" ) )
					return true;
			}
			return false;
		}
	}

	const SceneReferenceGraph::DocumentChunk* SelectSingleBoundPainterChunk(
		const std::vector<const SceneReferenceGraph::DocumentChunk*>& matches,
		const ParameterDescriptor& p )
	{
		if( matches.size() == 1 ) return matches[0];
		if( matches.size() > 1
			&& p.semantics.pipe == ParameterPipe::Color
			&& p.semantics.colorAlsoAcceptsScalar )
		{
			std::vector<const SceneReferenceGraph::DocumentChunk*> scalarMatches;
			for( const SceneReferenceGraph::DocumentChunk* dc : matches )
			{
				if( dc && dc->keyword == String( "scalar_painter" ) )
					scalarMatches.push_back( dc );
			}
			if( scalarMatches.size() == 1 ) return scalarMatches[0];
		}
		return nullptr;
	}
}   // anonymous namespace

std::vector<String> CstIntrospection::CandidateNamesForChunkCategory(
	IJobPriv& job, ChunkCategory cat )
{
	CollectNames cb;
	switch( cat )
	{
	case ChunkCategory::Painter:
		// A colour/scalar material slot references EITHER pipe -- offer the
		// two-pipe union (colour + scalar).
		if( IPainterManager* m = job.GetPainters() ) m->EnumerateItemNames( cb );
		if( IScalarPainterManager* m = job.GetScalarPainters() ) m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Function:
		// A Function reference binds an IFunction1D/2D, NOT a painter --
		// enumerate the FUNCTION managers.  Colour painters that also
		// implement IFunction dual-register there (Cst.cpp PASS A), so this
		// stays a superset of the bindable candidates WITHOUT wrongly offering
		// scalar painters (which cannot bind a function slot) or omitting
		// standalone Function1D/2D chunks (which the painter union missed).
		if( IFunction1DManager* m = job.GetFunction1Ds() ) m->EnumerateItemNames( cb );
		if( IFunction2DManager* m = job.GetFunction2Ds() ) m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Material:
		if( IMaterialManager* m = job.GetMaterials() ) m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Geometry:
		if( IGeometryManager* m = job.GetGeometries() ) m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Medium:
		job.EnumerateMediumNames( cb );
		break;
	case ChunkCategory::HairGuides:
		// The other manager-less Job-side name table (`hairGuidesMap`), so
		// the same shape as Medium above: a dedicated IJob enumeration hook
		// rather than a manager.  A `hair_geometry.guides` row's candidates
		// come from here and NOWHERE else -- guide sets are never in the
		// IGeometryManager, which is exactly why they no longer share
		// ChunkCategory::Geometry with the chunks that are.
		job.EnumerateHairGuideNames( cb );
		break;
	case ChunkCategory::Object:
		if( IScenePriv* s = job.GetScene() )
			if( const IObjectManager* m = s->GetObjects() )
				m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Camera:
		if( IScenePriv* s = job.GetScene() )
			if( const ICameraManager* m = s->GetCameras() )
				m->EnumerateItemNames( cb );
		break;
	case ChunkCategory::Light:
		if( IScenePriv* s = job.GetScene() )
			if( const ILightManager* m = s->GetLights() )
				m->EnumerateItemNames( cb );
		break;
	default:
		// Shader / ShaderOp / Modifier / PhotonMap / ... -- no cheap live
		// enumeration surface today; callers keep the descriptor presets.
		break;
	}
	SortStable( cb.names );
	return cb.names;
}

std::vector<CameraProperty> CstIntrospection::Inspect(
	const RISE::Cst::Document* doc, IJobPriv& job,
	const String& entityName, const char* roleKindSuffix,
	const char* typeRowDescription )
{
	std::vector<CameraProperty> out;
	if( !doc || entityName.size() <= 1 || !roleKindSuffix || !roleKindSuffix[0] ) return out;

	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, entityName.c_str(), nullptr, roleKindSuffix, /*uniqueFallback=*/false );
	if( id == 0 ) return out;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return out;

	// Defensive kind check (inherited from the painter original).  Since
	// round 6 (85e1b9bd) DocFindByNameAnyRole enforces roleKindSuffix as a
	// HARD CONSTRAINT on every match (single or ambiguous), so a resolved
	// chunk already satisfies the kind; this re-verify is defense-in-depth
	// against a name existing in a manager while some OTHER category's chunk
	// carries the same name.  It calls the resolver's OWN shared predicate
	// (suffix fast path + registry-classifier authority) so it can never
	// diverge -- the old suffix-only re-check here wrongly refused registry-
	// classified kinds (an expression_function2d addressed as "painter").
	if( !RISE::Cst::RoleMatchesKindConstraint( chunk->role, roleKindSuffix ) ) return out;

	const String keyword( chunk->role.c_str() );
	const ChunkDescriptor* cd = DescriptorForKeyword( keyword );

	// Leading read-only identity row -- always present even if the
	// keyword has no registered descriptor, so the panel isn't blank.
	{
		CameraProperty row;
		row.name        = String( "chunk_type" );
		row.kind         = ValueKind::String;
		row.value        = keyword;
		row.description  = String( typeRowDescription ? typeRowDescription : "Chunk keyword" );
		// DL-323 follow-through: a deprecated chunk type says so on its identity row (it still works).
		// Legacy Phase 1: a FROZEN type is labelled legacy / unsupported instead.
		if( cd && cd->frozen )
			row.description = String( ( std::string( row.description.c_str() ) + " -- LEGACY (unsupported; frozen 2026-10-09, still renders exactly as before): use " + cd->replacement ).c_str() );
		else if( cd && cd->deprecated )
			row.description = String( ( std::string( row.description.c_str() ) + " -- DEPRECATED (still renders exactly as before): use " + cd->replacement ).c_str() );
		row.editable     = false;
		out.push_back( row );
	}
	if( !cd ) return out;

	bool graphBuilt = false;
	std::vector<SceneReferenceGraph::DocumentChunk> allChunks;
	std::vector<ReferenceEdge> allEdges;
	std::set<RISE::Cst::NodeId> ancestorsOfChunk;
	std::unordered_map<std::string, std::vector<const SceneReferenceGraph::DocumentChunk*> > priorChunksByName;
	std::unordered_map<RISE::Cst::NodeId, std::vector<RISE::Cst::NodeId> > referrersByTargetId;
	std::unordered_map<RISE::Cst::NodeId, unsigned int> slotRefCountByTargetId;

	auto ensureGraph = [&]()
	{
		if( graphBuilt ) return;
		graphBuilt = true;
		allChunks = SceneReferenceGraph::AllChunks( *doc );
		allEdges  = SceneReferenceGraph::Edges( *doc, &allChunks );

		std::size_t targetChunkIdx = allChunks.size();
		std::unordered_map<RISE::Cst::NodeId, const SceneReferenceGraph::DocumentChunk*> chunkById;
		std::unordered_map<RISE::Cst::NodeId, std::size_t> chunkIdxById;
		std::unordered_map<std::string, std::vector<const SceneReferenceGraph::DocumentChunk*> > allChunksByName;
		chunkById.reserve( allChunks.size() );
		chunkIdxById.reserve( allChunks.size() );
		for( std::size_t i = 0; i < allChunks.size(); ++i )
		{
			chunkById[allChunks[i].id] = &allChunks[i];
			chunkIdxById[allChunks[i].id] = i;
			if( allChunks[i].name.size() > 1 )
			{
				allChunksByName[allChunks[i].name.c_str()].push_back( &allChunks[i] );
			}
			if( allChunks[i].id == id && targetChunkIdx == allChunks.size() )
			{
				targetChunkIdx = i;
			}
		}
		for( std::size_t i = 0; i < targetChunkIdx && i < allChunks.size(); ++i )
		{
			if( allChunks[i].name.size() > 1 )
			{
				priorChunksByName[allChunks[i].name.c_str()].push_back( &allChunks[i] );
			}
		}

		auto makeNameCatKey = []( const char* nm, ChunkCategory cat, bool isScalarPainter ) -> std::string
		{
			std::string key( nm ? nm : "" );
			key.push_back( '\0' );
			key.push_back( static_cast<char>( cat ) );
			key.push_back( isScalarPainter ? '\x01' : '\x00' );
			return key;
		};

		std::unordered_map<std::string, std::vector<RISE::Cst::NodeId> > referrersByNameCat;
		for( const ReferenceEdge& e : allEdges )
		{
			if( e.referrerId == 0 ) continue;
			RISE::Cst::NodeId resolvedTargetId = e.targetId;
			if( ( e.targetCategory == ChunkCategory::Painter || e.targetCategory == ChunkCategory::Function )
				&& e.targetName.size() > 1 )
			{
				auto itSameName = allChunksByName.find( e.targetName.c_str() );
				if( itSameName != allChunksByName.end() && itSameName->second.size() > 1 )
				{
					const ChunkDescriptor* refCd = DescriptorForKeyword( e.referrerKeyword );
					const ParameterDescriptor* refPd = nullptr;
					if( refCd && e.paramName.size() > 1 )
					{
						for( const ParameterDescriptor& rp : refCd->parameters )
						{
							if( rp.name == e.paramName.c_str() ) { refPd = &rp; break; }
						}
					}
					if( refPd && !refPd->carriesExpressionSampleRefs )
					{
						auto itRefIdx = chunkIdxById.find( e.referrerId );
						const std::size_t maxIdx =
							( itRefIdx != chunkIdxById.end() ) ? itRefIdx->second : allChunks.size();
						std::vector<const SceneReferenceGraph::DocumentChunk*> domainMatches;
						for( const SceneReferenceGraph::DocumentChunk* cand : itSameName->second )
						{
							auto itCandIdx = chunkIdxById.find( cand->id );
							if( itCandIdx != chunkIdxById.end() && itCandIdx->second < maxIdx
								&& DeclaredChunkMatchesParamDomain( *cand, *refPd ) )
							{
								domainMatches.push_back( cand );
							}
						}
						if( const SceneReferenceGraph::DocumentChunk* exactDc =
							SelectSingleBoundPainterChunk( domainMatches, *refPd ) )
						{
							resolvedTargetId = exactDc->id;
						}
						else if( !domainMatches.empty() )
						{
							resolvedTargetId = domainMatches[0]->id;
						}
					}
				}
			}
			if( resolvedTargetId != 0 )
			{
				referrersByTargetId[resolvedTargetId].push_back( e.referrerId );
				slotRefCountByTargetId[resolvedTargetId] += 1u;
			}
			if( e.targetName.size() > 1 )
			{
				auto itTargetDc = chunkById.find( resolvedTargetId );
				const ChunkCategory resolvedCat =
					( itTargetDc != chunkById.end() && itTargetDc->second )
						? itTargetDc->second->category : e.targetCategory;
				const bool targetIsScalar =
					( itTargetDc != chunkById.end() && itTargetDc->second
					  && itTargetDc->second->keyword == String( "scalar_painter" ) );
				referrersByNameCat[makeNameCatKey( e.targetName.c_str(), resolvedCat, targetIsScalar )].push_back( e.referrerId );
			}
		}

		ancestorsOfChunk.insert( id );
		std::vector<RISE::Cst::NodeId> work;
		work.push_back( id );
		while( !work.empty() )
		{
			const RISE::Cst::NodeId cur = work.back();
			work.pop_back();

			auto itId = referrersByTargetId.find( cur );
			if( itId != referrersByTargetId.end() )
			{
				for( RISE::Cst::NodeId refId : itId->second )
				{
					if( ancestorsOfChunk.insert( refId ).second )
						work.push_back( refId );
				}
			}

			auto itDc = chunkById.find( cur );
			if( itDc != chunkById.end() && itDc->second
				&& itDc->second->name.size() > 1 && itDc->second->hasCategory )
			{
				const bool curIsScalar = ( itDc->second->keyword == String( "scalar_painter" ) );
				const std::string key = makeNameCatKey(
					itDc->second->name.c_str(), itDc->second->category, curIsScalar );
				auto itName = referrersByNameCat.find( key );
				if( itName != referrersByNameCat.end() )
				{
					for( RISE::Cst::NodeId refId : itName->second )
					{
						if( ancestorsOfChunk.insert( refId ).second )
							work.push_back( refId );
					}
				}
			}
		}
	};

	for( const ParameterDescriptor& p : cd->parameters )
	{
		if( p.name == "name" ) continue;      // covered by the identity row above; renaming is a dedicated affordance
		// Repeatable params are NOT surfaced by the generic surface: one row per
		// role cannot represent an ordered list, and this surface's edit route
		// addresses occurrence 0.  A family that needs them enumerates its own
		// occurrence rows on top of this one and routes them with an explicit
		// `occ` -- PainterIntrospection does exactly that (doc 88 S4 + S4b).
		if( p.repeatable ) continue;

		// LAST occurrence, not first: this row has to say what the LIVE entity is,
		// and the parse this panel sits on top of is last-wins (Cst::
		// ParamValueAsParsed's doc has the chain).  Reading occurrence 0 here made
		// the panel disagree with the renderer on any chunk that spells a
		// non-repeatable param twice -- `geometry none` above `geometry gv` showed
		// `none` for an object that is bound to `gv`.
		bool present = false;
		const std::string raw = RISE::Cst::ParamValueAsParsed( chunk, p.name, &present );

		// ...and a duplicate makes the row UNWRITABLE, not merely mis-read.  The
		// edit route underneath (ApplyAgentParamEdit / SceneEditor::Apply, both
		// occ=0) addresses the FIRST occurrence, which is the dead one -- so an
		// edit of this row would rewrite an invisible line and leave the displayed
		// value untouched.  Job::ApplyCstParamEditImpl_ refuses that write; a
		// read-only row keeps the panel from offering an edit it knows is refused,
		// and the description says which line to delete.
		const int occurrences = RISE::Cst::ParamOccurrenceCount( chunk, p.name );

		CameraProperty row;
		row.name        = String( p.name.c_str() );
		row.kind         = p.kind;
		row.value        = present ? String( raw.c_str() ) : String( p.defaultValueHint.c_str() );
		row.description  = String( p.description.c_str() );
		// 0 occurrences is EDITABLE, not read-only: the row is showing the
		// descriptor default for a slot the scene text omits, and the edit route
		// INSERTS it (DocSetOrAddParamValue).  Only a genuine duplicate is unwritable.
		row.editable     = ( occurrences <= 1 );   // routed through ApplyAgentParamEdit -- see header doc
		row.unitLabel    = String( p.unitLabel.c_str() );

		if( occurrences > 1 )
		{
			row.description = String( ( std::string( p.description.c_str() )
				+ "  [READ-ONLY: `" + p.name + "` is spelled " + std::to_string( occurrences )
				+ " times in this chunk but is not a repeatable parameter.  The scene derives from"
				  " the LAST one (shown here); the earlier ones are dead text.  Delete the"
				  " duplicates in the scene file to make this row editable again.]" ).c_str() );
		}

		if( p.kind == ValueKind::Reference )
		{
			// Jump-to-definition metadata: what kind(s) of element the
			// value names, straight from the descriptor.
			row.referenceCategories = p.referenceCategories;
			row.targetsPainter = TargetsPainterCategory( p.referenceCategories );
			ensureGraph();

			const std::vector<String> rawCands = GatherCandidateNamesForParam( job, p );
			std::vector<String> cands;
			IScalarPainterManager* spm = job.GetScalarPainters();

			for( const String& n : rawCands )
			{
				if( n == String( "none" ) )
				{
					// Exclude built-in "none" when a keyword allowlist restricts the
					// target chunk keywords, when the parameter is required (e.g.
					// coated_material.base, arealight_shaderop.material,
					// displace_geometry.geometry), or when the pipe is Geometry on a
					// non-standard_object chunk or a Function1D/Function2D pipe where
					// "none" is not registered.
					if( !p.semantics.keywordAllowlist.empty() ) continue;
					if( p.required ) continue;
					if( p.semantics.pipe == ParameterPipe::Geometry && cd->keyword != "standard_object" ) continue;
					if( p.semantics.pipe == ParameterPipe::Function1D || p.semantics.pipe == ParameterPipe::Function2D ) continue;
					cands.push_back( n );
					continue;
				}

				// Every non-"none" reference in a CST document must resolve to a
				// chunk declared strictly BEFORE the referencing chunk in document
				// order (DeriveToJob is sequential; forward references and runtime-
				// only entities with no CST chunk are rejected by
				// WouldPersistDanglingReference_).
				auto itPrior = priorChunksByName.find( n.c_str() );
				if( itPrior == priorChunksByName.end() ) continue;
				std::vector<const SceneReferenceGraph::DocumentChunk*> matchedChunks;
				for( const SceneReferenceGraph::DocumentChunk* dc : itPrior->second )
				{
					if( DeclaredChunkMatchesParamDomain( *dc, p ) )
					{
						matchedChunks.push_back( dc );
					}
				}
				if( matchedChunks.empty() ) continue;
				if( matchedChunks.size() > 1
					&& p.semantics.pipe == ParameterPipe::Color
					&& p.semantics.colorAlsoAcceptsScalar )
				{
					std::vector<const SceneReferenceGraph::DocumentChunk*> scalarMatches;
					for( const SceneReferenceGraph::DocumentChunk* dc : matchedChunks )
					{
						if( dc && dc->keyword == String( "scalar_painter" ) )
							scalarMatches.push_back( dc );
					}
					if( !scalarMatches.empty() )
						matchedChunks.swap( scalarMatches );
				}

				bool allLegal = true;
				for( const SceneReferenceGraph::DocumentChunk* dc : matchedChunks )
				{
					if( ancestorsOfChunk.find( dc->id ) != ancestorsOfChunk.end() )
					{
						allLegal = false;   // self-reference or cycle
						break;
					}

					bool sourceHasValues = false;
					if( dc->keyword == String( "scalar_painter" ) )
					{
						if( dc->item && RISE::Cst::ParamOccurrenceCount( dc->item, "values" ) > 0 )
							sourceHasValues = true;
						if( !sourceHasValues && spm )
						{
							if( IScalarPainter* sp = spm->GetItem( n.c_str() ) )
							{
								if( sp->HasPerChannelVariation() )
								{
									if( p.semantics.pipe == ParameterPipe::Scalar )
									{
										// DL-29: ResolveOrDiagnoseScalar(..., requireSingle=true)
										// accepts a spectral-curve scalar_painter (sellmeier,
										// cauchy, file, function1d) whenever MakeSingleScalarSlotView()
										// returns a non-null 550nm view; only 3-value `values` and
										// `vec3(...)` expression scalar painters return nullptr.
										IScalarPainter* singleView = sp->MakeSingleScalarSlotView();
										if( singleView )
										{
											safe_release( singleView );
										}
										else
										{
											sourceHasValues = true;
										}
									}
									else
									{
										// ResolveRotationPainterDual (Color pipe with
										// colorAlsoAcceptsScalar) rejects any
										// sp->HasPerChannelVariation() without calling
										// MakeSingleScalarSlotView().
										sourceHasValues = true;
									}
								}
							}
						}
					}

					const ConnectionVerdict check = ConnectionLegality::CheckConnectionByKeyword(
						cd->keyword, p.name,
						dc->keyword.c_str(), dc->category,
						sourceHasValues );
					if( !check.legal )
					{
						allLegal = false;
						break;
					}
				}
				if( !allLegal ) continue;

				cands.push_back( n );
			}

			if( !cands.empty() )
			{
				for( const String& n : cands )
				{
					ParameterPreset pr;
					pr.label = n.c_str();
					pr.value = n.c_str();
					row.presets.push_back( pr );
				}
			}

			// Material Editor Slice 3: detect when this reference slot is bound to a
			// CST-authored constant painter (`uniformcolor_painter` with 3 decimal
			// numbers in `color`, or Form-1 `scalar_painter` with a single `value`)
			// that is uniquely addressable by `(name, role)` in the document.
			if( row.targetsPainter && row.editable && present && occurrences == 1
				&& row.value.size() > 1 && row.value != String( "none" ) )
			{
				auto itBound = priorChunksByName.find( row.value.c_str() );
				if( itBound != priorChunksByName.end() )
				{
					std::vector<const SceneReferenceGraph::DocumentChunk*> boundMatches;
					for( const SceneReferenceGraph::DocumentChunk* dc : itBound->second )
					{
						if( DeclaredChunkMatchesParamDomain( *dc, p ) )
						{
							boundMatches.push_back( dc );
						}
					}
					const SceneReferenceGraph::DocumentChunk* targetDc =
						SelectSingleBoundPainterChunk( boundMatches, p );
					if( targetDc && targetDc->item )
					{
						const RISE::Cst::NodeId exactId = RISE::Cst::DocFindByNameAnyRole(
							*doc, row.value.c_str(), nullptr, std::string( targetDc->keyword.c_str() ), /*uniqueFallback=*/false );
						if( exactId == targetDc->id )
						{
							std::string inlineVal;
							CameraProperty::InlinePainterKind inlineKind = CameraProperty::InlinePainterKind::None;
							if( IsConstantUniformColorPainterChunk( targetDc->item, inlineVal ) )
							{
								inlineKind = CameraProperty::InlinePainterKind::UniformColor;
							}
							else if( IsConstantUniformScalarPainterChunk( targetDc->item, inlineVal ) )
							{
								inlineKind = CameraProperty::InlinePainterKind::ScalarValue;
							}
							if( inlineKind != CameraProperty::InlinePainterKind::None )
							{
								auto itRefs = slotRefCountByTargetId.find( targetDc->id );
								const unsigned int refCount =
									( itRefs != slotRefCountByTargetId.end() && itRefs->second > 0u )
									? itRefs->second
									: 1u;
								row.inlinePainterKind     = inlineKind;
								row.inlinePainterValue    = String( inlineVal.c_str() );
								row.inlinePainterRefCount = refCount;
							}
						}
					}
				}
			}
		}
		if( row.presets.empty() )
		{
			for( const ParameterPreset& pr : p.presets )
			{
				ParameterPreset copy;
				copy.label = pr.label;
				copy.value = pr.value;
				row.presets.push_back( copy );
			}
		}
		// Enum rows: the descriptor's enumValues ARE the pick list.  Fold
		// them into presets when the descriptor declared no explicit
		// presets -- both shells drive their enum combo/chip UI off
		// row.presets (the wire surface has no separate enumValues field),
		// so without this an Enum row degrades to a bare text well.
		if( row.presets.empty() && p.kind == ValueKind::Enum )
		{
			for( const std::string& ev : p.enumValues )
			{
				ParameterPreset pr;
				pr.label = ev;
				pr.value = ev;
				row.presets.push_back( pr );
			}
		}
		if( row.editable && p.hasSoftRange )
		{
			row.rangeMin  = p.softMin;
			row.rangeMax  = p.softMax;
			row.rangeStep = p.softStep;
			if( IsSingleFiniteNumber( row.value.c_str(), p.kind == ValueKind::UInt ) )
			{
				row.hasRange = true;
			}
		}
		out.push_back( row );
	}
	return out;
}


void CstIntrospection::AugmentWithCstRows(
	std::vector<CameraProperty>& rows, const RISE::Cst::Document* doc,
	IJobPriv& job, const String& entityName, const char* roleKindSuffix )
{
	const std::vector<CameraProperty> generic =
		Inspect( doc, job, entityName, roleKindSuffix, "Chunk keyword" );
	if( generic.empty() ) return;   // no CST chunk of this kind -- live rows stand

	for( const CameraProperty& g : generic )
	{
		// Live modules carry their own identity rows (a capitalized "Type" --
		// MaterialIntrospection etc.), so the generic identity row is dropped
		// here.  NOTE the name-merge below is CASE-SENSITIVE: a future
		// descriptor that declares a real lowercase `type` param on a merged
		// category would NOT merge into the live "Type" row -- it would list
		// as its own editable row alongside it (today no Material descriptor
		// declares one; only sdf3d_painter / polynomial_function2d_painter /
		// file_rasterizeroutput do, none on this merge path).
		if( g.name == String( "chunk_type" ) ) continue;

		CameraProperty* existing = nullptr;
		for( CameraProperty& r : rows )
		{
			if( r.name == g.name ) { existing = &r; break; }
		}
		if( !existing )
		{
			rows.push_back( g );   // descriptor param the live module never surfaced
			continue;
		}
		if( !existing->editable && g.editable )
		{
			// The live module could only READ this param (captured at
			// construction, no setter surface); the CST edit route can
			// WRITE it.  Upgrade in place, adopting the CST-backed value,
			// kind, unitLabel, presets, referenceCategories, and soft-range
			// metadata that the edit route round-trips through.
			existing->editable            = true;
			if( g.value.size() > 1 || std::strncmp( existing->value.c_str(), "(unregistered", 13 ) == 0 )
				existing->value           = g.value;
			existing->kind                = g.kind;
			existing->unitLabel           = g.unitLabel;
			existing->presets             = g.presets;
			existing->referenceCategories = g.referenceCategories;
			existing->rangeMin            = g.rangeMin;
			existing->rangeMax            = g.rangeMax;
			existing->rangeStep           = g.rangeStep;
			existing->hasRange            = ( g.rangeMax > g.rangeMin )
				&& IsSingleFiniteNumber( existing->value.c_str(), existing->kind == ValueKind::UInt );
			existing->targetsPainter        = g.targetsPainter;
			existing->inlinePainterKind     = g.inlinePainterKind;
			existing->inlinePainterValue    = g.inlinePainterValue;
			existing->inlinePainterRefCount = g.inlinePainterRefCount;
		}
		else if( !g.editable )
		{
			// The reverse direction, and the reason it has to exist: Inspect
			// clears `editable` only when the chunk spells this non-repeatable
			// param more than once, which the CST edit route REFUSES to write
			// (Job::ApplyCstParamEditImpl_).  A live row that looks editable
			// would still route its edit through that refusal, so the live
			// module's optimism has to yield to the CST's answer -- including
			// the value, since last-wins is what the scene derived from.
			existing->editable              = false;
			existing->value                 = g.value;
			existing->description           = g.description;
			existing->hasRange              = false;
			existing->targetsPainter        = g.targetsPainter;
			existing->inlinePainterKind     = CameraProperty::InlinePainterKind::None;
			existing->inlinePainterValue    = String();
			existing->inlinePainterRefCount = 0;
		}
		else if( existing->editable && g.editable )
		{
			if( existing->unitLabel.size() <= 1 && g.unitLabel.size() > 1 )
				existing->unitLabel = g.unitLabel;
			if( !existing->hasRange && ( g.hasRange || g.rangeMax > g.rangeMin ) )
			{
				existing->rangeMin  = g.rangeMin;
				existing->rangeMax  = g.rangeMax;
				existing->rangeStep = g.rangeStep;
				if( IsSingleFiniteNumber( existing->value.c_str(), existing->kind == ValueKind::UInt ) )
				{
					existing->hasRange = true;
				}
			}
			// Always overwrite Reference and Enum presets with `g.presets` even
			// when `g.presets` is empty: an intentionally empty CST-filtered
			// preset list (e.g. a required Material slot when no earlier-declared
			// legal substrate exists) must clear the live module's unfiltered
			// CollectPainterPresets list rather than leaving illegal candidates.
			if( g.kind == ValueKind::Reference || g.kind == ValueKind::Enum || !g.presets.empty() )
				existing->presets = g.presets;
			existing->targetsPainter        = g.targetsPainter;
			existing->inlinePainterKind     = g.inlinePainterKind;
			existing->inlinePainterValue    = g.inlinePainterValue;
			existing->inlinePainterRefCount = g.inlinePainterRefCount;
		}
		if( existing->referenceCategories.empty() && !g.referenceCategories.empty() )
			existing->referenceCategories = g.referenceCategories;
		if( !existing->targetsPainter && g.targetsPainter )
			existing->targetsPainter = true;
	}
}

void CstIntrospection::AnnotateReferenceRows(
	std::vector<CameraProperty>& rows, const RISE::Cst::Document* doc,
	const String& entityName, const char* roleKindSuffix )
{
	if( !doc || entityName.size() <= 1 || !roleKindSuffix || !roleKindSuffix[0] ) return;

	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, entityName.c_str(), nullptr, roleKindSuffix, /*uniqueFallback=*/false );
	if( id == 0 ) return;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return;
	const ChunkDescriptor* cd = DescriptorForKeyword( String( chunk->role.c_str() ) );
	if( !cd ) return;

	for( CameraProperty& r : rows )
	{
		if( r.referenceCategories.empty() )
		{
			for( const ParameterDescriptor& p : cd->parameters )
			{
				if( p.kind != ValueKind::Reference ) continue;
				if( r.name == String( p.name.c_str() ) )
				{
					r.referenceCategories = p.referenceCategories;
					break;
				}
			}
		}
		if( !r.referenceCategories.empty() && r.value != String( "(scene-file only)" ) )
		{
			r.targetsPainter = TargetsPainterCategory( r.referenceCategories );
		}
	}
}

}   // namespace RISE
