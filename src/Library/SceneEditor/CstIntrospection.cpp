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
#include "../Interfaces/IGeometryManager.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IFunction1DManager.h"
#include "../Interfaces/IFunction2DManager.h"
#include "../Interfaces/IScenePriv.h"
#include <algorithm>
#include <string>

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
		row.editable     = false;
		out.push_back( row );
	}
	if( !cd ) return out;

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
			// Live pick-from-what-exists presets: the union of candidate
			// names across the declared target categories.  Falls back to
			// the descriptor's static presets when nothing enumerates
			// (e.g. a Shader reference).
			std::vector<String> cands;
			for( ChunkCategory rc : p.referenceCategories )
			{
				const std::vector<String> names = CandidateNamesForChunkCategory( job, rc );
				for( const String& n : names )
				{
					bool dup = false;
					for( const String& e : cands ) { if( e == n ) { dup = true; break; } }
					if( !dup ) cands.push_back( n );
				}
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
		if( g.name == String( "chunk_type" ) ) continue;   // live modules carry their own identity rows

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
			// WRITE it.  Upgrade in place, adopting the CST-backed value
			// the edit route round-trips through.
			existing->editable = true;
			existing->value    = g.value;
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
			existing->editable   = false;
			existing->value      = g.value;
			existing->description = g.description;
		}
		if( existing->referenceCategories.empty() && !g.referenceCategories.empty() )
			existing->referenceCategories = g.referenceCategories;
		if( existing->presets.empty() && !g.presets.empty() )
			existing->presets = g.presets;
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
		if( !r.referenceCategories.empty() ) continue;
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
}

}   // namespace RISE
