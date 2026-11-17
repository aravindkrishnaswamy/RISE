//////////////////////////////////////////////////////////////////////
//
//  FrameStoreTest.cpp - L1 regression gate for FrameStore +
//  Channel<T> + tile locking + Render readback.
//
//  Coverage:
//    1. Construction: width/height/tileEdge geometry; channel
//       presence per Spec.aovChannels; Beauty + Alpha always
//       present.
//    2. Channel<T>: bounds, fill, row/column access; type-safety
//       via ChannelTraits.
//    3. Tile locking: BeginTile/EndTile bracket pixel writes,
//       generation counter advances, observer fires once per
//       commit.
//    4. Concurrent reader / writer: multi-threaded stress test
//       proves no torn reads under contention.
//    5. Render() readback: identity ViewTransform on a known
//       beauty buffer produces expected RGBA8_sRGB bytes;
//       exposure scaling propagates; tone curve gated by
//       TargetFormatInfo.isLDRFixed.
//    6. Region readback: Render with a sub-rect produces the
//       right pixel for that rect.
//    7. Reference lifetime: addref/release behaves; safe_release
//       cleans up.
//    8. AsBeautyRasterImage shim: SetPEL / GetPEL / Clear /
//       width/height match the underlying channel.
//    9. Observer attach/detach: idempotent attach, silent detach
//       of unknown observer, observer fires per commit + per frame.
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>

#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobRasterizerOutput.h"
#include "../src/Library/Rendering/FilteredFilm.h"
#include "../src/Library/Rendering/SplatFilm.h"
#include "../src/Library/Rendering/ThreadLocalSplatBuffer.h"
#include "../src/Library/Rendering/RasterizeDispatchers.h"
#include "../src/Library/Rendering/MortonRasterizeSequence.h"
#include "../src/Library/Rendering/BDPTRasterizerBase.h"
#include "../src/Library/Rendering/MLTSpectralRasterizer.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Utilities/ThreadPool.h"
#include "../src/Library/Rendering/ProgressiveFilm.h"
#include "../src/Library/Rendering/BidirectionalRasterizerBase.h"
#include "../src/Library/Rendering/OIDNDenoiser.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRenderObserver.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/CheckedSize.h"

using namespace RISE;
using namespace RISE::FrameStoreOutput;
using RISE::Implementation::AOVBuffers;
using RISE::Implementation::FrameStore;
using RISE::Implementation::FrameStoreBulkBracket;
using RISE::Implementation::MakeAOVPlan;
using RISE::Implementation::PropagateAOVsToFrameStore;

namespace RISE { namespace Implementation {
	IRasterizerOutput* ForTest_CreateCallbackRasterizerOutput(IJobRasterizerOutput& receiver);
	namespace MLTResourceTestHooks {
		using Observer=void (*)(IReference*,bool,void*);
		void SetObserver(Observer observer,void* context) noexcept;
	}
} }

namespace
{
	int gFailCount = 0;
	int gPassCount = 0;

	class ProcessWatchdog
	{
	public:
		ProcessWatchdog() : completed_(std::make_shared<std::atomic<bool>>(false))
		{
			const auto completed = completed_;
			std::thread([completed]() {
				std::this_thread::sleep_for(std::chrono::seconds(30));
				if( !completed->load(std::memory_order_acquire) ) {
					std::fputs("FAIL: FrameStoreTest exceeded 30-second watchdog\n",stderr);
					std::_Exit(124);
				}
			}).detach();
		}

		~ProcessWatchdog()
		{
			completed_->store(true,std::memory_order_release);
		}

	private:
		std::shared_ptr<std::atomic<bool>> completed_;
	};

	void Check( bool cond, const std::string& label )
	{
		if ( cond ) {
			++gPassCount;
			if(label.find("MLT resource exception:")==0 || label.find("manual splat lifecycle:")==0 || label.find("tile exception:")==0 || label.find("full entry exception:")==0 || label.find("splat task exception:")==0 || label.find("film bounds:")==0 || label.find("film footprint:")==0)std::cout << "PASS: " << label << "\n";
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << "\n";
		}
	}

	bool ApproxEq( double a, double b, double eps )
	{
		const double d = a - b;
		return ( d >= -eps ) && ( d <= eps );
	}

	// Test helper: builds a small store and returns the handle.
	// Per RISE convention, `new Reference` starts at refcount 1, so
	// the caller releases exactly once at the end (no extra addref).
	FrameStore* MakeStore( size_t w, size_t h, size_t tile = 8,
	                       std::vector<ChannelId> aovs = {} )
	{
		FrameStore::Spec spec;
		spec.width = w;
		spec.height = h;
		spec.tileEdge = tile;
		spec.aovChannels = std::move( aovs );
		return new FrameStore( spec );
	}

	// A bad lock cleanup must produce a bounded assertion, not hang this
	// standalone executable. A timed-out worker owns the store reference and
	// its result state, so detaching is safe even on the intentional RED path.
	// GREEN always joins; no detached worker or extra reference survives.
	bool ReadAndReacquireTiles(FrameStore& store)
	{
		struct Result {std::atomic<bool> done{false};bool ok=false;};
		auto result=std::make_shared<Result>();store.addref();
		std::thread reader([result,frame=&store]() {
			try {
				std::vector<float> pixels(frame->Width()*frame->Height()*4);
				frame->Render(pixels.data(),frame->Width()*4*sizeof(float),
					Rect(0,0,static_cast<unsigned int>(frame->Height()),static_cast<unsigned int>(frame->Width())),
					TargetFormat::RGBA32F_Linear,ViewTransform::Identity());
				for(size_t y=0;y<frame->TileCountY();++y)for(size_t x=0;x<frame->TileCountX();++x) {
					frame->BeginTile(x,y);frame->EndTile(x,y);
				}
				result->ok=true;
			} catch(...) {}
			frame->release();result->done.store(true,std::memory_order_release);
		});
		const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
		while(!result->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if(!result->done.load(std::memory_order_acquire)){reader.detach();return false;}
		reader.join();return result->ok;
	}


	void TestTileBracketSynchronousReadback()
	{
		using RISE::Implementation::FrameStoreTileBracket;
		struct ReadObserver final : IRenderObserver
		{
			std::shared_ptr<FrameStore> store;
			unsigned int phase = 1;
			unsigned int reads = 0;
			bool exact = true;
			void OnTileComplete( const Rect&, uint64_t ) override
			{
				// Deliberately synchronous full-frame readback. Every other
				// tile owned by this same bracket must already be unlocked.
				std::vector<float> pixels( 4u * 4u * 4u );
				store->Render( pixels.data(), 4u * 4u * sizeof(float), Rect(0,0,4,4),
					TargetFormat::RGBA32F_Linear, ViewTransform::Identity() );
				for( size_t y = 0; y < 4; ++y ) {
					for( size_t x = 0; x < 4; ++x ) {
						const size_t i = (y * 4u + x) * 4u;
						exact &= pixels[i] == float(x + phase) / 8.0f &&
							pixels[i+1] == float(y + phase) / 8.0f &&
							pixels[i+2] == float(x + y + phase) / 16.0f && pixels[i+3] == 0.5f;
					}
				}
				++reads;
			}
		};
		struct Result
		{
			std::shared_ptr<FrameStore> store;
			std::shared_ptr<ReadObserver> observer;
			std::atomic<bool> done{false};
			bool failed = false;
			bool firstPass = false;
			bool retry = false;
		};
		auto result = std::make_shared<Result>();
		result->store = std::shared_ptr<FrameStore>( MakeStore(4,4,2),
			[](FrameStore* store) { store->release(); } );
		result->observer = std::make_shared<ReadObserver>();
		result->observer->store = result->store;
		// The worker owns all three heap objects. On an intentionally bad
		// RED build, no detached worker can access dead stack storage.
		std::thread worker( [result] {
			auto& store = *result->store;
			auto& observer = *result->observer;
			store.AddObserver( &observer );
			try {
				const auto before = store.Generation();
				FrameStoreTileBracket bracket( &store, 0, 0, 2, 2 );
				const auto writePixels = [&] {
					auto* beauty = store.GetChannel<ChannelId::Beauty>();
					auto* alpha = store.GetChannel<ChannelId::Alpha>();
					for( size_t y = 0; y < 4; ++y ) {
						for( size_t x = 0; x < 4; ++x ) {
							beauty->At(x,y) = RISEPel( double(x + observer.phase) / 8.0,
								double(y + observer.phase) / 8.0, double(x + y + observer.phase) / 16.0 );
							alpha->At(x,y) = 0.5;
						}
					}
				};
				writePixels();
				bracket.Release();
				bracket.Release();
				result->firstPass = observer.reads == 4 && observer.exact &&
					store.Generation() == before + 4;
				observer.phase = 2;
				bracket.Acquire();
				writePixels();
				bracket.Release();
				result->retry = observer.reads == 8 && observer.exact &&
					store.Generation() == before + 8;
			} catch( ... ) {
				result->failed = true;
			}
			store.RemoveObserver( &observer );
			result->done.store( true, std::memory_order_release );
		} );
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while( !result->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline )
			std::this_thread::sleep_for( std::chrono::milliseconds(1) );
		const bool completed = result->done.load( std::memory_order_acquire );
		if( completed ) worker.join();
		else worker.detach(); // RED only: its captures own store, observer and result.
		Check( completed,
			"tile exception: synchronous full-frame observer readback completes within deadline" );
		Check( completed && !result->failed && result->firstPass,
			"tile exception: all four notifications read exact committed pixels after every tile unlocks" );
		Check( completed && !result->failed && result->retry,
			"tile exception: bracket reacquire and retry permits four more exact synchronous observer reads" );
	}

	struct ThrowingTileObserver final : IRenderObserver
	{
		unsigned int calls=0;bool fail=true;
		void OnTileComplete(const Rect&,uint64_t) override {
			++calls;if(fail)throw std::runtime_error("observer failure "+std::to_string(calls));
		}
	};

	void TestTileBracketExceptions()
	{
		using RISE::Implementation::FrameStoreTileBracket;
		using RISE::Implementation::FrameStoreBulkBracket;
		const auto release=[](auto* p){if(p)p->release();};
		{
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(4,4,2),release);
			ThrowingTileObserver observer;store->AddObserver(&observer);
			const auto before=store->Generation();std::string error;
			FrameStoreTileBracket bracket(store.get(),0,0,2,2);
			try{bracket.Release();}catch(const std::exception& e){error=e.what();}
			Check(error=="observer failure 1" && observer.calls==4 && store->Generation()==before+4,
				"tile exception: explicit Release propagates first observer error after committing all four tiles");
			bracket.Release();Check(observer.calls==4,"tile exception: repeated Release neither double-unlocks nor notifies");
			store->RemoveObserver(&observer);
			const bool readable=ReadAndReacquireTiles(*store);
			Check(readable,"tile exception: explicit Release leaves every tile readable and reacquirable within deadline");
			if(readable){bracket.Acquire();bracket.Release();}
			Check(readable && store->Generation()==before+12,"tile exception: released bracket can acquire and commit the same four tiles again");
		}
		for(bool bulk : {false,true}) {
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(4,4,2),release);
			ThrowingTileObserver observer;store->AddObserver(&observer);
			std::string error;
			try {
				if(bulk) {
					FrameStoreBulkBracket bracket(store.get(),store->AsBeautyRasterImage());
					throw std::runtime_error("original callback failure");
				} else {
					FrameStoreTileBracket bracket(store.get(),0,0,2,2);
					throw std::runtime_error("original callback failure");
				}
			}catch(const std::exception& e){error=e.what();}
			Check(error=="original callback failure" && observer.calls==4,
				bulk?"tile exception: bulk destructor preserves active callback error and notifies all tiles despite observer errors":
				"tile exception: tile destructor preserves active callback error and notifies all tiles despite observer errors");
			store->RemoveObserver(&observer);
			Check(ReadAndReacquireTiles(*store),bulk?"tile exception: bulk destructor unlocks every tile within deadline":
				"tile exception: tile destructor unlocks every tile within deadline");
		}
		{
			struct Source final : RISERasterImage {
				bool fail=true;mutable unsigned int reads=0;
				Source():RISERasterImage(2,2,RISEColor(.5,.25,.125,.5)){}
				RISEColor GetPEL(unsigned int x,unsigned int y) const override {
					if(fail && ++reads==2)throw std::runtime_error("source read failure");
					return RISERasterImage::GetPEL(x,y);
				}
			} source;
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(2,2,2),release);
			ThrowingTileObserver observer;store->AddObserver(&observer);std::string error;
			try{store->CopyTileFromRasterImage(0,0,source,Rect(0,0,2,2));}
			catch(const std::exception& e){error=e.what();}
			Check(error=="source read failure" && observer.calls==1,"tile exception: CopyTile preserves source error while cleanup observer also throws");
			store->RemoveObserver(&observer);const bool readable=ReadAndReacquireTiles(*store);
			Check(readable,"tile exception: CopyTile source failure releases its tile within deadline");
			if(readable){source.fail=false;store->CopyTileFromRasterImage(0,0,source,Rect(0,0,2,2));}
			bool exact=readable;
			for(unsigned int y=0;y<2;++y)for(unsigned int x=0;x<2;++x) {
				const auto c=store->AsBeautyRasterImage().GetPEL(x,y);
				exact &= c.base.r==.5 && c.base.g==.25 && c.base.b==.125 && c.a==.5;
			}
			Check(exact,"tile exception: CopyTile healthy retry copies all independent RGB and alpha values");
		}
	}

	void TestRasterizerBlockExceptions()
	{
		using namespace RISE::Implementation;
		struct Probe final : PixelBasedRasterizerHelper {
			Probe(IRayCaster* caster,FrameStore* store):Rasterizer(store),PixelBasedRasterizerHelper(caster,store){}
			bool TakeSingleSample(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel&) const override{return false;}
			void IntegratePixel(const RuntimeContext&,unsigned int,unsigned int,unsigned int,const IScene&,
				RISEColor& result,bool,Scalar,Scalar) const override {
				// Test-only instrumentation is not part of the image semantics.
				++integratedCalls;result=RISEColor(.5,.25,.125,.5);
			}
			mutable unsigned int integratedCalls=0;
		};
		struct Output final : IRasterizerOutput,Reference {
			bool fail=true;unsigned int calls=0;
			void OutputIntermediateImage(const IRasterImage&,const Rect*) override {
				++calls;if(fail)throw std::runtime_error("original output failure");
			}
			void OutputImage(const IRasterImage& image,const Rect* region,unsigned int) override{OutputIntermediateImage(image,region);}
		};
		struct Receiver final : IJobRasterizerOutput {
			bool fail=true;unsigned int calls=0;
			bool PremultipliedAlpha() override{return true;}
			int GetColorSpace() override{return 0;}
			bool WantsFloat32() override{return true;}
			void OutputImageRGBA16(const unsigned short*,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int) override{}
			void OutputImageRGBA32F(const float*,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int) override {
				++calls;if(fail)throw std::runtime_error("original receiver failure");
			}
		};
		const auto release=[](auto* p){if(p)p->release();};
		std::unique_ptr<StandardShader,decltype(release)> shader(new StandardShader(std::vector<IShaderOp*>()),release);
		IRayCaster* rawCaster=nullptr;RISE_API_CreateRayCaster(&rawCaster,false,10,*shader,true);
		std::unique_ptr<IRayCaster,decltype(release)> caster(rawCaster,release);
		std::unique_ptr<Scene,decltype(release)> scene(new Scene(),release);
		RandomNumberGenerator random;RuntimeContext context(random,RuntimeContext::PASS_NORMAL,false);
		for(bool animation : {false,true})for(unsigned int mode=0;mode<3;++mode) {
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(4,4,2),release);
			Probe probe(caster.get(),store.get());Output output;Receiver receiver;
			std::unique_ptr<IRasterizerOutput,decltype(release)> adapter(
				mode==1?ForTest_CreateCallbackRasterizerOutput(receiver):nullptr,release);
			if(mode==1)probe.AddRasterizerOutput(adapter.get());else probe.AddRasterizerOutput(&output);
			// mode2 exercises the explicit split Release rather than unwind.
			output.fail=mode==0;ThrowingTileObserver observer;store->AddObserver(&observer);
			const auto run=[&]() {
				const Rect block(0,0,3,3);
				if(animation)probe.SPRasterizeSingleBlockOfAnimation(context,store->AsBeautyRasterImage(),*scene,block,4,
					PixelBasedRasterizerHelper::AnimFrameData{PixelBasedRasterizerHelper::FIELD_BOTH,0,0,0,0});
				else probe.SPRasterizeSingleBlock(context,store->AsBeautyRasterImage(),*scene,block,4);
			};
			std::string error;try{run();}catch(const std::exception& e){error=e.what();}
			const std::string expected=mode==0?"original output failure":mode==1?"original receiver failure":"observer failure 1";
			const std::string label=std::string("tile exception: ")+(animation?"animation ":"static ")+
				(mode==0?"IRasterizerOutput ":mode==1?"receiver ":"split Release ");
			Check(error==expected && observer.calls==4 && probe.integratedCalls==0,
				label+"propagates original failure and releases all four tiles despite throwing observers");
			store->RemoveObserver(&observer);const bool readable=ReadAndReacquireTiles(*store);
			Check(readable,label+"allows bounded full-store read and tile reacquisition");
			if(readable){output.fail=false;receiver.fail=false;run();}
			bool exact=readable && probe.integratedCalls==16 && (mode==1?receiver.calls:output.calls)==3;
			for(unsigned int y=0;y<4;++y)for(unsigned int x=0;x<4;++x) {
				const auto c=store->AsBeautyRasterImage().GetPEL(x,y);
				exact &= c.base.r==.5 && c.base.g==.25 && c.base.b==.125 && c.a==.5;
			}
			Check(exact,label+"healthy retry writes every independent beauty and alpha sample");
			probe.FreeRasterizerOutputs();
		}
	}

	void TestFullEntryExceptions()
	{
		using namespace RISE::Implementation;
		struct SamplingCounts {unsigned int liveClones=0;};
		struct Sampling final : ISampling2D,Reference {
			std::shared_ptr<SamplingCounts> counts;unsigned int samples=5;bool clone=false;
			explicit Sampling(std::shared_ptr<SamplingCounts> value):counts(std::move(value)){}
			Sampling(const Sampling& source):Reference(source),counts(source.counts),samples(source.samples),clone(true){++counts->liveClones;}
			~Sampling() override{if(clone)--counts->liveClones;}
			void SetNumSamples(unsigned int n) override{samples=n;}
			unsigned int GetNumSamples() override{return samples;}
			void GenerateSamplePoints(const RandomNumberGenerator&,SamplesList2D& points) const override{points.assign(samples,Point2(.5,.5));}
			ISampling2D* Clone() const override{return new Sampling(*this);}
		};
		struct Probe final : PixelBasedRasterizerHelper {
			mutable unsigned int acquisitions=0,releases=0;
			mutable std::atomic<bool> sawThreaded{false};
			mutable std::vector<unsigned int> passSamples;
			Probe(IRayCaster* caster,FrameStore* store):Rasterizer(store),PixelBasedRasterizerHelper(caster,store){ForTest_SetThreadCountOverride(1);}
			bool TakeSingleSample(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel&) const override{return false;}
			void PrepareRuntimeContext(RuntimeContext& rc) const override {
				PixelBasedRasterizerHelper::PrepareRuntimeContext(rc);sawThreaded.store(rc.bThreaded);
			}
			void IntegratePixel(const RuntimeContext& rc,unsigned int x,unsigned int y,unsigned int,const IScene&,
				RISEColor& color,bool,Scalar,Scalar) const override {
				sawThreaded.store(rc.bThreaded);
				const auto n=ActiveSampling()?ActiveSampling()->GetNumSamples():1;
				if(rc.pProgressiveFilm) {
					auto& px=rc.pProgressiveFilm->Get(x,y);px.alphaSum+=n;px.weightSum+=n;px.sampleIndex+=n;
				}
				color=RISEColor(0,0,0,1);
			}
			void OnProgressivePassBegin(const IScene&,unsigned int) const override{passSamples.push_back(ActiveSampling()->GetNumSamples());}
			void PrepareImageForNewRender(IRasterImage& image,const Rect*) const override {
				FrameStoreBulkBracket bracket(mFrameStore,image);image.Clear(RISEColor(0,0,0,1),nullptr);
			}
			IRasterImage* AcquireRenderImage(unsigned int w,unsigned int h) const override {
				++acquisitions;return PixelBasedRasterizerHelper::AcquireRenderImage(w,h);
			}
			void ReleaseRenderImage(IRasterImage* image) const override {
				++releases;PixelBasedRasterizerHelper::ReleaseRenderImage(image);
			}
			ISampling2D* AuthoredKernel()const{return pSampling;}
			bool Idle()const{return !mProgressiveFilm && !mTotalProgressiveSPP &&
				mProgressBase==0 && mProgressWeight==0 && mProgressTotal==0 && ActiveSampling()==pSampling;}
		};
		struct Output final : IRasterizerOutput,Reference {
			const Probe& probe;bool fail=true,final=false,stableConfig=true,exactFinal=true;
			unsigned int finalCalls=0;
			std::mutex outputMutex;
			std::string kernel;ISampling2D* authored;
			Output(const Probe& p,ISampling2D* expected):probe(p),kernel(p.ForTest_SamplingKernelName()),authored(expected){}
			void Observe() {
				stableConfig &= probe.AuthoredKernel()==authored && probe.GetSampleCountOverride()==5 &&
					probe.ForTest_SamplingKernelName()==kernel;
			}
			void OutputIntermediateImage(const IRasterImage&,const Rect*) override {
				std::lock_guard<std::mutex> lock(outputMutex);
				Observe();if(fail && !final)throw std::runtime_error("full-entry intermediate failure");
			}
			void OutputImage(const IRasterImage& image,const Rect*,unsigned int) override {
				std::lock_guard<std::mutex> lock(outputMutex);
				Observe();++finalCalls;
				for(unsigned int y=0;y<4;++y)for(unsigned int x=0;x<4;++x) {
					const auto pixel=image.GetPEL(x,y);
					exactFinal &= pixel.base.r==0 && pixel.base.g==0 && pixel.base.b==0 && pixel.a==1;
				}
				if(fail && final)throw std::runtime_error("full-entry final failure");
			}
		};
		const auto release=[](auto* p){if(p)p->release();};
		std::unique_ptr<Job,decltype(release)> job(new Job(),release);
		const double location[]={0,0,2},lookAt[]={0,0,0},up[]={0,1,0},orientation[]={0,0,0},target[]={0,0};
		const bool setup=job->SetFilm(4,4,1) && job->AddPinholeCamera("tile-exception",location,lookAt,up,1,0,0,0,orientation,target);
		Check(setup,"full entry exception: tiny initialized scene fixture installs a four-by-four film and camera");
		if(!setup)return;
		std::unique_ptr<StandardShader,decltype(release)> shader(new StandardShader(std::vector<IShaderOp*>()),release);
		IRayCaster* rawCaster=nullptr;RISE_API_CreateRayCaster(&rawCaster,false,10,*shader,true);
		std::unique_ptr<IRayCaster,decltype(release)> caster(rawCaster,release);
		for(bool pooled:{false,true})for(bool animation:{false,true})for(bool progressive:{false,true})for(bool final:{false,true}) {
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(4,4,2),release);
			auto counts=std::make_shared<SamplingCounts>();
			std::unique_ptr<Sampling,decltype(release)> kernel(new Sampling(counts),release);
			Probe probe(caster.get(),store.get());probe.ForTest_SetThreadCountOverride(pooled?2:1);probe.SubSampleRays(kernel.get(),nullptr);
			ProgressiveConfig config;config.enabled=progressive;config.samplesPerPass=2;probe.SetProgressiveConfig(config);
			Output output(probe,kernel.get());output.final=final;probe.AddRasterizerOutput(&output);
			const auto storeRefs=store->refcount(),kernelRefs=kernel->refcount();
			const auto run=[&]() {
				if(animation)probe.RasterizeSceneAnimation(*job->GetScene(),0,0,1,false,false,nullptr,nullptr,nullptr);
				else probe.RasterizeScene(*job->GetScene(),nullptr,nullptr);
			};
			std::string error;try{run();}catch(const std::exception& e){error=e.what();}
			const std::string label=std::string("full entry exception: ")+(pooled?"worker pool ":"caller thread ")+(animation?"animation ":"static ")+
				(progressive?"progressive ":"single ")+(final?"final ":"intermediate ");
			Check(error==(final?"full-entry final failure":"full-entry intermediate failure") &&
				probe.acquisitions==1 && probe.releases==1 && store->refcount()==storeRefs &&
				(!pooled || final || probe.sawThreaded.load()),
				label+"propagates callback failure and calls virtual image release exactly once without leaking store refs");
			const bool idle=probe.Idle() && counts->liveClones==0 && kernel->refcount()==kernelRefs &&
				probe.AuthoredKernel()==kernel.get() && output.stableConfig;
			Check(idle,label+"restores execution state and keeps authored kernel identity count and ownership stable");
			const bool readable=ReadAndReacquireTiles(*store);
			Check(readable,label+"leaves every tile readable and reacquirable within deadline");
			if(idle && readable){output.fail=false;probe.passSamples.clear();run();}
			const std::vector<unsigned int> expected=progressive?std::vector<unsigned int>{2,2,1}:std::vector<unsigned int>{};
			Check(idle && readable && probe.acquisitions==2 && probe.releases==2 && store->refcount()==storeRefs &&
				probe.Idle() && counts->liveClones==0 && probe.passSamples==expected && output.stableConfig &&
				output.exactFinal && output.finalCalls==(final?2u:1u) && probe.sawThreaded.load()==pooled,
				label+"healthy retry uses exact pass sample counts and releases cloned kernels and image references");
			probe.FreeRasterizerOutputs();
		}
	}

	void TestSplatTaskExceptions()
	{
		using namespace RISE::Implementation;
		struct Source {
			SplatFilm* film=nullptr;
			unsigned int emitted=0;
			void Emit(unsigned int x,unsigned int y) {
				film->Splat(x,y,RISEPel(.5,.25,.125));++emitted;
			}
		};
		struct Probe final : PixelBasedRasterizerHelper {
			Source& source;
			Probe(IRayCaster* caster,FrameStore* store,Source& value):
				Rasterizer(store),PixelBasedRasterizerHelper(caster,store),source(value) {ForTest_SetThreadCountOverride(1);}
			bool TakeSingleSample(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel&) const override{return false;}
			bool ShouldFireToggleObserverEvents() const override{return false;}
			void PrepareImageForNewRender(IRasterImage&,const Rect*) const override{}
			void IntegratePixel(const RuntimeContext&,unsigned int x,unsigned int y,unsigned int,const IScene&,
				RISEColor& color,bool,Scalar,Scalar) const override {source.Emit(x,y);color=RISEColor(0,0,0,1);}
		};
		struct BidirectionalProbe final : BDPTRasterizerBase {
			Source& source;
			BidirectionalProbe(IRayCaster* caster,FrameStore* store,Source& value):
				Rasterizer(store),PixelBasedRasterizerHelper(caster,store),
				BDPTRasterizerBase(caster,3,3,PathGuidingConfig(),StabilityConfig(),store),source(value) {ForTest_SetThreadCountOverride(1);}
			bool TakeSingleSample(const RuntimeContext&,const RasterizerState&,const Ray&,RISEPel&) const override{return false;}
			bool ShouldFireToggleObserverEvents() const override{return false;}
			const char* GetProgressTitle() const override{return "Splat task probe: ";}
			void PreRenderSetup(const IScene&,const Rect*) const override{}
			void IntegratePixel(const RuntimeContext&,unsigned int x,unsigned int y,unsigned int,const IScene&,
				RISEColor& color,bool,Scalar,Scalar) const override {source.Emit(x,y);color=RISEColor(0,0,0,1);}
		};
		struct Observer final : IRenderObserver {
			Source& source;bool fail=true,exact=true;unsigned int calls=0;
			explicit Observer(Source& value):source(value){}
			void OnTileComplete(const Rect&,uint64_t) override {
				auto& tls=GetThreadLocalSplatBuffer();
				if(fail && tls.Size()!=0) {
					exact &= tls.GetBoundFilm()==source.film && tls.Size()==16 && source.emitted==16;
					++calls;throw std::runtime_error("postpixel splat failure");
				}
			}
		};
		struct Result {bool failure=false,clean=false,discarded=false,fresh=false,normalClean=false,onWorker=false;};
		const auto release=[](auto* p){if(p)p->release();};
		std::unique_ptr<Job,decltype(release)> job(new Job(),release);
		const double location[]={0,0,2},lookAt[]={0,0,0},up[]={0,1,0},orientation[]={0,0,0},target[]={0,0};
		std::unique_ptr<StandardShader,decltype(release)> shader(new StandardShader(std::vector<IShaderOp*>()),release);
		IRayCaster* rawCaster=nullptr;RISE_API_CreateRayCaster(&rawCaster,false,10,*shader,true);
		std::unique_ptr<IRayCaster,decltype(release)> caster(rawCaster,release);
		const bool setup=caster && job->SetFilm(4,4,1) &&
			job->AddPinholeCamera("splat-task-exception",location,lookAt,up,1,0,0,0,orientation,target);
		Check(setup,"splat task exception: initialized four-by-four constant-probe scene");
		if(!setup)return;
		const auto caller=std::this_thread::get_id();
		const auto exercise=[&](unsigned int mode) {
			Result result;result.onWorker=std::this_thread::get_id()!=caller;
			const bool pool=mode==2 || mode==3,animation=mode==1 || mode==3,bdpt=mode>=4,progressive=mode==5;
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(4,4,2),release);
			std::unique_ptr<SplatFilm,decltype(release)> film(new SplatFilm(4,4),release);
			Source source;source.film=film.get();Observer observer(source);store->AddObserver(&observer);
			std::unique_ptr<PixelBasedRasterizerHelper,decltype(release)> probe(
				bdpt?static_cast<PixelBasedRasterizerHelper*>(new BidirectionalProbe(caster.get(),store.get(),source)):
				static_cast<PixelBasedRasterizerHelper*>(new Probe(caster.get(),store.get(),source)),release);
			ProgressiveConfig config;config.enabled=progressive;config.samplesPerPass=2;probe->SetProgressiveConfig(config);
			if(progressive) {
				ISampling2D* sampling=nullptr;RISE_API_CreateUniformSampling2D(&sampling,1,1);
				sampling->SetNumSamples(3);probe->SubSampleRays(sampling,nullptr);sampling->release();
			}
			const auto run=[&]() {
				if(pool) {
					std::unique_ptr<MortonRasterizeSequence,decltype(release)> sequence(new MortonRasterizeSequence(8),release);
					sequence->Begin(0,3,0,3);
					if(animation) {
						const PixelBasedRasterizerHelper::AnimFrameData data{PixelBasedRasterizerHelper::FIELD_BOTH,0,0,0,0};
						RasterizeBlockAnimationDispatcher dispatcher(RuntimeContext::PASS_NORMAL,
							store->AsBeautyRasterImage(),*job->GetScene(),*sequence,*probe,nullptr,data,0,0,0);
						dispatcher.DoAnimWork();
					} else {
						RasterizeBlockDispatcher dispatcher(RuntimeContext::PASS_NORMAL,
							store->AsBeautyRasterImage(),*job->GetScene(),*sequence,*probe,nullptr,0,0,0);
						dispatcher.DoWork();
					}
				} else if(animation)probe->RasterizeSceneAnimation(*job->GetScene(),0,0,1,false,false,nullptr,nullptr,nullptr);
				else probe->RasterizeScene(*job->GetScene(),nullptr,nullptr);
			};
			std::string error;try{run();}catch(const std::exception& e){error=e.what();}
			auto& tls=GetThreadLocalSplatBuffer();
			result.failure=error=="postpixel splat failure" && observer.exact && observer.calls==4;
			// Inspect the actual rendering thread before destroying its film.
			// Never dereference a stale binding, even in the intentional RED.
			result.clean=tls.GetBoundFilm()==nullptr && tls.Size()==0;
			std::unique_ptr<RISERasterImage,decltype(release)> discarded(new RISERasterImage(4,4,RISEColor(0,0,0,1)),release);
			film->Resolve(*discarded,1);
			result.discarded=true;
			for(unsigned int y=0;y<4;++y)for(unsigned int x=0;x<4;++x) {
				const auto pixel=discarded->GetPEL(x,y);
				result.discarded &= pixel.base.r==0 && pixel.base.g==0 && pixel.base.b==0;
			}
			if(!result.clean)tls.DiscardAndUnbind(); // Safe RED cleanup; retry stays gated.
			film.reset(); // Training-style immediate release before reuse of this same TLS.
			film.reset(new SplatFilm(5,5));source.film=film.get();source.emitted=0;observer.fail=false;
			if(result.clean)run();
			result.normalClean=result.clean && tls.GetBoundFilm()==nullptr && tls.Size()==0;
			std::unique_ptr<RISERasterImage,decltype(release)> fresh(new RISERasterImage(5,5,RISEColor(0,0,0,1)),release);
			film->Resolve(*fresh,1);
			const unsigned int passes=progressive?2:1;
			result.fresh=result.clean && source.emitted==16*passes;
			for(unsigned int y=0;y<5;++y)for(unsigned int x=0;x<5;++x) {
				const auto pixel=fresh->GetPEL(x,y);const double scale=(x<4 && y<4)?passes:0;
				result.fresh &= pixel.base.r==.5*scale && pixel.base.g==.25*scale && pixel.base.b==.125*scale;
			}
			store->RemoveObserver(&observer);return result;
		};
		const char* labels[]={"caller static","caller animation","pool static dispatcher","pool animation dispatcher",
			"BDPT bespoke caller","BDPT progressive caller"};
		for(unsigned int mode=0;mode<6;++mode) {
			Result result;
			if(mode==2 || mode==3) {
				// Submit (not ParallelFor) ensures failure inspection, film death
				// and retry all run on this one real persistent worker thread.
				ThreadPool pool(1,{});std::promise<Result> promise;auto future=promise.get_future();
				pool.Submit([&]{try{promise.set_value(exercise(mode));}catch(...){promise.set_exception(std::current_exception());}});
				result=future.get();
			} else result=exercise(mode);
			const std::string label=std::string("splat task exception: ")+labels[mode]+" ";
			Check(result.failure,label+"throws only after sixteen real pending splats reach the postpixel observer");
			Check(result.clean && (mode<2 || mode>3 || result.onWorker),label+"same-thread catch sees no binding or pending records before film destruction");
			Check(result.discarded && result.fresh,label+"discarded film stays black and fresh differently-sized film has exact uncontaminated batch sums");
			Check(result.normalClean,label+"healthy retry flushes all normal splats and unbinds before task exit");
		}
	}

	void TestManualSplatLifecycle()
	{
		using namespace RISE::Implementation;
		const auto release=[](auto* p){if(p)p->release();};
		// Owners precede the task scope: its destructor must unbind before
		// either the manual film or its target image can be destroyed.
		std::unique_ptr<SplatFilm,decltype(release)> film(new SplatFilm(1,1),release);
		std::unique_ptr<RISERasterImage,decltype(release)> image(new RISERasterImage(1,1,RISEColor(0,0,0,1)),release);
		ThreadLocalSplatBufferTaskScope task;
		auto& tls=GetThreadLocalSplatBuffer();
		film->Splat(0u,0u,RISEPel(.5,.25,.125));
		film->Resolve(*image,1);
		auto pixel=image->GetPEL(0,0);
		Check(pixel.base.r==0 && pixel.base.g==0 && pixel.base.b==0 && pixel.a==1 &&
			tls.Size()==1 && tls.GetBoundFilm()==film.get(),
			"manual splat lifecycle: Resolve leaves pending caller records and binding untouched");
		film->FlushCallingThreadBuffer();
		film->Resolve(*image,1);
		pixel=image->GetPEL(0,0);
		Check(pixel.base.r==.5 && pixel.base.g==.25 && pixel.base.b==.125 && pixel.a==1 &&
			tls.Size()==0 && tls.GetBoundFilm()==film.get(),
			"manual splat lifecycle: explicit caller flush commits exact RGB and retains empty binding");
		FlushCallingThreadSplatBuffer();
		Check(tls.Size()==0 && tls.GetBoundFilm()==nullptr,
			"manual splat lifecycle: task completion flush unbinds before film owner release");

		struct Filter final : IPixelFilter,Reference {
			double scale=1;
			Scalar warp(const RandomNumberGenerator&,const Point2& in,Point2& out)const override{out=in;return 1;}
			Scalar warpOnScreen(const RandomNumberGenerator&,const Point2& in,Point2& out,unsigned int,unsigned int)const override{out=in;return 1;}
			void GetFilterSupport(Scalar& w,Scalar& h)const override{w=h=1;}
			Scalar EvaluateFilter(Scalar x,Scalar y)const override{return scale*(2-std::abs(x))*(2-std::abs(y));}
		};
		const auto paint=[&](double scale) {
			std::unique_ptr<SplatFilm,decltype(release)> filtered(new SplatFilm(3,3),release);
			std::unique_ptr<RISERasterImage,decltype(release)> target(new RISERasterImage(3,3,RISEColor(0,0,0,1)),release);
			Filter filter;filter.scale=scale;ThreadLocalSplatBufferTaskScope filteredTask;
			filtered->SplatFiltered(1.5,1.5,RISEPel(.5,.25,.125),filter);
			filtered->FlushCallingThreadBuffer();filtered->Resolve(*target,1);
			std::vector<RISEColor> result;
			for(unsigned int y=0;y<3;++y)for(unsigned int x=0;x<3;++x)result.push_back(target->GetPEL(x,y));
			return result;
		};
		const auto unit=paint(1),twice=paint(2);bool invariant=true;
		// Independent outer-product weights {1,2,1} x {1,2,1} sum to16.
		// Multiplication by2 changes the continuous integral but not the
		// normalized discrete distribution or its exact binary RGB values.
		for(unsigned int y=0;y<3;++y)for(unsigned int x=0;x<3;++x){const auto i=y*3+x;
			const double weight=(x==1?2:1)*(y==1?2:1);
			invariant &= unit[i].base.r==weight/32 && unit[i].base.g==weight/64 && unit[i].base.b==weight/128 && unit[i].a==1 &&
				twice[i].base.r==unit[i].base.r && twice[i].base.g==unit[i].base.g && twice[i].base.b==unit[i].base.b && twice[i].a==1;
		}
		Check(invariant,"manual splat lifecycle: positive kernel scaling preserves the independent exact nine-pixel normalized distribution");
	}

	void TestMLTResourceExceptions()
	{
		using namespace RISE::Implementation;
		struct Resources {
			std::vector<IReference*> samplers;
			std::vector<IReference*> films;
			~Resources(){Clear();}
			void Clear(){for(auto* p:samplers)p->release();for(auto* p:films)p->release();samplers.clear();films.clear();}
			static void Observe(IReference* object,bool isFilm,void* context) {
				auto& self=*static_cast<Resources*>(context);
				(isFilm?self.films:self.samplers).push_back(object);object->addref();
			}
			bool Exact(unsigned int refs)const {
				if(samplers.size()!=2 || films.size()!=1)return false;
				for(auto* p:samplers)if(p->refcount()!=refs)return false;
				for(auto* p:films)if(p->refcount()!=refs)return false;
				return true;
			}
		};
		struct ObserveScope {
			explicit ObserveScope(Resources& resources){MLTResourceTestHooks::SetObserver(Resources::Observe,&resources);}
			~ObserveScope(){MLTResourceTestHooks::SetObserver(nullptr,nullptr);}
		};
		struct ProgressCallback final : IProgressCallback {
			Resources& resources;bool fail=true,onTitle=true,rendering=false,ownedAtThrow=false;
			unsigned int renderingTitles=0,renderingProgress=0;
			explicit ProgressCallback(Resources& value):resources(value){}
			void SetTitle(const char* title) override {
				rendering=std::strcmp(title,"MLT Rendering: ")==0 || std::strcmp(title,"MLT Spectral Rendering: ")==0;
				if(rendering){++renderingTitles;if(fail && onTitle){ownedAtThrow=resources.Exact(2);throw std::runtime_error("MLT phase-three title failure");}}
			}
			bool Progress(const double,const double) override {
				if(rendering){++renderingProgress;if(fail && !onTitle){ownedAtThrow=resources.Exact(2);throw std::runtime_error("MLT phase-three progress failure");}}
				return true;
			}
		};
		struct Output final : IRasterizerOutput,Reference {
			unsigned int intermediate=0,final=0;bool valid=true;std::vector<RISEColor> pixels;
			void OutputIntermediateImage(const IRasterImage&,const Rect*) override{++intermediate;}
			void OutputImage(const IRasterImage& image,const Rect*,unsigned int) override {
				++final;valid &= image.GetWidth()==2 && image.GetHeight()==2;pixels.clear();double total=0;
				for(unsigned int y=0;y<2;++y)for(unsigned int x=0;x<2;++x){const auto p=image.GetPEL(x,y);pixels.push_back(p);
					valid &= std::isfinite(p.base.r) && std::isfinite(p.base.g) && std::isfinite(p.base.b) && p.a==1;
					total+=p.base.r+p.base.g+p.base.b;}
				valid &= total>0;
			}
		};
		const auto release=[](auto* p){if(p)p->release();};
		std::unique_ptr<Job,decltype(release)> job(new Job(),release);
		const double location[]={0,0,2},lookAt[]={0,0,0},up[]={0,1,0},orientation[]={0,0,0},target[]={0,0};
		std::unique_ptr<StandardShader,decltype(release)> shader(new StandardShader(std::vector<IShaderOp*>()),release);
		IRayCaster* rawCaster=nullptr;RISE_API_CreateRayCaster(&rawCaster,true,3,*shader,true);
		std::unique_ptr<IRayCaster,decltype(release)> caster(rawCaster,release);
		IPainter* rawPainter=nullptr;RISE_API_CreateUniformColorPainter(&rawPainter,RISEPel(1,1,1),eSpectrumKind_Illuminant);
		std::unique_ptr<IPainter,decltype(release)> painter(rawPainter,release);
		IRadianceMap* rawMap=nullptr;if(painter)RISE_API_CreateRadianceMap(&rawMap,*painter,1);
		std::unique_ptr<IRadianceMap,decltype(release)> map(rawMap,release);
		// BDPT needs finite scene bounds for its synthetic environment endpoint.
		// This sphere lies entirely behind every camera ray; its bounding
		// sphere encloses the camera, while its actual surface does not.
		const double behindCamera[]={0,0,3.25},unitScale[]={1,1,1};
		const bool setup=caster && map && job->SetFilm(2,2,1) &&
			job->AddPinholeCamera("MLT-resource-exception",location,lookAt,up,1,0,0,0,orientation,target) &&
			job->AddSphereGeometry("MLT-resource-bound",1) &&
			job->AddObject("MLT-resource-bound","MLT-resource-bound","none",nullptr,nullptr,RadianceMapConfig(),
				behindCamera,orientation,unitScale,true,true) && job->SetGlobalRadianceMap(map.get());
		Check(setup,"MLT resource exception: actual four-pixel scene has a positive constant environment");
		if(!setup)return;
		for(unsigned int family=0;family<3;++family)for(bool animation:{false,true})for(bool onTitle:{true,false}) {
			std::unique_ptr<FrameStore,decltype(release)> store(MakeStore(2,2,2),release);
			// At least twenty mutations per chain keep the existing timed-round
			// fallback bounded even when bootstrap finishes within one clock tick.
			std::unique_ptr<MLTRasterizer,decltype(release)> rasterizer(family==0?
				new MLTRasterizer(caster.get(),3,3,8,2,16,.3,store.get()):
				new MLTSpectralRasterizer(caster.get(),3,3,8,2,16,.3,380,780,1,family==2,store.get()),release);
			Resources resources;ObserveScope observe(resources);ProgressCallback progress(resources);progress.onTitle=onTitle;
			Output output;rasterizer->AddRasterizerOutput(&output);rasterizer->SetProgressCallback(&progress);
			const auto run=[&](){
				if(animation)rasterizer->RasterizeSceneAnimation(*job->GetScene(),0,0,1,false,false,nullptr,nullptr,nullptr);
				else rasterizer->RasterizeScene(*job->GetScene(),nullptr,nullptr);
			};
			std::string error;try{run();}catch(const std::exception& e){error=e.what();}
			const std::string label=std::string("MLT resource exception: ")+(family==0?"RGB ":family==1?"spectral NM ":"spectral HWSS ")+(animation?"animation ":"still ")+(onTitle?"title ":"progress ");
			Check(error==(onTitle?"MLT phase-three title failure":"MLT phase-three progress failure") &&
				progress.renderingTitles==1 && progress.renderingProgress==(onTitle?0u:1u),label+"propagates phase-three callback failure before mutation work");
			Check(progress.ownedAtThrow && resources.Exact(1),label+"releases both actual chain samplers and film leaving only tester references");
			Check(output.final==0 && output.intermediate==0 && GetThreadLocalSplatBuffer().Size()==0 &&
				GetThreadLocalSplatBuffer().GetBoundFilm()==nullptr,label+"publishes no image and leaves caller TLS unbound on early failure");
			// Retry without reinstalling the callback: an animation must not
			// leave its borrowed stack frame-progress adapter in authored state.
			resources.Clear();progress.fail=false;error.clear();try{run();}catch(const std::exception& e){error=e.what();}
			Check(error.empty() && resources.Exact(1) && progress.renderingTitles==2 && progress.renderingProgress>0,
				label+"healthy retry completes with exactly balanced new chain and film references");
			Check(output.final==1 && output.valid && output.pixels.size()==4,label+"healthy retry publishes one finite positive four-pixel opaque image");
			const auto expected=output.pixels;resources.Clear();error.clear();try{run();}catch(const std::exception& e){error=e.what();}
			bool same=expected.size()==4 && output.pixels.size()==4;
			if(same)for(std::size_t i=0;i<4;++i){const auto& a=expected[i];const auto& b=output.pixels[i];
				same &= ApproxEq(a.base.r,b.base.r,1e-10) && ApproxEq(a.base.g,b.base.g,1e-10) && ApproxEq(a.base.b,b.base.b,1e-10) && a.a==b.a;}
			Check(error.empty() && resources.Exact(1) && output.final==2 && output.valid && same,
				label+"second healthy render preserves the deterministic image and exact ownership balance");
			rasterizer->SetProgressCallback(nullptr);rasterizer->FreeRasterizerOutputs();
		}
	}

	bool TestFilmAllocationBounds()
	{
		using namespace RISE::Implementation;
		struct SplatProbe final : SplatFilm {
			using SplatFilm::SplatFilm;
			static size_t Count(unsigned int w,unsigned int h){return CheckedPixelCount(w,h);}
			size_t Rows()const{return rowMutexes.size();}
		};
		struct FilteredProbe final : FilteredFilm {
			using FilteredFilm::FilteredFilm;
			static size_t Count(unsigned int w,unsigned int h){return CheckedPixelCount(w,h);}
			size_t Rows()const{return rowMutexes.size();}
		};
		const auto release=[](auto* p){if(p)p->release();};
		const auto rejects=[](auto operation) {
			try{operation();}catch(const std::length_error&){return true;}catch(...){return false;}
			return false;
		};
		const auto nativeMax=std::numeric_limits<size_t>::max();
		const auto unsignedMax=std::numeric_limits<unsigned int>::max();
		Check(CheckedPixelCount<std::uint64_t>(3,2)==6,"film bounds: ordinary checked product preserves exact pixel count");
		Check(CheckedPixelCount<std::uint64_t>(0,nativeMax)==0 && CheckedPixelCount<std::uint64_t>(nativeMax,0)==0,
			"film bounds: empty dimensions require no product or byte allocation");
		Check(rejects([&]{CheckedPixelCount<std::uint64_t>(nativeMax,2);}) &&
			rejects([&]{CheckedPixelCount<std::uint64_t>(nativeMax/sizeof(std::uint64_t)+1,1);}),
			"film bounds: native product and native byte overflow throw length_error before allocation");
		Check(CheckedPixelCount<unsigned char>(4,2,8)==8 && rejects([]{CheckedPixelCount<unsigned char>(4,3,8);}),
			"film bounds: explicit index-count budget includes its endpoint and rejects the next row");
		// Old code allocates only65536pixels and65537row mutexes here,
		// so normal RED never requests giant storage or performs an OOB write.
		const bool splatGuarded=rejects([&]{std::unique_ptr<SplatProbe,decltype(release)> film(new SplatProbe(65536,65537),release);});
		Check(splatGuarded,"film bounds: actual SplatFilm rejects a wrapped uint32 pixel product before allocation");
		const auto fullIndexDomain=std::uint64_t(1)<<32;
		const bool wide=sizeof(size_t)>4;
		Check(wide?SplatProbe::Count(65536,65536)==fullIndexDomain:
			rejects([]{SplatProbe::Count(65536,65536);}),"film bounds: pure SplatFilm preflight permits the full representable uint32 index domain");
		Check(wide?FilteredProbe::Count(65536,65536)==fullIndexDomain:
			rejects([]{FilteredProbe::Count(65536,65536);}),"film bounds: pure FilteredFilm preflight retains native count budget without allocating backing storage");
		// Both old products wrap toONEelement, a bounded constructor canary.
		const bool progressiveGuarded=rejects([&]{ProgressiveFilm film(unsignedMax,unsignedMax);});
		Check(progressiveGuarded,"film bounds: actual ProgressiveFilm rejects native byte overflow instead of a one-pixel wrapped allocation");
		const bool channelGuarded=rejects([&]{Channel<std::uint64_t> channel(nativeMax,nativeMax);});
		Check(channelGuarded,"film bounds: actual Channel rejects native product overflow instead of a one-element wrapped allocation");
		Check(wide?(CheckedPixelCount<ProgressivePixel>(65536,65536)==fullIndexDomain &&
			CheckedPixelCount<std::uint64_t>(65536,65536)==fullIndexDomain):
			rejects([]{CheckedPixelCount<ProgressivePixel>(65536,65536);}),
			"film bounds: progressive uint64 aggregates and Channel retain native-width pure storage counts");
		Check(rejects([&]{FilteredProbe::Count(unsignedMax,unsignedMax);}) &&
			rejects([&]{CheckedPixelCount<ProgressivePixel>(unsignedMax,unsignedMax);}),
			"film bounds: pure filtered and progressive native byte preflights reject without backing allocation");
		const unsigned int empty[][2]={{0,0},{0,2},{2,0}};
		for(const auto& dimensions:empty) {
			std::unique_ptr<SplatProbe,decltype(release)> splats(new SplatProbe(dimensions[0],dimensions[1]),release);
			std::unique_ptr<FilteredProbe,decltype(release)> filtered(new FilteredProbe(dimensions[0],dimensions[1]),release);
			ProgressiveFilm progressive(dimensions[0],dimensions[1]);Channel<float> channel(dimensions[0],dimensions[1]);
			std::unique_ptr<RISERasterImage,decltype(release)> target(new RISERasterImage(1,1,RISEColor(.75,.5,.25,1)),release);
			splats->Resolve(*target,1);splats->Unresolve(*target,1);splats->Clear();
			filtered->Resolve(*target);filtered->Unresolve(*target);filtered->Clear();
			progressive.Resolve(*target);progressive.Clear();channel.Fill(2);
			bool exact=splatGuarded && progressiveGuarded && channelGuarded && splats->Rows()==0 && filtered->Rows()==0 &&
				progressive.CountDone(1)==0 && progressive.IsTileDone(Rect(0,0,unsignedMax,unsignedMax),1) && channel.Size()==0;
			if(exact && dimensions[0]==0 && dimensions[1]!=0)exact=channel.Row(0)==channel.Data();
			if(exact) {
				const SplatFilm::BatchRecord record{0,RISEPel(1,0,0)};splats->BatchCommit(&record,1);
				const auto pixel=target->GetPEL(0,0);exact=pixel.base.r==.75 && pixel.base.g==.5 && pixel.base.b==.25 && pixel.a==1;
			}
			Check(exact,"film bounds: zero-axis films allocate no rows, aggregates are empty and resolves preserve every target channel");
		}
		ProgressiveFilm progressive(3,2);auto& last=progressive.Get(2,1);last.sampleIndex=4;last.converged=true;
		Check(progressiveGuarded && progressive.CountDone(4)==1 && progressive.IsConverged(2,1) &&
			progressive.IsTileDone(Rect(1,2,unsignedMax,unsignedMax),4) && progressive.IsTileDone(Rect(8,8,9,9),4),
			"film bounds: progressive last-row indexing and clipped or outside tile aggregates have exact completion semantics");
		progressive.Clear();Check(progressive.CountDone(4)==0 && !progressive.IsConverged(2,1),
			"film bounds: progressive Clear resets the complete last row");
		Channel<std::uint64_t> channel(3,2);channel.Fill(42);channel.At(2,1)=99;
		Check(channel.Size()==6 && channel.Row(1)[2]==99 && channel.Data()[5]==99 && channel.At(2,0)==42,
			"film bounds: Channel normal last-row storage and fill preserve exact values");
		return splatGuarded;
	}

	void TestFilmFootprintBounds(bool splatGuarded)
	{
		using namespace RISE::Implementation;
		struct Filter final : IPixelFilter,Reference {
			double support=1;bool fail=false;
			Scalar warp(const RandomNumberGenerator&,const Point2& input,Point2& output)const override{output=input;return 1;}
			Scalar warpOnScreen(const RandomNumberGenerator&,const Point2& input,Point2& output,unsigned int,unsigned int)const override{output=input;return 1;}
			void GetFilterSupport(Scalar& w,Scalar& h)const override{w=h=support;}
			Scalar EvaluateFilter(Scalar,Scalar)const override {
				if(fail)throw std::runtime_error("filter evaluation failure");
				return 1;
			}
		};
		struct Probe final : FilteredFilm {
			using FilteredFilm::FilteredFilm;
			bool TryRow(unsigned int y)const {if(!rowMutexes[y]->try_lock())return false;rowMutexes[y]->unlock();return true;}
			void UnlockKnownOwnedRow(unsigned int y){rowMutexes[y]->unlock();}
		};
		const auto release=[](auto* p){if(p)p->release();};
		std::unique_ptr<Probe,decltype(release)> probe(new Probe(2,2),release);Filter filter;filter.fail=true;
		bool reported=false;try{probe->Splat(.5,.5,XYZPel(.25,.25,.25),filter);}catch(const std::runtime_error& e){reported=std::string(e.what())=="filter evaluation failure";}
		Check(reported,"film footprint: throwing EvaluateFilter propagates its original exception");
		bool rowsFree=false;std::thread reader([&]{rowsFree=probe->TryRow(0) && probe->TryRow(1);});reader.join();
		Check(rowsFree,"film footprint: another thread can try_lock every row after EvaluateFilter throws");
		// Old failure leaves row0 locked by THIS caller. Repair only that
		// known ownership before teardown, and skip its unsafe cast paths.
		if(!rowsFree)probe->UnlockKnownOwnedRow(0);
		probe->Clear();filter.fail=false;
		std::unique_ptr<RISERasterImage,decltype(release)> target(new RISERasterImage(2,2,RISEColor(0,0,0,1)),release);
		if(rowsFree){probe->Splat(.5,.5,XYZPel(.25,.25,.25),filter);probe->Resolve(*target);}
		bool retry=rowsFree;
		for(unsigned int y=0;y<2;++y)for(unsigned int x=0;x<2;++x) {
			const auto pixel=target->GetPEL(x,y);
			retry &= ApproxEq(pixel.base.r,.3011985,1e-12) && ApproxEq(pixel.base.g,.237073,1e-12) && ApproxEq(pixel.base.b,.227229,1e-12);
		}
		Check(retry,"film footprint: healthy retry resolves the fixed XYZ-to-Rec709 oracle after row-lock recovery");
		const auto paint=[&](bool splat,double support,double sx,double sy,unsigned int extent) {
			if(!(splat?splatGuarded:rowsFree))return false;
			Filter input;input.support=support;
			std::unique_ptr<RISERasterImage,decltype(release)> image(new RISERasterImage(4,4,RISEColor(0,0,0,1)),release);
			if(splat) {
				std::unique_ptr<SplatFilm,decltype(release)> film(new SplatFilm(4,4),release);
				ThreadLocalSplatBufferTaskScope task;
				film->SplatFiltered(sx,sy,RISEPel(.5,.25,.125),input);FlushCallingThreadSplatBuffer();film->Resolve(*image,1);
			} else {
				std::unique_ptr<FilteredFilm,decltype(release)> film(new FilteredFilm(4,4),release);
				film->Splat(sx,sy,XYZPel(.25,.25,.25),input);film->Resolve(*image);
			}
			bool exact=true;
			for(unsigned int y=0;y<4;++y)for(unsigned int x=0;x<4;++x) {
				const auto pixel=image->GetPEL(x,y);const bool active=x<extent && y<extent;
				const double scale=active?(splat?1.0/(extent*extent):1):0;
				exact &= ApproxEq(pixel.base.r,(splat?.5:.3011985)*scale,1e-12) &&
					ApproxEq(pixel.base.g,(splat?.25:.237073)*scale,1e-12) && ApproxEq(pixel.base.b,(splat?.125:.227229)*scale,1e-12);
			}
			return exact;
		};
		const double huge=std::strtod("0x1.fffffffffffffp1023",nullptr),nan=std::strtod("nan",nullptr),
			pinf=std::strtod("1e999",nullptr),ninf=std::strtod("-1e999",nullptr);
		for(bool splat:{false,true}) {
			const std::string label=std::string("film footprint: ")+(splat?"SplatFilm ":"FilteredFilm ");
			Check(paint(splat,huge,1.5,1.5,4),label+"DBL_MAX support clips before narrowing and matches independent finite pixel values");
			Check(paint(splat,huge,-10,.5,splat?0:4),label+"retains its documented off-image sample membership policy");
			for(double bad:{nan,pinf,ninf,-1.0})Check(paint(splat,bad,1.5,1.5,0),label+"invalid support performs no writes");
			bool finitePositions=true;for(double bad:{nan,pinf,ninf})finitePositions &= paint(splat,1,bad,.5,0) && paint(splat,1,.5,bad,0);
			Check(finitePositions,label+"nonfinite screen positions perform no writes");
			Check(paint(splat,1,huge,huge,0),label+"finite nonintersecting extreme screen positions reject before narrowing");
			Check(paint(splat,1,1.5,1.5,3),label+"ordinary unit support retains the independent three-by-three footprint oracle");
		}
	}

	void TestFilmBoundsAndFootprints()
	{
		TestFilmFootprintBounds(TestFilmAllocationBounds());
	}

	// ─── Section 1: construction & geometry ───────────────────────
	void TestConstructionAndGeometry()
	{
		FrameStore* store = MakeStore( 64, 32, 16 );

		Check( store->Width()  == 64, "Width=64" );
		Check( store->Height() == 32, "Height=32" );
		Check( store->TileEdge() == 16, "TileEdge=16" );
		Check( store->TileCountX() == 4, "TileCountX = 64/16 = 4" );
		Check( store->TileCountY() == 2, "TileCountY = 32/16 = 2" );

		// Beauty + Alpha auto-allocated.
		Check( store->HasChannel( ChannelId::Beauty ), "Beauty auto-allocated" );
		Check( store->HasChannel( ChannelId::Alpha ),  "Alpha auto-allocated" );
		// AOVs not requested → absent.
		Check( !store->HasChannel( ChannelId::Albedo ), "Albedo not allocated" );
		Check( !store->HasChannel( ChannelId::Normal ), "Normal not allocated" );

		// Initial state: black + opaque.
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		Check( beauty != nullptr, "Beauty channel pointer non-null" );
		Check( beauty->Width() == 64 && beauty->Height() == 32, "Beauty dims" );
		Check( beauty->At( 0, 0 ).r == 0.0 && beauty->At( 0, 0 ).g == 0.0
		    && beauty->At( 0, 0 ).b == 0.0, "Initial Beauty = (0,0,0)" );

		auto* alpha = store->GetChannel<ChannelId::Alpha>();
		Check( alpha != nullptr, "Alpha channel pointer non-null" );
		Check( alpha->At( 0, 0 ) == 1.0f, "Initial Alpha = 1.0" );

		// Ragged tile geometry: 65x33 tile=16 → 5x3 tiles, last tile partially out.
		FrameStore* ragged = MakeStore( 65, 33, 16 );
		Check( ragged->TileCountX() == 5, "Ragged TileCountX" );
		Check( ragged->TileCountY() == 3, "Ragged TileCountY" );
		ragged->release();

		store->release();
	}

	// ─── Section 2: AOV channel allocation ────────────────────────
	void TestAOVChannels()
	{
		FrameStore* store = MakeStore( 16, 16, 8,
			{ ChannelId::Albedo, ChannelId::Normal, ChannelId::Depth,
			  ChannelId::ObjectId, ChannelId::PrimitiveId } );

		Check( store->HasChannel( ChannelId::Albedo ),     "Albedo opt-in allocated" );
		Check( store->HasChannel( ChannelId::Normal ),     "Normal opt-in allocated" );
		Check( store->HasChannel( ChannelId::Depth ),      "Depth opt-in allocated" );
		Check( store->HasChannel( ChannelId::ObjectId ),   "ObjectId opt-in allocated" );
		Check( store->HasChannel( ChannelId::PrimitiveId ), "PrimitiveId opt-in allocated" );

		// Compile-time typed access:
		Channel<RISEPel>*  ab = store->GetChannel<ChannelId::Albedo>();
		Channel<Vector3>*  nm = store->GetChannel<ChannelId::Normal>();
		Channel<float>*    dp = store->GetChannel<ChannelId::Depth>();
		Channel<uint32_t>* oi = store->GetChannel<ChannelId::ObjectId>();
		Channel<uint32_t>* pi = store->GetChannel<ChannelId::PrimitiveId>();

		Check( ab && ab->Width() == 16 && ab->Height() == 16, "Albedo dims" );
		Check( nm && nm->Width() == 16, "Normal dims" );
		Check( dp && dp->Width() == 16, "Depth dims" );
		Check( oi && oi->Width() == 16, "ObjectId dims" );
		Check( pi && pi->Width() == 16, "PrimitiveId dims" );

		// Channel<T>::At and Fill work.
		nm->At( 5, 7 ) = Vector3( 1.0, 0.0, 0.0 );
		Check( nm->At( 5, 7 ).x == 1.0 && nm->At( 5, 7 ).y == 0.0,
			"Channel<Vector3>::At assignment" );
		dp->Fill( 42.0f );
		Check( dp->At( 0, 0 ) == 42.0f && dp->At( 15, 15 ) == 42.0f,
			"Channel<float>::Fill" );

		store->release();
	}

	// ─── Section 2b: planned AOV bridge + exact storage ───────
	void TestPlannedAOVBridge()
	{
		const unsigned int w = 2, h = 1;
		const size_t pixels = static_cast<size_t>( w ) * h;

		AOVBuffers depthOnly( w, h, AOVBuffers::Plan( false, false, true ) );
		Check( depthOnly.GetAlbedoPtr() == nullptr && depthOnly.GetNormalPtr() == nullptr,
			"depth-only AOV plan does not allocate RGB planes" );
		Check( depthOnly.GetDepthPtr() != nullptr, "depth-only AOV plan allocates depth" );
		Check( depthOnly.StorageBytes() == pixels * 2u * sizeof(float),
			"depth-only AOV plan costs exactly 8 bytes/pixel including hit weights" );

		AOVBuffers full( w, h, AOVBuffers::Plan( true, true, true ) );
		Check( full.StorageBytes() == pixels * 8u * sizeof(float),
			"agent AOV tap costs exactly 32 bytes/pixel" );
		full.AccumulateAlbedo( 0, 0, RISEPel( 0.2, 0.4, 0.6 ), 2.0 );
		full.AccumulateNormal( 0, 0, Vector3( -1.0, 0.5, 1.0 ), 2.0 );
		full.AccumulateDepth( 0, 0, 7.5, 2.0 );
		full.Normalize( 0, 0, 0.5 );
		Check( full.HasData(), "planned AOV tap reports accumulated data" );
		Check( full.HasAlbedoData() && full.HasNormalData() && full.HasDepthData() &&
		       !full.NeedsFallback(),
			"planned AOV readiness is tracked independently per plane" );

		AOVBuffers depthFirst( w, h, AOVBuffers::Plan( true, true, true ) );
		depthFirst.AccumulateDepth( 0, 0, 3.0, 1.0 );
		const AOVBuffers::Plan missingGuides = depthFirst.MissingPlan();
		Check( missingGuides.albedo && missingGuides.normal && !missingGuides.depth &&
		       depthFirst.NeedsFallback(),
			"depth activity cannot suppress missing albedo/normal completion" );

		FrameStore* store = MakeStore( w, h, 1,
			{ ChannelId::Albedo, ChannelId::Normal, ChannelId::Depth } );
		PropagateAOVsToFrameStore( store, full );
		auto* albedo = store->GetChannel<ChannelId::Albedo>();
		auto* normal = store->GetChannel<ChannelId::Normal>();
		auto* depth = store->GetChannel<ChannelId::Depth>();
		Check( ApproxEq( albedo->At( 0, 0 ).r, 0.2, 1e-6 ) &&
		       ApproxEq( albedo->At( 0, 0 ).b, 0.6, 1e-6 ),
			"AOV bridge propagates albedo" );
		// Normalize() renormalizes the accumulated normal to unit length:
		// the weighted arithmetic mean of unit normals is not itself a
		// unit vector, and OIDN's normal guide is contractually one.
		// (-1, 0.5, 1) has length exactly 1.5, so the propagated normal
		// must be that direction scaled by 1/1.5 -- direction preserved,
		// magnitude restored.
		Check( ApproxEq( normal->At( 0, 0 ).x, -1.0/1.5, 1e-6 ) &&
		       ApproxEq( normal->At( 0, 0 ).y, 0.5/1.5, 1e-6 ) &&
		       ApproxEq( normal->At( 0, 0 ).z, 1.0/1.5, 1e-6 ),
			"AOV bridge propagates a unit-length world normal" );
		Check( ApproxEq( depth->At( 0, 0 ), 7.5, 1e-6 ),
			"AOV bridge propagates camera-ray depth" );

		// A regional propagation must not replace previously valid AOV data
		// outside the crop with the sidecar's zero-initialized pixels.
		albedo->At( 1, 0 ) = RISEPel( 0.9, 0.8, 0.7 );
		normal->At( 1, 0 ) = Vector3( 0.4, 0.5, 0.6 );
		depth->At( 1, 0 ) = 42.0f;
		const Rect firstPixelOnly( 0, 0, 0, 0 );
		PropagateAOVsToFrameStore( store, full, &firstPixelOnly );
		Check( ApproxEq( albedo->At( 1, 0 ).r, 0.9, 1e-6 )
		    && ApproxEq( normal->At( 1, 0 ).z, 0.6, 1e-6 )
		    && ApproxEq( depth->At( 1, 0 ), 42.0, 1e-6 ),
			"regional AOV propagation preserves channels outside the crop" );

		AOVBuffers silhouette( 1, 1, AOVBuffers::Plan( false, false, true ) );
		silhouette.AccumulateDepth( 0, 0, 5.0, 1.0 );
		silhouette.AccumulateDepth( 0, 0, 0.0, 1.0 );
		silhouette.Normalize( 0, 0, 0.5 );
		Check( ApproxEq( silhouette.GetDepthPtr()[0], 5.0, 1e-6 ),
			"depth normalization excludes miss samples at silhouettes" );

		// High-frequency geometry (a groom, foliage) puts several
		// disagreeing surfaces inside one pixel.  The raw weighted mean
		// of their unit normals is SHORT -- here two normals 90 degrees
		// apart average to length 1/sqrt(2) -- which OIDN would read as
		// a differently-oriented surface rather than as disagreement.
		// Normalize() must hand back a unit vector along the mean
		// direction, and must be idempotent so a second Normalize()
		// (the bounded first-hit completion pass can issue one) cannot
		// shrink or grow it again.
		AOVBuffers crossing( 1, 1, AOVBuffers::Plan( false, true, false ) );
		crossing.AccumulateNormal( 0, 0, Vector3( 1, 0, 0 ), 1.0 );
		crossing.AccumulateNormal( 0, 0, Vector3( 0, 1, 0 ), 1.0 );
		crossing.Normalize( 0, 0, 0.5 );
		const float* cn = crossing.GetNormalPtr();
		const double kInvRoot2 = 1.0 / sqrt( 2.0 );
		Check( ApproxEq( cn[0], kInvRoot2, 1e-6 ) && ApproxEq( cn[1], kInvRoot2, 1e-6 )
		    && ApproxEq( cn[2], 0.0, 1e-6 ),
			"disagreeing samples average to a UNIT normal, not a short one" );
		crossing.Normalize( 0, 0, 0.25 );
		Check( ApproxEq( cn[0], kInvRoot2, 1e-6 ) && ApproxEq( cn[1], kInvRoot2, 1e-6 ),
			"normal renormalization is idempotent under a repeated Normalize" );

		// A pixel every sample missed keeps the zero vector rather than
		// having a direction invented for it.
		AOVBuffers allMiss( 1, 1, AOVBuffers::Plan( false, true, false ) );
		allMiss.Normalize( 0, 0, 1.0 );
		Check( allMiss.GetNormalPtr()[0] == 0.0f && allMiss.GetNormalPtr()[1] == 0.0f
		    && allMiss.GetNormalPtr()[2] == 0.0f,
			"an all-miss pixel keeps a zero normal instead of a fabricated one" );

		const AOVBuffers::Plan requested = MakeAOVPlan( store, false );
		Check( requested.albedo && requested.normal && requested.depth,
			"FrameStore requests become an exact full AOV plan" );
		const AOVBuffers::Plan denoiser = MakeAOVPlan( nullptr, true );
		Check( denoiser.albedo && denoiser.normal && !denoiser.depth,
			"denoiser-only plan preserves legacy albedo+normal allocation" );
		const AOVBuffers::Plan empty = MakeAOVPlan( nullptr, false );
		Check( !empty.Any(), "no consumer produces a zero-allocation AOV plan" );

		AOVBuffers oidnCache( w, h, AOVBuffers::Plan( true, true, true ) );
		oidnCache.ReleaseDepthStorage();
		Check( oidnCache.GetDepthPtr() == nullptr && !oidnCache.GetPlan().depth,
			"post-capture release removes the perception-only depth plane" );
		Check( oidnCache.StorageBytes() == pixels * 6u * sizeof(float) &&
		       oidnCache.ReservedBytes() == pixels * 6u * sizeof(float),
			"OIDN cache retains only its 24-byte/pixel albedo+normal pair" );
		oidnCache.Reset( w, h, AOVBuffers::Plan( false, false, false ) );
		Check( oidnCache.ReservedBytes() == 0,
			"disabling an AOV plane releases capacity instead of pinning prior render memory" );

		AOVBuffers resized( 64, 64, AOVBuffers::Plan( true, true, true ) );
		resized.Reset( 2, 1, AOVBuffers::Plan( true, true, true ) );
		Check( resized.ReservedBytes() == pixels * 8u * sizeof(float),
			"downsizing releases obsolete high-resolution AOV capacity" );

		store->release();
	}

	// ─── Section 3: tile locks + observer firing ────────────────
	struct CountingObserver : public IRenderObserver
	{
		std::atomic<int> tileCount{ 0 };
		std::atomic<int> frameCount{ 0 };
		std::atomic<uint64_t> lastGen{ 0 };

		void OnTileComplete( const Rect& /*roi*/, uint64_t generation ) override
		{
			++tileCount;
			lastGen.store( generation, std::memory_order_release );
		}
		void OnFrameComplete( unsigned /*frame*/, uint64_t generation ) override
		{
			++frameCount;
			lastGen.store( generation, std::memory_order_release );
		}
	};

	void TestTileLockingAndObserver()
	{
		FrameStore* store = MakeStore( 32, 16, 8 );

		CountingObserver obs;
		store->AddObserver( &obs );

		// Idempotent attach.
		store->AddObserver( &obs );
		Check( true, "AddObserver idempotent (no crash on duplicate)" );

		const uint64_t gen0 = store->Generation();

		// Single tile commit.
		store->BeginTile( 0, 0 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		beauty->At( 1, 1 ) = RISEPel( 0.5, 0.25, 0.125 );
		store->EndTile( 0, 0 );

		Check( store->Generation() > gen0, "Generation advances on EndTile" );
		Check( obs.tileCount.load() == 1, "Observer fired once on EndTile" );
		Check( obs.lastGen.load() == store->Generation(),
			"Observer received current generation" );

		// Frame complete.
		store->MarkFrameComplete( 0 );
		Check( obs.frameCount.load() == 1, "Observer fired on MarkFrameComplete" );

		// Detach + commit again → no further fires.
		store->RemoveObserver( &obs );
		store->BeginTile( 1, 0 );
		store->EndTile( 1, 0 );
		Check( obs.tileCount.load() == 1, "Observer detached: no further tile fires" );

		// Detach unknown observer is silent.
		CountingObserver other;
		store->RemoveObserver( &other );
		Check( true, "RemoveObserver(unattached) silent" );

		store->release();
	}

	// ─── Section 4: concurrent reader / writer (tile-lock stress) ───
	void TestConcurrentTileLocking()
	{
		FrameStore* store = MakeStore( 32, 32, 8 );

		std::atomic<bool> stop{ false };
		std::atomic<int>  tornReads{ 0 };
		std::atomic<int>  iterations{ 0 };

		// Writer thread: bumps tile (0,0) + fills with a known pattern,
		// then commits.  Pattern: each pixel encodes the writer's
		// epoch counter as a uniform color.  Readers must see one
		// consistent epoch per tile read.
		//
		// IMPORTANT: writerGen sequences through values whose linear
		// magnitudes differ enough that quantisation through the encode
		// pipeline produces distinct R bytes per epoch.  If two
		// adjacent epochs encoded to the same byte, the torn-read
		// detector would be blind to a tear at the boundary.  See L1
		// adversarial review MED-7.  We use values 0.1, 0.2, 0.3, ...
		// (cycling), each separated by 25.5 quantised units in sRGB —
		// well above any rounding ambiguity.
		std::thread writer( [&]() {
			auto* beauty = store->GetChannel<ChannelId::Beauty>();
			int epoch = 0;
			while ( !stop.load() ) {
				epoch = ( epoch + 1 ) % 9;  // 1..9
				const double v = 0.1 * static_cast<double>( epoch );
				store->BeginTile( 0, 0 );
				for ( unsigned y = 0; y < 8; ++y ) {
					for ( unsigned x = 0; x < 8; ++x ) {
						beauty->At( x, y ) = RISEPel( v, v, v );
					}
				}
				store->EndTile( 0, 0 );
			}
		} );

		// Reader function: Render() into an RGBA8 buffer for the
		// (0,0) tile and verify all 64 pixels are byte-identical
		// across all 4 channels (R, G, B, A).  Comparing all 4
		// bytes (not just R) closes the false-negative window
		// where two epochs happened to round to the same R byte.
		auto readerBody = [&]() {
			std::vector<uint8_t> buf( 8 * 8 * 4, 0 );
			ViewTransform xf = ViewTransform::Identity();
			while ( !stop.load() ) {
				const Rect roi( 0, 0, 8, 8 );
				store->Render( buf.data(), 8 * 4, roi,
				               TargetFormat::RGBA8_sRGB, xf );

				// Compare the full 4-byte pattern of pixel 0 against
				// every other pixel.  All 64 must match — otherwise
				// the read tore between two writer epochs.
				const uint32_t pat0 =
					( static_cast<uint32_t>( buf[0] )       )
					| ( static_cast<uint32_t>( buf[1] ) << 8 )
					| ( static_cast<uint32_t>( buf[2] ) << 16 )
					| ( static_cast<uint32_t>( buf[3] ) << 24 );
				bool uniform = true;
				for ( int i = 1; i < 64; ++i ) {
					const uint32_t pat =
						( static_cast<uint32_t>( buf[ i * 4 + 0 ] )       )
						| ( static_cast<uint32_t>( buf[ i * 4 + 1 ] ) << 8 )
						| ( static_cast<uint32_t>( buf[ i * 4 + 2 ] ) << 16 )
						| ( static_cast<uint32_t>( buf[ i * 4 + 3 ] ) << 24 );
					if ( pat != pat0 ) { uniform = false; break; }
				}
				if ( !uniform ) tornReads.fetch_add( 1 );
				iterations.fetch_add( 1 );
			}
		};

		// Two reader threads to amplify contention vs the single
		// writer.  Run for 500 ms — long enough on Apple Silicon
		// (where reorderings are rare events) to accumulate
		// thousands of cross-thread interleavings.  This is still
		// far short of what TSan would catch immediately, but it's
		// the best stress signal we can get without TSan integration.
		std::thread r1( readerBody );
		std::thread r2( readerBody );

		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		stop.store( true );
		writer.join();
		r1.join();
		r2.join();

		std::ostringstream os;
		os << "concurrent readers (2) saw " << iterations.load()
		   << " tile reads, " << tornReads.load() << " torn";
		Check( tornReads.load() == 0, os.str() );
		Check( iterations.load() > 100, "reader threads ran enough iterations to stress" );

		store->release();
	}

	// ─── Section 4b: observer self-detach safety ──────────────────
	// Per L1 adversarial review HIGH-4: an observer that calls
	// RemoveObserver(this) inside its own callback must not deadlock
	// on the observer mutex.  The DispatchObservers snapshot pattern
	// guarantees this.
	struct SelfDetachingObserver : public IRenderObserver
	{
		FrameStore* store = nullptr;
		std::atomic<int> fires{ 0 };

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
			// Detach self while we're being dispatched — would
			// deadlock under naive "lock then iterate" pattern.
			if ( store ) store->RemoveObserver( this );
		}
	};

	void TestObserverSelfDetach()
	{
		FrameStore* store = MakeStore( 16, 16, 8 );
		SelfDetachingObserver obs;
		obs.store = store;
		store->AddObserver( &obs );

		// First commit fires + self-detaches; second commit should
		// see no fire (observer already removed).
		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );
		Check( obs.fires.load() == 1, "self-detaching observer fires once" );

		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );
		Check( obs.fires.load() == 1,
			"self-detaching observer does not fire after self-detach" );

		store->release();
	}

	// ─── Section 4c: cross-thread RemoveObserver waits for in-flight ─
	// Per L1 adversarial review P2: RemoveObserver must wait for
	// any in-flight dispatch whose snapshot may hold the observer
	// pointer to complete.  Otherwise a caller that calls
	// RemoveObserver-then-destroys the observer races a dispatcher
	// that's part-way through invoking observer.OnXxx().
	//
	// Test pattern:
	//   - Slow observer's callback sleeps for ~5ms.
	//   - Writer thread commits tiles in a loop, firing the
	//     dispatch.
	//   - Main thread sleeps briefly, then RemoveObserver +
	//     destroys the observer.  RemoveObserver MUST NOT return
	//     until the in-flight callback finishes — otherwise
	//     destroying the observer would UAF the in-flight callback.
	struct SlowObserver : public IRenderObserver
	{
		std::atomic<int> active{ 0 };  // mirrors "callback in flight"
		std::atomic<int> fires{ 0 };

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			active.fetch_add( 1 );
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			active.fetch_sub( 1 );
			fires.fetch_add( 1 );
		}
	};

	// ─── Section 4d: same-thread cascade removal must not UAF ─────
	// Per L1 adversarial review round 3 P2: observer A's callback
	// removing-and-destroying observer B (where B appears LATER in
	// the same dispatch snapshot) must not lead the dispatcher to
	// dereference B's freed pointer in the next iteration.  The
	// fix is a per-iteration recheck against observers_ before
	// invoking each snapshot entry.
	struct CascadeCounter : public IRenderObserver
	{
		std::atomic<int> fires{ 0 };
		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
		}
	};

	struct CascadeRemover : public IRenderObserver
	{
		FrameStore*      store      = nullptr;
		IRenderObserver* victim     = nullptr;
		std::atomic<int> fires{ 0 };
		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
			// Remove + destroy the victim DURING dispatch.  Without
			// the per-iteration recheck, the dispatch loop would
			// later dereference victim's freed pointer.
			if ( store && victim ) {
				store->RemoveObserver( victim );
				delete victim;
				victim = nullptr;
			}
		}
	};

	void TestObserverCascadeRemovalNoUAF()
	{
		FrameStore* store = MakeStore( 8, 8, 8 );

		// Heap-allocate the cascade so the remover can `delete` the
		// victim — replicating realistic same-thread destruction.
		auto* a = new CascadeRemover();
		auto* b = new CascadeCounter();   // victim
		auto* c = new CascadeCounter();

		a->store  = store;
		a->victim = b;

		// AddObserver order matters: A first, then B (which A will
		// kill mid-dispatch), then C (which must still fire).
		store->AddObserver( a );
		store->AddObserver( b );
		store->AddObserver( c );

		// Single tile commit triggers DispatchObservers with
		// snapshot = [A, B, C].  A's callback removes-and-destroys
		// B; the next iteration must skip B (now freed) and
		// proceed to C.
		store->BeginTile( 0, 0 );
		store->EndTile( 0, 0 );

		Check( a->fires.load() == 1, "A fired once" );
		Check( c->fires.load() == 1, "C still fires after B removed mid-dispatch" );
		// Reaching this assertion at all means no UAF on B's freed
		// pointer during the dispatch loop.
		Check( true, "no UAF on freed observer B" );

		store->RemoveObserver( a );
		store->RemoveObserver( c );
		delete a;
		delete c;

		store->release();
	}

	void TestObserverRemoveWaitsForInFlight()
	{
		FrameStore* store = MakeStore( 16, 16, 8 );
		SlowObserver obs;
		store->AddObserver( &obs );

		std::atomic<bool> stopWriter{ false };
		std::thread writer( [&]() {
			while ( !stopWriter.load() ) {
				store->BeginTile( 0, 0 );
				store->EndTile( 0, 0 );
			}
		} );

		// Wait for the slow observer to start at least once.
		while ( obs.active.load() == 0 ) {
			std::this_thread::yield();
		}

		// Now RemoveObserver while the slow observer is mid-callback.
		// RemoveObserver must wait until obs.active goes to 0
		// before returning, OR the in-flight callback finishes
		// strictly before RemoveObserver returns.
		store->RemoveObserver( &obs );
		const int activeAfterRemove = obs.active.load();

		// Critical assertion: when RemoveObserver returns, no
		// callback can be in flight on this observer.  Otherwise
		// the caller would be free to destroy it and create a UAF.
		Check( activeAfterRemove == 0,
			"RemoveObserver waits for in-flight callback (no UAF window)" );

		stopWriter.store( true );
		writer.join();

		store->release();
	}

	struct CrossThreadVictim : public IRenderObserver
	{
		std::mutex mutex;
		std::condition_variable condition;
		bool entered = false;
		bool continueCallback = false;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			condition.notify_all();
			condition.wait(lock,[this]() { return continueCallback; });
		}
	};

	struct CrossThreadRemover : public IRenderObserver
	{
		FrameStore* store = nullptr;
		IRenderObserver* victim = nullptr;
		std::mutex mutex;
		std::condition_variable condition;
		bool started = false;
		bool returned = false;

		void OnFrameComplete( unsigned int, uint64_t ) override
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				started = true;
				condition.notify_all();
			}
			store->RemoveObserver(victim);
			{
				std::lock_guard<std::mutex> lock(mutex);
				returned = true;
				condition.notify_all();
			}
		}
	};

	struct ObserverDispatchContentionWitness
	{
		static void Notify( void* context )
		{
			auto* witness = static_cast<ObserverDispatchContentionWitness*>(context);
			{
				std::lock_guard<std::mutex> lock(witness->mutex);
				witness->observed = true;
			}
			witness->condition.notify_all();
		}

		bool Wait( const std::chrono::milliseconds timeout )
		{
			std::unique_lock<std::mutex> lock(mutex);
			return condition.wait_for(lock,timeout,[this]() { return observed; });
		}

		std::mutex mutex;
		std::condition_variable condition;
		bool observed = false;
	};

	void TestObserverRemovalFromUnrelatedCallbackWaitsForVictim()
	{
		FrameStore* store = MakeStore(8,8,8);
		CrossThreadRemover remover;
		CrossThreadVictim victim;
		remover.store = store;
		remover.victim = &victim;
		store->AddObserver(&remover);
		store->AddObserver(&victim);

		std::thread tileDispatch([&]() {
			store->BeginTile(0,0);
			store->EndTile(0,0);
		});
		{
			std::unique_lock<std::mutex> lock(victim.mutex);
			victim.condition.wait(lock,[&]() { return victim.entered; });
		}
		ObserverDispatchContentionWitness contention;
		RISE::Implementation::SetFrameStoreObserverDispatchContentionHookForTests(
			&ObserverDispatchContentionWitness::Notify,&contention);
		std::thread frameDispatch([&]() { store->MarkFrameComplete(1u); });
		const bool frameDispatchContended =
			contention.Wait(std::chrono::milliseconds(2000));
		RISE::Implementation::SetFrameStoreObserverDispatchContentionHookForTests(
			nullptr,nullptr);
		bool removeStarted = false;
		bool removeReturnedEarly = false;
		{
			std::lock_guard<std::mutex> lock(remover.mutex);
			removeStarted = remover.started;
			removeReturnedEarly = remover.returned;
		}
		{
			std::lock_guard<std::mutex> lock(victim.mutex);
			victim.continueCallback = true;
			victim.condition.notify_all();
		}
		tileDispatch.join();
		frameDispatch.join();
		Check(frameDispatchContended && !removeStarted &&
			!removeReturnedEarly && remover.returned,
			"observer callbacks serialize before unrelated removal can overlap a victim" );

		store->RemoveObserver(&remover);
		store->release();
	}

	struct ConcurrentSelfRemovingObserver : public IRenderObserver
	{
		FrameStore* store = nullptr;
		std::atomic<unsigned int> fires{0u};

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			fires.fetch_add(1u);
			store->RemoveObserver(this);
		}
	};

	void TestConcurrentSelfRemovalDrainsOtherThreads()
	{
		FrameStore* store = MakeStore(8,8,8);
		ConcurrentSelfRemovingObserver observer;
		observer.store = store;
		store->AddObserver(&observer);
		std::mutex startMutex;
		std::condition_variable startCondition;
		unsigned int ready = 0u;
		bool start = false;
		auto dispatch = [&]() {
			{
				std::unique_lock<std::mutex> lock(startMutex);
				++ready;
				startCondition.notify_all();
				startCondition.wait(lock,[&]() { return start; });
			}
			store->BeginTile(0,0);
			store->EndTile(0,0);
		};
		std::thread first(dispatch);
		std::thread second(dispatch);
		{
			std::unique_lock<std::mutex> lock(startMutex);
			startCondition.wait(lock,[&]() { return ready == 2u; });
			start = true;
			startCondition.notify_all();
		}
		first.join();
		second.join();
		Check(observer.fires.load() == 1u,
			"concurrent dispatches serialize so a self-removing observer fires once" );
		store->release();
	}

	struct SharedCrossStoreObserver : public IRenderObserver
	{
		FrameStore* removeFrom = nullptr;
		std::mutex mutex;
		std::condition_variable condition;
		unsigned int fires = 0u;
		bool firstEntered = false;
		bool continueFirst = false;
		bool secondReturned = false;
		bool removalRejected = false;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			std::unique_lock<std::mutex> lock(mutex);
			++fires;
			if( fires == 1u ) {
				firstEntered = true;
				condition.notify_all();
				condition.wait(lock,[this]() { return continueFirst; });
				return;
			}
			lock.unlock();
			try {
				removeFrom->RemoveObserver(this);
			} catch( const std::runtime_error& ) {
				removalRejected = true;
			}
			lock.lock();
			secondReturned = true;
			condition.notify_all();
		}
	};

	void TestSharedObserverCrossStoreRemovalIsSerialized()
	{
		FrameStore* firstStore = MakeStore(8,8,8);
		FrameStore* secondStore = MakeStore(8,8,8);
		SharedCrossStoreObserver observer;
		observer.removeFrom = firstStore;
		firstStore->AddObserver(&observer);
		secondStore->AddObserver(&observer);

		std::thread firstDispatch([&]() {
			firstStore->BeginTile(0,0);
			firstStore->EndTile(0,0);
		});
		{
			std::unique_lock<std::mutex> lock(observer.mutex);
			observer.condition.wait(lock,[&]() { return observer.firstEntered; });
		}
		std::thread secondDispatch([&]() {
			secondStore->BeginTile(0,0);
			secondStore->EndTile(0,0);
		});
		bool overlapped = false;
		{
			std::unique_lock<std::mutex> lock(observer.mutex);
			overlapped = observer.condition.wait_for(lock,std::chrono::milliseconds(2000),
				[&]() { return observer.fires > 1u || observer.secondReturned; });
			observer.continueFirst = true;
			observer.condition.notify_all();
		}
		firstDispatch.join();
		secondDispatch.join();
		Check(overlapped && observer.fires == 2u && observer.secondReturned &&
			observer.removalRejected,
			"callback removal fails closed instead of waiting on another store callback" );

		firstStore->RemoveObserver(&observer);
		secondStore->RemoveObserver(&observer);
		firstStore->release();
		secondStore->release();
	}

	struct MutualRemovingObserver : public IRenderObserver
	{
		FrameStore* victimStore = nullptr;
		IRenderObserver* victim = nullptr;
		std::atomic<unsigned int> fires{0u};
		std::atomic<unsigned int> rejections{0u};
		std::mutex* startMutex = nullptr;
		std::condition_variable* startCondition = nullptr;
		unsigned int* entered = nullptr;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			fires.fetch_add(1u);
			{
				std::unique_lock<std::mutex> lock(*startMutex);
				++*entered;
				startCondition->notify_all();
				startCondition->wait_for(lock,std::chrono::milliseconds(2000),
					[this]() { return *entered == 2u; });
			}
			try { victimStore->RemoveObserver(victim); }
			catch( const std::runtime_error& ) { rejections.fetch_add(1u); }
		}
	};

	void TestCrossStoreMutualRemovalCannotDeadlock()
	{
		FrameStore* firstStore = MakeStore(8,8,8);
		FrameStore* secondStore = MakeStore(8,8,8);
		MutualRemovingObserver firstObserver;
		MutualRemovingObserver secondObserver;
		firstObserver.victimStore = secondStore;
		firstObserver.victim = &secondObserver;
		secondObserver.victimStore = firstStore;
		secondObserver.victim = &firstObserver;
		std::mutex callbackMutex;
		std::condition_variable callbackCondition;
		unsigned int entered = 0u;
		firstObserver.startMutex = secondObserver.startMutex = &callbackMutex;
		firstObserver.startCondition = secondObserver.startCondition = &callbackCondition;
		firstObserver.entered = secondObserver.entered = &entered;
		firstStore->AddObserver(&firstObserver);
		secondStore->AddObserver(&secondObserver);

		std::mutex startMutex;
		std::condition_variable startCondition;
		unsigned int ready = 0u;
		bool start = false;
		auto dispatch = [&]( FrameStore* store ) {
			{
				std::unique_lock<std::mutex> lock(startMutex);
				++ready;
				startCondition.notify_all();
				startCondition.wait(lock,[&]() { return start; });
			}
			store->BeginTile(0,0);
			store->EndTile(0,0);
		};
		std::thread firstDispatch(dispatch,firstStore);
		std::thread secondDispatch(dispatch,secondStore);
		{
			std::unique_lock<std::mutex> lock(startMutex);
			startCondition.wait(lock,[&]() { return ready == 2u; });
			start = true;
			startCondition.notify_all();
		}
		firstDispatch.join();
		secondDispatch.join();
		Check(firstObserver.fires.load()+secondObserver.fires.load() == 2u &&
			firstObserver.rejections.load()+secondObserver.rejections.load() == 2u,
			"cross-store mutual removal fails closed instead of forming a wait cycle" );

		firstStore->RemoveObserver(&firstObserver);
		secondStore->RemoveObserver(&secondObserver);
		firstStore->release();
		secondStore->release();
	}

	struct ReentrantDispatchObserver : public IRenderObserver
	{
		FrameStore* store = nullptr;
		bool rejected = false;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			try { store->MarkFrameComplete(1u); }
			catch( const std::runtime_error& error ) {
				rejected = std::string(error.what()).find("reentrant") != std::string::npos;
			}
		}
	};

	void TestReentrantObserverDispatchFailsClosed()
	{
		FrameStore* store = MakeStore(8,8,8);
		ReentrantDispatchObserver observer;
		observer.store = store;
		store->AddObserver(&observer);
		store->BeginTile(0,0);
		store->EndTile(0,0);
		Check(observer.rejected && store->Generation() == 1u && store->Meta().frame == 0u,
			"observer callback reentrant publication preserves frame metadata and generation" );
		store->RemoveObserver(&observer);
		store->release();
	}

	struct CrossThreadPublishingObserver : public IRenderObserver
	{
		FrameStore* other = nullptr;
		std::mutex mutex;
		std::condition_variable condition;
		bool startWorker = false;
		bool workerDone = false;
		bool completedWhileCallbackActive = false;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			std::unique_lock<std::mutex> lock(mutex);
			startWorker = true;
			condition.notify_all();
			completedWhileCallbackActive = condition.wait_for(
				lock,std::chrono::milliseconds(2000),[this]() { return workerDone; });
		}
	};

	void TestCrossThreadCrossStorePublicationDoesNotDeadlock()
	{
		FrameStore* first = MakeStore(8,8,8);
		FrameStore* second = MakeStore(8,8,8);
		CrossThreadPublishingObserver observer;
		observer.other = second;
		first->AddObserver(&observer);
		std::thread worker([&]() {
			{
				std::unique_lock<std::mutex> lock(observer.mutex);
				observer.condition.wait(lock,[&]() { return observer.startWorker; });
			}
			second->MarkFrameComplete(3u);
			{
				std::lock_guard<std::mutex> lock(observer.mutex);
				observer.workerDone = true;
			}
			observer.condition.notify_all();
		});
		first->BeginTile(0,0);
		first->EndTile(0,0);
		worker.join();
		Check(observer.completedWhileCallbackActive && second->Meta().frame == 3u,
			"cross-store publication on another thread is not globally serialized" );
		first->RemoveObserver(&observer);
		first->release();
		second->release();
	}

	struct BulkReadObserver : public IRenderObserver
	{
		FrameStore* store = nullptr;
		std::mutex mutex;
		std::condition_variable condition;
		std::thread reader;
		bool started = false;
		bool done = false;
		bool completedBeforeCallbackReturned = false;

		void OnTileComplete( const Rect&, uint64_t ) override
		{
			if( started ) return;
			started = true;
			reader = std::thread([this]() {
				std::vector<unsigned char> pixels(store->Width()*store->Height()*4u);
				store->Render(pixels.data(),store->Width()*4u,
					Rect(0,0,static_cast<unsigned int>(store->Height()),
						static_cast<unsigned int>(store->Width())),
					TargetFormat::RGBA8_sRGB,ViewTransform::Identity());
				{
					std::lock_guard<std::mutex> lock(mutex);
					done = true;
				}
				condition.notify_all();
			});
			std::unique_lock<std::mutex> lock(mutex);
			completedBeforeCallbackReturned = condition.wait_for(
				lock,std::chrono::milliseconds(2000),[this]() { return done; });
		}
	};

	struct CountingThrowingTileObserver : public IRenderObserver
	{
		unsigned int fires = 0u;
		void OnTileComplete( const Rect&, uint64_t ) override
		{
			++fires;
			throw std::runtime_error("injected bulk observer failure");
		}
	};

	void TestBulkBracketReleasesAllTilesBeforeNotification()
	{
		FrameStore* store = MakeStore(16,8,8);
		BulkReadObserver reader;
		CountingThrowingTileObserver thrower;
		reader.store = store;
		store->AddObserver(&reader);
		store->AddObserver(&thrower);
		{
			FrameStoreBulkBracket bracket(store,store->AsBeautyRasterImage());
		}
		if( reader.reader.joinable() ) reader.reader.join();
		Check(reader.completedBeforeCallbackReturned && thrower.fires == 2u &&
			store->Generation() == 2u,
			"bulk bracket unlocks every tile before callbacks and contains observer failures" );
		store->RemoveObserver(&reader);
		store->RemoveObserver(&thrower);
		store->release();
	}

	// ─── Section 5: Render readback identity & exposure ───────────
	void TestConcurrentFrameMetadata()
	{
		FrameStore* store = MakeStore(2,2,2);
		const unsigned int iterations = 20000;
		std::atomic<bool> start{false};
		std::atomic<bool> writerDone{false};
		std::atomic<bool> valid{true};
		std::atomic<unsigned int> checkpoint{0u};
		std::atomic<unsigned int> acknowledged{0u};
		std::atomic<unsigned int> readsWhileWriterActive{0u};
		std::thread writer([&]() {
			while( !start.load(std::memory_order_acquire) ) {}
			for( unsigned int frame=1; frame<=iterations; ++frame ) {
				if( frame%2u ) {
					store->MarkFrameComplete(frame);
				} else {
					store->MarkDenoiseComplete(frame);
				}
				if( frame%100u == 0u && frame != iterations ) {
					checkpoint.store(frame,std::memory_order_release);
					while( acknowledged.load(std::memory_order_acquire) < frame ) {}
				}
			}
			writerDone.store(true,std::memory_order_release);
		});
		std::thread reader([&]() {
			start.store(true,std::memory_order_release);
			while( !writerDone.load(std::memory_order_acquire) ) {
				const Metadata metadata = store->Meta();
				const bool expectedDenoised = metadata.frame != 0u &&
					metadata.frame%2u == 0u;
				if( metadata.frame > iterations ||
					metadata.denoisedContent != expectedDenoised ) {
					valid.store(false);
				}
				readsWhileWriterActive.fetch_add(1u,std::memory_order_relaxed);
				const unsigned int requested =
					checkpoint.load(std::memory_order_acquire);
				if( requested && metadata.frame >= requested ) {
					acknowledged.store(requested,std::memory_order_release);
				}
			}
		});
		writer.join();
		reader.join();
		const Metadata finalMetadata = store->Meta();
		Check(valid.load() &&
			readsWhileWriterActive.load() >= (iterations/100u)-1u &&
			finalMetadata.frame == iterations && finalMetadata.denoisedContent,
			"Mark*Complete and Meta publish coherent frame/denoise pairs during proven overlap");
		store->release();
	}

	void TestRenderReadback()
	{
		FrameStore* store = MakeStore( 4, 4, 4 );

		// Fill beauty with a known pattern: pixel (x, y) = (x*0.1, y*0.1, 0).
		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		store->BeginTile( 0, 0 );
		for ( unsigned y = 0; y < 4; ++y ) {
			for ( unsigned x = 0; x < 4; ++x ) {
				beauty->At( x, y ) = RISEPel( x * 0.1, y * 0.1, 0.0 );
			}
		}
		store->EndTile( 0, 0 );

		// Render with identity transform into RGBA8_sRGB.
		std::vector<uint8_t> buf( 4 * 4 * 4, 0 );
		ViewTransform xf = ViewTransform::Identity();
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xf );

		// Pixel (0, 0) = ROMM(0, 0, 0) → encoded sRGB(0, 0, 0).
		Check( buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 255,
			"render (0,0) = #00000000FF (black, opaque)" );

		// Pixel (3, 3) = ROMM(0.3, 0.3, 0) → after ROMM→sRGB linear ≈
		// (0.36, 0.31, 0.28).  Then sRGB transfer ≈ (0.65, 0.59, 0.57).
		// Quantised: ~166, 151, 145.  Just check it's nonzero and
		// reasonable; precise values depend on the published ROMM
		// chromatic adaptation.  IMPORTANT: snapshot the BYTE value
		// (not the pointer) — subsequent Render() calls into the
		// same buffer overwrite the pointed-to bytes.
		const uint8_t identityR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		const uint8_t identityA = buf[ ( 3 * 4 + 3 ) * 4 + 3 ];
		Check( identityR > 100 && identityR < 200,
			"render (3,3) R in expected range" );
		Check( identityA == 255, "render (3,3) alpha opaque" );

		// Exposure +1 EV doubles linear, then re-encodes through sRGB.
		// On (3, 3) brighter: R should be larger than identity.
		ViewTransform xfBright = ViewTransform::ForLDRDisplay( 1.0f, eDisplayTransform_None );
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xfBright );
		const uint8_t brightR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		Check( brightR > identityR,
			"exposure +1 EV brightens R" );

		// Exposure -1 EV halves; should darken.
		ViewTransform xfDark = ViewTransform::ForLDRDisplay( -1.0f, eDisplayTransform_None );
		store->Render( buf.data(), 4 * 4, Rect( 0, 0, 4, 4 ),
		               TargetFormat::RGBA8_sRGB, xfDark );
		const uint8_t darkR = buf[ ( 3 * 4 + 3 ) * 4 + 0 ];
		Check( darkR < identityR,
			"exposure -1 EV darkens R" );

		store->release();
	}

	// ─── Section 6: tone curve gated by isLDRFixed ────────────────
	void TestToneCurveGating()
	{
		FrameStore* store = MakeStore( 1, 1, 8 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();

		// Use input 2.0 (1 stop over white).  ACES at this level
		// compresses meaningfully below 1.0; ACES at 10.0 saturates
		// to its natural ceiling of ~1.0, defeating the gating
		// observation since with-tone-curve and without-tone-curve
		// both round-trip to ~255.
		store->BeginTile( 0, 0 );
		beauty->At( 0, 0 ) = RISEPel( 2.0, 2.0, 2.0 );
		store->EndTile( 0, 0 );

		// LDR target WITHOUT tone curve: linear 2.0 is above sRGB
		// max so the transfer function (followed by quantisation)
		// produces saturated white (255).
		ViewTransform xfNoTone = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_None );
		uint8_t bufNoTone[4] = { 0 };
		store->Render( bufNoTone, 4, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA8_sRGB, xfNoTone );
		Check( bufNoTone[0] == 255,
			"LDR target, no tone curve: 2.0 → saturated 255" );

		// LDR target WITH ACES: ACES(2.224) ≈ 0.93, then sRGB
		// transfer ≈ 0.97, then quant → ~247.  Materially below 255.
		ViewTransform xfACES = ViewTransform::ForLDRDisplay( 0.0f, eDisplayTransform_ACES );
		uint8_t bufACES[4] = { 0 };
		store->Render( bufACES, 4, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA8_sRGB, xfACES );
		Check( bufACES[0] < 255 && bufACES[0] > 200,
			"LDR target, ACES tone curve: compresses 2.0 below saturated white" );
		Check( bufACES[0] < bufNoTone[0],
			"ACES output strictly less than no-tone-curve at same input" );

		// HDR float target (RGBA32F_Linear) with the SAME ACES
		// ViewTransform: tone curve is GATED OFF by
		// TargetFormatInfo.isLDRFixed = false, so the raw linear
		// value passes through (~2.0 after primaries conversion;
		// matrix coefficients on white are slightly above unity so
		// > 1 expected).
		float buf32[4] = { 0 };
		store->Render( buf32, 16, Rect( 0, 0, 1, 1 ),
		               TargetFormat::RGBA32F_Linear, xfACES );
		Check( buf32[0] > 1.5f,
			"HDR float target preserves > 1.0 even when tone curve is set in xform" );

		store->release();
	}

	// ─── Section 7: reference lifetime ────────────────────────────
	void TestReferenceLifetime()
	{
		// Per RISE convention (Reference.cpp:23) the constructor
		// initialises m_nRefcount to 1, so a freshly-allocated
		// Reference-derived object owns the first reference.
		FrameStore::Spec spec;
		spec.width = 4;
		spec.height = 4;
		FrameStore* store = new FrameStore( spec );

		Check( store->refcount() == 1, "fresh FrameStore has refcount 1 (RISE convention)" );

		store->addref();
		Check( store->refcount() == 2, "addref → refcount 2" );

		store->addref();
		Check( store->refcount() == 3, "addref → refcount 3" );

		// IMPORTANT: Reference::release() (Reference.cpp:63) returns
		// `!bDestroy` — i.e. TRUE when the object is STILL ALIVE
		// after the release, FALSE when the release destroyed it.
		// The IReference comment claims the opposite ("TRUE if the
		// object was deleted") but the implementation contradicts it
		// and `safe_release` ignores the return anyway, so callers
		// in the wild aren't affected.  This test exercises the
		// actual implementation behaviour.
		bool released = store->release();
		Check( released && store->refcount() == 2,
			"release at refcount 3 returns true (still alive), decrements to 2" );

		released = store->release();
		Check( released && store->refcount() == 1,
			"release at refcount 2 returns true (still alive), decrements to 1" );

		released = store->release();
		Check( !released, "release at refcount 1 returns false (destroyed)" );
		// store is now freed; do not access.
	}

	// ─── Section 8: AsBeautyRasterImage shim ──────────────────────
	void TestBeautyRasterImageShim()
	{
		FrameStore* store = MakeStore( 8, 4, 4 );

		// Get the IRasterImage view.
		IRasterImage& img = store->AsBeautyRasterImage();
		Check( img.GetWidth()  == 8, "shim width" );
		Check( img.GetHeight() == 4, "shim height" );

		// SetPEL through shim writes into Beauty + Alpha channels.
		RISEColor c( RISEPel( 0.5, 0.25, 0.125 ), 0.7 );
		img.SetPEL( 3, 2, c );

		auto* beauty = store->GetChannel<ChannelId::Beauty>();
		auto* alpha  = store->GetChannel<ChannelId::Alpha>();
		Check( beauty->At( 3, 2 ).r == 0.5
		    && beauty->At( 3, 2 ).g == 0.25
		    && beauty->At( 3, 2 ).b == 0.125,
			"shim SetPEL writes Beauty" );
		Check( ApproxEq( alpha->At( 3, 2 ), 0.7f, 1e-6f ),
			"shim SetPEL writes Alpha" );

		// GetPEL returns what was written.
		RISEColor back = img.GetPEL( 3, 2 );
		Check( back.base.r == 0.5 && back.base.g == 0.25
		    && ApproxEq( back.a, 0.7, 1e-6 ),
			"shim GetPEL round-trips" );

		// Clear with a specific color writes the rectangle.
		RISEColor white( RISEPel( 1.0, 1.0, 1.0 ), 1.0 );
		Rect rc( 0, 0, 2, 2 );
		img.Clear( white, &rc );
		Check( beauty->At( 0, 0 ).r == 1.0 && beauty->At( 1, 1 ).g == 1.0,
			"shim Clear with rect" );
		// Outside the rect: untouched (still default 0).  Use (5,3) to be safe.
		Check( beauty->At( 5, 3 ).r == 0.0,
			"shim Clear leaves outside-rect untouched" );

		// An inclusive UINT_MAX endpoint means "to the edge" (as for
		// RISERasterImage::Clear); a 32-bit `right + 1` wrapped it to 0
		// and cleared nothing.
		{
			const unsigned int kMax = std::numeric_limits<unsigned int>::max();
			RISEColor grey( RISEPel( 0.25, 0.25, 0.25 ), 0.5 );
			Rect toEdge( 2, 5, kMax, kMax );	// rows 2..3, columns 5..7
			img.Clear( grey, &toEdge );
			unsigned bad = 0;
			for( unsigned y = 0; y < 4; ++y )
				for( unsigned x = 0; x < 8; ++x ) {
					const bool in = x >= 5 && y >= 2;
					const double r = beauty->At( x, y ).r;
					if( in ? ( r != 0.25 || !ApproxEq( alpha->At( x, y ), 0.5f, 1e-6f ) ) : r == 0.25 ) ++bad;
				}
			Check( bad == 0, "shim Clear with a UINT_MAX endpoint clears exactly to the edge (" +
				std::to_string( bad ) + " wrong)" );
			Rect outside( 4, 8, kMax, kMax );
			img.Clear( white, &outside );
			Check( beauty->At( 7, 3 ).r == 0.25, "shim Clear with a wholly outside UINT_MAX region writes nothing" );
		}

		store->release();
	}

	// ─── Section 8b: HDR archival float targets ───────────────────
	// Per L1 adversarial review P3:
	//   - RGBA32F_Linear must apply NO Sanitise step in its transfer
	//     function — negatives, NaN, and Inf must pass through so
	//     reconstruction-filter ringing and out-of-gamut samples are
	//     preserved in archival output.
	//   - RGBA32F_ROMM_Linear paired with ViewTransform::Identity()
	//     must produce a buffer that's bit-identical to the FrameStore's
	//     beauty channel (modulo float-vs-double precision) — i.e.,
	//     no primaries conversion, no transfer, no clamping.
	void TestHDRArchivalIdentity()
	{
		FrameStore* store = MakeStore( 4, 1, 8 );
		auto* beauty = store->GetChannel<ChannelId::Beauty>();

		// Set 4 pixels covering the full range of pathological float
		// inputs an archival path must preserve:
		//   pixel 0: ordinary positive HDR value
		//   pixel 1: negative R (filter ringing)
		//   pixel 2: NaN G (poison-input chaos test)
		//   pixel 3: +Inf B (saturated highlight)
		const uint64_t kPosInfBits = 0x7FF0000000000000ULL;
		const uint64_t kQNaNBits   = 0x7FF8000000000000ULL;
		double posInf, nanD;
		std::memcpy( &posInf, &kPosInfBits, sizeof(posInf) );
		std::memcpy( &nanD,   &kQNaNBits,   sizeof(nanD) );

		store->BeginTile( 0, 0 );
		beauty->At( 0, 0 ) = RISEPel( 2.5,  1.0,  0.5  );  // ordinary HDR
		beauty->At( 1, 0 ) = RISEPel( -0.3, 0.5,  0.5  );  // negative R
		beauty->At( 2, 0 ) = RISEPel( 0.5,  nanD, 0.5  );  // NaN G
		beauty->At( 3, 0 ) = RISEPel( 0.5,  0.5,  posInf ); // +Inf B
		store->EndTile( 0, 0 );

		ViewTransform xf = ViewTransform::Identity();

		// ─ Test ROMM archival ─
		// Post Stage B colour-space migration, RGBA32F_ROMM_Linear
		// applies a REAL Rec.709→ROMM Bradford+matrix conversion (it
		// no longer aliases RISEPel which is now Rec.709 not ROMM).
		// The archival invariants still hold — the matrix is bounded
		// (~5-7 % off-diagonals), preserves the white axis, and does
		// NOT sanitize negatives / NaN / Inf — but exact bit-identity
		// only holds for the sRGB-aliased path RGBA32F_Linear below.
		float rommBuf[ 4 * 4 ] = { 0 };
		store->Render( rommBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_ROMM_Linear, xf );

		// Pixel 0: ordinary HDR survives finite and roughly in range.
		// The Rec.709→ROMM matrix mixes channels (~±10 %); we only
		// assert finite-and-bounded, not bit-identity.  Use a bit-
		// pattern check because under -ffast-math std::isfinite may be
		// constant-folded by the compiler (kIsFinite = exponent != all-
		// ones AND value is normal/zero, ignoring sign).
		auto isFiniteBits = []( float f ) -> bool {
			uint32_t bits;
			std::memcpy( &bits, &f, sizeof(bits) );
			return ( ( bits & 0x7F800000u ) != 0x7F800000u );  // exp != 0xFF (not Inf/NaN)
		};
		Check( isFiniteBits( rommBuf[ 0 * 4 + 0 ] ) && rommBuf[ 0 * 4 + 0 ] > 0.0f,
			"ROMM archival pixel 0 R finite positive" );
		Check( isFiniteBits( rommBuf[ 0 * 4 + 1 ] ) && rommBuf[ 0 * 4 + 1 ] > 0.0f,
			"ROMM archival pixel 0 G finite positive" );
		Check( isFiniteBits( rommBuf[ 0 * 4 + 2 ] ) && rommBuf[ 0 * 4 + 2 ] > 0.0f,
			"ROMM archival pixel 0 B finite positive" );

		// Pixel 1: matrix doesn't apply a Sanitise floor (NaN / negative
		// pathologicals can still emit non-zero output rather than be
		// silently zeroed).  We don't assert a specific output value
		// because the Rec.709→ROMM matrix has all-positive coefficients
		// in its top row, so a small negative R in mixed with positive
		// G+B can still produce all-positive output (it's the matrix's
		// behaviour, not a Sanitise step).  The next stanza below
		// tests the cleaner "negative survives the matrix" invariant
		// against `RGBA32F_Linear` (identity target) where the
		// invariant holds tautologically post Stage B.

		// Pixel 2 / 3: NaN/Inf preservation through the matrix multiply
		// is NOT a reliable invariant under -ffast-math (the compiler
		// is told no non-finite inputs exist, so `Inf - Inf = NaN`
		// branches may be folded away or special-cased differently
		// across compilers).  Bit-identical NaN/Inf preservation is
		// tested below against `RGBA32F_Linear`, which IS identity to
		// RISEPel and therefore round-trips bit-exactly.

		// ─ Test bit-exact archival via the RGBA32F_Linear path ─
		// Post Stage B, `RGBA32F_Linear` uses `FSColorSpace::sRGB_Linear`
		// which is identity to RISEPel = Rec.709.  No matrix multiply
		// is applied at the colour-space stage, no transfer function
		// is applied (Linear), so the float bytes are bit-identical
		// to RISEPel internal storage modulo the double→float cast.
		// Negatives, NaN, and Inf MUST pass through unchanged — this
		// is the strict archival contract.
		float srgbBuf[ 4 * 4 ] = { 0 };
		store->Render( srgbBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_Linear, xf );

		// Pixel 0: ordinary HDR survives bit-identically.
		Check( srgbBuf[ 0 * 4 + 0 ] == 2.5f, "RGBA32F_Linear archival pixel 0 R = 2.5" );
		Check( srgbBuf[ 0 * 4 + 1 ] == 1.0f, "RGBA32F_Linear archival pixel 0 G = 1.0" );
		Check( srgbBuf[ 0 * 4 + 2 ] == 0.5f, "RGBA32F_Linear archival pixel 0 B = 0.5" );

		// Pixel 1: negative R PRESERVED (key archival invariant —
		// identity target = no Sanitise floor possible).
		Check( srgbBuf[ 1 * 4 + 0 ] == -0.3f,
			"RGBA32F_Linear archival preserves negative R (no Sanitise clamp)" );

		// Pixel 2: NaN G PRESERVED.  Verify via bit pattern (under
		// -ffast-math, isnan() may constant-fold to false).
		uint32_t nanGBits;
		std::memcpy( &nanGBits, &srgbBuf[ 2 * 4 + 1 ], sizeof(nanGBits) );
		const bool gIsNaN = ( ( nanGBits & 0x7F800000u ) == 0x7F800000u )
		                 && ( ( nanGBits & 0x007FFFFFu ) != 0u );
		Check( gIsNaN, "RGBA32F_Linear archival preserves NaN G (bit-pattern check)" );

		// Pixel 3: +Inf B PRESERVED.
		uint32_t infBBits;
		std::memcpy( &infBBits, &srgbBuf[ 3 * 4 + 2 ], sizeof(infBBits) );
		const bool bIsPosInf = ( infBBits == 0x7F800000u );
		Check( bIsPosInf, "RGBA32F_Linear archival preserves +Inf B (bit-pattern check)" );

		// Pixel 0 ordinary positive: bit-identical positive (asserted
		// strictly above; this is the loose-bound finite-positive
		// version that survives an eventual identity-vs-matrix-target
		// distinction).
		Check( srgbBuf[ 0 * 4 + 0 ] > 0.0f && srgbBuf[ 0 * 4 + 0 ] < 10.0f,
			"sRGB archival pixel 0 R finite positive" );

		// Pixel 1 negative R — RGBA32F_Linear is identity to RISEPel
		// post Stage B (no matrix multiply at the colour-space stage),
		// so the input negative passes through unchanged.  Crucially,
		// no ZERO floor was applied.
		const bool anySRGBNegative =
			   srgbBuf[ 1 * 4 + 0 ] < 0.0f
			|| srgbBuf[ 1 * 4 + 1 ] < 0.0f
			|| srgbBuf[ 1 * 4 + 2 ] < 0.0f;
		Check( anySRGBNegative,
			"sRGB archival preserves negative (no Sanitise floor)" );

		// Alpha is 1.0 (default) for all pixels and should pass
		// through bit-identically on float targets per our gated
		// alpha-sanitise rule (skips clamp on float targets).
		Check( srgbBuf[ 0 * 4 + 3 ] == 1.0f, "alpha float passthrough" );
		Check( rommBuf[ 1 * 4 + 3 ] == 1.0f, "alpha ROMM passthrough" );

		// ─ Verify alpha out-of-range survives float archival ─
		// Set pixel 0's alpha to -0.5 (negative — premultiplied
		// reconstruction artifact) and 5.0 (over-1 HDR alpha).
		// Both should pass through bit-identically on float targets.
		store->BeginTile( 0, 0 );
		auto* alpha = store->GetChannel<ChannelId::Alpha>();
		alpha->At( 0, 0 ) = -0.5f;
		alpha->At( 1, 0 ) = 5.0f;
		store->EndTile( 0, 0 );

		store->Render( rommBuf, 4 * 4 * sizeof(float),
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA32F_ROMM_Linear, xf );
		Check( rommBuf[ 0 * 4 + 3 ] == -0.5f,
			"float archival preserves negative alpha (no LDR clamp)" );
		Check( rommBuf[ 1 * 4 + 3 ] == 5.0f,
			"float archival preserves alpha > 1 (no LDR clamp)" );

		// ─ Same alphas through LDR target should clamp ─
		uint8_t ldrBuf[ 4 * 4 ] = { 0 };
		store->Render( ldrBuf, 4 * 4,
			Rect( 0, 0, 1, 4 ),
			TargetFormat::RGBA8_sRGB, xf );
		Check( ldrBuf[ 0 * 4 + 3 ] == 0,
			"LDR target clamps negative alpha to 0" );
		Check( ldrBuf[ 1 * 4 + 3 ] == 255,
			"LDR target clamps alpha > 1 to 255" );

		store->release();
	}

	// ─── Section 9: CopyTileFromRasterImage ingest path ───────────
	void TestCopyTileFromRasterImage()
	{
		// Build a source IRasterImage from FrameStore "B" then copy
		// a tile of B into FrameStore "A" via the ingest path.
		FrameStore* a = MakeStore( 16, 16, 8 );
		FrameStore* b = MakeStore( 16, 16, 8 );

		// Fill B's tile (1, 0) with a known color.
		auto* bBeauty = b->GetChannel<ChannelId::Beauty>();
		auto* bAlpha  = b->GetChannel<ChannelId::Alpha>();
		for ( unsigned y = 0; y < 8; ++y ) {
			for ( unsigned x = 8; x < 16; ++x ) {
				bBeauty->At( x, y ) = RISEPel( 0.7, 0.4, 0.2 );
				bAlpha->At( x, y ) = 0.5f;
			}
		}

		// Copy B's tile (1, 0) into A's tile (1, 0).
		IRasterImage& bView = b->AsBeautyRasterImage();
		const Rect srcRect( 0, 8, 8, 16 );
		a->CopyTileFromRasterImage( 1, 0, bView, srcRect );

		auto* aBeauty = a->GetChannel<ChannelId::Beauty>();
		auto* aAlpha  = a->GetChannel<ChannelId::Alpha>();
		Check( aBeauty->At( 9, 3 ).r == 0.7 && aBeauty->At( 14, 6 ).g == 0.4,
			"CopyTileFromRasterImage copies Beauty" );
		Check( ApproxEq( aAlpha->At( 9, 3 ), 0.5f, 1e-6f ),
			"CopyTileFromRasterImage copies Alpha" );

		// Tile (0, 0) (untouched) still default.
		Check( aBeauty->At( 0, 0 ).r == 0.0,
			"CopyTileFromRasterImage doesn't touch other tiles" );

		// Generation advanced.
		Check( a->Generation() > 0, "CopyTileFromRasterImage bumps generation" );

		a->release();
		b->release();
	}

	// ─── Section 10: regional post-processing confinement ─────────
	void TestSnapshotRestore()
	{
		FrameStore* store = MakeStore( 2, 2, 1, {
			ChannelId::Albedo, ChannelId::Normal, ChannelId::Depth,
			ChannelId::ObjectId, ChannelId::PrimitiveId } );
		store->AsBeautyRasterImage().SetPEL( 0, 0,
			RISEColor(RISEPel(1.0,2.0,3.0),0.25) );
		store->GetChannel<ChannelId::Albedo>()->At(0,0) = RISEPel(4.0,5.0,6.0);
		store->GetChannel<ChannelId::Normal>()->At(0,0) = Vector3(7.0,8.0,9.0);
		store->GetChannel<ChannelId::Depth>()->At(0,0) = 10.0f;
		store->GetChannel<ChannelId::ObjectId>()->At(0,0) = 11u;
		store->GetChannel<ChannelId::PrimitiveId>()->At(0,0) = 12u;
		FrameStore::Metadata metadata;
		metadata.renderFidelityStatus = "preview";
		metadata.rendererBuildId = "prior";
		store->SetMetadata(metadata);
		const FrameStore::Snapshot snapshot = store->CaptureSnapshot();

		store->AsBeautyRasterImage().SetPEL( 0, 0, RISEColor(RISEPel(0.0),1.0) );
		store->GetChannel<ChannelId::Albedo>()->At(0,0) = RISEPel(0.0);
		store->GetChannel<ChannelId::Normal>()->At(0,0) = Vector3(0.0,0.0,0.0);
		store->GetChannel<ChannelId::Depth>()->At(0,0) = 0.0f;
		store->GetChannel<ChannelId::ObjectId>()->At(0,0) = 0u;
		store->GetChannel<ChannelId::PrimitiveId>()->At(0,0) = 0u;
		store->SetMetadata(FrameStore::Metadata());
		const uint64_t generationBeforeRestore = store->Generation();
		const bool restored = store->RestoreSnapshot(snapshot);
		const RISEColor beauty = store->AsBeautyRasterImage().GetPEL(0,0);
		const RISEPel albedo = store->GetChannel<ChannelId::Albedo>()->At(0,0);
		const Vector3 normal = store->GetChannel<ChannelId::Normal>()->At(0,0);
		Check( restored && beauty.base[0] == 1.0 && beauty.base[1] == 2.0 &&
			beauty.base[2] == 3.0 && beauty.a == 0.25 &&
			albedo[0] == 4.0 && albedo[1] == 5.0 && albedo[2] == 6.0 &&
			normal.x == 7.0 && normal.y == 8.0 && normal.z == 9.0 &&
			store->GetChannel<ChannelId::Depth>()->At(0,0) == 10.0f &&
			store->GetChannel<ChannelId::ObjectId>()->At(0,0) == 11u &&
			store->GetChannel<ChannelId::PrimitiveId>()->At(0,0) == 12u &&
			store->Meta().rendererBuildId == "prior" &&
			store->Generation() == generationBeforeRestore + 1u,
			"snapshot restore atomically recovers every channel, metadata, and generation" );
		store->release();
	}

	void TestRegionalPostProcessingConfinement()
	{
		const unsigned int w = 16, h = 16;
		IRasterImage* image = new RISERasterImage(
			w, h, RISEColor( RISEPel( 0.25, 0.5, 0.75 ), 0.6 ) );

		IPixelFilter* filter = nullptr;
		Check( RISE_API_CreateBoxPixelFilter( &filter, 2.0, 2.0 ) && filter,
			"regional filtered-film test creates a filter" );
		if( filter ) {
			RISE::Implementation::FilteredFilm* film =
				new RISE::Implementation::FilteredFilm( w, h );
			// Center the footprint across the left crop boundary: x=4 must
			// remain sentinel while the adjacent x=5 resolve proves useful
			// work happened inside the requested region.
			film->Splat( 4.75, 7.0, XYZPel( 2.0, 1.0, 0.5 ), *filter );
			const Rect region( 5, 5, 10, 10 );
			film->Resolve( *image, &region );
			const RISEColor outside = image->GetPEL( 4, 7 );
			Check( ApproxEq( outside.base.r, 0.25, 1e-12 )
			    && ApproxEq( outside.base.g, 0.5, 1e-12 )
			    && ApproxEq( outside.base.b, 0.75, 1e-12 )
			    && ApproxEq( outside.a, 0.6, 1e-12 ),
				"regional filtered-film resolve leaves sentinel pixels outside untouched" );
			const RISEColor inside = image->GetPEL( 5, 7 );
			Check( !ApproxEq( inside.base.r, 0.25, 1e-6 )
			    || !ApproxEq( inside.base.g, 0.5, 1e-6 )
			    || !ApproxEq( inside.base.b, 0.75, 1e-6 ),
				"regional filtered-film resolve still writes its in-region footprint" );
			film->release();
			filter->release();
		}

		// BDPT/VCM light-to-camera paths use SplatFilm, independently of
		// FilteredFilm. Resolve and its temporary pre-denoise inverse must
		// obey the same inclusive region bounds.
		RISE::Implementation::SplatFilm* splats =
			new RISE::Implementation::SplatFilm( w, h );
		splats->Splat( 4, 7, RISEPel( 3.0, 2.0, 1.0 ) );
		splats->Splat( 5, 7, RISEPel( 3.0, 2.0, 1.0 ) );
		splats->FlushCallingThreadBuffer();
		const Rect region( 5, 5, 10, 10 );
		const RISEColor outsideSplatBefore = image->GetPEL( 4, 7 );
		const RISEColor insideSplatBefore = image->GetPEL( 5, 7 );
		splats->Resolve( *image, 2.0, &region );
		Check( image->GetPEL( 4, 7 ).base.r == outsideSplatBefore.base.r,
			"regional splat resolve leaves a contributed outside pixel untouched" );
		Check( image->GetPEL( 5, 7 ).base.r > insideSplatBefore.base.r,
			"regional splat resolve adds its in-region contribution" );
		splats->Unresolve( *image, 2.0, &region );
		Check( ApproxEq( image->GetPEL( 5, 7 ).base.r, insideSplatBefore.base.r, 1e-12 ),
			"regional splat unresolve restores its in-region pixel" );
		RISE::Implementation::FlushCallingThreadSplatBuffer();
		splats->release();

		// Adaptive-map mode used to overwrite every outside pixel with zero
		// because an unrendered ProgressivePixel has sampleIndex==0.
		RISE::Implementation::ProgressiveFilm progressive( w, h );
		progressive.Get( 5, 7 ).sampleIndex = 4;
		const RISEColor progressiveOutsideBefore = image->GetPEL( 4, 7 );
		progressive.Resolve( *image, true, 8, &region );
		Check( image->GetPEL( 4, 7 ).base.r == progressiveOutsideBefore.base.r,
			"regional adaptive-map resolve preserves outside pixels" );
		Check( ApproxEq( image->GetPEL( 5, 7 ).base.r, 0.5, 1e-12 ),
			"regional adaptive-map resolve writes in-region sample progress" );
		for( unsigned int y=5; y<=10; ++y ) {
			for( unsigned int x=5; x<=10; ++x ) {
				progressive.Get( x, y ).sampleIndex = 8;
			}
		}
		Check( progressive.CountDone( 8, &region ) == 36,
			"regional progressive completion ignores unfinished outside pixels" );
		Check( progressive.CountDone( 8 ) == 36,
			"full-frame progressive completion still includes unfinished outside pixels" );

		const Rect quarterFrame( 0, 0, 7, 7 );
		Check(
			RISE::Implementation::BidirectionalRasterizerBase::RegionalPixelCount(
				w, h, &quarterFrame ) == 64,
			"regional camera metrics count selected pixels only" );
		Check( ApproxEq(
			RISE::Implementation::BidirectionalRasterizerBase::RegionalSplatSPP(
				8.0, w, h, &quarterFrame ), 2.0, 1e-12 ),
			"regional BDPT/VCM splat normalization scales by selected area" );

#ifdef RISE_ENABLE_OIDN
		// Fill the beauty image with deterministic high-frequency input, then
		// denoise only the middle crop. Outside RGB and alpha must remain exact.
		for( unsigned int y=0; y<h; ++y ) {
			for( unsigned int x=0; x<w; ++x ) {
				const double v = ( (x+y) & 1u ) ? 0.9 : 0.1;
				image->SetPEL( x, y, RISEColor( RISEPel( v, 0.5*v, 0.25*v ), 0.35 ) );
			}
		}
		const RISEColor outsideBefore = image->GetPEL( 1, 1 );
		std::vector<RISEColor> insideBefore;
		for( unsigned int y=4; y<=11; ++y ) {
			for( unsigned int x=4; x<=11; ++x ) insideBefore.push_back( image->GetPEL( x, y ) );
		}
		AOVBuffers noGuides( w, h, AOVBuffers::Plan( false, false, false ) );
		RISE::Implementation::OIDNDenoiser denoiser;
		denoiser.ApplyDenoiseRegion( *image, noGuides, w, h,
			4, 4, 11, 11, OidnQuality::Fast, OidnDevice::CPU,
			OidnPrefilter::Fast, 0.1 );
		const RISEColor outsideAfter = image->GetPEL( 1, 1 );
		Check( outsideAfter.base.r == outsideBefore.base.r
		    && outsideAfter.base.g == outsideBefore.base.g
		    && outsideAfter.base.b == outsideBefore.base.b
		    && outsideAfter.a == outsideBefore.a,
			"regional OIDN leaves every channel of an outside sentinel untouched" );
		Check( image->GetPEL( 6, 6 ).a == 0.35,
			"regional OIDN preserves alpha inside the crop" );
		bool changedInside = false;
		size_t insideIndex = 0;
		for( unsigned int y=4; y<=11 && !changedInside; ++y ) {
			for( unsigned int x=4; x<=11; ++x, ++insideIndex ) {
				const RISEColor after = image->GetPEL( x, y );
				const RISEColor before = insideBefore[insideIndex];
				if( !ApproxEq( after.base.r, before.base.r, 1e-6 )
				 || !ApproxEq( after.base.g, before.base.g, 1e-6 )
				 || !ApproxEq( after.base.b, before.base.b, 1e-6 ) ) {
					changedInside = true;
					break;
				}
			}
		}
		Check( changedInside, "regional OIDN denoises pixels inside the crop" );
#endif

		image->release();
	}
}

int main(int argc,char** argv)
{
	ProcessWatchdog watchdog;
	if(argc==2 && std::strcmp(argv[1],"--mlt-resource-only")==0) {
		TestManualSplatLifecycle();TestMLTResourceExceptions();
		std::cout << "MLT resource exceptions: passed " << gPassCount << ", failed " << gFailCount << "\n";
		return gFailCount==0?0:1;
	}
	if(argc==2 && std::strcmp(argv[1],"--film-bounds-only")==0) {
		TestFilmBoundsAndFootprints();
		std::cout << "Film bounds: passed " << gPassCount << ", failed " << gFailCount << "\n";
		return gFailCount==0?0:1;
	}
	if(argc==2 && std::strcmp(argv[1],"--tile-exception-only")==0) {
		TestTileBracketExceptions();TestRasterizerBlockExceptions();TestTileBracketSynchronousReadback();TestFullEntryExceptions();TestSplatTaskExceptions();
		std::cout << "FrameStore tile exceptions: passed " << gPassCount << ", failed " << gFailCount << "\n";
		return gFailCount==0?0:1;
	}
	std::cout << "FrameStoreTest L1 — buffer + tile locking + Render readback\n";
	std::cout << "------------------------------------------------------------\n";

	TestTileBracketExceptions();
	TestTileBracketSynchronousReadback();
	TestRasterizerBlockExceptions();
	TestFullEntryExceptions();
	TestSplatTaskExceptions();
	TestFilmBoundsAndFootprints();
	TestManualSplatLifecycle();
	TestMLTResourceExceptions();
	TestConstructionAndGeometry();
	TestAOVChannels();
	TestPlannedAOVBridge();
	TestTileLockingAndObserver();
	TestObserverSelfDetach();
	TestObserverCascadeRemovalNoUAF();
	TestObserverRemoveWaitsForInFlight();
	TestObserverRemovalFromUnrelatedCallbackWaitsForVictim();
	TestConcurrentSelfRemovalDrainsOtherThreads();
	TestSharedObserverCrossStoreRemovalIsSerialized();
	TestCrossStoreMutualRemovalCannotDeadlock();
	TestReentrantObserverDispatchFailsClosed();
	TestCrossThreadCrossStorePublicationDoesNotDeadlock();
	TestBulkBracketReleasesAllTilesBeforeNotification();
	TestConcurrentTileLocking();
	TestConcurrentFrameMetadata();
	TestRenderReadback();
	TestToneCurveGating();
	TestReferenceLifetime();
	TestBeautyRasterImageShim();
	TestHDRArchivalIdentity();
	TestCopyTileFromRasterImage();
	TestSnapshotRestore();
	TestRegionalPostProcessingConfinement();

	std::cout << "------------------------------------------------------------\n";
	std::cout << "passed " << gPassCount << ", failed " << gFailCount << "\n";
	return gFailCount == 0 ? 0 : 1;
}
