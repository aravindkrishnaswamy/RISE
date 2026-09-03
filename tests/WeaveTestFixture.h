//////////////////////////////////////////////////////////////////////
//
//  WeaveTestFixture.h - Builds a `weave_material` from a shipped
//    WeavePresets.h row, for the three suites that need one
//    (SPFBSDFConsistencyTest, SPFPdfConsistencyTest,
//    LayeredWhiteFurnaceTest) plus WeaveMaterialChunkTest's
//    direct-construction checks.
//
//  WHY A SHARED FIXTURE RATHER THAN THREE LOCAL COPIES.  A
//  `WeaveMaterial` takes EIGHTEEN painter bindings.  Three hand-rolled
//  constructions would be three chances to bind a different number into
//  the same nominal preset, and the three suites would then be
//  measuring three different materials while all claiming to measure
//  "satin".  Building every one of them from the SHIPPED preset table
//  is also what makes the furnace's locked curve and the reciprocity
//  sweep statements about what authors actually get, rather than about
//  numbers only the tests know.
//
//  The fixture OWNS its painters (refcounted, released in the
//  destructor) and hands out a borrowed `WeaveMaterial*`.  Deliberately
//  non-copyable: a copy would double-release.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_TEST_FIXTURE_
#define WEAVE_TEST_FIXTURE_

#include <string>
#include <vector>

#include "../src/Library/Materials/WeaveMaterial.h"
#include "../src/Library/Materials/WeavePresets.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"

namespace RISE
{
	namespace WeaveTest
	{
		//! One `weave_material` built from a named preset, with the two
		//! fields a test may legitimately want to vary layered on top.
		class PresetWeave
		{
		public:
			//! @param presetName  a WeavePresets.h row name
			//! @param rotation    `weave_rotation` in radians (presets set none:
			//!                    an angle field is a spatial quantity a preset
			//!                    cannot see, exactly as `fabric_material`'s own
			//!                    preset table declines to bake one)
			//! @param coverage    when >= 0, overrides the DRAFT with a constant
			//!                    warp share -- for a test that wants a stated
			//!                    two-family mix independent of where in the
			//!                    weave cell the harness's fixed `ptCoord` lands
			//! @param thin        P2-B (docs/CLOTH_FABRIC_DESIGN.md 10).  DEFAULT
			//!                    FALSE, and DELIBERATELY NOT the preset table's
			//!                    own `transmission` default: every suite this
			//!                    fixture already served (SPFBSDFConsistencyTest,
			//!                    SPFPdfConsistencyTest, LayeredWhiteFurnaceTest,
			//!                    WeaveMaterialChunkTest) has locked numbers
			//!                    measured against the REFLECTION-ONLY P2-A
			//!                    material, for every preset including silk /
			//!                    satin / linen (whose `fabric` chunk default
			//!                    flipped to `transmission thin` in P2-B).  A
			//!                    fixture that silently followed the preset's new
			//!                    default would retune every one of those locked
			//!                    numbers out from under the suites that pinned
			//!                    them.  Pass `true` explicitly to build the
			//!                    `thin` material P2-B's own tests exercise.
			//! @param warpTransmit / weftTransmit  when `thin` and >= 0,
			//!                    OVERRIDE the preset's own per-family
			//!                    `transmit`; ignored (and the field is always 0)
			//!                    when `thin` is false.
			//! @param gapOverride  when >= 0, OVERRIDES the preset's own `gap`
			//!                    (P2-B: also the `thin` delta lobe's aperture).
			explicit PresetWeave( const char* presetName,
			                      const Scalar rotation = 0,
			                      const Scalar coverage = -1,
			                      const bool whiteDyes = false,
			                      const bool thin = false,
			                      const Scalar warpTransmit_ = -1,
			                      const Scalar weftTransmit_ = -1,
			                      const Scalar gapOverride = -1 )
			{
				using namespace RISE::Implementation;
				const WeavePreset& P = LookupWeavePreset( presetName );
				name = P.name;

				// `whiteDyes` forces both families to 1.0, which is what
				// LayeredWhiteFurnaceTest's white rows need: a furnace is a
				// statement about ENERGY, and a preset's own dark dye would
				// let a lost lobe hide behind the dye rather than showing
				// up as a deficit.
				const RISEPel kWhite( 1, 1, 1 );
				warpColor = mkColor( whiteDyes ? kWhite : P.warp.color );
				weftColor = mkColor( whiteDyes ? kWhite : P.weft.color );

				scale   = mkScalar( P.weaveScale );
				rot     = mkScalar( rotation );
				skew    = mkScalar( 0 );
				gap     = mkScalar( gapOverride >= 0 ? gapOverride : P.gap );

				warpIor = mkScalar( P.warp.ior );
				warpWid = mkScalar( P.warp.width );
				warpAzi = mkScalar( P.warp.azimuth );
				warpKd  = mkScalar( P.warp.kd );
				warpTil = mkScalar( P.warp.tilt );

				weftIor = mkScalar( P.weft.ior );
				weftWid = mkScalar( P.weft.width );
				weftAzi = mkScalar( P.weft.azimuth );
				weftKd  = mkScalar( P.weft.kd );
				weftTil = mkScalar( P.weft.tilt );

				const Scalar wTransmitVal = thin ? ( warpTransmit_ >= 0 ? warpTransmit_ : P.warp.transmit ) : Scalar( 0 );
				const Scalar fTransmitVal = thin ? ( weftTransmit_ >= 0 ? weftTransmit_ : P.weft.transmit ) : Scalar( 0 );
				warpTransmit = mkScalar( wTransmitVal );
				weftTransmit = mkScalar( fTransmitVal );

				cov = 0;
				WeavePatternKind pattern = P.weave;
				if( coverage >= 0 ) {
					pattern = eWeaveCustom;
					cov = mkScalar( coverage );
				}

				const WeaveTransmissionKind transKind = thin ? eWeaveTransmissionThin : eWeaveTransmissionNone;

				mat = new WeaveMaterial( pattern, transKind, *scale, *rot, *skew, cov, *gap,
				                         *warpColor, *warpIor, *warpWid, *warpAzi, *warpKd, *warpTil, *warpTransmit,
				                         *weftColor, *weftIor, *weftWid, *weftAzi, *weftKd, *weftTil, *weftTransmit );
				mat->addref();
			}

			~PresetWeave()
			{
				safe_release( mat );
				for( size_t i = 0; i < owned.size(); ++i ) {
					owned[i]->release();
				}
			}

			PresetWeave( const PresetWeave& ) = delete;
			PresetWeave& operator=( const PresetWeave& ) = delete;

			Implementation::WeaveMaterial* Material() const { return mat; }
			ISPF*  SPF()  const { return mat->GetSPF(); }
			IBSDF* BSDF() const { return mat->GetBSDF(); }
			const std::string& Name() const { return name; }

		private:
			Implementation::UniformScalarPainter* mkScalar( const Scalar v )
			{
				Implementation::UniformScalarPainter* p = new Implementation::UniformScalarPainter( v );
				p->addref();
				owned.push_back( p );
				return p;
			}
			Implementation::UniformColorPainter* mkColor( const RISEPel& c )
			{
				Implementation::UniformColorPainter* p = new Implementation::UniformColorPainter( c );
				p->addref();
				owned.push_back( p );
				return p;
			}

			std::string								name;
			std::vector<IReference*>				owned;
			Implementation::UniformColorPainter*	warpColor;
			Implementation::UniformColorPainter*	weftColor;
			Implementation::UniformScalarPainter*	scale;
			Implementation::UniformScalarPainter*	rot;
			Implementation::UniformScalarPainter*	skew;
			Implementation::UniformScalarPainter*	gap;
			Implementation::UniformScalarPainter*	cov;
			Implementation::UniformScalarPainter*	warpIor;
			Implementation::UniformScalarPainter*	warpWid;
			Implementation::UniformScalarPainter*	warpAzi;
			Implementation::UniformScalarPainter*	warpKd;
			Implementation::UniformScalarPainter*	warpTil;
			Implementation::UniformScalarPainter*	weftIor;
			Implementation::UniformScalarPainter*	weftWid;
			Implementation::UniformScalarPainter*	weftAzi;
			Implementation::UniformScalarPainter*	weftKd;
			Implementation::UniformScalarPainter*	weftTil;
			Implementation::UniformScalarPainter*	warpTransmit;
			Implementation::UniformScalarPainter*	weftTransmit;
			Implementation::WeaveMaterial*			mat;
		};
	}
}

#endif
