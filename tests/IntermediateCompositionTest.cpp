//////////////////////////////////////////////////////////////////////
//
//  IntermediateCompositionTest.cpp - Region-only preview composition
//  (PixelBasedRasterizerHelper / BidirectionalRasterizerBase
//  ::GetIntermediateOutputImage) and film read locking.
//
//  Workers call GetIntermediateOutputImage after every finished tile and
//  hand the result to the outputs with that tile's rect.  The getter used
//  to compose the WHOLE frame into ONE shared scratch image (and lazily
//  allocate it), so concurrent workers overwrote each other's pixels, read
//  primary pixels other workers were writing, raced the allocation, and
//  read film rows while writers committed to them.
//
//    A  deterministic oracle: a region's composed pixels equal the
//       full-frame composition (same film state); pixels outside the
//       region are never written (sentinel).  Bidirectional + base getter.
//    B  concurrent compose + consume of disjoint tiles (static films):
//       every read must match the oracle exactly.
//    C  concurrent film writers (SplatFilm::BatchCommit, FilteredFilm::Splat)
//       vs Resolve/Unresolve: every resolved row is a whole-batch snapshot.
//    D  scratch preparation: the real BDPT PreRenderSetup prepares the
//       scratch before any worker; concurrent getters never allocate.
//    E  real multi-threaded renders through RasterizeScene /
//       RasterizeSceneAnimation with a wide filter: BDPT (pel + spectral,
//       single-pass + progressive, one animation frame), VCM (single-pass,
//       progressive, animation frame) and PT.  Each must deliver composed
//       previews (an image that is not the primary), and the composition
//       scratch must be released when the render returns; each job renders
//       twice to show a released scratch is rebuilt for the next render.
//
//  Modes: (none) = all; --oracle, --compose, --films, --scratch, --render.
//  Build flag ICT_HEAD_API compiles against the pre-fix one-argument
//  getter (red-proof shim; the scratch is then allocated lazily).
//
//////////////////////////////////////////////////////////////////////

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/Library/Rendering/BDPTRasterizerBase.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/FilteredFilm.h"
#include "../src/Library/Rendering/SplatFilm.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/RasterImages/RasterImage.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IPixelFilter.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int gPass = 0, gFail = 0;
	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++gPass; } else { ++gFail; std::cout << "  FAIL: " << what << "\n"; }
	}

	const auto releaser = []( auto* p ){ if( p ) p->release(); };

	const unsigned int W = 83, H = 50;	// ragged edge tiles at 16

	RISEColor PrimaryValue( unsigned int x, unsigned int y )
	{
		return RISEColor( RISEPel( 0.01*x, 0.02*y, 0.003*(x+y) ), 1.0 );
	}

	bool Same( const RISEColor& a, const RISEColor& b )
	{
		return a.base.r == b.base.r && a.base.g == b.base.g && a.base.b == b.base.b && a.a == b.a;
	}

	// Positive tent-like filter with support 1.5 so a splat spans
	// neighbouring pixels (and rows) -- the case that made FilteredFilm
	// rows shared between tiles.
	class TestFilter : public virtual IPixelFilter, public virtual Reference
	{
		Scalar half;
		bool flat;
	public:
		TestFilter( Scalar h, bool f ) : half( h ), flat( f ) {}
		Scalar warp( const RandomNumberGenerator&, const Point2& c, Point2& w ) const override { w = c; return 1; }
		Scalar warpOnScreen( const RandomNumberGenerator&, const Point2& c, Point2& w, const unsigned int x, const unsigned int y ) const override
		{ w = Point2( c.x + x, c.y + y ); return 1; }
		Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const override
		{
			if( flat ) return 1;
			return ( 1.0 - std::fabs( dx ) / ( half + 0.5 ) ) * ( 1.0 - std::fabs( dy ) / ( half + 0.5 ) );
		}
		void GetFilterSupport( Scalar& hw, Scalar& hh ) const override { hw = half; hh = half; }
	protected:
		~TestFilter() override {}
	};

	// Probes ------------------------------------------------------------

	struct BidiProbe final : BDPTRasterizerBase
	{
		BidiProbe( IRayCaster* caster ) :
			Rasterizer( nullptr ), PixelBasedRasterizerHelper( caster, nullptr ),
			BDPTRasterizerBase( caster, 3, 3, PathGuidingConfig(), StabilityConfig(), nullptr ) {}
		bool TakeSingleSample( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel& ) const override { return false; }
		const char* GetProgressTitle() const override { return "composition probe: "; }
		void IntegratePixel( const RuntimeContext&, unsigned int, unsigned int, unsigned int, const IScene&,
			RISEColor& c, bool, Scalar, Scalar ) const override { c = RISEColor( 0, 0, 0, 1 ); }

		void Install( SplatFilm* splat, FilteredFilm* filtered, const Rect* splatRegion, Scalar spp ) const
		{
			if( splat ) splat->addref();
			if( filtered ) filtered->addref();
			safe_release( pSplatFilm ); pSplatFilm = splat;
			safe_release( pFilteredFilm ); pFilteredFilm = filtered;
			ConfigureSplatRegion( splatRegion, W, H );
			mSplatTotalSamples = spp;
			mTotalAdaptiveSamples.store( 0 );
		}
		void Prepare() const
		{
#ifndef ICT_HEAD_API
			EnsureCompositionScratch( W, H );
#endif
		}
		void RealPreRenderSetup( const IScene& scene ) const { BDPTRasterizerBase::PreRenderSetup( scene, nullptr ); }
		IRasterImage& Compose( IRasterImage& primary, const Rect* region ) const
		{
#ifdef ICT_HEAD_API
			(void)region;
			return GetIntermediateOutputImage( primary );
#else
			return GetIntermediateOutputImage( primary, region );
#endif
		}
		IRasterImage* Scratch() const { return pScratchImage; }
		void DropScratch() const { safe_release( pScratchImage ); pScratchImage = 0; }
		Scalar EffectiveSPP() const { return GetEffectiveSplatSPP( W, H ); }
		const Rect* Active() const { return ActiveSplatRegion(); }
		SplatFilm* Splat() const { return pSplatFilm; }
	};

	struct BaseProbe final : PixelBasedRasterizerHelper
	{
		BaseProbe( IRayCaster* caster ) : Rasterizer( nullptr ), PixelBasedRasterizerHelper( caster, nullptr ) {}
		bool TakeSingleSample( const RuntimeContext&, const RasterizerState&, const Ray&, RISEPel& ) const override { return false; }
		void IntegratePixel( const RuntimeContext&, unsigned int, unsigned int, unsigned int, const IScene&,
			RISEColor& c, bool, Scalar, Scalar ) const override { c = RISEColor( 0, 0, 0, 1 ); }
		void Install( FilteredFilm* filtered, const Rect* outputRegion ) const
		{
			filtered->addref();
			safe_release( pFilteredFilm ); pFilteredFilm = filtered;
			safe_release( pFilteredScratch );
			pFilteredScratch = new RISERasterImage( W, H, RISEColor( 0, 0, 0, 0 ) );
			// ConfigureOutputRegion is private: the base probe runs with no
			// output crop (the crop intersection is the same IntersectRegion
			// helper the bidirectional crop rows exercise).
			(void)outputRegion;
		}
		IRasterImage& Compose( IRasterImage& primary, const Rect* region ) const
		{
#ifdef ICT_HEAD_API
			(void)region;
			return GetIntermediateOutputImage( primary );
#else
			return GetIntermediateOutputImage( primary, region );
#endif
		}
		IRasterImage* Scratch() const { return pFilteredScratch; }
		const Rect* Active() const { return ForTest_HasActiveOutputRegion() ? &region_ : nullptr; }
		void CacheActive() { region_ = ForTest_ActiveOutputRegion(); }
		Rect region_ = Rect( 0, 0, 0, 0 );
	};

	IRayCaster* MakeCaster()
	{
		StandardShader* shader = new StandardShader( std::vector<IShaderOp*>() );
		IRayCaster* caster = nullptr;
		RISE_API_CreateRayCaster( &caster, false, 10, *shader, true );
		shader->release();
		return caster;
	}

	// Film contents --------------------------------------------------

	void LoadSplat( SplatFilm& film )
	{
		std::vector<SplatFilm::BatchRecord> recs;
		for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x ) {
			if( ( x*7 + y*3 ) % 5 == 0 ) {
				recs.push_back( { y*W + x, RISEPel( 0.1*(x%3), 0.2, 0.05*(y%4) ) } );
				if( x % 4 == 0 ) recs.push_back( { y*W + x, RISEPel( 0.3, 0.0, 0.125 ) } );
			}
		}
		film.BatchCommit( recs.data(), recs.size() );
	}

	void LoadFiltered( FilteredFilm& film, const IPixelFilter& filter )
	{
		// Covers only part of the frame so some pixels keep the primary.
		for( unsigned int j = 0; j < 14; ++j ) for( unsigned int i = 0; i < 30; ++i ) {
			const Scalar sx = 4.3 + i * 2.17, sy = 3.7 + j * 2.9;
			film.Splat( sx, sy, XYZPel( 0.2 + 0.01*i, 0.3, 0.1 + 0.02*j ), filter );
		}
	}

	RISERasterImage* MakePrimary()
	{
		RISERasterImage* img = new RISERasterImage( W, H, RISEColor( 0, 0, 0, 0 ) );
		for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x ) img->SetPEL( x, y, PrimaryValue( x, y ) );
		return img;
	}

	RISERasterImage* BidiOracle( const RISERasterImage& primary, const FilteredFilm* filtered, const SplatFilm* splat,
		const Rect* active, Scalar spp )
	{
		RISERasterImage* e = new RISERasterImage( W, H, RISEColor( 0, 0, 0, 0 ) );
		for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x ) e->SetPEL( x, y, primary.GetPEL( x, y ) );
		if( filtered ) filtered->Resolve( *e, active );
		if( splat ) splat->Resolve( *e, spp, active );
		return e;
	}

	bool Clamp( const Rect* r, Rect& out )
	{
		if( !r ) { out = Rect( 0, 0, H-1, W-1 ); return true; }
		if( r->left > r->right || r->top > r->bottom || r->left >= W || r->top >= H ) return false;
		out = Rect( r->top, r->left, std::min( r->bottom, H-1 ), std::min( r->right, W-1 ) );
		return true;
	}

	const RISEColor kSentinel( RISEPel( -7.0, 123.0, -9.5 ), 0.25 );

	void FillSentinel( IRasterImage& img )
	{
		for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x ) img.SetPEL( x, y, kSentinel );
	}

	// Checks: inside region == oracle, outside == sentinel (unless null).
	void CheckRegion( const char* label, IRasterImage& out, const IRasterImage& oracle, const Rect* region )
	{
		Rect r( 0, 0, 0, 0 );
		const bool any = Clamp( region, r );
		unsigned int insideBad = 0, outsideWritten = 0;
		for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x ) {
			const bool in = any && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
			const RISEColor v = out.GetPEL( x, y );
			if( in ) { if( !Same( v, oracle.GetPEL( x, y ) ) ) ++insideBad; }
			else if( region && !Same( v, kSentinel ) ) ++outsideWritten;
		}
		Check( insideBad == 0, std::string( label ) + ": region pixels equal the full-frame composition (" +
			std::to_string( insideBad ) + " differ)" );
		if( region ) {
			Check( outsideWritten == 0, std::string( label ) + ": pixels outside the region are not written (" +
				std::to_string( outsideWritten ) + " written)" );
		}
	}

	struct NamedRegion { const char* name; bool null; Rect r; };
	std::vector<NamedRegion> Regions()
	{
		return {
			{ "null region", true, Rect( 0, 0, 0, 0 ) },
			{ "interior tile", false, Rect( 16, 16, 31, 31 ) },
			{ "right/bottom edge tile", false, Rect( 48, 80, 63, 95 ) },
			{ "top-left tile", false, Rect( 0, 0, 15, 15 ) },
			{ "1x1", false, Rect( 7, 5, 7, 5 ) },
			{ "non-aligned", false, Rect( 3, 5, 20, 41 ) },
			{ "off-image", false, Rect( 60, 90, 70, 99 ) },
		};
	}

	// A ------------------------------------------------------------------
	void TestOracle()
	{
		std::cout << "A: deterministic composition oracle\n";
		std::unique_ptr<IRayCaster, decltype(releaser)> caster( MakeCaster(), releaser );
		std::unique_ptr<RISERasterImage, decltype(releaser)> primary( MakePrimary(), releaser );
		std::unique_ptr<TestFilter, decltype(releaser)> filter( new TestFilter( 1.5, false ), releaser );
		std::unique_ptr<SplatFilm, decltype(releaser)> splat( new SplatFilm( W, H ), releaser );
		std::unique_ptr<FilteredFilm, decltype(releaser)> filtered( new FilteredFilm( W, H ), releaser );
		LoadSplat( *splat ); LoadFiltered( *filtered, *filter );

		const Rect crop( 10, 12, 30, 60 );
		struct Cfg { const char* name; bool splat, filtered; const Rect* active; };
		const Cfg cfgs[] = {
			{ "bidi splat+filtered", true, true, nullptr },
			{ "bidi splat only", true, false, nullptr },
			{ "bidi filtered only", false, true, nullptr },
			{ "bidi splat+filtered, crop", true, true, &crop },
		};
		for( const Cfg& cfg : cfgs ) {
			std::unique_ptr<BidiProbe, decltype(releaser)> probe( new BidiProbe( caster.get() ), releaser );
			probe->Install( cfg.splat ? splat.get() : nullptr, cfg.filtered ? filtered.get() : nullptr, cfg.active, 7.0 );
			probe->Prepare();
			std::unique_ptr<RISERasterImage, decltype(releaser)> oracle(
				BidiOracle( *primary, cfg.filtered ? filtered.get() : nullptr, cfg.splat ? splat.get() : nullptr,
					probe->Active(), probe->EffectiveSPP() ), releaser );
			// Independent closed-form spot check of the oracle: (1,1) gets
			// one splat record (0.1,0.2,0.05) and no filtered weight.
			if( cfg.splat && !cfg.filtered && !cfg.active ) {
				const RISEColor o = oracle->GetPEL( 1, 1 );
				const RISEColor p = PrimaryValue( 1, 1 );
				Check( std::fabs( o.base.g - ( p.base.g + 0.2/7.0 ) ) < 1e-15 &&
				       std::fabs( o.base.r - ( p.base.r + 0.1/7.0 ) ) < 1e-15,
					"oracle closed form: splat-only pixel = primary + c/spp" );
			}
			for( const NamedRegion& nr : Regions() ) {
				const Rect* region = nr.null ? nullptr : &nr.r;
#ifdef ICT_HEAD_API
				if( !probe->Scratch() ) probe->Compose( *primary, nullptr );
#endif
				IRasterImage* scratch = probe->Scratch();
				if( scratch ) FillSentinel( *scratch );
				IRasterImage& out = probe->Compose( *primary, region );
				Rect r( 0, 0, 0, 0 );
				if( !Clamp( region, r ) ) {
					Check( &out == primary.get() || &out == scratch, std::string( cfg.name ) + " / off-image returns a valid image" );
					unsigned int written = 0;
					for( unsigned int y = 0; y < H; ++y ) for( unsigned int x = 0; x < W; ++x )
						if( scratch && !Same( scratch->GetPEL( x, y ), kSentinel ) ) ++written;
					Check( written == 0, std::string( cfg.name ) + " / off-image region writes nothing (" + std::to_string( written ) + ")" );
					continue;
				}
				Check( &out == scratch, std::string( cfg.name ) + " / " + nr.name + ": composes into the prepared scratch" );
				CheckRegion( ( std::string( cfg.name ) + " / " + nr.name ).c_str(), out, *oracle, region );
			}
		}

		// Base getter (PT / pixel rasterizers with a wide filter).
		const Rect* bases[] = { nullptr };
		for( const Rect* active : bases ) {
			std::unique_ptr<BaseProbe, decltype(releaser)> probe( new BaseProbe( caster.get() ), releaser );
			probe->Install( filtered.get(), active );
			probe->CacheActive();
			std::unique_ptr<RISERasterImage, decltype(releaser)> oracle(
				BidiOracle( *primary, filtered.get(), nullptr, probe->Active(), 1.0 ), releaser );
			for( const NamedRegion& nr : Regions() ) {
				const Rect* region = nr.null ? nullptr : &nr.r;
				Rect r( 0, 0, 0, 0 );
				if( !Clamp( region, r ) ) continue;
				FillSentinel( *probe->Scratch() );
				IRasterImage& out = probe->Compose( *primary, region );
				CheckRegion( ( std::string( active ? "base, crop" : "base" ) + " / " + nr.name ).c_str(), out, *oracle, region );
			}
		}
	}

	// B ------------------------------------------------------------------
	template<class Probe>
	unsigned long RunConcurrentCompose( const Probe& probe, IRasterImage& primary, const IRasterImage& oracle,
		unsigned int threads, unsigned int rounds )
	{
		std::vector<Rect> tiles;
		for( unsigned int y = 0; y < H; y += 16 ) for( unsigned int x = 0; x < W; x += 16 )
			tiles.push_back( Rect( y, x, std::min( y + 15, H - 1 ), std::min( x + 15, W - 1 ) ) );
		std::atomic<unsigned long> bad{ 0 };
		std::atomic<unsigned int> ready{ 0 };
		std::vector<std::thread> pool;
		for( unsigned int t = 0; t < threads; ++t ) {
			pool.emplace_back( [&, t]() {
				++ready; while( ready.load() < threads ) {}
				for( unsigned int round = 0; round < rounds; ++round ) {
					for( size_t i = t; i < tiles.size(); i += threads ) {
						const Rect& r = tiles[i];
						IRasterImage& out = probe.Compose( primary, &r );
						// "Consume": read only the rect, like every real output.
						for( unsigned int y = r.top; y <= r.bottom; ++y ) for( unsigned int x = r.left; x <= r.right; ++x )
							if( !Same( out.GetPEL( x, y ), oracle.GetPEL( x, y ) ) ) ++bad;
					}
				}
			} );
		}
		for( auto& th : pool ) th.join();
		return bad.load();
	}

	void TestConcurrentCompose()
	{
		std::cout << "B: concurrent compose + consume of disjoint tiles\n";
		std::unique_ptr<IRayCaster, decltype(releaser)> caster( MakeCaster(), releaser );
		std::unique_ptr<RISERasterImage, decltype(releaser)> primary( MakePrimary(), releaser );
		std::unique_ptr<TestFilter, decltype(releaser)> filter( new TestFilter( 1.5, false ), releaser );
		std::unique_ptr<SplatFilm, decltype(releaser)> splat( new SplatFilm( W, H ), releaser );
		std::unique_ptr<FilteredFilm, decltype(releaser)> filtered( new FilteredFilm( W, H ), releaser );
		LoadSplat( *splat ); LoadFiltered( *filtered, *filter );
		const unsigned int threads = 8, rounds = 60;
		{
			std::unique_ptr<BidiProbe, decltype(releaser)> probe( new BidiProbe( caster.get() ), releaser );
			probe->Install( splat.get(), filtered.get(), nullptr, 7.0 );
			probe->Prepare();
			probe->Compose( *primary, nullptr );	// HEAD shim: allocate before the race
			std::unique_ptr<RISERasterImage, decltype(releaser)> oracle(
				BidiOracle( *primary, filtered.get(), splat.get(), probe->Active(), probe->EffectiveSPP() ), releaser );
			const unsigned long bad = RunConcurrentCompose( *probe, *primary, *oracle, threads, rounds );
			Check( bad == 0, "bidirectional: every consumed tile pixel matches the oracle (" + std::to_string( bad ) + " mismatches)" );
		}
		{
			std::unique_ptr<BaseProbe, decltype(releaser)> probe( new BaseProbe( caster.get() ), releaser );
			probe->Install( filtered.get(), nullptr );
			std::unique_ptr<RISERasterImage, decltype(releaser)> oracle(
				BidiOracle( *primary, filtered.get(), nullptr, nullptr, 1.0 ), releaser );
			const unsigned long bad = RunConcurrentCompose( *probe, *primary, *oracle, threads, rounds );
			Check( bad == 0, "base: every consumed tile pixel matches the oracle (" + std::to_string( bad ) + " mismatches)" );
		}
	}

	// C ------------------------------------------------------------------
	void TestFilmWritersVsResolve()
	{
		std::cout << "C: concurrent film writers vs Resolve\n";
		const unsigned int FW = 64, FH = 16;
		const RISEPel c( 0.5, 0.25, 0.125 );
		std::unique_ptr<SplatFilm, decltype(releaser)> splat( new SplatFilm( FW, FH ), releaser );
		std::unique_ptr<FilteredFilm, decltype(releaser)> filtered( new FilteredFilm( FW, FH ), releaser );
		std::unique_ptr<TestFilter, decltype(releaser)> flat( new TestFilter( 200.0, true ), releaser );
		std::unique_ptr<RISERasterImage, decltype(releaser)> target( new RISERasterImage( FW, FH, RISEColor( 0, 0, 0, 0 ) ), releaser );
		const RISEPel filteredExpected = RISEPel( XYZPel( c ) );

		std::vector<SplatFilm::BatchRecord> all;
		for( unsigned int i = 0; i < FW*FH; ++i ) all.push_back( { i, c } );

		std::atomic<bool> stop{ false };
		std::atomic<unsigned long> splatTorn{ 0 }, splatBadValue{ 0 }, filteredTorn{ 0 }, resolves{ 0 };
		// Resolve calls that overlapped a film write: a write that had not
		// finished when the call began (commits) had started (started) by the
		// time it returned.  Writers start once a resolver has finished a
		// pass and keep writing past their 400 commits until such a call has
		// happened, so the checks below are exercised even on a busy machine.
		std::atomic<unsigned long> overlapResolves{ 0 }, commits{ 0 }, started{ 0 };
		std::vector<std::thread> threads;
		for( int w = 0; w < 3; ++w ) {
			threads.emplace_back( [&]() {
				while( resolves.load() == 0 ) std::this_thread::yield();
				for( int i = 0; i < 400 || overlapResolves.load() == 0; ++i ) {
					++started;
					splat->BatchCommit( all.data(), all.size() );
					filtered->Splat( 32.0, 8.0, XYZPel( c ), *flat );
					++commits;
				}
			} );
		}
		const unsigned int resolvers = 4;
		for( unsigned int rI = 0; rI < resolvers; ++rI ) {
			threads.emplace_back( [&, rI]() {
				const unsigned int y0 = rI * ( FH / resolvers ), y1 = y0 + FH / resolvers - 1;
				const Rect region( y0, 0, y1, FW - 1 );
				while( !stop.load() ) {
					for( int mode = 0; mode < 3; ++mode ) {
						for( unsigned int y = y0; y <= y1; ++y ) for( unsigned int x = 0; x < FW; ++x )
							target->SetPEL( x, y, RISEColor( 0, 0, 0, 0 ) );
						const unsigned long finishedBefore = commits.load();
						if( mode == 0 ) splat->Resolve( *target, 1.0, &region );
						else if( mode == 1 ) splat->Unresolve( *target, 1.0, &region );
						else filtered->Resolve( *target, &region );
						if( started.load() > finishedBefore ) ++overlapResolves;
						for( unsigned int y = y0; y <= y1; ++y ) {
							const RISEColor first = target->GetPEL( 0, y );
							for( unsigned int x = 0; x < FW; ++x ) {
								const RISEColor v = target->GetPEL( x, y );
								if( !Same( v, first ) ) {
									if( mode == 2 ) ++filteredTorn; else ++splatTorn;
								}
								if( mode == 2 ) {
									if( v.a != 0 && std::fabs( v.base.r - filteredExpected.r ) > 1e-9 * std::fabs( filteredExpected.r ) ) ++filteredTorn;
								} else {
									const double k = ( mode == 0 ? v.base.r : -v.base.r ) / c.r;
									if( k < 0 || k != std::floor( k ) || v.base.g != ( mode == 0 ? 1 : -1 ) * k * c.g ) ++splatBadValue;
								}
							}
						}
						++resolves;
					}
				}
			} );
		}
		for( int w = 0; w < 3; ++w ) threads[w].join();
		stop.store( true );
		for( size_t i = 3; i < threads.size(); ++i ) threads[i].join();
		std::cout << "  resolves: " << resolves.load() << ", of which during writes: " << overlapResolves.load() << "\n";
		Check( overlapResolves.load() > 0, "resolves ran while writers were writing" );
		Check( splatTorn.load() == 0, "SplatFilm Resolve/Unresolve rows are whole-batch snapshots (" + std::to_string( splatTorn.load() ) + " torn pixels)" );
		Check( splatBadValue.load() == 0, "SplatFilm resolved pixels are whole multiples of the record (" + std::to_string( splatBadValue.load() ) + ")" );
		Check( filteredTorn.load() == 0, "FilteredFilm Resolve rows read colorSum/weightSum consistently (" + std::to_string( filteredTorn.load() ) + ")" );

		// Final state, single-threaded: one record per pixel per commit.
		for( unsigned int y = 0; y < FH; ++y ) for( unsigned int x = 0; x < FW; ++x ) target->SetPEL( x, y, RISEColor( 0, 0, 0, 0 ) );
		splat->Resolve( *target, 1.0 );
		Check( std::fabs( target->GetPEL( 5, 5 ).base.r - double( commits.load() ) * c.r ) < 1e-9 * double( commits.load() ),
			"SplatFilm final count is one record per commit (" + std::to_string( commits.load() ) + ")" );
	}

	// D ------------------------------------------------------------------
	void TestScratchPreparation()
	{
		std::cout << "D: scratch preparation (training-pass shape)\n";
		std::unique_ptr<Job, decltype(releaser)> job( new Job(), releaser );
		const double location[] = { 0, 0, 2 }, lookAt[] = { 0, 0, 0 }, up[] = { 0, 1, 0 }, orientation[] = { 0, 0, 0 }, target[] = { 0, 0 };
		const bool setup = job->SetFilm( W, H, 1 ) &&
			job->AddPinholeCamera( "ict", location, lookAt, up, 1, 0, 0, 0, orientation, target );
		Check( setup, "scene with film" );
		if( !setup ) return;
		std::unique_ptr<IRayCaster, decltype(releaser)> caster( MakeCaster(), releaser );
		std::unique_ptr<RISERasterImage, decltype(releaser)> primary( MakePrimary(), releaser );
		std::unique_ptr<BidiProbe, decltype(releaser)> probe( new BidiProbe( caster.get() ), releaser );

		// 1. Concurrent getters with NO scratch and an active splat film
		//    must not allocate (they return the uncomposited primary).
		{
			std::unique_ptr<SplatFilm, decltype(releaser)> splat( new SplatFilm( W, H ), releaser );
			probe->Install( splat.get(), nullptr, nullptr, 1.0 );
			probe->DropScratch();
			std::atomic<unsigned int> ready{ 0 };
			std::atomic<unsigned int> returnedPrimary{ 0 };
			std::vector<std::thread> pool;
			const unsigned int N = 8;
			for( unsigned int t = 0; t < N; ++t ) pool.emplace_back( [&, t]() {
				++ready; while( ready.load() < N ) {}
				const Rect r( 0, t * 10, 15, t * 10 + 9 );
				if( &probe->Compose( *primary, &r ) == primary.get() ) ++returnedPrimary;
			} );
			for( auto& th : pool ) th.join();
			Check( probe->Scratch() == nullptr, "concurrent getters never allocate the scratch" );
			Check( returnedPrimary.load() == N, "without a prepared scratch the getter returns the primary" );
			probe->DropScratch();
		}

		// 2. The real BDPT PreRenderSetup (which runs before path-guiding
		//    training and every pass) prepares the scratch; concurrent
		//    workers then share one stable, correctly sized buffer.
		probe->RealPreRenderSetup( *job->GetScene() );
		IRasterImage* before = probe->Scratch();
		Check( before != nullptr, "PreRenderSetup prepared the composition scratch before any worker" );
		Check( before && before->GetWidth() == W && before->GetHeight() == H, "scratch has the film dimensions" );
		LoadSplat( *probe->Splat() );
		std::unique_ptr<RISERasterImage, decltype(releaser)> oracle(
			BidiOracle( *primary, nullptr, probe->Splat(), probe->Active(), probe->EffectiveSPP() ), releaser );
		const unsigned long bad = RunConcurrentCompose( *probe, *primary, *oracle, 8, 20 );
		Check( bad == 0, "training-shape concurrent compose matches the oracle (" + std::to_string( bad ) + ")" );
		Check( probe->Scratch() == before, "the scratch pointer is stable across the concurrent pass" );
		Check( probe->Scratch() && probe->Scratch()->refcount() == 1, "the scratch holds exactly one reference (no leak/double alloc)" );
		probe->RealPreRenderSetup( *job->GetScene() );
		Check( probe->Scratch() == before, "a repeated PreRenderSetup with unchanged dimensions keeps the buffer" );
	}

	// E ------------------------------------------------------------------
	// Captures every delivery.  Every distinct intermediate image is
	// referenced on first sight (so an address cannot be reused by a later
	// allocation and alias another image), and the final images
	// (OutputImage = the rasterizer's primary) are recorded.  A post-block
	// delivery whose image is NOT a final image is a composed preview: if
	// production silently fell back to delivering the primary, there would
	// be none.  After the render, refcount()==1 on a composed image means the
	// rasterizer released its composition scratch.
	class CaptureOutput : public virtual IRasterizerOutput, public virtual Reference
	{
	public:
		std::mutex m;
		std::vector<Rect> rects;
		std::vector<const IRasterImage*> rectImages;	///< per intermediate delivery
		std::vector<const IRasterImage*> held;			///< distinct non-final, referenced
		std::vector<const IRasterImage*> finalsSeen;
		unsigned long nonFinite = 0, finals = 0;
		void Hold( const IRasterImage& img )
		{
			if( IsFinal( &img ) ) return;
			for( const IRasterImage* h : held ) if( h == &img ) return;
			img.addref(); held.push_back( &img );
		}
		//! The primary may be a FrameStore view the store owns outright, so
		//! no reference to it is kept past the moment it is identified.
		void Unhold( const IRasterImage& img )
		{
			for( size_t i = 0; i < held.size(); ++i ) if( held[i] == &img ) {
				held.erase( held.begin() + i ); img.release(); return;
			}
		}
		void OutputIntermediateImage( const IRasterImage& img, const Rect* r ) override
		{
			Rect rc( 0, 0, img.GetHeight() - 1, img.GetWidth() - 1 );
			if( r ) rc = *r;
			unsigned long bad = 0;
			for( unsigned int y = rc.top; y <= rc.bottom && y < img.GetHeight(); ++y )
				for( unsigned int x = rc.left; x <= rc.right && x < img.GetWidth(); ++x ) {
					const RISEColor c = img.GetPEL( x, y );
					if( !std::isfinite( c.base.r ) || !std::isfinite( c.base.g ) || !std::isfinite( c.base.b ) ) ++bad;
				}
			std::lock_guard<std::mutex> lk( m );
			rects.push_back( rc ); rectImages.push_back( &img ); nonFinite += bad;
			Hold( img );
		}
		void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
		{
			unsigned long bad = 0;
			for( unsigned int y = 0; y < img.GetHeight(); ++y ) for( unsigned int x = 0; x < img.GetWidth(); ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				if( !std::isfinite( c.base.r ) || !std::isfinite( c.base.g ) || !std::isfinite( c.base.b ) ) ++bad;
			}
			std::lock_guard<std::mutex> lk( m );
			nonFinite += bad; ++finals;
			if( !IsFinal( &img ) ) finalsSeen.push_back( &img );
			Unhold( img );
		}
		bool IsFinal( const IRasterImage* p ) const
		{
			for( const IRasterImage* f : finalsSeen ) if( f == p ) return true;
			return false;
		}
		//! Deliveries [from, end) whose image is not a final (primary) image.
		unsigned long ComposedDeliveries( size_t from = 0 ) const
		{
			unsigned long n = 0;
			for( size_t i = from; i < rectImages.size(); ++i ) if( !IsFinal( rectImages[i] ) ) ++n;
			return n;
		}
		//! Composed images still referenced by someone other than us.
		unsigned long RetainedComposed() const
		{
			unsigned long n = 0;
			for( const IRasterImage* h : held ) if( !IsFinal( h ) && h->refcount() != 1 ) ++n;
			return n;
		}
		unsigned long DistinctComposed() const
		{
			unsigned long n = 0;
			for( const IRasterImage* h : held ) if( !IsFinal( h ) ) ++n;
			return n;
		}
	protected:
		~CaptureOutput() override { for( const IRasterImage* h : held ) h->release(); }
	};

	std::string RenderSceneText( const char* rasterizer, bool progressive )
	{
		const bool bidi = std::strncmp( rasterizer, "pathtracing", 11 ) != 0;
		return std::string( "RISE ASCII SCENE 7\n\n" ) +
			"film\n{\n\twidth 96\n\theight 64\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
			"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 10\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -20 -20 0\n\tptb 20 -20 0\n\tptc 20 20 0\n\tptd -20 20 0\n\tdoublesided FALSE\n}\n\n"
			"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_e\n\tpta -0.5 -0.5 0\n\tptb 0.5 -0.5 0\n\tptc 0.5 0.5 0\n\tptd -0.5 0.5 0\n\tdoublesided TRUE\n}\n\n"
			"standard_object\n{\n\tname e\n\tgeometry geo_e\n\tmaterial lum\n\tposition 0.3 0.2 1.5\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" +
			std::string( rasterizer ) + "\n{\n" + ( bidi ? "\tmax_eye_depth 4\n\tmax_light_depth 4\n" : "" ) + "\tsamples 4\n"
			"\tpixel_sampler uniform\n\toidn_denoise FALSE\n\tpixel_filter mitchell-netravali\n" +
			( progressive ? "\tprogressive_rendering TRUE\n\tprogressive_samples_per_pass 2\n" : "\tprogressive_rendering FALSE\n" ) +
			"}\n\n";
	}

	struct RenderCase { const char* ras; bool progressive; bool animation; };

	void TestRealRenders()
	{
		std::cout << "E: real multi-threaded renders (BDPT, VCM, PT) through RasterizeScene\n";
		const RenderCase cases[] = {
			{ "bdpt_pel_rasterizer", false, false },
			{ "bdpt_pel_rasterizer", true, false },
			{ "bdpt_spectral_rasterizer", false, false },
			{ "bdpt_spectral_rasterizer", true, false },
			// Animation per-block previews deliver the primary by design
			// (SPRasterizeSingleBlockOfAnimation composes nothing), so the
			// BDPT animation row is progressive: its pass-end previews are
			// composed through the scratch PreRenderSetup prepares per frame.
			{ "bdpt_pel_rasterizer", true, true },
			{ "vcm_pel_rasterizer", false, false },
			{ "vcm_pel_rasterizer", true, false },
			{ "vcm_pel_rasterizer", false, true },
			{ "pathtracing_pel_rasterizer", false, false },
		};
		for( const RenderCase& rc : cases ) {
			const std::string label = std::string( rc.ras ) + ( rc.animation ? ( rc.progressive ? " progressive animation frame" : " animation frame" ) : rc.progressive ? " progressive" : " single-pass" );
			const std::string path = RISE::TestSupport::ScratchBaseDir() + "/ict_" + rc.ras + ( rc.animation ? "_a" : rc.progressive ? "_p" : "_s" ) + ".RISEscene";
			{ std::ofstream ofs( path ); ofs << RenderSceneText( rc.ras, rc.progressive ); }
			IJobPriv* pJob = nullptr;
			const bool loaded = RISE_CreateJobPriv( &pJob ) && pJob && pJob->LoadAsciiSceneViaCst( path.c_str() );
			Check( loaded, label + ": scene loads" );
			if( !loaded ) { safe_release( pJob ); continue; }
			pJob->RemoveRasterizerOutputs();
			CaptureOutput* cap = new CaptureOutput();
			pJob->GetRasterizer()->AddRasterizerOutput( cap );
			if( Rasterizer* pRas = dynamic_cast<Rasterizer*>( pJob->GetRasterizer() ) ) pRas->ForTest_SetThreadCountOverride( 6 );
			// Two renders on the same rasterizer: the second must still
			// compose previews after the first released its scratch.
			for( int pass = 0; pass < 2; ++pass ) {
				const std::string pl = label + ( pass ? " (second render)" : "" );
				const size_t firstDelivery = cap->rects.size();
				const unsigned long finalsBefore = cap->finals;
				if( rc.animation ) pJob->RasterizeAnimation( 0, 0, 1, false, false );
				else pJob->Rasterize();
				// Pre-block (DrawToggles) + post-block deliveries share each
				// block rect; count area per distinct delivery.
				unsigned long long area = 0;
				for( size_t i = firstDelivery; i < cap->rects.size(); ++i ) {
					const Rect& r = cap->rects[i];
					area += 1ull * ( r.right - r.left + 1 ) * ( r.bottom - r.top + 1 );
				}
				const unsigned long composed = cap->ComposedDeliveries( firstDelivery );
				std::cout << "  " << pl << ": " << composed << " composed of " << ( cap->rects.size() - firstDelivery )
					<< " deliveries; composed images still referenced by the rasterizer: " << cap->RetainedComposed()
					<< " of " << cap->DistinctComposed() << "\n";
				Check( cap->finals > finalsBefore, pl + ": render completed with a final image" );
				Check( cap->nonFinite == 0, pl + ": every delivered pixel is finite" );
				Check( composed > 0, pl + ": composed previews were delivered (not the primary) -- " +
					std::to_string( composed ) + " of " + std::to_string( cap->rects.size() - firstDelivery ) + " deliveries" );
				if( !rc.progressive && !rc.animation ) {
					// One full-frame delivery after the initial clear, then each
					// block's rect twice (pre-block border + post-block result).
					Check( cap->rects.size() > firstDelivery && area == 3ull * 96 * 64,
						pl + ": deliveries = initial frame + each block twice, " +
						std::to_string( cap->rects.size() - firstDelivery ) + " callbacks, area " + std::to_string( area ) );
				}
				Check( cap->RetainedComposed() == 0, pl + ": the composition scratch was released when the render returned (" +
					std::to_string( cap->RetainedComposed() ) + " of " + std::to_string( cap->DistinctComposed() ) + " composed images still referenced)" );
			}
			safe_release( cap );
			safe_release( pJob );
			std::remove( path.c_str() );
		}
	}
}

int main( int argc, char** argv )
{
	const std::string mode = argc > 1 ? argv[1] : "";
	std::cout << "IntermediateCompositionTest\n";
	if( mode.empty() || mode == "--oracle" ) TestOracle();
	if( mode.empty() || mode == "--compose" ) TestConcurrentCompose();
	if( mode.empty() || mode == "--films" ) TestFilmWritersVsResolve();
	if( mode.empty() || mode == "--scratch" ) TestScratchPreparation();
	if( mode.empty() || mode == "--render" ) TestRealRenders();
	std::cout << "passed " << gPass << ", failed " << gFail << "\n";
	return gFail == 0 ? 0 : 1;
}
