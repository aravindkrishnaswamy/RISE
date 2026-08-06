//////////////////////////////////////////////////////////////////////
//
//  FireOptics.h - Versioned fire constituent optical records
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 6, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREOPTICS_
#define FIREOPTICS_

#include "RISECBOR64.h"
#include <string>
#include <vector>

namespace RISE
{
	struct SpectralDerivativeEnclosure
	{
		double wavelengthMinNM;
		double wavelengthMaxNM;
		double derivativeMin;
		double derivativeMax;
	};

	class DifferentiableSpectrum
	{
		std::vector<double> m_wavelengths;
		std::vector<double> m_values;
		std::vector<double> m_slopes;
		std::vector<SpectralDerivativeEnclosure> m_enclosures;
		bool m_valid;

	public:
		DifferentiableSpectrum();

		bool Initialize(
			const std::vector<double>& wavelengths,
			const std::vector<double>& values,
			const std::vector<double>& slopes,
			const std::vector<SpectralDerivativeEnclosure>& enclosures,
			std::string* error = 0
			);

		bool IsValid() const { return m_valid; }
		bool Contains( double wavelengthNM ) const;
		double Evaluate( double wavelengthNM ) const;
		double Derivative( double wavelengthNM ) const;

		const std::vector<double>& Wavelengths() const { return m_wavelengths; }
		const std::vector<double>& Values() const { return m_values; }
		const std::vector<double>& Slopes() const { return m_slopes; }
		const std::vector<SpectralDerivativeEnclosure>& DerivativeEnclosures() const
		{
			return m_enclosures;
		}
	};

	struct FireFidelityEvaluation
	{
		bool predictiveAllowed;
		std::string renderFidelityStatus;
		std::vector<std::string> reasonCodes;
	};

	class FireOpticsPreset
	{
	public:
		enum RecordClass
		{
			InvalidRecord,
			PredictiveOpticalPreset,
			SyntheticRegressionFixture
		};

	private:
		bool m_valid;
		RecordClass m_recordClass;
		std::string m_recordName;
		std::string m_recordId;
		RISECBOR64::Bytes m_recordBytes;
		double m_domainMinNM;
		double m_domainMaxNM;
		double m_hotFractionMinK;
		double m_hotFractionMaxK;
		double m_densityGCM3;
		double m_constantEffectiveAbsorption;
		DifferentiableSpectrum m_mac;
		DifferentiableSpectrum m_hotOmega;
		DifferentiableSpectrum m_hotG;
		double m_coolKm633;
		double m_coolExponent;
		double m_coolOmega;
		double m_coolG;
		double m_coolDomainMinNM;
		double m_coolDomainMaxNM;
		std::string m_coolOutOfDomainPolicy;
		DifferentiableSpectrum m_condensedKm;
		DifferentiableSpectrum m_condensedOmega;
		DifferentiableSpectrum m_condensedG;
		double m_condensedFixtureKm633;
		double m_condensedFixtureExponent;
		double m_condensedFixtureOmega;
		double m_condensedFixtureG;
		double m_condensedPreviewExponent;
		std::string m_condensedApplicability;
		std::string m_condensedIRClosureStatus;
		std::string m_condensedPredictiveReason;

		bool LoadSemanticRecord(
			const RISECBOR64::Value& record,
			std::string* error
			);

	public:
		FireOpticsPreset();

		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		static const FireOpticsPreset& PredictiveV1();
		static const FireOpticsPreset& SyntheticRegressionV1();
		static const FireOpticsPreset* Named( const char* name );

		static FireOpticsPreset CreateExplicitSyntheticFixture(
			double sootEffectiveAbsorption,
			double sootDensityKgM3,
			double hotAlbedo,
			double hotG,
			double coolKm633,
			double coolExponent,
			double coolAlbedo,
			double coolG,
			double condensedKm633,
			double condensedExponent,
			double condensedAlbedo,
			double condensedG,
			std::string* error = 0
			);

		bool IsValid() const { return m_valid; }
		bool IsSynthetic() const
		{
			return m_recordClass == SyntheticRegressionFixture;
		}
		RecordClass GetRecordClass() const { return m_recordClass; }
		const std::string& RecordName() const { return m_recordName; }
		const std::string& RecordId() const { return m_recordId; }
		const RISECBOR64::Bytes& RecordBytes() const { return m_recordBytes; }
		const std::string& CondensedApplicability() const
		{
			return m_condensedApplicability;
		}
		const std::string& CondensedIRClosureStatus() const
		{
			return m_condensedIRClosureStatus;
		}
		double DomainMinNM() const { return m_domainMinNM; }
		double DomainMaxNM() const { return m_domainMaxNM; }
		double CoolCarbonDomainMinNM() const { return m_coolDomainMinNM; }
		double CoolCarbonDomainMaxNM() const { return m_coolDomainMaxNM; }
		bool SupportsWavelengthRange( double minimumNM, double maximumNM ) const;
		double HotFractionMinK() const { return m_hotFractionMinK; }
		double HotFractionMaxK() const { return m_hotFractionMaxK; }
		double HotFraction( double temperatureK ) const;
		double SootDensityGCM3() const { return m_densityGCM3; }
		double SootDensityKgM3() const { return m_densityGCM3*1000.0; }

		double MAC( double wavelengthNM ) const;
		double EffectiveAbsorption( double wavelengthNM ) const;
		double EffectiveAbsorptionDerivative( double wavelengthNM ) const;
		double HotAbsorptionMass( double wavelengthNM ) const;
		double HotAlbedo( double wavelengthNM ) const;
		double HotG( double wavelengthNM ) const;
		double HotExtinctionMass( double wavelengthNM ) const;
		double CoolExtinctionMass( double wavelengthNM ) const;
		double CoolAlbedo( double wavelengthNM ) const;
		double CoolG( double wavelengthNM ) const;
		double CondensedExtinctionMass( double wavelengthNM ) const;
		double CondensedAlbedo( double wavelengthNM ) const;
		double CondensedG( double wavelengthNM ) const;

		double MaximumExtinctionMassVisible() const;
		double MaximumHotAbsorptionMassVisible() const;
		double MaximumCoolAbsorptionMassVisible() const;
		double MaximumCondensedAbsorptionMassVisible() const;

		double PelApproximateEffectiveAbsorption633() const
		{
			return EffectiveAbsorption(633.0);
		}
		double PelApproximateHotAlbedo() const { return HotAlbedo(550.0); }
		double PelApproximateHotG() const { return HotG(550.0); }
		double PelApproximateCoolKm633() const { return m_coolKm633; }
		double PelApproximateCoolExponent() const { return m_coolExponent; }
		double PelApproximateCoolAlbedo() const { return m_coolOmega; }
		double PelApproximateCoolG() const { return m_coolG; }
		double PelApproximateCondensedKm633() const;
		double PelApproximateCondensedExponent() const;
		double PelApproximateCondensedAlbedo() const;
		double PelApproximateCondensedG() const;

		const DifferentiableSpectrum& MACSpectrum() const { return m_mac; }
		const DifferentiableSpectrum& HotAlbedoSpectrum() const { return m_hotOmega; }
		const DifferentiableSpectrum& HotGSpectrum() const { return m_hotG; }
		const DifferentiableSpectrum& CondensedExtinctionSpectrum() const
		{
			return m_condensedKm;
		}

		FireFidelityEvaluation EvaluateFidelity(
			bool predictiveRequested,
			bool hasNonzeroCondensedInventory,
			bool hasChemChannels
			) const;
	};
}

#endif
