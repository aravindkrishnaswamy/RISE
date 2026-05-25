//////////////////////////////////////////////////////////////////////
//
//  Channel.h - Typed buffer for one channel of a FrameStore.
//
//  A FrameStore holds a (mostly fixed) set of channels: Beauty
//  (RISEPel), Alpha (float), and several optional AOV channels
//  (albedo, normal, depth, object id, primitive id).  Each channel
//  has a known compile-time element type, so storage is
//  template-typed rather than type-erased.
//
//  Adding a new channel kind requires:
//    1. Adding a value to the ChannelId enum (below)
//    2. Adding a ChannelTraits<...> specialisation mapping the
//       enum value to its element type
//    3. Adding storage + accessor wiring in FrameStore.{h,cpp}
//
//  This is intentionally a recompile-on-extend design — the
//  expected channel set is small and known up-front, so the
//  type-safety win outweighs the runtime-extensibility cost.
//
//  Author: design landing L1
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESTORE_CHANNEL_
#define FRAMESTORE_CHANNEL_

#include <cstddef>
#include <cstdint>
#include <vector>
#include <cassert>

#include "../Utilities/Color/Color.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace FrameStoreOutput
	{
		//! Identifier for a FrameStore channel.  The value is also
		//! the index into per-channel-presence bookkeeping inside
		//! FrameStore; the COUNT sentinel is the iteration boundary.
		//!
		//! Beauty is implicitly always present.  Alpha is implicitly
		//! present whenever the rasterizer has alpha to track (the
		//! current default).  All other channels are opt-in via
		//! FrameStore::Spec::aovChannels.
		enum class ChannelId : uint32_t
		{
			Beauty       = 0,  ///< RISEPel (Rec.709 Linear D65 post Stage B colour-space migration), full HDR radiance
			Alpha        = 1,  ///< float, [0, 1]
			Albedo       = 2,  ///< RISEPel, Rec.709 Linear (denoiser AOV / export)
			Normal       = 3,  ///< Vector3 world-space, unit length
			Depth        = 4,  ///< float, camera-space distance
			ObjectId     = 5,  ///< uint32_t, object index from the scene's ObjectManager
			PrimitiveId  = 6,  ///< uint32_t, primitive index within object

			// Sentinel — must stay last.  Used as the iteration
			// boundary and as the array size for per-channel
			// presence bookkeeping in FrameStore.
			COUNT
		};

		//! Compile-time mapping from ChannelId to element type.
		//! Specialisations live in this header so users can write
		//! `Channel<ChannelTraits<ChannelId::Normal>::Type>` and get
		//! a Channel<Vector3>.
		template <ChannelId C>
		struct ChannelTraits;

		template <> struct ChannelTraits<ChannelId::Beauty>      { using Type = RISEPel;   };
		// Alpha is `Chel` (= double) NOT float to match the legacy
		// Color_Template<RISEPel>::a precision.  Storing as float
		// would introduce a lossy double→float→double roundtrip on
		// non-1.0 alpha values (e.g. 0.7) and break byte-identical
		// output to FileRasterizerOutput.  The 4-byte-per-pixel cost
		// (~32 MB on a 4K frame) is acceptable; we revisit if a
		// future memory pressure justifies splitting beauty + alpha
		// into a 4-channel struct.  See L2 adversarial review HIGH-1.
		template <> struct ChannelTraits<ChannelId::Alpha>       { using Type = Chel;      };
		template <> struct ChannelTraits<ChannelId::Albedo>      { using Type = RISEPel;   };
		template <> struct ChannelTraits<ChannelId::Normal>      { using Type = Vector3;   };
		template <> struct ChannelTraits<ChannelId::Depth>       { using Type = float;     };
		template <> struct ChannelTraits<ChannelId::ObjectId>    { using Type = uint32_t;  };
		template <> struct ChannelTraits<ChannelId::PrimitiveId> { using Type = uint32_t;  };

		//! Convenience alias.
		template <ChannelId C>
		using ChannelType = typename ChannelTraits<C>::Type;

		//! 2D row-major typed buffer.  Header-only because the
		//! storage is just a std::vector<T>; no virtual dispatch.
		//!
		//! NOT thread-safe by itself.  FrameStore wraps Channel
		//! access in a tile-level seqlock for write/read ordering;
		//! direct callers (single-threaded fill, post-render
		//! encoder readback under FrameStore guarantees) bypass it.
		template <typename T>
		class Channel
		{
		public:
			Channel( size_t width, size_t height )
				: width_( width )
				, height_( height )
				, data_( width * height )
			{
			}

			size_t Width()  const { return width_; }
			size_t Height() const { return height_; }
			size_t Size()   const { return data_.size(); }

			//! Pointer to the start of row y.  Asserts y is in
			//! range; caller treats subsequent (width) elements as
			//! contiguous.
			T* Row( size_t y )
			{
				assert( y < height_ );
				return data_.data() + y * width_;
			}
			const T* Row( size_t y ) const
			{
				assert( y < height_ );
				return data_.data() + y * width_;
			}

			//! Per-pixel access.  Bounds-checked in debug; raw
			//! indexing in release.
			T& At( size_t x, size_t y )
			{
				assert( x < width_ && y < height_ );
				return data_[ y * width_ + x ];
			}
			const T& At( size_t x, size_t y ) const
			{
				assert( x < width_ && y < height_ );
				return data_[ y * width_ + x ];
			}

			//! Bulk pointer for encoders that walk linearly.
			//! Stride is `Width()` elements (i.e. tightly packed).
			T*       Data()       { return data_.data(); }
			const T* Data() const { return data_.data(); }

			//! Fill the channel with a single value.
			void Fill( const T& v )
			{
				std::fill( data_.begin(), data_.end(), v );
			}

		private:
			size_t          width_;
			size_t          height_;
			std::vector<T>  data_;
		};

	} // namespace FrameStoreOutput
} // namespace RISE

#endif
