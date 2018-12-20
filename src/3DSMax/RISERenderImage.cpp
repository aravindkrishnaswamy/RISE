#include "pch.h"

#include <io.h>			// for _access
#include <map>

#include "maxincl.h"
#include "rise3dsmax.h"
#include "scontext.h"
#include "rendutil.h"
#include "resource.h"

#include "RISEIncludes.h"


#include "MAXGeometry.h"
#include "MAXShaderOp.h"
#include "MAXMaterial.h"
#include "MAXRadianceMap.h"
#include "MAXAtmosphere.h"

class BitmapOutput : public RISE::IJobRasterizerOutput
{
protected:
	Bitmap& tobm;

public:
	BitmapOutput( Bitmap& bm ) : tobm( bm )
	{
	}

	bool PremultipliedAlpha()
	{
		return !!tobm.PreMultipliedAlpha();
	}

	double GetGammaCorrection()
	{
		return tobm.Gamma();
	}

	void OutputImageRGBA16( 
		const unsigned short* pImageData,			///< [in] Data pointer to rasterized image
		const unsigned int width,					///< [in] Width of the image
		const unsigned int height,					///< [in] Height of the image
		const unsigned int rc_top,					///< [in] Defines the precise rectangular area to update
		const unsigned int rc_left,					///< [in] Defines the precise rectangular area to update
		const unsigned int rc_bottom,				///< [in] Defines the precise rectangular area to update
		const unsigned int rc_right					///< [in] Defines the precise rectangular area to update
	)
	{
		for( unsigned int y=rc_top; y<=rc_bottom; y++ ) {
			tobm.PutPixels(rc_left,y,rc_right-rc_left+1,(BMM_Color_64*)&pImageData[y*width*4+rc_left*4] );
		}

		RECT rect;
		rect.left = rc_left;
		rect.top = rc_top;
		rect.right = rc_right+1;
		rect.bottom = rc_bottom+1;
		tobm.RefreshWindow(&rect);
	}
};

class ProgressCallback : public RISE::IProgressCallback
{
protected:
	RendProgressCallback* prog;

public:
	ProgressCallback( RendProgressCallback* prog_ ) : 
	  prog( prog_ )
	  {}

	bool Progress( const double progress, const double total )
	{
		if( prog ) {
			int n = 0;
			if( total == 0 ) {
				n = 100;
			} else {
				n = static_cast<int>( (progress/total) * 100.0 );
			}

			if( prog->Progress( n, 100 ) == RENDPROG_ABORT ) {
				return false;
			}
		}

		return true;
	}

	void SetTitle( const char* title )
	{
		if( prog ) {
			prog->SetTitle( title );
		}
	}
};


bool SetupCamera( RISE::IJobPriv& job, const ViewParams& view, const int width, const int height, const double aspect )
{
	if( view.projType != PROJ_PERSPECTIVE ) {
		return false;
	}

	Matrix3 mxCamera = Inverse(view.affineTM);

	const double fov = view.fov * (double(height)/double(width));
	Point3 p = mxCamera.GetTrans();
	Point3 t = p + mxCamera.GetRow(2)*(-100.0f);  // target is (somewhere) in negative Z direction

	double loc[3] = {p.x, p.y, p.z};
	double lookat[3] = {t.x, t.y, t.z};
	
	Point3 vUp = mxCamera.VectorTransform( Point3( 0, 1, 0 ) );
	double up[3] = {vUp.x, vUp.y, vUp.z};

	double o[3]={0}, to[2]={0};

	// See if there is a camera already set
	RISE::ICamera* pCamera = job.GetScene()->GetCamera();
	if( !pCamera ) {
		job.SetPinholeCamera( loc, lookat, up, fov, width, height, 1.0, 0, 0, 0, o, to );
	} else {
		// Otherwise, modify the existing camera with the MAX camera settings
        // I know this is a hack, but we can use the IKeyframamble interface to set intermediate values 
		// to change the existing camera setting's parameters
		char val[256] = {0};

		// Location
		{
			sprintf( val, "%lf %lf %lf", loc[0], loc[1], loc[2] );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "location", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// Look at
		{
			sprintf( val, "%lf %lf %lf", lookat[0], lookat[1], lookat[2] );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "lookat", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// Up vector
		{
			sprintf( val, "%lf %lf %lf", up[0], up[1], up[2] );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "up", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// FOV
		{
			sprintf( val, "%lf", fov* RAD_TO_DEG );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "fov", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// Width
		{
			sprintf( val, "%u", width );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "width", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// Height
		{
			sprintf( val, "%u", height );
			RISE::IKeyframeParameter* p = pCamera->KeyframeFromParameters( "height", val );
			if( p ) {
				pCamera->SetIntermediateValue( *p );
				p->release();
			}
		}

		// Tell the camera to get itself ready for rendering
		pCamera->RegenerateData();
	}

	return true;
}

int RiseRenderer::RenderImage( RiseRendererParams& rp, TimeValue t, Bitmap* tobm, RendProgressCallback *prog, HWND hwnd )
{
	RISE::IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );
	pJob->SetPrimaryAcceleration( true, false, 100, 8 );

	ProgressCallback pc( prog );
	pJob->SetProgress( &pc );

	///////////////////////////////////////////////
	// Create the global MAX stuff
	// We do this here so that the .RISEscene
	// file we load can have accesst o it
	///////////////////////////////////////////////
	{
		// Since the MAX material gets the mtl identifier from the pInst, we only ever need
		// one global MAX material
		MAXMaterial* pMaterial = new MAXMaterial( this, &rendParams );
		RISE::GlobalLog()->PrintNew( pMaterial, __FILE__, __LINE__, "MAXMaterial" );

		pJob->GetMaterials()->AddItem( pMaterial, "GloballMAXMaterial" );
		pMaterial->release();
	}

	{
		// Since the MAX ShaderOp gets the mtl identifier from the pInst, we only ever need
		// one global MAX shader and shaderop
		MAXShaderOp* pShader = new MAXShaderOp( this, &rendParams );
		RISE::GlobalLog()->PrintNew( pShader, __FILE__, __LINE__, "MAXShader" );

		const char* globalShaderOpName = "3DSMAX";
		pJob->GetShaderOps()->AddItem( pShader, globalShaderOpName );
		pJob->AddStandardShader( globalShaderOpName, 1, (const char**)&globalShaderOpName );
		pShader->release();
	}

	// Try and load the .RISEscene that contains the settings and materials
	if( _access( this->szRenderSettingsFile, 0 ) ) {
		// Default renderer and global shader
		const char* shaderops = "3DSMAX";
		pJob->AddStandardShader( "global", 1, &shaderops );

		double or[3] = {0,0,0};

		pJob->SetPixelBasedPelRasterizer(
			1, 1, 10, 0.01, 
			"global", 0, true, 0, or, 0, 0, 0, 0, 0, 1.0, 1.0, 0.0, 0.0, true, false, false, true );
	} else {
		prog->SetTitle( "Loading renderer settings and materials" );
		if( !pJob->LoadAsciiScene( szRenderSettingsFile ) ) {
			MessageBox( hwnd, "Failed to properly load the renderer/material settings file, aborting render", GetString(IDS_RENDRISETITLE), MB_OK );
			safe_release( pJob );
			return 0;
		}
	}

	// Setup the camera
	if( !SetupCamera( *pJob, view, rendParams.devWidth, rendParams.devHeight, rendParams.devAspect ) ) {
		MessageBox( hwnd, "Failed to properly set camera, aborting render", GetString(IDS_RENDRISETITLE), MB_OK );
		return 0;
	}

	/////////////////////////////////////////
	// Setup all of the objects
	/////////////////////////////////////////

	typedef std::map<Mesh*, std::string> MeshMap;
	MeshMap geoms;

	typedef std::map<Mtl*, std::string> MtlMap;
	MtlMap riseMaterials;

	if( prog ) {
		prog->SetTitle( "Creating Acceleration Structures" );
		prog->Progress( 0, 1 );
	}

	for( int n = 0; n < instTab.Count(); n++ ) {
		Instance* pInst = instTab[n];

		if( !pInst->mesh ) {
			continue;
		}

		char geomname[256] = {0};

		// Check to see if the geometry instance already is in our list
		MeshMap::iterator it = geoms.find( pInst->GetMesh() );
		if( it != geoms.end() ) {
			// Instance of object already exists!
			strcpy( geomname, it->second.c_str() );
		} else {
			// Create the geometry instance 
			sprintf( geomname, "Geom::%s", pInst->GetName() );

			MAXGeometry* pGeometry = new MAXGeometry( pInst );
			RISE::GlobalLog()->PrintNew( pGeometry, __FILE__, __LINE__, "MAX geometry" );
			pJob->GetGeometries()->AddItem( pGeometry, geomname );
			pGeometry->release();

			// Add to the geometry instance list
			geoms[pInst->GetMesh()] = std::string( geomname );
		}

		// Retreive the material property string
		TSTR material_name;
		if( !pInst->GetINode()->GetUserPropString( "rise_material", material_name ) )
		{
			// If there is a rise material set, look at the rise_make_material flag, if that is set, then we are to make a 
			// R.I.S.E. material from the MAX material, otherwise we use the MAX material directly
			BOOL bMakeMaterial = FALSE;
			pInst->GetINode()->GetUserPropBool( "rise_make_material", bMakeMaterial );

			material_name = "color/material::";
			material_name.append( pInst->GetINode()->GetName() );

			if( bMakeMaterial ) {
				Mtl* mtl = pInst->mtl;
				if( mtl ) {
					// Check to see if we've already made this material
					MtlMap::const_iterator it = riseMaterials.find( mtl );
					if( it != riseMaterials.end() ) {
						// We already made this material, re-use it
						material_name = it->second.c_str();
					} else {
						// User didn't set a material, so lets create one
						Interval valid;
						mtl->Update(t,valid);
						Color diffuse = mtl->GetDiffuse();
						Color specular = mtl->GetSpecular();

						const float shininessStrength = mtl->GetShinStr();
						const float shininess = mtl->GetShininess();

						char rd[512] = {0};
						strcpy( rd, material_name );
						strcat( rd, "::diffuse" );
						double d[3] = {diffuse.r, diffuse.g, diffuse.b};
						pJob->AddUniformColorPainter( rd, d, "RGB" );

						if( shininessStrength > 0 && shininess > 0 ) {
							char rs[512] = {0};
							strcpy( rs, material_name );
							strcat( rs, "::specular" );
							double s[3] = {specular.r*shininessStrength, specular.g*shininessStrength, specular.b*shininessStrength};
							pJob->AddUniformColorPainter( rs, s, "RGB" );
							
							char phong[256] = {0};
							sprintf( phong, "%f", shininess*100.0 );

							pJob->AddIsotropicPhongMaterial( material_name, rd, rs, phong );
						} else {
							pJob->AddLambertianMaterial( material_name, rd );
						}
					}
				} else  {
					// This node doesn't even have a basic material, use wireframe color
					double c[3] = {0};
					DWORD dwCol = pInst->GetINode()->GetWireColor();
					c[2] = double((dwCol&0x00FF0000)>>16) / 255.0;
					c[1] = double((dwCol&0x0000FF00)>>8) / 255.0;
					c[0] = double(dwCol&0x000000FF) / 255.0;
					pJob->AddUniformColorPainter( material_name, c, "RGB" );
					pJob->AddLambertianMaterial( material_name, material_name );
				}
			} else {
				// We use the one global MAXMaterial
				material_name = "GloballMAXMaterial";
			}
		}

		// Retreive the shader property string				
		TSTR shader_name;
		bool bShader = !!pInst->GetINode()->GetUserPropString( "rise_shader", shader_name );

		// Retreive the modifer property string
		TSTR modifier_name;
		bool bModifier = !!pInst->GetINode()->GetUserPropString( "rise_modifier", modifier_name );

		// Retreive the radiance map property string
		TSTR radiancemap_name;
		bool bRadianceMap = !!pInst->GetINode()->GetUserPropString( "rise_radiancemap", radiancemap_name );

		// Retreive the radiance map scale property
		float radiancemap_scale = 1.0;
		pInst->GetINode()->GetUserPropFloat( "rise_radiancemap_scale", radiancemap_scale );

		// Retreive the object intersection error property
		float intersection_error = 1e-8f;
		bool bIntersectionError = !!pInst->GetINode()->GetUserPropFloat( "rise_intersection_error", intersection_error );

		// Add the actual object with material and shader identifiers
		{
			// Gets the transformation matrix 
			const Matrix3& tm = pInst->objToWorld;
			Point3 p  = tm.GetTrans();

			double pos[3]={0}, orient[3]={0}, scale[3]={0}, radorient[3]={0};
			if( pJob->AddObject( pInst->GetName(), geomname, material_name, bModifier?modifier_name.data():0, bShader?shader_name.data():0, bRadianceMap?radiancemap_name.data():0, radiancemap_scale, radorient, pos, orient, scale, !!pInst->GetINode()->CastShadows(), !!pInst->GetINode()->RcvShadows() ) ) {

				// Manually set the transformation matrix, since MAX may have done offset transformations
				RISE::IObjectPriv* pObject = pJob->GetObjects()->GetItem( pInst->GetName() );
				pObject->ClearAllTransforms();
	            
				// Start with the translation
				RISE::Matrix4 mx = RISE::Matrix4Ops::Translation( RISE::Vector3( p.x, p.y, p.z ) );

				// Copy the other elements
				{
					mx._00 = tm.GetRow(0).x;
					mx._01 = tm.GetRow(0).y;
					mx._02 = tm.GetRow(0).z;
					mx._10 = tm.GetRow(1).x;
					mx._11 = tm.GetRow(1).y;
					mx._12 = tm.GetRow(1).z;
					mx._20 = tm.GetRow(2).x;
					mx._21 = tm.GetRow(2).y;
					mx._22 = tm.GetRow(2).z;
				}

				pObject->PushTopTransStack( mx );
				pObject->FinalizeTransformations( );

				if( bIntersectionError ) {
					pObject->SetSurfaceIntersecError( intersection_error );
				}
			}
		}

		if( prog ) {
			prog->Progress( n+1, instTab.Count() );
		}
	}


	/////////////////////////////////
	// Setup all the lights
	////////////////////////////////

	// Check the lights
	for( int i = 0; i < lightTab.Count(); i++) {
		RenderLight* light = lightTab[i];
		if( light && light->pDesc ) {
			// Gets the transformation matrix 
			Matrix3 tm = light->pDesc->lightToWorld;
			Point3 p = tm.GetTrans();

			Color col = light->pDesc->ls.color;
			const double intensity = light->pDesc->ls.intens * PI;

			double srgb[3] = {col.r,col.g,col.b};

			char lightname[256] = {0};
			sprintf( lightname, "light%d", i );

			if( light->pDesc->ls.type == OMNI_LGT )
			{
				// OMNI Light --------------
				double pos[3] = {p.x,p.y,p.z};
				pJob->AddPointOmniLight( lightname, intensity, srgb, pos, 0.0, 0.0 );
			}
			else if( light->pDesc->ls.type == SPOT_LGT )
			{
				// SPOT light ----------
				// target is in the negative Z direction.
				Point3 tp = p - 100.0f*tm.GetRow(2);
				double pos[3] = {p.x,p.y,p.z};
				double foc[3] = {tp.x,tp.y,tp.z};
				pJob->AddPointSpotLight( lightname, intensity, srgb, foc, light->pDesc->ls.hotsize*DEG_TO_RAD, light->pDesc->ls.fallsize*DEG_TO_RAD, pos, 0.0, 0.0 );
			}
			else if( light->pDesc->ls.type == DIRECT_LGT )
			{
				// DIRECTIONAL light ----------
				Point3 d = tm.GetRow(2);
				double dir[3] = {d.x,d.y,d.z};
				pJob->AddDirectionalLight( lightname, intensity, srgb, dir );
			}
			else if( light->pDesc->ls.type == AMBIENT_LGT )
			{
				// AMBIENT light ----------
				pJob->AddAmbientLight( lightname, intensity, srgb );
			}
		}
	}

	// Setup the radiance map, check to see if one exists first
	if( !pJob->GetScene()->GetGlobalRadianceMap() ) {
		// Setup one up
		MAXRadianceMap* pRadianceMap = new MAXRadianceMap( this, &rendParams, 1.0 );
		pJob->GetScene()->SetGlobalRadianceMap( pRadianceMap );
		pRadianceMap->release();
	}

	// Setup the atmospheric effect if one exists
	if( rendParams.atmos ) {
		MAXAtmosphere* pAtmosphere = new MAXAtmosphere( this, &rendParams );
		pJob->GetScene()->SetGlobalAtmosphere( pAtmosphere );
		pAtmosphere->release();
	}

	// Load the supplementary file
	if( _access( this->szSupplementarySettingsFile, 0 ) == 0 ) {
		prog->SetTitle( "Loading supplementary scene file" );
		if( !pJob->LoadAsciiScene( this->szSupplementarySettingsFile ) ) {
			MessageBox( hwnd, "Failed to properly load the supplementary settings file, aborting render", GetString(IDS_RENDRISETITLE), MB_OK );
			safe_release( pJob );
			return 0;
		}
	}

	BitmapOutput* bo = 0;
	if( tobm ) {
		bo = new BitmapOutput( *tobm );
		if( !pJob->AddCallbackRasterizerOutput( bo ) ) {
			MessageBox( hwnd, "Failed to set the rasterizer output, did you forget to specify a renderer? Aborting render", GetString(IDS_RENDRISETITLE), MB_OK );
			safe_release( pJob );
			delete bo;
			return 0;
		}
	}

	if( prog ) {
		prog->SetTitle( "Rasterizing Scene..." );
		prog->Progress( 0, 100 );
		pJob->Rasterize(-1, 0);
		prog->Progress( 1, 1 );
	} else {
		pJob->Rasterize(-1, 0);
	}
	
	safe_release( pJob );

	delete bo;

	return 1;
}