//////////////////////////////////////////////////////////////////////
//
//  BlendPainter.h - Defines a painter that paints some
//  uniform color
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BLEND_PAINTER_
#define BLEND_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class BlendPainter : public Painter
		{
		public:
			//! P2.4 (doc 88): the blend formula applied BETWEEN a and b
			//! before the mask interpolates toward b.  `mode` decides HOW
			//! a and b combine; `mask` still decides HOW MUCH of that
			//! combination shows through versus b -- see Combine()'s file
			//! comment for the exact algebra and why Mix is byte-identical
			//! to the pre-P2.4 formula.
			enum Mode
			{
				Mode_Mix      = 0,	// out = a (mask alone decides a vs b -- the original behaviour)
				Mode_Multiply = 1,	// out = a * b  (unclamped)
				Mode_Screen   = 2,	// out = 1 - (1-a) * (1-b) -- a,b CLAMPED to [0,1] first (P1-C)
				Mode_Overlay  = 3,	// out = b<=0.5 ? 2*a*b : 1-2*(1-a)*(1-b)  (b is the "base") -- a,b CLAMPED to [0,1] first (P1-C)
				Mode_Add      = 4	// out = a + b  (unclamped, like the rest of RISE's colour math)
			};

		protected:
			const IPainter&	a;
			const IPainter& b;
			const IPainter& mask;
			const Mode		mode;

			virtual ~BlendPainter()
			{
				a.release();
				b.release();
				mask.release();
			};

			//! Per-channel blend formula, shared by GetColor (per RGB
			//! channel) and GetColorNM (per spectral sample) so the two
			//! paths can never diverge in shape -- only in what `a`/`b`
			//! evaluate to.  For Mode_Mix this returns `av` UNCHANGED, so
			//! `Combine(av,bv)*mask + bv*(1-mask)` reduces ALGEBRAICALLY
			//! (no extra floating-point op) to the original
			//! `av*mask + bv*(1-mask)` -- existing scenes (mode omitted =
			//! Mix) are byte-identical.
			static inline Scalar Combine( const Scalar av, const Scalar bv, const Mode m )
			{
				switch( m ) {
					case Mode_Multiply:	return av * bv;
					case Mode_Screen: {
						// P1-C fix (S7 review round 1): Screen and Overlay are
						// DISPLAY-COMPOSITING curves defined algebraically only on
						// the [0,1] domain -- outside it they don't just
						// extrapolate oddly, they SIGN-FLIP (e.g. a=1.5 gives
						// `1-(1-1.5)*(1-b) = 1+0.5*(1-b)`, a POSITIVE term added
						// for an input that's supposed to be "more than fully
						// bright").  Clamp both operands to [0,1] before the
						// curve.  Mix/Multiply/Add stay genuinely unbounded (RISE
						// colour math routinely carries HDR values > 1 through
						// multiply/add chains, and neither formula sign-flips
						// outside [0,1]).
						const Scalar ac = Clamp01( av );
						const Scalar bc = Clamp01( bv );
						return Scalar(1) - (Scalar(1)-ac) * (Scalar(1)-bc);
					}
					case Mode_Overlay: {
						// Same [0,1]-domain rationale as Mode_Screen above -- the
						// `bv <= 0.5` branch selector itself assumes a normalized
						// input, so an out-of-range bv can land on the wrong half
						// of the piecewise definition entirely without clamping.
						const Scalar ac = Clamp01( av );
						const Scalar bc = Clamp01( bv );
						return bc <= Scalar(0.5) ? Scalar(2)*ac*bc : Scalar(1) - Scalar(2)*(Scalar(1)-ac)*(Scalar(1)-bc);
					}
					case Mode_Add:			return av + bv;
					case Mode_Mix:
					default:				return av;
				}
			}

			//! [0,1] clamp for the Screen/Overlay display-compositing curves
			//! -- see Combine()'s comment above for why only those two modes
			//! need it.
			static inline Scalar Clamp01( const Scalar v )
			{
				return v < Scalar(0) ? Scalar(0) : ( v > Scalar(1) ? Scalar(1) : v );
			}

		public:

			BlendPainter(
				const IPainter&	a_,
				const IPainter&	b_,
				const IPainter&	mask_,
				const Mode		mode_ = Mode_Mix
				) :
			a( a_ ),
			b( b_ ),
			mask( mask_ ),
			mode( mode_ )
			{
				a.addref();
				b.addref();
				mask.addref();
			};

			//! Structural introspection (CstDeriveGoldenTest's DumpJob
			//! composition digest): operands, mask, and blend mode.
			const IPainter& GetA() const { return a; }
			const IPainter& GetB() const { return b; }
			const IPainter& GetMask() const { return mask; }
			Mode GetMode() const { return mode; }

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const
			{
				const RISEPel ca = a.GetColor( ri );
				const RISEPel cb = b.GetColor( ri );
				const RISEPel cmask = mask.GetColor( ri );

				const RISEPel combined(
					Combine( ca.r, cb.r, mode ),
					Combine( ca.g, cb.g, mode ),
					Combine( ca.b, cb.b, mode ) );

				return combined*cmask + cb*(RISEPel(1.0,1.0,1.0)-cmask);
			}

			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				const Scalar dmask = mask.GetColorNM( ri, nm );
				const Scalar da = a.GetColorNM( ri, nm );
				const Scalar db = b.GetColorNM( ri, nm );
				const Scalar combined = Combine( da, db, mode );
				return (combined*dmask + db*(1.0-dmask));
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif

