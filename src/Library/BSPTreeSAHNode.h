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
		int bestOffset = -1;
		std::vector<BoundEdge> bestEdges;
		bestEdges.reserve( my_elements.size() * 2 );

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

			for( unsigned int i=0; i<edges.size(); i++ ) {
				if( edges[i].type == BoundEdge::EDGE_END ) {
					nAbove--;
				}

				const Scalar edgeT = edges[i].t;
				if( edgeT > nodeMin + NEARZERO && edgeT < nodeMax - NEARZERO ) {
					const unsigned char otherAxis0 = (axis + 1) % 3;
					const unsigned char otherAxis1 = (axis + 2) % 3;
					const Vector3 d = my_bb.GetExtents();
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
						bestOffset = static_cast<int>(i);
						bestEdges = edges;
					}
				}

				if( edges[i].type == BoundEdge::EDGE_START ) {
					nBelow++;
				}
			}
		}

		if( bestAxis == BSP_SAH_AXIS_INVALID || bestOffset < 0 ) {
			BuildLeaf( my_elements );
			return;
		}

		ElementInfoListType leftElements;
		ElementInfoListType rightElements;
		leftElements.reserve( my_elements.size() );
		rightElements.reserve( my_elements.size() );

		for( int i=0; i<bestOffset; i++ ) {
			if( bestEdges[i].type == BoundEdge::EDGE_START ) {
				leftElements.push_back( my_elements[bestEdges[i].primNum] );
			}
		}

		for( unsigned int i=static_cast<unsigned int>(bestOffset+1); i<bestEdges.size(); i++ ) {
			if( bestEdges[i].type == BoundEdge::EDGE_END ) {
				rightElements.push_back( my_elements[bestEdges[i].primNum] );
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

	void IntersectRayBB( const BoundingBox& my_bb, const Ray& ray, BOX_HIT& h ) const
	{
		if( GeometricUtilities::IsPointInsideBox( ray.origin, my_bb.ll, my_bb.ur ) ) {
			h.bHit = true;
			h.dRange = NEARZERO;
		} else {
			RayBoxIntersection( ray, h, my_bb.ll, my_bb.ur );
		}
	}

	void IntersectRay(
		const TreeElementProcessor<Element>& ep,
		RayIntersectionGeometric& ri,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const BoundingBox& my_bb
		) const
	{
		RISE_PROFILE_INC(nBSPNodeTraversals);

		if( pChildren ) {
			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			SplitBoundingBox( my_bb, splitAxis, splitLocation, bba, bbb );

			if( pChildren[0] ) {
				pChildren[0]->IntersectRayBB( bba, ri.ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( bbb, ri.ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				const Scalar Vd = ha.dRange - hb.dRange;
				static const Scalar DISTANCE_THRESHOLD = 0.001;

				if( Vd < -DISTANCE_THRESHOLD ) {
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bba );
					if( !ri.bHit || (ri.range > hb.dRange) ) {
						RayIntersectionGeometric myRI( ri.ray, ri.rast );
						pChildren[1]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bbb );
						if( myRI.bHit && myRI.range < ri.range ) {
							ri = myRI;
						}
					}
				} else if( Vd > DISTANCE_THRESHOLD ) {
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb );
					if( !ri.bHit || (ri.range > ha.dRange) ) {
						RayIntersectionGeometric myRI( ri.ray, ri.rast );
						pChildren[0]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bba );
						if( myRI.bHit && myRI.range < ri.range ) {
							ri = myRI;
						}
					}
				} else {
					RayIntersectionGeometric ri_temp( ri.ray, ri.rast );
					pChildren[0]->IntersectRay( ep, ri_temp, bHitFrontFaces, bHitBackFaces, bba );
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb );

					if( ri.bHit && ri_temp.bHit ) {
						if( ri_temp.range < ri.range ) {
							ri = ri_temp;
						}
					} else if( ri_temp.bHit ) {
						ri = ri_temp;
					}
				}
			} else {
				if( ha.bHit ) {
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bba );
				}

				if( hb.bHit ) {
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb );
				}
			}
		}

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
	}

	void IntersectRay(
		const TreeElementProcessor<Element>& ep,
		RayIntersection& ri,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const bool bComputeExitInfo,
		const BoundingBox& my_bb
		) const
	{
		RISE_PROFILE_INC(nBSPNodeTraversals);

		if( pChildren ) {
			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			SplitBoundingBox( my_bb, splitAxis, splitLocation, bba, bbb );

			if( pChildren[0] ) {
				pChildren[0]->IntersectRayBB( bba, ri.geometric.ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( bbb, ri.geometric.ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				const Scalar Vd = ha.dRange - hb.dRange;
				static const Scalar DISTANCE_THRESHOLD = 0.001;

				if( Vd < -DISTANCE_THRESHOLD ) {
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba );
					if( !ri.geometric.bHit || (ri.geometric.range > hb.dRange) ) {
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );
						pChildren[1]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb );
						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				} else if( Vd > DISTANCE_THRESHOLD ) {
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb );
					if( !ri.geometric.bHit || (ri.geometric.range > ha.dRange) ) {
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );
						pChildren[0]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba );
						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				} else {
					RayIntersection ri_temp( ri.geometric.ray, ri.geometric.rast );
					pChildren[0]->IntersectRay( ep, ri_temp, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba );
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb );

					if( ri.geometric.bHit && ri_temp.geometric.bHit ) {
						if( ri_temp.geometric.range < ri.geometric.range ) {
							ri = ri_temp;
						}
					} else if( ri_temp.geometric.bHit ) {
						ri = ri_temp;
					}
				}
			} else {
				if( ha.bHit ) {
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba );
				}

				if( hb.bHit ) {
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb );
				}
			}
		}

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
	}

	bool IntersectRay_IntersectionOnly(
		const TreeElementProcessor<Element>& ep,
		const Ray& ray,
		const Scalar dHowFar,
		const bool bHitFrontFaces,
		const bool bHitBackFaces,
		const BoundingBox& my_bb
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

		if( pChildren ) {
			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			SplitBoundingBox( my_bb, splitAxis, splitLocation, bba, bbb );

			if( pChildren[0] ) {
				pChildren[0]->IntersectRayBB( bba, ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( bbb, ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				if( ha.dRange <= hb.dRange ) {
					if( pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba ) ) {
						return true;
					}
					return pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb );
				}

				if( pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb ) ) {
					return true;
				}
				return pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba );
			}

			if( ha.bHit ) {
				return pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba );
			}

			if( hb.bHit ) {
				return pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb );
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
