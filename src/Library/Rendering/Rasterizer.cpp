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
	if( ro ) {
		ro->addref();
		outs.push_back( ro );
	}
}

void Rasterizer::FreeRasterizerOutputs( )
{
	RasterizerOutputListType::iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		safe_release( (*i) );
	}
	outs.clear();
}

void Rasterizer::EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const
{
	RasterizerOutputListType::const_iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		pFunc( *(*i) );
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

	// Notify every attached output of the new FrameStore.  The
	// dispatch happens AFTER `mFrameStore` is updated so observers
	// that re-query via `GetFrameStore()` see the new state.
	//
	// L6e-2b adversarial review P1-A — snapshot the outs list
	// before iterating.  `outs` is a `std::vector<IRasterizerOutput*>`;
	// if any callback re-enters `AddRasterizerOutput` (push_back →
	// potential reallocation) or `FreeRasterizerOutputs` (clear),
	// the live iterator is invalidated → UB.  Today's
	// `ViewportFrameStore::OnRasterizerFrameStoreChanged` doesn't
	// re-enter, but the `IRasterizerOutput` contract doesn't forbid
	// it (the default impl is no-op; subclasses are free to do what
	// they like).  The snapshot is cheap (vector of pointers) and
	// makes the dispatch robust against any future override.
	//
	// Iteration order: the snapshot preserves the insertion order
	// matching `EnumerateRasterizerOutputs`'s contract.
	const RasterizerOutputListType snapshot = outs;
	for( RasterizerOutputListType::const_iterator it = snapshot.begin(),
	     e = snapshot.end(); it != e; ++it )
	{
		(*it)->OnRasterizerFrameStoreChanged( mFrameStore );
	}
}

