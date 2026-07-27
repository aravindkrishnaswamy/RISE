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
//    SetObjectMaterial / SetObjectShader / SetObjectGeometry /
//    SetObjectShadowFlags),
//    all going through SceneEditor::Apply for undo/redo.
//
//    Material, shader, interior_medium, and geometry are live-
//    editable references (a geometry swap rebuilds the top-level
//    acceleration on the next render — see
//    SceneEdit::OpNeedsSpatialRebuild).  The remaining construction-
//    time-only chunk params (modifier, quaternion, matrix,
//    radiance_*) are surfaced read-only — the panel lists them for
//    at-a-glance inspection but doesn't accept edits.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ObjectIntrospection.h"
#include "ChunkDescriptorRegistry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IShader.h"
#include "../Interfaces/IShaderManager.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IJob.h"
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

// Collect medium presets via the IJob enumeration callback.  Mirrors
// `CollectManagerPresets` but routes through IJob's GetMedium /
// EnumerateMediumNames pair since media live in a Job-private
// std::map<string, IMedium*> rather than a real IManager<T>.
//
// Prepends a synthetic "(none)" entry with an empty value so the
// user can clear the interior medium binding from the panel.  Apply
// translates empty-value -> ClearInteriorMedium, matching parser-
// load behaviour where `interior_medium "none"` leaves the interior
// unset.
std::vector<ParameterPreset> CollectMediumPresets( const IJob* job )
{
	std::vector<ParameterPreset> out;
	{
		// Synthetic "clear" entry, always first in the menu.  Label
		// uses a leading "—" so it can't visually collide with a
		// user-registered medium named "(none)" — the dispatch is
		// by VALUE (empty string for clear, registered name for a
		// real medium) so a name collision is functionally safe, but
		// a distinct label keeps the picker readable.
		ParameterPreset clear;
		clear.label = std::string( "— (none)" );
		clear.value = std::string();   // empty = clear
		out.push_back( clear );
	}
	if( !job ) return out;
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
	job->EnumerateMediumNames( cb );
	return out;
}

// Reverse-lookup a medium pointer to its registered name.  Empty if
// the object has no interior medium or the pointer isn't registered.
String FindMediumName( const IJob* job, const IMedium* target )
{
	if( !job || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IJob*    job;
		const IMedium* target;
		String         found;
		bool operator()( const char* const& name ) override {
			if( job->GetMedium( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.job    = job;
	cb.target = target;
	job->EnumerateMediumNames( cb );
	return cb.found;
}

// Collect geometry presets via IJob::EnumerateGeometryNames for the
// `geometry` reference dropdown (runtime geometry swap).  No synthetic
// "(none)" entry — every object requires a geometry binding.
std::vector<ParameterPreset> CollectGeometryPresets( const IJob* job )
{
	std::vector<ParameterPreset> out;
	if( !job ) return out;
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
	job->EnumerateGeometryNames( cb );
	return out;
}

// Reverse-lookup a geometry pointer to its registered name (mirror of
// FindMediumName; runtime geometry swap).
String FindGeometryName( const IJob* job, const IGeometry* target )
{
	if( !job || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IJob*      job;
		const IGeometry* target;
		String           found;
		bool operator()( const char* const& name ) override {
			if( job->GetGeometry( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.job    = job;
	cb.target = target;
	job->EnumerateGeometryNames( cb );
	return cb.found;
}

Vector3 LeastAlignedCardinal_( const Vector3& unit )
{
	const Scalar ax = std::fabs( unit.x );
	const Scalar ay = std::fabs( unit.y );
	const Scalar az = std::fabs( unit.z );
	return ax <= ay && ax <= az ? Vector3( 1, 0, 0 )
	     : ay <= az             ? Vector3( 0, 1, 0 )
	                            : Vector3( 0, 0, 1 );
}

Vector3 PerpendicularUnit_( const Vector3& unit )
{
	return Vector3Ops::Normalize( Vector3Ops::Cross( unit, LeastAlignedCardinal_( unit ) ) );
}

}  // namespace

void ObjectIntrospection::DecomposeFinalAffine(
	const Matrix4& transform, Matrix4& rotation, Matrix4& residual )
{
	const Vector3 c0( transform._00, transform._01, transform._02 );
	const Vector3 c1( transform._10, transform._11, transform._12 );
	const Vector3 c2( transform._20, transform._21, transform._22 );
	const Scalar l0 = Vector3Ops::Magnitude( c0 );
	const Scalar l1 = Vector3Ops::Magnitude( c1 );
	const Scalar l2 = Vector3Ops::Magnitude( c2 );

	Vector3 x;
	if( l0 > 0 ) {
		x = Vector3Ops::Normalize( c0 );
	} else if( l1 > 0 && l2 > 0 && Vector3Ops::Magnitude( Vector3Ops::Cross( c1, c2 ) ) > 0 ) {
		x = Vector3Ops::Normalize( Vector3Ops::Cross( c1, c2 ) );
	} else if( l1 > 0 ) {
		x = PerpendicularUnit_( Vector3Ops::Normalize( c1 ) );
	} else if( l2 > 0 ) {
		x = PerpendicularUnit_( Vector3Ops::Normalize( c2 ) );
	} else {
		x = Vector3( 1, 0, 0 );
	}

	Vector3 yRaw = c1 - x * Vector3Ops::Dot( x, c1 );
	Vector3 y;
	if( Vector3Ops::Magnitude( yRaw ) > 0 ) {
		y = Vector3Ops::Normalize( yRaw );
	} else if( l2 > 0 && Vector3Ops::Magnitude( Vector3Ops::Cross( c2, x ) ) > 0 ) {
		y = Vector3Ops::Normalize( Vector3Ops::Cross( c2, x ) );
	} else {
		y = PerpendicularUnit_( x );
	}
	const Vector3 z = Vector3Ops::Normalize( Vector3Ops::Cross( x, y ) );
	y = Vector3Ops::Normalize( Vector3Ops::Cross( z, x ) );

	rotation = Matrix4Ops::Identity();
	rotation._00 = x.x; rotation._01 = x.y; rotation._02 = x.z;
	rotation._10 = y.x; rotation._11 = y.y; rotation._12 = y.z;
	rotation._20 = z.x; rotation._21 = z.y; rotation._22 = z.z;

	Matrix4 linear = transform;
	linear._30 = linear._31 = linear._32 = 0;
	linear._03 = linear._13 = linear._23 = 0;
	linear._33 = 1;
	residual = Matrix4Ops::Inverse( rotation ) * linear;
}

Vector3 ObjectIntrospection::RotationEulerDegrees( const Matrix4& m )
{
	const Scalar r00 = m._00;
	const Scalar r10 = m._10;
	const Scalar r20 = m._20;
	const Scalar r21 = m._21;
	const Scalar r22 = m._22;
	const Scalar clampedSinY = r20 < -1 ? -1 : r20 > 1 ? 1 : r20;
	const Scalar y = std::asin( clampedSinY );
	Scalar x = 0;
	Scalar z = 0;
	if( std::fabs( clampedSinY ) < 1 ) {
		x = std::atan2( -r21, r22 );
		z = std::atan2( -r10, r00 );
	} else {
		x = clampedSinY > 0
			? std::atan2( m._01, m._11 )
			: std::atan2( -m._01, m._11 );
	}
	return Vector3( x * RAD_TO_DEG, y * RAD_TO_DEG, z * RAD_TO_DEG );
}

namespace {

// Read a descriptor parameter's current value as a parser-formatted
// string.  Returns empty for params we don't yet have a runtime
// reader for (modifier name, quaternion / matrix
// decomposition, radiance_*) — the panel surfaces those read-only
// with the descriptor's default-hint.  `interior_medium` reads back
// through the optional IJob* (reverse-lookup) when provided.
String ReadObjectParam( const String& paramName, const IObject& obj,
	const IMaterialManager* materials, const IShaderManager* shaders,
	const IJob* job )
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
		Matrix4 rotation, residual;
		ObjectIntrospection::DecomposeFinalAffine( m, rotation, residual );
		const Vector3 euler = ObjectIntrospection::RotationEulerDegrees( rotation );
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( euler.x ),
			static_cast<double>( euler.y ),
			static_cast<double>( euler.z ) );
		return String( buf );
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
	if( paramName == String( "geometry" ) ) {
		const IGeometry* geom = obj.GetGeometry();
		const String found = FindGeometryName( job, geom );
		return found.size() > 1 ? found : String();
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
	if( paramName == String( "interior_medium" ) ) {
		const IMedium* med = obj.GetInteriorMedium();
		const String found = FindMediumName( job, med );
		// Empty result = "no interior medium bound".  Return empty
		// (NOT "none") so the panel value field is blank — matching
		// how `material`/`shader` rows display the actual bound name.
		// The preset list's synthetic "(none)" entry has an empty
		// value too, so re-selecting it routes through
		// ClearInteriorMedium symmetrically.
		return found;
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
	if( paramName == "interior_medium" )  return true;
	if( paramName == "geometry" )         return true;
	return false;
}

}  // namespace

std::vector<CameraProperty> ObjectIntrospection::Inspect( const String& name, const IObject& obj,
	const IMaterialManager* materials, const IShaderManager* shaders, const IJob* job )
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

		// Add presets for material / shader / interior_medium rows
		// from their respective lookup sources.
		if( p.name == "material" )            cp.presets = CollectManagerPresets( materials );
		else if( p.name == "shader" )         cp.presets = CollectManagerPresets( shaders );
		else if( p.name == "interior_medium" ) cp.presets = CollectMediumPresets( job );
		else if( p.name == "geometry" )       cp.presets = CollectGeometryPresets( job );
		else cp.presets = p.presets;

		const String currentValue = ReadObjectParam( cp.name, obj, materials, shaders, job );
		if( IsRuntimeEditable( p.name ) ) {
			cp.value    = currentValue.size() > 1 ? currentValue : String();
			cp.editable = true;
			// Material / shader / interior_medium edits need their
			// lookup source — if absent, degrade to read-only.
			if( p.name == "material" && !materials ) cp.editable = false;
			if( p.name == "shader"   && !shaders   ) cp.editable = false;
			if( p.name == "interior_medium" && !job ) cp.editable = false;
			if( p.name == "geometry" && !job ) cp.editable = false;
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
