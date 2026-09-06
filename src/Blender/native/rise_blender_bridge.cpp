#include "rise_blender_bridge.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <RISE_API.h>
#include <Interfaces/IFunction2DManager.h>
#include <Interfaces/ILog.h>
#include <Interfaces/IJobPriv.h>
#include <Interfaces/IRasterizer.h>
#include <Interfaces/IJobRasterizerOutput.h>
#include <Interfaces/ILightManager.h>
#include <Interfaces/IMaterialManager.h>
#include <Interfaces/IMedium.h>
#include <Interfaces/IObjectPriv.h>
#include <Interfaces/IPainterManager.h>
#include <Interfaces/IPhaseFunction.h>
#include <Interfaces/IScalarPainter.h>
#include <Interfaces/IScalarPainterManager.h>
#include <Interfaces/IProgressCallback.h>
#include <Utilities/AdaptiveSamplingConfig.h>
#include <Utilities/Math3D/Math3D.h>
#include <Utilities/OidnConfig.h>
#include <Utilities/PathGuidingField.h>
#include <Utilities/ProgressiveConfig.h>
#include <Utilities/SMSConfig.h>
#include <Utilities/SpectralConfig.h>
#include <Utilities/StabilityConfig.h>

#ifdef RISE_BLENDER_ENABLE_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace
{
	const char* kShaderName = "rise_blender_surface";
	const double kPi = 3.14159265358979323846;

	template<typename T>
	T clamp_value( const T value, const T low, const T high )
	{
		return std::min( high, std::max( low, value ) );
	}

	void write_error( char* buffer, const size_t size, const char* message )
	{
		if( !buffer || size == 0 ) {
			return;
		}

		std::snprintf( buffer, size, "%s", message ? message : "Unknown error" );
		buffer[size-1] = 0;
	}

	bool color_non_zero( const float color[3] )
	{
		return color && (
			std::fabs( color[0] ) > 1e-6f ||
			std::fabs( color[1] ) > 1e-6f ||
			std::fabs( color[2] ) > 1e-6f
		);
	}

	// Blender's `light.color` is linear Rec.709, and since 2026-09-02
	// Job::Add*Light take a linear triple plus a colour-space name -- so
	// the value passes straight through.  What used to live here was an
	// sRGB OETF encode that existed ONLY to cancel those methods'
	// unconditional gamma DECODE (without it, mid-greys came out ~57 %
	// dim and colours nonlinearly desaturated).  Both the trap and the
	// workaround are gone; see IJob.h's "COLOUR CONVENTION (2026-09-02)".
	//
	// Negative clamp is kept: Blender colour pickers occasionally emit
	// slightly negative tristimulus values, which have no meaning as
	// emitted radiance.
	void blender_light_color( const float linear[3], double out[3] )
	{
		out[0] = std::max( 0.0, double( linear[0] ) );
		out[1] = std::max( 0.0, double( linear[1] ) );
		out[2] = std::max( 0.0, double( linear[2] ) );
	}

	void normalize3( double value[3] )
	{
		const double magnitude = std::sqrt( value[0]*value[0] + value[1]*value[1] + value[2]*value[2] );
		if( magnitude <= 1e-12 ) {
			return;
		}

		value[0] /= magnitude;
		value[1] /= magnitude;
		value[2] /= magnitude;
	}

	const char* uniform_color_space_name( const int color_space )
	{
		switch( color_space )
		{
		case RISE_BLENDER_COLOR_SRGB:
			return "sRGB";
		case RISE_BLENDER_COLOR_ROMM_LINEAR:
			return "ROMMRGB_Linear";
		case RISE_BLENDER_COLOR_LINEAR:
		default:
			return "Rec709RGB_Linear";
		}
	}

	char texture_color_space_value( const int color_space )
	{
		// Char encoding mirrors Job::AddPNGTexturePainter etc.:
		//   0 = Rec709RGB_Linear
		//   1 = sRGB
		//   2 = ROMMRGB_Linear  (preserves bit-exact vector data)
		switch( color_space )
		{
		case RISE_BLENDER_COLOR_SRGB:
			return 1;
		case RISE_BLENDER_COLOR_ROMM_LINEAR:
			return 2;
		case RISE_BLENDER_COLOR_LINEAR:
		default:
			return 0;
		}
	}

	unsigned int bounce_limit_from_ui( const uint32_t value )
	{
		return value == 0 ? UINT_MAX : value;
	}

	RISE::GuidingSamplingType guiding_sampling_type_from_int( const uint32_t value )
	{
		return value == 1 ? RISE::eGuidingRIS : RISE::eGuidingOneSampleMIS;
	}

	RISE::OidnQuality oidn_quality_from_int( const uint32_t value )
	{
		switch( value )
		{
		case 1: return RISE::OidnQuality::High;
		case 2: return RISE::OidnQuality::Balanced;
		case 3: return RISE::OidnQuality::Fast;
		case 0:
		default:
			return RISE::OidnQuality::Auto;
		}
	}

	RISE::OidnDevice oidn_device_from_int( const uint32_t value )
	{
		switch( value )
		{
		case 1: return RISE::OidnDevice::CPU;
		case 2: return RISE::OidnDevice::GPU;
		case 0:
		default:
			return RISE::OidnDevice::Auto;
		}
	}

	RISE::OidnPrefilter oidn_prefilter_from_int( const uint32_t value )
	{
		return value == 1 ? RISE::OidnPrefilter::Accurate : RISE::OidnPrefilter::Fast;
	}

	RISE::SMSSeedingMode sms_seeding_mode_from_int( const uint32_t value )
	{
		return value == 1 ? RISE::SMSSeedingMode::Uniform : RISE::SMSSeedingMode::Snell;
	}

	RISE::Matrix4 blender_matrix_to_rise_matrix( const float transform[16] )
	{
		// Blender's mathutils.Matrix iterates by ROWS (per the 4.x
		// API), so the addon's _flatten_matrix produces row-major:
		//   transform[r*4 + c] = m[r][c]
		// Blender uses column-vector math (v' = M·v) with translation
		// at m[0,3], m[1,3], m[2,3].
		//
		// RISE Matrix4 uses _<col><row> naming and the same column-
		// vector convention (Job.cpp matmul confirms this), so the
		// math element at (row R, col C) maps to mx._<col=C, row=R>.
		// In code: mx._<c><r> = transform[r*4 + c].
		//
		// Pre-2026-05 the rotation 3x3 block was transposed (the
		// off-diagonal indices read from the row instead of the
		// column), which made a Blender 90 deg Z rotation render as
		// -90 deg.  Translations were unaffected, which is why
		// axis-aligned default scenes looked correct.
		RISE::Matrix4 matrix;

		matrix._00 = transform[ 0]; matrix._01 = transform[ 4]; matrix._02 = transform[ 8]; matrix._03 = transform[12];
		matrix._10 = transform[ 1]; matrix._11 = transform[ 5]; matrix._12 = transform[ 9]; matrix._13 = transform[13];
		matrix._20 = transform[ 2]; matrix._21 = transform[ 6]; matrix._22 = transform[10]; matrix._23 = transform[14];
		matrix._30 = transform[ 3]; matrix._31 = transform[ 7]; matrix._32 = transform[11]; matrix._33 = transform[15];

		return matrix;
	}

	bool validate_index_array( const uint32_t* indices, const size_t count, const uint32_t limit )
	{
		if( !indices ) {
			return false;
		}

		for( size_t i = 0; i < count; ++i ) {
			if( indices[i] >= limit ) {
				return false;
			}
		}

		return true;
	}

	bool validate_mesh_payload( const rise_blender_mesh& mesh, char* error_message, const size_t error_message_size )
	{
		if( !mesh.name || !mesh.vertices || !mesh.vertex_indices || mesh.num_vertices == 0 || mesh.num_triangles == 0 ) {
			write_error( error_message, error_message_size, "Encountered an invalid mesh payload" );
			return false;
		}

		const size_t triangle_index_count = static_cast<size_t>( mesh.num_triangles ) * 3;

		if( !validate_index_array( mesh.vertex_indices, triangle_index_count, mesh.num_vertices ) ) {
			write_error( error_message, error_message_size, "Mesh vertex indices are out of range" );
			return false;
		}

		if( mesh.num_normals > 0 && !mesh.use_face_normals ) {
			if( !mesh.normals || !validate_index_array( mesh.normal_indices, triangle_index_count, mesh.num_normals ) ) {
				write_error( error_message, error_message_size, "Mesh normal payload is invalid" );
				return false;
			}
		}

		if( mesh.num_uvs > 0 ) {
			if( !mesh.uvs || !validate_index_array( mesh.uv_indices, triangle_index_count, mesh.num_uvs ) ) {
				write_error( error_message, error_message_size, "Mesh UV payload is invalid" );
				return false;
			}
		}

		return true;
	}

	class MemoryRasterizerOutput : public RISE::IJobRasterizerOutput
	{
	protected:
		std::vector<float> rgba_;
		unsigned int width_;
		unsigned int height_;
		rise_blender_image_callback image_callback_;
		void* user_data_;
		mutable std::mutex buffer_mutex_;

	public:
		MemoryRasterizerOutput( rise_blender_image_callback image_callback, void* user_data ) :
			width_( 0 ),
			height_( 0 ),
			image_callback_( image_callback ),
			user_data_( user_data )
		{
		}

		bool PremultipliedAlpha() override
		{
			return false;
		}

		int GetColorSpace() override
		{
			return 0;
		}

		void OutputImageRGBA16(
			const unsigned short* pImageData,
			const unsigned int width,
			const unsigned int height,
			const unsigned int rc_top,
			const unsigned int rc_left,
			const unsigned int rc_bottom,
			const unsigned int rc_right
		) override
		{
			// Legacy 16-bit path.  RISE now drives the HDR float path
			// via `WantsFloat32` + `OutputImageRGBA32F` below, so this
			// callback shouldn't fire in production renders — kept for
			// the pure-virtual contract and as a safety fallback for
			// any RISE build that predates the float32 plumbing.
			if( !pImageData ) {
				return;
			}

			{
				std::lock_guard<std::mutex> guard( buffer_mutex_ );

				if( width_ != width || height_ != height || rgba_.empty() ) {
					width_ = width;
					height_ = height;
					rgba_.assign( width_ * height_ * 4, 0.0f );
				}

				for( unsigned int y = rc_top; y <= rc_bottom; ++y ) {
					for( unsigned int x = rc_left; x <= rc_right; ++x ) {
						const unsigned int offset = ( y * width_ + x ) * 4;
						rgba_[offset+0] = float( pImageData[offset+0] ) / 65535.0f;
						rgba_[offset+1] = float( pImageData[offset+1] ) / 65535.0f;
						rgba_[offset+2] = float( pImageData[offset+2] ) / 65535.0f;
						rgba_[offset+3] = float( pImageData[offset+3] ) / 65535.0f;
					}
				}
			}

			if( image_callback_ ) {
				const unsigned int region_width = rc_right - rc_left + 1;
				const unsigned int region_height = rc_bottom - rc_top + 1;
				std::vector<unsigned short> region_rgba( region_width * region_height * 4, 0 );

				for( unsigned int y = 0; y < region_height; ++y ) {
					const unsigned short* src = pImageData + ( ( ( rc_top + y ) * width + rc_left ) * 4 );
					unsigned short* dst = &region_rgba[y * region_width * 4];
					std::memcpy( dst, src, sizeof( unsigned short ) * region_width * 4 );
				}

				image_callback_(
					user_data_,
					&region_rgba[0],
					region_width,
					region_height,
					width,
					height,
					rc_top,
					rc_left
				);
			}
		}

		bool WantsFloat32() override
		{
			// HDR path: Blender's render pass consumes linear-float
			// RGBA and applies its own view transform (Filmic) +
			// exposure compensation downstream.  Without this opt-in
			// the 16-bit Integerize step clamps every linear value
			// > 1.0 to 1.0, leaving no HDR headroom for Filmic.
			// Verified against the Cycles reference: pre-fix RISE
			// peaked at linear value 1.10; Cycles delivers up to ~223
			// for sun-disc pixels on the same scene.
			return true;
		}

		void OutputImageRGBA32F(
			const float* pImageData,
			const unsigned int width,
			const unsigned int height,
			const unsigned int rc_top,
			const unsigned int rc_left,
			const unsigned int rc_bottom,
			const unsigned int rc_right
		) override
		{
			if( !pImageData ) {
				return;
			}

			{
				std::lock_guard<std::mutex> guard( buffer_mutex_ );

				if( width_ != width || height_ != height || rgba_.empty() ) {
					width_ = width;
					height_ = height;
					rgba_.assign( width_ * height_ * 4, 0.0f );
				}

				for( unsigned int y = rc_top; y <= rc_bottom; ++y ) {
					for( unsigned int x = rc_left; x <= rc_right; ++x ) {
						const unsigned int offset = ( y * width_ + x ) * 4;
						rgba_[offset+0] = pImageData[offset+0];
						rgba_[offset+1] = pImageData[offset+1];
						rgba_[offset+2] = pImageData[offset+2];
						rgba_[offset+3] = pImageData[offset+3];
					}
				}
			}

			// Preview callback still ships uint16 (the addon's preview
			// loop unpacks 16-bit via `array.array("H")`).  Quantize
			// here, clamping HDR > 1.0 to display-max — this is just
			// the preview, not the final image; HDR is preserved in
			// `rgba_` for the final `_set_pass_rect` delivery.
			if( image_callback_ ) {
				const unsigned int region_width = rc_right - rc_left + 1;
				const unsigned int region_height = rc_bottom - rc_top + 1;
				std::vector<unsigned short> region_rgba( region_width * region_height * 4, 0 );

				for( unsigned int y = 0; y < region_height; ++y ) {
					for( unsigned int x = 0; x < region_width; ++x ) {
						const unsigned int src = ( ( rc_top + y ) * width + ( rc_left + x ) ) * 4;
						const unsigned int dst = ( y * region_width + x ) * 4;
						for( int c = 0; c < 4; ++c ) {
							float v = pImageData[src + c];
							if( v < 0.0f ) v = 0.0f;
							if( v > 1.0f ) v = 1.0f;
							region_rgba[dst + c] = static_cast<unsigned short>( v * 65535.0f + 0.5f );
						}
					}
				}

				image_callback_(
					user_data_,
					&region_rgba[0],
					region_width,
					region_height,
					width,
					height,
					rc_top,
					rc_left
				);
			}
		}

		bool CopyToResult( rise_blender_render_result& result ) const
		{
			std::lock_guard<std::mutex> guard( buffer_mutex_ );

			if( rgba_.empty() || width_ == 0 || height_ == 0 ) {
				return false;
			}

			const size_t total_values = rgba_.size();
			result.rgba = static_cast<float*>( std::malloc( total_values * sizeof( float ) ) );
			if( !result.rgba ) {
				return false;
			}

			std::memcpy( result.rgba, &rgba_[0], total_values * sizeof( float ) );
			result.width = width_;
			result.height = height_;
			return true;
		}
	};

	class ProgressBridge : public RISE::IProgressCallback
	{
	protected:
		rise_blender_progress_callback callback_;
		void* user_data_;
		bool aborted_;

	public:
		ProgressBridge( rise_blender_progress_callback callback, void* user_data ) :
			callback_( callback ),
			user_data_( user_data ),
			aborted_( false )
		{
		}

		bool Progress( const double progress, const double total ) override
		{
			if( !callback_ ) {
				return true;
			}

			const float normalized = total > 0.0 ? float( progress / total ) : 1.0f;
			aborted_ = callback_( user_data_, clamp_value<float>( normalized, 0.0f, 1.0f ), 0 ) == 0;
			return !aborted_;
		}

		void SetTitle( const char* title ) override
		{
			if( !callback_ ) {
				return;
			}

			aborted_ = callback_( user_data_, -1.0f, title ) == 0;
		}

		bool Aborted() const
		{
			return aborted_;
		}
	};

	struct SliceStackInfo
	{
		std::filesystem::path directory;
		std::string file_pattern;
		unsigned int width;
		unsigned int height;
		unsigned int start_z;
		unsigned int end_z;
		double density_scale;
	};

	struct VolumeCacheGuard
	{
		std::vector<std::filesystem::path> directories;

		~VolumeCacheGuard()
		{
			for( std::vector<std::filesystem::path>::const_reverse_iterator it = directories.rbegin(); it != directories.rend(); ++it ) {
				std::error_code ec;
				std::filesystem::remove_all( *it, ec );
			}
		}
	};

#ifdef RISE_BLENDER_ENABLE_OPENVDB
	bool openvdb_initialized()
	{
		static bool initialized = false;
		if( !initialized ) {
			openvdb::initialize();
			initialized = true;
		}
		return initialized;
	}

	bool make_volume_cache_directory(
		const char* temp_root,
		std::filesystem::path& directory,
		char* error_message,
		const size_t error_message_size
	)
	{
		std::filesystem::path root = ( temp_root && temp_root[0] ) ? std::filesystem::path( temp_root ) : std::filesystem::temp_directory_path();
		std::error_code ec;
		std::filesystem::create_directories( root, ec );
		if( ec ) {
			write_error( error_message, error_message_size, "Failed to create the temporary directory for VDB conversion" );
			return false;
		}

		for( int attempt = 0; attempt < 64; ++attempt ) {
			char suffix[64] = {0};
			std::snprintf( suffix, sizeof( suffix ), "rise_blender_vdb_%d_%d", int( std::time( 0 ) & 0x7fffffff ), attempt );
			std::filesystem::path candidate = root / suffix;
			if( std::filesystem::create_directory( candidate, ec ) ) {
				directory = candidate;
				return true;
			}
			ec.clear();
		}

		write_error( error_message, error_message_size, "Failed to allocate a unique temporary directory for VDB conversion" );
		return false;
	}

	template<typename GridT>
	bool create_slice_stack_from_grid(
		typename GridT::Ptr grid,
		const char* temp_root,
		SliceStackInfo& stack_info,
		char* error_message,
		const size_t error_message_size
	)
	{
		const openvdb::CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
		if( bbox.empty() ) {
			write_error( error_message, error_message_size, "The VDB density grid is empty" );
			return false;
		}

		const openvdb::Coord dim = bbox.dim();
		if( dim.x() <= 0 || dim.y() <= 0 || dim.z() <= 0 ) {
			write_error( error_message, error_message_size, "The VDB density grid has invalid dimensions" );
			return false;
		}

		stack_info.width = static_cast<unsigned int>( dim.x() );
		stack_info.height = static_cast<unsigned int>( dim.y() );
		stack_info.start_z = 0;
		stack_info.end_z = static_cast<unsigned int>( dim.z() - 1 );
		stack_info.density_scale = 0.0;

		for( typename GridT::ValueOnCIter iter = grid->cbeginValueOn(); iter; ++iter ) {
			const double value = static_cast<double>( *iter );
			if( std::isfinite( value ) && value > stack_info.density_scale ) {
				stack_info.density_scale = value;
			}
		}

		if( !make_volume_cache_directory( temp_root, stack_info.directory, error_message, error_message_size ) ) {
			return false;
		}

		stack_info.file_pattern = ( stack_info.directory / "slice_%04d.raw" ).string();

		typename GridT::ConstAccessor accessor = grid->getConstAccessor();
		const int min_x = bbox.min().x();
		const int min_y = bbox.min().y();
		const int min_z = bbox.min().z();

		for( unsigned int z = stack_info.start_z; z <= stack_info.end_z; ++z ) {
			std::vector<unsigned char> slice( stack_info.width * stack_info.height, 0 );
			for( unsigned int y = 0; y < stack_info.height; ++y ) {
				for( unsigned int x = 0; x < stack_info.width; ++x ) {
					const openvdb::Coord coord( min_x + int( x ), min_y + int( y ), min_z + int( z ) );
					double value = static_cast<double>( accessor.getValue( coord ) );
					if( !std::isfinite( value ) || value < 0.0 ) {
						value = 0.0;
					}

					double normalized = 0.0;
					if( stack_info.density_scale > 0.0 ) {
						normalized = clamp_value<double>( value / stack_info.density_scale, 0.0, 1.0 );
					}
					slice[y * stack_info.width + x] = static_cast<unsigned char>( std::lround( normalized * 255.0 ) );
				}
			}

			char filename[4096] = {0};
			std::snprintf( filename, sizeof( filename ), stack_info.file_pattern.c_str(), z );
			std::ofstream stream( filename, std::ios::binary );
			if( !stream ) {
				write_error( error_message, error_message_size, "Failed to write a temporary density slice for VDB conversion" );
				return false;
			}
			stream.write( reinterpret_cast<const char*>( &slice[0] ), std::streamsize( slice.size() ) );
			if( !stream.good() ) {
				write_error( error_message, error_message_size, "Failed while writing a temporary density slice for VDB conversion" );
				return false;
			}
		}

		return true;
	}

	bool create_vdb_slice_stack(
		const char* source_filepath,
		const char* grid_name,
		const char* temp_root,
		SliceStackInfo& stack_info,
		char* error_message,
		const size_t error_message_size
	)
	{
		openvdb_initialized();

		if( !source_filepath || !source_filepath[0] ) {
			write_error( error_message, error_message_size, "A heterogeneous volume payload is missing its VDB file path" );
			return false;
		}

		if( !grid_name || !grid_name[0] ) {
			write_error( error_message, error_message_size, "A heterogeneous volume payload is missing its density grid name" );
			return false;
		}

		try {
			openvdb::io::File file( source_filepath );
			file.open();
			openvdb::GridBase::Ptr base_grid = file.readGrid( grid_name );
			file.close();

			if( !base_grid ) {
				write_error( error_message, error_message_size, "Failed to read the requested VDB density grid" );
				return false;
			}

			if( openvdb::FloatGrid::Ptr float_grid = openvdb::gridPtrCast<openvdb::FloatGrid>( base_grid ) ) {
				return create_slice_stack_from_grid<openvdb::FloatGrid>( float_grid, temp_root, stack_info, error_message, error_message_size );
			}
			if( openvdb::DoubleGrid::Ptr double_grid = openvdb::gridPtrCast<openvdb::DoubleGrid>( base_grid ) ) {
				return create_slice_stack_from_grid<openvdb::DoubleGrid>( double_grid, temp_root, stack_info, error_message, error_message_size );
			}
		} catch( const std::exception& exc ) {
			write_error( error_message, error_message_size, exc.what() );
			return false;
		}

		write_error( error_message, error_message_size, "RISE only supports float or double VDB density grids for heterogeneous media" );
		return false;
	}
#endif

	bool configure_camera( RISE::IJobPriv& job, const rise_blender_camera& camera, char* error_message, const size_t error_message_size )
	{
		double location[3] = { camera.location[0], camera.location[1], camera.location[2] };
		double forward[3] = { camera.forward[0], camera.forward[1], camera.forward[2] };
		double up[3] = { camera.up[0], camera.up[1], camera.up[2] };
		normalize3( forward );
		normalize3( up );

		double lookat[3] = {
			location[0] + forward[0],
			location[1] + forward[1],
			location[2] + forward[2]
		};
		double orientation[3] = { 0.0, 0.0, 0.0 };
		double target_orientation[2] = { 0.0, 0.0 };

		// Drive Scene's active Film from the Blender render-settings
		// dims BEFORE adding the camera.  Job::Add*Camera reads
		// xres/yres/pixelAR from the active Film at construction; no
		// need to thread them through the call.
		job.SetFilm( camera.width, camera.height, camera.pixel_aspect );

		if( camera.projection_type == RISE_BLENDER_CAMERA_ORTHOGRAPHIC ) {
			const double square_dimension = std::max<unsigned int>( camera.width, camera.height );
			const double vp_scale[2] = {
				double( camera.ortho_scale ) * ( double( camera.width ) / square_dimension ) * 0.5,
				double( camera.ortho_scale ) * ( double( camera.height ) / square_dimension ) * 0.5
			};

			if( !job.AddOrthographicCamera(
				"default",
				location,
				lookat,
				up,
				vp_scale,
				0.0,
				0.0,
				0.0,
				orientation,
				target_orientation ) )
			{
				write_error( error_message, error_message_size, "Failed to configure the orthographic camera" );
				return false;
			}

			return true;
		}

		// Realistic-camera dispatch (ABI v7): when Blender's DOF is
		// enabled on the camera data, route through AddThinlensCamera
		// so focal length / sensor size / fstop / focus distance /
		// aperture blades / anamorphic squeeze / tilt-shift all apply.
		// Otherwise the pinhole path preserves the v6 behaviour bit-
		// identically — fov_y already encodes (focal, sensor) for
		// the simple optical model.
		//
		// ISO / fstop / shift can ALSO drive the pinhole path: when
		// iso > 0 we forward iso + fstop into AddPinholeCamera for
		// the EV-compensation pipeline (Landing 5), so users who
		// want physical exposure without DOF still get it.  The
		// shift_x/shift_y mm-conversion uses sensor_width as the
		// reference dimension (Blender stores shift as a fraction
		// of the sensor dimension, regardless of sensor_fit).
		const bool use_thinlens = ( camera.use_dof != 0 );
		const double scene_unit_meters =
			( camera.scene_unit_meters > 0.0f )
				? double( camera.scene_unit_meters )
				: 1.0;

		if( use_thinlens ) {
			// RISE's AddThinlensCamera convention is "sensor_size is
			// the horizontal extent and the image is fit horizontally"
			// (see ThinLensCamera.cpp ~line 153).  Cycles supports
			// three fit modes; we translate each to the equivalent
			// horizontal sensor extent so RISE's renderer reproduces
			// Cycles' framing 1:1.
			//
			//   HORIZONTAL fit: pass sensor_width directly.
			//   VERTICAL   fit: pass sensor_height * display_aspect so
			//                   the image's horizontal extent ends up
			//                   = sensor_height * display_aspect and
			//                   vertical = sensor_height. ✓
			//   AUTO       fit: Blender's `BKE_camera_sensor_fit` picks
			//                   by RENDER GATE aspect (pixel-aspect-
			//                   aware), not physical sensor aspect.
			//                   The Python exporter resolves AUTO to
			//                   HORIZONTAL/VERTICAL via that rule
			//                   before sending us the answer.
			//
			// The "display aspect" used here MUST include pixel_aspect
			// because RISE's camera transform later pre-stretches X
			// by pixelAR via ComputeScaleFromAR — see ThinLensCamera
			// imageAspect derivation for the matching consumer side.
			const double image_w = ( camera.width > 0 ) ? double( camera.width ) : 1.0;
			const double image_h = ( camera.height > 0 ) ? double( camera.height ) : 1.0;
			const double pixel_ar = ( camera.pixel_aspect > 0.0f )
				? double( camera.pixel_aspect ) : 1.0;
			const double display_aspect = ( image_w * pixel_ar ) / image_h;
			const double sensor_w_mm = double( camera.sensor_width_mm );
			const double sensor_h_mm = double( camera.sensor_height_mm );
			const double sensor_size_mm =
				( camera.sensor_fit_vertical != 0 )
					? ( sensor_h_mm * display_aspect )
					: sensor_w_mm;

			// Blender / Cycles store lens shift as a fraction of the
			// FITTED sensor dimension, applied to BOTH X and Y — not
			// against separate physical sensor axes.  See Blender's
			// `view_plane_compute` (source/blender/blenkernel/intern/
			// camera.cc): `dx = sensor_size_fit * cam->shiftx; dy =
			// sensor_size_fit * cam->shifty;`  Earlier code used
			// sensor_w for shift_x and sensor_h for shift_y, which
			// under-shifted Y on horizontal fit and over-shifted X on
			// vertical fit — adversarial review round 8, P1.
			const double sensor_size_fit_mm = ( camera.sensor_fit_vertical != 0 )
				? sensor_h_mm
				: sensor_w_mm;
			const double shift_x_mm =
				double( camera.shift_x ) * sensor_size_fit_mm;
			const double shift_y_mm =
				double( camera.shift_y ) * sensor_size_fit_mm;
			const double aperture_ratio =
				( camera.aperture_ratio > 0.0f )
					? double( camera.aperture_ratio )
					: 1.0;

			if( !job.AddThinlensCamera(
				"default",
				location,
				lookat,
				up,
				sensor_size_mm,
				double( camera.focal_length_mm ),
				double( camera.fstop ),
				double( camera.focus_distance ),
				scene_unit_meters,
				0.0,	// exposure (Cycles has no native shutter time → 0)
				0.0,	// scanningRate
				0.0,	// pixelRate
				orientation,
				target_orientation,
				static_cast<unsigned int>( camera.aperture_blades < 0 ? 0 : camera.aperture_blades ),
				double( camera.aperture_rotation_radians ),
				aperture_ratio,
				0.0,	// tiltX — Blender's camera has no native tilt
				0.0,	// tiltY
				shift_x_mm,
				shift_y_mm,
				double( camera.iso ) ) )
			{
				write_error( error_message, error_message_size,
					"Failed to configure the thin-lens (DOF) camera" );
				return false;
			}

			return true;
		}

		// Pinhole fallback — forward iso / fstop so the EV pipeline
		// still applies when the user wants physical exposure without
		// aperture-disc DOF.
		if( !job.AddPinholeCamera(
			"default",
			location,
			lookat,
			up,
			camera.fov_y_radians,
			0.0,
			0.0,
			0.0,
			orientation,
			target_orientation,
			double( camera.iso ),
			double( camera.fstop ) ) )
		{
			write_error( error_message, error_message_size, "Failed to configure the perspective camera" );
			return false;
		}

		return true;
	}

	bool add_painter(
		RISE::IJobPriv& job,
		const rise_blender_painter& painter,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !painter.name || !painter.name[0] ) {
			write_error( error_message, error_message_size, "Encountered a painter with no name" );
			return false;
		}

		double color[3] = { painter.color[0], painter.color[1], painter.color[2] };
		double scale[3] = { painter.scale[0], painter.scale[1], painter.scale[2] };
		double shift[3] = { painter.shift[0], painter.shift[1], painter.shift[2] };
		const char filter_type = static_cast<char>( painter.filter_type );
		const char color_space = texture_color_space_value( painter.color_space );

		switch( painter.kind )
		{
		case RISE_BLENDER_PAINTER_UNIFORM:
			if( !job.AddUniformColorPainter( painter.name, color, uniform_color_space_name( painter.color_space ) ) ) {
				write_error( error_message, error_message_size, "Failed to create a uniform painter" );
				return false;
			}
			return true;

		case RISE_BLENDER_PAINTER_TEXTURE_PNG:
			if( !painter.path || !job.AddPNGTexturePainter( painter.name, painter.path, color_space, filter_type, painter.lowmemory != 0, scale, shift ) ) {
				write_error( error_message, error_message_size, "Failed to create a PNG texture painter" );
				return false;
			}
			return true;

		case RISE_BLENDER_PAINTER_TEXTURE_EXR:
#ifdef NO_EXR_SUPPORT
			write_error( error_message, error_message_size, "This build of RISE does not include EXR texture support" );
			return false;
#else
			if( !painter.path || !job.AddEXRTexturePainter( painter.name, painter.path, color_space, filter_type, painter.lowmemory != 0, scale, shift ) ) {
				write_error( error_message, error_message_size, "Failed to create an EXR texture painter" );
				return false;
			}
			return true;
#endif

		case RISE_BLENDER_PAINTER_TEXTURE_HDR:
			if( !painter.path || !job.AddHDRTexturePainter( painter.name, painter.path, filter_type, painter.lowmemory != 0, scale, shift ) ) {
				write_error( error_message, error_message_size, "Failed to create an HDR texture painter" );
				return false;
			}
			return true;

		case RISE_BLENDER_PAINTER_TEXTURE_TIFF:
#ifdef NO_TIFF_SUPPORT
			write_error( error_message, error_message_size, "This build of RISE does not include TIFF texture support" );
			return false;
#else
			if( !painter.path || !job.AddTIFFTexturePainter( painter.name, painter.path, color_space, filter_type, painter.lowmemory != 0, scale, shift ) ) {
				write_error( error_message, error_message_size, "Failed to create a TIFF texture painter" );
				return false;
			}
			return true;
#endif

		case RISE_BLENDER_PAINTER_BLEND:
			if( !painter.painter_a_name || !painter.painter_b_name || !painter.mask_painter_name ||
				!job.AddBlendPainter( painter.name, painter.painter_a_name, painter.painter_b_name, painter.mask_painter_name ) )
			{
				write_error( error_message, error_message_size, "Failed to create a blend painter" );
				return false;
			}
			return true;

		case RISE_BLENDER_PAINTER_TEXTURE_JPEG:
			if( !painter.path || !job.AddJPEGTexturePainter( painter.name, painter.path, color_space, filter_type, painter.lowmemory != 0, scale, shift ) ) {
				write_error( error_message, error_message_size, "Failed to create a JPEG texture painter" );
				return false;
			}
			return true;

		case RISE_BLENDER_PAINTER_UV_TRANSFORM:
			// Wraps a source painter (named by `painter_a_name`) in a
			// UV-space affine transform.  Used for Blender Mapping
			// nodes (vector_type=POINT) between a Texture Coordinate
			// and an Image Texture node; the {offset_u, offset_v,
			// rotation, scale_u, scale_v} tuple comes from the
			// Mapping node's Location.xy / Rotation.z / Scale.xy.
			if( !painter.painter_a_name ||
				!job.AddUVTransformPainter(
					painter.name,
					painter.painter_a_name,
					painter.uv_offset_u,
					painter.uv_offset_v,
					painter.uv_rotation,
					painter.uv_scale_u,
					painter.uv_scale_v ) )
			{
				write_error( error_message, error_message_size, "Failed to create a UV transform painter" );
				return false;
			}
			return true;
		}

		write_error( error_message, error_message_size, "Encountered an unsupported painter type" );
		return false;
	}

	bool add_modifier(
		RISE::IJobPriv& job,
		const rise_blender_modifier& modifier,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !modifier.name || !modifier.source_painter_name ) {
			write_error( error_message, error_message_size, "Encountered an invalid modifier payload" );
			return false;
		}

		switch( modifier.kind )
		{
		case RISE_BLENDER_MODIFIER_BUMP:
			{
				// `IJob::AddBumpMapModifier`'s SIGNATURE is ABI-frozen (it has no
				// `normalize` parameter) and always builds the legacy
				// window-COUPLED fold (RISE_API_CreateBumpMapModifier ==
				// ...Ex(..., normalizeGradient=false)) -- see Job.cpp /
				// RISE_API.cpp.  That fold made the exporter's Bump-node
				// amplitude ~200x weaker once `_build_bump_modifier` shrank its
				// half-step `window` from 1.0 (the whole UV range -- a bug) to
				// 0.005 (a texel-scale finite-difference step), because the
				// delivered tilt is `scale*2*window*grad`.  Bypass the ABI-frozen
				// shim here and reach `RISE_API_CreateBumpMapModifierEx` directly
				// with normalizeGradient=modifier.normalize, resolving/registering
				// through IJobPriv exactly the way the old registry-parser
				// `normalize_gradient TRUE` path did -- the same
				// resolve-via-manager / Ex-create / AddItem-then-release shape
				// used elsewhere in this file (e.g. the hair scalar-painter
				// wrapper above).
				RISE::IFunction2D* pFunc = job.GetFunction2Ds()->GetItem( modifier.source_painter_name );
				if( !pFunc ) {
					write_error( error_message, error_message_size, "Failed to create a bump modifier: source painter/function not found" );
					return false;
				}

				RISE::IRayIntersectionModifier* pModifier = 0;
				RISE::RISE_API_CreateBumpMapModifierEx(
					&pModifier, *pFunc, modifier.scale, modifier.window, modifier.normalize != 0 );
				if( !pModifier ) {
					write_error( error_message, error_message_size, "Failed to create a bump modifier" );
					return false;
				}

				const bool added = job.GetModifiers()->AddItem( pModifier, modifier.name );
				pModifier->release();
				if( !added ) {
					write_error( error_message, error_message_size, "Failed to create a bump modifier" );
					return false;
				}
			}
			return true;

		case RISE_BLENDER_MODIFIER_NORMAL_MAP:
			// AddNormalMapModifier(name, painter, scale) — glTF
			// normalTexture.scale.  The painter must have been
			// registered with ROMMRGB_Linear colour space so the
			// encoded surface tangent stays bit-exact.
			if( !job.AddNormalMapModifier( modifier.name, modifier.source_painter_name, modifier.scale ) ) {
				write_error( error_message, error_message_size, "Failed to create a normal-map modifier" );
				return false;
			}
			return true;
		}

		write_error( error_message, error_message_size, "Encountered an unsupported modifier type" );
		return false;
	}

	bool create_surface_material(
		RISE::IJobPriv& job,
		const rise_blender_material& material,
		const char* material_name,
		char* error_message,
		const size_t error_message_size
	)
	{
		switch( material.model )
		{
		case RISE_BLENDER_MATERIAL_LAMBERT:
			if( material.diffuse_painter_name && job.AddLambertianMaterial( material_name, material.diffuse_painter_name ) ) {
				return true;
			}
			write_error( error_message, error_message_size, "Failed to create a Lambertian material" );
			return false;

		case RISE_BLENDER_MATERIAL_GGX:
			if( material.diffuse_painter_name && material.specular_painter_name && material.alpha_x_painter_name &&
				material.alpha_y_painter_name && material.ior_painter_name && material.extinction_painter_name &&
				job.AddGGXMaterial(
					material_name,
					material.diffuse_painter_name,
					material.specular_painter_name,
					material.alpha_x_painter_name,
					material.alpha_y_painter_name,
					material.ior_painter_name,
					material.extinction_painter_name ) )
			{
				return true;
			}
			write_error( error_message, error_message_size, "Failed to create a GGX material" );
			return false;

		case RISE_BLENDER_MATERIAL_DIELECTRIC:
			if( material.tau_painter_name && material.ior_painter_name && material.scatter_painter_name &&
				job.AddDielectricMaterial(
					material_name,
					material.tau_painter_name,
					material.ior_painter_name,
					material.scatter_painter_name,
					false ) )
			{
				return true;
			}
			write_error( error_message, error_message_size, "Failed to create a dielectric material" );
			return false;
		}

		write_error( error_message, error_message_size, "Encountered an unsupported material model" );
		return false;
	}

	bool add_pbr_metallic_roughness_material(
		RISE::IJobPriv& job,
		const rise_blender_material& material,
		char* error_message,
		const size_t error_message_size
	)
	{
		// AddPBRMetallicRoughnessMaterial bakes emission into the
		// material itself, so we don't need the Luminaire wrapper
		// here.  Painter slots default to RISE's documented sentinels
		// when the bridge passes null: specular_factor="1.0",
		// specular_color="none" (untinted), anisotropy_factor="0.0",
		// anisotropy_rotation="0.0".  emissive="none" disables.
		const char* base_color = material.base_color_painter_name;
		const char* metallic = material.metallic_painter_name;
		const char* roughness = material.roughness_painter_name;
		if( !base_color || !metallic || !roughness ) {
			write_error( error_message, error_message_size, "PBR metallic-roughness material missing base_color/metallic/roughness" );
			return false;
		}

		const char* emissive = ( material.emission_painter_name && material.emission_painter_name[0] )
			? material.emission_painter_name : "none";
		const char* specular_factor = ( material.specular_factor_painter_name && material.specular_factor_painter_name[0] )
			? material.specular_factor_painter_name : "1.0";
		const char* specular_color = ( material.specular_color_painter_name && material.specular_color_painter_name[0] )
			? material.specular_color_painter_name : "none";
		const char* anisotropy_factor = ( material.anisotropy_factor_painter_name && material.anisotropy_factor_painter_name[0] )
			? material.anisotropy_factor_painter_name : "0.0";
		const char* anisotropy_rotation = ( material.anisotropy_rotation_painter_name && material.anisotropy_rotation_painter_name[0] )
			? material.anisotropy_rotation_painter_name : "0.0";

		if( !job.AddPBRMetallicRoughnessMaterial(
			material.name,
			base_color,
			metallic,
			roughness,
			1.5,                              // ior — preserved for API stability, ignored
			emissive,
			material.emissive_scale > 0.0 ? material.emissive_scale : 1.0,
			specular_factor,
			specular_color,
			anisotropy_factor,
			anisotropy_rotation ) )
		{
			write_error( error_message, error_message_size, "Failed to create a PBR metallic-roughness material" );
			return false;
		}
		return true;
	}

	bool add_material(
		RISE::IJobPriv& job,
		const rise_blender_material& material,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !material.name || !material.name[0] ) {
			write_error( error_message, error_message_size, "Encountered a material with no name" );
			return false;
		}

		// PBR-MR handles its own emission inside the factory — no
		// Luminaire wrapper needed.
		if( material.model == RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS ) {
			return add_pbr_metallic_roughness_material( job, material, error_message, error_message_size );
		}

		if( material.emission_painter_name && material.emission_painter_name[0] ) {
			const std::string surface_name = std::string( material.name ) + "::surface";
			if( !create_surface_material( job, material, surface_name.c_str(), error_message, error_message_size ) ) {
				return false;
			}
			if( !job.AddLambertianLuminaireMaterial( material.name, material.emission_painter_name, surface_name.c_str(), 1.0 ) ) {
				write_error( error_message, error_message_size, "Failed to create an emissive material wrapper" );
				return false;
			}
			return true;
		}

		return create_surface_material( job, material, material.name, error_message, error_message_size );
	}

	bool add_mesh(
		RISE::IJobPriv& job,
		const rise_blender_mesh& mesh,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !validate_mesh_payload( mesh, error_message, error_message_size ) ) {
			return false;
		}

		if( !job.AddIndexedTriangleMeshGeometry(
			mesh.name,
			mesh.vertices,
			mesh.normals,
			mesh.uvs,
			mesh.vertex_indices,
			mesh.uv_indices,
			mesh.normal_indices,
			mesh.num_vertices,
			mesh.num_normals,
			mesh.num_uvs,
			mesh.num_triangles,
			mesh.double_sided != 0,
			mesh.use_face_normals != 0 ) )
		{
			write_error( error_message, error_message_size, "Failed to add indexed triangle mesh geometry to the RISE job" );
			return false;
		}

		return true;
	}

	bool add_object(
		RISE::IJobPriv& job,
		const rise_blender_object& object,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !object.name || !object.geometry_name || !object.material_name ) {
			write_error( error_message, error_message_size, "Encountered an object with missing references" );
			return false;
		}

		double zero[3] = { 0.0, 0.0, 0.0 };
		double one[3] = { 1.0, 1.0, 1.0 };

		RISE::RadianceMapConfig object_radiance_map;

		if( !job.AddObject(
			object.name,
			object.geometry_name,
			object.material_name,
			object.modifier_name,
			0,
			object_radiance_map,
			zero,
			zero,
			one,
			object.casts_shadows != 0,
			object.receives_shadows != 0 ) )
		{
			write_error( error_message, error_message_size, "Failed to create an object instance in the RISE job" );
			return false;
		}

		RISE::IObjectPriv* rise_object = job.GetObjects()->GetItem( object.name );
		if( !rise_object ) {
			write_error( error_message, error_message_size, "Failed to retrieve the just-created RISE object" );
			return false;
		}

		rise_object->ClearAllTransforms();
		rise_object->PushTopTransStack( blender_matrix_to_rise_matrix( object.transform ) );
		rise_object->FinalizeTransformations();
		rise_object->SetWorldVisible( object.visible != 0 );
		return true;
	}

	//
	// Hair / fur (ABI v9; the texture-driven scalar slots are v10).  See
	// rise_blender_hair_material / rise_blender_hair_object in the
	// header for the struct contract, and
	// docs/BLENDER_MATERIAL_TRANSLATION.md's hair section for the
	// Blender-side translation these two consume.
	//
	// EVERYTHING IN THIS BLOCK IS NON-FATAL.  Unlike add_material /
	// add_mesh / add_object above -- which write into `error_message`
	// and abort the whole render -- a hair failure records a warning
	// and skips exactly one groom.  A `.hair` path that went stale
	// between the export and the render (a cleared temp dir, a
	// half-written file) is a routine, recoverable condition; killing a
	// frame the other 400 objects rendered fine over it would be the
	// wrong trade.  The warnings reach the artist through
	// rise_blender_render_result.warnings.
	//
	// The v10 texture-scalar slots degrade one step FURTHER than that:
	// an unresolvable `*_texture_painter_name` does not even skip the
	// material, it falls back to that slot's numeric field and warns.
	// A missing roughness map is a reason to render the groom at its
	// constant roughness, not a reason to lose the groom.
	//

	// Blender-parity rescale for the two melanin concentrations.
	//
	// Cycles and RISE both take (eumelanin, pheomelanin) CONCENTRATIONS
	// and multiply them by a per-pigment sigma_a triple, but the triples
	// differ (docs/BLENDER_MATERIAL_TRANSLATION.md, "Units disclosure"):
	//
	//     pigment      Cycles                  RISE (OMLC, G-anchored)
	//     eumelanin    (0.506, 0.841, 1.653)   (0.518, 0.697, 1.293)
	//     pheomelanin  (0.343, 0.733, 1.924)   (0.232, 0.400, 1.067)
	//
	// At equal concentration RISE absorbs ~17 % less green for eumelanin
	// and ~45 % less for pheomelanin, so the same Melanin / Melanin
	// Redness sliders read lighter and less saturated here than in a
	// Cycles reference.  These two ratios are the green-channel
	// correction that puts the two renderers back on top of each other.
	//
	// GREEN IS THE ANCHOR, and that is the whole approximation: one
	// scalar per pigment cannot match all three channels (the R and B
	// ratios differ -- eumelanin R is 0.977, B is 1.278), so the rescale
	// restores luminance-level parity, not a per-channel match.  Green
	// carries most of the luminance and is the channel RISE's own
	// triples are anchored on, which is why it is the one chosen.
	//
	// DEFAULT ON, decided in this slice (P2-D): the add-on's job is to
	// render the artist's Blender scene, and an artist who dialled a
	// groom in against Cycles' viewport expects that groom back.  The
	// physical-anchoring alternative would silently re-light their
	// asset.  Per-material and switchable: clear
	// `apply_melanin_parity_rescale` to get RISE's own OMLC-anchored
	// absorption instead.  A future .RISEscene export path would NOT
	// apply it -- a scene file is authored in RISE's own units.
	const double kEumelaninBlenderParityScale   = 0.841 / 0.697;	// ~1.2066
	const double kPheomelaninBlenderParityScale = 0.733 / 0.400;	// ~1.8325

	void record_warning( std::vector<std::string>& warnings, const std::string& message )
	{
		warnings.push_back( message );
		// Also into the RISE log, so a headless / non-Blender caller
		// that ignores the result struct's warnings still sees it.
		RISE::GlobalLog()->PrintEx( RISE::eLog_Warning, "%s", message.c_str() );
	}

	std::string quoted_name( const char* name )
	{
		return std::string( "'" ) + ( name && name[0] ? name : "(unnamed)" ) + "'";
	}

	// Format a float as an inline numeric literal for one of
	// `AddHairMaterial`'s IScalarPainter slots.  %.9g round-trips every
	// float exactly, and Job's own parser (ResolveScalarPainterArg)
	// reads it back with strtod.
	std::string scalar_literal( const float value )
	{
		char buffer[64];
		std::snprintf( buffer, sizeof( buffer ), "%.9g", double( value ) );
		return std::string( buffer );
	}

	std::string rgb_literal( const float value[3] )
	{
		char buffer[192];
		std::snprintf( buffer, sizeof( buffer ), "%.9g %.9g %.9g",
			double( value[0] ), double( value[1] ), double( value[2] ) );
		return std::string( buffer );
	}

	//! Suffix that turns a colour-painter name into the name of its
	//! hair-scalar wrapper.  The two live in DIFFERENT managers (an
	//! `IPainterManager` and an `IScalarPainterManager`), so the suffix
	//! is not needed to avoid a clash between them -- it is there so
	//! that the wrapper is unmistakably bridge-private and cannot be
	//! confused with, or collide with, a scalar painter some other code
	//! path registered under a plain name.  `::`-qualified derived names
	//! are this bridge's existing idiom (`<object>::hairgeom`).  The
	//! incoming painter name is itself unique -- it came out of the
	//! exporter's `_unique_name` / painter-registration path and has
	//! already been accepted by `IPainterManager::AddItem`, which
	//! refuses duplicates -- so the derived name is unique too.
	const char* const kHairScalarWrapperSuffix = "::hairscalar";

	//! Resolve one OPTIONAL v10 texture-driven IScalarPainter slot on a
	//! hair material to the string `IJob::AddHairMaterial` should be
	//! handed for it.
	//!
	//! `Job::AddHairMaterial` resolves every scalar slot by looking the
	//! string up in the job's IScalarPainterManager FIRST and only then
	//! parsing it as an inline number (Job.cpp's
	//! `ResolveOrDiagnoseScalar` / `ResolveScalarPainterArg`), so the
	//! job of this function is to get a suitable `IScalarPainter`
	//! registered under a name and return that name.
	//!
	//! The wrapper is `RISE_API_CreatePainterChannelScalarPainter`
	//! (`PainterChannelScalarPainter`), NOT the older
	//! `PainterToScalarAdapter`: it is the general, already-shipped
	//! "wrap a named colour painter as a named scalar painter" surface
	//! the scene language's `scalar_painter { painter <name> }` chunk is
	//! built on (ChunkParserRegistry.cpp, doc 88 P2.1/S3), it supports
	//! channel selection and an affine remap should this ever need them,
	//! and it carries the SAME post-colourspace caveat as the adapter.
	//! CHANNEL R (0), matching both that chunk's default channel and
	//! `PainterToScalarAdapter`'s fixed channel: the exporter registers
	//! these maps as non-colour data (`colorspace_is_data=True` ->
	//! linear), i.e. a single physical value replicated across the
	//! channels, so R is the value.  scale 1 / bias 0 -- the numeric
	//! remap the artist wants already lives in the image.
	//!
	//! CAVEAT, inherited from the wrapper and worth repeating: the value
	//! read is POST-colourspace, whatever the source painter was
	//! registered with.  For the data-linear maps the exporter sends
	//! that is the texel value verbatim, which is the intent.
	//!
	//! NON-FATAL throughout, matching this whole block: an empty field,
	//! an unresolvable name, or a failure to build/register the wrapper
	//! all return `fallbackLiteral` (the slot's numeric field, already
	//! formatted) so the material still registers with its constant
	//! value.
	std::string resolve_hair_scalar_slot(
		RISE::IJobPriv& job,
		const char* painterName,
		const char* slotLabel,
		const std::string& who,
		const std::string& fallbackLiteral,
		std::vector<std::string>& warnings
	)
	{
		if( !painterName || !painterName[0] ) {
			return fallbackLiteral;			// the common case: no texture bound
		}

		RISE::IPainterManager* pmgr = job.GetPainters();
		RISE::IScalarPainterManager* smgr = job.GetScalarPainters();
		RISE::IPainter* source = pmgr ? pmgr->GetItem( painterName ) : 0;

		if( !source || !smgr ) {
			record_warning( warnings,
				std::string( "RISE could not bind the texture named '" ) + painterName +
				"' to hair material " + who + "'s " + slotLabel +
				"; that one parameter falls back to its constant value." );
			return fallbackLiteral;
		}

		const std::string wrapperName = std::string( painterName ) + kHairScalarWrapperSuffix;

		// Two hair materials may legitimately share one texture painter
		// (a groom split across several Blender materials that all point
		// at the same roughness map).  The wrapper is fully determined by
		// the source painter plus the fixed channel/scale/bias, so the
		// second material reuses the first's rather than failing on
		// AddItem's duplicate-name refusal.
		if( !smgr->GetItem( wrapperName.c_str() ) ) {
			RISE::IScalarPainter* wrapper = 0;
			RISE::RISE_API_CreatePainterChannelScalarPainter(
				&wrapper, *source, 0, RISE::Scalar( 1.0 ), RISE::Scalar( 0.0 ) );
			if( !wrapper ) {
				record_warning( warnings,
					std::string( "RISE could not build a scalar view of texture '" ) + painterName +
					"' for hair material " + who + "'s " + slotLabel +
					"; that one parameter falls back to its constant value." );
				return fallbackLiteral;
			}

			// AddItem takes its OWN reference (GenericManager::AddItem
			// addrefs), so the local one is released either way -- the
			// same add-then-release pairing ChunkParserRegistry.cpp's
			// scalar_painter chunk uses.
			const bool added = smgr->AddItem( wrapper, wrapperName.c_str() );
			wrapper->release();

			if( !added ) {
				record_warning( warnings,
					std::string( "RISE could not register a scalar view of texture '" ) + painterName +
					"' for hair material " + who + "'s " + slotLabel +
					"; that one parameter falls back to its constant value." );
				return fallbackLiteral;
			}
		}

		return wrapperName;
	}

	// Pack the accumulated non-fatal warnings into the fixed result
	// buffer: newline separated, NUL terminated, and truncated with a
	// visible note rather than silently cut mid-sentence.  Factored out
	// of rise_blender_render_scene so it is reachable from a test
	// without driving a whole render.
	void pack_warnings( const std::vector<std::string>& warnings, char* out, const size_t capacity )
	{
		if( !out || capacity == 0 ) {
			return;
		}

		std::string joined;
		for( size_t i = 0; i < warnings.size(); ++i ) {
			if( !joined.empty() ) {
				joined += '\n';
			}
			joined += warnings[i];
		}

		const char* truncation_note = "\n(further warnings omitted; see the RISE log)";
		const size_t note_length = std::strlen( truncation_note );
		if( joined.size() >= capacity ) {
			// capacity-1 usable bytes, minus room for the note.  A
			// buffer too small to hold even the note degrades to a plain
			// snprintf truncation below.
			if( capacity > note_length + 1 ) {
				// Note: this resize() is a byte-count cut and can land
				// mid-codepoint if a warning contains multi-byte UTF-8
				// (e.g. a Blender object/material name with non-ASCII
				// characters) right at the boundary.  Not fixed up here:
				// the Python side decodes with errors="replace"
				// (bridge.py's `_decode_bridge_warnings`), which turns a
				// split sequence into a single U+FFFD instead of raising.
				joined.resize( capacity - 1 - note_length );
				joined += truncation_note;
			}
		}

		std::snprintf( out, capacity, "%s", joined.c_str() );
	}

	bool add_hair_material(
		RISE::IJobPriv& job,
		const rise_blender_hair_material& material,
		std::vector<std::string>& warnings
	)
	{
		if( !material.name || !material.name[0] ) {
			record_warning( warnings, "RISE skipped a hair material with no name." );
			return false;
		}

		const std::string who = quoted_name( material.name );

		// Non-finite values would reach AddHairMaterial as the literal
		// spellings "inf" / "nan", which its resolver rejects at the
		// string layer with a diagnostic that doesn't name Blender.
		// Catch them here so the artist gets a message about their
		// material instead.
		if( !std::isfinite( material.beta_m ) || !std::isfinite( material.beta_n ) ||
			!std::isfinite( material.alpha_degrees ) || !std::isfinite( material.ior ) )
		{
			record_warning( warnings,
				"RISE skipped hair material " + who +
				": one of beta_m / beta_n / alpha / ior is not a finite number." );
			return false;
		}

		std::string color       = "none";
		std::string sigma_a     = "none";
		std::string eumelanin   = "none";
		std::string pheomelanin = "none";

		switch( material.tier )
		{
		case RISE_BLENDER_HAIR_TIER_MELANIN:
		{
			if( !std::isfinite( material.eumelanin ) || !std::isfinite( material.pheomelanin ) ||
				material.eumelanin < 0.0f || material.pheomelanin < 0.0f )
			{
				record_warning( warnings,
					"RISE skipped hair material " + who +
					": eumelanin / pheomelanin must be finite and non-negative concentrations." );
				return false;
			}

			double eu = double( material.eumelanin );
			double ph = double( material.pheomelanin );
			if( material.apply_melanin_parity_rescale ) {
				eu *= kEumelaninBlenderParityScale;
				ph *= kPheomelaninBlenderParityScale;
			}
			eumelanin   = scalar_literal( float( eu ) );
			pheomelanin = scalar_literal( float( ph ) );
			break;
		}

		case RISE_BLENDER_HAIR_TIER_SIGMA_A:
		{
			if( !std::isfinite( material.sigma_a[0] ) || !std::isfinite( material.sigma_a[1] ) ||
				!std::isfinite( material.sigma_a[2] ) )
			{
				record_warning( warnings,
					"RISE skipped hair material " + who + ": sigma_a is not a finite RGB triple." );
				return false;
			}
			if( material.sigma_a[0] < 0.0f || material.sigma_a[1] < 0.0f || material.sigma_a[2] < 0.0f )
			{
				record_warning( warnings,
					"RISE skipped hair material " + who +
					": sigma_a must be non-negative absorption coefficients (a negative value would amplify energy)." );
				return false;
			}
			sigma_a = rgb_literal( material.sigma_a );
			break;
		}

		case RISE_BLENDER_HAIR_TIER_COLOR:
		{
			if( !material.color_painter_name || !material.color_painter_name[0] ) {
				record_warning( warnings,
					"RISE skipped hair material " + who +
					": the colour tier is selected but no colour painter is bound." );
				return false;
			}
			color = material.color_painter_name;
			break;
		}

		default:
			record_warning( warnings,
				"RISE skipped hair material " + who + ": unrecognised colour tier." );
			return false;
		}

		// v10: three of the shared scalar slots may be driven by a
		// texture instead of a number.  Each resolves to a registered
		// IScalarPainter NAME when a painter is bound and resolvable, and
		// to the numeric literal otherwise -- including on a name the job
		// cannot resolve, which warns but does not lose the material.
		// `alpha_degrees` has no texture form by design (Blender's
		// `Offset` socket is never texture-sampled by the exporter).
		const std::string beta_m = resolve_hair_scalar_slot(
			job, material.beta_m_texture_painter_name, "beta_m (Roughness)",
			who, scalar_literal( material.beta_m ), warnings );
		const std::string beta_n = resolve_hair_scalar_slot(
			job, material.beta_n_texture_painter_name, "beta_n (Radial Roughness)",
			who, scalar_literal( material.beta_n ), warnings );
		const std::string alpha  = scalar_literal( material.alpha_degrees );
		const std::string ior    = resolve_hair_scalar_slot(
			job, material.ior_texture_painter_name, "ior (IOR)",
			who, scalar_literal( material.ior ), warnings );

		if( !job.AddHairMaterial(
			material.name,
			color.c_str(),
			sigma_a.c_str(),
			eumelanin.c_str(),
			pheomelanin.c_str(),
			beta_m.c_str(),
			beta_n.c_str(),
			alpha.c_str(),
			ior.c_str() ) )
		{
			// AddHairMaterial logged the specific reason (unknown
			// painter, tier-count violation, unparseable literal).
			record_warning( warnings,
				"RISE could not create hair material " + who +
				"; the groom(s) using it will not render (see the RISE log for the reason)." );
			return false;
		}

		return true;
	}

	bool add_hair_object(
		RISE::IJobPriv& job,
		const rise_blender_hair_object& object,
		std::vector<std::string>& warnings
	)
	{
		if( !object.name || !object.name[0] ) {
			record_warning( warnings, "RISE skipped a hair object with no name." );
			return false;
		}

		const std::string who = quoted_name( object.name );

		if( !object.file_path || !object.file_path[0] ) {
			record_warning( warnings, "RISE skipped hair object " + who + ": no .hair file path." );
			return false;
		}

		if( !object.material_name || !object.material_name[0] ) {
			record_warning( warnings, "RISE skipped hair object " + who + ": no hair material bound." );
			return false;
		}

		if( !std::isfinite( object.width_root_scale ) || object.width_root_scale <= 0.0f ||
			!std::isfinite( object.width_tip_scale ) || object.width_tip_scale <= 0.0f )
		{
			record_warning( warnings,
				"RISE skipped hair object " + who +
				": the width multipliers must be finite and greater than zero." );
			return false;
		}

		// Check the material BEFORE importing the file.  Two reasons:
		// a groom whose material failed cannot be placed anyway, and a
		// production `.hair` file is megabytes -- there is no sense
		// paying that read to then fail at AddObject and leave an
		// orphaned geometry registered under a name nothing references.
		if( !job.GetMaterials() || !job.GetMaterials()->GetItem( object.material_name ) ) {
			record_warning( warnings,
				"RISE skipped hair object " + who + ": its hair material '" +
				object.material_name + "' was not created (see the earlier warning for why)." );
			return false;
		}

		// The geometry is an internal registration, one per groom -- the
		// ABI carries no geometry name because a `.hair` file is written
		// per Curves object by the exporter, so there is nothing to
		// share.  Suffixed rather than reusing the object's own name
		// because objects and geometries live in different managers but
		// the same author-visible namespace in diagnostics.
		const std::string geometry_name = std::string( object.name ) + "::hairgeom";

		RISE::HairFileGroomDescriptor descriptor;
		descriptor.file = object.file_path;
		descriptor.widthRootScale = double( object.width_root_scale );
		descriptor.widthTipScale = double( object.width_tip_scale );

		if( !job.AddHairGeometryFromFile( geometry_name.c_str(), descriptor ) ) {
			// HairFileLoader logged the specific reason: missing file,
			// bad magic, truncated arrays, counts over its caps, every
			// strand rejected.
			record_warning( warnings,
				"RISE could not import the .hair file for " + who + " ('" + object.file_path +
				"'); that groom will not render (see the RISE log for the reason)." );
			return false;
		}

		// Bind through the SAME object path a mesh takes -- transform
		// convention, visibility and shadow flags included -- rather
		// than a parallel implementation that could drift from it.
		rise_blender_object bound;
		std::memset( &bound, 0, sizeof( bound ) );
		bound.name = object.name;
		bound.geometry_name = geometry_name.c_str();
		bound.material_name = object.material_name;
		std::memcpy( bound.transform, object.transform, sizeof( bound.transform ) );
		bound.casts_shadows = object.casts_shadows;
		bound.receives_shadows = object.receives_shadows;
		bound.visible = object.visible;
		bound.modifier_name = 0;
		bound.interior_medium_name = 0;

		char object_error[512];
		object_error[0] = 0;
		if( !add_object( job, bound, object_error, sizeof( object_error ) ) ) {
			// The geometry above registered successfully; add_object
			// failing (e.g. a duplicate object name) must not leave it
			// behind under a name nothing references.
			job.RemoveGeometry( geometry_name.c_str() );
			record_warning( warnings,
				"RISE could not place hair object " + who + " (material '" + object.material_name +
				"'): " + ( object_error[0] ? object_error : "unknown error" ) + "." );
			return false;
		}

		return true;
	}

	bool add_light(
		RISE::IJobPriv& job,
		const rise_blender_light& light,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !light.name ) {
			write_error( error_message, error_message_size, "Encountered a light with no name" );
			return false;
		}

		// Blender's light.color is linear Rec.709 -- which is exactly
		// what Job::Add*Light take when handed "Rec709RGB_Linear".
		double color[3];
		blender_light_color( light.color, color );
		const char* const kLinear = "Rec709RGB_Linear";

		switch( light.type )
		{
		case RISE_BLENDER_LIGHT_POINT:
		case RISE_BLENDER_LIGHT_AREA:
		{
			double position[3] = { light.position[0], light.position[1], light.position[2] };
			if( !job.AddPointOmniLight( light.name, light.intensity, color, kLinear, position, false ) ) {
				write_error( error_message, error_message_size, "Failed to add a point light" );
				return false;
			}
			return true;
		}
		case RISE_BLENDER_LIGHT_SPOT:
		{
			double position[3] = { light.position[0], light.position[1], light.position[2] };
			double direction[3] = { light.direction[0], light.direction[1], light.direction[2] };
			normalize3( direction );
			double focus[3] = {
				position[0] + direction[0],
				position[1] + direction[1],
				position[2] + direction[2]
			};
			const double outer_angle = clamp_value<float>( light.spot_size, 0.0f, float( kPi ) );
			const double inner_angle = outer_angle * ( 1.0 - clamp_value<float>( light.spot_blend, 0.0f, 1.0f ) );
			if( !job.AddPointSpotLight( light.name, light.intensity, color, kLinear, focus, inner_angle, outer_angle, position, false ) ) {
				write_error( error_message, error_message_size, "Failed to add a spot light" );
				return false;
			}
			return true;
		}
		case RISE_BLENDER_LIGHT_SUN:
		{
			double direction[3] = { -light.direction[0], -light.direction[1], -light.direction[2] };
			normalize3( direction );
			if( !job.AddDirectionalLight( light.name, light.intensity, color, kLinear, direction ) ) {
				write_error( error_message, error_message_size, "Failed to add a directional light" );
				return false;
			}
			return true;
		}
		case RISE_BLENDER_LIGHT_AMBIENT:
		default:
			if( !job.AddAmbientLight( light.name, light.intensity, color, kLinear ) ) {
				write_error( error_message, error_message_size, "Failed to add an ambient light" );
				return false;
			}
			return true;
		}
	}

	bool create_phase_function(
		const rise_blender_medium& medium,
		RISE::IPhaseFunction** phase,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !phase ) {
			write_error( error_message, error_message_size, "Invalid phase function output" );
			return false;
		}

		(*phase) = 0;
		switch( medium.phase_type )
		{
		case RISE_BLENDER_PHASE_HG:
			if( !RISE::RISE_API_CreateHenyeyGreensteinPhaseFunction( phase, clamp_value<double>( medium.phase_g, -0.99, 0.99 ) ) ) {
				write_error( error_message, error_message_size, "Failed to create a Henyey-Greenstein phase function" );
				return false;
			}
			return true;

		case RISE_BLENDER_PHASE_ISOTROPIC:
		default:
			if( !RISE::RISE_API_CreateIsotropicPhaseFunction( phase ) ) {
				write_error( error_message, error_message_size, "Failed to create an isotropic phase function" );
				return false;
			}
			return true;
		}
	}

	bool create_homogeneous_medium(
		const rise_blender_medium& medium,
		const RISE::IMedium** out_medium,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !out_medium || !medium.name ) {
			write_error( error_message, error_message_size, "Encountered an invalid homogeneous medium payload" );
			return false;
		}

		RISE::IPhaseFunction* phase = 0;
		if( !create_phase_function( medium, &phase, error_message, error_message_size ) ) {
			return false;
		}

		RISE::IMedium* created_medium = 0;
		const RISE::RISEPel sigma_a( medium.sigma_a[0], medium.sigma_a[1], medium.sigma_a[2] );
		const RISE::RISEPel sigma_s( medium.sigma_s[0], medium.sigma_s[1], medium.sigma_s[2] );
		const RISE::RISEPel emission( medium.emission[0], medium.emission[1], medium.emission[2] );

		const bool has_emission = color_non_zero( medium.emission );
		const bool ok = has_emission ?
			RISE::RISE_API_CreateHomogeneousMediumWithEmission( &created_medium, sigma_a, sigma_s, emission, *phase ) :
			RISE::RISE_API_CreateHomogeneousMedium( &created_medium, sigma_a, sigma_s, *phase );
		RISE::safe_release( phase );

		if( !ok || !created_medium ) {
			write_error( error_message, error_message_size, "Failed to create a homogeneous medium" );
			return false;
		}

		(*out_medium) = created_medium;
		return true;
	}

	bool create_heterogeneous_medium(
		const rise_blender_medium& medium,
		const rise_blender_render_settings& settings,
		VolumeCacheGuard& cache_guard,
		const RISE::IMedium** out_medium,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( !out_medium || !medium.name ) {
			write_error( error_message, error_message_size, "Encountered an invalid heterogeneous medium payload" );
			return false;
		}

#ifndef RISE_BLENDER_ENABLE_OPENVDB
		(void)settings;
		(void)cache_guard;
		(void)medium;
		write_error( error_message, error_message_size, "This bridge build does not include OpenVDB support for Blender volume objects" );
		return false;
#else
		SliceStackInfo stack_info;
		if( !create_vdb_slice_stack(
			medium.source_filepath,
			medium.source_grid_name,
			settings.temporary_directory,
			stack_info,
			error_message,
			error_message_size ) )
		{
			return false;
		}

		cache_guard.directories.push_back( stack_info.directory );

		RISE::IPhaseFunction* phase = 0;
		if( !create_phase_function( medium, &phase, error_message, error_message_size ) ) {
			return false;
		}

		const double density_scale = stack_info.density_scale;
		const RISE::RISEPel sigma_a(
			medium.sigma_a[0] * density_scale,
			medium.sigma_a[1] * density_scale,
			medium.sigma_a[2] * density_scale );
		const RISE::RISEPel sigma_s(
			medium.sigma_s[0] * density_scale,
			medium.sigma_s[1] * density_scale,
			medium.sigma_s[2] * density_scale );
		const RISE::RISEPel emission(
			medium.emission[0] * density_scale,
			medium.emission[1] * density_scale,
			medium.emission[2] * density_scale );
		const RISE::Point3 bbox_min( medium.bbox_min[0], medium.bbox_min[1], medium.bbox_min[2] );
		const RISE::Point3 bbox_max( medium.bbox_max[0], medium.bbox_max[1], medium.bbox_max[2] );

		RISE::IMedium* created_medium = 0;
		const bool has_emission = color_non_zero( medium.emission );
		const bool ok = has_emission ?
			RISE::RISE_API_CreateHeterogeneousMediumWithEmission(
				&created_medium,
				sigma_a,
				sigma_s,
				emission,
				*phase,
				stack_info.file_pattern.c_str(),
				stack_info.width,
				stack_info.height,
				stack_info.start_z,
				stack_info.end_z,
				't',
				bbox_min,
				bbox_max ) :
			RISE::RISE_API_CreateHeterogeneousMedium(
				&created_medium,
				sigma_a,
				sigma_s,
				*phase,
				stack_info.file_pattern.c_str(),
				stack_info.width,
				stack_info.height,
				stack_info.start_z,
				stack_info.end_z,
				't',
				bbox_min,
				bbox_max );
		RISE::safe_release( phase );

		if( !ok || !created_medium ) {
			write_error( error_message, error_message_size, "Failed to create a heterogeneous medium" );
			return false;
		}

		(*out_medium) = created_medium;
		return true;
#endif
	}

	bool add_volume_boundary_object(
		RISE::IJobPriv& job,
		const rise_blender_medium& medium,
		const RISE::IMedium& medium_instance,
		char* error_message,
		const size_t error_message_size
	)
	{
		const double width = std::max( double( medium.bbox_max[0] - medium.bbox_min[0] ), 1e-6 );
		const double height = std::max( double( medium.bbox_max[1] - medium.bbox_min[1] ), 1e-6 );
		const double depth = std::max( double( medium.bbox_max[2] - medium.bbox_min[2] ), 1e-6 );
		const double center[3] = {
			( medium.bbox_min[0] + medium.bbox_max[0] ) * 0.5,
			( medium.bbox_min[1] + medium.bbox_max[1] ) * 0.5,
			( medium.bbox_min[2] + medium.bbox_max[2] ) * 0.5
		};
		double zero[3] = { 0.0, 0.0, 0.0 };
		double one[3] = { 1.0, 1.0, 1.0 };
		const std::string geometry_name = std::string( medium.name ) + "::boundary_geom";
		const std::string object_name = std::string( medium.name ) + "::boundary_obj";

		if( !job.AddBoxGeometry( geometry_name.c_str(), width, height, depth ) ) {
			write_error( error_message, error_message_size, "Failed to create the heterogeneous volume boundary geometry" );
			return false;
		}

		RISE::RadianceMapConfig volume_object_radiance_map;

		if( !job.AddObject(
			object_name.c_str(),
			geometry_name.c_str(),
			"none",
			0,
			0,
			volume_object_radiance_map,
			center,
			zero,
			one,
			true,
			true ) )
		{
			write_error( error_message, error_message_size, "Failed to create the heterogeneous volume boundary object" );
			return false;
		}

		RISE::IObjectPriv* object = job.GetObjects()->GetItem( object_name.c_str() );
		if( !object || !object->AssignInteriorMedium( medium_instance ) ) {
			write_error( error_message, error_message_size, "Failed to assign the heterogeneous interior medium to its boundary object" );
			return false;
		}

		return true;
	}

	bool create_and_assign_media(
		RISE::IJobPriv& job,
		const rise_blender_scene& scene,
		const rise_blender_render_settings& settings,
		VolumeCacheGuard& cache_guard,
		char* error_message,
		const size_t error_message_size
	)
	{
		std::map<std::string, const RISE::IMedium*> media;

		for( uint32_t i = 0; i < scene.num_mediums; ++i ) {
			const rise_blender_medium& medium = scene.mediums[i];
			if( !medium.name || !medium.name[0] ) {
				write_error( error_message, error_message_size, "Encountered a participating medium with no name" );
				return false;
			}

			const RISE::IMedium* created_medium = 0;
			bool ok = false;
			switch( medium.kind )
			{
			case RISE_BLENDER_MEDIUM_HOMOGENEOUS:
				ok = create_homogeneous_medium( medium, &created_medium, error_message, error_message_size );
				break;
			case RISE_BLENDER_MEDIUM_HETEROGENEOUS_VDB:
				ok = create_heterogeneous_medium( medium, settings, cache_guard, &created_medium, error_message, error_message_size );
				break;
			default:
				write_error( error_message, error_message_size, "Encountered an unsupported medium type" );
				return false;
			}

			if( !ok || !created_medium ) {
				return false;
			}

			media[medium.name] = created_medium;

			if( medium.kind == RISE_BLENDER_MEDIUM_HETEROGENEOUS_VDB ) {
				if( !add_volume_boundary_object( job, medium, *created_medium, error_message, error_message_size ) ) {
					return false;
				}
			}
		}

		if( scene.global_medium_name && scene.global_medium_name[0] ) {
			std::map<std::string, const RISE::IMedium*>::const_iterator found = media.find( scene.global_medium_name );
			if( found == media.end() ) {
				write_error( error_message, error_message_size, "The scene references a missing global medium" );
				return false;
			}
			job.GetScene()->SetGlobalMedium( found->second );
		}

		for( uint32_t i = 0; i < scene.num_objects; ++i ) {
			const rise_blender_object& object = scene.objects[i];
			if( !object.interior_medium_name || !object.interior_medium_name[0] ) {
				continue;
			}

			std::map<std::string, const RISE::IMedium*>::const_iterator found = media.find( object.interior_medium_name );
			if( found == media.end() ) {
				write_error( error_message, error_message_size, "An object references a missing interior medium" );
				return false;
			}

			RISE::IObjectPriv* rise_object = job.GetObjects()->GetItem( object.name );
			if( !rise_object || !rise_object->AssignInteriorMedium( *( found->second ) ) ) {
				write_error( error_message, error_message_size, "Failed to assign an interior medium to an object" );
				return false;
			}
		}

		for( std::map<std::string, const RISE::IMedium*>::const_iterator it = media.begin(); it != media.end(); ++it ) {
			RISE::IMedium* medium = const_cast<RISE::IMedium*>( it->second );
			RISE::safe_release( medium );
		}

		return true;
	}

	bool configure_shader( RISE::IJobPriv& job, const rise_blender_render_settings& settings, char* error_message, const size_t error_message_size )
	{
		// SetPathTracingPelRasterizer bypasses the shader-op chain
		// (the path tracer integrator owns shading), so the named
		// shader is only consulted by the legacy direct path.  We
		// always register a default 4-op stack so the same name
		// resolves for both modes.
		(void)settings;

		const char* shader_ops[4] = {
			"DefaultDirectLighting",
			"DefaultEmission",
			"DefaultReflection",
			"DefaultRefraction"
		};

		if( !job.AddStandardShader( kShaderName, 4, shader_ops ) ) {
			write_error( error_message, error_message_size, "Failed to assemble the RISE shader stack" );
			return false;
		}

		return true;
	}

	void build_sms_config( const rise_blender_render_settings& settings, RISE::SMSConfig& sms_config )
	{
		sms_config.enabled = settings.sms_enabled != 0;
		sms_config.maxIterations = std::max<uint32_t>( settings.sms_max_iterations, 1u );
		sms_config.threshold = settings.sms_threshold > 0.0f ? settings.sms_threshold : 1e-5;
		sms_config.maxChainDepth = std::max<uint32_t>( settings.sms_max_chain_depth, 1u );
		sms_config.biased = settings.sms_biased != 0;
		sms_config.bernoulliTrials = std::max<uint32_t>( settings.sms_bernoulli_trials, 1u );
		sms_config.multiTrials = std::max<uint32_t>( settings.sms_multi_trials, 1u );
		sms_config.photonCount = settings.sms_photon_count;
		sms_config.maxPhotonSeedsPerShadingPoint = settings.sms_max_photon_seeds;
		sms_config.twoStage = settings.sms_two_stage != 0;
		sms_config.useLevenbergMarquardt = settings.sms_use_levenberg_marquardt != 0;
		sms_config.seedingMode = sms_seeding_mode_from_int( settings.sms_seeding_mode );
		sms_config.targetBounces = settings.sms_target_bounces;
	}

	void build_path_guiding_config( const rise_blender_render_settings& settings, RISE::PathGuidingConfig& guiding_config )
	{
		guiding_config.enabled = settings.path_guiding_enabled != 0;
		guiding_config.trainingIterations = std::max<uint32_t>( settings.path_guiding_training_iterations, 1u );
		guiding_config.trainingSPP = std::max<uint32_t>( settings.path_guiding_training_spp, 1u );
		guiding_config.combineTrainingIterations = settings.path_guiding_combine_training != 0;
		guiding_config.online = settings.path_guiding_online != 0;
		guiding_config.warmupIterations = settings.path_guiding_warmup_iterations;
		guiding_config.alpha = clamp_value<double>( settings.path_guiding_alpha, 0.0, 1.0 );
		guiding_config.learnedAlpha = settings.path_guiding_learned_alpha != 0;
		guiding_config.maxGuidingDepth = settings.path_guiding_max_depth;
		guiding_config.maxLightGuidingDepth = settings.path_guiding_max_light_depth;
		guiding_config.samplingType = guiding_sampling_type_from_int( settings.path_guiding_sampling_type );
		guiding_config.risCandidates = std::max<uint32_t>( settings.path_guiding_ris_candidates, 2u );
		guiding_config.completePathGuiding = false;
		guiding_config.completePathStrategySelection = false;
		guiding_config.completePathStrategySamples = 2;
	}

	void build_adaptive_config( const rise_blender_render_settings& settings, RISE::AdaptiveSamplingConfig& adaptive_config )
	{
		adaptive_config.maxSamples = settings.adaptive_max_samples > 0 ?
			std::max<uint32_t>( settings.adaptive_max_samples, std::max<uint32_t>( settings.pixel_samples, 1u ) ) :
			0u;
		adaptive_config.threshold = settings.adaptive_threshold > 0.0f ? settings.adaptive_threshold : 0.01f;
		adaptive_config.showMap = settings.adaptive_show_map != 0 && adaptive_config.maxSamples > 0;
	}

	void build_stability_config( const rise_blender_render_settings& settings, RISE::StabilityConfig& stability_config )
	{
		stability_config.directClamp = std::max( 0.0f, settings.stability_direct_clamp );
		stability_config.indirectClamp = std::max( 0.0f, settings.stability_indirect_clamp );
		stability_config.filterGlossy = std::max( 0.0f, settings.stability_filter_glossy );
		stability_config.rrMinDepth = settings.stability_rr_min_depth;
		stability_config.rrThreshold = std::max( 0.0f, settings.stability_rr_threshold );
		stability_config.maxDiffuseBounce = bounce_limit_from_ui( settings.stability_max_diffuse_bounce );
		stability_config.maxGlossyBounce = bounce_limit_from_ui( settings.stability_max_glossy_bounce );
		stability_config.maxTransmissionBounce = bounce_limit_from_ui( settings.stability_max_transmission_bounce );
		stability_config.maxTranslucentBounce = bounce_limit_from_ui( settings.stability_max_translucent_bounce );
		stability_config.maxVolumeBounce = bounce_limit_from_ui( settings.stability_max_volume_bounce );
		stability_config.useLightBVH = settings.stability_use_light_bvh != 0;
		stability_config.optimalMIS = settings.stability_optimal_mis != 0;
		stability_config.optimalMISTrainingIterations = std::max<uint32_t>( settings.stability_optimal_mis_training_iterations, 1u );
		stability_config.optimalMISTileSize = std::max<uint32_t>( settings.stability_optimal_mis_tile_size, 1u );
	}

	void build_progressive_config( const rise_blender_render_settings& settings, RISE::ProgressiveConfig& progressive_config )
	{
		progressive_config.enabled = settings.progressive_enabled != 0;
		progressive_config.samplesPerPass = std::max<uint32_t>( settings.progressive_samples_per_pass, 1u );
	}

	void build_pixel_filter_config(
		const rise_blender_render_settings& settings,
		RISE::PixelFilterConfig& pixel_filter_config
	)
	{
		// Default everything to the RISE-internal defaults (width=1,
		// height=1, paramA=0, paramB=0) — each branch overrides what
		// it cares about.  Mirrors the scene-language parser at
		// src/Library/Job.cpp:5850+.
		const double width = settings.pixel_filter_width > 0.0f ? settings.pixel_filter_width : 1.5;
		const double paramA = settings.pixel_filter_param_a;
		const double paramB = settings.pixel_filter_param_b;
		pixel_filter_config.width = width;
		pixel_filter_config.height = width;
		pixel_filter_config.paramA = paramA;
		pixel_filter_config.paramB = paramB;

		switch( settings.pixel_filter )
		{
		case RISE_BLENDER_PIXEL_FILTER_NONE:
			pixel_filter_config.filter = "none";
			break;
		case RISE_BLENDER_PIXEL_FILTER_BOX:
			pixel_filter_config.filter = "box";
			break;
		case RISE_BLENDER_PIXEL_FILTER_TENT:
			pixel_filter_config.filter = "tent";
			break;
		case RISE_BLENDER_PIXEL_FILTER_GAUSSIAN:
			// Gaussian takes paramA = width, paramB = alpha (decay).
			// Default alpha 2.0 matches the typical Mitsuba / PBRT default.
			pixel_filter_config.filter = "gaussian";
			pixel_filter_config.paramA = width;
			pixel_filter_config.paramB = paramB > 0.0 ? paramB : 2.0;
			break;
		case RISE_BLENDER_PIXEL_FILTER_MITCHELL:
			// Mitchell-Netravali B+C standard: B = C = 1/3.
			pixel_filter_config.filter = "mitchell-netravali";
			pixel_filter_config.paramA = paramA > 0.0 ? paramA : 1.0 / 3.0;
			pixel_filter_config.paramB = paramB > 0.0 ? paramB : 1.0 / 3.0;
			break;
		case RISE_BLENDER_PIXEL_FILTER_CATMULL_ROM:
			pixel_filter_config.filter = "catmull-rom";
			break;
		case RISE_BLENDER_PIXEL_FILTER_CUBIC_BSPLINE:
			pixel_filter_config.filter = "cubic_bspline";
			break;
		case RISE_BLENDER_PIXEL_FILTER_BLACKMAN:
			pixel_filter_config.filter = "windowed_sinc_blackman";
			break;
		default:
			pixel_filter_config.filter = "gaussian";
			pixel_filter_config.paramA = width;
			pixel_filter_config.paramB = 2.0;
			break;
		}
	}

	void build_radiance_map_config(
		const rise_blender_scene& scene,
		RISE::RadianceMapConfig& radiance_map_config
	)
	{
		if( !scene.world_radiance_painter_name || !scene.world_radiance_painter_name[0] ) {
			return;
		}
		// Honour Background.Strength=0 (user explicitly disabled the
		// world).  A zero scale produces a zero-radiance IBL, which
		// matches Cycles' "world off" semantics.  The earlier
		// `scale > 0 ? scale : 1.0` fallback silently turned the IBL
		// back on at full strength, masking real "no geometry hit"
		// bugs and producing a grey-everywhere render when the user
		// expected pitch black.
		radiance_map_config.name = RISE::String( scene.world_radiance_painter_name );
		radiance_map_config.scale = std::max( 0.0f, scene.world_radiance_scale );
		radiance_map_config.orientation[0] = scene.world_radiance_orientation[0];
		radiance_map_config.orientation[1] = scene.world_radiance_orientation[1];
		radiance_map_config.orientation[2] = scene.world_radiance_orientation[2];
		radiance_map_config.isBackground = scene.world_radiance_is_background != 0;
	}

	bool configure_rasterizer(
		RISE::IJobPriv& job,
		const rise_blender_render_settings& settings,
		const rise_blender_scene& scene,
		char* error_message,
		const size_t error_message_size
	)
	{
		if( settings.oidn_denoise ) {
#ifndef RISE_ENABLE_OIDN
			write_error( error_message, error_message_size, "This RISE build does not include OIDN support" );
			return false;
#endif
		}

		if( settings.path_guiding_enabled ) {
#ifndef RISE_ENABLE_OPENPGL
			write_error( error_message, error_message_size, "This RISE build does not include OpenPGL path guiding support" );
			return false;
#endif
		}

		RISE::SMSConfig sms_config;                         build_sms_config( settings, sms_config );
		RISE::PathGuidingConfig guiding_config;             build_path_guiding_config( settings, guiding_config );
		RISE::AdaptiveSamplingConfig adaptive_config;       build_adaptive_config( settings, adaptive_config );
		RISE::StabilityConfig stability_config;             build_stability_config( settings, stability_config );
		RISE::ProgressiveConfig progressive_config;         build_progressive_config( settings, progressive_config );

		const RISE::OidnQuality oidn_quality = oidn_quality_from_int( settings.oidn_quality );
		const RISE::OidnDevice oidn_device = oidn_device_from_int( settings.oidn_device );
		const RISE::OidnPrefilter oidn_prefilter = oidn_prefilter_from_int( settings.oidn_prefilter );

		RISE::PixelFilterConfig pixel_filter_config;
		build_pixel_filter_config( settings, pixel_filter_config );

		RISE::RadianceMapConfig radiance_map_config;
		build_radiance_map_config( scene, radiance_map_config );

		const uint32_t pixel_samples = std::max<uint32_t>( settings.pixel_samples, 1u );
		const uint32_t light_samples = std::max<uint32_t>( settings.light_samples, 1u );
		const uint32_t max_recursion = std::max<uint32_t>( settings.max_recursion, 1u );
		// Subpath depth fallbacks for BDPT / VCM / MLT: when the
		// caller leaves bidir_max_*_depth at 0, default to the
		// shared `max_recursion` knob (already exposed in the UI).
		const uint32_t bidir_eye   = settings.bidir_max_eye_depth   > 0u ? settings.bidir_max_eye_depth   : max_recursion;
		const uint32_t bidir_light = settings.bidir_max_light_depth > 0u ? settings.bidir_max_light_depth : max_recursion;

		// Resolve the rasterizer kind.  ABI v4 callers populate
		// `use_path_tracing` and leave `rasterizer_kind = 0`; that
		// combination means "use PT-pel if use_path_tracing else
		// legacy pixelpel" — preserving the v4 behaviour.  ABI v5
		// callers set `rasterizer_kind` directly and we ignore
		// `use_path_tracing`.
		uint32_t kind = settings.rasterizer_kind;
		if( kind == RISE_BLENDER_RASTERIZER_PIXELPEL ) {
			kind = settings.use_path_tracing
				? RISE_BLENDER_RASTERIZER_PT_PEL
				: RISE_BLENDER_RASTERIZER_PIXELPEL;
		}

		// Spectral integrators all share a default SpectralConfig:
		// 380–780 nm visible range, 10 bins, 1 stochastic spectral
		// sample/pixel.  Future improvement: expose nm range / bin
		// count / HWSS toggle in the addon UI.
		RISE::SpectralConfig spectral_config;

		switch( kind )
		{
		case RISE_BLENDER_RASTERIZER_PIXELPEL:
			// Legacy non-PT path — drives the recursive shader-op
			// stack (`DefaultDirectLighting` etc.).  SMS unavailable
			// here because SMS lives in the PT integrator only.
			if( !job.SetPixelBasedPelRasterizer(
				pixel_samples, light_samples, max_recursion,
				kShaderName, radiance_map_config, 0, 0.0,
				pixel_filter_config, settings.show_lights != 0,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the pixel rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_PT_PEL:
			if( !job.SetPathTracingPelRasterizer(
				pixel_samples, kShaderName, radiance_map_config,
				pixel_filter_config, settings.show_lights != 0,
				sms_config, settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the PT-pel rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_PT_SPECTRAL:
			if( !job.SetPathTracingSpectralRasterizer(
				pixel_samples, kShaderName, radiance_map_config,
				pixel_filter_config, settings.show_lights != 0,
				spectral_config, sms_config,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the PT-spectral rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_BDPT_PEL:
			if( !job.SetBDPTPelRasterizer(
				pixel_samples, bidir_eye, bidir_light,
				kShaderName, radiance_map_config, pixel_filter_config,
				settings.show_lights != 0,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the BDPT-pel rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_BDPT_SPECTRAL:
			if( !job.SetBDPTSpectralRasterizer(
				pixel_samples, bidir_eye, bidir_light,
				kShaderName, radiance_map_config, pixel_filter_config,
				settings.show_lights != 0,
				spectral_config,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the BDPT-spectral rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_VCM_PEL:
		{
			// enable_vc / enable_vm default to true when both come
			// in as 0 — otherwise users would have to toggle two
			// extra knobs just to get default VCM behaviour.
			const bool enable_vc = settings.vcm_enable_vc != 0 || ( settings.vcm_enable_vc == 0 && settings.vcm_enable_vm == 0 );
			const bool enable_vm = settings.vcm_enable_vm != 0 || ( settings.vcm_enable_vc == 0 && settings.vcm_enable_vm == 0 );
			if( !job.SetVCMPelRasterizer(
				pixel_samples, bidir_eye, bidir_light,
				kShaderName, radiance_map_config, pixel_filter_config,
				settings.show_lights != 0,
				settings.vcm_merge_radius,
				enable_vc, enable_vm,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the VCM-pel rasterizer" );
				return false;
			}
			return true;
		}

		case RISE_BLENDER_RASTERIZER_VCM_SPECTRAL:
		{
			const bool enable_vc = settings.vcm_enable_vc != 0 || ( settings.vcm_enable_vc == 0 && settings.vcm_enable_vm == 0 );
			const bool enable_vm = settings.vcm_enable_vm != 0 || ( settings.vcm_enable_vc == 0 && settings.vcm_enable_vm == 0 );
			if( !job.SetVCMSpectralRasterizer(
				pixel_samples, bidir_eye, bidir_light,
				kShaderName, radiance_map_config, pixel_filter_config,
				settings.show_lights != 0,
				spectral_config,
				settings.vcm_merge_radius,
				enable_vc, enable_vm,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the VCM-spectral rasterizer" );
				return false;
			}
			return true;
		}

		case RISE_BLENDER_RASTERIZER_MLT_PEL:
		{
			// PSSMLT defaults — tuned for the same quality envelope
			// as production MLT scenes in scenes/FeatureBased.
			const uint32_t n_bootstrap = settings.mlt_bootstrap > 0 ? settings.mlt_bootstrap : 10000u;
			const uint32_t n_chains    = settings.mlt_chains    > 0 ? settings.mlt_chains    : 8u;
			const uint32_t mutations   = settings.mlt_mutations_per_pixel > 0 ? settings.mlt_mutations_per_pixel : pixel_samples;
			const double   large_step  = settings.mlt_large_step_prob > 0.0f ? double( settings.mlt_large_step_prob ) : 0.3;
			if( !job.SetMLTRasterizer(
				bidir_eye, bidir_light,
				n_bootstrap, n_chains, mutations, large_step,
				kShaderName, settings.show_lights != 0,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				pixel_filter_config, stability_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the MLT-pel rasterizer" );
				return false;
			}
			return true;
		}

		case RISE_BLENDER_RASTERIZER_MLT_SPECTRAL:
		{
			const uint32_t n_bootstrap = settings.mlt_bootstrap > 0 ? settings.mlt_bootstrap : 10000u;
			const uint32_t n_chains    = settings.mlt_chains    > 0 ? settings.mlt_chains    : 8u;
			const uint32_t mutations   = settings.mlt_mutations_per_pixel > 0 ? settings.mlt_mutations_per_pixel : pixel_samples;
			const double   large_step  = settings.mlt_large_step_prob > 0.0f ? double( settings.mlt_large_step_prob ) : 0.3;
			if( !job.SetMLTSpectralRasterizer(
				bidir_eye, bidir_light,
				n_bootstrap, n_chains, mutations, large_step,
				kShaderName, settings.show_lights != 0,
				spectral_config,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				pixel_filter_config, stability_config ) )
			{
				write_error( error_message, error_message_size, "Failed to create the MLT-spectral rasterizer" );
				return false;
			}
			return true;
		}

		case RISE_BLENDER_RASTERIZER_AUTO_PEL:
			if( !job.SetAutoRasterizer(
				RISE::AutoIntegratorChoice::Auto,
				pixel_samples, kShaderName, radiance_map_config,
				pixel_filter_config, settings.show_lights != 0,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				guiding_config, adaptive_config, stability_config, progressive_config,
				/*probeEnabled*/ true ) )
			{
				write_error( error_message, error_message_size, "Failed to create the auto rasterizer" );
				return false;
			}
			return true;

		case RISE_BLENDER_RASTERIZER_AUTO_SPECTRAL:
			if( !job.SetAutoSpectralRasterizer(
				RISE::AutoIntegratorChoice::Auto,
				pixel_samples, kShaderName, radiance_map_config,
				pixel_filter_config, settings.show_lights != 0,
				spectral_config,
				settings.oidn_denoise != 0,
				oidn_quality, oidn_device, oidn_prefilter,
				adaptive_config, stability_config, progressive_config,
				/*probeEnabled*/ true ) )
			{
				write_error( error_message, error_message_size, "Failed to create the auto-spectral rasterizer" );
				return false;
			}
			return true;

		default:
			write_error( error_message, error_message_size, "Unknown rasterizer kind" );
			return false;
		}
	}
}

extern "C" int rise_blender_api_version( void )
{
	return RISE_BLENDER_API_VERSION;
}

extern "C" int rise_blender_get_capabilities( rise_blender_capabilities* capabilities )
{
	if( !capabilities ) {
		return 0;
	}

	capabilities->api_version = RISE_BLENDER_API_VERSION;
#ifdef RISE_ENABLE_OIDN
	capabilities->supports_oidn = 1;
#else
	capabilities->supports_oidn = 0;
#endif
#ifdef RISE_ENABLE_OPENPGL
	capabilities->supports_path_guiding = 1;
#else
	capabilities->supports_path_guiding = 0;
#endif
#ifdef RISE_BLENDER_ENABLE_OPENVDB
	capabilities->supports_vdb_volumes = 1;
#else
	capabilities->supports_vdb_volumes = 0;
#endif
	return 1;
}

extern "C" int rise_blender_render_scene(
	const rise_blender_scene* scene,
	const rise_blender_render_settings* settings,
	rise_blender_progress_callback progress_callback,
	rise_blender_image_callback image_callback,
	void* user_data,
	rise_blender_render_result* result,
	char* error_message,
	size_t error_message_size
)
{
	write_error( error_message, error_message_size, "" );

	if( result ) {
		result->rgba = 0;
		result->width = 0;
		result->height = 0;
		result->warnings[0] = 0;
	}

	if( !scene || !scene->camera || !settings || !result ) {
		write_error( error_message, error_message_size, "rise_blender_render_scene received null input" );
		return 0;
	}

	RISE::IJobPriv* job = 0;
	if( !RISE::RISE_CreateJobPriv( &job ) || !job ) {
		write_error( error_message, error_message_size, "Failed to create a RISE job" );
		return 0;
	}

	ProgressBridge progress( progress_callback, user_data );
	MemoryRasterizerOutput raster_output( image_callback, user_data );
	VolumeCacheGuard volume_cache_guard;

	job->SetProgress( &progress );
	// Top-level BVH for the scene's objects.  CLAUDE.md documents the
	// production default as (leaf=4, depth=32) — same as
	// `InitializeContainers` installs; we re-call to be explicit and to
	// keep this on the documented path.  The previous (leaf=100,
	// depth=8) silently overflowed for any scene with > 25k objects:
	// 2^8 leaves × 100 objects/leaf = 25,600 max-addressable objects.
	// Real Blender scenes (Geometry-Nodes scatters, packed grass,
	// foliage instancing) routinely exceed 80k objects; the overflow
	// collapsed traversal so that primary rays never found a hit, and
	// the whole frame rendered as just the world IBL.  See the
	// lone-monk scene (≈ 87,800 objects) which failed silently under
	// the old caps.
	job->SetPrimaryAcceleration( true, false, 4, 32 );

	if( !configure_camera( *job, *scene->camera, error_message, error_message_size ) ) {
		RISE::safe_release( job );
		return 0;
	}

	for( uint32_t i = 0; i < scene->num_painters; ++i ) {
		if( !add_painter( *job, scene->painters[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i = 0; i < scene->num_modifiers; ++i ) {
		if( !add_modifier( *job, scene->modifiers[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i = 0; i < scene->num_materials; ++i ) {
		if( !add_material( *job, scene->materials[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i = 0; i < scene->num_meshes; ++i ) {
		if( !add_mesh( *job, scene->meshes[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i = 0; i < scene->num_objects; ++i ) {
		if( !add_object( *job, scene->objects[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	// Hair / fur grooms (ABI v9).  Materials first -- a hair object
	// names one.  Both loops are non-fatal: a failure skips that groom
	// and records a warning for the result struct (see the hair block
	// in the anonymous namespace above for why).
	std::vector<std::string> warnings;
	for( uint32_t i = 0; i < scene->num_hair_materials; ++i ) {
		(void)add_hair_material( *job, scene->hair_materials[i], warnings );
	}

	for( uint32_t i = 0; i < scene->num_hair_objects; ++i ) {
		(void)add_hair_object( *job, scene->hair_objects[i], warnings );
	}

	if( !create_and_assign_media( *job, *scene, *settings, volume_cache_guard, error_message, error_message_size ) ) {
		RISE::safe_release( job );
		return 0;
	}

	for( uint32_t i = 0; i < scene->num_lights; ++i ) {
		if( !add_light( *job, scene->lights[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	if( scene->use_world_ambient && scene->world_strength > 1e-6f && color_non_zero( scene->world_color ) ) {
		rise_blender_light ambient;
		std::memset( &ambient, 0, sizeof( ambient ) );
		ambient.name = "rise_blender_world_ambient";
		ambient.type = RISE_BLENDER_LIGHT_AMBIENT;
		ambient.color[0] = scene->world_color[0];
		ambient.color[1] = scene->world_color[1];
		ambient.color[2] = scene->world_color[2];
		ambient.intensity = scene->world_strength;
		if( !add_light( *job, ambient, error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	if( !configure_shader( *job, *settings, error_message, error_message_size ) ) {
		RISE::safe_release( job );
		return 0;
	}

	if( !configure_rasterizer( *job, *settings, *scene, error_message, error_message_size ) ) {
		RISE::safe_release( job );
		return 0;
	}

	if( !job->AddCallbackRasterizerOutput( &raster_output ) ) {
		write_error( error_message, error_message_size, "Failed to attach the rasterizer callback output" );
		RISE::safe_release( job );
		return 0;
	}

	if( !job->Rasterize() ) {
		if( progress.Aborted() ) {
			write_error( error_message, error_message_size, "RISE render canceled" );
		} else {
			write_error( error_message, error_message_size, "RISE rasterization failed" );
		}
		RISE::safe_release( job );
		return 0;
	}

	if( !raster_output.CopyToResult( *result ) ) {
		write_error( error_message, error_message_size, "RISE did not produce an image buffer" );
		RISE::safe_release( job );
		return 0;
	}

	// Auto-dispatcher resolution (ABI v8): if the active rasterizer is the
	// auto_rasterizer, report the concrete integrator it resolved to + the reason
	// for the Blender UI's "Auto -> X" surfacing.  Queried while the rasterizer is
	// still alive (before the job is released).
	// Non-fatal diagnostics (ABI v9).  Populated only on the success
	// path: a failed render reports through `error_message` instead.
	pack_warnings( warnings, result->warnings, sizeof( result->warnings ) );

	result->is_auto = 0;
	result->resolved_integrator[0] = '\0';
	result->resolve_reason[0] = '\0';
	{
		RISE::IRasterizer* active = job->GetRasterizer();
		if( active && active->IsAutoDispatcher() ) {
			result->is_auto = 1;
			std::snprintf( result->resolved_integrator, sizeof( result->resolved_integrator ),
				"%s", active->ResolvedIntegratorName() );
			std::snprintf( result->resolve_reason, sizeof( result->resolve_reason ),
				"%s", active->ResolveReason() );
		}
	}

	RISE::safe_release( job );
	return 1;
}

extern "C" void rise_blender_free_render_result( rise_blender_render_result* result )
{
	if( !result ) {
		return;
	}

	if( result->rgba ) {
		std::free( result->rgba );
	}

	result->rgba = 0;
	result->width = 0;
	result->height = 0;
}

extern "C" void rise_blender_shutdown( void )
{
	RISE::GlobalLogCleanupAndShutdown();
}
