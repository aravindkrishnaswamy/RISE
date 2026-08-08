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

namespace
{
	class RetainedRasterizerOutputSnapshot
	{
	public:
		RetainedRasterizerOutputSnapshot(
			const std::vector<IRasterizerOutput*>& source,
			std::mutex& sourceMutex
			)
		{
			std::lock_guard<std::mutex> lock( sourceMutex );
			try {
				for( IRasterizerOutput* output : source ) {
					output->addref();
					try {
						mOutputs.push_back(output);
					}
					catch( ... ) {
						output->release();
						throw;
					}
				}
			}
			catch( ... ) {
				Release();
				throw;
			}
		}

		~RetainedRasterizerOutputSnapshot()
		{
			Release();
		}

		const std::vector<IRasterizerOutput*>& Outputs() const
		{
			return mOutputs;
		}

	private:
		void Release()
		{
			for( IRasterizerOutput* output : mOutputs ) output->release();
			mOutputs.clear();
		}

		std::vector<IRasterizerOutput*> mOutputs;
	};
}

Rasterizer::Rasterizer( FrameStore* frameStore ) :
  pProgressFunc( 0 )
  ,mFrameStore( frameStore )
  ,mDenoisingPrefilter( OidnPrefilter::Fast )
#ifdef RISE_ENABLE_OIDN
  ,bDenoisingEnabled( false )
  ,mDenoisingQuality( OidnQuality::Auto )
  ,mDenoisingDevice( OidnDevice::Auto )
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
	if( mForTestThreadCountOverride > 0 ) {
		return mForTestThreadCountOverride;
	}
	// Thread count derives from CPU topology AND user overrides.
	// ComputeRenderPoolSize already honours force_number_of_threads
	// and maximum_thread_count, so caller dispatch count aligns with
	// the actual render-pool size regardless of which knob the user
	// turned.
	return static_cast<int>( RISE::Implementation::ComputeRenderPoolSize() );
}

void Rasterizer::AddRasterizerOutput( IRasterizerOutput* ro )
{
	RegisterRasterizerOutput( ro );
}

bool Rasterizer::RegisterRasterizerOutput( IRasterizerOutput* ro )
{
	if( !ro ) return false;
	FrameStore* frameStoreSnapshot = 0;

	// L8 review round 5 — mutex + dedup.  See `outsMutex` comment in
	// Rasterizer.h.  Dedup eliminates the unbounded-vector-growth
	// + iterator-invalidation symptom from Swift's per-display-refresh
	// `attachViewportFrameStoreToOpaqueRasterizer` calls (each was
	// pushing a duplicate VFS into `outs` before this fix; logs
	// showed 30+ duplicates accumulated per render).
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		for( IRasterizerOutput* existing : outs ) {
			if( existing == ro ) {
				return false;  // already registered, no-op
			}
		}
		// Take the list's reference first, but roll it back if vector growth
		// throws.  IReference::addref is a virtual legacy API without a noexcept
		// declaration, so neither ordering is independently safe; this explicit
		// transaction leaves no published entry and no extra ref on either throw.
		ro->addref();
		try {
			outs.push_back( ro );
		}
		catch( ... ) {
			ro->release();
			throw;
		}
		frameStoreSnapshot = mFrameStore;
		if( frameStoreSnapshot ) frameStoreSnapshot->addref();
	}
	try {
		ro->OnRasterizerFrameStoreChanged( frameStoreSnapshot );
	}
	catch( ... ) {
		safe_release(frameStoreSnapshot);
		RemoveRasterizerOutput( ro );
		throw;
	}
	safe_release(frameStoreSnapshot);
	return true;
}

void Rasterizer::RemoveRasterizerOutput( IRasterizerOutput* ro )
{
	UnregisterRasterizerOutput( ro );
}

bool Rasterizer::UnregisterRasterizerOutput( IRasterizerOutput* ro )
{
	if( !ro ) return false;

	std::lock_guard<std::mutex> lock( outsMutex );
	for( RasterizerOutputListType::iterator i=outs.begin(), e=outs.end(); i!=e; ++i ) {
		if( *i == ro ) {
			IRasterizerOutput* removed = *i;
			outs.erase( i );
			safe_release( removed );
			return true;
		}
	}
	return false;
}

void Rasterizer::FreeRasterizerOutputs( )
{
	ReleaseRasterizerOutputs();
}

bool Rasterizer::ReleaseRasterizerOutputs()
{
	std::lock_guard<std::mutex> lock( outsMutex );
	const bool released = !outs.empty();
	RasterizerOutputListType::iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		safe_release( (*i) );
	}
	outs.clear();
	return released;
}

void Rasterizer::EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const
{
	// L8 review round 5 — snapshot under lock then invoke without it.
	// `pFunc` could re-enter `AddRasterizerOutput` / `FreeRasterizerOutputs`
	// (recursive lock would deadlock) and shouldn't hold the lock for
	// the duration of arbitrary callback work.
	RetainedRasterizerOutputSnapshot snapshot( outs, outsMutex );
	for( IRasterizerOutput* ro : snapshot.Outputs() ) {
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
	FrameStore* previous = 0;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		// Same-pointer early-return: existing outputs are already bound, while
		// outputs attached after the original swap receive the current store from
		// RegisterRasterizerOutput at insertion time.
		if( frameStore == mFrameStore ) {
			return;
		}
		if( frameStore ) frameStore->addref();
		previous = mFrameStore;
		mFrameStore = frameStore;
	}
	safe_release(previous);

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
	RetainedRasterizerOutputSnapshot snapshot( outs, outsMutex );
	FrameStore* frameStoreSnapshot = 0;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		frameStoreSnapshot = mFrameStore;
		if( frameStoreSnapshot ) frameStoreSnapshot->addref();
	}
	try {
		for( RasterizerOutputListType::const_iterator it = snapshot.Outputs().begin(),
		     e = snapshot.Outputs().end(); it != e; ++it )
		{
			(*it)->OnRasterizerFrameStoreChanged( frameStoreSnapshot );
		}
	}
	catch( ... ) {
		safe_release(frameStoreSnapshot);
		throw;
	}
	safe_release(frameStoreSnapshot);
}
