//////////////////////////////////////////////////////////////////////
//
//  PRISEOctreeNode.h - Definition of the octree node for PRISE
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 13, 2002
//  Tabs: 4
//  Comments: NOTE:  NEVER include this file directly, it is meant
//  to be included from PRISEOctree.h as part of the templated
//  PRISEOctree class.  Attemping to include this file directly
//  will result in compiler errors!!!!!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

class PRISEOctreeNode
{
protected:
	typedef std::vector<Element>	ElementListType;

	PRISEOctreeNode*		pChildren[8];				// The eight children of this node
	bool					bChildren;					// Does this node have children ?
	unsigned int			polys_all_levels_down;		// Number of polygons for this level and all levels down

	Octree<const Element*>*	pOctree;					// The octree for the actual mesh (level 2 octree)
	ElementListType*		pElements;

	int						which_cpu;					// Which CPU does this node belong to

	bool					bRemoteNode;				// Is data in this octree node remove, ie not on this processor?

	// Functions that people who use us must implement
	bool ElementBoxIntersection( const Element elem, const Point3D& ll, const Point3D& ur ) const;
	void Serialize( IWriteBuffer& buffer, const Element& elem ) const;
	void DeserializeElement( IReadBuffer& buffer, Element& ret );

public:
	PRISEOctreeNode( ) : 
	  bChildren( false ),
	  polys_all_levels_down( 0 ),
	  pOctree( 0 ),
	  pElements( 0 ),
	  which_cpu( -1 ), 
	  bRemoteNode( false )
	{
	}

	virtual ~PRISEOctreeNode( )
	{
		if( bChildren ) {
			for( int i=0; i<8; i++ ) {
				if( pChildren[i] ) {
					GlobalLog()->PrintDelete( pChildren[i], __FILE__, __LINE__ );
					delete pChildren[i];
					pChildren[i] = 0;
				}
			}
		}

		if( pOctree ) {
			pOctree->RemoveRef();
			pOctree = 0;
		}

		if( pElements ) {
			GlobalLog()->PrintDelete( pElements, __FILE__, __LINE__ );
			delete pElements;
			pElements = 0;
		}
	}

	void MyBBFromParent( const Point3D& ll_parent, const Point3D& ur_parent, char which_child, Point3D& my_ll, Point3D& my_ur ) const
	{
		// Figure out our bouding box based on the parent's bounding box and which child
		// we are... 
		Vector3D vBoxCenter = (ur_parent + ll_parent) * 0.5;

		const Scalar&	AvgX = vBoxCenter.x;
		const Scalar&	AvgY = vBoxCenter.y;
		const Scalar&	AvgZ = vBoxCenter.z;

//		static const Scalar box_error = NEARZERO * 10.0;
		static const Scalar box_error = NEARZERO;

		switch( which_child )
		{
		case 99:
			// Entire
			my_ll = ll_parent;
			my_ur = ur_parent;
			break;
		case 0:
			// Sub node 1, same LL as us, UR as our center
			my_ll = Point3D( ll_parent.x-box_error, ll_parent.y-box_error, ll_parent.z-box_error );
			my_ur = Point3D( vBoxCenter.x+box_error, vBoxCenter.y+box_error, vBoxCenter.z+box_error );
			break;
		case 1:
			// Sub node 2, almost the same LL as us, but x is now averaged with max
			// UR is our UR but z and y is averaged with min
			my_ll = Point3D( AvgX-box_error, ll_parent.y-box_error, ll_parent.z-box_error );
			my_ur = Point3D( ur_parent.x+box_error, AvgY+box_error, AvgZ+box_error );
			break;
		case 2:
			// Sub node 3, almost same LL as us, but y is averaged. UR is same for y but x and 
			// z are averaged
			my_ll = Point3D( ll_parent.x-box_error, AvgY-box_error, ll_parent.z-box_error );
			my_ur = Point3D( AvgX+box_error, ur_parent.y+box_error, AvgZ+box_error );
			break;
		case 3:
			// Sub node 4, LL.z is same as our LL but x and y are averaged, UR x and y are our UR but z is averaged
			my_ll = Point3D( AvgX-box_error, AvgY-box_error, ll_parent.z-box_error );
			my_ur = Point3D( ur_parent.x+box_error, ur_parent.y+box_error, AvgZ+box_error );
			break;
		case 4:
			// Sub node 5, LL x and y is our LL, z is averaged, UR z is our UR y and z are averaged
			my_ll = Point3D( ll_parent.x-box_error, ll_parent.y-box_error, AvgZ-box_error );
			my_ur = Point3D( AvgX+box_error, AvgY+box_error, ur_parent.z+box_error );
			break;
		case 5:
			// Sub node 6, LL x and z are averaged, y is our LL, UR, x and z are our UR and y is averaged
			my_ll = Point3D( AvgX-box_error, ll_parent.y-box_error, AvgZ-box_error );
			my_ur = Point3D( ur_parent.x+box_error, AvgY+box_error, ur_parent.z+box_error );
			break;
		case 6:
			// Sub node 7, LL y and z are averaged, x is our LL, UR, y and z are our UR and x is averaged
			my_ll = Point3D( ll_parent.x-box_error, AvgY-box_error, AvgZ-box_error );
			my_ur = Point3D( AvgX+box_error, ur_parent.y+box_error, ur_parent.z+box_error );
			break;
		case 7:
			// Sub node 8, LL is the center and UR is our UR
			my_ll = Point3D( vBoxCenter.x-box_error, vBoxCenter.y-box_error, vBoxCenter.z-box_error );
			my_ur = Point3D( ur_parent.x+box_error, ur_parent.y+box_error, ur_parent.z+box_error );
			break;
		};
	}

	bool AddElements( const std::vector<Element>& elements, const unsigned int maxElements,
					  const Point3D& ll_parent, const Point3D& ur_parent, const char which_child, const unsigned char max_recursion_level,
					  const unsigned int maxElementsLevel2, const unsigned char maxRecursionLevel2)
	{
		// We add the given elements to our section, 
		// First we see how many elements qualify for us, if that number is less than
		// or equal to the minimum defined, we don't create any children, and we simply keep
		// the polygons

		// If children must be created we subdivide evenly into 8 children passing
		// the element list

		static unsigned int tree_level = 0;

		tree_level++;

		Point3D	my_ll;
		Point3D my_ur;
			
		MyBBFromParent( ll_parent, ur_parent, which_child, my_ll, my_ur );
		
		ElementListType elements_list;
		typename ElementListType::const_iterator i, e;
		for( i=elements.begin(), e=elements.end(); i!=e; i++ ) {
			const Element& p = *i;
			if( ElementBoxIntersection( p, my_ll, my_ur ) ) {
				elements_list.push_back( p );
			}
		}

		unsigned elements_list_size = elements_list.size();

		if( elements_list_size < 1 ) {
			tree_level--;
			bChildren = false;
			return false;
		}

		// If the box for this node is only slightly bigger than the epsilon, then there's
		// no point in creating children...
		Vector3D vBoxSize = my_ur-my_ll;

		// If we have reached the maximum recursion level, then stop and don't try to create any more children
		if( vBoxSize.x <= error_delta_box_size || vBoxSize.y <= error_delta_box_size || vBoxSize.z <= error_delta_box_size ||
			tree_level > max_recursion_level || 
			elements_list_size <= maxElements )
		{
			bChildren = false;

			pElements = new ElementListType( elements_list_size );
			GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );
			std::copy( elements_list.begin(), elements_list.end(), pElements->begin() );

			//
			// At this point create the actual triangle octree and toss our triangles to that
			//
			pOctree = new Octree<const Element*>( my_ll, my_ur, maxElementsLevel2 );

			// Create temp list
			std::vector<const Element*>	temp;
			{
				typename ElementListType::iterator i, e;
				for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
					const Element& p = (*i);
					temp.push_back( &p );
				}
			}

			pOctree->AddElements( temp, maxRecursionLevel2 );

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
			for( char x=0; x<8; x++ )
			{
				pChildren[x] = new PRISEOctreeNode( );
				GlobalLog()->PrintNew( pChildren[x], __FILE__, __LINE__, "ChildNode" );
				if( !pChildren[x]->AddElements( elements_list, maxElements, my_ll, my_ur, x, max_recursion_level, maxElementsLevel2, maxRecursionLevel2 ) ) {
					GlobalLog()->PrintDelete( pChildren[x], __FILE__, __LINE__ );
					delete pChildren[x];
					pChildren[x] = 0;
					numRejects++;
				}
			}

			// If all of our children have been rejected, then there's no reason for this node itself
			// to even exist!
			if( numRejects == 8 ) {
				GlobalLog()->Print( eLog_Error, TYPICAL_PRIORITY, "OctreeNode: I have elements but none of my children do!  Should never happen" );
				bChildren = false;
				tree_level--;
				return false;
			} else {
				bChildren = true;
			}
		}

		tree_level--;
		return true;
	}

	void IntersectRayBB( const Point3D& ll_parent, const Point3D& ur_parent, const char which_child,
						 const Ray& ray, BOX_HIT& h ) const
	{
		Point3D	my_ll;
		Point3D my_ur;
			
		MyBBFromParent( ll_parent, ur_parent, which_child, my_ll, my_ur );
		RayBoxIntersection( ray, h, my_ll, my_ur );
	}

	struct CHILDHIT
	{
		Scalar				range;
		PRISEOctreeNode*	pChild;
		char				nodeid;
	};

	static inline bool ChildHitRangeCompare( const CHILDHIT& lhs, const CHILDHIT& rhs )
	{
		return lhs.range < rhs.range;
	}

	mutable std::vector<CHILDHIT>	hits;

	void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces,
					   const Point3D& ll_parent, const Point3D& ur_parent, const char which_child, IMemoryBuffer* traversalBuf, unsigned int nCallStack ) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB.
		// We check all the octant nodes that have an intersection, however
		// we will not check those nodes that we can conclusively say
		// will not hit, or is behind existing nodes

		ri.bHit = false;
		ri.range = INFINITY;

		Point3D	my_ll;
		Point3D my_ur;
			
		MyBBFromParent( ll_parent, ur_parent, which_child, my_ll, my_ur );

		if( bChildren )
		{
			hits.clear();

			for( char i=0; i<8; i++ )
			{
				if( pChildren[i] )
				{
					BOX_HIT	h;
					pChildren[i]->IntersectRayBB( my_ll, my_ur, i, ri.ray, h );

					// This checks not only if a particular octant node 
					// was hit, but also checks to see if the closest distance
					// in that octant node is too far away for us to care!
					if( h.bHit ) {
						CHILDHIT	hit;
						hit.range = h.dRange;
						hit.pChild = pChildren[i];
						hit.nodeid = i;
						hits.push_back( hit );
					}
				}
			}

			std::sort( hits.begin(), hits.end(), &ChildHitRangeCompare );

			typename std::vector<CHILDHIT>::const_iterator m,n;
			for( m=hits.begin(), n=hits.end(); m!=n; m++ ) {
				traversalBuf->setChar( m->nodeid );
				traversalBuf->setDouble( m->range );
			}

			traversalBuf->setChar( 99 );

			for( m=hits.begin(), n=hits.end(); m!=n; m++ )
			{
				RayIntersectionGeometric		myRI;
				myRI.ray = ri.ray;		
				
				traversalBuf->setChar( m->nodeid );		// so we know which node we are working on
				m->pChild->IntersectRay( myRI, bHitFrontFaces, bHitBackFaces, my_ll, my_ur, m->nodeid, traversalBuf, nCallStack*10+m->nodeid+1 );

				if( myRI.bHit ) {
					ri = myRI;
					break;
				}

				if( (IMemoryBuffer*)myRI.custom != 0 ) {
					ri.custom = (void*)(IMemoryBuffer*)myRI.custom;
					break;
				} else {
					traversalBuf->seek( IBuffer::CUR, -1 );	// reset which node we are working on
				}
			}
		}
		else if( pOctree )
		{
			// If we have an octree, then let the octree do that intersection
			pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
		}
		else if( bRemoteNode )
		{
			// This means the data to do this intersection is on another processor, 
			//   so we should send this ray to the scheduler so that someone else can process it
			traversalBuf->setChar( 100 );
			traversalBuf->setUInt( nCallStack );
//			GlobalLog()->PrintEx( eLog_Error, TYPICAL_PRIORITY, "%d", nCallStack );
			ri.custom = (void*)traversalBuf;
		}
	}

	void IntersectRayImcomplete( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces,
					   const Point3D& ll_parent, const Point3D& ur_parent, const char which_child, IMemoryBuffer* traversalBuf, unsigned int nCallStack, IMemoryBuffer* pastTraversal ) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB.
		// We check all the octant nodes that have an intersection, however
		// we will not check those nodes that we can conclusively say
		// will not hit, or is behind existing nodes

		ri.bHit = false;
		ri.range = INFINITY;

		Point3D	my_ll;
		Point3D my_ur;
			
		MyBBFromParent( ll_parent, ur_parent, which_child, my_ll, my_ur );

		hits.clear();

		if( bChildren ) {
			while( 1 ) {
				char child = pastTraversal->getChar();

				if( child == 99 || child == 100 ) {
					break;
				} else {
					CHILDHIT	hit;
					hit.range = pastTraversal->getDouble();;
					hit.pChild = pChildren[child];
					hit.nodeid = child;
					hits.push_back( hit );

					traversalBuf->setChar( child );
					traversalBuf->setDouble( hit.range );
				}
			}

			traversalBuf->setChar( 99 );

			char nodeLastAt = pastTraversal->getChar();
			bool bcheckedlastat = false;

			typename std::vector<CHILDHIT>::const_iterator m,n;
			for( m=hits.begin(), n=hits.end(); m!=n; m++ )
			{
				RayIntersectionGeometric		myRI;
				myRI.ray = ri.ray;		

				// Only check the nodes that we didn't before
				if( !bcheckedlastat ) {
					if( m->nodeid == nodeLastAt ) {
						traversalBuf->setChar( m->nodeid );		// so we know which node we are working on
						m->pChild->IntersectRayImcomplete( myRI, bHitFrontFaces, bHitBackFaces, my_ll, my_ur, m->nodeid, traversalBuf, nCallStack*10+m->nodeid+1, pastTraversal );
	
						bcheckedlastat = true;

						if( myRI.bHit ) {
							ri = myRI;
							break;
						}

						if( (IMemoryBuffer*)myRI.custom != 0 ) {
							ri.custom = (void*)(IMemoryBuffer*)myRI.custom;
							break;
						} else {
							traversalBuf->seek( IBuffer::CUR, -1 );	// reset which node we are working on
						}
					}
				} else  {					
					traversalBuf->setChar( m->nodeid );		// so we know which node we are working on
					m->pChild->IntersectRayImcomplete( myRI, bHitFrontFaces, bHitBackFaces, my_ll, my_ur, m->nodeid, traversalBuf, nCallStack*10+m->nodeid+1, pastTraversal );

					if( myRI.bHit ) {
						ri = myRI;
						break;
					}

					if( (IMemoryBuffer*)myRI.custom != 0 ) {
						ri.custom = (void*)(IMemoryBuffer*)myRI.custom;
						break;
					} else {
						traversalBuf->seek( IBuffer::CUR, -1 );	// reset which node we are working on
					}
				}
			}
		}
		else if( pOctree )
		{
			// If we have an octree, then let the octree do that intersection
			pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
		}
		else if( bRemoteNode )
		{
			// This means the data to do this intersection is on another processor, 
			//   so we should send this ray to the scheduler so that someone else can process it
			traversalBuf->setChar( 100 );
			traversalBuf->setUInt( nCallStack );
//			GlobalLog()->PrintEx( eLog_Error, TYPICAL_PRIORITY, "%d", nCallStack );
			ri.custom = (void*)traversalBuf;
		}
	}

	bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces,
		const Point3D& ll_parent, const Point3D& ur_parent, const char which_child ) const
	{
		// If we are called we'll assume our parent has made sure the ray
		// intersects our BB, so now we check our children's BB, take the 
		// smallest range and call IntersectRay on that child, if that
		// child fails, we try the second smallest and so on, until we 
		// either run out of children or we get an intersection
		if( bChildren )
		{
			Point3D	my_ll;
			Point3D my_ur;
				
			MyBBFromParent( ll_parent, ur_parent, which_child, my_ll, my_ur );

			PRISEOctreeNode*	childrenhit[8];
			char				numchildrenhit=0;
			char				nodeid[8];

			for( char i=0; i<8; i++ ) {
				if( pChildren[i] ) {
					BOX_HIT	h;
					pChildren[i]->IntersectRayBB( my_ll, my_ur, i, ray, h );

					if( h.bHit && h.dRange < dHowFar ) {
						childrenhit[numchildrenhit] = pChildren[i];
						nodeid[numchildrenhit] = i;
						numchildrenhit++;
					}
				}
			}

			int m;
			
			// run through the successful children in order
			for( m=0; m<numchildrenhit; m++ ) {
				if( childrenhit[m]->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces, my_ll, my_ur, nodeid[m] ) ) {
					return true;
				}
			}

		}
		else if( pOctree )
		{
			// If we have an octree, then let the octree do that intersection
			return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
		}
		else if( bRemoteNode )
		{
			// This means the data to do this intersection is on another processor, 
			//   so we should send this ray to the scheduler so that someone else can process it
		}

		return false;
	}

	void Serialize( IWriteBuffer& buffer ) const
	{
		// Write whether we have elements (ie a level 2 octree)
		buffer.ResizeForMore( sizeof(char)+sizeof(int) );
		buffer.setInt( which_cpu );
		buffer.setChar( pOctree ? 1 : 0 );
		
		// If we have elements, then write out the elements
		if( pOctree ) {
			pOctree->Serialize( buffer, (const void*)&(*(pElements->begin())) );
		}

		if( pElements ) {
			buffer.ResizeForMore( sizeof(Element)*pElements->size() + sizeof( unsigned int ) );
			buffer.setUInt( pElements->size() );

			typename ElementListType::const_iterator		it;
			for( it=pElements->begin(); it!=pElements->end(); it++ ) {
				Serialize( buffer, *it );
			}			
		}

		// Write whether we have children
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( bChildren ? 1 : 0 );

		if( bChildren ) {
			// If we have children, then write out each of the children
			for( int i=0; i<8; i++ ) {
				buffer.ResizeForMore( sizeof(char) );
				buffer.setChar( pChildren[i] ? 1 : 0 );

				if( pChildren[i] ) {
					pChildren[i]->Serialize( buffer );
				}
			}
		}
	}

	void SerializeForCPU( IWriteBuffer& buffer, int cpu ) const
	{
		//
		// Almost the same as Serialize, except we only write out
		//   lower levels and the polygon list if this CPU needs that info
		//
		// Write whether we have elements
		buffer.ResizeForMore( sizeof(char)+sizeof(int) );
		buffer.setInt( which_cpu );

		if( cpu == which_cpu || which_cpu==-1 ) {

			buffer.setChar( pElements ? 1 : 0 );

			// If we have elements, then write out the elements
			if( pElements ) {
				buffer.ResizeForMore( sizeof(Element)*pElements->size() + sizeof( unsigned int ) );
				buffer.setUInt( pElements->size() );

				typename ElementListType::const_iterator		it;
				for( it=pElements->begin(); it!=pElements->end(); it++ ) {
					Serialize( buffer, *it );
				}			
			}

			if( pOctree ) {
				pOctree->Serialize( buffer, (const void*)&(*(pElements->begin())) );
			}
		} else if( pElements ) {
			buffer.setChar( 2 );
		} else {
			buffer.setChar( 0 );
		}

		// Write whether we have children
		buffer.ResizeForMore( sizeof(char) );
		buffer.setChar( bChildren ? 1 : 0 );

		if( bChildren ) {
			// If we have children, then write out each of the children
			for( int i=0; i<8; i++ ) {
				buffer.ResizeForMore( sizeof(char) );

				if( pChildren[i] ) {
//					if( (cpu == which_cpu || which_cpu==-1) ) {
						buffer.setChar( 1 );
						pChildren[i]->SerializeForCPU( buffer, cpu );
//					} else {
//						buffer.setChar( 2 );
//					}
				} else {
					buffer.setChar( 0 );
				}
			}
		}
	}

	int CPUFromCallStack( std::deque<char>& call_stack ) const
	{
		if( call_stack.size() == 1 ) {
			return pChildren[call_stack.front()-1]->which_cpu;
		} else {
			PRISEOctreeNode* pChild = pChildren[call_stack.front()-1];
			call_stack.pop_front();
			return pChild->CPUFromCallStack( call_stack );
		}
	}

	void Deserialize( IReadBuffer& buffer )
	{
		// Check whether we have elements
		which_cpu = buffer.getInt();
		char bElements = buffer.getChar();

		if( bElements == 1 ) {
			unsigned int numElements = buffer.getUInt();
			pElements = new ElementListType( numElements );

			for( int i=0; i<numElements; i++ ) {
				DeserializeElement( buffer, (*pElements)[i] );
			}

			// Then there is also an octree here
			pOctree = new Octree<const Element*>( Vector3D(0,0,0), Vector3D(0,0,0), 100 );
			pOctree->Deserialize( buffer, (const void*)&(*(pElements->begin())) );
		} else if( bElements == 2 ) {
			bRemoteNode = true;
		}

		// Check if we are to have children
		bChildren = !!buffer.getChar();

		if( bChildren ) {
			// Check if each child exists
			for( int i=0; i<8; i++ ) {
				char chChildStatus = buffer.getChar();
				if( chChildStatus == 1 ) {
					pChildren[i] = new PRISEOctreeNode();
					pChildren[i]->Deserialize( buffer );
//				} else if( chChildStatus == 2 ) {
					// Says someelse has the octree from this point forward
//					pChildren[i] = new PRISEOctreeNode();
//					pChildren[i]->bRemoteNode = true;
				} else {
					pChildren[i] = 0;
				}
			}
		}
	}

	// This computes the polygon count for all children going down the tree at every level
	unsigned int PrecomputePolyCount( )
	{
		polys_all_levels_down = 0;

		if( pElements ) {
			polys_all_levels_down += pElements->size();
		}

		if( bChildren ) {
			for( int i=0; i<8; i++ ) {
				if( pChildren[i] ) {
					polys_all_levels_down += pChildren[i]->PrecomputePolyCount();
				}
			}
		}

		return polys_all_levels_down;
	}

	// This returns the number of polygons at this level and all levels below this one
	unsigned int GetPolysAllChildren( )
	{
		return polys_all_levels_down;
	}

	void BalancedSegmentForCPUS( const unsigned int num_cpus, unsigned int* pPolysToCPUs )
	{
		// For a balanced segment, what we do is try to maintain some kind of 
		// balance in the segmentation.  

		// The more CPUs we have, the trickier this gets.  

		// don't know how to do this yet ?  Need a nifty heuristic
	}

	void PropagateCPUAssignmentToLeaves( const int cpu )
	{
		if( pElements ) {
			which_cpu = cpu;
		}

		if( bChildren ) {
			for( int i=0; i<8; i++ ) {
				if( pChildren[i] ) {
					pChildren[i]->PropagateCPUAssignmentToLeaves( cpu );
				}
			}
		}
	}

	void SimpleSegmentForCPUS( const unsigned int num_cpus, unsigned int* pPolysToCPUs )
	{
		// Basically assign the nodes in round robin fashion to each of the CPUs
		for( int i=0, j=0; j<8; j++ ) {
			if( i>=num_cpus ) {
				i=0;
			}

			if( pChildren[j] ) {
				// Assign this child to that CPU
				pChildren[j]->PropagateCPUAssignmentToLeaves( i );
				pPolysToCPUs[i] += pChildren[j]->GetPolysAllChildren( );
				i++;
			}
		}
	}


	unsigned int DumpStatistics( const LOG_ENUM e, const unsigned int p, const unsigned int maxElements ) const
	{
		static unsigned int tree_level = 0;
		unsigned int total_nodes = 0;
		tree_level++;
		char buf[1024];
		memset( buf, ' ', tree_level*2 );
		buf[tree_level*2] = 0;

		GlobalLog()->PrintEx(e,p,"%s%d:\t%d\t%Lf %%", buf, tree_level, pElements?pElements->size():0, pElements?Scalar(pElements->size())/Scalar(maxElements)*100 : 0 );

		if( bChildren ) {
			for( unsigned char j=0; j<8; j++ ) {
				if( pChildren[j] ) {
					total_nodes += pChildren[j]->DumpStatistics(e,p,maxElements) + 1;		
				}
			}
		}

		tree_level--;

		return total_nodes;
	}
};
