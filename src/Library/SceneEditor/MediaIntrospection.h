//////////////////////////////////////////////////////////////////////
//
//  MediaIntrospection.h - Read-back + edit dispatch for participating
//    media (IMedium) in the interactive scene editor.
//
//    Mirrors the LightIntrospection / MaterialIntrospection pattern.
//    Currently exposes editable absorption / scattering / emission
//    for HomogeneousMedium; HeterogeneousMedium is surfaced read-only
//    because its volume data, bounding box, and majorant grid are
//    baked at construction (changing any of those without rebuilding
//    the medium would desync the delta-tracking majorant).
//
//    Slot names match the parser chunk parameter names (absorption,
//    scattering, emission) per the SceneEdit.h:193 contract — direct
//    scene-editor API callers using those names work end-to-end.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_MEDIAINTROSPECTION_
#define RISE_MEDIAINTROSPECTION_

#include "../Utilities/RString.h"
#include "../Utilities/Color/Color.h"
#include "CameraIntrospection.h"   // CameraProperty re-used as the panel-row struct
#include <vector>

namespace RISE
{
	class IMedium;

	//! Slot value type returned by `GetSlotValue` / accepted by
	//! `SetSlotValue`.  Today every editable medium slot is a vec3
	//! (absorption / scattering / emission); kept as a struct so a
	//! future scalar / phase-function slot can be added without
	//! changing the call sites.
	struct MediumSlotValue
	{
		enum Kind { None, Vec3 };
		Kind   kind = None;
		double v3[3] = { 0, 0, 0 };
	};

	class MediaIntrospection
	{
	public:
		//! Build the panel row set for `medium`.  Returns one row per
		//! displayed property (Name, Type, then the per-type slots).
		//! `editable` is true only on slots the dispatch path actually
		//! mutates — Heterogeneous medium rows are all read-only.
		static std::vector<CameraProperty> Inspect(
			const String& name,
			const IMedium& medium );

		//! Read the current value of a slot.  Returns kind=None for
		//! unknown slot or unsupported medium type.
		static MediumSlotValue GetSlotValue(
			const IMedium& medium,
			const String& slotName );

		//! Mutate a slot.  Returns true if the slot was recognised AND
		//! the value type matched.  Returns false for unknown slot,
		//! wrong-type value, or unsupported (e.g. heterogeneous)
		//! medium type.  Caller is responsible for the cancel-and-park
		//! gate against the render thread — setters can re-derive
		//! cached state (e.g. HomogeneousMedium's sigma_t / sigma_t_max
		//! after SetAbsorption / SetScattering).
		static bool SetSlotValue(
			IMedium& medium,
			const String& slotName,
			const MediumSlotValue& value );
	};
}

#endif
