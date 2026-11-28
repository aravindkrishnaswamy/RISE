//////////////////////////////////////////////////////////////////////
//
//  FireOptics.h - Versioned fire constituent optical records
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 6, 2026
//  Tabs: 4
//  Comments:
//    Canonical CBOR64-backed optical property records for fire and
//    smoke constituents (hot soot, cool carbon smoke, and condensed
//    organics), monotone C1 PCHIP spectral curves with certified
//    derivative enclosures, temperature partitioning phi(T), and
//    predictive vs. preview fidelity gating.
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
	//! Certified [derivativeMin, derivativeMax] enclosure of d(value)/d(x)
	//! over one knot interval [wavelengthMinNM, wavelengthMaxNM] (also reused
	//! for temperature-axis PCHIP curves in transport records).
	struct SpectralDerivativeEnclosure
	{
		double wavelengthMinNM;
		double wavelengthMaxNM;
		double derivativeMin;
		double derivativeMax;
	};

	//! Monotone C1 PCHIP cubic-Hermite curve over strictly increasing knots
	//! (wavelength in nm, or temperature in K for transport records).
	//! Evaluate() and Derivative() clamp at domain endpoints; callers enforcing
	//! strict out-of-domain rejection must check Contains() first.
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

	//! Result of predictive-mode gating for a fire render configuration.
	//! reasonCodes is sorted and deduplicated when predictiveAllowed is false.
	struct FireFidelityEvaluation
	{
		bool predictiveAllowed;
		std::string renderFidelityStatus;
		std::vector<std::string> reasonCodes;
	};

	//! Immutable fire-constituent optical preset decoded from canonical CBOR64
	//! bytes (RecordId() is the lowercase SHA-256 hex digest of RecordBytes()).
	//! Separates certified [380, 780] nm PCHIP presets (PredictiveOpticalPreset)
	//! from analytic constant fixtures (SyntheticRegressionFixture).
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
		double m_hotFixtureOmega;
		double m_hotFixtureG;
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

		//! Decodes and validates canonical CBOR64 bytes, verifying provenance,
		//! source bindings, and PCHIP derivative enclosures. Resets to invalid on error.
		bool LoadCanonicalRecord(
			const RISECBOR64::Bytes& bytes,
			std::string* error = 0
			);

		//! Built-in embedded presets verified against pinned SHA-256 digests.
		static const FireOpticsPreset& PredictiveV1();
		static const FireOpticsPreset& SyntheticRegressionV1();
		//! Resolves by canonical record name or alias ("fire_optics_v1", "synthetic_regression_v1").
		static const FireOpticsPreset* Named( const char* name );

		//! Synthesizes a valid SyntheticRegressionFixture record from caller-supplied
		//! analytic constants (sootDensityKgM3 in kg/m^3, k_m in m^2/g); never predictive.
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
		//! Returns true if [minimumNM, maximumNM] lies inside the certified wavelength domain
		//! (synthetic analytic fixtures accept any positive ordered range).
		bool SupportsWavelengthRange( double minimumNM, double maximumNM ) const;
		double HotFractionMinK() const { return m_hotFractionMinK; }
		double HotFractionMaxK() const { return m_hotFractionMaxK; }
		//! C1 cubic-Hermite smoothstep phi(T) over [HotFractionMinK(), HotFractionMaxK()] K
		//! partitioning carbon mass into hot soot (phi) and cool carbon (1 - phi).
		double HotFraction( double temperatureK ) const;
		//! Pinned soot material density in g/cm^3 and kg/m^3.
		double SootDensityGCM3() const { return m_densityGCM3; }
		double SootDensityKgM3() const { return m_densityGCM3*1000.0; }

		//! Spectral constituent optical properties at wavelengthNM (nm):
		//! mass absorption/extinction in m^2/g, dimensionless E_eff and albedo omega in [0,1),
		//! and Henyey-Greenstein asymmetry g in (-1,1).
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

		//! Conservative visible-band majorant upper bounds (m^2/g) over SpectralConfig's
		//! [nmBegin, nmEnd] band for null-collision tracking.
		double MaximumExtinctionMassVisible() const;
		double MaximumHotAbsorptionMassVisible() const;
		double MaximumCoolAbsorptionMassVisible() const;
		double MaximumCondensedAbsorptionMassVisible() const;

		//! Representative single-wavelength (550 nm / 633 nm) scalar proxies for RGB preview.
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

		//! Evaluates predictive-mode eligibility given render inputs and populates
		//! sorted unique reasonCodes blocking predictive status.
		FireFidelityEvaluation EvaluateFidelity(
			bool predictiveRequested,
			bool hasNonzeroCondensedInventory,
			bool hasChemChannels
			) const;
	};
}

#endif
