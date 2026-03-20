//////////////////////////////////////////////////////////////////////
//
//  BSPTreeSAHNode.h - Definition of the SAH BSP tree node.
//
//  Author: OpenAI Codex
//  Date: March 19, 2026
//  Tabs: 4
//  Comments: NOTE: NEVER include this file directly. It is meant to
//  be included from BSPTreeSAH.h as part of the templated BSPTreeSAH
//  class.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

class BSPTreeNodeSAH
{
protected:
	BSPTreeNodeSAH**		pChildren;
	ElementListType*		pElements;
	unsigned char			splitAxis;
	Scalar					splitLocation;

public:
	BSPTreeNodeSAH() :
		pChildren( 0 ),
		pElements( 0 ),
		splitAxis( BSP_SAH_AXIS_INVALID ),
		splitLocation( 0 )
	{}

	virtual ~BSPTreeNodeSAH()
	{
		Clear();
	}

	void Clear()
	{
		if( pChildren ) {
			for( int i=0; i<2; i++ ) {
				if( pChildren[i] ) {
					GlobalLog()->PrintDelete( pChildren[i], __FILE__, __LINE__ );
					delete pChildren[i];
					pChildren[i] = 0;
				}
			}

			GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
			delete [] pChildren;
			pChildren = 0;
		}

		if( pElements ) {
			GlobalLog()->PrintDelete( pElements, __FILE__, __LINE__ );
			delete pElements;
			pElements = 0;
		}

		splitAxis = BSP_SAH_AXIS_INVALID;
		splitLocation = 0;
	}

	bool GetSplit( unsigned char& axis, Scalar& location ) const
	{
		if( !pChildren || splitAxis == BSP_SAH_AXIS_INVALID ) {
			return false;
		}

		axis = splitAxis;
		location = splitLocation;
		return true;
	}

	Scalar SurfaceArea( const BoundingBox& bb ) const
	{
		const Vector3 d = bb.GetExtents();
		const Scalar dx = fabs( d.x );
		const Scalar dy = fabs( d.y );
		const Scalar dz = fabs( d.z );
		return 2.0 * (dx*dy + dx*dz + dy*dz);
	}

	void SplitBoundingBox( const BoundingBox& bbox, const unsigned char axis, const Scalar location, BoundingBox& left, BoundingBox& right ) const
	{
		static const Scalar box_error = NEARZERO;

		left = bbox;
		right = bbox;

		left.ur[axis] = location + box_error;
		right.ll[axis] = location - box_error;
	}

	void BuildLeaf( const ElementInfoListType& elements )
	{
		pElements = new ElementListType( elements.size() );
		GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );

		unsigned int idx = 0;
		typename ElementInfoListType::const_iterator i, e;
		for( i=elements.begin(), e=elements.end(); i!=e; i++, idx++ ) {
			(*pElements)[idx] = i->elem;
		}
	}

	bool ShouldTerminate(
		const ElementInfoListType& elements,
		const BoundingBox& bbox,
		const unsigned int maxElements,
		const unsigned char max_recursion_level,
		const unsigned int tree_level
		) const
	{
		const Vector3 vBoxSize = bbox.GetExtents();
		return (
			vBoxSize.x <= bsp_sah_error_delta_box_size ||
			vBoxSize.y <= bsp_sah_error_delta_box_size ||
			vBoxSize.z <= bsp_sah_error_delta_box_size ||
			tree_level > max_recursion_level ||
			elements.size() <= maxElements
			);
	}

	bool FindBestExactSplit(
		const ElementInfoListType& my_elements,
		const BoundingBox& my_bb,
		const Scalar totalSA,
		Scalar& bestCost,
		unsigned char& bestAxis,
		Scalar& bestLocation
		) const
	{
		bool bFound = false;

		for( unsigned char axis=BSP_SAH_AXIS_X; axis<=BSP_SAH_AXIS_Z; axis++ ) {
			const Scalar nodeMin = my_bb.ll[axis];
			const Scalar nodeMax = my_bb.ur[axis];

			if( (nodeMax-nodeMin) <= bsp_sah_error_delta_box_size ) {
				continue;
			}

			std::vector<BoundEdge> edges;
			edges.reserve( my_elements.size() * 2 );

			for( unsigned int i=0; i<my_elements.size(); i++ ) {
				edges.push_back( BoundEdge( my_elements[i].bbox.ll[axis], i, true ) );
				edges.push_back( BoundEdge( my_elements[i].bbox.ur[axis], i, false ) );
			}

			std::sort( edges.begin(), edges.end(), BoundEdgeLess() );

			unsigned int nBelow = 0;
			unsigned int nAbove = static_cast<unsigned int>( my_elements.size() );
			const unsigned char otherAxis0 = (axis + 1) % 3;
			const unsigned char otherAxis1 = (axis + 2) % 3;
			const Vector3 d = my_bb.GetExtents();

			for( unsigned int i=0; i<edges.size(); i++ ) {
				if( edges[i].type == BoundEdge::EDGE_END ) {
					nAbove--;
				}

				const Scalar edgeT = edges[i].t;
				if( edgeT > nodeMin + NEARZERO && edgeT < nodeMax - NEARZERO ) {
					const Scalar belowSA = 2.0 * (d[otherAxis0]*d[otherAxis1] + (edgeT-nodeMin) * (d[otherAxis0]+d[otherAxis1]));
					const Scalar aboveSA = 2.0 * (d[otherAxis0]*d[otherAxis1] + (nodeMax-edgeT) * (d[otherAxis0]+d[otherAxis1]));
					const Scalar pBelow = belowSA / totalSA;
					const Scalar pAbove = aboveSA / totalSA;
					const Scalar emptyBonus = (nAbove == 0 || nBelow == 0) ? bsp_sah_empty_bonus : 0.0;
					const Scalar cost = bsp_sah_traversal_cost +
						bsp_sah_intersection_cost * (1.0-emptyBonus) * (pBelow*Scalar(nBelow) + pAbove*Scalar(nAbove));

					if( cost < bestCost ) {
						bestCost = cost;
						bestAxis = axis;
						bestLocation = edgeT;
						bFound = true;
					}
				}

				if( edges[i].type == BoundEdge::EDGE_START ) {
					nBelow++;
				}
			}
		}

		return bFound;
	}

	bool FindBestBinnedSplit(
		const ElementInfoListType& my_elements,
		const BoundingBox& my_bb,
		const Scalar totalSA,
		Scalar& bestCost,
		unsigned char& bestAxis,
		Scalar& bestLocation
		) const
	{
		bool bFound = false;
		const Vector3 d = my_bb.GetExtents();

		for( unsigned char axis=BSP_SAH_AXIS_X; axis<=BSP_SAH_AXIS_Z; axis++ ) {
			const Scalar nodeMin = my_bb.ll[axis];
			const Scalar nodeMax = my_bb.ur[axis];
			const Scalar nodeExtent = nodeMax - nodeMin;

			if( nodeExtent <= bsp_sah_error_delta_box_size ) {
				continue;
			}

			std::vector<unsigned int> startCounts( bsp_sah_num_bins, 0 );
			std::vector<unsigned int> endCounts( bsp_sah_num_bins, 0 );
			std::vector<unsigned int> prefixStarts( bsp_sah_num_bins, 0 );
			std::vector<unsigned int> suffixEnds( bsp_sah_num_bins, 0 );

			const Scalar invExtent = Scalar( bsp_sah_num_bins ) / nodeExtent;

			for( unsigned int i=0; i<my_elements.size(); i++ ) {
				Scalar minEdge = my_elements[i].bbox.ll[axis];
				Scalar maxEdge = my_elements[i].bbox.ur[axis];

				if( minEdge < nodeMin ) {
					minEdge = nodeMin;
				} else if( minEdge > nodeMax ) {
					minEdge = nodeMax;
				}

				if( maxEdge < nodeMin ) {
					maxEdge = nodeMin;
				} else if( maxEdge > nodeMax ) {
					maxEdge = nodeMax;
				}

				int startBin = static_cast<int>( (minEdge-nodeMin) * invExtent );
				int endBin = static_cast<int>( (maxEdge-nodeMin) * invExtent );

				if( startBin < 0 ) {
					startBin = 0;
				} else if( startBin >= static_cast<int>(bsp_sah_num_bins) ) {
					startBin = bsp_sah_num_bins - 1;
				}

				if( endBin < 0 ) {
					endBin = 0;
				} else if( endBin >= static_cast<int>(bsp_sah_num_bins) ) {
					endBin = bsp_sah_num_bins - 1;
				}

				startCounts[startBin]++;
				endCounts[endBin]++;
			}

			unsigned int runningStarts = 0;
			for( unsigned int i=0; i<bsp_sah_num_bins; i++ ) {
				runningStarts += startCounts[i];
				prefixStarts[i] = runningStarts;
			}

			unsigned int runningEnds = 0;
			for( int i=static_cast<int>(bsp_sah_num_bins)-1; i>=0; i-- ) {
				runningEnds += endCounts[static_cast<unsigned int>(i)];
				suffixEnds[static_cast<unsigned int>(i)] = runningEnds;
			}

			const unsigned char otherAxis0 = (axis + 1) % 3;
			const unsigned char otherAxis1 = (axis + 2) % 3;

			for( unsigned int i=0; i<bsp_sah_num_bins-1; i++ ) {
				const Scalar edgeT = nodeMin + nodeExtent * Scalar(i+1) / Scalar(bsp_sah_num_bins);
				const unsigned int nBelow = prefixStarts[i];
				const unsigned int nAbove = suffixEnds[i+1];
				const Scalar belowSA = 2.0 * (d[otherAxis0]*d[otherAxis1] + (edgeT-nodeMin) * (d[otherAxis0]+d[otherAxis1]));
				const Scalar aboveSA = 2.0 * (d[otherAxis0]*d[otherAxis1] + (nodeMax-edgeT) * (d[otherAxis0]+d[otherAxis1]));
				const Scalar pBelow = belowSA / totalSA;
				const Scalar pAbove = aboveSA / totalSA;
				const Scalar emptyBonus = (nAbove == 0 || nBelow == 0) ? bsp_sah_empty_bonus : 0.0;
				const Scalar cost = bsp_sah_traversal_cost +
					bsp_sah_intersection_cost * (1.0-emptyBonus) * (pBelow*Scalar(nBelow) + pAbove*Scalar(nAbove));

				if( cost < bestCost ) {
					bestCost = cost;
					bestAxis = axis;
					bestLocation = edgeT;
					bFound = true;
				}
			}
		}

		return bFound;
	}

	void AddElements(
		const TreeElementProcessor<Element>& ep,
		const ElementInfoListType& my_elements,
		const unsigned int maxElements,
		const BoundingBox& my_bb,
		const unsigned char max_recursion_level,
		const unsigned int tree_level
		)
	{
		Clear();

		if( my_elements.empty() ) {
			BuildLeaf( my_elements );
			return;
		}

		if( ShouldTerminate( my_elements, my_bb, maxElements, max_recursion_level, tree_level ) ) {
			BuildLeaf( my_elements );
			return;
		}

		const Scalar totalSA = SurfaceArea( my_bb );
		if( totalSA <= NEARZERO ) {
			BuildLeaf( my_elements );
			return;
		}

		Scalar bestCost = bsp_sah_intersection_cost * Scalar( my_elements.size() );
		unsigned char bestAxis = BSP_SAH_AXIS_INVALID;
		Scalar bestLocation = 0;
		const bool bFoundSplit = my_elements.size() <= bsp_sah_exact_threshold ?
			FindBestExactSplit( my_elements, my_bb, totalSA, bestCost, bestAxis, bestLocation ) :
			FindBestBinnedSplit( my_elements, my_bb, totalSA, bestCost, bestAxis, bestLocation );

		if( !bFoundSplit || bestAxis == BSP_SAH_AXIS_INVALID ) {
			BuildLeaf( my_elements );
			return;
		}

		ElementInfoListType leftElements;
		ElementInfoListType rightElements;
		leftElements.reserve( my_elements.size() );
		rightElements.reserve( my_elements.size() );

		typename ElementInfoListType::const_iterator i, e;
		for( i=my_elements.begin(), e=my_elements.end(); i!=e; i++ ) {
			const Scalar minEdge = i->bbox.ll[bestAxis];
			const Scalar maxEdge = i->bbox.ur[bestAxis];
			const bool bGoesLeft = (minEdge < bestLocation) || fabs( maxEdge-bestLocation ) <= NEARZERO;
			const bool bGoesRight = (maxEdge > bestLocation) || fabs( minEdge-bestLocation ) <= NEARZERO;

			if( bGoesLeft ) {
				leftElements.push_back( *i );
			}

			if( bGoesRight ) {
				rightElements.push_back( *i );
			}
		}

		if( leftElements.size() == my_elements.size() && rightElements.size() == my_elements.size() ) {
			BuildLeaf( my_elements );
			return;
		}

		BoundingBox leftBB, rightBB;
		SplitBoundingBox( my_bb, bestAxis, bestLocation, leftBB, rightBB );

		splitAxis = bestAxis;
		splitLocation = bestLocation;

		pChildren = new BSPTreeNodeSAH*[2];
		GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "bsptreesah children" );
		pChildren[0] = 0;
		pChildren[1] = 0;

		if( leftElements.size() ) {
			pChildren[0] = new BSPTreeNodeSAH();
			GlobalLog()->PrintNew( pChildren[0], __FILE__, __LINE__, "Left ChildNode" );
			pChildren[0]->AddElements( ep, leftElements, maxElements, leftBB, max_recursion_level, tree_level+1 );
		}

		if( rightElements.size() ) {
			pChildren[1] = new BSPTreeNodeSAH();
			GlobalLog()->PrintNew( pChildren[1], __FILE__, __LINE__, "Right ChildNode" );
			pChildren[1]->AddElements( ep, rightElements, maxElements, rightBB, max_recursion_level, tree_level+1 );
		}

		if( !pChildren[0] && !pChildren[1] ) {
			GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
			delete [] pChildren;
			pChildren = 0;
			splitAxis = BSP_SAH_AXIS_INVALID;
			splitLocation = 0;
			BuildLeaf( my_elements );
		}
	}

	//
	// kd-tree style traversal using tmin/tmax intervals.
	// Instead of computing child bounding boxes and performing full
	// ray-box intersection at every node, we compute tSplit (distance
	// to the split plane) and use it to determine near/far child
	// traversal order. This eliminates billions of RayBoxIntersection
	// calls inside the tree.
	//

	void IntersectRay(
		const TreeElementProcessor<Element>& ep,
		RayIntersectionGeometric& ri,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const Scalar tmin,
		const Scalar tmax
		) const
	{
		RISE_PROFILE_INC(nBSPNodeTraversals);

		if( pElements ) {
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				RayIntersectionGeometric myRI( ri.ray, ri.rast );
				ep.RayElementIntersection( myRI, *i, bHitFrontFaces, bHitBackFaces );
				if( myRI.bHit && myRI.range < ri.range ) {
					ri = myRI;
				}
			}
		}

		if( !pChildren ) {
			return;
		}

		// Distance to the split plane along the ray
		const Scalar tSplit = (splitLocation - ri.ray.origin[splitAxis]) * ri.ray.invDir[splitAxis];

		// Near child is the one the ray enters first.
		// If ray direction is positive (sign==0), near = left (0), far = right (1).
		// If ray direction is negative (sign==1), near = right (1), far = left (0).
		const int nearIdx = ri.ray.sign[splitAxis];
		const int farIdx  = 1 - nearIdx;
		BSPTreeNodeSAH* nearChild = pChildren[nearIdx];
		BSPTreeNodeSAH* farChild  = pChildren[farIdx];

		if( tSplit < tmin ) {
			// Split plane is behind ray entry: only traverse far child
			if( farChild ) {
				farChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, tmin, tmax );
			}
		} else if( tSplit > tmax ) {
			// Split plane is beyond ray exit: only traverse near child
			if( nearChild ) {
				nearChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, tmin, tmax );
			}
		} else {
			// Ray crosses the split plane: traverse near first, then far
			if( nearChild ) {
				nearChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, tmin, tSplit );
			}

			// Only traverse far child if we haven't found a hit closer than tSplit
			if( farChild && (!ri.bHit || ri.range > tSplit) ) {
				RayIntersectionGeometric myRI( ri.ray, ri.rast );
				farChild->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, tSplit, tmax );
				if( myRI.bHit && myRI.range < ri.range ) {
					ri = myRI;
				}
			}
		}
	}

	void IntersectRay(
		const TreeElementProcessor<Element>& ep,
		RayIntersection& ri,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const bool bComputeExitInfo,
		const Scalar tmin,
		const Scalar tmax
		) const
	{
		RISE_PROFILE_INC(nBSPNodeTraversals);

		if( pElements ) {
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );
				ep.RayElementIntersection( myRI, *i, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
				if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
					ri = myRI;
				}
			}
		}

		if( !pChildren ) {
			return;
		}

		const Scalar tSplit = (splitLocation - ri.geometric.ray.origin[splitAxis]) * ri.geometric.ray.invDir[splitAxis];

		const int nearIdx = ri.geometric.ray.sign[splitAxis];
		const int farIdx  = 1 - nearIdx;
		BSPTreeNodeSAH* nearChild = pChildren[nearIdx];
		BSPTreeNodeSAH* farChild  = pChildren[farIdx];

		if( tSplit < tmin ) {
			if( farChild ) {
				farChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, tmin, tmax );
			}
		} else if( tSplit > tmax ) {
			if( nearChild ) {
				nearChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, tmin, tmax );
			}
		} else {
			if( nearChild ) {
				nearChild->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, tmin, tSplit );
			}

			if( farChild && (!ri.geometric.bHit || ri.geometric.range > tSplit) ) {
				RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );
				farChild->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, tSplit, tmax );
				if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
					ri = myRI;
				}
			}
		}
	}

	bool IntersectRay_IntersectionOnly(
		const TreeElementProcessor<Element>& ep,
		const Ray& ray,
		const Scalar dHowFar,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const Scalar tmin,
		const Scalar tmax
		) const
	{
		RISE_PROFILE_INC(nBSPNodeTraversals);

		if( pElements ) {
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				if( ep.RayElementIntersection_IntersectionOnly( ray, dHowFar, *i, bHitFrontFaces, bHitBackFaces ) ) {
					return true;
				}
			}
		}

		if( !pChildren ) {
			return false;
		}

		const Scalar tSplit = (splitLocation - ray.origin[splitAxis]) * ray.invDir[splitAxis];

		const int nearIdx = ray.sign[splitAxis];
		const int farIdx  = 1 - nearIdx;
		BSPTreeNodeSAH* nearChild = pChildren[nearIdx];
		BSPTreeNodeSAH* farChild  = pChildren[farIdx];

		if( tSplit < tmin ) {
			if( farChild ) {
				return farChild->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, tmin, tmax );
			}
		} else if( tSplit > tmax ) {
			if( nearChild ) {
				return nearChild->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, tmin, tmax );
			}
		} else {
			if( nearChild ) {
				if( nearChild->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, tmin, tSplit ) ) {
					return true;
				}
			}
			if( farChild ) {
				return farChild->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, tSplit, tmax );
			}
		}

		return false;
	}

	void Serialize( const TreeElementProcessor<Element>& ep, IWriteBuffer& buffer ) const
	{
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( pElements ? 1 : 0 );

		if( pElements ) {
			buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Element)*pElements->size() + sizeof(unsigned int)) );
			buffer.setUInt( static_cast<unsigned int>( pElements->size() ) );

			typename ElementListType::const_iterator it;
			for( it=pElements->begin(); it!=pElements->end(); it++ ) {
				ep.SerializeElement( buffer, *it );
			}
		}

		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( pChildren ? 1 : 0 );

		if( pChildren ) {
			buffer.ResizeForMore( sizeof(unsigned char) + sizeof(Scalar) );
			buffer.setUChar( splitAxis );
			buffer.setDouble( splitLocation );

			for( int i=0; i<2; i++ ) {
				buffer.ResizeForMore( sizeof(char) );
				buffer.setChar( pChildren[i] ? 1 : 0 );

				if( pChildren[i] ) {
					pChildren[i]->Serialize( ep, buffer );
				}
			}
		}
	}

	void Deserialize( const TreeElementProcessor<Element>& ep, IReadBuffer& buffer )
	{
		Clear();

		if( buffer.getChar() ) {
			const unsigned int numElements = buffer.getUInt();
			pElements = new ElementListType( numElements );
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "BSPTreeSAHNode elements" );

			for( unsigned int i=0; i<numElements; i++ ) {
				ep.DeserializeElement( buffer, (*pElements)[i] );
			}
		}

		if( buffer.getChar() ) {
			splitAxis = buffer.getUChar();
			splitLocation = buffer.getDouble();

			pChildren = new BSPTreeNodeSAH*[2];
			GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "bsptreesah children" );

			for( int i=0; i<2; i++ ) {
				if( buffer.getChar() ) {
					pChildren[i] = new BSPTreeNodeSAH();
					GlobalLog()->PrintNew( pChildren[i], __FILE__, __LINE__, "BSPTreeSAH child" );
					pChildren[i]->Deserialize( ep, buffer );
				} else {
					pChildren[i] = 0;
				}
			}
		}
	}

	unsigned int DumpStatistics( const LOG_ENUM e, const unsigned int maxElements, unsigned int& total_elems, const unsigned int tree_level ) const
	{
		unsigned int total_nodes = 1;
		char buf[1024];
		const unsigned int indent = std::min( tree_level * 2, static_cast<unsigned int>(1022) );
		memset( buf, ' ', indent );
		buf[indent] = 0;

		GlobalLog()->PrintEx( e, "%s%u:\t%d\t%lf %%", buf, tree_level, pElements ? static_cast<int>(pElements->size()) : 0,
			pElements ? Scalar(pElements->size())/Scalar(maxElements)*100.0 : 0.0 );

		if( pElements ) {
			total_elems += static_cast<unsigned int>( pElements->size() );
		}

		if( pChildren ) {
			for( int i=0; i<2; i++ ) {
				if( pChildren[i] ) {
					total_nodes += pChildren[i]->DumpStatistics( e, maxElements, total_elems, tree_level+1 );
				}
			}
		}

		return total_nodes;
	}
};
