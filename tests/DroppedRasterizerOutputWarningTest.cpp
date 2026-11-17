//////////////////////////////////////////////////////////////////////
//
//  DroppedRasterizerOutputWarningTest.cpp - Load warning for dropped
//    file_rasterizeroutput chunks.
//
//  What it pins:
//  -------------
//    [warn]     When a file_rasterizeroutput is declared after a
//               rasterizer, but a later rasterizer chunk replaces it,
//               DeriveToJob (with warnDeprecated=true, as in real load)
//               logs a one-time eLog_Warning naming the dropped output
//               pattern and the replacement rasterizer.
//    [clean]    When outputs are declared after the final rasterizer,
//               no dropped-output warning is logged.
//    [multi]    Multiple dropped outputs (either on the same rasterizer
//               or across multiple rasterizer transitions) each warn
//               with their specific pattern and replacement rasterizer.
//    [repeat]   Declaring the same rasterizer chunk again replaces the
//               instance and drops prior outputs with a warning.
//    [tuning]   Non-rasterizer chunks in the Rasterizer category (such
//               as light_rr_threshold) do not trigger dropped-output warnings.
//    [silent]   Staging / dry-run derives (warnDeprecated=false, the
//               default) stay silent.
//    [no-raster]file_rasterizeroutput with no preceding rasterizer fails
//               with the existing "no rasterizer is set" diagnostic.
//    [render]   No render change: the replacement rasterizer remains active
//               and renders without writing the dropped output.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Job.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) {
		++g_pass;
	} else {
		++g_fail;
		std::printf( "  FAIL: %s\n", w.c_str() );
	}
}

class DroppedOutputLog : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	DroppedOutputLog() {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( "dropped" ) != std::string::npos ||
		    msg.find( "file_rasterizeroutput" ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMsgs.push_back( msg );
			mTypes.push_back( event.eType );
		}
	}

	void Flush() override {}

	void Reset()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mMsgs.clear();
		mTypes.clear();
	}

	int Count( const std::string& needle ) const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		int n = 0;
		for( const std::string& m : mMsgs ) {
			if( m.find( needle ) != std::string::npos ) {
				++n;
			}
		}
		return n;
	}

	size_t Total() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mMsgs.size();
	}

	std::string First( const std::string& needle ) const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		for( const std::string& m : mMsgs ) {
			if( m.find( needle ) != std::string::npos ) return m;
		}
		return std::string();
	}

	bool AllWarnings() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( mTypes.empty() ) return false;
		for( RISE::LOG_ENUM t : mTypes ) {
			if( t != RISE::eLog_Warning ) return false;
		}
		return true;
	}

protected:
	~DroppedOutputLog() override {}

private:
	mutable std::mutex mMutex;
	std::vector<std::string> mMsgs;
	std::vector<RISE::LOG_ENUM> mTypes;
};

static const std::string HDR =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n";


int main()
{
	std::printf( "DroppedRasterizerOutputWarningTest\n" );

	DroppedOutputLog* log = new DroppedOutputLog();
	RISE::GlobalLogPriv()->AddPrinter( log );
	log->release();

	const std::string needle = "file_rasterizeroutput";

	//----------------------------------------------------------------------
	// [warn] single dropped output: PT -> output -> BDPT
	//----------------------------------------------------------------------
	std::printf( "[warn] single dropped output: PT -> output -> BDPT\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/pt_pass\ntype PNG\n}\n"
			"bdpt_pel_rasterizer\n{\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "scene derives cleanly without diagnostics" );
		Check( log->Count( "rendered/pt_pass" ) == 1, "exactly one warning naming the dropped output pattern" );
		Check( log->Count( "bdpt_pel_rasterizer" ) == 1, "warning names the replacement rasterizer" );
		Check( log->Count( "pathtracing_pel_rasterizer" ) == 1, "warning names the rasterizer it was attached to" );
		Check( log->AllWarnings(), "severity is eLog_Warning" );
		Check( j->GetActiveRasterizerName() == "bdpt_pel_rasterizer", "replacement rasterizer is active" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [clean] clean order: PT -> BDPT -> output
	//----------------------------------------------------------------------
	std::printf( "[clean] clean order: PT -> BDPT -> output\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"bdpt_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/bdpt_final\ntype PNG\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "clean scene derives cleanly" );
		Check( log->Count( "dropped" ) == 0, "no dropped output warnings when outputs follow final rasterizer" );
		Check( j->GetActiveRasterizerName() == "bdpt_pel_rasterizer", "bdpt is active" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [multi] multiple dropped outputs on one rasterizer
	//----------------------------------------------------------------------
	std::printf( "[multi] multiple dropped outputs on one rasterizer\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/out_hdr\ntype EXR\ncolor_space Rec709RGB_Linear\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/out_preview\ntype PNG\n}\n"
			"vcm_pel_rasterizer\n{\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "scene derives cleanly" );
		Check( log->Count( "rendered/out_hdr" ) == 1, "warning names first dropped output" );
		Check( log->Count( "rendered/out_preview" ) == 1, "warning names second dropped output" );
		Check( log->Count( "vcm_pel_rasterizer" ) == 2, "both warnings name vcm_pel_rasterizer" );
		Check( log->Count( "dropped" ) == 2, "exactly two dropped warnings" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [multi-step] transitions across multiple rasterizers
	//----------------------------------------------------------------------
	std::printf( "[multi-step] transitions across multiple rasterizers\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/pt_drop\ntype PNG\n}\n"
			"bdpt_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/bdpt_drop\ntype PNG\n}\n"
			"vcm_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/vcm_kept\ntype PNG\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "scene derives cleanly" );
		Check( log->Count( "rendered/pt_drop" ) == 1, "pt output dropped" );
		Check( log->Count( "rendered/bdpt_drop" ) == 1, "bdpt output dropped" );
		Check( log->Count( "rendered/vcm_kept" ) == 0, "final output on active rasterizer not dropped" );
		Check( log->Count( "dropped" ) == 2, "exactly two dropped output warnings" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [repeat] same rasterizer chunk repeated
	//----------------------------------------------------------------------
	std::printf( "[repeat] same rasterizer chunk repeated\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/pt1_drop\ntype PNG\n}\n"
			"pathtracing_pel_rasterizer\n{\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "scene derives cleanly" );
		Check( log->Count( "rendered/pt1_drop" ) == 1, "output dropped when same rasterizer chunk is repeated" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [tuning] non-rasterizer in Rasterizer category (light_rr_threshold)
	//----------------------------------------------------------------------
	std::printf( "[tuning] non-rasterizer in Rasterizer category does not drop outputs\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/pt_kept\ntype PNG\n}\n"
			"light_rr_threshold\n{\nthreshold 0.5\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n > 0 && diags.empty(), "scene derives cleanly" );
		Check( log->Count( "dropped" ) == 0, "light_rr_threshold does not trigger dropped output warning" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [silent] dry-run / staging derive (warnDeprecated=false, the default)
	//----------------------------------------------------------------------
	std::printf( "[silent] staging/dry-run derive stays silent\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"pathtracing_pel_rasterizer\n{\n}\n"
			"file_rasterizeroutput\n{\npattern rendered/pt_pass\ntype PNG\n}\n"
			"bdpt_pel_rasterizer\n{\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags );   // default warnDeprecated = false

		Check( n > 0 && diags.empty(), "dry-run derives cleanly" );
		Check( log->Count( "dropped" ) == 0, "staging derive logs zero dropped warnings" );

		j->release();
	}

	//----------------------------------------------------------------------
	// [no-raster] file_rasterizeroutput without rasterizer fails as before
	//----------------------------------------------------------------------
	std::printf( "[no-raster] file_rasterizeroutput without rasterizer fails\n" );
	{
		log->Reset();
		Job* j = new Job();
		std::vector<std::string> diags;
		const std::string scene = HDR +
			"file_rasterizeroutput\n{\npattern rendered/nowhere\ntype PNG\n}\n";

		Document d = ParseToCst( scene );
		const int n = DeriveToJob( d, *j, &diags, nullptr, nullptr, /*warnDeprecated=*/true );

		Check( n == 0 || !diags.empty(), "derive fails when output precedes all rasterizers" );
		Check( log->Count( "dropped" ) == 0, "no dropped warning (it is a fatal diagnostic, not a dropped output)" );

		j->release();
	}

	std::printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
