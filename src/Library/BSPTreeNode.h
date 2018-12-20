//////////////////////////////////////////////////////////////////////
//
//  BSPTreeNode.h - Definition of the BSPTree node. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2002
//  Tabs: 4
//  Comments: NOTE:  NEVER include this file directly, it is meant
//  to be included from BSPTree.h as part of the templated
//  BSPTree class.  Attemping to include this file directly
//  will result in compiler errors!!!!!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

class BSPTreeNode
{
protected:
	BSPTreeNode**			pChildren;
	ElementListType*		pElements;

public:
	BSPTreeNode( ) : 
	  pChildren( 0 ), pElements( 0 )
	{
	}

	virtual ~BSPTreeNode( )
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
			delete pChildren;
			pChildren = 0;
		}

		if( pElements ) {
			GlobalLog()->PrintDelete( pElements, __FILE__, __LINE__ );
			delete pElements;
			pElements = 0;
		}
	}

	void MySplittingPlane( const BoundingBox& bb, char which_child, Plane& my_p ) const 
	{
		// The only thing we need from which_child is what kind of splitting plane
		// we are supposed to be
		// We can get the center of the parent plane from the parent bounding box
		// And based on that we can figure out the our center
		// Encoding:
		//   lowest bit 0 - left, 1 - right
		//  The 2nd and 3rd bits specify the axis
		//   2 - X axis
		//   4 - Y axis
		//   6 - Z axis
		switch( which_child )
		{
		case 2:
		case 3:
			{
				my_p.Set( Vector3(1,0,0), bb.GetCenter() );
			}
			break;
		case 4:
		case 5:
			{
				my_p.Set( Vector3(0,1,0), bb.GetCenter() );
			}
			break;
		case 8:
		case 9:
			{
				my_p.Set( Vector3(0,0,1), bb.GetCenter() );
			}
			break;
		};
	}

	void MyBBFromParent( const BoundingBox& bbox, char which_child, BoundingBox& my_bb ) const
	{
//		static const Scalar box_error = NEARZERO * 100.0;
		static const Scalar box_error = NEARZERO;

		// There are two things encoded in which child.
		// The first is the axis to split along
		// The second is whether we are the left or right child
		// Encoding:
		//   lowest bit 0 - left, 1 - right
		//  The 2nd and 3rd bits specify the axis
		//   2 - X axis
		//   4 - Y axis
		//   6 - Z axis
		//   This gives 6 possible combinations:
		//     2 - X on left
		//     3 - X on right
		//     4 - Y on left
		//     5 - Y on right
		//     8 - Z on left
		//     9 - Z on right
		//    10 - all
		switch( which_child )
		{
		case 10:
			// Entire
			my_bb = bbox;
			break;
		case 2:
			{
				const Scalar	AvgX = (bbox.ll.x + bbox.ur.x)*0.5;
				my_bb.ll = bbox.ll;
				my_bb.ur = Point3( AvgX+box_error, bbox.ur.y, bbox.ur.z );
			}
			break;
		case 3:
			{
				const Scalar	AvgX = (bbox.ll.x + bbox.ur.x)*0.5;
				my_bb.ll = Point3( AvgX-box_error, bbox.ll.y, bbox.ll.z );
				my_bb.ur = bbox.ur;
			}
			break;
		case 4:
			{
				const Scalar	AvgY = (bbox.ll.y + bbox.ur.y)*0.5;
				my_bb.ll = bbox.ll;
				my_bb.ur = Point3( bbox.ur.x, AvgY+box_error, bbox.ur.z );
			}
			break;
		case 5:
			{
				const Scalar	AvgY = (bbox.ll.y + bbox.ur.y)*0.5;
				my_bb.ll = Point3( bbox.ll.x, AvgY-box_error, bbox.ll.z );
				my_bb.ur = bbox.ur;
			}
			break;
		case 8:
			{
				const Scalar	AvgZ = (bbox.ll.z + bbox.ur.z)*0.5;
				my_bb.ll = bbox.ll;
				my_bb.ur = Point3( bbox.ur.x, bbox.ur.y, AvgZ+box_error );
			}
			break;
		case 9:
			{
				const Scalar	AvgZ = (bbox.ll.z + bbox.ur.z)*0.5;
				my_bb.ll = Point3( bbox.ll.x, bbox.ll.y, AvgZ-box_error );
				my_bb.ur = bbox.ur;
			}
			break;
		};
	}

	void AddElements( 
		const TreeElementProcessor<Element>& ep,
		const std::vector<Element>& my_elements,
		const std::vector<Element>& shared_elements,
		const unsigned int maxElements,
		const BoundingBox& bbox,
		const char which_child,
		const unsigned char max_recursion_level
		)
	{
		// We add the given elements to our section, 
		// First we see how many elements qualify for us, if that number is less than
		// or equal to the minimum defined, we don't create any children, and we simply keep
		// the polygons

		// If children must be created we subdivide evenly into 8 children passing
		// the element list

		static unsigned int tree_level = 0;
		tree_level++;

		BoundingBox my_bb;
		MyBBFromParent( bbox, which_child, my_bb );
		
		// If the box for this node is only slightly bigger than the epsilon, then there's
		// no point in creating children...
		const Vector3 vBoxSize = my_bb.GetExtents();

		// If we have reached the maximum recursion level, then stop and don't try to create any more children
		const unsigned int total_count = my_elements.size()+shared_elements.size();
		if( vBoxSize.x <= bsp_error_delta_box_size ||
			vBoxSize.y <= bsp_error_delta_box_size || 
			vBoxSize.z <= bsp_error_delta_box_size ||
			tree_level > max_recursion_level || 
			total_count <= maxElements )
		{
			pElements = new ElementListType( total_count );
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );

			typename ElementListType::iterator it = std::copy( my_elements.begin(), my_elements.end(), pElements->begin() );
			if( shared_elements.size() ) {
				std::copy( shared_elements.begin(), shared_elements.end(), it );
			}
		}
		else
		{
			// Subdivision required
			// Make the two children
			pChildren = new BSPTreeNode*[2];
			GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "bsptree children" );

			const char child_base_type = next_base_type[(which_child&0xfe)>>2];

			// Create the polygon list for the left child, based on plane intersection
			Plane my_plane;
			MySplittingPlane( my_bb, child_base_type, my_plane );

			{
				ElementListType left_elements;
				ElementListType right_elements;
				ElementListType both_elements;

				typename ElementListType::const_iterator i, e;
				for( i=my_elements.begin(), e=my_elements.end(); i!=e; i++ ) {
					const Element& p = *i;
					char side = ep.WhichSideofPlaneIsElement( p, my_plane );
					if( side == 0 ) {
						left_elements.push_back( p );
					} else if( side == 1 ) {
						right_elements.push_back( p );
					} else {
						both_elements.push_back( p );
					}
				}

				for( i=shared_elements.begin(), e=shared_elements.end(); i!=e; i++ ) {
					const Element& p = *i;
					char side = ep.WhichSideofPlaneIsElement( p, my_plane );
					if( side == 0 ) {
						left_elements.push_back( p );
					} else if( side == 1 ) {
						right_elements.push_back( p );
					} else {
						both_elements.push_back( p );
					}
				}

				if( both_elements.size() ) {
					if( (both_elements.size() <= maxElements/*>>1*/) ||									// check if it doesn't matter to split
						((both_elements.size() == my_elements.size()+shared_elements.size()) && tree_level>=(unsigned int)(max_recursion_level>>2) )			// check if we split nothing
						)
					{
						// Might as well keep it here!
						pElements = new ElementListType( both_elements.size() );
						GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );
						std::copy( both_elements.begin(), both_elements.end(), pElements->begin() );

						stl_utils::container_erase_all(both_elements);
					}
				}

				if( left_elements.size()||both_elements.size() ) {
					pChildren[0] = new BSPTreeNode();
					GlobalLog()->PrintNew( pChildren[0], __FILE__, __LINE__, "Left ChildNode" );
					
					pChildren[0]->AddElements( ep, left_elements, both_elements, maxElements, my_bb, child_base_type, max_recursion_level );
				} else {
					pChildren[0] = 0;
				}

				if( right_elements.size()||both_elements.size() ) {
					pChildren[1] = new BSPTreeNode();
					GlobalLog()->PrintNew( pChildren[1], __FILE__, __LINE__, "Right ChildNode" );

					pChildren[1]->AddElements( ep, right_elements, both_elements, maxElements, my_bb, child_base_type+1, max_recursion_level );
				} else {
					pChildren[1] = 0;
				}
			}
		}

		tree_level--;
	}

	void IntersectRayBB( 
		const BoundingBox& bbox,
		BoundingBox& my_bb,
		const char which_child,
		const Ray& ray, 
		BOX_HIT& h 
		) const
	{
		MyBBFromParent( bbox, which_child, my_bb );

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
		const BoundingBox& my_bb, 
		const char which_child
		) const
	{
		// We can have both children and elements
		if( pChildren ) {
			// We have children instead
			// Pass the call down to our children
			const char child_base_type = next_base_type[(which_child&0xfe)>>2];

			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			if( pChildren[0] ) { 
				pChildren[0]->IntersectRayBB( my_bb, bba, child_base_type, ri.ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( my_bb, bbb, child_base_type+1, ri.ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				const Scalar Vd = ha.dRange - hb.dRange;
				static const Scalar DISTANCE_THRESHOLD = 0.001;

				if( Vd < -DISTANCE_THRESHOLD ) {
					// Left then right
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bba, child_base_type );
					if( !ri.bHit || (ri.range > hb.dRange) ) {
						RayIntersectionGeometric myRI( ri.ray, ri.rast );

						pChildren[1]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );

						if( (myRI.bHit && myRI.range < ri.range) ) {
							ri = myRI;
						}
					}
				} else if( Vd > DISTANCE_THRESHOLD ) {
					// Right then left
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );
					if( !ri.bHit || (ri.range > ha.dRange) ) {
						RayIntersectionGeometric myRI( ri.ray, ri.rast );

						pChildren[0]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bba, child_base_type );

						if( myRI.bHit && myRI.range < ri.range ) {
							ri = myRI;
						}
					}
				} else {
					// Do both
					RayIntersectionGeometric ri_temp( ri.ray, ri.rast );
					pChildren[0]->IntersectRay( ep, ri_temp, bHitFrontFaces, bHitBackFaces, bba, child_base_type );
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );

					if( ri.bHit && ri_temp.bHit ) {
						// Take the closest one
						if( ri_temp.range < ri.range ) {
							ri = ri_temp;
						}
					} else if( ri_temp.bHit ) {
						ri = ri_temp;
					}
				}
				 
			} else {
				if( ha.bHit ) {
					// Left child only
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bba, child_base_type );
				}
				if( hb.bHit ) {
					// Right child only
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );
				}
			}
		}

		if( pElements )
		{
			// Otherwise we just go through our element list and check each
			// element to see if any of them intersect
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ )
			{
				const Element& p = (*i);

				RayIntersectionGeometric myRI( ri.ray, ri.rast );

				ep.RayElementIntersection( myRI, p, bHitFrontFaces, bHitBackFaces );

				if( myRI.bHit ) {
					// We are only interested in the element closest to us
					if( myRI.range < ri.range ) {
						ri = myRI;
					}
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
		const BoundingBox& my_bb, 
		const char which_child
		) const
	{
		// We can have both children and elements
		if( pChildren ) {
			// We have children instead
			// Pass the call down to our children
			const char child_base_type = next_base_type[(which_child&0xfe)>>2];

			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			if( pChildren[0] ) { 
				pChildren[0]->IntersectRayBB( my_bb, bba, child_base_type, ri.geometric.ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( my_bb, bbb, child_base_type+1, ri.geometric.ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				const Scalar Vd = ha.dRange - hb.dRange;
				static const Scalar DISTANCE_THRESHOLD = 0.001;

				if( Vd < -DISTANCE_THRESHOLD ) {
					// Left then right
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba, child_base_type );
					if( !ri.geometric.bHit || (ri.geometric.range > hb.dRange) ) {
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );

						pChildren[1]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb, child_base_type+1 );

						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				} else if( Vd > DISTANCE_THRESHOLD ) {
					// Right then left
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb, child_base_type+1 );
					if( !ri.geometric.bHit || (ri.geometric.range > ha.dRange) ) {
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );

						pChildren[0]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba, child_base_type );

						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				} else {
					// Do both
					RayIntersection ri_temp( ri.geometric.ray, ri.geometric.rast );
					pChildren[0]->IntersectRay( ep, ri_temp, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba, child_base_type );
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb, child_base_type+1 );

					if( ri.geometric.bHit && ri_temp.geometric.bHit ) {
						// Take the closest one
						if( ri_temp.geometric.range < ri.geometric.range ) {
							ri = ri_temp;
						}
					} else if( ri_temp.geometric.bHit ) {
						ri = ri_temp;
					}
				}
				 
			} else {
				if( ha.bHit ) {
					// Left child only
					pChildren[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bba, child_base_type );
				}
				if( hb.bHit ) {
					// Right child only
					pChildren[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbb, child_base_type+1 );
				}
			}
		}

		if( pElements )
		{
			// Otherwise we just go through our element list and check each
			// element to see if any of them intersect
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ )
			{
				const Element& p = (*i);
				RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );

				ep.RayElementIntersection( myRI, p, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );

				if( myRI.geometric.bHit ) {
					// We are only interested in the element closest to us
					if( myRI.geometric.range < ri.geometric.range ) {
						ri = myRI;
					}
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
		const BoundingBox& my_bb,
		const char which_child
		) const
	{		
		if( pElements )
		{
			// Just go through our element list and check each
			// element to see if any of them intersect
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				const Element& p = (*i);

				if( ep.RayElementIntersection_IntersectionOnly( ray, dHowFar, p, bHitFrontFaces, bHitBackFaces) ) {
					return true;
				}
			}
		} 
		
		if( pChildren ) {
			// We have children
			// We have children instead
			// Pass the call down to our children
			const char child_base_type = next_base_type[(which_child&0xfe)>>2];

			BOX_HIT ha, hb;
			BoundingBox bba, bbb;
			if( pChildren[0] ) { 
				pChildren[0]->IntersectRayBB( my_bb, bba, child_base_type, ray, ha );
			}

			if( pChildren[1] ) {
				pChildren[1]->IntersectRayBB( my_bb, bbb, child_base_type+1, ray, hb );
			}

			if( ha.bHit && hb.bHit ) {
				const Scalar Vd = ha.dRange - hb.dRange;

				if( Vd <= 0 ) {
					// Left then right
					if( !pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba, child_base_type ) ) {
						return pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );
					} else {
						return true;
					}
				} else if( Vd > 0 ) {
					// Right then left
					if( !pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 ) ) {
						return pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba, child_base_type );
					} else {
						return true;
					}
				}				 
			} else {
				if( ha.bHit ) {
					// Left child only
					return pChildren[0]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bba, child_base_type );
				}
				if( hb.bHit ) {
					// Right child only
					return pChildren[1]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbb, child_base_type+1 );
				}
			}
		}

		return false;
	}

	void Serialize( const TreeElementProcessor<Element>& ep, IWriteBuffer& buffer ) const
	{
		// Write whether we have elements
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( pElements ? 1 : 0 );
		
		// If we have elements, then write out the elements
		if( pElements ) {
			buffer.ResizeForMore( sizeof(Element)*pElements->size() + sizeof( unsigned int ) );
			buffer.setUInt( pElements->size() );

			typename ElementListType::const_iterator		it;
			for( it=pElements->begin(); it!=pElements->end(); it++ ) {
				ep.SerializeElement( buffer, *it );
			}			
		}

		// If we have children, then write out each of the children
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( pChildren ? 1 : 0);

		if( pChildren ) {
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
		// Check whether we have elements
		char bElements = buffer.getChar();

		if( bElements ) {
			unsigned int numElements = buffer.getUInt();
			pElements = new ElementListType( numElements );
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "BSPTreenode elements" );

			for( unsigned int i=0; i<numElements; i++ ) {
				ep.DeserializeElement( buffer, (*pElements)[i] );
			}
		}

		// Check if children exist
		if( buffer.getChar() ) {
			pChildren = new BSPTreeNode*[2];
			GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "bsptree children" );

			for( int i=0; i<2; i++ ) {
				// Check if each child exists
				if( buffer.getChar() ) {
					pChildren[i] = new BSPTreeNode();
					GlobalLog()->PrintNew( pChildren[i], __FILE__, __LINE__, "BSPTreenode left child" );
					pChildren[i]->Deserialize( ep, buffer );
				} else {
					pChildren[i] = 0;
				}
			}
		}
	}


	unsigned int DumpStatistics( const LOG_ENUM e, const unsigned int p, const unsigned int maxElements, unsigned int& total_elems ) const
	{
		static unsigned int tree_level = 0;
		unsigned int total_nodes = 0;
		tree_level++;
		char buf[1024];
		memset( buf, ' ', tree_level*2 );
		buf[tree_level*2] = 0;

		GlobalLog()->PrintEx(e,p,"%s%d:\t%d\t%lf %%", buf, tree_level, pElements?pElements->size():0, pElements?Scalar(pElements->size())/Scalar(maxElements)*100 : 0 );

		if( pElements ) {
			total_elems += pElements->size();
		}

		if( pChildren ) {
			for( int i=0; i<2; i++ ) {
				if( pChildren[1] ) {
					total_nodes += pChildren[1]->DumpStatistics(e,p,maxElements,total_elems) + 1;		
				}
			}
		}

		tree_level--;

		return total_nodes;
	}
};
