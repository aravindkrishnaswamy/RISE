//////////////////////////////////////////////////////////////////////
//
//  SSSContainment.h - Phase-B preview containment for nonlocal SSS.
//
//////////////////////////////////////////////////////////////////////

#ifndef SSS_CONTAINMENT_H
#define SSS_CONTAINMENT_H

#include "../../Utilities/MISWeights.h"

namespace RISE
{
	class IShader;
	class IShaderOp;

	namespace Implementation
	{
		//! Fail-closed runtime classification used by the Phase-B preview.
		//! Unknown concrete shader and shader-op types require containment.
		bool ShaderRequiresSSSContainment( const IShader& shader );
		bool ShaderOpRequiresSSSContainment( const IShaderOp& shaderOp );

		//! Request-local containment inherited by nested Shade/CastRay calls.
		//! The fresh volume-emission state makes the first child segment
		//! march-only (competitionAvailable=false); transport call sites also
		//! consult IsSSSContainmentActive() before drawing thermal pivots or
		//! endpoints.
		class SSSContainmentScope
		{
		public:
			SSSContainmentScope();
			~SSSContainmentScope();

			SSSContainmentScope( const SSSContainmentScope& ) = delete;
			SSSContainmentScope& operator=( const SSSContainmentScope& ) = delete;

		private:
			const VolumeEmissionSegmentStateScope volumeStateScope_;
		};

		bool IsSSSContainmentActive();

		//! Structural witnesses used by the Phase-B fixtures. Transport never
		//! reads these counters; they only make a forbidden proposal observable.
		void RecordSSSContainedVolumePivotAttempt();
		void RecordSSSContainedVolumeEndpointAttempt();
		void RecordSSSContainedChildLaunch();
		unsigned long long SSSContainedVolumePivotAttemptCount();
		unsigned long long SSSContainedVolumeEndpointAttemptCount();
		unsigned long long SSSContainedChildLaunchCount();
		bool SSSContainedChildCompetitionObserved();
		bool SSSContainmentDiagnosticEmitted();
	}
}

#endif
