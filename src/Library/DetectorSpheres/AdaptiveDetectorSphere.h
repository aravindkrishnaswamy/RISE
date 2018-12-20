//////////////////////////////////////////////////////////////////////
//
//  AdaptiveDetectorSphere.h - An actual detector sphere for a virtual 
//    goniophotometer.  This one changes its geometry as we shoot rays
//    at the collector sphere.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 2, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ADAPTIVE_DETECTOR_SPHERE_
#define ADAPTIVE_DETECTOR_SPHERE_

#include "../Interfaces/IDetectorSphere.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/GeometricUtilities.h"

namespace RISE
{
	namespace Implementation
	{
		//
		// Definition of the Adaptive Detector Sphere
		//
		class AdaptiveDetectorSphere : public virtual IDetectorSphere, public virtual Reference
		{
		public:
			struct PATCH
			{
				Scalar		dThetaBegin;
				Scalar		dThetaEnd;
				Scalar		dPhiBegin;
				Scalar		dPhiEnd;
				Scalar		dRatio;
				Scalar		dSolidProjectedAngle;
			};

			typedef std::vector<PATCH> PatchListType;

		protected:
			//
			// Definition of the PatchQuatree
			//
			class PatchQuatree
			{
			protected:

				//
				// Definition of the PatchQuatreeNode
				//
				class PatchQuatreeNode
				{
				protected:
					bool bChildren;								///< Does this node have children?
					PatchQuatreeNode* pChildren[4];				///< The four children of this node

					struct DeposittedPhoton
					{
						Scalar		dDepositedPower;			///< Amount of depositted power
						Scalar		dTheta;						///< Theta
						Scalar		dPhi;						///< Phi
					};
					
					typedef std::vector<DeposittedPhoton>	DeposittedPhotonsType;
					DeposittedPhotonsType			storedPhotons;
					Scalar							dTotalPower;	///< Total power stored here

				public:
					PatchQuatreeNode();

					virtual ~PatchQuatreeNode();

					void DepositSample(
						const Scalar dThetaBegin,				///< [in] 
						const Scalar dThetaEnd,					///< [in] 
						const Scalar dPhiBegin,					///< [in] 
						const Scalar dPhiEnd,					///< [in] 
						const unsigned int nMaxPatches,			///< [in] Maximum number of patches to tesselate to
						unsigned int& nNumPatches,				///< [out] A place where we can store the number of patches
						const Scalar dThreshold,				///< [in] Amount of power to store in the bin before splitting
						const Scalar dTheta,					///< [in] Angle in $\Theta$ for the sample
						const Scalar dPhi,						///< [in] Angle in $\Phi$ for the sample
						const Scalar dPower						///< [in] Amount of flux in the sample
						);

					void FlattenTree( 
						const Scalar dThetaBegin,				///< [in] 
						const Scalar dThetaEnd,					///< [in] 
						const Scalar dPhiBegin,					///< [in] 
						const Scalar dPhiEnd,					///< [in] 
						PatchListType& patches					///< [out] Vector to store all patches in
					);
				};

				PatchQuatreeNode		root;

				Scalar				m_dThetaBegin;
				Scalar				m_dThetaEnd;
				Scalar				m_dPhiBegin;
				Scalar				m_dPhiEnd;

			public:

				PatchQuatree(
					const Scalar dThetaBegin,
					const Scalar dThetaEnd,
					const Scalar dPhiBegin,
					const Scalar dPhiEnd
					);

				virtual ~PatchQuatree();

				void DepositSample( 
					const unsigned int nMaxPatches,			///< [in] Maximum number of patches to tesselate to
					unsigned int& nNumPatches,				///< [out] A place where we can store the number of patches
					const Scalar dThreshold,				///< [in] Amount of power to store in the bin before splitting
					const Scalar dTheta,					///< [in] Angle in $\Theta$ for the sample
					const Scalar dPhi,						///< [in] Angle in $\Phi$ for the sample
					const Scalar dPower						///< [in] Amount of flux in the sample
					);

				void FlattenTree( 
					PatchListType& patches					///< [out] Vector to store all patches in
					);
			};

		protected:
			PatchQuatree*			m_pRoot;

			PatchListType			m_vTopPatches;
			unsigned int			m_MaxPatches;
			Scalar					m_dRadius;
			Scalar					m_dThreshold;

			const RandomNumberGenerator		random;

			virtual ~AdaptiveDetectorSphere( );	

		public:
			AdaptiveDetectorSphere( );		

			// Initializes the patches of the detector
			virtual void InitPatches( 
				const unsigned int max_patches,								///< [in] Maximum number of patches to tesselate to
				const Scalar radius,										///< [in] Radius of the detector sphere
				const Scalar threshold										///< [in] Amount of power to store in the bin before splitting
				);

			// Performs a set of measurements, this will delete any measurements from before
			//
			// vEmmitterLocation - The exact location in R3 of the emmitter 
			// radiant_power - The radiant power of the emmitter, given in watts
			// pSample - What we are measuring the BRDF of.. this is a material that fill out
			//           the material record properly
			// num_samples - The number of rays we will fire from the emmitter for this
			//             measurement
			//
			virtual void PerformMeasurement( 
				const ISampleGeometry& pEmmitter,							///< [in] Sample geometry of the emmitter
				const ISampleGeometry& pSpecimenGeom,						///< [in] Sample geometry of the specimen
				const Scalar& radiant_power,
				const IMaterial& pSample,
				const unsigned int num_samples,
				const unsigned int samples_base,
				IProgressCallback* pProgressFunc = 0,
				int progress_rate=0
				);

			virtual unsigned int numPatches() const{ return m_vTopPatches.size(); };

			virtual void DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;
			virtual void DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;

			virtual inline const PatchListType& getTopPatches( ) const{ return m_vTopPatches; };
	//		virtual inline PATCH* getBottomPatches( ) const;
		};
	}
}

#endif

