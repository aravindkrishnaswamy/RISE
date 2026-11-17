//////////////////////////////////////////////////////////////////////
//
//  FabricPresets.h - The `fabric` enum's preset table
//    (docs/CLOTH_FABRIC_DESIGN.md 9.3 / 9.6).
//
//  ONE TABLE, TWO CONSUMERS.  The preset is read in two places that
//  must never disagree:
//
//    1. `FabricMaterialAsciiChunkParser::Finalize` seeds the chunk's
//       OWN slots from it (`sheen_roughness`, and -- indirectly, via
//       `Job::AddFabricMaterial` -- the sheen colour).
//    2. `Job::AddFabricMaterial` reads the RECOMMENDED SUBSTRATE half
//       to decide whether to emit the warn-level mismatch diagnostic,
//       and the future `make_fabric` verb (9.7) reads the same half to
//       MINT that substrate.
//
//  Keeping both halves in one struct is what makes 9.3's split table
//  ("what the enum sets" vs "what only a verb can supply") a single
//  source of truth rather than two lists that drift.
//
//  WHY THE SPLIT EXISTS AT ALL (9.3).  `fabric_material` holds a
//  REFERENCE to an already-constructed base material; neither the
//  parser nor `Job` can retype or re-parameterise it.  So `fabric
//  satin` over a Lambertian base yields chalk with a faint sheen --
//  the tight, directional, anisotropic highlight that IS satin lives
//  in a substrate the chunk cannot reach.  The enum therefore seeds
//  only the left-hand columns; the right-hand ones are RECOMMENDATIONS
//  that drive a diagnostic here and a mint in the verb.
//
//  THE NUMBERS ARE INDICATIVE, NOT MEASURED.  9.3 says so explicitly:
//  "starting points to be tuned against reference photography, not
//  measurements".  Treat a change to them as an appearance change,
//  not a bug fix.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FABRIC_PRESETS_
#define FABRIC_PRESETS_

#include "../Utilities/Color/Color.h"
#include <cstring>

namespace RISE
{
	namespace Implementation
	{
		//! The substrate class a preset was calibrated against.  Used
		//! for the warn-level mismatch diagnostic in
		//! `Job::AddFabricMaterial` and, later, as what `make_fabric`
		//! mints (docs/CLOTH_FABRIC_DESIGN.md 9.7 step 1).
		enum FabricSubstrateClass
		{
			eFabricSubstrateAny = 0,	///< `custom`: no recommendation, never warns
			eFabricSubstrateLambertian,
			eFabricSubstrateOrenNayar,
			eFabricSubstrateGGX,
			//! PHASE 2.  `weave_material` -- the structured
			//! two-thread-family cloth BSDF.  Recommended by exactly the
			//! three presets whose look IS pattern-scale structure
			//! (denim's wale, silk's and satin's floats), and by none of
			//! the others: cotton, linen and wool read statistically at
			//! any sane thread count, and velvet is a PILE, not a weave
			//! -- giving any of them a weave draft would author structure
			//! the fabric does not have.
			eFabricSubstrateWeave
		};

		struct FabricPreset
		{
			const char*				name;

			// ---- What the enum actually SETS on the chunk's own slots.
			Scalar					sheenRoughness;		///< seeds `sheen_roughness`
			bool					setsSheenColor;		///< false => the slot defaults to WHITE
			RISEPel					sheenColor;			///< meaningful only when setsSheenColor

			// ---- What the enum can only RECOMMEND (verb-supplied).
			FabricSubstrateClass	substrate;
			Scalar					substrateRoughness;	///< Oren-Nayar sigma; 0 when not applicable
			Scalar					substrateAlphaX;	///< GGX alphax; 0 when not applicable
			Scalar					substrateAlphaY;	///< GGX alphay; 0 when not applicable

			//! PHASE 2.  The `weave_material` PRESET name this fabric
			//! wants under it, meaningful only when
			//! `substrate == eFabricSubstrateWeave`.  `make_fabric` mints
			//! a `weave_material` carrying it.
			//!
			//! `substrateAlphaX` / `substrateAlphaY` are KEPT on those
			//! three rows rather than zeroed, and that is deliberate:
			//! they are the documented Phase-1 FALLBACK.  An anisotropic
			//! `ggx_material` under one of these presets is still a
			//! legal, shipping composition -- it is what every
			//! pre-Phase-2 scene has -- it is simply not what the preset
			//! now recommends, and the WARN-level diagnostic says so.
			//! Deleting the numbers would make the fallback
			//! undiscoverable.
			const char*				weavePreset;		///< NULL when not applicable

			const char*				note;				///< one line, for diagnostics and docs
		};

		//! Human-readable substrate-class name, for the mismatch
		//! diagnostic.  Uses the SCENE-LANGUAGE keyword, not the C++
		//! class name, because that is what an author would have to type.
		inline const char* FabricSubstrateClassText( const FabricSubstrateClass c )
		{
			switch( c ) {
				case eFabricSubstrateLambertian: return "lambertian_material";
				case eFabricSubstrateOrenNayar:  return "orennayar_material";
				case eFabricSubstrateGGX:        return "ggx_material";
				case eFabricSubstrateWeave:      return "weave_material";
				case eFabricSubstrateAny:
				default:                         return "any";
			}
		}

		//! docs/CLOTH_FABRIC_DESIGN.md 9.3's table, verbatim.
		//!
		//! `custom` is LAST and is the fallback for an unrecognised
		//! spelling; it sets sheen_roughness to 0.5, which is
		//! `sheen_material`'s own long-standing default, so a
		//! `fabric_material` that names no preset behaves like the bare
		//! sheen lobe it wraps.
		//!
		//! Every roughness here is at or above `FabricBRDF::kMinSheenAlpha`
		//! (0.04); velvet's 0.08 is the tightest and is exactly the
		//! "pure grazing halo" the table describes.
		inline const FabricPreset* FabricPresetTable( unsigned int& count )
		{
			static const FabricPreset kPresets[] = {
				// ORDER MATCHES docs/CLOTH_FABRIC_DESIGN.md 9.3's enum
				// spelling exactly (cotton|denim|silk|satin|velvet|wool|
				// linen|custom), and FabricPresetNamesText() plus the
				// chunk descriptor's enumValues must stay in lockstep
				// with it -- that list is what surfaces in the editor's
				// autocomplete and in the agent-facing schema, so a
				// divergence from the doc is a divergence on the
				// authoring surface.  Lookup is BY NAME, so the order is
				// presentation only: LookupFabricPreset finds both the
				// requested name and the `custom` fallback by strcmp, so
				// reordering this table cannot change behaviour.
				//
				// name       rough  setsCol  colour                       substrate                  sigma  ax     ay     weavePreset
				{ "cotton",   0.55,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateOrenNayar, 0.40,  0.0,   0.0,   0,
				  "matte; isotropic base is correct" },
				{ "denim",    0.45,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateWeave,     0.0,   0.34,  0.22,  "denim",
				  "the twill wale is the look, and since Phase 2 it is a weave_material's 3/1 draft, not substrate anisotropy" },
				{ "silk",     0.20,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateWeave,     0.0,   0.30,  0.10,  "silk",
				  "strongly directional; a flat warp crossing a twisted weft, on a 5-harness satin draft" },
				{ "satin",    0.12,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateWeave,     0.0,   0.34,  0.06,  "satin",
				  "float direction dominates; the tightest longitudinal lobe in the set" },
				// Velvet is the ONE preset that sets a colour: 9.3's table
				// reads "dye, dark" for its sheen column and "lambertian_material,
				// dark" for its substrate.  The pile's deep tone is the dye;
				// the bright rim comes from the Charlie lobe's own grazing
				// peak, not from a white tint.  Indicative, per the table's
				// own caveat.
				{ "velvet",   0.08,  true,    RISEPel(0.30,0.30,0.30),     eFabricSubstrateLambertian,0.0,   0.0,   0.0,   0,
				  "pure grazing halo; ISOTROPIC on purpose -- velvet is a pile, not a weave" },
				{ "wool",     0.75,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateOrenNayar, 0.60,  0.0,   0.0,   0,
				  "broad, soft sheen; isotropic" },
				{ "linen",    0.65,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateOrenNayar, 0.50,  0.0,   0.0,   0,
				  "coarser slub; pair with a gabor3d_painter breakup, or with a weave_material linen for a real plain draft" },
				{ "custom",   0.50,  false,   RISEPel(1.0,1.0,1.0),        eFabricSubstrateAny,       0.0,   0.0,   0.0,   0,
				  "no preset; every slot is the author's own" }
			};
			count = (unsigned int)( sizeof( kPresets ) / sizeof( kPresets[0] ) );
			return kPresets;
		}

		//! The enum's value list, as ONE string, so the descriptor's
		//! `enumValues`, the diagnostic and the future verb's tool
		//! schema cannot drift.  Order matches FabricPresetTable.
		inline const char* FabricPresetNamesText()
		{
			return "cotton, denim, silk, satin, velvet, wool, linen, custom";
		}

		//! Case-sensitive lookup; an unrecognised (or null) name falls
		//! back to `custom`, which is also the descriptor's default.
		//! The parser's descriptor already constrains the slot to the
		//! enum values, so the fallback is a defence for API callers,
		//! not the expected path.
		inline const FabricPreset& LookupFabricPreset( const char* name )
		{
			unsigned int n = 0;
			const FabricPreset* t = FabricPresetTable( n );
			if( name && name[0] ) {
				for( unsigned int i = 0; i < n; ++i ) {
					if( std::strcmp( t[i].name, name ) == 0 ) return t[i];
				}
			}
			// The fallback is `custom`, found BY NAME rather than by
			// position.  An earlier revision returned `t[n-1]` on the
			// assumption that custom is the last row -- true today, and
			// silently wrong the moment anyone reorders the table, at
			// which point some other fabric would quietly become the
			// default for every unrecognised spelling with no diagnostic
			// anywhere.  Searching costs one extra strcmp on a path that
			// runs once per chunk at parse time.
			for( unsigned int i = 0; i < n; ++i ) {
				if( std::strcmp( t[i].name, "custom" ) == 0 ) return t[i];
			}
			return t[n - 1];	// unreachable while the table has a `custom` row
		}
	}
}

#endif
