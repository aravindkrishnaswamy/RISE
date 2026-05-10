//////////////////////////////////////////////////////////////////////
//
//  Rasterizer.cpp - Implements the functions in implementation help
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Rasterizer.h"
#include "FrameStore.h"
#include "OIDNDenoiser.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/CPU.h"
#include "../Utilities/CPUTopology.h"

using namespace RISE;
using namespace RISE::Implementation;

Rasterizer::Rasterizer( FrameStore* frameStore ) :
  pProgressFunc( 0 )
  ,mFrameStore( frameStore )
#ifdef RISE_ENABLE_OIDN
  ,bDenoisingEnabled( false )
  ,mDenoisingQuality( OidnQuality::Auto )
  ,mDenoisingDevice( OidnDevice::Auto )
  ,mDenoisingPrefilter( OidnPrefilter::Fast )
  ,mRenderStartTime( std::chrono::steady_clock::now() )
  ,mDenoiser( new OIDNDenoiser() )
#endif
{
	// L6a — addref the FrameStore so the rasterizer keeps it alive
	// for its own lifetime.  Job (or whatever else owns the original
	// allocation) is welcome to release its own ref independently;
	// FrameStore stays alive until the LAST holder releases.  Null
	// is permitted during the L6a → L6b transition window.
	if( mFrameStore ) {
		mFrameStore->addref();
	}
}

Rasterizer::~Rasterizer( )
{
	FreeRasterizerOutputs();
#ifdef RISE_ENABLE_OIDN
	delete mDenoiser;
	mDenoiser = 0;
#endif
	// L6a — drop our FrameStore reference.  If we held the last ref
	// (e.g. Job already torn down), this destroys the FrameStore;
	// otherwise the surviving holder keeps it alive.
	safe_release( mFrameStore );
}

int Rasterizer::HowManyThreadsToSpawn() const
{
	// Thread count derives from CPU topology AND user overrides.
	// ComputeRenderPoolSize already honours force_number_of_threads
	// and maximum_thread_count, so caller dispatch count aligns with
	// the actual render-pool size regardless of which knob the user
	// turned.
	return static_cast<int>( RISE::Implementation::ComputeRenderPoolSize() );
}

void Rasterizer::AddRasterizerOutput( IRasterizerOutput* ro )
{
	if( !ro ) return;

	// L8 review round 5 — mutex + dedup.  See `outsMutex` comment in
	// Rasterizer.h.  Dedup eliminates the unbounded-vector-growth
	// + iterator-invalidation symptom from Swift's per-display-refresh
	// `attachViewportFrameStoreToOpaqueRasterizer` calls (each was
	// pushing a duplicate VFS into `outs` before this fix; logs
	// showed 30+ duplicates accumulated per render).
	std::lock_guard<std::mutex> lock( outsMutex );
	for( IRasterizerOutput* existing : outs ) {
		if( existing == ro ) {
			return;  // already registered, no-op
		}
	}
	ro->addref();
	outs.push_back( ro );
}

void Rasterizer::FreeRasterizerOutputs( )
{
	std::lock_guard<std::mutex> lock( outsMutex );
	RasterizerOutputListType::iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		safe_release( (*i) );
	}
	outs.clear();
}

void Rasterizer::EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const
{
	// L8 review round 5 — snapshot under lock then invoke without it.
	// `pFunc` could re-enter `AddRasterizerOutput` / `FreeRasterizerOutputs`
	// (recursive lock would deadlock) and shouldn't hold the lock for
	// the duration of arbitrary callback work.
	RasterizerOutputListType snapshot;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		snapshot = outs;
	}
	for( IRasterizerOutput* ro : snapshot ) {
		pFunc( *ro );
	}
}

void Rasterizer::SetProgressCallback( IProgressCallback* pFunc )
{
	pProgressFunc = pFunc;
}

// L6b — Late-binding FrameStore setter.  Called by Job after scene
// load when the canonical FrameStore can finally be allocated against
// the active camera's dims.  Lifecycle mirrors the ctor: addref the
// new store + release the old.  Idempotent at the same pointer
// (addref + release on the same object cancel out).
//
// L6e-2b — After the swap, fire `OnRasterizerFrameStoreChanged` on
// every attached `IRasterizerOutput` so direct-consumers (e.g.
// `ViewportFrameStore` post-L6e-2a) can rebind to the new store.
// Default impl on `IRasterizerOutput` is a no-op, so file outputs +
// legacy callback sinks are unaffected.
void Rasterizer::SetFrameStore( FrameStore* frameStore )
{
	// Same-pointer early-return: the caller wants no-op semantics
	// (typical: Job's `PushJobFrameStoreToRasterizers` re-runs after
	// a non-camera-related event and the FrameStore hasn't actually
	// changed).  Outputs that were already bound to this pointer
	// don't need a redundant notification — they're already in the
	// right state.  Outputs that attached AFTER the original swap
	// catch up via `Attach`'s `GetFrameStore()` pull, NOT via a
	// SetFrameStore re-dispatch.  See L6e-2b adversarial review P2.
	if( frameStore == mFrameStore ) {
		return;  // no-op when caller passes the same pointer
	}
	if( frameStore ) {
		frameStore->addref();
	}
	safe_release( mFrameStore );  // null-safe + zeroes the local
	mFrameStore = frameStore;

	// L6e-3 — Re-dispatch path lives in `ReannounceFrameStore`
	// below; the swap path here calls into it after updating
	// `mFrameStore`.

	ReannounceFrameStore();
}

void Rasterizer::ReannounceFrameStore()
{
	// L6e-3 — Re-fire `OnRasterizerFrameStoreChanged(mFrameStore)`
	// on every attached output.  Caller has either just swapped
	// `mFrameStore` (called from `SetFrameStore`) or wants the
	// outs to receive the CURRENT binding without a swap (e.g.
	// after `FreeRasterizerOutputs` + `AddRasterizerOutput(newSink)`
	// in the SceneEditController interactive flow).
	//
	// Snapshot `outs` before iterating — see L6e-2b adversarial
	// review P1-A.  If any callback re-enters
	// `AddRasterizerOutput`/`FreeRasterizerOutputs`, the live
	// iterator would otherwise be invalidated.
	// L8 round 5 — snapshot now happens under `outsMutex` to guard
	// against concurrent mutators from non-render threads (Swift
	// UI display-refresh path).
	RasterizerOutputListType snapshot;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		snapshot = outs;
	}
	for( RasterizerOutputListType::const_iterator it = snapshot.begin(),
	     e = snapshot.end(); it != e; ++it )
	{
		(*it)->OnRasterizerFrameStoreChanged( mFrameStore );
	}
}

