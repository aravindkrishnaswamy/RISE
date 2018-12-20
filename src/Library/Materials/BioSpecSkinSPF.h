//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinSPF.h - Declaration of the SPF for the BioSpec skin
//    model.  The SPF is where all the action happens since there is
//    no IBSDF implementation for BioSpec
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_SKIN_SPF_
#define BIOSPEC_SKIN_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class BioSpecSkinSPF : public virtual ISPF, public virtual Implementation::Reference
		{
			friend class GenericHumanTissueSPF;

		protected:
			//////////////////////////////////////////////////////////////////////////
			// Member variables
			//////////////////////////////////////////////////////////////////////////

			/////////////////////////////////////////////////
			// Parameters passed into the model
			/////////////////////////////////////////////////
			const IPainter&					pnt_thickness_SC;							///< Thickness of the stratum corneum (in cm)
			const IPainter&					pnt_thickness_epidermis;					///< Thickness of the epidermis (in cm)
			const IPainter&					pnt_thickness_papillary_dermis;				///< Thickness of the papillary dermis (in cm)
			const IPainter&					pnt_thickness_reticular_dermis;				///< Thickness of the reticular layer (in cm)

			const IPainter&					pnt_ior_SC;									///< Index of refraction of the stratum corneum
			const IPainter&					pnt_ior_epidermis;							///< Index of refraction of the epidermis
			const IPainter&					pnt_ior_papillary_dermis;					///< Index of refraction of the papillary dermis
			const IPainter&					pnt_ior_reticular_dermis;					///< Index of refraction of the reticular layer

			const IPainter&					pnt_concentration_eumelanin;				///< Average Concentration of eumelanin in the melanosomes
			const IPainter&					pnt_concentration_pheomelanin;				///< Average Concentration of pheomelan in in the melanosomes

			const IPainter&					pnt_melanosomes_in_epidermis;				///< Percentage of the epidermis composed of melanosomes

			const IPainter&					pnt_hb_ratio;								///< Oxy/deoxy hemoglobin ratio
			const IPainter&					pnt_whole_blood_in_papillary_dermis;		///< Percentage of the papillary dermis composed of whole blood
			const IPainter&					pnt_whole_blood_in_reticular_dermis;		///< Percentage of the reticular layer composed of whole blood

			const IPainter&					pnt_betacarotene_concentration_SC;			///< Concentration of beta-carotene in the stratum corneum
			const IPainter&					pnt_betacarotene_concentration_epidermis;	///< Concentration of beta-carotene in the epidermis
			const IPainter&					pnt_betacarotene_concentration_dermis;		///< Concentration of beta-carotene in the dermis

			const IPainter&					pnt_bilirubin_concentration;				///< Concentration of bilirubin in whole blood

			const IPainter&					pnt_folds_aspect_ratio;						///< Aspect ratio of the skin wrinkles and folds
			const bool						subdermal_layer;							///< Should we simulate a reflective subdermal layer?

			/////////////////////////////////////////////////
			// Parameters set by the model
			/////////////////////////////////////////////////

			Scalar						hb_concentration;				///< Concentration of hemoglobin in whole blood

			/////////////////////////////////////////////////
			// Variables used internally
			/////////////////////////////////////////////////
			IFunction2D*				pSCscattering;					///< Lookup table for stratum corneum scattering (at epidermal boudary)
			IFunction2D*				pEpidermisScat;					///< Lookup table for epidermal scattering

			IFunction1D*				pEumelaninExt;					///< Lookup table for eumelanin extinction (from absorption)
			IFunction1D*				pPheomelaninExt;				///< Lookup table for pheomelanin extinction (from absorption)

			IFunction1D*				pOxyHemoglobinExt;				///< Lookup table for oxyhemoglobin extinction (from absorption)
			IFunction1D*				pDeoxyHemoglobinExt;			///< Lookup table for deoxyhemoglobin extinction (from absorption)

			IFunction1D*				pBilirubinExt;					///< Lookup table for bilirubin extinction (from absorption)
			IFunction1D*				pBetaCaroteneExt;				///< Lookup table for beta-carotene extinction (from absorption)


			////////////////////////////////////////////////
			// Run-time variables
			////////////////////////////////////////////////
			
			// These variables are used in the runtime, they are basically
			// instance specific values for the painters above
			mutable Scalar					thickness_SC;							///< Thickness of the stratum corneum (in cm)
			mutable Scalar					thickness_epidermis;					///< Thickness of the epidermis (in cm)
			mutable Scalar					thickness_papillary_dermis;				///< Thickness of the papillary dermis (in cm)
			mutable Scalar					thickness_reticular_dermis;				///< Thickness of the reticular layer (in cm)

			mutable Scalar					ior_SC;									///< Index of refraction of the stratum corneum
			mutable Scalar					ior_epidermis;							///< Index of refraction of the epidermis
			mutable Scalar					ior_papillary_dermis;					///< Index of refraction of the papillary dermis
			mutable Scalar					ior_reticular_dermis;					///< Index of refraction of the reticular layer

			mutable Scalar					concentration_eumelanin;				///< Average Concentration of eumelanin in the melanosomes
			mutable Scalar					concentration_pheomelanin;				///< Average Concentration of pheomelan in in the melanosomes

			mutable Scalar					melanosomes_in_epidermis;				///< Percentage of the epidermis composed of melanosomes

			mutable Scalar					hb_ratio;								///< Oxy/deoxy hemoglobin ratio
			mutable Scalar					whole_blood_in_papillary_dermis;		///< Percentage of the papillary dermis composed of whole blood
			mutable Scalar					whole_blood_in_reticular_dermis;		///< Percentage of the reticular layer composed of whole blood

			mutable Scalar					betacarotene_concentration_SC;			///< Concentration of beta-carotene in the stratum corneum
			mutable Scalar					betacarotene_concentration_epidermis;	///< Concentration of beta-carotene in the epidermis
			mutable Scalar					betacarotene_concentration_dermis;		///< Concentration of beta-carotene in the dermis

			mutable Scalar					bilirubin_concentration;				///< Concentration of bilirubin in whole blood

			mutable Scalar					folds_aspect_ratio;						///< Aspect ratio of the skin wrinkles and folds


			virtual ~BioSpecSkinSPF();

			//////////////////////////////////////////////////////////////////////////
			// Helper functions
			//////////////////////////////////////////////////////////////////////////

			/////////////////////////////////////////////////
			// Scattering stuff
			/////////////////////////////////////////////////


			//! Scatters according to a Trowbridge-Reitz PDF
			static Vector3 TrowbridgeReitz_Scattering(
				const RandomNumberGenerator& random,		///< [in] Random number generator
				const Vector3& incoming,							///< [in] The direction of the incoming ray
				const Scalar aspect_ratio							///< [in] Aspect ratio of the cells
				);

			//! Scatters a ray at the stratum corneum 
			/// \return The direction of the outgoing ray
			inline Vector3 StratumCorneum_Cell_Scattering( 
				const RandomNumberGenerator& random,		///< [in] Random number generator
				const Vector3& incoming								///< [in] The direction of the incoming ray
				) const
			{
				return TrowbridgeReitz_Scattering( random, incoming, folds_aspect_ratio );
			}

			//! Scatters accoding to a lookup which is described in the function
			static Vector3 Scattering_From_TableLookup( 
				const Scalar nm,										///< [in] The wavelength to do the lookup for
				const RandomNumberGenerator& random,			///< [in] Random number generator
				const Vector3& incoming,								///< [in] The direction of the incoming ray
				const IFunction2D& pFunc								///< [in] Function to use for doing the scattering
				);

			//! Scatters according to the Henyey-Greenstein phase function
			static Vector3 Scattering_From_HenyeyGreenstein(
				const RandomNumberGenerator& random,			///< [in] Random number generator
				const Vector3& incoming,								///< [in] The direction of the incoming ray
				const Scalar g											///< [in] The asymmetry factor
				);
			
			//! Scatters a ray at the epidermis using a table lookup
			inline Vector3 StratumCorneum_Scattering( 
				const Scalar nm,										///< [in] The wavelength to do the lookup for
				const RandomNumberGenerator& random,			///< [in] Random number generator
				const Vector3& incoming									///< [in] The direction of the incoming ray
				) const
			{
				return Scattering_From_TableLookup( nm, random, incoming, *pSCscattering );
		//		return Scattering_From_HenyeyGreenstein( random, incoming, 0.917 );
			}

			//! Scatters a ray at the epidermis using a table lookup
			inline Vector3 Epidermis_Scattering( 
				const Scalar nm,										///< [in] The wavelength to do the lookup for
				const RandomNumberGenerator& random,			///< [in] Random number generator
				const Vector3& incoming									///< [in] The direction of the incoming ray
				) const
			{
				return Scattering_From_TableLookup( nm, random, incoming, *pEpidermisScat );
		//		return Scattering_From_HenyeyGreenstein( random, incoming, 0.781 );
			}

			//! Scatters a ray at the dermis (both papillary and reticular layer)
			static Vector3 Dermis_Scattering( 
				const RandomNumberGenerator& random,		///< [in] Random number generator
				const Vector3& incoming								///< [in] The direction of the incoming ray
				);

			//! Scatters a ray by using the Rayleigh phase function
			static Vector3 Rayleigh_Phase_Function_Scattering( 
				const RandomNumberGenerator& random,		///< [in] Random number generator
				const Vector3& incoming								///< [in] The direction of the incoming ray
				);


			/////////////////////////////////////////////////
			// Absorption stuff
			/////////////////////////////////////////////////
			
			//! Given the extinction / cm / mg*ml and the concentration in mg*ml, returns
			//! the extinctin / cm
			static inline Scalar ComputeExtinction( 
				const Scalar ext,										///< [in] Extinction factor of the pigment
				const Scalar concentration								///< [in] Concentration of the pigment
				)
			{
				return ext * concentration;
			}

			//! Computes the baseline absorption coefficient of skin
			static inline Scalar ComputeSkinBaselineAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				)
			{
				return (0.244 + 85.3 * exp( -(nm-154.0)/66.2));
			};

			static inline Scalar ComputeSkinBaselineAbsorptionCoefficient2( 
				const Scalar nm											///< [in] Wavelength of light to consider
				)
			{
				return (7.84e8 * pow(nm, -3.255) );
			};

			//! Given the extinction / cm and the depth (in cm), returns the total extinction
			static inline Scalar ComputeTotalExtinction( 
				const Scalar od,										///< [in] Absorption of the pigment
				const Scalar amount										///< [in] Amount travelled in pigment
				)
			{
				return od * amount;
			}

			//! Given the total extinction, computes the transmittance
			static inline Scalar ComputeTransmittance( 
				const Scalar total_ext									///< [in] Total extinction
				)
			{
				return exp( -1.0 * total_ext );
			}

			//! Computes the absorption coefficient for melanin
			static inline Scalar ComputeMelaninAbsorptionCoefficient( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const IFunction1D* pFunc,								///< [in] The function that represents the extinction
				const Scalar concentration								///< [in] Concentration of the melanin
				)
			{
				return ComputeExtinction( pFunc->Evaluate(nm), concentration );
			}

			//! Computes the total transmittance for a given melanin for a particular distance
			//! with a particular concentration for a particular wavelength
			static inline Scalar ComputeMelaninTransmittance( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const IFunction1D* pFunc,								///< [in] The function that represents the extinction
				const Scalar distance,									///< [in] Amount travelled in pigment
				const Scalar concentration								///< [in] Concentration of the melanin
				)
			{
				return ComputeTransmittance( ComputeTotalExtinction( ComputeMelaninAbsorptionCoefficient( nm, pFunc, concentration ), distance ) );
			}

			//! Computes the absorption coefficient for eumelanin
			inline Scalar ComputeEumelaninAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeMelaninAbsorptionCoefficient( nm, pEumelaninExt, concentration_eumelanin );
			}

			//! Computes the absorption coefficient for pheomelanin
			inline Scalar ComputePheomelaninAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeMelaninAbsorptionCoefficient( nm, pPheomelaninExt, concentration_pheomelanin );
			}

			//! Computes the total transmittance for eumelanin
			inline Scalar ComputeEumelaninTransmittance( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const Scalar distance									///< [in] Amount travelled in pigment
				) const
			{
				return ComputeMelaninTransmittance( nm, pEumelaninExt, distance, concentration_eumelanin );
			}

			//! Computes the total transmittance for pheomelanin
			inline Scalar ComputePheomelaninTransmittance( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const Scalar distance									///< [in] Amount travelled in pigment
				) const
			{
				return ComputeMelaninTransmittance( nm, pPheomelaninExt, distance, concentration_pheomelanin );
			}

			//! Computes the absorption coefficient for the epidermis
			inline Scalar ComputeEpidermisAbsorptionCoefficient(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				const Scalar abs_eumelanin = ComputeEumelaninAbsorptionCoefficient( nm );
				const Scalar abs_pheomelanin = ComputePheomelaninAbsorptionCoefficient( nm );
				const Scalar abs_carotene = ComputeBetaCaroteneAbsorptionCoefficientEpidermis( nm );

				return ((abs_eumelanin+abs_pheomelanin)*melanosomes_in_epidermis)+abs_carotene*(1.0-melanosomes_in_epidermis);
			}

			//! Computes the absorption coefficient for hemoglobin
			static inline Scalar ComputeHemoglobinAbsorptionCoefficient( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const IFunction1D* pFunc,								///< [in] The function that represents the molar extinction coefficient
				const Scalar concentration								///< [in] Concentration of hemoglobin (in g/L)
				)
			{
				return ( (pFunc->Evaluate(nm) * concentration) / 66500.0);		// 66,500 g/mole is the gram molecular weight of hemoglobin
			}

			// Computes the absorption coefficient for oxyhemoglobin
			inline Scalar ComputeOxyHemoglobinAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeHemoglobinAbsorptionCoefficient( nm, pOxyHemoglobinExt, hb_concentration );
			}

			// Computes the absorption coeffcient for deoxyhemoglobin
			inline Scalar ComputeDeoxyHemoglobinAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeHemoglobinAbsorptionCoefficient( nm, pDeoxyHemoglobinExt, hb_concentration );
			}

			// Computes the absorption coefficient for bilirubin
			inline Scalar ComputeBilirubinAbsorptionCoefficient(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return pBilirubinExt->Evaluate(nm) * bilirubin_concentration / 585.0;
			}

			// Computes the absorption coefficient for beta-carotene in generate
			inline Scalar ComputeBetaCaroteneAbsorptionCoefficient(
				const Scalar nm,										///< [in] Wavelength of light to consider
				const Scalar concentration								///< [in] Concentration
				) const
			{
				return pBetaCaroteneExt->Evaluate(nm) * concentration / 537.0;
			}

			// Computes the absorption coefficient for beta-carotene in the stratum corneum
			inline Scalar ComputeBetaCaroteneAbsorptionCoefficientStratumCorneum(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeBetaCaroteneAbsorptionCoefficient( nm, betacarotene_concentration_SC );
			}

			// Computes the absorption coefficient for beta-carotene in the epidermis
			inline Scalar ComputeBetaCaroteneAbsorptionCoefficientEpidermis(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeBetaCaroteneAbsorptionCoefficient( nm, betacarotene_concentration_epidermis );
			}

			// Computes the absorption coefficient for beta-carotene in the epidermis
			inline Scalar ComputeBetaCaroteneAbsorptionCoefficientDermis(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const
			{
				return ComputeBetaCaroteneAbsorptionCoefficient( nm, betacarotene_concentration_dermis );
			}

			// Computes the absorption coefficient for the papillary dermis
			inline Scalar ComputeReticularDermisAbsorptionCoefficient(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const 
			{
				const Scalar abs_hbo2 = ComputeOxyHemoglobinAbsorptionCoefficient( nm ) * hb_ratio;
				const Scalar abs_hb = ComputeDeoxyHemoglobinAbsorptionCoefficient( nm )* (1.0-hb_ratio);
				const Scalar abs_bilirubin = ComputeBilirubinAbsorptionCoefficient( nm );
				const Scalar abs_carotene = ComputeBetaCaroteneAbsorptionCoefficientDermis( nm );

				return ((abs_hbo2+abs_hb+abs_bilirubin)*whole_blood_in_papillary_dermis)+abs_carotene;
			}

			// Computes the absorption coefficient for the reticular dermis
			inline Scalar ComputePapillaryDermisAbsorptionCoefficient(
				const Scalar nm											///< [in] Wavelength of light to consider
				) const 
			{
				const Scalar abs_hbo2 = ComputeOxyHemoglobinAbsorptionCoefficient( nm ) * hb_ratio;
				const Scalar abs_hb = ComputeDeoxyHemoglobinAbsorptionCoefficient( nm )* (1.0-hb_ratio);
				const Scalar abs_bilirubin = ComputeBilirubinAbsorptionCoefficient( nm );
				const Scalar abs_carotene = ComputeBetaCaroteneAbsorptionCoefficientDermis( nm );

				return ((abs_hbo2+abs_hb+abs_bilirubin)*whole_blood_in_reticular_dermis)+abs_carotene;
			}

			static inline Scalar ComputeBeta( 
				const Scalar lambda,									///< [in] Wavelength of light to consider
				const Scalar ior_medium									///< [in] Index of refraction of the medium
				)
			{
				static const double ior_collangen = 1.5;
				static const double ior_diff = ior_collangen/ior_medium;
				static const double ior_factor = pow( ior_diff*ior_diff-1.0, 2.0 );

				static const double PI_3 = ::pow( PI, 3.0 );  

				static const double sphere_radius = 5e-6/2.0;
				static const double sphere_volume = 4.0/3.0 * pow(sphere_radius,3.0) * PI;
				static const double num_fibers = 1.0 / sphere_volume * 0.21;

				double a = 8.0 * PI_3 * ior_factor;
				double b = 3.0 * pow( lambda, 4.0 );
				double c = b * num_fibers;

				return a / c;
			}

			// Computes the probability of a photon being Rayleigh scattered
			Scalar ComputeRayleighScatteringProbability( 
				const Scalar nm,										///< [in] Wavelength of light to consider
				const Scalar ior_medium,								///< [in] Index of refraction of the medium
				const Scalar distance									///< [in] Amount travellened in the pigment (in cm)	
				) const
			{
				return ComputeTransmittance( ComputeTotalExtinction( ComputeBeta( nm*1e-6, ior_medium ), distance ) );
			}


			/////////////////////////////////////////////////
			// Refraction
			/////////////////////////////////////////////////

			//! Refraction between two layers of skin (at their boundaries)
			/// \return The absolute reflectance
			static Scalar Boundary_Refraction (
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const Vector3& n,										///< [in] The normal
				const Scalar ior_from,									///< [in] Index of refraction of where coming from
				const Scalar ior_to										///< [in] Index of refraction of where going to
				);

			//! Refraction between outside and stratum corneum
			inline Scalar Outside_SC_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb,							///< [in] Orthonormal basis of the skin
				const Scalar ior_outside								///< [in] Index of refraction of the outside of skin (ie. air)
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_outside, ior_SC );
			}

			//! Refraction between stratum corneum and outside
			inline Scalar SC_Outside_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb,							///< [in] Orthonormal basis of the skin
				const Scalar ior_outside								///< [in] Index of refraction of the outside of skin (ie. air)
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_SC, ior_outside );
			}

			//! Refraction between stratum corneum and epidermis
			inline Scalar SC_Epidermis_Boundary_Refraction( 
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_SC, ior_epidermis );
			}

			//! Refraction between epidermis and startum corneum
			inline Scalar Epidermis_SC_Boundary_Refraction( 
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_epidermis, ior_SC );
			}

			//! Refraction between the epidermis and air
			inline Scalar Epidermis_Outside_Boundary_Refraction( 
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb,							///< [in] Orthonormal basis of the skin
				const Scalar ior_outside								///< [in] Index of refraction of the outside of skin (ie. air)
				) const 
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_epidermis, ior_outside );
			}

			//! Refraction between the epidermis and the papillary dermis
			inline Scalar Epidermis_PapillaryDermis_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_epidermis, ior_papillary_dermis );
			}

			//! Refraction between the papillary dermis and the epidermis
			inline Scalar PapillaryDermis_Epidermis_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_papillary_dermis, ior_epidermis );
			}

			//! Refraction between the papillary dermis and the reticular layer
			inline Scalar PapillaryDermis_ReticularLayer_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_papillary_dermis, ior_reticular_dermis );
			}

			//! Refraction between the reticular layer and the papillary dermis
			inline Scalar ReticularLayer_PapillaryDermis_Boundary_Refraction(
				const Vector3& incoming,								///< [in] Direction of incoming ray
				Vector3& outgoing,										///< [out] Direction of outgoing ray
				const OrthonormalBasis3D& onb							///< [in] Orthonormal basis of the skin
				) const
			{
				return Boundary_Refraction( incoming, outgoing, onb.w(), ior_reticular_dermis, ior_papillary_dermis );
			}

			/////////////////////////////////////////////////
			// Recursive Monte Carlo routines
			/////////////////////////////////////////////////

			//! Does stratum corneum interactions
			//! Can pass the photon off to the epidermis, or can return the photon back
			//! \return TRUE if the photon was absorbed, FALSE otherwise
			bool ProcessSCInteraction( 
				const Scalar nm,											///< The wavelength we are working in
				const Vector3& photon_in,									///< The photon we are starting with
				Vector3& photon_out,										///< The photon coming out of the layer
				const OrthonormalBasis3D& onb,								///< The orthonormal basis
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				const bool bAtOutsideBoundary,								///< The photon is at the boundary with the outside of the skin,
																			///< if false, it is at the epidermal boundary
				const bool bDoCellScattering,								///< Should we apply the Trowbridge cell scattering
				const bool bDoFresnel										///< Should we do a Fresnel check?
				) const;

			//! Does epidermal interactions
			//! Can pass the photon off to the papillary dermis or to the stratum corneum
			//! \return TRUE if the photon was absorbed, FALSE otherwise
			bool ProcessEpidermisInteraction(
				const Scalar nm,											///< The wavelength we are working in
				const Vector3& photon_in,									///< The photon we are starting with
				Vector3& photon_out,										///< The photon coming out of the layer
				const OrthonormalBasis3D& onb,								///< The orthonormal basis
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				const bool bAtSCBoundary,									///< The photon is at the boundary with the stratum corneum,
																			///< if false, it is at the dermal boundary
				const bool bDoFresnel										///< Should we do a Fresnel check?
				) const;

			//! Does papillary dermal interactions
			//! Can pass the photon off to the epidermis or reticular dermis
			//! \return TRUE if the photon was absorbed, FALSE otherwise
			bool ProcessPapillaryDermisInteraction(
				const Scalar nm,											///< The wavelength we are working in
				const Vector3& photon_in,									///< The photon we are starting with
				Vector3& photon_out,										///< The photon coming out of the layer
				const OrthonormalBasis3D& onb,								///< The orthonormal basis
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				const bool bAtEpidermisBoundary,							///< The photon is at the boundary with the epidermis,
																			///< if false, it is at the reticular dermis boundary
				const bool bDoFresnel										///< Should we do a Fresnel check?
				) const;
			
			//! Does reticular dermal interactions
			//! Can pass the photon off to the epidermis or reticular dermis
			//! \return TRUE if the photon was absorbed, FALSE otherwise
			bool ProcessReticularDermisInteraction(
				const Scalar nm,											///< The wavelength we are working in
				const Vector3& photon_in,									///< The photon we are starting with
				Vector3& photon_out,										///< The photon coming out of the layer
				const OrthonormalBasis3D& onb,								///< The orthonormal basis
				const RandomNumberGenerator& random,				///< Random number generator for the MC process
				const bool bAtPapillaryDermisBoundary,						///< The photon is at the boundary with the papillary dermis,
																			///< if false, it is at the subdermal boundary
				const bool bDoFresnel										///< Should we do a Fresnel check?
				) const;

			//! Does subdermal interactions
			//! Can pass the photon off to the dermis
			//! \return TRUE if the photon was absorbed, FALSE otherwise
			bool ProcessSubdermisInteraction(
				const Scalar nm,											///< The wavelength we are working in
				const Vector3& photon_in,									///< The photon we are starting with
				Vector3& photon_out,										///< The photon coming out of the layer
				const OrthonormalBasis3D& onb,								///< The orthonormal basis
				const RandomNumberGenerator& random				///< Random number generator for the MC process
				) const;

		public:

			BioSpecSkinSPF(
				const IPainter& thickness_SC_,								///< Thickness of the stratum corneum (in cm)
				const IPainter& thickness_epidermis_,						///< Thickness of the epidermis (in cm)
				const IPainter& thickness_papillary_dermis_,				///< Thickness of the papillary dermis (in cm)
				const IPainter& thickness_reticular_dermis_,				///< Thickness of the reticular dermis (in cm)
				const IPainter& ior_SC_,									///< Index of refraction of the stratum corneum
				const IPainter& ior_epidermis_,								///< Index of refraction of the epidermis
				const IPainter& ior_papillary_dermis_,						///< Index of refraction of the papillary dermis
				const IPainter& ior_reticular_dermis_,						///< Index of refraction of the reticular dermis
				const IPainter& concentration_eumelanin_,					///< Average Concentration of eumelanin in the melanosomes
				const IPainter& concentration_pheomelanin_,					///< Average Concentration of pheomelanin in the melanosomes
				const IPainter& melanosomes_in_epidermis_,					///< Percentage of the epidermis made up of melanosomes
				const IPainter& hb_ratio_,									///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
				const IPainter& whole_blood_in_papillary_dermis_,			///< Percentage of the papillary dermis made up of whole blood
				const IPainter& whole_blood_in_reticular_dermis_,			///< Percentage of the reticular dermis made up of whole blood
				const IPainter& bilirubin_concentration_,					///< Concentration of Bilirubin in whole blood
				const IPainter& betacarotene_concentration_SC_,				///< Concentration of Beta-Carotene in the stratum corneum
				const IPainter& betacarotene_concentration_epidermis_,		///< Concentration of Beta-Carotene in the epidermis
				const IPainter& betacarotene_concentration_dermis_,			///< Concentration of Beta-Carotene in the dermis
				const IPainter& folds_aspect_ratio_,						///< Aspect ratio of the little folds and wrinkles on the skin surface
				const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
				);

			void Scatter( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			void ScatterNM( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;	
		};
	}
}

#endif

