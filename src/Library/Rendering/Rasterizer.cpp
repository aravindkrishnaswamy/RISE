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
#include "../Interfaces/IObject.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/CPU.h"
#include "../Utilities/CPUTopology.h"
#include "../Utilities/RISECBOR64.h"
#include <stdexcept>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool SceneHasActiveFireMedium( const IScene& scene )
	{
		const IMedium* global = scene.GetGlobalMedium();
		if( global && global->IsFireMedium() ) return true;
		const IObjectManager* objects = scene.GetObjects();
		if( !objects ) return false;
		struct Collector : public IEnumCallback<const char*>
		{
			const IObjectManager& objects;
			bool found = false;
			explicit Collector( const IObjectManager& source ) : objects(source) {}
			bool operator()( const char* const& name ) override
			{
				const IObject* object = objects.GetItem(name);
				const IMedium* medium = object ? object->GetInteriorMedium() : nullptr;
				found = medium && medium->IsFireMedium();
				return !found;
			}
		} collector(*objects);
		objects->EnumerateItemNames(collector);
		return collector.found;
	}

	bool HasCompleteFireOutputMetadata( const FrameStoreOutput::Metadata& metadata )
	{
		return metadata.renderFidelityStatus == "preview" &&
			!metadata.renderReasonCodes.empty() &&
			!metadata.activeFireOpticsRecordIds.empty() &&
			!metadata.activeFireMedia.empty() &&
			!metadata.resolvedRenderConfigCoreV1.empty() &&
			!metadata.rendererBuildV1.empty() &&
			!metadata.rendererBuildId.empty();
	}

	std::string FireOutputMetadataBinding(
		const FrameStoreOutput::Metadata& metadata )
	{
		using RISECBOR64::Value;
		Value::Values reasons;
		for( const std::string& reason : metadata.renderReasonCodes ) {
			reasons.push_back(Value::String(reason));
		}
		Value::Values recordIds;
		for( const std::string& recordId : metadata.activeFireOpticsRecordIds ) {
			recordIds.push_back(Value::String(recordId));
		}
		Value::Values media;
		for( const FrameStoreOutput::ActiveFireMedium& medium : metadata.activeFireMedia ) {
			Value::Values opticalIds;
			for( const std::string& recordId : medium.opticalRecordIds ) {
				opticalIds.push_back(Value::String(recordId));
			}
			media.push_back(Value::MapValue({
				{ "authored_config_digest", Value::String(medium.authoredConfigDigest) },
				{ "binding_kind", Value::String(medium.bindingKind) },
				{ "binding_owner", Value::String(medium.bindingOwner) },
				{ "manager_name", Value::String(medium.managerName) },
				{ "media_kind", Value::String(medium.mediaKind) },
				{ "optical_record_ids", Value::ArrayValue(opticalIds) }
			}));
		}
		RISECBOR64::Bytes encoded;
		std::string error;
		const Value binding = Value::MapValue({
			{ "active_fire_media", Value::ArrayValue(media) },
			{ "active_fire_optics_record_ids", Value::ArrayValue(recordIds) },
			{ "render_fidelity_status", Value::String(metadata.renderFidelityStatus) },
			{ "render_reason_codes", Value::ArrayValue(reasons) },
			{ "renderer_build_id", Value::String(metadata.rendererBuildId) },
			{ "renderer_build_v1", Value::BytesValue(metadata.rendererBuildV1) },
			{ "resolved_render_config_core_v1",
				Value::BytesValue(metadata.resolvedRenderConfigCoreV1) }
		});
		if( !RISECBOR64::Encode(binding,encoded,&error) ) return std::string();
		return RISECBOR64::SHA256Hex(encoded);
	}
}

Rasterizer::Rasterizer( FrameStore* frameStore ) :
  pProgressFunc( 0 )
	,mFrameStore( frameStore )
	,mFireRenderPreflightAuthorization(FireRenderPreflightAuthorization::None)
	,mFireRenderPreflightScene(nullptr)
	,mFireRenderPreflightStore(nullptr)
	,mFireRenderPreflightGeneration(0u)
	,mFireRenderPreflightOutputTopologyGeneration(0u)
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

bool Rasterizer::RequireFireRenderPreflight(
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	if( !SceneHasActiveFireMedium(scene) ) {
		ClearFireRenderPreflightAuthorization();
		return false;
	}
	FireRenderPreflightAuthorization authorized = FireRenderPreflightAuthorization::None;
	const IScene* authorizedScene = nullptr;
	const FrameStore* authorizedStore = nullptr;
	uint64_t authorizedGeneration = 0u;
	uint64_t authorizedOutputTopologyGeneration = 0u;
	std::string authorizedMetadataBinding;
	{
		std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
		authorized = mFireRenderPreflightAuthorization;
		authorizedScene = mFireRenderPreflightScene;
		authorizedStore = mFireRenderPreflightStore;
		authorizedGeneration = mFireRenderPreflightGeneration;
		authorizedOutputTopologyGeneration =
			mFireRenderPreflightOutputTopologyGeneration;
		authorizedMetadataBinding = mFireRenderPreflightMetadataBinding;
		mFireRenderPreflightAuthorization = FireRenderPreflightAuthorization::None;
		mFireRenderPreflightScene = nullptr;
		mFireRenderPreflightStore = nullptr;
		mFireRenderPreflightGeneration = 0u;
		mFireRenderPreflightOutputTopologyGeneration = 0u;
		mFireRenderPreflightMetadataBinding.clear();
	}
	if( !SupportsFireMediaTransport() ) {
		GlobalLog()->PrintEasyError(
			"unsupported_integrator_for_fire_media: rasterizer entry rejected before workers launch");
		throw std::runtime_error(
			"unsupported_integrator_for_fire_media: rasterizer entry rejected before workers launch");
	}
	if( authorized != authorization || authorizedScene != &scene ) {
		GlobalLog()->PrintEasyError(
			"output_provenance_unavailable: fire rasterizer entry requires Job preflight");
		throw std::runtime_error(
			"output_provenance_unavailable: fire rasterizer entry requires Job preflight");
	}
	if( authorization == FireRenderPreflightAuthorization::Render &&
		(!mFrameStore || authorizedStore != mFrameStore ||
		 authorizedGeneration != mFrameStore->Generation() ||
		 authorizedOutputTopologyGeneration !=
			mFireOutputTopologyGeneration.load(std::memory_order_acquire) ||
		 !HasCompleteFireOutputMetadata(mFrameStore->Meta()) ||
		 authorizedMetadataBinding.empty() ||
		 authorizedMetadataBinding != FireOutputMetadataBinding(mFrameStore->Meta())) ) {
		GlobalLog()->PrintEasyError(
			"output_provenance_unavailable: fire rasterizer entry has incomplete output metadata");
		throw std::runtime_error(
			"output_provenance_unavailable: fire rasterizer entry has incomplete output metadata");
	}
	return true;
}

void Rasterizer::ClearFireRenderPreflightAuthorization() const
{
	std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
	mFireRenderPreflightAuthorization = FireRenderPreflightAuthorization::None;
	mFireRenderPreflightScene = nullptr;
	mFireRenderPreflightStore = nullptr;
	mFireRenderPreflightGeneration = 0u;
	mFireRenderPreflightOutputTopologyGeneration = 0u;
	mFireRenderPreflightMetadataBinding.clear();
}

bool Rasterizer::AuthorizeFireRenderPreflight(
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
	mFireRenderPreflightAuthorization = FireRenderPreflightAuthorization::None;
	mFireRenderPreflightScene = nullptr;
	mFireRenderPreflightStore = nullptr;
	mFireRenderPreflightGeneration = 0u;
	mFireRenderPreflightOutputTopologyGeneration = 0u;
	mFireRenderPreflightMetadataBinding.clear();
	if( authorization == FireRenderPreflightAuthorization::None ) return true;
	mFireRenderPreflightAuthorization = authorization;
	mFireRenderPreflightScene = &scene;
	mFireRenderPreflightStore = mFrameStore;
	mFireRenderPreflightGeneration = mFrameStore ? mFrameStore->Generation() : 0u;
	mFireRenderPreflightOutputTopologyGeneration =
		mFireOutputTopologyGeneration.load(std::memory_order_acquire);
	if( authorization == FireRenderPreflightAuthorization::Render ) {
		if( !mFrameStore ) return false;
		const FrameStoreOutput::Metadata metadata = mFrameStore->Meta();
		if( !HasCompleteFireOutputMetadata(metadata) ) return false;
		mFireRenderPreflightMetadataBinding = FireOutputMetadataBinding(metadata);
		if( mFireRenderPreflightMetadataBinding.empty() ) return false;
	}
	return true;
}

void Rasterizer::AuthorizeInternalFireReentry(
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	if( SceneHasActiveFireMedium(scene) ) {
		if( !AuthorizeFireRenderPreflight(scene,authorization) ) {
			throw std::runtime_error(
				"output_provenance_unavailable: internal fire reentry authorization failed");
		}
	}
}

void Rasterizer::AuthorizeInternalFireDelegate(
	IRasterizer& delegate,
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	Rasterizer* concrete = dynamic_cast<Rasterizer*>(&delegate);
	if( !concrete || !concrete->AuthorizeFireRenderPreflight(scene,authorization) ) {
		throw std::runtime_error(
			"output_provenance_unavailable: fire delegate authorization failed");
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
		mFireOutputTopologyGeneration.fetch_add(1u,std::memory_order_release);
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
			mFireOutputTopologyGeneration.fetch_add(1u,std::memory_order_release);
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
	if( released ) mFireOutputTopologyGeneration.fetch_add(1u,std::memory_order_release);
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
