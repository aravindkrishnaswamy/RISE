//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjectionTest.cpp - production P2 manufactured gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireProductionProjection.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace
{
	int failures=0;

	void Check( bool condition, const char* message )
	{
		if( !condition ) {std::cerr << "FAIL: " << message << '\n';++failures;}
	}

	std::size_t Cell( const RISE::FireProductionProjectionShape& shape,
		std::size_t x,std::size_t y,std::size_t z )
	{
		return (z*shape.ny+y)*shape.nx+x;
	}

	std::size_t Face( const RISE::FireProductionProjectionShape& shape,
		unsigned int axis,std::size_t x,std::size_t y,std::size_t z )
	{
		if( axis==0u ) return (z*shape.ny+y)*(shape.nx+1u)+x;
		if( axis==1u ) return (z*(shape.ny+1u)+y)*shape.nx+x;
		return (z*shape.ny+y)*shape.nx+x;
	}

	RISE::FireProductionProjectionRequest EmptyRequest( std::size_t nx,
		std::size_t ny,std::size_t nz )
	{
		RISE::FireProductionProjectionRequest request;
		request.shape.nx=nx;request.shape.ny=ny;request.shape.nz=nz;
		request.shape.cellWidthM=0.1f;request.timeStepS=0.01f;
		request.ambientDensityKGPerM3=1.0f;
		request.gasDensityKGPerM3.assign(request.shape.CellCount(),1.0f);
		request.divergenceTargetPerS.assign(request.shape.CellCount(),0.0f);
		for( unsigned int axis=0;axis<3u;++axis ) request.provisionalMomentumKGPerM2S[axis].
			assign(RISE::FireProductionProjectionFaceCount(request.shape,axis),0.0f);
		return request;
	}

	void SetBoundary( RISE::FireProductionProjectionRequest& request,
		RISE::FireProductionProjectionBoundary kind )
	{
		request.boundary.fill(kind);
	}

	float IndependentFaceDensity( const RISE::FireProductionProjectionRequest& request,
		unsigned int axis,std::size_t x,std::size_t y,std::size_t z )
	{
		const RISE::FireProductionProjectionShape& shape=request.shape;
		std::size_t lowX=x,lowY=y,lowZ=z,highX=x,highY=y,highZ=z;
		const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
		const std::size_t extent=axis==0u?shape.nx:(axis==1u?shape.ny:shape.nz);
		if( coordinate>0u&&coordinate<extent ) {
			if( axis==0u ) --lowX;if( axis==1u ) --lowY;if( axis==2u ) --lowZ;
			return 0.5f*request.gasDensityKGPerM3[Cell(shape,lowX,lowY,lowZ)]+
				0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
		}
		const unsigned int side=2u*axis+(coordinate?1u:0u);
		if( request.boundary[side]==RISE::FireProductionProjectionPeriodic ) {
			if( axis==0u ) {lowX=shape.nx-1u;highX=0u;}
			if( axis==1u ) {lowY=shape.ny-1u;highY=0u;}
			if( axis==2u ) {lowZ=shape.nz-1u;highZ=0u;}
			return 0.5f*request.gasDensityKGPerM3[Cell(shape,lowX,lowY,lowZ)]+
				0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
		}
		if( axis==0u ) highX=coordinate?shape.nx-1u:0u;
		if( axis==1u ) highY=coordinate?shape.ny-1u:0u;
		if( axis==2u ) highZ=coordinate?shape.nz-1u:0u;
		return request.boundary[side]==RISE::FireProductionProjectionPressureOpen?
			0.5f*request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)]+
			0.5f*request.ambientDensityKGPerM3:
			request.gasDensityKGPerM3[Cell(shape,highX,highY,highZ)];
	}

	bool EveryZero( const RISE::FireProductionProjectionResult& result )
	{
		if( !std::all_of(result.pressurePa.begin(),result.pressurePa.end(),
			[](float value){return value==0.0f;}) ) return false;
		for( unsigned int axis=0;axis<3u;++axis )
			if( !std::all_of(result.velocityMPerS[axis].begin(),result.velocityMPerS[axis].end(),
				[](float value){return value==0.0f;}) ) return false;
		return true;
	}
}

int main()
{
	using namespace RISE;
	std::string error;
	for( const FireProductionProjectionBoundary boundary : {
		FireProductionProjectionPeriodic,FireProductionProjectionWall,
		FireProductionProjectionPressureOpen} ) {
		FireProductionProjectionRequest rest=EmptyRequest(9u,7u,5u);
		SetBoundary(rest,boundary);
		FireProductionProjectionResult result;
		Check(ProjectFireProductionCPU(rest,result,&error)&&EveryZero(result)&&
			result.validationPassed&&result.maximumPreProjectionResidualPerS==0.0f&&
			result.maximumPostProjectionResidualPerS==0.0f,
			"P2 periodic, wall, and pressure-open rest states are byte exact");
	}

	FireProductionProjectionRequest sinusoid=EmptyRequest(16u,8u,4u);
	SetBoundary(sinusoid,FireProductionProjectionPeriodic);
	const float pi=static_cast<float>(std::acos(-1.0));
	std::vector<float> sinusoidPressure(sinusoid.shape.CellCount(),0.0f);
	for( std::size_t z=0;z<sinusoid.shape.nz;++z )
		for( std::size_t y=0;y<sinusoid.shape.ny;++y )
			for( std::size_t x=0;x<sinusoid.shape.nx;++x )
				sinusoidPressure[Cell(sinusoid.shape,x,y,z)]=std::sin(2.0f*pi*
					(static_cast<float>(x)+0.5f)/static_cast<float>(sinusoid.shape.nx));
	for( std::size_t z=0;z<sinusoid.shape.nz;++z )
		for( std::size_t y=0;y<sinusoid.shape.ny;++y )
			for( std::size_t x=0;x<=sinusoid.shape.nx;++x ) {
				const std::size_t low=x==0u||x==sinusoid.shape.nx?sinusoid.shape.nx-1u:x-1u;
				const std::size_t high=x==0u||x==sinusoid.shape.nx?0u:x;
				sinusoid.provisionalMomentumKGPerM2S[0][Face(sinusoid.shape,0u,x,y,z)]=
					sinusoid.timeStepS*(sinusoidPressure[Cell(sinusoid.shape,high,y,z)]-
					sinusoidPressure[Cell(sinusoid.shape,low,y,z)])/sinusoid.shape.cellWidthM;
			}
	FireProductionProjectionResult sinusoidResult;
	Check(ProjectFireProductionCPU(sinusoid,sinusoidResult,&error)&&
		sinusoidResult.validationPassed&&sinusoidResult.maximumPostProjectionResidualPerS<=5.0e-5f,
		"P2 constant-density sinusoidal pressure is recovered within the fixed schedule");

	FireProductionProjectionRequest manufactured=EmptyRequest(17u,9u,7u);
	SetBoundary(manufactured,FireProductionProjectionPeriodic);
	std::vector<float> knownPressure(manufactured.shape.CellCount(),0.0f);
	for( std::size_t z=0;z<manufactured.shape.nz;++z )
		for( std::size_t y=0;y<manufactured.shape.ny;++y )
			for( std::size_t x=0;x<manufactured.shape.nx;++x ) {
				const float phaseX=2.0f*pi*(static_cast<float>(x)+0.5f)/
					static_cast<float>(manufactured.shape.nx);
				const float phaseY=2.0f*pi*(static_cast<float>(y)+0.5f)/
					static_cast<float>(manufactured.shape.ny);
				knownPressure[Cell(manufactured.shape,x,y,z)]=0.7f*std::sin(phaseX)+
					0.2f*std::cos(phaseY);
				manufactured.gasDensityKGPerM3[Cell(manufactured.shape,x,y,z)]=
					1.0f+0.2f*std::sin(phaseX)*std::cos(phaseY);
			}
	for( unsigned int axis=0;axis<3u;++axis ) {
		const std::size_t ex=axis==0u?manufactured.shape.nx+1u:manufactured.shape.nx;
		const std::size_t ey=axis==1u?manufactured.shape.ny+1u:manufactured.shape.ny;
		const std::size_t ez=axis==2u?manufactured.shape.nz+1u:manufactured.shape.nz;
		for( std::size_t z=0;z<ez;++z ) for( std::size_t y=0;y<ey;++y )
			for( std::size_t x=0;x<ex;++x ) {
				const std::size_t coordinate=axis==0u?x:(axis==1u?y:z);
				const std::size_t extent=axis==0u?manufactured.shape.nx:
					(axis==1u?manufactured.shape.ny:manufactured.shape.nz);
				std::size_t lx=x,ly=y,lz=z,hx=x,hy=y,hz=z;
				if( coordinate==0u||coordinate==extent ) {
					if( axis==0u ) {lx=manufactured.shape.nx-1u;hx=0u;}
					if( axis==1u ) {ly=manufactured.shape.ny-1u;hy=0u;}
					if( axis==2u ) {lz=manufactured.shape.nz-1u;hz=0u;}
				} else {
					if( axis==0u ) --lx;if( axis==1u ) --ly;if( axis==2u ) --lz;
				}
				const float gradient=(knownPressure[Cell(manufactured.shape,hx,hy,hz)]-
					knownPressure[Cell(manufactured.shape,lx,ly,lz)])/manufactured.shape.cellWidthM;
				float baseVelocity=axis==0u?0.12f:(axis==1u?-0.07f:0.03f);
				const std::size_t periodicCoordinate=coordinate==extent?0u:coordinate;
				if( axis==0u ) baseVelocity+=0.04f*std::sin(2.0f*pi*
					static_cast<float>(periodicCoordinate)/static_cast<float>(extent));
				if( axis==1u ) baseVelocity+=0.02f*std::cos(2.0f*pi*
					static_cast<float>(periodicCoordinate)/static_cast<float>(extent));
				const float density=IndependentFaceDensity(manufactured,axis,x,y,z);
				manufactured.provisionalMomentumKGPerM2S[axis][Face(
					manufactured.shape,axis,x,y,z)]=density*baseVelocity+
					manufactured.timeStepS*gradient;
			}
	}
	for( std::size_t z=0;z<manufactured.shape.nz;++z )
		for( std::size_t y=0;y<manufactured.shape.ny;++y )
			for( std::size_t x=0;x<manufactured.shape.nx;++x ) {
				const float ux0=0.12f+0.04f*std::sin(2.0f*pi*static_cast<float>(x)/
					static_cast<float>(manufactured.shape.nx));
				const float ux1=0.12f+0.04f*std::sin(2.0f*pi*static_cast<float>((x+1u)%manufactured.shape.nx)/
					static_cast<float>(manufactured.shape.nx));
				const float uy0=-0.07f+0.02f*std::cos(2.0f*pi*static_cast<float>(y)/
					static_cast<float>(manufactured.shape.ny));
				const float uy1=-0.07f+0.02f*std::cos(2.0f*pi*static_cast<float>((y+1u)%manufactured.shape.ny)/
					static_cast<float>(manufactured.shape.ny));
				manufactured.divergenceTargetPerS[Cell(manufactured.shape,x,y,z)]=
					(ux1-ux0+uy1-uy0)/manufactured.shape.cellWidthM;
			}
	FireProductionProjectionResult manufacturedResult,repeatedResult;
	const bool manufacturedOK=ProjectFireProductionCPU(manufactured,manufacturedResult,&error);
	const bool repeatedOK=ProjectFireProductionCPU(manufactured,repeatedResult,&error);
	float maximumPressureError=0.0f;
	if( manufacturedOK ) {
		float pressureMean=0.0f;
		for( const float value : manufacturedResult.pressurePa ) pressureMean+=value;
		pressureMean/=static_cast<float>(manufacturedResult.pressurePa.size());
		for( std::size_t cell=0;cell<knownPressure.size();++cell )
			maximumPressureError=std::max(maximumPressureError,std::fabs(
				(manufacturedResult.pressurePa[cell]-pressureMean)-knownPressure[cell]));
	}
	if( !manufacturedOK||!manufacturedResult.validationPassed )
		std::cerr << "Manufactured projection detail: " << error << " post=" <<
			manufacturedResult.maximumPostProjectionResidualPerS << " pressure_error=" <<
			maximumPressureError << '\n';
	Check(manufacturedOK&&repeatedOK&&manufacturedResult.validationPassed&&
		maximumPressureError<=0.025f&&
		manufacturedResult.pressurePa==repeatedResult.pressurePa&&
		manufacturedResult.velocityMPerS==repeatedResult.velocityMPerS,
		"P2 odd-grid variable-density manufactured projection is accurate and byte deterministic");

	FireProductionProjectionRequest expansion=EmptyRequest(11u,7u,5u);
	SetBoundary(expansion,FireProductionProjectionPressureOpen);
	const float expansionRate=0.4f;
	const float length=expansion.shape.cellWidthM*static_cast<float>(expansion.shape.nx);
	for( std::size_t z=0;z<expansion.shape.nz;++z ) for( std::size_t y=0;y<expansion.shape.ny;++y )
		for( std::size_t x=0;x<=expansion.shape.nx;++x )
			expansion.provisionalMomentumKGPerM2S[0][Face(expansion.shape,0u,x,y,z)]=
				expansionRate*(static_cast<float>(x)*expansion.shape.cellWidthM-0.5f*length);
	std::fill(expansion.divergenceTargetPerS.begin(),expansion.divergenceTargetPerS.end(),
		expansionRate);
	FireProductionProjectionResult expansionResult;
	const bool expansionOK=ProjectFireProductionCPU(expansion,expansionResult,&error);
	float expansionMaximumPressure=0.0f;
	for( const float value : expansionResult.pressurePa )
		expansionMaximumPressure=std::max(expansionMaximumPressure,std::fabs(value));
	if( !expansionOK||!expansionResult.validationPassed||
		expansionResult.maximumPostProjectionResidualPerS>1.0e-5f||
		expansionMaximumPressure>1.0e-6f )
		std::cerr << "Expansion projection detail: " << error << " pre=" <<
			expansionResult.maximumPreProjectionResidualPerS << " post=" <<
			expansionResult.maximumPostProjectionResidualPerS << " complementarity=" <<
			expansionResult.maximumOpenComplementarityDiscrepancyMPerS << " pressure=" <<
			expansionMaximumPressure << '\n';
	Check(expansionOK&&
		expansionResult.validationPassed&&expansionResult.maximumPostProjectionResidualPerS<=1.0e-5f&&
		expansionMaximumPressure<=1.0e-6f&&
		std::all_of(expansionResult.pressureOpenInflow.begin(),
			expansionResult.pressureOpenInflow.end(),[](const std::vector<unsigned char>& side){
				return std::all_of(side.begin(),side.end(),
					[](unsigned char value){return value==0u;});}),
		"P2 pressure-open expansion preserves the independent x-linear target field");

	FireProductionProjectionRequest roles=EmptyRequest(8u,6u,4u);
	SetBoundary(roles,FireProductionProjectionPressureOpen);
	for( std::size_t z=0;z<roles.shape.nz;++z ) for( std::size_t y=0;y<roles.shape.ny;++y ) {
		roles.provisionalMomentumKGPerM2S[0][Face(roles.shape,0u,0u,y,z)]=0.2f;
		roles.provisionalMomentumKGPerM2S[0][Face(roles.shape,0u,roles.shape.nx,y,z)]=0.2f;
	}
	FireProductionProjectionResult rolesResult;
	Check(ProjectFireProductionCPU(roles,rolesResult,&error)&&
		std::all_of(rolesResult.pressureOpenInflow[0].begin(),
			rolesResult.pressureOpenInflow[0].end(),[](unsigned char value){return value==1u;})&&
		std::all_of(rolesResult.pressureOpenInflow[1].begin(),
			rolesResult.pressureOpenInflow[1].end(),[](unsigned char value){return value==0u;}),
		"P2 frozen pressure-open total-head roles distinguish inflow and outflow signs");

	FireProductionProjectionRequest invalid=manufactured;
	invalid.gasDensityKGPerM3[0]=std::numeric_limits<float>::quiet_NaN();
	FireProductionProjectionResult invalidResult;invalidResult.pressurePa.push_back(9.0f);
	Check(!ProjectFireProductionCPU(invalid,invalidResult,&error)&&
		invalidResult.pressurePa.empty()&&!error.empty(),
		"P2 structural input failure returns no partial projection");

	if( failures==0 ) {
		std::cout << "FireProductionProjectionTest passed: post_residual=" <<
			manufacturedResult.maximumPostProjectionResidualPerS <<
			" pressure_error=" << maximumPressureError << '\n';
		return 0;
	}
	std::cerr << failures << " FireProductionProjectionTest failure(s)\n";
	return 1;
}
