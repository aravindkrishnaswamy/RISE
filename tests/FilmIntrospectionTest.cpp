//////////////////////////////////////////////////////////////////////
//
//  FilmIntrospectionTest.cpp - Phase G unit tests for the descriptor-
//    driven Film introspection wiring used by the GUI editors'
//    "Output Settings" panel.
//
//    Verifies:
//      1. Inspect() returns the 3 rows the `film` chunk descriptor
//         declares (width / height / pixelAR), populated from the
//         active IFilm's current values.
//      2. SetProperty() with each name parses + commits via
//         Job::SetFilm, which resyncs every camera in lockstep.
//      3. Invalid values are rejected without mutating Film state
//         (zero / negative width, non-positive pixelAR, unknown name,
//         unparseable string).
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/FilmIntrospection.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// RAII wrapper for the refcounted Job — destructor is protected so
// Job can't sit on the stack.  This keeps each test body tidy without
// scattering release() calls.
struct JobHandle {
	Job* p;
	JobHandle() : p( new Job() ) {}
	~JobHandle() { if( p ) p->release(); }
	Job& operator*() { return *p; }
	Job* operator->() { return p; }
	JobHandle( const JobHandle& ) = delete;
	JobHandle& operator=( const JobHandle& ) = delete;
};

static std::string FindRowValue(
	const std::vector<CameraProperty>& rows,
	const char* name )
{
	for( const auto& r : rows ) {
		if( std::strcmp( r.name.c_str(), name ) == 0 ) {
			return std::string( r.value.c_str() );
		}
	}
	return std::string();
}

static bool RowExists(
	const std::vector<CameraProperty>& rows,
	const char* name )
{
	for( const auto& r : rows ) {
		if( std::strcmp( r.name.c_str(), name ) == 0 ) return true;
	}
	return false;
}

//////////////////////////////////////////////////////////////////////
// 1. Inspect surfaces exactly the descriptor's parameters.
//////////////////////////////////////////////////////////////////////

static void TestInspectDescriptorDriven()
{
	std::cout << "Testing FilmIntrospection::Inspect descriptor coverage..." << std::endl;
	JobHandle job;
	const IFilm* film = job->GetScene()->GetFilm();
	Check( film != nullptr, "default Film exists post-InitializeContainers" );

	std::vector<CameraProperty> rows = FilmIntrospection::Inspect( *film );

	// The film chunk descriptor declares exactly three parameters:
	// width, height, pixelAR.  Phase G's panel should surface all three.
	Check( rows.size() == 3,                    "three property rows" );
	Check( RowExists( rows, "width" ),          "width row present" );
	Check( RowExists( rows, "height" ),         "height row present" );
	Check( RowExists( rows, "pixelAR" ),        "pixelAR row present" );

	// Each row marked editable (Film dims are runtime-editable).
	for( const auto& r : rows ) {
		Check( r.editable, "row marked editable" );
	}

	// Values reflect the InitializeContainers default qHD Film.
	Check( FindRowValue( rows, "width" )  == "960", "default width = 960" );
	Check( FindRowValue( rows, "height" ) == "540", "default height = 540" );
	Check( FindRowValue( rows, "pixelAR" )== "1",   "default pixelAR = 1" );
}

//////////////////////////////////////////////////////////////////////
// 2. SetProperty round-trip on each editable name.
//////////////////////////////////////////////////////////////////////

static void TestSetPropertyWidthRoundtrip()
{
	std::cout << "Testing SetProperty(width) updates Film + cameras..." << std::endl;
	JobHandle job;

	const double loc[3]    = {0,0,5};
	const double lookat[3] = {0,0,0};
	const double up[3]     = {0,1,0};
	const double orient[3] = {0,0,0};
	const double tgt[2]    = {0,0};
	Check( job->AddPinholeCamera( "cam", loc, lookat, up,
		Scalar( 0.785398 ), 0, 0, 0, orient, tgt ),
		"AddPinholeCamera succeeds" );

	// Edit width via the introspection layer.
	const bool ok = FilmIntrospection::SetProperty( *job, String("width"), String("1280") );
	Check( ok, "SetProperty(width=1280) succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film && film->GetWidth() == 1280,  "Film width == 1280" );
	Check( film && film->GetHeight() == 540,  "Film height preserved (540)" );

	// Resync invariant: the camera's projection cache matches the new Film.
	const ICamera* pCam = job->GetScene()->GetCameras()->GetItem( "cam" );
	const CameraCommon* cc = dynamic_cast<const CameraCommon*>( pCam );
	Check( cc != nullptr, "camera is CameraCommon" );
	Check( cc && cc->GetWidth()  == 1280, "camera resynced to width=1280" );
	Check( cc && cc->GetHeight() == 540,  "camera height preserved (540)" );
}

static void TestSetPropertyHeightRoundtrip()
{
	std::cout << "Testing SetProperty(height) updates Film + cameras..." << std::endl;
	JobHandle job;
	Check( FilmIntrospection::SetProperty( *job, String("height"), String("720") ),
		"SetProperty(height=720) succeeds" );
	const IFilm* film = job->GetScene()->GetFilm();
	Check( film && film->GetHeight() == 720, "Film height == 720" );
	Check( film && film->GetWidth()  == 960, "Film width preserved (960)" );
}

static void TestSetPropertyPixelARRoundtrip()
{
	std::cout << "Testing SetProperty(pixelAR) updates Film + cameras..." << std::endl;
	JobHandle job;

	const double loc[3]    = {0,0,5};
	const double lookat[3] = {0,0,0};
	const double up[3]     = {0,1,0};
	const double orient[3] = {0,0,0};
	const double tgt[2]    = {0,0};
	Check( job->AddPinholeCamera( "cam", loc, lookat, up,
		Scalar( 0.785398 ), 0, 0, 0, orient, tgt ),
		"AddPinholeCamera succeeds" );

	Check( FilmIntrospection::SetProperty( *job, String("pixelAR"), String("2.0") ),
		"SetProperty(pixelAR=2.0) succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film && film->GetPixelAR() == Scalar( 2.0 ), "Film pixelAR == 2.0" );

	const ICamera* pCam = job->GetScene()->GetCameras()->GetItem( "cam" );
	const CameraCommon* cc = dynamic_cast<const CameraCommon*>( pCam );
	Check( cc && cc->GetPixelAR() == Scalar( 2.0 ), "camera pixelAR resynced" );
}

//////////////////////////////////////////////////////////////////////
// 3. Invalid inputs are rejected, leave Film state untouched.
//////////////////////////////////////////////////////////////////////

static void TestSetPropertyRejectsInvalid()
{
	std::cout << "Testing SetProperty rejects invalid inputs..." << std::endl;
	JobHandle job;
	const IFilm* film = job->GetScene()->GetFilm();
	const unsigned int origW   = film->GetWidth();
	const unsigned int origH   = film->GetHeight();
	const Scalar       origPAR = film->GetPixelAR();

	// Zero / negative width.
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("0") ),
		"width=0 rejected" );
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("-100") ),
		"width=-100 rejected" );

	// Zero / negative pixelAR.
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("0") ),
		"pixelAR=0 rejected" );
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("-1.5") ),
		"pixelAR=-1.5 rejected" );

	// Unparseable string.
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("not-a-number") ),
		"unparseable width rejected" );

	// Unknown property name.
	Check( !FilmIntrospection::SetProperty( *job, String("rubbish_param"), String("42") ),
		"unknown property name rejected" );

	// Non-finite pixelAR — `NaN <= 0.0` is false, so a naive guard would
	// let NaN slip past and poison every camera's projection on the
	// resync.  std::isfinite catches NaN AND ±inf.
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("nan") ),
		"pixelAR=nan rejected" );
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("inf") ),
		"pixelAR=inf rejected" );
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("-inf") ),
		"pixelAR=-inf rejected" );

	// UINT_MAX + 1 — a naive sscanf-then-cast would wrap silently to 1
	// and pass the kMaxFilmWidth sanity bound, surreptitiously shrinking
	// the Film to 1×origH instead of failing the edit.  strtoll + the
	// `tmp > UINT_MAX` gate in ParseUInt catches this.
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("4294967296") ),
		"width=UINT_MAX+1 rejected (no silent wrap)" );

	// Partial-token parses — sscanf("%lld") would happily accept these.
	// strtoll + the `*endp != '\0'` gate rejects trailing garbage.
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("12.5") ),
		"width=12.5 rejected (no fractional truncation)" );
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("1e3") ),
		"width=1e3 rejected (no exponent partial-parse)" );
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("100abc") ),
		"width=100abc rejected" );

	// Empty string — strtol returns 0 and endp == start, both
	// rejected explicitly.
	Check( !FilmIntrospection::SetProperty( *job, String("width"), String("") ),
		"width=empty-string rejected" );

	// Same wholeness check for the double parser (pixelAR field).
	Check( !FilmIntrospection::SetProperty( *job, String("pixelAR"), String("1.5xyz") ),
		"pixelAR=1.5xyz rejected" );

	// State preserved after every rejected edit.
	Check( film->GetWidth()   == origW,   "Film width unchanged after rejects" );
	Check( film->GetHeight()  == origH,   "Film height unchanged after rejects" );
	Check( film->GetPixelAR() == origPAR, "Film pixelAR unchanged after rejects" );
}

//////////////////////////////////////////////////////////////////////
// 5. Same-dim SetProperty is a no-op (no camera projection thrash).
//////////////////////////////////////////////////////////////////////

static void TestSetPropertyNoOpSameDims()
{
	std::cout << "Testing SetProperty same-value short-circuit..." << std::endl;
	JobHandle job;

	// Snapshot the default Film, then set each field BACK to its
	// current value via the introspection path.  The same-dim short-
	// circuit in Job::SetFilm should treat each as a no-op (still
	// returns true, but doesn't re-allocate or resync cameras).  The
	// behavioural check here is just that the Film state stays
	// identical after the round-trip.
	const IFilm* film = job->GetScene()->GetFilm();
	const unsigned int origW   = film->GetWidth();
	const unsigned int origH   = film->GetHeight();
	const Scalar       origPAR = film->GetPixelAR();

	char wbuf[32], hbuf[32], pbuf[32];
	std::snprintf( wbuf, sizeof(wbuf), "%u",   origW );
	std::snprintf( hbuf, sizeof(hbuf), "%u",   origH );
	std::snprintf( pbuf, sizeof(pbuf), "%.6g", origPAR );

	Check( FilmIntrospection::SetProperty( *job, String("width"),   String(wbuf) ),
		"same-value width SetProperty succeeds" );
	Check( FilmIntrospection::SetProperty( *job, String("height"),  String(hbuf) ),
		"same-value height SetProperty succeeds" );
	Check( FilmIntrospection::SetProperty( *job, String("pixelAR"), String(pbuf) ),
		"same-value pixelAR SetProperty succeeds" );

	const IFilm* film2 = job->GetScene()->GetFilm();
	Check( film2->GetWidth()   == origW,   "Film width preserved" );
	Check( film2->GetHeight()  == origH,   "Film height preserved" );
	Check( film2->GetPixelAR() == origPAR, "Film pixelAR preserved" );
}

//////////////////////////////////////////////////////////////////////
// 4. GetPropertyValue reads the formatted string SetProperty accepts.
//////////////////////////////////////////////////////////////////////

static void TestGetPropertyValueRoundtrip()
{
	std::cout << "Testing GetPropertyValue/SetProperty round-trip..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.5 );
	const IFilm* film = job->GetScene()->GetFilm();

	const String wv = FilmIntrospection::GetPropertyValue( *film, String("width") );
	const String hv = FilmIntrospection::GetPropertyValue( *film, String("height") );
	const String pv = FilmIntrospection::GetPropertyValue( *film, String("pixelAR") );

	Check( std::string( wv.c_str() ) == "1920", "GetPropertyValue(width)=1920" );
	Check( std::string( hv.c_str() ) == "1080", "GetPropertyValue(height)=1080" );
	Check( std::string( pv.c_str() ) == "1.5",  "GetPropertyValue(pixelAR)=1.5" );

	// The returned strings round-trip back through SetProperty cleanly.
	JobHandle job2;
	Check( FilmIntrospection::SetProperty( *job2, String("width"),   wv ), "round-trip width set" );
	Check( FilmIntrospection::SetProperty( *job2, String("height"),  hv ), "round-trip height set" );
	Check( FilmIntrospection::SetProperty( *job2, String("pixelAR"), pv ), "round-trip pixelAR set" );

	const IFilm* film2 = job2->GetScene()->GetFilm();
	Check( film2->GetWidth()   == 1920, "round-tripped width matches" );
	Check( film2->GetHeight()  == 1080, "round-tripped height matches" );
	Check( film2->GetPixelAR() == Scalar( 1.5 ), "round-tripped pixelAR matches" );
}

//////////////////////////////////////////////////////////////////////
// 7. Preset list — surface for the accordion's dropdown.
//////////////////////////////////////////////////////////////////////

static void TestPresetListShape()
{
	std::cout << "Testing FilmIntrospection preset list shape..." << std::endl;
	const unsigned int n = FilmIntrospection::PresetCount();
	// User asked for "5-7 common resolutions"; pin the count so the
	// list can't accidentally drift to one entry again.
	Check( n >= 5 && n <= 7, "preset count between 5 and 7 inclusive" );

	// Every entry has a non-empty label and positive dims.
	for( unsigned int i = 0; i < n; ++i ) {
		const FilmPreset* p = FilmIntrospection::PresetAt( i );
		Check( p != nullptr,                "PresetAt(i) non-null" );
		Check( p && p->label && p->label[0],"preset has non-empty label" );
		Check( p && p->width  > 0,          "preset width  > 0" );
		Check( p && p->height > 0,          "preset height > 0" );
	}

	// Out-of-range index returns null.
	Check( FilmIntrospection::PresetAt( n )      == nullptr, "PresetAt(n) null" );
	Check( FilmIntrospection::PresetAt( n + 99 ) == nullptr, "PresetAt(n+99) null" );
}

static void TestPresetCoversLowToHigh()
{
	std::cout << "Testing preset list spans low to high resolutions..." << std::endl;
	const unsigned int n = FilmIntrospection::PresetCount();
	const FilmPreset* lo = FilmIntrospection::PresetAt( 0 );
	const FilmPreset* hi = FilmIntrospection::PresetAt( n - 1 );
	Check( lo && hi,                                          "endpoints present" );
	// Endpoints define the "low to HD/4K" coverage the request asked
	// for: the smallest entry sits under HD (720) and the largest
	// reaches 4K (2160).
	Check( lo && lo->width  <= 720,  "smallest preset width <= 720 (sub-HD)" );
	Check( hi && hi->height >= 2160, "largest preset height >= 2160 (4K)" );

	// Entries are strictly increasing in long-edge — the dropdown
	// reads naturally top-to-bottom from low to high.
	for( unsigned int i = 1; i < n; ++i ) {
		const FilmPreset* a = FilmIntrospection::PresetAt( i - 1 );
		const FilmPreset* b = FilmIntrospection::PresetAt( i );
		Check( a && b && a->width  < b->width,  "preset widths strictly increasing" );
		Check( a && b && a->height < b->height, "preset heights strictly increasing" );
	}
}

static void TestFindPresetRoundTrip()
{
	std::cout << "Testing FindPresetByDims / FindPresetByLabel round-trip..." << std::endl;
	const unsigned int n = FilmIntrospection::PresetCount();
	for( unsigned int i = 0; i < n; ++i ) {
		const FilmPreset* p = FilmIntrospection::PresetAt( i );
		Check( p != nullptr, "preset present" );
		if( !p ) continue;
		const int byDims  = FilmIntrospection::FindPresetByDims(  p->width, p->height );
		const int byLabel = FilmIntrospection::FindPresetByLabel( String( p->label ) );
		Check( byDims  == static_cast<int>( i ), "FindPresetByDims  round-trips" );
		Check( byLabel == static_cast<int>( i ), "FindPresetByLabel round-trips" );
	}

	// Non-matches return -1.
	Check( FilmIntrospection::FindPresetByDims( 800, 450 ) == -1,
		"800 x 450 is not a preset (ScaleFilmToFit result)" );
	Check( FilmIntrospection::FindPresetByLabel( String( "no such label" ) ) == -1,
		"unknown label rejected" );
	Check( FilmIntrospection::FindPresetByLabel( String( "" ) ) == -1,
		"empty label rejected" );
}

int main()
{
	std::cout << "=== FilmIntrospection Test (Phase G) ===" << std::endl;

	TestInspectDescriptorDriven();
	TestSetPropertyWidthRoundtrip();
	TestSetPropertyHeightRoundtrip();
	TestSetPropertyPixelARRoundtrip();
	TestSetPropertyRejectsInvalid();
	TestSetPropertyNoOpSameDims();
	TestGetPropertyValueRoundtrip();
	TestPresetListShape();
	TestPresetCoversLowToHigh();
	TestFindPresetRoundTrip();

	std::cout << std::endl;
	std::cout << "=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
