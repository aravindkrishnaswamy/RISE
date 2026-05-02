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
	return String();
}

// Whether a descriptor parameter is runtime-editable for a light of
// this type.  The chunk descriptor lists every construction-time
// param, but a few (name, shootphotons) don't have a runtime setter
// and are surfaced read-only in the panel.
bool IsRuntimeEditable( ILight::LightType /*type*/, const std::string& paramName )
{
	if( paramName == "name" )         return false;  // shown as panel header
	if( paramName == "shootphotons" ) return false;  // no runtime SetCanGeneratePhotons API yet
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
	return fallback;
}

}  // namespace

std::vector<CameraProperty> LightIntrospection::Inspect( const String& name, const ILight& light )
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
		cp.value       = ReadLightParam( light, p.name );
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

	// Read-only metadata footer — only emit for light kinds where
	// photon emission is a meaningful concept (point + spot).  The
	// other kinds default `CanGeneratePhotons` to false; surfacing
	// the row for them would imply a settable property that
	// effectively isn't.
	if( type == ILight::LightType::Point || type == ILight::LightType::Spot ) {
		rows.push_back( MakeReadOnlyRow(
			"Photons", String( light.CanGeneratePhotons() ? "yes" : "no" ),
			"Whether the light emits photons for the photon-mapping passes (set via the scene file's `shootphotons` param)." ) );
	}

	return rows;
}
