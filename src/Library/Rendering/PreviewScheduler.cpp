//////////////////////////////////////////////////////////////////////
//
//  PreviewScheduler.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PreviewScheduler.h"

using namespace RISE;
using namespace RISE::Implementation;

PreviewScheduler::PreviewScheduler( double targetIntervalSeconds ) :
	targetInterval( targetIntervalSeconds > 0 ? targetIntervalSeconds : 0 ),
	firstCall( true )
{
	lastPreview = std::chrono::steady_clock::now();
}

bool PreviewScheduler::ShouldRunPreview()
{
	// First call: always preview — user wants feedback fast.
	if( firstCall ) {
		firstCall = false;
		return true;
	}
	if( targetInterval <= 0 ) {
		return true;
	}
	const auto now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> elapsed = now - lastPreview;
	return elapsed.count() >= targetInterval;
}

void PreviewScheduler::MarkPreviewRan()
{
	lastPreview = std::chrono::steady_clock::now();
}
