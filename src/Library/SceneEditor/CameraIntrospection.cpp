//////////////////////////////////////////////////////////////////////
//
//  CameraIntrospection.cpp
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CameraIntrospection.h"
#include "../Cameras/CameraCommon.h"
#include "../Cameras/PinholeCamera.h"
#include "../Cameras/ThinLensCamera.h"
#include "../Cameras/FisheyeCamera.h"
#include "../Cameras/OrthographicCamera.h"
#include "../Parsers/ChunkParserRegistry.h"
#include "ChunkDescriptorRegistry.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Interfaces/ILog.h"
#include <cstdio>
#include <map>
#include <mutex>

#ifndef DEG_TO_RAD
#define DEG_TO_RAD ( 3.14159265358979323846 / 180.0 )
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG ( 180.0 / 3.14159265358979323846 )
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// DescriptorForKeyword now lives in `ChunkDescriptorRegistry.{h,cpp}`
	// so light / rasterizer / object introspection can share the same
	// cached lookup instead of each maintaining their own.

	String FormatDouble( Scalar v )
	{
		char buf[64];
		std::snprintf( buf, sizeof(buf), "%.6g", v );
		return String( buf );
	}

	String FormatVec3( const Vector3& v )
	{
		char buf[128];
		std::snprintf( buf, sizeof(buf), "%.6g %.6g %.6g", v.x, v.y, v.z );
		return String( buf );
	}

	String FormatPoint3( const Point3& p )
	{
		char buf[128];
		std::snprintf( buf, sizeof(buf), "%.6g %.6g %.6g", p.x, p.y, p.z );
		return String( buf );
	}

	String FormatUInt( unsigned int v )
	{
		char buf[32];
		std::snprintf( buf, sizeof(buf), "%u", v );
		return String( buf );
	}

	bool ParseDouble( const String& s, Scalar& out )
	{
		return std::sscanf( s.c_str(), "%lf", &out ) == 1;
	}

	bool ParseVec3( const String& s, Scalar& a, Scalar& b, Scalar& c )
	{
		return std::sscanf( s.c_str(), "%lf %lf %lf", &a, &b, &c ) == 3;
	}

	// Read one parameter's current value off `cam`.  Returns true if
	// the parameter is recognised; sets `editable` accordingly.
	bool ReadCameraProperty( const CameraCommon& cam,
	                         const String& name,
	                         CameraProperty& out )
	{
		out.name = name;
		out.editable = true;

		const std::string n( name.c_str() );

		if( n == "location" )
		{
			// Rest position (before orbit) — NOT GetLocation() which
			// includes the target_orientation rotation.  The orbit
			// tool deltas target_orientation; if "location" displayed
			// the post-orbit position, the panel would show the
			// camera's rest location apparently changing during an
			// orbit, contradicting the tool's name.
			out.kind = ValueKind::DoubleVec3;
			out.value = FormatPoint3( cam.GetRestLocation() );
			return true;
		}
		if( n == "lookat" )
		{
			out.kind = ValueKind::DoubleVec3;
			out.value = FormatPoint3( cam.GetStoredLookAt() );
			return true;
		}
		if( n == "up" )
		{
			out.kind = ValueKind::DoubleVec3;
			out.value = FormatVec3( cam.GetStoredUp() );
			return true;
		}
		if( n == "width" )
		{
			out.kind = ValueKind::UInt;
			out.value = FormatUInt( cam.GetWidth() );
			out.editable = false;   // resizing mid-render is unsafe
			return true;
		}
		if( n == "height" )
		{
			out.kind = ValueKind::UInt;
			out.value = FormatUInt( cam.GetHeight() );
			out.editable = false;
			return true;
		}
		if( n == "pixelAR" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetPixelAR() );
			out.editable = false;   // const-bound member
			return true;
		}
		if( n == "exposure" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetExposureTimeStored() );
			return true;
		}
		if( n == "scanning_rate" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetScanningRateStored() );
			return true;
		}
		if( n == "pixel_rate" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetPixelRateStored() );
			return true;
		}
		// Euler orientation is stored in radians but the parser
		// accepts/emits degrees; convert for display.
		if( n == "orientation" )
		{
			out.kind = ValueKind::DoubleVec3;
			Vector3 o = cam.GetEulerOrientation();
			Vector3 deg;
			deg.x = o.x * RAD_TO_DEG;
			deg.y = o.y * RAD_TO_DEG;
			deg.z = o.z * RAD_TO_DEG;
			out.value = FormatVec3( deg );
			return true;
		}
		if( n == "pitch" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetEulerOrientation().x * RAD_TO_DEG );
			return true;
		}
		if( n == "roll" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetEulerOrientation().y * RAD_TO_DEG );
			return true;
		}
		if( n == "yaw" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetEulerOrientation().z * RAD_TO_DEG );
			return true;
		}
		if( n == "target_orientation" )
		{
			out.kind = ValueKind::DoubleVec3;
			Vector2 t = cam.GetTargetOrientation();
			// target_orientation is Vec2 in storage but the parser
			// param is DoubleVec3; emit (theta, phi, 0) for clarity.
			Vector3 v;
			v.x = t.x * RAD_TO_DEG;
			v.y = t.y * RAD_TO_DEG;
			v.z = 0;
			out.value = FormatVec3( v );
			return true;
		}
		if( n == "theta" )
		{
			// Storage is radians; emit in degrees so the panel
			// matches the orientation row's units and so the
			// scrub-handle drag feels natural (1°/px-ish at
			// typical magnitudes).  SetProperty mirrors the
			// reverse conversion.
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetTargetOrientation().x * RAD_TO_DEG );
			return true;
		}
		if( n == "phi" )
		{
			out.kind = ValueKind::Double;
			out.value = FormatDouble( cam.GetTargetOrientation().y * RAD_TO_DEG );
			return true;
		}
		// Camera-type-specific parameters.  Pinhole's fov is in radians
		// at storage; the parser accepts degrees, so we convert on the
		// way out and the SetProperty path converts back on the way in.
		// ThinLens uses photographic params (sensor_size + focal_length
		// + fstop + focus_distance, all in mm or scene units); FOV is
		// derived and not user-editable on this camera.
		if( n == "fov" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const PinholeCamera* p = dynamic_cast<const PinholeCamera*>( &cam ) ) {
				out.value = FormatDouble( p->GetFovStored() * RAD_TO_DEG );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "sensor_size" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetSensorSize() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "focal_length" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetFocalLengthStored() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "fstop" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			// Landing 5: fstop on PinholeCamera is the photographic
			// f-number used solely for EV computation (pinhole has no
			// geometric DOF).  On ThinLensCamera the same field drives
			// DOF too — same number, two effects.
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetFstop() );
				return true;
			}
			if( const PinholeCamera* p = dynamic_cast<const PinholeCamera*>( &cam ) ) {
				out.value = FormatDouble( p->GetFstop() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "iso" )
		{
			// Landing 5: ISO sensitivity, the third leg of the
			// photographic-exposure triplet (with fstop and exposure).
			// Wired on both pinhole and thinlens; default 0 means
			// "physical exposure disabled" so the camera contributes
			// no EV.
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetIsoStored() );
				return true;
			}
			if( const PinholeCamera* p = dynamic_cast<const PinholeCamera*>( &cam ) ) {
				out.value = FormatDouble( p->GetIsoStored() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "focus_distance" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetFocusDistanceStored() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "aperture_blades" )
		{
			out.kind = ValueKind::UInt;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				char buf[32];
				std::snprintf( buf, sizeof(buf), "%u", t->GetApertureBlades() );
				out.value = String( buf );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "aperture_rotation" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetApertureRotation() * RAD_TO_DEG );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "anamorphic_squeeze" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetAnamorphicSqueeze() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		// Tilt is stored in radians; emit in degrees so the panel and
		// scene-file values agree.  Shift is stored in MM directly
		// (Phase 1.2 unit convention) — no conversion at the editor
		// surface; the camera's Recompute() does mm → scene-units
		// internally.
		if( n == "tilt_x" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetTiltX() * RAD_TO_DEG );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "tilt_y" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetTiltY() * RAD_TO_DEG );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "shift_x" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetShiftX() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "shift_y" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const ThinLensCamera* t = dynamic_cast<const ThinLensCamera*>( &cam ) ) {
				out.value = FormatDouble( t->GetShiftY() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "scale" )
		{
			out.kind = ValueKind::Double;
			out.editable = true;
			if( const FisheyeCamera* f = dynamic_cast<const FisheyeCamera*>( &cam ) ) {
				out.value = FormatDouble( f->GetScaleStored() );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		if( n == "viewport_scale" )
		{
			// Stored as Vector2 but the descriptor declares Double.
			// The parser reads the field as "x y" (two doubles); we
			// emit the same string form here so the panel round-trips
			// the user's input.
			out.kind = ValueKind::String;
			out.editable = true;
			if( const OrthographicCamera* o = dynamic_cast<const OrthographicCamera*>( &cam ) ) {
				const Vector2 v = o->GetViewportScaleStored();
				char buf[128];
				std::snprintf( buf, sizeof(buf), "%.6g %.6g", v.x, v.y );
				out.value = String( buf );
				return true;
			}
			out.value = String( "(unavailable)" );
			out.editable = false;
			return true;
		}
		// Other camera-type-specific parameters not handled above —
		// show as unavailable.  Keep the row so the descriptor list
		// is fully represented to the user.
		out.kind = ValueKind::String;
		out.value = String( "(read-only / unavailable)" );
		out.editable = false;
		return true;   // recognised the name (in descriptor list)
	}
}

// -------------------------------------------------------------------

String CameraIntrospection::GetDescriptorKeyword( const ICamera& camera )
{
	if( dynamic_cast<const PinholeCamera*>( &camera ) )      return String( "pinhole_camera" );
	if( dynamic_cast<const ThinLensCamera*>( &camera ) )     return String( "thinlens_camera" );
	if( dynamic_cast<const FisheyeCamera*>( &camera ) )      return String( "fisheye_camera" );
	if( dynamic_cast<const OrthographicCamera*>( &camera ) ) return String( "orthographic_camera" );
	return String( "" );
}

namespace
{
	// The camera descriptor declares both multi-component and
	// single-component versions of the same stored data — e.g.
	// `orientation` (vec3) is shadowed by `pitch`/`roll`/`yaw`
	// (scalars), and `target_orientation` (vec3) by `theta`/`phi`
	// (scalars).  Both forms map to the same Vector3/Vector2 in
	// the camera, so showing both in the panel is just noise; we
	// pick the form that fits the interactive workflow:
	//
	//   • target_orientation: split into theta + phi scalars.  The
	//     interactive Orbit tool drives theta and phi independently
	//     and each scrub-handle gives a fast way to dial them in.
	//
	//   • orientation: HIDDEN entirely.  Camera Euler tilt is rarely
	//     adjusted in an interactive workflow — the Orbit tool covers
	//     the typical "rotate the camera" use, and the Roll tool
	//     covers screen-axis tilt by mutating orientation.z directly.
	//     Showing the Vec3 row when the user can't usefully drag a
	//     three-tuple is just clutter.  The pitch/yaw/roll scalar
	//     shadows are also filtered, both for the same clutter
	//     reason and because the parser's "roll" → .y / "yaw" → .z
	//     naming disagrees with the math (.y is rotation around up =
	//     yaw, .z is rotation around forward = roll), so a panel
	//     "roll" row would mutate the wrong component vs. what the
	//     user expects from the Roll tool.  Scene-file authors who
	//     need to set orientation can still do so via the text
	//     editor; programmatic / animator paths are unaffected.
	bool IsRedundantParameter( const std::string& name )
	{
		return name == "pitch"
		    || name == "roll"
		    || name == "yaw"
		    || name == "orientation"
		    || name == "target_orientation";
	}

	// More informative descriptions than the parser's terse one-liners.
	// The parser descriptions are tuned for grammar-completion; the
	// panel needs to explain what a *live edit* of this field does.
	String OverrideDescription( const std::string& name, const String& fallback )
	{
		if( name == "location" )
			return String( "Eye position in world coordinates.  Pan and Zoom translate this; Orbit pivots conceptually around `lookat` via `target_orientation` (which leaves this at its rest value)." );
		if( name == "lookat" )
			return String( "Target point the camera is aimed at.  Orbit pivots around this via `target_orientation`; Pan translates it; Zoom doesn't touch it." );
		if( name == "up" )
			return String( "World up direction (typically 0 1 0).  Defines which way is up for the look-at frame." );
		if( name == "orientation" )
			return String( "Pitch / yaw / roll Euler rotation in degrees, applied ON TOP OF the look-at frame.  The Roll tool deltas the .z (roll) component — drag horizontally in the viewport.  Pitch / yaw remain hand-edits unless you want them animated." );
		if( name == "target_orientation" )
			return String( "Theta / phi (elevation / azimuth) of the camera relative to `lookat`.  The Orbit tool deltas these — drag horizontally for phi, vertically for theta.  Stored in radians; the panel surfaces them as the `theta` and `phi` rows in degrees so each can be scrubbed independently." );
		if( name == "theta" )
			return String( "Elevation angle in degrees relative to `lookat` — positive looks down at the scene, negative looks up.  Drag the chevron to scrub.  Clamped to ±89° to avoid gimbal lock." );
		if( name == "phi" )
			return String( "Azimuth angle in degrees relative to `lookat` — drag the chevron to spin the camera around the up axis.  Wraps freely past 360°." );
		if( name == "fov" )
			return String( "Field of view in degrees (full-angle, not half-angle)." );
		if( name == "exposure" )
			return String( "Shutter open time in seconds.  0 = instantaneous (no motion blur).  When `iso` > 0, this number ALSO drives photographic exposure: half it to lose 1 EV stop on the LDR PNG output (the EXR is unchanged)." );
		if( name == "iso" )
			return String( "ISO sensitivity for photographic exposure.  Default 0 = physical exposure DISABLED — no EV applied to LDR outputs.  When > 0, evCompensation = -log2(1.2) - log2(N²·100/(ISO·T)) is stacked into LDR outputs' exposure_compensation; HDR archival outputs (EXR) ignore it." );
		if( name == "fstop" )
			return String( "f-number.  On thinlens this controls BOTH DOF (aperture diameter = focal_length / fstop) and EV when iso > 0.  On pinhole there is no geometric DOF, so this is purely an EV input — set to a typical photographic value (f/2.8 - f/16)." );
		if( name == "scanning_rate" )
			return String( "Per-scanline time offset for rolling-shutter simulation.  0 disables." );
		if( name == "pixel_rate" )
			return String( "Per-pixel time offset.  0 disables." );
		if( name == "width" )
			return String( "Image width in pixels.  Editable only between renders." );
		if( name == "height" )
			return String( "Image height in pixels.  Editable only between renders." );
		if( name == "pixelAR" )
			return String( "Pixel aspect ratio.  Stored as const at camera construction." );
		return fallback;
	}
}

std::vector<CameraProperty> CameraIntrospection::Inspect( const ICamera& camera )
{
	std::vector<CameraProperty> out;

	const CameraCommon* cam = dynamic_cast<const CameraCommon*>( &camera );
	if( !cam ) return out;

	String keyword = GetDescriptorKeyword( camera );
	if( keyword.size() <= 1 ) return out;

	const ChunkDescriptor* desc = DescriptorForKeyword( keyword );
	if( !desc ) return out;

	out.reserve( desc->parameters.size() );
	for( const ParameterDescriptor& p : desc->parameters )
	{
		// Skip scalar shadows of multi-component parameters — see
		// IsRedundantParameter for the rationale.
		if( IsRedundantParameter( p.name ) ) continue;

		CameraProperty cp;
		ReadCameraProperty( *cam, String( p.name.c_str() ), cp );
		// Prefer the descriptor's declared kind over our heuristic.
		cp.kind = p.kind;
		// Replace the parser's terse description with a panel-friendly
		// one when we have a more useful explanation.
		cp.description = OverrideDescription( p.name, String( p.description.c_str() ) );
		// Forward the descriptor's quick-pick presets to the panel.
		// The panel renders these as a combo box alongside the line
		// edit when the list is non-empty (see PropertiesPanel.swift /
		// ViewportProperties.cpp).
		cp.presets = p.presets;
		// Forward the unit label.  Many CameraCommon params have no
		// per-descriptor unit declaration but ARE displayed in known
		// units after the editor's deg/rad conversions in
		// ReadCameraProperty (pitch/roll/yaw/orientation/theta/phi
		// emit degrees regardless of stored radians); supply "°" for
		// those when the descriptor itself didn't.
		cp.unitLabel = String( p.unitLabel.c_str() );
		if( cp.unitLabel.size() <= 1 ) {
			const std::string n( p.name.c_str() );
			if( n == "pitch" || n == "roll" || n == "yaw" ||
			    n == "orientation" || n == "theta" || n == "phi" ||
			    n == "target_orientation" ) {
				cp.unitLabel = String( "°" );
			}
		}
		// Resolve the generic "scene units" placeholder to a concrete
		// unit name when the camera tells us its scene_unit_meters.
		// This is what lets the panel show "5000 mm" or "5 m" rather
		// than the ambiguous "5000 scene units" — the user reads the
		// real unit at a glance.  Falls through to the generic label
		// when the scale is non-standard (e.g. cm = 0.01, inches =
		// 0.0254, feet = 0.3048; otherwise stay generic).
		if( std::string( cp.unitLabel.c_str() ) == "scene units" ) {
			if( const ThinLensCamera* tl = dynamic_cast<const ThinLensCamera*>( cam ) ) {
				const double su = tl->GetSceneUnitMeters();
				// Tolerance because the user could type "0.001000"
				// vs "0.001" — both meaning mm.
				const auto match = []( double a, double b ) {
					return fabs( a - b ) < 1e-9;
				};
				if(      match( su, 1.0    ) ) cp.unitLabel = String( "m"  );
				else if( match( su, 0.01   ) ) cp.unitLabel = String( "cm" );
				else if( match( su, 0.001  ) ) cp.unitLabel = String( "mm" );
				else if( match( su, 0.0254 ) ) cp.unitLabel = String( "in" );
				else if( match( su, 0.3048 ) ) cp.unitLabel = String( "ft" );
				// otherwise leave as the generic "scene units"
			}
		}
		out.push_back( cp );
	}
	return out;
}

String CameraIntrospection::GetPropertyValue( const ICamera& camera,
                                              const String& name )
{
	const CameraCommon* cam = dynamic_cast<const CameraCommon*>( &camera );
	if( !cam ) return String();
	CameraProperty p;
	if( !ReadCameraProperty( *cam, name, p ) ) return String();
	return p.value;
}

bool CameraIntrospection::SetProperty( ICamera& camera,
                                       const String& name,
                                       const String& value )
{
	CameraCommon* cam = dynamic_cast<CameraCommon*>( &camera );
	if( !cam ) return false;

	const std::string n( name.c_str() );

	if( n == "location" ) {
		Scalar x, y, z;
		if( !ParseVec3( value, x, y, z ) ) return false;
		cam->SetLocation( Point3( x, y, z ) );
	}
	else if( n == "lookat" ) {
		Scalar x, y, z;
		if( !ParseVec3( value, x, y, z ) ) return false;
		cam->SetLookAt( Point3( x, y, z ) );
	}
	else if( n == "up" ) {
		Scalar x, y, z;
		if( !ParseVec3( value, x, y, z ) ) return false;
		cam->SetUp( Vector3( x, y, z ) );
	}
	else if( n == "exposure" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		cam->SetExposureTimeStored( v );
	}
	else if( n == "scanning_rate" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		cam->SetScanningRateStored( v );
	}
	else if( n == "pixel_rate" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		cam->SetPixelRateStored( v );
	}
	else if( n == "orientation" ) {
		Scalar x, y, z;
		if( !ParseVec3( value, x, y, z ) ) return false;
		// degrees -> radians (matches parser convention)
		cam->SetEulerOrientation( Vector3( x * DEG_TO_RAD, y * DEG_TO_RAD, z * DEG_TO_RAD ) );
	}
	else if( n == "pitch" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		Vector3 o = cam->GetEulerOrientation();
		o.x = v * DEG_TO_RAD;
		cam->SetEulerOrientation( o );
	}
	else if( n == "roll" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		Vector3 o = cam->GetEulerOrientation();
		o.y = v * DEG_TO_RAD;
		cam->SetEulerOrientation( o );
	}
	else if( n == "yaw" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		Vector3 o = cam->GetEulerOrientation();
		o.z = v * DEG_TO_RAD;
		cam->SetEulerOrientation( o );
	}
	else if( n == "target_orientation" ) {
		Scalar x, y, z;
		if( !ParseVec3( value, x, y, z ) ) return false;
		// Stored as Vec2; only x/y are used.  Convert from degrees.
		cam->SetTargetOrientation( Vector2( x * DEG_TO_RAD, y * DEG_TO_RAD ) );
	}
	else if( n == "theta" ) {
		// Panel emits in degrees (see ReadCameraProperty); convert
		// back to radians for storage.  Apply the ±89° clamp too —
		// AdjustCameraForThetaPhi clamps symmetrically as well, but
		// keeping the clamp at the storage boundary means the panel
		// never displays an out-of-band value that diverges from
		// what the rasterizer is using.
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		static const Scalar kThetaLimit = 1.553343;  // ~89° in rad
		Scalar rad = v * DEG_TO_RAD;
		if( rad >  kThetaLimit ) rad =  kThetaLimit;
		if( rad < -kThetaLimit ) rad = -kThetaLimit;
		Vector2 t = cam->GetTargetOrientation();
		t.x = rad;
		cam->SetTargetOrientation( t );
	}
	else if( n == "phi" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		Vector2 t = cam->GetTargetOrientation();
		t.y = v * DEG_TO_RAD;
		cam->SetTargetOrientation( t );
	}
	else if( n == "fov" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		// Parser accepts degrees; storage is radians.  ThinLens
		// derives FOV from sensor_size + focal_length and does not
		// expose a direct FOV setter — those users edit the
		// photographic params instead.
		if( PinholeCamera* p = dynamic_cast<PinholeCamera*>( cam ) ) {
			p->SetFovStored( v * DEG_TO_RAD );
		} else {
			return false;
		}
	}
	else if( n == "sensor_size" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetSensorSize( v );
		} else {
			return false;
		}
	}
	else if( n == "focal_length" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetFocalLengthStored( v );
		} else {
			return false;
		}
	}
	else if( n == "fstop" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetFstop( v );
		} else if( PinholeCamera* p = dynamic_cast<PinholeCamera*>( cam ) ) {
			// Landing 5: pinhole's fstop is photographic-only (no
			// geometric DOF effect).  RegenerateData() — invoked at
			// the end of the panel's batch — picks up the new value
			// and recomputes evCompensation_.
			p->SetFstop( v );
		} else {
			return false;
		}
	}
	else if( n == "iso" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetIsoStored( v );
		} else if( PinholeCamera* p = dynamic_cast<PinholeCamera*>( cam ) ) {
			p->SetIsoStored( v );
		} else {
			return false;
		}
	}
	else if( n == "focus_distance" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetFocusDistanceStored( v );
		} else {
			return false;
		}
	}
	else if( n == "aperture_blades" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetApertureBlades( static_cast<unsigned int>( v < 0 ? 0 : v ) );
		} else {
			return false;
		}
	}
	else if( n == "aperture_rotation" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		// Editor surfaces rotation in degrees; storage is radians.
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetApertureRotation( v * DEG_TO_RAD );
		} else {
			return false;
		}
	}
	else if( n == "anamorphic_squeeze" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetAnamorphicSqueeze( v );
		} else {
			return false;
		}
	}
	else if( n == "tilt_x" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		// Editor surfaces tilt in degrees; storage is radians.
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetTiltX( v * DEG_TO_RAD );
		} else {
			return false;
		}
	}
	else if( n == "tilt_y" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetTiltY( v * DEG_TO_RAD );
		} else {
			return false;
		}
	}
	else if( n == "shift_x" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		// Shift is in MM (Phase 1.2 unit convention) — camera's
		// Recompute() does the mm → scene-units conversion.
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetShiftX( v );
		} else {
			return false;
		}
	}
	else if( n == "shift_y" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( ThinLensCamera* t = dynamic_cast<ThinLensCamera*>( cam ) ) {
			t->SetShiftY( v );
		} else {
			return false;
		}
	}
	else if( n == "scale" ) {
		Scalar v;
		if( !ParseDouble( value, v ) ) return false;
		if( FisheyeCamera* f = dynamic_cast<FisheyeCamera*>( cam ) ) {
			f->SetScaleStored( v );
		} else {
			return false;
		}
	}
	else if( n == "viewport_scale" ) {
		// Stored as Vector2; the parser reads "x y".  Accept the same
		// form here.  If the user types one number we treat it as a
		// uniform scale (x = y = v) to match the descriptor's
		// single-Double declaration.
		Scalar x = 0, y = 0;
		const int got = std::sscanf( value.c_str(), "%lf %lf", &x, &y );
		if( got < 1 ) return false;
		if( got == 1 ) y = x;
		if( OrthographicCamera* o = dynamic_cast<OrthographicCamera*>( cam ) ) {
			o->SetViewportScaleStored( Vector2( x, y ) );
		} else {
			return false;
		}
	}
	else {
		// Unknown / unsupported / read-only parameter.  Log a warning
		// so a stuck "no-op edit" in the panel is visible in logs;
		// platform UIs that care can also surface a toast based on
		// the bool return.
		GlobalLog()->PrintEx( eLog_Warning,
			"CameraIntrospection::SetProperty: ignored '%s' = '%s' (unknown, unsupported, or read-only)",
			name.c_str(), value.c_str() );
		return false;
	}

	cam->RegenerateData();
	return true;
}
