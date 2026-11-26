//////////////////////////////////////////////////////////////////////
//
//  mix_and_shadow_closed_form_proto.cpp - Standalone closed-form
//    contract tests and working prototypes for the Review Round 3
//    (2026-11-16) P1 counterexamples on `mix_material`,
//    `passthrough_material` (HWSS), `scaled_material`, and shadow
//    visibility.
//
//  Lives under tools/prototypes/ (NOT tests/) because these contracts
//  prototype Phase 3/4 runtime types (`mix_material`, `scaled_material`,
//  `passthrough_material`, shadow-visibility walk) that are not yet
//  implemented in src/Library/.
//
//  Build & run from repo root:
//    c++ -std=c++17 -O2 -Wall -Wextra -Werror \
//        tools/prototypes/mix_and_shadow_closed_form_proto.cpp \
//        -o /tmp/mix_and_shadow_proto && /tmp/mix_and_shadow_proto
//
//  Contracts proven here (every contract paired with a negative proof
//  showing the pre-Round-3 bug fails the same assertion):
//    1. Bounded mix response-coefficient semantics (MIX §I.1):
//       `0.25 Diffuse + 0` reflects 0.25 (NOT normalized to 1.0);
//       `S > 1` clamps by dividing weights by S.
//    2. Round 3 §3 independent per-hit sampler draw vs coincident faces:
//       two coincident 50%-opaque sheets give 0.25 joint rate under
//       independent draws (vs 0.50 under the withdrawn patchwork hash).
//    3. Round 3 P1-1 VCM connectible-leaf thinning:
//       `0.25 Diffuse + 0.75 none`: absorber leaf MUST set
//       `isConnectible = false` BEFORE early exit so merge deposit weight
//       `1 / (a * q)` converges to 0.25, NOT 4x over-bright 1.0.
//    4. Round 3 P1-2 alpha-coverage / pass-through registration:
//       bounded mix with pass-through reports `HasAlphaCoverage() == true`
//       and `HitCoverage() == 1 - w_clear` through one shared helper.
//    5. Round 3 P1-5(a,b,d) gain mix & HWSS `passthrough_material`:
//       - `S` stamped on the hit at resolution time (stays exact even
//         when a leaf modifier shifts the shading point P -> P_shifted
//         where S(P_shifted) != S(P_orig))
//       - companion `-1` sentinel preserved (never multiplied by S!)
//       - narrowband tint with zero hero wavelength does NOT zero
//         companion wavelengths in HWSS passthrough!
//    6. Round 3 P1-7(a,b,c,d) & §4 shadow visibility walk:
//       - (7a) emitter with `casts_shadows=FALSE` credited BEFORE skip
//       - (7b) ordinary alpha surface in front of shadow-dependent surface
//              tested ONCE (BSDF and light-sampling estimators agree)
//       - (7c) shadow proxy invisible to scatter rays still blocks shadow
//              rays (no light leak)
//       - (7d) genuine 3D geometry + Monte Carlo integration of a point
//              light above a translucent diffuse-transmitting sheet with
//              `casts_shadows=FALSE` over a diffuse floor: preserves BOTH
//              direct irradiance and diffusely-transmitted irradiance.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <random>
#include <vector>

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	int g_pass = 0;
	int g_fail = 0;

	void Check( bool cond, const char* msg )
	{
		if( cond ) {
			++g_pass;
		} else {
			++g_fail;
			std::cerr << "FAIL: " << msg << "\n";
		}
	}

	bool Near( double a, double b, double tol )
	{
		return std::fabs( a - b ) <= tol;
	}

	// ================================================================
	// Prototype 1: mix_material + scaled_material + passthrough_material
	// ================================================================

	enum LeafKind { eLeafDiffuse = 0, eLeafAbsorber = 1, eLeafPassthrough = 2 };

	struct ProtoLeaf
	{
		LeafKind kind;
		double   weight;
		double   albedo;
		double   spectralTint[4];
		bool     isConnectible() const { return kind == eLeafDiffuse; }
	};

	struct ProtoMixMaterial
	{
		std::vector<ProtoLeaf> leaves;
		double                 wPassthrough = 0.0;
		bool                   allowEnergyGain = false;

		// Round 3 P1-2: single shared gate for alpha registration.
		bool HasAlphaCoverage() const
		{
			return !allowEnergyGain && wPassthrough > 0.0;
		}

		// Compute effective weights (clamping negative/NaN to 0) and sum S.
		// Optional spatially varying scale `weightScale` simulates a textured
		// weight painter evaluated at a shading point P.
		void EffectiveWeights(
			std::vector<double>& wOut,
			double& wClearOut,
			double& SOut,
			double weightScale = 1.0 ) const
		{
			wOut.resize( leaves.size() );
			double S = 0.0;
			for( size_t i = 0; i < leaves.size(); ++i ) {
				const double raw = leaves[i].weight * weightScale;
				const double w = ( std::isfinite( raw ) && raw > 0.0 ) ? raw : 0.0;
				wOut[i] = w;
				S += w;
			}
			const double rawClear = wPassthrough * weightScale;
			wClearOut = ( std::isfinite( rawClear ) && rawClear > 0.0 ) ? rawClear : 0.0;
			S += wClearOut;
			if( !allowEnergyGain && S > 1.0 ) {
				const double inv = 1.0 / S;
				for( double& w : wOut ) w *= inv;
				wClearOut *= inv;
				S = 1.0;
			}
			SOut = S;
		}

		double HitCoverage() const
		{
			if( allowEnergyGain ) return 1.0;
			std::vector<double> w;
			double wClear = 0.0, S = 0.0;
			EffectiveWeights( w, wClear, S );
			return 1.0 - wClear;
		}

		struct ResolvedHit
		{
			int    leafIdx;        // -1 = absorbed (shortfall / none), -2 = delta passthrough
			double coverage;       // acceptance probability `a`
			double connectibleProb;// `q` = P(connectible | accepted)
			double stampedGainS;   // Round 3 P1-5(a): stamped at resolution time
			bool   isConnectible;  // Round 3 P1-1: set BEFORE any early exit!
		};

		ResolvedHit Resolve( double uAlpha, double uLeaf, double weightScale = 1.0 ) const
		{
			std::vector<double> w;
			double wClear = 0.0, S = 0.0;
			EffectiveWeights( w, wClear, S, weightScale );
			ResolvedHit rh{ -1, 1.0, 0.0, 1.0, false };

			if( !allowEnergyGain ) {
				const double a = 1.0 - wClear;
				rh.coverage = a;
				if( a <= 0.0 || uAlpha >= a ) {
					rh.leafIdx = -2;
					rh.isConnectible = false;
					return rh;
				}
				double qSum = 0.0;
				for( size_t i = 0; i < leaves.size(); ++i ) {
					if( leaves[i].isConnectible() ) qSum += w[i];
				}
				rh.connectibleProb = qSum / a;

				double xi = uLeaf * a;
				double acc = 0.0;
				for( size_t i = 0; i < leaves.size(); ++i ) {
					acc += w[i];
					if( xi < acc ) {
						rh.leafIdx = (int)i;
						rh.isConnectible = leaves[i].isConnectible();
						return rh;
					}
				}
				rh.leafIdx = -1;
				rh.isConnectible = false;
				return rh;
			}

			rh.coverage = 1.0;
			rh.stampedGainS = S;
			if( S <= 0.0 ) {
				rh.leafIdx = -1;
				rh.isConnectible = false;
				return rh;
			}
			double qSum = 0.0;
			for( size_t i = 0; i < leaves.size(); ++i ) {
				if( leaves[i].isConnectible() ) qSum += w[i];
			}
			rh.connectibleProb = qSum / S;

			double xi = uLeaf * S;
			double acc = 0.0;
			for( size_t i = 0; i < leaves.size(); ++i ) {
				acc += w[i];
				if( xi < acc ) {
					rh.leafIdx = (int)i;
					rh.isConnectible = leaves[i].isConnectible();
					return rh;
				}
			}
			rh.leafIdx = -2;
			rh.isConnectible = false;
			return rh;
		}
	};

	double ScaledCompanionEval( double childKrayNM, double stampedS )
	{
		if( childKrayNM < 0.0 ) return -1.0;
		return childKrayNM * stampedS;
	}

	double BuggyScaledCompanionEval( double childKrayNM, double stampedS )
	{
		return childKrayNM * stampedS;
	}

	void EvaluateHWSSPassthrough(
		const double tint[4],
		const double incomingThroughput[4],
		double outThroughput[4] )
	{
		for( int lane = 0; lane < 4; ++lane ) {
			outThroughput[lane] = incomingThroughput[lane] * tint[lane];
		}
	}

	void BuggyHeroRatioHWSSPassthrough(
		const double tint[4],
		const double incomingThroughput[4],
		double outThroughput[4] )
	{
		// Pre-Round-3 bug: when hero tint == 0, early-out kills all companion lanes.
		if( tint[0] <= 0.0 ) {
			for( int lane = 0; lane < 4; ++lane ) outThroughput[lane] = 0.0;
			return;
		}
		for( int lane = 0; lane < 4; ++lane ) {
			outThroughput[lane] = incomingThroughput[lane] * ( tint[lane] / tint[0] ) * tint[0];
		}
	}

	void TestBoundedMixAndVCMThinning()
	{
		std::mt19937_64 rng( 42 );
		std::uniform_real_distribution<double> U( 0.0, 1.0 );
		const int N = 200000;

		// 1. Bounded mix response-coefficient semantics: 0.25 Diffuse (albedo=1.0), shortfall 0.75 absorbs
		ProtoMixMaterial m25;
		m25.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.25, 1.0, {1,1,1,1} } );
		double reflSum = 0.0;
		double buggyNormalizedSum = 0.0;
		for( int i = 0; i < N; ++i ) {
			auto rh = m25.Resolve( U(rng), U(rng) );
			if( rh.leafIdx == 0 ) reflSum += m25.leaves[0].albedo;
			// Buggy (pre-Round-3): normalizing lone leaf weight 0.25 -> 1.0 always picks leaf 0
			buggyNormalizedSum += m25.leaves[0].albedo;
		}
		Check( Near( reflSum / N, 0.25, 0.005 ),
			"MIX §I.1: lone 0.25 Diffuse reflects 0.25 (NOT normalized to 1.0)" );
		Check( !Near( buggyNormalizedSum / N, 0.25, 0.005 ),
			"MIX §I.1 RED-PROOF: normalizing shortfall to 1.0 fails the 0.25 assertion" );

		// Over-sum bounded mix (0.6 + 0.9 = 1.5 -> clamped to 0.4 + 0.6)
		ProtoMixMaterial mClamp;
		mClamp.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.6, 1.0, {1,1,1,1} } );
		mClamp.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.9, 0.5, {0.5,0.5,0.5,0.5} } );
		double cSum = 0.0;
		for( int i = 0; i < N; ++i ) {
			auto rh = mClamp.Resolve( U(rng), U(rng) );
			if( rh.leafIdx >= 0 ) cSum += mClamp.leaves[rh.leafIdx].albedo;
		}
		Check( Near( cSum / N, 0.4 * 1.0 + 0.6 * 0.5, 0.005 ),
			"MIX §I.1: bounded mix with S > 1 divides weights by S (0.4*1 + 0.6*0.5 = 0.7)" );

		// 2. Round 3 P1-1: VCM merge on `0.25 Diffuse + 0.75 none`
		ProtoMixMaterial mVcm;
		mVcm.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.25, 1.0, {1,1,1,1} } );
		mVcm.leaves.push_back( ProtoLeaf{ eLeafAbsorber, 0.75, 0.0, {0,0,0,0} } );
		double fixedMergeEst = 0.0;
		double buggyMergeEst = 0.0;
		for( int i = 0; i < N; ++i ) {
			auto rh = mVcm.Resolve( U(rng), U(rng) );
			const double denom = rh.coverage * rh.connectibleProb;
			if( rh.isConnectible ) {
				fixedMergeEst += ( 1.0 / denom ) * 0.25;
			}
			// Buggy (pre-Round-3): absorber left isConnectible=true -> 4x over-bright
			buggyMergeEst += ( 1.0 / denom ) * 0.25;
		}
		Check( Near( fixedMergeEst / N, 0.25, 0.005 ),
			"Round 3 P1-1 FIXED: VCM merge on (0.25 Diffuse + 0.75 none) converges to exact 0.25" );
		Check( Near( buggyMergeEst / N, 1.0, 0.005 ),
			"Round 3 P1-1 RED-PROOF: buggy default isConnectible=true is 4x over-bright (1.0)" );

		// 3. Round 3 P1-2: bounded mix with pass-through registers as alpha material
		ProtoMixMaterial mPass;
		mPass.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.6, 0.8, {0.8,0.8,0.8,0.8} } );
		mPass.wPassthrough = 0.4;
		Check( mPass.HasAlphaCoverage(), "Round 3 P1-2: mix with pass-through registers HasAlphaCoverage() == true" );
		Check( Near( mPass.HitCoverage(), 0.6, 1e-12 ), "Round 3 P1-2: HitCoverage() == 1 - w_clear == 0.6" );
		// Negative proof: gain mix must NOT register as alpha-coverage (coverage stays 1.0)
		ProtoMixMaterial mGainPass = mPass;
		mGainPass.allowEnergyGain = true;
		Check( !mGainPass.HasAlphaCoverage() && Near( mGainPass.HitCoverage(), 1.0, 1e-12 ),
			"Round 3 P1-2 RED-PROOF: gain mix does not register alpha coverage" );

		// 4. Round 3 §3: independent per-hit sampler draws on two coincident 50% sheets
		ProtoMixMaterial sheet;
		sheet.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 0.5, 1.0, {1,1,1,1} } );
		sheet.leaves.push_back( ProtoLeaf{ eLeafAbsorber, 0.5, 0.0, {0,0,0,0} } );
		int bothDiffuseIndep = 0;
		int bothDiffusePatchwork = 0;
		for( int i = 0; i < N; ++i ) {
			const double u1 = U(rng);
			const double u2 = U(rng);
			if( sheet.Resolve( 0.0, u1 ).leafIdx == 0 && sheet.Resolve( 0.0, u2 ).leafIdx == 0 ) {
				++bothDiffuseIndep;
			}
			if( sheet.Resolve( 0.0, u1 ).leafIdx == 0 && sheet.Resolve( 0.0, u1 ).leafIdx == 0 ) {
				++bothDiffusePatchwork;
			}
		}
		Check( Near( double(bothDiffuseIndep) / N, 0.25, 0.005 ),
			"Round 3 §3: independent per-hit draws give 0.25 joint rate on coincident 50% sheets" );
		Check( Near( double(bothDiffusePatchwork) / N, 0.50, 0.005 ),
			"Round 3 §3 RED-PROOF: withdrawn patchwork hash correlates coincident sheets to 0.50" );
	}

	void TestGainMixAndHWSSPassthrough()
	{
		// Round 3 P1-5(a): gain mix Add(Diffuse(0.8), Diffuse(0.6)) -> weights (1.0, 1.0) so S = 2.0
		// stamped at resolution, giving expected reflectance (0.5*0.8 + 0.5*0.6) * S = 0.7 * 2.0 = 1.4.
		// Also simulate a leaf modifier (e.g. ReliefModifier) shifting the shading point after
		// resolution so re-evaluating S at the shifted point yields S_shifted = 3.0 != S_orig = 2.0.
		ProtoMixMaterial mGain;
		mGain.allowEnergyGain = true;
		mGain.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 1.0, 0.8, {0.8,0.8,0.8,0.8} } );
		mGain.leaves.push_back( ProtoLeaf{ eLeafDiffuse, 1.0, 0.6, {0.6,0.6,0.6,0.6} } );
		std::mt19937_64 rng( 99 );
		std::uniform_real_distribution<double> U( 0.0, 1.0 );
		const int N = 200000;
		double stampedSum = 0.0;
		double reevalShiftedSum = 0.0;
		bool allStampedS = true;
		for( int i = 0; i < N; ++i ) {
			auto rh = mGain.Resolve( U(rng), U(rng), /*weightScale at P_orig=*/1.0 );
			if( !Near( rh.stampedGainS, 2.0, 1e-12 ) ) allStampedS = false;
			if( rh.leafIdx >= 0 ) {
				stampedSum += mGain.leaves[rh.leafIdx].albedo * rh.stampedGainS;
				// Buggy (pre-Round-3): re-evaluating S after modifier shifted P -> weightScale 1.5 -> S_shifted=3.0
				std::vector<double> wShift;
				double wClearShift = 0.0, sShifted = 0.0;
				mGain.EffectiveWeights( wShift, wClearShift, sShifted, /*weightScale at P_shifted=*/1.5 );
				reevalShiftedSum += mGain.leaves[rh.leafIdx].albedo * sShifted;
			}
		}
		Check( allStampedS, "gain mix stamps S = 2.0 at resolution on every hit" );
		Check( Near( stampedSum / N, 0.8 + 0.6, 0.005 ),
			"Round 3 P1-5(a): Add(Diffuse(0.8), Diffuse(0.6)) converges to 1.4 using stamped S=2.0" );
		Check( Near( reevalShiftedSum / N, 2.1, 0.01 ),
			"Round 3 P1-5(a) RED-PROOF: re-evaluating S after modifier-shifted P biases result to 2.1" );

		// Round 3 P1-5(b): companion -1 sentinel preserved under scaling
		Check( ScaledCompanionEval( -1.0, 2.0 ) == -1.0,
			"Round 3 P1-5(b): scaled_material preserves -1 unsupported companion sentinel" );
		Check( BuggyScaledCompanionEval( -1.0, 2.0 ) == -2.0,
			"Round 3 P1-5(b) RED-PROOF: naive multiply corrupts -1 sentinel to -2" );
		Check( Near( ScaledCompanionEval( 0.4, 2.0 ), 0.8, 1e-12 ),
			"Round 3 P1-5(b): scaled_material scales valid companion weight by stamped S" );

		// Round 3 P1-5(d): HWSS narrowband passthrough with zero hero lane preserves companions!
		const double narrowTint[4] = { 0.0, 0.9, 0.5, 0.25 };
		const double inThru[4] = { 1.0, 1.0, 1.0, 1.0 };
		double outThru[4] = {};
		EvaluateHWSSPassthrough( narrowTint, inThru, outThru );
		Check( outThru[0] == 0.0 && Near( outThru[1], 0.9, 1e-12 ) && Near( outThru[2], 0.5, 1e-12 ) && Near( outThru[3], 0.25, 1e-12 ),
			"Round 3 P1-5(d): HWSS passthrough with zero hero wavelength preserves companion lanes" );
		double buggyThru[4] = {};
		BuggyHeroRatioHWSSPassthrough( narrowTint, inThru, buggyThru );
		Check( buggyThru[1] == 0.0 && buggyThru[2] == 0.0 && buggyThru[3] == 0.0,
			"Round 3 P1-5(d) RED-PROOF: hero-gated passthrough zeroes live companion wavelengths" );
	}

	// ================================================================
	// Prototype 2: Shadow visibility rules (Round 3 P1-7 & §4)
	// ================================================================

	struct SurfaceHit
	{
		double t;
		bool   isEmitter;
		double emission;
		bool   castsShadows;      // false = transparent to shadow rays
		bool   visibleToScatter;  // false = shadow proxy (absent to scatter/camera rays)
		double alpha;             // ordinary alpha coverage in [0, 1]
	};

	double ShadowTransmittance( const std::vector<SurfaceHit>& surfaces )
	{
		double T = 1.0;
		for( const auto& s : surfaces ) {
			if( !s.castsShadows ) continue;
			T *= ( 1.0 - s.alpha );
			if( T <= 0.0 ) return 0.0;
		}
		return T;
	}

	double BuggyShadowTransmittanceIgnoresProxy( const std::vector<SurfaceHit>& surfaces )
	{
		double T = 1.0;
		for( const auto& s : surfaces ) {
			if( !s.visibleToScatter ) continue; // P1-7(c) bug: skips shadow proxy!
			if( !s.castsShadows ) continue;
			T *= ( 1.0 - s.alpha );
			if( T <= 0.0 ) return 0.0;
		}
		return T;
	}

	double BSDFWalkEmission( const std::vector<SurfaceHit>& surfaces )
	{
		double L = 0.0;
		double T = 1.0;
		for( const auto& s : surfaces ) {
			if( !s.visibleToScatter ) continue;
			if( s.isEmitter ) {
				L += T * s.alpha * s.emission;
			}
			T *= ( 1.0 - s.alpha );
			if( T <= 0.0 ) break;
		}
		return L;
	}

	double BuggyBSDFWalkSkipsNoShadowEmitter( const std::vector<SurfaceHit>& surfaces )
	{
		double L = 0.0;
		double T = 1.0;
		for( const auto& s : surfaces ) {
			if( !s.visibleToScatter ) continue;
			if( !s.castsShadows ) continue; // P1-7(a) bug: skips before crediting emitter!
			if( s.isEmitter ) L += T * s.alpha * s.emission;
			T *= ( 1.0 - s.alpha );
			if( T <= 0.0 ) break;
		}
		return L;
	}

	double BuggyBSDFWalkDoubleTestsAlpha( const std::vector<SurfaceHit>& surfaces )
	{
		double L = 0.0;
		double T = 1.0;
		for( const auto& s : surfaces ) {
			if( !s.visibleToScatter ) continue;
			if( s.isEmitter ) L += T * s.alpha * s.emission;
			// P1-7(b) bug: tests alpha twice (once in scatter, once in shadow check)
			T *= ( 1.0 - s.alpha ) * ( 1.0 - s.alpha );
			if( T <= 0.0 ) break;
		}
		return L;
	}

	void TestShadowVisibilityCounterexamples()
	{
		// (7a) Emitter with casts_shadows = FALSE must still contribute its BSDF emission share
		{
			std::vector<SurfaceHit> hits = {
				SurfaceHit{ 1.0, true, 5.0, /*castsShadows=*/false, /*visibleToScatter=*/true, 1.0 }
			};
			Check( Near( BSDFWalkEmission( hits ), 5.0, 1e-12 ),
				"Round 3 P1-7(a): emitter with casts_shadows=FALSE is credited before skip" );
			Check( Near( BuggyBSDFWalkSkipsNoShadowEmitter( hits ), 0.0, 1e-12 ),
				"Round 3 P1-7(a) RED-PROOF: skipping casts_shadows=FALSE before credit drops emission to 0" );
			Check( Near( ShadowTransmittance( hits ), 1.0, 1e-12 ),
				"Round 3 P1-7(a): casts_shadows=FALSE emitter does not block shadow rays behind it" );
		}

		// (7b) Ordinary alpha=0.4 surface in front of shadow-dependent surface:
		//      attenuates by (1 - 0.4) = 0.6 ONCE (NOT 0.36!).
		{
			std::vector<SurfaceHit> hits = {
				SurfaceHit{ 1.0, false, 0.0, true, true, 0.4 },
				SurfaceHit{ 2.0, true, 10.0, false, true, 1.0 },
			};
			Check( Near( ShadowTransmittance( hits ), 0.6, 1e-12 ),
				"Round 3 P1-7(b): ordinary alpha attenuates shadow ray ONCE (0.6)" );
			Check( Near( BSDFWalkEmission( hits ), 6.0, 1e-12 ),
				"Round 3 P1-7(b): BSDF walk tests ordinary alpha ONCE (0.6 * 10 = 6.0)" );
			Check( Near( BuggyBSDFWalkDoubleTestsAlpha( hits ), 3.6, 1e-12 ),
				"Round 3 P1-7(b) RED-PROOF: double-testing alpha biases emission to 3.6" );
		}

		// (7c) Shadow proxy (visibleToScatter=FALSE, castsShadows=TRUE, alpha=1.0)
		//      between shading point and light: blocks shadow rays completely (no light leak!)
		//      even though scatter rays ignore it (BSDFWalkEmission passes straight through to emitter).
		{
			std::vector<SurfaceHit> occluders = {
				SurfaceHit{ 1.0, false, 0.0, /*castsShadows=*/true, /*visibleToScatter=*/false, 1.0 },
			};
			Check( Near( ShadowTransmittance( occluders ), 0.0, 1e-12 ),
				"Round 3 P1-7(c): shadow proxy absent to scatter rays still blocks shadow rays (no light leak)" );
			Check( Near( BuggyShadowTransmittanceIgnoresProxy( occluders ), 1.0, 1e-12 ),
				"Round 3 P1-7(c) RED-PROOF: ignoring shadow proxy leaks 100% of direct light" );
			std::vector<SurfaceHit> scatterHits = {
				SurfaceHit{ 1.0, false, 0.0, /*castsShadows=*/true, /*visibleToScatter=*/false, 1.0 },
				SurfaceHit{ 2.0, true, 10.0, true, true, 1.0 },
			};
			Check( Near( BSDFWalkEmission( scatterHits ), 10.0, 1e-12 ),
				"Round 3 P1-7(c): scatter ray ignores shadow proxy and sees emitter behind it" );
		}

		// (7d) Genuine 3D geometry & Monte Carlo integration for Round 3 §4:
		//      Point light of intensity I = 4.0 at L = (0, 0, 2) above a circular translucent
		//      disk of radius R = 1.0 at z = 1 (diffuse transmittance T = 0.8, casts_shadows=FALSE)
		//      over a diffuse floor shading point P = (0, 0, 0), N = (0, 0, 1).
		//      1. Direct irradiance at P through casts_shadows=FALSE disk:
		//         E_direct = ShadowTransmittance(disk) * I / 2^2 = 1.0.
		//      2. Indirect 1-bounce diffuse transmission through the disk at z = 1:
		//         Exact closed form:
		//           E_trans_exact = ∫_0^R 2*pi*r * (I * T / pi) * (r^2 + 1)^(-7/2) dr
		//                         = (2/5) * I * T * (1 - (1 + R^2)^(-5/2))
		//         Verified against cosine-weighted hemisphere Monte Carlo ray tracing from P.
		{
			const double I = 4.0;
			const double T = 0.8;
			const double R = 1.0;

			std::vector<SurfaceHit> shadowHits = {
				SurfaceHit{ 1.0, false, 0.0, /*castsShadows=*/false, /*visibleToScatter=*/true, 1.0 }
			};
			const double E_direct = ShadowTransmittance( shadowHits ) * I / ( 2.0 * 2.0 );
			Check( Near( E_direct, 1.0, 1e-12 ),
				"Round 3 P1-7(d): direct point-light irradiance passes through casts_shadows=FALSE disk" );

			const double E_trans_exact = 0.4 * I * T * ( 1.0 - std::pow( 1.0 + R * R, -2.5 ) );

			// Cosine-weighted hemisphere ray trace from P=(0,0,0) onto the disk at z=1
			std::mt19937_64 rng( 777 );
			std::uniform_real_distribution<double> U( 0.0, 1.0 );
			const int NSamples = 300000;
			double E_trans_mc = 0.0;
			for( int i = 0; i < NSamples; ++i ) {
				const double u1 = U(rng);
				const double u2 = U(rng);
				const double r = std::sqrt( u1 );
				const double phi = 2.0 * kPi * u2;
				const double wx = r * std::cos( phi );
				const double wy = r * std::sin( phi );
				const double wz = std::sqrt( std::max( 0.0, 1.0 - u1 ) );
				if( wz <= 1e-12 ) continue;
				const double tHit = 1.0 / wz;
				const double hx = wx * tHit;
				const double hy = wy * tHit;
				if( hx * hx + hy * hy <= R * R ) {
					const double d2 = hx * hx + hy * hy + 1.0;
					const double cosIn = 1.0 / std::sqrt( d2 );
					const double L_trans = ( I * cosIn / d2 ) * ( T / kPi );
					E_trans_mc += kPi * L_trans;
				}
			}
			E_trans_mc /= NSamples;

			Check( Near( E_trans_mc, E_trans_exact, 0.006 ),
				"Round 3 P1-7(d): cosine-hemisphere MC transmitted irradiance matches closed-form disk integral" );

			const double E_total = E_direct + E_trans_mc;
			Check( Near( E_total, 1.0 + E_trans_exact, 0.006 ),
				"Round 3 P1-7(d): total floor irradiance preserves BOTH direct and diffusely-transmitted light" );

			// Negative proof: treating translucent disk as shadow-blocking drops E_direct to 0
			std::vector<SurfaceHit> opaqueShadowHits = {
				SurfaceHit{ 1.0, false, 0.0, /*castsShadows=*/true, /*visibleToScatter=*/true, 1.0 }
			};
			const double E_buggy_blocked = ShadowTransmittance( opaqueShadowHits ) * I / 4.0 + E_trans_mc;
			Check( !Near( E_buggy_blocked, 1.0 + E_trans_exact, 0.05 ),
				"Round 3 P1-7(d) RED-PROOF: ignoring casts_shadows=FALSE loses direct irradiance" );
		}
	}
}

int main()
{
	TestBoundedMixAndVCMThinning();
	TestGainMixAndHWSSPassthrough();
	TestShadowVisibilityCounterexamples();

	std::cout << "mix_and_shadow_closed_form_proto: " << g_pass << " passed, " << g_fail << " failed.\n";
	return g_fail == 0 ? 0 : 1;
}
