//////////////////////////////////////////////////////////////////////
//
//  biospecbsdfmaker.cpp - This is a utility that makes an RGB bsdf
//    using the biospec model to be used by a data driven bsdf 
//    during rendering.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include "../Library/RISE_API.h"
#include "../Library/DetectorSpheres/IsotropicRGBDetectorSphere.h"
#include "../Library/Parsers/StdOutProgress.h"
#include <string.h>

using namespace RISE;

IPainter* UniformColorPainterFromScalar( const Scalar val )
{
	IPainter* ret = 0;
	RISE_API_CreateUniformColorPainter( &ret, RISEPel( val, val, val ) );
	return ret;
}

int main( int argc, char** argv )
{
	if( argc < 7 ) {
		std::cout << "biospecbsdfmaker - Make .RISEdataBSDF files using the BioSpec model.  You can then use these files with" << std::endl;
		std::cout << "                   the Data Driven Material for faster rendering." << std::endl;
		std::cout << "Usage: biospecbsdfmaker <num patches> <num positions> <num samples> <out file> <melanosomes> <folds ratio>" << std::endl;
		std::cout << "Example: biospecbsdfmaker 10 10 -1 test.RISEdataBSDF 0.1 0.75" << std::endl;
		return 1;
	}

	const unsigned int numPatches = atoi(argv[1]);
	const unsigned int numPositions = atoi(argv[2]);
	int num_samples = atoi(argv[3]);
	const char* outfile = argv[4];

	if( num_samples <= 0 ) {
		// Use the bound to compute the number of samples to take
		const Scalar gamma = 0.1;
		const Scalar epsilon = 0.005;
		num_samples = (unsigned int)ceil(log(2.0/gamma) / (2.0*epsilon*epsilon)) * numPatches;
		std::cout << "Using the bound to compute number of samples: " << num_samples << std::endl;
	}

	// Create the detector sphere
	Implementation::IsotropicRGBDetectorSphere* pDetector = new Implementation::IsotropicRGBDetectorSphere();
	pDetector->InitPatches( numPatches, Implementation::IsotropicRGBDetectorSphere::eEqualAngles );

	IMaterial* pMaterial = 0;

	{
		// Create the biospec material

		Scalar thickness_SC = 0.001;							// 10 - 40 um
		Scalar thickness_epidermis = 0.01;						// 80 - 200 um
		Scalar thickness_papillary_dermis = 0.02;
		Scalar thickness_reticular_dermis = 0.18;
		Scalar ior_SC = 1.55;
		Scalar ior_epidermis = 1.4;
		Scalar ior_papillary_dermis = 1.36;
		Scalar ior_reticular_dermis = 1.38;
		Scalar concentration_eumelanin = 80.0;
		Scalar concentration_pheomelanin = 12.0;
		Scalar melanosomes_in_epidermis = atof(argv[5]);		// dark east indian = 0.1
		Scalar hb_ratio = 0.75;
		Scalar whole_blood_in_papillary_dermis = 0.012;			// 0.2 - 5%
		Scalar whole_blood_in_reticular_dermis = 0.0091;		// 0.2 - 5%
		Scalar bilirubin_concentration = 0.005;					// from 0.005 to 0.5 even 5.0 might be ok g/L
		Scalar betacarotene_concentration_SC = 2.1e-4;
		Scalar betacarotene_concentration_epidermis = 2.1e-4;
		Scalar betacarotene_concentration_dermis = 7.0e-5;
		Scalar folds_aspect_ratio = atof(argv[6]);				// good default is 0.75
		bool bSubdermalLayer = true;

		RISE_API_CreateBioSpecSkinMaterial( 
			&pMaterial,
			*(UniformColorPainterFromScalar( thickness_SC )),
			*(UniformColorPainterFromScalar( thickness_epidermis )),
			*(UniformColorPainterFromScalar( thickness_papillary_dermis )),
			*(UniformColorPainterFromScalar( thickness_reticular_dermis )),
			*(UniformColorPainterFromScalar( ior_SC )),
			*(UniformColorPainterFromScalar( ior_epidermis )),
			*(UniformColorPainterFromScalar( ior_papillary_dermis )),
			*(UniformColorPainterFromScalar( ior_reticular_dermis )),
			*(UniformColorPainterFromScalar( concentration_eumelanin )),
			*(UniformColorPainterFromScalar( concentration_pheomelanin )),
			*(UniformColorPainterFromScalar( melanosomes_in_epidermis )),
			*(UniformColorPainterFromScalar( hb_ratio )),
			*(UniformColorPainterFromScalar( whole_blood_in_papillary_dermis )),
			*(UniformColorPainterFromScalar( whole_blood_in_reticular_dermis )),
			*(UniformColorPainterFromScalar( bilirubin_concentration )),
			*(UniformColorPainterFromScalar( betacarotene_concentration_SC )),
			*(UniformColorPainterFromScalar( betacarotene_concentration_epidermis )),
			*(UniformColorPainterFromScalar( betacarotene_concentration_dermis )),
			*(UniformColorPainterFromScalar( folds_aspect_ratio )),
			bSubdermalLayer
			);
	}

	// Now we run the virtual goniophotometer measurements for each emitter position
	IWriteBuffer* pBuffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &pBuffer, outfile );

	// Marker for a data driven BSDF file
	pBuffer->setInt( 0xBDF );
	pBuffer->setInt( 1 );					// Version of the file
	pBuffer->setInt( numPositions );		// Number of emitter positions
	pBuffer->setInt( numPatches );			// Number of patches
	
	for( unsigned int i=0; i<numPositions; i++ ) {
		char buf[256] = {0};
		sprintf( buf, "Performing measurement %.2d of %.2d: ", i+1, numPositions );
		StdOutProgress	progress( buf );

		const Scalar dEmitterTheta = (PI_OV_TWO/(numPositions-1))*(i);

		pDetector->PerformMeasurement(
			dEmitterTheta,
			1,
			*pMaterial,
			num_samples,
			1,
			true, 
			380.0,
			780.0,
			&progress,
			num_samples>100 ? num_samples/100 : 1			
			);

		// Get the results of this measurement
		Implementation::IsotropicRGBDetectorSphere::PATCH* pTopPatches = pDetector->getTopPatches();
		Implementation::IsotropicRGBDetectorSphere::PATCH* pBottomPatches = pDetector->getBottomPatches();

		pBuffer->setDouble( dEmitterTheta );
		for( unsigned int i=0; i<pDetector->numPatches()/2; i++ )
		{
			pBuffer->setDouble( pTopPatches[i].dThetaBegin );
			pBuffer->setDouble( pTopPatches[i].dThetaEnd );
			{
				ProPhotoRGBPel pel( pTopPatches[i].dRatio );
				pBuffer->setDouble( pel.r );
				pBuffer->setDouble( pel.g );
				pBuffer->setDouble( pel.b );
			}

			pBuffer->setDouble( pBottomPatches[i].dThetaBegin );
			pBuffer->setDouble( pBottomPatches[i].dThetaEnd );
			{
				ProPhotoRGBPel pel( pBottomPatches[i].dRatio );
				pBuffer->setDouble( pel.r );
				pBuffer->setDouble( pel.g );
				pBuffer->setDouble( pel.b );
			}
		}
	}

	pBuffer->release();
}
