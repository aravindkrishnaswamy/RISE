//////////////////////////////////////////////////////////////////////
//
//  OctreeNode.h - Definition of the octree node. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2002
//  Tabs: 4
//  Comments: NOTE:  NEVER include this file directly, it is meant
//  to be included from Octree.h as part of the templated
//  Octree class.  Attemping to include this file directly
//  will result in compiler errors!!!!!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

class OctreeNode
{
protected:
	typedef std::vector<Element>	ElementListType;

	OctreeNode**			pChildren;		// The eight children of this node
	ElementListType*		pElements;

public:
	OctreeNode( ) : 
	  pChildren( 0 ), pElements( 0 )
	{
	}

	virtual ~OctreeNode( )
	{
		if( pChildren ) {
			for( int i=0; i<8; i++ ) {
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

	void MyBBFromParent( const BoundingBox& bbox, char which_child, BoundingBox& my_bb ) const
	{
		// Figure out our bouding box based on the parent's bounding box and which child
		// we are... 
		Point3 ptBoxCenter = Point3Ops::WeightedAverage2( bbox.ll, bbox.ur, 0.5 );

		const Scalar&	AvgX = ptBoxCenter.x;
		const Scalar&	AvgY = ptBoxCenter.y;
		const Scalar&	AvgZ = ptBoxCenter.z;

//		static const Scalar box_error = NEARZERO * 10.0;
		static const Scalar box_error = NEARZERO;

		switch( which_child )
		{
		case 99:
			// Entire
			my_bb = bbox;
			break;
		case 0:
			// Sub node 1, same LL as us, UR as our center
			my_bb.ll = Point3( bbox.ll.x-box_error, bbox.ll.y-box_error, bbox.ll.z-box_error );
			my_bb.ur = Point3( ptBoxCenter.x+box_error, ptBoxCenter.y+box_error, ptBoxCenter.z+box_error );
			break;
		case 1:
			// Sub node 2, almost the same LL as us, but x is now averaged with max
			// UR is our UR but z and y is averaged with min
			my_bb.ll = Point3( AvgX-box_error, bbox.ll.y-box_error, bbox.ll.z-box_error );
			my_bb.ur = Point3( bbox.ur.x+box_error, AvgY+box_error, AvgZ+box_error );
			break;
		case 2:
			// Sub node 3, almost same LL as us, but y is averaged. UR is same for y but x and 
			// z are averaged
			my_bb.ll = Point3( bbox.ll.x-box_error, AvgY-box_error, bbox.ll.z-box_error );
			my_bb.ur = Point3( AvgX+box_error, bbox.ur.y+box_error, AvgZ+box_error );
			break;
		case 3:
			// Sub node 4, LL.z is same as our LL but x and y are averaged, UR x and y are our UR but z is averaged
			my_bb.ll = Point3( AvgX-box_error, AvgY-box_error, bbox.ll.z-box_error );
			my_bb.ur = Point3( bbox.ur.x+box_error, bbox.ur.y+box_error, AvgZ+box_error );
			break;
		case 4:
			// Sub node 5, LL x and y is our LL, z is averaged, UR z is our UR y and z are averaged
			my_bb.ll = Point3( bbox.ll.x-box_error, bbox.ll.y-box_error, AvgZ-box_error );
			my_bb.ur = Point3( AvgX+box_error, AvgY+box_error, bbox.ur.z+box_error );
			break;
		case 5:
			// Sub node 6, LL x and z are averaged, y is our LL, UR, x and z are our UR and y is averaged
			my_bb.ll = Point3( AvgX-box_error, bbox.ll.y-box_error, AvgZ-box_error );
			my_bb.ur = Point3( bbox.ur.x+box_error, AvgY+box_error, bbox.ur.z+box_error );
			break;
		case 6:
			// Sub node 7, LL y and z are averaged, x is our LL, UR, y and z are our UR and x is averaged
			my_bb.ll = Point3( bbox.ll.x-box_error, AvgY-box_error, AvgZ-box_error );
			my_bb.ur = Point3( AvgX+box_error, bbox.ur.y+box_error, bbox.ur.z+box_error );
			break;
		case 7:
			// Sub node 8, LL is the center and UR is our UR
			my_bb.ll = Point3( ptBoxCenter.x-box_error, ptBoxCenter.y-box_error, ptBoxCenter.z-box_error );
			my_bb.ur = Point3( bbox.ur.x+box_error, bbox.ur.y+box_error, bbox.ur.z+box_error );
			break;
		};
	}

	bool AddElements( 
		const TreeElementProcessor<Element>& ep,
		const std::vector<Element>& elements,
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
		
		ElementListType elements_list;
		typename ElementListType::const_iterator i, e;
		for( i=elements.begin(), e=elements.end(); i!=e; i++ ) {
			const Element& p = *i;
			if( ep.ElementBoxIntersection( p, my_bb ) ) {
				elements_list.push_back( p );
			}
		}

		if( elements_list.size() < 1 ) {
			tree_level--;
			return false;
		}

		// If the box for this node is only slightly bigger than the epsilon, then there's
		// no point in creating children...
		Vector3 vBoxSize = my_bb.GetExtents();//Vector3Ops::mkVector3( my_ur, my_ll );

		// If we have reached the maximum recursion level, then stop and don't try to create any more children
		if( vBoxSize.x <= error_delta_box_size || vBoxSize.y <= error_delta_box_size || vBoxSize.z <= error_delta_box_size ||
			tree_level > max_recursion_level || 
			elements_list.size() <= maxElements )
		{
			pElements = new ElementListType( elements_list.size() );
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );
			std::copy( elements_list.begin(), elements_list.end(), pElements->begin() );

			tree_level--;
			return true;
		}
		else
		{
			unsigned char	numRejects = 0;		// keeps track of how many children of this
												// node are crap, if they are all crap
												// then this node doesn't need to exist at all!

			// Subdivision required
			// Make eight children
			pChildren = new OctreeNode*[8];
			GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "octree children" );

			for( unsigned char x=0; x<8; x++ )
			{
				pChildren[x] = new OctreeNode( );
				GlobalLog()->PrintNew( pChildren[x], __FILE__, __LINE__, "ChildNode" );
				if( !pChildren[x]->AddElements( ep, elements_list, maxElements, my_bb, x, max_recursion_level ) ) {
					GlobalLog()->PrintDelete( pChildren[x], __FILE__, __LINE__ );
					delete pChildren[x];
					pChildren[x] = 0;
					numRejects++;
				}
			}

			// If all of our children have been rejected, then there's no reason for this node itself
			// to even exist!
			if( numRejects == 8 ) {
				GlobalLog()->Print( eLog_Error, "OctreeNode: I have elements but none of my children do!  Should never happen" );

				GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
				delete pChildren;
				pChildren = 0;
				tree_level--;
				return false;
			}
		}

		tree_level--;
		return true;
	}

	void IntersectRayBB( const BoundingBox& bbox, const char which_child,
						 const Ray& ray, BOX_HIT& h ) const
	{
		BoundingBox my_bb;
			
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
		const BoundingBox& bbox, 
		const char which_child 
		) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB.
		// We check all the octant nodes that have an intersection, however
		// we will not check those nodes that we can conclusively say
		// will not hit, or is behind existing nodes

		ri.bHit = false;
		ri.range = INFINITY;

		BoundingBox my_bb;
			
		MyBBFromParent( bbox, which_child, my_bb );

		if( pChildren )
		{
			Scalar			ranges[8];
			OctreeNode*		childrenhit[8];
			char			nodeid[8];
			unsigned char	numchildrenhit=0;

			for( unsigned char i=0; i<8; i++ )
			{
				if( pChildren[i] )
				{
					BOX_HIT	h;
					pChildren[i]->IntersectRayBB( my_bb, i, ri.ray, h );

					// This checks not only if a particular octant node 
					// was hit, but also checks to see if the closest distance
					// in that octant node is too far away for us to care!
					if( h.bHit ) {
						childrenhit[numchildrenhit] = pChildren[i];
						ranges[numchildrenhit] = h.dRange;
						nodeid[numchildrenhit] = i;
						numchildrenhit++;
					}
				}
			}

			// We add a little optimization here
			// This was giving problems with 3DS MAX meshes with lots of polygons
			// I suspect that the ranges are too close
			
			if( numchildrenhit==1 ) {
				// Special case for only one child hit
				childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[0] );
			} else if( numchildrenhit==2 ) {
				// Special case for only two child hits
				if( ranges[0] > ranges[1] ) {
					// Do 1 first!
					childrenhit[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[1] );
					if( !ri.bHit || (ri.range > ranges[0]) ) {
						RayIntersectionGeometric myRI( ri.ray, ri.rast );

						childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[0] );

						if( myRI.bHit && myRI.range < ri.range ) {
							ri = myRI;
						}
					}
				} else {
					// Do 2 first!
					childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[0] );
					if( !ri.bHit || (ri.range > ranges[1]) ) {\
						RayIntersectionGeometric myRI( ri.ray, ri.rast );

						childrenhit[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[1] );

						if( myRI.bHit && myRI.range < ri.range ) {
							ri = myRI;
						}
					}
				}
			} else if( numchildrenhit==3 ) {
				int a, b, c;

				// Special case for three child hits
				if( ranges[0] > ranges[1] ) {
					if( ranges[0] > ranges[2] ) {
						if( ranges[1] > ranges[2] ) {
							// 2, 1, 0
							a=2; b=1; c=0;
						} else {
							// 1, 2, 0
							a=1; b=2; c=0;
						}
					} else {
						// 1, 0, 2
						a=1; b=0; c=2;
					}
				} else {
					if( ranges[1] > ranges[2] ) {
						if( ranges[2] > ranges[0] ) {
							// 0, 2, 1
							a=0; b=2; c=1;
						} else {
							// 2, 0, 1
							a=2; b=0; c=1;
						}
					} else {
						// 0, 1, 2
						a=0; b=1; c=2;
					}
				}

				childrenhit[a]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[a] );
				if( !ri.bHit || (ri.range > ranges[b]) ) {
					RayIntersectionGeometric myRI( ri.ray, ri.rast );

					childrenhit[b]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[b] );

					if( myRI.bHit && myRI.range < ri.range ) {
						ri = myRI;
					}
				}
				if( !ri.bHit || (ri.range > ranges[c]) ) {
					RayIntersectionGeometric myRI( ri.ray, ri.rast );

					childrenhit[c]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[c] );

					if( myRI.bHit && myRI.range < ri.range ) {
						ri = myRI;
					}
				}
			} else if( numchildrenhit >= 4 ) {
				// Otherwise, forget it...
				// Run through the children
				for( int m=0; m<numchildrenhit; m++ )
				{
					RayIntersectionGeometric myRI( ri.ray, ri.rast );

					if( ranges[m] <= ri.range )		// Make sure we don't check a node if we don't have to
					{
						childrenhit[m]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[m] );

						if( myRI.bHit ) {
							if( myRI.range < ri.range ) {
								ri = myRI;
							}
						}
					}
				}
			}
		}
		else if( pElements )
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
		const BoundingBox& bbox, 
		const char which_child 
		) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB.
		// We check all the octant nodes that have an intersection, however
		// we will not check those nodes that we can conclusively say
		// will not hit, or is behind existing nodes

		ri.geometric.bHit = false;
		ri.geometric.range = INFINITY;

		BoundingBox my_bb;
			
		MyBBFromParent( bbox, which_child, my_bb );

		if( pChildren )
		{
			Scalar			ranges[8];
			OctreeNode*		childrenhit[8];
			char			nodeid[8];
			unsigned char	numchildrenhit=0;

			for( unsigned char i=0; i<8; i++ )
			{
				if( pChildren[i] )
				{
					BOX_HIT	h;
					pChildren[i]->IntersectRayBB( my_bb, i, ri.geometric.ray, h );

					// This checks not only if a particular octant node 
					// was hit, but also checks to see if the closest distance
					// in that octant node is too far away for us to care!
					if( h.bHit ) {
						childrenhit[numchildrenhit] = pChildren[i];
						ranges[numchildrenhit] = h.dRange;
						nodeid[numchildrenhit] = i;
						numchildrenhit++;
					}
				}
			}

			// We add a little optimization here
			// We can attack the candidate nodes in a sorted order which manages to speed things up
			// a little
			
			if( numchildrenhit==1 ) {
				// Special case for only one child hit
				childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[0] );
			} else if( numchildrenhit==2 ) {
				// Special case for only two child hits
				if( ranges[0] > ranges[1] ) {
					// Do 1 first!
					childrenhit[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[1] );
					if( !ri.geometric.bHit || (ri.geometric.range > ranges[0]) ) {
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );

						childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[0] );

						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				} else {
					// Do 2 first!
					childrenhit[0]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[0] );
					if( !ri.geometric.bHit || (ri.geometric.range > ranges[1]) ) {\
						RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );

						childrenhit[1]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[1] );

						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				}
			} else if( numchildrenhit==3 ) {
				int a, b, c;

				// Special case for three child hits
				if( ranges[0] > ranges[1] ) {
					if( ranges[0] > ranges[2] ) {
						if( ranges[1] > ranges[2] ) {
							// 2, 1, 0
							a=2; b=1; c=0;
						} else {
							// 1, 2, 0
							a=1; b=2; c=0;
						}
					} else {
						// 1, 0, 2
						a=1; b=0; c=2;
					}
				} else {
					if( ranges[1] > ranges[2] ) {
						if( ranges[2] > ranges[0] ) {
							// 0, 2, 1
							a=0; b=2; c=1;
						} else {
							// 2, 0, 1
							a=2; b=0; c=1;
						}
					} else {
						// 0, 1, 2
						a=0; b=1; c=2;
					}
				}

				childrenhit[a]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[a] );
				if( !ri.geometric.bHit || (ri.geometric.range > ranges[b]) ) {
					RayIntersection		myRI( ri.geometric.ray, ri.geometric.rast );

					childrenhit[b]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[b] );

					if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
						ri = myRI;
					}
				}
				if( !ri.geometric.bHit || (ri.geometric.range > ranges[c]) ) {
					RayIntersection		myRI( ri.geometric.ray, ri.geometric.rast );

					childrenhit[c]->IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[c] );

					if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
						ri = myRI;
					}
				}
			} else if( numchildrenhit >= 4 ) {
				// Otherwise, forget it...
				// Run through the children
				for( int m=0; m<numchildrenhit; m++ )
				{
					RayIntersection		myRI( ri.geometric.ray, ri.geometric.rast );

					if( ranges[m] <= ri.geometric.range )		// Make sure we don't check a node if we don't have to
					{
						childrenhit[m]->IntersectRay( ep, myRI, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, my_bb, nodeid[m] );

						if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
							ri = myRI;
						}
					}
				}
			}
		}
		else if( pElements )
		{
			// Otherwise we just go through our element list and check each
			// element to see if any of them intersect
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ )
			{
				const Element& p = (*i);

				RayIntersection		myRI( ri.geometric.ray, ri.geometric.rast );

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
		const BoundingBox& bbox, 
		const char which_child 
		) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB, take the 
		// smallest range and call IntersectRay on that child, if that
		// child fails, we try the second smallest and so on, until we 
		// either run out of children or we get an intersection
		if( pChildren )
		{
			BoundingBox my_bb;
			
			MyBBFromParent( bbox, which_child, my_bb );

			OctreeNode*		childrenhit[8];
			unsigned char	numchildrenhit=0;
			unsigned char	nodeid[8];

			for( unsigned char i=0; i<8; i++ ) {
				if( pChildren[i] ) {
					BOX_HIT	h;
					pChildren[i]->IntersectRayBB( my_bb, i, ray, h );

					if( (h.dRange < dHowFar) ||  
						GeometricUtilities::IsPointInsideBox( ray.origin, bbox.ll, bbox.ur )
						)
					{
						childrenhit[numchildrenhit] = pChildren[i];
						nodeid[numchildrenhit] = i;
						numchildrenhit++;
					}
				}
			}

			unsigned char m;
			
			// run through the successful children in order
			for( m=0; m<numchildrenhit; m++ ) {
				if( childrenhit[m]->IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, my_bb, nodeid[m] ) ) {
					return true;
				}
			}

		}
		else if( pElements )
		{
			// Otherwise we just go through our element list and check each
			// element to see if any of them intersect
			typename ElementListType::const_iterator i, e;
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				const Element& p = (*i);

				if( ep.RayElementIntersection_IntersectionOnly( ray, dHowFar, p, bHitFrontFaces, bHitBackFaces) ) {
					return true;
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
				ep.SerializeElement( buffer, *it);
			}			
		}

		// Write whether we have children
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( pChildren ? 1 : 0 );

		if( pChildren ) {
			// If we have children, then write out each of the children
			for( int i=0; i<8; i++ ) {
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
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "octreenode elements" );

			for( unsigned int i=0; i<numElements; i++ ) {
				ep.DeserializeElement( buffer, (*pElements)[i] );
			}
		}

		// Check if we are to have children
		if( !!buffer.getChar() ) {
			pChildren = new OctreeNode*[8];
			GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "octree children" );

			// Check if each child exists
			for( int i=0; i<8; i++ ) {
				if( buffer.getChar() ) {
					pChildren[i] = new OctreeNode();
					GlobalLog()->PrintNew( pChildren[i], __FILE__, __LINE__, "octreenode children" );
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
			for( unsigned char j=0; j<8; j++ ) {
				if( pChildren[j] ) {
					total_nodes += pChildren[j]->DumpStatistics(e,p,maxElements,total_elems) + 1;		
				}
			}
		}

		tree_level--;

		return total_nodes;
	}
};
