//////////////////////////////////////////////////////////////////////
//
//  LightIntrospection.cpp - Descriptor-driven light introspection.
//    The list of editable rows is sourced from the chunk descriptor
//    that the parser uses to LOAD the light (`omni_light`,
//    `spot_light`, etc.) — same single source of truth that drives
//    `CameraIntrospection`.  Adding a parameter to the parser's
//    `Describe()` automatically surfaces it in the panel.
//
//    Read-back routes through the per-type virtuals on `ILight`
//    (emissionColor / emissionEnergy / emissionTarget / etc.).
//    Write-back routes through `SceneEdit::SetLightProperty` which
//    in turn calls `KeyframeFromParameters` + `SetIntermediateValue`
//    + `RegenerateData`.  A small chunk-name → keyframe-name
//    translation table covers the few cases where the parser uses
//    a different parameter name than the keyframe API ("power" ↔
//    "energy", "inner" ↔ "inner_angle", "outer" ↔ "outer_angle").
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LightIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Cst/Cst.h"                 // the `colorspace` row reads the light's own chunk
#include "../Interfaces/ILight.h"
#include "../Parsers/ChunkDescriptor.h"

#include <cstdio>
#include <string>

using namespace RISE;

namespace {

CameraProperty MakeReadOnlyRow( const char* name, const String& value, const char* description )
{
	CameraProperty p;
	p.name        = String( name );
	p.value       = value;
	p.description = String( description );
	p.kind        = ValueKind::String;
	p.editable    = false;
	return p;
}

const char* LightTypeName( ILight::LightType t )
{
	switch( t ) {
	case ILight::LightType::Point:       return "Point (Omni)";
	case ILight::LightType::Spot:        return "Spot";
	case ILight::LightType::Directional: return "Directional";
	case ILight::LightType::Ambient:     return "Ambient";
	default:                             return "(unknown type)";
	}
}

// Map the runtime light type to its chunk keyword so we can look up
// the descriptor.  The accordion's editable surface mirrors the scene
// file's keyword vocabulary for that type.
const char* KeywordForLightType( ILight::LightType t )
{
	switch( t ) {
	case ILight::LightType::Point:       return "omni_light";
	case ILight::LightType::Spot:        return "spot_light";
	case ILight::LightType::Directional: return "directional_light";
	case ILight::LightType::Ambient:     return "ambient_light";
	default:                             return "";
	}
}

// Read the current light value for a parameter that the chunk
// descriptor describes.  Returns the formatted string (matching the
// formats `KeyframeFromParameters` accepts on the way back in) or
// empty for unknown / non-readable parameter names.
// The light's authored `colorspace`, read out of its own chunk in the
// retained CST Document.  This is the ONE descriptor row that cannot be
// answered from the live ILight: the colour space is a LOAD-TIME
// interpretation, consumed by Job::Add*Light before the light exists, and
// what the light HOLDS (and what the `color` row prints) is the converted
// linear RISEPel.
//
// Review fix (2026-09-02): this used to hard-code "Rec709RGB_Linear" for
// every light, which is the language DEFAULT but a flat falsehood on the
// (large) migrated corpus -- `tools/migrate_scenes_light_colorspace.py`
// wrote `colorspace sRGB` onto 581 colour lines to preserve their look, and
// the panel claimed every one of them was linear.  Reporting the chunk's
// actual value is both honest and load-bearing for the reader: it explains
// why the `color` row above does NOT match the digits in the scene file.
//
// Falls back to the language default when there is no Document (an
// API-constructed light), when the light's chunk does not resolve, or when
// the chunk omits the line -- all three are cases where the light really
// was built with the linear reading.  LAST occurrence (ParamValueAsParsed),
// matching what the derive reads.
String ReadLightColorSpace( const RISE::Cst::Document* doc, const String& lightName )
{
	static const char* const kDefault = "Rec709RGB_Linear";
	if( !doc || lightName.size() <= 1 ) return String( kDefault );
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, lightName.c_str(), nullptr, "light", /*uniqueFallback=*/false );
	if( id == 0 ) return String( kDefault );
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return String( kDefault );
	bool present = false;
	const std::string cs = RISE::Cst::ParamValueAsParsed( chunk, "colorspace", &present );
	if( !present || cs.empty() ) return String( kDefault );
	return String( cs.c_str() );
}

String ReadLightParam( const ILight& light, const std::string& paramName )
{
	char buf[128];
	if( paramName == "power" ) {
		std::snprintf( buf, sizeof(buf), "%g", static_cast<double>( light.emissionEnergy() ) );
		return String( buf );
	}
	if( paramName == "color" ) {
		const RISEPel c = light.emissionColor();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( c.r ), static_cast<double>( c.g ), static_cast<double>( c.b ) );
		return String( buf );
	}
	if( paramName == "position" ) {
		const Point3 p = light.position();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( p.x ), static_cast<double>( p.y ), static_cast<double>( p.z ) );
		return String( buf );
	}
	if( paramName == "target" ) {
		const Point3 t = light.emissionTarget();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( t.x ), static_cast<double>( t.y ), static_cast<double>( t.z ) );
		return String( buf );
	}
	if( paramName == "direction" ) {
		const Vector3 d = light.emissionDirection();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( d.x ), static_cast<double>( d.y ), static_cast<double>( d.z ) );
		return String( buf );
	}
	if( paramName == "inner" ) {
		const double deg = static_cast<double>( light.emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
		std::snprintf( buf, sizeof(buf), "%g", deg );
		return String( buf );
	}
	if( paramName == "outer" ) {
		const double deg = static_cast<double>( light.emissionOuterAngle() ) * 180.0 / static_cast<double>( PI );
		std::snprintf( buf, sizeof(buf), "%g", deg );
		return String( buf );
	}
	if( paramName == "shootphotons" ) {
		return String( light.CanGeneratePhotons() ? "true" : "false" );
	}
	// `colorspace` is deliberately NOT answered here -- it is not light
	// state.  Inspect() fills it from the CST chunk (ReadLightColorSpace).
	return String();
}

// Whether a descriptor parameter is runtime-editable for a light of
// this type.  The chunk descriptor lists every construction-time
// param; `name` is shown as panel header and renaming would
// invalidate every object's light reference, so it stays read-only.
// `shootphotons` is editable on photon-capable types (Point / Spot)
// via `ILight::SetCanGeneratePhotons`; non-photon-capable types
// (Ambient / Directional) hard-code `CanGeneratePhotons() == false`
// and don't surface a shootphotons row in their chunk descriptors
// in the first place.
bool IsRuntimeEditable( ILight::LightType type, const std::string& paramName )
{
	if( paramName == "name" ) return false;  // shown as panel header
	// See ReadLightColorSpace's doc: a load-time interpretation, already
	// consumed by the time the light exists.  Re-interpreting the
	// already-converted RISEPel at runtime would silently re-decode it, so
	// the row reports the scene's value and stays read-only -- change the
	// scene text (or edit the colour, which converts the chunk to linear)
	// to change the interpretation.
	if( paramName == "colorspace" ) return false;
	if( paramName == "shootphotons" ) {
		return type == ILight::LightType::Point
		    || type == ILight::LightType::Spot;
	}
	return true;
}

// Override description text where the chunk's terse description
// would benefit from a panel-friendly explanation.
const char* OverrideDescription( const std::string& paramName, const char* fallback )
{
	if( paramName == "power" ) {
		return "Radiant energy multiplier.  Scales the per-channel `color` to produce emitted radiance.";
	}
	if( paramName == "inner" ) {
		return "Inner cone full-angle (degrees).  Full intensity within this cone.";
	}
	if( paramName == "outer" ) {
		return "Outer cone full-angle (degrees).  Falls off from inner to zero at this angle.";
	}
	if( paramName == "colorspace" ) {
		return "READ-ONLY.  How the scene file's `color` triple was interpreted when the light was built.  "
		       "The `color` row above shows the CONVERTED linear value, so on a `colorspace sRGB` chunk the "
		       "two deliberately disagree.  Editing `color` rewrites this to `Rec709RGB_Linear`.";
	}
	return fallback;
}

}  // namespace

std::vector<CameraProperty> LightIntrospection::Inspect(
	const String& name, const ILight& light, const RISE::Cst::Document* doc )
{
	std::vector<CameraProperty> rows;
	const ILight::LightType type = light.lightType();

	rows.push_back( MakeReadOnlyRow(
		"Name", name,
		"The light's manager-registered name (matches the chunk's first arg in the .RISEscene file)." ) );

	rows.push_back( MakeReadOnlyRow(
		"Type", String( LightTypeName( type ) ),
		"Concrete light kind.  Each kind shows different controls below." ) );

	const char* keyword = KeywordForLightType( type );
	const ChunkDescriptor* desc = ( keyword && *keyword ) ? DescriptorForKeyword( String( keyword ) ) : 0;
	if( !desc ) {
		// Unknown / out-of-tree light type.  Surface the common
		// fallback rows so something useful still shows up.
		rows.push_back( MakeReadOnlyRow(
			"Status", String( "(no descriptor)" ),
			"Light's chunk keyword is not registered with the parser.  Out-of-tree light types fall back to a minimal row set." ) );
		return rows;
	}

	// Iterate the descriptor's parameters in declaration order — that's
	// the natural display order, matching what users see in their scene
	// files.
	for( const ParameterDescriptor& p : desc->parameters ) {
		if( p.name == "name" ) continue;  // panel header carries this

		// Skip per-axis Eulers / quaternion shadows of the spotlight
		// `position` (none today, but defensive).  Kept aligned with
		// CameraIntrospection's `IsRedundantParameter` style.

		CameraProperty cp;
		cp.name        = String( p.name.c_str() );
		cp.kind        = p.kind;
		cp.description = String( OverrideDescription( p.name, p.description.c_str() ) );
		cp.value       = ( p.name == "colorspace" )
			? ReadLightColorSpace( doc, name )     // scene text, not light state -- see its doc
			: ReadLightParam( light, p.name );
		cp.editable    = IsRuntimeEditable( type, p.name );
		cp.presets     = p.presets;
		cp.unitLabel   = String( p.unitLabel.c_str() );
		// Add a "°" unit hint for the spot-light angles even when the
		// descriptor doesn't declare one.  The values displayed are
		// degrees (matches what `inner`/`outer` accept on input).
		if( cp.unitLabel.size() <= 1 && ( p.name == "inner" || p.name == "outer" ) ) {
			cp.unitLabel = String( "°" );
		}
		rows.push_back( cp );
	}

	// `shootphotons` is surfaced by the descriptor loop above as an
	// editable bool row when the type is Point / Spot, so there is no
	// separate "Photons" summary footer.  Pre-Phase-1 the footer
	// duplicated the row info as a read-only "yes/no" line; after the
	// runtime setter landed, the descriptor row IS the source of
	// truth.

	return rows;
}
