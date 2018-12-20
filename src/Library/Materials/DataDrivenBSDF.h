//////////////////////////////////////////////////////////////////////
//
//  DataDrivenBSDF.h - Defines a data driven BSDF, which performs
//    its calculations according to a precomputed data file
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef DATADRIVEN_BSDF_H
#define DATADRIVEN_BSDF_H

#include "../Interfaces/IBSDF.h"
#include "../Utilities/Reference.h"
#include "../Utilities/CubicInterpolator.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class DataDrivenBSDF : 
			public virtual IBSDF, 
			public virtual Reference
		{
		protected:
			virtual ~DataDrivenBSDF();

			struct PATCH
			{
				Scalar dThetaBegin;
				Scalar dThetaEnd;
				Scalar dTheta;
				RISEPel value;
			};

			typedef std::vector<PATCH> PatchValuesListType;

			struct EMITTER_SAMPLE
			{
				Scalar theta;
				PatchValuesListType values;
			};

			typedef std::vector<EMITTER_SAMPLE> BSDFValuesType;

			BSDFValuesType brdf;
			BSDFValuesType btdf;

			CubicInterpolator<RISEPel>* pInterpolator; 

			RISEPel ComputeValueForPatchSet( BSDFValuesType::const_iterator it, int idx, int total, Scalar x ) const;

		public:
			DataDrivenBSDF( const char* filename  );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

