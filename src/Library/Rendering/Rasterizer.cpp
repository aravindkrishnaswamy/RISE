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
#include <cstdint>
#include <stdexcept>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	class FireOutputBindingActivity
	{
	public:
		explicit FireOutputBindingActivity( std::atomic<unsigned int>& active )
			: active_(active)
		{
			active_.fetch_add(1u,std::memory_order_acq_rel);
		}
		~FireOutputBindingActivity()
		{
			active_.fetch_sub(1u,std::memory_order_release);
		}
	private:
		std::atomic<unsigned int>& active_;
	};

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

	std::string SceneFireMediaBinding( const IScene& scene )
	{
		using RISECBOR64::Value;
		Value::Values bindings;
		const IMedium* global = scene.GetGlobalMedium();
		if( global && global->IsFireMedium() ) {
			bindings.push_back(Value::MapValue({
				{ "binding_kind", Value::String("global_medium") },
				{ "binding_owner", Value::String("scene") },
				{ "runtime_identity", Value::Unsigned(
					static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(global))) }
			}));
		}
		const IObjectManager* objects = scene.GetObjects();
		if( objects ) {
			struct Collector : public IEnumCallback<const char*>
			{
				const IObjectManager& objects;
				Value::Values& bindings;
				Collector( const IObjectManager& source, Value::Values& output ) :
					objects(source), bindings(output) {}
				bool operator()( const char* const& name ) override
				{
					const IObject* object = objects.GetItem(name);
					const IMedium* medium = object ? object->GetInteriorMedium() : nullptr;
					if( medium && medium->IsFireMedium() ) {
						bindings.push_back(Value::MapValue({
							{ "binding_kind", Value::String("object_interior_medium") },
							{ "binding_owner", Value::String(name ? name : "") },
							{ "runtime_identity", Value::Unsigned(static_cast<std::uint64_t>(
								reinterpret_cast<std::uintptr_t>(medium))) }
						}));
					}
					return true;
				}
			} collector(*objects,bindings);
			objects->EnumerateItemNames(collector);
		}
		if( bindings.empty() ) return std::string();
		RISECBOR64::Bytes encoded;
		std::string error;
		return RISECBOR64::Encode(Value::ArrayValue(bindings),encoded,&error) ?
			RISECBOR64::SHA256Hex(encoded) : std::string();
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
	FireRenderPreflightAuthorization authorized = FireRenderPreflightAuthorization::None;
	const IScene* authorizedScene = nullptr;
	const FrameStore* authorizedStore = nullptr;
	uint64_t authorizedGeneration = 0u;
	uint64_t authorizedOutputTopologyGeneration = 0u;
	std::string authorizedMetadataBinding;
	std::string authorizedSceneMediaBinding;
	{
		std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
		authorized = mFireRenderPreflightAuthorization;
		authorizedScene = mFireRenderPreflightScene;
		authorizedStore = mFireRenderPreflightStore;
		authorizedGeneration = mFireRenderPreflightGeneration;
		authorizedOutputTopologyGeneration =
			mFireRenderPreflightOutputTopologyGeneration;
		authorizedMetadataBinding = mFireRenderPreflightMetadataBinding;
		authorizedSceneMediaBinding = mFireRenderPreflightSceneMediaBinding;
		mFireRenderPreflightAuthorization = FireRenderPreflightAuthorization::None;
		mFireRenderPreflightScene = nullptr;
		mFireRenderPreflightStore = nullptr;
		mFireRenderPreflightGeneration = 0u;
		mFireRenderPreflightOutputTopologyGeneration = 0u;
		mFireRenderPreflightMetadataBinding.clear();
		mFireRenderPreflightSceneMediaBinding.clear();
	}
	const std::string currentSceneMediaBinding = SceneFireMediaBinding(scene);
	if( currentSceneMediaBinding.empty() ) {
		if( authorized == FireRenderPreflightAuthorization::None ) return false;
		GlobalLog()->PrintEasyError(
			"output_provenance_unavailable: active fire media changed after preflight");
		throw std::runtime_error(
			"output_provenance_unavailable: active fire media changed after preflight");
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
	FrameStore* currentStore = nullptr;
	uint64_t currentTopology = 0u;
	unsigned int bindingInProgress = 0u;
	{
		std::lock_guard<std::mutex> outputsLock(outsMutex);
		currentStore = mFrameStore;
		if( currentStore ) currentStore->addref();
		currentTopology =
			mFireOutputTopologyGeneration.load(std::memory_order_relaxed);
		bindingInProgress =
			mFireOutputBindingInProgress.load(std::memory_order_relaxed);
	}
	bool valid = authorizedOutputTopologyGeneration == currentTopology &&
		authorizedSceneMediaBinding == currentSceneMediaBinding &&
		bindingInProgress == 0u;
	if( valid && authorization == FireRenderPreflightAuthorization::Render ) {
		const FrameStoreOutput::Metadata metadata = currentStore ?
			currentStore->Meta() : FrameStoreOutput::Metadata();
		std::string metadataError;
		valid = currentStore && authorizedStore == currentStore &&
			authorizedGeneration == currentStore->Generation() &&
			FrameStoreOutput::ValidateFireOutputMetadata(metadata,metadataError) &&
			!authorizedMetadataBinding.empty() &&
			authorizedMetadataBinding == FireOutputMetadataBinding(metadata);
	}
	if( valid && authorization == FireRenderPreflightAuthorization::Render ) {
		std::lock_guard<std::mutex> outputsLock(outsMutex);
		valid = currentStore == mFrameStore && currentTopology ==
			mFireOutputTopologyGeneration.load(std::memory_order_relaxed) &&
			mFireOutputBindingInProgress.load(std::memory_order_relaxed) == 0u;
		if( valid ) {
			currentStore->AcquireFireMetadataLease();
			++mFireOutputTopologyLeaseCount;
		}
	}
	safe_release(currentStore);
	if( !valid ) {
		GlobalLog()->PrintEasyError(
			"output_provenance_unavailable: fire rasterizer entry has incomplete output metadata");
		throw std::runtime_error(
			"output_provenance_unavailable: fire rasterizer entry has incomplete output metadata");
	}
	return true;
}

Rasterizer::FireOutputTopologyLease::FireOutputTopologyLease(
	const Rasterizer& owner,
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) :
	owner_(owner.RequireFireRenderPreflight(scene,authorization) &&
		authorization == FireRenderPreflightAuthorization::Render ? &owner : nullptr)
{
}

Rasterizer::FireOutputTopologyLease::~FireOutputTopologyLease()
{
	if( owner_ ) owner_->ReleaseFireOutputTopologyLease();
}

void Rasterizer::ReleaseFireOutputTopologyLease() const
{
	std::lock_guard<std::mutex> lock(outsMutex);
	if( mFireOutputTopologyLeaseCount ) {
		if( mFrameStore ) mFrameStore->ReleaseFireMetadataLease();
		--mFireOutputTopologyLeaseCount;
	}
}

void Rasterizer::ClearFireRenderPreflightAuthorization() const
{
	{
		std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
		mFireRenderPreflightAuthorization = FireRenderPreflightAuthorization::None;
		mFireRenderPreflightScene = nullptr;
		mFireRenderPreflightStore = nullptr;
		mFireRenderPreflightGeneration = 0u;
		mFireRenderPreflightOutputTopologyGeneration = 0u;
		mFireRenderPreflightMetadataBinding.clear();
		mFireRenderPreflightSceneMediaBinding.clear();
	}
	ClearFireDelegatePreflight();
}

bool Rasterizer::AuthorizeFireRenderPreflight(
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	ClearFireRenderPreflightAuthorization();
	if( authorization == FireRenderPreflightAuthorization::None ) return true;
	struct FireRouteCollector : public IEnumCallback<IRasterizerOutput>
	{
		bool sawArtifact = false;
		bool sawPrimary = false;
		bool invalid = false;
		bool operator()( const IRasterizerOutput& output ) override
		{
			const IFireRasterizerOutputRoute* route =
				dynamic_cast<const IFireRasterizerOutputRoute*>(&output);
			if( !route ) {
				invalid = true;
				return true;
			}
			switch( route->FireArtifactRoute() ) {
			case FireArtifactRouteKind::DisplayOnly:
				return true;
			case FireArtifactRouteKind::UnavailableArtifact:
				sawArtifact = true;
				invalid = true;
				return true;
			case FireArtifactRouteKind::PrimaryArtifact:
				sawArtifact = true;
				sawPrimary = true;
				return true;
			case FireArtifactRouteKind::DerivativeArtifact:
				sawArtifact = true;
				if( !sawPrimary ) invalid = true;
				return true;
			}
			invalid = true;
			return true;
		}
	};
	if( mFireOutputBindingInProgress.load(std::memory_order_acquire) != 0u ) {
		return false;
	}
	const std::string sceneMediaBinding = SceneFireMediaBinding(scene);
	if( sceneMediaBinding.empty() ) return false;
	const uint64_t observedTopology =
		mFireOutputTopologyGeneration.load(std::memory_order_acquire);
	FireRouteCollector routes;
	EnumerateRasterizerOutputs(routes);
	if( routes.invalid || (routes.sawArtifact && !routes.sawPrimary) ||
		mFireOutputBindingInProgress.load(std::memory_order_acquire) != 0u ||
		observedTopology !=
			mFireOutputTopologyGeneration.load(std::memory_order_acquire) ) {
		return false;
	}
	FrameStore* store = nullptr;
	{
		std::lock_guard<std::mutex> outputsLock(outsMutex);
		if( observedTopology !=
				mFireOutputTopologyGeneration.load(std::memory_order_relaxed) ||
			mFireOutputBindingInProgress.load(std::memory_order_relaxed) != 0u ) {
			return false;
		}
		store = mFrameStore;
		if( store ) store->addref();
	}
	uint64_t storeGeneration = store ? store->Generation() : 0u;
	std::string metadataBinding;
	if( authorization == FireRenderPreflightAuthorization::Render ) {
		if( !store ) return false;
		const FrameStoreOutput::Metadata metadata = store->Meta();
		std::string metadataError;
		if( storeGeneration != store->Generation() ||
			!FrameStoreOutput::ValidateFireOutputMetadata(metadata,metadataError) ) {
			safe_release(store);
			return false;
		}
		metadataBinding = FireOutputMetadataBinding(metadata);
		if( metadataBinding.empty() ) {
			safe_release(store);
			return false;
		}
	}
	if( observedTopology !=
			mFireOutputTopologyGeneration.load(std::memory_order_acquire) ||
		mFireOutputBindingInProgress.load(std::memory_order_acquire) != 0u ) {
		safe_release(store);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mFireRenderPreflightMutex);
		mFireRenderPreflightAuthorization = authorization;
		mFireRenderPreflightScene = &scene;
		mFireRenderPreflightStore = store;
		mFireRenderPreflightGeneration = storeGeneration;
		mFireRenderPreflightOutputTopologyGeneration = observedTopology;
		mFireRenderPreflightMetadataBinding = metadataBinding;
		mFireRenderPreflightSceneMediaBinding = sceneMediaBinding;
	}
	if( !AuthorizeFireDelegatePreflight(scene,authorization) ) {
		safe_release(store);
		ClearFireRenderPreflightAuthorization();
		return false;
	}
	bool unchanged = observedTopology ==
		mFireOutputTopologyGeneration.load(std::memory_order_acquire) &&
		mFireOutputBindingInProgress.load(std::memory_order_acquire) == 0u &&
		sceneMediaBinding == SceneFireMediaBinding(scene);
	if( unchanged && authorization == FireRenderPreflightAuthorization::Render ) {
		const FrameStoreOutput::Metadata metadata = store->Meta();
		unchanged = storeGeneration == store->Generation() &&
			metadataBinding == FireOutputMetadataBinding(metadata);
	}
	safe_release(store);
	if( !unchanged ) {
		ClearFireRenderPreflightAuthorization();
		return false;
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

void Rasterizer::ClearInternalFireDelegateAuthorization(
	IRasterizer& delegate ) const
{
	Rasterizer* concrete = dynamic_cast<Rasterizer*>(&delegate);
	if( concrete ) concrete->ClearFireRenderPreflightAuthorization();
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
	FireOutputBindingActivity binding(mFireOutputBindingInProgress);
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
		if( mFireOutputTopologyLeaseCount ) {
			throw std::runtime_error(
				"output_provenance_unavailable: fire render output topology is leased");
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
	if( mFireOutputTopologyLeaseCount ) {
		throw std::runtime_error(
			"output_provenance_unavailable: fire render output topology is leased");
	}
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
	if( mFireOutputTopologyLeaseCount ) {
		throw std::runtime_error(
			"output_provenance_unavailable: fire render output topology is leased");
	}
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
	FireOutputBindingActivity binding(mFireOutputBindingInProgress);
	FrameStore* previous = 0;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		if( mFireOutputTopologyLeaseCount ) {
			throw std::runtime_error(
				"output_provenance_unavailable: fire render output topology is leased");
		}
		// Same-pointer early-return: existing outputs are already bound, while
		// outputs attached after the original swap receive the current store from
		// RegisterRasterizerOutput at insertion time.
		if( frameStore == mFrameStore ) {
			return;
		}
		if( frameStore ) frameStore->addref();
		previous = mFrameStore;
		mFrameStore = frameStore;
		mFireOutputTopologyGeneration.fetch_add(1u,std::memory_order_release);
	}
	safe_release(previous);

	// L6e-3 — Re-dispatch path lives in `ReannounceFrameStore`
	// below; the swap path here calls into it after updating
	// `mFrameStore`.

	ReannounceFrameStore();
}

void Rasterizer::ReannounceFrameStore()
{
	FireOutputBindingActivity binding(mFireOutputBindingInProgress);
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		if( mFireOutputTopologyLeaseCount ) {
			throw std::runtime_error(
				"output_provenance_unavailable: fire render output topology is leased");
		}
	}
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
