//////////////////////////////////////////////////////////////////////
//
//  ObjectIntrospection.cpp - Descriptor-driven object introspection.
//    Sources panel rows from the `standard_object` chunk descriptor —
//    same single source of truth `CameraIntrospection`,
//    `LightIntrospection`, and `RasterizerIntrospection` use.  Adding
//    a parameter to the parser's `Describe()` automatically surfaces
//    it here.
//
//    Read-back routes through IObject virtuals (GetMaterial,
//    GetShader, GetFinalTransformMatrix, DoesCastShadows,
//    DoesReceiveShadows).  Write-back routes through SceneEdit ops
//    (SetObjectPosition / Orientation / Stretch / Scale /
//    SetObjectMaterial / SetObjectShader / SetObjectShadowFlags),
//    all going through SceneEditor::Apply for undo/redo.
//
//    A small subset of construction-time-only chunk params (geometry,
//    modifier, quaternion, matrix, interior_medium, radiance_*) are
//    surfaced read-only — the panel still lists them for at-a-glance
//    inspection but doesn't accept edits.  Phase 5 will add runtime
//    setters for those that have meaningful runtime mutation paths.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ObjectIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IShader.h"
#include "../Interfaces/IShaderManager.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Parsers/ChunkDescriptor.h"

#include <cmath>
#include <cstdio>
#include <set>
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

template <class MgrT>
std::vector<ParameterPreset> CollectManagerPresets( const MgrT* mgr )
{
	std::vector<ParameterPreset> out;
	if( !mgr ) return out;
	struct Cb : public IEnumCallback<const char*> {
		std::vector<ParameterPreset>* out;
		bool operator()( const char* const& name ) override {
			if( name ) {
				ParameterPreset p;
				p.label = std::string( name );
				p.value = std::string( name );
				out->push_back( p );
			}
			return true;
		}
	};
	Cb cb;
	cb.out = &out;
	const_cast<MgrT*>( mgr )->EnumerateItemNames( cb );
	return out;
}

template <class MgrT, class ItemT>
String FindManagerName( const MgrT* mgr, const ItemT* target )
{
	if( !mgr || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const MgrT* mgr;
		const ItemT* target;
		String found;
		bool operator()( const char* const& name ) override {
			if( const_cast<MgrT*>(mgr)->GetItem( name ) == target ) {
				found = String( name );
				return false;
			}
			return true;
		}
	};
	Cb cb;
	cb.mgr    = mgr;
	cb.target = target;
	const_cast<MgrT*>( mgr )->EnumerateItemNames( cb );
	return cb.found;
}

// Read a descriptor parameter's current value as a parser-formatted
// string.  Returns empty for params we don't yet have a runtime
// reader for (geometry name, modifier name, quaternion / matrix
// decomposition, interior_medium, radiance_*) — the panel surfaces
// those read-only with the descriptor's default-hint.
String ReadObjectParam( const String& paramName, const IObject& obj,
	const IMaterialManager* materials, const IShaderManager* shaders )
{
	const Matrix4 m = obj.GetFinalTransformMatrix();
	char buf[256];

	if( paramName == String( "position" ) ) {
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( m._30 ),
			static_cast<double>( m._31 ),
			static_cast<double>( m._32 ) );
		return String( buf );
	}
	if( paramName == String( "orientation" ) ) {
		// We don't have a stored Euler-decomposition accessor on
		// IObject so display "0 0 0" — user's edit overrides.
		// Phase 5 can add proper decomposition for round-trip display.
		return String( "0 0 0" );
	}
	if( paramName == String( "scale" ) ) {
		const Scalar lx = std::sqrt( m._00 * m._00 + m._01 * m._01 + m._02 * m._02 );
		const Scalar ly = std::sqrt( m._10 * m._10 + m._11 * m._11 + m._12 * m._12 );
		const Scalar lz = std::sqrt( m._20 * m._20 + m._21 * m._21 + m._22 * m._22 );
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( lx ),
			static_cast<double>( ly ),
			static_cast<double>( lz ) );
		return String( buf );
	}
	if( paramName == String( "material" ) ) {
		const IMaterial* mat = obj.GetMaterial();
		const String found = FindManagerName( materials, mat );
		return found.size() > 1 ? found : String( "none" );
	}
	if( paramName == String( "shader" ) ) {
		const IShader* sh = obj.GetShader();
		const String found = FindManagerName( shaders, sh );
		return found.size() > 1 ? found : String( "none" );
	}
	if( paramName == String( "casts_shadows" ) ) {
		return String( obj.DoesCastShadows() ? "true" : "false" );
	}
	if( paramName == String( "receives_shadows" ) ) {
		return String( obj.DoesReceiveShadows() ? "true" : "false" );
	}
	return String();
}

// Whether the introspection layer has a writer (via the
// SceneEditController::SetProperty Object branch).  Construction-time-
// only descriptor params are surfaced read-only so the user can SEE
// them at a glance but the panel doesn't accept edits.
bool IsRuntimeEditable( const std::string& paramName )
{
	if( paramName == "position" )         return true;
	if( paramName == "orientation" )      return true;
	if( paramName == "scale" )            return true;   // mapped to SetObjectStretch (vec3)
	if( paramName == "material" )         return true;
	if( paramName == "shader" )           return true;
	if( paramName == "casts_shadows" )    return true;
	if( paramName == "receives_shadows" ) return true;
	return false;
}

}  // namespace

std::vector<CameraProperty> ObjectIntrospection::Inspect( const String& name, const IObject& obj,
	const IMaterialManager* materials, const IShaderManager* shaders )
{
	std::vector<CameraProperty> rows;

	rows.push_back( MakeReadOnlyRow(
		"Name", name,
		"The object's manager-registered name (matches the chunk's first arg in the .RISEscene file)." ) );

	// Look up the standard_object descriptor.  If/when we add per-
	// type object discriminators (csg_object, etc.) the keyword
	// selection branches here.
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "standard_object" ) );
	if( !desc ) {
		rows.push_back( MakeReadOnlyRow(
			"Status", String( "(no descriptor)" ),
			"standard_object chunk keyword is not registered with the parser." ) );
		return rows;
	}

	// Skip duplicated transform shadows: the descriptor lists
	// `quaternion` and `matrix` as alternatives to `orientation`,
	// but they're construction-time-only writes.  Surface them
	// read-only with a "(scene-file only)" placeholder.
	for( const ParameterDescriptor& p : desc->parameters ) {
		if( p.name == "name" ) continue;   // panel header carries this

		CameraProperty cp;
		cp.name        = String( p.name.c_str() );
		cp.kind        = p.kind;
		cp.description = String( p.description.c_str() );
		cp.unitLabel   = String( p.unitLabel.c_str() );

		// Add presets for material / shader rows from their managers.
		if( p.name == "material" )  cp.presets = CollectManagerPresets( materials );
		else if( p.name == "shader" ) cp.presets = CollectManagerPresets( shaders );
		else cp.presets = p.presets;

		const String currentValue = ReadObjectParam( cp.name, obj, materials, shaders );
		if( IsRuntimeEditable( p.name ) ) {
			cp.value    = currentValue.size() > 1 ? currentValue : String();
			cp.editable = true;
			// Material / shader edits need a manager — if absent,
			// degrade to read-only.
			if( p.name == "material" && !materials ) cp.editable = false;
			if( p.name == "shader"   && !shaders   ) cp.editable = false;
		} else {
			cp.value    = currentValue.size() > 1
				? currentValue
				: ( p.defaultValueHint.empty()
				    ? String( "(scene-file only)" )
				    : String( p.defaultValueHint.c_str() ) );
			cp.editable = false;
		}
		rows.push_back( cp );
	}

	// Read-only bounding-box footer — useful for verification.
	{
		BoundingBox bb = obj.getBoundingBox();
		char buf[256];
		std::snprintf( buf, sizeof(buf), "%g %g %g  →  %g %g %g",
		              static_cast<double>( bb.ll.x ),
		              static_cast<double>( bb.ll.y ),
		              static_cast<double>( bb.ll.z ),
		              static_cast<double>( bb.ur.x ),
		              static_cast<double>( bb.ur.y ),
		              static_cast<double>( bb.ur.z ) );
		rows.push_back( MakeReadOnlyRow(
			"Bounds", String( buf ),
			"Axis-aligned bounding box (lower-left → upper-right) in world space." ) );
	}

	return rows;
}
