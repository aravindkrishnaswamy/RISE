//////////////////////////////////////////////////////////////////////
//
//  PainterIntrospection.cpp - See PainterIntrospection.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PainterIntrospection.h"
#include "CstIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Painters/ExpressionPainter.h"
#include <string>
#include <cstdio>

namespace RISE
{

namespace
{
	//! The value tokens of the `occ`-th occurrence of `pname` on `chunk`,
	//! joined EXACTLY as Cst::ParamValueAsParsed joins them (once the first
	//! `pvalue` Token is seen, every later kid's text -- Trivia and Token
	//! alike -- is appended verbatim, so `1 0.5 0` comes back with its
	//! separators intact rather than as `10.50`).
	//!
	//! Cst exports a LAST-occurrence reader (ParamValueAsParsed) and an
	//! occurrence COUNT (ParamOccurrenceCount) but no occurrence-indexed
	//! reader, and a repeatable param needs each occurrence separately --
	//! `stop` lines are an ordered list, not a last-wins scalar.  Kept
	//! local (rather than exported from Cst) because it is a pure green-node
	//! tree read with no Document state, and because the ONLY consumer is a
	//! read-only panel row: nothing pairs it with a write, so it carries
	//! none of the capture/write-lockstep obligations that make
	//! AgentReadFirstParamValue's first-occurrence semantics load-bearing.
	std::string ParamValueAtOccurrence( const RISE::Cst::NodeRef& chunk,
	                                    const std::string& pname, int occ )
	{
		if( !chunk || occ < 0 ) return std::string();
		int seen = 0;
		for( const auto& kid : chunk->kids )
		{
			if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
			if( kid->role != pname ) continue;
			if( seen++ != occ ) continue;
			std::string val;
			bool inVal = false;
			bool sawName = false;
			for( const auto& tk : kid->kids )
			{
				if( !tk ) continue;
				if( !inVal && tk->kind == RISE::Cst::NodeKind::Token
				 && tk->role == "pname" && !sawName ) { sawName = true; continue; }
				if( !inVal && tk->kind == RISE::Cst::NodeKind::Token
				 && tk->role == "pvalue" ) inVal = true;
				if( inVal ) val += tk->text;
			}
			return val;
		}
		return std::string();
	}

	std::string TrimAscii( const std::string& s )
	{
		size_t b = 0, e = s.size();
		while( b < e && ( s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n' ) ) ++b;
		while( e > b && ( s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n' ) ) --e;
		return s.substr( b, e - b );
	}

	//! The leading whitespace-delimited token of `s` -- for a `param` line
	//! (`<name> <value> [min ...]`) that is the param's own name, which is
	//! how a CST occurrence is matched to its parsed ParamSpec.  Matching by
	//! NAME rather than by index is deliberate: the two orderings agree today
	//! (BuildExpressionProgramFromChunkFields walks bag.GetRepeatable("param")
	//! in declaration order), but a name match cannot silently mis-pair a
	//! slider with the wrong knob if that ever stops being true.
	std::string FirstToken( const std::string& s )
	{
		const std::string t = TrimAscii( s );
		const size_t sp = t.find_first_of( " \t" );
		return sp == std::string::npos ? t : t.substr( 0, sp );
	}

	//! Format a Scalar the way the panel should show it -- short, no
	//! trailing-zero noise, and parseable back by the same scanner that
	//! produced it.
	std::string FormatScalar( Scalar v )
	{
		char buf[64];
		std::snprintf( buf, sizeof( buf ), "%g", (double)v );
		return std::string( buf );
	}

	//! The ExpressionParamSpec list a painter parsed at construction, for
	//! whichever pipe `painterName` resolves in.  Empty for every painter
	//! kind that is not one of the two expression forms -- which is the
	//! common case, and why this is a per-class hook rather than an
	//! interface method: `GetParamSpecs` exists only on the two classes that
	//! have specs to give, and adding it to IPainter would put a
	//! metadata accessor on 36 painters that have none.
	const std::vector<Implementation::ParamSpec>* ExpressionParamSpecsFor(
		IJobPriv& job, const String& painterName )
	{
		if( IPainterManager* pm = job.GetPainters() )
		{
			if( IPainter* p = pm->GetItem( painterName.c_str() ) )
			{
				if( const Implementation::ExpressionPainter* ep =
						dynamic_cast<const Implementation::ExpressionPainter*>( p ) )
					return &ep->GetParamSpecs();
			}
		}
		if( IScalarPainterManager* spm = job.GetScalarPainters() )
		{
			if( IScalarPainter* p = spm->GetItem( painterName.c_str() ) )
			{
				if( const Implementation::ExpressionScalarPainter* ep =
						dynamic_cast<const Implementation::ExpressionScalarPainter*>( p ) )
					return &ep->GetParamSpecs();
			}
		}
		return 0;
	}

	//! The `min`/`max`/`step`/`label` sentence appended to a `param[i]`
	//! row's description, or empty when the author declared no metadata.
	std::string ParamSpecSuffix( const Implementation::ParamSpec& spec )
	{
		std::string s;
		if( spec.hasLabel ) s += "  Label: \"" + spec.label + "\".";
		if( spec.hasMin || spec.hasMax )
		{
			s += "  Range: ";
			s += spec.hasMin ? FormatScalar( spec.min ) : std::string( "(unbounded)" );
			s += " .. ";
			s += spec.hasMax ? FormatScalar( spec.max ) : std::string( "(unbounded)" );
			s += ".";
		}
		if( spec.hasStep ) s += "  Step: " + FormatScalar( spec.step ) + ".";
		return s;
	}
}   // anonymous namespace

unsigned int PainterIntrospection::PipesFor( IJobPriv& job, const String& painterName )
{
	unsigned int mask = PipeNone;
	if( painterName.size() <= 1 ) return mask;
	if( IPainterManager* pm = job.GetPainters() )
	{
		if( pm->GetItem( painterName.c_str() ) != 0 ) mask |= PipeColour;
	}
	if( IScalarPainterManager* spm = job.GetScalarPainters() )
	{
		if( spm->GetItem( painterName.c_str() ) != 0 ) mask |= PipeScalar;
	}
	return mask;
}

bool PainterIntrospection::IsOccurrenceRowName( const String& rowName )
{
	if( rowName.size() <= 1 ) return false;
	const std::string s( rowName.c_str() );
	// "<role>[<digits>]" -- the shape Inspect's repeatable-row loop mints.  A
	// real CST param role never contains a bracket (the scene grammar's
	// parameter names are bare identifiers), so the bracket alone is a
	// sufficient and unambiguous discriminator.
	return s.find( '[' ) != std::string::npos && s[s.size()-1] == ']';
}

std::vector<CameraProperty> PainterIntrospection::Inspect(
	const RISE::Cst::Document* doc, IJobPriv& job, const String& painterName )
{
	// ---- rows 1 + 3 (header's numbering): the identity row and every
	// SINGLE-OCCURRENCE parameter come from the GENERIC descriptor+CST
	// surface -- the same one Geometry and the synthesized-object fallback
	// use, and the same descriptor the parser validates against.  Not
	// re-derived here: a second walk over the same descriptor is exactly
	// the drift CstIntrospection was factored out to prevent.
	std::vector<CameraProperty> rows =
		CstIntrospection::Inspect( doc, job, painterName, "painter", "Painter chunk keyword" );
	if( rows.empty() ) return rows;   // unresolved / no descriptor -- generic contract

	// ---- row 2: the pipe (the one genuinely live-object-sourced fact) ----
	{
		const unsigned int pipes = PipesFor( job, painterName );
		CameraProperty row;
		row.name        = String( "pipe" );
		row.kind        = ValueKind::String;
		row.editable    = false;
		if( pipes == ( PipeColour | PipeScalar ) )
			row.value = String( "colour + scalar" );
		else if( pipes == PipeColour )
			row.value = String( "colour" );
		else if( pipes == PipeScalar )
			row.value = String( "scalar" );
		else
			row.value = String( "(not registered)" );
		row.description = String(
			"Which painter manager this name is registered in.  `colour` is the IPainter pipe "
			"(reflectance / emission / tint -- spectrally uplifted through the Jakob-Hanika LUT); "
			"`scalar` is the IScalarPainter pipe (IOR / roughness / scattering -- per-wavelength "
			"magnitudes, NEVER uplifted).  A material slot accepts one or the other by physical "
			"meaning, not by resemblance.  No kind registers in both today -- `scalar_painter` is the "
			"only chunk that ever registers into the scalar pipe; `blend_painter` / `ramp_painter` have "
			"a real dual-registration, but it is colour + IFunction2D (so the same name can also be "
			"bound as a UV-domain function elsewhere), not colour + scalar." );
		// Directly after the generic surface's leading "type" row, so the
		// two identity facts read together at the top of the panel.  Index 1
		// is always valid: the empty case returned above, and the generic
		// surface's first row is unconditionally the identity row.
		rows.insert( rows.begin() + 1, row );
	}

	// ---- rows 4: one read-only row per REPEATABLE-param occurrence ----
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, painterName.c_str(), nullptr, "painter", /*uniqueFallback=*/false );
	if( id == 0 ) return rows;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return rows;
	const ChunkDescriptor* cd = DescriptorForKeyword( String( chunk->role.c_str() ) );
	if( !cd ) return rows;

	const std::vector<Implementation::ParamSpec>* specs =
		ExpressionParamSpecsFor( job, painterName );

	for( const ParameterDescriptor& p : cd->parameters )
	{
		if( !p.repeatable ) continue;   // the generic surface already emitted it
		const int n = RISE::Cst::ParamOccurrenceCount( chunk, p.name );
		for( int i = 0; i < n; ++i )
		{
			const std::string raw = TrimAscii( ParamValueAtOccurrence( chunk, p.name, i ) );

			CameraProperty row;
			{
				char nameBuf[128];
				std::snprintf( nameBuf, sizeof( nameBuf ), "%s[%d]", p.name.c_str(), i );
				row.name = String( nameBuf );
			}
			row.kind     = p.kind;
			row.value    = String( raw.c_str() );
			row.editable = false;   // see the header's editability contract
			row.unitLabel = String( p.unitLabel.c_str() );

			std::string desc = p.description;
			// ExpressionParamSpec metadata -- only `param` lines carry it,
			// and only on the two expression painter forms.
			if( specs && p.name == "param" )
			{
				const std::string pname = FirstToken( raw );
				for( std::size_t s = 0; s < specs->size(); ++s )
				{
					if( (*specs)[s].name != pname ) continue;
					desc += ParamSpecSuffix( (*specs)[s] );
					break;
				}
			}
			desc += "  [READ-ONLY in this build: `" + p.name + "` is a REPEATABLE parameter, and the "
			        "shared CST edit route addresses occurrence 0 only (SceneEdit carries no occurrence "
			        "index, so an Undo could not restore the right line).  Edit these in the scene text; "
			        "occurrence-addressed editing is a scoped follow-up.]";
			row.description = String( desc.c_str() );

			rows.push_back( row );
		}
	}

	return rows;
}

}   // namespace RISE
