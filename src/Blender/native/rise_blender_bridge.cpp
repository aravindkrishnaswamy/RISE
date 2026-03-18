#include "rise_blender_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <RISE_API.h>
#include <Interfaces/IJobPriv.h>
#include <Interfaces/IJobRasterizerOutput.h>
#include <Interfaces/IObjectPriv.h>
#include <Interfaces/IProgressCallback.h>
#include <Interfaces/ILog.h>
#include <Utilities/Math3D/Math3D.h>

namespace
{
	const char* kShaderName = "rise_blender_surface";
	const char* kPathTracingShaderOpName = "rise_blender_pathtracer";

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
		return color && ( std::fabs(color[0]) > 1e-6f || std::fabs(color[1]) > 1e-6f || std::fabs(color[2]) > 1e-6f );
	}

	double linear_to_srgb_channel( const float value )
	{
		const double clamped = clamp_value<double>( value, 0.0, 1.0 );
		if( clamped <= 0.0031308 ) {
			return clamped * 12.92;
		}
		return 1.055 * std::pow( clamped, 1.0/2.4 ) - 0.055;
	}

	void linear_rgb_to_srgb( const float linear[3], double srgb[3] )
	{
		srgb[0] = linear_to_srgb_channel( linear[0] );
		srgb[1] = linear_to_srgb_channel( linear[1] );
		srgb[2] = linear_to_srgb_channel( linear[2] );
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

	RISE::Matrix4 blender_matrix_to_rise_matrix( const float transform[16] )
	{
		RISE::Matrix4 matrix;

		matrix._00 = transform[0];
		matrix._01 = transform[1];
		matrix._02 = transform[2];
		matrix._03 = transform[12];

		matrix._10 = transform[4];
		matrix._11 = transform[5];
		matrix._12 = transform[6];
		matrix._13 = transform[13];

		matrix._20 = transform[8];
		matrix._21 = transform[9];
		matrix._22 = transform[10];
		matrix._23 = transform[14];

		matrix._30 = transform[3];
		matrix._31 = transform[7];
		matrix._32 = transform[11];
		matrix._33 = transform[15];

		return matrix;
	}

	bool validate_index_array( const uint32_t* indices, const size_t count, const uint32_t limit )
	{
		if( !indices ) {
			return false;
		}

		for( size_t i=0; i<count; ++i ) {
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

		const size_t triangle_index_count = static_cast<size_t>(mesh.num_triangles) * 3;

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

	public:
		MemoryRasterizerOutput() :
			width_( 0 ),
			height_( 0 )
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
			if( !pImageData ) {
				return;
			}

			if( width_ != width || height_ != height || rgba_.empty() ) {
				width_ = width;
				height_ = height;
				rgba_.assign( width_ * height_ * 4, 0.0f );
			}

			for( unsigned int y = rc_top; y <= rc_bottom; y++ ) {
				for( unsigned int x = rc_left; x <= rc_right; x++ ) {
					const unsigned int offset = ( y * width_ + x ) * 4;
					rgba_[offset+0] = float(pImageData[offset+0]) / 65535.0f;
					rgba_[offset+1] = float(pImageData[offset+1]) / 65535.0f;
					rgba_[offset+2] = float(pImageData[offset+2]) / 65535.0f;
					rgba_[offset+3] = float(pImageData[offset+3]) / 65535.0f;
				}
			}
		}

		bool CopyToResult( rise_blender_render_result& result ) const
		{
			if( rgba_.empty() || width_ == 0 || height_ == 0 ) {
				return false;
			}

			const size_t total_values = rgba_.size();
			result.rgba = static_cast<float*>( std::malloc( total_values * sizeof(float) ) );
			if( !result.rgba ) {
				return false;
			}

			std::memcpy( result.rgba, &rgba_[0], total_values * sizeof(float) );
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

			const float normalized = total > 0.0 ? float(progress / total) : 1.0f;
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

		if( camera.projection_type == RISE_BLENDER_CAMERA_ORTHOGRAPHIC ) {
			const double square_dimension = std::max<unsigned int>( camera.width, camera.height );
			const double vp_scale[2] = {
				double(camera.ortho_scale) * ( double(camera.width) / square_dimension ) * 0.5,
				double(camera.ortho_scale) * ( double(camera.height) / square_dimension ) * 0.5
			};

			if( !job.SetOrthographicCamera(
				location,
				lookat,
				up,
				camera.width,
				camera.height,
				vp_scale,
				camera.pixel_aspect,
				0.0,
				0.0,
				0.0,
				orientation,
				target_orientation ) )
			{
				write_error( error_message, error_message_size, "Failed to configure orthographic camera" );
				return false;
			}

			return true;
		}

		if( !job.SetPinholeCamera(
			location,
			lookat,
			up,
			camera.fov_y_radians,
			camera.width,
			camera.height,
			camera.pixel_aspect,
			0.0,
			0.0,
			0.0,
			orientation,
			target_orientation ) )
		{
			write_error( error_message, error_message_size, "Failed to configure perspective camera" );
			return false;
		}

		return true;
	}

	bool configure_shader( RISE::IJobPriv& job, const rise_blender_render_settings& settings, char* error_message, const size_t error_message_size )
	{
		if( settings.use_path_tracing ) {
			if( !job.AddPathTracingShaderOp( kPathTracingShaderOpName, false, true, false, true, true, true, true ) ) {
				write_error( error_message, error_message_size, "Failed to create path tracing shader op" );
				return false;
			}

			const char* shader_ops[3] = {
				"DefaultDirectLighting",
				"DefaultEmission",
				kPathTracingShaderOpName
			};

			if( !job.AddStandardShader( kShaderName, 3, shader_ops ) ) {
				write_error( error_message, error_message_size, "Failed to assemble the RISE shader stack" );
				return false;
			}

			return true;
		}

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

	bool add_material( RISE::IJobPriv& job, const rise_blender_material& material, char* error_message, const size_t error_message_size )
	{
		if( !material.name ) {
			write_error( error_message, error_message_size, "Encountered a material with no name" );
			return false;
		}

		const std::string material_name( material.name );
		const std::string base_painter_name = material_name + "::base";
		double base_color[3] = {
			material.base_color[0],
			material.base_color[1],
			material.base_color[2]
		};

		if( !job.AddUniformColorPainter( base_painter_name.c_str(), base_color, "Rec709RGB_Linear" ) ) {
			write_error( error_message, error_message_size, "Failed to create a base color painter" );
			return false;
		}

		const bool emissive = material.emission_strength > 1e-6f && color_non_zero( material.emission_color );
		const std::string surface_material_name = emissive ? material_name + "::surface" : material_name;
		bool created_surface = false;

		if( material.transmission > 0.8f ) {
			char ior_string[64] = {0};
			std::snprintf( ior_string, sizeof(ior_string), "%.6f", clamp_value<float>( material.ior, 1.0f, 4.0f ) );

			if( material.roughness < 0.02f ) {
				created_surface = job.AddPerfectRefractorMaterial( surface_material_name.c_str(), base_painter_name.c_str(), ior_string );
			} else {
				char scatter_string[64] = {0};
				std::snprintf( scatter_string, sizeof(scatter_string), "%.6f", clamp_value<float>( material.roughness, 0.0f, 1.0f ) );
				created_surface = job.AddDielectricMaterial( surface_material_name.c_str(), base_painter_name.c_str(), ior_string, scatter_string, false );
			}
		} else if( material.metallic > 0.9f && material.roughness < 0.02f ) {
			created_surface = job.AddPerfectReflectorMaterial( surface_material_name.c_str(), base_painter_name.c_str() );
		} else if( material.specular > 1e-3f || material.metallic > 1e-3f || material.roughness > 0.05f ) {
			const std::string specular_painter_name = material_name + "::specular";
			double specular_color[3];
			for( int i=0; i<3; i++ ) {
				const double dielectric = 0.04 * clamp_value<float>( material.specular, 0.0f, 1.0f );
				const double metallic = clamp_value<float>( material.metallic, 0.0f, 1.0f );
				specular_color[i] = clamp_value<double>( dielectric * (1.0 - metallic) + base_color[i] * metallic, 0.0, 1.0 );
			}

			if( !job.AddUniformColorPainter( specular_painter_name.c_str(), specular_color, "Rec709RGB_Linear" ) ) {
				write_error( error_message, error_message_size, "Failed to create a specular painter" );
				return false;
			}

			char roughness_string[64] = {0};
			std::snprintf( roughness_string, sizeof(roughness_string), "%.6f", clamp_value<float>( material.roughness, 0.02f, 1.0f ) );
			created_surface = job.AddSchlickMaterial( surface_material_name.c_str(), base_painter_name.c_str(), specular_painter_name.c_str(), roughness_string, "1.0" );
		} else {
			created_surface = job.AddLambertianMaterial( surface_material_name.c_str(), base_painter_name.c_str() );
		}

		if( !created_surface ) {
			write_error( error_message, error_message_size, "Failed to create a RISE material" );
			return false;
		}

		if( emissive ) {
			const std::string emission_painter_name = material_name + "::emission";
			double emission_color[3] = {
				double(material.emission_color[0]) * material.emission_strength,
				double(material.emission_color[1]) * material.emission_strength,
				double(material.emission_color[2]) * material.emission_strength
			};

			if( !job.AddUniformColorPainter( emission_painter_name.c_str(), emission_color, "Rec709RGB_Linear" ) ) {
				write_error( error_message, error_message_size, "Failed to create an emission painter" );
				return false;
			}

			if( !job.AddLambertianLuminaireMaterial( material_name.c_str(), emission_painter_name.c_str(), surface_material_name.c_str(), 1.0 ) ) {
				write_error( error_message, error_message_size, "Failed to create an emissive material wrapper" );
				return false;
			}
		}

		return true;
	}

	bool add_mesh( RISE::IJobPriv& job, const rise_blender_mesh& mesh, char* error_message, const size_t error_message_size )
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
			24,
			24,
			mesh.double_sided != 0,
			true,
			mesh.use_face_normals != 0 ) )
		{
			write_error( error_message, error_message_size, "Failed to add indexed triangle mesh geometry to the RISE job" );
			return false;
		}

		return true;
	}

	bool add_object( RISE::IJobPriv& job, const rise_blender_object& object, char* error_message, const size_t error_message_size )
	{
		if( !object.name || !object.geometry_name || !object.material_name ) {
			write_error( error_message, error_message_size, "Encountered an object with missing references" );
			return false;
		}

		double zero[3] = { 0.0, 0.0, 0.0 };
		double one[3] = { 1.0, 1.0, 1.0 };

		if( !job.AddObject(
			object.name,
			object.geometry_name,
			object.material_name,
			0,
			0,
			0,
			1.0,
			zero,
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

	bool add_light( RISE::IJobPriv& job, const rise_blender_light& light, char* error_message, const size_t error_message_size )
	{
		if( !light.name ) {
			write_error( error_message, error_message_size, "Encountered a light with no name" );
			return false;
		}

		double color[3];
		linear_rgb_to_srgb( light.color, color );

		switch( light.type )
		{
		case RISE_BLENDER_LIGHT_POINT:
		case RISE_BLENDER_LIGHT_AREA:
		{
			double position[3] = { light.position[0], light.position[1], light.position[2] };
			if( !job.AddPointOmniLight( light.name, light.intensity, color, position, 0.0, 0.0 ) ) {
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
			const double outer_angle = clamp_value<float>( light.spot_size, 0.0f, float(M_PI) ) * 0.5;
			const double inner_angle = outer_angle - outer_angle * clamp_value<float>( light.spot_blend, 0.0f, 1.0f );
			if( !job.AddPointSpotLight( light.name, light.intensity, color, focus, inner_angle, outer_angle, position, 0.0, 0.0 ) ) {
				write_error( error_message, error_message_size, "Failed to add a spot light" );
				return false;
			}
			return true;
		}
		case RISE_BLENDER_LIGHT_SUN:
		{
			double direction[3] = { light.direction[0], light.direction[1], light.direction[2] };
			normalize3( direction );
			if( !job.AddDirectionalLight( light.name, light.intensity, color, direction ) ) {
				write_error( error_message, error_message_size, "Failed to add a directional light" );
				return false;
			}
			return true;
		}
		case RISE_BLENDER_LIGHT_AMBIENT:
		default:
			if( !job.AddAmbientLight( light.name, light.intensity, color ) ) {
				write_error( error_message, error_message_size, "Failed to add an ambient light" );
				return false;
			}
			return true;
		}
	}
}

extern "C" int rise_blender_api_version( void )
{
	return 1;
}

extern "C" int rise_blender_render_scene(
	const rise_blender_scene* scene,
	const rise_blender_render_settings* settings,
	rise_blender_progress_callback progress_callback,
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
	MemoryRasterizerOutput raster_output;

	job->SetProgress( &progress );
	job->SetPrimaryAcceleration( true, false, 100, 8 );

	if( !configure_camera( *job, *scene->camera, error_message, error_message_size ) ) {
		RISE::safe_release( job );
		return 0;
	}

	for( uint32_t i=0; i<scene->num_materials; i++ ) {
		if( !add_material( *job, scene->materials[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i=0; i<scene->num_meshes; i++ ) {
		if( !add_mesh( *job, scene->meshes[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i=0; i<scene->num_objects; i++ ) {
		if( !add_object( *job, scene->objects[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	for( uint32_t i=0; i<scene->num_lights; i++ ) {
		if( !add_light( *job, scene->lights[i], error_message, error_message_size ) ) {
			RISE::safe_release( job );
			return 0;
		}
	}

	if( scene->use_world_ambient && scene->world_strength > 1e-6f && color_non_zero( scene->world_color ) ) {
		rise_blender_light ambient;
		std::memset( &ambient, 0, sizeof(ambient) );
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

	double orientation[3] = { 0.0, 0.0, 0.0 };
	if( !job->SetPixelBasedPelRasterizer(
		std::max<uint32_t>( settings->pixel_samples, 1 ),
		std::max<uint32_t>( settings->light_samples, 1 ),
		std::max<uint32_t>( settings->max_recursion, 1 ),
		settings->min_importance > 0.0f ? settings->min_importance : 0.01f,
		kShaderName,
		0,
		false,
		1.0,
		orientation,
		0,
		0.0,
		0,
		0.0,
		0,
		1.0,
		1.0,
		0.0,
		0.0,
		settings->show_lights != 0,
		settings->use_ior_stack != 0,
		settings->choose_one_light != 0 ) )
	{
		write_error( error_message, error_message_size, "Failed to create the pixel rasterizer" );
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
