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
	//! metadata accessor on every other painter kind that has none.
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

bool PainterIntrospection::ParseOccurrenceRowName(
	const String& rowName, std::string& outRole, int& outOccurrence )
{
	if( rowName.size() <= 1 ) return false;
	const std::string s( rowName.c_str() );
	const std::size_t lb = s.find( '[' );
	if( lb == std::string::npos || lb == 0 ) return false;   // no bracket, or an empty role
	if( s.empty() || s[s.size()-1] != ']' ) return false;    // must END at the bracket -- no trailing text
	const std::size_t digitsBegin = lb + 1;
	const std::size_t digitsEnd   = s.size() - 1;            // index of ']'
	if( digitsEnd <= digitsBegin ) return false;             // "role[]"
	// DECIMAL DIGITS ONLY.  No sign (a negative occurrence is meaningless and
	// would sail past a `< count` bound), no whitespace, no second bracket, and
	// no trailing junk -- std::atoi would happily read "2junk" as 2 and route
	// the edit to a line the user never named.
	unsigned long long acc = 0;
	for( std::size_t i = digitsBegin; i < digitsEnd; ++i ) {
		const char c = s[i];
		if( c < '0' || c > '9' ) return false;
		acc = acc * 10ull + (unsigned long long)( c - '0' );
		if( acc > 1000000ull ) return false;                 // absurd index; refuse rather than wrap
	}
	outRole       = s.substr( 0, lb );
	outOccurrence = (int)acc;
	return true;
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
		// Directly after the generic surface's leading "chunk_type" row, so the
		// two identity facts read together at the top of the panel.  Index 1
		// is always valid: the empty case returned above, and the generic
		// surface's first row is unconditionally the identity row.
		rows.insert( rows.begin() + 1, row );
	}

	// ---- rows 4: one EDITABLE row per REPEATABLE-param occurrence ----
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
			const std::string raw = TrimAscii(
				RISE::Cst::ParamValueAtOccurrence( chunk, p.name, i ) );

			CameraProperty row;
			{
				char nameBuf[128];
				std::snprintf( nameBuf, sizeof( nameBuf ), "%s[%d]", p.name.c_str(), i );
				row.name = String( nameBuf );
			}
			row.kind     = p.kind;
			row.value    = String( raw.c_str() );
			// doc 88 S4b: EDITABLE.  A write to this row name is parsed back into
			// (role, occurrence) by SceneEditController's Painter arm and routed
			// with that `occ` through the same checked CST pathway every other
			// painter param uses -- see the header's editability contract.
			row.editable = true;
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
					const Implementation::ParamSpec& spec = (*specs)[s];
					// S4b: the range as FIRST-CLASS row fields, so a shell can
					// draw a slider.  BOTH bounds required -- a half-open range
					// has no slider track, and inventing the missing end would
					// clamp values the language accepts.  `step` rides along
					// independently (0 = continuous).
					if( spec.hasMin && spec.hasMax && spec.max > spec.min ) {
						row.hasRange  = true;
						row.rangeMin  = spec.min;
						row.rangeMax  = spec.max;
						row.rangeStep = spec.hasStep ? spec.step : Scalar( 0 );
					}
					// The description fold STAYS even though the fields now
					// carry the same numbers: it is the only surface a shell
					// that does not (yet) read the range fields shows -- the Qt
					// panel today, and every text/AX rendering of a row -- and
					// it also carries `label`, which has no field of its own.
					desc += ParamSpecSuffix( spec );
					break;
				}
			}
			desc += "  [Occurrence " + std::to_string( i ) + " of the REPEATABLE parameter `" + p.name +
			        "`.  Editing this row rewrites THAT line only; the value is the WHOLE line after the "
			        "parameter name, so keep every token (an expression `param` line keeps its own name "
			        "first, then the value, then any min/max/step/label metadata).]";
			row.description = String( desc.c_str() );

			rows.push_back( row );
		}
	}

	return rows;
}

}   // namespace RISE
