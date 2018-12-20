//////////////////////////////////////////////////////////////////////
//
//  NRooksSampling2D.h - Type of Latin Hypercubes sampling
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef NROOKSSAMPLING_2D_
#define NROOKSSAMPLING_2D_

#include "Sampling2D.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Generates stratified sampling using the N-Rooks algorithm
	//
	namespace Implementation
	{
		class NRooksSampling2D : public virtual Sampling2D, public virtual Reference
		{
		protected:
			Scalar dHowFar;		// How far from the center of strata are we allowed
								// to generate samples to ?
			struct SAMPLE_ELEMENT
			{
				Point2			vSample;
				int				iRow;
				int				iCol;
			};

			struct SAMPLE_ROW_VALID
			{
				unsigned int		col;
				bool				valid;
				SAMPLE_ROW_VALID( unsigned int a, bool b ) : col( a ), valid( b ){};
			};

			typedef std::vector<SAMPLE_ELEMENT> SampleElementList;
			typedef std::vector<SAMPLE_ROW_VALID> SampleRowValidList;

			virtual ~NRooksSampling2D( );

			unsigned int FindAndSet( SampleRowValidList& v, int idx ) const;
			bool IsGoodPlace( const SampleElementList& samples, int iRow, int iCol ) const;

		public:
			NRooksSampling2D( Scalar _dSpaceWidth, Scalar _dSpaceHeight, Scalar _dHowFar );

			virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samplePoints ) const;
			virtual ISampling2D* Clone( ) const;
		};
	}
}

#endif
