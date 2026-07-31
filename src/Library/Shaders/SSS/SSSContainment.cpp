//////////////////////////////////////////////////////////////////////
//
//  SSSContainment.cpp - Phase-B preview containment for nonlocal SSS.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SSSContainment.h"
#include "../StandardShader.h"
#include "../AdvancedShader.h"
#include "../DirectVolumeRenderingShader.h"
#include "SubSurfaceScatteringShaderOp.h"
#include "DonnerJensenSkinSSSShaderOp.h"
#include "../AlphaTestShaderOp.h"
#include "../AmbientOcclusionShaderOp.h"
#include "../AreaLightShaderOp.h"
#include "../CausticPelPhotonMapShaderOp.h"
#include "../CausticSpectralPhotonMapShaderOp.h"
#include "../DirectLightingShaderOp.h"
#include "../DistributionTracingShaderOp.h"
#include "../EmissionShaderOp.h"
#include "../FinalGatherShaderOp.h"
#include "../GlobalPelPhotonMapShaderOp.h"
#include "../GlobalSpectralPhotonMapShaderOp.h"
#include "../PathTracingShaderOp.h"
#include "../ReflectionShaderOp.h"
#include "../RefractionShaderOp.h"
#include "../ShadowPhotonMapShaderOp.h"
#include "../SMSShaderOp.h"
#include "../TranslucentPelPhotonMapShaderOp.h"
#include "../TransparencyShaderOp.h"
#include "../../Utilities/Log/Log.h"
#include <atomic>
#include <set>
#include <typeinfo>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	thread_local unsigned int g_sssContainmentDepth = 0;
	std::atomic<unsigned long long> g_pivotAttempts( 0 );
	std::atomic<unsigned long long> g_endpointAttempts( 0 );
	std::atomic<unsigned long long> g_childLaunches( 0 );
	std::atomic<bool> g_childCompetitionObserved( false );
	std::atomic<bool> g_diagnosticEmitted( false );

	bool ShaderRequiresSSSContainmentImpl(
		const IShader& shader,
		std::set<const IShader*>& activeShaders )
	{
		if( !activeShaders.insert( &shader ).second ) {
			// A cyclic authored dependency is not demonstrably local.
			return true;
		}

		bool result = true;
		const std::type_info& type = typeid( shader );
		if( type == typeid( StandardShader ) ) {
			result = false;
			const std::vector<IShaderOp*>& ops =
				dynamic_cast<const StandardShader&>( shader ).ShaderOpsForSSSClassification();
			for( const IShaderOp* op : ops ) {
				if( !op || ShaderOpRequiresSSSContainment( *op ) ) {
					result = true;
					break;
				}
			}
		} else if( type == typeid( AdvancedShader ) ) {
			result = false;
			const AdvancedShader::ShadeOpListType& ops =
				dynamic_cast<const AdvancedShader&>( shader ).ShaderOpsForSSSClassification();
			for( const AdvancedShader::SHADE_OP& op : ops ) {
				if( !op.pShaderOp || ShaderOpRequiresSSSContainment( *op.pShaderOp ) ) {
					result = true;
					break;
				}
			}
		} else if( type == typeid( DirectVolumeRenderingShader ) ) {
			const IShader* nested = dynamic_cast<const DirectVolumeRenderingShader&>(
				shader ).ISOShaderForSSSClassification();
			result = nested ? ShaderRequiresSSSContainmentImpl(
				*nested, activeShaders ) : false;
		}

		activeShaders.erase( &shader );
		return result;
	}
}

bool RISE::Implementation::ShaderOpRequiresSSSContainment(
	const IShaderOp& shaderOp )
{
	const std::type_info& type = typeid( shaderOp );
	if( type == typeid( SubSurfaceScatteringShaderOp ) ||
		type == typeid( DonnerJensenSkinSSSShaderOp ) ) return true;

	return !( type == typeid( AlphaTestShaderOp ) ||
		type == typeid( AmbientOcclusionShaderOp ) ||
		type == typeid( AreaLightShaderOp ) ||
		type == typeid( CausticPelPhotonMapShaderOp ) ||
		type == typeid( CausticSpectralPhotonMapShaderOp ) ||
		type == typeid( DirectLightingShaderOp ) ||
		type == typeid( DistributionTracingShaderOp ) ||
		type == typeid( EmissionShaderOp ) ||
		type == typeid( FinalGatherShaderOp ) ||
		type == typeid( GlobalPelPhotonMapShaderOp ) ||
		type == typeid( GlobalSpectralPhotonMapShaderOp ) ||
		type == typeid( PathTracingShaderOp ) ||
		type == typeid( ReflectionShaderOp ) ||
		type == typeid( RefractionShaderOp ) ||
		type == typeid( ShadowPhotonMapShaderOp ) ||
		type == typeid( SMSShaderOp ) ||
		type == typeid( TranslucentPelPhotonMapShaderOp ) ||
		type == typeid( TransparencyShaderOp ) );
}

bool RISE::Implementation::ShaderRequiresSSSContainment(
	const IShader& shader )
{
	std::set<const IShader*> activeShaders;
	return ShaderRequiresSSSContainmentImpl( shader, activeShaders );
}

SSSContainmentScope::SSSContainmentScope() :
	volumeStateScope_( VolumeEmissionSegmentState() )
{
	++g_sssContainmentDepth;
	bool expected = false;
	if( g_diagnosticEmitted.compare_exchange_strong(
		expected, true, std::memory_order_relaxed ) ) {
		GlobalLog()->PrintEasyInfo(
			"SSS preview containment: thermal-volume NEE disabled; "
			"nested continuation uses march-only emission" );
	}
}

SSSContainmentScope::~SSSContainmentScope()
{
	--g_sssContainmentDepth;
}

bool RISE::Implementation::IsSSSContainmentActive()
{
	return g_sssContainmentDepth != 0;
}

void RISE::Implementation::RecordSSSContainedVolumePivotAttempt()
{
	if( IsSSSContainmentActive() ) {
		g_pivotAttempts.fetch_add( 1, std::memory_order_relaxed );
	}
}

void RISE::Implementation::RecordSSSContainedVolumeEndpointAttempt()
{
	if( IsSSSContainmentActive() ) {
		g_endpointAttempts.fetch_add( 1, std::memory_order_relaxed );
	}
}

void RISE::Implementation::RecordSSSContainedChildLaunch()
{
	if( !IsSSSContainmentActive() ) return;
	g_childLaunches.fetch_add( 1, std::memory_order_relaxed );
	if( CurrentVolumeEmissionSegmentState().competitionAvailable ) {
		g_childCompetitionObserved.store( true, std::memory_order_relaxed );
	}
}

unsigned long long RISE::Implementation::SSSContainedVolumePivotAttemptCount()
{
	return g_pivotAttempts.load( std::memory_order_relaxed );
}

unsigned long long RISE::Implementation::SSSContainedVolumeEndpointAttemptCount()
{
	return g_endpointAttempts.load( std::memory_order_relaxed );
}

unsigned long long RISE::Implementation::SSSContainedChildLaunchCount()
{
	return g_childLaunches.load( std::memory_order_relaxed );
}

bool RISE::Implementation::SSSContainedChildCompetitionObserved()
{
	return g_childCompetitionObserved.load( std::memory_order_relaxed );
}

bool RISE::Implementation::SSSContainmentDiagnosticEmitted()
{
	return g_diagnosticEmitted.load( std::memory_order_relaxed );
}
