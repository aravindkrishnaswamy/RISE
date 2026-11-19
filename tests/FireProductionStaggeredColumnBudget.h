#ifndef FIRE_PRODUCTION_STAGGERED_COLUMN_BUDGET_H
#define FIRE_PRODUCTION_STAGGERED_COLUMN_BUDGET_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <vector>

namespace FireProductionStaggeredColumn
{
	using FaceFields=std::array<std::vector<float>,3>;
	using FaceRates64=std::array<std::vector<double>,3>;
	struct RateView
	{
		const FaceFields* binary32;
		const FaceRates64* binary64;
		RateView(const FaceFields& field) : binary32(&field),binary64(nullptr) {}
		RateView(const FaceRates64& field) : binary32(nullptr),binary64(&field) {}
		std::size_t Size(const unsigned int axis) const
		{return binary32?(*binary32)[axis].size():(*binary64)[axis].size();}
		double Value(const unsigned int axis,const std::size_t face) const
		{return binary32?static_cast<double>((*binary32)[axis][face]):(*binary64)[axis][face];}
	};
	struct Inputs
	{
		const FaceFields& beginning;
		const FaceFields& provisional;
		const FaceFields& terminal;
		RateView stress,buoyancy,advection,source;
		const std::vector<float>& eddyViscosity;
	};
	struct Row
	{
		unsigned int axis;
		std::size_t face,x,y,z,lowerCell,upperCell;
		double beginning,provisional,terminal,stress,buoyancy,advection,source;
		double combinedProjection,total,closure,lowerVreman,upperVreman;
	};
	inline bool DimensionsValid(const std::array<std::size_t,3>& dimensions)
	{
		for(const std::size_t extent:dimensions)
			if(extent==0u||extent==std::numeric_limits<std::size_t>::max())return false;
		for(unsigned int axis=0u;axis<3u;++axis){std::size_t count=1u;
			for(unsigned int dimension=0u;dimension<3u;++dimension){
				const std::size_t extent=dimensions[dimension]+(axis==dimension?1u:0u);
				if(extent>std::numeric_limits<std::size_t>::max()/count)return false;
				count*=extent;
			}}
		return true;
	}

	inline bool DecodeFace(const std::array<std::size_t,3>& dimensions,const unsigned int axis,
		const std::size_t face,std::size_t& x,std::size_t& y,std::size_t& z)
	{
		if(axis>=3u||!DimensionsValid(dimensions))return false;
		const std::size_t nx=dimensions[0]+(axis==0u?1u:0u),
			ny=dimensions[1]+(axis==1u?1u:0u),nz=dimensions[2]+(axis==2u?1u:0u);
		if(face>=nx*ny*nz)return false;
		x=face%nx;y=(face/nx)%ny;z=face/(nx*ny);return true;
	}

	// x/y are the face coordinates of the extreme. At a high open face the
	// adjacent column is the last interior cell, not an out-of-domain column.
	inline bool AdjacentColumn(const std::array<std::size_t,3>& dimensions,
		const unsigned int axis,const std::size_t x,const std::size_t y,
		const std::size_t z,std::size_t& columnX,std::size_t& columnY)
	{
		if(axis>=3u||!DimensionsValid(dimensions)||
			x>=dimensions[0]+(axis==0u?1u:0u)||
			y>=dimensions[1]+(axis==1u?1u:0u)||
			z>=dimensions[2]+(axis==2u?1u:0u))return false;
		columnX=std::min(x,dimensions[0]-1u);
		columnY=std::min(y,dimensions[1]-1u);return true;
	}

	inline bool Build(const std::array<std::size_t,3>& dimensions,
		const std::size_t columnX,const std::size_t columnY,const double dt,
		const Inputs& input,std::vector<Row>& result)
	{
		result.clear();
		const std::size_t nx=dimensions[0],ny=dimensions[1],nz=dimensions[2];
		if(!DimensionsValid(dimensions)||columnX>=nx||columnY>=ny||
			!std::isfinite(dt)||!(dt>0.0)||input.eddyViscosity.size()!=nx*ny*nz)return false;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t faces=(nx+(axis==0u?1u:0u))*(ny+(axis==1u?1u:0u))*
				(nz+(axis==2u?1u:0u));
			for(const FaceFields* field:{&input.beginning,&input.provisional,&input.terminal})
				if((*field)[axis].size()!=faces)return false;
			for(const RateView* field:{&input.stress,&input.buoyancy,&input.advection,&input.source})
				if(field->Size(axis)!=faces)return false;
		}
		auto append=[&](const unsigned int axis,const std::size_t x,
			const std::size_t y,const std::size_t z){
			Row row;row.axis=axis;row.x=x;row.y=y;row.z=z;
			row.face=x+(nx+(axis==0u?1u:0u))*(y+(ny+(axis==1u?1u:0u))*z);
			std::array<std::size_t,3> lower={{x,y,z}},upper=lower;
			lower[axis]=lower[axis]==0u?0u:lower[axis]-1u;
			upper[axis]=std::min(upper[axis],dimensions[axis]-1u);
			row.lowerCell=lower[0]+nx*(lower[1]+ny*lower[2]);
			row.upperCell=upper[0]+nx*(upper[1]+ny*upper[2]);
			row.beginning=input.beginning[axis][row.face];
			row.provisional=input.provisional[axis][row.face];
			row.terminal=input.terminal[axis][row.face];
			row.stress=input.stress.Value(axis,row.face);row.buoyancy=input.buoyancy.Value(axis,row.face);
			row.advection=input.advection.Value(axis,row.face);row.source=input.source.Value(axis,row.face);
			row.combinedProjection=(row.terminal-row.provisional)/dt;
			row.total=(row.terminal-row.beginning)/dt;
			row.closure=row.total-(row.stress+row.buoyancy+row.advection+row.source+
				row.combinedProjection);
			row.lowerVreman=input.eddyViscosity[row.lowerCell];
			row.upperVreman=input.eddyViscosity[row.upperCell];
			for(const double value:{row.beginning,row.provisional,row.terminal,row.stress,
				row.buoyancy,row.advection,row.source,row.combinedProjection,row.total,
				row.closure,row.lowerVreman,row.upperVreman})if(!std::isfinite(value))return false;
			result.push_back(row);return true;
		};
		for(std::size_t z=0u;z<nz;++z)
			if(!append(0u,columnX,columnY,z)||!append(0u,columnX+1u,columnY,z)||
				!append(1u,columnX,columnY,z)||!append(1u,columnX,columnY+1u,z)){
				result.clear();return false;}
		for(std::size_t z=0u;z<=nz;++z)if(!append(2u,columnX,columnY,z)){
			result.clear();return false;}
		return true;
	}

	inline bool Write(std::ostream& output,const bool header,const double beginningTime,
		const unsigned int candidate,const double dt,const std::vector<Row>& rows)
	{
		if(header)output<<"beginning_time_s,candidate,dt_s,axis,face,x_face,y_face,z_face,"
			"lower_cell,upper_cell,beginning_momentum_kg_per_m2_s,provisional_momentum_kg_per_m2_s,"
			"terminal_momentum_kg_per_m2_s,stress_rate_kg_per_m2_s2,buoyancy_rate_kg_per_m2_s2,"
			"advection_rate_kg_per_m2_s2,source_rate_kg_per_m2_s2,combined_projection_rate_kg_per_m2_s2,"
			"total_rate_kg_per_m2_s2,closure_residual_kg_per_m2_s2,lower_vreman_m2_per_s,upper_vreman_m2_per_s\n";
		for(const Row& row:rows)output<<std::setprecision(17)<<beginningTime<<','<<candidate<<','<<dt<<','
			<<row.axis<<','<<row.face<<','<<row.x<<','<<row.y<<','<<row.z<<','<<row.lowerCell<<','
			<<row.upperCell<<','<<row.beginning<<','<<row.provisional<<','<<row.terminal<<','<<row.stress<<','
			<<row.buoyancy<<','<<row.advection<<','<<row.source<<','<<row.combinedProjection<<','<<
			row.total<<','<<row.closure<<','<<row.lowerVreman<<','<<row.upperVreman<<'\n';
		return static_cast<bool>(output);
	}

	inline bool Fixture()
	{
		const std::array<std::size_t,3> dimensions={{3u,4u,2u}};
		FaceFields beginning,provisional,terminal,stress,buoyancy,advection,source;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t faces=(3u+(axis==0u?1u:0u))*(4u+(axis==1u?1u:0u))*
				(2u+(axis==2u?1u:0u));
			for(std::size_t face=0u;face<faces;++face){
				const float sentinel=static_cast<float>(100u*axis+face);
				beginning[axis].push_back(sentinel);provisional[axis].push_back(sentinel+10.0f);
				terminal[axis].push_back(sentinel+14.0f);stress[axis].push_back(sentinel+20.0f);
				buoyancy[axis].push_back(sentinel+30.0f);advection[axis].push_back(sentinel+40.0f);
				source[axis].push_back(sentinel+50.0f);
			}
		}
		std::vector<float> nu;for(std::size_t cell=0u;cell<24u;++cell)
			nu.push_back(static_cast<float>(cell)+0.25f);
		const Inputs input={beginning,provisional,terminal,stress,buoyancy,advection,source,nu};
		// All orientations, both open boundaries, and horizontal interior extrema.
		for(const std::array<std::size_t,4>& extreme:std::vector<std::array<std::size_t,4>>{
			{{0u,0u,2u,1u}},{{0u,1u,2u,1u}},{{0u,3u,2u,1u}},
			{{1u,1u,0u,1u}},{{1u,1u,2u,1u}},{{1u,1u,4u,1u}},
			{{2u,1u,2u,0u}},{{2u,1u,2u,2u}}}){
			const unsigned int axis=static_cast<unsigned int>(extreme[0]);
			const std::size_t encodedFace=extreme[1]+(3u+(axis==0u?1u:0u))*
				(extreme[2]+(4u+(axis==1u?1u:0u))*extreme[3]);
			std::size_t decodedX=0u,decodedY=0u,decodedZ=0u;
			if(!DecodeFace(dimensions,axis,encodedFace,decodedX,decodedY,decodedZ)||
				decodedX!=extreme[1]||decodedY!=extreme[2]||decodedZ!=extreme[3])return false;
			std::size_t columnX=0u,columnY=0u;
			if(!AdjacentColumn(dimensions,axis,decodedX,decodedY,decodedZ,columnX,columnY))return false;
			std::vector<Row> rows;if(!Build(dimensions,columnX,columnY,0.5,input,rows)||rows.size()!=11u)return false;
			bool found=false;std::array<std::size_t,3> counts={{0u,0u,0u}};
			for(const Row& row:rows){
				++counts[row.axis];
				const std::size_t expectedFace=row.axis==0u?row.x+4u*(row.y+4u*row.z):
					(row.axis==1u?row.x+3u*(row.y+5u*row.z):row.x+3u*(row.y+4u*row.z));
				const double sentinel=static_cast<double>(100u*row.axis+expectedFace);
				std::size_t lx=row.x,ly=row.y,lz=row.z,ux=lx,uy=ly,uz=lz;
				if(row.axis==0u){lx=row.x==0u?0u:row.x-1u;ux=std::min(row.x,std::size_t{2u});}
				if(row.axis==1u){ly=row.y==0u?0u:row.y-1u;uy=std::min(row.y,std::size_t{3u});}
				if(row.axis==2u){lz=row.z==0u?0u:row.z-1u;uz=std::min(row.z,std::size_t{1u});}
				const std::size_t lower=lx+3u*(ly+4u*lz),upper=ux+3u*(uy+4u*uz);
				if(row.face!=expectedFace||row.beginning!=sentinel||row.provisional!=sentinel+10.0||
					row.terminal!=sentinel+14.0||row.stress!=sentinel+20.0||row.buoyancy!=sentinel+30.0||
					row.advection!=sentinel+40.0||row.source!=sentinel+50.0||row.combinedProjection!=8.0||
					row.total!=28.0||row.closure!=28.0-(4.0*sentinel+140.0+8.0)||
					row.lowerCell!=lower||row.upperCell!=upper||row.lowerVreman!=static_cast<double>(lower)+0.25||
					row.upperVreman!=static_cast<double>(upper)+0.25)return false;
				if(row.axis==axis&&row.x==extreme[1]&&row.y==extreme[2]&&row.z==extreme[3])found=true;
			}
			if(!found||counts!=std::array<std::size_t,3>{{4u,4u,3u}})return false;
		}
		std::vector<Row> rows;
		if(Build(dimensions,3u,0u,0.5,input,rows)||Build(dimensions,0u,0u,0.0,input,rows))return false;
		FaceRates64 stress64;
		for(unsigned int axis=0u;axis<3u;++axis)
			for(const float value:stress[axis])stress64[axis].push_back(
				std::nextafter(static_cast<double>(value),std::numeric_limits<double>::infinity()));
		const Inputs legacyInput={beginning,provisional,terminal,stress64,buoyancy,advection,source,nu};
		if(!Build(dimensions,0u,0u,0.5,legacyInput,rows)||
			rows.front().stress!=stress64[0][0]||rows.front().stress==stress[0][0])return false;
		std::ostringstream output;
		if(!Write(output,true,1.0,2u,0.5,rows)||output.str().find(
			"combined_projection_rate_kg_per_m2_s2")==std::string::npos)return false;
		std::size_t x=0u,y=0u;
		if(AdjacentColumn(dimensions,0u,4u,0u,0u,x,y)||
			DimensionsValid({{std::numeric_limits<std::size_t>::max(),4u,2u}}))return false;
		// A z-only/axis-swap mutant would miss the named horizontal sentinel or
		// read a different value. A truncated authority must refuse, not omit a row.
		advection[0].pop_back();
		return !Build(dimensions,0u,0u,0.5,input,rows);
	}
}
#endif
