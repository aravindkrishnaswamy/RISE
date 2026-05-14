//////////////////////////////////////////////////////////////////////
//
//  PainterToScalarAdapter.h — adapts an `IPainter` to the
//  `IScalarPainter` interface by reading the red channel.
//
//  This is a TRANSITIONAL bridge used during the
//  `docs/ISCALARPAINTER_REFACTOR.md` rollout.  Several material
//  factories (notably `PBRMetallicRoughness`, `KHR_materials_anisotropy`)
//  build dynamic `BlendPainter` / `UVTransformPainter` chains at
//  construction time and feed those chains into material slots that
//  Phase 4 has converted to `IScalarPainter`.  Rebuilding those chains
//  in pure-scalar space would require a fleet of new
//  `*ScalarPainter` operator classes (Blend, UVTransform, etc.) — a
//  meaningful workstream of its own.
//
//  Until that pure-scalar painter algebra is built out, this adapter
//  lets the existing IPainter chains plug into the new `IScalarPainter`
//  consumers correctly enough — physical-scalar slots like roughness
//  and metallic are conventionally grayscale, so reading channel 0
//  is the canonical lossy read.
//
//  Spectral path: GetValueAtNM samples source.GetColor(ri).v[0] and
//  returns the same scalar at every wavelength.  An earlier revision
//  forwarded the call to source.GetColorNM(ri, nm), which routes
//  through the JH spectral uplift inside `UniformColorPainter` and
//  the like — exactly the bug this entire refactor is trying to
//  excise.  ChannelPainter (see Painters/ChannelPainter.h) documents
//  the same pattern for glTF metallicRoughness extraction.  Wrapping
//  a wavelength-dependent IPainter (e.g. a `spectral_painter`) in
//  this adapter is intentionally unsupported — wavelength dependence
//  belongs in a native IScalarPainter form (Sellmeier, polynomial,
//  piecewise-linear file, etc.).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PAINTER_TO_SCALAR_ADAPTER_
#define PAINTER_TO_SCALAR_ADAPTER_

#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PainterToScalarAdapter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const IPainter& source;

			virtual ~PainterToScalarAdapter()
			{
				source.release();
			}

		public:
			explicit PainterToScalarAdapter( const IPainter& src ) : source( src )
			{
				source.addref();
			}

			ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
			{
				const RISEPel pel = source.GetColor( ri );
				return ScalarTriple( pel[0], pel[1], pel[2] );
			}

			Scalar GetValueAtNM( const RayIntersectionGeometric& ri, const Scalar nm ) const override
			{
				// Physical scalars are wavelength-independent by
				// construction.  Do NOT call source.GetColorNM(ri, nm)
				// here — that routes through the JH spectral uplift
				// for `UniformColorPainter`-backed inputs, which is
				// exactly the bug the IScalarPainter pipe exists to
				// avoid.  Read the source's red channel via the
				// non-spectral GetColor and return that scalar at
				// every wavelength.  ChannelPainter follows the same
				// pattern for glTF metallicRoughness extraction.
				(void)nm;
				return source.GetColor( ri )[0];
			}

			//! The wrapped painter's red channel may differ from G/B
			//! (texture-mapped roughness, e.g.).  Treat as per-channel
			//! variable so single-scalar-required slots get rejected,
			//! and dispersion-detection paths in materials correctly
			//! trigger spectral evaluation when fed a wrapped painter.
			bool HasPerChannelVariation() const override
			{
				return true;
			}
		};
	}
}

#endif
