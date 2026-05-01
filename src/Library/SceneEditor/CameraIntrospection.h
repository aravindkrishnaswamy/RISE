//////////////////////////////////////////////////////////////////////
//
//  CameraIntrospection.h - Descriptor-driven introspection of an
//    ICamera for the interactive editor's properties panel.
//
//    Looks up the ChunkDescriptor that the parser uses to LOAD the
//    camera (e.g. "pinhole_camera"), then produces a list of
//    {name, kind, value-as-string, description} tuples by reading
//    the camera's current state.  The same descriptor that drives
//    parsing also drives the panel's rows — single source of truth.
//
//    Editing is done via SetProperty(): the caller hands back a
//    string for the new value and we parse + dispatch to the right
//    camera setter, then ask the camera to RegenerateData() once.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CAMERAINTROSPECTION_
#define RISE_CAMERAINTROSPECTION_

#include "../Interfaces/ICamera.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include <vector>

namespace RISE
{
	struct CameraProperty
	{
		String                       name;          // parameter name as it appears in scene files
		ValueKind                    kind;          // descriptor kind (Double / DoubleVec3 / UInt / etc.)
		String                       value;         // current value formatted as a string the user can edit
		String                       description;   // descriptor's human-readable description
		bool                         editable;      // false for fields like pixelAR that are const-bound
		// Optional quick-pick presets surfaced to the editor as a
		// combo box.  Values are parser-acceptable literals; the user
		// can still type an arbitrary value (the line-edit stays).
		std::vector<ParameterPreset> presets;
		// Optional short unit suffix shown next to the editor field
		// (e.g. "mm" for sensor_size / focal_length / shift_x/y, "°"
		// for tilt and rotation angles, "scene units" for
		// focus_distance, "" for dimensionless params like fstop).
		// The presence of a unit label lets the panel disambiguate at
		// a glance — the user-visible "35" then reads as "35 mm"
		// rather than possibly being misread as "35 metres".
		String                       unitLabel;
	};

	class CameraIntrospection
	{
	public:
		//! Returns one CameraProperty per parameter the camera's chunk
		//! descriptor declares.  The list is fully descriptor-driven so
		//! adding a parameter to the parser automatically surfaces it
		//! here.  Returns empty vector if the camera type isn't
		//! recognised.
		static std::vector<CameraProperty> Inspect( const ICamera& camera );

		//! Parse `value` according to the descriptor kind and apply to
		//! the camera.  Calls RegenerateData() on success.  Returns
		//! false on parse failure or if the parameter isn't editable.
		static bool SetProperty( ICamera& camera,
		                         const String& name,
		                         const String& value );

		//! Read the named property's current value as a formatted
		//! string (in the same form `SetProperty` accepts).  Used by
		//! the undo path to capture the prev-value before a panel edit.
		//! Returns empty string for unknown / unreadable properties.
		static String GetPropertyValue( const ICamera& camera,
		                                const String& name );

	private:
		// Returns "pinhole_camera" / "thinlens_camera" / etc., or
		// empty if the camera isn't a known type.
		static String GetDescriptorKeyword( const ICamera& camera );
	};
}

#endif
