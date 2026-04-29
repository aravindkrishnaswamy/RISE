//////////////////////////////////////////////////////////////////////
//
//  AOVBuffers.h - Float-precision AOV (Arbitrary Output Variable)
//  buffers for denoiser input.  Stores first-hit albedo and
//  world-space normals as interleaved RGB float arrays, suitable
//  for direct consumption by Intel OIDN.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AOV_BUFFERS_H_
#define AOV_BUFFERS_H_

#include <vector>
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	/// First-hit AOV data extracted from a path sample.
	/// Used to populate the denoiser's auxiliary buffers.
	struct PixelAOV
	{
		RISEPel		albedo;
		Vector3		normal;
		bool		valid;

		PixelAOV() : valid( false ) {}
	};

	namespace Implementation
	{
		/// Stores first-hit albedo and normal AOVs as float buffers.
		/// Thread-safe for concurrent writes when each pixel is
		/// written by exactly one thread (guaranteed by RISE's
		/// non-overlapping block dispatch).
		class AOVBuffers
		{
			unsigned int width;
			unsigned int height;
			bool bHasData;					///< True once any sample has been accumulated
			std::vector<float> albedo;		///< width*height*3, RGB interleaved
			std::vector<float> normals;		///< width*height*3, XYZ interleaved

		public:
			AOVBuffers( unsigned int w, unsigned int h );

			/// Clears existing contents for reuse.  Reallocates only
			/// when dimensions change; otherwise preserves vector
			/// capacity and zeroes the existing storage.
			void Reset( unsigned int w, unsigned int h );

			/// Accumulates a weighted albedo sample at (x,y).
			/// The RISEPel channels (double) are narrowed to float.
			void AccumulateAlbedo(
				unsigned int x,
				unsigned int y,
				const RISEPel& c,
				Scalar weight
				);

			/// Accumulates a weighted normal sample at (x,y).
			/// The Vector3 components (double) are narrowed to float.
			void AccumulateNormal(
				unsigned int x,
				unsigned int y,
				const Vector3& n,
				Scalar weight
				);

			/// Divides accumulated albedo and normal at (x,y) by the
			/// total weight to produce the final per-pixel average.
			void Normalize(
				unsigned int x,
				unsigned int y,
				Scalar invWeight
				);

			/// Returns true if any AOV data has been accumulated.
			bool HasData() const { return bHasData; }

			const float* GetAlbedoPtr() const { return albedo.data(); }
			const float* GetNormalPtr() const { return normals.data(); }
			unsigned int GetWidth() const { return width; }
			unsigned int GetHeight() const { return height; }
		};
	}
}

#endif
