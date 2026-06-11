//////////////////////////////////////////////////////////////////////
//
//  GuillochePainter.cpp - guilloché oxide-dose IFunction2D painter +
//  the descriptor converter.  See GuillochePainter.h.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GuillochePainter.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	namespace Implementation
	{
		GuillocheParams GuillocheParamsFromDescriptor( const GuillocheDialDescriptor& d )
		{
			GuillocheParams p;
			switch( d.pattern )
			{
			default:
			case eGuillochePatternUniform:   p.pattern = GuillocheParams::ePatUniform;   break;
			case eGuillochePatternLightning: p.pattern = GuillocheParams::ePatLightning; break;
			case eGuillochePatternRadial:    p.pattern = GuillocheParams::ePatRadial;    break;
			case eGuillochePatternIris:      p.pattern = GuillocheParams::ePatIris;      break;
			case eGuillochePatternSwirl:     p.pattern = GuillocheParams::ePatSwirl;     break;
			case eGuillochePatternVarwidth:  p.pattern = GuillocheParams::ePatVarwidth;  break;
			}
			p.radius      = d.radius;
			p.numArms     = d.numArms;
			p.swirl       = d.swirl;
			p.seamJag     = d.seamJag;
			p.seamJagFreq = d.seamJagFreq;
			p.cell        = d.cell;
			p.gridAmp     = d.gridAmp;
			p.petalAmp    = d.petalAmp;
			p.gridE0      = d.gridE0;
			p.gridE1      = d.gridE1;
			p.petalE0     = d.petalE0;
			p.petalE1     = d.petalE1;
			p.base        = d.base;
			p.landLevel   = d.landLevel;
			p.reliefDepth = d.reliefDepth;
			p.centerRadius = d.centerRadius;
			p.cellMode    = ( d.cellMode == 1 ) ? GuillocheParams::eCellSelect : GuillocheParams::eCellFreqBlend;
			p.lightningCellScale = d.lightningCellScale;
			p.lightningLo = d.lightningLo;
			p.lightningHi = d.lightningHi;
			p.lightningRelief = d.lightningRelief;
			p.zigzagAmp   = d.zigzagAmp;
			p.zigzagFreq  = d.zigzagFreq;
			p.fieldCell   = d.fieldCell;
			p.fieldFrame  = ( d.fieldFrame == 1 ) ? GuillocheParams::eFrameRadial : GuillocheParams::eFrameGlobal;
			switch( d.boltStyle )
			{
			default:
			case 0: p.boltStyle = GuillocheParams::eBoltRung;  break;
			case 1: p.boltStyle = GuillocheParams::eBoltCube;  break;
			case 2: p.boltStyle = GuillocheParams::eBoltWoven; break;
			}
			p.rungLen     = d.rungLen;
			p.rungWidth   = d.rungWidth;
			p.irisAperture = d.irisAperture;
			p.irisSwirl   = d.irisSwirl;
			p.irisEdge    = d.irisEdge;
			p.swirlTurns  = d.swirlTurns;
			return p;
		}
	}
}

GuillocheOxidePainter::GuillocheOxidePainter(
	const GuillocheParams& params,
	const int falloffMode,
	const Scalar activationEa,
	const Scalar torchAmount ) :
	m_field( params ),
	m_falloffMode( falloffMode ),
	m_activationEa( activationEa ),
	m_torchAmount( torchAmount ),
	m_g0( GuillocheField::ArrheniusG( GuillocheField::kTMinK, activationEa ) ),
	m_gSpan( GuillocheField::ArrheniusG( GuillocheField::kTMaxK, activationEa ) - m_g0 )
{
}

GuillocheOxidePainter::~GuillocheOxidePainter()
{
}

Scalar GuillocheOxidePainter::Evaluate( const Scalar u, const Scalar v ) const
{
	// (u, v) = the dial's linear Cartesian UV -> dial-space (x, y).
	// The Arrhenius endpoint constants are hoisted to the ctor (Ea-only):
	// 1 exp per query instead of 3 on the render-time film_thickness path.
	const GuillocheParams& p = m_field.Params();
	const Scalar x = ( Scalar(2) * u - Scalar(1) ) * p.radius;
	const Scalar y = ( Scalar(2) * v - Scalar(1) ) * p.radius;
	const Scalar heat = m_field.HeatAt( x, y, m_falloffMode );
	return m_field.OxideDoseWithEndpoints( x, y, heat, m_activationEa, m_g0, m_gSpan, m_torchAmount );
}
