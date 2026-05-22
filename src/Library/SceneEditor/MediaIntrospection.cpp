//////////////////////////////////////////////////////////////////////
//
//  MediaIntrospection.cpp - Per-type panel row builder + slot
//    get/set dispatch for participating media.
//
//    HomogeneousMedium is fully editable (absorption / scattering /
//    emission as vec3 rows).  HeterogeneousMedium is surfaced
//    read-only with an explanatory note: changing its max-coefficient
//    bounds without rebuilding the majorant grid would desync delta
//    tracking, and the volume data + bbox were baked at construction.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MediaIntrospection.h"
#include "../Interfaces/IMedium.h"
#include "../Materials/HomogeneousMedium.h"
#include "../Materials/HeterogeneousMedium.h"
#include <cstdio>

namespace RISE
{

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

CameraProperty MakeVec3Row( const char* name, const RISEPel& c, const char* description, bool editable )
{
	char buf[128];
	std::snprintf( buf, sizeof(buf), "%g %g %g",
		static_cast<double>( c.r ),
		static_cast<double>( c.g ),
		static_cast<double>( c.b ) );
	CameraProperty p;
	p.name        = String( name );
	p.value       = String( buf );
	p.description = String( description );
	p.kind        = ValueKind::DoubleVec3;
	p.editable    = editable;
	return p;
}

const char* MediumTypeName( const IMedium& m )
{
	if( dynamic_cast<const HomogeneousMedium*>( &m ) )   return "Homogeneous";
	if( dynamic_cast<const HeterogeneousMedium*>( &m ) ) return "Heterogeneous";
	return "(unknown type)";
}

}  // namespace

std::vector<CameraProperty> MediaIntrospection::Inspect(
	const String& name, const IMedium& medium )
{
	std::vector<CameraProperty> rows;

	rows.push_back( MakeReadOnlyRow(
		"Name", name,
		"The medium's manager-registered name (matches the chunk's first arg in the .RISEscene file)." ) );

	rows.push_back( MakeReadOnlyRow(
		"Type", String( MediumTypeName( medium ) ),
		"Concrete medium kind.  Each kind has its own set of editable / frozen fields." ) );

	if( const HomogeneousMedium* hom = dynamic_cast<const HomogeneousMedium*>( &medium ) ) {
		rows.push_back( MakeVec3Row( "absorption", hom->GetAbsorption(),
			"Absorption coefficient σ_a per channel [1/m].  Updates the cached extinction "
			"σ_t = σ_a + σ_s and the channel-max majorant used by free-flight sampling.",
			true ) );
		rows.push_back( MakeVec3Row( "scattering", hom->GetScattering(),
			"Scattering coefficient σ_s per channel [1/m].  Together with absorption, drives "
			"Beer-Lambert transmittance and exponential free-flight sampling.",
			true ) );
		rows.push_back( MakeVec3Row( "emission", hom->GetEmission(),
			"Volumetric emission per channel.  Contributes radiance independent of σ_t — "
			"usually zero unless modelling a glowing volume.  Read-only from the panel: "
			"the `homogeneous_medium` scene chunk has no `emission` parameter, so an edit "
			"could not be saved back to the .RISEscene file (use `heterogeneous_medium`, "
			"which does author emission, if you need a glowing volume).",
			false ) );
		rows.push_back( MakeReadOnlyRow(
			"phase", String( "(set at construction)" ),
			"Phase function (Isotropic or Henyey-Greenstein with g).  Rebinding the phase "
			"function from the panel is out of scope — recreate the medium chunk if you need "
			"a different scattering distribution." ) );
	}
	else if( dynamic_cast<const HeterogeneousMedium*>( &medium ) ) {
		rows.push_back( MakeReadOnlyRow(
			"params", String( "baked at construction" ),
			"Heterogeneous media bake the max-coefficient bounds, volume dataset, accessor, "
			"and majorant grid at construction time — changing any of those without rebuilding "
			"the majorant grid would desync delta tracking.  Recreate the medium chunk to "
			"change these values." ) );
	}

	return rows;
}

MediumSlotValue MediaIntrospection::GetSlotValue(
	const IMedium& medium, const String& slotName )
{
	MediumSlotValue out;
	if( const HomogeneousMedium* hom = dynamic_cast<const HomogeneousMedium*>( &medium ) ) {
		const RISEPel* src = nullptr;
		if( slotName == String( "absorption" ) ) src = &hom->GetAbsorption();
		else if( slotName == String( "scattering" ) ) src = &hom->GetScattering();
		else if( slotName == String( "emission" ) )   src = &hom->GetEmission();
		if( src ) {
			out.kind = MediumSlotValue::Vec3;
			out.v3[0] = static_cast<double>( src->r );
			out.v3[1] = static_cast<double>( src->g );
			out.v3[2] = static_cast<double>( src->b );
		}
	}
	return out;
}

bool MediaIntrospection::SetSlotValue(
	IMedium& medium, const String& slotName, const MediumSlotValue& value )
{
	if( value.kind != MediumSlotValue::Vec3 ) return false;
	if( HomogeneousMedium* hom = dynamic_cast<HomogeneousMedium*>( &medium ) ) {
		const RISEPel v( value.v3[0], value.v3[1], value.v3[2] );
		if( slotName == String( "absorption" ) ) { hom->SetAbsorption( v ); return true; }
		if( slotName == String( "scattering" ) ) { hom->SetScattering( v ); return true; }
		if( slotName == String( "emission" ) )   { hom->SetEmission( v );   return true; }
	}
	return false;
}

}  // namespace RISE
