//////////////////////////////////////////////////////////////////////
//
//  PixelBasedSpectralIntegratingRasterizer.cpp - Implementation of the 
//    PixelBasedSpectralIntegratingRasterizer class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 19, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedSpectralIntegratingRasterizer.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "ProgressiveFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedSpectralIntegratingRasterizer::PixelBasedSpectralIntegratingRasterizer(
		IRayCaster* pCaster_,
		const Scalar lambda_begin_,
		const Scalar lambda_end_,
		const unsigned int num_wavelengths_,
		const unsigned int specsamp,
		const StabilityConfig& stabilityCfg,
		bool useZSobol_,
		bool useHWSS_
		) :
  PixelBasedRasterizerHelper( pCaster_ ),
	  lambda_begin( lambda_begin_ ),
	  lambda_end( lambda_end_ ),
	  lambda_diff( lambda_end_-lambda_begin_ ),
	  num_wavelengths( num_wavelengths_ ),
	  wavelength_steps( (lambda_diff)/Scalar(num_wavelengths) ),
	  nSpectralSamples( specsamp ),
	  bUseHWSS( useHWSS_ ),
	  stabilityConfig( stabilityCfg )
{
	useZSobol = useZSobol_;
	vecSpectralSamples.reserve( nSpectralSamples );
}

PixelBasedSpectralIntegratingRasterizer::~PixelBasedSpectralIntegratingRasterizer( )
{
}

void PixelBasedSpectralIntegratingRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	PixelBasedRasterizerHelper::PrepareRuntimeContext( rc );
	rc.pStabilityConfig = &stabilityConfig;
}

bool PixelBasedSpectralIntegratingRasterizer::TakeSingleSample(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	ColorXYZ& c
	) const
{
	if( bUseHWSS ) {
		return TakeSingleSampleHWSS( rc, rast, ray, c );
	}

	bool bHit = false;
	c = ColorXYZ(0,0,0,0);

	if( nSpectralSamples == 1 )
	{
		// Special case if only one spectral sample
		const Scalar nm = num_wavelengths < 10000 ? 
				(lambda_begin + int(rc.random.CanonicalRandom()*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);
		Scalar nmvalue = 0;
		bHit = pCaster->CastRayNM( rc, rast, ray, nmvalue, IRayCaster::RAY_STATE(), nm, 0, 0 );

		if( bHit && nmvalue > 0 ) {
			XYZPel thisNM( 0, 0, 0 );
			if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
				thisNM = thisNM * nmvalue;
				c = ColorXYZ( thisNM, 1.0 );
			}
		} else if( bHit ) {
			c.a = 1.0;
		}
	}
	else 
	{
		vecSpectralSamples.clear();

		// Take n samples
		for( unsigned int i=0; i<nSpectralSamples; i++ )
		{
			const Scalar nm = num_wavelengths < 10000 ? 
				(lambda_begin + int(rc.random.CanonicalRandom()*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

			// Now shoot a ray into the scene at that wavelength, and accrue the values
			SPECTRAL_SAMPLE	samp;
			samp.nm = nm;

			bool bThisHit = pCaster->CastRayNM( rc, rast, ray, samp.value, IRayCaster::RAY_STATE(), nm, 0, 0 );

			if( bThisHit ) {
				bHit = true;
				vecSpectralSamples.push_back( samp );
			}
		}

		// Now take out spectral samples and convert it to an XYZ value
		// For each wavelength, get the XYZ value, scale it by the value at that wavelength in the spectral
		// packet, and sum... then divide out the total by the wavelength range...
		XYZPel	sum( 0, 0, 0 );

		std::vector<SPECTRAL_SAMPLE>::const_iterator m, n;

		for( m=vecSpectralSamples.begin(), n=vecSpectralSamples.end(); m!=n; m++ )
		{
			const SPECTRAL_SAMPLE& samp = *m;

			if( samp.value > 0 ) {
				XYZPel thisNM( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( thisNM, samp.nm ) ) {
					thisNM = thisNM * samp.value;
					sum = sum + thisNM;
				}
			}
		}

		sum = sum * (1.0/Scalar(nSpectralSamples));

		if( bHit ) {
			c = ColorXYZ( sum, 1.0 );
		}
	}

	return bHit;
}

/// HWSS path: each spectral sample produces a bundle of
/// SampledWavelengths::N equidistant wavelengths.  The hero drives
/// all directional decisions in the shader op; companions share
/// the geometric path but carry independent spectral throughput.
/// XYZ conversion uses the same normalization as the single-wavelength
/// path: (1/numActive) * sum_i XYZFromNM(lambda_i) * c_i.
bool PixelBasedSpectralIntegratingRasterizer::TakeSingleSampleHWSS(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	ColorXYZ& c
	) const
{
	bool bHit = false;
	c = ColorXYZ(0,0,0,0);

	XYZPel totalSum( 0, 0, 0 );
	unsigned int totalActive = 0;

	for( unsigned int s = 0; s < nSpectralSamples; s++ )
	{
		const Scalar u = rc.random.CanonicalRandom();
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );

		Scalar cHWSS[SampledWavelengths::N] = {0};
		IRayCaster::RAY_STATE rs;
		IORStack cameraIorStack( 1.0 );
		bool bThisHit = pCaster->CastRayHWSS( rc, rast, ray, cHWSS, rs, swl, 0, 0, cameraIorStack );

		if( bThisHit ) {
			bHit = true;
		}

		// Count all active (non-terminated) wavelengths in the
		// denominator, including those with zero contribution.
		// Only XYZFromNM failures are excluded.
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			if( !swl.terminated[i] ) {
				XYZPel thisNM( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( thisNM, swl.lambda[i] ) ) {
					totalSum = totalSum + thisNM * cHWSS[i];
					totalActive++;
				}
			}
		}
	}

	if( totalActive > 0 ) {
		totalSum = totalSum * (1.0 / Scalar(totalActive));
	}

	if( bHit ) {
		c = ColorXYZ( totalSum, 1.0 );
	}

	return bHit;
}

void PixelBasedSpectralIntegratingRasterizer::IntegratePixel(
		const RuntimeContext& rc,
		const unsigned int x,
		const unsigned int y, 
		const unsigned int height,
		const IScene& pScene,
		RISEColor& cret,
		const bool temporal_samples,
		const Scalar temporal_start,
		const Scalar temporal_exposure
	) const
{
	RasterizerState rast = {x,y};

	// Derive a per-pixel seed for Owen scrambling.
	// ZSobol (blue-noise): seed from Morton index.
	uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	if( pSampling && pPixelFilter && rc.UsesPixelSampling() )
	{
		ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			if( px.converged ) {
				if( px.alphaSum > 0 ) {
					cret = RISEColor( px.colorSum * (1.0/px.alphaSum), px.alphaSum / px.weightSum );
				}
				return;
			}
		}

		const unsigned int batchSize = pSampling->GetNumSamples();
		const unsigned int maxSamples = batchSize;
		const unsigned int zSobolSPP = rc.totalProgressiveSPP > 0 ? rc.totalProgressiveSPP : maxSamples;

		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			const uint32_t mi = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2( zSobolSPP ) );
			if( l2 < 32 &&
				(uint64_t(mi) << l2) < (uint64_t(1) << 32) )
			{
				mortonIndex = mi;
				log2SPP = l2;
				pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
			} else {
				pixelSeed = SobolSequence::HashCombine(
					static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			}
		}

		RISEPel colAccrued( 0, 0, 0 );
		ColorXYZ colAccruedXYZ( 0, 0, 0, 0 );
		Scalar weights = 0;
		Scalar alphas = 0;
		Scalar wMean = 0;
		Scalar wM2 = 0;
		unsigned int wN = 0;

		uint32_t sampleIndex = 0;
		bool converged = false;

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			colAccrued = px.colorSum;
			weights = px.weightSum;
			alphas = px.alphaSum;
			wMean = px.wMean;
			wM2 = px.wM2;
			wN = px.wN;
			sampleIndex = px.sampleIndex;
		}

		const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
			? rc.totalProgressiveSPP
			: maxSamples;
		uint32_t passEndIndex = targetSamples;
		if( pProgFilm ) {
			const uint64_t desiredEnd = static_cast<uint64_t>( sampleIndex ) + static_cast<uint64_t>( batchSize );
			passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
		}

		while( sampleIndex < passEndIndex && !converged )
		{
			ISampling2D::SamplesList2D	samples;
			pSampling->GenerateSamplePoints( rc.random, samples );

			ISampling2D::SamplesList2D::const_iterator		m, n;
			for( m=samples.begin(), n=samples.end(); m!=n && sampleIndex<passEndIndex; m++, sampleIndex++ )
			{
				ColorXYZ	c;
				Point2		ptOnScreen;

				const bool filmMode = (pFilteredFilm != 0);
				Scalar weight;
				if( filmMode ) {
					ptOnScreen = Point2(
						static_cast<Scalar>(x) + (*m).x - 0.5,
						static_cast<Scalar>(height-y) + (*m).y - 0.5 );
					weight = 1.0;
					weights += 1.0;
				} else {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
					weights += weight;
				}

				if( temporal_samples ) {
					pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}

				// Install a Sobol sampler for this pixel sample so that
				// shader ops can use low-discrepancy sampling across the
				// full path recursion (including all spectral samples).
				SobolSampler stdSampler( sampleIndex, pixelSeed );
				ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, pixelSeed );
				rc.pSampler = useZSobol
					? static_cast<ISampler*>(&zSampler)
					: static_cast<ISampler*>(&stdSampler);

				Ray ray;
				if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
					TakeSingleSample( rc, rast, ray, c );
					const RISEPel cpel( c.base );
					if( filmMode ) {
						pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, c.base, *pPixelFilter );
						colAccruedXYZ = colAccruedXYZ + c;
						colAccrued = colAccrued + cpel;
						alphas += c.a;
					} else {
						colAccruedXYZ = colAccruedXYZ + c*weight;
						colAccrued = colAccrued + cpel*weight;
						alphas += c.a * weight;
					}

					if( pProgFilm ) {
						const Scalar lum = ColorMath::MaxValue( cpel );
						wN++;
						const Scalar delta = lum - wMean;
						wMean += delta / Scalar(wN);
						const Scalar delta2 = lum - wMean;
						wM2 += delta * delta2;
					}
				} else if( pProgFilm ) {
					wN++;
					const Scalar delta = -wMean;
					wMean += delta / Scalar(wN);
					const Scalar delta2 = -wMean;
					wM2 += delta * delta2;
				}

				rc.pSampler = 0;
			}

			if( pProgFilm && wN >= 32 )
			{
				const Scalar variance = wM2 / Scalar(wN - 1);
				const Scalar stdError = sqrt( variance / Scalar(wN) );
				const Scalar meanAbs = fabs( wMean );

				if( meanAbs > NEARZERO ) {
					const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
					if( stdError / meanAbs < 0.01 * confidence ) {
						converged = true;
					}
				} else if( wM2 < NEARZERO && wN >= 64 ) {
					converged = true;
				}
			}
		}

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			px.colorSum = colAccrued;
			px.weightSum = weights;
			px.alphaSum = alphas;
			px.wMean = wMean;
			px.wM2 = wM2;
			px.wN = wN;
			px.sampleIndex = sampleIndex;
			px.converged = converged;
		}

		if( pProgFilm && alphas > 0 ) {
			cret = RISEColor( colAccrued * (1.0/alphas), alphas/weights );
		} else if( !pProgFilm && colAccruedXYZ.a > 0 ) {
			cret = RISEColor( colAccruedXYZ.base * (1.0/colAccruedXYZ.a), colAccruedXYZ.a/weights );
		}
	}
	else
	{
		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			mortonIndex = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
		}
		SobolSampler stdSampler( 0, pixelSeed );
		ZSobolSampler zSampler( 0, mortonIndex, 0, pixelSeed );
		rc.pSampler = useZSobol
			? static_cast<ISampler*>(&zSampler)
			: static_cast<ISampler*>(&stdSampler);

		Ray ray;
		if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
			ColorXYZ	c;
			TakeSingleSample( rc, rast, ray, c );
			cret = RISEColor( c.base, c.a );
		}

		rc.pSampler = 0;
	}
}
